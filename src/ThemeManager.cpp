#include <QHash>
#include "ThemeManager.h"
#include "Lang.h"
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QIconEngine>
#include <QPixmapCache>
#include <QAtomicInt>
#include <memory>
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

// 2.0.1 themes from the design's Themes artboard. Each row: strip, toolbar, page, card, text,
// muted, field, border, accent, onAccent.
namespace {
struct ThemeDef { const char* id; const char* name; bool dark; const char *strip, *toolbar, *bg, *card, *text, *muted, *field, *border, *accent, *onAccent; };
const ThemeDef kThemes[] = {
    {"light", "Pacitum Light", false, "#E8E2DE", "#FFFFFF", "#F7F4F1", "#FFFFFF", "#151112", "#6E6561", "#F1ECE8", "#E2DBD6", "#D9141F", "#FFFFFF"},
    {"dark", "Pacitum Dark", true, "#0E0B0C", "#1B1617", "#141011", "#1B1617", "#F6F1EF", "#A89E9B", "#262021", "#322B2C", "#FF3B47", "#141011"},
    {"midnight", "Midnight", true, "#0B1230", "#16204A", "#0A1028", "#141D44", "#EEF2FF", "#A5B0D6", "#22305F", "#22305F", "#5A9BFF", "#0A1028"},
    {"forest", "Forest", false, "#1E3A2C", "#F3F7F4", "#E8EFE9", "#FFFFFF", "#0F1F17", "#56695E", "#E1E9E3", "#D6E0D9", "#1F8F5F", "#FFFFFF"},
    {"sakura", "Sakura", false, "#F3D3DA", "#FFF8F9", "#FCEFF2", "#FFFFFF", "#2A1418", "#85565F", "#F7E3E8", "#F0D9DF", "#D6336C", "#FFFFFF"},
    {"sand", "Sand", false, "#E6D8C1", "#FBF7F0", "#F3ECDF", "#FFFDF9", "#2B2117", "#76664F", "#EEE4D3", "#E6DAC6", "#C2410C", "#FFFFFF"},
    {"graphite", "Graphite", true, "#1F2026", "#2E2F36", "#24252B", "#2E2F36", "#F2F2F4", "#A8A9B3", "#3B3C44", "#3B3C44", "#FF7A45", "#1F2026"},
    {"contrast", "High contrast", true, "#000000", "#000000", "#000000", "#000000", "#FFFFFF", "#FFFFFF", "#1A1A1A", "#FFFFFF", "#FFD400", "#000000"},
};
QString mixHex(const QString& a, const QString& b, qreal t) {
    const QColor x(a), y(b);
    return QColor::fromRgbF(x.redF() + (y.redF() - x.redF()) * t, x.greenF() + (y.greenF() - x.greenF()) * t, x.blueF() + (y.blueF() - x.blueF()) * t).name().toUpper();
}
// Custom theme: strings live here so the ThemeDef can point at them.
struct CustomStore { bool set = false; QByteArray name, strip, toolbar, bg, card, text, muted, field, border, accent, onAccent; ThemeDef def{}; };
CustomStore& custom() { static CustomStore c; return c; }
const ThemeDef* findTheme(const QString& id) {
    for (const auto& t : kThemes) if (id == QLatin1String(t.id)) return &t;
    if (id == QLatin1String("custom") && custom().set) return &custom().def;
    return nullptr;
}
}

void ThemeManager::setCustomTheme(const QString& name, const QString& strip, const QString& toolbar, const QString& bg, const QString& accent, bool dark) {
    auto& c = custom();
    auto ok = [](const QString& h, const char* fb) { return QColor(h).isValid() ? QColor(h).name().toUpper().toLatin1() : QByteArray(fb); };
    c.name = (name.trimmed().isEmpty() ? QStringLiteral("My theme") : name.trimmed().left(40)).toUtf8();
    c.strip = ok(strip, "#E8E2DE"); c.toolbar = ok(toolbar, "#FFFFFF"); c.bg = ok(bg, "#F7F4F1"); c.accent = ok(accent, "#D9141F");
    c.card = c.toolbar;
    c.text = dark ? QByteArray("#F6F1EF") : QByteArray("#151112");
    c.muted = mixHex(QString::fromLatin1(c.text), QString::fromLatin1(c.bg), 0.42).toLatin1();
    c.field = mixHex(QString::fromLatin1(c.bg), QString::fromLatin1(c.text), 0.05).toLatin1();
    c.border = mixHex(QString::fromLatin1(c.bg), QString::fromLatin1(c.text), 0.11).toLatin1();
    c.onAccent = QColor(QString::fromLatin1(c.accent)).lightness() > 150 ? QByteArray("#151112") : QByteArray("#FFFFFF");
    c.def = ThemeDef{"custom", c.name.constData(), dark, c.strip.constData(), c.toolbar.constData(), c.bg.constData(), c.card.constData(), c.text.constData(),
                     c.muted.constData(), c.field.constData(), c.border.constData(), c.accent.constData(), c.onAccent.constData()};
    c.set = true;
}

