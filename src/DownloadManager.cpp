#include "AppSettings.h"
#include "Widgets.h"
#include "DownloadManager.h"
#include <QSet>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include "Motion.h"
#include "ThemeManager.h"
#include "WebProfile.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStyle>
#include <QAbstractButton>
#include <QtMath>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>

using DS = QWebEngineDownloadRequest;

// ---------- helpers ----------
static const PacitumPalette& pal() { return ThemeManager::current(); }
static bool isDark() { return QColor(pal().page).lightness() < 128; }

static QIcon cachedIcon(const QString& name, const QString& color, int size) {
    static QHash<QString, QIcon> cache;
    const QString key = name + QLatin1Char('|') + color + QLatin1Char('|') + QString::number(size);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.insert(key, ThemeManager::icon(name, color, size));
    return *it;
}
static QPixmap iconPm(const QString& name, const QString& color, int size, qreal dpr) {
    return cachedIcon(name, color, size).pixmap(QSize(size, size), dpr);
}
static QString rgba(const QString& hex, double a) {
    QColor c(hex); return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a);
}

QString DownloadEntry::filePath() const { return QDir(dir).filePath(holdName.isEmpty() ? fileName : holdName); }
QString DownloadEntry::domain() const {
    QString h = QUrl(url).host(); if (h.startsWith(QStringLiteral("www."))) h = h.mid(4);
    return h.isEmpty() ? QUrl(url).scheme() : h;
}
double DownloadEntry::speed() const {
    if (samples.size() < 2) return 0;
    const auto& a = samples.first(); const auto& b = samples.last();
    const qint64 dt = b.first - a.first; return dt > 0 ? double(b.second - a.second) * 1000.0 / dt : 0;
}
qint64 DownloadEntry::secondsLeft() const {
    if (total <= 0 || !started.isValid() || started.msecsTo(QDateTime::currentDateTime()) < 2000) return -1;
    const double s = speed(); if (s <= 1) return -1;
    return qint64((total - received) / s) + 1;
}

QString DownloadManager::formatSize(qint64 b) {
    if (b < 0) return QStringLiteral("?");
    if (b < 1024) return QString::number(b) + QStringLiteral(" B");
    const char* units[] = {"KB", "MB", "GB", "TB"};
    double v = b / 1024.0; int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    return QString::number(v, 'f', v < 100 ? 1 : 0) + QLatin1Char(' ') + QLatin1String(units[u]);
}
static QString fmtLeft(qint64 s) {
    if (s < 60) return QStringLiteral("%1 s left").arg(s);
    if (s < 3600) return QStringLiteral("%1 min left").arg((s + 59) / 60);
    return QStringLiteral("%1 h %2 min left").arg(s / 3600).arg((s % 3600) / 60);
}

QString DownloadManager::fileKind(const QString& name) {
    const QString n = name.toLower(); const QString ext = QFileInfo(n).suffix();
    static const QStringList arch{"zip","rar","7z","tar","gz","xz","bz2","tgz","zst","iso","dmg","cab"};
    static const QStringList vid{"mp4","mkv","webm","avi","mov","wmv","flv","m4v"};
    static const QStringList img{"png","jpg","jpeg","gif","webp","svg","bmp","ico","tif","tiff","avif","heic"};
    static const QStringList code{"c","cpp","h","hpp","py","js","ts","json","xml","html","css","java","rs","go","sh","ps1","cs","kt","rb","php","yml","yaml","cmake"};
    static const QStringList aud{"mp3","wav","flac","ogg","m4a","aac","opus","wma"};
    static const QStringList txt{"txt","md","log","csv","rtf","doc","docx","odt"};
    if (arch.contains(ext) || n.endsWith(QStringLiteral(".tar.gz")) || n.endsWith(QStringLiteral(".tar.xz"))) return QStringLiteral("archive");
    if (ext == QStringLiteral("pdf")) return QStringLiteral("pdf");
    if (vid.contains(ext)) return QStringLiteral("video");
    if (img.contains(ext)) return QStringLiteral("image");
    if (code.contains(ext)) return QStringLiteral("code");
    if (aud.contains(ext)) return QStringLiteral("audio");
    if (txt.contains(ext)) return QStringLiteral("text");
    return QString();
}
QString DownloadManager::kindColor(const QString& k) {
    if (k == QStringLiteral("archive")) return QStringLiteral("#F59E0B");
    if (k == QStringLiteral("pdf")) return QStringLiteral("#EF4444");
    if (k == QStringLiteral("video")) return QStringLiteral("#8B5CF6");
    if (k == QStringLiteral("image")) return QStringLiteral("#0EA5E9");
    if (k == QStringLiteral("code")) return QStringLiteral("#22C55E");
    if (k == QStringLiteral("audio")) return QStringLiteral("#EC4899");
    return QStringLiteral("#64748B");
}
// 1.9.0 / 1.8.11 security: the Windows "mark of the web" (Zone.Identifier stream, internet zone),
// as Chrome writes it, so SmartScreen / Office Protected View check the file before it runs.
// Survives the rename of a held dangerous file (NTFS keeps streams on rename). Other systems: no-op
// (macOS quarantine needs the LaunchServices API; the held-file prompt still applies).
void DownloadManager::markFromInternet(const QString& path, const QString& url, const QString& referrer) {
#ifdef Q_OS_WIN
    const QUrl u(url);
    if (u.scheme() == QLatin1String("file") || path.isEmpty()) return;
    auto strip = [](const QString& s){ QUrl x(s); x.setUserInfo({}); x.setQuery(QString()); x.setFragment({}); return x.toString(); };   // no passwords / tokens in the stream
    QByteArray body = "[ZoneTransfer]\r\nZoneId=3\r\n";
    if (!referrer.isEmpty() && !referrer.startsWith(QLatin1String("data:"))) body += "ReferrerUrl=" + strip(referrer).toUtf8() + "\r\n";
    if (u.scheme().startsWith(QLatin1String("http"))) body += "HostUrl=" + strip(url).toUtf8() + "\r\n";
    const std::wstring stream = QDir::toNativeSeparators(path).toStdWString() + L":Zone.Identifier";
    HANDLE h = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // FAT/exFAT drives have no streams: nothing to do
    DWORD written = 0;
    WriteFile(h, body.constData(), DWORD(body.size()), &written, nullptr);
    CloseHandle(h);
#else
    Q_UNUSED(path); Q_UNUSED(url); Q_UNUSED(referrer);
#endif
}

bool DownloadManager::isDangerousName(const QString& name) {
    // 1.9.0 / 1.8.11: more types that run code when opened (Windows, macOS and Linux)
    static const QStringList bad{"exe","msi","msix","msixbundle","appx","appxbundle","msp","bat","cmd","scr","js","jse","vbs","vbe","wsf","wsh",
                                 "com","pif","jar","ps1","psm1","hta","lnk","url","scf","reg","dll","sys","cpl","inf","chm","application",
                                 "dmg","pkg","mpkg","app","command","workflow","sh","run","bin","deb","rpm","appimage","desktop"};
    return bad.contains(QFileInfo(name).suffix().toLower());
}
static QString reasonText(DS::DownloadInterruptReason r) {
    switch (r) {
    case DS::FileNoSpace: return QStringLiteral("No space");
    case DS::FileAccessDenied: return QStringLiteral("Access denied");
    case DS::FileVirusInfected: case DS::FileBlocked: case DS::FileSecurityCheckFailed: return QStringLiteral("Blocked");
    case DS::NetworkFailed: case DS::NetworkTimeout: case DS::NetworkDisconnected: case DS::NetworkServerDown:
    case DS::NetworkInvalidRequest: return QStringLiteral("Network error");
    case DS::ServerUnauthorized: case DS::ServerForbidden: return QStringLiteral("Forbidden");
    case DS::ServerFailed: case DS::ServerBadContent: case DS::ServerCertProblem: case DS::ServerUnreachable: return QStringLiteral("Server error");
    default: return QStringLiteral("File error");
    }
}

// ---------- model ----------
DownloadManager* DownloadManager::instance() { static DownloadManager* m = new DownloadManager(qApp); return m; }

