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

/* Media extractor engine -- see extractor.h.
 *
 * Free-standing: libc + POSIX <regex.h> + <dirent.h> only. No Hyperflux
 * headers, so it builds and tests in isolation like url_glob. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <regex.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>		/* mkdir (extractor_user_dir) */
#include <sys/types.h>

#include "extractor.h"

/* Bound the variable store and interpolated string lengths so malformed or
 * hostile configs can't blow up memory. */
#define EXT_MAX_VALUE	(1024 * 1024)	/* 1 MiB per interpolated string */

/* ---- small helpers ---------------------------------------------------- */

static char *
ext_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p)
		memcpy(p, s, n);
	return p;
}

static char *
ext_strndup(const char *s, size_t n)
{
	char *p = malloc(n + 1);

	if (!p)
		return NULL;
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

/* Allocate a "path:line: msg" diagnostic into *err (if non-NULL). */
#ifdef __GNUC__
__attribute__((format(printf, 4, 5)))
#endif
static void
ext_set_err(char **err, const char *path, int line, const char *fmt, ...)
{
	if (!err)
		return;
	*err = NULL;

	char body[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);

	const char *p = path ? path : "<config>";
	size_t need = strlen(p) + 32 + strlen(body);
	char *out = malloc(need);
	if (!out)
		return;
	snprintf(out, need, "%s:%d: %s", p, line, body);
	*err = out;
}

/* ---- variable store --------------------------------------------------- */

typedef struct {
	char *name;
	char *value;	/* owned */
} ext_var_t;

typedef struct {
	ext_var_t vars[EXTRACTOR_MAX_VARS];
	size_t nvars;
} ext_store_t;

static const char *
store_get(const ext_store_t *st, const char *name, size_t namelen)
{
	for (size_t i = 0; i < st->nvars; i++)
		if (strlen(st->vars[i].name) == namelen &&
		    memcmp(st->vars[i].name, name, namelen) == 0)
			return st->vars[i].value;
	return NULL;
}

/* Insert/replace a variable. Takes ownership of `value`. Returns 0 on success,
 * -1 on overflow (value is freed) or OOM. */
static int
store_set(ext_store_t *st, const char *name, char *value)
{
	for (size_t i = 0; i < st->nvars; i++) {
		if (strcmp(st->vars[i].name, name) == 0) {
			free(st->vars[i].value);
			st->vars[i].value = value;
			return 0;
		}
	}
	if (st->nvars >= EXTRACTOR_MAX_VARS) {
		free(value);
		return -1;
	}
	char *dupname = ext_strdup(name);
	if (!dupname) {
		free(value);
		return -1;
	}
	st->vars[st->nvars].name = dupname;
	st->vars[st->nvars].value = value;
	st->nvars++;
	return 0;
}

static void
store_free(ext_store_t *st)
{
	for (size_t i = 0; i < st->nvars; i++) {
		free(st->vars[i].name);
		free(st->vars[i].value);
	}
	st->nvars = 0;
}

/* ---- {var} interpolation ---------------------------------------------- */

/* Append [src, src+len) to *buf at *pos, growing as needed. Returns 0 or -1. */
static int
buf_append(char **buf, size_t *cap, size_t *pos, const char *src, size_t len)
{
	if (len > EXT_MAX_VALUE || *pos > EXT_MAX_VALUE - len)
		return -1;	/* refuse pathological growth */
	if (*pos + len + 1 > *cap) {
		size_t ncap = *cap ? *cap : 64;
		while (ncap < *pos + len + 1) {
			if (ncap > EXT_MAX_VALUE)
				return -1;
			ncap *= 2;
		}
		char *p = realloc(*buf, ncap);
		if (!p)
			return -1;
		*buf = p;
		*cap = ncap;
	}
	memcpy(*buf + *pos, src, len);
	*pos += len;
	(*buf)[*pos] = '\0';
	return 0;
}

/* Interpolate {name} references in `tpl` using `st`. On an undefined variable
 * sets *err and returns NULL. Returns a malloc'd string otherwise. */
static char *
interpolate(const char *tpl, const ext_store_t *st, char **err)
{
	char *buf = NULL;
	size_t cap = 0, pos = 0;

	for (const char *p = tpl; *p;) {
		if (*p != '{') {
			const char *q = p;
			while (*q && *q != '{')
				q++;
			if (buf_append(&buf, &cap, &pos, p, (size_t)(q - p)) < 0)
				goto oom;
			p = q;
			continue;
		}
		/* find closing brace */
		const char *close = strchr(p + 1, '}');
		if (!close) {	/* lone '{' is a literal */
			if (buf_append(&buf, &cap, &pos, p, 1) < 0)
				goto oom;
			p++;
			continue;
		}
		size_t namelen = (size_t)(close - (p + 1));
		const char *val = store_get(st, p + 1, namelen);
		if (!val) {
			if (err) {
				char nm[128];
				size_t c = namelen < sizeof(nm) - 1
					? namelen : sizeof(nm) - 1;
				memcpy(nm, p + 1, c);
				nm[c] = '\0';
				ext_set_err(err, NULL, 0,
					    "undefined variable {%s}", nm);
			}
			free(buf);
			return NULL;
		}
		if (buf_append(&buf, &cap, &pos, val, strlen(val)) < 0)
			goto oom;
		p = close + 1;
	}
	if (!buf)	/* empty template */
		return ext_strdup("");
	return buf;

 oom:
	free(buf);
	if (err)
		ext_set_err(err, NULL, 0, "out of memory during interpolation");
	return NULL;
}

/* ---- POSIX ERE group-1 capture ---------------------------------------- */

/* Run `ere` over `src`, capturing group 1 into a malloc'd string at *out.
 * Returns 1 on a match with a group, 0 on no match (or no group), -1 on a
 * regex-compile error (message in *err). */
static int
regex_capture1(const char *ere, const char *src, char **out, char **err)
{
	regex_t re;
	regmatch_t m[2];
	int rc;

	*out = NULL;
	rc = regcomp(&re, ere, REG_EXTENDED);
	if (rc != 0) {
		if (err) {
			char ebuf[256];
			regerror(rc, &re, ebuf, sizeof(ebuf));
			ext_set_err(err, NULL, 0, "bad regex /%s/: %s",
				    ere, ebuf);
		}
		regfree(&re);
		return -1;
	}
	if (re.re_nsub < 1) {	/* a var/list regex must have a capture group () */
		if (err)
			ext_set_err(err, NULL, 0,
				    "regex /%s/ has no capture group ()", ere);
		regfree(&re);
		return -1;
	}

	rc = regexec(&re, src, 2, m, 0);
	if (rc != 0 || m[1].rm_so < 0) {
		regfree(&re);
		return 0;
	}

	size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
	char *cap = ext_strndup(src + m[1].rm_so, len);
	regfree(&re);
	if (!cap)
		return -1;
	*out = cap;
	return 1;
}

/* Run `ere` over `src` repeatedly, appending every group-1 match (malloc'd) to
 * a caller-owned growable array. Stops after `cap_max` matches. On a compile
 * error sets *err and returns -1. Returns the number of matches appended (>= 0)
 * otherwise. On any allocation failure frees what it added, sets *out=NULL,
 * *n=0, and returns -1. The empty-array (zero-match) case returns 0. */
static int
regex_capture_all(const char *ere, const char *src, size_t cap_max,
		  char ***out, size_t *n, char **err)
{
	regex_t re;
	regmatch_t m[2];
	int rc;

	*out = NULL;
	*n = 0;

	rc = regcomp(&re, ere, REG_EXTENDED);
	if (rc != 0) {
		if (err) {
			char ebuf[256];
			regerror(rc, &re, ebuf, sizeof(ebuf));
			ext_set_err(err, NULL, 0, "bad regex /%s/: %s",
				    ere, ebuf);
		}
		regfree(&re);
		return -1;
	}
	if (re.re_nsub < 1) {
		if (err)
			ext_set_err(err, NULL, 0,
				    "regex /%s/ has no capture group ()", ere);
		regfree(&re);
		return -1;
	}

	char **arr = NULL;
	size_t count = 0, cap = 0;
	const char *p = src;

	while (count < cap_max && regexec(&re, p, 2, m, 0) == 0) {
		if (m[1].rm_so >= 0) {
			size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
			char *item = ext_strndup(p + m[1].rm_so, len);
			if (!item)
				goto oom;
			if (count >= cap) {
				size_t ncap = cap ? cap * 2 : 16;
				char **na = realloc(arr, ncap * sizeof(*na));
				if (!na) {
					free(item);
					goto oom;
				}
				arr = na;
				cap = ncap;
			}
			arr[count++] = item;
		}
		/* Advance past this whole match; guard against a zero-width
		 * match (e.g. an ERE that can match empty) to avoid spinning. */
		regoff_t adv = m[0].rm_eo;
		if (adv <= 0)
			adv = 1;
		p += adv;
		if (!*p)
			break;
	}

	regfree(&re);
	*out = arr;
	*n = count;
	return (int)count;

 oom:
	regfree(&re);
	for (size_t i = 0; i < count; i++)
		free(arr[i]);
	free(arr);
	if (err)
		ext_set_err(err, NULL, 0, "out of memory capturing list");
	return -1;
}

/* ---- relative -> absolute URL resolution ------------------------------ */

/* Split an absolute URL into scheme://authority and the path. Returns the
 * length of the "scheme://authority" prefix, or 0 if `base` is not absolute. */
static size_t
url_authority_len(const char *base)
{
	const char *sep = strstr(base, "://");
	if (!sep)
		return 0;
	const char *auth = sep + 3;
	/* authority ends at the first '/', '?' or '#', or end of string */
	const char *end = auth + strcspn(auth, "/?#");
	return (size_t)(end - base);
}

char *
extractor_resolve_url(const char *base, const char *ref)
{
	if (!ref)
		return NULL;

	/* Absolute reference (has its own scheme): use as-is. */
	if (strstr(ref, "://") != NULL)
		return ext_strdup(ref);

	size_t authlen = url_authority_len(base);
	if (authlen == 0)	/* base not absolute: best effort, return ref */
		return ext_strdup(ref);

	/* Scheme-relative: //host/path */
	if (ref[0] == '/' && ref[1] == '/') {
		const char *sep = strstr(base, "://");
		size_t schemelen = (size_t)(sep - base) + 1;	/* include ':' */
		size_t reflen = strlen(ref);
		char *out = malloc(schemelen + reflen + 1);
		if (!out)
			return NULL;
		memcpy(out, base, schemelen);
		memcpy(out + schemelen, ref, reflen + 1);
		return out;
	}

	/* Absolute path: replace everything after the authority. */
	if (ref[0] == '/') {
		size_t reflen = strlen(ref);
		char *out = malloc(authlen + reflen + 1);
		if (!out)
			return NULL;
		memcpy(out, base, authlen);
		memcpy(out + authlen, ref, reflen + 1);
		return out;
	}

	/* Relative path: keep base up to and including the last '/' of its
	 * path (after the authority), then append ref. */
	const char *path = base + authlen;
	const char *q = path + strcspn(path, "?#");	/* drop query/fragment */
	const char *lastslash = NULL;
	for (const char *c = path; c < q; c++)
		if (*c == '/')
			lastslash = c;

	size_t dirlen;
	if (lastslash)
		dirlen = (size_t)(lastslash - base) + 1;
	else
		dirlen = authlen;	/* no path slash: base authority + '/' */

	size_t reflen = strlen(ref);
	int need_slash = !lastslash;	/* insert a '/' after a bare authority */
	char *out = malloc(dirlen + need_slash + reflen + 1);
	if (!out)
		return NULL;
	memcpy(out, base, dirlen);
	if (need_slash)
		out[dirlen] = '/';
	memcpy(out + dirlen + need_slash, ref, reflen + 1);
	return out;
}

/* ---- parser ----------------------------------------------------------- */

/* Trim leading/trailing ASCII whitespace in-place, returning the new start. */
static char *
trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r')
		s++;
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
			 s[n - 1] == '\r'))
		s[--n] = '\0';
	return s;
}

