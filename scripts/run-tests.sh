#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# run-tests.sh — build (if needed) and run the HTTP integration test suite.
#
# Usage:
#   scripts/run-tests.sh              # build if build/HoustonKVM is missing, then test
#   HOUSTONKVM_BINARY=/path/to/HoustonKVM scripts/run-tests.sh   # test a specific binary
# -----------------------------------------------------------------------------
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

if [[ -z "${HOUSTONKVM_BINARY:-}" && ! -x build/HoustonKVM ]]; then
    echo "==> build/HoustonKVM not found, building first"
    cmake -B build -S .
    cmake --build build -j"$(nproc)"
fi

exec python3 -m unittest discover -s tests/integration -p 'test_*.py' -v
