// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// RendezvousPipe -- a file-free handoff for the managed instance. Instead of writing the discovery
// file, the helper connects as a CLIENT to a named pipe created by the launcher (the pipe SERVER)
// and writes the serialized HelperInstance JSON over it once, at startup, after the control port is
// bound. The pipe NAME is supplied by the launcher via --rendezvous-pipe (a non-secret per-launch
// value passed on the command line); this file hard-codes nothing about it.
//
// Standalone mode never uses this path (it keeps writing %APPDATA%/MoxRelay/helper-config.json).

#pragma once

#include <string>

namespace moxrelay {

class RendezvousPipe {
public:
	// Connect to the launcher's named pipe \\.\pipe\<pipeName> and write `json` over it once.
	// Retries briefly on ERROR_PIPE_BUSY (a startup race with the server). Returns false (and logs
	// once) on any failure; the caller degrades exactly as it would for a failed discovery-file write.
	static bool writeInstance(const std::string &pipeName, const std::string &json);
};

} // namespace moxrelay
