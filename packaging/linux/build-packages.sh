#!/usr/bin/env bash
# Builds PacitumBrowser-<ver>-linux-x86_64.deb and .rpm from the bundled AppDir (made by linuxdeploy in CI).
# The app lives in /opt/pacitum-browser (Qt and Chromium libraries inside, so it needs only basic system libraries);
# /usr/bin/pacitum-browser, the menu entry and the icon point at it.
# usage: build-packages.sh <version> <AppDir> <repo root>
set -euo pipefail
ver="$1"; appdir="$2"; src="$3"
root="$PWD/pkgroot"
rm -rf "$root"
mkdir -p "$root/opt/pacitum-browser" "$root/usr/bin" "$root/usr/share/applications" "$root/usr/share/icons/hicolor/256x256/apps"
cp -a "$appdir/usr/." "$root/opt/pacitum-browser/"
ln -s /opt/pacitum-browser/bin/PacitumBrowser "$root/usr/bin/pacitum-browser"
sed 's|^Exec=.*|Exec=pacitum-browser %U|' "$src/resources/linux/pacitum-browser.desktop" > "$root/usr/share/applications/pacitum-browser.desktop"
cp "$src/resources/linux/pacitum-browser.png" "$root/usr/share/icons/hicolor/256x256/apps/pacitum-browser.png"
chmod 755 "$root/opt/pacitum-browser/bin/PacitumBrowser"

cat > "$PWD/after-install.sh" <<'S'
#!/bin/sh
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
exit 0
S
chmod +x "$PWD/after-install.sh"

common=(-s dir -n pacitum-browser -v "$ver" --url "https://github.com/patchon-t/pacitum-browser"
        --maintainer "Pacitum <noreply@users.noreply.github.com>" --category web
        --description "Pacitum Browser: a calm, fast web browser (Qt WebEngine / Chromium)." --after-install "$PWD/after-install.sh" -C "$root")
fpm "${common[@]}" -t deb -a amd64 -p "PacitumBrowser-$ver-linux-x86_64.deb" \
    -d libc6 -d libnss3 -d libxkbcommon-x11-0 -d libxcb-cursor0 -d libxcb-icccm4 -d libxcb-image0 -d libxcb-keysyms1 \
    -d libxcb-render-util0 -d libxcb-xinerama0 -d "libasound2 | libasound2t64" -d libgl1 -d libegl1 -d libxcomposite1 -d libxdamage1 \
    -d libxrandr2 -d libxtst6 -d libfontconfig1 -d libdbus-1-3 .
fpm "${common[@]}" -t rpm -a x86_64 -p "PacitumBrowser-$ver-linux-x86_64.rpm" --no-rpm-autoreqprov \
    -d nss -d libxkbcommon-x11 -d xcb-util-cursor -d xcb-util-wm -d xcb-util-image -d xcb-util-keysyms -d xcb-util-renderutil \
    -d alsa-lib -d mesa-libGL -d mesa-libEGL -d libXcomposite -d libXdamage -d libXrandr -d libXtst -d fontconfig -d dbus-libs .
for f in PacitumBrowser-"$ver"-linux-x86_64.deb PacitumBrowser-"$ver"-linux-x86_64.rpm; do sha256sum "$f" > "$f.sha256"; ls -la "$f"; done
