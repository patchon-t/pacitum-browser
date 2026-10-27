#include "BrowserTab.h"
#include "PageDialogs.h"
#include <QAuthenticator>
#include <QDateTime>
#include <QSet>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QContextMenuEvent>
#include <QWebEngineContextMenuRequest>
#include "Tools.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QWebEngineScript>
#include <QFontMetrics>
#include <QAction>
#include "Widgets.h"
#include "FindBar.h"
#include "PacitumScheme.h"
#include "WebProfile.h"
#include <functional>

std::function<BrowserTab*(int)> BrowserTab::s_popupFactory;

#include "PermissionController.h"
#include <QPointer>
#include <QTimer>
namespace {
class PacitumWebView : public QWebEngineView {
public:
    using QWebEngineView::QWebEngineView;
    std::function<void(const QPoint&)> menuHandler;   // Pacitum page context menu (replaces Qt's default)
protected:
    void contextMenuEvent(QContextMenuEvent* e) override {
        if (menuHandler && lastContextMenuRequest()) { menuHandler(e->globalPos()); e->accept(); return; }
        QWebEngineView::contextMenuEvent(e);
    }
    QWebEngineView* createWindow(QWebEnginePage::WebWindowType type) override {
        if (!BrowserTab::s_popupFactory) return nullptr;
        BrowserTab* t = BrowserTab::s_popupFactory(int(type));
        return t ? t->view() : nullptr;
    }
};

// Blocked pop-up (permissions-spec §11): Chromium gets a detached page that refuses every
// navigation, so nothing loads, and the first URL is recorded for the "pop-ups blocked" chip.
class PopupSink : public QWebEnginePage {
public:
    PopupSink(QWebEngineProfile* p, PermissionController* c) : QWebEnginePage(p), m_c(c) {
        QTimer::singleShot(4000, this, [this]{ report(QUrl()); deleteLater(); });
    }
protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType, bool) override { report(url); return false; }
    QWebEnginePage* createWindow(WebWindowType) override { return nullptr; }
private:
    void report(const QUrl& u) {
        if (m_done) return;
        if (u.isEmpty() || u.scheme() == QLatin1String("about")) { if (!u.isEmpty()) return; }   // wait for the real target
        m_done = true;
        if (m_c) m_c->popupBlocked(u);
        QTimer::singleShot(0, this, &QObject::deleteLater);
    }
    QPointer<PermissionController> m_c;
    bool m_done = false;
};

class PacitumPage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
    QPointer<PermissionController> perms;
    QPointer<PasswordAgent> forms;   // 2.0.1 passwords / autofill helper
protected:
    // 2.0.1 page dialogs drawn by Pacitum inside the tab; a site that keeps opening them can be stopped.
    QWidget* tabWidget() const { auto* v = QWebEngineView::forPage(this); return v ? v->parentWidget() : nullptr; }
    QSet<QString> m_muted; QString m_lastOrigin; qint64 m_lastAt = 0; int m_burst = 0;
    bool muted(const QUrl& o) {
        const QString k = o.host();
        if (m_muted.contains(k)) return true;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_burst = (k == m_lastOrigin && now - m_lastAt < 10000) ? m_burst + 1 : 1;
        m_lastOrigin = k; m_lastAt = now;
        return false;
    }
    void after(const QUrl& o, const PageDialogs::Result& r) { if (r.suppress) m_muted.insert(o.host()); m_lastAt = QDateTime::currentMSecsSinceEpoch(); }
    void javaScriptAlert(const QUrl& o, const QString& msg) override {
        if (muted(o)) return;
        after(o, PageDialogs::run(tabWidget(), PageDialogs::Alert, o.host(), msg, {}, m_burst >= 2));
    }
    bool javaScriptConfirm(const QUrl& o, const QString& msg) override {
        // Qt asks "leave this page?" (beforeunload) through confirm
        const bool leave = msg.startsWith(QLatin1String("Are you sure you want to leave"), Qt::CaseInsensitive) || msg.contains(QLatin1String("may not be saved"), Qt::CaseInsensitive);
        if (!leave && muted(o)) return false;
        const auto r = PageDialogs::run(tabWidget(), leave ? PageDialogs::Leave : PageDialogs::Confirm, o.host(), msg, {}, !leave && m_burst >= 2);
        after(o, r);
        return r.ok;
    }
    bool javaScriptPrompt(const QUrl& o, const QString& msg, const QString& def, QString* result) override {
        if (muted(o)) return false;
        const auto r = PageDialogs::run(tabWidget(), PageDialogs::Prompt, o.host(), msg, def, m_burst >= 2);
        after(o, r);
        if (r.ok && result) *result = r.text;
        return r.ok;
    }
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString& message, int line, const QString& source) override {
        if (forms && forms->handleConsole(message)) return;
        QWebEnginePage::javaScriptConsoleMessage(level, message, line, source);
    }
    QWebEnginePage* createWindow(WebWindowType type) override {
        if (perms && !perms->allowPopup(type)) return new PopupSink(profile(), perms);
        if (!BrowserTab::s_popupFactory) return nullptr;
        BrowserTab* t = BrowserTab::s_popupFactory(int(type));
        return t ? t->view()->page() : nullptr;
    }
};
}

