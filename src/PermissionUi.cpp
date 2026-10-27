#include "PermissionUi.h"
#include "ThemeManager.h"
#include "Motion.h"
#include "Widgets.h"
#include <QAccessible>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSvgRenderer>
#include <QFile>
#include <QTimer>
#include <QToolButton>
#include <QAbstractListModel>
#include <QGuiApplication>
#include <QWindow>
#include <QJsonObject>
#include <QStyle>
#include <QtMath>

namespace PermUi {

static QColor qc(const QString& s) { return QColor(s); }
static QString rgba(const QString& c, double a) { QColor q(c); return QStringLiteral("rgba(%1,%2,%3,%4)").arg(q.red()).arg(q.green()).arg(q.blue()).arg(a); }

QPixmap glyphPixmap(const QString& name, const QString& color, int size, qreal dpr) {
    return ThemeManager::icon(name, color, size).pixmap(QSize(size, size), dpr);
}
QString stateColor(Perm::Decision d) {
    const auto& p = ThemeManager::current();
    return d == Perm::Decision::Allow ? p.secure : d == Perm::Decision::Block ? p.danger : p.textSecondary;
}
QIcon letterIcon(const QString& host, int size) {
    const QString reg = Perm::registrableDomain(host);
    const QChar ch = reg.isEmpty() ? QChar('?') : reg.at(0).toUpper();
    static const char* colors[] = {"#1a73e8", "#188038", "#d93025", "#e37400", "#9334e6", "#007b83", "#3c4043"};
    const QColor bg(colors[qHash(reg) % 7]);
    QPixmap pm(size * 2, size * 2); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(bg); p.setPen(Qt::NoPen); p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.3, size * 0.3);
    QFont f = qApp->font(); f.setPixelSize(int(size * 0.55)); f.setBold(true); p.setFont(f); p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QString(ch));
    return QIcon(pm);
}
void announce(QWidget* w, const QString& text) {
    if (!w) return;
    w->setAccessibleDescription(text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (QAccessible::isActive()) { QAccessibleAnnouncementEvent ev(w, text); QAccessible::updateAccessibility(&ev); }
#endif
}

// shadow-popup approximation: stacked translucent rounded rects (9-slice equivalent, cheap)
static void paintCard(QPainter& p, const QRectF& card, qreal radius) {
    radius = qMax<qreal>(radius, 20);   // 2.0.1 design: 20 px cards
    const auto& pal = ThemeManager::current();
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 10; i >= 1; --i) {
        QColor c(0, 0, 0, int(3.2 * (11 - i) * 0.55));
        p.setPen(Qt::NoPen); p.setBrush(c);
        p.drawRoundedRect(card.adjusted(-i, -i + 4, i, i + 4), radius + i, radius + i);
    }
    p.setBrush(qc(pal.card())); p.setPen(QPen(qc(pal.border), 1));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}
static constexpr int kShadow = 14;

static QString pillStyle() {
    const auto& p = ThemeManager::current();
    return QStringLiteral(
        "QPushButton{background:%1;color:%2;border:none;border-radius:12px;min-height:38px;font-size:13.5px;font-weight:600;padding:0 14px;}"
        "QPushButton:hover{background:%3;} QPushButton:pressed{background:%4;}"
        "QPushButton:focus{outline:none;border:2px solid %5;}"
        "QPushButton:disabled{color:%6;}"
        "QPushButton[guard=\"true\"]{color:%6;}"
        "QPushButton[primary=\"true\"]{background:%7;color:%8;} QPushButton[primary=\"true\"]:hover{background:%9;}"
        "QPushButton[primary=\"true\"]:disabled{background:%1;color:%6;}")
        .arg(p.omnibox, p.text, p.surfaceHover, p.pressed, p.focusRing, p.disabled, p.primary, p.onPrimary, QColor(p.primary).lightness() > 128 ? QColor(p.primary).darker(112).name() : QColor(p.primary).lighter(160).name());
}

