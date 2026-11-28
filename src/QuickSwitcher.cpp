#include "QuickSwitcher.h"
#include "ThemeManager.h"
#include "Lang.h"
#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>

namespace {
constexpr int kField = 56, kRow = 46, kHeader = 30, kFooter = 34, kPad = 8, kMaxList = 430;
QColor col(const QString& c, qreal alpha = 1.0) { QColor q(c); q.setAlphaF(q.alphaF() * alpha); return q; }
}

QuickSwitcher::QuickSwitcher(Provider provider, QWidget* parent) : QWidget(parent), m_provider(std::move(provider)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setAccessibleName(Lang::tr(QStringLiteral("Quick switcher")));
    m_field = new QLineEdit(this);
    m_field->setFrame(false);
    m_field->setPlaceholderText(Lang::tr(QStringLiteral("Search tabs, bookmarks, history and actions")));
    m_field->setAccessibleName(Lang::tr(QStringLiteral("Quick switcher")));
    m_field->installEventFilter(this);
    m_fieldOpacity = new QGraphicsOpacityEffect(m_field);
    m_fieldOpacity->setOpacity(0.0);
    m_field->setGraphicsEffect(m_fieldOpacity);
    const PacitumPalette& p = ThemeManager::current();
    m_field->setStyleSheet(QStringLiteral("QLineEdit{background:transparent;border:0;color:%1;font-size:16px;selection-background-color:%2;selection-color:%3}")
                               .arg(p.text, p.primary, p.onPrimary));
    connect(m_field, &QLineEdit::textChanged, this, [this]{ refresh(); });
    m_anim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        m_show = v.toReal();
        if (m_fieldOpacity) m_fieldOpacity->setOpacity(m_show);
        updateFieldGeometry();
        update();
    });
    connect(&m_anim, &QVariantAnimation::finished, this, [this]{
        if (!m_closing) return;
        auto then = std::move(m_then);
        emit closed();
        close();
        if (then) then();
    });
}

void QuickSwitcher::open() {
    if (parentWidget()) setGeometry(parentWidget()->rect());
    refresh();
    m_field->setEnabled(true);
    show(); raise();
    m_field->setFocus();
    const int ms = Pacitum::motionMs(140);
    if (ms <= 0) {
        m_show = 1.0;
        if (m_fieldOpacity) m_fieldOpacity->setOpacity(1.0);
        updateFieldGeometry();
        update();
        return;
    }
    m_show = 0.0;
    if (m_fieldOpacity) m_fieldOpacity->setOpacity(0.0);
    updateFieldGeometry();
    m_anim.setDuration(ms); m_anim.setStartValue(0.0); m_anim.setEndValue(1.0); m_anim.start();
}

QString QuickSwitcher::query() const { return m_field->text(); }
void QuickSwitcher::setQuery(const QString& q) { m_field->setText(q); }

QStringList QuickSwitcher::words(const QString& query) {
    return query.simplified().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

int QuickSwitcher::score(const QStringList& words, const QString& title, const QString& sub, const QString& keywords) {
    if (words.isEmpty()) return 0;
    const QString t = title.toLower(), s = sub.toLower(), k = keywords.toLower();
    const QString q = words.join(QLatin1Char(' '));
    int total = 0;
    for (const QString& w : words) {
        int best = -1;
        const int ti = t.indexOf(w);
        if (ti == 0) best = 40;
        else if (ti > 0) best = t.at(ti - 1).isLetterOrNumber() ? 14 : 26;   // inside a word / at a word start
        if (best < 0 && k.contains(w)) best = 18;
        if (best < 0) {
            const int si = s.indexOf(w);
            if (si >= 0) best = (si == 0 || !s.at(si - 1).isLetterOrNumber()) ? 12 : 6;
        }
        if (best < 0) return -1;
        total += best;
    }
    if (t == q) total += 60;
    else if (t.startsWith(q)) total += 30;
    return total;
}

QString QuickSwitcher::kindLabel(Item::Kind k) {
    switch (k) {
    case Item::Tab: return Lang::tr(QStringLiteral("Open tabs"));
    case Item::Action: return Lang::tr(QStringLiteral("Actions"));
    case Item::Bookmark: return Lang::tr(QStringLiteral("Bookmarks"));
    case Item::History: return Lang::tr(QStringLiteral("History"));
    case Item::Go: case Item::Web: return QString();
    }
    return QString();
}

void QuickSwitcher::refresh() {
    m_items = m_provider ? m_provider(m_field->text()) : QList<Item>();
    // the provider sorts within a kind; keep the kinds in their fixed order (stable)
    std::stable_sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b){ return a.kind < b.kind; });
    m_scroll = 0;
    layoutRows();
    m_sel = m_items.isEmpty() ? -1 : 0;
    update();
}

