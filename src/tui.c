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

/* Self-contained TUI -- see tui.h. The hard requirement here is terminal
 * safety: raw mode and the alternate screen MUST be undone on every exit path,
 * including SIGINT/SIGTERM and errors, so the user is never left with a broken
 * terminal. We save the original termios once and restore it via atexit and a
 * signal handler, guarding against a double restore. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "tui.h"

/* Saved terminal state and a flag tracking whether raw mode is currently in
 * force. Static module state because atexit/signal handlers take no argument.
 * `volatile sig_atomic_t` for the flag touched from the signal handler. */
static struct termios tui_saved_termios;
static volatile sig_atomic_t tui_raw_active;	/* 1 while raw mode is on */
static volatile sig_atomic_t tui_inline;	/* 1 = inline render (no alt screen) */
static volatile sig_atomic_t tui_resized;	/* set by SIGWINCH */
static int tui_atexit_installed;		/* register cleanup only once */

/* Previously-installed signal handlers, restored when we leave the TUI so we do
 * not clobber the caller's SIGINT/SIGTERM handling for the rest of the run. */
static struct sigaction tui_old_int, tui_old_term, tui_old_winch;
static int tui_handlers_installed;

/* Leave raw mode and the alternate screen. Idempotent and async-signal-safe in
 * the parts that matter (tcsetattr + a write of a fixed ANSI string). */
static void
tui_restore(void)
{
	if (!tui_raw_active)
		return;
	tui_raw_active = 0;
	/* Show cursor; leave the alternate screen only if we entered it. Inline
	 * mode renders in the normal buffer, so we just re-show the cursor and
	 * emit a CRLF so the shell prompt starts on a fresh line. Best-effort:
	 * ignore short writes on a dying terminal. */
	const char *leave = tui_inline ? "\033[?25h\r\n" : "\033[?25h\033[?1049l";
	size_t leavelen = strlen(leave);
	ssize_t w = write(STDOUT_FILENO, leave, leavelen);
	(void)w;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &tui_saved_termios);
}

/* atexit hook: guarantees restore even if the caller exit()s mid-screen. */
static void
tui_atexit(void)
{
	tui_restore();
}

/* Signal handler: restore the terminal, reinstall the previous handler, and
 * re-raise so the default action (or the caller's handler) still runs. */
static void
tui_on_signal(int sig)
{
	tui_restore();
	struct sigaction *old = NULL;
	if (sig == SIGINT)
		old = &tui_old_int;
	else if (sig == SIGTERM)
		old = &tui_old_term;
	if (old)
		sigaction(sig, old, NULL);
	raise(sig);
}

static void
tui_on_winch(int sig)
{
	(void)sig;
	tui_resized = 1;
}

/* Enter raw mode. With `inline_mode` zero we also switch to the alternate
 * screen (full-screen menus); with `inline_mode` non-zero we render inline in
 * the normal buffer (the skills-styled prompt). Returns 0 on success, -1 on
 * failure (terminal state unchanged). */
static int
tui_enter(int inline_mode)
{
	struct termios raw;

	if (tcgetattr(STDIN_FILENO, &tui_saved_termios) != 0)
		return -1;

	raw = tui_saved_termios;
	/* Disable canonical mode and echo; keep ISIG off so we read Ctrl-C as a
	 * key but still install a handler as a belt-and-braces restore. */
	raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	raw.c_oflag &= ~(tcflag_t)(OPOST);
	raw.c_cc[VMIN] = 1;	/* block for at least one byte */
	raw.c_cc[VTIME] = 0;

	if (!tui_atexit_installed) {
		if (atexit(tui_atexit) == 0)
			tui_atexit_installed = 1;
	}

	/* Install signal handlers (saving the old ones) before flipping the
	 * terminal, so a signal between here and the first read still restores. */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = tui_on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, &tui_old_int);
	sigaction(SIGTERM, &sa, &tui_old_term);
	struct sigaction wa;
	memset(&wa, 0, sizeof(wa));
	wa.sa_handler = tui_on_winch;
	sigemptyset(&wa.sa_mask);
	/* No SA_RESTART: let SIGWINCH interrupt read() so the loop redraws. */
	sigaction(SIGWINCH, &wa, &tui_old_winch);
	tui_handlers_installed = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
		/* Roll back the handlers we just installed. */
		sigaction(SIGINT, &tui_old_int, NULL);
		sigaction(SIGTERM, &tui_old_term, NULL);
		sigaction(SIGWINCH, &tui_old_winch, NULL);
		tui_handlers_installed = 0;
		return -1;
	}
	tui_raw_active = 1;
	tui_inline = inline_mode ? 1 : 0;

	/* Full-screen: enter alternate screen + hide cursor. Inline: just hide
	 * the cursor (the prompt draws in the normal buffer). */
	const char *enter = inline_mode ? "\033[?25l" : "\033[?1049h\033[?25l";
	size_t enterlen = strlen(enter);
	ssize_t w = write(STDOUT_FILENO, enter, enterlen);
	(void)w;
	return 0;
}

