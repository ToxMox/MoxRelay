// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// DeviceArrivalWatch implementation. <windows.h> + <cfgmgr32.h> are isolated to this translation unit
// (the header keeps the notification handles as void*, so no Win32 surface leaks into the headers).
// The cfgmgr32 callback fires on an arbitrary OS thread and does NOTHING but hop to the Qt main thread
// to restart a debounce timer -- no obs / engine access ever happens off the main thread. All state
// changes flow from the debounced main-thread callback, so an over-triggering notification is harmless.

#include "DeviceArrivalWatch.hpp"

#include <cstdio>

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <windows.h>

#include <cfgmgr32.h>

namespace moxrelay {

namespace {

// Trailing single-shot debounce: coalesce a burst of arrival/removal notifications (a single replug
// emits several across both interface classes) into ONE snapshot/diff/self-heal pass. Restarted per
// notification. The one-shot baseline delay lets the engine + sources settle before the first pass.
constexpr int kDebounceMs = 1500;
constexpr int kBaselineMs = 3000;

// KS device-interface class GUIDs (from ksmedia.h) declared locally so this TU stays free of the heavy
// ks/mmsystem includes. A video-capture device surfaces its interface under BOTH categories depending
// on the driver; registering for both is harmless -- the notifications are only hints and the debounced
// enumeration diff is the ground truth, so duplicate/over-triggering notifications cost nothing.
const GUID kKsCategoryVideoCamera = {
	0xe5323777, 0xf976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}};
const GUID kKsCategoryCapture = {
	0x65e8773d, 0x8f56, 0x11d0, {0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};

// cfgmgr32 notification callback (arbitrary thread). HINT-ONLY: on an interface arrival OR removal it
// just pokes the watch; it never reads the event payload and never touches obs/engine state. Returns
// CR_SUCCESS as required for a device-interface notification.
DWORD CALLBACK deviceNotifyCallback(HCMNOTIFICATION /*hNotify*/, PVOID context, CM_NOTIFY_ACTION action,
				    PCM_NOTIFY_EVENT_DATA /*eventData*/, DWORD /*eventDataSize*/)
{
	if (context && (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
			action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)) {
		static_cast<DeviceArrivalWatch *>(context)->onDeviceEvent();
	}
	return CR_SUCCESS;
}

} // namespace

void DeviceArrivalWatch::onDeviceEvent()
{
	// Arbitrary-thread entry. Hop to the Qt main thread (where the timers live) and (re)start the
	// trailing debounce. A hop landing after event-loop exit is simply never processed (teardown runs
	// post-exec()), so it can never touch a half-torn-down timer.
	QMetaObject::invokeMethod(
		QCoreApplication::instance(),
		[this] {
			if (debounce_)
				debounce_->start(kDebounceMs);
		},
		Qt::QueuedConnection);
}

void DeviceArrivalWatch::start(std::function<void()> onArrival)
{
	onArrival_ = std::move(onArrival);

	// Owned QTimers, created on the Qt main thread (this runs there). Both single-shot; both fire the
	// same snapshot/diff/self-heal callback. The debounce coalesces device-event bursts; the baseline
	// establishes the reference snapshot (and heals any already-frameless source) once shortly after boot.
	debounce_ = new QTimer();
	debounce_->setSingleShot(true);
	QObject::connect(debounce_, &QTimer::timeout, debounce_, [this] {
		if (onArrival_)
			onArrival_();
	});

	baseline_ = new QTimer();
	baseline_->setSingleShot(true);
	QObject::connect(baseline_, &QTimer::timeout, baseline_, [this] {
		if (onArrival_)
			onArrival_();
	});
	baseline_->start(kBaselineMs);

	// Register for device-interface arrival/removal on BOTH capture categories (cfgmgr32; callback-
	// based, no HWND, no polling). Failure is non-fatal: log generically and leave auto-recovery off.
	CM_NOTIFY_FILTER filter = {};
	filter.cbSize = sizeof(filter);
	filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;

	filter.u.DeviceInterface.ClassGuid = kKsCategoryVideoCamera;
	HCMNOTIFICATION hCamera = nullptr;
	const CONFIGRET rCamera = CM_Register_Notification(&filter, this, deviceNotifyCallback, &hCamera);
	if (rCamera == CR_SUCCESS)
		notifyCamera_ = hCamera;

	filter.u.DeviceInterface.ClassGuid = kKsCategoryCapture;
	HCMNOTIFICATION hCapture = nullptr;
	const CONFIGRET rCapture = CM_Register_Notification(&filter, this, deviceNotifyCallback, &hCapture);
	if (rCapture == CR_SUCCESS)
		notifyCapture_ = hCapture;

	if (!notifyCamera_ && !notifyCapture_) {
		std::fprintf(stdout,
			     "[DeviceArrivalWatch] device-notification registration failed (cam=0x%lx "
			     "cap=0x%lx) -- automatic source recovery on device arrival is disabled\n",
			     (unsigned long)rCamera, (unsigned long)rCapture);
		std::fflush(stdout);
	}
}

DeviceArrivalWatch::~DeviceArrivalWatch()
{
	// Unregister FIRST: CM_Unregister_Notification blocks until any in-flight callback drains, so no
	// callback can still be running once these return. Only then destroy the timers the callback path
	// marshals onto. (After app.exec() has returned the marshalled pokes are inert anyway -- the event
	// loop never processes them -- but draining before teardown keeps the ordering strict.)
	if (notifyCamera_) {
		CM_Unregister_Notification(static_cast<HCMNOTIFICATION>(notifyCamera_));
		notifyCamera_ = nullptr;
	}
	if (notifyCapture_) {
		CM_Unregister_Notification(static_cast<HCMNOTIFICATION>(notifyCapture_));
		notifyCapture_ = nullptr;
	}
	delete debounce_;
	debounce_ = nullptr;
	delete baseline_;
	baseline_ = nullptr;
}

} // namespace moxrelay
