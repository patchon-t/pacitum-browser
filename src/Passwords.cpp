#include "Passwords.h"
#include "AppSettings.h"
#include "ThemeManager.h"
#include "Widgets.h"
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <QUuid>
#include <QUrl>
#include <QSet>
#include <QHash>
#include <QTextStream>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QGraphicsDropShadowEffect>
#include <QEventLoop>
#include <QTimer>
#include <QPointer>
#include <QScreen>
#include <QWindow>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <wincred.h>
#include <dpapi.h>
#ifndef CRED_PACK_PROTECTED_CREDENTIALS
#define CRED_PACK_PROTECTED_CREDENTIALS 0x1
#endif
#include <string>
#include <vector>
#ifdef _MSC_VER   // Windows Hello needs C++/WinRT (MSVC builds)
#define PACITUM_HELLO 1
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>
#include <UserConsentVerifierInterop.h>
#pragma comment(lib, "windowsapp")
#endif
#endif

// ===================================================================== Vault
namespace Vault {
#ifndef Q_OS_WIN
static QByteArray key() {
    static QByteArray k;
    if (!k.isEmpty()) return k;
    QFile f(QStringLiteral("vault.key"));
    if (f.open(QIODevice::ReadOnly)) k = f.readAll();
    if (k.size() != 32) {
        k.resize(32);
        for (int i = 0; i < 32; i += 4) { const quint32 r = QRandomGenerator::system()->generate(); memcpy(k.data() + i, &r, 4); }
        QSaveFile out(QStringLiteral("vault.key"));
        if (out.open(QIODevice::WriteOnly)) { out.write(k); out.commit(); }
        QFile::setPermissions(QStringLiteral("vault.key"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return k;
}
static QByteArray stream(const QByteArray& k, const QByteArray& nonce, int len) {
    QByteArray out; out.reserve(len + 32);
    for (quint32 ctr = 0; out.size() < len; ++ctr) {
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(k); h.addData(nonce); h.addData(QByteArray(reinterpret_cast<const char*>(&ctr), 4));
        out += h.result();
    }
    out.truncate(len);
    return out;
}
#endif

QByteArray seal(const QByteArray& plain) {
#ifdef Q_OS_WIN
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))}, out{};
    if (!CryptProtectData(&in, L"Pacitum", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray r("PMV1W", 5); r += QByteArray(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return r;
#else
    const QByteArray k = key();
    QByteArray nonce(16, Qt::Uninitialized);
    for (int i = 0; i < 16; i += 4) { const quint32 r = QRandomGenerator::system()->generate(); memcpy(nonce.data() + i, &r, 4); }
    QByteArray body = plain;
    const QByteArray ks = stream(k, nonce, body.size());
    for (int i = 0; i < body.size(); ++i) body[i] = char(body[i] ^ ks[i]);
    const QByteArray mac = QMessageAuthenticationCode::hash(nonce + body, k, QCryptographicHash::Sha256);
    return QByteArray("PMV1K", 5) + nonce + mac + body;
#endif
}

QByteArray open(const QByteArray& sealed, bool* ok) {
    if (ok) *ok = false;
    if (sealed.size() < 5) return {};
#ifdef Q_OS_WIN
    if (!sealed.startsWith("PMV1W")) return {};
    DATA_BLOB in{DWORD(sealed.size() - 5), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.constData() + 5))}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray r(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData);
    if (ok) *ok = true;
    return r;
#else
    if (!sealed.startsWith("PMV1K") || sealed.size() < 5 + 16 + 32) return {};
    const QByteArray k = key();
    const QByteArray nonce = sealed.mid(5, 16), mac = sealed.mid(21, 32);
    QByteArray body = sealed.mid(53);
    if (QMessageAuthenticationCode::hash(nonce + body, k, QCryptographicHash::Sha256) != mac) return {};
    const QByteArray ks = stream(k, nonce, body.size());
    for (int i = 0; i < body.size(); ++i) body[i] = char(body[i] ^ ks[i]);
    if (ok) *ok = true;
    return body;
#endif
}
}

// ===================================================================== OS check
namespace OsAuth {
static qint64 s_lastOk = 0;
bool recentlyVerified() { return s_lastOk > 0 && QDateTime::currentMSecsSinceEpoch() - s_lastOk < 60000; }

#ifdef Q_OS_WIN
// Windows password through the standard credential dialog (used when Windows Hello isn't set up).
static bool credUiVerify(HWND hwnd, const QString& reason) {
    const std::wstring caption = L"Pacitum", msg = reason.toStdWString();
    CREDUI_INFOW ci{}; ci.cbSize = sizeof(ci); ci.hwndParent = hwnd; ci.pszCaptionText = caption.c_str(); ci.pszMessageText = msg.c_str();
    ULONG pkg = 0, outSize = 0; LPVOID out = nullptr; BOOL save = FALSE;
    // Pre-fill the current user so only the password is asked.
    wchar_t name[512]; DWORD nlen = 512; std::vector<BYTE> inBuf; ULONG inSize = 0;
    if (GetUserNameW(name, &nlen)) {
        DWORD need = 0;
        CredPackAuthenticationBufferW(0, name, const_cast<LPWSTR>(L""), nullptr, &need);
        inBuf.resize(need);
        if (CredPackAuthenticationBufferW(0, name, const_cast<LPWSTR>(L""), inBuf.data(), &need)) inSize = need; else inBuf.clear();
    }
    const DWORD r = CredUIPromptForWindowsCredentialsW(&ci, 0, &pkg, inBuf.empty() ? nullptr : inBuf.data(), inSize, &out, &outSize, &save, CREDUIWIN_ENUMERATE_CURRENT_USER);
    if (r != ERROR_SUCCESS || !out) return false;
    wchar_t user[514] = {0}, dom[514] = {0}, pw[514] = {0};
    DWORD ul = 513, dl = 513, pl = 513;
    bool good = false;
    if (CredUnPackAuthenticationBufferW(CRED_PACK_PROTECTED_CREDENTIALS, out, outSize, user, &ul, dom, &dl, pw, &pl)) {
        std::wstring u(user), d(dom);
        const auto slash = u.find(L'\\');
        if (slash != std::wstring::npos && d.empty()) { d = u.substr(0, slash); u = u.substr(slash + 1); }
        HANDLE tok = nullptr;
        if (LogonUserW(u.c_str(), d.empty() ? nullptr : d.c_str(), pw, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &tok)) { good = true; CloseHandle(tok); }
        else if (LogonUserW(u.c_str(), d.empty() ? nullptr : d.c_str(), pw, LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &tok)) { good = true; CloseHandle(tok); }
    }
    SecureZeroMemory(pw, sizeof(pw)); SecureZeroMemory(out, outSize); CoTaskMemFree(out);
    return good;
}

// Windows Hello (PIN, face, fingerprint). Returns 1 verified, 0 refused/cancelled, -1 not available.
#ifndef PACITUM_HELLO
static int helloVerify(HWND, const QString&) { return -1; }
#else
static int helloVerify(HWND hwnd, const QString& reason) {
    using namespace winrt::Windows::Security::Credentials::UI;
    try {
        auto interop = winrt::get_activation_factory<UserConsentVerifier, IUserConsentVerifierInterop>();
        winrt::Windows::Foundation::IAsyncOperation<UserConsentVerificationResult> op{nullptr};
        const winrt::hstring msg(reason.toStdWString());
        winrt::check_hresult(interop->RequestVerificationForWindowAsync(hwnd, static_cast<HSTRING>(winrt::get_abi(msg)),
                                                                       winrt::guid_of<decltype(op)>(), winrt::put_abi(op)));
        // Wait without blocking the UI thread (the Hello dialog belongs to our window).
        QEventLoop loop;
        QTimer poll; poll.setInterval(30);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]{ if (op.Status() != winrt::Windows::Foundation::AsyncStatus::Started) loop.quit(); });
        poll.start(); loop.exec();
        if (op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) return 0;
        switch (op.GetResults()) {
        case UserConsentVerificationResult::Verified: return 1;
        case UserConsentVerificationResult::DeviceNotPresent:
        case UserConsentVerificationResult::NotConfiguredForUser:
        case UserConsentVerificationResult::DisabledByPolicy: return -1;
        default: return 0;
        }
    } catch (...) { return -1; }
}
#endif
#endif

