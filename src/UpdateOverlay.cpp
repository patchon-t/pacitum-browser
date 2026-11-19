#include "UpdateOverlay.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>
#include <cmath>
#include <iterator>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace UpdUi {

namespace {
double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
double seg(double t, double a, double b) { return clamp01((t - a) / (b - a)); }   // 0…1 inside [a, b]
double easeOut(double t) { return 1 - std::pow(1 - t, 3); }
double easeOutBack(double t) { const double c = 1.70158, c3 = c + 1; return 1 + c3 * std::pow(t - 1, 3) + c * std::pow(t - 1, 2); }
QColor alpha(QColor c, double a) { c.setAlphaF(clamp01(a) * c.alphaF()); return c; }

// Old pop-up content fading out above the new content during Sheet::swap().
class Cover : public QWidget {
public:
    using QWidget::QWidget;
    QPixmap pm; double op = 0;
protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); p.setOpacity(op); p.drawPixmap(0, 0, pm); }
};

// The tick from the design: M5 12.5 l4.5 4.5 L19 7.5 (24 × 24 box), drawn up to `t`.
void drawCheck(QPainter& p, const QPointF& c, double size, double t, const QColor& col, double width) {
    if (t <= 0) return;
    const double k = size / 24.0;
    const QPointF a(c.x() + (5 - 12) * k, c.y() + (12.5 - 12) * k), b(c.x() + (9.5 - 12) * k, c.y() + (17 - 12) * k),
                  e(c.x() + (19 - 12) * k, c.y() + (7.5 - 12) * k);
    const double l1 = QLineF(a, b).length(), l2 = QLineF(b, e).length(), d = (l1 + l2) * clamp01(t);
    QPainterPath path(a);
    if (d <= l1) path.lineTo(a + (b - a) * (d / l1));
    else { path.lineTo(b); path.lineTo(b + (e - b) * ((d - l1) / l2)); }
    p.setPen(QPen(col, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}
}

QString Colors::toArg() const {
    QStringList l;
    for (const QColor& c : {bg, border, text, sub, accent, onAccent, track, panel}) l << c.name(QColor::HexRgb);
    return l.join(QLatin1Char(','));
}
Colors Colors::fromArg(const QString& s) {
    Colors c;
    const QStringList l = s.split(QLatin1Char(','));
    QColor* f[] = {&c.bg, &c.border, &c.text, &c.sub, &c.accent, &c.onAccent, &c.track, &c.panel};
    for (int i = 0; i < 8 && i < l.size(); ++i) if (QColor v(l[i]); v.isValid()) *f[i] = v;
    return c;
}

// --------------------------------------------------------------------------------------------- Hero
Hero::Hero(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    m_introAnim = new QVariantAnimation(this);
    m_introAnim->setStartValue(0.0); m_introAnim->setEndValue(1.0);
    connect(m_introAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_intro = v.toDouble(); update(); });
    m_tick = new QTimer(this); m_tick->setInterval(16);
    connect(m_tick, &QTimer::timeout, this, [this]{
        if (m_mode == Spinner || (m_mode == Ring && m_target < 0)) m_spin = std::fmod(m_spin + 16.0 / 900.0, 1.0);
        if (m_mode == Ring && m_target >= 0) {
            m_shown += (m_target - m_shown) * 0.16;
            if (std::abs(m_target - m_shown) < 0.0008) { m_shown = m_target; m_tick->stop(); }
        }
        update();
    });
}

QSize Hero::sizeHint() const {
    switch (m_mode) {
    case Arrow: return {56, 56};
    case Done: return {128, 128};
    case Spinner: return {72, 72};
    case Check: case Warn: return {80, 80};
    default: return {112, 112};
    }
}

void Hero::setMode(Mode m) {
    const bool changed = m != m_mode;
    m_mode = m;
    if (changed) updateGeometry();
    static const int ms[] = {500, 450, 700, 300, 1250, 400};
    m_introAnim->stop();
    if (m_reduced) m_intro = 1.0;
    else { m_intro = 0.0; m_introAnim->setDuration(ms[m]); m_introAnim->start(); }
    if (m == Spinner || (m == Ring && m_target < 0)) m_tick->start(); else if (m != Ring) m_tick->stop();
    if (m == Ring && changed) m_shown = qMax(0.0, m_target);
    update();
}

