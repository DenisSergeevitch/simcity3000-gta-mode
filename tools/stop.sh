#!/bin/zsh
# Stop the game by exact PID (never pkill -f patterns that could match the caller).
for p in $(pgrep -f 'Apps\\SC3U\.exe'); do kill $p; done
for i in {1..20}; do pgrep -f 'Apps\\SC3U\.exe' >/dev/null || exit 0; sleep 0.5; done
for p in $(pgrep -f 'Apps\\SC3U\.exe'); do kill -9 $p; done
