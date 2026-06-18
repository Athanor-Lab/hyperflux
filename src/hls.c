/*
  Hyperflux -- A lighter download accelerator for Linux and other Unices

  Copyright 2026  Hyperflux contributors

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  In addition, as a special exception, the copyright holders give
  permission to link the code of portions of this program with the
  OpenSSL library under certain conditions as described in each
  individual source file, and distribute linked combinations including
  the two.

  You must obey the GNU General Public License in all respects for all
  of the code used other than OpenSSL. If you modify file(s) with this
  exception, you may extend this exception to your version of the
  file(s), but you are not obligated to do so. If you do not wish to do
  so, delete this exception statement from your version. If you delete
  this exception statement from all source files in the program, then
  also delete it here.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* HLS downloader -- see hls.h.
 *
 * Split in two halves:
 *  - pure: playlist parse, variant select, AES-128-CBC decrypt (libc + OpenSSL
 *    EVP only). Unit-tested standalone like url_glob / extractor.
 *  - download: parallel segment fetch + assemble + optional ffmpeg remux. Uses
 *    http_fetch/conf_t/pthreads; compiled only inside flux (HLS_HAVE_FLUX). */

#ifdef HLS_HAVE_FLUX
/* Pull in conf_t/http_fetch/abuf_t/MAX_STRING before hls.h so the download
 * prototype (which takes conf_t) resolves. The pure unit test omits this. */
#include "config.h"
#include "flux.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>

#include <openssl/evp.h>

#include "hls.h"

/* The extractor owns the canonical relative->absolute URL resolver; reuse it
 * instead of duplicating that logic (per the F2 spec). */
#include "extractor.h"

/* ---- small helpers ---------------------------------------------------- */

static char *
hls_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p)
		memcpy(p, s, n);
	return p;
}

static char *
hls_strndup(const char *s, size_t n)
{
	char *p = malloc(n + 1);

	if (!p)
		return NULL;
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

/* Allocate a "msg" diagnostic into *err (if non-NULL). */
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void
hls_set_err(char **err, const char *fmt, ...)
{
	if (!err)
		return;
	*err = NULL;

	char body[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);
	*err = hls_strdup(body);
}

/* Trim leading/trailing ASCII whitespace (incl. CR) in-place; return new start.*/
static char *
hls_trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
			 s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = '\0';
	return s;
}

/* Skip an optional UTF-8 BOM and leading ASCII whitespace. */
static const char *
hls_skip_bom_ws(const char *s)
{
	if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
	    (unsigned char)s[2] == 0xBF)
		s += 3;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	return s;
}

/* ---- URL kind / m3u8 sniff -------------------------------------------- */

int
hls_is_playlist_url(const char *url)
{
	if (!url)
		return 0;

	/* Examine the path component only: stop at '?' or '#'. */
	size_t pathlen = strcspn(url, "?#");

	static const char *const exts[] = { ".m3u8", ".m3u" };
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		size_t el = strlen(exts[i]);
		if (pathlen >= el &&
		    strncasecmp(url + pathlen - el, exts[i], el) == 0)
			return 1;
	}
	return 0;
}

int
hls_is_m3u8(const char *text)
{
	if (!text)
		return 0;
	const char *p = hls_skip_bom_ws(text);
	return strncmp(p, "#EXTM3U", 7) == 0;
}

/* ---- attribute-list parsing (KEY=VALUE,KEY="VALUE",...) ---------------- */

/* Copy the value of attribute `key` from a comma-separated attribute list `attr`
 * into a malloc'd string (quotes stripped). Returns NULL if not present. */
static char *
attr_get(const char *attr, const char *key)
{
	size_t klen = strlen(key);
	const char *p = attr;

	while (*p) {
		while (*p == ' ' || *p == ',')
			p++;
		const char *eq = strchr(p, '=');
		if (!eq)
			break;
		size_t namelen = (size_t)(eq - p);
		const char *val = eq + 1;
		const char *vend;
		char *out = NULL;

		if (*val == '"') {
			val++;
			vend = strchr(val, '"');
			if (!vend)
				vend = val + strlen(val);
		} else {
			vend = val + strcspn(val, ",");
		}

		if (namelen == klen && strncasecmp(p, key, klen) == 0) {
			out = hls_strndup(val, (size_t)(vend - val));
			return out;	/* may be NULL on OOM; caller checks */
		}

		/* advance past this attribute */
		p = vend;
		if (*p == '"')
			p++;
		while (*p && *p != ',')
			p++;
	}
	return NULL;
}

/* ---- master playlist -------------------------------------------------- */

void
hls_master_free(hls_master_t *m)
{
	if (!m)
		return;
	for (size_t i = 0; i < m->nvariants; i++) {
		free(m->variants[i].url);
		free(m->variants[i].audio_group);
	}
	for (size_t i = 0; i < m->naudios; i++) {
		free(m->audios[i].group_id);
		free(m->audios[i].url);
	}
	free(m);
}

/* Parse a "#EXT-X-MEDIA:" tag into the audio rendition list (AUDIO only). */
static int
master_add_media(hls_master_t *m, const char *attr, const char *base, char **err)
{
	char *type = attr_get(attr, "TYPE");
	if (!type || strcasecmp(type, "AUDIO") != 0) {
		free(type);
		return 0;	/* not audio: ignore subtitles/video here */
	}
	free(type);

	if (m->naudios >= HLS_MAX_VARIANTS)
		return 0;	/* silently cap; not fatal */

	hls_audio_t *a = &m->audios[m->naudios];
	memset(a, 0, sizeof(*a));
	a->group_id = attr_get(attr, "GROUP-ID");

	char *def = attr_get(attr, "DEFAULT");
	a->is_default = def && strcasecmp(def, "YES") == 0;
	free(def);

	char *uri = attr_get(attr, "URI");
	if (uri) {
		a->url = extractor_resolve_url(base, uri);
		free(uri);
		if (!a->url) {
			free(a->group_id);
			hls_set_err(err, "out of memory resolving audio URI");
			return -1;
		}
	}
	m->naudios++;
	return 0;
}

