#include <QHash>
#include "ThemeManager.h"
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QGuiApplication>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Pacitum 2.0 palette (docs/design-system-2.0.md): warm neutrals, ink for actions, red for indicators.
static PacitumPalette darkPalette() {
    PacitumPalette p;
    p.frame = "#0E0B0C"; p.tab = "#1B1617"; p.tabHover = "#2A2324"; p.toolbar = "#1B1617"; p.page = "#141011";
    p.surface = "#2A2324"; p.surfaceHover = "#322B2C"; p.border = "#322B2C"; p.text = "#F6F1EF";
    p.textSecondary = "#A89E9B"; p.disabled = "#5E5556"; p.accent = "#FF3B47"; p.onAccent = "#FFFFFF";
    p.accentSoft = "rgba(255,59,71,0.16)"; p.secure = "#4CC38A"; p.danger = "#FF6B73";
    p.omnibox = "#141011"; p.omniboxHover = "#201A1B"; p.pressed = "#322B2C"; p.borderStrong = "#3D3536";
    p.textTertiary = "#7A7071"; p.accentHover = "#FF5C66"; p.accentPressed = "#E0202C";
    p.focusRing = "#F6F1EF"; p.warning = "#F2A65A";
    p.primary = "#F6F1EF"; p.onPrimary = "#141011"; p.dangerSoft = "rgba(255,107,115,0.14)";
    return p;
}
static PacitumPalette lightPalette() {
    PacitumPalette p;
    p.frame = "#E8E2DE"; p.tab = "#FFFFFF"; p.tabHover = "#EFE9E5"; p.toolbar = "#FFFFFF"; p.page = "#F7F4F1";
    p.surface = "#EFE9E5"; p.surfaceHover = "#E2DBD6"; p.border = "#E2DBD6"; p.text = "#151112";
    p.textSecondary = "#6E6561"; p.disabled = "#B8AFAA"; p.accent = "#D9141F"; p.onAccent = "#FFFFFF";
    p.accentSoft = "rgba(217,20,31,0.08)"; p.secure = "#1F7A57"; p.danger = "#A30F19";
    p.omnibox = "#F1ECE8"; p.omniboxHover = "#EBE5E1"; p.pressed = "#E2DBD6"; p.borderStrong = "#CFC7C2";
    p.textTertiary = "#9C928D"; p.accentHover = "#B8101A"; p.accentPressed = "#930C14";
    p.focusRing = "#151112"; p.warning = "#B8470F";
    p.primary = "#151112"; p.onPrimary = "#F7F4F1"; p.dangerSoft = "#FBE4E6";
    return p;
}

namespace Pacitum {
static int s_reduceOverride = -1;
void setReducedMotion(bool on) { s_reduceOverride = on ? 1 : 0; }
void clearReducedMotionOverride() { s_reduceOverride = -1; }
bool reducedMotion() {
    if (s_reduceOverride >= 0) return s_reduceOverride == 1;
    static int os = -1;
    if (os < 0) {
        os = qEnvironmentVariableIntValue("PACITUM_REDUCED_MOTION") ? 1 : 0;
#ifdef Q_OS_WIN
        BOOL anim = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim) os = 1;
#endif
    }
    return os == 1;
}
int motionMs(int ms) { return reducedMotion() ? 0 : ms; }
}

static PacitumPalette s_current = darkPalette();
const PacitumPalette& ThemeManager::current() { return s_current; }

ThemeManager::ThemeManager(bool darkMode) : m_darkMode(darkMode) { rebuild(); }

void ThemeManager::setDarkMode(bool enabled) { m_darkMode = enabled; rebuild(); }

void ThemeManager::rebuild() {
    m_pal = m_darkMode ? darkPalette() : lightPalette();
    if (!m_accentOverride.isEmpty()) {
        QColor c(m_accentOverride);
        if (c.isValid()) {
            m_pal.accent = c.name();
            m_pal.accentSoft = QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(m_darkMode ? 0.14 : 0.10);
            m_pal.accentHover = (m_darkMode ? c.lighter(125) : c.darker(120)).name();
            m_pal.accentPressed = (m_darkMode ? c.darker(115) : c.darker(140)).name();
            m_pal.onAccent = c.lightness() > 140 ? QStringLiteral("#151112") : QStringLiteral("#FFFFFF");
        }
    }
    if (m_private) m_pal.frame = m_darkMode ? QStringLiteral("#16121C") : QStringLiteral("#E6E1EC");
    s_current = m_pal;
}

