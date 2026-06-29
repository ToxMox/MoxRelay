// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// MainWindow implementation. The Sources dock is a real model (selection drives
// the Properties dock), Add Source / remove ride the CreateSource/RemoveSource dispatch seams,
// the Properties dock hosts the media transport strip + the descriptor-driven PropertyPanel,
// and control events (sourceAdded/sourceRemoved/propertyChanged/mediaChanged) keep everything
// coherent with WS clients -- both directions share the verb layer's single state path.

#include "MainWindow.hpp"

#include "AudioMixerStrip.hpp"
#include "FilterInspector.hpp"
#include "MoxDisplayWidget.hpp"
#include "PropertyPanel.hpp"
#include "SettingsDialog.hpp"
#include "WheelGuard.hpp"

#include "app/HelperConfig.hpp" // kMoxRelayVersion (About box; read at build time, not hardcoded)
#include "app/ProfileStore.hpp" // item 05: standalone profiles store (import/export + name validation)
#include "spout/SpoutSenderEngine.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace moxrelay {

namespace {

using nlohmann::json;

constexpr int kSourceIdRole = Qt::UserRole;
constexpr int kSourceTypeRole = Qt::UserRole + 1;
constexpr int kSourceFormatRole = Qt::UserRole + 2;

// Live-scrub seek cadence: at most one SeekMedia per window while the handle moves, with a
// trailing flush so the last position lands. 100 ms (10 Hz) keeps demuxer seek churn bounded;
// every seek's timestamp jump is fade-wrapped by the audio engine.
constexpr int kSeekLiveMs = 100;

QString formatMs(int64_t ms)
{
	const int64_t totalSeconds = ms / 1000;
	return QStringLiteral("%1:%2").arg(totalSeconds / 60).arg(totalSeconds % 60, 2, 10,
								  QLatin1Char('0'));
}

// The toolbar device selector re-enumerates endpoints every time its popup opens (devices come
// and go); a plain showPopup override needs no meta-object machinery.
class DeviceComboBox : public QComboBox {
public:
	using QComboBox::QComboBox;
	std::function<void()> beforePopup;
	void showPopup() override
	{
		if (beforePopup)
			beforePopup();
		QComboBox::showPopup();
	}
};

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
	setWindowTitle(QStringLiteral("MoxRelay"));
	resize(1280, 720);
	setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks);

	// CENTRAL: the live preview. Kept as the central widget so it never regresses.
	preview_ = new MoxDisplayWidget(this);
	setCentralWidget(preview_);

	buildMenuBar();
	buildToolBar();
	buildDocks();

	statusLabel_ = new QLabel(this);
	statusBar()->addPermanentWidget(statusLabel_);

	// Resolution/fps come from libobs canvas state; refresh once now and then poll lightly
	// (cheap, UI-thread obs_get_video_info read -- never the render thread).
	refreshStatus();
	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, &MainWindow::refreshStatus);
	timer->start(1000);

	// The transport strip's position/duration poll while a media source is selected.
	mediaTimer_ = new QTimer(this);
	connect(mediaTimer_, &QTimer::timeout, this, &MainWindow::refreshMediaStatus);
	mediaTimer_->setInterval(500);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	// Item 04: close-to-tray. The handler reads the LIVE toggle (both modes; false when unset or when
	// no tray icon exists). When it fires we ignore the close and hide to the tray instead of
	// quitting, so the X-click parks the window in the tray rather than ending the process. The
	// managing application's Shutdown verb calls QCoreApplication::quit() directly (bypassing
	// closeEvent), so this branch can never swallow the managed stop path regardless of the toggle.
	if (closeToTrayHandler_ && closeToTrayHandler_()) {
		event->ignore();
		hide();
		return;
	}
	QMainWindow::closeEvent(event);
	// A tray-hidden window's close does not emit lastWindowClosed (Qt counts only visible windows),
	// and with quitOnLastWindowClosed disabled an accepted close would otherwise leave the event loop
	// running -- so quit explicitly on any accepted close to run the clean-shutdown sequence.
	if (event->isAccepted())
		QCoreApplication::quit();
}

void MainWindow::changeEvent(QEvent *event)
{
	// Item 04: minimize-to-tray (both modes). When the user minimizes and the handler opts in,
	// hide to the tray instead of leaving a taskbar-minimized window. Deferred (queued) so the hide
	// does not run inside the window-state-change dispatch. CLEAR the minimized flag before hiding so
	// the window leaves the taskbar in a NORMAL state -- a bare hide() on a still-minimized window
	// leaves the minimized state latched, so the tray-restore (showNormal) is the only clean way back.
	if (event->type() == QEvent::WindowStateChange && isMinimized() && minimizeToTrayHandler_ &&
	    minimizeToTrayHandler_()) {
		QTimer::singleShot(0, this, [this] {
			setWindowState(windowState() & ~Qt::WindowMinimized);
			hide();
		});
	}
	QMainWindow::changeEvent(event);
}

void MainWindow::setCloseToTrayHandler(std::function<bool()> handler)
{
	closeToTrayHandler_ = std::move(handler);
}

void MainWindow::setMinimizeToTrayHandler(std::function<bool()> handler)
{
	minimizeToTrayHandler_ = std::move(handler);
}

void MainWindow::buildMenuBar()
{
	// QMainWindow::menuBar() auto-creates the bar. Item 04: File>Quit, Tools>Settings..., Help>About.
	QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
	// Quit ENDS the process directly (QCoreApplication::quit), bypassing close() so it can never be
	// swallowed by standalone close-to-tray. Routing it through close() would let the close-to-tray
	// toggle convert this explicit Quit into a hide -- leaving the user with no way out of the tray.
	// The window-X still rides closeEvent (hide-to-tray when enabled); only the explicit Quit forces
	// termination. quit() unwinds app.exec() so the normal engine + libobs teardown still runs.
	QObject::connect(fileMenu->addAction(QStringLiteral("Quit")), &QAction::triggered, this,
			 [] { QCoreApplication::quit(); });

	QMenu *toolsMenu = menuBar()->addMenu(QStringLiteral("Tools"));
	QObject::connect(toolsMenu->addAction(QStringLiteral("Settings...")), &QAction::triggered, this,
			 &MainWindow::openSettingsDialog);

	QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("Help"));
	QObject::connect(helpMenu->addAction(QStringLiteral("About MoxRelay")), &QAction::triggered,
			 this, &MainWindow::showAboutBox);
}

void MainWindow::openSettingsDialog()
{
	// Mirror addSourceDialog: a modal QDialog parented to this. The control URL + token-state are
	// read-only info; pass what we know (the status bar already carries the live control URL).
	// viewOnly_ == managed mode: it suppresses the Rendering GPU row + self-restart in the dialog.
	SettingsDialog dialog(controlStatus_, viewOnly_, this);
	dialog.exec(); // SettingsDialog saves changed keys on accept; Cancel discards.
}

void MainWindow::showAboutBox()
{
	// Version is READ at build time from kMoxRelayVersion (never hardcoded). The disclaimer +
	// license text are the README wording verbatim. "OBS" appears ONLY as the required libobs
	// attribution; there is ZERO host-application branding here.
	const QString text =
		QStringLiteral("<b>MoxRelay</b><br>Version %1<br><br>"
			       "Built with libobs. Not affiliated with or endorsed by the OBS Project."
			       "<br><br>"
			       "GPLv3. MoxRelay links libobs (GPLv2-or-later), so the application is "
			       "distributed under the GNU General Public License, version 3. See "
			       "<code>LICENSE</code>. Full corresponding source is published in this "
			       "repository.")
			.arg(QString::fromUtf8(moxrelay::kMoxRelayVersion));
	QMessageBox::about(this, QStringLiteral("About MoxRelay"), text);
}

