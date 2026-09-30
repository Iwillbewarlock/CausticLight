// CausticLight draw pass: one fullscreen triangle per water zone (scissored to the zone), drawn
// after opaque geometry with a multiply blend (dest * src). The pixel shader rebuilds the world
// position from scene depth and brightens the pixel by the light the water reflects onto it, for
// up to three lights, as a fraction of the light the pixel already gets: boost = reflected /
// (direct from the same lights + ambient). A face the lights do not reach directly (a bridge underside)
// is lit by the ambient only, so the same reflection is a much larger share of it (Cycles reference:
// tools/blender-caustic-ref.py). Water is a rippled mirror: the reflection of each light is spread over a
// cone (glossy), so a point gathers light from a disc of water whose radius grows with its
// distance from the water; the water mask says how much of that disc is really water.
//
// Compiled offline by fxc at build time (CMakeLists.txt): VSMain vs_5_0, PSMain ps_5_0.
// The zone math is mirrored in C++ by cl::math::CausticBoost (src/CausticMath.cpp) for the
// offline tests (tests/PassRender.cpp compares the two per pixel on the GPU); keep them in sync.

cbuffer CausticPass : register(b0)
{
	row_major float4x4 InvViewProj;  // (ndc.x, ndc.y, ndc.z, 1) -> position relative to PosAdjust
	float4 PosAdjust;                // xyz
	float4 ViewportRect;             // x, y, width, height (render-target pixels)
	float4 DepthRange;               // viewport minDepth, maxDepth, bit mask (bits 0..2: light i reads its own lit plane; 3..5: light i waits for it and reflects nothing), floor strength
	float4 ZoneRect;                 // water mask bounds minX, minY, maxX, maxY (world)
	float4 Water;                    // waterZ, reach above the water, reflection spread (tan), distance fade
	float4 Params;                   // frame clock 0..64, ambient (in key-light units), debugTint strength (0 = off, -1 = light mask, -2 = direct light), gain x strength x brightness
	float4 Light[3];                 // world xyz (not mirrored), radius (0 = no falloff); w < 0: the sun, xyz = unit vector toward it
	float4 LightColor[3];            // rgb hue x relative strength (key max channel 1; 0 = unused); [0].w = max boost, [1..2].w = its reflected share / the key's level
	float4 Layers;                   // calm, normal, rough pattern weights (sum 1)
	float4 Ripple;                   // strongest swimmer's world x, y, amplitude 0..1, ring radius
	row_major float4x4 ViewProj;     // rel position -> clip (the reflected path's occlusion test)
	float4 AmbientPos;               // the cell's directional ambient (key-light units) on faces toward +x, +y, +z
	float4 AmbientNeg;               // ... toward -x, -y, -z
	float4 Misc;                     // x: floor contrast e-fold with depth, y: occlusion test on, z: the borrowed tides field's size, w: 1 = the reflection atlases are missing (flat walls' pattern)
	float4 AmbientHue;               // rgb: the ambient's colour at luminance 1 (the share is taken per channel)
	float4 LightFalloff[3];          // per light: inverse-square (1), sizeBias, fadeZone, radius (Community Shaders' lights)
	float4 Fade;                     // x: the camera distance where the pattern is gone per pixel (the game's shadow distance; 0 = none), y: ripple contrast (1 = physical)
};

Texture2D<float>  SceneDepth : register(t0);
Texture2D<float4> CalmAtlas : register(t1);
Texture2D<float4> WaterMask : register(t2);  // r: water, g/b/a: water light 0/1/2 reaches (a copy of r when not known)
Texture2D<float4> NormalAtlas : register(t3);
Texture2D<float4> RoughAtlas : register(t4);

// Surface Tides (Nexus 191955), borrowed when bound: its height field and patch (SurfaceField.hlsli layout).
cbuffer TidesPatch : register(b1)
{
	float4 TPatch;      // origin xy, spacing, rest height
	float4 TPrevPatch;
	float4 TOptions;    // resolution, normal strength, displacement strength, maximum tessellation
	float4 TStatus;     // enabled, history valid, edge fade cells, water-height tolerance
	float4 TDebug;
};
Texture2D<float4> TidesField : register(t5);  // x height / displacement strength, w valid
Texture2D<float4> TidesCurv : register(t6);
Texture2D<float4> ReflCalm : register(t7);    // the walls' reflection screens (RGB) per layer: files of their own, so BC7
Texture2D<float4> ReflNormal : register(t8);  // does not share a block with the floor's alpha
Texture2D<float4> ReflRough : register(t9);   // per field cell: hxx, hyy, hxy of the height (CurvaturePS, each frame)
static const float TidesFocusSoftness = 0.15;  // kTidesFocusSoftness
static const float TidesReflectScale = 0.615, TidesStillSpread = 0.1103;  // kTidesReflectScale, kTidesStillSpread
static const float RadiusScale = 1.5;  // kRadiusScale
static const float FloorSpreadFactor = (1 - 1 / 1.33) / 2;                  // kFloorSpreadFactor
static const float SunReflectScale = 1.07;                               // kSunReflectScale
SamplerState      LinearClamp : register(s0);

