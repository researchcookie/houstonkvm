#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# fetch-rpm-sources.sh — populate ~/rpmbuild/SOURCES for houstonkvm.spec
#
# This is the ONLY step in the RPM packaging workflow that touches the
# network. Every dependency below is pinned to a specific commit/tag (see
# houstonkvm.spec's Source0..9 comments for why each pin was chosen) —
# `rpmbuild -ba houstonkvm.spec` itself runs fully offline afterward.
#
# Usage:
#   bash scripts/fetch-rpm-sources.sh [SOURCEDIR] [VERSION]
#   (defaults: SOURCEDIR=~/rpmbuild/SOURCES, VERSION=0.1.3 — must match
#   houstonkvm.spec's Version: field)
# -----------------------------------------------------------------------------
set -euo pipefail

SOURCEDIR="${1:-$HOME/rpmbuild/SOURCES}"
VERSION="${2:-0.1.3}"

GREEN='\033[0;32m'; NC='\033[0m'
step() { echo -e "\n${GREEN}==> $*${NC}"; }

mkdir -p "$SOURCEDIR"

# name|url — one curl -fsSL -o per line. Commit-pinned archive URLs are of
# the form github.com/<org>/<repo>/archive/<commit>.tar.gz; tag-pinned ones
# use /archive/refs/tags/<tag>.tar.gz.
SOURCES=(
  "usockets-86097c4.tar.gz|https://github.com/uNetworking/uSockets/archive/86097c490263ab662d62e8e7b541390bdec7d149.tar.gz"
  "uwebsockets-20.79.0.tar.gz|https://github.com/uNetworking/uWebSockets/archive/refs/tags/v20.79.0.tar.gz"
  "libdatachannel-0.24.5.tar.gz|https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.5.tar.gz"
  "plog-94899e0.tar.gz|https://github.com/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz"
  "usrsctp-fec583d.tar.gz|https://github.com/paullouisageneau/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz"
  "libjuice-3c40a35.tar.gz|https://github.com/paullouisageneau/libjuice/archive/3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6.tar.gz"
  "libdatachannel-json-55f9368.tar.gz|https://github.com/nlohmann/json/archive/55f93686c01528224f448c19128836e7df245f72.tar.gz"
  "libsrtp-24b3bf8.tar.gz|https://github.com/cisco/libsrtp/archive/24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5.tar.gz"
)

step "Downloading ${#SOURCES[@]} pinned dependency sources to $SOURCEDIR"
for entry in "${SOURCES[@]}"; do
    name="${entry%%|*}"
    url="${entry#*|}"
    out="$SOURCEDIR/$name"
    if [[ -f "$out" ]]; then
        echo "  $name already present, skipping (delete it to re-fetch)"
    else
        echo "  fetching $name"
        curl -fsSL -o "$out" "$url"
    fi
    sha256sum "$out"
done

step "Building Source0 (houstonkvm-${VERSION}.tar.gz) from this project tree"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARENT_DIR="$(dirname "$PROJECT_DIR")"
PROJECT_BASENAME="$(basename "$PROJECT_DIR")"
OUT="$SOURCEDIR/houstonkvm-${VERSION}.tar.gz"

tar -C "$PARENT_DIR" -czf "$OUT" \
    --exclude="$PROJECT_BASENAME/build*" \
    --exclude="$PROJECT_BASENAME/.[!.]*" \
    --exclude="$PROJECT_BASENAME/houstonkvm.db" \
    --exclude="$PROJECT_BASENAME/houstonkvm.db-shm" \
    --exclude="$PROJECT_BASENAME/houstonkvm.db-wal" \
    --transform "s,^${PROJECT_BASENAME},houstonkvm-${VERSION}," \
    "$PROJECT_BASENAME"
sha256sum "$OUT"

step "Staging small in-repo files referenced by houstonkvm.spec's Source20..26"
# Not a network operation — these already live under scripts/ — but
# rpmbuild resolves bare-filename Source lines against %_sourcedir just
# like everything else above, so they need to land here too.
cp -a "$PROJECT_DIR/scripts/houstonkvm.service"     "$SOURCEDIR/houstonkvm.service"
cp -a "$PROJECT_DIR/scripts/houstonkvm-hid.service" "$SOURCEDIR/houstonkvm-hid.service"
cp -a "$PROJECT_DIR/scripts/houstonkvm.conf"        "$SOURCEDIR/houstonkvm.conf"
cp -a "$PROJECT_DIR/scripts/99-houstonkvm-hid.rules" "$SOURCEDIR/99-houstonkvm-hid.rules"
cp -a "$PROJECT_DIR/scripts/setup-hid-gadget.sh"  "$SOURCEDIR/setup-hid-gadget.sh"
cp -a "$PROJECT_DIR/scripts/houstonkvm.firewalld.xml" "$SOURCEDIR/houstonkvm.firewalld.xml"
cp -a "$PROJECT_DIR/scripts/houstonkvm.sysusers"    "$SOURCEDIR/houstonkvm.sysusers"

step "All sources present in $SOURCEDIR — rpmbuild -ba houstonkvm.spec needs no network access"
