/* Standalone unit tests for the media extractor. Build:
 *   cc -D_DEFAULT_SOURCE -Wall -Wextra -g \
 *      src/extractor.c src/test_extractor.c -o /tmp/test_extractor && /tmp/test_extractor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extractor.h"

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

#define CHECK_STR(got, want, msg)					\
	do {								\
		checks++;						\
		if (!(got) || strcmp((got), (want)) != 0) {		\
			printf("FAIL %s: got \"%s\" want \"%s\"\n",	\
			       (msg), (got) ? (got) : "(null)", (want));\
			failures++;					\
		}							\
	} while (0)

/* ---- fetch stub: a tiny in-memory site ----------------------------------
 * The test config GETs an API URL; we return a fixed fragment so the chain
 * page -> API -> .mp4 can be exercised without any network. */

struct fake_site {
	const char *expect_url;		/* if non-NULL, assert the GET target */
	const char *expect_referer;	/* if non-NULL, assert a Referer header */
	int got_referer;
	char last_url[1024];
};

static int
fake_fetch(const char *url, const ext_header_t *headers, size_t nheaders,
	   char **out_body, void *userdata)
{
	struct fake_site *s = userdata;

	*out_body = NULL;
	if (s) {
		snprintf(s->last_url, sizeof(s->last_url), "%s", url);
		for (size_t i = 0; i < nheaders; i++) {
			if (strcmp(headers[i].key, "Referer") == 0) {
				s->got_referer = 1;
				if (s->expect_referer &&
				    strcmp(headers[i].val, s->expect_referer) != 0)
					return -1;
			}
		}
		if (s->expect_url && strcmp(url, s->expect_url) != 0)
			return -1;
	}

	/* Return an API fragment carrying the media URL. */
	const char *frag =
		"{\"grabber\":\"x\",\"name\":\"...\","
		"\"target\":\"<source src=\\\"https://cdn.example.com/v/ep.mp4\\\">\"}";
	char *body = malloc(strlen(frag) + 1);
	if (!body)
		return -1;
	strcpy(body, frag);
	*out_body = body;
	return 0;
}

/* ---- config parsing ----------------------------------------------------- */

static const char *CFG_VALID =
	"name   test\n"
	"match  example\\.[a-z]+/play/\n"
	"\n"
	"# capture the episode id from the page URL\n"
	"var    epid   <- url    regex /play/[^/]+/([A-Za-z0-9]+)\n"
	"get    player <- https://api.example.com/ep?id={epid}\n"
	"       header Referer={url}\n"
	"       header X-Requested-With=XMLHttpRequest\n"
	"var    media  <- player regex <source[^>]+src=\\\\\"([^\\\\\"]+)\\\\\"\n"
	"output {media}\n";

