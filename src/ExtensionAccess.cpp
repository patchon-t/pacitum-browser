#include "ExtensionAccess.h"
#include "AppSettings.h"
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

namespace ExtensionAccess {
namespace {
const char* kKey = "ext_access";
const char* kBackup = "/pacitum-original-manifest.json";
// A content script needs at least one match: this one never matches a real page.
const char* kNowhere = "https://pacitum.invalid/*";

QHash<QString, QStringList>& granted() { static QHash<QString, QStringList> g; return g; }

QJsonObject parse(const QByteArray& raw) {
    // Chrome allows whole-line // comments in manifest.json.
    QByteArray clean;
    for (const QByteArray& line : raw.split('\n')) if (!line.trimmed().startsWith("//")) clean += line + '\n';
    return QJsonDocument::fromJson(clean).object();
}
QByteArray readFile(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

struct Pattern { QString scheme, host, path; bool valid = false; };
Pattern split(const QString& p) {
    if (p == QLatin1String("<all_urls>")) return {QStringLiteral("*"), QStringLiteral("*"), QStringLiteral("/*"), true};
    static const QRegularExpression re(QStringLiteral("^(\\*|https?|wss?|file|ftp)://([^/]*)(/.*)?$"));
    const auto m = re.match(p);
    if (!m.hasMatch()) return {};
    return {m.captured(1), m.captured(2), m.captured(3).isEmpty() ? QStringLiteral("/*") : m.captured(3), true};
}
bool isHostPattern(const QString& p) { return p == QLatin1String("<all_urls>") || p.contains(QLatin1String("://")); }
QStringList strings(const QJsonValue& v) { QStringList out; for (const auto& x : v.toArray()) if (x.isString()) out << x.toString(); return out; }
QJsonArray array(const QStringList& l) { QJsonArray a; for (const QString& s : l) a.append(s); return a; }
}

QString modeName(Mode m) { return m == Mode::Sites ? QStringLiteral("sites") : m == Mode::Click ? QStringLiteral("click") : QStringLiteral("all"); }
Mode modeFromName(const QString& s) { return s == QLatin1String("sites") ? Mode::Sites : s == QLatin1String("click") ? Mode::Click : Mode::All; }

Rule rule(const QString& id) {
    const QJsonObject o = AppSettings::get(QLatin1String(kKey)).toObject().value(id).toObject();
    Rule r;
    r.mode = modeFromName(o.value(QStringLiteral("mode")).toString());
    r.sites = strings(o.value(QStringLiteral("sites")));
    r.allowPrivate = o.value(QStringLiteral("private")).toBool(false);
    return r;
}

void setRule(const QString& id, const Rule& r) {
    QJsonObject all = AppSettings::get(QLatin1String(kKey)).toObject();
    QStringList sites;
    for (const QString& s : r.sites) if (const QString h = normalizeHost(s); !h.isEmpty() && !sites.contains(h)) sites << h;
    all.insert(id, QJsonObject{{"mode", modeName(r.mode)}, {"sites", array(sites)}, {"private", r.allowPrivate}});
    AppSettings::set(QLatin1String(kKey), all);
}

void forget(const QString& id) {
    QJsonObject all = AppSettings::get(QLatin1String(kKey)).toObject();
    all.remove(id);
    AppSettings::set(QLatin1String(kKey), all);
    granted().remove(id);
}

QStringList clickGranted(const QString& id) { return granted().value(id); }

bool grantClick(const QString& id, const QString& host) {
    const QString h = normalizeHost(host);
    if (h.isEmpty() || granted()[id].contains(h)) return false;
    granted()[id] << h;
    return true;
}

QStringList allowedHosts(const QString& id) {
    const Rule r = rule(id);
    if (r.mode == Mode::Sites) return r.sites;
    if (r.mode == Mode::Click) return clickGranted(id);
    return {};
}

QString normalizeHost(const QString& input) {
    QString s = input.trimmed().toLower();
    if (s.isEmpty()) return {};
    if (s.contains(QLatin1String("://"))) s = QUrl(s).host();
    else s = s.section(QLatin1Char('/'), 0, 0).section(QLatin1Char(':'), 0, 0);
    while (s.startsWith(QLatin1String("*."))) s = s.mid(2);
    if (s.startsWith(QLatin1String("www."))) s = s.mid(4);
    static const QRegularExpression ok(QStringLiteral("^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*$"));
    return ok.match(s).hasMatch() ? s : QString();
}

bool patternsReachAll(const QStringList& patterns) {
    for (const QString& p : patterns) { const Pattern x = split(p); if (x.valid && x.host == QLatin1String("*")) return true; }
    return false;
}

QStringList narrowPatterns(const QStringList& patterns, const QStringList& hosts) {
    QStringList out;
    auto add = [&out](const QString& p){ if (!out.contains(p)) out << p; };
    for (const QString& raw : patterns) {
        const Pattern p = split(raw);
        if (!p.valid) continue;
        for (const QString& h : hosts) {
            if (p.host == QLatin1String("*")) {   // every site: give this one (and its subdomains)
                add(p.scheme + QStringLiteral("://") + h + p.path);
                add(p.scheme + QStringLiteral("://*.") + h + p.path);
            } else if (p.host.startsWith(QLatin1String("*."))) {
                const QString base = p.host.mid(2);
                if (h == base || base.endsWith(QLatin1Char('.') + h)) add(raw);   // the user allowed the whole domain
                else if (h.endsWith(QLatin1Char('.') + base)) {                    // a part of the extension's domain
                    add(p.scheme + QStringLiteral("://") + h + p.path);
                    add(p.scheme + QStringLiteral("://*.") + h + p.path);
                }
            } else {
                const QString ph = p.host.section(QLatin1Char(':'), 0, 0);
                const QString bare = ph.startsWith(QLatin1String("www.")) ? ph.mid(4) : ph;
                if (bare == h || bare.endsWith(QLatin1Char('.') + h)) add(raw);
            }
        }
    }
    return out;
}

QJsonObject originalManifest(const QString& dir) {
    const QByteArray backup = readFile(dir + QLatin1String(kBackup));
    return parse(backup.isEmpty() ? readFile(dir + QStringLiteral("/manifest.json")) : backup);
}

QStringList requestedPatterns(const QJsonObject& m) {
    QStringList out = strings(m.value(QStringLiteral("host_permissions")));
    for (const QString& p : strings(m.value(QStringLiteral("permissions")))) if (isHostPattern(p)) out << p;
    for (const auto& cs : m.value(QStringLiteral("content_scripts")).toArray()) out << strings(cs.toObject().value(QStringLiteral("matches")));
    out.removeDuplicates();
    return out;
}

bool writeManifestForRule(const QString& dir, const QString& id) {
    const QString manifestPath = dir + QStringLiteral("/manifest.json");
    const QString backupPath = dir + QLatin1String(kBackup);
    const QByteArray current = readFile(manifestPath);
    if (current.isEmpty()) return false;
    if (!QFile::exists(backupPath)) {   // first change: keep the manifest as installed
        QSaveFile b(backupPath);
        if (!b.open(QIODevice::WriteOnly) || b.write(current) != current.size() || !b.commit()) return false;
    }
    QJsonObject m = parse(readFile(backupPath));
    if (m.isEmpty()) return false;
    // The store "key" may have been added to manifest.json after the backup: keep the extension's ID.
    const QJsonObject cur = parse(current);
    if (cur.contains(QStringLiteral("key"))) m.insert(QStringLiteral("key"), cur.value(QStringLiteral("key")));

    const Rule r = rule(id);
    if (r.mode != Mode::All) {
        const QStringList hosts = allowedHosts(id);
        if (m.contains(QStringLiteral("host_permissions")))
            m.insert(QStringLiteral("host_permissions"), array(narrowPatterns(strings(m.value(QStringLiteral("host_permissions"))), hosts)));
        QStringList perms, hostPerms;
        for (const QString& p : strings(m.value(QStringLiteral("permissions")))) (isHostPattern(p) ? hostPerms : perms) << p;
        perms << narrowPatterns(hostPerms, hosts);
        if (r.mode == Mode::Click && !perms.contains(QStringLiteral("activeTab"))) perms << QStringLiteral("activeTab");
        if (m.contains(QStringLiteral("permissions")) || r.mode == Mode::Click) m.insert(QStringLiteral("permissions"), array(perms));
        QJsonArray scripts;
        for (const auto& v : m.value(QStringLiteral("content_scripts")).toArray()) {
            QJsonObject cs = v.toObject();
            QStringList matches = narrowPatterns(strings(cs.value(QStringLiteral("matches"))), hosts);
            if (matches.isEmpty()) matches << QLatin1String(kNowhere);
            cs.insert(QStringLiteral("matches"), array(matches));
            scripts.append(cs);
        }
        if (m.contains(QStringLiteral("content_scripts"))) m.insert(QStringLiteral("content_scripts"), scripts);
    }
    QSaveFile f(manifestPath);
    const QByteArray out = QJsonDocument(m).toJson(QJsonDocument::Indented);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size() && f.commit();
}
}
