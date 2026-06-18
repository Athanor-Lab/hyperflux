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
};

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
	/* Catch-all: any bare media URL in the text (unknown context). */
	{ "(https?:[^\"'[:space:]<>()]+\\.(m3u8|mp4|webm)([?#][^\"'[:space:]<>()]*)?)",
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

/* ---- public: scan ----------------------------------------------------- */

scan_result_t *
scan_page(const char *page_url, extractor_fetch_fn fetch, scan_probe_fn probe,
	  void *userdata, char **err)
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

	struct scan_ctx ctx = { r, page_url, fetch, probe, userdata };

	if (collect_patterns(&ctx, body) < 0 ||
	    collect_iframes(&ctx, body, 0) < 0) {
		free(body);
		scan_set_err(err, "out of memory while scanning page");
		scan_result_free(r);
		return NULL;
	}
	free(body);

	/* Enrich and score. */
	for (size_t i = 0; i < r->ncands; i++) {
		if (r->cands[i].kind == SCAN_KIND_HLS)
			enrich_hls(&ctx, &r->cands[i]);
		else
			enrich_file(&ctx, &r->cands[i]);
	}
	score_candidates(r);
	sort_candidates(r);

	return r;
}

void
scan_result_free(scan_result_t *r)
{
	if (!r)
		return;
	free(r->page_url);
	for (size_t i = 0; i < r->ncands; i++) {
		free(r->cands[i].url);
		free(r->cands[i].host);
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
	/* Find the last two dot-separated labels; take the first of those. */
	const char *last = NULL, *prev = NULL;
	for (const char *p = host; *p; p++) {
		if (*p == '.') {
			prev = last;
			last = p;
		}
	}
	const char *start;
	size_t n;
	if (last && prev) {
		start = prev + 1;
		n = (size_t)(last - start);
	} else if (last) {
		start = host;
		n = (size_t)(last - host);
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

	const scan_candidate_t *c = pick_candidate(r, chosen);

	fprintf(out, "# Hyperflux extractor config (generated by --extract-scan)\n");
	fprintf(out, "# Source page: %s\n", r->page_url);
	fprintf(out, "# Review and edit before use: a config issues HTTP requests\n");
	fprintf(out, "# with arbitrary headers. Treat shared configs like scripts.\n");
	fprintf(out, "\n");
	fprintf(out, "name   %s\n", name);
	fprintf(out, "match  %s\n", match);
	fprintf(out, "\n");

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
	fprintf(out, "# Detected media candidates (best first):\n");
	for (size_t i = 0; i < r->ncands; i++) {
		const scan_candidate_t *d = &r->cands[i];
		fprintf(out, "#   [%zu] %s%s\n", i,
			d->kind == SCAN_KIND_HLS ? "(hls)  " : "(file) ",
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
		fprintf(out, "\n");
	}
	fprintf(out, "\n");

	/* The chosen media URL is present in the page text, so a static output
	 * line resolves it directly. The user can swap in a different [N]. */
	fprintf(out, "# Media URL detected directly in the page.\n");
	if (c->kind == SCAN_KIND_HLS)
		fprintf(out, "# HLS playlist -> downloaded segment-by-segment.\n");
	fprintf(out, "output %s\n", c->url);

	free(host);
	return 0;
}