void Hero::setProgress(double frac) {
    m_target = frac < 0 ? -1.0 : clamp01(frac);
    if (m_mode == Ring && !m_tick->isActive()) m_tick->start();
}

void Hero::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(width() / 2.0, height() / 2.0);
    const double t = m_intro;
    switch (m_mode) {
    case Arrow: {
        const double s = 0.6 + 0.4 * easeOutBack(seg(t, 0, 1));
        p.translate(c); p.scale(s, s);
        p.setPen(Qt::NoPen); p.setBrush(alpha(m_c.accent, 0.16 * (0.3 + 0.7 * t)));
        p.drawRoundedRect(QRectF(-26, -26, 52, 52), 15, 15);
        const double bob = -4 * std::sin(seg(t, 0.3, 1) * M_PI);
        p.setPen(QPen(m_c.accent, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(0, 9 + bob), QPointF(0, -9 + bob));
        p.drawPolyline(QPolygonF({QPointF(-7.5, -1.5 + bob), QPointF(0, -9 + bob), QPointF(7.5, -1.5 + bob)}));
        break;
    }
    case Ring: {
        const double s = 0.86 + 0.14 * easeOut(t);
        p.translate(c); p.scale(s, s); p.setOpacity(0.2 + 0.8 * t);
        const QRectF r(-48, -48, 96, 96);
        p.setPen(QPen(m_c.track, 8)); p.setBrush(Qt::NoBrush); p.drawEllipse(r);
        p.setPen(QPen(m_c.accent, 8, Qt::SolidLine, Qt::RoundCap));
        if (m_target < 0) p.drawArc(r, int((90 - m_spin * 360) * 16), -90 * 16);
        else if (m_shown > 0.002) p.drawArc(r, 90 * 16, -int(m_shown * 360 * 16));
        QFont f = font(); f.setPixelSize(m_small.isEmpty() ? 22 : 19); f.setWeight(QFont::Bold);
        p.setFont(f); p.setPen(m_c.text);
        const QString big = m_big.isEmpty() && m_target >= 0 ? QStringLiteral("%1%").arg(qRound(m_shown * 100)) : m_big;
        if (m_small.isEmpty()) p.drawText(r, Qt::AlignCenter, big);
        else {
            p.drawText(QRectF(-44, -20, 88, 24), Qt::AlignHCenter | Qt::AlignBottom, big);
            QFont fs = font(); fs.setPixelSize(11); p.setFont(fs); p.setPen(m_c.sub);
            p.drawText(QRectF(-44, 6, 88, 16), Qt::AlignHCenter | Qt::AlignTop, m_small);
        }
        break;
    }
    case Check: case Warn: {
        const bool warn = m_mode == Warn;
        const QColor col = warn ? QColor(0xF8, 0x71, 0x71) : m_c.accent;
        const double s = 0.6 + 0.4 * easeOutBack(seg(t, 0, 0.45));
        p.save(); p.translate(c); p.scale(s, s);
        p.setPen(Qt::NoPen); p.setBrush(alpha(col, 0.15)); p.drawEllipse(QPointF(0, 0), 38, 38);
        p.restore();
        if (warn) {
            p.setPen(QPen(col, 3.4, Qt::SolidLine, Qt::RoundCap)); const double o = seg(t, 0.3, 1);
            p.setOpacity(o); p.drawLine(QPointF(c.x(), c.y() - 11), QPointF(c.x(), c.y() + 3)); p.drawPoint(QPointF(c.x(), c.y() + 11));
        } else drawCheck(p, c, 40, seg(t, 0.32, 1), col, 3.8);
        break;
    }
    case Spinner: {
        const QRectF r(c.x() - 26, c.y() - 26, 52, 52);
        p.setOpacity(0.3 + 0.7 * t);
        p.setPen(QPen(m_c.track, 6)); p.setBrush(Qt::NoBrush); p.drawEllipse(r);
        p.setPen(QPen(m_c.accent, 6, Qt::SolidLine, Qt::RoundCap));
        const double wobble = 70 + 50 * std::sin(m_spin * 2 * M_PI);   // arc breathes while it turns
        p.drawArc(r, int((90 - m_spin * 360) * 16), -int(wobble * 16));
        break;
    }
    case Done: {
        for (int i = 0; i < 2; ++i) {   // rings burst out of the circle
            const double b = seg(t, 0.22 + i * 0.16, 0.75 + i * 0.16);
            if (b <= 0 || b >= 1) continue;
            p.setPen(QPen(alpha(m_c.accent, 0.55 * (1 - b)), 2.0)); p.setBrush(Qt::NoBrush);
            const double rr = 34 + 30 * easeOut(b);
            p.drawEllipse(c, rr, rr);
        }
        const double pop = t < 0.35 ? 1.12 * easeOut(seg(t, 0, 0.35)) : 1.12 - 0.12 * easeOut(seg(t, 0.35, 0.5));
        p.save(); p.translate(c); p.scale(pop, pop);
        p.setPen(Qt::NoPen); p.setBrush(m_c.accent); p.drawEllipse(QPointF(0, 0), 33, 33);
        p.restore();
        if (t >= 1) { p.setPen(QPen(alpha(m_c.accent, 0.3), 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(c, 44.0, 44.0); }
        drawCheck(p, c, 34, seg(t, 0.35, 0.8), m_c.onAccent, 3.6);
        break;
    }
    }
}

// --------------------------------------------------------------------------------------------- ZoomText
ZoomText::ZoomText(QWidget* parent) : QWidget(parent) {
    QFont f = font(); f.setPixelSize(38); f.setWeight(QFont::ExtraBold); f.setLetterSpacing(QFont::AbsoluteSpacing, -1); setFont(f);
    m_anim = new QVariantAnimation(this);
    m_anim->setStartValue(0.0); m_anim->setEndValue(1.0); m_anim->setDuration(900); m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_t = v.toDouble(); update(); });
}
QSize ZoomText::sizeHint() const { return {QFontMetrics(font()).horizontalAdvance(m_text) + 16, QFontMetrics(font()).height() + 22}; }   // room for the zoom
void ZoomText::play(int delayMs, bool reduced) {
    m_anim->stop();
    if (reduced) { m_t = 1; update(); return; }
    m_t = 0; update();
    QTimer::singleShot(delayMs, m_anim, [a = m_anim]{ a->start(); });
}
void ZoomText::paintEvent(QPaintEvent*) {
    if (m_t <= 0) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font());
    const double s = 2.4 - 1.4 * m_t, blur = (1 - m_t) * 9;
    p.translate(width() / 2.0, height() / 2.0); p.scale(s, s);
    const QRectF r(-width(), -height() / 2.0, width() * 2.0, height());
    p.setPen(m_color);
    if (blur > 0.4) {   // soft "blur": the text smeared in a circle, sharpening as it lands
        p.setOpacity(m_t * 0.22);
        for (int i = 0; i < 8; ++i) {
            const double a = i * M_PI / 4;
            p.drawText(r.translated(std::cos(a) * blur / s, std::sin(a) * blur / s), Qt::AlignCenter, m_text);
        }
    }
    p.setOpacity(m_t * m_t);
    p.drawText(r, Qt::AlignCenter, m_text);
}