/* Strip a trailing inline comment ("  # ...") not inside the directive value.
 * We keep it simple: a '#' that follows whitespace starts a comment. A '#' at
 * the very start of a token is handled by full-line comment detection. */
static void
strip_comment(char *s)
{
	int prev_ws = 1;	/* treat line start as preceded by whitespace */
	for (char *p = s; *p; p++) {
		if (*p == '#' && prev_ws) {
			*p = '\0';
			return;
		}
		prev_ws = (*p == ' ' || *p == '\t');
	}
}

/* Consume the next whitespace-delimited token; returns its start and advances
 * *cur past it, NUL-terminating in place. Returns NULL when none remain. */
static char *
next_token(char **cur)
{
	char *p = *cur;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return NULL;
	char *start = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	if (*p)
		*p++ = '\0';
	*cur = p;
	return start;
}

static void
free_directive(ext_directive_t *d)
{
	free(d->name);
	free(d->source);
	free(d->arg);
	for (size_t i = 0; i < d->nheaders; i++) {
		free(d->headers[i].key);
		free(d->headers[i].val);
	}
	d->nheaders = 0;
}

void
extractor_free(extractor_t *ex)
{
	if (!ex)
		return;
	free(ex->name);
	free(ex->path);
	for (size_t i = 0; i < ex->nmatches; i++)
		free(ex->matches[i]);
	for (size_t i = 0; i < ex->ndirectives; i++)
		free_directive(&ex->directives[i]);
	free(ex);
}

