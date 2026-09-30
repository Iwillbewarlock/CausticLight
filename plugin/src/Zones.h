#pragma once

namespace cl::Zones
{
	// kDataLoaded: resolve the ESP base forms and the zones' cells, register the events.
	bool Initialize();

	// kPreLoadGame / kNewGame: forget every handle (the refs belong to the old game state),
	// bump the task generation so queued tasks do nothing, and stop placement until
	// OnGameReady.
	void Reset();

	// kPostLoadGame / kNewGame: the world is live; placement may start.
	void OnGameReady();

	// Called once per rendered frame from inside the render routine (see Hooks.cpp). Only
	// keeps time and writes the shimmer; queues one SKSE task for everything else.
	void OnFrame();
}