/* Full teardown: restore terminal and reinstall the caller's signal handlers. */
static void
tui_leave(void)
{
	tui_restore();
	if (tui_handlers_installed) {
		sigaction(SIGINT, &tui_old_int, NULL);
		sigaction(SIGTERM, &tui_old_term, NULL);
		sigaction(SIGWINCH, &tui_old_winch, NULL);
		tui_handlers_installed = 0;
	}
}

/* Query the terminal row count; default to a sane value on failure. */
static int
tui_rows(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
		return ws.ws_row;
	return 24;
}

/* Write a NUL-terminated string to the terminal (best effort). */
static void
tui_puts(const char *s)
{
	size_t len = strlen(s);
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(STDOUT_FILENO, s + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)w;
	}
}

/* Render the menu: clear screen, draw the title, then each item (highlighting
 * the cursor). `top` is the first visible item when the list scrolls. */
static void
tui_draw(const char *title, const tui_item_t *items, size_t n, size_t cursor,
	 size_t top, size_t visible)
{
	tui_puts("\033[2J\033[H");	/* clear + home */
	if (title) {
		tui_puts("\033[1m");	/* bold */
		tui_puts(title);
		tui_puts("\033[0m\r\n");
	}
	tui_puts("  (up/down or j/k to move, Enter to select, q to cancel)\r\n\r\n");

	size_t end = top + visible;
	if (end > n)
		end = n;
	for (size_t i = top; i < end; i++) {
		if (i == cursor)
			tui_puts("\033[7m> ");	/* reverse video */
		else
			tui_puts("  ");
		tui_puts(items[i].label ? items[i].label : "");
		if (i == cursor)
			tui_puts("\033[0m");
		tui_puts("\r\n");
		if (items[i].detail) {
			tui_puts("    \033[2m");	/* dim */
			tui_puts(items[i].detail);
			tui_puts("\033[0m\r\n");
		}
	}
}

/* Read one logical key. Returns: 'k'/'j' for up/down (arrows mapped to these),
 * '\r' for enter, 'q' for cancel, or 0 on a redraw-only event/EINTR. */
static int
tui_readkey(void)
{
	unsigned char c;
	ssize_t r = read(STDIN_FILENO, &c, 1);
	if (r <= 0) {
		if (r < 0 && errno == EINTR)
			return 0;	/* signal (e.g. SIGWINCH): redraw */
		return 'q';		/* EOF: treat as cancel */
	}

	if (c == '\033') {	/* escape: maybe an arrow sequence */
		unsigned char seq[2];
		ssize_t r1 = read(STDIN_FILENO, &seq[0], 1);
		if (r1 <= 0)
			return 'q';	/* lone Esc cancels */
		ssize_t r2 = read(STDIN_FILENO, &seq[1], 1);
		if (r2 <= 0)
			return 'q';
		if (seq[0] == '[') {
			if (seq[1] == 'A')
				return 'k';	/* up */
			if (seq[1] == 'B')
				return 'j';	/* down */
		}
		return 0;	/* unknown sequence: ignore */
	}
	if (c == '\r' || c == '\n')
		return '\r';
	if (c == 'q' || c == 3 /* Ctrl-C */)
		return 'q';
	return c;
}

/* Non-TTY fallback: numbered prompt on stderr, read a line from stdin. Returns
 * the chosen 0-based index or -1. */
static int
tui_fallback(const char *title, const tui_item_t *items, size_t n)
{
	if (title)
		fprintf(stderr, "%s\n", title);
	for (size_t i = 0; i < n; i++) {
		fprintf(stderr, "  %zu) %s\n", i + 1,
			items[i].label ? items[i].label : "");
		if (items[i].detail)
			fprintf(stderr, "       %s\n", items[i].detail);
	}
	fprintf(stderr, "Select [1-%zu] (or q to cancel): ", n);
	fflush(stderr);

	char line[64];
	if (!fgets(line, sizeof(line), stdin))
		return -1;
	if (line[0] == 'q' || line[0] == 'Q')
		return -1;
	char *end = NULL;
	long v = strtol(line, &end, 10);
	if (end == line || v < 1 || (size_t)v > n)
		return -1;
	return (int)(v - 1);
}