// ------------------------------------------------------------------ Bubble
Bubble::Bubble(QWidget* window) : QWidget(window, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAccessibleName(QStringLiteral("Permission request"));
    setProperty("accessibleRole", QStringLiteral("dialog"));
    const auto& pal = ThemeManager::current();
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadow + 16, kShadow + 16, kShadow + 16, kShadow + 16);
    outer->setSpacing(12);
    auto* head = new QHBoxLayout(); head->setSpacing(10);
    m_favicon = new QLabel(this); m_favicon->setFixedSize(20, 20);
    m_question = new QLabel(this); m_question->setTextFormat(Qt::RichText); m_question->setWordWrap(true);
    m_question->setStyleSheet(QStringLiteral("color:%1;font-size:12.5px;").arg(pal.textSecondary));
    m_close = new QToolButton(this); m_close->setFixedSize(34, 34); m_close->setAutoRaise(true);
    m_close->setIcon(ThemeManager::icon(QStringLiteral("close"), pal.textSecondary, 16)); m_close->setIconSize(QSize(16, 16));
    m_close->setAccessibleName(QStringLiteral("Close"));
    m_close->setStyleSheet(QStringLiteral("QToolButton{border:none;border-radius:17px;} QToolButton:hover{background:%1;}").arg(pal.surfaceHover));
    connect(m_close, &QToolButton::clicked, this, [this]{ emit closed(true); dismiss(); });
    head->addWidget(m_favicon, 0, Qt::AlignTop); head->addWidget(m_question, 1); head->addWidget(m_close, 0, Qt::AlignTop);
    head->setContentsMargins(0, 0, -6, 0);
    outer->addLayout(head);
    m_body = new QVBoxLayout(); m_body->setSpacing(12); outer->addLayout(m_body);
    m_actionsBox = new QWidget(this); outer->addWidget(m_actionsBox);
    m_anim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        const qreal t = v.toReal();
        setWindowOpacity(t);
        if (Motion::enabled()) move(m_target + QPoint(0, int(qRound((1.0 - t) * -4.0))));
    });
    connect(&m_anim, &QVariantAnimation::finished, this, [this]{ if (m_closing) { hide(); deleteLater(); } });
    qApp->installEventFilter(this);
}

void Bubble::setHeader(const QIcon& fav, const QString& rich, const QString& plain) {
    m_favicon->setPixmap(fav.pixmap(QSize(20, 20), devicePixelRatioF()));
    m_question->setText(rich);
    m_question->setAccessibleName(plain);
    m_a11y = plain;
    setAccessibleName(plain);
}

void Bubble::addCapability(const QString& glyph, const QString& label, const QString& detail, bool warning) {
    const auto& pal = ThemeManager::current();
    auto* row = new QWidget(this); auto* l = new QHBoxLayout(row); l->setContentsMargins(0, 0, 0, 0); l->setSpacing(12);
    auto* tile = new QLabel(row); tile->setFixedSize(32, 32); tile->setAlignment(Qt::AlignCenter);
    const QString tint = warning ? pal.warning : pal.accent;
    tile->setStyleSheet(QStringLiteral("background:%1;border-radius:10px;").arg(warning ? rgba(pal.warning, 0.16) : pal.omnibox));
    tile->setPixmap(glyphPixmap(glyph, warning ? tint : pal.text, 18, devicePixelRatioF()));
    auto* texts = new QVBoxLayout(); texts->setSpacing(1);
    auto* t = new QLabel(label, row); t->setWordWrap(true); t->setStyleSheet(QStringLiteral("color:%1;font-size:15px;font-weight:600;").arg(pal.text));
    texts->addWidget(t);
    if (!detail.isEmpty()) { auto* d = new QLabel(detail, row); d->setWordWrap(true); d->setStyleSheet(QStringLiteral("color:%1;font-size:12.5px;").arg(pal.textSecondary)); texts->addWidget(d); }
    if (detail.isEmpty()) l->setAlignment(Qt::AlignVCenter);
    l->addWidget(tile, 0, Qt::AlignTop); l->addLayout(texts, 1);
    m_body->addWidget(row);
}

void Bubble::addWidget(QWidget* w) { w->setParent(this); m_body->addWidget(w); }

void Bubble::addNote(const QString& text, bool warning) {
    const auto& pal = ThemeManager::current();
    auto* box = new QWidget(this); auto* l = new QHBoxLayout(box); l->setContentsMargins(12, 10, 12, 10); l->setSpacing(10);
    if (warning) {
        box->setObjectName(QStringLiteral("PermWarn"));
        box->setStyleSheet(QStringLiteral("#PermWarn{background:%1;border-radius:10px;}").arg(rgba(pal.warning, 0.14)));
        auto* ic = new QLabel(box); ic->setPixmap(glyphPixmap(QStringLiteral("warning"), pal.warning, 18, devicePixelRatioF())); l->addWidget(ic, 0, Qt::AlignTop);
    } else l->setContentsMargins(0, 0, 0, 0);
    auto* t = new QLabel(text, box); t->setWordWrap(true); t->setTextFormat(Qt::RichText);
    t->setStyleSheet(QStringLiteral("color:%1;font-size:12.5px;").arg(warning ? pal.text : pal.textSecondary));
    l->addWidget(t, 1);
    m_body->addWidget(box);
}