static const float Atlas = 2048, Cell = 256, Gutter = 4, Content = 248, Frames = 64;
static const float Tile = 168;        // world units per pattern tile
// the walls' pattern: RGB of refl_<layer>.dds = the same water as a mirror, light reflected onto screens 0.5, 1.5 and 4 m
// above it (tools/gen.cjs --mirror, a sea of its own per layer at 3 / 8.5 / 14 deg RMS slope), stored linear over
// the channel's p99.9; these are the stored means (tools/build-all.ps1), so channel / mean is the light (mean 1)
static const float3 ReflMeanCalm = float3(0.4184, 0.0765, 0.0713);
static const float3 ReflMeanNormal = float3(0.0790, 0.0867, 0.1604);
static const float3 ReflMeanRough = float3(0.2758, 0.5436, 0.7001);
static const float3 ReflDist = float3(35, 105, 280);  // those screens in world units (0.5, 1.5, 4 m)
// a lamp's size blurs its caustic on the wall by (wall distance) x (lamp radius / lamp distance); the sun by its
// half angle (physics). The lamp radius is assumed (a torch or candle flame; Cycles used 25 cm for its lamps).
static const float LampRadius = 5, SunHalfAngle = 0.00465;
static const float TexelWorld = 168.0 / 248.0;  // world units per atlas texel (Tile / Content)
static const float MaskSize = 64;     // water mask texels per side
static const float PatternMean = 0.1401;  // the calm atlas alpha's mean: the unit the pattern terms are kept in
static const int   OcclusionSteps = 6;                                   // kOcclusionSteps
static const float OcclusionBias = 8, OcclusionThickness = 150, OcclusionNear = 20;  // kOcclusionBias, kOcclusionThickness, kOcclusionNear
static const float BottomMaxDepth = 400, BottomAbsorb = 300, BottomContrast = 3.2, BottomMaxContrast = 0.5, MinBottomBoost = -0.3, MaxFloorBoost = 1.5;  // pool floor (CausticMath.h; kMaxFloorBoost: the floor's own cap)

// How light i fades at distance d (CausticMath.cpp Attenuation): Community Shaders' inverse square when flagged,
// else vanilla 1 - (d / (radius x RadiusScale))^2 (radius 0: none).
float Atten(uint i, float radius, float d)
{
	float4 f = LightFalloff[i];
	float  a = 1;
	[branch] if (f.x > 0.5) {
		float t = saturate((f.w - d) * f.z);
		a = 3920 / max(d * d + f.y, 1) * t * t * (3 - 2 * t);
	} else if (radius > 0) {
		float e = d / (radius * RadiusScale);
		a = saturate(1 - e * e);
	}
	return a;
}

// Light already on a face with normal n that the tracked lamps miss: the isotropic part plus the cell's
// directional ambient (CausticMath.cpp AmbientAt).
float AmbientAt(float3 n)
{
	return max(Params.y + dot(max(n, 0), AmbientPos.xyz) + dot(max(-n, 0), AmbientNeg.xyz), 0.02);
}