int
tui_select_one(const char *title, const tui_item_t *items, size_t n)
{
	if (!items || n == 0)
		return -1;

	/* Degrade to a line prompt when full-screen control is unsafe. */
	const char *term = getenv("TERM");
	int is_dumb = !term || !*term || strcmp(term, "dumb") == 0;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || is_dumb)
		return tui_fallback(title, items, n);

	if (tui_enter(0) != 0)
		return tui_fallback(title, items, n);

	int result = -1;
	size_t cursor = 0, top = 0;
	for (;;) {
		int rows = tui_rows();
		/* Reserve 4 lines for the title/help; 2 terminal rows per item
		 * when details are shown is the worst case, so be conservative. */
		size_t visible = rows > 6 ? (size_t)(rows - 4) / 2 : 1;
		if (visible == 0)
			visible = 1;
		if (cursor < top)
			top = cursor;
		else if (cursor >= top + visible)
			top = cursor - visible + 1;

		tui_draw(title, items, n, cursor, top, visible);
		tui_resized = 0;

		int key = tui_readkey();
		if (key == 'q') {
			result = -1;
			break;
		} else if (key == '\r') {
			result = (int)cursor;
			break;
		} else if (key == 'j') {
			if (cursor + 1 < n)
				cursor++;
		} else if (key == 'k') {
			if (cursor > 0)
				cursor--;
		} else if (key == 'g') {	/* jump to top */
			cursor = 0;
		} else if (key == 'G') {	/* jump to bottom */
			cursor = n - 1;
		}
		/* key == 0: redraw only (SIGWINCH or unknown sequence). */
	}

	tui_leave();
	return result;
}

/* ---- multi-select ----------------------------------------------------- */

/* Render the multi-select menu: a checkbox per row plus a selected count. */
static void
tui_draw_multi(const char *title, const tui_item_t *items, size_t n,
	       const unsigned char *sel, size_t cursor, size_t top,
	       size_t visible)
{
	tui_puts("\033[2J\033[H");	/* clear + home */
	if (title) {
		tui_puts("\033[1m");	/* bold */
		tui_puts(title);
		tui_puts("\033[0m\r\n");
	}
	tui_puts("  (up/down or j/k move, space toggle, a all, Enter confirm, q cancel)\r\n");

	size_t nsel = 0;
	for (size_t i = 0; i < n; i++)
		if (sel[i])
			nsel++;
	char count[64];
	snprintf(count, sizeof(count), "  %zu of %zu selected\r\n\r\n", nsel, n);
	tui_puts(count);

	size_t end = top + visible;
	if (end > n)
		end = n;
	for (size_t i = top; i < end; i++) {
		if (i == cursor)
			tui_puts("\033[7m> ");	/* reverse video */
		else
			tui_puts("  ");
		tui_puts(sel[i] ? "[x] " : "[ ] ");
		tui_puts(items[i].label ? items[i].label : "");
		if (i == cursor)
			tui_puts("\033[0m");
		tui_puts("\r\n");
		if (items[i].detail) {
			tui_puts("      \033[2m");	/* dim */
			tui_puts(items[i].detail);
			tui_puts("\033[0m\r\n");
		}
	}
}

/* Apply a comma-separated 1-based spec ("1,3-5,8" or "all"/"*") to sel[0..n).
 * Returns 0 on success (sel updated), -1 on a malformed/out-of-range spec. */
static int
tui_apply_spec(const char *spec, unsigned char *sel, size_t n)
{
	while (*spec == ' ' || *spec == '\t')
		spec++;
	if (strcmp(spec, "all") == 0 || strcmp(spec, "*") == 0 ||
	    strcmp(spec, "a") == 0) {
		memset(sel, 1, n);
		return 0;
	}

	unsigned char *tmp = calloc(n, 1);
	if (!tmp)
		return -1;
	const char *p = spec;
	int ok = 1;
	while (*p && ok) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p < '0' || *p > '9') { ok = 0; break; }
		size_t a = 0;
		while (*p >= '0' && *p <= '9') {
			a = a * 10 + (size_t)(*p - '0');
			if (a > n + 1)
				a = n + 1;
			p++;
		}
		size_t b = a;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '-') {
			p++;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p < '0' || *p > '9') { ok = 0; break; }
			b = 0;
			while (*p >= '0' && *p <= '9') {
				b = b * 10 + (size_t)(*p - '0');
				if (b > n + 1)
					b = n + 1;
				p++;
			}
		}
		if (a < 1 || a > n || b < a || b > n) { ok = 0; break; }
		for (size_t i = a; i <= b; i++)
			tmp[i - 1] = 1;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p == ',') { p++; continue; }
		if (*p == '\0')
			break;
		ok = 0;
	}
	if (ok)
		memcpy(sel, tmp, n);
	free(tmp);
	return ok ? 0 : -1;
}

