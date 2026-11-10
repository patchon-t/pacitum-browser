#include "ExtensionManager.h"
#include "ExtensionAccess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>

namespace {
const QString kPrefix = QStringLiteral("pacitum-ext:");
constexpr qint64 kMaxFile = 4 * 1024 * 1024;   // one script/style file
constexpr qint64 kMaxFolder = 32 * 1024 * 1024;

QJsonObject readJson(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxFile) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QStringList strings(const QJsonValue& v) {
    QStringList out;
    if (v.isString()) out << v.toString();
    for (const auto& x : v.toArray()) if (x.isString()) out << x.toString();
    return out;
}

// A content-script group: either the manifest's top level or one "content_scripts" entry.
struct Group { QStringList matches, excludes, js, css; QString runAt; };

QList<Group> groupsOf(const QJsonObject& m) {
    QList<Group> out;
    auto one = [](const QJsonObject& o) {
        Group g;
        g.matches = strings(o.value(QStringLiteral("matches")));
        g.excludes = strings(o.value(QStringLiteral("exclude_matches")));
        g.js = strings(o.value(QStringLiteral("js")));
        g.css = strings(o.value(QStringLiteral("css")));
        g.runAt = o.value(QStringLiteral("run_at")).toString(QStringLiteral("document_idle"));
        return g;
    };
    const QJsonArray cs = m.value(QStringLiteral("content_scripts")).toArray();
    if (!cs.isEmpty()) { for (const auto& e : cs) out << one(e.toObject()); }
    else out << one(m);
    return out;
}

// Only files inside the extension folder (no "..", no absolute paths).
QString safeFile(const QString& dir, const QString& rel) {
    if (rel.isEmpty() || QDir::isAbsolutePath(rel) || rel.contains(QStringLiteral(".."))) return {};
    const QString p = QDir::cleanPath(dir + QLatin1Char('/') + rel);
    if (!p.startsWith(QDir::cleanPath(dir) + QLatin1Char('/'))) return {};
    return p;
}

QString readText(const QString& path) {
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly) || f.size() > kMaxFile) return {};
    return QString::fromUtf8(f.readAll());
}

QString jsString(const QString& s) {   // JSON-encode a string for embedding in JS
    const QByteArray j = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(j.mid(1, j.size() - 2));
}

bool copyDir(const QString& from, const QString& to) {
    QDir().mkpath(to);
    QDirIterator it(from, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString rel = QDir(from).relativeFilePath(src);
        const QString dst = to + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile::remove(dst);
        if (!QFile::copy(src, dst)) return false;
    }
    return true;
}

qint64 folderSize(const QString& dir) {
    qint64 n = 0;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); n += it.fileInfo().size(); }
    return n;
}
}

ExtensionManager* ExtensionManager::instance() {
    static auto* m = new ExtensionManager(QCoreApplication::instance());
    return m;
}

ExtensionManager::ExtensionManager(QObject* parent) : QObject(parent) { load(); }

QString ExtensionManager::folder() const { return QDir::current().absoluteFilePath(QStringLiteral("extensions")); }

QString ExtensionManager::matchPatternToRegex(const QString& pattern) {
    if (pattern == QLatin1String("<all_urls>")) return QStringLiteral("^(https?|file|ftp)://.*$");
    static const QRegularExpression re(QStringLiteral("^(\\*|https?|file|ftp)://([^/]*)(/.*)$"));
    const auto m = re.match(pattern);
    if (!m.hasMatch()) return {};
    const QString scheme = m.captured(1) == QLatin1String("*") ? QStringLiteral("https?") : QRegularExpression::escape(m.captured(1));
    QString host = m.captured(2);
    QString hostRe;
    if (host == QLatin1String("*")) hostRe = QStringLiteral("[^/]*");
    else if (host.startsWith(QLatin1String("*."))) hostRe = QStringLiteral("([^/]*\\.)?") + QRegularExpression::escape(host.mid(2));
    else hostRe = QRegularExpression::escape(host);
    QString path = QRegularExpression::escape(m.captured(3));
    path.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
    return QStringLiteral("^") + scheme + QStringLiteral("://") + hostRe + path + QStringLiteral("$");
}

