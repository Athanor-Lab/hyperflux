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

/* Page scanner -- see scan.h.
 *
 * Free-standing: libc + POSIX <regex.h> only, plus extractor (URL resolve) and
 * hls (playlist duration) which are themselves free-standing. HTTP is injected
 * via callbacks so the scanner unit-tests over saved HTML with no network. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <regex.h>

#include "scan.h"
#include "extractor.h"
/* HLS duration enrichment links the pure half of hls.c. The non-SSL flux build
 * omits hls.c entirely, so guard the HLS calls behind SCAN_HAVE_HLS (defined by
 * the SSL build and by the standalone unit test). Without it, HLS candidates
 * are still collected and ranked, just without a duration signal. */
#ifdef SCAN_HAVE_HLS
/* Inside the flux build hls.h declares hls_download(conf_t *) behind
 * HLS_HAVE_FLUX, which needs conf_t in scope; pull in the flux headers first.
 * The standalone unit test defines neither macro and uses only the pure
 * parser functions, so it needs no flux headers. */
#ifdef HLS_HAVE_FLUX
#include "config.h"
#include "flux.h"
#endif
#include "hls.h"
#endif

/* Bound a fetched page so a hostile/huge body can't blow up the scanner. */
#define SCAN_MAX_PAGE	(8 * 1024 * 1024)	/* 8 MiB of HTML/JSON */

/* ---- small helpers ---------------------------------------------------- */

static char *
scan_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p)
		memcpy(p, s, n);
	return p;
}