/* Non-TTY fallback for multi-select: numbered list + a spec line. Fills sel and
 * returns the count, or -1 on cancel/EOF. */
static int
tui_fallback_multi(const char *title, const tui_item_t *items, size_t n,
		   unsigned char *sel)
{
	if (title)
		fprintf(stderr, "%s\n", title);
	for (size_t i = 0; i < n; i++) {
		fprintf(stderr, "  %zu) %s\n", i + 1,
			items[i].label ? items[i].label : "");
		if (items[i].detail)
			fprintf(stderr, "       %s\n", items[i].detail);
	}
	fprintf(stderr,
		"Select (e.g. 1,3-5,8 or 'all'; q to cancel): ");
	fflush(stderr);

	char line[256];
	if (!fgets(line, sizeof(line), stdin))
		return -1;
	char *nl = strpbrk(line, "\r\n");
	if (nl)
		*nl = '\0';
	if (line[0] == 'q' || line[0] == 'Q')
		return -1;

	memset(sel, 0, n);
	if (tui_apply_spec(line, sel, n) != 0)
		return -1;
	int cnt = 0;
	for (size_t i = 0; i < n; i++)
		if (sel[i])
			cnt++;
	return cnt ? cnt : -1;
}

/* Build the malloc'd selected-index array from sel; returns count, sets *out.
 * On OOM returns -2 with *out NULL. A zero count yields *out NULL, returns 0. */
static int
tui_collect(const unsigned char *sel, size_t n, size_t **out)
{
	*out = NULL;
	size_t cnt = 0;
	for (size_t i = 0; i < n; i++)
		if (sel[i])
			cnt++;
	if (cnt == 0)
		return 0;
	size_t *idx = malloc(cnt * sizeof(*idx));
	if (!idx)
		return -2;
	size_t k = 0;
	for (size_t i = 0; i < n; i++)
		if (sel[i])
			idx[k++] = i;
	*out = idx;
	return (int)cnt;
}

int
tui_select_many(const char *title, const tui_item_t *items, size_t n,
		const unsigned char *preselect, size_t **out_idx)
{
	if (out_idx)
		*out_idx = NULL;
	if (!items || n == 0 || !out_idx)
		return -1;

	unsigned char *sel = calloc(n, 1);
	if (!sel)
		return -2;
	if (preselect)
		for (size_t i = 0; i < n; i++)
			sel[i] = preselect[i] ? 1 : 0;

	const char *term = getenv("TERM");
	int is_dumb = !term || !*term || strcmp(term, "dumb") == 0;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || is_dumb) {
		int cnt = tui_fallback_multi(title, items, n, sel);
		if (cnt < 0) {
			free(sel);
			return -1;
		}
		int r = tui_collect(sel, n, out_idx);
		free(sel);
		return r;
	}

	if (tui_enter(0) != 0) {
		int cnt = tui_fallback_multi(title, items, n, sel);
		if (cnt < 0) {
			free(sel);
			return -1;
		}
		int r = tui_collect(sel, n, out_idx);
		free(sel);
		return r;
	}

	int cancelled = 0;
	size_t cursor = 0, top = 0;
	for (;;) {
		int rows = tui_rows();
		/* Reserve 5 lines for title/help/count; up to 2 rows per item. */
		size_t visible = rows > 7 ? (size_t)(rows - 5) / 2 : 1;
		if (visible == 0)
			visible = 1;
		if (cursor < top)
			top = cursor;
		else if (cursor >= top + visible)
			top = cursor - visible + 1;

		tui_draw_multi(title, items, n, sel, cursor, top, visible);
		tui_resized = 0;

		int key = tui_readkey();
		if (key == 'q') {
			cancelled = 1;
			break;
		} else if (key == '\r') {
			break;
		} else if (key == 'j') {
			if (cursor + 1 < n)
				cursor++;
		} else if (key == 'k') {
			if (cursor > 0)
				cursor--;
		} else if (key == ' ') {
			sel[cursor] = !sel[cursor];
		} else if (key == 'a') {	/* toggle all on/off */
			int all = 1;
			for (size_t i = 0; i < n; i++)
				if (!sel[i]) { all = 0; break; }
			memset(sel, all ? 0 : 1, n);
		} else if (key == 'g') {
			cursor = 0;
		} else if (key == 'G') {
			cursor = n - 1;
		}
		/* key == 0: redraw only (SIGWINCH or unknown sequence). */
	}

	tui_leave();

	if (cancelled) {
		free(sel);
		return -1;
	}
	int r = tui_collect(sel, n, out_idx);
	free(sel);
	return r;
}