/* Parse a "var/list <name> <- <source> regex <ERE>" tail (cur points just
 * after the directive keyword). Fills d. Returns 0 or -1 (err set). */
static int
parse_var_like(ext_directive_t *d, char *cur, const char *path, int line,
	       char **err)
{
	char *name = next_token(&cur);
	char *arrow = next_token(&cur);
	char *source = next_token(&cur);
	char *kw = next_token(&cur);

	if (!name || !arrow || strcmp(arrow, "<-") != 0 || !source || !kw ||
	    strcmp(kw, "regex") != 0) {
		ext_set_err(err, path, line,
			    "expected '<name> <- <source> regex <ERE>'");
		return -1;
	}
	char *ere = trim(cur);
	if (!*ere) {
		ext_set_err(err, path, line, "missing regex");
		return -1;
	}
	d->name = ext_strdup(name);
	d->source = ext_strdup(source);
	d->arg = ext_strdup(ere);
	if (!d->name || !d->source || !d->arg) {
		ext_set_err(err, path, line, "out of memory");
		return -1;
	}
	return 0;
}

extractor_t *
extractor_parse(const char *text, const char *path, char **err)
{
	if (err)
		*err = NULL;
	if (!text) {
		ext_set_err(err, path, 0, "empty config");
		return NULL;
	}

	extractor_t *ex = calloc(1, sizeof(*ex));
	if (!ex) {
		ext_set_err(err, path, 0, "out of memory");
		return NULL;
	}
	if (path) {
		ex->path = ext_strdup(path);
		if (!ex->path) {
			extractor_free(ex);
			ext_set_err(err, path, 0, "out of memory");
			return NULL;
		}
	}

	const char *lineptr = text;
	int line = 0;
	int have_output = 0;
	ext_directive_t *last_get = NULL;	/* for indented `header` lines */

	while (*lineptr) {
		line++;
		const char *nl = strchr(lineptr, '\n');
		size_t llen = nl ? (size_t)(nl - lineptr) : strlen(lineptr);

		char *raw = ext_strndup(lineptr, llen);
		if (!raw) {
			ext_set_err(err, path, line, "out of memory");
			goto fail;
		}
		lineptr = nl ? nl + 1 : lineptr + llen;

		int indented = (raw[0] == ' ' || raw[0] == '\t');
		strip_comment(raw);
		char *cur = raw;
		char *kw = next_token(&cur);
		if (!kw) {	/* blank or comment-only line */
			free(raw);
			continue;
		}

		/* Indented `header K=V` lines attach to the preceding `get`. */
		if (indented && strcmp(kw, "header") == 0) {
			if (!last_get) {
				ext_set_err(err, path, line,
					    "'header' without a preceding 'get'");
				free(raw);
				goto fail;
			}
			char *kv = trim(cur);
			char *eq = strchr(kv, '=');
			if (!eq || eq == kv) {
				ext_set_err(err, path, line,
					    "header must be 'K=V'");
				free(raw);
				goto fail;
			}
			if (last_get->nheaders >= EXTRACTOR_MAX_HEADERS) {
				ext_set_err(err, path, line,
					    "too many headers (max %d)",
					    EXTRACTOR_MAX_HEADERS);
				free(raw);
				goto fail;
			}
			*eq = '\0';
			char *k = trim(kv);
			char *v = trim(eq + 1);
			ext_header_t *h =
				&last_get->headers[last_get->nheaders];
			h->key = ext_strdup(k);
			h->val = ext_strdup(v);
			if (!h->key || !h->val) {
				free(h->key);
				free(h->val);
				ext_set_err(err, path, line, "out of memory");
				free(raw);
				goto fail;
			}
			last_get->nheaders++;
			free(raw);
			continue;
		}

		/* A non-header directive ends the header-attach context. */
		if (strcmp(kw, "name") == 0) {
			char *v = trim(cur);
			if (!*v) {
				ext_set_err(err, path, line, "name needs a value");
				free(raw);
				goto fail;
			}
			free(ex->name);
			ex->name = ext_strdup(v);
			if (!ex->name) {
				ext_set_err(err, path, line, "out of memory");
				free(raw);
				goto fail;
			}
			last_get = NULL;
		} else if (strcmp(kw, "match") == 0) {
			char *v = trim(cur);
			if (!*v) {
				ext_set_err(err, path, line, "match needs a pattern");
				free(raw);
				goto fail;
			}
			if (ex->nmatches >= EXTRACTOR_MAX_MATCHES) {
				ext_set_err(err, path, line,
					    "too many match lines (max %d)",
					    EXTRACTOR_MAX_MATCHES);
				free(raw);
				goto fail;
			}
			/* Validate the ERE at parse time. */
			regex_t tre;
			int rc = regcomp(&tre, v, REG_EXTENDED | REG_NOSUB);
			if (rc != 0) {
				char ebuf[256];
				regerror(rc, &tre, ebuf, sizeof(ebuf));
				regfree(&tre);
				ext_set_err(err, path, line,
					    "bad match regex: %s", ebuf);
				free(raw);
				goto fail;
			}
			regfree(&tre);
			ex->matches[ex->nmatches] = ext_strdup(v);
			if (!ex->matches[ex->nmatches]) {
				ext_set_err(err, path, line, "out of memory");
				free(raw);
				goto fail;
			}
			ex->nmatches++;
			last_get = NULL;
		} else if (strcmp(kw, "var") == 0 || strcmp(kw, "list") == 0) {
			if (ex->ndirectives >= EXTRACTOR_MAX_DIRECTIVES) {
				ext_set_err(err, path, line,
					    "too many directives (max %d)",
					    EXTRACTOR_MAX_DIRECTIVES);
				free(raw);
				goto fail;
			}
			ext_directive_t *d = &ex->directives[ex->ndirectives];
			d->kind = (kw[0] == 'l') ? EXT_DIR_LIST : EXT_DIR_VAR;
			if (parse_var_like(d, cur, path, line, err) < 0) {
				free_directive(d);
				free(raw);
				goto fail;
			}
			ex->ndirectives++;
			last_get = NULL;
		} else if (strcmp(kw, "get") == 0) {
			if (ex->ndirectives >= EXTRACTOR_MAX_DIRECTIVES) {
				ext_set_err(err, path, line,
					    "too many directives (max %d)",
					    EXTRACTOR_MAX_DIRECTIVES);
				free(raw);
				goto fail;
			}
			char *name = next_token(&cur);
			char *arrow = next_token(&cur);
			char *urltpl = trim(cur);
			if (!name || !arrow || strcmp(arrow, "<-") != 0 ||
			    !*urltpl) {
				ext_set_err(err, path, line,
					    "expected 'get <name> <- <url>'");
				free(raw);
				goto fail;
			}
			ext_directive_t *d = &ex->directives[ex->ndirectives];
			d->kind = EXT_DIR_GET;
			d->name = ext_strdup(name);
			d->arg = ext_strdup(urltpl);
			if (!d->name || !d->arg) {
				free_directive(d);
				ext_set_err(err, path, line, "out of memory");
				free(raw);
				goto fail;
			}
			ex->ndirectives++;
			last_get = d;
		} else if (strcmp(kw, "output") == 0) {
			if (ex->ndirectives >= EXTRACTOR_MAX_DIRECTIVES) {
				ext_set_err(err, path, line,
					    "too many directives (max %d)",
					    EXTRACTOR_MAX_DIRECTIVES);
				free(raw);
				goto fail;
			}
			char *tpl = trim(cur);
			if (!*tpl) {
				ext_set_err(err, path, line,
					    "output needs a template");
				free(raw);
				goto fail;
			}
			ext_directive_t *d = &ex->directives[ex->ndirectives];
			d->kind = EXT_DIR_OUTPUT;
			d->arg = ext_strdup(tpl);
			if (!d->arg) {
				ext_set_err(err, path, line, "out of memory");
				free(raw);
				goto fail;
			}
			ex->ndirectives++;
			last_get = NULL;
		} else {
			ext_set_err(err, path, line, "unknown directive '%s'", kw);
			free(raw);
			goto fail;
		}
		free(raw);
	}

	if (!ex->name) {
		ext_set_err(err, path, 0, "config has no 'name'");
		goto fail;
	}

	for (size_t i = 0; i < ex->ndirectives; i++)
		if (ex->directives[i].kind == EXT_DIR_OUTPUT)
			have_output = 1;
	if (!have_output) {
		ext_set_err(err, path, 0, "config has no 'output'");
		goto fail;
	}

	return ex;

 fail:
	extractor_free(ex);
	return NULL;
}

