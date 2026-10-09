cmake_minimum_required(VERSION 3.16)
project(HoustonKVM VERSION 0.1.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HOUSTONKVM_DEV_BUILD
    "Dev mode: -Og debug build + compile_commands.json + format/tidy/valgrind targets"
    OFF)

# Must be set before add_executable() below — CMake captures its value as
# each target's sources are added, not lazily at generate time, so setting
# it any later (even still-conditionally on HOUSTONKVM_DEV_BUILD) silently
# produces no compile_commands.json.
if(HOUSTONKVM_DEV_BUILD)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# ── UI bundle (ui/** → build/generated/ui_bundle.h) ─────────────────────────
#
# Every file under ui/ is embedded generically (see cmake/embed_files.py) —
# adding a new frontend file needs no changes here, just one `cmake`
# reconfigure so this GLOB_RECURSE picks it up (editing an existing file
# rebuilds automatically; only *new* files need the reconfigure).
find_program(PYTHON3 python3 REQUIRED)
set(UI_BUNDLE_HEADER ${CMAKE_BINARY_DIR}/generated/ui_bundle.h)
file(GLOB_RECURSE UI_FILES ${CMAKE_SOURCE_DIR}/ui/*)

add_custom_command(
    OUTPUT  ${UI_BUNDLE_HEADER}
    COMMAND ${PYTHON3}
            ${CMAKE_SOURCE_DIR}/cmake/embed_files.py
            ${CMAKE_SOURCE_DIR}/ui
            ${UI_BUNDLE_HEADER}
    DEPENDS ${UI_FILES}
            ${CMAKE_SOURCE_DIR}/cmake/embed_files.py
    COMMENT "Embedding UI files"
    VERBATIM
)
add_custom_target(ui_bundle DEPENDS ${UI_BUNDLE_HEADER})

set(SERVER_SOURCES
    src/server.cpp
    src/core/audit_log.cpp
    src/core/database.cpp
    src/core/json_log.cpp
    src/core/metrics.cpp
    src/core/routes_audit.cpp
    src/core/routes_targets.cpp
    src/core/target_manager.cpp
    src/auth/auth.cpp
    src/auth/password_hasher.cpp
    src/auth/rate_limiter.cpp
    src/auth/routes_auth.cpp
    src/hid/ch9329_inject.cpp
    src/hid/input_inject.cpp
    src/hid/input_queue.cpp
    src/hid/qemu_inject.cpp
    src/hid/routes_health.cpp
    src/hid/routes_input.cpp
    src/hid/serial_ports.cpp
    src/hid/text_typing.cpp
    src/video/audio_capture.cpp
    src/video/routes_settings.cpp
    src/video/routes_stream.cpp
    src/video/stream_manager.cpp
    src/video/v4l2_capture.cpp
    src/video/video_encoder.cpp
    src/webrtc/ice_servers.cpp
    src/webrtc/publisher.cpp
    src/webrtc/routes_webrtc.cpp
    src/webrtc/selective_forwarding_unit.cpp
    src/tls/certificate.cpp
    src/tls/generate.cpp
    src/tls/routes_tls.cpp
    src/tls/tls_manager.cpp
)

# Load testing: synthetic capture sources in the server (a target device of
# "synthetic:desktop" or "synthetic:motion", see synthetic_capture.h) and the
# HoustonKVM-loadgen viewer client. scripts/loadtest.py builds with this in
# its own build directory; packages never do.
option(HOUSTONKVM_LOADTEST "Build synthetic capture sources and HoustonKVM-loadgen" OFF)
if(HOUSTONKVM_LOADTEST)
    list(APPEND SERVER_SOURCES src/video/synthetic_capture.cpp)
endif()

add_executable(HoustonKVM ${SERVER_SOURCES})
add_dependencies(HoustonKVM ui_bundle)
# Which build this is, beyond the version (a branch and commit, say), shown
# by --version and in the startup log.
set(HOUSTONKVM_BUILD_ID "" CACHE STRING "Extra build identification shown after the version")
if(HOUSTONKVM_BUILD_ID)
    set(HOUSTONKVM_FULL_VERSION "${PROJECT_VERSION} (${HOUSTONKVM_BUILD_ID})")
else()
    set(HOUSTONKVM_FULL_VERSION "${PROJECT_VERSION}")
endif()
target_compile_definitions(HoustonKVM PRIVATE HOUSTONKVM_VERSION="${HOUSTONKVM_FULL_VERSION}")

# Where uSockets/uWebSockets/libdatachannel headers+libs are staged. Defaults
# to /usr/local; override with -DHOUSTONKVM_DEPS_PREFIX=<dir>.
set(HOUSTONKVM_DEPS_PREFIX "/usr/local" CACHE PATH
    "Prefix where uSockets/uWebSockets/libdatachannel headers+libs are staged")

target_include_directories(HoustonKVM PRIVATE
    include
    ${HOUSTONKVM_DEPS_PREFIX}/include
    ${CMAKE_BINARY_DIR}/generated
)

target_link_directories(HoustonKVM PRIVATE
    ${HOUSTONKVM_DEPS_PREFIX}/lib
    ${HOUSTONKVM_DEPS_PREFIX}/lib64
)

target_link_libraries(HoustonKVM PRIVATE
    ${HOUSTONKVM_DEPS_PREFIX}/lib/libSockets.a
    datachannel
    sodium
    sqlite3
    ssl
    crypto
    z
    pthread
    turbojpeg
    openh264
)

# The target's sound: ALSA in, Opus out over WebRTC (see
# include/video/audio_capture.h). Without it a target's audio_device is
# accepted but reports "unsupported".
option(HOUSTONKVM_AUDIO "Capture targets' sound (needs alsa-lib and opus)" ON)
if(HOUSTONKVM_AUDIO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AUDIO_DEPS IMPORTED_TARGET alsa opus)
    if(NOT AUDIO_DEPS_FOUND)
        message(FATAL_ERROR "Sound needs the alsa-lib and opus development files "
                "(EL9: dnf install alsa-lib-devel opus-devel, with CRB enabled for opus-devel). "
                "Or configure with -DHOUSTONKVM_AUDIO=OFF to build without sound.")
    endif()
    target_link_libraries(HoustonKVM PRIVATE PkgConfig::AUDIO_DEPS)
    target_compile_definitions(HoustonKVM PRIVATE HOUSTONKVM_AUDIO)
endif()

if(HOUSTONKVM_DEV_BUILD)
    target_compile_options(HoustonKVM PRIVATE -Wall -Wextra -Og -g -fno-omit-frame-pointer)
else()
    target_compile_options(HoustonKVM PRIVATE -Wall -Wextra -O2)
endif()

if(HOUSTONKVM_LOADTEST)
    target_compile_definitions(HoustonKVM PRIVATE HOUSTONKVM_SYNTHETIC_CAPTURE)

    add_executable(HoustonKVM-loadgen tools/loadgen/loadgen.cpp)
    target_include_directories(HoustonKVM-loadgen PRIVATE ${HOUSTONKVM_DEPS_PREFIX}/include)
    target_link_directories(HoustonKVM-loadgen PRIVATE
        ${HOUSTONKVM_DEPS_PREFIX}/lib
        ${HOUSTONKVM_DEPS_PREFIX}/lib64
    )
    target_link_libraries(HoustonKVM-loadgen PRIVATE datachannel ssl crypto pthread)
    target_compile_options(HoustonKVM-loadgen PRIVATE -Wall -Wextra -O2)
endif()

include(GNUInstallDirs)

option(HOUSTONKVM_PRIVATE_LIBDATACHANNEL
    "Installed binary finds a bundled libdatachannel.so in <libdir>/houstonkvm (off when the system provides it)"
    ON)

# INSTALL_RPATH only (not BUILD_WITH_INSTALL_RPATH): this is applied when
# `cmake --install`/`make install` runs (used by houstonkvm.spec's
# %cmake_install to find the vendored libdatachannel.so in a private,
# non-system libdir — avoids registering it in the public linker path via
# ldconfig, where an untracked bundled .so could collide with a future
# real system package of the same name). Left at CMake's default for the
# build tree itself, so `./build/HoustonKVM` finds libraries via the normal
# automatic build-rpath.
if(HOUSTONKVM_PRIVATE_LIBDATACHANNEL)
    set_target_properties(HoustonKVM PROPERTIES
        INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}/houstonkvm"
    )
endif()

install(TARGETS HoustonKVM RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── Tests ────────────────────────────────────────────────────────────────
#
# HTTP integration tests (tests/integration/) drive a real HoustonKVM binary
# over the network against a throwaway sqlite db — see
# tests/integration/server_harness.py. Stdlib-only (no pip deps), so no
# extra find_package needed beyond the python3 already required above for
# embed_files.py.
enable_testing()
add_test(
    NAME integration
    COMMAND ${PYTHON3} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests/integration -p test_*.py -v
)
set_tests_properties(integration PROPERTIES
    ENVIRONMENT "HOUSTONKVM_BINARY=$<TARGET_FILE:HoustonKVM>"
)

# ── Dev tooling (format / tidy / valgrind) ──────────────────────────────────
#
# Only wired up under -DHOUSTONKVM_DEV_BUILD=ON (use a separate build
# directory, e.g. -B build-dev, with -DCMAKE_BUILD_TYPE=Debug) —
# keeps the normal release build free of the extra find_program() probing
# and the -Og/-g flags these targets need (valgrind's stack traces and
# clang-tidy's analysis are both far more useful without -O2 reordering
# things).
if(HOUSTONKVM_DEV_BUILD)
    file(GLOB_RECURSE HOUSTONKVM_HEADERS ${CMAKE_SOURCE_DIR}/include/*.h)

    find_program(CLANG_FORMAT_EXE clang-format)
    if(CLANG_FORMAT_EXE)
        add_custom_target(format
            COMMAND ${CLANG_FORMAT_EXE} -i ${SERVER_SOURCES} ${HOUSTONKVM_HEADERS}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "clang-format: rewriting sources in place"
        )
        add_custom_target(format-check
            COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${SERVER_SOURCES} ${HOUSTONKVM_HEADERS}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "clang-format: checking style (no changes written)"
        )
    else()
        message(STATUS "HOUSTONKVM_DEV_BUILD: clang-format not found, skipping format/format-check targets")
    endif()

    find_program(CLANG_TIDY_EXE clang-tidy)
    if(CLANG_TIDY_EXE)
        add_custom_target(tidy
            COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} ${SERVER_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS ${UI_BUNDLE_HEADER}
            COMMENT "clang-tidy: static analysis (see .clang-tidy for enabled checks)"
        )
    else()
        message(STATUS "HOUSTONKVM_DEV_BUILD: clang-tidy not found, skipping tidy target")
    endif()

    find_program(VALGRIND_EXE valgrind)
    if(VALGRIND_EXE)
        # Reuses the integration suite as the valgrind workload rather than
        # a separate smoke test: HOUSTONKVM_VALGRIND=1 makes server_harness.py
        # launch the real binary under `valgrind --leak-check=full` instead
        # of directly, and SIGTERM (used by HoustonKVMServer.stop()) already
        # triggers the app's own graceful-shutdown path, so destructors run
        # and valgrind's exit-time leak check reflects genuine leaks rather
        # than just "still reachable at SIGTERM".
        add_custom_target(valgrind
            COMMAND ${CMAKE_COMMAND} -E env
                    HOUSTONKVM_BINARY=$<TARGET_FILE:HoustonKVM>
                    HOUSTONKVM_VALGRIND=1
                    ${PYTHON3} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests/integration -p test_*.py -v
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS HoustonKVM
            COMMENT "Running integration tests with the server under valgrind memcheck"
        )
    else()
        message(STATUS "HOUSTONKVM_DEV_BUILD: valgrind not found, skipping valgrind target")
    endif()
endif()
