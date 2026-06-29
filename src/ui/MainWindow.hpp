// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// MainWindow -- the application shell: toolbar (Add Source / Add Filter / Start Broadcast),
// the live Sources dock (selection drives the Properties dock), the Properties dock (the
// media transport strip + the descriptor-driven PropertyPanel), the preview as the central
// widget, and the status bar (preview fps + senders + fleet + control endpoint).
//
// CONTROL SEAMS: every GUI read/mutation goes through the injected ControlSeams -- thin
// std::function wrappers main.cpp binds to the ControlVerbs gui* dispatch methods -- so a GUI
// action takes the EXACT path a wire request takes (validation, applied echo, event emission;
// the registry-SoT rule). The window NEVER talks to libobs or the engine for mutations; the
// borrowed engine pointer feeds the read-only status readout only.
//
// EVENTS: main.cpp chains the ControlVerbs event sink to onControlEvent so the window reacts
// to sourceAdded/sourceRemoved (list refresh), propertyChanged (open-panel refresh -- including
// changes made by WS clients), and mediaChanged (transport strip refresh). All handling is
// deferred (queued) -- events can fire synchronously inside a dispatch this window initiated.

#pragma once

#include <nlohmann/json.hpp>

#include <QMainWindow>
#include <QString>

#include <cstdint>
#include <functional>
#include <string>

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QWidget;

typedef struct obs_source obs_source_t; // matches the libobs typedef; full obs.h not needed here

