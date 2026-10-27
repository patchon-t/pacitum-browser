#include <cmath>
#include "Widgets.h"
#include "ThemeManager.h"
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionToolButton>
#include <QApplication>
#include <QMenu>
#include <QPropertyAnimation>
#include <QCursor>
#include <QGraphicsOpacityEffect>
#include "Motion.h"
#include <QDialog>
#include <QPointer>
#include <QTimer>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QLibrary>
#include <cstdlib>
#include <QtMath>
#include <QButtonGroup>

static QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

AnimatedToolButton::AnimatedToolButton(QWidget* parent) : QToolButton(parent) {
    setAutoRaise(true);
    setAttribute(Qt::WA_Hover, true);
    m_anim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_level = v.toReal(); update(); });
    connect(&m_pressAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_pressScale = v.toReal(); update(); });
    connect(&m_spinAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_rotation = v.toReal(); update(); });
    connect(&m_spinAnim, &QVariantAnimation::finished, this, [this]{ m_rotation = 0; update(); });
    m_spinAnim.setEasingCurve(QEasingCurve::OutCubic);
    m_fadeAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_fadeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_fade = v.toReal(); update(); });
    connect(&m_nudgeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ const qreal t = v.toReal(); m_nudge = m_nudgeDx * std::sin(t * M_PI) * (1.0 - 0.3 * t); update(); });
    connect(&m_nudgeAnim, &QVariantAnimation::finished, this, [this]{ m_nudge = 0; update(); });
    connect(&m_fadeAnim, &QVariantAnimation::finished, this, [this]{ m_prevIcon = QIcon(); m_fade = 1.0; update(); });
}
void AnimatedToolButton::animatePress(qreal to, int ms, QEasingCurve::Type ease) {
    m_pressAnim.stop();
    if (Pacitum::reducedMotion() || ms <= 0) { m_pressScale = 1.0; update(); return; }   // spec §5: no scale when reduced
    m_pressAnim.setDuration(ms); m_pressAnim.setEasingCurve(ease);
    m_pressAnim.setStartValue(m_pressScale); m_pressAnim.setEndValue(to); m_pressAnim.start();
}
void AnimatedToolButton::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && isEnabled()) {
        if (m_anim.state() == QAbstractAnimation::Running) { m_anim.stop(); m_level = 1.0; }   // press during hover-in: bg jumps to 1
        animatePress(0.96, 80, QEasingCurve::OutCubic);   // kit #39: press 80 ms to 0.96
    }
    QToolButton::mousePressEvent(e);
}
void AnimatedToolButton::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) animatePress(1.0, 160, QEasingCurve::OutBack);
    QToolButton::mouseReleaseEvent(e);
}
void AnimatedToolButton::spin() {
    if (Pacitum::reducedMotion()) return;
    // Interruption: a second click adds another turn, 400 ms from now, starting at the current angle.
    const qreal from = m_spinAnim.state() == QAbstractAnimation::Running ? m_rotation : 0.0;
    const qreal to = m_spinAnim.state() == QAbstractAnimation::Running ? m_spinAnim.endValue().toReal() + 360.0 : 360.0;
    // kit #12. (1.9.0 fix: the start of the spin had ended up inside this comment, so it never played.)
    m_spinAnim.stop(); m_spinAnim.setEasingCurve(QEasingCurve::OutCubic); m_spinAnim.setDuration(280);
    m_spinAnim.setStartValue(from); m_spinAnim.setEndValue(to); m_spinAnim.start();
}
void AnimatedToolButton::bump() {
    // Download start (kit #19): glyph scales 1→1.18→1 over 160 ms OutCubic.
    if (Pacitum::reducedMotion()) return;
    m_pressAnim.stop(); m_pressAnim.setDuration(Motion::Base); m_pressAnim.setEasingCurve(QEasingCurve::OutCubic);
    m_pressAnim.setStartValue(1.0); m_pressAnim.setKeyValueAt(0.5, 1.18); m_pressAnim.setEndValue(1.0); m_pressAnim.start();
    QTimer::singleShot(Motion::Base + 20, this, [this]{ if (m_pressAnim.state() != QAbstractAnimation::Running) m_pressAnim.setKeyValues({}); });
}
void AnimatedToolButton::nudge(qreal dx) {
    // Back / forward feedback: the glyph steps dx px in the direction of travel and settles back.
    if (Pacitum::reducedMotion()) return;
    m_nudgeAnim.stop(); m_nudgeDx = dx;
    m_nudgeAnim.setDuration(Motion::ms(Motion::Medium)); m_nudgeAnim.setStartValue(0.0); m_nudgeAnim.setEndValue(1.0);
    m_nudgeAnim.start();
}
void AnimatedToolButton::setIconAnimated(const QIcon& ic, int ms) {
    if (ic.cacheKey() == icon().cacheKey()) return;
    const QIcon old = icon();
    setIcon(ic);
    if (Pacitum::reducedMotion() || old.isNull() || !isVisible()) { m_fadeAnim.stop(); m_prevIcon = QIcon(); m_fade = 1.0; update(); return; }
    m_prevIcon = old;
    m_fadeAnim.stop(); m_fadeAnim.setDuration(ms); m_fadeAnim.setStartValue(0.0); m_fadeAnim.setEndValue(1.0); m_fadeAnim.start();
}
void AnimatedToolButton::paintGlyph(QPainter& p) {
    const QIcon::Mode mode = isEnabled() ? QIcon::Normal : QIcon::Disabled;
    const QSize is = iconSize();
    const QPointF c(width() / 2.0, height() / 2.0);
    auto draw = [&](const QIcon& ic, qreal scale, qreal opacity) {
        if (ic.isNull() || opacity <= 0.001 || scale <= 0.001) return;
        const QPixmap pm = ic.pixmap(is, devicePixelRatioF(), mode);
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setOpacity(p.opacity() * opacity);
        p.translate(c + QPointF(m_nudge, 0)); if (m_rotation != 0.0) p.rotate(m_rotation); p.scale(scale, scale); p.translate(-c);
        p.drawPixmap(QRectF(c.x() - is.width() / 2.0, c.y() - is.height() / 2.0, is.width(), is.height()), pm, QRectF(pm.rect()));
        p.restore();
    };
    const qreal s = m_pressScale * m_extScale;
    if (!m_prevIcon.isNull() && m_fade < 1.0) draw(m_prevIcon, s, (1.0 - m_fade) * m_extOpacity);
    draw(icon(), s * (m_prevIcon.isNull() ? 1.0 : 0.8 + 0.2 * m_fade), (m_prevIcon.isNull() ? 1.0 : m_fade) * m_extOpacity);
}
void AnimatedToolButton::animateTo(qreal v) {
    // spec §3/§6: hover in 120 ms, out 160 ms, OutCubic; 0 with reduced motion
    const int ms = Pacitum::motionMs(v > m_level ? 120 : 160);
    m_anim.stop();
    if (ms == 0) { m_level = v; update(); return; }
    m_anim.setDuration(ms); m_anim.setStartValue(m_level); m_anim.setEndValue(v); m_anim.start();
}
void AnimatedToolButton::enterEvent(QEnterEvent* e) { if (isEnabled()) animateTo(1.0); QToolButton::enterEvent(e); }
void AnimatedToolButton::leaveEvent(QEvent* e) { if (!m_forcedHover) animateTo(0.0); QToolButton::leaveEvent(e); }