hls_master_t *
hls_parse_master(const char *text, const char *base_url, char **err)
{
	if (err)
		*err = NULL;
	if (!text || !hls_is_m3u8(text)) {
		hls_set_err(err, "not an m3u8 playlist (missing #EXTM3U)");
		return NULL;
	}

	hls_master_t *m = calloc(1, sizeof(*m));
	if (!m) {
		hls_set_err(err, "out of memory");
		return NULL;
	}

	const char *p = text;
	int pending = 0;		/* a STREAM-INF awaiting its URL line */
	long pend_bw = 0;
	int pend_w = 0, pend_h = 0;
	char *pend_audio = NULL;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t llen = nl ? (size_t)(nl - p) : strlen(p);
		char *raw = hls_strndup(p, llen);
		if (!raw) {
			hls_set_err(err, "out of memory");
			free(pend_audio);
			hls_master_free(m);
			return NULL;
		}
		p = nl ? nl + 1 : p + llen;
		char *line = hls_trim(raw);

		if (line[0] == '\0') {
			free(raw);
			continue;
		}

		if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
			const char *attr = line + 18;
			char *bw = attr_get(attr, "BANDWIDTH");
			pend_bw = bw ? strtol(bw, NULL, 10) : 0;
			free(bw);
			pend_w = pend_h = 0;
			char *res = attr_get(attr, "RESOLUTION");
			if (res) {
				sscanf(res, "%dx%d", &pend_w, &pend_h);
				free(res);
			}
			free(pend_audio);
			pend_audio = attr_get(attr, "AUDIO");
			pending = 1;
			free(raw);
			continue;
		}

		if (strncmp(line, "#EXT-X-MEDIA:", 13) == 0) {
			if (master_add_media(m, line + 13, base_url, err) < 0) {
				free(pend_audio);
				free(raw);
				hls_master_free(m);
				return NULL;
			}
			free(raw);
			continue;
		}

		if (line[0] == '#') {	/* other tag: ignore */
			free(raw);
			continue;
		}

		/* A non-tag line is a URL; it belongs to the pending STREAM-INF. */
		if (pending) {
			if (m->nvariants >= HLS_MAX_VARIANTS) {
				free(raw);
				continue;	/* cap */
			}
			hls_variant_t *v = &m->variants[m->nvariants];
			memset(v, 0, sizeof(*v));
			v->url = extractor_resolve_url(base_url, line);
			if (!v->url) {
				hls_set_err(err, "out of memory resolving variant URL");
				free(pend_audio);
				free(raw);
				hls_master_free(m);
				return NULL;
			}
			v->bandwidth = pend_bw;
			v->width = pend_w;
			v->height = pend_h;
			v->audio_group = pend_audio;	/* hand over ownership */
			pend_audio = NULL;
			m->nvariants++;
			pending = 0;
		}
		free(raw);
	}

	free(pend_audio);
	return m;
}

int
hls_select_variant(const hls_master_t *m, const char *quality)
{
	if (!m || m->nvariants == 0)
		return -1;

	int want_best = 1;	/* default */
	int want_worst = 0;
	long target_h = 0;

	if (quality && *quality) {
		if (strcasecmp(quality, "best") == 0) {
			want_best = 1;
		} else if (strcasecmp(quality, "worst") == 0) {
			want_best = 0;
			want_worst = 1;
		} else {
			char *endp = NULL;
			long h = strtol(quality, &endp, 10);
			if (endp && *endp == '\0' && h > 0) {
				want_best = 0;
				target_h = h;
			}
			/* unparsable -> fall back to best */
		}
	}

	/* Rank by (height, then bandwidth) so resolution dominates. */
	int chosen = 0;
	for (size_t i = 1; i < m->nvariants; i++) {
		const hls_variant_t *c = &m->variants[i];
		const hls_variant_t *b = &m->variants[chosen];

		if (want_best || want_worst) {
			int better;
			if (c->height != b->height)
				better = c->height > b->height;
			else
				better = c->bandwidth > b->bandwidth;
			if (want_worst)
				better = !better;
			if (better)
				chosen = (int)i;
		}
	}

	if (want_best || want_worst)
		return chosen;

	/* Target height: pick the largest height <= target; if none, the
	 * smallest height overall. Bandwidth breaks ties. */
	int best_le = -1;		/* index of best height not exceeding target */
	int smallest = 0;
	int any_resolution = 0;		/* any variant carries RESOLUTION? */
	for (size_t i = 0; i < m->nvariants; i++) {
		const hls_variant_t *c = &m->variants[i];
		if (c->height > 0)
			any_resolution = 1;
		if (c->height <= m->variants[smallest].height) {
			if (c->height < m->variants[smallest].height ||
			    c->bandwidth < m->variants[smallest].bandwidth)
				smallest = (int)i;
		}
		if (c->height <= target_h) {
			if (best_le < 0 ||
			    c->height > m->variants[best_le].height ||
			    (c->height == m->variants[best_le].height &&
			     c->bandwidth > m->variants[best_le].bandwidth))
				best_le = (int)i;
		}
	}
	/* BANDWIDTH-only master: with no resolution data a height target would
	 * otherwise select the highest bandwidth (every height is 0 <= target);
	 * honor the cap intent by returning the lowest-bandwidth variant. */
	if (!any_resolution)
		return smallest;
	return best_le >= 0 ? best_le : smallest;
}

