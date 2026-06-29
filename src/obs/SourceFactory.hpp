// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SourceFactory -- programmatic libobs source creation (R4: extracted from main.cpp).
//
// Two invariants every creation path here enforces:
//   1. REGISTRATION-CHECK FIRST via obs_enum_input_types. obs_source_create returns a NON-NULL
//      PLACEHOLDER for an unregistered id (libobs keeps unknown sources so scenes round-trip), so
//      a null-return check is a false negative -- the placeholder reports 0x0 and renders nothing.
//   2. obs_data_release(settings) after create -- the source context takes its OWN ref; a leaked
//      obs_data is NOT freed by obs_shutdown (only visible via bnum_allocs).
//
// Sources are returned with obs_source_inc_showing already applied (capture sources free their
// capture state when not showing -- duplicator hard-gates its tick on obs_source_showing). The
// caller owns the returned strong ref and must obs_source_dec_showing + obs_source_release it.
//
// M2.2 adds CONFIG-DRIVEN creation: a JSON document (parsed with libobs obs_data -- no new
// dependency; the HelperConfig precedent) describing N sources, each created as a PRIVATE source
// (obs_source_create_private: not entered into the global source-name registry, so duplicate
// display names are legal -- the sender-name collision is handled by the SenderNameAllocator,
// not by source naming). Shape:
//
//   { "port": 7341,
//     "sources": [ { "id": "color_source_v3", "name": "ColorA",
//                    "settings": { "color": 4280303808, "width": 640, "height": 360 },
//                    "format": "srgb87" }, ... ] }
//
// "settings" is passed VERBATIM as the source's obs_data settings (no per-type translation
// layer); a monitor_capture entry without an explicit "monitor_id" is seeded with the primary
// display exactly like the M1 test source. "format" (optional) selects the Spout sender pixel
// format -- srgb87 (default) | linear87 | fp16 -- and rides beside the settings, never inside
// them. Full config persistence/management is M6.

#pragma once

#include <obs.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace moxrelay {

// One created source: a strong ref (showing already incremented) + the display name used both in
// the Sources UI and as the {source} segment of the Spout sender name. `format` is the sender
// pixel format from the optional per-entry "format" key (srgb87 | linear87 | fp16; absent ==
// srgb87) -- carried BESIDE the verbatim settings, never through them (a settings-key format
// would be wire-visible through the wrong door and 1006-rejected there by design). The optional
// per-entry audio keys (gain / muted / balance) ride beside settings the same way and are
// APPLIED at creation (engine source state); the fields here echo what was applied.
struct CreatedSource {
	obs_source_t *source = nullptr;
	std::string name;
	std::string format = "srgb87";
	float gain = 1.0f;
	bool muted = false;
	float balance = 0.5f;
	// True when the factory raised the source's showing ref at creation -- a configured target, or
	// a type that is never gated inert. False == created INERT (a standalone "Add Source" with no
	// target chosen): NO showing ref was taken, so the caller must NOT dec_showing it, and the
	// owning SourceRec's deviceShowing must start false. The ref is raised later, once a target is
	// committed (SetSourceProperties). Defaults true so every non-config creation path is unchanged.
	bool showing = true;
	// Optional client-supplied stable id from a per-entry "externalId" string -- opaque to the
	// helper (never interpreted), carried beside settings like "format"; echoed back in the
	// source's SourceSummary so a control client can re-adopt the same source across restarts.
	std::string externalId;
};

// Result of a config load: every source created (in document order) or ok=false with `error`
// naming the first failure -- a config that references an unregistered source id is an ERROR,
// never a silent skip (a worker booting "green" with missing sources is the M2.3 hard-fail case).
struct SourceConfigResult {
	bool ok = false;
	std::string error;
	int port = 7341;
	std::vector<CreatedSource> sources;
};

class SourceFactory {
public:
	// The set of input source ids ACTUALLY REGISTERED in the loaded module set (invariant 1).
	static std::set<std::string> registeredInputIds();

	// A solid-color source ("color_source_v3" -- the bare "color_source" id is the OBSOLETE 400x400
	// v1; ids are version-suffixed) for deterministic self-test / cross-process pixel assertions.
	// `colorAbgr` is the color_source "color" setting layout: 0xAABBGGRR. Returns nullptr if the
	// id is not registered (image-source module missing). Returned showing + owned by the caller.
	static obs_source_t *createColorSource(const std::string &name, uint32_t colorAbgr, int width,
					       int height);

	// Config-driven creation (M2.2; header note above for the JSON shape). On ok=false the
	// partial set has already been released -- callers never clean up a failed load. The caller
	// owns every returned source (showing + strong ref each, factory contract).
	static SourceConfigResult createFromConfigFile(const std::string &path);
	static SourceConfigResult createFromConfigJson(const char *json);
};

} // namespace moxrelay