#include <QVBoxLayout>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineHistory>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QDateTime>
#include <QPointer>
#include <QHideEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include "Motion.h"
#include <QEasingCurve>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include "ThemeManager.h"
#include "Passwords.h"
#include "PageDialogs.h"
#include <QAuthenticator>
#include <QDateTime>
#include <QSet>
#include "Widgets.h"
#include "Motion.h"

int BrowserTab::s_idCounter = 0;

BrowserTab::BrowserTab(const QString& url, bool privateMode, const QString& searchEngine, QWidget* parent)
    : QWidget(parent), m_private(privateMode), m_searchEngine(searchEngine) {
    s_idCounter++; m_tabId = s_idCounter;

    // (1.5.0-1.5.1 put a QGraphicsOpacityEffect on the whole tab, i.e. on the QWebEngineView. Graphics
    // effects re-render the GPU-composited web view offscreen on every frame; Qt documents them as
    // unsupported for such widgets, and it was a source of black/blank pages, fullscreen glitches
    // and crashes in the compositor. Page transitions now fade a plain veil widget instead.)

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);

    m_progress = new QProgressBar(this);
    m_progress->setFixedHeight(3); m_progress->setTextVisible(false); m_progress->setRange(0,100); m_progress->hide();
    layout->addWidget(m_progress);

    m_view = new PacitumWebView(this);
    static_cast<PacitumWebView*>(m_view)->menuHandler = [this](const QPoint& p){ showContextMenu(p); };
    if (m_private) {
        m_profile = new QWebEngineProfile(this);   // off-the-record
        m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        Pacitum::configureProfile(m_profile, false);
        m_page = new PacitumPage(m_profile, m_view);
    } else {
        m_page = new PacitumPage(Pacitum::mainProfile(), m_view);
    }
    m_view->setPage(m_page);
    PermissionController::installProfile(m_page->profile());
    m_perms = new PermissionController(this, m_view, m_page);
    static_cast<PacitumPage*>(m_page)->perms = m_perms;
    m_page->setUrlRequestInterceptor(new Pacitum::GoogleSignInInterceptor(m_page));
    connect(m_page, &QWebEnginePage::authenticationRequired, this, [this](const QUrl& url, QAuthenticator* auth){
        const auto r = PageDialogs::run(this, PageDialogs::Auth, url.host() + (url.port() > 0 ? QStringLiteral(":%1").arg(url.port()) : QString()),
                                        auth->realm().isEmpty() ? QString() : QStringLiteral("“%1”").arg(auth->realm()), auth->user(), false, url.scheme() == QLatin1String("http"));
        if (r.ok) { auth->setUser(r.user); auth->setPassword(r.pass); } else *auth = QAuthenticator();
    });
    connect(m_page, &QWebEnginePage::proxyAuthenticationRequired, this, [this](const QUrl&, QAuthenticator* auth, const QString& proxyHost){
        const auto r = PageDialogs::run(this, PageDialogs::Auth, proxyHost, QStringLiteral("Your proxy server needs a username and password."), auth->user());
        if (r.ok) { auth->setUser(r.user); auth->setPassword(r.pass); } else *auth = QAuthenticator();
    });
    {   // 2.0.1: saved passwords, addresses and cards
        auto* agent = new PasswordAgent(m_page, m_private, this);
        static_cast<PacitumPage*>(m_page)->forms = agent;
        connect(agent, &PasswordAgent::offerSave, this, &BrowserTab::passwordOffer);
        connect(agent, &PasswordAgent::offerSaveAddress, this, &BrowserTab::addressOffer);
    }

    applyWebSettings();

    connect(m_view->page(), &QWebEnginePage::renderProcessTerminated, this, &BrowserTab::onRenderCrashed);
    // Website fullscreen (YouTube etc.): without a handler Qt rejects every request.
    connect(m_view->page(), &QWebEnginePage::fullScreenRequested, this, &BrowserTab::fullScreenRequested);
    // Background-tab lifecycle (1.5.2, conservative). 1.5.1 applied Chromium's recommendedState
    // directly, including Discarded, on any page whose widget was hidden: pages were frozen while
    // still loading, discarded tabs were then touched (zoom, theme reload, JavaScript, popups opened
    // into them), and the welcome tab's web channel could be discarded. Now: no discarding at all;
    // a tab is only frozen after it has been in the background for kFreezeAfterMs, is fully loaded,
    // silent, not an internal page, not fullscreen, and Chromium agrees it may be frozen. Any API
    // call that needs the page (load, reload, history, zoom, show) thaws it first.
    m_freezeTimer = new QTimer(this);
    m_freezeTimer->setSingleShot(true);
    m_freezeTimer->setInterval(kFreezeAfterMs);
    connect(m_freezeTimer, &QTimer::timeout, this, &BrowserTab::maybeFreeze);
    m_discardTimer = new QTimer(this);
    m_discardTimer->setSingleShot(true);
    m_discardTimer->setInterval(kDiscardAfterMs);
    connect(m_discardTimer, &QTimer::timeout, this, &BrowserTab::maybeDiscard);

    buildSignInBar();
    layout->addWidget(m_signInBar);
    layout->addWidget(m_view, 1);

    m_findBar = new FindBar(this);
    m_findBar->setBrowserView(m_view);
    layout->addWidget(m_findBar);

    connect(m_view, &QWebEngineView::urlChanged, this, &BrowserTab::onUrlChanged);
    connect(m_view, &QWebEngineView::titleChanged, this, &BrowserTab::onTitleChanged);
    connect(m_view, &QWebEngineView::loadStarted, this, &BrowserTab::onLoadStarted);
    connect(m_view, &QWebEngineView::loadProgress, this, &BrowserTab::onLoadProgress);
    connect(m_view, &QWebEngineView::loadFinished, this, &BrowserTab::onLoadFinished);
    connect(m_view, &QWebEngineView::iconChanged, this, &BrowserTab::iconChanged);
    // 1.8.8 speed: hovering a link to another site opens the connection early (DNS + TCP + TLS), so the
    // click loads sooner. Only a <link rel=preconnect> hint: nothing is downloaded, at most 24 sites a page.
    connect(m_view, &QWebEngineView::loadStarted, this, [this]{ m_preconnected.clear(); });
    connect(m_page, &QWebEnginePage::linkHovered, this, [this](const QString& link) {
        const QUrl u(link);
        if (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("http")) return;
        const QString origin = u.scheme() + QStringLiteral("://") + u.authority();
        const QUrl here = m_view->url();
        if (m_private || origin == here.scheme() + QStringLiteral("://") + here.authority()) return;
        if (m_preconnected.contains(origin) || m_preconnected.size() >= 24) return;
        m_preconnected.insert(origin);
        const QString js = QStringLiteral(
            "(function(o){if(!document.head)return;var l=document.createElement('link');l.rel='preconnect';l.href=o;"
            "document.head.appendChild(l);var d=document.createElement('link');d.rel='dns-prefetch';d.href=o;document.head.appendChild(d);})(%1)")
            .arg(QString::fromUtf8(QJsonDocument(QJsonArray{origin}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
        m_page->runJavaScript(js, QWebEngineScript::ApplicationWorld);
    });

    if (!url.isEmpty()) loadUrl(url);
}

void BrowserTab::loadUrl(const QString& raw) {
    QString url = raw.trimmed();
    if (url.isEmpty()) url = PacitumSchemeHandler::newTabUrl();
    // Any "scheme://…" is an address as typed (chrome-extension://, ftp://, view-source: …).
    // 1.9.0 fixes: "chrome-extension://id/options.html" became "https://chrome-extension://…",
    // and "localhost:3000" / "192.168.1.1:8080" were searched instead of opened.
    static const QRegularExpression scheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*://"));
    static const QRegularExpression local(QStringLiteral("^(localhost|\\d{1,3}(\\.\\d{1,3}){3}|\\[[0-9a-fA-F:]+\\])(:\\d+)?([/?#].*)?$"));
    if (!scheme.match(url).hasMatch() && !url.startsWith(QStringLiteral("about:")) && !url.startsWith(QStringLiteral("pacitum:"))
        && !url.startsWith(QStringLiteral("data:")) && !url.startsWith(QStringLiteral("view-source:"))) {
        if (local.match(url).hasMatch()) url = QStringLiteral("http://") + url;   // local servers rarely have HTTPS
        else if (url.contains('.') && !url.contains(' ')) url = QStringLiteral("https://") + url;
        else url = searchUrl(url);
    }
    ensureActive();
    m_view->setUrl(QUrl(url));
}

BrowserTab::~BrowserTab() {
    if (m_pip) endPictureInPicture();   // the view lives in the PiP window: bring it home before we go
}

QPixmap BrowserTab::captureView() const { return m_view && m_view->isVisible() ? m_view->grab() : QPixmap(); }
QRect BrowserTab::viewRectIn(QWidget* w) const {
    if (!m_view || !w) return {};
    return QRect(m_view->mapTo(w->window(), QPoint(0, 0)) - w->mapTo(w->window(), QPoint(0, 0)), m_view->size());
}

// Picture-in-picture: Chromium's own API needs a click on the page, so Pacitum pops the page out
// instead: the main video is pinned to fill the viewport and the web view moves into a small
// always-on-top window (playback never restarts). "Back to tab" undoes both.
void BrowserTab::togglePictureInPicture() {
    if (m_pip) { endPictureInPicture(); return; }
    static const QString start = QStringLiteral(
        "(function(){var vs=Array.prototype.slice.call(document.querySelectorAll('video')).filter(function(v){return v.readyState>0&&v.offsetWidth>0;});"
        "if(!vs.length)return 'none';"
        "vs.sort(function(a,b){return ((b.paused?0:1)-(a.paused?0:1))||(b.clientWidth*b.clientHeight-a.clientWidth*a.clientHeight);});"
        "var v=vs[0];v.setAttribute('data-pacitum-pip','1');"
        "var s=document.getElementById('pacitum-pip-style');if(!s){s=document.createElement('style');s.id='pacitum-pip-style';(document.head||document.documentElement).appendChild(s);}"
        "s.textContent='html,body{overflow:hidden!important;background:#000!important}body *{visibility:hidden!important}"
        "video[data-pacitum-pip]{visibility:visible!important;position:fixed!important;left:0!important;top:0!important;width:100vw!important;height:100vh!important;"
        "max-width:none!important;max-height:none!important;z-index:2147483647!important;background:#000!important;object-fit:contain!important;transform:none!important}';"
        "if(v.paused){var p=v.play();if(p&&p.catch)p.catch(function(){});}return 'ok';})()");
    QPointer<BrowserTab> guard(this);
    m_view->page()->runJavaScript(start, QWebEngineScript::ApplicationWorld, [guard](const QVariant& r) {
        if (!guard) return;
        if (r.toString() != QLatin1String("ok")) { emit guard->notice(QStringLiteral("No video playing on this page")); return; }
        auto* lay = qobject_cast<QVBoxLayout*>(guard->layout());
        if (!lay || guard->m_pip) return;
        guard->setNoFreeze(true);
        guard->m_viewLayoutIndex = lay->indexOf(guard->m_view);
        lay->removeWidget(guard->m_view);
        if (!guard->m_pipPlaceholder) {
            auto* ph = new QWidget(guard);
            auto* pl = new QVBoxLayout(ph); pl->setAlignment(Qt::AlignCenter); pl->setSpacing(12);
            auto* t = new QLabel(QStringLiteral("Playing in picture-in-picture"), ph); t->setObjectName(QStringLiteral("SettingsH2")); t->setAlignment(Qt::AlignCenter);
            auto* b = new QPushButton(QStringLiteral("Bring it back"), ph);
            QObject::connect(b, &QPushButton::clicked, guard, [g = QPointer<BrowserTab>(guard)]{ if (g) g->endPictureInPicture(); });
            pl->addWidget(t); pl->addWidget(b, 0, Qt::AlignCenter);
            guard->m_pipPlaceholder = ph;
        }
        lay->insertWidget(qMax(0, guard->m_viewLayoutIndex), guard->m_pipPlaceholder, 1);
        guard->m_pipPlaceholder->show();
        auto* pip = new PipWindow(guard->m_view, guard->m_title.isEmpty() ? QStringLiteral("Picture-in-picture") : guard->m_title, guard->window());
        guard->m_pip = pip;
        QObject::connect(pip, &PipWindow::returnRequested, guard, [g = QPointer<BrowserTab>(guard)]{ if (g) g->endPictureInPicture(); });
        pip->show();
    });
}

void BrowserTab::endPictureInPicture() {
    if (!m_pip) return;
    PipWindow* pip = m_pip;
    m_pip = nullptr;
    auto* lay = qobject_cast<QVBoxLayout*>(layout());
    if (m_pipPlaceholder) { if (lay) lay->removeWidget(m_pipPlaceholder); m_pipPlaceholder->hide(); }
    m_view->setParent(this);
    if (lay) lay->insertWidget(qMax(0, m_viewLayoutIndex), m_view, 1);
    m_view->show();
    m_view->page()->runJavaScript(QStringLiteral(
        "(function(){var s=document.getElementById('pacitum-pip-style');if(s)s.remove();"
        "var v=document.querySelector('video[data-pacitum-pip]');if(v)v.removeAttribute('data-pacitum-pip');})()"),
        QWebEngineScript::ApplicationWorld);
    setNoFreeze(false);
    pip->m_closingByReturn = true;
    pip->close();
}

QString BrowserTab::searchUrl(const QString& terms) const {
    static const QMap<QString, QString> engines{
        {QStringLiteral("Google"), QStringLiteral("https://www.google.com/search?q=")},
        {QStringLiteral("Bing"), QStringLiteral("https://www.bing.com/search?q=")},
        {QStringLiteral("DuckDuckGo"), QStringLiteral("https://duckduckgo.com/?q=")},
        {QStringLiteral("Yahoo"), QStringLiteral("https://search.yahoo.com/search?p=")},
        {QStringLiteral("Brave"), QStringLiteral("https://search.brave.com/search?q=")}
    };
    const QString base = engines.value(m_searchEngine, QStringLiteral("https://www.google.com/search?q="));
    return base + QString::fromLatin1(QUrl::toPercentEncoding(terms));
}

// 1.9 page context menu (link / image / selection / editable field / page), Pacitum popup style.
// Copy, save and edit items reuse QWebEnginePage's own actions so Chromium does the work.
void BrowserTab::showContextMenu(const QPoint& globalPos) {
    QWebEngineContextMenuRequest* req = m_view->lastContextMenuRequest();
    if (!req) return;
    Pacitum::PopupMenu menu(this);
    const QFontMetrics fm(menu.font());
    auto label = [](const QString& text, const QString& keys) { return keys.isEmpty() ? text : text + QLatin1Char('\t') + keys; };
    auto page = [&](QWebEnginePage::WebAction wa, const QString& text, const QString& keys = QString()) {
        QAction* pa = m_view->pageAction(wa);
        QAction* a = menu.addAction(label(text, keys));
        a->setEnabled(pa && pa->isEnabled());
        if (pa) connect(a, &QAction::triggered, pa, &QAction::trigger);
        return a;
    };
    auto open = [&](const QString& text, const QUrl& url, int target) {
        QAction* a = menu.addAction(text);
        connect(a, &QAction::triggered, this, [this, url, target]{ emit openLinkRequested(url, target); });
        return a;
    };
    auto sep = [&menu]{ if (!menu.isEmpty() && !menu.actions().constLast()->isSeparator()) menu.addSeparator(); };

    const QUrl link = req->linkUrl();
    const QString sel = req->selectedText().simplified();
    const bool editable = req->isContentEditable();
    const bool image = req->mediaType() == QWebEngineContextMenuRequest::MediaTypeImage && req->mediaUrl().isValid();

    if (link.isValid() && !link.isEmpty()) {
        QAction* head = menu.addAction(fm.elidedText(link.toDisplayString(), Qt::ElideMiddle, 260).replace(QLatin1Char('&'), QStringLiteral("&&")));
        head->setEnabled(false);
        open(QStringLiteral("Open link in new tab"), link, NewTab);
        open(QStringLiteral("Open link in new window"), link, NewWindow);
        open(QStringLiteral("Open link in private tab"), link, PrivateTab);
        open(QStringLiteral("Open link in split view"), link, SplitView);
        sep();
        page(QWebEnginePage::CopyLinkToClipboard, QStringLiteral("Copy link address"));
        page(QWebEnginePage::DownloadLinkToDisk, QStringLiteral("Save link as…"));
    }
    if (image) {
        sep();
        open(QStringLiteral("Open image in new tab"), req->mediaUrl(), NewTab);
        page(QWebEnginePage::DownloadImageToDisk, QStringLiteral("Save image as…"));
        page(QWebEnginePage::CopyImageToClipboard, QStringLiteral("Copy image"));
        page(QWebEnginePage::CopyImageUrlToClipboard, QStringLiteral("Copy image address"));
    }
    if (editable) {
        sep();
        page(QWebEnginePage::Undo, QStringLiteral("Undo"), QStringLiteral("Ctrl+Z"));
        page(QWebEnginePage::Redo, QStringLiteral("Redo"), QStringLiteral("Ctrl+Y"));
        menu.addSeparator();
        page(QWebEnginePage::Cut, QStringLiteral("Cut"), QStringLiteral("Ctrl+X"));
        page(QWebEnginePage::Copy, QStringLiteral("Copy"), QStringLiteral("Ctrl+C"));
        page(QWebEnginePage::Paste, QStringLiteral("Paste"), QStringLiteral("Ctrl+V"));
        page(QWebEnginePage::PasteAndMatchStyle, QStringLiteral("Paste as plain text"), QStringLiteral("Ctrl+Shift+V"));
        page(QWebEnginePage::SelectAll, QStringLiteral("Select all"), QStringLiteral("Ctrl+A"));
    } else if (!sel.isEmpty()) {
        sep();
        page(QWebEnginePage::Copy, QStringLiteral("Copy"), QStringLiteral("Ctrl+C"));
        const QString quoted = fm.elidedText(sel, Qt::ElideRight, 160).replace(QLatin1Char('&'), QStringLiteral("&&"));   // no mnemonics
        open(QStringLiteral("Search %1 for \u201C%2\u201D").arg(m_searchEngine, quoted), QUrl(searchUrl(sel)), NewTab);
    }
    if (menu.isEmpty()) {   // plain page
        page(QWebEnginePage::Back, QStringLiteral("Back"), QStringLiteral("Alt+Left"));
        page(QWebEnginePage::Forward, QStringLiteral("Forward"), QStringLiteral("Alt+Right"));
        page(QWebEnginePage::Reload, QStringLiteral("Reload"), QStringLiteral("Ctrl+R"));
        menu.addSeparator();
        page(QWebEnginePage::SavePage, QStringLiteral("Save page as…"), QStringLiteral("Ctrl+S"));
        page(QWebEnginePage::SelectAll, QStringLiteral("Select all"), QStringLiteral("Ctrl+A"));
        const QUrl here = m_view->url();
        if (here.scheme() == QLatin1String("http") || here.scheme() == QLatin1String("https"))
            open(QStringLiteral("View page source\tCtrl+U"), QUrl(QStringLiteral("view-source:") + here.toString()), NewTab);
    }
    menu.exec(globalPos);
}

bool BrowserTab::s_freezeEnabled = true;
bool BrowserTab::s_discardEnabled = true;

bool BrowserTab::hasInternalChannel() const {
    auto* pg = m_view ? m_view->page() : nullptr;
    return pg && pg->webChannel() && (!m_perms || pg->webChannel() != m_perms->channel());
}

bool BrowserTab::isDiscarded() const {
    auto* pg = m_view ? m_view->page() : nullptr;
    return pg && pg->lifecycleState() == QWebEnginePage::LifecycleState::Discarded;
}

void BrowserTab::ensureActive() {
    if (m_freezeTimer) m_freezeTimer->stop();
    if (m_discardTimer) m_discardTimer->stop();
    auto* pg = m_view ? m_view->page() : nullptr;
    if (pg && pg->lifecycleState() != QWebEnginePage::LifecycleState::Active) {
        const bool wasDiscarded = pg->lifecycleState() == QWebEnginePage::LifecycleState::Discarded;
        pg->setLifecycleState(QWebEnginePage::LifecycleState::Active);   // Discarded -> Active reloads the page
        if (wasDiscarded && !qFuzzyCompare(m_view->zoomFactor(), m_zoom)) m_view->setZoomFactor(m_zoom);
    }
}

// Second stage, only for a tab that is already frozen and still in the background: same guards as
// maybeFreeze, plus Chromium must recommend Discarded. Any call that needs the page thaws it first.
void BrowserTab::maybeDiscard() {
    auto* pg = m_view ? m_view->page() : nullptr;
    if (!s_freezeEnabled || !s_discardEnabled || !pg || isVisible() || m_loading || m_noFreeze || !m_pendingUrl.isEmpty()) return;
    if (pg->lifecycleState() != QWebEnginePage::LifecycleState::Frozen || pg->recentlyAudible()) return;
    if (pg->recommendedState() != QWebEnginePage::LifecycleState::Discarded) return;
    if (hasInternalChannel()) return;   // internal pages with a bridge (welcome) are never discarded
    pg->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
}

void BrowserTab::maybeFreeze() {
    auto* pg = m_view ? m_view->page() : nullptr;
    if (!s_freezeEnabled || !pg || isVisible() || m_loading || m_noFreeze || !m_pendingUrl.isEmpty()) return;
    if (m_url.isEmpty() || m_url.startsWith(QStringLiteral("pacitum")) || m_url.startsWith(QStringLiteral("about:"))) return;
    if (pg->recentlyAudible() || pg->lifecycleState() != QWebEnginePage::LifecycleState::Active) return;
    if (pg->recommendedState() == QWebEnginePage::LifecycleState::Active) return;   // Chromium says no (e.g. WebRTC, devtools)
    pg->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
    if (s_discardEnabled && m_discardTimer) m_discardTimer->start();
}

void BrowserTab::setNoFreeze(bool on) { m_noFreeze = on; if (on) ensureActive(); }

void BrowserTab::setWindowMinimized(bool on) {
    if (m_windowMinimized == on || !m_page) return;
    m_windowMinimized = on;
    if (isVisible()) {
        // The shown tab of a minimized window: Chromium still treated it as visible and ran
        // requestAnimationFrame, CSS animations and video at full rate. Hidden pages are throttled.
        if (!m_noFreeze || !on) m_page->setVisible(!on);
    } else if (m_freezeTimer && m_freezeTimer->isActive()) {
        m_freezeTimer->start(on ? kFreezeMinimizedMs : kFreezeAfterMs);   // background tab: freeze sooner while minimized
    }
}

void BrowserTab::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (!e->spontaneous()) { if (m_veil) m_veil->stop(); if (m_navShot) m_navShot->stop(); }   // drop overlays/snapshots of background tabs
    if (!e->spontaneous() && s_freezeEnabled && m_freezeTimer) m_freezeTimer->start(m_windowMinimized ? kFreezeMinimizedMs : kFreezeAfterMs);   // tab switched away (not window minimize)
}

void BrowserTab::applyWebSettings() { Pacitum::applyWebSettings(m_view->settings()); }

void BrowserTab::goBack() { if (m_view->history()->canGoBack()) { ensureActive(); m_view->back(); } }
void BrowserTab::goForward() { if (m_view->history()->canGoForward()) { ensureActive(); m_view->forward(); } }
void BrowserTab::reload() { ensureActive(); m_view->reload(); }
void BrowserTab::stop() { m_view->stop(); }
bool BrowserTab::canGoBack() const { return m_view->history()->canGoBack(); }
bool BrowserTab::canGoForward() const { return m_view->history()->canGoForward(); }

void BrowserTab::setZoomFactor(double f) {
    m_zoom = qBound(0.25, f, 5.0);
    if (!isDiscarded()) m_view->setZoomFactor(m_zoom);   // applied again when the tab is thawed
}

void BrowserTab::setMuted(bool m) { m_view->page()->setAudioMuted(m); }
bool BrowserTab::isMuted() const { return m_view->page()->isAudioMuted(); }

void BrowserTab::refreshTheme() { if (m_signInBar) m_signInBar->refresh(); }
void BrowserTab::showFindBar() { m_findBar->showFind(); }
void BrowserTab::hideFindBar() { m_findBar->hideFind(); }

void BrowserTab::setLazyUrl(const QString& url, const QString& title) {
    m_pendingUrl = url; m_url = url;
    m_title = title.isEmpty() ? url : title;
    emit urlChanged(m_url); emit titleChanged(m_title);
}
void BrowserTab::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    ensureActive();   // thaw a frozen tab before it paints
    if (m_page && !m_windowMinimized && !m_page->isVisible()) m_page->setVisible(true);
    if (!m_pendingUrl.isEmpty()) { const QString u = m_pendingUrl; m_pendingUrl.clear(); loadUrl(u); }
}