void QuickSwitcher::layoutRows() {
    m_rows.clear();
    int y = 0;
    int lastKind = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        const Item& it = m_items[i];
        if (int(it.kind) != lastKind) {
            const QString h = kindLabel(it.kind);
            if (!h.isEmpty()) { m_rows << Row{-1, h, y, kHeader}; y += kHeader; }
            else if (lastKind >= 0) y += 6;
            lastKind = int(it.kind);
        }
        m_rows << Row{i, QString(), y, kRow};
        y += kRow;
    }
    m_contentH = y;
}

QRect QuickSwitcher::cardRect() const {
    const int w = qMin(660, width() - 32);
    const int listH = m_items.isEmpty() ? 64 : qMin(kMaxList, m_contentH + kPad * 2);
    const int h = kField + 1 + listH + kFooter;
    const int top = qMax(24, int(height() * 0.11));
    return QRect((width() - w) / 2, top, w, qMin(h, height() - top - 16));
}

QRect QuickSwitcher::listRect() const {
    const QRect c = cardRect();
    return QRect(c.left() + kPad, c.top() + kField + 1 + kPad, c.width() - kPad * 2, c.height() - kField - 1 - kFooter - kPad * 2);
}

void QuickSwitcher::updateFieldGeometry() {
    QRect c = cardRect();
    if (m_show < 1.0) c.translate(0, int(-10 * (1.0 - m_show)));
    m_field->setGeometry(c.left() + 52, c.top() + 10, c.width() - 52 - 18, kField - 20);
}

void QuickSwitcher::resizeEvent(QResizeEvent*) {
    updateFieldGeometry();
}

void QuickSwitcher::select(int item) {
    if (m_items.isEmpty()) { m_sel = -1; return; }
    m_sel = qBound(0, item, int(m_items.size()) - 1);
    ensureVisible();
    update();
}

void QuickSwitcher::ensureVisible() {
    const QRect lr = listRect();
    for (const Row& r : std::as_const(m_rows)) {
        if (r.item != m_sel) continue;
        int top = r.y;
        // keep the group header in view with the first row of its group
        for (const Row& h : std::as_const(m_rows)) if (h.item < 0 && h.y + h.h == r.y) top = h.y;
        if (top < m_scroll) m_scroll = top;
        else if (r.y + r.h > m_scroll + lr.height()) m_scroll = r.y + r.h - lr.height();
        break;
    }
    m_scroll = qBound(0, m_scroll, qMax(0, m_contentH - lr.height()));
}

int QuickSwitcher::rowAt(const QPoint& p) const {
    const QRect lr = listRect();
    if (!lr.contains(p)) return -1;
    const int y = p.y() - lr.top() + m_scroll;
    for (const Row& r : std::as_const(m_rows)) if (r.item >= 0 && y >= r.y && y < r.y + r.h) return r.item;
    return -1;
}

void QuickSwitcher::activate(int index) {
    if (index < 0 || index >= m_items.size() || m_closing) return;
    finish(m_items[index].run);
}

void QuickSwitcher::finish(std::function<void()> then) {
    if (m_closing) return;
    m_closing = true;
    m_then = std::move(then);
    m_field->clearFocus();
    m_field->setEnabled(false);
    const int ms = Pacitum::motionMs(110);
    if (ms <= 0) {
        auto t = std::move(m_then);
        emit closed(); close();
        if (t) t();
        return;
    }
    m_anim.stop();
    m_anim.setDuration(ms); m_anim.setStartValue(m_show); m_anim.setEndValue(0.0); m_anim.start();
}

