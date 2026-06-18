/* Standalone unit tests for the TUI's pure window/row-count math. The
 * interactive prompt is not unit-tested, but the windowing and physical-row
 * counting ARE, since miscounting wrapped rows is the documented breakage mode
 * (the prompt reprints itself when clearRender under-counts). Build:
 *   cc -D_DEFAULT_SOURCE -Wall -Wextra -g \
 *      src/tui.c src/test_tui.c -o /tmp/test_tui && /tmp/test_tui
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "tui.h"

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

#define CHECK_EQ(got, want, msg)					\
	do {								\
		checks++;						\
		if ((size_t)(got) != (size_t)(want)) {			\
			printf("FAIL %s: got %zu want %zu\n", (msg),	\
			       (size_t)(got), (size_t)(want));		\
			failures++;					\
		}							\
	} while (0)

/* ---- tui_visible_start: windowed scrolling over arbitrary N -------------- */

static void
test_visible_start_small(void)
{
	/* Whole list fits in the window: always start at 0. */
	CHECK_EQ(tui_visible_start(0, 5, 8), 0, "fits: cursor 0");
	CHECK_EQ(tui_visible_start(4, 5, 8), 0, "fits: cursor last");
	CHECK_EQ(tui_visible_start(0, 8, 8), 0, "exact fit: start 0");
}

static void
test_visible_start_scroll(void)
{
	/* 20 items, window 8, half = 4. start = clamp(cursor-4, 0, 12). */
	CHECK_EQ(tui_visible_start(0, 20, 8), 0, "top: clamps to 0");
	CHECK_EQ(tui_visible_start(3, 20, 8), 0, "near top: still 0 (3-4<0)");
	CHECK_EQ(tui_visible_start(4, 20, 8), 0, "cursor 4: 4-4=0");
	CHECK_EQ(tui_visible_start(10, 20, 8), 6, "mid: 10-4=6");
	CHECK_EQ(tui_visible_start(16, 20, 8), 12, "near end: clamps to 12");
	CHECK_EQ(tui_visible_start(19, 20, 8), 12, "last: clamps to len-window");
}

static void
test_visible_start_large(void)
{
	/* No cap on N: a 200-item list windows correctly throughout, and every
	 * item stays reachable (the window's last index covers len-1). */
	size_t len = 200, win = 12, half = win / 2;
	for (size_t cursor = 0; cursor < len; cursor++) {
		size_t start = tui_visible_start(cursor, len, win);
		/* The cursor is always inside [start, start+win). */
		CHECK(cursor >= start && cursor < start + win,
		      "large N: cursor stays within the window");
		/* start never exceeds len-win and is never negative (unsigned). */
		CHECK(start <= len - win, "large N: start clamped to len-window");
		/* Matches the documented formula. */
		size_t want = cursor > half ? cursor - half : 0;
		if (want > len - win)
			want = len - win;
		CHECK_EQ(start, want, "large N: start matches the formula");
	}
	/* The last window reaches the final item (nothing is unreachable). */
	size_t laststart = tui_visible_start(len - 1, len, win);
	CHECK_EQ(laststart + win, len, "large N: last window ends at len");
}

static void
test_visible_start_edge(void)
{
	CHECK_EQ(tui_visible_start(0, 0, 8), 0, "empty list: start 0");
	CHECK_EQ(tui_visible_start(0, 10, 0), 0, "zero window: start 0");
	CHECK_EQ(tui_visible_start(9, 10, 1), 9, "window 1: start tracks cursor");
}

/* ---- tui_visual_rows_for_line: physical rows with wrapping + ANSI -------- */

static void
test_rows_plain(void)
{
	CHECK_EQ(tui_visual_rows_for_line("", 80), 1, "empty line is 1 row");
	CHECK_EQ(tui_visual_rows_for_line("hello", 80), 1, "short line is 1 row");
	/* 80 cells in 80 cols = exactly 1 row; 81 cells = 2 rows. */
	char c80[81], c81[82];
	memset(c80, 'x', 80); c80[80] = '\0';
	memset(c81, 'x', 81); c81[81] = '\0';
	CHECK_EQ(tui_visual_rows_for_line(c80, 80), 1, "80 in 80 cols: 1 row");
	CHECK_EQ(tui_visual_rows_for_line(c81, 80), 2, "81 in 80 cols: 2 rows");
}

static void
test_rows_wrapping(void)
{
	char c200[201];
	memset(c200, 'x', 200); c200[200] = '\0';
	/* 200 cells / 80 cols = ceil(2.5) = 3 rows. */
	CHECK_EQ(tui_visual_rows_for_line(c200, 80), 3, "200 in 80: 3 rows");
	/* Narrow terminal: 200 / 20 = 10 rows. */
	CHECK_EQ(tui_visual_rows_for_line(c200, 20), 10, "200 in 20: 10 rows");
	/* columns clamped to >= 1: 5 cells in "0" cols -> treat as 1 col -> 5. */
	CHECK_EQ(tui_visual_rows_for_line("abcde", 0), 5, "0 cols clamps to 1");
}

static void
test_rows_ansi_not_counted(void)
{
	/* SGR escapes contribute zero display width: a green 5-char word fits
	 * in 1 row even though the byte string is much longer. */
	const char *colored = "\033[32mhello\033[0m";
	CHECK_EQ(tui_visual_rows_for_line(colored, 80), 1,
		 "ANSI SGR not counted toward width");
	/* A line of exactly 80 visible cells wrapped in colors is still 1 row. */
	char buf[256];
	char body[81];
	memset(body, 'y', 80); body[80] = '\0';
	snprintf(buf, sizeof(buf), "\033[2m\033[36m%s\033[0m", body);
	CHECK_EQ(tui_visual_rows_for_line(buf, 80), 1,
		 "80 visible cells + ANSI: still 1 row");
	/* 81 visible cells + ANSI wraps to 2. */
	char body2[82];
	memset(body2, 'y', 81); body2[81] = '\0';
	snprintf(buf, sizeof(buf), "\033[1m%s\033[0m", body2);
	CHECK_EQ(tui_visual_rows_for_line(buf, 80), 2,
		 "81 visible cells + ANSI: 2 rows");
}

static void
test_rows_utf8(void)
{
	/* Three '│' bars (3 UTF-8 bytes each) count as 3 cells, not 9. */
	const char *bars = "\xE2\x94\x82\xE2\x94\x82\xE2\x94\x82";
	CHECK_EQ(tui_visual_rows_for_line(bars, 80), 1, "UTF-8 bars: 3 cells, 1 row");
	CHECK_EQ(tui_visual_rows_for_line(bars, 2), 2, "3 cells in 2 cols: 2 rows");
}

int
main(void)
{
	test_visible_start_small();
	test_visible_start_scroll();
	test_visible_start_large();
	test_visible_start_edge();
	test_rows_plain();
	test_rows_wrapping();
	test_rows_ansi_not_counted();
	test_rows_utf8();

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
