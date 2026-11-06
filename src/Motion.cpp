#include "Motion.h"
#include "ThemeManager.h"
#include <QApplication>
#include <QEvent>
#include <QPaintEvent>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QSequentialAnimationGroup>
#include <QLabel>
#include <QPropertyAnimation>
#include <QScreen>
#include <QVariantAnimation>
#include <QWidget>
#include <QTimer>
#include <QGuiApplication>
#include <QRadialGradient>
#include <cmath>
#include <QAbstractAnimation>
#include <QElapsedTimer>

namespace {
// Ticks at the fastest connected screen's refresh rate on a precise timer. Qt's own driver uses a ~16 ms
// coarse timer, which on Windows lands on a 15.6 ms grid: frames are uneven at 60 Hz and capped at 60 fps on
// faster screens.
class RefreshRateDriver : public QAnimationDriver {
public:
    explicit RefreshRateDriver(int forcedHz, QObject* parent) : QAnimationDriver(parent), m_forced(forcedHz) {
        m_tick.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_tick, &QTimer::timeout, this, [this]{ advance(); });
        auto rescan = [this]{ updateRate(); };
        QObject::connect(qApp, &QGuiApplication::screenAdded, this, rescan);
        QObject::connect(qApp, &QGuiApplication::screenRemoved, this, rescan);
        for (QScreen* s : QGuiApplication::screens()) QObject::connect(s, &QScreen::refreshRateChanged, this, rescan);
        QObject::connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* s){
            QObject::connect(s, &QScreen::refreshRateChanged, this, [this]{ updateRate(); });
        });
        updateRate();
    }
    qint64 elapsed() const override { return m_clock.isValid() ? m_clock.elapsed() : 0; }
    int hz() const { return m_hz; }
protected:
    void start() override { m_clock.start(); m_tick.start(); QAnimationDriver::start(); }
    void stop() override { m_tick.stop(); QAnimationDriver::stop(); }
private:
    void updateRate() {
        qreal hz = m_forced > 0 ? m_forced : 60.0;
        if (m_forced <= 0) for (QScreen* s : QGuiApplication::screens()) hz = qMax(hz, s->refreshRate());
        m_hz = int(qBound(60.0, hz, 240.0));
        m_tick.setInterval(qMax(1, int(1000.0 / m_hz)));
    }
    QTimer m_tick;
    QElapsedTimer m_clock;
    int m_forced = 0, m_hz = 60;
};
}

void pacitumLog(const QString& line);   // main.cpp (startup.log)

