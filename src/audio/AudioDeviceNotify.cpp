// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioDeviceNotify implementation. A minimal IMMNotificationClient: only the default-device
// edge is consumed, and only for the render flow's console role (the same role the device
// enumeration reports isDefault for). Every other notification is acknowledged and ignored --
// device arrival/removal surfaces to the mixer reactively through its render-loop HRESULTs.

#include "AudioDeviceNotify.hpp"

#include <windows.h>

#include <mmdeviceapi.h>

namespace moxrelay {

class AudioDeviceNotify::Client : public IMMNotificationClient {
public:
	explicit Client(std::function<void()> cb) : cb_(std::move(cb)) {}

	// IUnknown (COM refcounted; constructed with one ref, freed by the final Release).
	STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }

	STDMETHODIMP_(ULONG) Release() override
	{
		const LONG val = InterlockedDecrement(&refs_);
		if (val == 0)
			delete this;
		return ULONG(val);
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ptr) override
	{
		if (riid == IID_IUnknown) {
			*ptr = static_cast<IUnknown *>(this);
		} else if (riid == __uuidof(IMMNotificationClient)) {
			*ptr = static_cast<IMMNotificationClient *>(this);
		} else {
			*ptr = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}

	// IMMNotificationClient -- only the default-render edge matters here.
	STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
	{
		if (flow == eRender && role == eConsole && cb_)
			cb_();
		return S_OK;
	}

	STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
	STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
	STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
	STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
	volatile LONG refs_ = 1;
	std::function<void()> cb_;
};

AudioDeviceNotify::AudioDeviceNotify(std::function<void()> onDefaultRenderChanged)
{
	// Defensive COM init (the AudioDevices convention): balance only what we performed.
	const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	comBalanced_ = (initHr == S_OK || initHr == S_FALSE);

	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				    __uuidof(IMMDeviceEnumerator), (void **)&enumerator_)) ||
	    !enumerator_) {
		enumerator_ = nullptr;
		return; // inert: default-follow degrades to the reactive reconnect path
	}
	client_ = new Client(std::move(onDefaultRenderChanged));
	if (FAILED(enumerator_->RegisterEndpointNotificationCallback(client_))) {
		client_->Release();
		client_ = nullptr;
		enumerator_->Release();
		enumerator_ = nullptr;
	}
}

AudioDeviceNotify::~AudioDeviceNotify()
{
	if (enumerator_) {
		enumerator_->UnregisterEndpointNotificationCallback(client_);
		enumerator_->Release();
		enumerator_ = nullptr;
	}
	if (client_) {
		client_->Release(); // unregistered above: no further callbacks can be in flight
		client_ = nullptr;
	}
	if (comBalanced_)
		CoUninitialize();
}

} // namespace moxrelay
