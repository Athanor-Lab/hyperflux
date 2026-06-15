# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit autotools

DESCRIPTION="Lightweight CLI download accelerator (fork of Axel) with URL range/list expansion"
HOMEPAGE="https://github.com/Athanor-Lab/hyperflux"

# The distro package is "hyperflux" but the release tarball and build dir use
# the upstream tarname "flux" (bin is /usr/bin/flux).
SRC_URI="https://github.com/Athanor-Lab/hyperflux/releases/download/v${PV}/flux-${PV}.tar.gz"
S="${WORKDIR}/flux-${PV}"

LICENSE="GPL-2+"
SLOT="0"
KEYWORDS="~amd64 ~arm64"

DEPEND="dev-libs/openssl:="
RDEPEND="${DEPEND}"
BDEPEND="
	sys-devel/gettext
	sys-devel/autoconf-archive
	app-text/txt2man
	virtual/pkgconfig
"

src_prepare() {
	default
	eautoreconf
}

src_configure() {
	# configure.ac uses AX_COMPILER_FLAGS, which turns -Werror on by default;
	# --disable-Werror keeps the build from failing on benign warnings.
	econf --disable-Werror
}

src_install() {
	default
	dodoc doc/fluxrc.example
}