bool verify(QWidget* parent, const QString& reason) {
    if (recentlyVerified()) return true;
    bool ok = true;
#ifdef Q_OS_WIN
    QWidget* w = parent ? parent->window() : QApplication::activeWindow();
    HWND hwnd = w ? reinterpret_cast<HWND>(w->winId()) : nullptr;
    const int hello = helloVerify(hwnd, reason);
    ok = hello == 1 || (hello < 0 && credUiVerify(hwnd, reason));
#else
    Q_UNUSED(parent); Q_UNUSED(reason);
#endif
    if (ok) s_lastOk = QDateTime::currentMSecsSinceEpoch();
    return ok;
}
}

// ===================================================================== helpers
static QString newId() { return QUuid::createUuid().toString(QUuid::Id128).left(16); }

static QByteArray readSealed(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};
    bool ok = false;
    const QByteArray plain = Vault::open(f.readAll(), &ok);
    return ok ? plain : QByteArray();
}
static void writeSealed(const QString& file, const QByteArray& plain) {
    const QByteArray sealed = Vault::seal(plain);
    if (sealed.isEmpty()) return;
    QSaveFile f(file);
    if (f.open(QIODevice::WriteOnly)) { f.write(sealed); f.commit(); }
    QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

// ===================================================================== passwords
QJsonObject Credential::toJson() const {
    return {{"id", id}, {"origin", origin}, {"url", url}, {"u", username}, {"p", password}, {"note", note}, {"created", double(created)}, {"used", double(used)}};
}
Credential Credential::fromJson(const QJsonObject& o) {
    Credential c; c.id = o.value("id").toString(); c.origin = o.value("origin").toString(); c.url = o.value("url").toString();
    c.username = o.value("u").toString(); c.password = o.value("p").toString(); c.note = o.value("note").toString();
    c.created = qint64(o.value("created").toDouble()); c.used = qint64(o.value("used").toDouble());
    return c;
}

QString PasswordStore::originOf(const QString& url) {
    QUrl u = QUrl::fromUserInput(url.trimmed());
    if (!u.isValid() || u.host().isEmpty()) return {};
    QString o = u.scheme().toLower() + QStringLiteral("://") + u.host().toLower();
    if (u.port() > 0 && !((u.scheme() == QLatin1String("https") && u.port() == 443) || (u.scheme() == QLatin1String("http") && u.port() == 80)))
        o += QLatin1Char(':') + QString::number(u.port());
    return o;
}

PasswordStore* PasswordStore::instance() { static auto* s = new PasswordStore(qApp); return s; }
PasswordStore::PasswordStore(QObject* parent) : QObject(parent) { load(); }

void PasswordStore::load() {
    const QJsonObject o = QJsonDocument::fromJson(readSealed(QStringLiteral("passwords.dat"))).object();
    m_items.clear();
    for (const auto& v : o.value("items").toArray()) m_items << Credential::fromJson(v.toObject());
    m_never.clear();
    for (const auto& v : o.value("never").toArray()) m_never << v.toString();
    m_check.at = qint64(o.value("checked").toDouble());
}
void PasswordStore::save() {
    QJsonArray items, never;
    for (const auto& c : std::as_const(m_items)) items << c.toJson();
    for (const auto& n : std::as_const(m_never)) never << n;
    writeSealed(QStringLiteral("passwords.dat"), QJsonDocument(QJsonObject{{"v", 1}, {"items", items}, {"never", never}, {"checked", double(m_check.at)}}).toJson(QJsonDocument::Compact));
    emit changed();
}
QList<Credential> PasswordStore::forOrigin(const QString& origin) const {
    QList<Credential> l;
    for (const auto& c : m_items) if (c.origin == origin) l << c;
    std::sort(l.begin(), l.end(), [](const Credential& a, const Credential& b){ return qMax(a.used, a.created) > qMax(b.used, b.created); });
    return l;
}
Credential PasswordStore::byId(const QString& id) const { for (const auto& c : m_items) if (c.id == id) return c; return {}; }
QString PasswordStore::add(const Credential& in) {
    Credential c = in;
    if (c.origin.isEmpty()) c.origin = originOf(c.url);
    if (c.origin.isEmpty()) return {};
    for (auto& e : m_items) if (e.origin == c.origin && e.username == c.username) {
        e.password = c.password; if (!c.note.isEmpty()) e.note = c.note; if (!c.url.isEmpty()) e.url = c.url;
        e.used = QDateTime::currentMSecsSinceEpoch(); save(); return e.id;
    }
    c.id = newId(); c.created = QDateTime::currentMSecsSinceEpoch();
    m_items << c; save();
    return c.id;
}
bool PasswordStore::update(const Credential& c) {
    for (auto& e : m_items) if (e.id == c.id) {
        e.username = c.username; e.password = c.password; e.note = c.note;
        if (!c.url.isEmpty()) { e.url = c.url; const QString o = originOf(c.url); if (!o.isEmpty()) e.origin = o; }
        save(); return true;
    }
    return false;
}
bool PasswordStore::remove(const QString& id) {
    for (int i = 0; i < m_items.size(); ++i) if (m_items[i].id == id) { m_items.removeAt(i); save(); return true; }
    return false;
}
void PasswordStore::markUsed(const QString& id) {
    for (auto& e : m_items) if (e.id == id) { e.used = QDateTime::currentMSecsSinceEpoch(); save(); return; }
}
void PasswordStore::addNeverSaved(const QString& origin) { if (!origin.isEmpty() && !m_never.contains(origin)) { m_never << origin; save(); } }
void PasswordStore::removeNeverSaved(const QString& origin) { if (m_never.removeAll(origin)) save(); }

bool PasswordStore::isWeak(const QString& pw) {
    if (pw.size() < 8) return true;
    static const QSet<QString> common{"password", "password1", "12345678", "123456789", "1234567890", "qwerty123", "qwertyuiop", "11111111",
                                      "iloveyou", "admin123", "welcome1", "abc12345", "password123", "00000000", "88888888", "letmein1"};
    if (common.contains(pw.toLower())) return true;
    int kinds = 0;
    bool lo = false, up = false, dg = false, sy = false;
    for (const QChar ch : pw) { if (ch.isLower()) lo = true; else if (ch.isUpper()) up = true; else if (ch.isDigit()) dg = true; else sy = true; }
    kinds = int(lo) + int(up) + int(dg) + int(sy);
    if (kinds <= 1 && pw.size() < 14) return true;
    QSet<QChar> distinct; for (const QChar ch : pw) distinct.insert(ch);
    return distinct.size() < 4;
}
PasswordStore::Check PasswordStore::check() {
    Check c; c.at = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, QStringList> byPw;
    for (const auto& e : std::as_const(m_items)) { if (isWeak(e.password)) c.weak << e.id; byPw[e.password] << e.id; }
    for (auto it = byPw.cbegin(); it != byPw.cend(); ++it) if (it.value().size() > 1 && !it.key().isEmpty()) c.reused << it.value();
    m_check = c;
    save();
    return c;
}
QString PasswordStore::generate(int length) {
    static const QString sets[] = {QStringLiteral("abcdefghijkmnopqrstuvwxyz"), QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ"), QStringLiteral("23456789"), QStringLiteral("-_.!?#")};
    const QString all = sets[0] + sets[1] + sets[2] + sets[3];
    QString out;
    auto* rng = QRandomGenerator::system();
    for (int i = 0; i < length; ++i) out += all.at(int(rng->bounded(quint32(all.size()))));
    for (int s = 0; s < 4; ++s) out[int(rng->bounded(quint32(length)))] = sets[s].at(int(rng->bounded(quint32(sets[s].size()))));
    return out;
}

static QStringList parseCsvLine(const QString& line, int* pos, const QString& text) {
    Q_UNUSED(line);
    QStringList cells; QString cur; bool q = false;
    int i = *pos;
    for (; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (q) {
            if (ch == QLatin1Char('"')) { if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) { cur += QLatin1Char('"'); ++i; } else q = false; }
            else cur += ch;
        } else if (ch == QLatin1Char('"')) q = true;
        else if (ch == QLatin1Char(',')) { cells << cur; cur.clear(); }
        else if (ch == QLatin1Char('\n')) { ++i; break; }
        else if (ch != QLatin1Char('\r')) cur += ch;
    }
    cells << cur;
    *pos = i;
    return cells;
}
int PasswordStore::importCsv(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = QStringLiteral("Couldn't open the file"); return 0; }
    const QString text = QString::fromUtf8(f.readAll());
    int pos = 0;
    const QStringList head = parseCsvLine(QString(), &pos, text);
    auto col = [&](std::initializer_list<const char*> names) {
        for (int i = 0; i < head.size(); ++i) for (const char* n : names) if (head[i].trimmed().compare(QLatin1String(n), Qt::CaseInsensitive) == 0) return i;
        return -1;
    };
    const int cUrl = col({"url", "login_uri", "website", "origin"}), cUser = col({"username", "login_username", "user", "email"}),
              cPw = col({"password", "login_password"}), cNote = col({"note", "notes", "extra"});
    if (cUrl < 0 || cPw < 0) { if (error) *error = QStringLiteral("This file doesn't look like a password export (needs url and password columns)"); return 0; }
    int added = 0;
    while (pos < text.size()) {
        const QStringList r = parseCsvLine(QString(), &pos, text);
        if (r.size() <= qMax(cUrl, cPw)) continue;
        Credential c; c.url = r.value(cUrl).trimmed(); c.username = cUser >= 0 ? r.value(cUser) : QString(); c.password = r.value(cPw);
        c.note = cNote >= 0 ? r.value(cNote) : QString();
        if (c.password.isEmpty() || originOf(c.url).isEmpty()) continue;
        c.origin = originOf(c.url);
        bool dup = false;
        for (const auto& e : std::as_const(m_items)) if (e.origin == c.origin && e.username == c.username && e.password == c.password) dup = true;
        if (dup) continue;
        c.id = newId(); c.created = QDateTime::currentMSecsSinceEpoch();
        m_items << c; ++added;
    }
    if (added) save();
    return added;
}
static QString csvCell(const QString& s) {
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n'))) { QString t = s; t.replace(QLatin1String("\""), QLatin1String("\"\"")); return QLatin1Char('"') + t + QLatin1Char('"'); }
    return s;
}
bool PasswordStore::exportCsv(const QString& path, QString* error) const {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (error) *error = QStringLiteral("Couldn't write the file"); return false; }
    QString out = QStringLiteral("name,url,username,password,note\n");
    for (const auto& c : m_items)
        out += csvCell(QUrl(c.origin).host()) + QLatin1Char(',') + csvCell(c.url.isEmpty() ? c.origin : c.url) + QLatin1Char(',') + csvCell(c.username)
             + QLatin1Char(',') + csvCell(c.password) + QLatin1Char(',') + csvCell(c.note) + QLatin1Char('\n');
    f.write(out.toUtf8());
    return f.commit();
}