// Is the reflected path from the water point fromW to the lit point toW clear? Samples along it are compared
// with the depth buffer: a sample behind the scene by more than the bias and less than the thickness is blocked.
// Off-screen samples count as clear (CausticMath PathOcclusion; the offline harness runs the same march).
float PathVisible(float3 fromW, float3 toW)
{
	float range = DepthRange.y - DepthRange.x;
	float vis = 1;
	[loop] for (int k = 1; k <= OcclusionSteps; k++) {
		float  f = 0.05 + 0.9 * k / (OcclusionSteps + 1);  // from near the lit point back toward the water, both ends skipped
		float3 xr = lerp(toW, fromW, f) - PosAdjust.xyz;
		float4 clip = mul(float4(xr, 1), ViewProj);
		[branch] if (clip.w <= 1)
			continue;
		float3 nd = clip.xyz / clip.w;
		[branch] if (abs(nd.x) >= 1 || abs(nd.y) >= 1)
			continue;
		float2 pix = ViewportRect.xy + float2(nd.x * 0.5 + 0.5, 0.5 - nd.y * 0.5) * ViewportRect.zw;
		float  d = SceneDepth.Load(int3(pix, 0));
		float4 hs = mul(float4(nd.xy, (d - DepthRange.x) / (abs(range) > 1e-6 ? range : 1), 1), InvViewProj);
		float3 sr = hs.xyz / (abs(hs.w) > 1e-6 ? hs.w : 1e-6);
		float  ds = length(sr), dx = length(xr);
		[branch] if (ds > OcclusionNear && ds < dx - OcclusionBias && ds > dx - OcclusionThickness) {
			vis = 0;
			break;
		}
	}
	return vis;
}

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
	// (-1,-1), (3,-1), (-1,3): one triangle covering the whole viewport
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, 2) - 1, 0, 1);
}

float CellSample(Texture2D<float4> tex, float2 q, float frame, float lod)
{
	float2 cell = float2(fmod(frame, 8), floor(frame / 8));
	float2 uv = (cell * Cell + Gutter + frac(q) * Content) / Atlas;
	return tex.SampleLevel(LinearClamp, uv, lod).w;
}

float4 CellSample4(Texture2D<float4> tex, float2 q, float frame, float lod)
{
	float2 cell = float2(fmod(frame, 8), floor(frame / 8));
	float2 uv = (cell * Cell + Gutter + frac(q) * Content) / Atlas;
	return tex.SampleLevel(LinearClamp, uv, lod);
}

// How the three reflection screens mix for a wall this far from the water (along the reflected ray; a lamp's
// diverging light counts as its equivalent parallel-light distance): linear in log distance between the screens;
// nearer than the first the pattern fades to flat (at the water itself the light is not yet bunched: w = flat).
float4 ReflWeights(float d)
{
	float4 wt = 0;
	[flatten] if (d <= ReflDist.x) {
		float t = saturate(d / ReflDist.x);
		wt = float4(t, 0, 0, 1 - t);
	} else if (d <= ReflDist.y) {
		float t = saturate(log2(d / ReflDist.x) / log2(ReflDist.y / ReflDist.x));
		wt = float4(1 - t, t, 0, 0);
	} else {
		float t = saturate(log2(d / ReflDist.y) / log2(ReflDist.z / ReflDist.y));
		wt = float4(0, 1 - t, t, 0);
	}
	return wt;
}

// Mixing tabulated patterns keeps the spread of one sea: a plain average of two unrelated patterns halves their
// bright streaks against each other (the real water is one pattern). The deviations from the mean are summed with
// the weights and divided by the weights' root sum of squares (variance-preserving blend; engineering, not optics).
// Returns the deviation from the mean (light - 1) of one layer at the wall's distance; the flat share (nearer than
// the first screen) scales it down: that part is physical (the light is not yet bunched near the water).
float ReflDeviation(Texture2D<float4> tex, float2 q, float frame, float lod, float4 wt, float3 mean)
{
	float  f0 = floor(frame);
	float4 s = lerp(CellSample4(tex, q, f0, lod), CellSample4(tex, q, fmod(f0 + 1, Frames), lod), frame - f0);
	float  norm = sqrt(max(dot(wt.xyz, wt.xyz), 1e-6));
	return dot(wt.xyz, s.xyz / mean - 1) / norm * (1 - wt.w);
}

// The walls' pattern as light (mean 1): the three layers mixed by the agitation, each at the wall's distance.
float ReflMix(float2 q, float frame, float lod, float dist)
{
	float dev = 0;
	[branch] if (Misc.w < 0.5) {  // else no reflection atlases: the mean light, no pattern (never a black wall)
		float4 wt = ReflWeights(dist);
		[branch] if (Layers.x > 0.01) dev += Layers.x * ReflDeviation(ReflCalm, q, frame, lod, wt, ReflMeanCalm);
		[branch] if (Layers.y > 0.01) dev += Layers.y * ReflDeviation(ReflNormal, q, frame, lod, wt, ReflMeanNormal);
		[branch] if (Layers.z > 0.01) dev += Layers.z * ReflDeviation(ReflRough, q, frame, lod, wt, ReflMeanRough);
	}
	return max(1 + dev / sqrt(max(dot(Layers.xyz, Layers.xyz), 1e-6)), 0);
}