void MainWindow::buildToolBar()
{
	auto *toolbar = addToolBar(QStringLiteral("Main"));
	toolbar->setObjectName(QStringLiteral("MainToolBar"));
	toolbar->setMovable(false);
	toolbar->setFloatable(false);

	// Enabled once the control seams are bound (everything dispatches through the verb layer).
	addSourceAction_ = toolbar->addAction(QStringLiteral("Add Source"));
	addSourceAction_->setEnabled(false);
	addSourceAction_->setToolTip(QStringLiteral("Add a capture source"));
	connect(addSourceAction_, &QAction::triggered, this, &MainWindow::addSourceDialog);

	addFilterAction_ = toolbar->addAction(QStringLiteral("Add Filter"));
	addFilterAction_->setEnabled(false);
	addFilterAction_->setToolTip(QStringLiteral("Add a filter to the selected source"));
	connect(addFilterAction_, &QAction::triggered, this, [this] {
		if (filterInspector_)
			filterInspector_->addFilterDialog();
	});

	toolbar->addSeparator();

	// M2.1: a real action, enabled once setBroadcast wires the engine + source. Ctrl+B so the
	// broadcast can be driven from the keyboard (and by scripted verification).
	broadcastAction_ = toolbar->addAction(QStringLiteral("Start Broadcast"));
	broadcastAction_->setEnabled(false);
	broadcastAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
	broadcastAction_->setToolTip(QStringLiteral("Begin emitting this source as a Spout sender (Ctrl+B)"));
	connect(broadcastAction_, &QAction::triggered, this, &MainWindow::toggleBroadcast);

	toolbar->addSeparator();

	// Instance-level audio output: the device selector (dispatches SetAudioOutputDevice -- the
	// same verb a wire client uses) + the master mix meter. Disabled until the seams are bound.
	toolbar->addWidget(new QLabel(QStringLiteral("Audio"), toolbar));
	auto *deviceCombo = new DeviceComboBox(toolbar);
	deviceCombo->setObjectName(QStringLiteral("AudioDeviceCombo"));
	deviceCombo->setMinimumWidth(180);
	deviceCombo->setToolTip(QStringLiteral("Output device for this instance's mixed audio"));
	// Click-to-focus only: as the window's first focusable widget this combo otherwise takes
	// initial keyboard focus, where a stray arrow key dispatches a device swap.
	deviceCombo->setFocusPolicy(Qt::ClickFocus);
	// Same hazard with the wheel: hovering the toolbar must never swap output devices.
	installWheelGuard(deviceCombo);
	deviceCombo->setEnabled(false);
	deviceCombo->beforePopup = [this] { refreshAudioDevices(); };
	connect(deviceCombo, &QComboBox::activated, this, &MainWindow::onAudioDeviceSelected);
	audioDeviceCombo_ = deviceCombo;
	toolbar->addWidget(audioDeviceCombo_);

	masterMeter_ = new LevelMeter(toolbar);
	masterMeter_->setFixedSize(120, 12);
	masterMeter_->setToolTip(QStringLiteral("Master mix level (red edge = clipped)"));
	toolbar->addWidget(masterMeter_);

	// Item 05: the standalone Profiles control (built hidden; shown only when
	// setStandaloneProfilesEnabled(true) runs in standalone mode).
	buildProfilesControl(toolbar);
}

// Item 05: the standalone toolbar group -- the per-profile FPS dropdown + a profile combo + a menu of
// profile actions (Save / Save As / Rename / Duplicate / Delete / Import / Export), mirroring the audio
// label+combo block above (ClickFocus + a wheel guard so a stray arrow/wheel can never swap the active
// profile or re-tier the engine). The whole group is wrapped in one container, added as ONE toolbar
// widget; the QAction addWidget() returns (and the preceding separator's action) are captured so the
// standalone gate can hide the group as a unit. A QToolBar lays an added widget out via its QAction, so
// toggling the widget's own visibility is unreliable -- the action is the canonical control. The group
// stays hidden until setStandaloneProfilesEnabled(true) (managed mode never calls it).
void MainWindow::buildProfilesControl(QWidget *toolbar)
{
	auto *bar = qobject_cast<QToolBar *>(toolbar);
	if (!bar)
		return;

	standaloneSeparatorAction_ = bar->addSeparator();

	profilesContainer_ = new QWidget(bar);
	auto *row = new QHBoxLayout(profilesContainer_);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(4);

	// Per-profile FPS tier dropdown. Changing it LIVE-re-tiers the engine in place and marks the active
	// profile dirty (a Save persists it -- the same path source/audio/filter edits use). ClickFocus + a
	// wheel guard so a stray arrow/wheel can never re-tier (a re-tier stops the engine + resets video).
	row->addWidget(new QLabel(QStringLiteral("FPS"), profilesContainer_));
	fpsTierCombo_ = new QComboBox(profilesContainer_);
	fpsTierCombo_->setToolTip(QStringLiteral("Per-profile frame rate (re-tiers the engine live)"));
	for (int tier : {24, 30, 48, 60, 120, 144, 240})
		fpsTierCombo_->addItem(QString::number(tier), tier);
	fpsTierCombo_->setFocusPolicy(Qt::ClickFocus);
	installWheelGuard(fpsTierCombo_);
	connect(fpsTierCombo_, &QComboBox::activated, this, &MainWindow::onFpsTierSelected);
	row->addWidget(fpsTierCombo_);

	row->addWidget(new QLabel(QStringLiteral("Profile"), profilesContainer_));

	profilesCombo_ = new QComboBox(profilesContainer_);
	profilesCombo_->setMinimumWidth(160);
	profilesCombo_->setToolTip(QStringLiteral("Saved scene profiles (sources, filters, audio, fps tier)"));
	// Click-to-focus + wheel guard: hovering or tabbing the toolbar must never load a different
	// profile (a profile load tears down + rebuilds the live source set).
	profilesCombo_->setFocusPolicy(Qt::ClickFocus);
	installWheelGuard(profilesCombo_);
	connect(profilesCombo_, &QComboBox::activated, this, &MainWindow::onProfileSelected);
	row->addWidget(profilesCombo_);

	auto *menuButton = new QToolButton(profilesContainer_);
	menuButton->setText(QStringLiteral("Profile Actions"));
	menuButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
	menuButton->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(menuButton);
	connect(menu->addAction(QStringLiteral("Save")), &QAction::triggered, this, &MainWindow::doProfileSave);
	connect(menu->addAction(QStringLiteral("Save As...")), &QAction::triggered, this,
		&MainWindow::doProfileSaveAs);
	connect(menu->addAction(QStringLiteral("Rename...")), &QAction::triggered, this,
		&MainWindow::doProfileRename);
	connect(menu->addAction(QStringLiteral("Duplicate...")), &QAction::triggered, this,
		&MainWindow::doProfileDuplicate);
	connect(menu->addAction(QStringLiteral("Delete...")), &QAction::triggered, this,
		&MainWindow::doProfileDelete);
	menu->addSeparator();
	connect(menu->addAction(QStringLiteral("Import...")), &QAction::triggered, this,
		&MainWindow::doProfileImport);
	connect(menu->addAction(QStringLiteral("Export...")), &QAction::triggered, this,
		&MainWindow::doProfileExport);
	menuButton->setMenu(menu);
	row->addWidget(menuButton);

	standaloneGroupAction_ = bar->addWidget(profilesContainer_);
	// Hide the group via the QActions (canonical for QToolBar slots), not the widget -- shown only when
	// setStandaloneProfilesEnabled(true) runs in standalone mode.
	standaloneGroupAction_->setVisible(false);
	if (standaloneSeparatorAction_)
		standaloneSeparatorAction_->setVisible(false);
}

void MainWindow::setStandaloneProfilesEnabled(bool enabled)
{
	profilesEnabled_ = enabled;
	// Toggle the toolbar-slot QActions (a QToolBar lays an added widget out via its QAction; hiding the
	// widget alone leaves the slot visible in managed mode). The separator rides the same gate so no
	// dangling divider is left behind.
	if (standaloneGroupAction_)
		standaloneGroupAction_->setVisible(enabled);
	if (standaloneSeparatorAction_)
		standaloneSeparatorAction_->setVisible(enabled);
	if (!enabled)
		return;
	refreshFpsTierCombo();
	refreshProfilesList();
	// Auto-load-last (standalone): drive the NORMAL load path (the verb/GUI seam) post-construction so
	// it is toolbar-consistent (single load path). Only when the seam is bound and a last profile is
	// recorded + still present.
	if (seamsSet_ && seams_.loadProfile && seams_.listProfiles) {
		const json reply = seams_.listProfiles ? seams_.listProfiles() : json::object();
		std::string last;
		if (reply.contains("result") && reply["result"].contains("lastProfile") &&
		    reply["result"]["lastProfile"].is_string())
			last = reply["result"]["lastProfile"].get<std::string>();
		if (!last.empty() && profilesCombo_ &&
		    profilesCombo_->findText(QString::fromStdString(last)) >= 0) {
			const json loadReply = seams_.loadProfile(last);
			if (loadReply.contains("result")) {
				activeProfile_ = QString::fromStdString(last);
				refreshSources();
				refreshFpsTierCombo(); // reflect the loaded profile's (possibly re-tiered) frame rate
			}
		}
	}
	refreshProfilesList();
	updateProfilesDirty();
}

