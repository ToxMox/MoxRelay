// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// TypeVocabulary implementation. See TypeVocabulary.hpp for the contract role of this file.

#include "TypeVocabulary.hpp"

#include <cstring>

namespace moxrelay {

const std::vector<SourceTypeEntry> &TypeVocabulary::sourceTypes()
{
	// Contract v1 closed set (docs/control-api.asyncapi.yaml, SourceType schema), contract order.
	static const std::vector<SourceTypeEntry> kTable = {
		{"camera", "dshow_input", "dshow_input", "Camera",
		 /*targetKey=*/"video_device_id", /*targetUnset=*/nullptr, "Select a camera..."},
		{"display", "monitor_capture", "monitor_capture", "Display",
		 /*targetKey=*/"monitor_id", /*targetUnset=*/"DUMMY", "Select a display..."},
		{"window", "window_capture", "window_capture", "Window",
		 /*targetKey=*/"window", /*targetUnset=*/nullptr, "Select a window..."},
		{"game", "game_capture", "game_capture", "Game"},
		{"media", "ffmpeg_source", "ffmpeg_source", "Media"},
		{"image", "image_source", "image_source", "Image"},
		{"color", "color_source_v3", "color_source", "Color"},
		{"text", "text_gdiplus_v3", "text_gdiplus", "Text"},
		{"audio_input", "wasapi_input_capture", "wasapi_input_capture", "Audio Input"},
		{"audio_output", "wasapi_output_capture", "wasapi_output_capture", "Audio Output"},
	};
	return kTable;
}

const std::vector<FilterTypeEntry> &TypeVocabulary::filterTypes()
{
	// Contract v1 closed set: 13 video + 9 audio. Engine ids are the REGISTERED ids: a filter
	// registered with a `version` field gets `_v{N}` appended at registration (the plain id
	// remains registered as the OBSOLETE v1) -- so the two-version filters must be created via
	// their `_v2` ids. ListAvailableFilters still intersects with the runtime-registered ids.
	static const std::vector<FilterTypeEntry> kTable = {
		{"color_correction", "color_filter_v2", "video", "Color Correction"},
		{"chroma_key", "chroma_key_filter_v2", "video", "Chroma Key"},
		{"color_key", "color_key_filter_v2", "video", "Color Key"},
		{"luma_key", "luma_key_filter_v2", "video", "Luma Key"},
		{"crop", "crop_filter", "video", "Crop/Pad"},
		{"mask", "mask_filter_v2", "video", "Image Mask/Blend"},
		{"lut", "clut_filter", "video", "Apply LUT"},
		{"scale", "scale_filter", "video", "Scaling/Aspect Ratio"},
		{"scroll", "scroll_filter", "video", "Scroll"},
		{"sharpness", "sharpness_filter_v2", "video", "Sharpen"},
		{"render_delay", "gpu_delay", "video", "Render Delay"},
		{"video_delay", "async_delay_filter", "video", "Video Delay (Async)"},
		{"hdr_tonemap", "hdr_tonemap_filter", "video", "HDR Tone Mapping"},
		{"gain", "gain_filter", "audio", "Gain"},
		{"compressor", "compressor_filter", "audio", "Compressor"},
		{"upward_compressor", "upward_compressor_filter", "audio", "Upward Compressor"},
		{"expander", "expander_filter", "audio", "Expander"},
		{"limiter", "limiter_filter", "audio", "Limiter"},
		{"noise_gate", "noise_gate_filter", "audio", "Noise Gate"},
		{"noise_suppress", "noise_suppress_filter_v2", "audio", "Noise Suppression"},
		{"eq_3band", "basic_eq_filter", "audio", "3-Band Equalizer"},
		{"invert_polarity", "invert_polarity_filter", "audio", "Invert Polarity"},
	};
	return kTable;
}

std::optional<std::string> TypeVocabulary::sourceEngineId(const std::string &wireName)
{
	for (const auto &e : sourceTypes()) {
		if (wireName == e.wireName)
			return std::string(e.engineId);
	}
	return std::nullopt;
}

std::optional<std::string> TypeVocabulary::sourceWireName(const std::string &unversionedId)
{
	for (const auto &e : sourceTypes()) {
		if (unversionedId == e.unversionedId)
			return std::string(e.wireName);
	}
	return std::nullopt;
}

const SourceTypeEntry *TypeVocabulary::sourceTypeByEngineId(const std::string &engineId)
{
	for (const auto &e : sourceTypes()) {
		if (engineId == e.engineId)
			return &e;
	}
	return nullptr;
}

bool TypeVocabulary::targetConfigured(const SourceTypeEntry *entry, obs_data_t *settings)
{
	if (!entry || !entry->targetKey)
		return true; // type is never gated inert -> always "configured"
	if (!settings)
		return false;
	const char *v = obs_data_get_string(settings, entry->targetKey);
	if (!v || !*v)
		return false; // empty string == unset for every gated type
	if (entry->targetUnset && std::strcmp(v, entry->targetUnset) == 0)
		return false; // built-in default sentinel (monitor_capture "DUMMY")
	return true;
}

const FilterTypeEntry *TypeVocabulary::filterByWireName(const std::string &wireName)
{
	for (const auto &e : filterTypes()) {
		if (wireName == e.wireName)
			return &e;
	}
	return nullptr;
}

const FilterTypeEntry *TypeVocabulary::filterByEngineId(const std::string &engineId)
{
	for (const auto &e : filterTypes()) {
		if (engineId == e.engineId)
			return &e;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Settings-key overlay. IDENTITY in contract v1: the published keys ARE the engine keys (audited
// -- the v1 surface is generic English). The seam stays load-bearing: every settings object in
// ControlVerbs round-trips through these, so a rename is added HERE (one table) and nowhere else.
// ---------------------------------------------------------------------------------------------

std::string TypeVocabulary::keyWireToEngine(const std::string &wireType, const std::string &key)
{
	(void)wireType;
	return key;
}

std::string TypeVocabulary::keyEngineToWire(const std::string &wireType, const std::string &key)
{
	(void)wireType;
	return key;
}

nlohmann::json TypeVocabulary::settingsWireToEngine(const std::string &wireType, const nlohmann::json &settings)
{
	nlohmann::json out = nlohmann::json::object();
	if (!settings.is_object())
		return out;
	for (auto it = settings.begin(); it != settings.end(); ++it)
		out[keyWireToEngine(wireType, it.key())] = it.value();
	return out;
}

nlohmann::json TypeVocabulary::settingsEngineToWire(const std::string &wireType, const nlohmann::json &settings)
{
	nlohmann::json out = nlohmann::json::object();
	if (!settings.is_object())
		return out;
	for (auto it = settings.begin(); it != settings.end(); ++it)
		out[keyEngineToWire(wireType, it.key())] = it.value();
	return out;
}

// ---------------------------------------------------------------------------------------------
// Property descriptors (contract PropertyDescriptor; the nine v1 types). Groups are flattened;
// types outside the contract (editable lists, frame-rate composites, ...) are omitted -- the
// contract allows additional types only via a revision, never ad hoc.
// ---------------------------------------------------------------------------------------------

namespace {

void describeInto(obs_properties_t *props, const std::string &wireType, nlohmann::json &out)
{
	// Inert-until-configured: resolve this type's primary target key (if any) so the matching
	// property descriptor can be flagged "isTarget" with its type-aware placeholder. The UI uses
	// the flag to show a blank "Select a <type>..." picker and to NOT auto-commit a default.
	const SourceTypeEntry *typeEntry = nullptr;
	if (auto eng = TypeVocabulary::sourceEngineId(wireType))
		typeEntry = TypeVocabulary::sourceTypeByEngineId(*eng);
	const char *targetKey = typeEntry ? typeEntry->targetKey : nullptr;
	const char *targetPlaceholder = typeEntry ? typeEntry->targetPlaceholder : nullptr;

	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		const enum obs_property_type type = obs_property_get_type(p);

		if (type == OBS_PROPERTY_GROUP) {
			if (obs_properties_t *content = obs_property_group_content(p))
				describeInto(content, wireType, out);
			continue;
		}

		const char *name = obs_property_name(p);
		const char *label = obs_property_description(p);
		if (!name || !*name)
			continue;

		nlohmann::json d = {
			{"name", TypeVocabulary::keyEngineToWire(wireType, name)},
			{"label", label ? label : ""},
			{"visible", obs_property_visible(p)},
			{"enabled", obs_property_enabled(p)},
		};

		// Flag the PRIMARY target property (camera device / display / window) so the UI renders a
		// blank type-aware placeholder for it and never auto-commits a default. `name` is the engine
		// key; `targetKey` is the same engine key (the wire overlay is identity in v1).
		if (targetKey && std::strcmp(name, targetKey) == 0) {
			d["isTarget"] = true;
			if (targetPlaceholder && *targetPlaceholder)
				d["placeholder"] = targetPlaceholder;
		}

		switch (type) {
		case OBS_PROPERTY_BOOL:
			d["type"] = "bool";
			break;
		case OBS_PROPERTY_INT:
			d["type"] = "int";
			d["min"] = obs_property_int_min(p);
			d["max"] = obs_property_int_max(p);
			d["step"] = obs_property_int_step(p);
			d["display"] = obs_property_int_type(p) == OBS_NUMBER_SLIDER ? "slider" : "scroller";
			break;
		case OBS_PROPERTY_FLOAT:
			d["type"] = "float";
			d["min"] = obs_property_float_min(p);
			d["max"] = obs_property_float_max(p);
			d["step"] = obs_property_float_step(p);
			d["display"] = obs_property_float_type(p) == OBS_NUMBER_SLIDER ? "slider" : "scroller";
			break;
		case OBS_PROPERTY_TEXT: {
			d["type"] = "text";
			switch (obs_property_text_type(p)) {
			case OBS_TEXT_PASSWORD:
				d["textMode"] = "password";
				break;
			case OBS_TEXT_MULTILINE:
				d["textMode"] = "multiline";
				break;
			case OBS_TEXT_INFO:
				d["textMode"] = "info";
				break;
			default:
				d["textMode"] = "default";
				break;
			}
			break;
		}
		case OBS_PROPERTY_PATH: {
			d["type"] = "path";
			d["pathMode"] = obs_property_path_type(p) == OBS_PATH_DIRECTORY ? "directory" : "file";
			if (const char *filter = obs_property_path_filter(p); filter && *filter)
				d["pathFilter"] = filter;
			break;
		}
		case OBS_PROPERTY_LIST: {
			d["type"] = "list";
			const enum obs_combo_format format = obs_property_list_format(p);
			switch (format) {
			case OBS_COMBO_FORMAT_INT:
				d["listFormat"] = "int";
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				d["listFormat"] = "float";
				break;
			case OBS_COMBO_FORMAT_BOOL:
				d["listFormat"] = "bool";
				break;
			default:
				d["listFormat"] = "string";
				break;
			}
			d["listEditable"] = obs_property_list_type(p) == OBS_COMBO_TYPE_EDITABLE;
			nlohmann::json items = nlohmann::json::array();
			const size_t n = obs_property_list_item_count(p);
			for (size_t i = 0; i < n; ++i) {
				nlohmann::json item;
				const char *itemName = obs_property_list_item_name(p, i);
				item["label"] = itemName ? itemName : "";
				switch (format) {
				case OBS_COMBO_FORMAT_INT:
					item["value"] = obs_property_list_item_int(p, i);
					break;
				case OBS_COMBO_FORMAT_FLOAT:
					item["value"] = obs_property_list_item_float(p, i);
					break;
				case OBS_COMBO_FORMAT_BOOL:
					item["value"] = obs_property_list_item_bool(p, i);
					break;
				default: {
					const char *v = obs_property_list_item_string(p, i);
					item["value"] = v ? v : "";
					break;
				}
				}
				item["disabled"] = obs_property_list_item_disabled(p, i);
				items.push_back(std::move(item));
			}
			d["items"] = std::move(items);
			break;
		}
		case OBS_PROPERTY_COLOR:
			d["type"] = "color";
			d["hasAlpha"] = false;
			break;
		case OBS_PROPERTY_COLOR_ALPHA:
			d["type"] = "color";
			d["hasAlpha"] = true;
			break;
		case OBS_PROPERTY_FONT:
			d["type"] = "font";
			break;
		case OBS_PROPERTY_BUTTON:
			d["type"] = "button";
			break;
		default:
			// Outside the contract's nine types (editable list, frame rate, ...): omit.
			continue;
		}

		out.push_back(std::move(d));
	}
}

} // namespace

nlohmann::json TypeVocabulary::describeProperties(obs_properties_t *props, const std::string &wireType)
{
	nlohmann::json out = nlohmann::json::array();
	if (props)
		describeInto(props, wireType, out);
	return out;
}

} // namespace moxrelay
