/*
  Hyperflux -- A lighter download accelerator for Linux and other Unices

  Copyright 2001-2007 Wilmer van der Gaast
  Copyright 2007-2009 Y Giridhar Appaji Nag
  Copyright 2008-2009 Philipp Hagemeister
  Copyright 2015-2017 Joao Eriberto Mota Filho
  Copyright 2016      Denis Denisov
  Copyright 2016      Ivan Gimenez
  Copyright 2016      Sjjad Hashemian
  Copyright 2016      Stephen Thirlwall
  Copyright 2017      Antonio Quartulli
  Copyright 2017-2019 Ismael Luceno
  Copyright 2017      nemermollon
  Copyright 2018      Shankar
  Copyright 2019      Evangelos Foutras

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

/* Main control */

#include "config.h"
#include "flux.h"
#include "assert.h"
#include "sleep.h"

/* Hyperflux */
static void save_state(flux_t *flux);
static void *setup_thread(void *);

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif /* __GNUC__ */
static void flux_message(flux_t *flux, const char *format, ...);
static void flux_divide(flux_t *flux);

static char *buffer = NULL;

#define MIN_CHUNK_WORTH (100 * 1024) /* 100 KB */


static
char *
stfile_makename(const char *bname)
{
	const char suffix[] = ".st";
	const size_t bname_len = strlen(bname);
	char *buf = malloc(bname_len + sizeof(suffix));
	if (!buf) {
		perror("stfile_open");
		abort();
	}
	memcpy(buf, bname, bname_len);
	memcpy(buf + bname_len, suffix, sizeof(suffix));
	return buf;
}


static
int
stfile_unlink(const char *bname)
{
	char *stname = stfile_makename(bname);
	int ret = unlink(stname);
	free(stname);
	return ret;
}

static
int
stfile_access(const char *bname, int mode)
{
	char *stname = stfile_makename(bname);
	int ret = access(stname, mode);
	free(stname);
	return ret;
}


static
int
stfile_open(const char *bname, int flags, mode_t mode)
{
	char *stname = stfile_makename(bname);
	int fd = open(stname, flags, mode);
	free(stname);
	return fd;
}


