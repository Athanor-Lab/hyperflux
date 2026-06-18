/*
  Hyperflux -- A lighter download accelerator for Linux and other Unices

  Copyright 2001-2007 Wilmer van der Gaast
  Copyright 2008      Y Giridhar Appaji Nag
  Copyright 2008-2010 Philipp Hagemeister
  Copyright 2015-2017 Joao Eriberto Mota Filho
  Copyright 2016      Denis Denisov
  Copyright 2016      Mridul Malpotra
  Copyright 2016      Stephen Thirlwall
  Copyright 2017      Antonio Quartulli
  Copyright 2017-2019 Ismael Luceno
  Copyright 2019      Evangelos Foutras
  Copyright 2019      Kun Ma


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

/* Text interface */

#include "config.h"
#include <sys/ioctl.h>
#include <strings.h>		/* strncasecmp (file_looks_like_html) */
#include "flux.h"
#include "url_glob.h"
#include "extractor.h"
#include "scan.h"
#include "tui.h"
#ifdef HAVE_SSL
#include "hls.h"
#endif


static void stop(int signal);
static char *time_human(char *dst, size_t len, unsigned int value);
static void print_commas(off_t bytes_done);
static void print_alternate_output(flux_t *flux);
static void print_progress(off_t cur, off_t prev, off_t total, double kbps);
static void print_help(void);
static void print_version(void);
static void print_version_info(void);
static int get_term_width(void);
static int download_one(conf_t *conf, const search_t *urls, int count,
			const char *out_name, char *out_final, size_t final_len);
static int download_media_url(conf_t *conf, const char *media_url,
			      const char *out_name, const char *hls_quality,
			      const char *hls_mux);
static int run_series(conf_t *conf, const char *page_url,
		      const char *extract_name, char **episode_urls,
		      size_t nepisodes, int select_all,
		      const char *episodes_spec, int auto_yes,
		      const char *out_name, const char *hls_quality,
		      const char *hls_mux);
static int is_directory(const char *p);
static int has_numbered_ref(const char *tpl, size_t ncaps);
static void resolve_outname(char *dst, size_t dlen, const char *tpl,
			    const url_glob_t *it, size_t ncaps);
static int file_looks_like_html(const char *path);

int run = 1;

#define MAX_REDIR_OPT	256
#define EXTRACT_OPT	257
#define EXTRACT_LIST_OPT	258
#define QUALITY_OPT	259
#define MUX_OPT		260
#define EXTRACT_SCAN_OPT	261
#define YES_OPT		262
#define ALL_OPT		263
#define EPISODES_OPT	264

#ifdef NOGETOPTLONG
#define getopt_long(a, b, c, d, e) getopt(a, b, c)
#else
static struct option flux_options[] = {
	/* name             has_arg flag  val */
	{"max-speed",       1,      NULL, 's'},
	{"num-connections", 1,      NULL, 'n'},
	{"max-redirect",    1,      NULL, MAX_REDIR_OPT},
	{"output",          1,      NULL, 'o'},
	{"search",          2,      NULL, 'S'},
	{"ipv4",            0,      NULL, '4'},
	{"ipv6",            0,      NULL, '6'},
	{"no-proxy",        0,      NULL, 'N'},
	{"quiet",           0,      NULL, 'q'},
	{"verbose",         0,      NULL, 'v'},
	{"help",            0,      NULL, 'h'},
	{"version",         0,      NULL, 'V'},
	{"alternate",       0,      NULL, 'a'},
	{"percentage",      0,      NULL, 'p'},
	{"insecure",        0,      NULL, 'k'},
	{"no-clobber",      0,      NULL, 'c'},
	{"header",          1,      NULL, 'H'},
	{"user-agent",      1,      NULL, 'U'},
	{"timeout",         1,      NULL, 'T'},
	{"extract",         1,      NULL, EXTRACT_OPT},
	{"extract-list",    0,      NULL, EXTRACT_LIST_OPT},
	{"extract-scan",    1,      NULL, EXTRACT_SCAN_OPT},
	{"yes",             0,      NULL, YES_OPT},
	{"quality",         1,      NULL, QUALITY_OPT},
	{"mux",             1,      NULL, MUX_OPT},
	{"all",             0,      NULL, ALL_OPT},
	{"episodes",        1,      NULL, EPISODES_OPT},
	{NULL,              0,      NULL, 0}
};
#endif

/* Adapter: bridge the extractor's fetch callback to http_fetch. userdata is the
 * conf_t. Converts the K/V header array into "K: V" strings http_fetch wants. */
static int
extract_http_fetch(const char *url, const ext_header_t *headers,
		   size_t nheaders, char **out_body, void *userdata)
{
	conf_t *conf = userdata;
	abuf_t body[1] = { { NULL, 0 } };
	char *hbuf[EXTRACTOR_MAX_HEADERS];
	const char *hptr[EXTRACTOR_MAX_HEADERS];
	size_t built = 0;
	int ret = -1;

	*out_body = NULL;
	if (nheaders > EXTRACTOR_MAX_HEADERS)
		return -1;

	for (size_t i = 0; i < nheaders; i++) {
		size_t need = strlen(headers[i].key) + 2 +
			      strlen(headers[i].val) + 1;
		hbuf[i] = malloc(need);
		if (!hbuf[i])
			goto cleanup;
		snprintf(hbuf[i], need, "%s: %s", headers[i].key,
			 headers[i].val);
		hptr[i] = hbuf[i];
		built++;
	}

	if (http_fetch(conf, url, hptr, nheaders, body) == 0 && body->p) {
		*out_body = body->p;	/* hand ownership to the caller */
		body->p = NULL;
		ret = 0;
	} else {
		abuf_setup(body, ABUF_FREE);
	}

 cleanup:
	for (size_t i = 0; i < built; i++)
		free(hbuf[i]);
	return ret;
}

/* Probe adapter for the scanner: report a direct file's byte size. Prefer a
 * HEAD that reads Content-Length without downloading the body (lighter, and the
 * only correct signal for files past http_fetch's 16 MiB cap). Fall back to a
 * capped body fetch only when the server omits Content-Length, treating a body
 * that hit the cap as size-unknown rather than reporting a truncated length.
 * Returns 0 with *out_size set on success, -1 on failure. userdata is conf_t. */
static int
extract_size_probe(const char *url, long long *out_size, void *userdata)
{
	conf_t *conf = userdata;
	abuf_t body[1] = { { NULL, 0 } };
	size_t blen = 0;

	if (out_size)
		*out_size = -1;

	if (http_probe_len(conf, url, out_size) == 0)
		return 0;

	/* Fallback: server gave no usable Content-Length. http_fetch_len fails
	 * (size-unknown) when the body exceeds its 16 MiB cap, so a success here
	 * means blen is the file's true size. */
	if (http_fetch_len(conf, url, NULL, 0, body, &blen) != 0 || !body->p) {
		abuf_setup(body, ABUF_FREE);
		return -1;
	}
	abuf_setup(body, ABUF_FREE);
	if (out_size)
		*out_size = (long long)blen;
	return 0;
}