// ===================================================================== autofill
QJsonObject AutofillAddress::toJson() const {
    return {{"id", id}, {"name", name}, {"organization", organization}, {"street", street}, {"city", city}, {"region", region},
            {"postal", postal}, {"country", country}, {"phone", phone}, {"email", email}};
}
AutofillAddress AutofillAddress::fromJson(const QJsonObject& o) {
    AutofillAddress a; a.id = o.value("id").toString(); a.name = o.value("name").toString(); a.organization = o.value("organization").toString();
    a.street = o.value("street").toString(); a.city = o.value("city").toString(); a.region = o.value("region").toString();
    a.postal = o.value("postal").toString(); a.country = o.value("country").toString(); a.phone = o.value("phone").toString(); a.email = o.value("email").toString();
    return a;
}
QString AutofillAddress::summary() const {
    QStringList p;
    for (const QString& s : {street, city, region, postal, country}) if (!s.trimmed().isEmpty()) p << s.trimmed();
    return p.join(QStringLiteral(", "));
}
QJsonObject AutofillCard::toJson() const {
    return {{"id", id}, {"name", name}, {"number", number}, {"nickname", nickname}, {"m", expMonth}, {"y", expYear}};
}
AutofillCard AutofillCard::fromJson(const QJsonObject& o) {
    AutofillCard c; c.id = o.value("id").toString(); c.name = o.value("name").toString(); c.number = o.value("number").toString();
    c.nickname = o.value("nickname").toString(); c.expMonth = o.value("m").toInt(); c.expYear = o.value("y").toInt();
    return c;
}
QString AutofillCard::masked() const {
    QString digits; for (const QChar ch : number) if (ch.isDigit()) digits += ch;
    return QStringLiteral("•••• •••• •••• ") + digits.right(4);
}