// --------------------------------------------------------------------------------------------- Sheet
Sheet::Sheet(const Colors& c, QWidget* parent, bool onTop) : QWidget(parent), m_c(c) {
    Qt::WindowFlags f = Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    if (onTop) f |= Qt::WindowStaysOnTopHint;
    setWindowFlags(f);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setObjectName(QStringLiteral("UpdSheet"));

    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("UpdBody"));
    m_body->setAttribute(Qt::WA_TranslucentBackground);
    m_body->setStyleSheet(QStringLiteral(
        "#UpdBody{background:transparent;}"
        "QLabel{color:%1;background:transparent;}"
        "QLabel#UpdTitle{font-size:15px;font-weight:600;}"
        "QLabel#UpdSub{color:%2;font-size:13px;}"
        "QLabel#UpdDetail{color:%2;font-size:12px;}"
        "QLabel#UpdLink{font-size:12px;font-weight:600;}"
        "QFrame#UpdNotes{background:%5;border-radius:12px;}"
        "QLabel#UpdNotesHead{color:%2;font-size:12px;font-weight:600;}"
        "QPushButton{min-height:34px;max-height:34px;padding:0 16px;border-radius:17px;font-size:13px;}"
        "QPushButton#UpdPrimary{background:%3;color:%4;border:none;font-weight:700;padding:0 20px;}"
        "QPushButton#UpdPrimary:hover{background:%6;}"
        "QPushButton#UpdGhost{background:transparent;color:%1;border:1px solid %7;}"
        "QPushButton#UpdGhost:hover{background:%8;}")
        .arg(c.text.name(), c.sub.name(), c.accent.name(), c.onAccent.name(), c.panel.name(),
             c.accent.lighter(112).name(), c.border.name(), alpha(c.text, 0.07).name(QColor::HexArgb)));
    auto* l = new QVBoxLayout(m_body);
    l->setContentsMargins(26, 24, 26, 22); l->setSpacing(8);
    m_hero = new Hero(m_body); m_hero->setColors(c);
    l->addWidget(m_hero, 0, Qt::AlignHCenter);
    l->addSpacing(4);
    m_title = new QLabel(m_body); m_title->setObjectName(QStringLiteral("UpdTitle")); m_title->setAlignment(Qt::AlignCenter); m_title->setWordWrap(true);
    m_zoom = new ZoomText(m_body); m_zoom->setColor(c.text); m_zoom->hide();
    l->addWidget(m_title);
    l->addWidget(m_zoom);   // full card width: the big first frames of the zoom stay visible
    m_text = new QLabel(m_body); m_text->setObjectName(QStringLiteral("UpdSub")); m_text->setAlignment(Qt::AlignCenter); m_text->setWordWrap(true);
    l->addWidget(m_text);
    m_detail = new QLabel(m_body); m_detail->setObjectName(QStringLiteral("UpdDetail")); m_detail->setAlignment(Qt::AlignCenter); m_detail->setWordWrap(true); m_detail->hide();
    l->addWidget(m_detail);
    m_link = new QLabel(m_body); m_link->setObjectName(QStringLiteral("UpdLink")); m_link->setAlignment(Qt::AlignCenter); m_link->hide();
    m_link->setCursor(Qt::PointingHandCursor); m_link->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    l->addWidget(m_link);
    m_notes = new QFrame(m_body); m_notes->setObjectName(QStringLiteral("UpdNotes")); m_notes->hide();
    auto* nl = new QVBoxLayout(m_notes); nl->setContentsMargins(14, 12, 14, 12); nl->setSpacing(6);
    l->addWidget(m_notes);
    l->addSpacing(6);
    auto* bl = new QHBoxLayout(); bl->setSpacing(8); bl->addStretch(1);
    m_primary = new QPushButton(m_body); m_primary->setObjectName(QStringLiteral("UpdPrimary"));
    m_secondary = new QPushButton(m_body); m_secondary->setObjectName(QStringLiteral("UpdGhost"));
    m_tertiary = new QPushButton(m_body); m_tertiary->setObjectName(QStringLiteral("UpdGhost"));
    for (auto* b : {m_primary, m_secondary, m_tertiary}) { b->setCursor(Qt::PointingHandCursor); bl->addWidget(b); b->hide(); }
    bl->addStretch(1);
    l->addLayout(bl);

    m_cover = new Cover(this);
    m_cover->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_cover->hide();
    relayout();
}

