#include "SyncManager.h"
#include "AppSettings.h"
#include "BookmarkManager.h"
#include "HistoryManager.h"
#include "Passwords.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

#ifndef PACITUM_GOOGLE_CLIENT_ID
#define PACITUM_GOOGLE_CLIENT_ID ""
#endif
#ifndef PACITUM_GOOGLE_CLIENT_SECRET
#define PACITUM_GOOGLE_CLIENT_SECRET ""
#endif

namespace {
const char* kScopes = "openid email profile https://www.googleapis.com/auth/drive.appdata";
const char* kFileName = "pacitum-sync.json";
constexpr int kHistoryKeep = 500;
// Settings that mean the same thing on every computer (paths and update feeds stay local).
const QStringList kSettingKeys = {
    QStringLiteral("accent"), QStringLiteral("adblock_enabled"), QStringLiteral("animations"), QStringLiteral("ask_download_location"),
    QStringLiteral("clear_on_exit"), QStringLiteral("dark_mode"), QStringLiteral("default_zoom"), QStringLiteral("do_not_track"),
    QStringLiteral("freeze_background_tabs"), QStringLiteral("homepage"), QStringLiteral("pin_downloads_button"),
    QStringLiteral("restore_session"), QStringLiteral("search_engine"), QStringLiteral("show_bookmarks_bar"),
    QStringLiteral("show_suggestions"), QStringLiteral("search_suggest_online"), QStringLiteral("bookmark_folders"), QStringLiteral("startup"), QStringLiteral("theme")};
const QStringList kKinds = {QStringLiteral("bookmarks"), QStringLiteral("settings"), QStringLiteral("notes"), QStringLiteral("history")};

QString here(const QString& f) { return QDir::current().absoluteFilePath(f); }
QJsonObject readJsonFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
bool writeJsonFile(const QString& path, const QJsonObject& o) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.commit();
}
QByteArray b64url(const QByteArray& b) { return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }
QByteArray randomToken(int bytes) {
    QByteArray r(bytes, Qt::Uninitialized);
    for (int i = 0; i < bytes; ++i) r[i] = char(QRandomGenerator::system()->bounded(256));
    return b64url(r);
}

// Keyed list merge: items in base but missing on one side were deleted there.
QJsonArray mergeList(const QJsonArray& base, const QJsonArray& local, const QJsonArray& remote, const QString& key, bool followDeletes) {
    QHash<QString, QJsonObject> b, r;
    for (const auto& v : base) b.insert(v.toObject().value(key).toString(), v.toObject());
    for (const auto& v : remote) r.insert(v.toObject().value(key).toString(), v.toObject());
    // 2.0.8 safety: an empty other side (a reset Drive file, a different account) or a change that would remove
    // most of this computer's items is not taken as "deleted there". Sync used to empty the bookmarks then.
    if (followDeletes) {
        int gone = 0;
        for (const auto& v : local) { const QString k = v.toObject().value(key).toString(); if (!r.contains(k) && b.contains(k)) ++gone; }
        if (remote.isEmpty() || (gone > 10 && gone * 2 > local.size())) followDeletes = false;
    }
    QSet<QString> seen;
    QJsonArray out;
    for (const auto& v : local) {
        const QJsonObject l = v.toObject(); const QString k = l.value(key).toString();
        if (k.isEmpty() || seen.contains(k)) continue;
        if (followDeletes && !r.contains(k) && b.contains(k)) continue;          // deleted on the other computer
        QJsonObject item = l;
        if (r.contains(k) && b.value(k) == l && r.value(k) != l) item = r.value(k);   // only the other side edited it
        out.append(item); seen.insert(k);
    }
    for (const auto& v : remote) {
        const QJsonObject ro = v.toObject(); const QString k = ro.value(key).toString();
        if (k.isEmpty() || seen.contains(k)) continue;
        if (followDeletes && b.contains(k)) continue;                             // deleted on this computer
        out.append(ro); seen.insert(k);
    }
    return out;
}
}