void AnimatedToolButton::paintBackground(QPainter& p) {
    const auto& pal = ThemeManager::current();
    QColor hover(m_hoverColor.isEmpty() ? pal.surface : m_hoverColor);
    QColor bg = hover; bg.setAlphaF(hover.alphaF() * m_level);
    if (!isEnabled()) return;
    if (isDown() || isChecked() || m_forcedDown) bg = m_hoverColor.isEmpty() ? QColor(pal.pressed) : hover.darker(110);
    if (bg.alpha() == 0) return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(bg);
    QRectF r(rect());
    const bool down = isDown() || isChecked() || m_forcedDown;
    if (!down && m_radius > 0 && m_level < 1.0) {   // spec §5: hover bg scales 0.85→1 with its opacity
        const qreal k = 0.85 + 0.15 * m_level;
        r = QRectF(r.center().x() - r.width() * k / 2, r.center().y() - r.height() * k / 2, r.width() * k, r.height() * k);
    }
    if (m_radius > 0) { const qreal rr = m_radius * r.width() / qMax(1, width()); p.drawRoundedRect(r, rr, rr); }
    else p.drawRect(r);
}

void AnimatedToolButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    paintBackground(p);
    // icon / text
    QStyleOptionToolButton opt; initStyleOption(&opt);
    if (!icon().isNull()) {
        paintGlyph(p);
    } else if (!text().isEmpty()) {
        p.setPen(QColor(ThemeManager::current().text));
        p.drawText(rect(), Qt::AlignCenter, text());
    }
    if (hasFocus() && focusPolicy() != Qt::NoFocus && opt.state & QStyle::State_HasFocus && opt.state & QStyle::State_KeyboardFocusChange) {
        p.setRenderHint(QPainter::Antialiasing);
        QColor ring(ThemeManager::current().accent); ring.setAlphaF(0.55);
        p.setPen(QPen(ring, 2)); p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1), qMax(0, m_radius - 1), qMax(0, m_radius - 1));
    }
    paintOverlay(p);
}

