#include "PacitumScheme.h"
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineProfile>
#include <QBuffer>
#include <QFile>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

void PacitumSchemeHandler::registerScheme() {
    QWebEngineUrlScheme s("pacitum");
    s.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    s.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(s);
}

PacitumSchemeHandler* PacitumSchemeHandler::instance() {
    static PacitumSchemeHandler* h = new PacitumSchemeHandler();
    return h;
}

void PacitumSchemeHandler::install(QWebEngineProfile* profile) {
    if (profile && !profile->urlSchemeHandler("pacitum")) profile->installUrlSchemeHandler("pacitum", instance());
}

void PacitumSchemeHandler::requestStarted(QWebEngineUrlRequestJob* job) {
    const QString host = job->requestUrl().host();
    QByteArray body;
    QByteArray mime = "text/html";
    auto svg = [](const QString& name, const QString& cls) {
        QFile s(QStringLiteral(":/icons/ui/%1.svg").arg(name));
        if (!s.open(QIODevice::ReadOnly)) return QString();
        QString t = QString::fromUtf8(s.readAll()).trimmed();
        t.replace(QStringLiteral(" width=\"24\" height=\"24\""), QString());
        t.replace(QStringLiteral("<svg "), QStringLiteral("<svg class=\"%1\" aria-hidden=\"true\" ").arg(cls));
        return t;
    };
    if (host == QStringLiteral("action")) {
        // Calls from the home page (same-origin fetch). Only a fixed set of names is acted on.
        const QString name = job->requestUrl().path().mid(1);
        QMap<QString, QString> args;
        for (const auto& kv : QUrlQuery(job->requestUrl()).queryItems(QUrl::FullyDecoded)) args.insert(kv.first, kv.second);
        const QString initiator = job->initiator().toString();
        if (initiator.isEmpty() || initiator.startsWith(QStringLiteral("pacitum:")) || initiator == QStringLiteral("null"))
            emit homeAction(name, args);
        auto* buf = new QBuffer(job); buf->setData("ok");
        job->reply("text/plain", buf);
        return;
    }
    if (host == QStringLiteral("fonts")) {
        // Bundled Inter for the internal pages (pacitum://fonts/Inter-Regular.ttf). Fixed names only.
        static const QStringList allowed{QStringLiteral("Rubik-VF.ttf"), QStringLiteral("Inter-Regular.ttf"), QStringLiteral("Inter-Medium.ttf"),
                                         QStringLiteral("Inter-SemiBold.ttf"), QStringLiteral("Inter-Bold.ttf")};
        const QString name = job->requestUrl().path().mid(1);
        QFile f(QStringLiteral(":/fonts/") + name);
        if (!allowed.contains(name) || !f.open(QIODevice::ReadOnly)) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        auto* buf = new QBuffer(job); buf->setData(f.readAll());
        job->reply("font/ttf", buf);
        return;
    }
    if (host == QStringLiteral("newtab") || host.isEmpty()) {
        QFile f(QStringLiteral(":/pages/newtab.html"));
        if (!f.open(QIODevice::ReadOnly)) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        QString html = QString::fromUtf8(f.readAll());
        static const QList<QPair<const char*, const char*>> icons{
            {"SEARCH", "search"}, {"WEATHER", "weather-partly"}, {"READING", "reading-list"}, {"DOWNLOAD", "download"},
            {"CUSTOMIZE", "customize"}, {"CLOSE", "close"}, {"UPLOAD", "upload"}, {"PALETTE", "palette"}, {"GRID", "grid"},
            {"EDIT", "edit"}, {"CHEV", "chevron-right"}, {"CLOCK", "clock"}, {"SYSTEM", "theme-system"}, {"SUN", "sun"}, {"MOON", "moon"}};
        for (const auto& ic : icons) html.replace(QStringLiteral("%I_%1%").arg(QLatin1String(ic.first)), svg(QLatin1String(ic.second), QStringLiteral("i")));
        const QJsonObject js{{"chevd", svg(QStringLiteral("chevron-down"), QStringLiteral("i"))}, {"edit12", svg(QStringLiteral("edit"), QStringLiteral("i"))},
                             {"weather44", svg(QStringLiteral("weather-partly"), QStringLiteral("i"))}, {"weather16", svg(QStringLiteral("weather-partly"), QStringLiteral("i"))},
                             {"upload", svg(QStringLiteral("upload"), QStringLiteral("i"))}};
        html.replace(QStringLiteral("%ICONS%"), QString::fromUtf8(QJsonDocument(js).toJson(QJsonDocument::Compact)).replace(QStringLiteral("</"), QStringLiteral("<\\/")));
        QString data = m_homeData ? m_homeData() : QStringLiteral("{}");
        data.replace(QStringLiteral("</"), QStringLiteral("<\\/"));
        const auto& vars = m_ntpVars.isEmpty() ? m_vars : m_ntpVars;
        for (auto it = vars.cbegin(); it != vars.cend(); ++it) html.replace(QLatin1Char('%') + it.key() + QLatin1Char('%'), it.value());
        html.replace(QStringLiteral("%SEARCH%"), m_search);
        {   // 2.0.1 new tab page: the PACITUM wordmark (dark letters follow the text colour)
            QFile wm(QStringLiteral(":/brand/wordmark_paths.svgfrag"));
            html.replace(QStringLiteral("%WORDMARK%"), wm.open(QIODevice::ReadOnly) ? QString::fromUtf8(wm.readAll()) : QString());
        }
        html.replace(QStringLiteral("%DATA%"), data);   // last: user data is never re-scanned for tokens
        QFile logo(QStringLiteral(":/pages/logo.png"));
        if (logo.open(QIODevice::ReadOnly))
            html.replace(QStringLiteral("qrc:/pages/logo.png"), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(logo.readAll().toBase64()));
        body = html.toUtf8();
    } else if (host == QStringLiteral("welcome")) {
        // First-run flow (firstrun-spec.md). Self-contained: tokens, icons, logo and qwebchannel.js inlined.
        QFile f(QStringLiteral(":/pages/welcome.html"));
        if (!f.open(QIODevice::ReadOnly)) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        QString html = QString::fromUtf8(f.readAll());
        for (auto it = m_vars.cbegin(); it != m_vars.cend(); ++it) html.replace(QLatin1Char('%') + it.key() + QLatin1Char('%'), it.value());
        html.replace(QStringLiteral("%ARROW%"), svg(QStringLiteral("arrow-right"), QStringLiteral("i")));
        html.replace(QStringLiteral("%I_IMPORT%"), svg(QStringLiteral("import"), QStringLiteral("i")));
        html.replace(QStringLiteral("%I_DEFAULT%"), svg(QStringLiteral("default-browser"), QStringLiteral("i")));
        html.replace(QStringLiteral("%I_SYSTEM%"), svg(QStringLiteral("theme-system"), QStringLiteral("i")));
        html.replace(QStringLiteral("%I_SUN%"), svg(QStringLiteral("sun"), QStringLiteral("i")));
        html.replace(QStringLiteral("%I_MOON%"), svg(QStringLiteral("moon"), QStringLiteral("i")));
        html.replace(QStringLiteral("%I_CHECK%"), svg(QStringLiteral("check"), QStringLiteral("i")));
        html.replace(QStringLiteral("%I_SPARKLE%"), svg(QStringLiteral("sparkle"), QStringLiteral("sp")));
        QFile wc(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
        html.replace(QStringLiteral("%QWEBCHANNEL%"), wc.open(QIODevice::ReadOnly) ? QString::fromUtf8(wc.readAll()) : QString());
        QFile logo(QStringLiteral(":/pages/logo.png"));
        if (logo.open(QIODevice::ReadOnly))
            html.replace(QStringLiteral("qrc:/pages/logo.png"), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(logo.readAll().toBase64()));
        body = html.toUtf8();
    } else if (host == QStringLiteral("settings")) {
        // 2.0.1 Settings (design: "Settings · all sections"). Talks to SettingsBridge over QWebChannel.
        QFile f(QStringLiteral(":/pages/settings.html"));
        if (!f.open(QIODevice::ReadOnly)) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        QString html = QString::fromUtf8(f.readAll());
        for (auto it = m_vars.cbegin(); it != m_vars.cend(); ++it) html.replace(QLatin1Char('%') + it.key() + QLatin1Char('%'), it.value());
        QFile wc(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
        html.replace(QStringLiteral("%QWEBCHANNEL%"), wc.open(QIODevice::ReadOnly) ? QString::fromUtf8(wc.readAll()) : QString());
        QFile np(QStringLiteral(":/brand/next_project_themed.svg"));
        QString mark = np.open(QIODevice::ReadOnly) ? QString::fromUtf8(np.readAll()) : QString();
        mark.replace(QRegularExpression(QStringLiteral("<\\?xml[^>]*>")), QString());
        mark.replace(QRegularExpression(QStringLiteral("<svg width=\"[0-9.]+\" height=\"[0-9.]+\"")), QStringLiteral("<svg"));
        mark.replace(QStringLiteral("<svg "), QStringLiteral("<svg class=\"mark\" style=\"height:16px;width:auto\" role=\"img\" aria-label=\"NEXT PROJECT\" "));
        html.replace(QStringLiteral("%NEXT_MARK%"), mark);
        body = html.toUtf8();
    } else {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }
    auto* buf = new QBuffer(job);
    buf->setData(body);
    job->reply(mime, buf);
}
