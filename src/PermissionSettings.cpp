#include "PermissionSettings.h"
#include "PermissionUi.h"
#include "ThemeManager.h"
#include "Motion.h"
#include "Widgets.h"
#include "AppSettings.h"
#include "WebProfile.h"
#include <QStackedWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QToolButton>
#include <QMenu>
#include <QMouseEvent>
#include <QComboBox>
#include <QWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QNetworkCookie>
#include <QTimer>
#include <QPointer>

using Perm::Kind;
using Perm::Decision;

namespace {
class ClickRow : public QWidget {
public:
    std::function<void()> onClick;
    using QWidget::QWidget;
protected:
    void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && onClick && rect().contains(e->pos())) onClick(); }
};
QString defaultSub(Perm::Store* st, Kind k) {
    const Decision d = st->defaultFor(k);
    switch (k) {
    case Kind::Notifications: return d == Decision::Block ? QStringLiteral("Not allowed") : st->quietNotifications() ? QStringLiteral("Quiet prompts · %1 blocked").arg(st->originsWith(k, Decision::Block).size()) : QStringLiteral("Sites can ask");
    case Kind::ScreenShare: return d == Decision::Block ? QStringLiteral("Not allowed") : QStringLiteral("Always asks");
    case Kind::Popups: return d == Decision::Allow ? QStringLiteral("Allowed") : QStringLiteral("Blocked");
    case Kind::Storage: return QStringLiteral("Managed automatically · sites can ask");
    case Kind::Downloads: return d == Decision::Block ? QStringLiteral("Not allowed") : QStringLiteral("Sites ask before downloading several files");
    case Kind::Clipboard: return d == Decision::Block ? QStringLiteral("Not allowed") : QStringLiteral("Sites ask before reading");
    default: return d == Decision::Block ? QStringLiteral("Not allowed") : d == Decision::Allow ? QStringLiteral("Allowed") : QStringLiteral("Sites ask before using");
    }
}
}

SitePermissionsPage::SitePermissionsPage(Perm::Store* store, QWidget* parent) : QWidget(parent), m_store(store) {
    auto* l = new QVBoxLayout(this); l->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(this); l->addWidget(m_stack);
    showList();
}

void SitePermissionsPage::swapTo(QWidget* w) {
    m_stack->addWidget(w); m_stack->setCurrentWidget(w);
    if (m_current && m_current != w) m_current->deleteLater();
    m_current = w;
    Motion::fadeIn(w, Motion::Base);
}

QWidget* SitePermissionsPage::page(const QString& title, QVBoxLayout** out, bool back) {
    auto* scroll = new QScrollArea(); scroll->setObjectName(QStringLiteral("SettingsScroll"));
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget(scroll); inner->setObjectName(QStringLiteral("SettingsContent"));
    auto* outer = new QHBoxLayout(inner); outer->setContentsMargins(48, 24, 48, 24);
    auto* col = new QWidget(inner); col->setMaximumWidth(760);
    auto* l = new QVBoxLayout(col); l->setContentsMargins(0, 0, 0, 0); l->setSpacing(10);
    if (back) {
        auto* b = new QPushButton(QStringLiteral("‹  Site settings"), col);
        b->setFlat(true); b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QPushButton{color:%1;border:none;text-align:left;font-size:13px;padding:0;}").arg(ThemeManager::current().accent));
        connect(b, &QPushButton::clicked, this, [this]{ QTimer::singleShot(0, this, &SitePermissionsPage::showList); });
        l->addWidget(b, 0, Qt::AlignLeft);
    }
    if (!title.isEmpty()) { auto* h = new QLabel(title, col); h->setObjectName(QStringLiteral("SettingsH1")); l->addWidget(h); l->addSpacing(6); }
    outer->addWidget(col, 1);
    scroll->setWidget(inner);
    *out = l;
    QTimer::singleShot(0, l, [l]{ l->addStretch(1); });
    return scroll;
}

QWidget* SitePermissionsPage::card(QVBoxLayout* pl, const QString& heading) {
    if (!heading.isEmpty()) { auto* h2 = new QLabel(heading); h2->setObjectName(QStringLiteral("SettingsH2")); pl->addWidget(h2); }
    auto* c = new QFrame(); c->setObjectName(QStringLiteral("Card"));
    auto* l = new QVBoxLayout(c); l->setContentsMargins(0, 0, 0, 0); l->setSpacing(0);
    pl->addWidget(c); pl->addSpacing(14);
    return c;
}

