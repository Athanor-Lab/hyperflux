# Hyperflux - Lightweight CLI download accelerator

## About

Hyperflux tries to accelerate the download process by using multiple
connections per file, and can also balance the load between
different servers.

Hyperflux tries to be as light as possible, so it might be useful on
byte-critical systems.

Hyperflux supports HTTP, HTTPS, FTP and FTPS protocols.

A single URL argument may contain a range or list pattern, which Hyperflux
expands into several separate downloads run one after another, each still
accelerated with multiple connections. Multiple distinct URL arguments are
still treated as mirrors of one file. Both brace forms (`{a,b,c}`,
`{N..M}`, `{N..M..S}`, `{a..z}`) and bracket forms (`[N-M]`, `[N-M:S]`,
`[a-z]`, `[a-z:S]`) are supported. Quote the URL so the shell does not
expand the braces first, for example:

    flux 'http://example.com/video[01-12].mp4'

See `man flux` for the full description, including how `-o` handles
expanded downloads.

## Usage

For usage information, see the manual page:

    man flux

## Installation

The distro package is named `hyperflux`, but the command it installs is `flux`.

### Arch / Manjaro / EndeavourOS

Available from the AUR. With an AUR helper such as `yay`:

    yay -S hyperflux        # latest stable release
    yay -S hyperflux-git    # latest from git

The installed binary is `flux`.

### Gentoo

Hyperflux ships an ebuild in `packaging/gentoo` and is heading to the GURU
overlay. Add the overlay and install:

    eselect repository enable guru
    emaint sync -r guru
    emerge net-misc/hyperflux

### Debian / Ubuntu

Add the apt repository (this also gets you future updates):

    curl -1sLf 'https://dl.cloudsmith.io/public/hyperflux/athanor-lab/gpg.1C966CF1BD82624F.key' | sudo gpg --dearmor -o /usr/share/keyrings/hyperflux-athanor-lab-archive-keyring.gpg
    echo "deb [signed-by=/usr/share/keyrings/hyperflux-athanor-lab-archive-keyring.gpg] https://dl.cloudsmith.io/public/hyperflux/athanor-lab/deb/any-distro any-version main" | sudo tee /etc/apt/sources.list.d/hyperflux.list
    sudo apt update && sudo apt install hyperflux

