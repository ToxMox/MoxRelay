// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ParentWatch implementation. <windows.h> is isolated to this translation unit (the header keeps
// the Win32 handles as void*, so no Win32 surface leaks into the headers). The watcher thread
// blocks on BOTH the owner process handle AND a manual-reset stop event, so normal shutdown can
// wake and join the thread before QApplication is destroyed -- the thread never outlives the app.

#include "ParentWatch.hpp"

#include <cstdio>

#include <QCoreApplication>

#include <thread>

#include <windows.h>

namespace moxrelay {

void ParentWatch::start(unsigned long ownerPid)
{
	// Manual-reset event so the destructor can wake the (currently blocked) watcher for a clean join.
	stopEvent_ = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
	process_ = ::OpenProcess(SYNCHRONIZE, FALSE, ownerPid);
	if (!process_) {
		// Could not open the owning process: log generically and do NOT start the thread. We never
		// self-exit on this -- the host-side reap remains the backstop.
		std::fprintf(stdout,
			     "[ParentWatch] could not open the owning process (pid=%lu, err=%lu) -- "
			     "not watching; the host-side reap remains the backstop\n",
			     ownerPid, ::GetLastError());
		std::fflush(stdout);
		return;
	}

	thread_ = std::thread([this] {
		HANDLE waits[2] = {(HANDLE)process_, (HANDLE)stopEvent_};
		DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		if (r == WAIT_OBJECT_0) {
			// The owning process exited/crashed/was killed: run the helper's normal graceful
			// quit on the event loop -- the exact primitive the host Shutdown verb uses.
			QMetaObject::invokeMethod(
				QCoreApplication::instance(), [] { QCoreApplication::quit(); },
				Qt::QueuedConnection);
			return;
		}
		// r == WAIT_OBJECT_0 + 1 (the stop event, signalled during normal shutdown): return quietly.
	});
}

ParentWatch::~ParentWatch()
{
	if (stopEvent_)
		::SetEvent((HANDLE)stopEvent_);
	if (thread_.joinable())
		thread_.join();
	if (process_)
		::CloseHandle((HANDLE)process_);
	if (stopEvent_)
		::CloseHandle((HANDLE)stopEvent_);
}

} // namespace moxrelay
