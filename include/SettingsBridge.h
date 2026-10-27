#pragma once
#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QPointer>
#include <functional>

class QWidget;

// 2.0.1: the QWebChannel object behind pacitum://settings (resources/pages/settings.html), the
// Settings screens from the design. The page reads everything with state(), writes plain settings
// with set(key, value) and runs anything else with act(name, args). It is only attached to tabs
// showing pacitum://settings.
class SettingsBridge : public QObject {
    Q_OBJECT
public:
    struct Hooks {
        std::function<QWidget*()> window;                                  // parent for dialogs
        std::function<void(const QString&)> settingChanged;                // apply one setting live
        std::function<void(const QString&)> setTheme;                      // System | Light | Dark | Midnight | ...
        std::function<void(const QString&)> setAccent;                     // "" = the theme's own colour
        std::function<void(bool, bool, bool)> clearData;                   // history, cookies, cache
        std::function<void(const QString&)> openUrl;                       // new tab
        std::function<void()> editProfile, manageProfiles, checkUpdates;
        std::function<QVariantList()> importSources;                       // [{id, name}]
        std::function<void(const QString&, const QStringList&)> startImport;
        std::function<int(const QString&)> importBookmarksHtml;            // file -> count
        std::function<int()> openTabCount;
        std::function<void()> resetExtensions;                             // reset: turn off extensions
        std::function<QStringList()> currentUrls;                          // "Use current pages"
    };
    explicit SettingsBridge(Hooks hooks, QObject* parent = nullptr);

    Q_INVOKABLE QVariantMap state() const;
    Q_INVOKABLE void set(const QString& key, const QVariant& value);
    Q_INVOKABLE QVariant act(const QString& name, const QVariantMap& args);

    // Called by the window when something the page shows changed elsewhere.
    void notifyChanged() { emit changed(); }
    void notifyImportFinished(const QVariantList& results) { emit importFinished(results); }

signals:
    void changed();                                   // page re-reads state()
    void importFinished(const QVariantList& results);
    void toast(const QString& text);

private:
    QWidget* win() const;
    Hooks m_h;
};
