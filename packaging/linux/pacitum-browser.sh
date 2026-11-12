#!/bin/sh
# Pacitum Browser launcher for the .deb / .rpm (2.0.9-A): /usr/bin/pacitum-browser.
#
# When the browser ends within a few seconds of starting with an error, it is started once more in a safe mode,
# so a computer where Chromium can't start its sandbox or its graphics still opens a window:
#   - "No usable sandbox" (user namespaces turned off or restricted): started without Chromium's sandbox
#   - anything else (often no working 3D graphics, e.g. a virtual machine): software rendering, X11
# Everything goes to ~/.local/share/Pacitum/Pacitum Browser/launch.log. PACITUM_SAFE_MODE=1 starts in safe mode.
APP=/opt/pacitum-browser/bin/PacitumBrowser
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/Pacitum/Pacitum Browser"
LOG="$DIR/launch.log"
mkdir -p "$DIR" 2>/dev/null
{ echo "=== $(date '+%Y-%m-%d %H:%M:%S') pacitum-browser $*"; echo "session=${XDG_SESSION_TYPE:-?} display=${DISPLAY:-} wayland=${WAYLAND_DISPLAY:-}"; } > "$LOG" 2>/dev/null

safe_graphics() {
    export PACITUM_DISABLE_GPU=1 QT_QUICK_BACKEND=software QT_XCB_GL_INTEGRATION=none LIBGL_ALWAYS_SOFTWARE=1
    [ -n "$DISPLAY" ] && export QT_QPA_PLATFORM=xcb
}

if [ "${PACITUM_SAFE_MODE:-0}" = 1 ]; then
    echo "safe mode asked for" >> "$LOG"
    safe_graphics
    exec "$APP" "$@" 2>>"$LOG"
fi

start=$(date +%s)
"$APP" "$@" 2>>"$LOG"
rc=$?
took=$(( $(date +%s) - start ))
echo "exit $rc after ${took}s" >> "$LOG"
# 0 = normal quit or handed over to a running window; 6 = profile already open (Pacitum said so itself).
if [ "$rc" -eq 0 ] || [ "$rc" -eq 6 ] || [ "$took" -ge 20 ]; then exit "$rc"; fi

# Chromium's sandbox can't start: "No usable sandbox" (no user namespaces), or "credentials.cc ... Permission denied"
# (Ubuntu 24.04-style AppArmor restriction when the profile isn't loaded).
sandbox_broken() { grep -q -e "No usable sandbox" -e "credentials.cc" -e "zygote_host_impl_linux" -e "setuid_sandbox_host" "$LOG" 2>/dev/null; }
try() { echo "retrying: $1" >> "$LOG"; shift; start=$(date +%s); "$APP" "$@" 2>>"$LOG"; rc=$?; took=$(( $(date +%s) - start )); echo "exit $rc after ${took}s" >> "$LOG"; }
if sandbox_broken; then
    export QTWEBENGINE_DISABLE_SANDBOX=1
    try "without Chromium's sandbox (this system doesn't allow it)" "$@"
else
    safe_graphics
    try "with software rendering" "$@"
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 6 ] && [ "$took" -lt 20 ]; then
        export QTWEBENGINE_DISABLE_SANDBOX=1
        try "with software rendering and without Chromium's sandbox" "$@"
    fi
fi
if [ "$rc" -ne 0 ] && [ "$rc" -ne 6 ]; then
    msg="Pacitum Browser couldn't start. Details are in $LOG"
    if command -v zenity >/dev/null 2>&1; then zenity --error --no-wrap --title="Pacitum Browser" --text="$msg" 2>/dev/null
    elif command -v kdialog >/dev/null 2>&1; then kdialog --title "Pacitum Browser" --error "$msg" 2>/dev/null
    elif command -v notify-send >/dev/null 2>&1; then notify-send "Pacitum Browser" "$msg" 2>/dev/null
    fi
    echo "$msg" >&2
fi
exit "$rc"