AutofillStore* AutofillStore::instance() { static auto* s = new AutofillStore(qApp); return s; }
AutofillStore::AutofillStore(QObject* parent) : QObject(parent) { load(); }
void AutofillStore::load() {
    const QJsonObject o = QJsonDocument::fromJson(readSealed(QStringLiteral("autofill.dat"))).object();
    for (const auto& v : o.value("addresses").toArray()) m_addresses << AutofillAddress::fromJson(v.toObject());
    for (const auto& v : o.value("cards").toArray()) m_cards << AutofillCard::fromJson(v.toObject());
}
void AutofillStore::save() {
    QJsonArray a, c;
    for (const auto& x : std::as_const(m_addresses)) a << x.toJson();
    for (const auto& x : std::as_const(m_cards)) c << x.toJson();
    writeSealed(QStringLiteral("autofill.dat"), QJsonDocument(QJsonObject{{"v", 1}, {"addresses", a}, {"cards", c}}).toJson(QJsonDocument::Compact));
    emit changed();
}
QString AutofillStore::saveAddress(const AutofillAddress& in) {
    AutofillAddress a = in;
    if (a.id.isEmpty()) {
        for (const auto& e : std::as_const(m_addresses))   // same address again: keep one
            if (e.name == a.name && e.street == a.street && e.postal == a.postal) return e.id;
        a.id = newId(); m_addresses << a;
    } else {
        bool found = false;
        for (auto& e : m_addresses) if (e.id == a.id) { e = a; found = true; }
        if (!found) m_addresses << a;
    }
    save();
    return a.id;
}
QString AutofillStore::saveCard(const AutofillCard& in) {
    AutofillCard c = in;
    if (c.id.isEmpty()) { c.id = newId(); m_cards << c; }
    else { bool found = false; for (auto& e : m_cards) if (e.id == c.id) { e = c; found = true; } if (!found) m_cards << c; }
    save();
    return c.id;
}
void AutofillStore::removeAddress(const QString& id) { for (int i = 0; i < m_addresses.size(); ++i) if (m_addresses[i].id == id) { m_addresses.removeAt(i); save(); return; } }
void AutofillStore::removeCard(const QString& id) { for (int i = 0; i < m_cards.size(); ++i) if (m_cards[i].id == id) { m_cards.removeAt(i); save(); return; } }

