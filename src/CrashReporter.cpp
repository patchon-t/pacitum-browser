#include "CrashReporter.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QUuid>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <functional>

void pacitumLog(const QString& line);   // main.cpp (startup.log)

namespace CrashReporter {
namespace {
QString g_dir;
bool g_abrupt = false;

QString webhook() {
#ifdef PACITUM_CRASH_WEBHOOK
    return QStringLiteral(PACITUM_CRASH_WEBHOOK);
#else
    return {};
#endif
}
QString statePath() { return g_dir + QStringLiteral("/crash-reports.json"); }
QJsonObject readState() { QFile f(statePath()); return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject(); }
void writeState(const QJsonObject& o) { QSaveFile f(statePath()); if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(o).toJson()); f.commit(); } }

QString installId() {   // random, only to tell reports from the same computer apart
    QJsonObject s = readState();
    QString id = s.value(QStringLiteral("installId")).toString();
    if (id.isEmpty()) { id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); s.insert(QStringLiteral("installId"), id); writeState(s); }
    return id;
}
QByteArray tail(const QString& path, qint64 max) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (f.size() > max) f.seek(f.size() - max);
    return f.readAll();
}
qint64 crashLogTime() { const QFileInfo fi(g_dir + QStringLiteral("/crash.log")); return fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0; }

QNetworkAccessManager* net() { static auto* n = new QNetworkAccessManager(qApp); return n; }

// Sends a Discord webhook message with the logs attached. `done(ok)` is called when it has finished.
void send(const QString& title, const QString& details, const QList<QPair<QString, QByteArray>>& files, const std::function<void(bool)>& done) {
    const QString url = webhook();
    if (url.isEmpty()) { if (done) done(false); return; }
    QString text = QStringLiteral("**%1**\n%2\nPacitum %3 · %4 · Qt %5 · Chromium %6 · install %7")
        .arg(title, details, QLatin1String(PACITUM_VERSION), QSysInfo::prettyProductName(), QLatin1String(qVersion()),
             QLatin1String(qWebEngineChromiumVersion()), installId());
    if (text.size() > 1900) text = text.left(1900) + QStringLiteral("…");   // Discord's limit is 2000
    auto* multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart payload;
    payload.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"payload_json\""));
    payload.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    payload.setBody(QJsonDocument(QJsonObject{{"content", text}, {"allowed_mentions", QJsonObject{{"parse", QJsonArray()}}}}).toJson(QJsonDocument::Compact));
    multi->append(payload);
    for (int i = 0; i < files.size(); ++i) {
        if (files[i].second.isEmpty()) continue;
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"files[%1]\"; filename=\"%2\"").arg(i).arg(files[i].first));
        part.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/plain; charset=utf-8"));
        part.setBody(files[i].second);
        multi->append(part);
    }
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(30000);
    QNetworkReply* reply = net()->post(req, multi);
    multi->setParent(reply);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done]{
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && code >= 200 && code < 300;
        pacitumLog(QStringLiteral("crash report: sent=%1 http=%2 %3").arg(ok).arg(code).arg(ok ? QString() : reply->errorString()));
        reply->deleteLater();
        if (done) done(ok);
    });
}
}

bool available() { return !webhook().isEmpty(); }

void init(const QString& logDir) {
    g_dir = logDir;
    // The new run truncates startup.log: keep the last run's log for a report.
    const QString cur = g_dir + QStringLiteral("/startup.log"), prev = g_dir + QStringLiteral("/startup-previous.log");
    if (QFile::exists(cur)) { QFile::remove(prev); QFile::copy(cur, prev); }
}

void setLastRunAbrupt(bool abrupt) { g_abrupt = g_abrupt || abrupt; }

QString consent() { const QString c = readState().value(QStringLiteral("consent")).toString(); return c == QLatin1String("on") || c == QLatin1String("off") ? c : QStringLiteral("ask"); }
void setConsent(const QString& c) { QJsonObject s = readState(); s.insert(QStringLiteral("consent"), c); writeState(s); }

