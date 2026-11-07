#include "TabOverview.h"
#include "ThemeManager.h"
#include "AppSettings.h"
#include <QUrl>
#include <QRadialGradient>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtMath>
#include <QRegularExpression>
#include <cmath>
#include <algorithm>

TabOverview::TabOverview(const QList<Card>& cards, int current, QWidget* parent)
    : QWidget(parent), m_cards(cards) {
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Tab overview"));
    m_sel = qBound(0, current, qMax(0, int(m_cards.size()) - 1));
    m_active = m_sel;
    m_pos = m_sel;
    const QString style = AppSettings::get(QStringLiteral("switcher_style")).toString();
    if (style == QLatin1String("stack") || style == QLatin1String("grid")) m_mode = style;
    m_posAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_posAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_pos = v.toReal(); update(); });
    m_showAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_showAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_show = v.toReal(); update(); });
    connect(&m_showAnim, &QVariantAnimation::finished, this, [this]{
        if (m_show <= 0.0) { emit chosen(m_result); close(); }
    });
}

void TabOverview::open() {
    if (parentWidget()) setGeometry(parentWidget()->rect());
    prepareCards();
    show(); raise(); setFocus();
    const int ms = Pacitum::motionMs(240);
    if (ms <= 0) { m_show = 1.0; update(); return; }
    m_showAnim.setDuration(ms); m_showAnim.setStartValue(0.0); m_showAnim.setEndValue(1.0); m_showAnim.start();
}

// The design's 520 x 360 card (44 px title bar) at 1440 x 900, scaled with the window.
QSize TabOverview::cardSize() const {
    const QRectF st = stageRect();
    const qreal k = qBound(0.45, qMin(width() / 1440.0, st.height() / 610.0), 1.5);
    return QSize(int(520 * k), int(316 * k));   // the snapshot part; the title bar is added on top
}
QRectF TabOverview::stageRect() const { return QRectF(0, 88, width(), qMax(200, height() - 88 - 150)); }

void TabOverview::setMode(const QString& m) {
    if (m == m_mode) return;
    m_mode = m;
    AppSettings::set(QStringLiteral("switcher_style"), m);
    update();
}

// Where card i goes in the current style (offset from the animated position m_pos).
TabOverview::Place TabOverview::place(int i, const QSizeF& cs, const QPointF& c) const {
    Place pl;
    const int n = int(m_cards.size());
    const qreal k = cs.width() / 520.0;
    auto persp = [](qreal z) { return 1000.0 / (1000.0 - z); };   // translateZ under a 1000 px perspective
    if (m_mode == QLatin1String("stack")) {
        qreal d = std::fmod(i - m_pos + n, qreal(n));
        if (d > n - 0.5) d -= n;
        const qreal z = -qAbs(d) * 150, s = persp(z);
        pl.tr.translate(c.x(), c.y() - d * 46 * k * s + 20 * k);
        pl.tr.rotate(d > 0.01 ? 8 : 0, Qt::XAxis);
        pl.tr.scale(s, s);
        pl.opacity = qAbs(d) > 4.5 ? 0 : 1;
        pl.dim = qBound(0.0, 0.15 + qAbs(d) * 0.1, 0.7) * qMin(1.0, qAbs(d));
        pl.depth = qAbs(d);
    } else if (m_mode == QLatin1String("grid")) {
        const int cols = 3, rows = (n + cols - 1) / cols;
        const int col = i % cols, row = i / cols;
        const qreal gk = qMin(0.66, (stageRect().height() / qMax(1, rows)) / (cs.height() * 1.25));
        const qreal sel = i == m_sel ? gk * 1.06 : gk;
        pl.tr.translate(c.x() + (col - 1) * cs.width() * gk * 1.18, c.y() + (row - (rows - 1) / 2.0) * cs.height() * gk * 1.3);
        pl.tr.scale(sel, sel);
        pl.depth = i == m_sel ? 0 : 1;
    } else {   // carousel
        const qreal o = i - m_pos, a = qAbs(o), sg = o < 0 ? -1 : 1;
        const qreal x = a < 1 ? o * 300 : sg * (300 + (a - 1) * 150);
        const qreal z = a < 1 ? 60 - a * 240 : -180 - (a - 1) * 90;
        const qreal s = persp(z);
        pl.tr.translate(c.x() + x * k, c.y());
        pl.tr.rotate(-qBound(-1.0, o, 1.0) * 52, Qt::YAxis);
        pl.tr.scale(s, s);
        pl.opacity = a > 3.5 ? 0 : 1;
        pl.dim = qMin(1.0, a) * (0.28 + a * 0.08);
        pl.depth = a;
    }
    return pl;
}

