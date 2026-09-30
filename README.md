# CausticLight

Water caustics for Skyrim Special Edition / Anniversary Edition: lamp, torch, magelight, sun and moon light
reflected off rippling water onto nearby walls, ceilings, bridge undersides and pool floors. An SKSE plugin that
adds one render pass of its own; it does not replace any shader files.

- Player guide: [docs/README.en.md](docs/README.en.md) (English), [docs/README.md](docs/README.md) (한국어)
- Design notes (Korean): [docs/DRAWPASS.md](docs/DRAWPASS.md)

## Layout

| Path | What |
|---|---|
| `plugin/src` | The SKSE plugin: water search, light choice, lit planes (shadows), the draw pass |
| `plugin/shaders/CausticPass.hlsl` | The pass itself; `plugin/src/CausticMath.*` is its C++ mirror used by the tests |
| `plugin/tests` | `CausticMathTest` (CPU) and `PassRender` (GPU vs the C++ mirror, dumps for the Blender compares) |
| `tools/gen.cjs` | Generates the caustic pattern atlases (refraction for the floor, mirror reflection for the walls) |
| `tools/blender-*.py` | Blender Cycles reference renders and the compares the strengths were fitted with |
| `tools/build-all.ps1` | Builds atlases, ESP and assembles `dist/CausticLight` |
| `dist/CausticLight` | The mod as shipped |

## Building

Requirements: Visual Studio 2022 (or newer) with C++, CMake, vcpkg (`VCPKG_ROOT`), Node.js; for the assets also
texconv (DirectXTex) and optionally Blender.

```
git clone --recurse-submodules https://github.com/Iwillbewarlock/CausticLight
cd CausticLight\plugin
build.cmd              :: configure, build Release, run the CPU and GPU tests
cd ..
pwsh tools\build-all.ps1 -SkipAtlas   :: ESP, textures, dist (drop -SkipAtlas to regenerate the atlases)
```

Tool paths for `build-all.ps1` go in `tools/local-paths.ps1` (not tracked): `$texconv`, `$blender`, `$validate`.

## License

GPL-3.0-only. See [LICENSE](LICENSE) and [NOTICE](NOTICE) for the credits: Community Shaders, Dynamic Terrain
Deformation, Surface Tides, caustic-volume, CommonLibSSE-NG.
