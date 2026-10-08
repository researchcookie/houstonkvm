#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# run-ui-tests.sh — browser tests for the web UI (tests/ui/).
#
# These drive a real HoustonKVM in headless Chromium via Playwright, which the
# stdlib-only integration suite (`python3 -m unittest discover -s
# tests/integration`) deliberately avoids.
# They SKIP, not fail, if Playwright or Chromium is missing:
#
#   pip install --user playwright && playwright install chromium
#
# On a minimal RHEL/CentOS box Chromium may also need system libraries
# (libatk, libgbm, libxkbcommon, ...): `playwright install-deps` as root, or
# point LD_LIBRARY_PATH at a directory holding them.
#
# Usage:
#   scripts/run-ui-tests.sh                 # build if needed, then test
#   HOUSTONKVM_BINARY=/path/to/HoustonKVM scripts/run-ui-tests.sh
# -----------------------------------------------------------------------------
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

if [[ -z "${HOUSTONKVM_BINARY:-}" && ! -x build/HoustonKVM ]]; then
    echo "==> build/HoustonKVM not found, building first"
    cmake -B build -S .
    cmake --build build -j"$(nproc)"
fi

exec python3 -m unittest discover -s tests/ui -p 'test_*.py' -v