// Pre-scale every snapshot once per size: the 3D frames then sample small pixmaps.
void TabOverview::prepareCards() {
    const QSize cs = cardSize() * devicePixelRatioF();
    m_scaled.clear();
    for (const Card& c : std::as_const(m_cards)) {
        QPixmap pm;
        if (!c.shot.isNull()) {
            pm = c.shot.scaled(cs, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            pm = pm.copy((pm.width() - cs.width()) / 2, 0, cs.width(), cs.height());
            pm.setDevicePixelRatio(devicePixelRatioF());
        }
        m_scaled << pm;
    }
}

void TabOverview::resizeEvent(QResizeEvent*) { prepareCards(); }

void TabOverview::select(int i, bool animate) {
    if (m_cards.isEmpty()) return;
    m_sel = qBound(0, i, int(m_cards.size()) - 1);
    const int ms = animate ? Pacitum::motionMs(260) : 0;
    m_posAnim.stop();
    if (ms <= 0) { m_pos = m_sel; update(); return; }
    m_posAnim.setDuration(ms); m_posAnim.setStartValue(m_pos); m_posAnim.setEndValue(qreal(m_sel)); m_posAnim.start();
}

void TabOverview::finish(int index) {
    if (m_closing) return;   // a second Esc / click while the close animation runs
    m_closing = true;
    m_result = index;
    if (index >= 0 && index != m_sel) select(index);
    const int ms = Pacitum::motionMs(180);
    if (ms <= 0) { emit chosen(m_result); close(); return; }
    m_showAnim.stop(); m_showAnim.setDuration(ms); m_showAnim.setStartValue(m_show); m_showAnim.setEndValue(0.0); m_showAnim.start();
}

void TabOverview::paintEvent(QPaintEvent*) {
    const auto& pal = ThemeManager::current();
    const QColor ink(0xF6, 0xF1, 0xEF), muted(0xA8, 0x9E, 0x9B), accent(pal.accent);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    // backdrop (design: radial, near-black)
    QRadialGradient g(QPointF(width() * 0.5, height() * 0.45), qMax(width(), height()) * 0.75);
    g.setColorAt(0, QColor(28, 22, 23, int(220 * m_show)));
    g.setColorAt(1, QColor(10, 8, 9, int(245 * m_show)));
    p.fillRect(rect(), g);
    if (m_cards.isEmpty()) return;
    m_buttons.clear();
    const qreal lift = (1.0 - m_show) * 40.0;
    auto font = [this](qreal px, int weight) { QFont f = this->font(); f.setPixelSize(qRound(px)); f.setWeight(QFont::Weight(weight)); f.setHintingPreference(QFont::PreferNoHinting); return f; };

    // ---- top bar: count, style switch, close
    p.setOpacity(m_show);
    const int n = int(m_cards.size());
    int asleep = 0; for (const auto& c : std::as_const(m_cards)) asleep += c.asleep ? 1 : 0;
    p.setPen(ink); p.setFont(font(28, QFont::DemiBold));
    const QString cnt = QString::number(n);
    const int cw = QFontMetrics(p.font()).horizontalAdvance(cnt);
    p.drawText(QRect(40, 0, cw + 4, 88), Qt::AlignVCenter | Qt::AlignLeft, cnt);
    p.setFont(font(15, QFont::Medium));
    p.drawText(QRect(40 + cw + 12, 26, 300, 20), Qt::AlignLeft | Qt::AlignVCenter, tr("tabs open"));
    p.setPen(muted); p.setFont(font(13, QFont::Normal));
    p.drawText(QRect(40 + cw + 12, 46, 300, 18), Qt::AlignLeft | Qt::AlignVCenter,
               asleep ? tr("%1 asleep, freeing memory").arg(asleep) : tr("all awake"));
    // close (X)
    const QRectF xr(width() - 40 - 40, 24, 40, 40);
    m_buttons << qMakePair(xr, QStringLiteral("close"));
    p.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap)); p.setBrush(Qt::NoBrush);
    const QPointF xc = xr.center();
    p.drawLine(xc + QPointF(-6, -6), xc + QPointF(6, 6)); p.drawLine(xc + QPointF(6, -6), xc + QPointF(-6, 6));
    // Carousel / Stack / Grid
    const QList<QPair<QString, QString>> modes{{QStringLiteral("carousel"), tr("Carousel")}, {QStringLiteral("stack"), tr("Stack")}, {QStringLiteral("grid"), tr("Grid")}};
    p.setFont(font(14, QFont::Medium));
    QList<qreal> ws; qreal total = 8;
    for (const auto& m : modes) { ws << QFontMetrics(p.font()).horizontalAdvance(m.second) + 28; total += ws.last() + 2; }
    QRectF seg(xr.left() - 16 - total, 22, total, 44);
    p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, 20)); p.drawRoundedRect(seg, 14, 14);
    qreal x = seg.left() + 4;
    for (int i = 0; i < modes.size(); ++i) {
        const QRectF b(x, seg.top() + 4, ws[i], 36);
        const bool on = modes[i].first == m_mode;
        if (on) { p.setBrush(ink); p.drawRoundedRect(b, 10, 10); }
        p.setPen(on ? QColor(0x15, 0x11, 0x12) : ink);
        p.drawText(b, Qt::AlignCenter, modes[i].second);
        p.setPen(Qt::NoPen);
        m_buttons << qMakePair(b, QStringLiteral("mode:") + modes[i].first);
        x += ws[i] + 2;
    }

    // ---- cards
    const QSize cs = cardSize();
    const qreal k = cs.width() / 520.0, head = 44 * k;
    const QRectF stage = stageRect();
    const QPointF centre(width() / 2.0, stage.center().y() + lift);
    const QRectF card(QPointF(-cs.width() / 2.0, -(cs.height() + head) / 2.0), QSizeF(cs.width(), cs.height() + head));
    m_hit = QList<QPolygonF>(n);
    QList<int> order;
    for (int i = 0; i < n; ++i) order << i;
    QList<Place> places; for (int i = 0; i < n; ++i) places << place(i, cs, centre);
    std::sort(order.begin(), order.end(), [&](int a, int b){ return places[a].depth > places[b].depth; });
    for (int i : order) {
        const Place& pl = places[i];
        if (pl.opacity <= 0.01) continue;
        p.save();
        p.setTransform(pl.tr);
        p.setOpacity(pl.opacity * m_show);
        const bool sel = i == m_sel;
        // shadow
        p.setPen(Qt::NoPen);
        for (int s = 0; s < 3; ++s) { p.setBrush(QColor(0, 0, 0, sel ? 38 : 28)); p.drawRoundedRect(card.adjusted(-s * 6, 10 + s * 8, s * 6, 18 + s * 12), 22 + s * 4, 22 + s * 4); }
        QPainterPath clip; clip.addRoundedRect(card, 20 * k, 20 * k);
        p.setClipPath(clip);
        p.fillRect(card, Qt::white);
        // title bar
        const QRectF hb(card.left(), card.top(), card.width(), head);
        p.fillRect(hb, Qt::white);
        p.fillRect(QRectF(hb.left(), hb.bottom() - 1, hb.width(), 1), QColor(0xE2, 0xDB, 0xD6));
        if (i == m_active) { p.setBrush(QColor(0xD9, 0x14, 0x1F)); p.drawRoundedRect(QRectF(hb.center().x() - 14 * k, hb.top() - 3, 28 * k, 6), 3, 3); }
        const QPixmap ic = m_cards[i].icon.pixmap(QSize(20, 20), devicePixelRatioF() * qMax(1.0, k));
        const QRectF icr(hb.left() + 16 * k, hb.center().y() - 10 * k, 20 * k, 20 * k);
        if (!ic.isNull()) p.drawPixmap(icr, ic, QRectF(ic.rect()));
        else { p.setBrush(QColor(0x5C, 0x54, 0x50)); p.drawRoundedRect(icr, 6 * k, 6 * k); }
        qreal right = hb.right() - 16 * k;
        if (m_cards[i].asleep) {
            p.setFont(font(11 * k, QFont::Medium)); p.setPen(QColor(0x67, 0x5E, 0x5A));
            const QString zz = tr("Asleep");
            const qreal zw = QFontMetricsF(p.font()).horizontalAdvance(zz);
            p.drawText(QRectF(right - zw, hb.top(), zw, head), Qt::AlignVCenter | Qt::AlignRight, zz);
            right -= zw + 10 * k;
        }
        p.setFont(font(13.5 * k, QFont::DemiBold)); p.setPen(QColor(0x15, 0x11, 0x12));
        const QRectF tr0(icr.right() + 10 * k, hb.top(), right - icr.right() - 10 * k, head);
        p.drawText(tr0, Qt::AlignVCenter | Qt::AlignLeft, QFontMetricsF(p.font()).elidedText(m_cards[i].title, Qt::ElideRight, tr0.width()));
        // snapshot
        const QRectF body(card.left(), hb.bottom(), card.width(), card.height() - head);
        const QPixmap& pm = m_scaled.value(i);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (!pm.isNull()) p.drawPixmap(body, pm, QRectF(pm.rect()));
        else {
            p.fillRect(body, QColor(0xF7, 0xF4, 0xF1));
            const QPixmap big = m_cards[i].icon.pixmap(QSize(48, 48), devicePixelRatioF());
            if (!big.isNull()) p.drawPixmap(QRectF(body.center().x() - 24 * k, body.center().y() - 24 * k, 48 * k, 48 * k), big, QRectF(big.rect()));
        }
        if (pl.dim > 0.01) p.fillRect(card, QColor(0, 0, 0, int(255 * qMin(0.75, pl.dim))));
        p.setClipping(false);
        if (sel) { p.setBrush(Qt::NoBrush); p.setPen(QPen(accent, 3)); p.drawRoundedRect(card.adjusted(-1.5, -1.5, 1.5, 1.5), 21 * k, 21 * k); }
        p.restore();
        m_hit[i] = pl.tr.map(QPolygonF(card));
    }

    // ---- arrows (not in Grid)
    p.setOpacity(m_show);
    if (m_mode != QLatin1String("grid") && n > 1) {
        for (int side = 0; side < 2; ++side) {
            const QRectF b(side ? width() - 36 - 56 : 36, stage.center().y() - 28, 56, 56);
            p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, 20)); p.drawRoundedRect(b, 18, 18);
            p.setPen(QPen(ink, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
            const QPointF c0 = b.center(); const qreal d = side ? 1 : -1;
            p.drawPolyline(QPolygonF{c0 + QPointF(-d * 4, -8), c0 + QPointF(d * 4, 0), c0 + QPointF(-d * 4, 8)});
            m_buttons << qMakePair(b, QString::fromLatin1(side ? "next" : "prev"));
        }
    }

    // ---- selected title, address, dots, hints
    const qreal by = stage.bottom() + 6;
    const Card& cur = m_cards.value(m_sel);
    p.setPen(ink); p.setFont(font(22, QFont::DemiBold));
    p.drawText(QRectF(60, by, width() - 120, 30), Qt::AlignCenter, QFontMetrics(p.font()).elidedText(cur.title, Qt::ElideRight, width() - 160));
    const QUrl u(cur.url);
    p.setPen(muted); p.setFont(font(14, QFont::Normal));
    const QString addr = u.scheme().startsWith(QLatin1String("http")) ? u.host().remove(QRegularExpression(QStringLiteral("^www\\."))) + u.path() : cur.url;
    p.drawText(QRectF(60, by + 32, width() - 120, 20), Qt::AlignCenter, QFontMetrics(p.font()).elidedText(addr, Qt::ElideMiddle, width() - 160));
    // dots
    const int maxDots = qMin(n, 30);
    const qreal dw = (maxDots - 1) * 12 + 28;
    qreal dx = width() / 2.0 - dw / 2.0;
    for (int i = 0; i < maxDots; ++i) {
        const bool on = i == qMin(m_sel, maxDots - 1);
        const QRectF d(dx, by + 66, on ? 28 : 6, 6);
        p.setPen(Qt::NoPen); p.setBrush(on ? accent : QColor(255, 255, 255, 77)); p.drawRoundedRect(d, 3, 3);
        m_buttons << qMakePair(d.adjusted(-3, -8, 3, 8), QStringLiteral("dot:%1").arg(i));
        dx += (on ? 28 : 6) + 6;
    }
    // keyboard hints
    struct Hint { QStringList keys; QString text; bool after; };
    const QList<Hint> hints{{{QStringLiteral("←"), QStringLiteral("→")}, tr("browse"), true}, {{QStringLiteral("Enter")}, tr("open"), true},
                            {{QStringLiteral("Esc")}, tr("cancel"), true}, {{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}, tr("style"), true}};
    QFont kf = font(11.5, QFont::DemiBold), hf = font(13, QFont::Normal);
    qreal hwTotal = 0; QList<qreal> hw;
    for (const auto& h : hints) {
        qreal w = 0; for (const auto& key : h.keys) w += qMax(26, QFontMetrics(kf).horizontalAdvance(key) + 14) + 6;
        w += QFontMetrics(hf).horizontalAdvance(h.text); hw << w; hwTotal += w + 22;
    }
    qreal hx = width() / 2.0 - (hwTotal - 22) / 2.0;
    const qreal hy = by + 92;
    for (int i = 0; i < hints.size(); ++i) {
        qreal x0 = hx;
        for (const auto& key : hints[i].keys) {
            const qreal kw = qMax(26, QFontMetrics(kf).horizontalAdvance(key) + 14);
            const QRectF kr(x0, hy, kw, 24);
            p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, 26)); p.drawRoundedRect(kr, 7, 7);
            p.setBrush(QColor(0, 0, 0, 90)); p.drawRoundedRect(QRectF(kr.left() + 1, kr.bottom() - 2, kr.width() - 2, 2), 1, 1);
            p.setPen(ink); p.setFont(kf); p.drawText(kr, Qt::AlignCenter, key);
            x0 += kw + 6;
        }
        p.setPen(muted); p.setFont(hf);
        p.drawText(QRectF(x0, hy, hw[i], 24), Qt::AlignVCenter | Qt::AlignLeft, hints[i].text);
        hx += hw[i] + 22;
    }
}

