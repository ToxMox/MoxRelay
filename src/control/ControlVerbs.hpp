// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ControlVerbs -- the control-API method handlers (docs/control-api.asyncapi.yaml is the wire
// contract; this file implements its verb semantics against the live engine).
//
// THREADING: every public method runs on the Qt MAIN thread only (ControlServer marshals each
// WS request there before dispatch; the poll tick and the GUI seam are main-thread by
// construction). No internal locking is needed or present.
//
// OWNERSHIP: this object OWNS the instance's sources -- the boot adopt path runs via
// adoptBootSources(), runtime ones are created by CreateSource. Each source carries the factory
// contract refs (showing + strong); releaseAllSources() releases everything that is still
// registered and MUST run after engine.stop() and before the engine bootstrap shuts down
// (the teardown order in main.cpp).
//
// IDENTITY: wire identity is the server-generated sourceId (src_N) / filterId (flt_N). The
// Spout sender name is read back from the engine (actual, possibly collision-suffixed) and is
// null on the wire until resolved; resolution is announced via the senderNameResolved event.

#pragma once

#include "obs/SourceFactory.hpp"

#include <nlohmann/json.hpp>

#include <obs.h>

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace moxrelay {

class AudioMixEngine;
class SpoutSenderEngine;

// The identity block every reply/event carries (GetVersion/GetStatus/event data).
struct InstanceIdentity {
	std::string instanceId;
	int port = 0;
	int fpsTier = 0;
	std::string version;
	std::string spoutPrefix;
	std::string machine; // sender-name machine segment (engine attach input)
	std::string ownerId; // opaque owner token (empty = unowned)
};

class ControlVerbs {
public:
	// `audio` may be null (audio-less harness contexts); every audio pairing below no-ops then.
	// The attach edge is shared: a slot attach is paired with the audio tap attach, a detach
	// with the tap removal -- and the tap removal ALWAYS precedes the source release (the same
	// ordering discipline as the media-signal contexts).
	ControlVerbs(SpoutSenderEngine *engine, AudioMixEngine *audio, InstanceIdentity identity);
	~ControlVerbs(); // releases any still-owned sources (normally releaseAllSources ran already)

	ControlVerbs(const ControlVerbs &) = delete;
	ControlVerbs &operator=(const ControlVerbs &) = delete;

	// Adopt the boot-time (spawn-configured) sources. `attachedInOrder` = the caller already
	// attached every source to the engine in vector order (the worker boot path); slot ids are
	// zipped from the engine's attach-ordered snapshot. False = not yet broadcasting (GUI).
	void adoptBootSources(std::vector<CreatedSource> sources, bool attachedInOrder);

	// Where emitted events go (ControlServer::publishEvent). Main-thread, non-blocking.
	void setEventSink(std::function<void(const std::string &, const nlohmann::json &)> sink);

	// Item 05 (standalone profiles): the live re-tier seam. LoadProfile runs it when the loaded
	// profile's fpsTier differs from the running tier. The handler re-runs obs_reset_video with the
	// new frame rate (ObsBootstrap::retier) AFTER this object has torn down all GPU work + the render
	// callback, and BEFORE it rebuilds the sources -- the strict ordering lives here in
	// loadProfileSources(), the handler is ONLY the obs_reset_video step. Returns true on a
	// successful reset. The change is TRANSIENT: the global engine/fpsTier QSetting (the no-profile
	// fallback) is never overwritten. Unset (managed mode / no GUI) -> a tier mismatch is ignored.
	void setRetierHandler(std::function<bool(int fpsNum)> handler);

	// Item 05: window-layout seams (the window geometry persisted in / restored from a profile). The
	// window NEVER mutates this object and this object NEVER touches Qt, so the geometry crosses the
	// seam as a plain json object {x, y, width, height, maximized}. SaveProfile reads it via the
	// provider; LoadProfile applies it via the applier. Unset -> the profile carries no window block.
	void setWindowLayoutSeams(std::function<nlohmann::json()> provider,
				  std::function<void(const nlohmann::json &)> applier);

	// Has a Shutdown been accepted on this instance (the idempotent-reack / no-more-1009 gate)?
	bool isStopping() const { return stopping_; }

	// Item 05: the CURRENT live fps tier. Boots at identity_.fpsTier; a standalone profile load can
	// re-tier it live (loadProfileSources updates identity_.fpsTier in place), so the discovery-file
	// publisher reads this rather than a boot-time snapshot to avoid publishing a stale tier.
	int currentFpsTier() const { return identity_.fpsTier; }

	// The event names THIS instance accepts in Subscribe (contract: unsupported names are
	// silently omitted from the ack).
	std::vector<std::string> subscribableEvents() const;

	// Dispatch one request (already envelope-validated by the server). Returns the COMPLETE
	// reply envelope: {id, result} or {id, error}. Unknown method -> -32601.
	nlohmann::json dispatch(const nlohmann::json &id, const std::string &method, const nlohmann::json &params);

	// 1s tick (ControlServer timer): refresh engine state; fires senderNameResolved on
	// resolution transitions. Cheap (one mutex-guarded engine snapshot).
	void pollTick();

	// The status event's data payload (contract StatusEventEnvelope.data).
	nlohmann::json statusEventData();

	// The audioLevels event's data payload (contract AudioLevelsEventEnvelope.data): master
	// post-sum/pre-clamp levels + per-source post-gain/mute levels for the broadcasting
	// audio-capable sources. DRAINS the engine's meter window -- call it only from the one
	// emission tick (ControlServer's 100 ms timer, gated on at least one subscriber).
	nlohmann::json audioLevelsEventData();

	// The instanceShuttingDown event's data payload (reason: shutdown|restart|fault).
	nlohmann::json shuttingDownEventData(const std::string &reason);

	// Teardown step: detach anything still broadcasting and release every owned source ref.
	// Idempotent. Runs while the engine bootstrap is still up.
	void releaseAllSources();

	// GUI seam: instance-wide broadcast toggle through the SAME handlers the wire uses (one
	// state path -- the toolbar action must never bypass the verb layer). Returns the new
	// instance-wide broadcasting state.
	bool guiToggleBroadcast(bool start);
	bool anyBroadcasting();

	// GUI seam: media transport through the SAME dispatch path as the wire (the GUI's media
	// transport controls). Each returns the full reply envelope ({id:0, result|error}).
	nlohmann::json guiGetMediaStatus(const std::string &sourceId);
	nlohmann::json guiControlMedia(const std::string &sourceId, const std::string &action);
	nlohmann::json guiSeekMedia(const std::string &sourceId, int64_t positionMs);

	// GUI seam: audio, same single-path rule (the mixer UI never touches engine audio state
	// directly; every mutation rides the wire verbs, so validation, the clamp echo, and
	// audioChanged emission are identical for GUI and WS actions). `fields` for
	// guiSetSourceAudio carries any subset of gain/muted/balance.
	nlohmann::json guiListAudioDevices(const std::string &flow);
	nlohmann::json guiSetAudioOutputDevice(const std::string &deviceId);
	nlohmann::json guiGetSourceAudio(const std::string &sourceId);
	nlohmann::json guiSetSourceAudio(const std::string &sourceId, const nlohmann::json &fields);

	// GUI seam: source CRUD + properties, same single-path rule -- every GUI mutation is
	// a wire dispatch, so validation, the applied echo, and event emission (sourceAdded/
	// sourceRemoved/propertyChanged) are identical for GUI and WS actions.
	nlohmann::json guiGetVersion();
	nlohmann::json guiGetStatus(); // device read-back (audioOutputDevice) for the toolbar combo
	nlohmann::json guiListSources();

	// GUI render-plumbing seam (NOT a wire dispatch): resolve a sourceId to the owned source so
	// the preview display can follow the Sources-list selection. Main-thread only (every
	// dispatch is marshalled to the Qt main thread, so the lookup cannot race a RemoveSource).
	// The returned pointer is BORROWED -- the caller must take its own ref immediately
	// (MoxDisplayWidget::setSource stores a weak ref). Unknown id -> nullptr.
	obs_source_t *guiPreviewSource(const std::string &sourceId);
	nlohmann::json guiCreateSource(const std::string &type, const std::string &displayName);
	nlohmann::json guiRemoveSource(const std::string &sourceId);
	nlohmann::json guiListSourceProperties(const std::string &sourceId);
	nlohmann::json guiSetSourceProperties(const std::string &sourceId, const nlohmann::json &settings);
	nlohmann::json guiInvokeSourceButton(const std::string &sourceId, const std::string &property);
	nlohmann::json guiSetSourceFormat(const std::string &sourceId, const std::string &format);

	// GUI seam: the filter chain, same single-path rule (filterId minting stays inside
	// handleAddFilter; reorder/enable/rename are the wire verbs, so filterAdded/filterRemoved/
	// filterChanged fire identically for GUI and WS actions).
	nlohmann::json guiListAvailableFilters(const std::string &sourceId);
	nlohmann::json guiListFilters(const std::string &sourceId);
	nlohmann::json guiAddFilter(const std::string &sourceId, const std::string &filterType,
				    const std::string &name);
	nlohmann::json guiRemoveFilter(const std::string &sourceId, const std::string &filterId);
	nlohmann::json guiSetFilterEnabled(const std::string &sourceId, const std::string &filterId, bool enabled);
	nlohmann::json guiReorderFilter(const std::string &sourceId, const std::string &filterId, int index);
	nlohmann::json guiRenameFilter(const std::string &sourceId, const std::string &filterId,
				       const std::string &name);
	nlohmann::json guiListFilterProperties(const std::string &sourceId, const std::string &filterId);
	nlohmann::json guiSetFilterProperties(const std::string &sourceId, const std::string &filterId,
					      const nlohmann::json &settings);
	nlohmann::json guiInvokeFilterButton(const std::string &sourceId, const std::string &filterId,
					     const std::string &property);

	// GUI seam: standalone profiles (item 05), same single-path rule -- the toolbar's Profiles
	// control dispatches through these so the verb gate (reject-when-managed), the live re-tier, and
	// the source/filter rebuild are identical for a GUI action and a (standalone) wire client.
	nlohmann::json guiListProfiles();
	nlohmann::json guiLoadProfile(const std::string &name);
	nlohmann::json guiSaveProfile(const std::string &name);
	nlohmann::json guiDeleteProfile(const std::string &name);

	// GUI seam: the per-profile FPS dropdown. LIVE-re-tiers the engine in place to `fpsTier` and
	// updates the running tier (identity_.fpsTier) so the per-profile snapshot captures it -- the
	// active profile then reads dirty and a Save persists it (no separate persistence path). Standalone
	// only: rejects when managed (the helper's frame rate comes from --fps-tier, never the GUI). Reuses
	// the SAME GPU-drain discipline a profile load uses (engine stop drains all wait=true graphics
	// teardown BEFORE obs_reset_video), but keeps the live source set -- obs sources survive a video
	// reset and re-init their GPU resources lazily. Returns {result:{fpsTier}} or {error}.
	nlohmann::json guiSetFpsTier(int fpsTier);

	// GUI helper: the live snapshot vs the saved profile differ (the dirty-state asterisk). Compares
	// the canonical snapshotProfileJson() of the CURRENT live state against the named profile's saved
	// JSON (sources + filters + audio + chrome). Missing/invalid profile -> dirty (true). Standalone
	// helper only; main-thread.
	bool profileIsDirty(const std::string &name);

	// Called by the obs media-signal thunk from ARBITRARY threads (video tick processes the
	// action queue; media_ended arrives from the playback thread). Marshals onto the Qt main
	// thread and re-evaluates the source's media state there (diff-emit mediaChanged).
	void notifyMediaSignal(const std::string &sourceId);

	// Present video-capture device set: deviceId -> friendly name. The unit the arrival watch diffs
	// and the self-heal predicate tests membership against.
	using DeviceSnapshot = std::map<std::string, std::string>;

	// Device-arrival self-heal seam (MAIN THREAD only). Driven by DeviceArrivalWatch (a debounced OS
	// device-notification poke, plus a one-shot post-start baseline poke): (1) enumerate the present
	// video-capture devices via the injected provider; (2) diff against the previous snapshot and emit
	// `devicesChanged` on a non-empty diff -- NEVER on the first (baseline) snapshot, which only
	// establishes the reference set; (3) run a stateless self-heal pass that force-restarts any camera
	// source which should be live but is producing no frames and whose saved device is present (the
	// standard propertyChanged echo then tells every client). Healthy sources are never touched.
	void refreshDeviceSnapshotAndHeal();

	// Inject the device-snapshot provider. The self-test replaces it to drive the diff/heal paths with
	// NO hardware; left unset in production, snapshots come from the real type-only enumeration.
	void setDeviceSnapshotProvider(std::function<DeviceSnapshot()> provider);

private:
	struct FilterRec {
		std::string filterId;
		obs_source_t *filter = nullptr; // owned strong ref (in addition to the chain's own)
		std::string wireType;           // contract filter-type name
		std::string name;               // display label (not unique)
	};

	struct SourceRec {
		std::string sourceId;
		obs_source_t *source = nullptr; // owned (factory contract: showing + strong ref)
		std::string wireType;           // contract source-type name
		std::string displayName;
		std::string format = "srgb87";  // contract SourceFormat (engine attach input)
		bool spawnConfigured = false;
		int slotId = -1;                 // engine slot while broadcasting; -1 otherwise
		std::string senderName;          // actual resolved name ("" = null on the wire)
		bool senderAnnounced = false;    // senderNameResolved fired for the current attach
		// Sticky sender name (the allocator reservation this record OWNS): captured at every
		// attach, kept across broadcast detach/attach so a re-attach publishes the SAME name,
		// released ONLY at RemoveSource / instance teardown (name stability for any name-keyed
		// consumer). Never on the wire -- clients see senderName/senderNameResolved as before.
		std::string allocatedSenderName;
		uint64_t sends = 0;              // last engine snapshot
		bool lastSendOk = false;
		bool initFailed = false;
		std::vector<FilterRec> filters;
		int nextFilterNum = 1;
		void *mediaSignalCtx = nullptr;  // MediaSignalCtx while media signals are connected
		std::string lastMediaState;      // last wire MediaState emitted/observed (diff-emit)
		std::string externalId;          // client-supplied stable id, opaque ("" = null on the wire)
		bool releaseWhenIdle = false;    // Phase 2: drop the showing ref while idle (device released)
		bool disabled        = false;    // Phase 2: hard override -- device released + no frames while on
		bool deviceShowing   = true;     // tracks the single showing ref this helper holds for the source

		// Self-heal discipline (main thread; see refreshDeviceSnapshotAndHeal). The stateless frameless
		// restart is rate-limited by a MONOTONIC cooldown since this source's last heal attempt AND a
		// consecutive-fruitless-attempt cap; arrival-triggered heals reset the streak and bypass the
		// cap, but a FRUITLESS arrival (no frames observed since the last attempt) still honors the
		// cooldown -- a flapping device must not restore the per-flap restart cadence.
		// healLastAttempt is a steady_clock stamp (never wall clock); healEverAttempted gates the first
		// pass (no prior stamp => cooldown does not apply); healFruitlessCount counts consecutive heal
		// attempts that did NOT yield frames (reset on observed nonzero dimensions or a new arrival);
		// healSeenFramesSinceAttempt records that nonzero dimensions were observed since the last heal
		// attempt (a productive spell => the next arrival heal is immediate).
		std::chrono::steady_clock::time_point healLastAttempt{};
		bool healEverAttempted = false;
		int  healFruitlessCount = 0;
		bool healSeenFramesSinceAttempt = false;

		// Property-enumeration cache (the dshow-camera settings-UI freeze fix). obs_source_properties
		// on a camera is the ~1s cost (EnumVideoDevices does one BindToObject per device), so it is run
		// ONCE -- at creation/adopt, on a cascade-key change, and on an explicit ListSourceProperties
		// re-fetch (the hotplug/rescan trigger) -- and the result is REPLAYED for plain toggles instead
		// of re-enumerating. cachedDescriptors is the describeProperties echo (the load-bearing
		// propertyChanged `properties` field); cachedAcceptedKeys is the SetSourceProperties key
		// allowlist. Populated ONLY for types whose cascade keys are audited (sourceCascadeKeysAudited);
		// every other type keeps enumerating live (its enumeration is cheap). propsCached gates reuse.
		bool propsCached = false;
		std::set<std::string> cachedAcceptedKeys;
		nlohmann::json cachedDescriptors = nlohmann::json::array();
	};

	SourceRec *findSource(const std::string &sourceId);
	FilterRec *findFilter(SourceRec &rec, const std::string &filterId);

	// Property-cache plumbing (camera dshow-enumeration freeze fix; see SourceRec cache fields).
	// refreshSourcePropsCache runs the single obs_source_properties enumeration and refills rec's
	// cachedDescriptors + cachedAcceptedKeys (called at creation/adopt, on a cascade-key change, and
	// on ListSourceProperties). sourceCascadeKeysAudited gates which source types trust the cached
	// echo (only types whose full cascade-key set we have audited). isCascadeKey flags a submitted
	// key that re-cascades the dependent option lists (resolution/fps/format), forcing a refresh.
	void refreshSourcePropsCache(SourceRec &rec);
	static bool sourceCascadeKeysAudited(const std::string &wireType);
	static bool isCascadeKey(const std::string &key);

	// The post-update sequence shared by the wire SetSourceProperties path and the device-arrival
	// self-heal: inert-until-configured activation, the descriptor-cache refresh (cascade path), the
	// standard propertyChanged echo emission, and returning that echo. `settings` selects the keys that
	// appear in the echo's `applied` map (each keyed to its post-update stored value, falling back to
	// the submitted value). `plainToggle` replays the cached descriptors verbatim (no enumeration);
	// otherwise an audited type refreshes the cache and a non-audited type enumerates live. Extracted so
	// the self-heal path reuses the EXACT wire echo shape without duplicating it.
	nlohmann::json emitPropertyChangedEcho(SourceRec &rec, const nlohmann::json &settings, bool plainToggle);

	// The real device-snapshot provider: a type-only enumeration of the dshow camera input's
	// video_device_id list (present set). Mirrors the EnumerateSourceVariants device-list walk, minus
	// the resolution/fps cascade. Never creates or touches a live source.
	static DeviceSnapshot enumerateVideoCaptureDevices();

	// True only for source types whose engine holds an exclusive HW capture device.
	bool sourceHoldsExclusiveDevice(const SourceRec &rec);
	// Balanced raise/lower of the single showing ref this helper holds for the source.
	void applyDeviceShowing(SourceRec &rec, bool shouldShow);
	// Phase 2: the single place the showing precedence lives. The device should show iff the source
	// is not disabled AND not (release-when-idle while idle). Precedence: disabled > releaseWhenIdle.
	static bool deviceShouldShow(const SourceRec &rec, bool idle);

	// Pull the engine slot snapshot into the recs; fire senderNameResolved on transitions.
	void refreshFromEngine();

	void emitEvent(const std::string &name, nlohmann::json data); // adds instanceId+ts (not fleet)

	// Media transport plumbing (contract 1.1.0). Signals are the edge trigger;
	// the 1s pollTick diff covers the signal-less transitions (opening/buffering).
	void connectMediaSignals(SourceRec &rec);    // no-op for non-CONTROLLABLE_MEDIA sources
	void disconnectMediaSignals(SourceRec &rec); // idempotent; MUST precede the source release
	void evalMediaState(SourceRec &rec);         // main thread: diff-emit mediaChanged

	std::string instanceState(); // contract InstanceState

	nlohmann::json sourceSummary(const SourceRec &rec, bool forceNullSender = false) const;
	nlohmann::json sourceDetail(const SourceRec &rec) const;

	// Contract FilterInfo for one chain entry; `index` is the rec.filters vector position
	// (vector order IS apply order -- index 0 applies first; see handleReorderFilter).
	nlohmann::json filterInfoJson(const FilterRec &f, int index) const;

	// Per-verb handlers (params pre-checked for object-ness by dispatch).
	nlohmann::json handleGetVersion(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleGetStatus(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleListSources(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleListSourceTypes(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleCreateSource(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleRemoveSource(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetSourceProperties(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleListSourceProperties(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleEnumerateSourceVariants(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleListAvailableFilters(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleAddFilter(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleRemoveFilter(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetFilterProperties(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleListFilterProperties(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleListFilters(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetFilterEnabled(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleReorderFilter(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetFilterName(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleListAudioDevices(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetAudioOutputDevice(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleGetSourceAudio(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetSourceAudio(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetSourceFormat(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleStartBroadcast(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleStopBroadcast(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSetSourceIdleMode(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleGetMediaStatus(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleControlMedia(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSeekMedia(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleInvokeSourceButton(const nlohmann::json &id, const nlohmann::json &params);

	// Lifecycle.
	nlohmann::json handleShutdown(const nlohmann::json &id, const nlohmann::json &params);

	// Item 05: standalone profile verbs (additive; reject when owner-id is present).
	nlohmann::json handleListProfiles(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleLoadProfile(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleSaveProfile(const nlohmann::json &id, const nlohmann::json &params);
	nlohmann::json handleDeleteProfile(const nlohmann::json &id, const nlohmann::json &params);

	// Item 05: build the canonical profile JSON (the SAVE side) from the live recs_ -- a SUPERSET of
	// SourceFactory's createFromConfigJson document shape (so it round-trips through the load
	// primitive): document-level profileName/fpsTier/audioOutputDevice/window/port + a sources[]
	// where each entry carries id(=wireType)/name/format/externalId/settings(engine, libobs-defaults
	// stripped)/gain/muted/balance/syncOffsetMs(MILLISECONDS)/filters[]. `profileName` and `fpsTier`
	// are passed in (the verb supplies the name; the running tier comes from identity_).
	nlohmann::json snapshotProfileJson(const std::string &profileName, int fpsTier) const;

	// Item 05: the LOAD side -- the strict-order teardown + rebuild for a profile load, with the
	// live re-tier folded in. `profileJson` is the whole profile document (its sources[] feed
	// createFromConfigJson; its filters[] are replayed post-adopt). `targetFpsNum` > 0 AND different
	// from the running tier triggers the live re-tier (the obs_reset_video step runs BETWEEN the GPU
	// teardown and the source rebuild). Returns "" on success or a human-readable error string.
	std::string loadProfileSources(const nlohmann::json &profileJson, int targetFpsNum);

	// Item 05: a re-entrant source release that mirrors releaseAllSources()'s per-rec order (audio
	// detach -> engine detach -> disconnectMediaSignals -> filter remove+release -> dec_showing+
	// release source) but WITHOUT the one-shot released_ latch -- so it can run on every profile load
	// while the destructor's releaseAllSources() still works at shutdown. Clears recs_.
	void releaseAllSourcesForReload();

	SpoutSenderEngine *engine_ = nullptr; // borrowed; outlives this object's active use
	AudioMixEngine *audio_ = nullptr;     // borrowed; may be null (audio-less harness contexts)
	InstanceIdentity identity_;
	std::function<void(const std::string &, const nlohmann::json &)> sink_;
	std::function<bool(int fpsNum)> retierHandler_;                   // item 05: live obs_reset_video
	std::function<nlohmann::json()> windowLayoutProvider_;            // item 05: read window geometry
	std::function<void(const nlohmann::json &)> windowLayoutApplier_; // item 05: apply window geometry
	bool stopping_ = false;     // a Shutdown has been accepted (idempotent re-ack gate)
	bool committedDrain_ = true; // the drain mode the FIRST Shutdown committed (re-acks echo it)
	std::vector<SourceRec> recs_; // insertion order (boot order, then creation order)
	int nextSourceNum_ = 1;
	bool released_ = false;

	// Device-arrival self-heal state (main thread). deviceSnapshotProvider_ unset => real enumeration.
	// deviceSnapshot_ is the last-seen present device set; haveDeviceSnapshot_ gates the baseline (the
	// first snapshot emits no devicesChanged -- it only establishes the reference set).
	std::function<DeviceSnapshot()> deviceSnapshotProvider_;
	DeviceSnapshot deviceSnapshot_;
	bool haveDeviceSnapshot_ = false;
};

} // namespace moxrelay
