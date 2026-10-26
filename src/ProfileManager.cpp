#include "ProfileManager.h"
#include <QThread>
#include "WebProfile.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <atomic>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QLocalSocket>
#include <QLockFile>
#include <QPainter>
#include <QPainterPath>
#include <QImage>
#include <QHash>
#include <QProcess>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QIcon>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QNetworkCookie>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <iostream>
#include "ThemeManager.h"

void pacitumLog(const QString& line);   // main.cpp (startup.log)

static const QString kRegistry = QStringLiteral("profiles.json");

QJsonObject ProfileInfo::toJson() const {
    QJsonObject o{{"id", id}, {"name", name}, {"colour", colour}, {"avatar", avatar},
                  {"created", double(created)}, {"last_used", double(lastUsed)}};
    if (!email.isEmpty()) o["email"] = email;
    return o;
}
ProfileInfo ProfileInfo::fromJson(const QJsonObject& o) {
    ProfileInfo p;
    p.id = o.value("id").toString(); p.name = o.value("name").toString(); p.email = o.value("email").toString();
    p.colour = o.value("colour").toString(QStringLiteral("#D9141F")); p.avatar = o.value("avatar").toString(QStringLiteral("initial"));
    p.created = qint64(o.value("created").toDouble()); p.lastUsed = qint64(o.value("last_used").toDouble());
    return p;
}

const QStringList& ProfileManager::colours() {
    static const QStringList c{"#D9141F", "#2DD4BF", "#6366F1", "#EC4899", "#F43F5E", "#F59E0B", "#22C55E", "#0EA5E9", "#64748B"};
    return c;
}
const QStringList& ProfileManager::glyphs() {
    static const QStringList g{"initial", "window", "globe", "bookmarks", "palette", "sparkle", "file-code"};
    return g;
}

ProfileManager* ProfileManager::instance() { static auto* m = new ProfileManager(qApp); return m; }
ProfileManager::ProfileManager(QObject* parent) : QObject(parent) {}

QString ProfileManager::profileDir(const QString& id) const { return m_root + QStringLiteral("/profiles/") + id; }

// ---- 1.8.8 custom profile photo ----
namespace { QString s_pendingId; QImage s_pendingPhoto; bool s_hasPending = false; }
QString ProfileManager::photoPath(const QString& id) const { return profileDir(id) + QStringLiteral("/avatar.png"); }
QImage ProfileManager::squarePhoto(const QImage& src) {
    if (src.isNull()) return {};
    const int side = qMin(src.width(), src.height());
    const QImage sq = src.copy((src.width() - side) / 2, (src.height() - side) / 2, side, side);
    return sq.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
void ProfileManager::setPendingPhoto(const QString& id, const QImage& img) {
    s_hasPending = !img.isNull(); s_pendingId = id; s_pendingPhoto = img;
}
QImage ProfileManager::photoFor(const ProfileInfo& p) {
    if (s_hasPending && p.id == s_pendingId) return s_pendingPhoto;
    if (p.id.isEmpty()) return {};
    static QHash<QString, QPair<qint64, QImage>> cache;   // path -> (mtime, image)
    const QString path = instance()->photoPath(p.id);
    const QFileInfo fi(path);
    if (!fi.exists()) return {};
    const qint64 stamp = fi.lastModified().toMSecsSinceEpoch() ^ fi.size();
    auto it = cache.find(path);
    if (it != cache.end() && it->first == stamp) return it->second;
    QImage img(path);
    cache.insert(path, qMakePair(stamp, img));
    return img;
}
bool ProfileManager::setPhoto(const QString& id, const QImage& img) {
    if (!contains(id)) return false;
    const QImage sq = squarePhoto(img);
    if (sq.isNull()) return false;
    QDir().mkpath(profileDir(id));
    if (!sq.save(photoPath(id), "PNG")) return false;
    ProfileInfo p = profile(id);
    p.avatar = QStringLiteral("photo");
    update(p);   // notifies windows (avatar repaint)
    return true;
}

QJsonObject ProfileManager::readRegistry() const {
    const QFileInfo fi(m_root + QLatin1Char('/') + kRegistry);
    const qint64 stamp = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() ^ (fi.size() << 40) : -2;
    if (stamp == m_cacheStamp) return m_cache;
    QFile f(fi.filePath());
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) { const auto d = QJsonDocument::fromJson(f.readAll()); if (d.isObject()) o = d.object(); }
    m_cache = o; m_cacheStamp = stamp;
    return o;
}
bool ProfileManager::writeRegistry(const QJsonObject& reg) const {
    QSaveFile f(m_root + QLatin1Char('/') + kRegistry);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(reg).toJson(QJsonDocument::Indented));
    if (!f.commit()) return false;
    m_cacheStamp = -1;
    return true;
}