float Pattern(Texture2D<float4> tex, float2 q, float frame, float lod)
{
	float f0 = floor(frame);
	return lerp(CellSample(tex, q, f0, lod), CellSample(tex, q, fmod(f0 + 1, Frames), lod), frame - f0);
}

// Edge fade of the tides field at grid position g (CausticMath.cpp TidesHeight).
float TidesFade(float2 g)
{
	float n = TOptions.x;
	float edge = min(min(g.x, g.y), min(n - 1 - g.x, n - 1 - g.y));
	float e = saturate(edge / max(1, TStatus.z));
	return (edge >= 0 ? 1 : 0) * e * e * (3 - 2 * e) * TStatus.x;
}

// Height x valid x edge fade of field cell c (c may lie outside: its fade is then 0).
float TidesNodeHeight(int2 c)
{
	int2   ic = clamp(c, 0, int(TOptions.x) - 1);
	float4 v = TidesField.Load(int3(ic, 0));
	return v.x * TOptions.z * v.w * TidesFade(float2(c));
}

// One texel per field cell: the height's second derivatives there (drawn once a frame into TidesCurv, so the
// main pass reads one filtered texel instead of a 4 x 4 block per light).
float4 CurvaturePS(float4 pos : SV_Position) : SV_Target
{
	int2  c = int2(pos.xy);
	float s = TPatch.z;
	float hc = TidesNodeHeight(c);
	float hxx = (TidesNodeHeight(c + int2(1, 0)) - 2 * hc + TidesNodeHeight(c - int2(1, 0))) / (s * s);
	float hyy = (TidesNodeHeight(c + int2(0, 1)) - 2 * hc + TidesNodeHeight(c - int2(0, 1))) / (s * s);
	float hxy = (TidesNodeHeight(c + int2(1, 1)) - TidesNodeHeight(c + int2(1, -1)) - TidesNodeHeight(c + int2(-1, 1)) + TidesNodeHeight(c + int2(-1, -1))) /
	            (4 * s * s);
	return float4(hxx, hyy, hxy, 0);
}

// How the real surface around xy bunches light: 1 / det(I + k H) (softened at the focus), 1 = flat.
// k = -2 L for a reflection L from the water (troughs focus), (1 - 1/n) d for a floor d under it (crests focus).
// The 3 x 3 taps one cell apart share one 4 x 4 block of cells, read exactly (Load), as the C++ mirror.
float TidesIntensity(float2 xy, float k1, float k2, float2 e1)
{
	float2 g = (xy - TPatch.xy) / TPatch.z;
	float4 Hn = TidesCurv.SampleLevel(LinearClamp, (g + 0.5) / TOptions.x, 0);  // bilinear of the cells' curvature
	float  hxx = Hn.x, hyy = Hn.y, hxy = Hn.z;
	// the curvature in the light's frame: e1 along its horizontal direction, e2 across (CausticMath.cpp)
	float  el = length(e1);
	float2 cxy = el > 1e-6 ? e1 / el : float2(1, 0);
	float  h11 = cxy.x * cxy.x * hxx + 2 * cxy.x * cxy.y * hxy + cxy.y * cxy.y * hyy;
	float  h22 = cxy.y * cxy.y * hxx - 2 * cxy.x * cxy.y * hxy + cxy.x * cxy.x * hyy;
	float  h12 = (cxy.y * cxy.x) * (hyy - hxx) + (cxy.x * cxy.x - cxy.y * cxy.y) * hxy;
	float  det = (1 + k1 * h11) * (1 + k2 * h22) - k1 * k2 * h12 * h12;
	float e = TidesFocusSoftness;
	float i = sqrt(1 + e * e) / sqrt(det * det + e * e);
	return isfinite(i) ? i : 1;
}

// The field applies to this zone: bound, at this water level (CausticMath.cpp TidesApply).
bool TidesHere()
{
	// the constants and the texture are captured separately: they must describe the same field (Misc.z = its size)
	return Layers.w > 0.5 && abs(Water.x - TPatch.w) < TStatus.w && abs(TOptions.x - Misc.z) < 0.5 && TPatch.z > 0;
}

// How much of the field covers w (its edge fade): 0 outside the patch.
float TidesCover(float2 w)
{
	return TidesHere() ? TidesFade((w - TPatch.xy) / TPatch.z) : 0;
}

