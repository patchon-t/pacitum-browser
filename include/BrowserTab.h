#pragma once
#include <QWidget>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QProgressBar>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <functional>
#include <QFrame>
#include <QUrl>
#include <QPointer>
#include <QSet>
#include <QPixmap>
#include <QWebEngineFullScreenRequest>
class QLabel;
class QTimer;
class QToolButton;

class FindBar;
class PermissionController;
#include "Passwords.h"
namespace Motion { class FadeOverlay; }

class BrowserTab : public QWidget {
    Q_OBJECT
public:
    explicit BrowserTab(const QString& url = QString(),
                        bool privateMode = false,
                        const QString& searchEngine = QStringLiteral("Google"),
                        QWidget* parent = nullptr);

    void loadUrl(const QString& rawUrl);
    QString currentUrl() const { return m_url; }
    QString currentTitle() const { return m_title; }
    void goBack();
    void goForward();
    void reload();
    void stop();
    bool canGoBack() const;
    bool canGoForward() const;
    void setZoomFactor(double factor);
    double zoomFactor() const { return m_zoom; }
    void setMuted(bool muted);
    bool isMuted() const;
    QIcon icon() const { return m_view->icon(); }
    bool isLoading() const { return m_loading; }
    int loadProgressValue() const { return m_loadValue; }
    void applyWebSettings();
    void setSearchEngine(const QString& e) { m_searchEngine = e; }
    QWebEngineView* view() const { return m_view; }
    enum LinkTarget { NewTab = 0, NewWindow = 1, PrivateTab = 2, SplitView = 3 };
    QString searchUrl(const QString& terms) const;   // search-engine results URL for plain text
    ~BrowserTab() override;
    // 1.8.5 tools
    void togglePictureInPicture();       // pop the page's main video out into an always-on-top window
    bool inPictureInPicture() const { return !m_pip.isNull(); }
    QPixmap captureView() const;         // visible page pixels (for screenshots / 3D effects)
    QRect viewRectIn(QWidget* w) const;  // the page area in w's coordinates
    void fadeIn();
    void navTransition(int dir);   // -1 back, +1 forward
    void ensureActive();                 // thaw a frozen page before touching it
    void setNoFreeze(bool on);           // e.g. while the page is fullscreen
    void setWindowMinimized(bool on);    // hide the page from Chromium / freeze idle background tabs sooner
    bool isPrivate() const { return m_private; }
    static bool s_freezeEnabled;         // settings: "freeze_background_tabs"
    // Session restore: remember the URL/title but only create the page load when first shown.
    void setLazyUrl(const QString& url, const QString& title);
    bool isLazy() const { return !m_pendingUrl.isEmpty(); }
    static std::function<BrowserTab*(int)> s_popupFactory;   // QWebEnginePage::WebWindowType
    void showFindBar();
    void refreshTheme();   // re-tint native chrome inside the tab (info bar)
    void hideFindBar();
    PermissionController* permissions() const { return m_perms; }
    int tabId() const { return m_tabId; }
    bool hasInternalChannel() const;

signals:
    void passwordOffer(const Credential& c, bool update);   // 2.0.1 "Save password?"
    void addressOffer(const AutofillAddress& a);
    void urlChanged(const QString& url);
    void titleChanged(const QString& title);
    void loadingChanged(bool loading);
    void iconChanged();
    void progressChanged(int progress);
    void loadDone(bool ok);
    void crashed();
    void fullScreenRequested(QWebEngineFullScreenRequest request);
    // Page context menu (1.9): open a URL somewhere else. Handled by BrowserWindow.
    void openLinkRequested(const QUrl& url, int target);
    void notice(const QString& text);    // short message for the window to show (bubble)

private:
    int m_loadValue = 0;
private slots:
    void onUrlChanged(const QUrl& url);
    void onTitleChanged(const QString& title);
    void onLoadStarted();
    void onLoadProgress(int p);
    void onLoadFinished(bool ok);
    void onRenderCrashed(QWebEnginePage::RenderProcessTerminationStatus status, int code);

private:
    void buildSignInBar();
    void showContextMenu(const QPoint& globalPos);
    void endPictureInPicture();
    QPointer<class PipWindow> m_pip;
    QWidget* m_pipPlaceholder = nullptr;
    int m_viewLayoutIndex = -1;
    QSet<QString> m_preconnected;        // origins warmed up by hovering links on this page (1.8.8)
    void showSignInBar(const QUrl& target);
    void checkGoogleSignIn(const QUrl& url);
    class InfoBar* m_signInBar = nullptr;
    QUrl m_signInUrl;
    QString m_pendingUrl;
protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
private:
    static int s_idCounter;
    int m_tabId = 0;
    bool m_private = false;
    QString m_searchEngine;
    QString m_url;
    QString m_title = QStringLiteral("New Tab");
    double m_zoom = 1.0;
    bool m_loading = false;

    QWebEngineView* m_view = nullptr;
    QWebEngineProfile* m_profile = nullptr;
    QWebEnginePage* m_page = nullptr;
    PermissionController* m_perms = nullptr;
    QProgressBar* m_progress = nullptr;
    FindBar* m_findBar = nullptr;
    Motion::FadeOverlay* m_navShot = nullptr;   // back/forward snapshot (navTransition), reused
    Motion::FadeOverlay* m_veil = nullptr;      // page-transition veil (fadeIn), reused
    QTimer* m_freezeTimer = nullptr;
    bool m_noFreeze = false;
    int m_crashCount = 0;
    qint64 m_lastCrashMs = 0;
    static constexpr int kFreezeAfterMs = 10 * 60 * 1000;
    static constexpr int kFreezeMinimizedMs = 60 * 1000;
    // Frozen pages still hold their renderer memory. After a long time in the background a frozen
    // tab is discarded (renderer memory released; reloaded from history when shown again).
    static constexpr int kDiscardAfterMs = 30 * 60 * 1000;
    QTimer* m_discardTimer = nullptr;
    void maybeDiscard();
public:
    static bool s_discardEnabled;        // settings: "discard_background_tabs"
    bool isDiscarded() const;
private:
    bool m_windowMinimized = false;
    void maybeFreeze();
};