static char *
scan_strndup(const char *s, size_t n)
{
	char *p = malloc(n + 1);

	if (!p)
		return NULL;
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void
scan_set_err(char **err, const char *fmt, ...)
{
	if (!err)
		return;
	*err = NULL;

	char body[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);
	*err = scan_strdup(body);
}

/* ---- URL host extraction --------------------------------------------- */

/* Extract a lowercased host (authority without userinfo or port) from an
 * absolute URL into a malloc'd string, or NULL if `url` has no scheme. */
static char *
url_host(const char *url)
{
	const char *sep = strstr(url, "://");
	if (!sep)
		return NULL;
	const char *auth = sep + 3;
	const char *end = auth + strcspn(auth, "/?#");
	/* Drop any userinfo before '@'. */
	const char *at = memchr(auth, '@', (size_t)(end - auth));
	if (at)
		auth = at + 1;
	/* Drop a :port suffix. */
	const char *colon = memchr(auth, ':', (size_t)(end - auth));
	if (colon)
		end = colon;
	char *h = scan_strndup(auth, (size_t)(end - auth));
	if (!h)
		return NULL;
	for (char *p = h; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	return h;
}

/* Return the first non-empty path segment of `url` as a malloc'd string, or
 * NULL if none exists (e.g. "https://host/" or no scheme). */
static char *
url_first_path_segment(const char *url)
{
	if (!url)
		return NULL;
	const char *sep = strstr(url, "://");
	if (!sep)
		return NULL;
	const char *path = sep + 3;
	path += strcspn(path, "/?#");	/* skip authority */
	if (*path != '/')
		return NULL;
	path++;				/* skip leading slash */
	size_t n = strcspn(path, "/?#");
	if (n == 0)
		return NULL;
	return scan_strndup(path, n);
}

/* ---- ad-domain blocklist --------------------------------------------- */

/* Small built-in list of ad/tracker hosts; a candidate on (or under) any of
 * these is deprioritised. Matched as a suffix on a dot boundary so
 * "ads.doubleclick.net" matches "doubleclick.net". */
static const char *const scan_ad_hosts[] = {
	"doubleclick.net",
	"googlesyndication.com",
	"googleadservices.com",
	"imasdk.googleapis.com",
	"adservice.google.com",
	"adsystem.com",
	"adnxs.com",
	"adsrvr.org",
	"moatads.com",
	"scorecardresearch.com",
	/* popunder / onclick / adult ad networks */
	"popads.net",
	"propellerads.com",
	"popcash.net",
	"adsterra.com",
	"hilltopads.net",
	"hilltopads.com",
	"exoclick.com",
	"clickadu.com",
	"juicyads.com",
	"adcash.com",
	"popmyads.com",
	"mgid.com",
	"onclickads.net",
	"trafficjunky.com",
};

int
scan_is_ad_host(const char *host)
{
	if (!host || !*host)
		return 0;
	size_t hlen = strlen(host);
	for (size_t i = 0; i < sizeof(scan_ad_hosts) / sizeof(scan_ad_hosts[0]);
	     i++) {
		const char *ad = scan_ad_hosts[i];
		/* "adservice.*" style: prefix match on the leading label. */
		if (strcmp(ad, "adservice.google.com") == 0 &&
		    strncasecmp(host, "adservice.", 10) == 0)
			return 1;
		size_t alen = strlen(ad);
		if (hlen < alen)
			continue;
		const char *tail = host + (hlen - alen);
		if (strcasecmp(tail, ad) != 0)
			continue;
		/* Exact host, or a dot precedes the matched suffix. */
		if (tail == host || tail[-1] == '.')
			return 1;
	}
	return 0;
}

/* ---- candidate kind sniff -------------------------------------------- */

/* Classify a media URL by its path extension. Returns 1 for a recognised media
 * URL (kind set), 0 for anything else. */
static int
classify_url(const char *url, scan_kind_t *kind)
{
	size_t pathlen = strcspn(url, "?#");

	static const struct { const char *ext; scan_kind_t kind; } exts[] = {
		{ ".m3u8", SCAN_KIND_HLS },
		{ ".m3u",  SCAN_KIND_HLS },
		{ ".mp4",  SCAN_KIND_FILE },
		{ ".webm", SCAN_KIND_FILE },
		{ ".mkv",  SCAN_KIND_FILE },
		{ ".mov",  SCAN_KIND_FILE },
		{ ".m4v",  SCAN_KIND_FILE },
	};
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		size_t el = strlen(exts[i].ext);
		if (pathlen >= el &&
		    strncasecmp(url + pathlen - el, exts[i].ext, el) == 0) {
			*kind = exts[i].kind;
			return 1;
		}
	}
	return 0;
}

/* ---- collection state ------------------------------------------------- */

struct scan_ctx {
	scan_result_t *result;
	const char *page_url;		/* base for relative resolution */
	extractor_fetch_fn fetch;
	scan_probe_fn probe;
	void *userdata;
	/* Recursion bookkeeping (#scan-recur). The visited set prevents cycles
	 * and self-links; the fetch counter is a hard DoS guard. */
	char *visited[SCAN_MAX_FETCHES];
	size_t nvisited;
	int nfetches;			/* total page fetches so far */
	/* page0 body for the filename-as-param heuristic. Points into caller's
	 * buffer; NOT owned. NULL outside of discover_chain recursion. */
	const char *page0_body;
};

/* Record `url` as visited; returns 1 if already seen, 0 if newly added/unrecordable.
 * Visited set is best-effort dedup: SCAN_MAX_FETCHES is the real termination bound. */
static int
scan_visited(struct scan_ctx *ctx, const char *url)
{
	for (size_t i = 0; i < ctx->nvisited; i++)
		if (strcmp(ctx->visited[i], url) == 0)
			return 1;
	if (ctx->nvisited < SCAN_MAX_FETCHES) {
		char *dup = scan_strdup(url);
		if (dup)
			ctx->visited[ctx->nvisited++] = dup;
	}
	return 0;
}

/* Add one candidate (resolving `raw_ref` against the page URL). `context` is
 * the DOM/source signal. Duplicate URLs bump the existing candidate's count
 * (and upgrade its context toward PLAYER). Returns 0 on success, -1 on a hard
 * error (OOM); a full list or a non-media URL is a silent no-op. */
static int
add_candidate(struct scan_ctx *ctx, const char *raw_ref, scan_ctx_t context)
{
	if (!raw_ref || !*raw_ref)
		return 0;

	char *url = extractor_resolve_url(ctx->page_url, raw_ref);
	if (!url)
		return -1;

	scan_kind_t kind;
	if (!classify_url(url, &kind)) {
		free(url);
		return 0;	/* not a media URL we handle */
	}

	scan_result_t *r = ctx->result;

	/* Deduplicate exact URLs. */
	for (size_t i = 0; i < r->ncands; i++) {
		if (strcmp(r->cands[i].url, url) == 0) {
			r->cands[i].count++;
			/* A stronger context wins (player beats unknown/ad). */
			if (context == SCAN_CTX_PLAYER)
				r->cands[i].context = SCAN_CTX_PLAYER;
			else if (context == SCAN_CTX_AD &&
				 r->cands[i].context == SCAN_CTX_UNKNOWN)
				r->cands[i].context = SCAN_CTX_AD;
			free(url);
			return 0;
		}
	}

	if (r->ncands >= SCAN_MAX_CANDIDATES) {
		free(url);
		return 0;	/* cap: ignore further candidates */
	}

	char *host = url_host(url);	/* may be NULL: best effort */
	scan_candidate_t *c = &r->cands[r->ncands];
	memset(c, 0, sizeof(*c));
	c->url = url;
	c->host = host;
	c->kind = kind;
	c->context = context;
	c->size = -1;
	c->count = 1;
	c->ad_host = host ? scan_is_ad_host(host) : 0;
	if (c->ad_host && c->context == SCAN_CTX_UNKNOWN)
		c->context = SCAN_CTX_AD;
	r->ncands++;
	return 0;
}

/* ---- regex collection passes ------------------------------------------ */

/* Run `ere` over `text` (REG_EXTENDED|REG_ICASE), adding capture group 1 of
 * every non-overlapping match as a candidate with `context`. Returns 0 on
 * success, -1 on OOM (regex-compile failures are skipped, not fatal: a bad
 * built-in pattern should never abort a scan). */
static int
collect_regex(struct scan_ctx *ctx, const char *text, const char *ere,
	      scan_ctx_t context)
{
	regex_t re;
	if (regcomp(&re, ere, REG_EXTENDED | REG_ICASE) != 0)
		return 0;

	int rc = 0;
	const char *p = text;
	regmatch_t m[2];
	while (regexec(&re, p, 2, m, 0) == 0) {
		if (m[1].rm_so >= 0) {
			size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
			char *ref = scan_strndup(p + m[1].rm_so, len);
			if (!ref) {
				rc = -1;
				break;
			}
			int ar = add_candidate(ctx, ref, context);
			free(ref);
			if (ar < 0) {
				rc = -1;
				break;
			}
		}
		/* Advance past this match; guard against a zero-width match. */
		regoff_t adv = m[0].rm_eo > m[0].rm_so ? m[0].rm_eo
						       : m[0].rm_so + 1;
		if (adv <= 0)
			break;
		p += adv;
		if (p > text + strlen(text))
			break;
	}
	regfree(&re);
	return rc;
}

/* Built-in collection patterns. Each captures a URL in group 1. The contexts
 * mark player-like vs generic sources; ad iframes are handled separately. */
struct scan_pattern {
	const char *ere;
	scan_ctx_t context;
};

static const struct scan_pattern scan_patterns[] = {
	/* <video ... src="URL"> and <source ... src="URL"> (player). */
	{ "<source[^>]+src=[\"']([^\"']+)[\"']", SCAN_CTX_PLAYER },
	{ "<video[^>]+src=[\"']([^\"']+)[\"']", SCAN_CTX_PLAYER },
	/* og:video / og:video:url meta. */
	{ "property=[\"']og:video(:url|:secure_url)?[\"'][^>]*content=[\"']([^\"']+)[\"']",
	  SCAN_CTX_PLAYER },
	{ "content=[\"']([^\"']+)[\"'][^>]*property=[\"']og:video",
	  SCAN_CTX_PLAYER },
	/* JSON-LD "contentUrl":"URL". */
	{ "\"contentUrl\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	  SCAN_CTX_PLAYER },
	/* player-config JSON keys: file:"URL", "file":"URL", "src":"URL",
	 * "hls":"URL", "source":"URL". */
	{ "[\"']?file[\"']?[[:space:]]*:[[:space:]]*[\"']([^\"']+)[\"']",
	  SCAN_CTX_PLAYER },
	{ "[\"']hls[\"'][[:space:]]*:[[:space:]]*[\"']([^\"']+)[\"']",
	  SCAN_CTX_PLAYER },
	{ "[\"']src[\"'][[:space:]]*:[[:space:]]*[\"']([^\"']+\\.(m3u8|mp4|webm)[^\"']*)[\"']",
	  SCAN_CTX_PLAYER },
	{ "[\"']source[\"'][[:space:]]*:[[:space:]]*[\"']([^\"']+)[\"']",
	  SCAN_CTX_PLAYER },
	/* Catch-all: any bare media URL in the text (unknown context).
	 * '&' is excluded from the PATH class so &quot; in HTML-attribute JSON
	 * doesn't bleed across into a later .mp4, but the optional query/fragment
	 * allows '&' so signed URLs like ?token=abc&expires=123 are not truncated. */
	{ "(https?:[^\"'[:space:]<>()&]+\\.(m3u8|mp4|webm)([?#][^\"'[:space:]<>()]*)?)",
	  SCAN_CTX_UNKNOWN },
};

/* The "og:video" two-pattern set captures group 2 in the first variant; to keep
 * collect_regex's "group 1" contract simple we special-case that ERE so its
 * captured URL is group 1. Rewrite it here with the URL as group 1. */
static const char *const scan_og_video_ere =
	"property=[\"']og:video[^\"']*[\"'][^>]*content=[\"']([^\"']+)[\"']";

/* Find same-origin <iframe src="URL"> targets in `text`, fetch each, and
 * re-scan its body for candidates (one level deep). Returns 0 or -1. */
static int
collect_iframes(struct scan_ctx *ctx, const char *text, int depth);
static int
collect_gdrive(struct scan_ctx *ctx, const char *text);

/* Run every built-in pattern over `text`. Returns 0 or -1 (OOM). */
static int
collect_patterns(struct scan_ctx *ctx, const char *text)
{
	for (size_t i = 0; i < sizeof(scan_patterns) / sizeof(scan_patterns[0]);
	     i++)
		if (collect_regex(ctx, text, scan_patterns[i].ere,
				  scan_patterns[i].context) < 0)
			return -1;
	if (collect_regex(ctx, text, scan_og_video_ere, SCAN_CTX_PLAYER) < 0)
		return -1;
	if (collect_gdrive(ctx, text) < 0)
		return -1;
	return 0;
}

/* ---- Google Drive recognition ----------------------------------------- */

/* Extract the Google Drive file ID from a GDrive share/view URL.
 * Returns 1 and writes into id_out (NUL-terminated, max id_len) on success.
 * Recognises:
 *   drive.google.com/open?id=<ID>
 *   drive.google.com/file/d/<ID>/...
 *   drive.google.com/uc?...id=<ID>
 *   docs.google.com/.../d/<ID>/...
 * <ID> is [A-Za-z0-9_-]+ (at least 10 chars to avoid false positives). */
static int
gdrive_extract_id(const char *url, char *id_out, size_t id_len)
{
	if (!url || id_len == 0)
		return 0;

	/* file/d/<ID> or .../d/<ID> form (Drive and Docs). */
	const char *fd = strstr(url, "/d/");
	if (fd) {
		const char *start = fd + 3;
		size_t n = strspn(start, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
		if (n >= 10 && n < id_len) {
			memcpy(id_out, start, n);
			id_out[n] = '\0';
			return 1;
		}
	}

	/* ?id=<ID> or &id=<ID> query param form. */
	const char *idp = strstr(url, "id=");
	while (idp) {
		if (idp == url || idp[-1] == '?' || idp[-1] == '&') {
			const char *start = idp + 3;
			size_t n = strspn(start, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
			if (n >= 10 && n < id_len) {
				memcpy(id_out, start, n);
				id_out[n] = '\0';
				return 1;
			}
		}
		idp = strstr(idp + 1, "id=");
	}
	return 0;
}

/* Is this URL a Google Drive share/view link (any recognised form)? */
int
is_gdrive_url(const char *url)
{
	if (!url)
		return 0;
	/* Must be drive.google.com or docs.google.com (after scheme). */
	const char *sep = strstr(url, "://");
	if (!sep)
		return 0;
	const char *host_start = sep + 3;
	/* Accept www. prefix too. */
	if (strncmp(host_start, "www.", 4) == 0)
		host_start += 4;
	return (strncmp(host_start, "drive.google.com/", 17) == 0 ||
		strncmp(host_start, "docs.google.com/", 16) == 0);
}

/* Build the canonical download URL for a GDrive file ID. */
static int
gdrive_download_url(const char *id, char *out, size_t outlen)
{
	int n = snprintf(out, outlen,
		"https://drive.usercontent.google.com/download?id=%s"
		"&export=download&confirm=t", id);
	return (n > 0 && (size_t)n < outlen) ? 1 : 0;
}

/* Normalise any Google Drive URL form to the canonical usercontent download
 * URL.  Returns a malloc'd string on success, NULL if url is not a GDrive URL
 * or the ID cannot be extracted.  The caller must free the returned string.
 * Idempotent: the canonical URL maps to itself. */
char *
gdrive_normalize(const char *url)
{
	if (!url || !is_gdrive_url(url))
		return NULL;
	char id[256];
	if (!gdrive_extract_id(url, id, sizeof(id)))
		return NULL;
	char dl[512];
	if (!gdrive_download_url(id, dl, sizeof(dl)))
		return NULL;
	return scan_strdup(dl);
}

/* Add a direct GDrive candidate from a raw GDrive share URL captured from the
 * page.  Resolves the ID, builds the canonical download URL, and records it
 * as SCAN_KIND_GDRIVE.  `filed` is 1 if the raw URL used the file/d/<ID> form.
 * Returns 0 on success (or benign no-op), -1 on OOM. */
static int
add_gdrive_candidate(struct scan_ctx *ctx, const char *raw_gdrive_url, int filed)
{
	if (!raw_gdrive_url || !*raw_gdrive_url)
		return 0;

	/* Resolve any relative URLs first (unlikely but defensive). */
	char *abs = extractor_resolve_url(ctx->page_url, raw_gdrive_url);
	if (!abs)
		return -1;

	if (!is_gdrive_url(abs)) {
		free(abs);
		return 0;
	}

	char id[256];
	if (!gdrive_extract_id(abs, id, sizeof(id))) {
		free(abs);
		return 0;
	}
	free(abs);

	char dl_url[512];
	if (!gdrive_download_url(id, dl_url, sizeof(dl_url)))
		return 0;

	scan_result_t *r = ctx->result;

	/* Deduplicate by canonical download URL. */
	for (size_t i = 0; i < r->ncands; i++) {
		if (strcmp(r->cands[i].url, dl_url) == 0) {
			r->cands[i].count++;
			return 0;
		}
	}
	if (r->ncands >= SCAN_MAX_CANDIDATES)
		return 0;

	char *url = scan_strdup(dl_url);
	if (!url)
		return -1;
	char *host = url_host(url);
	scan_candidate_t *c = &r->cands[r->ncands];
	memset(c, 0, sizeof(*c));
	c->url          = url;
	c->host         = host;
	c->kind         = SCAN_KIND_GDRIVE;
	c->context      = SCAN_CTX_PLAYER;
	c->size         = -1;
	c->count        = 1;
	c->gdrive_filed = filed;
	r->ncands++;
	return 0;
}

/* Scan `text` for GDrive share/view links and add each as a candidate.
 * Returns 0 on success, -1 on OOM. */
static int
collect_gdrive(struct scan_ctx *ctx, const char *text)
{
	/* Match href="https://drive.google.com/..." (with or without HTML-encoded
	 * &amp; between query params, since WordPress encodes href attributes). */
	static const char *const gdrive_eres[] = {
		"href=[\"'](https?://drive\\.google\\.com/open\\?id=[A-Za-z0-9_-]+[^\"'<>]*)[\"']",
		"href=[\"'](https?://drive\\.google\\.com/file/d/[A-Za-z0-9_-]+[^\"'<>]*)[\"']",
		"href=[\"'](https?://drive\\.google\\.com/uc\\?[^\"'<>]*id=[A-Za-z0-9_-]+[^\"'<>]*)[\"']",
	};

	for (size_t ei = 0; ei < sizeof(gdrive_eres) / sizeof(gdrive_eres[0]); ei++) {
		regex_t re;
		if (regcomp(&re, gdrive_eres[ei], REG_EXTENDED | REG_ICASE) != 0)
			continue;
		regmatch_t m[2];
		const char *p = text;
		while (regexec(&re, p, 2, m, 0) == 0) {
			if (m[1].rm_so >= 0) {
				size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
				char *raw = scan_strndup(p + m[1].rm_so, len);
				if (!raw) { regfree(&re); return -1; }
				/* Un-HTML-encode &amp; -> & so ID extraction works. */
				char clean[1024];
				size_t co = 0;
				for (size_t ri = 0; ri < len && co + 1 < sizeof(clean); ri++) {
					if (strncmp(raw + ri, "&amp;", 5) == 0) {
						clean[co++] = '&';
						ri += 4;
					} else {
						clean[co++] = raw[ri];
					}
				}
				clean[co] = '\0';
				free(raw);
				/* ei==1 is the file/d/<ID> pattern */
				int ar = add_gdrive_candidate(ctx, clean, ei == 1);
				if (ar < 0) { regfree(&re); return -1; }
			}
			regoff_t adv = m[0].rm_eo > m[0].rm_so ? m[0].rm_eo : m[0].rm_so + 1;
			if (adv <= 0) break;
			p += adv;
			if (p > text + strlen(text)) break;
		}
		regfree(&re);
	}
	return 0;
}

/* ---- iframe following ------------------------------------------------- */

static int
collect_iframes(struct scan_ctx *ctx, const char *text, int depth)
{
	if (depth >= SCAN_MAX_IFRAME_DEPTH || !ctx->fetch)
		return 0;

	char *page_host = url_host(ctx->page_url);
	regex_t re;
	if (regcomp(&re, "<iframe[^>]+src=[\"']([^\"']+)[\"']",
		    REG_EXTENDED | REG_ICASE) != 0) {
		free(page_host);
		return 0;
	}

	int rc = 0;
	const char *p = text;
	regmatch_t m[2];
	while (regexec(&re, p, 2, m, 0) == 0) {
		if (m[1].rm_so < 0)
			break;
		size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
		char *ref = scan_strndup(p + m[1].rm_so, len);
		if (!ref) {
			rc = -1;
			break;
		}
		char *iframe_url = extractor_resolve_url(ctx->page_url, ref);
		free(ref);
		if (!iframe_url) {
			rc = -1;
			break;
		}

		/* Follow only same-origin iframes (ad iframes are usually
		 * cross-origin; following them risks pulling ad creatives). */
		char *ihost = url_host(iframe_url);
		int same = page_host && ihost && strcmp(page_host, ihost) == 0;
		free(ihost);
		if (same) {
			char *body = NULL;
			if (ctx->fetch(iframe_url, NULL, 0, &body,
				       ctx->userdata) == 0 && body) {
				if (strlen(body) <= SCAN_MAX_PAGE) {
					/* Re-scan the iframe body against the
					 * iframe's own URL as the new base. */
					const char *saved = ctx->page_url;
					ctx->page_url = iframe_url;
					int pr = collect_patterns(ctx, body);
					int ir = pr == 0 ?
						collect_iframes(ctx, body,
								depth + 1) : 0;
					ctx->page_url = saved;
					if (pr < 0 || ir < 0)
						rc = -1;
				}
				free(body);
			}
		}
		free(iframe_url);
		if (rc < 0)
			break;

		regoff_t adv = m[0].rm_eo > m[0].rm_so ? m[0].rm_eo
						       : m[0].rm_so + 1;
		if (adv <= 0)
			break;
		p += adv;
	}
	regfree(&re);
	free(page_host);
	return rc;
}

/* ---- recursive watch/play/embed discovery (#scan-recur) --------------- */

/* Link-following patterns: group 1 captures the next-page href. POSIX ERE only,
 * no {}/{}, no ' #', exactly one capture group. Most-specific first. */
static const char *const scan_link_eres[] = {
	/* anchors to common player/watch paths (one entry per path token so the
	 * single capture group is the href, not an alternation branch). */
	"<a[^>]+href=[\"']([^\"'<> ]*/watch[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/play[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/player[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/embed[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/stream[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/video[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/v/[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/e/[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/load[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/iframe[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/serve[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/f/[^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/d/[^\"'<> ]*)[\"']",
	/* query-param links carrying a media/player handle. */
	"href=[\"']([^\"'<> ]*[?&]file=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]v=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]id=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]ep=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]url=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]embed=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]hash=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]watch=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]token=[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*[?&]e=[^\"'<> ]+)[\"']",
	/* iframes and embeds (cross-origin allowed here: the point is to reach
	 * the player host; the ad-host blocklist still applies). */
	"<iframe[^>]+src=[\"']([^\"'<> ]+)[\"']",
	"<embed[^>]+src=[\"']([^\"'<> ]+)[\"']",
	/* data-* attributes pointing at the player/source URL. */
	"data-src=[\"']([^\"'<> ]+)[\"']",
	"data-file=[\"']([^\"'<> ]+)[\"']",
	"data-url=[\"']([^\"'<> ]+)[\"']",
	"data-embed=[\"']([^\"'<> ]+)[\"']",
	"data-link=[\"']([^\"'<> ]+)[\"']",
	"data-video=[\"']([^\"'<> ]+)[\"']",
	"data-player=[\"']([^\"'<> ]+)[\"']",
	"data-hash=[\"']([^\"'<> ]+)[\"']",
	/* JSON page pointers to an embed/player/server URL. */
	"\"embedurl\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"embed_url\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"iframe\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"player\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"server\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"serverurl\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"source\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
};

/* Episode-index patterns: group 1 = an episode href. If a single pattern
 * matches >= SCAN_SERIES_MIN distinct hrefs on the landing page, it is a
 * series. Same POSIX-ERE constraints as scan_link_eres. */
static const char *const scan_episode_eres[] = {
	"<a[^>]+href=[\"']([^\"'<> ]*/ep/[0-9][^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/episode/[0-9][^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*/e/[0-9][^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*-episode-[0-9][^\"'<> ]*)[\"']",
	"<a[^>]+href=[\"']([^\"'<> ]*[?&]ep=[0-9]+[^\"'<> ]*)[\"']",
};



/* Media-capture patterns for the chain's final step. group 1 = the media URL,
 * config-safe (REG_EXTENDED-only, no '{', no ' #', lowercase). */
static const char *const scan_chain_media_eres[] = {
	"<source[^>]+src=[\"']([^\"'<> ]+)[\"']",
	"<video[^>]+src=[\"']([^\"'<> ]+)[\"']",
	"\"file\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"hls\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"source\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"\"src\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"",
	"file[[:space:]]*:[[:space:]]*[\"']([^\"'<> ]+)[\"']",
	"(https?://[^\"'<> ]+\\.m3u8[^\"'<> ]*)",
	"(https?://[^\"'<> ]+\\.mp4[^\"'<> ]*)",
	"(https?://[^\"'<> ]+\\.webm[^\"'<> ]*)",
};

/* Capture group 1 of the first match of `ere` in `text` into a malloc'd string
 * at *out (REG_EXTENDED|REG_ICASE). Returns 1 on a captured match, 0 on no
 * match (out left NULL), -1 on OOM. A bad pattern is treated as no match. */
static int
scan_first_capture(const char *text, const char *ere, char **out)
{
	*out = NULL;
	regex_t re;
	if (regcomp(&re, ere, REG_EXTENDED | REG_ICASE) != 0)
		return 0;
	regmatch_t m[2];
	int rc = 0;
	if (regexec(&re, text, 2, m, 0) == 0 && m[1].rm_so >= 0) {
		size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
		char *cap = scan_strndup(text + m[1].rm_so, len);
		if (!cap)
			rc = -1;
		else {
			*out = cap;
			rc = 1;
		}
	}
	regfree(&re);
	return rc;
}

/* True if `ref` is a follow-worthy reference: not empty, not a pure fragment or
 * anchor, not a mailto:/javascript:/tel: scheme. */
static int
scan_followable_ref(const char *ref)
{
	if (!ref || !*ref)
		return 0;
	if (ref[0] == '#')
		return 0;
	if (strncasecmp(ref, "mailto:", 7) == 0 ||
	    strncasecmp(ref, "javascript:", 11) == 0 ||
	    strncasecmp(ref, "tel:", 4) == 0)
		return 0;
	return 1;
}

/* Follow-link entry: resolved URL, raw ref, and capture ERE (all owned). */
struct scan_follow {
	char *abs;	/* resolved absolute URL (owned) */
	char *ref;	/* raw captured ref (owned) */
	char *ere;	/* capture ERE for this link (owned; strdup'd from table or synthesized) */
};

/* Max hrefs collected for series template grouping (fits episode-10+ comfortably). */
#define BSF_MAX 128

/* Forward declarations — helpers defined after detect_series. */
static int detect_series_generic(struct scan_ctx *ctx, const char *body,
				 char **first_ep);
static int build_series_from_hrefs(struct scan_ctx *ctx,
				   char **hrefs, size_t nhrefs,
				   char **first_ep);

/* Synthesize a reusable capturing ERE from a concrete href `ref`. Literal parts
 * are ERE-escaped; [0-9]+ replaces numeric runs; [A-Za-z0-9_-]+ replaces id-ish
 * runs of 6+ alnum chars. Result: href=["'](<generalized>)["']. Exactly one
 * capture group, POSIX ERE, no {}, no ' #'. Returns malloc'd string or NULL. */
static char *
synthesize_link_ere(const char *ref)
{
	if (!ref || !*ref)
		return NULL;

	/* First pass: build the generalized body into a fixed buffer. */
	char body[512];
	size_t bo = 0;
	const char *p = ref;
	while (*p && bo + 4 < sizeof(body)) {
		/* Digit run -> [0-9]+ */
		if (isdigit((unsigned char)*p)) {
			if (bo + 7 >= sizeof(body))
				break;
			memcpy(body + bo, "[0-9]+", 6);
			bo += 6;
			while (isdigit((unsigned char)*p))
				p++;
			continue;
		}
		/* Id-ish run: 6+ consecutive [A-Za-z0-9] chars that are all
		 * alphanumeric (no punctuation), replace with [A-Za-z0-9_-]+. */
		if (isalnum((unsigned char)*p)) {
			const char *q = p;
			while (isalnum((unsigned char)*q))
				q++;
			if ((size_t)(q - p) >= 6) {
				if (bo + 14 >= sizeof(body))
					break;
				memcpy(body + bo, "[A-Za-z0-9_-]+", 14);
				bo += 14;
				p = q;
				continue;
			}
			/* Short alnum literal: fall through to char-by-char. */
		}
		/* ERE metachar escape. */
		char c = *p++;
		if (strchr(".^$*+?()|[]\\", c)) {
			body[bo++] = '\\';
			if (bo >= sizeof(body))
				break;
		}
		body[bo++] = c;
	}
	if (bo >= sizeof(body))
		bo = sizeof(body) - 1;
	body[bo] = '\0';

	/* Wrap: href=["'](<body>)["'] */
	char out[600];
	int n = snprintf(out, sizeof(out), "href=[\"'](%s)[\"']", body);
	if (n <= 0 || (size_t)n >= sizeof(out))
		return NULL;

	/* Verify POSIX ERE compiles cleanly. */
	regex_t re;
	if (regcomp(&re, out, REG_EXTENDED) != 0)
		return NULL;
	regfree(&re);

	return scan_strdup(out);
}

/* Boost/penalty signals for generic link relevance scoring.
 * Positive words lift; negative words (effectively) skip. */
static const char *const scan_boost_tokens[] = {
	"episode", "ep", "watch", "play", "player", "stream", "video", "vid",
	"guarda", "vedi", "season", "stagione", "puntata", "episodio",
	"capitolo", "movie", "film", "serie", "anime", "embed",
	NULL
};
static const char *const scan_penalty_tokens[] = {
	"login", "signin", "signup", "register", "account", "cart",
	"checkout", "category", "categor", "genre", "gener", "tag",
	"search", "cerca", "contact", "about", "privacy", "terms", "faq",
	"donat", "page=", "/page/", "rss", "feed", "sitemap",
	"facebook", "twitter", "x.com", "instagram", "youtube", "t.me",
	"whatsapp",
	/* redirect / affiliate / ad traps */
	"/out", "/go/", "/redirect", "redir", "aff=", "affiliate",
	"sponsor", "promo", "banner", "popup", "popunder", "/ads",
	"adserver", "/click", "utm_", "linkvert", "shorten", "/away",
	NULL
};

/* Return a relevance score for a candidate link. `href` is the raw ref;
 * `ctx_window` is ~80 chars of surrounding HTML (may be NULL). Higher=better;
 * negative means skip. */
static int
score_link(const char *href, const char *ctx_window)
{
	if (!href)
		return -9999;

	/* Scratch buffer: lowercased href + window for token search. */
	char buf[256];
	snprintf(buf, sizeof(buf), "%s %s", href, ctx_window ? ctx_window : "");
	for (char *q = buf; *q; q++)
		*q = (char)tolower((unsigned char)*q);

	/* Hard penalties first. */
	for (size_t i = 0; scan_penalty_tokens[i]; i++)
		if (strstr(buf, scan_penalty_tokens[i]))
			return -9999;

	int score = 0;
	for (size_t i = 0; scan_boost_tokens[i]; i++)
		if (strstr(buf, scan_boost_tokens[i]))
			score += 10;

	/* Digit run in the href path boosts (episode numbers). */
	const char *path = strchr(href, '/');
	if (path) {
		for (const char *q = path; *q; q++)
			if (isdigit((unsigned char)*q)) {
				score += 5;
				break;
			}
		/* Id-ish last segment (6+ alnum) also boosts. */
		const char *last = strrchr(path, '/');
		if (last) {
			last++;
			size_t alen = 0;
			for (const char *q = last; isalnum((unsigned char)*q); q++)
				alen++;
			if (alen >= 6)
				score += 3;
		}
	}
	return score;
}

/* Extract the path portion of an absolute URL (everything from the first '/'
 * after the authority up to '?' or '#'), or "" if none. Result is a pointer
 * into `url` — not malloc'd, valid for the lifetime of `url`. */
static const char *
url_path_ptr(const char *url)
{
	if (!url)
		return "";
	const char *sep = strstr(url, "://");
	if (!sep)
		return "";
	const char *auth = sep + 3;
	const char *path = auth + strcspn(auth, "/?#");
	return (*path == '/') ? path : "";
}

/* Path portion of a ref that may be absolute, scheme-relative, or relative.
 * Returns a pointer into `ref` so a relative template anchors path-to-path. */
static const char *
ref_path_ptr(const char *ref)
{
	if (!ref)
		return "";
	const char *sep = strstr(ref, "://");
	if (sep) {			/* absolute: skip scheme://authority */
		const char *auth = sep + 3;
		const char *path = auth + strcspn(auth, "/?#");
		return (*path == '/') ? path : "";
	}
	if (ref[0] == '/' && ref[1] == '/') {	/* scheme-relative //host/path */
		const char *auth = ref + 2;
		const char *path = auth + strcspn(auth, "/?#");
		return (*path == '/') ? path : "";
	}
	return ref;			/* absolute or relative path: as-is */
}

/* Compute length of the common prefix between two strings (char count). */
static size_t
common_prefix_len(const char *a, const char *b)
{
	size_t n = 0;
	while (*a && *a == *b) { a++; b++; n++; }
	return n;
}

/* Collect same-site <a href> links not already in `out[0..n-1]`, score them,
 * and append up to `max - n` of the highest-scoring non-penalized ones.
 * At `depth > 0` (already inside a followed page), generic candidates must
 * share a path prefix with ctx->page_url to stay within the same content
 * subtree. Returns updated count, or -1 on OOM. */
static int
collect_generic_links(struct scan_ctx *ctx, const char *text,
		      struct scan_follow *out, size_t n, size_t max,
		      int depth)
{
	/* Generic anchor harvest ERE: group 1 = href value. */
	static const char generic_ere[] =
		"<a[^>]+href=[\"']([^\"'<> ]+)[\"']";

	char *page_host = url_host(ctx->page_url);

	/* Scored candidate pool (bounded). */
#define GLINK_MAX 64
	struct { char *abs; char *ref; int score; } pool[GLINK_MAX];
	size_t npool = 0;

	regex_t re;
	if (regcomp(&re, generic_ere, REG_EXTENDED | REG_ICASE) != 0) {
		free(page_host);
		return (int)n;
	}

	const char *p = text;
	regmatch_t m[2];
	while (npool < GLINK_MAX && regexec(&re, p, 2, m, 0) == 0) {
		if (m[1].rm_so < 0)
			break;

		/* Extract surrounding window (~80 chars before the match start). */
		ptrdiff_t wstart = m[0].rm_so > 80 ? m[0].rm_so - 80 : 0;
		size_t wlen = (size_t)(m[0].rm_eo - wstart);
		if (wlen > 160)
			wlen = 160;
		char window[161];
		memcpy(window, p + wstart, wlen);
		window[wlen] = '\0';

		size_t rlen = (size_t)(m[1].rm_eo - m[1].rm_so);
		char *ref = scan_strndup(p + m[1].rm_so, rlen);
		if (!ref) {
			regfree(&re);
			free(page_host);
			for (size_t i = 0; i < npool; i++) {
				free(pool[i].abs);
				free(pool[i].ref);
			}
			return -1;
		}

		regoff_t adv = m[0].rm_eo > m[0].rm_so ? m[0].rm_eo
							 : m[0].rm_so + 1;
		p += adv;

		if (!scan_followable_ref(ref)) {
			free(ref);
			continue;
		}

		int sc = score_link(ref, window);
		if (sc < 0) {
			free(ref);
			continue;
		}

		char *abs = extractor_resolve_url(ctx->page_url, ref);
		if (!abs) {
			free(ref);
			continue;	/* resolution failure: tolerate */
		}

		/* Same-site only. */
		char *h = url_host(abs);
		int same = (h && page_host && strcmp(h, page_host) == 0);
		int is_ad = (h && scan_is_ad_host(h));
		free(h);
		if (!same || is_ad) {
			free(abs);
			free(ref);
			continue;
		}

		/* At depth>0, generic links must share a path prefix with the
		 * current page to stay within the same content subtree. */
		if (depth > 0) {
			const char *my_path = url_path_ptr(ctx->page_url);
			const char *cand_path = url_path_ptr(abs);
			/* Require at least one shared path segment (the leading
			 * slash counts; we need the prefix up to the next '/'). */
			const char *my_seg_end = strchr(my_path + 1, '/');
			size_t my_seg_len = my_seg_end
				? (size_t)(my_seg_end - my_path)
				: strlen(my_path);
			size_t cp_len = common_prefix_len(my_path, cand_path);
			if (cp_len < my_seg_len) {
				free(abs);
				free(ref);
				continue;
			}
		}

		/* Skip if already in out[] or already queued in pool. */
		int dup = 0;
		for (size_t i = 0; i < n && !dup; i++)
			if (strcmp(out[i].abs, abs) == 0)
				dup = 1;
		for (size_t i = 0; i < npool && !dup; i++)
			if (strcmp(pool[i].abs, abs) == 0)
				dup = 1;
		/* Also skip the current page itself. */
		if (!dup && strcmp(abs, ctx->page_url) == 0)
			dup = 1;
		if (dup) {
			free(abs);
			free(ref);
			continue;
		}

		pool[npool].abs   = abs;
		pool[npool].ref   = ref;
		pool[npool].score = sc;
		npool++;
	}
	regfree(&re);
	free(page_host);

	/* Sort pool descending by score (insertion sort; small pool). */
	for (size_t i = 1; i < npool; i++) {
		typeof(pool[0]) key = pool[i];
		size_t j = i;
		while (j > 0 && pool[j - 1].score < key.score) {
			pool[j] = pool[j - 1];
			j--;
		}
		pool[j] = key;
	}

	/* Emit top candidates up to remaining capacity; synthesize EREs. */
	for (size_t i = 0; i < npool && n < max; i++) {
		char *synth = synthesize_link_ere(pool[i].ref);
		if (!synth) {
			/* fallback: generic href ERE */
			synth = scan_strdup(generic_ere);
		}
		if (!synth) {
			/* OOM: free pool tail and bail. */
			for (size_t k = i; k < npool; k++) {
				free(pool[k].abs);
				free(pool[k].ref);
			}
			/* Entries 0..i-1 already consumed (abs/ref transferred). */
			return -1;
		}
		out[n].abs = pool[i].abs;
		out[n].ref = pool[i].ref;
		out[n].ere = synth;
		n++;
		pool[i].abs = NULL;
		pool[i].ref = NULL;
	}
	/* Free any pool entries that didn't fit. */
	for (size_t i = 0; i < npool; i++) {
		free(pool[i].abs);
		free(pool[i].ref);
	}
	return (int)n;
#undef GLINK_MAX
}

/* Collect up to SCAN_MAX_FOLLOW unique, followable "next page" links from
 * `text`. Each output entry holds the resolved absolute URL, the raw ref (for a
 * provenance comment), and the ERE that captured it. `depth` is the hop count
 * at which these links will be followed (0 = landing page). Caller frees
 * abs/ref/ere of each filled slot. Returns the number filled, or -1 on OOM. */
static int
collect_follow_links(struct scan_ctx *ctx, const char *text,
		     struct scan_follow *out, size_t max, int depth)
{
	size_t n = 0;
	for (size_t pi = 0;
	     pi < sizeof(scan_link_eres) / sizeof(scan_link_eres[0]) && n < max;
	     pi++) {
		const char *ere = scan_link_eres[pi];
		regex_t re;
		if (regcomp(&re, ere, REG_EXTENDED | REG_ICASE) != 0)
			continue;
		const char *p = text;
		regmatch_t m[2];
		while (n < max && regexec(&re, p, 2, m, 0) == 0) {
			if (m[1].rm_so < 0)
				break;
			size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
			char *ref = scan_strndup(p + m[1].rm_so, len);
			if (!ref) {
				regfree(&re);
				return -1;
			}
			if (scan_followable_ref(ref)) {
				char *abs = extractor_resolve_url(ctx->page_url,
								  ref);
				if (!abs) {
					free(ref);
					regfree(&re);
					return -1;
				}
				/* Skip ad hosts and already-followed/queued URLs. */
				char *h = url_host(abs);
				int bad = (h && scan_is_ad_host(h));
				free(h);
				for (size_t i = 0; i < ctx->nvisited && !bad; i++)
					if (strcmp(ctx->visited[i], abs) == 0)
						bad = 1;
				for (size_t i = 0; i < n && !bad; i++)
					if (strcmp(out[i].abs, abs) == 0)
						bad = 1;
				if (bad) {
					free(abs);
					free(ref);
				} else {
					char *ere_owned = scan_strdup(ere);
					if (!ere_owned) {
						free(abs);
						free(ref);
						regfree(&re);
						return -1;
					}
					out[n].abs = abs;
					out[n].ref = ref;
					out[n].ere = ere_owned;
					n++;
				}
			} else {
				free(ref);
			}
			regoff_t adv = m[0].rm_eo > m[0].rm_so ? m[0].rm_eo
							       : m[0].rm_so + 1;
			if (adv <= 0)
				break;
			p += adv;
		}
		regfree(&re);
	}
	/* Fill remaining slots with generic relevance-ranked same-site links. */
	if ((size_t)n < max) {
		int gn = collect_generic_links(ctx, text, out, (size_t)n, max,
					       depth);
		if (gn < 0)
			return -1;
		n = gn;
	}
	return (int)n;
}

static void
free_follow_links(struct scan_follow *links, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free(links[i].abs);
		free(links[i].ref);
		free(links[i].ere);
	}
}

/* Record a media candidate discovered at the end of a chain. `steps`/`nsteps`
 * are the accumulated capture steps (link steps + the final media step). The
 * candidate's chain is a deep copy of those steps. Returns 0 on success (or a
 * benign no-op), -1 on OOM. Deduplicates by URL like add_candidate, keeping the
 * first-seen (shortest) chain. */
static int
add_chain_candidate(struct scan_ctx *ctx, const char *media_url,
		    scan_kind_t kind, scan_ctx_t context,
		    const scan_step_t *steps, size_t nsteps, int depth)
{
	scan_result_t *r = ctx->result;

	for (size_t i = 0; i < r->ncands; i++) {
		if (strcmp(r->cands[i].url, media_url) == 0) {
			r->cands[i].count++;
			if (context == SCAN_CTX_PLAYER)
				r->cands[i].context = SCAN_CTX_PLAYER;
			return 0;
		}
	}
	if (r->ncands >= SCAN_MAX_CANDIDATES)
		return 0;
	if (nsteps == 0 || nsteps > SCAN_MAX_CHAIN)
		return 0;	/* defensive: a chain must have at least one step */

	char *url = scan_strdup(media_url);
	if (!url)
		return -1;
	char *host = url_host(url);
	scan_candidate_t *c = &r->cands[r->ncands];
	memset(c, 0, sizeof(*c));
	c->url = url;
	c->host = host;
	c->kind = kind;
	c->context = context;
	c->size = -1;
	c->count = 1;
	c->ad_host = host ? scan_is_ad_host(host) : 0;
	c->depth = depth;
	for (size_t i = 0; i < nsteps; i++) {
		c->chain[i].ere = scan_strdup(steps[i].ere);
		c->chain[i].sample = steps[i].sample ?
			scan_strdup(steps[i].sample) : NULL;
		if (!c->chain[i].ere ||
		    (steps[i].sample && !c->chain[i].sample)) {
			/* OOM mid-copy: unwind this partial chain and bail. */
			for (size_t j = 0; j <= i; j++) {
				free(c->chain[j].ere);
				free(c->chain[j].sample);
			}
			free(url);
			free(host);
			return -1;
		}
	}
	c->nchain = nsteps;
	r->ncands++;
	return 0;
}

/* Check whether the media URL's basename appears on page0 as <token>=<basename>
 * where <token> is [A-Za-z_][A-Za-z0-9_]*. If so, set *out_token and
 * *out_base (both malloc'd). Returns 1 on match, 0 on no match, -1 on OOM.
 * Only fires when the full media URL itself is NOT present on page0 (that's the
 * existing depth-0 direct case, handled separately). See #scan-basename-param. */
static int
check_basename_param(const char *page0, const char *media_url,
		     char **out_token, char **out_base)
{
	if (!page0 || !media_url)
		return 0;

	/* Extract basename: everything after the last '/'. */
	const char *slash = strrchr(media_url, '/');
	if (!slash || slash[1] == '\0')
		return 0;
	const char *basename = slash + 1;
	size_t blen = strlen(basename);
	if (blen == 0)
		return 0;

	/* Reject if full URL appears literally in page0 (direct case). */
	if (strstr(page0, media_url) != NULL)
		return 0;

	/* Search page0 for <token>=<basename> where token=[A-Za-z_][A-Za-z0-9_]* */
	const char *p = page0;
	while (*p) {
		/* Find the basename in the remaining page text. */
		const char *hit = strstr(p, basename);
		if (!hit)
			break;

		/* Walk back past '=' to find the token. */
		if (hit == page0 || hit[-1] != '=') {
			p = hit + 1;
			continue;
		}
		/* Find start of token (the identifier before '='). */
		const char *eq = hit - 1;
		const char *tok_end = eq;	/* points at '=' */
		const char *tok_start = tok_end - 1;
		while (tok_start > page0 &&
		       (isalnum((unsigned char)tok_start[-1]) ||
			tok_start[-1] == '_'))
			tok_start--;
		size_t tok_len = (size_t)(tok_end - tok_start);
		if (tok_len == 0 || (!isalpha((unsigned char)tok_start[0]) &&
		    tok_start[0] != '_')) {
			p = hit + 1;
			continue;
		}

		/* Verify char after basename is a natural delimiter (not alnum/./%). */
		const char *after = hit + blen;
		if (*after != '\0' && *after != '"' && *after != '\'' &&
		    *after != '&' && *after != '<' && *after != '>' &&
		    *after != ' ' && *after != '\t' && *after != '\n' &&
		    *after != ')') {
			p = hit + 1;
			continue;
		}

		/* Found a valid <token>=<basename> match. Build media_base. */
		/* media_base = media_url up to and including the last '/'. */
		size_t base_len = (size_t)(slash - media_url) + 1; /* includes '/' */
		char *token = malloc(tok_len + 1);
		char *base  = malloc(base_len + 1);
		if (!token || !base) {
			free(token);
			free(base);
			return -1;
		}
		memcpy(token, tok_start, tok_len);
		token[tok_len] = '\0';
		memcpy(base, media_url, base_len);
		base[base_len] = '\0';

		*out_token = token;
		*out_base  = base;
		return 1;
	}
	return 0;
}

/* Recursively follow watch/play/embed links from `body` (the page at
 * ctx->page_url) looking for media. `steps`/`nsteps` is the chain accumulated so
 * far (link steps). `depth` is the number of hops already taken to reach this
 * page. Returns 0 on success (media found or not), -1 on a hard error. */
static int
discover_chain(struct scan_ctx *ctx, const char *body, scan_step_t *steps,
	       size_t nsteps, int depth, int max_depth)
{
	/* 1) Media on this page? Record a candidate with the media step appended. */
	for (size_t mi = 0;
	     mi < sizeof(scan_chain_media_eres) / sizeof(scan_chain_media_eres[0]);
	     mi++) {
		char *cap = NULL;
		int r = scan_first_capture(body, scan_chain_media_eres[mi], &cap);
		if (r < 0)
			return -1;
		if (r == 0)
			continue;
		char *media_url = extractor_resolve_url(ctx->page_url, cap);
		if (!media_url) {
			free(cap);
			return -1;
		}
		scan_kind_t kind;
		if (classify_url(media_url, &kind) && nsteps < SCAN_MAX_CHAIN) {
			steps[nsteps].ere = (char *)scan_chain_media_eres[mi];
			steps[nsteps].sample = media_url;
			int ar = add_chain_candidate(ctx, media_url, kind,
						     SCAN_CTX_PLAYER, steps,
						     nsteps + 1, depth);
			/* Filename-as-param heuristic: if page0 body contains
			 * <token>=<basename> pointing at this media, record it so
			 * emit can produce a 1-hop template instead of the chain. */
			if (ar == 0 && depth > 0 && ctx->page0_body) {
				scan_result_t *res = ctx->result;
				scan_candidate_t *cand = NULL;
				for (size_t ci = 0; ci < res->ncands; ci++) {
					if (strcmp(res->cands[ci].url,
						   media_url) == 0) {
						cand = &res->cands[ci];
						break;
					}
				}
				if (cand && !cand->param_token) {
					char *tok = NULL, *base = NULL;
					int hr = check_basename_param(
						ctx->page0_body, media_url,
						&tok, &base);
					if (hr == 1) {
						cand->param_token = tok;
						cand->media_base  = base;
					} else if (hr < 0) {
						ar = -1; /* OOM */
					}
				}
			}
			free(media_url);
			free(cap);
			return ar;	/* first media hit on a page is enough */
		}
		free(media_url);
		free(cap);
	}

	/* 2) No media here: follow links one level deeper if budget remains. */
	if (depth >= max_depth || nsteps >= SCAN_MAX_CHAIN - 1)
		return 0;

	struct scan_follow links[SCAN_MAX_FOLLOW];
	memset(links, 0, sizeof(links));
	int nl = collect_follow_links(ctx, body, links, SCAN_MAX_FOLLOW, depth);
	if (nl < 0)
		return -1;

	int rc = 0;
	for (int i = 0; i < nl && rc == 0; i++) {
		if (ctx->nfetches >= SCAN_MAX_FETCHES)
			break;
		if (scan_visited(ctx, links[i].abs))
			continue;	/* cycle/self-link guard */
		char *child = NULL;
		ctx->nfetches++;
		if (ctx->fetch(links[i].abs, NULL, 0, &child, ctx->userdata) != 0
		    || !child)
			continue;	/* dead link: tolerate and move on */
		if (strlen(child) > SCAN_MAX_PAGE) {
			free(child);
			continue;
		}
		/* Push this link's capture step and recurse against the child. */
		steps[nsteps].ere = links[i].ere;
		steps[nsteps].sample = links[i].ref;
		const char *saved = ctx->page_url;
		ctx->page_url = links[i].abs;
		rc = discover_chain(ctx, child, steps, nsteps + 1, depth + 1,
				    max_depth);
		ctx->page_url = saved;
		free(child);
	}
	free_follow_links(links, (size_t)nl);
	return rc;
}

/* Detect a series index on the landing `body`. If a single episode pattern
 * matches >= SCAN_SERIES_MIN distinct hrefs, set result->is_series and fill
 * list_ere/series_path, and return the FIRST episode href (malloc'd) in
 * *first_ep for chain discovery. Returns 0 (sets *first_ep on a series), -1 on
 * OOM. *first_ep is NULL when not a series. */
static int
detect_series(struct scan_ctx *ctx, const char *body, char **first_ep)
{
	*first_ep = NULL;

	/* Try each hardcoded episode pattern.  Collect ALL matching hrefs (cap
	 * BSF_MAX so episode-10+ is not dropped) and pass them to the shared
	 * anchoring helper so the dominant series wins, not the broadest match. */
	for (size_t pi = 0;
	     pi < sizeof(scan_episode_eres) / sizeof(scan_episode_eres[0]);
	     pi++) {
		regex_t re;
		if (regcomp(&re, scan_episode_eres[pi],
			    REG_EXTENDED | REG_ICASE) != 0)
			continue;

		char *hrefs[BSF_MAX];
		size_t nhrefs = 0;
		const char *p = body;
		regmatch_t m[2];
		while (nhrefs < BSF_MAX && regexec(&re, p, 2, m, 0) == 0) {
			if (m[1].rm_so < 0)
				break;
			size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
			char *ref = scan_strndup(p + m[1].rm_so, len);
			if (!ref) {
				for (size_t i = 0; i < nhrefs; i++)
					free(hrefs[i]);
				regfree(&re);
				return -1;
			}
			int dup = 0;
			for (size_t i = 0; i < nhrefs; i++)
				if (strcmp(hrefs[i], ref) == 0) {
					dup = 1;
					break;
				}
			if (!dup)
				hrefs[nhrefs++] = ref;
			else
				free(ref);
			regoff_t adv = m[0].rm_eo > m[0].rm_so ? m[0].rm_eo
							       : m[0].rm_so + 1;
			if (adv <= 0)
				break;
			p += adv;
		}
		regfree(&re);

		if (nhrefs < SCAN_SERIES_MIN) {
			for (size_t i = 0; i < nhrefs; i++)
				free(hrefs[i]);
			continue;
		}

		/* Ownership transferred; build_series_from_hrefs frees hrefs[]. */
		int rc = build_series_from_hrefs(ctx, hrefs, nhrefs, first_ep);
		if (rc != 0 || ctx->result->is_series)
			return rc;
		/* Helper freed hrefs[] but found no dominant group; try next pattern. */
	}

	if (ctx->result->is_series)
		return 0;

	/* Fallback: structural template-based detection for non-standard URLs. */
	return detect_series_generic(ctx, body, first_ep);
}

/* Shared helper: given an array of `nhrefs` distinct hrefs (all owned, freed
 * before return), select the dominant series group by template relatedness to
 * ctx->page_url, build an anchored list_ere, and return it + the first episode
 * href.  Sets r->is_series / r->list_ere / r->series_path on success.
 * Returns 0 (with *first_ep set) if a series was found, 0 (NULL) if not, -1
 * on OOM. All hrefs[] entries and templates[] are freed before return. */
static int
build_series_from_hrefs(struct scan_ctx *ctx,
			char **hrefs, size_t nhrefs,
			char **first_ep)
{
	*first_ep = NULL;
	scan_result_t *r = ctx->result;

	if (nhrefs < SCAN_SERIES_MIN) {
		for (size_t i = 0; i < nhrefs; i++)
			free(hrefs[i]);
		return 0;
	}

	/* Build templates: replace maximal [0-9]+ runs with '#'. */
	char *templates[BSF_MAX];
	for (size_t i = 0; i < nhrefs; i++) {
		char tmp[512];
		size_t to = 0;
		for (const char *q = hrefs[i]; *q && to + 2 < sizeof(tmp); ) {
			if (isdigit((unsigned char)*q)) {
				tmp[to++] = '#';
				while (isdigit((unsigned char)*q))
					q++;
			} else {
				tmp[to++] = *q++;
			}
		}
		tmp[to] = '\0';
		templates[i] = scan_strdup(tmp);
		if (!templates[i]) {
			for (size_t k = 0; k < i; k++)
				free(templates[k]);
			for (size_t k = 0; k < nhrefs; k++)
				free(hrefs[k]);
			return -1;
		}
	}

	/* Select the dominant template: prefer the one most related to the
	 * landing URL so a multi-series page picks the right one. Tiers:
	 * 2 = generalized ERE matches landing URL (user is on an episode);
	 * 1 = longest common path prefix with landing path;
	 * 0 = no relation (fall back to member count). See #scan-series-anchor. */
	const char *landing_path = url_path_ptr(ctx->page_url);

	char *best_tmpl = NULL;
	size_t best_cnt = 0;
	int    best_rel  = -1;
	size_t best_pfx  = 0;

	for (size_t i = 0; i < nhrefs; i++) {
		/* Skip duplicate template strings (score each unique template once). */
		int seen = 0;
		for (size_t k = 0; k < i && !seen; k++)
			if (strcmp(templates[k], templates[i]) == 0)
				seen = 1;
		if (seen)
			continue;

		size_t cnt = 0;
		for (size_t j = 0; j < nhrefs; j++)
			if (strcmp(templates[i], templates[j]) == 0)
				cnt++;
		if (cnt < SCAN_SERIES_MIN)
			continue;

		/* Build candidate ERE for tier-2 test (landing matches an episode). */
		char e_body[512];
		size_t eb = 0;
		for (const char *q = templates[i]; *q && eb + 10 < sizeof(e_body); ) {
			if (*q == '#') {
				memcpy(e_body + eb, "[0-9]+", 6);
				eb += 6;
				q++;
			} else {
				char c = *q++;
				if (strchr(".^$*+?()|[]\\", c))
					e_body[eb++] = '\\';
				e_body[eb++] = c;
			}
		}
		e_body[eb] = '\0';
		char cand_ere[600];
		snprintf(cand_ere, sizeof(cand_ere), "href=[\"'](%s)[\"']", e_body);

		int rel = 0;
		size_t pfx = 0;

		regex_t cre;
		if (regcomp(&cre, cand_ere, REG_EXTENDED) == 0) {
			char hb[1024];
			regmatch_t cm[2];
			snprintf(hb, sizeof(hb), "href=\"%s\"", ctx->page_url);
			if (regexec(&cre, hb, 2, cm, 0) == 0)
				rel = 2;
			if (rel < 2 && *landing_path) {
				snprintf(hb, sizeof(hb), "href=\"%s\"", landing_path);
				if (regexec(&cre, hb, 2, cm, 0) == 0)
					rel = 2;
			}
			regfree(&cre);
		}
		if (rel < 2) {
			pfx = common_prefix_len(ref_path_ptr(templates[i]),
						landing_path);
			if (pfx > 1)
				rel = 1;
		}

		int better = (rel > best_rel) ||
			     (rel == best_rel && pfx > best_pfx) ||
			     (rel == best_rel && pfx == best_pfx &&
			      cnt > best_cnt);
		if (!best_tmpl || better) {
			best_tmpl = templates[i];
			best_cnt  = cnt;
			best_rel  = rel;
			best_pfx  = pfx;
		}
	}

	if (!best_tmpl || best_cnt < SCAN_SERIES_MIN) {
		for (size_t i = 0; i < nhrefs; i++) {
			free(templates[i]);
			free(hrefs[i]);
		}
		return 0;
	}

	/* Build list_ere: template '#' -> [0-9]+, ERE-escape literals. */
	char ere_body[512];
	size_t eo = 0;
	for (const char *q = best_tmpl; *q && eo + 10 < sizeof(ere_body); ) {
		if (*q == '#') {
			memcpy(ere_body + eo, "[0-9]+", 6);
			eo += 6;
			q++;
		} else {
			char c = *q++;
			if (strchr(".^$*+?()|[]\\", c))
				ere_body[eo++] = '\\';
			ere_body[eo++] = c;
		}
	}
	ere_body[eo] = '\0';

	char list_ere_buf[600];
	int sn = snprintf(list_ere_buf, sizeof(list_ere_buf),
			  "href=[\"'](%s)[\"']", ere_body);
	if (sn <= 0 || (size_t)sn >= sizeof(list_ere_buf)) {
		for (size_t i = 0; i < nhrefs; i++) {
			free(templates[i]);
			free(hrefs[i]);
		}
		return 0;
	}

	/* Verify the ERE compiles under REG_EXTENDED before committing. */
	regex_t lre;
	if (regcomp(&lre, list_ere_buf, REG_EXTENDED) != 0) {
		for (size_t i = 0; i < nhrefs; i++) {
			free(templates[i]);
			free(hrefs[i]);
		}
		return 0;
	}
	regfree(&lre);

	/* Capture the template string and first matching href before freeing. */
	char *tmpl_copy = scan_strdup(best_tmpl);
	char *first = NULL;
	if (!tmpl_copy) {
		for (size_t k = 0; k < nhrefs; k++) {
			free(templates[k]);
			free(hrefs[k]);
		}
		return -1;
	}
	for (size_t i = 0; i < nhrefs; i++) {
		if (strcmp(templates[i], tmpl_copy) == 0 && !first) {
			first = scan_strdup(hrefs[i]);
			if (!first) {
				free(tmpl_copy);
				for (size_t k = 0; k < nhrefs; k++) {
					free(templates[k]);
					free(hrefs[k]);
				}
				return -1;
			}
		}
	}
	for (size_t i = 0; i < nhrefs; i++) {
		free(templates[i]);
		free(hrefs[i]);
	}

	if (!first) {
		free(tmpl_copy);
		return 0;
	}

	r->is_series = 1;
	r->series_path = tmpl_copy;
	r->list_ere = scan_strdup(list_ere_buf);
	if (!r->list_ere) {
		free(first);
		return -1;	/* series_path freed by scan_result_free */
	}
	*first_ep = first;
	return 0;
}

/* Structural (template-based) series detection. Harvest same-site anchor hrefs
 * and delegate to build_series_from_hrefs for template selection. */
static int
detect_series_generic(struct scan_ctx *ctx, const char *body, char **first_ep)
{
	*first_ep = NULL;

	static const char anc_ere[] = "<a[^>]+href=[\"']([^\"'<> ]+)[\"']";
	regex_t re;
	if (regcomp(&re, anc_ere, REG_EXTENDED | REG_ICASE) != 0)
		return 0;

	char *page_host = url_host(ctx->page_url);
	char *hrefs[BSF_MAX];
	size_t nhrefs = 0;

	const char *p = body;
	regmatch_t m[2];
	while (nhrefs < BSF_MAX && regexec(&re, p, 2, m, 0) == 0) {
		if (m[1].rm_so < 0)
			break;
		size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
		char *ref = scan_strndup(p + m[1].rm_so, len);
		if (!ref) {
			regfree(&re);
			free(page_host);
			for (size_t i = 0; i < nhrefs; i++)
				free(hrefs[i]);
			return -1;
		}
		regoff_t adv = m[0].rm_eo > m[0].rm_so ? m[0].rm_eo
							 : m[0].rm_so + 1;
		p += adv;

		if (!scan_followable_ref(ref)) { free(ref); continue; }
		char *abs = extractor_resolve_url(ctx->page_url, ref);
		if (!abs) { free(ref); continue; }
		char *h = url_host(abs);
		int same = (h && page_host && strcmp(h, page_host) == 0);
		free(h);
		free(abs);
		if (!same) { free(ref); continue; }
		int dup = 0;
		for (size_t i = 0; i < nhrefs && !dup; i++)
			if (strcmp(hrefs[i], ref) == 0)
				dup = 1;
		if (dup) { free(ref); continue; }
		hrefs[nhrefs++] = ref;
	}
	regfree(&re);
	free(page_host);

	/* Ownership of hrefs[] transferred to build_series_from_hrefs. */
	return build_series_from_hrefs(ctx, hrefs, nhrefs, first_ep);
}

/* Drive the recursive discovery once the landing page yielded no direct media.
 * Series pages run the chain from the first episode; otherwise from each
 * followed landing link. Returns 0 on success, -1 on a hard error. */
static int
scan_recurse(struct scan_ctx *ctx, const char *body, int max_depth)
{
	scan_step_t steps[SCAN_MAX_CHAIN];
	memset(steps, 0, sizeof(steps));

	char *first_ep = NULL;
	if (detect_series(ctx, body, &first_ep) < 0)
		return -1;

	if (ctx->result->is_series && first_ep) {
		/* Series: run chain discovery from the sample episode (P0). The
		 * generated chain becomes the per-episode pipeline. */
		char *ep_url = extractor_resolve_url(ctx->page_url, first_ep);
		free(first_ep);
		if (!ep_url)
			return -1;
		int rc = 0;
		if (!scan_visited(ctx, ep_url) &&
		    ctx->nfetches < SCAN_MAX_FETCHES) {
			char *ep_body = NULL;
			ctx->nfetches++;
			if (ctx->fetch(ep_url, NULL, 0, &ep_body,
				       ctx->userdata) == 0 && ep_body &&
			    strlen(ep_body) <= SCAN_MAX_PAGE) {
				const char *saved = ctx->page_url;
				ctx->page_url = ep_url;
				ctx->page0_body = ep_body; /* for basename-param heuristic */
				rc = discover_chain(ctx, ep_body, steps, 0, 0,
						    max_depth);
				ctx->page0_body = NULL;
				ctx->page_url = saved;
			}
			free(ep_body);
		}
		free(ep_url);
		return rc;
	}
	free(first_ep);

	/* Non-series: discover a chain from the landing page itself. */
	ctx->page0_body = body;
	int rc2 = discover_chain(ctx, body, steps, 0, 0, max_depth);
	ctx->page0_body = NULL;
	return rc2;
}

/* ---- enrichment (duration / size) ------------------------------------- */

/* For an HLS candidate, fetch the playlist and record total duration and (for a
 * media playlist) variant resolution/bandwidth. Network failures are tolerated:
 * the candidate simply keeps duration 0. */
static void
enrich_hls(struct scan_ctx *ctx, scan_candidate_t *c)
{
#ifndef SCAN_HAVE_HLS
	(void)ctx;
	(void)c;	/* no HLS parser in this build: skip duration enrichment */
	return;
#else
	if (!ctx->fetch)
		return;
	char *body = NULL;
	if (ctx->fetch(c->url, NULL, 0, &body, ctx->userdata) != 0 || !body)
		return;
	if (strlen(body) > SCAN_MAX_PAGE) {
		free(body);
		return;
	}

	char *err = NULL;
	if (strstr(body, "#EXT-X-STREAM-INF") != NULL) {
		/* Master: pick the best variant, record its resolution. */
		hls_master_t *mst = hls_parse_master(body, c->url, &err);
		free(err);
		err = NULL;
		if (mst) {
			int vi = hls_select_variant(mst, "best");
			if (vi >= 0) {
				c->width = mst->variants[vi].width;
				c->height = mst->variants[vi].height;
				c->bandwidth = mst->variants[vi].bandwidth;
				/* Follow the chosen media playlist for duration. */
				char *vbody = NULL;
				if (ctx->fetch(mst->variants[vi].url, NULL, 0,
					       &vbody, ctx->userdata) == 0 &&
				    vbody) {
					if (strlen(vbody) <= SCAN_MAX_PAGE) {
						hls_media_t *md =
							hls_parse_media(vbody,
								mst->variants[vi].url,
								&err);
						free(err);
						err = NULL;
						if (md) {
							c->duration =
								md->total_duration;
							hls_media_free(md);
						}
					}
					free(vbody);
				}
			}
			hls_master_free(mst);
		}
	} else {
		hls_media_t *md = hls_parse_media(body, c->url, &err);
		free(err);
		if (md) {
			c->duration = md->total_duration;
			hls_media_free(md);
		}
	}
	free(body);
#endif				/* SCAN_HAVE_HLS */
}

/* For a direct-file candidate, probe its Content-Length if a probe is set. */
static void
enrich_file(struct scan_ctx *ctx, scan_candidate_t *c)
{
	if (!ctx->probe)
		return;
	long long sz = -1;
	if (ctx->probe(c->url, &sz, ctx->userdata) == 0 && sz > 0)
		c->size = sz;
}

/* ---- scoring ---------------------------------------------------------- */

/* Compute each candidate's score. Signals, strongest first: duration/size,
 * resolution/bitrate, ad-host penalty, DOM context, uniqueness. */
static void
score_candidates(scan_result_t *r)
{
	for (size_t i = 0; i < r->ncands; i++) {
		scan_candidate_t *c = &r->cands[i];
		double s = 0.0;

		/* Duration dominates: a long stream is content. ~10 points per
		 * minute, so a 24-minute episode scores ~240. */
		if (c->duration > 0)
			s += c->duration / 6.0;

		/* Direct-file size: ~1 point per MB, capped so a huge file does
		 * not swamp every other signal. */
		if (c->size > 0) {
			double mb = (double)c->size / (1024.0 * 1024.0);
			if (mb > 500.0)
				mb = 500.0;
			s += mb;
		}

		/* Resolution / bitrate (HLS variant). */
		if (c->height > 0)
			s += c->height / 10.0;	/* 1080 -> +108 */
		else if (c->bandwidth > 0)
			s += (double)c->bandwidth / 100000.0;

		/* DOM/source context. */
		if (c->context == SCAN_CTX_PLAYER)
			s += 50.0;
		else if (c->context == SCAN_CTX_AD)
			s -= 200.0;

		/* Ad-host blocklist: a hard penalty independent of context. */
		if (c->ad_host)
			s -= 500.0;

		/* Uniqueness: a stream that appears many times is more likely
		 * an ad creative reused across slots; a slight penalty. */
		if (c->count > 1)
			s -= (c->count - 1) * 5.0;

		/* HLS is usually the real adaptive content vs a one-off ad mp4;
		 * a tiny nudge when nothing else separates them. */
		if (c->kind == SCAN_KIND_HLS)
			s += 1.0;

		c->score = s;
	}
}

/* Sort the candidate list in place by descending score (stable insertion sort:
 * the list is small and we want deterministic ties for reproducible configs). */
static void
sort_candidates(scan_result_t *r)
{
	for (size_t i = 1; i < r->ncands; i++) {
		scan_candidate_t key = r->cands[i];
		size_t j = i;
		while (j > 0 && r->cands[j - 1].score < key.score) {
			r->cands[j] = r->cands[j - 1];
			j--;
		}
		r->cands[j] = key;
	}
}

/* If the landing page has >= SCAN_SERIES_MIN direct-media candidates that share
 * a numeric template (maximal [0-9]+ runs replaced with '#'), mark the result
 * as a media-list series and build list_ere from the dominant extension.
 * Also handles GDrive series (no numeric template required).
 * Returns 0 on success, -1 on OOM. No-op when is_series already set. */
static int
detect_direct_media_series(scan_result_t *r)
{
	if (r->is_series || r->ncands < SCAN_SERIES_MIN)
		return 0;
	/* Only fire when all candidates are depth-0 (direct, no chain). */
	for (size_t i = 0; i < r->ncands; i++)
		if (r->cands[i].nchain > 0)
			return 0;

	/* GDrive series: >= SCAN_SERIES_MIN GDrive candidates => series.
	 * IDs are random so no numeric template is needed.  Pick the list ERE
	 * based on the predominant raw link form seen on the page (open?id= vs
	 * file/d/); that form is recorded on each candidate by collect_gdrive. */
	{
		size_t ngdrive = 0;
		size_t nfiled  = 0;
		for (size_t i = 0; i < r->ncands; i++) {
			if (r->cands[i].kind != SCAN_KIND_GDRIVE)
				continue;
			ngdrive++;
			if (r->cands[i].gdrive_filed)
				nfiled++;
		}
		if (ngdrive >= SCAN_SERIES_MIN && ngdrive == r->ncands) {
			/* Pick the predominant raw page link form. */
			int use_filed = (nfiled * 2 >= ngdrive); /* majority */
			const char *list_ere = use_filed
				? "(https?://drive\\.google\\.com/file/d/[A-Za-z0-9_-]+)"
				: "(https?://drive\\.google\\.com/open\\?id=[A-Za-z0-9_-]+)";
			regex_t re;
			if (regcomp(&re, list_ere, REG_EXTENDED | REG_ICASE) != 0)
				return 0;
			regfree(&re);
			char *le = scan_strdup(list_ere);
			if (!le)
				return -1;
			r->list_ere  = le;
			r->is_series = 1;
			/* Stash the form so scan_emit_config can emit the right var. */
			r->gdrive_series_filed = use_filed;
			return 0;
		}
	}

	/* Template: replace every maximal [0-9]+ run with '#'.
	 * Count how many candidates share the most common template. */
	char templates[SCAN_MAX_CANDIDATES][512];
	size_t ntmpl = r->ncands;
	for (size_t i = 0; i < ntmpl; i++) {
		const char *src = r->cands[i].url;
		size_t o = 0;
		for (const char *p = src; *p && o + 2 < sizeof(templates[i]); ) {
			if (isdigit((unsigned char)*p)) {
				if (o + 1 < sizeof(templates[i]))
					templates[i][o++] = '#';
				while (isdigit((unsigned char)*p))
					p++;
			} else {
				templates[i][o++] = *p++;
			}
		}
		templates[i][o] = '\0';
	}

	/* Find dominant template (most members). */
	size_t best_cnt = 0;
	size_t best_ti  = 0;
	for (size_t i = 0; i < ntmpl; i++) {
		size_t cnt = 0;
		for (size_t j = 0; j < ntmpl; j++)
			if (strcmp(templates[i], templates[j]) == 0)
				cnt++;
		if (cnt > best_cnt) {
			best_cnt = cnt;
			best_ti  = i;
		}
	}
	if (best_cnt < SCAN_SERIES_MIN)
		return 0;

	/* Detect the file extension from the dominant template's representative
	 * URL (e.g. "mp4"). Strip query/fragment first so strrchr('.') doesn't
	 * produce ext="mp4?token=ABC" and a malformed ERE. */
	const char *rep_url = r->cands[best_ti].url;
	/* Find end of path (before '?' or '#'). */
	size_t path_len = strcspn(rep_url, "?#");
	char path_only[512];
	if (path_len >= sizeof(path_only))
		path_len = sizeof(path_only) - 1;
	memcpy(path_only, rep_url, path_len);
	path_only[path_len] = '\0';

	const char *dot = strrchr(path_only, '.');
	if (!dot || dot[1] == '\0')
		return 0;
	const char *ext = dot + 1;

	/* Build ERE: (https?:[^"'<> &]+\.<ext>([?#][^"'<> ]*)?).
	 * POSIX ERE, one capture group, no braces, regcomp-verified before storing.
	 * The optional query tail reuses the catch-all shape so signed URLs with
	 * &-separated params are captured in full. */
	char ere[160];
	int n = snprintf(ere, sizeof(ere),
			 "(https?:[^\"'<> &]+\\.%s([?#][^\"'<> ]*)?)", ext);
	if (n <= 0 || (size_t)n >= sizeof(ere))
		return 0;

	regex_t re;
	if (regcomp(&re, ere, REG_EXTENDED | REG_ICASE) != 0)
		return 0;
	regfree(&re);

	char *list_ere = scan_strdup(ere);
	if (!list_ere)
		return -1;

	r->list_ere  = list_ere;
	r->is_series = 1;
	/* series_path left NULL: match is derived from host + landing path segment
	 * in scan_emit_config via the is_series branch. */
	return 0;
}

/* ---- public: scan ----------------------------------------------------- */

scan_result_t *
scan_page(const char *page_url, extractor_fetch_fn fetch, scan_probe_fn probe,
	  int max_depth, void *userdata, char **err)
{
	if (err)
		*err = NULL;
	if (!page_url) {
		scan_set_err(err, "scan: no page URL");
		return NULL;
	}
	if (!fetch) {
		scan_set_err(err, "scan: no HTTP fetcher available");
		return NULL;
	}

	scan_result_t *r = calloc(1, sizeof(*r));
	if (!r) {
		scan_set_err(err, "out of memory");
		return NULL;
	}
	r->page_url = scan_strdup(page_url);
	if (!r->page_url) {
		scan_set_err(err, "out of memory");
		free(r);
		return NULL;
	}

	/* Fetch the landing page. */
	char *body = NULL;
	if (fetch(page_url, NULL, 0, &body, userdata) != 0 || !body) {
		scan_set_err(err, "scan: failed to fetch %s", page_url);
		scan_result_free(r);
		return NULL;
	}
	if (strlen(body) > SCAN_MAX_PAGE) {
		free(body);
		scan_set_err(err, "scan: page is too large (> %d bytes)",
			     SCAN_MAX_PAGE);
		scan_result_free(r);
		return NULL;
	}

	struct scan_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.result = r;
	ctx.page_url = page_url;
	ctx.fetch = fetch;
	ctx.probe = probe;
	ctx.userdata = userdata;
	scan_visited(&ctx, page_url);	/* the landing page counts as visited */
	ctx.nfetches = 1;		/* landing page already fetched above */

	if (collect_patterns(&ctx, body) < 0 ||
	    collect_iframes(&ctx, body, 0) < 0) {
		free(body);
		for (size_t i = 0; i < ctx.nvisited; i++)
			free(ctx.visited[i]);
		scan_set_err(err, "out of memory while scanning page");
		scan_result_free(r);
		return NULL;
	}

	/* No direct media: follow watch/play/embed links (and detect series) to
	 * reach the player a bounded number of hops away (#scan-recur). */
	if (r->ncands == 0 && max_depth >= 1) {
		if (scan_recurse(&ctx, body, max_depth) < 0) {
			free(body);
			for (size_t i = 0; i < ctx.nvisited; i++)
				free(ctx.visited[i]);
			scan_set_err(err, "out of memory while scanning page");
			scan_result_free(r);
			return NULL;
		}
	}
	free(body);
	for (size_t i = 0; i < ctx.nvisited; i++)
		free(ctx.visited[i]);
	ctx.nvisited = 0; /* prevent double-free if detect_direct_media_series OOMs */

	/* Enrich and score (recursive candidates participate in scoring).
	 * Skip GDrive (no HEAD needed) and cap file probes. See #perf-scan. */
#define SCAN_MAX_PROBES 12
	int nprobes = 0;
	for (size_t i = 0; i < r->ncands; i++) {
		if (r->cands[i].kind == SCAN_KIND_HLS)
			enrich_hls(&ctx, &r->cands[i]);
		else if (r->cands[i].kind == SCAN_KIND_GDRIVE)
			; /* skip: slow HEAD + irrelevant for ranking */
		else if (nprobes < SCAN_MAX_PROBES) {
			enrich_file(&ctx, &r->cands[i]);
			nprobes++;
		}
	}
	score_candidates(r);
	sort_candidates(r);

	/* Promote direct multi-episode pages (all URLs in the page, numeric
	 * template) to a media-list series so emit generates a list config. */
	if (detect_direct_media_series(r) < 0) {
		for (size_t i = 0; i < ctx.nvisited; i++)
			free(ctx.visited[i]);
		scan_set_err(err, "out of memory while scanning page");
		scan_result_free(r);
		return NULL;
	}

	return r;
}

void
scan_result_free(scan_result_t *r)
{
	if (!r)
		return;
	free(r->page_url);
	free(r->list_ere);
	free(r->series_path);
	for (size_t i = 0; i < r->ncands; i++) {
		free(r->cands[i].url);
		free(r->cands[i].host);
		free(r->cands[i].param_token);
		free(r->cands[i].media_base);
		for (size_t j = 0; j < r->cands[i].nchain; j++) {
			free(r->cands[i].chain[j].ere);
			free(r->cands[i].chain[j].sample);
		}
	}
	free(r);
}

/* ---- config generation ------------------------------------------------ */

/* Derive a config `name` from the page host: the registrable-ish label before
 * the public suffix (e.g. www.animeworld.ac -> animeworld). Falls back to the
 * whole host with dots stripped. Writes into dst (always NUL-terminated). */
static void
derive_name(const char *host, char *dst, size_t len)
{
	if (!host || !*host || len == 0) {
		snprintf(dst, len ? len : 1, "site");
		return;
	}
	/* Collect up to 4 dot positions so we can index labels right-to-left. */
	const char *dots[4];
	int ndots = 0;
	for (const char *p = host; *p && ndots < 4; p++)
		if (*p == '.')
			dots[ndots++] = p;

	/* Generic second-level / public-suffix tokens (case-insensitive). When the
	 * second-to-last label matches one of these and there is a third-to-last
	 * label, step back one more to get the registrable name. See #derive-name. */
	static const char *const sld_tokens[] = {
		"com", "co", "net", "org", "edu", "gov", "mil", "ac",
		"gob", "gouv", "ne", "or", "go", "nom", "ltd", "plc", NULL
	};

	const char *start;
	size_t n;
	if (ndots >= 2) {
		/* last label = dots[ndots-1]+1 .. end
		 * second-to-last label = dots[ndots-2]+1 .. dots[ndots-1] */
		const char *sld_start = dots[ndots - 2] + 1;
		size_t sld_len = (size_t)(dots[ndots - 1] - sld_start);
		int is_generic = 0;
		for (int ti = 0; sld_tokens[ti] && !is_generic; ti++) {
			size_t tl = strlen(sld_tokens[ti]);
			if (tl == sld_len &&
			    strncasecmp(sld_start, sld_tokens[ti], tl) == 0)
				is_generic = 1;
		}
		if (is_generic && ndots >= 2) {
			/* Step back one more to get the registrable label.
			 * kissanime.com.cv: ndots=2, step to the label before dots[0]. */
			start = (ndots >= 3) ? dots[ndots - 3] + 1 : host;
			n = (size_t)(dots[ndots - 2] - start);
		} else {
			start = dots[ndots - 2] + 1;
			n = (size_t)(dots[ndots - 1] - start);
		}
	} else if (ndots == 1) {
		start = host;
		n = (size_t)(dots[0] - host);
	} else {
		start = host;
		n = strlen(host);
	}
	/* Strip a leading "www" label if it leaked in. */
	if (n >= 4 && strncasecmp(start, "www.", 4) == 0) {
		start += 4;
		n -= 4;
	}
	if (n == 0 || n >= len)
		n = strlen(host) < len ? strlen(host) : len - 1;
	/* An all-digit label means an IP-literal host (e.g. 127.0.0.1); there is
	 * no meaningful site name, so use a generic placeholder. */
	int all_digits = n > 0;
	for (size_t i = 0; i < n; i++)
		if (!isdigit((unsigned char)start[i])) {
			all_digits = 0;
			break;
		}
	if (all_digits) {
		snprintf(dst, len, "site");
		return;
	}
	size_t o = 0;
	for (size_t i = 0; i < n && o + 1 < len; i++) {
		char ch = start[i];
		dst[o++] = isalnum((unsigned char)ch) ? (char)tolower((unsigned char)ch)
						      : '_';
	}
	dst[o] = '\0';
	if (o == 0)
		snprintf(dst, len, "site");
}

/* Build a 'match' ERE from the page host: escape dots so the regex is literal
 * on the host (e.g. animeworld\.ac). Writes into dst (NUL-terminated). */
static void
derive_match(const char *host, char *dst, size_t len)
{
	if (!host || !*host || len == 0) {
		snprintf(dst, len ? len : 1, ".");
		return;
	}
	size_t o = 0;
	for (const char *p = host; *p && o + 2 < len; p++) {
		if (*p == '.')
			dst[o++] = '\\';
		dst[o++] = *p;
	}
	dst[o] = '\0';
}

/* Resolve which candidate to emit: an explicit index, or the top-ranked one.
 * Returns the candidate pointer, or NULL if the list is empty/out of range. */
static const scan_candidate_t *
pick_candidate(const scan_result_t *r, int chosen)
{
	if (r->ncands == 0)
		return NULL;
	size_t idx = 0;
	if (chosen >= 0) {
		if ((size_t)chosen >= r->ncands)
			return NULL;
		idx = (size_t)chosen;
	}
	return &r->cands[idx];
}

/* Append `path` to a 'match' ERE buffer, escaping regex metacharacters so the
 * token is matched literally (e.g. "/play/" -> "/play/"). Only '.' needs
 * escaping for the path tokens we emit, but escape the common ERE specials to
 * be safe. Writes onto the end of dst (already NUL-terminated). */
static void
match_append_path(char *dst, size_t len, const char *path)
{
	if (!path || !*path)
		return;
	size_t o = strlen(dst);
	for (const char *p = path; *p && o + 2 < len; p++) {
		if (strchr(".+*?()[]{}|^$\\", *p))
			dst[o++] = '\\';
		dst[o++] = *p;
	}
	dst[o] = '\0';
}

/* Emit the ranked-candidate comment block (shared by direct and recursive
 * modes) so the user sees the signals the scanner weighed. */
static void
emit_candidate_comments(const scan_result_t *r, FILE *out)
{
	fprintf(out, "# Detected media candidates (best first):\n");
	for (size_t i = 0; i < r->ncands; i++) {
		const scan_candidate_t *d = &r->cands[i];
		fprintf(out, "#   [%zu] %s%s\n", i,
			d->kind == SCAN_KIND_HLS    ? "(hls)    " :
			d->kind == SCAN_KIND_GDRIVE ? "(gdrive) " : "(file)   ",
			d->url);
		fprintf(out, "#        score=%.0f", d->score);
		if (d->duration > 0)
			fprintf(out, " duration=%.0fs", d->duration);
		if (d->size > 0)
			fprintf(out, " size=%lldB", d->size);
		if (d->height > 0)
			fprintf(out, " res=%dx%d", d->width, d->height);
		else if (d->bandwidth > 0)
			fprintf(out, " bw=%ldbps", d->bandwidth);
		if (d->ad_host)
			fprintf(out, " [ad-host]");
		if (d->context == SCAN_CTX_PLAYER)
			fprintf(out, " [player]");
		if (d->nchain >= 2)
			fprintf(out, " [%d hop(s)]", d->depth);
		fprintf(out, "\n");
	}
	fprintf(out, "\n");
}

/* Emit the 1-hop filename-as-param pipeline when c->param_token is set.
 * page0 = {url}, capture <token>=(<basename>), output <base>{<token>}. */
static void
emit_param_pipeline(const scan_candidate_t *c, FILE *out)
{
	/* Build the capture ERE: <token>=([^"'&<> ]+\.<ext>).
	 * The extension comes from the last '.' in the media basename. */
	const char *slash = strrchr(c->url, '/');
	const char *basename = slash ? slash + 1 : c->url;
	const char *dot = strrchr(basename, '.');
	/* ERE-escape the extension dot -> \. */
	char ext_ere[32] = "mp4"; /* fallback */
	if (dot && dot[1] != '\0') {
		size_t ei = 0;
		ext_ere[ei++] = '\\';
		ext_ere[ei++] = '.';
		const char *ep = dot + 1;
		while (*ep && ei < sizeof(ext_ere) - 1)
			ext_ere[ei++] = *ep++;
		ext_ere[ei] = '\0';
	}
	fprintf(out, "get    page0 <- {url}\n");
	fprintf(out, "var    %s <- page0 regex %s=([^\"'&<> ]+%s)\n",
		c->param_token, c->param_token, ext_ere);
	fprintf(out, "output %s{%s}\n", c->media_base, c->param_token);
}

/* Emit the multi-step get/var pipeline for a chained candidate `c`, with the
 * first get reading {url}. Each step i<nchain-1 is a LINK step (var linkI +
 * get pageI+1); the final step is the media capture (var media) followed by
 * `output {media}`. */
static void
emit_chain_pipeline(const scan_candidate_t *c, FILE *out)
{
	fprintf(out, "get    page0 <- {url}\n");
	for (size_t i = 0; i < c->nchain; i++) {
		const scan_step_t *s = &c->chain[i];
		if (i < c->nchain - 1) {
			/* LINK step: capture the next page's ref, then fetch it. */
			if (s->sample)
				fprintf(out, "# via %s\n", s->sample);
			fprintf(out, "var    link%zu <- page%zu regex %s\n",
				i, i, s->ere);
			fprintf(out, "get    page%zu <- {link%zu}\n", i + 1, i);
		} else {
			/* FINAL step: capture the media URL and output it. */
			if (s->sample)
				fprintf(out, "# media: %s\n", s->sample);
			fprintf(out, "var    media <- page%zu regex %s\n",
				i, s->ere);
			fprintf(out, "output {media}\n");
		}
	}
}

int
scan_emit_config(const scan_result_t *r, int chosen, FILE *out)
{
	if (!r || !out)
		return -1;

	char *host = url_host(r->page_url);
	char name[128];
	char match[256];
	derive_name(host, name, sizeof(name));
	derive_match(host, match, sizeof(match));
	/* For series, tighten match from the LANDING page path so extractor_matches
	 * fires on the series index URL, not on episode URLs. See #scan-series-match. */
	if (r->is_series) {
		char *seg = url_first_path_segment(r->page_url);
		if (seg) {
			match_append_path(match, sizeof(match), "/");
			match_append_path(match, sizeof(match), seg);
			free(seg);
		}
	} else if (r->series_path) {
		match_append_path(match, sizeof(match), r->series_path);
	}

	const scan_candidate_t *c = pick_candidate(r, chosen);

	fprintf(out, "# Hyperflux extractor config (generated by --extract-scan)\n");
	fprintf(out, "# Source page: %s\n", r->page_url);
	fprintf(out, "# Review and edit before use: a config issues HTTP requests\n");
	fprintf(out, "# with arbitrary headers. Treat shared configs like scripts.\n");
	fprintf(out, "\n");
	fprintf(out, "name   %s\n", name);
	fprintf(out, "match  %s\n", match);
	fprintf(out, "\n");

	/* SERIES: emit the list setup, then the per-episode pipeline (the chosen
	 * candidate's chain, with page0 fetched from {url} = the episode). */
	if (r->is_series && r->list_ere) {
		if (c)
			emit_candidate_comments(r, out);
		fprintf(out, "# Series index detected: lists episodes, multi-select on `flux <episode-url>`.\n");
		fprintf(out, "get    page <- {url}\n");
		fprintf(out, "list   eps  <- page regex %s\n", r->list_ere);
		fprintf(out, "\n");
		if (c && c->nchain == 0 && c->kind == SCAN_KIND_GDRIVE) {
			/* GDrive series: listed URL is the share link; extract the ID
			 * using the right regex for the link form on this page. */
			if (r->gdrive_series_filed)
				fprintf(out, "var    gid  <- url regex /d/([A-Za-z0-9_-]+)\n");
			else
				fprintf(out, "var    gid  <- url regex id=([A-Za-z0-9_-]+)\n");
			fprintf(out, "output https://drive.usercontent.google.com/download?id={gid}&export=download&confirm=t\n");
		} else if (c && c->nchain == 0) {
			/* Direct media-list series: each listed URL IS the media. */
			fprintf(out, "output {url}\n");
		} else if (c && c->nchain >= 1) {
			fprintf(out, "# per-episode pipeline ({url} is each episode):\n");
			if (c->param_token)
				emit_param_pipeline(c, out);
			else
				emit_chain_pipeline(c, out);
		} else {
			/* Series detected but no episode media reachable within depth.
			 * Emit a parseable stub; adjust the media regex before use. */
			fprintf(out, "# per-episode pipeline ({url} is each episode); adjust regex:\n");
			fprintf(out, "get    page0 <- {url}\n");
			fprintf(out, "var    media <- page0 regex src=\"([^\"]+\\.mp4)\"\n");
			fprintf(out, "output {media}\n");
		}
		free(host);
		return 0;
	}

	if (!c) {
		/* No candidate at all: pure skeleton with guided TODOs. The
		 * media URL was not discoverable in the page (JS/API case). */
		fprintf(out, "# The scanner found no direct media URL on this page.\n");
		fprintf(out, "# The player most likely loads it from an internal API.\n");
		fprintf(out, "# Fill in the steps below, then test with:  flux <page-url>\n");
		fprintf(out, "\n");
		fprintf(out, "# 1) Capture an id from the page URL (edit the regex).\n");
		fprintf(out, "# var    id     <- url     regex /([A-Za-z0-9]+)$\n");
		fprintf(out, "\n");
		fprintf(out, "# 2) Fetch the API endpoint that returns the media URL.\n");
		fprintf(out, "# get    player <- https://%s/api/...?id={id}\n",
			host ? host : "HOST");
		fprintf(out, "#        header Referer={url}\n");
		fprintf(out, "#        header X-Requested-With=XMLHttpRequest\n");
		fprintf(out, "\n");
		fprintf(out, "# 3) Capture the media URL from the API response.\n");
		fprintf(out, "# var    media  <- player  regex \"file\":\"([^\"]+)\"\n");
		fprintf(out, "\n");
		fprintf(out, "# TODO output {media}\n");
		free(host);
		return 0;
	}

	/* Show the ranked alternatives as comments so the user sees the signals
	 * the scanner weighed and can switch the output by hand. */
	emit_candidate_comments(r, out);

	/* RECURSIVE non-series: the media is reached via a multi-step chain. */
	if (c->nchain >= 2) {
		if (c->param_token) {
			fprintf(out, "# 1-hop filename-as-param config (basename on page0):\n");
			emit_param_pipeline(c, out);
		} else {
			fprintf(out, "# Media reached via %d hop(s); multi-step config:\n",
				c->depth);
			emit_chain_pipeline(c, out);
		}
		free(host);
		return 0;
	}

	/* DIRECT: the chosen media URL is present in the page text, so a static
	 * output line resolves it directly. The user can swap in a different [N]. */
	if (c->kind == SCAN_KIND_GDRIVE) {
		/* Single GDrive video: capture the ID from the page link using the
		 * right regex for the form seen on this page. */
		fprintf(out, "# Google Drive video detected; 1-hop download config:\n");
		fprintf(out, "get    page0 <- {url}\n");
		if (c->gdrive_filed)
			fprintf(out, "var    gid   <- page0 regex drive\\.google\\.com/file/d/([A-Za-z0-9_-]+)\n");
		else
			fprintf(out, "var    gid   <- page0 regex drive\\.google\\.com/open\\?id=([A-Za-z0-9_-]+)\n");
		fprintf(out, "output https://drive.usercontent.google.com/download?id={gid}&export=download&confirm=t\n");
	} else {
		fprintf(out, "# Media URL detected directly in the page.\n");
		if (c->kind == SCAN_KIND_HLS)
			fprintf(out, "# HLS playlist -> downloaded segment-by-segment.\n");
		fprintf(out, "output %s\n", c->url);
	}

	free(host);
	return 0;
}

int
scan_config_name(const scan_result_t *r, char *dst, size_t len)
{
	if (!r || !dst || len == 0)
		return -1;
	char *host = url_host(r->page_url);	/* may be NULL: derive_name copes */
	derive_name(host, dst, len);
	free(host);
	return 0;
}
