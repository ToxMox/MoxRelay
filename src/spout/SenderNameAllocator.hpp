// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SenderNameAllocator -- pre-create Spout sender-name resolution (M2.2).
//
// Spout's name registry is SESSION-WIDE and Spout itself silently auto-suffixes colliding names
// ("name_1", "name_2", ...) at registration, mutating the registered name away from what the
// caller asked for. Published names (helper-config.json, status UI) must never diverge from what
// we requested, so collisions are resolved BEFORE a sender is created:
//
//   1. base = SpoutNaming::makeSenderName(machine, port, source)
//   2. prune crashed senders still holding names (CleanSenders)
//   3. while the candidate is registered session-wide (FindSenderName) OR reserved by THIS
//      process, suffix "_2", "_3", ... (start at _2 -- Spout's own backstop starts at _1, so our
//      pre-resolved scheme can never collide with a Spout-generated suffix of the same base)
//   4. reserve the winner locally and return it
//
// The LOCAL reservation set is what the registry probe alone cannot give: a sender only appears
// in the session registry at its FIRST SendTexture, so two attaches in the same frame would both
// probe "free" and resolve to the same name. Reservations are released on detach/stop.
//
// THREADING: all methods are callable from any non-graphics thread; internally serialized by one
// mutex. Registry probes are pure shared-memory lookups -- no D3D11 involvement.

#pragma once

#include <mutex>
#include <set>
#include <string>

namespace moxrelay {

class SenderNameAllocator {
public:
	// Resolve a unique, immediately-reserved sender name. Returns "" if no free candidate was
	// found within the suffix budget (callers must treat that as attach failure, not retry-spin).
	std::string resolve(const std::string &machine, int port, const std::string &source);

	// Drop a local reservation (detach/stop). Unknown / already-released names are a safe no-op.
	void release(const std::string &resolvedName);

	// Current local reservation count (self-test introspection).
	size_t reservedCount();

private:
	std::mutex mutex_;
	std::set<std::string> reserved_;
};

} // namespace moxrelay
