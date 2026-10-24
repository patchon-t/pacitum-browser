#include "ChromeExtensions.h"
#include "AppSettings.h"
#include "ThemeManager.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtEndian>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QTimer>
#include <functional>
#include <memory>
#ifdef PACITUM_CHROME_EXTENSIONS
#include <QWebEngineExtensionManager>
#include <QWebEngineExtensionInfo>
#endif

void pacitumLog(const QString& line);   // main.cpp (startup.log)

namespace {
QJsonObject readManifest(const QString& dir) {
    QFile f(dir + QStringLiteral("/manifest.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    // Chrome allows comments in manifest.json; strip whole-line // comments before parsing.
    QByteArray raw = f.readAll();
    QByteArray clean;
    for (const QByteArray& line : raw.split('\n')) if (!line.trimmed().startsWith("//")) clean += line + '\n';
    return QJsonDocument::fromJson(clean).object();
}

// "__MSG_appName__" -> text from _locales/<lang or default_locale>/messages.json
QString localized(const QString& dir, const QJsonObject& m, const QString& value) {
    static const QRegularExpression re(QStringLiteral("^__MSG_(\\w+)__$"));
    const auto match = re.match(value);
    if (!match.hasMatch()) return value;
    const QString key = match.captured(1).toLower();
    QStringList langs{QLocale::system().name(), QLocale::system().name().section(QLatin1Char('_'), 0, 0),
                      m.value(QStringLiteral("default_locale")).toString(), QStringLiteral("en")};
    for (const QString& l : langs) {
        if (l.isEmpty()) continue;
        QFile f(dir + QStringLiteral("/_locales/") + l + QStringLiteral("/messages.json"));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject msgs = QJsonDocument::fromJson(f.readAll()).object();
        for (auto it = msgs.begin(); it != msgs.end(); ++it)
            if (it.key().toLower() == key) return it.value().toObject().value(QStringLiteral("message")).toString();
    }
    return value;
}

QIcon manifestIcon(const QString& dir, const QJsonObject& m) {
    QJsonValue v = m.value(QStringLiteral("action")).toObject().value(QStringLiteral("default_icon"));
    if (v.isUndefined()) v = m.value(QStringLiteral("browser_action")).toObject().value(QStringLiteral("default_icon"));
    if (v.isUndefined()) v = m.value(QStringLiteral("icons"));
    QIcon icon;
    if (v.isString()) icon.addFile(dir + QLatin1Char('/') + v.toString());
    else for (auto it = v.toObject().begin(); it != v.toObject().end(); ++it) {
        const QString file = dir + QLatin1Char('/') + it.value().toString();
        if (QFileInfo::exists(file)) icon.addFile(file, QSize(it.key().toInt(), it.key().toInt()));
    }
    return icon;
}

const char* kStoreHome = "https://chromewebstore.google.com/";
}

ChromeExtensions* ChromeExtensions::instance() { static auto* c = new ChromeExtensions(qApp); return c; }

ChromeExtensions::ChromeExtensions(QObject* parent) : QObject(parent) {
    m_net = new QNetworkAccessManager(this);
}

bool ChromeExtensions::available() {
#ifdef PACITUM_CHROME_EXTENSIONS
    return true;
#else
    return false;
#endif
}

void ChromeExtensions::attach(QWebEngineProfile* profile) {
    if (!profile || m_profile) return;
    m_profile = profile;
#ifdef PACITUM_CHROME_EXTENSIONS
    if (profile->isOffTheRecord()) return;   // Guest: nothing is installed or kept
    auto* mgr = profile->extensionManager();
    if (!mgr) return;
    connect(mgr, &QWebEngineExtensionManager::installFinished, this, [this](const QWebEngineExtensionInfo& e){
        const QByteArray key = m_keys.isEmpty() ? QByteArray() : m_keys.takeFirst();
        if (!e.error().isEmpty()) {
            pacitumLog(QStringLiteral("extension install failed: %1").arg(e.error()));
            emit message(tr("Couldn't add the extension: %1").arg(e.error()));
            emit changed();
            return;
        }
        // 1.9.1: keep the extension's Chrome Web Store ID. Qt installs a package as an unpacked
        // extension, whose ID comes from the "key" in manifest.json, or else from the folder path.
        // Store packages carry the key in the .crx header only, so put it in the manifest and load
        // the extension again (as Chrome does). Extensions that talk to their own ID or to their
        // website (and "Add to Pacitum" knowing it's installed) need the real one.
        const QString want = key.isEmpty() ? QString() : idFromKey(key);
        if (!want.isEmpty() && e.id() != want && addKeyToManifest(e.path(), key)) {
            pacitumLog(QStringLiteral("extension installed: %1 (%2); reloading with its store id %3").arg(e.name(), e.id(), want));
            m_expectId = want;
            m_profile->extensionManager()->unloadExtension(e);
            m_profile->extensionManager()->loadExtension(e.path());
            return;   // finished in loadFinished
        }
        pacitumLog(QStringLiteral("extension installed: %1 (%2)").arg(e.name(), e.id()));
        emit message(tr("Added %1").arg(localized(e.path(), readManifest(e.path()), e.name())));
        emit installed(e.id());
        emit changed();
    });
    connect(mgr, &QWebEngineExtensionManager::uninstallFinished, this, [this](const QWebEngineExtensionInfo& e){
        setPinned(e.id(), false);
        emit changed();
    });
    connect(mgr, &QWebEngineExtensionManager::loadFinished, this, [this](const QWebEngineExtensionInfo& e){
        if (!m_expectId.isEmpty() && (e.id() == m_expectId || !e.error().isEmpty())) {
            const QString id = e.id();
            m_expectId.clear();
            if (!e.error().isEmpty()) emit message(tr("Couldn't add the extension: %1").arg(e.error()));
            else {
                pacitumLog(QStringLiteral("extension ready with store id %1").arg(id));
                emit message(tr("Added %1").arg(localized(e.path(), readManifest(e.path()), e.name())));
                emit installed(id);
            }
        }
        emit changed();
    });
    connect(mgr, &QWebEngineExtensionManager::unloadFinished, this, &ChromeExtensions::changed);
#endif
}

QList<ChromeExtensionItem> ChromeExtensions::items() const {
    QList<ChromeExtensionItem> out;
#ifdef PACITUM_CHROME_EXTENSIONS
    if (!m_profile || m_profile->isOffTheRecord() || !m_profile->extensionManager()) return out;
    for (const QWebEngineExtensionInfo& e : m_profile->extensionManager()->extensions()) {
        if (!e.isInstalled()) continue;   // only what the user added (not Chromium's built-in components)
        ChromeExtensionItem it;
        const QJsonObject m = readManifest(e.path());
        it.id = e.id(); it.path = e.path(); it.error = e.error();
        it.name = localized(e.path(), m, e.name().isEmpty() ? m.value(QStringLiteral("name")).toString() : e.name());
        it.description = localized(e.path(), m, e.description().isEmpty() ? m.value(QStringLiteral("description")).toString() : e.description());
        it.version = m.value(QStringLiteral("version")).toString();
        it.enabled = e.isEnabled(); it.loaded = e.isLoaded();
        it.popupUrl = e.actionPopupUrl();
        QString options = m.value(QStringLiteral("options_ui")).toObject().value(QStringLiteral("page")).toString();
        if (options.isEmpty()) options = m.value(QStringLiteral("options_page")).toString();
        if (!options.isEmpty()) it.optionsUrl = QUrl(QStringLiteral("chrome-extension://%1/%2").arg(e.id(), options));
        it.icon = manifestIcon(e.path(), m);
        it.permissions = describePermissions(e.path());
        out << it;
    }
#endif
    return out;
}

QStringList ChromeExtensions::describePermissions(const QString& dir) {
    const QJsonObject m = readManifest(dir);
    QStringList hosts, out;
    auto addHost = [&hosts](const QString& p){
        if (p == QLatin1String("<all_urls>") || p.startsWith(QLatin1String("*://*/")) || p.startsWith(QLatin1String("http://*/")) || p.startsWith(QLatin1String("https://*/"))) {
            if (!hosts.contains(QObject::tr("all websites"))) hosts.prepend(QObject::tr("all websites"));
        } else if (p.contains(QStringLiteral("://"))) {
            QString h = p.section(QStringLiteral("://"), 1).section(QLatin1Char('/'), 0, 0);
            if (h.startsWith(QStringLiteral("*."))) h = h.mid(2);
            if (!h.isEmpty() && !hosts.contains(h)) hosts << h;
        }
    };
    static const QHash<QString, QString> api{
        {QStringLiteral("tabs"), QObject::tr("See your open tabs and the pages you visit")},
        {QStringLiteral("history"), QObject::tr("Read and change your browsing history")},
        {QStringLiteral("bookmarks"), QObject::tr("Read and change your bookmarks")},
        {QStringLiteral("cookies"), QObject::tr("Read and change cookies")},
        {QStringLiteral("downloads"), QObject::tr("Manage your downloads")},
        {QStringLiteral("webRequest"), QObject::tr("See and change network requests")},
        {QStringLiteral("declarativeNetRequest"), QObject::tr("Block or change network requests")},
        {QStringLiteral("clipboardRead"), QObject::tr("Read what you copy")},
        {QStringLiteral("nativeMessaging"), QObject::tr("Talk to other programs on this computer")},
        {QStringLiteral("management"), QObject::tr("Manage your other extensions")},
        {QStringLiteral("debugger"), QObject::tr("Debug pages (full access to them)")},
        {QStringLiteral("proxy"), QObject::tr("Change your proxy settings")},
        {QStringLiteral("privacy"), QObject::tr("Change privacy settings")},
        {QStringLiteral("geolocation"), QObject::tr("Know your location")},
        {QStringLiteral("notifications"), QObject::tr("Show notifications")},
        {QStringLiteral("scripting"), QObject::tr("Run scripts on pages")},
    };
    for (const auto& v : m.value(QStringLiteral("permissions")).toArray()) {
        const QString p = v.toString();
        if (api.contains(p)) out << api.value(p); else addHost(p);
    }
    for (const auto& v : m.value(QStringLiteral("host_permissions")).toArray()) addHost(v.toString());
    for (const auto& cs : m.value(QStringLiteral("content_scripts")).toArray())
        for (const auto& v : cs.toObject().value(QStringLiteral("matches")).toArray()) addHost(v.toString());
    if (!hosts.isEmpty()) {
        if (hosts.first() == QObject::tr("all websites")) hosts = QStringList{hosts.first()};   // the rest adds nothing
        QStringList shown = hosts.mid(0, 4);
        if (hosts.size() > 4) shown << QObject::tr("%n more", nullptr, int(hosts.size()) - 4);
        out.prepend(QObject::tr("Read and change pages on %1").arg(shown.join(QStringLiteral(", "))));
    }
    return out;
}

// Chrome extension IDs: the first 16 bytes of SHA-256(public key), each nibble written as 'a'..'p'.
QString ChromeExtensions::idFromKey(const QByteArray& derKey) {
    const QByteArray h = QCryptographicHash::hash(derKey, QCryptographicHash::Sha256).left(16);
    QString id;
    for (const char c : h) { id += QChar(u'a' + ((uchar(c) >> 4) & 0xF)); id += QChar(u'a' + (uchar(c) & 0xF)); }
    return id;
}

// The public key in a .crx header (DER SubjectPublicKeyInfo). CRX2 holds one key; CRX3 holds a
// protobuf CrxFileHeader with several proofs, and the extension's own key is the one whose ID
// matches signed_header_data.crx_id.
QByteArray ChromeExtensions::crxPublicKey(const QByteArray& d) {
    if (!d.startsWith("Cr24") || d.size() < 16) return {};
    const quint32 ver = qFromLittleEndian<quint32>(d.constData() + 4);
    if (ver == 2) {
        const quint32 keyLen = qFromLittleEndian<quint32>(d.constData() + 8);
        return qsizetype(16 + keyLen) <= d.size() ? d.mid(16, keyLen) : QByteArray();
    }
    if (ver != 3) return {};
    const quint32 hlen = qFromLittleEndian<quint32>(d.constData() + 8);
    if (qsizetype(12 + hlen) > d.size()) return {};
    const QByteArray hdr = d.mid(12, hlen);
    // minimal protobuf reader: varints and length-delimited fields only
    auto varint = [](const QByteArray& b, qsizetype& i, bool& ok) -> quint64 {
        quint64 v = 0; int shift = 0;
        while (i < b.size() && shift < 64) { const uchar c = uchar(b[i++]); v |= quint64(c & 0x7F) << shift; if (!(c & 0x80)) return v; shift += 7; }
        ok = false; return 0;
    };
    auto fields = [&varint](const QByteArray& b) {
        QList<QPair<quint64, QByteArray>> out; qsizetype i = 0; bool ok = true;
        while (i < b.size() && ok) {
            const quint64 tag = varint(b, i, ok); if (!ok) break;
            const int wire = int(tag & 7);
            if (wire == 0) { varint(b, i, ok); continue; }
            if (wire != 2) break;
            const quint64 len = varint(b, i, ok);
            if (!ok || i + qsizetype(len) > b.size()) break;
            out << qMakePair(tag >> 3, b.mid(i, qsizetype(len))); i += qsizetype(len);
        }
        return out;
    };
    QByteArray crxId; QList<QByteArray> keys;
    for (const auto& f : fields(hdr)) {
        if (f.first == 2 || f.first == 3) {   // sha256_with_rsa / sha256_with_ecdsa: AsymmetricKeyProof
            for (const auto& g : fields(f.second)) if (g.first == 1) keys << g.second;
        } else if (f.first == 10000) {        // signed_header_data: SignedData { bytes crx_id = 1; }
            for (const auto& g : fields(f.second)) if (g.first == 1) crxId = g.second;
        }
    }
    for (const QByteArray& k : keys)
        if (!crxId.isEmpty() && QCryptographicHash::hash(k, QCryptographicHash::Sha256).left(16) == crxId) return k;
    return {};
}

bool ChromeExtensions::addKeyToManifest(const QString& dir, const QByteArray& key) {
    QFile f(dir + QStringLiteral("/manifest.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray raw = f.readAll(); f.close();
    if (raw.startsWith("\xEF\xBB\xBF")) raw.remove(0, 3);
    QJsonParseError err{};
    QJsonObject m = QJsonDocument::fromJson(raw, &err).object();
    if (err.error != QJsonParseError::NoError || m.isEmpty()) return false;
    if (m.contains(QStringLiteral("key"))) return false;   // it has its own ID already
    m.insert(QStringLiteral("key"), QString::fromLatin1(key.toBase64()));
    QSaveFile out(dir + QStringLiteral("/manifest.json"));
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.write(QJsonDocument(m).toJson(QJsonDocument::Indented));
    return out.commit();
}

QString ChromeExtensions::crxToZip(const QString& crxPath, QString* error) {
    QFile f(crxPath);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 256 * 1024 * 1024) { *error = tr("Can't read %1").arg(QFileInfo(crxPath).fileName()); return {}; }
    const QByteArray d = f.readAll();
    m_lastKey = crxPublicKey(d);
    qsizetype zipAt = -1;
    if (d.startsWith("PK\x03\x04")) zipAt = 0;   // already a zip
    else if (d.startsWith("Cr24") && d.size() > 16) {
        const quint32 ver = qFromLittleEndian<quint32>(d.constData() + 4);
        if (ver == 3) zipAt = 12 + qsizetype(qFromLittleEndian<quint32>(d.constData() + 8));
        else if (ver == 2) zipAt = 16 + qsizetype(qFromLittleEndian<quint32>(d.constData() + 8)) + qFromLittleEndian<quint32>(d.constData() + 12);
    }
    if (zipAt < 0 || zipAt + 4 > d.size() || d.mid(zipAt, 4) != QByteArray("PK\x03\x04", 4)) {
        *error = tr("%1 isn't a Chrome extension package.").arg(QFileInfo(crxPath).fileName());
        return {};
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/PacitumExtensions");
    QDir().mkpath(dir);
    QString name = QFileInfo(crxPath).completeBaseName();
    name.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")));
    const QString zip = dir + QLatin1Char('/') + (name.isEmpty() ? QStringLiteral("extension") : name) + QStringLiteral(".zip");
    QFile out(zip);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(d.constData() + zipAt, d.size() - zipAt) != d.size() - zipAt) {
        *error = tr("Can't write %1").arg(zip); return {};
    }
    return zip;
}

void ChromeExtensions::installFromFile(const QString& path, QWidget* parent) {
#ifdef PACITUM_CHROME_EXTENSIONS
    if (!m_profile || m_profile->isOffTheRecord()) { emit message(tr("Extensions can't be added in Guest mode")); return; }
    const QFileInfo fi(path);
    QString source = path, error;
    QString question;
    if (fi.isDir()) {
        const QJsonObject m = readManifest(path);
        if (m.isEmpty()) { QMessageBox::warning(parent, tr("Add extension"), tr("This folder has no manifest.json.")); return; }
        const QString name = localized(path, m, m.value(QStringLiteral("name")).toString());
        const QStringList perms = describePermissions(path);
        question = tr("Add \"%1\"?").arg(name);
        if (!perms.isEmpty()) question += QStringLiteral("\n\n") + tr("It can:") + QStringLiteral("\n• ") + perms.join(QStringLiteral("\n• "));
    } else {
        source = crxToZip(path, &error);
        if (source.isEmpty()) { QMessageBox::warning(parent, tr("Add extension"), error); return; }
        question = tr("Add the extension in \"%1\"?\n\nOnly add extensions from people you trust: an extension can read and change the pages you visit.").arg(fi.fileName());
    }
    if (QMessageBox::question(parent, tr("Add extension"), question) != QMessageBox::Yes) return;
    installSource(source, fi.isDir() ? QByteArray() : m_lastKey);
#else
    Q_UNUSED(path);
    QMessageBox::information(parent, tr("Chrome extensions"), tr("Chrome extensions need Pacitum 1.9 (built with Qt 6.11). This is the %1 version for computers that can't run it.").arg(QCoreApplication::applicationVersion()));
#endif
}

void ChromeExtensions::installFromWebStore(const QString& urlOrId, const QString& title, QWidget* parent) {
#ifdef PACITUM_CHROME_EXTENSIONS
    if (!m_profile || m_profile->isOffTheRecord()) { emit message(tr("Extensions can't be added in Guest mode")); return; }
    static const QRegularExpression idRe(QStringLiteral("^[a-p]{32}$"));
    const QString id = idRe.match(urlOrId).hasMatch() ? urlOrId : webStoreId(QUrl(urlOrId));
    if (id.isEmpty()) return;
    QString name = title;
    name.remove(QRegularExpression(QStringLiteral("\\s*[-–—|]\\s*Chrome Web Store\\s*$"), QRegularExpression::CaseInsensitiveOption));
    if (name.trimmed().isEmpty()) name = id;
    if (QMessageBox::question(parent, tr("Add to Pacitum"),
            tr("Add \"%1\" from the Chrome Web Store?\n\nAn extension can read and change the pages you visit. Pacitum shows what it may do once it's added.").arg(name)) != QMessageBox::Yes) return;
    downloadFromStore(id, name);
#else
    Q_UNUSED(urlOrId); Q_UNUSED(title);
    installFromFile(QString(), parent);   // explains that 1.9 is needed
#endif
}

void ChromeExtensions::downloadFromStore(const QString& id, const QString& name) {
#ifdef PACITUM_CHROME_EXTENSIONS
    // The store's own download link, the same one Chrome uses.
    QUrl u(QStringLiteral("https://clients2.google.com/service/update2/crx"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("response"), QStringLiteral("redirect"));
    q.addQueryItem(QStringLiteral("prodversion"), QString::fromLatin1(qWebEngineChromiumVersion()));
    q.addQueryItem(QStringLiteral("acceptformat"), QStringLiteral("crx2,crx3"));
    q.addQueryItem(QStringLiteral("x"), QStringLiteral("id=%1&uc").arg(id));
    u.setQuery(q);
    QNetworkRequest r(u);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    r.setRawHeader("User-Agent", m_profile->httpUserAgent().toUtf8());
    r.setTransferTimeout(60000);
    emit message(tr("Downloading %1…").arg(name));
    auto* reply = m_net->get(r);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, name]{
        reply->deleteLater();
        // Only over HTTPS from Google (the redirect lands on a googleusercontent.com host).
        const QUrl final = reply->url();
        if (reply->error() != QNetworkReply::NoError || final.scheme() != QLatin1String("https")
            || !(final.host().endsWith(QStringLiteral(".google.com")) || final.host().endsWith(QStringLiteral(".googleusercontent.com")) || final.host() == QLatin1String("google.com"))) {
            emit message(tr("Couldn't download %1: %2").arg(name, reply->errorString()));
            return;
        }
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/PacitumExtensions");
        QDir().mkpath(dir);
        const QString crx = dir + QLatin1Char('/') + id + QStringLiteral(".crx");
        QFile f(crx);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { emit message(tr("Can't write %1").arg(crx)); return; }
        f.write(reply->readAll()); f.close();
        QString error;
        const QString zip = crxToZip(crx, &error);
        QFile::remove(crx);
        if (zip.isEmpty()) { emit message(error); return; }
        installSource(zip, m_lastKey);
    });
#else
    Q_UNUSED(id); Q_UNUSED(name);
#endif
}

void ChromeExtensions::installSource(const QString& source, const QByteArray& key) {
#ifdef PACITUM_CHROME_EXTENSIONS
    if (!m_profile || !m_profile->extensionManager()) return;
    m_keys << key;   // one per install, in order (installFinished arrives in the same order)
    m_profile->extensionManager()->installExtension(source);
#else
    Q_UNUSED(source); Q_UNUSED(key);
#endif
}

void ChromeExtensions::setEnabled(const QString& id, bool on) {
#ifdef PACITUM_CHROME_EXTENSIONS
    if (!m_profile || !m_profile->extensionManager()) return;
    for (const auto& e : m_profile->extensionManager()->extensions())
        if (e.id() == id) { m_profile->extensionManager()->setExtensionEnabled(e, on); break; }
    emit changed();
#else
    Q_UNUSED(id); Q_UNUSED(on);
#endif
}

void ChromeExtensions::uninstall(const QString& id) {
#ifdef PACITUM_CHROME_EXTENSIONS
    if (!m_profile || !m_profile->extensionManager()) return;
    for (const auto& e : m_profile->extensionManager()->extensions())
        if (e.id() == id) { m_profile->extensionManager()->uninstallExtension(e); break; }
#else
    Q_UNUSED(id);
#endif
}

bool ChromeExtensions::pinned(const QString& id) const {
    return AppSettings::get(QStringLiteral("pinned_extensions")).toArray().contains(id);
}

void ChromeExtensions::setPinned(const QString& id, bool on) {
    QJsonArray a = AppSettings::get(QStringLiteral("pinned_extensions")).toArray();
    const bool has = a.contains(id);
    if (on == has) return;
    if (on) a.append(id);
    else for (int i = a.size() - 1; i >= 0; --i) if (a.at(i).toString() == id) a.removeAt(i);
    AppSettings::set(QStringLiteral("pinned_extensions"), a);
    emit changed();
}

QString ChromeExtensions::webStoreId(const QUrl& url) {
    const QString host = url.host().toLower();
    const bool store = host == QLatin1String("chromewebstore.google.com")
        || (host == QLatin1String("chrome.google.com") && url.path().startsWith(QStringLiteral("/webstore/")));
    if (!store || !url.path().contains(QStringLiteral("/detail/"))) return {};
    static const QRegularExpression idRe(QStringLiteral("^[a-p]{32}$"));
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (auto it = parts.crbegin(); it != parts.crend(); ++it) if (idRe.match(*it).hasMatch()) return *it;
    return {};
}

// ------------------------------------------------------------------------------------- pop-up
namespace {
// The extension's popup.html in a small frameless window under its toolbar button. It grows to
// the page's size (Chrome caps pop-ups at 800 × 600) and closes when focus leaves it.
class ExtensionPopup : public QWidget {
public:
    ExtensionPopup(QWebEngineProfile* profile, const QUrl& url, QWidget* anchor)
        : QWidget(anchor ? anchor->window() : nullptr, Qt::Popup | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_DeleteOnClose);
        const auto& p = ThemeManager::current();
        setStyleSheet(QStringLiteral("ExtensionPopup, QWidget#ExtPopup{background:%1;border:1px solid %2;}").arg(p.toolbar, p.borderStrong));
        setObjectName(QStringLiteral("ExtPopup"));
        auto* l = new QVBoxLayout(this); l->setContentsMargins(1, 1, 1, 1);
        m_view = new QWebEngineView(this);
        auto* page = new QWebEnginePage(profile, m_view);
        m_view->setPage(page);
        l->addWidget(m_view);
        resize(360, 240);
        connect(page, &QWebEnginePage::contentsSizeChanged, this, [this](const QSizeF& s){
            const QSize want(qBound(200, qCeil(s.width()) + 2, 800), qBound(60, qCeil(s.height()) + 2, 600));
            if (want != size()) { resize(want); place(); }
        });
        connect(page, &QWebEnginePage::windowCloseRequested, this, &QWidget::close);   // window.close() in the popup
        m_view->load(url);
        m_anchor = anchor;
        place();
        setWindowOpacity(0.0);
        show();
        auto* a = new QVariantAnimation(this);   // short fade + drop, like the other menus
        a->setStartValue(0.0); a->setEndValue(1.0); a->setDuration(Pacitum::motionMs(160) > 0 ? 160 : 1);
        a->setEasingCurve(QEasingCurve::OutCubic);
        const QPoint end = pos();
        connect(a, &QVariantAnimation::valueChanged, this, [this, end](const QVariant& v){
            const double t = v.toDouble(); setWindowOpacity(t); move(end - QPoint(0, qRound(6 * (1 - t))));
        });
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
private:
    void place() {
        if (!m_anchor) return;
        QPoint at = m_anchor->mapToGlobal(QPoint(m_anchor->width() - width(), m_anchor->height() + 6));
        if (const QScreen* s = m_anchor->screen()) {
            const QRect a = s->availableGeometry();
            at.setX(qBound(a.left() + 4, at.x(), a.right() - width() - 4));
            at.setY(qMin(at.y(), a.bottom() - height() - 4));
        }
        move(at);
    }
    QWebEngineView* m_view = nullptr;
    QPointer<QWidget> m_anchor;
};
}

void ChromeExtensions::openPopup(const QString& id, QWidget* anchor) {
    for (const auto& it : items()) {
        if (it.id != id) continue;
        if (!it.enabled) { emit message(tr("%1 is turned off").arg(it.name)); return; }
        if (it.popupUrl.isValid() && !it.popupUrl.isEmpty()) new ExtensionPopup(m_profile, it.popupUrl, anchor);
        else if (it.optionsUrl.isValid()) emit message(tr("%1 has no pop-up; open its options from the extensions menu").arg(it.name));
        else emit message(tr("%1 works on its own and has no pop-up").arg(it.name));
        return;
    }
}

// ============================================================================ CI self test
// --extensions-test: add extensions the way a user does and switch them on with the same call as
// the menu's "Enabled" item, then visit a page and open the pop-up. Logs each step (a crash shows
// where it stopped) and "EXTENSIONS OK" at the end. 1.9.0 crashed when "Enabled" was clicked.
void ChromeExtensions::runSelfTest(QWidget* anchor, const std::function<void(const QString&)>& openPage) {
#ifdef PACITUM_CHROME_EXTENSIONS
    auto log = [](const QString& s){ pacitumLog(QStringLiteral("EXT TEST ") + s); };
    if (!m_profile || !m_profile->extensionManager()) { log(QStringLiteral("FAIL no extension manager")); QCoreApplication::exit(5); return; }
    // A small Manifest V3 extension with what most store extensions have: a service worker, a
    // content script on every page, storage and a toolbar pop-up.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/PacitumExtTest/pacitum-test-ext");
    QDir(dir).removeRecursively(); QDir().mkpath(dir);
    auto put = [&dir](const char* name, const QByteArray& data){ QFile f(dir + QLatin1Char('/') + QLatin1String(name)); if (f.open(QIODevice::WriteOnly)) f.write(data); };
    put("manifest.json", R"({"manifest_version":3,"name":"Pacitum test extension","version":"1.0",
        "description":"CI test","permissions":["storage"],"host_permissions":["<all_urls>"],
        "background":{"service_worker":"bg.js"},
        "content_scripts":[{"matches":["<all_urls>"],"js":["cs.js"],"run_at":"document_idle"}],
        "action":{"default_popup":"popup.html","default_title":"Test"}})");
    put("bg.js", "chrome.runtime.onInstalled.addListener(() => chrome.storage.local.set({installed: Date.now()}));\n"
                 "chrome.runtime.onMessage.addListener((m, s, reply) => { reply({ok: true}); });\n");
    put("cs.js", "document.documentElement.dataset.pacitumTest = '1'; chrome.runtime.sendMessage({hello: 1}, () => void chrome.runtime.lastError);\n");
    put("popup.html", "<!doctype html><html><body style='width:200px'><p>Pacitum test pop-up</p><script src='popup.js'></script></body></html>");
    put("popup.js", "chrome.storage.local.get('installed', v => { document.body.append(' ' + JSON.stringify(v)); });\n");

    struct Step { QString what; QString source; QString storeId; };
    auto steps = std::make_shared<QList<Step>>(QList<Step>{
        {QStringLiteral("unpacked test extension"), dir, {}},
        {QStringLiteral("store: Enhancer for YouTube"), {}, QStringLiteral("ponfpcnoihfmfllpaingbgckeeldkhle")},
        {QStringLiteral("store: uBlock Origin Lite"), {}, QStringLiteral("ddkjiahejlhfcafbddmgiahcphecmpfh")},
    });
    auto mgr = m_profile->extensionManager();
    auto runNext = std::make_shared<std::function<void()>>();
    auto waiting = std::make_shared<QString>();   // what we wait an installFinished for
    auto fails = std::make_shared<int>(0);
    QPointer<QWidget> a(anchor);
    connect(mgr, &QWebEngineExtensionManager::installFinished, this, [=](const QWebEngineExtensionInfo& e){
        if (waiting->isEmpty() || e.error().isEmpty()) return;
        const QString what = *waiting; waiting->clear();
        log(QStringLiteral("%1: install failed: %2").arg(what, e.error())); ++*fails; QTimer::singleShot(0, this, *runNext);
    });
    connect(this, &ChromeExtensions::installed, this, [=](const QString& id){
        if (waiting->isEmpty()) return;
        const QString what = *waiting; waiting->clear();
        log(QStringLiteral("%1: installed id=%2").arg(what, id));
        QTimer::singleShot(500, this, [=]{
            log(QStringLiteral("%1: enabling").arg(what));
            setEnabled(id, true);   // the menu's "Enabled" item
            log(QStringLiteral("%1: setEnabled returned").arg(what));
            QTimer::singleShot(3000, this, [=]{
                bool on = false;
                for (const auto& it : items()) if (it.id == id) on = it.enabled;
                log(QStringLiteral("%1: enabled=%2; opening a page").arg(what).arg(on));
                if (!on) ++*fails;
                if (openPage) openPage(QStringLiteral("https://example.com/"));
                QTimer::singleShot(5000, this, [=]{
                    log(QStringLiteral("%1: opening the pop-up").arg(what));
                    openPopup(id, a);
                    QTimer::singleShot(3000, this, [=]{
                        for (auto* w : QApplication::topLevelWidgets()) if (w->objectName() == QLatin1String("ExtPopup")) w->close();
                        log(QStringLiteral("%1: turning off and on again").arg(what));
                        setEnabled(id, false);
                        QTimer::singleShot(1000, this, [=]{
                            setEnabled(id, true);
                            QTimer::singleShot(2000, this, [=]{
                                log(QStringLiteral("%1: removing").arg(what));
                                uninstall(id);
                                QTimer::singleShot(1500, this, *runNext);
                            });
                        });
                    });
                });
            });
        });
    });
    *runNext = [=]{
        if (steps->isEmpty()) {
            log(*fails ? QStringLiteral("FAIL (%1 step(s))").arg(*fails) : QStringLiteral("all steps passed"));
            pacitumLog(*fails ? QStringLiteral("EXTENSIONS FAIL") : QStringLiteral("EXTENSIONS OK"));
            QCoreApplication::exit(*fails ? 3 : 0);
            return;
        }
        const Step st = steps->takeFirst();
        *waiting = st.what;
        log(QStringLiteral("%1: installing").arg(st.what));
        if (!st.storeId.isEmpty()) downloadFromStore(st.storeId, st.what);
        else installSource(st.source, {});
        QTimer::singleShot(60000, this, [=]{ if (*waiting == st.what) { log(st.what + QStringLiteral(": no install result after 60 s")); waiting->clear(); ++*fails; (*runNext)(); } });
    };
    (*runNext)();
#else
    Q_UNUSED(anchor); Q_UNUSED(openPage);
    pacitumLog(QStringLiteral("EXTENSIONS OK (this build has no Chrome extensions)"));
    QCoreApplication::exit(0);
#endif
}
