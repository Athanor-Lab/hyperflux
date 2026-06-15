/* Standalone unit tests for url_glob. Build:
 *   cc -D_DEFAULT_SOURCE -Wall -Wextra -fsanitize=address,undefined \
 *      src/url_glob.c src/test_url_glob.c -o /tmp/test_url_glob && /tmp/test_url_glob
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "url_glob.h"

static int failures;
static int checks;

/* Expand `src` and assert the result equals the NULL-terminated `expect`
 * list, in order. */
static void
expect_list(const char *src, const char *const *expect)
{
	url_glob_t *items = NULL;
	size_t count = 0, ncaps = 0;
	size_t want = 0;

	while (expect[want])
		want++;

	checks++;
	int rc = url_glob(src, 1024, &items, &count, &ncaps);
	if (rc < 0) {
		printf("FAIL %-32s -> error (expected %zu URLs)\n", src, want);
		failures++;
		return;
	}
	if (count != want) {
		printf("FAIL %-32s -> %zu URLs (expected %zu)\n", src, count,
		       want);
		failures++;
		url_glob_free(items, count, ncaps);
		return;
	}
	for (size_t i = 0; i < count; i++) {
		if (strcmp(items[i].url, expect[i]) != 0) {
			printf("FAIL %-32s -> [%zu]=\"%s\" (expected \"%s\")\n",
			       src, i, items[i].url, expect[i]);
			failures++;
			url_glob_free(items, count, ncaps);
			return;
		}
	}
	url_glob_free(items, count, ncaps);
}

/* Assert that the expansion is rejected (returns -1). */
static void
expect_error(const char *src, size_t max_len)
{
	url_glob_t *items = NULL;
	size_t count = 0, ncaps = 0;

	checks++;
	int rc = url_glob(src, max_len, &items, &count, &ncaps);
	if (rc >= 0) {
		printf("FAIL %-32s -> %zu URLs (expected error)\n", src, count);
		failures++;
		url_glob_free(items, count, ncaps);
	}
}

/* Assert capture count and one capture value. */
static void
expect_caps(const char *src, size_t want_ncaps, size_t item, size_t cap,
	    const char *value)
{
	url_glob_t *items = NULL;
	size_t count = 0, ncaps = 0;

	checks++;
	int rc = url_glob(src, 1024, &items, &count, &ncaps);
	if (rc < 0) {
		printf("FAIL caps %-27s -> error\n", src);
		failures++;
		return;
	}
	if (ncaps != want_ncaps) {
		printf("FAIL caps %-27s -> ncaps %zu (expected %zu)\n", src,
		       ncaps, want_ncaps);
		failures++;
	} else if (item < count && ncaps &&
		   strcmp(items[item].caps[cap], value) != 0) {
		printf("FAIL caps %-27s -> [%zu].cap[%zu]=\"%s\" (expected \"%s\")\n",
		       src, item, cap, items[item].caps[cap], value);
		failures++;
	}
	url_glob_free(items, count, ncaps);
}

#define L(...) ((const char *const[]){ __VA_ARGS__, NULL })

int
main(void)
{
	/* No pattern: a single passthrough item. */
	expect_list("http://h/f.iso", L("http://h/f.iso"));
	expect_list("", L(""));

	/* Bash-style numeric range with zero padding (the headline case). */
	expect_list("v{01..12}.mp4",
		    L("v01.mp4", "v02.mp4", "v03.mp4", "v04.mp4", "v05.mp4",
		      "v06.mp4", "v07.mp4", "v08.mp4", "v09.mp4", "v10.mp4",
		      "v11.mp4", "v12.mp4"));
	expect_list("f{1..3}", L("f1", "f2", "f3"));
	expect_list("f{001..3}", L("f001", "f002", "f003"));

	/* Step and descending (bash allows descending). */
	expect_list("n{0..6..2}", L("n0", "n2", "n4", "n6"));
	expect_list("n{3..1}", L("n3", "n2", "n1"));
	expect_list("n{10..01}",
		    L("n10", "n09", "n08", "n07", "n06", "n05", "n04", "n03",
		      "n02", "n01"));

	/* curl-style bracket ranges. */
	expect_list("img[1-3].jpg", L("img1.jpg", "img2.jpg", "img3.jpg"));
	expect_list("img[01-03].jpg",
		    L("img01.jpg", "img02.jpg", "img03.jpg"));
	expect_list("p[1-10:3]", L("p1", "p4", "p7", "p10"));
	/* Bracket ranges are ascending only; a descending one stays literal. */
	expect_list("x[3-1]", L("x[3-1]"));

	/* Alphabetic ranges. */
	expect_list("[a-e]", L("a", "b", "c", "d", "e"));
	expect_list("{a..c}", L("a", "b", "c"));
	expect_list("[a-f:2]", L("a", "c", "e"));

	/* Lists. */
	expect_list("pic.{jpg,png,gif}",
		    L("pic.jpg", "pic.png", "pic.gif"));
	expect_list("a{,x}b", L("ab", "axb"));

	/* Multiple patterns: cartesian product, rightmost varies fastest. */
	expect_list("[1-2]/[1-2]", L("1/1", "1/2", "2/1", "2/2"));
	expect_list("h{a,b}[1-2]",
		    L("ha1", "ha2", "hb1", "hb2"));

	/* Things that must NOT be touched. */
	expect_list("http://[::1]/f", L("http://[::1]/f"));
	expect_list("http://[::1]:8080/f", L("http://[::1]:8080/f"));
	expect_list("q?a[b]=c", L("q?a[b]=c"));
	expect_list("lone{brace", L("lone{brace"));
	expect_list("plain[abc]", L("plain[abc]"));
	expect_list("{single}", L("{single}"));
	expect_list("a[1-2-3]b", L("a[1-2-3]b"));
	expect_list("s[1-3:0]", L("s[1-3:0]"));	/* step 0 -> literal */

	/* Captures back #N substitution. */
	expect_caps("[5-9]/v[01-02].bin", 2, 0, 0, "5");
	expect_caps("[5-9]/v[01-02].bin", 2, 0, 1, "01");
	expect_caps("[5-9]/v[01-02].bin", 2, 3, 1, "02");
	expect_caps("no-pattern", 0, 0, 0, "");

	/* Bounds: over the cap is rejected, at the cap is accepted. */
	expect_error("[1-100000]", 1024);
	expect_error("[1-100][1-101]", 1024);	/* product 10100 > 10000 */
	{
		url_glob_t *items = NULL;
		size_t count = 0, ncaps = 0;
		checks++;
		int rc = url_glob("[1-10000]", 1024, &items, &count, &ncaps);
		if (rc != 10000 || count != 10000) {
			printf("FAIL cap boundary -> rc=%d count=%zu\n", rc,
			       count);
			failures++;
		}
		url_glob_free(items, count, ncaps);
	}

	/* An expanded URL that would not fit max_len is rejected. */
	expect_error("aaaaaaaa{1,2}", 8);

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
