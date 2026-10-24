#pragma once
// 1.8.9 update flow in the browser (design: "1.8.9 Updater" boards).
//
//  1. Update offered: a card slides in under the toolbar (Update / Later). Later leaves a small
//     Update pill in the toolbar.
//  2. Downloading: one ring with the percentage. "Show details" turns the centre into
//     "78 MB / of 120 MB" and adds one line: speed · time left · check. Hide moves the progress
//     into the toolbar pill (a mini ring); Cancel stops it.
//  3. Ready: the tick draws itself; nothing installs until Restart now.
//  4. Installing: the browser window shrinks and fades away; the pop-up lifts and is handed to the
//     install helper (UpdateOverlay.h), which keeps it on screen until the new version runs.
//  5. Update done: the new version shows bursting rings, the version number zooming in and
//     "What's new" from the GitHub release.
#include "Updater.h"
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QWidget>

namespace UpdUi { class Sheet; struct Colors; class Hero; }
class QLabel;
class QPushButton;
class QVariantAnimation;

// Toolbar pill: "Update" (after Later), a mini progress ring (download hidden), "Restart" (ready).
class UpdatePill : public QAbstractButton {
    Q_OBJECT
public:
    enum Mode { Hidden, Available, Downloading, Ready };
    explicit UpdatePill(QWidget* parent = nullptr);
    void setMode(Mode m);
    void setProgress(double frac) { m_frac = frac; update(); }
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; update(); }
private:
    QString label() const;
    Mode m_mode = Hidden;
    double m_frac = 0, m_appear = 1;
    bool m_hover = false;
    QVariantAnimation* m_anim = nullptr;
};

// "Pacitum X is available" card under the toolbar.
class UpdateCard : public QWidget {
    Q_OBJECT
public:
    UpdateCard(const UpdateInfo& info, QWidget* window, QWidget* anchor);
    void slideIn();
    void slideOut();
signals:
    void updateClicked();
    void laterClicked();
protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void place();
    QPointer<QWidget> m_anchor;
    bool m_leaving = false;
    QPointer<QVariantAnimation> m_anim;   // slide-in or slide-out in progress
};

class UpdateFlow : public QObject {
    Q_OBJECT
public:
    static UpdateFlow* instance();
    // Each browser window registers its toolbar pill; anchor = the widget the card hangs under.
    void attach(QWidget* window, UpdatePill* pill, QWidget* anchor);
    void offer(const UpdateInfo& info, QWidget* window, bool userInitiated);
    void afterStartup(QWidget* window);   // first window of a run: finish a hand-off, show "Update done"
    // CI smoke test: offer a dummy update, click "Update" on the card like a user, and check the
    // download pop-up opens (1.8.9/1.8.10 crashed right there). Logs "UPDATE UI OK|FAIL".
    void runSelfTest(QWidget* window);

    enum State { Idle, Offered, Downloading, Ready, Failed, Installing };
    State state() const { return m_state; }

private:
    explicit UpdateFlow(QObject* parent);
    UpdUi::Colors colors() const;
    QWidget* hostWindow(QWidget* preferred = nullptr) const;
    void setPills(UpdatePill::Mode m);
    void startDownload(QWidget* window);
    void showSheet(QWidget* window);     // (re)open the pop-up for the current state
    void fillDownloading();
    void fillReady(UpdUi::Sheet* s);
    void fillFailed(const QString& error);
    void fillInstalling(UpdUi::Sheet* s);
    void refreshProgress();
    void hideSheet();
    void install();
    void retryFresh(QWidget* window);    // "Try again": check the release again, then download
    void showDone(QWidget* window, const QJsonObject& pending);

    State m_state = Idle;
    bool m_retryFresh = false;
    QPointer<QWidget> m_retryWindow;
    UpdateInfo m_info;
    QString m_error;
    QList<QPointer<UpdatePill>> m_pills;
    QPointer<UpdUi::Sheet> m_sheet;
    QPointer<UpdateCard> m_card;
    bool m_details = false, m_cancelled = false, m_startupDone = false;
    qint64 m_received = 0, m_total = -1;
    double m_speed = 0;                    // bytes/s, smoothed
    qint64 m_lastBytes = 0, m_lastMs = 0;
    QElapsedTimer m_clock;
};
