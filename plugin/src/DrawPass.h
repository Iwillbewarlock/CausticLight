#pragma once

#include "CausticMath.h"

#include <array>
#include <cstddef>

// CausticLight's own draw pass. After opaque geometry (Hooks.cpp) it draws one fullscreen
// triangle per active zone, scissored to the zone, with shaders/CausticPass.hlsl and a multiply
// blend onto the main colour target. Scene depth is read as a texture; the engine's own
// shaders are not touched.
//
// Threads: everything here runs on the main thread (render hooks and SKSE tasks alike).
// Creation, texture loading and all logging happen from the task; Render() only reads engine
// renderer state, maps one constant buffer per zone and draws.
namespace cl::DrawPass
{
	// Task: create shaders (bytecode compiled at build time), constant buffer, blend / raster /
	// depth / sampler states. Logs the result once. Safe to call every tick.
	bool EnsureCreated();

	// the Surface Tides bind watch, installed at data load (before Surface Tides hooks the renderer)
	void WatchTidesEarly();

	// Task: load the calm atlas through the engine's texture loader (bounded retries, logged).
	void EnsureTextures();

	// Task: per-slot zone records for the next frames (slot = index, unused = false).
	void Stage(const std::array<math::ZoneGpuInput, math::kMaxDrawZones>& zones, const std::array<bool, math::kMaxDrawZones>& used);
	void ClearStaging();

	// Per-frame hook: the slot's atlas frame clock (0..64).
	void SetFrameClock(std::size_t slot, float clock);

	// Per-frame hook: a new frame starts (the pass draws at most once per frame).
	void BeginFrame();

	// Post-opaque hook: draw every staged zone.
	void Render();

	// Task: one-time logs gathered by Render (first frame diagnostics, skip reasons, Map failure).
	void ReportRenderInfo();

	// Hooks.cpp: the post-opaque call site was written.
	void NoteHookInstalled(bool installed);

	// Runtime switch (toggle key). On by default.
	void SetEnabled(bool on);
	bool Enabled();

	// Task, once per second with debug on: did the pass draw this second, how many zones and
	// how many pixels its scissor rects covered. Resets the counters.
	void LogSecond();

	bool Ready();
	bool AtlasReady();
}
