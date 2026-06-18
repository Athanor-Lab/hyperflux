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

/* Self-contained terminal UI (no ncurses): termios raw mode with bulletproof
 * restore, ANSI alternate screen, arrow + j/k navigation, SIGWINCH redraw.
 *
 * The item model carries an optional second "detail" line per row so callers
 * (the scanner) can show the scoring signals. The model is shaped so a future
 * multi-select screen is a small addition (add a per-item selected flag and a
 * tui_select_many entry point) without reworking the render loop. */

#ifndef FLUX_TUI_H
#define FLUX_TUI_H

#include <stddef.h>

/* One selectable row: a primary label and an optional dimmed detail suffix. */
typedef struct {
	const char *label;	/* primary text (borrowed, not owned) */
	const char *detail;	/* optional secondary text, may be NULL */
} tui_item_t;

/* Present a single-select menu of `n` items under `title`. Returns the chosen
 * index in [0, n) on Enter, or -1 if the user cancels (q / Esc / Ctrl-C) or if
 * the terminal is unsuitable.
 *
 * When stdin/stdout is not a TTY or TERM is "dumb"/unset, falls back to a
 * numbered line prompt on stderr/stdin (still returns an index or -1). The
 * raw-mode terminal is always restored (alternate screen left, cursor shown)
 * on every exit path including signals and errors. */
int tui_select_one(const char *title, const tui_item_t *items, size_t n);

#endif				/* FLUX_TUI_H */
