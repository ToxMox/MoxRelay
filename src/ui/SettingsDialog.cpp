// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SettingsDialog implementation. See the header for the contract. Mirrors addSourceDialog's
// construction pattern (a QFormLayout + a QDialogButtonBox(Ok|Cancel)); on accept it writes the
// changed keys via AppSettings.

#include "SettingsDialog.hpp"

#include "app/AppSettings.hpp"

#include "WheelGuard.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include <obs.h> // gs_enum_adapters + obs_enter_graphics/obs_leave_graphics (public libobs API)

namespace moxrelay {

namespace {
// Read-only control-URL info: pull the "ws://..." substring out of the status-bar string. Falls
// back to the raw string when no scheme is present (e.g. a BIND FAILED message).
QString controlUrlFrom(const QString &status)
{
	const int ws = status.indexOf(QStringLiteral("ws://"));
	if (ws >= 0)
		return status.mid(ws).trimmed();
	return status.trimmed();
}

// gs_enum_adapters callback: append each adapter to the combo as text=name, itemData=true id (the
// real adapter index, NOT the row). `param` is the QComboBox*. Returning true keeps enumerating.
bool collectAdapter(void *param, const char *name, uint32_t id)
{
	auto *combo = static_cast<QComboBox *>(param);
	combo->addItem(QString::fromUtf8(name), QVariant(static_cast<int>(id)));
	return true;
}
} // namespace

SettingsDialog::SettingsDialog(const QString &controlStatus, bool managed, QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle(QStringLiteral("Settings"));

	auto *outer = new QVBoxLayout(this);
	auto *form = new QFormLayout();
	outer->addLayout(form);

	// --- Tray ---
	minimizeToTray_ = new QCheckBox(QStringLiteral("Minimize to tray"), this);
	form->addRow(minimizeToTray_);
	closeToTray_ = new QCheckBox(QStringLiteral("Close to tray (X minimizes instead of quitting)"),
				     this);
	form->addRow(closeToTray_);
	startMinimized_ = new QCheckBox(QStringLiteral("Start minimized"), this);
	form->addRow(startMinimized_);

	// --- Logging ---
	logLevel_ = new QComboBox(this);
	logLevel_->addItems({QStringLiteral("error"), QStringLiteral("warning"), QStringLiteral("info"),
			     QStringLiteral("debug")});
	installWheelGuard(logLevel_); // an unfocused hover-wheel must not change the level
	form->addRow(QStringLiteral("Log level"), logLevel_);

	logToFile_ = new QCheckBox(QStringLiteral("Write logs to file"), this);
	form->addRow(logToFile_);

	// Read-only log folder path + an "Open log folder" button.
	logDir_ = AppSettings().logDir();
	auto *logDirRow = new QHBoxLayout();
	logDirLabel_ = new QLabel(logDir_, this);
	logDirLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *openLogButton = new QPushButton(QStringLiteral("Open log folder"), this);
	connect(openLogButton, &QPushButton::clicked, this,
		[this] { QDesktopServices::openUrl(QUrl::fromLocalFile(logDir_)); });
	logDirRow->addWidget(logDirLabel_, 1);
	logDirRow->addWidget(openLogButton);
	form->addRow(QStringLiteral("Log folder"), logDirRow);

	// --- Engine ---
	// (No FPS control here: the frame rate is PER-PROFILE, set on the toolbar FPS dropdown, which
	// re-tiers live and is saved with the profile. The global engine/fpsTier QSetting is now only the
	// silent no-profile boot fallback -- never user-facing.)
	//
	// Rendering GPU selector: STANDALONE ONLY. In managed/helper mode the host owns the GPU via
	// --adapter and owns process lifecycle, so the row (and its self-restart) are suppressed.
	if (!managed) {
		adapter_ = new QComboBox(this);
		// Enumerate adapters via libobs's public API (true adapter indices stored as itemData).
		// gs_enum_adapters must run inside a graphics context.
		obs_enter_graphics();
		gs_enum_adapters(&collectAdapter, adapter_);
		obs_leave_graphics();
		if (adapter_->count() == 0)
			adapter_->addItem(QStringLiteral("Default (primary GPU)"), QVariant(0));
		installWheelGuard(adapter_); // an unfocused hover-wheel must not change the GPU
		form->addRow(QStringLiteral("Rendering GPU"), adapter_);
	}

	// Max Spout senders: the process-wide cap on simultaneously published Spout senders, written to
	// the machine-global Spout registry once at engine start. Shown in BOTH standalone and managed
	// mode (a machine-wide user preference, not a per-host knob). Default 64; raise it to publish
	// many sources at once. A change needs a restart (the cap is fixed for the process lifetime).
	maxSenders_ = new QSpinBox(this);
	maxSenders_->setRange(1, 1024);
	installWheelGuard(maxSenders_); // an unfocused hover-wheel must not change the cap
	form->addRow(QStringLiteral("Max Spout senders"), maxSenders_);

	// --- Read-only control info (NEVER the bind address or the token value) ---
	const QString url = controlUrlFrom(controlStatus);
	auto *urlLabel = new QLabel(url.isEmpty() ? QStringLiteral("(not available)") : url, this);
	urlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(QStringLiteral("Control URL"), urlLabel);
	// Token-state: a per-launch CSPRNG token is generated by default; it is disabled only on RNG
	// failure (rare, logged). When a control URL is present, auth is enabled.
	const bool authEnabled = url.startsWith(QStringLiteral("ws://"));
	form->addRow(QStringLiteral("Auth"),
		     new QLabel(authEnabled ? QStringLiteral("enabled") : QStringLiteral("disabled"),
				this));

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);