void Bubble::setActions(const QList<Action>& actions, bool row, const QString& focusId) {
    delete m_actionsBox->layout();
    qDeleteAll(m_buttons); m_buttons.clear();
    // 2.0.1 design: a primary action spans the card; the other choices share one row below it.
    // Row prompts (two choices) split the width evenly.
    auto* l = new QVBoxLayout(m_actionsBox);
    l->setContentsMargins(0, 0, 0, 0); l->setSpacing(6);
    QHBoxLayout* pairRow = nullptr;
    int secondary = 0; for (const auto& a : actions) if (!a.primary) ++secondary;
    for (const auto& a : actions) {
        auto* b = new QPushButton(a.text, m_actionsBox);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setProperty("actionId", a.id);
        b->setProperty("primary", a.primary);
        b->setEnabled(a.enabled);
        b->setCursor(Qt::PointingHandCursor);
        b->setAutoDefault(false);
        if (!a.icon.isEmpty()) { b->setIcon(ThemeManager::icon(a.icon, a.primary ? ThemeManager::current().onAccent : ThemeManager::current().text, 15)); b->setIconSize(QSize(15, 15)); }
        b->setStyleSheet(pillStyle());
        b->installEventFilter(this);
        connect(b, &QPushButton::clicked, this, [this, b]{
            // §10 anti-clickjacking: ignore clicks for 500 ms after appearing / moving / resizing.
            if (!m_armed.isValid() || m_armed.elapsed() < clickGuardMs()) {
                b->setProperty("guard", true); b->style()->unpolish(b); b->style()->polish(b);
                QPointer<QPushButton> g(b);
                QTimer::singleShot(clickGuardMs(), b, [g]{ if (g) { g->setProperty("guard", false); g->style()->unpolish(g); g->style()->polish(g); } });
                return;
            }
            emit action(b->property("actionId").toString());
        });
        if (row || (!a.primary && secondary >= 2)) {
            if (!pairRow) { pairRow = new QHBoxLayout; pairRow->setSpacing(6); l->addLayout(pairRow); }
            pairRow->addWidget(b, 1);
        } else l->addWidget(b);
        m_buttons << b;
        if (a.id == focusId) b->setFocus();
    }
    if (!focusId.isEmpty()) for (auto* b : m_buttons) if (b->property("actionId").toString() == focusId) setFocusProxy(b);
}

void Bubble::setActionEnabled(const QString& id, bool on) { for (auto* b : m_buttons) if (b->property("actionId").toString() == id) b->setEnabled(on); }
void Bubble::setActionText(const QString& id, const QString& text) { for (auto* b : m_buttons) if (b->property("actionId").toString() == id) b->setText(text); }

void Bubble::popup(const QPoint& anchor, bool takeFocus) {
    QWidget* win = parentWidget() ? parentWidget()->window() : nullptr;
    int w = m_width;
    QPoint at = anchor;
    if (win && win->width() < 400) { w = win->width() - 16; at.setX(win->mapToGlobal(QPoint(8, 0)).x()); }   // §1 narrow windows
    setFixedWidth(w + 2 * kShadow);
    adjustSize();
    m_target = at - QPoint(kShadow, kShadow);
    if (auto* scr = QGuiApplication::screenAt(anchor)) {   // keep on screen
        const QRect g = scr->availableGeometry();
        m_target.setX(qBound(g.left() - kShadow, m_target.x(), g.right() - width() + kShadow));
    }
    move(m_target);
    setWindowOpacity(0.0);
    if (takeFocus) setAttribute(Qt::WA_ShowWithoutActivating, false);
    show();
    if (takeFocus) { activateWindow(); if (focusProxy()) focusProxy()->setFocus(Qt::OtherFocusReason); }
    m_armed.start();
    const int d = Motion::enabled() ? Motion::fadeMs(Motion::Base) : 100;   // reduced: opacity only, 100 ms
    m_anim.stop(); m_anim.setDuration(qMax(1, d)); m_anim.setEasingCurve(QEasingCurve::OutCubic);
    m_anim.setStartValue(0.0); m_anim.setEndValue(1.0); m_anim.start();
    announce(this, m_a11y + QStringLiteral(". Prompt, %1 options").arg(m_buttons.size()));
}

void Bubble::dismiss() {
    if (m_closing) return;
    m_closing = true;
    qApp->removeEventFilter(this);
    m_target = pos();
    const int d = Motion::enabled() ? Motion::Fast : 100;
    m_anim.stop(); m_anim.setDuration(d); m_anim.setEasingCurve(QEasingCurve::InCubic);
    m_anim.setStartValue(windowOpacity()); m_anim.setEndValue(0.0);
    disconnect(&m_anim, &QVariantAnimation::valueChanged, this, nullptr);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        setWindowOpacity(v.toReal());
        if (Motion::enabled()) move(m_target + QPoint(0, int(qRound((1.0 - v.toReal()) * -4.0))));
    });
    m_anim.start();
}

void Bubble::paintEvent(QPaintEvent*) { QPainter p(this); paintCard(p, QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow), 14); }