void Sheet::setNotes(const QStringList& lines, const QString& heading) {
    auto* nl = static_cast<QVBoxLayout*>(m_notes->layout());
    while (auto* it = nl->takeAt(0)) { delete it->widget(); delete it; }
    if (lines.isEmpty()) { m_notes->hide(); return; }
    auto* h = new QLabel(heading, m_notes); h->setObjectName(QStringLiteral("UpdNotesHead")); nl->addWidget(h);
    for (const QString& s : lines) {
        auto* lab = new QLabel(QStringLiteral("·  ") + s, m_notes); lab->setWordWrap(true);
        lab->setTextFormat(Qt::PlainText); nl->addWidget(lab);
    }
    m_notes->show();
}

void Sheet::setButtons(const QString& primary, const QString& secondary, const QString& tertiary) {
    const QString t[] = {primary, secondary, tertiary};
    QPushButton* b[] = {m_primary, m_secondary, m_tertiary};
    for (int i = 0; i < 3; ++i) { b[i]->setText(t[i]); b[i]->setVisible(!t[i].isEmpty()); QObject::disconnect(b[i], &QPushButton::clicked, nullptr, nullptr); }   // 2.0.10: only clicks (a bare disconnect() also cut destroyed() and Qt warned every time)
    if (!primary.isEmpty()) m_primary->setDefault(true);
}

