#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QStringList>
#include <QByteArray>

// 2.0.1 language system. Pacitum's own screens (menus, dialogs, bubbles, Settings, New tab ...) follow the
// display language chosen in Settings > Languages, or the Windows / system language by default.
//
// The program text is English. A language file (resources/lang/<code>.json) maps an English text to its
// translation; anything without an entry is shown in English. Entries may contain %1, %2 ... for numbers and
// names ("%1 passwords"): they match any text of that shape, so formatted text needs no code change.
//
// Qt widgets are translated by an application-wide filter (labels, buttons, menus, tool tips, window titles,
// placeholders); the internal pages translate their own text with the same dictionary (pages/i18n.js).
namespace Lang {

struct Info { QString code, native, english; };

// Languages that ship with Pacitum (English first).
const QList<Info>& available();

QString setting();                         // what the user chose: "system" or a language code
QString resolve(const QString& choice);    // "system" -> best match for the OS language, otherwise "en"
void init();                               // start of the program: load, install the widget filter, set the locale
void apply(const QString& choice);         // switch now: retranslates every open window, emits changed()
QString code();                            // language in use, e.g. "th"
bool isEnglish();
QStringList fontFamilies();                // UI faces for the script of the language in use (empty for Latin / Cyrillic languages)
QString cssFontStack();                    // the same as a CSS font-family list ("A", "B"), empty when none
void applyAppFont();                       // put those faces in front of the application font
QString tr(const QString& english);        // translation, or the same text when there is none
QByteArray dictionaryJson();               // dictionary of the language in use, for the internal pages
class Notifier;
Notifier* notifier();                      // signal: changed()

class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
}
