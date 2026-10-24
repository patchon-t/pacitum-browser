#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QWebChannel>
#include "FirstRun.h"
#include <QPainter>
#include <QResizeEvent>
#include <QDateTime>
#include "BrowserWindow.h"
#include "BrowserTab.h"
#include "TabWidget.h"
#include "AddressBar.h"
#include "BookmarksBar.h"
#include "BookmarkManager.h"
#include "HistoryManager.h"
#include "AppSettings.h"
#include "Updater.h"
#include "UpdateUi.h"
#include <QShowEvent>
#include <QScreen>
#include <QWindow>
#include "DownloadManager.h"
#include "ProfileManager.h"
#include "Motion.h"
#include "ProfileUi.h"
#include "ThemeManager.h"
#include "SessionManager.h"
#include "ZoomController.h"
#include "AdBlocker.h"
#include "SettingsPage.h"
#include "WebProfile.h"
#include "Widgets.h"
#include <QFileDialog>
#include <QPushButton>
#include <QInputDialog>
#include <QTextDocument>
#include <QDialog>
#include <QListWidget>
#include <QListWidgetItem>
#include <QHBoxLayout>
#include <QStandardPaths>
#include <QPointer>
#include <QWindow>
#include <QMouseEvent>
#include <QWebEngineHistory>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#endif
#include "PacitumScheme.h"
#include <QWidgetAction>
#include <QStyleHints>
#include <QGuiApplication>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QShortcut>
#include "PermissionController.h"
#include "SitePermissions.h"
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineCookieStore>
#include <QStatusBar>
#include <QLocale>
#include <QUrl>
#include <QUrlQuery>
#include <QKeyEvent>
#include <QStyle>
#include <QTimer>
#include <QFileInfo>
#include <QSaveFile>
#include <QDateTime>
#include <QWebEngineFullScreenRequest>
#include <QtMath>
#include <QDir>
#include <QWebEngineView>
#include <QRegularExpression>
#include <QHash>
#include <QFrame>
#include <QLineEdit>
#include "SplitView.h"
#include "ExtensionManager.h"
#include "ChromeExtensions.h"
#include "Tools.h"
#include "TabOverview.h"
#include "SyncManager.h"
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QDir>
#include "Motion.h"

BrowserWindow::BrowserWindow(QWidget* parent) : QMainWindow(parent) {
    m_history = new HistoryManager(QStringLiteral("browser_history.db"));
    m_bookmarks = new BookmarkManager(QStringLiteral("bookmarks.json"));
    m_downloads = DownloadManager::instance();
    m_theme = new ThemeManager(false);
    m_session = new SessionManager(QStringLiteral("browser_session.json"));
    m_zoom = new ZoomController(1.0, this);

    setWindowIcon(QIcon(QStringLiteral(":/icons/pacitum-icon-256.png")));
    // Chrome/Edge-style merged title bar: frameless window, tabs live in the caption row.
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    Pacitum::installMenuFader();
    m_spinTimer = new QTimer(this);
    m_spinTimer->setInterval(50);   // 20 fps spinner is plenty and halves repaint work
    connect(m_spinTimer, &QTimer::timeout, this, &BrowserWindow::tickSpinner);
    m_sessionTimer = new QTimer(this);
    m_sessionTimer->setSingleShot(true);
    m_sessionTimer->setInterval(3000);   // coalesce bursts of navigations / tab changes into one write
    connect(m_sessionTimer, &QTimer::timeout, this, [this]{
        const auto tabs = sessionTabs();
        if (!tabs.isEmpty()) m_session->saveAsync(tabs);
    });
    setupAdBlocker();
    setupDownloadHandler();
    setupUI();
    loadSettings();
    applyTheme();
    restoreOrNewTab();
    updateDownloadsButton();
    installPopupFactory();
    setupSync();
    qApp->installEventFilter(this);
    m_noBackgroundUpdates = qApp->arguments().contains(QStringLiteral("--smoke-test")) || qApp->arguments().contains(QStringLiteral("--stress-test")) || qApp->arguments().contains(QStringLiteral("--resize-test")) || qApp->arguments().contains(QStringLiteral("--permissions-test"));
    setupUpdater();
    setupHome();
#ifdef Q_OS_WIN
    // Do NOT force native window creation from the constructor; apply the frame
    // once Qt has created the HWND during show().
    QTimer::singleShot(0, this, [this]{ applyNativeFrame(); });
#endif
}

void BrowserWindow::showEvent(QShowEvent* e) {
#ifdef Q_OS_WIN
    // Runs before the first ShowWindow: with WS_CAPTION etc. already set, DWM plays the native
    // open animation, and minimize/maximize/restore animate from the very first time.
    if (!e->spontaneous()) applyNativeFrame();
#endif
    QMainWindow::showEvent(e);
    if (!m_shownOnce && !e->spontaneous()) {
        m_shownOnce = true;
        fadeInContent();
    }
}

// Window fade-in on open: a theme-coloured veil over the content fades out (220 ms). Window-level
// opacity (WS_EX_LAYERED) is deliberately not used: it is incompatible with the GPU swap chain
// that hosts the web view and would turn the page black.
void BrowserWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    // Transient full-window overlays (open fade veil, theme cross-fade shot) must follow the size.
    for (auto* c : findChildren<QWidget*>(QStringLiteral("PacitumWindowOverlay"), Qt::FindDirectChildrenOnly)) c->setGeometry(rect());
}

// 1.9.0 / 1.8.11: a new window opens with a circle growing from the centre (plain veil fade with
// reduced motion).
void BrowserWindow::fadeInContent() {
    if (Motion::enabled() && centralWidget()) { Motion::reveal(this, QColor(m_theme->palette().page), Pacitum::guestMode() || m_privateMode ? 620 : 520); return; }
    fadeVeil(Pacitum::guestMode() || m_privateMode ? Motion::Slow : Motion::Medium, 1.0, 30);   // kit #31 / #41
}

// Close / minimize / maximize on macOS and Linux: our frameless window gets no system animation
// there, so a snapshot shrinks away / drops / morphs while the real window has already changed.
void BrowserWindow::animatedMinimize() {
    if (Motion::windowGhostsAllowed() && isVisible() && !isMinimized()) {
        const QRect from = frameGeometry();
        QRect to(QPoint(), from.size() * 0.35); to.moveCenter(QPoint(from.center().x(), (screen() ? screen()->availableGeometry().bottom() : from.bottom()) - to.height() / 3));
        Motion::windowGhost(grab(), from, to, Motion::ms(Motion::Slow), 0.0);
    }
    showMinimized();
}

void BrowserWindow::animatedToggleMaximize() {
    const bool ghost = Motion::windowGhostsAllowed() && isVisible();
    const QPixmap snap = ghost ? grab() : QPixmap();
    const QRect from = frameGeometry();
    if (isMaximized()) showNormal(); else showMaximized();
    if (!ghost) return;
    QPointer<BrowserWindow> self(this);
    QTimer::singleShot(30, this, [self, snap, from]{   // once the window manager has applied the new size
        if (self) Motion::windowGhost(snap, from, self->frameGeometry(), Motion::ms(Motion::Medium), 0.0);
    });
}
// Page-coloured veil over the whole window that fades out (window open, fullscreen enter/exit).
// It is a direct child named PacitumWindowOverlay, so resizeEvent keeps it at the window size.
void BrowserWindow::fadeVeil(int duration, qreal from, int delay) {
    const int ms = Motion::fadeMs(duration);   // opacity only, so it stays (120 ms) with reduced motion
    if (ms <= 0 || !centralWidget()) return;
    auto* veil = new Motion::FadeOverlay(this);   // painted with opacity, no graphics effect
    veil->setObjectName(QStringLiteral("PacitumWindowOverlay"));
    veil->setColor(QColor(m_theme->palette().page));
    veil->setGeometry(rect());
    veil->fade(from, 0.0, ms, QEasingCurve::OutCubic, delay, {}, true);   // starts after the first frame
}

void BrowserWindow::setupUpdater() {
    m_updater = Updater::instance();
    connect(m_updater, &Updater::updateAvailable, this, [this](const UpdateInfo& info, bool user){
        if (!user && AppSettings::get(QStringLiteral("update_skipped_version")).toString() == info.version) return;
        if (!user && (m_chromeHidden || !isActiveWindow())) {   // don't interrupt fullscreen video or another app
            m_pendingUpdatePrompt = true; return;
        }
        m_pendingUpdatePrompt = false;
        UpdateFlow::instance()->offer(info, this, user);
    });
    connect(m_updater, &Updater::upToDate, this, [this](bool user){ if (user) statusBar()->showMessage(m_updater->lastStatus(), 4000); });
    connect(m_updater, &Updater::checkFailed, this, [this](const QString& err, bool user){ if (user) statusBar()->showMessage(tr("Update check failed: %1").arg(err), 6000); });
    if (!m_noBackgroundUpdates) m_updater->setAutoCheck(AppSettings::get(QStringLiteral("auto_check_updates")).toBool(true));
    if (qApp->arguments().contains(QStringLiteral("--check-updates"))) QTimer::singleShot(1500, m_updater, [this]{ m_updater->check(true); });
    UpdateFlow::instance()->attach(this, m_updatePill, m_menuBtn);
    // Started by the updater: tell the install helper we're up and show "Update done".
    if (!m_noBackgroundUpdates) QTimer::singleShot(700, this, [this]{ if (isVisible()) UpdateFlow::instance()->afterStartup(this); });
}

void BrowserWindow::showUpdatePrompt(const UpdateInfo& info) {
    m_pendingUpdatePrompt = false;
    UpdateFlow::instance()->offer(info, this, false);   // a background offer that waited for the window
}

void BrowserWindow::installPopupFactory() {
    QPointer<BrowserWindow> self(this);
    BrowserTab::s_popupFactory = [self]() -> BrowserTab* {
        if (!self) return nullptr;
        self->newTab(QStringLiteral("about:blank"));
        return self->activeTab();
    };
}

BrowserWindow::~BrowserWindow() {
    qApp->removeEventFilter(this);
    if (auto* sm = SyncManager::instance(); sm->history() == m_history) sm->setHistory(nullptr);   // m_history is deleted below
    SyncManager::instance()->disconnect(this);
    // Child widgets (tab bars, tabs) are deleted after this body and after our members are gone.
    // Cut their signals to us now, so no handler runs against freed members (m_thumbs, m_thumbTimer)
    // while the window is being torn down.
    m_thumbTimer.stop();
    for (auto* tw : {m_tabs, m_splitTabs}) if (tw) tw->disconnect(this);
    for (auto* t : findChildren<BrowserTab*>()) t->disconnect(this);
    if (m_firstRun) m_firstRun->waitForImport();   // the import worker writes into m_history
    if (m_updater) m_updater->disconnect(this);
    delete m_history; delete m_bookmarks; delete m_theme; delete m_session;
}

void BrowserWindow::setupAdBlocker() {
    static AdBlocker* shared = nullptr;   // one interceptor for the shared profile (outlives windows)
    if (!shared) { shared = new AdBlocker(true, qApp); Pacitum::mainProfile()->setUrlRequestInterceptor(shared); }
    m_adblock = shared;
}

void BrowserWindow::setupDownloadHandler() {
    connect(Pacitum::mainProfile(), &QWebEngineProfile::downloadRequested, this, [this](QWebEngineDownloadRequest* dl){
        // every window receives the signal; let the active one (or the first) handle it
        if (dl->state() != QWebEngineDownloadRequest::DownloadRequested) return;
        auto* aw = qobject_cast<BrowserWindow*>(QApplication::activeWindow());
        if (aw && aw != this) return;
        // Multiple automatic downloads (§11): held / blocked per origin by the page's controller.
        for (auto* c : PermissionController::all())
            if (c->page() && c->page() == dl->page() && !c->filterDownload(dl)) { dl->cancel(); return; }
        onDownloadRequested(dl);
    });
}

void BrowserWindow::setupUI() {
    setWindowTitle(QStringLiteral("Pacitum Browser"));
    setMinimumSize(720,520); resize(1440,900);   // spec §4.4: min width 720

    auto* central = new QWidget(this); setCentralWidget(central);
    auto* mainLay = new QVBoxLayout(central); mainLay->setContentsMargins(0,0,0,0); mainLay->setSpacing(0);

    createToolbar();

    m_bookmarksBar = new BookmarksBar(m_bookmarks, this);
    connect(m_bookmarksBar, &BookmarksBar::urlActivated, this, &BrowserWindow::navigateToUrl);
    connect(m_bookmarksBar, &BookmarksBar::urlActivatedInNewTab, this, [this](const QString& u){ newTab(u); });
    connect(m_bookmarksBar, &BookmarksBar::bookmarksChanged, this, [this]{
        if (auto* t = activeTab()) m_addressBar->setBookmarked(m_bookmarks->isBookmarked(t->currentUrl()));
    });
    mainLay->addWidget(m_bookmarksBar);
    // 1 px line that separates the chrome from content; always present, even with the bar hidden.
    auto* divider = new QFrame(central); divider->setObjectName(QStringLiteral("ChromeDivider")); divider->setFixedHeight(1);
    mainLay->addWidget(divider);

    // content row: [split view | notes panel]
    m_contentRow = new QWidget(central);
    auto* rowLay = new QHBoxLayout(m_contentRow); rowLay->setContentsMargins(0, 0, 0, 0); rowLay->setSpacing(0);
    m_splitter = new PacitumSplitter(m_contentRow);
    rowLay->addWidget(m_splitter, 1);
    mainLay->addWidget(m_contentRow, 1);
    m_thumbTimer.setSingleShot(true); m_thumbTimer.setInterval(700);
    connect(&m_thumbTimer, &QTimer::timeout, this, &BrowserWindow::captureActiveThumb);

    m_tabs = new PacitumTabWidget(m_verticalTabs, this);
    connect(m_tabs, &PacitumTabWidget::tabCloseRequested, this, &BrowserWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &BrowserWindow::onTabChanged);
    connect(m_tabs, &PacitumTabWidget::newTabRequested, this, [this]{ newTab(); });
    connect(m_tabs, &PacitumTabWidget::tabSplitRequested, this, &BrowserWindow::moveTabToSplit);
    connect(m_tabs, &PacitumTabWidget::tabDuplicateRequested, this, &BrowserWindow::duplicateTab);
    connect(m_tabs, &PacitumTabWidget::tabReopenRequested, this, &BrowserWindow::reopenClosedTab);
    connect(m_tabs, &PacitumTabWidget::tabMuteRequested, this, &BrowserWindow::muteTab);
    connect(m_tabs, &PacitumTabWidget::tabPinRequested, this, &BrowserWindow::pinTab);
    connect(m_tabs, &PacitumTabWidget::tabReloadRequested, this, [this](int i){ if (auto* t = qobject_cast<BrowserTab*>(m_tabs->widget(i))) t->reload(); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this]{ m_tabs->stripBar() ? m_tabs->stripBar()->update() : void(); });
    m_splitter->addWidget(m_tabs);
    m_activeWidget = m_tabs;

    // Vertical stack per design: TabStrip 40 -> Toolbar 44 -> BookmarksBar 32 -> content
    m_tabStrip = m_tabs->createTabStrip(central);
    {
        auto* sl = m_tabs->stripLayout();
        m_minBtn = new WindowButton(WindowButton::Minimize, m_tabStrip);
        m_maxBtn = new WindowButton(WindowButton::Maximize, m_tabStrip);
        m_closeBtn = new WindowButton(WindowButton::Close, m_tabStrip);
        connect(m_minBtn, &QToolButton::clicked, this, &BrowserWindow::animatedMinimize);
        connect(m_maxBtn, &QToolButton::clicked, this, &BrowserWindow::animatedToggleMaximize);
        connect(m_closeBtn, &QToolButton::clicked, this, &QWidget::close);
#ifdef Q_OS_MACOS
        // macOS order and place: close, minimise, zoom on the left, before the tabs
        sl->insertWidget(0, m_closeBtn); sl->insertWidget(1, m_minBtn); sl->insertWidget(2, m_maxBtn); sl->insertSpacing(3, 12);
#else
        sl->addWidget(m_minBtn); sl->addWidget(m_maxBtn); sl->addWidget(m_closeBtn);
#endif
        m_tabStrip->installEventFilter(this);
        m_tabs->stripBar()->installEventFilter(this);
    }
    mainLay->insertWidget(0, m_tabStrip);
    mainLay->insertWidget(1, m_toolbar);

    m_splitTabs = new PacitumTabWidget(m_verticalTabs, this);
    m_splitTabs->hide();
    connect(m_splitTabs, &PacitumTabWidget::tabCloseRequested, this, &BrowserWindow::closeSplitTab);
    connect(m_splitTabs, &QTabWidget::currentChanged, this, &BrowserWindow::onTabChanged);
    connect(m_splitTabs, &PacitumTabWidget::newTabRequested, this, [this]{ newTab(QString{}); /* in split */ });
    connect(m_splitTabs, &PacitumTabWidget::tabSplitRequested, this, &BrowserWindow::moveTabFromSplit);
    connect(m_splitTabs, &PacitumTabWidget::tabDuplicateRequested, this, &BrowserWindow::duplicateTab);
    m_splitter->addWidget(m_splitTabs);

    setupShortcuts();
    createMenuBar();
    createStatusBar();
    menuBar()->hide();          // design uses the ⋮ main menu instead of a menu bar
    statusBar()->hide();
    if (auto* sh = QGuiApplication::styleHints())
        connect(sh, &QStyleHints::colorSchemeChanged, this, [this]{ if (m_themeSetting == QStringLiteral("System")) applyTheme(); });
    applyTheme();
}

QToolButton* BrowserWindow::makeToolBtn(const QString& tip) {
    auto* b = new AnimatedToolButton(m_toolbar);
    b->setFixedSize(36,36); b->setIconSize(QSize(18,18)); b->setRadius(18);   // 1.9: 36 px circles
    b->setToolTip(tip); b->setFocusPolicy(Qt::TabFocus);
    return b;
}