/* Create a new flux_t structure */
flux_t *
flux_new(conf_t *conf, int count, const search_t *res)
{
	flux_t *flux;
	int status;
	url_t *u;
	char *s;
	int i;

	if (!count || !res)
		return NULL;

	flux = calloc(1, sizeof(flux_t));
	if (!flux)
		goto nomem;

	flux->conf = conf;
	flux->conn = calloc(flux->conf->num_connections, sizeof(conn_t));
	if (!flux->conn)
		goto nomem;

	for (i = 0; i < flux->conf->num_connections; i++)
		pthread_mutex_init(&flux->conn[i].lock, NULL);

	if (flux->conf->max_speed > 0) {
		/* max_speed / buffer_size < .5 */
		if (16 * flux->conf->max_speed / flux->conf->buffer_size < 8) {
			if (flux->conf->verbose >= 2)
				flux_message(flux,
					     _("Buffer resized for this speed."));
			flux->conf->buffer_size = flux->conf->max_speed;
		}
		uint64_t delay =
			UINT64_C(1073741824) * flux->conf->buffer_size *
			flux->conf->num_connections / flux->conf->max_speed;

		flux->delay_time.tv_sec  = delay / 1073741824;
		flux->delay_time.tv_nsec = delay % 1073741824;
	}
	if (buffer == NULL) {
		buffer = malloc(flux->conf->buffer_size);
		if (!buffer)
			goto nomem;
	}

	u = malloc(sizeof(url_t) * count);
	if (!u)
		goto nomem;
	flux->url = u;

	for (i = 0; i < count; i++) {
		strlcpy(u[i].text, res[i].url, sizeof(u[i].text));
		u[i].next = &u[i + 1];
	}
	u[count - 1].next = u;

	flux->conn[0].conf = flux->conf;
	if (!conn_set(&flux->conn[0], flux->url->text)) {
		flux_message(flux, _("Could not parse URL.\n"));
		flux->ready = -1;
		return flux;
	}

	flux->conn[0].local_if = flux->conf->interfaces->text;
	flux->conf->interfaces = flux->conf->interfaces->next;

	strlcpy(flux->filename, flux->conn[0].file, sizeof(flux->filename));
	http_decode(flux->filename);

	if ((s = strchr(flux->filename, '?')) != NULL &&
	    flux->conf->strip_cgi_parameters)
		*s = 0;		/* Get rid of CGI parameters */

	if (*flux->filename == 0)	/* Index page == no fn */
		strlcpy(flux->filename, flux->conf->default_filename,
			sizeof(flux->filename));

	if (flux->conf->no_clobber && access(flux->filename, F_OK) == 0) {
		int ret = stfile_access(flux->filename, F_OK);
		if (ret) {
			printf(_("File '%s' already there; not retrieving.\n"),
			       flux->filename);
			flux->ready = -1;
			return flux;
		}
		printf(_("Incomplete download found, ignoring "
			 "no-clobber option\n"));
	}

	do {
		if (!conn_init(&flux->conn[0])) {
			flux_message(flux, "%s", flux->conn[0].message);
			flux->ready = -1;
			return flux;
		}

		/* This does more than just checking the file size, it all
		 * depends on the protocol used. */
		status = conn_info(&flux->conn[0]);
		if (!status) {
			char msg[80];
			int code = conn_info_status_get(msg, sizeof(msg), flux->conn);
			fprintf(stderr, _("ERROR %d: %s.\n"), code, msg);
			flux->ready = -1;
			return flux;
		}
	} while (status == -1); /* re-init in case of protocol change. This can
				 * happen only once because the FTP protocol
				 * can't redirect back to HTTP */

	conn_url(flux->url->text, sizeof(flux->url->text) - 1, flux->conn);
	flux->size = flux->conn[0].size;
	if (flux->conf->verbose > 0) {
		if (flux->size != LLONG_MAX) {
			char hsize[32];
			flux_size_human(hsize, sizeof(hsize), flux->size);
			flux_message(flux, _("File size: %s (%jd bytes)"),
				     hsize, flux->size);
		} else {
			flux_message(flux, _("File size: unavailable"));
		}
	}

	/* Wildcards in URL --> Get complete filename */
	if (flux->filename[strcspn(flux->filename, "*?")])
		strlcpy(flux->filename, flux->conn[0].file,
			sizeof(flux->filename));

	if (*flux->conn[0].output_filename != 0) {
		strlcpy(flux->filename, flux->conn[0].output_filename,
			sizeof(flux->filename));
	}

	return flux;
 nomem:
	flux_close(flux);
	printf("%s\n", strerror(errno));
	return NULL;
}