/* Run --extract-scan: scan `page_url`, optionally disambiguate via the TUI, and
 * emit a generated config. Output destination:
 *   - no -o (out_file NULL/empty): SAVE active into the user extractor dir as
 *     <user-dir>/<name>.conf (the dir is created). Refuses to overwrite an
 *     existing auto-path so a hand-tuned config is never clobbered.
 *   - '-o -' : write to stdout (the legacy default).
 *   - '-o FILE': write to FILE (explicit override, may overwrite).
 * `auto_yes` skips the TUI and picks the top-ranked candidate. Returns 0 on
 * success, 1 on any error (matching the caller's exit-code convention). */
static int
run_extract_scan(conf_t *conf, const char *page_url, const char *out_file,
		 int auto_yes)
{
	char *err = NULL;
	scan_result_t *r = scan_page(page_url, extract_http_fetch,
				     extract_size_probe, conf, &err);
	if (!r) {
		fprintf(stderr, _("flux: scan failed: %s\n"),
			err ? err : "unknown error");
		free(err);
		return 1;
	}

	int chosen = -1;	/* -1 -> emit the top-ranked candidate */

	/* Ambiguous (>1 candidate) and interactive without --yes: ask the user.
	 * The TUI itself degrades to a numbered prompt on a non-TTY. With --yes
	 * or a single candidate we keep the scorer's top pick. */
	if (!auto_yes && r->ncands > 1) {
		tui_item_t *items = calloc(r->ncands, sizeof(*items));
		char (*details)[256] = calloc(r->ncands, sizeof(*details));
		if (!items || !details) {
			free(items);
			free(details);
			scan_result_free(r);
			fprintf(stderr, _("flux: out of memory\n"));
			return 1;
		}
		for (size_t i = 0; i < r->ncands; i++) {
			const scan_candidate_t *c = &r->cands[i];
			int o = 0;
			o += snprintf(details[i] + o, sizeof(details[i]) - o,
				      "%s score=%.0f",
				      c->kind == SCAN_KIND_HLS ? "hls" : "file",
				      c->score);
			if (c->duration > 0 && o >= 0 &&
			    (size_t)o < sizeof(details[i]))
				o += snprintf(details[i] + o,
					      sizeof(details[i]) - o,
					      " dur=%.0fs", c->duration);
			if (c->size > 0 && o >= 0 && (size_t)o < sizeof(details[i]))
				o += snprintf(details[i] + o,
					      sizeof(details[i]) - o,
					      " size=%lldB", c->size);
			if (c->height > 0 && o >= 0 &&
			    (size_t)o < sizeof(details[i]))
				o += snprintf(details[i] + o,
					      sizeof(details[i]) - o,
					      " %dx%d", c->width, c->height);
			if (c->ad_host && o >= 0 && (size_t)o < sizeof(details[i]))
				snprintf(details[i] + o, sizeof(details[i]) - o,
					 " [ad]");
			items[i].label = c->url;
			items[i].detail = details[i];
		}
		chosen = tui_select_one(_("Select the media stream to extract:"),
					items, r->ncands);
		free(items);
		free(details);
		if (chosen < 0) {	/* user cancelled */
			scan_result_free(r);
			fprintf(stderr, _("flux: scan cancelled.\n"));
			return 1;
		}
	}

	/* Non-TTY + ambiguous + no --yes: scan_page still ran, but we must not
	 * silently guess. tui_select_one's fallback already handled the prompt
	 * above; if it returned a valid index we proceed. */

	/* Resolve the output destination. The auto-save path (no -o) lands in the
	 * active user dir; '-o -' is stdout; '-o FILE' is an explicit override. */
	int to_stdout = (out_file && strcmp(out_file, "-") == 0);
	int explicit_file = (out_file && *out_file && !to_stdout);

	char autopath[MAX_STRING];
	const char *dest = out_file;	/* path used for messages (explicit case) */

	if (!to_stdout && !explicit_file) {
		/* Default: save active into <user-dir>/<name>.conf, creating it. */
		char dir[4096];
		if (!extractor_user_dir(dir, sizeof(dir), 1)) {
			fprintf(stderr,
				_("flux: cannot determine or create the user extractor directory.\n"));
			scan_result_free(r);
			return 1;
		}
		char name[128];
		if (scan_config_name(r, name, sizeof(name)) != 0) {
			fprintf(stderr, _("flux: cannot derive a config name.\n"));
			scan_result_free(r);
			return 1;
		}
		int n = snprintf(autopath, sizeof(autopath), "%s/%s.conf",
				 dir, name);
		if (n <= 0 || (size_t)n >= sizeof(autopath)) {
			fprintf(stderr, _("flux: extractor config path too long.\n"));
			scan_result_free(r);
			return 1;
		}
		/* Never silently clobber a hand-tuned config on the auto-path. */
		if (access(autopath, F_OK) == 0) {
			fprintf(stderr,
				_("flux: %s already exists; pass -o to overwrite or choose a path.\n"),
				autopath);
			scan_result_free(r);
			return 1;
		}
		dest = autopath;
	}

	FILE *out = stdout;
	int close_out = 0;
	if (!to_stdout) {
		out = fopen(dest, "w");
		if (!out) {
			fprintf(stderr, _("flux: cannot write config to %s: %s\n"),
				dest, strerror(errno));
			scan_result_free(r);
			return 1;
		}
		close_out = 1;
	}

	int er = scan_emit_config(r, chosen, out);
	if (close_out) {
		if (fclose(out) != 0)
			er = -1;
	} else {
		fflush(out);
	}
	scan_result_free(r);

	if (er != 0) {
		fprintf(stderr, _("flux: failed to write generated config.\n"));
		return 1;
	}
	if (close_out && conf->verbose >= 0) {
		if (explicit_file)
			fprintf(stderr,
				_("flux: wrote extractor config to %s\n"), dest);
		else
			fprintf(stderr,
				_("flux: saved active extractor config to %s\n"),
				dest);
	}
	return 0;
}

/**
 * Unified percentage calculation for all progress indicators.
 */
static
unsigned
calc_percentage(off_t cur, off_t total)
{
	return min(100, (100 * cur + total / 2) / total);
}