// ---------------- migration ----------------
static QByteArray fileHash(const QString& path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256); h.addData(&f); return h.result();
}
// Top-level entries of the pre-1.6 data dir that belong to the (single) profile. Everything except
// app-level files (logs, the registry, test output, Chromium's default-profile folder).
static bool isLegacyProfileEntry(const QFileInfo& fi) {
    const QString n = fi.fileName();
    static const QStringList skip{"profiles", "profiles.json", "QtWebEngine", "resize-shots", "stress-shots", "PacitumUpdate"};
    if (skip.contains(n) || n.endsWith(QStringLiteral(".log")) || n.startsWith(QLatin1Char('.'))) return false;
    return true;
}
// Relative paths of every file to migrate (browser_data/cache is regenerated, so it is skipped).
static QStringList legacyFiles(const QString& root) {
    QStringList out;
    const QDir r(root);
    for (const QFileInfo& fi : r.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (!isLegacyProfileEntry(fi)) continue;
        if (fi.isFile()) { out << fi.fileName(); continue; }
        QDirIterator it(fi.filePath(), QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString rel = r.relativeFilePath(it.next());
            if (rel.startsWith(QStringLiteral("browser_data/cache/"))) continue;
            out << rel;
        }
    }
    return out;
}
static bool copyVerified(const QString& from, const QString& to, const QStringList& files, QString* log,
                         const std::function<void(qint64, qint64)>& progress = {}) {
    qint64 total = 0, done = 0;
    if (progress) { for (const QString& rel : files) total += QFileInfo(from + QLatin1Char('/') + rel).size(); progress(0, total); }
    for (const QString& rel : files) {
        const QString s = from + QLatin1Char('/') + rel, d = to + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(d).absolutePath());
        QFile::remove(d);
        if (!QFile::copy(s, d)) { if (log) *log += QStringLiteral("copy failed: %1\n").arg(rel); return false; }
        QFile::setPermissions(d, QFile::permissions(s) | QFileDevice::WriteOwner);
        if (QFileInfo(s).size() != QFileInfo(d).size() || fileHash(s) != fileHash(d)) {
            if (log) *log += QStringLiteral("verify failed: %1\n").arg(rel);
            return false;
        }
        if (progress) { done += QFileInfo(s).size(); progress(done, total); }
    }
    return true;
}
static bool verifyTree(const QString& from, const QString& to, const QStringList& files) {
    for (const QString& rel : files) {
        const QString s = from + QLatin1Char('/') + rel, d = to + QLatin1Char('/') + rel;
        if (!QFileInfo::exists(d) || QFileInfo(s).size() != QFileInfo(d).size() || fileHash(s) != fileHash(d)) return false;
    }
    return true;
}
static QJsonObject readJson(const QString& path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
    const auto d = QJsonDocument::fromJson(f.readAll()); return d.isObject() ? d.object() : QJsonObject{};
}

