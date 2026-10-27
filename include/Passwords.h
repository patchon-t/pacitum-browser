#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>
#include <QPointer>
#include <QFrame>
#include <functional>

class QWebEnginePage;
class QWidget;
class QLabel;

// 2.0.1 password manager and form autofill (design: Settings › Passwords, Addresses and payments).
//
// Everything is stored per profile (the working directory is the profile folder):
//   passwords.dat  saved sign-ins + "never save" sites
//   autofill.dat   addresses and payment cards
// Both files are encrypted with Vault: Windows DPAPI (tied to the Windows account), elsewhere a
// random per-profile key in vault.key (owner-only) with an HMAC so a damaged file is detected.
namespace Vault {
QByteArray seal(const QByteArray& plain);
QByteArray open(const QByteArray& sealed, bool* ok = nullptr);
}

// Asks the person at the computer to confirm who they are before a password is shown, copied or
// exported. Windows: Windows Hello (PIN, fingerprint, face), or the Windows password when Hello is
// not set up. A successful check is remembered for 60 seconds.
namespace OsAuth {
bool verify(QWidget* parent, const QString& reason);
bool recentlyVerified();
}

struct Credential {
    QString id, origin, url, username, password, note;
    qint64 created = 0, used = 0;
    QJsonObject toJson() const;
    static Credential fromJson(const QJsonObject& o);
};

class PasswordStore : public QObject {
    Q_OBJECT
public:
    static PasswordStore* instance();
    QList<Credential> all() const { return m_items; }
    QList<Credential> forOrigin(const QString& origin) const;
    Credential byId(const QString& id) const;
    QString add(const Credential& c);                       // returns the id; replaces same origin+username
    bool update(const Credential& c);
    bool remove(const QString& id);
    void markUsed(const QString& id);
    QStringList neverSaved() const { return m_never; }
    bool isNeverSaved(const QString& origin) const { return m_never.contains(origin); }
    void addNeverSaved(const QString& origin);
    void removeNeverSaved(const QString& origin);
    // Password check: ids of weak and reused passwords.
    struct Check { QStringList weak, reused; qint64 at = 0; };
    Check check();
    Check lastCheck() const { return m_check; }
    static bool isWeak(const QString& pw);
    static QString generate(int length = 16);
    // Chrome / Edge / Firefox CSV (name,url,username,password[,note]). Returns rows added.
    int importCsv(const QString& path, QString* error = nullptr);
    bool exportCsv(const QString& path, QString* error = nullptr) const;
    static QString originOf(const QString& url);    // scheme://host[:port]
signals:
    void changed();
private:
    explicit PasswordStore(QObject* parent = nullptr);
    void load();
    void save();
    QList<Credential> m_items;
    QStringList m_never;
    Check m_check;
};

struct AutofillAddress {
    QString id, name, organization, street, city, region, postal, country, phone, email;
    QJsonObject toJson() const;
    static AutofillAddress fromJson(const QJsonObject& o);
    QString summary() const;   // one line for lists
};
struct AutofillCard {
    QString id, name, number, nickname;
    int expMonth = 0, expYear = 0;
    QJsonObject toJson() const;
    static AutofillCard fromJson(const QJsonObject& o);
    QString masked() const;    // •••• •••• •••• 4242
};

class AutofillStore : public QObject {
    Q_OBJECT
public:
    static AutofillStore* instance();
    QList<AutofillAddress> addresses() const { return m_addresses; }
    QList<AutofillCard> cards() const { return m_cards; }
    QString saveAddress(const AutofillAddress& a);   // new id when a.id is empty
    QString saveCard(const AutofillCard& c);
    void removeAddress(const QString& id);
    void removeCard(const QString& id);
signals:
    void changed();
private:
    explicit AutofillStore(QObject* parent = nullptr);
    void load();
    void save();
    QList<AutofillAddress> m_addresses;
    QList<AutofillCard> m_cards;
};

// One per tab: injects the form helper into the page (an isolated script world the site's own
// scripts can't reach), fills saved sign-ins and addresses, suggests strong passwords and reports
// submitted sign-ins so the window can offer to save them.
class PasswordAgent : public QObject {
    Q_OBJECT
public:
    PasswordAgent(QWebEnginePage* page, bool offTheRecord, QObject* parent);
    // Fed by the page's console hook. Returns true if the message was ours (then it is swallowed).
    bool handleConsole(const QString& message);
signals:
    void offerSave(const Credential& c, bool update);     // window shows the "Save password?" bubble
    void offerSaveAddress(const AutofillAddress& a);
private:
    void fillPage();
    QPointer<QWebEnginePage> m_page;
    QString m_nonce;
    bool m_otr;
};

// "Save password?" bubble under the address bar.
class SavePasswordBubble : public QFrame {
    Q_OBJECT
public:
    SavePasswordBubble(const Credential& c, bool update, QWidget* anchor);
    void showAt(QWidget* anchor);
};

// "Save address?" bubble.
class SaveAddressBubble : public QFrame {
    Q_OBJECT
public:
    SaveAddressBubble(const AutofillAddress& a, QWidget* anchor);
    void showAt(QWidget* anchor);
};

// Small bubble under the address bar with one main action (e.g. "Translate this page?").
class ActionBubble : public QFrame {
    Q_OBJECT
public:
    ActionBubble(const QString& title, const QString& text, const QString& primary, const QString& secondary,
                 std::function<void()> onPrimary, std::function<void()> onSecondary, QWidget* anchor);
    void showAt(QWidget* anchor);
};