static void
test_parse_valid(void)
{
	char *err = NULL;
	extractor_t *ex = extractor_parse(CFG_VALID, "test.conf", &err);

	CHECK(ex != NULL, "valid config parses");
	if (!ex) {
		printf("  parse err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	CHECK_STR(ex->name, "test", "name parsed");
	CHECK(ex->nmatches == 1, "one match line");
	CHECK(extractor_matches(ex, "https://example.tv/play/foo/AbC123"),
	      "match hits the play URL");
	CHECK(!extractor_matches(ex, "https://example.tv/watch/x"),
	      "match misses a non-play URL");
	/* var epid, get player(+2 headers), var media, output */
	CHECK(ex->ndirectives == 4, "four directives");
	CHECK(ex->directives[1].kind == EXT_DIR_GET, "second directive is get");
	CHECK(ex->directives[1].nheaders == 2, "get has two headers");
	CHECK_STR(ex->directives[1].headers[0].key, "Referer", "header 0 key");
	CHECK_STR(ex->directives[1].headers[0].val, "{url}", "header 0 val");
	extractor_free(ex);
}

static void
test_parse_malformed(void)
{
	struct { const char *cfg; const char *what; } cases[] = {
		{ "match foo\noutput {x}\n", "missing name" },
		{ "name x\nmatch foo\n", "missing output" },
		{ "name x\nbogus directive\noutput {x}\n", "unknown directive" },
		{ "name x\nvar a <- url\noutput {a}\n", "var missing regex kw" },
		{ "name x\nmatch (unbalanced\noutput {x}\n", "bad match regex" },
		{ "name x\n  header K=V\noutput {x}\n", "header without get" },
		{ "name x\nget g <- u\n  header noequals\noutput {x}\n",
		  "malformed header" },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char *err = NULL;
		extractor_t *ex = extractor_parse(cases[i].cfg, "bad.conf", &err);
		checks++;
		if (ex != NULL) {
			printf("FAIL malformed accepted: %s\n", cases[i].what);
			failures++;
			extractor_free(ex);
		}
		/* 'unknown directive' sits on line 2: assert the file:line prefix
		 * and the offending token reach the diagnostic. */
		if (strcmp(cases[i].what, "unknown directive") == 0)
			CHECK(err && strstr(err, "bad.conf:2:") &&
			      strstr(err, "bogus"),
			      "parse error reports file:line and directive");
		free(err);
	}
}

/* ---- regex group-1 capture + interpolation via a run ------------------- */

static void
test_run_chain(void)
{
	char *err = NULL;
	extractor_t *ex = extractor_parse(CFG_VALID, "test.conf", &err);
	if (!ex) {
		CHECK(0, "chain config parses");
		printf("  parse err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}

	struct fake_site site;
	memset(&site, 0, sizeof(site));
	site.expect_url = "https://api.example.com/ep?id=AbC123";
	site.expect_referer = "https://example.tv/play/anime-name/AbC123";

	char *media = NULL;
	int r = extractor_run(ex, site.expect_referer, fake_fetch, &site,
			      &media, &err);
	CHECK(r == 0, "chain runs to output");
	if (r != 0) {
		printf("  run err: %s\n", err ? err : "(none)");
		free(err);
	} else {
		CHECK_STR(media, "https://cdn.example.com/v/ep.mp4",
			  "group-1 capture + interpolation");
		CHECK(site.got_referer == 1, "Referer header was sent");
	}
	free(media);
	extractor_free(ex);
}

/* A var whose regex does not match its source must fail clearly. */
static void
test_run_nomatch_var(void)
{
	const char *cfg =
		"name x\n"
		"var a <- url regex zzz([0-9]+)zzz\n"
		"output {a}\n";
	char *err = NULL;
	extractor_t *ex = extractor_parse(cfg, "x.conf", &err);
	CHECK(ex != NULL, "nomatch-var config parses");
	if (!ex) { free(err); return; }

	char *media = NULL;
	int r = extractor_run(ex, "https://h/p", NULL, NULL, &media, &err);
	CHECK(r < 0, "unmatched var regex fails the run");
	CHECK(media == NULL, "no media URL on failure");
	free(err);
	extractor_free(ex);
}

/* An undefined {var} in a template must fail with a message naming it. */
static void
test_run_undefined_var(void)
{
	const char *cfg =
		"name x\n"
		"output https://h/{nope}/v.mp4\n";
	char *err = NULL;
	extractor_t *ex = extractor_parse(cfg, "x.conf", &err);
	CHECK(ex != NULL, "undefined-var config parses");
	if (!ex) { free(err); return; }

	char *media = NULL;
	int r = extractor_run(ex, "https://h/p", NULL, NULL, &media, &err);
	CHECK(r < 0, "undefined {var} fails the run");
	CHECK(media == NULL, "no media URL on undefined var");
	CHECK(err && strstr(err, "nope"), "error names the undefined variable");
	free(err);
	extractor_free(ex);
}

/* ---- series mode (F4) --------------------------------------------------- */

/* An AnimeWorld-like series config: 'list' captures every episode href, the
 * per-episode pipeline turns {url} into the episode's media URL. */
static const char *CFG_SERIES =
	"name   series\n"
	"match  example\\.[a-z]+/play/\n"
	"list   episodes <- url regex href=\"(/play/[^\"]+/[A-Za-z0-9]+)\"\n"
	"var    epid     <- url  regex /play/[^/]+/([A-Za-z0-9]+)\n"
	"output https://cdn.example.com/v/{epid}.mp4\n";

/* A saved series page listing several episodes (plus duplicates and a noise
 * link the regex must not capture). */
static const char *SERIES_HTML =
	"<!doctype html><html><body>\n"
	"<a class=\"other\" href=\"/login\">login</a>\n"
	"<div class=\"episodes\">\n"
	"  <a data-num=\"1\" href=\"/play/anime-name/Ep01\">1</a>\n"
	"  <a data-num=\"2\" href=\"/play/anime-name/Ep02\">2</a>\n"
	"  <a data-num=\"3\" href=\"/play/anime-name/Ep03\">3</a>\n"
	"  <a data-num=\"3\" href=\"/play/anime-name/Ep03\">3 (dup)</a>\n"
	"  <a data-num=\"4\" href=\"https://example.tv/play/anime-name/Ep04\">4</a>\n"
	"</div></body></html>\n";

/* A fetch stub returning a fixed body (the series HTML) handed via userdata.
 * Used so 'list <- page' can be exercised without a network. */
static int
series_fetch(const char *url, const ext_header_t *headers, size_t nheaders,
	     char **out_body, void *userdata)
{
	(void)url;
	(void)headers;
	(void)nheaders;
	const char *html = userdata;
	*out_body = NULL;
	char *body = malloc(strlen(html) + 1);
	if (!body)
		return -1;
	strcpy(body, html);
	*out_body = body;
	return 0;
}

/* 'list' captures ALL group-1 matches, resolved relative->absolute, deduped,
 * order preserved. The list source is a 'get' body so a stub feeds the HTML. */
static void
test_list_episodes(void)
{
	const char *cfg =
		"name series\n"
		"match example\\.[a-z]+/play/\n"
		"get page <- https://example.tv/series/anime-name\n"
		"list episodes <- page regex href=\"([^\"]*/play/[^\"]+/[A-Za-z0-9]+)\"\n"
		"var epid <- url regex /play/[^/]+/([A-Za-z0-9]+)\n"
		"output https://cdn.example.com/v/{epid}.mp4\n";
	char *err = NULL;
	extractor_t *ex = extractor_parse(cfg, "series.conf", &err);
	CHECK(ex != NULL, "series config parses");
	if (!ex) {
		printf("  parse err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	CHECK(extractor_has_list(ex) == 1, "series config reports a list");

	char **urls = NULL;
	size_t n = 0;
	int r = extractor_list_episodes(ex,
		"https://example.tv/series/anime-name",
		series_fetch, (void *)SERIES_HTML, &urls, &n, &err);
	CHECK(r == 0, "list_episodes succeeds");
	if (r != 0) {
		printf("  list err: %s\n", err ? err : "(none)");
		free(err);
		extractor_free(ex);
		return;
	}
	CHECK(n == 4, "list captured 4 unique episodes (dup dropped)");
	if (n == 4) {
		CHECK_STR(urls[0], "https://example.tv/play/anime-name/Ep01",
			  "episode 1 resolved absolute");
		CHECK_STR(urls[1], "https://example.tv/play/anime-name/Ep02",
			  "episode 2 resolved absolute");
		CHECK_STR(urls[2], "https://example.tv/play/anime-name/Ep03",
			  "episode 3 (first of dup) preserved");
		CHECK_STR(urls[3], "https://example.tv/play/anime-name/Ep04",
			  "episode 4 absolute href passes through");
	}
	extractor_free_urls(urls, n);
	extractor_free(ex);
}

/* {var} in a var/list regex is interpolated against the store before regcomp,
 * so a slug captured from the URL can anchor the list to one series. The fixture
 * mixes this series' /play/ links with another series' links the slug excludes,
 * and uses real data-num attributes plus an epid containing '-' and '_'. */
static const char *SERIES_HTML_SLUG =
	"<!doctype html><html><body>\n"
	"<a data-num=\"1\" href=\"/play/anime-name.4242/Ep01\">1</a>\n"
	"<a data-num=\"2\" href=\"/play/anime-name.4242/VbGT-F\">2</a>\n"
	"<a data-num=\"3\" href=\"/play/anime-name.4242/a_b-C9\">3</a>\n"
	"<a data-num=\"1\" href=\"/play/other-show.9999/Zz01\">other 1</a>\n"
	"</body></html>\n";

/* The slug-anchored list: capture {slug} from {url}, fetch the page, then list
 * only this series' /play/{slug}/<epid> hrefs. epids may carry '-'/'_'. */
static void
test_list_episodes_slug_interp(void)
{
	const char *cfg =
		"name series\n"
		"match example\\.[a-z]+/play/\n"
		"var  slug <- url  regex /play/([^/]+)/\n"
		"get  page <- {url}\n"
		"list eps  <- page regex href=\"(/play/{slug}/[A-Za-z0-9_-]+)\"\n"
		"var  epid <- url  regex /play/[^/]+/([A-Za-z0-9_-]+)\n"
		"output https://cdn.example.com/v/{epid}.mp4\n";
	char *err = NULL;
	extractor_t *ex = extractor_parse(cfg, "series.conf", &err);
	CHECK(ex != NULL, "slug-interp series config parses");
	if (!ex) {
		printf("  parse err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}

	char **urls = NULL;
	size_t n = 0;
	int r = extractor_list_episodes(ex,
		"https://example.tv/play/anime-name.4242/Ep01",
		series_fetch, (void *)SERIES_HTML_SLUG, &urls, &n, &err);
	CHECK(r == 0, "slug-anchored list_episodes succeeds");
	if (r != 0) {
		printf("  list err: %s\n", err ? err : "(none)");
		free(err);
		extractor_free(ex);
		return;
	}
	CHECK(n == 3, "slug list captured exactly this series' 3 episodes");
	if (n == 3) {
		CHECK_STR(urls[0],
			  "https://example.tv/play/anime-name.4242/Ep01",
			  "slug ep1 absolute");
		CHECK_STR(urls[1],
			  "https://example.tv/play/anime-name.4242/VbGT-F",
			  "slug ep2 keeps a '-' in the epid");
		CHECK_STR(urls[2],
			  "https://example.tv/play/anime-name.4242/a_b-C9",
			  "slug ep3 keeps '_' and '-' in the epid");
	}
	extractor_free_urls(urls, n);
	extractor_free(ex);
}

/* An interpolated {var} in a regex that is undefined must fail clearly. */
static void
test_regex_interp_undefined_var(void)
{
	const char *cfg =
		"name x\n"
		"var a <- url regex /play/({missing})\n"
		"output {a}\n";
	char *err = NULL;
	extractor_t *ex = extractor_parse(cfg, "x.conf", &err);
	CHECK(ex != NULL, "regex-undefined-var config parses");
	if (!ex) { free(err); return; }

	char *media = NULL;
	int r = extractor_run(ex, "https://h/play/foo", NULL, NULL, &media, &err);
	CHECK(r < 0, "undefined {var} in a regex fails the run");
	CHECK(media == NULL, "no media URL on undefined regex var");
	CHECK(err && strstr(err, "missing"),
	      "error names the undefined regex variable");
	free(err);
	extractor_free(ex);
}

/* A per-episode run must SKIP the 'list' directive and resolve {url} to media. */
static void
test_per_episode_run_skips_list(void)
{
	char *err = NULL;
	extractor_t *ex = extractor_parse(CFG_SERIES, "series.conf", &err);
	CHECK(ex != NULL, "series config parses for per-episode run");
	if (!ex) { free(err); return; }

	char *media = NULL;
	int r = extractor_run(ex, "https://example.tv/play/anime-name/Ep02",
			      NULL, NULL, &media, &err);
	CHECK(r == 0, "per-episode run skips list, reaches output");
	if (r != 0) {
		printf("  run err: %s\n", err ? err : "(none)");
		free(err);
	} else {
		CHECK_STR(media, "https://cdn.example.com/v/Ep02.mp4",
			  "per-episode media URL from {url}");
	}
	free(media);
	extractor_free(ex);
}

/* Pre-'list' var/get are series setup: run once during listing, NOT re-run
 * per episode. This config has a pre-'list' `var seriesid <- url` whose regex
 * only matches the series URL; if the per-episode run re-ran it against an
 * episode URL it would fail. The per-episode pipeline must skip it. */
static void
test_pre_list_setup_not_rerun(void)
{
	const char *cfg =
		"name series\n"
		"match example\\.[a-z]+/(series|play)/\n"
		"var seriesid <- url regex /series/([a-z-]+)\n"
		"get page <- https://example.tv/series/{seriesid}\n"
		"list episodes <- page regex href=\"([^\"]*/play/[^\"]+/[A-Za-z0-9]+)\"\n"
		"var epid <- url regex /play/[^/]+/([A-Za-z0-9]+)\n"
		"output https://cdn.example.com/v/{epid}.mp4\n";
	char *err = NULL;
	extractor_t *ex = extractor_parse(cfg, "series.conf", &err);
	CHECK(ex != NULL, "pre-list-setup config parses");
	if (!ex) {
		printf("  parse err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}

	/* Listing uses the series URL, so the pre-'list' var matches. */
	char **urls = NULL;
	size_t n = 0;
	int r = extractor_list_episodes(ex,
		"https://example.tv/series/anime-name",
		series_fetch, (void *)SERIES_HTML, &urls, &n, &err);
	CHECK(r == 0, "pre-list var matches on series URL during listing");
	if (r != 0) {
		printf("  list err: %s\n", err ? err : "(none)");
		free(err);
		err = NULL;
	}
	extractor_free_urls(urls, n);

	/* Per-episode run with an episode URL (no '/series/' segment): the
	 * pre-'list' var must be skipped, so the run still reaches output. */
	char *media = NULL;
	r = extractor_run(ex, "https://example.tv/play/anime-name/Ep02",
			  NULL, NULL, &media, &err);
	CHECK(r == 0, "per-episode run skips pre-list series setup");
	if (r != 0) {
		printf("  run err: %s\n", err ? err : "(none)");
		free(err);
	} else {
		CHECK_STR(media, "https://cdn.example.com/v/Ep02.mp4",
			  "per-episode output unaffected by pre-list var");
	}
	free(media);
	extractor_free(ex);
}

/* ---- --episodes spec parsing -------------------------------------------- */

static void
test_episodes_spec(void)
{
	unsigned char *sel = NULL;
	size_t nsel = 0;
	char eb[128];

	/* "1,3-5" over 8 episodes selects {1,3,4,5}. */
	int r = extractor_parse_episodes("1,3-5", 8, &sel, &nsel, eb, sizeof(eb));
	CHECK(r == 0, "spec '1,3-5' parses");
	if (r == 0) {
		CHECK(nsel == 4, "'1,3-5' selects 4 episodes");
		unsigned char want[8] = {1,0,1,1,1,0,0,0};
		int eq = 1;
		for (size_t i = 0; i < 8; i++)
			if (sel[i] != want[i]) eq = 0;
		CHECK(eq, "'1,3-5' selects exactly {1,3,4,5}");
	}
	free(sel);

	/* "1,3-5,8" over 8 -> {1,3,4,5,8}. */
	sel = NULL; nsel = 0;
	r = extractor_parse_episodes("1,3-5,8", 8, &sel, &nsel, eb, sizeof(eb));
	CHECK(r == 0 && nsel == 5, "spec '1,3-5,8' selects 5");
	if (r == 0) {
		CHECK(sel[0] && sel[7] && !sel[1], "endpoints 1 and 8 selected");
	}
	free(sel);

	/* whitespace tolerated. */
	sel = NULL; nsel = 0;
	r = extractor_parse_episodes(" 2 , 4 ", 5, &sel, &nsel, eb, sizeof(eb));
	CHECK(r == 0 && nsel == 2, "spec ' 2 , 4 ' parses with spaces");
	free(sel);

	/* Bad specs are rejected (out of range, reversed, junk, empty). */
	const char *bad[] = { "0", "9", "5-3", "1,,2", "abc", "1-", "-2",
			      "1-9", "", "3-" };
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		sel = NULL; nsel = 0;
		r = extractor_parse_episodes(bad[i], 8, &sel, &nsel, eb,
					     sizeof(eb));
		CHECK(r < 0 && sel == NULL,
		      "bad --episodes spec rejected");
		free(sel);
	}

	/* NULL spec and zero count are rejected without crashing. */
	r = extractor_parse_episodes(NULL, 8, &sel, &nsel, eb, sizeof(eb));
	CHECK(r < 0, "NULL spec rejected");
	r = extractor_parse_episodes("1", 0, &sel, &nsel, eb, sizeof(eb));
	CHECK(r < 0, "zero count rejected");
}

/* ---- relative -> absolute URL resolution -------------------------------- */

static void
check_resolve(const char *base, const char *ref, const char *want)
{
	char *got = extractor_resolve_url(base, ref);
	CHECK_STR(got, want, "resolve_url");
	free(got);
}

static void
test_resolve_url(void)
{
	check_resolve("https://h.com/a/b/page.html", "/v/ep.mp4",
		      "https://h.com/v/ep.mp4");
	check_resolve("https://h.com/a/b/page.html", "ep.mp4",
		      "https://h.com/a/b/ep.mp4");
	check_resolve("https://h.com/a/b/", "ep.mp4",
		      "https://h.com/a/b/ep.mp4");
	check_resolve("https://h.com", "ep.mp4",
		      "https://h.com/ep.mp4");
	check_resolve("https://h.com/a/b/page", "https://cdn.x/y.mp4",
		      "https://cdn.x/y.mp4");
	check_resolve("https://h.com/a/page", "//cdn.x/y.mp4",
		      "https://cdn.x/y.mp4");
	check_resolve("https://h.com/a/b/page?x=1", "ep.mp4",
		      "https://h.com/a/b/ep.mp4");
	check_resolve("https://h.com/play/foo/AbC123", "/api/ep?id=1",
		      "https://h.com/api/ep?id=1");
	/* JSON-embedded media: backslash-slash sequences must be unescaped. */
	check_resolve("https://base.test/x/",
		      "https:\\/\\/host.test\\/a\\/b.mp4",
		      "https://host.test/a/b.mp4");
}

/* ---- no-match passthrough (resolve with a missing config dir) ----------- */

static void
test_passthrough(void)
{
	/* Point discovery at an empty/nonexistent dir so nothing matches. */
	setenv("XDG_CONFIG_HOME", "/nonexistent-flux-test-dir-xyz", 1);
	char *out = NULL;
	int r = extractor_resolve("https://nowhere.invalid/file.bin", NULL,
				  NULL, NULL, &out);
	CHECK(r == 0, "no-match resolve returns 0");
	CHECK_STR(out, "https://nowhere.invalid/file.bin",
		  "passthrough copies the page URL");
	free(out);
}

int
main(void)
{
	test_parse_valid();
	test_parse_malformed();
	test_run_chain();
	test_run_nomatch_var();
	test_run_undefined_var();
	test_list_episodes();
	test_list_episodes_slug_interp();
	test_regex_interp_undefined_var();
	test_per_episode_run_skips_list();
	test_pre_list_setup_not_rerun();
	test_episodes_spec();
	test_resolve_url();
	test_passthrough();

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
