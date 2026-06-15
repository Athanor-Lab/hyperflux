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

/* URL range/list expansion (curl- and bash-style globbing).
 *
 * Intentionally free-standing: depends only on the C standard library so it
 * can be unit-tested without the rest of Hyperflux. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>

#include "url_glob.h"

/* A parsed URL is a sequence of literal runs and pattern groups. A pattern
 * holds the list of strings it expands to. */
typedef struct {
	int is_pattern;
	const char *lit;	/* literal: borrowed slice of the source URL */
	size_t lit_len;
	char **alts;		/* pattern: nalts owned strings */
	size_t nalts;
} seg_t;

static char *
dupn(const char *s, size_t n)
{
	char *p = malloc(n + 1);

	if (!p)
		return NULL;
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static int
mul_overflow(size_t a, size_t b, size_t *out)
{
	if (b != 0 && a > SIZE_MAX / b)
		return 1;
	*out = a * b;
	return 0;
}

/* Parse exactly `len` decimal digits into a non-negative long. Returns -1 on
 * a non-digit, an empty slice, or a value past LONG_MAX. */
static int
parse_uint(const char *s, size_t len, long *out)
{
	unsigned long long v = 0;

	if (len == 0 || len > 19)
		return -1;
	for (size_t i = 0; i < len; i++) {
		if (!isdigit((unsigned char)s[i]))
			return -1;
		v = v * 10 + (unsigned)(s[i] - '0');
		if (v > (unsigned long long)LONG_MAX)
			return -1;
	}
	*out = (long)v;
	return 0;
}

static const char *
mem_find(const char *hay, size_t hlen, const char *needle)
{
	size_t nlen = strlen(needle);

	if (nlen == 0 || hlen < nlen)
		return NULL;
	for (size_t i = 0; i + nlen <= hlen; i++) {
		if (memcmp(hay + i, needle, nlen) == 0)
			return hay + i;
	}
	return NULL;
}

static void
free_alts(char **alts, size_t n)
{
	if (!alts)
		return;
	for (size_t i = 0; i < n; i++)
		free(alts[i]);
	free(alts);
}

/* Build the alternatives for a comma list. Caller guarantees a comma is
 * present, so the result always has at least two items (possibly empty). */
static int
build_list(const char *c, size_t clen, char ***alts_out, size_t *nalts_out)
{
	size_t nitems = 1;

	for (size_t i = 0; i < clen; i++)
		if (c[i] == ',')
			nitems++;
	if (nitems > URL_GLOB_MAX_URLS)
		return -1;

	char **alts = calloc(nitems, sizeof(*alts));
	if (!alts)
		return -1;

	size_t idx = 0, start = 0;
	for (size_t i = 0; i <= clen; i++) {
		if (i == clen || c[i] == ',') {
			alts[idx] = dupn(c + start, i - start);
			if (!alts[idx]) {
				free_alts(alts, idx);
				return -1;
			}
			idx++;
			start = i + 1;
		}
	}
	*alts_out = alts;
	*nalts_out = nitems;
	return 1;
}

/* Build the alternatives for a numeric or single-character range.
 * `open` is '{' (bash, ".." separator, may descend) or '[' (curl, "-"
 * separator with optional ":step", ascending only).
 * Returns 1 when built, 0 when the content is not a valid range (treat as
 * literal), -1 on overflow / too many results / out of memory. */
static int
build_range(char open, const char *c, size_t clen, char ***alts_out,
	    size_t *nalts_out)
{
	const char *start, *end, *step = NULL;
	size_t slen, elen, steplen = 0;

	if (open == '{') {
		const char *sep = mem_find(c, clen, "..");
		if (!sep)
			return 0;
		start = c;
		slen = (size_t)(sep - c);
		const char *rest = sep + 2;
		size_t restlen = clen - slen - 2;
		const char *sep2 = mem_find(rest, restlen, "..");
		if (sep2) {
			end = rest;
			elen = (size_t)(sep2 - rest);
			step = sep2 + 2;
			steplen = restlen - elen - 2;
		} else {
			end = rest;
			elen = restlen;
		}
	} else {
		const char *dash = memchr(c, '-', clen);
		if (!dash)
			return 0;
		start = c;
		slen = (size_t)(dash - c);
		const char *rest = dash + 1;
		size_t restlen = clen - slen - 1;
		const char *colon = memchr(rest, ':', restlen);
		if (colon) {
			end = rest;
			elen = (size_t)(colon - rest);
			step = colon + 1;
			steplen = restlen - elen - 1;
		} else {
			end = rest;
			elen = restlen;
		}
	}

	long stepv = 1;
	if (step) {
		if (parse_uint(step, steplen, &stepv) || stepv < 1)
			return 0;
	}

	long lo, hi;
	int numeric = (parse_uint(start, slen, &lo) == 0 &&
		       parse_uint(end, elen, &hi) == 0);
	int alpha = (slen == 1 && elen == 1 &&
		     isalpha((unsigned char)start[0]) &&
		     isalpha((unsigned char)end[0]));
	if (!numeric && alpha) {
		lo = (unsigned char)start[0];
		hi = (unsigned char)end[0];
	} else if (!numeric) {
		return 0;
	}

	int ascending = lo <= hi;
	if (open == '[' && !ascending)
		return 0;

	unsigned long span = ascending ? (unsigned long)(hi - lo)
				       : (unsigned long)(lo - hi);
	size_t count = (size_t)(span / (unsigned long)stepv) + 1;
	if (count > URL_GLOB_MAX_URLS)
		return -1;

	int width = 0;
	if (numeric) {
		int lead = (slen > 1 && start[0] == '0') ||
			   (elen > 1 && end[0] == '0');
		if (lead)
			width = (int)(slen > elen ? slen : elen);
	}

	char **alts = calloc(count, sizeof(*alts));
	if (!alts)
		return -1;

	for (size_t k = 0; k < count; k++) {
		long v = ascending ? lo + (long)k * stepv
				   : lo - (long)k * stepv;
		char buf[32];
		int n;
		if (alpha)
			n = snprintf(buf, sizeof(buf), "%c", (int)v);
		else
			n = snprintf(buf, sizeof(buf), "%0*ld", width, v);
		if (n < 0 || (size_t)n >= sizeof(buf)) {
			free_alts(alts, k);
			return -1;
		}
		alts[k] = dupn(buf, (size_t)n);
		if (!alts[k]) {
			free_alts(alts, k);
			return -1;
		}
	}
	*alts_out = alts;
	*nalts_out = count;
	return 1;
}

/* Dispatch a {...} or [...] group to the matching builder. */
static int
build_pattern(char open, const char *c, size_t clen, char ***alts,
	      size_t *nalts)
{
	if (open == '{') {
		if (mem_find(c, clen, ".."))
			return build_range(open, c, clen, alts, nalts);
		if (memchr(c, ',', clen))
			return build_list(c, clen, alts, nalts);
		return 0;
	}
	return build_range(open, c, clen, alts, nalts);
}

static void
free_segs(seg_t *segs, size_t n)
{
	if (!segs)
		return;
	for (size_t i = 0; i < n; i++)
		if (segs[i].is_pattern)
			free_alts(segs[i].alts, segs[i].nalts);
	free(segs);
}

int
url_glob(const char *src, size_t max_len, url_glob_t **out, size_t *count,
	 size_t *ncaps_out)
{
	*out = NULL;
	if (max_len == 0)
		return -1;

	size_t srclen = strlen(src);
	seg_t *segs = calloc(srclen + 1, sizeof(*segs));
	if (!segs)
		return -1;

	size_t nseg = 0, product = 1, ncaps = 0, lit_start = 0, i = 0;

	while (i < srclen) {
		char ch = src[i];
		if (ch != '{' && ch != '[') {
			i++;
			continue;
		}

		char close = (ch == '{') ? '}' : ']';
		size_t j = i + 1;
		while (j < srclen && src[j] != close)
			j++;

		char **alts = NULL;
		size_t nalts = 0;
		int r = 0;
		if (j < srclen)
			r = build_pattern(ch, src + i + 1, j - i - 1, &alts,
					  &nalts);
		if (r == -1)
			goto error;
		if (r == 0) {	/* unmatched or not a pattern: keep literal */
			i++;
			continue;
		}

		size_t np;
		if (mul_overflow(product, nalts, &np) || np > URL_GLOB_MAX_URLS) {
			free_alts(alts, nalts);
			goto error;
		}
		if (i > lit_start) {
			segs[nseg].is_pattern = 0;
			segs[nseg].lit = src + lit_start;
			segs[nseg].lit_len = i - lit_start;
			nseg++;
		}
		segs[nseg].is_pattern = 1;
		segs[nseg].alts = alts;
		segs[nseg].nalts = nalts;
		nseg++;
		product = np;
		ncaps++;
		i = j + 1;
		lit_start = i;
	}
	if (srclen > lit_start) {
		segs[nseg].is_pattern = 0;
		segs[nseg].lit = src + lit_start;
		segs[nseg].lit_len = srclen - lit_start;
		nseg++;
	}

	url_glob_t *items = calloc(product, sizeof(*items));
	char *buf = malloc(max_len);
	size_t *idx = calloc(ncaps + 1, sizeof(*idx));
	seg_t **pats = calloc(ncaps + 1, sizeof(*pats));
	if (!items || !buf || !idx || !pats)
		goto gen_error;

	for (size_t s = 0, k = 0; s < nseg; s++)
		if (segs[s].is_pattern)
			pats[k++] = &segs[s];

	for (size_t n = 0; n < product; n++) {
		size_t pos = 0, pk = 0;
		for (size_t s = 0; s < nseg; s++) {
			const char *txt;
			size_t len;
			if (segs[s].is_pattern) {
				txt = segs[s].alts[idx[pk++]];
				len = strlen(txt);
			} else {
				txt = segs[s].lit;
				len = segs[s].lit_len;
			}
			if (pos + len >= max_len)
				goto gen_error;
			memcpy(buf + pos, txt, len);
			pos += len;
		}
		items[n].url = dupn(buf, pos);
		if (!items[n].url)
			goto gen_error;
		if (ncaps) {
			items[n].caps = calloc(ncaps, sizeof(char *));
			if (!items[n].caps)
				goto gen_error;
			for (size_t k = 0; k < ncaps; k++) {
				const char *v = pats[k]->alts[idx[k]];
				items[n].caps[k] = dupn(v, strlen(v));
				if (!items[n].caps[k])
					goto gen_error;
			}
		}
		for (size_t k = ncaps; k-- > 0;) {
			if (++idx[k] < pats[k]->nalts)
				break;
			idx[k] = 0;
		}
	}

	free(buf);
	free(idx);
	free(pats);
	free_segs(segs, nseg);
	*out = items;
	*count = product;
	*ncaps_out = ncaps;
	return (int)product;

 gen_error:
	url_glob_free(items, product, ncaps);
	free(buf);
	free(idx);
	free(pats);
 error:
	free_segs(segs, nseg);
	return -1;
}

void
url_glob_free(url_glob_t *items, size_t count, size_t ncaps)
{
	if (!items)
		return;
	for (size_t i = 0; i < count; i++) {
		free(items[i].url);
		if (items[i].caps) {
			for (size_t k = 0; k < ncaps; k++)
				free(items[i].caps[k]);
			free(items[i].caps);
		}
	}
	free(items);
}
