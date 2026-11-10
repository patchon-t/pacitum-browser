#pragma once
#include <QVariantAnimation>
#include <QSet>
#include <QObject>
#include <QWidget>
#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QDateTime>
#include <QList>
#include <QVector>
#include <QPair>
#include <QHash>
#include <QWebEngineDownloadRequest>

class QVBoxLayout;
class QLineEdit;
class QProgressBar;
class QToolButton;
class QPushButton;
class QTimer;
class QWebEnginePage;

// Downloads (design-spec §9). One app-wide model backed by QWebEngineDownloadRequest,
// history persisted in SQLite next to the profile; a full page (pacitum://downloads)
// and a toolbar popup render it.

struct DownloadEntry {
    enum State { InProgress, Paused, Completed, Failed, Cancelled, Dangerous };
    qint64 id = 0;
    QString url, pageUrl, dir, fileName, holdName;   // holdName: temp name while a dangerous file is held
    qint64 received = 0, total = -1;
    State state = InProgress;
    QString reason;
    QDateTime started;
    QPointer<QWebEngineDownloadRequest> req;
    QVector<QPair<qint64, qint64>> samples;  // (msecs since epoch, bytes) for 3 s moving average
    qint64 lastEmit = 0;
    bool offTheRecord = false;   // 2.0.8: from a private tab: shown this session, never written to disk

    QString filePath() const;
    QString domain() const;
    bool finished() const { return state != InProgress && state != Paused && state != Dangerous; }
    bool active() const { return state == InProgress || state == Paused; }
    double speed() const;          // bytes/s, 3 s moving average
    qint64 secondsLeft() const;    // -1 if unknown or within the first 2 s
};

class DownloadManager : public QObject {
    Q_OBJECT
public:
    static void markFromInternet(const QString& path, const QString& url, const QString& referrer);   // Windows mark of the web
    static DownloadManager* instance();
    static QString formatSize(qint64 bytes);
    static QString fileKind(const QString& name);           // archive/pdf/video/image/code/audio/text
    static QString kindColor(const QString& kind);
    static bool isDangerousName(const QString& name);

    void addDownload(QWebEngineDownloadRequest* dl);
    const QList<DownloadEntry>& entries() const { return m_entries; }   // newest first
    const DownloadEntry* entry(qint64 id) const;
    int activeCount() const;
    double aggregateProgress() const;    // 0..1, or -1 when nothing active
    bool hadDownloadThisSession() const { return m_sessionDownloads > 0; }

    void pause(qint64 id);
    void resume(qint64 id);
    void cancel(qint64 id);
    void retry(qint64 id);
    void remove(qint64 id);
    bool deleteFile(qint64 id);          // caller confirms first
    void keepDangerous(qint64 id);       // caller confirms first
    void discardDangerous(qint64 id);
    void open(qint64 id);
    void showInFolder(qint64 id);
    QList<DownloadEntry> clearFinished();               // returns removed entries (for Undo)
    void restore(const QList<DownloadEntry>& list);
    QString downloadDir() const;

signals:
    void entryChanged(qint64 id);
    void listChanged();
    void downloadStarted(qint64 id);
    void downloadFinished(qint64 id, bool ok);

private:
    explicit DownloadManager(QObject* parent = nullptr);
    DownloadEntry* find(qint64 id);
    void attach(DownloadEntry& e, QWebEngineDownloadRequest* dl);
    void onState(qint64 id);
    void save(const DownloadEntry& e);
    void dbRemove(qint64 id);
    void load();
    QString m_dbPath;
    QList<DownloadEntry> m_entries;
    QHash<QString, qint64> m_pendingRetry;  // url → entry id
    QWebEnginePage* m_retryPage = nullptr;
    qint64 m_nextId = 1;
    int m_sessionDownloads = 0;
};

// Label that middle-elides its text (file names).
class ElideLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent*) override;
};

// One download row; compact = toolbar popup row, otherwise a page card.
class DownloadItemWidget : public QFrame {
    Q_OBJECT
public:
    DownloadItemWidget(qint64 id, bool compact, QWidget* parent = nullptr);
    qint64 id() const { return m_id; }
    void refresh();
signals:
    void openSourceRequested(const QString& url);
protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
private:
    QToolButton* iconBtn(const QString& tip);
    QPushButton* textBtn(const QString& text, const QString& obj);
    void confirmDelete();
    void confirmKeep();
    qint64 m_id; bool m_compact;
    QLabel* m_tile; ElideLabel* m_name; QLabel* m_warn; QLabel* m_meta; QProgressBar* m_bar;
    QPushButton* m_primary; QPushButton* m_danger; QToolButton* m_b1; QToolButton* m_b2;
    QPoint m_pressPos; bool m_pressed = false;
};

class DownloadsPage : public QWidget {
    Q_OBJECT
public:
    explicit DownloadsPage(QWidget* parent = nullptr);
    void applyTheme();
    static QString styleSheetFor(bool compact);
signals:
    void openUrlRequested(const QString& url);
private:
    void rebuild();
    void showUndo(const QList<DownloadEntry>& removed);
    QLineEdit* m_search; QVBoxLayout* m_list; QWidget* m_listHost; QWidget* m_empty;
    QSet<qint64> m_shownIds; bool m_listPrimed = false;   // entry add/remove animation
    QPushButton* m_folderBtn; QPushButton* m_clearBtn; QLabel* m_searchIcon; QLabel* m_emptyIcon;
    QFrame* m_toast = nullptr; QTimer* m_toastTimer = nullptr; QList<DownloadEntry> m_undo;
};

class DownloadsPopup : public QWidget {
    Q_OBJECT
public:
    explicit DownloadsPopup(QWidget* parent = nullptr);
    void showUnder(QWidget* button, bool autoClose);
    void applyTheme();
    qint64 hiddenAt() const { return m_hiddenAt; }
signals:
    void showAllRequested();
    void settingsRequested();
    void openUrlRequested(const QString& url);
protected:
    void enterEvent(QEnterEvent* e) override;
    void hideEvent(QHideEvent* e) override;
private:
    void rebuild();
    QFrame* m_card; QVBoxLayout* m_list; QToolButton* m_folder; QToolButton* m_more; QPushButton* m_footer;
    QWidget* m_empty; QLabel* m_emptyIcon; QTimer* m_autoClose; bool m_hovered = false;
    QPointer<QWidget> m_button; qint64 m_hiddenAt = 0;
};

// Overlay for the toolbar button: progress ring, completion check, danger dot.
// Toolbar download button decoration (animations-spec §4): progress ring tween, indeterminate arc,
// completion sequence (arrow out → check pop → badge dot), failure tint, badge until the popup opens.
class DownloadButtonBadge : public QWidget {
    Q_OBJECT
public:
    explicit DownloadButtonBadge(QWidget* button);
    void clearAlert();   // popup opened: badge scales out (120 ms InCubic)
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void onProgress();
    void onAllFinished(bool ok);
    void setGlyph(qreal scale, qreal opacity);
    QVariantAnimation m_ring, m_spin, m_seq, m_badgeAnim, m_ringFade;
    qreal m_shown = 0, m_spinAngle = 0, m_t = -1, m_badge = 0, m_ringOpacity = 1;
    bool m_active = false, m_failed = false, m_badgeDanger = false, m_alert = false;
};
