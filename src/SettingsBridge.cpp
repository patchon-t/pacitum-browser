#include "SettingsBridge.h"
#include "AppSettings.h"
#include "ThemeManager.h"
#include "ProfileManager.h"
#include "Passwords.h"
#include "SyncManager.h"
#include "SyncGuide.h"
#include "Updater.h"
#include "FirstRun.h"
#include "SitePermissions.h"
#include "WebProfile.h"
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QStandardPaths>
#include <QBuffer>
#include <QImage>
#include <QLocale>
#include <QUrl>
#include <QTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <QSysInfo>
#include <QDir>

#ifndef PACITUM_UPDATE_TRACK
#define PACITUM_UPDATE_TRACK ""
#endif

namespace {
QVariantMap paletteMap() {
    const auto& p = ThemeManager::current();
    return {{"page", p.page}, {"card", p.toolbar}, {"text", p.text}, {"sec", p.textSecondary}, {"ter", p.textTertiary},
            {"border", p.border}, {"border2", p.borderStrong}, {"field", p.omnibox}, {"surface", p.surface}, {"surfh", p.surfaceHover},
            {"accent", p.accent}, {"onacc", p.onAccent}, {"primary", p.primary}, {"onprimary", p.onPrimary}, {"danger", p.danger},
            {"dangersoft", p.dangerSoft}, {"ok", p.secure}, {"warn", p.warning}, {"chrome", p.frame}, {"disabled", p.disabled},
            {"dark", QColor(p.page).lightness() < 128}};
}
QString siteColor(const QString& host) {
    static const QStringList c{"#151112", "#2F6FEB", "#D9141F", "#1F8F5F", "#5C5450", "#4B3FBF", "#B8470F", "#0E7490"};
    uint h = qHash(host);
    return c.at(int(h % uint(c.size())));
}
QString decisionLabel(Perm::Kind k, Perm::Decision d) {
    switch (d) {
    case Perm::Decision::Allow: return QStringLiteral("Allowed");
    case Perm::Decision::Block: return k == Perm::Kind::Popups ? QStringLiteral("Blocked") : QStringLiteral("Blocked");
    default: return QStringLiteral("Ask first");
    }
}
QString permLabel(Perm::Kind k) {
    switch (k) {
    case Perm::Kind::Popups: return QStringLiteral("Pop-ups and redirects");
    case Perm::Kind::Downloads: return QStringLiteral("Automatic downloads");
    default: return Perm::title(k);
    }
}
QStringList defaultLanguages() {
    QStringList l;
    for (const QString& s : QLocale::system().uiLanguages()) {
        const QString code = s.section(QLatin1Char('-'), 0, 0).toLower();
        if (!code.isEmpty() && !l.contains(code)) l << code;
        if (l.size() >= 2) break;
    }
    if (!l.contains(QStringLiteral("en"))) l << QStringLiteral("en");
    return l;
}
QString languageName(const QString& code) {
    const QLocale loc(code);
    const QString native = loc.nativeLanguageName();
    const QString english = QLocale::languageToString(loc.language());
    if (native.isEmpty() || native.compare(english, Qt::CaseInsensitive) == 0) return english;
    return native + QStringLiteral(" · ") + english;
}
QJsonObject settings() { return AppSettings::read(); }
}

SettingsBridge::SettingsBridge(Hooks hooks, QObject* parent) : QObject(parent), m_h(std::move(hooks)) {
    connect(PasswordStore::instance(), &PasswordStore::changed, this, &SettingsBridge::changed);
    connect(AutofillStore::instance(), &AutofillStore::changed, this, &SettingsBridge::changed);
    connect(SyncManager::instance(), &SyncManager::changed, this, &SettingsBridge::changed);
    connect(ProfileManager::instance(), &ProfileManager::profilesChanged, this, &SettingsBridge::changed);
    connect(ProfileManager::instance(), &ProfileManager::currentProfileChanged, this, &SettingsBridge::changed);
    connect(Updater::instance(), &Updater::statusChanged, this, &SettingsBridge::changed);
    connect(Perm::Store::forProfile(Pacitum::mainProfile()), &Perm::Store::changed, this, &SettingsBridge::changed);
}

