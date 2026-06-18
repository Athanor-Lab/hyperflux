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

/* 'list' is parsed but rejected at run time in F1. */
static void
test_list_not_supported(void)
{
	const char *cfg =
		"name x\n"
		"list eps <- url regex href=\"(/play/[^\"]+)\"\n"
		"output {url}\n";
	char *err = NULL;
	extractor_t *ex = extractor_parse(cfg, "x.conf", &err);
	CHECK(ex != NULL, "list config parses");
	if (!ex) { free(err); return; }

	char *media = NULL;
	int r = extractor_run(ex, "https://h/p", NULL, NULL, &media, &err);
	CHECK(r < 0, "list directive is rejected at run time");
	free(err);
	extractor_free(ex);
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
	test_list_not_supported();
	test_resolve_url();
	test_passthrough();

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
