// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ProfileStore -- the standalone profiles on-disk layer (item 05).
//
// A profile is a saved scene: sources + verbatim per-source settings + the per-source audio quad +
// a persisted filter chain + a per-profile fps tier + the audio output device + window layout. Each
// profile is one JSON file under %APPDATA%/MoxRelay/profiles/<name>.json (ROAMING -- the same
// MoxRelay-branded %APPDATA%/MoxRelay namespace as the discovery file and MoxRelay.ini, NOT the
// %LOCALAPPDATA%/MoxRelay module-cache dir). A separate %APPDATA%/MoxRelay/state.json carries the
// last-loaded-profile pointer for auto-load-last on standalone launch.
//
// This layer is STORAGE ONLY: it resolves paths, validates profile names, atomically writes (temp +
// rename, reusing the HelperConfig idiom), reads, lists, and deletes profile JSON files, and reads/
// writes the last-profile pointer. The profile JSON's CONTENT (the source/filter/audio snapshot and
// its replay) is owned by ControlVerbs. Nothing here references any host application -- every path
// segment, file name, and key is generic / MoxRelay-branded.

#pragma once

#include <string>
#include <vector>

namespace moxrelay {

class ProfileStore {
public:
	// The profiles directory: %APPDATA%/MoxRelay/profiles (forward-slash normalized, NO trailing
	// slash). Falls back to "MoxRelay/profiles" (relative) if %APPDATA% cannot be resolved. Does NOT
	// create the directory (the atomic write creates it on first save).
	static std::string profilesDir();

	// The full path for one profile: <profilesDir>/<name>.json. `name` is the bare profile name
	// (no extension, no path). Returns "" if the name is not a valid profile name (see isValidName).
	static std::string profilePath(const std::string &name);

	// The last-profile pointer file: %APPDATA%/MoxRelay/state.json.
	static std::string statePath();

	// A profile name is valid iff it is non-empty and contains no path separators, drive markers, or
	// other characters that could escape the profiles directory or collide with the filesystem. This
	// is the gate every name-taking entry point applies before touching disk.
	static bool isValidName(const std::string &name);

	// The names of all profiles currently on disk (the <name> of each <name>.json), sorted
	// case-insensitively. Missing directory -> empty list (not an error).
	static std::vector<std::string> listProfiles();

	// True if <name>.json exists on disk.
	static bool exists(const std::string &name);

	// Read a profile's raw JSON text. Returns true + fills `out` on success; false if the name is
	// invalid or the file is missing/unreadable.
	static bool read(const std::string &name, std::string &out);

	// Atomically write a profile's JSON text (temp + rename, creates the profiles dir if missing).
	// False on an invalid name or any write failure.
	static bool write(const std::string &name, const std::string &json);

	// Delete a profile file. Returns true if the file was removed (or was already absent); false on
	// an invalid name or a delete failure.
	static bool remove(const std::string &name);

	// The last-loaded profile name (state.json "lastProfile"), or "" if none / unreadable.
	static std::string lastProfile();

	// Persist the last-loaded profile pointer (atomic write of state.json). "" clears it. False on a
	// write failure.
	static bool setLastProfile(const std::string &name);
};

} // namespace moxrelay