SyncManager* SyncManager::instance() {
    static auto* s = new SyncManager(QCoreApplication::instance());
    return s;
}

SyncManager::SyncManager(QObject* parent) : QObject(parent) {
    m_net = new QNetworkAccessManager(this);
    loadAccount();
    m_timer.setInterval(15 * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &SyncManager::syncNow);
    if (signedIn()) { m_timer.start(); QTimer::singleShot(4000, this, &SyncManager::syncNow); }
}

QString SyncManager::clientId() const {
    QString id = AppSettings::get(QStringLiteral("sync_client_id")).toString().trimmed();
    if (id.isEmpty()) id = qEnvironmentVariable("PACITUM_GOOGLE_CLIENT_ID");
    if (id.isEmpty()) id = QString::fromLatin1(PACITUM_GOOGLE_CLIENT_ID);
    return id;
}
static QString clientSecret() {
    QString s = AppSettings::get(QStringLiteral("sync_client_secret")).toString().trimmed();
    if (s.isEmpty()) s = qEnvironmentVariable("PACITUM_GOOGLE_CLIENT_SECRET");
    if (s.isEmpty()) s = QString::fromLatin1(PACITUM_GOOGLE_CLIENT_SECRET);
    return s;
}
bool SyncManager::hasClient() const { return !clientId().isEmpty(); }
void SyncManager::setClient(const QString& id, const QString& secret) {
    AppSettings::set(QStringLiteral("sync_client_id"), id.trimmed());
    AppSettings::set(QStringLiteral("sync_client_secret"), secret.trimmed());
    emit changed();
}

bool SyncManager::syncs(const QString& kind) const { return m_choices.value(kind).toBool(true); }
void SyncManager::setSyncs(const QString& kind, bool on) { m_choices.insert(kind, on); saveAccount(); emit changed(); }

void SyncManager::loadAccount() {
    const QJsonObject o = readJsonFile(here(QStringLiteral("sync-account.json")));
    // 2.0.8: the token is kept encrypted (Vault, as saved passwords). A plain one from older versions is read once.
    bool opened = false;
    const QByteArray sealedTok = Vault::open(QByteArray::fromBase64(o.value(QStringLiteral("refresh_token_sealed")).toString().toLatin1()), &opened);
    m_refreshToken = opened ? QString::fromUtf8(sealedTok) : o.value(QStringLiteral("refresh_token")).toString();
    m_account = {o.value(QStringLiteral("email")).toString(), o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("picture")).toString()};
    m_choices = o.value(QStringLiteral("choices")).toObject();
    const qint64 last = qint64(o.value(QStringLiteral("last_sync")).toDouble());
    m_lastSync = last > 0 ? QDateTime::fromMSecsSinceEpoch(last) : QDateTime();
}
void SyncManager::saveAccount() const {
    const QString path = here(QStringLiteral("sync-account.json"));
    if (m_refreshToken.isEmpty()) { QFile::remove(path); return; }
    QJsonObject o{
        {QStringLiteral("email"), m_account.email},
        {QStringLiteral("name"), m_account.name}, {QStringLiteral("picture"), m_account.picture},
        {QStringLiteral("choices"), m_choices}, {QStringLiteral("last_sync"), double(m_lastSync.isValid() ? m_lastSync.toMSecsSinceEpoch() : 0)}};
    const QByteArray sealed = Vault::seal(m_refreshToken.toUtf8());
    if (!sealed.isEmpty()) o.insert(QStringLiteral("refresh_token_sealed"), QString::fromLatin1(sealed.toBase64()));
    else o.insert(QStringLiteral("refresh_token"), m_refreshToken);   // no vault on this system: as before
    writeJsonFile(path, o);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);   // the token is a credential
}
void SyncManager::setStatus(const QString& s) { m_status = s; emit changed(); }

