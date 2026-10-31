#!/usr/bin/env bash
# Runs inside a Fedora container: installs the .rpm (dependencies must resolve) and starts the app on a virtual display.
set -u
rpmfile="$1"
dnf install -y "$rpmfile" xorg-x11-server-Xvfb >/dev/null || { echo "dnf install failed"; exit 1; }
test -x /usr/bin/pacitum-browser || { echo "no /usr/bin/pacitum-browser"; exit 1; }
export QTWEBENGINE_DISABLE_SANDBOX=1
xvfb-run -a -s "-screen 0 1440x900x24" /usr/bin/pacitum-browser --smoke-test || echo "exit code $?"
log="$HOME/.local/share/Pacitum/Pacitum Browser/startup.log"
tail -n 40 "$log" || true
grep -q "SMOKE OK" "$log"
