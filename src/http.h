/*
  Hyperflux -- A lighter download accelerator for Linux and other Unices

  Copyright 2001-2007 Wilmer van der Gaast
  Copyright 2008      Y Giridhar Appaji Nag
  Copyright 2016      Phillip Berndt
  Copyright 2016      Sjjad Hashemian
  Copyright 2016      Stephen Thirlwall
  Copyright 2017      Antonio Quartulli
  Copyright 2017-2019 Ismael Luceno

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

/* HTTP control include file */

#ifndef FLUX_HTTP_H
#define FLUX_HTTP_H

typedef struct {
	char host[MAX_STRING];
	char auth[MAX_STRING];
	abuf_t request[1], headers[1];
	int port;
	int proto;		/* FTP through HTTP proxies */
	int proxy;
	char proxy_auth[MAX_STRING];
	off_t firstbyte;
	off_t lastbyte;
	int status;
	tcp_t tcp;
	char *local_if;
} http_t;

int http_connect(http_t *conn, int proto, char *proxy, char *host, int port,
		 char *user, char *pass, unsigned io_timeout);
void http_disconnect(http_t *conn);
void http_get(http_t *conn, char *lurl);
void http_request(http_t *conn, const char *method, char *lurl);
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif /* __GNUC__ */
void http_addheader(http_t *conn, const char *format, ...);
int http_exec(http_t *conn);
const char *http_header(const http_t *conn, const char *header);
void http_filename(const http_t *conn, char *filename);
off_t http_size(http_t *conn);
off_t http_size_from_range(http_t *conn);
void http_decode(char *s);

/* Perform a GET to `url`, following redirects, and read the full response body
 * into `body` (an abuf_t the caller must abuf_setup(.,ABUF_FREE) afterwards).
 * `headers` is an array of `nheaders` "Key: Value" strings appended verbatim
 * (no CRLF). Returns 0 on success (2xx with body), negative on any failure.
 * For small page/API bodies only, never the media download itself.
 *
 * Sends "Connection: close" and reads until EOF. Limitation: a chunked
 * Transfer-Encoding body is returned raw (chunk framing not stripped); the
 * page/API endpoints this targets reply with Content-Length. The body is
 * capped at 16 MiB. */
int http_fetch(conf_t *conf, const char *url, const char *const *headers,
	       size_t nheaders, abuf_t *body);

/* Like http_fetch, but also reports the exact body byte count via *out_len
 * (may be NULL). Use this for binary bodies (HLS segments, AES keys) whose
 * content can embed NUL bytes, where strlen() of body->p underreports. Body is
 * capped at 16 MiB; use http_fetch_max for larger media bodies. */
int http_fetch_len(conf_t *conf, const char *url, const char *const *headers,
		   size_t nheaders, abuf_t *body, size_t *out_len);

/* Like http_fetch_len, but caps the body at `max_body` bytes instead of the
 * 16 MiB default. Use for media segments that legitimately exceed 16 MiB
 * (high-bitrate 1080p, single-file VOD). `max_body` must be > 0. */
int http_fetch_max(conf_t *conf, const char *url, const char *const *headers,
		   size_t nheaders, abuf_t *body, size_t *out_len,
		   size_t max_body);

/* Probe a direct file's size via a HEAD request, reading Content-Length from
 * the response without downloading the body. Follows redirects. Returns 0 with
 * *out_size set (>= 0) on success, -1 when the server omits Content-Length or on
 * any error, so callers can fall back to a body-based probe. */
int http_probe_len(conf_t *conf, const char *url, long long *out_size);

#endif				/* FLUX_HTTP_H */
