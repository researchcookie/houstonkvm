#!/usr/bin/env bash
# Configures/builds the dev-tooling build into build-dev/, kept separate
# from build/ so switching between a release build and dev checks never
# forces a reconfigure of the other. Re-run any time after pulling changes;
# CMake only reconfigures when needed.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -B build-dev -DHOUSTONKVM_DEV_BUILD=ON -DCMAKE_BUILD_TYPE=Debug "$@"
cmake --build build-dev -j"$(nproc)"

cat <<'EOF'

Dev build ready in build-dev/. Available targets:
  cmake --build build-dev --target format        # clang-format, rewrites in place
  cmake --build build-dev --target format-check   # clang-format, check only (CI-friendly)
  cmake --build build-dev --target tidy           # clang-tidy static analysis
  cmake --build build-dev --target valgrind       # integration suite under valgrind memcheck
  ctest --test-dir build-dev                      # plain integration suite (no valgrind)
EOF