Or grab a single `.deb` from the
[GitHub Releases](https://github.com/Athanor-Lab/hyperflux/releases) page and
install it directly:

    sudo apt install ./hyperflux_1.0.0_amd64.deb

### Fedora / RHEL

Add the dnf/yum repository (this also gets you future updates):

    sudo tee /etc/yum.repos.d/hyperflux.repo >/dev/null <<'EOF'
    [hyperflux-athanor-lab]
    name=hyperflux-athanor-lab
    baseurl=https://dl.cloudsmith.io/public/hyperflux/athanor-lab/rpm/any-distro/any-version/$basearch
    gpgkey=https://dl.cloudsmith.io/public/hyperflux/athanor-lab/gpg.1C966CF1BD82624F.key
    gpgcheck=1
    repo_gpgcheck=1
    enabled=1
    EOF
    sudo dnf install hyperflux

Or grab a single `.rpm` from the
[GitHub Releases](https://github.com/Athanor-Lab/hyperflux/releases) page and
install it directly:

    sudo dnf install ./hyperflux-1.0.0.x86_64.rpm

### Alpine

Add the apk repository (this also gets you future updates):

    curl -1sLf 'https://dl.cloudsmith.io/public/hyperflux/athanor-lab/rsa.7F72E89045765348.key' | sudo tee /etc/apk/keys/athanor-lab@hyperflux-7F72E89045765348.rsa.pub >/dev/null
    echo 'https://dl.cloudsmith.io/public/hyperflux/athanor-lab/alpine/any-version/main' | sudo tee -a /etc/apk/repositories
    sudo apk update && sudo apk add hyperflux

Or grab a single `.apk` from the
[GitHub Releases](https://github.com/Athanor-Lab/hyperflux/releases) page and
install it directly:

    sudo apk add --allow-untrusted ./hyperflux_1.0.0.apk

### Build from source

Install the build dependencies (a C compiler, `make`, `autoconf`, `automake`,
`libtool`, `autoconf-archive`, `gettext`, `pkg-config`, the OpenSSL development
headers and `txt2man`), then build:

    autoreconf -fi          # or ./autogen.sh; skip this on a release tarball
    ./configure --disable-Werror
    make
    sudo make install

A release tarball already ships `configure`, so the `autoreconf` step is only
needed from a git checkout. The installed command is `flux`.

## How to help
If you can code and are interested in improving Hyperflux, please read the
[CONTRIBUTING.md](CONTRIBUTING.md) file; if you're looking for ideas check the
project page at <https://example.invalid/hyperflux>.

## Installing from binaries
Your operating system may contain a precompiled version of Hyperflux, and if so you
should probably use it.  If the package is outdated please get in touch with the
package maintainer or open a support ticket with your distro.

## Building from source
WARNING: Building from the source code repository is recommended only when doing
development, otherwise only use release tarballs.

Hyperflux uses GNU autotools for it's buildsystem; instructions are provided in the
[INSTALL](INSTALL) file. The basic actions for most users are:

    ./configure && make && make install

To build without SSL/TLS support, pass to `configure` the `--without-ssl` flag.

If you're working from the source code repository instead of a release tarball,
you need to generate the buildsystem first with:

    autoreconf -i

When working from a git repository the build system will detect that and will
add -Werror to the CFLAGS if supported; so if you're not doing development you
should probably consider passing `--disable-Werror` to `configure` in order to
prevent build failures due to mere warnings.

### Dependencies
* `gettext` (or `gettext-tiny`)
* `pkg-config`

Optional:

* `libssl` (OpenSSL, LibreSSL or compatible) -- for SSL/TLS support.

#### Extra dependencies for building from snapshots
* `autoconf-archive`
* `autoconf`
* `automake`
* `autopoint`
* `txt2man`

#### Packages on Debian-based systems
* `build-essential`
* `autoconf`
* `autoconf-archive`
* `automake`
* `autopoint`
* `gettext`
* `libssl-dev`
* `pkg-config`
* `txt2man`


#### Packages on Mac OS X (Homebrew)
* `autoconf-archive`
* `automake`
* `gettext`
* `openssl`

### Building on Mac OS X (Homebrew)

You'll need to provide some extra options to autotools so it can find gettext
and openssl.

	GETTEXT=/usr/local/opt/gettext
	OPENSSL=/usr/local/opt/openssl
	PATH="$GETTEXT/bin:$PATH"

	[ -x configure ] || autoreconf -fiv -I$GETTEXT/share/aclocal/

	CFLAGS="-I$GETTEXT/include -I$OPENSSL/include" \
	LDFLAGS=-L$GETTEXT/lib ./configure

You can just run `make` as usual after these steps.

## Related projects ##

* [aria2](https://github.com/aria2/aria2)
* [hget](https://github.com/huydx/hget)
* [lftp](https://github.com/lavv17/lftp)
* [nugget](https://github.com/maxogden/nugget)
* [pget](https://github.com/Code-Hex/pget)

## License ##

Hyperflux is licensed under GPL-2+ with the OpenSSL exception.

## Credits ##

Hyperflux is a fork of [Axel](https://github.com/axel-download-accelerator/axel).
Axel was originally written by Wilmer van der Gaast and developed by many
contributors over the years; the full list lives in the [AUTHORS](AUTHORS) file.
Thanks to all of them for the work Hyperflux builds on.

Hyperflux is licensed under GPL-2.0-or-later (with the OpenSSL exception). See
the [COPYING](COPYING) file for the full license text.