WindowButton::WindowButton(Kind k, QWidget* parent) : AnimatedToolButton(parent), m_kind(k) {
#ifdef Q_OS_MACOS
    setFixedSize(20, 42); setRadius(0);   // macOS "traffic lights" (painted below), left of the tabs
#else
    setFixedSize(46, 42); setRadius(0);
#endif
    setFocusPolicy(Qt::TabFocus);   // reachable with Tab; Space/Enter activates
    const QString name = k == Minimize ? QStringLiteral("Minimize") : k == Maximize ? QStringLiteral("Maximize") : QStringLiteral("Close");
    setToolTip(name); setAccessibleName(name);
}
void WindowButton::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { animateClick(); return; }
    AnimatedToolButton::keyPressEvent(e);
}
void WindowButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto& pal = ThemeManager::current();
#ifdef Q_OS_MACOS
    {   // 12 px circles: red / yellow / green, grey when the window is inactive; glyph on hover
        p.setRenderHint(QPainter::Antialiasing);
        const bool active = window() && window()->isActiveWindow();
        const QColor fill = !active && hoverLevel() < 0.01 ? QColor(QColor(pal.frame).lightness() < 128 ? 0x4A4E57 : 0xCDD2DA)
                          : m_kind == Close ? QColor(0xFF5F57) : m_kind == Minimize ? QColor(0xFEBC2E) : QColor(0x28C840);
        const QRectF c(width() / 2.0 - 6, height() / 2.0 - 6, 12, 12);
        p.setPen(QPen(fill.darker(isDown() ? 140 : 115), 0.5)); p.setBrush(isDown() ? fill.darker(115) : fill);
        p.drawEllipse(c);
        if (hoverLevel() > 0.01) {
            QColor g(0, 0, 0, int(150 * hoverLevel())); p.setPen(QPen(g, 1.2, Qt::SolidLine, Qt::RoundCap));
            const QPointF m = c.center();
            if (m_kind == Close) { p.drawLine(m + QPointF(-2.5, -2.5), m + QPointF(2.5, 2.5)); p.drawLine(m + QPointF(-2.5, 2.5), m + QPointF(2.5, -2.5)); }
            else if (m_kind == Minimize) p.drawLine(m + QPointF(-3, 0), m + QPointF(3, 0));
            else { p.drawLine(m + QPointF(-3, 0), m + QPointF(3, 0)); p.drawLine(m + QPointF(0, -3), m + QPointF(0, 3)); }
        }
        if (hasFocus()) { p.setPen(QPen(QColor(pal.accent), 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(c.adjusted(-2, -2, 2, 2)); }
        return;
    }
#endif
    const bool down = isDown() || m_forcedDown;
    const qreal lvl = hoverLevel();
    // Windows caption-button states: hover = surface / #E81123 (close), pressed = surfaceHover / #F1707A (close)
    QColor bg;
    if (m_kind == Close) { bg = down ? QColor(0xF1, 0x70, 0x7A) : QColor(0xE8, 0x11, 0x23); if (!down) bg.setAlphaF(lvl); }
    else { bg = down ? QColor(pal.pressed) : QColor(pal.surface); if (!down) bg.setAlphaF(bg.alphaF() * lvl); }
    if (bg.alpha() > 0) p.fillRect(rect(), bg);
    const bool inactive = window() && !window()->isActiveWindow();
    QColor fg = (m_kind == Close && (lvl > 0.5 || down)) ? QColor(Qt::white) : QColor(inactive ? pal.textSecondary : pal.text);
    // Icon pack glyphs (window-minimize / window-maximize / window-restore / close), 16 px centred.
    const QString name = m_kind == Minimize ? QStringLiteral("window-minimize")
                       : m_kind == Close ? QStringLiteral("close")
                       : (m_max ? QStringLiteral("window-restore") : QStringLiteral("window-maximize"));
    const int gs = m_kind == Maximize ? 14 : 16;   // spec §4.2: maximise/restore glyph 14, others 16
    const QPixmap pm = ThemeManager::icon(name, fg.name(), gs).pixmap(QSize(gs, gs), devicePixelRatioF());
    p.drawPixmap(QPointF((width() - gs) / 2.0, (height() - gs) / 2.0), pm);
    if (hasFocus()) {   // keyboard focus ring (Tab only, see focusPolicy)
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(pal.accent), 2)); p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5));
    }
}

