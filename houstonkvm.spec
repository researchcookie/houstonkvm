# Builds offline: scripts/fetch-rpm-sources.sh downloads Source0-9 first.
# Needs EPEL and CRB. OpenH264 resolves to EPEL's noopenh264 stub; for H.264
# video, install Cisco's openh264 (epel-cisco-openh264) on the server.

Name:           houstonkvm
Version:        0.1.0
Release:        1%{?dist}
Summary:        Self-hosted multi-user IP-KVM server

# EL9 and Fedora have libdatachannel; EL10 doesn't yet, so it's bundled there.
%if 0%{?fedora} || 0%{?rhel} == 9
%bcond_without system_libdatachannel
%else
%bcond_with system_libdatachannel
%endif

# Apache-2.0: HoustonKVM, uSockets, uWebSockets. MIT: nlohmann/json, plog.
# OFL-1.1: the embedded web fonts. Bundled with libdatachannel: MPL-2.0
# (libdatachannel, libjuice), BSD-3-Clause (usrsctp, libsrtp).
# See THIRD_PARTY_NOTICES.md.
%if %{without system_libdatachannel}
License:        Apache-2.0 AND MIT AND BSD-3-Clause AND MPL-2.0 AND OFL-1.1
%else
License:        Apache-2.0 AND MIT AND OFL-1.1
%endif
URL:            https://github.com/researchcookie/houstonkvm
Source0:        %{url}/archive/v%{version}/houstonkvm-%{version}.tar.gz
# uSockets at the commit uWebSockets v20.79.0 uses as its submodule.
Source1:        https://github.com/uNetworking/uSockets/archive/86097c490263ab662d62e8e7b541390bdec7d149.tar.gz#/usockets-86097c4.tar.gz
Source2:        https://github.com/uNetworking/uWebSockets/archive/refs/tags/v20.79.0.tar.gz#/uwebsockets-20.79.0.tar.gz
# libdatachannel and its submodules, at the commits its v0.24.5 pins.
Source4:        https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.5.tar.gz#/libdatachannel-0.24.5.tar.gz
Source5:        https://github.com/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz#/plog-94899e0.tar.gz
Source6:        https://github.com/paullouisageneau/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz#/usrsctp-fec583d.tar.gz
Source7:        https://github.com/paullouisageneau/libjuice/archive/3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6.tar.gz#/libjuice-3c40a35.tar.gz
Source8:        https://github.com/nlohmann/json/archive/55f93686c01528224f448c19128836e7df245f72.tar.gz#/libdatachannel-json-55f9368.tar.gz
Source9:        https://github.com/cisco/libsrtp/archive/24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5.tar.gz#/libsrtp-24b3bf8.tar.gz
Source20:       houstonkvm.service
Source21:       houstonkvm-hid.service
Source22:       houstonkvm.conf
Source23:       99-houstonkvm-hid.rules
Source24:       setup-hid-gadget.sh
Source25:       houstonkvm.firewalld.xml
Source26:       houstonkvm.sysusers

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  sqlite-devel
BuildRequires:  openssl-devel
BuildRequires:  libsodium-devel
BuildRequires:  json-static
%if %{with system_libdatachannel}
BuildRequires:  libdatachannel-devel
%endif
BuildRequires:  turbojpeg-devel
BuildRequires:  pkgconfig(openh264)
BuildRequires:  alsa-lib-devel
BuildRequires:  pkgconfig(opus)
BuildRequires:  zlib-devel
BuildRequires:  kernel-headers
BuildRequires:  perl-IPC-Cmd
BuildRequires:  systemd-rpm-macros
BuildRequires:  firewalld-filesystem
# For %%check: the test suite, and the TLS tests' certificates.
BuildRequires:  python3
BuildRequires:  openssl

Requires:       firewalld-filesystem
%{?systemd_requires}
# RPM creates sysusers.d users before installing files only from 4.20.
%if 0%{?fedora} < 42
Requires(pre):  shadow-utils
%endif

Provides:       bundled(uSockets) = 0^git86097c4
Provides:       bundled(uWebSockets) = 20.79.0
%if %{without system_libdatachannel}
Provides:       bundled(libdatachannel) = 0.24.5
Provides:       bundled(libjuice) = 1.7.2
Provides:       bundled(libsrtp) = 2.8.0
Provides:       bundled(usrsctp) = 0.9.5.0
Provides:       bundled(plog) = 1.1.10
# The private libdatachannel.so must not satisfy, or be satisfied by, the
# system package.
%global __provides_exclude_from ^%{_libdir}/houstonkvm/.*$
%global __requires_exclude ^libdatachannel\\.so.*$
%endif

%description
HoustonKVM is a self-hosted, multi-user IP-KVM server. It captures a machine's
screen from a V4L2 device (an HDMI capture dongle, a webcam, or a VM through
v4l2loopback), streams it to the browser as MJPEG or over WebRTC, and sends
keyboard and mouse input back through a USB HID gadget, a CH9329
serial-to-USB-HID adapter, or QEMU's QMP socket. Viewers watch, Operators
drive and Owners administer; API tokens give scripts the same access. The web
interface is built into the executable.

%global deps %{_builddir}/houstonkvm-%{version}/deps
%global stage %{_builddir}/houstonkvm-%{version}/_stage