// ------------------------------------------------------------------ sign-in (OAuth 2.0, loopback + PKCE)
void SyncManager::signIn() {
    if (!hasClient()) { setStatus(tr("Add a Google OAuth client ID first (Settings › Sync › Advanced).")); return; }
    if (m_loopback) m_loopback->deleteLater();
    auto* server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, 0)) { server->deleteLater(); setStatus(tr("Couldn't start the sign-in listener.")); return; }
    m_loopback = server;
    const QString redirect = QStringLiteral("http://127.0.0.1:%1").arg(server->serverPort());
    const QByteArray verifier = randomToken(48);
    const QByteArray challenge = b64url(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256));
    const QByteArray state = randomToken(16);
    QUrl url(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"), clientId());
    q.addQueryItem(QStringLiteral("redirect_uri"), redirect);
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("scope"), QString::fromLatin1(kScopes));
    q.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("state"), QString::fromLatin1(state));
    q.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
    q.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
    url.setQuery(q);
    connect(server, &QTcpServer::newConnection, this, [this, server, redirect, verifier, state] {
        while (QTcpSocket* s = server->nextPendingConnection()) {
            connect(s, &QTcpSocket::readyRead, s, [this, s, server, redirect, verifier, state] {
                const QByteArray head = s->readAll();
                const QList<QByteArray> parts = head.split(' ');
                const QUrl got(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(parts.value(1)));
                const QUrlQuery gq(got);
                const QString code = gq.queryItemValue(QStringLiteral("code"));
                const bool ok = !code.isEmpty() && gq.queryItemValue(QStringLiteral("state")) == QString::fromLatin1(state);
                const QByteArray page = ok
                    ? QByteArray("<!doctype html><meta charset=utf-8><title>Pacitum</title><body style=\"font:16px system-ui;padding:48px\"><h2>Signed in to Pacitum</h2><p>You can close this tab and go back to Pacitum.</p>")
                    : QByteArray("<!doctype html><meta charset=utf-8><title>Pacitum</title><body style=\"font:16px system-ui;padding:48px\"><h2>Sign-in didn't finish</h2><p>Go back to Pacitum and try again.</p>");
                s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " + QByteArray::number(page.size()) + "\r\n\r\n" + page);
                s->disconnectFromHost();
                if (parts.value(1).startsWith("/favicon")) return;
                server->close(); server->deleteLater();
                if (ok) exchangeCode(code, redirect, QString::fromLatin1(verifier));
                else setStatus(tr("Sign-in was cancelled."));
            });
        }
    });
    QTimer::singleShot(5 * 60 * 1000, server, [this, server]{ server->close(); server->deleteLater(); if (!signedIn()) setStatus(tr("Sign-in timed out.")); });
    setStatus(tr("Continue in your browser…"));
    QDesktopServices::openUrl(url);
}

void SyncManager::exchangeCode(const QString& code, const QString& redirect, const QString& verifier) {
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("code"), code);
    body.addQueryItem(QStringLiteral("client_id"), clientId());
    body.addQueryItem(QStringLiteral("client_secret"), clientSecret());
    body.addQueryItem(QStringLiteral("redirect_uri"), redirect);
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    body.addQueryItem(QStringLiteral("code_verifier"), verifier);
    QNetworkRequest r(QUrl(QStringLiteral("https://oauth2.googleapis.com/token")));
    r.setTransferTimeout(30000);
    r.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/x-www-form-urlencoded"));
    QNetworkReply* rep = m_net->post(r, body.query(QUrl::FullyEncoded).toUtf8());
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        if (o.value(QStringLiteral("refresh_token")).toString().isEmpty()) {
            setStatus(tr("Google didn't accept the sign-in: %1").arg(o.value(QStringLiteral("error_description")).toString(o.value(QStringLiteral("error")).toString(rep->errorString()))));
            return;
        }
        m_refreshToken = o.value(QStringLiteral("refresh_token")).toString();
        m_accessToken = o.value(QStringLiteral("access_token")).toString();
        m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(o.value(QStringLiteral("expires_in")).toInt(3600) - 60);
        saveAccount();
        m_timer.start();
        fetchAccount();
        syncNow();
    });
}