void BrowserTab::buildSignInBar() {
    m_signInBar = new InfoBar(this);
    m_signInBar->setKind(InfoBar::Warning);
    m_signInBar->setMessage(tr("Google doesn't allow signing in from this browser."));
    m_signInBar->setPrimary(tr("Open in default browser"), QStringLiteral("open-in-browser"));
    connect(m_signInBar, &InfoBar::primaryClicked, this, [this]{
        const QUrl target = m_signInUrl.isValid() ? m_signInUrl : m_view->url();
        QDesktopServices::openUrl(target);
    });
}

void BrowserTab::showSignInBar(const QUrl& target) {
    m_signInUrl = target;
    m_signInBar->showAnimated();
}

void BrowserTab::checkGoogleSignIn(const QUrl& u) {
    if (Pacitum::isGoogleSignInBlocked(u)) {
        // The rejection page carries the original sign-in URL in "continue" when present.
        QUrl target(QStringLiteral("https://accounts.google.com/"));
        const QString cont = QUrlQuery(u).queryItemValue(QStringLiteral("continue"), QUrl::FullyDecoded);
        if (!cont.isEmpty()) {
            const QUrl c(cont);
            if (c.scheme() == QLatin1String("https")) target = c;
        }
        showSignInBar(target);
        return;
    }
    if (!Pacitum::isGoogleSignInHost(u.host())) { m_signInBar->hideAnimated(); return; }
    // Fallback: detect the rejection by page text (Google sometimes keeps the generic /v3/signin URL).
    m_view->page()->runJavaScript(QStringLiteral(
        "(function(){var t=(document.body&&document.body.innerText)||'';"
        "return /Couldn.t sign you in|This browser or app may not be secure/i.test(t);})()"),
        [this, u](const QVariant& v){ if (v.toBool() && m_view->url() == u) showSignInBar(u); });
}

