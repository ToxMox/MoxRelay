// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// MoxRelay entry point. Console subsystem (a console alongside the GUI is wanted for dev logs).
// The dispatch is CliOptions (STRICT parser -- unknown args fail, never fall through to the GUI)
// -> run_gui / run_selftest / run_perf.
//
//   --selftest  : headless -- construct QApplication (NO window, NO event loop), boot libobs in the
//                 corrected boot order, assert Gate A (default.effect), Gate B (0 module failures),
//                 Gate C (M2.1 sender engine end-to-end), Gate D (M2.2 N senders + pre-resolved
//                 collision counter; owns the optional --hold window for cross-process receiver
//                 verification) and Gate E
//                 (R8 helper-config atomic write + pre-startup obs_data pin), shut down
//                 (twice -- the R3 idempotence assertion), exit 0/1/2.
//   (no args)   : the GUI path = a single instance. "Start Broadcast" emits the instance's
//                 sources; the GUI process publishes the helper-config.json discovery file.

#include "app/AppSettings.hpp"
#include "app/CliOptions.hpp"
#include "app/HelperConfig.hpp"
#include "app/LogSink.hpp"
#include "app/ObsBootstrap.hpp"
#include "app/ParentWatch.hpp"
#include "app/ProfileStore.hpp"
#include "app/RendezvousPipe.hpp"
#include "app/SpoutNaming.hpp"
#include "audio/AudioMixEngine.hpp"
#include "audio/ServoTrimSim.hpp"
#include "control/ControlServer.hpp"
#include "control/ControlToken.hpp"
#include "control/ControlVerbs.hpp"
#include "control/TypeVocabulary.hpp"
#include "obs/SourceFactory.hpp"
#include "perf/PerfHarness.hpp"
#include "spout/SpoutSenderEngine.hpp"
#include "ui/MainWindow.hpp"
#include "ui/MoxDisplayWidget.hpp"
#include "ui/PreviewConsumeEngine.hpp"
#include "ui/SliderJumpStyle.hpp"
#include "ui/TrayController.hpp"

#include "SpoutDX.h" // GATE D2: the in-process receiver side of the per-format assertions

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QMenu>
#include <QProcess>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QtGlobal>

#include <vector>

#include <obs.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// Win32/DXGI -- isolated to this TU for the GPU-adapter count (countGpuAdapters). winsock2.h is
// already pulled in above via IXNetSystem.h, which defines _WINSOCKAPI_, so a later <windows.h>
// will NOT drag in the legacy <winsock.h> (no winsock1/winsock2 redefinition clash).
#include <windows.h>
#include <dxgi.h>

// Apply the embedded QSS dark theme app-wide. Compiled in via CMAKE_AUTORCC from
// resources/resources.qrc, so there is NO runtime file dependency -- the stylesheet ships in the exe.
static void apply_dark_theme(QApplication &app)
{
	// Base-style tweak first (groove clicks jump to position); the stylesheet wraps it.
	app.setStyle(new moxrelay::SliderJumpStyle);
	QFile f(QStringLiteral(":/theme/dark.qss"));
	if (f.open(QFile::ReadOnly | QFile::Text)) {
		app.setStyleSheet(QString::fromUtf8(f.readAll()));
		f.close();
	} else {
		// stdout (not stderr): under the /SUBSYSTEM:WINDOWS build the no-console stderr FILE* is
		// _NO_CONSOLE_FILENO and drops writes; LogSink captures stdout. See LogSink.cpp.
		std::fprintf(stdout, "[MoxRelay] WARNING: embedded theme :/theme/dark.qss not found "
				     "(QSS not applied -- default Qt style).\n");
		std::fflush(stdout);
	}
}

// The MoxRelay application icon as a multi-resolution QIcon, assembled from the PNG sizes embedded
// in the Qt resource bundle (:/icons/moxrelay-*.png). PNG is a BUILT-IN QtGui image handler, so
// this needs no imageformats plugin deployed beside the exe (DeployRuntime ships only the platform
// plugin). Drives the window/taskbar/Alt-Tab icon (setWindowIcon) and the system-tray icon; the
// exe's own Explorer icon is stamped separately by the Win32 resource (resources/icons/moxrelay.rc).
static QIcon moxRelayAppIcon()
{
	QIcon icon;
	for (int sz : {16, 32, 48, 64, 128, 256})
		icon.addFile(QStringLiteral(":/icons/moxrelay-%1.png").arg(sz));
	return icon;
}

// Exercise the design hooks headlessly so the formats are console-verifiable. These prints are
// ADDITIVE to the self-test gates -- they do not affect the exit code. They demonstrate (1) the
// Spout sender-name scheme and (2) the bare single-instance helper-config.json shape clients read.
static void print_design_hook_samples()
{
	using moxrelay::HelperConfig;
	using moxrelay::HelperInstance;
	using moxrelay::SpoutNaming;

	std::printf("\n--- DESIGN HOOKS (headless samples) ---\n");

	// (1) SpoutNaming -- the exact locked sender-name format. Fixed sample for verifiability.
	const std::string sender = SpoutNaming::makeSenderName("DESKTOP-X", 7341, "Facecam");
	std::printf("SpoutNaming::makeSenderName(\"DESKTOP-X\", 7341, \"Facecam\") = %s\n", sender.c_str());
	std::printf("  (local machine name resolved as: %s)\n", SpoutNaming::localMachineName().c_str());

	// (2) HelperConfig -- the bare single-instance object clients read to open the WS control endpoint.
	HelperInstance inst;
	inst.instanceId = "tier-60";
	inst.port = 7341;
	inst.version = moxrelay::kMoxRelayVersion;
	inst.fpsTier = 60;
	inst.spoutPrefix = SpoutNaming::makeSpoutPrefix("DESKTOP-X", 7341);
	inst.ownerId = "";
	inst.controlToken = "a3f81c0e7d2b49f6a3f81c0e7d2b49f6";
	inst.sources = {"DESKTOP-X:Helper_7341_Facecam", "DESKTOP-X:Helper_7341_Desktop"};

	std::printf("HelperConfig::canonicalConfigPath() = %s\n", HelperConfig::canonicalConfigPath().c_str());
	std::printf("HelperConfig::serialize (bare object) =\n%s\n",
		    HelperConfig::serialize(inst, /*pretty=*/true).c_str());
	std::printf("--- END DESIGN HOOKS ---\n");
	std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------
// Control-port resolution (used by the GUI path AND asserted by GATE I). The requested port
// (launch config; default 7341) is TRIED FIRST; if it is busy we scan a small contiguous fallback
// range so a single occupied port never kills the control endpoint (a control client would
// otherwise be unable to reach this instance). The bound port is resolved BEFORE identity / the
// discovery-file writer / the status bar are built, so the ACTUAL port -- not the requested one --
// is the single value baked into the spoutPrefix and the helper-config.json discovery file.
//
// The probe binds a throwaway loopback listen socket (the only portable way to ask "is this port
// free?" -- the vendored IXSocketServer never calls getsockname(), so server.start(0) cannot
// report an OS-assigned ephemeral port). The probe binds EXCLUSIVE (SO_EXCLUSIVEADDRUSE) so a port
// already held by ANOTHER instance's listener is reliably REJECTED (a plain SO_REUSEADDR probe
// would falsely "succeed" against another live SO_REUSEADDR listener and let two instances claim
// the same port). The probe closes immediately and ControlServer rebinds the same port; only the
// sub-ms probe-close -> rebind window remains as the accepted tiny TOCTOU.
// Returns the chosen port, or 0 when NO port in [requested .. requested + kControlPortFallbackSpan]
// can bind (caller degrades to Spout-only -- the only remaining failure mode).
// ---------------------------------------------------------------------------------------------
namespace {

constexpr int kControlPortFallbackSpan = 16; // requested + up to 16 successors before giving up

bool probeLoopbackPortFree(int port)
{
	const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET)
		return false;
	int enable = 1;
	::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&enable),
		     sizeof(enable));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<unsigned short>(port));
	ix::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	const bool free = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
	::closesocket(fd);
	return free;
}

int resolveControlPort(int requested)
{
	// initNetSystem() ref-counts WSAStartup (paired uninit below); ControlServer::start() does
	// the same independently, so the probe is safe alongside it.
	ix::initNetSystem();
	int chosen = 0;
	for (int candidate = requested; candidate <= requested + kControlPortFallbackSpan; ++candidate) {
		if (probeLoopbackPortFree(candidate)) {
			chosen = candidate;
			break;
		}
	}
	ix::uninitNetSystem();
	return chosen;
}

// Count the installed DXGI graphics adapters via CreateDXGIFactory1 + EnumAdapters1 (dxgi.lib is
// already linked). Used only to range-clamp a stale/too-high adapter index before boot. Returns 0
// on any probe failure so the caller skips the clamp (an unknown count must never force index 0).
int countGpuAdapters()
{
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				        reinterpret_cast<void **>(&factory))) ||
	    !factory)
		return 0;
	int count = 0;
	IDXGIAdapter1 *adapter = nullptr;
	while (factory->EnumAdapters1(static_cast<UINT>(count), &adapter) != DXGI_ERROR_NOT_FOUND) {
		if (adapter) {
			adapter->Release();
			adapter = nullptr;
		}
		++count;
	}
	factory->Release();
	return count;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// GATE C (M2.1): the Spout sender engine end-to-end, headless. Creates a deterministic color
// source (ABGR 0xFF2040C0 = RGB(192,64,32)), attaches the engine, waits for real video frames,
// then asserts: SendTexture succeeded, the GetName() read-back is non-empty, and the session-wide
// Spout registry enumerates the name. Teardown is deliberately run TWICE -- engine.stop()
// idempotence is part of the gate (as is the double ObsBootstrap::shutdown in the caller, the R3
// assertion). The cross-process hold window moved to GATE D in M2.2 (its four senders include the
// same deterministic ColorA for the pixel assertion).
// ---------------------------------------------------------------------------------------------
static bool run_gate_c()
{
	using moxrelay::SourceFactory;
	using moxrelay::SpoutNaming;
	using moxrelay::SpoutSenderEngine;

	obs_source_t *src = SourceFactory::createColorSource("SelfTestColor", 0xFF2040C0u, 640, 360);
	if (!src) {
		std::printf("GATE C (spout sender): FAIL (color_source_v3 unavailable)\n");
		std::fflush(stdout);
		return false;
	}

	SpoutSenderEngine engine;

	bool pass = false;
	do {
		// Selftest gates use the engine default cap (64) -- a diagnostic must NOT depend on the
		// user's saved Max-Spout-senders preference. This gate publishes a single sender.
		if (!engine.start())
			break;
		const int slot = engine.attach(src, SpoutNaming::localMachineName(), 7341, "SelfTestColor");
		if (slot < 0)
			break;

		// The libobs graphics thread runs independently of any Qt event loop: wait until it has
		// produced >= 120 video frames (2s at 60 fps), bounded by a 5s timeout.
		const uint32_t startFrames = obs_get_total_frames();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline &&
		       obs_get_total_frames() - startFrames < 120)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));

		const auto infos = engine.slotInfos();
		if (infos.empty())
			break;
		const auto &info = infos.front();

		const bool sendOk = info.sends > 0 && info.lastSendOk;
		const bool nameOk = !info.actualName.empty();
		const bool enumOk = nameOk && SpoutSenderEngine::senderRegistered(info.actualName);
		std::printf("  gate-c: sends=%llu lastSendOk=%d actual='%s' registered=%d\n",
			    static_cast<unsigned long long>(info.sends), info.lastSendOk ? 1 : 0,
			    info.actualName.c_str(), enumOk ? 1 : 0);
		std::fflush(stdout);
		if (!(sendOk && nameOk && enumOk))
			break;

		engine.detach(slot);
		pass = true;
	} while (false);

	engine.stop();
	engine.stop(); // idempotence assertion: the second stop must be a safe no-op

	obs_source_dec_showing(src);
	obs_source_release(src);

	std::printf("GATE C (spout sender): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE D (M2.2): N senders + the pre-resolved name-collision counter, exercised through the REAL
// config path (the embedded JSON goes through SourceFactory::createFromConfigJson exactly like a
// user config file would). Four color sources, two of which share the display name "Dup": the
// allocator must pre-resolve the second to "..._2" (OUR counter, never Spout's silent backstop),
// every slot must be actively sending, and every GetName() read-back must equal the resolved
// name. With holdSeconds > 0 the four senders stay open after the assertions pass so an external
// receiver can assert them cross-process (machine-parseable SELFTEST HOLD lines mark the set).
// ---------------------------------------------------------------------------------------------
static const char *kGateDConfigJson = R"json({
  "port": 7341,
  "sources": [
    {"id": "color_source_v3", "name": "ColorA", "settings": {"color": 4280303808, "width": 640, "height": 360}},
    {"id": "color_source_v3", "name": "ColorB", "settings": {"color": 4278255360, "width": 320, "height": 240}},
    {"id": "color_source_v3", "name": "Dup",    "settings": {"color": 4294901760, "width": 256, "height": 256}},
    {"id": "color_source_v3", "name": "Dup",    "settings": {"color": 4286611456, "width": 256, "height": 256}}
  ]
})json";

static bool run_gate_d(int holdSeconds)
{
	using moxrelay::SourceFactory;
	using moxrelay::SpoutNaming;
	using moxrelay::SpoutSenderEngine;

	moxrelay::SourceConfigResult cfg = SourceFactory::createFromConfigJson(kGateDConfigJson);
	if (!cfg.ok || cfg.sources.size() != 4) {
		std::printf("GATE D (n senders + collision): FAIL (config path: %s)\n",
			    cfg.error.empty() ? "wrong source count" : cfg.error.c_str());
		std::fflush(stdout);
		return false;
	}

	const std::string machine = SpoutNaming::localMachineName();
	const std::string dupBase = SpoutNaming::makeSenderName(machine, cfg.port, "Dup");

	SpoutSenderEngine engine;
	bool pass = false;
	do {
		// Selftest gate uses the engine default cap (64) -- a diagnostic must NOT depend on the
		// user's saved Max-Spout-senders preference. This gate publishes four senders.
		if (!engine.start())
			break;

		bool attachOk = true;
		for (const moxrelay::CreatedSource &cs : cfg.sources) {
			if (engine.attach(cs.source, machine, cfg.port, cs.name) < 0)
				attachOk = false;
		}
		if (!attachOk)
			break;

		// Wait for >= 120 video frames (the graphics thread runs without a Qt event loop), so
		// every sender has sent real frames (first frame skips the prev-buffer send by design).
		const uint32_t startFrames = obs_get_total_frames();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline &&
		       obs_get_total_frames() - startFrames < 120)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));

		const auto infos = engine.slotInfos();
		if (infos.size() != 4)
			break;

		bool allSending = true;
		bool allNamesMatch = true;
		bool allRegistered = true;
		std::string dupFirst;
		std::string dupSecond;
		for (const auto &info : infos) {
			std::printf("  gate-d: slot=%d requested='%s' resolved='%s' actual='%s' sends=%llu ok=%d\n",
				    info.slotId, info.requestedName.c_str(), info.resolvedName.c_str(),
				    info.actualName.c_str(), static_cast<unsigned long long>(info.sends),
				    info.lastSendOk ? 1 : 0);
			if (!(info.sends > 0 && info.lastSendOk))
				allSending = false;
			if (info.actualName.empty() || info.actualName != info.resolvedName)
				allNamesMatch = false;
			if (!SpoutSenderEngine::senderRegistered(info.resolvedName))
				allRegistered = false;
			if (info.requestedName == dupBase) {
				if (dupFirst.empty())
					dupFirst = info.resolvedName;
				else
					dupSecond = info.resolvedName;
			}
		}
		// The collision pair: first "Dup" keeps the base, second is OUR "_2" suffix (never
		// Spout's silent "_1" backstop -- that would show up as actual != resolved above).
		const bool collisionOk = (dupFirst == dupBase) && (dupSecond == dupBase + "_2");
		std::printf("  gate-d: collision pair '%s' / '%s' (expected '%s' / '%s_2')\n", dupFirst.c_str(),
			    dupSecond.c_str(), dupBase.c_str(), dupBase.c_str());
		std::fflush(stdout);

		if (!(allSending && allNamesMatch && allRegistered && collisionOk))
			break;

		if (holdSeconds > 0) {
			// slotInfos preserves attach order, which is cfg.sources document order.
			for (size_t i = 0; i < infos.size() && i < cfg.sources.size(); ++i) {
				std::printf("SELFTEST HOLD sender=%s w=%u h=%u seconds=%d\n",
					    infos[i].actualName.c_str(),
					    obs_source_get_base_width(cfg.sources[i].source),
					    obs_source_get_base_height(cfg.sources[i].source), holdSeconds);
			}
			std::printf("SELFTEST HOLD count=%zu seconds=%d\n", infos.size(), holdSeconds);
			std::fflush(stdout);
			std::this_thread::sleep_for(std::chrono::seconds(holdSeconds));
		}

		pass = true;
	} while (false);

	engine.stop();
	engine.stop(); // idempotence assertion, same as GATE C

	for (moxrelay::CreatedSource &cs : cfg.sources) {
		if (cs.showing)
			obs_source_dec_showing(cs.source); // mirror factory's conditional inc: an inert source took no showing ref
		obs_source_release(cs.source);
	}

	std::printf("GATE D (n senders + collision): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE D2 (M3): per-slot sender formats + the alpha contract, in-process end to end. Three
// color sources share ONE color value (0x80BCBCBC ABGR: half-transparent grey, sRGB byte 188)
// and attach one per SenderFormat through the real config path ("format" rides beside settings).
// An in-process SpoutDX RECEIVER (own device -- the normal cross-device shared-texture open)
// asserts per sender:
//   srgb87   -- registered DXGI format 87; RGB bytes == 188 EXACT (raw sRGB passthrough).
//   linear87 -- format 87; RGB bytes == linear(188/255)*255 ~= 128 (+/-2: spec-exact GPU sRGB
//               decode, UNORM rounding).
//   fp16     -- format 10; half-float RGB within epsilon of linear(188/255) ~= 0.5029.
//   ALL THREE -- alpha EXACTLY 0x80: the sRGB encode/decode must never touch the A channel
//               (the transparency contract gate; fp16 alpha asserted after byte rounding).
// ---------------------------------------------------------------------------------------------
static const char *kGateD2ConfigJson = R"json({
  "port": 7341,
  "sources": [
    {"id": "color_source_v3", "name": "FmtSrgb",   "settings": {"color": 2159852732, "width": 64, "height": 64}},
    {"id": "color_source_v3", "name": "FmtLinear", "settings": {"color": 2159852732, "width": 64, "height": 64}, "format": "linear87"},
    {"id": "color_source_v3", "name": "FmtHalf",   "settings": {"color": 2159852732, "width": 64, "height": 64}, "format": "fp16"}
  ]
})json";

