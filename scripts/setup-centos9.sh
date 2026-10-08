#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# setup-centos9.sh
# Install all HoustonKVM build dependencies on CentOS Stream 9.
#
# Usage:
#   chmod +x scripts/setup-centos9.sh
#   ./scripts/setup-centos9.sh
#
# What this does:
#   1. Enables EPEL + CRB repos
#   2. Installs system packages (gcc, cmake, sqlite, openssl, libsodium, …)
#   3. Builds µSockets  → /usr/local/lib/libSockets.a
#   4. Copies uWebSockets headers → /usr/local/include/
#   5. Builds & installs libdatachannel → /usr/local/lib/libdatachannel.so
# -----------------------------------------------------------------------------
set -euo pipefail

PREFIX=/usr/local
BUILDROOT=/tmp/houstonkvm-deps

# ── Colour output ─────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
step() { echo -e "\n${GREEN}==> $*${NC}"; }
warn() { echo -e "${YELLOW}WARN: $*${NC}"; }

# ── 1. Repositories ───────────────────────────────────────────────────────────
step "Enabling EPEL, CRB, and Cisco openh264 repositories"
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb
# Cisco's royalty-covered openh264 build — used for H.264 encode of the KVM
# feed for WebRTC delivery (see include/video/video_encoder.h). Avoids needing
# RPM Fusion / ffmpeg for H.264 support.
sudo dnf config-manager --set-enabled epel-cisco-openh264
sudo dnf makecache --quiet

# ── 2. System packages ────────────────────────────────────────────────────────
step "Installing system packages"
sudo dnf install -y \
    gcc gcc-c++ \
    cmake ninja-build \
    make git curl pkg-config \
    sqlite-devel \
    openssl-devel \
    libsodium-devel \
    json-devel \
    turbojpeg-devel \
    openh264-devel \
    alsa-lib-devel \
    opus-devel \
    zlib-devel \
    kernel-headers \
    perl-IPC-Cmd     # needed by some cmake scripts in libdatachannel submodules

# Verify the compiler supports C++20
CXX_VER=$(g++ -dumpversion | cut -d. -f1)
if [[ "${CXX_VER}" -lt 11 ]]; then
    warn "g++ ${CXX_VER} detected. C++20 requires GCC ≥ 11."
    warn "Install gcc-toolset-12 or newer and prepend its bin to PATH."
    warn "  sudo dnf install -y gcc-toolset-12"
    warn "  source /opt/rh/gcc-toolset-12/enable"
fi

# ── 3. µSockets ───────────────────────────────────────────────────────────────
step "Building µSockets (uNetworking/uSockets)"
mkdir -p "${BUILDROOT}"
cd "${BUILDROOT}"

if [[ ! -d uSockets ]]; then
    git clone --depth 1 https://github.com/uNetworking/uSockets.git
fi
cd uSockets
# WITH_OPENSSL=1 enables TLS support; required even for plain HTTP builds
# because uWebSockets links against OpenSSL unconditionally.
make WITH_OPENSSL=1 -j"$(nproc)"
sudo cp uSockets.a "${PREFIX}/lib/libSockets.a"
sudo cp src/libusockets.h "${PREFIX}/include/"
echo "  → ${PREFIX}/lib/libSockets.a"
cd "${BUILDROOT}"

# ── 4. uWebSockets (header-only) ─────────────────────────────────────────────
step "Installing uWebSockets headers (uNetworking/uWebSockets)"
if [[ ! -f "${PREFIX}/include/App.h" ]]; then
    if [[ ! -d uWebSockets ]]; then
        git clone --depth 1 https://github.com/uNetworking/uWebSockets.git
    fi
    sudo cp uWebSockets/src/*.h "${PREFIX}/include/"
    echo "  → ${PREFIX}/include/App.h (and friends)"
else
    echo "  already installed, skipping."
fi
cd "${BUILDROOT}"

# ── 5. libdatachannel ─────────────────────────────────────────────────────────
step "Building libdatachannel"
if [[ ! -f "${PREFIX}/lib/libdatachannel.so" ]] && \
   [[ ! -f "${PREFIX}/lib64/libdatachannel.so" ]]; then
    if [[ ! -d libdatachannel ]]; then
        git clone --depth 1 --recurse-submodules \
            https://github.com/paullouisageneau/libdatachannel.git
    fi
    mkdir -p libdatachannel/build
    cd libdatachannel/build
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        -DUSE_GNUTLS=OFF \
        -DUSE_NICE=OFF \
        -DNO_WEBSOCKET=ON \
        -DNO_MEDIA=OFF
    make -j"$(nproc)"
    sudo make install
    sudo ldconfig
    echo "  → ${PREFIX}/lib/libdatachannel.so"
    cd "${BUILDROOT}"
else
    echo "  already installed, skipping."
fi

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}All dependencies installed.${NC}"
echo ""
echo "Build HoustonKVM:"
echo "  cd $(dirname "$(realpath "$0")")/.."
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build -j\$(nproc)"
echo ""
echo "Run:"
echo "  ./build/HoustonKVM --port=8080"
