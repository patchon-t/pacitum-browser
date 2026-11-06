#pragma once
#include <QObject>
#include <QString>
#include <QUrl>
#include <QPointer>
#include <QStringList>

class QNetworkAccessManager;
#include <QNetworkReply>
class QTimer;
class QFile;

// Built-in updater (1.5.2).
//
// Feed: any URL that returns a GitHub "release" JSON object (tag_name, name, body, html_url,
// assets[{name, size, browser_download_url}]). Default:
//   https://api.github.com/repos/namo2024mntik-ops/pacitum-browser/releases?per_page=40 (a list: the highest
//   version of this build's line is used; the legacy Qt 6.8 build only takes 1.8.x)
// Overrides (first wins): env PACITUM_UPDATE_FEED, setting "update_feed_url".
// Flow: check -> compare versions -> prompt with release notes -> download the Setup .exe with
// progress -> verify size (and SHA256 when a "<installer>.sha256" or "SHA256SUMS" asset exists) ->
// "Restart now" -> startInstall(): a helper (PacitumUpdateHelper.exe on Windows, the new AppImage
// with --update-overlay on Linux) shows the "Installing update…" pop-up, waits for the browser to
// exit, runs Setup /VERYSILENT … /RELAUNCH=1 (same AppId: in-place upgrade) and stays on screen
// until the new version writes markerFile(). The new version then shows "Update done" (1.8.9).
struct UpdateInfo {
    QString version;        // "1.5.3"
    QString tag;            // "v1.5.3"
    QString name;
    QString notes;          // markdown
    QString pageUrl;
    QUrl installerUrl;
    QString installerName;
    qint64 installerSize = -1;
    QUrl shaUrl;            // optional
    bool shaIsSums = false; // SHA256SUMS (many lines) vs single "<hash>  <file>"
    bool valid() const { return !version.isEmpty() && installerUrl.isValid(); }
};

class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(QObject* parent = nullptr);
    static Updater* instance();

    static QUrl feedUrl();
    static int compareVersions(const QString& a, const QString& b);   // <0, 0, >0
    static QString currentVersion();

    void check(bool userInitiated);
    void download(const UpdateInfo& info);
    void cancelDownload();
    // Starts the install helper with these extra pop-up arguments (--cx/--cy/--colors…). False when
    // nothing could be started: the app stays open and offers the downloaded file.
    bool startInstall(const QStringList& overlayArgs);
    static QString workDir();           // <temp>/PacitumUpdate: download, helper copy, hand-off files
    static QString readyFile() { return workDir() + QStringLiteral("/overlay-ready"); }    // helper's pop-up is on screen
    static QString markerFile() { return workDir() + QStringLiteral("/relaunched"); }      // new version is running
    static QString pendingFile() { return workDir() + QStringLiteral("/pending.json"); }   // what was installed (for "Update done")
    static QString updateLog();         // <temp>/PacitumUpdate/update.log: browser + helper, survives restarts
    static void logLine(const QString& line);   // startup.log and update.log
    bool helperRunning() const;         // Windows: the install helper process is still alive
    bool runInstallerYourself();        // Windows: Setup with its own window (when the helper failed)
    static QStringList noteLines(const QString& markdown, int max = 4);   // release-note bullets as plain text
    const UpdateInfo& latest() const { return m_latest; }
    QString downloadedPath() const { return m_file; }
    bool busy() const { return m_reply != nullptr; }

    void setAutoCheck(bool on);         // periodic background check (setting "auto_check_updates")
    QString lastStatus() const { return m_status; }

signals:
    void checking();
    void updateAvailable(const UpdateInfo& info, bool userInitiated);
    void upToDate(bool userInitiated);
    void checkFailed(const QString& error, bool userInitiated);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(bool ok, const QString& error);
    void statusChanged(const QString& status);

private:
    void setStatus(const QString& s);
    void fetchSha();
    void verifyAndFinish(const QByteArray& expectedHex);
    QNetworkAccessManager* m_net = nullptr;
    QPointer<QNetworkReply> m_reply;
    QTimer* m_timer = nullptr;
    UpdateInfo m_latest;
    UpdateInfo m_pending;
    QFile* m_out = nullptr;
    QString m_file;
    qint64 m_helperPid = 0;
    QString m_status;
};
