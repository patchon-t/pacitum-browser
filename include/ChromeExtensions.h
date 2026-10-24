#pragma once
// 1.9.0 Chrome extensions (Qt WebEngine 6.10+, QWebEngineExtensionManager).
//
// Real Chromium extensions (chrome.* APIs, background workers, popups) installed into the profile:
// from the Chrome Web Store (the "Add to Pacitum" button on a store page downloads the .crx), a
// .crx or .zip file, or an unpacked folder. Installed extensions load by themselves at startup.
// The legacy build (Qt 6.8, Pacitum 1.8.x) has no extension engine: available() is false there and
// the menus say Chrome extensions need Pacitum 1.9.
#include <QObject>
#include <QIcon>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0) && QT_CONFIG(webengine_extensions)
#define PACITUM_CHROME_EXTENSIONS 1
#endif

class QWebEngineProfile;
class QNetworkAccessManager;
class QWidget;

// What the menus and toolbar need, read from the extension and its manifest.json.
struct ChromeExtensionItem {
    QString id, name, description, version, path, error;
    QUrl popupUrl, optionsUrl;
    QIcon icon;
    QStringList permissions;   // human-readable: "Read and change pages on: …", "Your tabs", …
    bool enabled = true, loaded = false;
};

class ChromeExtensions : public QObject {
    Q_OBJECT
public:
    static ChromeExtensions* instance();
    static bool available();                        // this build has the Chromium extension engine
    void attach(QWebEngineProfile* profile);        // the main (on-disk) profile
    QList<ChromeExtensionItem> items() const;
    // Folder, .zip or .crx. Asks first (with what it may do when that can be read).
    void installFromFile(const QString& path, QWidget* parent);
    // A Chrome Web Store page URL or a 32-letter extension id.
    void installFromWebStore(const QString& urlOrId, const QString& title, QWidget* parent);
    void setEnabled(const QString& id, bool on);
    void uninstall(const QString& id);
    bool pinned(const QString& id) const;
    void setPinned(const QString& id, bool on);
    // The extension's toolbar pop-up (popup.html) under `anchor`, in the extension's own profile.
    void openPopup(const QString& id, QWidget* anchor);
    static QString webStoreId(const QUrl& url);     // "" when the URL isn't a store detail page
    static QStringList describePermissions(const QString& extensionDir);
    // CI (--extensions-test): add, enable, use, disable and remove extensions; logs EXTENSIONS OK|FAIL.
    void runSelfTest(QWidget* anchor, const std::function<void(const QString&)>& openPage);
signals:
    void changed();
    void message(const QString& text);              // short notices ("Added uBlock Origin")
    void installed(const QString& id);              // added and ready (with its final ID)
private:
    explicit ChromeExtensions(QObject* parent);
    QString crxToZip(const QString& crxPath, QString* error);   // also sets m_lastKey
    void installSource(const QString& source, const QByteArray& key);
    static QString idFromKey(const QByteArray& derKey);
    static QByteArray crxPublicKey(const QByteArray& crx);
    static bool addKeyToManifest(const QString& dir, const QByteArray& key);
    QList<QByteArray> m_keys;                       // .crx public key per pending install
    QByteArray m_lastKey;
    QString m_expectId;                             // reloading under its store ID
    void downloadFromStore(const QString& id, const QString& name);   // no questions asked (callers ask)
    QPointer<QWebEngineProfile> m_profile;
    QNetworkAccessManager* m_net = nullptr;
    QStringList m_pending;                          // names being installed (for the notice)
};
