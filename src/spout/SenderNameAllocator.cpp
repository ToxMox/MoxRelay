// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SenderNameAllocator implementation. Extracted from the M2.1 inline resolve in
// SpoutSenderEngine (which probed the registry per-slot on the graphics thread) and moved to
// attach time with a process-local reservation set -- the registry probe alone cannot see a
// sibling slot whose sender has not sent its first frame yet (registration happens at the first
// SendTexture, not at SetSenderName).

#include "SenderNameAllocator.hpp"

#include "app/SpoutNaming.hpp"

#include "SpoutSenderNames.h"

#include <cstdio>

namespace moxrelay {

namespace {
// Suffix budget: far above the session-wide sender cap (default 64 registry slots); hitting it
// means something is pathologically wrong -- fail loudly rather than spin.
constexpr int kMaxSuffix = 256;

// Truncation budget (vendored SDK cap). The registry stores names in fixed
// char[SpoutMaxSenderNameLen] buffers (SpoutSenderNames.h), one byte of which is the terminator.
// Over-long names are truncated FIRST -- to a budget that also reserves room for the WORST
// uniquification suffix ("_" + up to 3 digits at kMaxSuffix) -- and uniquified AFTER, so two long
// names that truncate equal still get distinct suffixes and no suffixed candidate can ever exceed
// the registry cap (which would re-collide at registration when the registry clips it back).
constexpr size_t kNameCap = SpoutMaxSenderNameLen - 1; // longest storable name (ex terminator)
constexpr size_t kSuffixReserve = 4;                   // worst suffix: "_256"
constexpr size_t kBaseBudget = kNameCap - kSuffixReserve;
} // namespace

std::string SenderNameAllocator::resolve(const std::string &machine, int port, const std::string &source,
					 const std::string &sticky, bool *wasSticky)
{
	if (wasSticky)
		*wasSticky = false;

	std::string base = SpoutNaming::makeSenderName(machine, port, source);
	if (base.size() > kBaseBudget)
		base.resize(kBaseBudget); // truncate FIRST, uniquify after (see kBaseBudget rationale)

	std::lock_guard<std::mutex> lock(mutex_);

	// STICKY re-attach: the caller already owns this reservation (kept across a broadcast
	// detach), so the name cannot have been handed to anyone else in between -- return it
	// unchanged, no new reservation. A sticky name we do NOT hold (released at shutdown, or a
	// stale caller record) falls through to fresh resolution.
	if (!sticky.empty() && reserved_.count(sticky) > 0) {
		if (wasSticky)
			*wasSticky = true;
		return sticky;
	}

	// Pure shared-memory registry handle; cheap to construct, no D3D11.
	spoutSenderNames names;
	names.CleanSenders(); // prune names held by crashed senders so they can be reclaimed

	std::string candidate = base;
	int counter = 2; // "_2" first: Spout's own backstop starts at "_1"
	while (names.FindSenderName(candidate.c_str()) || reserved_.count(candidate) > 0) {
		if (counter > kMaxSuffix) {
			std::fprintf(stderr,
				     "[SenderNameAllocator] no free name for '%s' within %d suffixes\n",
				     base.c_str(), kMaxSuffix);
			return std::string();
		}
		candidate = base + "_" + std::to_string(counter++);
	}

	reserved_.insert(candidate);
	return candidate;
}

void SenderNameAllocator::rebind(const std::string &oldName, const std::string &adoptedName)
{
	if (adoptedName.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!oldName.empty())
		reserved_.erase(oldName);
	reserved_.insert(adoptedName);
}

void SenderNameAllocator::release(const std::string &resolvedName)
{
	if (resolvedName.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	reserved_.erase(resolvedName);
}

size_t SenderNameAllocator::reservedCount()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return reserved_.size();
}

} // namespace moxrelay