QWidget* SettingsBridge::win() const { return m_h.window ? m_h.window() : QApplication::activeWindow(); }

QVariantMap SettingsBridge::state() const {
    const QJsonObject s = settings();
    QVariantMap o;
    o["s"] = s.toVariantMap();
    o["palette"] = paletteMap();
    o["version"] = QCoreApplication::applicationVersion();
    o["qt"] = QString::fromLatin1(qVersion());
    o["chromium"] = QString::fromLatin1(qWebEngineChromiumVersion());
    o["arch"] = QSysInfo::buildCpuArchitecture().contains(QLatin1String("64")) ? QStringLiteral("64-bit") : QStringLiteral("32-bit");
    o["legacy"] = !QStringLiteral(PACITUM_UPDATE_TRACK).isEmpty();
    o["ua"] = Pacitum::mainProfile()->httpUserAgent();
    o["dataDir"] = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
#ifdef Q_OS_WIN
    o["os"] = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    o["os"] = QStringLiteral("mac");
#else
    o["os"] = QStringLiteral("linux");
#endif
    o["downloadDir"] = QDir::toNativeSeparators(s.value(QStringLiteral("download_path")).toString(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)));

    // profile
    auto* pm = ProfileManager::instance();
    const ProfileInfo me = pm->current();
    QVariantMap prof{{"id", me.id}, {"name", me.name}, {"colour", me.colour}, {"email", me.email}, {"guest", pm->isGuest()},
                     {"initial", me.name.isEmpty() ? QStringLiteral("P") : me.name.left(1).toUpper()}};
    if (me.avatar == QLatin1String("photo")) {
        const QImage img = ProfileManager::photoFor(me);
        if (!img.isNull()) { QByteArray png; QBuffer b(&png); b.open(QIODevice::WriteOnly); img.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&b, "PNG");
                             prof["photo"] = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64()); }
    }
    prof["tabs"] = m_h.openTabCount ? m_h.openTabCount() : 0;
    o["profile"] = prof;
    o["profileCount"] = int(pm->profiles().size());
    o["picker"] = pm->showPickerOnStartup();

    // themes
    QVariantList themes;
    for (const QString& id : ThemeManager::themeIds()) themes << QVariantMap{{"id", id}, {"name", ThemeManager::themeName(id)}, {"dark", ThemeManager::themeIsDark(id)}, {"accent", ThemeManager::themeAccent(id)}};
    o["themes"] = themes;

    // passwords (never the passwords themselves)
    auto* ps = PasswordStore::instance();
    const auto chk = ps->lastCheck();
    QVariantList pw;
    for (const auto& c : ps->all()) {
        const QString host = QUrl(c.origin).host();
        QString issue;
        if (chk.weak.contains(c.id)) issue = QStringLiteral("Weak");
        else if (chk.reused.contains(c.id)) issue = QStringLiteral("Reused");
        pw << QVariantMap{{"id", c.id}, {"site", host}, {"origin", c.origin}, {"url", c.url.isEmpty() ? c.origin : c.url}, {"user", c.username}, {"note", c.note},
                          {"letter", host.isEmpty() ? QStringLiteral("?") : host.section(QLatin1Char('.'), host.startsWith(QLatin1String("www.")) ? 1 : 0, 0).left(1).toUpper()},
                          {"color", siteColor(host)}, {"issue", issue}, {"used", double(qMax(c.used, c.created))}};
    }
    o["passwords"] = pw;
    o["pwCheck"] = QVariantMap{{"weak", chk.weak.size()}, {"reused", chk.reused.size()}, {"at", double(chk.at)}};
    o["never"] = ps->neverSaved();

    // autofill
    QVariantList addrs, cards;
    for (const auto& a : AutofillStore::instance()->addresses()) { QVariantMap m = a.toJson().toVariantMap(); m["line"] = a.summary(); addrs << m; }
    for (const auto& c : AutofillStore::instance()->cards())
        cards << QVariantMap{{"id", c.id}, {"name", c.name}, {"nickname", c.nickname}, {"masked", c.masked()}, {"m", c.expMonth}, {"y", c.expYear}};
    o["addresses"] = addrs; o["cards"] = cards;

    // site permissions
    auto* store = Perm::Store::forProfile(Pacitum::mainProfile());
    QVariantList perms;
    for (Perm::Kind k : Perm::settingsKinds()) {
        QVariantList allowed, blocked;
        for (const QString& og : store->originsWith(k, Perm::Decision::Allow)) allowed << og;
        for (const QString& og : store->originsWith(k, Perm::Decision::Block)) blocked << og;
        perms << QVariantMap{{"key", Perm::key(k)}, {"label", permLabel(k)}, {"value", decisionLabel(k, store->defaultFor(k))},
                             {"decision", int(store->defaultFor(k))}, {"twoState", Perm::isTwoState(k)}, {"allowed", allowed}, {"blocked", blocked}};
    }
    o["sitePerms"] = perms;
    QVariantList sites; for (const QString& og : store->origins()) sites << og;
    o["sites"] = sites;

    // languages
    QStringList langs;
    for (const auto& v : s.value(QStringLiteral("languages")).toArray()) langs << v.toString();
    if (langs.isEmpty()) langs = defaultLanguages();
    QVariantList lv; for (const QString& c : langs) lv << QVariantMap{{"code", c}, {"name", languageName(c)}};
    o["languages"] = lv;
    QVariantList avail;
    for (const char* c : {"en", "th", "zh", "ja", "ko", "vi", "id", "ms", "fil", "hi", "bn", "ar", "he", "fa", "tr", "ru", "uk", "pl", "cs", "de", "nl", "fr",
                          "es", "pt", "it", "sv", "da", "nb", "fi", "el", "hu", "ro", "bg", "hr", "sr", "sk", "sl", "lt", "lv", "et", "lo", "km", "my"}) {
        const QString code = QString::fromLatin1(c);
        if (!langs.contains(code)) avail << QVariantMap{{"code", code}, {"name", languageName(code)}};
    }
    o["moreLanguages"] = avail;

    // default browser, updates, sync
    o["isDefault"] = FirstRunBridge::isDefaultBrowserStatic();
    auto* up = Updater::instance();
    o["update"] = QVariantMap{{"status", up->lastStatus()}, {"busy", up->busy()}};
    auto* sm = SyncManager::instance();
    o["sync"] = QVariantMap{{"signedIn", sm->signedIn()}, {"email", sm->account().email}, {"hasClient", sm->hasClient()}, {"busy", sm->busy()},
                            {"status", sm->status()}, {"last", sm->lastSync().isValid() ? QLocale().toString(sm->lastSync(), QLocale::ShortFormat) : QString()},
                            {"bookmarks", sm->syncs(QStringLiteral("bookmarks"))}, {"settings", sm->syncs(QStringLiteral("settings"))},
                            {"notes", sm->syncs(QStringLiteral("notes"))}, {"history", sm->syncs(QStringLiteral("history"))},
                            {"clientId", sm->clientId()}};
    return o;
}

