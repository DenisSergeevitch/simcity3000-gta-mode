#!/usr/bin/env bash
# Build gta_mode.dll (a 32-bit Windows DLL) with mingw-w64.
#   ./build.sh                build/gta_mode.dll, then copy it into the game if SC3U_APPS is set
#   ./build.sh --no-install   build only
#   ./build.sh --dist         also package dist/gta-mode-<version>.zip for a release
# SC3U_APPS is the game's Apps folder (the one with SC3U.exe). Stop the game before installing: replacing a
# loaded DLL can crash it under Wine.
set -euo pipefail
cd "$(dirname "$0")"
CC="${CC:-i686-w64-mingw32-gcc}"
VERSION="$(sed -n 's/^#define GTA_MODE_VERSION "\(.*\)"/\1/p' src/version.h)"
install=1; dist=0
for a in "$@"; do
  case "$a" in
    --no-install) install=0 ;;
    --dist) dist=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done
mkdir -p build
"$CC" -std=gnu11 -O2 -Wall -Wno-unused-function -Wno-format-truncation -shared \
  -o build/gta_mode.dll src/*.c src/*.S -s -Wl,--kill-at -static-libgcc -lgdi32 -luser32 -lkernel32
echo "built build/gta_mode.dll (GTA mode $VERSION)"

apps="${SC3U_APPS:-}"
legacy="../../prefix/drive_c/Games/SimCity 3000 Unlimited/Apps"   # the author's layout: repo next to the Wine prefix
if [[ -z "$apps" && -f "$legacy/SC3U.exe" ]]; then apps="$legacy"; fi
if (( install )) && [[ -n "$apps" ]]; then
  cp build/gta_mode.dll "$apps/gta_mode.dll"
  echo "installed -> $apps/gta_mode.dll"
fi

if (( dist )); then
  out="dist/gta-mode-$VERSION"
  rm -rf "$out" "$out.zip"; mkdir -p "$out"
  cp build/gta_mode.dll LICENSE "$out/"
  cp docs/INSTALL.txt "$out/README.txt"
  (cd dist && zip -qr "gta-mode-$VERSION.zip" "gta-mode-$VERSION")
  echo "packaged $out.zip"
fi
