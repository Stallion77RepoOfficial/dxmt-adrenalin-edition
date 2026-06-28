#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

LLVM_PATH="/usr/local/opt/llvm"
WINE_BUILD="/Users/berkegulacar/Downloads/wine-adrenalin-edition/build/wine-x64"
WINE_INSTALL="/Users/berkegulacar/Downloads/wine-adrenalin-edition/wine-install"
MESON="arch -x86_64 /usr/local/bin/meson"

echo ">>> Eski build temizleniyor..."
rm -rf build .build_x64 .build_x86

echo ">>> x86_64 build kuruluyor..."
$MESON setup .build_x64 \
  --cross-file build-win64.txt \
  --prefix=/ \
  -Dnative_llvm_path="$LLVM_PATH" \
  -Db_lto=false \
  -Denable_nvapi=true \
  -Denable_nvngx=true \
  -Dwine_build_path="$WINE_BUILD" \
  -Dwine_install_path="$WINE_INSTALL"

echo ">>> i386 build kuruluyor..."
$MESON setup .build_x86 \
  --cross-file build-win32.txt \
  --prefix=/ \
  -Dnative_llvm_path="$LLVM_PATH" \
  -Db_lto=false \
  -Dwine_build_path="$WINE_BUILD" \
  -Dwine_install_path="$WINE_INSTALL"

echo ">>> Derleniyor (paralel)..."
$MESON compile -C .build_x64 &
$MESON compile -C .build_x86 &
wait

echo ">>> build/ klasorune kopyalaniyor..."
mkdir -p build
DESTDIR="$SCRIPT_DIR/build" $MESON install -C .build_x64 --no-rebuild --quiet
DESTDIR="$SCRIPT_DIR/build" $MESON install -C .build_x86 --no-rebuild --quiet

echo ">>> Derleme artiklari temizleniyor..."
find build -type f ! \( -name "*.dll" -o -name "*.so" \) -delete
find build -type d -empty -delete

echo ""
echo "=== Build tamamlandi ==="
find build -type f | sort
