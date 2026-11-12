#pragma once
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSet>
#include <QUrl>

class QNetworkAccessManager;

// 2.0.8: site icons for the bookmarks bar and bookmark menus. Icons of pages you open are kept in
// <profile>/favicons/<host>.png; a bookmarked site not seen yet has its /favicon.ico fetched once.
// Private tabs never store anything here.
class FaviconCache : public QObject {
    Q_OBJECT
public:
    static FaviconCache* instance();
    QIcon icon(const QString& url);                    // null when not known (yet); starts a fetch
    void store(const QString& url, const QIcon& icon);  // from a tab that finished loading its icon
signals:
    void changed(const QString& host);
private:
    explicit FaviconCache(QObject* parent);
    QString fileFor(const QString& host) const;
    void fetch(const QString& host);
    QHash<QString, QIcon> m_icons;
    QSet<QString> m_tried;
    QNetworkAccessManager* m_net = nullptr;
};