ToggleSwitch::ToggleSwitch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true); setCursor(Qt::PointingHandCursor); setFixedSize(40, 24);
    m_anim.setDuration(160); m_anim.setEasingCurve(QEasingCurve::OutCubic);   // kit #38: knob 160 ms
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_pos = v.toReal(); update(); });
}
void ToggleSwitch::checkStateSet() { m_anim.stop(); m_pos = isChecked() ? 1.0 : 0.0; update(); }
void ToggleSwitch::nextCheckState() {
    QAbstractButton::nextCheckState();
    m_anim.stop(); m_anim.setDuration(Motion::fadeMs(160));   // reduced: still moves, 120 ms (state must stay visible)
    m_anim.setStartValue(m_pos); m_anim.setEndValue(isChecked() ? 1.0 : 0.0); m_anim.start();
}
void ToggleSwitch::paintEvent(QPaintEvent*) {
    // 2.0.1 (design): 40x24 pill, ink ("primary") when on, borderStrong when off, 18 px white knob
    // with a soft shadow.
    const auto& pal = ThemeManager::current();
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    QColor off(pal.borderStrong), on(pal.primary.isEmpty() ? pal.accent : pal.primary);
    if (!isEnabled()) { off.setAlphaF(0.5); on.setAlphaF(0.4); }
    const qreal y0 = (height() - 24) / 2.0;
    p.setBrush(mix(off, on, m_pos));
    p.drawRoundedRect(QRectF(0, y0, 40, 24), 12, 12);
    const qreal x = 3 + 16 * m_pos;
    p.setBrush(QColor(0, 0, 0, 50));
    p.drawEllipse(QRectF(x, y0 + 4, 18, 18));
    p.setBrush(isEnabled() ? QColor(Qt::white) : QColor(255, 255, 255, 200));
    p.drawEllipse(QRectF(x, y0 + 3, 18, 18));
}

SegmentThumb::SegmentThumb(QButtonGroup* group, QWidget* parent) : QWidget(parent), m_group(group) {
    setObjectName(QStringLiteral("SegThumb"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_anim = new QPropertyAnimation(this, "geometry", this);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    parent->installEventFilter(this);
    for (auto* b : group->buttons()) b->installEventFilter(this);
    connect(group, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool on){ if (on) place(true); });
    lower();
}
bool SegmentThumb::eventFilter(QObject*, QEvent* e) {
    switch (e->type()) {
    case QEvent::Resize: case QEvent::Move: case QEvent::Show: case QEvent::LayoutRequest:
        if (m_anim->state() != QAbstractAnimation::Running) place(false);
        break;
    default: break;
    }
    return false;
}
void SegmentThumb::place(bool animate) {
    QAbstractButton* b = m_group->checkedButton();
    if (!b) { hide(); return; }
    const QRect target = b->geometry();
    const bool fresh = isHidden() || geometry().width() <= 1;
    show(); lower();
    const int ms = animate && !fresh ? Pacitum::motionMs(220) : 0;
    m_anim->stop();
    if (ms <= 0) { setGeometry(target); return; }
    m_anim->setDuration(ms); m_anim->setStartValue(geometry()); m_anim->setEndValue(target);
    m_anim->start();
}

SwatchButton::SwatchButton(const QString& color, QWidget* parent) : QAbstractButton(parent), m_color(color) {
    setCheckable(true); setFixedSize(30, 30); setCursor(Qt::PointingHandCursor); setToolTip(color);
}
void SwatchButton::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(QColor(m_color));
    p.drawEllipse(QRectF(4, 4, 22, 22));
    if (isChecked()) { p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(m_color), 2)); p.drawEllipse(QRectF(1, 1, 28, 28)); }   // ring in the swatch colour, 2 px gap
}

