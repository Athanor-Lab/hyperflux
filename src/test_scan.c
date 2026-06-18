/* Standalone unit tests for the page scanner. Build:
 *   cc -D_DEFAULT_SOURCE -DSCAN_HAVE_HLS -Wall -Wextra -g \
 *      src/scan.c src/extractor.c src/hls.c src/test_scan.c -lcrypto \
 *      -o /tmp/test_scan && /tmp/test_scan
 *
 * The scanner is free-standing: HTTP is injected via callbacks, so these tests
 * run over saved HTML fixtures with no network. SCAN_HAVE_HLS pulls in the pure
 * half of hls.c so HLS candidate durations are computed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan.h"

static int failures;
static int checks;

#define CHECK(cond, msg)						\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			printf("FAIL %s\n", (msg));			\
			failures++;					\
		}							\
	} while (0)

/* ---- fake site: a URL->body map for the injected fetch/probe -----------
 * Each test registers a small table of (url, body) pairs and (url, size) pairs;
 * the stubs look them up. An unknown URL fails the fetch (returns -1), letting a
 * test assert that a missing API endpoint yields a skeleton config. */

struct kv { const char *url; const char *body; };
struct kvsz { const char *url; long long size; };

struct fake_site {
	const struct kv *pages;
	size_t npages;
	const struct kvsz *sizes;
	size_t nsizes;
};

static int
fake_fetch(const char *url, const ext_header_t *headers, size_t nheaders,
	   char **out_body, void *userdata)
{
	(void)headers;
	(void)nheaders;
	struct fake_site *s = userdata;

	*out_body = NULL;
	for (size_t i = 0; i < s->npages; i++) {
		if (strcmp(s->pages[i].url, url) == 0) {
			char *b = malloc(strlen(s->pages[i].body) + 1);
			if (!b)
				return -1;
			strcpy(b, s->pages[i].body);
			*out_body = b;
			return 0;
		}
	}
	return -1;	/* unknown URL: fetch fails */
}

static int
fake_probe(const char *url, long long *out_size, void *userdata)
{
	struct fake_site *s = userdata;

	if (out_size)
		*out_size = -1;
	for (size_t i = 0; i < s->nsizes; i++) {
		if (strcmp(s->sizes[i].url, url) == 0) {
			if (out_size)
				*out_size = s->sizes[i].size;
			return 0;
		}
	}
	return -1;
}

/* Render the generated config into a heap buffer via open_memstream so tests
 * can grep it. Caller frees *out. Returns 0 on success. */
static int
emit_to_string(const scan_result_t *r, int chosen, char **out)
{
	size_t len = 0;
	*out = NULL;
	FILE *fp = open_memstream(out, &len);
	if (!fp)
		return -1;
	int rc = scan_emit_config(r, chosen, fp);
	fclose(fp);
	if (rc != 0) {
		free(*out);
		*out = NULL;
	}
	return rc;
}

/* ---- (a) direct <video>/<source> -> near-complete config --------------- */