QPoint Sheet::cardCenter() const { return mapToGlobal(rect().center()); }

void Sheet::placeCenter(const QPoint& c) {
    QRect g(QPoint(), size()); g.moveCenter(c);
    if (const QScreen* s = QGuiApplication::screenAt(c)) {   // keep the card on screen
        const QRect a = s->availableGeometry().adjusted(-kShadow, -kShadow, kShadow, kShadow);
        if (g.right() > a.right()) g.moveRight(a.right());
        if (g.left() < a.left()) g.moveLeft(a.left());
        if (g.bottom() > a.bottom()) g.moveBottom(a.bottom());
        if (g.top() < a.top()) g.moveTop(a.top());
    }
    move(g.topLeft());
}

void Sheet::relayout() {
    const QPoint c = isVisible() ? cardCenter() : QPoint();
    m_body->layout()->activate();
    const int h = qMax(120, m_body->layout()->totalHeightForWidth(kCardWidth));
    m_body->setGeometry(kShadow, kShadow, kCardWidth, h);
    resize(kCardWidth + 2 * kShadow, h + 2 * kShadow);
    if (isVisible()) placeCenter(c);
}

QPixmap Sheet::snapshot() {
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(size() * dpr); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const bool z = m_zooming; m_zooming = false;
        const double s = m_scale, o = m_opacity; m_scale = 1; m_opacity = 1;
        render(&p, QPoint(), QRegion(), QWidget::DrawWindowBackground);   // card only (children skipped below)
        m_zooming = z; m_scale = s; m_opacity = o;
        m_body->render(&p, m_body->pos(), QRegion(), QWidget::DrawChildren);
    }
    return pm;
}