// ===================================================================== page agent
PasswordAgent::PasswordAgent(QWebEnginePage* page, bool offTheRecord, QObject* parent)
    : QObject(parent), m_page(page), m_otr(offTheRecord) {
    m_nonce = QStringLiteral("⁣pm") + QUuid::createUuid().toString(QUuid::Id128) + QLatin1Char(':');
    static QString src;
    if (src.isEmpty()) { QFile f(QStringLiteral(":/forms/agent.js")); if (f.open(QIODevice::ReadOnly)) src = QString::fromUtf8(f.readAll()); }
    QString code = src; code.replace(QStringLiteral("__PM_NONCE__"), m_nonce);
    QWebEngineScript sc;
    sc.setName(QStringLiteral("pacitum-forms"));
    sc.setSourceCode(code);
    sc.setInjectionPoint(QWebEngineScript::DocumentReady);
    sc.setWorldId(QWebEngineScript::ApplicationWorld);
    sc.setRunsOnSubFrames(false);
    page->scripts().insert(sc);
    connect(page, &QWebEnginePage::loadFinished, this, [this](bool ok){ if (ok) fillPage(); });
    connect(PasswordStore::instance(), &PasswordStore::changed, this, [this]{ if (m_page && !m_page->url().isEmpty()) fillPage(); });
}

