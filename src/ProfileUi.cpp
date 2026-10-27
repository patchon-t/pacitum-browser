#include "ProfileUi.h"
#include "Motion.h"
#include "ThemeManager.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QEventLoop>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QPushButton>
#include <QShortcut>
#include <QStyleHints>
#include <QToolButton>
#include <QVBoxLayout>
#include <QTimer>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSvgRenderer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QLocale>
#include <QKeyEvent>
#include <QWindow>
#include <algorithm>
#include "Widgets.h"

static const PacitumPalette& pal() { return ThemeManager::current(); }
static bool dark() { return QColor(pal().page).lightness() < 128; }
static QString rgba(const QString& hex, double a) { QColor c(hex); return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a); }
static void ensurePalette() {
    // Before any BrowserWindow exists (startup picker) the palette follows the OS colour scheme.
    if (QApplication::topLevelWidgets().isEmpty() || qApp->styleSheet().isEmpty()) {
        auto* sh = QGuiApplication::styleHints();
        static ThemeManager* tm = nullptr;
        if (!tm) tm = new ThemeManager(sh ? sh->colorScheme() != Qt::ColorScheme::Light : true);
    }
}
static QString baseStyle() {
    const auto& p = pal();
    QString s = QStringLiteral(R"(
QWidget#PfRoot, QDialog#PfRoot { background: @page; color: @text; font-size: 13px; }
QLabel { color: @text; background: transparent; }
#PfTitle { font-size: 26px; font-weight: 600; }
#PfH1 { font-size: 20px; font-weight: 600; }
#PfSub, #PfCardSub, #PfHint { color: @sec; font-size: 12px; }
#PfSub { font-size: 13px; }
#PfLabel { color: @sec; font-size: 11px; font-weight: 600; letter-spacing: 0.5px; }
#PfCard { background: @toolbar; border: 1px solid @border; border-radius: 18px; }
#PfCard:hover { border: 2px solid @ring; }
#PfCard[dashed="true"] { background: transparent; border: 1.5px dashed @border2; }
#PfCardName { font-size: 14px; font-weight: 600; }
#PfBadge { background: @surface; color: @sec; border-radius: 10px; padding: 0 8px; font-size: 11px; font-weight: 600; min-height: 20px; max-height: 20px; }
#PfEditBtn { background: @surface; border: none; border-radius: 14px; }
#PfFooter { background: @toolbar; border-top: 1px solid @border; }
#PfPill { background: transparent; color: @text; border: 1px solid @border; border-radius: 15px; min-height: 30px; padding: 0 14px; font-weight: 500; }
#PfPill:hover { background: @surface; }
#PfPrimary { background: @accent; color: @onAccent; border: none; border-radius: 16px; min-height: 32px; padding: 0 18px; font-weight: 600; }
#PfDanger { background: transparent; color: @danger; border: none; font-weight: 600; padding: 0 8px; min-height: 30px; }
#PfDanger:hover { background: @dangerSoft; border-radius: 8px; }
QLineEdit#PfName { background: @toolbar; color: @text; border: 1px solid @border; border-radius: 10px; min-height: 38px; padding: 0 12px; font-size: 14px; }
QLineEdit#PfName:focus { border: 2px solid @accent; }
QLineEdit#PfName[error="true"] { border: 2px solid @danger; }
#PfError { color: @danger; font-size: 12px; }
#PfPreview { background: @chrome; border-right: 1px solid @border; }
#PfPrevName { font-size: 18px; font-weight: 600; }
QCheckBox { color: @text; spacing: 8px; }
QCheckBox::indicator { width: 18px; height: 18px; border-radius: 5px; border: 1px solid @border2; background: @toolbar; }
QCheckBox::indicator:checked { background: @accent; border: 1px solid @accent; image: url(none); }
#PsCard { background: @toolbar; border: 1px solid @border; border-radius: 14px; }
#PsName { font-size: 15px; font-weight: 600; }
#PsRow { background: transparent; border: none; border-radius: 8px; text-align: left; padding: 0 8px; min-height: 46px; color: @text; }
#PsRow:hover { background: @surface; }
#PsAction { background: transparent; border: none; border-radius: 8px; text-align: left; padding: 0 10px; min-height: 36px; color: @text; }
#PsAction:hover { background: @surface; }
#PsDivider { background: @border; min-height: 1px; max-height: 1px; }
#PsShortcut { color: @sec; font-size: 12px; }
)");
    const QList<QPair<QString, QString>> v{
        {"@ring", p.accent}, {"@border2", dark() ? "#3A404C" : "#C9CED8"}, {"@dangerSoft", rgba(p.danger, 0.1)},
        {"@chrome", p.frame}, {"@page", p.page}, {"@toolbar", p.toolbar}, {"@surface", p.surface}, {"@border", p.border},
        {"@text", p.text}, {"@sec", p.textSecondary}, {"@accent", p.accent}, {"@onAccent", p.onAccent}, {"@danger", p.danger}};
    for (const auto& x : v) s.replace(x.first, x.second);
    return s;
}

namespace {
struct ClickFrame : QFrame {
    std::function<void()> onClick;
    using QFrame::QFrame;
    void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && onClick) onClick(); QFrame::mouseReleaseEvent(e); }
    void enterEvent(QEnterEvent* e) override { if (auto* b = findChild<QWidget*>(QStringLiteral("PfEditBtn"))) if (!property("editAll").toBool()) b->show(); QFrame::enterEvent(e); }
    void leaveEvent(QEvent* e) override { if (auto* b = findChild<QWidget*>(QStringLiteral("PfEditBtn"))) if (!property("editAll").toBool()) b->hide(); QFrame::leaveEvent(e); }
};
QLabel* avatarLabel(QWidget* parent, const ProfileInfo& p, int size, int ring = 0, const QString& gap = QString()) {
    auto* l = new QLabel(parent);
    const QPixmap pm = ProfileManager::avatarPixmap(p, size, parent->devicePixelRatioF(), ring, gap);
    l->setPixmap(pm); l->setFixedSize(pm.size() / pm.devicePixelRatio()); l->setAlignment(Qt::AlignCenter);
    return l;
}
}

