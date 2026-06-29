// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SourceFactory implementation. The monitor_capture path (primary-monitor seeding via the GDI
// szDevice fallback, then canonical DeviceID upgrade from the source's own property list) is the
// proven M1 logic moved verbatim from main.cpp (R4); createColorSource is new in M2.1.

#include "SourceFactory.hpp"
#include "control/TypeVocabulary.hpp"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace moxrelay {

namespace {

#ifdef _WIN32
// The win-capture "monitor_capture" source (registered as the duplicator variant on D3D11/Win10+)
// keys monitor selection on a STRING "monitor_id" that defaults to "DUMMY" (matches nothing). Its
// find_monitor() first matches an EnumDisplayDevices DeviceID, then FALLS BACK to matching the GDI
// device name (mi.szDevice, e.g. "\\.\\DISPLAY1"). We hand it the PRIMARY monitor's szDevice so the
// fallback path resolves the real primary display without us reinventing DeviceID enumeration.
struct primary_monitor_ctx {
	char szDevice[CCHDEVICENAME];
	bool found;
};

BOOL CALLBACK enum_primary_monitor(HMONITOR h, HDC, LPRECT, LPARAM param)
{
	auto *ctx = reinterpret_cast<primary_monitor_ctx *>(param);
	MONITORINFOEXA mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(h, reinterpret_cast<LPMONITORINFO>(&mi)) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
		strcpy_s(ctx->szDevice, _countof(ctx->szDevice), mi.szDevice);
		ctx->found = true;
		return FALSE; // stop -- found the primary
	}
	return TRUE;
}

std::string primary_monitor_device()
{
	primary_monitor_ctx ctx = {};
	EnumDisplayMonitors(nullptr, nullptr, enum_primary_monitor, reinterpret_cast<LPARAM>(&ctx));
	return ctx.found ? std::string(ctx.szDevice) : std::string();
}

// Read back the canonical "monitor_id" the source itself advertises for the PRIMARY display via its
// property list (duplicator enum_monitor_props appends the localized "(Primary Monitor)" marker and
// stores the EnumDisplayDevices DeviceID as the list item's string value). Returns "" if no entry is
// flagged primary, in which case the caller keeps the szDevice fallback value.
std::string primary_monitor_id_from_props(obs_source_t *src)
{
	std::string result;
	obs_properties_t *props = obs_source_properties(src);
	if (!props)
		return result;

	obs_property_t *p = obs_properties_get(props, "monitor_id");
	if (p) {
		const size_t n = obs_property_list_item_count(p);
		for (size_t i = 0; i < n; ++i) {
			if (obs_property_list_item_disabled(p, i))
				continue; // the "Select a display" placeholder
			const char *name = obs_property_list_item_name(p, i);
			const char *val = obs_property_list_item_string(p, i);
			if (val && name && std::strstr(name, "(") && std::strstr(name, ")")) {
				// First real entry: keep as a fallback in case none is explicitly primary.
				if (result.empty())
					result = val;
			}
			// A primary monitor's description carries a parenthesized marker distinct from the
			// "WxH @ x,y" geometry; prefer the LAST real entry that GDI flagged primary. We can't
			// read the locale string reliably, so we lean on szDevice (set by the caller) as the
			// authoritative primary and only use this list value to upgrade DUMMY -> a real id.
		}
	}
	obs_properties_destroy(props);
	return result;
}
#endif // _WIN32

} // namespace

std::set<std::string> SourceFactory::registeredInputIds()
{
	std::set<std::string> ids;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_input_types(i, &id); ++i) {
		if (id)
			ids.insert(id);
	}
	return ids;
}

