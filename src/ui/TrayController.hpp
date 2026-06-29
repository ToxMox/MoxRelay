// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// TrayController -- owns the system-tray icon, its context menu (Show / Quit), and the
// minimize/close-to-tray policy for the MainWindow. Promoted from the inline --start-minimized
// tray block in run_gui so the same tray serves both the managed launch AND the standalone
// minimize/close-to-tray feature.
//
// TRAY PRESENCE: the QSystemTrayIcon is created whenever the OS reports a system tray
// (QSystemTrayIcon::isSystemTrayAvailable()) -- in BOTH modes. When no system tray exists, no icon
// is created and the window falls back to normal/minimized behavior.
//
// TOGGLE policy (UNIFIED across modes): the close/minimize-to-tray decision is read LIVE from the
// persisted tray/closeToTray + tray/minimizeToTray settings on every event, so flipping a toggle in
// Settings takes effect on the very next close/minimize with no restart. There is NO launch-time
// snapshot of the toggles and NO mode gate on them -- managed and standalone honor the same prefs.
// (The managing application's Shutdown verb calls QCoreApplication::quit() directly, bypassing
// closeEvent, so close-to-tray can never swallow the managed stop path regardless of the toggle.)
//
// LIFETIME: constructed in run_gui inside the window scope, parented to the window, so it (and the
// QSystemTrayIcon it owns) is destroyed before libobs shutdown. The MainWindow forwards its
// close/minimize policy questions to this controller via the seams MainWindow exposes.

#pragma once

#include "app/AppSettings.hpp" // live tray-toggle source (held as a member, re-read per event)

#include <QObject>

class QSystemTrayIcon;
class QIcon;

namespace moxrelay {

class MainWindow;

class TrayController : public QObject {
	Q_OBJECT

public:
	// `window` is the shell whose close/minimize policy this drives + whose show/raise/activate the
	// tray restores. `icon` is the app icon shown in the tray. `managed` is owner-id PRESENCE,
	// retained for non-tray mode differences; it does NOT gate the tray toggles (those are unified
	// across modes). The tray icon is created whenever the OS system tray is available.
	TrayController(MainWindow *window, const QIcon &icon, bool managed, const QString &displayName, QObject *parent = nullptr);

	// True when a tray icon was actually created (system tray available). When false, the caller
	// falls back to showMinimized() for the start-hidden case (matches the old inline behavior).
	bool trayAvailable() const { return tray_ != nullptr; }

	// Show + raise + activate the window (the exact restore a normal launch uses). Public so the
	// tray menu / activation and any caller can reuse the one restore path.
	void restoreWindow();

private:
	// Policy questions the MainWindow seams forward here. Each reads the LIVE persisted toggle every
	// call (no snapshot) and applies in BOTH modes -- there is no managed gate on the toggles.
	bool shouldHideOnClose() const;    // tray/closeToTray && tray available
	bool shouldHideOnMinimize() const; // tray/minimizeToTray && tray available
	void hideToTray();

	MainWindow *window_ = nullptr;
	QSystemTrayIcon *tray_ = nullptr;
	bool managed_ = false; // owner-id presence; retained for non-tray mode differences, NOT a toggle gate
	AppSettings settings_; // live tray-toggle source, re-read on every close/minimize event
};

} // namespace moxrelay
