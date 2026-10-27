#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <functional>
#include <QFuture>

class BookmarkManager;
class HistoryManager;

// QWebChannel object behind pacitum://welcome (firstrun-spec.md). Exposed to the page as
// `pacitum.firstRun`; only attached to tabs that are showing pacitum://welcome.
class FirstRunBridge : public QObject {
    Q_OBJECT
public:
    struct Hooks {
        std::function<void(const QString&)> setTheme;    // "system" | "light" | "dark"
        std::function<void(const QString&)> setAccent;   // #RRGGBB
        std::function<QVariantMap()> palette;             // current tokens for the page
        std::function<void()> finish;                     // set flag + navigate to the new tab page
        std::function<void(const QString&)> setSearchEngine;   // 2.0.1: "Google" | "Bing" | "DuckDuckGo"
        std::function<void()> signIn;                     // 2.0.1: Google sign-in for sync
        std::function<void()> openSettings;               // 2.0.1: "Open Settings" on the last step
    };
    FirstRunBridge(BookmarkManager* bm, HistoryManager* hm, Hooks hooks, QObject* parent = nullptr);

    // Installed browsers with a profile we can read: [{id, name, path}], first = pre-selected.
    Q_INVOKABLE QVariantList listImportSources() const;
    // Runs in the background; emits importProgress / importFinished. types: bookmarks, history, passwords, tabs
    Q_INVOKABLE void startImport(const QString& source, const QStringList& types);
    Q_INVOKABLE bool isDefaultBrowser() const;
    Q_INVOKABLE void requestDefaultBrowser();
    Q_INVOKABLE void setTheme(const QString& mode);
    Q_INVOKABLE void setAccent(const QString& hex);
    Q_INVOKABLE QVariantMap palette() const;
    Q_INVOKABLE bool reducedMotion() const;
    Q_INVOKABLE void finish();
    // 2.0.1 welcome steps "Choose a search engine" and "Keep it in sync"
    Q_INVOKABLE QString searchEngine() const;
    Q_INVOKABLE void setSearchEngine(const QString& name);
    Q_INVOKABLE QVariantMap syncState() const;          // {signedIn, email, name, bookmarks, settings, notes, history}
    Q_INVOKABLE void syncSignIn();
    Q_INVOKABLE void syncSignOut();
    Q_INVOKABLE void setSync(const QString& kind, bool on);
    Q_INVOKABLE void openSettings();

    // Blocks until a running background import has finished. The window calls this before it
    // deletes the HistoryManager the worker is writing to (1.5.1: closing the window during an
    // import freed the manager under the worker -> crash).
    void waitForImport();

    static bool isDefaultBrowserStatic();
    static void requestDefaultBrowserStatic();   // opens the OS default-apps settings
    static bool isFirstRunDone();          // browser_settings.json "first_run_done"
    static void markFirstRunDone();

signals:
    void importProgress(const QString& type, int done, int total);
    void importFinished(const QVariantList& results);   // [{type, ok, count, message}]
    void themeChanged(const QVariantMap& palette);
    void defaultBrowserChanged(bool isDefault);
    void syncChanged(const QVariantMap& state);

private:
    BookmarkManager* m_bm;
    HistoryManager* m_hm;
    Hooks m_hooks;
    bool m_importRunning = false;
    QFuture<void> m_future;
};
