// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioRing -- a lock-free single-producer/single-consumer ring of interleaved STEREO float
// frames. One ring exists per attached source: the producer is that source's own audio push
// thread (capture thread, media decode thread -- different sources push concurrently, but each
// ring has exactly one producer), the consumer is the instance's single audio render thread.
//
// NEVER-BLOCK CONTRACT: writeFrames is drop-on-full (all-or-nothing per chunk -- the caller
// counts the drop); readFrames returns what is available. Neither side ever waits, so a frozen
// consumer can never stall a source push thread and a starved producer can never stall the
// render loop.
//
// Capacity is a power of two FRAMES (one frame = 2 floats). The default (65536 frames at the
// 48 kHz line rate, ~1365 ms) is deliberately generous: burst headroom -- device capture
// packets, decode chunks, and audio-filter re-blocking arrive in bursts well above the
// steady-state fill the consumer maintains -- plus room for a per-source sync-offset delay
// reserve (up to 950 ms of deliberately resident fill) on top of the prime cushion; capacity
// is NOT resident latency.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace moxrelay {

class AudioRing {
public:
	static constexpr size_t kDefaultFrames = 65536; // power of two; ~1365 ms of 48 kHz stereo

	explicit AudioRing(size_t frameCapacity = kDefaultFrames)
		: capacity_(roundUpPow2(frameCapacity)),
		  mask_(capacity_ - 1),
		  buf_(capacity_ * 2, 0.0f)
	{
	}

	AudioRing(const AudioRing &) = delete;
	AudioRing &operator=(const AudioRing &) = delete;

	size_t capacityFrames() const { return capacity_; }

	// Producer side. Frames currently free for writing.
	size_t freeFrames() const
	{
		const uint64_t head = head_.load(std::memory_order_acquire);
		const uint64_t tail = tail_.load(std::memory_order_relaxed);
		return capacity_ - size_t(tail - head);
	}

	// Producer side. Writes `frames` interleaved stereo frames; all-or-nothing: returns false
	// (writing nothing) when the ring lacks space -- the caller drops the chunk and counts it.
	bool writeFrames(const float *interleaved, size_t frames)
	{
		if (frames > freeFrames())
			return false;
		const uint64_t tail = tail_.load(std::memory_order_relaxed);
		for (size_t i = 0; i < frames; ++i) {
			const size_t slot = size_t((tail + i) & mask_) * 2;
			buf_[slot] = interleaved[i * 2];
			buf_[slot + 1] = interleaved[i * 2 + 1];
		}
		tail_.store(tail + frames, std::memory_order_release);
		return true;
	}

	// Consumer side. Frames currently buffered.
	size_t fillFrames() const
	{
		const uint64_t tail = tail_.load(std::memory_order_acquire);
		const uint64_t head = head_.load(std::memory_order_relaxed);
		return size_t(tail - head);
	}

	// Consumer side. Reads up to `maxFrames` interleaved stereo frames; returns the count read.
	size_t readFrames(float *interleaved, size_t maxFrames)
	{
		const uint64_t tail = tail_.load(std::memory_order_acquire);
		const uint64_t head = head_.load(std::memory_order_relaxed);
		size_t avail = size_t(tail - head);
		if (avail > maxFrames)
			avail = maxFrames;
		for (size_t i = 0; i < avail; ++i) {
			const size_t slot = size_t((head + i) & mask_) * 2;
			interleaved[i * 2] = buf_[slot];
			interleaved[i * 2 + 1] = buf_[slot + 1];
		}
		head_.store(head + avail, std::memory_order_release);
		return avail;
	}

	// Absolute stream positions (frames since construction, monotonic). The producer's tail
	// position marks where the NEXT write lands; the consumer's head position marks the next
	// frame to be read. Used to address discontinuity joints across the SPSC boundary.
	uint64_t tailPos() const { return tail_.load(std::memory_order_relaxed); } // producer side
	uint64_t headPos() const { return head_.load(std::memory_order_relaxed); } // consumer side

	// Consumer side. Discards up to `maxFrames` buffered frames (recovery resync); returns the
	// count discarded.
	size_t discardFrames(size_t maxFrames)
	{
		const uint64_t tail = tail_.load(std::memory_order_acquire);
		const uint64_t head = head_.load(std::memory_order_relaxed);
		size_t avail = size_t(tail - head);
		if (avail > maxFrames)
			avail = maxFrames;
		head_.store(head + avail, std::memory_order_release);
		return avail;
	}

private:
	static size_t roundUpPow2(size_t v)
	{
		size_t p = 1;
		while (p < v)
			p <<= 1;
		return p;
	}

	const size_t capacity_; // frames
	const size_t mask_;
	std::vector<float> buf_; // interleaved stereo: 2 floats per frame
	std::atomic<uint64_t> head_{0}; // consumer cursor (frames, monotonic)
	std::atomic<uint64_t> tail_{0}; // producer cursor (frames, monotonic)
};

} // namespace moxrelay