/* Open a local file to store the downloaded data */
int
flux_open(flux_t *flux)
{
	int i, fd;
	ssize_t nread;

	if (flux->conf->verbose > 0)
		flux_message(flux, _("Opening output file %s"), flux->filename);

	flux->outfd = -1;

	/* Check whether server knows about RESTart and switch back to
	   single connection download if necessary */
	if (!flux->conn[0].supported) {
		flux_message(flux, _("Server unsupported, "
				     "starting from scratch with one connection."));
		flux->conf->num_connections = 1;
		void *new_conn = realloc(flux->conn, sizeof(conn_t));
		if (!new_conn)
			return 0;

		flux->conn = new_conn;
		flux_divide(flux);
	} else if ((fd = stfile_open(flux->filename, O_RDONLY, 0)) != -1) {
		int old_format = 0;
		off_t stsize = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);

		nread = read(fd, &flux->conf->num_connections,
			     sizeof(flux->conf->num_connections));
		if (nread != sizeof(flux->conf->num_connections)) {
			printf(_("%s.st: Error, truncated state file\n"),
			       flux->filename);
			close(fd);
			return 0;
		}

		if (flux->conf->num_connections < 1) {
			fprintf(stderr,
				_("Bogus number of connections stored in state file\n"));
			close(fd);
			return 0;
		}

		if (stsize < (off_t)(sizeof(flux->conf->num_connections) +
				     sizeof(flux->bytes_done) +
				     2 * flux->conf->num_connections *
				     sizeof(flux->conn[0].currentbyte))) {
			/* FIXME this might be wrong, the file may have been
			 * truncated, we need another way to check. */
#ifndef NDEBUG
			printf(_("State file has old format.\n"));
#endif
			old_format = 1;
		}

		void *new_conn = realloc(flux->conn, sizeof(conn_t) *
					 flux->conf->num_connections);
		if (!new_conn) {
			close(fd);
			return 0;
		}
		flux->conn = new_conn;

		memset(flux->conn + 1, 0,
		       sizeof(conn_t) * (flux->conf->num_connections - 1));

		if (old_format)
			flux_divide(flux);

		nread = read(fd, &flux->bytes_done, sizeof(flux->bytes_done));
		assert(nread == sizeof(flux->bytes_done));
		for (i = 0; i < flux->conf->num_connections; i++) {
			nread = read(fd, &flux->conn[i].currentbyte,
				     sizeof(flux->conn[i].currentbyte));
			assert(nread == sizeof(flux->conn[i].currentbyte));
			if (!old_format) {
				nread = read(fd, &flux->conn[i].lastbyte,
					     sizeof(flux->conn[i].lastbyte));
				assert(nread == sizeof(flux->conn[i].lastbyte));
			}
		}

		flux_message(flux,
			     _("State file found: %jd bytes downloaded, %jd to go."),
			     flux->bytes_done, flux->size - flux->bytes_done);

		close(fd);

		if ((flux->outfd = open(flux->filename, O_WRONLY, 0666)) == -1) {
			flux_message(flux, _("Error opening local file"));
			return 0;
		}
	}

	/* If outfd == -1 we have to start from scrath now */
	if (flux->outfd == -1) {
		flux_divide(flux);

		if ((flux->outfd =
		     open(flux->filename, O_CREAT | O_WRONLY, 0666)) == -1) {
			flux_message(flux, _("Error opening local file"));
			return 0;
		}

		/* And check whether the filesystem can handle seeks to
		   past-EOF areas.. Speeds things up. :) AFAIK this
		   should just not happen: */
		if (lseek(flux->outfd, flux->size, SEEK_SET) == -1 &&
		    flux->conf->num_connections > 1) {
			/* But if the OS/fs does not allow to seek behind
			   EOF, we have to fill the file with zeroes before
			   starting. Slow.. */
			flux_message(flux,
				     _("Crappy filesystem/OS.. Working around. :-("));
			lseek(flux->outfd, 0, SEEK_SET);
			memset(buffer, 0, flux->conf->buffer_size);
			off_t j = flux->size;
			while (j > 0) {
				ssize_t nwrite;

				if ((nwrite =
				     write(flux->outfd, buffer,
					   min(j, flux->conf->buffer_size))) < 0) {
					if (errno == EINTR || errno == EAGAIN)
						continue;
					flux_message(flux,
						     _("Error creating local file"));
					return 0;
				}
				j -= nwrite;
			}
		}
	}

	return 1;
}

/**
 * Steals half of the largest available chunk of work of at least
 * MIN_CHUNK_WORTH size, from an active connection to feed a finished one.
 *
 * Must be called with the conn_t lock held.
 */