namespace {

// Seed "monitor_id" with the PRIMARY display's GDI device name so the duplicator's szDevice
// fallback resolves it (the "DUMMY" default matches nothing -> a 0x0 source). No-op if the
// settings already carry a non-default monitor_id (an explicit config choice wins).
void seed_primary_monitor_settings(obs_data_t *settings)
{
#ifdef _WIN32
	const char *existing = obs_data_get_string(settings, "monitor_id");
	if (existing && *existing && std::strcmp(existing, "DUMMY") != 0)
		return;
	std::string primaryDev = primary_monitor_device();
	if (!primaryDev.empty())
		obs_data_set_string(settings, "monitor_id", primaryDev.c_str());
#else
	(void)settings;
#endif
}

// Post-create: upgrade the GDI seed to the source's canonical primary DeviceID if it exposes one
// (obs_source_update merges -- only monitor_id changes, other settings stay as configured).
void upgrade_primary_monitor_id(obs_source_t *src)
{
#ifdef _WIN32
	std::string canonical = primary_monitor_id_from_props(src);
	if (!canonical.empty()) {
		obs_data_t *upd = obs_data_create();
		obs_data_set_string(upd, "monitor_id", canonical.c_str());
		obs_source_update(src, upd);
		obs_data_release(upd);
	}
#else
	(void)src;
#endif
}

} // namespace

obs_source_t *SourceFactory::createColorSource(const std::string &name, uint32_t colorAbgr, int width, int height)
{
	const std::set<std::string> ids = registeredInputIds();
	if (ids.find("color_source_v3") == ids.end()) {
		std::fprintf(stderr, "[SourceFactory] color_source_v3 not registered (image-source module missing)\n");
		std::fflush(stderr);
		return nullptr;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "color", static_cast<long long>(colorAbgr)); // 0xAABBGGRR layout
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);

	obs_source_t *src = obs_source_create("color_source_v3", name.c_str(), settings, nullptr);
	obs_data_release(settings);

	if (src)
		obs_source_inc_showing(src);
	return src;
}

