# Build — Pacitum Browser

Pacitum builds on **Windows, Linux and macOS** with Qt 6.8+ (Qt WebEngine), CMake 3.16+ and a C++17 compiler.

**Two editions from one source:** with Qt **6.11 or newer** you get Pacitum **1.9.x** (newer Chromium, Chrome extensions);
with Qt **6.8 LTS** you get the **1.8.x legacy** edition (no Chrome extensions; its updater only offers 1.8.x releases).
CMake prints which one it is (`-- Pacitum 1.9.0 with Qt 6.11.x`) and writes it to `build/pacitum-version.txt`. The examples
below use 6.8.3; use your 6.11 path for 1.9. To release: run Actions → Release with a tag such as `v1.9.1` (needs `release-notes/<tag>.md`) (each workflow job uploads only to its own tag).
Install Qt with the official Qt Online Installer (or `aqtinstall`) and include the modules
**Qt WebEngine, Qt WebChannel, Qt Positioning**, and optionally **Qt Serial Port** and **Qt Connectivity** (Bluetooth).

## Windows (MSVC)
```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build
build\PacitumBrowser.exe
```
Installer: `installer.iss` (Inno Setup 6.6+), see `.github/workflows/windows.yml`.

## Linux
```bash
sudo apt install ninja-build libgl1-mesa-dev libxkbcommon-x11-0 libxcb-cursor0 libnss3   # Debian/Ubuntu
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/gcc_64"
cmake --build build
./build/PacitumBrowser
sudo cmake --install build      # optional: binary, menu entry and icon under /usr/local
```
An AppImage is produced by `.github/workflows/linux-macos.yml` (linuxdeploy + Qt plugin).
Data and logs: `~/.local/share/Pacitum/Pacitum Browser`.

## macOS (Apple silicon or Intel)
```bash
brew install ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build build
open build/PacitumBrowser.app
```
To share the app: `macdeployqt build/PacitumBrowser.app` then `codesign --force --deep --sign - build/PacitumBrowser.app`
(ad-hoc signature; Apple silicon won't run unsigned code). CI builds a `.dmg`.
The download isn't notarized, so the first launch needs right-click → **Open**.
Data and logs: `~/Library/Application Support/Pacitum/Pacitum Browser`.

## Platform notes
- **Window frame:** Pacitum draws its own title bar. Windows uses native snap/resize hit-testing; Linux and macOS move
  and resize through the system (`startSystemMove` / `startSystemResize`). macOS shows red/yellow/green buttons on the left.
- **Updates:** the built-in updater picks the asset for the running system: `*Setup*.exe` (Windows, installs and
  restarts), `*linux*.AppImage` (replaces the running AppImage and restarts), `*macOS*.dmg` (opens the disk image).
- **Shortcuts:** `Ctrl` shortcuts are `Cmd` on macOS (Qt maps them automatically).
