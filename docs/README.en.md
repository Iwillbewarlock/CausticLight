# CausticLight — water light on walls and ceilings

Water throws lamp light back onto nearby walls, bridge undersides, ceilings and the pool floor as a moving net of
light (caustics). CausticLight draws that light for Skyrim SE / AE with an SKSE plugin. It does not touch Community
Shaders' files; it paints one extra pass of its own.

## Install

- Install with your mod manager and enable `CausticLight.esp` (ESL-flagged, no masters).
- Requires: SKSE, Address Library. Skyrim SE or AE (not VR).
- Optional: Community Shaders (works without it), Surface Tides (the pattern then follows the real water surface).

## What it does on its own

- **Finds the water**: around the player it traces rays down and maps the water by itself, indoors and outdoors,
  within a second, never spending more than 2 ms of ray time in a frame. No zones to place.
- **Lights**: the three lights that bring the most light to the water are reflected, each in its own colour and
  strength: placed lamps, a carried torch, spell light in hand, magelight, Light Placer lights, the sun and the moon.
  Every other light still counts as the light already on the walls.
- **Shadows**: for each reflected light it works out which water that light actually reaches, so a lamp in the
  next room does not reflect through the wall, and water under a bridge does not reflect the sun.
- **Brightness**: reflected light divided by the light the surface already gets. A bridge underside turned away
  from the lamp shows the pattern strongly; a wall the lamp lights head-on shows it softly, as in real life.
  Community Shaders' linear lighting and inverse-square lights are read and counted the same way.
- **Ripples**: weather, wind and swimmers change the water; the pattern and its spread follow. With Surface Tides
  the pattern comes from the real water surface, including the ripples you make walking or swimming.
- **Pool floor**: light refracted through the surface gathers in bands on the floor, fading with depth and murk.
- **Distance**: caustics reach as far as your game's shadow distance and fade out at its end, like the shadows.
- **Stability**: a light keeps its place until another brings 20 % more, so near-equal lights do not flicker,
  and a torch's flame flicker is smoothed.

## Settings (CausticLight.ini)

`SKSE\Plugins\CausticLight.ini` — edit and save with the game running; changes apply within a second or two.

| Key | Default | Meaning |
|---|---|---|
| `fStrength` | 6.4 | Reflected light strength. 2.14 = the physical amount (matched to a Blender Cycles render), 6.4 = chosen to show in game, 0–50 |
| `fRippleContrast` | 1.0 | Ripple pattern contrast. 0 = an even glow, 1 = physical, up to 3 |
| `fFloorStrength` | 1.0 | Pool floor pattern, 0–5 |
| `bFollowShadowDistance` | 1 | 1 = caustics reach as far as the game's shadows (fShadowDistance) |
| `fMaxDistance` | 3000 | Where caustics end when not following the shadows |
| `bUseSurfaceTides` | 1 | Use Surface Tides' water surface when it is installed |

`fStrength` wins over the value in your save at every load.

## Console (no hotkeys)

| Command | Effect |
|---|---|
| `set CL_Enabled to 0` / `1` | Off / on |
| `set CL_Strength to N` | Strength 0–50 for this session (the lasting value is `fStrength` in the ini) |
| `set CL_DebugTint to 1` / `2` / `3` / `0` | Debug views: zone reach / where reflected light lands / direct lamp light / off |

Do not save with a debug view on.

## Community Shaders and ENB

- The renderer is detected at start and logged (`renderer light:` in `CausticLight.log`).
- **Community Shaders**: linear lighting and inverse-square light settings are read.
- **ENB**: the [ENVIRONMENT] light multipliers in enbseries.ini are read through the ENB SDK. The Surface Tides link
  is off under ENB by default (`tidesWatchUnderEnb` in zones.json). **ENB is not tested yet** — reports welcome:
  1. the `renderer light: ENB ...` and `ENB light multipliers ...` lines from `CausticLight.log`;
  2. caustics on walls near lit interior water;
  3. no flicker or corruption on the water or the screen;
  4. no crash opening / closing the ENB editor (Shift+Enter).

## Cost

Measured in game (RTX 3070 laptop, the whole screen inside a water zone): usually 0.14–0.2 ms per frame, peaks about 0.5 ms.

## Limits

- Particle lights (Community Shaders / ENB flame glow lights) live only inside the renderer and cannot be read.
- Blocking between the water and a wall is known only for what is on screen.
- Puddles narrower than about 1.6 m may be missed by the water search.
- ENB's time-of-day multiplier curve is not public; it is blended linearly (an assumption).

## Source and license

CausticLight is open source under GPL-3.0: https://github.com/Iwillbewarlock/CausticLight

## Credits

- **Community Shaders** contributors: the two engine call sites CausticLight hooks come from Community Shaders' references.
- **NearMidnightNow** (Dynamic Terrain Deformation): those call sites were learned from DTD's hooks.
- **Surface Tides** (Nexus mod 191955) and its author: CausticLight reads its water field, following its data layout.
- **ScottieFox** (caustic-volume, MIT): the built-in caustic pattern generator started from its wave recipe; its license is included.
- **CommonLibSSE-NG** (MIT).
