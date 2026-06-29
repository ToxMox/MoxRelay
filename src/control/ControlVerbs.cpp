// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ControlVerbs implementation. Wire semantics per docs/control-api.asyncapi.yaml; every method
// here runs on the Qt main thread (see the header threading contract).

#include "ControlVerbs.hpp"

#include "app/ProfileStore.hpp"
#include "audio/AudioMixEngine.hpp"
#include "control/AudioDevices.hpp"
#include "control/TypeVocabulary.hpp"
#include "spout/SpoutSenderEngine.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <unordered_map>

namespace moxrelay {

namespace {

using nlohmann::json;

// Application error codes (contract error table).
constexpr int kErrSourceNotFound = 1001;
constexpr int kErrFilterNotFound = 1002;
constexpr int kErrSourceTypeUnavailable = 1003;
constexpr int kErrFilterTypeUnavailable = 1004;
// 1005 on the SOURCE path stays unused (async sources settle after the reply; failures surface
// via status lastSendOk/size); SetAudioOutputDevice's synchronous open probe DOES report it.
constexpr int kErrDeviceBusy = 1005;
constexpr int kErrInvalidPropertyValue = 1006;
constexpr int kErrNotImplemented = 1007;
// 1008 NotReady is contract-defined; unobservable here (the server starts serving only after
// engine bring-up), so no handler emits it in this build.
constexpr int kErrSourceCreateFailed = 1010;
constexpr int kErrBroadcastStateConflict = 1011; // SetSourceFormat re-attach failure
constexpr int kErrMediaNotSupported = 1012;
// Item 05: the profile verbs (ListProfiles/LoadProfile/SaveProfile/DeleteProfile) are STANDALONE
// ONLY -- they reject when an owner-id is present (managed/helper mode). A clear invalid-state code
// (NOT kErrMethodNotFound, which would read as "verb absent"): the verb EXISTS but is unavailable in
// this mode. 1013 continues the application error table.
constexpr int kErrProfilesUnavailable = 1013;
// Item 05: a profile that names a name the store rejects (path-escaping / empty), or a load of a
// profile that is missing / unreadable / not valid JSON.
constexpr int kErrProfileInvalid = 1014;
constexpr int kErrProfileNotFound = 1015;

constexpr int kErrInvalidParams = -32602;
constexpr int kErrMethodNotFound = -32601;

int64_t nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

json ok(const json &id, json result)
{
	return json{{"id", id}, {"result", std::move(result)}};
}

json err(const json &id, int code, const std::string &message, json data = nullptr)
{
	json e = {{"code", code}, {"message", message}};
	if (!data.is_null())
		e["data"] = std::move(data);
	return json{{"id", id}, {"error", std::move(e)}};
}

// Settings (engine side) -> wire json object, through the overlay.
json engineSettingsToWire(obs_source_t *src, const std::string &wireType)
{
	json out = json::object();
	if (obs_data_t *settings = obs_source_get_settings(src)) {
		if (const char *text = obs_data_get_json_with_defaults(settings)) {
			json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
			if (parsed.is_object())
				out = TypeVocabulary::settingsEngineToWire(wireType, parsed);
		}
		obs_data_release(settings);
	}
	return out;
}

// Item 05: the ENGINE settings with libobs defaults STRIPPED -- for a profile snapshot. obs_data_get_json
// (WITHOUT defaults, unlike engineSettingsToWire's _with_defaults) serializes only EXPLICITLY-SET items,
// so a libobs-default that changes in a future bump is never frozen into an old profile. NO TypeVocabulary
// overlay: SourceFactory::createFromConfigJson feeds a profile entry's "settings" VERBATIM to
// obs_source_create_private as ENGINE settings (no wire->engine step on the load path), so the saved
// settings must be raw engine settings. Camera custom-mode note (decision 5): the seeded resolution
// (seedCameraResolutionForCustomMode) is written into the source as an explicit setting, so it is part of
// the source's actual rendered state and persists -- which is exactly what the round-trip contract asserts.
json engineSettingsStripped(obs_source_t *src)
{
	json out = json::object();
	if (obs_data_t *settings = obs_source_get_settings(src)) {
		if (const char *text = obs_data_get_json(settings)) {
			json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
			if (parsed.is_object())
				out = std::move(parsed);
		}
		obs_data_release(settings);
	}
	return out;
}

// Item 05: the WIRE settings (TypeVocabulary overlay applied) with libobs defaults STRIPPED -- for a
// profile's persisted FILTER settings. Filters are replayed on load through handleAddFilter, which
// runs settingsWireToEngine() on the incoming "settings", so the saved filter settings must be WIRE
// settings (unlike a SOURCE entry's "settings", which createFromConfigJson feeds VERBATIM as engine
// settings). Defaults stripped: obs_data_get_json (no defaults) -> overlay, so a libobs-default bump
// is never frozen into an old profile.
json engineSettingsToWireStripped(obs_source_t *src, const std::string &wireType)
{
	json out = json::object();
	if (obs_data_t *settings = obs_source_get_settings(src)) {
		if (const char *text = obs_data_get_json(settings)) {
			json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
			if (parsed.is_object())
				out = TypeVocabulary::settingsEngineToWire(wireType, parsed);
		}
		obs_data_release(settings);
	}
	return out;
}

// Camera dependent-settings seed. The capture engine resolves a camera's FPS and video-format
// option lists from the chosen resolution; while the resolution-mode setting is "device default"
// no resolution is stored, so a bare flip to custom mode leaves the dependent lists at their
// unresolved floor until a resolution is chosen. The engine's own dialog flow re-seeds from a
// remembered resolution (persisted per scene there); our runtime sources have no such memory, so
// when an update flips a camera to custom mode WITHOUT submitting a resolution and none is
// stored, seed one into the SAME atomic patch: the live delivery size when the device is
// capturing, else the head of the device's advertised resolution list (what a user would click
// first). The seed is an ordinary stored setting -- discoverable via ListSourceProperties, never
// echoed in `applied` (the contract echoes submitted keys only), exactly like the dependent keys
// the engine itself materializes.
void seedCameraResolutionForCustomMode(obs_source_t *src, const std::string &wireType, json &engineSettings)
{
	if (wireType != "camera")
		return;
	const auto rt = engineSettings.find("res_type");
	if (rt == engineSettings.end() || !rt->is_number_integer() || rt->get<int>() != 1)
		return; // not a flip to custom mode
	if (engineSettings.contains("resolution"))
		return; // caller submitted one -- theirs wins
	bool stored = false;
	if (obs_data_t *settings = obs_source_get_settings(src)) {
		const char *res = obs_data_get_string(settings, "resolution");
		stored = res && *res;
		obs_data_release(settings);
	}
	if (stored)
		return;
	std::string seed;
	const uint32_t cx = obs_source_get_width(src);
	const uint32_t cy = obs_source_get_height(src);
	if (cx > 0 && cy > 0) {
		seed = std::to_string(cx) + "x" + std::to_string(cy);
	} else if (obs_properties_t *props = obs_source_properties(src)) {
		if (obs_property_t *p = obs_properties_get(props, "resolution")) {
			if (obs_property_list_item_count(p) > 0) {
				if (const char *first = obs_property_list_item_string(p, 0))
					seed = first;
			}
		}
		obs_properties_destroy(props);
	}
	if (!seed.empty())
		engineSettings["resolution"] = seed;
}

// The set of settings keys a source/filter accepts: descriptor names + currently-stored keys
// (some accepted settings are not surfaced as properties). Wire-side names.
std::set<std::string> acceptedWireKeys(obs_source_t *src, const std::string &wireType)
{
	std::set<std::string> keys;
	if (obs_properties_t *props = obs_source_properties(src)) {
		const json descriptors = TypeVocabulary::describeProperties(props, wireType);
		for (const auto &d : descriptors)
			keys.insert(d.at("name").get<std::string>());
		obs_properties_destroy(props);
	}
	const json current = engineSettingsToWire(src, wireType);
	for (auto it = current.begin(); it != current.end(); ++it)
		keys.insert(it.key());
	return keys;
}

// 1012 keying: the engine's own capability bit. Today only ffmpeg_source (the `media` type)
// carries OBS_SOURCE_CONTROLLABLE_MEDIA; libobs gates every obs_source_media_* call on exactly
// this flag, so it is the honest "has a media pipeline" predicate (never a type-name compare).
bool isMediaControllable(obs_source_t *src)
{
	return src && (obs_source_get_output_flags(src) & OBS_SOURCE_CONTROLLABLE_MEDIA) != 0;
}

// obs_media_state -> contract MediaState (product names; the full 8-state set).
const char *mediaStateWire(enum obs_media_state state)
{
	switch (state) {
	case OBS_MEDIA_STATE_PLAYING:
		return "playing";
	case OBS_MEDIA_STATE_OPENING:
		return "opening";
	case OBS_MEDIA_STATE_BUFFERING:
		return "buffering";
	case OBS_MEDIA_STATE_PAUSED:
		return "paused";
	case OBS_MEDIA_STATE_STOPPED:
		return "stopped";
	case OBS_MEDIA_STATE_ENDED:
		return "ended";
	case OBS_MEDIA_STATE_ERROR:
		return "error";
	case OBS_MEDIA_STATE_NONE:
	default:
		return "none";
	}
}

// Per-source context bound to the obs media-signal connections. Heap-owned by the SourceRec
// (mediaSignalCtx); freed only AFTER signal_handler_disconnect, which the handler mutex makes
// safe against in-flight callbacks. Carries the sourceId by value -- the thunk never touches
// registry state (it fires on the video/playback threads).
struct MediaSignalCtx {
	ControlVerbs *owner;
	std::string sourceId;
};

void mediaSignalThunk(void *param, calldata_t *)
{
	auto *ctx = static_cast<MediaSignalCtx *>(param);
	ctx->owner->notifyMediaSignal(ctx->sourceId);
}

// The six media signals worth observing: the four action signals fire from the video tick's
// action processing; started/ended come from the source implementation (playback thread).
// media_next/media_previous are playlist-only -- nothing in the v1 surface emits them.
constexpr const char *kMediaSignals[] = {"media_play", "media_pause",   "media_restart",
					 "media_stopped", "media_started", "media_ended"};

} // namespace

ControlVerbs::ControlVerbs(SpoutSenderEngine *engine, AudioMixEngine *audio, InstanceIdentity identity)
	: engine_(engine),
	  audio_(audio),
	  identity_(std::move(identity))
{
}

ControlVerbs::~ControlVerbs()
{
	releaseAllSources();
}

void ControlVerbs::adoptBootSources(std::vector<CreatedSource> sources, bool attachedInOrder)
{
	// When the caller already attached (worker boot), the engine snapshot preserves attach
	// order, which is the same vector order -- zip slot ids by index.
	std::vector<SenderSlotInfo> infos;
	if (attachedInOrder && engine_)
		infos = engine_->slotInfos();

	for (size_t i = 0; i < sources.size(); ++i) {
		CreatedSource &cs = sources[i];
		if (!cs.source)
			continue;
		SourceRec rec;
		rec.sourceId = "src_" + std::to_string(nextSourceNum_++);
		rec.source = cs.source;
		rec.displayName = cs.name;
		rec.format = cs.format; // factory-validated (srgb87 | linear87 | fp16)
		rec.externalId = cs.externalId; // client-supplied stable id from the boot entry ("" = none)
		rec.spawnConfigured = true;
		// Mirror the factory's showing decision (managed boot configs are fully configured, so this is
		// normally true; an unset target would adopt inert). Keeps teardown dec_showing balanced.
		rec.deviceShowing = cs.showing;
		const char *unversioned = obs_source_get_unversioned_id(cs.source);
		const auto wire = TypeVocabulary::sourceWireName(unversioned ? unversioned : "");
		if (wire) {
			rec.wireType = *wire;
		} else {
			// A spawn config used a type outside the contract vocabulary. Surface it as
			// the generic media type rather than leaking an internal id onto the wire.
			rec.wireType = "media";
			std::fprintf(stderr,
				     "[control] WARNING: source '%s' has no contract type mapping; "
				     "reported as 'media'\n",
				     cs.name.c_str());
		}
		if (attachedInOrder && i < infos.size())
			rec.slotId = infos[i].slotId;
		recs_.push_back(std::move(rec));
		connectMediaSignals(recs_.back());
		// Warm the property cache for cascade-audited types (camera) -- same freeze-fix reason as
		// handleCreateSource; an adopted (boot/profile-load) camera must skip enum on its first toggle.
		if (sourceCascadeKeysAudited(recs_.back().wireType))
			refreshSourcePropsCache(recs_.back());
		// Boot-attached sources (worker path) pair their audio tap here, where the wire
		// sourceId exists -- the same attach edge the runtime verbs pair.
		if (audio_ && recs_.back().slotId >= 0)
			audio_->attachSource(recs_.back().source, recs_.back().sourceId);
	}
	refreshFromEngine();
}

void ControlVerbs::setEventSink(std::function<void(const std::string &, const nlohmann::json &)> sink)
{
	sink_ = std::move(sink);
}

void ControlVerbs::setRetierHandler(std::function<bool(int fpsNum)> handler)
{
	retierHandler_ = std::move(handler);
}

void ControlVerbs::setWindowLayoutSeams(std::function<nlohmann::json()> provider,
					std::function<void(const nlohmann::json &)> applier)
{
	windowLayoutProvider_ = std::move(provider);
	windowLayoutApplier_ = std::move(applier);
}

std::vector<std::string> ControlVerbs::subscribableEvents() const
{
	return {"status",           "sourceAdded",       "sourceRemoved",
		"filterAdded",       "filterRemoved",     "filterChanged",
		"senderNameResolved", "broadcastChanged",  "propertyChanged",
		"mediaChanged",      "audioChanged",      "audioLevels",
		"sourceIdleModeChanged"};
}

void ControlVerbs::emitEvent(const std::string &name, nlohmann::json data)
{
	if (!sink_)
		return;
	data["instanceId"] = identity_.instanceId;
	data["ts"] = nowMs();
	sink_(name, data);
}

ControlVerbs::SourceRec *ControlVerbs::findSource(const std::string &sourceId)
{
	for (auto &rec : recs_) {
		if (rec.sourceId == sourceId)
			return &rec;
	}
	return nullptr;
}

ControlVerbs::FilterRec *ControlVerbs::findFilter(SourceRec &rec, const std::string &filterId)
{
	for (auto &f : rec.filters) {
		if (f.filterId == filterId)
			return &f;
	}
	return nullptr;
}

void ControlVerbs::refreshFromEngine()
{
	if (!engine_)
		return;
	const auto infos = engine_->slotInfos();
	std::unordered_map<int, const SenderSlotInfo *> bySlot;
	for (const auto &info : infos)
		bySlot[info.slotId] = &info;

	for (auto &rec : recs_) {
		if (rec.slotId < 0)
			continue;
		const auto it = bySlot.find(rec.slotId);
		if (it == bySlot.end())
			continue; // slot vanished (engine stopped); state stays last-known
		const SenderSlotInfo &info = *it->second;
		rec.sends = info.sends;
		rec.lastSendOk = info.lastSendOk;
		rec.initFailed = info.initFailed;
		if (!info.actualName.empty() &&
		    (!rec.senderAnnounced || rec.senderName != info.actualName)) {
			rec.senderName = info.actualName;
			rec.senderAnnounced = true;
			emitEvent("senderNameResolved",
				  {{"sourceId", rec.sourceId}, {"senderName", rec.senderName}});
		}
	}
}

void ControlVerbs::pollTick()
{
	refreshFromEngine();
	// Media-state completeness net: the obs signals cover the action + started/ended edges, but
	// opening/buffering transitions are signal-less (set_media_state internal) -- diff them here
	// at the tick cadence. evalMediaState only emits on actual change, so this never duplicates
	// a signal-driven emission.
	for (auto &rec : recs_)
		evalMediaState(rec);
}

// ---------------------------------------------------------------------------------------------
// Media transport plumbing (contract 1.1.0)
// ---------------------------------------------------------------------------------------------

void ControlVerbs::connectMediaSignals(SourceRec &rec)
{
	if (!rec.source || rec.mediaSignalCtx || !isMediaControllable(rec.source))
		return;
	rec.lastMediaState = mediaStateWire(obs_source_media_get_state(rec.source));
	auto *ctx = new MediaSignalCtx{this, rec.sourceId};
	rec.mediaSignalCtx = ctx;
	signal_handler_t *sh = obs_source_get_signal_handler(rec.source);
	for (const char *sig : kMediaSignals)
		signal_handler_connect(sh, sig, mediaSignalThunk, ctx);
}

void ControlVerbs::disconnectMediaSignals(SourceRec &rec)
{
	if (!rec.mediaSignalCtx)
		return;
	auto *ctx = static_cast<MediaSignalCtx *>(rec.mediaSignalCtx);
	if (rec.source) {
		signal_handler_t *sh = obs_source_get_signal_handler(rec.source);
		for (const char *sig : kMediaSignals)
			signal_handler_disconnect(sh, sig, mediaSignalThunk, ctx);
	}
	// Safe to free now: disconnect serializes against in-flight callbacks (handler mutex).
	delete ctx;
	rec.mediaSignalCtx = nullptr;
}

void ControlVerbs::notifyMediaSignal(const std::string &sourceId)
{
	// Arbitrary-thread entry (video tick / playback thread). Hop to the Qt main thread where
	// the registry lives; a lambda landing after the source is gone no-ops via findSource, and
	// one landing after event-loop exit is never executed (teardown runs post-exec()).
	QMetaObject::invokeMethod(
		QCoreApplication::instance(),
		[this, sourceId] {
			if (SourceRec *rec = findSource(sourceId))
				evalMediaState(*rec);
		},
		Qt::QueuedConnection);
}

void ControlVerbs::evalMediaState(SourceRec &rec)
{
	if (!rec.source || !isMediaControllable(rec.source))
		return;
	const char *state = mediaStateWire(obs_source_media_get_state(rec.source));
	if (rec.lastMediaState == state)
		return; // no transition -- this is also what keeps a looping wrap silent
	rec.lastMediaState = state;
	emitEvent("mediaChanged", {{"sourceId", rec.sourceId},
				   {"state", state},
				   {"positionMs", obs_source_media_get_time(rec.source)}});
}

std::string ControlVerbs::instanceState()
{
	bool anyAttached = false;
	for (const auto &rec : recs_) {
		if (rec.initFailed)
			return "error";
		if (rec.slotId >= 0)
			anyAttached = true;
	}
	return anyAttached ? "broadcasting" : "ready";
}

bool ControlVerbs::anyBroadcasting()
{
	for (const auto &rec : recs_) {
		if (rec.slotId >= 0)
			return true;
	}
	return false;
}

json ControlVerbs::sourceSummary(const SourceRec &rec, bool forceNullSender) const
{
	json senderName;
	if (!forceNullSender && !rec.senderName.empty() && rec.slotId >= 0)
		senderName = rec.senderName;
	// externalId is echoed verbatim; null on the wire when the source carries none (legacy/absent).
	json externalId;
	if (!rec.externalId.empty())
		externalId = rec.externalId;
	return json{{"sourceId", rec.sourceId},
		    {"type", rec.wireType},
		    {"displayName", rec.displayName},
		    {"senderName", senderName},
		    {"broadcasting", rec.slotId >= 0},
		    {"format", rec.format},
		    {"externalId", externalId}};
}

json ControlVerbs::sourceDetail(const SourceRec &rec) const
{
	json detail = sourceSummary(rec);
	detail["sends"] = rec.sends;
	detail["lastSendOk"] = rec.lastSendOk;
	detail["width"] = rec.source ? obs_source_get_base_width(rec.source) : 0;
	detail["height"] = rec.source ? obs_source_get_base_height(rec.source) : 0;
	return detail;
}

nlohmann::json ControlVerbs::statusEventData()
{
	refreshFromEngine();
	json sources = json::array();
	for (const auto &rec : recs_) {
		json senderName;
		if (!rec.senderName.empty() && rec.slotId >= 0)
			senderName = rec.senderName;
		sources.push_back({{"sourceId", rec.sourceId},
				   {"senderName", senderName},
				   {"broadcasting", rec.slotId >= 0},
				   {"lastSendOk", rec.lastSendOk}});
	}
	return json{{"instanceId", identity_.instanceId},
		    {"ts", nowMs()},
		    {"state", instanceState()},
		    {"fps", obs_get_active_fps()},
		    {"avgFrameMs", double(obs_get_average_frame_time_ns()) / 1e6},
		    {"totalFrames", obs_get_total_frames()},
		    {"laggedFrames", obs_get_lagged_frames()},
		    {"sources", std::move(sources)}};
}

nlohmann::json ControlVerbs::audioLevelsEventData()
{
	AudioEngineStats st;
	if (audio_)
		st = audio_->stats();

	// "Audio-contributing" = the source TYPE produces audio (the engine output flag); a
	// broadcasting video-only source carries an inert tap and is not a meter row.
	std::set<std::string> audioCapable;
	for (const auto &rec : recs_) {
		if (rec.source && (obs_source_get_output_flags(rec.source) & OBS_SOURCE_AUDIO))
			audioCapable.insert(rec.sourceId);
	}

	json sources = json::array();
	for (const auto &s : st.sources) {
		if (!audioCapable.count(s.sourceId))
			continue;
		sources.push_back({{"sourceId", s.sourceId}, {"peak", s.peak}, {"rms", s.rms}});
	}
	return json{{"instanceId", identity_.instanceId},
		    {"ts", nowMs()},
		    {"master", {{"peak", st.masterPeak}, {"rms", st.masterRms}, {"clipped", st.clipped}}},
		    {"sources", std::move(sources)}};
}

nlohmann::json ControlVerbs::shuttingDownEventData(const std::string &reason)
{
	return json{{"instanceId", identity_.instanceId},
		    {"ts", nowMs()},
		    {"reason", reason},
		    {"graceMs", nullptr}};
}

void ControlVerbs::releaseAllSources()
{
	if (released_)
		return;
	released_ = true;
	for (auto &rec : recs_) {
		// Audio tap removal precedes BOTH the sender detach and the source release below
		// (the media-signal ordering discipline; idempotent on never-attached sources).
		if (audio_ && rec.source)
			audio_->detachSource(rec.source);
		if (rec.slotId >= 0 && engine_) {
			engine_->detach(rec.slotId);
			rec.slotId = -1;
		}
		disconnectMediaSignals(rec);
		for (auto &f : rec.filters) {
			if (rec.source && f.filter)
				obs_source_filter_remove(rec.source, f.filter);
			if (f.filter)
				obs_source_release(f.filter);
			f.filter = nullptr;
		}
		rec.filters.clear();
		if (rec.source) {
			if (rec.deviceShowing)
				obs_source_dec_showing(rec.source);
			obs_source_release(rec.source);
			rec.source = nullptr;
		}
	}
	recs_.clear();
}

bool ControlVerbs::guiToggleBroadcast(bool start)
{
	dispatch(json(0), start ? "StartBroadcast" : "StopBroadcast", json::object());
	return anyBroadcasting();
}

// ---------------------------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------------------------

nlohmann::json ControlVerbs::dispatch(const nlohmann::json &id, const std::string &method,
				      const nlohmann::json &params)
{
	if (!params.is_object())
		return err(id, kErrInvalidParams, "Invalid params: 'params' must be an object");

	if (method == "GetVersion")
		return handleGetVersion(id, params);
	if (method == "GetStatus")
		return handleGetStatus(id, params);
	if (method == "ListSources")
		return handleListSources(id, params);
	if (method == "ListSourceTypes")
		return handleListSourceTypes(id, params);
	if (method == "CreateSource")
		return handleCreateSource(id, params);
	if (method == "RemoveSource")
		return handleRemoveSource(id, params);
	if (method == "SetSourceProperties")
		return handleSetSourceProperties(id, params);
	if (method == "ListSourceProperties")
		return handleListSourceProperties(id, params);
	if (method == "EnumerateSourceVariants")
		return handleEnumerateSourceVariants(id, params);
	if (method == "ListAvailableFilters")
		return handleListAvailableFilters(id, params);
	if (method == "AddFilter")
		return handleAddFilter(id, params);
	if (method == "RemoveFilter")
		return handleRemoveFilter(id, params);
	if (method == "SetFilterProperties")
		return handleSetFilterProperties(id, params);
	if (method == "ListFilterProperties")
		return handleListFilterProperties(id, params);
	if (method == "ListFilters")
		return handleListFilters(id, params);
	if (method == "SetFilterEnabled")
		return handleSetFilterEnabled(id, params);
	if (method == "ReorderFilter")
		return handleReorderFilter(id, params);
	if (method == "SetFilterName")
		return handleSetFilterName(id, params);
	if (method == "SetSourceFormat")
		return handleSetSourceFormat(id, params);
	if (method == "ListAudioDevices")
		return handleListAudioDevices(id, params);
	if (method == "SetAudioOutputDevice")
		return handleSetAudioOutputDevice(id, params);
	if (method == "GetSourceAudio")
		return handleGetSourceAudio(id, params);
	if (method == "SetSourceAudio")
		return handleSetSourceAudio(id, params);
	if (method == "StartBroadcast")
		return handleStartBroadcast(id, params);
	if (method == "StopBroadcast")
		return handleStopBroadcast(id, params);
	if (method == "SetSourceIdleMode")
		return handleSetSourceIdleMode(id, params);
	if (method == "GetMediaStatus")
		return handleGetMediaStatus(id, params);
	if (method == "ControlMedia")
		return handleControlMedia(id, params);
	if (method == "SeekMedia")
		return handleSeekMedia(id, params);
	if (method == "InvokeSourceButton")
		return handleInvokeSourceButton(id, params);
	if (method == "Shutdown")
		return handleShutdown(id, params);
	if (method == "ListProfiles")
		return handleListProfiles(id, params);
	if (method == "LoadProfile")
		return handleLoadProfile(id, params);
	if (method == "SaveProfile")
		return handleSaveProfile(id, params);
	if (method == "DeleteProfile")
		return handleDeleteProfile(id, params);

	return err(id, kErrMethodNotFound, "Unknown method: " + method);
}

// ---------------------------------------------------------------------------------------------
// Verb handlers
// ---------------------------------------------------------------------------------------------

nlohmann::json ControlVerbs::handleGetVersion(const nlohmann::json &id, const nlohmann::json &)
{
	const std::set<std::string> registered = SourceFactory::registeredInputIds();

	json sourceTypes = json::array();
	for (const auto &e : TypeVocabulary::sourceTypes()) {
		if (registered.count(e.engineId))
			sourceTypes.push_back(e.wireName);
	}
	json filterTypes = json::array();
	for (const auto &e : TypeVocabulary::filterTypes()) {
		if (obs_source_get_display_name(e.engineId))
			filterTypes.push_back(e.wireName);
	}

	return ok(id, json{{"version", identity_.version},
			   {"apiVersion", 1},
			   {"instanceId", identity_.instanceId},
			   {"ownerId", identity_.ownerId},
			   {"fpsTier", identity_.fpsTier},
			   {"port", identity_.port},
			   {"capabilities",
			    {{"sourceTypes", std::move(sourceTypes)},
			     {"filterTypes", std::move(filterTypes)},
			     {"events", subscribableEvents()},
			     // True exactly when the routing verb is live: the audio engine is
			     // wired in every real construction (GUI, worker, selftest); only an
			     // audio-less harness reports false, and SetAudioOutputDevice answers
			     // 1007 there -- the flag and the verb can never disagree.
			     {"audioOutput", audio_ != nullptr}}}});
}

nlohmann::json ControlVerbs::handleGetStatus(const nlohmann::json &id, const nlohmann::json &)
{
	refreshFromEngine();
	json sources = json::array();
	for (const auto &rec : recs_)
		sources.push_back(sourceDetail(rec));
	// publishedSenderNames: the ACTUAL Spout sender names this instance has published, read
	// straight from the engine's slot table (actualName / lastSendOk). This carries
	// published-but-not-yet-live names -- a source whose first frame has gone out (lastSendOk)
	// appears here even before the per-source senderName resolves into sources[]. It is the
	// pre-live picker projection (the old discovery sources[].name list), surfaced live.
	json publishedSenderNames = json::array();
	if (engine_) {
		for (const auto &info : engine_->slotInfos()) {
			if (!info.actualName.empty() && info.lastSendOk)
				publishedSenderNames.push_back(info.actualName);
		}
	}
	return ok(id, json{{"state", instanceState()},
			   {"instanceId", identity_.instanceId},
			   {"port", identity_.port},
			   {"version", identity_.version},
			   {"fpsTier", identity_.fpsTier},
			   {"spoutPrefix", identity_.spoutPrefix},
			   {"audioOutputDevice", audio_ ? audio_->outputDevice() : "default"},
			   {"fps", obs_get_active_fps()},
			   {"avgFrameMs", double(obs_get_average_frame_time_ns()) / 1e6},
			   {"totalFrames", obs_get_total_frames()},
			   {"laggedFrames", obs_get_lagged_frames()},
			   {"publishedSenderNames", std::move(publishedSenderNames)},
			   {"sources", std::move(sources)}});
}

nlohmann::json ControlVerbs::handleListSources(const nlohmann::json &id, const nlohmann::json &)
{
	refreshFromEngine();
	json sources = json::array();
	for (const auto &rec : recs_)
		sources.push_back(sourceSummary(rec));
	return ok(id, json{{"sources", std::move(sources)}});
}

nlohmann::json ControlVerbs::handleListSourceTypes(const nlohmann::json &id, const nlohmann::json &)
{
	// The same registered-set intersection GetVersion's capabilities.sourceTypes uses (and the
	// same vocabulary order), enriched with the display label and per-type capability flags read
	// from the engine's type registration.
	const std::set<std::string> registered = SourceFactory::registeredInputIds();
	json sourceTypes = json::array();
	for (const auto &e : TypeVocabulary::sourceTypes()) {
		if (!registered.count(e.engineId))
			continue;
		const uint32_t flags = obs_get_source_output_flags(e.engineId);
		sourceTypes.push_back({{"type", e.wireName},
				       {"label", e.label},
				       {"hasVideo", (flags & OBS_SOURCE_VIDEO) != 0},
				       {"hasAudio", (flags & OBS_SOURCE_AUDIO) != 0},
				       {"hasMedia", (flags & OBS_SOURCE_CONTROLLABLE_MEDIA) != 0}});
	}
	return ok(id, json{{"sourceTypes", std::move(sourceTypes)}});
}

nlohmann::json ControlVerbs::handleCreateSource(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("type") || !params["type"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'type' must be a string");
	const std::string wireType = params["type"].get<std::string>();

	const auto engineId = TypeVocabulary::sourceEngineId(wireType);
	if (!engineId)
		return err(id, kErrSourceTypeUnavailable, "Unsupported source type: " + wireType,
			   {{"category", "source"},
			    {"detail", "supported types are listed in GetVersion capabilities.sourceTypes"}});
	if (!SourceFactory::registeredInputIds().count(*engineId))
		return err(id, kErrSourceTypeUnavailable,
			   "Source type not available in this build: " + wireType,
			   {{"category", "source"}});

	std::string displayName;
	if (params.contains("displayName")) {
		if (!params["displayName"].is_string())
			return err(id, kErrInvalidParams, "Invalid params: 'displayName' must be a string");
		displayName = params["displayName"].get<std::string>();
	}
	if (displayName.empty())
		displayName = wireType + "_" + std::to_string(nextSourceNum_);

	json wireSettings = json::object();
	if (params.contains("settings")) {
		if (!params["settings"].is_object())
			return err(id, kErrInvalidParams, "Invalid params: 'settings' must be an object");
		wireSettings = params["settings"];
	}
	bool startBroadcast = false;
	if (params.contains("startBroadcast")) {
		if (!params["startBroadcast"].is_boolean())
			return err(id, kErrInvalidParams, "Invalid params: 'startBroadcast' must be a boolean");
		startBroadcast = params["startBroadcast"].get<bool>();
	}
	std::string format = "srgb87";
	if (params.contains("format")) {
		if (!params["format"].is_string())
			return err(id, kErrInvalidParams, "Invalid params: 'format' must be a string");
		format = params["format"].get<std::string>();
		SenderFormat parsed;
		if (!SpoutSenderEngine::parseFormat(format, parsed))
			return err(id, kErrInvalidPropertyValue, "Unknown format value: " + format,
				   {{"category", "source"},
				    {"property", "format"},
				    {"detail", "valid values: srgb87, linear87, fp16"}});
	}

	// Optional audio seeds (gain / muted / balance) -- format-style param SIBLINGS, never
	// settings keys; the same validation + ceiling clamp as SetSourceAudio.
	float gain = 1.0f;
	bool muted = false;
	float balance = 0.5f;
	if (params.contains("gain")) {
		if (!params["gain"].is_number() || params["gain"].get<double>() < 0.0)
			return err(id, kErrInvalidParams, "Invalid params: 'gain' must be a number >= 0");
		const double v = params["gain"].get<double>();
		gain = float(v > 20.0 ? 20.0 : v);
	}
	if (params.contains("muted")) {
		if (!params["muted"].is_boolean())
			return err(id, kErrInvalidParams, "Invalid params: 'muted' must be a boolean");
		muted = params["muted"].get<bool>();
	}
	if (params.contains("balance")) {
		if (!params["balance"].is_number() || params["balance"].get<double>() < 0.0 ||
		    params["balance"].get<double>() > 1.0)
			return err(id, kErrInvalidParams,
				   "Invalid params: 'balance' must be a number in [0, 1]");
		balance = params["balance"].get<float>();
	}
	int syncOffsetMs = 0;
	if (params.contains("syncOffsetMs")) {
		if (!params["syncOffsetMs"].is_number_integer() || params["syncOffsetMs"].get<int64_t>() < 0)
			return err(id, kErrInvalidParams,
				   "Invalid params: 'syncOffsetMs' must be an integer >= 0");
		const int64_t v = params["syncOffsetMs"].get<int64_t>();
		syncOffsetMs = int(v > AudioMixEngine::kSyncOffsetMaxMs ? AudioMixEngine::kSyncOffsetMaxMs : v);
	}

	// Optional client-supplied stable id -- opaque to the helper: stored verbatim and echoed back,
	// never interpreted or validated for uniqueness. A null value is treated as absent.
	std::string externalId;
	if (params.contains("externalId") && !params["externalId"].is_null()) {
		if (!params["externalId"].is_string())
			return err(id, kErrInvalidParams, "Invalid params: 'externalId' must be a string");
		externalId = params["externalId"].get<std::string>();
	}

	// Create through the factory's proven config path (registration check, settings
	// ref-handling, display seeding) -- one source document.
	const json engineSettings = TypeVocabulary::settingsWireToEngine(wireType, wireSettings);
	const json doc = {{"port", identity_.port},
			  {"sources", json::array({json{{"id", *engineId},
							{"name", displayName},
							{"settings", engineSettings}}})}};
	SourceConfigResult cfg = SourceFactory::createFromConfigJson(doc.dump().c_str());
	if (!cfg.ok || cfg.sources.size() != 1)
		return err(id, kErrSourceCreateFailed, "Source creation failed: " + cfg.error,
			   {{"category", "source"}});

	SourceRec rec;
	rec.sourceId = "src_" + std::to_string(nextSourceNum_++);
	rec.source = cfg.sources.front().source;
	rec.wireType = wireType;
	rec.displayName = displayName;
	rec.format = format;
	rec.externalId = externalId;
	rec.spawnConfigured = false;
	// Mirror the factory's showing decision: a standalone-added camera/display/window with no target
	// is created INERT (no showing ref). It is activated later when the user commits a target
	// (handleSetSourceProperties). Keeping this in sync keeps removal/teardown dec_showing balanced.
	rec.deviceShowing = cfg.sources.front().showing;

	// Audio seeds apply BEFORE any attach, so the tap's gain envelope starts at the seeded
	// value and a seeded sync offset becomes the attach-time delay reserve (engine source
	// state; defaults match the engine's own, so this is a no-op when the params were absent).
	obs_source_set_volume(rec.source, gain);
	obs_source_set_muted(rec.source, muted);
	obs_source_set_balance_value(rec.source, balance);
	obs_source_set_sync_offset(rec.source, int64_t(syncOffsetMs) * 1000000);

	if (startBroadcast && engine_) {
		engine_->start(); // idempotent
		SenderFormat fmt = SenderFormat::Srgb87;
		SpoutSenderEngine::parseFormat(rec.format, fmt); // validated above
		rec.slotId = engine_->attach(rec.source, identity_.machine, identity_.port, rec.displayName, fmt);
		if (rec.slotId >= 0 && audio_)
			audio_->attachSource(rec.source, rec.sourceId); // the attach edge IS the audio edge
	}

	recs_.push_back(std::move(rec));
	connectMediaSignals(recs_.back());
	// Warm the property cache for cascade-audited types (camera) so the first plain toggle replays
	// the descriptors instead of running the ~1s dshow enumeration (the freeze fix). Other types stay
	// cold and enumerate live per apply (their enumeration is cheap and their cascade keys unaudited).
	if (sourceCascadeKeysAudited(recs_.back().wireType))
		refreshSourcePropsCache(recs_.back());
	const SourceRec &stored = recs_.back();

	emitEvent("sourceAdded", {{"source", sourceSummary(stored, /*forceNullSender=*/true)}});

	// The reply's senderName is ALWAYS null (contract): resolution is asynchronous and is
	// announced via senderNameResolved.
	return ok(id, sourceSummary(stored, /*forceNullSender=*/true));
}

nlohmann::json ControlVerbs::handleRemoveSource(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	// Audio tap removal precedes the sender detach and the source release (the media-signal
	// ordering discipline; no-op when the source never broadcast).
	if (audio_ && rec->source)
		audio_->detachSource(rec->source);
	if (rec->slotId >= 0 && engine_) {
		engine_->detach(rec->slotId);
		rec->slotId = -1;
	}
	disconnectMediaSignals(*rec);
	for (auto &f : rec->filters) {
		if (f.filter) {
			obs_source_filter_remove(rec->source, f.filter);
			obs_source_release(f.filter);
		}
	}
	rec->filters.clear();
	if (rec->source) {
		if (rec->deviceShowing)
			obs_source_dec_showing(rec->source);
		obs_source_release(rec->source);
		rec->source = nullptr;
	}
	for (auto it = recs_.begin(); it != recs_.end(); ++it) {
		if (it->sourceId == sourceId) {
			recs_.erase(it);
			break;
		}
	}

	emitEvent("sourceRemoved", {{"sourceId", sourceId}});
	return ok(id, json{{"removed", true}});
}

// --- Property-cache plumbing (camera dshow-enumeration freeze fix) ----------------------------
// The dshow camera enumeration (obs_source_properties -> GetDShowProperties -> EnumVideoDevices,
// one BindToObject per device) is the ~1s cost behind the settings-UI freeze. Per the measurement
// it runs TWICE per apply (key-validation + post-update echo). We run it ONCE per source and replay
// the result for plain toggles, re-enumerating ONLY when a cascade key changes the dependent option
// lists (resolution/fps/format) or an explicit ListSourceProperties re-fetch picks up a device
// hotplug. Everything stays on the Qt main thread -- this only removes redundant enumerations, it
// never moves any obs_data / obs_source access off-thread.

bool ControlVerbs::sourceCascadeKeysAudited(const std::string &wireType)
{
	// Only the dshow camera type has (a) the expensive enumeration we are eliminating and (b) a
	// cascade-key set we have AUDITED from the plugin (win-dshow's modified callbacks). Every other
	// type defaults to a fresh enumeration per apply: its enumeration is cheap, and we have NOT
	// audited its dependent-list cascade keys (e.g. ffmpeg_source's is_local_file toggles field
	// visibility), so trusting a cached echo there could go stale. This is the bible's
	// "default-to-enumerate for any unknown wireType" guardrail.
	return wireType == "camera";
}

bool ControlVerbs::isCascadeKey(const std::string &key)
{
	// The win-dshow setting keys that carry an obs_property_set_modified_callback -- i.e. whose change
	// MUTATES the visible/contained property set, so the descriptor cache must be re-enumerated.
	// Audited against the OBS Studio 32.1.2 source we build against (plugins/win-dshow/win-dshow.cpp,
	// tag 32.1.2): video_device_id (DeviceSelectionChanged) -> res_type (ResTypeChanged) -> resolution
	// (DeviceResolutionChanged) -> frame_interval (DeviceIntervalChanged) / video_format
	// (VideoFormatChanged), and use_custom_audio_device (CustomAudioClicked, which shows/hides
	// audio_device_id). color_space/color_range have NO modified callback in 32.1.2 but are kept as a
	// conservative belt-and-suspenders cascade (the bible's explicit choice). Plain bools without a
	// callback (flip_vertically, hw_decode, buffering, autorotation, deactivate_when_not_showing,
	// audio_output_mode, active) are deliberately EXCLUDED -- they are the plain-toggle fast path.
	static const std::set<std::string> kCascadeKeys = {
		"video_device_id", "res_type",     "resolution",  "frame_interval",
		"video_format",    "color_space",  "color_range", "use_custom_audio_device"};
	return kCascadeKeys.count(key) != 0;
}

void ControlVerbs::refreshSourcePropsCache(SourceRec &rec)
{
	// ONE enumeration feeds BOTH the descriptor echo and the accepted-key allowlist. Mirrors the
	// (now bypassed on the plain path) acceptedWireKeys() + describeProperties pair exactly, so the
	// cached values are byte-identical to what a live enumeration would have produced.
	json descriptors = json::array();
	std::set<std::string> keys;
	if (obs_properties_t *props = obs_source_properties(rec.source)) {
		descriptors = TypeVocabulary::describeProperties(props, rec.wireType);
		for (const auto &d : descriptors)
			keys.insert(d.at("name").get<std::string>());
		obs_properties_destroy(props);
	}

	// ECHO-CONTRACT guard (load-bearing): NEVER commit an empty enumeration. A later plain toggle
	// replays cachedDescriptors verbatim, and an empty `properties` array means "no cascade / keep
	// optimistic" to the standalone panel + the host reconciler -- never "rebuild from empty". So
	// if the enumeration yielded nothing (null props object, or describeProperties produced []), leave
	// the cache UNTOUCHED: a previously-good cache survives (not clobbered with empties) and a cold
	// cache stays cold (propsCached false) so the next apply takes the safe live-enumerate path.
	// cachedDescriptors and cachedAcceptedKeys are refreshed together or not at all (kept consistent),
	// which makes propsCached == true a guarantee that cachedDescriptors is non-empty.
	if (descriptors.empty())
		return;

	// Some accepted settings are stored but never surfaced as properties; include them so a resend
	// of a stored value still validates (mirrors acceptedWireKeys()'s current-settings union).
	const json current = engineSettingsToWire(rec.source, rec.wireType);
	for (auto it = current.begin(); it != current.end(); ++it)
		keys.insert(it.key());
	rec.cachedDescriptors = std::move(descriptors);
	rec.cachedAcceptedKeys = std::move(keys);
	rec.propsCached = true;
}

nlohmann::json ControlVerbs::handleSetSourceProperties(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("settings") || !params["settings"].is_object())
		return err(id, kErrInvalidParams, "Invalid params: 'settings' must be an object");
	const std::string sourceId = params["sourceId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	const json &settings = params["settings"];

	// Cascade decision (the freeze fix). For an AUDITED type (camera) with a warm cache, a submit
	// touching NONE of the cascade keys is a PLAIN TOGGLE: it cannot change the dependent option
	// lists, so we skip BOTH dshow enumerations and replay the cached descriptors/accepted keys.
	// A submit touching a cascade key (or a cold cache) takes the CASCADE path: ONE enumeration,
	// post-update, that refreshes the cache. Any non-audited type always enumerates live (its
	// enumeration is cheap; we have not audited its cascade keys) -- the conservative default.
	const bool audited = sourceCascadeKeysAudited(rec->wireType);
	bool cascadeKeyHit = false;
	for (auto it = settings.begin(); it != settings.end(); ++it) {
		if (isCascadeKey(it.key())) {
			cascadeKeyHit = true;
			break;
		}
	}
	const bool plainToggle = audited && rec->propsCached && !cascadeKeyHit;

	// Enumeration #1 (key validation). Plain toggle AND camera cascade both reuse the cached
	// accepted-key set (zero pre-update enumeration; the cascade keys are always present descriptors).
	// Only a non-audited type or a cold cache enumerates live here.
	const bool useCachedKeys = audited && rec->propsCached;
	std::set<std::string> liveAccepted;
	if (!useCachedKeys)
		liveAccepted = acceptedWireKeys(rec->source, rec->wireType);
	const std::set<std::string> &accepted = useCachedKeys ? rec->cachedAcceptedKeys : liveAccepted;
	for (auto it = settings.begin(); it != settings.end(); ++it) {
		if (!accepted.count(it.key()))
			return err(id, kErrInvalidPropertyValue, "Unknown settings key: " + it.key(),
				   {{"category", "source"}, {"sourceId", sourceId}, {"property", it.key()}});
	}

	json engineSettings = TypeVocabulary::settingsWireToEngine(rec->wireType, settings);
	seedCameraResolutionForCustomMode(rec->source, rec->wireType, engineSettings);
	if (obs_data_t *patch = obs_data_create_from_json(engineSettings.dump().c_str())) {
		// #4 No-op guard: an update can RESTART the underlying device (a win-dshow camera reopens on
		// every obs_source_update), so skip it entirely when every submitted key already equals the
		// source's current value. Reconcile re-applies that re-send the stored value therefore never
		// restart capture. Compared as JSON over the submitted keys only; a genuine change always
		// differs, so a real edit is never skipped.
		bool changed = true;
		if (obs_data_t *cur = obs_source_get_settings(rec->source)) {
			const char *curStr = obs_data_get_json(cur);
			const json curJson = json::parse(curStr ? curStr : "{}", nullptr, /*allow_exceptions=*/false);
			if (!curJson.is_discarded()) {
				changed = false;
				for (auto it = engineSettings.begin(); it != engineSettings.end(); ++it) {
					const json curVal = curJson.contains(it.key()) ? curJson[it.key()] : json();
					if (curVal != it.value()) { changed = true; break; }
				}
			}
			obs_data_release(cur);
		}
		if (changed) {
			obs_source_update(rec->source, patch);
			// A settings change on a media pipeline restarts playback, and the resume is
			// timestamp-smoothed -- tell the audio engine so a restart-induced dry-out is attributed
			// as transport, not starvation.
			if (audio_ && isMediaControllable(rec->source))
				audio_->noteSourceTransport(rec->source);
		}
		obs_data_release(patch);
	} else {
		return err(id, kErrInvalidPropertyValue, "Settings object could not be applied",
			   {{"category", "source"}, {"sourceId", sourceId}});
	}

	// Inert-until-configured: a source created without its target (standalone "Add Source") holds NO
	// showing ref. Once the user commits a valid target, raise the ref so it activates/captures --
	// the same state a configured-at-creation source has. Gated on the target NOW being set, so an
	// unrelated property edit never spuriously activates a still-unconfigured source. Only the gated
	// types (camera/display/window) can be inert, so non-target types never reach the raise.
	if (!rec->deviceShowing) {
		const SourceTypeEntry *typeEntry = nullptr;
		if (auto eng = TypeVocabulary::sourceEngineId(rec->wireType))
			typeEntry = TypeVocabulary::sourceTypeByEngineId(*eng);
		if (obs_data_t *cur = obs_source_get_settings(rec->source)) {
			if (TypeVocabulary::targetConfigured(typeEntry, cur)) {
				const bool idle = (rec->slotId < 0);
				applyDeviceShowing(*rec, deviceShouldShow(*rec, idle));
			}
			obs_data_release(cur);
		}
	}

	// Echo the submitted keys with their STORED values after the update.
	const json stored = engineSettingsToWire(rec->source, rec->wireType);
	json applied = json::object();
	for (auto it = settings.begin(); it != settings.end(); ++it)
		applied[it.key()] = stored.contains(it.key()) ? stored[it.key()] : it.value();

	// Enumeration #2 -- the echo `properties` (full descriptors AFTER the update so a camera's
	// freshly-cascaded resolution/fps/format lists+selections are reflected; same shape
	// ListSourceProperties returns). PLAIN TOGGLE: replay the CACHED descriptors -- ZERO enumeration.
	// CASCADE (audited): one enumeration that refreshes the cache, then echo it. Non-audited: live
	// enumeration as before (not cached). ECHO-CONTRACT (load-bearing): `properties` MUST stay
	// populated on EVERY path -- the standalone panel's cascade test (newProps != lastProperties) and
	// the host reconciler treat a populated array as "the current descriptor set"; an empty array
	// would be read as "rebuild from empty", never "no cascade". So a plain toggle replays the cache,
	// it never emits an empty array.
	json properties;
	if (plainToggle) {
		properties = rec->cachedDescriptors; // cache replay -- no enumeration
	} else if (audited) {
		refreshSourcePropsCache(*rec); // cascade: single enumeration refreshes descriptors + keys
		properties = rec->cachedDescriptors;
	} else {
		properties = json::array(); // non-audited: live enumeration (cheap; not cached)
		if (obs_properties_t *props = obs_source_properties(rec->source)) {
			properties = TypeVocabulary::describeProperties(props, rec->wireType);
			obs_properties_destroy(props);
		}
	}

	// Every successful apply -- wire or GUI, both ride this handler -- announces itself with
	// the SAME echo the reply carries (contract: delivered to all subscribed connections,
	// including the originator). `applied` stays submitted-keys-only; `settings`/`properties`
	// add the full post-apply state (additive; old clients ignore them).
	json echo = {{"sourceId", sourceId},
		     {"applied", std::move(applied)},
		     {"settings", stored},
		     {"properties", std::move(properties)}};
	emitEvent("propertyChanged", echo);

	return ok(id, std::move(echo));
}

nlohmann::json ControlVerbs::handleListSourceProperties(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	// ListSourceProperties is the explicit client/GUI re-fetch, and there is no device-hotplug signal
	// in this build, so it doubles as the cache-invalidation / device-rescan trigger. For a
	// cascade-audited type (camera) re-enumerate and REFRESH the cache: a freshly plugged/removed
	// device is reflected, and the next plain toggle replays the now-current descriptors. Reply from
	// the refreshed cache. Non-audited types enumerate live as before (not cached).
	json properties;
	if (sourceCascadeKeysAudited(rec->wireType)) {
		refreshSourcePropsCache(*rec);
		properties = rec->cachedDescriptors;
	} else {
		properties = json::array();
		if (obs_properties_t *props = obs_source_properties(rec->source)) {
			properties = TypeVocabulary::describeProperties(props, rec->wireType);
			obs_properties_destroy(props);
		}
	}
	return ok(id, json{{"properties", std::move(properties)},
			   {"settings", engineSettingsToWire(rec->source, rec->wireType)}});
}

// EnumerateSourceVariants -- GENERIC, type-only (no live source), NON-MUTATING discovery of a
// source TYPE's full option dependency tree. Drives the engine's PUBLIC properties API across a
// TRANSIENT type-only properties object built with obs_get_source_properties(engineId): it NEVER
// touches a live source (no obs_source_update). For the camera type the dependent lists
// (resolution -> fps/format) are walked by feeding synthetic settings through obs_property_modified
// (no graph runs -- DirectShow metadata only), so a capturing device is never disturbed. Every
// other type falls back to the flat current-descriptor snapshot (describeProperties).
nlohmann::json ControlVerbs::handleEnumerateSourceVariants(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("type") || !params["type"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'type' must be a string");
	const std::string wireType = params["type"].get<std::string>();

	const auto engineId = TypeVocabulary::sourceEngineId(wireType);
	if (!engineId)
		return err(id, kErrSourceTypeUnavailable, "Unsupported source type: " + wireType,
			   {{"category", "source"},
			    {"detail", "supported types are listed in GetVersion capabilities.sourceTypes"}});
	if (!SourceFactory::registeredInputIds().count(*engineId))
		return err(id, kErrSourceTypeUnavailable,
			   "Source type not available in this build: " + wireType,
			   {{"category", "source"}});

	// TRANSIENT, type-only props -- no source instance is ever created or touched. The caps cache
	// is pre-filled (camera: EnumVideoDevices metadata only).
	obs_properties_t *props = obs_get_source_properties(engineId->c_str());

	json variants = json::object();
	if (wireType == "camera") {
		// STATIC-CASCADE walk: device -> resolution -> {fps, format}. One props object, one
		// obs_data per device. Defensive against a null/empty device list (no crash, empty tree).
		json devices = json::array();
		obs_property_t *devList = props ? obs_properties_get(props, "video_device_id") : nullptr;
		const size_t devN = devList ? obs_property_list_item_count(devList) : 0;
		for (size_t di = 0; di < devN; ++di) {
			if (obs_property_list_item_disabled(devList, di))
				continue; // skip disabled "no devices"/placeholder rows
			const char *devId = obs_property_list_item_string(devList, di);
			const char *devName = obs_property_list_item_name(devList, di);
			if (!devId || !*devId)
				continue;

			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "video_device_id", devId);
			obs_data_set_int(s, "res_type", 1); // Custom -- Preferred early-returns past fps/format
			// Rebuild the resolution list for this device.
			obs_property_modified(devList, s);

			json resolutions = json::array();
			obs_property_t *resList = obs_properties_get(props, "resolution");
			const size_t resN = resList ? obs_property_list_item_count(resList) : 0;
			for (size_t ri = 0; ri < resN; ++ri) {
				if (obs_property_list_item_disabled(resList, ri))
					continue;
				const char *res = obs_property_list_item_string(resList, ri);
				if (!res || !*res)
					continue;

				obs_data_set_string(s, "resolution", res);
				// Rebuild fps (frame_interval) + format (video_format) for this resolution.
				obs_property_modified(obs_properties_get(props, "resolution"), s);

				json fpsArr = json::array();
				obs_property_t *fpsList = obs_properties_get(props, "frame_interval");
				const size_t fpsN = fpsList ? obs_property_list_item_count(fpsList) : 0;
				for (size_t fi = 0; fi < fpsN; ++fi) {
					if (obs_property_list_item_disabled(fpsList, fi))
						continue;
					const long long interval = obs_property_list_item_int(fpsList, fi);
					const char *flabel = obs_property_list_item_name(fpsList, fi);
					const double fps = interval > 0 ? 10000000.0 / (double)interval : 0.0;
					fpsArr.push_back({{"label", flabel ? flabel : ""},
							  {"value", fps},
							  {"interval", interval}});
				}

				json fmtArr = json::array();
				obs_property_t *fmtList = obs_properties_get(props, "video_format");
				const size_t fmtN = fmtList ? obs_property_list_item_count(fmtList) : 0;
				for (size_t mi = 0; mi < fmtN; ++mi) {
					if (obs_property_list_item_disabled(fmtList, mi))
						continue;
					const char *mlabel = obs_property_list_item_name(fmtList, mi);
					fmtArr.push_back({{"label", mlabel ? mlabel : ""},
							  {"value", obs_property_list_item_int(fmtList, mi)}});
				}

				resolutions.push_back({{"value", res},
						       {"fps", std::move(fpsArr)},
						       {"formats", std::move(fmtArr)}});
			}

			devices.push_back({{"id", devId},
					   {"name", devName ? devName : ""},
					   {"resolutions", std::move(resolutions)}});
			obs_data_release(s);
		}
		variants["devices"] = std::move(devices);
	} else {
		// DYNAMIC-LIST + FREE-FORM types: the generic flat descriptor snapshot (no live source).
		variants["properties"] = props ? TypeVocabulary::describeProperties(props, wireType)
						: json::array();
	}

	if (props)
		obs_properties_destroy(props);

	return ok(id, json{{"type", wireType}, {"engineId", *engineId}, {"variants", std::move(variants)}});
}

nlohmann::json ControlVerbs::handleListAvailableFilters(const nlohmann::json &id, const nlohmann::json &params)
{
	bool wantVideo = true;
	bool wantAudio = true;
	if (params.contains("sourceId")) {
		if (!params["sourceId"].is_string())
			return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
		const std::string sourceId = params["sourceId"].get<std::string>();
		SourceRec *rec = findSource(sourceId);
		if (!rec)
			return err(id, kErrSourceNotFound, "No such source: " + sourceId,
				   {{"category", "source"}, {"sourceId", sourceId}});
		const uint32_t flags = obs_source_get_output_flags(rec->source);
		wantVideo = (flags & OBS_SOURCE_VIDEO) != 0;
		wantAudio = (flags & OBS_SOURCE_AUDIO) != 0;
	}

	json filters = json::array();
	for (const auto &e : TypeVocabulary::filterTypes()) {
		if (!obs_source_get_display_name(e.engineId))
			continue; // not registered in this build
		const bool isVideo = std::string(e.kind) == "video";
		if ((isVideo && wantVideo) || (!isVideo && wantAudio))
			filters.push_back({{"filterType", e.wireName}, {"kind", e.kind}, {"label", e.label}});
	}
	return ok(id, json{{"filters", std::move(filters)}});
}

nlohmann::json ControlVerbs::filterInfoJson(const FilterRec &f, int index) const
{
	const FilterTypeEntry *entry = TypeVocabulary::filterByWireName(f.wireType);
	return json{{"filterId", f.filterId},
		    {"filterType", f.wireType},
		    {"name", f.name},
		    {"kind", entry ? entry->kind : "video"},
		    {"enabled", f.filter ? obs_source_enabled(f.filter) : false},
		    {"index", index}};
}

nlohmann::json ControlVerbs::handleAddFilter(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("filterType") || !params["filterType"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterType' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string filterType = params["filterType"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	const FilterTypeEntry *entry = TypeVocabulary::filterByWireName(filterType);
	if (!entry || !obs_source_get_display_name(entry->engineId))
		return err(id, kErrFilterTypeUnavailable, "Unknown filter type: " + filterType,
			   {{"category", "filter"},
			    {"detail", "available types are listed by ListAvailableFilters"}});

	const uint32_t srcFlags = obs_source_get_output_flags(rec->source);
	const bool isVideo = std::string(entry->kind) == "video";
	if ((isVideo && !(srcFlags & OBS_SOURCE_VIDEO)) || (!isVideo && !(srcFlags & OBS_SOURCE_AUDIO)))
		return err(id, kErrFilterTypeUnavailable,
			   "Filter type '" + filterType + "' is not applicable to source " + sourceId,
			   {{"category", "filter"}, {"sourceId", sourceId}});

	std::string name = entry->label;
	if (params.contains("name")) {
		if (!params["name"].is_string())
			return err(id, kErrInvalidParams, "Invalid params: 'name' must be a string");
		if (!params["name"].get<std::string>().empty())
			name = params["name"].get<std::string>();
	}

	obs_data_t *settings = nullptr;
	if (params.contains("settings")) {
		if (!params["settings"].is_object())
			return err(id, kErrInvalidParams, "Invalid params: 'settings' must be an object");
		const json engineSettings =
			TypeVocabulary::settingsWireToEngine(filterType, params["settings"]);
		settings = obs_data_create_from_json(engineSettings.dump().c_str());
	}

	obs_source_t *filter = obs_source_create_private(entry->engineId, name.c_str(), settings);
	if (settings)
		obs_data_release(settings);
	if (!filter)
		return err(id, kErrFilterTypeUnavailable, "Filter creation failed: " + filterType,
			   {{"category", "filter"}});

	obs_source_filter_add(rec->source, filter); // the chain takes its own ref; ours is kept

	FilterRec f;
	f.filterId = "flt_" + std::to_string(rec->nextFilterNum++);
	f.filter = filter;
	f.wireType = filterType;
	f.name = name;
	rec->filters.push_back(f);

	// obs_source_filter_add inserts at internal-array position 0 == LAST applied, so the new
	// filter's chain position is the end of the apply order -- the vector position it just took.
	emitEvent("filterAdded", {{"sourceId", sourceId},
				  {"filter", filterInfoJson(rec->filters.back(),
							    int(rec->filters.size()) - 1)}});

	return ok(id, json{{"filterId", f.filterId}, {"filterType", filterType}, {"name", name}});
}

nlohmann::json ControlVerbs::handleRemoveFilter(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("filterId") || !params["filterId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string filterId = params["filterId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	FilterRec *f = findFilter(*rec, filterId);
	if (!f)
		return err(id, kErrFilterNotFound, "No such filter: " + filterId,
			   {{"category", "filter"}, {"sourceId", sourceId}, {"filterId", filterId}});

	obs_source_filter_remove(rec->source, f->filter);
	obs_source_release(f->filter);
	for (auto it = rec->filters.begin(); it != rec->filters.end(); ++it) {
		if (it->filterId == filterId) {
			rec->filters.erase(it);
			break;
		}
	}
	emitEvent("filterRemoved", {{"sourceId", sourceId}, {"filterId", filterId}});
	return ok(id, json{{"removed", true}});
}

nlohmann::json ControlVerbs::handleSetFilterProperties(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("filterId") || !params["filterId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterId' must be a string");
	if (!params.contains("settings") || !params["settings"].is_object())
		return err(id, kErrInvalidParams, "Invalid params: 'settings' must be an object");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string filterId = params["filterId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	FilterRec *f = findFilter(*rec, filterId);
	if (!f)
		return err(id, kErrFilterNotFound, "No such filter: " + filterId,
			   {{"category", "filter"}, {"sourceId", sourceId}, {"filterId", filterId}});

	const json &settings = params["settings"];
	const std::set<std::string> accepted = acceptedWireKeys(f->filter, f->wireType);
	for (auto it = settings.begin(); it != settings.end(); ++it) {
		if (!accepted.count(it.key()))
			return err(id, kErrInvalidPropertyValue, "Unknown settings key: " + it.key(),
				   {{"category", "filter"},
				    {"sourceId", sourceId},
				    {"filterId", filterId},
				    {"property", it.key()}});
	}

	const json engineSettings = TypeVocabulary::settingsWireToEngine(f->wireType, settings);
	if (obs_data_t *patch = obs_data_create_from_json(engineSettings.dump().c_str())) {
		obs_source_update(f->filter, patch);
		obs_data_release(patch);
	}

	const json stored = engineSettingsToWire(f->filter, f->wireType);
	json applied = json::object();
	for (auto it = settings.begin(); it != settings.end(); ++it)
		applied[it.key()] = stored.contains(it.key()) ? stored[it.key()] : it.value();

	// Full property descriptors of the FILTER, captured after the update (same shape
	// ListFilterProperties returns). Additive context; old clients ignore it.
	json properties = json::array();
	if (obs_properties_t *props = obs_source_properties(f->filter)) {
		properties = TypeVocabulary::describeProperties(props, f->wireType);
		obs_properties_destroy(props);
	}

	// Filter-scoped propertyChanged: same apply announcement, filterId present (contract).
	// `applied` stays submitted-keys-only; `settings`/`properties` add the full post-apply
	// state (additive; old clients ignore them).
	json echo = {{"sourceId", sourceId},
		     {"filterId", filterId},
		     {"applied", std::move(applied)},
		     {"settings", stored},
		     {"properties", std::move(properties)}};
	emitEvent("propertyChanged", echo);

	echo.erase("sourceId"); // ok() reply is {filterId, applied, settings, properties}; sourceId is event-only (matches SetFilterPropertiesResult)
	return ok(id, std::move(echo));
}

nlohmann::json ControlVerbs::handleListFilterProperties(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("filterId") || !params["filterId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string filterId = params["filterId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	FilterRec *f = findFilter(*rec, filterId);
	if (!f)
		return err(id, kErrFilterNotFound, "No such filter: " + filterId,
			   {{"category", "filter"}, {"sourceId", sourceId}, {"filterId", filterId}});

	json properties = json::array();
	if (obs_properties_t *props = obs_source_properties(f->filter)) {
		properties = TypeVocabulary::describeProperties(props, f->wireType);
		obs_properties_destroy(props);
	}
	return ok(id, json{{"properties", std::move(properties)},
			   {"settings", engineSettingsToWire(f->filter, f->wireType)}});
}

// CHAIN ORDER MODEL: rec.filters vector order IS the contract apply order (index 0 first).
// obs_source_filter_add inserts each new filter at internal-array position 0, and the render/
// enum paths walk that array from the END down -- so the internal array is the REVERSE of the
// apply order, and add order == apply order == our vector order. The only place the two
// numberings meet is ReorderFilter's obs sync: internal index = (N-1) - contract index.

nlohmann::json ControlVerbs::handleListFilters(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	json filters = json::array();
	for (size_t i = 0; i < rec->filters.size(); ++i)
		filters.push_back(filterInfoJson(rec->filters[i], int(i)));
	return ok(id, json{{"filters", std::move(filters)}});
}

nlohmann::json ControlVerbs::handleSetFilterEnabled(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("filterId") || !params["filterId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterId' must be a string");
	if (!params.contains("enabled") || !params["enabled"].is_boolean())
		return err(id, kErrInvalidParams, "Invalid params: 'enabled' must be a boolean");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string filterId = params["filterId"].get<std::string>();
	const bool enabled = params["enabled"].get<bool>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	FilterRec *f = findFilter(*rec, filterId);
	if (!f)
		return err(id, kErrFilterNotFound, "No such filter: " + filterId,
			   {{"category", "filter"}, {"sourceId", sourceId}, {"filterId", filterId}});

	// Idempotent: only an actual transition touches the engine or emits (contract).
	if (obs_source_enabled(f->filter) != enabled) {
		obs_source_set_enabled(f->filter, enabled);
		emitEvent("filterChanged",
			  {{"sourceId", sourceId}, {"filterId", filterId}, {"enabled", enabled}});
	}
	return ok(id, json{{"filterId", filterId}, {"enabled", enabled}});
}

nlohmann::json ControlVerbs::handleReorderFilter(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("filterId") || !params["filterId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterId' must be a string");
	// Schema: integer, minimum 0 -- negatives are schema-invalid (-32602); only indexes ABOVE
	// the range are "out-of-range" in the contract's clamp sense.
	if (!params.contains("index") || !params["index"].is_number_integer() ||
	    params["index"].get<int64_t>() < 0)
		return err(id, kErrInvalidParams, "Invalid params: 'index' must be a non-negative integer");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string filterId = params["filterId"].get<std::string>();
	const int64_t requested = params["index"].get<int64_t>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	FilterRec *f = findFilter(*rec, filterId);
	if (!f)
		return err(id, kErrFilterNotFound, "No such filter: " + filterId,
			   {{"category", "filter"}, {"sourceId", sourceId}, {"filterId", filterId}});

	const int count = int(rec->filters.size());
	const int target = int(std::min<int64_t>(requested, count - 1));
	int from = 0;
	for (; from < count; ++from) {
		if (rec->filters[size_t(from)].filterId == filterId)
			break;
	}
	if (from == target) // idempotent no-op: no engine touch, no event
		return ok(id, json{{"filterId", filterId}, {"index", target}});

	// Move within the registry vector (apply order), then sync the engine's reversed array.
	obs_source_t *filterPtr = f->filter; // the vector move invalidates f
	FilterRec moved = std::move(rec->filters[size_t(from)]);
	rec->filters.erase(rec->filters.begin() + from);
	rec->filters.insert(rec->filters.begin() + target, std::move(moved));
	obs_source_filter_set_index(rec->source, filterPtr, size_t((count - 1) - target));

	emitEvent("filterChanged", {{"sourceId", sourceId}, {"filterId", filterId}, {"index", target}});
	return ok(id, json{{"filterId", filterId}, {"index", target}});
}

nlohmann::json ControlVerbs::handleSetFilterName(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("filterId") || !params["filterId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterId' must be a string");
	if (!params.contains("name") || !params["name"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'name' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string filterId = params["filterId"].get<std::string>();
	const std::string name = params["name"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	FilterRec *f = findFilter(*rec, filterId);
	if (!f)
		return err(id, kErrFilterNotFound, "No such filter: " + filterId,
			   {{"category", "filter"}, {"sourceId", sourceId}, {"filterId", filterId}});

	// Idempotent no-op on the current name (contract); the label is not unique and never
	// participates in identity (filterId does).
	if (f->name != name) {
		f->name = name;
		obs_source_set_name(f->filter, name.c_str());
		emitEvent("filterChanged",
			  {{"sourceId", sourceId}, {"filterId", filterId}, {"name", name}});
	}
	return ok(id, json{{"filterId", filterId}, {"name", name}});
}

nlohmann::json ControlVerbs::handleListAudioDevices(const nlohmann::json &id, const nlohmann::json &params)
{
	std::string flow;
	if (params.contains("flow")) {
		if (!params["flow"].is_string())
			return err(id, kErrInvalidParams, "Invalid params: 'flow' must be a string");
		flow = params["flow"].get<std::string>();
		if (flow != "render" && flow != "capture")
			return err(id, kErrInvalidParams,
				   "Invalid params: 'flow' must be 'render' or 'capture'");
	}
	return ok(id, json{{"devices", AudioDevices::enumerate(flow)}});
}

nlohmann::json ControlVerbs::handleSetAudioOutputDevice(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("deviceId") || !params["deviceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'deviceId' must be a string");
	const std::string deviceId = params["deviceId"].get<std::string>();

	// capabilities.audioOutput mirrors exactly this: no audio engine, no routing verb (the
	// audio-less harness construction). Real builds always wire the engine.
	if (!audio_)
		return err(id, kErrNotImplemented,
			   "SetAudioOutputDevice is not available in this build (audio output routing has not shipped)",
			   {{"category", "audio"}});

	// Validation order per the contract: unknown id -> 1006; known but unopenable -> 1005;
	// success stores the id ("default" stays "default", never resolved), marshals the render-
	// client swap to the audio thread, and echoes the STORED selection.
	if (!AudioDevices::renderDeviceExists(deviceId))
		return err(id, kErrInvalidPropertyValue, "Unknown audio output device",
			   {{"category", "audio"}, {"property", "deviceId"}});
	if (!AudioDevices::probeOpenRender(deviceId))
		return err(id, kErrDeviceBusy, "Audio output device could not be opened",
			   {{"category", "audio"},
			    {"property", "deviceId"},
			    {"detail", "the endpoint exists but a shared-mode open failed"}});

	audio_->setOutputDevice(deviceId);
	return ok(id, json{{"deviceId", deviceId}});
}

// Per-source audio state: gain/muted/balance ARE engine source state (volume/mute/balance on
// the source itself -- the single source of truth the tap producer reads), so the verbs are
// thin accessors. The sync offset rides the source's own stored sync-offset field the same way
// (that store is inert for the capture-callback path the mix engine consumes -- the engine
// seeds from it at attach and is marshalled explicitly on live changes). Available for ALL
// sources; on a source with no audio output the values are stored and inert (contract).
static int sourceSyncOffsetMs(obs_source_t *source)
{
	return std::clamp(int(obs_source_get_sync_offset(source) / 1000000), 0,
			  AudioMixEngine::kSyncOffsetMaxMs);
}

static json sourceAudioStateJson(const std::string &sourceId, obs_source_t *source)
{
	return json{{"sourceId", sourceId},
		    {"gain", obs_source_get_volume(source)},
		    {"muted", obs_source_muted(source)},
		    {"balance", obs_source_get_balance_value(source)},
		    {"syncOffsetMs", sourceSyncOffsetMs(source)}};
}

nlohmann::json ControlVerbs::handleGetSourceAudio(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	return ok(id, sourceAudioStateJson(sourceId, rec->source));
}

nlohmann::json ControlVerbs::handleSetSourceAudio(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();

	const bool hasGain = params.contains("gain");
	const bool hasMuted = params.contains("muted");
	const bool hasBalance = params.contains("balance");
	const bool hasSyncOffset = params.contains("syncOffsetMs");
	if (!hasGain && !hasMuted && !hasBalance && !hasSyncOffset)
		return err(id, kErrInvalidParams,
			   "Invalid params: at least one of 'gain', 'muted', 'balance', 'syncOffsetMs' must be present");

	// Validate every submitted field BEFORE applying any (no partial applies).
	float gain = 0.0f;
	if (hasGain) {
		if (!params["gain"].is_number() || params["gain"].get<double>() < 0.0)
			return err(id, kErrInvalidParams, "Invalid params: 'gain' must be a number >= 0");
		const double v = params["gain"].get<double>();
		gain = float(v > 20.0 ? 20.0 : v); // ceiling clamp; the echo carries the effective value
	}
	bool muted = false;
	if (hasMuted) {
		if (!params["muted"].is_boolean())
			return err(id, kErrInvalidParams, "Invalid params: 'muted' must be a boolean");
		muted = params["muted"].get<bool>();
	}
	float balance = 0.0f;
	if (hasBalance) {
		if (!params["balance"].is_number() || params["balance"].get<double>() < 0.0 ||
		    params["balance"].get<double>() > 1.0)
			return err(id, kErrInvalidParams,
				   "Invalid params: 'balance' must be a number in [0, 1]");
		balance = params["balance"].get<float>();
	}
	int syncOffsetMs = 0;
	if (hasSyncOffset) {
		if (!params["syncOffsetMs"].is_number_integer() || params["syncOffsetMs"].get<int64_t>() < 0)
			return err(id, kErrInvalidParams,
				   "Invalid params: 'syncOffsetMs' must be an integer >= 0");
		const int64_t v = params["syncOffsetMs"].get<int64_t>();
		// Ceiling clamp; the echo carries the effective value (the gain-clamp pattern).
		syncOffsetMs = int(v > AudioMixEngine::kSyncOffsetMaxMs ? AudioMixEngine::kSyncOffsetMaxMs : v);
	}

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	// Apply, then diff the STORED state -- audioChanged carries the changed fields only and
	// fires on actual transitions only (a no-op set emits nothing). Every origin rides this
	// handler (the GUI seam dispatches here), so wire and UI changes announce identically.
	const float oldGain = obs_source_get_volume(rec->source);
	const bool oldMuted = obs_source_muted(rec->source);
	const float oldBalance = obs_source_get_balance_value(rec->source);
	const int oldSyncOffsetMs = sourceSyncOffsetMs(rec->source);

	if (hasGain)
		obs_source_set_volume(rec->source, gain);
	if (hasMuted)
		obs_source_set_muted(rec->source, muted);
	if (hasBalance)
		obs_source_set_balance_value(rec->source, balance);
	if (hasSyncOffset) {
		// The stored offset is the canonical value (read back by the accessors and the
		// attach seed); the engine marshal applies the live delay-reserve change as one
		// fade-wrapped splice on the render thread (no-op while the source has no tap).
		obs_source_set_sync_offset(rec->source, int64_t(syncOffsetMs) * 1000000);
		if (audio_)
			audio_->setSourceSyncOffset(rec->source, syncOffsetMs);
	}

	json changed = json::object();
	if (hasGain && obs_source_get_volume(rec->source) != oldGain)
		changed["gain"] = obs_source_get_volume(rec->source);
	if (hasMuted && obs_source_muted(rec->source) != oldMuted)
		changed["muted"] = obs_source_muted(rec->source);
	if (hasBalance && obs_source_get_balance_value(rec->source) != oldBalance)
		changed["balance"] = obs_source_get_balance_value(rec->source);
	if (hasSyncOffset && syncOffsetMs != oldSyncOffsetMs)
		changed["syncOffsetMs"] = syncOffsetMs;
	if (!changed.empty()) {
		changed["sourceId"] = sourceId;
		emitEvent("audioChanged", std::move(changed));
	}

	return ok(id, sourceAudioStateJson(sourceId, rec->source));
}

nlohmann::json ControlVerbs::handleSetSourceFormat(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("format") || !params["format"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'format' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string format = params["format"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	SenderFormat parsed;
	if (!SpoutSenderEngine::parseFormat(format, parsed))
		return err(id, kErrInvalidPropertyValue, "Unknown format value: " + format,
			   {{"category", "source"},
			    {"sourceId", sourceId},
			    {"property", "format"},
			    {"detail", "valid values: srgb87, linear87, fp16"}});

	// Idempotent no-op on the current format (contract): no detach, no sender churn.
	if (format == rec->format)
		return ok(id, json{{"sourceId", sourceId}, {"format", format}});

	rec->format = format;

	// A broadcasting source recreates its sender: the format is fixed per attach (engine
	// contract), so detach + re-attach. senderName drops to null until the new sender's first
	// send; the cleared senderAnnounced makes refreshFromEngine fire a fresh senderNameResolved
	// (contract-documented observability). A non-broadcasting source just keeps the new format
	// for its next attach.
	if (rec->slotId >= 0 && engine_) {
		// The re-attach cycles the audio tap with the sender (tap off before the detach,
		// back on after a successful attach) -- harmless by design.
		if (audio_)
			audio_->detachSource(rec->source);
		engine_->detach(rec->slotId);
		rec->slotId = -1;
		rec->senderName.clear();
		rec->senderAnnounced = false;
		rec->sends = 0;
		rec->lastSendOk = false;

		const int slot = engine_->attach(rec->source, identity_.machine, identity_.port,
						 rec->displayName, parsed);
		if (slot < 0) {
			// The transition failed mid-flight: the source is now genuinely detached, which
			// IS a broadcast stop -- surface it (the event is the truth), then report 1011.
			emitEvent("broadcastChanged", {{"sourceId", sourceId}, {"broadcasting", false}});
			return err(id, kErrBroadcastStateConflict,
				   "Format change could not re-attach the sender",
				   {{"category", "source"},
				    {"sourceId", sourceId},
				    {"detail", "the source is no longer broadcasting; StartBroadcast to retry"}});
		}
		rec->slotId = slot;
		if (audio_)
			audio_->attachSource(rec->source, rec->sourceId);
	}

	return ok(id, json{{"sourceId", sourceId}, {"format", format}});
}

nlohmann::json ControlVerbs::handleStartBroadcast(const nlohmann::json &id, const nlohmann::json &params)
{
	std::vector<SourceRec *> targets;
	const bool subset = params.contains("sourceIds") && params["sourceIds"].is_array() &&
			    !params["sourceIds"].empty();
	if (params.contains("sourceIds") && !params["sourceIds"].is_array())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceIds' must be an array");
	if (subset) {
		for (const auto &v : params["sourceIds"]) {
			if (!v.is_string())
				return err(id, kErrInvalidParams,
					   "Invalid params: 'sourceIds' entries must be strings");
			SourceRec *rec = findSource(v.get<std::string>());
			if (!rec)
				return err(id, kErrSourceNotFound, "No such source: " + v.get<std::string>(),
					   {{"category", "source"}, {"sourceId", v.get<std::string>()}});
			targets.push_back(rec);
		}
	} else {
		for (auto &rec : recs_)
			targets.push_back(&rec);
	}

	json started = json::array();
	if (engine_) {
		engine_->start(); // idempotent
		for (SourceRec *rec : targets) {
			if (rec->slotId >= 0 || !rec->source)
				continue; // already broadcasting: idempotent, not listed
			applyDeviceShowing(*rec, /*shouldShow=*/!rec->disabled); // reacquire (if released) before attach, unless disabled
			SenderFormat fmt = SenderFormat::Srgb87;
			SpoutSenderEngine::parseFormat(rec->format, fmt); // rec.format is always validated
			const int slot = engine_->attach(rec->source, identity_.machine, identity_.port,
							 rec->displayName, fmt);
			if (slot < 0)
				continue; // could not attach; absent from `started`
			rec->slotId = slot;
			if (audio_)
				audio_->attachSource(rec->source, rec->sourceId);
			rec->senderName.clear();
			rec->senderAnnounced = false;
			rec->sends = 0;
			rec->lastSendOk = false;
			started.push_back(rec->sourceId);
		}
	}

	if (!started.empty()) {
		if (subset) {
			for (const auto &sid : started)
				emitEvent("broadcastChanged", {{"sourceId", sid}, {"broadcasting", true}});
		} else {
			emitEvent("broadcastChanged", {{"sourceId", nullptr}, {"broadcasting", true}});
		}
	}

	return ok(id, json{{"broadcasting", anyBroadcasting()}, {"started", std::move(started)}});
}

nlohmann::json ControlVerbs::handleStopBroadcast(const nlohmann::json &id, const nlohmann::json &params)
{
	std::vector<SourceRec *> targets;
	const bool subset = params.contains("sourceIds") && params["sourceIds"].is_array() &&
			    !params["sourceIds"].empty();
	if (params.contains("sourceIds") && !params["sourceIds"].is_array())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceIds' must be an array");
	if (subset) {
		for (const auto &v : params["sourceIds"]) {
			if (!v.is_string())
				return err(id, kErrInvalidParams,
					   "Invalid params: 'sourceIds' entries must be strings");
			SourceRec *rec = findSource(v.get<std::string>());
			if (!rec)
				return err(id, kErrSourceNotFound, "No such source: " + v.get<std::string>(),
					   {{"category", "source"}, {"sourceId", v.get<std::string>()}});
			targets.push_back(rec);
		}
	} else {
		for (auto &rec : recs_)
			targets.push_back(&rec);
	}

	json stopped = json::array();
	for (SourceRec *rec : targets) {
		if (rec->slotId < 0)
			continue; // not broadcasting: idempotent, not listed
		if (audio_)
			audio_->detachSource(rec->source); // tap off before the sender detach
		if (engine_)
			engine_->detach(rec->slotId);
		rec->slotId = -1;
		rec->senderName.clear();
		rec->senderAnnounced = false;
		rec->lastSendOk = false;
		if ((rec->releaseWhenIdle || rec->disabled) && sourceHoldsExclusiveDevice(*rec))
			applyDeviceShowing(*rec, /*shouldShow=*/false); // release the idle/disabled device
		stopped.push_back(rec->sourceId);
	}

	if (!stopped.empty()) {
		if (subset) {
			for (const auto &sid : stopped)
				emitEvent("broadcastChanged", {{"sourceId", sid}, {"broadcasting", false}});
		} else {
			emitEvent("broadcastChanged", {{"sourceId", nullptr}, {"broadcasting", false}});
		}
	}

	return ok(id, json{{"broadcasting", anyBroadcasting()}, {"stopped", std::move(stopped)}});
}

// True only for source types whose engine holds an exclusive HW capture device. Generic across
// camera (dshow_input) + WASAPI audio so standalone clients can release any of them while idle.
bool ControlVerbs::sourceHoldsExclusiveDevice(const SourceRec &rec)
{
	if (!rec.source)
		return false;
	const char *uid = obs_source_get_unversioned_id(rec.source); // e.g. "dshow_input"
	if (!uid)
		return false;
	return std::strcmp(uid, "dshow_input") == 0 ||
	       std::strcmp(uid, "wasapi_input_capture") == 0 ||
	       std::strcmp(uid, "wasapi_output_capture") == 0;
}

// Balanced raise/lower of the single showing ref this helper holds for the source. The source
// gets exactly one inc_showing at creation; deviceShowing mirrors whether that ref is raised so
// dec/inc are always balanced (avoids over-decrementing libobs's atomic show_refs).
void ControlVerbs::applyDeviceShowing(SourceRec &rec, bool shouldShow)
{
	if (!rec.source)
		return;
	if (shouldShow && !rec.deviceShowing) {
		obs_source_inc_showing(rec.source); // -> Action::Activate -> device reacquired
		rec.deviceShowing = true;
	} else if (!shouldShow && rec.deviceShowing) {
		obs_source_dec_showing(rec.source); // -> Action::Deactivate -> device released
		rec.deviceShowing = false;
	}
}

// Phase 2: the single source of truth for the showing precedence. A disabled source NEVER shows
// (hard device-release override); otherwise release-when-idle drops the ref while idle; otherwise
// the device is held live. Precedence: disabled > releaseWhenIdle > live.
bool ControlVerbs::deviceShouldShow(const SourceRec &rec, bool idle)
{
	return !rec.disabled && !(rec.releaseWhenIdle && idle);
}

nlohmann::json ControlVerbs::handleSetSourceIdleMode(const nlohmann::json &id,
						    const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("releaseWhenIdle") || !params["releaseWhenIdle"].is_boolean())
		return err(id, kErrInvalidParams, "Invalid params: 'releaseWhenIdle' must be a boolean");

	const std::string sourceId = params["sourceId"].get<std::string>();
	const bool releaseWhenIdle = params["releaseWhenIdle"].get<bool>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	rec->releaseWhenIdle = releaseWhenIdle;

	// Phase 2 "disabled" -- additive/back-compat. Parse only if PRESENT (and a boolean); an older
	// client that omits it leaves the current rec->disabled untouched. disabled is a HARD override:
	// the device is released and no frames are produced regardless of idle/placement.
	if (params.contains("disabled")) {
		if (!params["disabled"].is_boolean())
			return err(id, kErrInvalidParams, "Invalid params: 'disabled' must be a boolean");
		rec->disabled = params["disabled"].get<bool>();
	}

	// Device-type gate: only camera (dshow_input) + WASAPI input/output honor the showing-ref
	// device release. For every other type the verb is a recorded no-op: the mode flags are kept
	// (so a future type change is harmless) but the showing ref and settings are never touched.
	if (sourceHoldsExclusiveDevice(*rec)) {
		// deactivate-when-not-showing is seeded at source CREATION (SourceFactory, camera-only),
		// so the device release is implemented PURELY by the showing ref below -- no runtime
		// obs_source_update here. (A settings update would restart win-dshow capture and reopen
		// the just-closed camera at the idle transition, flickering the LED.) The showing decision
		// flows through deviceShouldShow so the precedence (disabled > releaseWhenIdle) lives once.
		const bool idle = (rec->slotId < 0); // no Spout consumer attached == idle
		applyDeviceShowing(*rec, /*shouldShow=*/deviceShouldShow(*rec, idle));
	}

	emitEvent("sourceIdleModeChanged",
		  {{"sourceId", sourceId}, {"releaseWhenIdle", releaseWhenIdle}, {"disabled", rec->disabled}});
	return ok(id, json{{"sourceId", sourceId}, {"releaseWhenIdle", releaseWhenIdle}, {"disabled", rec->disabled}});
}

// ---------------------------------------------------------------------------------------------
// Media transport handlers (contract 1.1.0)
// ---------------------------------------------------------------------------------------------

nlohmann::json ControlVerbs::handleGetMediaStatus(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	if (!isMediaControllable(rec->source))
		return err(id, kErrMediaNotSupported,
			   "Source " + sourceId + " (" + rec->wireType + ") does not support media transport",
			   {{"category", "source"}, {"sourceId", sourceId}});

	const int64_t duration = obs_source_media_get_duration(rec->source);
	const json stored = engineSettingsToWire(rec->source, rec->wireType);
	return ok(id, json{{"sourceId", sourceId},
			   {"state", mediaStateWire(obs_source_media_get_state(rec->source))},
			   {"positionMs", obs_source_media_get_time(rec->source)},
			   {"durationMs", duration > 0 ? json(duration) : json(nullptr)},
			   {"looping", stored.value("looping", false)}});
}

nlohmann::json ControlVerbs::handleControlMedia(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("action") || !params["action"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'action' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string action = params["action"].get<std::string>();
	if (action != "play" && action != "pause" && action != "restart" && action != "stop")
		return err(id, kErrInvalidParams,
			   "Invalid params: 'action' must be one of play|pause|restart|stop");

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	if (!isMediaControllable(rec->source))
		return err(id, kErrMediaNotSupported,
			   "Source " + sourceId + " (" + rec->wireType + ") does not support media transport",
			   {{"category", "source"}, {"sourceId", sourceId}});

	const enum obs_media_state before = obs_source_media_get_state(rec->source);
	enum obs_media_state expected;
	if (action == "play") {
		// play_pause(false) cannot resume a finished/halted pipeline (the source flips its
		// state word to PLAYING but the playback thread is gone) -- the contract's "play
		// resumes a paused or ended source" is honored as a restart for those states.
		if (before == OBS_MEDIA_STATE_ENDED || before == OBS_MEDIA_STATE_STOPPED ||
		    before == OBS_MEDIA_STATE_NONE || before == OBS_MEDIA_STATE_ERROR)
			obs_source_media_restart(rec->source);
		else
			obs_source_media_play_pause(rec->source, false);
		expected = OBS_MEDIA_STATE_PLAYING;
	} else if (action == "pause") {
		obs_source_media_play_pause(rec->source, true);
		expected = OBS_MEDIA_STATE_PAUSED;
	} else if (action == "restart") {
		obs_source_media_restart(rec->source);
		expected = OBS_MEDIA_STATE_PLAYING;
	} else { // stop
		obs_source_media_stop(rec->source);
		expected = OBS_MEDIA_STATE_STOPPED;
	}

	// The transport calls above only ENQUEUE onto the source's media-action queue; the video
	// thread executes them on its next tick (process_media_actions). Bounded-wait so the reply
	// state is the authoritative POST-action state (one frame interval typically; the cap only
	// matters if the pipeline cannot reach the expected state -- then the current state is the
	// honest best-effort answer per the contract).
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	while (obs_source_media_get_state(rec->source) != expected &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));

	// Transport by definition -- attribute any resulting tap dry-out as such (inert when no
	// dry-out follows).
	if (audio_)
		audio_->noteSourceTransport(rec->source);

	// The transition's mediaChanged rides the obs media signals (queued back to this thread);
	// the reply itself carries the settled state.
	return ok(id, json{{"sourceId", sourceId},
			   {"state", mediaStateWire(obs_source_media_get_state(rec->source))}});
}

nlohmann::json ControlVerbs::handleSeekMedia(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("positionMs") || !params["positionMs"].is_number_integer() ||
	    params["positionMs"].get<int64_t>() < 0)
		return err(id, kErrInvalidParams,
			   "Invalid params: 'positionMs' must be a non-negative integer");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const int64_t requested = params["positionMs"].get<int64_t>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});
	if (!isMediaControllable(rec->source))
		return err(id, kErrMediaNotSupported,
			   "Source " + sourceId + " (" + rec->wireType + ") does not support media transport",
			   {{"category", "source"}, {"sourceId", sourceId}});

	const int64_t duration = obs_source_media_get_duration(rec->source);
	if (duration <= 0)
		return err(id, kErrMediaNotSupported,
			   "Source " + sourceId + " has no known duration to seek within",
			   {{"category", "source"},
			    {"sourceId", sourceId},
			    {"detail", "durationMs is null (stream or still opening); seek requires a known duration"}});

	// Clamp server-side (contract); the engine's seek hook is fire-and-forget, so the clamped
	// target IS the effective position. Seeking changes no play/pause state and emits no
	// mediaChanged (the state word does not transition on a seek).
	const int64_t effective = std::clamp(requested, int64_t{0}, duration);
	obs_source_media_set_time(rec->source, effective);
	// Transport by definition -- attribute any resulting tap dry-out as such (a rapid scrub's
	// re-dries can otherwise outlive the producer-side jump bracket).
	if (audio_)
		audio_->noteSourceTransport(rec->source);
	return ok(id, json{{"sourceId", sourceId}, {"positionMs", effective}});
}

nlohmann::json ControlVerbs::handleInvokeSourceButton(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!params.contains("sourceId") || !params["sourceId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'sourceId' must be a string");
	if (!params.contains("property") || !params["property"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'property' must be a string");
	if (params.contains("filterId") && !params["filterId"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'filterId' must be a string");
	const std::string sourceId = params["sourceId"].get<std::string>();
	const std::string property = params["property"].get<std::string>();

	SourceRec *rec = findSource(sourceId);
	if (!rec)
		return err(id, kErrSourceNotFound, "No such source: " + sourceId,
			   {{"category", "source"}, {"sourceId", sourceId}});

	obs_source_t *target = rec->source;
	std::string wireType = rec->wireType;
	json errData = {{"category", "source"}, {"sourceId", sourceId}, {"property", property}};
	if (params.contains("filterId")) {
		const std::string filterId = params["filterId"].get<std::string>();
		FilterRec *f = findFilter(*rec, filterId);
		if (!f)
			return err(id, kErrFilterNotFound, "No such filter: " + filterId,
				   {{"category", "filter"}, {"sourceId", sourceId}, {"filterId", filterId}});
		target = f->filter;
		wireType = f->wireType;
		errData = {{"category", "filter"},
			   {"sourceId", sourceId},
			   {"filterId", filterId},
			   {"property", property}};
	}

	// The wire property name goes through the SAME overlay every settings key does (identity
	// today; one-table rename seam) before it meets the engine descriptor set.
	std::string engineProp = property;
	{
		const json translated =
			TypeVocabulary::settingsWireToEngine(wireType, json{{property, nullptr}});
		if (translated.is_object() && !translated.empty())
			engineProp = translated.begin().key();
	}

	// Instance-based enumeration (the proven safe path); the button callback runs against the
	// LIVE source/filter, so side effects land on the real object. Contract: button side
	// effects do not emit propertyChanged -- clients (and the GUI panel) re-fetch descriptors.
	bool invoked = false;
	if (obs_properties_t *props = obs_source_properties(target)) {
		if (obs_property_t *p = obs_properties_get(props, engineProp.c_str())) {
			if (obs_property_get_type(p) == OBS_PROPERTY_BUTTON) {
				obs_property_button_clicked(p, target);
				invoked = true;
			}
		}
		obs_properties_destroy(props);
	}
	if (!invoked)
		return err(id, kErrInvalidPropertyValue,
			   "Property '" + property + "' does not exist or is not a button", errData);
	return ok(id, json{{"invoked", true}});
}

// ---------------------------------------------------------------------------------------------
// Lifecycle + fleet snapshot (contract 1.5.0)
// ---------------------------------------------------------------------------------------------

nlohmann::json ControlVerbs::handleShutdown(const nlohmann::json &id, const nlohmann::json &params)
{
	// drain (default true): true = stop accepting, frame-drain the pipeline, then exit; false =
	// immediate clean exit (skip the drain WAIT) but STILL run the proven clean-teardown chain.
	bool drain = true;
	if (params.contains("drain")) {
		if (!params["drain"].is_boolean())
			return err(id, kErrInvalidParams, "Invalid params: 'drain' must be a boolean");
		drain = params["drain"].get<bool>();
	}

	// Repeat Shutdown while already stopping: idempotent re-ack echoing the FIRST committed drain
	// (a later drain:false cannot un-drain an in-progress drain). NOT the 1009 ShuttingDown error.
	if (stopping_)
		return ok(id, json{{"accepted", true}, {"drain", committedDrain_}, {"alreadyStopping", true}});

	// First Shutdown: commit the effective drain, then schedule the event-loop exit as a QUEUED
	// call. Queued (not direct) guarantees this reply value returns to the server FIRST -- the
	// quit lambda runs on a later main-loop iteration, after the connection thread has already
	// sent the reply (see the reply-before-quit ordering note below). QCoreApplication::quit()
	// drives the established teardown chain (instanceShuttingDown -> stopAll -> ...), so honoring
	// drain:false is "exit promptly without an added drain wait", which this scheduling already is.
	stopping_ = true;
	committedDrain_ = drain;
	QMetaObject::invokeMethod(
		QCoreApplication::instance(), [] { QCoreApplication::quit(); }, Qt::QueuedConnection);

	return ok(id, json{{"accepted", true}, {"drain", drain}});
}

// ---------------------------------------------------------------------------------------------
// Standalone profiles (item 05). FULLY INERT in managed (helper) mode: the four verbs reject when
// an owner-id is present, and auto-load/auto-save (driven from the GUI seam) is gated the same way.
// A profile is a saved scene -- sources + verbatim per-source settings + the per-source audio quad +
// a persisted filter chain + a per-profile fps tier + the audio output device + window layout.
// ---------------------------------------------------------------------------------------------

nlohmann::json ControlVerbs::snapshotProfileJson(const std::string &profileName, int fpsTier) const
{
	// Document level: the createFromConfigJson keys (port + sources[]) PLUS the profile chrome
	// (profileName, fpsTier, audioOutputDevice, window). createFromConfigJson ignores the chrome keys
	// harmlessly, so the same document round-trips through the load primitive.
	json doc = json::object();
	doc["profileName"] = profileName;
	doc["fpsTier"] = fpsTier;
	doc["port"] = identity_.port;
	if (audio_)
		doc["audioOutputDevice"] = audio_->outputDevice();
	if (windowLayoutProvider_) {
		const json win = windowLayoutProvider_();
		if (win.is_object())
			doc["window"] = win;
	}

	json sources = json::array();
	for (const auto &rec : recs_) {
		if (!rec.source)
			continue;
		// Each entry mirrors a SourceFactory per-source entry: id(=wireType so the load path's
		// sourceEngineId() maps it back to the engine id), name, format/externalId/audio quad as
		// SIBLING keys (never nested in settings -- a format/audio key inside settings is rejected by
		// design), settings VERBATIM (engine, defaults stripped), and the NEW filters[] sibling.
		json entry = json::object();
		entry["id"] = rec.wireType;
		entry["name"] = rec.displayName;
		entry["format"] = rec.format;
		if (!rec.externalId.empty())
			entry["externalId"] = rec.externalId;
		entry["settings"] = engineSettingsStripped(rec.source);
		// Per-source audio quad. gain/muted/balance are read live off the source (the single source of
		// truth the audio tap reads). syncOffsetMs: the engine stores NANOSECONDS; the profile JSON
		// carries MILLISECONDS (sourceSyncOffsetMs converts ns->ms with the wire clamp), and the load
		// path re-multiplies by 1e6 -- a naive raw-ns emit would inflate 1e6x and clamp to 950.
		entry["gain"] = obs_source_get_volume(rec.source);
		entry["muted"] = obs_source_muted(rec.source);
		entry["balance"] = obs_source_get_balance_value(rec.source);
		entry["syncOffsetMs"] = sourceSyncOffsetMs(rec.source);

		// Persisted filter chain (NEW schema). rec.filters vector order IS the contract apply order
		// (index 0 first); emit front-to-back so a front-to-back replay reproduces the chain. enabled
		// is read LIVE via obs_source_enabled (FilterRec has no enabled field); filterId is per-session
		// and is NOT persisted; settings are the filter's engine settings, defaults stripped.
		json filters = json::array();
		for (const auto &f : rec.filters) {
			if (!f.filter)
				continue;
			// Filter settings are saved as WIRE settings (overlay applied, defaults stripped) because
			// the load-path replay (handleAddFilter) runs settingsWireToEngine on them -- see
			// engineSettingsToWireStripped. This differs from a SOURCE entry's engine-verbatim settings.
			filters.push_back(json{{"filterType", f.wireType},
					       {"name", f.name},
					       {"enabled", obs_source_enabled(f.filter)},
					       {"settings", engineSettingsToWireStripped(f.filter, f.wireType)}});
		}
		entry["filters"] = std::move(filters);

		sources.push_back(std::move(entry));
	}
	doc["sources"] = std::move(sources);
	return doc;
}

void ControlVerbs::releaseAllSourcesForReload()
{
	// Mirrors releaseAllSources()'s per-rec order EXACTLY but WITHOUT the one-shot released_ latch,
	// so it is safe to run on every profile load (the destructor's releaseAllSources() still works at
	// shutdown -- released_ stays false here). Audio tap removal precedes the sender detach and the
	// source release (the media-signal ordering discipline; idempotent on never-attached sources).
	for (auto &rec : recs_) {
		if (audio_ && rec.source)
			audio_->detachSource(rec.source);
		if (rec.slotId >= 0 && engine_) {
			engine_->detach(rec.slotId);
			rec.slotId = -1;
		}
		disconnectMediaSignals(rec);
		for (auto &f : rec.filters) {
			if (rec.source && f.filter)
				obs_source_filter_remove(rec.source, f.filter);
			if (f.filter)
				obs_source_release(f.filter);
			f.filter = nullptr;
		}
		rec.filters.clear();
		if (rec.source) {
			if (rec.deviceShowing)
				obs_source_dec_showing(rec.source);
			obs_source_release(rec.source);
			rec.source = nullptr;
		}
	}
	recs_.clear();
}

std::string ControlVerbs::loadProfileSources(const nlohmann::json &profileJson, int targetFpsNum)
{
	// A profile is a saved scene: LOAD REPLACES (swaps) the live source set. The strict order below
	// folds the live re-tier in. CRASH HAZARD: obs_reset_video joins/stops the graphics thread, so
	// ALL wait=true OBS_TASK_GRAPHICS teardown (SpoutSenderEngine::stop's per-slot GPU drain) MUST
	// complete BEFORE the reset; the re-tier runs strictly between the teardown and the rebuild. This
	// runs on the control/Qt main thread (every dispatch is marshalled here), never the graphics
	// thread, so obs_reset_video cannot self-join.
	const bool doRetier = targetFpsNum > 0 && targetFpsNum != identity_.fpsTier && retierHandler_;

	// 1. Detach audio taps FIRST (the media-signal ordering discipline; the tap removal always
	//    precedes the source release + the sender detach). Done explicitly here -- before the engine
	//    stop -- to match the strict re-tier sequence; releaseAllSourcesForReload's own tap-detach is
	//    then an idempotent no-op.
	if (audio_) {
		for (auto &rec : recs_) {
			if (rec.source)
				audio_->detachSource(rec.source);
		}
	}

	// 2. Stop the sender engine: removes the render callback FIRST (under the callbacks' mutex), THEN
	//    drains per-slot GPU teardown via wait=true OBS_TASK_GRAPHICS tasks WHILE the graphics thread
	//    is still alive. After stop() returns, ZERO GPU objects + ZERO render callbacks remain -- so
	//    the obs_reset_video below can safely join/recreate the graphics thread with no wait=true task
	//    left to hang. THIS ORDERING (all GPU teardown before the reset) is the crash-hazard mitigation.
	if (engine_)
		engine_->stop();

	// 3. Re-entrant source release (the destructive swap): mirrors releaseAllSources()'s per-rec order
	//    WITHOUT the one-shot released_ latch. engine_->detach() inside is a no-op now (stop() already
	//    tore down every slot), which is correct -- the GPU work was already drained in step 2.
	releaseAllSourcesForReload();

	// 4. obs_reset_video with the new fps (only when the tier actually changes). With every GPU object
	//    and the render callback gone (steps 2-3), the reset can safely join/recreate the graphics
	//    thread. The tier change is TRANSIENT -- the global engine/fpsTier QSetting is untouched.
	if (doRetier) {
		if (!retierHandler_(targetFpsNum)) {
			// The reset failed; libobs is in an indeterminate video state for this session. Re-arm
			// the engine so a non-broadcasting GUI still runs, and surface the failure.
			if (engine_)
				engine_->start();
			return "live re-tier (obs_reset_video) failed";
		}
		identity_.fpsTier = targetFpsNum;
	}

	// 5. Rebuild the engine sources from the profile (createFromConfigJson takes the WHOLE document) +
	//    6. adopt them (mints recs_ -- NOT yet broadcasting; GUI semantics) + 7. replay each source's
	//    filters front-to-back (vector order == apply order).
	SourceConfigResult res = SourceFactory::createFromConfigJson(profileJson.dump().c_str());
	if (!res.ok) {
		// On ok=false the partial source set is already released by the factory -- do NOT double
		// release. recs_ is empty (we swapped it out above); the instance is now source-less.
		if (engine_)
			engine_->start();
		return res.error.empty() ? std::string("profile sources failed to build") : res.error;
	}
	adoptBootSources(std::move(res.sources), /*attachedInOrder=*/false);

	// Filter replay: the sources[] order in the document matches recs_ insertion order (adoptBootSources
	// pushes in vector order), so zip by index. Replay front-to-back via the AddFilter path so the
	// chain apply order is reproduced; carry the saved enabled flag.
	if (profileJson.contains("sources") && profileJson["sources"].is_array()) {
		const json &srcArr = profileJson["sources"];
		for (size_t i = 0; i < recs_.size() && i < srcArr.size(); ++i) {
			const json &entry = srcArr[i];
			if (!entry.is_object() || !entry.contains("filters") || !entry["filters"].is_array())
				continue;
			const std::string sourceId = recs_[i].sourceId;
			for (const json &f : entry["filters"]) {
				if (!f.is_object())
					continue;
				const std::string filterType = f.value("filterType", std::string());
				const std::string name = f.value("name", std::string());
				if (filterType.empty())
					continue;
				json addParams = json{{"sourceId", sourceId}, {"filterType", filterType}};
				if (!name.empty())
					addParams["name"] = name;
				if (f.contains("settings") && f["settings"].is_object())
					addParams["settings"] = f["settings"];
				const json reply = dispatch(json(0), "AddFilter", addParams);
				// Best-effort: a filter type missing on this machine warns and is skipped (decision:
				// best-effort create + warn, do not hard-reject the whole profile). Apply the saved
				// enabled flag if the add succeeded and the saved state was disabled.
				if (reply.contains("result") && reply["result"].contains("filterId")) {
					const bool enabled = f.value("enabled", true);
					if (!enabled) {
						const std::string filterId =
							reply["result"]["filterId"].get<std::string>();
						dispatch(json(0), "SetFilterEnabled",
							 json{{"sourceId", sourceId},
							      {"filterId", filterId},
							      {"enabled", false}});
					}
				} else {
					std::fprintf(stderr,
						     "[control] profile load: filter '%s' on '%s' could not be "
						     "re-applied (type unavailable?)\n",
						     filterType.c_str(), sourceId.c_str());
				}
			}
		}
	}

	// 8. Re-arm the sender engine render callback (idempotent; zero slots until a StartBroadcast).
	if (engine_)
		engine_->start();

	// Apply the audio output device (best-effort) + the window layout from the profile chrome.
	if (audio_ && profileJson.contains("audioOutputDevice") &&
	    profileJson["audioOutputDevice"].is_string()) {
		const std::string dev = profileJson["audioOutputDevice"].get<std::string>();
		if (!dev.empty())
			dispatch(json(0), "SetAudioOutputDevice", json{{"deviceId", dev}});
	}
	if (windowLayoutApplier_ && profileJson.contains("window") && profileJson["window"].is_object())
		windowLayoutApplier_(profileJson["window"]);

	return std::string(); // success
}

nlohmann::json ControlVerbs::handleListProfiles(const nlohmann::json &id, const nlohmann::json &params)
{
	(void)params;
	if (!identity_.ownerId.empty())
		return err(id, kErrProfilesUnavailable, "Profiles are unavailable in managed mode",
			   {{"category", "profile"}});
	json names = json::array();
	for (const std::string &name : ProfileStore::listProfiles())
		names.push_back(name);
	return ok(id, json{{"profiles", std::move(names)}, {"lastProfile", ProfileStore::lastProfile()}});
}

nlohmann::json ControlVerbs::handleSaveProfile(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!identity_.ownerId.empty())
		return err(id, kErrProfilesUnavailable, "Profiles are unavailable in managed mode",
			   {{"category", "profile"}});
	if (!params.contains("name") || !params["name"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'name' must be a string");
	const std::string name = params["name"].get<std::string>();
	if (!ProfileStore::isValidName(name))
		return err(id, kErrProfileInvalid, "Invalid profile name",
			   {{"category", "profile"}, {"detail", "names cannot contain path separators"}});

	const json doc = snapshotProfileJson(name, identity_.fpsTier);
	if (!ProfileStore::write(name, doc.dump(/*indent=*/2)))
		return err(id, kErrProfileInvalid, "Profile could not be written",
			   {{"category", "profile"}, {"name", name}});
	ProfileStore::setLastProfile(name);
	return ok(id, json{{"name", name}, {"sources", int(doc.value("sources", json::array()).size())}});
}

nlohmann::json ControlVerbs::handleDeleteProfile(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!identity_.ownerId.empty())
		return err(id, kErrProfilesUnavailable, "Profiles are unavailable in managed mode",
			   {{"category", "profile"}});
	if (!params.contains("name") || !params["name"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'name' must be a string");
	const std::string name = params["name"].get<std::string>();
	if (!ProfileStore::isValidName(name))
		return err(id, kErrProfileInvalid, "Invalid profile name", {{"category", "profile"}});
	if (!ProfileStore::exists(name))
		return err(id, kErrProfileNotFound, "No such profile: " + name,
			   {{"category", "profile"}, {"name", name}});
	if (!ProfileStore::remove(name))
		return err(id, kErrProfileInvalid, "Profile could not be deleted",
			   {{"category", "profile"}, {"name", name}});
	if (ProfileStore::lastProfile() == name)
		ProfileStore::setLastProfile(std::string());
	return ok(id, json{{"name", name}, {"deleted", true}});
}

nlohmann::json ControlVerbs::handleLoadProfile(const nlohmann::json &id, const nlohmann::json &params)
{
	if (!identity_.ownerId.empty())
		return err(id, kErrProfilesUnavailable, "Profiles are unavailable in managed mode",
			   {{"category", "profile"}});
	if (!params.contains("name") || !params["name"].is_string())
		return err(id, kErrInvalidParams, "Invalid params: 'name' must be a string");
	const std::string name = params["name"].get<std::string>();
	if (!ProfileStore::isValidName(name))
		return err(id, kErrProfileInvalid, "Invalid profile name", {{"category", "profile"}});

	std::string text;
	if (!ProfileStore::read(name, text))
		return err(id, kErrProfileNotFound, "No such profile: " + name,
			   {{"category", "profile"}, {"name", name}});
	json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (!doc.is_object())
		return err(id, kErrProfileInvalid, "Profile JSON is corrupt: " + name,
			   {{"category", "profile"}, {"name", name}});

	// The loaded profile's fpsTier wins in standalone (transient live re-tier). Absent/<=0 -> keep the
	// running tier (no re-tier).
	int targetFps = 0;
	if (doc.contains("fpsTier") && doc["fpsTier"].is_number_integer())
		targetFps = doc["fpsTier"].get<int>();

	const std::string error = loadProfileSources(doc, targetFps);
	if (!error.empty())
		return err(id, kErrSourceCreateFailed, "Profile load failed: " + error,
			   {{"category", "profile"}, {"name", name}});
	ProfileStore::setLastProfile(name);

	// The profile load swapped the whole source set. The GUI driver refreshes its list explicitly off
	// this reply (the toolbar Profiles control), so no bespoke wire event is emitted -- the contract's
	// sourceAdded/sourceRemoved carry a single-source shape and must not be repurposed for a bulk swap.
	// A standalone wire client re-pulls the new set with ListSources/GetStatus (which the reply mirrors).
	json sources = json::array();
	for (const auto &rec : recs_)
		sources.push_back(sourceSummary(rec));
	return ok(id, json{{"name", name}, {"fpsTier", identity_.fpsTier}, {"sources", std::move(sources)}});
}

// ---------------------------------------------------------------------------------------------
// GUI media-transport seams -- the GUI drives transport through the SAME dispatch path as the
// wire, so validation, state, and event emission stay single-pathed.
// ---------------------------------------------------------------------------------------------

nlohmann::json ControlVerbs::guiGetMediaStatus(const std::string &sourceId)
{
	return dispatch(json(0), "GetMediaStatus", json{{"sourceId", sourceId}});
}

nlohmann::json ControlVerbs::guiControlMedia(const std::string &sourceId, const std::string &action)
{
	return dispatch(json(0), "ControlMedia", json{{"sourceId", sourceId}, {"action", action}});
}

nlohmann::json ControlVerbs::guiSeekMedia(const std::string &sourceId, int64_t positionMs)
{
	return dispatch(json(0), "SeekMedia", json{{"sourceId", sourceId}, {"positionMs", positionMs}});
}

// ---------------------------------------------------------------------------------------------
// GUI source/property seams -- thin dispatch wrappers, same single-path rule as above.
// ---------------------------------------------------------------------------------------------

nlohmann::json ControlVerbs::guiGetVersion()
{
	return dispatch(json(0), "GetVersion", json::object());
}

nlohmann::json ControlVerbs::guiGetStatus()
{
	return dispatch(json(0), "GetStatus", json::object());
}

nlohmann::json ControlVerbs::guiListSources()
{
	return dispatch(json(0), "ListSources", json::object());
}

obs_source_t *ControlVerbs::guiPreviewSource(const std::string &sourceId)
{
	// Render plumbing, not a dispatch: borrowed pointer, main-thread only (see header note).
	SourceRec *rec = findSource(sourceId);
	return rec ? rec->source : nullptr;
}

nlohmann::json ControlVerbs::guiCreateSource(const std::string &type, const std::string &displayName)
{
	json params = {{"type", type}};
	if (!displayName.empty())
		params["displayName"] = displayName;
	return dispatch(json(0), "CreateSource", params);
}

nlohmann::json ControlVerbs::guiRemoveSource(const std::string &sourceId)
{
	return dispatch(json(0), "RemoveSource", json{{"sourceId", sourceId}});
}

nlohmann::json ControlVerbs::guiListSourceProperties(const std::string &sourceId)
{
	return dispatch(json(0), "ListSourceProperties", json{{"sourceId", sourceId}});
}

nlohmann::json ControlVerbs::guiSetSourceProperties(const std::string &sourceId, const nlohmann::json &settings)
{
	return dispatch(json(0), "SetSourceProperties", json{{"sourceId", sourceId}, {"settings", settings}});
}

nlohmann::json ControlVerbs::guiInvokeSourceButton(const std::string &sourceId, const std::string &property)
{
	return dispatch(json(0), "InvokeSourceButton", json{{"sourceId", sourceId}, {"property", property}});
}

nlohmann::json ControlVerbs::guiSetSourceFormat(const std::string &sourceId, const std::string &format)
{
	return dispatch(json(0), "SetSourceFormat", json{{"sourceId", sourceId}, {"format", format}});
}

nlohmann::json ControlVerbs::guiListAudioDevices(const std::string &flow)
{
	return dispatch(json(0), "ListAudioDevices",
			flow.empty() ? json::object() : json{{"flow", flow}});
}

nlohmann::json ControlVerbs::guiSetAudioOutputDevice(const std::string &deviceId)
{
	return dispatch(json(0), "SetAudioOutputDevice", json{{"deviceId", deviceId}});
}

nlohmann::json ControlVerbs::guiGetSourceAudio(const std::string &sourceId)
{
	return dispatch(json(0), "GetSourceAudio", json{{"sourceId", sourceId}});
}

nlohmann::json ControlVerbs::guiSetSourceAudio(const std::string &sourceId, const nlohmann::json &fields)
{
	json params = fields.is_object() ? fields : json::object();
	params["sourceId"] = sourceId;
	return dispatch(json(0), "SetSourceAudio", std::move(params));
}

nlohmann::json ControlVerbs::guiListAvailableFilters(const std::string &sourceId)
{
	return dispatch(json(0), "ListAvailableFilters", json{{"sourceId", sourceId}});
}

nlohmann::json ControlVerbs::guiListFilters(const std::string &sourceId)
{
	return dispatch(json(0), "ListFilters", json{{"sourceId", sourceId}});
}

nlohmann::json ControlVerbs::guiAddFilter(const std::string &sourceId, const std::string &filterType,
					  const std::string &name)
{
	json params = {{"sourceId", sourceId}, {"filterType", filterType}};
	if (!name.empty())
		params["name"] = name;
	return dispatch(json(0), "AddFilter", params);
}

nlohmann::json ControlVerbs::guiRemoveFilter(const std::string &sourceId, const std::string &filterId)
{
	return dispatch(json(0), "RemoveFilter", json{{"sourceId", sourceId}, {"filterId", filterId}});
}

nlohmann::json ControlVerbs::guiSetFilterEnabled(const std::string &sourceId, const std::string &filterId,
						 bool enabled)
{
	return dispatch(json(0), "SetFilterEnabled",
			json{{"sourceId", sourceId}, {"filterId", filterId}, {"enabled", enabled}});
}

nlohmann::json ControlVerbs::guiReorderFilter(const std::string &sourceId, const std::string &filterId, int index)
{
	return dispatch(json(0), "ReorderFilter",
			json{{"sourceId", sourceId}, {"filterId", filterId}, {"index", index}});
}

nlohmann::json ControlVerbs::guiRenameFilter(const std::string &sourceId, const std::string &filterId,
					     const std::string &name)
{
	return dispatch(json(0), "SetFilterName",
			json{{"sourceId", sourceId}, {"filterId", filterId}, {"name", name}});
}

nlohmann::json ControlVerbs::guiListFilterProperties(const std::string &sourceId, const std::string &filterId)
{
	return dispatch(json(0), "ListFilterProperties", json{{"sourceId", sourceId}, {"filterId", filterId}});
}

nlohmann::json ControlVerbs::guiInvokeFilterButton(const std::string &sourceId, const std::string &filterId,
						   const std::string &property)
{
	return dispatch(json(0), "InvokeSourceButton",
			json{{"sourceId", sourceId}, {"filterId", filterId}, {"property", property}});
}

nlohmann::json ControlVerbs::guiSetFilterProperties(const std::string &sourceId, const std::string &filterId,
						    const nlohmann::json &settings)
{
	return dispatch(json(0), "SetFilterProperties",
			json{{"sourceId", sourceId}, {"filterId", filterId}, {"settings", settings}});
}

// Item 05: GUI profile seams -- the toolbar Profiles control dispatches through the same verb path
// (the reject-when-managed gate, the live re-tier, the source/filter rebuild are identical to a
// standalone wire client). The toolbar is hidden in managed mode, so the gate's reject is the
// belt-and-braces backstop, not the primary inert mechanism.
nlohmann::json ControlVerbs::guiListProfiles()
{
	return dispatch(json(0), "ListProfiles", json::object());
}

nlohmann::json ControlVerbs::guiLoadProfile(const std::string &name)
{
	return dispatch(json(0), "LoadProfile", json{{"name", name}});
}

nlohmann::json ControlVerbs::guiSaveProfile(const std::string &name)
{
	return dispatch(json(0), "SaveProfile", json{{"name", name}});
}

nlohmann::json ControlVerbs::guiDeleteProfile(const std::string &name)
{
	return dispatch(json(0), "DeleteProfile", json{{"name", name}});
}

nlohmann::json ControlVerbs::guiSetFpsTier(int fpsTier)
{
	// STANDALONE only: the helper's frame rate comes from --fps-tier, never the GUI. Reject when owned
	// so the dropdown can never re-tier a managed instance.
	if (!identity_.ownerId.empty())
		return err(json(0), kErrProfilesUnavailable, "FPS tier is unavailable in managed mode",
			   {{"category", "engine"}});
	if (fpsTier <= 0)
		return err(json(0), kErrInvalidParams, "Invalid fpsTier: must be a positive integer");
	// No-op when the tier is unchanged -- avoid a pointless engine teardown + obs_reset_video.
	if (fpsTier == identity_.fpsTier)
		return ok(json(0), json{{"fpsTier", identity_.fpsTier}});
	if (!retierHandler_)
		return err(json(0), kErrBroadcastStateConflict, "Live re-tier is unavailable (no GUI engine)",
			   {{"category", "engine"}});

	// Preserve a live broadcast across the re-tier: stop it FIRST (proper slot bookkeeping -- detaches
	// audio taps, detaches every slot with its wait=true GPU drain, resets slotId/sender state, emits
	// broadcastChanged), then re-arm it at the new tier after the reset. A bare retier while
	// broadcasting would re-tier under live senders; the stop/start bracket keeps the bookkeeping
	// single-pathed through the existing verbs.
	const bool wasBroadcasting = anyBroadcasting();
	if (wasBroadcasting)
		dispatch(json(0), "StopBroadcast", json::object());

	// Drain the render callback before obs_reset_video. Every per-slot GPU object was already torn down
	// by the StopBroadcast detaches (or there were none); engine_->stop() now removes the main render
	// callback so the reset can safely join/recreate the graphics thread. Sources in recs_ stay alive --
	// obs sources survive a video reset and re-init their GPU resources lazily on the next render.
	if (engine_)
		engine_->stop();

	if (!retierHandler_(fpsTier)) {
		// The reset failed; libobs video is in an indeterminate state for this session. Re-arm the
		// engine so a non-broadcasting GUI still runs, and surface the failure (the tier is unchanged).
		// The API name stays in this INTERNAL log line only -- never in the user-facing message below.
		std::fprintf(stderr, "[control] live re-tier failed: obs_reset_video returned failure\n");
		if (engine_)
			engine_->start();
		return err(json(0), kErrBroadcastStateConflict,
			   "Live FPS change failed (video engine reset)", {{"category", "engine"}});
	}
	identity_.fpsTier = fpsTier;

	// Re-arm the render callback (zero slots until a StartBroadcast), then restore broadcasting if it
	// was live before the change -- the same StartBroadcast verb re-attaches every source at the new tier.
	if (engine_)
		engine_->start();
	if (wasBroadcasting)
		dispatch(json(0), "StartBroadcast", json::object());

	return ok(json(0), json{{"fpsTier", identity_.fpsTier}});
}

bool ControlVerbs::profileIsDirty(const std::string &name)
{
	// Dirty = the live snapshot differs from the saved profile (the toolbar asterisk). A missing or
	// unreadable/corrupt profile reads as dirty (there is unsaved live state vs nothing on disk).
	std::string text;
	if (!ProfileStore::read(name, text))
		return true;
	json saved = json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (!saved.is_object())
		return true;
	// Snapshot with the SAVED name (so the profileName chrome never self-diffs) but the LIVE fps tier:
	// the per-profile FPS is now a user-facing edit (the toolbar dropdown), so a running-vs-saved tier
	// difference is real drift and must light the asterisk -- exactly like a source/audio/filter edit --
	// prompting a Save. window geometry is volatile (the user moves the window constantly) and would
	// make the asterisk perpetually on, so it is excluded from the comparison.
	json live = snapshotProfileJson(name, identity_.fpsTier);
	live.erase("window");
	saved.erase("window");
	return live != saved;
}

} // namespace moxrelay
