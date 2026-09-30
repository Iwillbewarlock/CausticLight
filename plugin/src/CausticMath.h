#pragma once

// Pure decision logic for CausticLight. No game headers here: everything takes and returns
// plain numbers, so tests/CausticMathTest.cpp can check it without Skyrim.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace cl::math
{
	struct RGB
	{
		float r{ 0.0f };
		float g{ 0.0f };
		float b{ 0.0f };
	};

	using Vec3 = std::array<float, 3>;

	[[nodiscard]] float Clamp01(float v) noexcept;
	[[nodiscard]] float Lerp(float a, float b, float t) noexcept;
	[[nodiscard]] RGB   Lerp(const RGB& a, const RGB& b, float t) noexcept;

	// ---- Outputs -------------------------------------------------------------------------

	// Crossfade over the three caustic layers. Sums to 1 for every a.
	//   a <  0.5 : Calm = 1-2a, Normal = 2a
	//   a >= 0.5 : Normal = 2-2a, Rough = 2a-1
	struct LayerWeights
	{
		float calm{ 1.0f };
		float normal{ 0.0f };
		float rough{ 0.0f };

		[[nodiscard]] float operator[](std::size_t i) const noexcept { return i == 0 ? calm : (i == 1 ? normal : rough); }
	};
	[[nodiscard]] LayerWeights CrossfadeWeights(float agitation) noexcept;

	// Distance fade: 1 up to 80 % of maxDistance, smooth to 0 at maxDistance and beyond.
	// maxDistance <= 0 disables the fade (always 1).
	[[nodiscard]] float DistanceFade(float distance, float maxDistance) noexcept;
	// The same per pixel: this point's camera distance against c.fade.x (1 when that is 0).
	[[nodiscard]] float PixelFade(const struct PassConstants& c, const Vec3& p) noexcept;

	// Playback speed multiplier: 0.35 + 1.9 * a  (0.35 .. 2.25). Real footage: calm water keeps a
	// pattern recognisable for 2-10 s, rough water for about 0.5 s.
	[[nodiscard]] float PlaybackSpeed(float agitation) noexcept;

	// Frame-rate independent exponential approach. tau = seconds to cover ~63 % of the gap.
	[[nodiscard]] float SmoothToward(float current, float target, float dt, float tau) noexcept;
	[[nodiscard]] RGB   SmoothToward(const RGB& current, const RGB& target, float dt, float tau) noexcept;
	[[nodiscard]] Vec3  SmoothToward(const Vec3& current, const Vec3& target, float dt, float tau) noexcept;

	// Atlas frame clock: 64 frames at 8 frames/s at speed 1, wrapped to [0, 64).
	inline constexpr float kAtlasFrames = 64.0f;
	inline constexpr float kAtlasFps = 8.0f;
	[[nodiscard]] float AdvanceFrameClock(float clock, float dt, float speed) noexcept;

	// ---- Agitation inputs ------------------------------------------------------------------

	struct WeatherSample
	{
		float wind01{ 0.0f };  // TESWeather::data.windSpeed / 255
		bool  rainy{ false };
		bool  snowy{ false };
		bool  cloudy{ false };
	};

	[[nodiscard]] float WeatherAgitation(const WeatherSample& w) noexcept;

	// The share of the sunlight that still arrives as a parallel beam (the rest is scattered into the sky by
	// cloud). Caustics are the beam focused by the waves: a diffuse sky makes none. Precipitation means a closed
	// cloud deck: 0 (physics). A "cloudy" weather still shows a sun disc through thinner cloud: kCloudyBeam
	// (assumed; thin-cloud direct transmittance is roughly 0.2..0.5). Clear: 1.
	inline constexpr float kCloudyBeam = 0.35f;

	// ENB's time of day (enbseries.ini [TIMEOFDAY]): dawn = SunriseTime - DawnDuration, sunrise, day, sunset,
	// dusk = SunsetTime + DuskDuration, night. A parameter has one value per point (…Dawn, …Sunrise, …Day, …Sunset,
	// …Dusk, …Night) and InteriorDay / InteriorNight. Blend: linear between the points around the clock (assumed: ENB's
	// own curve is not published); interiors: InteriorDay while the sun is up (sunrise..sunset), InteriorNight
	// from dusk to dawn, linear across dawn / dusk (assumed).
	struct EnbSchedule
	{
		float dawnDuration{ 1.0f }, sunriseTime{ 6.0f }, dayTime{ 13.0f }, sunsetTime{ 20.0f }, duskDuration{ 1.0f }, nightTime{ 1.0f };
	};
	// values: Dawn, Sunrise, Day, Sunset, Dusk, Night
	[[nodiscard]] float EnbExteriorBlend(const std::array<float, 6>& values, const EnbSchedule& s, float hour) noexcept;
	// how much it is day for the interior pair: 1 sunrise..sunset, 0 dusk..dawn, linear between
	[[nodiscard]] float EnbDayWeight(const EnbSchedule& s, float hour) noexcept;
	[[nodiscard]] float    DirectBeamFraction(const WeatherSample& w) noexcept;
	[[nodiscard]] float BlendedWeatherAgitation(const WeatherSample& last, const WeatherSample& current, float transition01) noexcept;

	// Impulse caused by one actor in water. submerge01: how deep (0..1); speed: units/s;
	// distance: actor to zone; range: impulse radius (default 600).
	[[nodiscard]] float ActorImpulse(float submerge01, float speed, float distance, float range) noexcept;
	[[nodiscard]] float DecayImpulse(float value, float dt, float tau) noexcept;
	[[nodiscard]] float TotalAgitation(float base, float weather, float impulse) noexcept;

	// ---- Water surface slope (physical) ------------------------------------------------------
	//
	// Independent wave fields add their slope variances. Wind: Cox & Munk (1954), total mean-square
	// slope 0.003 + 0.00512 U (U wind speed in m/s). Rain and a swimmer add their own ripples.
	// The RMS slope angle drives the reflection spread (tan 2 sigma), the floor focus and, normalised,
	// the pattern crossfade and playback speed.
	inline constexpr float kSlopeStill = 0.003f;        // Cox-Munk intercept: a still surface's residual ripples
	inline constexpr float kSlopePerWind = 0.00512f;    // per m/s of wind (Cox-Munk)
	inline constexpr float kWindMaxMS = 15.0f;          // TESWeather windSpeed 255 = 15 m/s (a strong breeze)
	inline constexpr float kSlopeRain = 0.012f;         // heavy rain's ring waves (order of the measured increase)
	inline constexpr float kSlopeSwimmer = 0.03f;       // a swimmer's wake near the swimmer (impulse 1)
	inline constexpr float kSlopeZoneBase = 0.02f;      // per unit of a zone's baseAgitation (drips, a current)
	[[nodiscard]] float SlopeMeanSquare(float zoneBase, float wind01, float rain01, float impulse) noexcept;
	[[nodiscard]] float SlopeAngle(float meanSquare) noexcept;           // RMS slope angle (radians)
	[[nodiscard]] float SpreadFromSlope(float sigma) noexcept;           // tan(2 sigma)
	[[nodiscard]] float AgitationFromSlope(float sigma) noexcept;        // 3 deg -> 0 .. 14 deg -> 1

	// ---- Light inputs ------------------------------------------------------------------------

	// Which lights are reflected: the key and two extras, from candidates ranked by their light at the water.
	// A light keeps its place until another brings 20 % more (hysteresis), and an extra keeps its slot index, so
	// near-equal lights do not trade places every tick (each trade would move their lit planes).
	struct RankCand
	{
		int           kind{ 0 };       // which list it came from: a light's identity is kind + id
		std::uint64_t id{ 0 };
		float         weight{ 0.0f };  // its light at the water (the ranking)
		float         intensity{ 0.0f };
		bool          dark{ false };   // its finished lit plane reaches no water: it reflects nothing here
	};
	struct SlotId
	{
		int           kind{ -1 };
		std::uint64_t id{ 0 };
	};
	struct SlotChoice
	{
		int                key{ -1 };  // index into the candidates, -1 none
		std::array<int, 2> extra{ -1, -1 };
	};
	[[nodiscard]] SlotChoice AssignReflectedSlots(std::span<const RankCand> c, SlotId prevKey, const std::array<SlotId, 2>& prevExtra,
		float keyHysteresis, float extraHysteresis, float minExtraRel);

	[[nodiscard]] float                SunLevel(float hour) noexcept;
	[[nodiscard]] float                DaylightLevel(float hour) noexcept;
	[[nodiscard]] float                WeatherDimming(const WeatherSample& w) noexcept;
	[[nodiscard]] std::array<float, 4> TimeOfDayWeights(float hour) noexcept;

	// One light's contribution to a zone. Zero at and beyond 1.5 * radius.
	[[nodiscard]] float LightContribution(float distance, float radius, float fade) noexcept;

	struct LightSample
	{
		RGB   color;
		float distance{ 0.0f };
		float radius{ 0.0f };
		float fade{ 1.0f };
	};

	struct LightResult
	{
		RGB   color{ 1.0f, 1.0f, 1.0f };
		float level{ 0.0f };
		float total{ 0.0f };  // unclamped sum (for folding in more lights)
	};

	[[nodiscard]] LightResult CombineLights(std::span<const LightSample> lights) noexcept;
	[[nodiscard]] LightResult FoldLight(const LightResult& base, const RGB& c, float w) noexcept;

	// Interior with no LIGH refs in range: the cell's own lighting colour at 0.35.
	inline constexpr float kCellLightingLevel = 0.35f;
	struct CellLightingChoice
	{
		LightResult light;
		bool        usedDirectional{ false };
	};
	[[nodiscard]] CellLightingChoice CellLightingFallback(const RGB& directional, const RGB& ambient) noexcept;

	// ---- Ripple ring ---------------------------------------------------------------------------

	inline constexpr float kRippleRadius = 150.0f;
	inline constexpr float kRipplePosTau = 0.15f;
	struct RippleState
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float amplitude{ 0.0f };
		bool  hasPosition{ false };
	};
	[[nodiscard]] RippleState StepRipple(const RippleState& s, float targetAmp, bool haveActor, float targetX, float targetY,
		float dt, float decayTau, float posTau) noexcept;

	// ---- Water zones (world-space rectangle on the water plane) ------------------------------

	struct Rect
	{
		float minX{ 0.0f };
		float minY{ 0.0f };
		float maxX{ 0.0f };
		float maxY{ 0.0f };
		bool  operator==(const Rect&) const = default;
	};

	// Rect spanning two authored corners (sorted). nullopt when either side < minSide.
	inline constexpr float kMinZoneSide = 32.0f;
	[[nodiscard]] std::optional<Rect> RectFromCorners(float ax, float ay, float bx, float by, float minSide = kMinZoneSide) noexcept;

	[[nodiscard]] bool  RectContains(const Rect& r, float x, float y) noexcept;
	[[nodiscard]] bool  RectsOverlap(const Rect& a, const Rect& b) noexcept;
	[[nodiscard]] float DistanceToRect2D(const Rect& r, float x, float y) noexcept;

	// 3D distance from p to the zone volume: the rect in x,y and [waterZ, waterZ + height] in z.
	[[nodiscard]] float DistanceToZone(const Rect& r, float waterZ, float height, const Vec3& p) noexcept;

	// Active = cell attached and the camera closer to the zone than maxDistance.
	[[nodiscard]] bool IsZoneActive(bool cellAttached, float distance, float maxDistance) noexcept;

	// ---- Camera and screen (C++ mirror of shaders/CausticPass.hlsl) ---------------------------

	// Row-major 4x4, row-vector convention: v' = v * M. Same memory layout as the engine's
	// ViewData matrices (DirectX::SimpleMath::Matrix) and HLSL `row_major` + mul(v, M).
	struct Mat4
	{
		float m[16]{};

		[[nodiscard]] float  at(int r, int c) const noexcept { return m[r * 4 + c]; }
		[[nodiscard]] float& at(int r, int c) noexcept { return m[r * 4 + c]; }
	};
	using Vec4 = std::array<float, 4>;

	[[nodiscard]] Mat4                Identity() noexcept;
	[[nodiscard]] Mat4                Mul(const Mat4& a, const Mat4& b) noexcept;  // apply a, then b
	[[nodiscard]] std::optional<Mat4> Inverse(const Mat4& m) noexcept;              // computed in double
	[[nodiscard]] Vec4                TransformRow(const Vec4& v, const Mat4& m) noexcept;

	// The scene viewport in render-target pixels plus its depth range (D3D11_VIEWPORT).
	struct Viewport
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float width{ 0.0f };
		float height{ 0.0f };
		float minDepth{ 0.0f };
		float maxDepth{ 1.0f };
	};

	struct ScreenPoint
	{
		float x{ 0.0f };      // pixels
		float y{ 0.0f };
		float depth{ 0.0f };  // depth-buffer value
		float w{ 0.0f };      // clip w (> 0 in front of the camera)
	};

	// rel = world - posAdjust. Forward projection (tests and the scissor).
	[[nodiscard]] ScreenPoint ProjectToPixel(const Vec3& rel, const Viewport& vp, const Mat4& viewProj) noexcept;

	// Inverse: pixel + depth-buffer value -> rel position. nullopt when w ~ 0 or the viewport is empty.
	[[nodiscard]] std::optional<Vec3> UnprojectPixel(float px, float py, float depth, const Viewport& vp, const Mat4& invViewProj) noexcept;

	// D3D11_RECT: right/bottom exclusive.
	struct PixelRect
	{
		int left{ 0 };
		int top{ 0 };
		int right{ 0 };
		int bottom{ 0 };
	};

	// Screen rect covering the zone volume the shader accepts: the rect grown by `reach` in x,y
	// and [waterZ, waterZ + height] in z. nullopt = nothing to draw (all 8 corners behind the
	// camera, or entirely off-screen). Any corner behind or within kNearW of the camera plane
	// (camera inside or next to the zone) -> the full viewport.
	inline constexpr float kScissorMargin = 2.0f;
	inline constexpr float kNearW = 5.0f;  // clip w (view distance) below which a corner counts as "at the camera"
	[[nodiscard]] std::optional<PixelRect> ScissorForZone(const Rect& r, float waterZ, float height, float reach, const Vec3& posAdjust,
		const Mat4& viewProj, const Viewport& vp, float margin = kScissorMargin) noexcept;

	// ---- Water mask: where the water plane really is water ------------------------------------
	//
	// A square grid over the zone's bounds, 1 = water, 0 = dry, with a zero border ring so the
	// clamp-addressed sample falls to 0 outside. Mip chain by 2x2 box filter; the shader samples
	// it trilinearly, so a coarser mip = the water fraction of a larger footprint (used for the
	// glossy spread). Built from the authored rect for manual zones, from a ray grid for automatic
	// ones. Immutable once built: shared between the task side and the render side.
	struct WaterMask
	{
		Rect                                   bounds;  // world x,y covered by the whole texture
		int                                    size{ 0 };
		std::vector<std::vector<std::uint8_t>> mips;  // mips[0] = size x size, row-major, row 0 = minY
		int                                    waterTexels{ 0 };
		Rect                                   waterBounds;  // tight world bounds of the water texels (light search, distances)
		// per reflected light (0 = key, 1..2 = extras): the water that light reaches (not in a shadow); empty = not known
		std::array<std::vector<std::vector<std::uint8_t>>, 3> litMips;
		std::array<int, 3>                                    litTexels{};
	};
	inline constexpr int kMaskSize = 64;

	// Fills mips[1..] from mips[0], counts the water texels and sets waterBounds. mips[0] must be size x size, size a power of 2.
	void BuildMaskMips(WaterMask& m);
	// Manual zone: the rect grown by a margin (the glossy spread reaches past the rect's edge).
	[[nodiscard]] std::shared_ptr<const WaterMask> MaskFromRect(const Rect& r, int size = kMaskSize);
	// Automatic zone: `water` is size x size (non-zero = water), `bounds` the area it covers; the border
	// ring is forced to 0. nullptr when no water texel is left.
	[[nodiscard]] std::shared_ptr<const WaterMask> MaskFromGrid(const Rect& bounds, int size, std::vector<std::uint8_t> water);

	// A copy of m whose lit plane for light `plane` (0 = key, 1..2 = extras) marks the water texels that light
	// reaches (lit: size x size, non-zero = lit; dry texels are never lit). An empty `lit` clears that plane.
	[[nodiscard]] std::shared_ptr<const WaterMask> WithLitPlane(const WaterMask& m, const std::vector<std::uint8_t>& lit, int plane = 0);
	// All three planes in one copy (nullptr clears that plane): a light moving to another slot takes its plane along.
	[[nodiscard]] std::shared_ptr<const WaterMask> WithLitPlanes(const WaterMask& m, const std::array<const std::vector<std::uint8_t>*, 3>& lit);

	// D3D-style trilinear sample of the 0..1 mask (clamp addressing, texel centres at (i + 0.5) / size).
	// u, v: texture space 0..1 (v = 0 at minY).
	// plane -1: the water; 0..2: the water that light reaches (the water when that plane is not known).
	[[nodiscard]] float SampleMask(const WaterMask& m, float u, float v, float lod, int plane = -1) noexcept;

	// ---- Adaptive water-grid scan (automatic zones) ----------------------------------------------
	//
	// Texel states: -1 unknown, 0 dry, 1 water. The scan visits ScanOrder(): every 4th texel first
	// (256 rays for a usable preview), then the texels between them. A texel whose coarser neighbours
	// are all known and agree takes their value without a ray, so rays go only where the shore is.
	// Features narrower than the coarse spacing (4 texels) can be missed.
	inline constexpr int kScanCoarseStep = 4;
	inline constexpr int kScanCoarseCount = (kMaskSize / kScanCoarseStep) * (kMaskSize / kScanCoarseStep);
	[[nodiscard]] const std::vector<std::uint16_t>& ScanOrder();  // kMaskSize^2 texel indices, coarse first
	// Value implied by the coarser neighbours (all known and equal), else -1 (cast a ray).
	[[nodiscard]] std::int8_t InferTexel(const std::vector<std::int8_t>& states, int ix, int iy) noexcept;
	// The previous grid moved by (dx, dy) texels (new texel (x, y) = old (x + dx, y + dy)); its border ring
	// (forced dry, never cast) and everything outside it become unknown.
	[[nodiscard]] std::vector<std::int8_t> ShiftGrid(const std::vector<std::int8_t>& old, int dx, int dy);
	// Water grid for MaskFromGrid; unknown texels take the nearest coarse sample (preview).
	[[nodiscard]] std::vector<std::uint8_t> GridFromStates(const std::vector<std::int8_t>& states);

	// Glossy spread of the reflection: water slopes of up to sigma tilt the reflected ray by up to
	// 2 sigma, so a point L from the water gathers light from a disc of radius L * tan(2 sigma).
	// sigma: 3 deg (calm) .. 14 deg (rough), from the agitation.
	[[nodiscard]] float ReflectionSpread(float agitation) noexcept;

	// ---- Surface Tides height field (optional) ------------------------------------------------
	//
	// Surface Tides (Nexus 191955) simulates the water surface around the player and gives its water shaders a
	// float4 field texture (x height / displacement strength, w valid) and a patch constant buffer
	// (Shaders/SurfaceTides/SurfaceField.hlsli). When the pass can borrow them, the brightness of the caustic
	// comes from the real surface: ripples are lenses, and light bunches by 1 / det(I + k H), H the height's
	// second derivatives. Reflection seen L from the water: the reflected ray tilts by -2 grad h, k = -2 L (a trough
	// is a concave mirror and focuses). Floor d under the water: the refracted ray tilts by (1 - 1/n) grad h,
	// k = (1 - 1/n) d (a crest is a converging lens). The caustic atlas then adds only the ripples finer than the field (kTidesFineContrast).
	struct TidesField
	{
		float              originX{ 0.0f }, originY{ 0.0f }, spacing{ 20.0f }, rest{ 0.0f };  // STPatch
		int                resolution{ 0 };                                                    // STOptions.x
		float              displacement{ 1.0f };                                               // STOptions.z
		float              enabled{ 1.0f }, edgeCells{ 2.0f }, tolerance{ 5.0f };            // STStatus x, z, w
		std::vector<float> height;                                                             // .x per cell (resolution^2)
		std::vector<float> valid;                                                              // .w per cell
	};
	inline constexpr float kTidesFocusSoftness = 0.15f;  // |det| floor: the lamp's size and the field's resolution blur the focus
	inline constexpr float kTidesFineContrast = 0.2f;    // atlas ripples finer than the field, on top of it
	// With the field, the walls need 1.32 x (not 2.14 x) the physical reflection to match Cycles: the local
	// Jacobian misses the folds of the light past the ripples' focus (tools/blender-caustic-compare.py ... tides).
	// The floor is inside the focus and matches with no factor.
	inline constexpr float kTidesReflectScale = 0.615f;  // with the lamp distance in the focus (1/Leff = 1/L + 1/D); refitted 2026-09-30 on the caustic-only render: tides 1.32 / lamp 2.14 (fitted)
	// Refracted light is bent far less than reflected: a surface tilted by t turns a near-vertical ray by t (1 - 1/n)
	// (Snell, small angles), while the reflection turns by 2t. So the floor's footprint from the ripples is
	// depth x tan(2 sigma) x (1 - 1/1.33) / 2 (physics).
	inline constexpr float kFloorSpreadFactor = (1.0f - 1.0f / 1.33f) / 2.0f;
	inline constexpr float kSunReflectScale = 1.07f;  // the sun: Cycles caustic-only fits 2.28 where the lamp fits 2.14 (fitted, not derived; 2.32 / 2.85 before, with the walls' mirror glow in the reference)
	// Where the field applies, the spread is the still surface's (sub-grid ripples only).
	inline const float     kTidesStillSpread = 0.1103f;  // tan(2 * atan(sqrt(kSlopeStill)))
	// World height of the field at (x, y) as the shader samples it (bilinear on cell centres, edge fade).
	[[nodiscard]] float TidesHeight(const TidesField& f, float x, float y) noexcept;
	// How the surface around (x, y) bunches light: 1 = flat. k1 along the light's horizontal direction (e1x, e1y),
	// k2 across it; a vertical ray has k1 = k2 = k as above. Oblique rays (TidesReflectK / TidesRefractK):
	//   reflection L from the water, ray at cos(theta) from vertical: k1 = -2 L / cos(theta), k2 = -2 L cos(theta)
	//   floor d under the water, cos i / cos t in / under the water: q = 1 - cos i / (n cos t), k1 = d q / cos^2 t, k2 = d q
	[[nodiscard]] float TidesIntensity(const TidesField& f, float x, float y, float k1, float k2, float e1x, float e1y) noexcept;
	[[nodiscard]] float TidesIntensity(const TidesField& f, float x, float y, float k) noexcept;

	// ---- Caustic pass: per-zone constant buffer (b0), must match CausticPass.hlsl exactly --------

	struct Float4
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
		float w{ 0.0f };
	};
	static_assert(sizeof(Float4) == 16);

	inline constexpr std::size_t kMaxLights = 3;  // reflected lights per zone

	struct PassConstants
	{
		float  invViewProj[16];    //   0 row_major: (ndc.x, ndc.y, ndc.z, 1) -> rel position (homogeneous)
		Float4 posAdjust;          //  64 xyz: the engine's posAdjust (world = rel + posAdjust)
		Float4 viewport;           //  80 x, y, width, height (render-target pixels)
		Float4 depthRange;         //  96 viewport minDepth, maxDepth, bit mask of the lights reading their own lit plane, pool-floor strength
		Float4 rect;               // 112 water mask bounds minX, minY, maxX, maxY (world)
		Float4 water;              // 128 waterZ, reach above the water, reflection spread (tan), distance fade
		Float4 params;             // 144 frame clock 0..64, ambient (key-light units), debugTint strength (0 = off, -1 = light mask, -2 = direct light), gain x strength x brightness
		Float4 light[kMaxLights];  // 160 world xyz (not mirrored), radius (0 = no falloff)
		Float4 color[kMaxLights];  // 208 rgb colour x level (0 = unused slot); color[0].w = max boost
		Float4 layers;             // 256 calm, normal, rough pattern weights (sum 1); w: 0 = no tides field, else 1 + fine contrast
		Float4 ripple;             // 272 strongest swimmer's world x, y, amplitude 0..1, ring radius
		float  viewProj[16];       // 288 row_major: rel position -> clip (for the reflected path's occlusion test)
		Float4 ambientPos;         // 352 directional ambient (key-light units) on faces toward +x, +y, +z
		Float4 ambientNeg;         // 368 ... toward -x, -y, -z
		Float4 misc;               // 384 x: floor contrast e-fold with depth (world units), y: occlusion test on (1), zw spare
		Float4 ambientHue;         // 400 rgb: the ambient's colour at luminance 1 (the share is taken per channel)
		Float4 falloff[kMaxLights];  // 416 per light: inverse-square (1), sizeBias, fadeZone, radius
		Float4 fade;               // 464 x: the camera distance where the pattern is gone, per pixel (the game's shadow distance; 0 = none), y: ripple contrast (1 = physical), zw spare
	};
	inline constexpr std::size_t kPassConstantsSize = 480;
	static_assert(sizeof(PassConstants) == kPassConstantsSize);
	static_assert(sizeof(PassConstants) % 16 == 0);

	inline constexpr std::size_t kMaxDrawZones = 4;
	inline constexpr float       kCausticGain = 1.0f / 0.1401f;  // 1 / the calm atlas's mean: strength 1 keeps the reflected energy
	inline constexpr float       kMaxBoost = 2.8f;               // output <= 3.8 x the scene: Cycles caustic-only p99.9 under the bridge is +119 % (lamp) / +281 % (sun) (fitted, tools/blender-caustic-stats.py)
	inline constexpr float       kMinAmbient = 0.02f;  // a pitch-black cell: 2 % of the key light, so tiny reflections do not jump to the cap
	inline constexpr float       kMaxDebugTint = 4.0f;      // debugTintStrength range 0.01..4 (output 1 + strength)
	inline constexpr float       kMaxStrength = 50.0f;      // CL_Strength range 0..50 (x the physical reflection)
	inline constexpr float       kFittedStrength = 2.14f;   // the physical match: fitted to the caustic-only Cycles reference (walls hidden from the water's mirror; blender-caustic-stats.py calm, 2026-09-30)
	inline constexpr float       kDefaultStrength = 6.4f;   // chosen, not physics: 3 x kFittedStrength so the pattern shows in game (the user's pick, 2026-09-30, after trying 10); the floor is normalised by it
	inline constexpr std::array<float, 3> kOldDefaultStrengths{ 2.85f, 2.14f, 10.0f };  // earlier ESP defaults: a save that still holds one is rewritten to kDefaultStrength when loaded
	inline constexpr float       kPatternTile = 168.0f;     // world units per pattern tile
	inline constexpr float       kPatternMean = 0.1401f;    // the calm atlas's mean (normal 0.1562, rough 0.1695: each layer is rescaled to it)
	inline constexpr float       kPatternContrast = 1.0f;   // the walls' pattern as it is: the atlas RGB is the reflected light itself (physics)
	// Pool floor under the water: the lamps' light refracted by the rippled surface is redistributed into
	// the pattern (its mean is the light the floor already gets, so the floor brightens on the streaks and
	// darkens a little between them). Water absorbs with depth; deeper than kBottomMaxDepth is ignored.
	inline constexpr float       kBottomMaxDepth = 400.0f;
	inline constexpr float       kBottomAbsorb = 300.0f;    // e-folding depth (world units, ~4.3 m)
	// Contrast grows with depth / focal length of the ripples as lenses: f = n / ((n - 1) curvature),
	// curvature ~ k tan(sigma) for waves of the tile's wavelength -> f = 0.641 tile / tan(sigma).
	// Shallow floors get a soft, weak modulation (blurred pattern), floors near the focus the sharp network.
	inline constexpr float       kBottomContrast = 3.2f;   // x depth / focal length (Cycles fit: tools/blender-floor-compare.py), capped at kBottomMaxContrast
	inline constexpr float       kBottomMaxContrast = 0.5f;
	[[nodiscard]] float BottomFocus(float spread) noexcept;         // focal length (world units) from the reflection spread tan(2 sigma)
	[[nodiscard]] float BottomContrastAt(float depth, float spread) noexcept;
	inline constexpr float       kMinBottomBoost = -0.3f;   // never darker than 0.7 x the scene
	inline constexpr float       kMaxFloorBoost = 1.5f;     // the floor's own cap, as before the walls' refit (kMaxBoost 1.5 then): the floor was not refitted

	// How a point light fades with distance. Vanilla (and Community Shaders' regular lights): 1 - (d / reach)^2 with
	// reach = radius x kRadiusScale. Community Shaders' inverse-square lights (a Light Placer 'InverseSquare' flag,
	// src/Features/InverseSquareLighting): 3920 / (d^2 + sizeBias) x smoothstep((radius - d) x fadeZone), 3920 = 0.8 x 70^2
	// (70 units a metre; 0.8 CS's scale), a sphere light of 'size' metres (physics with CS's fitted 0.8).
	inline constexpr float kRadiusScale = 1.5f;  // assumed (no source found; CS uses radius.x itself): settled by the in-game log
	struct Falloff
	{
		float inverseSquare{ 0.0f };  // 1: CS inverse-square
		float sizeBias{ 0.0f };
		float fadeZone{ 0.0f };
		float radius{ 0.0f };         // the inverse-square light's cut radius
	};
	// CS InverseSquareLighting::ProcessLight: intensity = fade x 4, cutoff (Light Placer ambient.green; 1 = CS default
	// 0.05, 0.022 for a shadow light), size = radius.z.
	[[nodiscard]] Falloff CsInverseSquare(float intensity, float cutoff, float size, bool shadowLight) noexcept;
	// The attenuation at distance d: inverse-square when f says so, else vanilla with this radius (0: none).
	[[nodiscard]] float Attenuation(const Falloff& f, float radius, float d) noexcept;

	struct ExtraLight
	{
		Vec3  pos{ 0.0f, 0.0f, 0.0f };
		float radius{ 0.0f };
		RGB   color{ 1.0f, 1.0f, 1.0f };
		float level{ 0.0f };  // intensity relative to the key light (0 = unused)
		std::uint64_t ref{ 0 };  // the light's key (its lit plane is kept by it); 0 = none
		bool          sun{ false };  // the sun / moon: pos is the direction toward it (no falloff)
		float         beam{ 1.0f };  // the share of it that is reflected (the sun: its parallel beam); its direct light stays whole
		Falloff       falloff{};
	};

	// Per-zone CPU record (task side). Everything the zone logic decides.
	struct ZoneGpuInput
	{
		Rect                                   rect;  // the water mask's bounds when a mask is set
		std::shared_ptr<const WaterMask>       mask;  // nullptr: the pass skips the zone
		bool                                   useLit{ false };  // the mask's lit plane 0 belongs to this key light
		std::array<bool, 2>                    useLitExtra{};    // ... planes 1, 2 to extra lights 0, 1
		std::array<bool, 3>                    planePending{};   // light i should read a lit plane that is not cast yet: it reflects nothing meanwhile
		bool                                   tides{ false };   // a Surface Tides field is bound (t5 / b1): brightness from its curvature
		float                                  tidesSize{ 0.0f };  // the bound field texture's size: its constants must say the same resolution
		float                                  tidesFine{ kTidesFineContrast };  // atlas contrast on top of the field
		float                                  waterZ{ 0.0f };
		float                                  height{ 250.0f };
		float                                  spread{ 0.0f };  // ReflectionSpread(agitation)
		float                                  distFade{ 1.0f };
		Vec3                                   light{ 0.0f, 0.0f, 0.0f };  // light 0 = the key light
		float                                  lightRadius{ 0.0f };
		Falloff                                keyFalloff{};  // the key light's (inverse-square or vanilla)
		bool                                   sun{ false };  // light is the direction toward the sun (any length), not a position
		RGB                                    lightColor{ 1.0f, 1.0f, 1.0f };
		float                                  level{ 0.0f };  // key light 0..1 (fades in; scales the result, not the share)
		std::array<ExtraLight, kMaxLights - 1> extra{};  // lights 1 and 2
		float                                  ambient{ 0.05f };  // light already on a surface the lamps miss, in key-light units (fade x max colour)
		std::array<float, 3>                   ambientPos{};      // + the cell's directional ambient (DALC) on faces toward +x, +y, +z
		std::array<float, 3>                   ambientNeg{};      // ... toward -x, -y, -z
		float                                  absorb{ 300.0f };  // floor contrast e-fold (world units): from the water type's underwater fog
		RGB                                    ambientHue{ 1.0f, 1.0f, 1.0f };  // the ambient's colour (normalised to luminance 1 by PackPass)
		bool                                   occlusion{ true }; // test the reflected path water -> surface against the depth buffer
		float pixelFadeDistance{ 0.0f };  // per pixel: the pattern fades out from 80 % of this camera distance (the game's shadow distance; 0 = off)
		float rippleContrast{ 1.0f };   // the ripple pattern's contrast (CausticLight.ini fRippleContrast; 1 = physical)
		float floorScale{ 1.0f };       // the pool floor pattern x this (CausticLight.ini fFloorStrength)
		float                                  brightness{ 1.0f };
		LayerWeights                           weights;  // calm / normal / rough pattern crossfade
		float                                  frameClock{ 0.0f };
		float                                  rippleX{ 0.0f };  // strongest swimmer (ring)
		float                                  rippleY{ 0.0f };
		float                                  rippleAmp{ 0.0f };
		float                                  rippleRadius{ kRippleRadius };
		bool                                   debugTint{ false };
		float                                  debugTintStrength{ 1.0f };  // debugTint output = 1 + strength (1.0 -> x2)
		bool                                   debugLight{ false };  // light mask: where the reflected light lands (CL_DebugTint 2)
		bool                                   debugDirect{ false };  // how much the lamps light a face directly (CL_DebugTint 3)
		float                                  strength{ 1.0f };     // x the physical reflection (CL_Strength); 1 = physical
	};

	struct FrameCamera
	{
		Mat4     viewProj;
		Mat4     invViewProj;
		Vec3     posAdjust{ 0.0f, 0.0f, 0.0f };
		Viewport viewport;
	};

	[[nodiscard]] PassConstants PackPass(const ZoneGpuInput& zone, const FrameCamera& cam) noexcept;

	// C++ mirror of the pixel shader's zone math, for offline checks. p: world position,
	// n: unit normal facing the camera, pattern: atlas value 0..1 (the same for every light),
	// mask: the zone's water mask (nullptr = the whole bounds are water). Returns the added
	// fraction per channel (the shader outputs 1 + result; the blend multiplies the scene by it).
	struct BoostResult
	{
		RGB  add;
		bool inside{ false };  // passed the zone's height/reach reject
	};
	// The occlusion test of the reflected path from a water point to the lit surface (world positions): 1 = clear.
	// The shader marches the depth buffer (PathVisible); the offline harness supplies the same march.
	struct PathOcclusion
	{
		virtual ~PathOcclusion() = default;
		[[nodiscard]] virtual float Visible(const Vec3& from, const Vec3& to) const noexcept = 0;
	};
	inline constexpr int   kOcclusionSteps = 6;
	inline constexpr float kOcclusionBias = 8.0f;        // a sample must be this much behind the scene to count as blocked
	inline constexpr float kOcclusionThickness = 150.0f;  // ... and less than this (thicker = the scene is behind, not in the way)
	inline constexpr float kOcclusionNear = 20.0f;        // scene closer to the camera than this never blocks (the near plane's clipping; assumed). 300 made every wall near a third-person camera transparent

	[[nodiscard]] BoostResult CausticBoost(const PassConstants& c, const Vec3& p, const Vec3& n, float pattern,
		const WaterMask* mask, const TidesField* tides = nullptr, const PathOcclusion* occlusion = nullptr) noexcept;
}
