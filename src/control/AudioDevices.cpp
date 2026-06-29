// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioDevices implementation. COM is initialized defensively per call (S_FALSE / mode-changed
// are fine -- the process has COM up via the engine bootstrap; we never tear it down here).

#include "AudioDevices.hpp"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <wrl/client.h>

namespace moxrelay {

namespace {

using Microsoft::WRL::ComPtr;

// PKEY_Device_FriendlyName (documented devpkey {a45c254e-df1c-4efd-8020-67d146a850e0}, pid 14),
// defined locally: the SDK's functiondiscoverykeys_devpkey.h does not survive WIN32_LEAN_AND_MEAN
// (its DEFINE_PROPERTYKEY support never gets pulled in), and this is the only key needed.
const PROPERTYKEY kDeviceFriendlyName = {
	{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
	14};

std::string utf16ToUtf8(const wchar_t *w)
{
	if (!w || !*w)
		return {};
	const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)
		return {};
	std::string out(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
	return out;
}

std::wstring utf8ToUtf16(const std::string &s)
{
	if (s.empty())
		return {};
	const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (len <= 1)
		return {};
	std::wstring out(static_cast<size_t>(len - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
	return out;
}

// Resolve `deviceId` ("default" or an endpoint id) to an ACTIVE render IMMDevice; null if it
// does not exist, is not a render endpoint, or is not active.
ComPtr<IMMDevice> resolveRenderDevice(IMMDeviceEnumerator *enumerator, const std::string &deviceId)
{
	ComPtr<IMMDevice> device;
	if (deviceId == "default") {
		if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
			return nullptr;
		return device;
	}
	const std::wstring wide = utf8ToUtf16(deviceId);
	if (wide.empty() || FAILED(enumerator->GetDevice(wide.c_str(), &device)) || !device)
		return nullptr;
	// GetDevice resolves ANY known endpoint (any flow, any state); constrain to active render.
	ComPtr<IMMEndpoint> endpoint;
	EDataFlow flow = eRender;
	if (FAILED(device.As(&endpoint)) || FAILED(endpoint->GetDataFlow(&flow)) || flow != eRender)
		return nullptr;
	DWORD state = 0;
	if (FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
		return nullptr;
	return device;
}

void enumerateFlow(IMMDeviceEnumerator *enumerator, EDataFlow flow, const char *flowName, nlohmann::json &out)
{
	// The default endpoint id for this flow (eConsole role), so isDefault is exact.
	std::string defaultId;
	{
		ComPtr<IMMDevice> def;
		if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &def))) {
			LPWSTR id = nullptr;
			if (SUCCEEDED(def->GetId(&id)) && id) {
				defaultId = utf16ToUtf8(id);
				CoTaskMemFree(id);
			}
		}
	}

	ComPtr<IMMDeviceCollection> devices;
	if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices)))
		return;
	UINT count = 0;
	if (FAILED(devices->GetCount(&count)))
		return;

	for (UINT i = 0; i < count; ++i) {
		ComPtr<IMMDevice> device;
		if (FAILED(devices->Item(i, &device)))
			continue;

		LPWSTR idW = nullptr;
		if (FAILED(device->GetId(&idW)) || !idW)
			continue;
		const std::string id = utf16ToUtf8(idW);
		CoTaskMemFree(idW);

		std::string name;
		ComPtr<IPropertyStore> store;
		if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
			PROPVARIANT value;
			PropVariantInit(&value);
			if (SUCCEEDED(store->GetValue(kDeviceFriendlyName, &value)) &&
			    value.vt == VT_LPWSTR)
				name = utf16ToUtf8(value.pwszVal);
			PropVariantClear(&value);
		}

		out.push_back({{"id", id},
			       {"name", name},
			       {"flow", flowName},
			       {"isDefault", !defaultId.empty() && id == defaultId}});
	}
}

} // namespace

nlohmann::json AudioDevices::enumerate(const std::string &flow)
{
	nlohmann::json devices = nlohmann::json::array();

	// Defensive init: S_OK/S_FALSE both fine; RPC_E_CHANGED_MODE means COM is already up in a
	// different mode -- proceed either way, and only balance the init we actually performed.
	const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	{
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
					       __uuidof(IMMDeviceEnumerator), &enumerator))) {
			if (flow.empty() || flow == "render")
				enumerateFlow(enumerator.Get(), eRender, "render", devices);
			if (flow.empty() || flow == "capture")
				enumerateFlow(enumerator.Get(), eCapture, "capture", devices);
		}
	}
	if (initHr == S_OK || initHr == S_FALSE)
		CoUninitialize();

	return devices;
}

bool AudioDevices::renderDeviceExists(const std::string &deviceId)
{
	if (deviceId == "default")
		return true; // the sentinel is part of the contract vocabulary, not an endpoint id

	bool exists = false;
	const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	{
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
					       __uuidof(IMMDeviceEnumerator), &enumerator)))
			exists = resolveRenderDevice(enumerator.Get(), deviceId) != nullptr;
	}
	if (initHr == S_OK || initHr == S_FALSE)
		CoUninitialize();
	return exists;
}

bool AudioDevices::probeOpenRender(const std::string &deviceId)
{
	bool ok = false;
	const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	{
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
					       __uuidof(IMMDeviceEnumerator), &enumerator))) {
			ComPtr<IMMDevice> device = resolveRenderDevice(enumerator.Get(), deviceId);
			ComPtr<IAudioClient> client;
			WAVEFORMATEX *wfx = nullptr;
			if (device &&
			    SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
						       (void **)client.GetAddressOf())) &&
			    client && SUCCEEDED(client->GetMixFormat(&wfx)) && wfx) {
				// A 100 ms shared-mode init proves the endpoint opens; released on scope
				// exit, so the mixer's own client is never disturbed (shared mode).
				ok = SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0,
								  wfx, nullptr));
			}
			if (wfx)
				CoTaskMemFree(wfx);
		}
	}
	if (initHr == S_OK || initHr == S_FALSE)
		CoUninitialize();
	return ok;
}

} // namespace moxrelay
