#include "CausticMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cl::math
{
	namespace
	{

		float Smoothstep01(float t) noexcept
		{
			t = Clamp01(t);
			return t * t * (3.0f - 2.0f * t);
		}

		float WrapHour(float hour) noexcept
		{
			if (!std::isfinite(hour)) {
				return 12.0f;
			}
			float h = std::fmod(hour, 24.0f);
			if (h < 0.0f) {
				h += 24.0f;
			}
			return h;
		}
	}

	float Clamp01(float v) noexcept
	{
		if (!(v > 0.0f)) {  // also catches NaN
			return 0.0f;
		}
		return v > 1.0f ? 1.0f : v;
	}

	float Lerp(float a, float b, float t) noexcept
	{
		return a + (b - a) * t;
	}

	RGB Lerp(const RGB& a, const RGB& b, float t) noexcept
	{
		return { Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t) };
	}

	// ---- Outputs -------------------------------------------------------------------------

	LayerWeights CrossfadeWeights(float agitation) noexcept
	{
		const float a = Clamp01(agitation);
		LayerWeights w;
		if (a < 0.5f) {
			w.calm = 1.0f - 2.0f * a;
			w.normal = 2.0f * a;
			w.rough = 0.0f;
		} else {
			w.calm = 0.0f;
			w.normal = 2.0f - 2.0f * a;
			w.rough = 2.0f * a - 1.0f;
		}
		return w;
	}


	float DistanceFade(float distance, float maxDistance) noexcept
	{
		if (!(maxDistance > 0.0f)) {
			return 1.0f;
		}
		const float start = 0.8f * maxDistance;
		if (distance <= start) {
			return 1.0f;
		}
		if (!(distance < maxDistance)) {
			return 0.0f;
		}
		return 1.0f - Smoothstep01((distance - start) / (maxDistance - start));
	}

	float PlaybackSpeed(float agitation) noexcept
	{
		return 0.35f + 1.9f * Clamp01(agitation);
	}

	float SmoothToward(float current, float target, float dt, float tau) noexcept
	{
		if (!std::isfinite(current)) {
			return target;
		}
		if (dt <= 0.0f) {
			return current;
		}
		if (tau <= 0.0f) {
			return target;
		}
		const float k = 1.0f - std::exp(-dt / tau);
		return current + (target - current) * k;
	}

	RGB SmoothToward(const RGB& current, const RGB& target, float dt, float tau) noexcept
	{
		return { SmoothToward(current.r, target.r, dt, tau),
			SmoothToward(current.g, target.g, dt, tau),
			SmoothToward(current.b, target.b, dt, tau) };
	}

	// ---- Agitation inputs ------------------------------------------------------------------

	float WeatherAgitation(const WeatherSample& w) noexcept
	{
		const float precip = w.rainy ? 1.0f : (w.snowy ? 0.35f : 0.0f);
		const float cloud = w.cloudy ? 0.05f : 0.0f;
		return Clamp01(0.6f * Clamp01(w.wind01) + 0.35f * precip + cloud);
	}

	float BlendedWeatherAgitation(const WeatherSample& last, const WeatherSample& current, float transition01) noexcept
	{
		return Lerp(WeatherAgitation(last), WeatherAgitation(current), Clamp01(transition01));
	}

	float ActorImpulse(float submerge01, float speed, float distance, float range) noexcept
	{
		if (!(submerge01 > 0.01f) || !(range > 0.0f) || !(distance < range)) {
			return 0.0f;
		}
		const float depth = Clamp01(submerge01 * 2.0f);  // waist deep already counts fully
		const float motion = Clamp01(std::fabs(speed) / 300.0f);
		const float falloff = 1.0f - std::max(distance, 0.0f) / range;
		return Clamp01(depth * (0.15f + 0.6f * motion) * falloff);
	}

	float DecayImpulse(float value, float dt, float tau) noexcept
	{
		if (dt <= 0.0f) {
			return value;
		}
		if (tau <= 0.0f) {
			return 0.0f;
		}
		return value * std::exp(-dt / tau);
	}

	float TotalAgitation(float base, float weather, float impulse) noexcept
	{
		return Clamp01(Clamp01(base) + Clamp01(weather) + Clamp01(impulse));
	}

	float SlopeMeanSquare(float zoneBase, float wind01, float rain01, float impulse) noexcept
	{
		return kSlopeStill + kSlopeZoneBase * std::max(zoneBase, 0.0f) + kSlopePerWind * kWindMaxMS * Clamp01(wind01) + kSlopeRain * Clamp01(rain01) +
		       kSlopeSwimmer * Clamp01(impulse);
	}

	float SlopeAngle(float meanSquare) noexcept
	{
		return std::atan(std::sqrt(std::max(std::isfinite(meanSquare) ? meanSquare : kSlopeStill, 0.0f)));
	}

	float SpreadFromSlope(float sigma) noexcept
	{
		return std::tan(2.0f * std::clamp(sigma, 0.0f, 0.6f));
	}

	float AgitationFromSlope(float sigma) noexcept
	{
		return Clamp01((sigma * 57.29578f - 3.0f) / 11.0f);
	}

	// ---- Light inputs ------------------------------------------------------------------------

	float SunLevel(float hour) noexcept
	{
		const float h = WrapHour(hour);
		if (h < 5.0f || h > 20.0f) {
			return 0.0f;
		}
		if (h < 8.0f) {
			return Smoothstep01((h - 5.0f) / 3.0f);
		}
		if (h <= 17.0f) {
			return 1.0f;
		}
		return 1.0f - Smoothstep01((h - 17.0f) / 3.0f);
	}

	float DaylightLevel(float hour) noexcept
	{
		constexpr float kMoon = 0.15f;
		const float     sun = SunLevel(hour);
		return sun + kMoon * (1.0f - sun);
	}

	float EnbExteriorBlend(const std::array<float, 6>& v, const EnbSchedule& s, float hour) noexcept
	{
		const float h = WrapHour(hour);
		const std::array<float, 6> t{ WrapHour(s.sunriseTime - s.dawnDuration), WrapHour(s.sunriseTime), WrapHour(s.dayTime), WrapHour(s.sunsetTime),
			WrapHour(s.sunsetTime + s.duskDuration), WrapHour(s.nightTime) };
		for (std::size_t i = 0; i < 6; ++i) {
			const std::size_t j = (i + 1) % 6;
			float             span = t[j] - t[i];
			if (span <= 0.0f) {
				span += 24.0f;  // across midnight
			}
			float into = h - t[i];
			if (into < 0.0f) {
				into += 24.0f;
			}
			if (into <= span) {
				return Lerp(v[i], v[j], span > 1e-4f ? into / span : 0.0f);
			}
		}
		return v[2];  // an inconsistent schedule: the day value
	}

	float EnbDayWeight(const EnbSchedule& s, float hour) noexcept
	{
		// the exterior blend of (0 at dawn, 1 sunrise, 1 day, 1 sunset, 0 dusk, 0 night)
		return Clamp01(EnbExteriorBlend({ 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f }, s, hour));
	}

	float DirectBeamFraction(const WeatherSample& w) noexcept
	{
		if (w.rainy || w.snowy) {
			return 0.0f;
		}
		return w.cloudy ? kCloudyBeam : 1.0f;
	}

	float WeatherDimming(const WeatherSample& w) noexcept
	{
		float d = 1.0f;
		if (w.cloudy) {
			d = std::min(d, 0.7f);
		}
		if (w.snowy) {
			d = std::min(d, 0.55f);
		}
		if (w.rainy) {
			d = std::min(d, 0.45f);
		}
		return d;
	}

	std::array<float, 4> TimeOfDayWeights(float hour) noexcept
	{
		enum
		{
			kSunrise,
			kDay,
			kSunset,
			kNight
		};
		const float          h = WrapHour(hour);
		std::array<float, 4> w{ 0.0f, 0.0f, 0.0f, 0.0f };
		auto                 blend = [&](int from, int to, float t) {
            t = Smoothstep01(t);
            w[from] = 1.0f - t;
            w[to] = t;
		};

		if (h < 4.0f) {
			w[kNight] = 1.0f;
		} else if (h < 6.0f) {
			blend(kNight, kSunrise, (h - 4.0f) / 2.0f);
		} else if (h < 7.0f) {
			w[kSunrise] = 1.0f;
		} else if (h < 9.0f) {
			blend(kSunrise, kDay, (h - 7.0f) / 2.0f);
		} else if (h < 16.0f) {
			w[kDay] = 1.0f;
		} else if (h < 18.0f) {
			blend(kDay, kSunset, (h - 16.0f) / 2.0f);
		} else if (h < 19.0f) {
			w[kSunset] = 1.0f;
		} else if (h < 21.0f) {
			blend(kSunset, kNight, (h - 19.0f) / 2.0f);
		} else {
			w[kNight] = 1.0f;
		}
		return w;
	}

	Falloff CsInverseSquare(float intensity, float cutoff, float size, bool shadowLight) noexcept
	{
		constexpr float k = 0.8f * 70.0f * 70.0f;  // SCALED_UNITS_SQ
		const float     c = (cutoff >= 1.0f || !(cutoff > 0.0f)) ? (shadowLight ? 0.022f : 0.05f) : cutoff;
		const float     s = std::max(size, 0.0f);
		float           r = std::sqrt(k * (2.0f * intensity - c * s * s) / (2.0f * c));
		if (!std::isfinite(r) || !(r > 0.0f)) {
			r = 1.0f;  // CS: NaN -> 1
		}
		Falloff f;
		f.inverseSquare = 1.0f;
		f.radius = r;
		f.sizeBias = k * s * s / 2.0f;
		f.fadeZone = 1.0f / (r * std::clamp(252.0f / r, 0.0f, 1.0f));
		return f;
	}

	float Attenuation(const Falloff& f, float radius, float d) noexcept
	{
		if (f.inverseSquare > 0.5f) {
			const float t = Clamp01((f.radius - d) * f.fadeZone);
			return 3920.0f / std::max(d * d + f.sizeBias, 1.0f) * t * t * (3.0f - 2.0f * t);  // a size-0 light at d 0: not infinite
		}
		if (!(radius > 0.0f)) {
			return 1.0f;
		}
		const float e = d / (radius * kRadiusScale);
		return Clamp01(1.0f - e * e);
	}

	float LightContribution(float distance, float radius, float fade) noexcept
	{
		if (!(radius > 0.0f) || !(fade > 0.0f)) {
			return 0.0f;
		}
		const float reach = 1.5f * radius;
		const float d = std::max(distance, 0.0f);
		if (d >= reach) {
			return 0.0f;
		}
		const float x = 1.0f - d / reach;
		return fade * x * x;
	}

	LightResult FoldLight(const LightResult& base, const RGB& c, float w) noexcept
	{
		if (!(w > 0.0f)) {
			return base;
		}
		const float bt = std::max(base.total, 0.0f);
		LightResult out;
		out.total = bt + w;
		out.color = { (base.color.r * bt + c.r * w) / out.total, (base.color.g * bt + c.g * w) / out.total,
			(base.color.b * bt + c.b * w) / out.total };
		out.level = std::min(out.total, 1.0f);
		return out;
	}

	Vec3 SmoothToward(const Vec3& cur, const Vec3& target, float dt, float tau) noexcept
	{
		return { SmoothToward(cur[0], target[0], dt, tau), SmoothToward(cur[1], target[1], dt, tau),
			SmoothToward(cur[2], target[2], dt, tau) };
	}

	RippleState StepRipple(const RippleState& s, float targetAmp, bool haveActor, float tx, float ty, float dt, float decayTau,
		float posTau) noexcept
	{
		RippleState out = s;
		const float target = haveActor ? Clamp01(targetAmp) : 0.0f;
		out.amplitude = std::max(Clamp01(DecayImpulse(s.amplitude, dt, decayTau)), target);
		if (haveActor && target > 0.0f) {
			if (!s.hasPosition) {
				out.x = tx;  // first sighting: no glide from the origin
				out.y = ty;
				out.hasPosition = true;
			} else {
				out.x = SmoothToward(s.x, tx, dt, posTau);
				out.y = SmoothToward(s.y, ty, dt, posTau);
			}
		}
		return out;
	}

	CellLightingChoice CellLightingFallback(const RGB& directional, const RGB& ambient) noexcept
	{
		CellLightingChoice c;
		const float dmax = std::max({ directional.r, directional.g, directional.b });
		const float amax = std::max({ ambient.r, ambient.g, ambient.b });
		const RGB   src = dmax > 0.02f ? directional : ambient;
		const float m = dmax > 0.02f ? dmax : amax;
		c.usedDirectional = dmax > 0.02f;
		if (m <= 0.02f) {
			c.light.level = 0.0f;  // the cell itself is black: stay dark
			return c;
		}
		c.light.color = { src.r / m, src.g / m, src.b / m };
		c.light.level = kCellLightingLevel;
		c.light.total = kCellLightingLevel;
		return c;
	}


	LightResult CombineLights(std::span<const LightSample> lights) noexcept
	{
		LightResult out;
		float       total = 0.0f;
		RGB         sum{};
		for (const auto& l : lights) {
			const float w = LightContribution(l.distance, l.radius, l.fade);
			if (w <= 0.0f) {
				continue;
			}
			total += w;
			sum.r += l.color.r * w;
			sum.g += l.color.g * w;
			sum.b += l.color.b * w;
		}
		if (total <= 0.0f) {
			return out;  // white, level 0 -> caustic fades out
		}
		out.color = { sum.r / total, sum.g / total, sum.b / total };
		out.level = std::min(total, 1.0f);
		out.total = total;
		return out;
	}

	// ---- Frame clock -------------------------------------------------------------------------

	float AdvanceFrameClock(float clock, float dt, float speed) noexcept
	{
		if (!std::isfinite(clock) || clock < 0.0f) {
			clock = 0.0f;
		}
		const float step = (dt > 0.0f && std::isfinite(dt) ? dt : 0.0f) * kAtlasFps * std::max(speed, 0.0f);
		float       c = std::fmod(clock + step, kAtlasFrames);
		if (c < 0.0f || !std::isfinite(c)) {
			c = 0.0f;
		}
		return c;
	}

	// ---- Water zones -------------------------------------------------------------------------

	std::optional<Rect> RectFromCorners(float ax, float ay, float bx, float by, float minSide) noexcept
	{
		Rect r{ std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by) };
		if (!(r.maxX - r.minX >= minSide) || !(r.maxY - r.minY >= minSide)) {
			return std::nullopt;
		}
		return r;
	}

	bool RectContains(const Rect& r, float x, float y) noexcept
	{
		return x >= r.minX && x <= r.maxX && y >= r.minY && y <= r.maxY;
	}

	bool RectsOverlap(const Rect& a, const Rect& b) noexcept
	{
		return a.minX < b.maxX && b.minX < a.maxX && a.minY < b.maxY && b.minY < a.maxY;
	}

	float DistanceToRect2D(const Rect& r, float x, float y) noexcept
	{
		const float dx = std::max({ r.minX - x, 0.0f, x - r.maxX });
		const float dy = std::max({ r.minY - y, 0.0f, y - r.maxY });
		return std::sqrt(dx * dx + dy * dy);
	}

	float DistanceToZone(const Rect& r, float waterZ, float height, const Vec3& p) noexcept
	{
		const float dxy = DistanceToRect2D(r, p[0], p[1]);
		const float top = waterZ + std::max(height, 0.0f);
		const float dz = std::max({ waterZ - p[2], 0.0f, p[2] - top });
		return std::sqrt(dxy * dxy + dz * dz);
	}

	float PixelFade(const PassConstants& c, const Vec3& p) noexcept
	{
		const float dx = p[0] - c.posAdjust.x, dy = p[1] - c.posAdjust.y, dz = p[2] - c.posAdjust.z;
		return c.fade.x > 0.0f ? DistanceFade(std::sqrt(dx * dx + dy * dy + dz * dz), c.fade.x) : 1.0f;
	}

	bool IsZoneActive(bool cellAttached, float distance, float maxDistance) noexcept
	{
		return cellAttached && distance < maxDistance;
	}


	// ---- Camera and screen ---------------------------------------------------------------------

	Mat4 Identity() noexcept
	{
		Mat4 r;
		for (int i = 0; i < 4; ++i) {
			r.at(i, i) = 1.0f;
		}
		return r;
	}

	Mat4 Mul(const Mat4& a, const Mat4& b) noexcept
	{
		Mat4 r;
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				double s = 0.0;
				for (int k = 0; k < 4; ++k) {
					s += static_cast<double>(a.at(i, k)) * static_cast<double>(b.at(k, j));
				}
				r.at(i, j) = static_cast<float>(s);
			}
		}
		return r;
	}

	std::optional<Mat4> Inverse(const Mat4& m) noexcept
	{
		// Gauss-Jordan with partial pivoting, in double (a projection with a far plane in the
		// hundreds of thousands is badly conditioned in float).
		double a[4][8];
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				a[i][j] = m.at(i, j);
				a[i][j + 4] = i == j ? 1.0 : 0.0;
			}
		}
		for (int c = 0; c < 4; ++c) {
			int    piv = c;
			double best = std::fabs(a[c][c]);
			for (int r = c + 1; r < 4; ++r) {
				if (std::fabs(a[r][c]) > best) {
					best = std::fabs(a[r][c]);
					piv = r;
				}
			}
			if (!(best > 1e-30)) {
				return std::nullopt;
			}
			if (piv != c) {
				for (int j = 0; j < 8; ++j) {
					std::swap(a[c][j], a[piv][j]);
				}
			}
			const double inv = 1.0 / a[c][c];
			for (int j = 0; j < 8; ++j) {
				a[c][j] *= inv;
			}
			for (int r = 0; r < 4; ++r) {
				if (r == c) {
					continue;
				}
				const double f = a[r][c];
				if (f != 0.0) {
					for (int j = 0; j < 8; ++j) {
						a[r][j] -= f * a[c][j];
					}
				}
			}
		}
		Mat4 out;
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				const double v = a[i][j + 4];
				if (!std::isfinite(v)) {
					return std::nullopt;
				}
				out.at(i, j) = static_cast<float>(v);
			}
		}
		return out;
	}

	Vec4 TransformRow(const Vec4& v, const Mat4& m) noexcept
	{
		Vec4 r{};
		for (int j = 0; j < 4; ++j) {
			r[j] = v[0] * m.at(0, j) + v[1] * m.at(1, j) + v[2] * m.at(2, j) + v[3] * m.at(3, j);
		}
		return r;
	}

	ScreenPoint ProjectToPixel(const Vec3& rel, const Viewport& vp, const Mat4& viewProj) noexcept
	{
		const Vec4 c = TransformRow({ rel[0], rel[1], rel[2], 1.0f }, viewProj);
		ScreenPoint s;
		s.w = c[3];
		if (!(std::fabs(c[3]) > 1e-12f)) {
			return s;
		}
		const float nx = c[0] / c[3], ny = c[1] / c[3], nz = c[2] / c[3];
		s.x = vp.x + (nx * 0.5f + 0.5f) * vp.width;
		s.y = vp.y + (0.5f - ny * 0.5f) * vp.height;  // NDC y up, pixels y down
		s.depth = vp.minDepth + nz * (vp.maxDepth - vp.minDepth);
		return s;
	}

	std::optional<Vec3> UnprojectPixel(float px, float py, float depth, const Viewport& vp, const Mat4& inv) noexcept
	{
		const float range = vp.maxDepth - vp.minDepth;
		if (!(vp.width > 0.0f) || !(vp.height > 0.0f) || !(std::fabs(range) > 1e-12f)) {
			return std::nullopt;
		}
		const float nx = (px - vp.x) / vp.width * 2.0f - 1.0f;
		const float ny = 1.0f - (py - vp.y) / vp.height * 2.0f;
		const float nz = (depth - vp.minDepth) / range;
		const Vec4  h = TransformRow({ nx, ny, nz, 1.0f }, inv);
		if (!(std::fabs(h[3]) > 1e-12f)) {
			return std::nullopt;
		}
		return Vec3{ h[0] / h[3], h[1] / h[3], h[2] / h[3] };
	}

	std::optional<PixelRect> ScissorForZone(const Rect& r, float waterZ, float height, float reach, const Vec3& adj, const Mat4& viewProj,
		const Viewport& vp, float margin) noexcept
	{
		// D3D11 viewports live in [-32768, 32767]; anything else is not a real viewport.
		constexpr float kLimit = 32767.0f;
		if (!(vp.width > 0.0f) || !(vp.height > 0.0f) || !std::isfinite(vp.x) || !std::isfinite(vp.y) || !(vp.x >= -kLimit) ||
			!(vp.y >= -kLimit) || !(vp.x + vp.width <= kLimit) || !(vp.y + vp.height <= kLimit)) {
			return std::nullopt;
		}
		const float     fl = std::floor(vp.x), ft = std::floor(vp.y);
		const float     fr = std::ceil(vp.x + vp.width), fb = std::ceil(vp.y + vp.height);
		const PixelRect full{ static_cast<int>(fl), static_cast<int>(ft), static_cast<int>(fr), static_cast<int>(fb) };

		const float xs[2] = { r.minX - reach, r.maxX + reach };
		const float ys[2] = { r.minY - reach, r.maxY + reach };
		const float zs[2] = { waterZ, waterZ + std::max(height, 0.0f) };

		int   behind = 0;  // w <= 0: behind the camera plane
		int   nearCount = 0;  // 0 < w <= kNearW: on or almost on the camera plane
		bool  bad = false;
		float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
		bool  first = true;
		for (float x : xs) {
			for (float y : ys) {
				for (float z : zs) {
					const ScreenPoint s = ProjectToPixel({ x - adj[0], y - adj[1], z - adj[2] }, vp, viewProj);
					if (!std::isfinite(s.w)) {
						bad = true;
						continue;
					}
					if (!(s.w > 0.0f)) {
						++behind;
						continue;
					}
					if (!(s.w > kNearW)) {
						++nearCount;
						continue;
					}
					if (!std::isfinite(s.x) || !std::isfinite(s.y)) {
						bad = true;
						continue;
					}
					minX = first ? s.x : std::min(minX, s.x);
					maxX = first ? s.x : std::max(maxX, s.x);
					minY = first ? s.y : std::min(minY, s.y);
					maxY = first ? s.y : std::max(maxY, s.y);
					first = false;
				}
			}
		}
		if (behind == 8) {
			return std::nullopt;  // the whole (convex) box is behind the camera: nothing of it can be on screen
		}
		if (behind > 0 || nearCount > 0 || bad) {
			return full;  // camera inside the box or a corner on the camera plane: projected corners are meaningless
		}
		// clamp in float before converting (no overflow for corners projected far off-screen)
		const float l = std::clamp(std::floor(minX - margin), fl, fr);
		const float t = std::clamp(std::floor(minY - margin), ft, fb);
		const float rr = std::clamp(std::ceil(maxX + margin), fl, fr);
		const float bb = std::clamp(std::ceil(maxY + margin), ft, fb);
		if (!(rr > l) || !(bb > t)) {
			return std::nullopt;  // entirely off-screen
		}
		return PixelRect{ static_cast<int>(l), static_cast<int>(t), static_cast<int>(rr), static_cast<int>(bb) };
	}

	// ---- Water mask --------------------------------------------------------------------------

	namespace
	{
		std::vector<std::vector<std::uint8_t>> PlaneMips(int size, std::vector<std::uint8_t> level0)
		{
			std::vector<std::vector<std::uint8_t>> mips;
			mips.push_back(std::move(level0));
			for (int s = size; s > 1; s /= 2) {
				const auto&               src = mips.back();
				const int                 d = s / 2;
				std::vector<std::uint8_t> dst(static_cast<std::size_t>(d) * d);
				for (int y = 0; y < d; ++y) {
					for (int x = 0; x < d; ++x) {
						const int sum = src[(2 * y) * s + 2 * x] + src[(2 * y) * s + 2 * x + 1] + src[(2 * y + 1) * s + 2 * x] +
						                src[(2 * y + 1) * s + 2 * x + 1];
						dst[static_cast<std::size_t>(y) * d + x] = static_cast<std::uint8_t>((sum + 2) / 4);
					}
				}
				mips.push_back(std::move(dst));
			}
			return mips;
		}
	}

	namespace
	{
		void SetLitPlane(WaterMask& out, const std::vector<std::uint8_t>* lit, std::size_t k)
		{
			if (!lit || out.mips.empty() || lit->size() != out.mips[0].size()) {
				out.litMips[k].clear();
				out.litTexels[k] = 0;
				return;
			}
			std::vector<std::uint8_t> l0(lit->size());
			int                       n = 0;
			for (std::size_t i = 0; i < lit->size(); ++i) {
				l0[i] = ((*lit)[i] && out.mips[0][i] >= 128) ? 255 : 0;
				n += l0[i] ? 1 : 0;
			}
			out.litTexels[k] = n;
			out.litMips[k] = PlaneMips(out.size, std::move(l0));
		}
	}

	std::shared_ptr<const WaterMask> WithLitPlane(const WaterMask& m, const std::vector<std::uint8_t>& lit, int plane)
	{
		auto out = std::make_shared<WaterMask>(m);
		SetLitPlane(*out, &lit, static_cast<std::size_t>(std::clamp(plane, 0, 2)));
		return out;
	}

	std::shared_ptr<const WaterMask> WithLitPlanes(const WaterMask& m, const std::array<const std::vector<std::uint8_t>*, 3>& lit)
	{
		auto out = std::make_shared<WaterMask>(m);
		for (std::size_t k = 0; k < 3; ++k) {
			SetLitPlane(*out, lit[k], k);
		}
		return out;
	}

	SlotChoice AssignReflectedSlots(std::span<const RankCand> c, SlotId prevKey, const std::array<SlotId, 2>& prevExtra, float keyHysteresis,
		float extraHysteresis, float minExtraRel)
	{
		SlotChoice out;
		const int  n = static_cast<int>(c.size());
		auto       usable = [&](int i) { return !c[i].dark && c[i].weight > 0.0f; };
		auto       same = [&](int i, const SlotId& s) { return s.kind == c[i].kind && s.id == c[i].id; };
		int        best = -1, prev = -1;
		for (int i = 0; i < n; ++i) {
			if (!usable(i)) {
				continue;
			}
			if (best < 0 || c[i].weight > c[best].weight) {
				best = i;  // the first of equals (the caller's order breaks ties)
			}
			if (prev < 0 && same(i, prevKey)) {
				prev = i;
			}
		}
		if (best < 0) {
			return out;
		}
		out.key = (prev >= 0 && c[best].weight < c[prev].weight * keyHysteresis) ? prev : best;
		const float keyI = std::max(c[out.key].intensity, 1e-4f);
		auto        taken = [&](int i) { return i == out.key || out.extra[0] == i || out.extra[1] == i; };
		auto        qualifies = [&](int i) { return usable(i) && i != out.key && c[i].intensity / keyI >= minExtraRel; };
		for (std::size_t s = 0; s < 2; ++s) {  // the extras that still qualify keep their slots
			for (int i = 0; i < n; ++i) {
				if (same(i, prevExtra[s]) && qualifies(i) && !taken(i)) {
					out.extra[s] = i;
					break;
				}
			}
		}
		std::vector<int> order;
		for (int i = 0; i < n; ++i) {
			if (qualifies(i) && !taken(i)) {
				order.push_back(i);
			}
		}
		std::ranges::stable_sort(order, [&](int a, int b) { return c[a].weight > c[b].weight; });
		for (const int ch : order) {
			int slot = out.extra[0] < 0 ? 0 : (out.extra[1] < 0 ? 1 : -1);  // an empty slot: taken at once
			if (slot < 0) {
				const int weak = c[out.extra[0]].weight <= c[out.extra[1]].weight ? 0 : 1;
				if (c[ch].weight > c[out.extra[weak]].weight * extraHysteresis) {
					slot = weak;  // more than the hysteresis over the weaker extra: it takes that slot
				}
			}
			if (slot >= 0) {
				out.extra[static_cast<std::size_t>(slot)] = ch;
			}
		}
		return out;
	}

	void BuildMaskMips(WaterMask& m)
	{
		m.mips.resize(1);
		int count = 0, x0 = m.size, y0 = m.size, x1 = -1, y1 = -1;
		for (int y = 0; y < m.size; ++y) {
			for (int x = 0; x < m.size; ++x) {
				if (m.mips[0][static_cast<std::size_t>(y) * m.size + x] >= 128) {
					++count;
					x0 = std::min(x0, x);
					y0 = std::min(y0, y);
					x1 = std::max(x1, x);
					y1 = std::max(y1, y);
				}
			}
		}
		m.waterTexels = count;
		const float tx = (m.bounds.maxX - m.bounds.minX) / m.size, ty = (m.bounds.maxY - m.bounds.minY) / m.size;
		m.waterBounds = count ? Rect{ m.bounds.minX + x0 * tx, m.bounds.minY + y0 * ty, m.bounds.minX + (x1 + 1) * tx, m.bounds.minY + (y1 + 1) * ty } :
		                        Rect{};
		for (int s = m.size; s > 1; s /= 2) {
			const auto& src = m.mips.back();
			const int   d = s / 2;
			std::vector<std::uint8_t> dst(static_cast<std::size_t>(d) * d);
			for (int y = 0; y < d; ++y) {
				for (int x = 0; x < d; ++x) {
					const int sum = src[(2 * y) * s + 2 * x] + src[(2 * y) * s + 2 * x + 1] + src[(2 * y + 1) * s + 2 * x] +
					                src[(2 * y + 1) * s + 2 * x + 1];
					dst[static_cast<std::size_t>(y) * d + x] = static_cast<std::uint8_t>((sum + 2) / 4);
				}
			}
			m.mips.push_back(std::move(dst));
		}
	}

	std::shared_ptr<const WaterMask> MaskFromRect(const Rect& r, int size)
	{
		size = kMaskSize;  // the shader assumes kMaskSize
		auto        m = std::make_shared<WaterMask>();
		const float gx = std::max((r.maxX - r.minX) / 6.0f, 16.0f);  // the rect covers 3/4 of the texture
		const float gy = std::max((r.maxY - r.minY) / 6.0f, 16.0f);
		m->bounds = { r.minX - gx, r.minY - gy, r.maxX + gx, r.maxY + gy };
		m->size = size;
		m->mips.assign(1, std::vector<std::uint8_t>(static_cast<std::size_t>(size) * size, 0));
		const float tx = (m->bounds.maxX - m->bounds.minX) / size, ty = (m->bounds.maxY - m->bounds.minY) / size;
		for (int y = 1; y < size - 1; ++y) {
			for (int x = 1; x < size - 1; ++x) {
				const float cx = m->bounds.minX + (x + 0.5f) * tx, cy = m->bounds.minY + (y + 0.5f) * ty;
				if (RectContains(r, cx, cy)) {
					m->mips[0][static_cast<std::size_t>(y) * size + x] = 255;
				}
			}
		}
		BuildMaskMips(*m);
		return m;
	}

	std::shared_ptr<const WaterMask> MaskFromGrid(const Rect& bounds, int size, std::vector<std::uint8_t> water)
	{
		if (size != kMaskSize || water.size() != static_cast<std::size_t>(size) * size) {  // the shader assumes kMaskSize
			return nullptr;
		}
		auto m = std::make_shared<WaterMask>();
		m->bounds = bounds;
		m->size = size;
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				auto& v = water[static_cast<std::size_t>(y) * size + x];
				v = (x == 0 || y == 0 || x == size - 1 || y == size - 1 || v == 0) ? 0 : 255;
			}
		}
		m->mips.assign(1, std::move(water));
		BuildMaskMips(*m);
		if (m->waterTexels == 0) {
			return nullptr;
		}
		return m;
	}

	namespace
	{
		float Bilinear(const std::vector<std::uint8_t>& t, int s, float u, float v)
		{
			const float x = u * s - 0.5f, y = v * s - 0.5f;
			const float fx0 = std::floor(x), fy0 = std::floor(y);
			const float fx = x - fx0, fy = y - fy0;
			auto        at = [&](int ix, int iy) {
                ix = std::clamp(ix, 0, s - 1);
                iy = std::clamp(iy, 0, s - 1);
                return t[static_cast<std::size_t>(iy) * s + ix] / 255.0f;
			};
			const int ix = static_cast<int>(fx0), iy = static_cast<int>(fy0);
			const float a = Lerp(at(ix, iy), at(ix + 1, iy), fx);
			const float b = Lerp(at(ix, iy + 1), at(ix + 1, iy + 1), fx);
			return Lerp(a, b, fy);
		}
	}

	float SampleMask(const WaterMask& m, float u, float v, float lod, int plane) noexcept
	{
		const bool  lit = plane >= 0 && plane < 3 && !m.litMips[static_cast<std::size_t>(plane)].empty();
		const auto& mips = lit ? m.litMips[static_cast<std::size_t>(plane)] : m.mips;
		if (mips.empty() || !std::isfinite(u) || !std::isfinite(v)) {
			return 0.0f;
		}
		u = std::clamp(u, 0.0f, 1.0f);
		v = std::clamp(v, 0.0f, 1.0f);
		const float top = static_cast<float>(mips.size() - 1);
		const float l = std::clamp(std::isfinite(lod) ? lod : 0.0f, 0.0f, top);
		const int   l0 = static_cast<int>(std::floor(l));
		const int   l1 = std::min(l0 + 1, static_cast<int>(top));
		const float a = Bilinear(mips[l0], m.size >> l0, u, v);
		const float b = Bilinear(mips[l1], m.size >> l1, u, v);
		return Lerp(a, b, l - static_cast<float>(l0));
	}

	const std::vector<std::uint16_t>& ScanOrder()
	{
		static const std::vector<std::uint16_t> order = [] {
			std::vector<std::uint16_t> o;
			o.reserve(static_cast<std::size_t>(kMaskSize) * kMaskSize);
			auto add = [&](int step, int skipStep) {
				for (int y = 0; y < kMaskSize; y += step) {
					for (int x = 0; x < kMaskSize; x += step) {
						if (skipStep && x % skipStep == 0 && y % skipStep == 0) {
							continue;
						}
						o.push_back(static_cast<std::uint16_t>(y * kMaskSize + x));
					}
				}
			};
			add(4, 0);  // coarse
			add(2, 4);  // the even texels between them
			add(1, 2);  // the rest
			return o;
		}();
		return order;
	}

	std::int8_t InferTexel(const std::vector<std::int8_t>& st, int ix, int iy) noexcept
	{
		if (st.size() != static_cast<std::size_t>(kMaskSize) * kMaskSize || ix < 0 || iy < 0 || ix >= kMaskSize || iy >= kMaskSize) {
			return -1;
		}
		if (ix % kScanCoarseStep == 0 && iy % kScanCoarseStep == 0) {
			return -1;  // a coarse sample: always cast
		}
		const int p = (ix % 2 == 0 && iy % 2 == 0) ? 4 : 2;  // parents one level coarser
		const int x0 = ix / p * p, y0 = iy / p * p;
		const int x1 = ix % p ? x0 + p : x0, y1 = iy % p ? y0 + p : y0;
		if (x1 >= kMaskSize || y1 >= kMaskSize) {
			return -1;
		}
		// the parents and one more ring of samples at the same spacing must all agree: a shore that bulges
		// between two samples of one kind is then still caught by a neighbour of the other kind
		const std::int8_t first = st[static_cast<std::size_t>(y0) * kMaskSize + x0];
		if (first < 0) {
			return -1;
		}
		for (int y = std::max(y0 - p, 0); y <= std::min(y1 + p, kMaskSize - 1); y += p) {
			for (int x = std::max(x0 - p, 0); x <= std::min(x1 + p, kMaskSize - 1); x += p) {
				if (x % p || y % p) {
					continue;
				}
				if (st[static_cast<std::size_t>(y) * kMaskSize + x] != first) {
					return -1;
				}
			}
		}
		return first;
	}

	std::vector<std::int8_t> ShiftGrid(const std::vector<std::int8_t>& old, int dx, int dy)
	{
		std::vector<std::int8_t> out(static_cast<std::size_t>(kMaskSize) * kMaskSize, -1);
		if (old.size() != out.size()) {
			return out;
		}
		for (int y = 0; y < kMaskSize; ++y) {
			for (int x = 0; x < kMaskSize; ++x) {
				const int ox = x + dx, oy = y + dy;
				if (ox <= 0 || oy <= 0 || ox >= kMaskSize - 1 || oy >= kMaskSize - 1) {
					continue;  // outside the old grid or on its (never cast) border ring
				}
				out[static_cast<std::size_t>(y) * kMaskSize + x] = old[static_cast<std::size_t>(oy) * kMaskSize + ox];
			}
		}
		return out;
	}

	std::vector<std::uint8_t> GridFromStates(const std::vector<std::int8_t>& st)
	{
		std::vector<std::uint8_t> g(static_cast<std::size_t>(kMaskSize) * kMaskSize, 0);
		if (st.size() != g.size()) {
			return g;
		}
		for (int y = 0; y < kMaskSize; ++y) {
			for (int x = 0; x < kMaskSize; ++x) {
				std::int8_t v = st[static_cast<std::size_t>(y) * kMaskSize + x];
				if (v < 0) {
					const int cx = std::min((x + 2) / 4 * 4, kMaskSize - 4), cy = std::min((y + 2) / 4 * 4, kMaskSize - 4);
					v = st[static_cast<std::size_t>(cy) * kMaskSize + cx];
				}
				g[static_cast<std::size_t>(y) * kMaskSize + x] = v > 0 ? 255 : 0;
			}
		}
		return g;
	}

	float BottomFocus(float spread) noexcept
	{
		const float t = std::tan(0.5f * std::atan(std::max(spread, 0.0f)));  // tan(sigma) from tan(2 sigma)
		return 0.641f * kPatternTile / std::max(t, 1e-3f);
	}

	float BottomContrastAt(float depth, float spread) noexcept
	{
		return std::min(kBottomContrast * Clamp01(depth / BottomFocus(spread)), kBottomMaxContrast);
	}

	float TidesHeight(const TidesField& f, float x, float y) noexcept
	{
		const int n = f.resolution;
		if (n < 2 || f.height.size() != static_cast<std::size_t>(n) * n || f.valid.size() != f.height.size() || !(f.spacing > 0.0f)) {
			return 0.0f;
		}
		const float gx = (x - f.originX) / f.spacing, gy = (y - f.originY) / f.spacing;
		const float edge = std::min({ gx, gy, n - 1 - gx, n - 1 - gy });
		if (!(edge >= 0.0f)) {
			return 0.0f;
		}
		const float e = Clamp01(edge / std::max(1.0f, f.edgeCells));
		const float fade = e * e * (3.0f - 2.0f * e) * f.enabled;
		// bilinear on cell centres: uv = (g + 0.5) / n, texel i centre at (i + 0.5) / n -> index space = g
		const float fx0 = std::floor(gx), fy0 = std::floor(gy), tx = gx - fx0, ty = gy - fy0;
		auto        at = [&](int ix, int iy) {
            ix = std::clamp(ix, 0, n - 1);
            iy = std::clamp(iy, 0, n - 1);
            const std::size_t i = static_cast<std::size_t>(iy) * n + ix;
            return std::pair{ f.height[i], f.valid[i] };
		};
		const int ix = static_cast<int>(fx0), iy = static_cast<int>(fy0);
		const auto [h00, v00] = at(ix, iy);
		const auto [h10, v10] = at(ix + 1, iy);
		const auto [h01, v01] = at(ix, iy + 1);
		const auto [h11, v11] = at(ix + 1, iy + 1);
		// as the shader: the product height x valid per cell, then bilinear
		const float hv = Lerp(Lerp(h00 * v00, h10 * v10, tx), Lerp(h01 * v01, h11 * v11, tx), ty);
		return hv * f.displacement * fade;
	}

	float TidesIntensity(const TidesField& f, float x, float y, float k) noexcept
	{
		return TidesIntensity(f, x, y, k, k, 1.0f, 0.0f);
	}

	namespace
	{
		// Height x valid x edge fade of cell (ix, iy), as the shader's TidesNodeHeight.
		float NodeHeight(const TidesField& f, int ix, int iy) noexcept
		{
			const int   n = f.resolution;
			const int   cx = std::clamp(ix, 0, n - 1), cy = std::clamp(iy, 0, n - 1);
			const float edge = std::min({ static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(n - 1 - ix), static_cast<float>(n - 1 - iy) });
			const float e = Clamp01(edge / std::max(1.0f, f.edgeCells));
			const float fade = (edge >= 0.0f ? 1.0f : 0.0f) * e * e * (3.0f - 2.0f * e) * f.enabled;
			const std::size_t i = static_cast<std::size_t>(cy) * n + cx;
			return f.height[i] * f.valid[i] * f.displacement * fade;
		}

		// The cell's second derivatives, as the shader's CurvaturePS.
		std::array<float, 3> NodeCurvature(const TidesField& f, int ix, int iy) noexcept
		{
			const float s = f.spacing, hc = NodeHeight(f, ix, iy);
			return { (NodeHeight(f, ix + 1, iy) - 2.0f * hc + NodeHeight(f, ix - 1, iy)) / (s * s),
				(NodeHeight(f, ix, iy + 1) - 2.0f * hc + NodeHeight(f, ix, iy - 1)) / (s * s),
				(NodeHeight(f, ix + 1, iy + 1) - NodeHeight(f, ix + 1, iy - 1) - NodeHeight(f, ix - 1, iy + 1) + NodeHeight(f, ix - 1, iy - 1)) / (4.0f * s * s) };
		}
	}

	float TidesIntensity(const TidesField& f, float x, float y, float k1, float k2, float e1x, float e1y) noexcept
	{
		const int n = f.resolution;
		if (n < 2 || f.height.size() != static_cast<std::size_t>(n) * n || f.valid.size() != f.height.size() || !(f.spacing > 0.0f)) {
			return 1.0f;
		}
		// bilinear (clamped) of the cells' curvature, as the shader samples its curvature map
		const float gx = (x - f.originX) / f.spacing, gy = (y - f.originY) / f.spacing;
		const float fx0 = std::floor(gx), fy0 = std::floor(gy), tx = gx - fx0, ty = gy - fy0;
		const int   ix = static_cast<int>(fx0), iy = static_cast<int>(fy0);
		auto        at = [&](int a, int b) { return NodeCurvature(f, std::clamp(a, 0, n - 1), std::clamp(b, 0, n - 1)); };
		const auto  c00 = at(ix, iy), c10 = at(ix + 1, iy), c01 = at(ix, iy + 1), c11 = at(ix + 1, iy + 1);
		auto        bl = [&](int k) { return Lerp(Lerp(c00[k], c10[k], tx), Lerp(c01[k], c11[k], tx), ty); };
		const float hxx = bl(0), hyy = bl(1), hxy = bl(2);
		// the curvature in the light's frame: e1 along its horizontal direction, e2 across
		const float el = std::sqrt(e1x * e1x + e1y * e1y);
		const float cx = el > 1e-6f ? e1x / el : 1.0f, cy = el > 1e-6f ? e1y / el : 0.0f;
		const float h11 = cx * cx * hxx + 2.0f * cx * cy * hxy + cy * cy * hyy;
		const float h22 = cy * cy * hxx - 2.0f * cx * cy * hxy + cx * cx * hyy;
		const float h12 = (cy * cx) * (hyy - hxx) + (cx * cx - cy * cy) * hxy;
		const float det = (1.0f + k1 * h11) * (1.0f + k2 * h22) - k1 * k2 * h12 * h12;
		constexpr float e = kTidesFocusSoftness;
		const float i = std::sqrt(1.0f + e * e) / std::sqrt(det * det + e * e);
		return std::isfinite(i) ? i : 1.0f;
	}

	float ReflectionSpread(float agitation) noexcept
	{
		const float sigmaDeg = Lerp(3.0f, 14.0f, Clamp01(agitation));
		return std::tan(2.0f * sigmaDeg * 3.14159265f / 180.0f);
	}

	// ---- Caustic pass ------------------------------------------------------------------------

	PassConstants PackPass(const ZoneGpuInput& in, const FrameCamera& cam) noexcept
	{
		PassConstants c{};
		std::memcpy(c.invViewProj, cam.invViewProj.m, sizeof(c.invViewProj));
		c.posAdjust = { cam.posAdjust[0], cam.posAdjust[1], cam.posAdjust[2], 0.0f };
		c.viewport = { cam.viewport.x, cam.viewport.y, cam.viewport.width, cam.viewport.height };
		// which lights read their own lit plane: bit i = light i (the shader decodes the float)
		unsigned litBits = (in.useLit && in.mask && !in.mask->litMips[0].empty()) ? 1u : 0u;
		for (std::size_t e = 0; e < 2; ++e) {
			if (in.useLitExtra[e] && in.mask && !in.mask->litMips[e + 1].empty() && in.extra[e].level > 0.0f) {
				litBits |= 2u << e;
			}
		}
		const float floorStrength = (std::isfinite(in.floorScale) ? std::clamp(in.floorScale, 0.0f, 5.0f) : 1.0f) *
		                            (std::isfinite(in.strength) ? std::clamp(in.strength, 0.0f, kMaxStrength) : 1.0f) / kDefaultStrength *
		                            std::max(in.brightness, 0.0f) * Clamp01(in.level);
		for (std::size_t i = 0; i < 3; ++i) {
			if (in.planePending[i]) {
				litBits |= 8u << i;  // bits 3..5: the light's plane is not cast yet (it reflects nothing)
			}
		}
		c.depthRange = { cam.viewport.minDepth, cam.viewport.maxDepth, static_cast<float>(litBits), floorStrength };
		const Rect& gr = in.mask ? in.mask->bounds : in.rect;  // mask texture space
		c.rect = { gr.minX, gr.minY, gr.maxX, gr.maxY };
		const float spread = std::isfinite(in.spread) ? std::clamp(in.spread, 0.0f, 2.0f) : 0.0f;
		c.water = { in.waterZ, std::max(in.height, 1.0f), spread, Clamp01(in.distFade) };
		const float b = std::max(in.brightness, 0.0f);
		const float k = 1.0f;  // the key light's own colour; its level scales the result (params.w), not the share
		if (in.sun) {  // a unit vector toward the sun, w = -1 (the smoothed direction is no longer unit length)
			const float l = std::sqrt(in.light[0] * in.light[0] + in.light[1] * in.light[1] + in.light[2] * in.light[2]);
			c.light[0] = l > 1e-6f ? Float4{ in.light[0] / l, in.light[1] / l, in.light[2] / l, -1.0f } : Float4{ 0.0f, 0.0f, -1.0f, -1.0f };
		} else {
			c.light[0] = { in.light[0], in.light[1], in.light[2], std::max(in.lightRadius, 0.0f) };
		}
		c.color[0] = { in.lightColor.r * k, in.lightColor.g * k, in.lightColor.b * k, kMaxBoost };
		auto packFalloff = [](const Falloff& f) {
			return f.inverseSquare > 0.5f ? Float4{ 1.0f, std::max(f.sizeBias, 0.0f), std::max(f.fadeZone, 0.0f), std::max(f.radius, 0.0f) } : Float4{};
		};
		c.falloff[0] = in.sun ? Float4{} : packFalloff(in.keyFalloff);
		for (std::size_t i = 0; i < in.extra.size(); ++i) {
			const auto& e = in.extra[i];
			const float ke = std::max(e.level, 0.0f) / std::max(in.level, 1e-4f);  // relative to the key light, level taken out
			if (e.sun) {  // a unit vector toward it, w = -1 (as light 0)
				const float l = std::sqrt(e.pos[0] * e.pos[0] + e.pos[1] * e.pos[1] + e.pos[2] * e.pos[2]);
				c.light[i + 1] = l > 1e-6f ? Float4{ e.pos[0] / l, e.pos[1] / l, e.pos[2] / l, -1.0f } : Float4{ 0.0f, 0.0f, -1.0f, -1.0f };
			} else {
				c.light[i + 1] = { e.pos[0], e.pos[1], e.pos[2], std::max(e.radius, 0.0f) };
			}
			// w: this light's own reflected share over the key's level, which params.w and the floor strength carry (the
			// key may be the sun at its beam): the numerator gets beam_i, the direct term the light whole
			const float reflectW = std::clamp(std::isfinite(e.beam) ? e.beam : 1.0f, 0.0f, 1.0f) / std::max(Clamp01(in.level), 1e-4f);
			c.color[i + 1] = { e.color.r * ke, e.color.g * ke, e.color.b * ke, ke > 0.0f ? reflectW : 0.0f };
			c.falloff[i + 1] = e.sun ? Float4{} : packFalloff(e.falloff);
		}
		// one pattern layer at full weight (the crossfade would fade it out at agitation >= 0.5
		// with nothing to replace it).
		const float tint = std::isfinite(in.debugTintStrength) ? std::clamp(in.debugTintStrength, 0.01f, kMaxDebugTint) : 1.0f;
		const float strength = std::isfinite(in.strength) ? std::clamp(in.strength, 0.0f, kMaxStrength) : 1.0f;
		const float ambient = std::isfinite(in.ambient) ? std::max(in.ambient, kMinAmbient) : 1.0f;
		const float wsum = std::max(in.weights.calm, 0.0f) + std::max(in.weights.normal, 0.0f) + std::max(in.weights.rough, 0.0f);
		c.layers = wsum > 1e-4f ? Float4{ std::max(in.weights.calm, 0.0f) / wsum, std::max(in.weights.normal, 0.0f) / wsum,
									  std::max(in.weights.rough, 0.0f) / wsum, 0.0f } :
		                          Float4{ 1.0f, 0.0f, 0.0f, 0.0f };
		c.ripple = { in.rippleX, in.rippleY, Clamp01(in.rippleAmp), std::max(in.rippleRadius, 1.0f) };
		c.layers.w = in.tides ? 1.0f + std::clamp(in.tidesFine, 0.0f, 1.0f) : 0.0f;
		const float tidesSize = in.tides && std::isfinite(in.tidesSize) ? std::max(in.tidesSize, 0.0f) : 0.0f;
		std::memcpy(c.viewProj, cam.viewProj.m, sizeof(c.viewProj));
		c.ambientPos = { std::max(in.ambientPos[0], 0.0f), std::max(in.ambientPos[1], 0.0f), std::max(in.ambientPos[2], 0.0f), 0.0f };
		c.ambientNeg = { std::max(in.ambientNeg[0], 0.0f), std::max(in.ambientNeg[1], 0.0f), std::max(in.ambientNeg[2], 0.0f), 0.0f };
		{
			const float lum = 0.2126f * in.ambientHue.r + 0.7152f * in.ambientHue.g + 0.0722f * in.ambientHue.b;
			const bool  ok = std::isfinite(lum) && lum > 1e-4f && in.ambientHue.r >= 0.0f && in.ambientHue.g >= 0.0f && in.ambientHue.b >= 0.0f;
			// each channel at least 5 % of the luminance: a face no lamp reaches has only the ambient to divide by (assumed floor)
			c.ambientHue = ok ? Float4{ std::max(in.ambientHue.r / lum, 0.05f), std::max(in.ambientHue.g / lum, 0.05f), std::max(in.ambientHue.b / lum, 0.05f), 0.0f } :
			                    Float4{ 1.0f, 1.0f, 1.0f, 0.0f };
		}
		c.misc = { std::isfinite(in.absorb) ? std::clamp(in.absorb, 20.0f, 20000.0f) : 300.0f, in.occlusion ? 1.0f : 0.0f, tidesSize, 0.0f };
		c.fade = { std::isfinite(in.pixelFadeDistance) ? std::max(in.pixelFadeDistance, 0.0f) : 0.0f,
			std::isfinite(in.rippleContrast) ? std::clamp(in.rippleContrast, 0.0f, 3.0f) : 1.0f, 0.0f, 0.0f };
		const float level = Clamp01(in.level);
		c.params = { in.frameClock, ambient, in.debugTint ? tint : (in.debugLight ? -1.0f : (in.debugDirect ? -2.0f : 0.0f)), kCausticGain * strength * b * level };
		return c;
	}

	namespace
	{
		// The shader's check that the borrowed constants describe the borrowed texture (captured separately).
		bool TidesPaired(const PassConstants& c, const TidesField& t) noexcept
		{
			return std::fabs(static_cast<float>(t.resolution) - c.misc.z) < 0.5f && t.spacing > 0.0f;
		}

		// The pattern term in calm-atlas units (mean kPatternMean): from the tides field when bound and at this
		// water level (its curvature, plus the atlas for finer ripples), else the atlas alone at 'contrast'.
		float PatternTerm(const PassConstants& c, const TidesField* tides, float wx, float wy, float k, float pattern, float contrast) noexcept
		{
			const float atlas = Clamp01(pattern) / kPatternMean;  // mean 1
			if (c.layers.w > 0.5f && tides && std::fabs(c.water.x - tides->rest) < tides->tolerance && TidesPaired(c, *tides)) {
				const float fine = c.layers.w - 1.0f;
				return kPatternMean * TidesIntensity(*tides, wx, wy, k) * (1.0f + fine * (atlas - 1.0f));
			}
			return kPatternMean * (1.0f + contrast * (atlas - 1.0f));
		}

		// The field's cover of (x, y): its edge fade when it applies to this zone (shader TidesCover).
		float TidesCover(const PassConstants& c, const TidesField* tides, float x, float y) noexcept
		{
			if (!(c.layers.w > 0.5f) || !tides || !(std::fabs(c.water.x - tides->rest) < tides->tolerance) || tides->resolution < 2 || !TidesPaired(c, *tides)) {
				return 0.0f;
			}
			const float gx = (x - tides->originX) / tides->spacing, gy = (y - tides->originY) / tides->spacing;
			const float edge = std::min({ gx, gy, tides->resolution - 1 - gx, tides->resolution - 1 - gy });
			const float e = Clamp01(edge / std::max(1.0f, tides->edgeCells));
			return (edge >= 0.0f ? 1.0f : 0.0f) * e * e * (3.0f - 2.0f * e) * tides->enabled;
		}

		float PatternTermOblique(const PassConstants& c, const TidesField* tides, float wx, float wy, float k1, float k2, float e1x, float e1y,
			float pattern, float contrast) noexcept
		{
			const float k = c.fade.y;  // the player's ripple contrast (1 = physical), as the shader
			const float atlas = Clamp01(pattern) / kPatternMean;
			const float fromAtlas = kPatternMean * std::max(1.0f + k * contrast * (atlas - 1.0f), 0.0f);
			const float cov = TidesCover(c, tides, wx, wy);
			if (cov > 0.0f) {
				const float fine = c.layers.w - 1.0f;
				const float ti = std::max(1.0f + k * (TidesIntensity(*tides, wx, wy, k1, k2, e1x, e1y) - 1.0f), 0.0f);
				return Lerp(fromAtlas, kPatternMean * ti * std::max(1.0f + k * fine * (atlas - 1.0f), 0.0f), cov);
			}
			return fromAtlas;
		}

		// Light already on a face with normal n that the tracked lamps miss (shader AmbientAt).
		float AmbientAt(const PassConstants& c, const Vec3& n) noexcept
		{
			const float a = c.params.y + std::max(n[0], 0.0f) * c.ambientPos.x + std::max(n[1], 0.0f) * c.ambientPos.y + std::max(n[2], 0.0f) * c.ambientPos.z +
			                std::max(-n[0], 0.0f) * c.ambientNeg.x + std::max(-n[1], 0.0f) * c.ambientNeg.y + std::max(-n[2], 0.0f) * c.ambientNeg.z;
			return std::max(a, kMinAmbient);
		}

		Falloff FalloffOf(const PassConstants& c, std::size_t i) noexcept
		{
			const Float4& f = c.falloff[i];
			return { f.x, f.y, f.z, f.w };
		}

		// The shader's lit-plane choice: light i reads plane i when bit i of depthRange.z is set, else the water.
		int LitPlaneOf(const PassConstants& c, std::size_t i) noexcept
		{
			const unsigned bits = static_cast<unsigned>(std::max(c.depthRange.z, 0.0f) + 0.5f);
			return ((bits >> i) & 1u) ? static_cast<int>(i) : -1;
		}

		// ... and bit 3 + i: light i waits for its lit plane (it reflects nothing until then; its direct light counts)
		bool PlanePending(const PassConstants& c, std::size_t i) noexcept
		{
			const unsigned bits = static_cast<unsigned>(std::max(c.depthRange.z, 0.0f) + 0.5f);
			return ((bits >> (3 + i)) & 1u) != 0;
		}

		// The floor reads a lit plane over the patch of water its refracted light comes through (depth x tan 2 sigma
		// x kFloorSpreadFactor), and over at least a texel, so a 40-unit texel does not print a square shadow.
		float FloorLitLod(const PassConstants& c, float depthW) noexcept
		{
			const float texel = std::min(c.rect.z - c.rect.x, c.rect.w - c.rect.y) / static_cast<float>(kMaskSize);
			return std::log2(std::max(std::max(depthW, 0.0f) * std::max(c.water.z, 0.0f) * kFloorSpreadFactor / std::max(texel, 1e-3f), 2.0f));
		}

		// Mirror of the shader's pool-floor branch (p below the water).
		BoostResult FloorBoost(const PassConstants& c, const Vec3& p, const Vec3& n, float pattern, const WaterMask* mask, const TidesField* tides) noexcept
		{
			BoostResult out;
			out.inside = true;
			const float waterZ = c.water.x;
			const float depthW = waterZ - p[2];
			const float sizeX = c.rect.z - c.rect.x, sizeY = c.rect.w - c.rect.y;
			auto        uvOf = [&](float x, float y) {
                return std::pair{ sizeX > 1e-3f ? (x - c.rect.x) / sizeX : 0.0f, sizeY > 1e-3f ? (y - c.rect.y) / sizeY : 0.0f };
			};
			const auto [pu, pv] = uvOf(p[0], p[1]);
			const float above = mask ? SampleMask(*mask, pu, pv, 0.0f) : ((pu >= 0.0f && pu <= 1.0f && pv >= 0.0f && pv <= 1.0f) ? 1.0f : 0.0f);
			if (!(above > 0.002f)) {
				return out;  // no water over this floor (the shader's threshold: above its 8-bit filter weights)
			}
			const bool  maskMode = c.params.z < 0.0f && c.params.z > -1.5f;
			const bool  directMode = c.params.z < -1.5f;
			const float fade = Clamp01((depthW - 2.0f) / 18.0f) * std::exp(-depthW / (c.misc.x > 0.0f ? c.misc.x : kBottomAbsorb)) * c.water.w;
			float       r = 0.0f, g = 0.0f, bl = 0.0f, mk = 0.0f;
			float       dR = 0.0f, dG = 0.0f, dB = 0.0f;  // direct light per channel (the share is taken per channel)
			for (std::size_t i = 0; i < kMaxLights; ++i) {
				const Float4& L = c.light[i];
				const Float4& C = c.color[i];
				if (!(std::max({ C.x, C.y, C.z }) > 0.0f)) {
					continue;
				}
				const bool  sun = L.w < 0.0f;  // the sun: L.xyz is the unit vector toward it, no falloff
				const float lx = sun ? L.x : L.x - p[0], ly = sun ? L.y : L.y - p[1], lz = sun ? L.z : L.z - p[2];
				const float dl = sun ? 1.0f : std::sqrt(lx * lx + ly * ly + lz * lz);
				const float ad = sun ? 1.0f : Attenuation(FalloffOf(c, i), L.w, dl);
				const float nd = Clamp01((n[0] * lx + n[1] * ly + n[2] * lz) / std::max(dl, 1e-3f));
				dR += C.x * ad * nd;
				dG += C.y * ad * nd;
				dB += C.z * ad * nd;
				if (directMode || (sun ? L.z <= 1e-3f : L.z <= waterZ + 1e-3f) || !(ad * nd > 0.0f)) {
					continue;
				}
				// where the refracted ray to p enters the water
				constexpr float eta = 1.0f / 1.33f;
				float           wx = p[0], wy = p[1], ci = 1.0f, e1x = 0.0f, e1y = 0.0f;
				if (sun) {  // parallel rays: one refraction
					const float dx = -L.x, dy = -L.y, dz = -L.z;
					ci = Clamp01(-dz);
					const float k2 = std::max(1.0f - eta * eta * (1.0f - ci * ci), 0.0f);
					const float tz = eta * dz + (eta * ci - std::sqrt(k2));
					const float s = depthW / std::max(-tz, 1e-3f);
					wx = p[0] - eta * dx * s;
					wy = p[1] - eta * dy * s;
					e1x = dx;
					e1y = dy;
				} else for (int it = 0; it < 2; ++it) {  // two fixed-point steps
					float       dx = wx - L.x, dy = wy - L.y, dz = waterZ - L.z;
					const float dn = 1.0f / std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
					dx *= dn;
					dy *= dn;
					dz *= dn;
					ci = Clamp01(-dz);
					const float     k2 = std::max(1.0f - eta * eta * (1.0f - ci * ci), 0.0f);
					const float     tz = eta * dz + (eta * ci - std::sqrt(k2));
					const float     s = depthW / std::max(-tz, 1e-3f);
					wx = p[0] - eta * dx * s;
					wy = p[1] - eta * dy * s;
					e1x = wx - L.x;
					e1y = wy - L.y;
				}
				const float m = Clamp01(1.0f - ci);
				const float tr = 1.0f - (0.02f + 0.98f * (m * m) * (m * m) * m);
				const auto [wu, wv] = uvOf(wx, wy);
				const int   plane = LitPlaneOf(c, i);
				const float cover = PlanePending(c, i) ? 0.0f :
				                    mask             ? SampleMask(*mask, wu, wv, plane >= 0 ? FloorLitLod(c, depthW) : 0.0f, plane) :
				                                       ((wu >= 0.0f && wu <= 1.0f && wv >= 0.0f && wv <= 1.0f) ? 1.0f : 0.0f);
				if (maskMode) {
					mk = std::max(mk, cover * tr * ad * nd * fade);
					continue;
				}
				// oblique refraction: ci / ct above / below the surface, e1 = the light's horizontal direction
				const float ct = std::sqrt(std::max(1.0f - (1.0f / 1.33f) * (1.0f / 1.33f) * (1.0f - ci * ci), 1e-4f));
				const float q = 1.0f - ci / (1.33f * ct);
				// the shader's diverging-lamp depth: d n D / (d + n D) (the sun: d)
				const float dL = std::sqrt((wx - L.x) * (wx - L.x) + (wy - L.y) * (wy - L.y) + (waterZ - L.z) * (waterZ - L.z));
				// Coddington for a flat surface: n D across the plane of incidence, n D cos^2 t / cos^2 i within it
				const float nD = 1.33f * dL, nDt = nD * ct * ct / std::max(ci * ci, 0.0025f);
				// along the refracted ray (s = d / cos t): s nD / (s + nD), as a vertical depth x cos t
				const float dEff = sun ? depthW : depthW * nD * ct / std::max(depthW + nD * ct, 1e-3f);
				const float dEffT = sun ? depthW : depthW * nDt * ct / std::max(depthW + nDt * ct, 1e-3f);
				const float term = PatternTermOblique(c, tides, wx, wy, dEffT * q / (ct * ct), dEff * q, e1x, e1y, pattern,
					BottomContrastAt(dEff, c.water.z));
				const float k = tr * ad * nd * cover * (term / kPatternMean - 1.0f) * (i == 0 ? 1.0f : C.w);
				r += C.x * k;
				g += C.y * k;
				bl += C.z * k;
			}
			if (maskMode) {
				const float q = Clamp01(mk);
				out.add = { 0.25f * q, 0.6f * q, 1.0f * q };
				return out;
			}
			if (directMode) {
				const float q = Clamp01(0.2126f * dR + 0.7152f * dG + 0.0722f * dB);
				out.add = { 1.0f * q, 0.6f * q, 0.25f * q };
				return out;
			}
			const float amb = AmbientAt(c, n), s0 = fade * c.depthRange.w;
			r *= s0 / (dR + amb * c.ambientHue.x);
			g *= s0 / (dG + amb * c.ambientHue.y);
			bl *= s0 / (dB + amb * c.ambientHue.z);
			if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(bl)) {
				return out;
			}
			const float hi = std::clamp(c.color[0].w, 0.0f, kMaxFloorBoost);  // the floor keeps its own cap
			const float pf = PixelFade(c, p);  // past the shadow distance: as the shader
			r *= pf;
			g *= pf;
			bl *= pf;
			out.add = { std::clamp(r, kMinBottomBoost, hi), std::clamp(g, kMinBottomBoost, hi), std::clamp(bl, kMinBottomBoost, hi) };
			return out;
		}
	}

	BoostResult CausticBoost(const PassConstants& c, const Vec3& p, const Vec3& n, float pattern, const WaterMask* mask, const TidesField* tides,
		const PathOcclusion* occlusion) noexcept
	{
		BoostResult out;
		if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(n[0]) || !std::isfinite(n[1]) ||
			!std::isfinite(n[2]) || !std::isfinite(pattern)) {
			return out;  // like the shader: a NaN / inf input leaves the pixel unchanged
		}
		const float waterZ = c.water.x;
		const float reach = c.water.y;
		const float h = p[2] - waterZ;
		if (h > reach || h < -kBottomMaxDepth || p[0] < c.rect.x - reach || p[0] > c.rect.z + reach || p[1] < c.rect.y - reach ||
			p[1] > c.rect.w + reach) {
			return out;
		}
		if (h < 2.0f) {
			if (h > -2.0f || c.params.z > 0.0f) {
				return out;  // at the waterline, or the flat tint (above the water only)
			}
			return FloorBoost(c, p, n, pattern, mask, tides);
		}
		out.inside = true;
		if (c.params.z > 0.0f) {
			out.add = { c.params.z, c.params.z, c.params.z };
			return out;
		}
		const bool  maskMode = c.params.z < 0.0f && c.params.z > -1.5f;
		const bool  directMode = c.params.z < -1.5f;
		const float sizeX = c.rect.z - c.rect.x, sizeY = c.rect.w - c.rect.y;
		const float texel = std::min(sizeX, sizeY) / static_cast<float>(kMaskSize);
		const float s = Clamp01((h - 2.0f) / 28.0f);
		const float heightFade = s * s * (3.0f - 2.0f * s) * Clamp01((reach - h) / (0.2f * reach)) * c.water.w;

		float r = 0.0f, g = 0.0f, bl = 0.0f, mk = 0.0f;
		float dR = 0.0f, dG = 0.0f, dB = 0.0f;  // direct light per channel
		for (std::size_t i = 0; i < kMaxLights; ++i) {
			const Float4& L = c.light[i];
			const Float4& C = c.color[i];
			if (!(std::max({ C.x, C.y, C.z }) > 0.0f)) {
				continue;  // unused slot
			}
			const bool sun = L.w < 0.0f;  // the sun: L.xyz is the unit vector toward it, no falloff
			{
				const float lx = sun ? L.x : L.x - p[0], ly = sun ? L.y : L.y - p[1], lz = sun ? L.z : L.z - p[2];
				const float dl = sun ? 1.0f : std::sqrt(lx * lx + ly * ly + lz * lz);
				const float ad = sun ? 1.0f : Attenuation(FalloffOf(c, i), L.w, dl);
				const float nd = Clamp01((n[0] * lx + n[1] * ly + n[2] * lz) / std::max(dl, 1e-3f));
				dR += C.x * ad * nd;
				dG += C.y * ad * nd;
				dB += C.z * ad * nd;
			}
			if (directMode) {
				continue;
			}
			if (sun ? L.z <= 1e-3f : L.z <= waterZ + 1e-3f) {
				continue;  // a light at or below the water (the sun below the horizon) reflects nothing upward
			}
			Vec3  dir{}, toP{};
			float len = 0.0f, atten = 1.0f, wx = 0.0f, wy = 0.0f;
			if (sun) {
				dir = { -L.x, -L.y, L.z };  // parallel rays reflected off the plane
				toP = { dir[0] * 1e6f, dir[1] * 1e6f, dir[2] * 1e6f };
				len = 1e6f;
				wx = p[0] - dir[0] * (h / std::max(dir[2], 1e-3f));
				wy = p[1] - dir[1] * (h / std::max(dir[2], 1e-3f));
			} else {
				const Vec3 lm{ L.x, L.y, 2.0f * waterZ - L.z };  // mirror image below the water
				toP = { p[0] - lm[0], p[1] - lm[1], p[2] - lm[2] };
				len = std::sqrt(toP[0] * toP[0] + toP[1] * toP[1] + toP[2] * toP[2]);
				atten = Attenuation(FalloffOf(c, i), L.w, len);
				const float il = 1.0f / std::max(len, 1e-3f);
				dir = { toP[0] * il, toP[1] * il, toP[2] * il };
				const float t = (waterZ - lm[2]) / (p[2] - lm[2]);
				wx = lm[0] + toP[0] * t;
				wy = lm[1] + toP[1] * t;
			}
			if (dir[2] <= 1e-3f || atten <= 0.0f) {
				continue;  // light below the water, or out of reach
			}
			const float dx = p[0] - wx, dy = p[1] - wy, dz = p[2] - waterZ;
			const float tidesCov = TidesCover(c, tides, wx, wy);
			const float spreadR = Lerp(c.water.z, kTidesStillSpread, tidesCov) * std::sqrt(dx * dx + dy * dy + dz * dz);
			const float lodM = std::log2(std::max(spreadR / std::max(texel, 1e-3f), 1.0f));
			const float u = sizeX > 1e-3f ? (wx - c.rect.x) / sizeX : 0.0f, v = sizeY > 1e-3f ? (wy - c.rect.y) / sizeY : 0.0f;
			const float ox = std::max({ c.rect.x - wx, wx - c.rect.z, 0.0f }), oy = std::max({ c.rect.y - wy, wy - c.rect.w, 0.0f });
			const float outside = Clamp01(1.0f - std::sqrt(ox * ox + oy * oy) / std::max(spreadR, texel));  // as the shader
			const int   plane = LitPlaneOf(c, i);  // a light with a lit plane: only the water it reaches reflects it
			float       cover = (mask ? SampleMask(*mask, u, v, lodM, plane) : ((u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) ? 1.0f : 0.0f)) * outside;
			if (PlanePending(c, i)) {
				cover = 0.0f;  // its lit plane is not cast yet
			}
			const float ndl = Clamp01(-(n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2]));
			if (cover <= 0.0f || ndl <= 0.0f || heightFade <= 0.0f) {
				continue;
			}
			if (c.misc.y > 0.5f && occlusion) {
				cover *= occlusion->Visible({ wx, wy, waterZ }, p);  // something between the water and this face blocks it
			}
			if (cover <= 0.0f) {
				continue;
			}
			if (maskMode) {
				mk = std::max(mk, atten * ndl * cover * heightFade);
				continue;
			}
			const float m = Clamp01(1.0f - dir[2]);
			if (atten * ndl * cover * heightFade * c.params.w * std::max({ C.x, C.y, C.z }) < 1e-4f) {
				continue;  // as the shader: too faint to sample the pattern
			}
			const float fres = 0.02f + 0.98f * (m * m) * (m * m) * m;
			const float lwp = std::sqrt(dx * dx + dy * dy + dz * dz), cz = std::max(dir[2], 0.1f);  // oblique reflection
			const float dlw = std::max(len - lwp, 1.0f);                                             // the lamp's image to the water point
			const float leff = lwp * dlw / (lwp + dlw);                                              // a diverging beam focuses farther away
			const float pat = PatternTermOblique(c, tides, wx, wy, -2.0f * leff / cz, -2.0f * leff * cz, toP[0], toP[1], pattern, kPatternContrast);  // as the shader
			const float rx = wx - c.ripple.x, ry = wy - c.ripple.y, rr = std::sqrt(rx * rx + ry * ry);
			const float ramp = Clamp01(c.ripple.z) * (1.0f - tidesCov) * std::exp(-rr / std::max(c.ripple.w, 1.0f));
			const float wave = std::sin(6.2831853f * (rr / 28.0f - c.params.x / 8.0f * 1.6f));
			const float k = atten * fres * ndl * pat * c.params.w * (i == 0 ? 1.0f : C.w) * Lerp(1.0f, kTidesReflectScale, tidesCov) * (sun ? kSunReflectScale : 1.0f) * cover * heightFade *
			                (1.0f + 0.6f * ramp * wave);
			r += C.x * k;
			g += C.y * k;
			bl += C.z * k;
		}
		if (maskMode) {
			const float q = Clamp01(mk);  // the geometric share as it is (no Fresnel, no pattern)
			out.add = { 0.25f * q, 0.6f * q, 1.0f * q };
			return out;
		}
		if (directMode) {
			const float q = Clamp01(0.2126f * dR + 0.7152f * dG + 0.0722f * dB);
			out.add = { 1.0f * q, 0.6f * q, 0.25f * q };
			return out;
		}
		const float amb = AmbientAt(c, n);  // as a share of the light already there, per channel
		r /= dR + amb * c.ambientHue.x;
		g /= dG + amb * c.ambientHue.y;
		bl /= dB + amb * c.ambientHue.z;
		if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(bl) || !std::isfinite(c.color[0].w)) {
			return out;  // like the shader: any NaN / inf leaves the pixel unchanged
		}
		const float hi = std::max(c.color[0].w, 0.0f);
		const float pf = PixelFade(c, p);  // past the shadow distance: as the shader
		r *= pf;
		g *= pf;
		bl *= pf;
		out.add = { std::clamp(r, 0.0f, hi), std::clamp(g, 0.0f, hi), std::clamp(bl, 0.0f, hi) };
		return out;
	}
}