void SettingsBridge::set(const QString& key, const QVariant& value) {
    // Keys the page may write. Anything else is ignored.
    static const QStringList plain{
        "show_bookmarks_bar", "show_home_button", "inline_autocomplete", "show_suggestions", "search_suggest_online", "search_engine",
        "ask_download_location", "show_downloads_when_done", "pin_downloads_button", "offer_translate", "spellcheck", "always_focus_ring",
        "reduce_motion", "caret_browsing", "min_font_size", "default_font_size", "default_zoom", "freeze_background_tabs",
        "gpu_acceleration", "run_in_background", "cookie_mode", "safe_browsing", "https_only", "adblock_enabled", "do_not_track",
        "clear_on_exit", "clear_cookies_on_exit", "tab_size", "startup", "startup_pages", "homepage", "auto_check_updates", "update_feed_url",
        "passwords_offer", "passwords_autosignin", "passwords_pin", "passwords_suggest", "autofill_addresses", "autofill_payments",
        "theme_ntp", "search_shortcuts", "languages", "custom_theme"};
    if (key == QLatin1String("theme")) { if (m_h.setTheme) m_h.setTheme(value.toString()); emit changed(); return; }
    if (key == QLatin1String("accent")) { if (m_h.setAccent) m_h.setAccent(value.toString()); emit changed(); return; }
    if (key == QLatin1String("picker")) { ProfileManager::instance()->setShowPickerOnStartup(value.toBool()); emit changed(); return; }
    if (key.startsWith(QLatin1String("sync."))) { SyncManager::instance()->setSyncs(key.mid(5), value.toBool()); emit changed(); return; }
    if (!plain.contains(key)) return;
    AppSettings::set(key, QJsonValue::fromVariant(value));
    if (key == QLatin1String("startup")) AppSettings::set(QStringLiteral("restore_session"), value.toString() == QLatin1String("restore"));
    if (key == QLatin1String("reduce_motion")) AppSettings::set(QStringLiteral("animations"), !value.toBool());
    if (m_h.settingChanged) m_h.settingChanged(key == QLatin1String("reduce_motion") ? QStringLiteral("animations") : key);
}