int
extractor_matches(const extractor_t *ex, const char *url)
{
	if (!ex || !url)
		return 0;
	for (size_t i = 0; i < ex->nmatches; i++) {
		regex_t re;
		if (regcomp(&re, ex->matches[i], REG_EXTENDED | REG_NOSUB) != 0)
			continue;	/* validated at parse, skip if anomalous */
		int rc = regexec(&re, url, 0, NULL, 0);
		regfree(&re);
		if (rc == 0)
			return 1;
	}
	return 0;
}

/* ---- engine ----------------------------------------------------------- */

int
extractor_run(const extractor_t *ex, const char *page_url,
	      extractor_fetch_fn fetch, void *userdata,
	      char **out_media_url, char **err)
{
	if (out_media_url)
		*out_media_url = NULL;
	if (err)
		*err = NULL;
	if (!ex || !page_url || !out_media_url)
		return -1;

	ext_store_t st;
	memset(&st, 0, sizeof(st));

	/* The builtin {url} tracks the current page being processed. */
	char *urlcopy = ext_strdup(page_url);
	if (!urlcopy) {
		ext_set_err(err, ex->path, 0, "out of memory");
		return -1;
	}
	if (store_set(&st, "url", urlcopy) < 0) {
		ext_set_err(err, ex->path, 0, "variable store full");
		store_free(&st);
		return -1;
	}

	int ret = -1;

	/* Series mode: directives up to and including the first 'list' are
	 * series setup, run once on the series page by extractor_list_episodes.
	 * A per-episode run executes only the post-'list' pipeline with {url}
	 * bound to the episode. With no 'list', every directive runs. See #F4. */
	size_t list_idx = ex->ndirectives;
	for (size_t i = 0; i < ex->ndirectives; i++)
		if (ex->directives[i].kind == EXT_DIR_LIST) {
			list_idx = i;
			break;
		}

	for (size_t i = 0; i < ex->ndirectives; i++) {
		const ext_directive_t *d = &ex->directives[i];

		/* Skip series-setup directives (at or before the first 'list'). */
		if (i <= list_idx && list_idx < ex->ndirectives)
			continue;

		if (d->kind == EXT_DIR_VAR) {
			const char *src = store_get(&st, d->source,
						    strlen(d->source));
			if (!src) {
				ext_set_err(err, ex->path, 0,
					    "var '%s': undefined source '%s'",
					    d->name, d->source);
				goto done;
			}
			/* Interpolate {var}s in the regex before it compiles. */
			char *ere = interpolate(d->arg, &st, err);
			if (!ere)
				goto done;
			char *cap = NULL;
			int r = regex_capture1(ere, src, &cap, err);
			if (r < 0) {
				free(ere);
				goto done;
			}
			if (r == 0) {
				ext_set_err(err, ex->path, 0,
					    "var '%s': regex /%s/ did not match source '%s'",
					    d->name, ere, d->source);
				free(ere);
				goto done;
			}
			free(ere);
			if (store_set(&st, d->name, cap) < 0) {
				ext_set_err(err, ex->path, 0,
					    "var '%s': store full", d->name);
				goto done;
			}
		} else if (d->kind == EXT_DIR_GET) {
			if (!fetch) {
				ext_set_err(err, ex->path, 0,
					    "get '%s': no HTTP fetcher available",
					    d->name);
				goto done;
			}
			char *url = interpolate(d->arg, &st, err);
			if (!url)
				goto done;
			/* Resolve relative GET targets against {url}. */
			const char *base = store_get(&st, "url", 3);
			char *absurl = extractor_resolve_url(base ? base : "",
							     url);
			free(url);
			if (!absurl) {
				ext_set_err(err, ex->path, 0,
					    "get '%s': out of memory", d->name);
				goto done;
			}

			/* Interpolate header values. */
			ext_header_t hdrs[EXTRACTOR_MAX_HEADERS];
			size_t nh = 0;
			int hdr_err = 0;
			for (size_t h = 0; h < d->nheaders; h++) {
				char *hv = interpolate(d->headers[h].val, &st,
						       err);
				if (!hv) {
					hdr_err = 1;
					break;
				}
				hdrs[nh].key = d->headers[h].key;	/* borrow */
				hdrs[nh].val = hv;			/* owned */
				nh++;
			}
			if (hdr_err) {
				for (size_t h = 0; h < nh; h++)
					free(hdrs[h].val);
				free(absurl);
				goto done;
			}

			char *body = NULL;
			int fr = fetch(absurl, hdrs, nh, &body, userdata);
			for (size_t h = 0; h < nh; h++)
				free(hdrs[h].val);
			if (fr < 0 || !body) {
				free(body);
				ext_set_err(err, ex->path, 0,
					    "get '%s': fetch failed for %s",
					    d->name, absurl);
				free(absurl);
				goto done;
			}
			free(absurl);
			if (store_set(&st, d->name, body) < 0) {	/* owns body */
				ext_set_err(err, ex->path, 0,
					    "get '%s': store full", d->name);
				goto done;
			}
		} else if (d->kind == EXT_DIR_OUTPUT) {
			char *media = interpolate(d->arg, &st, err);
			if (!media)
				goto done;
			const char *base = store_get(&st, "url", 3);
			char *absmedia = extractor_resolve_url(base ? base : "",
							       media);
			free(media);
			if (!absmedia) {
				ext_set_err(err, ex->path, 0,
					    "output: out of memory");
				goto done;
			}
			*out_media_url = absmedia;
			ret = 0;
			goto done;
		}
	}

	/* No output directive ran (should be caught at parse). */
	ext_set_err(err, ex->path, 0, "config produced no output");

 done:
	store_free(&st);
	return ret;
}

