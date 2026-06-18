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
	/* Show cursor, leave alternate screen. Best-effort: ignore short writes
	 * on a dying terminal. */
	static const char leave[] = "\033[?25h\033[?1049l";
	ssize_t w = write(STDOUT_FILENO, leave, sizeof(leave) - 1);
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

/* Enter raw mode and the alternate screen. Returns 0 on success, -1 on
 * failure (terminal state unchanged). */
static int
tui_enter(void)
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
	wa.sa_flags = SA_RESTART;	/* don't let resize interrupt our read() */
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

	/* Enter alternate screen, hide cursor. */
	static const char enter[] = "\033[?1049h\033[?25l";
	ssize_t w = write(STDOUT_FILENO, enter, sizeof(enter) - 1);
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

	if (tui_enter() != 0)
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

	if (tui_enter() != 0) {
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