/* ---- skills-styled episode multi-select ------------------------------- */

/* ANSI SGR (picocolors -> escapes): green/dim/cyan/red/bold/underline/reset. */
#define SGR_GREEN	"\033[32m"
#define SGR_DIM		"\033[2m"
#define SGR_CYAN	"\033[36m"
#define SGR_RED		"\033[31m"
#define SGR_BOLD	"\033[1m"
#define SGR_ULINE	"\033[4m"
#define SGR_RESET	"\033[0m"

/* UTF-8 glyphs matching the skills reference (◆ ◇ ■ ● ○ │ ❯ └ ✓). */
#define G_ACTIVE	"\xE2\x97\x86"	/* ◆ */
#define G_SUBMIT	"\xE2\x97\x87"	/* ◇ */
#define G_CANCEL	"\xE2\x96\xA0"	/* ■ */
#define G_RADIO_ON	"\xE2\x97\x8F"	/* ● */
#define G_RADIO_OFF	"\xE2\x97\x8B"	/* ○ */
#define G_BAR		"\xE2\x94\x82"	/* │ */
#define G_CURSOR	"\xE2\x9D\xAF"	/* ❯ */
#define G_CLOSER	"\xE2\x94\x94"	/* └ */
#define G_TICK		"\xE2\x9C\x93"	/* ✓ */

/* Episode-prompt hint line, shared so the height estimate matches the render. */
#define EPISODE_HINT	SGR_DIM G_BAR \
	"  \xE2\x86\x91\xE2\x86\x93 move \xC2\xB7 space select \xC2\xB7" \
	" a all \xC2\xB7 enter confirm \xC2\xB7 q cancel" SGR_RESET

size_t
tui_visual_rows_for_line(const char *line, size_t columns)
{
	if (!line)
		return 1;
	if (columns < 1)
		columns = 1;

	/* Display width = bytes that are not part of an ANSI SGR escape and not
	 * UTF-8 continuation bytes (0x80..0xBF). One cell per code point keeps
	 * this simple; our glyphs are all single-width in modern terminals. */
	size_t width = 0;
	for (const char *p = line; *p;) {
		if (*p == '\033') {	/* skip a CSI "...m" sequence */
			p++;
			if (*p == '[') {
				p++;
				while (*p && *p != 'm')
					p++;
				if (*p == 'm')
					p++;
			}
			continue;
		}
		unsigned char c = (unsigned char)*p;
		if ((c & 0xC0) != 0x80)	/* count leading bytes, skip continuations */
			width++;
		p++;
	}
	size_t rows = (width + columns - 1) / columns;	/* ceil */
	return rows ? rows : 1;
}

size_t
tui_visible_start(size_t cursor, size_t len, size_t max_visible)
{
	if (max_visible == 0 || len <= max_visible)
		return 0;
	size_t half = max_visible / 2;
	size_t maxstart = len - max_visible;	/* len > max_visible here */
	size_t start = cursor > half ? cursor - half : 0;
	if (start > maxstart)
		start = maxstart;
	return start;
}

/* A growable list of heap-owned logical lines built per render. */
typedef struct {
	char **v;
	size_t n, cap;
} line_buf_t;

/* Append a copy of [s] to lb. Returns 0 or -1 (OOM; lb left consistent). */
static int
lb_push(line_buf_t *lb, const char *s)
{
	if (lb->n >= lb->cap) {
		size_t ncap = lb->cap ? lb->cap * 2 : 16;
		char **nv = realloc(lb->v, ncap * sizeof(*nv));
		if (!nv)
			return -1;
		lb->v = nv;
		lb->cap = ncap;
	}
	char *dup = malloc(strlen(s) + 1);
	if (!dup)
		return -1;
	strcpy(dup, s);
	lb->v[lb->n++] = dup;
	return 0;
}

static void
lb_free(line_buf_t *lb)
{
	for (size_t i = 0; i < lb->n; i++)
		free(lb->v[i]);
	free(lb->v);
	lb->v = NULL;
	lb->n = lb->cap = 0;
}

/* Byte length of the UTF-8 sequence starting at `p` (p points at a non-NUL),
 * clamped so a truncated trailing sequence never advances past the NUL. */
static size_t
utf8_adv(const char *p)
{
	unsigned char c = (unsigned char)*p;
	size_t adv = 1;
	if (c >= 0xF0)
		adv = 4;
	else if (c >= 0xE0)
		adv = 3;
	else if (c >= 0xC0)
		adv = 2;
	/* Stop early if the sequence is cut short by the NUL terminator. */
	for (size_t k = 1; k < adv; k++)
		if (p[k] == '\0')
			return k;
	return adv;
}