static
void
reactivate_connection(flux_t *flux, int thread)
{
	/* TODO Make the minimum also depend on the connection speed */
	off_t max_remaining = MIN_CHUNK_WORTH - 1;
	int idx = -1;

	if (flux->conn[thread].enabled ||
	    flux->conn[thread].currentbyte < flux->conn[thread].lastbyte)
		return;

	for (int j = 0; j < flux->conf->num_connections; j++) {
		off_t remaining =
			flux->conn[j].lastbyte - flux->conn[j].currentbyte;
		if (remaining > max_remaining) {
			max_remaining = remaining;
			idx = j;
		}
	}

	if (idx == -1)
		return;
#ifndef NDEBUG
	printf(_("\nReactivate connection %d\n"), thread);
#endif
	flux->conn[thread].lastbyte = flux->conn[idx].lastbyte;
	flux->conn[idx].lastbyte = flux->conn[idx].currentbyte
		+ max_remaining / 2;
	flux->conn[thread].currentbyte = flux->conn[idx].lastbyte;
}

/* Start downloading */
void
flux_start(flux_t *flux)
{
	int i;
	url_t *url_ptr;

	/* HTTP might've redirected and FTP handles wildcards, so
	   re-scan the URL for every conn */
	url_ptr = flux->url;
	for (i = 0; i < flux->conf->num_connections; i++) {
		conn_set(&flux->conn[i], url_ptr->text);
		url_ptr = url_ptr->next;
		flux->conn[i].local_if = flux->conf->interfaces->text;
		flux->conf->interfaces = flux->conf->interfaces->next;
		flux->conn[i].conf = flux->conf;
		if (i)
			flux->conn[i].supported = true;
	}

	if (flux->conf->verbose > 0)
		flux_message(flux, _("Starting download"));

	for (i = 0; i < flux->conf->num_connections; i++) {
		if (flux->conn[i].currentbyte >= flux->conn[i].lastbyte) {
			pthread_mutex_lock(&flux->conn[i].lock);
			reactivate_connection(flux, i);
			pthread_mutex_unlock(&flux->conn[i].lock);
		} else if (flux->conn[i].currentbyte < flux->conn[i].lastbyte) {
			if (flux->conf->verbose >= 2) {
				flux_message(flux,
					     _("Connection %i downloading from %s:%i using interface %s"),
					     i, flux->conn[i].host,
					     flux->conn[i].port,
					     flux->conn[i].local_if);
			}

			flux->conn[i].state = true;
			if (pthread_create
			    (flux->conn[i].setup_thread, NULL, setup_thread,
			     &flux->conn[i]) != 0) {
				flux_message(flux, _("pthread error!!!"));
				flux->ready = -1;
			}
		}
	}

	/* The real downloading will start now, so let's start counting */
	flux->start_time = flux_gettime();
	flux->ready = 0;
}

