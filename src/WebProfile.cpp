#include "WebProfile.h"
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
void GoogleSignInInterceptor::interceptRequest(QWebEngineUrlRequestInfo& info) {
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
