#include <QSet>
#include "BookmarkManager.h"
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include "AppSettings.h"

BookmarkManager::BookmarkManager(const QString& file) : m_file(file) { load(); }

bool BookmarkManager::load() {
    QFile f(m_file);
    if (!f.exists()) { m_bookmarks.clear(); return true; }
    if (!f.open(QIODevice::ReadOnly)) return false;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) {
        // 2.0.8: keep a damaged file aside instead of overwriting it with the next bookmark added.
        f.close();
        QFile::rename(m_file, m_file + QStringLiteral(".damaged-") + QString::number(QDateTime::currentSecsSinceEpoch()));
        m_bookmarks.clear();
        return false;
    }
    m_bookmarks.clear();
    for (auto v : doc.array()) {
        auto o = v.toObject();
        m_bookmarks.push_back({ o["title"].toString(), o["url"].toString(), o["added"].toString(), o["folder"].toString().trimmed() });
    }
    return true;
}

bool BookmarkManager::save() const {
    QJsonArray arr;
    for (auto &b : m_bookmarks) {
        QJsonObject o{{"title", b.title},{"url", b.url},{"added", b.added}};
        if (!b.folder.isEmpty()) o.insert("folder", b.folder);   // older versions just ignore it
        arr.append(o);
    }
    QSaveFile f(m_file);   // atomic: a crash while saving can't leave a half-written bookmarks file
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return f.commit();
}

int BookmarkManager::addBookmarks(const QVector<QPair<QString, QString>>& items) {
    QSet<QString> have; for (const auto& b : std::as_const(m_bookmarks)) have.insert(b.url);
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    int n = 0;
    for (const auto& it : items) {
        if (it.second.isEmpty() || have.contains(it.second)) continue;
        have.insert(it.second); m_bookmarks.push_back({it.first.isEmpty() ? it.second : it.first, it.second, now}); ++n;
    }
    if (n) save();
    return n;
}

bool BookmarkManager::addBookmark(const QString& title, const QString& url, const QString& folder) {
    if (isBookmarked(url)) return false;
    if (!folder.isEmpty()) addFolder(folder);
    m_bookmarks.push_back({title, url, QDateTime::currentDateTime().toString(Qt::ISODate), folder});
    save();
    return true;
}

void BookmarkManager::removeBookmark(const QString& url) {
    m_bookmarks.erase(std::remove_if(m_bookmarks.begin(), m_bookmarks.end(),
        [&](const Bookmark& b){ return b.url == url; }), m_bookmarks.end());
    save();
}

bool BookmarkManager::isBookmarked(const QString& url) const {
    for (auto &b : m_bookmarks) if (b.url == url) return true;
    return false;
}

QVector<Bookmark> BookmarkManager::search(const QString& q) const {
    QVector<Bookmark> r;
    QString lq = q.toLower();
    for (auto &b : m_bookmarks) if (b.title.toLower().contains(lq) || b.url.toLower().contains(lq)) r.push_back(b);
    return r;
}

// ------------------------------------------------------------------ folders
QStringList BookmarkManager::folders() const {
    QStringList out;
    for (const auto& v : AppSettings::get(QStringLiteral("bookmark_folders")).toArray()) {
        const QString n = v.toString().trimmed();
        if (!n.isEmpty() && !out.contains(n)) out << n;
    }
    for (const auto& b : m_bookmarks) if (!b.folder.isEmpty() && !out.contains(b.folder)) out << b.folder;   // e.g. arrived by sync
    return out;
}

void BookmarkManager::setFolderOrder(const QStringList& order) {
    QJsonArray a; for (const QString& n : order) if (!n.trimmed().isEmpty()) a.append(n.trimmed());
    AppSettings::set(QStringLiteral("bookmark_folders"), a);
}

QVector<Bookmark> BookmarkManager::bookmarksIn(const QString& folder) const {
    QVector<Bookmark> r;
    for (const auto& b : m_bookmarks) if (b.folder == folder) r.push_back(b);
    return r;
}

bool BookmarkManager::addFolder(const QString& raw) {
    const QString name = raw.trimmed().left(60);
    if (name.isEmpty()) return false;
    QStringList f = folders();
    if (f.contains(name)) return false;
    f << name; setFolderOrder(f);
    return true;
}

bool BookmarkManager::renameFolder(const QString& from, const QString& raw) {
    const QString to = raw.trimmed().left(60);
    QStringList f = folders();
    if (to.isEmpty() || !f.contains(from) || (to != from && f.contains(to))) return false;
    f[f.indexOf(from)] = to; setFolderOrder(f);
    for (auto& b : m_bookmarks) if (b.folder == from) b.folder = to;
    save();
    return true;
}

void BookmarkManager::removeFolder(const QString& name, bool keepBookmarks) {
    QStringList f = folders(); f.removeAll(name); setFolderOrder(f);
    if (keepBookmarks) { for (auto& b : m_bookmarks) if (b.folder == name) b.folder.clear(); }
    else m_bookmarks.erase(std::remove_if(m_bookmarks.begin(), m_bookmarks.end(), [&](const Bookmark& b){ return b.folder == name; }), m_bookmarks.end());
    save();
}

void BookmarkManager::moveToFolder(const QString& url, const QString& folder) {
    const QString f = folder.trimmed();
    bool changed = false;
    for (auto& b : m_bookmarks) if (b.url == url && b.folder != f) { b.folder = f; changed = true; }
    if (!changed) return;   // sync calls this for every bookmark
    if (!f.isEmpty()) addFolder(f);
    save();
}

void BookmarkManager::setTitle(const QString& url, const QString& title) {
    bool changed = false;
    for (auto& b : m_bookmarks) if (b.url == url && b.title != title) { b.title = title; changed = true; }
    if (changed) save();
}