namespace {

struct ReceivedPixels {
	bool ok = false;
	DWORD format = 0; // registered DXGI format of the sender
	unsigned int width = 0;
	unsigned int height = 0;
	unsigned int rowPitch = 0;
	std::vector<uint8_t> bytes;
	std::string error;
};

float half_to_float(uint16_t h)
{
	const uint32_t sign = (uint32_t(h) & 0x8000u) << 16;
	uint32_t exp = (h >> 10) & 0x1Fu;
	uint32_t mant = h & 0x3FFu;
	uint32_t bits;
	if (exp == 0) {
		if (mant == 0) {
			bits = sign; // +/- 0
		} else {
			exp = 127 - 15 + 1; // subnormal half -> normalized float
			while (!(mant & 0x400u)) {
				mant <<= 1;
				--exp;
			}
			mant &= 0x3FFu;
			bits = sign | (exp << 23) | (mant << 13);
		}
	} else if (exp == 31) {
		bits = sign | 0x7F800000u | (mant << 13); // inf/nan
	} else {
		bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
	}
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

// Open `name` with a dedicated receiver, pull one settled frame, read the pixels back through a
// staging texture. Self-contained: own device, released before return.
ReceivedPixels receive_sender_pixels(const std::string &name, int timeoutMs)
{
	ReceivedPixels out;
	spoutDX rx;
	if (!rx.OpenDirectX11() || !rx.GetDX11Device()) {
		out.error = "receiver OpenDirectX11 failed";
		return out;
	}
	rx.SetReceiverName(name.c_str());

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	bool settled = false;
	while (std::chrono::steady_clock::now() < deadline) {
		if (rx.ReceiveTexture()) {
			if (rx.IsUpdated()) {
				// Size/format (re)allocation frame: the class texture was rebuilt;
				// receive again so it carries real pixels.
				settled = false;
			} else if (rx.GetSenderTexture() && rx.GetSenderWidth() > 0) {
				if (settled)
					break; // two consecutive settled receives: pixels are real
				settled = true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
	}
	ID3D11Texture2D *tex = rx.GetSenderTexture();
	if (!settled || !tex) {
		out.error = "no settled frame from '" + name + "' within timeout";
		rx.ReleaseReceiver();
		rx.CloseDirectX11();
		return out;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	tex->GetDesc(&desc);
	D3D11_TEXTURE2D_DESC sdesc = desc;
	sdesc.Usage = D3D11_USAGE_STAGING;
	sdesc.BindFlags = 0;
	sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sdesc.MiscFlags = 0;
	ID3D11Texture2D *staging = nullptr;
	if (FAILED(rx.GetDX11Device()->CreateTexture2D(&sdesc, nullptr, &staging)) || !staging) {
		out.error = "staging texture creation failed";
		rx.ReleaseReceiver();
		rx.CloseDirectX11();
		return out;
	}
	rx.GetDX11Context()->CopyResource(staging, tex);
	D3D11_MAPPED_SUBRESOURCE map = {};
	if (FAILED(rx.GetDX11Context()->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
		out.error = "staging map failed";
		staging->Release();
		rx.ReleaseReceiver();
		rx.CloseDirectX11();
		return out;
	}
	out.width = desc.Width;
	out.height = desc.Height;
	out.rowPitch = map.RowPitch;
	out.bytes.assign(static_cast<const uint8_t *>(map.pData),
			 static_cast<const uint8_t *>(map.pData) + size_t(map.RowPitch) * desc.Height);
	rx.GetDX11Context()->Unmap(staging, 0);
	staging->Release();
	out.format = (DWORD)rx.GetSenderFormat();
	out.ok = true;
	rx.ReleaseReceiver();
	rx.CloseDirectX11();
	return out;
}

} // namespace

static bool run_gate_d2()
{
	using moxrelay::SenderFormat;
	using moxrelay::SourceFactory;
	using moxrelay::SpoutNaming;
	using moxrelay::SpoutSenderEngine;

	moxrelay::SourceConfigResult cfg = SourceFactory::createFromConfigJson(kGateD2ConfigJson);
	if (!cfg.ok || cfg.sources.size() != 3) {
		std::printf("GATE D2 (sender formats + alpha): FAIL (config path: %s)\n",
			    cfg.error.empty() ? "wrong source count" : cfg.error.c_str());
		std::fflush(stdout);
		return false;
	}

	const std::string machine = SpoutNaming::localMachineName();
	// linear(188/255) on the spec sRGB curve = 0.50288...; UNORM target rounds to 128.
	constexpr double kLinearOf188 = 0.502886;

	SpoutSenderEngine engine;
	bool pass = false;
	do {
		// Selftest gate uses the engine default cap (64) -- a diagnostic must NOT depend on the
		// user's saved Max-Spout-senders preference. This gate publishes three senders.
		if (!engine.start())
			break;

		bool attachOk = true;
		for (const moxrelay::CreatedSource &cs : cfg.sources) {
			SenderFormat fmt = SenderFormat::Srgb87;
			SpoutSenderEngine::parseFormat(cs.format, fmt);
			if (engine.attach(cs.source, machine, cfg.port, cs.name, fmt) < 0)
				attachOk = false;
		}
		if (!attachOk)
			break;

		// Let every sender emit real frames (first frame skips the prev-buffer send).
		const uint32_t startFrames = obs_get_total_frames();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline &&
		       obs_get_total_frames() - startFrames < 120)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));

		const auto infos = engine.slotInfos();
		if (infos.size() != 3)
			break;

		// Engine-side cross-check: the slots carry the configured formats (attach order ==
		// document order), every slot is live.
		const SenderFormat expectedFmt[3] = {SenderFormat::Srgb87, SenderFormat::Linear87,
						     SenderFormat::Fp16};
		bool slotsOk = true;
		for (size_t i = 0; i < 3; ++i) {
			if (infos[i].format != expectedFmt[i] || infos[i].actualName.empty() ||
			    !(infos[i].sends > 0 && infos[i].lastSendOk))
				slotsOk = false;
		}
		if (!slotsOk) {
			std::printf("  gate-d2: engine slot state/format mismatch\n");
			break;
		}

		bool allOk = true;
		for (size_t i = 0; i < 3; ++i) {
			const char *fmtName = SpoutSenderEngine::formatName(expectedFmt[i]);
			const ReceivedPixels px = receive_sender_pixels(infos[i].actualName, 5000);
			if (!px.ok) {
				std::printf("  gate-d2: %s receive FAIL (%s)\n", fmtName, px.error.c_str());
				allOk = false;
				continue;
			}
			const DWORD wantFormat = (expectedFmt[i] == SenderFormat::Fp16) ? 10u : 87u;
			const bool formatOk = (px.format == wantFormat);
			bool rgbOk = false;
			bool alphaOk = false;
			if (expectedFmt[i] == SenderFormat::Fp16) {
				const uint8_t *row = px.bytes.data() + size_t(px.rowPitch) * (px.height / 2);
				const uint16_t *texel =
					reinterpret_cast<const uint16_t *>(row + size_t(px.width / 2) * 8);
				const float r = half_to_float(texel[0]);
				const float g = half_to_float(texel[1]);
				const float b = half_to_float(texel[2]);
				const float a = half_to_float(texel[3]);
				rgbOk = std::abs(r - kLinearOf188) < 0.01 && std::abs(g - kLinearOf188) < 0.01 &&
					std::abs(b - kLinearOf188) < 0.01;
				// Alpha is linear data: 128/255 exactly, byte-exact after rounding.
				alphaOk = (int)std::lround(a * 255.0f) == 128;
				std::printf("  gate-d2: %s fmt=%lu rgb=(%.4f,%.4f,%.4f) a=%.4f\n", fmtName,
					    (unsigned long)px.format, r, g, b, a);
			} else {
				const uint8_t *row = px.bytes.data() + size_t(px.rowPitch) * (px.height / 2);
				const uint8_t *texel = row + size_t(px.width / 2) * 4; // BGRA
				const int want = (expectedFmt[i] == SenderFormat::Srgb87)
							 ? 188
							 : (int)std::lround(kLinearOf188 * 255.0);
				const int tol = (expectedFmt[i] == SenderFormat::Srgb87) ? 0 : 2;
				rgbOk = std::abs(texel[0] - want) <= tol && std::abs(texel[1] - want) <= tol &&
					std::abs(texel[2] - want) <= tol;
				alphaOk = (texel[3] == 0x80);
				std::printf("  gate-d2: %s fmt=%lu bgra=(%d,%d,%d,%d) want rgb~%d a=128\n",
					    fmtName, (unsigned long)px.format, texel[0], texel[1], texel[2],
					    texel[3], want);
			}
			if (!(formatOk && rgbOk && alphaOk)) {
				std::printf("  gate-d2: %s FAIL (format=%d rgb=%d alpha=%d)\n", fmtName,
					    formatOk ? 1 : 0, rgbOk ? 1 : 0, alphaOk ? 1 : 0);
				allOk = false;
			}
		}
		std::fflush(stdout);
		if (!allOk)
			break;

		pass = true;
	} while (false);

	engine.stop();

	for (moxrelay::CreatedSource &cs : cfg.sources) {
		if (cs.showing)
			obs_source_dec_showing(cs.source); // mirror factory's conditional inc: an inert source took no showing ref
		obs_source_release(cs.source);
	}

	std::printf("GATE D2 (sender formats + alpha): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE E (M2.3/R8): the helper-config atomic write, end to end -- serialize -> writeTo a TEMP
// path (never the live %APPDATA% discovery file a running fleet may own) -> parse the file back
// and verify the instance survived. `preStartupSerializeOk` pins the R8 claim that obs_data is
// bmem-only (it ran BEFORE ObsBootstrap::startup, with obs.dll merely loaded).
// ---------------------------------------------------------------------------------------------
static bool run_gate_e(bool preStartupSerializeOk)
{
	using moxrelay::HelperConfig;
	using moxrelay::HelperInstance;
	using moxrelay::SpoutNaming;

	bool pass = false;
	do {
		if (!preStartupSerializeOk)
			break;

		HelperInstance inst;
		inst.instanceId = "tier-60";
		inst.port = 7341;
		inst.version = moxrelay::kMoxRelayVersion;
		inst.fpsTier = 60;
		inst.spoutPrefix = SpoutNaming::makeSpoutPrefix("DESKTOP-X", 7341);
		inst.ownerId = "owner-x";
		inst.controlToken = "a3f81c0e7d2b49f6a3f81c0e7d2b49f6";
		inst.sources = {"DESKTOP-X:Helper_7341_Facecam"};

		const std::string path =
			QDir::tempPath().toStdString() + "/moxrelay-selftest-helper-config.json";
		if (!HelperConfig::writeTo(path, inst))
			break;

		// Parse-back asserts the CANONICAL bare-object field set round-trips -- a serializer
		// regression that drops or renames any of the eight fields (incl. controlToken/ownerId,
		// which a control client reads from the file) must fail the gate, not just port.
		bool parsed = false;
		if (obs_data_t *doc = obs_data_create_from_json_file(path.c_str())) {
			obs_data_array_t *srcs = obs_data_get_array(doc, "sources");
			obs_data_t *s0 = (srcs && obs_data_array_count(srcs) == 1)
						 ? obs_data_array_item(srcs, 0)
						 : nullptr;
			// fpsTier must round-trip as a JSON NUMBER (int normalized 2026-06-09,
			// pre-consumer) -- a regression back to a string must fail this gate.
			bool fpsTierIsInt = false;
			if (obs_data_item_t *it = obs_data_item_byname(doc, "fpsTier")) {
				fpsTierIsInt = obs_data_item_gettype(it) == OBS_DATA_NUMBER &&
					       obs_data_item_get_int(it) == 60;
				obs_data_item_release(&it);
			}
			parsed = std::strcmp(obs_data_get_string(doc, "instanceId"), "tier-60") == 0 &&
				 obs_data_get_int(doc, "port") == 7341 &&
				 std::strcmp(obs_data_get_string(doc, "version"),
					      moxrelay::kMoxRelayVersion) == 0 &&
				 fpsTierIsInt &&
				 std::strcmp(obs_data_get_string(doc, "spoutPrefix"),
					      inst.spoutPrefix.c_str()) == 0 &&
				 std::strcmp(obs_data_get_string(doc, "ownerId"), "owner-x") == 0 &&
				 std::strcmp(obs_data_get_string(doc, "controlToken"),
					      "a3f81c0e7d2b49f6a3f81c0e7d2b49f6") == 0 &&
				 s0 != nullptr &&
				 std::strcmp(obs_data_get_string(s0, "name"),
					      "DESKTOP-X:Helper_7341_Facecam") == 0;
			if (s0)
				obs_data_release(s0);
			if (srcs)
				obs_data_array_release(srcs);
			obs_data_release(doc);
		}
		std::remove(path.c_str());
		if (!parsed)
			break;

		pass = true;
	} while (false);

	std::printf("GATE E (helper-config write): %s%s\n", pass ? "PASS" : "FAIL",
		    preStartupSerializeOk ? "" : " (pre-startup obs_data serialize failed)");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE F (M6): the control endpoint end-to-end, in-process. Spins the real ControlVerbs +
// ControlServer on a local probe-selected port, connects with the vendored WebSocket CLIENT
// class, and walks the contract: GetVersion (apiVersion + capabilities), -32601 on an unknown
// method, -32602 on an empty Subscribe, a gated status push, the audio routing + per-source
// audio surface (device echo/1006, state round-trip, audioChanged gating, audioLevels cadence),
// CreateSource(color) -> StartBroadcast -> senderNameResolved -> GetStatus shows the
// slot sending, RemoveSource -> sourceRemoved + clean detach, double-stop idempotence, the
// closed-set sourceTypes capability check, the text-source walk (create/broadcast/resolve,
// real dims + format field, font descriptor + FontValue round-trip), the media-transport
// negatives (GetMediaStatus/ControlMedia/SeekMedia -> 1012 on a non-media source, proving the
// verbs exist and gate on the media-pipeline capability), the mediaChanged Subscribe ack, the
// propertyChanged both-connections + filter-scoped emission, the InvokeSourceButton
// negatives (1006 non-button/unknown property, 1001 unknown source), and the filter-chain
// verb surface (ListFilters order, SetFilterEnabled/ReorderFilter/SetFilterName + their
// filterChanged emissions, clamp semantics, filterAdded/filterRemoved).
// Full transport BEHAVIOR (play/pause/seek/ended/looping) needs a clip and lives in the
// conformance suite (examples/contract-test).
// The selftest never runs an event loop, so the gate pumps QCoreApplication::processEvents()
// while waiting (queued verb dispatch + the 1s server timer both ride the pump).
// ---------------------------------------------------------------------------------------------
namespace {

// The self-test server runs WITH a token (so gate-f exercises a real authenticated round-trip);
// every GateFClient appends it to the connect URL. Native ws-loopback Origin (ixwebsocket's
// default) passes the narrowed Origin gate.
constexpr const char *kSelfTestToken = "00112233445566778899aabbccddeeff";

struct GateFClient {
	ix::WebSocket ws;
	std::mutex mutex;
	std::deque<nlohmann::json> inbox;
	std::atomic<bool> open{false};
	std::atomic<bool> closed{false};
	std::atomic<uint16_t> closeCode{0};

	// originOverride: when non-empty, sent as an explicit Origin header (negative gate test).
	void start(int port, const std::string &originOverride = std::string(), bool withToken = true)
	{
		std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/control";
		if (withToken)
			url += "?token=" + std::string(kSelfTestToken);
		ws.setUrl(url);
		ws.disableAutomaticReconnection();
		if (!originOverride.empty())
			ws.setExtraHeaders({{"Origin", originOverride}});
		ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
			if (msg->type == ix::WebSocketMessageType::Open) {
				open = true;
			} else if (msg->type == ix::WebSocketMessageType::Close) {
				closeCode = msg->closeInfo.code;
				closed = true;
			} else if (msg->type == ix::WebSocketMessageType::Message) {
				nlohmann::json j = nlohmann::json::parse(msg->str, nullptr, false);
				if (!j.is_discarded()) {
					std::lock_guard<std::mutex> lock(mutex);
					inbox.push_back(std::move(j));
				}
			}
		});
		ws.start();
	}

	// Pump the Qt loop + poll until pred() or timeout.
	static bool pumpUntil(const std::function<bool()> &pred, int timeoutMs)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline) {
			QCoreApplication::processEvents();
			if (pred())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		QCoreApplication::processEvents();
		return pred();
	}

	// Take the first inbox entry matching `match` (reply by id / event by name).
	bool take(const std::function<bool(const nlohmann::json &)> &match, nlohmann::json &out, int timeoutMs)
	{
		const auto scan = [&]() -> bool {
			std::lock_guard<std::mutex> lock(mutex);
			for (auto it = inbox.begin(); it != inbox.end(); ++it) {
				if (match(*it)) {
					out = *it;
					inbox.erase(it);
					return true;
				}
			}
			return false;
		};
		return pumpUntil(scan, timeoutMs);
	}

	bool request(int id, const std::string &method, const nlohmann::json &params, nlohmann::json &reply,
		     int timeoutMs = 3000)
	{
		nlohmann::json req = {{"id", id}, {"method", method}};
		if (!params.is_null())
			req["params"] = params;
		ws.send(req.dump());
		return take([id](const nlohmann::json &j) { return j.contains("id") && j["id"] == id; },
			    reply, timeoutMs);
	}

	bool awaitEvent(const std::string &name, nlohmann::json &evt, int timeoutMs)
	{
		return take(
			[&name](const nlohmann::json &j) {
				return !j.contains("id") && j.contains("event") && j["event"] == name;
			},
			evt, timeoutMs);
	}
};

// Additive print (not a gate): scan every contract type's property + default-settings keys for
// engine-branded names. The published keys are part of the contract; anything suspicious here
// gets renamed through the TypeVocabulary overlay BEFORE clients pin the bytes.
void run_settings_key_audit()
{
	using moxrelay::TypeVocabulary;
	size_t scanned = 0;
	size_t flagged = 0;
	const auto audit = [&](const char *typeName, const char *engineId) {
		std::printf("KEY AUDIT scan type=%s\n", typeName);
		std::fflush(stdout);
		std::set<std::string> keys;
		// Property enumeration goes through a throwaway PRIVATE instance: some property
		// callbacks dereference their instance data unconditionally (the expander family,
		// expander-filter.c expander_properties), so the TYPE-LEVEL query crashes for them.
		// This mirrors the product paths, which are always instance-based.
		if (obs_source_t *probe = obs_source_create_private(engineId, "_keyaudit", nullptr)) {
			if (obs_properties_t *props = obs_source_properties(probe)) {
				for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
					if (const char *n = obs_property_name(p))
						keys.insert(n);
				}
				obs_properties_destroy(props);
			}
			obs_source_release(probe);
		}
		if (obs_data_t *defaults = obs_get_source_defaults(engineId)) {
			for (obs_data_item_t *item = obs_data_first(defaults); item;
			     obs_data_item_next(&item)) {
				if (const char *n = obs_data_item_get_name(item))
					keys.insert(n);
			}
			obs_data_release(defaults);
		}
		for (const std::string &key : keys) {
			++scanned;
			std::string lower = key;
			for (char &c : lower)
				c = char(::tolower(static_cast<unsigned char>(c)));
			if (lower.find("obs") != std::string::npos) {
				++flagged;
				std::printf("KEY AUDIT FLAG type=%s key=%s\n", typeName, key.c_str());
			}
		}
	};
	for (const auto &e : TypeVocabulary::sourceTypes())
		audit(e.wireName, e.engineId);
	for (const auto &e : TypeVocabulary::filterTypes())
		audit(e.wireName, e.engineId);
	std::printf("KEY AUDIT: %zu keys scanned, %zu flagged\n", scanned, flagged);
	std::fflush(stdout);
}

} // namespace

static bool run_gate_f()
{
	using moxrelay::ControlServer;
	using moxrelay::ControlVerbs;
	using moxrelay::InstanceIdentity;
	using moxrelay::SpoutNaming;
	using moxrelay::SpoutSenderEngine;
	using nlohmann::json;

	bool pass = false;
	SpoutSenderEngine engine;
	moxrelay::AudioMixEngine audio; // test-sink mode: the wire surface needs no endpoint
	do {
		if (!engine.start())
			break;
		audio.setTestSink([](const float *, size_t) {});
		if (!audio.start())
			break;

		const std::string machine = SpoutNaming::localMachineName();
		InstanceIdentity identity;
		identity.instanceId = "selftest";
		identity.fpsTier = 60;
		identity.version = moxrelay::kMoxRelayVersion;
		identity.machine = machine;

		ControlVerbs verbs(&engine, &audio, identity); // GATE G asserts the mix path itself
		ControlServer server(&verbs);
		// Run the self-test server WITH a token so gate-f authenticates for real (narrowed
		// Origin gate + GATE-2 token both exercised on every connect).
		server.setControlToken(kSelfTestToken);
		int port = 0;
		for (int candidate = 47341; candidate <= 47350; ++candidate) {
			if (server.start(candidate)) {
				port = candidate;
				break;
			}
		}
		if (!port) {
			std::printf("  gate-f: no local port available in 47341-47350\n");
			break;
		}

		GateFClient client;
		client.start(port);
		if (!GateFClient::pumpUntil([&] { return client.open.load(); }, 3000)) {
			std::printf("  gate-f: client connect timed out\n");
			break;
		}

		// Negative gate regression-guards (the server runs WITH a token + the narrowed Origin
		// gate). The gates run in onOpen, which fires AFTER ixwebsocket accepts the WS upgrade
		// (so the client may see Open then a server-initiated Close); the observable rejection
		// is the close CODE: (a) NO ?token= -> GATE 2 closes 4401. (b) a browser-style Origin
		// (http) -> GATE 1 closes 4403. Each client must observe its expected close code.
		bool okAuth4401 = false;
		{
			GateFClient noToken;
			noToken.start(port, std::string(), /*withToken=*/false);
			okAuth4401 = GateFClient::pumpUntil([&] { return noToken.closed.load(); }, 3000) &&
				     noToken.closeCode.load() == 4401;
			std::printf("  gate-f: no-token -> 4401 %s (code=%u)\n",
				    okAuth4401 ? "ok" : "FAIL",
				    static_cast<unsigned>(noToken.closeCode.load()));
			noToken.ws.stop();
		}

		bool okOrigin4403 = false;
		{
			GateFClient badOrigin;
			badOrigin.start(port, "http://evil.example");
			okOrigin4403 = GateFClient::pumpUntil([&] { return badOrigin.closed.load(); }, 3000) &&
				       badOrigin.closeCode.load() == 4403;
			std::printf("  gate-f: browser-Origin -> 4403 %s (code=%u)\n",
				    okOrigin4403 ? "ok" : "FAIL",
				    static_cast<unsigned>(badOrigin.closeCode.load()));
			badOrigin.ws.stop();
		}

		json reply;
		json evt;

		// 1. GetVersion: apiVersion 1 + capability sets present.
		bool ok1 = client.request(1, "GetVersion", nullptr, reply) && reply.contains("result") &&
			   reply["result"]["apiVersion"] == 1 &&
			   reply["result"]["capabilities"]["sourceTypes"].is_array() &&
			   !reply["result"]["capabilities"]["sourceTypes"].empty() &&
			   reply["result"]["capabilities"]["audioOutput"] == true;
		std::printf("  gate-f: GetVersion %s\n", ok1 ? "ok" : "FAIL");

		// 2. Unknown method -> -32601.
		bool ok2 = client.request(2, "NoSuchVerb", nullptr, reply) && reply.contains("error") &&
			   reply["error"]["code"] == -32601;
		std::printf("  gate-f: -32601 %s\n", ok2 ? "ok" : "FAIL");

		// 3. Subscribe with an empty events array -> -32602 (locked message shape).
		bool ok3 = client.request(3, "Subscribe", json{{"events", json::array()}}, reply) &&
			   reply.contains("error") && reply["error"]["code"] == -32602;
		std::printf("  gate-f: -32602 %s\n", ok3 ? "ok" : "FAIL");

		// 4. Subscribe -> ack lists the accepted names; a status push follows within ~2s.
		const json subscribeEvents = {{"events",
					       {"status", "sourceAdded", "sourceRemoved",
						"senderNameResolved", "broadcastChanged"}}};
		bool ok4 = client.request(4, "Subscribe", subscribeEvents, reply) &&
			   reply.contains("result") && reply["result"]["subscribed"].size() == 5 &&
			   client.awaitEvent("status", evt, 2500) && evt["data"].contains("fps");
		std::printf("  gate-f: subscribe+status %s\n", ok4 ? "ok" : "FAIL");

		// 5. SetAudioOutputDevice: the "default" sentinel succeeds with the stored-selection
		//    echo, GetStatus reads the selection back, and an unknown endpoint id is 1006
		//    with data.property naming the offender.
		bool ok5 = client.request(5, "SetAudioOutputDevice", json{{"deviceId", "default"}}, reply) &&
			   reply.contains("result") && reply["result"]["deviceId"] == "default" &&
			   client.request(46, "GetStatus", nullptr, reply) && reply.contains("result") &&
			   reply["result"]["audioOutputDevice"] == "default" &&
			   client.request(47, "SetAudioOutputDevice",
					  json{{"deviceId", "not-an-endpoint-id"}}, reply) &&
			   reply.contains("error") && reply["error"]["code"] == 1006 &&
			   reply["error"]["data"]["property"] == "deviceId";
		std::printf("  gate-f: SetAudioOutputDevice echo + 1006 %s\n", ok5 ? "ok" : "FAIL");

		// 5b. Device-queue latency bound on the REAL default endpoint (the wire engine above
		//     is test-sink, so this is the selftest's one real WASAPI render client): past
		//     warmup the queue must sit at the write target, never at buffer capacity --
		//     queued device audio is audible latency the ring servo cannot see.
		bool ok5b = false;
		{
			moxrelay::AudioMixEngine realAudio;
			if (realAudio.start()) {
				GateFClient::pumpUntil([] { return false; }, 3500); // ~1.5 s past warmup
				const auto rst = realAudio.stats();
				ok5b = rst.deviceUp && rst.deviceFillMaxMs > 0.0 && rst.deviceFillMaxMs <= 60.0 &&
				       rst.deviceStarves == 0;
				std::printf("  gate-f: device-fill bound %s (max %.1f ms, avg %.1f ms, starves %llu)\n",
					    ok5b ? "ok" : "FAIL", rst.deviceFillMaxMs, rst.deviceFillAvgMs,
					    (unsigned long long)rst.deviceStarves);
				realAudio.stop();
				// Let the audio session unwind before the timing-sensitive audio gate
				// that follows the endpoint walk.
				GateFClient::pumpUntil([] { return false; }, 750);
			} else {
				std::printf("  gate-f: device-fill bound FAIL (engine start)\n");
			}
		}

		// 6. CreateSource(color): reply carries sourceId + ALWAYS-null senderName; the
		//    sourceAdded event mirrors it.
		std::string sourceId;
		bool ok6 = client.request(6, "CreateSource",
					  json{{"type", "color"},
					       {"displayName", "GateF"},
					       {"settings", {{"color", 4280303808u}, {"width", 320}, {"height", 180}}}},
					  reply) &&
			   reply.contains("result") && reply["result"]["senderName"].is_null();
		if (ok6) {
			sourceId = reply["result"]["sourceId"].get<std::string>();
			ok6 = client.awaitEvent("sourceAdded", evt, 2000) &&
			      evt["data"]["source"]["sourceId"] == sourceId;
		}
		std::printf("  gate-f: CreateSource %s (%s)\n", ok6 ? "ok" : "FAIL", sourceId.c_str());

		// 7. StartBroadcast -> sender resolves (engine first-send + 1s tick), GetStatus
		//    shows the slot actively sending.
		bool ok7 = false;
		if (ok6) {
			ok7 = client.request(7, "StartBroadcast", json{{"sourceIds", {sourceId}}}, reply) &&
			      reply.contains("result") &&
			      reply["result"]["started"] == json::array({sourceId}) &&
			      client.awaitEvent("senderNameResolved", evt, 5000) &&
			      evt["data"]["sourceId"] == sourceId &&
			      !evt["data"]["senderName"].get<std::string>().empty();
			if (ok7) {
				// Let a few more frames flow, then assert the live snapshot.
				GateFClient::pumpUntil([] { return false; }, 300);
				ok7 = client.request(8, "GetStatus", nullptr, reply) &&
				      reply.contains("result") &&
				      reply["result"]["state"] == "broadcasting" &&
				      reply["result"]["sources"].size() == 1 &&
				      reply["result"]["sources"][0]["broadcasting"] == true &&
				      reply["result"]["sources"][0]["sends"].get<uint64_t>() > 0;
			}
		}
		std::printf("  gate-f: broadcast+resolve %s\n", ok7 ? "ok" : "FAIL");

		// 8. RemoveSource -> sourceRemoved + the instance returns to ready with no sources.
		bool ok8 = false;
		if (ok7) {
			ok8 = client.request(9, "RemoveSource", json{{"sourceId", sourceId}}, reply) &&
			      reply.contains("result") && reply["result"]["removed"] == true &&
			      client.awaitEvent("sourceRemoved", evt, 2000) &&
			      evt["data"]["sourceId"] == sourceId &&
			      client.request(10, "GetStatus", nullptr, reply) &&
			      reply["result"]["sources"].empty() && reply["result"]["state"] == "ready";
		}
		std::printf("  gate-f: RemoveSource %s\n", ok8 ? "ok" : "FAIL");

		// 9. capabilities.sourceTypes is EXACTLY the current closed set (order-insensitive).
		//    Catches both a silently-missing module (a type drops out -- a build without the
		//    audio capture module MUST fail here) and an accidental extra type. Deliberately a
		//    hardcoded set equality, never registration-aware (a weaker regression guard).
		bool ok9 = false;
		if (client.request(11, "GetVersion", nullptr, reply) && reply.contains("result")) {
			std::set<std::string> got;
			for (const auto &t : reply["result"]["capabilities"]["sourceTypes"])
				got.insert(t.get<std::string>());
			const std::set<std::string> expected = {"camera",      "display", "window",
								"game",        "media",   "image",
								"color",       "text",    "audio_input",
								"audio_output"};
			ok9 = (got == expected);
		}
		std::printf("  gate-f: sourceTypes closed set %s\n", ok9 ? "ok" : "FAIL");

		// 10. text source end-to-end: create (non-empty text so GDI+ renders nonzero dims),
		//     broadcast, sender resolves, live snapshot shows real dims + the format field.
		std::string textId;
		bool ok10 = client.request(12, "CreateSource",
					   json{{"type", "text"},
						{"displayName", "GateFText"},
						{"settings", {{"text", "MoxRelay GateF"}}}},
					   reply) &&
			    reply.contains("result") && reply["result"]["senderName"].is_null() &&
			    reply["result"]["format"] == "srgb87";
		if (ok10) {
			textId = reply["result"]["sourceId"].get<std::string>();
			ok10 = client.awaitEvent("sourceAdded", evt, 2000) &&
			       evt["data"]["source"]["sourceId"] == textId &&
			       client.request(13, "StartBroadcast", json{{"sourceIds", {textId}}}, reply) &&
			       reply.contains("result") &&
			       client.awaitEvent("senderNameResolved", evt, 5000) &&
			       evt["data"]["sourceId"] == textId;
			if (ok10) {
				GateFClient::pumpUntil([] { return false; }, 300);
				ok10 = client.request(14, "GetStatus", nullptr, reply) &&
				       reply.contains("result");
				if (ok10) {
					ok10 = false;
					for (const auto &s : reply["result"]["sources"]) {
						if (s["sourceId"] != textId)
							continue;
						ok10 = s["width"].get<int>() > 0 &&
						       s["height"].get<int>() > 0 &&
						       s["format"] == "srgb87";
					}
				}
			}
		}
		std::printf("  gate-f: text source %s (%s)\n", ok10 ? "ok" : "FAIL", textId.c_str());

		// 11. text descriptors: ListSourceProperties carries a font-typed descriptor, and the
		//     FontValue settings object round-trips (contract FontValue: face/style/size/flags).
		bool ok11 = false;
		if (ok10) {
			ok11 = client.request(15, "ListSourceProperties", json{{"sourceId", textId}},
					      reply) &&
			       reply.contains("result");
			if (ok11) {
				bool fontDescriptor = false;
				for (const auto &p : reply["result"]["properties"])
					fontDescriptor |= (p["type"] == "font");
				ok11 = fontDescriptor && reply["result"]["settings"].contains("font");
			}
			if (ok11) {
				const json fontValue = {{"face", "Arial"}, {"size", 64}, {"flags", 0}};
				ok11 = client.request(16, "SetSourceProperties",
						      json{{"sourceId", textId},
							   {"settings", {{"font", fontValue}}}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["applied"]["font"]["face"] == "Arial" &&
				       reply["result"]["applied"]["font"]["size"] == 64;
			}
			// Cleanup regardless of the assertion outcome so the engine ends idle.
			client.request(17, "RemoveSource", json{{"sourceId", textId}}, reply);
			client.awaitEvent("sourceRemoved", evt, 2000);
		}
		std::printf("  gate-f: text font round-trip %s\n", ok11 ? "ok" : "FAIL");

		// 12. media transport negatives: all three verbs ANSWER (never -32601) and reply the
		//     defined 1012 MediaNotSupported on a source without a media pipeline.
		bool ok12 = false;
		{
			std::string colorId;
			ok12 = client.request(18, "CreateSource",
					      json{{"type", "color"},
						   {"displayName", "GateFTransport"},
						   {"settings", {{"width", 64}, {"height", 36}}}},
					      reply) &&
			       reply.contains("result");
			if (ok12) {
				colorId = reply["result"]["sourceId"].get<std::string>();
				client.awaitEvent("sourceAdded", evt, 2000);
				ok12 = client.request(19, "GetMediaStatus", json{{"sourceId", colorId}},
						      reply) &&
				       reply.contains("error") && reply["error"]["code"] == 1012 &&
				       client.request(20, "ControlMedia",
						      json{{"sourceId", colorId}, {"action", "play"}},
						      reply) &&
				       reply.contains("error") && reply["error"]["code"] == 1012 &&
				       client.request(21, "SeekMedia",
						      json{{"sourceId", colorId}, {"positionMs", 0}},
						      reply) &&
				       reply.contains("error") && reply["error"]["code"] == 1012;
				client.request(22, "RemoveSource", json{{"sourceId", colorId}}, reply);
				client.awaitEvent("sourceRemoved", evt, 2000);
			}
		}
		std::printf("  gate-f: media transport 1012 %s\n", ok12 ? "ok" : "FAIL");

		// 13. mediaChanged is a registered subscribable: a fresh Subscribe acks it.
		bool ok13 = client.request(23, "Subscribe", json{{"events", {"mediaChanged"}}}, reply) &&
			    reply.contains("result") &&
			    reply["result"]["subscribed"] == json::array({"mediaChanged"});
		std::printf("  gate-f: mediaChanged subscribable %s\n", ok13 ? "ok" : "FAIL");

		// 14. propertyChanged end-to-end: every successful settings apply announces itself
		//     with the reply's applied echo, delivered to EVERY subscribed connection --
		//     including the originator -- and the filter-scoped variant carries filterId.
		bool ok14 = false;
		std::string propSourceId;
		{
			GateFClient client2;
			client2.start(port);
			ok14 = GateFClient::pumpUntil([&] { return client2.open.load(); }, 3000) &&
			       client.request(24, "Subscribe", json{{"events", {"propertyChanged"}}},
					      reply) &&
			       reply.contains("result") &&
			       reply["result"]["subscribed"] == json::array({"propertyChanged"}) &&
			       client2.request(1, "Subscribe", json{{"events", {"propertyChanged"}}},
					       reply) &&
			       reply.contains("result");
			if (ok14) {
				ok14 = client.request(25, "CreateSource",
						      json{{"type", "color"},
							   {"displayName", "GateFProps"},
							   {"settings", {{"width", 64}, {"height", 36}}}},
						      reply) &&
				       reply.contains("result");
				if (ok14)
					propSourceId = reply["result"]["sourceId"].get<std::string>();
				client.awaitEvent("sourceAdded", evt, 2000);
			}
			if (ok14) {
				// Source-scoped: both connections see the apply, echo intact, no filterId.
				ok14 = client.request(26, "SetSourceProperties",
						      json{{"sourceId", propSourceId},
							   {"settings", {{"color", 4278255360u}}}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"].contains("settings") &&
				       reply["result"]["properties"].is_array() &&
				       client.awaitEvent("propertyChanged", evt, 2000) &&
				       evt["data"]["sourceId"] == propSourceId &&
				       evt["data"]["applied"]["color"] == 4278255360u &&
				       evt["data"].contains("settings") &&
				       evt["data"]["properties"].is_array() &&
				       !evt["data"].contains("filterId");
				json evt2;
				ok14 = ok14 && client2.awaitEvent("propertyChanged", evt2, 2000) &&
				       evt2["data"]["sourceId"] == propSourceId &&
				       evt2["data"]["applied"]["color"] == 4278255360u;
			}
			if (ok14) {
				// Filter-scoped: the event carries filterId.
				std::string filterId;
				ok14 = client.request(27, "AddFilter",
						      json{{"sourceId", propSourceId},
							   {"filterType", "color_correction"}},
						      reply) &&
				       reply.contains("result");
				if (ok14) {
					filterId = reply["result"]["filterId"].get<std::string>();
					ok14 = client.request(28, "SetFilterProperties",
							      json{{"sourceId", propSourceId},
								   {"filterId", filterId},
								   {"settings", {{"gamma", 0.5}}}},
							      reply) &&
					       reply.contains("result") &&
					       reply["result"].contains("settings") &&
					       reply["result"]["properties"].is_array() &&
					       client.awaitEvent("propertyChanged", evt, 2000) &&
					       evt["data"]["sourceId"] == propSourceId &&
					       evt["data"]["filterId"] == filterId &&
					       evt["data"]["applied"].contains("gamma") &&
					       evt["data"].contains("settings") &&
					       evt["data"]["properties"].is_array();
				}
			}
			client2.ws.stop();
		}
		std::printf("  gate-f: propertyChanged both-connections + filter-scoped %s\n",
			    ok14 ? "ok" : "FAIL");

		// 15. InvokeSourceButton answers (never -32601): 1006 on a non-button property
		//     (data names the property), 1006 on an unknown property, 1001 on an unknown
		//     source.
		bool ok15 = false;
		if (!propSourceId.empty()) {
			ok15 = client.request(29, "InvokeSourceButton",
					      json{{"sourceId", propSourceId}, {"property", "width"}},
					      reply) &&
			       reply.contains("error") && reply["error"]["code"] == 1006 &&
			       reply["error"]["data"]["property"] == "width" &&
			       client.request(30, "InvokeSourceButton",
					      json{{"sourceId", propSourceId},
						   {"property", "no_such_button"}},
					      reply) &&
			       reply.contains("error") && reply["error"]["code"] == 1006 &&
			       client.request(31, "InvokeSourceButton",
					      json{{"sourceId", "src_none"}, {"property", "width"}},
					      reply) &&
			       reply.contains("error") && reply["error"]["code"] == 1001;
			// Cleanup the step-14/15 source regardless of outcome.
			client.request(32, "RemoveSource", json{{"sourceId", propSourceId}}, reply);
			client.awaitEvent("sourceRemoved", evt, 2000);
		}
		std::printf("  gate-f: InvokeSourceButton negatives %s\n", ok15 ? "ok" : "FAIL");
		std::fflush(stdout);

		// 16. Filter-chain wire surface round-trip: ListFilters order (index 0 applies
		//     first; add order == apply order), SetFilterEnabled + filterChanged{enabled},
		//     ReorderFilter + filterChanged{index} + clamp-to-range, SetFilterName +
		//     filterChanged{name}, filterAdded/filterRemoved emission. No-op-emits-nothing
		//     negatives live in the conformance suite (bounded-silence checks need a
		//     second-connection observation window).
		bool ok16 = false;
		{
			std::string chainId, fltA, fltB;
			// Pushes are opt-in per name: subscribe the three filter events first.
			ok16 = client.request(45, "Subscribe",
					      json{{"events",
						    {"filterAdded", "filterRemoved", "filterChanged"}}},
					      reply) &&
			       reply.contains("result") &&
			       reply["result"]["subscribed"] ==
				       json::array({"filterAdded", "filterRemoved", "filterChanged"});
			ok16 = ok16 &&
			       client.request(33, "CreateSource",
					      json{{"type", "color"},
						   {"displayName", "GateFChain"},
						   {"settings", {{"width", 64}, {"height", 36}}}},
					      reply) &&
			       reply.contains("result");
			if (ok16) {
				chainId = reply["result"]["sourceId"].get<std::string>();
				client.awaitEvent("sourceAdded", evt, 2000);
			}
			if (ok16) {
				ok16 = client.request(34, "AddFilter",
						      json{{"sourceId", chainId},
							   {"filterType", "color_correction"}},
						      reply) &&
				       reply.contains("result");
				if (ok16) {
					fltA = reply["result"]["filterId"].get<std::string>();
					ok16 = client.awaitEvent("filterAdded", evt, 2000) &&
					       evt["data"]["sourceId"] == chainId &&
					       evt["data"]["filter"]["filterId"] == fltA &&
					       evt["data"]["filter"]["kind"] == "video" &&
					       evt["data"]["filter"]["enabled"] == true &&
					       evt["data"]["filter"]["index"] == 0;
				}
			}
			if (ok16) {
				ok16 = client.request(35, "AddFilter",
						      json{{"sourceId", chainId},
							   {"filterType", "sharpness"}},
						      reply) &&
				       reply.contains("result");
				if (ok16) {
					fltB = reply["result"]["filterId"].get<std::string>();
					ok16 = client.awaitEvent("filterAdded", evt, 2000) &&
					       evt["data"]["filter"]["filterId"] == fltB &&
					       evt["data"]["filter"]["index"] == 1;
				}
			}
			if (ok16) {
				ok16 = client.request(36, "ListFilters", json{{"sourceId", chainId}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["filters"].size() == 2 &&
				       reply["result"]["filters"][0]["filterId"] == fltA &&
				       reply["result"]["filters"][1]["filterId"] == fltB &&
				       reply["result"]["filters"][1]["index"] == 1;
			}
			if (ok16) {
				ok16 = client.request(37, "SetFilterEnabled",
						      json{{"sourceId", chainId},
							   {"filterId", fltA},
							   {"enabled", false}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["enabled"] == false &&
				       client.awaitEvent("filterChanged", evt, 2000) &&
				       evt["data"]["filterId"] == fltA &&
				       evt["data"]["enabled"] == false &&
				       !evt["data"].contains("name") && !evt["data"].contains("index");
			}
			if (ok16) {
				ok16 = client.request(38, "ReorderFilter",
						      json{{"sourceId", chainId},
							   {"filterId", fltB},
							   {"index", 0}},
						      reply) &&
				       reply.contains("result") && reply["result"]["index"] == 0 &&
				       client.awaitEvent("filterChanged", evt, 2000) &&
				       evt["data"]["filterId"] == fltB && evt["data"]["index"] == 0;
			}
			if (ok16) {
				ok16 = client.request(39, "ListFilters", json{{"sourceId", chainId}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["filters"][0]["filterId"] == fltB &&
				       reply["result"]["filters"][1]["filterId"] == fltA &&
				       reply["result"]["filters"][1]["enabled"] == false;
			}
			if (ok16) {
				ok16 = client.request(40, "SetFilterName",
						      json{{"sourceId", chainId},
							   {"filterId", fltB},
							   {"name", "Sharpen Edges"}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["name"] == "Sharpen Edges" &&
				       client.awaitEvent("filterChanged", evt, 2000) &&
				       evt["data"]["filterId"] == fltB &&
				       evt["data"]["name"] == "Sharpen Edges";
			}
			if (ok16) {
				// Out-of-range index clamps to the valid range (here N-1 == 1).
				ok16 = client.request(41, "ReorderFilter",
						      json{{"sourceId", chainId},
							   {"filterId", fltB},
							   {"index", 99}},
						      reply) &&
				       reply.contains("result") && reply["result"]["index"] == 1;
				client.awaitEvent("filterChanged", evt, 2000); // drain the move event
			}
			if (ok16) {
				ok16 = client.request(42, "RemoveFilter",
						      json{{"sourceId", chainId}, {"filterId", fltA}},
						      reply) &&
				       reply.contains("result") &&
				       client.awaitEvent("filterRemoved", evt, 2000) &&
				       evt["data"]["sourceId"] == chainId &&
				       evt["data"]["filterId"] == fltA;
			}
			if (ok16) {
				ok16 = client.request(43, "ListFilters", json{{"sourceId", chainId}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["filters"].size() == 1 &&
				       reply["result"]["filters"][0]["filterId"] == fltB &&
				       reply["result"]["filters"][0]["index"] == 0;
			}
			if (!chainId.empty()) { // cleanup regardless of outcome
				client.request(44, "RemoveSource", json{{"sourceId", chainId}}, reply);
				client.awaitEvent("sourceRemoved", evt, 2000);
			}
		}
		std::printf("  gate-f: filter-chain verbs + events %s\n", ok16 ? "ok" : "FAIL");
		std::fflush(stdout);

		// 17. Per-source audio state + the audio events: defaults on a fresh source, the
		//     SetSourceAudio round-trip (full-state echo), audioChanged on a SECOND
		//     connection with changed-fields-only, the no-op silence, the Subscribe ack for
		//     both audio event names, and the ~10 Hz audioLevels cadence + shape while
		//     subscribed (the engine runs on the test sink -- no endpoint involved).
		bool ok17 = false;
		{
			std::string audioId;
			GateFClient client3;
			client3.start(port);
			ok17 = GateFClient::pumpUntil([&] { return client3.open.load(); }, 3000) &&
			       client3.request(1, "Subscribe", json{{"events", {"audioChanged"}}},
					       reply) &&
			       reply.contains("result") &&
			       reply["result"]["subscribed"] == json::array({"audioChanged"}) &&
			       client.request(48, "Subscribe",
					      json{{"events", {"audioChanged", "audioLevels"}}}, reply) &&
			       reply.contains("result") &&
			       reply["result"]["subscribed"] ==
				       json::array({"audioChanged", "audioLevels"});
			ok17 = ok17 &&
			       client.request(49, "CreateSource",
					      json{{"type", "color"},
						   {"displayName", "GateFAudio"},
						   {"settings", {{"width", 64}, {"height", 36}}}},
					      reply) &&
			       reply.contains("result");
			if (ok17) {
				audioId = reply["result"]["sourceId"].get<std::string>();
				client.awaitEvent("sourceAdded", evt, 2000);
				// Defaults: unity gain, unmuted, centered balance.
				ok17 = client.request(50, "GetSourceAudio", json{{"sourceId", audioId}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["gain"] == 1.0 &&
				       reply["result"]["muted"] == false &&
				       reply["result"]["balance"] == 0.5;
			}
			if (ok17) {
				// Round-trip: the reply is the FULL applied state; the event carries
				// the changed fields only (balance untouched -> absent).
				ok17 = client.request(51, "SetSourceAudio",
						      json{{"sourceId", audioId},
							   {"gain", 0.25},
							   {"muted", true}},
						      reply) &&
				       reply.contains("result") &&
				       reply["result"]["gain"] == 0.25 &&
				       reply["result"]["muted"] == true &&
				       reply["result"]["balance"] == 0.5 &&
				       client3.awaitEvent("audioChanged", evt, 2500) &&
				       evt["data"]["sourceId"] == audioId &&
				       evt["data"]["gain"] == 0.25 && evt["data"]["muted"] == true &&
				       !evt["data"].contains("balance");
			}
			if (ok17) {
				// Setting the current values is a no-op: success reply, no event.
				ok17 = client.request(52, "SetSourceAudio",
						      json{{"sourceId", audioId}, {"gain", 0.25}},
						      reply) &&
				       reply.contains("result") &&
				       !client3.awaitEvent("audioChanged", evt, 1000);
			}
			if (ok17) {
				// audioLevels cadence: subscribed -> ~10 Hz pushes with the contract
				// shape; the unsubscribed second connection receives NONE.
				int got = 0;
				bool shapeOk = true;
				if (client.awaitEvent("audioLevels", evt, 2500)) {
					got = 1;
					while (got < 12 && client.awaitEvent("audioLevels", evt, 250))
						got++;
					shapeOk = evt["data"].contains("master") &&
						  evt["data"]["master"].contains("peak") &&
						  evt["data"]["master"].contains("rms") &&
						  evt["data"]["master"]["clipped"].is_boolean() &&
						  evt["data"]["sources"].is_array();
				}
				json stray;
				ok17 = got >= 8 && shapeOk &&
				       !client3.take(
					       [](const nlohmann::json &j) {
						       return j.contains("event") &&
							      j["event"] == "audioLevels";
					       },
					       stray, 200);
				if (!ok17)
					std::printf("  gate-f: audioLevels got=%d shapeOk=%d\n", got,
						    int(shapeOk));
			}
			if (!audioId.empty()) { // cleanup regardless of outcome
				client.request(53, "RemoveSource", json{{"sourceId", audioId}}, reply);
				client.awaitEvent("sourceRemoved", evt, 2000);
			}
			client3.ws.stop();
		}
		std::printf("  gate-f: source audio + audio events %s\n", ok17 ? "ok" : "FAIL");
		std::fflush(stdout);

		// 18. Shutdown (contract 1.5.0) reply CONTRACT -- drain echo over all three input forms +
		//     the idempotent re-ack. Dispatched directly on THROWAWAY ControlVerbs so the live WS
		//     server/event loop is untouched; the verb schedules a QUEUED QCoreApplication::quit(),
		//     harmless here (the selftest runs no event loop -- there is nothing to exit). A fresh
		//     instance per drain variant so each tests its OWN first-Shutdown commit.
		const auto shutdownReply = [&](bool hasDrain, bool drainVal) {
			ControlVerbs v(&engine, &audio, identity);
			json p = json::object();
			if (hasDrain)
				p["drain"] = drainVal;
			return v.dispatch(json(70), "Shutdown", p);
		};
		const json sdDefault = shutdownReply(false, false); // drain omitted -> effective true
		const json sdTrue = shutdownReply(true, true);
		const json sdFalse = shutdownReply(true, false);
		bool ok18a = sdDefault.contains("result") && sdDefault["result"]["accepted"] == true &&
			     sdDefault["result"]["drain"] == true &&
			     !sdDefault["result"].contains("alreadyStopping") &&
			     sdTrue.contains("result") && sdTrue["result"]["drain"] == true &&
			     sdFalse.contains("result") && sdFalse["result"]["drain"] == false;
		// Idempotent re-ack on the SAME instance: a later drain:false cannot un-drain; the first
		// committed (true) is echoed and alreadyStopping is set -- NOT a 1009 error.
		bool ok18b = false;
		{
			ControlVerbs v(&engine, &audio, identity);
			const json first = v.dispatch(json(71), "Shutdown", json{{"drain", true}});
			const json again = v.dispatch(json(72), "Shutdown", json{{"drain", false}});
			ok18b = first.contains("result") && !first["result"].contains("alreadyStopping") &&
				again.contains("result") && again["result"]["alreadyStopping"] == true &&
				again["result"]["drain"] == true && again["result"]["accepted"] == true;
		}
		bool ok18 = ok18a && ok18b;
		std::printf("  gate-f: Shutdown drain variants + idempotent re-ack %s\n", ok18 ? "ok" : "FAIL");

		// 19. Discovery-file shape: the bare single-instance object round-trips through
		//     HelperConfig::serialize() with EXACTLY the canonical eight-field set
		//     (instanceId, port, version, fpsTier, spoutPrefix, ownerId, controlToken, sources[]).
		//     This gates the on-disk discovery contract control clients read; a field dropped,
		//     renamed, or re-wrapped in an "instances" array must fail here. The cleared
		//     (no-helper) state is a bare empty object.
		bool ok19 = false;
		{
			using moxrelay::HelperConfig;
			using moxrelay::HelperInstance;

			HelperInstance inst;
			inst.instanceId = "tier-60";
			inst.port = 7341;
			inst.version = "0.7.0";
			inst.fpsTier = 60;
			inst.spoutPrefix = "M:Helper_7341";
			inst.ownerId = "owner-x";
			inst.controlToken = "a3f81c0e7d2b49f6a3f81c0e7d2b49f6";
			inst.sources = {"M:Helper_7341_Desktop"};

			const json doc =
				json::parse(HelperConfig::serialize(inst, /*pretty=*/false), nullptr,
					    /*allow_exceptions=*/false);
			const std::set<std::string> expected = {"instanceId", "port",     "version",
								 "fpsTier",    "spoutPrefix", "ownerId",
								 "controlToken", "sources"};
			std::set<std::string> got;
			if (doc.is_object())
				for (auto it = doc.begin(); it != doc.end(); ++it)
					got.insert(it.key());
			const bool shapeOk = doc.is_object() && got == expected &&
					     doc["instanceId"] == "tier-60" && doc["port"] == 7341 &&
					     doc["controlToken"] == "a3f81c0e7d2b49f6a3f81c0e7d2b49f6" &&
					     doc["sources"].is_array() && doc["sources"].size() == 1 &&
					     doc["sources"][0].is_object() &&
					     doc["sources"][0]["name"] == "M:Helper_7341_Desktop";

			const json emptyDoc =
				json::parse(HelperConfig::serializeEmpty(/*pretty=*/false), nullptr,
					    /*allow_exceptions=*/false);
			const bool emptyOk = emptyDoc.is_object() && emptyDoc.empty();

			ok19 = shapeOk && emptyOk;
		}
		std::printf("  gate-f: discovery-file bare-object shape (8 fields + cleared) %s\n",
			    ok19 ? "ok" : "FAIL");
		std::fflush(stdout);

		// 20. externalId (client-supplied stable id) round-trips through BOTH paths:
		//     (a) the durable BOOT/RESPAWN path -- a config-JSON source carrying "externalId" is
		//         created via the factory and ingested through adoptBootSources() exactly as a
		//         worker respawn does, then ListSources echoes the SAME externalId verbatim; a
		//         sibling boot source with NO externalId echoes null (absent == legacy);
		//     (b) the runtime CreateSource path -- the verb param echoes back in its own reply and
		//         in ListSources. Omitting it keeps senderName-style null behavior.
		bool ok20 = false;
		{
			using moxrelay::CreatedSource;
			using moxrelay::SourceConfigResult;
			using moxrelay::SourceFactory;

			const char *bootJson = R"json({
			  "port": 7341,
			  "sources": [
			    {"id": "color_source_v3", "name": "BootWithExt", "externalId": "ext-boot-123",
			     "settings": {"color": 4280303808, "width": 64, "height": 64}},
			    {"id": "color_source_v3", "name": "BootNoExt",
			     "settings": {"color": 4278255360, "width": 64, "height": 64}}
			  ]
			})json";
			SourceConfigResult boot = SourceFactory::createFromConfigJson(bootJson);
			bool factoryOk = boot.ok && boot.sources.size() == 2 &&
					 boot.sources[0].externalId == "ext-boot-123" &&
					 boot.sources[1].externalId.empty();

			// Ingest exactly as the worker boot/respawn does (not attached: bind-only adoption).
			if (factoryOk)
				verbs.adoptBootSources(std::move(boot.sources), /*attachedInOrder=*/false);

			// (a) ListSources echoes the booted externalId verbatim; the sibling echoes null.
			bool bootEcho = false;
			if (factoryOk && client.request(80, "ListSources", nullptr, reply) &&
			    reply.contains("result") && reply["result"]["sources"].is_array()) {
				const json &srcs = reply["result"]["sources"];
				bool withSeen = false, withoutSeen = false;
				for (const auto &s : srcs) {
					if (s.value("displayName", "") == "BootWithExt")
						withSeen = s.contains("externalId") &&
							   s["externalId"] == "ext-boot-123";
					if (s.value("displayName", "") == "BootNoExt")
						withoutSeen = s.contains("externalId") &&
							      s["externalId"].is_null();
				}
				bootEcho = withSeen && withoutSeen;
			}

			// (b) runtime CreateSource: the param echoes in the reply AND in ListSources.
			bool runtimeEcho = false;
			if (client.request(81, "CreateSource",
					   json{{"type", "color"},
						{"displayName", "RuntimeWithExt"},
						{"externalId", "ext-rt-456"},
						{"settings", {{"color", 4286611456u}, {"width", 64}, {"height", 64}}}},
					   reply) &&
			    reply.contains("result") && reply["result"].value("externalId", "") == "ext-rt-456") {
				const std::string rtId = reply["result"]["sourceId"].get<std::string>();
				if (client.request(82, "ListSources", nullptr, reply) && reply.contains("result")) {
					for (const auto &s : reply["result"]["sources"]) {
						if (s.value("sourceId", "") == rtId)
							runtimeEcho = s.contains("externalId") &&
								      s["externalId"] == "ext-rt-456";
					}
				}
			}

			ok20 = factoryOk && bootEcho && runtimeEcho;
			std::printf("  gate-f: externalId round-trip (boot-respawn + runtime) %s\n",
				    ok20 ? "ok" : "FAIL");
			std::fflush(stdout);
		}

		client.ws.stop();
		server.stop();
		server.stop(); // idempotence assertion, same as the engine/bootstrap gates

		pass = okAuth4401 && okOrigin4403 && ok1 && ok2 && ok3 && ok4 && ok5 && ok5b && ok6 &&
		       ok7 && ok8 && ok9 && ok10 && ok11 && ok12 && ok13 && ok14 && ok15 && ok16 &&
		       ok17 && ok18 && ok19 && ok20;
	} while (false);

	audio.stop();
	audio.stop(); // idempotence assertion
	engine.stop();
	engine.stop(); // idempotence assertion

	std::printf("GATE F (control endpoint): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE G (audio engine, in-process + device-independent): the audio production path end to end
// without any audio endpoint -- the engine's TEST SINK seam swaps the WASAPI client for an
// in-process capture of the mixed output, so the full path (tap -> ring -> mixer -> fades ->
// fill servo -> resampler -> meters) is asserted deterministically. The gate generates its own
// 440 Hz tone WAV (exactly 1760 cycles over 4 s at 48 kHz -- the loop joint is phase-continuous
// by construction), plays it through a real media source gated on the active edge, and asserts:
//   energy     -- the attached clip's tone reaches the mix within a bounded wait (a cold
//                 demuxer first-open can take tens of seconds).
//   gain/mute  -- volume 0.25 scales the measured RMS ~4x down; mute zeroes the contribution
//                 AND the per-source meter; both transitions (and a seek-induced timestamp
//                 jump, and the detach) keep the max inter-sample delta within the steady
//                 tone's own baseline -- a step transient FAILS the gate.
//   servo      -- ring fill converges to and holds the steady-state band; zero producer drops,
//                 zero underruns, zero resync events across the run.
//   meters     -- tone RMS/peak within tolerance of the analytic values (amp 0.5 sine: RMS
//                 0.354, peak 0.5); detach returns the mix to silence.
// ---------------------------------------------------------------------------------------------
namespace {

// Minimal PCM16 stereo RIFF/WAVE writer for the gate tone.
bool write_tone_wav(const std::string &path, double freqHz, double seconds, double amplitude)
{
	const uint32_t rate = 48000;
	const uint16_t channels = 2, bits = 16;
	const uint32_t frames = uint32_t(rate * seconds);
	const uint32_t dataBytes = frames * channels * (bits / 8);
	std::vector<uint8_t> wav;
	wav.reserve(44 + dataBytes);
	auto put32 = [&](uint32_t v) {
		for (int i = 0; i < 4; ++i)
			wav.push_back(uint8_t(v >> (i * 8)));
	};
	auto put16 = [&](uint16_t v) {
		for (int i = 0; i < 2; ++i)
			wav.push_back(uint8_t(v >> (i * 8)));
	};
	auto putTag = [&](const char *t) { wav.insert(wav.end(), t, t + 4); };
	putTag("RIFF");
	put32(36 + dataBytes);
	putTag("WAVE");
	putTag("fmt ");
	put32(16);
	put16(1); // PCM
	put16(channels);
	put32(rate);
	put32(rate * channels * (bits / 8));
	put16(channels * (bits / 8));
	put16(bits);
	putTag("data");
	put32(dataBytes);
	for (uint32_t i = 0; i < frames; ++i) {
		const double s = amplitude * std::sin(2.0 * 3.14159265358979323846 * freqHz * double(i) /
						      double(rate));
		const auto v = uint16_t(int16_t(std::lround(s * 32767.0)));
		put16(v);
		put16(v);
	}
	FILE *f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
		return false;
	const size_t n = std::fwrite(wav.data(), 1, wav.size(), f);
	std::fclose(f);
	return n == wav.size();
}

// In-process capture of the engine's mixed output (interleaved stereo float at 48 kHz). Keeps
// the most recent ~30 s; windows are addressed by absolute frame position.
struct GateGSink {
	std::mutex mutex;
	std::vector<float> samples;
	uint64_t total = 0; // frames ever delivered

	void append(const float *interleaved, size_t frames)
	{
		std::lock_guard<std::mutex> lock(mutex);
		samples.insert(samples.end(), interleaved, interleaved + frames * 2);
		total += frames;
		const size_t maxFloats = size_t(48000) * 2 * 30;
		if (samples.size() > maxFloats)
			samples.erase(samples.begin(), samples.begin() + (samples.size() - maxFloats));
	}

	uint64_t pos()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return total;
	}

	// Interleaved copy of frames [from, to); clamped to the retained span.
	std::vector<float> window(uint64_t from, uint64_t to)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const uint64_t kept = samples.size() / 2;
		const uint64_t first = total - kept;
		if (from < first)
			from = first;
		if (to > total)
			to = total;
		if (to <= from)
			return {};
		const size_t off = size_t(from - first) * 2;
		const size_t len = size_t(to - from) * 2;
		return std::vector<float>(samples.begin() + off, samples.begin() + off + len);
	}
};

double gate_g_rms(const std::vector<float> &w)
{
	if (w.empty())
		return 0.0;
	double sum = 0.0;
	for (float v : w)
		sum += double(v) * double(v);
	return std::sqrt(sum / double(w.size()));
}

// Max per-channel inter-sample delta -- the step-transient detector.
double gate_g_max_delta(const std::vector<float> &w)
{
	double maxDelta = 0.0;
	for (size_t i = 2; i + 1 < w.size(); i += 2) {
		const double dl = std::fabs(double(w[i]) - double(w[i - 2]));
		const double dr = std::fabs(double(w[i + 1]) - double(w[i - 1]));
		if (dl > maxDelta)
			maxDelta = dl;
		if (dr > maxDelta)
			maxDelta = dr;
	}
	return maxDelta;
}

} // namespace

static bool run_gate_g()
{
	using moxrelay::AudioMixEngine;
	using moxrelay::SourceFactory;
	using nlohmann::json;

	// Unique per invocation: the previous source's release is asynchronous, so a fixed name
	// can still be held open (Windows write-share) when the next invocation rewrites it.
	static std::atomic<int> toneSeq{0};
	const std::string wavPath = QDir::tempPath().toStdString() + "/moxrelay-selftest-tone-" +
				    std::to_string(toneSeq.fetch_add(1)) + ".wav";
	if (!write_tone_wav(wavPath, 440.0, 4.0, 0.5)) {
		std::printf("GATE G (audio engine): FAIL (tone WAV write failed)\n");
		std::fflush(stdout);
		return false;
	}

	const json doc = {{"port", 7341},
			  {"sources", json::array({json{{"id", "ffmpeg_source"},
							{"name", "GateGTone"},
							{"settings",
							 {{"local_file", wavPath},
							  {"is_local_file", true},
							  {"looping", true}}}}})}};
	moxrelay::SourceConfigResult cfg = SourceFactory::createFromConfigJson(doc.dump().c_str());
	if (!cfg.ok || cfg.sources.size() != 1) {
		std::printf("GATE G (audio engine): FAIL (media source: %s)\n",
			    cfg.error.empty() ? "wrong source count" : cfg.error.c_str());
		std::fflush(stdout);
		std::remove(wavPath.c_str());
		return false;
	}
	obs_source_t *src = cfg.sources.front().source;
	obs_source_t *src2 = nullptr; // the twin-tap phase's second tone source (step 6b)
	bool active2 = false;
	std::string wav2Path;

	auto sink = std::make_shared<GateGSink>();
	AudioMixEngine audio;
	audio.setTestSink([sink](const float *d, size_t f) { sink->append(d, f); });

	const auto sleepMs = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
	const auto lastRms = [&](double seconds) {
		const uint64_t p = sink->pos();
		const auto frames = uint64_t(seconds * 48000);
		return gate_g_rms(sink->window(p > frames ? p - frames : 0, p));
	};

	bool pass = false;
	bool active = false;
	do {
		if (!audio.start())
			break;
		obs_source_inc_active(src); // the same active edge a sender attach raises
		active = true;
		audio.attachSource(src, "gate_g");

		// 1. Energy: the clip becomes audible in the mix within a bounded wait.
		bool energy = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < deadline) {
			if (lastRms(0.25) > 0.1) {
				energy = true;
				break;
			}
			sleepMs(100);
		}
		std::printf("  gate-g: tone energy %s (rms=%.3f)\n", energy ? "ok" : "FAIL", lastRms(0.25));
		if (!energy)
			break;

		// 2. Steady baseline: the tone's own inter-sample delta bounds every later
		//    transition (analytic max slope of a 440 Hz amp-0.5 sine at 48 kHz: ~0.029).
		sleepMs(1500);
		const uint64_t pBase = sink->pos();
		const auto base = sink->window(pBase - 48000, pBase);
		const double baseRms = gate_g_rms(base);
		const double baseDelta = gate_g_max_delta(base);
		const double thr = baseDelta * 2.0; // a step transient is >10x this
		std::printf("  gate-g: baseline rms=%.4f maxDelta=%.4f (continuity threshold %.4f)\n",
			    baseRms, baseDelta, thr);
		const bool baseOk = baseRms > 0.2 && baseDelta > 0.001 && baseDelta < 0.1;
		if (!baseOk)
			break;

		// 3. Gain change: continuity across the transition + ~4x RMS scale-down.
		const uint64_t pGain = sink->pos();
		obs_source_set_volume(src, 0.25f);
		sleepMs(1000);
		const double gainDelta =
			gate_g_max_delta(sink->window(pGain > 4800 ? pGain - 4800 : 0, pGain + 24000));
		const double gainRms = lastRms(0.5);
		const double ratio = gainRms > 0.0 ? baseRms / gainRms : 0.0;
		const bool gainOk = gainDelta <= thr && ratio > 3.0 && ratio < 5.5;
		std::printf("  gate-g: gain 0.25 %s (maxDelta=%.4f rms=%.4f ratio=%.2f)\n",
			    gainOk ? "ok" : "FAIL", gainDelta, gainRms, ratio);
		if (!gainOk)
			break;

		// 4. Mute: continuity across the fade, silent contribution, zeroed source meter.
		audio.stats(); // drain the meter window
		const uint64_t pMute = sink->pos();
		obs_source_set_muted(src, true);
		sleepMs(700);
		const double muteDelta =
			gate_g_max_delta(sink->window(pMute > 4800 ? pMute - 4800 : 0, pMute + 24000));
		const double muteRms = lastRms(0.25);
		audio.stats(); // drain the window spanning the fade
		sleepMs(400);
		const auto muteStats = audio.stats();
		const bool meterZero = !muteStats.sources.empty() &&
				       muteStats.sources[0].rms < 0.005 && muteStats.sources[0].peak < 0.01;
		const bool muteOk = muteDelta <= thr && muteRms < 0.01 && meterZero;
		std::printf("  gate-g: mute %s (maxDelta=%.4f rms=%.4f meterRms=%.4f meterPeak=%.4f)\n",
			    muteOk ? "ok" : "FAIL", muteDelta, muteRms,
			    muteStats.sources.empty() ? -1.0f : muteStats.sources[0].rms,
			    muteStats.sources.empty() ? -1.0f : muteStats.sources[0].peak);
		if (!muteOk)
			break;

		// 5. Unmute + unity gain, then a seek-induced timestamp jump stays continuous.
		obs_source_set_muted(src, false);
		obs_source_set_volume(src, 1.0f);
		sleepMs(1200);
		if (lastRms(0.5) < 0.2) {
			std::printf("  gate-g: unmute FAIL (rms=%.4f)\n", lastRms(0.5));
			break;
		}
		const uint64_t pSeek = sink->pos();
		// Production parity: every user-reachable seek passes the verb layer, which stamps
		// the transport hint (engine smoothing can re-time post-seek pushes continuous, so
		// the tap-side jump bracket alone cannot always classify the dry-out).
		audio.noteSourceTransport(src);
		obs_source_media_set_time(src, 500);
		sleepMs(1000);
		const double seekDelta =
			gate_g_max_delta(sink->window(pSeek > 4800 ? pSeek - 4800 : 0, pSeek + 28800));
		const bool seekOk = seekDelta <= thr;
		std::printf("  gate-g: seek continuity %s (maxDelta=%.4f)\n", seekOk ? "ok" : "FAIL",
			    seekDelta);
		if (!seekOk)
			break;

		// 6. Servo/steady run: the regulated fill TROUGH (the just-before-push minimum;
		//    target 15 ms) converges to and holds its band -- the mean fill rides the
		//    producer's push-block size above it by physics and is asserted bounded. The
		//    counters stay at zero across everything above (the seek's dry-out, if any,
		//    classifies as a transport gap, never an underrun -- the hint above carries
		//    that even when smoothing absorbs the tap-side timestamp jump).
		// The bound is calibrated to the ENGINE's contract, not a flat band: the trim
		// backstop deliberately tolerates up to 3 consecutive windows above its threshold
		// (~target+4 ms) before shedding, and a block-paced producer puts an isolated
		// window's trough one push block high by design. A REGRESSION therefore shows as
		// trough excursions the trim never sheds (4+ consecutive high windows -- the
		// standing-fill defect class), a floor breach (under-cushion), or an insane single
		// window; isolated in-patience excursions are healthy regulation.
		bool troughOk = true;
		bool trimOk = true;
		bool meanOk = true;
		double trMin = 1e9, trMax = 0.0;
		int consecHigh = 0, worstRun = 0;
		// Sample the ENGINE's own servo windows, each exactly once, keyed on the
		// monotonic window counter: a free-running 250 ms poll drifts through the
		// engine's 250 ms cadence and can read one window twice (or skip one),
		// inflating a 3-window in-patience excursion into a phantom 4-run. The
		// double-read guard re-snapshots when a window boundary lands between the
		// two loads of a snapshot.
		int consumed = 0;
		uint64_t lastWin = audio.stats().servoWindows;
		const auto servoEnd = std::chrono::steady_clock::now() + std::chrono::seconds(14);
		while (consumed < 24 && std::chrono::steady_clock::now() < servoEnd) {
			sleepMs(50);
			auto st = audio.stats();
			if (st.servoWindows == lastWin)
				continue;
			const auto st2 = audio.stats();
			if (st2.servoWindows != st.servoWindows)
				continue; // window boundary landed inside the snapshot; re-poll
			lastWin = st.servoWindows;
			consumed++;
			if (consumed > 12) { // judge the settled tail only
				trMin = std::min(trMin, st.fillMinMs);
				trMax = std::max(trMax, st.fillMinMs);
				if (st.fillMinMs < 8.0 || st.fillMinMs > 35.0)
					troughOk = false;
				if (st.fillMinMs > 20.0) {
					consecHigh++;
					worstRun = std::max(worstRun, consecHigh);
					if (consecHigh > 3)
						trimOk = false; // the trim's patience is 3 windows
				} else {
					consecHigh = 0;
				}
				if (st.fillMs > 120.0)
					meanOk = false;
			}
		}
		if (consumed < 24) {
			troughOk = false; // the engine stopped publishing windows -- a real failure
			std::printf("  gate-g: servo windows stalled (consumed=%d)\n", consumed);
		}
		const auto servoStats = audio.stats();
		const bool countersOk = servoStats.producerDrops == 0 && servoStats.underruns == 0 &&
					servoStats.resyncs == 0;
		const bool servoOk = troughOk && trimOk && meanOk && countersOk &&
				     std::fabs(servoStats.servoPpm) < 500.0;
		std::printf("  gate-g: servo %s (trough=%.1f..%.1fms highrun=%d fill=%.1fms ppm=%+.0f drops=%llu "
			    "underruns=%llu gaps=%llu resyncs=%llu)\n",
			    servoOk ? "ok" : "FAIL", trMin, trMax, worstRun, servoStats.fillMs, servoStats.servoPpm,
			    (unsigned long long)servoStats.producerDrops,
			    (unsigned long long)servoStats.underruns,
			    (unsigned long long)servoStats.transportGaps,
			    (unsigned long long)servoStats.resyncs);
		if (!servoOk)
			break;

		// 6b. Two concurrent taps: a second tone source attaches, then BOTH sources seek
		//     back-to-back inside one servo window -- a standing-fill step can land on two
		//     rings at once, so deferred shed orders can coexist, and a shed resolving on
		//     one tap must retire the sibling's order (never shed its healthy fill later;
		//     a stale order would dive the sibling's trough and dry it, which the
		//     underrun/trough asserts below catch live). Continuity is re-baselined on the
		//     two-tone mix; the window-keyed sampling re-asserts the same trim contract
		//     with two rings live, thresholds unchanged.
		static std::atomic<int> tone2Seq{0};
		wav2Path = QDir::tempPath().toStdString() + "/moxrelay-selftest-tone2-" +
			   std::to_string(tone2Seq.fetch_add(1)) + ".wav";
		if (!write_tone_wav(wav2Path, 880.0, 4.0, 0.25)) {
			std::printf("  gate-g: twin-tap FAIL (tone2 WAV write failed)\n");
			break;
		}
		const json doc2 = {{"port", 7341},
				   {"sources", json::array({json{{"id", "ffmpeg_source"},
								 {"name", "GateGTone2"},
								 {"settings",
								  {{"local_file", wav2Path},
								   {"is_local_file", true},
								   {"looping", true}}}}})}};
		moxrelay::SourceConfigResult cfg2 =
			SourceFactory::createFromConfigJson(doc2.dump().c_str());
		if (!cfg2.ok || cfg2.sources.size() != 1) {
			std::printf("  gate-g: twin-tap FAIL (media source 2: %s)\n",
				    cfg2.error.empty() ? "wrong source count" : cfg2.error.c_str());
			break;
		}
		src2 = cfg2.sources.front().source;
		obs_source_inc_active(src2); // the same active edge a sender attach raises
		active2 = true;
		audio.attachSource(src2, "gate_g2");
		bool energy2 = false;
		const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline2) {
			const auto st = audio.stats();
			if (st.sources.size() >= 2 && st.sources[1].rms > 0.05) {
				energy2 = true;
				break;
			}
			sleepMs(100);
		}
		std::printf("  gate-g: twin-tap energy %s\n", energy2 ? "ok" : "FAIL");
		if (!energy2)
			break;
		sleepMs(1000);
		const uint64_t pTwin = sink->pos();
		const double twinThr = gate_g_max_delta(sink->window(pTwin - 48000, pTwin)) * 2.0;
		// Both seeks pass the verb-layer transport hint (production parity, as in step 5)
		// and land well inside one 250 ms servo window.
		audio.noteSourceTransport(src);
		obs_source_media_set_time(src, 500);
		audio.noteSourceTransport(src2);
		obs_source_media_set_time(src2, 500);
		sleepMs(1200);
		const double twinSeekDelta =
			gate_g_max_delta(sink->window(pTwin > 4800 ? pTwin - 4800 : 0, pTwin + 57600));
		const bool twinSeekOk = twinSeekDelta <= twinThr;
		std::printf("  gate-g: twin-tap seek continuity %s (maxDelta=%.4f thr=%.4f)\n",
			    twinSeekOk ? "ok" : "FAIL", twinSeekDelta, twinThr);
		if (!twinSeekOk)
			break;
		// Window-keyed trough sampling with two live rings (the step-6 protocol). The head
		// windows carry the post-seek shed-resolution transient -- a standing fill on a
		// block-paced ring resolves on the staircase of window/push-cycle alignments, not
		// within the latch patience -- so the judged SETTLED TAIL starts later than step
		// 6's (the double seek doubles the resolution work); thresholds are identical.
		bool twinTroughOk = true;
		bool twinTrimOk = true;
		double twinMin = 1e9, twinMax = 0.0;
		int twinHigh = 0, twinWorst = 0, twinConsumed = 0;
		uint64_t twinLastWin = audio.stats().servoWindows;
		const auto twinEnd = std::chrono::steady_clock::now() + std::chrono::seconds(12);
		while (twinConsumed < 20 && std::chrono::steady_clock::now() < twinEnd) {
			sleepMs(50);
			auto st = audio.stats();
			if (st.servoWindows == twinLastWin)
				continue;
			const auto st2 = audio.stats();
			if (st2.servoWindows != st.servoWindows)
				continue; // window boundary landed inside the snapshot; re-poll
			twinLastWin = st.servoWindows;
			twinConsumed++;
			if (twinConsumed > 12) { // judge the settled tail only
				twinMin = std::min(twinMin, st.fillMinMs);
				twinMax = std::max(twinMax, st.fillMinMs);
				if (st.fillMinMs < 8.0 || st.fillMinMs > 35.0)
					twinTroughOk = false;
				if (st.fillMinMs > 20.0) {
					twinHigh++;
					twinWorst = std::max(twinWorst, twinHigh);
					if (twinHigh > 3)
						twinTrimOk = false; // the trim's patience is 3 windows
				} else {
					twinHigh = 0;
				}
			}
		}
		if (twinConsumed < 20) {
			twinTroughOk = false; // the engine stopped publishing windows
			std::printf("  gate-g: twin-tap windows stalled (consumed=%d)\n", twinConsumed);
		}
		const auto twinStats = audio.stats();
		const bool twinCountersOk = twinStats.producerDrops == 0 &&
					    twinStats.underruns == 0 && twinStats.resyncs == 0;
		const bool twinOk = twinTroughOk && twinTrimOk && twinCountersOk;
		std::printf("  gate-g: twin-tap servo %s (trough=%.1f..%.1fms highrun=%d "
			    "underruns=%llu gaps=%llu resyncs=%llu)\n",
			    twinOk ? "ok" : "FAIL", twinMin, twinMax, twinWorst,
			    (unsigned long long)twinStats.underruns,
			    (unsigned long long)twinStats.transportGaps,
			    (unsigned long long)twinStats.resyncs);
		if (!twinOk)
			break;
		// Detach the second tap (fade-wrapped) and settle back to the single tone before
		// the meter step.
		const uint64_t pDetach2 = sink->pos();
		audio.detachSource(src2);
		sleepMs(500);
		const double detach2Delta = gate_g_max_delta(
			sink->window(pDetach2 > 4800 ? pDetach2 - 4800 : 0, pDetach2 + 19200));
		const bool detach2Ok = detach2Delta <= twinThr;
		std::printf("  gate-g: twin-tap detach %s (maxDelta=%.4f)\n",
			    detach2Ok ? "ok" : "FAIL", detach2Delta);
		if (!detach2Ok)
			break;

		// 6c. Per-source sync offset: a live offset increase re-primes through the refill
		//     machinery -- the audible gap IS the added delay (fade-wrapped at both ends,
		//     never a click). Transport accounting: ONE deliberate gap from the re-prime,
		//     plus AT MOST one orbit-landing trim shed (the re-primed orbit lands where
		//     the refill phase puts it; the trim takes its designed one bite to the
		//     target -- the same one-time landing the boot prime takes), and ZERO
		//     underruns. The regulated trough re-converges to the SAME band on the
		//     delay-exempt basis while the raw mean fill carries the reserve (the proof
		//     the reserve is resident AND exempt); the sink and meters follow the HEARD
		//     signal through the gap; the decrease back to zero is one fade-wrapped
		//     splice removing exactly the reserve, gap-free. Thresholds identical to
		//     step 6.
		const auto preKnob = audio.stats();
		const uint64_t pKnob = sink->pos();
		audio.setSourceSyncOffset(src, 120);
		sleepMs(1100);
		// The heard gap: ~50..110 ms after the set lies inside the reserve refill (the
		// fade-out completes within ~25 ms; the 120 ms reserve finishes accumulating
		// ~130+ ms in), so this window is silence between the two fades.
		const double knobGapRms = gate_g_rms(sink->window(pKnob + 2400, pKnob + 5280));
		const double knobDelta = gate_g_max_delta(
			sink->window(pKnob > 4800 ? pKnob - 4800 : 0, pKnob + 48000));
		const double knobRms = lastRms(0.5);
		bool knobTroughOk = true;
		bool knobMeanOk = false; // must SEE the reserve in the raw mean at least once
		double knobMin = 1e9, knobMax = 0.0;
		int knobConsumed = 0;
		uint64_t knobLastWin = audio.stats().servoWindows;
		const auto knobEnd = std::chrono::steady_clock::now() + std::chrono::seconds(8);
		while (knobConsumed < 12 && std::chrono::steady_clock::now() < knobEnd) {
			sleepMs(50);
			auto st = audio.stats();
			if (st.servoWindows == knobLastWin)
				continue;
			const auto st2 = audio.stats();
			if (st2.servoWindows != st.servoWindows)
				continue; // window boundary landed inside the snapshot; re-poll
			knobLastWin = st.servoWindows;
			knobConsumed++;
			if (knobConsumed > 6) { // judge the settled tail only
				knobMin = std::min(knobMin, st.fillMinMs);
				knobMax = std::max(knobMax, st.fillMinMs);
				if (st.fillMinMs < 8.0 || st.fillMinMs > 35.0)
					knobTroughOk = false;
				if (st.fillMs > 100.0 && st.fillMs < 300.0)
					knobMeanOk = true;
			}
		}
		if (knobConsumed < 12) {
			knobTroughOk = false; // the engine stopped publishing windows
			std::printf("  gate-g: sync-offset windows stalled (consumed=%d)\n",
				    knobConsumed);
		}
		const auto knobStats = audio.stats();
		const uint64_t knobGaps = knobStats.transportGaps - preKnob.transportGaps;
		const bool knobOk = knobGapRms < 0.01 && knobDelta <= thr && knobRms > 0.2 &&
				    knobTroughOk && knobMeanOk && knobGaps >= 1 && knobGaps <= 2 &&
				    knobStats.underruns == preKnob.underruns;
		std::printf("  gate-g: sync-offset 120 %s (gapRms=%.4f maxDelta=%.4f rms=%.3f "
			    "trough=%.1f..%.1fms fill=%.1fms gaps+%llu underruns+%llu)\n",
			    knobOk ? "ok" : "FAIL", knobGapRms, knobDelta, knobRms, knobMin, knobMax,
			    knobStats.fillMs, (unsigned long long)knobGaps,
			    (unsigned long long)(knobStats.underruns - preKnob.underruns));
		if (!knobOk)
			break;
		const uint64_t pKnobOff = sink->pos();
		audio.setSourceSyncOffset(src, 0);
		sleepMs(900);
		const double knobOffDelta = gate_g_max_delta(
			sink->window(pKnobOff > 4800 ? pKnobOff - 4800 : 0, pKnobOff + 28800));
		const double knobOffRms = lastRms(0.4);
		const auto knobOffStats = audio.stats();
		const bool knobOffOk = knobOffDelta <= thr && knobOffRms > 0.2 &&
				       knobOffStats.fillMs < 120.0 &&
				       knobOffStats.transportGaps == knobStats.transportGaps &&
				       knobOffStats.underruns == preKnob.underruns;
		std::printf("  gate-g: sync-offset 0 %s (maxDelta=%.4f rms=%.3f fill=%.1fms)\n",
			    knobOffOk ? "ok" : "FAIL", knobOffDelta, knobOffRms,
			    knobOffStats.fillMs);
		if (!knobOffOk)
			break;

		// 7. Meters against the analytic tone values (amp 0.5: RMS 0.354, peak 0.5).
		audio.stats(); // fresh window
		sleepMs(500);
		const auto meterStats = audio.stats();
		const bool metersOk = !meterStats.sources.empty() &&
				      meterStats.sources[0].rms > 0.25 && meterStats.sources[0].rms < 0.46 &&
				      meterStats.sources[0].peak > 0.35 && meterStats.sources[0].peak < 0.65 &&
				      meterStats.masterRms > 0.25 && meterStats.masterRms < 0.46 &&
				      !meterStats.clipped;
		std::printf("  gate-g: meters %s (src rms=%.3f peak=%.3f master rms=%.3f clipped=%d)\n",
			    metersOk ? "ok" : "FAIL",
			    meterStats.sources.empty() ? -1.0f : meterStats.sources[0].rms,
			    meterStats.sources.empty() ? -1.0f : meterStats.sources[0].peak,
			    meterStats.masterRms, meterStats.clipped ? 1 : 0);
		if (!metersOk)
			break;

		// 8. Detach: fade-wrapped (continuity) and the mix returns to silence.
		const uint64_t pDetach = sink->pos();
		audio.detachSource(src);
		sleepMs(600);
		const double detachDelta =
			gate_g_max_delta(sink->window(pDetach > 4800 ? pDetach - 4800 : 0, pDetach + 19200));
		const double detachRms = lastRms(0.25);
		const bool detachOk = detachDelta <= thr && detachRms < 0.005;
		std::printf("  gate-g: detach silence %s (maxDelta=%.4f rms=%.4f)\n",
			    detachOk ? "ok" : "FAIL", detachDelta, detachRms);
		if (!detachOk)
			break;

		pass = true;
	} while (false);

	audio.detachSource(src); // idempotent (already detached on the pass path)
	if (active)
		obs_source_dec_active(src);
	if (src2) {
		audio.detachSource(src2); // idempotent (already detached on the pass path)
		if (active2)
			obs_source_dec_active(src2);
	}
	audio.stop();
	audio.stop(); // idempotence assertion, the engine-gate convention

	obs_source_dec_showing(src);
	obs_source_release(src);
	std::remove(wavPath.c_str());
	if (src2) {
		obs_source_dec_showing(src2);
		obs_source_release(src2);
	}
	if (!wav2Path.empty())
		std::remove(wav2Path.c_str());

	std::printf("GATE G (audio engine): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE H (servo/trim scenarios, pure logic): the shed/latch/patience decision core asserted
// against thousands of deterministic synthetic producer traces (ServoTrimSim) -- the same
// constants and the same window decisions the render thread runs (the ServoTrimLogic seam),
// with no threads, devices, or wall clock. Every family asserts, including the floor-policy
// families (burst-envelope release, satisfiability boundaries, capped re-prime, mixed shed
// windows, post-shed landing margins).
// ---------------------------------------------------------------------------------------------
static bool run_gate_h()
{
	const moxrelay::ServoTrimSimReport rep = moxrelay::runServoTrimSim();
	std::printf("  gate-h: %d scenarios asserted, %d failed (%.0f ms)\n", rep.scenarios,
		    rep.failed, rep.elapsedMs);
	for (const std::string &line : rep.failures)
		std::printf("  gate-h: FAIL %s\n", line.c_str());
	const bool pass = rep.scenarios > 0 && rep.failed == 0;
	std::printf("GATE H (servo/trim scenarios): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE I (control-port auto-fallback): proves a busy requested port never kills the control
// endpoint AND that the ACTUAL bound port -- not the requested one -- is what propagates into
// identity / the discovery-file writer / the helper-config.json discovery file (the value a
// control client reads to find this instance). Mechanism, no event loop:
//   1. OCCUPY a requested port with a real loopback listen socket (so it is genuinely busy).
//   2. resolveControlPort(requested) must return a DIFFERENT, free port (auto-fallback works).
//   3. Build the InstanceIdentity + the HelperInstance EXACTLY as run_gui() does with the
//      resolved port, serialize through HelperConfig::writeTo (the real discovery-file path), parse
//      it back, and assert the persisted port AND spoutPrefix carry the RESOLVED port -- NOT the
//      occupied requested one. If propagation regressed (locator file advertising the requested
//      port while the server bound the fallback), this gate fails.
// ---------------------------------------------------------------------------------------------
static bool run_gate_i()
{
	using moxrelay::HelperConfig;
	using moxrelay::HelperInstance;
	using moxrelay::SpoutNaming;

	ix::initNetSystem();
	bool pass = false;
	SOCKET occupied = INVALID_SOCKET;
	do {
		// Pick a requested port that is currently free, then OCCUPY it for real so the resolver
		// must fall back. (Probing high in the dynamic range avoids clashing with a live helper.)
		int requested = 0;
		for (int candidate = 48100; candidate <= 48150; ++candidate) {
			if (probeLoopbackPortFree(candidate)) {
				requested = candidate;
				break;
			}
		}
		if (!requested) {
			std::printf("  gate-i: no free probe port in 48100-48150\n");
			break;
		}

		occupied = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (occupied == INVALID_SOCKET) {
			std::printf("  gate-i: could not create occupier socket\n");
			break;
		}
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<unsigned short>(requested));
		ix::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (::bind(occupied, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
		    ::listen(occupied, 1) != 0) {
			std::printf("  gate-i: could not occupy port %d\n", requested);
			break;
		}

		// (a) The fallback must pick a DIFFERENT, real port -- a busy port never kills the endpoint.
		const int bound = resolveControlPort(requested);
		const bool fellBack = bound != 0 && bound != requested;
		std::printf("  gate-i: requested %d busy -> bound %d (%s)\n", requested, bound,
			    fellBack ? "ok" : "FAIL");
		if (!fellBack)
			break;

		// (b) The bound port -- NOT the requested one -- must be what feeds identity + the locator
		// file. Replicate the run_gui() field build with `bound`, serialize, parse back, assert.
		const std::string machine = SpoutNaming::localMachineName();
		HelperInstance inst;
		inst.instanceId = "tier-60";
		inst.port = bound; // run_gui(): inst.port = port (= bound)
		inst.version = moxrelay::kMoxRelayVersion;
		inst.fpsTier = 60;
		inst.spoutPrefix = SpoutNaming::makeSpoutPrefix(machine, bound); // run_gui() spoutPrefix
		const std::string expectedPrefix = inst.spoutPrefix;

		const std::string path =
			QDir::tempPath().toStdString() + "/moxrelay-gate-i-helper-config.json";
		if (!HelperConfig::writeTo(path, inst)) {
			std::printf("  gate-i: discovery-file write failed\n");
			break;
		}

		bool persistedOk = false;
		if (obs_data_t *doc = obs_data_create_from_json_file(path.c_str())) {
			// The discovery file MUST carry the bound port, and MUST NOT carry the
			// occupied requested port -- that is the connects-to-dead-port bug.
			persistedOk = obs_data_get_int(doc, "port") == bound &&
				      obs_data_get_int(doc, "port") != requested &&
				      std::strcmp(obs_data_get_string(doc, "spoutPrefix"),
						  expectedPrefix.c_str()) == 0;
			obs_data_release(doc);
		}
		std::remove(path.c_str());
		std::printf("  gate-i: discovery file carries bound port + matching spoutPrefix (%s)\n",
			    persistedOk ? "ok" : "FAIL");
		if (!persistedOk)
			break;

		pass = true;
	} while (false);

	if (occupied != INVALID_SOCKET)
		::closesocket(occupied);
	ix::uninitNetSystem();
	std::printf("GATE I (control-port auto-fallback): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// GATE J (item 05): the profile SAVE/LOAD round-trip, in-process end to end. Builds sources +
// filters + per-source audio (incl. a non-zero sync offset) through the REAL ControlVerbs gui*
// seams, SaveProfile()s a snapshot to the real store, LoadProfile()s it back (the destructive swap +
// rebuild + filter replay), re-snapshots, and asserts the rebuilt sources[] equals the original --
// including filter chain ORDER, filter settings (minus stripped libobs defaults), and the audio
// quad. The syncOffset round-trip is asserted EXPLICITLY (ms-in == ms-out) to catch the ns/ms unit
// trap. Two uniquely-named test profiles are written + cleaned up; this is the round-trip
// DONE-CRITERION. (No re-tier is exercised here: the load runs at the running tier, so the
// obs_reset_video branch is skipped -- the live re-tier is smoke-tested in the GUI, per the spec.)
// ---------------------------------------------------------------------------------------------
static bool run_gate_j()
{
	using moxrelay::ControlVerbs;
	using moxrelay::InstanceIdentity;
	using moxrelay::ProfileStore;
	using moxrelay::SpoutNaming;
	using moxrelay::SpoutSenderEngine;
	using nlohmann::json;

	const std::string nameA = "__moxrelay_selftest_roundtrip_a__";
	const std::string nameB = "__moxrelay_selftest_roundtrip_b__";

	bool pass = false;
	SpoutSenderEngine engine;
	moxrelay::AudioMixEngine audio;
	do {
		if (!engine.start())
			break;
		audio.setTestSink([](const float *, size_t) {});
		if (!audio.start())
			break;

		InstanceIdentity identity;
		identity.instanceId = "selftest";
		identity.fpsTier = 60;
		identity.version = moxrelay::kMoxRelayVersion;
		identity.machine = SpoutNaming::localMachineName();
		// ownerId stays EMPTY -- the profile verbs reject when owned, so the gate must run unowned.

		ControlVerbs verbs(&engine, &audio, identity);
		verbs.adoptBootSources({}, /*attachedInOrder=*/false);

		// Build two color sources with a filter + audio each. color_source_v3 is always registered
		// (asserted by GATE C); "color" is its wire name.
		auto requireResult = [](const json &reply) -> bool {
			return reply.is_object() && reply.contains("result");
		};
		const json s1 = verbs.guiCreateSource("color", "RoundTripA");
		const json s2 = verbs.guiCreateSource("color", "RoundTripB");
		if (!requireResult(s1) || !requireResult(s2)) {
			std::printf("  gate-j: source create failed\n");
			break;
		}
		const std::string id1 = s1["result"].value("sourceId", std::string());
		const std::string id2 = s2["result"].value("sourceId", std::string());

		// A color filter chain on source 1 (two filters -> order matters); one on source 2. The
		// crop/pad filter ("crop_filter") is a core video filter (always registered).
		const json f1a = verbs.guiAddFilter(id1, "crop", "CropOne");
		const json f1b = verbs.guiAddFilter(id1, "crop", "CropTwo");
		const json f2a = verbs.guiAddFilter(id2, "crop", "CropX");
		(void)f2a;
		if (!requireResult(f1a) || !requireResult(f1b)) {
			std::printf("  gate-j: AddFilter failed (filter type unavailable?)\n");
			break;
		}
		// Non-default crop settings on the first filter so the round-trip carries explicit values
		// (defaults are stripped on save; a stripped-default that survived would be a bug).
		const std::string fid1a = f1a["result"].value("filterId", std::string());
		verbs.guiSetFilterProperties(id1, fid1a, json{{"left", 12}, {"top", 8}});

		// Per-source audio incl. a non-zero sync offset (the ns/ms trap) and a gain != default.
		verbs.guiSetSourceAudio(id1, json{{"gain", 0.5}, {"muted", true}, {"syncOffsetMs", 42}});
		verbs.guiSetSourceAudio(id2, json{{"balance", 0.25}});

		// Snapshot #1: SaveProfile writes the real store. The original snapshot to compare against is
		// re-read from disk (the actual persisted bytes).
		const json saveA = verbs.guiSaveProfile(nameA);
		if (!requireResult(saveA)) {
			std::printf("  gate-j: SaveProfile(A) failed\n");
			break;
		}
		std::string savedA;
		if (!ProfileStore::read(nameA, savedA)) {
			std::printf("  gate-j: could not read back profile A\n");
			break;
		}

		// Load it back (destructive swap + rebuild + filter replay), then snapshot #2.
		const json loadReply = verbs.guiLoadProfile(nameA);
		if (!requireResult(loadReply)) {
			std::printf("  gate-j: LoadProfile failed: %s\n",
				    loadReply.contains("error")
					    ? loadReply["error"].value("message", std::string()).c_str()
					    : "(no error field)");
			break;
		}
		const json saveB = verbs.guiSaveProfile(nameB);
		if (!requireResult(saveB)) {
			std::printf("  gate-j: SaveProfile(B) failed\n");
			break;
		}
		std::string savedB;
		if (!ProfileStore::read(nameB, savedB)) {
			std::printf("  gate-j: could not read back profile B\n");
			break;
		}

		json docA = json::parse(savedA, nullptr, /*allow_exceptions=*/false);
		json docB = json::parse(savedB, nullptr, /*allow_exceptions=*/false);
		if (!docA.is_object() || !docB.is_object()) {
			std::printf("  gate-j: profile JSON parse failed\n");
			break;
		}

		// The sources[] array (settings, filter chain order + settings, audio quad) must be identical
		// across the save -> load -> save round-trip. profileName differs (A vs B) so compare only the
		// sources arrays.
		const json srcA = docA.value("sources", json::array());
		const json srcB = docB.value("sources", json::array());
		if (srcA != srcB) {
			std::printf("  gate-j: sources[] diverged across the round-trip\n");
			break;
		}

		// Explicit assertions on the load-bearing details (a == comparison would already catch these,
		// but spell them out so a regression names the cause).
		bool detailOk = srcA.is_array() && srcA.size() == 2;
		if (detailOk) {
			// Filter chain order preserved (CropOne before CropTwo on source 1).
			const json &filters0 = srcA[0].value("filters", json::array());
			detailOk = filters0.is_array() && filters0.size() == 2 &&
				   filters0[0].value("name", std::string()) == "CropOne" &&
				   filters0[1].value("name", std::string()) == "CropTwo";
			if (!detailOk)
				std::printf("  gate-j: filter chain order not preserved\n");
		}
		if (detailOk) {
			// The syncOffset round-trips as MILLISECONDS (42 in == 42 out, NOT 42e6 ns clamped to 950).
			const int syncMs = srcA[0].value("syncOffsetMs", -1);
			detailOk = (syncMs == 42);
			if (!detailOk)
				std::printf("  gate-j: syncOffsetMs round-trip FAILED (got %d, expected 42 ms)\n",
					    syncMs);
		}
		if (detailOk) {
			// muted survived; gain survived (~0.5).
			const bool muted = srcA[0].value("muted", false);
			const double gain = srcA[0].value("gain", -1.0);
			detailOk = muted && gain > 0.49 && gain < 0.51;
			if (!detailOk)
				std::printf("  gate-j: audio quad (gain/muted) round-trip FAILED\n");
		}
		if (!detailOk)
			break;

		pass = true;
	} while (false);

	// Cleanup: drop the two test profiles + the last-profile pointer if it points at one of them.
	ProfileStore::remove(nameA);
	ProfileStore::remove(nameB);
	if (ProfileStore::lastProfile() == nameA || ProfileStore::lastProfile() == nameB)
		ProfileStore::setLastProfile(std::string());

	engine.stop();
	audio.stop();
	std::printf("GATE J (profile save/load round-trip): %s\n", pass ? "PASS" : "FAIL");
	std::fflush(stdout);
	return pass;
}

// ---------------------------------------------------------------------------------------------
// Headless self-test path (print/exit contract UNCHANGED: gates print "GATE X (desc): PASS|FAIL",
// AND into ok; exit 0=pass / 1=gate fail / 2=boot fail; no window, no event loop). Gates C/D/E
// are the ADDITIVE M2.1/M2.2/M2.3 gates; the design-hook sample prints remain additive as before.
// ---------------------------------------------------------------------------------------------
// Sequential child-process rep loop: libobs is not restartable in-process, so each pass runs as a
// fresh --selftest child (same gate subset, same hold/rundir), output forwarded straight through.
// Stops at the first failing pass and returns its exit code (negative QProcess codes map to 2).
static int run_selftest_reps(int argc, char **argv, const moxrelay::CliOptions &options)
{
	QCoreApplication app(argc, argv);
	QStringList childArgs;
	childArgs << QStringLiteral("--selftest");
	if (!options.selftestGates.isEmpty())
		childArgs << QStringLiteral("--gates") << options.selftestGates.join(QLatin1Char(','));
	if (options.holdSeconds > 0)
		childArgs << QStringLiteral("--hold") << QString::number(options.holdSeconds);
	if (!options.rundir.isEmpty())
		childArgs << QStringLiteral("--rundir") << options.rundir;

	const QString self = QCoreApplication::applicationFilePath();
	for (int i = 1; i <= options.selftestReps; ++i) {
		std::printf("SELFTEST REP %d/%d: start\n", i, options.selftestReps);
		std::fflush(stdout);
		QElapsedTimer timer;
		timer.start();
		const int code = QProcess::execute(self, childArgs);
		const double sec = double(timer.elapsed()) / 1000.0;
		std::printf("SELFTEST REP %d/%d: %s (exit %d, %.1f s)\n", i, options.selftestReps,
			    code == 0 ? "PASS" : "FAIL", code, sec);
		std::fflush(stdout);
		if (code != 0) {
			std::printf("SELFTEST REPS: stopped at rep %d/%d (exit %d)\n", i,
				    options.selftestReps, code);
			std::fflush(stdout);
			return code < 0 ? 2 : code;
		}
	}
	std::printf("SELFTEST REPS: %d/%d passed\n", options.selftestReps, options.selftestReps);
	std::fflush(stdout);
	return 0;
}

static int run_selftest(int argc, char **argv, const moxrelay::CliOptions &options)
{
	// WINDOWS-subsystem build: reattach the parent console (dev shell) so gate output stays visible;
	// fall back to the file log sink when detached / on CI. FIRST, before any libobs boot.
	moxrelay::installConsoleOrFileLogSink();

	if (options.selftestReps > 1)
		return run_selftest_reps(argc, argv, options);

	const int holdSeconds = options.holdSeconds;
	// Gate selection: empty = the full suite, byte-identical to an unqualified --selftest run.
	// Gates a/b always run -- they assert the engine boot every other gate depends on. Skipped
	// gates print SKIP and are excluded from the verdict; the additive informational prints
	// (design-hook samples, settings-key audit) run only on full-suite passes.
	const bool selAll = options.selftestGates.isEmpty();
	const auto sel = [&](const char *g) {
		return selAll || options.selftestGates.contains(QLatin1String(g));
	};

	QApplication app(argc, argv);
	std::printf("Qt version: %s\n", qVersion());
	std::fflush(stdout);

	// R8 pin, evaluated BEFORE libobs starts: obs_data is bmem-only, so serialization must work
	// with obs.dll merely loaded. Asserted by GATE E after the boot. The cleared/no-helper file
	// is a bare empty object, so a working pre-startup serialize yields "{}".
	const std::string preStartupJson = moxrelay::HelperConfig::serializeEmpty(/*pretty=*/false);
	const bool preStartupSerializeOk = preStartupJson.find('{') != std::string::npos;

	// Hermetic gate: keep the per-module config/cache out of the real %LOCALAPPDATA%/MoxRelay.
	moxrelay::BootstrapOptions boot;
	boot.adapter = options.adapter; // --adapter GPU index (make_ovi clamps negative to 0)
	boot.moduleConfigDirOverride = QDir::tempPath().toStdString() + "/moxrelay-modulecfg";
	moxrelay::BootstrapResult r = moxrelay::ObsBootstrap::startup(boot);

	if (!r.started) {
		std::printf("GATE A (default.effect): FAIL\n");
		std::printf("GATE B (mfi.count==0): FAIL\n");
		std::printf("GATE C (spout sender): FAIL\n");
		std::printf("GATE D (n senders + collision): FAIL\n");
		std::printf("GATE D2 (sender formats + alpha): FAIL\n");
		std::printf("GATE E (helper-config write): FAIL\n");
		std::printf("GATE F (control endpoint): FAIL\n");
		std::printf("GATE G (audio engine): FAIL\n");
		std::printf("GATE H (servo/trim scenarios): FAIL\n");
		std::printf("GATE I (control-port auto-fallback): FAIL\n");
		std::printf("GATE J (profile save/load round-trip): FAIL\n");
		std::printf("SELFTEST: FAIL (libobs failed to start)\n");
		std::fflush(stdout);
		moxrelay::ObsBootstrap::shutdown();
		return 2;
	}

	std::printf("GATE A (default.effect): %s\n", r.gateA ? "PASS" : "FAIL");
	std::printf("GATE B (mfi.count==0): %s\n", r.gateB ? "PASS" : "FAIL");
	std::printf("loaded modules: %d\n", r.modulesLoaded);
	if (!r.gateB)
		std::printf("module failures: %zu\n", r.moduleFailures);
	std::fflush(stdout);

	// ADDITIVE: exercise the M2 design hooks headlessly (must run while libobs is still up, since
	// HelperConfig::serialize uses obs_data). Does NOT change the gate outcome or exit code.
	if (selAll)
		print_design_hook_samples();

	// GATE C (M2.1): the sender engine end-to-end (needs libobs up; runs after the hook prints so
	// the M1 output ordering is preserved).
	bool gateC = true;
	if (sel("c")) {
		gateC = run_gate_c();
	} else {
		std::printf("GATE C (spout sender): SKIP\n");
		std::fflush(stdout);
	}

	// GATE D (M2.2): N senders through the real config path + the collision counter. Owns the
	// cross-process hold window (its four senders include the deterministic ColorA).
	bool gateD = true;
	if (sel("d")) {
		gateD = run_gate_d(holdSeconds);
	} else {
		std::printf("GATE D (n senders + collision): SKIP\n");
		std::fflush(stdout);
	}

	// GATE D2 (M3): per-slot sender formats + the alpha contract (in-process receiver).
	bool gateD2 = true;
	if (sel("d2")) {
		gateD2 = run_gate_d2();
	} else {
		std::printf("GATE D2 (sender formats + alpha): SKIP\n");
		std::fflush(stdout);
	}

	// GATE E (M2.3/R8): the helper-config atomic write + the pre-startup obs_data pin.
	bool gateE = true;
	if (sel("e")) {
		gateE = run_gate_e(preStartupSerializeOk);
	} else {
		std::printf("GATE E (helper-config write): SKIP\n");
		std::fflush(stdout);
	}

	// GATE F (M6): the control endpoint end-to-end (real server + real client, in-process).
	bool gateF = true;
	if (sel("f")) {
		gateF = run_gate_f();
	} else {
		std::printf("GATE F (control endpoint): SKIP\n");
		std::fflush(stdout);
	}

	// GATE G: the audio engine end-to-end through the in-process test sink (no endpoint).
	bool gateG = true;
	if (sel("g")) {
		gateG = run_gate_g();
	} else {
		std::printf("GATE G (audio engine): SKIP\n");
		std::fflush(stdout);
	}

	// GATE H: the servo/trim decision core against deterministic synthetic producer traces.
	bool gateH = true;
	if (sel("h")) {
		gateH = run_gate_h();
	} else {
		std::printf("GATE H (servo/trim scenarios): SKIP\n");
		std::fflush(stdout);
	}

	// GATE I: control-port auto-fallback -- a busy requested port falls back to a free one, and
	// the BOUND port (not the requested one) is what the helper-config.json locator file carries.
	bool gateI = true;
	if (sel("i")) {
		gateI = run_gate_i();
	} else {
		std::printf("GATE I (control-port auto-fallback): SKIP\n");
		std::fflush(stdout);
	}

	// GATE J (item 05): the profile save/load round-trip (sources + filter chain order + filter
	// settings minus stripped defaults + the audio quad incl. the ms syncOffset).
	bool gateJ = true;
	if (sel("j")) {
		gateJ = run_gate_j();
	} else {
		std::printf("GATE J (profile save/load round-trip): SKIP\n");
		std::fflush(stdout);
	}

	// ADDITIVE: the published-settings-key audit (contract vocabulary hygiene; printed, not
	// gated -- a flag means a key gets renamed through the TypeVocabulary overlay).
	if (selAll)
		run_settings_key_audit();

	moxrelay::ObsBootstrap::shutdown();
	// R3 idempotence assertion: a second shutdown must be a safe no-op (obs_shutdown itself has no
	// guard for the double/never-started cases -- the R3 gate keeps this from crashing).
	moxrelay::ObsBootstrap::shutdown();

	const bool ok = r.gateA && r.gateB && gateC && gateD && gateD2 && gateE && gateF && gateG &&
			gateH && gateI && gateJ;
	std::printf("SELFTEST: %s\n", ok ? "PASS" : "FAIL");
	std::fflush(stdout);
	return ok ? 0 : 1;
}

// (R4: registered_input_ids / primary-monitor helpers moved verbatim to src/obs/SourceFactory --
// see SourceFactory::registeredInputIds.)

// ---------------------------------------------------------------------------------------------
// GUI path (this process = one single instance). HARD ORDERING: libobs bootstrap (incl.
// obs_reset_video) MUST complete before any window is shown / display created. The instance starts
// with no sources -- they come from Add Source / the wire. This process is the single writer of the
// helper-config.json discovery file.
// ---------------------------------------------------------------------------------------------
static int run_gui(int argc, char **argv, const moxrelay::CliOptions &options)
{
	// Item 04: identity for QSettings. Org = App = "MoxRelay" (100% unbranded); IniFormat default so
	// even a default-constructed QSettings lands in %APPDATA%/MoxRelay/MoxRelay.ini (never the
	// registry). These statics are callable BEFORE the QApplication ctor and MUST run before
	// installFileLogSink() below, because the file-log toggle reads the log/toFile QSetting inside it.
	// No setOrganizationDomain (it would synthesize a reverse-DNS path key on some platforms).
	QCoreApplication::setOrganizationName(QStringLiteral("MoxRelay"));
	QCoreApplication::setApplicationName(QStringLiteral("MoxRelay"));
	QSettings::setDefaultFormat(QSettings::IniFormat);

	// Item 04: mode gate is owner-id PRESENCE (empty => standalone). Resolved once and threaded into
	// the standalone settings resolution + the TrayController below. NEVER compared against any
	// literal token value.
	const bool managed = !options.ownerId.isEmpty();

	// Item 04: set the libobs log-level threshold from the resolved standalone log/level. Logging may
	// apply in both modes (harmless); the MOXRELAY_LOG_FILE env var stays the universal file override.
	moxrelay::setLogLevelThreshold(
		moxrelay::logLevelThresholdFor(moxrelay::AppSettings().logLevel().toStdString()));

	// WINDOWS-subsystem build: no console. Redirect stdout/stderr to a per-pid file log FIRST --
	// before ObsBootstrap::startup registers the libobs log handler -- so the earliest libobs boot
	// lines are captured. GUI (standalone + managed child) gets the file sink, never AttachConsole.
	moxrelay::installFileLogSink();

	QApplication app(argc, argv);
	apply_dark_theme(app); // embedded QSS dark theme -- before any window is shown
	app.setWindowIcon(moxRelayAppIcon()); // default icon for every top-level window (titlebar/taskbar/Alt-Tab)

	moxrelay::BootstrapOptions boot;
	// Adapter precedence: explicit --adapter wins; else (standalone) the saved setting; else 0.
	// Managed: the host owns the GPU choice via --adapter, never the saved standalone setting.
	int resolvedAdapter = (options.adapter >= 0)
				      ? options.adapter
				      : (!managed ? moxrelay::AppSettings().adapterIndex() : 0);
	// Clamp a stale/out-of-range index (e.g. a saved GPU that was since removed, or a too-high
	// --adapter) to 0 so boot can never abort on a nonexistent adapter; log when we clamp.
	const int adapterCount = countGpuAdapters();
	if (resolvedAdapter < 0 || (adapterCount > 0 && resolvedAdapter >= adapterCount)) {
		std::fprintf(stdout,
			     "[MoxRelay] requested rendering GPU index %d is unavailable (%d adapter(s) "
			     "detected) -- falling back to the primary GPU (index 0).\n",
			     resolvedAdapter, adapterCount);
		std::fflush(stdout);
		resolvedAdapter = 0;
	}
	boot.adapter = resolvedAdapter; // resolved + range-clamped GPU index (make_ovi also clamps <0)
	boot.rundir = options.rundir.toStdString();
	int port = (options.port > 0) ? options.port : 7341; // --port base (0 -> default 7341)
	int fpsTier = 60;
	std::string instanceId = "tier-60";
	if (options.fpsTier > 0) {
		// Explicit --fps-tier (the path the managing application uses when launching us): run at
		// that frame rate instead of 60. This wins exactly as today -- managed semantics unchanged.
		boot.fpsNum = options.fpsTier; // fpsDen stays 1
		fpsTier = options.fpsTier;
		instanceId = "tier-" + std::to_string(options.fpsTier);
	} else if (!managed) {
		// Item 05 FPS precedence (standalone): --fps-tier > the auto-loaded profile's fpsTier > the
		// global default-new-session engine/fpsTier > 60. A profile's tier is authoritative in
		// standalone, so peek the last-loaded profile's fpsTier here and BOOT at it directly -- the
		// post-window auto-load then finds the boot tier already matching and skips a redundant live
		// re-tier. (A later profile switch re-tiers LIVE via ControlVerbs; that does NOT touch the
		// global engine/fpsTier QSetting, which stays the no-profile fallback below.)
		int resolvedTier = 0;
		const std::string lastProfile = moxrelay::ProfileStore::lastProfile();
		if (!lastProfile.empty()) {
			std::string text;
			if (moxrelay::ProfileStore::read(lastProfile, text)) {
				nlohmann::json doc =
					nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
				if (doc.is_object() && doc.contains("fpsTier") &&
				    doc["fpsTier"].is_number_integer())
					resolvedTier = doc["fpsTier"].get<int>();
			}
		}
		if (resolvedTier <= 0)
			resolvedTier = moxrelay::AppSettings().fpsTier(); // global default-new-session fallback
		if (resolvedTier > 0 && resolvedTier != fpsTier) {
			boot.fpsNum = resolvedTier; // fpsDen stays 1
			fpsTier = resolvedTier;
			instanceId = "tier-" + std::to_string(resolvedTier);
		}
	}

	moxrelay::BootstrapResult r = moxrelay::ObsBootstrap::startup(boot);
	if (!r.started || !r.gateA) {
		std::fprintf(stdout, "[MoxRelay] libobs bootstrap failed (started=%d gateA=%d) -- aborting GUI.\n",
			     r.started, r.gateA);
		std::fflush(stdout);
		moxrelay::ObsBootstrap::shutdown();
		return 2;
	}
	std::printf("[MoxRelay] libobs up: Gate A=%s Gate B=%s, %d modules loaded (tier %d, port %d).\n",
		    r.gateA ? "PASS" : "FAIL", r.gateB ? "PASS" : "FAIL", r.modulesLoaded, fpsTier, port);
	std::fflush(stdout);

	// The instance starts EMPTY -- sources come from Add Source / the wire, and the preview
	// follows the Sources-list selection.
	std::vector<moxrelay::CreatedSource> sources;

	// The sender engine. Declared BEFORE the window scope (the window borrows it) and stopped
	// AFTER the window scope ends -- always before libobs shutdown (engine contract).
	moxrelay::SpoutSenderEngine engine;
	// Max Spout senders is read in BOTH standalone and managed mode (a machine-wide preference,
	// unlike the standalone-only GPU adapter). Must be set before start().
	engine.setMaxSenders(moxrelay::AppSettings().maxSenders());
	engine.start(); // zero slots until "Start Broadcast"; a slot-less callback is a no-op

	// The preview consume keeps the CURRENTLY PREVIEWED source's async texture fresh at the full
	// graphics-tick rate (independent of the present/vsync gate), so a webcam preview is smooth at
	// canvas fps > monitor refresh without broadcasting. Declared BESIDE the sender engine (same
	// lifecycle: stopped after the window scope, before libobs shutdown) and wired to the window's
	// preview source-change chokepoint below. A no-op until a source is set.
	moxrelay::PreviewConsumeEngine previewConsume;
	previewConsume.start();

	// The audio engine, stack-owned beside the sender engine (same lifecycle contract: stopped
	// after the window scope, before libobs shutdown). Tap-less until a broadcast attaches.
	moxrelay::AudioMixEngine audio;
	audio.start();

	const std::string machine = moxrelay::SpoutNaming::localMachineName();

	// Resolve the ACTUAL control port BEFORE anything keyed on it is built. The requested port is
	// tried first; if it is busy a fallback in the same range is chosen so the control endpoint
	// always comes up while ANY port is free. `boundPort` (0 only when the whole range is taken)
	// is from here on the SINGLE source of truth for identity / spoutPrefix / the discovery-file
	// writer / the status bar / the helper-config.json discovery file -- never the requested port.
	const int requestedPort = port;
	const int boundPort = resolveControlPort(requestedPort);
	if (boundPort != 0 && boundPort != requestedPort) {
		std::fprintf(stdout, "[MoxRelay] control port %d busy; bound %d instead.\n", requestedPort,
			     boundPort);
		std::fflush(stdout);
	}
	// Everything downstream keys on the bound port. (When boundPort == 0 the endpoint will not
	// come up at all; identity still uses the requested port so the degraded status reads sensibly.)
	port = (boundPort != 0) ? boundPort : requestedPort;

	// M6: the control-verb layer ADOPTS the instance's sources -- single ownership and ONE
	// broadcast state path for the GUI toolbar and the wire.
	moxrelay::InstanceIdentity identity;
	identity.instanceId = instanceId;
	identity.port = port;
	identity.fpsTier = fpsTier;
	identity.version = moxrelay::kMoxRelayVersion;
	identity.spoutPrefix = moxrelay::SpoutNaming::makeSpoutPrefix(machine, port);
	identity.machine = machine;
	identity.ownerId = options.ownerId.toStdString();
	moxrelay::ControlVerbs verbs(&engine, &audio, identity);
	verbs.adoptBootSources(std::move(sources), /*attachedInOrder=*/false);
	sources.clear();

	// Item 05: the live re-tier seam (standalone profiles). ControlVerbs::loadProfileSources owns the
	// strict teardown/rebuild order (engine stop -> source release -> THIS reset -> source rebuild ->
	// engine start); this handler is ONLY the obs_reset_video step, run while the graphics thread is
	// already drained. The canvas dims travel from the boot ovi so the re-run keeps the resolution.
	// The change is TRANSIENT -- the global engine/fpsTier QSetting (the no-profile fallback) is never
	// overwritten here. Gated implicitly by the verb layer (the profile verbs reject when managed).
	verbs.setRetierHandler([boot](int fpsNum) -> bool {
		moxrelay::ObsBootstrap::RetierOptions rt;
		rt.fpsNum = fpsNum;
		rt.fpsDen = 1;
		rt.baseWidth = boot.baseWidth;
		rt.baseHeight = boot.baseHeight;
		rt.outputWidth = boot.outputWidth;
		rt.outputHeight = boot.outputHeight;
		return moxrelay::ObsBootstrap::retier(rt).ok;
	});

	const std::string ownerId = options.ownerId.toStdString();
	// Per-launch random auth token (CSPRNG). Generated ONCE here and captured BY VALUE into the
	// discovery-file builder (which re-runs on every publish tick -- the value MUST be stable),
	// written to helper-config.json, and enforced on the control WS upgrade (server.setControlToken
	// below). Empty on RNG failure => control-API token auth disabled (degraded); the one-time
	// WARNING below is the single log site, and the Origin-reject gate stays active regardless.
	const std::string controlToken = moxrelay::generateControlToken();
	if (controlToken.empty()) {
		std::fprintf(stdout, "[MoxRelay] WARNING: control-API token authentication DISABLED -- "
				     "token generation failed (CSPRNG). The Origin-reject protection remains "
				     "active.\n");
		std::fflush(stdout);
	}

	// Discovery-file publishing (RELOCATED from the old Supervisor: this process is the SINGLE
	// writer of helper-config.json so a control client can always find the live port + token).
	// Build the ONE HelperInstance INLINE from the live engine state: actual sender names while
	// broadcasting. The path is --discovery-path when set, else the canonical
	// %APPDATA%/MoxRelay/helper-config.json. The build re-runs on every publish tick, so a source
	// going live (or away) re-publishes the file.
	const std::string discoveryPath = options.discoveryPath.isEmpty()
						  ? moxrelay::HelperConfig::canonicalConfigPath()
						  : options.discoveryPath.toStdString();
	const auto buildInstance = [&engine, &verbs, machine, port, instanceId, ownerId,
				    controlToken]() -> moxrelay::HelperInstance {
		moxrelay::HelperInstance inst;
		inst.instanceId = instanceId;
		inst.port = port;
		inst.version = moxrelay::kMoxRelayVersion;
		// Item 05: read the LIVE tier (not a boot-time snapshot) -- a standalone profile load can
		// re-tier in-process, and the next publish tick must reflect the current frame rate.
		inst.fpsTier = verbs.currentFpsTier();
		inst.spoutPrefix = moxrelay::SpoutNaming::makeSpoutPrefix(machine, port);
		inst.ownerId = ownerId;
		inst.controlToken = controlToken;
		for (const auto &info : engine.slotInfos()) {
			if (!info.actualName.empty() && info.lastSendOk)
				inst.sources.push_back(info.actualName);
		}
		return inst;
	};
	// Standalone (and old --discovery-path callers) publish the discovery FILE; in pipe mode the
	// instance is handed off over the rendezvous pipe instead (after the bind below), so the whole
	// publish-tick machinery is skipped. publishTimer is hoisted before the guard so the teardown
	// stop stays valid (it simply stays null in pipe mode).
	QTimer *publishTimer = nullptr;
	if (options.rendezvousPipe.isEmpty()) {
		// Publish-on-change: serialize, dedupe against the last published doc, write atomically.
		auto lastPublishedJson = std::make_shared<std::string>();
		const auto publishDiscovery = [discoveryPath, buildInstance, lastPublishedJson] {
			const moxrelay::HelperInstance inst = buildInstance();
			const std::string json = moxrelay::HelperConfig::serialize(inst, /*pretty=*/true);
			if (json == *lastPublishedJson)
				return;
			if (moxrelay::HelperConfig::writeTo(discoveryPath, inst))
				*lastPublishedJson = json;
			else
				std::fprintf(stdout,
					     "[MoxRelay] helper-config write failed (will retry on next change)\n");
		};
		// 1 s publish tick (the old Supervisor cadence) + an immediate initial write so discovery
		// works the moment the instance is up.
		publishTimer = new QTimer(&app);
		QObject::connect(publishTimer, &QTimer::timeout, &app,
				 [publishDiscovery] { publishDiscovery(); });
		publishTimer->start(1000);
		publishDiscovery();
	}

	// M6: the control endpoint (contract: docs/control-api.asyncapi.yaml). Start AFTER the engine
	// and the publish loop (a connectable endpoint always represents a live instance). Bind failure
	// degrades to Spout-only -- the GUI keeps running and the status bar says so.
	moxrelay::ControlServer server(&verbs);
	server.setControlToken(controlToken);
	// `port` is the port resolveControlPort() found free; bind it (the only remaining failure is
	// a TOCTOU loss of that port between probe and bind, which the fallback range made unlikely).
	// boundPort == 0 means no port in the range was ever free -- skip the bind, stay Spout-only.
	const bool controlUp = (boundPort != 0) && server.start(port);
	if (!controlUp) {
		std::fprintf(stdout, "[MoxRelay] control endpoint bind failed on port %d -- running Spout-only.\n",
			     port);
		std::fflush(stdout);
	}

	// Pipe-mode handoff: with the bound port + token + controlUp known, write the ONE instance
	// back over the rendezvous pipe (the launcher is the pipe SERVER, created before this launch).
	// This REPLACES the discovery-file publish (gated off above). When control did not come up
	// (boundPort == 0), buildInstance() still bakes the requested port into inst.port, so force
	// inst.port = 0 to signal "no control endpoint" to the reader (port <= 0 => Spout-only).
	if (!options.rendezvousPipe.isEmpty()) {
		moxrelay::HelperInstance inst = buildInstance();
		if (!controlUp)
			inst.port = 0;
		const std::string json = moxrelay::HelperConfig::serialize(inst, /*pretty=*/false);
		if (!moxrelay::RendezvousPipe::writeInstance(options.rendezvousPipe.toStdString(), json)) {
			std::fprintf(stdout,
				     "[MoxRelay] rendezvous pipe write failed (instance not handed off)\n");
		}
		std::fflush(stdout);
	}

	// Managed-mode owner-process death switch: when launched with --owner-pid, watch that process and
	// run the helper's normal graceful quit the INSTANT it exits/crashes/is killed (immediate, no
	// grace period). Standalone (no --owner-pid) is unaffected. Declared here at run_gui body scope --
	// AFTER `app` and OUTSIDE the window block below -- so it lives across app.exec() and the whole
	// teardown chain, and its watcher thread is stopped+joined at run_gui scope exit BEFORE
	// QApplication is destroyed.
	moxrelay::ParentWatch parentWatch;
	if (managed && options.ownerPid > 0)
		parentWatch.start(static_cast<unsigned long>(options.ownerPid));

	int rc = 0;
	{
		// Scope the window so it (and the MoxDisplayWidget) is fully destructed -- which removes the
		// draw callback + obs_display_destroy on the UI thread -- BEFORE libobs is shut down below.
		moxrelay::MainWindow window;

		// Preview follows the Sources-list selection. The resolver is render plumbing, not a
		// JSON mutation seam: it returns a borrowed pointer the display widget immediately
		// weak-refs (every dispatch -- and this lookup -- runs on the Qt main thread, so the
		// resolution cannot race a RemoveSource).
		window.setPreviewResolver(
			[&verbs](const std::string &sourceId) { return verbs.guiPreviewSource(sourceId); });

		// Route the previewed-source change through the single MoxDisplayWidget::setSource chokepoint
		// into the full-rate preview consume. Wired BEFORE setControlSeams populates the list (and any
		// initial selection fires onSelectionChanged), so the first previewed source propagates too;
		// every later switch, the teardown clear, and the dtor's setSource(nullptr) follow automatically.
		// previewConsume lives in the outer scope, so it outlives the window/widget that calls back.
		window.preview()->setSourceChangedHook(
			[&previewConsume](obs_source_t *source) { previewConsume.setSource(source); });

		// Status readout + the broadcast action through the verb layer (one state path with
		// the wire: the toolbar literally runs StartBroadcast/StopBroadcast).
		window.setEngine(&engine);
		window.setBroadcastHandler([&verbs](bool start) { return verbs.guiToggleBroadcast(start); });
		// M8: read-only seam so the toolbar button re-syncs when broadcast changes via the
		// control API (anyBroadcasting() iterates the source records; no toggle, no mutation).
		window.setBroadcastQueryHandler([&verbs]() { return verbs.anyBroadcasting(); });

		// Every GUI read/mutation rides the ControlVerbs gui* dispatch seams (the
		// registry-SoT rule -- GUI and WS actions share one verb path, one event stream).
		moxrelay::MainWindow::ControlSeams seams;
		seams.getVersion = [&verbs] { return verbs.guiGetVersion(); };
		seams.listSources = [&verbs] { return verbs.guiListSources(); };
		seams.createSource = [&verbs](const std::string &type, const std::string &displayName) {
			return verbs.guiCreateSource(type, displayName);
		};
		seams.removeSource = [&verbs](const std::string &sourceId) {
			return verbs.guiRemoveSource(sourceId);
		};
		seams.listSourceProperties = [&verbs](const std::string &sourceId) {
			return verbs.guiListSourceProperties(sourceId);
		};
		seams.setSourceProperties = [&verbs](const std::string &sourceId,
						     const nlohmann::json &settings) {
			return verbs.guiSetSourceProperties(sourceId, settings);
		};
		seams.invokeSourceButton = [&verbs](const std::string &sourceId,
						    const std::string &property) {
			return verbs.guiInvokeSourceButton(sourceId, property);
		};
		seams.setSourceFormat = [&verbs](const std::string &sourceId, const std::string &format) {
			return verbs.guiSetSourceFormat(sourceId, format);
		};
		seams.getMediaStatus = [&verbs](const std::string &sourceId) {
			return verbs.guiGetMediaStatus(sourceId);
		};
		seams.controlMedia = [&verbs](const std::string &sourceId, const std::string &action) {
			return verbs.guiControlMedia(sourceId, action);
		};
		seams.seekMedia = [&verbs](const std::string &sourceId, int64_t positionMs) {
			return verbs.guiSeekMedia(sourceId, positionMs);
		};
		seams.getStatus = [&verbs] { return verbs.guiGetStatus(); };
		seams.listAudioDevices = [&verbs](const std::string &flow) {
			return verbs.guiListAudioDevices(flow);
		};
		seams.setAudioOutputDevice = [&verbs](const std::string &deviceId) {
			return verbs.guiSetAudioOutputDevice(deviceId);
		};
		seams.getSourceAudio = [&verbs](const std::string &sourceId) {
			return verbs.guiGetSourceAudio(sourceId);
		};
		seams.setSourceAudio = [&verbs](const std::string &sourceId, const nlohmann::json &fields) {
			return verbs.guiSetSourceAudio(sourceId, fields);
		};
		seams.listAvailableFilters = [&verbs](const std::string &sourceId) {
			return verbs.guiListAvailableFilters(sourceId);
		};
		seams.listFilters = [&verbs](const std::string &sourceId) {
			return verbs.guiListFilters(sourceId);
		};
		seams.addFilter = [&verbs](const std::string &sourceId, const std::string &filterType,
					   const std::string &name) {
			return verbs.guiAddFilter(sourceId, filterType, name);
		};
		seams.removeFilter = [&verbs](const std::string &sourceId, const std::string &filterId) {
			return verbs.guiRemoveFilter(sourceId, filterId);
		};
		seams.setFilterEnabled = [&verbs](const std::string &sourceId, const std::string &filterId,
						  bool enabled) {
			return verbs.guiSetFilterEnabled(sourceId, filterId, enabled);
		};
		seams.reorderFilter = [&verbs](const std::string &sourceId, const std::string &filterId,
					       int index) {
			return verbs.guiReorderFilter(sourceId, filterId, index);
		};
		seams.renameFilter = [&verbs](const std::string &sourceId, const std::string &filterId,
					      const std::string &name) {
			return verbs.guiRenameFilter(sourceId, filterId, name);
		};
		seams.listFilterProperties = [&verbs](const std::string &sourceId,
						      const std::string &filterId) {
			return verbs.guiListFilterProperties(sourceId, filterId);
		};
		seams.setFilterProperties = [&verbs](const std::string &sourceId, const std::string &filterId,
						     const nlohmann::json &settings) {
			return verbs.guiSetFilterProperties(sourceId, filterId, settings);
		};
		seams.invokeFilterButton = [&verbs](const std::string &sourceId, const std::string &filterId,
						    const std::string &property) {
			return verbs.guiInvokeFilterButton(sourceId, filterId, property);
		};
		// Item 05: standalone profile seams (the toolbar Profiles control). Bound unconditionally --
		// the verb layer rejects them when managed, and the toolbar control is shown only when
		// standalone (setStandaloneProfilesEnabled(!managed) below), so they are dormant in helper mode.
		seams.listProfiles = [&verbs] { return verbs.guiListProfiles(); };
		seams.loadProfile = [&verbs](const std::string &name) { return verbs.guiLoadProfile(name); };
		seams.saveProfile = [&verbs](const std::string &name) { return verbs.guiSaveProfile(name); };
		seams.deleteProfile = [&verbs](const std::string &name) { return verbs.guiDeleteProfile(name); };
		seams.profileIsDirty = [&verbs](const std::string &name) { return verbs.profileIsDirty(name); };
		// Per-profile FPS (the standalone toolbar FPS dropdown): a live re-tier through the verb layer
		// (reject-when-managed) + a read of the current live tier so the dropdown reflects it after a
		// profile load. Bound unconditionally; the dropdown is part of the standalone group (hidden in
		// managed mode), and the verb rejects when owned, so this stays dormant in helper mode.
		seams.setFpsTier = [&verbs](int fpsTier) { return verbs.guiSetFpsTier(fpsTier); };
		seams.currentFpsTier = [&verbs] { return verbs.currentFpsTier(); };
		window.setControlSeams(std::move(seams));

		// Item 05: the window-layout seam so a profile can persist + restore the window geometry. The
		// window exposes geometry as plain JSON; ControlVerbs reads it on SaveProfile and applies it on
		// LoadProfile (the window never touches the profile store directly).
		verbs.setWindowLayoutSeams([&window] { return window.windowLayoutJson(); },
					   [&window](const nlohmann::json &layout) {
						   window.applyWindowLayout(layout);
					   });

		// Chain the event sink so the window tracks wire-driven changes too (the WS
		// publish stays first; the window handler is queued-internally + re-entrancy-safe).
		verbs.setEventSink([&server, &window](const std::string &name, const nlohmann::json &data) {
			server.publishEvent(name, data);
			window.onControlEvent(name, data);
		});

		// The GUI meters consume the audioLevels stream in-process (the same payload wire
		// subscribers get -- one tick, one drain, one emission path).
		server.setLevelsSink(
			[&window](const nlohmann::json &data) { window.onAudioLevels(data); });

		// M6: control endpoint state in the status bar.
		window.setControlStatus(controlUp ? QStringLiteral("control: ws://127.0.0.1:%1/control").arg(port)
						  : QStringLiteral("control: BIND FAILED (Spout-only)"));

		// Single instance: no fleet status to show.
		window.setFleetStatus(QString());

		// Item 04: start-minimized precedence (CLI --start-minimized > standalone
		// window/startMinimized QSetting > false). The CLI flag is what the managing application
		// passes for the managed start-hidden launch; standalone users opt in via the persisted
		// setting. The QSetting is consulted ONLY when standalone (empty owner-id).
		const bool startHidden =
			options.startMinimized || (!managed && moxrelay::AppSettings().startMinimized());

		// Runtime managed identity for the window title + tray tooltip: prefer the explicit
		// --client-name, fall back to the owner token, else empty (standalone -> plain "MoxRelay").
		const QString displayName = !options.clientName.isEmpty() ? options.clientName : options.ownerId;

		// Item 04: the TrayController owns the system-tray icon, its Show/Quit menu, and the
		// minimize/close-to-tray policy (promoted from the old inline block). Parented to the
		// window so it tears down before libobs shutdown. The tray icon is created whenever the OS
		// system tray is available, and the close/minimize-to-tray toggles are honored LIVE in BOTH
		// modes (the Shutdown verb calls quit() directly, so close-to-tray can never swallow the
		// managed stop path).
		moxrelay::TrayController trayController(&window, app.windowIcon(), managed, displayName, &window);

		// With a live tray icon, hide-to-tray closes the last VISIBLE window -- so leaving the default
		// quitOnLastWindowClosed=true on would terminate the process the moment the window hides to the
		// tray. Disable it; termination now comes ONLY from the explicit Quit actions (File>Quit /
		// tray Quit -> QCoreApplication::quit) and the closeEvent quit() on an accepted close. Without a
		// tray (no hide path), the default last-window-quit stays so the app still ends on window close.
		if (trayController.trayAvailable())
			app.setQuitOnLastWindowClosed(false);

		if (startHidden && trayController.trayAvailable()) {
			// Force the native window handle into existence while the window stays hidden, so the
			// hidden window is fully realized before it is ever shown/restored from the tray.
			// (Kept deliberately -- the tray "Show" action restores this same window.)
			window.createWinId();
			std::printf("tray mode: system tray icon shown (window hidden)\n");
			std::fflush(stdout);
		} else if (startHidden) {
			// No system tray available: fall back to a minimized window (matches old behavior).
			window.showMinimized();
			std::printf("tray mode: no system tray available -- starting as a minimized window\n");
			std::fflush(stdout);
		} else {
			window.show();
			// Bring the window to the user: a detached/script launch otherwise leaves it behind
			// whatever app holds the foreground (standard raise/activate; the OS may still only
			// flash the taskbar entry when foreground rules deny the switch).
			window.raise();
			window.activateWindow();
		}

		// Item 05: the standalone Profiles toolbar control is shown ONLY when standalone (empty
		// owner-id). This also runs auto-load-last through the normal GUI/verb load path (post window
		// construction, single load path). In managed (helper) mode the control stays hidden and no
		// profile is auto-loaded -- the owning integrator drives the source set and standalone
		// profiles stay inert.
		window.setStandaloneProfilesEnabled(!managed);

		// Managed (helper) mode source view-only lock: the Sources list, the preview, and the
		// media transport strip stay live, but every source-EDITING affordance (Add Source / Add
		// Filter, the Remove Source menu, the format combo, the property panel, the filter
		// inspector) is suppressed so the user cannot mutate the source set the owning integrator
		// reconciles. Standalone (empty owner-id) leaves it all editable.
		window.setSourceEditingEnabled(!managed);

		// Runtime managed identity in the window title (mirrors the tray tooltip). Empty displayName
		// (standalone) leaves the plain "MoxRelay" title set in the ctor.
		window.setManagedIdentity(displayName);

		rc = app.exec();

		// The window leaves scope next: restore the server-only sink FIRST so no teardown
		// emission (or a late levels tick) can touch the dead window.
		server.setLevelsSink(nullptr);
		verbs.setEventSink([&server](const std::string &name, const nlohmann::json &data) {
			server.publishEvent(name, data);
		});

		// Detach the source from the preview before its draw callback could fire again.
		window.preview()->setSource(nullptr);
	} // <- MoxDisplayWidget::~ tears the display down here, while libobs is still alive.

	// Teardown (contract order): goodbye push to connected clients, then stop the control
	// endpoint (synchronous; joins its connection threads), then stop the discovery publish tick
	// and CLEAR the discovery file (a bare {} object -- consumers see "no helper running"; stale
	// content therefore only survives a crash), then the engine, then the sources (owned by the
	// verb layer since adoption), then libobs.
	server.publishInstanceShuttingDown("shutdown");
	server.stop();
	if (publishTimer)
		publishTimer->stop();
	// Clear the discovery file on clean shutdown -- but ONLY in file mode. In pipe mode no file was
	// ever written (the publish tick was gated off), so there is nothing to clear.
	if (options.rendezvousPipe.isEmpty())
		moxrelay::HelperConfig::writeEmptyTo(discoveryPath);
	audio.stop(); // taps off while sources are alive, beside the sender-engine stop
	previewConsume.stop(); // remove the consume callback + tear its texrender down (libobs still up)
	engine.stop();
	verbs.releaseAllSources();
	moxrelay::ObsBootstrap::shutdown();
	return rc;
}

int main(int argc, char **argv)
{
	// R5: STRICT parse before any Q*Application exists (the mode decides WHICH app class to
	// construct -- R6). Unknown/invalid arguments fail loudly; they never fall through to the GUI.
	QStringList args;
	args.reserve(argc);
	for (int i = 0; i < argc; ++i)
		args << QString::fromLocal8Bit(argv[i]);

	const moxrelay::CliOptions options = moxrelay::CliOptions::parse(args);
	if (!options.ok) {
		std::fprintf(stderr, "moxrelay: %s\n\n%s", options.error.toUtf8().constData(),
			     moxrelay::CliOptions::helpText().toUtf8().constData());
		std::fflush(stderr);
		return 2;
	}
	if (options.helpRequested) {
		std::printf("%s", moxrelay::CliOptions::helpText().toUtf8().constData());
		return 0;
	}

	switch (options.mode) {
	case moxrelay::CliOptions::Mode::Selftest:
		return run_selftest(argc, argv, options);
	case moxrelay::CliOptions::Mode::Perf:
		return moxrelay::run_perf(argc, argv, options);
	case moxrelay::CliOptions::Mode::Gui:
	default:
		return run_gui(argc, argv, options);
	}
}