namespace moxrelay {

class AudioMixerStrip;
class FilterInspector;
class LevelMeter;
class MoxDisplayWidget;
class PropertyPanel;
class SpoutSenderEngine;

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	// Thin wrappers over the ControlVerbs gui* dispatch seams; each returns the full reply
	// envelope ({id, result|error}).
	struct ControlSeams {
		std::function<nlohmann::json()> getVersion;
		std::function<nlohmann::json()> listSources;
		std::function<nlohmann::json(const std::string &type, const std::string &displayName)> createSource;
		std::function<nlohmann::json(const std::string &sourceId)> removeSource;
		std::function<nlohmann::json(const std::string &sourceId)> listSourceProperties;
		std::function<nlohmann::json(const std::string &sourceId, const nlohmann::json &settings)>
			setSourceProperties;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &property)>
			invokeSourceButton;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &format)>
			setSourceFormat;
		std::function<nlohmann::json(const std::string &sourceId)> getMediaStatus;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &action)> controlMedia;
		std::function<nlohmann::json(const std::string &sourceId, int64_t positionMs)> seekMedia;
		// Audio (the mixer strip + the toolbar device selector / meters).
		std::function<nlohmann::json()> getStatus; // device read-back (audioOutputDevice)
		std::function<nlohmann::json(const std::string &flow)> listAudioDevices;
		std::function<nlohmann::json(const std::string &deviceId)> setAudioOutputDevice;
		std::function<nlohmann::json(const std::string &sourceId)> getSourceAudio;
		std::function<nlohmann::json(const std::string &sourceId, const nlohmann::json &fields)>
			setSourceAudio;
		// Filter chain (forwarded into the FilterInspector's seams).
		std::function<nlohmann::json(const std::string &sourceId)> listAvailableFilters;
		std::function<nlohmann::json(const std::string &sourceId)> listFilters;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterType,
					     const std::string &name)>
			addFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId)> removeFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId, bool enabled)>
			setFilterEnabled;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId, int index)>
			reorderFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId,
					     const std::string &name)>
			renameFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId)>
			listFilterProperties;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId,
					     const nlohmann::json &settings)>
			setFilterProperties;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId,
					     const std::string &property)>
			invokeFilterButton;
		// Item 05: standalone profiles (the toolbar Profiles control). Bound ONLY in standalone
		// mode; in managed mode they stay unset and the whole control is hidden.
		std::function<nlohmann::json()> listProfiles;
		std::function<nlohmann::json(const std::string &name)> loadProfile;
		std::function<nlohmann::json(const std::string &name)> saveProfile;
		std::function<nlohmann::json(const std::string &name)> deleteProfile;
		std::function<bool(const std::string &name)> profileIsDirty;
		// Per-profile FPS tier (the standalone toolbar FPS dropdown). setFpsTier LIVE-re-tiers the
		// engine through the verb layer (obs_reset_video) and updates the running tier; currentFpsTier
		// reads the live tier so the dropdown can reflect it after a profile load. Bound ONLY in
		// standalone mode (the dropdown is part of the standalone group, hidden in managed mode). The
		// tier is captured by the per-profile snapshot, so a change marks the active profile dirty and
		// a Save persists it -- the same dirty/Save path source/audio/filter edits use.
		std::function<nlohmann::json(int fpsTier)> setFpsTier; // {result|error}
		std::function<int()> currentFpsTier;
	};

	explicit MainWindow(QWidget *parent = nullptr);

	// The preview widget so main.cpp can clear it ahead of teardown.
	MoxDisplayWidget *preview() const { return preview_; }

	// Preview-source resolver (render plumbing, not a mutation seam): maps a sourceId to the
	// verb-layer-owned source so the central preview can follow the Sources-list selection.
	// The window stores no source refs -- MoxDisplayWidget weak-refs the resolved pointer on
	// every rebind, and selection changes re-resolve through this hook.
	void setPreviewResolver(std::function<obs_source_t *(const std::string &)> resolver);

	// Bind the control seams; enables Add Source/remove/properties and populates the list.
	void setControlSeams(ControlSeams seams);

	// ControlVerbs event tap (chained after the WS publish by main.cpp). Main thread; handling
	// is internally deferred (queued) for re-entrancy safety.
	void onControlEvent(const std::string &name, const nlohmann::json &data);

	// The in-process audioLevels consumer (ControlServer's levels sink): feeds the toolbar
	// master meter + the open mixer strip's source row. Main thread (the 100 ms timer); the
	// data is the SAME payload wire subscribers receive -- one emission path.
	void onAudioLevels(const nlohmann::json &data);

	// M2.3: fleet state from the Supervisor (worker tiers), appended to the status bar on the
	// existing 1s refresh. Empty = single-instance (no fleet segment shown).
	void setFleetStatus(const QString &summary);

	// M2.2: the engine pointer feeds the STATUS-BAR sender readout only. Since M6 the window
	// never drives the engine directly -- broadcast goes through the handler below, so the GUI
	// and the control API share ONE state path. The window does NOT own the engine -- main.cpp
	// does, and it stops the engine after this window is destroyed (and before libobs shutdown).
	void setEngine(SpoutSenderEngine *engine);

	// M6: the "Start/Stop Broadcast" toolbar action delegates here (the control-verb layer).
	// The handler receives the DESIRED state (true = start) and returns the resulting
	// instance-wide broadcasting state. Setting a handler enables the action.
	void setBroadcastHandler(std::function<bool(bool start)> handler);

	// M8: read-only query seam reporting the current instance-wide broadcasting state WITHOUT
	// toggling the engine. Lets the window re-sync the button when broadcast state changes via
	// the control API (broadcastChanged event). Binding it seeds the button immediately.
	void setBroadcastQueryHandler(std::function<bool()> handler);

	// M6: control endpoint state for the status bar (empty = no control segment shown).
	void setControlStatus(const QString &status);

	// Item 05: window geometry as a plain json object {x, y, width, height, maximized}, for the
	// ControlVerbs window-layout seam (a profile persists / restores the layout). The window never
	// touches the profile store; ControlVerbs reads/writes the JSON across the seam.
	nlohmann::json windowLayoutJson() const;
	void applyWindowLayout(const nlohmann::json &layout);

	// Item 05: enable the standalone Profiles toolbar control (standalone only). Off by default; the
	// whole control stays hidden until this is called with true (managed mode never calls it). When
	// enabled, the control populates from the profile seams and auto-loads the last profile.
	void setStandaloneProfilesEnabled(bool enabled);

	// Source view-only lock for managed (helper) mode. With enabled=false every source-EDITING
	// affordance is suppressed -- the Add Source / Add Filter toolbar actions are hidden, the
	// per-source Remove Source context action is never offered, and the format combo, the
	// PropertyPanel, and the FilterInspector are disabled -- so the user can never mutate the
	// source set the owning integrator reconciles. The Sources list (selection), the central
	// preview, and the media transport strip stay live, so a source can still be picked, viewed,
	// and played. Standalone mode (enabled=true, the default) keeps everything editable. The lock
	// is durable against the seam layer's own re-enables (see the .cpp for why), so a later
	// setControlSeams / onSelectionChanged cannot undo it.
	void setSourceEditingEnabled(bool enabled);

	// Managed-mode identity: set the window title to "MoxRelay - <displayName>" (or plain
	// "MoxRelay" when displayName is empty). The runtime managed identity only; never branded.
	void setManagedIdentity(const QString &displayName);

	// Item 04: open the modal Settings dialog (Tools > Settings...). Standalone-facing; in managed
	// mode the window is never shown so it is unreachable in practice.
	void openSettingsDialog();
	// Item 04: show the About box (Help > About MoxRelay) -- version + libobs/OBS disclaimer + GPLv3.
	void showAboutBox();

	// Item 04: close/minimize-to-tray policy seams the TrayController populates. Each returns true
	// when the window should HIDE to the tray instead of running its default close/minimize. Unset
	// (the default, e.g. managed mode) -> the existing behavior is unchanged. The handler, not the
	// window, encodes the standalone/managed + toggle decision; the window only consults it.
	void setCloseToTrayHandler(std::function<bool()> handler);    // true => hide instead of accept-close
	void setMinimizeToTrayHandler(std::function<bool()> handler); // true => hide instead of minimize