/* Truncate `src` to at most `maxcells` display cells, appending an ellipsis
 * ".." when it had to cut, into dst[dlen]. Plain text only (no ANSI). Counts
 * UTF-8 code points as one cell each. Always NUL-terminates. */
static void
truncate_cells(char *dst, size_t dlen, const char *src, size_t maxcells)
{
	if (!dlen)
		return;
	if (maxcells < 1)
		maxcells = 1;

	/* Count code points. utf8_adv() never steps past the NUL, so malformed
	 * input (a truncated lead byte) can't read out of bounds. */
	size_t cells = 0;
	const char *p = src;
	while (*p) {
		p += utf8_adv(p);
		cells++;
	}

	if (cells <= maxcells) {	/* fits: copy verbatim */
		size_t L = strlen(src);
		if (L >= dlen)
			L = dlen - 1;
		memcpy(dst, src, L);
		dst[L] = '\0';
		return;
	}

	/* Need to cut: reserve two cells for "..". */
	size_t keep = maxcells > 2 ? maxcells - 2 : 1;
	cells = 0;
	p = src;
	while (*p && cells < keep) {
		p += utf8_adv(p);
		cells++;
	}
	size_t blen = (size_t)(p - src);
	size_t o = 0;
	for (size_t i = 0; i < blen && o + 1 < dlen; i++)
		dst[o++] = src[i];
	if (o + 2 < dlen) {
		dst[o++] = '.';
		dst[o++] = '.';
	}
	dst[o] = '\0';
}

/* Build the "Selected:" summary (up to 3 labels then "+N more") into dst. */
static void
build_summary(char *dst, size_t dlen, const tui_episode_t *items, size_t n,
	      const unsigned char *sel)
{
	if (!dlen)
		return;
	dst[0] = '\0';

	size_t total = 0;
	for (size_t i = 0; i < n; i++)
		if (sel[i])
			total++;

	if (total == 0) {
		snprintf(dst, dlen, "(none)");
		return;
	}

	size_t o = 0, shown = 0;
	for (size_t i = 0; i < n && shown < 3; i++) {
		if (!sel[i])
			continue;
		const char *lab = items[i].label ? items[i].label : "";
		if (shown > 0 && o + 2 < dlen) {
			dst[o++] = ',';
			dst[o++] = ' ';
		}
		for (const char *c = lab; *c && o + 1 < dlen; c++)
			dst[o++] = *c;
		shown++;
	}
	dst[o] = '\0';
	if (total > 3) {
		size_t used = strlen(dst);
		if (used < dlen)
			snprintf(dst + used, dlen - used, " +%zu more", total - 3);
	}
}

/* Erase the previous render: move up `phys` physical rows and clear each,
 * leaving the cursor at the start of the first cleared row. Mirrors the skills
 * clearRender(); phys is a PHYSICAL row count (wrapping accounted for). */
static void
episode_clear(size_t phys)
{
	if (phys == 0)
		return;
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "\033[%zuA", phys);
	if (len > 0)
		tui_puts(buf);
	for (size_t i = 0; i < phys; i++)
		tui_puts("\033[2K\033[1B");	/* clear line, move down one */
	len = snprintf(buf, sizeof(buf), "\033[%zuA", phys);
	if (len > 0)
		tui_puts(buf);
}

/* Width of the terminal in columns; default 80 on failure. */
static size_t
tui_cols(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 80;
}

/* Render the whole prompt for the given state. Returns the number of PHYSICAL
 * rows written (so the next clear erases exactly that much), or 0 on OOM. */
