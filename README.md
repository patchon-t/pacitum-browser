![Pacitum_Titile](สกรีนช็อต%202026-10-13%20152618.png)
# Pacitum Browser (Qt6 WebEngine)

A calm, fast web browser written in C++17 on Qt 6 WebEngine.
Version 1.9.1 and 1.8.12 (two editions from the same source):
- **Pacitum 1.9.x** is built with **Qt 6.11** (newer Chromium) and runs **Chrome extensions**: "Add to Pacitum" on a Chrome Web Store page, or add a `.crx` / `.zip` / unpacked folder from the puzzle menu. Pin an extension to the toolbar to open its pop-up; switch it off or remove it from the same menu. Before adding, Pacitum lists what the extension may do.
- **Pacitum 1.8.x** is the **legacy edition** (Qt 6.8 LTS) for computers that can't run the Qt 6.11 build. It gets everything below, but not Chrome extensions, and its updater stays on 1.8.x. **1.8.12 is the last legacy release**: from 1.9.1 on, only the Qt 6.11 edition is built.
- Both: **bookmark folders** (on the bookmarks bar, with a drop-down; right-click to make one or move a bookmark; drag a bookmark onto a folder; pick the folder right after pressing ☆). Folders sync.
- Both: **animations**. Menus, pop-ups and dialogs zoom out of where you opened them, a new window opens with a growing circle, and on macOS and Linux the window shrinks away when closed, drops when minimized and morphs when maximized (Windows animates these itself). The update pop-up zooms in with a soft bounce.
- Both: **security**. Downloads are marked as "from the internet" on Windows so SmartScreen checks them; more file types that run code are held until you confirm; the updater only installs a release with a SHA-256 checksum, downloaded over HTTPS from GitHub.
- Both: **fixes**. The reload button's spin animation never played; restoring a minimized window refreshed the wrong tabs; `localhost:3000` and IP addresses were searched instead of opened; `chrome-extension://` addresses broke; old bookmark chips could flash over new ones; bookmarks are saved atomically.

**1.9.1 fixes**
- Turning a Chrome extension on crashed Pacitum 1.9.0. The main profile is now created with its storage folder (Qt 6.9+), so Qt no longer rebuilds its preferences under the extension engine.
- Extensions from the Chrome Web Store keep their real Chrome ID.

**1.9.0 / 1.8.12 fixes**
- Clicking **Update** on the update card closed Pacitum (1.8.9–1.8.11). The pop-up now opens. On those versions, click **Later** on the card, then the update button in the toolbar, or download Setup from the releases page.
- The profile picture and the ⋮ menu sat on top of the Back button. They are back at the right end of the toolbar.
- The updater picks the installer that has a `.sha256` file, so an extra hand-uploaded file in a release isn't used by mistake.
- If the install helper can't start, Pacitum opens the normal Setup window instead of closing with nothing installed.
- Before installing, the helper waits for Pacitum's background processes to close, so Setup doesn't fail on locked files.
- Update steps are logged to `%TEMP%\PacitumUpdate\update.log`, which is kept when Pacitum restarts.

Version 1.8.10:
- **Search suggestions in the address bar**: as you type, Pacitum lists keyword suggestions from your search engine (Google, Bing, DuckDuckGo, Brave or Yahoo), with the part you typed in normal text and the rest in bold, then up to 4 pages from your history with their titles. Switch it off in Settings › Search engine.
- Fix: typing in the address bar could suddenly be replaced by the page's address (all selected, so the next key wiped it) when the suggestion list opened or closed.
- Fix: typing "h" no longer fills in "ttps://…". Inline completion only fills in site names ("yout" → "youtube.com/").

Version 1.8.9:
- **New updater**: an "update available" card under the toolbar, one progress ring while downloading (Show details for MB, speed and time left; Hide puts the progress in the toolbar), a "Restart now" prompt, then the window shrinks away while an "Installing update…" pop-up stays on screen until the new version is running, which greets you with "Update done" and what's new from the GitHub release. If the old version hangs while closing, it is ended after 25 s instead of blocking the update.
- **Google sync setup guide**: Settings › Sync and Google › Start the guide. Seven steps with a picture of each Google Cloud page, numbered highlights showing where to click, buttons that open the right page, and a drop zone for the downloaded client file (or it picks the Client ID up from the clipboard).

Version 1.8.8:
- **Google sync**: sign in with Google in Settings › Sync and Google; bookmarks, settings, notes and recent history sync between computers through a private area of your Google Drive. One-time setup of the Google sign-in key: see `SYNC.md`.
- **Profile photo**: use your own picture (Edit profile › camera button) or your Google account photo as the profile avatar.
- **Developer tools work again** (F12): the Windows build no longer deletes the developer-tools files.
- **Faster browsing**: the blocker also stops ad/tracker subdomains (e.g. www.google-analytics.com) and no longer blocks pages you open yourself; hovering a link to another site opens the connection early; the disk cache is 512 MB.