void BrowserWindow::createToolbar() {
    m_toolbar = new QWidget(this); m_toolbar->setObjectName(QStringLiteral("NavToolbar"));
    m_toolbar->setAttribute(Qt::WA_StyledBackground, true);
    m_toolbar->setFixedHeight(48);
    auto* lay = new QHBoxLayout(m_toolbar); lay->setContentsMargins(8,0,8,0); lay->setSpacing(2);
    m_toolbar->installEventFilter(this);   // resize -> collapse buttons per spec §4.4

    m_backBtn = makeToolBtn(QStringLiteral("Back (Alt+Left)")); connect(m_backBtn, &QToolButton::clicked, this, &BrowserWindow::goBack);
    m_forwardBtn = makeToolBtn(QStringLiteral("Forward (Alt+Right)")); connect(m_forwardBtn, &QToolButton::clicked, this, &BrowserWindow::goForward);
    m_reloadBtn = makeToolBtn(QStringLiteral("Reload (F5)"));
    connect(m_reloadBtn, &QToolButton::clicked, this, [this]{
        auto* t = activeTab(); if (!t) return;
        if (t->isLoading()) t->stop(); else reloadPage();
    });
    m_homeBtn = makeToolBtn(QStringLiteral("Home")); connect(m_homeBtn, &QToolButton::clicked, this, &BrowserWindow::goHome);
    m_addressBar = new AddressBar(m_history, this);
    connect(m_addressBar, &AddressBar::returnPressed, this, &BrowserWindow::navigateToUrl);
    connect(m_addressBar, &AddressBar::starClicked, this, &BrowserWindow::toggleBookmark);
    connect(m_addressBar, &AddressBar::securityIconClicked, this, &BrowserWindow::showPageInfo);
    m_downloadsBtn = makeToolBtn(QStringLiteral("Downloads (Ctrl+J)")); connect(m_downloadsBtn, &QToolButton::clicked, this, [this]{ toggleDownloadsPopup(false); });
    m_downloadsBadge = new DownloadButtonBadge(m_downloadsBtn);
    m_downloadsPopup = new DownloadsPopup(this);
    connect(m_downloadsPopup, &DownloadsPopup::showAllRequested, this, &BrowserWindow::showDownloads);
    connect(m_downloadsPopup, &DownloadsPopup::openUrlRequested, this, [this](const QString& u){ newTab(u); });
    connect(m_downloadsPopup, &DownloadsPopup::settingsRequested, this, [this]{
        showSettings();
        for (int i = 0; i < m_tabs->count(); ++i) if (auto* sp = qobject_cast<SettingsPage*>(m_tabs->widget(i))) sp->showSection(QStringLiteral("Downloads"));
    });
    connect(m_downloads, &DownloadManager::listChanged, this, &BrowserWindow::updateDownloadsButton);
    connect(m_downloads, &DownloadManager::downloadStarted, this, &BrowserWindow::updateDownloadsButton);
    connect(m_downloads, &DownloadManager::downloadStarted, this, [this]{ if (isActiveWindow()) QTimer::singleShot(0, this, [this]{ flyToDownloads(); }); });
    // spec issue #6: puzzle "extensions" glyph instead of the unclear window icon
    m_splitBtn = makeToolBtn(QStringLiteral("Extensions")); connect(m_splitBtn, &QToolButton::clicked, this, &BrowserWindow::showExtensionsMenu);
    m_privateBadge = new QLabel(m_toolbar); m_privateBadge->setFixedSize(36,36); m_privateBadge->setAlignment(Qt::AlignCenter);
    m_privateBadge->setToolTip(QStringLiteral("Private browsing")); m_privateBadge->hide();
    m_profileBtn = makeToolBtn(QStringLiteral("Profile"));
    // 1.6 profiles: avatar of this window's profile with a ring in the profile colour (spec §2.2)
    m_profileAvatar = new QLabel(m_profileBtn); m_profileAvatar->setObjectName(QStringLiteral("ProfileAvatar"));
    m_profileAvatar->setAttribute(Qt::WA_TransparentForMouseEvents);
    refreshProfileAvatar();
    connect(m_profileBtn, &QToolButton::clicked, this, &BrowserWindow::showProfileSwitcher);
    connect(ProfileManager::instance(), &ProfileManager::profilesChanged, this, &BrowserWindow::refreshProfileAvatar);
    connect(ProfileManager::instance(), &ProfileManager::currentProfileChanged, this, [this]{ crossFadeTheme([this]{ loadSettings(); applyTheme(); refreshProfileAvatar(); }); });   // kit #34 accent cross-fade
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+M")), this, [this]{ showProfileManager(); });
    m_menuBtn = makeToolBtn(QStringLiteral("Customize and control Pacitum (Alt+F)")); connect(m_menuBtn, &QToolButton::clicked, this, &BrowserWindow::showMenu);

    lay->addWidget(m_backBtn); lay->addWidget(m_forwardBtn); lay->addWidget(m_reloadBtn); lay->addWidget(m_homeBtn);
    m_homeBtn->setVisible(readSettings().value(QStringLiteral("show_home_button")).toBool(false));   // off by default (spec §4.3)
    lay->addSpacing(6);
    lay->addWidget(m_addressBar, 1);
    lay->addSpacing(6);
    // 1.9.0: "Add to Pacitum" while a Chrome Web Store extension page is open
    m_storeBtn = new QPushButton(QStringLiteral("Add to Pacitum"), m_toolbar);
    m_storeBtn->setObjectName(QStringLiteral("StoreButton")); m_storeBtn->setCursor(Qt::PointingHandCursor);
    m_storeBtn->setToolTip(QStringLiteral("Install this extension from the Chrome Web Store"));
    m_storeBtn->hide();
    connect(m_storeBtn, &QPushButton::clicked, this, [this]{
        if (auto* t = activeTab()) ChromeExtensions::instance()->installFromWebStore(t->currentUrl(), t->currentTitle(), this);
    });
    lay->addWidget(m_storeBtn);
    lay->addSpacing(4);
    m_updatePill = new UpdatePill(m_toolbar);
    lay->addWidget(m_updatePill);
    m_extPins = new QWidget(m_toolbar);
    { auto* pl = new QHBoxLayout(m_extPins); pl->setContentsMargins(0, 0, 0, 0); pl->setSpacing(2); }
    lay->addWidget(m_downloadsBtn); lay->addWidget(m_extPins); lay->addWidget(m_splitBtn);
    connect(ChromeExtensions::instance(), &ChromeExtensions::changed, this, [this]{
        rebuildExtensionPins();
        if (auto* t = activeTab()) updateStoreButton(t->currentUrl());
    });
    connect(ChromeExtensions::instance(), &ChromeExtensions::message, this, [this](const QString& m){ if (isActiveWindow()) showNotice(m); });
    QTimer::singleShot(1200, this, [this]{ rebuildExtensionPins(); });   // after the profile has loaded its extensions
    lay->addWidget(m_privateBadge); lay->addWidget(m_profileBtn); lay->addWidget(m_menuBtn);
}

void BrowserWindow::refreshIcons() {
    m_reloadBtn->setProperty("pacitumLoading", QVariant());   // theme changed: re-render the reload icon too
    refreshProfileAvatar();
    auto* tm = m_theme;
    m_backBtn->setIcon(tm->icon(QStringLiteral("back")));
    m_forwardBtn->setIcon(tm->icon(QStringLiteral("forward")));
    auto* t = activeTab();
    {   // spec §2: reload ⇄ stop cross-fade (120 ms OutCubic, scale 0.8→1)
        const QIcon ic = tm->icon(t && t->isLoading() ? QStringLiteral("stop") : QStringLiteral("reload"));
        if (auto* b = qobject_cast<AnimatedToolButton*>(m_reloadBtn)) b->setIconAnimated(ic); else m_reloadBtn->setIcon(ic);
    }
    m_reloadBtn->setToolTip(t && t->isLoading() ? QStringLiteral("Stop loading") : QStringLiteral("Reload (F5)"));
    m_homeBtn->setIcon(tm->icon(QStringLiteral("home")));
    m_downloadsBtn->setIcon(tm->icon(QStringLiteral("download")));
    m_splitBtn->setIcon(tm->icon(QStringLiteral("extensions")));
    m_menuBtn->setIcon(tm->icon(QStringLiteral("menu")));
    m_privateBadge->setPixmap(tm->icon(QStringLiteral("private")).pixmap(QSize(18,18), devicePixelRatioF()));
}

void BrowserWindow::updateNavState() {
    auto* t = activeTab();
    m_backBtn->setEnabled(t && t->canGoBack());
    m_forwardBtn->setEnabled(t && t->canGoForward());
    // Only the reload/stop state depends on the tab. (This used to re-render every toolbar icon and
    // the profile avatar on each tab switch and each load start/finish.)
    const bool loading = t && t->isLoading();
    if (m_reloadBtn->property("pacitumLoading").isValid() && m_reloadBtn->property("pacitumLoading").toBool() == loading) return;
    m_reloadBtn->setProperty("pacitumLoading", loading);
    const QIcon ic = m_theme->icon(loading ? QStringLiteral("stop") : QStringLiteral("reload"));
    if (auto* b = qobject_cast<AnimatedToolButton*>(m_reloadBtn)) b->setIconAnimated(ic); else m_reloadBtn->setIcon(ic);
    m_reloadBtn->setToolTip(loading ? QStringLiteral("Stop loading") : QStringLiteral("Reload (F5)"));
}

// Letter tile used when a site has no favicon (spec §5.1: letter fallback with site colour).
static QIcon letterTile(const QString& url) {
    static QHash<QString, QIcon> cache;
    const QString host = QUrl(url).host().remove(QRegularExpression(QStringLiteral("^www\\.")));
    if (host.isEmpty()) return {};
    if (auto it = cache.constFind(host); it != cache.constEnd()) return *it;
    static const char* colors[] = {"#EA4335","#24292F","#41CD52","#FF0000","#0EA5E9","#8B5CF6","#F97316","#0A66C2","#1DB954","#64748B","#E11D48","#14B8A6"};
    const QColor bg(colors[qHash(host) % 12]);
    QPixmap pm(32, 32); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(bg); p.drawRoundedRect(QRectF(0, 0, 32, 32), 8, 8);
    QFont f = QApplication::font(); f.setPixelSize(19); f.setWeight(QFont::Bold); p.setFont(f);
    p.setPen(Qt::white); p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, host.left(1).toUpper());
    p.end(); pm.setDevicePixelRatio(2.0);
    QIcon ic(pm);
    if (cache.size() > 500) cache.clear();
    cache.insert(host, ic);
    return ic;
}

void BrowserWindow::updateTabIcon(BrowserTab* tab) {
    for (auto* tw : {m_tabs, m_splitTabs}) {
        int idx = tw->indexOf(tab);
        if (idx < 0) continue;
        const bool floating = tw->hasFloatingStrip();
        if (floating) tw->setTabLoading(idx, tab->isLoading());   // tab bar animates its own 16 px spinner
        if (tab->isLoading() && !floating) { if (!m_spinTimer->isActive()) m_spinTimer->start(); continue; }
        QIcon ic = tab->icon();
        if (ic.isNull()) {
            const QString u = tab->currentUrl();
            ic = u.startsWith(QStringLiteral("pacitum:")) || u.isEmpty()
                 ? []{ static const QIcon app(QStringLiteral(":/icons/pacitum-icon-32.png")); return app; }()
                 : letterTile(u);
            if (ic.isNull()) ic = m_theme->secondaryIcon(QStringLiteral("globe"), 16);
        }
        tw->setTabIcon(idx, ic);
    }
}

void BrowserWindow::tickSpinner() {
    if (isMinimized()) { m_spinTimer->stop(); return; }
    m_spinAngle = (m_spinAngle + 12) % 360;
    // 30 frames per accent colour, rendered once: the old code painted a new pixmap on every tick.
    static QHash<QString, QVector<QIcon>> frames;
    const QString accent = QColor(m_theme->palette().accent).name();
    auto& f = frames[accent];
    if (f.isEmpty()) { if (frames.size() > 4) { frames.clear(); frames[accent]; } f.resize(30); }
    QIcon& ic = frames[accent][m_spinAngle / 12];
    if (ic.isNull()) ic = Pacitum::spinnerIcon(m_spinAngle, m_theme->palette().accent);
    bool any = false;
    for (auto* tw : {m_tabs, m_splitTabs}) {
        if (tw->hasFloatingStrip()) continue;
        for (int i = 0; i < tw->count(); ++i)
            if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i)); t && t->isLoading()) { tw->setTabIcon(i, ic); any = true; }
    }
    if (!any) m_spinTimer->stop();
}

void BrowserWindow::updateTabAudio(BrowserTab* tab) {
    const int idx = m_tabs->indexOf(tab);
    if (idx < 0 || !tab->view()) return;
    m_tabs->setTabAudio(idx, tab->view()->page()->recentlyAudible(), tab->isMuted());
}

void BrowserWindow::rebuildExtensionPins() {
    if (!m_extPins) return;
    auto* l = m_extPins->layout();
    while (auto* it = l->takeAt(0)) { delete it->widget(); delete it; }
    auto* ce = ChromeExtensions::instance();
    for (const ChromeExtensionItem& e : ce->items()) {
        if (!ce->pinned(e.id) || !e.enabled) continue;
        auto* b = makeToolBtn(e.name);
        b->setIcon(e.icon.isNull() ? m_theme->secondaryIcon(QStringLiteral("puzzle"), 18) : e.icon);
        b->setIconSize(QSize(18, 18));
        const QString id = e.id;
        connect(b, &QToolButton::clicked, this, [b, id]{ ChromeExtensions::instance()->openPopup(id, b); });
        l->addWidget(b);
    }
    m_extPins->setVisible(l->count() > 0);
}

void BrowserWindow::updateStoreButton(const QString& url) {
    if (!m_storeBtn) return;
    const QString id = ChromeExtensions::webStoreId(QUrl(url));
    bool show = !id.isEmpty() && !m_privateMode;
    if (show) for (const auto& e : ChromeExtensions::instance()->items()) if (e.id == id) { show = false; break; }   // already added
    if (show) {
        const auto& p = m_theme->palette();
        m_storeBtn->setStyleSheet(QStringLiteral("QPushButton#StoreButton{background:%1;color:%2;border:none;border-radius:14px;padding:0 14px;min-height:28px;max-height:28px;font-weight:600;}"
                                                 "QPushButton#StoreButton:hover{background:%3;}").arg(p.accent, p.onAccent, p.accentHover));
        m_storeBtn->setText(ChromeExtensions::available() ? QStringLiteral("Add to Pacitum") : QStringLiteral("Needs Pacitum 1.9"));
    }
    if (show != m_storeBtn->isVisible()) {
        m_storeBtn->setVisible(show);
        if (show) Motion::fadeIn(m_storeBtn, Motion::Base);
    }
}

