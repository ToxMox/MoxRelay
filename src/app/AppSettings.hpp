// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AppSettings -- the standalone persistent-settings store. A thin typed wrapper over a single
// QSettings(IniFormat, UserScope, "MoxRelay", "MoxRelay") instance, which lands at
// %APPDATA%/MoxRelay/MoxRelay.ini (portable, inspectable, and under the same MoxRelay-branded
// %APPDATA%/MoxRelay namespace as the discovery file). The org/app statics + IniFormat default
// are set ONCE in run_gui before any QSettings use (see main.cpp / installFileLogSink ordering).
//
// MODE NOTE: every key here is STANDALONE-only. The tray/window/engine/control keys are consulted
// only when owner-id is empty; in managed (helper) mode the launcher dictates those knobs and the
// tray toggles are forced off. The logging keys (log/level, log/toFile) AND engine/maxSenders (the
// machine-wide Spout sender cap) may apply in both modes (harmless; the MOXRELAY_LOG_FILE env var
// stays the universal override). Nothing here references any host application -- org = app = "MoxRelay".

#pragma once

#include <QSettings>
#include <QString>

namespace moxrelay {

class AppSettings {
public:
	// Constructs the backing QSettings (explicit IniFormat/UserScope/"MoxRelay"/"MoxRelay" so the
	// store is correct even if the default-format static was somehow missed). Cheap to construct.
	AppSettings();

	// --- tray/* (standalone only; forced off in managed mode by TrayController) ---
	bool minimizeToTray() const;            // tray/minimizeToTray (default false)
	void setMinimizeToTray(bool v);
	bool closeToTray() const;               // tray/closeToTray   (default false)
	void setCloseToTray(bool v);

	// --- window/* (standalone only; CLI --start-minimized overrides) ---
	bool startMinimized() const;            // window/startMinimized (default false)
	void setStartMinimized(bool v);

	// --- log/* (may apply in both modes) ---
	QString logLevel() const;               // log/level (default "info"): error/warning/info/debug
	void setLogLevel(const QString &v);
	bool logToFile() const;                 // log/toFile (default false)
	void setLogToFile(bool v);
	// log/dir default = <LocalAppData>/MoxRelay/logs (read-only/display for now; the sink still
	// writes to moduleConfigDir()+"/logs"). Persisted so a future item can make it user-settable.
	QString logDir() const;
	void setLogDir(const QString &v);

	// --- engine/* (standalone only) ---
	int fpsTier() const;                    // engine/fpsTier (default 60)
	void setFpsTier(int v);
	int adapterIndex() const;               // engine/adapterIndex (default 0): GPU adapter index
	void setAdapterIndex(int v);
	// engine/maxSenders is the ONE engine key read in BOTH standalone AND managed mode: the Spout
	// sender cap is a machine-wide user preference (it writes the machine-global Spout MaxSenders
	// registry), unlike the standalone-only adapter/fps keys above.
	int maxSenders() const;                 // engine/maxSenders (default 64): max simultaneous Spout senders
	void setMaxSenders(int v);

	// The default log directory (<LocalAppData>/MoxRelay/logs), resolved from moduleConfigDir().
	// Shared so the dialog and logLevel default can both reference the same string.
	static QString defaultLogDir();

private:
	mutable QSettings settings_;
};

} // namespace moxrelay
