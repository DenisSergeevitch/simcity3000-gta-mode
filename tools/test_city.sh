#!/bin/zsh
# Scripted smoke test of GTA mode in one city: possess, walk, steal, drive, exit, punch. Frames -> $OUT/<tag>_*.png
TOOLS="$(cd "$(dirname "$0")" && pwd)"; source "$TOOLS/env.sh"
cd "$TOOLS/.."
CITY="$1"; TAG="${2:-test}"; OUT="${OUT:-/tmp}"
T=tools/gtacmd.sh
grab() { $T "grab $1.bmp" >/dev/null; sleep 0.6; python3 -c "from PIL import Image; Image.open('$APPS/$1.bmp').save('$OUT/$1.png')"; }
tools/start_city.sh "$CITY" >/dev/null
$T "click 523 563" >/dev/null
$T "gta on" | head -1
sleep 3
$T "gta possess" "netscan" | grep -E 'mode=|tiles in'
$T "hold D 1200" >/dev/null; sleep 1.5
grab ${TAG}_walk
$T "gta tpcar" | tail -1; sleep 0.3
$T "key 0x0d" >/dev/null; sleep 0.3
$T "hold W 1800" >/dev/null; sleep 0.9; $T "hold D 700" >/dev/null; sleep 1.2
grab ${TAG}_drive
$T "gta" | grep -E 'car ang|player'
$T "key 0x0d" >/dev/null; sleep 0.5
$T "key 0x20" >/dev/null; sleep 0.5
$T "gta" | grep -E 'mode=|player|peds='
pgrep -f 'Apps\\SC3U.exe' >/dev/null && echo "game alive" || echo "GAME DIED"
