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

/* Generalized reusable list EREs paired with scan_episode_eres above: group 1
 * captures the family of episode hrefs (not just the first one). Same index. */
static const char *const scan_episode_list_eres[] = {
	"href=[\"']([^\"'<> ]*/ep/[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*/episode/[^\"'<> ]+)[\"']",
	"href=[\"']([^\"'<> ]*/e/[0-9][^\"'<> ]*)[\"']",
	"href=[\"']([^\"'<> ]*-episode-[0-9][^\"'<> ]*)[\"']",
	"href=[\"']([^\"'<> ]*[?&]ep=[0-9]+[^\"'<> ]*)[\"']",
};

/* A stable path token for each episode family, used to tighten the match line. */
static const char *const scan_episode_paths[] = {
	"/ep/", "/episode/", "/e/", "-episode-", "ep=",
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

/* Collect up to SCAN_MAX_FOLLOW unique, followable "next page" links from
 * `text`. Each output entry holds the resolved absolute URL, the raw ref (for a
 * provenance comment), and the ERE that captured it. Caller frees abs/ref of
 * each filled slot. Returns the number filled, or -1 on OOM. */
struct scan_follow {
	char *abs;	/* resolved absolute URL (owned) */
	char *ref;	/* raw captured ref (owned) */
	const char *ere;	/* the link ERE that matched (borrowed, static) */
};

static int
collect_follow_links(struct scan_ctx *ctx, const char *text,
		     struct scan_follow *out, size_t max)
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
					out[n].abs = abs;
					out[n].ref = ref;
					out[n].ere = ere;
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
	return (int)n;
}

static void
free_follow_links(struct scan_follow *links, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free(links[i].abs);
		free(links[i].ref);
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
	int nl = collect_follow_links(ctx, body, links, SCAN_MAX_FOLLOW);
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
		steps[nsteps].ere = (char *)links[i].ere;
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
	scan_result_t *r = ctx->result;

	for (size_t pi = 0;
	     pi < sizeof(scan_episode_eres) / sizeof(scan_episode_eres[0]);
	     pi++) {
		regex_t re;
		if (regcomp(&re, scan_episode_eres[pi],
			    REG_EXTENDED | REG_ICASE) != 0)
			continue;
		/* Count distinct hrefs (bounded) and remember the first. */
		char *seen[SCAN_SERIES_MIN * 4];
		size_t nseen = 0;
		char *first = NULL;
		const char *p = body;
		regmatch_t m[2];
		while (regexec(&re, p, 2, m, 0) == 0) {
			if (m[1].rm_so < 0)
				break;
			size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
			char *ref = scan_strndup(p + m[1].rm_so, len);
			if (!ref) {
				for (size_t i = 0; i < nseen; i++)
					free(seen[i]);
				free(first);
				regfree(&re);
				return -1;
			}
			int dup = 0;
			for (size_t i = 0; i < nseen; i++)
				if (strcmp(seen[i], ref) == 0) {
					dup = 1;
					break;
				}
			if (!dup && nseen < sizeof(seen) / sizeof(seen[0])) {
				if (!first)
					first = scan_strdup(ref);
				seen[nseen++] = ref;
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

		if (nseen >= SCAN_SERIES_MIN && first) {
			r->is_series = 1;
			r->list_ere = scan_strdup(scan_episode_list_eres[pi]);
			r->series_path = scan_strdup(scan_episode_paths[pi]);
			*first_ep = first;
			first = NULL;
			for (size_t i = 0; i < nseen; i++)
				free(seen[i]);
			if (!r->list_ere || !r->series_path)
				return -1;
			return 0;
		}
		for (size_t i = 0; i < nseen; i++)
			free(seen[i]);
		free(first);
	}
	return 0;
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
				rc = discover_chain(ctx, ep_body, steps, 0, 0,
						    max_depth);
				ctx->page_url = saved;
			}
			free(ep_body);
		}
		free(ep_url);
		return rc;
	}
	free(first_ep);

	/* Non-series: discover a chain from the landing page itself. */
	return discover_chain(ctx, body, steps, 0, 0, max_depth);
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

	/* Enrich and score (recursive candidates participate in scoring). */
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
	free(r->list_ere);
	free(r->series_path);
	for (size_t i = 0; i < r->ncands; i++) {
		free(r->cands[i].url);
		free(r->cands[i].host);
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
		if (d->nchain >= 2)
			fprintf(out, " [%d hop(s)]", d->depth);
		fprintf(out, "\n");
	}
	fprintf(out, "\n");
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
		if (c && c->nchain >= 1) {
			fprintf(out, "# per-episode pipeline ({url} is each episode):\n");
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
		fprintf(out, "# Media reached via %d hop(s); multi-step config:\n",
			c->depth);
		emit_chain_pipeline(c, out);
		free(host);
		return 0;
	}

	/* DIRECT: the chosen media URL is present in the page text, so a static
	 * output line resolves it directly. The user can swap in a different [N]. */
	fprintf(out, "# Media URL detected directly in the page.\n");
	if (c->kind == SCAN_KIND_HLS)
		fprintf(out, "# HLS playlist -> downloaded segment-by-segment.\n");
	fprintf(out, "output %s\n", c->url);

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