QStringList ThemeManager::themeIds() { QStringList l; for (const auto& t : kThemes) l << QString::fromLatin1(t.id); if (custom().set) l << QStringLiteral("custom"); return l; }
bool ThemeManager::themeIsDark(const QString& id) { const auto* t = findTheme(id); return t && t->dark; }
QString ThemeManager::themeName(const QString& id) { const auto* t = findTheme(id); return t ? QString::fromUtf8(t->name) : QString(); }
QString ThemeManager::themeAccent(const QString& id) { const auto* t = findTheme(id); return t ? QString::fromLatin1(t->accent) : QStringLiteral("#D9141F"); }
void ThemeManager::setThemeId(const QString& id) {
    m_themeId = findTheme(id) ? id : QString();
    if (const auto* t = findTheme(m_themeId)) m_darkMode = t->dark;
    rebuild();
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
void ThemeManager::restoreCurrent(const PacitumPalette& p) { s_current = p; }

ThemeManager::ThemeManager(bool darkMode) : m_darkMode(darkMode) { rebuild(); }

void ThemeManager::setDarkMode(bool enabled) { m_darkMode = enabled; rebuild(); }

void ThemeManager::rebuild() {
    const ThemeDef* t = m_private ? nullptr : findTheme(m_themeId);
    const bool dark = m_private ? true : t ? t->dark : m_darkMode;   // design: private windows are always Pacitum Dark
    m_darkMode = dark;
    m_pal = dark ? darkPalette() : lightPalette();
    if (t && QLatin1String(t->id) != QLatin1String("light") && QLatin1String(t->id) != QLatin1String("dark")) {
        const QString text = QString::fromLatin1(t->text), muted = QString::fromLatin1(t->muted), bg = QString::fromLatin1(t->bg);
        m_pal.frame = QString::fromLatin1(t->strip); m_pal.tab = QString::fromLatin1(t->toolbar); m_pal.toolbar = QString::fromLatin1(t->toolbar);
        m_pal.page = bg; m_pal.text = text; m_pal.textSecondary = muted;
        m_pal.surface = QString::fromLatin1(t->field); m_pal.tabHover = m_pal.surface;
        m_pal.surfaceHover = QString::fromLatin1(t->border); m_pal.pressed = m_pal.surfaceHover; m_pal.border = QString::fromLatin1(t->border);
        m_pal.borderStrong = mixHex(m_pal.border, text, 0.18);
        m_pal.textTertiary = mixHex(muted, bg, 0.3); m_pal.disabled = mixHex(muted, bg, 0.5);
        m_pal.omnibox = QString::fromLatin1(t->field); m_pal.omniboxHover = mixHex(m_pal.omnibox, text, 0.05);
        m_pal.accent = QString::fromLatin1(t->accent); m_pal.onAccent = QString::fromLatin1(t->onAccent);
        const QColor ac(m_pal.accent);
        m_pal.accentSoft = QStringLiteral("rgba(%1,%2,%3,%4)").arg(ac.red()).arg(ac.green()).arg(ac.blue()).arg(dark ? 0.16 : 0.10);
        m_pal.accentHover = (dark ? ac.lighter(115) : ac.darker(115)).name(); m_pal.accentPressed = (dark ? ac.darker(110) : ac.darker(135)).name();
        m_pal.focusRing = text; m_pal.primary = text; m_pal.onPrimary = bg;
        if (QLatin1String(t->id) == QLatin1String("contrast")) { m_pal.focusRing = m_pal.accent; m_pal.textTertiary = text; m_pal.disabled = QStringLiteral("#9A9A9A"); }
    }
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
    if (m_private) m_pal.frame = QStringLiteral("#16121C");
    s_current = m_pal;
}

namespace {
// 2.0.1 vector icon: the SVG is rendered at the exact device size that is asked for (any screen
// scale, 125 % / 150 % included) instead of being rasterised once at a fixed size and then resampled,
// which is what made the icons soft.
class TintedSvgEngine : public QIconEngine {
public:
    TintedSvgEngine(const QByteArray& normal, const QByteArray& disabled)
        : m_id(nextId()), m_normal(std::make_shared<QSvgRenderer>(normal)),
          m_disabled(disabled.isEmpty() ? m_normal : std::make_shared<QSvgRenderer>(disabled)), m_hasDisabled(!disabled.isEmpty()) {}
    QIconEngine* clone() const override { return new TintedSvgEngine(*this); }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        rendererFor(mode)->render(p, QRectF(rect));
        p->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State) override {
        if (size.isEmpty()) return {};
        const QString key = QStringLiteral("pcsvg%1|%2x%3|%4").arg(m_id).arg(size.width()).arg(size.height()).arg(mode == QIcon::Disabled && m_hasDisabled);
        QPixmap pm;
        if (QPixmapCache::find(key, &pm)) return pm;
        pm = QPixmap(size); pm.fill(Qt::transparent);
        QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
        rendererFor(mode)->render(&p, QRectF(QPointF(0, 0), QSizeF(size)));
        p.end();
        QPixmapCache::insert(key, pm);
        return pm;
    }
private:
    static int nextId() { static QAtomicInt n; return n.fetchAndAddRelaxed(1); }
    QSvgRenderer* rendererFor(QIcon::Mode mode) const { return (mode == QIcon::Disabled && m_hasDisabled ? m_disabled : m_normal).get(); }
    int m_id;
    std::shared_ptr<QSvgRenderer> m_normal, m_disabled;
    bool m_hasDisabled;
};
}