namespace {

// Shared config-driven creation core. Consumes (releases) `doc`. See the header note for the
// JSON shape and the ok=false contract (partial sets are released here, never leaked to callers).
SourceConfigResult create_from_config_data(obs_data_t *doc, const char *origin)
{
	SourceConfigResult result;
	if (!doc) {
		result.error = std::string("config parse failed (") + origin + ")";
		std::fprintf(stderr, "[SourceFactory] %s\n", result.error.c_str());
		return result;
	}

	obs_data_set_default_int(doc, "port", 7341);
	result.port = static_cast<int>(obs_data_get_int(doc, "port"));

	const std::set<std::string> registered = SourceFactory::registeredInputIds();

	auto fail = [&](std::string err, obs_data_array_t *arr) {
		result.error = std::move(err);
		std::fprintf(stderr, "[SourceFactory] config error: %s\n", result.error.c_str());
		for (CreatedSource &cs : result.sources) {
			if (cs.showing)
				obs_source_dec_showing(cs.source); // balanced: an inert source took no showing ref
			obs_source_release(cs.source);
		}
		result.sources.clear();
		result.ok = false;
		if (arr)
			obs_data_array_release(arr);
		obs_data_release(doc);
		return result;
	};

	obs_data_array_t *arr = obs_data_get_array(doc, "sources");
	if (!arr || obs_data_array_count(arr) == 0)
		return fail("config has no \"sources\" array (or it is empty)", arr);

	const size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		std::string id = item ? obs_data_get_string(item, "id") : "";
		const std::string name = item ? obs_data_get_string(item, "name") : "";
		const std::string format = item ? obs_data_get_string(item, "format") : "";
		// Optional client-supplied stable id, opaque to the helper -- stored verbatim and echoed
		// back in SourceSummary so a boot/respawned source can be re-adopted across restarts.
		const std::string externalId = item ? obs_data_get_string(item, "externalId") : "";

		if (id.empty() || name.empty()) {
			if (item)
				obs_data_release(item);
			return fail("source entry " + std::to_string(i) + " needs both \"id\" and \"name\"", arr);
		}
		// Optional per-entry sender format (absent == srgb87). Strict validation: an unknown
		// value is a config ERROR, never a silent default (the FleetConfig hard-fail rule).
		if (!format.empty() && format != "srgb87" && format != "linear87" && format != "fp16") {
			obs_data_release(item);
			return fail("source entry '" + name + "' has unknown \"format\" value '" + format +
					    "' (srgb87 | linear87 | fp16)",
				    arr);
		}

		// Optional per-entry audio state (gain / muted / balance / syncOffsetMs), riding
		// beside settings exactly like "format". Strict validation, same hard-fail rule:
		// gain numeric >= 0 (values above the 20.0 ceiling clamp at apply, the wire rule),
		// balance numeric in [0, 1], muted boolean, syncOffsetMs integer >= 0 (values above
		// the 950 ceiling clamp at apply, the wire rule).
		float gain = 1.0f;
		bool muted = false;
		float balance = 0.5f;
		long long syncOffsetMs = 0;
		{
			std::string audioErr;
			if (obs_data_item_t *gi = obs_data_item_byname(item, "gain")) {
				if (obs_data_item_gettype(gi) == OBS_DATA_NUMBER) {
					const double v = obs_data_item_get_double(gi);
					if (v >= 0.0)
						gain = float(v > 20.0 ? 20.0 : v);
					else
						audioErr = "\"gain\" must be a number >= 0";
				} else {
					audioErr = "\"gain\" must be a number >= 0";
				}
				obs_data_item_release(&gi);
			}
			if (audioErr.empty()) {
				if (obs_data_item_t *mi = obs_data_item_byname(item, "muted")) {
					if (obs_data_item_gettype(mi) == OBS_DATA_BOOLEAN)
						muted = obs_data_item_get_bool(mi);
					else
						audioErr = "\"muted\" must be a boolean";
					obs_data_item_release(&mi);
				}
			}
			if (audioErr.empty()) {
				if (obs_data_item_t *bi = obs_data_item_byname(item, "balance")) {
					if (obs_data_item_gettype(bi) == OBS_DATA_NUMBER) {
						const double v = obs_data_item_get_double(bi);
						if (v >= 0.0 && v <= 1.0)
							balance = float(v);
						else
							audioErr = "\"balance\" must be a number in [0, 1]";
					} else {
						audioErr = "\"balance\" must be a number in [0, 1]";
					}
					obs_data_item_release(&bi);
				}
			}
			if (audioErr.empty()) {
				if (obs_data_item_t *si = obs_data_item_byname(item, "syncOffsetMs")) {
					if (obs_data_item_gettype(si) == OBS_DATA_NUMBER) {
						const long long v = obs_data_item_get_int(si);
						if (v >= 0)
							syncOffsetMs = v > 950 ? 950 : v;
						else
							audioErr = "\"syncOffsetMs\" must be an integer >= 0";
					} else {
						audioErr = "\"syncOffsetMs\" must be an integer >= 0";
					}
					obs_data_item_release(&si);
				}
			}
			if (!audioErr.empty()) {
				obs_data_release(item);
				return fail("source entry '" + name + "': " + audioErr, arr);
			}
		}
		// wire->engine: a public wire source-type name (e.g. "color", "window", "display")
		// resolves to its engine id; a raw engine id has no wire match and passes through
		// unchanged. Resolve BEFORE the registration check and the monitor_capture special-cases
		// so registered.find(id), the id=="monitor_capture" seeds, and obs_source_create_private
		// all see the engine id. Engine ids and wire names are disjoint, so passthrough is safe.
		if (auto eng = TypeVocabulary::sourceEngineId(id))
			id = *eng;
		// Registration-check FIRST (invariant 1): an unregistered id would still "create" a
		// 0x0 placeholder source that renders nothing -- that must be a config ERROR.
		if (registered.find(id) == registered.end()) {
			obs_data_release(item);
			return fail("source id '" + id + "' is not registered in the loaded module set", arr);
		}

		obs_data_t *settings = obs_data_get_obj(item, "settings"); // own ref (or null)
		if (!settings)
			settings = obs_data_create();

		// Inert-until-configured: decide from the INCOMING settings whether this entry names a
		// concrete capture target. A fully-configured entry (the managed-helper path, or a reload of
		// a configured source) has its target set -> seed/upgrade/activate exactly as before. A bare
		// standalone "Add Source" entry (camera/display/window with no device/display/window chosen)
		// is left INERT: no default-target seed and no showing ref, so it grabs nothing until the user
		// picks a target (which raises the ref via SetSourceProperties). Types without a targetKey
		// (game/audio/color/...) are always "configured" here -> wholly unchanged.
		const SourceTypeEntry *typeEntry = TypeVocabulary::sourceTypeByEngineId(id);
		const bool targetSet = TypeVocabulary::targetConfigured(typeEntry, settings);

		if (id == "monitor_capture" && targetSet)
			seed_primary_monitor_settings(settings);
		// Camera (win-dshow) holds an EXCLUSIVE capture device. Seed deactivate-when-not-showing
		// at CREATION so the engine closes the device whenever the helper's showing ref drops to
		// zero (the release-when-idle path dec_showing's it). Setting this at creation -- rather
		// than via a later obs_source_update -- avoids the win-dshow "restart capture on ANY
		// settings update" reopen that flickered the LED at the idle transition. Camera-only:
		// the flag is a dshow_input setting and must NOT touch other source types.
		if (id == "dshow_input")
			obs_data_set_bool(settings, "deactivate_when_not_showing", true);

		// PRIVATE source: duplicate display names are legal (sender-name uniqueness is the
		// allocator's job, not source naming's).
		obs_source_t *src = obs_source_create_private(id.c_str(), name.c_str(), settings);
		obs_data_release(settings); // the source context took its OWN ref (invariant 2)
		obs_data_release(item);

		if (!src)
			return fail("obs_source_create_private failed for '" + id + "' ('" + name + "')", arr);

		if (id == "monitor_capture" && targetSet)
			upgrade_primary_monitor_id(src);
		// Factory contract: a source with a SET target is returned showing (owned by the caller). An
		// unset (inert) source carries NO showing ref by design -- it is raised once a target is
		// committed. cs.showing below records which, so teardown stays balanced.
		if (targetSet)
			obs_source_inc_showing(src);

		// Apply the (validated) audio seeds as engine source state at creation -- the same
		// single source of truth the wire verbs read and write. Defaults match the engine's
		// own (1.0 / false / 0.5 / 0), so unconditional application is a no-op for absent
		// keys. The stored sync offset becomes the attach-time delay reserve (the mix engine
		// seeds from it; the store itself is inert for the capture-callback path).
		obs_source_set_volume(src, gain);
		obs_source_set_muted(src, muted);
		obs_source_set_balance_value(src, balance);
		obs_source_set_sync_offset(src, syncOffsetMs * 1000000);

		CreatedSource cs;
		cs.source = src;
		cs.name = name;
		cs.showing = targetSet; // false == created inert (no showing ref); SourceRec mirrors this
		if (!format.empty())
			cs.format = format;
		cs.gain = gain;
		cs.muted = muted;
		cs.balance = balance;
		cs.externalId = externalId;
		result.sources.push_back(cs);
	}

	obs_data_array_release(arr);
	obs_data_release(doc);

	std::printf("[SourceFactory] config (%s): %zu source(s), port %d\n", origin, result.sources.size(),
		    result.port);
	std::fflush(stdout);
	result.ok = true;
	return result;
}

} // namespace

SourceConfigResult SourceFactory::createFromConfigFile(const std::string &path)
{
	return create_from_config_data(obs_data_create_from_json_file(path.c_str()), path.c_str());
}

SourceConfigResult SourceFactory::createFromConfigJson(const char *json)
{
	return create_from_config_data(obs_data_create_from_json(json), "inline json");
}

} // namespace moxrelay