namespace Motion {
void installRefreshRateClock() {
    bool set = false;
    const int forced = qEnvironmentVariableIntValue("PACITUM_ANIMATION_HZ", &set);
    if (set && forced == 0) return;   // Qt's own driver
    static RefreshRateDriver* d = nullptr;
    if (d) return;
    d = new RefreshRateDriver(set ? forced : 0, qApp);
    d->install();
    pacitumLog(QStringLiteral("animations: %1 Hz clock").arg(d->hz()));
}

int ms(int token) { return Pacitum::motionMs(token); }
// Opacity-only fades survive reduced motion, capped at 120 ms (shared kit: reduced = fade 120, no movement).
int fadeMs(int token) { return Pacitum::reducedMotion() ? qMin(token, 120) : token; }
bool targetVisible(QWidget* w) { const QVariant v = w->property("_pmTarget"); return v.isValid() ? v.toBool() : w->isVisible(); }
bool enabled() { return !Pacitum::reducedMotion(); }

void fadeIn(QWidget* w, int token) {
    if (!w || fadeMs(token) <= 0 || w->graphicsEffect()) return;
    auto* eff = new QGraphicsOpacityEffect(w); eff->setOpacity(0.0); w->setGraphicsEffect(eff);
    auto* a = new QPropertyAnimation(eff, "opacity", w);
    a->setDuration(fadeMs(token)); a->setStartValue(0.0); a->setEndValue(1.0); a->setEasingCurve(QEasingCurve::OutCubic);
    QPointer<QWidget> g(w);
    QObject::connect(a, &QPropertyAnimation::finished, w, [g]{ if (g) g->setGraphicsEffect(nullptr); });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

static int restMax(QWidget* w) { const QVariant v = w->property("_pmMax"); return v.isValid() ? v.toInt() : QWIDGETSIZE_MAX; }

void slideVisible(QWidget* w, bool show, int target, int token) {
    if (!w) return;
    w->setProperty("_pmTarget", show);
    if (!w->property("_pmMin").isValid()) w->setProperty("_pmMin", w->minimumHeight());
    if (!w->property("_pmMax").isValid() && w->maximumHeight() < QWIDGETSIZE_MAX && w->maximumHeight() > 0) w->setProperty("_pmMax", w->maximumHeight());
    if (auto* old = w->findChild<QPropertyAnimation*>(QStringLiteral("_pmSlide"), Qt::FindDirectChildrenOnly)) { old->stop(); old->deleteLater(); }
    if (target <= 0) { const int mx = w->maximumHeight(); w->setMaximumHeight(QWIDGETSIZE_MAX); target = qMax(1, w->sizeHint().height()); w->setMaximumHeight(mx); }
    const int d = ms(token);
    if (d <= 0 || !w->parentWidget() || !w->window()->isVisible()) {
        w->setMaximumHeight(restMax(w)); w->setMinimumHeight(w->property("_pmMin").toInt());
        w->setVisible(show);
        return;
    }
    if (show && w->isVisible() && w->maximumHeight() >= target) return;
    if (!show && !w->isVisible()) return;
    const int from = show ? (w->isVisible() ? qMin(w->maximumHeight(), target) : 0) : qMin(w->height(), target);
    w->setMinimumHeight(0);
    w->setMaximumHeight(from);
    if (show) w->show();
    auto* a = new QPropertyAnimation(w, "maximumHeight", w);
    a->setObjectName(QStringLiteral("_pmSlide"));
    a->setDuration(d); a->setEasingCurve(show ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    a->setStartValue(from); a->setEndValue(show ? target : 0);
    QPointer<QWidget> g(w);
    QObject::connect(a, &QPropertyAnimation::finished, w, [g, show, target]{
        if (!g) return;
        if (!show) g->hide();
        g->setMaximumHeight(restMax(g));   // never leave a clamp behind (resize safety)
        g->setMinimumHeight(g->property("_pmMin").toInt());
        Q_UNUSED(target);
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void pop(QWidget* w) {
    if (!w || w->graphicsEffect()) return;
    auto* eff = new QGraphicsOpacityEffect(w); eff->setOpacity(0.4); w->setGraphicsEffect(eff);
    auto* a = new QPropertyAnimation(eff, "opacity", w);
    a->setDuration(fadeMs(Medium)); a->setStartValue(0.4); a->setEndValue(1.0); a->setEasingCurve(QEasingCurve::OutCubic);
    QPointer<QWidget> g(w);
    QObject::connect(a, &QPropertyAnimation::finished, w, [g]{ if (g) g->setGraphicsEffect(nullptr); });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

namespace {
class Ghost : public QWidget {
public:
    Ghost(const QPixmap& pm, const QRect& geo, qreal scaleTo)
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_pm(pm), m_scaleTo(scaleTo) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(geo);
    }
    qreal t = 0.0;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setOpacity(1.0 - t);
        const qreal s = 1.0 + (m_scaleTo - 1.0) * t;
        const QPointF c = QRectF(rect()).center();
        p.translate(c); p.scale(s, s); p.translate(-c);
        p.drawPixmap(rect(), m_pm);
    }
private:
    QPixmap m_pm; qreal m_scaleTo;
};
}

void ghostOut(QWidget* w, int token, qreal scaleTo) {
    const int d = fadeMs(token);
    if (!enabled()) scaleTo = 1.0;
    if (!w || d <= 0 || QCoreApplication::closingDown() || w->width() < 2 || w->height() < 2) return;
    if (w->width() * w->height() > 1400 * 1000) return;   // never ghost something window-sized
    const QPixmap pm = w->grab();
    if (pm.isNull()) return;
    auto* g = new Ghost(pm, w->geometry(), scaleTo);
    g->show();
    auto* a = new QVariantAnimation(g);
    a->setDuration(d); a->setStartValue(0.0); a->setEndValue(1.0); a->setEasingCurve(QEasingCurve::InCubic);
    QObject::connect(a, &QVariantAnimation::valueChanged, g, [g](const QVariant& v){ g->t = v.toReal(); g->update(); });
    QObject::connect(a, &QVariantAnimation::finished, g, &QWidget::close);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

namespace {
class HoverLift : public QObject {
public:
    HoverLift(QWidget* w, int dy) : QObject(w), m_w(w), m_dy(dy) {}
    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() == QEvent::Enter) go(true);
        else if (e->type() == QEvent::Leave || e->type() == QEvent::Hide) go(false);
        else if (e->type() == QEvent::Move && !m_anim) m_base = m_w->pos() - QPoint(0, m_lifted ? m_dy : 0);
        return false;
    }
private:
    void go(bool up) {
        if (up == m_lifted) return;
        if (!m_anim) m_base = m_w->pos() - QPoint(0, m_lifted ? m_dy : 0);
        m_lifted = up;
        const QPoint end = m_base + QPoint(0, up ? m_dy : 0);
        if (m_anim) { m_anim->stop(); m_anim->deleteLater(); }
        if (!enabled()) { m_w->move(end); return; }
        m_anim = new QPropertyAnimation(m_w, "pos", this);
        m_anim->setDuration(ms(Base)); m_anim->setEasingCurve(QEasingCurve::OutCubic);
        m_anim->setStartValue(m_w->pos()); m_anim->setEndValue(end);
        connect(m_anim, &QPropertyAnimation::finished, this, [this]{ if (m_anim) { m_anim->deleteLater(); m_anim = nullptr; } });
        m_anim->start();
    }
    QWidget* m_w; int m_dy; bool m_lifted = false; QPoint m_base; QPointer<QPropertyAnimation> m_anim;
};
}
void installHoverLift(QWidget* w, int dy) {
    if (!w) return;
    w->installEventFilter(new HoverLift(w, dy));
}

namespace {
// Transient pill (zoom percentage, fullscreen hint). A child of the window that re-centres itself
// on every resize, is click-through, and deletes itself after the sequence.
class Bubble : public QLabel {
public:
    Bubble(QWidget* host, int topY) : QLabel(host), m_top(topY) {
        setObjectName(QStringLiteral("PacitumBubble"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAlignment(Qt::AlignCenter);
        host->installEventFilter(this);
    }
    void place() { adjustSize(); move((parentWidget()->width() - width()) / 2, m_top); }
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == parentWidget() && e->type() == QEvent::Resize) place();
        return false;
    }
    int m_top;
};
}
void bubble(QWidget* host, const QString& text, int holdMs, int topY) {
    if (!host || !host->isVisible()) return;
    QLabel* b = host->findChild<QLabel*>(QStringLiteral("PacitumBubble"), Qt::FindDirectChildrenOnly);
    if (b) { for (auto* a : b->findChildren<QAbstractAnimation*>()) a->stop(); b->deleteLater(); }
    auto* nb = new Bubble(host, topY); b = nb;
    const auto& p = ThemeManager::current();
    b->setStyleSheet(QStringLiteral("QLabel{background:%1;color:%2;border:1px solid %3;border-radius:16px;padding:6px 16px;font-weight:600;}")
                         .arg(p.card(), p.text, p.border));
    nb->setText(text); nb->place(); nb->show(); nb->raise();
    auto* eff = new QGraphicsOpacityEffect(b); eff->setOpacity(0.0); b->setGraphicsEffect(eff);
    auto* seq = new QSequentialAnimationGroup(b);
    auto* in = new QPropertyAnimation(eff, "opacity"); in->setDuration(qMax(1, fadeMs(Base))); in->setStartValue(0.0); in->setEndValue(1.0); in->setEasingCurve(QEasingCurve::OutCubic);
    auto* out = new QPropertyAnimation(eff, "opacity"); out->setDuration(qMax(1, fadeMs(Quick))); out->setStartValue(1.0); out->setEndValue(0.0); out->setEasingCurve(QEasingCurve::InCubic);
    seq->addAnimation(in); seq->addPause(holdMs); seq->addAnimation(out);
    QObject::connect(seq, &QAbstractAnimation::finished, b, &QObject::deleteLater);
    seq->start(QAbstractAnimation::DeleteWhenStopped);
}

// Toast (kit #26): rises 16 px + fades in 240 OutCubic; leaves 8 px down + fade 180 InCubic.
void toast(QWidget* w, bool show) {
    if (!w) return;
    for (auto* a : w->findChildren<QAbstractAnimation*>(QStringLiteral("_pmToast"))) { a->stop(); a->deleteLater(); }
    if (show) w->show(); else if (!w->isVisible()) return;
    const int d = fadeMs(show ? Enter : Exit);
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect());
    if (d <= 0) { w->setGraphicsEffect(nullptr); w->setVisible(show); return; }
    if (!eff) { eff = new QGraphicsOpacityEffect(w); eff->setOpacity(show ? 0.0 : 1.0); w->setGraphicsEffect(eff); }
    auto* g = new QParallelAnimationGroup(w); g->setObjectName(QStringLiteral("_pmToast"));
    auto* op = new QPropertyAnimation(eff, "opacity"); op->setDuration(d); op->setStartValue(eff->opacity()); op->setEndValue(show ? 1.0 : 0.0);
    op->setEasingCurve(show ? QEasingCurve::OutCubic : QEasingCurve::InCubic); g->addAnimation(op);
    const QPoint rest = w->pos();
    if (enabled()) {
        auto* mv = new QPropertyAnimation(w, "pos"); mv->setDuration(d);
        mv->setStartValue(show ? rest + QPoint(0, 16) : rest); mv->setEndValue(show ? rest : rest + QPoint(0, 8));
        mv->setEasingCurve(show ? QEasingCurve::OutCubic : QEasingCurve::InCubic); g->addAnimation(mv);
    }
    QPointer<QWidget> gw(w);
    QObject::connect(g, &QAbstractAnimation::finished, w, [gw, show, rest]{
        if (!gw) return;
        gw->setGraphicsEffect(nullptr);
        if (!show) { gw->hide(); gw->move(rest); }
    });
    g->start(QAbstractAnimation::DeleteWhenStopped);
}

FadeOverlay::FadeOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        const qreal t = v.toReal();
        m_t = t;
        m_opacity = m_from + (m_to - m_from) * t;
        if (!m_drift.isNull()) move(m_origin + QPoint(qRound(m_drift.x() * t), qRound(m_drift.y() * t)));
        update();
    });
    connect(&m_anim, &QVariantAnimation::finished, this, [this] {
        m_tilt = 0.0; m_scaleTo = 1.0; m_t = 0.0;
        if (m_deleteWhenDone) { hide(); deleteLater(); return; }
        if (m_to <= 0.0) { hide(); m_pm = QPixmap(); }   // release the snapshot right away
    });
}
void FadeOverlay::fade(qreal from, qreal to, int ms, QEasingCurve::Type curve, int delayMs, QPoint drift, bool deleteWhenDone) {
    m_anim.stop();
    m_from = from; m_to = to; m_drift = drift; m_origin = pos(); m_deleteWhenDone = deleteWhenDone;
    m_opacity = from; m_t = 0.0;
    show(); raise(); update();
    m_anim.setDuration(qMax(1, ms)); m_anim.setStartValue(0.0); m_anim.setEndValue(1.0); m_anim.setEasingCurve(curve);
    if (delayMs <= 0) { m_anim.start(); return; }
    m_pendingStart = true;
    QPointer<FadeOverlay> g(this);
    QTimer::singleShot(delayMs, this, [g] { if (g && g->m_pendingStart) { g->m_pendingStart = false; g->m_anim.start(); } });
}
void FadeOverlay::stop() { m_pendingStart = false; m_anim.stop(); hide(); m_pm = QPixmap(); }
void FadeOverlay::paintEvent(QPaintEvent* e) {
    if (m_opacity <= 0.0) return;
    QPainter p(this);
    p.setOpacity(m_opacity);
    if (!m_pm.isNull() && (m_tilt != 0.0 || m_scaleTo != 1.0)) {
        // perspective turn around the vertical centre line; bilinear sampling off mid-flight (fast)
        const QPointF c = QRectF(rect()).center();
        const qreal s = 1.0 + (m_scaleTo - 1.0) * m_t;
        QTransform tr;
        tr.translate(c.x(), c.y());
        tr.rotate(m_tilt * m_t, Qt::YAxis);
        tr.scale(s, s);
        tr.translate(-c.x(), -c.y());
        p.setTransform(tr);
        p.drawPixmap(rect(), m_pm);
        return;
    }
    if (!m_pm.isNull()) p.drawPixmap(rect(), m_pm);
    else p.fillRect(e->rect(), m_color);
}