QIcon ThemeManager::icon(const QString& name, const QString& color, int size, const QString& disabledColor) {
    // Tinted SVG icons are requested on every repaint/theme pass; render each variant once.
    static QHash<QString, QIcon> cache;
    const qreal cdpr = qMax<qreal>(1.0, qGuiApp ? qGuiApp->devicePixelRatio() : 1.0);
    const QString key = name + QLatin1Char('|') + color + QLatin1Char('|') + QString::number(size) + QLatin1Char('|') + disabledColor
                      + QLatin1Char('|') + QString::number(cdpr);
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    if (cache.size() > 2000) cache.clear();
    QIcon made = renderIcon(name, color, size, disabledColor);
    cache.insert(key, made);
    return made;
}

QIcon ThemeManager::renderIcon(const QString& name, const QString& color, int size, const QString& disabledColor) {
    QFile f(QStringLiteral(":/icons/ui/%1.svg").arg(name));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray src = f.readAll();
    auto render = [&](const QString& col) {
        QByteArray svg = src; svg.replace("currentColor", col.toUtf8());
        QSvgRenderer r(svg);
        const qreal dpr = qMax<qreal>(1.0, qGuiApp ? qGuiApp->devicePixelRatio() : 1.0);
        const int px = qRound(size * qMax<qreal>(dpr, 2.0));
        QPixmap pm(px, px); pm.fill(Qt::transparent);
        QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); r.render(&p); p.end();
        return pm;
    };
    QIcon ic;
    ic.addPixmap(render(color), QIcon::Normal);
    ic.addPixmap(render(color), QIcon::Active);
    if (!disabledColor.isEmpty()) ic.addPixmap(render(disabledColor), QIcon::Disabled);
    return ic;
}

