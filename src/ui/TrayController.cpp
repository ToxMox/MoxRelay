// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// TrayController implementation. See the header for the contract + mode model. Promoted from the
// inline --start-minimized tray block in run_gui.

#include "TrayController.hpp"

#include "MainWindow.hpp"

#include "app/AppSettings.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

namespace moxrelay {

TrayController::TrayController(MainWindow *window, const QIcon &icon, bool managed, const QString &displayName, QObject *parent)
	: QObject(parent), window_(window), managed_(managed)
{
	// Create the tray icon whenever the OS system tray is available -- in BOTH modes, regardless of
	// the toggle state. The icon is the app's persistent tray presence; the close/minimize-to-tray
	// decision is made LIVE per event from the current settings (see shouldHideOn*). No launch-time
	// snapshot of the toggles is taken. With no system tray, tray_ stays null and the window falls
	// back to normal/minimized behavior.
	if (QSystemTrayIcon::isSystemTrayAvailable()) {
		tray_ = new QSystemTrayIcon(icon, window_);
		tray_->setToolTip(displayName.isEmpty() ? QStringLiteral("MoxRelay")
							: QStringLiteral("MoxRelay - %1").arg(displayName));

		QMenu *trayMenu = new QMenu(window_);
		QObject::connect(trayMenu->addAction(QStringLiteral("Show MoxRelay")), &QAction::triggered,
				 this, &TrayController::restoreWindow);
		trayMenu->addSeparator();
		// Quit ENDS the process directly (QCoreApplication::quit), bypassing window->close() so it can
		// never be intercepted by close-to-tray. Routing it through close() would let a standalone
		// close-to-tray toggle convert this explicit Quit into a hide -- leaving no way out of the tray.
		// quit() unwinds app.exec() and the normal teardown (engine + libobs shutdown) still runs.
		QObject::connect(trayMenu->addAction(QStringLiteral("Quit MoxRelay")), &QAction::triggered,
				 qApp, &QCoreApplication::quit);
		tray_->setContextMenu(trayMenu);

		QObject::connect(tray_, &QSystemTrayIcon::activated, this,
				 [this](QSystemTrayIcon::ActivationReason reason) {
					 if (reason == QSystemTrayIcon::Trigger ||
					     reason == QSystemTrayIcon::DoubleClick)
						 restoreWindow();
				 });
		tray_->show();
	}

	// Wire the MainWindow close/minimize policy to this controller. Both queries read the live toggle
	// (both modes) and return false when no tray icon exists, so the default behavior runs then.
	window_->setCloseToTrayHandler([this] { return shouldHideOnClose(); });
	window_->setMinimizeToTrayHandler([this] { return shouldHideOnMinimize(); });
}

bool TrayController::shouldHideOnClose() const
{
	// LIVE read (both modes): hide-to-tray on the window-X iff close-to-tray is on AND a tray icon
	// exists. No managed gate -- managed honors the same persisted toggle as standalone.
	return settings_.closeToTray() && tray_ != nullptr;
}

bool TrayController::shouldHideOnMinimize() const
{
	// LIVE read (both modes): hide-to-tray on minimize iff minimize-to-tray is on AND a tray icon exists.
	return settings_.minimizeToTray() && tray_ != nullptr;
}

void TrayController::restoreWindow()
{
	// The exact show/raise/activate a normal launch uses. showNormal() also clears the minimized
	// state so a minimize-to-tray hide followed by Show comes back un-minimized.
	window_->showNormal();
	window_->raise();
	window_->activateWindow();
}

void TrayController::hideToTray()
{
	window_->hide();
}

} // namespace moxrelay
