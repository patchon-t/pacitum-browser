#pragma once
// 2.0.5 crash safety for Chrome extensions. An extension that makes Pacitum crash (Tampermonkey did) used to crash
// its profile on every start, until the user found and deleted the extension's folder by hand.
//
// Each start writes startup-guard.json in the profile folder ("running"); a clean exit, or a minute of running,
// clears it. When a start finds it still set, the last run ended abruptly. After that happens twice in a row, or
// once with a fresh crash.log, the most recently added extension is moved out of the profile's extension folder
// (to extensions-quarantine/) before Qt loads it, and the window says which extension was turned off.
#include <QString>

namespace ExtensionSafety {
// Before the main profile is created (QDir::current() is the profile folder). `crashLog`: Pacitum's crash.log.
void beginStartup(const QString& crashLog);
// The extension manager's folder, remembered for the next start (it is only known once the profile exists).
void rememberInstallPath(const QString& path);
// The run got past the risky part (a minute in) or is quitting normally.
void markHealthy();
// The last run ended without a normal quit (known after beginStartup).
bool lastRunEndedAbruptly();
// What was turned off at this start, for the user ("" when nothing was).
QString takeNotice();
}