bool QuickSwitcher::eventFilter(QObject* o, QEvent* e) {
    // The window's own shortcuts (Esc stops loading, Ctrl+K opens this box…) would take these keys first
    if (o == m_field && e->type() == QEvent::ShortcutOverride) {
        auto* k = static_cast<QKeyEvent*>(e);
        switch (k->key()) {
        case Qt::Key_Escape: case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Up: case Qt::Key_Down:
        case Qt::Key_PageUp: case Qt::Key_PageDown: case Qt::Key_Tab: case Qt::Key_Backtab:
            e->accept(); return true;
        case Qt::Key_K:
            if (k->modifiers() & Qt::ControlModifier) { e->accept(); return true; }
            break;
        default: break;
        }
    }
    if (o == m_field && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        const int page = qMax(1, listRect().height() / kRow - 1);
        switch (k->key()) {
        case Qt::Key_Escape: finish({}); return true;
        case Qt::Key_Down: select(m_sel + 1); return true;
        case Qt::Key_Up: select(m_sel - 1); return true;
        case Qt::Key_Tab: select(m_sel + 1 >= m_items.size() ? 0 : m_sel + 1); return true;
        case Qt::Key_Backtab: select(m_sel <= 0 ? int(m_items.size()) - 1 : m_sel - 1); return true;
        case Qt::Key_PageDown: select(m_sel + page); return true;
        case Qt::Key_PageUp: select(m_sel - page); return true;
        case Qt::Key_Return: case Qt::Key_Enter: activate(m_sel); return true;
        case Qt::Key_K:
            if (k->modifiers() & Qt::ControlModifier) { finish({}); return true; }   // Ctrl+K again closes
            break;
        default: break;
        }
    }
    return QWidget::eventFilter(o, e);
}

void QuickSwitcher::mousePressEvent(QMouseEvent* e) {
    const QPoint p = e->position().toPoint();
    if (!cardRect().contains(p)) { finish({}); return; }
    const int r = rowAt(p);
    if (r >= 0 && e->button() == Qt::LeftButton) activate(r);
    m_field->setFocus();
}

void QuickSwitcher::mouseMoveEvent(QMouseEvent* e) {
    const int r = rowAt(e->position().toPoint());
    if (r >= 0 && r != m_sel) { m_sel = r; update(); }
}

void QuickSwitcher::wheelEvent(QWheelEvent* e) {
    const QRect lr = listRect();
    m_scroll = qBound(0, m_scroll - e->angleDelta().y() / 2, qMax(0, m_contentH - lr.height()));
    update();
}