bool ExtensionManager::readManifest(const QString& dir, PacitumExtension* out, QString* error) const {
    const QJsonObject m = readJson(dir + QStringLiteral("/manifest.json"));
    if (m.isEmpty()) { if (error) *error = QStringLiteral("No readable manifest.json in that folder."); return false; }
    PacitumExtension e;
    e.name = m.value(QStringLiteral("name")).toString().trimmed();
    if (e.name.isEmpty() || e.name.startsWith(QLatin1String("__MSG_"))) e.name = QFileInfo(dir).fileName();
    e.version = m.value(QStringLiteral("version")).toString(QStringLiteral("1.0"));
    e.description = m.value(QStringLiteral("description")).toString();
    e.dir = dir;
    int usable = 0;
    for (const Group& g : groupsOf(m)) {
        if (g.matches.isEmpty() || (g.js.isEmpty() && g.css.isEmpty())) continue;
        for (const QString& p : g.matches) {
            if (matchPatternToRegex(p).isEmpty()) { if (error) *error = QStringLiteral("Unsupported match pattern: %1").arg(p); return false; }
            if (!e.sites.contains(p)) e.sites << p;
        }
        for (const QString& f : g.js + g.css)
            if (safeFile(dir, f).isEmpty() || !QFile::exists(safeFile(dir, f))) { if (error) *error = QStringLiteral("Missing file: %1").arg(f); return false; }
        ++usable;
    }
    if (!usable) {
        if (error) *error = QStringLiteral("This extension has no content scripts or styles. Pacitum supports script/style extensions only (no background pages or chrome.* APIs).");
        return false;
    }
    e.scriptCount = usable;
    if (out) *out = e;
    return true;
}

void ExtensionManager::load() {
    m_exts.clear();
    const QJsonObject state = readJson(QDir::current().absoluteFilePath(QStringLiteral("extensions.json")));
    const QDir root(folder());
    for (const QString& id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        PacitumExtension e;
        if (!readManifest(root.absoluteFilePath(id), &e, nullptr)) continue;
        e.id = id;
        e.enabled = state.value(id).toObject().value(QStringLiteral("enabled")).toBool(true);
        m_exts << e;
    }
}