void Bubble::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) { emit closed(true); dismiss(); return; }   // Esc = dismiss = ask again
    if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
        int i = -1; for (int k = 0; k < m_buttons.size(); ++k) if (m_buttons[k]->hasFocus()) i = k;
        int n = i < 0 ? 0 : (i + (e->key() == Qt::Key_Down ? 1 : -1) + m_buttons.size()) % m_buttons.size();
        if (!m_buttons.isEmpty()) m_buttons[n]->setFocus(Qt::TabFocusReason);
        return;
    }
    QWidget::keyPressEvent(e);
}
void Bubble::moveEvent(QMoveEvent* e) { QWidget::moveEvent(e); if (m_anim.state() != QAbstractAnimation::Running) m_armed.start(); }
void Bubble::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); m_armed.start(); }
bool Bubble::eventFilter(QObject* o, QEvent* e) {
    // Click on the page / elsewhere in the window: collapse to the chip (§1 auto-dismiss).
    if (e->type() == QEvent::MouseButtonPress && !m_closing && isVisible()) {
        auto* w = qobject_cast<QWidget*>(o);
        if (w && w->window() != this && parentWidget() && w->window() == parentWidget()->window()) {
            QTimer::singleShot(0, this, [this]{ if (!m_closing) { emit closed(false); dismiss(); } });
        }
    }
    return QWidget::eventFilter(o, e);
}
void Bubble::restyle() {}

// ------------------------------------------------------------------ DeviceList
DeviceList::DeviceList(QWidget* parent) : QWidget(parent) {
    const auto& pal = ThemeManager::current();
    auto* l = new QVBoxLayout(this); l->setContentsMargins(0, 0, 0, 0); l->setSpacing(4);
    m_list = new QListWidget(this);
    m_list->setAccessibleName(QStringLiteral("Devices"));   // listbox with options (§10)
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setMinimumHeight(44 * 3); m_list->setMaximumHeight(44 * 4 + 4);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;outline:0;} QListWidget::item{min-height:44px;border-radius:10px;padding:0 10px;color:%1;}"
        "QListWidget::item:hover{background:%2;} QListWidget::item:selected{background:%3;color:%1;}").arg(pal.text, pal.surfaceHover, pal.accentSoft));
    l->addWidget(m_list);
    m_status = new QLabel(QStringLiteral("Looking for devices…"), this);
    m_status->setStyleSheet(QStringLiteral("color:%1;font-size:12px;padding-left:10px;").arg(pal.textSecondary));
    l->addWidget(m_status);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]{ emit selectionChanged(!m_list->selectedItems().isEmpty()); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]{ emit activated(); });
}
void DeviceList::setDevices(const QList<QJsonObject>& devs) {
    const QString sel = selected().value(QStringLiteral("id")).toString();
    m_devs = devs; m_list->clear();
    for (const auto& d : devs) {
        const QString meta = d.value(QStringLiteral("meta")).toString();
        auto* it = new QListWidgetItem(d.value(QStringLiteral("name")).toString() + (meta.isEmpty() ? QString() : QStringLiteral("   ·   ") + meta), m_list);
        it->setData(Qt::UserRole, d.value(QStringLiteral("id")).toString());
        it->setSizeHint(QSize(0, 44));
        if (d.value(QStringLiteral("id")).toString() == sel) it->setSelected(true);
    }
}
void DeviceList::setSearching(bool on) {
    m_status->setText(on ? QStringLiteral("Looking for devices…") : m_devs.isEmpty() ? QStringLiteral("No compatible devices found") : QString());
    m_status->setVisible(!m_status->text().isEmpty());
}
QJsonObject DeviceList::selected() const {
    const auto s = m_list->selectedItems();
    if (s.isEmpty()) return {};
    const QString id = s.first()->data(Qt::UserRole).toString();
    for (const auto& d : m_devs) if (d.value(QStringLiteral("id")).toString() == id) return d;
    return {};
}