/* ---- series mode ------------------------------------------------------ */

int
extractor_has_list(const extractor_t *ex)
{
	if (!ex)
		return 0;
	for (size_t i = 0; i < ex->ndirectives; i++)
		if (ex->directives[i].kind == EXT_DIR_LIST)
			return 1;
	return 0;
}

void
extractor_free_urls(char **urls, size_t n)
{
	if (!urls)
		return;
	for (size_t i = 0; i < n; i++)
		free(urls[i]);
	free(urls);
}

/* Parse a non-negative integer at *p (advancing it). Returns -1 if no digit is
 * present or the value overflows our episode cap. */
static long
parse_uint(const char **p)
{
	const char *s = *p;
	if (!isdigit((unsigned char)*s))
		return -1;
	long v = 0;
	while (isdigit((unsigned char)*s)) {
		v = v * 10 + (*s - '0');
		if (v > EXTRACTOR_MAX_EPISODES + 1)	/* clamp; range-checked below */
			v = EXTRACTOR_MAX_EPISODES + 1;
		s++;
	}
	*p = s;
	return v;
}

int
extractor_parse_episodes(const char *spec, size_t count,
			 unsigned char **out_sel, size_t *out_nsel,
			 char *errbuf, size_t errlen)
{
	if (out_sel)
		*out_sel = NULL;
	if (out_nsel)
		*out_nsel = 0;
	if (errbuf && errlen)
		errbuf[0] = '\0';
	if (!spec || !out_sel || !out_nsel || count == 0)
		return -1;

	unsigned char *sel = calloc(count, 1);
	if (!sel)
		return -1;

	const char *p = spec;
	int ok = 1;
	while (*p && ok) {
		while (*p == ' ' || *p == '\t')
			p++;
		long a = parse_uint(&p);
		if (a < 1 || (size_t)a > count) {
			ok = 0;
			break;
		}
		long b = a;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '-') {	/* a range "a-b" */
			p++;
			while (*p == ' ' || *p == '\t')
				p++;
			b = parse_uint(&p);
			if (b < a || (size_t)b > count) {
				ok = 0;
				break;
			}
		}
		for (long i = a; i <= b; i++)
			sel[i - 1] = 1;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '\0')
			break;
		ok = 0;	/* unexpected trailing junk */
	}

	if (!ok) {
		free(sel);
		if (errbuf && errlen)
			snprintf(errbuf, errlen,
				 "bad --episodes spec '%s' (1-based, 1-%zu)",
				 spec, count);
		return -1;
	}

	size_t nsel = 0;
	for (size_t i = 0; i < count; i++)
		if (sel[i])
			nsel++;
	if (nsel == 0) {	/* e.g. an empty/all-whitespace spec */
		free(sel);
		if (errbuf && errlen)
			snprintf(errbuf, errlen, "empty --episodes selection");
		return -1;
	}

	*out_sel = sel;
	*out_nsel = nsel;
	return 0;
}