void BrowserWindow::showExtensionsMenu() {
    // 1.8.5: Pacitum mini extensions (user scripts / styles) + split view controls.
    // 1.9.0: Chrome extensions first (Qt 6.11 build); the legacy build says where to get them.
    Pacitum::PopupMenu m(this);
    auto* ce = ChromeExtensions::instance();
    {
        const auto chrome = ce->items();
        auto* ch = m.addAction(m_theme->secondaryIcon(QStringLiteral("puzzle"), 18),
                               !ChromeExtensions::available() ? QStringLiteral("Chrome extensions need Pacitum 1.9")
                               : chrome.isEmpty() ? QStringLiteral("No Chrome extensions yet") : QStringLiteral("Chrome extensions"));
        ch->setEnabled(false);
        for (const ChromeExtensionItem& e : chrome) {
            QString label = e.name;
            if (!e.enabled) label += QStringLiteral("  (off)");
            else if (!e.error.isEmpty()) label += QStringLiteral("  (error)");
            QMenu* sub = m.addMenu(e.icon, label.replace(QLatin1Char('&'), QStringLiteral("&&")));
            sub->setToolTipsVisible(true);
            if (!e.description.isEmpty()) { auto* d = sub->addAction(e.description.left(90)); d->setEnabled(false); }
            if (!e.error.isEmpty()) { auto* er = sub->addAction(QStringLiteral("Error: ") + e.error.left(80)); er->setEnabled(false); }
            for (const QString& perm : e.permissions) { auto* pa = sub->addAction(QStringLiteral("• ") + perm.left(90)); pa->setEnabled(false); }
            sub->addSeparator();
            const QString id = e.id, name = e.name;
            if (e.popupUrl.isValid() && !e.popupUrl.isEmpty())
                connect(sub->addAction(QStringLiteral("Open")), &QAction::triggered, this, [this, id]{ ChromeExtensions::instance()->openPopup(id, m_splitBtn); });
            if (e.optionsUrl.isValid())
                connect(sub->addAction(QStringLiteral("Options")), &QAction::triggered, this, [this, u = e.optionsUrl]{ newTab(u.toString()); });
            auto* pin = sub->addAction(QStringLiteral("Pin to toolbar")); pin->setCheckable(true); pin->setChecked(ce->pinned(id));
            // 1.9.1: these run once the menu has closed. Switching an extension on starts it inside
            // Chromium; doing that from inside the menu's own event loop froze Pacitum (1.9.0).
            connect(pin, &QAction::toggled, this, [this, id](bool v){ QTimer::singleShot(0, this, [id, v]{ ChromeExtensions::instance()->setPinned(id, v); }); });
            auto* on = sub->addAction(QStringLiteral("Enabled")); on->setCheckable(true); on->setChecked(e.enabled);
            connect(on, &QAction::toggled, this, [this, id](bool v){ QTimer::singleShot(0, this, [id, v]{ ChromeExtensions::instance()->setEnabled(id, v); }); });
            connect(sub->addAction(QStringLiteral("Remove…")), &QAction::triggered, this, [this, id, name]{
                QTimer::singleShot(0, this, [this, id, name]{
                    if (QMessageBox::question(this, QStringLiteral("Remove extension"), QStringLiteral("Remove \"%1\"?").arg(name)) == QMessageBox::Yes)
                        ChromeExtensions::instance()->uninstall(id);
                });
            });
        }
        if (ChromeExtensions::available()) {
            connect(m.addAction(QStringLiteral("Get extensions from the Chrome Web Store")), &QAction::triggered, this, [this]{ newTab(QStringLiteral("https://chromewebstore.google.com/")); });
            connect(m.addAction(QStringLiteral("Add a .crx or .zip file…")), &QAction::triggered, this, [this]{
                const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Add a Chrome extension"), QString(), QStringLiteral("Chrome extensions (*.crx *.zip)"));
                if (!f.isEmpty()) ChromeExtensions::instance()->installFromFile(f, this);
            });
            connect(m.addAction(QStringLiteral("Load unpacked extension…")), &QAction::triggered, this, [this]{
                const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose the extension folder (with manifest.json)"));
                if (!d.isEmpty()) ChromeExtensions::instance()->installFromFile(d, this);
            });
        }
        m.addSeparator();
    }
    auto* em = ExtensionManager::instance();
    auto* head = m.addAction(m_theme->secondaryIcon(QStringLiteral("extensions"), 18),
                             em->extensions().isEmpty() ? QStringLiteral("No user scripts") : QStringLiteral("User scripts and styles"));
    head->setEnabled(false);
    for (const PacitumExtension& e : em->extensions()) {
        QMenu* sub = m.addMenu(QStringLiteral("%1  %2").arg(e.name, e.enabled ? QString() : QStringLiteral("(off)")).trimmed().replace(QLatin1Char('&'), QStringLiteral("&&")));
        sub->setToolTipsVisible(true);
        auto* info = sub->addAction(QStringLiteral("Version %1 · runs on %2").arg(e.version, e.sites.join(QStringLiteral(", "))).left(90));
        info->setEnabled(false);
        if (!e.description.isEmpty()) { auto* d = sub->addAction(e.description.left(90)); d->setEnabled(false); }
        sub->addSeparator();
        auto* on = sub->addAction(QStringLiteral("Enabled")); on->setCheckable(true); on->setChecked(e.enabled);
        const QString id = e.id, name = e.name, dir = e.dir;
        connect(on, &QAction::toggled, this, [this, id](bool v){ ExtensionManager::instance()->setEnabled(id, v); if (auto* t = activeTab()) t->reload(); });
        connect(sub->addAction(QStringLiteral("Show folder")), &QAction::triggered, this, [dir]{ QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
        connect(sub->addAction(QStringLiteral("Remove…")), &QAction::triggered, this, [this, id, name]{
            if (QMessageBox::question(this, QStringLiteral("Remove extension"), QStringLiteral("Remove \"%1\"?").arg(name)) == QMessageBox::Yes) {
                ExtensionManager::instance()->remove(id);
                if (auto* t = activeTab()) t->reload();
            }
        });
    }
    m.addSeparator();
    connect(m.addAction(QStringLiteral("Load user-script folder…")), &QAction::triggered, this, [this]{ installExtensionFromFolder(); });
    connect(m.addAction(QStringLiteral("Reload extensions")), &QAction::triggered, this, [this]{
        ExtensionManager::instance()->reload(); if (auto* t = activeTab()) t->reload(); });
    connect(m.addAction(QStringLiteral("Open extensions folder")), &QAction::triggered, this, []{
        const QString f = ExtensionManager::instance()->folder(); QDir().mkpath(f); QDesktopServices::openUrl(QUrl::fromLocalFile(f)); });
    m.addSeparator();
    auto* split = m.addAction(m_theme->secondaryIcon(QStringLiteral("split-view"), 18),
                              m_splitActive ? QStringLiteral("Close split view\tCtrl+Alt+S") : QStringLiteral("Split view\tCtrl+Alt+S"));
    connect(split, &QAction::triggered, this, [this]{ toggleSplitView(m_splitActive ? -1 : m_tabs->currentIndex()); });
    if (m_splitActive) {
        connect(m.addAction(QStringLiteral("Swap sides\tCtrl+Alt+X")), &QAction::triggered, this, [this]{ swapSplitSides(); });
        connect(m.addAction(QStringLiteral("Make sides equal")), &QAction::triggered, this, [this]{ m_splitter->equalize(); });
    }
    const QSize sz = m.sizeHint();
    QPoint anchor = m_splitBtn->mapToGlobal(QPoint(m_splitBtn->width(), m_splitBtn->height() + 6));
    m.exec(QPoint(anchor.x() - sz.width(), anchor.y()));
}

// ---------------------------------------------------------------- 1.8.8 Google Drive sync
void BrowserWindow::setupSync() {
    auto* sm = SyncManager::instance();
    if (!sm->history()) sm->setHistory(m_history);
    connect(sm, &SyncManager::aboutToCollect, this, [this]{ if (m_notes) m_notes->flush(); m_history->flush(); });
    connect(sm, &SyncManager::applied, this, [this](const QStringList& kinds){
        if (kinds.contains(QStringLiteral("bookmarks"))) {
            m_bookmarks->load();
            if (m_bookmarksBar) m_bookmarksBar->refresh();
            if (auto* t = activeTab()) m_addressBar->setBookmarked(m_bookmarks->isBookmarked(t->currentUrl()));
        }
        if (kinds.contains(QStringLiteral("settings"))) { loadSettings(); applyTheme(); }
        if (kinds.contains(QStringLiteral("notes")) && m_notes) m_notes->reload();
    });
}

// ---------------------------------------------------------------- 1.8.5 tools + 3D overview
void BrowserWindow::showNotice(const QString& text) {
    if (text.isEmpty()) return;
    const int top = (m_chromeHidden || !m_toolbar) ? 16 : m_toolbar->mapTo(this, QPoint(0, m_toolbar->height())).y() + 12;
    Motion::bubble(this, text, 1600, top);
}

void BrowserWindow::toggleNotes() {
    if (!m_notes) {
        m_notes = new NotesPanel(m_contentRow);
        static_cast<QHBoxLayout*>(m_contentRow->layout())->addWidget(m_notes);
        connect(m_notes, &NotesPanel::closeRequested, this, [this]{ toggleNotes(); });
        m_notes->hide();
    }
    if (m_notes->isVisible()) { m_notes->flush(); m_notes->hide(); return; }
    m_notes->show();
    Motion::fadeIn(m_notes, Motion::Base);   // the panel fades in; the page is resized once, not animated
    m_notes->focusEditor();
}

void BrowserWindow::takeScreenshot() {
    auto* t = activeTab();
    if (!t) return;
    const QPixmap shot = t->captureView();
    if (shot.isNull()) { showNotice(QStringLiteral("Nothing to capture")); return; }
    auto* o = new ScreenshotOverlay(shot, this);
    o->setGeometry(t->viewRectIn(this));
    connect(o, &ScreenshotOverlay::finished, this, &BrowserWindow::showNotice);
    o->show(); o->raise(); o->setFocus();
}

void BrowserWindow::showPageQr() {
    auto* t = activeTab();
    const QString url = t ? t->currentUrl() : QString();
    if (url.isEmpty() || url.startsWith(QStringLiteral("pacitum:"))) { showNotice(QStringLiteral("Open a website to make a QR code")); return; }
    auto* d = new QrDialog(url, this);
    d->open();
}

void BrowserWindow::togglePictureInPicture() {
    if (auto* t = activeTab()) t->togglePictureInPicture();
}

void BrowserWindow::captureActiveThumb() {
    auto* t = activeTab();
    if (!t || !t->isVisible() || t->inPictureInPicture()) return;
    QPixmap pm = t->captureView();
    if (pm.isNull()) return;
    if (pm.width() > 640) pm = pm.scaledToWidth(640, Qt::SmoothTransformation);   // small cache = fast 3D frames
    m_thumbs.insert(t, pm);
}

void BrowserWindow::showTabOverview() {
    if (findChild<TabOverview*>(QString(), Qt::FindDirectChildrenOnly)) return;   // already open
    if (m_tabs->count() < 2) { showNotice(QStringLiteral("Open more tabs to use the overview")); return; }
    captureActiveThumb();
    QList<TabOverview::Card> cards;
    for (int i = 0; i < m_tabs->count(); ++i) {
        QWidget* w = m_tabs->widget(i);
        TabOverview::Card c;
        c.title = m_tabs->tabText(i);
        c.icon = m_tabs->tabIcon(i);
        c.shot = m_thumbs.value(w);
        if (c.shot.isNull() && w && w->isVisible() && !qobject_cast<BrowserTab*>(w)) c.shot = w->grab();   // settings / downloads page
        cards << c;
    }
    // Remember the tabs themselves: one may close while the overview is open, shifting the indexes.
    QList<QPointer<QWidget>> widgets;
    for (int i = 0; i < m_tabs->count(); ++i) widgets << m_tabs->widget(i);
    auto* ov = new TabOverview(cards, m_tabs->currentIndex(), this);
    ov->setObjectName(QStringLiteral("PacitumWindowOverlay"));   // resizeEvent keeps it at the window size
    connect(ov, &TabOverview::chosen, this, [this, widgets](int i){
        if (i < 0 || i >= widgets.size() || !widgets[i]) return;
        const int at = m_tabs->indexOf(widgets[i]);
        if (at >= 0) m_tabs->setCurrentIndex(at);
    });
    ov->open();
}

void BrowserWindow::installExtensionFromFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose an extension folder (with manifest.json)"));
    if (dir.isEmpty()) return;
    auto* em = ExtensionManager::instance();
    PacitumExtension e; QString err;
    if (!em->readManifest(dir, &e, &err)) { QMessageBox::warning(this, QStringLiteral("Can't load extension"), err); return; }
    // Extensions run their code on matching pages: say where before installing.
    const auto answer = QMessageBox::question(this, QStringLiteral("Add extension"),
        QStringLiteral("Add \"%1\" %2?\n\nIt can read and change pages on:\n%3")
            .arg(e.name, e.version, e.sites.join(QLatin1Char('\n'))));
    if (answer != QMessageBox::Yes) return;
    if (em->install(dir, &err).isEmpty()) { QMessageBox::warning(this, QStringLiteral("Can't load extension"), err); return; }
    if (auto* t = activeTab()) t->reload();
}

// spec §4.4 step 3: when the toolbar gets too narrow for a 240 px omnibox, collapse
// extensions -> downloads -> profile (they stay reachable from the ⋮ menu).
void BrowserWindow::updateToolbarOverflow() {
    if (!m_toolbar) return;
    const int w = m_toolbar->width();
    const int fixed = 16 + 38 * 3 + (m_homeBtn->isVisibleTo(m_toolbar) ? 38 : 0) + 6 * 2 + 36 /* ⋮ */ + 2 * 6
                      + (m_privateBadge->isVisibleTo(m_toolbar) ? 38 : 0);
    int room = w - fixed - 240;
    const bool wantDownloads = m_downloadsWanted;
    auto fits = [&room](bool want){ if (!want) return false; if (room >= 38) { room -= 38; return true; } return false; };
    // reserve in priority order (profile kept longest, extensions dropped first)
    const bool showProfile = fits(true);
    const bool showDownloads = fits(wantDownloads);
    const bool showExt = fits(true);
    m_profileBtn->setVisible(showProfile);
    m_downloadsBtn->setVisible(showDownloads);
    m_splitBtn->setVisible(showExt);
}

void BrowserWindow::setupShortcuts() {
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Tab")), this, SLOT(nextTab()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), this, SLOT(prevTab()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this, SLOT(focusAddressBar()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+F")), this, SLOT(showFind()));
    new QShortcut(QKeySequence(QStringLiteral("F5")), this, SLOT(reloadPage()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+H")), this, SLOT(showHistory()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+J")), this, SLOT(showDownloads()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this, SLOT(toggleBookmark()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")), this, SLOT(toggleBookmarksBar()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")), this, SLOT(toggleVerticalTabs()));
    new QShortcut(QKeySequence(Qt::Key_Escape), this, SLOT(handleEscape()));
    // §10: F6 / Alt+Shift+A focus the pending permission prompt (F6 falls back to the omnibox).
    new QShortcut(QKeySequence(QStringLiteral("Alt+Shift+A")), this, [this]{ if (auto* t = activeTab(); t && t->permissions()) t->permissions()->focusPending(); });
    new QShortcut(QKeySequence(Qt::Key_F6), this, [this]{
        if (auto* t = activeTab(); t && t->permissions() && t->permissions()->hasPending()) t->permissions()->focusPending(); else focusAddressBar(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+T")), this, [this]{ newTab(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+W")), this, [this]{ closeTab(m_tabs->currentIndex()); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+D")), this, SLOT(toggleBookmark()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")), this, SLOT(toggleBookmarksBar()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")), this, SLOT(newPrivateTab()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")), this, SLOT(reopenClosedTab()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+=")), this, SLOT(zoomIn()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+-")), this, SLOT(zoomOut()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+0")), this, SLOT(zoomReset()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), this, SLOT(showSettings()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+P")), this, [this]{ if (auto* t = activeTab()) t->findChild<QWebEngineView*>()->printToPdf(QDir::homePath() + QStringLiteral("/page.pdf")); });
    new QShortcut(QKeySequence(QStringLiteral("F11")), this, SLOT(toggleFullscreen()));
    new QShortcut(QKeySequence(QStringLiteral("Alt+F")), this, SLOT(showMenu()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")), this, [this]{ m_tabs->showTabSearch(); });
    new QShortcut(QKeySequence(QStringLiteral("F12")), this, SLOT(showDevTools()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")), this, SLOT(toggleReaderMode()));
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")), this, [this]{ toggleSplitView(m_splitActive ? -1 : m_tabs->currentIndex()); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+X")), this, [this]{ swapSplitSides(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")), this, [this]{ takeScreenshot(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Y")), this, [this]{ toggleNotes(); });
    new QShortcut(QKeySequence(QStringLiteral("Alt+P")), this, [this]{ togglePictureInPicture(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Space")), this, [this]{ showTabOverview(); });
    new QShortcut(QKeySequence(QStringLiteral("Alt+Left")), this, SLOT(goBack()));
    new QShortcut(QKeySequence(QStringLiteral("Alt+Right")), this, SLOT(goForward()));
    for (int i = 1; i <= 9; ++i)
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i)), this, [this, i]{
            if (i == 9) lastTab(); else if (i - 1 < m_tabs->count()) m_tabs->setCurrentIndex(i - 1); });
}

void BrowserWindow::createMenuBar() {
    auto* mb = menuBar();
    auto* file = mb->addMenu(QStringLiteral("File"));
    file->addAction(QStringLiteral("New Tab"), QKeySequence::AddTab, this, [this]{ newTab(); });
    file->addAction(QStringLiteral("New Private Tab"), QKeySequence(QStringLiteral("Ctrl+Shift+N")), this, &BrowserWindow::newPrivateTab);
    file->addAction(QStringLiteral("Reopen Closed Tab"), QKeySequence(QStringLiteral("Ctrl+Shift+T")), this, &BrowserWindow::reopenClosedTab);
    file->addSeparator();
    file->addAction(QStringLiteral("Close Tab"), QKeySequence::Close, this, [this]{ closeTab(m_tabs->currentIndex()); });
    file->addSeparator();
    file->addAction(QStringLiteral("Save Session As..."), this, &BrowserWindow::saveSessionDialog);
    file->addAction(QStringLiteral("Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = mb->addMenu(QStringLiteral("Edit"));
    edit->addAction(QStringLiteral("Find in Page"), QKeySequence::Find, this, &BrowserWindow::showFind);
    edit->addAction(QStringLiteral("Zoom In"), QKeySequence(QStringLiteral("Ctrl+=")), this, &BrowserWindow::zoomIn);
    edit->addAction(QStringLiteral("Zoom Out"), QKeySequence(QStringLiteral("Ctrl+-")), this, &BrowserWindow::zoomOut);
    edit->addAction(QStringLiteral("Zoom Reset"), QKeySequence(QStringLiteral("Ctrl+0")), this, &BrowserWindow::zoomReset);

    auto* view = mb->addMenu(QStringLiteral("View"));
    view->addAction(QStringLiteral("Toggle Dark Mode"), this, &BrowserWindow::toggleDarkMode);
    view->addAction(QStringLiteral("Toggle Bookmarks Bar"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), this, &BrowserWindow::toggleBookmarksBar);
    view->addAction(QStringLiteral("Toggle Vertical Tabs"), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this, &BrowserWindow::toggleVerticalTabs);
    view->addSeparator();
    view->addAction(QStringLiteral("Full Screen"), QKeySequence(QStringLiteral("F11")), this, &BrowserWindow::toggleFullscreen);
    view->addAction(QStringLiteral("Reader mode"), QKeySequence(QStringLiteral("Ctrl+Shift+R")), this, &BrowserWindow::toggleReaderMode);

    auto* hist = mb->addMenu(QStringLiteral("History"));
    hist->addAction(QStringLiteral("Show History"), QKeySequence(QStringLiteral("Ctrl+H")), this, &BrowserWindow::showHistory);
    hist->addAction(QStringLiteral("Clear History"), this, &BrowserWindow::clearHistory);

    auto* bm = mb->addMenu(QStringLiteral("Bookmarks"));
    bm->addAction(QStringLiteral("Add Bookmark"), QKeySequence(QStringLiteral("Ctrl+B")), this, &BrowserWindow::toggleBookmark);
    bm->addAction(QStringLiteral("Show All Bookmarks"), this, &BrowserWindow::showBookmarksDialog);
    bm->addAction(QStringLiteral("Import Bookmarks from HTML…"), this, &BrowserWindow::importBookmarks);

    auto* win = mb->addMenu(QStringLiteral("Window"));
    win->addAction(QStringLiteral("Downloads"), QKeySequence(QStringLiteral("Ctrl+J")), this, &BrowserWindow::showDownloads);
    win->addAction(QStringLiteral("Split View"), this, [this]{ toggleSplitView(); });

    auto* set = mb->addMenu(QStringLiteral("Settings"));
    set->addAction(QStringLiteral("Preferences"), QKeySequence(QStringLiteral("Ctrl+," )), this, &BrowserWindow::showSettings);
}

void BrowserWindow::createStatusBar() {
    statusBar()->showMessage(QStringLiteral("Ready"));
    m_zoomIndicator = new QLabel(QStringLiteral("100%"), this);
    statusBar()->addPermanentWidget(m_zoomIndicator);
}

// ---- Tab management ----
void BrowserWindow::newTab(const QString& url) {
    const bool lazy = url == QLatin1String("pacitum-lazy:");   // session restore placeholder: no load yet
    QString u = lazy ? QString() : url.isEmpty() ? PacitumSchemeHandler::newTabUrl() : url;
    auto* tab = new BrowserTab(u, m_privateMode, m_searchEngine, this);
    tab->setZoomFactor(m_zoom->zoom());
    // Only the active tab may drive the address bar (background tabs that redirect or finish
    // loading used to overwrite it with their URL, e.g. "welcome" or a stale page).
    connect(tab, &BrowserTab::urlChanged, this, [this, tab](const QString& u){
        if (!tab->isPrivate() && !tab->isLazy() && !u.startsWith(QStringLiteral("pacitum:"))) m_history->addVisit(u, tab->currentTitle());
        if (tab == activeTab()) updateAddressBar(u);
        scheduleSessionSave();
    });
    connect(tab, &BrowserTab::titleChanged, this, &BrowserWindow::updateTabTitle);
    connect(tab, &BrowserTab::openLinkRequested, this, &BrowserWindow::openLink);
    connect(tab, &BrowserTab::notice, this, &BrowserWindow::showNotice);
    connect(tab, &QObject::destroyed, this, [this, tab]{ m_thumbs.remove(tab); });
    connect(tab, &BrowserTab::titleChanged, this, [this, tab](const QString& t){ if (!tab->isPrivate() && !tab->isLazy()) m_history->updateTitle(tab->currentUrl(), t); });
    connect(tab, &BrowserTab::loadingChanged, this, &BrowserWindow::onTabLoading);
    // omnibox progress follows the active tab (animations-spec §7)
    connect(tab, &BrowserTab::loadingChanged, this, [this, tab](bool on){ if (tab == activeTab() && on) m_addressBar->progressStarted(); });
    connect(tab, &BrowserTab::progressChanged, this, [this, tab](int p){
        if (tab == activeTab()) m_addressBar->progressChanged(p);
        for (auto* tw : {m_tabs, m_splitTabs}) if (int i = tw->indexOf(tab); i >= 0) tw->setTabWaiting(i, tab->isLoading() && p <= 10);
    });
    connect(tab, &BrowserTab::loadDone, this, [this, tab](bool ok){ if (tab == activeTab()) m_addressBar->progressFinished(ok); });
    connect(tab, &BrowserTab::iconChanged, this, [this, tab]{ updateTabIcon(tab); });
    connect(tab, &BrowserTab::urlChanged, this, [this, tab]{ updateTabIcon(tab); });
    connect(tab, &BrowserTab::fullScreenRequested, this, [this, tab](QWebEngineFullScreenRequest req){
        if (req.toggleOn()) {
            // Only the tab the user is looking at may take the screen: requests from background
            // tabs, closing tabs or a minimized window used to hide the chrome for the wrong tab.
            if (tab != activeTab() || m_closing.contains(tab) || isMinimized() || !isVisible()) { req.reject(); return; }
            req.accept();
            m_fullscreenTab = tab;
            m_pageFullscreen = true;
            tab->setNoFreeze(true);
            enterContentFullscreen(true);
        } else {
            req.accept();
            if (m_pageFullscreen && m_fullscreenTab == tab) finishExitFullscreen();
        }
    });
    connect(tab, &BrowserTab::crashed, this, [this, tab]{ const int i = m_tabs->indexOf(tab); if (i >= 0) m_tabs->setTabCrashed(i, true); });
    wirePermissions(tab);
    if (tab->view()) {
        connect(tab->view()->page(), &QWebEnginePage::recentlyAudibleChanged, this, [this, tab]{ updateTabAudio(tab); });
        connect(tab->view()->page(), &QWebEnginePage::audioMutedChanged, this, [this, tab]{ updateTabAudio(tab); });
    }
    // choose target
    PacitumTabWidget* target = m_splitActive && m_splitTabs->isVisible() && m_splitTabs->count()==0 ? m_splitTabs : m_tabs;
    // if split active and caller wants split, use split; otherwise default to main
    // For regular newTab, use main tabs
    target = m_tabs;
    int idx = target->addTab(tab, QStringLiteral("New Tab"));
    target->setCurrentIndex(idx);
    tab->applyWebSettings();
    updateTabIcon(tab);
    if (url.isEmpty()) QTimer::singleShot(0, this, [this]{ m_addressBar->focusUrl(); });
}

void BrowserWindow::newPrivateTab() {
    m_privateMode = true; newTab(); m_privateMode = false;
    int idx = m_tabs->currentIndex();
    m_tabs->setTabText(idx, QStringLiteral("Private — ") + m_tabs->tabText(idx));
    m_privateBadge->show();
}

void BrowserWindow::openLink(const QUrl& url, int target) {
    if (!url.isValid()) return;
    const QString u = url.toString();
    switch (target) {
    case BrowserTab::NewWindow: {
        auto* w = new BrowserWindow(); w->setAttribute(Qt::WA_DeleteOnClose); w->show();
        QPointer<BrowserWindow> guard(w);
        QTimer::singleShot(0, w, [guard, u]{ if (guard) guard->navigateToUrl(u); });
        break;
    }
    case BrowserTab::PrivateTab: {
        m_privateMode = true; newTab(u); m_privateMode = false;
        const int idx = m_tabs->currentIndex();
        m_tabs->setTabText(idx, QStringLiteral("Private — ") + m_tabs->tabText(idx));
        m_privateBadge->show();
        break;
    }
    case BrowserTab::SplitView:
        newTab(u);
        moveTabToSplit(m_tabs->currentIndex());
        break;
    default:
        newTab(u);
        break;
    }
}

void BrowserWindow::closeTab(int index) {
    if (index < 0 || index >= m_tabs->count()) return;
    QWidget* w = m_tabs->widget(index);
    if (m_closing.contains(w)) return;
    if (m_tabs->count() == 1 && !m_splitActive) { close(); return; }
    m_closing.insert(w);
    if (m_chromeHidden && m_pageFullscreen && m_fullscreenTab == w) finishExitFullscreen();   // closing the fullscreen tab
    if (auto* bt = qobject_cast<BrowserTab*>(w); bt && bt->currentUrl().startsWith(QStringLiteral("pacitum://welcome")))
        endWelcome();   // closing the welcome tab = "Skip all" (defaults kept, flag set)
    QPointer<QWidget> guard(w);
    m_tabs->animateRemove(index, [this, guard, w]{
        m_closing.remove(w);
        if (!guard) return;
        const int idx = m_tabs->indexOf(guard);
        if (idx < 0) return;
        if (auto* t = qobject_cast<BrowserTab*>(guard.data())) {
            m_recentlyClosed.push_back({t->currentUrl(), t->currentTitle()});
            if (m_recentlyClosed.size() > 20) m_recentlyClosed.pop_front();
        }
        scheduleSessionSave();
        if (m_tabs->count() > 1) { m_tabs->removeTab(idx); guard->deleteLater(); }
        else if (m_splitActive) { m_tabs->removeTab(idx); guard->deleteLater(); toggleSplitView(); }
        else close();
    });
}

void BrowserWindow::closeSplitTab(int index) {
    if (index < 0 || index >= m_splitTabs->count()) return;
    QWidget* w = m_splitTabs->widget(index);
    m_splitTabs->removeTab(index);
    if (w) w->deleteLater();   // a closed right-side tab really closes (it used to keep running)
    if (m_splitTabs->count() == 0) closeSplit();
}

void BrowserWindow::onTabChanged(int index) {
    auto* sender = qobject_cast<PacitumTabWidget*>(QObject::sender());
    if (!sender || index < 0) return;
    if (m_restoring) { m_activeWidget = sender; return; }   // session restore: done once at the end
    tabActivated(sender, index);
}

void BrowserWindow::tabActivated(PacitumTabWidget* sender, int index) {
    m_activeWidget = sender;
    if (m_splitter) m_splitter->setActivePane(sender);
    m_thumbTimer.start();   // refresh this tab's 3D-overview card once it has painted
    QWidget* w = sender->widget(index);
    // Switching tabs while a page is fullscreen (Ctrl+Tab, a popup opening a tab): leave fullscreen
    // so the new tab is never shown without chrome.
    if (m_pageFullscreen && m_fullscreenTab && w != m_fullscreenTab && sender == m_tabs) finishExitFullscreen();
    for (auto* tw : {m_tabs, m_splitTabs}) if (tw) for (int i = 0; i < tw->count(); ++i)   // bubble → chip on tab switch (§1)
        if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i)); t && t != w && t->permissions()) t->permissions()->collapse();
    if (auto* tab = qobject_cast<BrowserTab*>(w)) {
        updateAddressBar(tab->currentUrl());
        refreshPermissionUi(tab);
        if (tab->permissions()) QTimer::singleShot(0, tab->permissions(), [p = QPointer<PermissionController>(tab->permissions())]{ if (p) p->tabActivated(); });
        m_addressBar->syncProgress(tab->isLoading(), tab->loadProgressValue());
        if (m_animations) tab->fadeIn();
        setWindowTitle(tab->currentTitle() + QStringLiteral(" — Pacitum Browser"));
    } else if (qobject_cast<DownloadsPage*>(w)) {
        m_addressBar->setPermissionChips({});
        m_addressBar->setUrl(QStringLiteral("pacitum://downloads"));
        m_addressBar->setBookmarked(false);
        setWindowTitle(QStringLiteral("Downloads — Pacitum Browser"));
    } else if (qobject_cast<SettingsPage*>(w)) {
        m_addressBar->setPermissionChips({});
        m_addressBar->setUrl(QStringLiteral("pacitum://settings"));
        m_addressBar->setBookmarked(false);
        setWindowTitle(QStringLiteral("Settings — Pacitum Browser"));
    }
    updateNavState();
}

