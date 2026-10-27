#pragma once
#include <QString>
class QWidget;

// 2.0.1: alert / confirm / prompt, "Leave this site?" and HTTP sign-in, drawn by Pacitum inside
// the tab (design "Web pop-ups and page dialogs"). Blocks only the calling page (nested loop).
namespace PageDialogs {
enum Kind { Alert, Confirm, Prompt, Leave, Auth };
struct Result { bool ok = false; QString text, user, pass; bool suppress = false; };
Result run(QWidget* tab, Kind kind, const QString& host, const QString& message, const QString& value = QString(),
           bool offerSuppress = false, bool insecure = false);
}