/* Main 'loop' */
void
flux_do(flux_t *flux)
{
	fd_set fds[1];
	int hifd, i;
	off_t remaining, size;
	struct timeval timeval[1];
	url_t *url_ptr;
	struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
	unsigned long long int max_speed_ratio;

	/* Create statefile if necessary */
	if (flux_gettime() > flux->next_state) {
		save_state(flux);
		flux->next_state = flux_gettime() + flux->conf->save_state_interval;
	}

	/* Wait for data on (one of) the connections */
	FD_ZERO(fds);
	hifd = 0;
	for (i = 0; i < flux->conf->num_connections; i++) {
		/* skip connection if setup thread hasn't released the lock yet */
		if (!pthread_mutex_trylock(&flux->conn[i].lock)) {
			if (flux->conn[i].enabled) {
				FD_SET(flux->conn[i].tcp->fd, fds);
				hifd = max(hifd, flux->conn[i].tcp->fd);
			}
			pthread_mutex_unlock(&flux->conn[i].lock);
		}
	}
	if (hifd == 0) {
		/* No connections yet. Wait... */
		if (flux_sleep(delay) < 0) {
			flux_message(flux,
				     _("Error while waiting for connection: %s"),
				     strerror(errno));
			flux->ready = -1;
			return;
		}
		goto conn_check;
	}

	timeval->tv_sec = 0;
	timeval->tv_usec = 100000;
	if (select(hifd + 1, fds, NULL, NULL, timeval) == -1) {
		/* A select() error probably means it was interrupted
		 * by a signal, or that something else's very wrong... */
		flux->ready = -1;
		return;
	}

	/* Handle connections which need attention */
	for (i = 0; i < flux->conf->num_connections; i++) {
		/* skip connection if setup thread hasn't released the lock yet */
		if (pthread_mutex_trylock(&flux->conn[i].lock))
			continue;

		if (!flux->conn[i].enabled)
			goto next_conn;

		if (!FD_ISSET(flux->conn[i].tcp->fd, fds)) {
			time_t timeout = flux->conn[i].last_transfer +
			    flux->conf->connection_timeout;
			if (flux_gettime() > timeout) {
				if (flux->conf->verbose)
					flux_message(flux,
						     _("Connection %i timed out"),
						     i);
				conn_disconnect(&flux->conn[i]);
			}
			goto next_conn;
		}

		flux->conn[i].last_transfer = flux_gettime();
		size =
		    tcp_read(flux->conn[i].tcp, buffer,
			     flux->conf->buffer_size);
		if (size == -1) {
			if (flux->conf->verbose) {
				flux_message(flux, _("Error on connection %i! "
						     "Connection closed"), i);
			}
			conn_disconnect(&flux->conn[i]);
			goto next_conn;
		}

		if (size == 0) {
			if (flux->conf->verbose) {
				/* Only abnormal behaviour if: */
				if (flux->conn[i].currentbyte <
				    flux->conn[i].lastbyte &&
				    flux->size != LLONG_MAX) {
					flux_message(flux,
						     _("Connection %i unexpectedly closed"),
						     i);
				} else {
					flux_message(flux,
						     _("Connection %i finished"),
						     i);
				}
			}
			if (!flux->conn[0].supported) {
				flux->ready = 1;
			}
			conn_disconnect(&flux->conn[i]);
			reactivate_connection(flux, i);
			goto next_conn;
		}

		/* remaining == Bytes to go */
		remaining = flux->conn[i].lastbyte - flux->conn[i].currentbyte;
		if (remaining < size) {
			if (flux->conf->verbose) {
				flux_message(flux, _("Connection %i finished"),
					     i);
			}
			conn_disconnect(&flux->conn[i]);
			size = remaining;
			/* Don't terminate, still stuff to write! */
		}
		/* This should always succeed.. */
		lseek(flux->outfd, flux->conn[i].currentbyte, SEEK_SET);
		if (write(flux->outfd, buffer, size) != size) {
			flux_message(flux, _("Write error!"));
			flux->ready = -1;
			pthread_mutex_unlock(&flux->conn[i].lock);
			return;
		}
		flux->conn[i].currentbyte += size;
		flux->bytes_done += size;
		if (remaining == size)
			reactivate_connection(flux, i);

 next_conn:
		pthread_mutex_unlock(&flux->conn[i].lock);
	}

	if (flux->ready)
		return;

 conn_check:
	/* Look for aborted connections and attempt to restart them. */
	url_ptr = flux->url;
	for (i = 0; i < flux->conf->num_connections; i++) {
		/* skip connection if setup thread hasn't released the lock yet */
		if (pthread_mutex_trylock(&flux->conn[i].lock))
			continue;

		if (!flux->conn[i].enabled &&
		    flux->conn[i].currentbyte < flux->conn[i].lastbyte) {
			if (!flux->conn[i].state) {
				// Wait for termination of this thread
				pthread_join(*(flux->conn[i].setup_thread),
					     NULL);

				conn_set(&flux->conn[i], url_ptr->text);
				url_ptr = url_ptr->next;
				/* flux->conn[i].local_if = flux->conf->interfaces->text;
				   flux->conf->interfaces = flux->conf->interfaces->next; */
				if (flux->conf->verbose >= 2)
					flux_message(flux,
						     _("Connection %i downloading from %s:%i using interface %s"),
						     i, flux->conn[i].host,
						     flux->conn[i].port,
						     flux->conn[i].local_if);

				flux->conn[i].state = true;
				if (pthread_create
				    (flux->conn[i].setup_thread, NULL,
				     setup_thread, &flux->conn[i]) == 0) {
					flux->conn[i].last_transfer = flux_gettime();
				} else {
					flux_message(flux,
						     _("pthread error!!!"));
					flux->ready = -1;
				}
			} else {
				if (flux_gettime() > (flux->conn[i].last_transfer +
						 flux->conf->reconnect_delay)) {
					pthread_cancel(*flux->conn[i].setup_thread);
					flux->conn[i].state = false;
					pthread_join(*flux->conn[i].
						     setup_thread, NULL);
				}
			}
		}
		pthread_mutex_unlock(&flux->conn[i].lock);
	}

	/* Calculate current average speed and finish_time */
	flux->bytes_per_second =
	    (off_t)((double)(flux->bytes_done - flux->start_byte) /
		  (flux_gettime() - flux->start_time));
	if (flux->bytes_per_second != 0)
		flux->finish_time =
		    (int)(flux->start_time +
			  (double)(flux->size - flux->start_byte) /
			  flux->bytes_per_second);
	else
		flux->finish_time = INT_MAX;

	/* Check speed. If too high, delay for some time to slow things
	   down a bit. I think a 5% deviation should be acceptable. */
	if (flux->conf->max_speed > 0) {
		max_speed_ratio = 1000 * flux->bytes_per_second /
		    flux->conf->max_speed;
		if (max_speed_ratio > 1050) {
			flux->delay_time.tv_nsec += 10000000;
			if (flux->delay_time.tv_nsec >= 1000000000) {
				flux->delay_time.tv_sec++;
				flux->delay_time.tv_nsec -= 1000000000;
			}
		} else if (max_speed_ratio < 950) {
			if (flux->delay_time.tv_nsec >= 10000000) {
				flux->delay_time.tv_nsec -= 10000000;
			} else if (flux->delay_time.tv_sec > 0) {
				flux->delay_time.tv_sec--;
				flux->delay_time.tv_nsec += 999000000;
			} else {
				flux->delay_time.tv_sec = 0;
				flux->delay_time.tv_nsec = 0;
			}
		}
		if (flux_sleep(flux->delay_time) < 0) {
			flux_message(flux,
				     _("Error while enforcing throttling: %s"),
				     strerror(errno));
			flux->ready = -1;
			return;
		}
	}

	/* Ready? */
	if (flux->bytes_done == flux->size)
		flux->ready = 1;
}

