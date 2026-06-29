// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// RendezvousPipe implementation. <windows.h> is isolated to this translation unit (no Win32 surface
// leaks into the headers). The launcher creates the pipe SERVER before launching us, so CreateFileA
// against \\.\pipe\<name> normally connects immediately; ERROR_PIPE_BUSY (the single server instance
// is momentarily occupied) is retried with WaitNamedPipeA within a bounded budget, and a not-yet-
// created server (the brief startup race) is retried until the same budget elapses.

#include "RendezvousPipe.hpp"

#include <cstdio>

#include <windows.h>

namespace moxrelay {

namespace {
// Total time we are willing to wait for the server pipe to become connectable before giving up. The
// launcher creates the server BEFORE launching us, so this only ever covers a brief startup race.
constexpr DWORD kConnectTimeoutMs = 5000;
constexpr DWORD kBusyWaitMs = 250;  // per-iteration WaitNamedPipeA budget when the instance is busy
constexpr DWORD kRetrySleepMs = 50; // backoff before retrying a not-yet-created server pipe
} // namespace

bool RendezvousPipe::writeInstance(const std::string &pipeName, const std::string &json)
{
	if (pipeName.empty()) {
		std::fprintf(stdout, "[MoxRelay] rendezvous pipe name is empty -- nothing written\n");
		return false;
	}

	const std::string path = "\\\\.\\pipe\\" + pipeName;
	const DWORD deadline = ::GetTickCount() + kConnectTimeoutMs;

	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (;;) {
		pipe = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
			break;

		const DWORD err = ::GetLastError();
		if (err == ERROR_PIPE_BUSY) {
			// Every server instance is momentarily busy -- wait for one to free up, then retry.
			::WaitNamedPipeA(path.c_str(), kBusyWaitMs);
			if (::GetTickCount() >= deadline) {
				std::fprintf(stdout, "[MoxRelay] rendezvous pipe busy past the wait budget\n");
				return false;
			}
			continue;
		}

		// The server may not exist yet during the startup race (ERROR_FILE_NOT_FOUND); retry within
		// the budget, then give up.
		if (::GetTickCount() >= deadline) {
			std::fprintf(stdout, "[MoxRelay] rendezvous pipe open failed (err=%lu)\n",
				     static_cast<unsigned long>(err));
			return false;
		}
		::Sleep(kRetrySleepMs);
	}

	bool ok = true;
	const char *bytes = json.data();
	DWORD remaining = static_cast<DWORD>(json.size());
	while (remaining > 0) {
		DWORD wrote = 0;
		if (!::WriteFile(pipe, bytes, remaining, &wrote, nullptr) || wrote == 0) {
			std::fprintf(stdout, "[MoxRelay] rendezvous pipe write failed (err=%lu)\n",
				     static_cast<unsigned long>(::GetLastError()));
			ok = false;
			break;
		}
		bytes += wrote;
		remaining -= wrote;
	}

	::CloseHandle(pipe);
	return ok;
}

} // namespace moxrelay