BrowserTab* BrowserWindow::activeTab() const {
    if (m_activeWidget && m_activeWidget->currentWidget()) return qobject_cast<BrowserTab*>(m_activeWidget->currentWidget());
    return qobject_cast<BrowserTab*>(m_tabs->currentWidget());
}

void BrowserWindow::navigateToUrl(const QString& url) {
    if (url.startsWith(QStringLiteral("pacitum://settings"))) { showSettings(); return; }
    if (url.startsWith(QStringLiteral("pacitum://downloads"))) { showDownloads(); return; }
    if (auto* t = activeTab()) { t->loadUrl(url); m_history->addVisit(url, {}, true); }
    else newTab(url);
}

void BrowserWindow::updateAddressBar(const QString& url) {
    m_addressBar->setUrl(url);
    updateStoreButton(url);
    // (History is recorded per tab in newTab(); switching tabs is not a visit.)
    // bookmark indicator
    m_addressBar->setBookmarked(m_bookmarks->isBookmarked(url));
}

void BrowserWindow::updateTabTitle(const QString& title) {
    auto* sender = qobject_cast<BrowserTab*>(QObject::sender());
    if (!sender) return;
    for (auto* tw : {m_tabs, m_splitTabs}) {
        int idx = tw->indexOf(sender);
        if (idx >= 0) {   // full title: the floating tab fades the overflow instead of "…"
            tw->setTabText(idx, title); tw->setTabToolTip(idx, title);
            if (sender == activeTab()) setWindowTitle(title + QStringLiteral(" — Pacitum Browser"));
            break;
        }
    }
}

void BrowserWindow::onTabLoading(bool) {
    auto* tab = qobject_cast<BrowserTab*>(sender());
    if (!tab) return;
    updateTabIcon(tab);           // starts the spinner or restores the favicon
    if (tab != activeTab()) return;
    updateNavState();
}

void BrowserWindow::goBack() { if (auto* t=activeTab()) { if (t->canGoBack()) { if (auto* b = qobject_cast<AnimatedToolButton*>(m_backBtn)) b->nudge(-3); t->navTransition(-1); } t->goBack(); QTimer::singleShot(200, this, [this]{ if(auto* tt=activeTab()){ m_backBtn->setEnabled(tt->canGoBack()); }}); } }
void BrowserWindow::goForward() { if (auto* t=activeTab()) { if (t->canGoForward()) { if (auto* b = qobject_cast<AnimatedToolButton*>(m_forwardBtn)) b->nudge(3); t->navTransition(1); } t->goForward(); QTimer::singleShot(200, this, [this]{ if(auto* tt=activeTab()){ m_forwardBtn->setEnabled(tt->canGoForward()); }}); } }
void BrowserWindow::reloadPage() {
    if (auto* t = activeTab()) {
        if (auto* b = qobject_cast<AnimatedToolButton*>(m_reloadBtn)) b->spin();   // animations-spec §2 (click or F5)
        t->reload();
    }
}
void BrowserWindow::goHome() { navigateToUrl(readSettings().value(QStringLiteral("homepage")).toString(PacitumSchemeHandler::newTabUrl())); }

void BrowserWindow::toggleBookmark() {
    auto* t = activeTab(); if (!t) return;
    QString url = t->currentUrl(), title = t->currentTitle();
    if (url.isEmpty()) return;
    if (m_bookmarks->isBookmarked(url)) { m_bookmarks->removeBookmark(url); m_addressBar->setBookmarked(false, true); statusBar()->showMessage(QStringLiteral("Bookmark removed"), 2000); }
    else {
        m_bookmarks->addBookmark(title, url); m_addressBar->setBookmarked(true, true);
        m_bookmarksBar->refresh();
        // 1.9.0 / 1.8.11: pick a folder right away (Chrome's "Bookmark added" bubble)
        if (!m_bookmarks->folders().isEmpty() || isActiveWindow()) {
            QPointer<BrowserWindow> self(this);
            QTimer::singleShot(Motion::enabled() ? 380 : 0, this, [self, url]{   // after the star's pop
                if (!self) return;
                Pacitum::PopupMenu m(self);
                auto* head = m.addAction(self->m_theme->icon(QStringLiteral("star-filled"), 16), QStringLiteral("Bookmark added. Save in:")); head->setEnabled(false);
                auto* bar = m.addAction(QStringLiteral("Bookmarks bar")); bar->setCheckable(true); bar->setChecked(true);
                QObject::connect(bar, &QAction::triggered, self, [self, url]{ self->m_bookmarks->moveToFolder(url, QString()); self->m_bookmarksBar->refresh(); });
                for (const QString& f : self->m_bookmarks->folders()) {
                    auto* a = m.addAction(self->m_theme->secondaryIcon(QStringLiteral("folder"), 16), QString(f).replace(QLatin1Char('&'), QStringLiteral("&&")));
                    QObject::connect(a, &QAction::triggered, self, [self, url, f]{ self->m_bookmarks->moveToFolder(url, f); self->m_bookmarksBar->refresh(); self->showNotice(QStringLiteral("Saved in %1").arg(f)); });
                }
                m.addSeparator();
                QObject::connect(m.addAction(self->m_theme->secondaryIcon(QStringLiteral("folder-add"), 16), QStringLiteral("New folder…")), &QAction::triggered, self, [self, url]{
                    bool ok = false;
                    const QString name = QInputDialog::getText(self, QStringLiteral("New folder"), QStringLiteral("Folder name"), QLineEdit::Normal, QString(), &ok).trimmed();
                    if (!ok || name.isEmpty()) return;
                    self->m_bookmarks->moveToFolder(url, name); self->m_bookmarksBar->refresh(); self->showNotice(QStringLiteral("Saved in %1").arg(name));
                });
                QObject::connect(m.addAction(QStringLiteral("Remove")), &QAction::triggered, self, [self, url]{
                    self->m_bookmarks->removeBookmark(url); self->m_addressBar->setBookmarked(false, true); self->m_bookmarksBar->refresh(); });
                const QPoint at = self->m_addressBar->mapToGlobal(QPoint(self->m_addressBar->width() - 260, self->m_addressBar->height() + 6));
                m.exec(at);
            });
        }
        return;
    }
    m_bookmarksBar->refresh();
}

void BrowserWindow::refreshProfileAvatar() {
    if (!m_profileAvatar) return;
    paintProfileAvatarLabel(m_profileAvatar, 24, devicePixelRatioF());
    m_profileAvatar->move((m_profileBtn->width() - m_profileAvatar->width()) / 2, (m_profileBtn->height() - m_profileAvatar->height()) / 2);
    m_profileBtn->setToolTip(QStringLiteral("%1 — switch profile").arg(ProfileManager::instance()->current().name));
}

void BrowserWindow::showProfileSwitcher() {
    auto* sw = new ProfileSwitcher(this);
    connect(sw, &ProfileSwitcher::settingsRequested, this, &BrowserWindow::showSettings);
    connect(sw, &ProfileSwitcher::manageRequested, this, &BrowserWindow::showProfileManager);
    connect(sw, &QObject::destroyed, m_profileBtn, [b = QPointer<QToolButton>(m_profileBtn)]{ if (b) b->setDown(false); });
    m_profileBtn->setDown(true);
    sw->showUnder(m_profileBtn);
}

void BrowserWindow::showProfileManager() {
    auto* picker = new ProfilePicker(ProfilePicker::Manage, nullptr);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &ProfilePicker::chosen, picker, [picker](const QString& id){
        if (id == QStringLiteral("guest")) ProfileManager::instance()->launchGuest();
        else ProfileManager::instance()->launch(id);
        picker->close();
    });
    picker->show(); picker->raise(); picker->activateWindow();
}

void BrowserWindow::showDownloads() {
    if (m_downloadsPopup) m_downloadsPopup->hide();
    for (int i = 0; i < m_tabs->count(); ++i)
        if (qobject_cast<DownloadsPage*>(m_tabs->widget(i))) { m_tabs->setCurrentIndex(i); return; }
    auto* page = new DownloadsPage(this);
    connect(page, &DownloadsPage::openUrlRequested, this, [this](const QString& u){ newTab(u); });
    const int idx = m_tabs->addTab(page, QStringLiteral("Downloads"));
    m_tabs->setTabIcon(idx, downloadsTabIcon());
    m_tabs->setCurrentIndex(idx);
}

QIcon BrowserWindow::downloadsTabIcon() const {
    // favicon: `download` glyph on an accent tile (design-spec §9)
    const auto& p = m_theme->palette();
    QPixmap pm(32, 32); pm.fill(Qt::transparent);
    QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing); g.setPen(Qt::NoPen); g.setBrush(QColor(p.accent));
    g.drawRoundedRect(QRectF(0, 0, 32, 32), 8, 8);
    g.drawPixmap(QRect(5, 5, 22, 22), ThemeManager::icon(QStringLiteral("download"), p.onAccent, 22).pixmap(QSize(44, 44)));
    g.end();
    return QIcon(pm);
}

// Download start (kit #19): a small accent disc with the download glyph flies from the middle of the
// page to the downloads button (280 ms InOutCubic, shrinking and fading at the end), then the button
// bumps (160 ms). A tiny click-through child that lives 280 ms. Reduced motion: no flight, no bump.
void BrowserWindow::flyToDownloads() {
    if (!Motion::enabled() || !m_downloadsBtn || !m_downloadsBtn->isVisible() || m_chromeHidden) return;
    const QPoint end = m_downloadsBtn->mapTo(this, m_downloadsBtn->rect().center());
    QWidget* content = m_tabs ? static_cast<QWidget*>(m_tabs) : centralWidget();
    const QPoint start = content->mapTo(this, content->rect().center());
    auto* fly = new QLabel(this);
    fly->setAttribute(Qt::WA_TransparentForMouseEvents);
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(qRound(32 * dpr), qRound(32 * dpr)); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    { QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing); g.setPen(Qt::NoPen); g.setBrush(QColor(m_theme->palette().accent)); g.drawEllipse(QRectF(0, 0, 32, 32));
      g.drawPixmap(QPointF(7, 7), ThemeManager::icon(QStringLiteral("download"), m_theme->palette().onAccent, 18).pixmap(QSize(18, 18), dpr)); }
    fly->setPixmap(pm); fly->setScaledContents(true);
    fly->setGeometry(start.x() - 16, start.y() - 16, 32, 32); fly->show(); fly->raise();
    auto* eff = new QGraphicsOpacityEffect(fly); fly->setGraphicsEffect(eff);
    auto* g = new QParallelAnimationGroup(fly);
    auto* geo = new QPropertyAnimation(fly, "geometry"); geo->setDuration(Motion::Slow); geo->setEasingCurve(QEasingCurve::InOutCubic);
    geo->setStartValue(fly->geometry()); geo->setEndValue(QRect(end.x() - 7, end.y() - 7, 14, 14)); g->addAnimation(geo);
    auto* op = new QPropertyAnimation(eff, "opacity"); op->setDuration(Motion::Slow);
    op->setKeyValueAt(0.0, 0.0); op->setKeyValueAt(0.15, 1.0); op->setKeyValueAt(0.8, 1.0); op->setKeyValueAt(1.0, 0.0); g->addAnimation(op);
    QPointer<QToolButton> btn = m_downloadsBtn;
    connect(g, &QAbstractAnimation::finished, fly, [fly, btn]{ fly->deleteLater(); if (auto* b = qobject_cast<AnimatedToolButton*>(btn.data())) b->bump(); });
    g->start(QAbstractAnimation::DeleteWhenStopped);
}

void BrowserWindow::toggleDownloadsPopup(bool autoClose) {
    if (!m_downloadsPopup) return;
    if (m_downloadsPopup->isVisible()) { m_downloadsPopup->hide(); return; }
    if (QDateTime::currentMSecsSinceEpoch() - m_downloadsPopup->hiddenAt() < 250) return;   // click on the button just closed it
    if (!m_downloadsBtn->isVisible()) m_downloadsBtn->show();
    m_downloadsBadge->clearAlert();
    m_downloadsPopup->showUnder(m_downloadsBtn, autoClose);
}

void BrowserWindow::updateDownloadsButton() {
    const bool show = readSettings().value(QStringLiteral("pin_downloads_button")).toBool(false)
                   || m_downloads->hadDownloadThisSession() || m_downloads->activeCount() > 0;
    m_downloadsWanted = show;
    m_downloadsBtn->setVisible(show);
    updateToolbarOverflow();
}
void BrowserWindow::showSettings() {
    for (int i = 0; i < m_tabs->count(); ++i)
        if (qobject_cast<SettingsPage*>(m_tabs->widget(i))) { m_tabs->setCurrentIndex(i); return; }
    auto* page = new SettingsPage(this);
    connect(page, &SettingsPage::settingChanged, this, &BrowserWindow::onSettingChanged);
    connect(page, &SettingsPage::clearDataRequested, this, &BrowserWindow::clearBrowsingData);
    const int idx = m_tabs->addTab(page, QStringLiteral("Settings"));
    m_tabs->setTabIcon(idx, m_theme->secondaryIcon(QStringLiteral("settings"), 16));
    m_tabs->setCurrentIndex(idx);
}

void BrowserWindow::clearBrowsingData(bool history, bool cookies, bool cache) {
    auto* p = Pacitum::mainProfile();
    if (history) { m_history->clear(); p->clearAllVisitedLinks(); m_recentlyClosed.clear(); }
    if (cookies) p->cookieStore()->deleteAllCookies();
    if (cache) p->clearHttpCache();
}