int
main(int argc, char *argv[])
{
	char fn[MAX_STRING];
	int do_search = 0;
	search_t *search;
	conf_t conf[1];
	int j, ret = 1;
	char *stdin_url = NULL;
	const char *single = NULL;
	const char *extract_name = NULL;	/* --extract <name> override */
	const char *scan_url = NULL;		/* --extract-scan <url> target */
	int auto_yes = 0;			/* --yes: no prompts */
	const char *hls_quality = NULL;		/* --quality best|worst|<height> */
	const char *hls_mux = NULL;		/* --mux mp4|ts */
	int select_all = 0;			/* --all: every episode */
	const char *episodes_spec = NULL;	/* --episodes 1,3-5,8 */

	fn[0] = 0;

/* Set up internationalization (i18n) */
#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
#endif
	if (flux_rnd_init() == -1)
		return 1;

	if (!conf_init(conf)) {
		return 1;
	}

	opterr = 0;

	j = -1;
	while (1) {
		int option = getopt_long(argc, argv,
					 "s:n:o:S::46NqvhVapkcH:U:T:",
					 flux_options, NULL);
		if (option == -1)
			break;

		switch (option) {
		case 'U':
			conf_hdr_make(conf->add_header[HDR_USER_AGENT],
				      "User-Agent", optarg);
			break;
		case 'H':
			if(!(conf->add_header_count<MAX_ADD_HEADERS)) {
				fprintf(stderr,
					_("Too many custom headers (-H)! Currently only %u custom headers can be appended.\n"), MAX_ADD_HEADERS-HDR_count_init);
				goto free_conf;
			}
			strlcpy(conf->add_header[conf->add_header_count++], optarg,
				sizeof(conf->add_header[0]));
			break;
		case 's':
			if (!sscanf(optarg, "%llu", &conf->max_speed)) {
				print_help();
				goto free_conf;
			}
			break;
		case 'n':
			if (!sscanf(optarg, "%hu", &conf->num_connections)) {
				print_help();
				goto free_conf;
			}
			break;
		case MAX_REDIR_OPT:
			if (!sscanf(optarg, "%i", &conf->max_redirect)) {
				print_help();
				return 1;
			}
			break;
		case 'o':
			strlcpy(fn, optarg, sizeof(fn));
			break;
		case 'S':
			do_search = 1;
			if (optarg) {
				if (!sscanf(optarg, "%i", &conf->search_top)) {
					print_help();
					goto free_conf;
				}
			}
			break;
		case '6':
			conf->ai_family = AF_INET6;
			break;
		case '4':
			conf->ai_family = AF_INET;
			break;
		case 'a':
			conf->progress_style = FLUX_PROGRESS_STYLE_ALTERNATIVE;
			break;
		case 'p':
			conf->progress_style = FLUX_PROGRESS_STYLE_PERCENTAGE;
			break;
		case 'k':
			conf->insecure = 1;
			break;
		case 'c':
			conf->no_clobber = 1;
			break;
		case 'N':
			*conf->http_proxy = 0;
			break;
		case 'h':
			print_help();
			ret = 0;
			goto free_conf;
		case 'v':
			if (j == -1)
				j = 1;
			else
				j++;
			break;
		case 'V':
			print_version_info();
			ret = 0;
			goto free_conf;
		case 'q':
			close(1);
			conf->verbose = -1;
			if (open("/dev/null", O_WRONLY) != 1) {
				fprintf(stderr,
					_("Can't redirect stdout to /dev/null.\n"));
				goto free_conf;
			}
			break;
		case 'T':
			conf->io_timeout = strtoul(optarg, NULL, 0);
			break;
		case EXTRACT_OPT:
			extract_name = optarg;
			break;
		case EXTRACT_LIST_OPT:
			extractor_list();
			ret = 0;
			goto free_conf;
		case EXTRACT_SCAN_OPT:
			scan_url = optarg;
			break;
		case YES_OPT:
			auto_yes = 1;
			break;
		case ALL_OPT:
			select_all = 1;
			break;
		case EPISODES_OPT:
			episodes_spec = optarg;
			break;
		case QUALITY_OPT:
			hls_quality = optarg;
			break;
		case MUX_OPT:
			if (strcmp(optarg, "mp4") != 0 &&
			    strcmp(optarg, "ts") != 0) {
				fprintf(stderr,
					_("Invalid --mux value '%s' (use mp4 or ts).\n"),
					optarg);
				goto free_conf;
			}
			hls_mux = optarg;
			break;
		default:
			print_help();
			goto free_conf;
		}
	}

	/* disable alternate outputs and verbosity when quiet is specified */
	if (conf->verbose < 0) {
		conf->progress_style = FLUX_PROGRESS_STYLE_CLASSIC;
	} else if (j > -1)
		conf->verbose = j;

	if (conf->num_connections < 1) {
		print_help();
		goto free_conf;
	}

	if (conf->max_redirect < 0) {
		print_help();
		return 1;
	}
#ifdef HAVE_SSL
	ssl_init(conf);
#endif				/* HAVE_SSL */

	/* --extract-scan carries its own URL and writes a config (to -o or
	 * stdout) instead of downloading; handle it before the positional-URL
	 * dispatch and exit. */
	if (scan_url) {
		ret = run_extract_scan(conf, scan_url, fn, auto_yes);
		goto free_conf;
	}

	if (argc - optind == 0) {
		print_help();
		goto free_conf;
	}

	if (strcmp(argv[optind], "-") == 0) {
		stdin_url = malloc(MAX_STRING);
		if (!stdin_url)
			goto free_conf;

		if (scanf("%1023[^\n]", stdin_url) != 1) {
			fprintf(stderr,
				_("Error when trying to read URL (Too long?).\n"));
			free(stdin_url);
			stdin_url = NULL;
			goto free_conf;
		}
		single = stdin_url;
	} else if (argc - optind == 1) {
		single = argv[optind];
		if (strlen(single) >= MAX_STRING) {
			fprintf(stderr,
				_("Can't handle URLs of length over %zu\n"),
				MAX_STRING);
			goto free_conf;
		}
	}

#ifndef HAVE_SSL
	(void)hls_quality;
	(void)hls_mux;	/* HLS dispatch is compiled only with SSL */
#endif

	if (do_search) {
		const char *url = single ? single : argv[optind];

		search = calloc(conf->search_amount + 1, sizeof(search_t));
		if (!search)
			goto cleanup;

		search[0].conf = conf;
		if (conf->verbose)
			printf(_("Doing search...\n"));
		int i = search_makelist(search, (char *)url);
		if (i < 0) {
			fprintf(stderr, _("File not found\n"));
			free(search);
			goto cleanup;
		}
		if (conf->verbose)
			printf(_("Testing speeds, this can take a while...\n"));
		j = search_getspeeds(search, i);
		if (j < 0) {
			fprintf(stderr, _("Speed testing failed\n"));
			free(search);
			ret = 1;
			goto cleanup;
		}

		search_sortlist(search, i);
		if (conf->verbose) {
			printf(_("%i usable servers found, will use these URLs:\n"),
			       j);
			j = min(j, conf->search_top);
			printf("%-60s %15s\n", "URL", _("Speed"));
			for (i = 0; i < j; i++)
				printf("%-70.70s %5jd\n", search[i].url,
				       search[i].speed);
			printf("\n");
		}
		ret = download_one(conf, search, j, fn, NULL, 0);
		free(search);
	} else if (single != NULL) {
		url_glob_t *items = NULL;
		size_t n = 0, ncaps = 0;
		char *resolved = NULL;

		/* Series mode: if a matching config has a 'list', resolve the
		 * episode set first. >1 episode -> select + batch download; a
		 * 1-episode list falls through to the normal single path with
		 * that episode's URL. No list -> normal single resolution. */
		char **episode_urls = NULL;
		size_t nepisodes = 0;
		int sr = extractor_resolve_series(single, extract_name,
						  extract_http_fetch, conf,
						  &episode_urls, &nepisodes);
		if (sr < 0) {
			ret = 1;
			goto cleanup;
		}
		if (sr == 1) {
			/* A series config matched. >1 episode -> select + batch.
			 * Exactly 1 -> resolve that episode via the same config
			 * (its URL won't match the series `match`, so we cannot
			 * re-discover) and download it directly. */
			if (nepisodes > 1) {
				ret = run_series(conf, single, extract_name,
						 episode_urls, nepisodes,
						 select_all, episodes_spec,
						 auto_yes, fn, hls_quality,
						 hls_mux);
			} else {
				char *media = NULL;
				int er1 = extractor_run_series_episode(single,
					episode_urls[0], extract_name,
					extract_http_fetch, conf, &media);
				if (er1 < 0 || !media) {
					free(media);
					ret = 1;
				} else {
					if (conf->verbose > 0)
						printf(_("Extracted media URL: %s\n"),
						       media);
					ret = download_media_url(conf, media, fn,
								 hls_quality,
								 hls_mux);
					free(media);
				}
			}
			extractor_free_urls(episode_urls, nepisodes);
			goto cleanup;
		}
		extractor_free_urls(episode_urls, nepisodes);

		/* Resolve a web-page URL to a direct media URL via extractor
		 * configs. No match -> resolved == copy of single (passthrough).
		 * On error abort; on success download the resolved URL. */
		int er = extractor_resolve(single, extract_name,
					   extract_http_fetch, conf, &resolved);
		if (er < 0) {
			ret = 1;
			goto cleanup;
		}
		if (resolved && strcmp(resolved, single) != 0 && conf->verbose > 0)
			printf(_("Extracted media URL: %s\n"), resolved);
		const char *src_url = resolved ? resolved : single;

#ifdef HAVE_SSL
		/* HLS playlist: hand off to the segment downloader and skip the
		 * normal glob/range path entirely. */
		if (hls_is_playlist_url(src_url)) {
			int hr = hls_download(conf, src_url, fn, hls_quality,
					      hls_mux);
			free(resolved);
			ret = hr == 0 ? 0 : 1;
			goto cleanup;
		}
#endif

		if (url_glob(src_url, MAX_STRING, &items, &n, &ncaps) < 0) {
			fprintf(stderr,
				_("Invalid URL pattern, or too many URLs (max %d).\n"),
				URL_GLOB_MAX_URLS);
			free(resolved);
			goto cleanup;
		}
		free(resolved);

		/* Refuse to clobber one file with many distinct downloads */
		if (n > 1 && *fn && !is_directory(fn)
		    && !has_numbered_ref(fn, ncaps)) {
			fprintf(stderr,
				_("Refusing to write %zu downloads to a single file '%s'; use a directory or #N in --output.\n"),
				n, fn);
			url_glob_free(items, n, ncaps);
			ret = 1;
			goto cleanup;
		}

		/* Best-effort hint: when a lone URL matched no extractor config and
		 * the fetched bytes are an HTML page, the user likely wanted it
		 * extracted. Only meaningful for the single-URL no-match case; the
		 * resolved on-disk path is captured to sniff it after download. */
		int want_html_hint = (n == 1 && er == 0);
		char final_path[MAX_STRING];
		final_path[0] = '\0';

		int failures = 0;
		size_t done = 0;
		for (size_t k = 0; k < n && run; k++) {
			if (n > 1)
				printf(_("\n=== %zu/%zu: %s ===\n"),
				       (size_t)(k + 1), n, items[k].url);

			search_t one;
			memset(&one, 0, sizeof(one));
			strlcpy(one.url, items[k].url, sizeof(one.url));

			char outname[MAX_STRING];
			resolve_outname(outname, sizeof(outname), fn,
					&items[k], ncaps);

			int r = download_one(conf, &one, 1, outname,
					     want_html_hint ? final_path : NULL,
					     want_html_hint ? sizeof(final_path) : 0);
			if (r == 0)
				done++;
			else
				failures++;
		}
		url_glob_free(items, n, ncaps);
		if (n > 1)
			printf(_("\n%zu of %zu downloads completed.\n"),
			       done, n);

		/* Hint after a successful no-match single download whose bytes look
		 * like HTML: the user probably wanted extraction, not the page. */
		if (want_html_hint && run && !failures && final_path[0]
		    && file_looks_like_html(final_path))
			fprintf(stderr,
				_("flux: %s looks like a web page, not a media file; "
				  "no extractor config matched. Try: flux --extract-scan \"%s\"\n"),
				single, single);

		/* Mirror Hyperflux's exit codes: 2 on interrupt, 1 on any failure. */
		if (!run)
			ret = 2;
		else
			ret = failures ? 1 : 0;
	} else {
		/* Multiple distinct URLs: legacy mirror mode */
		if (extract_name)
			fprintf(stderr,
				_("Warning: --extract is ignored with multiple URLs.\n"));
		search = calloc(argc - optind, sizeof(search_t));
		if (!search)
			goto cleanup;

		for (int i = 0; i < argc - optind; i++) {
			strlcpy(search[i].url, argv[optind + i],
				sizeof(search[i].url));
			// FIXME check url here
		}
		ret = download_one(conf, search, argc - optind, fn, NULL, 0);
		free(search);
	}

 cleanup:
	free(stdin_url);
 free_conf:
	conf_free(conf);

	return ret;
}

