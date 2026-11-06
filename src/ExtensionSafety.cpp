#include "ExtensionSafety.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

void pacitumLog(const QString& line);   // main.cpp (startup.log)

namespace ExtensionSafety {
namespace {
QString guardPath() { return QDir::current().absoluteFilePath(QStringLiteral("startup-guard.json")); }
QJsonObject readGuard() {
    QFile f(guardPath());
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}
void writeGuard(const QJsonObject& o) {
    QSaveFile f(guardPath());
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(o).toJson(QJsonDocument::Compact)); f.commit(); }
}
QString g_notice;

// The extension's display name from its manifest ("__MSG_…" names fall back to the folder name).
QString extensionName(const QString& dir) {
    QFile f(dir + QStringLiteral("/manifest.json"));
    QString n;
    if (f.open(QIODevice::ReadOnly)) n = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("name")).toString();
    return n.isEmpty() || n.startsWith(QLatin1String("__MSG_")) ? QFileInfo(dir).fileName() : n;
}
}

void beginStartup(const QString& crashLog) {
    QJsonObject g = readGuard();
    const qint64 started = qint64(g.value(QStringLiteral("started")).toDouble());
    const bool crashed = g.value(QStringLiteral("running")).toBool(false);
    int strikes = crashed ? g.value(QStringLiteral("strikes")).toInt() + 1 : 0;
    const QFileInfo cl(crashLog);
    const bool crashLogged = crashed && cl.exists() && cl.lastModified().toMSecsSinceEpoch() >= started;
    const QString installPath = g.value(QStringLiteral("installPath")).toString();
    if (crashed) pacitumLog(QStringLiteral("startup guard: the last run ended abruptly (%1 in a row%2)").arg(strikes).arg(crashLogged ? QStringLiteral(", crash.log written") : QString()));

    if (crashed && (strikes >= 2 || crashLogged) && !installPath.isEmpty() && QDir(installPath).exists()) {
        // The most recently added extension is the likely cause: move it out of the folder Qt loads from.
        auto added = [](const QFileInfo& d){ return d.birthTime().isValid() ? d.birthTime() : d.lastModified(); };
        QFileInfo newest;
        for (const QFileInfo& d : QDir(installPath).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (!newest.exists() || added(d) > added(newest)) newest = d;
        if (newest.exists()) {
            const QString name = extensionName(newest.absoluteFilePath());
            const QString qdir = QDir::current().absoluteFilePath(QStringLiteral("extensions-quarantine"));
            QDir().mkpath(qdir);
            const QString target = qdir + QLatin1Char('/') + newest.fileName() + QLatin1Char('-') + QString::number(QDateTime::currentSecsSinceEpoch());
            if (QDir().rename(newest.absoluteFilePath(), target)) {
                pacitumLog(QStringLiteral("startup guard: turned off %1 (moved %2 to %3)").arg(name, newest.absoluteFilePath(), target));
                g_notice = QObject::tr("Pacitum turned off the extension %1 because Pacitum closed unexpectedly after it was added. "
                                       "You can remove it from Extensions, or add it again later.").arg(name);
                strikes = 0;
            } else pacitumLog(QStringLiteral("startup guard: could not move %1").arg(newest.absoluteFilePath()));
        }
    }
    g.insert(QStringLiteral("running"), true);
    g.insert(QStringLiteral("started"), double(QDateTime::currentMSecsSinceEpoch()));
    g.insert(QStringLiteral("strikes"), strikes);
    writeGuard(g);
}

void rememberInstallPath(const QString& path) {
    if (path.isEmpty()) return;
    QJsonObject g = readGuard();
    if (g.value(QStringLiteral("installPath")).toString() == path) return;
    g.insert(QStringLiteral("installPath"), path);
    writeGuard(g);
}

void markHealthy() {
    QJsonObject g = readGuard();
    if (!g.value(QStringLiteral("running")).toBool(false)) return;
    g.insert(QStringLiteral("running"), false);
    g.insert(QStringLiteral("strikes"), 0);
    writeGuard(g);
}

QString takeNotice() { const QString n = g_notice; g_notice.clear(); return n; }
}
