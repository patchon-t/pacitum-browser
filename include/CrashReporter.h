#pragma once
// 2.0.5 crash reports. When the last run crashed or ended abruptly, Pacitum offers to send a report: what happened,
// the Pacitum / Qt / Chromium / system versions, and the logs of that run (startup.log, crash.log). Reports go to the
// Discord webhook built in with -DPACITUM_CRASH_WEBHOOK=... (a repository secret in CI); builds without one never send.
//
// Consent is asked once ("Send automatically from now on" / "Send this time" / "Don't send") and can be changed in
// Settings > System. Before sending, page addresses are cut down to their site, and user names and e-mail addresses
// are removed from the logs. State lives in crash-reports.json next to the logs (shared by all profiles).
#include <QString>
class QWidget;

namespace CrashReporter {
bool available();                       // a webhook was built in
void init(const QString& logDir);       // before startup.log is opened: keeps the previous run's log
void setLastRunAbrupt(bool abrupt);     // from the startup guard (a run that ended without a normal quit)
void maybeReport(QWidget* parent);      // once the window is up: ask / send for the previous run
void reportProblem(QWidget* parent);    // Help > Report a problem: the user's description + this run's log
QString consent();                      // "ask", "on" or "off"
void setConsent(const QString& c);
// For tests: the log text as it would be sent.
QString scrub(const QString& text);
}