/* ---- media playlist --------------------------------------------------- */

void
hls_media_free(hls_media_t *m)
{
	if (!m)
		return;
	for (size_t i = 0; i < m->nsegments; i++) {
		free(m->segments[i].url);
		free(m->segments[i].key_url);
	}
	free(m->segments);
	free(m);
}

/* Current key state while scanning a media playlist top-to-bottom. */
typedef struct {
	hls_enc_t enc;
	char *key_url;		/* resolved, owned by the scanner */
	uint8_t iv[HLS_AES_BLOCK];
	int have_iv;
} hls_keystate_t;

/* Apply an #EXT-X-KEY tag to the running key state. Returns 0, or -1 with *err
 * set on an unsupported method / bad value. */
static int
media_apply_key(hls_keystate_t *ks, const char *attr, const char *base,
		char **err)
{
	char *method = attr_get(attr, "METHOD");
	if (!method) {
		hls_set_err(err, "#EXT-X-KEY without METHOD");
		return -1;
	}

	int rc = 0;
	free(ks->key_url);
	ks->key_url = NULL;
	ks->have_iv = 0;

	if (strcasecmp(method, "NONE") == 0) {
		ks->enc = HLS_ENC_NONE;
	} else if (strcasecmp(method, "AES-128") == 0) {
		ks->enc = HLS_ENC_AES_128;
		char *uri = attr_get(attr, "URI");
		if (!uri) {
			hls_set_err(err, "AES-128 key without URI");
			rc = -1;
			goto out;
		}
		ks->key_url = extractor_resolve_url(base, uri);
		free(uri);
		if (!ks->key_url) {
			hls_set_err(err, "out of memory resolving key URI");
			rc = -1;
			goto out;
		}
		char *iv = attr_get(attr, "IV");
		if (iv) {
			if (hls_parse_iv(iv, ks->iv) != 0) {
				hls_set_err(err, "malformed IV in #EXT-X-KEY");
				free(iv);
				rc = -1;
				goto out;
			}
			ks->have_iv = 1;
			free(iv);
		}
	} else if (strcasecmp(method, "SAMPLE-AES") == 0 ||
		   strcasecmp(method, "SAMPLE-AES-CTR") == 0) {
		hls_set_err(err, "SAMPLE-AES encryption is not supported");
		rc = -1;
	} else {
		hls_set_err(err, "unsupported #EXT-X-KEY METHOD=%s", method);
		rc = -1;
	}

 out:
	free(method);
	return rc;
}

hls_media_t *
hls_parse_media(const char *text, const char *base_url, char **err)
{
	if (err)
		*err = NULL;
	if (!text || !hls_is_m3u8(text)) {
		hls_set_err(err, "not an m3u8 playlist (missing #EXTM3U)");
		return NULL;
	}

	hls_media_t *m = calloc(1, sizeof(*m));
	if (!m) {
		hls_set_err(err, "out of memory");
		return NULL;
	}

	size_t cap = 16;
	m->segments = malloc(cap * sizeof(m->segments[0]));
	if (!m->segments) {
		hls_set_err(err, "out of memory");
		free(m);
		return NULL;
	}

	hls_keystate_t ks;
	memset(&ks, 0, sizeof(ks));
	ks.enc = HLS_ENC_NONE;

	double pend_dur = 0.0;
	int pending = 0;		/* an EXTINF awaiting its URL line */
	const char *p = text;
	long seq_index = 0;		/* index of next segment from sequence base */

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t llen = nl ? (size_t)(nl - p) : strlen(p);
		char *raw = hls_strndup(p, llen);
		if (!raw) {
			hls_set_err(err, "out of memory");
			goto fail;
		}
		p = nl ? nl + 1 : p + llen;
		char *line = hls_trim(raw);

		if (line[0] == '\0') {
			free(raw);
			continue;
		}

		if (strncmp(line, "#EXTINF:", 8) == 0) {
			pend_dur = strtod(line + 8, NULL);
			pending = 1;
			free(raw);
			continue;
		}
		if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
			m->media_sequence = strtol(line + 22, NULL, 10);
			free(raw);
			continue;
		}
		if (strncmp(line, "#EXT-X-KEY:", 11) == 0) {
			if (media_apply_key(&ks, line + 11, base_url, err) < 0) {
				free(raw);
				goto fail;
			}
			if (ks.enc != HLS_ENC_NONE)
				m->enc = ks.enc;
			free(raw);
			continue;
		}
		if (strncmp(line, "#EXT-X-MAP:", 11) == 0) {
			/* fMP4 init segment: parts need it prepended, which we do
			 * not do; fail clearly instead of writing a broken file. */
			hls_set_err(err, "fMP4 / #EXT-X-MAP playlists are not supported");
			free(raw);
			goto fail;
		}
		if (line[0] == '#') {	/* other tag: ignore */
			free(raw);
			continue;
		}

		/* URL line: append a segment (with current key state). */
		if (!pending) {
			free(raw);	/* stray URL with no EXTINF; skip */
			continue;
		}
		if (m->nsegments >= HLS_MAX_SEGMENTS) {
			hls_set_err(err, "too many segments (max %d)",
				    HLS_MAX_SEGMENTS);
			free(raw);
			goto fail;
		}
		if (m->nsegments >= cap) {
			size_t ncap = cap * 2;
			hls_segment_t *ns = realloc(m->segments,
						    ncap * sizeof(ns[0]));
			if (!ns) {
				hls_set_err(err, "out of memory");
				free(raw);
				goto fail;
			}
			m->segments = ns;
			cap = ncap;
		}

		hls_segment_t *seg = &m->segments[m->nsegments];
		memset(seg, 0, sizeof(*seg));
		seg->url = extractor_resolve_url(base_url, line);
		if (!seg->url) {
			hls_set_err(err, "out of memory resolving segment URL");
			free(raw);
			goto fail;
		}
		seg->duration = pend_dur;
		seg->enc = ks.enc;

		if (ks.enc == HLS_ENC_AES_128) {
			seg->key_url = ks.key_url ? hls_strdup(ks.key_url) : NULL;
			if (ks.key_url && !seg->key_url) {
				hls_set_err(err, "out of memory copying key URL");
				free(raw);
				goto fail;
			}
			if (ks.have_iv) {
				memcpy(seg->iv, ks.iv, HLS_AES_BLOCK);
				seg->have_iv = 1;
			} else {
				/* IV defaults to the media sequence number. */
				uint64_t s = (uint64_t)(m->media_sequence +
							seq_index);
				hls_iv_from_sequence(s, seg->iv);
				seg->have_iv = 1;
			}
		}

		m->total_duration += pend_dur;
		m->nsegments++;
		seq_index++;
		pending = 0;
		free(raw);
	}

	free(ks.key_url);
	return m;

 fail:
	free(ks.key_url);
	hls_media_free(m);
	return NULL;
}

