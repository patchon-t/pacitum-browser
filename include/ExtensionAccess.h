#pragma once
// 2.0.3 extension controls: where an extension may read and change pages, and whether it runs in private tabs.
// Shared by Chrome extensions (ChromeExtensions) and Pacitum user scripts (ExtensionManager).
//
//   All    – the sites the extension asks for (as installed).
//   Sites  – only the sites the user lists (never more than the extension asks for).
//   Click  – nothing until the user clicks the extension's toolbar button on a site; that site is then allowed
//            until Pacitum closes.
//
// Chrome has no API for this in Qt WebEngine, so for a Chrome extension the installed manifest.json is narrowed
// (host permissions and content-script matches) and the extension is loaded again. The manifest as it was
// installed is kept next to it (pacitum-original-manifest.json) and put back for "All".
// Rules live in the settings file under "ext_access": { "<id>": { "mode": "all|sites|click", "sites": [...],
// "private": bool } }.
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ExtensionAccess {
enum class Mode { All, Sites, Click };
struct Rule {
    Mode mode = Mode::All;
    QStringList sites;          // bare hosts ("youtube.com"), for Sites
    bool allowPrivate = false;
};

Rule rule(const QString& id);
void setRule(const QString& id, const Rule& r);
void forget(const QString& id);                       // extension removed
QString modeName(Mode m);                             // "all" | "sites" | "click"
Mode modeFromName(const QString& s);

// Click mode: sites allowed by a click, for this run of Pacitum only.
QStringList clickGranted(const QString& id);
bool grantClick(const QString& id, const QString& host);   // false when it was already allowed
// The hosts the rule allows (Sites: the list, Click: the granted ones). Empty for All.
QStringList allowedHosts(const QString& id);

// "https://www.youtube.com/watch?v=1" / "www.youtube.com" / "*.youtube.com" -> "youtube.com"; "" if not a host.
QString normalizeHost(const QString& input);
// Match patterns limited to `hosts`: a broad pattern ("<all_urls>", "*://*/*", "*://*.google.com/*") becomes
// one pattern per allowed host (and its subdomains); a pattern for one site is kept only if that site is allowed.
// The extension never gains a site it didn't ask for.
QStringList narrowPatterns(const QStringList& patterns, const QStringList& hosts);
// True when any pattern reaches every website.
bool patternsReachAll(const QStringList& patterns);

// Chrome manifest helpers.
QJsonObject originalManifest(const QString& dir);      // as installed (the backup, else manifest.json)
QStringList requestedPatterns(const QJsonObject& m);   // host permissions + content-script matches
// Writes manifest.json for the rule (backing up the installed one first). False on a write error.
bool writeManifestForRule(const QString& dir, const QString& id);
}
