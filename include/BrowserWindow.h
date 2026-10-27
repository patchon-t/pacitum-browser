#pragma once
#include <functional>
#include <QJsonValue>
#include <QMainWindow>
#include <QSplitter>
#include <QPushButton>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QLabel>
#include <QJsonObject>
#include <QWebEngineDownloadRequest>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QSet>
#include <QPointer>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QHash>
#include <QPixmap>

class PacitumTabWidget;
class BrowserTab;
class AddressBar;
class BookmarksBar;
class ThemeManager;
class HistoryManager;
class BookmarkManager;
class DownloadManager;
class SessionManager;
struct SessionTab;
class ZoomController;
class FirstRunBridge;
class QWebChannel;
class AdBlocker;
class WindowButton;

class BrowserWindow : public QMainWindow {
    Q_OBJECT
public:
    void openUrlInNewTab(const QString& url) { newTab(url); }   // command-line / default-browser launches
    explicit BrowserWindow(QWidget* parent = nullptr);
    ~BrowserWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
private:
    void applyNativeFrame();                       // styles, DWM shadow, Win11 rounded corners
    void showSystemMenu(int physX, int physY);     // right-click on caption / Alt+Space
    QPoint nativeToLocal(void* hwnd, long x, long y) const;
    bool overMaxButton(const QPoint& local) const;
    void setNcMaxHover(bool on);
    bool m_ncMaxHover = false;
    bool m_ncMaxPressed = false;
    bool m_inSizeMove = false;
    bool m_sizeMoveLocked = false;                 // move loop started while maximized/fullscreen
    bool moveLocked() const;                       // maximized / fullscreen: no caption drag, no move/size
    void ensureFullscreenCoversMonitor();
protected:
#endif
    void keyPressEvent(QKeyEvent* e) override;

private slots:
    void newTab(const QString& url = {});
    void newPrivateTab();
    void openLink(const QUrl& url, int target);   // page context menu targets (BrowserTab::LinkTarget)
    void closeTab(int index);
    void closeSplitTab(int index);
    void onTabChanged(int index);
    void wirePermissions(class BrowserTab* tab);
    void refreshPermissionUi(class BrowserTab* tab);
    void updateRecordingIndicator();
    void navigateToUrl(const QString& url);
    void updateAddressBar(const QString& url);
    void updateTabTitle(const QString& title);
    void onTabLoading(bool loading);
    void goBack();
    void goForward();
    void reloadPage();
    void goHome();
    void toggleBookmark();
    void showDownloads();
    void showSettings(const QString& section = QString());
    void showMenu();
    void toggleDarkMode();
    void toggleBookmarksBar();
    void toggleVerticalTabs();
    void toggleSplitView(int index = -1);
    // 1.8.5 split view
    void openSplit(int index);           // index < 0: new tab on the right
    void closeSplit();
    void moveTabToSplit(int index);      // main → right pane (opens split if needed)
    void moveTabFromSplit(int index);    // right pane → main (closes split when it empties)
    void swapSplitSides();
    void splitCrossFade(const std::function<void()>& change);
    void installExtensionFromFolder();
    // 1.8.5 tools + 3D
    void toggleNotes();
    void takeScreenshot();
    void showPageQr();
    void togglePictureInPicture();
    void showTabOverview();
    void captureActiveThumb();
    void showNotice(const QString& text);
    void setupSync();
    void duplicateTab(int index);
    void muteTab(int index, bool muted);
    void pinTab(int index, bool pinned);
    void reopenClosedTab();
    void saveSessionDialog();
    void onDownloadRequested(QWebEngineDownloadRequest* dl);
    void copyCurrentUrl();
    void showPageInfo();
    void showHistory();
    void clearHistory();
    void showBookmarksDialog();
    void importBookmarks();
    void toggleFullscreen();
    void focusAddressBar();
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void showFind();
    void handleEscape();
    void nextTab();
    void prevTab();
    void lastTab();
    void onSettingChanged(const QString& key);
    void clearBrowsingData(bool history, bool cookies, bool cache);
    void tickSpinner();
    void showDevTools();
    void showExtensionsMenu();
    void toggleReaderMode();
    void translatePage(const QString& targetLang);

private:
    void setupUI();
    void createToolbar();
    void createMenuBar();
    void createStatusBar();
    void setupShortcuts();
    void setupAdBlocker();
    void setupDownloadHandler();
    void applyTheme();
    void applyToolbarTheme();
    void loadSettings();
    void saveSettings();
    void restoreOrNewTab();
    BrowserTab* activeTab() const;
    QJsonObject readSettings() const;
    void writeSettingsFile(const QJsonObject& s);
    // fullscreen (website-requested or F11): chrome hidden, previous state restored on exit
    void enterContentFullscreen(bool fromPage);
    void exitContentFullscreen();
    void finishExitFullscreen();       // restore chrome + window state now (no page round-trip)
    bool m_fsTransition = false;       // entering fullscreen: ignore intermediate state reports
    bool m_shownOnce = false;
    bool m_titleGuard = false, m_overlayShown = false;
    void fadeInContent();
    void fadeVeil(int duration, qreal from, int delay);
    void showZoomBubble();
    void flyToDownloads();
    // built-in updater
    void setupUpdater();
    void animatedMinimize();          // 1.9.0 window animations (ghost on macOS / Linux)
    void animatedToggleMaximize();
    void showUpdatePrompt(const struct UpdateInfo& info);
    class Updater* m_updater = nullptr;
    class UpdatePill* m_updatePill = nullptr;   // toolbar: "Update" / download ring / "Restart"
    bool m_pendingUpdatePrompt = false;
    bool m_noBackgroundUpdates = false;
    // 1.6 home page bridge
    void setupHome();
    void handleHomeAction(const QString& action, const QMap<QString, QString>& args);
    QString homeData();
public:
    void runPerfTest();
    void runStressTest();              // --stress-test (CI): tabs, switching, fullscreen, loads
    void runResizeTest();              // --resize-test (CI): window/client/web view geometry after resizes
private:
    void setChromeHidden(bool hidden);
    bool m_pageFullscreen = false;     // a page (e.g. YouTube) owns fullscreen
    bool m_chromeHidden = false;
    Qt::WindowStates m_preFullscreenState;
    QPointer<BrowserTab> m_fullscreenTab;
    QList<QPointer<QWidget>> m_hiddenChrome;
    QToolButton* makeToolBtn(const QString& tip);
    void refreshIcons();
    void installPopupFactory();
    bool isCaptionArea(const QPoint& local) const;
    Qt::Edges edgesAt(const QPoint& global) const;
    void updateTabIcon(BrowserTab* tab);
    void updateTabAudio(BrowserTab* tab);
    void updateToolbarOverflow();
    bool m_downloadsWanted = false;
    void updateNavState();