QWidget* SitePermissionsPage::row(QWidget* c, const QString& glyph, const QString& title, const QString& sub, QWidget* control, bool clickable) {
    const auto& pal = ThemeManager::current();
    auto* l = static_cast<QVBoxLayout*>(c->layout());
    if (l->count() > 0) { auto* d = new QFrame(c); d->setObjectName(QStringLiteral("Divider")); l->addWidget(d); }
    auto* r = new ClickRow(c); r->setMinimumHeight(56);
    if (clickable) r->setCursor(Qt::PointingHandCursor);
    auto* rl = new QHBoxLayout(r); rl->setContentsMargins(20, 10, 20, 10); rl->setSpacing(14);
    if (!glyph.isEmpty()) {
        auto* gi = new QLabel(r);
        if (glyph.startsWith(QLatin1String("site:"))) gi->setPixmap(PermUi::letterIcon(Perm::hostOf(glyph.mid(5)), 20).pixmap(20, 20));
        else gi->setPixmap(PermUi::glyphPixmap(glyph, pal.textSecondary, 18, r->devicePixelRatioF()));
        rl->addWidget(gi);
    }
    auto* tx = new QVBoxLayout(); tx->setSpacing(2);
    auto* t = new QLabel(title, r); tx->addWidget(t);
    if (!sub.isEmpty()) { auto* s = new QLabel(sub, r); s->setObjectName(QStringLiteral("RowSub")); s->setWordWrap(true); tx->addWidget(s); }
    rl->addLayout(tx, 1);
    if (control) { control->setParent(r); rl->addWidget(control, 0, Qt::AlignVCenter); }
    if (clickable) { auto* ch = new QLabel(r); ch->setPixmap(PermUi::glyphPixmap(QStringLiteral("chevron-right"), pal.textTertiary, 16, r->devicePixelRatioF())); rl->addWidget(ch); }
    r->setAccessibleName(title + (sub.isEmpty() ? QString() : QStringLiteral(", ") + sub));
    l->addWidget(r);
    return r;
}

void SitePermissionsPage::showList() {
    QVBoxLayout* l; QWidget* pg = page(QString(), &l, false);
    // Recent activity (last 4 sites)
    const QStringList recent = m_store->recentOrigins(4);
    if (!recent.isEmpty()) {
        auto* c = card(l, QStringLiteral("Recent activity"));
        for (const QString& o : recent) {
            auto* r = static_cast<ClickRow*>(row(c, QStringLiteral("site:") + o, Perm::hostOf(o), m_store->summary(o), nullptr, true));
            r->onClick = [this, o]{ QTimer::singleShot(0, this, [this, o]{ showSite(o); }); };
        }
    }
    auto* c = card(l, QStringLiteral("Permissions"));
    for (Kind k : Perm::settingsKinds()) {
        if (k == Kind::Fonts) continue;
        auto* r = static_cast<ClickRow*>(row(c, Perm::glyph(k), Perm::title(k), defaultSub(m_store, k), nullptr, true));
        r->onClick = [this, k]{ QTimer::singleShot(0, this, [this, k]{ showKind(k); }); };
    }
    auto* c2 = card(l, QStringLiteral("Content"));
    auto* js = new ToggleSwitch(); js->setChecked(AppSettings::read().value(QStringLiteral("javascript_enabled")).toBool(true));
    connect(js, &ToggleSwitch::toggled, this, [this](bool on){ AppSettings::set(QStringLiteral("javascript_enabled"), on); emit settingChanged(QStringLiteral("javascript_enabled")); });
    row(c2, QStringLiteral("code"), QStringLiteral("JavaScript"), QStringLiteral("Sites can use JavaScript (most sites need it)"), js, false);
    swapTo(pg);
}

