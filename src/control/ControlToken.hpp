// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ControlToken -- per-launch random auth token for the loopback control WebSocket.
//
// The helper self-generates a token at start and writes it into helper-config.json
// (instances[].controlToken). A client reads it from that file (the same read it already
// does for `port`) and appends `?token=<controlToken>` to the WS upgrade URI. The server
// enforces an exact, constant-time match before any verb dispatch. The token is fully
// automatic: the user never sees it, no prompt, no flag, no env var. It rotates on every
// helper launch, so a client must re-read it before each fresh connection.
//
// Generation uses a Windows CSPRNG (BCryptGenRandom, link bcrypt.lib), NOT rand(). On RNG
// failure an empty string is returned; the server treats an empty controlToken as
// "auth disabled" (degraded) -- fail-open ONLY on our own RNG failure, never on a missing
// client token. The startup call site (main.cpp) emits the single one-time WARNING when the
// generated token is empty.

#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h> // link bcrypt.lib
#endif

namespace moxrelay {

// 32 hex chars = 16 CSPRNG bytes. Returns "" only on RNG failure.
inline std::string generateControlToken()
{
#ifdef _WIN32
	unsigned char buf[16];
	if (BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return std::string();
	static const char *hex = "0123456789abcdef";
	std::string out;
	out.reserve(32);
	for (unsigned char b : buf) {
		out.push_back(hex[b >> 4]);
		out.push_back(hex[b & 0xF]);
	}
	return out;
#else
	return std::string();
#endif
}

} // namespace moxrelay