QIcon ThemeManager::icon(const QString& name, const QString& color, int size, const QString& disabledColor) {
    // Tinted SVG icons are requested on every repaint/theme pass; build each variant once.
    static QHash<QString, QIcon> cache;
    const QString key = name + QLatin1Char('|') + color + QLatin1Char('|') + disabledColor;
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    if (cache.size() > 2000) cache.clear();
    QIcon made = renderIcon(name, color, size, disabledColor);
    cache.insert(key, made);
    return made;
}

QIcon ThemeManager::renderIcon(const QString& name, const QString& color, int /*size*/, const QString& disabledColor) {
    QFile f(QStringLiteral(":/icons/ui/%1.svg").arg(name));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray src = f.readAll();
    auto tint = [&](const QString& col) { QByteArray svg = src; svg.replace("currentColor", col.toUtf8()); return svg; };
    return QIcon(new TintedSvgEngine(tint(color), disabledColor.isEmpty() ? QByteArray() : tint(disabledColor)));
}

QString ThemeManager::mainWindowStyle() const {
    const auto& p = m_pal;
    QString s = QStringLiteral(R"(
QWidget { color: @text; font-family: @fontstack"Rubik", "Inter", "Segoe UI", "Helvetica Neue", "Cantarell", sans-serif; font-size: 13px; }
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
#MenuProfile { background: transparent; border-radius: 12px; }
#MenuProfile:hover { background: @surface; }
#MenuProfileName { font-weight: 500; font-size: 14.5px; }
#MenuProfileSub { color: @textSecondary; font-size: 12.5px; }
#MenuProfileSwitch { color: @textSecondary; font-size: 12.5px; font-weight: 500; }
#MainMenu { border: 1px solid @border; border-radius: 18px; padding: 8px; }
#MainMenu::item, #MainSubMenu::item { height: 38px; padding: 0 12px; border-radius: 10px; font-size: 14px; }
#MainMenu::separator { margin: 6px 8px; }
#MainMenu QLabel { font-size: 14px; }
#MenuZoomPct { font-weight: 500; }
#MenuFootText { color: @textSecondary; font-size: 12.5px; }
#MenuExit { font-size: 12.5px; font-weight: 500; min-height: 30px; padding: 0 12px; border-radius: 10px; }
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
        {"@disabled", p.disabled}, {"@fontstack", Lang::cssFontStack().isEmpty() ? QString() : Lang::cssFontStack() + QStringLiteral(", ")},
        {"@accent", p.accent}, {"@tab", p.tab},
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
#OmniTabHint { background: @omnibox; color: @text; border: 1px solid @border; border-radius: 7px; padding: 2px 8px; font-size: 12px; font-weight: 600; }

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