void SitePermissionsPage::showKind(Kind k) {
    QVBoxLayout* l; QWidget* pg = page(Perm::title(k), &l, true);
    const auto& pal = ThemeManager::current();
    auto* c = card(l, QStringLiteral("Default behaviour"));
    auto* group = new QButtonGroup(c);
    struct Opt { QString text, sub; Decision d; int quiet; };
    QList<Opt> opts;
    if (k == Kind::Popups) opts = {{QStringLiteral("Sites can send pop-ups and use redirects"), QString(), Decision::Allow, -1},
                                   {QStringLiteral("Don't allow sites to send pop-ups or use redirects"), QStringLiteral("Pop-ups opened by a click are still allowed"), Decision::Block, -1}};
    else if (k == Kind::Notifications) opts = {{QStringLiteral("Sites can ask to send notifications"), QString(), Decision::Ask, 0},
                                               {QStringLiteral("Use quieter messaging"), QStringLiteral("Blocks prompts after repeated dismissals or on page load"), Decision::Ask, 1},
                                               {Perm::blockVerb(k), QString(), Decision::Block, -1}};
    else opts = {{k == Kind::ScreenShare ? QStringLiteral("Sites can ask to share your screen") : QStringLiteral("Sites can ask"), k == Kind::Storage ? QStringLiteral("Chromium may still keep data persistent for sites you use a lot") : QString(), Decision::Ask, -1},
                 {Perm::blockVerb(k), QString(), Decision::Block, -1}};
    const Decision cur = m_store->defaultFor(k);
    for (const auto& o : opts) {
        auto* rb = new QRadioButton();
        rb->setAccessibleName(o.text);
        group->addButton(rb);
        rb->setChecked(o.d == cur && (o.quiet < 0 || (o.quiet == 1) == m_store->quietNotifications()));
        row(c, QString(), o.text, o.sub, rb, false);
        const Opt opt = o;
        connect(rb, &QRadioButton::toggled, this, [this, k, opt](bool on){
            if (!on) return;
            m_store->setDefault(k, opt.d);
            if (opt.quiet >= 0) m_store->setQuietNotifications(opt.quiet == 1);
            if (k == Kind::Popups) { AppSettings::set(QStringLiteral("popups_allowed"), opt.d == Decision::Allow); Pacitum::webPrefs().popups = opt.d == Decision::Allow; }
        });
    }
    auto siteList = [&](const QString& heading, Decision d) {
        QStringList sites = m_store->originsWith(k, d);
        if (Perm::isDevice(k) && d == Decision::Allow) for (const QString& o : m_store->origins()) if (!sites.contains(o) && !m_store->devices(o, k).isEmpty() && m_store->decision(o, k) != Decision::Block) sites << o;
        auto* lc = card(l, heading);
        if (sites.isEmpty()) { row(lc, QString(), QStringLiteral("No sites"), QString(), nullptr, false); return; }
        for (const QString& o : sites) {
            QString sub;
            if (Perm::isDevice(k)) { QStringList n; for (const auto& v : m_store->devices(o, k)) n << v.toObject().value(QStringLiteral("name")).toString(); sub = n.join(QStringLiteral(", ")); }
            auto* more = new QToolButton(); more->setAutoRaise(true); more->setFixedSize(32, 32); more->setAccessibleName(QStringLiteral("More actions for %1").arg(Perm::hostOf(o)));
            more->setIcon(ThemeManager::icon(QStringLiteral("more-horizontal"), pal.textSecondary, 18));
            connect(more, &QToolButton::clicked, this, [this, more, o, k]{
                QMenu m(more);
                m.addAction(QStringLiteral("Allow"), this, [this, o, k]{ m_store->setDecision(o, k, Decision::Allow); });
                m.addAction(QStringLiteral("Block"), this, [this, o, k]{ m_store->setDecision(o, k, Decision::Block); });
                m.addAction(QStringLiteral("Remove"), this, [this, o, k]{ m_store->setDecision(o, k, Decision::Ask); if (Perm::isDevice(k)) m_store->revokeDevices(o, k); });
                m.exec(more->mapToGlobal(QPoint(0, more->height())));
                QTimer::singleShot(0, this, [this, k]{ showKind(k); });
            });
            row(lc, QStringLiteral("site:") + o, Perm::hostOf(o), sub, more, false);
        }
    };
    siteList(QStringLiteral("Allowed"), Decision::Allow);
    siteList(QStringLiteral("Not allowed"), Decision::Block);
    swapTo(pg);
}