namespace Pacitum {
// Translucent popups need a compositor. Windows/macOS/Wayland always composite; on X11 ask the
// server whether a compositing manager owns _NET_WM_CM_S<n> (libxcb is resolved at runtime, it is
// already loaded by Qt's xcb platform plugin, so there is no new build dependency).
static bool canCompositePopups() {
#if defined(Q_OS_LINUX)
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    cached = 1;
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        cached = 0;
        if (auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
            QLibrary xcb(QStringLiteral("xcb"), 1);
            struct Cookie { unsigned int sequence; };
            struct AtomReply { quint8 rt, pad; quint16 seq; quint32 len; quint32 atom; };
            struct OwnerReply { quint8 rt, pad; quint16 seq; quint32 len; quint32 owner; };
            using InternFn = Cookie (*)(void*, quint8, quint16, const char*);
            using InternReplyFn = AtomReply* (*)(void*, Cookie, void**);
            using OwnerFn = Cookie (*)(void*, quint32);
            using OwnerReplyFn = OwnerReply* (*)(void*, Cookie, void**);
            auto intern = reinterpret_cast<InternFn>(xcb.resolve("xcb_intern_atom"));
            auto internReply = reinterpret_cast<InternReplyFn>(xcb.resolve("xcb_intern_atom_reply"));
            auto owner = reinterpret_cast<OwnerFn>(xcb.resolve("xcb_get_selection_owner"));
            auto ownerReply = reinterpret_cast<OwnerReplyFn>(xcb.resolve("xcb_get_selection_owner_reply"));
            if (intern && internReply && owner && ownerReply && x11->connection()) {
                void* c = x11->connection();
                const QByteArray name = "_NET_WM_CM_S0";
                if (AtomReply* a = internReply(c, intern(c, 0, quint16(name.size()), name.constData()), nullptr)) {
                    if (OwnerReply* o = ownerReply(c, owner(c, a->atom), nullptr)) { cached = o->owner ? 1 : 0; free(o); }
                    free(a);
                }
            }
        }
    }
    return cached == 1;
#else
    return true;
#endif
}

PopupMenu::PopupMenu(QWidget* parent) : QMenu(parent) {
    m_shadow = canCompositePopups();
    if (!m_shadow) return;   // spec fallback: plain card with 1 px outline (app QSS), no shadow
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(QStringLiteral("QMenu{background:transparent;border:none;margin:%1px;padding:6px;}").arg(kShadow));
}
void PopupMenu::paintEvent(QPaintEvent* e) {
    if (m_shadow) {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const auto& pal = ThemeManager::current();
        const QRectF card = QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow);
        const bool dark = QColor(pal.toolbar).lightness() < 128;
        // fake 0 16px 40px blur: stacked rounded rects, offset downwards, decreasing alpha
        const int layers = kShadow;
        const qreal peak = dark ? 0.55 : 0.16;
        p.setPen(Qt::NoPen);
        for (int i = layers; i >= 1; --i) {
            const qreal t = qreal(i) / layers;
            QColor c = dark ? QColor(0, 0, 0) : QColor(0x10, 0x18, 0x28);
            c.setAlphaF(peak * 0.11 * (1.0 - t) * (1.0 - t) + 0.002);
            const qreal grow = i * 1.0;
            p.setBrush(c);
            p.drawRoundedRect(card.adjusted(-grow, -grow * 0.5, grow, grow).translated(0, qMin<qreal>(6, i * 0.5)), 16 + grow, 16 + grow);
        }
        p.setBrush(QColor(pal.toolbar));
        p.setPen(QPen(QColor(pal.borderStrong), 1));
        p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 16, 16);
    }
    QMenu::paintEvent(e);
}

QIcon spinnerIcon(int angle, const QString& color) {
    const int px = 32;
    QPixmap pm(px, px); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(color), 4.0); pen.setCapStyle(Qt::RoundCap); p.setPen(pen);
    p.drawArc(QRectF(4, 4, px - 8, px - 8), -angle * 16, 270 * 16);
    p.end();
    pm.setDevicePixelRatio(2.0);
    return QIcon(pm);
}