void SyncManager::withToken(std::function<void(bool ok)> next) {
    if (!m_accessToken.isEmpty() && QDateTime::currentDateTimeUtc() < m_tokenExpiry) { next(true); return; }
    if (m_refreshToken.isEmpty()) { next(false); return; }
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), clientId());
    body.addQueryItem(QStringLiteral("client_secret"), clientSecret());
    body.addQueryItem(QStringLiteral("refresh_token"), m_refreshToken);
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    QNetworkRequest r(QUrl(QStringLiteral("https://oauth2.googleapis.com/token")));
    r.setTransferTimeout(30000);
    r.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/x-www-form-urlencoded"));
    QNetworkReply* rep = m_net->post(r, body.query(QUrl::FullyEncoded).toUtf8());
    connect(rep, &QNetworkReply::finished, this, [this, rep, next] {
        rep->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        m_accessToken = o.value(QStringLiteral("access_token")).toString();
        if (m_accessToken.isEmpty()) {
            if (o.value(QStringLiteral("error")).toString() == QLatin1String("invalid_grant")) {   // revoked / expired: sign in again
                m_refreshToken.clear(); saveAccount(); m_timer.stop();
            }
            next(false); return;
        }
        m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(o.value(QStringLiteral("expires_in")).toInt(3600) - 60);
        next(true);
    });
}

QNetworkReply* SyncManager::api(const QByteArray& verb, const QUrl& url, const QByteArray& body, const QByteArray& type) {
    QNetworkRequest r(url);
    r.setTransferTimeout(30000);   // 2.0.8: a stalled request used to leave sync "busy" until restart
    r.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    if (!type.isEmpty()) r.setHeader(QNetworkRequest::ContentTypeHeader, type);
    return m_net->sendCustomRequest(r, verb, body);
}

void SyncManager::fetchAccount() {
    withToken([this](bool ok) {
        if (!ok) return;
        QNetworkReply* rep = api("GET", QUrl(QStringLiteral("https://openidconnect.googleapis.com/v1/userinfo")));
        connect(rep, &QNetworkReply::finished, this, [this, rep] {
            rep->deleteLater();
            const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
            if (o.isEmpty()) return;
            m_account = {o.value(QStringLiteral("email")).toString(), o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("picture")).toString()};
            saveAccount(); emit changed();
        });
    });
}

void SyncManager::fetchPhoto(std::function<void(const QImage&)> done) {
    if (m_account.picture.isEmpty()) { done(QImage()); return; }
    QUrl u(m_account.picture);
    QString s = u.toString(); if (s.contains(QStringLiteral("=s96-c"))) s.replace(QStringLiteral("=s96-c"), QStringLiteral("=s256-c"));
    QNetworkReply* rep = m_net->get(QNetworkRequest(QUrl(s)));
    connect(rep, &QNetworkReply::finished, this, [rep, done] { rep->deleteLater(); QImage img; img.loadFromData(rep->readAll()); done(img); });
}