// ============================================================================ 1.9.0 motion
namespace {
// Snapshot that zooms from an origin point (pop-in) or flies between two rects (window ghost).
class ZoomGhost : public QWidget {
public:
    ZoomGhost(const QPixmap& pm, const QRect& geo)
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_pm(pm) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(geo);
    }
    // pop-in: scale around `origin` (local), opacity
    QPointF origin; qreal scale = 1.0, opacity = 1.0;
    // fly: draw into this local rect instead (when valid)
    QRectF target;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setOpacity(opacity);
        if (target.isValid()) { p.drawPixmap(target, m_pm, QRectF(m_pm.rect())); return; }
        p.translate(origin); p.scale(scale, scale); p.translate(-origin);
        p.drawPixmap(rect(), m_pm);
    }
private:
    QPixmap m_pm;
};

bool onWayland() { return QGuiApplication::platformName().startsWith(QLatin1String("wayland")); }

// Page-colour cover with a growing round hole (soft edge), for a new window.
class RevealOverlay : public QWidget {
public:
    RevealOverlay(QWidget* window, const QColor& c) : QWidget(window), m_c(c) {
        setObjectName(QStringLiteral("PacitumWindowOverlay"));   // BrowserWindow::resizeEvent keeps it full size
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(window->rect());
        raise(); show();
    }
    qreal t = 0.0;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c = QRectF(rect()).center();
        const qreal maxR = std::hypot(width() / 2.0, height() / 2.0) + 40;
        const qreal r = maxR * t;
        QRadialGradient g(c, qMax<qreal>(1, r));
        QColor solid = m_c; solid.setAlphaF(qBound<qreal>(0, 1.0 - t * t, 1));   // the cover also fades
        QColor clear = m_c; clear.setAlphaF(0);
        g.setColorAt(0.0, clear); g.setColorAt(0.82, clear); g.setColorAt(1.0, solid);
        p.fillRect(rect(), r < 1 ? QBrush(solid) : QBrush(g));
    }
