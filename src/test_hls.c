/* Standalone unit tests for the HLS module. Build:
 *   cc -D_DEFAULT_SOURCE -Wall -Wextra -g \
 *      src/hls.c src/extractor.c src/test_hls.c -lcrypto -o /tmp/test_hls \
 *      && /tmp/test_hls
 *
 * The pure half of hls.c (playlist parse, variant select, AES-128 decrypt) is
 * compiled here without HLS_HAVE_FLUX, so no flux headers/HTTP are pulled in.
 * extractor.c provides extractor_resolve_url() used for segment URLs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hls.h"

static int failures;
static int checks;

#define CHECK(cond, msg)						\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			printf("FAIL %s\n", (msg));			\
			failures++;					\
		}							\
	} while (0)

#define CHECK_STR(got, want, msg)					\
	do {								\
		checks++;						\
		if (!(got) || strcmp((got), (want)) != 0) {		\
			printf("FAIL %s: got \"%s\" want \"%s\"\n",	\
			       (msg), (got) ? (got) : "(null)", (want));\
			failures++;					\
		}							\
	} while (0)

/* ---- URL kind / m3u8 sniff -------------------------------------------- */

static void
test_is_playlist_url(void)
{
	CHECK(hls_is_playlist_url("https://h/stream.m3u8"), "ends in .m3u8");
	CHECK(hls_is_playlist_url("https://h/a/b/master.M3U8"), "case-insensitive");
	CHECK(hls_is_playlist_url("https://h/p.m3u8?token=abc"), "m3u8 with query");
	CHECK(hls_is_playlist_url("https://h/p.m3u"), "ends in .m3u");
	CHECK(!hls_is_playlist_url("https://h/video.mp4"), "mp4 is not a playlist");
	CHECK(!hls_is_playlist_url("https://h/m3u8.txt"), "m3u8 not at path end");
	CHECK(!hls_is_playlist_url(NULL), "NULL is not a playlist");

	CHECK(hls_is_m3u8("#EXTM3U\n#EXT-X-VERSION:3\n"), "leading EXTM3U");
	CHECK(hls_is_m3u8("\xEF\xBB\xBF#EXTM3U\n"), "EXTM3U after BOM");
	CHECK(hls_is_m3u8("   \n#EXTM3U\n"), "EXTM3U after whitespace");
	CHECK(!hls_is_m3u8("hello\n#EXTM3U\n"), "EXTM3U not leading");
	CHECK(!hls_is_m3u8(NULL), "NULL body");
}

/* ---- master playlist parsing + variant selection ---------------------- */

static const char *MASTER =
	"#EXTM3U\n"
	"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"English\",DEFAULT=YES,"
	"URI=\"audio/eng.m3u8\"\n"
	"#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,AUDIO=\"aud\"\n"
	"low/index.m3u8\n"
	"#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x720,AUDIO=\"aud\"\n"
	"mid/index.m3u8\n"
	"#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,AUDIO=\"aud\"\n"
	"high/index.m3u8\n";

static void
test_master_parse(void)
{
	char *err = NULL;
	hls_master_t *m = hls_parse_master(MASTER, "https://cdn.x/v/master.m3u8",
					   &err);
	CHECK(m != NULL, "master parses");
	if (!m) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	CHECK(m->nvariants == 3, "three variants");
	CHECK(m->naudios == 1, "one audio rendition");
	CHECK(m->variants[0].height == 360, "variant 0 height");
	CHECK(m->variants[0].bandwidth == 800000, "variant 0 bandwidth");
	CHECK_STR(m->variants[1].url, "https://cdn.x/v/mid/index.m3u8",
		  "relative variant URL resolved");
	CHECK_STR(m->variants[2].audio_group, "aud", "variant audio group");
	CHECK_STR(m->audios[0].url, "https://cdn.x/v/audio/eng.m3u8",
		  "audio URI resolved");
	CHECK(m->audios[0].is_default == 1, "audio is default");

	/* best -> 1080p, worst -> 360p */
	int best = hls_select_variant(m, "best");
	CHECK(best >= 0 && m->variants[best].height == 1080, "best is 1080");
	int worst = hls_select_variant(m, "worst");
	CHECK(worst >= 0 && m->variants[worst].height == 360, "worst is 360");
	int def = hls_select_variant(m, NULL);
	CHECK(def >= 0 && m->variants[def].height == 1080, "default is best");

	/* exact height target */
	int h720 = hls_select_variant(m, "720");
	CHECK(h720 >= 0 && m->variants[h720].height == 720, "target 720 exact");
	/* not-exceeding: 800 -> 720 (largest <= 800) */
	int h800 = hls_select_variant(m, "800");
	CHECK(h800 >= 0 && m->variants[h800].height == 720, "target 800 -> 720");
	/* below all -> smallest */
	int h100 = hls_select_variant(m, "100");
	CHECK(h100 >= 0 && m->variants[h100].height == 360, "target 100 -> 360");

	hls_master_free(m);
}

