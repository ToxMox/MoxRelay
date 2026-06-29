// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// TypeVocabulary -- the control API's published vocabulary, mapped to the engine's internal ids.
//
// The wire contract (docs/control-api.asyncapi.yaml) speaks MoxRelay's OWN names: source types
// (camera/display/window/...), filter types (color_correction/chroma_key/...), and settings keys.
// This is the ONLY file that knows both vocabularies; nothing else may put an internal engine id
// on the wire or accept one from it.
//
// Settings keys are published verbatim in contract v1, but EVERY key crossing the wire passes
// through the overlay functions below (identity today, in both directions), so a future key
// rename is a one-table change here with zero impact on the contract mechanism or any caller.

#pragma once

#include <nlohmann/json.hpp>

#include <obs.h>

#include <optional>
#include <string>
#include <vector>

namespace moxrelay {

struct SourceTypeEntry {
	const char *wireName;      // contract vocabulary, e.g. "display"
	const char *engineId;      // internal source id used for CREATION (version-pinned where versioned)
	const char *unversionedId; // internal UNVERSIONED id used for REVERSE lookup on live sources
	const char *label;         // human-readable label published by ListSourceTypes
	// Inert-until-configured (standalone "Add Source"): the PRIMARY target setting key whose value
	// names what the source captures (camera device / display / window). nullptr = this type is
	// NEVER gated inert -- it captures safely with its built-in default (game any_fullscreen, audio
	// Default device, color/text/image/media). `targetUnset` is an extra "unset" sentinel beyond
	// the empty string (monitor_capture's built-in "DUMMY"); nullptr means only the empty string
	// counts as unset. `targetPlaceholder` is the type-aware combo prompt shown until a target is
	// picked. The three default to "ungated" so the seven non-capture types need no extra columns.
	const char *targetKey = nullptr;
	const char *targetUnset = nullptr;
	const char *targetPlaceholder = nullptr;
};

struct FilterTypeEntry {
	const char *wireName; // contract vocabulary, e.g. "color_correction"
	const char *engineId; // internal filter id
	const char *kind;     // "video" | "audio" (contract FilterKind)
	const char *label;    // human-readable label published by ListAvailableFilters
};

class TypeVocabulary {
public:
	// The full closed v1 tables (contract order). Availability is still checked at runtime --
	// callers must intersect with the registered set, never assume the table is live.
	static const std::vector<SourceTypeEntry> &sourceTypes();
	static const std::vector<FilterTypeEntry> &filterTypes();

	// Source-type lookups. Empty optional = not part of the contract vocabulary. The reverse
	// lookup takes the UNVERSIONED id (versioned engine types share one base id).
	static std::optional<std::string> sourceEngineId(const std::string &wireName);
	static std::optional<std::string> sourceWireName(const std::string &unversionedId);

	// Full type entry by its CREATION engine id (e.g. "monitor_capture"); nullptr if the id is
	// outside the contract vocabulary. Drives the inert-until-configured gate.
	static const SourceTypeEntry *sourceTypeByEngineId(const std::string &engineId);

	// True iff `settings` carries a CONFIGURED (non-default) primary target for `entry`'s type.
	// A null entry, or a type with no targetKey, is ALWAYS configured (never gated inert). The
	// per-type unset rule lives here: empty string is always unset; monitor_capture also treats
	// its built-in "DUMMY" as unset. Used by both the factory gate and the SetSourceProperties
	// reactivation hook, so the "what counts as a target" rule lives in exactly one place.
	static bool targetConfigured(const SourceTypeEntry *entry, obs_data_t *settings);

	// Filter-type lookups.
	static const FilterTypeEntry *filterByWireName(const std::string &wireName);
	static const FilterTypeEntry *filterByEngineId(const std::string &engineId);

	// ---- settings-key overlay (identity today; the rename seam) ----
	// Translate one key. `wireType` is the contract source/filter type the key belongs to
	// (reserved for per-type overlays; unused while the overlay is identity).
	static std::string keyWireToEngine(const std::string &wireType, const std::string &key);
	static std::string keyEngineToWire(const std::string &wireType, const std::string &key);

	// Translate a whole settings object, key by key, through the overlay.
	static nlohmann::json settingsWireToEngine(const std::string &wireType, const nlohmann::json &settings);
	static nlohmann::json settingsEngineToWire(const std::string &wireType, const nlohmann::json &settings);

	// ---- property descriptors (contract PropertyDescriptor) ----
	// Walk an obs_properties_t and produce the contract descriptor array. Group properties are
	// flattened; property types outside the contract's nine are omitted. Keys pass through the
	// engine->wire overlay. The caller owns `props` (this never destroys it).
	static nlohmann::json describeProperties(obs_properties_t *props, const std::string &wireType);
};

} // namespace moxrelay