bool ProfileManager::migrateLegacy(const QString& root, QString* log, const std::function<void(qint64, qint64)>& progress) {
    const QString regPath = root + QLatin1Char('/') + kRegistry;
    const QJsonObject reg = readJson(regPath);
    if (reg.value("profiles").isArray() && !reg.value("profiles").toArray().isEmpty()) {
        if (log) *log += QStringLiteral("registry present; nothing to migrate\n");
        return true;   // idempotent: never re-run once a registry exists
    }
    const QString target = root + QStringLiteral("/profiles/default");
    const QString staging = root + QStringLiteral("/profiles/default.migrating");
    const QStringList files = legacyFiles(root);
    if (!files.isEmpty()) {
        if (QFileInfo::exists(target) && !verifyTree(root, target, files)) {
            // A target without a registry that doesn't match: keep it aside, never delete data.
            const QString aside = target + QStringLiteral(".incomplete-") + QString::number(QDateTime::currentSecsSinceEpoch());
            QDir().rename(target, aside);
            if (log) *log += QStringLiteral("kept unverified folder as %1\n").arg(aside);
        }
        if (!QFileInfo::exists(target)) {
            if (QFileInfo::exists(staging)) {       // leftover of an interrupted copy (copy only, never the source)
                const bool removed = QDir(staging).removeRecursively();
                pacitumLog(QStringLiteral("profiles: leftover %1 from an interrupted migration %2").arg(staging, removed ? QStringLiteral("removed") : QStringLiteral("could NOT be fully removed")));
                if (log) *log += QStringLiteral("cleaned up leftover default.migrating\n");
            }
            QDir().mkpath(staging);
            if (!copyVerified(root, staging, files, log, progress)) return false;
            if (!QDir().rename(staging, target)) { if (log) *log += QStringLiteral("rename failed\n"); return false; }
        }
        if (log) *log += QStringLiteral("migrated %1 files into profiles/default (originals kept as backup)\n").arg(files.size());
    } else {
        QDir().mkpath(target);
    }
    // Registry last: until it exists the migration is simply redone.
    const QJsonObject s = readJson(target + QStringLiteral("/browser_settings.json"));
    ProfileInfo p;
    p.id = defaultId();
    p.name = s.value("profile_name").toString().trimmed();
    if (p.name.isEmpty()) p.name = QStringLiteral("Default");
    const QString acc = s.value("accent").toString();
    p.colour = colours().contains(acc, Qt::CaseInsensitive) ? acc.toUpper() : QStringLiteral("#D9141F");
    p.created = p.lastUsed = QDateTime::currentMSecsSinceEpoch();
    QJsonObject out{{"version", 1}, {"profiles", QJsonArray{p.toJson()}}, {"last_used", p.id}, {"show_picker", true}};
    if (!files.isEmpty()) out["legacy_backup"] = QStringLiteral("pre-1.6 files in this folder were copied to profiles/default and left as a backup");
    QSaveFile f(regPath);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool ProfileManager::init(const QString& root) {
    m_root = root; m_cacheStamp = -1;
    QDir().mkpath(root + QStringLiteral("/profiles"));
    for (const auto& d : QDir(root + QStringLiteral("/profiles")).entryList({QStringLiteral(".deleted-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot))
        QDir(root + QStringLiteral("/profiles/") + d).removeRecursively();   // leftovers of a delete that hit locked files
    QString log;
    // Big pre-1.6 profiles (GBs of web storage) take many seconds to copy and verify. Run the copy on
    // a worker and, once it is clearly not instant, show a small progress window -- a blank 20+ s
    // startup made people kill the process, which restarted the copy from scratch next time.
    std::atomic<qint64> done{0}, total{0};
    QFuture<bool> job = QtConcurrent::run([&]{ return migrateLegacy(root, &log, [&](qint64 d, qint64 t){ done = d; total = t; }); });
    if (!job.isFinished()) {
        QEventLoop loop;
        QFutureWatcher<bool> watch;
        QObject::connect(&watch, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
        watch.setFuture(job);
        QWidget* win = nullptr; QProgressBar* bar = nullptr; QLabel* detail = nullptr;
        QTimer tick; tick.setInterval(100);
        QElapsedTimer since; since.start();
        QObject::connect(&tick, &QTimer::timeout, &loop, [&]{
            if (!win && since.elapsed() > 700 && total > 0) {
                win = new QWidget(nullptr, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
                win->setWindowTitle(QStringLiteral("Pacitum Browser"));
                auto* lay = new QVBoxLayout(win); lay->setContentsMargins(24, 20, 24, 20); lay->setSpacing(10);
                auto* title = new QLabel(QStringLiteral("Updating your profile\u2026"), win);
                QFont f = title->font(); f.setPixelSize(15); f.setBold(true); title->setFont(f);
                auto* sub = new QLabel(QStringLiteral("This happens once. Please don't close Pacitum."), win);
                bar = new QProgressBar(win); bar->setRange(0, 1000); bar->setTextVisible(false); bar->setFixedHeight(6);
                detail = new QLabel(win);
                lay->addWidget(title); lay->addWidget(sub); lay->addWidget(bar); lay->addWidget(detail);
                win->setFixedWidth(380); win->show();
                pacitumLog(QStringLiteral("profiles: migration progress window shown (%1 MB)").arg(total / 1048576));
            }
            if (win && total > 0) {
                bar->setValue(int(1000.0 * done / total));
                detail->setText(QStringLiteral("%1 of %2 MB").arg(done / 1048576).arg(total / 1048576));
            }
        });
        tick.start();
        if (!job.isFinished()) loop.exec();
        tick.stop();
        delete win;
    }
    const bool ok = job.result();
    pacitumLog(QStringLiteral("profiles: %1").arg(log.trimmed().replace(QLatin1Char('\n'), QStringLiteral("; "))));
    if (!ok) pacitumLog(QStringLiteral("profiles: MIGRATION FAILED, legacy data untouched"));
    for (const auto& p : profiles()) QDir().mkpath(profileDir(p.id));
    return ok;
}

QList<ProfileInfo> ProfileManager::profiles() const {
    QList<ProfileInfo> out;
    for (const auto& v : readRegistry().value("profiles").toArray()) {
        ProfileInfo p = ProfileInfo::fromJson(v.toObject());
        if (!p.id.isEmpty()) out << p;
    }
    if (out.isEmpty()) { ProfileInfo d; d.id = defaultId(); d.name = QStringLiteral("Default"); out << d; }
    return out;
}
bool ProfileManager::contains(const QString& id) const { for (const auto& p : profiles()) if (p.id == id) return true; return false; }
ProfileInfo ProfileManager::profile(const QString& id) const { for (const auto& p : profiles()) if (p.id == id) return p; return {}; }
QString ProfileManager::lastUsed() const {
    const QString id = readRegistry().value("last_used").toString();
    return contains(id) ? id : profiles().first().id;
}
bool ProfileManager::showPickerOnStartup() const { return readRegistry().value("show_picker").toBool(true); }
void ProfileManager::setShowPickerOnStartup(bool on) { auto r = readRegistry(); r["show_picker"] = on; writeRegistry(r); }

ProfileInfo ProfileManager::create(const QString& name, const QString& colour, const QString& avatar) {
    ProfileInfo p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = name.trimmed().left(32); p.colour = colour; p.avatar = avatar;
    p.created = QDateTime::currentMSecsSinceEpoch();
    QDir().mkpath(profileDir(p.id));
    // Seed the profile's own settings: its accent is the profile colour (ThemeManager reads "accent").
    QJsonObject s{{"accent", colour}, {"profile_name", p.name}, {"first_run_done", true}};
    { QSaveFile f(profileDir(p.id) + QStringLiteral("/browser_settings.json"));
      if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(s).toJson()); f.commit(); } }
    auto r = readRegistry(); auto arr = r.value("profiles").toArray(); arr.append(p.toJson()); r["profiles"] = arr;
    if (!r.contains("version")) r["version"] = 1;
    writeRegistry(r);
    emit profilesChanged();
    for (const auto& o : profiles()) if (o.id != m_current) notify(o.id, "reload");
    return p;
}

static void setProfileSetting(const QString& dir, const QString& key, const QJsonValue& v) {
    const QString path = dir + QStringLiteral("/browser_settings.json");
    QJsonObject s = readJson(path);
    if (s.value(key) == v) return;
    s[key] = v;
    QSaveFile f(path); if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(s).toJson(QJsonDocument::Indented)); f.commit(); }
}

void ProfileManager::update(const ProfileInfo& p) {
    auto r = readRegistry(); auto arr = r.value("profiles").toArray();
    for (int i = 0; i < arr.size(); ++i) {
        ProfileInfo o = ProfileInfo::fromJson(arr[i].toObject());
        if (o.id != p.id) continue;
        o.name = p.name.trimmed().left(32); o.colour = p.colour; o.avatar = p.avatar;
        arr[i] = o.toJson();
    }
    r["profiles"] = arr; writeRegistry(r);
    // The profile's accent follows its colour (written into that profile's own settings file;
    // AppSettings in the running process picks up external edits).
    setProfileSetting(profileDir(p.id), QStringLiteral("accent"), p.colour);
    setProfileSetting(profileDir(p.id), QStringLiteral("profile_name"), p.name.trimmed().left(32));
    emit profilesChanged();
    if (p.id == m_current) emit currentProfileChanged();
    for (const auto& o : profiles()) if (o.id != m_current) notify(o.id, p.id == o.id ? "reload-self" : "reload");
}

bool ProfileManager::remove(const QString& id, QString* error) {
    if (profiles().size() <= 1) { if (error) *error = QStringLiteral("You can't delete the only profile."); return false; }
    if (id == m_current || isRunning(id)) { if (error) *error = QStringLiteral("Close this profile's windows first."); return false; }
    auto r = readRegistry(); QJsonArray arr;
    for (const auto& v : r.value("profiles").toArray()) if (v.toObject().value("id").toString() != id) arr.append(v);
    r["profiles"] = arr;
    if (r.value("last_used").toString() == id) r["last_used"] = arr.first().toObject().value("id").toString();
    if (!writeRegistry(r)) { if (error) *error = QStringLiteral("Couldn't update profiles.json."); return false; }
    // Chromium may release its files a moment after the last QWebEngineProfile goes away (Windows
    // keeps them locked), so retry briefly; anything left is moved aside and swept on next start.
    const QString dir = profileDir(id);
    for (int i = 0; i < 50 && QFileInfo::exists(dir); ++i) {
        if (QDir(dir).removeRecursively()) break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50); QThread::msleep(100);
    }
    if (QFileInfo::exists(dir)) {
        const QString aside = m_root + QStringLiteral("/profiles/.deleted-") + id + QLatin1Char('-') + QString::number(QDateTime::currentSecsSinceEpoch());
        if (!QDir().rename(dir, aside)) pacitumLog(QStringLiteral("profiles: couldn't fully remove ") + dir + QStringLiteral(" (will retry on next start)"));
    }
    emit profilesChanged();
    for (const auto& o : profiles()) if (o.id != m_current) notify(o.id, "reload");
    return true;
}

// ---------------- per-process profile ----------------
QString ProfileManager::serverName(const QString& id) const {
    const QByteArray h = QCryptographicHash::hash(m_root.toUtf8(), QCryptographicHash::Sha1).toHex().left(10);
    return QStringLiteral("pacitum-%1-%2").arg(QString::fromLatin1(h), id.left(36));
}
bool ProfileManager::isRunning(const QString& id) const {
    if (id == m_current && !m_guest) return true;
    QLocalSocket s; s.connectToServer(serverName(id));
    const bool ok = s.waitForConnected(300);
    if (ok) { s.write("ping\n"); s.waitForBytesWritten(300); s.disconnectFromServer(); }
    return ok;
}
bool ProfileManager::handOff(const QString& id, const QStringList& urls) const {
    QLocalSocket s; s.connectToServer(serverName(id));
    if (!s.waitForConnected(500)) return false;
    QByteArray msg = "open\n";
    for (const auto& u : urls) msg += "url " + u.toUtf8() + '\n';
    msg += "end\n";   // the whole request is in; older instances ignore this line
    s.write(msg); s.waitForBytesWritten(1000);
    // "ok" from the running instance. It may still be starting up (no event loop yet): the
    // request stays queued in the socket and is handled then, so this is not a failure.
    s.waitForReadyRead(3000);
    s.disconnectFromServer();
    return true;
}
void ProfileManager::notify(const QString& id, const QByteArray& cmd) const {
    QLocalSocket s; s.connectToServer(serverName(id));
    if (!s.waitForConnected(200)) return;
    s.write(cmd + '\n'); s.waitForBytesWritten(300); s.disconnectFromServer();
}
void ProfileManager::startServer(const QString& id) {
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(serverName(id))) { QLocalServer::removeServer(serverName(id)); m_server->listen(serverName(id)); }
    connect(m_server, &QLocalServer::newConnection, this, [this]{
        while (auto* c = m_server->nextPendingConnection()) {
            connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
            // The request can arrive in several chunks (long URL lists): only handle it once it is
            // complete ("end" line, or the sender disconnected). It used to be parsed on the first
            // line, so URLs after the first chunk were dropped.
            auto handle = [this, c](bool final){
                QByteArray all = c->property("pacitumBuf").toByteArray() + c->readAll();
                const bool isOpen = all.startsWith("open\n");
                const bool complete = isOpen ? (all.contains("\nend\n") || final) : all.endsWith('\n') || final;
                if (!complete) { c->setProperty("pacitumBuf", all); return; }
                c->setProperty("pacitumBuf", QByteArray());
                if (all.isEmpty()) return;
                QStringList urls; bool open = false;
                for (const QByteArray& line : all.split('\n')) {
                    if (line == "open") open = true;
                    else if (line.startsWith("url ")) urls << QString::fromUtf8(line.mid(4));
                    else if (line == "reload") { m_cacheStamp = -1; emit profilesChanged(); }
                    else if (line == "reload-self") { m_cacheStamp = -1; emit profilesChanged(); emit currentProfileChanged(); }
                }
                if (open) { c->write("ok\n"); c->flush(); emit activateRequested(urls); }
            };
            connect(c, &QLocalSocket::readyRead, c, [handle]{ handle(false); });
            connect(c, &QLocalSocket::disconnected, c, [handle]{ handle(true); });
            if (c->bytesAvailable()) handle(false);
        }
    });
}
void ProfileManager::touchLastUsed(const QString& id) {
    auto r = readRegistry(); r["last_used"] = id;
    auto arr = r.value("profiles").toArray();
    for (int i = 0; i < arr.size(); ++i) { auto o = arr[i].toObject(); if (o.value("id").toString() == id) { o["last_used"] = double(QDateTime::currentMSecsSinceEpoch()); arr[i] = o; } }
    r["profiles"] = arr; writeRegistry(r);
}
bool ProfileManager::activate(const QString& id) {
    const QString dir = profileDir(id);
    QDir().mkpath(dir);
    m_lock = new QLockFile(dir + QStringLiteral("/profile.lock"));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(200)) { delete m_lock; m_lock = nullptr; return false; }
    m_current = id; m_guest = false;
    QDir::setCurrent(dir);
    startServer(id);
    touchLastUsed(id);
    pacitumLog(QStringLiteral("profile %1 (%2) active, dir %3").arg(id, profile(id).name, dir));
    return true;
}
bool ProfileManager::activateGuest() {
    m_guestDir = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/pacitum-guest-XXXXXX"));
    m_guest = true; m_current = QStringLiteral("guest");
    QDir::setCurrent(m_guestDir->path());
    Pacitum::setGuestMode(true);
    pacitumLog(QStringLiteral("guest profile active (off-the-record), scratch dir %1").arg(m_guestDir->path()));
    return true;
}
ProfileInfo ProfileManager::current() const {
    if (m_guest) { ProfileInfo g; g.id = QStringLiteral("guest"); g.name = QStringLiteral("Guest"); g.colour = QStringLiteral("#64748B"); g.avatar = QStringLiteral("guest"); g.isGuest = true; return g; }
    return profile(m_current);
}
void ProfileManager::launch(const QString& id) const {
    if (id == m_current && !m_guest) { emit const_cast<ProfileManager*>(this)->activateRequested({}); return; }
    if (handOff(id, {})) return;
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--profile-directory=") + id});
}
void ProfileManager::launchGuest() const {
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--guest")});
}

