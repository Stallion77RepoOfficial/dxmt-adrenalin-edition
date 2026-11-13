#!/bin/bash
# Standalone source build for native Apple Silicon Wine.
set -euo pipefail
source_root=$(cd "$(dirname "$0")" && pwd)
cache=${GOOSE_BUILD_CACHE:-"$HOME/Library/Caches/GooseHUB"}
wine_build=${WINE_BUILD:-"$cache/Build/Wine/build"}
llvm_root=${LLVM_ROOT:-/opt/homebrew/opt/llvm}
mingw_root=${LLVM_MINGW_ROOT:-"$cache/Toolchains/llvm-mingw-20261006-ucrt-macos-universal"}
build_root=${DXMT_BUILD_ROOT:-"$cache/Build/DXMT"}
[[ -f $wine_build/include/config.h ]] || { echo "Build the matching Wine sources first: $wine_build" >&2; exit 1; }
export PATH="$mingw_root/bin:$llvm_root/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
for architecture in ${DXMT_ARCHITECTURES:-arm64ec x86}; do
    case $architecture in
        arm64ec) cross=build-arm64ec.txt ;;
        x86) cross=build-win32.txt ;;
        *) echo "Unsupported architecture: $architecture" >&2; exit 2 ;;
    esac
    output=$build_root/$architecture
    if [[ ! -f $output/build.ninja ]]; then
        meson setup "$output" "$source_root" --cross-file "$source_root/$cross" \
            --buildtype=release --prefix=/ -Db_lto=false \
            -Dnative_llvm_path="$llvm_root" -Dwine_build_path="$wine_build"
    fi
    meson compile -C "$output" -j "${GOOSE_BUILD_JOBS:-8}"
    meson install -C "$output" --no-rebuild --destdir "$build_root/$architecture-install"
done
echo "Built DXMT in $build_root"