Version 1.8.7:
- Fix: 3D and animated web pages (and the New Tab page) could blink while moving the mouse. Pacitum no longer forces Chromium's zero-copy and GPU-rasterization modes (Chromium enables them itself where the GPU supports them), the New Tab depth effect moves its blurred background without re-blurring it, and tab-overview snapshots are taken less often. `PACITUM_DISABLE_GPU=1` switches to software rendering for broken graphics drivers.
- The ⋮ menu is shorter: New tab / New window / Private tab and the built-in tools are one row each.

Version 1.8.6:
- **Linux and macOS**: Pacitum now builds and runs on all three systems. Linux gets an AppImage; macOS gets a `.dmg` with the red/yellow/green window buttons and the camera/microphone permission texts macOS requires. The updater downloads the right file for each system. See `BUILD.md`.
- Fix: the browser could crash while closing (from 1.8.5).

Version 1.8.5:
- **UI refresh**: the active tab merges into the toolbar and slides between tabs; rounded-rectangle omnibox; new tab, link, image, text and page right-click menus; Settings search with a sliding theme switcher.
- **Split view**: grip handle (double-click for 50/50), accent bar on the focused side, swap sides (Ctrl+Alt+X), toggle with Ctrl+Alt+S, links open straight into the right side.
- **Extensions**: user scripts and styles loaded from a folder with a `manifest.json` (simple Chrome content-script extensions load too). See `examples/extensions/`.
- **Built-in tools**: screenshot (Ctrl+Shift+S), picture-in-picture (Alt+P), notes side panel (Ctrl+Shift+Y), QR code for the page.
- **3D**: 3D tab overview (Ctrl+Shift+Space), back/forward pages turn away in perspective, 3D depth on the New Tab page. All off with reduced motion.
- The Inter font ships with the app (`resources/fonts`, SIL Open Font License); QR codes use Nayuki's QR Code generator (`third_party/qrcodegen`, MIT).

Version 1.7.8: omnibox inline hints select only the untyped suffix.

Version 1.7.7: reader mode (Ctrl+Shift+R) and Translate page (Google Translate, language submenu in the ⋮ menu).

Version 1.2.0: merged Chrome-style title bar (frameless window, tabs in caption row, custom min/max/close, Aero Snap via WM_NCHITTEST), animations (tab width, hover, tab fade, loading spinner, menu fade, NTP), full-page Settings (`pacitum://settings`), standard Chrome user agent + persistent disk profile, in-place upgrading installer. User data now lives in `%LOCALAPPDATA%\Pacitum\Pacitum Browser` (migrated automatically from the install folder).

Version 1.1.0 implemented the Pacitum design system: graphite/paper surfaces, single teal accent, pill omnibox,
tab strip above the toolbar, design SVG icon set tinted at runtime, and a built-in `pacitum://newtab` page.

## Architecture (1:1 with Python)
| Python (`safari_browser/*.py`) | C++ (`safari_browser_cpp/include|src/*.cpp`) |
|---|---|
| `main.py` | `src/main.cpp` |
| `browser_window.py` | `BrowserWindow.h/cpp` |
| `browser_tab.py` | `BrowserTab.h/cpp` |
| `tab_widget.py` | `TabWidget.h/cpp` (PacitumTabWidget + PacitumTabBar) |
| `address_bar.py` | `AddressBar.h/cpp` |
| `bookmark_manager.py` | `BookmarkManager.h/cpp` |
| `bookmarks_bar.py` | `BookmarksBar.h/cpp` |
| `history_manager.py` | `HistoryManager.h/cpp` (SQLite via QtSql) |
| `download_manager.py` | `DownloadManager.h/cpp` |
| `find_bar.py` | `FindBar.h/cpp` |
| `ad_blocker.py` | `AdBlocker.h/cpp` (QWebEngineUrlRequestInterceptor) |
| `session_manager.py` | `SessionManager.h/cpp` |
| `zoom_controller.py` | `ZoomController.h/cpp` |
| `settings_dialog.py` | `SettingsPage.h/cpp` (full-page settings tab) |
| — | `WebProfile.h/cpp` (UA, Accept-Language, persistent profile) · `Widgets.h/cpp` (animated buttons, switches, spinner) |
| `theme_manager.py` | `ThemeManager.h/cpp` (Pacitum design tokens → QSS, SVG icon tinting) |
| — | `PacitumScheme.h/cpp` (`pacitum://newtab` internal page) |

## Prerequisites
- Qt 6.8+ with **QtWebEngineWidgets**. CI builds both editions: Qt 6.11 → Pacitum 1.9.x (Chrome extensions), Qt 6.8 LTS → Pacitum 1.8.x legacy. CMake picks the version from the Qt it finds.
- CMake 3.16+, Ninja or MSVC/MinGW
- C++17 compiler (MSVC 2019+, MinGW 13+, GCC 11+)

On Windows with MSVC:
```
winget install Qt.Qt.6.9
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

On MSYS2 UCRT64 (mingw):
```
pacman -S mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-webengine \
          mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

On Ubuntu, install Qt 6.8+ with QtWebEngineWidgets using the Qt Online Installer; the distribution packages may be older than the required version. Install CMake and Ninja with `sudo apt install cmake ninja-build`.

