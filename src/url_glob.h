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

/* URL range/list expansion (curl- and bash-style globbing) */

#ifndef FLUX_URL_GLOB_H
#define FLUX_URL_GLOB_H

#include <stddef.h>

/* Upper bound on the number of URLs a single expansion may produce. */
#define URL_GLOB_MAX_URLS 10000

/* One expanded URL together with the value each pattern took, in
 * left-to-right order. The captures back #N references in output names. */
typedef struct {
	char *url;
	char **caps;
} url_glob_t;

/* Expand brace/bracket range and list patterns in `src` into concrete URLs.
 *
 * A group is expanded only when it fully matches one of the grammars below;
 * anything else is left verbatim, so IPv6 literals (http://[::1]/) and CGI
 * parameters (a[b]=c) survive untouched:
 *   {a,b,c}        list
 *   {N..M}         numeric range (bash-style, may descend), padded by width
 *   {N..M..S}      numeric range with step
 *   {a..z}         single-character range
 *   [N-M] [N-M:S]  numeric range (curl-style, ascending only), optional step
 *   [a-z] [a-z:S]  single-character range
 *
 * On success returns the URL count (>= 1), sets *out to a calloc'd array of
 * url_glob_t (release with url_glob_free) and *ncaps to the number of pattern
 * captures per item (0 when there is no pattern). A pattern-free URL yields a
 * single item.
 *
 * Returns -1 on error: integer overflow, more than URL_GLOB_MAX_URLS results,
 * an expanded URL that would not fit in max_len bytes (including the NUL), or
 * out of memory. On error *out is set to NULL. */
int url_glob(const char *src, size_t max_len, url_glob_t **out, size_t *count,
	     size_t *ncaps);

void url_glob_free(url_glob_t *items, size_t count, size_t ncaps);

#endif				/* FLUX_URL_GLOB_H */