// ------------------------------------------------------------------ ScreenSharePicker
ScreenSharePicker::ScreenSharePicker(QWidget* host, const QString& hostName, const QIcon& fav,
                                     QAbstractListModel* screens, QAbstractListModel* windows, const QList<TabSource>& tabs)
    : QWidget(host), m_screens(screens), m_windows(windows), m_tabs(tabs) {
    const auto& pal = ThemeManager::current();
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    setGeometry(host->rect());
    host->installEventFilter(this);
    m_dialog = new QWidget(this);
    m_dialog->setObjectName(QStringLiteral("ShareDialog"));
    m_dialog->setAccessibleName(QStringLiteral("Choose what to share with %1").arg(hostName));
    auto* l = new QVBoxLayout(m_dialog); l->setContentsMargins(24 + kShadow, 22 + kShadow, 24 + kShadow, 22 + kShadow); l->setSpacing(14);
    auto* head = new QHBoxLayout(); head->setSpacing(12);
    auto* fi = new QLabel(m_dialog); fi->setPixmap(fav.pixmap(QSize(28, 28), devicePixelRatioF())); fi->setFixedSize(28, 28);
    auto* tl = new QVBoxLayout(); tl->setSpacing(2);
    auto* t = new QLabel(QStringLiteral("Choose what to share with <b>%1</b>").arg(hostName.toHtmlEscaped()), m_dialog);
    t->setStyleSheet(QStringLiteral("color:%1;font-size:17px;font-weight:600;").arg(pal.text));
    auto* sub = new QLabel(QStringLiteral("The site will be able to see the contents of your selection."), m_dialog);
    sub->setStyleSheet(QStringLiteral("color:%1;font-size:13px;").arg(pal.textSecondary));
    tl->addWidget(t); tl->addWidget(sub);
    head->addWidget(fi, 0, Qt::AlignTop); head->addLayout(tl, 1);
    l->addLayout(head);
    // Segmented control: Tabs / Windows / Entire screen
    auto* segBox = new QWidget(m_dialog); segBox->setObjectName(QStringLiteral("Seg"));
    segBox->setStyleSheet(QStringLiteral("#Seg{background:%1;border-radius:10px;} QPushButton{border:none;border-radius:8px;padding:6px 16px;color:%2;background:transparent;font-size:13px;}"
                                         "QPushButton:checked{background:%3;color:%4;font-weight:600;}").arg(pal.surface, pal.textSecondary, pal.card(), pal.text));
    auto* sl = new QHBoxLayout(segBox); sl->setContentsMargins(3, 3, 3, 3); sl->setSpacing(2);
    const QStringList names = {QStringLiteral("Tabs"), QStringLiteral("Windows"), QStringLiteral("Entire screen")};
    for (int i = 0; i < 3; ++i) {
        auto* b = new QPushButton(names[i], segBox); b->setCheckable(true); b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, i]{ showSegment(i); });
        sl->addWidget(b); m_seg << b;
    }
    auto* segRow = new QHBoxLayout(); segRow->addWidget(segBox); segRow->addStretch(1); l->addLayout(segRow);
    m_grid = new QListWidget(m_dialog);
    m_grid->setViewMode(QListView::IconMode); m_grid->setResizeMode(QListView::Adjust); m_grid->setMovement(QListView::Static);
    m_grid->setIconSize(QSize(210, 118)); m_grid->setGridSize(QSize(228, 160)); m_grid->setSpacing(14);
    m_grid->setUniformItemSizes(true); m_grid->setWordWrap(false); m_grid->setTextElideMode(Qt::ElideRight);
    m_grid->setFrameShape(QFrame::NoFrame); m_grid->setMinimumHeight(340);
    m_grid->setStyleSheet(QStringLiteral("QListWidget{background:transparent;outline:0;} QListWidget::item{border-radius:12px;padding:6px;color:%1;font-size:12.5px;border:2px solid transparent;}"
                                         "QListWidget::item:hover{background:%2;} QListWidget::item:selected{background:%3;border:2px solid %4;color:%1;}")
                          .arg(pal.text, pal.surfaceHover, pal.accentSoft, pal.accent));
    m_grid->setAccessibleName(QStringLiteral("Share sources"));
    connect(m_grid, &QListWidget::itemSelectionChanged, this, [this]{ m_share->setEnabled(!m_grid->selectedItems().isEmpty()); });
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this]{ finish(true); });
    l->addWidget(m_grid, 1);
    auto* foot = new QHBoxLayout(); foot->setSpacing(8);
    auto* note = new QLabel(m_dialog); note->setObjectName(QStringLiteral("ShareNote"));
    note->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(pal.textTertiary));
    foot->addWidget(note, 1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), m_dialog);
    m_share = new QPushButton(QStringLiteral("Share"), m_dialog); m_share->setProperty("primary", true); m_share->setEnabled(false);
    for (auto* b : {cancel, m_share}) { b->setStyleSheet(pillStyle()); b->setMinimumWidth(96); b->setCursor(Qt::PointingHandCursor); foot->addWidget(b); }
    connect(cancel, &QPushButton::clicked, this, [this]{ finish(false); });
    connect(m_share, &QPushButton::clicked, this, [this]{ finish(true); });
    l->addLayout(foot);
    m_refresh = new QTimer(this); m_refresh->setInterval(1000);   // thumbnails refresh every 1 s
    connect(m_refresh, &QTimer::timeout, this, &ScreenSharePicker::refresh);
    for (auto* m : {m_screens.data(), m_windows.data()}) if (m) {
        connect(m, &QAbstractItemModel::rowsInserted, this, &ScreenSharePicker::refresh);
        connect(m, &QAbstractItemModel::rowsRemoved, this, &ScreenSharePicker::refresh);
        connect(m, &QAbstractItemModel::modelReset, this, &ScreenSharePicker::refresh);
        connect(m, &QAbstractItemModel::dataChanged, this, &ScreenSharePicker::refresh);
    }
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_t = v.toReal(); resizeEvent(nullptr); update(); });
    showSegment(m_tabs.isEmpty() ? 2 : 0);
    if (m_tabs.isEmpty()) m_seg[0]->setEnabled(false);
}

