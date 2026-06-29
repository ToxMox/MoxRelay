// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SpoutNaming implementation. See SpoutNaming.hpp for the rationale.

#include "SpoutNaming.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace moxrelay {

std::string SpoutNaming::makeSenderName(const std::string &machine, int port, const std::string &source)
{
	// Exactly "{machine}:Helper_{port}_{source}" -- the locked sender-name format.
	return machine + ":Helper_" + std::to_string(port) + "_" + source;
}

std::string SpoutNaming::makeSpoutPrefix(const std::string &machine, int port)
{
	// "{machine}:Helper_{port}" -- the per-instance prefix every one of an instance's senders shares.
	return machine + ":Helper_" + std::to_string(port);
}

std::string SpoutNaming::localMachineName()
{
#ifdef _WIN32
	char buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
	DWORD len = static_cast<DWORD>(sizeof(buf));
	if (GetComputerNameA(buf, &len) && len > 0)
		return std::string(buf, len);
	return "UNKNOWN";
#else
	return "UNKNOWN";
#endif
}

} // namespace moxrelay