// ---------------- avatar ----------------
QPixmap ProfileManager::avatarPixmap(const ProfileInfo& p, int size, qreal dpr, int ring, const QString& gapColour) {
    const int pad = ring > 0 ? ring + 2 : 0;
    const int full = size + pad * 2;
    QPixmap pm(qRound(full * dpr), qRound(full * dpr)); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
    const QColor c(p.colour);
    if (ring > 0) {
        g.setPen(QPen(c, ring)); g.setBrush(gapColour.isEmpty() ? Qt::NoBrush : QBrush(QColor(gapColour)));
        g.drawEllipse(QRectF(ring / 2.0, ring / 2.0, full - ring, full - ring));
    }
    const QRectF r(pad, pad, size, size);
    if (p.isGuest || p.avatar == QStringLiteral("guest")) {
        const auto& pal = ThemeManager::current();
        g.setPen(QPen(QColor(pal.textSecondary), qMax(1.0, size / 30.0))); g.setBrush(QColor(pal.surface));
        g.drawEllipse(r.adjusted(1, 1, -1, -1));
        const int gs = qRound(size * 0.55);
        g.drawPixmap(QPointF(r.center().x() - gs / 2.0, r.center().y() - gs / 2.0),
                     ThemeManager::icon(QStringLiteral("profile-guest"), pal.textSecondary, gs).pixmap(QSize(gs, gs), dpr));
        return pm;
    }
    if (p.avatar == QStringLiteral("photo")) {
        const QImage img = photoFor(p);
        if (!img.isNull()) {
            QPainterPath clip; clip.addEllipse(r);
            g.setClipPath(clip);
            g.setRenderHint(QPainter::SmoothPixmapTransform);
            g.drawImage(r, img);
            return pm;
        }
    }
    QLinearGradient lg(r.topLeft(), r.bottomRight()); lg.setColorAt(0, c.lighter(115)); lg.setColorAt(1, c.darker(125));
    g.setPen(Qt::NoPen); g.setBrush(lg); g.drawEllipse(r);
    if (p.avatar.isEmpty() || p.avatar == QStringLiteral("initial") || p.avatar == QStringLiteral("photo")) {
        QFont f = g.font(); f.setPixelSize(qMax(8, qRound(size * 0.44))); f.setWeight(QFont::DemiBold); g.setFont(f);
        g.setPen(Qt::white);
        g.drawText(r, Qt::AlignCenter, p.name.trimmed().left(1).toUpper());
    } else {
        const int gs = qRound(size * 0.5);
        g.drawPixmap(QPointF(r.center().x() - gs / 2.0, r.center().y() - gs / 2.0),
                     ThemeManager::icon(p.avatar, QStringLiteral("#FFFFFF"), gs).pixmap(QSize(gs, gs), dpr));
    }
    return pm;
}