private:
    QColor m_c;
};
}

bool popIn(QWidget* w, const QPoint& originGlobal, qreal fromScale, int token) {
    const int d = ms(token);
    if (!w || d <= 0 || onWayland() || QCoreApplication::closingDown()) return false;
    if (w->width() < 4 || w->height() < 4 || w->width() * w->height() > 1400 * 1000) return false;
    const QPixmap pm = w->grab();
    if (pm.isNull()) return false;
    auto* g = new ZoomGhost(pm, w->geometry());
    const QPoint local = originGlobal - w->geometry().topLeft();
    g->origin = QPointF(qBound(0, local.x(), w->width()), qBound(0, local.y(), w->height()));
    g->scale = fromScale; g->opacity = 0.0;
    w->setWindowOpacity(0.0);
    g->show();
    QPointer<QWidget> guard(w);
    auto* a = new QVariantAnimation(g);
    a->setDuration(d); a->setStartValue(0.0); a->setEndValue(1.0);
    QObject::connect(a, &QVariantAnimation::valueChanged, g, [g, fromScale, guard](const QVariant& v){
        if (!guard || !guard->isVisible()) { g->close(); return; }
        const qreal t = v.toReal();
        QEasingCurve back(QEasingCurve::OutBack); back.setOvershoot(1.15);
        g->scale = fromScale + (1.0 - fromScale) * back.valueForProgress(t);
        g->opacity = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(qMin<qreal>(1.0, t * 1.6));
        g->update();
    });
    QObject::connect(a, &QVariantAnimation::finished, g, [g, guard]{
        if (guard) guard->setWindowOpacity(1.0);   // the real window takes over in the same frame
        g->close();
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
    return true;
}

bool windowGhostsAllowed() {
#ifdef Q_OS_WIN
    return false;   // DWM already animates open / close / minimize / maximize of our window
#else
    return enabled() && !onWayland() && !QCoreApplication::closingDown();
#endif
}

void windowGhost(const QPixmap& snapshot, const QRect& from, const QRect& to, int msDur, qreal fadeTo) {
    if (snapshot.isNull() || !from.isValid() || !to.isValid() || msDur <= 0) return;
    // one window covering both rects; the snapshot is drawn into the moving rect
    const QRect area = from.united(to);
    QPixmap pm = snapshot;
    if (pm.width() > 1600) pm = pm.scaledToWidth(1600, Qt::SmoothTransformation);   // cheap to repaint
    auto* g = new ZoomGhost(pm, area);
    g->target = QRectF(from.translated(-area.topLeft()));
    g->show();
    auto* a = new QVariantAnimation(g);
    a->setDuration(msDur); a->setStartValue(0.0); a->setEndValue(1.0); a->setEasingCurve(QEasingCurve::InOutCubic);
    const QRectF f(from.translated(-area.topLeft())), t(to.translated(-area.topLeft()));
    QObject::connect(a, &QVariantAnimation::valueChanged, g, [g, f, t, fadeTo](const QVariant& v){
        const qreal k = v.toReal();
        g->target = QRectF(f.x() + (t.x() - f.x()) * k, f.y() + (t.y() - f.y()) * k,
                           f.width() + (t.width() - f.width()) * k, f.height() + (t.height() - f.height()) * k);
        g->opacity = 1.0 + (fadeTo - 1.0) * k;
        g->update();
    });
    QObject::connect(a, &QVariantAnimation::finished, g, &QWidget::close);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void reveal(QWidget* window, const QColor& color, int msDur) {
    if (!window) return;
    const int d = ms(msDur);
    auto* o = new RevealOverlay(window, color);
    if (d <= 0) { o->deleteLater(); return; }
    auto* a = new QVariantAnimation(o);
    a->setDuration(d); a->setStartValue(0.0); a->setEndValue(1.0); a->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(a, &QVariantAnimation::valueChanged, o, [o](const QVariant& v){ o->t = v.toReal(); o->update(); });
    QObject::connect(a, &QVariantAnimation::finished, o, &QObject::deleteLater);
    QTimer::singleShot(30, a, [a]{ a->start(QAbstractAnimation::DeleteWhenStopped); });   // after the first frame
}
}
