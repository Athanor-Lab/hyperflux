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

/* Media extractor: resolve a web-page URL to a direct media URL by running a
 * user-authored config of HTTP GETs and POSIX-ERE captures.
 *
 * Intentionally free-standing: the parser, interpolation, regex capture and
 * URL resolution depend only on libc + <regex.h>, so the module can be
 * unit-tested without the rest of Hyperflux. HTTP access is injected via a
 * callback (extractor_fetch_fn), so tests can stub the network. */

#ifndef FLUX_EXTRACTOR_H
#define FLUX_EXTRACTOR_H

#include <stddef.h>

/* Caps. A config is small and hand-written; these bound malformed input. */
#define EXTRACTOR_MAX_MATCHES	16
#define EXTRACTOR_MAX_DIRECTIVES	64
#define EXTRACTOR_MAX_VARS	64
#define EXTRACTOR_MAX_HEADERS	16

/* Cap the episode list a single 'list' directive can yield, so a malformed or
 * hostile series page can't make us iterate (and download) without bound. */
#define EXTRACTOR_MAX_EPISODES	2000

/* One request header (K=V) attached to a `get` directive. */
typedef struct {
	char *key;
	char *val;
} ext_header_t;

/* Directive kinds, evaluated top-to-bottom. */
typedef enum {
	EXT_DIR_VAR,	/* var  <name> <- <source> regex <ERE> */
	EXT_DIR_GET,	/* get  <name> <- <url-template> [+ headers] */
	EXT_DIR_OUTPUT,	/* output <template> */
	EXT_DIR_LIST,	/* list <name> ... (parsed, not yet supported in F1) */
} ext_dir_kind_t;

typedef struct {
	ext_dir_kind_t kind;
	char *name;		/* target var/response name (NULL for output) */
	char *source;		/* source name for var/list (NULL otherwise) */
	char *arg;		/* regex (var/list) or url-template (get) or
				 * output-template (output) */
	ext_header_t headers[EXTRACTOR_MAX_HEADERS];
	size_t nheaders;
} ext_directive_t;

/* A parsed extractor config. */
typedef struct {
	char *name;
	char *path;		/* source file, for diagnostics */
	char *matches[EXTRACTOR_MAX_MATCHES];
	size_t nmatches;
	ext_directive_t directives[EXTRACTOR_MAX_DIRECTIVES];
	size_t ndirectives;
} extractor_t;

/* HTTP fetch callback. Perform a GET to `url` with `nheaders` custom headers
 * and store the full response body (NUL-terminated) into *out_body (malloc'd,
 * owned by the caller). Returns 0 on success, negative on failure.
 * On failure *out_body must be left NULL. */
typedef int (*extractor_fetch_fn)(const char *url, const ext_header_t *headers,
				  size_t nheaders, char **out_body,
				  void *userdata);

/* Parse a config from a NUL-terminated buffer. On success returns a malloc'd
 * extractor_t (free with extractor_free) and *err is left NULL. On a parse
 * error returns NULL and, if err is non-NULL, sets *err to a malloc'd
 * "path:line: message" diagnostic the caller must free. `path` is recorded for
 * diagnostics and may be NULL. */
extractor_t *extractor_parse(const char *text, const char *path, char **err);

void extractor_free(extractor_t *ex);

/* True if any of the config's `match` patterns matches `url`. */
int extractor_matches(const extractor_t *ex, const char *url);

/* Run the config against `page_url`, fetching `get` bodies through `fetch`.
 * On success the resolved, absolute media URL is stored in *out_media_url
 * (malloc'd, owned by the caller) and 0 is returned. On failure returns a
 * negative value and, if err is non-NULL, sets *err to a malloc'd diagnostic
 * the caller must free; *out_media_url is left NULL. */
int extractor_run(const extractor_t *ex, const char *page_url,
		  extractor_fetch_fn fetch, void *userdata,
		  char **out_media_url, char **err);

/* True (1) if the config has a `list` directive (series mode), else 0. */
int extractor_has_list(const extractor_t *ex);