/* ---- AES-128 helpers -------------------------------------------------- */

void
hls_iv_from_sequence(uint64_t seq, uint8_t iv[HLS_AES_BLOCK])
{
	memset(iv, 0, HLS_AES_BLOCK);
	for (int i = 0; i < 8; i++)
		iv[HLS_AES_BLOCK - 1 - i] = (uint8_t)(seq >> (8 * i));
}

static int
hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int
hls_parse_iv(const char *hex, uint8_t iv[HLS_AES_BLOCK])
{
	if (!hex)
		return -1;
	if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
		hex += 2;
	if (strlen(hex) != HLS_AES_BLOCK * 2)
		return -1;
	for (int i = 0; i < HLS_AES_BLOCK; i++) {
		int hi = hex_nibble(hex[2 * i]);
		int lo = hex_nibble(hex[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		iv[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

int
hls_aes128_cbc_decrypt(const uint8_t *key, const uint8_t *iv,
		       const uint8_t *in, size_t inlen,
		       uint8_t *out, size_t *outlen)
{
	if (!key || !iv || !out || !outlen)
		return -1;
	if (inlen == 0 || inlen % HLS_AES_BLOCK != 0)
		return -1;	/* CBC ciphertext is whole blocks */

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return -1;

	int ret = -1;
	int len = 0, total = 0;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv) != 1)
		goto out;
	/* HLS uses PKCS#7 padding on the last block; let OpenSSL strip it. */
	if (EVP_DecryptUpdate(ctx, out, &len, in, (int)inlen) != 1)
		goto out;
	total = len;
	if (EVP_DecryptFinal_ex(ctx, out + total, &len) != 1)
		goto out;
	total += len;
	*outlen = (size_t)total;
	ret = 0;

 out:
	EVP_CIPHER_CTX_free(ctx);
	return ret;
}

/* ===================================================================== *
 *  Download orchestration (links against flux: http_fetch, conf_t).     *
 *  Compiled only inside the flux binary.                                *
 * ===================================================================== */

#ifdef HLS_HAVE_FLUX

#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

#include "flux.h"	/* conf_t, http_fetch, abuf_t, MAX_STRING */

#define HLS_DEFAULT_CONCURRENCY	8

/* Per-segment body cap. Real media segments (high-bitrate 1080p, long target
 * durations, single-file VOD .ts) routinely exceed the 16 MiB page default, so
 * fetch segments with a far larger limit while still bounding per-job memory. */
#define HLS_SEGMENT_MAX_BODY	((size_t)512 * 1024 * 1024)

/* Fetch a URL fully into a malloc'd buffer. On success returns the body
 * (NUL-terminated one past the end for text use) and stores its exact byte
 * length in *len; on failure returns NULL. The exact length matters because
 * binary bodies (segments, AES keys) can embed NUL bytes. */
static uint8_t *
hls_http_get(conf_t *conf, const char *url, size_t *len)
{
	abuf_t body[1] = { { NULL, 0 } };
	size_t blen = 0;

	if (http_fetch_len(conf, url, NULL, 0, body, &blen) != 0 || !body->p) {
		abuf_setup(body, ABUF_FREE);
		return NULL;
	}
	*len = blen;
	return (uint8_t *)body->p;	/* caller frees */
}

/* --- per-job state for the thread pool --- */

struct hls_job {
	const hls_segment_t *seg;
	const uint8_t *key;	/* 16 bytes if seg->enc==AES_128, else NULL */
	char path[1024];	/* temp file for this segment */
	int ok;			/* 1 if written successfully */
};

struct hls_pool {
	conf_t *conf;
	struct hls_job *jobs;
	size_t njobs;
	size_t next;		/* next job index to claim */
	int failed;		/* set if any job failed */
	pthread_mutex_t lock;
};

/* Fetch a segment, decrypt if needed, and write it to its temp file. Returns 0
 * on success, -1 on failure. */
static int
hls_fetch_segment(conf_t *conf, struct hls_job *job)
{
	abuf_t body[1] = { { NULL, 0 } };
	size_t inlen = 0;

	/* Segments can exceed the 16 MiB page cap; use the larger media limit. */
	if (http_fetch_max(conf, job->seg->url, NULL, 0, body, &inlen,
			   HLS_SEGMENT_MAX_BODY) != 0 || !body->p) {
		abuf_setup(body, ABUF_FREE);
		fprintf(stderr, _("HLS: failed to fetch segment %s\n"),
			job->seg->url);
		return -1;
	}

	const uint8_t *data = (const uint8_t *)body->p;
	uint8_t *plain = NULL;
	size_t outlen = inlen;

	if (job->seg->enc == HLS_ENC_AES_128) {
		if (!job->key) {
			abuf_setup(body, ABUF_FREE);
			fprintf(stderr, _("HLS: missing key for segment\n"));
			return -1;
		}
		/* OpenSSL may write up to inl + one block during decrypt; size the
		 * buffer accordingly even though CBC plaintext never expands. */
		plain = malloc(inlen + HLS_AES_BLOCK);
		if (!plain) {
			abuf_setup(body, ABUF_FREE);
			return -1;
		}
		if (hls_aes128_cbc_decrypt(job->key, job->seg->iv, data, inlen,
					   plain, &outlen) != 0) {
			free(plain);
			abuf_setup(body, ABUF_FREE);
			fprintf(stderr, _("HLS: decrypt failed for segment %s\n"),
				job->seg->url);
			return -1;
		}
		data = plain;
	}

	int fd = open(job->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		free(plain);
		abuf_setup(body, ABUF_FREE);
		fprintf(stderr, _("HLS: cannot create temp file %s: %s\n"),
			job->path, strerror(errno));
		return -1;
	}
	size_t off = 0;
	int werr = 0;
	while (off < outlen) {
		ssize_t w = write(fd, data + off, outlen - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			werr = 1;
			break;
		}
		off += (size_t)w;
	}
	if (close(fd) != 0)
		werr = 1;
	free(plain);
	abuf_setup(body, ABUF_FREE);
	if (werr) {
		fprintf(stderr, _("HLS: write error for segment %s\n"),
			job->seg->url);
		return -1;
	}
	job->ok = 1;
	return 0;
}

static void *
hls_worker(void *arg)
{
	struct hls_pool *pool = arg;

	for (;;) {
		pthread_mutex_lock(&pool->lock);
		if (pool->failed || pool->next >= pool->njobs) {
			pthread_mutex_unlock(&pool->lock);
			break;
		}
		size_t idx = pool->next++;
		pthread_mutex_unlock(&pool->lock);

		if (hls_fetch_segment(pool->conf, &pool->jobs[idx]) != 0) {
			pthread_mutex_lock(&pool->lock);
			pool->failed = 1;
			pthread_mutex_unlock(&pool->lock);
			break;
		}
	}
	return NULL;
}

/* Run the segment jobs across a bounded thread pool. Returns 0 if all jobs
 * succeeded, -1 otherwise. */
static int
hls_run_pool(conf_t *conf, struct hls_job *jobs, size_t njobs, size_t conc)
{
	if (njobs == 0)
		return 0;
	if (conc < 1)
		conc = 1;
	if (conc > njobs)
		conc = njobs;

	struct hls_pool pool;
	memset(&pool, 0, sizeof(pool));
	pool.conf = conf;
	pool.jobs = jobs;
	pool.njobs = njobs;
	if (pthread_mutex_init(&pool.lock, NULL) != 0)
		return -1;

	pthread_t *threads = calloc(conc, sizeof(*threads));
	if (!threads) {
		pthread_mutex_destroy(&pool.lock);
		return -1;
	}

	size_t started = 0;
	for (size_t i = 0; i < conc; i++) {
		if (pthread_create(&threads[i], NULL, hls_worker, &pool) != 0)
			break;
		started++;
	}
	/* If we couldn't start any thread, run inline on the main thread. */
	if (started == 0)
		hls_worker(&pool);
	for (size_t i = 0; i < started; i++)
		pthread_join(threads[i], NULL);

	free(threads);
	int failed = pool.failed;
	pthread_mutex_destroy(&pool.lock);
	return failed ? -1 : 0;
}

/* Return 1 if `name` is found in PATH, 0 otherwise. */
static int
hls_have_in_path(const char *name)
{
	const char *path = getenv("PATH");
	if (!path || !*path)
		return 0;

	const char *p = path;
	while (*p) {
		const char *colon = strchr(p, ':');
		size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
		char full[1024];
		if (dlen > 0 && dlen + 1 + strlen(name) + 1 <= sizeof(full)) {
			memcpy(full, p, dlen);
			full[dlen] = '/';
			strcpy(full + dlen + 1, name);
			if (access(full, X_OK) == 0)
				return 1;
		}
		if (!colon)
			break;
		p = colon + 1;
	}
	return 0;
}

/* Concatenate the segment temp files (in order) into `out_path`. Returns 0/-1.*/
static int
hls_concat(const struct hls_job *jobs, size_t njobs, const char *out_path)
{
	int ofd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (ofd < 0) {
		fprintf(stderr, _("HLS: cannot create %s: %s\n"), out_path,
			strerror(errno));
		return -1;
	}

	int rc = 0;
	char buf[65536];
	for (size_t i = 0; i < njobs && rc == 0; i++) {
		int ifd = open(jobs[i].path, O_RDONLY);
		if (ifd < 0) {
			fprintf(stderr, _("HLS: cannot read %s: %s\n"),
				jobs[i].path, strerror(errno));
			rc = -1;
			break;
		}
		for (;;) {
			ssize_t n = read(ifd, buf, sizeof(buf));
			if (n < 0) {
				if (errno == EINTR)
					continue;
				rc = -1;
				break;
			}
			if (n == 0)
				break;
			size_t off = 0;
			while (off < (size_t)n) {
				ssize_t w = write(ofd, buf + off, (size_t)n - off);
				if (w < 0) {
					if (errno == EINTR)
						continue;
					rc = -1;
					break;
				}
				off += (size_t)w;
			}
			if (rc != 0)
				break;
		}
		close(ifd);
	}
	if (close(ofd) != 0)
		rc = -1;
	return rc;
}

/* Copy `src` to `dst` byte-for-byte (used when rename() crosses filesystems).
 * Returns 0/-1. */
static int
hls_copy_file(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}

	int rc = 0;
	char buf[65536];
	for (;;) {
		ssize_t n = read(in, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -1;
			break;
		}
		if (n == 0)
			break;
		size_t off = 0;
		while (off < (size_t)n) {
			ssize_t w = write(out, buf + off, (size_t)n - off);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				rc = -1;
				break;
			}
			off += (size_t)w;
		}
		if (rc != 0)
			break;
	}
	close(in);
	if (close(out) != 0)
		rc = -1;
	return rc;
}

/* Move `src` to `dst`: try rename, fall back to copy+unlink across devices. */
static int
hls_move_file(const char *src, const char *dst)
{
	if (rename(src, dst) == 0)
		return 0;
	if (errno != EXDEV)
		return -1;
	if (hls_copy_file(src, dst) != 0)
		return -1;
	unlink(src);
	return 0;
}

/* Run ffmpeg to remux `in_ts` (+ optional `in_audio`) into `out_mp4`. Returns
 * 0 on success, -1 on failure (no shell; argv exec only). */
static int
hls_ffmpeg_remux(const char *in_ts, const char *in_audio, const char *out_mp4)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		/* Child: silence ffmpeg, exec it. */
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		if (in_audio) {
			execlp("ffmpeg", "ffmpeg", "-y", "-i", in_ts, "-i",
			       in_audio, "-c", "copy", "-map", "0:v:0",
			       "-map", "1:a:0", out_mp4, (char *)NULL);
		} else {
			execlp("ffmpeg", "ffmpeg", "-y", "-i", in_ts, "-c",
			       "copy", out_mp4, (char *)NULL);
		}
		_exit(127);	/* exec failed */
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	return -1;
}