// Source view-only lock (managed/helper mode). Mirrors the setStandaloneProfilesEnabled gate:
// main.cpp calls this with !managed at the run_gui seam. When view-only, every source-EDITING
// affordance is suppressed while the Sources list (selection), the central preview, and the media
// transport strip stay live -- a user can still pick a source and drive its playback, but cannot
// add/remove/edit a source that would fight the owning integrator's reconciler.
//
// DURABILITY: the lock cannot be undone by a later seam bind or selection change because the two
// state dimensions it uses are orthogonal to the ones the seam layer touches. The toolbar actions
// are HIDDEN (setControlSeams/onSelectionChanged only ever toggle their setEnabled flag, never
// visibility, and neither action carries a keyboard shortcut, so a hidden action has no trigger
// path), and the PropertyPanel + FilterInspector are disabled at the PARENT widget (their own
// methods only ever toggle child rows/buttons, and Qt refuses to re-enable a child while its
// parent is disabled, so a rebuilt property form / refreshed filter list stays locked). The two
// setEnabled lines that re-derive the action state are ALSO gated on !viewOnly_, so a re-entrant
// bind keeps them greyed even if they were ever shown again.
void MainWindow::setSourceEditingEnabled(bool enabled)
{
	viewOnly_ = !enabled;

	// Add Source / Add Filter: hide outright. Clearing the enabled flag too keeps the action's
	// state consistent with the !viewOnly_ gate the seam layer now applies.
	if (addSourceAction_) {
		addSourceAction_->setVisible(enabled);
		if (viewOnly_)
			addSourceAction_->setEnabled(false);
	}
	if (addFilterAction_) {
		addFilterAction_->setVisible(enabled);
		if (viewOnly_)
			addFilterAction_->setEnabled(false);
	}
	if (broadcastAction_) {
		broadcastAction_->setVisible(enabled);
		if (viewOnly_)
			broadcastAction_->setEnabled(false);
	}

	// Per-source sender format: disable the combo (its row stays visible so the current format is
	// still readable). onFormatSelected is the only dispatch path and it is now unreachable.
	if (formatCombo_)
		formatCombo_->setEnabled(enabled);

	// Property panel: stays VISIBLE so descriptor values read out, but disabled so no edit can
	// dispatch SetSourceProperties. Disabling the parent locks every (re)built editor row.
	if (propertyPanel_)
		propertyPanel_->setEnabled(enabled);

	// Filter inspector: disabled as a unit (add/remove/reorder/enable-toggle and the double-click
	// rename all live inside it). Same parent-disable durability as the property panel.
	if (filterInspector_)
		filterInspector_->setEnabled(enabled);

	// The Remove Source context action is gated inline in the customContextMenuRequested handler
	// (a viewOnly_ short-circuit), so nothing further is needed for it here.
}

void MainWindow::setManagedIdentity(const QString &displayName)
{
	setWindowTitle(displayName.isEmpty() ? QStringLiteral("MoxRelay")
					     : QStringLiteral("MoxRelay - %1").arg(displayName));
}

// FPS combo -> live re-tier through the verb layer, then mark the active profile dirty. The re-tier
// updates the running tier (identity_.fpsTier), which the per-profile snapshot captures -- so the
// active profile reads dirty and an explicit Save persists the new tier (the same dirty/Save path the
// source/audio/filter edits use; no separate persistence is invented here). With no active profile the
// re-tier still applies live (the global engine/fpsTier QSetting stays the silent no-profile boot
// fallback only -- it is never written from the GUI).
void MainWindow::onFpsTierSelected(int index)
{
	if (fpsRefreshing_ || !profilesEnabled_ || !fpsTierCombo_ || !seams_.setFpsTier)
		return;
	if (index < 0)
		return;
	bool ok = false;
	const int tier = fpsTierCombo_->itemData(index).toInt(&ok);
	if (!ok || tier <= 0)
		return;
	const json reply = seams_.setFpsTier(tier);
	if (reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("FPS change failed"),
				     QString::fromStdString(reply["error"].value(
					     "message", std::string("Could not change the frame rate"))));
		refreshFpsTierCombo(); // revert the combo to the actual live tier
		return;
	}
	updateProfilesDirty(); // the tier is per-profile chrome -> the active profile is now dirty
}

// Set the FPS combo to the live engine tier WITHOUT dispatching a re-tier (re-entrancy guarded). Used
// after a profile load (the LoadProfile verb already re-tiered) and on standalone enable. An exact
// match selects it; an off-list live tier (e.g. a profile saved at a custom rate) is added so the
// combo always shows the truth.
void MainWindow::refreshFpsTierCombo()
{
	if (!fpsTierCombo_ || !seams_.currentFpsTier)
		return;
	const int tier = seams_.currentFpsTier();
	if (tier <= 0)
		return;
	fpsRefreshing_ = true;
	QSignalBlocker blocker(fpsTierCombo_);
	int idx = fpsTierCombo_->findData(tier);
	if (idx < 0) {
		fpsTierCombo_->addItem(QString::number(tier), tier);
		idx = fpsTierCombo_->findData(tier);
	}
	if (idx >= 0)
		fpsTierCombo_->setCurrentIndex(idx);
	fpsRefreshing_ = false;
}

std::string MainWindow::activeProfileName() const
{
	return activeProfile_.toStdString();
}

void MainWindow::refreshProfilesList()
{
	if (!profilesEnabled_ || !profilesCombo_ || !seamsSet_ || !seams_.listProfiles)
		return;
	const json reply = seams_.listProfiles();
	if (!reply.contains("result"))
		return;
	const json &result = reply["result"];

	profilesRefreshing_ = true;
	QSignalBlocker blocker(profilesCombo_);
	profilesCombo_->clear();
	if (result.contains("profiles") && result["profiles"].is_array()) {
		for (const json &p : result["profiles"]) {
			if (p.is_string())
				profilesCombo_->addItem(QString::fromStdString(p.get<std::string>()));
		}
	}
	// Re-select the active profile if it still exists.
	if (!activeProfile_.isEmpty()) {
		const int idx = profilesCombo_->findText(activeProfile_);
		if (idx >= 0)
			profilesCombo_->setCurrentIndex(idx);
	}
	profilesRefreshing_ = false;
	updateProfilesDirty();
}

void MainWindow::updateProfilesDirty()
{
	if (!profilesEnabled_ || !profilesCombo_)
		return;
	if (activeProfile_.isEmpty() || !seams_.profileIsDirty)
		return;
	const int idx = profilesCombo_->findText(activeProfile_);
	if (idx < 0)
		return;
	const bool dirty = seams_.profileIsDirty(activeProfile_.toStdString());
	// The asterisk decorates the displayed label only; the underlying item text stays the bare name
	// (findText / activeProfile_ comparisons rely on the bare name).
	const QString label = dirty ? activeProfile_ + QStringLiteral(" *") : activeProfile_;
	if (profilesCombo_->itemText(idx) != label) {
		QSignalBlocker blocker(profilesCombo_);
		profilesCombo_->setItemText(idx, label);
	}
}

void MainWindow::onProfileSelected(int index)
{
	if (profilesRefreshing_ || !profilesEnabled_ || !profilesCombo_ || !seams_.loadProfile)
		return;
	if (index < 0)
		return;
	// The displayed item may carry the dirty asterisk; strip it to the bare name.
	QString name = profilesCombo_->itemText(index);
	if (name.endsWith(QStringLiteral(" *")))
		name.chop(2);
	if (name == activeProfile_)
		return; // re-selecting the active profile is a no-op (no destructive reload)

	// Confirm before swapping ONLY if the active profile has unsaved changes (dirty). The verb itself
	// allows the swap unconditionally (standalone only); the prompt is the GUI's data-loss guard.
	if (!activeProfile_.isEmpty() && seams_.profileIsDirty &&
	    seams_.profileIsDirty(activeProfile_.toStdString())) {
		const auto choice = QMessageBox::question(
			this, QStringLiteral("Switch profile?"),
			QStringLiteral("'%1' has unsaved changes. Load '%2' and discard them?")
				.arg(activeProfile_, name),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (choice != QMessageBox::Yes) {
			refreshProfilesList(); // revert the combo to the active profile
			return;
		}
	}

	const json reply = seams_.loadProfile(name.toStdString());
	if (reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Load failed"),
				     QString::fromStdString(reply["error"].value("message",
									       std::string("Profile load failed"))));
		refreshProfilesList();
		return;
	}
	activeProfile_ = name;
	refreshSources();
	refreshFpsTierCombo(); // the LoadProfile verb already re-tiered; reflect the loaded tier in the combo
	refreshProfilesList();
}