// ======================= picker =======================
// 2.0.1: rebuilt from the design's "Profile picker" artboard (1040 x 680).
namespace {
QString relativeDay(qint64 ms) {
    if (ms <= 0) return QStringLiteral("Not used yet");
    const QDate d = QDateTime::fromMSecsSinceEpoch(ms).date(), today = QDate::currentDate();
    const qint64 days = d.daysTo(today);
    if (days <= 0) return QStringLiteral("Used today");
    if (days == 1) return QStringLiteral("Yesterday");
    if (days < 7) return QStringLiteral("%1 days ago").arg(days);
    if (days < 14) return QStringLiteral("Last week");
    if (days < 60) return QStringLiteral("%1 weeks ago").arg(days / 7);
    return QLocale().toString(d, QLocale::ShortFormat);
}
int sessionTabs(const QString& id) {
    QFile f(ProfileManager::instance()->profileDir(id) + QStringLiteral("/browser_session.json"));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    return int(QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("tabs")).toArray().size());
}
QPixmap svgPixmap(const QString& file, int h, qreal dpr) {
    QSvgRenderer r(file);
    if (!r.isValid()) return {};
    const QSize ds = r.defaultSize();
    const int w = qMax(1, ds.width() * h / qMax(1, ds.height()));
    QPixmap pm(QSize(w, h) * dpr); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); r.render(&p, QRectF(0, 0, w, h));
    return pm;
}

// One profile row: 68 px, radius 16. Selected = card fill + border + a 4 x 24 accent bar on the left.
class ProfileRow : public QAbstractButton {
public:
    ProfileRow(const ProfileInfo& p, QWidget* parent) : QAbstractButton(parent), m_p(p) {
        setCheckable(true); setAutoExclusive(false); setCursor(Qt::PointingHandCursor);
        setFixedHeight(68); setFocusPolicy(Qt::StrongFocus); setAttribute(Qt::WA_Hover, true);
        const int tabs = p.isGuest ? 0 : sessionTabs(p.id);
        const QString what = p.isGuest ? QStringLiteral("Nothing is saved") : p.email.isEmpty() ? QStringLiteral("Local profile") : p.email;
        m_detail = p.isGuest ? what : QStringLiteral("%1 · %2").arg(what, tabs == 0 ? QStringLiteral("No tabs") : tabs == 1 ? QStringLiteral("1 tab") : QStringLiteral("%1 tabs").arg(tabs));
        m_last = p.isGuest ? QString() : relativeDay(p.lastUsed);
        setAccessibleName(p.name);
    }
    QString id() const { return m_p.isGuest ? QStringLiteral("guest") : m_p.id; }
    QString name() const { return m_p.name; }
    bool editMode = false;
    std::function<void()> onEdit, onActivate;
protected:
    void paintEvent(QPaintEvent*) override {
        const auto& c = pal();
        QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (isChecked()) {
            g.setPen(QPen(QColor(c.border), 1)); g.setBrush(QColor(c.toolbar)); g.drawRoundedRect(r, 16, 16);
            g.setPen(Qt::NoPen); g.setBrush(QColor(c.accent));
            QPainterPath bar; bar.addRoundedRect(QRectF(-4, height() / 2.0 - 12, 8, 24), 4, 4);
            g.save(); g.setClipRect(QRectF(0, 0, 4, height())); g.drawPath(bar); g.restore();
        } else if (underMouse()) {
            g.setPen(Qt::NoPen); g.setBrush(QColor(c.surface)); g.drawRoundedRect(r, 16, 16);
        }
        if (hasFocus() && !isChecked()) { g.setPen(QPen(QColor(c.focusRing), 2)); g.setBrush(Qt::NoBrush); g.drawRoundedRect(r.adjusted(1, 1, -1, -1), 15, 15); }
        const qreal dpr = devicePixelRatioF();
        g.drawPixmap(QPointF(18, 14), ProfileManager::avatarPixmap(m_p, 40, dpr));
        QFont f = font(); f.setPixelSize(15); f.setWeight(QFont::Medium);
        g.setFont(f); g.setPen(QColor(c.text));
        const int right = editMode ? 52 : 16;
        QFontMetrics fm(f);
        g.drawText(QRectF(72, 13, width() - 72 - 130, 22), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(m_p.name, Qt::ElideRight, width() - 200));
        QFont s = font(); s.setPixelSize(13); g.setFont(s); g.setPen(QColor(c.textSecondary));
        g.drawText(QRectF(72, 35, width() - 72 - 130, 20), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(s).elidedText(m_detail, Qt::ElideRight, width() - 200));
        QFont l = font(); l.setPixelSize(12); g.setFont(l);
        if (!m_last.isEmpty()) g.drawText(QRectF(width() - right - 120, 0, 120, height()), Qt::AlignRight | Qt::AlignVCenter, m_last);
        if (editMode && !m_p.isGuest)
            g.drawPixmap(QPointF(width() - 38, height() / 2.0 - 9), ThemeManager::icon(QStringLiteral("edit"), c.textSecondary, 18).pixmap(QSize(18, 18), dpr));
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (editMode && !m_p.isGuest && e->position().x() > width() - 52 && onEdit) { onEdit(); return; }
        QAbstractButton::mouseReleaseEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { if (onActivate) onActivate(); }
    void nextCheckState() override { setChecked(true); }
private:
    ProfileInfo m_p; QString m_detail, m_last;
};

// Text button with an icon (Add profile, Browse as guest, Manage).
QPushButton* flatButton(QWidget* parent, const QString& icon, const QString& text, int px = 14) {
    auto* b = new QPushButton(text, parent); b->setObjectName(QStringLiteral("PfFlat"));
    if (!icon.isEmpty()) { b->setIcon(ThemeManager::icon(icon, pal().text, 16)); b->setIconSize(QSize(16, 16)); }
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("QPushButton#PfFlat{background:transparent;border:0;border-radius:12px;padding:0 14px;min-height:40px;font-size:%1px;font-weight:500;color:%2;}"
                                    "QPushButton#PfFlat:hover{background:%3;}QPushButton#PfFlat:pressed{background:%4;}")
                         .arg(px).arg(pal().text, pal().surface, pal().surfaceHover));
    return b;
}
}