void Sheet::runScale(double from, double to, double opFrom, double opTo, int ms, bool hideAfter, std::function<void()> done) {
    if (m_anim) { m_anim->stop(); m_anim->deleteLater(); }
    m_snap = snapshot();
    m_zooming = true; m_body->hide(); m_cover->hide();
    auto* a = new QVariantAnimation(this); m_anim = a;
    a->setStartValue(0.0); a->setEndValue(1.0); a->setDuration(m_reduced ? qMin(ms, 160) : ms);
    if (to >= from) { QEasingCurve back(QEasingCurve::OutBack); back.setOvershoot(1.25); a->setEasingCurve(back); }   // 1.9.0: soft overshoot
    else a->setEasingCurve(QEasingCurve::InCubic);
    connect(a, &QVariantAnimation::valueChanged, this, [this, from, to, opFrom, opTo](const QVariant& v){
        const double t = v.toDouble();
        m_scale = m_reduced ? 1.0 : from + (to - from) * t;
        m_opacity = qBound(0.0, opFrom + (opTo - opFrom) * qMin(1.0, t * 1.4), 1.0);
        update();
    });
    connect(a, &QVariantAnimation::finished, this, [this, hideAfter, done]{
        m_zooming = false; m_scale = 1; m_opacity = 1; m_snap = QPixmap();
        if (hideAfter) hide();
        m_body->show(); update();
        if (done) done();
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void Sheet::popIn() {
    relayout();
    m_scale = 0.82; m_opacity = 0;
    m_zooming = true;
    show(); raise();
    runScale(0.82, 1.0, 0.0, 1.0, 420, false, {});
}

void Sheet::popOut(std::function<void()> done) {
    if (!isVisible()) { if (done) done(); return; }
    runScale(1.0, 0.9, 1.0, 0.0, 220, true, std::move(done));
}

void Sheet::swap(const std::function<void()>& change) {
    if (!isVisible() || m_zooming) { change(); relayout(); return; }
    const QPixmap old = m_body->grab();
    change();
    relayout();
    auto* cover = static_cast<Cover*>(m_cover);
    cover->pm = old; cover->op = 1.0;
    cover->setGeometry(m_body->geometry());
    cover->show(); cover->raise();
    auto* a = new QVariantAnimation(cover);
    a->setStartValue(1.0); a->setEndValue(0.0); a->setDuration(m_reduced ? 100 : 200);
    connect(a, &QVariantAnimation::valueChanged, cover, [cover](const QVariant& v){ cover->op = v.toDouble(); cover->update(); });
    connect(a, &QVariantAnimation::finished, cover, [cover]{ cover->hide(); cover->pm = QPixmap(); });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void Sheet::lift() {
    auto* a = new QVariantAnimation(this);
    a->setStartValue(0.0); a->setEndValue(1.0); a->setDuration(m_reduced ? 150 : 520); a->setEasingCurve(QEasingCurve::OutCubic);
    const QPoint start = pos();
    connect(a, &QVariantAnimation::valueChanged, this, [this, start](const QVariant& v){
        const double t = v.toDouble(); m_glow = t;
        if (!m_reduced) move(start - QPoint(0, qRound(14 * t)));
        update();
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void Sheet::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && m_secondary->isVisible()) { m_secondary->click(); return; }
    QWidget::keyPressEvent(e);
}

void Sheet::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_zooming) {
        if (m_snap.isNull()) return;
        p.setOpacity(m_opacity);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QPointF c = QRectF(rect()).center();
        p.translate(c); p.scale(m_scale, m_scale); p.translate(-c);
        p.drawPixmap(0, 0, m_snap);
        return;
    }
    const QRectF card(kShadow, kShadow, width() - 2 * kShadow, height() - 2 * kShadow);
    // soft shadow: stacked translucent rounded rects, a little lower than the card
    p.setPen(Qt::NoPen);
    const int layers = 12;
    for (int i = layers; i >= 1; --i) {
        const double g = i * 2.2;
        p.setBrush(QColor(0, 0, 0, int(5 + (layers - i) * 1.1 + m_glow * 2)));
        p.drawRoundedRect(card.adjusted(-g, -g + 8, g, g + 8), 22 + g, 22 + g);
    }
    p.setBrush(m_c.bg);
    const QColor edge = m_glow > 0 ? QColor::fromRgbF(
        m_c.border.redF() + (m_c.accent.redF() - m_c.border.redF()) * m_glow * 0.6,
        m_c.border.greenF() + (m_c.accent.greenF() - m_c.border.greenF()) * m_glow * 0.6,
        m_c.border.blueF() + (m_c.accent.blueF() - m_c.border.blueF()) * m_glow * 0.6) : m_c.border;
    p.setPen(QPen(edge, 1));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 22, 22);
}

// --------------------------------------------------------------------------------------------- helper
namespace {
QString argValue(const QStringList& a, const QString& key) {
    const int i = a.indexOf(key);
    return i >= 0 && i + 1 < a.size() ? a.at(i + 1) : QString();
}

#ifdef Q_OS_WIN
bool processAlive(qint64 pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return false;
    const DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}
void killProcess(qint64 pid) {
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))) { TerminateProcess(h, 1); CloseHandle(h); }
}
// Processes still running from the install folder (QtWebEngineProcess.exe and friends): they keep
// files locked, and Setup in silent mode then stops instead of replacing them.
QList<qint64> processesIn(const QString& folder) {
    QList<qint64> out;
    if (folder.isEmpty()) return out;
    const QString want = QDir::cleanPath(QDir::fromNativeSeparators(folder)).toLower() + QLatin1Char('/');
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2]; DWORD n = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            const QString p = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(path, int(n)))).toLower();
            if (p.startsWith(want)) out << qint64(pe.th32ProcessID);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return out;
}
#else
bool processAlive(qint64 pid) { return pid > 0 && (::kill(pid_t(pid), 0) == 0 || errno == EPERM); }
void killProcess(qint64 pid) { if (pid > 0) ::kill(pid_t(pid), SIGKILL); }
#endif