void ExtensionManager::saveState() const {
    QJsonObject o;
    for (const auto& e : m_exts) o.insert(e.id, QJsonObject{{QStringLiteral("enabled"), e.enabled}});
    QFile f(QDir::current().absoluteFilePath(QStringLiteral("extensions.json")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson());
}

QString ExtensionManager::install(const QString& sourceDir, QString* error) {
    PacitumExtension e;
    if (!readManifest(sourceDir, &e, error)) return {};
    if (folderSize(sourceDir) > kMaxFolder) { if (error) *error = QStringLiteral("That folder is larger than 32 MB."); return {}; }
    QString base = e.name.toLower();
    base.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    base = base.left(40).trimmed();
    if (base.isEmpty()) base = QStringLiteral("extension");
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(QFileInfo(sourceDir).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex().left(6));
    const QString id = base + QLatin1Char('-') + hash;
    const QString dest = folder() + QLatin1Char('/') + id;
    if (QDir(dest).exists()) QDir(dest).removeRecursively();   // reinstall = update
    if (!copyDir(sourceDir, dest)) { if (error) *error = QStringLiteral("Could not copy the extension files."); QDir(dest).removeRecursively(); return {}; }
    load();
    for (auto& x : m_exts) if (x.id == id) x.enabled = true;
    saveState(); inject(); emit changed();
    return id;
}

void ExtensionManager::setEnabled(const QString& id, bool on) {
    for (auto& e : m_exts) if (e.id == id) e.enabled = on;
    saveState(); inject(); emit changed();
}

void ExtensionManager::remove(const QString& id) {
    ExtensionAccess::forget(id);
    if (id.isEmpty() || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')) || id.startsWith(QLatin1Char('.'))) return;
    QDir(folder() + QLatin1Char('/') + id).removeRecursively();
    load(); saveState(); inject(); emit changed();
}

void ExtensionManager::reload() { load(); inject(); emit changed(); }

void ExtensionManager::attach(QWebEngineProfile* profile, bool privateProfile) {
    if (!profile) return;
    for (const auto& p : std::as_const(m_profiles)) if (p == profile) return;
    for (int i = m_profiles.size() - 1; i >= 0; --i) if (!m_profiles[i]) { m_profiles.removeAt(i); m_profilePrivate.removeAt(i); }
    m_profiles << profile;
    m_profilePrivate << privateProfile;
    inject();
}

void ExtensionManager::applyAccess() { inject(); emit changed(); }

void ExtensionManager::inject() {
    // Build the scripts once, then replace ours in every attached profile.
    QList<QWebEngineScript> scripts, privateScripts;
    for (const auto& e : std::as_const(m_exts)) {
        if (!e.enabled) continue;
        const QJsonObject m = readJson(e.dir + QStringLiteral("/manifest.json"));
        const ExtensionAccess::Rule rule = ExtensionAccess::rule(e.id);
        int n = 0;
        for (const Group& g : groupsOf(m)) {
            if (g.matches.isEmpty() || (g.js.isEmpty() && g.css.isEmpty())) continue;
            // 2.0.3 site access: only the sites the user allows (user scripts have no toolbar button, so "When I
            // click it" allows nothing until a site is added).
            const QStringList matches = rule.mode == ExtensionAccess::Mode::All ? g.matches
                                      : ExtensionAccess::narrowPatterns(g.matches, ExtensionAccess::allowedHosts(e.id));
            if (matches.isEmpty()) continue;
            QStringList inc, exc;
            for (const QString& p : matches) if (const QString r = matchPatternToRegex(p); !r.isEmpty()) inc << jsString(r);
            for (const QString& p : g.excludes) if (const QString r = matchPatternToRegex(p); !r.isEmpty()) exc << jsString(r);
            QString css;
            for (const QString& f : g.css) css += readText(safeFile(e.dir, f)) + QLatin1Char('\n');
            QString js;
            for (const QString& f : g.js) js += readText(safeFile(e.dir, f)) + QStringLiteral("\n;\n");
            QString src = QStringLiteral(
                "(function(){var u=location.href;var inc=[%1],exc=[%2];"
                "if(!inc.some(function(p){return new RegExp(p).test(u);})||exc.some(function(p){return new RegExp(p).test(u);}))return;")
                .arg(inc.join(QLatin1Char(',')), exc.join(QLatin1Char(',')));
            if (!css.trimmed().isEmpty())
                src += QStringLiteral("var st=document.createElement('style');st.dataset.pacitumExtension=%1;st.textContent=%2;"
                                      "(document.head||document.documentElement).appendChild(st);").arg(jsString(e.id), jsString(css));
            if (!js.trimmed().isEmpty())
                src += QStringLiteral("try{(function(){\n%1\n})();}catch(err){console.error('[Pacitum extension '+%2+']',err);}").arg(js, jsString(e.name));
            src += QStringLiteral("})();");
            QWebEngineScript s;
            s.setName(kPrefix + e.id + QLatin1Char(':') + QString::number(n++));
            s.setSourceCode(src);
            s.setWorldId(QWebEngineScript::UserWorld);   // 2.0.8: its own world, away from Pacitum's password helper
            s.setRunsOnSubFrames(false);
            s.setInjectionPoint(g.runAt == QLatin1String("document_start") ? QWebEngineScript::DocumentCreation
                                : g.runAt == QLatin1String("document_end") ? QWebEngineScript::DocumentReady
                                : QWebEngineScript::Deferred);
            scripts << s;
            if (rule.allowPrivate) privateScripts << s;
        }
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        if (!p) continue;
        QWebEngineScriptCollection* col = p->scripts();
        for (const QWebEngineScript& s : col->toList()) if (s.name().startsWith(kPrefix)) col->remove(s);
        for (const QWebEngineScript& s : std::as_const(m_profilePrivate.value(i) ? privateScripts : scripts)) col->insert(s);
    }
}