/* Run a single download (one file, possibly multi-connection / multi-mirror).
 * urls[0].url is the displayed URL; out_name is a filename template (may be
 * empty, a directory, or contain #N references already resolved by the caller).
 * When out_final is non-NULL the final on-disk path is copied into it (capacity
 * final_len) once the filename is resolved, so a caller can inspect the saved
 * file. Returns 0 if completed, 2 if interrupted/incomplete, 1 on setup error. */
static int
download_one(conf_t *conf, const search_t *urls, int count, const char *out_name,
	     char *out_final, size_t final_len)
{
	flux_t *flux;
	int ret = 1;
	char *s;
	/* flux_new/flux_divide rotate the shared interfaces list; restore the
	 * head on return so conf_free releases the original node. */
	flux_if_t *saved_if = conf->interfaces;

	if (out_final && final_len)
		out_final[0] = '\0';

	if (conf->progress_style != FLUX_PROGRESS_STYLE_PERCENTAGE)
		printf(_("Initializing download: %s\n"), urls[0].url);

	flux = flux_new(conf, count, urls);
	if (!flux || flux->ready == -1) {
		print_messages(flux);
		goto close_flux;
	}
	print_messages(flux);

	/* Work on a local copy: callers reuse the template across downloads */
	char buf[MAX_STRING];
	strlcpy(buf, out_name, sizeof(buf));

	/* Check if a file name has been specified */
	if (*buf) {
		struct stat sbuf;

		if (stat(buf, &sbuf) == 0) {
			if (S_ISDIR(sbuf.st_mode)) {
				size_t fnlen = strlen(buf);
				size_t fluxfnlen = strlen(flux->filename);

				if (fnlen + 1 + fluxfnlen + 1 > MAX_STRING) {
					fprintf(stderr, _("Filename too long!\n"));
					goto close_flux;
				}

				buf[fnlen] = '/';
				memcpy(buf + fnlen + 1, flux->filename,
				       fluxfnlen);
				buf[fnlen + 1 + fluxfnlen] = '\0';
			}
		}
		char statefn[MAX_STRING + 3];
		snprintf(statefn, sizeof(statefn), "%s.st", buf);
		if (access(buf, F_OK) == 0 && access(statefn, F_OK) != 0) {
			fprintf(stderr, _("No state file, cannot resume!\n"));
			goto close_flux;
		}
		if (access(statefn, F_OK) == 0 && access(buf, F_OK) != 0) {
			printf(_("State file found, but no downloaded data. Starting from scratch.\n"));
			unlink(statefn);
		}
		strlcpy(flux->filename, buf, sizeof(flux->filename));
	} else {
		/* Local file existence check */
		s = flux->filename + strlen(flux->filename);
		for (int i = 0; 1; i++) {
			char statefn[MAX_STRING + 3];
			snprintf(statefn, sizeof(statefn), "%s.st",
				 flux->filename);

			int f_exists = !access(flux->filename, F_OK);
			int st_exists = !access(statefn, F_OK);
			if (f_exists) {
				if (flux->conn[0].supported && st_exists)
						break;
			} else if (!st_exists)
				break;
			snprintf(s, flux->filename + sizeof(flux->filename) - s,
				 ".%i", i);
		}
	}

	/* Report the resolved on-disk path so a caller can inspect the file. */
	if (out_final && final_len)
		strlcpy(out_final, flux->filename, final_len);

	if (!flux_open(flux)) {
		print_messages(flux);
		goto close_flux;
	}
	print_messages(flux);
	flux_start(flux);
	print_messages(flux);

	if (conf->progress_style == FLUX_PROGRESS_STYLE_ALTERNATIVE
	    || conf->progress_style == FLUX_PROGRESS_STYLE_PERCENTAGE) {
		putchar('\n');
	} else if (flux->bytes_done > 0) {	/* Print first dots if resuming */
		putchar('\n');
		print_commas(flux->bytes_done);
		fflush(stdout);

	}
	flux->start_byte = flux->bytes_done;

	/* Install save_state signal handler for resuming support */
	signal(SIGINT, stop);
	signal(SIGTERM, stop);

	while (!flux->ready && run) {
		off_t prev;

		prev = flux->bytes_done;
		flux_do(flux);

		if (conf->progress_style == FLUX_PROGRESS_STYLE_PERCENTAGE) {
			if (!flux->message && prev != flux->bytes_done)
				printf("%u\n", calc_percentage(flux->bytes_done, flux->size));
		} else 	if (conf->progress_style == FLUX_PROGRESS_STYLE_ALTERNATIVE) {
			if (!flux->message && prev != flux->bytes_done)
				print_alternate_output(flux);
		} else if (conf->verbose > -1) {
			print_progress(flux->bytes_done, prev, flux->size,
				       (double)flux->bytes_per_second / 1024);
		}

		if (flux->message) {
			if (conf->progress_style == FLUX_PROGRESS_STYLE_ALTERNATIVE) {
				/* clreol-simulation */
				fputs("\e[2K\r", stdout);
			} else {
				putchar('\n');
			}
			print_messages(flux);
			if (!flux->ready) {
				if (conf->progress_style != FLUX_PROGRESS_STYLE_ALTERNATIVE)
					print_commas(flux->bytes_done);
				else
					print_alternate_output(flux);
			}
		} else if (flux->ready) {
			putchar('\n');
		}
		fflush(stdout);
	}

	char hsize[MAX_STRING / 2], htime[MAX_STRING / 2];
	time_human(htime, sizeof(htime), flux_gettime() - flux->start_time);
	flux_size_human(hsize, sizeof(hsize), flux->bytes_done - flux->start_byte);

	printf(_("\nDownloaded %s in %s. (%.2f KB/s)\n"), hsize, htime,
	       (double)flux->bytes_per_second / 1024);

	ret = flux->ready ? 0 : 2;

 close_flux:
	flux_close(flux);
	conf->interfaces = saved_if;
	return ret;
}