QVariant SettingsBridge::act(const QString& name, const QVariantMap& a) {
    auto* ps = PasswordStore::instance();
    const bool needPin = settings().value(QStringLiteral("passwords_pin")).toBool(true);
    auto verify = [&](const QString& why) { return !needPin || OsAuth::verify(win(), why); };

    if (name == QLatin1String("pwReveal")) {
        const Credential c = ps->byId(a.value("id").toString());
        if (c.id.isEmpty() || !verify(QStringLiteral("Pacitum wants to show a saved password"))) return {};
        return c.password;
    }
    if (name == QLatin1String("pwCopy")) {
        const Credential c = ps->byId(a.value("id").toString());
        if (c.id.isEmpty() || !verify(QStringLiteral("Pacitum wants to copy a saved password"))) return false;
        QApplication::clipboard()->setText(c.password);
        const QString copied = c.password;
        QTimer::singleShot(60000, qApp, [copied]{ if (QApplication::clipboard()->text() == copied) QApplication::clipboard()->clear(); });
        return true;
    }
    if (name == QLatin1String("copy")) { QApplication::clipboard()->setText(a.value("text").toString()); return true; }
    if (name == QLatin1String("pwSave")) {
        Credential c; c.id = a.value("id").toString(); c.url = a.value("url").toString().trimmed(); c.username = a.value("user").toString().trimmed();
        c.password = a.value("password").toString(); c.note = a.value("note").toString();
        if (!c.url.contains(QLatin1String("://"))) c.url = QStringLiteral("https://") + c.url;
        if (c.id.isEmpty()) {
            if (PasswordStore::originOf(c.url).isEmpty() || c.password.isEmpty()) return QStringLiteral("Enter a site and a password");
            return ps->add(c);
        }
        const Credential old = ps->byId(c.id);
        if (c.password.isEmpty()) c.password = old.password;   // the page doesn't know it unless revealed
        ps->update(c);
        return c.id;
    }
    if (name == QLatin1String("pwDelete")) { return ps->remove(a.value("id").toString()); }
    if (name == QLatin1String("pwCheck")) { const auto r = ps->check(); emit changed(); return QVariantMap{{"weak", r.weak.size()}, {"reused", r.reused.size()}}; }
    if (name == QLatin1String("pwGenerate")) return PasswordStore::generate();
    if (name == QLatin1String("neverRemove")) { ps->removeNeverSaved(a.value("origin").toString()); return true; }
    if (name == QLatin1String("pwImport")) {
        const QString f = QFileDialog::getOpenFileName(win(), QStringLiteral("Import passwords"), QDir::homePath(), QStringLiteral("Password files (*.csv)"));
        if (f.isEmpty()) return {};
        QString err; const int n = ps->importCsv(f, &err);
        if (!err.isEmpty()) return err;
        return n == 1 ? QStringLiteral("1 password imported") : QStringLiteral("%1 passwords imported").arg(n);
    }
    if (name == QLatin1String("pwExport")) {
        if (!OsAuth::verify(win(), QStringLiteral("Pacitum wants to export your saved passwords"))) return {};
        const QString f = QFileDialog::getSaveFileName(win(), QStringLiteral("Export passwords"), QDir::homePath() + QStringLiteral("/Pacitum Passwords.csv"), QStringLiteral("CSV (*.csv)"));
        if (f.isEmpty()) return {};
        QString err;
        return ps->exportCsv(f, &err) ? QStringLiteral("Passwords exported. Anyone with this file can read them.") : err;
    }
    if (name == QLatin1String("addrSave")) {
        AutofillAddress x = AutofillAddress::fromJson(QJsonObject::fromVariantMap(a));
        return AutofillStore::instance()->saveAddress(x);
    }
    if (name == QLatin1String("addrDelete")) { AutofillStore::instance()->removeAddress(a.value("id").toString()); return true; }
    if (name == QLatin1String("cardSave")) {
        AutofillCard c; c.id = a.value("id").toString(); c.name = a.value("name").toString(); c.nickname = a.value("nickname").toString();
        c.expMonth = a.value("m").toInt(); c.expYear = a.value("y").toInt();
        QString num; for (const QChar ch : a.value("number").toString()) if (ch.isDigit()) num += ch;
        if (!c.id.isEmpty() && num.isEmpty()) for (const auto& e : AutofillStore::instance()->cards()) if (e.id == c.id) num = e.number;
        if (num.size() < 12 || num.size() > 19) return QStringLiteral("Enter the full card number");
        c.number = num;
        return AutofillStore::instance()->saveCard(c);
    }
    if (name == QLatin1String("cardDelete")) { AutofillStore::instance()->removeCard(a.value("id").toString()); return true; }

    if (name == QLatin1String("clearData")) {
        if (m_h.clearData) m_h.clearData(a.value("history").toBool(), a.value("cookies").toBool(), a.value("cache").toBool());
        return true;
    }
    if (name == QLatin1String("chooseDownloadDir")) {
        const QString cur = settings().value(QStringLiteral("download_path")).toString(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        const QString d = QFileDialog::getExistingDirectory(win(), QStringLiteral("Save files to"), cur);
        if (d.isEmpty()) return {};
        AppSettings::set(QStringLiteral("download_path"), d);
        if (m_h.settingChanged) m_h.settingChanged(QStringLiteral("download_path"));
        emit changed();
        return QDir::toNativeSeparators(d);
    }
    if (name == QLatin1String("makeDefault")) {
        FirstRunBridge::requestDefaultBrowserStatic();
        for (int ms : {3000, 8000, 15000, 30000}) QTimer::singleShot(ms, this, [this]{ emit changed(); });
        return true;
    }
    if (name == QLatin1String("checkUpdates")) { if (m_h.checkUpdates) m_h.checkUpdates(); return true; }
    if (name == QLatin1String("openUrl")) {
        const QUrl u(a.value("url").toString());
        if ((u.scheme() == QLatin1String("https") || u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("pacitum")) && m_h.openUrl) m_h.openUrl(u.toString());
        return true;
    }
    if (name == QLatin1String("openProxy")) {
#ifdef Q_OS_WIN
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:network-proxy")));
#elif defined(Q_OS_MACOS)
        QDesktopServices::openUrl(QUrl(QStringLiteral("x-apple.systempreferences:com.apple.Network-Settings.extension")));
#endif
        return true;
    }
    if (name == QLatin1String("openFolder")) { QDesktopServices::openUrl(QUrl::fromLocalFile(a.value("path").toString())); return true; }
    if (name == QLatin1String("pagesCurrent")) {
        const QStringList l = m_h.currentUrls ? m_h.currentUrls() : QStringList();
        AppSettings::set(QStringLiteral("startup_pages"), QJsonArray::fromStringList(l));
        emit changed();
        return l.size();
    }
    if (name == QLatin1String("restart")) { if (m_h.openUrl) m_h.openUrl(QStringLiteral("pacitum://restart")); return true; }
    if (name == QLatin1String("editProfile")) { if (m_h.editProfile) QTimer::singleShot(0, this, [this]{ m_h.editProfile(); }); return true; }
    if (name == QLatin1String("manageProfiles")) { if (m_h.manageProfiles) QTimer::singleShot(0, this, [this]{ m_h.manageProfiles(); }); return true; }
    if (name == QLatin1String("importSources")) return m_h.importSources ? m_h.importSources() : QVariantList{};
    if (name == QLatin1String("import")) {
        if (m_h.startImport) m_h.startImport(a.value("source").toString(), a.value("types").toStringList());
        return true;
    }
    if (name == QLatin1String("importHtml")) {
        const QString f = QFileDialog::getOpenFileName(win(), QStringLiteral("Import bookmarks"), QDir::homePath(), QStringLiteral("Bookmarks (*.html *.htm)"));
        if (f.isEmpty() || !m_h.importBookmarksHtml) return {};
        const int n = m_h.importBookmarksHtml(f);
        return n == 1 ? QStringLiteral("1 bookmark imported") : QStringLiteral("%1 bookmarks imported").arg(n);
    }
    if (name == QLatin1String("permDefault")) {
        Perm::Kind k; if (!Perm::fromKey(a.value("key").toString(), &k)) return false;
        Perm::Store::forProfile(Pacitum::mainProfile())->setDefault(k, Perm::Decision(a.value("decision").toInt()));
        emit changed();
        return true;
    }
    if (name == QLatin1String("permSite")) {
        Perm::Kind k; if (!Perm::fromKey(a.value("key").toString(), &k)) return false;
        Perm::Store::forProfile(Pacitum::mainProfile())->setDecision(a.value("origin").toString(), k, Perm::Decision(a.value("decision").toInt()));
        emit changed();
        return true;
    }
    if (name == QLatin1String("siteDetail")) {
        const QString og = a.value("origin").toString();
        auto* store = Perm::Store::forProfile(Pacitum::mainProfile());
        QVariantList rows;
        for (Perm::Kind k : Perm::settingsKinds())
            rows << QVariantMap{{"key", Perm::key(k)}, {"label", permLabel(k)}, {"decision", int(store->decision(og, k))},
                                {"default", decisionLabel(k, store->defaultFor(k))}, {"twoState", Perm::isTwoState(k)}};
        return QVariantMap{{"origin", og}, {"host", Perm::hostOf(og)}, {"rows", rows}};
    }
    if (name == QLatin1String("siteReset")) {
        const QString og = a.value("origin").toString();
        auto* store = Perm::Store::forProfile(Pacitum::mainProfile());
        for (Perm::Kind k : Perm::settingsKinds()) store->setDecision(og, k, Perm::Decision::Ask);
        emit changed();
        return true;
    }
    if (name == QLatin1String("resetSettings")) {
        // Start page, new tab page, search engine, pinned tabs, extensions, cookies. Bookmarks,
        // history and saved passwords are kept.
        QJsonObject s = settings();
        for (const char* k : {"startup", "startup_pages", "homepage", "restore_session", "search_engine", "search_shortcuts", "theme", "accent", "theme_ntp",
                              "show_bookmarks_bar", "show_home_button", "default_zoom", "default_font_size", "min_font_size", "tab_size", "cookie_mode",
                              "safe_browsing", "https_only", "adblock_enabled", "do_not_track", "inline_autocomplete", "show_suggestions",
                              "search_suggest_online", "ntp_bg", "ntp_tiles", "pinned_tabs"})
            s.remove(QLatin1String(k));
        AppSettings::write(s);
        Pacitum::mainProfile()->cookieStore()->deleteAllCookies();
        if (m_h.resetExtensions) m_h.resetExtensions();
        if (m_h.setTheme) m_h.setTheme(QStringLiteral("System"));
        for (const char* k : {"search_engine", "default_zoom", "show_bookmarks_bar", "adblock_enabled", "cookie_mode", "https_only", "tab_size", "show_home_button"})
            if (m_h.settingChanged) m_h.settingChanged(QLatin1String(k));
        emit changed();
        return true;
    }
    if (name == QLatin1String("sync")) {
        auto* sm = SyncManager::instance();
        const QString what = a.value("do").toString();
        if (what == QLatin1String("signIn")) { if (sm->hasClient()) sm->signIn(); else SyncGuide::open(win()); }
        else if (what == QLatin1String("signOut")) sm->signOut();
        else if (what == QLatin1String("now")) sm->syncNow();
        else if (what == QLatin1String("guide")) SyncGuide::open(win());
        else if (what == QLatin1String("client")) sm->setClient(a.value("id").toString(), a.value("secret").toString());
        else if (what == QLatin1String("photo")) {
            sm->fetchPhoto([](const QImage& img){ if (!img.isNull()) { auto* pm = ProfileManager::instance(); pm->setPhoto(pm->currentId(), img); } });
        }
        return true;
    }
    return {};
}
