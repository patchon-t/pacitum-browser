/*
 * Pacitum Browser — Qt6 WebEngine browser
 * Entry point with GPU detection and Chromium flags (mirrors main.py:31)
 */

#include <QApplication>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QIcon>
#include <QFont>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QtGlobal>
#include "BrowserWindow.h"
#include "UpdateOverlay.h"
#include "UpdateUi.h"
#include "ChromeExtensions.h"
#include "ProfileManager.h"
#include "ProfileUi.h"
#include <QMessageBox>
#include <QThread>
#include "PacitumScheme.h"
#include "WebProfile.h"
#include "AppSettings.h"
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMutex>
#include <QTimer>
#include <QUrl>
#include <QFontDatabase>
#include <QTextStream>
#include <QWindow>
#include <QProcess>
#include <QFileInfo>
#include "BrowserTab.h"
#include <ctime>
#include <memory>
#include <exception>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ---------------- startup / crash logging ----------------
// startup.log: every Qt message of the last run, plus timings. crash.log: unhandled exceptions/terminate.
static QString g_logDir;
static FILE* g_log = nullptr;
static QElapsedTimer g_clock;
// Lines are buffered until we know whether this process is the browser (startup.log) or a second
// launch that only hands its URLs to the running one (handoff.log). Opening startup.log with "w"
// right away let every second launch truncate the running browser's log.
static QByteArray g_pendingLog;
static std::string g_crashPath;   // precomputed: no Qt allocations needed inside the crash handler

static QString logDir() {
    if (!g_logDir.isEmpty()) return g_logDir;
#ifdef Q_OS_WIN
    wchar_t buf[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    QString base = n ? QString::fromWCharArray(buf, int(n)) : QDir::tempPath();
#else
    // Linux: ~/.local/share, macOS: ~/Library/Application Support (same place as the profile data)
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.local/share");
#endif
    g_logDir = base + QStringLiteral("/Pacitum/Pacitum Browser");
    QDir().mkpath(g_logDir);
    return g_logDir;
}

void pacitumLog(const QString& line) {
    static QMutex mx; QMutexLocker lock(&mx);
    const QByteArray b = QStringLiteral("[%1 ms] %2\n").arg(g_clock.isValid() ? g_clock.elapsed() : 0, 6).arg(line).toUtf8();
    if (!g_log) { g_pendingLog += b; return; }
    std::fwrite(b.constData(), 1, size_t(b.size()), g_log); std::fflush(g_log);
}

static void openLog(const char* name) {
    if (g_log) return;
    const QString path = logDir() + QLatin1Char('/') + QLatin1String(name);
    g_log = std::fopen(path.toLocal8Bit().constData(), "w");
    if (g_log && !g_pendingLog.isEmpty()) { std::fwrite(g_pendingLog.constData(), 1, size_t(g_pendingLog.size()), g_log); std::fflush(g_log); }
    g_pendingLog.clear();
}

static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    static const char* names[] = {"debug", "warning", "critical", "fatal", "info"};
    pacitumLog(QStringLiteral("%1: %2%3").arg(QLatin1String(names[qBound(0, int(type), 4)]), msg,
               ctx.file ? QStringLiteral(" (%1:%2)").arg(QLatin1String(ctx.file)).arg(ctx.line) : QString()));
    std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
}

static void writeCrash(const char* what) {
    // Plain C only: the heap or Qt may be in a broken state here.
    char stamp[32] = "?";
    const time_t now = time(nullptr);
    if (const tm* lt = localtime(&now)) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", lt);
    const long long ms = g_clock.isValid() ? (long long)g_clock.elapsed() : 0LL;
    if (FILE* f = std::fopen(g_crashPath.c_str(), "a")) {
        std::fprintf(f, "%s Pacitum %s crashed after %lld ms: %s\n", stamp, PACITUM_VERSION, ms, what);
        std::fclose(f);
    }
    if (!g_log) openLog("startup.log");   // keep the buffered lines next to the crash
    if (g_log) { std::fprintf(g_log, "[%6lld ms] CRASH: %s\n", ms, what); std::fflush(g_log); }
}