void BrowserTab::onUrlChanged(const QUrl& u) { m_url = u.toString(); emit urlChanged(m_url); checkGoogleSignIn(u); }
void BrowserTab::onTitleChanged(const QString& t) { m_title = t.isEmpty() ? QStringLiteral("New Tab") : t; emit titleChanged(m_title); }
// Progress is shown in the omnibox (animations-spec §7). The old in-page 3 px QProgressBar was
// shown/hidden on every load and shifted the page down by 3 px each time; it is no longer shown.
void BrowserTab::onLoadStarted() { m_loading = true; m_loadValue = 0; emit loadingChanged(true); m_progress->setValue(0); }
void BrowserTab::onLoadProgress(int p) { m_loadValue = p; m_progress->setValue(p); emit progressChanged(p); }
void BrowserTab::onLoadFinished(bool ok) { checkGoogleSignIn(m_view->url()); m_loading = false; m_loadValue = 100; emit loadingChanged(false); emit loadDone(ok); m_progress->setValue(100); }
// Renderer gone: show the crashed state and reload once after a second -- but not in a loop (1.5.1
// reloaded forever when a page crashed its renderer on load, e.g. after a discard), and not when the
// renderer exited normally.
void BrowserTab::onRenderCrashed(QWebEnginePage::RenderProcessTerminationStatus st, int) {
    emit crashed();
    if (st == QWebEnginePage::NormalTerminationStatus) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastCrashMs > 60000) m_crashCount = 0;
    m_lastCrashMs = now;
    if (++m_crashCount > 2) return;   // user can reload manually
    QTimer::singleShot(1000, this, &BrowserTab::reload);
}