static size_t
episode_render(const char *message, const tui_episode_t *items, size_t n,
	       const unsigned char *sel, size_t cursor, size_t on_disk_count,
	       size_t total, size_t prev_phys, int state /*0 active,1 submit,2 cancel*/)
{
	size_t cols = tui_cols();
	line_buf_t lb = { NULL, 0, 0 };
	char line[1024];
	int oom = 0;
	/* Declared before any `goto emit` so the jump skips no initialization. */
	size_t start = 0, end = 0, label_budget = 2, before = 0, after = 0;

	size_t rows = (size_t)tui_rows();

	const char *icon = state == 0 ? SGR_GREEN G_ACTIVE SGR_RESET
		: state == 2 ? SGR_RED G_CANCEL SGR_RESET
		: SGR_GREEN G_SUBMIT SGR_RESET;

	/* Header: 'icon  <bold message>   (M/N on disk)'. */
	snprintf(line, sizeof(line), "%s  " SGR_BOLD "%s" SGR_RESET
		 "   " SGR_DIM "(%zu/%zu on disk)" SGR_RESET,
		 icon, message ? message : "", on_disk_count, total);

	/* Chrome physical rows: header + hint (both measured, they wrap on a narrow
	 * term), two gutters, scroll, closer (1 each) and the selected summary (up
	 * to 2 rows). Each item row is <= cols so it never wraps. Clamp max_visible
	 * so chrome + window <= rows-1, keeping the block inside the viewport so
	 * episode_clear() never desyncs. */
	size_t chrome = tui_visual_rows_for_line(line, cols)
		+ tui_visual_rows_for_line(EPISODE_HINT, cols) + 6;
	size_t budget = rows > 1 ? rows - 1 : 1;
	size_t max_visible = budget > chrome ? budget - chrome : 1;
	if (max_visible > n)
		max_visible = n;
	if (max_visible == 0)
		max_visible = 1;

	if (lb_push(&lb, line) < 0) { oom = 1; goto emit; }

	if (state != 0) {	/* submit / cancel: one summary/closer line */
		if (state == 1) {
			char summary[512];
			build_summary(summary, sizeof(summary), items, n, sel);
			snprintf(line, sizeof(line),
				 SGR_DIM G_BAR SGR_RESET "  " SGR_GREEN
				 "Selected:" SGR_RESET " %s", summary);
		} else {
			snprintf(line, sizeof(line),
				 SGR_DIM G_BAR "  Cancelled" SGR_RESET);
		}
		if (lb_push(&lb, line) < 0) { oom = 1; goto emit; }
		goto emit;
	}

	/* Hint line. */
	if (lb_push(&lb, EPISODE_HINT) < 0) { oom = 1; goto emit; }

	/* Empty gutter. */
	if (lb_push(&lb, SGR_DIM G_BAR SGR_RESET) < 0) { oom = 1; goto emit; }

	/* Windowed item rows. */
	start = tui_visible_start(cursor, n, max_visible);
	end = start + max_visible;
	if (end > n)
		end = n;

	/* Budget for the label: columns minus the row prefix "│ ❯ ● " (~6 cells)
	 * minus a trailing " ✓" (2 cells) so on-disk rows don't wrap. */
	label_budget = cols > 10 ? cols - 8 : 2;

	for (size_t i = start; i < end; i++) {
		const char *radio = sel[i] ? SGR_GREEN G_RADIO_ON SGR_RESET
					   : SGR_DIM G_RADIO_OFF SGR_RESET;
		int is_cur = (i == cursor);
		const char *prefix = is_cur ? SGR_CYAN G_CURSOR SGR_RESET : " ";
		char lab[768];
		truncate_cells(lab, sizeof(lab),
			       items[i].label ? items[i].label : "", label_budget);
		const char *tick = items[i].on_disk
			? " " SGR_GREEN G_TICK SGR_RESET : "";
		if (is_cur)
			snprintf(line, sizeof(line),
				 SGR_DIM G_BAR SGR_RESET " %s %s " SGR_ULINE
				 "%s" SGR_RESET "%s", prefix, radio, lab, tick);
		else
			snprintf(line, sizeof(line),
				 SGR_DIM G_BAR SGR_RESET " %s %s %s%s",
				 prefix, radio, lab, tick);
		if (lb_push(&lb, line) < 0) { oom = 1; goto emit; }
	}

	/* Scroll indicator: '↑ X more   ↓ Y more' when items are off-window. */
	before = start;
	after = n - end;
	if (before > 0 || after > 0) {
		char ind[128];
		int o = 0;
		ind[0] = '\0';
		if (before > 0)
			o += snprintf(ind + o, sizeof(ind) - o,
				      "\xE2\x86\x91 %zu more", before);
		if (after > 0)
			snprintf(ind + o, sizeof(ind) - o, "%s\xE2\x86\x93 %zu more",
				 before > 0 ? "   " : "", after);
		snprintf(line, sizeof(line), SGR_DIM G_BAR "  %s" SGR_RESET, ind);
		if (lb_push(&lb, line) < 0) { oom = 1; goto emit; }
	}

	/* Gutter + Selected summary + closer. */
	if (lb_push(&lb, SGR_DIM G_BAR SGR_RESET) < 0) { oom = 1; goto emit; }
	{
		char summary[512];
		build_summary(summary, sizeof(summary), items, n, sel);
		size_t nsel = 0;
		for (size_t i = 0; i < n; i++)
			if (sel[i])
				nsel++;
		if (nsel == 0)
			snprintf(line, sizeof(line),
				 SGR_DIM G_BAR "  Selected: (none)" SGR_RESET);
		else
			snprintf(line, sizeof(line),
				 SGR_DIM G_BAR SGR_RESET "  " SGR_GREEN
				 "Selected:" SGR_RESET " %s", summary);
		if (lb_push(&lb, line) < 0) { oom = 1; goto emit; }
	}
	if (lb_push(&lb, SGR_DIM G_CLOSER SGR_RESET) < 0) { oom = 1; goto emit; }

 emit:
	episode_clear(prev_phys);
	if (oom) {
		lb_free(&lb);
		return 0;
	}

	/* Write all lines joined by CRLF; count physical rows for the next clear.
	 * Trailing CRLF leaves the cursor on a fresh line below the closer. */
	size_t phys = 0;
	for (size_t i = 0; i < lb.n; i++) {
		tui_puts(lb.v[i]);
		tui_puts("\r\n");
		phys += tui_visual_rows_for_line(lb.v[i], cols);
	}
	lb_free(&lb);
	return phys;
}

