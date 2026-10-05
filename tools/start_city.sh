#!/bin/zsh
# Restart the game straight into a city and dismiss the startup tip.  tools/start_city.sh ["Madison, WI"]
TOOLS="$(cd "$(dirname "$0")" && pwd)"; source "$TOOLS/env.sh"
cd "$TOOLS/.."
CITY="${1:-Madison, WI}"
tools/stop.sh
tools/run.sh "$WIN_GAME\\Cities\\$CITY.sc3" >/dev/null
for i in {1..40}; do tools/gtacmd.sh "ping" >/dev/null 2>&1 && break; sleep 1; done
sleep 8
tools/gtacmd.sh "click 522 403" >/dev/null
# --play: unpause and zoom to the closest level
if [[ "$2" == "--play" ]]; then
  tools/gtacmd.sh "click 523 563" >/dev/null; sleep 1
  for i in 1 2 3 4; do tools/gtacmd.sh "click 667 545" >/dev/null; sleep 1.2; done
fi
