#include "FirstRun.h"
#include "BookmarkManager.h"
#include "HistoryManager.h"
#include "AppSettings.h"
#include "ThemeManager.h"
#include "SyncManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QPointer>
#include <QColor>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace {
struct Source { QString id, name, path; bool firefox; };

QString home() { return QDir::homePath(); }

QList<Source> detectSources() {
    QList<Source> out;
    auto addChromium = [&](const QString& id, const QString& name, const QString& userData) {
        const QString prof = userData + QStringLiteral("/Default");
        if (QFileInfo::exists(prof + QStringLiteral("/Bookmarks")) || QFileInfo::exists(prof + QStringLiteral("/History")))
            out.push_back({id, name, prof, false});
    };
#ifdef Q_OS_WIN
    const QString local = qEnvironmentVariable("LOCALAPPDATA"), roaming = qEnvironmentVariable("APPDATA");
    addChromium(QStringLiteral("chrome"), QStringLiteral("Chrome"), local + QStringLiteral("/Google/Chrome/User Data"));
    addChromium(QStringLiteral("edge"), QStringLiteral("Edge"), local + QStringLiteral("/Microsoft/Edge/User Data"));
    addChromium(QStringLiteral("brave"), QStringLiteral("Brave"), local + QStringLiteral("/BraveSoftware/Brave-Browser/User Data"));
    const QString ffRoot = roaming + QStringLiteral("/Mozilla/Firefox");
#elif defined(Q_OS_MACOS)
    const QString as = home() + QStringLiteral("/Library/Application Support");
    addChromium(QStringLiteral("chrome"), QStringLiteral("Chrome"), as + QStringLiteral("/Google/Chrome"));
    addChromium(QStringLiteral("edge"), QStringLiteral("Edge"), as + QStringLiteral("/Microsoft Edge"));
    addChromium(QStringLiteral("brave"), QStringLiteral("Brave"), as + QStringLiteral("/BraveSoftware/Brave-Browser"));
    const QString ffRoot = as + QStringLiteral("/Firefox");
#else
    const QString cfg = home() + QStringLiteral("/.config");
    addChromium(QStringLiteral("chrome"), QStringLiteral("Chrome"), cfg + QStringLiteral("/google-chrome"));
    addChromium(QStringLiteral("chromium"), QStringLiteral("Chromium"), cfg + QStringLiteral("/chromium"));
    addChromium(QStringLiteral("edge"), QStringLiteral("Edge"), cfg + QStringLiteral("/microsoft-edge"));
    addChromium(QStringLiteral("brave"), QStringLiteral("Brave"), cfg + QStringLiteral("/BraveSoftware/Brave-Browser"));
    const QString ffRoot = home() + QStringLiteral("/.mozilla/firefox");
#endif
    // Firefox: default profile from profiles.ini (first Default=1, else the first profile with places.sqlite)
    QSettings ini(ffRoot + QStringLiteral("/profiles.ini"), QSettings::IniFormat);
    QString ffProfile;
    for (const QString& g : ini.childGroups()) {
        if (!g.startsWith(QStringLiteral("Profile")) && !g.startsWith(QStringLiteral("Install"))) continue;
        ini.beginGroup(g);
        QString p = ini.value(g.startsWith(QStringLiteral("Install")) ? QStringLiteral("Default") : QStringLiteral("Path")).toString();
        const bool rel = g.startsWith(QStringLiteral("Install")) || ini.value(QStringLiteral("IsRelative"), 1).toInt() == 1;
        const bool def = g.startsWith(QStringLiteral("Install")) || ini.value(QStringLiteral("Default")).toInt() == 1;
        ini.endGroup();
        if (p.isEmpty()) continue;
        if (rel) p = ffRoot + QLatin1Char('/') + p;
        if (!QFileInfo::exists(p + QStringLiteral("/places.sqlite"))) continue;
        if (def || ffProfile.isEmpty()) ffProfile = p;
        if (def) break;
    }
    if (!ffProfile.isEmpty()) out.push_back({QStringLiteral("firefox"), QStringLiteral("Firefox"), ffProfile, true});
    return out;
}

struct Bm { QString title, url; };
struct Hv { QString title, url; };

void walkChromium(const QJsonObject& node, QList<Bm>& out) {
    if (node.value(QStringLiteral("type")).toString() == QLatin1String("url")) {
        out.push_back({node.value(QStringLiteral("name")).toString(), node.value(QStringLiteral("url")).toString()});
        return;
    }
    for (const auto& c : node.value(QStringLiteral("children")).toArray()) walkChromium(c.toObject(), out);
}

// Copies a (possibly locked) SQLite file to a temp dir and runs one query.
QList<QStringList> sqliteQuery(const QString& file, const QString& sql, int cols) {
    QList<QStringList> rows;
    QTemporaryDir tmp; if (!tmp.isValid()) return rows;
    const QString copy = tmp.filePath(QStringLiteral("db.sqlite"));
    if (!QFile::copy(file, copy)) return rows;
    const QString cn = QStringLiteral("pacitum_import_%1").arg(quintptr(&rows));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), cn);
        db.setDatabaseName(copy);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql)) while (q.next()) { QStringList r; for (int i = 0; i < cols; ++i) r << q.value(i).toString(); rows << r; }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(cn);
    return rows;
}

