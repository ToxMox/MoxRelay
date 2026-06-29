// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ParentWatch -- managed-mode owner-process death switch. When the helper is launched with a known
// owner PID it opens a SYNCHRONIZE handle to that process and a watcher thread blocks on it; the
// instant the owner exits (clean exit, crash, or kill) the watcher schedules the helper's normal
// graceful quit on the Qt event loop -- immediate, no grace period. Standalone launches (no owner
// PID) never construct a started watcher. The Win32 handles are held as void* so this header stays
// free of <windows.h> (whose macros would clash with Qt's min/max); the .cpp casts them back.

#pragma once

#include <thread>

namespace moxrelay {

class ParentWatch {
public:
	ParentWatch() = default;
	~ParentWatch();

	ParentWatch(const ParentWatch &) = delete;
	ParentWatch &operator=(const ParentWatch &) = delete;

	// Begin watching the process with the given PID. If the process cannot be opened a generic
	// warning is logged and NO thread is started (the host-side reap remains the backstop); the
	// helper is NEVER self-exited on that failure. Call once.
	void start(unsigned long ownerPid);

private:
	void *process_ = nullptr;   // HANDLE to the watched owner process (SYNCHRONIZE)
	void *stopEvent_ = nullptr; // HANDLE to the manual-reset stop event
	std::thread thread_;
};

} // namespace moxrelay
