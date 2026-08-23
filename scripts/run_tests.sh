#!/usr/bin/env bash
# Builds and runs the host-side unit tests.
#
# These cover the parts of the mod that are plain C++ and do not need Minecraft
# or the Android toolchain, so they can run in CI and locally with any C++20
# compiler.
#
# A few tests additionally include the preloader and nlohmann_json headers
# that xmake normally provides. Their include dirs are auto-detected from the
# xmake package cache and can be overridden with PRE_LOADER_INCLUDE /
# JSON_INCLUDE. When they are missing the affected tests are reported as
# skipped (not failed); each test's header comment documents how to build it
# manually. Tests that touch Android-only JNI code compile against the host
# fake in tests/fakejni instead of the NDK's <jni.h>.
#
#     ./scripts/run_tests.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${root}/build/tests"
cxx="${CXX:-g++}"
flags=(-std=c++20 -Wall -Wextra -O1 -I "${root}/src" -I "${root}/include")

preloader_inc="${PRE_LOADER_INCLUDE:-$(echo "${HOME}"/.xmake/packages/p/preloader/main/*/include 2>/dev/null | head -n1)}"
json_inc="${JSON_INCLUDE:-$(echo "${HOME}"/.xmake/packages/n/nlohmann_json/v3.11.3/*/include 2>/dev/null | head -n1)}"
entt_inc="${ENTT_INCLUDE:-$(echo "${HOME}"/.xmake/packages/e/entt/*/include 2>/dev/null | head -n1)}"

mkdir -p "${outdir}"

status=0
for source in "${root}"/tests/*_test.cpp; do
    name="$(basename "${source}" .cpp)"
    printf '\n=== %s ===\n' "${name}"

    # Per-test requirements (see the header comment of each test).
    extra=()
    skip=""
    case "${name}" in
        commandhotkey_test|crosshair_test)
            if [ -n "${preloader_inc}" ] && [ -d "${preloader_inc}" ] &&
               [ -n "${json_inc}" ] && [ -d "${json_inc}" ]; then
                extra+=(-I "${preloader_inc}" -I "${json_inc}" -I "${root}/tests/fakejni")
            else
                skip="preloader/nlohmann_json headers (set PRE_LOADER_INCLUDE and JSON_INCLUDE)"
            fi
            ;;
        effectdisplay_test)
            # Also needs entt from the xmake packages; fmt and <android/log.h>
            # are covered by the host fakes in tests/fakejni.
            if [ -n "${preloader_inc}" ] && [ -d "${preloader_inc}" ] &&
               [ -n "${json_inc}" ] && [ -d "${json_inc}" ] &&
               [ -n "${entt_inc}" ] && [ -d "${entt_inc}" ]; then
                extra+=(-I "${preloader_inc}" -I "${json_inc}" -I "${entt_inc}"
                        -I "${root}/tests/fakejni")
            else
                skip="preloader/nlohmann_json/entt headers (set PRE_LOADER_INCLUDE, JSON_INCLUDE and ENTT_INCLUDE)"
            fi
            ;;
        externalbuttonrefresh_test)
            extra+=(-I "${root}/tests/fakejni")
            ;;
    esac

    if [ -n "${skip}" ]; then
        echo "  SKIP: needs ${skip}"
        continue
    fi

    if ! "${cxx}" "${flags[@]}" ${extra[@]+"${extra[@]}"} "${source}" -o "${outdir}/${name}"; then
        echo "  FAIL to compile (see above)"
        status=1
        continue
    fi
    if ! "${outdir}/${name}"; then
        status=1
    fi
done

printf '\n'
if [ "${status}" -eq 0 ]; then
    echo "all test binaries passed"
else
    echo "some tests failed"
fi
exit "${status}"