/* ---- media playlist parsing ------------------------------------------- */

static const char *MEDIA_PLAIN =
	"#EXTM3U\n"
	"#EXT-X-VERSION:3\n"
	"#EXT-X-TARGETDURATION:10\n"
	"#EXT-X-MEDIA-SEQUENCE:0\n"
	"#EXTINF:9.009,\n"
	"seg0.ts\n"
	"#EXTINF:9.009,\n"
	"https://other.cdn/seg1.ts\n"
	"#EXTINF:3.003,\n"
	"sub/seg2.ts\n"
	"#EXT-X-ENDLIST\n";

static void
test_media_parse_plain(void)
{
	char *err = NULL;
	hls_media_t *m = hls_parse_media(MEDIA_PLAIN,
					 "https://cdn.x/v/720/index.m3u8", &err);
	CHECK(m != NULL, "plain media parses");
	if (!m) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	CHECK(m->nsegments == 3, "three segments");
	CHECK(m->media_sequence == 0, "media sequence 0");
	CHECK_STR(m->segments[0].url, "https://cdn.x/v/720/seg0.ts",
		  "relative segment URL resolved");
	CHECK_STR(m->segments[1].url, "https://other.cdn/seg1.ts",
		  "absolute segment URL kept");
	CHECK_STR(m->segments[2].url, "https://cdn.x/v/720/sub/seg2.ts",
		  "nested relative segment resolved");
	CHECK(m->segments[0].enc == HLS_ENC_NONE, "segment 0 unencrypted");
	/* total duration ~ 21.021 */
	CHECK(m->total_duration > 21.0 && m->total_duration < 21.05,
	      "total duration summed");
	hls_media_free(m);
}

/* ---- #EXT-X-KEY: AES-128 with explicit IV ----------------------------- */

static const char *MEDIA_AES_IV =
	"#EXTM3U\n"
	"#EXT-X-MEDIA-SEQUENCE:0\n"
	"#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\","
	"IV=0x000102030405060708090a0b0c0d0e0f\n"
	"#EXTINF:6.0,\n"
	"seg0.ts\n"
	"#EXTINF:6.0,\n"
	"seg1.ts\n";

