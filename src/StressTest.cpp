// --stress-test (CI): exercise the paths that crashed or misbehaved in 1.5.1 -- many tabs opened
// and closed (also while loading and while their close animation runs), fast tab switching,
// F11 and website fullscreen (also entered from maximized, and with the fullscreen tab switched
// away from or closed), frozen background tabs being shown/closed, history writes from the UI
// thread while a worker imports, and rapid settings writes. Exits 0 with "STRESS OK" in
// startup.log; any crash is caught by the crash handler (crash.log, non-zero exit).
#include "BrowserWindow.h"
#include "BrowserTab.h"
#include "TabWidget.h"
#include "HistoryManager.h"
#include "AppSettings.h"
#include <QApplication>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QtConcurrent/QtConcurrentRun>
#include <QRandomGenerator>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include <QPointer>
#include <QMouseEvent>
#include <QUrl>
#include "AddressBar.h"

void pacitumLog(const QString& line);

void BrowserWindow::runStressTest() {
    struct State { int step = 0; int opened = 0; int closed = 0; int fs = 0; int pageFsReq = 0; int frozen = 0; int pageFsEntered = 0; int stuck = 0; QElapsedTimer t; QFuture<int> import; QPointer<BrowserTab> fsTab; };
    auto st = std::make_shared<State>();
    st->t.start();
    const QStringList urls{
        QStringLiteral("about:blank"),
        QStringLiteral("pacitum://newtab"),
        QStringLiteral("https://example.com/"),
        QStringLiteral("data:text/html,<title>timers</title><body><canvas id=c width=400 height=300></canvas><script>"
                       "let n=0;setInterval(()=>{document.title='t'+(n++)},16);"
                       "(function f(){const g=c.getContext('2d');g.fillStyle='hsl('+(n%360)+',60%,50%)';g.fillRect(0,0,400,300);requestAnimationFrame(f)})();"
                       "</script>"),
        QStringLiteral("data:text/html,<title>video</title><video id=v autoplay muted loop style='width:100%'></video>"),
    };
    connect(this, &QObject::destroyed, qApp, []{});
    // count website-fullscreen requests that actually reached the window
    auto hookTab = [this, st](BrowserTab* t){ if (t) connect(t, &BrowserTab::fullScreenRequested, this, [st]{ st->pageFsReq++; }); };

    // background history import racing with UI-thread addVisit()
    QVector<QPair<QString, QString>> items;
    for (int i = 0; i < 3000; ++i) items.push_back({QStringLiteral("Imported %1").arg(i), QStringLiteral("https://import.example/%1").arg(i)});
    HistoryManager* hm = m_history;
    st->import = QtConcurrent::run([hm, items]{ return hm->importVisits(items); });

    auto* timer = new QTimer(this);
    timer->setInterval(40);
    connect(timer, &QTimer::timeout, this, [this, st, urls, timer, hookTab]{
        auto* rng = QRandomGenerator::global();
        const int s = st->step++;
        const int op = s % 12;
        auto randomTab = [this, rng]() -> int { return m_tabs->count() ? int(rng->bounded(m_tabs->count())) : -1; };
        m_history->addVisit(QStringLiteral("https://stress.example/%1").arg(s), QStringLiteral("stress"));
        if (s % 7 == 0) AppSettings::set(QStringLiteral("stress_counter"), s);
        switch (op) {
        case 0: case 1: case 2:
            newTab(urls[s % urls.size()]); st->opened++; hookTab(activeTab()); break;
        case 3: if (m_tabs->count() > 3) { closeTab(randomTab()); st->closed++; } break;
        case 4: if (m_tabs->count()) m_tabs->setCurrentIndex(randomTab()); break;
        case 5: toggleFullscreen(); st->fs++; break;                                   // F11 on/off
        case 6: {                                                                        // website fullscreen
            // requestFullscreen needs a user gesture: open a page that goes fullscreen on click and
            // deliver a real (synthetic Qt) click to the web view. The random ops pause meanwhile.
            if (s % 24 != 6) break;
            timer->stop();
            if (s % 48 == 6 && !isMaximized()) showMaximized();                          // fullscreen entered from maximized
            newTab(QStringLiteral("data:text/html,<title>fs</title><body style='margin:0;background:%23123'><div style='height:100vh'>click</div>"
                                  "<script>document.onclick=()=>{document.fullscreenElement?document.exitFullscreen():document.documentElement.requestFullscreen()}</script>"));
            st->opened++; hookTab(activeTab());
            QPointer<BrowserTab> ft = activeTab();
            QTimer::singleShot(900, this, [this, ft, st, s, timer]{
                if (ft && ft == activeTab() && ft->view() && ft->view()->focusProxy()) {
                    QWidget* target = ft->view()->focusProxy();
                    const QPointF pos(target->width() / 2.0, target->height() / 2.0), gp = target->mapToGlobal(pos.toPoint());
                    QMouseEvent press(QEvent::MouseButtonPress, pos, gp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, pos, gp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(target, &press); QApplication::sendEvent(target, &release);
                }
                QTimer::singleShot(700, this, [this, ft, st, s, timer]{
                    if (m_pageFullscreen) st->pageFsEntered++;
                    const int how = (s / 24) % 4;                                            // leave fullscreen 4 different ways
                    if (how == 0) handleEscape();
                    else if (how == 1 && m_tabs->count() > 1) nextTab();
                    else if (how == 2 && ft && m_tabs->indexOf(ft) >= 0 && m_tabs->count() > 2) { closeTab(m_tabs->indexOf(ft)); st->closed++; }
                    else toggleFullscreen();
                    QTimer::singleShot(700, this, [this, st, timer]{ if (m_chromeHidden) st->stuck++; timer->start(); });
                });
            });
            break;
        }
        case 7: if (m_chromeHidden && m_tabs->count() > 2) nextTab(); else if (!m_chromeHidden) isMaximized() ? showNormal() : showMaximized(); break;
        case 8: if (m_chromeHidden && m_pageFullscreen && m_tabs->count() > 3) closeTab(m_tabs->currentIndex()); else handleEscape(); break;
        case 9:  // freeze a background tab right away, then show or close it
            for (int i = 0; i < m_tabs->count(); ++i)
                if (auto* t = qobject_cast<BrowserTab*>(m_tabs->widget(i)); t && !t->isVisible() && t->view()
                    && t->view()->page()->lifecycleState() == QWebEnginePage::LifecycleState::Active && !t->isLoading()) {
                    t->view()->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen); st->frozen++;
                    if (s % 2) m_tabs->setCurrentIndex(i); else if (m_tabs->count() > 3) { closeTab(i); st->closed++; }
                    break;
                }
            break;
        case 10: if (auto* t = activeTab()) { t->reload(); if (s % 3 == 0) t->goBack(); } break;
        case 11: if (m_tabs->count() > 3) { closeTab(m_tabs->count() - 1); closeTab(m_tabs->count() - 2); st->closed += 2; } break;
        }
        if (s >= 360) {
            timer->stop();
            if (m_chromeHidden) finishExitFullscreen();
            st->import.waitForFinished();
            // 1.6.5: the address bar must show the active tab's URL, never a background tab's.
            if (auto* t = activeTab(); t && !t->currentUrl().isEmpty() && m_addressBar->currentUrl() != t->currentUrl()) {
                pacitumLog(QStringLiteral("STRESS FAIL address bar '%1' while active tab is '%2'").arg(m_addressBar->currentUrl().left(80), t->currentUrl().left(80)));
                QCoreApplication::exit(6); return;
            }
            // 1.6: the home page (pacitum://newtab) must render and its script must run.
            newTab(QStringLiteral("pacitum://newtab"));
            QPointer<BrowserTab> home = activeTab();
            auto ntp = std::make_shared<int>(-1);
            QTimer::singleShot(2500, this, [home, ntp]{
                if (home && home->view()) home->view()->page()->runJavaScript(
                    QStringLiteral("(typeof S==='object'&&!!document.getElementById('q')&&!!document.getElementById('cz')&&document.getElementById('clock').textContent.length>0)?1:0"),
                    [ntp](const QVariant& v){ *ntp = v.toInt(); });
            });
            QTimer::singleShot(4000, this, [this, st, ntp]{
                pacitumLog(QStringLiteral("HOME %1").arg(*ntp == 1 ? QStringLiteral("OK") : QStringLiteral("FAIL")));
                if (*ntp != 1) { pacitumLog(QStringLiteral("STRESS FAIL home page")); QCoreApplication::exit(5); return; }
                pacitumLog(QStringLiteral("STRESS OK steps=%1 opened=%2 closed=%3 tabsLeft=%4 f11=%5 pageFullscreenRequests=%6 frozen=%7 imported=%8 history=%9 in %10 ms pageFullscreenEntered=%11 stuckInFullscreen=%12")
                           .arg(st->step).arg(st->opened).arg(st->closed).arg(m_tabs->count()).arg(st->fs).arg(st->pageFsReq)
                           .arg(st->frozen).arg(st->import.result()).arg(m_history->count()).arg(st->t.elapsed()).arg(st->pageFsEntered).arg(st->stuck));
                QCoreApplication::exit(0);
            });
        }
    });
    timer->start();
}
