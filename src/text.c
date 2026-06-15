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
#include "flux.h"
#include "url_glob.h"


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
			const char *out_name);
static int is_directory(const char *p);
static int has_numbered_ref(const char *tpl, size_t ncaps);
static void resolve_outname(char *dst, size_t dlen, const char *tpl,
			    const url_glob_t *it, size_t ncaps);

int run = 1;

#define MAX_REDIR_OPT	256

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
	{NULL,              0,      NULL, 0}
};
#endif

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
		ret = download_one(conf, search, j, fn);
		free(search);
	} else if (single != NULL) {
		url_glob_t *items = NULL;
		size_t n = 0, ncaps = 0;

		if (url_glob(single, MAX_STRING, &items, &n, &ncaps) < 0) {
			fprintf(stderr,
				_("Invalid URL pattern, or too many URLs (max %d).\n"),
				URL_GLOB_MAX_URLS);
			goto cleanup;
		}

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

			int r = download_one(conf, &one, 1, outname);
			if (r == 0)
				done++;
			else
				failures++;
		}
		url_glob_free(items, n, ncaps);
		if (n > 1)
			printf(_("\n%zu of %zu downloads completed.\n"),
			       done, n);
		/* Mirror Hyperflux's exit codes: 2 on interrupt, 1 on any failure. */
		if (!run)
			ret = 2;
		else
			ret = failures ? 1 : 0;
	} else {
		/* Multiple distinct URLs: legacy mirror mode */
		search = calloc(argc - optind, sizeof(search_t));
		if (!search)
			goto cleanup;

		for (int i = 0; i < argc - optind; i++) {
			strlcpy(search[i].url, argv[optind + i],
				sizeof(search[i].url));
			// FIXME check url here
		}
		ret = download_one(conf, search, argc - optind, fn);
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
 * Returns 0 if completed, 2 if interrupted/incomplete, 1 on setup error. */
static int
download_one(conf_t *conf, const search_t *urls, int count, const char *out_name)
{
	flux_t *flux;
	int ret = 1;
	char *s;
	/* flux_new/flux_divide rotate the shared interfaces list; restore the
	 * head on return so conf_free releases the original node. */
	flux_if_t *saved_if = conf->interfaces;

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
