#include "FaviconCache.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QRegularExpression>

namespace {
QString hostOf(const QString& url) {
    const QUrl u(url);
    if (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https")) return {};
    QString h = u.host().toLower();
    if (h.startsWith(QLatin1String("www."))) h = h.mid(4);
    static const QRegularExpression ok(QStringLiteral("^[a-z0-9.-]+$"));
    return ok.match(h).hasMatch() ? h : QString();
}
}

FaviconCache* FaviconCache::instance() { static auto* c = new FaviconCache(QCoreApplication::instance()); return c; }

FaviconCache::FaviconCache(QObject* parent) : QObject(parent) {}

QString FaviconCache::fileFor(const QString& host) const {
    return QDir::current().absoluteFilePath(QStringLiteral("favicons/") + host + QStringLiteral(".png"));
}

QIcon FaviconCache::icon(const QString& url) {
    const QString host = hostOf(url);
    if (host.isEmpty()) return {};
    if (auto it = m_icons.constFind(host); it != m_icons.constEnd()) return *it;
    const QString file = fileFor(host);
    if (QFileInfo::exists(file)) {
        const QPixmap pm(file);
        if (!pm.isNull()) { const QIcon ic(pm); m_icons.insert(host, ic); return ic; }
    }
    fetch(host);
    return {};
}

void FaviconCache::store(const QString& url, const QIcon& icon) {
    const QString host = hostOf(url);
    if (host.isEmpty() || icon.isNull()) return;
    const QPixmap pm = icon.pixmap(QSize(32, 32));
    if (pm.isNull()) return;
    QDir().mkpath(QFileInfo(fileFor(host)).absolutePath());
    pm.save(fileFor(host), "PNG");
    const bool isNew = !m_icons.contains(host);
    m_icons.insert(host, QIcon(pm));
    if (isNew) emit changed(host);
}

void FaviconCache::fetch(const QString& host) {
    if (m_tried.contains(host)) return;   // once per site per session
    m_tried.insert(host);
    if (!m_net) m_net = new QNetworkAccessManager(this);
    QNetworkRequest r(QUrl(QStringLiteral("https://%1/favicon.ico").arg(host)));
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    r.setTransferTimeout(8000);
    r.setRawHeader("User-Agent", QByteArray("PacitumBrowser/") + QCoreApplication::applicationVersion().toLatin1());
    QNetworkReply* rep = m_net->get(r);
    connect(rep, &QNetworkReply::finished, this, [this, rep, host]{
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) return;
        const QByteArray data = rep->read(512 * 1024);
        QImage img; img.loadFromData(data);
        if (img.isNull() || img.width() < 8) return;
        const QPixmap pm = QPixmap::fromImage(img.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        QDir().mkpath(QFileInfo(fileFor(host)).absolutePath());
        pm.save(fileFor(host), "PNG");
        m_icons.insert(host, QIcon(pm));
        emit changed(host);
    });
}
