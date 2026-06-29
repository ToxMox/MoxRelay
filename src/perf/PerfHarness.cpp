// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// PerfHarness implementation. See PerfHarness.hpp for the contract.
// The ramp uses color_source_v3 (deterministic, GPU-cheap) so the measured cost is the
// SENDER path -- texrender passes + CopyResource + Flush -- not source production.

#include "PerfHarness.hpp"

#include "app/AppSettings.hpp"
#include "app/CliOptions.hpp"
#include "app/HelperConfig.hpp" // kMoxRelayVersion (single-sourced version string)
#include "app/LogSink.hpp"
#include "app/ObsBootstrap.hpp"
#include "app/SpoutNaming.hpp"
#include "obs/SourceFactory.hpp"
#include "spout/SpoutSenderEngine.hpp"

#include <QCoreApplication>
#include <QDir>

#include <obs.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace moxrelay {

namespace {

// The perf senders' {port} segment. Distinct from the fleet defaults (7341+) so a harness run
// never collides with a live fleet's names; the allocator would pre-resolve a collision anyway.
constexpr int kPerfPort = 7399;

// Distinct, fully-opaque ABGR colors (color_source "color" layout), cycled across the ramp.
constexpr uint32_t kPalette[] = {
	0xFF2040C0u, 0xFF40C020u, 0xFFC02040u, 0xFF20C0C0u,
	0xFFC0C020u, 0xFFC020C0u, 0xFF6080E0u, 0xFF80E060u,
};

struct WindowStats {
	double meanMs = 0.0;
	double p99Ms = 0.0;
};

WindowStats stats_of(std::vector<double> &values)
{
	WindowStats s;
	if (values.empty())
		return s;
	double sum = 0.0;
	for (double v : values)
		sum += v;
	s.meanMs = sum / double(values.size());
	std::sort(values.begin(), values.end());
	const size_t idx = std::min(values.size() - 1, size_t(std::ceil(double(values.size()) * 0.99)) - 0);
	s.p99Ms = values[std::min(idx, values.size() - 1)];
	return s;
}

// Emit one JSONL line: stdout always; the --out file (append) when given.
void emit_line(const std::string &line, std::ofstream *outFile)
{
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
	if (outFile && outFile->is_open()) {
		(*outFile) << line << "\n";
		outFile->flush();
	}
}

} // namespace