void BrowserWindow::onSettingChanged(const QString& key) {
    // Apply one setting live. Everything is re-read from browser_settings.json.
    const auto s = readSettings();
    auto forTabs = [this](auto fn){ for (auto* tw : {m_tabs, m_splitTabs}) for (int i = 0; i < tw->count(); ++i) if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i))) fn(t); };
    if (key == QStringLiteral("theme") || key == QStringLiteral("accent")) {
        loadSettings(); applyTheme();
    } else if (key == QStringLiteral("search_engine")) {
        loadSettings();
        forTabs([this](BrowserTab* t){ t->setSearchEngine(m_searchEngine); });
    } else if (key == QStringLiteral("default_zoom")) {
        m_zoom->setZoom(s.value(key).toInt(100) / 100.0);
        forTabs([this](BrowserTab* t){ t->setZoomFactor(m_zoom->zoom()); });
    } else if (key == QStringLiteral("adblock_enabled")) {
        m_adblock->setEnabled(s.value(key).toBool(true));
    } else if (key == QStringLiteral("do_not_track")) {
        m_adblock->setDoNotTrack(s.value(key).toBool(false));
    } else if (key == QStringLiteral("show_bookmarks_bar")) {
        Motion::slideVisible(m_bookmarksBar, s.value(key).toBool(true), 0, Motion::Base);
    } else if (key == QStringLiteral("javascript_enabled") || key == QStringLiteral("popups_allowed")) {
        Pacitum::webPrefs().javascript = s.value(QStringLiteral("javascript_enabled")).toBool(true);
        Pacitum::webPrefs().popups = s.value(QStringLiteral("popups_allowed")).toBool(false);
        if (key == QStringLiteral("popups_allowed"))
            Perm::Store::forProfile(Pacitum::mainProfile())->setDefault(Perm::Kind::Popups, Pacitum::webPrefs().popups ? Perm::Decision::Allow : Perm::Decision::Block);
        Pacitum::applyWebSettings(Pacitum::mainProfile()->settings());
        forTabs([](BrowserTab* t){ t->applyWebSettings(); });
    } else if (key == QStringLiteral("animations")) {
        loadSettings();
    } else if (key == QStringLiteral("pin_downloads_button")) {
        updateDownloadsButton();
    } else if (key == QStringLiteral("auto_check_updates")) {
        if (m_updater && !m_noBackgroundUpdates) m_updater->setAutoCheck(s.value(key).toBool(true));
    } else if (key == QStringLiteral("check_updates_now")) {
        if (m_updater) m_updater->check(true);
    } else if (key == QStringLiteral("freeze_background_tabs")) {
        BrowserTab::s_freezeEnabled = s.value(key).toBool(true);
        if (!BrowserTab::s_freezeEnabled) forTabs([](BrowserTab* t){ t->ensureActive(); });
    } else if (key == QStringLiteral("show_suggestions")) {
        m_addressBar->setSuggestionsEnabled(s.value(key).toBool(true));
    } else if (key == QStringLiteral("search_suggest_online")) {
        m_addressBar->setOnlineSuggestions(s.value(key).toBool(true));
    }
    // homepage, startup/restore_session, download_path, ask_download_location, clear_on_exit are read when used.
}
void BrowserWindow::showMenu() {
    // ⋮ menu (spec §5.5, menu-*.png): 312 wide, r14, padding 6, rows 34 r8, icons 18 text-secondary.
    Pacitum::PopupMenu m(this);
    m.setObjectName(QStringLiteral("MainMenu"));
    m.setMinimumWidth(312 + 2 * m.shadowMargin());
    const auto& pal = m_theme->palette();
    auto add = [&](const QString& icon, const QString& text, const QString& keys, auto&& fn) {
        auto* a = m.addAction(m_theme->secondaryIcon(icon, 18), text);
        if (!keys.isEmpty()) a->setShortcut(QKeySequence(keys));
        a->setShortcutVisibleInContextMenu(true);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };

    // Profile header: avatar 36, name 13/600, status 12 (local profile; Pacitum has no sync service)
    {
        auto* hdr = new QWidget(&m); hdr->setObjectName(QStringLiteral("MenuProfile"));
        hdr->setAttribute(Qt::WA_StyledBackground, true); hdr->setAttribute(Qt::WA_Hover, true);
        hdr->setFixedHeight(52); hdr->setCursor(Qt::PointingHandCursor);
        auto* hl = new QHBoxLayout(hdr); hl->setContentsMargins(10, 8, 10, 8); hl->setSpacing(12);
        const QString name = ProfileManager::instance()->current().name;
        auto* av = new QLabel(hdr);   // 1.8.8: same avatar as everywhere else (photo, glyph or initial)
        av->setFixedSize(36, 36); av->setAlignment(Qt::AlignCenter);
        av->setPixmap(ProfileManager::avatarPixmap(ProfileManager::instance()->current(), 36, devicePixelRatioF()));
        auto* col = new QVBoxLayout; col->setSpacing(1); col->setContentsMargins(0, 0, 0, 0);
        auto* nl = new QLabel(name, hdr); nl->setObjectName(QStringLiteral("MenuProfileName"));
        const auto* sync = SyncManager::instance();
        const QString subtitle = m_privateBadge->isVisible() ? QStringLiteral("Private browsing")
                               : sync->signedIn() ? QStringLiteral("Syncing · %1").arg(sync->account().email.isEmpty() ? QStringLiteral("Google account") : sync->account().email)
                               : QStringLiteral("Local profile · Not syncing");
        auto* sl = new QLabel(subtitle, hdr);
        sl->setObjectName(QStringLiteral("MenuProfileSub"));
        col->addWidget(nl); col->addWidget(sl);
        auto* chev = new QLabel(hdr); chev->setPixmap(m_theme->secondaryIcon(QStringLiteral("chevron-right"), 16).pixmap(QSize(16, 16), devicePixelRatioF()));
        hl->addWidget(av); hl->addLayout(col, 1); hl->addWidget(chev);
        auto* wa = new QWidgetAction(&m); wa->setDefaultWidget(hdr); m.addAction(wa);
        struct Click : QObject { std::function<void()> fn; using QObject::QObject;
            bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::MouseButtonRelease) { fn(); return true; } return false; } };
        auto* cf = new Click(hdr); cf->fn = [this, &m]{ m.close(); QTimer::singleShot(0, this, &BrowserWindow::showSettings); };
        hdr->installEventFilter(cf);
        hdr->setToolTip(QStringLiteral("Profile settings"));
    }
    // A row of side-by-side buttons (icon over a short label) in place of several full-width rows,
    // so the menu stays short enough to never wrap into a second column.
    struct Tile { QString icon, label, tip; std::function<void()> fn; };
    auto addTiles = [&](const QList<Tile>& tiles) {
        auto* row = new QWidget(&m); row->setObjectName(QStringLiteral("MenuTiles"));
        auto* rl = new QHBoxLayout(row); rl->setContentsMargins(2, 2, 2, 2); rl->setSpacing(4);
        for (const Tile& t : tiles) {
            auto* b = new QToolButton(row);
            b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            b->setIcon(m_theme->secondaryIcon(t.icon, 20)); b->setIconSize(QSize(20, 20));
            b->setText(t.label); b->setToolTip(t.tip); b->setAccessibleName(t.tip);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); b->setFixedHeight(54);
            b->setCursor(Qt::PointingHandCursor); b->setFocusPolicy(Qt::TabFocus);
            const auto fn = t.fn;
            connect(b, &QToolButton::clicked, this, [this, &m, fn]{ m.close(); QTimer::singleShot(0, this, fn); });
            rl->addWidget(b);
        }
        auto* wa = new QWidgetAction(&m); wa->setDefaultWidget(row); m.addAction(wa);
    };
    m.addSeparator();
    addTiles({
        {QStringLiteral("tab-new"), QStringLiteral("New tab"), QStringLiteral("New tab (Ctrl+T)"), [this]{ newTab(); }},
        {QStringLiteral("window"), QStringLiteral("New window"), QStringLiteral("New window (Ctrl+N)"), [this]{ auto* w = new BrowserWindow(); w->setAttribute(Qt::WA_DeleteOnClose); w->show(); }},
        {QStringLiteral("private"), QStringLiteral("Private tab"), QStringLiteral("New private tab (Ctrl+Shift+N)"), [this]{ newPrivateTab(); }},
    });
    m.addSeparator();
    add(QStringLiteral("history"), QStringLiteral("History"), QStringLiteral("Ctrl+H"), [this]{ showHistory(); });
    add(QStringLiteral("download"), QStringLiteral("Downloads"), QStringLiteral("Ctrl+J"), [this]{ showDownloads(); });
    add(QStringLiteral("bookmarks"), QStringLiteral("Bookmarks"), QStringLiteral("Ctrl+Shift+O"), [this]{ showBookmarksDialog(); });
    add(QStringLiteral("extensions"), QStringLiteral("Extensions"), QString(), [this]{ QTimer::singleShot(0, this, &BrowserWindow::showExtensionsMenu); });
    m.addSeparator();
    // 1.8.5 built-in tools, one row
    addTiles({
        {QStringLiteral("grid"), QStringLiteral("Overview"), QStringLiteral("3D tab overview (Ctrl+Shift+Space)"), [this]{ showTabOverview(); }},
        {QStringLiteral("screenshot"), QStringLiteral("Screenshot"), QStringLiteral("Screenshot (Ctrl+Shift+S)"), [this]{ takeScreenshot(); }},
        {QStringLiteral("picture-in-picture"), QStringLiteral("PiP"), QStringLiteral("Picture-in-picture (Alt+P)"), [this]{ togglePictureInPicture(); }},
        {QStringLiteral("notes"), QStringLiteral("Notes"), QStringLiteral("Notes (Ctrl+Shift+Y)"), [this]{ toggleNotes(); }},
        {QStringLiteral("qr-code"), QStringLiteral("QR code"), QStringLiteral("QR code for this page"), [this]{ showPageQr(); }},
    });
    m.addSeparator();

    // Zoom row: [zoom] Zoom ......  ( −  100 %  + )  [⛶]
    {
        auto* zoomRow = new QWidget(&m);
        auto* zl = new QHBoxLayout(zoomRow); zl->setContentsMargins(10, 2, 2, 2); zl->setSpacing(0);
        auto* zi = new QLabel(zoomRow); zi->setPixmap(m_theme->secondaryIcon(QStringLiteral("zoom-in"), 18).pixmap(QSize(18, 18), devicePixelRatioF()));
        auto* zt = new QLabel(QStringLiteral("Zoom"), zoomRow);
        auto* pill = new QWidget(zoomRow); pill->setObjectName(QStringLiteral("ZoomPill")); pill->setAttribute(Qt::WA_StyledBackground, true);
        pill->setFixedHeight(30);
        auto* pl = new QHBoxLayout(pill); pl->setContentsMargins(2, 0, 2, 0); pl->setSpacing(0);
        auto mk = [&](const QString& icon, const QString& tip, QWidget* parent) {
            auto* b = new QToolButton(parent); b->setFixedSize(26, 26); b->setIconSize(QSize(16, 16));
            b->setIcon(m_theme->secondaryIcon(icon, 16)); b->setToolTip(tip); b->setAccessibleName(tip); b->setAutoRaise(true);
            return b; };
        auto* zminus = mk(QStringLiteral("zoom-out"), QStringLiteral("Zoom out (Ctrl+-)"), pill);
        auto* zpct = new QLabel(QString::number(m_zoom->zoomPercent()) + QStringLiteral("%"), pill); zpct->setFixedWidth(44); zpct->setAlignment(Qt::AlignCenter);
        auto* zplus = mk(QStringLiteral("zoom-in"), QStringLiteral("Zoom in (Ctrl+=)"), pill);
        pl->addWidget(zminus); pl->addWidget(zpct); pl->addWidget(zplus);
        auto* fs = mk(isFullScreen() ? QStringLiteral("fullscreen-exit") : QStringLiteral("fullscreen"), QStringLiteral("Full screen (F11)"), zoomRow);
        fs->setFixedSize(30, 30); fs->setIconSize(QSize(18, 18));
        connect(zminus, &QToolButton::clicked, this, [this, zpct]{ zoomOut(); zpct->setText(QString::number(m_zoom->zoomPercent()) + QStringLiteral("%")); Motion::pop(zpct); });
        connect(zplus, &QToolButton::clicked, this, [this, zpct]{ zoomIn(); zpct->setText(QString::number(m_zoom->zoomPercent()) + QStringLiteral("%")); Motion::pop(zpct); });
        connect(fs, &QToolButton::clicked, this, [this, &m]{ m.close(); toggleFullscreen(); });
        zl->addWidget(zi); zl->addSpacing(12); zl->addWidget(zt); zl->addStretch(1); zl->addWidget(pill); zl->addSpacing(10); zl->addWidget(fs);
        zoomRow->setFixedHeight(36);
        auto* za = new QWidgetAction(&m); za->setDefaultWidget(zoomRow); m.addAction(za);
    }
    m.addSeparator();
    add(QStringLiteral("print"), QStringLiteral("Print…"), QStringLiteral("Ctrl+P"), [this]{ if (auto* t = activeTab()) if (auto* v = t->findChild<QWebEngineView*>()) { v->printToPdf(QDir::homePath() + QStringLiteral("/page.pdf")); statusBar()->showMessage(QStringLiteral("Saved page.pdf to your home folder"), 2500); } });
    add(QStringLiteral("find"), QStringLiteral("Find in page…"), QStringLiteral("Ctrl+F"), [this]{ showFind(); });
    add(QStringLiteral("reader-mode"), QStringLiteral("Reader mode"), QStringLiteral("Ctrl+Shift+R"), [this]{ toggleReaderMode(); });
    add(QStringLiteral("share"), QStringLiteral("Share… (copy link)"), QString(), [this]{ copyCurrentUrl(); });
    add(QStringLiteral("devtools"), QStringLiteral("Developer tools"), QStringLiteral("F12"), [this]{ showDevTools(); });
    {
        auto* sub = m.addMenu(m_theme->secondaryIcon(QStringLiteral("translate"), 18), QStringLiteral("Translate page"));
        const QString ui = QLocale::system().name().section(QLatin1Char('_'), 0, 0).toLower();
        struct Lang { const char* code; const char* name; };
        const Lang langs[] = {
            {"en", "English"}, {"es", "Spanish"}, {"fr", "French"}, {"de", "German"},
            {"pt", "Portuguese"}, {"th", "Thai"}, {"ja", "Japanese"}, {"ko", "Korean"},
            {"zh-CN", "Chinese (Simplified)"}, {"ar", "Arabic"},
        };
        for (const Lang& lang : langs) {
            if (ui == QLatin1String(lang.code)) continue;
            auto* a = sub->addAction(QString::fromUtf8(lang.name));
            const QString code = QString::fromLatin1(lang.code);
            connect(a, &QAction::triggered, this, [this, code]{ translatePage(code); });
        }
        sub->addSeparator();
        auto* sys = sub->addAction(QStringLiteral("System language"));
        connect(sys, &QAction::triggered, this, [this, ui]{ translatePage(ui.isEmpty() ? QStringLiteral("en") : ui); });
    }
    m.addSeparator();
    add(QStringLiteral("settings"), QStringLiteral("Settings"), QStringLiteral("Ctrl+,"), [this]{ showSettings(); });
    add(QStringLiteral("help"), QStringLiteral("Help && about"), QString(), [this]{
        QMessageBox::about(this, QStringLiteral("About Pacitum Browser"),
            QStringLiteral("<b>Pacitum Browser</b> %1<br>A calm, fast web browser built on Qt %2 WebEngine.")
                .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion()))); });
    add(QStringLiteral("exit"), QStringLiteral("Quit Pacitum"), QStringLiteral("Ctrl+Q"), [this]{ close(); });
    Q_UNUSED(pal);

    // top-right anchored: 8 px from the window's right edge, 6 px under the toolbar
    const QSize sz = m.sizeHint();
    const QPoint tb = m_toolbar->mapToGlobal(QPoint(m_toolbar->width(), m_toolbar->height()));
    m_menuBtn->setDown(true);
    m.exec(QPoint(tb.x() - 8 - sz.width() + m.shadowMargin(), tb.y() + 6 - m.shadowMargin()));
    m_menuBtn->setDown(false);
}

void BrowserWindow::showDevTools() {
    auto* t = activeTab(); if (!t || !t->view()) return;
    auto* page = t->view()->page();
    if (page->devToolsPage()) { if (auto* v = QWebEngineView::forPage(page->devToolsPage())) { v->window()->show(); v->window()->raise(); v->window()->activateWindow(); return; } }
    auto* v = new QWebEngineView();
    v->setAttribute(Qt::WA_DeleteOnClose);
    v->setWindowTitle(QStringLiteral("Developer tools — ") + t->currentTitle());
    v->resize(1000, 700);
    page->setDevToolsPage(v->page());
    v->show();
}

void BrowserWindow::copyCurrentUrl() { if (auto* t=activeTab()) { QApplication::clipboard()->setText(t->currentUrl()); statusBar()->showMessage(QStringLiteral("Page address copied"), 1800);} }

void BrowserWindow::translatePage(const QString& targetLang) {
    auto* t = activeTab();
    if (!t) return;
    const QUrl src(t->currentUrl());
    if (!src.isValid() || (src.scheme() != QStringLiteral("http") && src.scheme() != QStringLiteral("https"))) {
        statusBar()->showMessage(QStringLiteral("Only web pages can be translated"), 2200);
        return;
    }
    QUrl dest(QStringLiteral("https://translate.google.com/translate"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
    QString tl = targetLang;
    if (tl == QStringLiteral("zh")) tl = QStringLiteral("zh-CN");
    q.addQueryItem(QStringLiteral("tl"), tl);
    q.addQueryItem(QStringLiteral("u"), src.toString(QUrl::FullyEncoded));
    dest.setQuery(q);
    t->loadUrl(dest.toString());
    statusBar()->showMessage(QStringLiteral("Translating this page…"), 2200);
}

void BrowserWindow::toggleReaderMode() {
    auto* t = activeTab();
    if (!t || !t->view() || !t->view()->page()) return;
    const QUrl src(t->currentUrl());
    if (src.scheme() != QStringLiteral("http") && src.scheme() != QStringLiteral("https")) {
        statusBar()->showMessage(QStringLiteral("Reader mode works on web pages"), 2200);
        return;
    }
    const auto& pal = m_theme->palette();
    const QString js = QStringLiteral(R"JS(
(function(){
  if (window.__pacitumReaderOn) {
    var st = document.getElementById('pacitum-reader-style'); if (st) st.remove();
    document.body.className = window.__pacitumReaderClass || '';
    document.body.innerHTML = window.__pacitumReaderHtml || '';
    window.__pacitumReaderOn = false;
    return 'off';
  }
  var root = document.querySelector('article') || document.querySelector('main') || document.body;
  if (!root) return 'empty';
  var clone = root.cloneNode(true);
  clone.querySelectorAll('script,style,noscript,iframe,svg,canvas,nav,footer,form,button,aside,[role="navigation"],[role="banner"],[role="contentinfo"]').forEach(function(n){ n.remove(); });
  var text = (clone.innerText || '').replace(/\s+/g, ' ').trim();
  var html = clone.innerHTML;
  if (text.length < 280) {
    var ps = Array.prototype.slice.call(document.querySelectorAll('p')).filter(function(p){ return (p.innerText || '').trim().length > 40; });
    html = ps.map(function(p){ return '<p>' + p.innerText.trim().replace(/</g,'') + '</p>'; }).join('');
    text = ps.map(function(p){ return p.innerText; }).join(' ');
  }
  if (!text || text.trim().length < 80) return 'empty';
  window.__pacitumReaderHtml = document.body.innerHTML;
  window.__pacitumReaderClass = document.body.className;
  var title = (document.title || '').replace(/</g, '');
  var css = 'html,body{margin:0;background:%1;color:%2;}'
    + '#pacitum-reader{max-width:680px;margin:48px auto 80px;padding:0 24px;font:18px/1.65 Georgia,Cambria,serif;}'
    + '#pacitum-reader h1{font:600 32px/1.25 sans-serif;margin:0 0 24px;letter-spacing:-0.02em;}'
    + '#pacitum-reader p{margin:0 0 16px;}'
    + '#pacitum-reader a{color:%3;}'
    + '#pacitum-reader img{max-width:100%;height:auto;border-radius:8px;}';
  var style = document.getElementById('pacitum-reader-style') || document.createElement('style');
  style.id = 'pacitum-reader-style';
  style.textContent = css;
  document.head.appendChild(style);
  document.body.className = '';
  document.body.innerHTML = '<article id="pacitum-reader"><h1>' + title + '</h1>' + html + '</article>';
  window.__pacitumReaderOn = true;
  return 'on';
})()
)JS").arg(pal.page, pal.text, pal.accent);
    QPointer<BrowserWindow> self(this);
    t->view()->page()->runJavaScript(js, [self](const QVariant& result) {
        if (!self) return;
        const QString state = result.toString();
        if (state == QStringLiteral("on"))
            self->statusBar()->showMessage(QStringLiteral("Reader mode on"), 1800);
        else if (state == QStringLiteral("off"))
            self->statusBar()->showMessage(QStringLiteral("Reader mode off"), 1800);
        else
            self->statusBar()->showMessage(QStringLiteral("This page does not have an article to read"), 2200);
    });
}
void BrowserWindow::showPageInfo() {
    if (auto* t = activeTab(); t && t->permissions() && !t->permissions()->origin().isEmpty()) {   // site controls panel (§7)
        t->permissions()->showSiteControls(m_addressBar->bubbleAnchor());
        return;
    }
    if (auto* t=activeTab()) {
        QString url=t->currentUrl();
        QString sec = url.startsWith(QStringLiteral("https://")) ? QStringLiteral("Secure HTTPS connection") : QStringLiteral("Unencrypted or local page");
        QMessageBox::information(this, QStringLiteral("Page Information"), sec + QStringLiteral("\n\n") + url);
    }
}
void BrowserWindow::showHistory() {
    auto r = m_history->recent(50);
    if (r.isEmpty()) { QMessageBox::information(this, QStringLiteral("History"), QStringLiteral("No browsing history yet.")); return; }
    QStringList lines; for (int i=0;i<qMin(20,(int)r.size());++i) lines << (r[i].title.isEmpty()?r[i].url:r[i].title);
    QMessageBox::information(this, QStringLiteral("History (%1 entries)").arg(r.size()), lines.join(QStringLiteral("\n")));
}
void BrowserWindow::clearHistory() { m_history->clear(); statusBar()->showMessage(QStringLiteral("History cleared"), 2000); }
void BrowserWindow::showBookmarksDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Bookmarks"));
    dlg.resize(640, 440);
    auto* layout = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    for (const auto& b : m_bookmarks->allBookmarks()) {
        auto* item = new QListWidgetItem(b.title + QStringLiteral("\\n") + b.url, list);
        item->setToolTip(b.url);
        item->setData(Qt::UserRole, b.url);
    }
    layout->addWidget(list, 1);
    auto* buttons = new QHBoxLayout;
    auto* importBtn = new QPushButton(QStringLiteral("Import HTML…"), &dlg);
    auto* openBtn = new QPushButton(QStringLiteral("Open"), &dlg);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), &dlg);
    buttons->addWidget(importBtn); buttons->addStretch(1); buttons->addWidget(openBtn); buttons->addWidget(closeBtn);
    layout->addLayout(buttons);
    connect(importBtn, &QPushButton::clicked, &dlg, [this, &dlg, list]{
        importBookmarks();
        list->clear();
        for (const auto& b : m_bookmarks->allBookmarks()) {
            auto* item = new QListWidgetItem(b.title + QStringLiteral("\\n") + b.url, list);
            item->setToolTip(b.url); item->setData(Qt::UserRole, b.url);
        }
    });
    connect(openBtn, &QPushButton::clicked, &dlg, [this, list, &dlg]{
        if (auto* item = list->currentItem()) { const QString url = item->data(Qt::UserRole).toString(); dlg.accept(); navigateToUrl(url); }
    });
    connect(list, &QListWidget::itemDoubleClicked, &dlg, [this, &dlg](QListWidgetItem* item){
        const QString url = item->data(Qt::UserRole).toString(); dlg.accept(); navigateToUrl(url);
    });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    dlg.exec();
}

