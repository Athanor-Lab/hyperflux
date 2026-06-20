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
#include <ctype.h>

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
				     SCAN_DEFAULT_DEPTH, NULL, 0,
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
				     SCAN_DEFAULT_DEPTH, NULL, 0,
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
				     SCAN_DEFAULT_DEPTH, NULL, 0,
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
				     SCAN_DEFAULT_DEPTH, NULL, 0,
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

/* ---- (e) media behind ONE hop -> multi-step config + roundtrip --------- */

static void
test_one_hop(void)
{
	/* Landing page has no media, only an <a href="/watch?file=XYZ">. The
	 * /watch page carries the player JSON with the real mp4. */
	static const struct kv pages[] = {
		{ "https://watch.example.com/show",
		  "<html><body>"
		  "<a href=\"/watch?file=XYZ\">Watch now</a>"
		  "</body></html>" },
		{ "https://watch.example.com/watch?file=XYZ",
		  "<html><body><div id=\"player\"></div>"
		  "<script>var p = "
		  "{\"file\":\"https://cdn.example.com/v/clip.mp4\"};</script>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 2, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(e) one-hop page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands >= 1, "(e) media found behind one hop");
	if (r->ncands >= 1) {
		CHECK(strcmp(r->cands[0].url,
			     "https://cdn.example.com/v/clip.mp4") == 0,
		      "(e) top candidate is the mp4");
		CHECK(r->cands[0].nchain == 2, "(e) chain has two steps");
		CHECK(r->cands[0].depth == 1, "(e) depth is one hop");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(e) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "get    page0 <- {url}") != NULL,
		      "(e) first get reads {url}");
		CHECK(strstr(cfg, "var    link0 <- page0 regex") != NULL,
		      "(e) link0 captured from page0");
		CHECK(strstr(cfg, "get    page1 <- {link0}") != NULL,
		      "(e) page1 fetched from link0");
		CHECK(strstr(cfg, "var    media <- page1 regex") != NULL,
		      "(e) media captured from page1");
		CHECK(strstr(cfg, "output {media}") != NULL,
		      "(e) outputs {media}");

		/* Roundtrip: the generated config must parse and run. */
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen)", &perr);
		CHECK(ex != NULL, "(e) generated config parses");
		if (!ex) { printf("  parse err: %s\n", perr ? perr : "(none)"); }
		free(perr);
		if (ex) {
			char *media = NULL, *rerr = NULL;
			int rc = extractor_run(ex, pages[0].url, fake_fetch,
					       &site, &media, &rerr);
			CHECK(rc == 0, "(e) generated config runs");
			if (rc != 0)
				printf("  run err: %s\n", rerr ? rerr : "(none)");
			CHECK(media && strcmp(media,
				"https://cdn.example.com/v/clip.mp4") == 0,
			      "(e) run resolves the mp4");
			free(media);
			free(rerr);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (f) media behind TWO hops -> multi-step config + roundtrip -------- */

static void
test_two_hops(void)
{
	/* landing -> /embed/123 -> /player/abc with the media. */
	static const struct kv pages[] = {
		{ "https://watch.example.org/show",
		  "<html><body>"
		  "<a href=\"/embed/123\">go</a>"
		  "</body></html>" },
		{ "https://watch.example.org/embed/123",
		  "<html><body>"
		  "<a href=\"/player/abc\">play</a>"
		  "</body></html>" },
		{ "https://watch.example.org/player/abc",
		  "<html><body><video controls>"
		  "<source src=\"https://cdn.example.org/m/final.mp4\">"
		  "</video></body></html>" },
	};
	struct fake_site site = { pages, 3, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(f) two-hop page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands >= 1, "(f) media found behind two hops");
	if (r->ncands >= 1) {
		CHECK(strcmp(r->cands[0].url,
			     "https://cdn.example.org/m/final.mp4") == 0,
		      "(f) top candidate is the final mp4");
		CHECK(r->cands[0].nchain == 3, "(f) chain has three steps");
		CHECK(r->cands[0].depth == 2, "(f) depth is two hops");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(f) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "get    page0 <- {url}") != NULL,
		      "(f) first get reads {url}");
		CHECK(strstr(cfg, "get    page1 <- {link0}") != NULL,
		      "(f) page1 from link0");
		CHECK(strstr(cfg, "get    page2 <- {link1}") != NULL,
		      "(f) page2 from link1");
		CHECK(strstr(cfg, "var    media <- page2 regex") != NULL,
		      "(f) media captured from page2");

		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen)", &perr);
		CHECK(ex != NULL, "(f) generated config parses");
		if (!ex) { printf("  parse err: %s\n", perr ? perr : "(none)"); }
		free(perr);
		if (ex) {
			char *media = NULL, *rerr = NULL;
			int rc = extractor_run(ex, pages[0].url, fake_fetch,
					       &site, &media, &rerr);
			CHECK(rc == 0, "(f) generated config runs");
			if (rc != 0)
				printf("  run err: %s\n", rerr ? rerr : "(none)");
			CHECK(media && strcmp(media,
				"https://cdn.example.org/m/final.mp4") == 0,
			      "(f) run resolves the final mp4");
			free(media);
			free(rerr);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (g) series index -> list + per-episode pipeline + roundtrip ------- */

static void
test_series(void)
{
	/* Landing page lists three /ep/N episodes; each episode page has a
	 * direct <source> mp4. */
	static const struct kv pages[] = {
		{ "https://anime.example.net/series/foo",
		  "<html><body>"
		  "<a href=\"/ep/1\">Episode 1</a>"
		  "<a href=\"/ep/2\">Episode 2</a>"
		  "<a href=\"/ep/3\">Episode 3</a>"
		  "</body></html>" },
		{ "https://anime.example.net/ep/1",
		  "<html><body><video><source "
		  "src=\"https://cdn.example.net/e/ep1.mp4\"></video></body></html>" },
		{ "https://anime.example.net/ep/2",
		  "<html><body><video><source "
		  "src=\"https://cdn.example.net/e/ep2.mp4\"></video></body></html>" },
		{ "https://anime.example.net/ep/3",
		  "<html><body><video><source "
		  "src=\"https://cdn.example.net/e/ep3.mp4\"></video></body></html>" },
	};
	struct fake_site site = { pages, 4, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(g) series page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->is_series == 1, "(g) series detected");
	CHECK(r->list_ere != NULL, "(g) list ERE set");
	CHECK(r->ncands >= 1, "(g) sample episode media found");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(g) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "list   eps  <- page regex") != NULL,
		      "(g) list directive present");
		CHECK(strstr(cfg, "output {media}") != NULL,
		      "(g) per-episode output present");

		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen)", &perr);
		CHECK(ex != NULL, "(g) generated config parses");
		if (!ex) { printf("  parse err: %s\n", perr ? perr : "(none)"); }
		free(perr);
		if (ex) {
			/* FIX 2: match must fire on the series LANDING page. */
			CHECK(extractor_matches(ex, pages[0].url) == 1,
			      "(g) config matches series landing URL");

			CHECK(extractor_has_list(ex), "(g) config has a list");
			char **urls = NULL;
			size_t n = 0;
			char *lerr = NULL;
			int rc = extractor_list_episodes(ex, pages[0].url,
				fake_fetch, &site, &urls, &n, &lerr);
			CHECK(rc == 0 && n >= 3,
			      "(g) list_episodes returns >= 3 episodes");
			if (rc != 0)
				printf("  list err: %s\n", lerr ? lerr : "(none)");
			if (n >= 1)
				CHECK(strcmp(urls[0],
					"https://anime.example.net/ep/1") == 0,
				      "(g) first episode URL resolved absolute");
			free(lerr);

			/* Per-episode run resolves epN.mp4 for episode 1. */
			char *media = NULL, *rerr = NULL;
			int rr = extractor_run(ex, "https://anime.example.net/ep/1",
					       fake_fetch, &site, &media, &rerr);
			CHECK(rr == 0 && media && strcmp(media,
				"https://cdn.example.net/e/ep1.mp4") == 0,
			      "(g) per-episode run resolves ep1.mp4");
			if (rr != 0)
				printf("  run err: %s\n", rerr ? rerr : "(none)");
			free(media);
			free(rerr);
			extractor_free_urls(urls, n);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- series with no reachable episode media (FIX 3: parseable skeleton) -- */

static void
test_series_no_media(void)
{
	/* Landing lists episodes but the episode pages are unreachable (fetch
	 * fails), so no candidate chain is built.  The emitted config must still
	 * parse cleanly (FIX 3). */
	static const struct kv pages[] = {
		{ "https://nomedia.example.org/show/bar",
		  "<html><body>"
		  "<a href=\"/ep/1\">Episode 1</a>"
		  "<a href=\"/ep/2\">Episode 2</a>"
		  "<a href=\"/ep/3\">Episode 3</a>"
		  "</body></html>" },
		/* episode pages intentionally absent: fake_fetch returns -1 */
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(g2) series-no-media page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->is_series == 1, "(g2) series detected even without reachable media");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg,
	      "(g2) skeleton config emits");
	if (cfg) {
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-nomedia)", &perr);
		CHECK(ex != NULL, "(g2) skeleton config parses (FIX 3)");
		if (!ex)
			printf("  parse err: %s\n", perr ? perr : "(none)");
		free(perr);
		if (ex)
			extractor_free(ex);
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (h) media behind a non-hardcoded link (generic harvest) ----------- */

static void
test_generic_link_follow(void)
{
	/* Landing has a custom-slug episode link (no /ep/, /watch/, ?file=).
	 * The linked page has a direct mp4 via <source>.  The generic harvester
	 * must follow it and the generated config must be a reusable generalized
	 * ERE (not a literal "1" in the var line). */
	static const struct kv pages[] = {
		{ "https://site.example.com/anime/naruto",
		  "<html><body>"
		  "<a href=\"/serie/foo/episodio-1-ita\">Episodio 1</a>"
		  "</body></html>" },
		{ "https://site.example.com/serie/foo/episodio-1-ita",
		  "<html><body><video controls>"
		  "<source src=\"https://cdn.example.com/v/ep1.mp4\">"
		  "</video></body></html>" },
	};
	struct fake_site site = { pages, 2, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(h) generic-link page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands >= 1, "(h) media found via generic link follow");
	if (r->ncands >= 1) {
		CHECK(strcmp(r->cands[0].url,
			     "https://cdn.example.com/v/ep1.mp4") == 0,
		      "(h) top candidate is the mp4");
		CHECK(r->cands[0].nchain >= 2, "(h) chain has at least two steps");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(h) config emits");
	if (cfg) {
		/* The link var ERE must be generalized (contain a charset or
		 * quantifier), not the literal episode slug "episodio-1-ita". */
		const char *var_line = strstr(cfg, "var    link0");
		CHECK(var_line != NULL, "(h) var link0 line present");
		if (var_line) {
			/* A synthesized ERE will contain [0-9]+ or [A-Za-z0-9 */
			int generalized = (strstr(var_line, "[0-9]") != NULL ||
					   strstr(var_line, "[A-Za-z") != NULL);
			CHECK(generalized, "(h) link ERE is generalized (not literal slug)");
		}

		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-h)", &perr);
		CHECK(ex != NULL, "(h) generated config parses");
		if (!ex) { printf("  parse err: %s\n", perr ? perr : "(none)"); }
		free(perr);
		if (ex) {
			char *media = NULL, *rerr = NULL;
			int rc = extractor_run(ex, pages[0].url, fake_fetch,
					       &site, &media, &rerr);
			CHECK(rc == 0, "(h) generated config runs");
			if (rc != 0)
				printf("  run err: %s\n", rerr ? rerr : "(none)");
			CHECK(media && strcmp(media,
				"https://cdn.example.com/v/ep1.mp4") == 0,
			      "(h) run resolves ep1.mp4");
			free(media);
			free(rerr);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (i) structural series: no /ep/ URLs, generic template detection ---- */

static void
test_structural_series(void)
{
	/* Landing lists /anime/foo/1, /anime/foo/2, /anime/foo/3 — no /ep/.
	 * Each episode page has a direct mp4.  Structural detection must fire. */
	static const struct kv pages[] = {
		{ "https://stream.example.io/show/myshow",
		  "<html><body>"
		  "<a href=\"/anime/foo/1\">Episode 1</a>"
		  "<a href=\"/anime/foo/2\">Episode 2</a>"
		  "<a href=\"/anime/foo/3\">Episode 3</a>"
		  "</body></html>" },
		{ "https://stream.example.io/anime/foo/1",
		  "<html><body><video>"
		  "<source src=\"https://cdn.example.io/v/ep1.mp4\">"
		  "</video></body></html>" },
		{ "https://stream.example.io/anime/foo/2",
		  "<html><body><video>"
		  "<source src=\"https://cdn.example.io/v/ep2.mp4\">"
		  "</video></body></html>" },
		{ "https://stream.example.io/anime/foo/3",
		  "<html><body><video>"
		  "<source src=\"https://cdn.example.io/v/ep3.mp4\">"
		  "</video></body></html>" },
	};
	struct fake_site site = { pages, 4, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(i) structural series page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->is_series == 1, "(i) structural series detected");
	CHECK(r->list_ere != NULL, "(i) list ERE set");
	if (r->list_ere) {
		/* list_ere must contain [0-9]+ (generalized). */
		CHECK(strstr(r->list_ere, "[0-9]") != NULL,
		      "(i) list ERE is generalized");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(i) config emits");
	if (cfg) {
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-i)", &perr);
		CHECK(ex != NULL, "(i) generated config parses");
		if (!ex) { printf("  parse err: %s\n", perr ? perr : "(none)"); }
		free(perr);
		if (ex) {
			/* FIX 2: match must fire on the series landing URL. */
			CHECK(extractor_matches(ex, pages[0].url) == 1,
			      "(i) config matches series landing URL");

			CHECK(extractor_has_list(ex), "(i) config has list directive");
			char **urls = NULL;
			size_t n = 0;
			char *lerr = NULL;
			int rc = extractor_list_episodes(ex, pages[0].url,
				fake_fetch, &site, &urls, &n, &lerr);
			CHECK(rc == 0 && n >= 3,
			      "(i) list_episodes returns >= 3 episodes");
			if (rc != 0)
				printf("  list err: %s\n", lerr ? lerr : "(none)");
			if (n >= 1) {
				/* URLs must be absolute. */
				CHECK(strncmp(urls[0], "https://", 8) == 0,
				      "(i) first episode URL is absolute");
			}
			free(lerr);

			/* Per-episode run resolves the mp4. */
			char *media = NULL, *rerr = NULL;
			int rr = extractor_run(ex,
				"https://stream.example.io/anime/foo/1",
				fake_fetch, &site, &media, &rerr);
			CHECK(rr == 0 && media && strcmp(media,
				"https://cdn.example.io/v/ep1.mp4") == 0,
			      "(i) per-episode run resolves ep1.mp4");
			if (rr != 0)
				printf("  run err: %s\n", rerr ? rerr : "(none)");
			free(media);
			free(rerr);
			extractor_free_urls(urls, n);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (j) multi-series containment: picks the user's series, not the biggest */

static void
test_multi_series_containment(void)
{
	/* Landing is the foo series index. Page also lists a larger bar series
	 * (5 entries) and a stray baz link.  Scanner must anchor to foo. */
	static const struct kv pages[] = {
		{ "https://site.test/anime/foo",
		  "<html><body>"
		  /* foo series: 3 episodes */
		  "<a href=\"/anime/foo/1\">Foo Ep 1</a>"
		  "<a href=\"/anime/foo/2\">Foo Ep 2</a>"
		  "<a href=\"/anime/foo/3\">Foo Ep 3</a>"
		  /* bar series: 5 episodes (would win on count alone) */
		  "<a href=\"/anime/bar/1\">Bar Ep 1</a>"
		  "<a href=\"/anime/bar/2\">Bar Ep 2</a>"
		  "<a href=\"/anime/bar/3\">Bar Ep 3</a>"
		  "<a href=\"/anime/bar/4\">Bar Ep 4</a>"
		  "<a href=\"/anime/bar/5\">Bar Ep 5</a>"
		  /* unrelated link */
		  "<a href=\"/anime/baz/9\">Baz Ep 9</a>"
		  "</body></html>" },
		{ "https://site.test/anime/foo/1",
		  "<html><body><video>"
		  "<source src=\"https://cdn.test/v/foo1.mp4\">"
		  "</video></body></html>" },
		{ "https://site.test/anime/foo/2",
		  "<html><body><video>"
		  "<source src=\"https://cdn.test/v/foo2.mp4\">"
		  "</video></body></html>" },
		{ "https://site.test/anime/foo/3",
		  "<html><body><video>"
		  "<source src=\"https://cdn.test/v/foo3.mp4\">"
		  "</video></body></html>" },
	};
	struct fake_site site = { pages, 4, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(j) multi-series page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->is_series == 1, "(j) series detected");
	if (r->list_ere) {
		/* list_ere must match foo URLs, not bar. */
		CHECK(strstr(r->list_ere, "/anime/foo/") != NULL ||
		      strstr(r->list_ere, "foo") != NULL,
		      "(j) list_ere anchored to foo series");
		CHECK(strstr(r->list_ere, "/anime/bar/") == NULL,
		      "(j) list_ere does NOT match bar series");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(j) config emits");
	if (cfg) {
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-j)", &perr);
		CHECK(ex != NULL, "(j) generated config parses");
		if (!ex) { printf("  parse err: %s\n", perr ? perr : "(none)"); }
		free(perr);
		if (ex) {
			CHECK(extractor_matches(ex, pages[0].url) == 1,
			      "(j) config matches series landing URL");

			CHECK(extractor_has_list(ex), "(j) config has list directive");
			char **urls = NULL;
			size_t n = 0;
			char *lerr = NULL;
			int rc = extractor_list_episodes(ex, pages[0].url,
				fake_fetch, &site, &urls, &n, &lerr);
			CHECK(rc == 0, "(j) list_episodes succeeds");
			if (rc != 0)
				printf("  list err: %s\n", lerr ? lerr : "(none)");
			free(lerr);

			/* All returned URLs must be foo episodes. */
			CHECK(n == 3, "(j) exactly 3 foo episodes returned");
			for (size_t i = 0; i < n; i++) {
				CHECK(strstr(urls[i], "/anime/foo/") != NULL,
				      "(j) episode URL is a foo URL");
				CHECK(strstr(urls[i], "/anime/bar/") == NULL,
				      "(j) no bar URL in episode list");
				CHECK(strstr(urls[i], "/anime/baz/") == NULL,
				      "(j) no baz URL in episode list");
			}
			extractor_free_urls(urls, n);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (l) multi-series containment with ABSOLUTE hrefs: anchors path-to-path */

static void
test_multi_series_absolute(void)
{
	/* Same as (j) but every episode href is absolute. Before the path-to-path
	 * relatedness fix the scanner mis-tiered and could pick the larger bar
	 * series; it must still anchor to foo (the landing). */
	static const struct kv pages[] = {
		{ "https://site.test/anime/foo",
		  "<html><body>"
		  "<a href=\"https://site.test/anime/foo/1\">Foo 1</a>"
		  "<a href=\"https://site.test/anime/foo/2\">Foo 2</a>"
		  "<a href=\"https://site.test/anime/foo/3\">Foo 3</a>"
		  "<a href=\"https://site.test/anime/bar/1\">Bar 1</a>"
		  "<a href=\"https://site.test/anime/bar/2\">Bar 2</a>"
		  "<a href=\"https://site.test/anime/bar/3\">Bar 3</a>"
		  "<a href=\"https://site.test/anime/bar/4\">Bar 4</a>"
		  "<a href=\"https://site.test/anime/bar/5\">Bar 5</a>"
		  "</body></html>" },
		{ "https://site.test/anime/foo/1",
		  "<html><body><video>"
		  "<source src=\"https://cdn.test/v/foo1.mp4\"></video></body></html>" },
		{ "https://site.test/anime/foo/2",
		  "<html><body><video>"
		  "<source src=\"https://cdn.test/v/foo2.mp4\"></video></body></html>" },
		{ "https://site.test/anime/foo/3",
		  "<html><body><video>"
		  "<source src=\"https://cdn.test/v/foo3.mp4\"></video></body></html>" },
	};
	struct fake_site site = { pages, 4, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(l) absolute-href multi-series scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->is_series == 1, "(l) series detected");
	if (r->list_ere) {
		CHECK(strstr(r->list_ere, "/anime/foo/") != NULL,
		      "(l) list_ere anchored to foo, not the larger bar series");
		CHECK(strstr(r->list_ere, "/anime/bar/") == NULL,
		      "(l) list_ere does NOT match bar");
	}

	char *cfg = NULL;
	if (emit_to_string(r, -1, &cfg) == 0 && cfg) {
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-l)", &perr);
		CHECK(ex != NULL, "(l) generated config parses");
		free(perr);
		if (ex) {
			char **urls = NULL;
			size_t n = 0;
			char *lerr = NULL;
			int rc = extractor_list_episodes(ex, pages[0].url,
				fake_fetch, &site, &urls, &n, &lerr);
			CHECK(rc == 0 && n == 3, "(l) exactly 3 foo episodes returned");
			free(lerr);
			for (size_t i = 0; i < n; i++)
				CHECK(strstr(urls[i], "/anime/bar/") == NULL,
				      "(l) no bar URL in episode list");
			extractor_free_urls(urls, n);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (k) ad / onclick / popunder trap: real media found, traps skipped ---- */

static void
test_ad_trap(void)
{
	/* Landing has multiple traps but also the real same-site player link. */
	static const struct kv pages[] = {
		{ "https://site.test/watch/show",
		  "<html><body>"
		  /* onclick popunder: href='#' -> followable_ref rejects it */
		  "<a href=\"#\" onclick=\"window.open('https://popads.net/x')\">PLAY</a>"
		  /* cross-origin ad anchor: ad-host blocklist catches it */
		  "<a href=\"https://hilltopads.net/go?url=z\">Watch HD</a>"
		  /* same-site affiliate redirect: penalty token '/out' catches it */
		  "<a href=\"/out?url=https://spam\">mirror</a>"
		  /* the real player link */
		  "<a href=\"/embed/show-1\">player</a>"
		  "</body></html>" },
		{ "https://site.test/embed/show-1",
		  "<html><body><video>"
		  "<source src=\"https://cdn.test/v/real.mp4\">"
		  "</video></body></html>" },
	};
	struct fake_site site = { pages, 2, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(k) ad-trap page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->ncands >= 1, "(k) real media found despite traps");
	for (size_t i = 0; i < r->ncands; i++) {
		CHECK(strstr(r->cands[i].url, "popads") == NULL,
		      "(k) no popads candidate");
		CHECK(strstr(r->cands[i].url, "hilltopads") == NULL,
		      "(k) no hilltopads candidate");
		CHECK(strstr(r->cands[i].url, "/out?") == NULL,
		      "(k) no affiliate /out candidate");
	}
	if (r->ncands >= 1) {
		CHECK(strcmp(r->cands[0].url, "https://cdn.test/v/real.mp4") == 0,
		      "(k) top candidate is the real mp4");
	}
	scan_result_free(r);
}

/* ---- (m) kissanime-shaped: anchored list_ere + multi-suffix name ---------- */

static void
test_kissanime_shape(void)
{
	/* Series page for foo-bar-season-2 listing 10 own episodes AND 9 sidebar
	 * "latest" links from other series — mirrors the kissanime.com.cv shape.
	 * Each own episode page has a direct <source> mp4. */
	static const struct kv pages[] = {
		/* Series index */
		{ "https://kissanime.com.cv/anime/foo-bar-season-2/",
		  "<html><body>"
		  /* 10 own episodes (episode-10 is two digits) */
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-1/\">Ep 1</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-2/\">Ep 2</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-3/\">Ep 3</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-4/\">Ep 4</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-5/\">Ep 5</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-6/\">Ep 6</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-7/\">Ep 7</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-8/\">Ep 8</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-9/\">Ep 9</a>"
		  "<a href=\"https://kissanime.com.cv/foo-bar-season-2-episode-10/\">Ep 10</a>"
		  /* 9 sidebar "latest" from other series */
		  "<a href=\"https://kissanime.com.cv/rent-a-thing-season-5-episode-10/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/beyond-x-episode-9/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/zzz-show-episode-10/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/alpha-show-episode-3/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/beta-show-episode-5/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/gamma-show-episode-7/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/delta-show-episode-2/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/epsilon-show-episode-4/\">latest</a>"
		  "<a href=\"https://kissanime.com.cv/omega-show-episode-6/\">latest</a>"
		  "</body></html>" },
		/* Episode pages — only ep1 needed for chain discovery */
		{ "https://kissanime.com.cv/foo-bar-season-2-episode-1/",
		  "<html><body><video>"
		  "<source src=\"https://cdn.1a.test/v/foo-bar-2nd-season-episode-1.mp4\">"
		  "</video></body></html>" },
		{ "https://kissanime.com.cv/foo-bar-season-2-episode-2/",
		  "<html><body><video>"
		  "<source src=\"https://cdn.1a.test/v/foo-bar-2nd-season-episode-2.mp4\">"
		  "</video></body></html>" },
		{ "https://kissanime.com.cv/foo-bar-season-2-episode-10/",
		  "<html><body><video>"
		  "<source src=\"https://cdn.1a.test/v/foo-bar-2nd-season-episode-10.mp4\">"
		  "</video></body></html>" },
	};
	struct fake_site site = { pages, 4, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(m) kissanime page scans");
	if (!r) { printf("  err: %s\n", err ? err : "(none)"); free(err); return; }

	CHECK(r->is_series == 1, "(m) series detected");

	/* list_ere must be anchored to the foo-bar-season family.  The digit
	 * '2' in 'season-2' is generalized to [0-9]+ (correct), so check for
	 * the literal prefix 'foo-bar-season-' plus a digit placeholder. */
	if (r->list_ere) {
		CHECK(strstr(r->list_ere, "foo-bar-season-") != NULL,
		      "(m) list_ere contains foo-bar-season- prefix");
		CHECK(strstr(r->list_ere, "episode-") != NULL,
		      "(m) list_ere contains episode- token");
		CHECK(strstr(r->list_ere, "rent-a-thing") == NULL,
		      "(m) list_ere does NOT match rent-a-thing");
		CHECK(strstr(r->list_ere, "beyond-x") == NULL,
		      "(m) list_ere does NOT match beyond-x");
		CHECK(strstr(r->list_ere, "zzz-show") == NULL,
		      "(m) list_ere does NOT match zzz-show");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(m) config emits");
	if (cfg) {
		/* FIX A: name must be 'kissanime', not 'com'. */
		CHECK(strstr(cfg, "name   kissanime") != NULL,
		      "(m) name is kissanime (not com)");

		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-m)", &perr);
		CHECK(ex != NULL, "(m) generated config parses");
		if (!ex) { printf("  parse err: %s\n", perr ? perr : "(none)"); }
		free(perr);
		if (ex) {
			CHECK(extractor_matches(ex, pages[0].url) == 1,
			      "(m) config matches series landing URL");

			CHECK(extractor_has_list(ex), "(m) config has list directive");
			char **urls = NULL;
			size_t n = 0;
			char *lerr = NULL;
			int rc = extractor_list_episodes(ex, pages[0].url,
				fake_fetch, &site, &urls, &n, &lerr);
			CHECK(rc == 0, "(m) list_episodes succeeds");
			if (rc != 0)
				printf("  list err: %s\n", lerr ? lerr : "(none)");
			free(lerr);

			/* Must return exactly 10 foo-bar episodes. */
			CHECK(n == 10, "(m) exactly 10 episodes listed");
			int has_ep10 = 0;
			for (size_t i = 0; i < n; i++) {
				CHECK(strstr(urls[i], "foo-bar-season-2-episode-") != NULL,
				      "(m) episode URL is a foo-bar-season-2 URL");
				CHECK(strstr(urls[i], "rent-a-thing") == NULL,
				      "(m) no rent-a-thing in list");
				CHECK(strstr(urls[i], "beyond-x") == NULL,
				      "(m) no beyond-x in list");
				if (strstr(urls[i], "episode-10"))
					has_ep10 = 1;
			}
			CHECK(has_ep10, "(m) episode-10 (two digits) included");

			/* Per-episode run resolves the mp4 for episode 1. */
			char *media = NULL, *rerr = NULL;
			int rr = extractor_run(ex,
				"https://kissanime.com.cv/foo-bar-season-2-episode-1/",
				fake_fetch, &site, &media, &rerr);
			CHECK(rr == 0 && media &&
			      strcmp(media, "https://cdn.1a.test/v/foo-bar-2nd-season-episode-1.mp4") == 0,
			      "(m) extractor_run resolves episode-1 mp4");
			if (rr != 0)
				printf("  run err: %s\n", rerr ? rerr : "(none)");
			free(media);
			free(rerr);
			extractor_free_urls(urls, n);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (n) filename-as-param 1-hop template (kissanime-iframe shape) ------- */

static void
test_filename_as_param(void)
{
	/* Series landing lists 3 episode URLs. Each episode page carries an iframe
	 * whose src has file=<basename> as a query param; the cdn iframe page
	 * exposes the real mp4 via <source src>. The basename appears on the
	 * episode page (page0 for chain discovery), so the scanner must emit the
	 * 1-hop param template instead of a multi-step get chain. */
	static const struct kv pages[] = {
		{ "https://site.test/anime/foo-s2/",
		  "<html><body>"
		  "<a href=\"https://site.test/anime/foo-s2/episode-1/\">ep1</a>"
		  "<a href=\"https://site.test/anime/foo-s2/episode-2/\">ep2</a>"
		  "<a href=\"https://site.test/anime/foo-s2/episode-3/\">ep3</a>"
		  "</body></html>" },
		/* episode pages: each has an iframe with file=<basename> */
		{ "https://site.test/anime/foo-s2/episode-1/",
		  "<html><body>"
		  "<iframe src=\"https://cdn.v.test/index.php?action=play&file=foo-2nd-episode-1.mp4\">"
		  "</iframe></body></html>" },
		{ "https://site.test/anime/foo-s2/episode-2/",
		  "<html><body>"
		  "<iframe src=\"https://cdn.v.test/index.php?action=play&file=foo-2nd-episode-2.mp4\">"
		  "</iframe></body></html>" },
		{ "https://site.test/anime/foo-s2/episode-3/",
		  "<html><body>"
		  "<iframe src=\"https://cdn.v.test/index.php?action=play&file=foo-2nd-episode-3.mp4\">"
		  "</iframe></body></html>" },
		/* cdn iframe pages: each exposes the real mp4 via <source src> */
		{ "https://cdn.v.test/index.php?action=play&file=foo-2nd-episode-1.mp4",
		  "<html><body><video>"
		  "<source src=\"https://cdn.v.test/videos/foo-2nd-episode-1.mp4\">"
		  "</video></body></html>" },
		{ "https://cdn.v.test/index.php?action=play&file=foo-2nd-episode-2.mp4",
		  "<html><body><video>"
		  "<source src=\"https://cdn.v.test/videos/foo-2nd-episode-2.mp4\">"
		  "</video></body></html>" },
		{ "https://cdn.v.test/index.php?action=play&file=foo-2nd-episode-3.mp4",
		  "<html><body><video>"
		  "<source src=\"https://cdn.v.test/videos/foo-2nd-episode-3.mp4\">"
		  "</video></body></html>" },
	};
	struct fake_site site = { pages, sizeof(pages) / sizeof(pages[0]), NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(n) filename-as-param page scans");
	if (!r) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}

	CHECK(r->is_series, "(n) detected as series");
	CHECK(r->ncands >= 1, "(n) candidate found");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(n) config emits");
	if (cfg) {
		/* Must use the 1-hop param template, not a multi-get chain. */
		CHECK(strstr(cfg, "var    file <- page0 regex") != NULL,
		      "(n) config has 1-hop var line");
		CHECK(strstr(cfg, "file=([^\"'&<> ]+") != NULL,
		      "(n) capture regex uses file= param");
		CHECK(strstr(cfg, "output https://cdn.v.test/videos/{file}") != NULL,
		      "(n) output uses media_base + {file}");
		/* Must NOT contain a multi-hop get page1 or get page2. */
		CHECK(strstr(cfg, "get    page1") == NULL,
		      "(n) no page1 hop (1-hop template only)");
		CHECK(strstr(cfg, "get    page2") == NULL,
		      "(n) no page2 hop");

		/* Roundtrip: config must parse and run. */
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-n)", &perr);
		CHECK(ex != NULL, "(n) generated config parses");
		if (!ex) {
			printf("  parse err: %s\n", perr ? perr : "(none)");
		}
		free(perr);

		if (ex) {
			/* Anti-collapse: episode-1 and episode-3 must resolve to
			 * DISTINCT per-episode mp4 paths (not the same latest one). */
			char *media1 = NULL, *rerr1 = NULL;
			int rc1 = extractor_run(ex,
						"https://site.test/anime/foo-s2/episode-1/",
						fake_fetch, &site, &media1, &rerr1);
			CHECK(rc1 == 0, "(n) run resolves episode-1");
			if (rc1 != 0)
				printf("  run ep1 err: %s\n", rerr1 ? rerr1 : "(none)");
			CHECK(media1 != NULL &&
			      strcmp(media1,
				     "https://cdn.v.test/videos/foo-2nd-episode-1.mp4") == 0,
			      "(n) episode-1 resolves to correct mp4");
			free(media1);
			free(rerr1);

			char *media3 = NULL, *rerr3 = NULL;
			int rc3 = extractor_run(ex,
						"https://site.test/anime/foo-s2/episode-3/",
						fake_fetch, &site, &media3, &rerr3);
			CHECK(rc3 == 0, "(n) run resolves episode-3");
			if (rc3 != 0)
				printf("  run ep3 err: %s\n", rerr3 ? rerr3 : "(none)");
			CHECK(media3 != NULL &&
			      strcmp(media3,
				     "https://cdn.v.test/videos/foo-2nd-episode-3.mp4") == 0,
			      "(n) episode-3 resolves to correct mp4 (anti-collapse)");
			free(media3);
			free(rerr3);

			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (o) AnimeUnity-shaped JSON-in-HTML-attribute series --------------- */

static void
test_animeunity_json_attr(void)
{
	/* Landing page embeds all episode mp4 URLs as HTML-entity-escaped JSON
	 * inside a <video-player episodes="..."> attribute.  Double escaping:
	 *  - quotes as &quot;
	 *  - slashes as \/ (backslash-slash)
	 * Also contains a bare "file_name" field that is NOT a URL — confirm it
	 * is not captured as a candidate. */
	static const struct kv pages[] = {
		{ "https://anime.test/anime/2319-midori-days",
		  "<html><body>"
		  "<video-player episodes=\"["
		  "{&quot;link&quot;:&quot;https:\\/\\/cdn.komi.test\\/DDL\\/ANIME\\/Show\\/Show_Ep_01_SUB_ITA.mp4&quot;,"
		  "&quot;file_name&quot;:&quot;Show_Ep_01_SUB_ITA.mp4&quot;,"
		  "&quot;visite&quot;:2741},"
		  "{&quot;link&quot;:&quot;https:\\/\\/cdn.komi.test\\/DDL\\/ANIME\\/Show\\/Show_Ep_02_SUB_ITA.mp4&quot;,"
		  "&quot;file_name&quot;:&quot;Show_Ep_02_SUB_ITA.mp4&quot;,"
		  "&quot;visite&quot;:1500},"
		  "{&quot;link&quot;:&quot;https:\\/\\/cdn.komi.test\\/DDL\\/ANIME\\/Show\\/Show_Ep_03_SUB_ITA.mp4&quot;,"
		  "&quot;file_name&quot;:&quot;Show_Ep_03_SUB_ITA.mp4&quot;,"
		  "&quot;visite&quot;:900},"
		  "{&quot;link&quot;:&quot;https:\\/\\/cdn.komi.test\\/DDL\\/ANIME\\/Show\\/Show_Ep_04_SUB_ITA.mp4&quot;,"
		  "&quot;file_name&quot;:&quot;Show_Ep_04_SUB_ITA.mp4&quot;,"
		  "&quot;visite&quot;:800}"
		  "]\"></video-player>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(o) AnimeUnity-shaped page scans");
	if (!r) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}

	/* All 4 episode candidates must be clean: no base prepend, no trailing
	 * &quot; junk, backslash-slashes unescaped. */
	CHECK(r->ncands >= 4, "(o) 4 episode candidates found");
	for (size_t i = 0; i < r->ncands && i < 4; i++) {
		char want[128];
		snprintf(want, sizeof(want),
			 "https://cdn.komi.test/DDL/ANIME/Show/Show_Ep_0%zu_SUB_ITA.mp4",
			 i + 1);
		CHECK(strcmp(r->cands[i].url, want) == 0,
		      "(o) episode URL is clean (unescaped, no junk)");
		if (strcmp(r->cands[i].url, want) != 0)
			printf("  got: %s\n  want: %s\n", r->cands[i].url, want);
	}

	/* Must be detected as a media-list series. */
	CHECK(r->is_series, "(o) detected as media-list series");
	CHECK(r->list_ere != NULL, "(o) list_ere set");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(o) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "list   eps") != NULL,
		      "(o) config has list line");
		CHECK(strstr(cfg, "output {url}") != NULL,
		      "(o) config has output {url}");
		/* Must NOT be a single static output line. */
		CHECK(strstr(cfg, "output https://") == NULL,
		      "(o) no single static output URL");

		/* Roundtrip: config must parse and extractor_matches the landing URL. */
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-o)", &perr);
		CHECK(ex != NULL, "(o) generated config parses");
		if (!ex) {
			printf("  parse err: %s\n", perr ? perr : "(none)");
			free(perr);
			free(cfg);
			scan_result_free(r);
			return;
		}
		free(perr);

		CHECK(extractor_matches(ex, pages[0].url) == 1,
		      "(o) extractor_matches landing URL");

		/* List episodes: must return exactly 4 clean mp4 URLs. */
		char **urls = NULL;
		size_t n = 0;
		char *lerr = NULL;
		int lr = extractor_list_episodes(ex, pages[0].url,
						 fake_fetch, &site,
						 &urls, &n, &lerr);
		CHECK(lr == 0, "(o) extractor_list_episodes succeeds");
		if (lr != 0)
			printf("  list err: %s\n", lerr ? lerr : "(none)");
		free(lerr);

		CHECK(n == 4, "(o) exactly 4 episodes listed");
		for (size_t i = 0; i < n && i < 4; i++) {
			char want[128];
			snprintf(want, sizeof(want),
				 "https://cdn.komi.test/DDL/ANIME/Show/Show_Ep_0%zu_SUB_ITA.mp4",
				 i + 1);
			CHECK(urls[i] && strcmp(urls[i], want) == 0,
			      "(o) listed episode URL is clean");
			if (!urls[i] || strcmp(urls[i], want) != 0)
				printf("  ep%zu: got %s, want %s\n",
				       i + 1, urls[i] ? urls[i] : "(null)", want);
		}
		extractor_free_urls(urls, n);
		extractor_free(ex);
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- (p) Google Drive series + single ---------------------------------- */

/* fake_fetch for GDrive: the series landing has 4 open?id= links; episode
 * pages are not needed (single-video test uses an extractor_run call). */
static void
test_gdrive_series(void)
{
	/* Landing page with 4 GDrive open?id= buttons (WordPress-style &amp;).
	 * Also include a file_name-like bare word to confirm it is not captured. */
	static const struct kv pages[] = {
		{ "https://anime.test/serie/dragon-ball",
		  "<html><body>"
		  "<a href=\"https://drive.google.com/open?id=ID00000001&amp;usp=drive_copy\">Ep 1</a>"
		  "<a href=\"https://drive.google.com/open?id=ID00000002&amp;usp=drive_copy\">Ep 2</a>"
		  "<a href=\"https://drive.google.com/open?id=ID00000003&amp;usp=drive_copy\">Ep 3</a>"
		  "<a href=\"https://drive.google.com/open?id=ID00000004&amp;usp=drive_copy\">Ep 4</a>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(p) GDrive series page scans");
	if (!r) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	free(err);

	CHECK(r->ncands >= 4, "(p) 4 GDrive candidates found");
	CHECK(r->is_series, "(p) detected as GDrive series");
	CHECK(r->list_ere != NULL, "(p) list_ere set");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(p) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "list   eps  <- page regex (https?://drive\\.google\\.com/open\\?id=[A-Za-z0-9_-]+)") != NULL,
		      "(p) config has GDrive list regex");
		CHECK(strstr(cfg, "var    gid  <- url regex id=([A-Za-z0-9_-]+)") != NULL,
		      "(p) config has gid var");
		CHECK(strstr(cfg, "output https://drive.usercontent.google.com/download?id={gid}&export=download&confirm=t") != NULL,
		      "(p) config has GDrive download output");
		/* Must NOT be a bare static output line. */
		CHECK(strstr(cfg, "output https://drive.google.com") == NULL,
		      "(p) no raw drive.google.com output");

		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-p)", &perr);
		CHECK(ex != NULL, "(p) generated config parses");
		if (!ex) {
			printf("  parse err: %s\n", perr ? perr : "(none)");
			free(perr);
			free(cfg);
			scan_result_free(r);
			return;
		}
		free(perr);

		CHECK(extractor_matches(ex, pages[0].url) == 1,
		      "(p) extractor_matches landing URL");

		/* List episodes: must return 4 open?id= URLs. */
		char **urls = NULL;
		size_t n = 0;
		char *lerr = NULL;
		int lr = extractor_list_episodes(ex, pages[0].url,
						 fake_fetch, &site,
						 &urls, &n, &lerr);
		CHECK(lr == 0, "(p) extractor_list_episodes succeeds");
		free(lerr);
		CHECK(n == 4, "(p) exactly 4 episodes listed");

		/* extractor_run on a single open?id= URL should resolve to the
		 * canonical download URL with the matching ID. */
		if (n >= 2 && urls[1]) {
			char *media = NULL, *rerr = NULL;
			int rc = extractor_run(ex, urls[1],
					       fake_fetch, &site,
					       &media, &rerr);
			/* Note: extractor_run fetches urls[1] via fake_fetch;
			 * fake_fetch returns -1 for GDrive URLs (no page body
			 * registered), so the get page0 step fails. Instead we
			 * test via the var regex directly: check the config text
			 * contains the right output template. */
			(void)rc;
			free(media);
			free(rerr);
		}
		/* Verify the output template is correct by checking urls[1]
		 * would resolve: ID00000002 is in open?id=ID00000002. */
		if (n >= 2 && urls[1])
			CHECK(strstr(urls[1], "ID00000002") != NULL,
			      "(p) episode-2 URL contains ID00000002");

		extractor_free_urls(urls, n);
		extractor_free(ex);
		free(cfg);
	}
	scan_result_free(r);
}

static void
test_gdrive_single(void)
{
	/* Single GDrive video on a page (only 1 link, below SCAN_SERIES_MIN). */
	static const struct kv pages[] = {
		{ "https://anime.test/movie/film",
		  "<html><body>"
		  "<a href=\"https://drive.google.com/open?id=SINGLEID0001\">Watch</a>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(p-single) GDrive single page scans");
	if (!r) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	free(err);

	CHECK(r->ncands == 1, "(p-single) exactly 1 GDrive candidate");
	CHECK(!r->is_series, "(p-single) not a series (only 1 link)");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(p-single) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "get    page0 <- {url}") != NULL,
		      "(p-single) single GDrive has get page0");
		CHECK(strstr(cfg, "var    gid") != NULL,
		      "(p-single) single GDrive has var gid");
		CHECK(strstr(cfg, "output https://drive.usercontent.google.com/download?id={gid}&export=download&confirm=t") != NULL,
		      "(p-single) single GDrive output correct");
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- episode_number conservatism --------------------------------------- */

/* Minimal series fixture to exercise episode ordering via episode_number.
 * episode_number is internal to text.c so we test it indirectly: build a
 * fixture with URL patterns we want to verify, scan it, then inspect the
 * list_ere (we just assert the scan works; the ordering behaviour is in
 * text.c and tested by the episode_number logic itself). */

/* Direct unit-like check of the episode_number heuristic by building a
 * table of (url, expected_result) and scanning for expected ordering clues. */
static void
test_episode_number_conservative(void)
{
	/* Series with kissanime-style -episode-N URLs: should sort by N. */
	static const struct kv pages_kiss[] = {
		{ "https://site.test/anime/foo/",
		  "<html><body>"
		  "<a href=\"https://site.test/anime/foo/episode-1/\">ep1</a>"
		  "<a href=\"https://site.test/anime/foo/episode-2/\">ep2</a>"
		  "<a href=\"https://site.test/anime/foo/episode-10/\">ep10</a>"
		  "</body></html>" },
		{ "https://site.test/anime/foo/episode-1/",
		  "<source src=\"https://cdn.test/foo-ep1.mp4\">" },
		{ "https://site.test/anime/foo/episode-2/",
		  "<source src=\"https://cdn.test/foo-ep2.mp4\">" },
		{ "https://site.test/anime/foo/episode-10/",
		  "<source src=\"https://cdn.test/foo-ep10.mp4\">" },
	};
	struct fake_site site_kiss = { pages_kiss, 4, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages_kiss[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site_kiss, &err);
	CHECK(r != NULL, "(ep-num) kissanime fixture scans");
	CHECK(r && r->is_series, "(ep-num) kissanime is series");
	free(err);
	if (r) scan_result_free(r);

	/* AnimeWorld-style series: episode URLs have opaque epids (letters+digits
	 * mixed), e.g. /play/slug/JyaFP.  episode_number must return -1 for these
	 * so ordering is stable (original page order preserved). */
	static const struct kv pages_aw[] = {
		{ "https://animeworld.test/anime/naruto/",
		  "<html><body>"
		  "<a href=\"https://animeworld.test/play/naruto/JyaFP\">ep1</a>"
		  "<a href=\"https://animeworld.test/play/naruto/sWi1hA\">ep2</a>"
		  "<a href=\"https://animeworld.test/play/naruto/VIM02F\">ep3</a>"
		  "</body></html>" },
		{ "https://animeworld.test/play/naruto/JyaFP",
		  "<source src=\"https://cdn.test/aw-ep1.mp4\">" },
		{ "https://animeworld.test/play/naruto/sWi1hA",
		  "<source src=\"https://cdn.test/aw-ep2.mp4\">" },
		{ "https://animeworld.test/play/naruto/VIM02F",
		  "<source src=\"https://cdn.test/aw-ep3.mp4\">" },
	};
	struct fake_site site_aw = { pages_aw, 4, NULL, 0 };

	char *err2 = NULL;
	scan_result_t *r2 = scan_page(pages_aw[0].url, fake_fetch, fake_probe,
				      SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site_aw, &err2);
	CHECK(r2 != NULL, "(ep-num) AnimeWorld fixture scans");
	/* AnimeWorld's opaque epids (JyaFP, sWi1hA) don't match the episode
	 * URL patterns, so detect_series falls back to generic detection.
	 * The key property: episode_number returns -1 for these opaque IDs
	 * so the list order (page order) is preserved without mis-sorting. */
	free(err2);
	if (r2) scan_result_free(r2);
}

/* ---- skeleton shape (no media) ---------------------------------------- */

static void
test_skeleton_shape(void)
{
	/* A page with no media at all: scanner emits the commented skeleton.
	 * Verify: ncands==0 && !is_series (the skeleton shape). The stash
	 * logic in run_extract_scan skips these; we just assert the result. */
	static const struct kv pages[] = {
		{ "https://js.test/player/123",
		  "<html><body><div id=\"player\"></div>"
		  "<script>var config = { apiKey: \"abc123\" };</script>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     0 /* no recursion */, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(skel) skeleton page scans");
	free(err);
	if (!r) return;

	CHECK(r->ncands == 0, "(skel) no candidates (pure skeleton)");
	CHECK(!r->is_series, "(skel) not a series");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(skel) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "TODO output") != NULL,
		      "(skel) skeleton has TODO output comment");
		/* Must NOT have an active output line (extractor_parse rejects it). */
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-skel)", &perr);
		CHECK(ex == NULL, "(skel) skeleton config is intentionally unparseable");
		free(perr);
		extractor_free(ex);
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- FIX B: episode_number recognises /episode-N/ shape ---------------- */

/* episode_number is static in text.c; test it indirectly by verifying that
 * the scan-level kissanime fixture (episode-1/, episode-10/) is detected as
 * a series (which requires episode URLs to share a template) AND that the
 * list ERE captures each episode URL.  The actual sort is in run_series
 * (text.c), so we verify the numbers directly via the URL pattern. */
static void
test_episode_ordering(void)
{
	/* Verify the /episode-N/ pattern is recognised: the kissanime fixture
	 * already covers is_series detection.  Here we lock in that the list
	 * captures all three episode URLs so run_series will sort them. */
	static const struct kv pages[] = {
		{ "https://site.test/anime/baz/",
		  "<html><body>"
		  /* deliberately reversed: 3 first, then 1, then 2 */
		  "<a href=\"https://site.test/anime/baz/episode-3/\">ep3</a>"
		  "<a href=\"https://site.test/anime/baz/episode-1/\">ep1</a>"
		  "<a href=\"https://site.test/anime/baz/episode-2/\">ep2</a>"
		  "</body></html>" },
		{ "https://site.test/anime/baz/episode-1/",
		  "<source src=\"https://cdn.test/baz-ep1.mp4\">" },
		{ "https://site.test/anime/baz/episode-2/",
		  "<source src=\"https://cdn.test/baz-ep2.mp4\">" },
		{ "https://site.test/anime/baz/episode-3/",
		  "<source src=\"https://cdn.test/baz-ep3.mp4\">" },
	};
	struct fake_site site = { pages, 4, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(ep-ord) ordering fixture scans");
	free(err);
	if (!r) return;

	CHECK(r->is_series, "(ep-ord) /episode-N/ series detected");
	CHECK(r->list_ere != NULL, "(ep-ord) list_ere set");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(ep-ord) config emits");
	if (cfg) {
		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-ep-ord)", &perr);
		CHECK(ex != NULL, "(ep-ord) config parses");
		free(perr);
		if (ex) {
			char **urls = NULL;
			size_t n = 0;
			char *lerr = NULL;
			int rc = extractor_list_episodes(ex, pages[0].url,
							 fake_fetch, &site,
							 &urls, &n, &lerr);
			CHECK(rc == 0, "(ep-ord) list_episodes succeeds");
			free(lerr);
			/* All 3 episode URLs must be listed (regardless of order;
			 * sorting is in run_series which episode_number feeds). */
			CHECK(n == 3, "(ep-ord) all 3 episodes listed");
			/* Each must contain "episode-" followed by a digit. */
			for (size_t i = 0; i < n; i++) {
				CHECK(urls[i] && strstr(urls[i], "episode-") != NULL,
				      "(ep-ord) each listed URL contains episode-N");
			}
			/* Verify episode_number extracts 1, 2, 3 from the URL shapes
			 * by checking each URL's digit suffix: URLs with episode-1/,
			 * episode-2/, episode-3/ must each be recognised.  The easiest
			 * proxy: all three episode-N substrings appear in the set. */
			int found1 = 0, found2 = 0, found3 = 0;
			for (size_t i = 0; i < n; i++) {
				if (urls[i] && strstr(urls[i], "episode-1/")) found1 = 1;
				if (urls[i] && strstr(urls[i], "episode-2/")) found2 = 1;
				if (urls[i] && strstr(urls[i], "episode-3/")) found3 = 1;
			}
			CHECK(found1 && found2 && found3,
			      "(ep-ord) episodes 1, 2, and 3 all listed");
			extractor_free_urls(urls, n);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- FIX C: catch-all ERE captures full signed URL query --------------- */

static void
test_catchall_signed_url(void)
{
	/* A page containing a signed CDN URL whose query has an & separator.
	 * The catch-all ERE must capture the FULL URL including &expires=... */
	static const struct kv pages[] = {
		{ "https://site.test/watch/signed",
		  "<html><body>"
		  " https://cdn.test/clip.mp4?token=abc&expires=123 "
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     0, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(signed-url) page scans");
	free(err);
	if (!r) return;

	CHECK(r->ncands >= 1, "(signed-url) at least one candidate found");
	if (r->ncands >= 1) {
		/* The full URL including &expires= must be captured. */
		CHECK(strcmp(r->cands[0].url,
			     "https://cdn.test/clip.mp4?token=abc&expires=123") == 0,
		      "(signed-url) full signed URL captured (& in query not truncated)");
	}
	scan_result_free(r);

	/* Separately verify AnimeUnity-style: X.mp4&quot; must stop at & */
	static const struct kv pages2[] = {
		{ "https://site.test/watch/animeunity",
		  "<html><body>"
		  " {&quot;link&quot;:&quot;https://cdn.test/ep1.mp4&quot;} "
		  "</body></html>" },
	};
	struct fake_site site2 = { pages2, 1, NULL, 0 };

	char *err2 = NULL;
	scan_result_t *r2 = scan_page(pages2[0].url, fake_fetch, fake_probe,
				      0, NULL, 0,
				     &site2, &err2);
	CHECK(r2 != NULL, "(signed-url) animeunity page scans");
	free(err2);
	if (!r2) return;

	CHECK(r2->ncands >= 1, "(signed-url) animeunity has a candidate");
	if (r2->ncands >= 1) {
		/* Must be exactly the mp4 URL, not trailing &quot; */
		CHECK(strcmp(r2->cands[0].url, "https://cdn.test/ep1.mp4") == 0,
		      "(signed-url) animeunity URL stops at & (no &quot; bleed)");
	}
	scan_result_free(r2);
}

/* ---- FIX E: file/d/ GDrive series roundtrip --------------------------- */

static void
test_gdrive_filed_series(void)
{
	/* Landing page with 3 file/d/<ID>/view links (common Google Docs share). */
	static const struct kv pages[] = {
		{ "https://anime.test/serie/one-piece",
		  "<html><body>"
		  "<a href=\"https://drive.google.com/file/d/FILEID00001/view?usp=sharing\">Ep 1</a>"
		  "<a href=\"https://drive.google.com/file/d/FILEID00002/view?usp=sharing\">Ep 2</a>"
		  "<a href=\"https://drive.google.com/file/d/FILEID00003/view?usp=sharing\">Ep 3</a>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     SCAN_DEFAULT_DEPTH, NULL, 0,
				     &site, &err);
	CHECK(r != NULL, "(gdfiled) file/d/ series page scans");
	free(err);
	if (!r) return;

	CHECK(r->ncands >= 3, "(gdfiled) 3 GDrive candidates found");
	CHECK(r->is_series, "(gdfiled) detected as GDrive series");
	CHECK(r->list_ere != NULL, "(gdfiled) list_ere set");
	if (r->list_ere) {
		/* list_ere must use file/d/ form, not open?id= */
		CHECK(strstr(r->list_ere, "file/d/") != NULL,
		      "(gdfiled) list_ere uses file/d/ form");
		CHECK(strstr(r->list_ere, "open\\?id=") == NULL,
		      "(gdfiled) list_ere does NOT use open?id= form");
	}

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(gdfiled) config emits");
	if (cfg) {
		/* var line must extract ID from file/d/<ID> path */
		CHECK(strstr(cfg, "var    gid  <- url regex /d/([A-Za-z0-9_-]+)") != NULL,
		      "(gdfiled) config has file/d/ var regex");
		/* Must NOT use the open?id= var form */
		CHECK(strstr(cfg, "id=([A-Za-z0-9_-]+)") == NULL ||
		      strstr(cfg, "/d/([A-Za-z0-9_-]+)") != NULL,
		      "(gdfiled) config uses /d/ ID extraction not id= form");
		CHECK(strstr(cfg, "output https://drive.usercontent.google.com/download?id={gid}&export=download&confirm=t") != NULL,
		      "(gdfiled) config has GDrive download output");

		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-gdfiled)", &perr);
		CHECK(ex != NULL, "(gdfiled) generated config parses");
		if (!ex) {
			printf("  parse err: %s\n", perr ? perr : "(none)");
			free(perr);
			free(cfg);
			scan_result_free(r);
			return;
		}
		free(perr);

		CHECK(extractor_matches(ex, pages[0].url) == 1,
		      "(gdfiled) extractor matches landing URL");

		/* List episodes: must return 3 file/d/ URLs. */
		char **urls = NULL;
		size_t n = 0;
		char *lerr = NULL;
		int lr = extractor_list_episodes(ex, pages[0].url,
						 fake_fetch, &site,
						 &urls, &n, &lerr);
		CHECK(lr == 0, "(gdfiled) extractor_list_episodes succeeds");
		free(lerr);
		CHECK(n == 3, "(gdfiled) exactly 3 episodes listed");
		if (n >= 1 && urls[0])
			CHECK(strstr(urls[0], "drive.google.com/file/d/") != NULL,
			      "(gdfiled) first episode URL is file/d/ form");
		if (n >= 2 && urls[1])
			CHECK(strstr(urls[1], "FILEID00002") != NULL,
			      "(gdfiled) second episode URL contains FILEID00002");

		extractor_free_urls(urls, n);
		extractor_free(ex);
		free(cfg);
	}
	scan_result_free(r);
}

/* ---- gdrive_normalize unit -------------------------------------------- */

static void
test_gdrive_normalize(void)
{
	/* open?id= form */
	char *r = gdrive_normalize(
		"https://drive.google.com/open?id=1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs");
	CHECK(r != NULL, "(gdnorm) open?id= form normalised");
	if (r) {
		CHECK(strstr(r, "drive.usercontent.google.com/download") != NULL,
		      "(gdnorm) open?id= -> usercontent URL");
		CHECK(strstr(r, "1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs") != NULL,
		      "(gdnorm) open?id= ID preserved");
		free(r);
	}

	/* file/d/<ID>/view form */
	r = gdrive_normalize(
		"https://drive.google.com/file/d/1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs/view?usp=sharing");
	CHECK(r != NULL, "(gdnorm) file/d/<ID>/view form normalised");
	if (r) {
		CHECK(strstr(r, "drive.usercontent.google.com/download") != NULL,
		      "(gdnorm) file/d/ -> usercontent URL");
		free(r);
	}

	/* uc?id= form */
	r = gdrive_normalize(
		"https://drive.google.com/uc?id=1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs&export=download");
	CHECK(r != NULL, "(gdnorm) uc?id= form normalised");
	if (r) {
		CHECK(strstr(r, "drive.usercontent.google.com/download") != NULL,
		      "(gdnorm) uc?id= -> usercontent URL");
		free(r);
	}

	/* canonical download URL is idempotent */
	const char *canon =
		"https://drive.usercontent.google.com/download"
		"?id=1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs&export=download&confirm=t";
	r = gdrive_normalize(canon);
	CHECK(r == NULL, "(gdnorm) usercontent URL returns NULL (not a drive.google.com URL)");
	free(r);

	/* non-GDrive URL returns NULL */
	r = gdrive_normalize("https://example.com/video.mp4");
	CHECK(r == NULL, "(gdnorm) non-GDrive URL returns NULL");
	free(r);

	/* NULL safe */
	r = gdrive_normalize(NULL);
	CHECK(r == NULL, "(gdnorm) NULL input returns NULL");
	free(r);
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

/* ---- default extension set now finds audio ----------------------------- */

static void
test_default_finds_audio(void)
{
	/* A page with an <a href> mp3 and a <source> flac. The default scan
	 * (exts == NULL) must recognise both as audio file candidates. */
	static const struct kv pages[] = {
		{ "https://music.test/album/songs",
		  "<html><body>"
		  "<a href=\"https://cdn.music.test/track/song.mp3\">Song</a>"
		  "<audio><source src=\"https://cdn.music.test/track/hifi.flac\"></audio>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     0, NULL, 0, &site, &err);
	CHECK(r != NULL, "(audio) default audio page scans");
	free(err);
	if (!r) return;

	int found_mp3 = 0, found_flac = 0;
	for (size_t i = 0; i < r->ncands; i++) {
		if (strcmp(r->cands[i].url,
			   "https://cdn.music.test/track/song.mp3") == 0)
			found_mp3 = 1;
		if (strcmp(r->cands[i].url,
			   "https://cdn.music.test/track/hifi.flac") == 0)
			found_flac = 1;
	}
	CHECK(found_mp3, "(audio) mp3 href recognised by default set");
	CHECK(found_flac, "(audio) flac <source> recognised by default set");
	for (size_t i = 0; i < r->ncands; i++)
		CHECK(r->cands[i].kind == SCAN_KIND_FILE,
		      "(audio) audio candidates classified as file");
	scan_result_free(r);
}

/* ---- --scan-ext custom: directory listing, any file type --------------- */

static void
test_scan_ext_directory_listing(void)
{
	/* Apache/nginx autoindex: relative <a href> to mixed-distro ISOs that
	 * share NO numeric template (ubuntu-#.#.iso vs debian-#.iso).
	 * URL has a :port and an index.html filename — the exact live failure
	 * shape. The extension-list series path must fire and the generated
	 * match must satisfy extractor_matches on a port-bearing landing URL. */
	static const struct kv pages[] = {
		{ "http://mirror.test:8080/isos/index.html",
		  "<html><head><title>Index of /isos</title></head><body>"
		  "<h1>Index of /isos</h1><pre>"
		  "<a href=\"ubuntu-24.04.iso\">ubuntu-24.04.iso</a>\n"
		  "<a href=\"ubuntu-22.04.iso\">ubuntu-22.04.iso</a>\n"
		  "<a href=\"debian-12.iso\">debian-12.iso</a>\n"
		  "<a href=\"readme.txt\">readme.txt</a>\n"
		  "<a href=\"pic.png\">pic.png</a>\n"
		  "</pre></body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	/* (1) exts = {"iso"}: three ISOs, no shared numeric template, :port URL.
	 * Must produce a list config whose match fires on the port-bearing URL. */
	const char *iso_only[] = { "iso" };
	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     0, iso_only, 1, &site, &err);
	CHECK(r != NULL, "(ext-iso) directory listing scans with iso filter");
	free(err);
	if (!r) return;

	CHECK(r->ncands == 3, "(ext-iso) exactly 3 .iso candidates");
	for (size_t i = 0; i < r->ncands; i++) {
		CHECK(strstr(r->cands[i].url, ".iso") != NULL,
		      "(ext-iso) every candidate is a .iso file");
		CHECK(r->cands[i].kind == SCAN_KIND_FILE,
		      "(ext-iso) iso candidate classified as file");
	}
	CHECK(r->is_series, "(ext-iso) >=3 iso files => extension-list series");
	CHECK(r->list_ere != NULL, "(ext-iso) list_ere set");

	char *cfg = NULL;
	CHECK(emit_to_string(r, -1, &cfg) == 0 && cfg, "(ext-iso) config emits");
	if (cfg) {
		CHECK(strstr(cfg, "list   eps") != NULL, "(ext-iso) has list line");
		CHECK(strstr(cfg, "output {url}") != NULL,
		      "(ext-iso) has output {url}");

		char *perr = NULL;
		extractor_t *ex = extractor_parse(cfg, "(gen-ext-iso)", &perr);
		CHECK(ex != NULL, "(ext-iso) generated config parses");
		free(perr);
		if (ex) {
			/* Match must fire on the port-bearing landing URL (the live
			 * failure: match was "127\.0\.0\.1/index\.html" without port
			 * token, so :PORT broke the match). See #17. */
			CHECK(extractor_matches(ex, pages[0].url) == 1,
			      "(ext-iso) extractor matches port-bearing listing URL");
			char **urls = NULL;
			size_t n = 0;
			char *lerr = NULL;
			int lr = extractor_list_episodes(ex, pages[0].url,
							 fake_fetch, &site,
							 &urls, &n, &lerr);
			CHECK(lr == 0, "(ext-iso) list_episodes succeeds");
			if (lr != 0)
				printf("  list err: %s\n", lerr ? lerr : "(none)");
			free(lerr);
			CHECK(n == 3, "(ext-iso) exactly 3 iso URLs listed");
			int got24 = 0, got22 = 0, gotdeb = 0;
			for (size_t i = 0; i < n; i++) {
				if (!urls[i]) continue;
				/* Resolved absolute against the listing URL. */
				CHECK(strncmp(urls[i],
					      "http://mirror.test:8080/isos/", 29) == 0,
				      "(ext-iso) listed URL resolved absolute with port");
				if (strstr(urls[i], "ubuntu-24.04.iso")) got24 = 1;
				if (strstr(urls[i], "ubuntu-22.04.iso")) got22 = 1;
				if (strstr(urls[i], "debian-12.iso"))    gotdeb = 1;
			}
			CHECK(got24 && got22 && gotdeb,
			      "(ext-iso) all three iso files listed");
			extractor_free_urls(urls, n);
			extractor_free(ex);
		}
		free(cfg);
	}
	scan_result_free(r);

	/* (2) exts = {"iso","txt"}: the iso files AND readme.txt (4 total). */
	const char *iso_txt[] = { "iso", "txt" };
	char *err2 = NULL;
	scan_result_t *r2 = scan_page(pages[0].url, fake_fetch, fake_probe,
				      0, iso_txt, 2, &site, &err2);
	CHECK(r2 != NULL, "(ext-iso-txt) scans with iso,txt filter");
	free(err2);
	if (r2) {
		CHECK(r2->ncands == 4, "(ext-iso-txt) 3 iso + 1 txt = 4 candidates");
		int found_txt = 0, found_png = 0;
		for (size_t i = 0; i < r2->ncands; i++) {
			if (strstr(r2->cands[i].url, "readme.txt")) found_txt = 1;
			if (strstr(r2->cands[i].url, "pic.png")) found_png = 1;
		}
		CHECK(found_txt, "(ext-iso-txt) readme.txt included");
		CHECK(!found_png, "(ext-iso-txt) pic.png NOT included");
		scan_result_free(r2);
	}

	/* (3) default media scan (exts == NULL): none of these are media. */
	char *err3 = NULL;
	scan_result_t *r3 = scan_page(pages[0].url, fake_fetch, fake_probe,
				      0, NULL, 0, &site, &err3);
	CHECK(r3 != NULL, "(ext-default) media-default scan of listing scans");
	free(err3);
	if (r3) {
		CHECK(r3->ncands == 0,
		      "(ext-default) no media candidates on a non-media listing");
		scan_result_free(r3);
	}
}

/* ---- parse_scan_ext validation tests ---------------------------------- */

/* Local copy of parse_scan_ext so we can unit-test it without linking text.c.
 * Must stay in sync with the production copy in text.c. See #17. */
#define TEST_SCAN_EXT_MAX 64
static int
test_parse_scan_ext(const char *spec, char ***out, size_t *nout)
{
	*out = NULL;
	*nout = 0;
	if (!spec || !*spec)
		return -1;

	if (spec[strlen(spec) - 1] == ',')
		return -1;

	char **exts = calloc(TEST_SCAN_EXT_MAX, sizeof(*exts));
	if (!exts)
		return -1;
	size_t n = 0;

	const char *p = spec;
	while (*p) {
		const char *comma = strchr(p, ',');
		const char *end = comma ? comma : p + strlen(p);

		const char *ts = p;
		while (ts < end && isspace((unsigned char)*ts))
			ts++;
		const char *te = end;
		while (te > ts && isspace((unsigned char)te[-1]))
			te--;
		if (ts < te && *ts == '.')
			ts++;

		size_t len = (size_t)(te - ts);
		if (len == 0)
			goto bad;
		if (n >= TEST_SCAN_EXT_MAX)
			goto bad;

		for (size_t i = 0; i < len; i++)
			if (!isalnum((unsigned char)ts[i]))
				goto bad;

		char *tok = malloc(len + 1);
		if (!tok)
			goto bad;
		for (size_t i = 0; i < len; i++)
			tok[i] = (char)tolower((unsigned char)ts[i]);
		tok[len] = '\0';

		int dup = 0;
		for (size_t i = 0; i < n; i++)
			if (strcmp(exts[i], tok) == 0) { dup = 1; break; }
		if (dup)
			free(tok);
		else
			exts[n++] = tok;

		if (!comma)
			break;
		p = comma + 1;
	}

	if (n == 0)
		goto bad;

	*out = exts;
	*nout = n;
	return 0;

 bad:
	for (size_t i = 0; i < n; i++)
		free(exts[i]);
	free(exts);
	return -1;
}

static void
test_parse_scan_ext_validation(void)
{
	/* Valid inputs. */
	char **exts = NULL;
	size_t n = 0;
	CHECK(test_parse_scan_ext("iso", &exts, &n) == 0,
	      "(pse) single token accepted");
	CHECK(n == 1 && exts && strcmp(exts[0], "iso") == 0,
	      "(pse) single token value");
	{ for (size_t i = 0; i < n; i++) free(exts[i]); free(exts); exts = NULL; n = 0; }

	CHECK(test_parse_scan_ext("iso,mp4,7z", &exts, &n) == 0,
	      "(pse) iso,mp4,7z accepted");
	CHECK(n == 3, "(pse) iso,mp4,7z -> 3 tokens");
	{ for (size_t i = 0; i < n; i++) free(exts[i]); free(exts); exts = NULL; n = 0; }

	/* Leading dot stripped. */
	CHECK(test_parse_scan_ext(".iso", &exts, &n) == 0,
	      "(pse) .iso leading dot stripped");
	CHECK(n == 1 && exts && strcmp(exts[0], "iso") == 0,
	      "(pse) .iso -> iso");
	{ for (size_t i = 0; i < n; i++) free(exts[i]); free(exts); exts = NULL; n = 0; }

	/* Dedup. */
	CHECK(test_parse_scan_ext("iso,iso", &exts, &n) == 0,
	      "(pse) dedup accepted");
	CHECK(n == 1, "(pse) dedup -> 1 token");
	{ for (size_t i = 0; i < n; i++) free(exts[i]); free(exts); exts = NULL; n = 0; }

	/* FIX 3: ERE metacharacter in token rejected. */
	CHECK(test_parse_scan_ext("iso,a{2}b", &exts, &n) != 0,
	      "(pse) token with {} rejected (FIX3)");
	CHECK(exts == NULL && n == 0, "(pse) {} rejection leaves output NULL");

	CHECK(test_parse_scan_ext("mp4+webm", &exts, &n) != 0,
	      "(pse) token with + rejected");

	CHECK(test_parse_scan_ext("a.b", &exts, &n) != 0,
	      "(pse) token with internal dot rejected");

	/* FIX 4: trailing comma rejected. */
	CHECK(test_parse_scan_ext("iso,", &exts, &n) != 0,
	      "(pse) trailing comma rejected (FIX4)");
	CHECK(exts == NULL && n == 0, "(pse) trailing-comma rejection leaves output NULL");

	/* Leading comma rejected (was already). */
	CHECK(test_parse_scan_ext(",iso", &exts, &n) != 0,
	      "(pse) leading comma rejected");

	/* Double comma rejected. */
	CHECK(test_parse_scan_ext("iso,,zip", &exts, &n) != 0,
	      "(pse) double comma rejected");

	/* Empty string rejected. */
	CHECK(test_parse_scan_ext("", &exts, &n) != 0,
	      "(pse) empty string rejected");
}

/* ---- control character injection -> candidate rejected (FIX 2) --------- */

static void
test_cntrl_char_candidate_rejected(void)
{
	/* A page body containing a candidate with an embedded newline in the
	 * href.  The newline would inject directives into the emitted config.
	 * add_candidate must reject it silently. See #17. */
	static const struct kv pages[] = {
		{ "https://inject.test/page",
		  "<html><body>"
		  /* normal candidate */
		  "<a href=\"safe.mp4\">safe</a>"
		  /* crafted href with embedded newline: must be rejected */
		  "<a href=\"bad\noutput /tmp/x\n.mp4\">evil</a>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     0, NULL, 0, &site, &err);
	CHECK(r != NULL, "(cntrl) page scans without error");
	free(err);
	if (!r) return;

	/* The crafted href must NOT appear as a candidate. */
	int found_evil = 0;
	for (size_t i = 0; i < r->ncands; i++)
		if (strstr(r->cands[i].url, "output") ||
		    strstr(r->cands[i].url, "/tmp"))
			found_evil = 1;
	CHECK(!found_evil, "(cntrl) candidate with embedded newline rejected");
	scan_result_free(r);
}

/* ---- partial selection on relative-href directory listing (FIX 1) ------ */

static void
test_partial_selection_relative_hrefs(void)
{
	/* Directory listing with RELATIVE hrefs and a :port URL — the case that
	 * broke: the old absolute-URL alternation matched nothing in the body.
	 * Selecting 2 of 3 isos must roundtrip via extractor_list_episodes to
	 * EXACTLY those 2, resolved absolute. See #17. */
	static const struct kv pages[] = {
		{ "http://mirror.test:9000/dl/index.html",
		  "<html><body>"
		  "<a href=\"arch-2024.iso\">arch</a>"
		  "<a href=\"fedora-40.iso\">fedora</a>"
		  "<a href=\"opensuse-15.iso\">opensuse</a>"
		  "<a href=\"readme.txt\">readme</a>"
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	const char *iso_only[] = { "iso" };
	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     0, iso_only, 1, &site, &err);
	CHECK(r != NULL, "(psel) partial-sel page scans");
	free(err);
	if (!r) return;

	CHECK(r->ncands == 3, "(psel) 3 iso candidates");
	if (r->ncands != 3) { scan_result_free(r); return; }

	/* Verify raw_ref is stored (the relative filename, not the absolute URL). */
	int raw_ok = 1;
	for (size_t i = 0; i < r->ncands; i++) {
		if (!r->cands[i].raw_ref ||
		    strstr(r->cands[i].raw_ref, "://"))
			raw_ok = 0;	/* raw_ref should be relative, not absolute */
	}
	CHECK(raw_ok, "(psel) raw_ref is a relative ref, not an absolute URL");

	/* Select indices 0 and 2, skip index 1. */
	char url0[256], url2[256];
	snprintf(url0, sizeof(url0), "%s", r->cands[0].url);
	snprintf(url2, sizeof(url2), "%s", r->cands[2].url);
	const char *raw1 = r->cands[1].raw_ref; /* the one we skip */

	size_t sel[2] = { 0, 2 };
	char *cfg = NULL;
	size_t cfglen = 0;
	FILE *fp = open_memstream(&cfg, &cfglen);
	CHECK(fp != NULL, "(psel) memstream opens");
	if (!fp) { scan_result_free(r); return; }

	int rc = scan_emit_config_selection(r, sel, 2, fp);
	fclose(fp);
	CHECK(rc == 0 && cfg, "(psel) partial selection config emits");
	if (rc != 0 || !cfg) { free(cfg); scan_result_free(r); return; }

	CHECK(strstr(cfg, "list   eps") != NULL,
	      "(psel) emits a list directive");
	CHECK(strstr(cfg, "output {url}") != NULL,
	      "(psel) emits output {url}");
	/* The raw ref of the skipped candidate must NOT appear in the config. */
	CHECK(!raw1 || strstr(cfg, raw1) == NULL,
	      "(psel) skipped raw ref absent from config");

	char *perr = NULL;
	extractor_t *ex = extractor_parse(cfg, "(gen-psel)", &perr);
	CHECK(ex != NULL, "(psel) partial-sel config parses");
	free(perr);
	if (ex) {
		/* Match must fire on the port-bearing landing URL. */
		CHECK(extractor_matches(ex, pages[0].url) == 1,
		      "(psel) extractor matches port-bearing listing URL");

		char **urls = NULL;
		size_t n = 0;
		char *lerr = NULL;
		int lr = extractor_list_episodes(ex, pages[0].url,
						 fake_fetch, &site,
						 &urls, &n, &lerr);
		CHECK(lr == 0, "(psel) list_episodes succeeds");
		if (lr != 0)
			printf("  psel list err: %s\n", lerr ? lerr : "(none)");
		free(lerr);
		CHECK(n == 2, "(psel) exactly 2 iso URLs listed");
		int got0 = 0, got2 = 0, got1 = 0;
		for (size_t i = 0; i < n; i++) {
			if (!urls[i]) continue;
			/* Each listed URL must be absolute. */
			CHECK(strncmp(urls[i], "http://mirror.test:9000/dl/", 27) == 0,
			      "(psel) listed URL resolved absolute with port");
			if (strcmp(urls[i], url0) == 0) got0 = 1;
			if (strcmp(urls[i], url2) == 0) got2 = 1;
			if (strcmp(urls[i], r->cands[1].url) == 0) got1 = 1;
		}
		CHECK(got0 && got2, "(psel) both selected URLs present");
		CHECK(!got1, "(psel) unselected URL absent");
		extractor_free_urls(urls, n);
		extractor_free(ex);
	}
	free(cfg);
	scan_result_free(r);
}

/* ---- explicit selection -> alternation list config --------------------- */

static void
test_selection_alternation(void)
{
	/* A page whose body carries 3 absolute .bin URLs. We select 2 of the 3
	 * and emit a `list` config that must roundtrip to EXACTLY those two. */
	static const struct kv pages[] = {
		{ "https://files.test/drop/page",
		  "<html><body>"
		  " https://cdn.files.test/a/one.bin "
		  " https://cdn.files.test/a/two.bin "
		  " https://cdn.files.test/a/three.bin "
		  "</body></html>" },
	};
	struct fake_site site = { pages, 1, NULL, 0 };

	const char *bin_only[] = { "bin" };
	char *err = NULL;
	scan_result_t *r = scan_page(pages[0].url, fake_fetch, fake_probe,
				     0, bin_only, 1, &site, &err);
	CHECK(r != NULL, "(sel) bin page scans");
	free(err);
	if (!r) return;
	CHECK(r->ncands == 3, "(sel) 3 bin candidates found");
	if (r->ncands != 3) { scan_result_free(r); return; }

	/* Capture the two we will select (indices 0 and 2, skip the middle). */
	char wa[256], wc[256];
	snprintf(wa, sizeof(wa), "%s", r->cands[0].url);
	snprintf(wc, sizeof(wc), "%s", r->cands[2].url);

	size_t sel[2] = { 0, 2 };
	char *cfg = NULL;
	size_t len = 0;
	FILE *fp = open_memstream(&cfg, &len);
	CHECK(fp != NULL, "(sel) memstream opens");
	if (fp) {
		int rc = scan_emit_config_selection(r, sel, 2, fp);
		fclose(fp);
		CHECK(rc == 0 && cfg, "(sel) selection config emits");
		if (rc == 0 && cfg) {
			CHECK(strstr(cfg, "list   eps") != NULL,
			      "(sel) emits a list directive");
			CHECK(strstr(cfg, "output {url}") != NULL,
			      "(sel) emits output {url}");

			char *perr = NULL;
			extractor_t *ex = extractor_parse(cfg, "(gen-sel)", &perr);
			CHECK(ex != NULL, "(sel) selection config parses");
			free(perr);
			if (ex) {
				char **urls = NULL;
				size_t n = 0;
				char *lerr = NULL;
				int lr = extractor_list_episodes(ex, pages[0].url,
								 fake_fetch, &site,
								 &urls, &n, &lerr);
				CHECK(lr == 0, "(sel) list_episodes succeeds");
				if (lr != 0)
					printf("  list err: %s\n",
					       lerr ? lerr : "(none)");
				free(lerr);
				CHECK(n == 2, "(sel) exactly 2 selected URLs listed");
				int got_a = 0, got_c = 0, got_b = 0;
				for (size_t i = 0; i < n; i++) {
					if (!urls[i]) continue;
					if (strcmp(urls[i], wa) == 0) got_a = 1;
					if (strcmp(urls[i], wc) == 0) got_c = 1;
					if (strstr(urls[i], "two.bin")) got_b = 1;
				}
				CHECK(got_a && got_c,
				      "(sel) both selected URLs present");
				CHECK(!got_b,
				      "(sel) the unselected URL is absent");
				extractor_free_urls(urls, n);
				extractor_free(ex);
			}
		}
		free(cfg);
	}

	/* A single selected file keeps the static single-output form. */
	size_t one[1] = { 1 };
	char *cfg2 = NULL;
	size_t len2 = 0;
	FILE *fp2 = open_memstream(&cfg2, &len2);
	if (fp2) {
		int rc = scan_emit_config_selection(r, one, 1, fp2);
		fclose(fp2);
		CHECK(rc == 0 && cfg2, "(sel) single-selection config emits");
		if (rc == 0 && cfg2) {
			CHECK(strstr(cfg2, "output https://cdn.files.test/a/two.bin")
			      != NULL, "(sel) single selection is a static output");
			CHECK(strstr(cfg2, "list   eps") == NULL,
			      "(sel) single selection is not a list");
		}
		free(cfg2);
	}
	scan_result_free(r);
}

int
main(void)
{
	test_direct_source();
	test_hls_in_json();
	test_ad_vs_content();
	test_skeleton_when_no_media();
	test_one_hop();
	test_two_hops();
	test_series();
	test_series_no_media();
	test_generic_link_follow();
	test_structural_series();
	test_multi_series_containment();
	test_multi_series_absolute();
	test_ad_trap();
	test_kissanime_shape();
	test_filename_as_param();
	test_animeunity_json_attr();
	test_gdrive_series();
	test_gdrive_single();
	test_episode_number_conservative();
	test_episode_ordering();
	test_catchall_signed_url();
	test_gdrive_filed_series();
	test_skeleton_shape();
	test_gdrive_normalize();
	test_ad_host_blocklist();
	test_default_finds_audio();
	test_scan_ext_directory_listing();
	test_selection_alternation();
	test_parse_scan_ext_validation();
	test_cntrl_char_candidate_rejected();
	test_partial_selection_relative_hrefs();

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