/* Download one already-resolved media URL into out_name (a file path, empty for
 * a derived name, or a directory). Dispatches HLS playlists to hls_download and
 * everything else to download_one. Returns 0 on success, non-zero on failure or
 * interruption (mirrors download_one's 0/2 plus 1 for HLS/setup errors). */
static int
download_media_url(conf_t *conf, const char *media_url, const char *out_name,
		   const char *hls_quality, const char *hls_mux)
{
#ifdef HAVE_SSL
	if (hls_is_playlist_url(media_url))
		return hls_download(conf, media_url, out_name, hls_quality,
				    hls_mux) == 0 ? 0 : 1;
#else
	(void)hls_quality;
	(void)hls_mux;
#endif
	search_t one;
	memset(&one, 0, sizeof(one));
	strlcpy(one.url, media_url, sizeof(one.url));
	return download_one(conf, &one, 1, out_name, NULL, 0);
}

/* Derive a unique, safe leaf filename for episode #num from its media URL.
 * Writes into dst (always NUL-terminated). The 1-based episode number is always
 * prefixed so two episodes never clobber even if their media leaves collide
 * (e.g. both "index.m3u8"). The URL's last path segment supplies the suffix
 * (sanitized to [A-Za-z0-9._-]); a missing/empty segment yields just the
 * numbered name. */