void MainWindow::doProfileSave()
{
	if (!profilesEnabled_ || !seams_.saveProfile)
		return;
	if (activeProfile_.isEmpty()) {
		doProfileSaveAs();
		return;
	}
	const json reply = seams_.saveProfile(activeProfile_.toStdString());
	if (reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Save failed"),
				     QString::fromStdString(reply["error"].value("message",
									       std::string("Profile save failed"))));
		return;
	}
	refreshProfilesList();
}

void MainWindow::doProfileSaveAs()
{
	if (!profilesEnabled_ || !seams_.saveProfile)
		return;
	bool ok = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("Save Profile As"),
						   QStringLiteral("Profile name:"), QLineEdit::Normal,
						   QString(), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;
	const QString trimmed = name.trimmed();
	if (!ProfileStore::isValidName(trimmed.toStdString())) {
		QMessageBox::warning(this, QStringLiteral("Invalid name"),
				     QStringLiteral("A profile name cannot contain path separators or the "
						    "characters / \\ : * ? \" < > |."));
		return;
	}
	const json reply = seams_.saveProfile(trimmed.toStdString());
	if (reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Save failed"),
				     QString::fromStdString(reply["error"].value("message",
									       std::string("Profile save failed"))));
		return;
	}
	activeProfile_ = trimmed;
	refreshProfilesList();
}

void MainWindow::doProfileRename()
{
	if (!profilesEnabled_ || activeProfile_.isEmpty() || !seams_.saveProfile || !seams_.deleteProfile)
		return;
	bool ok = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("Rename Profile"),
						   QStringLiteral("New name:"), QLineEdit::Normal,
						   activeProfile_, &ok);
	if (!ok || name.trimmed().isEmpty() || name.trimmed() == activeProfile_)
		return;
	const QString trimmed = name.trimmed();
	if (!ProfileStore::isValidName(trimmed.toStdString())) {
		QMessageBox::warning(this, QStringLiteral("Invalid name"),
				     QStringLiteral("A profile name cannot contain path separators or the "
						    "characters / \\ : * ? \" < > |."));
		return;
	}
	// Rename = save the live state under the new name, then delete the old. (The live state IS the
	// active profile's state unless it is dirty -- a dirty rename carries the unsaved edits forward,
	// which matches the user's intent of "this scene, under a new name".)
	const QString old = activeProfile_;
	const json saveReply = seams_.saveProfile(trimmed.toStdString());
	if (saveReply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Rename failed"),
				     QString::fromStdString(saveReply["error"].value(
					     "message", std::string("Profile save failed"))));
		return;
	}
	seams_.deleteProfile(old.toStdString());
	activeProfile_ = trimmed;
	refreshProfilesList();
}

void MainWindow::doProfileDuplicate()
{
	if (!profilesEnabled_ || activeProfile_.isEmpty() || !seams_.saveProfile)
		return;
	bool ok = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("Duplicate Profile"),
						   QStringLiteral("New profile name:"), QLineEdit::Normal,
						   activeProfile_ + QStringLiteral(" copy"), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;
	const QString trimmed = name.trimmed();
	if (!ProfileStore::isValidName(trimmed.toStdString())) {
		QMessageBox::warning(this, QStringLiteral("Invalid name"),
				     QStringLiteral("A profile name cannot contain path separators or the "
						    "characters / \\ : * ? \" < > |."));
		return;
	}
	// Duplicate = save the live state under the new name (the active profile remains as it was on
	// disk); the duplicate becomes the active profile.
	const json reply = seams_.saveProfile(trimmed.toStdString());
	if (reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Duplicate failed"),
				     QString::fromStdString(reply["error"].value(
					     "message", std::string("Profile save failed"))));
		return;
	}
	activeProfile_ = trimmed;
	refreshProfilesList();
}

void MainWindow::doProfileDelete()
{
	if (!profilesEnabled_ || activeProfile_.isEmpty() || !seams_.deleteProfile)
		return;
	const auto choice = QMessageBox::question(
		this, QStringLiteral("Delete profile?"),
		QStringLiteral("Delete the profile '%1'? This cannot be undone.").arg(activeProfile_),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (choice != QMessageBox::Yes)
		return;
	const json reply = seams_.deleteProfile(activeProfile_.toStdString());
	if (reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Delete failed"),
				     QString::fromStdString(reply["error"].value(
					     "message", std::string("Profile delete failed"))));
		return;
	}
	activeProfile_.clear();
	refreshProfilesList();
}

void MainWindow::doProfileImport()
{
	if (!profilesEnabled_)
		return;
	const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Profile"),
							  QString(), QStringLiteral("Profiles (*.json)"));
	if (path.isEmpty())
		return;
	QFile in(path);
	if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QMessageBox::warning(this, QStringLiteral("Import failed"),
				     QStringLiteral("Could not read the selected file."));
		return;
	}
	const QString text = QString::fromUtf8(in.readAll());
	in.close();

	// Default the imported profile name to the file's base name; let the user confirm/override.
	bool ok = false;
	const QString suggested = QFileInfo(path).completeBaseName();
	const QString name = QInputDialog::getText(this, QStringLiteral("Import Profile"),
						   QStringLiteral("Save imported profile as:"),
						   QLineEdit::Normal, suggested, &ok);
	if (!ok || name.trimmed().isEmpty())
		return;
	const QString trimmed = name.trimmed();
	if (!ProfileStore::isValidName(trimmed.toStdString())) {
		QMessageBox::warning(this, QStringLiteral("Invalid name"),
				     QStringLiteral("A profile name cannot contain path separators or the "
						    "characters / \\ : * ? \" < > |."));
		return;
	}
	// Store the raw JSON verbatim under the new name; the user can then load it from the combo.
	if (!ProfileStore::write(trimmed.toStdString(), text.toStdString())) {
		QMessageBox::warning(this, QStringLiteral("Import failed"),
				     QStringLiteral("The profile could not be written."));
		return;
	}
	refreshProfilesList();
}

void MainWindow::doProfileExport()
{
	if (!profilesEnabled_ || activeProfile_.isEmpty())
		return;
	std::string text;
	if (!ProfileStore::read(activeProfile_.toStdString(), text)) {
		QMessageBox::warning(this, QStringLiteral("Export failed"),
				     QStringLiteral("The active profile could not be read."));
		return;
	}
	const QString path = QFileDialog::getSaveFileName(
		this, QStringLiteral("Export Profile"), activeProfile_ + QStringLiteral(".json"),
		QStringLiteral("Profiles (*.json)"));
	if (path.isEmpty())
		return;
	QFile out(path);
	if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		QMessageBox::warning(this, QStringLiteral("Export failed"),
				     QStringLiteral("Could not write the selected file."));
		return;
	}
	out.write(QByteArray::fromStdString(text));
	out.close();
}

nlohmann::json MainWindow::windowLayoutJson() const
{
	const QRect g = geometry();
	return json{{"x", g.x()},
		    {"y", g.y()},
		    {"width", g.width()},
		    {"height", g.height()},
		    {"maximized", isMaximized()}};
}

void MainWindow::applyWindowLayout(const nlohmann::json &layout)
{
	if (!layout.is_object())
		return;
	if (layout.value("maximized", false)) {
		showMaximized();
		return;
	}
	const int x = layout.value("x", geometry().x());
	const int y = layout.value("y", geometry().y());
	const int w = layout.value("width", geometry().width());
	const int h = layout.value("height", geometry().height());
	if (w > 0 && h > 0) {
		if (isMaximized())
			showNormal();
		setGeometry(x, y, w, h);
	}
}

void MainWindow::setEngine(SpoutSenderEngine *engine)
{
	engine_ = engine;
}

