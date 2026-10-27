#pragma once
#include <QTabWidget>
#include <QTabBar>
#include <QPushButton>
#include <QToolButton>
#include <QSet>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <functional>
#include <QAbstractButton>
#include <QVariantAnimation>
#include <QElapsedTimer>

class AnimatedToolButton;

class ThemeManager;

class PacitumTabBar;

// 22 px circular close button used by floating tabs (spec §5.1).
class TabCloseButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit TabCloseButton(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {22, 22}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent* e) override { update(); QAbstractButton::enterEvent(e); }
    void leaveEvent(QEvent* e) override { update(); QAbstractButton::leaveEvent(e); }
};

class PacitumTabBar : public QTabBar {
    Q_OBJECT
public:
    static void setCompact(bool on);   // 2.0.1 Settings › Tab size
    void setSpinnerSuspended(bool on);   // pause the loading-spinner animation while the window is minimized/hidden
    // Pacitum 1.5 floating-tab look (custom painted; used for the window's tab strip).
    void enableFloatingTabs();
    bool floating() const { return m_floating; }
    void setTabLoading(int index, bool on);
    void setTabWaiting(int index, bool on);   // before first byte: reversed, tertiary arc (spec §6)
    void setTabAudio(int index, bool audible, bool muted);
    void setTabCrashed(int index, bool on);
    // 1.7.5 in-use indicator (permissions-spec §5): 0 none, 1 location, 2 screen share, 3 recording.
    void setTabUsage(int index, int usage);
    void paintFloatingTab(QPainter* p, const class QStyleOptionTab& opt) const;
    QRect closeButtonRect(const QRect& tabRect) const;
    int normalTabWidth() const;
    explicit PacitumTabBar(bool vertical = false, QWidget* parent = nullptr);
    void setVerticalMode(bool v);
    bool isTabPinned(int index) const { return m_pinned.contains(index); }
    void setTabPinned(int index, bool pinned);
    void setAvailableWidth(int w);
    // Width animations (150 ms): grow a freshly inserted tab / shrink before removal.
    void animateIn(int index);
    void animateOut(int index, std::function<void()> done);
    bool animationsEnabled = true;

protected:
    QSize tabSizeHint(int index) const override;
    QSize minimumTabSizeHint(int index) const override;

signals:
    void tabRightClicked(int index, const QPoint& globalPos);
    void tabMuteToggled(int index, bool muted);
    void emptySpaceRightClicked(const QPoint& globalPos);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void tabLayoutChange() override;
    void paintEvent(QPaintEvent* e) override;

private:
    void startSelectionSlide();
    void cancelSlide();                       // snap the active shape to its tab
    void markStructChange();                  // open / close / move: no glide for a moment
    QElapsedTimer m_structTimer;
    bool m_structSeen = false;
    bool m_dragging = false;                  // a tab is being dragged
    QPoint m_pressPos;
    mutable bool m_selPainted = false;        // the active tab was painted in this paint event
    QVariantAnimation* m_slideAnim = nullptr;
    QRectF m_slideFrom;
    qreal m_slideT = 1.0;                     // 1 = no slide running
    mutable QRectF m_lastSelBody;             // where the active-tab shape was last drawn
    struct TabFlags { bool loading = false, waiting = false, audible = false, muted = false, crashed = false; int usage = 0; qint64 usageSince = 0; };
    class QTimer* m_usagePulse = nullptr;
    quint64 ensureId(int index);
    void setHoverIndex(int i);
    void updateCloseButtons();
    void updateSpinner();
    QRect audioRect(int index) const;
    bool m_floating = false;
    QHash<quint64, TabFlags> m_flags;
    QHash<quint64, qreal> m_hover;            // per-tab hover level 0..1 (120/160 ms fades)
    QHash<quint64, QVariantAnimation*> m_hoverAnim;
    quint64 m_hoverId = 0;
    QVariantAnimation* m_spin = nullptr;
    QElapsedTimer m_spinTick;                 // spinner repaints are limited to ~30 a second
    bool m_spinSuspended = false;
    qreal m_spinAngle = 0;
    qreal m_spinV = 0;                        // shared spinner phase 0..1 (1600 ms loop)
    QHash<quint64, qreal> m_favIn;            // favicon fade-in after load (160 ms)
    bool m_inUpdate = false;
    int m_frozenWidth = 0;                    // keep widths while closing with the mouse (Chrome-like)
    quint64 idOf(int index) const;
    int indexOfId(quint64 id) const;
    void relayout();
    bool m_vertical = false;
    QSet<int> m_pinned;
    int m_avail = 0;
    quint64 m_nextId = 1;
    QHash<quint64, qreal> m_progress;
    QHash<quint64, QPair<int, qreal>> m_pinMorph;   // pin/unpin width morph: from width, t
};

