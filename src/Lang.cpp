#include "Lang.h"
#include "AppSettings.h"
#include <QApplication>
#include <QAbstractButton>
#include <QAction>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGroupBox>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextEdit>
#include <QTimer>
#include <QTranslator>
#include <QVector>
#include <QWidget>
#include <QWidgetAction>
#include <functional>

namespace Lang {
namespace {

struct Pattern {
    QString prefix;                 // literal text before the first %n (cheap pre-check)
    QRegularExpression re;
    QVector<int> numbers;           // placeholder number for each capture group
    QString out;                    // translation, with %n
};

struct State {
    QString choice = QStringLiteral("system");
    QString code = QStringLiteral("en");
    QHash<QString, QString> exact;
    QVector<Pattern> patterns;
    QHash<QString, QString> cache;
    QJsonObject raw;
    Notifier notifier;
    QTranslator* qtTranslator = nullptr;
};
State& S() { static State s; return s; }

QString norm(const QString& s) { return s.simplified(); }

const QRegularExpression& placeholderRe() {
    static const QRegularExpression re(QStringLiteral("%(\\d)"));
    return re;
}

void addEntry(const QString& key, const QString& value) {
    if (key.isEmpty() || value.isEmpty()) return;
    auto& st = S();
    if (!placeholderRe().match(key).hasMatch()) { st.exact.insert(norm(key), value); return; }
    // "Zoom: %1%" -> ^Zoom: (.+?)%$ ; the number of each %n is remembered so a translation may reorder them
    Pattern p;
    QString rx = QStringLiteral("^");
    const QString k = norm(key);
    int last = 0;
    bool first = true;
    auto it = placeholderRe().globalMatch(k);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString lit = k.mid(last, m.capturedStart() - last);
        if (first) { p.prefix = lit; first = false; }
        rx += QRegularExpression::escape(lit) + QStringLiteral("(.+?)");
        p.numbers.append(m.captured(1).toInt());
        last = m.capturedEnd();
    }
    rx += QRegularExpression::escape(k.mid(last)) + QStringLiteral("$");
    p.re = QRegularExpression(rx);
    p.out = value;
    st.patterns.append(p);
}

QString lookupCore(const QString& core, int depth);

QString fill(const Pattern& p, const QRegularExpressionMatch& m, int depth) {
    QString out = p.out;
    for (int i = 0; i < p.numbers.size(); ++i) {
        const QString cap = m.captured(i + 1);
        // the captured part may itself be a UI word ("Default (%1)" with "Ask first")
        out.replace(QLatin1Char('%') + QString::number(p.numbers[i]), depth < 2 ? lookupCore(cap, depth + 1) : cap);
    }
    return out;
}

QString lookupCore(const QString& core, int depth) {
    auto& st = S();
    const QString n = norm(core);
    if (auto it = st.exact.constFind(n); it != st.exact.constEnd()) return it.value();
    if (n.size() < 2 || n.size() > 400 || n.startsWith(QLatin1Char('<'))) return core;
    for (const Pattern& p : std::as_const(st.patterns)) {
        if (!p.prefix.isEmpty() && !n.startsWith(p.prefix)) continue;
        const auto m = p.re.match(n);
        if (m.hasMatch()) return fill(p, m, depth);
    }
    return core;
}

void loadDictionary(const QString& code) {
    auto& st = S();
    st.code = code;
    st.exact.clear(); st.patterns.clear(); st.cache.clear(); st.raw = QJsonObject();
    if (code == QLatin1String("en")) return;
    QFile f(QStringLiteral(":/lang/%1.json").arg(code));
    if (!f.open(QIODevice::ReadOnly)) { st.code = QStringLiteral("en"); return; }
    st.raw = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = st.raw.begin(); it != st.raw.end(); ++it) addEntry(it.key(), it.value().toString());
}

QString qtLocaleName(const QString& code) { return QString(code).replace(QLatin1Char('-'), QLatin1Char('_')); }

void applyQtLocale() {
    auto& st = S();
    QLocale::setDefault(st.code == QLatin1String("en") ? QLocale::system() : QLocale(qtLocaleName(st.code)));
    if (st.qtTranslator) { QCoreApplication::removeTranslator(st.qtTranslator); delete st.qtTranslator; st.qtTranslator = nullptr; }
    if (st.code == QLatin1String("en")) return;
    // Qt's own texts (standard buttons, ...) when the Qt translations are shipped next to the program
    auto* t = new QTranslator;
    const QStringList dirs{QCoreApplication::applicationDirPath() + QStringLiteral("/translations"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)};
    for (const QString& d : dirs)
        if (t->load(QLocale(qtLocaleName(st.code)), QStringLiteral("qtbase"), QStringLiteral("_"), d)) { QCoreApplication::installTranslator(t); st.qtTranslator = t; return; }
    delete t;
}