void TabOverview::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Escape: finish(-1); return;
    case Qt::Key_Left: case Qt::Key_Up: select(m_sel - 1); return;
    case Qt::Key_Right: case Qt::Key_Down: case Qt::Key_Tab: select(m_sel + 1); return;
    case Qt::Key_Backtab: select(m_sel - 1); return;
    case Qt::Key_Home: select(0); return;
    case Qt::Key_1: setMode(QStringLiteral("carousel")); return;
    case Qt::Key_2: setMode(QStringLiteral("stack")); return;
    case Qt::Key_3: setMode(QStringLiteral("grid")); return;
    case Qt::Key_End: select(int(m_cards.size()) - 1); return;
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space: finish(m_sel); return;
    default: QWidget::keyPressEvent(e);
    }
}

void TabOverview::wheelEvent(QWheelEvent* e) {
    m_wheelAcc += (e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x());
    while (m_wheelAcc >= 120) { m_wheelAcc -= 120; select(m_sel - 1); }
    while (m_wheelAcc <= -120) { m_wheelAcc += 120; select(m_sel + 1); }
    e->accept();
}

void TabOverview::mousePressEvent(QMouseEvent* e) {
    for (const auto& b : std::as_const(m_buttons)) {
        if (!b.first.contains(e->position())) continue;
        const QString& a = b.second;
        if (a == QLatin1String("close")) finish(-1);
        else if (a == QLatin1String("prev")) select(m_sel - 1 < 0 ? int(m_cards.size()) - 1 : m_sel - 1);
        else if (a == QLatin1String("next")) select((m_sel + 1) % int(m_cards.size()));
        else if (a.startsWith(QLatin1String("mode:"))) setMode(a.mid(5));
        else if (a.startsWith(QLatin1String("dot:"))) select(a.mid(4).toInt());
        return;
    }
    // topmost (nearest) card under the cursor wins
    int best = -1; qreal bestDist = 1e9;
    for (int i = 0; i < m_hit.size(); ++i)
        if (!m_hit[i].isEmpty() && m_hit[i].containsPoint(e->position(), Qt::OddEvenFill) && qAbs(i - m_pos) < bestDist) { best = i; bestDist = qAbs(i - m_pos); }
    if (best < 0) { finish(-1); return; }   // click on the backdrop closes
    if (best == m_sel) finish(best); else select(best);
}
