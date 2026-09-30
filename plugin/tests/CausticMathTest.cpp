// Offline checks for the CausticLight decision math. No game needed.
// Exit code 0 = all pass.

#include <algorithm>
#include "CausticMath.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace cl::math;

namespace
{
	int g_failures = 0;
	int g_checks = 0;

	void Check(bool ok, const char* what, double got = 0.0, double want = 0.0)
	{
		++g_checks;
		if (!ok) {
			++g_failures;
			std::printf("  FAIL: %s (got %.6f, want %.6f)\n", what, got, want);
		}
	}

	bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

	void CheckWeights(float a, float c, float n, float r, const char* label)
	{
		const auto w = CrossfadeWeights(a);
		char       buf[128];
		std::snprintf(buf, sizeof(buf), "%s calm", label);
		Check(Near(w.calm, c), buf, w.calm, c);
		std::snprintf(buf, sizeof(buf), "%s normal", label);
		Check(Near(w.normal, n), buf, w.normal, n);
		std::snprintf(buf, sizeof(buf), "%s rough", label);
		Check(Near(w.rough, r), buf, w.rough, r);
	}
}

int main()
{
	// --- crossfade weights ------------------------------------------------------------
	std::printf("[weights]\n");
	CheckWeights(0.0f, 1.0f, 0.0f, 0.0f, "a=0");
	CheckWeights(0.25f, 0.5f, 0.5f, 0.0f, "a=0.25");
	CheckWeights(0.5f, 0.0f, 1.0f, 0.0f, "a=0.5");
	CheckWeights(0.75f, 0.0f, 0.5f, 0.5f, "a=0.75");
	CheckWeights(1.0f, 0.0f, 0.0f, 1.0f, "a=1");
	CheckWeights(-3.0f, 1.0f, 0.0f, 0.0f, "a<0 clamps");
	CheckWeights(7.0f, 0.0f, 0.0f, 1.0f, "a>1 clamps");
	CheckWeights(NAN, 1.0f, 0.0f, 0.0f, "a=NaN -> calm");

	float maxSumErr = 0.0f;
	bool  allNonNeg = true;
	bool  continuous = true;
	auto  prev = CrossfadeWeights(0.0f);
	for (int i = 0; i <= 10000; ++i) {
		const float a = static_cast<float>(i) / 10000.0f;
		const auto  w = CrossfadeWeights(a);
		maxSumErr = std::fmax(maxSumErr, std::fabs(w.calm + w.normal + w.rough - 1.0f));
		allNonNeg = allNonNeg && w.calm >= 0.0f && w.normal >= 0.0f && w.rough >= 0.0f;
		// no layer may jump between neighbouring agitation values (step 1e-4 -> <= 2e-4 change)
		continuous = continuous && std::fabs(w.calm - prev.calm) <= 3e-4f &&
		             std::fabs(w.normal - prev.normal) <= 3e-4f && std::fabs(w.rough - prev.rough) <= 3e-4f;
		prev = w;
	}
	Check(maxSumErr < 1e-5f, "weights sum to 1 over [0,1]", maxSumErr, 0.0);
	Check(allNonNeg, "weights never negative");
	Check(continuous, "weights continuous (no jump at a=0.5)");

	// --- distance fade ----------------------------------------------------------------
	std::printf("[distance]\n");
	{
		Check(Near(DistanceFade(0.0f, 3000.0f), 1.0f), "near -> 1");
		Check(Near(DistanceFade(2400.0f, 3000.0f), 1.0f), "80% of max -> 1");
		Check(Near(DistanceFade(3000.0f, 3000.0f), 0.0f), "at max -> 0");
		Check(Near(DistanceFade(9000.0f, 3000.0f), 0.0f), "beyond max -> 0");
		Check(DistanceFade(2700.0f, 3000.0f) > 0.0f && DistanceFade(2700.0f, 3000.0f) < 1.0f, "fade band in between");
		Check(Near(DistanceFade(1e6f, 0.0f), 1.0f), "maxDistance 0 disables the fade");
		bool mono = true;
		for (int i = 1; i <= 300; ++i) {
			mono = mono && DistanceFade(i * 10.0f, 3000.0f) <= DistanceFade((i - 1) * 10.0f, 3000.0f);
		}
		Check(mono, "distance fade never rises with distance");
	}

	// --- playback speed ---------------------------------------------------------------
	std::printf("[speed]\n");
	Check(Near(PlaybackSpeed(0.0f), 0.35f), "speed(0) = 0.35", PlaybackSpeed(0.0f), 0.35);
	Check(Near(PlaybackSpeed(0.5f), 1.3f), "speed(0.5) = 1.3", PlaybackSpeed(0.5f), 1.3);
	Check(Near(PlaybackSpeed(1.0f), 2.25f), "speed(1) = 2.25", PlaybackSpeed(1.0f), 2.25);
	Check(Near(PlaybackSpeed(5.0f), 2.25f), "speed clamps above", PlaybackSpeed(5.0f), 2.25);
	bool mono = true;
	for (int i = 1; i <= 100; ++i) {
		mono = mono && PlaybackSpeed(i / 100.0f) > PlaybackSpeed((i - 1) / 100.0f);
	}
	Check(mono, "speed strictly increasing");

	// --- smoothing: no jumps, converges -------------------------------------------------
	std::printf("[smoothing]\n");
	{
		float       s = PlaybackSpeed(0.0f);
		const float target = PlaybackSpeed(1.0f);
		float       maxStep = 0.0f;
		for (int i = 0; i < 100; ++i) {  // 10 s at 10 Hz, tau 0.8 s
			const float next = SmoothToward(s, target, 0.1f, 0.8f);
			maxStep = std::fmax(maxStep, std::fabs(next - s));
			s = next;
		}
		Check(maxStep < 0.25f, "one 0.1 s tick moves speed < 0.25 (0.35 -> 2.25 step)", maxStep, 0.25);
		Check(Near(s, target, 1e-3f), "smoothing converges", s, target);
		Check(Near(SmoothToward(1.0f, 2.0f, 0.0f, 0.8f), 1.0f), "dt=0 keeps value");
	}

	// --- agitation inputs ---------------------------------------------------------------
	std::printf("[agitation]\n");
	{
		const WeatherSample calm{ 0.0f, false, false, false };
		const WeatherSample storm{ 1.0f, true, false, true };
		Check(Near(WeatherAgitation(calm), 0.0f), "still clear weather -> 0");
		Check(WeatherAgitation(storm) > 0.9f, "windy rain -> near 1", WeatherAgitation(storm), 0.9);
		Check(Near(BlendedWeatherAgitation(calm, storm, 0.0f), WeatherAgitation(calm)), "transition 0 = last weather");
		Check(Near(BlendedWeatherAgitation(calm, storm, 1.0f), WeatherAgitation(storm)), "transition 1 = current weather");

		Check(Near(ActorImpulse(0.0f, 500.0f, 10.0f, 600.0f), 0.0f), "dry actor -> no impulse");
		Check(Near(ActorImpulse(1.0f, 500.0f, 600.0f, 600.0f), 0.0f), "actor at range edge -> no impulse");
		Check(ActorImpulse(0.5f, 300.0f, 0.0f, 600.0f) > ActorImpulse(0.5f, 0.0f, 0.0f, 600.0f), "moving splashes more than standing");
		Check(ActorImpulse(0.5f, 300.0f, 100.0f, 600.0f) > ActorImpulse(0.5f, 300.0f, 400.0f, 600.0f), "closer splashes more");

		float imp = 0.75f;
		for (int i = 0; i < 50; ++i) {
			imp = DecayImpulse(imp, 0.1f, 1.5f);  // 5 s
		}
		Check(imp < 0.03f, "impulse decays within a few seconds", imp, 0.03);
		Check(Near(TotalAgitation(0.2f, 0.9f, 0.5f), 1.0f), "total agitation clamps to 1");
		Check(Near(TotalAgitation(0.2f, 0.0f, 0.0f), 0.2f), "interior: base only");
	}

	// --- light inputs -------------------------------------------------------------------
	std::printf("[light]\n");
	{
		Check(Near(SunLevel(12.0f), 1.0f), "noon sun = 1");
		Check(Near(SunLevel(0.0f), 0.0f), "midnight sun = 0");
		Check(DaylightLevel(0.0f) > 0.0f && DaylightLevel(0.0f) < 0.3f, "night has dim moonlight", DaylightLevel(0.0f), 0.15);
		for (float h : { 0.0f, 5.0f, 6.5f, 8.0f, 12.0f, 17.0f, 18.5f, 20.0f, 23.9f, 30.0f, -2.0f }) {
			const auto  w = TimeOfDayWeights(h);
			const float s = w[0] + w[1] + w[2] + w[3];
			Check(Near(s, 1.0f), "time-of-day weights sum to 1", s, 1.0);
		}
		Check(WeatherDimming({ 0.2f, true, false, true }) < WeatherDimming({ 0.2f, false, false, false }), "rain dims the sky");
		Check(Near(DirectBeamFraction({ 0.2f, false, false, false }), 1.0f), "clear sky: the whole sunlight is a beam");
		{
			const EnbSchedule s;  // dawn 5, sunrise 6, day 13, sunset 20, dusk 21, night 1
			const std::array<float, 6> v{ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
			Check(Near(EnbExteriorBlend(v, s, 6.0f), 2.0f) && Near(EnbExteriorBlend(v, s, 13.0f), 3.0f) && Near(EnbExteriorBlend(v, s, 21.0f), 5.0f) &&
					  Near(EnbExteriorBlend(v, s, 1.0f), 6.0f),
				"ENB blend: each point's own value at its time");
			Check(Near(EnbExteriorBlend(v, s, 9.5f), 2.5f) && Near(EnbExteriorBlend(v, s, 23.0f), 5.5f) && Near(EnbExteriorBlend(v, s, 3.0f), 3.5f),
				"ENB blend: linear between points, across midnight too");
			Check(Near(EnbDayWeight(s, 13.0f), 1.0f) && Near(EnbDayWeight(s, 2.0f), 0.0f) && Near(EnbDayWeight(s, 5.5f), 0.5f), "ENB interior day weight");
		}
		Check(Near(DirectBeamFraction({ 0.2f, true, false, true }), 0.0f) && Near(DirectBeamFraction({ 0.2f, false, true, false }), 0.0f),
			"rain or snow: a closed cloud deck, no beam (no caustic pattern)");
		Check(DirectBeamFraction({ 0.2f, false, false, true }) > 0.0f && DirectBeamFraction({ 0.2f, false, false, true }) < 1.0f, "cloudy: part of the beam");

		Check(Near(LightContribution(0.0f, 300.0f, 1.0f), 1.0f), "light on zone = full fade");
		Check(Near(LightContribution(450.0f, 300.0f, 1.0f), 0.0f), "light at 1.5*radius = 0");
		Check(LightContribution(100.0f, 300.0f, 1.0f) > LightContribution(300.0f, 300.0f, 1.0f), "nearer light contributes more");

		const LightResult none = CombineLights({});
		Check(Near(none.level, 0.0f), "no light nearby -> level 0 (caustic fades out)");

		std::vector<LightSample> ls{
			{ { 1.0f, 0.5f, 0.0f }, 50.0f, 400.0f, 1.0f },     // orange, close
			{ { 0.0f, 0.0f, 1.0f }, 5000.0f, 400.0f, 1.0f },  // blue, out of reach
		};
		const LightResult one = CombineLights(ls);
		Check(Near(one.color.r, 1.0f) && Near(one.color.g, 0.5f) && Near(one.color.b, 0.0f), "out-of-reach light does not tint");
		Check(one.level > 0.0f && one.level <= 1.0f, "level in (0,1]", one.level, 0.5);
	}

	// --- light fallbacks and folding -------------------------------------------------
	std::printf("[lightmix]\n");
	{
		const auto dir = CellLightingFallback({ 0.8f, 0.4f, 0.2f }, { 0.1f, 0.1f, 0.3f });
		Check(dir.usedDirectional && Near(dir.light.level, 0.35f) && Near(dir.light.color.r, 1.0f) && Near(dir.light.color.g, 0.5f), "cell lighting: directional colour at 0.35");
		const auto amb = CellLightingFallback({ 0.0f, 0.0f, 0.0f }, { 0.2f, 0.2f, 0.4f });
		Check(!amb.usedDirectional && Near(amb.light.level, 0.35f) && Near(amb.light.color.b, 1.0f), "cell lighting: black directional -> ambient");
		const auto none = CellLightingFallback({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f });
		Check(Near(none.light.level, 0.0f), "cell lighting: black cell stays dark");
		// folding a dynamic light into the static result
		LightResult dark;  // no static light
		const auto  lit = FoldLight(dark, { 0.5f, 0.7f, 1.0f }, 0.6f);
		Check(Near(lit.level, 0.6f) && Near(lit.color.b, 1.0f), "magelight in a dark cell lights the caustic");
		LightResult warm;
		warm.color = { 1.0f, 0.5f, 0.0f };
		warm.total = 0.5f;
		warm.level = 0.5f;
		const auto mix = FoldLight(warm, { 0.0f, 0.5f, 1.0f }, 0.5f);
		Check(Near(mix.color.r, 0.5f) && Near(mix.color.b, 0.5f) && Near(mix.level, 1.0f), "equal weights average the colours");
		Check(Near(FoldLight(warm, { 0.0f, 0.0f, 1.0f }, 0.0f).color.r, 1.0f), "zero-weight light changes nothing");

		// smoothing glides and converges
		Vec3  p{ 0.0f, 0.0f, 0.0f };
		float maxStep = 0.0f;
		for (int i = 0; i < 30; ++i) {  // 3 s at 10 Hz, tau 0.2 s
			const Vec3 n = SmoothToward(p, { 100.0f, 0.0f, 0.0f }, 0.1f, 0.2f);
			maxStep = std::fmax(maxStep, n[0] - p[0]);
			p = n;
		}
		Check(maxStep < 40.0f && Near(p[0], 100.0f, 0.1f), "key light position glides (no jump) and settles", maxStep, 40.0);
	}

	// --- ripple ring --------------------------------------------------------------------
	std::printf("[ripple]\n");
	{
		const float decay = 1.5f;  // impulseDecaySeconds default
		RippleState r;
		Check(Near(r.amplitude, 0.0f) && !r.hasPosition, "starts silent with no position");

		// nobody in the water: stays 0, position untouched
		r = StepRipple(r, 0.0f, false, 50.0f, 50.0f, 0.1f, decay, kRipplePosTau);
		Check(Near(r.amplitude, 0.0f) && !r.hasPosition && Near(r.x, 0.0f), "no actor -> amplitude 0, position unchanged");

		// actor enters: amplitude rises at once, position snaps on first sighting
		r = StepRipple(r, 0.6f, true, 40.0f, -30.0f, 0.1f, decay, kRipplePosTau);
		Check(Near(r.amplitude, 0.6f) && r.hasPosition && Near(r.x, 40.0f) && Near(r.y, -30.0f), "first sighting snaps position, amp = impulse");

		// actor moves: position glides (no jump), converges
		float maxStep = 0.0f;
		for (int i = 0; i < 20; ++i) {  // 2 s at 10 Hz
			const RippleState n = StepRipple(r, 0.6f, true, 140.0f, -30.0f, 0.1f, decay, kRipplePosTau);
			maxStep = std::fmax(maxStep, n.x - r.x);
			r = n;
		}
		Check(maxStep < 100.0f && Near(r.x, 140.0f, 0.1f), "ring glides after the actor and catches up", maxStep, 100.0);

		// actor leaves: amplitude fades (not snaps), position kept
		const RippleState left = StepRipple(r, 0.0f, false, 999.0f, 999.0f, 0.1f, decay, kRipplePosTau);
		Check(left.amplitude > 0.5f && left.amplitude < 0.6f, "leaving: amplitude starts fading, no snap", left.amplitude, 0.56);
		Check(Near(left.x, r.x) && Near(left.y, r.y), "leaving: ring stays where the actor was", left.x, r.x);
		RippleState fade = left;
		for (int i = 0; i < 60; ++i) {  // 6 s
			fade = StepRipple(fade, 0.0f, false, 0.0f, 0.0f, 0.1f, decay, kRipplePosTau);
		}
		Check(fade.amplitude < 0.02f && Near(fade.x, r.x), "rings fade out within a few seconds", fade.amplitude, 0.02);

		// actor stops moving: impulse drops to its standing value; amplitude decays down to it
		RippleState stop = r;  // amp 0.6
		stop = StepRipple(stop, 0.1f, true, 140.0f, -30.0f, 0.1f, decay, kRipplePosTau);
		Check(stop.amplitude > 0.5f, "stopping: amplitude decays instead of dropping to the standing value", stop.amplitude, 0.5);
		for (int i = 0; i < 80; ++i) {
			stop = StepRipple(stop, 0.1f, true, 140.0f, -30.0f, 0.1f, decay, kRipplePosTau);
		}
		Check(Near(stop.amplitude, 0.1f, 1e-3f), "settles at the standing actor's impulse", stop.amplitude, 0.1);

		Check(Near(StepRipple({}, 7.0f, true, 0.0f, 0.0f, 0.1f, decay, kRipplePosTau).amplitude, 1.0f), "amplitude clamped to 1");
	}

	// --- water zones: rect authoring, distance, active --------------------------------
	std::printf("[zones]\n");
	{
		const auto r = RectFromCorners(300.0f, -50.0f, 100.0f, 250.0f);  // corners given "backwards"
		Check(r.has_value() && Near(r->minX, 100.0f) && Near(r->maxX, 300.0f) && Near(r->minY, -50.0f) && Near(r->maxY, 250.0f),
			"two corners -> sorted rect");
		Check(!RectFromCorners(0.0f, 0.0f, 31.0f, 500.0f).has_value(), "31 units wide -> refused");
		Check(!RectFromCorners(0.0f, 0.0f, 500.0f, 31.9f).has_value(), "31.9 units deep -> refused");
		Check(RectFromCorners(0.0f, 0.0f, 32.0f, 32.0f).has_value(), "32 x 32 -> accepted");
		Check(!RectFromCorners(5.0f, 5.0f, 5.0f, 5.0f).has_value(), "same corner twice -> refused");

		const Rect z{ 0.0f, 0.0f, 100.0f, 200.0f };
		Check(RectContains(z, 50.0f, 50.0f) && !RectContains(z, 150.0f, 50.0f), "rect contains");
		Check(Near(DistanceToRect2D(z, 50.0f, 50.0f), 0.0f), "inside -> 2D distance 0");
		Check(Near(DistanceToRect2D(z, 130.0f, 240.0f), 50.0f), "outside the corner -> 3-4-5 distance");
		Check(Near(DistanceToZone(z, 10.0f, 250.0f, { 50.0f, 50.0f, 100.0f }), 0.0f), "inside the volume -> 0");
		Check(Near(DistanceToZone(z, 10.0f, 250.0f, { 50.0f, 50.0f, 300.0f }), 40.0f), "above the height -> distance to the top");
		Check(Near(DistanceToZone(z, 10.0f, 250.0f, { 50.0f, 50.0f, -20.0f }), 30.0f), "below the water -> distance to the water");
		Check(Near(DistanceToZone(z, 10.0f, 250.0f, { 130.0f, 240.0f, 300.0f }), std::sqrt(50.0f * 50.0f + 40.0f * 40.0f), 1e-3f),
			"outside corner and above -> both combined");

		Check(IsZoneActive(true, 100.0f, 3000.0f), "attached and near -> active");
		Check(!IsZoneActive(false, 100.0f, 3000.0f), "cell not attached -> inactive");
		Check(!IsZoneActive(true, 3000.0f, 3000.0f) && !IsZoneActive(true, 5000.0f, 3000.0f), "at/after maxDistance -> inactive");

		Check(RectsOverlap(z, { 50.0f, 50.0f, 300.0f, 300.0f }), "overlapping rects");
		Check(!RectsOverlap(z, { 100.0f, 0.0f, 200.0f, 200.0f }), "touching edges do not overlap");
		Check(!RectsOverlap(z, { 500.0f, 500.0f, 600.0f, 600.0f }), "far rects do not overlap");
	}

	// --- atlas frame clock ------------------------------------------------------------
	std::printf("[clock]\n");
	{
		Check(Near(AdvanceFrameClock(0.0f, 1.0f, 1.0f), 8.0f), "1 s at speed 1 -> 8 frames");
		Check(Near(AdvanceFrameClock(0.0f, 1.0f, 2.25f), 18.0f), "speed multiplies the frame rate");
		Check(Near(AdvanceFrameClock(63.9f, 0.1f, 1.0f), 0.7f, 1e-4f), "wraps past 64", AdvanceFrameClock(63.9f, 0.1f, 1.0f), 0.7);
		Check(Near(AdvanceFrameClock(12.5f, 0.0f, 1.0f), 12.5f), "dt 0 -> unchanged");
		bool inRange = true;
		float c = 0.0f;
		for (int i = 0; i < 100000; ++i) {
			c = AdvanceFrameClock(c, 0.0167f, 2.25f);
			inRange = inRange && c >= 0.0f && c < 64.0f;
		}
		Check(inRange, "always in [0, 64) over a long run");
		Check(Near(AdvanceFrameClock(-5.0f, 0.0f, 1.0f), 0.0f) && Near(AdvanceFrameClock(NAN, 0.0f, 1.0f), 0.0f), "bad clock resets to 0");
		Check(Near(AdvanceFrameClock(3.0f, 1.0f, -1.0f), 3.0f), "negative speed does not run backwards");
	}

	// --- camera: matrices, projection round trip -------------------------------------
	// Synthetic camera in the engine's convention (row vectors, v * View * Proj, D3D depth 0..1).
	auto makeView = [](const Vec3& eye, const Vec3& r, const Vec3& u, const Vec3& f) {
		Mat4 v = Identity();
		for (int i = 0; i < 3; ++i) {
			v.at(i, 0) = r[i];
			v.at(i, 1) = u[i];
			v.at(i, 2) = f[i];
		}
		v.at(3, 0) = -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]);
		v.at(3, 1) = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
		v.at(3, 2) = -(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
		return v;
	};
	auto makeProj = [](float fovY, float aspect, float zn, float zf) {
		Mat4        p;
		const float ys = 1.0f / std::tan(fovY * 0.5f);
		p.at(0, 0) = ys / aspect;
		p.at(1, 1) = ys;
		p.at(2, 2) = zf / (zf - zn);
		p.at(2, 3) = 1.0f;
		p.at(3, 2) = -zn * zf / (zf - zn);
		return p;
	};
	auto near3 = [](const Vec3& a, const Vec3& b, float eps) {
		return Near(a[0], b[0], eps) && Near(a[1], b[1], eps) && Near(a[2], b[2], eps);
	};

	// camera at the origin of rel space (posAdjust = camera), looking north (+y), z up
	const Mat4     view = makeView({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
	const Mat4     proj = makeProj(1.2f, 16.0f / 9.0f, 15.0f, 353840.0f);
	const Mat4     vpm = Mul(view, proj);
	const Viewport vp{ 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };

	std::printf("[camera]\n");
	{
		const auto inv = Inverse(vpm);
		Check(inv.has_value(), "view x proj is invertible");
		const Mat4 id = Mul(vpm, *inv);
		float      err = 0.0f;
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				err = std::fmax(err, std::fabs(id.at(i, j) - (i == j ? 1.0f : 0.0f)));
			}
		}
		Check(err < 1e-4f, "M x inverse(M) = identity", err, 0.0);
		Mat4 singular;  // all zeros
		Check(!Inverse(singular).has_value(), "singular matrix -> no inverse");

		const ScreenPoint c = ProjectToPixel({ 0.0f, 500.0f, 0.0f }, vp, vpm);
		Check(Near(c.x, 960.0f, 1e-2f) && Near(c.y, 540.0f, 1e-2f) && c.w > 0.0f, "point straight ahead -> viewport centre");
		const ScreenPoint up = ProjectToPixel({ 0.0f, 500.0f, 100.0f }, vp, vpm);
		const ScreenPoint right = ProjectToPixel({ 100.0f, 500.0f, 0.0f }, vp, vpm);
		Check(up.y < 540.0f && right.x > 960.0f, "higher -> smaller pixel y; east -> larger pixel x");
		Check(ProjectToPixel({ 0.0f, -500.0f, 0.0f }, vp, vpm).w < 0.0f, "behind the camera -> w < 0");

		bool  roundTrip = true;
		float worst = 0.0f;
		for (const Vec3 p : { Vec3{ 0.0f, 100.0f, 0.0f }, Vec3{ -300.0f, 800.0f, 120.0f }, Vec3{ 250.0f, 400.0f, -90.0f },
				 Vec3{ 900.0f, 1800.0f, 300.0f }, Vec3{ -40.0f, 60.0f, -20.0f } }) {
			const ScreenPoint s = ProjectToPixel(p, vp, vpm);
			const auto        back = UnprojectPixel(s.x, s.y, s.depth, vp, *inv);
			const float       d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
			const float       tol = 0.02f + 2e-7f * d * d;  // float depth precision grows with distance^2
			roundTrip = roundTrip && back && near3(*back, p, tol);
			if (back) {
				worst = std::fmax(worst, std::fabs((*back)[1] - p[1]));
			}
		}
		Check(roundTrip, "pixel + depth -> the same point (5 points, up to 2000 units)", worst, 0.0);

		// offset viewport (dynamic resolution / letterbox) and a reversed depth range
		const Viewport vp2{ 100.0f, 50.0f, 1280.0f, 720.0f, 1.0f, 0.0f };
		const Vec3     q{ 120.0f, 650.0f, 40.0f };
		const auto     s2 = ProjectToPixel(q, vp2, vpm);
		const auto     b2 = UnprojectPixel(s2.x, s2.y, s2.depth, vp2, *inv);
		Check(b2 && near3(*b2, q, 0.05f), "round trip with an offset viewport and reversed depth range");
		Check(!UnprojectPixel(10.0f, 10.0f, 0.5f, Viewport{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }, *inv).has_value(), "empty viewport -> none");
		Check(!UnprojectPixel(10.0f, 10.0f, 0.5f, Viewport{ 0.0f, 0.0f, 100.0f, 100.0f, 0.5f, 0.5f }, *inv).has_value(),
			"zero depth range -> none");
	}

	// --- scissor from the zone volume ------------------------------------------------
	std::printf("[scissor]\n");
	{
		const Vec3 adj{ 1000.0f, 2000.0f, 50.0f };  // camera world position = posAdjust
		// zone 600-1000 units ahead of the camera, water 50 below the eye
		const Rect ahead{ 800.0f, 2600.0f, 1200.0f, 3000.0f };
		const auto s = ScissorForZone(ahead, 0.0f, 250.0f, 250.0f, adj, vpm, vp);
		Check(s.has_value(), "zone ahead -> a scissor rect");
		if (s) {
			const ScreenPoint c = ProjectToPixel({ 0.0f, 800.0f, 0.0f }, vp, vpm);  // rect centre on the water
			Check(s->left >= 0 && s->top >= 0 && s->right <= 1920 && s->bottom <= 1080, "scissor inside the viewport");
			Check(s->left <= c.x && c.x <= s->right && s->top <= c.y && c.y <= s->bottom, "scissor contains the zone centre");
			Check(s->right - s->left < 1920 || s->bottom - s->top < 1080, "zone ahead does not cover the whole screen");
		}

		const Rect behind{ 800.0f, 1000.0f, 1200.0f, 1400.0f };  // 350+ units behind, reach 250 keeps it behind
		Check(!ScissorForZone(behind, 0.0f, 250.0f, 250.0f, adj, vpm, vp).has_value(), "zone entirely behind -> not drawn");

		const Rect around{ 900.0f, 1900.0f, 1100.0f, 2100.0f };  // camera stands over the water
		const auto a = ScissorForZone(around, 0.0f, 250.0f, 250.0f, adj, vpm, vp);
		Check(a && a->left == 0 && a->top == 0 && a->right == 1920 && a->bottom == 1080, "camera inside the zone -> full viewport");

		const Rect side{ 9000.0f, 2600.0f, 9400.0f, 3000.0f };  // far to the east, outside the view cone
		Check(!ScissorForZone(side, 0.0f, 250.0f, 250.0f, adj, vpm, vp).has_value(), "zone outside the view cone -> not drawn");

		const Rect edge{ 1500.0f, 2600.0f, 3000.0f, 3000.0f };  // runs off the right edge
		const auto e = ScissorForZone(edge, 0.0f, 250.0f, 250.0f, adj, vpm, vp);
		Check(e && e->right == 1920 && e->left > 960, "zone off the right edge -> clamped to the viewport");

		// reach grows the box: the wall band next to the rect is inside the scissor
		const auto noReach = ScissorForZone(ahead, 0.0f, 250.0f, 0.0f, adj, vpm, vp);
		Check(noReach && s && (s->right - s->left) > (noReach->right - noReach->left), "reach widens the scissor");

		// a corner 0.01 in front of the camera plane (zone starts right beside the camera)
		const Rect beside{ 1500.0f, 2000.0f + 250.0f + 0.01f, 1600.0f, 2600.0f };
		const auto bs = ScissorForZone(beside, 0.0f, 250.0f, 250.0f, adj, vpm, vp);
		Check(bs && bs->left == 0 && bs->top == 0 && bs->right == 1920 && bs->bottom == 1080,
			"corner almost on the camera plane -> full viewport, not dropped");
		const Rect atPlane{ 1500.0f, 2000.0f + 250.0f, 1600.0f, 2600.0f };  // corner exactly on the plane (w = 0)
		const auto ap = ScissorForZone(atPlane, 0.0f, 250.0f, 250.0f, adj, vpm, vp);
		Check(ap && ap->right == 1920 && ap->bottom == 1080, "corner exactly on the camera plane -> full viewport");

		// corners just past kNearW but two million units to the side: projected pixel x ~ 1e8+
		const Rect farSide{ 1000.0f + 2e6f, 2000.0f + 250.0f + 6.0f, 1000.0f + 2e6f + 100.0f, 2400.0f };
		Check(!ScissorForZone(farSide, 0.0f, 250.0f, 250.0f, adj, vpm, vp).has_value(),
			"huge projected coordinates are clamped in float (no int overflow) -> off-screen");
		const Rect farLeft{ 1000.0f - 2e6f - 100.0f, 2000.0f + 250.0f + 6.0f, 1000.0f + 100.0f, 2400.0f };  // spans far left to centre
		const auto fl = ScissorForZone(farLeft, 0.0f, 250.0f, 250.0f, adj, vpm, vp);
		Check(fl && fl->left == 0 && fl->right > 960 && fl->right <= 1920, "a corner far off the left edge clamps to 0");

		Check(!ScissorForZone(ahead, 0.0f, 250.0f, 250.0f, adj, vpm, Viewport{ 1e30f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f }).has_value(),
			"absurd viewport -> not drawn");

		const Viewport off{ 100.0f, 50.0f, 1280.0f, 720.0f, 0.0f, 1.0f };
		const auto     o = ScissorForZone(around, 0.0f, 250.0f, 250.0f, adj, vpm, off);
		Check(o && o->left == 100 && o->top == 50 && o->right == 1380 && o->bottom == 770, "full viewport follows the viewport offset");
	}

	// --- pass constants (b0) -----------------------------------------------------------
	std::printf("[pass]\n");
	{
		Check(sizeof(PassConstants) == 480, "PassConstants is 480 bytes", sizeof(PassConstants), 480);
		Check(offsetof(PassConstants, falloff) == 416, "falloff at 416");
		{
			// Community Shaders' inverse-square light: the Light Placer example (fade 1.5 -> intensity 6, cutoff 0.2, size 3.4)
			const Falloff f = CsInverseSquare(6.0f, 0.2f, 3.4f, false);
			Check(std::fabs(f.radius - 308.0f) < 2.0f, "CS inverse-square radius (6, 0.2, 3.4) ~ 308", f.radius, 308.0);
			Check(Near(Attenuation(f, 0.0f, 0.0f), 3920.0f / f.sizeBias, 1e-5f) && std::fabs(Attenuation(f, 0.0f, 0.0f) - 0.173f) < 0.002f,
				"CS inverse-square attenuation at the light ~ 0.173", Attenuation(f, 0.0f, 0.0f), 0.173);
			Check(Near(Attenuation(f, 0.0f, f.radius), 0.0f) && Attenuation(f, 0.0f, 50.0f) < Attenuation(f, 0.0f, 10.0f) && Attenuation(f, 0.0f, 200.0f) < Attenuation(f, 0.0f, 50.0f),
				"CS inverse-square: falls with distance, 0 at its radius");
			Check(Near(CsInverseSquare(6.0f, 1.0f, 3.4f, false).radius, CsInverseSquare(6.0f, 0.05f, 3.4f, false).radius), "cutoff 1 = CS default 0.05");
			const float at0 = Attenuation(CsInverseSquare(6.0f, 0.2f, 0.0f, false), 0.0f, 0.0f);
			Check(std::isfinite(at0) && at0 > 0.0f, "a size-0 inverse-square light at its own place: finite", at0, 3920.0);
			Check(Near(Attenuation(Falloff{}, 100.0f, 100.0f * kRadiusScale), 0.0f) && Near(Attenuation(Falloff{}, 100.0f, 0.0f), 1.0f), "vanilla falloff: 1 at the light, 0 at the reach");
		}
		Check(offsetof(PassConstants, ambientHue) == 400, "ambient hue at 400");
		Check(offsetof(PassConstants, viewProj) == 288 && offsetof(PassConstants, ambientPos) == 352 && offsetof(PassConstants, ambientNeg) == 368 &&
				  offsetof(PassConstants, misc) == 384,
			"viewProj 288, ambient 352 / 368, misc 384");
		const std::size_t offs[] = { offsetof(PassConstants, posAdjust), offsetof(PassConstants, viewport),
			offsetof(PassConstants, depthRange), offsetof(PassConstants, rect), offsetof(PassConstants, water),
			offsetof(PassConstants, params), offsetof(PassConstants, light) };
		bool offsetsOk = offsetof(PassConstants, invViewProj) == 0 && offsetof(PassConstants, color) == 208 &&
		                 offsetof(PassConstants, layers) == 256 && offsetof(PassConstants, ripple) == 272;
		for (std::size_t i = 0; i < 7; ++i) {
			offsetsOk = offsetsOk && offs[i] == 64 + 16 * i;
		}
		Check(offsetsOk, "matrix at 0, float4s at 64..144, light[3] at 160, color[3] at 208, layers 256, ripple 272 (HLSL packing)");
		{
			ZoneGpuInput zl;
			zl.weights = CrossfadeWeights(0.75f);
			const PassConstants cl = PackPass(zl, FrameCamera{});
			Check(Near(cl.layers.x + cl.layers.y + cl.layers.z, 1.0f) && Near(cl.layers.y, zl.weights.normal) && Near(cl.layers.z, zl.weights.rough),
				"layer weights packed (sum 1)");
			zl.weights = { 0.0f, 0.0f, 0.0f };
			Check(Near(PackPass(zl, FrameCamera{}).layers.x, 1.0f), "no weights -> calm only");
		}

		ZoneGpuInput z;
		z.rect = { -10.0f, -20.0f, 30.0f, 40.0f };
		z.waterZ = 5.0f;
		z.height = 250.0f;
		z.distFade = 0.75f;
		z.light = { 100.0f, 200.0f, 300.0f };
		z.lightRadius = 512.0f;
		z.lightColor = { 1.0f, 0.5f, 0.25f };
		z.level = 0.8f;
		z.brightness = 0.5f;
		z.weights = CrossfadeWeights(0.9f);  // calm weight 0: must not dim the single v1 layer
		z.frameClock = 12.5f;
		z.debugTint = true;
		FrameCamera cam;
		cam.invViewProj = *Inverse(vpm);
		cam.posAdjust = { 7.0f, 8.0f, 9.0f };
		cam.viewport = { 10.0f, 20.0f, 1600.0f, 900.0f, 0.0f, 1.0f };
		const PassConstants c = PackPass(z, cam);
		Check(std::memcmp(c.invViewProj, cam.invViewProj.m, sizeof(c.invViewProj)) == 0, "matrix copied as-is (row-major)");
		Check(Near(c.posAdjust.x, 7.0f) && Near(c.posAdjust.z, 9.0f), "posAdjust");
		Check(Near(c.viewport.x, 10.0f) && Near(c.viewport.w, 900.0f) && Near(c.depthRange.y, 1.0f), "viewport + depth range");
		Check(Near(c.rect.x, -10.0f) && Near(c.rect.w, 40.0f), "rect");
		Check(Near(c.water.x, 5.0f) && Near(c.water.y, 250.0f) && Near(c.water.z, 0.0f) && Near(c.water.w, 0.75f), "water");
		Check(Near(c.light[0].x, 100.0f) && Near(c.light[0].z, 300.0f) && Near(c.light[0].w, 512.0f), "key light + radius");
		Check(Near(c.color[0].x, 1.0f) && Near(c.color[0].y, 0.5f) && Near(c.color[0].z, 0.25f) && Near(c.color[0].w, kMaxBoost), "key colour as given (level and brightness go to the gain), max boost");
		Check(Near(c.params.x, 12.5f) && Near(c.params.y, 0.05f) && Near(c.params.z, 1.0f) && Near(c.params.w, kCausticGain * 0.5f * 0.8f),
			"clock, ambient (default 0.05), debugTint strength 1 (default), gain x brightness 0.5 x level 0.8");
		{
			ZoneGpuInput zs = z;  // the sun: a (smoothed, not unit) direction becomes a unit vector with w = -1
			zs.sun = true;
			zs.light = { 0.3f, 0.0f, 0.4f };
			const PassConstants cs = PackPass(zs, cam);
			Check(Near(cs.light[0].w, -1.0f) && Near(cs.light[0].x, 0.6f) && Near(cs.light[0].z, 0.8f), "sun: unit vector toward it, w = -1");
			zs.light = { 0.0f, 0.0f, 0.0f };
			Check(Near(PackPass(zs, cam).light[0].z, -1.0f), "sun with no direction: below the horizon (draws nothing)");
			ZoneGpuInput zx = z;  // a torch as the key, the sun as an extra (its beam at level 0.5)
			zx.extra[0] = { { 0.0f, 0.6f, 0.8f }, 0.0f, { 1.0f, 1.0f, 1.0f }, 0.5f, 0u, true };
			const PassConstants cx = PackPass(zx, cam);
			Check(Near(cx.light[1].w, -1.0f) && Near(cx.light[1].y, 0.6f) && Near(cx.light[1].z, 0.8f) && cx.color[1].x > 0.0f,
				"the sun as an extra light: a unit vector toward it, w = -1");
			zs.absorb = 5.0f;
			zs.occlusion = false;
			zs.ambientPos = { 0.1f, -1.0f, 0.3f };
			const PassConstants cm = PackPass(zs, cam);
			Check(Near(cm.misc.x, 20.0f) && Near(cm.misc.y, 0.0f) && Near(cm.ambientPos.x, 0.1f) && Near(cm.ambientPos.y, 0.0f) && Near(cm.ambientPos.z, 0.3f),
				"absorb clamped to 20, occlusion off, directional ambient clamped at 0");
		}
		ZoneGpuInput zt = z;
		zt.debugTintStrength = 0.5f;
		Check(Near(PackPass(zt, cam).params.z, 0.5f), "debugTint strength 0.5 packed");
		zt.debugTintStrength = 0.0f;
		Check(Near(PackPass(zt, cam).params.z, 0.01f), "debugTint on with strength 0 -> still on (0.01)");
		zt.debugTintStrength = 99.0f;
		Check(Near(PackPass(zt, cam).params.z, 4.0f), "debugTint strength clamped to 4");
		zt.debugTint = false;
		Check(Near(PackPass(zt, cam).params.z, 0.0f), "debugTint off -> 0 whatever the strength");
		zt.debugLight = true;
		Check(Near(PackPass(zt, cam).params.z, -1.0f), "light mask -> -1");
		zt.debugTint = true;
		Check(Near(PackPass(zt, cam).params.z, 4.0f), "flat tint wins over the light mask");
		ZoneGpuInput zs = z;
		zs.strength = 10.0f;
		Check(Near(PackPass(zs, cam).params.w, kCausticGain * 10.0f * 0.5f * 0.8f, 1e-3f), "strength 10 -> gain x10");
		zs.strength = 99.0f;
		Check(Near(PackPass(zs, cam).params.w, kCausticGain * 50.0f * 0.5f * 0.8f, 1e-3f), "strength clamped to 50");
		zs.strength = -3.0f;
		Check(Near(PackPass(zs, cam).params.w, 0.0f), "negative strength -> 0");
		zs.strength = std::numeric_limits<float>::quiet_NaN();
		Check(Near(PackPass(zs, cam).params.w, kCausticGain * 0.5f * 0.8f, 1e-4f), "NaN strength -> physical (x1)");
	}

	// --- the pixel shader's zone math (C++ mirror) --------------------------------------
	std::printf("[boost]\n");
	// no mask: the whole rect is water (the manual-zone case before masks); no spread
	auto Boost = [](const PassConstants& pc, const Vec3& p, const Vec3& nn, float pat) { return CausticBoost(pc, p, nn, pat, nullptr); };
	{
		ZoneGpuInput z;
		z.rect = { -200.0f, -200.0f, 200.0f, 200.0f };
		z.waterZ = 0.0f;
		z.height = 250.0f;
		z.distFade = 1.0f;
		z.light = { 0.0f, 0.0f, 100.0f };  // lantern 100 above the middle of the water
		z.lightRadius = 0.0f;
		z.lightColor = { 1.0f, 1.0f, 1.0f };
		z.level = 1.0f;
		FrameCamera cam;
		cam.invViewProj = Identity();
		const PassConstants c = PackPass(z, cam);

		const Vec3 wallP{ 150.0f, 0.0f, 50.0f };
		const auto wall = Boost(c, wallP, { -1.0f, 0.0f, 0.0f }, 0.5f);  // wall facing the water
		Check(wall.inside && wall.add.r > 0.0f && wall.add.r <= 0.35f, "wall facing the water is lit", wall.add.r, 0.01);
		// hand-computed: dir (0.707,0,0.707), fres 0.0221, ndl 0.707, pattern 0.5, gain 2, heightFade 0.8
		// reflected: fres 0.0221 x ndl 0.707 x pattern 0.5 (0.32 at half contrast) x gain 7.138 (height fade 1 at 50 of 250);
		// share of the light already there: direct from the lamp ndl 150/158.1 = 0.949, + ambient 0.05
		const float patEff = kPatternMean + kPatternContrast * (0.5f - kPatternMean);  // pattern 0.5 at the walls' contrast
		const float wallHand = 0.0221f * 0.7071f * patEff * kCausticGain / (150.0f / std::sqrt(150.0f * 150.0f + 50.0f * 50.0f) + 0.05f);
		Check(Near(wall.add.r, wallHand, 1e-3f), "wall value matches the hand calculation", wall.add.r, wallHand);
		ZoneGpuInput darkRoom = z;
		darkRoom.ambient = 0.5f;
		Check(Boost(PackPass(darkRoom, cam), wallP, { -1.0f, 0.0f, 0.0f }, 0.5f).add.r < wall.add.r, "more ambient light -> the same reflection is a smaller share");
		const Vec3 underside{ 50.0f, 0.0f, 60.0f };
		const float underB = Boost(c, underside, { 0.0f, 0.0f, -1.0f }, 0.5f).add.r;
		ZoneGpuInput lit = z;
		const float asWall = Boost(PackPass(lit, cam), underside, { -1.0f, 0.0f, 0.0f }, 0.5f).add.r;
		Check(underB > 0.3f || underB >= kMaxBoost - 1e-4f, "a face the lamp cannot see (underside) gets a large share", underB, 0.3);
		(void)asWall;

		const auto floorUp = Boost(c, { 150.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 1.0f }, 1.0f);
		Check(floorUp.inside && Near(floorUp.add.r, 0.0f), "floor facing up gets nothing (reflected light only goes up)");
		const auto ceiling = Boost(c, { 50.0f, 0.0f, 200.0f }, { 0.0f, 0.0f, -1.0f }, 1.0f);
		Check(ceiling.add.r > 0.0f, "ceiling above the water is lit", ceiling.add.r, 0.0);
		const auto away = Boost(c, wallP, { 1.0f, 0.0f, 0.0f }, 1.0f);
		Check(Near(away.add.r, 0.0f), "wall facing away gets nothing");

		Check(!Boost(c, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, 1.0f).inside, "less than 2 above the water -> rejected");
		Check(!Boost(c, { 0.0f, 0.0f, 300.0f }, { 0.0f, 0.0f, -1.0f }, 1.0f).inside, "above the reach -> rejected");
		Check(!Boost(c, { 500.0f, 0.0f, 50.0f }, { -1.0f, 0.0f, 0.0f }, 1.0f).inside, "farther than reach from the rect -> rejected");

		const auto outside = Boost(c, { 400.0f, 0.0f, 50.0f }, { -1.0f, 0.0f, 0.0f }, 1.0f);
		Check(outside.inside && Near(outside.add.r, 0.0f), "light would meet the water outside the rect -> nothing");

		const auto dark = Boost(c, wallP, { -1.0f, 0.0f, 0.0f }, 0.0f);
		Check(Near(dark.add.r, wallHand * (kPatternMean * (1.0f - kPatternContrast)) / patEff, 1e-4f), "pattern 0 -> only the base glow (none at full contrast)");

		ZoneGpuInput hot = z;
		hot.strength = 50.0f;  // colour alone no longer clamps: it scales the direct light too
		hot.ambient = kMinAmbient;
		const auto clamped = Boost(PackPass(hot, cam), wallP, { -1.0f, 0.0f, 0.0f }, 1.0f);
		Check(Near(clamped.add.r, kMaxBoost) && Near(clamped.add.b, kMaxBoost), "boost clamped to the cap");

		ZoneGpuInput under = z;
		under.light = { 0.0f, 0.0f, -50.0f };  // light under the water: its image is above -> no reflection
		Check(Near(Boost(PackPass(under, cam), wallP, { -1.0f, 0.0f, 0.0f }, 1.0f).add.r, 0.0f), "light below the water -> nothing");
		Check(Near(Boost(PackPass(under, cam), { 150.0f, 0.0f, 100.0f }, { -1.0f, 0.0f, 0.0f }, 1.0f).add.r, 0.0f),
			"light below the water, point above its image -> still nothing");

		ZoneGpuInput small = z;
		small.lightRadius = 100.0f;  // reach 150; mirrored path to the wall is 212
		Check(Near(Boost(PackPass(small, cam), wallP, { -1.0f, 0.0f, 0.0f }, 1.0f).add.r, 0.0f), "beyond 1.5 x radius -> nothing");
		ZoneGpuInput big = z;
		big.lightRadius = 1000.0f;
		const float withR = Boost(PackPass(big, cam), wallP, { -1.0f, 0.0f, 0.0f }, 0.5f).add.r;
		Check(withR > 0.0f && withR < wall.add.r, "a radius only weakens (radius 0 = no falloff)", withR, wall.add.r);

		ZoneGpuInput tint = z;
		tint.debugTint = true;
		tint.lightColor = { 0.0f, 0.0f, 0.0f };
		const PassConstants ct = PackPass(tint, cam);
		const auto          t1 = Boost(ct, { 150.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 1.0f }, 0.0f);
		Check(t1.inside && Near(t1.add.r, 1.0f) && Near(t1.add.g, 1.0f), "debugTint default: flat +100 % (output 2) ignoring light, pattern, facing and the cap");
		ZoneGpuInput tintHalf = tint;
		tintHalf.debugTintStrength = 0.5f;
		Check(Near(Boost(PackPass(tintHalf, cam), { 150.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 1.0f }, 0.0f).add.b, 0.5f), "debugTintStrength 0.5 -> +50 %");
		Check(!Boost(ct, { 0.0f, 0.0f, 300.0f }, { 0.0f, 0.0f, 1.0f }, 1.0f).inside, "debugTint still limited to the zone");

		ZoneGpuInput strong = z;
		strong.strength = 10.0f;
		Check(Near(Boost(PackPass(strong, cam), wallP, { -1.0f, 0.0f, 0.0f }, 0.5f).add.r, std::min(10.0f * wall.add.r, kMaxBoost), 1e-3f), "strength 10 -> 10x the physical value");
		{
			// CausticLight.ini fRippleContrast: 0 = the mean light whatever the pattern, 2 = twice the swing around it
			ZoneGpuInput zc0 = z;
			zc0.rippleContrast = 0.0f;
			const float flatHi = Boost(PackPass(zc0, cam), wallP, { -1.0f, 0.0f, 0.0f }, 0.9f).add.r;
			const float flatMean = Boost(PackPass(zc0, cam), wallP, { -1.0f, 0.0f, 0.0f }, kPatternMean).add.r;
			Check(flatMean > 0.0f && Near(flatHi, flatMean, 1e-6f), "ripple contrast 0: the pattern does not show", flatHi, flatMean);
			ZoneGpuInput zc2 = z;
			zc2.rippleContrast = 2.0f;
			const float base = Boost(PackPass(z, cam), wallP, { -1.0f, 0.0f, 0.0f }, 0.3f).add.r;
			const float two = Boost(PackPass(zc2, cam), wallP, { -1.0f, 0.0f, 0.0f }, 0.3f).add.r;
			Check(two - flatMean > 1.9f * (base - flatMean) && two - flatMean < 2.1f * (base - flatMean), "ripple contrast 2: twice the swing around the mean", two - flatMean, 2.0 * (base - flatMean));
		}
		{
			// past the game's shadow distance the pattern is gone, as the shadows; well inside it nothing changes
			PassConstants pf = PackPass(z, cam);
			const float   dx = wallP[0] - pf.posAdjust.x, dy = wallP[1] - pf.posAdjust.y, dz = wallP[2] - pf.posAdjust.z;
			const float   d = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float   base = Boost(pf, wallP, { -1.0f, 0.0f, 0.0f }, 0.5f).add.r;
			pf.fade.x = 2.0f * d;
			const float near = Boost(pf, wallP, { -1.0f, 0.0f, 0.0f }, 0.5f).add.r;
			pf.fade.x = 0.5f * d;
			const float far = Boost(pf, wallP, { -1.0f, 0.0f, 0.0f }, 0.5f).add.r;
			Check(base > 0.0f && Near(near, base, 1e-6f) && Near(far, 0.0f), "shadow distance: unchanged well inside, gone beyond", far, 0.0);
		}
		strong.strength = 0.0f;
		Check(Near(Boost(PackPass(strong, cam), wallP, { -1.0f, 0.0f, 0.0f }, 1.0f).add.r, 0.0f), "strength 0 -> nothing");

		ZoneGpuInput mask = z;
		mask.debugLight = true;
		const PassConstants cm = PackPass(mask, cam);
		// wall: atten 1, ndl 0.707, inside 1, heightFade 1 -> 0.707 as it is (no x4); no Fresnel, pattern or cap
		const auto m1 = Boost(cm, wallP, { -1.0f, 0.0f, 0.0f }, 0.0f);
		Check(m1.inside && Near(m1.add.b, 0.7071f, 5e-3f) && Near(m1.add.r, 0.25f * 0.7071f, 5e-3f), "light mask on the wall ignores pattern and Fresnel", m1.add.b, 0.7071);
		Check(Near(Boost(cm, { 150.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 1.0f }, 1.0f).add.b, 0.0f), "light mask: floor facing up still gets nothing");
		Check(Near(Boost(cm, { 400.0f, 0.0f, 50.0f }, { -1.0f, 0.0f, 0.0f }, 1.0f).add.b, 0.0f), "light mask: outside the rect still gets nothing");

		// The Ragged Flagon test zone (2026-09-29): the skylight lamp 493 above the water, 600 away; a bridge
		// underside 50 above the water. Ambient from the log: cell 0.235 + other lights, key intensity 12.9 x 0.77.
		ZoneGpuInput rf;
		rf.rect = { -1651.0f, 1334.0f, -1432.0f, 1461.0f };
		rf.waterZ = -149.9f;
		rf.height = 250.0f;
		rf.distFade = 1.0f;
		rf.light = { -1177.8f, 1835.0f, 343.0f };
		rf.lightRadius = 1000.0f;
		rf.lightColor = { 0.77f, 0.77f, 0.77f };
		rf.level = 1.0f;
		rf.ambient = (0.235f + 0.755f * 0.64f) / (12.91f * 0.77f);
		const Vec3 rfUnder{ -1577.0f, 1356.0f, -99.9f };  // meets the water at about (-1540, 1400), well inside
		const float phys = Boost(PackPass(rf, cam), rfUnder, { 0.0f, 0.0f, -1.0f }, 1.0f).add.r;
		rf.strength = kDefaultStrength;
		const float art = Boost(PackPass(rf, cam), rfUnder, { 0.0f, 0.0f, -1.0f }, 1.0f).add.r;
		std::printf("  Ragged Flagon bridge underside, pattern peak: physical +%.1f %%, strength %.0f +%.1f %%\n", phys * 100.0f, kDefaultStrength, art * 100.0f);
		rf.strength = 1.0f;
		const float rfWall = Boost(PackPass(rf, cam), { -1577.0f, 1356.0f, -99.9f }, { 0.4f, 0.9165f, 0.0f }, 1.0f).add.r;  // same spot, facing the lamp
		std::printf("  Ragged Flagon, same spot facing the lamp: +%.1f %%\n", rfWall * 100.0f);
		Check(phys > 0.05f, "Ragged Flagon: an underside gets a visible share even at strength 1", phys, 0.05);
		Check(rfWall < phys, "Ragged Flagon: a face lit by the lamp gets a smaller share", rfWall, phys);

		const float nan = std::numeric_limits<float>::quiet_NaN();
		const auto  np = Boost(c, { nan, 0.0f, 50.0f }, { -1.0f, 0.0f, 0.0f }, 1.0f);
		Check(!np.inside && Near(np.add.r, 0.0f), "NaN position -> pixel unchanged");
		Check(Near(Boost(c, wallP, { nan, 0.0f, 0.0f }, 1.0f).add.r, 0.0f), "NaN normal -> pixel unchanged");
		Check(Near(Boost(c, wallP, { -1.0f, 0.0f, 0.0f }, nan).add.r, 0.0f), "NaN pattern -> pixel unchanged");
		ZoneGpuInput neg = z;
		neg.lightColor = { -5.0f, -5.0f, -5.0f };
		neg.level = 1.0f;
		const PassConstants cn = PackPass(neg, cam);
		Check(Boost(cn, wallP, { -1.0f, 0.0f, 0.0f }, 1.0f).add.r >= 0.0f, "negative colour -> never darker than the scene");
		PassConstants cinf = c;
		cinf.color[0].x = std::numeric_limits<float>::infinity();
		Check(Near(Boost(cinf, wallP, { -1.0f, 0.0f, 0.0f }, 1.0f).add.r, 0.0f), "infinite colour -> pixel unchanged");

		// pool floor under the water: the refracted light bunched into the pattern around its mean
		{
			ZoneGpuInput rz = z;
			rz.spread = ReflectionSpread(0.5f);  // ripples: the surface focuses at BottomFocus
			const PassConstants cr = PackPass(rz, cam);
			const Vec3 floorP{ 50.0f, 0.0f, -200.0f }, up{ 0.0f, 0.0f, 1.0f };
			const float atMean = Boost(cr, floorP, up, kPatternMean).add.r;
			const float onStreak = Boost(cr, floorP, up, 1.0f).add.r;
			const float inGap = Boost(cr, floorP, up, 0.0f).add.r;
			Check(Boost(cr, floorP, up, 0.5f).inside, "a floor under the water is handled");
			Check(Near(atMean, 0.0f, 1e-5f), "floor: the pattern's mean adds nothing (light only redistributed)", atMean, 0.0);
			Check(onStreak > 0.02f && inGap < 0.0f && inGap >= kMinBottomBoost, "floor: brighter on a streak, a little darker in a gap", onStreak, inGap);
			{
				ZoneGpuInput off = rz;
				off.floorScale = 0.0f;  // CausticLight.ini fFloorStrength 0
				Check(Near(Boost(PackPass(off, cam), floorP, up, 1.0f).add.r, 0.0f), "floor strength 0: no floor pattern");
			}
			{
				ZoneGpuInput hot = rz;
				hot.strength = kMaxStrength;
				const float top = Boost(PackPass(hot, cam), floorP, up, 1.0f).add.r;
				Check(Near(top, kMaxFloorBoost, 1e-4f) && kMaxFloorBoost < kMaxBoost, "floor: its own cap (not the walls' higher one)", top, kMaxFloorBoost);
			}
			const float shallow = Boost(cr, { 50.0f, 0.0f, -20.0f }, up, 1.0f).add.r;
			Check(shallow > 0.0f && shallow < onStreak, "floor: shallow water (far from the focus) is gentler", shallow, onStreak);
			Check(BottomFocus(ReflectionSpread(1.0f)) < BottomFocus(ReflectionSpread(0.2f)), "rougher ripples focus closer");
			Check(Near(BottomContrastAt(0.0f, rz.spread), 0.0f) && Near(BottomContrastAt(1e6f, rz.spread), kBottomMaxContrast), "floor contrast: 0 at the surface, capped far down");
			Check(Near(Boost(cr, { 50.0f, 0.0f, -500.0f }, up, 1.0f).add.r, 0.0f), "floor: deeper than the limit is ignored");
			Check(Near(Boost(cr, { 350.0f, 0.0f, -50.0f }, up, 1.0f).add.r, 0.0f), "floor with no water over it: nothing");
			Check(Near(Boost(cr, { 50.0f, 0.0f, -1.0f }, up, 1.0f).add.r, 0.0f), "at the waterline: nothing");
			// a lamp just above the water: its light arrives diverging, the floor pattern is softer than from far away
			{
				ZoneGpuInput nearL = rz, farL = rz;
				nearL.light = { 50.0f, 0.0f, 60.0f };
				farL.light = { 50.0f, 0.0f, 50000.0f };
				nearL.lightRadius = farL.lightRadius = 0.0f;
				const float sNear = Boost(PackPass(nearL, cam), floorP, up, 1.0f).add.r, sFar = Boost(PackPass(farL, cam), floorP, up, 1.0f).add.r;
				Check(sNear > 0.0f && sNear < sFar, "floor: a lamp close to the water focuses less (d n D / (d + n D))", sNear, sFar);
			}
			ZoneGpuInput off = rz;
			off.strength = 0.0f;
			Check(Near(Boost(PackPass(off, cam), floorP, up, 1.0f).add.r, 0.0f), "floor: strength 0 turns it off too");
		}

		ZoneGpuInput faded = z;
		faded.distFade = 0.0f;
		Check(Near(Boost(PackPass(faded, cam), wallP, { -1.0f, 0.0f, 0.0f }, 1.0f).add.r, 0.0f), "distance fade 0 -> nothing");
	}


	// --- water mask, glossy spread, extra lights -------------------------------------------------
	std::printf("[mask]\n");
	{
		const auto m = MaskFromRect({ -200.0f, -200.0f, 200.0f, 200.0f });
		Check(m && m->size == 64 && m->mips.size() == 7, "rect mask: 64 x 64 with 7 mips");
		Check(Near(m->bounds.minX, -200.0f - 400.0f / 6.0f) && Near(m->bounds.maxY, 200.0f + 400.0f / 6.0f), "rect mask bounds grow by a sixth of the side");
		Check(Near(SampleMask(*m, 0.5f, 0.5f, 0.0f), 1.0f), "mask centre is water");
		Check(Near(SampleMask(*m, 0.02f, 0.5f, 0.0f), 0.0f), "mask border is dry");
		Check(Near(SampleMask(*m, 0.5f, 0.5f, 6.0f), m->mips[6][0] / 255.0f) && m->mips[6][0] > 0 && m->mips[6][0] < 255,
			"coarsest mip = the water fraction of the whole texture");
		const float half = SampleMask(*m, 0.5f, 0.5f, 3.0f);
		Check(half > 0.9f, "a mid mip at the centre stays mostly water", half, 0.9);
		const float edgeFine = SampleMask(*m, 0.125f, 0.5f, 0.0f), edgeCoarse = SampleMask(*m, 0.125f, 0.5f, 4.0f);
		Check(edgeFine > 0.3f && edgeFine < 0.7f, "sample on the rect edge is partly water", edgeFine, 0.5);
		Check(edgeCoarse > 0.0f && edgeCoarse < 1.0f, "coarse sample at the edge is partly water", edgeCoarse, 0.5);

		std::vector<std::uint8_t> g(64 * 64, 255);
		const auto gm = MaskFromGrid({ 0.0f, 0.0f, 640.0f, 640.0f }, 64, g);
		Check(gm && gm->waterTexels == 62 * 62, "grid mask: the border ring is forced dry", gm ? gm->waterTexels : -1, 62 * 62);
		Check(MaskFromGrid({ 0.0f, 0.0f, 640.0f, 640.0f }, 64, std::vector<std::uint8_t>(64 * 64, 0)) == nullptr, "grid without water -> no mask");
		std::vector<std::uint8_t> patch(64 * 64, 0);
		for (int y = 30; y <= 35; ++y) {
			for (int x = 10; x <= 20; ++x) {
				patch[static_cast<std::size_t>(y) * 64 + x] = 255;
			}
		}
		const auto pm = MaskFromGrid({ 0.0f, 0.0f, 640.0f, 640.0f }, 64, patch);  // 10 units per texel
		Check(pm && Near(pm->waterBounds.minX, 100.0f) && Near(pm->waterBounds.maxX, 210.0f) && Near(pm->waterBounds.minY, 300.0f) &&
				  Near(pm->waterBounds.maxY, 360.0f),
			"waterBounds = the wet patch, not the whole grid");
		Check(Near(m->waterBounds.minX, -200.0f, 5.0f) && Near(m->waterBounds.maxX, 200.0f, 5.0f), "rect mask: waterBounds ~ the rect");
		Check(MaskFromGrid({ 0.0f, 0.0f, 640.0f, 640.0f }, 64, std::vector<std::uint8_t>(10, 255)) == nullptr, "wrong grid size -> no mask");

		Check(Near(ReflectionSpread(0.0f), std::tan(6.0f * 3.14159265f / 180.0f), 1e-4f), "calm water: 3 deg slopes -> tan 6 deg");
		Check(Near(ReflectionSpread(1.0f), std::tan(28.0f * 3.14159265f / 180.0f), 1e-4f), "rough water: 14 deg slopes -> tan 28 deg");
		Check(ReflectionSpread(0.2f) > ReflectionSpread(0.0f) && ReflectionSpread(2.0f) == ReflectionSpread(1.0f), "spread grows with agitation, clamped");

		ZoneGpuInput z;
		z.rect = m->bounds;
		z.mask = m;
		z.waterZ = 0.0f;
		z.height = 1000.0f;
		z.distFade = 1.0f;
		z.light = { 0.0f, 0.0f, 100.0f };
		z.lightColor = { 1.0f, 1.0f, 1.0f };
		z.level = 1.0f;
		FrameCamera cam;
		cam.invViewProj = Identity();
		// a wall just outside the water: the mirror ray lands on dry ground
		const Vec3 farWall{ 330.0f, 0.0f, 60.0f };
		const Vec3 inward{ -1.0f, 0.0f, 0.0f };
		const float sharp = CausticBoost(PackPass(z, cam), farWall, inward, 1.0f, m.get()).add.r;
		z.spread = ReflectionSpread(1.0f);
		const float glossy = CausticBoost(PackPass(z, cam), farWall, inward, 1.0f, m.get()).add.r;
		Check(Near(sharp, 0.0f) && glossy > 0.0f, "rough water spreads light onto a wall a perfect mirror misses", glossy, 0.0);
		const Vec3 nearWall{ 150.0f, 0.0f, 50.0f };
		z.spread = 0.0f;
		const float nearSharp = CausticBoost(PackPass(z, cam), nearWall, inward, 1.0f, m.get()).add.r;
		z.spread = ReflectionSpread(1.0f);
		const float nearGlossy = CausticBoost(PackPass(z, cam), nearWall, inward, 1.0f, m.get()).add.r;
		Check(nearSharp > 0.0f && nearGlossy > 0.0f && nearGlossy <= nearSharp + 1e-6f, "over open water the spread never adds light", nearGlossy, nearSharp);
		// far from the water: the clamped edge sample of a coarse mip must not leak light
		z.spread = ReflectionSpread(1.0f);
		// a light beside the pool whose mirror ray meets the plane 633 units outside the mask; the spread
		// disc (radius ~75) is nowhere near water, but a clamped coarse-mip sample would still read ~0.5
		ZoneGpuInput zo = z;
		zo.light = { 800.0f, 0.0f, 100.0f };
		Check(Near(CausticBoost(PackPass(zo, cam), { 1000.0f, 0.0f, 100.0f }, inward, 1.0f, m.get()).add.r, 0.0f), "no light from water far outside the mask");
		Check(MaskFromGrid({ 0.0f, 0.0f, 320.0f, 320.0f }, 32, std::vector<std::uint8_t>(32 * 32, 255)) == nullptr, "only 64 x 64 masks (the shader's size)");

		ZoneGpuInput two = z;
		two.spread = 0.0f;
		two.ambient = 1000.0f;  // so the share is set by the ambient alone and the reflection itself is compared
		const float one = CausticBoost(PackPass(two, cam), nearWall, inward, 1.0f, m.get()).add.r;
		two.extra[0] = { { 0.0f, 0.0f, 100.0f }, 0.0f, { 1.0f, 1.0f, 1.0f }, 1.0f };
		const float both = CausticBoost(PackPass(two, cam), nearWall, inward, 1.0f, m.get()).add.r;
		Check(Near(both, 2.0f * one, 1e-4f), "a second identical light doubles the reflection", both, 2.0 * one);
		two.extra[0].level = 0.0f;
		Check(Near(CausticBoost(PackPass(two, cam), nearWall, inward, 1.0f, m.get()).add.r, one), "an extra light at level 0 is ignored");
		two.extra[0] = { { 0.0f, 0.0f, 100.0f }, 0.0f, { 1.0f, 0.0f, 0.0f }, 1.0f };
		const auto tinted = CausticBoost(PackPass(two, cam), nearWall, inward, 1.0f, m.get()).add;
		Check(Near(tinted.r, 2.0f * one, 1e-4f) && Near(tinted.g, one, 1e-4f), "each light keeps its own colour", tinted.r, 2.0 * one);
		// the share per channel: reflected_c / (direct_c + ambient_c)
		{
			ZoneGpuInput oc = two;
			oc.extra = {};
			oc.ambient = 0.5f;
			oc.lightColor = { 1.0f, 0.5f, 0.25f };
			oc.ambientHue = { 1.0f, 0.5f, 0.25f };  // the room lit in the lamp's own colour: every channel the same share
			const auto same = CausticBoost(PackPass(oc, cam), nearWall, inward, 1.0f, m.get()).add;
			Check(same.r > 0.0f && std::fabs(same.r - same.b) < 1e-3f * same.r + 1e-6f && std::fabs(same.r - same.g) < 1e-3f * same.r + 1e-6f,
				"a lamp in a room of its own colour: the same share in every channel", same.b, same.r);
			oc.ambientHue = { 1.0f, 1.0f, 1.0f };  // a white room: the reflected orange stays orange against it
			const auto white = CausticBoost(PackPass(oc, cam), nearWall, inward, 1.0f, m.get()).add;
			Check(white.r > white.g && white.g > white.b && white.b > 0.0f, "a coloured lamp in a white room: its colour shows in the share", white.b, white.r);
			const auto pc = PackPass(oc, cam);
			Check(Near(0.2126f * pc.ambientHue.x + 0.7152f * pc.ambientHue.y + 0.0722f * pc.ambientHue.z, 1.0f), "ambient hue packed at luminance 1");
		}
		// shadowed water: the key light is reflected only by the water it reaches
		{
			std::vector<std::uint8_t> lit(64 * 64, 0);  // only the west half of the pool is in the lamp's light
			for (int y = 0; y < 64; ++y) {
				for (int x = 0; x < 32; ++x) {
					lit[static_cast<std::size_t>(y) * 64 + x] = 1;
				}
			}
			const auto ml = WithLitPlane(*m, lit);
			Check(ml->litTexels[0] > 0 && ml->litTexels[0] < ml->waterTexels && ml->litMips[0].size() == ml->mips.size(), "lit plane: part of the water, full mip chain");
			ZoneGpuInput zs = z;
			zs.spread = 0.0f;
			zs.mask = ml;
			const Vec3  eastWall{ 330.0f, 0.0f, 60.0f };  // its mirror ray meets the water at x ~ 206: east, in the shadow
			const Vec3  westWall{ -150.0f, 0.0f, 50.0f };  // meets the water at x ~ -100: west, lit
			const Vec3  east{ -1.0f, 0.0f, 0.0f }, west{ 1.0f, 0.0f, 0.0f };
			zs.useLit = false;
			const float wUnshadowed = CausticBoost(PackPass(zs, cam), westWall, west, 1.0f, ml.get()).add.r;
			const float nUnshadowed = CausticBoost(PackPass(zs, cam), nearWall, inward, 1.0f, ml.get()).add.r;
			zs.useLit = true;
			Check(Near(PackPass(zs, cam).depthRange.z, 1.0f), "useLit packed");
			Check(Near(CausticBoost(PackPass(zs, cam), westWall, west, 1.0f, ml.get()).add.r, wUnshadowed), "water in the light reflects as before");
			Check(Near(CausticBoost(PackPass(zs, cam), nearWall, inward, 1.0f, ml.get()).add.r, 0.0f) && nUnshadowed > 0.0f,
				"water in the shadow does not reflect the key light", nUnshadowed, 0.0);
			(void)eastWall;
			(void)east;
			zs.extra[0] = { { 0.0f, 0.0f, 100.0f }, 0.0f, { 1.0f, 1.0f, 1.0f }, 1.0f };
			Check(CausticBoost(PackPass(zs, cam), nearWall, inward, 1.0f, ml.get()).add.r > 0.0f, "other lights still reflect off that water (their shadows are not known)");
			// the extra light with a lit plane of its own (plane 1): the same west-half shadow for it
			{
				const auto   m2 = WithLitPlane(*ml, lit, 1);
				ZoneGpuInput ze = zs;
				ze.mask = m2;
				ze.useLit = false;  // the key light reflects everywhere; only the extra is shadowed
				const float  keyOnly = [&] {
					ZoneGpuInput k = ze;
					k.extra[0].level = 0.0f;
					return CausticBoost(PackPass(k, cam), nearWall, inward, 1.0f, m2.get()).add.r;
				}();
				const float bothOpen = CausticBoost(PackPass(ze, cam), nearWall, inward, 1.0f, m2.get()).add.r;
				ze.useLitExtra[0] = true;
				Check(Near(PackPass(ze, cam).depthRange.z, 2.0f), "extra light 0's lit plane packed as bit 1", PackPass(ze, cam).depthRange.z, 2.0);
				const float extraShadowed = CausticBoost(PackPass(ze, cam), nearWall, inward, 1.0f, m2.get()).add.r;
				// shadowed, the extra still lights the wall directly (the share's denominator) but reflects nothing: below the key alone
				Check(bothOpen > keyOnly && extraShadowed < keyOnly && extraShadowed > 0.0f, "an extra light in a shadow of its own: that water does not reflect it",
					extraShadowed, keyOnly);
				ze.useLit = true;
				Check(Near(PackPass(ze, cam).depthRange.z, 3.0f), "key and extra lit planes packed together (bits 0 and 1)");
				ze.extra[0].level = 0.0f;
				Check(Near(PackPass(ze, cam).depthRange.z, 1.0f), "an unused extra slot never reads a lit plane");
				ZoneGpuInput zn = ze;
				zn.mask = ml;  // plane 1 not known
				zn.extra[0].level = 1.0f;
				Check(Near(PackPass(zn, cam).depthRange.z, 1.0f), "an extra light without its lit plane reads the water");
				Check(m2->litTexels[1] == ml->litTexels[0] && m2->litMips[0].size() == ml->litMips[0].size(), "planes are kept apart (plane 1 added, plane 0 kept)");
				// all planes in one copy: the same as one at a time, and nullptr clears
				{
					const auto both = WithLitPlanes(*m, { &lit, &lit, nullptr });
					Check(both->litTexels[0] == ml->litTexels[0] && both->litTexels[1] == ml->litTexels[0] && both->litMips[2].empty() &&
					          both->litMips[1] == m2->litMips[1],
						"WithLitPlanes: planes 0 and 1 as WithLitPlane, plane 2 cleared");
					const auto cleared = WithLitPlanes(*both, { nullptr, nullptr, nullptr });
					Check(cleared->litMips[0].empty() && cleared->litMips[1].empty() && cleared->litTexels[0] == 0, "WithLitPlanes: nullptr clears");
				}
				// a lit plane not cast yet: the light reflects nothing meanwhile (not everywhere), its direct light still counts
				{
					ZoneGpuInput zp = zs;  // key with the west-half plane, extra 0 without a plane
					zp.useLitExtra = {};
					zp.planePending[1] = true;
					Check(Near(PackPass(zp, cam).depthRange.z, 1.0f + 16.0f), "a pending extra packs as bit 4 (slot 1)", PackPass(zp, cam).depthRange.z, 17.0);
					const float pending = CausticBoost(PackPass(zp, cam), westWall, west, 1.0f, ml.get()).add.r;
					std::vector<std::uint8_t> none(64 * 64, 0);
					ZoneGpuInput zd = zs;  // the same extra, cast and reaching no water
					zd.mask = WithLitPlane(*ml, none, 1);
					zd.useLitExtra[0] = true;
					const float dark = CausticBoost(PackPass(zd, cam), westWall, west, 1.0f, zd.mask.get()).add.r;
					ZoneGpuInput zo = zs;  // ... and the old behaviour: unshadowed
					zo.useLitExtra = {};
					const float open = CausticBoost(PackPass(zo, cam), westWall, west, 1.0f, ml.get()).add.r;
					Check(Near(pending, dark, 1e-5f) && pending < open, "a pending extra reflects as one in full shadow (not as an unshadowed one)", pending, dark);
					zp.planePending = {};
					zp.planePending[0] = true;
					zp.useLit = false;
					zp.extra[0].level = 0.0f;
					Check(Near(PackPass(zp, cam).depthRange.z, 8.0f), "a pending key packs as bit 3");
					Check(Near(CausticBoost(PackPass(zp, cam), westWall, west, 1.0f, ml.get()).add.r, 0.0f), "a pending key alone reflects nothing");
					ZoneGpuInput zm = zs;  // a moving extra (no plane by design) is never pending: unshadowed as before
					zm.useLitExtra = {};
					zm.extra[0].ref = 0;
					Check(Near(CausticBoost(PackPass(zm, cam), westWall, west, 1.0f, ml.get()).add.r, open), "a moving extra stays unshadowed");
				}
			}
			ZoneGpuInput nz = zs;
			nz.mask = m;  // no lit plane
			Check(Near(PackPass(nz, cam).depthRange.z, 0.0f), "useLit without a lit plane -> ignored");
		}
		// which lights are reflected: key and extra hysteresis, extras keep their slot index, dark lights left out
		{
			using RC = RankCand;
			auto ids = [](const std::vector<RC>& c, const SlotChoice& s) {
				auto id = [&](int i) { return i < 0 ? 0ull : c[static_cast<std::size_t>(i)].id; };
				return std::array<std::uint64_t, 3>{ id(s.key), id(s.extra[0]), id(s.extra[1]) };
			};
			const float kh = 1.2f, eh = 1.2f, minRel = 0.05f;
			std::vector<RC> c = { { 2, 1, 10.0f, 10.0f }, { 2, 2, 5.0f, 5.0f }, { 2, 3, 4.9f, 4.9f }, { 2, 4, 4.8f, 4.8f } };
			auto            s = AssignReflectedSlots(c, { 2, 1 }, { SlotId{ 2, 4 }, SlotId{ 2, 3 } }, kh, eh, minRel);
			Check(ids(c, s) == std::array<std::uint64_t, 3>{ 1, 4, 3 }, "near-equal extras keep their slots (a challenger within 20 % waits)");
			c[1].weight = 6.0f;  // > 4.8 x 1.2
			s = AssignReflectedSlots(c, { 2, 1 }, { SlotId{ 2, 4 }, SlotId{ 2, 3 } }, kh, eh, minRel);
			Check(ids(c, s) == std::array<std::uint64_t, 3>{ 1, 2, 3 }, "a challenger 20 % over the weaker extra takes that slot; the other keeps its own");
			s = AssignReflectedSlots(std::vector<RC>{ c[0], c[1], c[3] }, { 2, 1 }, { SlotId{ 2, 3 }, SlotId{ 2, 4 } }, kh, eh, minRel);
			Check(s.extra[1] == 2 && s.extra[0] == 1, "an extra that left frees its slot for the best other; the staying one keeps its index");
			std::vector<RC> w = { { 2, 1, 10.0f, 10.0f }, { 2, 2, 5.0f, 0.4f } };  // bright at the water, but 4 % of the key's own strength
			s = AssignReflectedSlots(w, { 2, 1 }, { SlotId{ 2, 2 }, SlotId{} }, kh, eh, minRel);
			Check(s.extra[0] < 0, "an extra under 5 % of the key is dropped even as the incumbent");
			std::vector<RC> p = { { 2, 1, 10.0f, 10.0f }, { 2, 2, 12.5f, 12.5f }, { 2, 3, 3.0f, 3.0f } };
			s = AssignReflectedSlots(p, { 2, 1 }, { SlotId{ 2, 2 }, SlotId{ 2, 3 } }, kh, eh, minRel);
			Check(ids(p, s) == std::array<std::uint64_t, 3>{ 2, 1, 3 }, "an extra promoted to key vacates its slot and the old key takes it");
			p[1].weight = 11.9f;
			s = AssignReflectedSlots(p, { 2, 1 }, { SlotId{ 2, 2 }, SlotId{ 2, 3 } }, kh, eh, minRel);
			Check(s.key == 0, "the key keeps its place against a light within 20 %");
			std::vector<RC> d = { { 0, 9, 30.0f, 30.0f, true }, { 2, 1, 10.0f, 10.0f }, { 2, 2, 5.0f, 5.0f } };
			s = AssignReflectedSlots(d, { 0, 9 }, { SlotId{ 2, 2 }, SlotId{ 0, 9 } }, kh, eh, minRel);
			Check(ids(d, s) == std::array<std::uint64_t, 3>{ 1, 2, 0 }, "a light that reaches no water is never key or extra, even as the incumbent");
			Check(AssignReflectedSlots(std::vector<RC>{ { 2, 1, 0.0f, 1.0f } }, {}, {}, kh, eh, minRel).key < 0, "no light brings anything: no key");
			// the flicker filter's use: a torch alternating +-25 % around a lamp's weight, smoothed at tau 1 s over 250 ms
			// ticks, never takes the key from it
			float torch = 7.5f;  // first seen at its first sample (a trough), as Deflicker takes it
			int   keyChanges = 0, key = 0;
			for (int t = 0; t < 40; ++t) {
				const float raw = 10.0f * (t % 2 ? 1.25f : 0.75f);
				torch = SmoothToward(torch, raw, 0.25f, 1.0f);
				std::vector<RC> f = { { 2, 1, 10.0f, 10.0f }, { 1, 7, torch, torch } };
				const auto      r = AssignReflectedSlots(f, key == 0 ? SlotId{ 2, 1 } : SlotId{ 1, 7 }, {}, kh, eh, minRel);
				keyChanges += r.key != key;
				key = r.key;
			}
			Check(keyChanges == 0, "a flickering torch around a lamp's weight (filtered) never trades the key", keyChanges, 0);
		}
		// a key swap only relabels: the same two lights as key + extra or extra + key give the same share
		{
			ZoneGpuInput a = two;
			a.spread = 0.0f;
			a.light = { 0.0f, 0.0f, 100.0f };
			a.lightRadius = 400.0f;
			a.lightColor = { 1.0f, 0.6f, 0.3f };
			a.ambient = 0.2f;  // in A's units
			a.ambientHue = { 1.0f, 1.0f, 1.0f };
			a.extra = {};
			a.extra[0] = { { 60.0f, 40.0f, 140.0f }, 300.0f, { 0.5f, 0.7f, 1.0f }, 2.0f };  // B: twice A's strength
			ZoneGpuInput b = a;
			b.light = { 60.0f, 40.0f, 140.0f };
			b.lightRadius = 300.0f;
			b.lightColor = { 0.5f, 0.7f, 1.0f };
			b.ambient = 0.1f;  // the same ambient in B's units
			b.extra[0] = { { 0.0f, 0.0f, 100.0f }, 400.0f, { 1.0f, 0.6f, 0.3f }, 0.5f };
			// wall points round the pool, each facing the water's middle
			auto swapDiff = [&](const ZoneGpuInput& za, const ZoneGpuInput& zb, int& litPts) {
				float worst = 0.0f;
				litPts = 0;
				for (const Vec3& pt : { nearWall, Vec3{ -120.0f, 30.0f, 60.0f }, Vec3{ 80.0f, -90.0f, 40.0f }, Vec3{ 20.0f, 150.0f, 90.0f }, Vec3{ -60.0f, -140.0f, 70.0f } }) {
					const float l = std::max(std::sqrt(pt[0] * pt[0] + pt[1] * pt[1]), 1e-3f);
					const Vec3  nn{ -pt[0] / l, -pt[1] / l, 0.0f };
					const RGB   ra = CausticBoost(PackPass(za, cam), pt, nn, 1.0f, m.get()).add;
					const RGB   rb = CausticBoost(PackPass(zb, cam), pt, nn, 1.0f, m.get()).add;
					const float big = std::max({ ra.r, ra.g, ra.b });
					litPts += big > 1e-3f;
					worst = std::max({ worst, (std::fabs(ra.r - rb.r) + std::fabs(ra.g - rb.g) + std::fabs(ra.b - rb.b)) / std::max(big, 1e-3f) });
				}
				return worst;
			};
			int         lit = 0;
			const float d1 = swapDiff(a, b, lit);
			Check(lit >= 4 && d1 < 1e-3f, "a key swap between two lamps changes nothing on the walls (relative)", d1, lit);
			// the sun at a beam of 0.3 as the key (level = beam) vs a lamp as the key with the sun as an extra (beam in w)
			ZoneGpuInput sa = a;
			const Vec3   sd{ 0.5f, 0.0f, 0.8660254f };
			sa.sun = true;
			sa.light = sd;
			sa.lightRadius = 0.0f;
			sa.lightColor = { 1.0f, 0.9f, 0.8f };
			sa.level = 0.3f;
			sa.ambient = 0.1f;  // 0.3 absolute in the sun's units (3)
			sa.extra[0] = { { 0.0f, 0.0f, 100.0f }, 400.0f, { 1.0f, 0.6f, 0.3f }, 0.5f * sa.level };  // the lamp: 1.5 (x the key's level, as OutputTick hands it)
			ZoneGpuInput sb = a;
			sb.level = 1.0f;
			sb.ambient = 0.2f;  // the same 0.3 in the lamp's units
			sb.extra[0] = { sd, 0.0f, { 1.0f, 0.9f, 0.8f }, 2.0f };
			sb.extra[0].sun = true;
			sb.extra[0].beam = 0.3f;
			const float d2 = swapDiff(sa, sb, lit);
			Check(lit >= 4 && d2 < 1e-3f, "the sun (beam 0.3) and a lamp trading the key change nothing on the walls", d2, lit);
		}
		const PassConstants pc2 = PackPass(two, cam);
		Check(Near(pc2.color[1].x, 1.0f) && Near(pc2.color[1].w, 1.0f / std::max(std::min(two.level, 1.0f), 1e-4f)) && Near(pc2.color[2].x, 0.0f) && Near(pc2.color[2].w, 0.0f),
			"extra light packed into slot 1 (w = its reflected share over the key's level), slot 2 unused");
		{
			// the key at half level (the sun at a beam of 0.5): an extra lamp's reflection must not be halved with it,
			// and its direct light must not be doubled
			ZoneGpuInput h = two;
			h.extra[0] = { { 0.0f, 0.0f, 100.0f }, 0.0f, { 1.0f, 1.0f, 1.0f }, 1.0f };
			ZoneGpuInput full = h, half = h;
			half.level = 0.5f;
			full.extra[0].level *= full.level;  // as OutputTick hands the extras over
			half.extra[0].level *= half.level;
			const float bothFull = CausticBoost(PackPass(full, cam), nearWall, inward, 1.0f, m.get()).add.r;
			const float bothHalf = CausticBoost(PackPass(half, cam), nearWall, inward, 1.0f, m.get()).add.r;
			Check(Near(bothHalf, 0.75f * bothFull, 1e-4f), "key at level 0.5: only the key's reflection halves (the extra keeps its own)", bothHalf, 0.75 * bothFull);
			// with little ambient the direct term matters: the extra's direct light must not grow as the key's level drops
			full.ambient = half.ambient = 0.05f;
			const float lowFull = CausticBoost(PackPass(full, cam), nearWall, inward, 1.0f, m.get()).add.r;
			const float lowHalf = CausticBoost(PackPass(half, cam), nearWall, inward, 1.0f, m.get()).add.r;
			Check(Near(lowHalf, 0.75f * lowFull, 1e-4f), "... also where the lamps' direct light dominates the share", lowHalf, 0.75 * lowFull);
		}
	}

	// --- adaptive scan of the automatic zone's water grid ----------------------------------------
	std::printf("[scan]\n");
	{
		const auto& order = ScanOrder();
		std::vector<int> seen(64 * 64, 0);
		for (auto t : order) {
			++seen[t];
		}
		Check(order.size() == 64 * 64 && std::all_of(seen.begin(), seen.end(), [](int v) { return v == 1; }), "scan order visits every texel once");
		Check(order[0] == 0 && order[kScanCoarseCount - 1] == 60 * 64 + 60, "coarse samples come first");

		// truth: a round pool (radius 12), a channel 4 texels wide to the east edge, a 5x5 pool, one 2x2 puddle
		auto truth = [](int x, int y) {
			const int dx = x - 28, dy = y - 30;
			return dx * dx + dy * dy <= 144 || (x >= 28 && x <= 62 && y >= 40 && y <= 43) || (x >= 6 && x <= 10 && y >= 50 && y <= 54) ||
			       (x >= 45 && x <= 46 && y >= 10 && y <= 11);
		};
		auto scan = [&](std::vector<std::int8_t> st, int& rays) {
			for (int i = 0; i < 64 * 64; ++i) {
				const int x = i % 64, y = i / 64;
				if (x == 0 || y == 0 || x == 63 || y == 63) {
					st[static_cast<std::size_t>(i)] = 0;
				}
			}
			for (auto t : order) {
				if (st[t] >= 0) {
					continue;
				}
				const int         x = t % 64, y = t / 64;
				const std::int8_t v = InferTexel(st, x, y);
				if (v >= 0) {
					st[t] = v;
				} else {
					st[t] = truth(x, y) ? 1 : 0;
					++rays;
				}
			}
			return st;
		};
		int        rays = 0;
		const auto st = scan(std::vector<std::int8_t>(64 * 64, -1), rays);
		int        wrong = 0, wrongBig = 0;
		for (int y = 1; y < 63; ++y) {
			for (int x = 1; x < 63; ++x) {
				const bool w = st[static_cast<std::size_t>(y) * 64 + x] == 1;
				if (w != truth(x, y)) {
					++wrong;
					wrongBig += !(x >= 45 && x <= 46 && y >= 10 && y <= 11);
				}
			}
		}
		std::printf("  adaptive scan: %d rays instead of 3844, %d texels wrong (%d outside the 2x2 puddle)\n", rays, wrong, wrongBig);
		Check(rays < 1800, "adaptive scan casts under half the rays", rays, 1800);
		Check(wrongBig == 0, "every feature at least 4 texels wide is exact", wrongBig, 0);

		// preview after the coarse pass only
		std::vector<std::int8_t> coarse(64 * 64, -1);
		for (int i = 0; i < kScanCoarseCount; ++i) {
			const int t = order[static_cast<std::size_t>(i)];
			coarse[static_cast<std::size_t>(t)] = truth(t % 64, t / 64) ? 1 : 0;
		}
		const auto preview = GridFromStates(coarse);
		Check(preview[30 * 64 + 28] == 255 && preview[5 * 64 + 5] == 0, "coarse preview: pool centre wet, far corner dry");

		// walking 8 texels east: the overlap is reused, only the new strip is scanned
		const auto shifted = ShiftGrid(st, 8, 0);
		Check(shifted[30 * 64 + 20] == st[30 * 64 + 28] && shifted[30 * 64 + 60] == -1, "shifted grid keeps the overlap, the new strip is unknown");
		int known = 0;
		for (auto v : shifted) {
			known += v >= 0;
		}
		int  rays2 = 0;
		auto truthAt = [&](int x, int y) { return truth(x + 8, y); };
		(void)truthAt;
		std::printf("  after an 8-texel move: %d of 4096 texels reused\n", known);
		Check(known > 3000, "most of the grid is reused after a short move", known, 3000);
		(void)rays2;
	}

	std::printf("[tides]\n");
	{
		TidesField f;
		f.resolution = 64;
		f.spacing = 20.0f;
		f.originX = f.originY = -640.0f;  // cell 32 is at (0, 0)
		f.height.assign(64 * 64, 0.0f);
		f.valid.assign(64 * 64, 1.0f);
		Check(Near(TidesIntensity(f, 0.0f, 0.0f, -1000.0f), 1.0f, 1e-5f), "flat surface: intensity 1");
		auto fill = [&](float c) {
			for (int y = 0; y < 64; ++y) {
				for (int x = 0; x < 64; ++x) {
					const float wx = -640.0f + x * 20.0f, wy = -640.0f + y * 20.0f;
					f.height[static_cast<std::size_t>(y) * 64 + x] = c * (wx * wx + wy * wy);
				}
			}
		};
		fill(1e-4f);  // a trough (concave): hxx = hyy = 2e-4
		const float mirrorL = 500.0f;
		const float want = std::sqrt(1.0f + 0.0225f) / std::sqrt(0.64f * 0.64f + 0.0225f);  // det = (1 - 2 L 2c)^2 = 0.64
		const float trough = TidesIntensity(f, 0.0f, 0.0f, -2.0f * mirrorL);
		Check(Near(trough, want, 1e-3f), "a trough reflects like a concave mirror: 1 / det(I - 2 L H)", trough, want);
		Check(TidesIntensity(f, 0.0f, 0.0f, (1.0f - 1.0f / 1.33f) * 200.0f) < 1.0f, "light refracted under a trough spreads (diverging lens)");
		fill(-1e-4f);  // a crest
		Check(TidesIntensity(f, 0.0f, 0.0f, -2.0f * mirrorL) < 1.0f, "a crest reflects like a convex mirror: spreads");
		Check(TidesIntensity(f, 0.0f, 0.0f, (1.0f - 1.0f / 1.33f) * 200.0f) > 1.0f, "light refracted under a crest bunches (converging lens)");
		Check(Near(TidesHeight(f, 5000.0f, 0.0f), 0.0f), "outside the field: height 0");
		Check(Near(TidesIntensity(f, 0.0f, 0.0f, 30.0f, 30.0f, 1.0f, 0.0f), TidesIntensity(f, 0.0f, 0.0f, 30.0f), 1e-6f), "equal k1 = k2: the isotropic case");
		Check(TidesIntensity(f, 0.0f, 0.0f, 90.0f, 45.0f, 0.3f, 0.7f) > TidesIntensity(f, 0.0f, 0.0f, 30.0f), "an oblique ray is more sensitive to the ripples");
		fill(1e-4f);
		const float atFocus = TidesIntensity(f, 0.0f, 0.0f, -2.0f * 2500.0f);  // 1 - 4 c L = 0 at L = 2500
		Check(atFocus > 5.0f && std::isfinite(atFocus), "at the focus: bright but finite", atFocus, 5.0);

		// the pass: the field replaces the atlas pattern when bound at the water level
		ZoneGpuInput zt;
		zt.mask = MaskFromRect({ -200.0f, -200.0f, 200.0f, 200.0f });
		zt.rect = zt.mask->bounds;
		zt.waterZ = 0.0f;
		zt.height = 1000.0f;
		zt.distFade = 1.0f;
		zt.light = { 0.0f, 0.0f, 100.0f };
		zt.lightColor = { 1.0f, 1.0f, 1.0f };
		zt.level = 1.0f;
		zt.tides = true;
		zt.tidesSize = static_cast<float>(f.resolution);  // the constants describe this texture
		zt.tidesFine = 0.0f;
		FrameCamera cam;
		cam.invViewProj = Identity();
		const Vec3 wallP{ 150.0f, 0.0f, 50.0f }, inward{ -1.0f, 0.0f, 0.0f };
		const PassConstants pt = PackPass(zt, cam);
		Check(Near(pt.layers.w, 1.0f), "tides flag packed (fine contrast 0)");
		const float withField = CausticBoost(pt, wallP, inward, 0.9f, zt.mask.get(), &f).add.r;
		const float withField2 = CausticBoost(pt, wallP, inward, 0.1f, zt.mask.get(), &f).add.r;
		Check(Near(withField, withField2, 1e-6f), "fine contrast 0: the atlas value does not matter");
		ZoneGpuInput za = zt;
		za.tides = false;
		Check(!Near(CausticBoost(PackPass(za, cam), wallP, inward, 0.9f, zt.mask.get(), &f).add.r, withField, 1e-4f), "no tides flag: the atlas decides");
		f.rest = 50.0f;  // another water level
		Check(Near(CausticBoost(pt, wallP, inward, 0.9f, zt.mask.get(), &f).add.r, CausticBoost(pt, wallP, inward, 0.9f, zt.mask.get(), nullptr).add.r, 1e-6f),
			"a field at another water level is ignored");
	}

	std::printf("[slope]\n");
	{
		const float still = SlopeAngle(SlopeMeanSquare(0.0f, 0.0f, 0.0f, 0.0f)) * 57.29578f;
		Check(Near(still, 3.14f, 0.05f), "still water: Cox-Munk intercept, RMS slope 3.1 deg", still, 3.14);
		const float u10 = SlopeAngle(SlopeMeanSquare(0.0f, 10.0f / kWindMaxMS, 0.0f, 0.0f)) * 57.29578f;
		Check(Near(u10, 13.1f, 0.2f), "10 m/s of wind: Cox-Munk, RMS slope ~13 deg", u10, 13.1);
		const float indoor = SlopeAngle(SlopeMeanSquare(0.2f, 0.0f, 0.0f, 0.0f)) * 57.29578f;
		std::printf("  indoor pool (base 0.2): %.1f deg; with a swimmer: %.1f deg; heavy rain outdoors: %.1f deg\n", indoor,
			SlopeAngle(SlopeMeanSquare(0.2f, 0.0f, 0.0f, 1.0f)) * 57.29578f, SlopeAngle(SlopeMeanSquare(0.0f, 0.3f, 1.0f, 0.0f)) * 57.29578f);
		Check(indoor > still && indoor < 6.0f, "an indoor pool is a little rougher than still water", indoor, still);
		Check(SlopeMeanSquare(0.2f, 0.5f, 1.0f, 1.0f) > SlopeMeanSquare(0.2f, 0.5f, 0.0f, 1.0f), "rain adds slope variance");
		Check(Near(AgitationFromSlope(3.0f / 57.29578f), 0.0f) && Near(AgitationFromSlope(14.0f / 57.29578f), 1.0f), "agitation 0..1 = slope 3..14 deg");
		Check(Near(SpreadFromSlope(6.0f / 57.29578f), ReflectionSpread(3.0f / 11.0f), 1e-4f), "spread from the slope = the old agitation spread at the same angle");
	}

	std::printf("\n%d checks, %d failed -> %s\n", g_checks, g_failures, g_failures ? "FAIL" : "ALL PASS");
	return g_failures ? 1 : 0;
}