/* Non-TTY fallback: numbered list with on-disk markers + an --episodes spec. */
static int
episode_fallback(const char *message, const tui_episode_t *items, size_t n,
		 unsigned char *sel)
{
	if (message)
		fprintf(stderr, "%s\n", message);
	for (size_t i = 0; i < n; i++)
		fprintf(stderr, "  %zu) %s%s\n", i + 1,
			items[i].label ? items[i].label : "",
			items[i].on_disk ? "  [on disk]" : "");
	fprintf(stderr,
		"Select (e.g. 1,3-5,8 or 'all'; Enter = missing only; q to cancel): ");
	fflush(stderr);

	char buf[256];
	if (!fgets(buf, sizeof(buf), stdin))
		return -1;
	char *nl = strpbrk(buf, "\r\n");
	if (nl)
		*nl = '\0';
	if (buf[0] == 'q' || buf[0] == 'Q')
		return -1;

	/* Empty line: keep the preselected (missing) set. */
	const char *p = buf;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0') {
		int cnt = 0;
		for (size_t i = 0; i < n; i++)
			if (sel[i])
				cnt++;
		return cnt;
	}

	memset(sel, 0, n);
	if (tui_apply_spec(buf, sel, n) != 0)
		return -1;
	int cnt = 0;
	for (size_t i = 0; i < n; i++)
		if (sel[i])
			cnt++;
	return cnt;
}

int
tui_episode_select(const char *message, const tui_episode_t *items, size_t n,
		   size_t on_disk_count, size_t total, size_t **out_idx)
{
	if (out_idx)
		*out_idx = NULL;
	if (!items || n == 0 || !out_idx)
		return -1;

	unsigned char *sel = calloc(n, 1);
	if (!sel)
		return -2;
	for (size_t i = 0; i < n; i++)
		sel[i] = items[i].selected ? 1 : 0;

	const char *term = getenv("TERM");
	int is_dumb = !term || !*term || strcmp(term, "dumb") == 0;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || is_dumb ||
	    tui_enter(1) != 0) {
		int cnt = episode_fallback(message, items, n, sel);
		if (cnt < 0) {
			free(sel);
			return -1;
		}
		int r = tui_collect(sel, n, out_idx);
		free(sel);
		return r;
	}

	int cancelled = 0;
	size_t cursor = 0;
	size_t prev_phys = 0;
	for (;;) {
		prev_phys = episode_render(message, items, n, sel, cursor,
					   on_disk_count, total, prev_phys, 0);
		tui_resized = 0;

		int key = tui_readkey();
		if (key == 'q') {
			cancelled = 1;
			break;
		} else if (key == '\r') {
			break;
		} else if (key == 'j') {
			if (cursor + 1 < n)
				cursor++;
		} else if (key == 'k') {
			if (cursor > 0)
				cursor--;
		} else if (key == ' ') {
			sel[cursor] = !sel[cursor];
		} else if (key == 'a') {	/* toggle all on <-> off */
			int all = 1;
			for (size_t i = 0; i < n; i++)
				if (!sel[i]) { all = 0; break; }
			memset(sel, all ? 0 : 1, n);
		} else if (key == 'g') {
			cursor = 0;
		} else if (key == 'G') {
			cursor = n - 1;
		}
		/* key == 0: redraw only (SIGWINCH or unknown sequence). */
	}

	/* Final frame: submit or cancel, then leave raw mode. */
	prev_phys = episode_render(message, items, n, sel, cursor,
				   on_disk_count, total, prev_phys,
				   cancelled ? 2 : 1);
	tui_leave();

	if (cancelled) {
		free(sel);
		return -1;
	}
	int r = tui_collect(sel, n, out_idx);
	free(sel);
	return r;
}
