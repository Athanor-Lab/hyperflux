bin_PROGRAMS = flux
flux_SOURCES = \
	compat-android.h \
	compat-bsd.h \
	compat-ssl.h \
	src/abuf.c \
	src/abuf.h \
	src/flux.c \
	src/flux.h \
	src/sleep.h \
	src/sleep.c \
	src/conf.c \
	src/conf.h \
	src/conn.c \
	src/conn.h \
	src/extractor.c \
	src/extractor.h \
	src/ftp.c \
	src/ftp.h \
	src/hash.c \
	src/hash.h \
	src/http.c \
	src/http.h \
	src/random.c \
	src/scan.c \
	src/scan.h \
	src/search.c \
	src/search.h \
	src/ssl.h \
	src/tcp.c \
	src/tcp.h \
	src/tui.c \
	src/tui.h \
	src/url_glob.c \
	src/url_glob.h \
	src/text.c

flux_CFLAGS = $(AM_CFLAGS) $(PTHREAD_CFLAGS)
AM_CFLAGS = $(WARN_CFLAGS) \
	-Wno-declaration-after-statement \
	-Wno-error=cast-align \
	-Wno-error=inline

if WITH_SSL
flux_SOURCES += \
	src/dn-match.c \
	src/hls.c \
	src/hls.h \
	src/ssl.c \
	src/ssl_verify.c
AM_CFLAGS += $(SSL_CFLAGS)
endif

flux_LDADD = $(LIBOBJS) $(SSL_LIBS) $(LIBINTL) $(PTHREAD_LIBS)
flux_CC = $(PTHREAD_CC)

AM_CPPFLAGS = -DLOCALEDIR=\""$(localedir)"\"
AM_CPPFLAGS += -D_BSD_SOURCE -D_DEFAULT_SOURCE
# HLS links the rest of flux (http_fetch, conf_t) and OpenSSL EVP; only hls.c
# (built under WITH_SSL) reads this. The pure parsing/crypto half compiles
# standalone without it for the unit test.
AM_CPPFLAGS += -DHLS_HAVE_FLUX

# The scanner's HLS duration enrichment links the pure half of hls.c, which is
# only compiled with SSL; gate those calls so the non-SSL build still links.
if WITH_SSL
AM_CPPFLAGS += -DSCAN_HAVE_HLS
endif