void MainWindow::setBroadcastHandler(std::function<bool(bool)> handler)
{
	broadcastHandler_ = std::move(handler);
	if (broadcastAction_)
		broadcastAction_->setEnabled(static_cast<bool>(broadcastHandler_) && !viewOnly_);
}

void MainWindow::setBroadcastQueryHandler(std::function<bool()> handler)
{
	broadcastQueryHandler_ = std::move(handler);
	// Seed the button now: a broadcast already in progress before this seam was bound must be
	// reflected immediately. Deferred (same pattern as onControlEvent) to stay off any
	// mid-construction/mid-dispatch callstack.
	if (broadcastQueryHandler_) {
		QTimer::singleShot(0, this, [this] {
			if (broadcastQueryHandler_)
				applyBroadcastActive(broadcastQueryHandler_());
		});
	}
}

void MainWindow::applyBroadcastActive(bool active)
{
	broadcastActive_ = active;
	broadcastAction_->setText(broadcastActive_ ? QStringLiteral("Stop Broadcast")
						   : QStringLiteral("Start Broadcast"));
	broadcastAction_->setToolTip(broadcastActive_
						 ? QStringLiteral("Stop emitting this source as a Spout sender (Ctrl+B)")
						 : QStringLiteral("Begin emitting this source as a Spout sender (Ctrl+B)"));
}

void MainWindow::toggleBroadcast()
{
	if (!broadcastHandler_)
		return;
	// One state path: the handler runs the SAME verb layer the control API uses, and reports
	// back the resulting instance-wide state (events fire there, not here).
	applyBroadcastActive(broadcastHandler_(!broadcastActive_));
	refreshStatus();
}

QWidget *MainWindow::buildTransportStrip(QWidget *parent)
{
	auto *strip = new QWidget(parent);
	strip->setObjectName(QStringLiteral("TransportStrip"));
	auto *layout = new QVBoxLayout(strip);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(8);
	playPauseButton_ = new QPushButton(QStringLiteral("Play"), strip);
	restartButton_ = new QPushButton(QStringLiteral("Restart"), strip);
	stopButton_ = new QPushButton(QStringLiteral("Stop"), strip);
	loopButton_ = new QPushButton(QStringLiteral("Loop"), strip);
	loopButton_->setCheckable(true);
	loopButton_->setToolTip(QStringLiteral("Loop playback (writes the 'looping' setting)"));
	buttons->addWidget(playPauseButton_);
	buttons->addWidget(restartButton_);
	buttons->addWidget(stopButton_);
	buttons->addWidget(loopButton_);
	buttons->addStretch(1);
	layout->addLayout(buttons);

	auto *seekRow = new QHBoxLayout();
	seekRow->setSpacing(8);
	seekSlider_ = new QSlider(Qt::Horizontal, strip);
	seekSlider_->setRange(0, 0);
	installWheelGuard(seekSlider_); // an accidental wheel must not seek playback
	timeLabel_ = new QLabel(QStringLiteral("-:-- / -:--"), strip);
	seekRow->addWidget(seekSlider_, /*stretch=*/1);
	seekRow->addWidget(timeLabel_);
	layout->addLayout(seekRow);

	connect(playPauseButton_, &QPushButton::clicked, this, [this] {
		if (mediaSourceId_.empty() || !seams_.controlMedia)
			return;
		const std::string action = lastMediaState_ == "playing" ? "pause" : "play";
		seams_.controlMedia(mediaSourceId_, action);
		refreshMediaStatus();
	});
	connect(restartButton_, &QPushButton::clicked, this, [this] {
		if (mediaSourceId_.empty() || !seams_.controlMedia)
			return;
		seams_.controlMedia(mediaSourceId_, "restart");
		refreshMediaStatus();
	});
	connect(stopButton_, &QPushButton::clicked, this, [this] {
		if (mediaSourceId_.empty() || !seams_.controlMedia)
			return;
		seams_.controlMedia(mediaSourceId_, "stop");
		refreshMediaStatus();
	});
	connect(loopButton_, &QPushButton::toggled, this, [this](bool on) {
		// The loop toggle WRITES the 'looping' setting through the normal settings
		// dispatch -- so it validates, echoes, and emits propertyChanged like any edit.
		if (mediaSourceId_.empty() || !seams_.setSourceProperties)
			return;
		seams_.setSourceProperties(mediaSourceId_, json{{"looping", on}});
		refreshMediaStatus();
	});
	// Live scrub: every user-driven value change (drag step, groove click, keyboard step)
	// seeks through the throttle so playback follows the handle while it moves; the release
	// flush lands the final position. Poll-driven setValue is signal-blocked, so this never
	// re-dispatches a position the engine itself reported.
	seekTrailing_ = new QTimer(this);
	seekTrailing_->setSingleShot(true);
	seekTrailing_->setInterval(kSeekLiveMs);
	connect(seekTrailing_, &QTimer::timeout, this, [this] { flushSeek(); });
	connect(seekSlider_, &QSlider::valueChanged, this, [this](int value) {
		if (mediaSourceId_.empty() || !seams_.seekMedia)
			return;
		seekDirty_ = true;
		timeLabel_->setText(QStringLiteral("%1 / %2").arg(formatMs(value),
								  formatMs(seekSlider_->maximum())));
		if (QDateTime::currentMSecsSinceEpoch() - seekAppliedMs_ >= kSeekLiveMs)
			flushSeek();
		else
			seekTrailing_->start();
	});
	connect(seekSlider_, &QSlider::sliderPressed, this, [this] { seekDirty_ = false; });
	connect(seekSlider_, &QSlider::sliderReleased, this, [this] {
		// A grab that never moved dispatches nothing (the poll value is slightly stale --
		// re-seeking it would audibly jump back).
		if (seekDirty_)
			flushSeek();
		refreshMediaStatus();
	});

	strip->setVisible(false);
	return strip;
}

void MainWindow::flushSeek()
{
	if (mediaSourceId_.empty() || !seams_.seekMedia)
		return;
	seekTrailing_->stop();
	seekAppliedMs_ = QDateTime::currentMSecsSinceEpoch();
	seams_.seekMedia(mediaSourceId_, seekSlider_->value());
}

