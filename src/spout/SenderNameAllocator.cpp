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
} // namespace

std::string SenderNameAllocator::resolve(const std::string &machine, int port, const std::string &source)
{
	const std::string base = SpoutNaming::makeSenderName(machine, port, source);

	std::lock_guard<std::mutex> lock(mutex_);

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
