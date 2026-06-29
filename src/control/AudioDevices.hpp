// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioDevices -- system audio endpoint enumeration for the ListAudioDevices verb
// (docs/control-api.asyncapi.yaml). Pure WASAPI/MMDevice (IMMDeviceEnumerator); no engine
// involvement. Returned ids are the system endpoint ids usable as device-selection settings
// values for audio_input / audio_output sources.

#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace moxrelay {

class AudioDevices {
public:
	// Enumerate ACTIVE endpoints. `flow` is "render", "capture", or "" (both). Returns the
	// contract's devices array: [{id, name, flow, isDefault}]. Failures degrade to an empty
	// array (enumeration is best-effort; an error never faults the instance).
	static nlohmann::json enumerate(const std::string &flow);

	// Whether `deviceId` names an ACTIVE render endpoint ("default" is always known -- it is
	// the sentinel, not an endpoint id). The SetAudioOutputDevice unknown-id (1006) check.
	static bool renderDeviceExists(const std::string &deviceId);

	// Synchronous shared-mode open probe of a render endpoint ("default" probes the system
	// default): activate an audio client, negotiate the mix format, initialize, release.
	// Proves the endpoint can actually be opened (the 1005 check) without touching the
	// mixer's own client -- shared mode admits any number of concurrent clients.
	static bool probeOpenRender(const std::string &deviceId);
};

} // namespace moxrelay