// The pattern term in calm-atlas units (mean PatternMean): the tides field's curvature (plus the atlas for finer
// ripples) where it covers w, blended into the atlas alone at 'contrast' towards the patch edge (PatternTerm).
float PatternTerm(float2 w, float k1, float k2, float2 e1, float atlasValue, float contrast)
{
	float k = Fade.y;  // the player's ripple contrast (CausticLight.ini fRippleContrast; 1 = physical)
	float atlas = atlasValue / PatternMean;
	float result = PatternMean * max(1 + k * contrast * (atlas - 1), 0);
	float cov = TidesCover(w);
	[branch] if (cov > 0)
		result = lerp(result, PatternMean * max(1 + k * (TidesIntensity(w, k1, k2, e1) - 1), 0) * max(1 + k * (Layers.w - 1) * (atlas - 1), 0), cov);
	return result;
}

// The three atlases crossfaded by the agitation, each rescaled to the calm atlas's mean.
float PatternMix(float2 q, float frame, float lod)
{
	float a = 0;
	[branch] if (Layers.x > 0.01) a += Layers.x * Pattern(CalmAtlas, q, frame, lod);
	[branch] if (Layers.y > 0.01) a += Layers.y * Pattern(NormalAtlas, q, frame, lod) * (0.1401 / 0.1562);
	[branch] if (Layers.z > 0.01) a += Layers.z * Pattern(RoughAtlas, q, frame, lod) * (0.1401 / 0.1695);
	return a;
}

// Pool floor under the water (CausticMath.cpp FloorBoost mirrors this): the lamps' light enters the rippled
// surface, is refracted and bunched into the pattern. Its mean is light the floor already gets, so the
// pattern is used around 1: brighter on the streaks, a little darker between them.
// As the shadows: 1 up to 80 % of the game's shadow distance, then smoothly to 0 at it (CausticMath DistanceFade).
float PixelFade(float d)
{
	float D = Fade.x;
	float t = D > 0 ? saturate((d - 0.8 * D) / max(0.2 * D, 1e-3)) : 0;
	return 1 - t * t * (3 - 2 * t);
}

