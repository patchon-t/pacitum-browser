#include "PageTest.h"
#include "BrowserWindow.h"
#include "BrowserTab.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <memory>

void pacitumLog(const QString& msg);

namespace PageTest {

int colorCount(const QImage& src) {
    if (src.isNull()) return 0;
    const QImage img = src.scaled(96, 60, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
    QSet<QRgb> seen;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) seen.insert(line[x] & 0xF8F8F8);   // ignore the lowest bits (dithering)
    }
    return int(seen.size());
}

namespace {
struct Case { QString url; bool soft; };
struct State {
    QPointer<BrowserWindow> w;
    QList<Case> cases;
    int at = -1, failed = 0, softFailed = 0;
    QElapsedTimer clock;
    QMetaObject::Connection conn;
    std::shared_ptr<bool> pending;   // this page still waits for its result
};

// A page made of coloured blocks and text: it needs no network, so it tells "Pacitum can't draw pages"
// apart from "the internet is slow".
QString localPage() {
    return QStringLiteral("data:text/html;charset=utf-8,"
        "<title>Pacitum page test</title><body style='margin:0;font:20px sans-serif'>"
        "<div style='display:flex;height:50vh'><div style='flex:1;background:%23d9141f'></div><div style='flex:1;background:%231f7a57'></div>"
        "<div style='flex:1;background:%231d4ed8'></div><div style='flex:1;background:%23f59e0b'></div></div>"
        "<h1 style='padding:20px'>Pacitum draws pages</h1><p style='padding:0 20px'>ก ข ค · 日本語 · Español</p></body>");
}

void next(const std::shared_ptr<State>& st);

void finish(const std::shared_ptr<State>& st, bool ok, const QString& why) {
    if (!st->pending || !*st->pending) return;
    *st->pending = false;
    QObject::disconnect(st->conn);
    const Case& c = st->cases[st->at];
    BrowserTab* t = st->w ? st->w->currentTab() : nullptr;
    const int colors = t ? colorCount(t->captureView()) : 0;
    const QString title = t ? t->currentTitle() : QString();
    // drew something: more than a few colours (a blank page has 1-2)
    const bool drew = colors >= 4;
    const bool pass = ok && drew;
    const QString label = pass ? QStringLiteral("OK") : c.soft ? QStringLiteral("SOFT-FAIL") : QStringLiteral("FAIL");
    pacitumLog(QStringLiteral("PAGE %1 %2 load=%3 colors=%4 title=\"%5\" %6 ms%7")
                   .arg(label, c.url.startsWith(QLatin1String("data:")) ? QStringLiteral("(built-in test page)") : c.url)
                   .arg(ok).arg(colors).arg(title.left(60)).arg(st->clock.elapsed())
                   .arg(why.isEmpty() ? QString() : QStringLiteral(" (") + why + QLatin1Char(')')));
    if (!pass) { if (c.soft) ++st->softFailed; else ++st->failed; }
    QTimer::singleShot(300, qApp, [st]{ next(st); });
}

void next(const std::shared_ptr<State>& st) {
    if (!st->w) { QCoreApplication::exit(5); return; }
    if (++st->at >= st->cases.size()) {
        pacitumLog(QStringLiteral("PAGE TEST %1: %2 of %3 required pages failed, %4 soft failures")
                       .arg(st->failed ? QStringLiteral("FAILED") : QStringLiteral("PASSED"))
                       .arg(st->failed).arg(st->cases.size()).arg(st->softFailed));
        QCoreApplication::exit(st->failed ? 5 : 0);
        return;
    }
    const Case c = st->cases[st->at];
    st->pending = std::make_shared<bool>(true);
    st->clock.restart();
    st->w->openUrlInNewTab(c.url);
    BrowserTab* t = st->w->currentTab();
    if (!t) { finish(st, false, QStringLiteral("no tab")); return; }
    auto pending = st->pending;
    // loaded: give the page a moment to paint, then look at it
    st->conn = QObject::connect(t, &BrowserTab::loadDone, st->w, [st, pending](bool ok){
        if (!*pending) return;
        QTimer::singleShot(2500, qApp, [st, pending, ok]{ if (*pending) finish(st, ok, QString()); });
    });
    QTimer::singleShot(c.soft ? 25000 : 40000, qApp, [st, pending]{ if (*pending) finish(st, false, QStringLiteral("timed out")); });
}
}

void run(BrowserWindow* w) {
    auto st = std::make_shared<State>();
    st->w = w;
    st->cases = {
        {QStringLiteral("pacitum://newtab"), false},
        {localPage(), false},
        {QStringLiteral("https://example.com/"), false},
        {QStringLiteral("https://en.wikipedia.org/wiki/Web_browser"), false},
        {QStringLiteral("https://www.youtube.com/"), true},
        {QStringLiteral("https://github.com/"), true},
        {QStringLiteral("https://www.google.com/search?q=pacitum"), true},
    };
    pacitumLog(QStringLiteral("page test: %1 pages").arg(st->cases.size()));
    next(st);
}

}