namespace {
class MenuFader : public QObject {
public:
    using QObject::QObject;
    // Menus, popups (downloads popup, omnibox suggestions, tooltips excluded) and dialogs fade in.
    // Dialogs also rise 8 px. Only plain raster top-levels are animated (never a window that hosts
    // a web view: layered-window opacity breaks its GPU swap chain). Reduced motion: instant.
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() != QEvent::Show && e->type() != QEvent::Hide) return false;
        auto* w = qobject_cast<QWidget*>(o);
        if (!w || !w->isWindow()) return false;
        // A widget in its QWidget base destructor reports QWidget's meta-object: never touch it.
        if (w->metaObject() == &QWidget::staticMetaObject) return false;
        const bool menu = qobject_cast<QMenu*>(w) != nullptr;
        const bool dialog = !menu && qobject_cast<QDialog*>(w) != nullptr && !w->inherits("QFileDialog");
        const bool popup = !menu && !dialog && (w->windowFlags() & Qt::WindowType_Mask) == Qt::Popup;
        if (!menu && !dialog && !popup) return false;
        if (w->property("_pacitumNoFade").toBool()) return false;
        for (auto* c : w->findChildren<QWidget*>()) if (c->inherits("QWebEngineView")) return false;
        const bool reduced = Pacitum::reducedMotion();
        if (e->type() == QEvent::Hide) {
            // 2.0.1: close is instant. The old snapshot "ghost" fade-out overlapped the next
            // pop-up's open animation when a button was clicked again quickly (double image, blink),
            // and grabbing a snapshot on every close made small actions lag.
            removeScrim(w);
            if (auto* a = w->findChild<QPropertyAnimation*>(QStringLiteral("_pmFadeIn"), Qt::FindDirectChildrenOnly)) a->stop();
            w->setWindowOpacity(1.0);
            return false;
        }
        // Context menus open at the cursor: a 90 ms fade. Drop-downs and pop-ups: 120 ms fade with a
        // 4 px drop. Dialogs: 160 ms fade rising 8 px over a scrim. No snapshots, no extra windows.
        const bool context = menu && !w->parentWidget() ? true
                             : menu && (QCursor::pos() - w->pos()).manhattanLength() < 24;
        const int ms = reduced ? 0 : dialog ? 160 : context ? 90 : 120;
        if (dialog && w->isModal()) addScrim(w);
        if (ms <= 0) return false;
        if (auto* old = w->findChild<QPropertyAnimation*>(QStringLiteral("_pmFadeIn"), Qt::FindDirectChildrenOnly)) { old->stop(); old->deleteLater(); }
        w->setWindowOpacity(0.0);
        auto* a = new QPropertyAnimation(w, "windowOpacity", w);
        a->setObjectName(QStringLiteral("_pmFadeIn"));
        a->setDuration(ms); a->setStartValue(0.0); a->setEndValue(1.0); a->setEasingCurve(QEasingCurve::OutCubic);
        a->start(QAbstractAnimation::DeleteWhenStopped);
        QPointer<QWidget> guard(w);
        const int dy = (context || w->property("_pmNoSlide").toBool()) ? 0 : dialog ? 8 : -4;
        if (dy) QTimer::singleShot(0, w, [guard, ms, dy]{   // after the window has been positioned
            if (!guard || !guard->isVisible()) return;
            const QPoint end = guard->pos();
            auto* m = new QPropertyAnimation(guard.data(), "pos", guard.data());
            m->setDuration(ms); m->setStartValue(end + QPoint(0, dy)); m->setEndValue(end); m->setEasingCurve(QEasingCurve::OutCubic);
            m->start(QAbstractAnimation::DeleteWhenStopped);
        });
        return false;
    }
private:
    // Modal scrim: a dim child over the parent browser window (named like the other transient
    // window overlays, so BrowserWindow::resizeEvent keeps it at the window size).
    static void addScrim(QWidget* dlg) {
        QWidget* host = dlg->parentWidget() ? dlg->parentWidget()->window() : nullptr;
        if (!host || !host->inherits("BrowserWindow") || !host->isVisible()) return;
        auto* scrim = new QWidget(host);
        scrim->setObjectName(QStringLiteral("PacitumWindowOverlay"));
        scrim->setProperty("_pmScrimFor", QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(dlg)));
        scrim->setAttribute(Qt::WA_TransparentForMouseEvents);
        scrim->setAutoFillBackground(true);
        QPalette pal = scrim->palette(); pal.setColor(QPalette::Window, QColor(0, 0, 0, 56)); scrim->setPalette(pal);
        scrim->setGeometry(host->rect()); scrim->show(); scrim->raise();
        auto* eff = new QGraphicsOpacityEffect(scrim); eff->setOpacity(0.0); scrim->setGraphicsEffect(eff);
        auto* a = new QPropertyAnimation(eff, "opacity", scrim);
        a->setDuration(Motion::fadeMs(Motion::Medium)); a->setStartValue(0.0); a->setEndValue(1.0); a->setEasingCurve(QEasingCurve::OutCubic);
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
    static void removeScrim(QWidget* dlg) {
        QWidget* host = dlg->parentWidget() ? dlg->parentWidget()->window() : nullptr;
        if (!host) return;
        for (auto* c : host->findChildren<QWidget*>(QStringLiteral("PacitumWindowOverlay"), Qt::FindDirectChildrenOnly)) {
            if (c->property("_pmScrimFor").value<quintptr>() != reinterpret_cast<quintptr>(dlg)) continue;
            c->setProperty("_pmScrimFor", QVariant());
            auto* eff = qobject_cast<QGraphicsOpacityEffect*>(c->graphicsEffect());
            const int d = Motion::fadeMs(Motion::Fast);
            if (!eff || d <= 0) { c->deleteLater(); continue; }
            auto* a = new QPropertyAnimation(eff, "opacity", c);
            a->setDuration(d); a->setStartValue(eff->opacity()); a->setEndValue(0.0); a->setEasingCurve(QEasingCurve::InCubic);
            QObject::connect(a, &QPropertyAnimation::finished, c, &QObject::deleteLater);
            a->start(QAbstractAnimation::DeleteWhenStopped);
        }
    }
};
}
void installMenuFader() {
    static MenuFader* f = nullptr;
    if (!f) { f = new MenuFader(qApp); qApp->installEventFilter(f); }
}
}