void MainWindow::buildDocks()
{
	// LEFT: Sources panel -- the live source model (selection drives the Properties dock).
	auto *sourcesDock = new QDockWidget(QStringLiteral("Sources"), this);
	sourcesDock->setObjectName(QStringLiteral("SourcesDock"));
	sourcesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
	sourcesDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

	sourceList_ = new QListWidget(sourcesDock);
	// Long names elide instead of forcing a horizontal scrollbar; the row tooltip carries
	// the full label.
	sourceList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	sourceList_->setTextElideMode(Qt::ElideRight);
	sourceList_->setWordWrap(false);
	sourceList_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(sourceList_, &QListWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
	connect(sourceList_, &QListWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) {
			QListWidgetItem *item = sourceList_->itemAt(pos);
			// View-only (managed) mode: the Sources list stays interactive for
			// selection, but the Remove Source action is never offered.
			if (!item || !seamsSet_ || viewOnly_)
				return;
			sourceList_->setCurrentItem(item);
			QMenu menu(sourceList_);
			QAction *remove = menu.addAction(QStringLiteral("Remove Source"));
			connect(remove, &QAction::triggered, this, &MainWindow::removeSelectedSource);
			menu.exec(sourceList_->mapToGlobal(pos));
		});
	sourcesDock->setWidget(sourceList_);
	addDockWidget(Qt::LeftDockWidgetArea, sourcesDock);

	// RIGHT: Properties panel -- the media transport strip (media sources only) above the
	// descriptor-driven property form.
	auto *propsDock = new QDockWidget(QStringLiteral("Properties"), this);
	propsDock->setObjectName(QStringLiteral("PropertiesDock"));
	propsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
	propsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

	auto *propsBody = new QWidget(propsDock);
	auto *propsLayout = new QVBoxLayout(propsBody);
	propsLayout->setContentsMargins(0, 0, 0, 0);
	propsLayout->setSpacing(0);

	// Dock header: the per-source sender pixel format. The combo dispatches SetSourceFormat --
	// the same verb a wire client uses; a broadcasting source's sender is recreated by the
	// server (brief senderName null + a fresh senderNameResolved).
	formatRow_ = new QWidget(propsBody);
	formatRow_->setObjectName(QStringLiteral("FormatRow"));
	auto *formatLayout = new QHBoxLayout(formatRow_);
	formatLayout->setContentsMargins(8, 8, 8, 8);
	formatLayout->setSpacing(8);
	formatLayout->addWidget(new QLabel(QStringLiteral("Format"), formatRow_));
	formatCombo_ = new QComboBox(formatRow_);
	formatCombo_->addItem(QStringLiteral("srgb87"));
	formatCombo_->addItem(QStringLiteral("linear87"));
	formatCombo_->addItem(QStringLiteral("fp16"));
	formatCombo_->setToolTip(QStringLiteral(
		"Spout sender pixel format. srgb87 (default) for sRGB-decoding receivers; linear87 "
		"raw linear bytes for non-decoding receivers; fp16 linear half-float HDR."));
	formatLayout->addWidget(formatCombo_, /*stretch=*/1);
	// A format change recreates a broadcasting sender -- never off a stray hover-wheel.
	installWheelGuard(formatCombo_);
	connect(formatCombo_, &QComboBox::activated, this, &MainWindow::onFormatSelected);
	formatRow_->setVisible(false);
	propsLayout->addWidget(formatRow_);

	transportStrip_ = buildTransportStrip(propsBody);
	propsLayout->addWidget(transportStrip_);

	// Per-source audio block (audio-bearing types only); sits with the transport strip above
	// the property form, in the same header stack.
	audioStrip_ = new AudioMixerStrip(propsBody);
	propsLayout->addWidget(audioStrip_);

	propertyPanel_ = new PropertyPanel(propsBody);
	propsLayout->addWidget(propertyPanel_, /*stretch=*/1);

	propsDock->setWidget(propsBody);
	addDockWidget(Qt::RightDockWidgetArea, propsDock);

	// RIGHT (below Properties): the per-source filter-chain inspector.
	auto *filtersDock = new QDockWidget(QStringLiteral("Filters"), this);
	filtersDock->setObjectName(QStringLiteral("FiltersDock"));
	filtersDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
	filtersDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
	filterInspector_ = new FilterInspector(filtersDock);
	filtersDock->setWidget(filterInspector_);
	addDockWidget(Qt::RightDockWidgetArea, filtersDock);
	splitDockWidget(propsDock, filtersDock, Qt::Vertical);

	// Default split: side panels wide enough that nothing scrolls at default settings,
	// while the central preview keeps most of the window.
	resizeDocks({sourcesDock, propsDock}, {240, 340}, Qt::Horizontal);
	resizeDocks({propsDock, filtersDock}, {380, 300}, Qt::Vertical);

	// Content-aware vertical split: when the filter list/configure form changes, grant the
	// Filters dock its natural height (capped) so filter settings scroll only on genuine
	// excess. Coalesced -- rebuilds arrive in bursts; small deltas are ignored so the
	// splitter never jitters, and a floating dock is left alone.
	auto *rebalance = new QTimer(this);
	rebalance->setSingleShot(true);
	rebalance->setInterval(0);
	connect(rebalance, &QTimer::timeout, this, [this, propsDock, filtersDock] {
		if (filtersDock->isFloating() || !filtersDock->isVisible())
			return;
		const int column = propsDock->height() + filtersDock->height();
		const int titleBar = filtersDock->height() - filterInspector_->height();
		int desired = filterInspector_->desiredHeight() + std::max(titleBar, 0);
		// The Properties dock keeps what ITS content needs (header rows + unscrolled
		// form); the Filters dock may take everything above that. Both scroll only
		// when the column genuinely cannot hold the two content heights. The props
		// dock's HARD minimum also caps the request -- asking Qt for less than a
		// dock's minimum makes the main window GROW to honor it instead of clamping.
		const int propsNeed = (propsDock->height() - propertyPanel_->height()) +
				      propertyPanel_->naturalHeight();
		const int propsFloor =
			std::max({propsNeed, propsDock->minimumSizeHint().height(), 160});
		desired = std::clamp(desired, 140, std::max(140, column - propsFloor));
		if (qAbs(desired - filtersDock->height()) <= 24)
			return;
		resizeDocks({propsDock, filtersDock}, {column - desired, desired}, Qt::Vertical);
	});
	connect(filterInspector_, &FilterInspector::desiredHeightChanged, rebalance,
		qOverload<>(&QTimer::start));
	// The Properties form can change height with no filter-side event at all (a settings
	// change that adds or removes dependent rows); its rebuilds re-run the same rebalance
	// so the props dock's content need is re-granted, not just read at filter-event time.
	connect(propertyPanel_, &PropertyPanel::contentRebuilt, rebalance, qOverload<>(&QTimer::start));
}

void MainWindow::setPreviewResolver(std::function<obs_source_t *(const std::string &)> resolver)
{
	previewResolver_ = std::move(resolver);
}

void MainWindow::setControlSeams(ControlSeams seams)
{
	seams_ = std::move(seams);
	seamsSet_ = true;
	if (addSourceAction_)
		addSourceAction_->setEnabled(static_cast<bool>(seams_.createSource) && !viewOnly_);
	if (filterInspector_) {
		FilterInspector::FilterSeams fs;
		fs.listAvailableFilters = seams_.listAvailableFilters;
		fs.listFilters = seams_.listFilters;
		fs.addFilter = seams_.addFilter;
		fs.removeFilter = seams_.removeFilter;
		fs.setFilterEnabled = seams_.setFilterEnabled;
		fs.reorderFilter = seams_.reorderFilter;
		fs.renameFilter = seams_.renameFilter;
		fs.listFilterProperties = seams_.listFilterProperties;
		fs.setFilterProperties = seams_.setFilterProperties;
		fs.invokeFilterButton = seams_.invokeFilterButton;
		filterInspector_->setSeams(std::move(fs));
	}
	if (audioStrip_) {
		AudioMixerStrip::AudioSeams as;
		as.getSourceAudio = seams_.getSourceAudio;
		as.setSourceAudio = seams_.setSourceAudio;
		audioStrip_->setSeams(std::move(as));
	}
	// Seed the device selector from the stored selection (the GetStatus read-back), then
	// populate the endpoint list around it.
	if (audioDeviceCombo_ && seams_.listAudioDevices && seams_.setAudioOutputDevice) {
		if (seams_.getStatus) {
			const json status = seams_.getStatus();
			if (status.is_object() && status.contains("result"))
				audioDeviceId_ =
					status["result"].value("audioOutputDevice", std::string("default"));
		}
		refreshAudioDevices();
		audioDeviceCombo_->setEnabled(true);
	}
	refreshSources();
}

void MainWindow::refreshAudioDevices()
{
	if (!audioDeviceCombo_ || !seams_.listAudioDevices)
		return;
	const json reply = seams_.listAudioDevices("render");
	QSignalBlocker block(audioDeviceCombo_);
	audioDeviceCombo_->clear();
	audioDeviceCombo_->addItem(QStringLiteral("Default"), QStringLiteral("default"));
	if (reply.is_object() && reply.contains("result")) {
		for (const auto &d : reply["result"].value("devices", json::array())) {
			audioDeviceCombo_->addItem(
				QString::fromStdString(d.value("name", std::string())),
				QString::fromStdString(d.value("id", std::string())));
		}
	}
	// Reflect the stored selection; a stored endpoint the enumerator no longer lists (yanked
	// device) still shows -- by its raw id -- so the combo never misrepresents the state.
	const QString current = QString::fromStdString(audioDeviceId_);
	int idx = audioDeviceCombo_->findData(current);
	if (idx < 0) {
		audioDeviceCombo_->addItem(current, current);
		idx = audioDeviceCombo_->count() - 1;
	}
	audioDeviceCombo_->setCurrentIndex(idx);
}

void MainWindow::onAudioDeviceSelected(int index)
{
	if (!audioDeviceCombo_ || !seams_.setAudioOutputDevice || index < 0)
		return;
	const std::string deviceId = audioDeviceCombo_->itemData(index).toString().toStdString();
	if (deviceId.empty() || deviceId == audioDeviceId_)
		return;
	const json reply = seams_.setAudioOutputDevice(deviceId);
	if (reply.is_object() && reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Audio Output"),
				     QString::fromStdString(
					     reply["error"].value("message", std::string("failed"))));
		refreshAudioDevices(); // re-seed to the (unchanged) stored selection
		return;
	}
	if (reply.is_object() && reply.contains("result"))
		audioDeviceId_ = reply["result"].value("deviceId", deviceId); // stored echo
}