void ScreenSharePicker::open() {
    show(); raise(); setFocus();
    m_refresh->start();
    const int d = Motion::enabled() ? Motion::Medium : 100;   // dialog 200 ms OutCubic; scrim 160
    m_anim.setDuration(d); m_anim.setEasingCurve(QEasingCurve::OutCubic); m_anim.setStartValue(0.0); m_anim.setEndValue(1.0); m_anim.start();
    announce(this, m_dialog->accessibleName());
}

static QPixmap tileWithIcon(const QString& title, const QIcon& icon) {
    const auto& pal = ThemeManager::current();
    QPixmap pm(420, 236); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(qc(pal.surface)); p.setPen(Qt::NoPen); p.drawRoundedRect(QRectF(0, 0, 210, 118), 8, 8);
    const QIcon ic = icon.isNull() ? ThemeManager::icon(QStringLiteral("window"), pal.textSecondary, 28) : icon;
    p.drawPixmap(QRect(91, 34, 28, 28), ic.pixmap(QSize(28, 28), 2));
    QFont f = qApp->font(); f.setPixelSize(11); p.setFont(f); p.setPen(qc(pal.textSecondary));
    p.drawText(QRectF(8, 70, 194, 40), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, QFontMetrics(f).elidedText(title, Qt::ElideRight, 380));
    return pm;
}
static QPixmap fitThumb(const QPixmap& src) {
    QPixmap pm(420, 236); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip; clip.addRoundedRect(QRectF(0, 0, 210, 118), 8, 8); p.setClipPath(clip);
    p.fillRect(QRectF(0, 0, 210, 118), qc(ThemeManager::current().surface));
    if (!src.isNull()) {
        const QSizeF s = QSizeF(src.size()).scaled(QSizeF(210, 118), Qt::KeepAspectRatio);
        p.drawPixmap(QRectF((210 - s.width()) / 2, (118 - s.height()) / 2, s.width(), s.height()), src, QRectF(src.rect()));
    }
    return pm;
}

void ScreenSharePicker::showSegment(int seg) {
    m_segment = seg;
    for (int i = 0; i < m_seg.size(); ++i) m_seg[i]->setChecked(i == seg);
    m_grid->clear();
    if (auto* n = m_dialog->findChild<QLabel*>(QStringLiteral("ShareNote")))
        n->setText(seg == 0 ? QStringLiteral("Tab sharing sends video only (no tab audio).")
                 : seg == 1 ? QStringLiteral("Window previews show the app icon.") : QString());
    refresh();
    m_share->setEnabled(false);
}

void ScreenSharePicker::refresh() {
    const int sel = m_grid->currentRow();
    const bool keepSel = !m_grid->selectedItems().isEmpty();
    QList<QPair<QString, QPixmap>> items;
    if (m_segment == 0) {
        for (const auto& t : m_tabs) items << qMakePair(t.title, fitThumb(t.grab ? t.grab() : QPixmap()));
    } else if (m_segment == 1 && m_windows) {
        for (int r = 0; r < m_windows->rowCount(); ++r) {
            const QString name = m_windows->data(m_windows->index(r), Qt::DisplayRole).toString();
            items << qMakePair(name, tileWithIcon(name, QIcon()));
        }
    } else if (m_segment == 2 && m_screens) {
        const auto screens = QGuiApplication::screens();
        for (int r = 0; r < m_screens->rowCount(); ++r) {
            const QString name = m_screens->data(m_screens->index(r), Qt::DisplayRole).toString();
            // Qt gives names only; previews come from QScreen::grabWindow(0) (1 fps). The model order
            // follows Chromium's screen enumeration, which matches QGuiApplication::screens() on Windows.
            QPixmap shot = r < screens.size() ? screens[r]->grabWindow(0) : QPixmap();
            items << qMakePair(name.isEmpty() ? QStringLiteral("Screen %1").arg(r + 1) : name, fitThumb(shot));
        }
    }
    if (m_grid->count() != items.size()) {
        m_grid->clear();
        for (const auto& it : items) { auto* li = new QListWidgetItem(QIcon(it.second), it.first, m_grid); li->setToolTip(it.first); }
    } else {
        for (int i = 0; i < items.size(); ++i) { m_grid->item(i)->setIcon(QIcon(items[i].second)); m_grid->item(i)->setText(items[i].first); }
    }
    if (keepSel && sel >= 0 && sel < m_grid->count()) m_grid->setCurrentRow(sel);
}

void ScreenSharePicker::finish(bool share) {
    if (m_done) return;
    const int row = m_grid->currentRow();
    if (share && (row < 0 || m_grid->selectedItems().isEmpty())) return;
    m_done = true;
    m_refresh->stop();
    if (!share) emit chosen(int(Choice::None), -1, QString());
    else if (m_segment == 0) emit chosen(int(Choice::Tab), row, row < m_tabs.size() ? m_tabs[row].token : QString());
    else emit chosen(int(m_segment == 1 ? Choice::Window : Choice::Screen), row, QString());
    close();
}