void BrowserWindow::importBookmarks() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Bookmarks"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("Bookmark HTML (*.html *.htm);;All files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Import Bookmarks"),
            QStringLiteral("Could not read the selected file."));
        return;
    }
    const QString html = QString::fromUtf8(file.readAll());
    // Chrome, Edge and Firefox export Netscape-style bookmark HTML with <A HREF="...">title</A>.
    static const QRegularExpression anchor(
        QStringLiteral("<a\\b([^>]*)>(.*?)</a\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression href(
        QStringLiteral("href\\s*=\\s*([\"'])(.*?)\\1"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    QVector<QPair<QString, QString>> items;
    auto match = anchor.globalMatch(html);
    QTextDocument decoder;
    while (match.hasNext()) {
        const auto a = match.next();
        const auto hm = href.match(a.captured(1));
        if (!hm.hasMatch()) continue;
        QUrl url = QUrl::fromEncoded(hm.captured(2).toUtf8());
        if (!url.isValid() || url.scheme().isEmpty() ||
            (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https") &&
             url.scheme() != QStringLiteral("file") && url.scheme() != QStringLiteral("ftp"))) continue;
        decoder.setHtml(a.captured(2));
        QString title = decoder.toPlainText().trimmed();
        if (title.isEmpty()) title = url.toString();
        items.push_back(qMakePair(title, url.toString(QUrl::FullyEncoded)));
    }
    if (items.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Import Bookmarks"),
            QStringLiteral("No supported bookmarks were found. Choose an HTML bookmarks export."));
        return;
    }
    const int added = m_bookmarks->addBookmarks(items);
    m_bookmarksBar->refresh();
    if (auto* tab = activeTab()) m_addressBar->setBookmarked(m_bookmarks->isBookmarked(tab->currentUrl()));
    QMessageBox::information(this, QStringLiteral("Import Bookmarks"),
        QStringLiteral("Found %1 bookmarks. Imported %2 new bookmarks; duplicates were skipped.")
            .arg(items.size()).arg(added));
}
void BrowserWindow::toggleBookmarksBar() { const bool show = !Motion::targetVisible(m_bookmarksBar); Motion::slideVisible(m_bookmarksBar, show, 0, Motion::Base); auto s = readSettings(); s[QStringLiteral("show_bookmarks_bar")] = show; writeSettingsFile(s); }
void BrowserWindow::toggleVerticalTabs() { m_verticalTabs = !m_verticalTabs; m_tabs->setVertical(m_verticalTabs); m_splitTabs->setVertical(m_verticalTabs); }
// ---------------- fullscreen ----------------
// F11 (browser fullscreen) and website fullscreen (QWebEngineFullScreenRequest) share one path:
// remember the window state, hide tab strip / toolbar / bookmarks bar (the window is frameless, so
// no frame remains), go truly fullscreen; on exit put the chrome back and restore the old state.
void BrowserWindow::toggleFullscreen() {
    if (m_chromeHidden) exitContentFullscreen();
    else enterContentFullscreen(false);
}

void BrowserWindow::enterContentFullscreen(bool) {
    if (!m_chromeHidden) {
        m_preFullscreenState = windowState() & ~(Qt::WindowFullScreen | Qt::WindowMinimized);
        setChromeHidden(true);
        if (!m_pageFullscreen) fadeVeil(Motion::Slow, 0.7, 40);   // F11 enter: soft reveal (not over page video)
        QTimer::singleShot(120, this, [this]{   // kit #30: exit hint, 2 s total
            if (m_chromeHidden) Motion::bubble(this, m_pageFullscreen ? QStringLiteral("Press Esc to exit full screen") : QStringLiteral("Press F11 to exit full screen"), 1700, 24);
        });
    }
    // Transition guard: while Windows moves from maximized to fullscreen it can report an
    // intermediate "restored" state; 1.5.1 took that as "the user left fullscreen" and undid it,
    // which is why website fullscreen from a maximized window sometimes failed.
    m_fsTransition = true;
    QTimer::singleShot(600, this, [this]{ m_fsTransition = false; });
    if (!isFullScreen()) setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowFullScreen);
    if (!isVisible()) show();
#ifdef Q_OS_WIN
    QTimer::singleShot(60, this, [this]{ ensureFullscreenCoversMonitor(); });
#endif
    if (m_fullscreenTab && m_fullscreenTab->view()) m_fullscreenTab->view()->setFocus();
}

// Esc / F11 / menu: during website fullscreen ask the page to leave (its toggleOn=false request
// calls finishExitFullscreen). If the page never answers (crashed renderer, page navigated away,
// state already out of sync) a fallback restores the window anyway, so it can't get stuck.
void BrowserWindow::exitContentFullscreen() {
    if (m_pageFullscreen && m_fullscreenTab && m_fullscreenTab->view()) {
        QPointer<BrowserTab> t = m_fullscreenTab;
        m_fullscreenTab->view()->triggerPageAction(QWebEnginePage::ExitFullScreen);
        QTimer::singleShot(500, this, [this, t]{ if (m_chromeHidden && m_pageFullscreen && m_fullscreenTab == t) finishExitFullscreen(); });
        return;
    }
    finishExitFullscreen();
}

void BrowserWindow::finishExitFullscreen() {
    if (m_fullscreenTab) m_fullscreenTab->setNoFreeze(false);
    const bool wasPage = m_pageFullscreen;
    QPointer<BrowserTab> t = m_fullscreenTab;
    m_pageFullscreen = false; m_fullscreenTab.clear();
    m_fsTransition = false;
    if (wasPage && t && t->view()) t->view()->triggerPageAction(QWebEnginePage::ExitFullScreen);   // keep the page's own state in sync (no-op if already out)
    if (!m_chromeHidden) return;
    setChromeHidden(false);
    if (isFullScreen()) setWindowState(m_preFullscreenState & ~Qt::WindowFullScreen);
    if (!wasPage) fadeVeil(Motion::Slow, 0.7, 40);   // F11 exit
#ifdef Q_OS_WIN
    QTimer::singleShot(0, this, [this]{ applyNativeFrame(); });   // Qt's fullscreen code swaps window styles
#endif
}

void BrowserWindow::setChromeHidden(bool hidden) {
    if (hidden == m_chromeHidden) return;
    m_chromeHidden = hidden;
    if (hidden) {
        m_hiddenChrome.clear();
        QList<QWidget*> chrome{ m_tabStrip, m_toolbar, m_bookmarksBar,
                                findChild<QWidget*>(QStringLiteral("ChromeDivider")),
                                m_tabs ? m_tabs->tabBar() : nullptr,
                                m_splitTabs ? m_splitTabs->tabBar() : nullptr };
        // website fullscreen in split view: hide the other pane as well
        if (m_pageFullscreen && m_fullscreenTab && m_splitTabs) {
            if (m_splitTabs->indexOf(m_fullscreenTab) >= 0) chrome << m_tabs;
            else chrome << m_splitTabs;
        }
        for (QWidget* w : chrome) if (w && w->isVisible()) { m_hiddenChrome << w; w->hide(); }
    } else {
        for (const auto& w : std::as_const(m_hiddenChrome)) if (w) w->show();
        m_hiddenChrome.clear();
    }
}
void BrowserWindow::focusAddressBar() { m_addressBar->focusUrl(); }
void BrowserWindow::zoomIn() { if(auto* t=activeTab()){ m_zoom->zoomIn(); t->setZoomFactor(m_zoom->zoom()); m_zoomIndicator->setText(QString::number(m_zoom->zoomPercent())+QStringLiteral("%")); statusBar()->showMessage(QStringLiteral("Zoom: %1%").arg(m_zoom->zoomPercent()), 1000); showZoomBubble();} }
void BrowserWindow::zoomOut(){ if(auto* t=activeTab()){ m_zoom->zoomOut(); t->setZoomFactor(m_zoom->zoom()); m_zoomIndicator->setText(QString::number(m_zoom->zoomPercent())+QStringLiteral("%")); statusBar()->showMessage(QStringLiteral("Zoom: %1%").arg(m_zoom->zoomPercent()), 1000); showZoomBubble();} }
void BrowserWindow::zoomReset(){ if(auto* t=activeTab()){ m_zoom->zoomReset(); t->setZoomFactor(m_zoom->zoom()); m_zoomIndicator->setText(QStringLiteral("100%")); statusBar()->showMessage(QStringLiteral("Zoom: 100%"), 1000); showZoomBubble();} }
// Zoom indicator (kit #28/#29): a pill under the toolbar, fade 160 / hold 900 / fade 140. The page
// zoom itself stays instant (animating setZoomFactor would reflow every frame).
void BrowserWindow::showZoomBubble() {
    const int top = (m_chromeHidden || !m_toolbar) ? 16 : m_toolbar->mapTo(this, QPoint(0, m_toolbar->height())).y() + 12;
    Motion::bubble(this, QString::number(m_zoom->zoomPercent()) + QStringLiteral("%"), 900, top);
}
void BrowserWindow::showFind(){ if(auto* t=activeTab()) t->showFindBar(); }
void BrowserWindow::handleEscape(){
    if (m_chromeHidden) { exitContentFullscreen(); return; }
    if(auto* t=activeTab()) t->hideFindBar();
}
void BrowserWindow::nextTab(){ m_tabs->setCurrentIndex((m_tabs->currentIndex()+1)%m_tabs->count()); }
void BrowserWindow::prevTab(){ m_tabs->setCurrentIndex((m_tabs->currentIndex()-1+m_tabs->count())%m_tabs->count()); }
void BrowserWindow::lastTab(){ m_tabs->setCurrentIndex(m_tabs->count()-1); }
void BrowserWindow::duplicateTab(int index){ if(auto* t=qobject_cast<BrowserTab*>(m_tabs->widget(index))) newTab(t->currentUrl()); }
void BrowserWindow::muteTab(int idx,bool m){ if(auto* t=qobject_cast<BrowserTab*>(m_tabs->widget(idx))){ t->setMuted(m); updateTabAudio(t); } }
void BrowserWindow::pinTab(int idx,bool p){ m_tabs->setTabPinned(idx,p); }
void BrowserWindow::reopenClosedTab(){ if(!m_recentlyClosed.isEmpty()){ auto last=m_recentlyClosed.takeLast(); newTab(last.url);} }
void BrowserWindow::saveSessionDialog(){
    if (m_firstRun) FirstRunBridge::markFirstRunDone();   // the welcome flow is shown once
    QVector<SessionTab> tabs;
    for(int i=0;i<m_tabs->count();++i) if(auto* t=qobject_cast<BrowserTab*>(m_tabs->widget(i)))
        if (!t->currentUrl().startsWith(QStringLiteral("pacitum://welcome"))) tabs.push_back({t->currentUrl(), t->currentTitle(), false});
    m_session->save(tabs); statusBar()->showMessage(QStringLiteral("Session saved"), 2000);
}
void BrowserWindow::onDownloadRequested(QWebEngineDownloadRequest* dl){
    const auto s = readSettings();
    const QString dir = s.value(QStringLiteral("download_path")).toString(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (!dir.isEmpty()) { QDir().mkpath(dir); dl->setDownloadDirectory(dir); }
    if (s.value(QStringLiteral("ask_download_location")).toBool(false)) {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save file"), QDir(dl->downloadDirectory()).filePath(dl->downloadFileName()));
        if (path.isEmpty()) { dl->cancel(); return; }
        dl->setDownloadDirectory(QFileInfo(path).absolutePath());
        dl->setDownloadFileName(QFileInfo(path).fileName());
    }
    m_downloads->addDownload(dl);
    updateDownloadsButton();
    // auto-open the popup on a new download, unless the downloads page is in front
    if (isActiveWindow() && !qobject_cast<DownloadsPage*>(m_tabs->currentWidget()))
        QTimer::singleShot(0, this, [this]{ if (m_downloadsPopup && !m_downloadsPopup->isVisible()) toggleDownloadsPopup(true); });
}

void BrowserWindow::toggleSplitView(int index) {
    if (!m_splitActive) openSplit(index); else closeSplit();
}

// Split open/close/swap: one snapshot of the content area fades out over the new layout, so the
// web views are resized once instead of every frame.
void BrowserWindow::splitCrossFade(const std::function<void()>& change) {
    const int ms = m_animations ? Motion::fadeMs(Motion::Medium) : 0;
    QWidget* host = m_splitter ? m_splitter->parentWidget() : nullptr;
    QPixmap shot = (ms > 0 && host && m_splitter->isVisible()) ? m_splitter->grab() : QPixmap();
    change();
    if (shot.isNull()) return;
    auto* veil = new Motion::FadeOverlay(host);
    veil->setPixmap(shot);
    veil->setGeometry(m_splitter->geometry());
    veil->fade(1.0, 0.0, ms, QEasingCurve::OutCubic, 0, QPoint(), true);
}

void BrowserWindow::openSplit(int index) {
    if (m_splitActive) { if (index >= 0) moveTabToSplit(index); return; }
    if (index < 0 || m_tabs->count() < 2) {   // nothing to put on the right yet: a fresh tab goes there
        newTab();
        index = m_tabs->currentIndex();
    }
    splitCrossFade([this, index]{
        m_splitActive = true;
        m_splitTabs->show();
        QWidget* tab = m_tabs->widget(index);
        const QString title = m_tabs->tabText(index);
        m_tabs->removeTab(index);
        m_splitTabs->addTab(tab, title);
        m_splitTabs->setCurrentWidget(tab);
        if (auto* bt = qobject_cast<BrowserTab*>(tab)) updateTabIcon(bt);
        m_splitter->equalize();
        tabActivated(m_splitTabs, m_splitTabs->currentIndex());
    });
}

void BrowserWindow::closeSplit() {
    if (!m_splitActive) return;
    splitCrossFade([this]{
        while (m_splitTabs->count() > 0) {
            QWidget* tab = m_splitTabs->widget(0);
            const QString title = m_splitTabs->tabText(0);
            m_splitTabs->removeTab(0);
            m_tabs->addTab(tab, title);
            if (auto* bt = qobject_cast<BrowserTab*>(tab)) updateTabIcon(bt);
        }
        m_splitTabs->hide();
        m_splitActive = false;
        m_activeWidget = m_tabs;
        m_splitter->setActivePane(nullptr);
        if (m_tabs->currentIndex() >= 0) tabActivated(m_tabs, m_tabs->currentIndex());
    });
}

void BrowserWindow::moveTabToSplit(int index) {
    if (index < 0 || index >= m_tabs->count()) return;
    if (!m_splitActive) { openSplit(index); return; }
    if (m_tabs->count() < 2) return;   // the left side must keep a tab
    QWidget* tab = m_tabs->widget(index);
    const QString title = m_tabs->tabText(index);
    m_tabs->removeTab(index);
    m_splitTabs->addTab(tab, title);
    m_splitTabs->setCurrentWidget(tab);
    if (auto* bt = qobject_cast<BrowserTab*>(tab)) updateTabIcon(bt);
    tabActivated(m_splitTabs, m_splitTabs->currentIndex());
}

void BrowserWindow::moveTabFromSplit(int index) {
    if (index < 0 || index >= m_splitTabs->count()) return;
    if (m_splitTabs->count() == 1) { closeSplit(); return; }
    QWidget* tab = m_splitTabs->widget(index);
    const QString title = m_splitTabs->tabText(index);
    m_splitTabs->removeTab(index);
    const int at = m_tabs->addTab(tab, title);
    if (auto* bt = qobject_cast<BrowserTab*>(tab)) updateTabIcon(bt);
    m_tabs->setCurrentIndex(at);
}

void BrowserWindow::swapSplitSides() {
    if (!m_splitActive || m_splitter->count() < 2) return;
    splitCrossFade([this]{
        const QList<int> sizes = m_splitter->sizes();
        m_splitter->insertWidget(0, m_splitter->widget(1));
        m_splitter->setSizes({sizes.value(1), sizes.value(0)});
        m_splitter->refreshMarker(false);
    });
}

void BrowserWindow::toggleDarkMode(){ m_darkMode=!m_darkMode; m_themeSetting = m_darkMode ? QStringLiteral("Dark") : QStringLiteral("Light"); applyTheme(); saveSettings(); }

void BrowserWindow::applyTheme(){
    if (m_themeSetting == QStringLiteral("System")) {
        auto* sh = QGuiApplication::styleHints();
        m_darkMode = sh ? sh->colorScheme() != Qt::ColorScheme::Light : true;
    }
    m_theme->setAccent(m_accent.compare(QStringLiteral("#2DD4BF"), Qt::CaseInsensitive) == 0 ? QString() : m_accent);
    m_theme->setDarkMode(m_darkMode);
    const auto& p = m_theme->palette();
    qApp->setStyleSheet(m_theme->mainWindowStyle());
    m_tabs->applyTheme(m_theme);
    m_splitTabs->applyTheme(m_theme);
    m_addressBar->applyTheme(m_theme);
    applyToolbarTheme();
    for (auto* tw : {m_tabs, m_splitTabs})
        for (int i = 0; i < tw->count(); ++i) if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i))) t->refreshTheme();
    refreshIcons();
    if (m_downloadsPopup) m_downloadsPopup->applyTheme();
    if (m_downloadsBadge) m_downloadsBadge->update();
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto* dp = qobject_cast<DownloadsPage*>(m_tabs->widget(i))) { dp->applyTheme(); m_tabs->setTabIcon(i, downloadsTabIcon()); }
    for (auto* tw : {m_tabs, m_splitTabs})
        for (int i = 0; i < tw->count(); ++i) if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i))) updateTabIcon(t);

    auto* h = PacitumSchemeHandler::instance();
    h->setThemeVars({{QStringLiteral("TOOL"), p.toolbar}, {QStringLiteral("PAGE"), p.page}, {QStringLiteral("SURF"), p.surface},
                     {QStringLiteral("BORD"), p.border}, {QStringLiteral("TXT"), p.text}, {QStringLiteral("SEC"), p.textSecondary},
                     {QStringLiteral("ACC"), p.accent}, {QStringLiteral("ACCS"), p.accentSoft},
                     {QStringLiteral("SURFH"), p.surfaceHover}, {QStringLiteral("BORD2"), p.borderStrong}, {QStringLiteral("TER"), p.textTertiary},
                     {QStringLiteral("ONACC"), p.onAccent}, {QStringLiteral("DANGER"), p.danger},
                     {QStringLiteral("CHROME"), p.frame}, {QStringLiteral("ACCH"), p.accentHover}, {QStringLiteral("RING"), p.focusRing},
                     {QStringLiteral("WARN"), p.warning.isEmpty() ? QStringLiteral("#F59E0B") : p.warning},
                     // home blob 1: the accent / profile colour (spec §2.4 "Home")
                     {QStringLiteral("PCOL"), QColor(m_accent).isValid() ? m_accent : QStringLiteral("#2DD4BF")},
                     {QStringLiteral("BLOBO"), m_darkMode ? QStringLiteral(".20") : QStringLiteral(".22")}});
    // Internal pages are re-served with the new tokens. The welcome flow is live-updated over its
    // web channel instead (a reload would lose the current step).
    for (auto* tw : {m_tabs, m_splitTabs})
        for (int i = 0; i < tw->count(); ++i)
            if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i)))
                if (t->currentUrl().startsWith(QStringLiteral("pacitum:")) && !t->currentUrl().startsWith(QStringLiteral("pacitum://welcome"))) t->reload();
    if (m_firstRun) emit m_firstRun->themeChanged(m_firstRun->palette());
}

void BrowserWindow::applyToolbarTheme(){
    // Styling for TabStrip / NavToolbar / BookmarksBar lives in the app-wide QSS (ThemeManager).
    m_bookmarksBar->setStyleSheet(QString());
    m_bookmarksBar->applyTheme(m_theme);
    m_toolbar->setStyleSheet(QString());
}

// Settings: one shared owner (AppSettings) so this window, the settings page and the welcome flow
// never overwrite each other's changes with stale copies.
QJsonObject BrowserWindow::readSettings() const { return AppSettings::read(); }
void BrowserWindow::writeSettingsFile(const QJsonObject& s) { AppSettings::write(s); }

void BrowserWindow::loadSettings(){
    auto s=readSettings();
    m_themeSetting = s.value(QStringLiteral("theme")).toString(s.contains(QStringLiteral("dark_mode")) ? (s.value(QStringLiteral("dark_mode")).toBool() ? QStringLiteral("Dark") : QStringLiteral("Light")) : QStringLiteral("System"));
    m_darkMode = m_themeSetting == QStringLiteral("Dark");
    m_accent = s.value(QStringLiteral("accent")).toString(QStringLiteral("#2DD4BF"));
    m_searchEngine = s.value(QStringLiteral("search_engine")).toString(QStringLiteral("Google"));
    {
        static const QMap<QString,QString> engines{
            {QStringLiteral("Google"), QStringLiteral("https://www.google.com/search?q=")},
            {QStringLiteral("Bing"), QStringLiteral("https://www.bing.com/search?q=")},
            {QStringLiteral("DuckDuckGo"), QStringLiteral("https://duckduckgo.com/?q=")},
            {QStringLiteral("Yahoo"), QStringLiteral("https://search.yahoo.com/search?p=")},
            {QStringLiteral("Brave"), QStringLiteral("https://search.brave.com/search?q=")}};
        PacitumSchemeHandler::instance()->setSearchBase(engines.value(m_searchEngine, engines.value(QStringLiteral("Google"))));
    }
    m_adblock->setEnabled(s.value(QStringLiteral("adblock_enabled")).toBool(true));
    m_adblock->setDoNotTrack(s.value(QStringLiteral("do_not_track")).toBool(false));
    m_animations = s.value(QStringLiteral("animations")).toBool(true);
    BrowserTab::s_freezeEnabled = s.value(QStringLiteral("freeze_background_tabs")).toBool(true);
    BrowserTab::s_discardEnabled = s.value(QStringLiteral("discard_background_tabs")).toBool(true);
    // "reduce_motion" (or animations off) -> every spec duration becomes 0 (spec §6)
    // (was: only ever switched *on*, so turning the setting off again needed a restart)
    if (!m_animations || s.value(QStringLiteral("reduce_motion")).toBool(false)) Pacitum::setReducedMotion(true);
    else Pacitum::clearReducedMotionOverride();
    if (auto* b = qobject_cast<PacitumTabBar*>(m_tabs->stripBar())) b->animationsEnabled = m_animations;
    m_addressBar->setSuggestionsEnabled(s.value(QStringLiteral("show_suggestions")).toBool(true));
    m_addressBar->setSearchEngine(m_searchEngine);
    m_addressBar->setOnlineSuggestions(s.value(QStringLiteral("search_suggest_online")).toBool(true));
    m_bookmarksBar->setProperty("_pmTarget", QVariant());
    m_bookmarksBar->setVisible(s.value(QStringLiteral("show_bookmarks_bar")).toBool(true));
    if(s.value(QStringLiteral("vertical_tabs")).toBool(false)){ m_verticalTabs=true; m_tabs->setVertical(true); m_splitTabs->setVertical(true); }
    int zoomPct=s.value(QStringLiteral("default_zoom")).toInt(100); m_zoom->setZoom(zoomPct/100.0);
}

