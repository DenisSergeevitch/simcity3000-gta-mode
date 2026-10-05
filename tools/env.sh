# Sourced by the developer tools (zsh). Finds the game and its Wine prefix:
#   SC3U_APPS   the game's Apps folder, the one with SC3U.exe. Default: the author's layout, with this repo at
#               <SimCity3000>/mods/gta-mode next to the Wine prefix <SimCity3000>/prefix.
#   WINEPREFIX  derived from SC3U_APPS when that lies inside a prefix's drive_c, unless set.
# Sets APPS (absolute path) and WIN_GAME (the game folder as a Windows path, e.g. C:\Games\SimCity 3000 Unlimited).
TOOLS="${TOOLS:-$(cd "$(dirname "$0")" && pwd)}"
APPS="${SC3U_APPS:-$TOOLS/../../../prefix/drive_c/Games/SimCity 3000 Unlimited/Apps}"
if [[ ! -f "$APPS/SC3U.exe" ]]; then
  echo "SimCity 3000 Unlimited not found at: $APPS" >&2
  echo "set SC3U_APPS to the game's Apps folder (the one with SC3U.exe)" >&2
  exit 1
fi
APPS="$(cd "$APPS" && pwd)"
if [[ "$APPS" == */drive_c/* ]]; then
  export WINEPREFIX="${WINEPREFIX:-${APPS%%/drive_c/*}}"
  WIN_GAME="C:\\${${APPS#*/drive_c/}%/Apps}"
  WIN_GAME="${WIN_GAME//\//\\}"
else
  WIN_GAME="${SC3U_WIN_GAME:-C:\\Games\\SimCity 3000 Unlimited}"
fi