ProfilePicker::ProfilePicker(Mode mode, QWidget* parent) : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), m_mode(mode) {
    ensurePalette();
    const auto& c = pal();
    setObjectName(QStringLiteral("PfRoot")); setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(QStringLiteral("Pacitum — Choose a profile"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/pacitum-icon-256.png")));
    setStyleSheet(baseStyle());
    setFixedSize(1040, 680);
    auto* h = new QHBoxLayout(this); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(0);

    // Left: ink panel, stacked wordmark, one line of help at the bottom.
    auto* side = new QWidget(this); side->setFixedWidth(290); side->setObjectName(QStringLiteral("PfSide"));
    side->setAttribute(Qt::WA_StyledBackground, true);
    side->setStyleSheet(QStringLiteral("#PfSide{background:%1;}").arg(dark() ? c.frame : QStringLiteral("#151112")));
    auto* sl = new QVBoxLayout(side); sl->setContentsMargins(36, 48, 36, 40); sl->setSpacing(0);
    auto* wm = new QLabel(side); wm->setPixmap(svgPixmap(QStringLiteral(":/brand/wordmark_stacked.svg"), 109, devicePixelRatioF()));
    wm->setAccessibleName(QStringLiteral("Pacitum"));
    sl->addWidget(wm, 0, Qt::AlignLeft | Qt::AlignTop); sl->addStretch(1);
    auto* help = new QLabel(QStringLiteral("Each profile keeps its own tabs, history, bookmarks and sign-ins. Switch any time from the menu."), side);
    help->setWordWrap(true); help->setStyleSheet(QStringLiteral("color:#B8AFAA;font-size:14px;"));
    sl->addWidget(help);
    h->addWidget(side);

    // Right: caption buttons, title, list, footer.
    auto* main = new QWidget(this); auto* ml = new QVBoxLayout(main); ml->setContentsMargins(0, 0, 0, 0); ml->setSpacing(0);
    auto* caps = new QHBoxLayout; caps->setContentsMargins(0, 0, 0, 0); caps->setSpacing(0); caps->addStretch(1);
    auto capBtn = [&](const QString& icon, const QString& tip, auto fn) {
        auto* b = new QToolButton(main); b->setFixedSize(46, 40); b->setToolTip(tip); b->setAccessibleName(tip);
        b->setIcon(ThemeManager::icon(icon, c.text, 12)); b->setIconSize(QSize(12, 12)); b->setCursor(Qt::ArrowCursor);
        b->setStyleSheet(QStringLiteral("QToolButton{border:0;background:transparent;}QToolButton:hover{background:%1;}").arg(icon == QLatin1String("close") ? QStringLiteral("#C42B1C") : c.surface));
        connect(b, &QToolButton::clicked, this, fn); caps->addWidget(b);
    };
    capBtn(QStringLiteral("window-minimize"), QStringLiteral("Minimize"), [this]{ showMinimized(); });
    capBtn(QStringLiteral("close"), QStringLiteral("Close window"), [this]{ close(); });
    ml->addLayout(caps);

    auto* body = new QWidget(main); auto* bl = new QVBoxLayout(body); bl->setContentsMargins(44, 8, 44, 0); bl->setSpacing(0);
    auto* head = new QHBoxLayout; head->setContentsMargins(0, 0, 0, 14);
    auto* title = new QLabel(m_mode == Manage ? QStringLiteral("Profiles") : QStringLiteral("Choose a profile"), body);
    title->setStyleSheet(QStringLiteral("font-size:22px;font-weight:600;"));
    m_manage = flatButton(body, QString(), QStringLiteral("Manage"), 13);
    m_manage->setStyleSheet(m_manage->styleSheet() + QStringLiteral("QPushButton#PfFlat{color:%1;min-height:28px;padding:0 8px;border-radius:8px;}").arg(c.textSecondary));
    connect(m_manage, &QAbstractButton::clicked, this, [this]{ m_editAll = !m_editAll; m_manage->setText(m_editAll ? QStringLiteral("Done") : QStringLiteral("Manage")); rebuild(); });
    head->addWidget(title); head->addStretch(1); head->addWidget(m_manage);
    bl->addLayout(head);
    m_list = new QVBoxLayout; m_list->setSpacing(4); m_list->setContentsMargins(0, 0, 0, 0);
    bl->addLayout(m_list);
    auto* actions = new QHBoxLayout; actions->setContentsMargins(0, 10, 0, 0); actions->setSpacing(4);
    auto* add = flatButton(body, QStringLiteral("add"), QStringLiteral("Add profile"));
    auto* guest = flatButton(body, QStringLiteral("profile"), QStringLiteral("Browse as guest"));
    connect(add, &QAbstractButton::clicked, this, [this]{ const QString id = ProfileEditDialog::addProfile(this); if (!id.isEmpty()) { m_selected = id; rebuild(); } });
    connect(guest, &QAbstractButton::clicked, this, [this]{ emit chosen(QStringLiteral("guest")); });
    actions->addWidget(add); actions->addWidget(guest); actions->addStretch(1);
    bl->addLayout(actions); bl->addStretch(1);
    ml->addWidget(body, 1);

    auto* foot = new QWidget(main); foot->setObjectName(QStringLiteral("PfFoot")); foot->setAttribute(Qt::WA_StyledBackground, true);
    foot->setStyleSheet(QStringLiteral("#PfFoot{border-top:1px solid %1;}").arg(c.border));
    auto* fl = new QHBoxLayout(foot); fl->setContentsMargins(44, 16, 44, 26); fl->setSpacing(10);
    auto* sw = new ToggleSwitch(foot); sw->setChecked(ProfileManager::instance()->showPickerOnStartup());
    connect(sw, &ToggleSwitch::toggled, this, [](bool on){ ProfileManager::instance()->setShowPickerOnStartup(on); });
    auto* swl = new QLabel(QStringLiteral("Show at startup"), foot); swl->setStyleSheet(QStringLiteral("font-size:14px;"));
    swl->setBuddy(sw);
    fl->addWidget(sw); fl->addWidget(swl); fl->addStretch(1);
    auto* open = new QPushButton(foot); m_open = open; open->setObjectName(QStringLiteral("PfOpen")); open->setCursor(Qt::PointingHandCursor);
    open->setLayoutDirection(Qt::RightToLeft);
    open->setIcon(ThemeManager::icon(QStringLiteral("arrow-right"), c.onPrimary, 16)); open->setIconSize(QSize(16, 16));
    open->setStyleSheet(QStringLiteral("QPushButton#PfOpen{background:%1;color:%2;border:0;border-radius:14px;min-height:46px;padding:0 22px;font-size:15px;font-weight:500;}"
                                       "QPushButton#PfOpen:hover{background:%3;}").arg(c.primary, c.onPrimary, QColor(c.primary).lighter(dark() ? 92 : 135).name()));
    open->setDefault(true);
    connect(open, &QAbstractButton::clicked, this, &ProfilePicker::openSelected);
    fl->addWidget(open);
    ml->addWidget(foot);
    h->addWidget(main, 1);

    connect(ProfileManager::instance(), &ProfileManager::profilesChanged, this, &ProfilePicker::rebuild);
    m_selected = ProfileManager::instance()->lastUsed();
    rebuild();
}

void ProfilePicker::paintEvent(QPaintEvent* e) {
    QPainter p(this); p.fillRect(rect(), QColor(pal().page));
    QWidget::paintEvent(e);
}

void ProfilePicker::rebuild() {
    while (auto* it = m_list->takeAt(0)) { if (auto* w = it->widget()) w->deleteLater(); delete it; }
    auto list = ProfileManager::instance()->profiles();
    std::sort(list.begin(), list.end(), [](const ProfileInfo& a, const ProfileInfo& b){ return a.lastUsed > b.lastUsed; });
    bool found = false;
    for (const auto& p : list) found |= p.id == m_selected;
    if (!found && !list.isEmpty()) m_selected = list.first().id;
    for (const auto& p : list) {
        auto* row = new ProfileRow(p, this);
        row->editMode = m_editAll;
        row->setChecked(p.id == m_selected);
        const QString id = p.id;
        connect(row, &QAbstractButton::clicked, this, [this, id]{ select(id); });
        row->onActivate = [this, id]{ select(id); openSelected(); };
        row->onEdit = [this, id]{ ProfileEditDialog::editProfile(id, this); rebuild(); };
        m_list->addWidget(row);
    }
    select(m_selected);
}

void ProfilePicker::select(const QString& id) {
    m_selected = id;
    QString name;
    for (int i = 0; i < m_list->count(); ++i)
        if (auto* r = dynamic_cast<ProfileRow*>(m_list->itemAt(i)->widget())) {
            r->setChecked(r->id() == id); r->update();
            if (r->id() == id) name = r->name();
        }
    if (m_open) {
        m_open->setText(QStringLiteral("Open as %1").arg(name.isEmpty() ? QStringLiteral("profile") : name));
        m_open->setEnabled(!name.isEmpty());
    }
}

void ProfilePicker::openSelected() { if (!m_selected.isEmpty()) emit chosen(m_selected); }

void ProfilePicker::keyPressEvent(QKeyEvent* e) {
    QList<ProfileRow*> rows;
    for (int i = 0; i < m_list->count(); ++i) if (auto* r = dynamic_cast<ProfileRow*>(m_list->itemAt(i)->widget())) rows << r;
    int cur = -1;
    for (int i = 0; i < rows.size(); ++i) if (rows[i]->id() == m_selected) cur = i;
    if ((e->key() == Qt::Key_Down || e->key() == Qt::Key_Up) && !rows.isEmpty()) {
        const int n = qBound(0, cur + (e->key() == Qt::Key_Down ? 1 : -1), int(rows.size()) - 1);
        select(rows[n]->id()); rows[n]->setFocus(); return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { openSelected(); return; }
    if (e->key() == Qt::Key_Escape) { close(); return; }
    QWidget::keyPressEvent(e);
}

// Frameless: drag the window from any empty spot.
void ProfilePicker::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        if (auto* wh = windowHandle(); wh && wh->startSystemMove()) return;
        m_dragFrom = e->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    QWidget::mousePressEvent(e);
}
void ProfilePicker::mouseMoveEvent(QMouseEvent* e) {
    if ((e->buttons() & Qt::LeftButton) && !m_dragFrom.isNull()) move(e->globalPosition().toPoint() - m_dragFrom);
    QWidget::mouseMoveEvent(e);
}

void ProfilePicker::closeEvent(QCloseEvent* e) { QWidget::closeEvent(e); }

QString ProfilePicker::pickAtStartup() {
    ProfilePicker picker(Startup);
    QString result; QEventLoop loop;
    connect(&picker, &ProfilePicker::chosen, &loop, [&](const QString& id){ result = id; loop.quit(); });
    struct CloseWatch : QObject { QEventLoop* l; bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::Close) l->quit(); return false; } } cw;
    cw.l = &loop; picker.installEventFilter(&cw);
    picker.show();
    loop.exec();
    picker.removeEventFilter(&cw);
    return result;
}