/* Resolve every raw[i] relative->absolute against `base`, dedup exact
 * duplicates preserving first-seen order, and store the malloc'd result array
 * in *urls with its count in *n. Returns 0 on success, -1 on OOM (outputs left
 * untouched on failure). */
static int
resolve_dedup_list(const char *base, char **raw, size_t nraw,
		   char ***urls, size_t *n)
{
	char **out = calloc(nraw, sizeof(*out));
	if (!out)
		return -1;
	size_t nout = 0;
	for (size_t i = 0; i < nraw; i++) {
		char *a = extractor_resolve_url(base ? base : "", raw[i]);
		if (!a) {
			extractor_free_urls(out, nout);
			return -1;
		}
		int dup = 0;
		for (size_t j = 0; j < nout; j++)
			if (strcmp(out[j], a) == 0) {
				dup = 1;
				break;
			}
		if (dup)
			free(a);
		else
			out[nout++] = a;
	}
	*urls = out;
	*n = nout;
	return 0;
}

int
extractor_list_episodes(const extractor_t *ex, const char *page_url,
			extractor_fetch_fn fetch, void *userdata,
			char ***urls, size_t *n, char **err)
{
	if (urls)
		*urls = NULL;
	if (n)
		*n = 0;
	if (err)
		*err = NULL;
	if (!ex || !page_url || !urls || !n)
		return -1;

	ext_store_t st;
	memset(&st, 0, sizeof(st));

	/* The builtin {url} is the series page being listed. */
	char *urlcopy = ext_strdup(page_url);
	if (!urlcopy) {
		ext_set_err(err, ex->path, 0, "out of memory");
		return -1;
	}
	if (store_set(&st, "url", urlcopy) < 0) {
		ext_set_err(err, ex->path, 0, "variable store full");
		store_free(&st);
		return -1;
	}

	int ret = -1;
	char **raw = NULL;	/* captured (possibly relative) list items */
	size_t nraw = 0;

	/* Evaluate var/get directives up to the first 'list', then capture it.
	 * Anything after the list (the per-episode pipeline) is not run here. */
	for (size_t i = 0; i < ex->ndirectives; i++) {
		const ext_directive_t *d = &ex->directives[i];

		if (d->kind == EXT_DIR_LIST) {
			const char *src = store_get(&st, d->source,
						    strlen(d->source));
			if (!src) {
				ext_set_err(err, ex->path, 0,
					    "list '%s': undefined source '%s'",
					    d->name, d->source);
				goto done;
			}
			/* Interpolate {var}s (e.g. a slug) before the regex
			 * compiles, so the list can be anchored to this series. */
			char *ere = interpolate(d->arg, &st, err);
			if (!ere)
				goto done;
			char *cerr = NULL;
			int rc = regex_capture_all(ere, src,
						   EXTRACTOR_MAX_EPISODES,
						   &raw, &nraw, &cerr);
			if (rc < 0) {
				if (err)
					*err = cerr;
				else
					free(cerr);
				free(ere);
				goto done;
			}
			if (nraw == 0) {
				ext_set_err(err, ex->path, 0,
					    "list '%s': regex /%s/ matched no items in source '%s'",
					    d->name, ere, d->source);
				free(ere);
				goto done;
			}
			free(ere);
			break;	/* one 'list' per config; stop at the first */
		} else if (d->kind == EXT_DIR_VAR) {
			const char *src = store_get(&st, d->source,
						    strlen(d->source));
			if (!src) {
				ext_set_err(err, ex->path, 0,
					    "var '%s': undefined source '%s'",
					    d->name, d->source);
				goto done;
			}
			/* Interpolate {var}s in the regex before it compiles. */
			char *ere = interpolate(d->arg, &st, err);
			if (!ere)
				goto done;
			char *cap = NULL;
			int r = regex_capture1(ere, src, &cap, err);
			if (r < 0) {
				free(ere);
				goto done;
			}
			if (r == 0) {
				ext_set_err(err, ex->path, 0,
					    "var '%s': regex /%s/ did not match source '%s'",
					    d->name, ere, d->source);
				free(ere);
				goto done;
			}
			free(ere);
			if (store_set(&st, d->name, cap) < 0) {
				ext_set_err(err, ex->path, 0,
					    "var '%s': store full", d->name);
				goto done;
			}
		} else if (d->kind == EXT_DIR_GET) {
			if (!fetch) {
				ext_set_err(err, ex->path, 0,
					    "get '%s': no HTTP fetcher available",
					    d->name);
				goto done;
			}
			char *url = interpolate(d->arg, &st, err);
			if (!url)
				goto done;
			const char *base = store_get(&st, "url", 3);
			char *absurl = extractor_resolve_url(base ? base : "",
							     url);
			free(url);
			if (!absurl) {
				ext_set_err(err, ex->path, 0,
					    "get '%s': out of memory", d->name);
				goto done;
			}

			ext_header_t hdrs[EXTRACTOR_MAX_HEADERS];
			size_t nh = 0;
			int hdr_err = 0;
			for (size_t h = 0; h < d->nheaders; h++) {
				char *hv = interpolate(d->headers[h].val, &st,
						       err);
				if (!hv) {
					hdr_err = 1;
					break;
				}
				hdrs[nh].key = d->headers[h].key;	/* borrow */
				hdrs[nh].val = hv;			/* owned */
				nh++;
			}
			if (hdr_err) {
				for (size_t h = 0; h < nh; h++)
					free(hdrs[h].val);
				free(absurl);
				goto done;
			}

			char *body = NULL;
			int fr = fetch(absurl, hdrs, nh, &body, userdata);
			for (size_t h = 0; h < nh; h++)
				free(hdrs[h].val);
			if (fr < 0 || !body) {
				free(body);
				ext_set_err(err, ex->path, 0,
					    "get '%s': fetch failed for %s",
					    d->name, absurl);
				free(absurl);
				goto done;
			}
			free(absurl);
			if (store_set(&st, d->name, body) < 0) {
				ext_set_err(err, ex->path, 0,
					    "get '%s': store full", d->name);
				goto done;
			}
		}
		/* EXT_DIR_OUTPUT before the list is ignored in series listing. */
	}

	if (!raw) {	/* no 'list' directive present */
		ext_set_err(err, ex->path, 0, "config has no 'list' directive");
		goto done;
	}

	/* Resolve relative->absolute against the series page and dedup. */
	if (resolve_dedup_list(store_get(&st, "url", 3), raw, nraw,
			       urls, n) < 0) {
		ext_set_err(err, ex->path, 0, "out of memory");
		goto done;
	}
	ret = 0;

 done:
	extractor_free_urls(raw, nraw);
	store_free(&st);
	return ret;
}