void BrowserWindow::saveSettings(){
    auto s=readSettings();
    s[QStringLiteral("dark_mode")] = m_darkMode;
    // (was: always wrote Dark/Light, silently turning "System" into a fixed theme on the next save)
    s[QStringLiteral("theme")] = m_themeSetting.isEmpty() ? (m_darkMode?QStringLiteral("Dark"):QStringLiteral("Light")) : m_themeSetting;
    writeSettingsFile(s);
}

void BrowserWindow::restoreOrNewTab(){
    auto s = readSettings();
    if (!s.value(QStringLiteral("first_run_done")).toBool(false)) {
        // Upgrades from ≤1.4 already have a profile: don't interrupt them with the welcome flow.
        const bool existingProfile = !s.isEmpty() || QFile::exists(QStringLiteral("browser_session.json")) || QFile::exists(QStringLiteral("bookmarks.json"));
        if (existingProfile || m_privateMode) FirstRunBridge::markFirstRunDone();
        else { openWelcome(); return; }
    }
    QString startup = s.value(QStringLiteral("startup")).toString();
    if (startup.isEmpty()) startup = s.value(QStringLiteral("restore_session")).toBool(true) ? QStringLiteral("restore") : QStringLiteral("newtab");
    if (startup == QStringLiteral("restore")) {
        auto tabs = m_session->restore();
        if (!tabs.isEmpty()) {
            // Restoring N tabs used to run the full tab-switch path (address bar, history, icons,
            // fade, nav state) N times during startup; it now runs once for the tab that ends up active.
            m_restoring = true;
            struct Done { BrowserWindow* w; ~Done(){ w->m_restoring = false; if (w->m_tabs->currentIndex() >= 0) w->tabActivated(w->m_tabs, w->m_tabs->currentIndex()); } } done{this};
            // Only the active (last) tab loads now; the others load on first activation.
            for (int i = 0; i < tabs.size(); ++i) {
                if (i == tabs.size() - 1) { newTab(tabs[i].url); break; }
                newTab(QStringLiteral("pacitum-lazy:"));
                if (auto* t = activeTab()) { t->setLazyUrl(tabs[i].url, tabs[i].title);
                    m_tabs->setTabText(m_tabs->currentIndex(), tabs[i].title.isEmpty() ? tabs[i].url : tabs[i].title); }
            }
            return;
        }
    } else if (startup == QStringLiteral("homepage")) {
        newTab(s.value(QStringLiteral("homepage")).toString(PacitumSchemeHandler::newTabUrl())); return;
    }
    newTab();
}

void BrowserWindow::closeEvent(QCloseEvent* e){
    // 1.9.0 / 1.8.11 (macOS, Linux): the window shrinks away (Windows animates it natively)
    if (Motion::windowGhostsAllowed() && isVisible() && !isMinimized() && !m_chromeHidden) {
        const QRect from = frameGeometry();
        QRect to(QPoint(), from.size() * 0.9); to.moveCenter(from.center() + QPoint(0, 12));
        Motion::windowGhost(grab(), from, to, Motion::ms(Motion::Exit) + 40, 0.0);
    }
    if (m_firstRun) FirstRunBridge::markFirstRunDone();   // the welcome flow is shown once
    m_sessionTimer->stop();
    m_session->waitForPendingSave();
    const auto tabs = sessionTabs();
    if(!tabs.isEmpty()) m_session->save(tabs);
    auto s=readSettings();
    if(s.value(QStringLiteral("clear_on_exit")).toBool(false)){
        m_history->clear();
        Pacitum::mainProfile()->clearHttpCache();
        Pacitum::mainProfile()->cookieStore()->deleteAllCookies();
        Pacitum::mainProfile()->clearAllVisitedLinks();
    }
    QMainWindow::closeEvent(e);
}

QVector<SessionTab> BrowserWindow::sessionTabs() const {
    QVector<SessionTab> tabs;
    for(int i=0;i<m_tabs->count();++i) if(auto* t=qobject_cast<BrowserTab*>(m_tabs->widget(i)))
        if (!t->isPrivate() && !t->currentUrl().isEmpty() && !t->currentUrl().startsWith(QStringLiteral("pacitum://welcome"))) tabs.push_back({t->currentUrl(), t->currentTitle(), false});
    return tabs;
}

void BrowserWindow::scheduleSessionSave() {
    if (m_sessionTimer && !m_noBackgroundUpdates) m_sessionTimer->start();
}

void BrowserWindow::keyPressEvent(QKeyEvent* e){
    if(e->key()==Qt::Key_Escape && m_chromeHidden) { exitContentFullscreen(); return; }
    QMainWindow::keyPressEvent(e);
}

void BrowserWindow::changeEvent(QEvent* e) {
    if (e->type() == QEvent::WindowTitleChange && !m_titleGuard) updateRecordingIndicator();
    if (e->type() == QEvent::WindowStateChange && m_maxBtn) {
        m_maxBtn->setMaximized(isMaximized());
        m_maxBtn->setToolTip(isMaximized() ? QStringLiteral("Restore") : QStringLiteral("Maximize"));
    }
    if (e->type() == QEvent::WindowStateChange) {
        // Minimized: stop the tab-strip spinner animations (they repainted ~60x/s for nothing).
        const bool minimized = isMinimized();
        for (auto* tw : {m_tabs, m_splitTabs})
            if (tw) if (auto* b = qobject_cast<PacitumTabBar*>(tw->stripBar())) b->setSpinnerSuspended(minimized);
        // 1.9.0 fix: the "else" below used to hang off the loop's inner "if" (a comment had been
        // placed between them), so the restore branch ran for every non-web tab instead.
        if (minimized) m_spinTimer->stop();
        else if (m_tabs) {
            for (int i = 0; i < m_tabs->count(); ++i)
                if (auto* t = qobject_cast<BrowserTab*>(m_tabs->widget(i)); t && t->isLoading()) updateTabIcon(t);
        }
        // Minimized: hide the visible pages from Chromium (rAF/timers throttled like a background
        // tab) and let idle background tabs freeze after 1 minute instead of 10.
        for (auto* tw : {m_tabs, m_splitTabs}) {
            if (!tw) continue;
            for (int i = 0; i < tw->count(); ++i) if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i))) t->setWindowMinimized(minimized);
        }
        // The OS/user left fullscreen some other way (Win+Down, Win+Shift+arrows, display change):
        // bring the chrome back. Checked a moment later and never during our own transition, because
        // Windows reports intermediate states while it changes maximized <-> fullscreen.
        if (m_chromeHidden && !minimized && !m_fsTransition) {
            QTimer::singleShot(150, this, [this]{
                if (m_chromeHidden && !m_fsTransition && !isFullScreen() && !isMinimized()) finishExitFullscreen();
            });
        }

    }
    if (e->type() == QEvent::ActivationChange && isActiveWindow()) {
        installPopupFactory();
        if (m_pendingUpdatePrompt && !m_chromeHidden && m_updater && m_updater->latest().valid())
            QTimer::singleShot(800, this, [this]{ if (m_pendingUpdatePrompt && isActiveWindow() && !m_chromeHidden) showUpdatePrompt(m_updater->latest()); });
    }
    QMainWindow::changeEvent(e);
}

// True when the point (window coords) is empty title-bar space: the strip itself, the logo,
// or the tab bar outside any tab.
bool BrowserWindow::isCaptionArea(const QPoint& local) const {
    if (!m_tabStrip || !m_tabStrip->isVisible()) return false;
    const QPoint sp = m_tabStrip->mapFrom(this, local);
    if (!m_tabStrip->rect().contains(sp)) return false;
    QWidget* w = m_tabStrip->childAt(sp);
    if (!w || w == m_tabs->stripLogo()) return true;
    if (w == m_tabs->stripBar()) return m_tabs->stripBar()->tabAt(m_tabs->stripBar()->mapFrom(m_tabStrip, sp)) < 0;
    return false;
}

Qt::Edges BrowserWindow::edgesAt(const QPoint& global) const {
    if (isMaximized() || isFullScreen() || m_chromeHidden) return {};
    const int b = 6;
    const QRect r = frameGeometry();
    Qt::Edges e;
    if (global.x() < r.left() + b) e |= Qt::LeftEdge;
    if (global.x() > r.right() - b) e |= Qt::RightEdge;
    if (global.y() < r.top() + b) e |= Qt::TopEdge;
    if (global.y() > r.bottom() - b) e |= Qt::BottomEdge;
    return e;
}

bool BrowserWindow::eventFilter(QObject* o, QEvent* e) {
    // User activation for pop-up / download / device-chooser gating (permissions-spec §11).
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::KeyPress || e->type() == QEvent::TouchBegin) {
        if (auto* w = qobject_cast<QWidget*>(o); w && w->window() == this) {
            for (QWidget* p = w; p; p = p->parentWidget())
                if (auto* t = qobject_cast<BrowserTab*>(p)) {
                    if (t->permissions() && (w == t->view() || t->view()->isAncestorOf(w)) && !(e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() >= Qt::Key_Shift && static_cast<QKeyEvent*>(e)->key() <= Qt::Key_ScrollLock))
                        t->permissions()->noteUserInput();
                    break;
                }
        }
    }
    if (m_chromeHidden && (e->type() == QEvent::KeyPress || e->type() == QEvent::ShortcutOverride)) {
        auto* ke = static_cast<QKeyEvent*>(e);
        auto* w = qobject_cast<QWidget*>(o);
        if ((ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_F11) && w && w->window() == this) {
            if (e->type() == QEvent::ShortcutOverride) { e->accept(); return false; }
            exitContentFullscreen(); return true;
        }
    }
    if (o == m_toolbar && e->type() == QEvent::Resize) updateToolbarOverflow();
#ifdef Q_OS_WIN
    // Maximized: the caption is client area (see WM_NCHITTEST), so double-click -> restore and
    // right-click -> system menu are done here. Nothing in this path can start a move.
    if ((e->type() == QEvent::MouseButtonDblClick || e->type() == QEvent::MouseButtonRelease) && isMaximized() && !m_chromeHidden) {
        auto* w = qobject_cast<QWidget*>(o);
        auto* me = static_cast<QMouseEvent*>(e);
        if (w && w->window() == this && isCaptionArea(mapFromGlobal(me->globalPosition().toPoint()))) {
            if (e->type() == QEvent::MouseButtonDblClick && me->button() == Qt::LeftButton) { showNormal(); return true; }
            if (e->type() == QEvent::MouseButtonRelease && me->button() == Qt::RightButton) {
                POINT pt; GetCursorPos(&pt); showSystemMenu(pt.x, pt.y); return true;
            }
        }
    }
#endif
#ifndef Q_OS_WIN
    // Frameless resize/move for non-Windows platforms via the compositor (startSystemResize/Move).
    if (e->type() == QEvent::MouseMove || e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
        auto* w = qobject_cast<QWidget*>(o);
        if (w && w->window() == this && windowHandle()) {
            auto* me = static_cast<QMouseEvent*>(e);
            const QPoint g = me->globalPosition().toPoint();
            const Qt::Edges edges = edgesAt(g);
            if (e->type() == QEvent::MouseMove && me->buttons() == Qt::NoButton) {
                Qt::CursorShape shape = Qt::ArrowCursor;
                if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) shape = Qt::SizeFDiagCursor;
                else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) shape = Qt::SizeBDiagCursor;
                else if (edges & (Qt::LeftEdge | Qt::RightEdge)) shape = Qt::SizeHorCursor;
                else if (edges & (Qt::TopEdge | Qt::BottomEdge)) shape = Qt::SizeVerCursor;
                if (edges && !m_edgeCursor) { QGuiApplication::setOverrideCursor(shape); m_edgeCursor = true; }
                else if (edges && m_edgeCursor) QGuiApplication::changeOverrideCursor(shape);
                else if (!edges && m_edgeCursor) { QGuiApplication::restoreOverrideCursor(); m_edgeCursor = false; }
            } else if (e->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
                if (edges) { windowHandle()->startSystemResize(edges); return true; }
                // Locked while maximized/fullscreen: a caption drag must not move or un-maximize the window.
                if (isCaptionArea(mapFromGlobal(g))) { if (!isMaximized() && !isFullScreen() && !m_chromeHidden) windowHandle()->startSystemMove(); return true; }
            } else if (e->type() == QEvent::MouseButtonDblClick && me->button() == Qt::LeftButton && !isFullScreen() && !m_chromeHidden && isCaptionArea(mapFromGlobal(g))) {
                animatedToggleMaximize(); return true;
            }
        }
    }
#endif
    return QMainWindow::eventFilter(o, e);
}

#ifdef Q_OS_WIN
// ---------------------------------------------------------------------------------------------
// Windows frameless caption. The window keeps WS_THICKFRAME|WS_CAPTION so Windows provides Aero
// Snap, Win+arrows, min/max animations, the DWM shadow and snap layouts; WM_NCCALCSIZE makes the
// whole window client area and WM_NCHITTEST tells Windows which parts are border/caption/buttons.
// ---------------------------------------------------------------------------------------------
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef WM_NCMOUSELEAVE
#define WM_NCMOUSELEAVE 0x02A2
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef ABM_GETAUTOHIDEBAREX
#define ABM_GETAUTOHIDEBAREX 0x0000000b
#endif

// Per-monitor DPI helpers, resolved at runtime (Windows 10 1607+) with a system-DPI fallback.
using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
using GetSystemMetricsForDpiFn = int (WINAPI*)(int, UINT);
static UINT windowDpi(HWND hwnd) {
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    UINT dpi = fn ? fn(hwnd) : 0;
    if (!dpi) { HDC dc = GetDC(nullptr); dpi = UINT(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(nullptr, dc); }
    return dpi ? dpi : 96;
}
static int metricForDpi(int index, UINT dpi) {
    static auto fn = reinterpret_cast<GetSystemMetricsForDpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (fn) return fn(index, dpi);
    return MulDiv(GetSystemMetrics(index), int(dpi), 96);
}
// Resize border thickness in physical pixels for the monitor the window is on.
static int resizeBorder(HWND hwnd, bool horizontal) {
    const UINT dpi = windowDpi(hwnd);
    return metricForDpi(horizontal ? SM_CXSIZEFRAME : SM_CYSIZEFRAME, dpi) + metricForDpi(SM_CXPADDEDBORDER, dpi);
}
// Edge of the monitor that holds an auto-hide taskbar (ABE_*), or -1.
static int autoHideTaskbarEdge(HMONITOR mon) {
    APPBARDATA abd{}; abd.cbSize = sizeof(abd);
    if (!(SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE)) return -1;
    MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    for (UINT edge : {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}) {
        APPBARDATA q{}; q.cbSize = sizeof(q); q.uEdge = edge; q.rc = mi.rcMonitor;
        if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &q)) return int(edge);
    }
    return -1;
}

