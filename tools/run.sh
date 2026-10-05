#!/bin/zsh
# Launch SC3U under Wine in the background, with the mod's developer command channel enabled.
#   tools/run.sh                                         -> main menu
#   tools/run.sh 'C:\Games\SimCity 3000 Unlimited\Cities\Madison, WI.sc3'
TOOLS="$(cd "$(dirname "$0")" && pwd)"; source "$TOOLS/env.sh"
export WINEDEBUG=${WINEDEBUG:--all} MVK_CONFIG_LOG_LEVEL=0
cd "$APPS"
rm -f gta_out.txt gta_cmd.txt
touch gta_mode.dev
start=$(date +%s)
nohup wine SC3U.exe -intro:off "$@" > /tmp/sc3u_wine.log 2>&1 &
for i in {1..60}; do grep -q "OnStart" gta_mode.log 2>/dev/null && [[ $(stat -f %m gta_mode.log) -ge $start ]] && break; sleep 1; done
pgrep -f 'Apps\\SC3U.exe' | head -1
