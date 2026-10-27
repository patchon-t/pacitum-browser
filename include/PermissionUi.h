#pragma once
// Pacitum 1.7.5 permission UI (permissions-spec.md §1-§9): the prompt bubble, device chooser
// list, screen-share picker and the site controls panel.
#include <QWidget>
#include <QPointer>
#include <QVariantAnimation>
#include <QElapsedTimer>
#include <QIcon>
#include <functional>
#include <QJsonObject>
#include "SitePermissions.h"
class QLabel;
class QPushButton;
class QVBoxLayout;
class QListWidget;
class QListWidgetItem;
class QAbstractListModel;
class QTimer;
class QToolButton;

// Omnibox chip model (§1 request chip, §4 quiet chip, §5 in-use chip, §6 blocked chip, §9 warning).
struct OmniChip {
    QString id;        // "request", "quiet", "inuse", "blocked:<kind>", "popups", "osdenied"
    QString glyph;
    QString color;     // token value
    QString text;      // optional label (quiet chip)
    bool dot = false;  // red recording dot
    int badge = 0;     // "+1" queued
    bool soft = false; // accent-soft background (request chip)
    QStringList glyphs;// several glyphs (in-use chip)
};

namespace PermUi {

// One bubble component for every request (§1). Frameless translucent top-level, non-modal.
class Bubble : public QWidget {
    Q_OBJECT
public:
    struct Action { QString id, text; bool primary = false; bool enabled = true; QString icon; };
    explicit Bubble(QWidget* window);
    void setHeader(const QIcon& favicon, const QString& richQuestion, const QString& plainForA11y);
    void addCapability(const QString& glyph, const QString& label, const QString& detail = QString(), bool warning = false);
    void addWidget(QWidget* w);                        // extra content (device list, warning box, url list)
    void addNote(const QString& text, bool warning = false);
    void setActions(const QList<Action>& actions, bool row = false, const QString& focusId = QString());
    void setActionEnabled(const QString& id, bool on);
    void setActionText(const QString& id, const QString& text);
    void setWidthHint(int w) { m_width = w; }
    void popup(const QPoint& anchorTopLeft, bool takeFocus);   // §1 appear: 160 ms OutCubic (reduced: opacity 100 ms)
    void dismiss();                                    // 120 ms InCubic, then deleteLater
    static int clickGuardMs() { return 500; }          // §10 anti-clickjacking
signals:
    void action(const QString& id);
    void closed(bool byUser);                          // Esc / close / outside → "dismiss" (ask again)
protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void restyle();
    QVBoxLayout* m_body = nullptr;
    QWidget* m_actionsBox = nullptr;
    QList<QPushButton*> m_buttons;
    QLabel* m_question = nullptr; QLabel* m_favicon = nullptr; QToolButton* m_close = nullptr;
    QElapsedTimer m_armed;
    QVariantAnimation m_anim;
    QPoint m_target;
    bool m_closing = false;
    int m_width = 300;
    QString m_a11y;
};

// Device chooser list (§1 table: rows h44, selected accent-soft, meta right, searching row).
class DeviceList : public QWidget {
    Q_OBJECT
public:
    explicit DeviceList(QWidget* parent = nullptr);
    void setDevices(const QList<QJsonObject>& devs);    // {id, name, meta}
    void setSearching(bool on);
    QJsonObject selected() const;
signals:
    void selectionChanged(bool any);
    void activated();
private:
    QListWidget* m_list = nullptr;
    QLabel* m_status = nullptr;
    QList<QJsonObject> m_devs;
};

// Screen-share picker (§3): modal over the web content only.
class ScreenSharePicker : public QWidget {
    Q_OBJECT
public:
    struct TabSource { QString token, title; QIcon icon; std::function<QPixmap()> grab; };
    ScreenSharePicker(QWidget* contentHost, const QString& host, const QIcon& favicon,
                      QAbstractListModel* screens, QAbstractListModel* windows, const QList<TabSource>& tabs);
    void open();
    enum class Choice { None, Screen, Window, Tab };
signals:
    void chosen(int choice, int row, const QString& tabToken);   // Choice::None = cancelled
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void showSegment(int seg);
    void refresh();
    void finish(bool share);
    QWidget* m_dialog = nullptr;
    QListWidget* m_grid = nullptr;
    QList<QPushButton*> m_seg;
    QPushButton* m_share = nullptr;
    QPointer<QAbstractListModel> m_screens, m_windows;
    QList<TabSource> m_tabs;
    int m_segment = 0;
    QTimer* m_refresh = nullptr;
    QVariantAnimation m_anim; qreal m_t = 0;
    bool m_done = false;
};

// Site controls panel (§7), opened from the lock badge.
struct SiteRow { Perm::Kind kind; QString state; int control; bool on; bool warning; };   // control 0 switch, 1 "Ask" tag, 2 none
class SiteControls : public QWidget {
    Q_OBJECT
public:
    SiteControls(QWidget* window, const QString& origin, const QIcon& favicon, bool secure, const QList<SiteRow>& rows,
                 int cookieCount);
    void popup(const QPoint& anchorTopLeft);
signals:
    void toggled(Perm::Kind kind, bool allow);
    void resetRequested();
    void siteSettingsRequested();
protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    QVariantAnimation m_anim; QPoint m_target;
};

// Small helpers shared by the settings page.
QString stateColor(Perm::Decision d);
QPixmap glyphPixmap(const QString& name, const QString& color, int size, qreal dpr);
QIcon letterIcon(const QString& host, int size);   // favicon fallback: letter in site colour
void announce(QWidget* w, const QString& text);    // QAccessibleAnnouncementEvent (Qt 6.8+)
}
