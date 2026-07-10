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
// probe "free" and resolve to the same name.
//
// STICKY NAMES: reservations are NOT released on broadcast detach -- a source record keeps its
// allocated name for its whole lifetime and re-attaches under the SAME name (pass it back as
// `sticky`). Release happens only when the owning record is removed or at engine shutdown. Name
// stability across broadcast cycles is a correctness property for ANY name-keyed consumer (a
// receiver bound to a published name must never observe that name re-issued to a different
// source while the original still exists); first-come re-allocation let the same source flap
// between the base name and a suffixed one, handing a stale binding a sibling's feed.
// Consequence (intended): a dark source still reserves its name; a later same-named source gets
// the next suffix even while the first is dark.
//
// TRUNCATION: the registry stores names in fixed-size buffers (vendored SDK cap), so over-long
// requested names are truncated FIRST and uniquified AFTER -- two long names that truncate equal
// still get distinct suffixes, and the suffix itself can never push a candidate past the cap.
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
	// Resolve a unique, immediately-reserved sender name. `sticky` is a name this caller already
	// holds the reservation for (sticky re-attach): when non-empty and still reserved by this
	// process it is returned UNCHANGED and no new reservation is taken -- the name stayed ours
	// across the detach, so nobody else can have received it in between. Otherwise resolution
	// runs fresh (truncate to the cap budget, then suffix "_2", "_3", ... past collisions).
	// `wasSticky` (optional out) reports WHICH path ran -- the explicit rollback discriminator
	// for a failed attach: a STALE sticky that freshly re-resolves to the very same string is
	// still a FRESH reservation and must be rolled back (string equality cannot tell these
	// apart). Returns "" if no free candidate was found within the suffix budget (callers must
	// treat that as attach failure, not retry-spin).
	std::string resolve(const std::string &machine, int port, const std::string &source,
			    const std::string &sticky = std::string(), bool *wasSticky = nullptr);

	// Registration-time adoption sync: the registry auto-suffixed a lost cross-process race, so
	// the PUBLISHED name differs from the one reserved here. Move the reservation (release
	// `oldName`, reserve `adoptedName`) so the sticky name IS the published name -- a later
	// sticky re-attach then re-announces the name receivers actually latched onto. Pure set
	// operations (no registry probe, no D3D): safe from the graphics thread. No-op when
	// adoptedName is empty.
	void rebind(const std::string &oldName, const std::string &adoptedName);

	// Drop a local reservation (record removal / engine shutdown -- NEVER broadcast detach; see
	// the sticky-name contract above). Unknown / already-released names are a safe no-op.
	void release(const std::string &resolvedName);

	// Current local reservation count (self-test introspection).
	size_t reservedCount();

private:
	std::mutex mutex_;
	std::set<std::string> reserved_;
};

} // namespace moxrelay