// ---------------------------------------------------------------- widgets
// The English text a widget was given is kept in a property, so a later language change starts from it.
void fixText(QObject* o, const char* kind, const QString& cur, const std::function<void(const QString&)>& set) {
    if (cur.isEmpty()) return;
    const QByteArray sk = QByteArray("_pmsrc_") + kind, ok = QByteArray("_pmout_") + kind;
    QString src = o->property(sk).toString();
    const QString out = o->property(ok).toString();
    if (src.isEmpty() && isEnglish()) return;
    if (src.isEmpty() || cur != out) src = cur;      // new text from the program
    const QString t = tr(src);
    if (t == src && !o->property(sk).isValid()) return;   // nothing to remember
    o->setProperty(sk, src); o->setProperty(ok, t);
    if (t != cur) set(t);
}

void fixAction(QAction* a) {
    if (!a || a->isSeparator() || qobject_cast<QWidgetAction*>(a)) return;
    // "Close tab\tCtrl+W": only the words are translated
    const QString full = a->text();
    const int tab = full.indexOf(QLatin1Char('\t'));
    if (tab < 0) {
        fixText(a, "text", full, [a](const QString& s) { a->setText(s); });
    } else {
        const QString keys = full.mid(tab);
        const QString src = a->property("_pmsrc_text").toString();
        const QString words = full.left(tab);
        const QString sw = src.isEmpty() || (words + keys) != a->property("_pmout_text").toString() ? words : src;
        const QString t = tr(sw);
        a->setProperty("_pmsrc_text", sw); a->setProperty("_pmout_text", t + keys);
        if (t + keys != full) a->setText(t + keys);
    }
    if (!a->toolTip().isEmpty() && a->toolTip() != a->text())
        fixText(a, "tip", a->toolTip(), [a](const QString& s) { a->setToolTip(s); });
}

void translateMenu(QMenu* m) {
    if (!m || m->property("pacitumNoTr").toBool()) return;
    for (QAction* a : m->actions()) fixAction(a);
}

void translateWidget(QWidget* w) {
    if (!w || w->property("pacitumNoTr").toBool()) return;
    if (!w->toolTip().isEmpty()) fixText(w, "tip", w->toolTip(), [w](const QString& s) { w->setToolTip(s); });
    if (w->isWindow() && !w->windowTitle().isEmpty()) fixText(w, "title", w->windowTitle(), [w](const QString& s) { w->setWindowTitle(s); });
    if (auto* l = qobject_cast<QLabel*>(w)) {
        if (!l->text().isEmpty() && !l->movie() && l->pixmap().isNull())
            fixText(l, "text", l->text(), [l](const QString& s) { l->setText(s); });
    } else if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        fixText(b, "text", b->text(), [b](const QString& s) { b->setText(s); });
    } else if (auto* e = qobject_cast<QLineEdit*>(w)) {
        fixText(e, "ph", e->placeholderText(), [e](const QString& s) { e->setPlaceholderText(s); });
    } else if (auto* g = qobject_cast<QGroupBox*>(w)) {
        fixText(g, "text", g->title(), [g](const QString& s) { g->setTitle(s); });
    } else if (auto* te = qobject_cast<QTextEdit*>(w)) {
        fixText(te, "ph", te->placeholderText(), [te](const QString& s) { te->setPlaceholderText(s); });
    } else if (auto* pe = qobject_cast<QPlainTextEdit*>(w)) {
        fixText(pe, "ph", pe->placeholderText(), [pe](const QString& s) { pe->setPlaceholderText(s); });
    } else if (auto* m = qobject_cast<QMenu*>(w)) {
        translateMenu(m);
    }
}

class Filter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override {
        if (m_busy) return false;
        switch (e->type()) {
        case QEvent::Polish:
        case QEvent::ToolTipChange:
        case QEvent::WindowTitleChange:
            if (auto* w = qobject_cast<QWidget*>(o)) { m_busy = true; translateWidget(w); m_busy = false; }
            break;
        case QEvent::Show:
            // menus that fill themselves in aboutToShow()
            if (auto* m = qobject_cast<QMenu*>(o); m && !isEnglish()) {
                m_busy = true;
                const QSize before = m->sizeHint();
                translateMenu(m);
                m_busy = false;
                if (m->sizeHint() != before) QTimer::singleShot(0, m, [m] { if (m->isVisible()) m->resize(m->sizeHint()); });
            }
            break;
        default: break;
        }
        return false;
    }
private:
    bool m_busy = false;
};

void retranslateAll() {
    for (QWidget* w : QApplication::allWidgets()) translateWidget(w);
}

}   // namespace