DownloadManager::DownloadManager(QObject* parent) : QObject(parent) {
    QString base = Pacitum::mainProfile()->persistentStoragePath();
    if (base.isEmpty()) base = QDir::currentPath();
    QDir().mkpath(base);
    m_dbPath = QDir(base).filePath(QStringLiteral("pacitum_downloads.db"));
    load();
    auto* t = new QTimer(this); t->setInterval(1000);   // refresh speed/time-left text even when bytes stall
    connect(t, &QTimer::timeout, this, [this]{ for (auto& e : m_entries) if (e.state == DownloadEntry::InProgress) emit entryChanged(e.id); });
    t->start();
}

template <typename F> static void withDb(const QString& path, F f) {
    const QString cn = QStringLiteral("pacitum_downloads");
    {
        QSqlDatabase db = QSqlDatabase::contains(cn) ? QSqlDatabase::database(cn) : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), cn);
        if (!db.isOpen()) { db.setDatabaseName(path); if (!db.open()) return;
            QSqlQuery(db).exec(QStringLiteral("CREATE TABLE IF NOT EXISTS downloads (id INTEGER PRIMARY KEY, url TEXT, page_url TEXT, dir TEXT, name TEXT, "
                                              "hold_name TEXT, received INTEGER, total INTEGER, state INTEGER, reason TEXT, started INTEGER)")); }
        QSqlQuery q(db); f(q);
    }
}

void DownloadManager::load() {
    withDb(m_dbPath, [this](QSqlQuery& q) {
        q.exec(QStringLiteral("SELECT id,url,page_url,dir,name,hold_name,received,total,state,reason,started FROM downloads ORDER BY started DESC, id DESC LIMIT 1000"));
        while (q.next()) {
            DownloadEntry e;
            e.id = q.value(0).toLongLong(); e.url = q.value(1).toString(); e.pageUrl = q.value(2).toString();
            e.dir = q.value(3).toString(); e.fileName = q.value(4).toString(); e.holdName = q.value(5).toString();
            e.received = q.value(6).toLongLong(); e.total = q.value(7).toLongLong();
            e.state = DownloadEntry::State(q.value(8).toInt()); e.reason = q.value(9).toString();
            e.started = QDateTime::fromMSecsSinceEpoch(q.value(10).toLongLong());
            if (e.active()) { e.state = DownloadEntry::Failed; e.reason = QStringLiteral("Interrupted"); }
            m_nextId = qMax(m_nextId, e.id + 1);
            m_entries.append(e);
        }
    });
}
void DownloadManager::save(const DownloadEntry& e) {
    withDb(m_dbPath, [&](QSqlQuery& q) {
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO downloads VALUES (?,?,?,?,?,?,?,?,?,?,?)"));
        for (const QVariant& v : {QVariant(e.id), QVariant(e.url), QVariant(e.pageUrl), QVariant(e.dir), QVariant(e.fileName), QVariant(e.holdName),
                                  QVariant(e.received), QVariant(e.total), QVariant(int(e.state)), QVariant(e.reason), QVariant(e.started.toMSecsSinceEpoch())})
            q.addBindValue(v);
        q.exec();
    });
}
void DownloadManager::dbRemove(qint64 id) {
    withDb(m_dbPath, [&](QSqlQuery& q) { q.prepare(QStringLiteral("DELETE FROM downloads WHERE id=?")); q.addBindValue(id); q.exec(); });
}

DownloadEntry* DownloadManager::find(qint64 id) { for (auto& e : m_entries) if (e.id == id) return &e; return nullptr; }
const DownloadEntry* DownloadManager::entry(qint64 id) const { for (auto& e : m_entries) if (e.id == id) return &e; return nullptr; }
int DownloadManager::activeCount() const { int n = 0; for (auto& e : m_entries) n += e.active(); return n; }
double DownloadManager::aggregateProgress() const {
    qint64 r = 0, t = 0; bool any = false;
    for (auto& e : m_entries) if (e.active()) { any = true; if (e.total > 0) { r += e.received; t += e.total; } }
    if (!any) return -1;
    return t > 0 ? double(r) / t : 0.0;
}

QString DownloadManager::downloadDir() const {
    const QString d = AppSettings::get(QStringLiteral("download_path")).toString();
    if (!d.isEmpty()) return d;
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

void DownloadManager::addDownload(QWebEngineDownloadRequest* dl) {
    DownloadEntry* e = nullptr;
    const QString url = dl->url().toString();
    if (m_pendingRetry.contains(url)) e = find(m_pendingRetry.take(url));
    if (!e) {
        DownloadEntry n; n.id = m_nextId++;
        m_entries.prepend(n); e = &m_entries.first();
    }
    e->url = url;
    e->pageUrl = dl->page() ? dl->page()->url().toString() : QString();
    e->dir = dl->downloadDirectory();
    e->fileName = dl->downloadFileName();
    e->holdName.clear(); e->reason.clear();
    e->received = 0; e->total = dl->totalBytes(); e->samples.clear();
    e->state = DownloadEntry::InProgress;
    e->started = QDateTime::currentDateTime();
    // Potentially dangerous executable from the web: hold it under a temp name until Keep/Delete.
    const QString scheme = dl->url().scheme();
    if (isDangerousName(e->fileName) && scheme != QStringLiteral("file") && scheme != QStringLiteral("data") && scheme != QStringLiteral("blob")) {
        e->holdName = e->fileName + QStringLiteral(".pacitum-hold");
        dl->setDownloadFileName(e->holdName);
    }
    ++m_sessionDownloads;
    const qint64 id = e->id;
    attach(*e, dl);
    if (dl->state() == DS::DownloadRequested) dl->accept();
    save(*find(id));
    emit listChanged();
    emit downloadStarted(id);
}

void DownloadManager::attach(DownloadEntry& e, QWebEngineDownloadRequest* dl) {
    e.req = dl;
    const qint64 id = e.id;
    auto prog = [this, id] {
        auto* x = find(id); if (!x || !x->req) return;
        x->received = x->req->receivedBytes(); x->total = x->req->totalBytes();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        x->samples.append({now, x->received});
        while (x->samples.size() > 2 && now - x->samples.first().first > 3000) x->samples.removeFirst();
        if (now - x->lastEmit > 200) { x->lastEmit = now; emit entryChanged(id); }
    };
    connect(dl, &DS::receivedBytesChanged, this, prog);
    connect(dl, &DS::totalBytesChanged, this, prog);
    connect(dl, &DS::isPausedChanged, this, [this, id] { onState(id); });
    connect(dl, &DS::stateChanged, this, [this, id] { onState(id); });
}

void DownloadManager::onState(qint64 id) {
    auto* e = find(id); if (!e || !e->req) return;
    auto* dl = e->req.data();
    e->received = dl->receivedBytes(); e->total = dl->totalBytes();
    const auto prev = e->state;
    switch (dl->state()) {
    case DS::DownloadRequested:
    case DS::DownloadInProgress: e->state = dl->isPaused() ? DownloadEntry::Paused : DownloadEntry::InProgress; break;
    case DS::DownloadCompleted:
        e->state = e->holdName.isEmpty() ? DownloadEntry::Completed : DownloadEntry::Dangerous;
        if (e->total <= 0) e->total = e->received;
        if (prev != DownloadEntry::Completed && prev != DownloadEntry::Dangerous) markFromInternet(e->filePath(), e->url, e->pageUrl);
        break;
    case DS::DownloadCancelled: e->state = DownloadEntry::Cancelled; e->reason.clear(); break;
    case DS::DownloadInterrupted: {
        const auto r = dl->interruptReason();
        if (r == DS::UserCanceled) { e->state = DownloadEntry::Cancelled; break; }
        e->state = DownloadEntry::Failed; e->reason = reasonText(r); break; }
    }
    if (e->state != DownloadEntry::Paused) e->samples.clear();
    if (prev != e->state) {
        if (e->finished() || e->state == DownloadEntry::Dangerous) {
            if (e->state == DownloadEntry::Cancelled || e->state == DownloadEntry::Failed) {
                if (!e->holdName.isEmpty()) { QFile::remove(e->filePath()); e->holdName.clear(); }
            }
            save(*e);
            emit downloadFinished(id, e->state == DownloadEntry::Completed);
        }
    }
    emit entryChanged(id);
}

void DownloadManager::pause(qint64 id) { if (auto* e = find(id); e && e->req) e->req->pause(); }
void DownloadManager::resume(qint64 id) { if (auto* e = find(id); e && e->req) e->req->resume(); }
void DownloadManager::cancel(qint64 id) {
    auto* e = find(id); if (!e) return;
    if (e->req && e->active()) e->req->cancel();
    else { e->state = DownloadEntry::Cancelled; save(*e); emit entryChanged(id); }
}
void DownloadManager::retry(qint64 id) {
    auto* e = find(id); if (!e || e->url.isEmpty()) return;
    if (e->req && e->state == DownloadEntry::Failed && e->req->state() == DS::DownloadInterrupted) {
        e->req->resume(); return;   // Chromium can resume interrupted downloads
    }
    if (!m_retryPage) m_retryPage = new QWebEnginePage(Pacitum::mainProfile(), this);
    m_pendingRetry.insert(e->url, id);
    m_retryPage->download(QUrl(e->url), e->fileName);
}
void DownloadManager::remove(qint64 id) {
    for (int i = 0; i < m_entries.size(); ++i) if (m_entries[i].id == id) {
        auto& e = m_entries[i];
        if (e.req && e.active()) e.req->cancel();
        if (e.state == DownloadEntry::Dangerous && !e.holdName.isEmpty()) QFile::remove(e.filePath());
        m_entries.removeAt(i); dbRemove(id); emit listChanged(); return;
    }
}
bool DownloadManager::deleteFile(qint64 id) {
    auto* e = find(id); if (!e) return false;
    const bool ok = QFile::remove(e->filePath()) || !QFileInfo::exists(e->filePath());
    if (ok) remove(id);
    return ok;
}
void DownloadManager::keepDangerous(qint64 id) {
    auto* e = find(id); if (!e || e->state != DownloadEntry::Dangerous) return;
    QString target = QDir(e->dir).filePath(e->fileName);
    if (QFileInfo::exists(target)) {   // never overwrite an existing file
        const QFileInfo fi(e->fileName);
        for (int n = 1; QFileInfo::exists(target); ++n)
            target = QDir(e->dir).filePath(QStringLiteral("%1 (%2)%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix()));
    }
    if (QFile::rename(e->filePath(), target)) {
        e->fileName = QFileInfo(target).fileName(); e->holdName.clear(); e->state = DownloadEntry::Completed;
    } else { e->state = DownloadEntry::Failed; e->reason = QStringLiteral("File error"); }
    save(*e); emit entryChanged(id);
}
void DownloadManager::discardDangerous(qint64 id) { remove(id); }
void DownloadManager::open(qint64 id) {
    auto* e = find(id); if (!e || e->state != DownloadEntry::Completed) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(e->filePath()));
}
void DownloadManager::showInFolder(qint64 id) {
    auto* e = find(id); if (!e) return;
    const QString path = e->filePath();
#ifdef Q_OS_WIN
    if (QFileInfo::exists(path)) { QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)}); return; }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
