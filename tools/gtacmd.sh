#!/bin/zsh
# Send commands to the running game through gta_mode.dll's developer command channel and print the reply.
#   tools/gtacmd.sh "ping" "mods"
# The channel is on only when Apps\gta_mode.dev existed at game start (tools/run.sh creates it).
TOOLS="$(cd "$(dirname "$0")" && pwd)"; source "$TOOLS/env.sh"
SEQ=$RANDOM
{ echo "#seq $SEQ"; for c in "$@"; do print -r -- "$c"; done; } > "$APPS/gta_cmd.tmp"
mv "$APPS/gta_cmd.tmp" "$APPS/gta_cmd.txt"
for i in {1..600}; do
  if grep -q "^#done $SEQ\$" "$APPS/gta_out.txt" 2>/dev/null; then grep -v "^#done" "$APPS/gta_out.txt"; exit 0; fi
  sleep 0.1
done
echo "timeout (is the game running with gta_mode.dll and Apps/gta_mode.dev?)" >&2; exit 1