// QTabWidget whose (horizontal) tabs can be shown in a separate "TabStrip" widget placed
// above the navigation toolbar, as in the Pacitum design (TabStrip 40 → Toolbar 44 → Bookmarks 32).
class PacitumTabWidget : public QTabWidget {
    Q_OBJECT
public:
    explicit PacitumTabWidget(bool vertical = false, QWidget* parent = nullptr);
    void setVertical(bool v);
    bool isVertical() const { return m_vertical; }
    void setTabPinned(int index, bool pinned);
    void applyTheme(ThemeManager* tm);

    // Creates the external tab strip (call once, for the main tab widget).
    QWidget* createTabStrip(QWidget* parent);
    QHBoxLayout* stripLayout() const { return m_stripLayout; }
    QTabBar* stripBar() const { return m_ext; }
    QWidget* stripLogo() const { return nullptr; }   // 1.5: no standalone logo in the strip
    QWidget* stripNewTabButton() const;
    void setTabLoading(int index, bool on);
    void setTabWaiting(int index, bool on);
    void setTabAudio(int index, bool audible, bool muted);
    void setTabCrashed(int index, bool on);
    // 1.7.5 in-use indicator (permissions-spec §5): 0 none, 1 location, 2 screen share, 3 recording.
    void setTabUsage(int index, int usage);
    void showTabSearch() { showTabList(); }
    bool hasFloatingStrip() const { return m_ext && !m_vertical; }
    // Animate the tab closed (if it lives in the strip), then call done().
    void animateRemove(int index, std::function<void()> done);

    // Shadow QTabWidget setters so the external strip stays in sync.
    void setTabText(int index, const QString& text);
    void setTabIcon(int index, const QIcon& icon);
    void setTabToolTip(int index, const QString& tip);

signals:
    void newTabRequested();
    void tabSplitRequested(int index);
    void tabDuplicateRequested(int index);
    void tabReloadRequested(int index);
    void tabMuteRequested(int index, bool muted);
    void tabPinRequested(int index, bool pinned);
    void tabReopenRequested();
    void stripDoubleClicked();

protected:
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private slots:
    void showTabContextMenu(int index, const QPoint& pos);
    void showBarContextMenu(const QPoint& pos);
    void showTabList();

private:
    void closeOtherTabs(int index);
    void closeTabsToRight(int index);
    void updateCorner();
    void updateBarVisibility();
    void updateTabWidths();
    PacitumTabBar* activeBar() const { return m_ext ? m_ext : m_bar; }

    bool m_vertical = false;
    bool m_syncing = false;
    PacitumTabBar* m_bar = nullptr;
    PacitumTabBar* m_ext = nullptr;
    QWidget* m_strip = nullptr;
    QToolButton* m_newTabBtn = nullptr;
    AnimatedToolButton* m_stripNewTab = nullptr;
    AnimatedToolButton* m_tabListBtn = nullptr;
    QHBoxLayout* m_stripLayout = nullptr;
    QLabel* m_logo = nullptr;
};