/* ---- config discovery ------------------------------------------------- */

/* The directories scanned for *.conf, in precedence order. The first entry is
 * resolved at runtime from XDG_CONFIG_HOME / HOME. */
static const char *const ext_system_dirs[] = {
	"/usr/local/share/hyperflux/extractors",
	"/usr/share/hyperflux/extractors",
};

/* Build the user config dir into dst (XDG_CONFIG_HOME or ~/.config). Returns 1
 * on success, 0 if no home is known. */
static int
user_config_dir(char *dst, size_t len)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		int n = snprintf(dst, len, "%s/hyperflux/extractors", xdg);
		return n > 0 && (size_t)n < len;
	}
	const char *home = getenv("HOME");
	if (home && *home) {
		int n = snprintf(dst, len, "%s/.config/hyperflux/extractors",
				 home);
		return n > 0 && (size_t)n < len;
	}
	return 0;
}

/* Apply `fn` to every "<dir>/<entry>.conf" path, stopping early if fn returns
 * non-zero (the truthy value is returned). Hidden files are skipped. */
typedef int (*ext_dir_cb)(const char *path, void *ud);

static int
scan_dir(const char *dir, ext_dir_cb cb, void *ud)
{
	DIR *dp = opendir(dir);
	if (!dp)
		return 0;

	int rc = 0;
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		const char *nm = de->d_name;
		if (nm[0] == '.')
			continue;
		size_t nl = strlen(nm);
		if (nl < 6 || strcmp(nm + nl - 5, ".conf") != 0)
			continue;
		char path[4096];
		int n = snprintf(path, sizeof(path), "%s/%s", dir, nm);
		if (n <= 0 || (size_t)n >= sizeof(path))
			continue;
		rc = cb(path, ud);
		if (rc)
			break;
	}
	closedir(dp);
	return rc;
}

/* Read a whole file into a malloc'd NUL-terminated buffer, or NULL. */
static char *
read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;

	char *buf = NULL;
	size_t cap = 0, len = 0;
	for (;;) {
		if (len + 4096 + 1 > cap) {
			size_t ncap = cap ? cap * 2 : 8192;
			if (ncap > EXT_MAX_VALUE) {	/* config files are tiny */
				free(buf);
				fclose(fp);
				return NULL;
			}
			char *p = realloc(buf, ncap);
			if (!p) {
				free(buf);
				fclose(fp);
				return NULL;
			}
			buf = p;
			cap = ncap;
		}
		size_t got = fread(buf + len, 1, 4096, fp);
		len += got;
		if (got < 4096) {
			if (ferror(fp)) {
				free(buf);
				fclose(fp);
				return NULL;
			}
			break;
		}
	}
	fclose(fp);
	if (!buf) {	/* empty file */
		buf = malloc(1);
		if (!buf)
			return NULL;
	}
	buf[len] = '\0';
	return buf;
}

/* Context for resolve: the page URL to match, the forced config name, and the
 * winning parsed config. */
typedef struct {
	const char *page_url;
	const char *force_name;	/* NULL = match mode */
	extractor_t *found;
} ext_find_ctx_t;

/* Callback: parse a candidate; keep it if it matches (or its name equals the
 * forced name). Returns 1 (stop) when a config is chosen. */
static int
find_cb(const char *path, void *ud)
{
	ext_find_ctx_t *ctx = ud;

	char *text = read_file(path);
	if (!text)
		return 0;
	char *err = NULL;
	extractor_t *ex = extractor_parse(text, path, &err);
	free(text);
	if (!ex) {
		if (err) {
			fprintf(stderr, "flux: extractor parse error: %s\n",
				err);
			free(err);
		}
		return 0;
	}

	int keep;
	if (ctx->force_name)
		keep = ex->name && strcmp(ex->name, ctx->force_name) == 0;
	else
		keep = extractor_matches(ex, ctx->page_url);

	if (keep) {
		ctx->found = ex;
		return 1;
	}
	extractor_free(ex);
	return 0;
}

/* Run find_cb across all config dirs in precedence order. */
static extractor_t *
discover(const char *page_url, const char *force_name)
{
	ext_find_ctx_t ctx = { page_url, force_name, NULL };

	char userdir[4096];
	if (user_config_dir(userdir, sizeof(userdir)))
		if (scan_dir(userdir, find_cb, &ctx))
			return ctx.found;

	for (size_t i = 0; i < sizeof(ext_system_dirs) /
		     sizeof(ext_system_dirs[0]); i++)
		if (scan_dir(ext_system_dirs[i], find_cb, &ctx))
			return ctx.found;

	return NULL;
}

