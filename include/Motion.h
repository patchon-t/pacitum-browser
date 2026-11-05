#pragma once
// Shared motion helpers (1.7.0). Timings follow the shared Pacitum motion tokens
// (/workspace/pacitum-animations/motion-tokens.css) so Qt matches the Firefox build.
// Every helper is instant when reduced motion is on (app setting, PACITUM_REDUCED_MOTION, or the
// Windows "Show animations in Windows" setting). Only small, raster child widgets or short-lived
// top-level ghosts are animated: never a widget that hosts a web view.
#include <QPointer>
#include <QWidget>
#include <QPixmap>
#include <QColor>
#include <QVariantAnimation>
class QWidget;
class QLabel;
class QString;

namespace Motion {
// 2.0.3: drive every Qt animation (tab glide, pop-ups, fades) from a precise timer at the screen's refresh
// rate (60–240 Hz) instead of Qt's default coarse ~60 Hz timer, so motion is smooth on 120/144 Hz displays.
// Call once after the QApplication exists. PACITUM_ANIMATION_HZ overrides the rate (0 = Qt's default driver).
void installRefreshRateClock();
enum : int { Instant = 80, Fast = 120, Quick = 140, Base = 160, Exit = 180, Medium = 200, Enter = 240, Slow = 280 };
int ms(int token);                 // movement: 0 with reduced motion
int fadeMs(int token);             // opacity fades: capped at 120 ms with reduced motion
bool enabled();
bool targetVisible(QWidget* w);   // visibility a running slide is heading to
// Opacity 0→1 on a child widget (temporary QGraphicsOpacityEffect, removed at the end).
void fadeIn(QWidget* w, int token = Base);
// Slide a widget in/out by animating maximumHeight (0 ↔ target), hiding at the end of an exit.
// target <= 0 uses sizeHint().height().
void slideVisible(QWidget* w, bool show, int target = 0, int token = Base);
// Short "pop" for a small label: fades in from 0.4 (zoom percentage etc.).
void pop(QWidget* w);
// Fade (and gently shrink) a snapshot of a top-level that is being hidden. The snapshot is a
// separate click-through window that lives ≤ 180 ms, so it never blocks input or follows resizes.
void ghostOut(QWidget* topLevel, int token, qreal scaleTo);
// 1.9.0 / 1.8.11 pop-ins: a menu / popup / dialog is shown at opacity 0 while a click-through
// snapshot zooms out of `originGlobal` (where it was opened from) with a soft overshoot, then the
// real window takes over. False when it can't be done here (reduced motion, Wayland, no snapshot):
// the caller falls back to the plain fade.
bool popIn(QWidget* topLevel, const QPoint& originGlobal, qreal fromScale = 0.88, int token = Medium);
// A snapshot of a whole browser window flying from `from` to `to` (global rects) while fading out:
// close (shrinks away), minimize (drops toward the bottom), maximize / restore (morphs). The real
// window has already changed underneath. Skipped on Windows (DWM animates these itself), on Wayland
// and with reduced motion.
bool windowGhostsAllowed();
void windowGhost(const QPixmap& snapshot, const QRect& from, const QRect& to, int ms, qreal fadeTo = 0.0);
// New window: the page colour covers it and a circle opens from the centre (plain fade if reduced).
void reveal(QWidget* window, const QColor& color, int ms = 520);
// Profile card style hover: lift a child widget by dy px (animating pos); restores on leave.
void installHoverLift(QWidget* w, int dy = -3);
// Centred pill near the top of a window: fade in 160, hold, fade out 140 (zoom 900 hold = 1.2 s,
// fullscreen hint ~1.7 s hold = 2 s). Follows window resizes; click-through.
void toast(QWidget* w, bool show);
void bubble(QWidget* host, const QString& text, int holdMs, int topY);

// Full-size fading overlay (page veil, back/forward snapshot, theme cross-fade). Paints a solid
// colour or a pixmap with QPainter::setOpacity: no QGraphicsOpacityEffect, so nothing is rendered
// offscreen per frame and nothing is allocated per frame (one reusable animation per overlay).
class FadeOverlay : public QWidget {
public:
    explicit FadeOverlay(QWidget* parent);
    void setColor(const QColor& c) { m_color = c; m_pm = QPixmap(); }
    void setPixmap(const QPixmap& pm) { m_pm = pm; }
    void setOpacity(qreal o) { m_opacity = o; update(); }
    // 1.8.5 3D: during the next fade the snapshot also turns around its vertical axis (perspective)
    // up to `degrees` and scales to `scaleTo`. Cleared when the fade ends.
    void setTilt(qreal degrees, qreal scaleTo = 1.0) { m_tilt = degrees; m_scaleTo = scaleTo; }
    // Fade from -> to over ms (optionally drifting by `drift` px); hides (or deletes) itself at the end.
    void fade(qreal from, qreal to, int ms, QEasingCurve::Type curve, int delayMs = 0, QPoint drift = {}, bool deleteWhenDone = false);
    void stop();
    bool running() const { return m_anim.state() == QAbstractAnimation::Running || m_pendingStart; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QColor m_color; QPixmap m_pm; qreal m_opacity = 1.0;
    qreal m_from = 1.0, m_to = 0.0; QPoint m_origin, m_drift;
    qreal m_t = 0.0, m_tilt = 0.0, m_scaleTo = 1.0;
    bool m_deleteWhenDone = false, m_pendingStart = false;
    QVariantAnimation m_anim;
};
}