std::string MainWindow::selectedSourceId() const
{
	const QListWidgetItem *item = sourceList_ ? sourceList_->currentItem() : nullptr;
	return item ? item->data(kSourceIdRole).toString().toStdString() : std::string();
}

std::string MainWindow::selectedSourceType() const
{
	const QListWidgetItem *item = sourceList_ ? sourceList_->currentItem() : nullptr;
	return item ? item->data(kSourceTypeRole).toString().toStdString() : std::string();
}

void MainWindow::refreshSources()
{
	if (!seamsSet_ || !seams_.listSources || !sourceList_)
		return;
	const json reply = seams_.listSources();
	if (!reply.is_object() || !reply.contains("result"))
		return;

	const std::string keepId = selectedSourceId();
	{
		QSignalBlocker block(sourceList_); // re-select below; one explicit rebind after
		sourceList_->clear();
		for (const auto &s : reply["result"].value("sources", json::array())) {
			const QString id = QString::fromStdString(s.value("sourceId", std::string()));
			const QString type = QString::fromStdString(s.value("type", std::string()));
			const QString name = QString::fromStdString(s.value("displayName", std::string()));
			// Audio-only rows get an "audio" tag: the audio capture types always, and a
			// media source whose sender never materializes (broadcasting with a null
			// senderName is the documented audio-only state -- such a source has no video
			// to publish).
			const bool audioType = (type == QLatin1String("audio_input") ||
						type == QLatin1String("audio_output"));
			const bool audioOnlyMedia = type == QLatin1String("media") &&
						    s.value("broadcasting", false) &&
						    s.contains("senderName") && s["senderName"].is_null();
			const QString label = (audioType || audioOnlyMedia)
						      ? QStringLiteral("%1  [%2, audio]").arg(name, type)
						      : QStringLiteral("%1  [%2]").arg(name, type);
			auto *item = new QListWidgetItem(label);
			item->setToolTip(label); // elided rows stay readable on hover
			item->setData(kSourceIdRole, id);
			item->setData(kSourceTypeRole, type);
			item->setData(kSourceFormatRole,
				      QString::fromStdString(s.value("format", std::string("srgb87"))));
			sourceList_->addItem(item);
			if (!keepId.empty() && id.toStdString() == keepId)
				sourceList_->setCurrentItem(item);
		}
	}
	onSelectionChanged(); // rebind the panel/strip to the (possibly changed) selection
}

void MainWindow::onSelectionChanged()
{
	const std::string sourceId = selectedSourceId();
	// Unchanged binding (a list refresh re-landed on the same source): keep the panel as-is.
	// Re-binding would drop an in-flight debounced edit, and the guard also dedupes the second
	// panel rebuild when a GUI action's direct refresh overlaps the queued event-driven one.
	if (!sourceId.empty() && seamsSet_ && sourceId == boundSourceId_)
		return;
	boundSourceId_ = sourceId;

	// Preview follows the selection. Empty selection or an unresolvable id renders the
	// background only; audio-only sources report 0x0 and draw background-only too (the
	// deliberate no-video placeholder -- their list row carries the "audio" tag).
	if (preview_)
		preview_->setSource((!sourceId.empty() && previewResolver_) ? previewResolver_(sourceId)
									    : nullptr);

	if (addFilterAction_)
		addFilterAction_->setEnabled(seamsSet_ && !sourceId.empty() && !viewOnly_);
	if (filterInspector_)
		filterInspector_->setSource(sourceId);
	if (sourceId.empty() || !seamsSet_) {
		propertyPanel_->clearTarget();
		transportStrip_->setVisible(false);
		formatRow_->setVisible(false);
		audioStrip_->setVisible(false);
		audioStrip_->setSource(std::string());
		audioSourceId_.clear();
		mediaSourceId_.clear();
		mediaTimer_->stop();
		return;
	}

	// Format header: reflect the selected source's current format. Refreshed on selection/list
	// refresh only -- format changes carry no event by contract (snapshot observability), and
	// the GUI is normally the only mutator of its own combo.
	if (const QListWidgetItem *item = sourceList_->currentItem()) {
		QSignalBlocker block(formatCombo_);
		formatCombo_->setCurrentText(item->data(kSourceFormatRole).toString());
	}
	// The format is the SENDER pixel format; the audio capture types never create a sender
	// (audio-only by construction), so the row would be an inert control for them.
	const std::string type = selectedSourceType();
	formatRow_->setVisible(type != "audio_input" && type != "audio_output");

	// Bind the panel seams to THIS source (the filter inspector binds itself above).
	propertyPanel_->setTarget(
		[this, sourceId] { return seams_.listSourceProperties(sourceId); },
		[this, sourceId](const json &settings) {
			return seams_.setSourceProperties(sourceId, settings);
		},
		[this, sourceId](const std::string &property) {
			return seams_.invokeSourceButton(sourceId, property);
		});

	const bool isMedia = type == "media";
	transportStrip_->setVisible(isMedia);
	mediaSourceId_ = isMedia ? sourceId : std::string();
	if (isMedia) {
		refreshMediaStatus();
		mediaTimer_->start();
	} else {
		mediaTimer_->stop();
	}

	// The mixer strip serves the audio-bearing types; everything else hides it. (Gain/mute/
	// balance are universal source state on the wire, but inert without audio -- no point
	// showing faders for a color source.)
	const bool audioBearing = isMedia || type == "audio_input" || type == "audio_output";
	const bool audioReady = audioBearing && seams_.getSourceAudio && seams_.setSourceAudio;
	audioStrip_->setVisible(audioReady);
	audioSourceId_ = audioReady ? sourceId : std::string();
	audioStrip_->setSource(audioSourceId_);
}

void MainWindow::onFormatSelected(int)
{
	const std::string sourceId = selectedSourceId();
	if (!seamsSet_ || !seams_.setSourceFormat || sourceId.empty())
		return;
	const std::string format = formatCombo_->currentText().toStdString();
	const json reply = seams_.setSourceFormat(sourceId, format);
	if (reply.is_object() && reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Source Format"),
				     QString::fromStdString(
					     reply["error"].value("message", std::string("failed"))));
		// A 1011 means the format applied but the sender could not re-attach (the source
		// stopped broadcasting); a refresh re-pulls the authoritative format + state.
		refreshSources();
		return;
	}
	if (QListWidgetItem *item = sourceList_->currentItem())
		item->setData(kSourceFormatRole, QString::fromStdString(format));
}

void MainWindow::addSourceDialog()
{
	if (!seamsSet_ || !seams_.getVersion || !seams_.createSource)
		return;
	const json version = seams_.getVersion();
	if (!version.is_object() || !version.contains("result"))
		return;

	QDialog dialog(this);
	dialog.setWindowTitle(QStringLiteral("Add Source"));
	auto *form = new QFormLayout(&dialog);
	auto *typeCombo = new QComboBox(&dialog);
	for (const auto &t : version["result"]["capabilities"].value("sourceTypes", json::array()))
		typeCombo->addItem(QString::fromStdString(t.get<std::string>()));
	auto *nameEdit = new QLineEdit(&dialog);
	nameEdit->setPlaceholderText(QStringLiteral("(server default)"));
	form->addRow(QStringLiteral("Type"), typeCombo);
	form->addRow(QStringLiteral("Name"), nameEdit);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	form->addRow(buttons);

	if (dialog.exec() != QDialog::Accepted || typeCombo->currentText().isEmpty())
		return;

	const json reply = seams_.createSource(typeCombo->currentText().toStdString(),
					       nameEdit->text().toStdString());
	if (reply.is_object() && reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Add Source"),
				     QString::fromStdString(
					     reply["error"].value("message", std::string("failed"))));
		return;
	}
	// sourceAdded also lands via onControlEvent; refresh now and select the new source.
	refreshSources();
	if (reply.is_object() && reply.contains("result")) {
		const QString newId =
			QString::fromStdString(reply["result"].value("sourceId", std::string()));
		for (int i = 0; i < sourceList_->count(); ++i) {
			if (sourceList_->item(i)->data(kSourceIdRole).toString() == newId) {
				sourceList_->setCurrentRow(i);
				break;
			}
		}
	}
}