float4 FloorPass(float3 p, float3 n, float3 rel)
{
	float  waterZ = Water.x;
	float  depthW = waterZ - p.z;
	float2 size = ZoneRect.zw - ZoneRect.xy;
	float2 pu = float2(size.x > 1e-3 ? (p.x - ZoneRect.x) / size.x : 0, size.y > 1e-3 ? (p.y - ZoneRect.y) / size.y : 0);
	float4 result = float4(1, 1, 1, 1);
	[branch] if (WaterMask.SampleLevel(LinearClamp, saturate(pu), 0).r > 0.002) {  // water over this floor (above the GPU's 8-bit filter weights)
	bool  maskMode = Params.z < 0 && Params.z > -1.5;
	bool  directMode = Params.z < -1.5;
	float fade = saturate((depthW - 2) / 18) * exp(-depthW / (Misc.x > 0 ? Misc.x : BottomAbsorb)) * Water.w;
	float camLod = log2(max(length(rel), 1) / 250);
	// the ripples as lenses: depth / focal length sets how sharp and how strong the floor pattern is
	float focus = 0.641 * Tile / max(tan(0.5 * atan(max(Water.z, 0))), 1e-3);

	float3 c = 0;
	float3 direct = 0;  // per channel: the share is reflected_c / (direct_c + ambient_c)
	float  mk = 0;
	[loop] for (uint i = 0; i < 3; i++) {
		float3 col = LightColor[i].rgb;
		[branch] if (!(max(col.r, max(col.g, col.b)) > 0))
			continue;
		float4 L = Light[i];
		bool   sun = L.w < 0;  // the sun: L.xyz is the unit vector toward it, no falloff
		float3 toL = sun ? L.xyz : L.xyz - p;
		float  dl = sun ? 1 : length(toL);
		float  ad = sun ? 1 : Atten(i, L.w, dl);
		float nd = saturate(dot(n, toL / max(dl, 1e-3)));
		direct += col * (ad * nd);
		[branch] if (directMode || (sun ? L.z <= 1e-3 : L.z <= waterZ + 1e-3) || !(ad * nd > 0))
			continue;

		static const float eta = 1 / 1.33;
		float2 w = p.xy;
		float  ci = 1;
		float2 e1 = 0;  // the light's horizontal travel direction
		[branch] if (sun) {
			float3 d = -L.xyz;  // parallel rays: one refraction, no search
			ci = saturate(-d.z);
			float k2 = max(1 - eta * eta * (1 - ci * ci), 0);
			float tz = eta * d.z + (eta * ci - sqrt(k2));
			w = p.xy - eta * d.xy * (depthW / max(-tz, 1e-3));
			e1 = d.xy;
		} else {
			[unroll] for (int it = 0; it < 2; it++) {
				float3 d = normalize(float3(w, waterZ) - L.xyz);
				ci = saturate(-d.z);
				float k2 = max(1 - eta * eta * (1 - ci * ci), 0);
				float tz = eta * d.z + (eta * ci - sqrt(k2));
				w = p.xy - eta * d.xy * (depthW / max(-tz, 1e-3));
			}
			e1 = w - L.xy;
		}
		float m = saturate(1 - ci);
		float tr = 1 - (0.02 + 0.98 * (m * m) * (m * m) * m);
		float2 wu = float2(size.x > 1e-3 ? (w.x - ZoneRect.x) / size.x : 0, size.y > 1e-3 ? (w.y - ZoneRect.y) / size.y : 0);
		bool   ownPlane = (((uint)(DepthRange.z + 0.5)) >> i) & 1;
		float  texelF = min(size.x, size.y) / MaskSize;
		// the lit plane over the patch the refracted light comes through (depth x tan 2 sigma x FloorSpreadFactor),
		// at least a texel: no square shadows
		float4 planes = WaterMask.SampleLevel(LinearClamp, saturate(wu), ownPlane ? log2(max(max(depthW, 0) * max(Water.z, 0) * FloorSpreadFactor / max(texelF, 1e-3), 2)) : 0);
		float  cover = ownPlane ? planes[1 + i] : planes.r;
		[flatten] if ((((uint)(DepthRange.z + 0.5)) >> (3 + i)) & 1)
			cover = 0;  // its lit plane is not cast yet
		[branch] if (maskMode) {
			mk = max(mk, cover * tr * ad * nd * fade);
			continue;
		}
		// a lamp's light arrives diverging: under the water it seems to come from n D above the surface, so the ripples
		// focus it as if the floor were d n D / (d + n D) deep (thin lens, Snell's apparent depth; the sun: d)
		// (Coddington for a flat surface: n D across the plane of incidence, n D cos^2 t / cos^2 i within it)
		float ctA = sqrt(max(1 - (1 / 1.33) * (1 / 1.33) * (1 - ci * ci), 1e-4));
		float nD = 1.33 * length(float3(w, waterZ) - L.xyz);
		float nDt = nD * ctA * ctA / max(ci * ci, 0.0025);
		// the coupling runs along the refracted ray (s = d / cos t): s nD / (s + nD), as a vertical depth x cos t
		float dEff = sun ? depthW : depthW * nD * ctA / max(depthW + nD * ctA, 1e-3);     // sagittal
		float dEffT = sun ? depthW : depthW * nDt * ctA / max(depthW + nDt * ctA, 1e-3);  // tangential
		float xl = saturate(dEff / focus);
		float contrastL = min(BottomContrast * xl, BottomMaxContrast);
		float lod = clamp(camLod + 4 * (1 - xl), 0, 5);  // far before the focus: only the broad shape of the pattern
		float ct = sqrt(max(1 - (1 / 1.33) * (1 / 1.33) * (1 - ci * ci), 1e-4));  // oblique refraction
		float qo = 1 - ci / (1.33 * ct);
		float term = PatternTerm(w, dEffT * qo / (ct * ct), dEff * qo, e1, saturate(PatternMix(w / Tile, Params.x, lod)), contrastL);
		c += col * (tr * ad * nd * cover * (term / PatternMean - 1) * (i == 0 ? 1 : LightColor[i].w));
	}
	c *= fade * DepthRange.w * PixelFade(length(rel)) / (direct + AmbientAt(n) * AmbientHue.rgb);
	[branch] if (maskMode)
		result = float4(1 + float3(0.25, 0.6, 1.0) * saturate(mk), 1);
	else if (directMode)
		result = float4(1 + float3(1.0, 0.6, 0.25) * saturate(dot(direct, float3(0.2126, 0.7152, 0.0722))), 1);
	else if (all(isfinite(c)))
		result = float4(1 + clamp(c, MinBottomBoost, clamp(LightColor[0].w, 0, MaxFloorBoost)), 1);
	}
	return result;
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
	// Position and normal first: ddx/ddy are only defined before any divergent branch.
	float  depth = SceneDepth.Load(int3(pos.xy, 0));
	float  range = DepthRange.y - DepthRange.x;
	float3 ndc = float3((pos.x - ViewportRect.x) / ViewportRect.z * 2 - 1,
		1 - (pos.y - ViewportRect.y) / ViewportRect.w * 2,
		(depth - DepthRange.x) / (abs(range) > 1e-6 ? range : 1));
	float4 h = mul(float4(ndc, 1), InvViewProj);
	float  hw = abs(h.w) > 1e-6 ? h.w : 1e-6;
	float3 rel = h.xyz / hw;           // relative to PosAdjust (the camera)
	float3 p = rel + PosAdjust.xyz;    // world

	float3 n = cross(ddy(rel), ddx(rel));
	float  nl = length(n);
	n = nl > 1e-6 ? n / nl : float3(0, 0, 1);
	n = dot(n, -rel) < 0 ? -n : n;     // face the camera

	float waterZ = Water.x;
	float reach = Water.y;
	float hgt = p.z - waterZ;          // height above the water
	// Any NaN / inf (bad depth, degenerate derivatives) leaves the pixel unchanged. fxc runs with /Gis
	// (IEEE strictness) so these checks are not optimised away.
	[branch] if (!all(isfinite(p)) || !all(isfinite(n)) || abs(h.w) <= 1e-6 || hgt > reach || hgt < -BottomMaxDepth || p.x < ZoneRect.x - reach ||
				 p.x > ZoneRect.z + reach || p.y < ZoneRect.y - reach || p.y > ZoneRect.w + reach)
		return float4(1, 1, 1, 1);
	[branch] if (hgt < 2) {
		[branch] if (hgt > -2 || Params.z > 0)
			return float4(1, 1, 1, 1);  // at the waterline, or the flat tint (above the water only)
		return FloorPass(p, n, rel);
	}

	[branch] if (Params.z > 0)
		return float4(1 + Params.zzz, 1);  // debug tint: the zone's whole band, light and pattern ignored

	bool   maskMode = Params.z < 0 && Params.z > -1.5;
	bool   directMode = Params.z < -1.5;
	float2 size = ZoneRect.zw - ZoneRect.xy;
	float  texel = min(size.x, size.y) / MaskSize;
	float  s = saturate((hgt - 2) / 28);
	float  heightFade = s * s * (3 - 2 * s) * saturate((reach - hgt) / (0.2 * reach)) * Water.w;  // soft only in the last 20 % of the reach
	float  camLod = log2(max(length(rel), 1) / 250);

	float3 c = 0;       // reflected light
	float3 direct = 0;  // direct light from the same lamps (no shadows known), per channel
	float  mk = 0;
	[loop] for (uint i = 0; i < 3; i++) {
		float3 col = LightColor[i].rgb;
		[branch] if (!(max(col.r, max(col.g, col.b)) > 0))
			continue;  // unused slot

		float4 L = Light[i];
		bool   sun = L.w < 0;  // the sun: L.xyz is the unit vector toward it, no falloff
		{
			float3 toL = sun ? L.xyz : L.xyz - p;
			float  dl = sun ? 1 : length(toL);
			float  ad = sun ? 1 : Atten(i, L.w, dl);
			direct += col * (ad * saturate(dot(n, toL / max(dl, 1e-3))));
		}
		[branch] if (directMode)
			continue;
		[branch] if (sun ? L.z <= 1e-3 : L.z <= waterZ + 1e-3)
			continue;  // a light at or below the water (the sun below the horizon) reflects nothing upward
		float3 dir, toP;
		float  len, atten = 1;
		float2 w;
		[branch] if (sun) {
			dir = float3(-L.x, -L.y, L.z);           // parallel rays reflected off the plane
			toP = dir * 1e6;                          // the sun's image is infinitely far
			len = 1e6;
			w = p.xy - dir.xy * (hgt / max(dir.z, 1e-3));
		} else {
			float3 lm = float3(L.xy, 2 * waterZ - L.z);  // the light's mirror image below the water
			toP = p - lm;
			len = length(toP);
			atten = Atten(i, L.w, len);
			dir = toP / max(len, 1e-3);  // the reflected light travels from lm up to p
			float t = (waterZ - lm.z) / (p.z - lm.z);
			w = lm.xy + toP.xy * t;       // where the mirror ray meets the water
		}
		[branch] if (dir.z <= 1e-3 || atten <= 0)
			continue;

		float  tidesCov = TidesCover(w);  // the field's waves replace the statistical spread and the rings
		float  spreadR = lerp(Water.z, TidesStillSpread, tidesCov) * length(p - float3(w, waterZ));
		float  lodM = log2(max(spreadR / max(texel, 1e-3), 1));
		float2 uv = float2(size.x > 1e-3 ? (w.x - ZoneRect.x) / size.x : 0, size.y > 1e-3 ? (w.y - ZoneRect.y) / size.y : 0);
		float4 planes = WaterMask.SampleLevel(LinearClamp, saturate(uv), lodM);
		float  cover = ((((uint)(DepthRange.z + 0.5)) >> i) & 1) ? planes[1 + i] : planes.r;  // a light with a lit plane: only the water it reaches
		[flatten] if ((((uint)(DepthRange.z + 0.5)) >> (3 + i)) & 1)
			cover = 0;  // its lit plane is not cast yet
		// outside the mask the clamped sample is an edge average: fade it out over the spread radius
		float2 out2 = max(max(ZoneRect.xy - w, w - ZoneRect.zw), 0);
		cover *= saturate(1 - length(out2) / max(spreadR, texel));
		float  ndl = saturate(dot(n, -dir));
		[branch] if (cover <= 0 || ndl <= 0 || heightFade <= 0)
			continue;
		[branch] if (Misc.y > 0.5)
			cover *= PathVisible(float3(w, waterZ), p);  // something between the water and this face blocks it
		[branch] if (cover <= 0)
			continue;

		[branch] if (maskMode) {
			mk = max(mk, atten * ndl * cover * heightFade);
			continue;
		}

		float m = saturate(1 - dir.z);
		float geo = atten * ndl * cover * heightFade;
		[branch] if (geo * Params.w * max(col.r, max(col.g, col.b)) < 1e-4)
			continue;  // too faint to matter: skip the two atlas samples
		float fres = 0.02 + 0.98 * (m * m) * (m * m) * m;  // Schlick, water F0 0.02

		// rings around the strongest swimmer: the pattern is pushed outward and brightens on the wave crests
		float2 rip = w - Ripple.xy;
		float  rr = length(rip);
		float  ramp = saturate(Ripple.z) * (1 - tidesCov) * exp(-rr / max(Ripple.w, 1));
		float  wave = sin(6.2831853 * (rr / 28 - Params.x / 8 * 1.6));
		float2 q = w / Tile + rip / max(rr, 1e-3) * (wave * ramp * 0.05);

		float lwp = length(p - float3(w, waterZ)), cz = max(dir.z, 0.1);  // oblique reflection
		float dlw = max(len - lwp, 1);                                  // the lamp's image to the water point (the sun: infinite)
		float leff = sun ? lwp : lwp * dlw / (lwp + dlw);               // a diverging beam focuses farther away
		// the pattern is the ripples' own effect (no extra blur for their slope): only the light's size softens it
		float blurR = lwp * (sun ? SunHalfAngle : LampRadius / dlw);
		float lod = clamp(camLod + log2(1 + blurR / TexelWorld), 0, 5);
		float a = PatternTerm(w, -2 * leff / cz, -2 * leff * cz, toP.xy, PatternMean * ReflMix(q, Params.x, lod, leff), 1);

		c += col * (atten * fres * ndl * a * Params.w * (i == 0 ? 1 : LightColor[i].w) * lerp(1, TidesReflectScale, tidesCov) * (sun ? SunReflectScale : 1) * cover * heightFade * (1 + 0.6 * ramp * wave));
	}

	[branch] if (maskMode)
		return float4(1 + float3(0.25, 0.6, 1.0) * saturate(mk), 1);  // the geometric share as it is (no Fresnel, no pattern)
	[branch] if (directMode)
		return float4(1 + float3(1.0, 0.6, 0.25) * saturate(dot(direct, float3(0.2126, 0.7152, 0.0722))), 1);  // orange: how much the lamps light this face directly

	c /= direct + AmbientAt(n) * AmbientHue.rgb;  // as a share of the light already there, per channel
	[branch] if (!all(isfinite(c)))
		return float4(1, 1, 1, 1);
	c *= PixelFade(length(rel));  // past the shadow distance, as the shadows
	c = clamp(c, 0, max(LightColor[0].w, 0));  // never darker than the scene, never above the cap
	return float4(1 + c, 1);
}
