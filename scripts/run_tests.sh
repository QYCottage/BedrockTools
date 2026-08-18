#!/usr/bin/env bash
# Builds and runs the host-side unit tests.
#
# These cover the parts of the mod that are plain C++ and do not need Minecraft
# or the Android toolchain (currently the Effect Display layout resolver and
# its formatting helpers), so they can run in CI and locally with any C++20
# compiler.
#
#     ./scripts/run_tests.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${root}/build/tests"
cxx="${CXX:-g++}"
flags=(-std=c++20 -Wall -Wextra -O1 -I "${root}/src")

mkdir -p "${outdir}"

status=0
for source in "${root}"/tests/*_test.cpp; do
    name="$(basename "${source}" .cpp)"
    printf '\n=== %s ===\n' "${name}"
    "${cxx}" "${flags[@]}" "${source}" -o "${outdir}/${name}"
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