void PasswordAgent::fillPage() {
    if (!m_page) return;
    const QUrl u = m_page->url();
    if (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("http")) return;
    const QString origin = PasswordStore::originOf(u.toString());
    const QJsonObject s = AppSettings::read();
    QJsonArray accounts;
    for (const auto& c : PasswordStore::instance()->forOrigin(origin))
        accounts << QJsonObject{{"id", c.id}, {"u", c.username}, {"p", c.password}};
    QJsonArray addrs, cards;
    const bool fillAddr = s.value(QStringLiteral("autofill_addresses")).toBool(true);
    const bool fillPay = s.value(QStringLiteral("autofill_payments")).toBool(false) && u.scheme() == QLatin1String("https");
    if (fillAddr) for (const auto& a : AutofillStore::instance()->addresses()) {
        QJsonObject o = a.toJson(); o.remove(QStringLiteral("id")); o["line"] = a.summary(); addrs << o;
    }
    if (fillPay) for (const auto& c : AutofillStore::instance()->cards())
        cards << QJsonObject{{"id", c.id}, {"masked", c.masked()}, {"sub", c.name}};
    const QJsonObject data{{"accounts", accounts}, {"auto", s.value(QStringLiteral("passwords_autosignin")).toBool(false)},
                           {"suggest", s.value(QStringLiteral("passwords_suggest")).toBool(true)},
                           {"offer", !m_otr && s.value(QStringLiteral("passwords_offer")).toBool(true)},
                           {"addresses", addrs}, {"fillAddresses", fillAddr}, {"cards", cards}, {"fillCards", fillPay}};
    m_page->runJavaScript(QStringLiteral("window.__pmData && window.__pmData(%1)").arg(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))),
                          QWebEngineScript::ApplicationWorld);
}

bool PasswordAgent::handleConsole(const QString& message) {
    if (!message.startsWith(m_nonce)) return false;
    if (!m_page) return true;
    const QJsonObject o = QJsonDocument::fromJson(message.mid(m_nonce.size()).toUtf8()).object();
    const QString t = o.value(QStringLiteral("t")).toString();
    const QString origin = PasswordStore::originOf(m_page->url().toString());
    if (t == QLatin1String("ready")) { fillPage(); return true; }
    if (t == QLatin1String("used")) { const Credential c = PasswordStore::instance()->byId(o.value("id").toString()); if (c.origin == origin) PasswordStore::instance()->markUsed(c.id); return true; }
    if (t == QLatin1String("card")) {
        if (!AppSettings::read().value(QStringLiteral("autofill_payments")).toBool(false)) return true;
        const QString id = o.value("id").toString();
        QPointer<QWebEnginePage> page = m_page;
        QTimer::singleShot(0, this, [this, id, page]{
            if (!page) return;
            for (const auto& c : AutofillStore::instance()->cards()) if (c.id == id) {
                QWebEngineView* v = QWebEngineView::forPage(page); QWidget* w = v ? v->window() : nullptr;
                if (!OsAuth::verify(w, QStringLiteral("Pacitum wants to fill your saved card"))) return;
                const QJsonObject card{{"number", c.number}, {"name", c.name}, {"m", c.expMonth}, {"y", c.expYear}};
                page->runJavaScript(QStringLiteral("window.__pmFillCard && window.__pmFillCard(%1)").arg(QString::fromUtf8(QJsonDocument(card).toJson(QJsonDocument::Compact))),
                                    QWebEngineScript::ApplicationWorld);
            }
        });
        return true;
    }
    if (m_otr || origin.isEmpty()) return true;
    const QJsonObject s = AppSettings::read();
    if (t == QLatin1String("cap")) {
        if (!s.value(QStringLiteral("passwords_offer")).toBool(true) || PasswordStore::instance()->isNeverSaved(origin)) return true;
        Credential c; c.origin = origin; c.url = m_page->url().toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
        c.username = o.value("u").toString().trimmed(); c.password = o.value("p").toString();
        if (c.password.isEmpty() || c.password.size() > 512) return true;
        bool update = false;
        for (const auto& e : PasswordStore::instance()->forOrigin(origin)) {
            if (e.username == c.username || (c.username.isEmpty() && PasswordStore::instance()->forOrigin(origin).size() == 1)) {
                if (e.password == c.password) return true;   // already saved
                update = true; if (c.username.isEmpty()) c.username = e.username;
            }
        }
        emit offerSave(c, update);
        return true;
    }
    if (t == QLatin1String("addr")) {
        if (!s.value(QStringLiteral("autofill_addresses")).toBool(true)) return true;
        const QJsonObject a = o.value("a").toObject();
        AutofillAddress addr; addr.name = a.value("name").toString(); addr.organization = a.value("organization").toString();
        addr.street = (a.value("street").toString() + QLatin1Char(' ') + a.value("street2").toString()).trimmed();
        addr.city = a.value("city").toString(); addr.region = a.value("region").toString(); addr.postal = a.value("postal").toString();
        addr.country = a.value("country").toString(); addr.phone = a.value("phone").toString(); addr.email = a.value("email").toString();
        for (const auto& e : AutofillStore::instance()->addresses()) if (e.street == addr.street && e.postal == addr.postal) return true;
        emit offerSaveAddress(addr);
        return true;
    }
    return true;
}

