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

/* HLS (HTTP Live Streaming) downloader: parse an m3u8 playlist, pick a variant,
 * fetch segments in parallel, AES-128 decrypt, and assemble a single output
 * (optionally remuxed to .mp4 via ffmpeg).
 *
 * The parsing, variant selection and AES-128-CBC decryption are free-standing
 * (libc + OpenSSL EVP only) so they unit-test in isolation like url_glob /
 * extractor. The threaded HTTP download orchestration links against the rest of
 * Hyperflux (http_fetch, conf_t) and is compiled only inside the flux binary. */

#ifndef FLUX_HLS_H
#define FLUX_HLS_H

#include <stddef.h>
#include <stdint.h>

/* Caps. Playlists are bounded so a malformed/hostile manifest can't blow up
 * memory; these are generous for real-world streams. */
#define HLS_MAX_VARIANTS	64
#define HLS_MAX_SEGMENTS	100000

/* AES-128 key/IV block size. */
#define HLS_AES_BLOCK	16

/* Segment encryption method (from #EXT-X-KEY). */
typedef enum {
	HLS_ENC_NONE = 0,	/* METHOD=NONE or no key */
	HLS_ENC_AES_128,	/* METHOD=AES-128 */
	HLS_ENC_SAMPLE_AES,	/* METHOD=SAMPLE-AES -- unsupported */
} hls_enc_t;

/* One variant of a master playlist (#EXT-X-STREAM-INF). */
typedef struct {
	char *url;		/* resolved absolute URL of the media playlist */
	long bandwidth;		/* BANDWIDTH attribute (bps), 0 if absent */
	int width;		/* RESOLUTION width, 0 if absent */
	int height;		/* RESOLUTION height, 0 if absent */
	char *audio_group;	/* AUDIO group id, NULL if none */
} hls_variant_t;

/* An audio rendition (#EXT-X-MEDIA:TYPE=AUDIO). */
typedef struct {
	char *group_id;		/* GROUP-ID */
	char *url;		/* resolved absolute URI, NULL if inline-only */
	int is_default;		/* DEFAULT=YES */
} hls_audio_t;

/* Parsed master playlist. */
typedef struct {
	hls_variant_t variants[HLS_MAX_VARIANTS];
	size_t nvariants;
	hls_audio_t audios[HLS_MAX_VARIANTS];
	size_t naudios;
} hls_master_t;

/* One media segment (#EXTINF). */
typedef struct {
	char *url;		/* resolved absolute URL */
	double duration;	/* EXTINF seconds */
	hls_enc_t enc;		/* encryption in force for this segment */
	char *key_url;		/* AES key URI (resolved), NULL if none */
	uint8_t iv[HLS_AES_BLOCK];	/* IV for this segment */
	int have_iv;		/* 1 if iv[] is populated */
} hls_segment_t;

/* Parsed media playlist. */
typedef struct {
	hls_segment_t *segments;	/* malloc'd array */
	size_t nsegments;
	long media_sequence;		/* #EXT-X-MEDIA-SEQUENCE, default 0 */
	double total_duration;		/* sum of EXTINF */
	hls_enc_t enc;			/* method seen (for diagnostics) */
} hls_media_t;

/* ---- pure parsing / selection / crypto (unit-testable) ---------------- */

/* True if `url` looks like an HLS playlist: a path ending in .m3u8/.m3u (any
 * case), optionally followed by a query/fragment. Returns 0/1. */
int hls_is_playlist_url(const char *url);

/* True if the buffer begins with the mandatory #EXTM3U tag (after an optional
 * UTF-8 BOM and leading whitespace). Returns 0/1. */
int hls_is_m3u8(const char *text);

/* Parse a master playlist body. Segment/variant URLs are resolved absolute
 * against `base_url`. On success returns a malloc'd hls_master_t (free with
 * hls_master_free) and *err is NULL; on failure returns NULL and, if err is
 * non-NULL, sets *err to a malloc'd message the caller frees. */
hls_master_t *hls_parse_master(const char *text, const char *base_url,
			       char **err);
void hls_master_free(hls_master_t *m);

/* Parse a media playlist body. Returns a malloc'd hls_media_t (free with
 * hls_media_free) or NULL with *err set. `key_iv_dummy` is unused. */
hls_media_t *hls_parse_media(const char *text, const char *base_url,
			     char **err);
void hls_media_free(hls_media_t *m);

/* Select a variant index by quality preference. `quality` is "best", "worst",
 * or a target height like "720" (nearest-not-exceeding, else smallest). NULL or
 * empty means "best". Returns an index into m->variants, or -1 if none. */
int hls_select_variant(const hls_master_t *m, const char *quality);

/* AES-128-CBC decrypt `inlen` bytes of `in` using 16-byte `key` and `iv` into
 * `out` (must hold at least inlen + HLS_AES_BLOCK bytes, per the OpenSSL EVP
 * contract). On success writes the plaintext length to *outlen and returns 0;
 * returns -1 on any OpenSSL/padding error or non-block-aligned input. */
int hls_aes128_cbc_decrypt(const uint8_t *key, const uint8_t *iv,
			   const uint8_t *in, size_t inlen,
			   uint8_t *out, size_t *outlen);

/* Build the 16-byte big-endian IV for a segment from its media sequence number
 * (used when #EXT-X-KEY carries no explicit IV=). */
void hls_iv_from_sequence(uint64_t seq, uint8_t iv[HLS_AES_BLOCK]);

/* Parse a hex string ("0x...", any case) of exactly 16 bytes into iv. Returns 0
 * on success, -1 on a malformed/wrong-length value. */
int hls_parse_iv(const char *hex, uint8_t iv[HLS_AES_BLOCK]);

/* ---- full download (links against the rest of Hyperflux) -------------- */

#ifdef HLS_HAVE_FLUX
/* conf_t must already be visible here (include flux.h / conf.h before hls.h
 * in any translation unit that uses hls_download). */

/* Download an HLS stream from `m3u8_url` and assemble it into an output file.
 * `out_name` is the desired output path (may be empty: a name is derived).
 * `quality` selects the variant (see hls_select_variant). `mux_pref` is "mp4",
 * "ts", or NULL/"" (auto: mp4 if ffmpeg is in PATH, else ts).
 * Returns 0 on success, negative on failure. All temp files are removed on both
 * paths. */
int hls_download(conf_t *conf, const char *m3u8_url, const char *out_name,
		 const char *quality, const char *mux_pref);
#endif				/* HLS_HAVE_FLUX */

#endif				/* FLUX_HLS_H */