#ifdef Q_OS_WIN
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    char buf[512];
    HMODULE mod = nullptr; char modName[MAX_PATH] = "?";
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(addr), &mod)) GetModuleFileNameA(mod, modName, MAX_PATH);
    std::snprintf(buf, sizeof(buf), "exception 0x%08lX at %p in %s (+0x%llx)",
                  ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0UL, addr, modName,
                  (unsigned long long)(mod ? (char*)addr - (char*)mod : 0));
    writeCrash(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void installLogging() {
    g_clock.start();
    g_crashPath = (logDir() + QStringLiteral("/crash.log")).toLocal8Bit().toStdString();
    qInstallMessageHandler(messageHandler);
    std::set_terminate([]{
        const char* what = "std::terminate";
        try { if (auto ep = std::current_exception()) std::rethrow_exception(ep); }
        catch (const std::exception& e) { writeCrash(e.what()); std::abort(); }
        catch (...) {}
        writeCrash(what); std::abort();
    });
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(crashFilter);
#endif
    pacitumLog(QStringLiteral("Pacitum %1 starting").arg(QLatin1String(PACITUM_VERSION)));
}

static QString detectGpu() {
#ifdef Q_OS_WIN
    // Simplified: query via WMI would need COM; return generic for flags
    return QStringLiteral("GPU detection: using desktop OpenGL");
#else
    return QStringLiteral("Unknown GPU");
#endif
}

static void setupEnvironment() {
    QString gpu = detectGpu();
    std::cout << "[Browser] " << gpu.toStdString() << std::endl;

    // Conservative, driver-safe Chromium flags. (Earlier builds disabled GPU driver-bug workarounds,
    // forced desktop OpenGL and passed --enable-features twice, which made Chromium drop the first list.)
    // 1.8.7: no forced --enable-gpu-rasterization / --enable-zero-copy. Forcing them skips Chromium's
    // per-GPU blocklist, and zero-copy tiles made 3D / animated pages blink on mouse move on some
    // drivers. Chromium still turns both on by itself where the GPU is known to handle them.
    // PACITUM_DISABLE_GPU=1 falls back to software rendering for broken drivers.
    QByteArray flags;
    if (qEnvironmentVariableIntValue("PACITUM_DISABLE_GPU")) flags += "--disable-gpu --disable-gpu-compositing";
#ifdef Q_OS_WIN
    flags += " --enable-features=OverlayScrollbar,ParallelDownloading --disable-features=DCompPresenter";
#else
    flags += " --enable-features=OverlayScrollbar,ParallelDownloading,VaapiVideoDecoder,VaapiVideoEncoder,WebRTCPipeWireCapturer";
#endif
    // Memory: cap renderer processes (Chromium otherwise starts one per site/tab). Tabs beyond the
    // cap share renderers; PACITUM_RENDERER_LIMIT overrides (0 = Chromium default).
    {
        bool ok = false; const int lim = qEnvironmentVariableIntValue("PACITUM_RENDERER_LIMIT", &ok);
        if (!ok) flags += " --renderer-process-limit=8";
        else if (lim > 0) flags += " --renderer-process-limit=" + QByteArray::number(lim);
    }
    if (qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS")) flags += ' ' + qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
}

// Time-to-first-paint: the first paint event delivered to any widget of the main window.
class FirstPaintProbe : public QObject {
public:
    explicit FirstPaintProbe(QWidget* w) : m_w(w) {}
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::Paint && o->isWidgetType() && static_cast<QWidget*>(o)->window() == m_w) {
            pacitumLog(QStringLiteral("first paint (time-to-first-paint)"));
            qApp->removeEventFilter(this); deleteLater();
        }
        return false;
    }
private:
    QWidget* m_w;
};