// ======================= edit =======================
ProfileEditDialog::ProfileEditDialog(const ProfileInfo& p, bool create, QWidget* parent) : QDialog(parent), m_p(p), m_create(create) {
    ensurePalette();
    setObjectName(QStringLiteral("PfRoot")); setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(create ? QStringLiteral("Pacitum — Add profile") : QStringLiteral("Pacitum — Edit profile"));
    setStyleSheet(baseStyle()); setFixedSize(760, 470);
    auto* v = new QVBoxLayout(this); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
    auto* top = new QHBoxLayout(); top->setSpacing(0);
    // live preview
    auto* prev = new QWidget(this); prev->setObjectName(QStringLiteral("PfPreview")); prev->setAttribute(Qt::WA_StyledBackground, true); prev->setFixedWidth(330);
    auto* pl = new QVBoxLayout(prev); pl->setContentsMargins(24, 24, 24, 24); pl->setSpacing(0);
    auto* pt = new QLabel(QStringLiteral("PREVIEW"), prev); pt->setObjectName(QStringLiteral("PfLabel"));
    m_avatar = new QLabel(prev); m_avatar->setAlignment(Qt::AlignCenter);
    m_previewName = new QLabel(prev); m_previewName->setObjectName(QStringLiteral("PfPrevName")); m_previewName->setAlignment(Qt::AlignCenter);
    auto* pe = new QLabel(QStringLiteral("Local profile"), prev); pe->setObjectName(QStringLiteral("PfHint")); pe->setAlignment(Qt::AlignCenter);
    m_mini = new QLabel(prev); m_mini->setFixedHeight(104);
    pl->addWidget(pt); pl->addSpacing(14); pl->addWidget(m_avatar, 0, Qt::AlignHCenter); pl->addSpacing(12); pl->addWidget(m_previewName);
    pl->addWidget(pe); pl->addSpacing(22); pl->addWidget(m_mini); pl->addStretch();
    top->addWidget(prev);
    // form
    auto* form = new QWidget(this); auto* fl = new QVBoxLayout(form); fl->setContentsMargins(24, 24, 24, 16); fl->setSpacing(0);
    auto* h = new QLabel(create ? QStringLiteral("Add profile") : QStringLiteral("Edit profile"), form); h->setObjectName(QStringLiteral("PfH1"));
    fl->addWidget(h); fl->addSpacing(20);
    auto* nl = new QLabel(QStringLiteral("NAME"), form); nl->setObjectName(QStringLiteral("PfLabel")); fl->addWidget(nl); fl->addSpacing(8);
    m_name = new QLineEdit(p.name, form); m_name->setObjectName(QStringLiteral("PfName")); m_name->setMaxLength(32);
    m_name->setPlaceholderText(QStringLiteral("e.g. Work"));
    fl->addWidget(m_name);
    m_error = new QLabel(QStringLiteral("Give this profile a name"), form); m_error->setObjectName(QStringLiteral("PfError")); m_error->hide();
    fl->addSpacing(4); fl->addWidget(m_error); fl->addSpacing(14);
    auto* cl = new QLabel(QStringLiteral("COLOUR"), form); cl->setObjectName(QStringLiteral("PfLabel")); fl->addWidget(cl); fl->addSpacing(8);
    auto* crow = new QHBoxLayout(); crow->setSpacing(6);
    for (const QString& col : ProfileManager::colours()) {
        auto* b = new QToolButton(form); b->setFixedSize(30, 30); b->setCheckable(true); b->setAutoRaise(true); b->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;padding:0;}")); b->setCursor(Qt::PointingHandCursor);
        b->setProperty("colour", col); b->setToolTip(col);
        connect(b, &QToolButton::clicked, this, [this, col]{ m_p.colour = col; refreshPreview(); Motion::pop(m_avatar); });
        crow->addWidget(b); m_colourBtns << b;
    }
    crow->addStretch(); fl->addLayout(crow); fl->addSpacing(16);
    auto* al = new QLabel(QStringLiteral("AVATAR"), form); al->setObjectName(QStringLiteral("PfLabel")); fl->addWidget(al); fl->addSpacing(8);
    auto* arow = new QHBoxLayout(); arow->setSpacing(4);
    for (const QString& g : ProfileManager::glyphs()) {
        auto* b = new QToolButton(form); b->setFixedSize(52, 52); b->setIconSize(QSize(52, 52)); b->setCheckable(true); b->setAutoRaise(true); b->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;padding:0;}"));
        b->setCursor(Qt::PointingHandCursor); b->setProperty("glyph", g); b->setToolTip(g == QStringLiteral("initial") ? QStringLiteral("Initial") : g);
        connect(b, &QToolButton::clicked, this, [this, g]{ m_p.avatar = g; refreshPreview(); Motion::pop(m_avatar); });
        arow->addWidget(b); m_glyphBtns << b;
    }
    {   // 1.8.8: your own photo (cropped to a circle)
        auto* b = new QToolButton(form); b->setFixedSize(52, 52); b->setIconSize(QSize(52, 52)); b->setCheckable(true); b->setAutoRaise(true);
        b->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;padding:0;}"));
        b->setCursor(Qt::PointingHandCursor); b->setToolTip(QStringLiteral("Use a photo…")); b->setAccessibleName(QStringLiteral("Use a photo"));
        connect(b, &QToolButton::clicked, this, [this]{
            const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a profile photo"),
                QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
            if (f.isEmpty()) { refreshPreview(); return; }
            QImage img(f);
            if (img.isNull()) { QMessageBox::warning(this, QStringLiteral("Profile photo"), QStringLiteral("That file isn't an image Pacitum can read.")); refreshPreview(); return; }
            m_photo = ProfileManager::squarePhoto(img);
            ProfileManager::setPendingPhoto(m_p.id, m_photo);
            m_p.avatar = QStringLiteral("photo");
            refreshPreview(); Motion::pop(m_avatar);
        });
        arow->addWidget(b); m_photoBtn = b;
    }
    arow->addStretch(); fl->addLayout(arow); fl->addStretch();
    top->addWidget(form, 1);
    v->addLayout(top, 1);
    // footer
    auto* foot = new QWidget(this); foot->setObjectName(QStringLiteral("PfFooter")); foot->setAttribute(Qt::WA_StyledBackground, true); foot->setFixedHeight(64);
    auto* ft = new QHBoxLayout(foot); ft->setContentsMargins(16, 0, 20, 0); ft->setSpacing(10);
    if (!create) {
        auto* del = new QPushButton(QStringLiteral("Delete profile"), foot); del->setObjectName(QStringLiteral("PfDanger"));
        del->setIcon(ThemeManager::icon(QStringLiteral("trash"), pal().danger, 16)); del->setCursor(Qt::PointingHandCursor);
        connect(del, &QPushButton::clicked, this, [this]{
            if (QMessageBox::question(this, QStringLiteral("Delete profile"),
                    QStringLiteral("Delete “%1”? This deletes its history, bookmarks and passwords from this computer.").arg(m_p.name),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
            QString err;
            if (!ProfileManager::instance()->remove(m_p.id, &err)) { QMessageBox::warning(this, QStringLiteral("Delete profile"), err); return; }
            done(2);
        });
        ft->addWidget(del);
    }
    ft->addStretch();
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), foot); cancel->setObjectName(QStringLiteral("PfPill")); cancel->setCursor(Qt::PointingHandCursor);
    auto* save = new QPushButton(create ? QStringLiteral("Create") : QStringLiteral("Save"), foot); save->setObjectName(QStringLiteral("PfPrimary"));
    save->setDefault(true); save->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &ProfileEditDialog::accept);
    ft->addWidget(cancel); ft->addWidget(save);
    v->addWidget(foot);
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString& t){ m_p.name = t; m_name->setProperty("error", false); m_error->hide();
        m_name->style()->unpolish(m_name); m_name->style()->polish(m_name); refreshPreview(); });
    refreshPreview();
    m_name->setFocus();
}