int
extractor_resolve(const char *page_url, const char *force_name,
		  extractor_fetch_fn fetch, void *userdata,
		  char **out_media_url)
{
	if (out_media_url)
		*out_media_url = NULL;
	if (!page_url || !out_media_url)
		return -1;

	extractor_t *ex = discover(page_url, force_name);
	if (!ex) {
		if (force_name) {
			fprintf(stderr,
				"flux: no extractor named '%s' found\n",
				force_name);
			return -1;
		}
		/* No match: pass the page URL through unchanged. */
		char *copy = ext_strdup(page_url);
		if (!copy)
			return -1;
		*out_media_url = copy;
		return 0;
	}

	char *err = NULL;
	char *media = NULL;
	int r = extractor_run(ex, page_url, fetch, userdata, &media, &err);
	extractor_free(ex);
	if (r < 0) {
		if (err) {
			fprintf(stderr, "flux: extractor: %s\n", err);
			free(err);
		} else {
			fprintf(stderr, "flux: extractor failed\n");
		}
		return -1;
	}
	*out_media_url = media;
	return 1;
}

int
extractor_resolve_series(const char *page_url, const char *force_name,
			 extractor_fetch_fn fetch, void *userdata,
			 char ***urls, size_t *n)
{
	if (urls)
		*urls = NULL;
	if (n)
		*n = 0;
	if (!page_url || !urls || !n)
		return -1;

	extractor_t *ex = discover(page_url, force_name);
	if (!ex) {
		if (force_name) {
			fprintf(stderr,
				"flux: no extractor named '%s' found\n",
				force_name);
			return -1;
		}
		return 0;	/* no config matched */
	}

	if (!extractor_has_list(ex)) {	/* matched, but a single-media config */
		extractor_free(ex);
		return 0;
	}

	char *err = NULL;
	char **list = NULL;
	size_t nlist = 0;
	int r = extractor_list_episodes(ex, page_url, fetch, userdata,
					&list, &nlist, &err);
	extractor_free(ex);
	if (r < 0) {
		if (err) {
			fprintf(stderr, "flux: extractor: %s\n", err);
			free(err);
		} else {
			fprintf(stderr, "flux: extractor: series listing failed\n");
		}
		return -1;
	}

	*urls = list;
	*n = nlist;
	return 1;
}

int
extractor_run_series_episode(const char *page_url, const char *episode_url,
			     const char *force_name,
			     extractor_fetch_fn fetch, void *userdata,
			     char **out_media_url)
{
	if (out_media_url)
		*out_media_url = NULL;
	if (!page_url || !episode_url || !out_media_url)
		return -1;

	/* Discover by the SERIES page URL: episode URLs typically don't match
	 * the series `match`, but they share the same config and pipeline. */
	extractor_t *ex = discover(page_url, force_name);
	if (!ex) {
		if (force_name)
			fprintf(stderr,
				"flux: no extractor named '%s' found\n",
				force_name);
		else
			fprintf(stderr, "flux: no extractor matched %s\n",
				page_url);
		return -1;
	}

	char *err = NULL;
	char *media = NULL;
	int r = extractor_run(ex, episode_url, fetch, userdata, &media, &err);
	extractor_free(ex);
	if (r < 0) {
		if (err) {
			fprintf(stderr, "flux: extractor: %s\n", err);
			free(err);
		} else {
			fprintf(stderr, "flux: extractor failed\n");
		}
		return -1;
	}
	*out_media_url = media;
	return 0;
}

/* ---- --extract-list --------------------------------------------------- */

static int
list_cb(const char *path, void *ud)
{
	(void)ud;
	char *text = read_file(path);
	if (!text)
		return 0;
	char *err = NULL;
	extractor_t *ex = extractor_parse(text, path, &err);
	free(text);
	if (!ex) {
		if (err) {
			fprintf(stderr, "  (parse error: %s)\n", err);
			free(err);
		}
		return 0;
	}
	printf("%-20s %s\n", ex->name ? ex->name : "(unnamed)", path);
	for (size_t i = 0; i < ex->nmatches; i++)
		printf("    match: %s\n", ex->matches[i]);
	if (ex->nmatches == 0)
		printf("    (no match patterns)\n");
	extractor_free(ex);
	return 0;	/* visit every config */
}

int
extractor_list(void)
{
	char userdir[4096];
	if (user_config_dir(userdir, sizeof(userdir))) {
		printf("%s\n", userdir);
		scan_dir(userdir, list_cb, NULL);
	}
	for (size_t i = 0; i < sizeof(ext_system_dirs) /
		     sizeof(ext_system_dirs[0]); i++) {
		printf("%s\n", ext_system_dirs[i]);
		scan_dir(ext_system_dirs[i], list_cb, NULL);
	}
	return 0;
}

/* Create `path` and any missing parents (mkdir -p), each at mode 0700. Mutates
 * the buffer in place while walking separators, then restores them. Returns 0
 * on success, -1 if any component could not be created. An existing directory
 * is not an error. */
static int
mkdir_p(char *path)
{
	if (!path || !*path)	/* enforce the precondition; callers change */
		return -1;
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0700) != 0 && errno != EEXIST) {
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int
extractor_user_dir(char *dst, size_t len, int create)
{
	if (!dst || len == 0)
		return 0;
	if (!user_config_dir(dst, len))
		return 0;
	if (create) {
		/* mkdir_p mutates a writable copy so we never clobber the
		 * caller's buffer on a path-walk failure. */
		char tmp[4096];
		int n = snprintf(tmp, sizeof(tmp), "%s", dst);
		if (n <= 0 || (size_t)n >= sizeof(tmp))
			return 0;
		if (mkdir_p(tmp) != 0)
			return 0;
	}
	return 1;
}
