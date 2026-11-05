#pragma once
// 1.8.5 Pacitum mini extensions: user scripts and user styles loaded from folders.
//
// An extension is a folder with a manifest.json:
//   { "name": "Dark YouTube", "version": "1.0", "description": "…",
//     "matches": ["*://*.youtube.com/*"], "exclude_matches": [],
//     "js": ["content.js"], "css": ["style.css"], "run_at": "document_end" }
// A Chrome manifest whose only feature is "content_scripts" works too (same keys per entry).
// Scripts run in an isolated world (like Chrome content scripts); there are no chrome.* APIs.
// Installed copies live in <profile>/extensions/<id>/, state in <profile>/extensions.json.
#include <QObject>
#include <QList>
#include <QPointer>
#include <QStringList>

class QWebEngineProfile;

struct PacitumExtension {
    QString id, name, version, description, dir;
    QStringList sites;     // every "matches" pattern, for display
    bool enabled = true;
    int scriptCount = 0;   // content-script groups that were loaded
};

class ExtensionManager : public QObject {
    Q_OBJECT
public:
    static ExtensionManager* instance();
    // Inject into this profile's pages. A private / guest profile only gets scripts allowed in private tabs (2.0.3).
    void attach(QWebEngineProfile* profile, bool privateProfile = false);
    const QList<PacitumExtension>& extensions() const { return m_exts; }
    QString folder() const;                            // <profile>/extensions
    // Validates <sourceDir>/manifest.json; on success copies the folder and returns the new id.
    bool readManifest(const QString& sourceDir, PacitumExtension* out, QString* error) const;
    QString install(const QString& sourceDir, QString* error);
    void setEnabled(const QString& id, bool on);
    void remove(const QString& id);
    void reload();                                     // re-read every folder from disk
    void applyAccess();                                // 2.0.3: site access / private rules changed
    static QString matchPatternToRegex(const QString& pattern);   // Chrome match pattern → JS regex source
signals:
    void changed();
private:
    explicit ExtensionManager(QObject* parent = nullptr);
    void load();
    void saveState() const;
    void inject();
    QList<PacitumExtension> m_exts;
    QList<QPointer<QWebEngineProfile>> m_profiles;
    QList<bool> m_profilePrivate;                      // parallel to m_profiles
};