QString ThemeManager::mainWindowStyle() const {
    const auto& p = m_pal;
    QString s = QStringLiteral(R"(
QWidget { color: @text; font-family: "Rubik", "Inter", "Segoe UI", "Helvetica Neue", "Cantarell", sans-serif; font-size: 13px; }
QMainWindow, QDialog { background: @page; }
#TabStrip { background: @frame; }
#TabStrip QToolButton { background: transparent; border: none; }
#NavToolbar { background: @toolbar; border: none; }
#NavToolbar QToolButton { background: transparent; border: none; padding: 0; }
#NavToolbar QToolButton::menu-indicator { image: none; width: 0; }
#ChromeDivider { background: @border; border: none; min-height: 1px; max-height: 1px; }
#Avatar { border-radius: 16px; color: @onPrimary; font-weight: 600; font-size: 14px; background: @primary; }
BookmarksBar { background: @toolbar; border: none; }
BookmarksBar QScrollArea, BookmarksBar QScrollArea > QWidget > QWidget { background: @toolbar; border: none; }
BookmarksBar QPushButton { background: transparent; border: none; border-radius: 8px; padding: 0 10px;
          height: 26px; min-height: 26px; max-width: 160px; font-size: 12px; color: @text; text-align: left; }
BookmarksBar QPushButton:hover { background: @surface; }
BookmarksBar QPushButton:pressed { background: @pressed; }
QMenu { background: @toolbar; border: 1px solid @borderStrong; border-radius: 16px; padding: 6px; }
QMenu::item { height: 34px; padding: 0 12px 0 6px; border-radius: 10px; background: transparent; color: @text; }
QMenu::item:selected { background: @surface; }
QMenu::item:pressed { background: @pressed; }
QMenu::item:disabled { color: @disabled; }
QMenu::icon { padding-left: 10px; }
QMenu::separator { height: 1px; background: @border; margin: 5px 8px; }
QMenu QToolButton { background: transparent; border: none; border-radius: 15px; }
QMenu QToolButton:hover { background: @surfaceHover; }
QMenu QToolButton:pressed { background: @pressed; }
QMenu QLabel { background: transparent; }
#MenuProfile { background: transparent; border-radius: 8px; }
#MenuProfile:hover { background: @surface; }
#MenuProfileName { font-weight: 600; font-size: 13px; }
#MenuProfileSub { color: @textSecondary; font-size: 12px; }
#MenuAvatar { border-radius: 18px; color: @onPrimary; font-weight: 600; font-size: 14px; background: @primary; }
#MenuTiles { background: transparent; }
#MenuTiles QToolButton { background: transparent; border: none; border-radius: 10px; padding: 6px 0 4px 0; font-size: 12px; color: @text; }
#MenuTiles QToolButton:hover { background: @surface; }
#MenuTiles QToolButton:pressed { background: @pressed; }
#MenuTiles QToolButton:focus { background: @surface; }
#ZoomPill { background: @surface; border-radius: 15px; }
#ZoomPill QLabel { font-weight: 600; font-size: 12px; }
#InfoBarWrap { background: @page; }
#InfoBar { background: @toolbar; border: 1px solid @border; border-radius: 10px; }
#InfoBar QLabel { background: transparent; border: none; font-size: 13px; color: @text; }
#InfoBar QPushButton { min-height: 32px; max-height: 32px; border-radius: 16px; padding: 0 14px; }
#InfoBar QToolButton { background: transparent; border: none; border-radius: 15px; }
#InfoBar QToolButton:hover { background: @surface; }
#InfoBar QToolButton:pressed { background: @pressed; }
QPushButton { background: @toolbar; border: 1px solid @border; border-radius: 12px; padding: 0 16px; min-height: 34px; color: @text; font-weight: 500; }
QPushButton:hover { background: @surface; }
QPushButton:pressed { background: @pressed; }
QPushButton:disabled { color: @disabled; border-color: @border; }
QPushButton:default, QPushButton#PrimaryButton { background: @primary; color: @onPrimary; border: none; font-weight: 500; }
QPushButton:default:hover, QPushButton#PrimaryButton:hover { background: @textSecondary; }
QPushButton:default:pressed, QPushButton#PrimaryButton:pressed { background: @textTertiary; }
QPushButton#DangerButton { background: transparent; color: @danger; border: 1px solid @dangerSoft; }
QPushButton#DangerButton:hover { background: @dangerSoft; }
QLineEdit, QComboBox, QSpinBox { background: @omnibox; border: 1px solid transparent; border-radius: 12px; min-height: 32px; padding: 0 12px; color: @text; selection-background-color: @primary; selection-color: @onPrimary; }
QLineEdit:hover, QComboBox:hover, QSpinBox:hover { border: 1px solid @border; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 2px solid @focusRing; background: @toolbar; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView { background: @toolbar; border: 1px solid @border; selection-background-color: @surface; selection-color: @text; outline: 0; }
QCheckBox { spacing: 10px; background: transparent; }
QCheckBox::indicator { width: 40px; height: 24px; border-radius: 12px; background: @borderStrong; }
QCheckBox::indicator:checked { background: @primary; }
QGroupBox { background: @toolbar; border: 1px solid @border; border-radius: 20px; margin-top: 28px; padding: 14px 20px 10px 20px; font-weight: 400; }
QGroupBox::title { subcontrol-origin: margin; left: 2px; top: 4px; padding: 0; font-size: 15px; font-weight: 600; color: @text; }
QGroupBox QLabel, QGroupBox QCheckBox, QGroupBox QRadioButton { background: transparent; }
QTabWidget#SettingsTabs::pane { border: none; background: @page; }
QTabWidget#SettingsTabs QTabBar::tab { background: transparent; color: @text; height: 36px; padding: 0 14px; margin: 8px 2px 0 2px; border-radius: 8px; }
QTabWidget#SettingsTabs QTabBar::tab:selected { background: @toolbar; color: @text; font-weight: 500; border-top: 3px solid @accent; }
QTabWidget#SettingsTabs QTabBar::tab:hover:!selected { background: @surface; }
#SettingsPage, #SettingsContent, #SettingsScroll, #SettingsScroll > QWidget > QWidget { background: @page; }
#SettingsNav { background: @page; border: none; border-right: 1px solid @border; outline: 0; padding: 0 12px; }
#SettingsNav::item { height: 42px; border-radius: 12px; padding: 0 12px; color: @text; margin: 1px 0; border-left: 4px solid transparent; }
#SettingsNav::item:hover { background: @surface; }
#SettingsNav::item:selected { background: @toolbar; color: @text; font-weight: 500; border-left: 4px solid @accent; }
#SettingsH1 { font-size: 26px; font-weight: 600; background: transparent; }
#SettingsTitle { font-size: 26px; font-weight: 600; background: transparent; }
QLineEdit#SettingsSearch { background: @toolbar; border: 1px solid @border; border-radius: 14px; min-height: 42px; padding: 0 14px; }
QLineEdit#SettingsSearch:focus { border: 2px solid @focusRing; padding: 0 13px; }
#SettingsH2 { font-size: 15px; font-weight: 600; background: transparent; margin-top: 6px; }
#Card { background: @toolbar; border: 1px solid @border; border-radius: 20px; }
#Card QLabel { background: transparent; }
#Divider { background: @border; border: none; min-height: 1px; max-height: 1px; }
#RowSub { color: @textSecondary; font-size: 12.5px; }
#Segmented { background: @omnibox; border-radius: 12px; }
#Segmented QPushButton { background: transparent; border: none; border-radius: 8px; min-height: 30px; padding: 0 14px; }
#Segmented QPushButton:hover { background: transparent; }
#Segmented QPushButton:checked { background: transparent; font-weight: 600; }
#SegThumb { background: @segThumb; border-radius: 8px; }
QStatusBar { background: @toolbar; color: @textSecondary; border-top: 1px solid @border; font-size: 12px; }
QStatusBar QLabel { background: transparent; color: @textSecondary; }
QToolTip { background: @primary; color: @onPrimary; border: none; border-radius: 8px; padding: 5px 9px; }
QProgressBar { background: transparent; border: none; }
QProgressBar::chunk { background: @primary; border-radius: 2px; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical { background: @border; border-radius: 4px; min-height: 30px; margin: 2px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QSplitter::handle { background: @border; }
#NotesPanel { background: @toolbar; border-left: 1px solid @border; }
QPlainTextEdit#NotesEdit { background: @page; border: 1px solid @border; border-radius: 12px; padding: 8px; font-size: 14px; selection-background-color: @primary; selection-color: @onPrimary; }
QPlainTextEdit#NotesEdit:focus { border: 2px solid @focusRing; padding: 7px; }
#ShotBar { background: @toolbar; border: 1px solid @borderStrong; border-radius: 22px; }
#ShotBar QLabel { background: transparent; color: @textSecondary; }
#ShotBar QPushButton { border-radius: 16px; min-height: 32px; max-height: 32px; }
QListWidget, QTableWidget, QTreeWidget { background: @toolbar; border: 1px solid @border; border-radius: 20px; alternate-background-color: @page; }
QHeaderView::section { background: @toolbar; color: @textSecondary; border: none; border-bottom: 1px solid @border; padding: 6px; }
)");
    const QList<QPair<QString, QString>> repl = {
        {"@surfaceHover", p.surfaceHover}, {"@borderStrong", p.borderStrong}, {"@textTertiary", p.textTertiary},
        {"@accentHover", p.accentHover}, {"@accentPressed", p.accentPressed}, {"@pressed", p.pressed}, {"@textSecondary", p.textSecondary}, {"@accentSoft", p.accentSoft},
        {"@onAccent", p.onAccent}, {"@tabHover", p.tabHover}, {"@frame", p.frame}, {"@toolbar", p.toolbar},
        {"@page", p.page}, {"@surface", p.surface}, {"@border", p.border}, {"@text", p.text},
        {"@disabled", p.disabled}, {"@accent", p.accent}, {"@tab", p.tab},
        {"@onPrimary", p.onPrimary}, {"@primary", p.primary}, {"@focusRing", p.focusRing},
        {"@dangerSoft", p.dangerSoft}, {"@danger", p.danger}, {"@omnibox", p.omnibox},
        {"@segThumb", m_darkMode ? p.pressed : p.toolbar}};
    for (auto& r : repl) s.replace(r.first, r.second);
    return s;
}

QString ThemeManager::tabWidgetStyle(bool vertical) const {
    return vertical ? verticalTabStyle() : horizontalTabStyle();
}

QString ThemeManager::horizontalTabStyle() const {
    const auto& p = m_pal;
    return QStringLiteral(R"(
QTabWidget::pane { border: none; background: %1; }
QTabWidget::tab-bar { left: 0; }
QTabBar { background: %2; qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: %3; height: 36px; min-width: 72px; max-width: 236px; width: 236px;
               padding: 0 8px 0 12px; margin-right: 4px; margin-top: 10px; text-align: left;
               border-top-left-radius: 14px; border-top-right-radius: 14px; }
QTabBar::tab:selected { background: %4; color: %5; }
QTabBar::tab:hover:!selected { background: %6; }
QTabBar::close-button { image: url(%7); subcontrol-position: right; width: 14px; height: 14px; padding: 3px; border-radius: 5px; }
QTabBar::close-button:hover { background: %8; }
QTabBar QToolButton { background: %2; border: none; }
)").arg(p.page, p.frame, p.textSecondary, p.tab, p.text, p.tabHover,
         m_darkMode ? QStringLiteral(":/icons/ui/close-dark.svg") : QStringLiteral(":/icons/ui/close-light.svg"),
         p.surfaceHover);
}

QString ThemeManager::verticalTabStyle() const {
    const auto& p = m_pal;
    return QStringLiteral(R"(
QTabWidget::pane { border: none; background: %1; }
QTabBar { background: %2; }
QTabBar::tab { background: transparent; color: %3; padding: 0 10px; margin: 2px 6px; border-radius: 12px; min-height: 40px; width: 220px; text-align: left; }
QTabBar::tab:selected { background: %4; color: %5; }
QTabBar::tab:hover:!selected { background: %6; }
QTabBar::close-button { image: url(%7); width: 14px; height: 14px; padding: 3px; border-radius: 5px; }
QTabBar::close-button:hover { background: %8; }
)").arg(p.page, p.frame, p.textSecondary, p.tab, p.text, p.tabHover,
         m_darkMode ? QStringLiteral(":/icons/ui/close-dark.svg") : QStringLiteral(":/icons/ui/close-light.svg"),
         p.surfaceHover);
}

QString ThemeManager::addressBarStyle() const {
    const auto& p = m_pal;
    QString s = QStringLiteral(R"(
#Omnibox { background: @omnibox; border: 1px solid transparent; border-radius: 14px; }
#Omnibox:hover { background: @omniboxHover; border: 1px solid transparent; }
#Omnibox[focused="true"] { background: @toolbar; border: 2px solid @focusRing; }
#Omnibox QLineEdit { background: transparent; border: none; padding: 0; min-height: 0; font-size: 15px; color: @text;
                     selection-background-color: @primary; selection-color: @onPrimary; }
#Omnibox QLineEdit[urlview="true"] { color: transparent; }
#Omnibox QLineEdit:focus { border: none; background: transparent; }
#Omnibox QLabel { background: transparent; border: none; font-size: 14px; }
#Omnibox QToolButton { background: transparent; border: none; border-radius: 15px; }
#Omnibox QToolButton:hover { background: @surface; }
#Omnibox QToolButton:pressed { background: @pressed; }
#OmniChip { border-radius: 9px; padding: 0 6px; font-size: 12px; }
#OmniChip[kind="danger"] { color: @danger; padding: 0 10px 0 8px; }
#OmniChip[kind="internal"] { color: @onPrimary; background: @primary; border-radius: 7px; padding: 0 8px; font-weight: 500; }
#OmniChip:hover { background: @surface; }
)");
    const QList<QPair<QString, QString>> repl = {
        {"@omniboxHover", p.omniboxHover}, {"@omnibox", p.omnibox}, {"@toolbar", p.toolbar}, {"@borderStrong", p.borderStrong}, {"@border", p.border},
        {"@accent", p.accent}, {"@onAccent", p.onAccent}, {"@text", p.text}, {"@surface", p.surface},
        {"@pressed", p.pressed}, {"@danger", p.danger},
        {"@onPrimary", p.onPrimary}, {"@primary", p.primary}, {"@focusRing", p.focusRing}};
    for (auto& r : repl) s.replace(r.first, r.second);
    return s;
}