/* Close an flux connection */
void
flux_close(flux_t *flux)
{
	if (!flux)
		return;

	/* this function can't be called with a partly initialized flux */
	assert(flux->conn);

	/* Terminate threads and close connections */
	for (int i = 0; i < flux->conf->num_connections; i++) {
		/* don't try to kill non existing thread */
		if (*flux->conn[i].setup_thread != 0) {
			pthread_cancel(*flux->conn[i].setup_thread);
			pthread_join(*flux->conn[i].setup_thread, NULL);
		}
		conn_disconnect(&flux->conn[i]);
	}

	free(flux->url);

	/* Delete state file if necessary */
	if (flux->ready == 1) {
		stfile_unlink(flux->filename);
	}
	/* Else: Create it.. */
	else if (flux->bytes_done > 0) {
		save_state(flux);
	}

	print_messages(flux);

	close(flux->outfd);

	if (!PROTO_IS_FTP(flux->conn->proto) || flux->conn->proxy) {
		abuf_setup(flux->conn->http->request, ABUF_FREE);
		abuf_setup(flux->conn->http->headers, ABUF_FREE);
	}
	free(flux->conn);
	free(flux);
	/* Reset global so the next download re-allocates. See url-glob multi-download. */
	free(buffer);
	buffer = NULL;
}

/* time() with more precision */
double
flux_gettime(void)
{
	struct timeval time[1];

	gettimeofday(time, NULL);
	return (double)time->tv_sec + (double)time->tv_usec / 1000000;
}