struct ImportResult { QList<Bm> bookmarks; QList<Hv> history; bool bmOk = false, hvOk = false; int hvAdded = 0; };

ImportResult readSource(const Source& s, bool wantBm, bool wantHv, HistoryManager* hm) {
    ImportResult r;
    if (wantBm) {
        if (s.firefox) {
            const auto rows = sqliteQuery(s.path + QStringLiteral("/places.sqlite"),
                QStringLiteral("SELECT b.title, p.url FROM moz_bookmarks b JOIN moz_places p ON p.id = b.fk WHERE b.type = 1 AND p.url LIKE 'http%' LIMIT 5000"), 2);
            for (const auto& row : rows) r.bookmarks.push_back({row[0], row[1]});
            r.bmOk = QFileInfo::exists(s.path + QStringLiteral("/places.sqlite"));
        } else {
            QFile f(s.path + QStringLiteral("/Bookmarks"));
            if (f.open(QIODevice::ReadOnly)) {
                const auto roots = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("roots")).toObject();
                for (const auto& k : roots.keys()) walkChromium(roots.value(k).toObject(), r.bookmarks);
                r.bmOk = true;
            }
        }
    }
    if (wantHv) {
        const QString file = s.firefox ? s.path + QStringLiteral("/places.sqlite") : s.path + QStringLiteral("/History");
        const QString sql = s.firefox
            ? QStringLiteral("SELECT title, url FROM moz_places WHERE url LIKE 'http%' AND visit_count > 0 ORDER BY last_visit_date DESC LIMIT 2000")
            : QStringLiteral("SELECT title, url FROM urls WHERE url LIKE 'http%' ORDER BY last_visit_time DESC LIMIT 2000");
        for (const auto& row : sqliteQuery(file, sql, 2)) r.history.push_back({row[0], row[1]});
        r.hvOk = QFileInfo::exists(file);
        QVector<QPair<QString, QString>> items;
        for (int i = r.history.size() - 1; i >= 0; --i) items.push_back({r.history[i].title, r.history[i].url});
        r.hvAdded = hm->importVisits(items);   // worker thread; HistoryManager is mutex-protected
    }
    return r;
}
} // namespace

FirstRunBridge::FirstRunBridge(BookmarkManager* bm, HistoryManager* hm, Hooks hooks, QObject* parent)
    : QObject(parent), m_bm(bm), m_hm(hm), m_hooks(std::move(hooks)) {
    connect(SyncManager::instance(), &SyncManager::changed, this, [this]{ emit syncChanged(syncState()); });
}

QVariantList FirstRunBridge::listImportSources() const {
    QVariantList l;
    for (const auto& s : detectSources()) l << QVariantMap{{QStringLiteral("id"), s.id}, {QStringLiteral("name"), s.name}};
    return l;
}

void FirstRunBridge::startImport(const QString& source, const QStringList& types) {
    if (m_importRunning) return;
    Source src; bool found = false;
    for (const auto& s : detectSources()) if (s.id == source) { src = s; found = true; }
    if (!found) { emit importFinished({QVariantMap{{QStringLiteral("type"), QStringLiteral("all")}, {QStringLiteral("ok"), false},
                                                   {QStringLiteral("message"), QStringLiteral("Browser not found")}}}); return; }
    m_importRunning = true;
    const bool wantBm = types.contains(QStringLiteral("bookmarks")), wantHv = types.contains(QStringLiteral("history"));
    auto* w = new QFutureWatcher<ImportResult>(this);
    connect(w, &QFutureWatcher<ImportResult>::finished, this, [this, w, src, types, wantBm, wantHv]{
        const ImportResult r = w->result(); w->deleteLater();
        QVariantList res;
        if (wantBm) {
            QVector<QPair<QString, QString>> items;
            for (const auto& b : r.bookmarks) items.push_back({b.title, b.url});
            const int n = m_bm->addBookmarks(items);
            emit importProgress(QStringLiteral("bookmarks"), n, r.bookmarks.size());
            res << QVariantMap{{QStringLiteral("type"), QStringLiteral("bookmarks")}, {QStringLiteral("ok"), r.bmOk}, {QStringLiteral("count"), n},
                               {QStringLiteral("message"), r.bmOk ? QString() : QStringLiteral("Couldn't import bookmarks from %1").arg(src.name)}};
        }
        if (wantHv) {
            emit importProgress(QStringLiteral("history"), r.hvAdded, r.history.size());
            res << QVariantMap{{QStringLiteral("type"), QStringLiteral("history")}, {QStringLiteral("ok"), r.hvOk}, {QStringLiteral("count"), r.history.size()},
                               {QStringLiteral("message"), r.hvOk ? QString() : QStringLiteral("Couldn't import history from %1").arg(src.name)}};
        }
        // Saved passwords are encrypted per-OS-user by the other browser; open tabs live in binary
        // session files. Neither is supported yet: report it instead of pretending.
        for (const char* t : {"passwords", "tabs"})
            if (types.contains(QLatin1String(t)))
                res << QVariantMap{{QStringLiteral("type"), QLatin1String(t)}, {QStringLiteral("ok"), false}, {QStringLiteral("count"), 0},
                                   {QStringLiteral("message"), QStringLiteral("Couldn't import %1 from %2 (not supported yet)")
                                        .arg(QLatin1String(t) == QLatin1String("tabs") ? QStringLiteral("open tabs") : QStringLiteral("passwords"), src.name)}};
        m_importRunning = false;
        emit importFinished(res);
    });
    HistoryManager* hm = m_hm;
    auto fut = QtConcurrent::run([src, wantBm, wantHv, hm]{ return readSource(src, wantBm, wantHv, hm); });
    m_future = QFuture<void>(fut);
    w->setFuture(fut);
}