void BrowserWindow::applyNativeFrame() {
    HWND hwnd = reinterpret_cast<HWND>(internalWinId());
    if (!hwnd) return;   // not created yet; never create from here
    if (isFullScreen() || m_chromeHidden) return;   // fullscreen uses Qt's popup style (taskbar must hide)
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    // 1px of DWM frame keeps the native drop shadow (and Win11 snap highlight) on the frameless window.
    const MARGINS m{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    // Windows 11 rounded corners (ignored on Windows 10).
    const int pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

bool BrowserWindow::moveLocked() const {
    HWND hwnd = reinterpret_cast<HWND>(internalWinId());
    return isMaximized() || isFullScreen() || m_chromeHidden || m_fsTransition || (hwnd && IsZoomed(hwnd));
}

// Fullscreen entered from maximized sometimes kept the work-area size (taskbar still visible):
// make sure the window really covers its monitor.
void BrowserWindow::ensureFullscreenCoversMonitor() {
    if (!isFullScreen()) return;
    HWND hwnd = reinterpret_cast<HWND>(internalWinId());
    if (!hwnd) return;
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
    RECT wr; GetWindowRect(hwnd, &wr);
    if (!EqualRect(&wr, &mi.rcMonitor))
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

QPoint BrowserWindow::nativeToLocal(void* h, long x, long y) const {
    POINT pt{x, y};
    ScreenToClient(static_cast<HWND>(h), &pt);         // physical client coords (client == our widget area)
    const qreal dpr = windowHandle() ? windowHandle()->devicePixelRatio() : devicePixelRatioF();
    return QPoint(qFloor(pt.x / dpr), qFloor(pt.y / dpr));
}

bool BrowserWindow::overMaxButton(const QPoint& local) const {
    if (!m_maxBtn || !m_maxBtn->isVisible()) return false;
    return QRect(m_maxBtn->mapTo(this, QPoint(0, 0)), m_maxBtn->size()).contains(local);
}

void BrowserWindow::setNcMaxHover(bool on) {
    if (m_ncMaxHover == on) return;
    m_ncMaxHover = on;
    if (m_maxBtn) m_maxBtn->setForcedHover(on);
    if (!on && m_ncMaxPressed) { m_ncMaxPressed = false; if (m_maxBtn) m_maxBtn->setForcedDown(false); }
}

void BrowserWindow::showSystemMenu(int physX, int physY) {
    HWND hwnd = reinterpret_cast<HWND>(internalWinId());
    if (!hwnd) return;
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu) return;
    const bool zoomed = IsZoomed(hwnd), iconic = IsIconic(hwnd);
    auto enable = [&](UINT cmd, bool on) { EnableMenuItem(menu, cmd, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    enable(SC_RESTORE, zoomed || iconic);
    enable(SC_MOVE, !zoomed);
    enable(SC_SIZE, !zoomed && !iconic);
    enable(SC_MINIMIZE, !iconic);
    enable(SC_MAXIMIZE, !zoomed);
    enable(SC_CLOSE, true);
    SetMenuDefaultItem(menu, zoomed ? SC_RESTORE : SC_MAXIMIZE, FALSE);
    const UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    const int cmd = TrackPopupMenu(menu, flags, physX, physY, 0, hwnd, nullptr);
    if (cmd) PostMessageW(hwnd, WM_SYSCOMMAND, WPARAM(cmd), 0);
}

bool BrowserWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    MSG* msg = static_cast<MSG*>(message);
    // Never call winId() here: messages arrive *during* CreateWindowEx, and winId()
    // would re-enter QWidget::create() -> recursive CreateWindowEx failures -> crash.
    if (!msg || !msg->hwnd || msg->hwnd != reinterpret_cast<HWND>(internalWinId())) return QMainWindow::nativeEvent(eventType, message, result);
    HWND hwnd = msg->hwnd;
    switch (msg->message) {
    case WM_NCCALCSIZE:
        if (msg->wParam == TRUE) {
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
            if (IsZoomed(hwnd) && !isFullScreen() && !m_chromeHidden) {
                // A maximized window is placed with its resize border off-screen. Derive the client
                // rect from the *proposed* window rect (rgrc[0]) -- never replace it wholesale -- so
                // the client area can't be larger than / offset from the window if the size Windows
                // proposes is not the full monitor (snap/restore in a move loop, monitor change).
                const int fx = resizeBorder(hwnd, true), fy = resizeBorder(hwnd, false);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
                // Monitor of the proposed rect, not of the current window position.
                HMONITOR mon = MonitorFromRect(&p->rgrc[0], MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(mon, &mi)) {
                    RECT clipped;
                    if (IntersectRect(&clipped, &p->rgrc[0], &mi.rcWork)) p->rgrc[0] = clipped;
                    // Leave 1px for an auto-hide taskbar so it can still be revealed.
                    switch (autoHideTaskbarEdge(mon)) {
                    case ABE_BOTTOM: p->rgrc[0].bottom -= 1; break;
                    case ABE_TOP:    p->rgrc[0].top += 1; break;
                    case ABE_LEFT:   p->rgrc[0].left += 1; break;
                    case ABE_RIGHT:  p->rgrc[0].right -= 1; break;
                    default: break;
                    }
                }
            }
            *result = 0;          // whole window is client area (no native caption / borders drawn)
            return true;
        }
        break;
    case WM_NCHITTEST: {
        if (isFullScreen() || m_chromeHidden || m_fsTransition) { *result = HTCLIENT; return true; }
        const LONG x = GET_X_LPARAM(msg->lParam), y = GET_Y_LPARAM(msg->lParam);
        if (!IsZoomed(hwnd)) {
            // All 8 resize zones, DPI-scaled for the current monitor. Corners get a larger grab area
            // along each edge so there are no dead pixels at the rounded Win11 corners.
            RECT wr; GetWindowRect(hwnd, &wr);
            const int bx = resizeBorder(hwnd, true), by = resizeBorder(hwnd, false);
            const int cx = bx * 2, cy = by * 2;
            const bool l = x < wr.left + bx, r = x >= wr.right - bx, t = y < wr.top + by, b = y >= wr.bottom - by;
            const bool lc = x < wr.left + cx, rc = x >= wr.right - cx, tc = y < wr.top + cy, bc = y >= wr.bottom - cy;
            if ((t && lc) || (l && tc)) { *result = HTTOPLEFT; return true; }
            if ((t && rc) || (r && tc)) { *result = HTTOPRIGHT; return true; }
            if ((b && lc) || (l && bc)) { *result = HTBOTTOMLEFT; return true; }
            if ((b && rc) || (r && bc)) { *result = HTBOTTOMRIGHT; return true; }
            if (l) { *result = HTLEFT; return true; }
            if (r) { *result = HTRIGHT; return true; }
            if (t) { *result = HTTOP; return true; }
            if (b) { *result = HTBOTTOM; return true; }
        }
        const QPoint local = nativeToLocal(hwnd, x, y);
        // HTMAXBUTTON makes Windows 11 show the snap-layouts flyout when hovering maximize.
        if (overMaxButton(local)) { *result = HTMAXBUTTON; return true; }
        // Maximized: the empty caption is reported as client area, so Windows has nothing to drag --
        // for every input path (mouse, touch, pen, drag-to-restore, Aero shake). Double-click
        // (restore) and right-click (system menu) are handled in eventFilter instead.
        if (isCaptionArea(local)) { *result = IsZoomed(hwnd) ? HTCLIENT : HTCAPTION; return true; }
        *result = HTCLIENT;
        return true;
    }
    // The maximize button is non-client (HTMAXBUTTON), so drive its hover/pressed/click ourselves.
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSEHOVER:
        if (msg->wParam == HTMAXBUTTON) {
            if (!m_ncMaxHover) {
                TRACKMOUSEEVENT tme{}; tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE | TME_NONCLIENT; tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            setNcMaxHover(true);
            *result = 0; return true;
        }
        setNcMaxHover(false);
        break;
    case WM_NCMOUSELEAVE:
    case WM_MOUSEMOVE:
        setNcMaxHover(false);
        break;
    case WM_NCLBUTTONDOWN:
        // Window is locked while maximized: swallow the caption press so DefWindowProc never starts
        // a move loop (which would drag the window out of maximized / snap it). Double-click
        // (WM_NCLBUTTONDBLCLK) still restores, and the window stays draggable when restored.
        if (msg->wParam == HTCAPTION && moveLocked()) {
            SetForegroundWindow(hwnd);
            *result = 0; return true;
        }
        [[fallthrough]];
    case WM_NCLBUTTONDBLCLK:
        if (msg->wParam == HTMAXBUTTON) {
            m_ncMaxPressed = true;
            if (m_maxBtn) m_maxBtn->setForcedDown(true);
            *result = 0; return true;            // don't let DefWindowProc draw a classic button
        }
        break;
    case WM_NCLBUTTONUP:
        if (msg->wParam == HTMAXBUTTON) {
            const bool click = m_ncMaxPressed;
            m_ncMaxPressed = false;
            if (m_maxBtn) m_maxBtn->setForcedDown(false);
            if (click) animatedToggleMaximize();
            *result = 0; return true;
        }
        if (m_ncMaxPressed) { m_ncMaxPressed = false; if (m_maxBtn) m_maxBtn->setForcedDown(false); }
        break;
    case WM_NCRBUTTONDOWN:
        if (msg->wParam == HTCAPTION || msg->wParam == HTMAXBUTTON) { *result = 0; return true; }
        break;
    case WM_NCRBUTTONUP:
        if (msg->wParam == HTCAPTION || msg->wParam == HTMAXBUTTON) {
            showSystemMenu(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
            *result = 0; return true;
        }
        break;
    case WM_SYSCOMMAND:
        // No keyboard/system-menu "Move" or "Size" while maximized or fullscreen.
        if (((msg->wParam & 0xFFF0) == SC_MOVE || (msg->wParam & 0xFFF0) == SC_SIZE) && moveLocked()) { *result = 0; return true; }
        // Entering fullscreen from a minimized/maximized window via the system menu etc. is ours.
        // Alt+Space: show the system menu under the top-left of our caption.
        if ((msg->wParam & 0xFFF0) == SC_KEYMENU && msg->lParam == VK_SPACE) {
            RECT wr; GetWindowRect(hwnd, &wr);
            const int off = IsZoomed(hwnd) ? 0 : resizeBorder(hwnd, true);
            const qreal dpr = windowHandle() ? windowHandle()->devicePixelRatio() : 1.0;
            showSystemMenu(wr.left + off, wr.top + off + qRound(40 * dpr));
            *result = 0; return true;
        }
        break;
    case WM_ENTERSIZEMOVE: m_inSizeMove = true; m_sizeMoveLocked = moveLocked(); break;
    case WM_EXITSIZEMOVE:  m_inSizeMove = false; m_sizeMoveLocked = false; break;
    case WM_WINDOWPOSCHANGING:
        // Last line of defence: a move loop that *started* while maximized must not drag the window.
        // Only the position is pinned, and only while still zoomed. Never add SWP_NOSIZE here: Aero
        // Snap / Win+arrows maximize or restore from inside a normal move loop (IsZoomed() is already
        // true when that WINDOWPOS arrives), and blocking the size left a maximized-state window at
        // the old size, so WM_NCCALCSIZE / Qt laid out for a different rect (clipped, shifted UI).
        if (m_inSizeMove && m_sizeMoveLocked && IsZoomed(hwnd) && !isFullScreen() && !m_chromeHidden) {
            auto* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
            if (!(wp->flags & SWP_NOSIZE)) break;   // a real resize (maximize/restore/snap): let it through
            wp->flags |= SWP_NOMOVE;
        }
        break;
    case WM_STYLECHANGING:
        // Keep WS_CAPTION / WS_THICKFRAME / WS_MINIMIZEBOX / WS_MAXIMIZEBOX whenever we are not
        // fullscreen: they are what makes DWM play the native minimize / maximize / restore
        // animations (and Snap) on this frameless window. WM_NCCALCSIZE hides the caption itself.
        if (msg->wParam == WPARAM(GWL_STYLE) && !isFullScreen() && !m_chromeHidden && !m_fsTransition) {
            auto* ss = reinterpret_cast<STYLESTRUCT*>(msg->lParam);
            ss->styleNew |= WS_THICKFRAME | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
        }
        break;
    case WM_DWMCOMPOSITIONCHANGED:
    case WM_DPICHANGED:
    case WM_THEMECHANGED:
        QTimer::singleShot(0, this, [this]{ applyNativeFrame(); });
        break;
    case WM_ACTIVATE:
        if (m_minBtn) { m_minBtn->update(); m_maxBtn->update(); m_closeBtn->update(); }
        break;
    default: break;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

// ---------------- first-run welcome flow ----------------
void BrowserWindow::writeSetting(const QString& key, const QJsonValue& value) {
    auto s = readSettings(); s[key] = value;
    writeSettingsFile(s);
}

void BrowserWindow::crossFadeTheme(const std::function<void()>& apply) {
    if (!isVisible() || Motion::fadeMs(Motion::Medium) <= 0) { apply(); return; }   // reduced motion: plain 120 ms cross-fade
    auto* shot = new Motion::FadeOverlay(this);   // grab() snapshot faded with QPainter opacity
    shot->setObjectName(QStringLiteral("PacitumWindowOverlay"));
    shot->setPixmap(grab());
    shot->setGeometry(rect());
    shot->setOpacity(1.0); shot->show(); shot->raise();
    apply();
    shot->fade(1.0, 0.0, Motion::fadeMs(Motion::Medium), QEasingCurve::OutCubic, 0, {}, true);
}

void BrowserWindow::openWelcome() {
    if (!m_firstRun) {
        FirstRunBridge::Hooks h;
        h.setTheme = [this](const QString& mode) {
            const QString m = mode == QLatin1String("dark") ? QStringLiteral("Dark") : mode == QLatin1String("light") ? QStringLiteral("Light") : QStringLiteral("System");
            if (m == m_themeSetting) return;
            crossFadeTheme([this, m]{ m_themeSetting = m; m_darkMode = m == QLatin1String("Dark"); applyTheme(); });
            writeSetting(QStringLiteral("theme"), m);
        };
        h.setAccent = [this](const QString& hex) {
            if (hex.compare(m_accent, Qt::CaseInsensitive) == 0) return;
            crossFadeTheme([this, hex]{ m_accent = hex.toUpper(); applyTheme(); });
            writeSetting(QStringLiteral("accent"), hex.toUpper());
        };
        h.palette = [this]() {
            const auto& p = m_theme->palette();
            return QVariantMap{{QStringLiteral("page"), p.page}, {QStringLiteral("card"), p.toolbar}, {QStringLiteral("surf"), p.surface},
                {QStringLiteral("surfh"), p.surfaceHover}, {QStringLiteral("bord"), p.border}, {QStringLiteral("bord2"), p.borderStrong},
                {QStringLiteral("txt"), p.text}, {QStringLiteral("sec"), p.textSecondary}, {QStringLiteral("ter"), p.textTertiary},
                {QStringLiteral("acc"), p.accent}, {QStringLiteral("accs"), p.accentSoft}, {QStringLiteral("onacc"), p.onAccent},
                {QStringLiteral("dan"), p.danger}};
        };
        h.finish = [this]() {
            for (int i = 0; i < m_tabs->count(); ++i)
                if (auto* t = qobject_cast<BrowserTab*>(m_tabs->widget(i)); t && t->currentUrl().startsWith(QStringLiteral("pacitum://welcome")))
                    t->loadUrl(PacitumSchemeHandler::newTabUrl());
            endWelcome();
        };
        m_firstRun = new FirstRunBridge(m_bookmarks, m_history, h, this);
        connect(m_firstRun, &FirstRunBridge::importFinished, this, [this]{ if (m_bookmarksBar) m_bookmarksBar->refresh(); });
        m_firstRunChannel = new QWebChannel(this);
        m_firstRunChannel->registerObject(QStringLiteral("firstRun"), m_firstRun);
    }
    newTab(QStringLiteral("pacitum://welcome"));
    auto* tab = activeTab();
    if (!tab || !tab->view()) return;
    tab->view()->page()->setWebChannel(m_firstRunChannel);
    m_bookmarksBar->hide();   // spec: bookmarks bar hidden on the welcome page
    tab->view()->setFocus();  // Enter = primary / Esc = Back work right away
    QPointer<BrowserTab> guard(tab);
    // Only pacitum://welcome gets the bridge: detach as soon as the tab navigates anywhere else.
    connect(tab, &BrowserTab::urlChanged, this, [this, guard](const QString& u){
        if (!guard || u.startsWith(QStringLiteral("pacitum://welcome"))) return;
        guard->view()->page()->setWebChannel(nullptr);
        endWelcome();
    });
}

void BrowserWindow::endWelcome() {
    FirstRunBridge::markFirstRunDone();
    if (m_bookmarksBar && !m_bookmarksBar->isVisible())
        Motion::slideVisible(m_bookmarksBar, readSettings().value(QStringLiteral("show_bookmarks_bar")).toBool(true), 0, Motion::Base);
}


// ---------------------------------------------------------------------------------------------
// 1.6 home page bridge (home-profile-spec.md §1): data for pacitum://newtab and the
// pacitum://action/<name> calls it makes (theme, engine, open, downloads, history).
// ---------------------------------------------------------------------------------------------
static QPointer<BrowserWindow> s_homeWindow;

void BrowserWindow::setupHome() {
    s_homeWindow = this;
    auto* h = PacitumSchemeHandler::instance();
    h->setHomeDataProvider([]{ return s_homeWindow ? s_homeWindow->homeData() : QStringLiteral("{}"); });
    connect(h, &PacitumSchemeHandler::homeAction, this, [this](const QString& n, const QMap<QString, QString>& a){
        if (s_homeWindow && s_homeWindow != this) return;   // one window handles each call
        handleHomeAction(n, a);
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now){ if (now && now->window() == this) s_homeWindow = this; });
}

QString BrowserWindow::homeData() {
    QJsonObject o;
    // Greeting name: the active profile's name (per profile); none in Guest/private or for an unnamed "Default".
    auto* pm = ProfileManager::instance();
    QString name;
    if (!pm->isGuest()) {
        name = pm->current().name.trimmed();
        if (name.isEmpty() || (pm->current().id == QStringLiteral("default") && name == QStringLiteral("Default")))
            name = readSettings().value(QStringLiteral("profile_name")).toString().trimmed();
        if (name == QStringLiteral("Default")) name.clear();
    }
    o["first"] = m_privateMode ? QString() : name.section(QLatin1Char(' '), 0, 0);
    o["dark"] = m_darkMode;
    o["reduced"] = Pacitum::reducedMotion();
    o["theme"] = m_themeSetting;
    o["engine"] = m_searchEngine;
    o["engines"] = QJsonObject{{"Google", "https://www.google.com/search?q="}, {"Bing", "https://www.bing.com/search?q="},
                               {"DuckDuckGo", "https://duckduckgo.com/?q="}, {"Yahoo", "https://search.yahoo.com/search?p="},
                               {"Brave", "https://search.brave.com/search?q="}};
    o["search"] = QStringLiteral("https://www.google.com/search?q=");
    // Continue reading: recently closed tabs first, then recent history (internal pages skipped).
    QJsonArray recent; QSet<QString> seen;
    auto addRecent = [&](const QString& u, const QString& t, double at){
        if (recent.size() >= 5 || u.isEmpty() || seen.contains(u) || !(u.startsWith(QLatin1String("http://")) || u.startsWith(QLatin1String("https://")))) return;
        seen.insert(u); recent.append(QJsonObject{{"u", u}, {"t", t}, {"at", at}});
    };
    for (int i = m_recentlyClosed.size() - 1; i >= 0; --i) addRecent(m_recentlyClosed[i].url, m_recentlyClosed[i].title, 0);
    if (m_history && !m_privateMode) for (const auto& e : m_history->recent(20)) addRecent(e.url, e.title, e.visitTime);
    o["recent"] = recent;
    QJsonArray top;
    if (m_history) for (const auto& e : m_history->topSites(8)) if (e.url.startsWith(QLatin1String("http"))) top.append(QJsonObject{{"u", e.url}, {"t", e.title}});
    o["top"] = top;
    QJsonArray dls; int n = 0;
    if (m_downloads) for (const auto& d : m_downloads->entries()) {
        if (n++ >= 3) break;
        const double p = d.active() ? (d.total > 0 ? double(d.received) / double(d.total) : 0.0) : -1.0;
        dls.append(QJsonObject{{"n", d.fileName}, {"p", p}, {"s", d.state == DownloadEntry::Completed ? DownloadManager::formatSize(d.total > 0 ? d.total : d.received)
                                                         : d.state == DownloadEntry::Failed ? QStringLiteral("Failed") : d.state == DownloadEntry::Cancelled ? QStringLiteral("Cancelled") : QString()}});
    }
    o["downloads"] = dls;
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void BrowserWindow::handleHomeAction(const QString& name, const QMap<QString, QString>& args) {
    const QString v = args.value(QStringLiteral("v"));
    if (name == QStringLiteral("theme") && (v == QLatin1String("System") || v == QLatin1String("Light") || v == QLatin1String("Dark"))) {
        writeSetting(QStringLiteral("theme"), v);
        crossFadeTheme([this]{ onSettingChanged(QStringLiteral("theme")); });
    } else if (name == QStringLiteral("engine") && QStringList{"Google", "Bing", "DuckDuckGo", "Yahoo", "Brave"}.contains(v)) {
        writeSetting(QStringLiteral("search_engine"), v);
        onSettingChanged(QStringLiteral("search_engine"));
    } else if (name == QStringLiteral("open")) {
        const QUrl u(args.value(QStringLiteral("url")));
        if (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https")) newTab(u.toString());
    } else if (name == QStringLiteral("downloads")) {
        showDownloads();
    } else if (name == QStringLiteral("history")) {
        showHistory();
    }
}


// ---------------------------------------------------------------- 1.7.5 site permissions
void BrowserWindow::wirePermissions(BrowserTab* tab) {
    auto* pc = tab->permissions();
    if (!pc) return;
    QPointer<BrowserTab> guard(tab);
    pc->anchor = [this]{ return m_addressBar->bubbleAnchor(); };
    pc->canPrompt = [this, guard]{ return guard && guard == activeTab() && isActiveWindow() && !isMinimized(); };
    pc->favicon = [guard]{ return guard ? guard->icon() : QIcon(); };
    pc->shareableTabs = [this, guard]{
        QList<PermUi::ScreenSharePicker::TabSource> out;
        for (auto* tw : {m_tabs, m_splitTabs}) if (tw) for (int i = 0; i < tw->count(); ++i) {
            auto* t = qobject_cast<BrowserTab*>(tw->widget(i));
            if (!t || t == guard || !t->permissions() || t->isLazy()) continue;
            if (t->currentUrl().startsWith(QStringLiteral("pacitum"))) continue;
            QPointer<PermissionController> p(t->permissions());
            out << PermUi::ScreenSharePicker::TabSource{t->permissions()->tabToken(), t->currentTitle(), t->icon(),
                                                      [p]{ return p ? p->grabFrame() : QPixmap(); }};
        }
        return out;
    };
    connect(pc, &PermissionController::stateChanged, this, [this, guard]{ if (guard) refreshPermissionUi(guard); });
    connect(pc, &PermissionController::openUrlRequested, this, [this](const QString& u){ newTab(u); });
    connect(pc, &PermissionController::mouseLockHint, this, [this]{ Motion::bubble(this, QStringLiteral("Press Esc to show your cursor"), 1700, 90); });
    connect(pc, &PermissionController::siteSettingsRequested, this, [this](const QString& origin){
        showSettings();
        for (int i = 0; i < m_tabs->count(); ++i) if (auto* sp = qobject_cast<SettingsPage*>(m_tabs->widget(i))) sp->showSiteDetails(origin);
    });
}

void BrowserWindow::refreshPermissionUi(BrowserTab* tab) {
    if (!tab || !tab->permissions()) return;
    auto* pc = tab->permissions();
    for (auto* tw : {m_tabs, m_splitTabs}) if (tw) if (int i = tw->indexOf(tab); i >= 0) tw->setTabUsage(i, pc->tabUsage());
    if (tab == activeTab()) m_addressBar->setPermissionChips(pc->chips());
    updateRecordingIndicator();
}

// §5 window level: "● " title prefix and (Windows) a red taskbar overlay while any tab captures.
void BrowserWindow::updateRecordingIndicator() {
    bool capturing = false;
    for (auto* tw : {m_tabs, m_splitTabs}) if (tw) for (int i = 0; i < tw->count(); ++i)
        if (auto* t = qobject_cast<BrowserTab*>(tw->widget(i)); t && t->permissions() && t->permissions()->capturing()) capturing = true;
    const QString dot = QStringLiteral("\u25CF ");
    QString title = windowTitle();
    const bool has = title.startsWith(dot);
    if (capturing != has) {
        m_titleGuard = true;
        setWindowTitle(capturing ? dot + title : title.mid(dot.size()));
        m_titleGuard = false;
    }
#ifdef Q_OS_WIN
    if (capturing != m_overlayShown && internalWinId()) {
        m_overlayShown = capturing;
        ITaskbarList3* tb = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, reinterpret_cast<void**>(&tb))) && tb) {
            tb->HrInit();
            HICON icon = nullptr;
            if (capturing) {
                QPixmap pm = ThemeManager::icon(QStringLiteral("recording-dot"), ThemeManager::current().danger, 16).pixmap(16, 16);
                icon = pm.toImage().toHICON();
            }
            tb->SetOverlayIcon(reinterpret_cast<HWND>(internalWinId()), icon, capturing ? L"Camera or microphone in use" : L"");
            if (icon) DestroyIcon(icon);
            tb->Release();
        }
    }
#endif
}