/* Download `audio_url` into `out_path` (concatenated, no decryption support for
 * a separate audio rendition's own key beyond what its media playlist needs).
 * Returns 0/-1. The audio rendition is parsed and downloaded like the video. */
static int
hls_download_audio(conf_t *conf, const char *audio_url, const char *tmpdir,
		   size_t conc, char *out_path, size_t out_len)
{
	size_t plen = 0;
	uint8_t *body = hls_http_get(conf, audio_url, &plen);
	if (!body)
		return -1;

	char *err = NULL;
	hls_media_t *media = hls_parse_media((const char *)body, audio_url, &err);
	free(body);
	if (!media) {
		fprintf(stderr, _("HLS: audio playlist parse failed: %s\n"),
			err ? err : "unknown");
		free(err);
		return -1;
	}
	if (media->nsegments == 0) {
		hls_media_free(media);
		return -1;
	}

	/* Fetch the audio key once if encrypted. */
	uint8_t keybuf[HLS_AES_BLOCK];
	int have_key = 0;
	const char *first_key_url = NULL;
	for (size_t i = 0; i < media->nsegments; i++) {
		if (media->segments[i].enc == HLS_ENC_AES_128 &&
		    media->segments[i].key_url) {
			first_key_url = media->segments[i].key_url;
			break;
		}
	}
	if (first_key_url) {
		size_t klen = 0;
		uint8_t *kb = hls_http_get(conf, first_key_url, &klen);
		if (!kb || klen < HLS_AES_BLOCK) {
			free(kb);
			hls_media_free(media);
			return -1;
		}
		memcpy(keybuf, kb, HLS_AES_BLOCK);
		free(kb);
		have_key = 1;
	}

	/* Reject mid-playlist key rotation like the video path; a single key is
	 * assumed for every encrypted audio segment. */
	if (have_key) {
		for (size_t i = 0; i < media->nsegments; i++) {
			const hls_segment_t *s = &media->segments[i];
			if (s->enc == HLS_ENC_AES_128 && s->key_url &&
			    strcmp(s->key_url, first_key_url) != 0) {
				fprintf(stderr,
					_("HLS: mid-playlist key rotation is not supported\n"));
				hls_media_free(media);
				return -1;
			}
		}
	}

	struct hls_job *jobs = calloc(media->nsegments, sizeof(*jobs));
	if (!jobs) {
		hls_media_free(media);
		return -1;
	}
	for (size_t i = 0; i < media->nsegments; i++) {
		jobs[i].seg = &media->segments[i];
		jobs[i].key = (media->segments[i].enc == HLS_ENC_AES_128 &&
			       have_key) ? keybuf : NULL;
		snprintf(jobs[i].path, sizeof(jobs[i].path),
			 "%s/audio-%06zu.part", tmpdir, i);
	}

	int rc = hls_run_pool(conf, jobs, media->nsegments, conc);
	if (rc == 0) {
		snprintf(out_path, out_len, "%s/audio.ts", tmpdir);
		rc = hls_concat(jobs, media->nsegments, out_path);
	}
	for (size_t i = 0; i < media->nsegments; i++)
		if (jobs[i].ok)
			unlink(jobs[i].path);
	free(jobs);
	hls_media_free(media);
	return rc;
}

