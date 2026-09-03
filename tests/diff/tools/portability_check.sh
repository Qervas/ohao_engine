#!/bin/bash
# Build the differentiable renderer with GCC/libstdc++ and ASSERTS LIVE.
#
# WHY THIS EXISTS, in two defects it found on its first run.
#
#   1. MSVC's standard headers transitively include far more than libstdc++
#      does, so a translation unit can use std::size_t, std::max or
#      std::numeric_limits with no <cstddef>, <algorithm> or <limits> in
#      sight and compile perfectly on Windows for months. Fourteen such
#      files were found here. <cstdint> in particular has been strict in
#      libstdc++ since GCC 13.
#
#   2. The CMake test targets build Release, which defines NDEBUG, which
#      compiles asserts away. ArenaLayout::block() asserted that its index
#      was in range AND returned an invalid block when it was not -- and a
#      test deliberately exercised the second path. Under NDEBUG the assert
#      vanished and the test passed. With asserts live it aborted the whole
#      binary. A test can only pass in a configuration nobody ships.
#
# Neither is caught by the normal Windows build, and both would have been
# somebody's first hour on a Linux machine.
#
#   usage:  tests/diff/tools/portability_check.sh [--syntax-only]
#
# Reads CXX (default g++). Needs the CMake build tree's _deps for the
# Vulkan, VMA, glm and GoogleTest headers, so configure once before running.
set -u

cd "$(dirname "$0")/../../.." || exit 1
ROOT=$(pwd)
CXX=${CXX:-g++}
DEPS=build/_deps
OUT=build/portability
mkdir -p "$OUT"

if [ ! -d "$DEPS/vulkan_headers-src" ]; then
    echo "error: $DEPS is missing. Configure the build tree first:"
    echo "  cmake -B build -S ."
    exit 1
fi

GT=$DEPS/googletest-src/googletest
INC="-Iohao -Itests/diff -I$DEPS/vulkan_headers-src/include
     -I$DEPS/vma-src/include -I$DEPS/glm-src -I$GT/include -I$GT -I."
# NO -DNDEBUG. That is the point: this build is the one where asserts run.
FLAGS="-std=c++20 -O1 -DGLM_ENABLE_EXPERIMENTAL -Wall -Wextra
       -Wno-unused-parameter -Wno-missing-field-initializers"

echo "=== $($CXX --version | head -1)"
echo "=== syntax over every translation unit"
fail=0
count=0
for f in $(find ohao/diff tests/diff -name '*.cpp' | sort); do
    count=$((count + 1))
    if ! $CXX $FLAGS $INC -fsyntax-only "$f" 2>"$OUT/err.txt"; then
        echo "--- FAILED: $f"
        grep -E "error:" "$OUT/err.txt" | head -5
        fail=$((fail + 1))
    fi
done
echo "=== $((count - fail)) of $count translation units clean"
[ "$fail" -gt 0 ] && exit 1
[ "${1:-}" = "--syntax-only" ] && exit 0

echo "=== compiling and linking diff_unit_tests (asserts live)"
objs=""
for f in "$GT/src/gtest-all.cc" "$GT/src/gtest_main.cc" \
         $(find ohao/diff -name '*.cpp' | sort) \
         ohao/gpu/vulkan/gpu_allocator.cpp \
         tests/diff/diff_unit_tests.cpp; do
    o="$OUT/$(echo "$f" | tr '/' '_').o"
    $CXX $FLAGS $INC -c "$f" -o "$o" || exit 1
    objs="$objs $o"
done

# The Vulkan loader: the unit tests create a bare instance to query device
# features. MinGW links the system DLL directly; elsewhere, -lvulkan.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) VKLIB=/c/Windows/System32/vulkan-1.dll ;;
    *)                    VKLIB=-lvulkan ;;
esac
$CXX $objs "$VKLIB" -o "$OUT/diff_unit_tests_gcc" || exit 1

echo "=== running"
"$OUT/diff_unit_tests_gcc"
