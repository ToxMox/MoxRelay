// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// DeviceArrivalWatch -- event-driven video-capture device arrival/removal watch that drives automatic
// source recovery. Windows reports a device-interface arrival or removal through a cfgmgr32 callback
// (no HWND, no polling); the callback is a HINT ONLY -- it never inspects hardware or touches engine
// state. Each notification (arbitrary thread) is marshalled to the Qt main thread, where it (re)starts
// a single trailing-debounce timer; on timeout ONE main-thread callback runs the snapshot/diff/self-
// heal pass. A separate one-shot post-start timer takes the initial baseline snapshot independent of
// any device event. The Win32/cfgmgr32 notification handles are held as void* so this header stays
// free of <windows.h> (whose macros clash with Qt's min/max); the .cpp casts them back.

#pragma once

#include <functional>

class QTimer;

namespace moxrelay {

class DeviceArrivalWatch {
public:
	DeviceArrivalWatch() = default;
	~DeviceArrivalWatch();

	DeviceArrivalWatch(const DeviceArrivalWatch &) = delete;
	DeviceArrivalWatch &operator=(const DeviceArrivalWatch &) = delete;

	// Begin watching. `onArrival` runs on the Qt MAIN thread after the debounce settles (and once
	// for the post-start baseline); it performs the snapshot/diff/self-heal pass. Registration failure
	// is logged generically and is NON-FATAL (the helper simply never auto-recovers). Call once, on the
	// Qt main thread, after the app/event loop context exists. The owned timers and OS notification live
	// until the destructor, which unregisters FIRST (draining in-flight callbacks) then stops the timers.
	void start(std::function<void()> onArrival);

	// Called from the cfgmgr32 notification callback on an ARBITRARY thread: marshal a debounced
	// refresh onto the Qt main thread. Public only so the file-local C callback can reach it; not part
	// of the intended API surface.
	void onDeviceEvent();

private:
	std::function<void()> onArrival_;
	void *notifyCamera_ = nullptr;  // HCMNOTIFICATION for the video-camera interface class
	void *notifyCapture_ = nullptr; // HCMNOTIFICATION for the capture interface class
	QTimer *debounce_ = nullptr;    // trailing single-shot debounce (owned; main-thread)
	QTimer *baseline_ = nullptr;    // one-shot post-start baseline poke (owned; main-thread)
};

} // namespace moxrelay