// ===================================================================== bubbles
namespace {
QString bubbleStyle() {
    const auto& p = ThemeManager::current();
    return QStringLiteral(
        "#PmBubble{background:%1;border:1px solid %2;border-radius:18px;}"
        "#PmBubble QLabel{color:%3;background:transparent;}"
        "#PmTitle{font-size:16px;font-weight:600;}"
        "#PmSub{color:%4;font-size:13px;}"
        "#PmField{background:%5;border:0;border-radius:12px;min-height:40px;padding:0 12px;font-size:14px;color:%3;}"
        "#PmPrimary{background:%6;color:%7;border:0;border-radius:12px;min-height:38px;padding:0 16px;font-weight:600;font-size:13.5px;}"
        "#PmSecondary{background:%5;color:%3;border:0;border-radius:12px;min-height:38px;padding:0 14px;font-weight:600;font-size:13.5px;}"
        "#PmSecondary:hover{background:%2;}")
        .arg(p.toolbar, p.border, p.text, p.textSecondary, p.omnibox, p.primary, p.onPrimary);
}
void placeUnder(QFrame* b, QWidget* anchor) {
    b->adjustSize();
    if (!anchor) { b->show(); return; }
    const QPoint g = anchor->mapToGlobal(QPoint(anchor->width() - b->width() - 8, anchor->height() + 6));
    QRect r(g, b->size());
    if (auto* sc = anchor->screen()) { const QRect a = sc->availableGeometry(); r.moveLeft(qBound(a.left() + 8, r.left(), a.right() - r.width() - 8)); }
    b->move(r.topLeft());
    b->show();
}
}