// --handoff-test <file> (CI): start a second, ordinary launch of this exe with <file>; it must hand
// the URL to this process (single instance) and exit 0, and the file must open in a tab here.
static void runHandoffTest(BrowserWindow* w) {
    QString file;
    for (const QString& a : qApp->arguments().mid(1)) if (!a.startsWith(QLatin1Char('-'))) file = a;
    const QString want = QFileInfo(file).fileName();
    pacitumLog(QStringLiteral("handoff test starting (%1)").arg(want));
    auto* p = new QProcess(qApp);
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments({file});
    QElapsedTimer* t = new QElapsedTimer; t->start();
    QObject::connect(p, &QProcess::finished, qApp, [p, t, w, want](int code, QProcess::ExitStatus){
        pacitumLog(QStringLiteral("handoff: second launch exited %1 after %2 ms").arg(code).arg(t->elapsed()));
        QTimer::singleShot(1500, qApp, [p, t, w, want, code]{
            bool found = false;
            for (QWidget* tw : QApplication::topLevelWidgets())
                if (auto* b = qobject_cast<BrowserWindow*>(tw)) for (auto* tab : b->findChildren<BrowserTab*>())
                    if (tab->currentUrl().endsWith(want)) found = true;
            const bool ok = found && code == 0 && w->isVisible();
            pacitumLog(QStringLiteral("HANDOFF %1 urlOpened=%2 secondExit=%3").arg(ok ? QStringLiteral("OK") : QStringLiteral("FAIL")).arg(found).arg(code));
            delete t; p->deleteLater();
            QCoreApplication::exit(ok ? 0 : 7);
        });
    });
    p->start();
}

void runPermissionTest(BrowserWindow* w);

int main(int argc, char *argv[]) {
    // Linux updater (1.8.9): the new AppImage first runs as the "Installing update…" pop-up, waits
    // for the old browser to exit and starts itself normally. No browser code runs in this mode.
    for (int i = 1; i < argc; ++i) if (qstrcmp(argv[i], "--update-overlay") == 0) {
        QApplication overlayApp(argc, argv);
        return UpdUi::runInstallOverlay(overlayApp.arguments());
    }
    installLogging();
    bool smoke = false;
    bool stress = false;
    bool resizeTest = false;
    bool perf = false, handoffTest = false;
    bool profileTest = false, guest = false, forcePicker = false, permTest = false, extTest = false;
    QString profileArg;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--smoke-test") == 0) smoke = true;
        if (qstrcmp(argv[i], "--stress-test") == 0) stress = true;
        if (qstrcmp(argv[i], "--resize-test") == 0) resizeTest = true;
        if (qstrcmp(argv[i], "--perf-test") == 0) perf = true;
        if (qstrcmp(argv[i], "--handoff-test") == 0) handoffTest = true;
        if (qstrcmp(argv[i], "--profile-test") == 0) profileTest = true;
        if (qstrcmp(argv[i], "--permissions-test") == 0) permTest = true;
        if (qstrcmp(argv[i], "--extensions-test") == 0) extTest = true;
        if (qstrcmp(argv[i], "--guest") == 0) guest = true;
        if (qstrcmp(argv[i], "--profile-picker") == 0) forcePicker = true;
        if (qstrncmp(argv[i], "--profile-directory=", 20) == 0) profileArg = QString::fromLocal8Bit(argv[i] + 20);
        if (qstrncmp(argv[i], "--profile=", 10) == 0) profileArg = QString::fromLocal8Bit(argv[i] + 10);
    }
    const QString launchDir = QDir::currentPath();   // before we switch to the data dir (relative file args)
    setupEnvironment();
    PacitumSchemeHandler::registerScheme();

    QApplication app(argc, argv);
    pacitumLog(QStringLiteral("QApplication ready"));
#ifdef Q_OS_WIN
    // Lets the installer (AppMutex) detect a running instance before upgrading.
    CreateMutexW(nullptr, FALSE, L"PacitumBrowserRunning");