const QList<Info>& available() {
    static QList<Info> all;
    if (all.isEmpty()) {
        const QList<Info> known = {
            {QStringLiteral("en"), QStringLiteral("English"), QStringLiteral("English")},
            {QStringLiteral("th"), QStringLiteral("ไทย"), QStringLiteral("Thai")},
            {QStringLiteral("ja"), QStringLiteral("日本語"), QStringLiteral("Japanese")},
            {QStringLiteral("zh-CN"), QStringLiteral("简体中文"), QStringLiteral("Chinese (Simplified)")},
            {QStringLiteral("zh-TW"), QStringLiteral("繁體中文"), QStringLiteral("Chinese (Traditional)")},
            {QStringLiteral("ko"), QStringLiteral("한국어"), QStringLiteral("Korean")},
            {QStringLiteral("es"), QStringLiteral("Español"), QStringLiteral("Spanish")},
            {QStringLiteral("fr"), QStringLiteral("Français"), QStringLiteral("French")},
            {QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German")},
            {QStringLiteral("pt-BR"), QStringLiteral("Português (Brasil)"), QStringLiteral("Portuguese (Brazil)")},
            {QStringLiteral("ru"), QStringLiteral("Русский"), QStringLiteral("Russian")},
            {QStringLiteral("id"), QStringLiteral("Bahasa Indonesia"), QStringLiteral("Indonesian")},
            {QStringLiteral("vi"), QStringLiteral("Tiếng Việt"), QStringLiteral("Vietnamese")},
            {QStringLiteral("it"), QStringLiteral("Italiano"), QStringLiteral("Italian")},
            {QStringLiteral("tr"), QStringLiteral("Türkçe"), QStringLiteral("Turkish")},
        };
        for (const Info& i : known)
            if (i.code == QLatin1String("en") || QFile::exists(QStringLiteral(":/lang/%1.json").arg(i.code))) all.append(i);
    }
    return all;
}

QString setting() { return AppSettings::get(QStringLiteral("ui_language")).toString(QStringLiteral("system")); }

QString resolve(const QString& choice) {
    const auto& langs = available();
    auto has = [&](const QString& c) { for (const Info& i : langs) if (i.code == c) return true; return false; };
    if (choice != QLatin1String("system") && !choice.isEmpty()) return has(choice) ? choice : QStringLiteral("en");
    for (const QString& u : QLocale::system().uiLanguages()) {
        const QString l = QString(u).replace(QLatin1Char('_'), QLatin1Char('-'));
        const QString base = l.section(QLatin1Char('-'), 0, 0).toLower();
        QString want = l;
        if (base == QLatin1String("zh")) {
            const bool trad = l.contains(QLatin1String("Hant")) || l.endsWith(QLatin1String("-TW")) || l.endsWith(QLatin1String("-HK")) || l.endsWith(QLatin1String("-MO"));
            want = trad ? QStringLiteral("zh-TW") : QStringLiteral("zh-CN");
        } else if (base == QLatin1String("pt")) {
            want = QStringLiteral("pt-BR");
        }
        if (has(want)) return want;
        if (has(base)) return base;
    }
    return QStringLiteral("en");
}

void init() {
    auto& st = S();
    st.choice = setting();
    loadDictionary(resolve(st.choice));
    applyQtLocale();
    static Filter* f = nullptr;
    if (!f) { f = new Filter(qApp); qApp->installEventFilter(f); }
}

void apply(const QString& choice) {
    auto& st = S();
    st.choice = choice.isEmpty() ? QStringLiteral("system") : choice;
    const QString want = resolve(st.choice);
    if (want == st.code) { emit st.notifier.changed(); return; }
    loadDictionary(want);
    applyQtLocale();
    retranslateAll();
    emit st.notifier.changed();
}

QString code() { return S().code; }
bool isEnglish() { return S().code == QLatin1String("en"); }

QString tr(const QString& english) {
    auto& st = S();
    if (st.code == QLatin1String("en") || english.isEmpty()) return english;
    if (auto it = st.cache.constFind(english); it != st.cache.constEnd()) return it.value();
    int a = 0; while (a < english.size() && english[a].isSpace()) ++a;
    int b = english.size(); while (b > a && english[b - 1].isSpace()) --b;
    QString out;
    const QString core = english.mid(a, b - a);
    const int tab = core.indexOf(QLatin1Char('\t'));
    if (tab >= 0) out = english.left(a) + lookupCore(core.left(tab), 0) + core.mid(tab) + english.mid(b);
    else out = english.left(a) + lookupCore(core, 0) + english.mid(b);
    if (st.cache.size() > 6000) st.cache.clear();
    st.cache.insert(english, out);
    return out;
}

QByteArray dictionaryJson() {
    const auto& st = S();
    if (st.code == QLatin1String("en")) return QByteArrayLiteral("{}");
    return QJsonDocument(st.raw).toJson(QJsonDocument::Compact);
}

Notifier* notifier() { return &S().notifier; }

}   // namespace Lang