SavePasswordBubble::SavePasswordBubble(const Credential& in, bool update, QWidget* anchor) : QFrame(anchor ? anchor->window() : nullptr, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("PmBubble")); setAttribute(Qt::WA_DeleteOnClose); setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(bubbleStyle()); setFixedWidth(380);
    auto* l = new QVBoxLayout(this); l->setContentsMargins(20, 18, 20, 18); l->setSpacing(10);
    auto* t = new QLabel(update ? QStringLiteral("Update saved password?") : QStringLiteral("Save password?"), this); t->setObjectName(QStringLiteral("PmTitle"));
    auto* s = new QLabel(QUrl(in.origin).host(), this); s->setObjectName(QStringLiteral("PmSub"));
    auto* user = new QLineEdit(in.username, this); user->setObjectName(QStringLiteral("PmField")); user->setPlaceholderText(QStringLiteral("Username"));
    auto* pw = new QLineEdit(in.password, this); pw->setObjectName(QStringLiteral("PmField")); pw->setEchoMode(QLineEdit::Password);
    auto* eye = pw->addAction(ThemeManager::icon(QStringLiteral("eye"), ThemeManager::current().textSecondary, 16), QLineEdit::TrailingPosition);
    connect(eye, &QAction::triggered, pw, [pw]{ pw->setEchoMode(pw->echoMode() == QLineEdit::Password ? QLineEdit::Normal : QLineEdit::Password); });
    l->addWidget(t); l->addWidget(s); l->addSpacing(2); l->addWidget(user); l->addWidget(pw);
    auto* row = new QHBoxLayout; row->setSpacing(8);
    auto* never = new QPushButton(update ? QStringLiteral("Not now") : QStringLiteral("Never for this site"), this); never->setObjectName(QStringLiteral("PmSecondary"));
    auto* save = new QPushButton(update ? QStringLiteral("Update") : QStringLiteral("Save"), this); save->setObjectName(QStringLiteral("PmPrimary")); save->setDefault(true);
    for (auto* b : {never, save}) b->setCursor(Qt::PointingHandCursor);
    row->addStretch(1); row->addWidget(never); row->addWidget(save);
    l->addSpacing(4); l->addLayout(row);
    const Credential c = in;
    connect(save, &QPushButton::clicked, this, [this, c, user, pw]{
        Credential x = c; x.username = user->text().trimmed(); x.password = pw->text();
        if (!x.password.isEmpty()) PasswordStore::instance()->add(x);
        close();
    });
    connect(never, &QPushButton::clicked, this, [this, c, update]{ if (!update) PasswordStore::instance()->addNeverSaved(c.origin); close(); });
}
void SavePasswordBubble::showAt(QWidget* anchor) { placeUnder(this, anchor); }

SaveAddressBubble::SaveAddressBubble(const AutofillAddress& a, QWidget* anchor) : QFrame(anchor ? anchor->window() : nullptr, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("PmBubble")); setAttribute(Qt::WA_DeleteOnClose); setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(bubbleStyle()); setFixedWidth(380);
    auto* l = new QVBoxLayout(this); l->setContentsMargins(20, 18, 20, 18); l->setSpacing(8);
    auto* t = new QLabel(QStringLiteral("Save address?"), this); t->setObjectName(QStringLiteral("PmTitle"));
    auto* n = new QLabel(a.name, this); n->setStyleSheet(QStringLiteral("font-weight:600;font-size:14px;"));
    auto* s = new QLabel(a.summary(), this); s->setObjectName(QStringLiteral("PmSub")); s->setWordWrap(true);
    l->addWidget(t); l->addWidget(n); l->addWidget(s);
    auto* row = new QHBoxLayout; row->setSpacing(8);
    auto* no = new QPushButton(QStringLiteral("No thanks"), this); no->setObjectName(QStringLiteral("PmSecondary"));
    auto* save = new QPushButton(QStringLiteral("Save"), this); save->setObjectName(QStringLiteral("PmPrimary")); save->setDefault(true);
    row->addStretch(1); row->addWidget(no); row->addWidget(save);
    l->addSpacing(6); l->addLayout(row);
    connect(save, &QPushButton::clicked, this, [this, a]{ AutofillStore::instance()->saveAddress(a); close(); });
    connect(no, &QPushButton::clicked, this, &QWidget::close);
}
void SaveAddressBubble::showAt(QWidget* anchor) { placeUnder(this, anchor); }

ActionBubble::ActionBubble(const QString& title, const QString& text, const QString& primary, const QString& secondary,
                           std::function<void()> onPrimary, std::function<void()> onSecondary, QWidget* anchor)
    : QFrame(anchor ? anchor->window() : nullptr, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("PmBubble")); setAttribute(Qt::WA_DeleteOnClose); setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(bubbleStyle()); setFixedWidth(360);
    auto* l = new QVBoxLayout(this); l->setContentsMargins(20, 18, 20, 18); l->setSpacing(8);
    auto* t = new QLabel(title, this); t->setObjectName(QStringLiteral("PmTitle")); t->setWordWrap(true);
    auto* d = new QLabel(text, this); d->setObjectName(QStringLiteral("PmSub")); d->setWordWrap(true);
    l->addWidget(t); if (!text.isEmpty()) l->addWidget(d);
    auto* row = new QHBoxLayout; row->setSpacing(8); row->addStretch(1);
    if (!secondary.isEmpty()) {
        auto* b = new QPushButton(secondary, this); b->setObjectName(QStringLiteral("PmSecondary")); b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, onSecondary]{ if (onSecondary) onSecondary(); close(); });
        row->addWidget(b);
    }
    auto* p = new QPushButton(primary, this); p->setObjectName(QStringLiteral("PmPrimary")); p->setCursor(Qt::PointingHandCursor); p->setDefault(true);
    connect(p, &QPushButton::clicked, this, [this, onPrimary]{ if (onPrimary) onPrimary(); close(); });
    row->addWidget(p);
    l->addSpacing(6); l->addLayout(row);
}
void ActionBubble::showAt(QWidget* anchor) { placeUnder(this, anchor); }