#endif
    app.setApplicationName(QStringLiteral("Pacitum Browser"));
    app.setOrganizationName(QStringLiteral("Pacitum"));
    app.setApplicationDisplayName(QStringLiteral("Pacitum Browser"));
    app.setApplicationVersion(QStringLiteral(PACITUM_VERSION));
    app.setDesktopFileName(QStringLiteral("pacitum-browser"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/pacitum-icon-256.png")));
    {
        // Inter ships with the app (resources/fonts, SIL OFL) so every machine gets the design font.
        // 2.0: Rubik (variable, SIL OFL) is the UI font; Inter stays as the fallback (Thai and others
        // fall through to the system fonts).
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Rubik-VF.ttf"));
        for (const char* w : {"Regular", "Medium", "SemiBold", "Bold"})
            QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter-%1.ttf").arg(QLatin1String(w)));
        QFont f = app.font();
        f.setFamilies({QStringLiteral("Rubik"), QStringLiteral("Inter"), QStringLiteral("Segoe UI"), QStringLiteral("Cantarell"), f.family()});
        f.setPixelSize(13);
        app.setFont(f);
    }

    // User data lives in %LOCALAPPDATA%/Pacitum/Pacitum Browser (survives upgrades/reinstalls).
    {
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(dataDir);
        const QString oldDir = QCoreApplication::applicationDirPath();   // 1.0/1.1 kept data next to the exe
        for (const char* n : {"browser_settings.json", "bookmarks.json", "browser_session.json", "browser_history.db"}) {
            const QString src = oldDir + QLatin1Char('/') + QLatin1String(n), dst = dataDir + QLatin1Char('/') + QLatin1String(n);
            if (QFile::exists(src) && !QFile::exists(dst)) QFile::copy(src, dst);
        }
        // 1.6 profiles: profiles.json + profiles/<id>/. Migrates pre-1.6 data into "default" first.
        auto* pm = ProfileManager::instance();
        if (profileTest) { openLog("startup.log"); QDir::setCurrent(dataDir); return ProfileManager::runProfileTest(); }
        pm->init(dataDir);
        QStringList urlArgs;
        for (const QString& a : app.arguments().mid(1)) {
            if (a.startsWith(QLatin1Char('-')) || a.isEmpty()) continue;
            const QUrl u = QUrl::fromUserInput(a, launchDir, QUrl::AssumeLocalFile);
            if (u.isValid()) urlArgs << u.toString();
        }
        const bool testRun = smoke || stress || resizeTest || perf || handoffTest || permTest || extTest;
        // Single instance: a plain second launch (taskbar, a link clicked in another app while
        // Pacitum is open) goes to the running browser before anything else -- the profile picker
        // used to come up first, and the link was lost. Explicit --profile/--guest/--profile-picker
        // launches keep their own flow below.
        if (!guest && profileArg.isEmpty() && !forcePicker && !testRun && pm->handOff(pm->lastUsed(), urlArgs)) {
            openLog("handoff.log");
            pacitumLog(QStringLiteral("running instance found; handed off %1 URL(s)").arg(urlArgs.size()));
            return 0;
        }
        if (!guest && profileArg.isEmpty() && !testRun && (pm->shouldShowPicker() || forcePicker)) {
            pacitumLog(QStringLiteral("profile picker shown"));
            const QString pick = ProfilePicker::pickAtStartup();
            if (pick.isEmpty()) { openLog("startup.log"); pacitumLog(QStringLiteral("profile picker closed")); return 0; }
            if (pick == QStringLiteral("guest")) guest = true; else profileArg = pick;
        }
        if (guest) pm->activateGuest();
        else {
            QString id = profileArg.isEmpty() || !pm->contains(profileArg) ? pm->lastUsed() : profileArg;
            // Already open in another process: hand the URLs over (it opens a window) and exit.
            if (pm->handOff(id, urlArgs)) { openLog("handoff.log"); pacitumLog(QStringLiteral("profile %1 already running; handed off").arg(id)); return 0; }
            if (!pm->activate(id)) {
                QThread::msleep(800);
                if (pm->handOff(id, urlArgs)) { openLog("handoff.log"); return 0; }
                openLog("handoff.log");   // never truncate the running instance's startup.log
                pacitumLog(QStringLiteral("profile %1 is locked by another process").arg(id));
                QMessageBox::warning(nullptr, QStringLiteral("Pacitum Browser"), QStringLiteral("This profile is already open in another Pacitum window."));
                return 6;
            }
        }
    }
    openLog("startup.log");   // this process is the browser for its profile
    {   // web prefs from settings before any page is created
        const auto o = AppSettings::read();
        Pacitum::webPrefs().javascript = o.value(QStringLiteral("javascript_enabled")).toBool(true);
        Pacitum::webPrefs().popups = o.value(QStringLiteral("popups_allowed")).toBool(false);
    }

    // Persistent profile storage (mirrors main.py:82)
    QString storagePath = QDir::currentPath() + QStringLiteral("/browser_data");  // persistent storage incl. cookies
    QDir().mkpath(storagePath + QStringLiteral("/storage"));
    QDir().mkpath(storagePath + QStringLiteral("/cache"));
    // 1.9.1: the profile is created with its paths (Qt 6.9+). Setting them afterwards made Qt 6.11
    // rebuild the profile's preferences under the extension engine, and enabling an extension crashed.
    if (!guest) Pacitum::setMainProfileStorage(storagePath + QStringLiteral("/storage"), storagePath + QStringLiteral("/cache"));
    auto* profile = Pacitum::mainProfile();
    if (!guest && !Pacitum::mainProfileBuiltWithStorage()) {   // Qt 6.8: no builder
        profile->setPersistentStoragePath(storagePath + QStringLiteral("/storage"));
        profile->setCachePath(storagePath + QStringLiteral("/cache"));
    }
    pacitumLog(QStringLiteral("profile storage: %1 (built with it: %2)").arg(profile->persistentStoragePath()).arg(Pacitum::mainProfileBuiltWithStorage()));

    auto* settings = profile->settings();
    settings->setAttribute(QWebEngineSettings::AutoLoadIconsForPage, true);
    settings->setAttribute(QWebEngineSettings::TouchIconsEnabled, true);
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    settings->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled, false);
    {   // Spell check needs Chromium .bdic dictionaries, which the Windows bundle does not ship: enabling
        // it anyway only searched the disk at startup and logged "Spellchecking can not be enabled".
        const QString dictEnv = qEnvironmentVariable("QTWEBENGINE_DICTIONARIES_PATH");
        const QString appDict = QCoreApplication::applicationDirPath() + QStringLiteral("/qtwebengine_dictionaries");
        const bool haveDicts = (!dictEnv.isEmpty() && QDir(dictEnv).exists()) || QDir(appDict).exists()
#ifndef Q_OS_WIN
            || QDir(QStringLiteral("/usr/share/qt6/qtwebengine_dictionaries")).exists()
#endif
            ;
        if (haveDicts) {
            profile->setSpellCheckEnabled(true);
            profile->setSpellCheckLanguages({QStringLiteral("en-US")});
        } else pacitumLog(QStringLiteral("spell check off: no qtwebengine_dictionaries"));
    }
    Pacitum::configureProfile(profile, !guest);
    ChromeExtensions::instance()->attach(profile);   // 1.9.0: installed Chrome extensions load with the profile
    Pacitum::configureProfile(QWebEngineProfile::defaultProfile(), false);
    std::cout << "[Browser] profile off-the-record: " << profile->isOffTheRecord() << " storage: " << profile->persistentStoragePath().toStdString() << std::endl;

    pacitumLog(QStringLiteral("profile configured"));
    try {
        // Another launch of this profile (taskbar, profile switcher, default-browser link): reuse this process.
        QObject::connect(ProfileManager::instance(), &ProfileManager::activateRequested, &app, [](const QStringList& urls){
            BrowserWindow* target = qobject_cast<BrowserWindow*>(QApplication::activeWindow());
            if (!target) for (QWidget* tw : QApplication::topLevelWidgets()) if (auto* b = qobject_cast<BrowserWindow*>(tw); b && b->isVisible()) { target = b; break; }
            if (!target || urls.isEmpty()) {
                auto* nw = new BrowserWindow(); nw->setAttribute(Qt::WA_DeleteOnClose);
                for (const QString& u : urls) nw->openUrlInNewTab(u);
                nw->show(); target = nw;
            } else for (const QString& u : urls) target->openUrlInNewTab(u);
            if (target->isMinimized()) target->showNormal();
            target->raise(); target->activateWindow();
            pacitumLog(QStringLiteral("handoff: opened %1 URL(s) from another launch").arg(urls.size()));
        });
        BrowserWindow w;
        pacitumLog(QStringLiteral("window constructed"));
        // URLs / files passed on the command line (default-browser handler: "PacitumBrowser.exe" "%1").
        for (const QString& a : app.arguments().mid(1)) {
            if (a.startsWith(QLatin1Char('-')) || a.isEmpty()) continue;
            const QUrl u = QUrl::fromUserInput(a, launchDir, QUrl::AssumeLocalFile);
            if (u.isValid()) w.openUrlInNewTab(u.toString());
        }
        qApp->installEventFilter(new FirstPaintProbe(&w));
        w.show();
        pacitumLog(QStringLiteral("window shown: visible=%1 geometry=%2,%3 %4x%5").arg(w.isVisible())
                   .arg(w.geometry().x()).arg(w.geometry().y()).arg(w.width()).arg(w.height()));
        QTimer::singleShot(0, &app, []{ pacitumLog(QStringLiteral("event loop running (startup time)")); });
        // Time-to-window: first moment the native window is actually exposed (on screen).
        auto* exposeProbe = new QTimer(&app);
        exposeProbe->setInterval(15);
        QObject::connect(exposeProbe, &QTimer::timeout, &app, [&w, exposeProbe, smoke, stress, resizeTest, perf, handoffTest, permTest, extTest]{
            if (!w.windowHandle() || !w.windowHandle()->isExposed()) return;
            exposeProbe->stop(); exposeProbe->deleteLater();
            pacitumLog(QStringLiteral("window exposed (time-to-window)"));
            if (auto* t = w.findChild<BrowserTab*>()) {   // first web content of the startup tab
                auto c = std::make_shared<QMetaObject::Connection>();
                *c = QObject::connect(t, &BrowserTab::loadDone, &w, [c](bool ok){
                    QObject::disconnect(*c);
                    pacitumLog(QStringLiteral("first page loaded ok=%1 (time-to-first-page)").arg(ok));
                });
            }
            if (handoffTest) QTimer::singleShot(1500, &w, [&w]{ runHandoffTest(&w); });
            else if (permTest) QTimer::singleShot(1500, &w, [&w]{ runPermissionTest(&w); });
            else if (extTest) QTimer::singleShot(1500, &w, [&w]{
                ChromeExtensions::instance()->runSelfTest(&w, [&w](const QString& u){ w.openUrlInNewTab(u); });
            });
            else if (perf) QTimer::singleShot(1500, &w, [&w]{ pacitumLog(QStringLiteral("perf test starting")); w.runPerfTest(); });
            else if (resizeTest) QTimer::singleShot(2500, &w, [&w]{ pacitumLog(QStringLiteral("resize test starting")); w.runResizeTest(); });
            else if (stress) QTimer::singleShot(1500, &w, [&w]{ pacitumLog(QStringLiteral("stress test starting")); w.runStressTest(); });
            else if (smoke) {
                // CI smoke test: window must stay visible; quit cleanly 5 s after it appeared.
                // Meanwhile the update pop-up is opened the way a user does it (1.8.10 crashed there).
                QTimer::singleShot(1500, &w, [&w]{ UpdateFlow::instance()->runSelfTest(&w); });
                QTimer::singleShot(5000, &w, [&w]{
                    const bool ok = w.isVisible() && w.windowHandle() && w.windowHandle()->isExposed() && w.width() > 200 && w.height() > 200;
                    pacitumLog(QStringLiteral("SMOKE %1 visible=%2 exposed=%3 size=%4x%5").arg(ok ? QStringLiteral("OK") : QStringLiteral("FAIL"))
                               .arg(w.isVisible()).arg(w.windowHandle() && w.windowHandle()->isExposed()).arg(w.width()).arg(w.height()));
                    QCoreApplication::exit(ok ? 0 : 3);
                });
            }
        });
        exposeProbe->start();
        if (smoke || stress || resizeTest || perf || handoffTest || permTest || extTest) QTimer::singleShot(extTest ? 240000 : stress || resizeTest || perf || handoffTest || permTest ? 120000 : 40000, &app, []{ pacitumLog(QStringLiteral("TIMEOUT: window never exposed / test hung")); QCoreApplication::exit(4); });
        const int rc = app.exec();
        pacitumLog(QStringLiteral("exit %1").arg(rc));
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