	loadFromSettings();

	// Write on accept (after the base-class accept flips the result to Accepted).
	connect(this, &QDialog::accepted, this, &SettingsDialog::saveToSettings);
}

void SettingsDialog::loadFromSettings()
{
	AppSettings s;
	minimizeToTray_->setChecked(s.minimizeToTray());
	closeToTray_->setChecked(s.closeToTray());
	startMinimized_->setChecked(s.startMinimized());
	const int levelIndex = logLevel_->findText(s.logLevel());
	logLevel_->setCurrentIndex(levelIndex >= 0 ? levelIndex : logLevel_->findText(QStringLiteral("info")));
	logToFile_->setChecked(s.logToFile());

	// Rendering GPU (standalone only -- adapter_ is null in managed mode). Select the row whose
	// itemData (the true adapter id) matches the saved index; if none matches, fall back to row 0.
	if (adapter_) {
		loadedAdapterId_ = s.adapterIndex();
		const int row = adapter_->findData(QVariant(loadedAdapterId_));
		adapter_->setCurrentIndex(row >= 0 ? row : 0);
	}

	// Max Spout senders (always present, both modes).
	loadedMaxSenders_ = s.maxSenders();
	maxSenders_->setValue(loadedMaxSenders_);
}

void SettingsDialog::saveToSettings()
{
	AppSettings s;
	s.setMinimizeToTray(minimizeToTray_->isChecked());
	s.setCloseToTray(closeToTray_->isChecked());
	s.setStartMinimized(startMinimized_->isChecked());
	s.setLogLevel(logLevel_->currentText());
	s.setLogToFile(logToFile_->isChecked());

	// Restart-requiring engine settings. Both the rendering GPU and the Max Spout senders cap are
	// fixed for the process lifetime (the GPU device is created once at boot; the sender cap is a
	// once-per-process registry write at engine start), so a change can only take effect on a fresh
	// boot. Persist every changed value, then offer a SINGLE active restart if anything changed.
	bool restartNeeded = false;

	// Rendering GPU (standalone only -- adapter_ is null in managed mode).
	if (adapter_) {
		const int newId = adapter_->currentData().toInt();
		if (newId != loadedAdapterId_) {
			s.setAdapterIndex(newId);
			restartNeeded = true;
		}
	}

	// Max Spout senders (always present, both modes).
	if (maxSenders_) {
		const int newMax = maxSenders_->value();
		if (newMax != loadedMaxSenders_) {
			s.setMaxSenders(newMax);
			restartNeeded = true;
		}
	}

	if (restartNeeded) {
		const auto choice = QMessageBox::question(
			this, QStringLiteral("Restart required"),
			QStringLiteral("Changing the rendering GPU or the Max Spout senders cap takes effect "
				       "after a restart. Restart MoxRelay now?"),
			QMessageBox::Yes | QMessageBox::No);
		if (choice == QMessageBox::Yes) {
			// Relaunch WITHOUT injecting any settings flag: the saved settings must govern so a
			// future dialog change is honored (injecting a flag would pin this choice).
			QProcess::startDetached(QCoreApplication::applicationFilePath(),
						QCoreApplication::arguments().mid(1));
			QCoreApplication::quit();
		}
	}
}

} // namespace moxrelay