static void
test_direct_source(void)
{
	static const struct kv pages[] = {
		{ "https://vids.example.com/watch/clip-42",
		  "<html><head>"
		  "<meta property=\"og:video\" content=\"https://cdn.example.com/v/clip-42.mp4\">"
		  "</head><body>"
		  "<video controls>"
		  "<source src=\"https://cdn.example.com/v/clip-42.mp4\" type=\"video/mp4\">"
		  "</video></body></html>" },
	};
	static const struct kvsz sizes[] = {
		{ "https://cdn.example.com/v/clip-42.mp4", 300LL * 1024 * 1024 },
	};
	struct fake_site site = { pages, 1, sizes, 1 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     &site, &err);
	CHECK(r != NULL, "(a) direct-source page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands == 1, "(a) one media candidate (deduplicated)");
	if (r->ncands >= 1) {
		CHECK(strcmp(r->cands[0].url,
			     "https://cdn.example.com/v/clip-42.mp4") == 0,
		      "(a) candidate URL is the mp4");
		CHECK(r->cands[0].kind == SCAN_KIND_FILE, "(a) classified as file");
		CHECK(r->cands[0].context == SCAN_CTX_PLAYER, "(a) player context");
		CHECK(r->cands[0].size == 300LL * 1024 * 1024,
		      "(a) probed size recorded");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(a) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "name   vids") != NULL ||
		      strstr(cfg, "name   example") != NULL,
		      "(a) name derived from host");
		CHECK(strstr(cfg, "match  vids\\.example\\.com") != NULL,
		      "(a) match escapes dots");
		CHECK(strstr(cfg, "output https://cdn.example.com/v/clip-42.mp4")
		      != NULL, "(a) output is the detected mp4");
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (b) HLS in inline JSON -> config + duration ----------------------- */

static void
test_hls_in_json(void)
{
	static const struct kv pages[] = {
		{ "https://stream.example.org/e/9000",
		  "<html><body><div id=\"player\"></div>"
		  "<script>var setup = {"
		  "\"file\":\"https://hls.example.org/9000/master.m3u8\","
		  "\"image\":\"poster.jpg\"};</script>"
		  "</body></html>" },
		{ "https://hls.example.org/9000/master.m3u8",
		  "#EXTM3U\n"
		  "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x720\n"
		  "720/index.m3u8\n" },
		{ "https://hls.example.org/9000/720/index.m3u8",
		  "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-MEDIA-SEQUENCE:0\n"
		  "#EXTINF:600.0,\nseg0.ts\n"
		  "#EXTINF:600.0,\nseg1.ts\n#EXT-X-ENDLIST\n" },
	};
	struct fake_site site = { pages, 3, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     &site, &err);
	CHECK(r != NULL, "(b) HLS-in-JSON page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands >= 1, "(b) at least one candidate");
	if (r->ncands >= 1) {
		CHECK(r->cands[0].kind == SCAN_KIND_HLS, "(b) top is HLS");
		CHECK(strcmp(r->cands[0].url,
			     "https://hls.example.org/9000/master.m3u8") == 0,
		      "(b) HLS master URL detected");
		CHECK(r->cands[0].duration > 1199.0 &&
		      r->cands[0].duration < 1201.0,
		      "(b) duration summed from media playlist (1200s)");
		CHECK(r->cands[0].height == 720, "(b) variant resolution recorded");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(b) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "output https://hls.example.org/9000/master.m3u8")
		      != NULL, "(b) output is the m3u8");
		CHECK(strstr(cfg, "HLS playlist") != NULL,
		      "(b) HLS note present");
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (c) ad creative vs real content: scorer ranks content above ad ---- */

static void
test_ad_vs_content(void)
{
	/* The page has a short ad mp4 on a blocklisted host (doubleclick) AND a
	 * long content HLS stream in the player JSON. Content must rank first. */
	static const struct kv pages[] = {
		{ "https://watch.example.net/ep/season1/77",
		  "<html><body>"
		  "<iframe src=\"https://ads.doubleclick.net/preroll?id=x\"></iframe>"
		  "<script>"
		  "var ad = {\"file\":\"https://ads.doubleclick.net/creative/spot.mp4\"};"
		  "var player = {\"file\":\"https://cdn.example.net/hls/77/master.m3u8\"};"
		  "</script>"
		  "</body></html>" },
		/* The same-origin iframe is NOT followed (doubleclick is cross
		 * origin), but the ad mp4 still appears via the catch-all/JSON
		 * pass so the scorer has to beat it. */
		{ "https://cdn.example.net/hls/77/master.m3u8",
		  "#EXTM3U\n"
		  "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080\n"
		  "1080/index.m3u8\n" },
		{ "https://cdn.example.net/hls/77/1080/index.m3u8",
		  "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:0\n"
		  "#EXTINF:720.0,\na.ts\n#EXTINF:720.0,\nb.ts\n#EXT-X-ENDLIST\n" },
	};
	static const struct kvsz sizes[] = {
		{ "https://ads.doubleclick.net/creative/spot.mp4",
		  2LL * 1024 * 1024 },		/* tiny 2 MB ad clip */
	};
	struct fake_site site = { pages, 3, sizes, 1 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     &site, &err);
	CHECK(r != NULL, "(c) ad-vs-content page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands >= 2, "(c) both ad and content collected");
	/* The top candidate must be the content HLS, not the ad mp4. */
	CHECK(r->ncands >= 1 && strcmp(r->cands[0].url,
		"https://cdn.example.net/hls/77/master.m3u8") == 0,
	      "(c) content ranks above the ad");

	/* Find the ad candidate and assert it is flagged + ranks last. */
	int ad_idx = -1;
	for (size_t i = 0; i < r->ncands; i++)
		if (strcmp(r->cands[i].url,
			   "https://ads.doubleclick.net/creative/spot.mp4") == 0)
			ad_idx = (int)i;
	CHECK(ad_idx >= 0, "(c) ad candidate present");
	if (ad_idx >= 0) {
		CHECK(r->cands[ad_idx].ad_host == 1, "(c) ad host flagged");
		CHECK(r->cands[ad_idx].score < r->cands[0].score,
		      "(c) ad scores below content");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(c) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "output https://cdn.example.net/hls/77/master.m3u8")
		      != NULL, "(c) output is the content stream");
		CHECK(strstr(cfg, "[ad-host]") != NULL,
		      "(c) ad host annotated in comments");
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (d) AnimeWorld-like: media NOT in HTML -> skeleton + TODO --------- */

static void
test_skeleton_when_no_media(void)
{
	/* The landing page references an episode id but no media URL: the
	 * player fetches it from an API the scanner can't see. Expect zero
	 * candidates and a skeleton config with guided TODOs. */
	static const struct kv pages[] = {
		{ "https://www.animeworld.ac/play/some-anime/AbC123",
		  "<html><body>"
		  "<div id=\"player\" data-id=\"AbC123\"></div>"
		  "<a class=\"episode\" data-num=\"1\" "
		  "href=\"/play/some-anime/AbC123\">1</a>"
		  "<script src=\"/assets/player.js\"></script>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     &site, &err);
	CHECK(r != NULL, "(d) animeworld-like page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands == 0, "(d) no media candidate found in HTML");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(d) skeleton config emits");
	if (cfg) {
		CHECK(strstr(cfg, "name   animeworld") != NULL,
		      "(d) name derived from animeworld host");
		CHECK(strstr(cfg, "match  ") != NULL, "(d) has a match line");
		CHECK(strstr(cfg, "TODO") != NULL, "(d) guided TODO present");
		CHECK(strstr(cfg, "internal API") != NULL,
		      "(d) explains the API case");
		CHECK(strstr(cfg, "regex") != NULL,
		      "(d) skeleton suggests an id-capture regex");
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- ad-host blocklist unit ------------------------------------------- */

static void
test_ad_host_blocklist(void)
{
	CHECK(scan_is_ad_host("doubleclick.net"), "exact ad host");
	CHECK(scan_is_ad_host("ads.doubleclick.net"), "subdomain ad host");
	CHECK(scan_is_ad_host("pagead2.googlesyndication.com"),
	      "googlesyndication subdomain");
	CHECK(scan_is_ad_host("imasdk.googleapis.com"), "imasdk host");
	CHECK(scan_is_ad_host("adservice.google.de"), "adservice.* prefix");
	CHECK(!scan_is_ad_host("cdn.example.com"), "content host not flagged");
	CHECK(!scan_is_ad_host("notdoubleclick.net"),
	      "lookalike host not flagged (dot-boundary)");
	CHECK(!scan_is_ad_host(NULL), "NULL host safe");
	CHECK(!scan_is_ad_host(""), "empty host safe");
}

int
main(void)
{
	test_direct_source();
	test_hls_in_json();
	test_ad_vs_content();
	test_skeleton_when_no_media();
	test_ad_host_blocklist();

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
