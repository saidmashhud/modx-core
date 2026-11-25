#!/bin/sh
# Installs modx-core into a temporary prefix and builds examples/install/hello.c
# against it, the ways a consumer would:
#   1. make install; pkg-config (shared library), run
#   2. pkg-config --static with libmodx.a, run
#   3. CMake find_package(modx-core) against the make-installed prefix, run
#   4. cmake --install of CMakeLists.txt into a second prefix; find_package from there, run
#   5. CMake add_subdirectory of the source tree, run
#   6. DESTDIR staging, then make uninstall leaves nothing behind
# CMake steps are skipped (with a note) when cmake is not installed; pkg-config steps likewise.
#   sh tests/install_test.sh            (from the repository root; make test-install)
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
base=${TMPDIR:-/tmp}
tmp=$(mktemp -d "${base%/}/modx-install-XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
CC=${CC:-cc}
MAKE=${MAKE:-make}
prefix="$tmp/prefix"
ok() { printf '  ok   %s\n' "$1"; }
skip() { printf '  skip %s\n' "$1"; }
run_hello() { # binary, label
    out=$("$1" "$tmp/data-$2") || { echo "FAIL: $2: hello exited $?"; exit 1; }
    case "$out" in "ok "*) ok "$2 ($out)" ;; *) echo "FAIL: $2: $out"; exit 1 ;; esac
}

echo "install test in $tmp"
$MAKE -s -C "$root" BUILD="$tmp/build" PREFIX="$prefix" install >/dev/null
for f in include/modx/http.h include/modx/store.h include/modx/pool.h lib/libmodx.a lib/pkgconfig/modx-core.pc; do
    [ -f "$prefix/$f" ] || { echo "FAIL: $f not installed"; exit 1; }
done
shlib=
for f in "$prefix"/lib/libmodx.*.dylib "$prefix"/lib/libmodx.so.*; do [ -f "$f" ] && shlib=$f; done
[ -n "$shlib" ] || { echo "FAIL: no shared library"; exit 1; }
ok "make install (headers, libmodx.a, shared library, modx-core.pc)"

if command -v pkg-config >/dev/null 2>&1; then
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
    [ "$(pkg-config --modversion modx-core)" = "$(awk '/define MODX_VERSION /{gsub(/"/,"",$3); print $3}' "$root/include/modx/version.h")" ] \
        || { echo "FAIL: pkg-config version"; exit 1; }
    # shared (the default -lmodx picks the shared library when both exist)
    # shellcheck disable=SC2046
    $CC -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags modx-core) -o "$tmp/hello-pc" "$root/examples/install/hello.c" \
        $(pkg-config --libs modx-core) -Wl,-rpath,"$prefix/lib"
    run_hello "$tmp/hello-pc" "pkg-config, shared"
    # static: the archive plus Libs.private
    # shellcheck disable=SC2046
    $CC -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags modx-core) -o "$tmp/hello-static" "$root/examples/install/hello.c" \
        "$prefix/lib/libmodx.a" $(pkg-config --libs-only-other modx-core) $(pkg-config --static --libs-only-l modx-core | sed 's/-lmodx//')
    run_hello "$tmp/hello-static" "pkg-config --static, libmodx.a"
else
    skip "pkg-config not installed"
fi

if command -v cmake >/dev/null 2>&1; then
    cmake -S "$root/examples/install" -B "$tmp/cm-find" -DCMAKE_PREFIX_PATH="$prefix" >/dev/null
    cmake --build "$tmp/cm-find" >/dev/null
    run_hello "$tmp/cm-find/hello" "CMake find_package (make-installed prefix)"

    cmake -S "$root" -B "$tmp/cm-lib" -DCMAKE_INSTALL_PREFIX="$tmp/prefix2" -DMODX_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$tmp/cm-lib" >/dev/null
    cmake --install "$tmp/cm-lib" >/dev/null
    [ -f "$tmp/prefix2/lib/cmake/modx-core/modx-core-config.cmake" ] || [ -f "$tmp/prefix2/lib64/cmake/modx-core/modx-core-config.cmake" ] \
        || { echo "FAIL: CMake package config not installed"; exit 1; }
    cmake -S "$root/examples/install" -B "$tmp/cm-find2" -DCMAKE_PREFIX_PATH="$tmp/prefix2" >/dev/null
    cmake --build "$tmp/cm-find2" >/dev/null
    run_hello "$tmp/cm-find2/hello" "CMake find_package (cmake --install)"

    cmake -S "$root/examples/install" -B "$tmp/cm-sub" -DMODX_CORE_SOURCE_DIR="$root" >/dev/null
    cmake --build "$tmp/cm-sub" >/dev/null
    run_hello "$tmp/cm-sub/hello" "CMake add_subdirectory"
else
    skip "cmake not installed"
fi

# DESTDIR staging records the final prefix, not the staging directory
$MAKE -s -C "$root" BUILD="$tmp/build-stage" PREFIX=/opt/modx DESTDIR="$tmp/stage" install >/dev/null
[ -f "$tmp/stage/opt/modx/include/modx/http.h" ] && grep -q '^prefix=/opt/modx$' "$tmp/stage/opt/modx/lib/pkgconfig/modx-core.pc" \
    || { echo "FAIL: DESTDIR staging"; exit 1; }
ok "DESTDIR staging"

$MAKE -s -C "$root" BUILD="$tmp/build" PREFIX="$prefix" uninstall >/dev/null
left=$(find "$prefix" -type f -o -type l)
[ -z "$left" ] || { echo "FAIL: uninstall left:"; echo "$left"; exit 1; }
ok "make uninstall"
echo "install test passed"