void ScreenSharePicker::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, int(0.45 * 255 * m_t)));   // scrim over web content only
    const QRect dr = m_dialog->geometry();
    p.save();
    p.setOpacity(m_t);
    const qreal s = Motion::enabled() ? 0.96 + 0.04 * m_t : 1.0;
    const QPointF c = QRectF(dr).center(); p.translate(c); p.scale(s, s); p.translate(-c);
    paintCard(p, QRectF(dr).adjusted(kShadow, kShadow, -kShadow, -kShadow), 16);
    p.restore();
}
void ScreenSharePicker::resizeEvent(QResizeEvent*) {
    const int w = qMin(760, width() - 32) + 2 * kShadow;
    const int h = qMin(height() - 32 + 2 * kShadow, 620);
    m_dialog->setGeometry((width() - w) / 2, qMax(0, (height() - h) / 2), w, h);
}
void ScreenSharePicker::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) { finish(false); return; }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && m_share->isEnabled()) { finish(true); return; }
    QWidget::keyPressEvent(e);
}
bool ScreenSharePicker::eventFilter(QObject* o, QEvent* e) {
    if (o == parentWidget() && e->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(o, e);
}

// ------------------------------------------------------------------ SiteControls
SiteControls::SiteControls(QWidget* window, const QString& origin, const QIcon& fav, bool secure, const QList<SiteRow>& rows, int cookieCount)
    : QWidget(window, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    const auto& pal = ThemeManager::current();
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_DeleteOnClose);
    setAccessibleName(QStringLiteral("Site controls"));
    setFixedWidth(380 + 2 * kShadow);
    auto* l = new QVBoxLayout(this); l->setContentsMargins(kShadow + 6, kShadow + 6, kShadow + 6, kShadow + 10); l->setSpacing(2);
    const QString host = Perm::hostOf(origin);
    auto* head = new QHBoxLayout(); head->setContentsMargins(10, 8, 0, 8); head->setSpacing(10);
    auto* fi = new QLabel(this); fi->setPixmap(fav.pixmap(QSize(28, 28), devicePixelRatioF())); fi->setFixedSize(28, 28);
    auto* tl = new QVBoxLayout(); tl->setSpacing(2);
    auto* h = new QLabel(host.toHtmlEscaped(), this); h->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:600;").arg(pal.text));
    auto* conn = new QWidget(this); auto* cl = new QHBoxLayout(conn); cl->setContentsMargins(0, 0, 0, 0); cl->setSpacing(4);
    auto* ci = new QLabel(conn); ci->setPixmap(glyphPixmap(secure ? QStringLiteral("lock") : QStringLiteral("not-secure"), secure ? pal.secure : pal.danger, 12, devicePixelRatioF()));
    auto* ct = new QLabel(secure ? QStringLiteral("Connection is secure") : QStringLiteral("Connection is not secure"), conn);
    ct->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(secure ? pal.secure : pal.danger));
    cl->addWidget(ci); cl->addWidget(ct); cl->addStretch(1);
    tl->addWidget(h); tl->addWidget(conn);
    auto* closeBtn = new QToolButton(this); closeBtn->setFixedSize(34, 34); closeBtn->setAutoRaise(true); closeBtn->setAccessibleName(QStringLiteral("Close"));
    closeBtn->setIcon(ThemeManager::icon(QStringLiteral("close"), pal.textSecondary, 16));
    closeBtn->setStyleSheet(QStringLiteral("QToolButton{border:none;border-radius:17px;} QToolButton:hover{background:%1;}").arg(pal.surfaceHover));
    connect(closeBtn, &QToolButton::clicked, this, &QWidget::close);
    head->addWidget(fi, 0, Qt::AlignTop); head->addLayout(tl, 1); head->addWidget(closeBtn, 0, Qt::AlignTop);
    l->addLayout(head);
    auto* lab = new QLabel(QStringLiteral("PERMISSIONS"), this);
    lab->setStyleSheet(QStringLiteral("color:%1;font-size:11px;font-weight:600;letter-spacing:0.5px;padding:6px 12px 2px 12px;").arg(pal.textTertiary));
    l->addWidget(lab);
    auto addRow = [&](const QString& glyph, const QString& name, const QString& state, QWidget* control, bool warn) {
        auto* row = new QWidget(this); row->setMinimumHeight(48); row->setObjectName(QStringLiteral("SCRow"));
        row->setStyleSheet(QStringLiteral("#SCRow{border-radius:10px;} #SCRow:hover{background:%1;}").arg(pal.surfaceHover));
        auto* rl = new QHBoxLayout(row); rl->setContentsMargins(12, 6, 12, 6); rl->setSpacing(12);
        auto* gi = new QLabel(row); gi->setPixmap(glyphPixmap(glyph, pal.textSecondary, 18, devicePixelRatioF()));
        auto* tx = new QVBoxLayout(); tx->setSpacing(1);
        auto* n = new QLabel(name, row); n->setStyleSheet(QStringLiteral("color:%1;font-size:13px;").arg(pal.text));
        auto* s = new QLabel(state, row); s->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(warn ? pal.warning : pal.textSecondary));
        tx->addWidget(n); tx->addWidget(s);
        rl->addWidget(gi); rl->addLayout(tx, 1);
        if (control) { control->setParent(row); rl->addWidget(control, 0, Qt::AlignVCenter); }
        row->setAccessibleName(name + QStringLiteral(": ") + state);
        l->addWidget(row);
    };
    int nPerm = 0;
    for (const auto& r : rows) {
        QWidget* ctl = nullptr;
        if (r.control == 0) {
            auto* sw = new ToggleSwitch(); sw->setFixedSize(38, 22); sw->setChecked(r.on);
            sw->setAccessibleName(Perm::title(r.kind));
            const Perm::Kind k = r.kind;
            connect(sw, &ToggleSwitch::toggled, this, [this, k](bool on){ emit toggled(k, on); });
            ctl = sw;
        } else if (r.control == 1) {
            auto* tag = new QLabel(QStringLiteral("Ask"));
            tag->setStyleSheet(QStringLiteral("color:%1;background:%2;border-radius:12px;padding:3px 10px;font-size:12px;").arg(pal.textSecondary, pal.surface));
            ctl = tag;
        }
        addRow(Perm::glyph(r.kind), Perm::title(r.kind), r.state, ctl, r.warning);
        ++nPerm;
    }
    addRow(QStringLiteral("cookie"), QStringLiteral("Cookies and site data"),
           cookieCount >= 0 ? QStringLiteral("%1 cookies in use").arg(cookieCount) : QStringLiteral("Allowed"), nullptr, false);
    // Footer: Reset permissions (inline confirm) + Site settings
    auto* foot = new QWidget(this); auto* fl = new QHBoxLayout(foot); fl->setContentsMargins(6, 8, 6, 0); fl->setSpacing(6);
    auto* reset = new QPushButton(QStringLiteral("Reset permissions"), foot);
    reset->setIcon(ThemeManager::icon(QStringLiteral("settings-reset"), pal.text, 15));
    auto* site = new QPushButton(QStringLiteral("Site settings"), foot);
    for (auto* b : {reset, site}) { b->setStyleSheet(pillStyle()); b->setCursor(Qt::PointingHandCursor); fl->addWidget(b, 1); }
    reset->setEnabled(nPerm > 0);
    l->addWidget(foot);
    auto* confirm = new QWidget(this); auto* cfl = new QHBoxLayout(confirm); cfl->setContentsMargins(12, 8, 6, 0); cfl->setSpacing(6);
    auto* ctext = new QLabel(QStringLiteral("Reset %1 permission%2 for %3?").arg(nPerm).arg(nPerm == 1 ? "" : "s").arg(host.toHtmlEscaped()), confirm);
    ctext->setWordWrap(true); ctext->setStyleSheet(QStringLiteral("color:%1;font-size:12.5px;").arg(pal.text));
    auto* yes = new QPushButton(QStringLiteral("Reset"), confirm); yes->setProperty("primary", true);
    auto* no = new QPushButton(QStringLiteral("Cancel"), confirm);
    for (auto* b : {no, yes}) { b->setStyleSheet(pillStyle()); b->setCursor(Qt::PointingHandCursor); }
    cfl->addWidget(ctext, 1); cfl->addWidget(no); cfl->addWidget(yes);
    confirm->hide(); l->addWidget(confirm);
    connect(reset, &QPushButton::clicked, this, [foot, confirm, this]{ foot->hide(); confirm->show(); adjustSize(); });
    connect(no, &QPushButton::clicked, this, [foot, confirm, this]{ confirm->hide(); foot->show(); adjustSize(); });
    connect(yes, &QPushButton::clicked, this, [this]{ emit resetRequested(); close(); });
    connect(site, &QPushButton::clicked, this, [this]{ emit siteSettingsRequested(); close(); });
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        setWindowOpacity(v.toReal());
        if (Motion::enabled()) move(m_target + QPoint(0, int(qRound((1.0 - v.toReal()) * -4.0))));
    });
}
void SiteControls::popup(const QPoint& anchor) {
    adjustSize();
    m_target = anchor - QPoint(kShadow, kShadow);
    move(m_target); setWindowOpacity(0.0); show();
    m_anim.setDuration(Motion::enabled() ? Motion::Quick : 100);   // like menus: 140 ms OutCubic
    m_anim.setEasingCurve(QEasingCurve::OutCubic); m_anim.setStartValue(0.0); m_anim.setEndValue(1.0); m_anim.start();
}
void SiteControls::paintEvent(QPaintEvent*) { QPainter p(this); paintCard(p, QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow), 14); }
void SiteControls::keyPressEvent(QKeyEvent* e) { if (e->key() == Qt::Key_Escape) { close(); return; } QWidget::keyPressEvent(e); }

}
