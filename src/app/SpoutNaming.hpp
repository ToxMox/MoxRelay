// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SpoutNaming -- pure-logic M2 design hook for the multi-instance Spout sender-name scheme.
//
// Spout's shared-memory name registry is SESSION-WIDE (CreateFileMappingA
// with no "Local\\" prefix), so every sender name must be globally unique across the 2-3 fps-tier
// processes the supervisor will run (M2). The scheme is:
//
//     {MachineName}:Helper_{port}_{SourceName}
//
// The per-instance {port} segment guarantees uniqueness across the fleet (each fps-tier instance
// binds its own WS control port). This file is pure formatting + a machine-name helper -- it does
// NOT create senders (that is the per-source Spout filter, M2/M3). M1 ships it as a verifiable seam.

#pragma once

#include <string>

namespace moxrelay {

class SpoutNaming {
public:
	// Build the full per-source Spout sender name exactly per the locked scheme:
	//   "{machine}:Helper_{port}_{source}"
	// This is the canonical BASE format only. The collision counter lives in
	// spout/SenderNameAllocator (M2.2): it pre-resolves "_2", "_3", ... suffixes against the
	// session-wide registry plus this process's reservations BEFORE a sender is created, so a
	// published name is always ours (Spout's own backstop would silently suffix "_1" upward).
	static std::string makeSenderName(const std::string &machine, int port, const std::string &source);

	// The per-instance prefix shared by all of an instance's senders: "{machine}:Helper_{port}".
	// This is the value persisted as helper-config.json's per-instance `spoutPrefix` so a client
	// can match an instance's senders without re-deriving the scheme.
	static std::string makeSpoutPrefix(const std::string &machine, int port);

	// The local machine name (GetComputerNameA on Windows). Returns "UNKNOWN" if it cannot be read.
	static std::string localMachineName();
};

} // namespace moxrelay
