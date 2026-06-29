// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioDeviceNotify -- system default-render-endpoint change notifications (IMMNotificationClient).
//
// The render mixer follows the SYSTEM DEFAULT endpoint live while its stored selection is the
// "default" sentinel; this object delivers the trigger. The callback fires on a COM worker
// thread and MUST be cheap and non-blocking (the consumer sets a flag and wakes its own thread;
// nothing here may touch device I/O or take long-held locks).
//
// Registration is best-effort: when the enumerator cannot be created the object is inert
// (isActive() false) and default-follow degrades to the reactive reconnect path.

#pragma once

#include <functional>

struct IMMDeviceEnumerator;

namespace moxrelay {

class AudioDeviceNotify {
public:
	// `onDefaultRenderChanged` fires when the system DEFAULT RENDER endpoint (console role)
	// changes. Registered for the object's lifetime; unregistered in the destructor.
	explicit AudioDeviceNotify(std::function<void()> onDefaultRenderChanged);
	~AudioDeviceNotify();

	AudioDeviceNotify(const AudioDeviceNotify &) = delete;
	AudioDeviceNotify &operator=(const AudioDeviceNotify &) = delete;

	bool isActive() const { return enumerator_ != nullptr; }

private:
	class Client; // the IMMNotificationClient implementation (COM refcounted)

	IMMDeviceEnumerator *enumerator_ = nullptr;
	Client *client_ = nullptr;
	bool comBalanced_ = false; // we performed the CoInitializeEx we must balance
};

} // namespace moxrelay