// ---------------- CI profile test ----------------
namespace {
struct TestHttp : QTcpServer {
    TestHttp() {
        connect(this, &QTcpServer::newConnection, this, [this]{
            while (auto* s = nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [s]{
                    s->readAll();
                    const QByteArray body = "<!doctype html><html><body>pacitum profile test</body></html>";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                             + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    s->flush(); s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
};
QVariant loadAndEval(QWebEngineProfile* prof, const QUrl& url, const QString& js) {
    QWebEnginePage page(prof);
    QEventLoop loop; bool ok = false;
    QObject::connect(&page, &QWebEnginePage::loadFinished, &loop, [&](bool r){ ok = r; loop.quit(); });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    page.load(url); loop.exec();
    if (!ok) return QStringLiteral("<load failed>");
    QVariant out = QStringLiteral("<timeout>");
    page.runJavaScript(js, [&](const QVariant& v){ out = v; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    return out;
}
void writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); f.open(QIODevice::WriteOnly); f.write(data);
}
bool sqliteSeed(const QString& path, const QString& create, const QString& insert) {
    bool ok = false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    { auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("pt_seed"));
      db.setDatabaseName(path);
      if (db.open()) { QSqlQuery q(db); ok = q.exec(create) && q.exec(insert); db.close(); } }
    QSqlDatabase::removeDatabase(QStringLiteral("pt_seed"));
    return ok;
}
QString sqliteScalar(const QString& path, const QString& sql) {
    QString v;
    { auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("pt_read"));
      db.setDatabaseName(path);
      if (db.open()) { QSqlQuery q(db); if (q.exec(sql) && q.next()) v = q.value(0).toString(); db.close(); } }
    QSqlDatabase::removeDatabase(QStringLiteral("pt_read"));
    return v;
}
}

int ProfileManager::runProfileTest() {
    QStringList fails;
    auto check = [&](bool cond, const QString& what) {
        pacitumLog(QStringLiteral("PROFILES %1: %2").arg(cond ? QStringLiteral("ok  ") : QStringLiteral("FAIL"), what));
        if (!cond) fails << what;
    };
    QTemporaryDir tmp; tmp.setAutoRemove(true);
    const QString root = tmp.path() + QStringLiteral("/Pacitum Browser");
    QDir().mkpath(root);

    // 1) Migration of seeded pre-1.6 data, including an interrupted earlier attempt.
    writeFile(root + "/browser_settings.json", R"({"accent":"#F43F5E","profile_name":"Patch","theme":"Dark"})");
    writeFile(root + "/bookmarks.json", R"([{"title":"Pacitum","url":"https://example.org/"}])");
    writeFile(root + "/browser_session.json", R"({"tabs":[{"url":"https://example.org/"}]})");
    check(sqliteSeed(root + "/browser_history.db", "CREATE TABLE history(url TEXT, title TEXT)", "INSERT INTO history VALUES('https://example.org/legacy','Legacy')"), "seed history db");
    check(sqliteSeed(root + "/browser_data/storage/pacitum_downloads.db", "CREATE TABLE downloads(id INTEGER, name TEXT)", "INSERT INTO downloads VALUES(1,'legacy.zip')"), "seed downloads db");
    writeFile(root + "/browser_data/storage/Local Storage/leveldb/000003.log", QByteArray(4096, 'x'));
    writeFile(root + "/browser_data/cache/Cache_Data/data_0", QByteArray(1024, 'c'));
    writeFile(root + "/startup.log", "log");
    writeFile(root + "/profiles/default.migrating/browser_settings.json", "{half-written");   // interrupted run
    const QByteArray settingsBefore = fileHash(root + "/browser_settings.json");

    QString log;
    check(migrateLegacy(root, &log), "migration succeeds: " + log.trimmed());
    const QString def = root + "/profiles/default";
    check(fileHash(def + "/browser_settings.json") == settingsBefore, "settings migrated byte-for-byte");
    check(QFile::exists(def + "/bookmarks.json") && QFile::exists(def + "/browser_session.json"), "bookmarks + session migrated");
    check(sqliteScalar(def + "/browser_history.db", "SELECT title FROM history") == "Legacy", "history row readable after migration");
    check(sqliteScalar(def + "/browser_data/storage/pacitum_downloads.db", "SELECT name FROM downloads") == "legacy.zip", "downloads history readable after migration");
    check(QFileInfo(def + "/browser_data/storage/Local Storage/leveldb/000003.log").size() == 4096, "web storage migrated");
    check(!QFile::exists(def + "/browser_data/cache/Cache_Data/data_0") && !QFile::exists(def + "/startup.log"), "cache and logs not copied");
    check(!QFileInfo::exists(root + "/profiles/default.migrating"), "interrupted staging folder cleaned up");
    check(QFile::exists(root + "/browser_settings.json") && QFile::exists(root + "/browser_history.db"), "legacy originals kept as backup");
    const QJsonObject reg = readJson(root + "/profiles.json");
    const auto first = reg.value("profiles").toArray().first().toObject();
    check(first.value("id").toString() == "default" && first.value("name").toString() == "Patch" && first.value("colour").toString() == "#F43F5E",
          "registry has Default profile with legacy name + accent");
    const QByteArray regBefore = fileHash(root + "/profiles.json");
    writeFile(root + "/browser_settings.json", R"({"accent":"#22C55E"})");   // later edit of the backup must not re-migrate
    check(migrateLegacy(root) && fileHash(root + "/profiles.json") == regBefore && fileHash(def + "/browser_settings.json") == settingsBefore,
          "second run is a no-op (idempotent)");

    // 2) Isolation: two persistent profiles + Guest, each with its own storage.
    auto* pm = instance();
    pm->init(root);
    const ProfileInfo work = pm->create(QStringLiteral("Work"), QStringLiteral("#6366F1"), QStringLiteral("initial"));
    check(pm->profiles().size() == 2 && pm->shouldShowPicker(), "two profiles registered, picker would show");
    check(readJson(pm->profileDir(work.id) + "/browser_settings.json").value("accent").toString() == "#6366F1", "new profile accent seeded from its colour");
    auto makeProfile = [&](const QString& id) {
        auto* p = new QWebEngineProfile(QStringLiteral("PacitumTest-") + id.left(8));
        p->setPersistentStoragePath(pm->profileDir(id) + "/browser_data/storage");
        p->setCachePath(pm->profileDir(id) + "/browser_data/cache");
        p->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
        Pacitum::configureProfile(p, true);
        return p;
    };
    QWebEngineProfile* a = makeProfile(defaultId());
    QWebEngineProfile* b = makeProfile(work.id);
    auto* guest = new QWebEngineProfile();   // off-the-record
    check(!a->isOffTheRecord() && !b->isOffTheRecord() && guest->isOffTheRecord(), "guest is off-the-record, profiles are persistent");
    check(a->persistentStoragePath() != b->persistentStoragePath() && a->persistentStoragePath().startsWith(def), "separate storage paths");

    TestHttp http;
    check(http.listen(QHostAddress::LocalHost, 0), "local test server");
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/profile-test").arg(http.serverPort()));
    a->cookieStore()->setCookie(QNetworkCookie("pacitum_native", "A"), url);
    const QString setJs = QStringLiteral("localStorage.setItem('pacitum_ls','A'); document.cookie='pacitum_js=A; path=/; max-age=3600';"
                                         "JSON.stringify([localStorage.getItem('pacitum_ls'), document.cookie])");
    const QString getJs = QStringLiteral("JSON.stringify([localStorage.getItem('pacitum_ls'), document.cookie])");
    const QString inA = loadAndEval(a, url, setJs).toString();
    pacitumLog("PROFILES profile A sees " + inA);
    check(inA.contains("\"A\"") && inA.contains("pacitum_js=A"), "value set in profile A");
    const QString inA2 = loadAndEval(a, url, getJs).toString();
    check(inA2.contains("\"A\"") && inA2.contains("pacitum_js=A") && inA2.contains("pacitum_native=A"), "profile A keeps its cookie + localStorage");
    const QString inB = loadAndEval(b, url, getJs).toString();
    pacitumLog("PROFILES profile B sees " + inB);
    check(inB.startsWith("[null") && !inB.contains("pacitum_"), "absent in the other profile");
    const QString inG = loadAndEval(guest, url, getJs).toString();
    pacitumLog("PROFILES guest sees " + inG);
    check(inG.startsWith("[null") && !inG.contains("pacitum_"), "absent in Guest");

    delete guest; delete b; delete a;
    // 3) Delete the second profile (not running).
    QString err;
    check(pm->remove(work.id, &err) && !QFileInfo::exists(pm->profileDir(work.id)) && pm->profiles().size() == 1, "delete profile " + err);
    const bool ok = fails.isEmpty();
    const QString res = ok ? QStringLiteral("PROFILES OK") : QStringLiteral("PROFILES FAIL: ") + fails.join(QStringLiteral(" | "));
    pacitumLog(res); std::cout << res.toStdString() << std::endl;
    return ok ? 0 : 5;
}
