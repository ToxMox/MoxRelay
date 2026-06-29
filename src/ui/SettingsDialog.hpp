// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SettingsDialog -- the standalone settings UI (Tools > Settings...). A modal QDialog whose
// controls map 1:1 to the AppSettings key inventory: tray toggles, start-minimized, log level +
// write-to-file + log folder, plus read-only info (the live control URL + token-state). On OK it
// writes the changed keys via AppSettings; Cancel discards.
//
// FPS NOTE: the frame rate is PER-PROFILE and lives on the standalone toolbar FPS dropdown (it
// re-tiers the engine live and is saved with the profile). It is intentionally NOT a setting here --
// the global engine/fpsTier QSetting survives only as the silent no-profile boot fallback.
//
// This dialog is reachable only in standalone mode in practice (the managed/helper window is never
// shown). It never exposes the bind address (127.0.0.1 loopback is by design) or the token value.

#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

namespace moxrelay {

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	// `controlStatus` is the live status-bar control string (e.g. "control: ws://127.0.0.1:7341/
	// control" or a BIND FAILED message); shown read-only. Parented like addSourceDialog's QDialog.
	// `managed` (== the window's viewOnly_ state): in managed/helper mode the host owns the GPU via
	// --adapter and owns process lifecycle, so the Rendering GPU row + self-restart are suppressed.
	SettingsDialog(const QString &controlStatus, bool managed, QWidget *parent = nullptr);

private:
	void loadFromSettings();   // seed every control from AppSettings
	void saveToSettings();     // write every control back via AppSettings (called on accept)

	QCheckBox *minimizeToTray_ = nullptr;
	QCheckBox *closeToTray_ = nullptr;
	QCheckBox *startMinimized_ = nullptr;
	QComboBox *logLevel_ = nullptr;
	QCheckBox *logToFile_ = nullptr;
	QComboBox *adapter_ = nullptr; // Rendering GPU selector; null in managed mode (row not created)
	int loadedAdapterId_ = 0;      // adapter id loaded from settings (change-detection for restart)
	QSpinBox *maxSenders_ = nullptr; // Max Spout senders (both modes; 1..1024)
	int loadedMaxSenders_ = 64;      // value loaded from settings (change-detection for restart)
	QLabel *logDirLabel_ = nullptr;
	QString logDir_;
};

} // namespace moxrelay
