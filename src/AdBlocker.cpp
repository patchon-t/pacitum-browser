#include <atomic>
#include <QDate>
#include "AdBlocker.h"

const QSet<QString> AdBlocker::s_hosts = {
    QStringLiteral("doubleclick.net"), QStringLiteral("googlesyndication.com"),
    QStringLiteral("googleadservices.com"), QStringLiteral("google-analytics.com"),
    QStringLiteral("googletagmanager.com"), QStringLiteral("adservice.google.com"),
    QStringLiteral("adsrvr.org"), QStringLiteral("adnxs.com"), QStringLiteral("rubiconproject.com"),
    QStringLiteral("criteo.com"), QStringLiteral("criteo.net"), QStringLiteral("pubmatic.com"),
    QStringLiteral("openx.net"), QStringLiteral("amazon-adsystem.com"), QStringLiteral("scorecardresearch.com"),
    QStringLiteral("connect.facebook.net"), QStringLiteral("bat.bing.com"),
    QStringLiteral("hotjar.com"), QStringLiteral("mouseflow.com"), QStringLiteral("taboola.com"),
    QStringLiteral("outbrain.com"), QStringLiteral("moatads.com"),
    // 1.8.8: more of the big ad / tracking networks (subdomains are matched too, see hostBlocked)
    QStringLiteral("googletagservices.com"), QStringLiteral("2mdn.net"), QStringLiteral("adform.net"),
    QStringLiteral("quantserve.com"), QStringLiteral("casalemedia.com"), QStringLiteral("smartadserver.com"),
    QStringLiteral("yieldmo.com"), QStringLiteral("teads.tv"), QStringLiteral("zedo.com"), QStringLiteral("ads-twitter.com")
};

const QStringList AdBlocker::s_patterns = {
    QStringLiteral("/ads/"), QStringLiteral("/ad/"), QStringLiteral("/ad-"),
    QStringLiteral("/banner"), QStringLiteral("/tracking"), QStringLiteral("/pixel"),
    QStringLiteral("pagead"), QStringLiteral("doubleclick"), QStringLiteral("advertisement"),
    QStringLiteral("google_ad"), QStringLiteral("sponsored"), QStringLiteral("popunder"),
    QStringLiteral("facebook.com/tr?"), QStringLiteral("facebook.com/tr/")
};

// Listed domains block their subdomains too (www.google-analytics.com, securepubads.g.doubleclick.net):
// walk up the labels, one hash lookup each.
static bool hostBlocked(const QSet<QString>& hosts, QString host) {
    while (host.count(QLatin1Char('.')) >= 1) {
        if (hosts.contains(host)) return true;
        host = host.mid(host.indexOf(QLatin1Char('.')) + 1);
    }
    return false;
}

AdBlocker::AdBlocker(bool enabled, QObject* parent) : QWebEngineUrlRequestInterceptor(parent), m_enabled(enabled) {}

void AdBlocker::interceptRequest(QWebEngineUrlRequestInfo &info) {
    if (m_dnt) { info.setHttpHeader("DNT", "1"); info.setHttpHeader("Sec-GPC", "1"); }
    if (!m_enabled) return;
    const QString host = info.requestUrl().host().toLower();
    if (host.isEmpty()) return;
    if (hostBlocked(s_hosts, host)) { info.block(true); AdBlocker::noteBlocked(); return; }
    // URL patterns only for what a page loads by itself: a page you open (address bar, link) is never
    // blocked for words like "sponsored" in its address.
    if (info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame) return;
    const QString url = info.requestUrl().toString().toLower();
    for (auto &p : s_patterns) if (url.contains(p)) { info.block(true); AdBlocker::noteBlocked(); return; }
}

// 2.0.1 new tab page: "N trackers blocked today" (network thread increments, UI reads).
static std::atomic<int> s_blocked{0};
static std::atomic<qint64> s_blockedDay{0};
void AdBlocker::noteBlocked() {
    const qint64 day = QDate::currentDate().toJulianDay();
    if (s_blockedDay.exchange(day) != day) s_blocked = 0;
    ++s_blocked;
}
int AdBlocker::blockedToday() { return s_blockedDay.load() == QDate::currentDate().toJulianDay() ? s_blocked.load() : 0; }
