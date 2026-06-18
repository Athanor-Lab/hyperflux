Name:           hyperflux
Version:        1.0.0
Release:        1%{?dist}
Summary:        Lightweight CLI download accelerator with URL range and list expansion

# The program is GPL-2.0-or-later with an OpenSSL linking exception.
# Fedora's short identifier for GPLv2+ is "GPLv2+".
License:        GPLv2+
URL:            https://github.com/Athanor-Lab/hyperflux

# The release tarball is produced by "make dist" and is named after
# PACKAGE_TARNAME ("flux"), so it unpacks to flux-%{version}/ even though
# the distro package is called hyperflux.
Source0:        %{url}/releases/download/v%{version}/flux-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  libtool
BuildRequires:  autoconf-archive
BuildRequires:  gettext
BuildRequires:  pkgconfig
BuildRequires:  openssl-devel
BuildRequires:  txt2man

# Runtime TLS support. Auto-generated library deps already pull this in,
# but stating it makes the dependency explicit.
Requires:       openssl-libs

%description
Hyperflux is a lightweight command line download accelerator and a fork of
Axel. It speeds up downloads by opening multiple connections to a server and
fetching different parts of the file in parallel. It also understands URL
expansion: a single URL containing {a,b}, {N..M} or [N-M] patterns is expanded
into separate downloads.

The installed command is "flux".

%prep
%autosetup -n flux-%{version}

%build
# AX_COMPILER_FLAGS enables -Werror by default; disable it so the build is
# not broken by harmless warnings on the build host's compiler.
%configure --disable-Werror
%make_build

%install
%make_install

# Collect the gettext catalogs into hyperflux.lang and reference it in %files.
%find_lang flux

%files -f flux.lang
%license COPYING
%doc doc/fluxrc.example
%{_bindir}/flux
%{_mandir}/man1/flux.1*
# Bundled extractor configs installed active into the discovery dir; match the
# Makefile.am dist_extractors_DATA path so rpmbuild from source stays packaged.
%dir %{_datadir}/hyperflux/extractors
%{_datadir}/hyperflux/extractors/*.conf

%changelog
* Mon Jun 15 2026 xAlcahest <xAlcahest@users.noreply.github.com> - 1.0.0-1
- Initial Hyperflux package (fork of Axel).