void QuickSwitcher::paintEvent(QPaintEvent*) {
    const PacitumPalette& p = ThemeManager::current();
    const bool dark = QColor(p.page).lightnessF() < 0.5;
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    g.fillRect(rect(), QColor(0, 0, 0, int((dark ? 120 : 70) * m_show)));
    if (m_show <= 0.0) return;

    g.setOpacity(m_show);
    QRect c = cardRect();
    c.translate(0, int(-10 * (1.0 - m_show)));
    // shadow
    for (int i = 6; i >= 1; --i) {
        QPainterPath sp; sp.addRoundedRect(QRectF(c).adjusted(-i * 1.5, -i + 4, i * 1.5, i * 2.5 + 4), 16 + i, 16 + i);
        g.fillPath(sp, QColor(0, 0, 0, dark ? 16 : 9));
    }
    QPainterPath card; card.addRoundedRect(QRectF(c), 16, 16);
    g.fillPath(card, col(p.elevated()));
    g.setPen(QPen(col(p.border), 1)); g.drawPath(card);
    // field
    ThemeManager::icon(QStringLiteral("search"), p.textSecondary, 20).paint(&g, QRect(c.left() + 20, c.top() + (kField - 20) / 2, 20, 20));
    g.setPen(col(p.border)); g.drawLine(c.left() + 1, c.top() + kField, c.right() - 1, c.top() + kField);

    QFont f = font();
    const QRect lr = listRect().translated(0, c.top() - cardRect().top());
    if (m_items.isEmpty()) {
        f.setPixelSize(13); g.setFont(f); g.setPen(col(p.textSecondary));
        g.drawText(lr, Qt::AlignCenter, Lang::tr(QStringLiteral("Nothing matches")));
    }
    g.save();
    g.setClipRect(lr);
    for (const Row& r : std::as_const(m_rows)) {
        const QRect rr(lr.left(), lr.top() + r.y - m_scroll, lr.width(), r.h);
        if (rr.bottom() < lr.top() || rr.top() > lr.bottom()) continue;
        if (r.item < 0) {
            f.setPixelSize(11); f.setWeight(QFont::DemiBold); f.setCapitalization(QFont::AllUppercase); f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            g.setFont(f); g.setPen(col(p.textTertiary.isEmpty() ? p.textSecondary : p.textTertiary));
            g.drawText(rr.adjusted(12, 8, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, r.header);
            f.setCapitalization(QFont::MixedCase); f.setLetterSpacing(QFont::AbsoluteSpacing, 0);
            continue;
        }
        const Item& it = m_items[r.item];
        if (r.item == m_sel) {
            QPainterPath hp; hp.addRoundedRect(QRectF(rr).adjusted(0, 2, 0, -2), 10, 10);
            g.fillPath(hp, col(p.surfaceHover));
        }
        const QRect ic(rr.left() + 12, rr.center().y() - 9, 18, 18);
        if (!it.icon.isNull()) it.icon.paint(&g, ic);
        int right = rr.right() - 12;
        if (!it.hint.isEmpty()) {
            f.setPixelSize(12); f.setWeight(QFont::Normal); g.setFont(f);
            const int hw = QFontMetrics(f).horizontalAdvance(it.hint) + 16;
            const QRect hr(right - hw, rr.center().y() - 11, hw, 22);
            if (it.kind == Item::Action) {
                QPainterPath kp; kp.addRoundedRect(QRectF(hr), 6, 6);
                g.fillPath(kp, col(p.surface));
                g.setPen(QPen(col(p.border), 1)); g.drawPath(kp);
            }
            g.setPen(col(p.textSecondary));
            g.drawText(hr, Qt::AlignCenter, it.hint);
            right = hr.left() - 12;
        }
        const int tx = ic.right() + 14;
        const QRect textR(tx, rr.top(), qMax(10, right - tx), rr.height());
        QFont tf = font(); tf.setPixelSize(14); tf.setWeight(QFont::Medium);
        QFont sf = font(); sf.setPixelSize(12);
        const QFontMetrics fm(tf), sm(sf);
        g.setFont(tf); g.setPen(col(p.text));
        if (it.sub.isEmpty()) {
            g.drawText(textR, Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(it.title, Qt::ElideRight, textR.width()));
        } else {
            g.drawText(QRect(textR.left(), rr.top() + 5, textR.width(), 20), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(it.title, Qt::ElideRight, textR.width()));
            g.setFont(sf); g.setPen(col(p.textSecondary));
            g.drawText(QRect(textR.left(), rr.top() + 24, textR.width(), 17), Qt::AlignLeft | Qt::AlignVCenter, sm.elidedText(it.sub, Qt::ElideMiddle, textR.width()));
        }
    }
    g.restore();
    // footer: keyboard hints
    const QRect fr(c.left(), c.bottom() - kFooter + 1, c.width(), kFooter);
    g.setPen(col(p.border)); g.drawLine(fr.left() + 1, fr.top(), fr.right() - 1, fr.top());
    f.setPixelSize(12); f.setWeight(QFont::Normal); g.setFont(f); g.setPen(col(p.textSecondary));
    const QString hints = Lang::tr(QStringLiteral("↑↓ to move · Enter to open · Esc to close"));
    g.drawText(fr.adjusted(18, 0, -18, 0), Qt::AlignLeft | Qt::AlignVCenter, hints);
    g.drawText(fr.adjusted(18, 0, -18, 0), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Ctrl+K"));
}
