#!/usr/bin/env bash
# Builds PacitumBrowser-<ver>-linux-x86_64.deb and .rpm from the bundled AppDir (made by linuxdeploy in CI).
# The app lives in /opt/pacitum-browser (Qt and Chromium libraries inside, so it needs only basic system libraries);
# /usr/bin/pacitum-browser, the menu entry and the icon point at it.
# usage: build-packages.sh <version> <AppDir> <repo root>
set -euo pipefail
ver="$1"; appdir="$2"; src="$3"
# 2.0.9-A: package versions. A letter is a fixed rebuild and must sort after the release ("2.0.9-A" > "2.0.9"),
# a beta before it ("2.1.0-beta.1" < "2.1.0"). RPM allows no "-" in a version at all.
base="${ver%%-*}"; suffix=""; [ "$ver" != "$base" ] && suffix="${ver#*-}"
if [ -z "$suffix" ]; then debver="$ver"; rpmver="$ver"
elif [[ "$suffix" =~ ^[A-Za-z]$ ]]; then debver="$base+$suffix"; rpmver="$base.$suffix"
else debver="$base~$suffix"; rpmver="$base~$suffix"; fi
echo "package versions: deb $debver, rpm $rpmver"
root="$PWD/pkgroot"
rm -rf "$root"
mkdir -p "$root/opt/pacitum-browser" "$root/usr/bin" "$root/usr/share/applications" "$root/usr/share/icons/hicolor/256x256/apps"
cp -a "$appdir/usr/." "$root/opt/pacitum-browser/"
# 2.0.9-A: /usr/bin/pacitum-browser is a small launcher: it retries in a safe mode when the browser can't start
# (no Chromium sandbox, no working 3D graphics) and writes launch.log, instead of nothing happening at all.
install -m 755 "$src/packaging/linux/pacitum-browser.sh" "$root/opt/pacitum-browser/bin/pacitum-browser"
ln -s /opt/pacitum-browser/bin/pacitum-browser "$root/usr/bin/pacitum-browser"
# AppArmor profile for Ubuntu 24.04 and systems built on it (installed by the after-install script where it applies)
install -D -m 644 "$src/packaging/linux/apparmor-pacitum-browser" "$root/opt/pacitum-browser/share/apparmor/pacitum-browser"
sed 's|^Exec=.*|Exec=pacitum-browser %U|' "$src/resources/linux/pacitum-browser.desktop" > "$root/usr/share/applications/pacitum-browser.desktop"
cp "$src/resources/linux/pacitum-browser.png" "$root/usr/share/icons/hicolor/256x256/apps/pacitum-browser.png"
chmod 755 "$root/opt/pacitum-browser/bin/PacitumBrowser"

cat > "$PWD/after-install.sh" <<'S'
#!/bin/sh
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
# 2.0.9-A: AppArmor 4 systems (Ubuntu 24.04, Mint 22...) only let programs with a profile use the user namespaces
# Chromium's sandbox needs. Older AppArmor doesn't restrict them (and can't read this profile), so it isn't installed there.
if [ -d /etc/apparmor.d ] && [ -e /etc/apparmor.d/abi/4.0 ]; then
    cp /opt/pacitum-browser/share/apparmor/pacitum-browser /etc/apparmor.d/pacitum-browser
    if command -v apparmor_parser >/dev/null 2>&1 && [ -d /sys/kernel/security/apparmor ]; then
        apparmor_parser -r -T -W /etc/apparmor.d/pacitum-browser || true
    fi
fi
exit 0
S
chmod +x "$PWD/after-install.sh"
cat > "$PWD/after-remove.sh" <<'S'
#!/bin/sh
# deb: "remove" / "purge"; rpm: 0 when the package is really removed (1 on an upgrade)
case "$1" in
    remove|purge|0)
        if [ -f /etc/apparmor.d/pacitum-browser ]; then
            command -v apparmor_parser >/dev/null 2>&1 && [ -d /sys/kernel/security/apparmor ] && apparmor_parser -R /etc/apparmor.d/pacitum-browser 2>/dev/null
            rm -f /etc/apparmor.d/pacitum-browser
        fi
        ;;
esac
exit 0
S
chmod +x "$PWD/after-remove.sh"

common=(-s dir -n pacitum-browser --url "https://github.com/patchon-t/pacitum-browser"
        --maintainer "Next PRJ <noreply@users.noreply.github.com>" --vendor "Next PRJ" --category web
        --description "Pacitum Browser: a calm, fast web browser (Qt WebEngine / Chromium)." --after-install "$PWD/after-install.sh" --after-remove "$PWD/after-remove.sh" -C "$root")
fpm "${common[@]}" -v "$debver" -t deb -a amd64 -p "PacitumBrowser-$ver-linux-x86_64.deb" \
    -d "libc6 (>= 2.35)" -d "libstdc++6 (>= 12)" -d libx11-6 -d libx11-xcb1 -d libxcb1 -d libgbm1 -d libwayland-client0 -d libfreetype6 -d libexpat1 -d zlib1g \
    -d libcom-err2 -d libgmp10 -d "libssl3t64 | libssl3" -d ca-certificates \
    -d libnss3 -d libxkbcommon-x11-0 -d libxcb-cursor0 -d libxcb-icccm4 -d libxcb-image0 -d libxcb-keysyms1 \
    -d libxcb-render-util0 -d libxcb-xinerama0 -d "libasound2t64 | libasound2 (>= 1.1)" -d libgl1 -d libegl1 -d libopengl0 -d libglx0 -d libxcomposite1 -d libxdamage1 \
    -d libxrandr2 -d libxtst6 -d libfontconfig1 -d libdbus-1-3 -d libgpg-error0 .
fpm "${common[@]}" -v "$rpmver" -t rpm -a x86_64 -p "PacitumBrowser-$ver-linux-x86_64.rpm" --no-rpm-autoreqprov \
    -d "glibc >= 2.35" -d "libstdc++ >= 12" -d libX11 -d libX11-xcb -d libxcb -d mesa-libgbm -d libwayland-client -d freetype -d expat -d zlib \
    -d libcom_err -d gmp -d openssl-libs -d ca-certificates \
    -d nss -d libxkbcommon-x11 -d xcb-util-cursor -d xcb-util-wm -d xcb-util-image -d xcb-util-keysyms -d xcb-util-renderutil \
    -d alsa-lib -d mesa-libGL -d mesa-libEGL -d libglvnd-opengl -d libglvnd-glx -d libglvnd-egl -d libXcomposite -d libXdamage -d libXrandr -d libXtst -d fontconfig -d dbus-libs -d libgpg-error .
for f in PacitumBrowser-"$ver"-linux-x86_64.deb PacitumBrowser-"$ver"-linux-x86_64.rpm; do sha256sum "$f" > "$f.sha256"; ls -la "$f"; done