protected:
	// A window hidden in the tray does not emit lastWindowClosed when closed (Qt only counts visible
	// windows), and with quitOnLastWindowClosed disabled an accepted close would otherwise leave the
	// event loop running -- so quit explicitly on any accepted close; for a visible window this
	// coincides with the default lastWindowClosed quit (quit() is idempotent). Item 04: when the
	// close-to-tray handler says so (live toggle, both modes), ignore the close and hide instead.
	void closeEvent(QCloseEvent *event) override;
	// Item 04: catch a user minimize so minimize-to-tray (live toggle, both modes) can hide instead.
	void changeEvent(QEvent *event) override;

private:
	void refreshStatus(); // reads obs_get_video_info + engine slot info on the UI thread
	void buildToolBar();
	void buildMenuBar(); // item 04: File>Quit, Tools>Settings..., Help>About MoxRelay
	void buildDocks();
	QWidget *buildTransportStrip(QWidget *parent);
	void toggleBroadcast();
	void applyBroadcastActive(bool active); // sets broadcastActive_ + the toolbar button label

	// Item 05: the standalone Profiles toolbar control.
	void buildProfilesControl(QWidget *toolbar); // mounts the label + combo + action menu + FPS combo
	void onFpsTierSelected(int);  // FPS combo -> live re-tier + mark the active profile dirty
	void refreshFpsTierCombo();   // set the FPS combo to reflect the live tier (after a profile load)
	void refreshProfilesList();   // ListProfiles -> the combo (active selection preserved)
	void updateProfilesDirty();   // recompute the dirty asterisk on the active profile name
	void onProfileSelected(int);  // combo -> LoadProfile (confirm if the active profile is dirty)
	void doProfileSave();         // Save (overwrite the active profile)
	void doProfileSaveAs();       // Save As (prompt for a new name)
	void doProfileRename();       // Rename the active profile
	void doProfileDuplicate();    // Duplicate the active profile under a new name
	void doProfileDelete();       // Delete the active profile (confirm)
	void doProfileImport();       // Import a profile JSON file from disk
	void doProfileExport();       // Export the active profile JSON to disk
	std::string activeProfileName() const; // the combo's current profile ("" = none)

	void refreshSources();        // ListSources -> the dock list (selection preserved by id)
	void onSelectionChanged();    // rebinds the panel + the transport strip
	void onFormatSelected(int);   // format combo -> SetSourceFormat dispatch
	void refreshAudioDevices();   // ListAudioDevices(render) -> the toolbar combo (Default first)
	void onAudioDeviceSelected(int); // device combo -> SetAudioOutputDevice dispatch
	void addSourceDialog();       // typed picker -> CreateSource dispatch
	void removeSelectedSource();  // context menu -> RemoveSource dispatch
	void refreshMediaStatus();    // GetMediaStatus -> strip state (poll + event driven)
	void flushSeek();             // immediate SeekMedia at the current slider position
	std::string selectedSourceId() const;
	std::string selectedSourceType() const;

	MoxDisplayWidget *preview_ = nullptr;
	QLabel *statusLabel_ = nullptr;
	QListWidget *sourceList_ = nullptr;
	QAction *addSourceAction_ = nullptr;
	QAction *addFilterAction_ = nullptr;
	QAction *broadcastAction_ = nullptr;
	PropertyPanel *propertyPanel_ = nullptr;
	FilterInspector *filterInspector_ = nullptr;

	// Per-source sender format header (Properties dock).
	QWidget *formatRow_ = nullptr;
	QComboBox *formatCombo_ = nullptr;

	// Per-source audio mixer strip (visible only for audio-bearing source types) + the
	// toolbar's instance-level output-device selector and master meter.
	AudioMixerStrip *audioStrip_ = nullptr;
	QComboBox *audioDeviceCombo_ = nullptr;
	LevelMeter *masterMeter_ = nullptr;
	std::string audioSourceId_; // mixer strip target ("" = hidden)
	std::string audioDeviceId_ = "default"; // last stored selection (GetStatus read-back)

	// Media transport strip (visible only for media-type sources).
	QWidget *transportStrip_ = nullptr;
	QPushButton *playPauseButton_ = nullptr;
	QPushButton *restartButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;
	QPushButton *loopButton_ = nullptr;
	QSlider *seekSlider_ = nullptr;
	QLabel *timeLabel_ = nullptr;
	QTimer *mediaTimer_ = nullptr;
	QTimer *seekTrailing_ = nullptr; // trailing live-scrub flush (single shot)
	qint64 seekAppliedMs_ = 0;       // last SeekMedia dispatch time (throttle anchor)
	bool seekDirty_ = false;         // a user change occurred since the handle was grabbed
	std::string mediaSourceId_;   // strip target ("" = hidden)
	std::string lastMediaState_;  // drives the play/pause caption
	std::string boundSourceId_;   // panel binding ("" = none); guards same-source rebinds

	// Item 05: the standalone toolbar group -- the per-profile FPS dropdown + the Profiles control.
	// The whole group is one toolbar slot so a single QAction toggle hides it as a unit in managed
	// mode (a QToolBar lays out an added widget via the QAction addWidget() returns -- hiding the
	// widget alone is unreliable, so the standalone gate flips these actions instead).
	QAction *standaloneGroupAction_ = nullptr;     // bar->addWidget(...) action for the group container
	QAction *standaloneSeparatorAction_ = nullptr; // the separator preceding the group
	QWidget *profilesContainer_ = nullptr; // the whole control (FPS combo + profile combo + menu button)
	QComboBox *fpsTierCombo_ = nullptr;    // per-profile FPS tier dropdown (live re-tier on change)
	QComboBox *profilesCombo_ = nullptr;
	QString activeProfile_;       // the loaded/selected profile name ("" = none)
	bool profilesEnabled_ = false; // standalone only
	bool profilesRefreshing_ = false; // re-entrancy guard while repopulating the combo
	bool fpsRefreshing_ = false;      // re-entrancy guard while setting the FPS combo programmatically

	QString fleetStatus_;   // M2.3: latest Supervisor summary ("" = no fleet)
	QString controlStatus_; // M6: control endpoint state ("" = none)

	SpoutSenderEngine *engine_ = nullptr; // not owned; status readout only
	std::function<obs_source_t *(const std::string &)> previewResolver_;
	std::function<bool(bool)> broadcastHandler_;
	std::function<bool()> broadcastQueryHandler_; // M8: read-only "are we broadcasting now?"
	std::function<bool()> closeToTrayHandler_;    // item 04: true => hide-to-tray on close
	std::function<bool()> minimizeToTrayHandler_; // item 04: true => hide-to-tray on minimize
	ControlSeams seams_;
	bool seamsSet_ = false;
	bool viewOnly_ = false; // managed mode: source-editing affordances hidden/disabled (see setSourceEditingEnabled)
	bool sourcesRefreshQueued_ = false;
	bool broadcastActive_ = false;
};

} // namespace moxrelay
