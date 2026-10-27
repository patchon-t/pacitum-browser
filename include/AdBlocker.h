#pragma once
#include <QWebEngineUrlRequestInterceptor>
#include <QSet>
#include <QString>

class AdBlocker : public QWebEngineUrlRequestInterceptor {
    Q_OBJECT
public:
    explicit AdBlocker(bool enabled = true, QObject* parent = nullptr);
    void setEnabled(bool e) { m_enabled = e; }
    bool isEnabled() const { return m_enabled; }
    void setDoNotTrack(bool d) { m_dnt = d; }
    void interceptRequest(QWebEngineUrlRequestInfo &info) override;
    static void noteBlocked();
    static int blockedToday();
private:
    bool m_enabled;
    bool m_dnt = false;
    static const QSet<QString> s_hosts;
    static const QStringList s_patterns;
};