QString scrub(const QString& in) {
    QString t = in;
    // Page addresses: keep the scheme and site, drop the path and query (they can hold searches, tokens, names).
    static const QRegularExpression url(QStringLiteral("\\b((?:https?|wss?|ftp)://)([^/\\s\"'<>]+)[^\\s\"'<>]*"));
    t.replace(url, QStringLiteral("\\1\\2/…"));
    static const QRegularExpression fileUrl(QStringLiteral("\\bfile:///[^\\s\"'<>]*"));
    t.replace(fileUrl, QStringLiteral("file:///…"));
    // User names in paths and e-mail addresses.
    static const QRegularExpression winUser(QStringLiteral("([A-Za-z]:[\\\\/]Users[\\\\/])[^\\\\/\\s\"']+"), QRegularExpression::CaseInsensitiveOption);
    t.replace(winUser, QStringLiteral("\\1<user>"));
    static const QRegularExpression home(QStringLiteral("(/home/)[^/\\s\"']+"));
    t.replace(home, QStringLiteral("\\1<user>"));
    static const QRegularExpression mail(QStringLiteral("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"));
    t.replace(mail, QStringLiteral("<email>"));
    return t;
}

void maybeReport(QWidget* parent) {
    if (!available() || g_dir.isEmpty()) return;
    QJsonObject s = readState();
    const qint64 crashed = crashLogTime();
    const bool newCrash = crashed > qint64(s.value(QStringLiteral("reportedCrash")).toDouble());
    if (!newCrash && !g_abrupt) return;
    g_abrupt = false;
    auto markHandled = [crashed]{ QJsonObject st = readState(); st.insert(QStringLiteral("reportedCrash"), double(crashed)); writeState(st); };
    auto doSend = [crashed, newCrash]{
        QString what = newCrash ? QString::fromUtf8(tail(g_dir + QStringLiteral("/crash.log"), 600)).trimmed().section(QLatin1Char('\n'), -1)
                                : QStringLiteral("The last run ended without a normal quit (no crash.log entry).");
        send(newCrash ? QStringLiteral("Crash report") : QStringLiteral("Unexpected exit"), scrub(what),
             {{QStringLiteral("startup-previous.log"), scrub(QString::fromUtf8(tail(g_dir + QStringLiteral("/startup-previous.log"), 1500 * 1024))).toUtf8()},
              {QStringLiteral("crash.log"), scrub(QString::fromUtf8(tail(g_dir + QStringLiteral("/crash.log"), 64 * 1024))).toUtf8()}},
             [crashed](bool ok){ if (ok) { QJsonObject st = readState(); st.insert(QStringLiteral("reportedCrash"), double(crashed)); writeState(st); } });
    };
    const QString c = consent();
    if (c == QLatin1String("off")) { markHandled(); return; }
    if (c == QLatin1String("on")) { doSend(); return; }

    QMessageBox box(QMessageBox::Question, QObject::tr("Pacitum closed unexpectedly"),
                    QObject::tr("Send a crash report to help fix it? It contains what went wrong, your Pacitum and system versions, "
                                "and Pacitum's log of that session with page addresses shortened to the site name."), QMessageBox::NoButton, parent);
    auto* always = box.addButton(QObject::tr("Send automatically from now on"), QMessageBox::AcceptRole);
    auto* once = box.addButton(QObject::tr("Send this time"), QMessageBox::YesRole);
    auto* never = box.addButton(QObject::tr("Don't send"), QMessageBox::RejectRole);
    box.setDefaultButton(always);
    box.exec();
    if (box.clickedButton() == always) { setConsent(QStringLiteral("on")); doSend(); }
    else if (box.clickedButton() == once) doSend();
    else { Q_UNUSED(never); markHandled(); }
}

void reportProblem(QWidget* parent) {
    if (!available()) return;
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(parent, QObject::tr("Report a problem"),
        QObject::tr("What went wrong? Pacitum's log of this session is sent with it (page addresses shortened to the site name)."), QString(), &ok);
    if (!ok || text.trimmed().isEmpty()) return;
    QPointer<QWidget> p(parent);
    send(QStringLiteral("Problem report"), scrub(text.trimmed().left(1200)),
         {{QStringLiteral("startup.log"), scrub(QString::fromUtf8(tail(g_dir + QStringLiteral("/startup.log"), 1500 * 1024))).toUtf8()}},
         [p](bool sent){ if (p) QMessageBox::information(p, QObject::tr("Report a problem"), sent ? QObject::tr("Thanks, your report was sent.") : QObject::tr("The report couldn't be sent. Check your connection and try again.")); });
}
}