void ProfileEditDialog::refreshPreview() {
    const qreal dpr = devicePixelRatioF();
    ProfileInfo shown = m_p; if (shown.name.trimmed().isEmpty()) shown.name = QStringLiteral("?");
    m_avatar->setPixmap(ProfileManager::avatarPixmap(shown, 112, dpr, 2, pal().frame));
    m_previewName->setText(m_p.name.trimmed().isEmpty() ? QStringLiteral("New profile") : m_p.name.trimmed());
    // mini browser: chrome tinted with the colour, accent button
    const int w = 282, hgt = 104;
    QPixmap pm(qRound(w * dpr), qRound(hgt * dpr)); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    {
        QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
        const QColor c(m_p.colour), base(pal().toolbar);
        auto mix = [](QColor a, QColor b, double t){ return QColor::fromRgbF(a.redF()*(1-t)+b.redF()*t, a.greenF()*(1-t)+b.greenF()*t, a.blueF()*(1-t)+b.blueF()*t); };
        QPainterPath clip; clip.addRoundedRect(QRectF(0.5, 0.5, w - 1, hgt - 1), 12, 12);
        g.setClipPath(clip);
        g.fillRect(QRectF(0, 0, w, hgt), QColor(pal().page));
        g.fillRect(QRectF(0, 0, w, 38), mix(base, c, dark() ? 0.18 : 0.14));
        g.setPen(Qt::NoPen); g.setBrush(QColor(pal().toolbar));
        g.drawRoundedRect(QRectF(8, 6, 50, 11), 4, 4);
        g.drawRoundedRect(QRectF(8, 22, w - 36, 11), 5.5, 5.5);
        g.setBrush(c); g.drawEllipse(QRectF(w - 22, 21, 13, 13));
        g.setBrush(QColor(pal().surface)); g.drawRoundedRect(QRectF(10, 50, 124, 6), 3, 3);
        g.setBrush(c); g.drawRoundedRect(QRectF(10, 66, 44, 11), 5.5, 5.5);
        g.setClipping(false); g.setBrush(Qt::NoBrush); g.setPen(QPen(QColor(pal().border), 1)); g.drawPath(clip);
    }
    static_cast<QLabel*>(m_mini)->setPixmap(pm);
    for (auto* b : m_colourBtns) {
        const QString col = b->property("colour").toString(); const bool sel = col.compare(m_p.colour, Qt::CaseInsensitive) == 0;
        QPixmap s(qRound(30 * dpr), qRound(30 * dpr)); s.setDevicePixelRatio(dpr); s.fill(Qt::transparent);
        QPainter g(&s); g.setRenderHint(QPainter::Antialiasing); g.setPen(Qt::NoPen); g.setBrush(QColor(col)); g.drawEllipse(QRectF(4, 4, 22, 22));
        if (sel) { g.setBrush(Qt::NoBrush); g.setPen(QPen(QColor(pal().text), 2)); g.drawEllipse(QRectF(1, 1, 28, 28)); }
        g.end(); b->setIcon(QIcon(s)); b->setIconSize(QSize(30, 30)); b->setChecked(sel);
    }
    for (auto* b : m_glyphBtns) {
        ProfileInfo q = shown; q.avatar = b->property("glyph").toString();
        const bool sel = q.avatar == m_p.avatar || (m_p.avatar.isEmpty() && q.avatar == QStringLiteral("initial"));
        b->setIcon(QIcon(ProfileManager::avatarPixmap(q, 44, dpr, sel ? 2 : 0, pal().page)));
        b->setChecked(sel);
    }
    if (m_photoBtn) {
        ProfileInfo q = shown; q.avatar = QStringLiteral("photo");
        const bool sel = m_p.avatar == QStringLiteral("photo");
        if (!ProfileManager::photoFor(q).isNull()) m_photoBtn->setIcon(QIcon(ProfileManager::avatarPixmap(q, 44, dpr, sel ? 2 : 0, pal().page)));
        else {   // no photo yet: an "add photo" circle
            QPixmap s(qRound(52 * dpr), qRound(52 * dpr)); s.setDevicePixelRatio(dpr); s.fill(Qt::transparent);
            QPainter g(&s); g.setRenderHint(QPainter::Antialiasing);
            g.setPen(QPen(QColor(pal().borderStrong), 1.5, Qt::DashLine)); g.setBrush(QColor(pal().surface)); g.drawEllipse(QRectF(5, 5, 42, 42));
            g.drawPixmap(QPointF(16, 16), ThemeManager::icon(QStringLiteral("camera"), pal().textSecondary, 20).pixmap(QSize(20, 20), dpr));
            g.end(); m_photoBtn->setIcon(QIcon(s));
        }
        m_photoBtn->setChecked(sel);
    }
}

