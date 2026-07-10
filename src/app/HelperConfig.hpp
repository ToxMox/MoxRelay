// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// HelperConfig -- the discovery file clients read to find the helper.
//
// Discovery is via %APPDATA%/MoxRelay/helper-config.json, a BARE single-instance object:
// {instanceId, port, version, fpsTier, spoutPrefix, ownerId, controlToken, sources:[{name}]}.
// A client reads the port + controlToken from this file, opens the loopback WebSocket control
// endpoint, then binds its Spout receivers to the published senders.
//
// This file (a) models the instance identity, (b) serializes it to the canonical JSON object
// (via libobs obs_data, already linked -- no new dependency; obs_data is bmem-only, so it works
// with obs.dll merely LOADED, before/without obs_startup), (c) reports the canonical
// %APPDATA%/MoxRelay/helper-config.json path, and (d) WRITES the file atomically (temp + rename).
// The GUI process is the single writer: it rewrites the file on every engine-state change and
// clears it (writeEmpty -> a bare {} object) on clean shutdown -- stale content therefore only
// survives a crash.

#pragma once

#include <string>
#include <vector>

namespace moxrelay {

// The helper build/protocol version published in every instance entry.
inline constexpr const char *kMoxRelayVersion = "0.21.0-m15";

// This instance's discovery identity.
//
// `sources` carries the FULL ACTUAL Spout sender names (GetName() read-backs, e.g.
// "MACHINE:Helper_7342_CaptureCard"), not bare source names -- collision suffixes ("_2") make the
// bare segment non-derivable, and the actual full name is what a client binds its Spout receiver to.
struct HelperInstance {
	std::string              instanceId;          // stable id for this process (e.g. "tier-60")
	int                      port = 0;            // this instance's loopback WS control port
	std::string              version;             // helper build/protocol version string
	int                      fpsTier = 0;         // the instance's global fps tier (e.g. 60, 240); JSON integer
	std::string              spoutPrefix;         // "{machine}:Helper_{port}" -- shared sender prefix
	std::vector<std::string> sources;             // ACTUAL full sender names bound to this instance
	std::string              ownerId;             // opaque owner token (empty = unowned)
	std::string              controlToken;        // per-launch random auth token; empty = unauthenticated (legacy)
};

class HelperConfig {
public:
	// Serialize the instance to the bare helper-config.json object. Returns a JSON object of the
	// form { instanceId, port, version, fpsTier, spoutPrefix, ownerId, controlToken,
	// sources:[{name}, ...] }. `pretty` selects obs_data_get_json_pretty (multi-line) vs
	// obs_data_get_json (compact).
	static std::string serialize(const HelperInstance &inst, bool pretty = true);

	// Serialize the cleared / no-helper state: a bare empty object {}.
	static std::string serializeEmpty(bool pretty = true);

	// The canonical discovery path: %APPDATA%/MoxRelay/helper-config.json (forward-slash normalized).
	// Falls back to "MoxRelay/helper-config.json" (relative) if %APPDATA% cannot be resolved.
	static std::string canonicalConfigPath();

	// Atomically write the discovery file to the canonical path (write/writeEmpty) or an explicit
	// path (writeTo/writeEmptyTo, used by the self-test so it never touches a live discovery file
	// and by --discovery-path callers). The *Empty variants write the cleared {} object (clean
	// shutdown). Creates the parent directory if missing, writes "<path>.tmp-<pid>", then renames
	// over the target (MoveFileEx REPLACE_EXISTING -- atomic on one volume), so a concurrent reader
	// never sees a torn file. Returns false (with the temp file removed) on any failure.
	static bool write(const HelperInstance &inst);
	static bool writeTo(const std::string &path, const HelperInstance &inst);
	static bool writeEmpty();
	static bool writeEmptyTo(const std::string &path);

private:
	// The shared atomic-write core: temp file + MoveFileEx rename over the target.
	static bool writeJsonTo(const std::string &path, const std::string &json);
};

} // namespace moxrelay
