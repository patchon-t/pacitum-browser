#include "WebProfile.h"
#include "AppSettings.h"
#include <QWebEngineCookieStore>
#include <QJsonObject>
#include <QJsonArray>
#include <QHostAddress>
#include <QSet>
#include <QMutexLocker>
#include <QMutex>
#include <atomic>
#include "PacitumScheme.h"
#include "ExtensionManager.h"
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <QRegularExpression>
#include <QLocale>
#include <QStringList>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QUrl>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEngineClientHints>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#include <QWebEngineProfileBuilder>
#endif

namespace Pacitum {

WebPrefs& webPrefs() { static WebPrefs p; return p; }

static bool g_guest = false;
void setGuestMode(bool on) { g_guest = on; }
bool guestMode() { return g_guest; }

static QString g_storagePath, g_cachePath;
static bool g_builtWithStorage = false;
void setMainProfileStorage(const QString& persistentStoragePath, const QString& cachePath) {
    g_storagePath = persistentStoragePath; g_cachePath = cachePath;
}
bool mainProfileBuiltWithStorage() { mainProfile(); return g_builtWithStorage; }

QWebEngineProfile* mainProfile() {
    // Guest (1.6 profiles): an off-the-record profile -- nothing is written to disk.
    static QWebEngineProfile* p = []() -> QWebEngineProfile* {
        if (g_guest) return new QWebEngineProfile();
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        if (!g_storagePath.isEmpty()) {
            QWebEngineProfileBuilder b;
            b.setPersistentStoragePath(g_storagePath);
            if (!g_cachePath.isEmpty()) b.setCachePath(g_cachePath);
            g_builtWithStorage = true;
            return b.createProfile(QStringLiteral("Pacitum"));
        }
#endif
        return new QWebEngineProfile(QStringLiteral("Pacitum"));
    }();
    return p;
}

static QString acceptLanguage() {
    QStringList langs;
    for (QString l : QLocale::system().uiLanguages()) {
        l.replace(QLatin1Char('_'), QLatin1Char('-'));
        if (!langs.contains(l, Qt::CaseInsensitive)) langs << l;
        const QString base = l.section(QLatin1Char('-'), 0, 0);
        if (!langs.contains(base, Qt::CaseInsensitive)) langs << base;
    }
    if (!langs.contains(QStringLiteral("en"), Qt::CaseInsensitive)) { langs << QStringLiteral("en-US") << QStringLiteral("en"); }
    QStringList out; double q = 1.0;
    for (int i = 0; i < langs.size() && i < 6; ++i) {
        if (i == 0) out << langs[i];
        else { q = qMax(0.1, 1.0 - 0.1 * i); out << QStringLiteral("%1;q=%2").arg(langs[i]).arg(q, 0, 'f', 1); }
    }
    return out.join(QLatin1Char(','));
}

static QString chromeUserAgent(const QString& defaultUa) {
    const QString chromiumVer = QString::fromLatin1(qWebEngineChromiumVersion());
    const QString major = chromiumVer.section(QLatin1Char('.'), 0, 0);
    QString ua = defaultUa;
    ua.remove(QRegularExpression(QStringLiteral("QtWebEngine/\\S+\\s*")));
    // Chrome ships a "reduced" UA: Chrome/<major>.0.0.0
    if (!major.isEmpty())
        ua.replace(QRegularExpression(QStringLiteral("Chrome/[\\d.]+")), QStringLiteral("Chrome/%1.0.0.0").arg(major));
    return ua.simplified();
}

static QString& defaultUaStore() { static QString ua; return ua; }
QString defaultUserAgent() { return defaultUaStore(); }

bool isGoogleSignInHost(const QString& rawHost) {
    const QString h = rawHost.toLower();
    return h == QLatin1String("accounts.google.com") || h.startsWith(QLatin1String("accounts.google."))
        || h == QLatin1String("accounts.youtube.com") || h == QLatin1String("gds.google.com");
}

bool isGoogleSignInBlocked(const QUrl& u) {
    if (!isGoogleSignInHost(u.host())) return false;
    const QString path = u.path().toLower();
    return path.contains(QLatin1String("/signin/rejected")) || path.contains(QLatin1String("/disallowed"))
        || path.contains(QLatin1String("deniedsigninrejected"));
}

GoogleSignInInterceptor::GoogleSignInInterceptor(QObject* parent) : QWebEngineUrlRequestInterceptor(parent) {}
namespace {
std::atomic<bool> s_httpsOnly{true};
QMutex s_httpMx;
QSet<QString> s_httpOk, s_upgraded;
bool isLocalHost(const QString& h) {
    if (h.isEmpty() || h == QLatin1String("localhost") || h.endsWith(QLatin1String(".localhost")) || h.endsWith(QLatin1String(".local"))
        || h.endsWith(QLatin1String(".lan")) || h.endsWith(QLatin1String(".internal")) || !h.contains(QLatin1Char('.'))) return true;
    QHostAddress a; if (a.setAddress(h)) return true;   // IP addresses: routers, printers, dev servers
    return false;
}
}
void setHttpsOnly(bool on) { s_httpsOnly = on; }
bool httpsOnly() { return s_httpsOnly; }
void allowHttp(const QString& host) { QMutexLocker l(&s_httpMx); s_httpOk.insert(host.toLower()); }
bool httpAllowed(const QString& host) { QMutexLocker l(&s_httpMx); return s_httpOk.contains(host.toLower()); }
bool wasUpgraded(const QString& host) { QMutexLocker l(&s_httpMx); return s_upgraded.contains(host.toLower()); }
void setSafeBrowsing(const QString&) {}

void applyProfilePrefs(QWebEngineProfile* profile, bool privateProfile) {
    if (!profile) return;
    const QJsonObject o = AppSettings::read();
    // Languages: Accept-Language and spell check follow Settings › Languages.
    QStringList langs;
    for (const auto& v : o.value(QStringLiteral("languages")).toArray()) langs << v.toString();
    if (langs.isEmpty()) profile->setHttpAcceptLanguage(acceptLanguage());
    else {
        QStringList out; double q = 1.0;
        for (int i = 0; i < langs.size() && i < 8; ++i) {
            if (i == 0) out << langs[i];
            else { q = qMax(0.1, 1.0 - 0.1 * i); out << QStringLiteral("%1;q=%2").arg(langs[i]).arg(q, 0, 'f', 1); }
        }
        profile->setHttpAcceptLanguage(out.join(QLatin1Char(',')));
    }
    const bool spell = o.value(QStringLiteral("spellcheck")).toBool(true);
    QStringList dicts;
    for (const QString& l : langs.isEmpty() ? QStringList{QLocale::system().name()} : langs) {
        QString d = l; d.replace(QLatin1Char('_'), QLatin1Char('-'));
        if (d == QLatin1String("en")) d = QStringLiteral("en-US");
        dicts << d;
    }
    profile->setSpellCheckLanguages(dicts);
    profile->setSpellCheckEnabled(spell);
    // Cookies: block third-party everywhere, in private tabs only, or allow all.
    const QString mode = o.value(QStringLiteral("cookie_mode")).toString(QStringLiteral("third"));
    const bool blockThird = mode == QLatin1String("third") || (mode == QLatin1String("private") && privateProfile);
    if (auto* cs = profile->cookieStore())
        cs->setCookieFilter([blockThird](const QWebEngineCookieStore::FilterRequest& r){ return !(blockThird && r.thirdParty); });
    // Fonts
    auto* ws = profile->settings();
    ws->setFontSize(QWebEngineSettings::DefaultFontSize, o.value(QStringLiteral("default_font_size")).toInt(16));
    ws->setFontSize(QWebEngineSettings::MinimumFontSize, o.value(QStringLiteral("min_font_size")).toInt(0));
    ws->setFontSize(QWebEngineSettings::MinimumLogicalFontSize, qMax(6, o.value(QStringLiteral("min_font_size")).toInt(0)));
    ws->setAttribute(QWebEngineSettings::SpatialNavigationEnabled, false);
    s_httpsOnly = o.value(QStringLiteral("https_only")).toBool(true);
}

void GoogleSignInInterceptor::interceptRequest(QWebEngineUrlRequestInfo& info) {
    // 2.0.1 "Always use secure connections": upgrade top-level http:// navigations to https://.
    if (s_httpsOnly && info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
        && info.requestUrl().scheme() == QLatin1String("http") && info.requestMethod() == "GET") {
        const QString host = info.requestUrl().host().toLower();
        if (!isLocalHost(host)) {
            bool ok;
            { QMutexLocker l(&s_httpMx); ok = s_httpOk.contains(host); if (!ok) s_upgraded.insert(host); }
            if (!ok) {
                QUrl u = info.requestUrl(); u.setScheme(QStringLiteral("https"));
                if (u.port() == 80) u.setPort(-1);
                info.redirect(u);
                return;
            }
        }
    }
    // Google sign-in pages must see the honest, unmodified QtWebEngine user agent (Google's embedded-browser policy).
    if (isGoogleSignInHost(info.requestUrl().host()) || isGoogleSignInHost(info.firstPartyUrl().host()))
        if (!defaultUaStore().isEmpty()) info.setHttpHeader("User-Agent", defaultUaStore().toUtf8());
}

void configureProfile(QWebEngineProfile* profile, bool persistent) {
    if (!profile) return;
    if (persistent) ExtensionManager::instance()->attach(profile);   // 1.8.5 mini extensions (not in private/guest)
    const QString honest = profile->httpUserAgent();
    if (defaultUaStore().isEmpty()) defaultUaStore() = honest;
    profile->setHttpUserAgent(chromeUserAgent(honest));
    {   // Same honesty for navigator.userAgent on Google sign-in pages.
        QString js = QStringLiteral(
            "(function(){var h=location.hostname;"
            "if(!(h==='accounts.google.com'||h.indexOf('accounts.google.')===0||h==='accounts.youtube.com'||h==='gds.google.com'))return;"
            "var ua=%1;try{Object.defineProperty(Navigator.prototype,'userAgent',{get:function(){return ua;},configurable:true});"
            "Object.defineProperty(Navigator.prototype,'appVersion',{get:function(){return ua.replace(/^Mozilla\\//,'');},configurable:true});}catch(e){}})();");
        QString q = honest; q.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('\''), QStringLiteral("\\'"));
        js = js.arg(QStringLiteral("'") + q + QStringLiteral("'"));
        QWebEngineScript sc; sc.setName(QStringLiteral("pacitum-honest-ua")); sc.setSourceCode(js);
        sc.setInjectionPoint(QWebEngineScript::DocumentCreation); sc.setWorldId(QWebEngineScript::MainWorld);
        sc.setRunsOnSubFrames(true);
        profile->scripts()->insert(sc);
    }
    profile->setHttpAcceptLanguage(acceptLanguage());
    applyProfilePrefs(profile, !persistent);   // 2.0.1 languages, spell check, cookies, fonts
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (auto* ch = profile->clientHints()) {
        const QString full = QString::fromLatin1(qWebEngineChromiumVersion());
        const QString major = full.section(QLatin1Char('.'), 0, 0);
        ch->setFullVersion(full);
        ch->setFullVersionList({{QStringLiteral("Chromium"), full},
                                {QStringLiteral("Not-A.Brand"), QStringLiteral("99.0.0.0")}});
        Q_UNUSED(major);
    }
#endif
    if (persistent) {
        profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
        profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
        profile->setHttpCacheMaximumSize(512 * 1024 * 1024);   // 1.8.8: more repeat visits come from disk
    }
    auto* s = profile->settings();
    s->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    s->setAttribute(QWebEngineSettings::WebGLEnabled, true);
    s->setAttribute(QWebEngineSettings::Accelerated2dCanvasEnabled, true);
    s->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, true);
    s->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
    s->setAttribute(QWebEngineSettings::ErrorPageEnabled, true);
    applyWebSettings(s);
    PacitumSchemeHandler::install(profile);
}

void applyWebSettings(QWebEngineSettings* s) {
    if (!s) return;
    s->setAttribute(QWebEngineSettings::JavascriptEnabled, webPrefs().javascript);
    // 1.7.5: Chromium always hands window.open to Pacitum (PacitumPage::createWindow), which applies the
    // per-site pop-up decision and records blocked pop-ups; "popups_allowed" is now the default.
    s->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);
}

}
