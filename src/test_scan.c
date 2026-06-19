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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
				     SCAN_DEFAULT_DEPTH, &site, &err);
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
	test_one_hop();
	test_two_hops();
	test_series();
	test_series_no_media();
	test_generic_link_follow();
	test_structural_series();
	test_multi_series_containment();
	test_multi_series_absolute();
	test_ad_trap();
	test_ad_host_blocklist();

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