// Page transition (tab switch): a page-coloured veil over the tab fades out (160 ms OutCubic). The
// web view itself is never wrapped in a graphics effect; only the small veil widget is.
void BrowserTab::fadeIn() {
    const int ms = Pacitum::motionMs(160);
    if (Pacitum::reducedMotion() || ms <= 0 || !isVisible() || width() < 2) { if (m_veil) m_veil->stop(); return; }
    if (!m_veil) m_veil = new Motion::FadeOverlay(this);   // one veil per tab, reused on every switch
    m_veil->setColor(QColor(ThemeManager::current().page));
    m_veil->setGeometry(rect());
    m_veil->fade(0.85, 0.0, ms, QEasingCurve::OutCubic);
}

// Back / forward (kit #10/#11): a grab() snapshot of the old page slides 32 px towards the
// direction of travel while fading out (200 ms OutCubic) over the view; the web view itself is
// never animated. Reduced motion: the snapshot only fades (120 ms). Dropped on resize.
void BrowserTab::navTransition(int dir) {
    if (m_navShot) m_navShot->stop();
    if (!m_view || !isVisible() || m_view->width() < 2) return;
    const int fade = Motion::fadeMs(Motion::Medium);
    if (fade <= 0) return;
    QPixmap pm = m_view->grab();   // one snapshot; the web view itself is never animated
    if (pm.isNull()) return;
    if (!m_navShot) m_navShot = new Motion::FadeOverlay(this);
    const bool depth = Motion::enabled();
    // 1.8.5 3D: the old page turns away in perspective. A half-size snapshot keeps each frame cheap.
    if (depth) pm = pm.scaled(pm.size() / 2, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    m_navShot->setPixmap(pm);
    m_navShot->setGeometry(QRect(m_view->mapTo(this, QPoint(0, 0)), m_view->size()));
    if (depth) m_navShot->setTilt(dir < 0 ? 24.0 : -24.0, 0.94);
    m_navShot->fade(1.0, 0.0, fade, QEasingCurve::OutCubic, 0, depth ? QPoint(dir < 0 ? 24 : -24, 0) : QPoint());
}

void BrowserTab::resizeEvent(QResizeEvent* e) {
    if (m_navShot) m_navShot->stop();   // snapshot never outlives a resize
    QWidget::resizeEvent(e);
    if (m_veil) m_veil->setGeometry(rect());   // page-transition veil follows the tab size
}