QList<DownloadEntry> DownloadManager::clearFinished() {
    QList<DownloadEntry> removed;
    for (int i = m_entries.size() - 1; i >= 0; --i)
        if (m_entries[i].finished()) { removed.prepend(m_entries[i]); dbRemove(m_entries[i].id); m_entries.removeAt(i); }
    if (!removed.isEmpty()) emit listChanged();
    return removed;
}
void DownloadManager::restore(const QList<DownloadEntry>& list) {
    for (const auto& e : list) { m_entries.append(e); save(e); }
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const DownloadEntry& a, const DownloadEntry& b){ return a.started > b.started; });
    emit listChanged();
}

// ---------- widgets ----------
void ElideLabel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setPen(palette().color(foregroundRole()));
    p.setFont(font());
    const QRect r = contentsRect();
    p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideMiddle, r.width()));
}

QString DownloadsPage::styleSheetFor(bool compact) {
    const auto& p = pal();
    QString s = QStringLiteral(R"(
#DlRoot, #DlScroll, #DlScroll > QWidget > QWidget, #DlColumn { background: @page; }
#DlTitle { font-size: 22px; font-weight: 600; color: @text; }
#DlSearchBox { background: @surface; border-radius: 18px; }
#DlSearch { background: transparent; border: none; color: @text; font-size: 13px; min-height: 34px; padding: 0; }
#DlSecondary { background: @toolbar; color: @text; border: 1px solid @border; border-radius: 8px; height: 32px; min-height: 32px; padding: 0 12px; font-size: 13px; }
#DlSecondary:hover { background: @surface; }
#DlGroup { font-size: 12px; font-weight: 600; color: @sec; letter-spacing: 0.6px; }
#DlCard { background: @toolbar; border: 1px solid @border; border-radius: 12px; }
#DlCard:hover { border-color: @hoverBorder; }
#DlCard:focus { border: 1px solid @accent; }
#DlCard[bad="true"] { background: @badBg; border-color: rgba(239,68,68,0.45); }
#DlRow { background: transparent; border: none; border-radius: 0; }
#DlRow:hover, #DlRow:focus { background: @surface; }
#DlName { color: @text; font-weight: 600; font-size: @nameSize; }
#DlName[struck="true"] { color: @sec; font-weight: 400; }
#DlMeta { color: @sec; font-size: 12px; }
#DlTextBtn { background: transparent; color: @accent; border: none; border-radius: 8px; height: 30px; min-height: 30px; padding: 0 12px; font-weight: 500; }
#DlTextBtn:hover { background: @accentSoft; }
#DlTextBtn[secondary="true"] { color: @sec; }
#DlTextBtn[secondary="true"]:hover { background: @surface; }
#DlDangerBtn { background: @danger; color: #FFFFFF; border: none; border-radius: 8px; height: 30px; min-height: 30px; padding: 0 14px; font-weight: 600; }
#DlIconBtn { background: transparent; border: none; border-radius: 8px; }
#DlIconBtn:hover { background: @surface; }
#DlIconBtn:pressed { background: @surfaceHover; }
QProgressBar#DlBar { background: @surface; border: none; border-radius: 2px; max-height: 4px; min-height: 4px; }
QProgressBar#DlBar::chunk { background: @accent; border-radius: 2px; }
QProgressBar#DlBar[paused="true"]::chunk { background: @sec; }
#DlEmptyTitle { color: @text; font-size: 15px; font-weight: 600; }
#DlEmptySub { color: @sec; font-size: 13px; }
#DlToast { background: @text; border-radius: 10px; }
#DlToast QLabel { color: @toolbar; }
#DlToast QPushButton { background: transparent; border: none; color: @accentOnInverse; font-weight: 600; padding: 0 8px; min-height: 28px; }
#DlPopCard { background: @toolbar; border: 1px solid @border; border-radius: 12px; }
#DlPopHeader { color: @text; font-weight: 600; font-size: 13px; }
#DlPopDivider { background: @border; max-height: 1px; min-height: 1px; }
#DlPopFooter { background: transparent; border: none; color: @accent; font-weight: 600; min-height: 40px; }
#DlPopFooter:hover { background: @surface; border-bottom-left-radius: 12px; border-bottom-right-radius: 12px; }
)");
    const QString badBg = isDark() ? QStringLiteral("#2A2124") : QStringLiteral("#FDF2F2");   // 7% danger over toolbar
    // accent on the inverse (ink) surface: the dark-theme red reads on light ink and vice versa
    QString accInv = isDark() ? QStringLiteral("#D9141F") : QStringLiteral("#FF3B47");
    if (QColor(p.accent).name().compare(QStringLiteral("#D9141F"), Qt::CaseInsensitive) && QColor(p.accent).name().compare(QStringLiteral("#FF3B47"), Qt::CaseInsensitive)) accInv = p.accent;
    const QList<QPair<QString, QString>> vars{
        {"@surfaceHover", p.surfaceHover}, {"@accentSoft", p.accentSoft}, {"@accentOnInverse", accInv}, {"@hoverBorder", rgba(p.textSecondary, 0.4)},
        {"@badBg", badBg}, {"@nameSize", compact ? "13px" : "14px"}, {"@page", p.page}, {"@toolbar", p.toolbar}, {"@surface", p.surface},
        {"@border", p.border}, {"@text", p.text}, {"@sec", p.textSecondary}, {"@accent", p.accent}, {"@danger", p.danger}};
    for (const auto& v : vars) s.replace(v.first, v.second);
    return s;
}