/**
 * Save the state of the current download.
 */
static
void
save_state(flux_t *flux)
{
	/* No use for such a file if the server doesn't support
	   resuming anyway.. */
	if (!flux->conn[0].supported)
		return;

	int fd;
	fd = stfile_open(flux->filename, O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (fd == -1) {
		return;		/* Not 100% fatal.. */
	}

	ssize_t nwrite;
	(void)nwrite; /* workaround unused variable warning */
	nwrite =
	    write(fd, &flux->conf->num_connections,
		  sizeof(flux->conf->num_connections));
	assert(nwrite == sizeof(flux->conf->num_connections));

	nwrite = write(fd, &flux->bytes_done, sizeof(flux->bytes_done));
	assert(nwrite == sizeof(flux->bytes_done));

	for (int i = 0; i < flux->conf->num_connections; i++) {
		nwrite =
		    write(fd, &flux->conn[i].currentbyte,
			  sizeof(flux->conn[i].currentbyte));
		assert(nwrite == sizeof(flux->conn[i].currentbyte));
		nwrite =
		    write(fd, &flux->conn[i].lastbyte,
			  sizeof(flux->conn[i].lastbyte));
		assert(nwrite == sizeof(flux->conn[i].lastbyte));
	}
	close(fd);
}

/* Thread used to set up a connection */
static
void *
setup_thread(void *c)
{
	conn_t *conn = c;
	int oldstate;

	/* Allow this thread to be killed at any time. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);

	pthread_mutex_lock(&conn->lock);
	if (conn_setup(conn)) {
		conn->last_transfer = flux_gettime();
		if (conn_exec(conn)) {
			conn->last_transfer = flux_gettime();
			conn->enabled = true;
			goto out;
		}
	}

	conn_disconnect(conn);
 out:
	conn->state = false;
	pthread_mutex_unlock(&conn->lock);

	return NULL;
}

/* Add a message to the flux->message structure */
static void
flux_message(flux_t *flux, const char *format, ...)
{
	message_t *m;
	va_list params;

	if (!flux)
		goto nomem;

	m = calloc(1, sizeof(message_t));
	if (!m)
		goto nomem;

	va_start(params, format);
	vsnprintf(m->text, MAX_STRING, format, params);
	va_end(params);

	if (flux->message == NULL) {
		flux->message = flux->last_message = m;
	} else {
		flux->last_message->next = m;
		flux->last_message = m;
	}

	return;

 nomem:
	/* Flush previous messages */
	print_messages(flux);
	va_start(params, format);
	vprintf(format, params);
	va_end(params);
}

/* Divide the file and set the locations for each connection */
static void
flux_divide(flux_t *flux)
{
	/* Optimize the number of connections in case the file is small */
	off_t maxconns = max(1u, flux->size / MIN_CHUNK_WORTH);
	if (maxconns < flux->conf->num_connections)
		flux->conf->num_connections = maxconns;

	/* Calculate each segment's size */
	off_t seg_len = flux->size / flux->conf->num_connections;

	if (!seg_len) {
		printf(_("Too few bytes remaining, forcing a single connection\n"));
		flux->conf->num_connections = 1;
		seg_len = flux->size;

		conn_t *new_conn = realloc(flux->conn, sizeof(*flux->conn));
		if (new_conn)
			flux->conn = new_conn;
	}

	for (int i = 0; i < flux->conf->num_connections; i++) {
		flux->conn[i].currentbyte = seg_len * i;
		flux->conn[i].lastbyte    = seg_len * i + seg_len;
	}

	/* Last connection downloads remaining bytes */
	size_t tail = flux->size % seg_len;
	flux->conn[flux->conf->num_connections - 1].lastbyte += tail;
#ifndef NDEBUG
	for (int i = 0; i < flux->conf->num_connections; i++) {
		printf(_("Downloading %jd-%jd using conn. %i\n"),
		       flux->conn[i].currentbyte,
		       flux->conn[i].lastbyte, i);
	}
#endif
}
