#pragma once
#include <QWebEngineUrlSchemeHandler>
#include <QMap>
#include <QString>
#include <functional>

class QWebEngineProfile;

// Serves internal pages such as pacitum://newtab (design-spec §4 "New Tab Page").
class PacitumSchemeHandler : public QWebEngineUrlSchemeHandler {
    Q_OBJECT
public:
    static void registerScheme();            // call before QApplication
    static PacitumSchemeHandler* instance();
    static void install(QWebEngineProfile* profile);
    static QString newTabUrl() { return QStringLiteral("pacitum://newtab"); }

    void setThemeVars(const QMap<QString, QString>& vars) { m_vars = vars; }
    void setNewTabVars(const QMap<QString, QString>& vars) { m_ntpVars = vars; }   // empty = same as the rest
    void setSearchBase(const QString& s) { m_search = s; }
    void requestStarted(QWebEngineUrlRequestJob* job) override;
    // 1.6 home page: per-request JSON (profile name, recent pages, downloads, engine, theme) and
    // pacitum://action/<name>?k=v calls made by the page (theme, engine, open, downloads, history).
    void setHomeDataProvider(std::function<QString()> fn) { m_homeData = std::move(fn); }
signals:
    void homeAction(const QString& name, const QMap<QString, QString>& args);

private:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;
    QMap<QString, QString> m_vars, m_ntpVars;
    std::function<QString()> m_homeData;
    QString m_search = QStringLiteral("https://www.google.com/search?q=");
};