%prep
%autosetup
# Each GitHub archive unpacks into deps/<name>. libdatachannel's archive has
# empty directories where its submodules go; theirs fill them.
mkdir -p %{deps}/usockets %{deps}/uwebsockets
tar -xzf %{SOURCE1} -C %{deps}/usockets --strip-components=1
tar -xzf %{SOURCE2} -C %{deps}/uwebsockets --strip-components=1
%if %{without system_libdatachannel}
mkdir -p %{deps}/libdatachannel
tar -xzf %{SOURCE4} -C %{deps}/libdatachannel --strip-components=1
tar -xzf %{SOURCE5} -C %{deps}/libdatachannel/deps/plog --strip-components=1
tar -xzf %{SOURCE6} -C %{deps}/libdatachannel/deps/usrsctp --strip-components=1
tar -xzf %{SOURCE7} -C %{deps}/libdatachannel/deps/libjuice --strip-components=1
tar -xzf %{SOURCE8} -C %{deps}/libdatachannel/deps/json --strip-components=1
tar -xzf %{SOURCE9} -C %{deps}/libdatachannel/deps/libsrtp --strip-components=1
%endif

%build
# uSockets (static) and uWebSockets (headers), staged for HoustonKVM's build.
# uSockets needs OpenSSL even for plain HTTP: uWebSockets always links it.
%make_build -C %{deps}/usockets WITH_OPENSSL=1
install -Dpm644 %{deps}/usockets/uSockets.a %{stage}/lib/libSockets.a
install -Dpm644 %{deps}/usockets/src/libusockets.h -t %{stage}/include
install -pm644 %{deps}/uwebsockets/src/*.h -t %{stage}/include
%if %{without system_libdatachannel}
cmake -S %{deps}/libdatachannel -B %{deps}/libdatachannel/build \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{stage} -DCMAKE_INSTALL_LIBDIR=lib \
    -DUSE_GNUTLS=OFF -DUSE_NICE=OFF -DNO_WEBSOCKET=ON
cmake --build %{deps}/libdatachannel/build %{?_smp_mflags}
cmake --install %{deps}/libdatachannel/build
%endif
%cmake -DHOUSTONKVM_DEPS_PREFIX=%{stage} \
       -DHOUSTONKVM_PRIVATE_LIBDATACHANNEL=%{?with_system_libdatachannel:OFF}%{!?with_system_libdatachannel:ON}
%cmake_build

%install
%cmake_install
install -Dpm755 %{SOURCE24} %{buildroot}%{_bindir}/houstonkvm-setup-hid
install -Dpm644 %{SOURCE20} %{buildroot}%{_unitdir}/houstonkvm.service
install -Dpm644 %{SOURCE21} %{buildroot}%{_unitdir}/houstonkvm-hid.service
install -Dpm644 %{SOURCE22} %{buildroot}%{_sysconfdir}/houstonkvm/houstonkvm.conf
install -dm750 %{buildroot}%{_sysconfdir}/houstonkvm/tls
install -dm750 %{buildroot}%{_sharedstatedir}/houstonkvm
install -Dpm644 %{SOURCE23} %{buildroot}%{_udevrulesdir}/99-houstonkvm-hid.rules
install -Dpm644 %{SOURCE25} %{buildroot}%{_prefix}/lib/firewalld/services/houstonkvm.xml
install -Dpm644 %{SOURCE26} %{buildroot}%{_sysusersdir}/houstonkvm.conf
# houstonkvm.8, and HoustonKVM.8 so "man HoustonKVM" finds it too.
install -Dpm644 docs/houstonkvm.8 docs/HoustonKVM.8 -t %{buildroot}%{_mandir}/man8
%if %{without system_libdatachannel}
# Private, found through the binary's RPATH; the .so symlink is for building.
install -d %{buildroot}%{_libdir}/houstonkvm
cp -a %{stage}/lib/libdatachannel.so.* %{buildroot}%{_libdir}/houstonkvm/
%endif

%check
HOUSTONKVM_BINARY=%{_vpath_builddir}/HoustonKVM \
    python3 -m unittest discover -s tests/integration -p 'test_*.py'

%files
%license LICENSE NOTICE THIRD_PARTY_NOTICES.md
%license ui/fonts/LICENSE-playfair-display.txt ui/fonts/LICENSE-source-sans-3.txt
%doc README.md CHANGELOG.md docs/API.md docs/examples
%{_bindir}/HoustonKVM
%{_bindir}/houstonkvm-setup-hid
%{_unitdir}/houstonkvm.service
%{_unitdir}/houstonkvm-hid.service
%{_udevrulesdir}/99-houstonkvm-hid.rules
%{_prefix}/lib/firewalld/services/houstonkvm.xml
%{_sysusersdir}/houstonkvm.conf
%{_mandir}/man8/houstonkvm.8*
%{_mandir}/man8/HoustonKVM.8*
%dir %{_sysconfdir}/houstonkvm
%config(noreplace) %{_sysconfdir}/houstonkvm/houstonkvm.conf
%dir %attr(0750,root,houstonkvm) %{_sysconfdir}/houstonkvm/tls
%dir %attr(0750,houstonkvm,houstonkvm) %{_sharedstatedir}/houstonkvm
%if %{without system_libdatachannel}
%{_libdir}/houstonkvm/
%endif

%pre
%if 0%{?fedora} < 42
%sysusers_create_compat %{SOURCE26}
%endif

%post
%systemd_post houstonkvm.service houstonkvm-hid.service
# firewalld reads service files only at load: without this, a port added
# on upgrade stays closed.
%firewalld_reload

%preun
%systemd_preun houstonkvm.service houstonkvm-hid.service

%postun
%systemd_postun_with_restart houstonkvm.service

%changelog
* Wed Oct 07 2026 Laszlo Coleman <laszlo.coleman@researchcookie.com> - 0.1.0-1
- First public release.