// Start a program outside the AppImage environment we were started in.
bool startClean(const QString& program) {
    QProcess p;
    p.setProgram(program);
    auto env = QProcessEnvironment::systemEnvironment();
    for (const char* k : {"APPDIR", "APPIMAGE", "ARGV0", "OWD", "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH"}) env.remove(QString::fromLatin1(k));
    p.setProcessEnvironment(env);
    return p.startDetached();
}
}

int runInstallOverlay(const QStringList& args) {
    QApplication::setQuitOnLastWindowClosed(false);
    const qint64 pid = argValue(args, QStringLiteral("--pid")).toLongLong();
    const QString installer = argValue(args, QStringLiteral("--installer"));
    const QString relaunch = argValue(args, QStringLiteral("--relaunch"));
    const QString app = argValue(args, QStringLiteral("--app"));
    const QString version = argValue(args, QStringLiteral("--version"));
    const QString readyFile = argValue(args, QStringLiteral("--ready"));
    const QString marker = argValue(args, QStringLiteral("--marker"));
    const QString logFile = argValue(args, QStringLiteral("--log"));
    const bool reduced = args.contains(QStringLiteral("--reduced"));
    const QPoint center(argValue(args, QStringLiteral("--cx")).toInt(), argValue(args, QStringLiteral("--cy")).toInt());

    auto log = [logFile](const QString& s){
        if (logFile.isEmpty()) return;
        QFile f(logFile);
        if (f.open(QIODevice::Append | QIODevice::Text))
            f.write((QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz ")) + QStringLiteral("[update helper] ") + s + QLatin1Char('\n')).toUtf8());
    };
    log(QStringLiteral("started: pid=%1 installer=%2 relaunch=%3 app=%4").arg(pid).arg(installer, relaunch, app));
    if (!marker.isEmpty()) QFile::remove(marker);

    Sheet sheet(Colors::fromArg(argValue(args, QStringLiteral("--colors"))), nullptr, true);
    sheet.setReducedMotion(reduced);
    sheet.setWindowTitle(QStringLiteral("Updating Pacitum"));
    sheet.hero()->setMode(Hero::Spinner);
    sheet.title()->setText(QStringLiteral("Installing update…"));
    sheet.text()->setText(QStringLiteral("Pacitum closed to update itself. It opens again by itself in a moment. Don't turn off your computer."));
    sheet.detail()->setText(version.isEmpty() ? QString() : QStringLiteral("Pacitum %1").arg(version));
    sheet.detail()->setVisible(!version.isEmpty());
    sheet.setButtons({});
    sheet.relayout();
    if (center.isNull()) { if (auto* s = QGuiApplication::primaryScreen()) sheet.placeCenter(s->availableGeometry().center()); }
    else sheet.placeCenter(center);
    sheet.show(); sheet.raise();
    QTimer::singleShot(80, &sheet, [readyFile]{   // after the first paint: the browser may hide its copy now
        QFile f(readyFile); if (!readyFile.isEmpty() && f.open(QIODevice::WriteOnly)) f.write("1");
    });

    enum Phase { WaitExit, WaitHelpers, Installing, WaitMarker, Failed } phase = WaitExit;
    QElapsedTimer clock; clock.start();
    qint64 phaseStart = 0;
    bool killed = false;
#ifdef Q_OS_WIN
    HANDLE setup = nullptr;
#endif
    auto fail = [&](const QString& title, const QString& why){
        phase = Failed;
        log(QStringLiteral("failed: ") + why);
        sheet.swap([&]{
            sheet.hero()->setMode(Hero::Warn);
            sheet.title()->setText(title);
            sheet.text()->setText(why);
            sheet.detail()->hide();
            sheet.setButtons(app.isEmpty() ? QString() : QStringLiteral("Open Pacitum"), QStringLiteral("Close"));
            QObject::connect(sheet.primary(), &QPushButton::clicked, &sheet, [&]{ startClean(app); sheet.popOut([]{ QCoreApplication::quit(); }); });
            QObject::connect(sheet.secondary(), &QPushButton::clicked, &sheet, [&]{ sheet.popOut([]{ QCoreApplication::quit(); }); });
        });
        sheet.activateWindow();
    };
    auto waitForNewVersion = [&]{ phase = WaitMarker; phaseStart = clock.elapsed(); };

    QTimer tick; tick.setInterval(200);
    QObject::connect(&tick, &QTimer::timeout, &sheet, [&]{
        const qint64 inPhase = clock.elapsed() - phaseStart;
        switch (phase) {
        case WaitExit:
            if (pid > 0 && processAlive(pid) && inPhase < 27000) {
                if (inPhase > 25000 && !killed) { killed = true; log(QStringLiteral("browser still running after 25 s; ending it")); killProcess(pid); }
                return;
            }
            log(QStringLiteral("browser exited after %1 ms").arg(inPhase));
            phaseStart = clock.elapsed();
            phase = WaitHelpers;
            return;
        case WaitHelpers:
#ifdef Q_OS_WIN
            if (!installer.isEmpty() && !app.isEmpty()) {
                const QList<qint64> left = processesIn(QFileInfo(app).absolutePath());
                if (!left.isEmpty() && inPhase < 8000) return;
                if (!left.isEmpty()) {
                    log(QStringLiteral("%1 Pacitum process(es) still running after 8 s; ending them").arg(left.size()));
                    for (qint64 p : left) killProcess(p);
                }
            }
#endif
            phaseStart = clock.elapsed();
            if (!installer.isEmpty()) {
#ifdef Q_OS_WIN
                // ShellExecute (not CreateProcess) so Windows can ask for permission when Pacitum
                // was installed for all users.
                const std::wstring file = QDir::toNativeSeparators(installer).toStdWString();
                SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof(sei);
                sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
                sei.lpVerb = L"open"; sei.lpFile = file.c_str();
                sei.lpParameters = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /RELAUNCH=1";
                sei.nShow = SW_HIDE;
                if (!ShellExecuteExW(&sei) || !sei.hProcess) {
                    const DWORD err = GetLastError();
                    fail(QStringLiteral("Update not installed"), err == ERROR_CANCELLED
                         ? QStringLiteral("Windows didn't get permission to install it. Your current version still works.")
                         : QStringLiteral("Setup couldn't start (error %1). Your current version still works.").arg(err));
                    return;
                }
                setup = sei.hProcess;
                phase = Installing;
                log(QStringLiteral("setup started"));
#endif
            } else if (!relaunch.isEmpty()) {
                const bool ok = startClean(relaunch);
                log(QStringLiteral("relaunch %1 ok=%2").arg(relaunch).arg(ok));
                if (!ok) { fail(QStringLiteral("Couldn't restart"), QStringLiteral("Start Pacitum again yourself; the update is already in place.")); return; }
                waitForNewVersion();
            } else waitForNewVersion();
            return;
        case Installing:
#ifdef Q_OS_WIN
            if (WaitForSingleObject(setup, 0) == WAIT_OBJECT_0) {
                DWORD code = 1; GetExitCodeProcess(setup, &code); CloseHandle(setup); setup = nullptr;
                log(QStringLiteral("setup finished: exit code %1").arg(code));
                if (code != 0) { fail(QStringLiteral("Update not installed"), QStringLiteral("Setup stopped with code %1. Your current version still works.").arg(code)); return; }
                sheet.title()->setText(QStringLiteral("Starting Pacitum…"));
                waitForNewVersion();
            } else if (inPhase > 10 * 60 * 1000) {
                fail(QStringLiteral("Update is taking too long"), QStringLiteral("Setup hasn't finished after 10 minutes."));
            }
#endif
            return;
        case WaitMarker:
            if (!marker.isEmpty() && QFile::exists(marker)) {
                log(QStringLiteral("new version is up; closing"));
                tick.stop();
                QFile::remove(marker);
                QTimer::singleShot(250, &sheet, [&]{ sheet.popOut([]{ QCoreApplication::quit(); }); });
                return;
            }
            if (inPhase > 90000)
                fail(QStringLiteral("Pacitum didn't open by itself"), QStringLiteral("The update is installed. Open Pacitum to finish."));
            return;
        case Failed:
            return;
        }
    });
    tick.start();
    return QApplication::exec();
}

}