void FirstRunBridge::waitForImport() { if (m_future.isValid() && !m_future.isFinished()) m_future.waitForFinished(); }

bool FirstRunBridge::isDefaultBrowser() const { return isDefaultBrowserStatic(); }
bool FirstRunBridge::isDefaultBrowserStatic() {
#ifdef Q_OS_WIN
    QSettings uc(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice"), QSettings::NativeFormat);
    return uc.value(QStringLiteral("ProgId")).toString().contains(QStringLiteral("Pacitum"), Qt::CaseInsensitive);
#else
    return false;
#endif
}

void FirstRunBridge::requestDefaultBrowser() {
    requestDefaultBrowserStatic();
    // Re-check a few times while the user is in Settings.
    for (int ms : {3000, 8000, 15000, 30000})
        QTimer::singleShot(ms, this, [this]{ emit defaultBrowserChanged(isDefaultBrowser()); });
}
void FirstRunBridge::requestDefaultBrowserStatic() {
#ifdef Q_OS_WIN
    // Windows 10/11 only allow the user to change it: open Settings → Default apps for Pacitum.
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps?registeredAppUser=Pacitum%20Browser")));
#else
    QDesktopServices::openUrl(QUrl(QStringLiteral("settings://default-apps")));
#endif
}

void FirstRunBridge::setTheme(const QString& mode) { if (m_hooks.setTheme) m_hooks.setTheme(mode); emit themeChanged(palette()); }
void FirstRunBridge::setAccent(const QString& hex) {
    if (!QColor::isValidColorName(hex)) return;
    if (m_hooks.setAccent) m_hooks.setAccent(hex);
    emit themeChanged(palette());
}
QVariantMap FirstRunBridge::palette() const { return m_hooks.palette ? m_hooks.palette() : QVariantMap{}; }
bool FirstRunBridge::reducedMotion() const { return Pacitum::reducedMotion(); }
void FirstRunBridge::finish() { markFirstRunDone(); if (m_hooks.finish) m_hooks.finish(); }

QString FirstRunBridge::searchEngine() const { return AppSettings::get(QStringLiteral("search_engine")).toString(QStringLiteral("Google")); }
void FirstRunBridge::setSearchEngine(const QString& name) { if (m_hooks.setSearchEngine) m_hooks.setSearchEngine(name); }
QVariantMap FirstRunBridge::syncState() const {
    auto* sm = SyncManager::instance();
    return QVariantMap{{QStringLiteral("signedIn"), sm->signedIn()}, {QStringLiteral("email"), sm->account().email},
                       {QStringLiteral("name"), sm->account().name},
                       {QStringLiteral("bookmarks"), sm->syncs(QStringLiteral("bookmarks"))}, {QStringLiteral("settings"), sm->syncs(QStringLiteral("settings"))},
                       {QStringLiteral("notes"), sm->syncs(QStringLiteral("notes"))}, {QStringLiteral("history"), sm->syncs(QStringLiteral("history"))}};
}
void FirstRunBridge::syncSignIn() {
    if (m_hooks.signIn) m_hooks.signIn(); else SyncManager::instance()->signIn();
}
void FirstRunBridge::syncSignOut() { SyncManager::instance()->signOut(); emit syncChanged(syncState()); }
void FirstRunBridge::setSync(const QString& kind, bool on) { SyncManager::instance()->setSyncs(kind, on); emit syncChanged(syncState()); }
void FirstRunBridge::openSettings() { markFirstRunDone(); if (m_hooks.openSettings) m_hooks.openSettings(); }

bool FirstRunBridge::isFirstRunDone() { return AppSettings::get(QStringLiteral("first_run_done")).toBool(false); }
void FirstRunBridge::markFirstRunDone() { AppSettings::set(QStringLiteral("first_run_done"), true); }