int run_perf(int argc, char **argv, const CliOptions &options)
{
	// WINDOWS-subsystem build: reattach the parent console (dev shell) so perf JSONL stays visible;
	// fall back to the file log sink when detached / on CI. FIRST, before any libobs boot.
	installConsoleOrFileLogSink();

	QCoreApplication app(argc, argv); // R6: core only -- no window, no platform plugin

	// 1. Boot at the cell's tier/resolution. Worker-grade strictness: every gate hard-fails
	//    (a perf number measured against a half-loaded module set would be garbage).
	BootstrapOptions boot;
	boot.fpsNum = options.fpsTier;
	boot.baseWidth = options.perfWidth;
	boot.baseHeight = options.perfHeight;
	boot.adapter = options.adapter;
	boot.rundir = options.rundir.toStdString();
	// Hermetic gate: keep the per-module config/cache out of the real %LOCALAPPDATA%/MoxRelay.
	boot.moduleConfigDirOverride = QDir::tempPath().toStdString() + "/moxrelay-modulecfg";
	const BootstrapResult r = ObsBootstrap::startup(boot);
	if (!r.started || !r.gateA || !r.gateB) {
		std::fprintf(stderr,
			     "[perf] libobs boot failed: started=%d gateA=%d gateB=%d moduleFailures=%zu\n",
			     r.started, r.gateA, r.gateB, r.moduleFailures);
		ObsBootstrap::shutdown();
		return 2;
	}
	{
		const auto registered = SourceFactory::registeredInputIds();
		if (registered.find("color_source_v3") == registered.end()) {
			std::fprintf(stderr, "[perf] color_source_v3 is not registered (image-source missing)\n");
			ObsBootstrap::shutdown();
			return 2;
		}
	}

	const double achievedFps =
		r.achievedFpsDen ? double(r.achievedFpsNum) / double(r.achievedFpsDen) : double(options.fpsTier);
	const double frameBudgetMs = 1000.0 / achievedFps;
	const char *flushModeName = options.perfFlushBatched ? "batched" : "immediate";

	std::ofstream outFile;
	if (!options.perfOutPath.isEmpty()) {
		outFile.open(options.perfOutPath.toStdString(), std::ios::app);
		if (!outFile.is_open())
			std::fprintf(stderr, "[perf] WARNING: could not open --out '%s' -- stdout only\n",
				     options.perfOutPath.toUtf8().constData());
	}

	int exitCode = 0;
	SpoutSenderEngine engine;
	std::vector<CreatedSource> sources;

	do {
		if (!engine.setFlushMode(options.perfFlushBatched ? SpoutSenderEngine::FlushMode::Batched
								  : SpoutSenderEngine::FlushMode::Immediate)) {
			exitCode = 3;
			break;
		}
		engine.enablePerfStats(true);
		// Honor the persistent Max Spout senders preference, but never below this run's ramp cap --
		// a high --max-n run must not be throttled by the default. Set before start().
		engine.setMaxSenders(std::max(AppSettings().maxSenders(), options.perfMaxN));
		if (!engine.start()) {
			std::fprintf(stderr, "[perf] sender engine refused to start\n");
			exitCode = 3;
			break;
		}

		const std::string machine = SpoutNaming::localMachineName();

		// Cell meta line first, so the JSONL is self-describing.
		{
			char meta[512];
			std::snprintf(meta, sizeof(meta),
				      "{\"meta\":{\"tier\":%d,\"achieved_fps\":%.3f,\"w\":%d,\"h\":%d,"
				      "\"flush\":\"%s\",\"adapter\":%d,\"max_n\":%d,\"settle_s\":%d,"
				      "\"measure_s\":%d,\"budget_pct\":%.1f,\"lagged_pct\":%.2f,"
				      "\"frame_budget_ms\":%.4f,\"version\":\"%s\"}}",
				      options.fpsTier, achievedFps, options.perfWidth, options.perfHeight,
				      flushModeName, r.achievedAdapter, options.perfMaxN, options.perfSettleSec,
				      options.perfMeasureSec, options.perfBudgetPct, options.perfLaggedPct,
				      frameBudgetMs, kMoxRelayVersion);
			emit_line(meta, &outFile);
		}

		int ceilingN = 0;        // last N that PASSED the criterion
		int criterionAtN = 0;    // the N that fired it (0 = never fired)
		std::string criterion;   // "budget" | "lagged" | "" (never fired)

		for (int n = 1; n <= options.perfMaxN; ++n) {
			// Attach sender #n: a fresh color source at the cell resolution.
			char name[32];
			std::snprintf(name, sizeof(name), "Perf%02d", n);
			obs_source_t *src = SourceFactory::createColorSource(
				name, kPalette[size_t(n - 1) % (sizeof(kPalette) / sizeof(kPalette[0]))],
				options.perfWidth, options.perfHeight);
			if (!src) {
				std::fprintf(stderr, "[perf] color source #%d creation failed\n", n);
				exitCode = 3;
				break;
			}
			CreatedSource cs;
			cs.source = src;
			cs.name = name;
			sources.push_back(cs);
			if (engine.attach(src, machine, kPerfPort, name) < 0) {
				std::fprintf(stderr, "[perf] attach #%d failed\n", n);
				exitCode = 3;
				break;
			}

			// Settle, then verify the new slot actually sends (catches init failures early).
			std::this_thread::sleep_for(std::chrono::seconds(options.perfSettleSec));
			{
				bool newUp = false;
				const auto deadline =
					std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (std::chrono::steady_clock::now() < deadline && !newUp) {
					for (const auto &info : engine.slotInfos()) {
						if (info.initFailed) {
							std::fprintf(stderr, "[perf] slot init failed at n=%d\n", n);
							exitCode = 3;
							break;
						}
					}
					if (exitCode)
						break;
					const auto infos = engine.slotInfos();
					size_t sending = 0;
					for (const auto &info : infos) {
						if (info.sends > 0 && info.lastSendOk)
							sending++;
					}
					newUp = (infos.size() == size_t(n)) && (sending == size_t(n));
					if (!newUp)
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
				}
				if (exitCode)
					break;
				if (!newUp) {
					std::fprintf(stderr, "[perf] sender #%d never came up\n", n);
					exitCode = 3;
					break;
				}
			}
			engine.drainPerfSamples(); // discard the settle window's samples

			// Measure window.
			const uint32_t total0 = obs_get_total_frames();
			const uint32_t lagged0 = obs_get_lagged_frames();
			std::this_thread::sleep_for(std::chrono::seconds(options.perfMeasureSec));
			const uint32_t total1 = obs_get_total_frames();
			const uint32_t lagged1 = obs_get_lagged_frames();
			const double avgMs = double(obs_get_average_frame_time_ns()) / 1e6;
			const double activeFps = obs_get_active_fps();

			const uint32_t totalDelta = total1 > total0 ? total1 - total0 : 0;
			const uint32_t laggedDelta = lagged1 > lagged0 ? lagged1 - lagged0 : 0;
			const double laggedRatioPct =
				totalDelta ? 100.0 * double(laggedDelta) / double(totalDelta) : 0.0;
			const double budgetUsedPct = 100.0 * avgMs / frameBudgetMs;

			// Engine-local timings for the window.
			auto samples = engine.drainPerfSamples();
			std::vector<double> sendMs, workMs, pass1Ms, pass2Ms, flushMs;
			sendMs.reserve(samples.size());
			workMs.reserve(samples.size());
			uint32_t overruns = 0;
			for (const auto &s : samples) {
				const double work = s.pass1Ms + s.pass2Ms + s.sendMs + s.flushMs;
				sendMs.push_back(s.sendMs);
				workMs.push_back(work);
				pass1Ms.push_back(s.pass1Ms);
				pass2Ms.push_back(s.pass2Ms);
				flushMs.push_back(s.flushMs);
				if (work > frameBudgetMs)
					overruns++;
			}
			const WindowStats send = stats_of(sendMs);
			const WindowStats work = stats_of(workMs);
			const WindowStats pass1 = stats_of(pass1Ms);
			const WindowStats pass2 = stats_of(pass2Ms);
			const WindowStats flush = stats_of(flushMs);
			const double overrunPct =
				samples.empty() ? 0.0 : 100.0 * double(overruns) / double(samples.size());

			char line[768];
			std::snprintf(
				line, sizeof(line),
				"{\"n\":%d,\"fps\":%.3f,\"avg_ms\":%.4f,\"budget_used_pct\":%.2f,"
				"\"lagged\":%u,\"total\":%u,\"lagged_pct\":%.4f,"
				"\"send_ms_mean\":%.4f,\"send_ms_p99\":%.4f,"
				"\"pass1_ms_mean\":%.4f,\"pass2_ms_mean\":%.4f,\"flush_ms_mean\":%.4f,"
				"\"work_ms_mean\":%.4f,\"work_ms_p99\":%.4f,\"overrun_pct\":%.2f,"
				"\"frames_sampled\":%zu}",
				n, activeFps, avgMs, budgetUsedPct, laggedDelta, totalDelta, laggedRatioPct,
				send.meanMs, send.p99Ms, pass1.meanMs, pass2.meanMs, flush.meanMs, work.meanMs,
				work.p99Ms, overrunPct, samples.size());
			emit_line(line, &outFile);
			std::fprintf(stderr, "[perf] n=%d fps=%.2f avg=%.3fms (%.1f%% budget) lagged=%.3f%%\n", n,
				     activeFps, avgMs, budgetUsedPct, laggedRatioPct);

			// Failure criterion (either threshold).
			if (budgetUsedPct > options.perfBudgetPct || laggedRatioPct > options.perfLaggedPct) {
				criterionAtN = n;
				criterion = budgetUsedPct > options.perfBudgetPct ? "budget" : "lagged";
				break;
			}
			ceilingN = n;
		}

		if (exitCode)
			break;

		char summary[512];
		std::snprintf(summary, sizeof(summary),
			      "{\"ceiling\":{\"tier\":%d,\"w\":%d,\"h\":%d,\"flush\":\"%s\","
			      "\"ceiling_n\":%d,\"criterion_at_n\":%d,\"criterion\":\"%s\","
			      "\"ramp_capped\":%s}}",
			      options.fpsTier, options.perfWidth, options.perfHeight, flushModeName, ceilingN,
			      criterionAtN, criterion.empty() ? "none" : criterion.c_str(),
			      (criterionAtN == 0 && ceilingN == options.perfMaxN) ? "true" : "false");
		emit_line(summary, &outFile);
	} while (false);

	// Teardown (locked order): engine first (removes the callback + GPU teardown via queued
	// graphics tasks), then our source refs, then libobs.
	engine.stop();
	for (CreatedSource &cs : sources) {
		obs_source_dec_showing(cs.source);
		obs_source_release(cs.source);
	}
	ObsBootstrap::shutdown();
	return exitCode;
}

} // namespace moxrelay