/* Pick the audio rendition matching the chosen variant's AUDIO group, if any.
 * Returns the resolved audio URL (borrowed from master) or NULL. */
static const char *
hls_pick_audio(const hls_master_t *master, const hls_variant_t *v)
{
	if (!v->audio_group)
		return NULL;
	const char *fallback = NULL;
	for (size_t i = 0; i < master->naudios; i++) {
		const hls_audio_t *a = &master->audios[i];
		if (!a->url || !a->group_id)
			continue;
		if (strcmp(a->group_id, v->audio_group) != 0)
			continue;
		if (a->is_default)
			return a->url;
		if (!fallback)
			fallback = a->url;
	}
	return fallback;
}

/* Derive an output base name from a URL when none was given. */
static void
hls_derive_name(const char *url, const char *ext, char *dst, size_t len)
{
	size_t pathlen = strcspn(url, "?#");
	const char *slash = NULL;
	for (size_t i = 0; i < pathlen; i++)
		if (url[i] == '/')
			slash = url + i;
	const char *base = slash ? slash + 1 : url;
	size_t blen = strcspn(base, "?#");
	/* drop the .m3u8 extension if present */
	if (blen >= 5 && strncasecmp(base + blen - 5, ".m3u8", 5) == 0)
		blen -= 5;
	else if (blen >= 4 && strncasecmp(base + blen - 4, ".m3u", 4) == 0)
		blen -= 4;
	if (blen == 0) {
		snprintf(dst, len, "hls_output%s", ext);
		return;
	}
	if (blen > len - strlen(ext) - 1)
		blen = len - strlen(ext) - 1;
	snprintf(dst, len, "%.*s%s", (int)blen, base, ext);
}