void SyncManager::signOut() {
    if (!m_refreshToken.isEmpty()) {   // best effort: revoke at Google too
        QNetworkRequest r(QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke")));
        r.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/x-www-form-urlencoded"));
        QNetworkReply* rep = m_net->post(r, "token=" + QUrl::toPercentEncoding(m_refreshToken));
        connect(rep, &QNetworkReply::finished, rep, &QObject::deleteLater);
    }
    m_refreshToken.clear(); m_accessToken.clear(); m_account = {}; m_lastSync = {};
    m_timer.stop();
    saveAccount();
    QFile::remove(here(QStringLiteral("sync-base.json")));   // a later sign-in starts a fresh merge
    setStatus(tr("Signed out. Your data stays on this computer and in your Google Drive."));
}

// ------------------------------------------------------------------ the sync itself
QJsonObject SyncManager::collectLocal() {
    emit aboutToCollect();
    QJsonObject o;
    if (syncs(QStringLiteral("bookmarks"))) {
        BookmarkManager bm(here(QStringLiteral("bookmarks.json"))); bm.load();
        QJsonArray a;
        for (const Bookmark& b : bm.allBookmarks()) {
            QJsonObject e{{QStringLiteral("title"), b.title}, {QStringLiteral("url"), b.url}, {QStringLiteral("added"), b.added}};
            if (!b.folder.isEmpty()) e.insert(QStringLiteral("folder"), b.folder);
            a.append(e);
        }
        o.insert(QStringLiteral("bookmarks"), a);
    }
    if (syncs(QStringLiteral("settings"))) {
        const QJsonObject all = AppSettings::read(); QJsonObject s;
        for (const QString& k : kSettingKeys) if (all.contains(k)) s.insert(k, all.value(k));
        o.insert(QStringLiteral("settings"), s);
    }
    if (syncs(QStringLiteral("notes"))) {
        QFile f(here(QStringLiteral("notes.txt")));
        o.insert(QStringLiteral("notes"), f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString());
    }
    if (syncs(QStringLiteral("history")) && m_history) {
        m_history->flush();
        QJsonArray a;
        for (const HistoryEntry& h : m_history->recent(kHistoryKeep))
            a.append(QJsonObject{{QStringLiteral("url"), h.url}, {QStringLiteral("title"), h.title}, {QStringLiteral("t"), h.visitTime}});
        o.insert(QStringLiteral("history"), a);
    }
    return o;
}

QJsonObject SyncManager::merge(const QJsonObject& base, const QJsonObject& local, const QJsonObject& remote, const QString& remoteDevice) {
    QJsonObject out = remote;   // kinds this computer doesn't sync pass through untouched
    if (local.contains(QStringLiteral("bookmarks")))
        out.insert(QStringLiteral("bookmarks"), mergeList(base.value(QStringLiteral("bookmarks")).toArray(), local.value(QStringLiteral("bookmarks")).toArray(),
                                                          remote.value(QStringLiteral("bookmarks")).toArray(), QStringLiteral("url"), true));
    if (local.contains(QStringLiteral("settings"))) {
        const QJsonObject b = base.value(QStringLiteral("settings")).toObject(), l = local.value(QStringLiteral("settings")).toObject(),
                          r = remote.value(QStringLiteral("settings")).toObject();
        QJsonObject s = r;
        const bool firstSync = !base.contains(QStringLiteral("settings"));   // a fresh computer must not overwrite Drive with defaults
        for (auto it = l.begin(); it != l.end(); ++it)
            if (!r.contains(it.key()) || (!firstSync && b.value(it.key()) != it.value())) s.insert(it.key(), it.value());   // changed here: ours wins
        out.insert(QStringLiteral("settings"), s);
    }
    if (local.contains(QStringLiteral("notes"))) {
        const QString b = base.value(QStringLiteral("notes")).toString(), l = local.value(QStringLiteral("notes")).toString();
        const QString r = remote.value(QStringLiteral("notes")).toString();
        QString n;
        if (!remote.contains(QStringLiteral("notes")) || r == l || r == b) n = l;    // only this side changed (or nothing)
        else if (l == b) n = r;                                                       // only the other side changed
        else n = l + QStringLiteral("\n\n— Notes from %1 —\n").arg(remoteDevice.isEmpty() ? QStringLiteral("another computer") : remoteDevice) + r;   // both: keep both
        out.insert(QStringLiteral("notes"), n);
    }
    if (local.contains(QStringLiteral("history"))) {
        QJsonArray h = mergeList({}, local.value(QStringLiteral("history")).toArray(), remote.value(QStringLiteral("history")).toArray(), QStringLiteral("url"), false);
        QList<QJsonObject> items; for (const auto& v : h) items << v.toObject();
        std::sort(items.begin(), items.end(), [](const QJsonObject& a, const QJsonObject& b){ return a.value(QStringLiteral("t")).toDouble() > b.value(QStringLiteral("t")).toDouble(); });
        QJsonArray kept; for (int i = 0; i < items.size() && i < kHistoryKeep; ++i) kept.append(items[i]);
        out.insert(QStringLiteral("history"), kept);
    }
    return out;
}

QStringList SyncManager::applyLocal(const QJsonObject& merged, const QJsonObject& local) {
    QStringList changedKinds;
    if (local.contains(QStringLiteral("bookmarks")) && merged.value(QStringLiteral("bookmarks")) != local.value(QStringLiteral("bookmarks"))) {
        // rewrite bookmarks.json through BookmarkManager so the file format stays its own
        BookmarkManager bm(here(QStringLiteral("bookmarks.json"))); bm.load();
        QSet<QString> want;
        for (const auto& v : merged.value(QStringLiteral("bookmarks")).toArray()) want.insert(v.toObject().value(QStringLiteral("url")).toString());
        for (const Bookmark& b : bm.allBookmarks()) if (!want.contains(b.url)) bm.removeBookmark(b.url);
        QVector<QPair<QString, QString>> add;
        for (const auto& v : merged.value(QStringLiteral("bookmarks")).toArray()) {
            const QJsonObject o = v.toObject();
            if (!bm.isBookmarked(o.value(QStringLiteral("url")).toString())) add.append({o.value(QStringLiteral("title")).toString(), o.value(QStringLiteral("url")).toString()});
        }
        if (!add.isEmpty()) bm.addBookmarks(add);
        for (const auto& v : merged.value(QStringLiteral("bookmarks")).toArray()) {   // folders and titles from the merge
            const QJsonObject o = v.toObject();
            const QString url = o.value(QStringLiteral("url")).toString();
            bm.moveToFolder(url, o.value(QStringLiteral("folder")).toString());
            if (!o.value(QStringLiteral("title")).toString().isEmpty()) bm.setTitle(url, o.value(QStringLiteral("title")).toString());
        }
        bm.save();
        changedKinds << QStringLiteral("bookmarks");
    }
    if (local.contains(QStringLiteral("settings")) && merged.value(QStringLiteral("settings")) != local.value(QStringLiteral("settings"))) {
        QJsonObject all = AppSettings::read();
        const QJsonObject s = merged.value(QStringLiteral("settings")).toObject();
        for (auto it = s.begin(); it != s.end(); ++it) if (kSettingKeys.contains(it.key())) all.insert(it.key(), it.value());
        AppSettings::write(all);
        changedKinds << QStringLiteral("settings");
    }
    if (local.contains(QStringLiteral("notes")) && merged.value(QStringLiteral("notes")) != local.value(QStringLiteral("notes"))) {
        QSaveFile f(here(QStringLiteral("notes.txt")));
        if (f.open(QIODevice::WriteOnly)) { f.write(merged.value(QStringLiteral("notes")).toString().toUtf8()); f.commit(); }
        changedKinds << QStringLiteral("notes");
    }
    if (local.contains(QStringLiteral("history")) && m_history) {
        QSet<QString> have;
        for (const auto& v : local.value(QStringLiteral("history")).toArray()) have.insert(v.toObject().value(QStringLiteral("url")).toString());
        QVector<QPair<QString, QString>> add;
        for (const auto& v : merged.value(QStringLiteral("history")).toArray()) {
            const QJsonObject o = v.toObject();
            if (!have.contains(o.value(QStringLiteral("url")).toString())) add.append({o.value(QStringLiteral("title")).toString(), o.value(QStringLiteral("url")).toString()});
        }
        if (!add.isEmpty()) { m_history->importVisits(add); changedKinds << QStringLiteral("history"); }
    }
    return changedKinds;
}

void SyncManager::finish(bool ok, const QString& message) {
    m_busy = false;
    if (ok) { m_lastSync = QDateTime::currentDateTime(); saveAccount(); }
    setStatus(message);
}

void SyncManager::syncNow() {
    if (m_busy || !signedIn()) return;
    m_busy = true;
    setStatus(tr("Syncing…"));
    withToken([this](bool ok) {
        if (!ok) { finish(false, signedIn() ? tr("Couldn't reach Google. Will try again later.") : tr("Sign in again to keep syncing.")); return; }
        QUrl list(QStringLiteral("https://www.googleapis.com/drive/v3/files"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("spaces"), QStringLiteral("appDataFolder"));
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("name='%1'").arg(QLatin1String(kFileName)));
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("files(id,modifiedTime)"));
        list.setQuery(q);
        QNetworkReply* rep = api("GET", list);
        connect(rep, &QNetworkReply::finished, this, [this, rep] {
            rep->deleteLater();
            if (rep->error() != QNetworkReply::NoError) { finish(false, tr("Google Drive error: %1").arg(rep->errorString())); return; }
            const QString fileId = QJsonDocument::fromJson(rep->readAll()).object().value(QStringLiteral("files")).toArray().first().toObject().value(QStringLiteral("id")).toString();
            auto proceed = [this, fileId](const QJsonObject& remote) {
                const QJsonObject local = collectLocal();
                QJsonObject base = readJsonFile(here(QStringLiteral("sync-base.json")));
                // 2.0.8: the last merge was with another Google account (or Drive has no file any more): start fresh.
                const QString baseAccount = base.value(QStringLiteral("account")).toString();
                if (remote.isEmpty() || (!baseAccount.isEmpty() && !m_account.email.isEmpty() && baseAccount != m_account.email)) base = {};
                QJsonObject merged = merge(base, local, remote, remote.value(QStringLiteral("device")).toString());
                const QStringList changedKinds = applyLocal(merged, local);
                if (!changedKinds.isEmpty()) emit applied(changedKinds);
                merged.insert(QStringLiteral("format"), 1);
                merged.insert(QStringLiteral("device"), QHostInfo::localHostName());
                merged.insert(QStringLiteral("updated"), double(QDateTime::currentMSecsSinceEpoch()));
                const QByteArray body = QJsonDocument(merged).toJson(QJsonDocument::Compact);
                QNetworkReply* up;
                if (fileId.isEmpty()) {   // first sync: create the file in appDataFolder (multipart: metadata + content)
                    const QByteArray boundary = "pacitum" + randomToken(12);
                    QByteArray mp;
                    mp += "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n";
                    mp += QJsonDocument(QJsonObject{{QStringLiteral("name"), QLatin1String(kFileName)}, {QStringLiteral("parents"), QJsonArray{QStringLiteral("appDataFolder")}}}).toJson(QJsonDocument::Compact);
                    mp += "\r\n--" + boundary + "\r\nContent-Type: application/json\r\n\r\n" + body + "\r\n--" + boundary + "--\r\n";
                    up = api("POST", QUrl(QStringLiteral("https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart")), mp, "multipart/related; boundary=" + boundary);
                } else {
                    up = api("PATCH", QUrl(QStringLiteral("https://www.googleapis.com/upload/drive/v3/files/%1?uploadType=media").arg(fileId)), body, "application/json");
                }
                connect(up, &QNetworkReply::finished, this, [this, up, merged, changedKinds] {
                    up->deleteLater();
                    if (up->error() != QNetworkReply::NoError) { finish(false, tr("Couldn't save to Google Drive: %1").arg(up->errorString())); return; }
                    QJsonObject baseOut = merged;
                    baseOut.insert(QStringLiteral("account"), m_account.email);
                    writeJsonFile(here(QStringLiteral("sync-base.json")), baseOut);
                    finish(true, changedKinds.isEmpty() ? tr("Up to date.") : tr("Synced: %1 updated.").arg(changedKinds.join(QStringLiteral(", "))));
                });
            };
            if (fileId.isEmpty()) { proceed({}); return; }
            QNetworkReply* dl = api("GET", QUrl(QStringLiteral("https://www.googleapis.com/drive/v3/files/%1?alt=media").arg(fileId)));
            connect(dl, &QNetworkReply::finished, this, [this, dl, proceed] {
                dl->deleteLater();
                if (dl->error() != QNetworkReply::NoError) { finish(false, tr("Couldn't read from Google Drive: %1").arg(dl->errorString())); return; }
                proceed(QJsonDocument::fromJson(dl->readAll()).object());
            });
        });
    });
}
