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

/* Page scanner (--extract-scan): fetch a web page, heuristically collect media
 * candidates, score them (ad vs content), and emit a commented starter config
 * in the extractor's line format.
 *
 * Free-standing like extractor/hls: HTTP access is injected via callbacks
 * (extractor_fetch_fn for page/iframe/HLS bodies, scan_probe_fn for a direct
 * file's Content-Length), so the whole scanner unit-tests over saved HTML
 * fixtures with no network. */

#ifndef FLUX_SCAN_H
#define FLUX_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>		/* FILE (scan_emit_config) */

#include "extractor.h"		/* extractor_fetch_fn, ext_header_t */

/* Caps. A page yields a bounded set of candidates so a hostile page packed with
 * thousands of URLs can't exhaust memory. */
#define SCAN_MAX_CANDIDATES	256
#define SCAN_MAX_IFRAME_DEPTH	1	/* follow same-origin iframes one level */

/* What the media URL points at. */
typedef enum {
	SCAN_KIND_FILE = 0,	/* direct .mp4/.webm/... */
	SCAN_KIND_HLS,		/* .m3u8 playlist */
} scan_kind_t;

/* Where the URL was found, used as a scoring signal. */
typedef enum {
	SCAN_CTX_UNKNOWN = 0,
	SCAN_CTX_PLAYER,	/* a <video>/<source>/og:video/player JSON hit */
	SCAN_CTX_AD,		/* inside an ad container/iframe or ad host */
} scan_ctx_t;

/* One scored media candidate. */
typedef struct {
	char *url;		/* resolved absolute media URL (owned) */
	char *host;		/* host of url (owned), for the blocklist/uniqueness */
	scan_kind_t kind;
	scan_ctx_t context;
	double duration;	/* seconds (HLS EXTINF sum), 0 if unknown */
	long long size;		/* bytes (direct-file Content-Length), -1 if unknown */
	int width;		/* HLS variant RESOLUTION width, 0 if unknown */
	int height;		/* HLS variant RESOLUTION height, 0 if unknown */
	long bandwidth;		/* HLS variant BANDWIDTH bps, 0 if unknown */
	int count;		/* how many times this URL appeared on the page */
	int ad_host;		/* 1 if host is on the ad blocklist */
	double score;		/* computed rank (higher is better) */
} scan_candidate_t;

/* A scan result: the page URL and the ranked candidate list (best first). */
typedef struct {
	char *page_url;		/* the scanned page URL (owned) */
	scan_candidate_t cands[SCAN_MAX_CANDIDATES];
	size_t ncands;
} scan_result_t;

/* Probe a direct file URL for its size. Store the byte length in *out_size and
 * return 0 on success; return negative on failure (size unknown). May be NULL,
 * in which case direct-file sizes are left unknown. */
typedef int (*scan_probe_fn)(const char *url, long long *out_size,
			     void *userdata);

/* Scan `page_url`. Fetches the page (and same-origin iframes one level) via
 * `fetch`, parses HLS candidate durations via `fetch`, and probes direct-file
 * sizes via `probe` (may be NULL). Both callbacks receive `userdata`.
 *
 * On success returns a malloc'd scan_result_t (free with scan_result_free) with
 * a scored, ranked candidate list and *err left NULL. On failure returns NULL
 * and, if err is non-NULL, sets *err to a malloc'd message the caller frees.
 * A page that yields no candidates is a success with ncands == 0. */
scan_result_t *scan_page(const char *page_url, extractor_fetch_fn fetch,
			 scan_probe_fn probe, void *userdata, char **err);

void scan_result_free(scan_result_t *r);

/* True if `host` (lowercased authority, no port) is on the built-in ad/tracker
 * blocklist. Exposed for unit testing. */
int scan_is_ad_host(const char *host);

/* Emit a commented starter config (extractor line format) for `r` into the
 * stdio stream `out`. `chosen` is the index of the selected candidate, or -1 to
 * use the top-ranked one. When the selected media URL is directly resolvable
 * from the page, a near-complete config (name/match/output) is written;
 * otherwise a skeleton with detected pieces and guided TODO comments is written.
 * Returns 0 on success, negative on a write/argument error. */
int scan_emit_config(const scan_result_t *r, int chosen, FILE *out);

/* Derive the host-based config name scan_emit_config writes (e.g.
 * www.animeworld.ac -> "animeworld"), used to build the default save filename
 * <name>.conf. Writes a NUL-terminated name into `dst`. Returns 0 on success,
 * negative on a NULL/empty argument. */
int scan_config_name(const scan_result_t *r, char *dst, size_t len);

#endif				/* FLUX_SCAN_H */