static void
test_media_parse_aes_iv(void)
{
	char *err = NULL;
	hls_media_t *m = hls_parse_media(MEDIA_AES_IV,
					 "https://cdn.x/v/index.m3u8", &err);
	CHECK(m != NULL, "AES+IV media parses");
	if (!m) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	CHECK(m->nsegments == 2, "two segments");
	CHECK(m->segments[0].enc == HLS_ENC_AES_128, "segment 0 AES-128");
	CHECK_STR(m->segments[0].key_url, "https://cdn.x/v/key.bin",
		  "key URI resolved");
	CHECK(m->segments[0].have_iv == 1, "segment 0 has IV");
	uint8_t want_iv[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
	CHECK(memcmp(m->segments[0].iv, want_iv, 16) == 0, "explicit IV parsed");
	/* Both segments share the same explicit IV. */
	CHECK(memcmp(m->segments[1].iv, want_iv, 16) == 0, "segment 1 same IV");
	hls_media_free(m);
}

/* ---- #EXT-X-KEY: AES-128 with sequence-derived IV --------------------- */

static const char *MEDIA_AES_SEQ =
	"#EXTM3U\n"
	"#EXT-X-MEDIA-SEQUENCE:5\n"
	"#EXT-X-KEY:METHOD=AES-128,URI=\"https://k.x/k\"\n"
	"#EXTINF:6.0,\n"
	"seg5.ts\n"
	"#EXTINF:6.0,\n"
	"seg6.ts\n";

static void
test_media_parse_aes_seq_iv(void)
{
	char *err = NULL;
	hls_media_t *m = hls_parse_media(MEDIA_AES_SEQ,
					 "https://cdn.x/v/index.m3u8", &err);
	CHECK(m != NULL, "AES seq-IV media parses");
	if (!m) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	CHECK(m->media_sequence == 5, "media sequence 5");
	CHECK(m->segments[0].enc == HLS_ENC_AES_128, "AES-128 in force");
	CHECK_STR(m->segments[0].key_url, "https://k.x/k", "absolute key kept");
	/* IV for first segment = media sequence 5 (big-endian). */
	uint8_t want0[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,5 };
	uint8_t want1[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,6 };
	CHECK(memcmp(m->segments[0].iv, want0, 16) == 0, "seq IV = 5");
	CHECK(memcmp(m->segments[1].iv, want1, 16) == 0, "seq IV = 6");
	hls_media_free(m);
}

/* ---- METHOD=NONE and SAMPLE-AES rejection ----------------------------- */

static void
test_key_method_none(void)
{
	const char *body =
		"#EXTM3U\n"
		"#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n"
		"#EXTINF:6.0,\n"
		"a.ts\n"
		"#EXT-X-KEY:METHOD=NONE\n"
		"#EXTINF:6.0,\n"
		"b.ts\n";
	char *err = NULL;
	hls_media_t *m = hls_parse_media(body, "https://cdn.x/v/i.m3u8", &err);
	CHECK(m != NULL, "METHOD=NONE media parses");
	if (!m) { free(err); return; }
	CHECK(m->segments[0].enc == HLS_ENC_AES_128, "first seg encrypted");
	CHECK(m->segments[1].enc == HLS_ENC_NONE, "second seg clear after NONE");
	hls_media_free(m);
}

static void
test_sample_aes_rejected(void)
{
	const char *body =
		"#EXTM3U\n"
		"#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n"
		"#EXTINF:6.0,\n"
		"a.ts\n";
	char *err = NULL;
	hls_media_t *m = hls_parse_media(body, "https://cdn.x/v/i.m3u8", &err);
	CHECK(m == NULL, "SAMPLE-AES is rejected");
	CHECK(err && strstr(err, "SAMPLE-AES"), "error names SAMPLE-AES");
	free(err);
}

static void
test_ext_x_map_rejected(void)
{
	/* fMP4 init segment must be detected, not silently dropped. */
	const char *body =
		"#EXTM3U\n"
		"#EXT-X-VERSION:7\n"
		"#EXT-X-MAP:URI=\"init.mp4\"\n"
		"#EXTINF:6.0,\n"
		"seg0.m4s\n"
		"#EXTINF:6.0,\n"
		"seg1.m4s\n";
	char *err = NULL;
	hls_media_t *m = hls_parse_media(body, "https://cdn.x/v/i.m3u8", &err);
	CHECK(m == NULL, "fMP4 / #EXT-X-MAP is rejected");
	CHECK(err && strstr(err, "EXT-X-MAP"), "error names #EXT-X-MAP");
	free(err);
}

/* BANDWIDTH-only master (no RESOLUTION on any variant): a height target must
 * honor the cap intent and not return the highest-bandwidth stream. */
static const char *MASTER_BW_ONLY =
	"#EXTM3U\n"
	"#EXT-X-STREAM-INF:BANDWIDTH=800000\n"
	"low/index.m3u8\n"
	"#EXT-X-STREAM-INF:BANDWIDTH=2400000\n"
	"mid/index.m3u8\n"
	"#EXT-X-STREAM-INF:BANDWIDTH=5000000\n"
	"high/index.m3u8\n";

static void
test_select_bandwidth_only(void)
{
	char *err = NULL;
	hls_master_t *m = hls_parse_master(MASTER_BW_ONLY,
					   "https://cdn.x/v/master.m3u8", &err);
	CHECK(m != NULL, "bandwidth-only master parses");
	if (!m) {
		printf("  err: %s\n", err ? err : "(none)");
		free(err);
		return;
	}
	CHECK(m->nvariants == 3, "three bandwidth-only variants");
	/* best/worst still rank by bandwidth when heights are all 0. */
	int best = hls_select_variant(m, "best");
	CHECK(best >= 0 && m->variants[best].bandwidth == 5000000,
	      "bw-only best is highest bandwidth");
	int worst = hls_select_variant(m, "worst");
	CHECK(worst >= 0 && m->variants[worst].bandwidth == 800000,
	      "bw-only worst is lowest bandwidth");
	/* A numeric cap with no resolution data must not return the max stream. */
	int h720 = hls_select_variant(m, "720");
	CHECK(h720 >= 0 && m->variants[h720].bandwidth == 800000,
	      "bw-only height cap falls back to lowest bandwidth");
	hls_master_free(m);
}

static void
test_not_m3u8_rejected(void)
{
	char *err = NULL;
	hls_media_t *m = hls_parse_media("not a playlist\nseg.ts\n",
					 "https://h/i.m3u8", &err);
	CHECK(m == NULL, "non-#EXTM3U body rejected (media)");
	free(err);
	err = NULL;
	hls_master_t *mm = hls_parse_master("garbage\n", "https://h/m.m3u8", &err);
	CHECK(mm == NULL, "non-#EXTM3U body rejected (master)");
	free(err);
}

/* ---- IV parsing ------------------------------------------------------- */

static void
test_parse_iv(void)
{
	uint8_t iv[16];
	CHECK(hls_parse_iv("0x000102030405060708090A0B0C0D0E0F", iv) == 0,
	      "0x hex IV parses");
	uint8_t want[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
	CHECK(memcmp(iv, want, 16) == 0, "IV bytes correct");
	CHECK(hls_parse_iv("00112233445566778899aabbccddeeff", iv) == 0,
	      "bare hex IV parses");
	CHECK(hls_parse_iv("0x00", iv) == -1, "short IV rejected");
	CHECK(hls_parse_iv("0xZZ0102030405060708090a0b0c0d0e0f", iv) == -1,
	      "non-hex IV rejected");
	CHECK(hls_parse_iv(NULL, iv) == -1, "NULL IV rejected");

	uint8_t seq[16];
	hls_iv_from_sequence(0x0102030405060708ULL, seq);
	uint8_t wseq[16] = { 0,0,0,0, 0,0,0,0, 1,2,3,4,5,6,7,8 };
	CHECK(memcmp(seq, wseq, 16) == 0, "sequence IV big-endian");
}

/* ---- AES-128-CBC known-answer decrypt --------------------------------- */

/* NIST SP 800-38A CBC-AES128 example, block 1.
 *   key = 2b7e151628aed2a6abf7158809cf4f3c
 *   iv  = 000102030405060708090a0b0c0d0e0f
 *   plaintext block 1  = 6bc1bee22e409f96e93d7e117393172a
 *   ciphertext block 1 = 7649abac8119b246cee98e9b12e9197d
 * We append the next CBC block so OpenSSL can validate; instead we feed a
 * 2-block ciphertext built from the published vector and check the recovered
 * plaintext (no PKCS#7 pad here, so we disable final-block padding by using a
 * ciphertext that is the encryption of padded data). To keep this test pure we
 * instead round-trip: encrypt is not exposed, so we use the canonical single
 * block with the matching padded ciphertext generated by OpenSSL offline. */

static void
test_aes_known_answer(void)
{
	/* key/iv from the NIST vector. */
	uint8_t key[16] = {
		0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
		0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
	};
	uint8_t iv[16] = {
		0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
		0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
	};
	/* ciphertext = AES-128-CBC(key,iv) of the 16-byte plaintext
	 *   6bc1bee22e409f96e93d7e117393172a
	 * with PKCS#7 padding (a full pad block appended). Generated with:
	 *   printf '...' | openssl enc -aes-128-cbc -K <key> -iv <iv>
	 * The two ciphertext blocks below are that output. */
	uint8_t ct[32] = {
		0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,
		0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d,
		0x89,0x64,0xe0,0xb1,0x49,0xc1,0x0b,0x7b,
		0x68,0x2e,0x6e,0x39,0xaa,0xeb,0x73,0x1c
	};
	uint8_t want_pt[16] = {
		0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
		0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a
	};

	uint8_t out[32 + 16];	/* inlen + one AES block, per OpenSSL contract */
	size_t outlen = 0;
	int rc = hls_aes128_cbc_decrypt(key, iv, ct, sizeof(ct), out, &outlen);
	CHECK(rc == 0, "AES-128-CBC decrypt succeeds");
	CHECK(outlen == 16, "PKCS#7 padding stripped to 16 bytes");
	CHECK(memcmp(out, want_pt, 16) == 0, "decrypted plaintext matches NIST");

	/* Non-block-aligned input must be rejected. */
	size_t bad = 0;
	CHECK(hls_aes128_cbc_decrypt(key, iv, ct, 17, out, &bad) == -1,
	      "non-block-aligned ciphertext rejected");
	CHECK(hls_aes128_cbc_decrypt(key, iv, ct, 0, out, &bad) == -1,
	      "empty ciphertext rejected");
}

int
main(void)
{
	test_is_playlist_url();
	test_master_parse();
	test_media_parse_plain();
	test_media_parse_aes_iv();
	test_media_parse_aes_seq_iv();
	test_key_method_none();
	test_sample_aes_rejected();
	test_ext_x_map_rejected();
	test_select_bandwidth_only();
	test_not_m3u8_rejected();
	test_parse_iv();
	test_aes_known_answer();

	if (failures == 0)
		printf("OK: all %d checks passed\n", checks);
	else
		printf("%d/%d checks FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