DownloadItemWidget::DownloadItemWidget(qint64 id, bool compact, QWidget* parent) : QFrame(parent), m_id(id), m_compact(compact) {
    setObjectName(compact ? QStringLiteral("DlRow") : QStringLiteral("DlCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(compact ? Qt::NoFocus : Qt::TabFocus);
    if (compact) { setMinimumHeight(60); } else { setMinimumHeight(72); setMaximumWidth(840); }
    auto* h = new QHBoxLayout(this);
    if (compact) { h->setContentsMargins(16, 10, 12, 10); h->setSpacing(12); }
    else { h->setContentsMargins(16, 14, 16, 14); h->setSpacing(16); }
    const int ts = compact ? 32 : 40;
    m_tile = new QLabel(this); m_tile->setFixedSize(ts, ts); m_tile->setAlignment(Qt::AlignCenter);
    h->addWidget(m_tile, 0, Qt::AlignVCenter);
    auto* col = new QVBoxLayout(); col->setContentsMargins(0, 0, 0, 0); col->setSpacing(2);
    m_name = new ElideLabel(this); m_name->setObjectName(QStringLiteral("DlName"));
    col->addWidget(m_name);
    auto* metaRow = new QHBoxLayout(); metaRow->setSpacing(4); metaRow->setContentsMargins(0, 0, 0, 0);
    m_warn = new QLabel(this); m_warn->setFixedSize(14, 14); m_warn->hide();
    m_meta = new QLabel(this); m_meta->setObjectName(QStringLiteral("DlMeta")); m_meta->setTextFormat(Qt::RichText);
    m_meta->setMinimumWidth(20); m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    metaRow->addWidget(m_warn); metaRow->addWidget(m_meta, 1);
    col->addLayout(metaRow);
    m_bar = new QProgressBar(this); m_bar->setObjectName(QStringLiteral("DlBar")); m_bar->setTextVisible(false); m_bar->setRange(0, 1000);
    m_bar->setFixedHeight(4);
    col->addSpacing(6); col->addWidget(m_bar);
    h->addLayout(col, 1);
    if (!compact) h->addSpacing(8);
    m_primary = textBtn(QString(), QStringLiteral("DlTextBtn"));
    m_danger = textBtn(QStringLiteral("Delete"), QStringLiteral("DlDangerBtn"));
    m_b1 = iconBtn(QString()); m_b2 = iconBtn(QString());
    auto* acts = new QHBoxLayout(); acts->setSpacing(compact ? 0 : 4);
    acts->addWidget(m_primary); acts->addWidget(m_danger); acts->addWidget(m_b1); acts->addWidget(m_b2);
    h->addLayout(acts);

    auto* dm = DownloadManager::instance();
    connect(dm, &DownloadManager::entryChanged, this, [this](qint64 x){ if (x == m_id) refresh(); });
    connect(m_primary, &QPushButton::clicked, this, [this]{
        auto* dm = DownloadManager::instance(); const auto* e = dm->entry(m_id); if (!e) return;
        if (e->state == DownloadEntry::Completed) dm->open(m_id);
        else if (e->state == DownloadEntry::Dangerous) confirmKeep();
        else dm->retry(m_id);
    });
    connect(m_danger, &QPushButton::clicked, this, [this]{ DownloadManager::instance()->discardDangerous(m_id); });
    connect(m_b1, &QToolButton::clicked, this, [this]{
        auto* dm = DownloadManager::instance(); const auto* e = dm->entry(m_id); if (!e) return;
        switch (e->state) {
        case DownloadEntry::InProgress: dm->pause(m_id); break;
        case DownloadEntry::Paused: dm->resume(m_id); break;
        case DownloadEntry::Completed: dm->showInFolder(m_id); break;
        case DownloadEntry::Failed: case DownloadEntry::Cancelled: dm->retry(m_id); break;   // compact only
        case DownloadEntry::Dangerous: {
            QMenu m(this);
            m.addAction(QStringLiteral("Keep anyway"), this, [this]{ confirmKeep(); });
            m.addAction(QStringLiteral("Delete"), this, [this]{ DownloadManager::instance()->discardDangerous(m_id); });
            m.exec(m_b1->mapToGlobal(QPoint(0, m_b1->height())));
            break; }
        }
    });
    connect(m_b2, &QToolButton::clicked, this, [this]{
        auto* dm = DownloadManager::instance(); const auto* e = dm->entry(m_id); if (!e) return;
        if (e->active()) dm->cancel(m_id); else dm->remove(m_id);
    });
    refresh();
}

QToolButton* DownloadItemWidget::iconBtn(const QString& tip) {
    auto* b = new QToolButton(this); b->setObjectName(QStringLiteral("DlIconBtn"));
    b->setFixedSize(32, 32); b->setIconSize(QSize(18, 18)); b->setToolTip(tip); b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}
QPushButton* DownloadItemWidget::textBtn(const QString& text, const QString& obj) {
    auto* b = new QPushButton(text, this); b->setObjectName(obj); b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(30); b->setFocusPolicy(Qt::NoFocus);
    return b;
}

void DownloadItemWidget::refresh() {
    const auto* e = DownloadManager::instance()->entry(m_id); if (!e) return;
    const auto& p = pal(); const qreal dpr = devicePixelRatioF();
    const bool bad = e->state == DownloadEntry::Dangerous;
    // file tile
    const QString kind = DownloadManager::fileKind(e->fileName);
    const QString col = bad ? p.danger : DownloadManager::kindColor(kind);
    const QString glyph = bad ? QStringLiteral("download-dangerous") : kind.isEmpty() ? QStringLiteral("file") : QStringLiteral("file-") + kind;
    const int ts = m_compact ? 32 : 40, gs = m_compact ? 18 : 22;
    QPixmap tile(ts * dpr, ts * dpr); tile.setDevicePixelRatio(dpr); tile.fill(Qt::transparent);
    { QPainter tp(&tile); tp.setRenderHint(QPainter::Antialiasing); QColor c(col); c.setAlpha(0x26);
      tp.setPen(Qt::NoPen); tp.setBrush(c); tp.drawRoundedRect(QRectF(0, 0, ts, ts), m_compact ? 8 : 10, m_compact ? 8 : 10);
      tp.drawPixmap(QPointF((ts - gs) / 2.0, (ts - gs) / 2.0), iconPm(glyph, col, gs, dpr)); }
    m_tile->setPixmap(tile);
    // name
    m_name->setText(e->fileName);
    const bool struck = e->state == DownloadEntry::Failed || e->state == DownloadEntry::Cancelled;
    QFont f = m_name->font(); f.setStrikeOut(struck); m_name->setFont(f);
    if (m_name->property("struck").toBool() != struck) { m_name->setProperty("struck", struck); m_name->style()->unpolish(m_name); m_name->style()->polish(m_name); }
    if (property("bad").toBool() != bad) { setProperty("bad", bad); style()->unpolish(this); style()->polish(this); }
    // meta
    auto dang = [&](const QString& t){ return QStringLiteral("<span style=\"color:%1\">%2</span>").arg(p.danger, t.toHtmlEscaped()); };
    QStringList parts;
    if (!m_compact) parts << e->domain().toHtmlEscaped();
    const QString of = DownloadManager::formatSize(e->received) + QStringLiteral(" of ") + DownloadManager::formatSize(e->total);
    m_warn->setVisible(bad && !m_compact);
    switch (e->state) {
    case DownloadEntry::InProgress: {
        parts << (e->total > 0 ? of : DownloadManager::formatSize(e->received));
        if (!m_compact && e->speed() > 0) parts << DownloadManager::formatSize(qint64(e->speed())) + QStringLiteral("/s");
        const qint64 left = e->secondsLeft(); if (left >= 0) parts << fmtLeft(left);
        break; }
    case DownloadEntry::Paused: parts << QStringLiteral("Paused — ") + (e->total > 0 ? of : DownloadManager::formatSize(e->received)); break;
    case DownloadEntry::Completed: parts << DownloadManager::formatSize(e->total > 0 ? e->total : e->received); if (m_compact) parts << QStringLiteral("Done"); break;
    case DownloadEntry::Failed: parts << dang(QStringLiteral("Failed — ") + (e->reason.isEmpty() ? QStringLiteral("Error") : e->reason)); break;
    case DownloadEntry::Cancelled: parts << dang(QStringLiteral("Cancelled")); break;
    case DownloadEntry::Dangerous:
        if (m_compact) parts << dang(QStringLiteral("Blocked · dangerous file"));
        else { parts.clear(); parts << dang(QStringLiteral("Blocked — this file may harm your device"));
               m_warn->setPixmap(iconPm(QStringLiteral("warning"), p.danger, 14, dpr)); }
        break;
    }
    m_meta->setText(parts.join(QStringLiteral(" &nbsp;·&nbsp; ")));
    // progress
    m_bar->setVisible(e->active());
    if (e->active()) {
        if (e->total > 0) { m_bar->setRange(0, 1000); m_bar->setValue(int(e->received * 1000 / qMax<qint64>(1, e->total))); }
        else m_bar->setRange(0, 0);
        const bool paused = e->state == DownloadEntry::Paused;
        if (m_bar->property("paused").toBool() != paused) { m_bar->setProperty("paused", paused); m_bar->style()->unpolish(m_bar); m_bar->style()->polish(m_bar); }
    }
    // actions
    auto setB = [&](QToolButton* b, const QString& icon, const QString& tip) {
        b->setVisible(!icon.isEmpty()); if (icon.isEmpty()) return;
        b->setIcon(cachedIcon(icon, p.textSecondary, 18)); b->setToolTip(tip);
    };
    m_primary->hide(); m_danger->hide();
    m_primary->setProperty("secondary", false);
    switch (e->state) {
    case DownloadEntry::InProgress: setB(m_b1, QStringLiteral("pause"), QStringLiteral("Pause")); setB(m_b2, m_compact ? QString() : QStringLiteral("close"), QStringLiteral("Cancel")); break;
    case DownloadEntry::Paused: setB(m_b1, QStringLiteral("play"), QStringLiteral("Resume")); setB(m_b2, m_compact ? QString() : QStringLiteral("close"), QStringLiteral("Cancel")); break;
    case DownloadEntry::Completed:
        if (!m_compact) { m_primary->setText(QStringLiteral("Open")); m_primary->show(); }
        setB(m_b1, QStringLiteral("folder"), QStringLiteral("Show in folder"));
        setB(m_b2, m_compact ? QString() : QStringLiteral("close"), QStringLiteral("Remove from list")); break;
    case DownloadEntry::Failed: case DownloadEntry::Cancelled:
        if (m_compact) { setB(m_b1, QStringLiteral("reload"), QStringLiteral("Retry")); setB(m_b2, QString(), QString()); }
        else { m_primary->setText(QStringLiteral("Retry")); m_primary->show(); setB(m_b1, QString(), QString()); setB(m_b2, QStringLiteral("close"), QStringLiteral("Remove from list")); }
        break;
    case DownloadEntry::Dangerous:
        if (m_compact) { setB(m_b1, QStringLiteral("more-horizontal"), QStringLiteral("More")); setB(m_b2, QString(), QString()); }
        else { m_primary->setText(QStringLiteral("Keep anyway")); m_primary->setProperty("secondary", true); m_primary->show(); m_danger->show();
               setB(m_b1, QString(), QString()); setB(m_b2, QString(), QString()); }
        break;
    }
    m_primary->style()->unpolish(m_primary); m_primary->style()->polish(m_primary);
    setCursor(e->state == DownloadEntry::Completed ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setToolTip(e->filePath());
}

void DownloadItemWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) { m_pressed = true; m_pressPos = e->pos(); }
    QFrame::mousePressEvent(e);
}
void DownloadItemWidget::mouseMoveEvent(QMouseEvent* ev) {
    const auto* e = DownloadManager::instance()->entry(m_id);
    if (m_pressed && e && e->state == DownloadEntry::Completed && (ev->pos() - m_pressPos).manhattanLength() > QApplication::startDragDistance()) {
        m_pressed = false;
        auto* drag = new QDrag(this); auto* md = new QMimeData; md->setUrls({QUrl::fromLocalFile(e->filePath())});
        drag->setMimeData(md); drag->setPixmap(m_tile->pixmap()); drag->exec(Qt::CopyAction);
        return;
    }
    QFrame::mouseMoveEvent(ev);
}
void DownloadItemWidget::mouseReleaseEvent(QMouseEvent* ev) {
    if (m_pressed && ev->button() == Qt::LeftButton && rect().contains(ev->pos())) DownloadManager::instance()->open(m_id);
    m_pressed = false;
    QFrame::mouseReleaseEvent(ev);
}
void DownloadItemWidget::keyPressEvent(QKeyEvent* ev) {
    auto* dm = DownloadManager::instance(); const auto* e = dm->entry(m_id);
    if (!e) return QFrame::keyPressEvent(ev);
    switch (ev->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: dm->open(m_id); return;
    case Qt::Key_Delete: dm->remove(m_id); return;
    case Qt::Key_Space: if (e->state == DownloadEntry::InProgress) dm->pause(m_id); else if (e->state == DownloadEntry::Paused) dm->resume(m_id); return;
    case Qt::Key_Down: focusNextChild(); return;
    case Qt::Key_Up: focusPreviousChild(); return;
    default: QFrame::keyPressEvent(ev);
    }
}
void DownloadItemWidget::contextMenuEvent(QContextMenuEvent* ev) {
    auto* dm = DownloadManager::instance(); const auto* e = dm->entry(m_id); if (!e) return;
    const bool done = e->state == DownloadEntry::Completed;
    const QString url = e->url, page = e->pageUrl;
    QMenu m(this);
    m.addAction(QStringLiteral("Open"), this, [dm, this]{ dm->open(m_id); })->setEnabled(done);
    m.addAction(QStringLiteral("Show in folder"), this, [dm, this]{ dm->showInFolder(m_id); })->setEnabled(done);
    m.addSeparator();
    m.addAction(QStringLiteral("Copy link"), this, [url]{ QApplication::clipboard()->setText(url); });
    m.addAction(QStringLiteral("Open source page"), this, [this, page]{ emit openSourceRequested(page); })->setEnabled(!page.isEmpty());
    m.addSeparator();
    m.addAction(QStringLiteral("Remove from list"), this, [dm, this]{ dm->remove(m_id); });
    m.addAction(QStringLiteral("Delete file"), this, [this]{ confirmDelete(); })->setEnabled(done);
    m.exec(ev->globalPos());
}
void DownloadItemWidget::confirmDelete() {
    const auto* e = DownloadManager::instance()->entry(m_id); if (!e) return;
    const qint64 id = m_id;
    if (QMessageBox::question(window(), QStringLiteral("Delete file"), QStringLiteral("Delete \"%1\" from disk? This cannot be undone.").arg(e->fileName),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
        if (!DownloadManager::instance()->deleteFile(id)) QMessageBox::warning(nullptr, QStringLiteral("Delete file"), QStringLiteral("The file could not be deleted."));
}
void DownloadItemWidget::confirmKeep() {
    const auto* e = DownloadManager::instance()->entry(m_id); if (!e) return;
    const qint64 id = m_id;
    if (QMessageBox::warning(window(), QStringLiteral("Keep dangerous file?"),
            QStringLiteral("\"%1\" may harm your device. Only keep it if you trust %2.").arg(e->fileName, e->domain()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
        DownloadManager::instance()->keepDangerous(id);
}

static QWidget* emptyState(QWidget* parent, QLabel** iconOut, bool compact) {
    auto* w = new QWidget(parent); auto* l = new QVBoxLayout(w);
    l->setContentsMargins(24, compact ? 32 : 80, 24, compact ? 28 : 40); l->setSpacing(6); l->setAlignment(Qt::AlignHCenter);
    auto* ic = new QLabel(w); ic->setFixedSize(64, 64); ic->setAlignment(Qt::AlignCenter); *iconOut = ic;
    auto* t = new QLabel(QStringLiteral("No downloads yet"), w); t->setObjectName(QStringLiteral("DlEmptyTitle")); t->setAlignment(Qt::AlignCenter);
    auto* s = new QLabel(QStringLiteral("Files you download will appear here."), w); s->setObjectName(QStringLiteral("DlEmptySub")); s->setAlignment(Qt::AlignCenter);
    l->addWidget(ic, 0, Qt::AlignHCenter); l->addSpacing(10); l->addWidget(t); l->addWidget(s);
    return w;
}
static void paintEmptyIcon(QLabel* ic) {
    const auto& p = pal(); const qreal dpr = ic->devicePixelRatioF();
    QPixmap pm(64 * dpr, 64 * dpr); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    QPainter pp(&pm); pp.setRenderHint(QPainter::Antialiasing); pp.setPen(Qt::NoPen);
    QColor c(p.accent); c.setAlphaF(isDark() ? 0.16 : 0.12); pp.setBrush(c); pp.drawEllipse(QRectF(0, 0, 64, 64));
    pp.drawPixmap(QPointF(18, 18), iconPm(QStringLiteral("download"), p.accent, 28, dpr)); pp.end();
    ic->setPixmap(pm);
}

// ---------- page ----------
DownloadsPage::DownloadsPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("DlRoot")); setAttribute(Qt::WA_StyledBackground, true);
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this); scroll->setObjectName(QStringLiteral("DlScroll")); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* host = new QWidget(); host->setObjectName(QStringLiteral("DlColumn"));
    auto* hl = new QHBoxLayout(host); hl->setContentsMargins(24, 28, 24, 40);
    auto* colW = new QWidget(host); colW->setObjectName(QStringLiteral("DlColumn")); colW->setMaximumWidth(840); colW->setMinimumWidth(480);
    hl->addStretch(); hl->addWidget(colW, 100); hl->addStretch();
    auto* col = new QVBoxLayout(colW); col->setContentsMargins(0, 0, 0, 0); col->setSpacing(0);
    // header
    auto* hd = new QHBoxLayout(); hd->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("Downloads"), colW); title->setObjectName(QStringLiteral("DlTitle"));
    hd->addWidget(title, 1);
    auto* sb = new QFrame(colW); sb->setObjectName(QStringLiteral("DlSearchBox")); sb->setFixedSize(300, 36);
    auto* sl = new QHBoxLayout(sb); sl->setContentsMargins(14, 0, 14, 0); sl->setSpacing(8);
    m_searchIcon = new QLabel(sb); m_searchIcon->setFixedSize(16, 16);
    m_search = new QLineEdit(sb); m_search->setObjectName(QStringLiteral("DlSearch")); m_search->setPlaceholderText(QStringLiteral("Search downloads"));
    m_search->setClearButtonEnabled(true);
    sl->addWidget(m_searchIcon); sl->addWidget(m_search, 1);
    hd->addWidget(sb);
    m_folderBtn = new QPushButton(QStringLiteral("Open downloads folder"), colW); m_folderBtn->setObjectName(QStringLiteral("DlSecondary"));
    m_clearBtn = new QPushButton(QStringLiteral("Clear all"), colW); m_clearBtn->setObjectName(QStringLiteral("DlSecondary"));
    for (auto* b : {m_folderBtn, m_clearBtn}) { b->setIconSize(QSize(16, 16)); b->setCursor(Qt::PointingHandCursor); b->setFixedHeight(32); hd->addWidget(b); }
    col->addLayout(hd); col->addSpacing(24 - 20);
    m_listHost = new QWidget(colW); m_listHost->setObjectName(QStringLiteral("DlColumn"));
    m_list = new QVBoxLayout(m_listHost); m_list->setContentsMargins(0, 0, 0, 0); m_list->setSpacing(8);
    col->addWidget(m_listHost);
    m_empty = emptyState(colW, &m_emptyIcon, false);
    col->addWidget(m_empty);
    col->addStretch();
    scroll->setWidget(host);

    connect(m_search, &QLineEdit::textChanged, this, &DownloadsPage::rebuild);
    connect(m_folderBtn, &QPushButton::clicked, this, []{
        const QString d = DownloadManager::instance()->downloadDir(); QDir().mkpath(d); QDesktopServices::openUrl(QUrl::fromLocalFile(d)); });
    connect(m_clearBtn, &QPushButton::clicked, this, [this]{ showUndo(DownloadManager::instance()->clearFinished()); });
    connect(DownloadManager::instance(), &DownloadManager::listChanged, this, &DownloadsPage::rebuild);
    applyTheme();
}

void DownloadsPage::applyTheme() {
    setStyleSheet(styleSheetFor(false));
    const auto& p = pal();
    m_searchIcon->setPixmap(iconPm(QStringLiteral("search"), p.textSecondary, 16, devicePixelRatioF()));
    m_folderBtn->setIcon(cachedIcon(QStringLiteral("folder"), p.textSecondary, 16));
    m_clearBtn->setIcon(cachedIcon(QStringLiteral("trash"), p.textSecondary, 16));
    paintEmptyIcon(m_emptyIcon);
    rebuild();
}

static QString groupLabel(const QDate& d) {
    const QDate today = QDate::currentDate();
    if (d == today) return QStringLiteral("Today");
    if (d == today.addDays(-1)) return QStringLiteral("Yesterday");
    return QLocale(QLocale::English).toString(d, d.year() == today.year() ? QStringLiteral("dddd, MMM d") : QStringLiteral("dddd, MMM d, yyyy"));
}

void DownloadsPage::rebuild() {
    QWidget* focused = QApplication::focusWidget();
    qint64 focusId = -1;
    if (auto* fw = qobject_cast<DownloadItemWidget*>(focused); fw && m_listHost->isAncestorOf(fw)) focusId = fw->id();
    // Entry removed: its card leaves as a fading snapshot (140 ms) at its old place; new entries fade in.
    QSet<qint64> nowIds; for (const auto& e : DownloadManager::instance()->entries()) nowIds.insert(e.id);
    const bool animate = Motion::enabled() && isVisible() && m_listPrimed;
    QHash<qint64, QRect> oldGeo; QHash<qint64, QPixmap> oldShots;
    for (int i = 0; i < m_list->count(); ++i)
        if (auto* c = qobject_cast<DownloadItemWidget*>(m_list->itemAt(i)->widget())) {
            if (animate && !nowIds.contains(c->id()) && c->isVisible()) { oldGeo[c->id()] = c->geometry(); oldShots[c->id()] = c->grab(); }
        }
    const QSet<qint64> before = m_shownIds;
    while (auto* it = m_list->takeAt(0)) { if (auto* w = it->widget()) w->deleteLater(); delete it; }
    for (auto it = oldShots.cbegin(); it != oldShots.cend(); ++it) {
        auto* ghost = new QLabel(m_listHost);
        ghost->setAttribute(Qt::WA_TransparentForMouseEvents);
        ghost->setPixmap(it.value()); ghost->setGeometry(oldGeo.value(it.key())); ghost->show(); ghost->raise();
        auto* eff = new QGraphicsOpacityEffect(ghost); ghost->setGraphicsEffect(eff);
        auto* a = new QPropertyAnimation(eff, "opacity", ghost);
        a->setDuration(Motion::ms(Motion::Quick)); a->setStartValue(1.0); a->setEndValue(0.0); a->setEasingCurve(QEasingCurve::InCubic);
        connect(a, &QPropertyAnimation::finished, ghost, &QObject::deleteLater);
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
    m_shownIds.clear();
    const QString q = m_search->text().trimmed();
    QDate last; int shown = 0;
    for (const auto& e : DownloadManager::instance()->entries()) {
        if (!q.isEmpty() && !e.fileName.contains(q, Qt::CaseInsensitive) && !e.domain().contains(q, Qt::CaseInsensitive)) continue;
        const QDate d = e.started.date();
        if (d != last || shown == 0) {
            last = d;
            auto* g = new QLabel(groupLabel(d).toUpper(), m_listHost); g->setObjectName(QStringLiteral("DlGroup"));
            g->setContentsMargins(4, 20 - 8 + (shown ? 4 : 0), 4, 0);   // 20 top / 8 bottom with the list's 8 spacing
            m_list->addWidget(g);
        }
        auto* card = new DownloadItemWidget(e.id, false, m_listHost);
        connect(card, &DownloadItemWidget::openSourceRequested, this, &DownloadsPage::openUrlRequested);
        m_list->addWidget(card);
        m_shownIds.insert(e.id);
        if (animate && !before.contains(e.id)) Motion::fadeIn(card, Motion::Medium);
        if (e.id == focusId) card->setFocus();
        ++shown;
    }
    m_listPrimed = true;
    m_empty->setVisible(shown == 0);
    m_empty->findChild<QLabel*>(QStringLiteral("DlEmptyTitle"))->setText(q.isEmpty() || DownloadManager::instance()->entries().isEmpty() ? QStringLiteral("No downloads yet") : QStringLiteral("No matching downloads"));
    bool anyFinished = false; for (const auto& e : DownloadManager::instance()->entries()) anyFinished |= e.finished();
    m_clearBtn->setEnabled(anyFinished);
}

void DownloadsPage::showUndo(const QList<DownloadEntry>& removed) {
    if (removed.isEmpty()) return;
    m_undo = removed;
    if (!m_toast) {
        m_toast = new QFrame(this); m_toast->setObjectName(QStringLiteral("DlToast"));
        auto* l = new QHBoxLayout(m_toast); l->setContentsMargins(16, 6, 8, 6); l->setSpacing(12);
        auto* t = new QLabel(m_toast); t->setObjectName(QStringLiteral("DlToastText")); l->addWidget(t);
        auto* u = new QPushButton(QStringLiteral("Undo"), m_toast); u->setCursor(Qt::PointingHandCursor); l->addWidget(u);
        connect(u, &QPushButton::clicked, this, [this]{ DownloadManager::instance()->restore(m_undo); m_undo.clear(); Motion::toast(m_toast, false); });
        m_toastTimer = new QTimer(this); m_toastTimer->setSingleShot(true); m_toastTimer->setInterval(5000);
        connect(m_toastTimer, &QTimer::timeout, this, [this]{ m_undo.clear(); Motion::toast(m_toast, false); });
    }
    m_toast->findChild<QLabel*>(QStringLiteral("DlToastText"))->setText(QStringLiteral("Cleared %1 download%2").arg(removed.size()).arg(removed.size() == 1 ? "" : "s"));
    m_toast->adjustSize();
    m_toast->move((width() - m_toast->width()) / 2, height() - m_toast->height() - 24);
    m_toast->raise(); Motion::toast(m_toast, true); m_toastTimer->start();
}

// ---------- popup ----------
DownloadsPopup::DownloadsPopup(QWidget* parent) : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(16, 4, 16, 28);   // room for the shadow
    m_card = new QFrame(this); m_card->setObjectName(QStringLiteral("DlPopCard")); m_card->setFixedWidth(380);
    auto* sh = new QGraphicsDropShadowEffect(m_card); sh->setBlurRadius(32); sh->setOffset(0, 12); m_card->setGraphicsEffect(sh);
    outer->addWidget(m_card);
    auto* v = new QVBoxLayout(m_card); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
    auto* hdr = new QWidget(m_card); hdr->setFixedHeight(44);
    auto* hl = new QHBoxLayout(hdr); hl->setContentsMargins(16, 0, 8, 0); hl->setSpacing(2);
    auto* t = new QLabel(QStringLiteral("Downloads"), hdr); t->setObjectName(QStringLiteral("DlPopHeader")); hl->addWidget(t, 1);
    m_folder = new QToolButton(hdr); m_more = new QToolButton(hdr);
    for (auto* b : {m_folder, m_more}) { b->setObjectName(QStringLiteral("DlIconBtn")); b->setFixedSize(32, 32); b->setIconSize(QSize(18, 18)); b->setCursor(Qt::PointingHandCursor); hl->addWidget(b); }
    m_folder->setToolTip(QStringLiteral("Open downloads folder")); m_more->setToolTip(QStringLiteral("More"));
    v->addWidget(hdr);
    auto* div = new QFrame(m_card); div->setObjectName(QStringLiteral("DlPopDivider")); v->addWidget(div);
    auto* listW = new QWidget(m_card); m_list = new QVBoxLayout(listW); m_list->setContentsMargins(0, 4, 0, 4); m_list->setSpacing(0);
    v->addWidget(listW);
    m_empty = emptyState(m_card, &m_emptyIcon, true); v->addWidget(m_empty);
    auto* div2 = new QFrame(m_card); div2->setObjectName(QStringLiteral("DlPopDivider")); v->addWidget(div2);
    m_footer = new QPushButton(QStringLiteral("Show all downloads"), m_card); m_footer->setObjectName(QStringLiteral("DlPopFooter")); m_footer->setCursor(Qt::PointingHandCursor);
    v->addWidget(m_footer);

    m_autoClose = new QTimer(this); m_autoClose->setSingleShot(true); m_autoClose->setInterval(6000);
    connect(m_autoClose, &QTimer::timeout, this, [this]{ if (!m_hovered && !underMouse()) hide(); });
    connect(m_folder, &QToolButton::clicked, this, [this]{ hide(); const QString d = DownloadManager::instance()->downloadDir(); QDir().mkpath(d); QDesktopServices::openUrl(QUrl::fromLocalFile(d)); });
    connect(m_more, &QToolButton::clicked, this, [this]{
        QMenu m(this);
        m.addAction(cachedIcon(QStringLiteral("trash"), pal().textSecondary, 16), QStringLiteral("Clear all"), this, []{ DownloadManager::instance()->clearFinished(); });
        m.addAction(cachedIcon(QStringLiteral("settings"), pal().textSecondary, 16), QStringLiteral("Settings"), this, [this]{ hide(); emit settingsRequested(); });
        m.exec(m_more->mapToGlobal(QPoint(0, m_more->height())));
    });
    connect(m_footer, &QPushButton::clicked, this, [this]{ hide(); emit showAllRequested(); });
    connect(DownloadManager::instance(), &DownloadManager::listChanged, this, [this]{ if (isVisible()) { rebuild(); adjustSize(); } });
    applyTheme();
}

void DownloadsPopup::applyTheme() {
    setStyleSheet(DownloadsPage::styleSheetFor(true));
    const auto& p = pal();
    if (auto* sh = qobject_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect()))
        sh->setColor(isDark() ? QColor(0, 0, 0, 128) : QColor(16, 24, 40, 46));
    m_folder->setIcon(cachedIcon(QStringLiteral("folder"), p.textSecondary, 18));
    m_more->setIcon(cachedIcon(QStringLiteral("more-horizontal"), p.textSecondary, 18));
    paintEmptyIcon(m_emptyIcon);
    if (isVisible()) rebuild();
}

void DownloadsPopup::rebuild() {
    while (auto* it = m_list->takeAt(0)) { if (auto* w = it->widget()) w->deleteLater(); delete it; }
    const auto& all = DownloadManager::instance()->entries();
    int n = 0;
    for (const auto& e : all) {
        if (n++ >= 5) break;
        auto* row = new DownloadItemWidget(e.id, true, m_list->parentWidget());
        connect(row, &DownloadItemWidget::openSourceRequested, this, [this](const QString& u){ hide(); emit openUrlRequested(u); });
        m_list->addWidget(row);
    }
    m_list->parentWidget()->setVisible(n > 0);
    m_empty->setVisible(n == 0);
    m_footer->setText(n == 0 ? QStringLiteral("Open downloads page") : QStringLiteral("Show all downloads"));
}

void DownloadsPopup::showUnder(QWidget* button, bool autoClose) {
    m_button = button; m_hovered = false;
    rebuild(); adjustSize();
    const QPoint br = button->mapToGlobal(QPoint(button->width(), button->height()));
    const QMargins m = layout()->contentsMargins();
    move(br.x() + 4 - (width() - m.right()), br.y() + 8 - m.top());
    show(); raise();
    if (autoClose) m_autoClose->start(); else m_autoClose->stop();
    if (auto* ab = qobject_cast<QAbstractButton*>(button)) ab->setDown(true);
}
void DownloadsPopup::enterEvent(QEnterEvent* e) { m_hovered = true; m_autoClose->stop(); QWidget::enterEvent(e); }
void DownloadsPopup::hideEvent(QHideEvent* e) { m_hiddenAt = QDateTime::currentMSecsSinceEpoch(); if (auto* ab = qobject_cast<QAbstractButton*>(m_button.data())) ab->setDown(false); m_autoClose->stop(); QWidget::hideEvent(e); }

// ---------- toolbar badge (animations-spec §4) ----------
static qreal ez(QEasingCurve::Type t, qreal x, qreal overshoot = -1) {
    QEasingCurve c(t); if (overshoot > 0) c.setOvershoot(overshoot);
    return c.valueForProgress(qBound<qreal>(0.0, x, 1.0));
}
DownloadButtonBadge::DownloadButtonBadge(QWidget* button) : QWidget(button) {
    setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_NoSystemBackground);
    setGeometry(button->rect());
    m_ring.setEasingCurve(QEasingCurve::Linear);
    connect(&m_ring, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_shown = v.toReal(); update(); });
    m_spin.setStartValue(0.0); m_spin.setEndValue(360.0); m_spin.setDuration(1000); m_spin.setLoopCount(-1);
    connect(&m_spin, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_spinAngle = v.toReal(); update(); });
    m_seq.setStartValue(0.0); m_seq.setEndValue(1700.0); m_seq.setDuration(1700);
    connect(&m_seq, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        m_t = v.toReal();
        if (m_failed) { update(); return; }
        // arrow out 0–120 InCubic; back in (cross-fade) 1500–1700 OutCubic
        if (m_t < 1500) { const qreal k = 1.0 - ez(QEasingCurve::InCubic, m_t / 120.0); setGlyph(k, k); }
        else { const qreal k = ez(QEasingCurve::OutCubic, (m_t - 1500) / 200.0); setGlyph(1.0, k); }
        if (m_t >= 200) m_badge = ez(QEasingCurve::OutBack, (m_t - 200) / 160.0);
        update(); });
    connect(&m_seq, &QVariantAnimation::finished, this, [this]{ m_t = -1; setGlyph(1, 1); m_active = false; m_failed = false; update(); });
    m_badgeAnim.setEasingCurve(QEasingCurve::InCubic);
    connect(&m_badgeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_badge = v.toReal(); update(); });
    m_ringFade.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_ringFade, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_ringOpacity = v.toReal(); update(); });
    auto* dm = DownloadManager::instance();
    connect(dm, &DownloadManager::entryChanged, this, [this]{ onProgress(); });
    connect(dm, &DownloadManager::listChanged, this, [this]{
        bool alert = false;
        for (const auto& e : DownloadManager::instance()->entries())
            if (e.started.date() == QDate::currentDate() && e.state == DownloadEntry::Dangerous) alert = true;
        if (alert && !m_alert) { m_alert = true; m_badgeDanger = true; m_badge = 1; }
        onProgress(); });
    connect(dm, &DownloadManager::downloadStarted, this, [this]{
        // a new download during the completion sequence: arrow back, ring fades in (150 ms)
        if (m_seq.state() == QAbstractAnimation::Running) { m_seq.stop(); m_t = -1; setGlyph(1, 1); }
        m_failed = false; m_active = true;
        if (Pacitum::reducedMotion()) m_ringOpacity = 1;
        else { m_ringFade.stop(); m_ringFade.setDuration(150); m_ringFade.setStartValue(0.0); m_ringFade.setEndValue(1.0); m_ringFade.start(); }
        m_shown = 0; onProgress(); });
    connect(dm, &DownloadManager::downloadFinished, this, [this](qint64, bool ok){
        if (DownloadManager::instance()->activeCount() == 0) onAllFinished(ok); else onProgress(); });
}
void DownloadButtonBadge::setGlyph(qreal scale, qreal opacity) {
    if (auto* b = qobject_cast<AnimatedToolButton*>(parentWidget())) b->setGlyphTransform(scale, opacity);
}
void DownloadButtonBadge::onProgress() {
    const double prog = DownloadManager::instance()->aggregateProgress();   // -1 = nothing active
    if (prog < 0) { m_spin.stop(); update(); return; }
    m_active = true;
    const bool indet = prog == 0.0 && DownloadManager::instance()->activeCount() > 0;
    bool unknown = indet;
    if (indet) for (const auto& e : DownloadManager::instance()->entries()) if (e.active() && e.total > 0) unknown = false;
    if (unknown) { if (m_spin.state() != QAbstractAnimation::Running && !Pacitum::reducedMotion()) m_spin.start(); update(); return; }
    m_spin.stop();
    if (prog <= m_shown) return;   // never backwards
    if (Pacitum::reducedMotion()) { m_shown = prog; update(); return; }
    m_ring.stop(); m_ring.setDuration(200); m_ring.setStartValue(m_shown); m_ring.setEndValue(prog); m_ring.start();
}
void DownloadButtonBadge::onAllFinished(bool ok) {
    m_spin.stop(); m_ring.stop();
    m_failed = !ok; m_badgeDanger = !ok;
    if (ok) m_shown = 1.0;
    if (Pacitum::reducedMotion()) { m_active = false; m_badge = 1; m_t = -1; update(); return; }   // swap to badge instantly
    m_seq.stop(); m_seq.setStartValue(0.0); m_seq.setEndValue(ok ? 1700.0 : 600.0); m_seq.setDuration(ok ? 1700 : 600);
    if (!ok) m_badge = 1;
    m_seq.start();
}
void DownloadButtonBadge::clearAlert() {
    m_alert = false;
    if (m_badge <= 0) { update(); return; }
    if (Pacitum::reducedMotion()) { m_badge = 0; update(); return; }
    m_badgeAnim.stop(); m_badgeAnim.setDuration(120); m_badgeAnim.setStartValue(m_badge); m_badgeAnim.setEndValue(0.0); m_badgeAnim.start();
}
void DownloadButtonBadge::paintEvent(QPaintEvent*) {
    if (parentWidget() && size() != parentWidget()->size()) setGeometry(parentWidget()->rect());
    const auto& p = pal();
    QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
    const QPointF c(width() / 2.0, height() / 2.0);
    const QRectF ring(c.x() - 14, c.y() - 14, 28, 28);   // 2 px stroke, inset 3 px in the 34 px circle
    // ring: visible while active; fades 200 ms OutCubic at completion (failed: danger 400 ms then fade)
    qreal ringOp = m_ringOpacity;
    if (m_t >= 0) ringOp *= m_failed ? (m_t < 400 ? 1.0 : 1.0 - ez(QEasingCurve::OutCubic, (m_t - 400) / 200.0))
                                     : 1.0 - ez(QEasingCurve::OutCubic, m_t / 200.0);
    const bool showRing = (m_active || m_t >= 0) && ringOp > 0.01;
    if (showRing) {
        g.setOpacity(ringOp);
        g.setPen(QPen(QColor(p.surface), 2)); g.setBrush(Qt::NoBrush); g.drawEllipse(ring);
        QPen pen(QColor(m_failed ? p.danger : p.accent), 2); pen.setCapStyle(Qt::RoundCap); g.setPen(pen);
        if (m_spin.state() == QAbstractAnimation::Running) g.drawArc(ring, int((90 - m_spinAngle) * 16), -90 * 16);
        else if (m_failed) g.drawEllipse(ring);
        else g.drawArc(ring, 90 * 16, -int(qMax(0.02, m_shown) * 360 * 16));
        g.setOpacity(1);
    }
    // check glyph pops in at 120 ms (200 ms OutBack), cross-fades back to the arrow at 1500 ms
    if (m_t >= 120 && !m_failed) {
        const qreal k = ez(QEasingCurve::OutBack, (m_t - 120) / 200.0);
        const qreal op = qMin<qreal>(1.0, (m_t - 120) / 200.0) * (m_t < 1500 ? 1.0 : 1.0 - ez(QEasingCurve::OutCubic, (m_t - 1500) / 200.0));
        const QPixmap pm = cachedIcon(QStringLiteral("check"), p.accent, 18).pixmap(QSize(18, 18), devicePixelRatioF());
        g.save(); g.setOpacity(op); g.setRenderHint(QPainter::SmoothPixmapTransform);
        g.translate(c); g.scale(k, k); g.translate(-c);
        g.drawPixmap(QRectF(c.x() - 9, c.y() - 9, 18, 18), pm, QRectF(pm.rect()));
        g.restore();
    }
    // badge dot: 8 px accent (danger when failed/dangerous), 2 px chrome ring, top-right
    if (m_badge > 0.001) {
        const QPointF bc(c.x() + 9, c.y() - 9); const qreal r = 4 * m_badge;
        g.setPen(QPen(QColor(p.chrome()), 2 * m_badge)); g.setBrush(QColor(m_badgeDanger ? p.danger : p.accent));
        g.drawEllipse(bc, r, r);
    }
}