/* Series mode: evaluate the directives up to the first `list`, capture ALL of
 * its group-1 matches, resolve each relative->absolute against `page_url`,
 * dedup exact duplicates preserving order, and return the ordered episode-page
 * URL array. On success stores a malloc'd array of malloc'd strings in *urls
 * and the count in *n, returns 0. On failure returns negative and, if err is
 * non-NULL, sets *err to a malloc'd diagnostic the caller frees; the urls/n
 * outputs are left NULL/0. The array is freed with extractor_free_urls. */
int extractor_list_episodes(const extractor_t *ex, const char *page_url,
			    extractor_fetch_fn fetch, void *userdata,
			    char ***urls, size_t *n, char **err);

/* Free an episode-URL array returned by extractor_list_episodes /
 * extractor_resolve_series. Safe on NULL. */
void extractor_free_urls(char **urls, size_t n);

/* Parse an --episodes spec ("1,3-5,8": comma list of 1-based numbers and N-M
 * ranges) selecting from `count` available episodes. Allocates a `count`-long
 * byte array in *out_sel where index i is 1 if episode i+1 is selected, else 0,
 * and stores the number of selected episodes in *out_nsel. Returns 0 on success
 * (caller frees *out_sel), -1 on a malformed spec or any out-of-range number
 * (with *out_sel left NULL); on -1, if errbuf is non-NULL a short message is
 * written there. `count` must be > 0. */
int extractor_parse_episodes(const char *spec, size_t count,
			     unsigned char **out_sel, size_t *out_nsel,
			     char *errbuf, size_t errlen);

/* Resolve one episode of a series. Discovers the config that matches the SERIES
 * `page_url` (the episode URLs themselves usually do not match the series
 * `match`), then runs its per-episode pipeline with the builtin {url} bound to
 * `episode_url` (the engine skips the `list` directive). On success stores the
 * resolved media URL in *out_media_url (malloc'd) and returns 0. On failure
 * returns negative; if a diagnostic is available it is printed to stderr.
 * `force_name`, if non-NULL, bypasses matching. */
int extractor_run_series_episode(const char *page_url, const char *episode_url,
				 const char *force_name,
				 extractor_fetch_fn fetch, void *userdata,
				 char **out_media_url);

/* Discover/force a config like extractor_resolve, but for series mode: build
 * the ordered episode-page URL list via extractor_list_episodes.
 *
 * Returns:
 *   1  matched a config with a `list` (urls/n hold the episode set)
 *   0  no config matched, or the matched config has no `list`
 *  <0  error (a diagnostic is printed to stderr); urls/n left NULL/0
 *
 * On the 0 and error paths the urls output is NULL and n is 0. `force_name`,
 * if non-NULL, bypasses matching and loads <force_name>.conf. */
int extractor_resolve_series(const char *page_url, const char *force_name,
			     extractor_fetch_fn fetch, void *userdata,
			     char ***urls, size_t *n);

/* Discover configs, pick the first whose `match` matches `page_url`, run it,
 * and store the resolved media URL in *out_media_url (malloc'd).
 *
 * Returns:
 *   1  matched and resolved (*out_media_url = malloc'd media URL)
 *   0  no config matched     (*out_media_url = malloc'd copy of page_url)
 *  <0  error (a diagnostic is printed to stderr); *out_media_url left NULL
 *
 * `force_name`, if non-NULL, bypasses matching and loads <force_name>.conf.
 * `fetch`/`userdata` provide HTTP access. */
int extractor_resolve(const char *page_url, const char *force_name,
		      extractor_fetch_fn fetch, void *userdata,
		      char **out_media_url);

/* Print discovered configs and their match patterns to stdout (--extract-list).
 * Returns 0 on success. */
int extractor_list(void);

/* Resolve a possibly-relative `ref` against absolute `base` into a malloc'd
 * absolute URL. Returns NULL on allocation failure or invalid input. Exposed
 * for unit testing. */
char *extractor_resolve_url(const char *base, const char *ref);

#endif				/* FLUX_EXTRACTOR_H */