/* Remove the temp directory `dir` (and any leftover files in it). */
static void
hls_rmtree(const char *dir)
{
	DIR *dp = opendir(dir);
	if (dp) {
		struct dirent *de;
		while ((de = readdir(dp)) != NULL) {
			if (strcmp(de->d_name, ".") == 0 ||
			    strcmp(de->d_name, "..") == 0)
				continue;
			char path[2048];
			if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir,
					     de->d_name) < sizeof(path))
				unlink(path);
		}
		closedir(dp);
	}
	rmdir(dir);
}

int
hls_download(conf_t *conf, const char *m3u8_url, const char *out_name,
	     const char *quality, const char *mux_pref)
{
	if (!conf || !m3u8_url)
		return -1;

	/* Fetch and parse the first playlist. */
	size_t plen = 0;
	uint8_t *body = hls_http_get(conf, m3u8_url, &plen);
	if (!body) {
		fprintf(stderr, _("HLS: cannot fetch playlist %s\n"), m3u8_url);
		return -1;
	}
	if (!hls_is_m3u8((const char *)body)) {
		fprintf(stderr, _("HLS: not an m3u8 playlist (missing #EXTM3U)\n"));
		free(body);
		return -1;
	}

	const char *media_url = m3u8_url;
	char *media_url_owned = NULL;
	const char *audio_url = NULL;
	char *err = NULL;
	hls_master_t *master = NULL;

	/* A master playlist has #EXT-X-STREAM-INF; otherwise treat as media. */
	if (strstr((const char *)body, "#EXT-X-STREAM-INF") != NULL) {
		master = hls_parse_master((const char *)body, m3u8_url, &err);
		free(body);
		body = NULL;
		if (!master) {
			fprintf(stderr, _("HLS: master parse failed: %s\n"),
				err ? err : "unknown");
			free(err);
			return -1;
		}
		int vi = hls_select_variant(master, quality);
		if (vi < 0) {
			fprintf(stderr, _("HLS: no playable variant found\n"));
			hls_master_free(master);
			return -1;
		}
		media_url_owned = hls_strdup(master->variants[vi].url);
		if (!media_url_owned) {
			hls_master_free(master);
			return -1;
		}
		media_url = media_url_owned;
		audio_url = hls_pick_audio(master, &master->variants[vi]);

		/* Fetch the chosen media playlist. */
		size_t mlen = 0;
		body = hls_http_get(conf, media_url, &mlen);
		if (!body) {
			fprintf(stderr, _("HLS: cannot fetch media playlist %s\n"),
				media_url);
			free(media_url_owned);
			hls_master_free(master);
			return -1;
		}
	}

	hls_media_t *media = hls_parse_media((const char *)body, media_url, &err);
	free(body);
	body = NULL;
	if (!media) {
		fprintf(stderr, _("HLS: media playlist parse failed: %s\n"),
			err ? err : "unknown");
		free(err);
		free(media_url_owned);
		hls_master_free(master);
		return -1;
	}
	if (media->nsegments == 0) {
		fprintf(stderr, _("HLS: playlist has no segments\n"));
		hls_media_free(media);
		free(media_url_owned);
		hls_master_free(master);
		return -1;
	}

	/* Fetch the AES key once (a media playlist normally uses one key). */
	uint8_t keybuf[HLS_AES_BLOCK];
	int have_key = 0;
	const char *first_key_url = NULL;
	for (size_t i = 0; i < media->nsegments; i++) {
		if (media->segments[i].enc == HLS_ENC_AES_128 &&
		    media->segments[i].key_url) {
			first_key_url = media->segments[i].key_url;
			break;
		}
	}
	if (first_key_url) {
		size_t klen = 0;
		uint8_t *kb = hls_http_get(conf, first_key_url, &klen);
		if (!kb || klen < HLS_AES_BLOCK) {
			free(kb);
			fprintf(stderr, _("HLS: cannot fetch AES key %s\n"),
				first_key_url);
			hls_media_free(media);
			free(media_url_owned);
			hls_master_free(master);
			return -1;
		}
		memcpy(keybuf, kb, HLS_AES_BLOCK);
		free(kb);
		have_key = 1;
	}

	/* Verify every encrypted segment shares the single fetched key URL; a
	 * key rotation mid-playlist is not supported, fail clearly. */
	if (have_key) {
		for (size_t i = 0; i < media->nsegments; i++) {
			const hls_segment_t *s = &media->segments[i];
			if (s->enc == HLS_ENC_AES_128 && s->key_url &&
			    strcmp(s->key_url, first_key_url) != 0) {
				fprintf(stderr,
					_("HLS: mid-playlist key rotation is not supported\n"));
				hls_media_free(media);
				free(media_url_owned);
				hls_master_free(master);
				return -1;
			}
		}
	}

	/* Create a temp working directory. */
	char tmpdir[] = "/tmp/flux-hls-XXXXXX";
	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, _("HLS: cannot create temp dir: %s\n"),
			strerror(errno));
		hls_media_free(media);
		free(media_url_owned);
		hls_master_free(master);
		return -1;
	}

	size_t conc = conf->num_connections ? conf->num_connections
					    : HLS_DEFAULT_CONCURRENCY;
	if (conc > 64)
		conc = 64;

	struct hls_job *jobs = calloc(media->nsegments, sizeof(*jobs));
	if (!jobs) {
		hls_rmtree(tmpdir);
		hls_media_free(media);
		free(media_url_owned);
		hls_master_free(master);
		return -1;
	}
	for (size_t i = 0; i < media->nsegments; i++) {
		jobs[i].seg = &media->segments[i];
		jobs[i].key = (media->segments[i].enc == HLS_ENC_AES_128 &&
			       have_key) ? keybuf : NULL;
		snprintf(jobs[i].path, sizeof(jobs[i].path),
			 "%s/seg-%06zu.part", tmpdir, i);
	}

	if (conf->verbose >= 0)
		printf(_("HLS: downloading %zu segments (%.0fs) with %zu connections\n"),
		       media->nsegments, media->total_duration, conc);

	int rc = hls_run_pool(conf, jobs, media->nsegments, conc);

	/* Decide the container and assemble. Declared up front so the early
	 * `goto cleanup` below does not jump over an initialization. */
	int ret = -1;
	char video_ts[2048];
	char audio_ts[2048];
	int have_audio = 0;
	snprintf(video_ts, sizeof(video_ts), "%s/video.ts", tmpdir);

	int want_mp4;
	int have_ffmpeg = hls_have_in_path("ffmpeg");
	if (mux_pref && strcasecmp(mux_pref, "ts") == 0)
		want_mp4 = 0;
	else if (mux_pref && strcasecmp(mux_pref, "mp4") == 0)
		want_mp4 = 1;
	else
		want_mp4 = have_ffmpeg;	/* auto */

	if (rc == 0)
		rc = hls_concat(jobs, media->nsegments, video_ts);

	/* Remove per-segment temp files as soon as concatenation is done. */
	for (size_t i = 0; i < media->nsegments; i++)
		if (jobs[i].ok)
			unlink(jobs[i].path);
	free(jobs);

	if (rc != 0) {
		fprintf(stderr, _("HLS: download/assembly failed\n"));
		goto cleanup;
	}

	/* Optionally fetch and remux a separate audio rendition. */
	if (audio_url && want_mp4 && have_ffmpeg) {
		if (hls_download_audio(conf, audio_url, tmpdir, conc, audio_ts,
				       sizeof(audio_ts)) == 0)
			have_audio = 1;
		else
			fprintf(stderr,
				_("HLS: separate audio download failed; muxing video only\n"));
	}

	/* Compute the final output name. */
	char outbuf[MAX_STRING];
	if (out_name && *out_name) {
		strlcpy(outbuf, out_name, sizeof(outbuf));
	} else {
		hls_derive_name(m3u8_url, want_mp4 ? ".mp4" : ".ts", outbuf,
				sizeof(outbuf));
	}

	if (want_mp4 && have_ffmpeg) {
		if (hls_ffmpeg_remux(video_ts, have_audio ? audio_ts : NULL,
				     outbuf) == 0) {
			if (conf->verbose >= 0)
				printf(_("HLS: wrote %s\n"), outbuf);
			ret = 0;
		} else {
			fprintf(stderr,
				_("HLS: ffmpeg remux failed; writing raw .ts\n"));
			want_mp4 = 0;	/* fall through to .ts move */
		}
	} else if (want_mp4 && !have_ffmpeg) {
		printf(_("HLS: ffmpeg not found; writing .ts (install ffmpeg for .mp4)\n"));
		want_mp4 = 0;
	}

	/* No mp4 produced: move the concatenated .ts to the final name. */
	if (!want_mp4 && ret != 0) {
		if (hls_move_file(video_ts, outbuf) == 0) {
			if (conf->verbose >= 0)
				printf(_("HLS: wrote %s\n"), outbuf);
			ret = 0;
		} else {
			fprintf(stderr, _("HLS: cannot write %s: %s\n"), outbuf,
				strerror(errno));
		}
	}

	if (have_audio)
		unlink(audio_ts);

 cleanup:
	unlink(video_ts);
	hls_rmtree(tmpdir);
	hls_media_free(media);
	free(media_url_owned);
	hls_master_free(master);
	return ret;
}

#endif				/* HLS_HAVE_FLUX */