ProfileEditDialog::~ProfileEditDialog() { ProfileManager::setPendingPhoto(QString(), QImage()); }

void ProfileEditDialog::accept() {
    if (m_name->text().trimmed().isEmpty()) {
        m_name->setProperty("error", true); m_name->style()->unpolish(m_name); m_name->style()->polish(m_name);
        m_error->show(); m_name->setFocus(); return;
    }
    m_p.name = m_name->text().trimmed().left(32);
    QDialog::accept();
}

bool ProfileEditDialog::editProfile(const QString& id, QWidget* parent) {
    auto* pm = ProfileManager::instance();
    if (!pm->contains(id)) return false;
    ProfileEditDialog d(pm->profile(id), false, parent);
    const int r = d.exec();
    if (r == QDialog::Accepted) {
        pm->update(d.result());
        if (!d.photo().isNull() && d.result().avatar == QStringLiteral("photo")) pm->setPhoto(id, d.photo());
        return true;
    }
    return r == 2;
}
QString ProfileEditDialog::addProfile(QWidget* parent) {
    ProfileInfo p; p.name.clear();
    const auto used = ProfileManager::instance()->profiles();
    for (const QString& c : ProfileManager::colours()) {   // first colour not taken yet
        bool taken = false; for (const auto& u : used) taken |= u.colour.compare(c, Qt::CaseInsensitive) == 0;
        if (!taken) { p.colour = c; break; }
    }
    ProfileEditDialog d(p, true, parent);
    if (d.exec() != QDialog::Accepted) return {};
    const ProfileInfo r = d.result();
    const bool photo = r.avatar == QStringLiteral("photo") && !d.photo().isNull();
    const QString id = ProfileManager::instance()->create(r.name, r.colour, photo ? QStringLiteral("initial") : r.avatar).id;
    if (photo) ProfileManager::instance()->setPhoto(id, d.photo());
    return id;
}