// ---------------------------------------------------------------------------
InfoBar::InfoBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("InfoBarWrap"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto* outer = new QHBoxLayout(this); outer->setContentsMargins(8, 8, 8, 8); outer->setSpacing(0);
    m_card = new QWidget(this); m_card->setObjectName(QStringLiteral("InfoBar"));
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    m_card->setFixedHeight(44);
    outer->addWidget(m_card);
    auto* h = new QHBoxLayout(m_card); h->setContentsMargins(14, 0, 6, 0); h->setSpacing(10);
    m_icon = new QLabel(m_card); m_icon->setFixedSize(18, 18);
    m_text = new QLabel(m_card); m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_secondary = new QPushButton(m_card); m_secondary->hide(); m_secondary->setCursor(Qt::PointingHandCursor);
    m_primary = new QPushButton(m_card); m_primary->setObjectName(QStringLiteral("PrimaryButton")); m_primary->setCursor(Qt::PointingHandCursor);
    m_primary->setIconSize(QSize(16, 16)); m_primary->hide();
    m_close = new QToolButton(m_card); m_close->setFixedSize(30, 30); m_close->setIconSize(QSize(16, 16));
    m_close->setToolTip(QStringLiteral("Close")); m_close->setAccessibleName(QStringLiteral("Close"));
    h->addWidget(m_icon); h->addWidget(m_text); h->addStretch(1);
    h->addWidget(m_secondary); h->addWidget(m_primary); h->addWidget(m_close);
    connect(m_primary, &QPushButton::clicked, this, &InfoBar::primaryClicked);
    connect(m_secondary, &QPushButton::clicked, this, &InfoBar::secondaryClicked);
    connect(m_close, &QToolButton::clicked, this, [this]{ hideAnimated(); emit closed(); });
    setMaximumHeight(0); hide();
}
void InfoBar::setMessage(const QString& t) { m_text->setText(t); }
void InfoBar::setPrimary(const QString& t, const QString& icon) { m_primary->setText(t); m_primaryIcon = icon; m_primary->setVisible(!t.isEmpty()); refresh(); }
void InfoBar::setSecondary(const QString& t) { m_secondary->setText(t); m_secondary->setVisible(!t.isEmpty()); }
void InfoBar::refresh() {
    const auto& p = ThemeManager::current();
    const bool warn = m_kind == Warning;
    m_icon->setPixmap(ThemeManager::icon(warn ? QStringLiteral("warning") : QStringLiteral("info"), warn ? p.warning : p.accent, 18)
                      .pixmap(QSize(18, 18), devicePixelRatioF()));
    m_close->setIcon(ThemeManager::icon(QStringLiteral("close"), p.textSecondary, 16));
    if (!m_primaryIcon.isEmpty()) m_primary->setIcon(ThemeManager::icon(m_primaryIcon, p.onAccent, 16));
}
void InfoBar::animateTo(int h, bool hideAtEnd) {
    if (m_anim) { m_anim->stop(); m_anim->deleteLater(); m_anim = nullptr; }
    const int ms = Pacitum::motionMs(160);
    if (ms == 0) { setMaximumHeight(h); setVisible(!hideAtEnd); return; }
    m_anim = new QPropertyAnimation(this, "maximumHeight", this);
    m_anim->setDuration(ms); m_anim->setEasingCurve(QEasingCurve::OutCubic);
    m_anim->setStartValue(maximumHeight()); m_anim->setEndValue(h);
    connect(m_anim, &QPropertyAnimation::finished, this, [this, hideAtEnd]{ if (hideAtEnd) hide(); });
    m_anim->start();
}
void InfoBar::showAnimated() { refresh(); if (isVisible() && maximumHeight() >= 60) return; setMaximumHeight(0); show(); animateTo(60, false); }
void InfoBar::hideAnimated() { if (!isVisible()) return; animateTo(0, true); }

