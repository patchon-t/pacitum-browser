#!/bin/bash
# Showcase screenshots: runs the built browser under Xvfb, opens the main screens in light and
# dark, and saves JPEGs to ./showcase. Each image is also printed to the log as base64 between
# ==SHOT name== / ==END== markers so it can be read back without downloading artifacts.
# usage: .github/showcase.sh ./build/PacitumBrowser
set -u
APP="$1"; OUT="$PWD/showcase"; W=1280; H=800
mkdir -p "$OUT"
export QTWEBENGINE_DISABLE_SANDBOX=1 DISPLAY=:77
Xvfb :77 -screen 0 ${W}x${H}x24 >/dev/null 2>&1 &
XVFB=$!; sleep 2
DATA="$HOME/.local/share/Pacitum/Pacitum Browser"
SET="$DATA/profiles/default/browser_settings.json"

launch() {   # launch [urls...]
  "$APP" "$@" >/dev/null 2>&1 &
  PID=$!
  for i in $(seq 1 60); do
    WIN=$(xdotool search --onlyvisible --pid $PID 2>/dev/null | tail -n 1)
    [ -n "$WIN" ] && break; sleep 0.5
  done
  [ -z "${WIN:-}" ] && { echo "window never appeared"; return 1; }
  xdotool windowmove "$WIN" 0 0 windowsize "$WIN" $W $H
  sleep 6
  focus
}
quit() { kill $PID 2>/dev/null; sleep 2; kill -9 $PID 2>/dev/null; pkill -9 -f QtWebEngineProcess 2>/dev/null; sleep 1; }
shot() { sleep "${2:-1.5}"; import -window root -crop ${W}x${H}+0+0 -quality 84 "$OUT/$1.jpg"; echo "shot $1 ($(stat -c %s "$OUT/$1.jpg") bytes)"; }
# Qt ignores synthetic (XSendEvent) keys, so focus the window and type through XTEST instead
focus() { xdotool windowfocus --sync "$WIN" 2>/dev/null; xdotool mousemove 640 400; }
key() { focus; xdotool key --clearmodifiers "$@"; }
theme() {   # theme Light|Dark
  mkdir -p "$(dirname "$SET")"; [ -s "$SET" ] || echo '{}' > "$SET"
  # fresh session each time (no restored tabs), settings kept
  find "$(dirname "$SET")" -mindepth 1 -maxdepth 1 ! -name browser_settings.json -exec rm -rf {} + 2>/dev/null
  jq --arg t "$1" '.theme=$t | .first_run_done=true' "$SET" > "$SET.tmp" && mv "$SET.tmp" "$SET"
}

# 1. First run: the welcome flow
launch && shot 01-welcome 3; quit

for T in Light Dark; do
  t=$(echo $T | tr A-Z a-z)
  theme $T
  launch "https://en.wikipedia.org/wiki/Web_browser" || continue
  key ctrl+t; shot 02-newtab-$t 4
  key alt+f; shot 03-menu-$t 1.5; key Escape; sleep 1
  key ctrl+shift+Tab; shot 04-website-$t 2
  key ctrl+comma; shot 05-settings-$t 3
  # Settings nav sits on the left; focus it and jump to its last row (About Pacitum)
  focus; xdotool mousemove 140 330 click 1; sleep 0.5; xdotool key End; shot 06-about-$t 2
  # scroll to the bottom of About (NEXT PROJECT mark)
  xdotool mousemove 700 500; for i in $(seq 1 15); do xdotool click 5; sleep 0.05; done; shot 07-about-bottom-$t 1.5
  quit
done
kill $XVFB 2>/dev/null
ls -la "$OUT"
for f in "$OUT"/*.jpg; do
  echo "==SHOT $(basename "$f")=="; base64 -w 4000 "$f"; echo "==END=="
done
exit 0