## Build
```bash
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/<version>/msvc2022_64" -G Ninja
cmake --build build --config Release
./build/PacitumBrowser        # or build/PacitumBrowser.exe on Windows
```

Qt Creator: Open `CMakeLists.txt` → Configure → Run.

## Features (parity with Python)
- Pacitum UI: tab strip (40) → toolbar (44) → bookmarks bar (32); pill omnibox with lock/warning/search icon and star; ⋮ main menu with zoom row; System/Light/Dark themes + accent colors
- Tabs: drag, close others/right, duplicate, split view (two PacitumTabWidgets in QSplitter), pin/mute, vertical mode (Ctrl+Shift+E)
- Navigation, history (SQLite), bookmarks (JSON), downloads (QWebEngineDownloadRequest), find (Ctrl+F), zoom 25-500%
- Ad blocker (host + path interceptor), private mode (NoPersistentCookies), session restore, Do Not Track
- Hardware acceleration, GPU raster, 512 MB disk cache, spellcheck, crash auto-reload

Made by Ai (Grok bot + opencode-BaseApp + claude code)

## Settings
Stored in `browser_settings.json`, `bookmarks.json`, `browser_session.json`, `browser_history.db` (identical filenames to Python so data migrates).

## Windows installer
GitHub Actions (`.github/workflows/windows.yml`) builds `PacitumBrowser.exe` with Qt 6.8 LTS MSVC 2022, bundles it with windeployqt and produces `installer/PacitumBrowser-Setup-<ver>.exe` via Inno Setup (`installer.iss`). The app icon (`resources/pacitum.ico`, 16–256 px) is embedded through `resources/app.rc`.

## System requirements
Since Pacitum Browser 1.8.7 Pacitum runs on Windows, macOS (Apple silicon) and Linux.
Since 1.9.0 there are two editions built from the same source:

| Edition | Built with | Chromium | Chrome extensions | Updates | Support |
|---|---|---|---|---|---|
| **Pacitum 1.9.x** (recommended) | Qt 6.11 | newer Chromium | ✅ | newest version | ✅ |
| **Pacitum 1.8.x legacy** | Qt 6.8 LTS | Chromium 122 base (security fixes backported) | — | stays on 1.8.x | - |

Use the legacy edition only if 1.9 doesn't start or doesn't draw correctly on your computer (for example older graphics drivers).

| | Windows | Linux | macOS |
|---|---|---|---|
| **OS** | Windows 10 (1809+) or Windows 11, 64-bit | x86_64 distro with glibc 2.35+ (Ubuntu 22.04, Debian 12, Fedora 36 or newer) | macOS 12 Monterey or newer (1.8.x legacy); 1.9.x may need a newer macOS |
| **CPU** | x86_64 | x86_64 | Apple silicon (M1 or newer) |
| **Download** | `PacitumBrowser-Setup-<ver>.exe` | `PacitumBrowser-<ver>-linux-x86_64.AppImage` | `PacitumBrowser-<ver>-macOS-arm64.dmg` |

Pick the file from the release of your edition: tag `v1.9.x` for 1.9, tag `v1.8.x` for legacy.

- **Memory:** 4 GB RAM minimum, 8 GB recommended (each site runs in its own Chromium process; Pacitum caps this at 8).
- **Disk:** about 500 MB free for the app plus space for cache, profile data and Chrome extensions.
- **Graphics:** a GPU with Direct3D 11 (Windows), OpenGL (Linux) or Metal (macOS) support. With a broken driver, start with `PACITUM_DISABLE_GPU=1` to use software rendering.
- **Linux AppImage:** needs FUSE (`libfuse2`) to run, or run it with `--appimage-extract-and-run`.
- **macOS:** the app isn't notarized; the first time, right-click → **Open**.

### Building from source
- Qt with Qt WebEngine, Qt WebChannel and Qt Positioning (optional: Qt Serial Port, Qt Connectivity):
  - **Qt 6.11 or newer** → builds Pacitum **1.9.x** (Chrome extensions)
  - **Qt 6.8 LTS** → builds Pacitum **1.8.x legacy**
  - CMake picks the edition from the Qt it finds and prints it (`-- Pacitum 1.9.0 with Qt 6.11.x`).
- CMake 3.16+ and Ninja
- A C++17 compiler: MSVC 2019+ (Windows), GCC 11+ (Linux), Xcode / Apple Clang (macOS)
- Windows CI note: aqtinstall 3.3.0 can't install Qt 6.11 for MSVC; the workflow pins the upstream fix.

See `BUILD.md` for the steps on each system.

### Building from source
- Qt **6.8 or newer** with Qt WebEngine, Qt WebChannel and Qt Positioning (optional: Qt Serial Port, Qt Connectivity)
- CMake 3.16+ and Ninja
- A C++17 compiler: MSVC 2019+ (Windows), GCC 11+ (Linux), Xcode / Apple Clang (macOS)

See `BUILD.md` for the steps on each system.