    bool m_darkMode = false;
    QString m_themeSetting = QStringLiteral("System");
    QString m_accent;
    bool m_privateMode = false;
    bool m_splitActive = false;
    bool m_verticalTabs = false;
    QString m_searchEngine = QStringLiteral("Google");

    HistoryManager* m_history = nullptr;
    BookmarkManager* m_bookmarks = nullptr;
    DownloadManager* m_downloads = nullptr;
    class DownloadsPopup* m_downloadsPopup = nullptr;
    class DownloadButtonBadge* m_downloadsBadge = nullptr;
    void toggleDownloadsPopup(bool autoClose);
    // 2.0: a second click on the button whose pop-up is open closes it (the closing click used to
    // reach the button again and reopen the pop-up, which blinked).
    bool popupJustClosedBy(QWidget* button) const;
    void notePopupClosed(QWidget* button);
    QHash<QWidget*, qint64> m_popupClosedAt;
    void updateDownloadsButton();
    QIcon downloadsTabIcon() const;
    ThemeManager* m_theme = nullptr;
    SessionManager* m_session = nullptr;
    ZoomController* m_zoom = nullptr;
    AdBlocker* m_adblock = nullptr;

    QWidget* m_toolbar = nullptr;
    QToolButton* m_backBtn = nullptr;
    QToolButton* m_forwardBtn = nullptr;
    QToolButton* m_reloadBtn = nullptr;
    QToolButton* m_homeBtn = nullptr;
    AddressBar* m_addressBar = nullptr;
    QToolButton* m_splitBtn = nullptr;
    // 1.9.0 Chrome extensions: pinned extension buttons + "Add to Pacitum" on Chrome Web Store pages
    QWidget* m_extPins = nullptr;
    class QPushButton* m_storeBtn = nullptr;
    void rebuildExtensionPins();
    void updateStoreButton(const QString& url);
    QToolButton* m_profileBtn = nullptr;
    QLabel* m_profileAvatar = nullptr;
    void refreshProfileAvatar();
    void showProfileSwitcher();
    void showProfileManager();
    QWidget* m_tabStrip = nullptr;
    WindowButton* m_minBtn = nullptr;
    WindowButton* m_maxBtn = nullptr;
    WindowButton* m_closeBtn = nullptr;
    QTimer* m_spinTimer = nullptr;
    int m_spinAngle = 0;
    QTimer* m_sessionTimer = nullptr;              // debounced crash-safe session save (worker thread)
    void scheduleSessionSave();
    bool m_restoring = false;
    void tabActivated(class PacitumTabWidget* tw, int index);
    QVector<SessionTab> sessionTabs() const;
    bool m_animations = true;
    // First-run welcome flow (firstrun-spec.md)
    FirstRunBridge* m_firstRun = nullptr;
    QWebChannel* m_firstRunChannel = nullptr;
    // 2.0.1 Settings page (pacitum://settings)
    class SettingsBridge* m_settingsBridge = nullptr;
    FirstRunBridge* m_importer = nullptr;   // Settings › Import
    bool m_backgroundClose = false;         // downloads finished after the window was closed
    QWebChannel* m_settingsChannel = nullptr;
    void setupSettingsBridge();
    void attachInternalChannel(class BrowserTab* tab, const QString& url);
    int importBookmarksFile(const QString& path, int* found = nullptr);   // -1: unreadable
    void restartBrowser();
    void openWelcome();
    void endWelcome();
    void writeSetting(const QString& key, const QJsonValue& value);
    void crossFadeTheme(const std::function<void()>& apply);   // 200 ms snapshot fade (spec "Theme change")
    bool m_edgeCursor = false;
    QSet<QWidget*> m_closing;
    QLabel* m_privateBadge = nullptr;
    QToolButton* m_downloadsBtn = nullptr;
    QToolButton* m_menuBtn = nullptr;

    BookmarksBar* m_bookmarksBar = nullptr;
    class PacitumSplitter* m_splitter = nullptr;
    class NotesPanel* m_notes = nullptr;
    QWidget* m_contentRow = nullptr;
    QHash<QObject*, QPixmap> m_thumbs;   // last snapshot per tab (3D tab overview), ~640 px wide
    QTimer m_thumbTimer;
    PacitumTabWidget* m_tabs = nullptr;
    PacitumTabWidget* m_splitTabs = nullptr;
    PacitumTabWidget* m_activeWidget = nullptr;

    QLabel* m_zoomIndicator = nullptr;
    QGraphicsOpacityEffect* m_toolbarOpacity = nullptr;
    QPropertyAnimation* m_toolbarAnim = nullptr;

    struct ClosedTab { QString url; QString title; };
    QVector<ClosedTab> m_recentlyClosed;
};