static void
episode_basename(char *dst, size_t dlen, const char *media_url, size_t num)
{
	if (!dlen)
		return;

	/* Always-unique numeric prefix. */
	int pre = snprintf(dst, dlen, "ep%02zu", num);
	if (pre <= 0 || (size_t)pre >= dlen) {
		if (dlen)
			dst[0] = '\0';
		return;
	}
	size_t o = (size_t)pre;

	/* Take the segment after the last '/', dropping any query/fragment. */
	const char *p = media_url;
	const char *sep = strstr(media_url, "://");
	if (sep)
		p = sep + 3;
	const char *last = p;
	for (const char *c = p; *c && *c != '?' && *c != '#'; c++)
		if (*c == '/')
			last = c + 1;
	size_t seglen = strcspn(last, "?#");

	/* Sanitize and append "-<leaf>" only when the leaf has a useful char. */
	int useful = 0;
	for (size_t i = 0; i < seglen; i++) {
		unsigned char c = (unsigned char)last[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
			if (c != '.')
				useful = 1;
		}
	}
	if (!useful)
		return;	/* nothing meaningful to add: keep "epNN" */

	if (o + 1 < dlen)
		dst[o++] = '-';
	for (size_t i = 0; i < seglen && o + 1 < dlen; i++) {
		unsigned char c = (unsigned char)last[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
			dst[o++] = (char)c;
		else
			dst[o++] = '_';
	}
	dst[o] = '\0';
}

/* Batch-download a selected subset of a series' episodes.
 *
 * episode_urls[0..nepisodes) are the ordered episode page URLs. Selection comes
 * from --all, --episodes <spec>, or (interactively) the multi-select TUI. Each
 * selected episode is re-resolved through the per-episode pipeline and
 * downloaded; one failure does not abort the rest. out_name, if a directory (or
 * empty), receives per-episode files; a plain -o file name is only honoured for
 * a single selected episode (else it would clobber). Returns 0 if all selected
 * succeeded, 2 on interrupt, 1 if any failed or on a setup error. */
static int
run_series(conf_t *conf, const char *page_url, const char *extract_name,
	   char **episode_urls, size_t nepisodes, int select_all,
	   const char *episodes_spec, int auto_yes, const char *out_name,
	   const char *hls_quality, const char *hls_mux)
{
	unsigned char *sel = NULL;	/* sel[i] != 0 -> download episode i */
	size_t nsel = 0;

	if (select_all && episodes_spec) {
		fprintf(stderr,
			_("Use either --all or --episodes, not both.\n"));
		return 1;
	}

	if (select_all) {
		sel = malloc(nepisodes);
		if (!sel) {
			fprintf(stderr, _("flux: out of memory\n"));
			return 1;
		}
		memset(sel, 1, nepisodes);
		nsel = nepisodes;
	} else if (episodes_spec) {
		char eb[160];
		if (extractor_parse_episodes(episodes_spec, nepisodes, &sel,
					     &nsel, eb, sizeof(eb)) != 0) {
			fprintf(stderr, "flux: %s\n", eb);
			return 1;
		}
	} else {
		/* Interactive (or non-TTY fallback) multi-select. With --yes and
		 * no explicit selection on a non-TTY we must not guess: the TUI
		 * fallback reads a spec from stdin; refuse if that is unavailable. */
		if (auto_yes && !isatty(STDIN_FILENO)) {
			fprintf(stderr,
				_("flux: %zu episodes found; pass --all or --episodes to choose non-interactively.\n"),
				nepisodes);
			return 1;
		}

		tui_item_t *items = calloc(nepisodes, sizeof(*items));
		char (*labels)[64] = calloc(nepisodes, sizeof(*labels));
		if (!items || !labels) {
			free(items);
			free(labels);
			fprintf(stderr, _("flux: out of memory\n"));
			return 1;
		}
		for (size_t i = 0; i < nepisodes; i++) {
			snprintf(labels[i], sizeof(labels[i]),
				 "Episode %zu", i + 1);
			items[i].label = labels[i];
			items[i].detail = episode_urls[i];
		}
		size_t *idx = NULL;
		int cnt = tui_select_many(_("Select episodes to download:"),
					  items, nepisodes, NULL, &idx);
		free(items);
		free(labels);
		if (cnt < 0) {	/* cancelled or OOM */
			free(idx);
			fprintf(stderr, _("flux: episode selection cancelled.\n"));
			return 1;
		}
		if (cnt == 0) {
			free(idx);
			fprintf(stderr, _("flux: no episodes selected.\n"));
			return 1;
		}
		sel = calloc(nepisodes, 1);
		if (!sel) {
			free(idx);
			fprintf(stderr, _("flux: out of memory\n"));
			return 1;
		}
		for (int i = 0; i < cnt; i++)
			if ((size_t)idx[i] < nepisodes)
				sel[idx[i]] = 1;
		free(idx);
		nsel = (size_t)cnt;
	}

	/* A plain -o file (not a directory) can hold only one episode. */
	int out_is_dir = out_name && *out_name && is_directory(out_name);
	int out_is_file = out_name && *out_name && !out_is_dir;
	if (out_is_file && nsel > 1) {
		fprintf(stderr,
			_("Refusing to write %zu episodes to a single file '%s'; use a directory.\n"),
			nsel, out_name);
		free(sel);
		return 1;
	}

	size_t done = 0, failed = 0, attempted = 0;
	for (size_t i = 0; i < nepisodes && run; i++) {
		if (!sel[i])
			continue;
		attempted++;

		printf(_("\n=== episode %zu/%zu: %s ===\n"),
		       i + 1, nepisodes, episode_urls[i]);

		/* Re-run the per-episode pipeline (discovered via the SERIES
		 * page URL; the engine skips 'list' and binds {url} to this
		 * episode) to get the concrete media URL. */
		char *media = NULL;
		int er = extractor_run_series_episode(page_url, episode_urls[i],
						      extract_name,
						      extract_http_fetch, conf,
						      &media);
		if (er < 0 || !media) {
			fprintf(stderr,
				_("flux: episode %zu: could not resolve media URL.\n"),
				i + 1);
			free(media);
			failed++;
			continue;
		}
		if (conf->verbose > 0)
			printf(_("Extracted media URL: %s\n"), media);

		/* Build a distinct output path. For a directory (or empty -o)
		 * derive a per-episode leaf so episodes never clobber. For a
		 * single-episode plain -o, honour the given name verbatim. */
		char outpath[MAX_STRING];
		if (out_is_file) {
			strlcpy(outpath, out_name, sizeof(outpath));
		} else {
			char base[96];
			episode_basename(base, sizeof(base), media, i + 1);
			if (out_is_dir) {
				int w = snprintf(outpath, sizeof(outpath),
						 "%s/%s", out_name, base);
				if (w <= 0 || (size_t)w >= sizeof(outpath)) {
					fprintf(stderr,
						_("flux: episode %zu: output path too long.\n"),
						i + 1);
					free(media);
					failed++;
					continue;
				}
			} else {
				strlcpy(outpath, base, sizeof(outpath));
			}
		}

		int r = download_media_url(conf, media, outpath, hls_quality,
					   hls_mux);
		free(media);
		if (r == 0)
			done++;
		else
			failed++;
	}

	free(sel);

	printf(_("\nEpisodes: %zu requested, %zu downloaded, %zu failed.\n"),
	       nsel, done, failed);

	if (!run && attempted < nsel)
		return 2;	/* interrupted before finishing the batch */
	return failed ? 1 : 0;
}

/* Return 1 if p exists and is a directory, 0 otherwise. */
static int
is_directory(const char *p)
{
	struct stat buf;

	if (stat(p, &buf) != 0)
		return 0;
	return S_ISDIR(buf.st_mode) ? 1 : 0;
}

/* Return 1 if tpl contains a '#' followed by digits naming a capture in
 * range 1..ncaps. */
static int
has_numbered_ref(const char *tpl, size_t ncaps)
{
	for (const char *p = tpl; *p; p++) {
		if (*p != '#' || !isdigit((unsigned char)p[1]))
			continue;

		size_t idx = 0;
		const char *q = p + 1;
		while (isdigit((unsigned char)*q)) {
			idx = idx * 10 + (size_t)(*q - '0');
			if (idx > ncaps)	/* avoid overflow, already out of range */
				break;
			q++;
		}
		if (idx >= 1 && idx <= ncaps)
			return 1;
	}
	return 0;
}

/* Build an output name from a template, substituting #D (D = 1..ncaps) with
 * the matching capture. A '#' not followed by a valid in-range index is copied
 * literally. Always NUL-terminates dst and never overflows dlen. */
static void
resolve_outname(char *dst, size_t dlen, const char *tpl,
		const url_glob_t *it, size_t ncaps)
{
	if (!dlen)
		return;

	if (!*tpl) {
		dst[0] = 0;
		return;
	}

	size_t o = 0;
	for (const char *p = tpl; *p && o + 1 < dlen;) {
		if (*p == '#' && isdigit((unsigned char)p[1])) {
			size_t idx = 0;
			const char *q = p + 1;
			while (isdigit((unsigned char)*q)) {
				idx = idx * 10 + (size_t)(*q - '0');
				if (idx > ncaps)
					break;
				q++;
			}
			if (idx >= 1 && idx <= ncaps && it->caps
			    && it->caps[idx - 1]) {
				for (const char *c = it->caps[idx - 1];
				     *c && o + 1 < dlen; c++)
					dst[o++] = *c;
				p = q;
				continue;
			}
		}
		dst[o++] = *p++;
	}
	dst[o] = 0;
}

/* Best-effort: 1 if the file at `path` begins with an HTML signature. Reads a
 * small prefix, skips a UTF-8 BOM and leading whitespace, and matches a
 * case-insensitive "<!doctype html" or "<html" (start tag or with attributes).
 * Any read error or non-HTML prefix returns 0. */
static int
file_looks_like_html(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return 0;

	unsigned char raw[512];
	size_t got = fread(raw, 1, sizeof(raw) - 1, fp);
	fclose(fp);
	if (got == 0)
		return 0;
	raw[got] = '\0';

	const char *p = (const char *)raw;
	const char *end = (const char *)raw + got;
	if (got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
		p += 3;				/* skip UTF-8 BOM */
	while (p < end && isspace((unsigned char)*p))
		p++;

	size_t avail = (size_t)(end - p);
	if (avail >= 14 && strncasecmp(p, "<!doctype html", 14) == 0)
		return 1;
	/* "<html" followed by a tag terminator or attribute whitespace. */
	if (avail >= 5 && strncasecmp(p, "<html", 5) == 0
	    && (p[5] == '>' || p[5] == ' ' || p[5] == '\t' || p[5] == '\n'
		|| p[5] == '\r'))
		return 1;
	return 0;
}

/* SIGINT/SIGTERM handler */
void
stop(int signal)
{
	(void)signal;
	run = 0;
}

/**
 * Integer base-2 logarithm.
 */
static inline
unsigned
log2i(unsigned long long x)
{
	return x ? sizeof(x) * 8 - 1 - __builtin_clzll(x) : 0;
}

/* Convert a number of bytes to a human-readable form */
char *
flux_size_human(char *dst, size_t len, size_t value)
{
	double fval = (double)value;
	const char * const oname[] = {
		"", _("Kilo"), _("Mega"), _("Giga"), _("Tera"),
	};
	const unsigned int order = min(sizeof(oname) / sizeof(oname[0]) - 1,
				       log2i(fval) / 10);

	fval /= (double)(1 << order * 10);
	int ret = snprintf(dst, len, _("%g %sbyte(s)"), fval, oname[order]);
	return ret < 0 ? NULL : dst;
}

/* Convert a number of seconds to a human-readable form */
char *
time_human(char *dst, size_t len, unsigned int value)
{
	unsigned int hh, mm, ss;

	ss = value % 60;
	mm = value / 60 % 60;
	hh = value / 3600;

	int ret;
	if (hh)
		ret = snprintf(dst, len, _("%i:%02i:%02i hour(s)"), hh, mm, ss);
	else if (mm)
		ret = snprintf(dst, len, _("%i:%02i minute(s)"), mm, ss);
	else
		ret = snprintf(dst, len, _("%i second(s)"), ss);

	return ret < 0 ? NULL : dst;
}

/* Part of the infamous wget-like interface. Just put it in a function
	because I need it quite often.. */
void
print_commas(off_t bytes_done)
{
	int i, j;

	printf("       ");
	j = (bytes_done / 1024) % 50;
	if (j == 0)
		j = 50;
	for (i = 0; i < j; i++) {
		if ((i % 10) == 0)
			putchar(' ');
		putchar(',');
	}
}


/**
 * The infamous wget-like 'interface'.. ;)
 */
static
void
print_progress(off_t cur, off_t prev, off_t total, double kbps)
{
	prev /= 1024;
	cur /= 1024;

	bool print_speed = prev > 0;
	for (off_t i = prev; i < cur; i++) {
		if (i % 50 == 0) {
			if (print_speed)
				printf("  [%6.1fKB/s]", kbps);

			if (total == LLONG_MAX)
				printf("\n[ N/A]  ");
			else
				printf("\n[%3u%%]  ",
				       calc_percentage(1024 * i, total));
		} else if (i % 10 == 0) {
			putchar(' ');
		}
		putchar('.');
	}
}

static
char
alt_id(int n)
{
	const char *p = "09AZaz";
	while (*p && n > p[1] - p[0]) {
		n -= p[1] - p[0] + 1;
		p += 2;
	}
	return *p ? *p + n : '*';
}

static void
print_alternate_output_progress(flux_t *flux, char *progress, int width,
				off_t done, off_t total,
				double now)
{
	if (!width)
		width = 1;
	if (!total)
		total = 1;
	for (int i = 0; i < flux->conf->num_connections; i++) {
		int offset = flux->conn[i].currentbyte * width / total;

		if (flux->conn[i].currentbyte < flux->conn[i].lastbyte) {
			if (now <= flux->conn[i].last_transfer
				   + flux->conf->connection_timeout / 2) {
				progress[offset] = alt_id(i);
			} else
				progress[offset] = '#';
		}
		memset(progress + offset + 1, ' ',
		       max(0, flux->conn[i].lastbyte * width / total - offset - 1));
	}

	progress[width] = '\0';
	printf("\r[%3u%%] [%s", calc_percentage(done, total), progress);
}

static void
print_alternate_output(flux_t *flux)
{
	off_t done = flux->bytes_done;
	off_t total = flux->size;
	double now = flux_gettime();
	int width = get_term_width();
	char *progress;

	if (width < 40) {
		fprintf(stderr,
			_("Can't setup alternate output. Deactivating.\n"));
		flux->conf->progress_style = FLUX_PROGRESS_STYLE_CLASSIC;

		return;
	}

	width -= 30;
	progress = malloc(width + 1);
	if (!progress)
		return;

	memset(progress, '.', width);

	if (total != LLONG_MAX) {
		print_alternate_output_progress(flux, progress, width, done,
						total, now);
	} else {
		progress[width] = '\0';
		printf("\r[ N/A] [%s", progress);
	}

	if (flux->bytes_per_second > 1048576)
		printf("] [%6.1fMB/s]",
		       (double)flux->bytes_per_second / (1024 * 1024));
	else if (flux->bytes_per_second > 1024)
		printf("] [%6.1fKB/s]", (double)flux->bytes_per_second / 1024);
	else
		printf("] [%6.1fB/s]", (double)flux->bytes_per_second);

	if (total != LLONG_MAX && done < total) {
		int seconds, minutes, hours, days;
		seconds = flux->finish_time - now;
		minutes = seconds / 60;
		seconds -= minutes * 60;
		hours = minutes / 60;
		minutes -= hours * 60;
		days = hours / 24;
		hours -= days * 24;
		if (days)
			printf(" [%2dd%2d]", days, hours);
		else if (hours)
			printf(" [%2dh%02d]", hours, minutes);
		else
			printf(" [%02d:%02d]", minutes, seconds);
	}

	free(progress);
}

static int
get_term_width(void)
{
	struct winsize w;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	return w.ws_col;
}

void
print_help(void)
{
	print_version();
#ifdef NOGETOPTLONG
	printf(_("Usage: flux [options] url1 [url2] [url...]\n"
		 "\n"
		 "-s x\tSpecify maximum speed (bytes per second)\n"
		 "-n x\tSpecify maximum number of connections\n"
		 "-o f\tSpecify local output file\n"
		 "-S[n]\tSearch for mirrors and download from n servers\n"
		 "-4\tUse the IPv4 protocol\n"
		 "-6\tUse the IPv6 protocol\n"
		 "-H x\tAdd HTTP header string\n"
		 "-U x\tSet user agent\n"
		 "-N\tJust don't use any proxy server\n"
		 "-k\tDon't verify the SSL certificate\n"
		 "-c\tSkip download if file already exists\n"
		 "-q\tLeave stdout alone\n"
		 "-v\tMore status information\n"
		 "-a\tAlternate progress indicator\n"
		 "-p\tPrint simple percentages instead of progress bar (0-100)\n"
		 "-h\tThis information\n"
		 "-T x\tSet I/O and connection timeout\n"
		 "-V\tVersion information\n"
		 "\n"
		 "Visit https://example.invalid/hyperflux/issues\n"));
#else
	printf(_("Usage: flux [options] url1 [url2] [url...]\n"
		 "\n"
		 "--max-speed=x\t\t-s x\tSpecify maximum speed (bytes per second)\n"
		 "--num-connections=x\t-n x\tSpecify maximum number of connections\n"
		 "--max-redirect=x\t\tSpecify maximum number of redirections\n"
		 "--output=f\t\t-o f\tSpecify local output file\n"
		 "--search[=n]\t\t-S[n]\tSearch for mirrors and download from n servers\n"
		 "--ipv4\t\t\t-4\tUse the IPv4 protocol\n"
		 "--ipv6\t\t\t-6\tUse the IPv6 protocol\n"
		 "--header=x\t\t-H x\tAdd HTTP header string\n"
		 "--user-agent=x\t\t-U x\tSet user agent\n"
		 "--no-proxy\t\t-N\tJust don't use any proxy server\n"
		 "--insecure\t\t-k\tDon't verify the SSL certificate\n"
		 "--no-clobber\t\t-c\tSkip download if file already exists\n"
		 "--quiet\t\t\t-q\tLeave stdout alone\n"
		 "--verbose\t\t-v\tMore status information\n"
		 "--alternate\t\t-a\tAlternate progress indicator\n"
		 "--percentage\t\t-p\tPrint simple percentages instead of progress bar (0-100)\n"
		 "--help\t\t\t-h\tThis information\n"
		 "--timeout=x\t\t-T x\tSet I/O and connection timeout\n"
		 "--extract=name\t\t\tForce a named extractor config\n"
		 "--extract-list\t\t\tList discovered extractor configs\n"
		 "--extract-scan=url\t\tScan a page and save an active config (-o FILE to a path, -o - to stdout)\n"
		 "--yes\t\t\t\tNon-interactive: auto-pick the top candidate\n"
		 "--all\t\t\t\tSeries: download every episode (no prompt)\n"
		 "--episodes=spec\t\t\tSeries: pick episodes, e.g. 1,3-5,8 (1-based)\n"
		 "--quality=q\t\t\tHLS variant: best|worst|<height> (default best)\n"
		 "--mux=c\t\t\t\tHLS container: mp4|ts (default mp4 if ffmpeg)\n"
		 "--version\t\t-V\tVersion information\n"
		 "\n"
		 "Visit https://example.invalid/hyperflux/issues to report bugs\n"));
#endif
}

void
print_version(void)
{
	printf(_("Hyperflux %s (%s)\n"), VERSION, ARCH);
}

void
print_version_info(void)
{
	print_version();
	printf("\nCopyright 2001-2007 Wilmer van der Gaast,\n"
	       "\t  2007-2009 Giridhar Appaji Nag,\n"
	       "\t  2008-2010 Philipp Hagemeister,\n"
	       "\t  2015-2017 Joao Eriberto Mota Filho,\n"
	       "\t  2016-2017 Stephen Thirlwall,\n"
	       "\t  2017      Antonio Quartulli,\n"
	       "\t  2017-2024 Ismael Luceno,\n"
	       "\t\t    %s\n%s\n\n", _("and others."),
	       _("Please, see the CREDITS file.\n\n"));
}

/* Print any message in the flux structure */
void
print_messages(flux_t *flux)
{
	message_t *m;

	if (!flux)
		return;

	while ((m = flux->message)) {
		printf("%s\n", m->text);
		flux->message = m->next;
		free(m);
	}
}