void MainWindow::removeSelectedSource()
{
	const std::string sourceId = selectedSourceId();
	if (sourceId.empty() || !seams_.removeSource)
		return;
	const json reply = seams_.removeSource(sourceId);
	if (reply.is_object() && reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Remove Source"),
				     QString::fromStdString(
					     reply["error"].value("message", std::string("failed"))));
		return;
	}
	refreshSources();
}

void MainWindow::refreshMediaStatus()
{
	if (mediaSourceId_.empty() || !seams_.getMediaStatus)
		return;
	const json reply = seams_.getMediaStatus(mediaSourceId_);
	if (!reply.is_object() || !reply.contains("result"))
		return;
	const json &r = reply["result"];

	lastMediaState_ = r.value("state", std::string("none"));
	playPauseButton_->setText(lastMediaState_ == "playing" ? QStringLiteral("Pause")
							       : QStringLiteral("Play"));

	const int64_t position = r.value("positionMs", int64_t(0));
	const bool hasDuration = r.contains("durationMs") && r["durationMs"].is_number();
	const int64_t duration = hasDuration ? r["durationMs"].get<int64_t>() : 0;
	seekSlider_->setEnabled(hasDuration);
	if (hasDuration) {
		QSignalBlocker block(seekSlider_);
		seekSlider_->setRange(0, int(duration));
		// While the handle is held the drag owns both the slider and the readout (the
		// valueChanged path writes the label); the poll resumes after release.
		if (!seekSlider_->isSliderDown()) {
			seekSlider_->setValue(int(std::min(position, duration)));
			timeLabel_->setText(
				QStringLiteral("%1 / %2").arg(formatMs(position), formatMs(duration)));
		}
	} else {
		timeLabel_->setText(QStringLiteral("%1 / live").arg(formatMs(position)));
	}
	{
		QSignalBlocker block(loopButton_);
		loopButton_->setChecked(r.value("looping", false));
	}
}

void MainWindow::onControlEvent(const std::string &name, const nlohmann::json &data)
{
	// Deferred handling: events fire synchronously inside dispatches this window may itself
	// have initiated (e.g. a panel apply); never mutate widgets mid-callstack.
	if (name == "sourceAdded" || name == "sourceRemoved") {
		if (sourcesRefreshQueued_)
			return;
		sourcesRefreshQueued_ = true;
		QTimer::singleShot(0, this, [this] {
			sourcesRefreshQueued_ = false;
			refreshSources();
		});
		return;
	}
	if (name == "propertyChanged") {
		// A settings apply landed (wire client or this GUI -- one emission path). Refresh
		// the open panel when it shows that source's own (non-filter) properties; route
		// filter-scoped applies to the inspector's configure pane.
		const std::string sourceId =
			data.is_object() ? data.value("sourceId", std::string()) : std::string();
		if (sourceId == selectedSourceId()) {
			if (data.contains("filterId")) {
				if (filterInspector_)
					filterInspector_->onFilterPropertyChanged(
						data.value("filterId", std::string()));
			} else {
				propertyPanel_->scheduleReload();
				if (sourceId == mediaSourceId_)
					QTimer::singleShot(0, this, &MainWindow::refreshMediaStatus);
			}
		}
		return;
	}
	if (name == "filterAdded" || name == "filterRemoved" || name == "filterChanged") {
		// Chain membership/order/state changed (wire client or this GUI -- one emission
		// path). The inspector reloads from ListFilters; refreshes are queued + coalesced
		// and the same-binding guard keeps the configure pane's in-flight edits.
		const std::string sourceId =
			data.is_object() ? data.value("sourceId", std::string()) : std::string();
		if (filterInspector_ && !sourceId.empty() && sourceId == selectedSourceId())
			filterInspector_->scheduleRefresh();
		return;
	}
	if (name == "mediaChanged") {
		const std::string sourceId =
			data.is_object() ? data.value("sourceId", std::string()) : std::string();
		if (!sourceId.empty() && sourceId == mediaSourceId_)
			QTimer::singleShot(0, this, &MainWindow::refreshMediaStatus);
		return;
	}
	if (name == "audioChanged") {
		// Audio state changed (wire client or this GUI -- one emission path). The open
		// strip applies the changed fields; in-flight fader edits win (the strip guards).
		const std::string sourceId =
			data.is_object() ? data.value("sourceId", std::string()) : std::string();
		if (!sourceId.empty() && sourceId == audioSourceId_ && audioStrip_) {
			const json copy = data;
			QTimer::singleShot(0, this, [this, copy] {
				if (audioStrip_)
					audioStrip_->onAudioChanged(copy);
			});
		}
		return;
	}
	if (name == "broadcastChanged") {
		// Broadcast state changed (wire client or this GUI -- one emission path). Re-derive the
		// instance-wide aggregate via the read-only query seam rather than trusting the event's
		// per-source flag (a subset stop may leave others broadcasting), and update the button.
		QTimer::singleShot(0, this, [this] {
			if (broadcastQueryHandler_)
				applyBroadcastActive(broadcastQueryHandler_());
		});
		return;
	}
}

void MainWindow::onAudioLevels(const nlohmann::json &data)
{
	// The 100 ms levels tick (main thread, never inside a dispatch) -- direct widget updates.
	if (!data.is_object())
		return;
	if (masterMeter_ && data.contains("master") && data["master"].is_object()) {
		const json &m = data["master"];
		masterMeter_->setClipped(m.value("clipped", false));
		masterMeter_->setLevels(m.value("peak", 0.0), m.value("rms", 0.0));
	}
	if (!audioStrip_)
		return;
	if (!audioSourceId_.empty() && data.contains("sources") && data["sources"].is_array()) {
		for (const auto &s : data["sources"]) {
			if (s.value("sourceId", std::string()) == audioSourceId_) {
				audioStrip_->setSourceLevels(s.value("peak", 0.0), s.value("rms", 0.0));
				return;
			}
		}
	}
	// No row for the bound source (not broadcasting, video-only media, or nothing bound).
	audioStrip_->clearSourceLevels();
}

void MainWindow::setFleetStatus(const QString &summary)
{
	fleetStatus_ = summary;
	refreshStatus();
}

void MainWindow::setControlStatus(const QString &status)
{
	controlStatus_ = status;
	refreshStatus();
}

void MainWindow::refreshStatus()
{
	QString text;
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		const double fps = ovi.fps_den ? double(ovi.fps_num) / double(ovi.fps_den) : 0.0;
		text = QStringLiteral("Preview %1x%2  @  %3 fps")
			       .arg(ovi.base_width)
			       .arg(ovi.base_height)
			       .arg(fps, 0, 'g', 4);
	} else {
		text = QStringLiteral("Preview (no video info)");
	}

	// M2.2: sender state on the same 1s refresh (engine slot snapshot is mutex-guarded + cheap).
	// Shows the count plus the first ACTUAL name; the full set lives in the Sources dock.
	// Audio-only slots are not senders (no name, nothing published) -- excluded from the count.
	if (engine_) {
		const auto infos = engine_->slotInfos();
		std::vector<const SenderSlotInfo *> senders;
		senders.reserve(infos.size());
		for (const auto &info : infos) {
			if (!info.audioOnly)
				senders.push_back(&info);
		}
		text += QStringLiteral("  |  senders: %1").arg(senders.size());
		if (!senders.empty()) {
			const auto &first = *senders.front();
			const std::string &name = !first.actualName.empty() ? first.actualName
									    : first.requestedName;
			text += QStringLiteral(" (%1").arg(QString::fromStdString(name));
			if (senders.size() > 1)
				text += QStringLiteral(" +%1 more").arg(senders.size() - 1);
			text += QStringLiteral(")");
		}
	}

	// M2.3: the worker-tier fleet summary (empty in single-instance mode).
	if (!fleetStatus_.isEmpty())
		text += QStringLiteral("  |  ") + fleetStatus_;

	// M6: the control endpoint state.
	if (!controlStatus_.isEmpty())
		text += QStringLiteral("  |  ") + controlStatus_;

	statusLabel_->setText(text);

	// Item 05: keep the Profiles dirty asterisk current on the same 1s tick (cheap: a single
	// snapshot+compare, and only when a profile is active in standalone mode).
	if (profilesEnabled_ && !activeProfile_.isEmpty())
		updateProfilesDirty();
}

} // namespace moxrelay