// ---------- StarButton (animations-spec §3) ----------
static qreal ease(QEasingCurve::Type t, qreal x, qreal overshoot = -1) {
    QEasingCurve c(t); if (overshoot > 0) c.setOvershoot(overshoot);
    return c.valueForProgress(qBound<qreal>(0.0, x, 1.0));
}
StarButton::StarButton(QWidget* parent) : AnimatedToolButton(parent) {
    setFixedSize(34, 34); setIconSize(QSize(18, 18)); setRadius(17);
    m_tl.setEasingCurve(QEasingCurve::Linear);
    connect(&m_tl, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ onTick(v.toReal()); });
    connect(&m_tl, &QVariantAnimation::finished, this, [this]{ m_t = -1; m_pop = false; setGlyphTransform(1.0, 1.0); });
}
void StarButton::setIcons(const QIcon& outline, const QIcon& filled) {
    m_outline = outline; m_filled = filled;
    if (m_tl.state() != QAbstractAnimation::Running) setIcon(m_on ? m_filled : m_outline);
    else if (!m_pop || m_swapped) setIcon(m_on ? m_filled : m_outline);
}
void StarButton::setStarred(bool on, bool animate) {
    if (on == m_on && m_tl.state() != QAbstractAnimation::Running) { setIcon(on ? m_filled : m_outline); return; }
    m_on = on;
    const qreal cur = m_extScale;
    m_tl.stop(); m_t = -1; setGlyphTransform(1.0, 1.0);
    if (!animate || Pacitum::reducedMotion() || !isVisible()) { setIcon(on ? m_filled : m_outline); update(); return; }
    m_from = cur; m_pop = on; m_swapped = false;
    if (on) { m_tl.setDuration(380); m_tl.setStartValue(0.0); m_tl.setEndValue(380.0); }
    else { setIconAnimated(m_outline, 150); m_tl.setDuration(150); m_tl.setStartValue(0.0); m_tl.setEndValue(150.0); }
    m_tl.start();
}
void StarButton::onTick(qreal t) {
    m_t = t;
    qreal s = 1.0;
    if (m_pop) {
        if (t < 80) s = m_from + (0.8 - m_from) * ease(QEasingCurve::InCubic, t / 80.0);
        else {
            if (!m_swapped) { m_swapped = true; setIcon(m_filled); }   // outline → filled accent at 80 ms
            if (t < 200) s = 0.8 + 0.4 * ease(QEasingCurve::OutBack, (t - 80) / 120.0, 1.2);
            else if (t < 300) s = 1.2 - 0.2 * ease(QEasingCurve::OutCubic, (t - 200) / 100.0);
        }
    } else {   // un-bookmark: scale 1→0.9→1 over 150 ms while the fill cross-fades
        if (t < 75) s = m_from + (0.9 - m_from) * ease(QEasingCurve::OutCubic, t / 75.0);
        else s = 0.9 + 0.1 * ease(QEasingCurve::OutCubic, (t - 75) / 75.0);
    }
    setGlyphTransform(s, 1.0);
}
void StarButton::paintOverlay(QPainter& p) {
    if (!m_pop || m_t < 80 || m_t > 380) return;
    const qreal k = ease(QEasingCurve::OutCubic, (m_t - 80) / 300.0);
    const qreal r = 6 + 9 * k, op = 1.0 - k, d = 3.0 * (1.0 - 0.6 * k);
    p.save(); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen);
    QColor c(ThemeManager::current().accent); c.setAlphaF(op); p.setBrush(c);
    const QPointF ctr(width() / 2.0, height() / 2.0);
    for (int i = 0; i < 6; ++i) {
        const qreal a = qDegreesToRadians(i * 60.0 - 90.0);
        p.drawEllipse(ctr + QPointF(qCos(a) * r, qSin(a) * r), d / 2, d / 2);
    }
    p.restore();
}