// ======================= switcher =======================
ProfileSwitcher::ProfileSwitcher(QWidget* parent) : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(16, 4, 16, 28);
    m_card = new QFrame(this); m_card->setObjectName(QStringLiteral("PsCard")); m_card->setFixedWidth(320);
    auto* sh = new QGraphicsDropShadowEffect(m_card); sh->setBlurRadius(32); sh->setOffset(0, 12);
    sh->setColor(dark() ? QColor(0, 0, 0, 128) : QColor(16, 24, 40, 46)); m_card->setGraphicsEffect(sh);
    outer->addWidget(m_card);
    setStyleSheet(baseStyle());
    build();
}

void ProfileSwitcher::build() {
    auto* pm = ProfileManager::instance();
    const ProfileInfo me = pm->current();
    const qreal dpr = devicePixelRatioF();
    auto* v = new QVBoxLayout(m_card); v->setContentsMargins(0, 0, 0, 6); v->setSpacing(0);
    // banner (64 px gradient of the profile colour 33 % -> 7 %) with the avatar overlapping it
    auto* head = new QLabel(m_card); head->setFixedHeight(64 + 36);
    {
        QPixmap b(qRound(320 * dpr), qRound(100 * dpr)); b.setDevicePixelRatio(dpr); b.fill(Qt::transparent);
        QPainter g(&b); g.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip; clip.addRoundedRect(QRectF(1, 1, 318, 120), 13, 13); g.setClipPath(clip);
        QColor c1(me.colour), c2(me.colour); c1.setAlphaF(0.33); c2.setAlphaF(0.07);
        QLinearGradient lg(0, 0, 320, 64); lg.setColorAt(0, c1); lg.setColorAt(1, c2);
        g.fillRect(QRectF(0, 0, 320, 64), lg); g.setClipping(false);
        const QPixmap av = ProfileManager::avatarPixmap(me, 64, dpr, 3, pal().toolbar);
        g.drawPixmap(QPointF((320 - av.width() / dpr) / 2.0, 64 - 36), av);
        g.end(); head->setPixmap(b);
    }
    v->addWidget(head);
    auto* name = new QLabel(me.name, m_card); name->setObjectName(QStringLiteral("PsName")); name->setAlignment(Qt::AlignCenter);
    auto* sub = new QLabel(me.isGuest ? QStringLiteral("Guest · nothing is saved") : QStringLiteral("Local profile · Not syncing"), m_card);
    sub->setObjectName(QStringLiteral("PfHint")); sub->setAlignment(Qt::AlignCenter);
    v->addSpacing(8); v->addWidget(name); v->addWidget(sub); v->addSpacing(10);
    auto* pills = new QHBoxLayout(); pills->setSpacing(8); pills->addStretch();
    if (!me.isGuest) {
        auto* edit = new QPushButton(QStringLiteral("Edit"), m_card); edit->setObjectName(QStringLiteral("PfPill"));
        edit->setIcon(ThemeManager::icon(QStringLiteral("edit"), pal().text, 14)); edit->setCursor(Qt::PointingHandCursor);
        const QString id = me.id;
        connect(edit, &QPushButton::clicked, this, [this, id]{ QWidget* par = parentWidget(); close(); ProfileEditDialog::editProfile(id, par); });
        pills->addWidget(edit);
    }
    auto* st = new QPushButton(QStringLiteral("Settings"), m_card); st->setObjectName(QStringLiteral("PfPill"));
    st->setIcon(ThemeManager::icon(QStringLiteral("settings"), pal().text, 14)); st->setCursor(Qt::PointingHandCursor);
    connect(st, &QPushButton::clicked, this, [this]{ emit settingsRequested(); close(); });
    pills->addWidget(st); pills->addStretch(); v->addLayout(pills); v->addSpacing(12);
    auto divider = [&]{ auto* d = new QFrame(m_card); d->setObjectName(QStringLiteral("PsDivider")); v->addWidget(d); v->addSpacing(6); };
    divider();
    auto* lists = new QVBoxLayout(); lists->setContentsMargins(6, 0, 6, 0); lists->setSpacing(0);
    QList<ProfileInfo> others; for (const auto& p : pm->profiles()) if (me.isGuest || p.id != me.id) others << p;
    if (!others.isEmpty()) {
        auto* lbl = new QLabel(QStringLiteral("OTHER PROFILES"), m_card); lbl->setObjectName(QStringLiteral("PfLabel")); lbl->setContentsMargins(8, 4, 0, 4);
        lists->addWidget(lbl);
    }
    auto row = [&](const QPixmap& icon, const QString& text, const QString& subText, std::function<void()> fn) {
        auto* b = new QPushButton(m_card); b->setObjectName(QStringLiteral("PsRow")); b->setCursor(Qt::PointingHandCursor);
        auto* hl = new QHBoxLayout(b); hl->setContentsMargins(8, 6, 8, 6); hl->setSpacing(10);
        auto* il = new QLabel(b); il->setPixmap(icon); il->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* col = new QVBoxLayout(); col->setSpacing(0);
        auto* t = new QLabel(text, b); t->setStyleSheet(QStringLiteral("font-weight:500;")); t->setAttribute(Qt::WA_TransparentForMouseEvents);
        col->addWidget(t);
        if (!subText.isEmpty()) { auto* s = new QLabel(subText, b); s->setObjectName(QStringLiteral("PfHint")); s->setAttribute(Qt::WA_TransparentForMouseEvents); col->addWidget(s); }
        hl->addWidget(il); hl->addLayout(col, 1);
        b->setMinimumHeight(46);
        connect(b, &QPushButton::clicked, this, [this, fn]{ close(); fn(); });
        lists->addWidget(b);
    };
    for (const auto& p : others) {
        const QString id = p.id;
        row(ProfileManager::avatarPixmap(p, 30, dpr), p.name, p.email.isEmpty() ? QStringLiteral("Local profile") : p.email, [id]{ ProfileManager::instance()->launch(id); });
    }
    ProfileInfo g; g.isGuest = true; g.avatar = QStringLiteral("guest");
    row(ProfileManager::avatarPixmap(g, 30, dpr), QStringLiteral("Open Guest window"), QString(), []{ ProfileManager::instance()->launchGuest(); });
    v->addLayout(lists); v->addSpacing(6); divider();
    auto action = [&](const QString& icon, const QString& text, const QString& keys, std::function<void()> fn) {
        auto* b = new QPushButton(m_card); b->setObjectName(QStringLiteral("PsAction")); b->setCursor(Qt::PointingHandCursor);
        auto* hl = new QHBoxLayout(b); hl->setContentsMargins(10, 0, 10, 0); hl->setSpacing(12);
        auto* il = new QLabel(b); il->setPixmap(ThemeManager::icon(icon, pal().textSecondary, 18).pixmap(QSize(18, 18), dpr)); il->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* t = new QLabel(text, b); t->setAttribute(Qt::WA_TransparentForMouseEvents);
        hl->addWidget(il); hl->addWidget(t, 1);
        if (!keys.isEmpty()) { auto* k = new QLabel(keys, b); k->setObjectName(QStringLiteral("PsShortcut")); k->setAttribute(Qt::WA_TransparentForMouseEvents); hl->addWidget(k); }
        connect(b, &QPushButton::clicked, this, [this, fn]{ close(); fn(); });
        auto* wrap = new QHBoxLayout(); wrap->setContentsMargins(6, 0, 6, 0); wrap->addWidget(b); v->addLayout(wrap);
    };
    QWidget* par = parentWidget();
    action(QStringLiteral("profile-add"), QStringLiteral("Add profile"), QString(), [par]{
        const QString id = ProfileEditDialog::addProfile(par);
        if (!id.isEmpty()) ProfileManager::instance()->launch(id);
    });
    action(QStringLiteral("profile-switch"), QStringLiteral("Manage profiles"), QStringLiteral("Ctrl+Shift+M"), [this]{ emit manageRequested(); });
}

void ProfileSwitcher::showUnder(QWidget* anchor) {
    adjustSize();
    const QPoint br = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    const QMargins m = layout()->contentsMargins();
    move(br.x() + 4 - (width() - m.right()), br.y() + 6 - m.top());
    show();   // popups get the 140 ms OutCubic fade from the app-wide fader (Widgets.cpp)
}

void paintProfileAvatarLabel(QLabel* label, int avatarSize, qreal dpr) {
    const ProfileInfo me = ProfileManager::instance()->current();
    const QPixmap pm = ProfileManager::avatarPixmap(me, avatarSize, dpr, 2, pal().toolbar);
    label->setText(QString()); label->setPixmap(pm);
    label->setFixedSize(pm.size() / pm.devicePixelRatio());
    label->setToolTip(me.name);
}
