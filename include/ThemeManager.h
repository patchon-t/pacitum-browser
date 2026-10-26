#pragma once
#include <QString>
#include <QColor>
#include <QIcon>

// Pacitum design tokens (design-spec-1.5.md §1).
// Legacy names kept for existing callers: frame = chrome, tab = elevated, toolbar = card.
struct PacitumPalette {
    QString frame, tab, tabHover, toolbar, page, surface, surfaceHover, border,
            text, textSecondary, disabled, accent, onAccent, accentSoft, secure, danger;
    // 1.5 additions
    QString omnibox, omniboxHover, pressed, borderStrong, textTertiary,
            accentHover, accentPressed, focusRing, warning;
    // 2.0: red accent = indicators only; primary (ink) = primary actions, switches, selection
    QString primary, onPrimary, dangerSoft;
    const QString& chrome() const { return frame; }
    const QString& elevated() const { return tab; }
    const QString& card() const { return toolbar; }
};

namespace Pacitum {
// Animation duration scaled for the OS / app "reduce motion" preference (0 when reduced).
int motionMs(int ms);
bool reducedMotion();
void setReducedMotion(bool on);   // app setting override
void clearReducedMotionOverride(); // back to the OS setting
}

class ThemeManager {
public:
    explicit ThemeManager(bool darkMode = false);

    void setDarkMode(bool enabled);
    bool isDarkMode() const { return m_darkMode; }
    void setPrivate(bool p) { m_private = p; }
    void setAccent(const QString& accent) { m_accentOverride = accent; }
    const PacitumPalette& palette() const { return m_pal; }
    static const PacitumPalette& current();   // palette of the most recently applied theme

    QString mainWindowStyle() const;
    QString tabWidgetStyle(bool vertical) const;
    QString addressBarStyle() const;

    // Load a design SVG icon from :/icons/ui/<name>.svg tinted with color.
    static QIcon icon(const QString& name, const QString& color, int size = 18,
                      const QString& disabledColor = QString());
private:
    static QIcon renderIcon(const QString& name, const QString& color, int size,
                      const QString& disabledColor);
public:
    QIcon icon(const QString& name, int size = 18) const {
        return icon(name, m_pal.text, size, m_pal.disabled);
    }
    QIcon secondaryIcon(const QString& name, int size = 18) const {
        return icon(name, m_pal.textSecondary, size, m_pal.disabled);
    }

private:
    void rebuild();
    QString horizontalTabStyle() const;
    QString verticalTabStyle() const;

    bool m_darkMode;
    bool m_private = false;
    QString m_accentOverride;
    PacitumPalette m_pal;
};
