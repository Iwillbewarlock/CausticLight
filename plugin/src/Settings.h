#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>


namespace cl
{
	// A zone is a water area: a world-space rectangle on the water plane z = waterZ.
	struct ZoneConfig
	{
		std::string          name;
		std::string          cell;        // "EditorID" or "0xFormID~Plugin.esp"
		std::string          worldspace;  // optional, exterior only
		float                waterZ{ 0.0f };
		std::array<float, 4> rect{ 0.0f, 0.0f, 0.0f, 0.0f };  // minX, minY, maxX, maxY
		float                height{ 1000.0f };  // how far above the water the reflected light reaches (250 missed ceilings)
		float                baseAgitation{ 0.2f };
		float                brightness{ 1.0f };
	};

	struct Settings
	{
		bool  debug{ false };        // log each active zone's inputs/outputs once per second
		bool  debugTint{ false };    // shader: flat tint inside zones (start value; CL_DebugTint overrides in game)
		float debugTintStrength{ 1.0f };  // debugTint output = 1 + strength (1.0 = x2), 0.01..4
		float tickHz{ 10.0f };       // outputs: smoothing, layer weights, playback speed
		float weatherHz{ 1.0f };     // weather / game-hour inputs
		float actorHz{ 4.0f };       // actor-in-water scan + key light
		float placementHz{ 2.0f };   // active-zone check (cell attached + camera distance)
		float maxDistance{ 3000.0f };  // camera distance to the zone where it fades to 0 and drops out (when not from the shadows)
		bool  maxDistanceFromShadows{ true };  // the game's shadow distance (fShadowDistance:Display, read live) instead of maxDistance
		float actorRange{ 600.0f };
		float lightSearchRange{ 1500.0f };
		float impulseDecaySeconds{ 1.5f };
		bool  autoZones{ true };  // find the water around the player by itself (no zone needed in zones.json)
		std::string renderer{ "auto" };   // auto | cs | enb | vanilla: 'enb' without ENB exercises the ENB path (multipliers 1, logged)
		bool  tidesWatchUnderEnb{ false };  // watch the context for Surface Tides' field under ENB (its wrapper; off until an ENB user tests it)
		bool  autoZonesOutdoors{ true };   // sunlit / moonlit water outdoors (automatic and manual zones); tested in game (Riften canal)

		// The player's own knobs: Data/SKSE/Plugins/CausticLight.ini, read at data load and again whenever the file
		// changes (checked once a second), so it can be tuned with the game running. Its values win over zones.json's.
		float iniStrength{ -1.0f };     // [Caustics] fStrength: the reflection strength (CL_Strength); < 0 = not set
		float rippleContrast{ 1.0f };   // [Caustics] fRippleContrast: the ripple pattern's contrast (0 = an even glow, 1 = physical)
		float floorStrength{ 1.0f };    // [Caustics] fFloorStrength: the pool floor pattern
		bool  useSurfaceTides{ true };  // [SurfaceTides] bUseSurfaceTides: borrow the real water surface when Surface Tides runs

		// Runtime control is through console globals in CausticLight.esp (CL_Enabled,
		// CL_DebugTint, CL_Strength, CL_Corner, CL_Remove - see Zones.cpp); there are no hotkeys.

		std::vector<ZoneConfig> zones;

		static Settings& Get();

		// Reads Data/SKSE/Plugins/CausticLight/zones.json. Returns false (and keeps defaults,
		// no zones) when the file is missing or malformed; the reason is logged.
		bool Load();

		// Reads Data/SKSE/Plugins/CausticLight.ini (after zones.json: its keys win). True when the file was read.
		bool LoadIni();
		// True (and re-read) when the ini changed on disk since the last read.
		bool ReloadIniIfChanged();

		// Edit zones.json in place: keeps every other entry and setting, pretty-prints.
		// (Comments in the file are not preserved.) Also updates `zones` in memory.
		bool AppendZone(const ZoneConfig& zone);
		bool RemoveZone(std::string_view name);
	};
}
