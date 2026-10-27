#pragma once
#include <QToolButton>
#include <QAbstractButton>
#include <QVariantAnimation>
#include <QPointer>
#include <QIcon>

// Tool button with an animated (120 ms ease-out) hover/press background, per design-spec §5.
class AnimatedToolButton : public QToolButton {
    Q_OBJECT
public:
    explicit AnimatedToolButton(QWidget* parent = nullptr);
    void setRadius(int r) { m_radius = r; update(); }
    void setHoverColor(const QString& c) { m_hoverColor = c; }   // empty → palette surface
    // Driven from native non-client messages (e.g. HTMAXBUTTON for Windows 11 snap layouts).
    void setForcedHover(bool on) { if (on != m_forcedHover) { m_forcedHover = on; animateTo(on || underMouse() ? 1.0 : 0.0); } }
    void setForcedDown(bool on) { if (on != m_forcedDown) { m_forcedDown = on; update(); } }
    // --- 1.5 micro-animations (animations-spec.md) ---
    // §2 reload spin: +360° in 400 ms OutCubic (extra turns stack when clicked again).
    void spin();
    void nudge(qreal dx);
    void bump();             // download-start bump (160 ms)   // back/forward glyph step (200 ms)
    // Cross-fade to a new glyph (120 ms OutCubic, incoming scale 0.8→1); instant with reduced motion.
    void setIconAnimated(const QIcon& icon, int ms = 120);
    // Extra glyph transform driven by owners (download completion arrow): scale / opacity.
    void setGlyphTransform(qreal scale, qreal opacity) { m_extScale = scale; m_extOpacity = opacity; update(); }
    qreal glyphRotation() const { return m_rotation; }
protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    // Draws the icon centred with rotation/scale/opacity transforms; subclasses may override.
    virtual void paintGlyph(QPainter& p);
    virtual void paintOverlay(QPainter&) {}
    qreal m_pressScale = 1.0, m_rotation = 0.0, m_extScale = 1.0, m_extOpacity = 1.0, m_fade = 1.0;
    QIcon m_prevIcon;
    bool m_forcedHover = false;
    bool m_forcedDown = false;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void paintBackground(QPainter& p);
    qreal hoverLevel() const { return m_level; }
private:
    void animateTo(qreal v);
    void animatePress(qreal to, int ms, QEasingCurve::Type ease);
    QVariantAnimation m_anim, m_pressAnim, m_spinAnim, m_fadeAnim, m_nudgeAnim;
    qreal m_nudge = 0.0, m_nudgeDx = 0.0;
    qreal m_level = 0.0;
    int m_radius = 8;
    QString m_hoverColor;
};

// Min / max / close caption button drawn with QPainter (46×40, Windows-style).
class WindowButton : public AnimatedToolButton {
    Q_OBJECT
public:
    enum Kind { Minimize, Maximize, Close };
    WindowButton(Kind k, QWidget* parent = nullptr);
    void setMaximized(bool m) { m_max = m; update(); }
    Kind kind() const { return m_kind; }
protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
private:
    Kind m_kind;
    bool m_max = false;
};

// 40×22 switch with sliding knob (1.9 refresh; design-spec §4 Settings controls).
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {40, 24}; }
protected:
    void paintEvent(QPaintEvent* e) override;
    void checkStateSet() override;
    void nextCheckState() override;
private:
    QVariantAnimation m_anim;
    qreal m_pos = 0.0;
};

class QButtonGroup;
// Sliding selection pill behind a row of checkable buttons (Settings › Theme). Sits under the
// buttons of its parent and glides to the checked one (220 ms OutCubic, instant with reduced motion).
class SegmentThumb : public QWidget {
    Q_OBJECT
public:
    SegmentThumb(QButtonGroup* group, QWidget* parent);
protected:
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void place(bool animate);
    QButtonGroup* m_group;
    class QPropertyAnimation* m_anim;
};

// 22 px accent swatch circle with selected outline.
class SwatchButton : public QAbstractButton {
    Q_OBJECT
public:
    SwatchButton(const QString& color, QWidget* parent = nullptr);
    QString color() const { return m_color; }
    QSize sizeHint() const override { return {30, 30}; }
protected:
    void paintEvent(QPaintEvent* e) override;
private:
    QString m_color;
};

class QLabel;
class QPushButton;
class QPropertyAnimation;

// Inset info bar card (spec §5.7): 8 px margins, 44 px, radius 10, card fill + 1 px border,
// leading 18 px icon, message, optional secondary + primary 32 px pills, 30 px close circle.
class InfoBar : public QWidget {
    Q_OBJECT
public:
    enum Kind { Info, Warning };
    explicit InfoBar(QWidget* parent = nullptr);
    void setKind(Kind k) { m_kind = k; refresh(); }
    void setMessage(const QString& text);
    void setPrimary(const QString& text, const QString& iconName = QString());
    void setSecondary(const QString& text);   // empty = hidden
    void showAnimated();
    void hideAnimated();
    void refresh();   // re-tint icons after a theme change
signals:
    void primaryClicked();
    void secondaryClicked();
    void closed();
private:
    void animateTo(int h, bool hideAtEnd);
    Kind m_kind = Info;
    QString m_primaryIcon;
    QWidget* m_card = nullptr;
    QLabel* m_icon = nullptr;
    QLabel* m_text = nullptr;
    QPushButton* m_secondary = nullptr;
    QPushButton* m_primary = nullptr;
    QToolButton* m_close = nullptr;
    QPropertyAnimation* m_anim = nullptr;
};

#include <QMenu>
namespace Pacitum {
// Popup with the spec's shadow-popup (0 16px 40px) painted into a 16 px transparent margin
// around a card (radius 14). Falls back to the 1 px outline when translucency isn't available.
class PopupMenu : public QMenu {
    Q_OBJECT
public:
    static constexpr int kShadow = 16;
    explicit PopupMenu(QWidget* parent = nullptr);
    int shadowMargin() const { return m_shadow ? kShadow : 0; }
protected:
    bool m_shadow = false;
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;   // sub-menus: sit beside the parent card, level with the item
};
// Sub-menu that looks like its parent (shadow card, same radius). A plain QMenu::addMenu() sub-menu
// of a PopupMenu used to inherit the "transparent" style and was drawn solid black.
QMenu* addSubMenu(QMenu* parent, const QString& title, const QIcon& icon = QIcon());
// 16 px accent spinner arc (2 px stroke) for loading tabs.
QIcon spinnerIcon(int angle, const QString& color);
// Fade-in for every QMenu popup (installed once on qApp).
void installMenuFader();
}

// Omnibox bookmark star with the 1.5 "pop" (animations-spec §3): 34 px circle, unclipped particles.
class StarButton : public AnimatedToolButton {
    Q_OBJECT
public:
    explicit StarButton(QWidget* parent = nullptr);
    void setIcons(const QIcon& outline, const QIcon& filled);
    void setStarred(bool on, bool animate);
    bool starred() const { return m_on; }
protected:
    void paintOverlay(QPainter& p) override;
private:
    void onTick(qreal t);
    QVariantAnimation m_tl;          // timeline in ms (Linear); per-segment easing computed in onTick
    QIcon m_outline, m_filled;
    bool m_on = false, m_pop = false, m_swapped = false;
    qreal m_t = -1, m_from = 1.0;
};