void SitePermissionsPage::showSite(const QString& origin) {
    QVBoxLayout* l; QWidget* pg = page(QString(), &l, true);
    const auto& pal = ThemeManager::current();
    const QString host = Perm::hostOf(origin);
    auto* head = new QWidget(); auto* hl = new QHBoxLayout(head); hl->setContentsMargins(0, 0, 0, 8); hl->setSpacing(14);
    auto* fi = new QLabel(head); fi->setPixmap(PermUi::letterIcon(host, 40).pixmap(40, 40));
    auto* tl = new QVBoxLayout(); tl->setSpacing(2);
    auto* h1 = new QLabel(host.toHtmlEscaped(), head); h1->setObjectName(QStringLiteral("SettingsH1"));
    auto* meta = new QLabel(QStringLiteral("Counting cookies…"), head); meta->setObjectName(QStringLiteral("RowSub"));
    tl->addWidget(h1); tl->addWidget(meta);
    hl->addWidget(fi); hl->addLayout(tl, 1);
    auto* del = new QPushButton(QStringLiteral("Delete data")); auto* reset = new QPushButton(QStringLiteral("Reset permissions"));
    for (auto* b : {del, reset}) {
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;border:none;border-radius:16px;padding:7px 16px;} QPushButton:hover{background:%3;}").arg(pal.surface, pal.text, pal.surfaceHover));
        hl->addWidget(b);
    }
    l->addWidget(head);
    // Cookies: Qt has no per-site query; replay the store once and count matching domains.
    auto* cookies = Pacitum::mainProfile()->cookieStore();
    auto count = std::make_shared<QList<QNetworkCookie>>();
    QPointer<QLabel> metaG(meta);
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(cookies, &QWebEngineCookieStore::cookieAdded, meta, [count, host, metaG](const QNetworkCookie& c){
        QString d = c.domain(); if (d.startsWith(QLatin1Char('.'))) d = d.mid(1);
        if (host == d || host.endsWith(QLatin1Char('.') + d)) count->append(c);
        if (metaG) metaG->setText(count->size() == 1 ? QStringLiteral("1 cookie") : QStringLiteral("%1 cookies").arg(count->size()));
    });
    cookies->loadAllCookies();
    QTimer::singleShot(600, meta, [metaG, count]{ if (metaG && count->isEmpty()) metaG->setText(QStringLiteral("No cookies")); });
    connect(del, &QPushButton::clicked, this, [cookies, count, metaG, origin]{
        for (const auto& c : std::as_const(*count)) cookies->deleteCookie(c, QUrl(origin));
        count->clear();
        if (metaG) metaG->setText(QStringLiteral("No cookies"));
    });
    connect(reset, &QPushButton::clicked, this, [this, origin]{ m_store->resetOrigin(origin); QTimer::singleShot(0, this, [this, origin]{ showSite(origin); }); });

    auto* c = card(l, QStringLiteral("Permissions"));
    for (Kind k : Perm::settingsKinds()) {
        auto* cb = new QComboBox(); cb->setMinimumWidth(150); cb->setFixedHeight(32);
        cb->setAccessibleName(Perm::title(k));
        const Decision def = m_store->defaultFor(k);
        const QString defName = def == Decision::Allow ? QStringLiteral("Allow") : def == Decision::Block ? QStringLiteral("Block") : QStringLiteral("Ask");
        cb->addItem(defName + QStringLiteral(" (default)"), int(Decision::Ask));
        if (k != Kind::Popups && k != Kind::Storage && k != Kind::Downloads && k != Kind::ScreenShare) cb->addItem(QStringLiteral("Ask"), -1);
        cb->addItem(QStringLiteral("Allow"), int(Decision::Allow));
        cb->addItem(QStringLiteral("Block"), int(Decision::Block));
        const Decision cur = m_store->decision(origin, k);
        cb->setCurrentIndex(qMax(0, cb->findData(int(cur))));
        auto recolor = [cb, def]{
            const int v = cb->currentData().toInt();
            const Decision d = v == int(Decision::Ask) ? def : v < 0 ? Decision::Ask : Decision(v);
            cb->setStyleSheet(QStringLiteral("QComboBox{border:1px solid %1;border-radius:16px;padding:0 12px;color:%2;background:transparent;}")
                              .arg(ThemeManager::current().borderStrong, PermUi::stateColor(d)));
        };
        recolor();
        connect(cb, &QComboBox::currentIndexChanged, this, [this, cb, origin, k, recolor]{
            const int v = cb->currentData().toInt();
            m_store->setDecision(origin, k, v < 0 || v == int(Decision::Ask) ? Decision::Ask : Decision(v));
            recolor();
        });
        QString sub;
        if (Perm::isDevice(k)) { QStringList n; for (const auto& v : m_store->devices(origin, k)) n << v.toObject().value(QStringLiteral("name")).toString(); if (!n.isEmpty()) sub = QStringLiteral("Allowed devices: ") + n.join(QStringLiteral(", ")); }
        if (k == Kind::Storage) sub = QStringLiteral("Managed automatically by Chromium; Pacitum records the site's request");
        row(c, Perm::glyph(k), Perm::title(k), sub, cb, false);
    }
    swapTo(pg);
}
