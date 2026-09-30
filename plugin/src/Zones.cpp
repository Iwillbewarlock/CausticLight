#include "PCH.h"

#include "Zones.h"

#include "CausticMath.h"
#include "DrawPass.h"
#include "Settings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <bit>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <map>
#include <numbers>

// Data-only plugin: zones are water rectangles from zones.json. Nothing is placed in the
// world and no engine shader is changed. The plugin decides, per active zone, the key light,
// agitation (layer weights and playback speed), ripple and fades, and stages a per-zone record
// for its own draw pass after opaque geometry (DrawPass.cpp, shaders/CausticPass.hlsl).
//
// Cost model
//   per frame (render routine): clock read and per-slot atlas frame clock. No game reads other
//     than the pause flag, no allocation, no logging. When game work is due ONE SKSE task is
//     queued (never more than one pending).
//   post-opaque (render routine): one scissored fullscreen triangle per visible zone (max 4).
//   placementHz : active-zone check (cell attached + camera distance), slot assignment.
//   tickHz      : smoothing, layer weights, speed, ripple, GPU record staging.
//   actorHz     : high-process actors in water (impulse + strongest swimmer) and key light.
//   weatherHz   : one Sky read while an exterior zone is active.
//   static LIGH refs around a zone are sampled once each time it becomes active.

namespace cl::Zones
{
	namespace
	{
		using math::RGB;

		constexpr RE::FormID kPanelLocalID = 0x800;  // ESP forms: logged only, never required
		constexpr RE::FormID kLightLocalID = 0x801;
		constexpr auto       kPluginName = "CausticLight.esp"sv;

		constexpr float kAgitationTau = 0.8f;
		constexpr float kLightTau = 1.0f;
		constexpr float kKeyTau = 0.2f;
		constexpr float kMinExtraLight = 0.05f;  // extra lights weaker than 5 % of the key light are left out
		constexpr float kSunShadowLength = 4000.0f;    // shadow rays toward the sun / moon: cliffs, trees, bridges
		constexpr float kSunRedoDegrees = 3.0f;        // the sun moved this far since the shadow pass: redo it
		constexpr float kSunUseDegrees = 6.0f;  // the last shadow pass is used while within this of the sun
		constexpr float kSunRecastMinIntensity = 0.05f;  // renderer units (the sun ~3, a new moon ~0.01): fainter, recast only near kSunUseDegrees
		// A reflected light's identity (its lit plane is kept by it): a placed LIGH ref's FormID (high word 0), a Light Placer
		// light as (owner FormID << 32) | its index + 1, the sun / moon kSunLitRef; 0 = none (moving lights keep no plane).
		using LightKey = std::uint64_t;
		constexpr LightKey kSunLitRef = 0xFFFFFFFFFFFFFFFEull;

		float DegreesBetween(const RE::NiPoint3& a, const RE::NiPoint3& b);

		// How far the caustics reach: the game's shadow distance (as a shadow, the pattern is light and shade on the
		// walls), read live so a change in the game's settings follows at once; else zones.json maxDistance.
		float CausticDistance()
		{
			const auto& s = Settings::Get();
			float       d = s.maxDistance;
			if (s.maxDistanceFromShadows) {
				static RE::Setting* setting = nullptr;
				if (!setting) {
					if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
						setting = prefs->GetSetting("fShadowDistance:Display");
					}
				}
				if (setting && std::isfinite(setting->GetFloat()) && setting->GetFloat() > 0.0f) {
					d = std::clamp(setting->GetFloat(), 500.0f, 100000.0f);
				}
			}
			static float logged = -1.0f;
			if (d != logged) {
				logger::info("caustic distance {:.0f} units ({:.0f} m){}", d, d / 70.0f,
					s.maxDistanceFromShadows ? " = the game's shadow distance (fShadowDistance)" : " (zones.json maxDistance)");
				logged = d;
			}
			return d;
		}

		// how far the sun / moon may move before its lit plane is cast again: kSunRedoDegrees, or for a sky light too
		// faint to be worth that (the engine swings a new moon round at dusk) just before the plane stops being used
		float SunRecastDegrees(float intensity) { return intensity >= kSunRecastMinIntensity ? kSunRedoDegrees : kSunUseDegrees - 1.0f; }

		float DegreesBetween(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float la = a.Length(), lb = b.Length();
			if (la < 1e-6f || lb < 1e-6f) {
				return 180.0f;
			}
			return std::acos(std::clamp(a.Dot(b) / (la * lb), -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
		}

		struct Zone
		{
			ZoneConfig         cfg;
			math::Rect         rect;
			RE::NiPoint3       center;  // rect centre on the water plane
			RE::TESObjectCELL* cell{ nullptr };
			RE::TESWorldSpace* worldspace{ nullptr };

			bool  active{ false };
			int   slot{ -1 };
			float cameraDistance{ 0.0f };
			float distFade{ 0.0f };

			// Light input. interiorStatic is sampled when the zone becomes active; interior adds the
			// best dynamic light (actor tick).
			math::LightResult interiorStatic;
			math::LightResult interior;
			int               interiorLightCount{ 0 };
			const char*       lightSource{ "sky" };

			bool         staticKeyValid{ false };
			RE::NiPoint3 staticKeyPos;
			float        staticKeyScore{ 0.0f };
			float        staticKeyRadius{ 0.0f };
			RE::FormID   staticKeyRef{ 0 };

			// Key light (world, not mirrored). Sun: unit vector toward the sun.
			bool                 keyValid{ false };
			bool                 keySun{ false };
			bool                 keySnap{ true };
			bool                 keyIsStatic{ false };  // the key light is staticKeyRef (not a dynamic light)
			LightKey             litKeyRef{ 0 };        // the light the mask's lit plane 0 was cast for (kSunLitRef: the sun / moon)
			RE::NiPoint3         litSunDir;             // ... and then the direction toward it
			std::array<LightKey, 2> litExtraRef{};      // the lights lit planes 1, 2 were cast for
			std::array<RE::NiPoint3, 2> litExtraDir{};  // ... the direction toward the sun when that is one of them
			RE::NiPoint3         keyTarget;
			math::Vec3           keyPos{ 0.0f, 0.0f, 0.0f };
			float                keyRadius{ 0.0f };
			std::array<char, 48> keySource{};

			// Smoothed state
			float impulse{ 0.0f };
			float agitation{ 0.0f };
			float speed{ 1.0f };
			float level{ 0.0f };
			RGB   color{ 1.0f, 1.0f, 1.0f };
			float frameClock{ 0.0f };  // advanced every frame by the hook
			float phase{ 0.0f };

			// Ripple: strongest in-water actor
			float             rippleBestImpulse{ 0.0f };
			RE::Actor*        rippleBestActor{ nullptr };  // this actor scan only
			RE::NiPoint3      rippleBestPos;
			float             rippleTargetAmp{ 0.0f };
			float             rippleTargetX{ 0.0f };
			float             rippleTargetY{ 0.0f };
			RE::FormID        rippleActorID{ 0 };
			math::RippleState ripple;

			// Debug
			int                inActors{ 0 };
			float              inWeather{ 0.0f };
			float              inTargetAgitation{ 0.0f };
			float              inTargetLevel{ 0.0f };
			math::LayerWeights weights;

			// Where the plane is really water: from the rect (manual zones) or a ray grid (automatic).
			std::shared_ptr<const math::WaterMask> mask;
			bool                                   automatic{ false };

			// The strongest static lights (sampled on activation) and the two besides the key light that
			// are reflected too; their level is their score relative to the key light's.
			struct StaticLight
			{
				RE::FormID   ref{ 0 };
				RE::NiPoint3 pos;
				float        radius{ 0.0f };
				RGB          color{ 1.0f, 1.0f, 1.0f };
				float        score{ 0.0f };
				float        intensity{ 0.0f };  // fade x max colour channel: the light's own strength (the pass's unit), before distance
				RGB          ambient{};          // what it adds to the walls at the zone (renderer units, decoded colour): counted when not reflected
			};
			std::array<StaticLight, math::kMaxLights>          staticTop{};
			int                                                staticTopCount{ 0 };
			std::array<math::ExtraLight, math::kMaxLights - 1> extra{};

			// Light a surface gets besides the reflected lamps: the cell's ambient plus every other light
			// near the zone (fade x falloff x luminance), and the same in key-light units for the pass.
			float ambientAbs{ 0.0f };
			std::vector<RE::FormID> scannedLights;  // the placed lights the scan counted (sorted): the rest in the light lists are new (a magelight)
			RGB   ambientBase{};  // the cell's ambient + every light below the top 3, per channel (the top-3 lights not reflected are added per key)
			RGB   ambientHue{ 1.0f, 1.0f, 1.0f };  // the ambient's colour (decoded), for the per-channel share
			RGB   keyColor{ 1.0f, 1.0f, 1.0f };    // the key light's own hue (decoded, max 1)
			float keyBeam{ 1.0f };                  // the sun as the key: the share of it reflected (its parallel beam)
			float      sunIntensity{ 0.0f };        // the sun / moon light's strength now (renderer units)
			LightKey      keyPlaneKey{ 0 };         // a stationary key's key (its lit plane 0); 0 = moving or none
			math::Falloff keyFalloff{};
			int        keyKind{ -1 };               // which light is the key (LightCand::Kind), -1 none
			LightKey   keyId{ 0 };                  // ... and its key (kSunLitRef for the sun)
			bool       keyJump{ false };            // the key changed to another light: its position is taken at once
			bool       keyFromNone{ false };        // ... and before it there was none
			std::array<math::SlotId, 2> extraSlot{};  // which light each extra slot holds (it keeps the slot: hysteresis)
			float ambientRel{ 0.05f };
			float keyIntensity{ 1.0f };
			// the cell's (or the weather's) directional ambient, DALC: luminance on faces toward +x, +y, +z and -x, -y, -z,
			// and the same in key-light units. Added to the isotropic ambient above (an assumption the log checks).
			std::array<float, 3> dalcPosAbs{}, dalcNegAbs{};
			std::array<float, 3> dalcPosRel{}, dalcNegRel{};
			// the pool floor's absorption e-fold, from the water type's underwater fog
			float       absorb{ math::kBottomAbsorb };
			std::string absorbSource{ "default" };
		};

		struct Exterior
		{
			float weatherAgitation{ 0.0f };
			float wind01{ 0.0f };  // blended between the last and current weather
			float rain01{ 0.0f };
			float level{ 0.5f };
			float beam{ 1.0f };  // DirectBeamFraction, blended between the last and current weather
			RGB   color{ 1.0f, 1.0f, 1.0f };
			bool  rainy{ false }, snowy{ false }, cloudy{ false };  // the current weather (log)
			float pct{ 1.0f };
		};

		std::vector<Zone>                         g_zones;
		std::array<int, math::kMaxDrawZones>       g_slotZone{ -1, -1, -1, -1 };  // zone index per GPU slot
		std::vector<RE::Actor*>                   g_actorScratch;
		int                                       g_activeCount{ 0 };
		int                                       g_activeExterior{ 0 };
		bool                                      g_warnedTooMany{ false };

		RE::TESBoundObject* g_panelBase{ nullptr };  // optional, logged only
		RE::TESBoundObject* g_lightBase{ nullptr };
		bool                g_ready{ false };

		Exterior g_ext;
		bool     g_extValid{ false };

		std::chrono::steady_clock::time_point g_lastFrame{};
		bool                                  g_haveLastFrame{ false };
		float g_placeAccum{ 1e9f };
		float g_outAccum{ 0.0f };
		float g_actorAccum{ 0.0f };
		float g_weatherAccum{ 1e9f };
		float g_debugAccum{ 0.0f };
		float g_globalsAccum{ 1e9f };

		std::atomic<bool>          g_forcePlacement{ false };
		std::atomic<bool>          g_taskPending{ false };
		std::atomic<std::uint32_t> g_generation{ 0 };
		std::atomic<bool>          g_worldReady{ false };
		bool                       g_debugTint{ false };  // effective: zones.json at start, then CL_DebugTint 1
		bool                       g_debugLight{ false };  // CL_DebugTint 2: light mask
		bool                       g_debugDirect{ false };  // CL_DebugTint 3: direct light
		float                      g_strength{ math::kDefaultStrength };  // CL_Strength

		// ---- helpers -------------------------------------------------------------------

		std::string Lower(std::string_view s)
		{
			std::string out(s);
			for (auto& c : out) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return out;
		}

		std::optional<std::pair<RE::FormID, std::string>> ParseFormRef(std::string_view s)
		{
			const auto tilde = s.find('~');
			if (tilde == std::string_view::npos) {
				return std::nullopt;
			}
			try {
				const auto id = static_cast<RE::FormID>(std::stoul(std::string(s.substr(0, tilde)), nullptr, 16));
				return std::make_pair(id, std::string(s.substr(tilde + 1)));
			} catch (...) {
				return std::nullopt;
			}
		}

		RE::TESObjectCELL* ResolveCell(std::string_view text)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return nullptr;
			}
			if (auto ref = ParseFormRef(text)) {
				return dh->LookupForm<RE::TESObjectCELL>(ref->first, ref->second);
			}
			if (auto* cell = RE::TESForm::LookupByEditorID<RE::TESObjectCELL>(text)) {
				return cell;
			}
			const auto want = Lower(text);  // fallback: interior cells keep their editor IDs
			for (auto* cell : dh->interiorCells) {
				if (cell) {
					const char* edid = cell->GetFormEditorID();
					if (edid && Lower(edid) == want) {
						return cell;
					}
				}
			}
			return nullptr;
		}

		RE::TESWorldSpace* ResolveWorldspace(std::string_view text)
		{
			if (text.empty()) {
				return nullptr;
			}
			if (auto ref = ParseFormRef(text)) {
				auto* dh = RE::TESDataHandler::GetSingleton();
				return dh ? dh->LookupForm<RE::TESWorldSpace>(ref->first, ref->second) : nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::TESWorldSpace>(text);
		}

		math::Vec3 V(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }

		RGB ToRGB(const RE::Color& c) { return { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f }; }

		float Lum(const RGB& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

		RGB NormalizeHue(RGB c)
		{
			const float m = std::max({ c.r, c.g, c.b });
			if (m <= 1e-4f) {
				return { 1.0f, 1.0f, 1.0f };
			}
			return { c.r / m, c.g / m, c.b / m };
		}

		// ---- how the renderer turns authored light colours into light ------------------------
		// Vanilla lights a surface with the authored colours as they are. Community Shaders' Linear Lighting
		// (Shaders/Common/Color.hlsli) decodes them first - colour^gamma - and multiplies point and directional
		// lights by pi (not the ambient: Ambient() has no pi), times its own multipliers. The share of reflected
		// light is taken against the ambient, so the two must be in the renderer's units. Read once from CS's
		// settings (user over default); no file or LL off = vanilla.
		struct RenderLight
		{
			bool  enb{ false };      // ENB (its d3d11.dll exports the SDK) or the 'renderer: enb' override
			bool  cs{ false };       // CommunityShaders.dll in the process
			bool  isl{ false };      // ... with its inverse-square feature (Light Placer's 'InverseSquare' lights)
			bool  linear{ false };
			float gamma{ 1.0f };
			float pointMult{ 1.0f };
			float dirMult{ 1.0f };
			float ambientMult{ 1.0f };
		};

		// ENB SDK (ENBSeriesSDK.h): parameters of enbseries.ini as ENB holds them.
		struct EnbParameter
		{
			unsigned char Data[16];
			unsigned long Size;
			long          Type;  // 1 float, 2 int, 3 hex, 4 bool, 5 colour3, 6 colour4, 7 vector3
		};
		using EnbGetParameterFn = BOOL (*)(char*, char*, char*, EnbParameter*);
		using EnbGetVersionFn = long (*)();
		EnbGetParameterFn g_enbGet = nullptr;

		bool DetectEnb(long& sdk, long& version)
		{
			HMODULE m = GetModuleHandleW(L"d3d11.dll");  // ENB's proxy is loaded under this name from the game folder
			auto*   getSdk = m ? reinterpret_cast<EnbGetVersionFn>(GetProcAddress(m, "ENBGetSDKVersion")) : nullptr;
			if (!getSdk) {
				return false;
			}
			sdk = getSdk();
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(m, path, MAX_PATH);
			logger::info("ENB: SDK found in {}", std::filesystem::path(path).string());
			auto* getVer = reinterpret_cast<EnbGetVersionFn>(GetProcAddress(m, "ENBGetVersion"));
			version = getVer ? getVer() : 0;
			g_enbGet = reinterpret_cast<EnbGetParameterFn>(GetProcAddress(m, "ENBGetParameter"));
			return true;
		}

		std::optional<float> EnbFloat(const char* category, const char* key)
		{
			if (!g_enbGet) {
				return std::nullopt;
			}
			EnbParameter p{};
			std::string  file = "enbseries.ini", cat = category, k = key;
			if (!g_enbGet(file.data(), cat.data(), k.data(), &p) || p.Type != 1 || p.Size < sizeof(float)) {
				return std::nullopt;
			}
			float f = 0.0f;
			std::memcpy(&f, p.Data, sizeof(float));
			return std::isfinite(f) ? std::optional<float>(f) : std::nullopt;
		}

		RenderLight& RenderMut();

		const RenderLight& Render() { return RenderMut(); }

		RenderLight& RenderMut()
		{
			static RenderLight r = [] {
				RenderLight out;
				const std::string force = Settings::Get().renderer;
				long              sdk = 0, ver = 0;
				if (force == "enb" || (force == "auto" && DetectEnb(sdk, ver))) {
					out.enb = true;  // ENB: vanilla colours (no CS decode) x its own multipliers, refreshed by RefreshEnb
					logger::info("renderer light: ENB{} (SDK {}, version {}): point / directional / ambient multipliers from enbseries.ini [ENVIRONMENT]",
						g_enbGet ? "" : " (override: no SDK, multipliers 1)", sdk, ver);
					return out;
				}
				if (force == "vanilla") {
					logger::info("renderer light: vanilla (override)");
					return out;
				}
				using json = nlohmann::json;
				auto load = [](const char* path) {
					std::ifstream in(path);
					if (!in) {
						return json{};
					}
					try {
						return json::parse(in, nullptr, true, true);
					} catch (...) {
						return json{};
					}
				};
				const json def = load("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json");
				const json usr = load("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json");
				auto num = [&](const char* section, const char* key, double fallback) {
					for (const json* j : { &usr, &def }) {
						if (j->is_object() && j->contains(section) && (*j)[section].is_object() && (*j)[section].contains(key) && (*j)[section][key].is_number()) {
							return (*j)[section][key].get<double>();
						}
					}
					return fallback;
				};
				out.cs = GetModuleHandleW(L"CommunityShaders.dll") != nullptr;
				std::error_code ec;
				auto disabledAtBoot = [&](const char* feature) {
					return usr.is_object() && usr.contains("Disable at Boot") && usr["Disable at Boot"].is_object() && usr["Disable at Boot"].contains(feature) &&
					       usr["Disable at Boot"][feature].is_boolean() && usr["Disable at Boot"][feature].get<bool>();
				};
				out.isl = out.cs && std::filesystem::exists("Data/Shaders/Features/InverseSquareLighting.ini", ec) && !disabledAtBoot("InverseSquareLighting");
				if (!usr.is_object() && !def.is_object()) {
					logger::info("renderer light: no Community Shaders settings found -> vanilla (authored colours as they are)");
					return out;
				}
				const double ll = num("Linear Lighting", "enableLinearLighting", 0.0);
				out.linear = ll > 0.5;
				out.gamma = out.linear ? static_cast<float>(std::clamp(num("Linear Lighting", "colorGamma", num("Linear Lighting", "lightGamma", 1.8)), 1.0, 3.0)) : 1.0f;
				out.pointMult = static_cast<float>(num("CS Utility", "pointLightMult", 1.0) * (out.linear ? num("Linear Lighting", "pointLightMult", 1.0) : 1.0));
				out.dirMult = static_cast<float>(num("CS Utility", "directionalLightMult", 1.0) * (out.linear ? num("Linear Lighting", "directionalLightMult", 1.0) : 1.0));
				out.ambientMult = static_cast<float>(num("CS Utility", "ambientLightMult", 1.0) * (out.linear ? num("Linear Lighting", "ambientMult", 1.0) : 1.0));
				logger::info("renderer light: Community Shaders {}{}, Linear Lighting {} (colour gamma {:.2f}), point x {:.2f}, directional x {:.2f}, ambient x {:.2f}{}",
					out.cs ? "loaded" : "NOT loaded", out.isl ? " (inverse-square lights on)" : "", out.linear ? "ON" : "off", out.gamma, out.pointMult, out.dirMult, out.ambientMult, out.linear ? ", lights x pi" : "");
				return out;
			}();
			return r;
		}

		// ENB: the light multipliers for the current hour (1 Hz). Keys with six time points, else Day / Night, else 1.
		float g_scanPointMult = 1.0f, g_scanAmbientMult = 1.0f;  // the multipliers at the last lamp scan

		// true when a multiplier moved more than 2 % from the last lamp scan's (the scanned lamps then need their values again)
		bool RefreshEnb()
		{
			auto& r = RenderMut();
			if (!r.enb) {
				return false;
			}
			const float oldPoint = g_scanPointMult, oldAmbient = g_scanAmbientMult;  // what the scanned lamps were counted at
			math::EnbSchedule s;
			auto              tod = [&](const char* k, float& v) {
                if (auto f = EnbFloat("TIMEOFDAY", k)) {
                    v = *f;
                }
			};
			tod("DawnDuration", s.dawnDuration);
			tod("SunriseTime", s.sunriseTime);
			tod("DayTime", s.dayTime);
			tod("SunsetTime", s.sunsetTime);
			tod("DuskDuration", s.duskDuration);
			tod("NightTime", s.nightTime);
			auto*       cal = RE::Calendar::GetSingleton();
			const float hour = cal ? cal->GetHour() : 12.0f;
			auto*       player = RE::PlayerCharacter::GetSingleton();
			const bool  interior = player && player->GetParentCell() && player->GetParentCell()->IsInteriorCell();
			auto value = [&](const std::string& base) {
				if (interior) {
					const auto d = EnbFloat("ENVIRONMENT", (base + "InteriorDay").c_str()), n = EnbFloat("ENVIRONMENT", (base + "InteriorNight").c_str());
					if (d && n) {
						return math::Lerp(*n, *d, math::EnbDayWeight(s, hour));
					}
					if (auto i = EnbFloat("ENVIRONMENT", (base + "Interior").c_str())) {
						return *i;
					}
				}
				static const char* six[6] = { "Dawn", "Sunrise", "Day", "Sunset", "Dusk", "Night" };
				std::array<float, 6> v{};
				bool                 all = true;
				for (int i = 0; i < 6 && all; ++i) {
					auto f = EnbFloat("ENVIRONMENT", (base + six[i]).c_str());
					all = f.has_value();
					v[static_cast<std::size_t>(i)] = f.value_or(1.0f);
				}
				if (all) {
					return math::EnbExteriorBlend(v, s, hour);
				}
				const auto d = EnbFloat("ENVIRONMENT", (base + "Day").c_str()), n = EnbFloat("ENVIRONMENT", (base + "Night").c_str());
				if (d && n) {
					return math::Lerp(*n, *d, math::EnbDayWeight(s, hour));
				}
				return 1.0f;  // not in this preset
			};
			r.pointMult = std::max(value("PointLightingIntensity"), 0.0f);
			r.dirMult = std::max(value("DirectLightingIntensity"), 0.0f);
			r.ambientMult = std::max(value("AmbientLightingIntensity"), 0.0f);
			const bool moved = std::fabs(r.pointMult - oldPoint) > 0.02f * std::max(oldPoint, 1e-3f) ||
			                   std::fabs(r.ambientMult - oldAmbient) > 0.02f * std::max(oldAmbient, 1e-3f);
			static int logs = 0;
			if (logs < 3 || Settings::Get().debug) {
				++logs;
				logger::info("ENB light multipliers at {:.1f} h ({}): point {:.2f}, directional {:.2f}, ambient {:.2f}", hour, interior ? "interior" : "exterior",
					r.pointMult, r.dirMult, r.ambientMult);
			}
			return moved;
		}

		RGB Decode(const RGB& c)
		{
			const float g = Render().gamma;
			return g == 1.0f ? c : RGB{ std::pow(std::max(c.r, 0.0f), g), std::pow(std::max(c.g, 0.0f), g), std::pow(std::max(c.b, 0.0f), g) };
		}

		// A point / directional light's strength on a lit face, in the renderer's units: fade x colour, decoded
		// (the shader decodes the colour the engine hands it, the fade already in it).
		// A light's colour in the renderer's units. Community Shaders (Lighting.hlsl): PointLight(colour) x attenuation x fade
		// - the colour alone is decoded (gamma, x pi, x the multiplier), fade multiplies after; a CS 'Linear' light
		// (Light Placer flag) skips the decode, the pi and the multiplier. Vanilla: the colour x fade.
		RGB LightRGB(float fade, const RGB& c, bool directional, bool linearLight = false)
		{
			const bool  lin = linearLight && Render().linear;
			const RGB   d = lin ? c : Decode(c);
			const float k = fade * (lin ? 1.0f : (Render().linear ? std::numbers::pi_v<float> : 1.0f) * (directional ? Render().dirMult : Render().pointMult));
			return { d.r * k, d.g * k, d.b * k };
		}

		float LightUnit(float fade, const RGB& c, bool directional, bool linearLight = false)
		{
			const RGB d = LightRGB(fade, c, directional, linearLight);
			return std::max({ d.r, d.g, d.b });
		}

		// Its luminance, for lights that only add to the ambient (the same units).
		float LightLum(float fade, const RGB& c, bool linearLight = false) { return Lum(LightRGB(fade, c, false, linearLight)); }

		float AmbientLum(const RGB& c) { return Lum(Decode(c)) * Render().ambientMult; }

		RGB LightHue(const RGB& c) { return NormalizeHue(Decode(c)); }

		bool IsUsable(const RE::TESObjectREFR* r) { return r && !r->IsDeleted() && !r->IsMarkedForDeletion(); }

		RE::NiPoint3 CameraPosition()
		{
			if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
				return cam->cameraRoot->world.translate;
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				return player->GetPosition();
			}
			return {};
		}

		// Same interior cell, or both exterior in one worldspace.
		bool SameSpace(const Zone& z, const RE::TESObjectCELL* other)
		{
			if (!z.cell || !other) {
				return false;
			}
			if (z.cell == other) {
				return true;
			}
			if (!z.cell->IsExteriorCell() || !other->IsExteriorCell()) {
				return false;
			}
			return z.cell->GetRuntimeData().worldSpace == other->GetRuntimeData().worldSpace;
		}

		float ZoneDistance(const Zone& z, const RE::NiPoint3& p)
		{
			return math::DistanceToZone(z.rect, z.cfg.waterZ, z.cfg.height, V(p));
		}

		// to the water itself (its rect on the water plane): what a light's reflection starts from, for the ranking
		float WaterDistance(const Zone& z, const RE::NiPoint3& p) { return math::DistanceToZone(z.rect, z.cfg.waterZ, 0.0f, V(p)); }

		void SetSource(Zone& z, std::string_view text)
		{
			const auto n = std::min(text.size(), z.keySource.size() - 1);
			std::memcpy(z.keySource.data(), text.data(), n);
			z.keySource[n] = '\0';
		}

		Zone MakeZone(const ZoneConfig& cfg, std::size_t index)
		{
			Zone z;
			z.cfg = cfg;
			z.rect = { cfg.rect[0], cfg.rect[1], cfg.rect[2], cfg.rect[3] };
			z.center = { 0.5f * (z.rect.minX + z.rect.maxX), 0.5f * (z.rect.minY + z.rect.maxY), cfg.waterZ };
			z.cell = ResolveCell(cfg.cell);
			z.worldspace = ResolveWorldspace(cfg.worldspace);
			z.phase = static_cast<float>(index) * 1.618f;
			z.frameClock = std::fmod(static_cast<float>(index) * 17.0f, math::kAtlasFrames);  // zones out of step
			z.agitation = cfg.baseAgitation;
			z.speed = math::PlaybackSpeed(z.agitation);
			z.mask = math::MaskFromRect(z.rect);
			if (!z.cell) {
				logger::error("[{}] cell '{}' not found - zone disabled", cfg.name, cfg.cell);
			} else {
				logger::info("[{}] cell '{}' -> {:08X} ({}) rect ({:.0f},{:.0f})-({:.0f},{:.0f}) waterZ {:.1f} height {:.0f}", cfg.name,
					cfg.cell, z.cell->GetFormID(), z.cell->IsInteriorCell() ? "interior" : "exterior", z.rect.minX, z.rect.minY,
					z.rect.maxX, z.rect.maxY, cfg.waterZ, cfg.height);
			}
			if (!cfg.worldspace.empty() && !z.worldspace) {
				logger::warn("[{}] worldspace '{}' not found - using the cell's own", cfg.name, cfg.worldspace);
			}
			return z;
		}

		// ---- interior light input (each time a zone becomes active) --------------------

		void UseCellLighting(Zone& z)
		{
			const RE::INTERIOR_DATA* xcll = z.cell->GetLighting();
			const auto*              tmpl = z.cell->GetRuntimeData().lightingTemplate;
			using I = RE::INTERIOR_DATA::Inherit;

			const RE::INTERIOR_DATA* dirSrc = xcll;
			const RE::INTERIOR_DATA* ambSrc = xcll;
			if (tmpl && (!xcll || xcll->lightingTemplateInheritanceFlags.any(I::kDirectionalColor))) {
				dirSrc = &tmpl->data;
			}
			if (tmpl && (!xcll || xcll->lightingTemplateInheritanceFlags.any(I::kAmbientColor))) {
				ambSrc = &tmpl->data;
			}
			const RGB dir = dirSrc ? ToRGB(dirSrc->directional) : RGB{};
			const RGB amb = ambSrc ? ToRGB(ambSrc->ambient) : RGB{};

			const auto choice = math::CellLightingFallback(dir, amb);
			z.interior = choice.light;
			z.lightSource = choice.light.level <= 0.0f ? "none (cell lighting is black)" :
			                choice.usedDirectional  ? (dirSrc == xcll ? "cell directional (XCLL)" : "lighting template directional") :
			                                          (ambSrc == xcll ? "cell ambient (XCLL)" : "lighting template ambient");
		}

		RGB CellAmbient(const Zone& z)
		{
			const RE::INTERIOR_DATA* xcll = z.cell->GetLighting();
			const auto*              tmpl = z.cell->GetRuntimeData().lightingTemplate;
			const RE::INTERIOR_DATA* src = xcll;
			if (tmpl && (!xcll || xcll->lightingTemplateInheritanceFlags.any(RE::INTERIOR_DATA::Inherit::kAmbientColor))) {
				src = &tmpl->data;
			}
			return src ? ToRGB(src->ambient) : RGB{};
		}

		std::array<float, 3> DalcLum(const RE::BGSDirectionalAmbientLightingColors& d, bool positive)
		{
			const auto& a = d.directional;
			return positive ? std::array<float, 3>{ AmbientLum(ToRGB(a.x.max)), AmbientLum(ToRGB(a.y.max)), AmbientLum(ToRGB(a.z.max)) } :
			                  std::array<float, 3>{ AmbientLum(ToRGB(a.x.min)), AmbientLum(ToRGB(a.y.min)), AmbientLum(ToRGB(a.z.min)) };
		}

		// The cell's directional ambient (max = faces toward +axis, min = toward -axis); from the lighting
		// template under the same inherit flag as the ambient colour (there is no separate one).
		void CellDalc(Zone& z)
		{
			const RE::INTERIOR_DATA* xcll = z.cell->GetLighting();
			const auto*              tmpl = z.cell->GetRuntimeData().lightingTemplate;
			const RE::BGSDirectionalAmbientLightingColors* d = xcll ? &xcll->directionalAmbientLightingColors : nullptr;
			if (tmpl && (!xcll || xcll->lightingTemplateInheritanceFlags.any(RE::INTERIOR_DATA::Inherit::kAmbientColor))) {
				d = &tmpl->directionalAmbientLightingColors;
			}
			z.dalcPosAbs = d ? DalcLum(*d, true) : std::array<float, 3>{};
			z.dalcNegAbs = d ? DalcLum(*d, false) : std::array<float, 3>{};
		}

		// Light fades into the water type's underwater fog: at the fog's far distance the floor is all but gone
		// (about e^-3). So the e-fold is far / 3 - an estimate, not a measured absorption.
		void WaterAbsorb(Zone& z)
		{
			z.absorb = math::kBottomAbsorb;
			z.absorbSource = "default";
			RE::TESWaterForm* w = nullptr;
			const char*       from = "";
			if (auto* x = z.cell->extraList.GetByType<RE::ExtraCellWaterType>(); x && x->water) {
				w = x->water;
				from = "cell";
			} else if (z.cell->IsExteriorCell() && z.cell->GetRuntimeData().worldSpace && z.cell->GetRuntimeData().worldSpace->worldWater) {
				w = z.cell->GetRuntimeData().worldSpace->worldWater;
				from = "worldspace";
			}
			if (!w) {
				return;
			}
			const float fogFar = w->data.underwaterFogDistFar, amount = w->data.underwaterFogAmount;
			const char* ed = w->GetFormEditorID();
			const std::string name = std::format("{} water {} ({:08X}): underwater fog far {:.0f}, amount {:.2f}", from, (ed && *ed) ? ed : "?",
				w->GetFormID(), fogFar, amount);
			if (!std::isfinite(fogFar) || fogFar < 30.0f || !(amount > 0.05f)) {
				z.absorbSource = name + " -> not usable, default";
				return;
			}
			z.absorb = std::clamp(fogFar / 3.0f, 50.0f, 3000.0f);
			z.absorbSource = name;
		}

		// A light the pass does not reflect, as it lights a wall by the water: the pass's own falloff (1 - (d / 1.5 r)^2)
		// at the distance to the zone's centre, times kWallCos, the mean n.l over the faces around the water (assumed:
		// the faces are not known here), in renderer units per channel.
		constexpr float kWallCos = 0.5f;
		RGB WallLight(float dist, float radius, float fade, const RGB& color, const math::Falloff& falloff = {}, bool linearLight = false)
		{
			if (!(radius > 0.0f) && !(falloff.inverseSquare > 0.5f)) {
				return {};
			}
			const float a = kWallCos * math::Attenuation(falloff, radius, std::max(dist, 0.0f));
			const RGB   d = LightRGB(fade > 0.0f ? fade : 1.0f, color, false, linearLight);
			return { d.r * a, d.g * a, d.b * a };
		}

		RGB AmbientRGB(const RGB& c)
		{
			const RGB d = Decode(c);
			const float m = Render().ambientMult;
			return { d.r * m, d.g * m, d.b * m };
		}

		void SetAmbient(Zone& z, const RGB& total)
		{
			const float l = Lum(total);
			z.ambientAbs = l;
			z.ambientHue = l > 1e-4f ? RGB{ total.r / l, total.g / l, total.b / l } : RGB{ 1.0f, 1.0f, 1.0f };
		}

		// How much a light brings to the water: its falloff there x its own decoded strength (the key is the largest).
		float LightWeight(float dist, float radius, float fade, const RGB& color, const math::Falloff& falloff = {}, bool linearLight = false)
		{
			if (!(radius > 0.0f) && !(falloff.inverseSquare > 0.5f)) {
				return 0.0f;
			}
			// the pass's falloff (vanilla or CS inverse square), as the lights are drawn
			return math::Attenuation(falloff, radius, std::max(dist, 0.0f)) * LightUnit(fade > 0.0f ? fade : 1.0f, color, false, linearLight);
		}

		void ScanInteriorLights(Zone& z)
		{
			z.staticKeyValid = false;
			z.staticKeyScore = 0.0f;
			z.staticKeyRef = 0;
			z.staticTopCount = 0;
			z.scannedLights.clear();
			g_scanPointMult = Render().pointMult;
			g_scanAmbientMult = Render().ambientMult;

			const float range = Settings::Get().lightSearchRange;
			const float halfDiag = 0.5f * std::hypot(z.rect.maxX - z.rect.minX, z.rect.maxY - z.rect.minY);

			std::vector<math::LightSample> samples;
			struct Candidate
			{
				RE::FormID   ref;
				RE::FormID   base;
				RE::NiPoint3 pos;
				float        radius, fade, dist, score;
				RE::Color    color;
				std::uint32_t flags;
				float        weight;       // falloff x decoded strength: the ranking
				float        centreDist;   // to the zone's centre on the water (the ambient's distance)
			};
			std::vector<Candidate> candidates;  // for the log: which lights are there (the skylight source is unknown)
			auto visit = [&](RE::TESObjectREFR* r) {
				if (!IsUsable(r) || r->IsDisabled()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* base = r->GetBaseObject();
				if (!base || base == g_lightBase || !base->Is(RE::FormType::Light)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* ligh = base->As<RE::TESObjectLIGH>();
				if (!ligh || ligh->data.flags.any(RE::TES_LIGHT_FLAGS::kNegative)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float dist = ZoneDistance(z, r->GetPosition());
				if (dist > range) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const math::LightSample s{ ToRGB(ligh->data.color), dist, static_cast<float>(ligh->data.radius),
					ligh->fade > 0.0f ? ligh->fade : 1.0f };
				samples.push_back(s);
				const float score = math::LightContribution(s.distance, s.radius, s.fade);
				z.scannedLights.push_back(r->GetFormID());
				candidates.push_back({ r->GetFormID(), base->GetFormID(), r->GetPosition(), s.radius, s.fade, dist, score, ligh->data.color,
					ligh->data.flags.underlying(), LightWeight(WaterDistance(z, r->GetPosition()), s.radius, s.fade, ToRGB(ligh->data.color)),
					r->GetPosition().GetDistance(z.center) });
				return RE::BSContainer::ForEachResult::kContinue;
			};
			auto* tes = RE::TES::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (z.cell->IsExteriorCell() && tes && player) {
				// a 2560 zone often straddles a cell edge: every loaded cell around the player (the zone is around it)
				tes->ForEachReferenceInRange(player, range + halfDiag + player->GetPosition().GetDistance(z.center), visit);
			} else {
				z.cell->ForEachReferenceInRange(z.center, range + halfDiag, visit);
			}
			std::ranges::sort(z.scannedLights);
			z.interior = math::CombineLights(samples);
			z.interiorLightCount = static_cast<int>(samples.size());
			z.lightSource = "LIGH refs";
			if (z.interior.level <= 0.0f) {
				UseCellLighting(z);
			}
			z.interiorStatic = z.interior;
			// by what each brings to the water; ties (lamps inside the zone's reach all score their fade) by the form ID
			std::ranges::stable_sort(candidates, [](const Candidate& a, const Candidate& b) { return a.weight != b.weight ? a.weight > b.weight : a.ref < b.ref; });
			for (const auto& c : candidates) {
				if (z.staticTopCount >= static_cast<int>(math::kMaxLights) || !(c.weight > 0.0f)) {
					break;
				}
				z.staticTop[static_cast<std::size_t>(z.staticTopCount++)] = { c.ref, c.pos, c.radius, LightHue(ToRGB(c.color)), c.weight,
					LightUnit(c.fade, ToRGB(c.color), false), WallLight(c.centreDist, c.radius, c.fade, ToRGB(c.color)) };  // the pass's light unit: hue x intensity
			}
			if (z.staticTopCount > 0) {  // the key is the first: its position, strength, colour and ref from one light
				const auto& k = z.staticTop[0];
				z.staticKeyValid = true;
				z.staticKeyScore = k.score;
				z.staticKeyPos = k.pos;
				z.staticKeyRadius = k.radius;
				z.staticKeyRef = k.ref;
			}
			z.ambientBase = AmbientRGB(CellAmbient(z));
			for (std::size_t i = static_cast<std::size_t>(z.staticTopCount); i < candidates.size(); ++i) {
				// lights not reflected still light the walls
				const auto& o = candidates[i];
				const RGB   w = WallLight(o.centreDist, o.radius, o.fade, ToRGB(o.color));
				z.ambientBase = { z.ambientBase.r + w.r, z.ambientBase.g + w.g, z.ambientBase.b + w.b };
			}
			SetAmbient(z, z.ambientBase);
			CellDalc(z);
			logger::info("[{}] directional ambient (DALC) +x {:.3f} -x {:.3f} +y {:.3f} -y {:.3f} +z {:.3f} -z {:.3f} beside the ambient colour {:.3f}",
				z.cfg.name, z.dalcPosAbs[0], z.dalcNegAbs[0], z.dalcPosAbs[1], z.dalcNegAbs[1], z.dalcPosAbs[2], z.dalcNegAbs[2], AmbientLum(CellAmbient(z)));
			logger::info("[{}] ambient for the share (renderer units): cell {:.3f} + other lights -> {:.3f} (key light intensity {:.2f})", z.cfg.name,
				AmbientLum(CellAmbient(z)), z.ambientAbs, z.staticTopCount ? z.staticTop[0].intensity : 0.0f);
			for (std::size_t i = 0; i < candidates.size() && i < 12; ++i) {
				const auto& c = candidates[i];
				logger::info("[{}] light #{}: ref {:08X} base {:08X} pos ({:.0f}, {:.0f}, {:.0f}) = {:.0f} above the water, radius {:.0f}, fade {:.2f}, "
							 "colour ({},{},{}), flags {:X}, distance to zone {:.0f}, score {:.3f}",
					z.cfg.name, i + 1, c.ref, c.base, c.pos.x, c.pos.y, c.pos.z, c.pos.z - z.cfg.waterZ, c.radius, c.fade, c.color.red,
					c.color.green, c.color.blue, c.flags, c.dist, c.score);
			}
			if (const RE::INTERIOR_DATA* xcll = z.cell->GetLighting()) {
				logger::info("[{}] cell lighting: directional ({},{},{}) rotation XY {} Z {} fade {:.2f}, ambient ({},{},{}), template {}",
					z.cfg.name, xcll->directional.red, xcll->directional.green, xcll->directional.blue, xcll->directionalXY,
					xcll->directionalZ, xcll->directionalFade, xcll->ambient.red, xcll->ambient.green, xcll->ambient.blue,
					z.cell->GetRuntimeData().lightingTemplate ? std::format("{:08X}", z.cell->GetRuntimeData().lightingTemplate->GetFormID()) :
					                                            std::string("none"));
			}
			logger::info("[{}] interior light input: {} LIGH ref(s) within {:.0f} -> source {}, level {:.2f}, colour ({:.2f},{:.2f},{:.2f}); "
						 "static key {}",
				z.cfg.name, samples.size(), range, z.lightSource, z.interior.level, z.interior.color.r, z.interior.color.g,
				z.interior.color.b, z.staticKeyValid ? std::format("{:08X}", z.staticKeyRef) : std::string("none"));
		}

		// ---- active set ----------------------------------------------------------------

		bool RefreshAutoZoneCell();

		void PollActive()
		{
			RefreshAutoZoneCell();  // before any zone cell is read
			const auto&  s = Settings::Get();
			const auto   cam = CameraPosition();
			struct Cand
			{
				float       dist;
				std::size_t idx;
			};
			std::vector<Cand> cands;

			for (std::size_t i = 0; i < g_zones.size(); ++i) {
				auto& z = g_zones[i];
				if (!z.cell) {
					continue;
				}
				z.cameraDistance = ZoneDistance(z, cam);
				if (math::IsZoneActive(z.cell->IsAttached(), z.cameraDistance, CausticDistance())) {
					cands.push_back({ z.cameraDistance, i });
				}
			}
			std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
			if (cands.size() > math::kMaxDrawZones && !g_warnedTooMany) {
				g_warnedTooMany = true;
				logger::warn("{} zones are active; only the nearest {} are drawn", cands.size(), math::kMaxDrawZones);
			}

			std::vector<bool> chosen(g_zones.size(), false);
			for (std::size_t k = 0; k < cands.size() && k < math::kMaxDrawZones; ++k) {
				chosen[cands[k].idx] = true;
			}

			g_slotZone.fill(-1);
			int active = 0;
			int exterior = 0;
			for (std::size_t i = 0; i < g_zones.size(); ++i) {
				auto&      z = g_zones[i];
				const bool now = chosen[i];
				if (now && !z.active) {
					// becoming active: sample static lights, start from current inputs
					z.keySnap = true;
					z.ripple = {};
					z.rippleTargetAmp = 0.0f;
					z.impulse = 0.0f;
					z.keyValid = false;
					ScanInteriorLights(z);  // placed lights outdoors too (lanterns, campfires)
					WaterAbsorb(z);
					logger::info("[{}] active (camera {:.0f} from the zone); floor absorption e-fold {:.0f} from {}", z.cfg.name, z.cameraDistance,
						z.absorb, z.absorbSource);
				} else if (!now && z.active) {
					logger::info("[{}] inactive", z.cfg.name);
				}
				z.active = now;
				z.slot = -1;
			}
			for (std::size_t k = 0; k < cands.size() && k < math::kMaxDrawZones; ++k) {
				auto& z = g_zones[cands[k].idx];
				z.slot = static_cast<int>(k);
				g_slotZone[k] = static_cast<int>(cands[k].idx);
				++active;
				exterior += z.cell->IsExteriorCell() ? 1 : 0;
			}
			g_activeCount = active;
			g_activeExterior = exterior;
		}

		// ---- inputs --------------------------------------------------------------------

		math::WeatherSample Sample(const RE::TESWeather* w)
		{
			math::WeatherSample s;
			if (!w) {
				return s;
			}
			using F = RE::TESWeather::WeatherDataFlag;
			s.wind01 = w->data.windSpeed / 255.0f;
			s.rainy = w->data.flags.any(F::kRainy);
			s.snowy = w->data.flags.any(F::kSnow);
			s.cloudy = w->data.flags.any(F::kCloudy);
			return s;
		}

		RGB SunColor(const RE::TESWeather* w, const std::array<float, 4>& tod)
		{
			if (!w) {
				return { 1.0f, 1.0f, 1.0f };
			}
			RGB c{};
			for (std::size_t t = 0; t < 4; ++t) {
				const RGB k = ToRGB(w->colorData[RE::TESWeather::ColorTypes::kSunlight][t]);
				c.r += k.r * tod[t];
				c.g += k.g * tod[t];
				c.b += k.b * tod[t];
			}
			return c;
		}

		void ReadExterior()
		{
			auto* sky = RE::Sky::GetSingleton();
			if (!sky || !sky->currentWeather) {
				g_ext = {};
				g_extValid = true;
				return;
			}
			const RE::TESWeather* cur = sky->currentWeather;
			const RE::TESWeather* last = sky->lastWeather ? sky->lastWeather : cur;
			const float           pct = math::Clamp01(sky->currentWeatherPct);
			const float           hour = sky->currentGameHour;

			const auto sCur = Sample(cur);
			const auto sLast = Sample(last);
			g_ext.weatherAgitation = math::BlendedWeatherAgitation(sLast, sCur, pct);
			g_ext.wind01 = math::Lerp(sLast.wind01, sCur.wind01, pct);
			g_ext.rain01 = math::Lerp(sLast.rainy ? 1.0f : 0.0f, sCur.rainy ? 1.0f : 0.0f, pct);
			const float dim = math::Lerp(math::WeatherDimming(sLast), math::WeatherDimming(sCur), pct);
			g_ext.level = math::DaylightLevel(hour) * dim;
			// the engine keeps the sun's colour up in rain (key intensity 3.14 in a rainy log): the beam fraction is
			// the weather term, crossfaded with the engine's own weather transition
			g_ext.beam = math::Lerp(math::DirectBeamFraction(sLast), math::DirectBeamFraction(sCur), pct);
			g_ext.rainy = sCur.rainy;
			g_ext.snowy = sCur.snowy;
			g_ext.cloudy = sCur.cloudy;
			g_ext.pct = pct;
			const auto tod = math::TimeOfDayWeights(hour);
			g_ext.color = LightHue(math::Lerp(SunColor(last, tod), SunColor(cur, tod), pct));
			g_extValid = true;
		}

		// High-process actors (plus the player) in water within actorRange of an active zone's
		// rect add an impulse; the strongest one drives the ripple ring.
		void ScanActors()
		{
			for (auto& z : g_zones) {
				z.inActors = 0;
				z.rippleBestImpulse = 0.0f;
				z.rippleBestActor = nullptr;
			}
			g_actorScratch.clear();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player) {
				g_actorScratch.push_back(player);
			}
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					if (auto a = h.get(); a) {
						g_actorScratch.push_back(a.get());
					}
				}
			}

			const float range = Settings::Get().actorRange;
			for (auto* a : g_actorScratch) {
				if (!a || a->IsDead() || !a->Is3DLoaded()) {
					continue;
				}
				auto* cell = a->GetParentCell();
				if (!cell) {
					continue;
				}
				const auto apos = a->GetPosition();

				float submerge = -1.0f;
				float speed = 0.0f;
				for (auto& z : g_zones) {
					if (!z.active || !SameSpace(z, cell)) {
						continue;
					}
					const float d = math::DistanceToRect2D(z.rect, apos.x, apos.y);
					if (d >= range) {
						continue;
					}
					if (submerge < 0.0f) {
						using B = RE::Actor::BOOL_BITS;
						const bool wetFlag = a->GetActorRuntimeData().boolBits.any(B::kInWater, B::kSwimming);
						submerge = math::Clamp01(a->GetSubmergedLevel(apos.z, cell));
						if (submerge <= 0.01f && wetFlag) {
							submerge = 0.3f;
						}
						if (submerge > 0.01f) {
							RE::NiPoint3 vel{};
							a->GetLinearVelocity(vel);
							speed = vel.Length();
						}
					}
					if (submerge <= 0.01f) {
						break;
					}
					const float imp = math::ActorImpulse(submerge, speed, d, range);
					if (imp > 0.0f) {
						z.impulse = std::max(z.impulse, imp);
						++z.inActors;
						if (imp > z.rippleBestImpulse) {
							z.rippleBestImpulse = imp;
							z.rippleBestActor = a;
							z.rippleBestPos = apos;
						}
					}
				}
			}

			for (auto& z : g_zones) {
				if (!z.active) {
					continue;
				}
				z.rippleTargetAmp = math::Clamp01(z.rippleBestImpulse);
				if (z.rippleBestActor) {
					z.rippleTargetX = z.rippleBestPos.x;  // world x,y (the shader works in world space)
					z.rippleTargetY = z.rippleBestPos.y;
					z.rippleActorID = z.rippleBestActor == player ? 0x14 : z.rippleBestActor->GetFormID();
				}
				z.rippleBestActor = nullptr;  // not kept past this scan
			}
		}

		// ---- key light (actor tick) ----------------------------------------------------------

		int  g_sunDirSign = 0;  // +1: worldDir points at the sun, -1: worldDir is the travel direction
		bool g_sunDown = false;

		// NiDirectionalLight::GetWorldDirection is "the first column of the world rotation"
		// (N/NiDirectionalLight.h:48-49). Whether it points at the sun is decided against the
		// sun disc node's position and logged when first known or when it changes.
		bool ToSun(math::Vec3& out)
		{
			auto* sky = RE::Sky::GetSingleton();
			auto* sun = sky ? sky->sun : nullptr;
			auto* light = sun ? sun->light.get() : nullptr;
			if (!light) {
				return false;
			}
			RE::NiPoint3 d = light->GetWorldDirection();
			const float  len = d.Length();
			if (len < 1e-4f) {
				return false;
			}
			d = d * (1.0f / len);

			int sign = g_sunDirSign != 0 ? g_sunDirSign : -1;  // Gamebryo convention: travel direction
			if (sun->sunBaseNode) {
				const RE::NiPoint3 toDisc = sun->sunBaseNode->world.translate - CameraPosition();
				// at night the game swings its directional light round to play the moon: the disc is then below the
				// horizon and cannot tell the sign. The light's convention does not change, so the sign learned by day
				// holds; before any day, the sign that puts the light above the horizon (moonlight comes from above).
				const bool night = toDisc.Length() > 1.0f && toDisc.z < 0.0f;
				if (night != g_sunDown) {
					logger::info("sun: {}", night ? "below the horizon (night): the directional light is the moon's now" : "above the horizon again");
					g_sunDown = night;
				}
				if (night) {
					const int s = g_sunDirSign != 0 ? g_sunDirSign : (d.z >= 0.0f ? +1 : -1);
					out = { d.x * s, d.y * s, d.z * s };
					return out[2] > 0.0f;
				}
				if (toDisc.Length() > 1.0f) {
					const int measured = d.Dot(toDisc) >= 0.0f ? +1 : -1;
					if (measured != g_sunDirSign) {
						logger::info("sun: NiDirectionalLight worldDir {} (checked against the sun disc node)",
							measured > 0 ? "points TOWARD the sun" : "is the direction the light TRAVELS (away from the sun)");
					}
					g_sunDirSign = measured;
					sign = measured;
				}
			}
			out = { d.x * sign, d.y * sign, d.z * sign };
			return true;
		}

		struct DynLight
		{
			bool         valid{ false };
			RE::NiPoint3 pos;
			RGB          color;
			float        score{ 0.0f };
			float        radius{ 0.0f };
			RE::FormID   refID{ 0 };
			std::uint64_t id{ 0 };           // the owner ref, or the light itself when it has none (a magelight)
			float        intensity{ 0.0f };  // fade x max colour channel
			float        weight{ 0.0f };     // falloff x strength, as the static lights are ranked
			RGB          wall{};             // what it adds to the walls when not reflected
			LightKey     key{ 0 };           // a stationary light's key (Light Placer on a fixed object); 0 = moving
			math::Falloff falloff{};         // CS inverse square (Light Placer flag) or vanilla
		};

		// Light Placer names its lights "LP_Light[...]#index" and keeps its data on the light (powerof3/LightPlacer
		// LightData.cpp): ambient.red = flags (Disabled 1<<9, InverseSquare 1<<10, Linear 1<<11, cull bits 24..31),
		// ambient.green = cutoff, radius.z = size.
		struct LpLight
		{
			bool          is{ false };
			std::uint32_t flags{ 0 };
			float         cutoff{ 1.0f };
			float         size{ 0.0f };
			std::uint32_t index{ 0 };
			std::uint32_t nameHash{ 0 };
		};

		LpLight ReadLp(const RE::NiLight* nl)
		{
			LpLight     lp;
			const char* nm = nl->name.c_str();
			if (!nm || std::strncmp(nm, "LP_Light", 8) != 0) {
				return lp;
			}
			const auto& d = nl->GetLightRuntimeData();
			lp.is = true;
			lp.flags = std::bit_cast<std::uint32_t>(d.ambient.red);
			lp.cutoff = d.ambient.green;
			lp.size = d.radius.z;
			if (const char* h = std::strrchr(nm, '#')) {
				lp.index = static_cast<std::uint32_t>(std::strtoul(h + 1, nullptr, 10));
			} else {
				lp.index = 0;
			}
			std::uint32_t hash = 2166136261u;  // the whole name's FNV-1a: two lights of one object never share a key
			for (const char* c = nm; *c; ++c) {
				hash = (hash ^ static_cast<std::uint8_t>(*c)) * 16777619u;
			}
			lp.nameHash = hash | 1u;  // never 0
			return lp;
		}

		// The first ref up the light's parents (Light Placer lights carry none themselves).
		RE::TESObjectREFR* LightOwner(const RE::NiLight* nl)
		{
			RE::TESObjectREFR* owner = nl->GetUserData();
			for (auto* p = nl->parent; p && !owner; p = p->parent) {
				owner = p->GetUserData();
			}
			return owner;
		}

		// the in-game check of kRadiusScale (and what Light Placer lights look like): a few lines per session
		int g_radiusLogs = 0, g_lpLogs = 0;

		// ---- which lights the water reflects -----------------------------------------------------------
		// Every light that could reflect off the water is ranked by what it brings there (LightWeight: its falloff
		// at the water x its own decoded strength): the sun or moon outdoors (its parallel beam only: the weather's
		// DirectBeamFraction), the three strongest moving lights (a carried torch, a magelight, a spell's light) and
		// the three strongest placed lights. The first is the key light, the next two the extras (when at least 5 %
		// of the key's own strength); every light not reflected still lights the walls and counts as ambient. A light
		// keeps its place until another brings 20 % more (AssignReflectedSlots), and a lamp whose lit plane reaches no
		// water gives its place up (it only lights the walls: ambient).
		struct LightCand
		{
			enum class Kind
			{
				kSun,
				kDynamic,
				kStatic
			} kind{ Kind::kStatic };
			float        weight{ 0.0f };     // the ranking: what it brings to the water
			float        intensity{ 0.0f };  // its own strength (renderer units): the relative level of an extra
			float        beam{ 1.0f };       // the sun: the share of it that is a parallel beam (reflected)
			RGB          hue{ 1.0f, 1.0f, 1.0f };
			RE::NiPoint3 at;                 // position, or the direction toward the sun
			float        radius{ 0.0f };
			LightKey     ref{ 0 };           // its key (0: a moving light, no lit plane)
			RGB          wall{};             // what it adds to the walls when it is not reflected
			math::Falloff falloff{};
			std::uint64_t id{ 0 };           // which light it is (the key's identity for the snap / hysteresis): the key, or a moving light's own
			RE::FormID    owner{ 0 };        // a moving light's owner (logs)
		};

		// true when the light's lit plane is cast and reaches no water here (the automatic zone): it reflects nothing
		bool PlaneDark(const Zone& z, const LightCand& c);
		// its lit plane is being cast again (a recast): an old plane that no longer fits waits for it
		bool PlaneRecasting(LightKey ref);
		// a 'stationary' light that keeps moving (an animated lamp): it moved again before its recast finished, twice;
		// it is then reflected unshadowed, as a moving light, instead of waiting for a plane that never comes
		bool PlaneWobbly(LightKey ref);
		// the mask with each wanted light's finished lit plane in its slot
		std::shared_ptr<const math::WaterMask> AlignedPlanes(Zone& z, std::shared_ptr<const math::WaterMask> mask);

		// A live light read at the 4 Hz light tick: the engine animates a torch's fade every frame (its flicker), so
		// the samples are random steps 250 ms apart, not the flame. The ranking and the relative levels take them
		// through a filter (engineering, not physics: it removes the aliasing; the engine's own direct light still
		// flickers per frame, and the key's own reflection scales with it in the share). kFlickerTau is assumed: of
		// the order of a torch's flicker period, so a carried torch still follows within about a second.
		constexpr float kFlickerTau = 1.0f;
		struct Filtered
		{
			float  weight{ 0.0f };
			float  intensity{ 0.0f };
			double seen{ 0.0 };
		};
		std::map<std::tuple<std::size_t, int, std::uint64_t>, Filtered> g_flicker;

		double NowSeconds()
		{
			static const auto t0 = std::chrono::steady_clock::now();
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		}

		void Deflicker(std::size_t slot, LightCand& c, double now)
		{
			auto [it, fresh] = g_flicker.try_emplace({ slot, static_cast<int>(c.kind), c.id });
			auto&       f = it->second;
			const float rawI = c.intensity;
			if (fresh || now - f.seen > 2.0) {
				f.weight = c.weight;  // first seen (or back after a while): as it is
				f.intensity = c.intensity;
			} else {
				const float dt = static_cast<float>(std::clamp(now - f.seen, 0.0, 1.0));
				f.weight = math::SmoothToward(f.weight, c.weight, dt, kFlickerTau);
				f.intensity = math::SmoothToward(f.intensity, c.intensity, dt, kFlickerTau);
			}
			f.seen = now;
			c.weight = f.weight;
			c.intensity = f.intensity;
			const float s = rawI > 1e-6f ? f.intensity / rawI : 1.0f;  // its wall light (ambient when not reflected) the same
			c.wall = { c.wall.r * s, c.wall.g * s, c.wall.b * s };
		}

		struct SkyLight
		{
			bool                 valid{ false };
			float                intensity{ 0.0f };
			RGB                  hue{ 1.0f, 1.0f, 1.0f };
			RGB                  ambient{};  // the weather's current ambient colour (renderer units)
			std::array<float, 3> dalcPos{}, dalcNeg{};
		};

		SkyLight ReadSkyLight()
		{
			SkyLight out;
			auto*    sky = RE::Sky::GetSingleton();
			if (!sky) {
				return out;
			}
			auto*      sun = sky->sun ? sky->sun->light.get() : nullptr;
			const auto d = sun ? sun->GetLightRuntimeData().diffuse : RE::NiColor{};
			out.valid = true;
			out.intensity = LightUnit(1.0f, { d.red, d.green, d.blue }, true);
			out.hue = LightHue({ d.red, d.green, d.blue });
			const auto& a = sky->skyColor[RE::TESWeather::ColorTypes::kAmbient];
			out.ambient = AmbientRGB({ a.red, a.green, a.blue });
			for (std::size_t k = 0; k < 3; ++k) {
				const auto& mx = sky->directionalAmbientColors[k][0];
				const auto& mn = sky->directionalAmbientColors[k][1];
				out.dalcPos[k] = AmbientLum({ mx.red, mx.green, mx.blue });
				out.dalcNeg[k] = AmbientLum({ mn.red, mn.green, mn.blue });
			}
			return out;
		}

		void ChooseLights(Zone& z, std::vector<LightCand> cands, const RGB& ambientBase)
		{
			std::ranges::stable_sort(cands, [](const LightCand& a, const LightCand& b) { return a.weight != b.weight ? a.weight > b.weight : a.ref < b.ref; });
			// the key keeps its place until another light brings 20 % more (no flicker between two near-equal lights);
			// the extras the same (assumed: the key's 20 %), each keeping its slot so its lit plane stays where it is
			constexpr float kKeyHysteresis = 1.2f;
			constexpr float kExtraHysteresis = 1.2f;
			std::vector<math::RankCand> rank(cands.size());
			for (std::size_t i = 0; i < cands.size(); ++i) {
				rank[i] = { static_cast<int>(cands[i].kind), cands[i].id, cands[i].weight, cands[i].intensity, PlaneDark(z, cands[i]) };
			}
			const auto pick = math::AssignReflectedSlots(rank, { z.keyKind, z.keyId }, z.extraSlot, kKeyHysteresis, kExtraHysteresis, kMinExtraLight);
			z.extra = {};
			z.keySun = false;
			z.keyIsStatic = false;
			if (pick.key < 0) {
				SetSource(z, "none (no light near the water)");
				z.keyValid = false;
				z.keyKind = -1;
				z.extraSlot = {};
				return;
			}
			const LightCand& key = cands[static_cast<std::size_t>(pick.key)];
			const float      keyI = std::max(key.intensity, 1e-4f);
			if (static_cast<int>(key.kind) != z.keyKind || key.id != z.keyId) {
				z.keyFromNone = z.keyKind < 0;  // back after no light: the level fades in, not snaps
				z.keyJump = true;  // another light: no sliding from the old one's position (or a direction into a position)
				z.keyKind = static_cast<int>(key.kind);
				z.keyId = key.id;
			}
			z.keyIntensity = keyI;
			z.keyColor = key.hue;
			z.keyTarget = key.at;
			z.keyRadius = key.kind == LightCand::Kind::kSun ? 0.0f : key.radius;
			z.keySun = key.kind == LightCand::Kind::kSun;
			z.keyIsStatic = key.kind == LightCand::Kind::kStatic;  // a placed lamp or a stationary Light Placer light
			z.keyPlaneKey = z.keyIsStatic ? key.ref : 0;
			z.keyFalloff = key.falloff;
			z.keyBeam = z.keySun ? key.beam : 1.0f;
			switch (key.kind) {
			case LightCand::Kind::kSun:
				SetSource(z, g_sunDown ? "moon (the night directional light)" : "sun");
				break;
			case LightCand::Kind::kDynamic:
				SetSource(z, key.owner ? std::format("dynamic, owner {:08X}", key.owner) : std::string("dynamic (no owner: a spell's light)"));
				break;
			default:
				SetSource(z, std::format("static {:08X}", key.ref));
				break;
			}
			// a reflected light's direct term is the whole light (the engine lights the wall with all of it); a light not
			// reflected lights the walls as ambient: the sun there with its whole strength x the mean cos
			RGB         ambient = ambientBase;
			auto        add = [&](const RGB& c) { ambient = { ambient.r + c.r, ambient.g + c.g, ambient.b + c.b }; };
			for (std::size_t i = 0; i < cands.size(); ++i) {
				if (static_cast<int>(i) == pick.key) {
					continue;
				}
				const auto& c = cands[i];
				const float rel = std::min(c.intensity / keyI, 50.0f);  // 50: a sanity bound only
				const int   slot = pick.extra[0] == static_cast<int>(i) ? 0 : (pick.extra[1] == static_cast<int>(i) ? 1 : -1);
				if (slot < 0) {
					if (c.kind == LightCand::Kind::kSun) {
						const float b = c.intensity * kWallCos;
						add({ c.hue.r * b, c.hue.g * b, c.hue.b * b });
					} else {
						add(c.wall);
					}
					continue;
				}
				math::ExtraLight x;
				x.sun = c.kind == LightCand::Kind::kSun;
				x.pos = V(c.at);
				x.radius = x.sun ? 0.0f : c.radius;
				x.falloff = c.falloff;
				x.color = c.hue;
				x.level = rel;                         // its direct light, whole
				x.beam = x.sun ? c.beam : 1.0f;        // the sun as an extra reflects its beam only
				x.ref = x.sun ? kSunLitRef : (c.kind == LightCand::Kind::kStatic ? c.ref : 0);  // moving lights keep no lit plane
				z.extra[static_cast<std::size_t>(slot)] = x;
			}
			for (std::size_t s = 0; s < 2; ++s) {
				const int i = pick.extra[s];
				z.extraSlot[s] = i >= 0 ? math::SlotId{ static_cast<int>(cands[static_cast<std::size_t>(i)].kind), cands[static_cast<std::size_t>(i)].id } : math::SlotId{};
			}
			SetAmbient(z, ambient);
			z.ambientRel = std::clamp(z.ambientAbs / keyI, math::kMinAmbient, 10.0f);
			for (std::size_t a = 0; a < 3; ++a) {
				z.dalcPosRel[a] = std::clamp(z.dalcPosAbs[a] / keyI, 0.0f, 10.0f);
				z.dalcNegRel[a] = std::clamp(z.dalcNegAbs[a] / keyI, 0.0f, 10.0f);
			}
			z.keyValid = true;
		}

		void UpdateKeyLights()
		{
			const float range = Settings::Get().lightSearchRange;

			std::array<std::array<DynLight, 3>, math::kMaxDrawZones> best{};  // per GPU slot: the three strongest moving lights
			std::array<RGB, math::kMaxDrawZones>                   dynWall{};  // every moving light's wall light (the tops taken off later)
			auto*                                                   ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			// every light the renderer has on: the ordinary ones and the shadow casters (a list of their own)
			std::array<std::array<DynLight, 3>, math::kMaxDrawZones> fixedBest{};  // per GPU slot: the three strongest stationary Light Placer lights
			// why a light is left out (the debug dump), nullptr: a candidate
			auto visitLightWhy = [&](const RE::BSLight* bl, bool isShadow) -> const char* {
					if (!bl || bl->ambientLight) {
						return "an ambient light";
					}
					auto* nl = bl->light.get();
					if (!nl) {
						return "no NiLight";
					}
					auto*      ref = LightOwner(nl);  // the light's own ref, else the first up its parents
					const bool placedLigh = ref && ref->GetBaseObject() && ref->GetBaseObject()->Is(RE::FormType::Light);
					if (placedLigh) {
						const auto* lb = ref->GetBaseObject()->As<RE::TESObjectLIGH>();
						if (lb && lb->data.flags.any(RE::TES_LIGHT_FLAGS::kNegative)) {
							return "a negative lamp";  // a negative lamp (flagged, as the scan skips it)
						}
					}
					const LpLight lp = ReadLp(nl);
					if (lp.is && ((lp.flags & (1u << 9)) || (lp.flags >> 24))) {
						return "Light Placer: disabled or culled";  // Light Placer: disabled, or culled
					}
					if (placedLigh && g_radiusLogs < 8) {
						++g_radiusLogs;
						const auto* lb = ref->GetBaseObject()->As<RE::TESObjectLIGH>();
						logger::info("light radius check: placed {:08X} NiLight radius {:.1f} vs LIGH radius {} (ratio {:.3f}), NiLight fade {:.2f} vs LIGH fade {:.2f}",
							ref->GetFormID(), nl->GetLightRuntimeData().radius.x, lb ? lb->data.radius : 0u,
							lb && lb->data.radius ? nl->GetLightRuntimeData().radius.x / static_cast<float>(lb->data.radius) : 0.0f, nl->GetLightRuntimeData().fade,
							lb ? lb->fade : 0.0f);
					}
					RE::TESObjectREFR* owner = ref;
					// stationary: a Light Placer light on a plain placed object (not an actor, not a projectile)
					// stationary: a Light Placer light on a plain placed object, or a placed LIGH the scan did not count (a lamp
					// placed or enabled later)
					const bool stationary = (lp.is && owner && owner->Is(RE::FormType::Reference)) || (placedLigh && !lp.is);
					const bool isl = lp.is && (lp.flags & (1u << 10)) && Render().isl;
					const bool linearLight = lp.is && (lp.flags & (1u << 11)) && Render().cs;
					if (lp.is && g_lpLogs < 12) {
						++g_lpLogs;
						logger::info("Light Placer light '{}': flags {:08X}{}{}, cutoff {:.3f}, size {:.2f}, radius {:.1f}, fade {:.2f}, owner {:08X} ({})", nl->name.c_str(),
							lp.flags, isl ? " inverse-square" : "", linearLight ? " linear" : "", lp.cutoff, lp.size, nl->GetLightRuntimeData().radius.x,
							nl->GetLightRuntimeData().fade, owner ? owner->GetFormID() : 0u, stationary ? "stationary" : "moving");
					}
					const auto& d = nl->GetLightRuntimeData();
					if (d.fade < 0.0f || d.diffuse.red < 0.0f || d.diffuse.green < 0.0f || d.diffuse.blue < 0.0f) {
						return "negative light";  // negative light
					}
					const RE::NiPoint3 lpos = nl->world.translate;
					const RGB          col{ d.diffuse.red, d.diffuse.green, d.diffuse.blue };  // live: Light Placer's emittance rewrites it
					// CS: an inverse-square light shines at fade x 4 over its own radius; every light x the LOD dimmer
					if (!(d.fade > 0.0f) || !(bl->lodDimmer > 0.0f)) {
						return "off (fade 0 or dimmed to 0 by its LOD)";  // off, or faded out by its LOD
					}
					const float        fade = d.fade * (isl ? 4.0f : 1.0f) * bl->lodDimmer;
					const math::Falloff fo = isl ? math::CsInverseSquare(fade, lp.cutoff, lp.size, isShadow) : math::Falloff{};
					const LightKey      lkey = !stationary ? 0 :
					                           lp.is       ? ((static_cast<LightKey>(owner->GetFormID()) << 32) | lp.nameHash) :
					                                         static_cast<LightKey>(ref->GetFormID());  // a placed LIGH keys by its FormID, as the scan's lamps
					const char* why = "no zone";  // why it was left out where it was not taken (the dump)
					bool        taken = false;
					for (std::size_t k = 0; k < math::kMaxDrawZones; ++k) {
						const int zi = g_slotZone[k];
						if (zi < 0) {
							continue;
						}
						const auto& z = g_zones[static_cast<std::size_t>(zi)];
						if (placedLigh && std::ranges::binary_search(z.scannedLights, ref->GetFormID())) {
							why = "counted by the lamp scan";
							continue;  // a placed light the scan already counted (a magelight's light is placed later: not in it)
						}
						const float dist = ZoneDistance(z, lpos);
						if (dist > range) {
							why = "out of range";
							continue;
						}
						taken = true;
						const float weight = LightWeight(WaterDistance(z, lpos), d.radius.x, fade, col, fo, linearLight);
						const RGB   wall = WallLight(lpos.GetDistance(z.center), d.radius.x, fade, col, fo, linearLight);
						dynWall[k] = { dynWall[k].r + wall.r, dynWall[k].g + wall.g, dynWall[k].b + wall.b };
						const DynLight dl{ true, lpos, (linearLight && Render().linear ? NormalizeHue(col) : LightHue(col)), math::LightContribution(dist, d.radius.x, fade),
							isl ? fo.radius : d.radius.x, owner ? owner->GetFormID() : 0, lkey ? lkey : (owner ? (static_cast<std::uint64_t>(owner->GetFormID()) << 32) |
							                                           // two lights of one owner (a torch and a candlelight on the player) apart: its own name, or its own NiLight
							                                           (lp.is ? static_cast<std::uint64_t>(lp.nameHash) : (((reinterpret_cast<std::uintptr_t>(nl) >> 4) & 0xFFFFFFFFull) | 1ull)) :
							                                           reinterpret_cast<std::uintptr_t>(nl)),
							LightUnit(fade, col, false, linearLight), weight, wall, lkey, fo };
						// stationary lights (their own top 3, with lit planes) and moving ones (top 3) kept apart
						DynLight* top = stationary ? fixedBest[k].data() : best[k].data();
						const std::size_t n = stationary ? fixedBest[k].size() : best[k].size();
						for (std::size_t t = 0; t < n; ++t) {
							if (weight > top[t].weight) {
								for (std::size_t u = n - 1; u > t; --u) {
									top[u] = top[u - 1];
								}
								top[t] = dl;
								break;
							}
						}
					}
					return taken ? nullptr : why;
			};
			// the moving lights' dump (zones.json debug, once a second): what each light is, and why it is left out
			static double lastDump = -10.0;
			const double  dumpNow = NowSeconds();
			const bool    dump = Settings::Get().debug && dumpNow - lastDump >= 1.0;
			int           dumped = 0;
			if (dump) {
				lastDump = dumpNow;
				if (ssn) {
					const auto& rd = ssn->GetRuntimeData();
					logger::info("light dump: renderer lists active {}, shadow {}, queued add {}, queued remove {}, unk190 {}", rd.activeLights.size(),
						rd.activeShadowLights.size(), rd.lightQueueAdd.size(), rd.lightQueueRemove.size(), rd.unk190.size());
				}
			}
			auto visitLight = [&](const RE::BSLight* bl, bool isShadow) {
				const char* why = visitLightWhy(bl, isShadow);
				if (!dump || !bl || !bl->light || dumped >= 32 || (why && std::string_view(why) == "out of range")) {
					return;
				}
				++dumped;
				auto*       nl = bl->light.get();
				const auto& d = nl->GetLightRuntimeData();
				auto*       owner = LightOwner(nl);
				auto*       base = owner ? owner->GetBaseObject() : nullptr;
				float       toWater = -1.0f;
				for (std::size_t k = 0; k < math::kMaxDrawZones; ++k) {
					if (g_slotZone[k] >= 0) {
						toWater = WaterDistance(g_zones[static_cast<std::size_t>(g_slotZone[k])], nl->world.translate);
						break;
					}
				}
				logger::info("light dump: '{}' {} owner {:08X} (form type {}, base {:08X} type {}) pos ({:.0f}, {:.0f}, {:.0f}) {:.0f} from the water, radius {:.0f}, fade {:.2f}, "
							 "LOD dimmer {:.2f}, diffuse ({:.2f}, {:.2f}, {:.2f}) -> {}",
					nl->name.c_str(), isShadow ? "shadow list" : "light list", owner ? owner->GetFormID() : 0u, owner ? static_cast<int>(owner->GetFormType()) : -1,
					base ? base->GetFormID() : 0u, base ? static_cast<int>(base->GetFormType()) : -1, nl->world.translate.x, nl->world.translate.y,
					nl->world.translate.z, toWater, d.radius.x, d.fade, bl->lodDimmer, d.diffuse.red, d.diffuse.green, d.diffuse.blue, why ? why : "a candidate");
			};
			if (ssn) {
				for (const auto& bl : ssn->GetRuntimeData().activeLights) {
					visitLight(bl.get(), false);
				}
				for (const auto& bl : ssn->GetRuntimeData().activeShadowLights) {
					visitLight(bl.get(), true);
				}
			}

			math::Vec3 toSun{};
			bool       haveSun = false, sunTried = false;
			SkyLight   sky;

			for (std::size_t k = 0; k < math::kMaxDrawZones; ++k) {
				const int zi = g_slotZone[k];
				if (zi < 0) {
					continue;
				}
				auto&                  z = g_zones[static_cast<std::size_t>(zi)];
				std::vector<LightCand> cands;
				RGB                    ambientBase = z.ambientBase;  // the cell's ambient + placed lights below the top 3
				if (z.cell->IsExteriorCell()) {
					if (!sunTried) {
						haveSun = ToSun(toSun);
						sky = ReadSkyLight();
						sunTried = true;
					}
					if (sky.valid) {
						ambientBase = { ambientBase.r + sky.ambient.r, ambientBase.g + sky.ambient.g, ambientBase.b + sky.ambient.b };
						z.dalcPosAbs = sky.dalcPos;
						z.dalcNegAbs = sky.dalcNeg;
					}
					z.sunIntensity = sky.valid ? sky.intensity : 0.0f;
					if (haveSun && sky.valid && sky.intensity > 0.0f) {
						LightCand s;
						s.kind = LightCand::Kind::kSun;
						s.intensity = sky.intensity;
						s.beam = g_ext.beam;
						s.weight = sky.intensity * g_ext.beam;
						s.hue = sky.hue;
						s.at = { toSun[0], toSun[1], toSun[2] };
						s.ref = kSunLitRef;
						s.id = kSunLitRef;
						cands.push_back(s);
					}
				}
				{
					RGB rest = dynWall[k];  // lights past the tops (moving three, stationary three): ambient only
					for (const auto& dl : best[k]) {
						if (dl.valid) {
							rest = { rest.r - dl.wall.r, rest.g - dl.wall.g, rest.b - dl.wall.b };
						}
					}
					for (const auto& dl : fixedBest[k]) {
						if (dl.valid) {
							rest = { rest.r - dl.wall.r, rest.g - dl.wall.g, rest.b - dl.wall.b };
						}
					}
					ambientBase = { ambientBase.r + std::max(rest.r, 0.0f), ambientBase.g + std::max(rest.g, 0.0f), ambientBase.b + std::max(rest.b, 0.0f) };
				}
				const double now = NowSeconds();
				for (const auto& dl : best[k]) {
					if (dl.valid) {
						LightCand c;
						c.kind = LightCand::Kind::kDynamic;
						c.weight = dl.weight;
						c.intensity = dl.intensity;
						c.hue = dl.color;
						c.at = dl.pos;
						c.radius = dl.radius;
						c.ref = 0;  // moving: no lit plane
						c.id = dl.id;
						c.owner = dl.refID;
						c.wall = dl.wall;
						c.falloff = dl.falloff;
						Deflicker(static_cast<std::size_t>(zi), c, now);
						cands.push_back(c);
					}
				}
				for (const auto& dl : fixedBest[k]) {
					if (dl.valid) {
						LightCand c;
						c.kind = LightCand::Kind::kStatic;  // stationary: ranked and shadowed like a placed lamp
						c.weight = dl.weight;
						c.intensity = dl.intensity;
						c.hue = dl.color;
						c.at = dl.pos;
						c.radius = dl.radius;
						c.ref = dl.key;
						c.id = dl.key;
						c.wall = dl.wall;
						c.falloff = dl.falloff;
						Deflicker(static_cast<std::size_t>(zi), c, now);  // a Light Placer light is live too (its flicker)
						cands.push_back(c);
					}
				}
				for (int i = 0; i < z.staticTopCount; ++i) {
					const auto& sl = z.staticTop[static_cast<std::size_t>(i)];
					LightCand   c;
					c.kind = LightCand::Kind::kStatic;
					c.weight = sl.score;  // the scan's ranking weight
					c.intensity = sl.intensity;
					c.hue = sl.color;
					c.at = sl.pos;
					c.radius = sl.radius;
					c.ref = sl.ref;
					c.id = sl.ref;
					c.wall = sl.ambient;
					cands.push_back(c);
				}
				// the interior light level (logs, the cell-lighting fallback) with the strongest moving light folded in
				if (z.cell->IsInteriorCell()) {
					z.interior = best[k][0].valid ? math::FoldLight(z.interiorStatic, best[k][0].color, best[k][0].score) : z.interiorStatic;
				}
				ChooseLights(z, std::move(cands), ambientBase);
				if (z.automatic) {
					z.mask = AlignedPlanes(z, z.mask);  // a light that changed slots takes its plane along now, not at the next shadow step
				}
			}
			const double now = NowSeconds();
			std::erase_if(g_flicker, [&](const auto& kv) { return now - kv.second.seen > 2.0; });
		}

		// ---- output tick ---------------------------------------------------------------

		void OutputTick(float dt, bool logNow)
		{
			const auto& s = Settings::Get();

			std::array<math::ZoneGpuInput, math::kMaxDrawZones> inputs{};
			std::array<bool, math::kMaxDrawZones>               used{};
			std::size_t                                         n = 0;

			for (std::size_t k = 0; k < math::kMaxDrawZones; ++k) {
				const int zi = g_slotZone[k];
				if (zi < 0) {
					continue;
				}
				auto& z = g_zones[static_cast<std::size_t>(zi)];
				z.impulse = math::DecayImpulse(z.impulse, dt, s.impulseDecaySeconds);
				z.distFade = math::DistanceFade(z.cameraDistance, CausticDistance());

				float targetLevel = 0.0f;
				RGB   targetColor{ 1.0f, 1.0f, 1.0f };
				float weather = 0.0f;
				if (z.cell->IsExteriorCell()) {
					weather = g_ext.weatherAgitation;
					// the share is already relative to the sun's current strength (key intensity): dimming it again by the
					// time of day and the weather would count them twice. Off with the outdoor switch (manual zones too).
					// the reflected beam only (the numerator): the direct term the share divides by stays what the
					// engine really puts on the wall
					targetLevel = s.autoZonesOutdoors ? (z.keySun ? z.keyBeam : 1.0f) : 0.0f;  // the sun reflects its beam only (extras keep their own level)
					targetColor = z.keyColor;  // the sun (or moon) light's own colour, as its strength
				} else {
					// the share is already relative to the lights' own strength (key units): the level only switches the zone on
					// when anything lights it. The colour is the key light's own (the extras carry theirs).
					targetLevel = z.keyValid ? 1.0f : 0.0f;
					targetColor = z.keyColor;
				}
				// the surface's RMS slope from its sources (their slope variances add); the agitation used for the
				// pattern crossfade and speed is that slope, normalised
				const bool  outside = z.cell->IsExteriorCell();
				const float targetSlope = math::SlopeAngle(math::SlopeMeanSquare(z.cfg.baseAgitation, outside ? g_ext.wind01 : 0.0f,
					outside ? g_ext.rain01 : 0.0f, z.impulse));
				const float targetAgitation = math::AgitationFromSlope(targetSlope);

				if (z.keySnap) {  // just became active: start from the current inputs
					z.agitation = targetAgitation;
					z.speed = math::PlaybackSpeed(targetAgitation);
					z.level = targetLevel;
					z.color = targetColor;
				} else {
					z.agitation = math::SmoothToward(z.agitation, targetAgitation, dt, kAgitationTau);
					z.speed = math::SmoothToward(z.speed, math::PlaybackSpeed(z.agitation), dt, kAgitationTau);
					const bool relabel = z.keyJump && !z.keyFromNone;  // a key swap between two lights, not a light coming on
					z.level = relabel ? targetLevel : math::SmoothToward(z.level, targetLevel, dt, kLightTau);  // with the key, as its colour
					// another light as the key: its colour with its position at once (the old key, now an extra, carries its
					// own), so the swap only relabels the same lights
					z.color = relabel ? targetColor : math::SmoothToward(z.color, targetColor, dt, kLightTau);
				}
				z.weights = math::CrossfadeWeights(z.agitation);
				z.inWeather = weather;
				z.inTargetAgitation = targetAgitation;
				z.inTargetLevel = targetLevel;

				if (z.keyValid) {
					if (z.keySnap || z.keyJump) {
						z.keyPos = V(z.keyTarget);
						z.keySnap = false;
						z.keyJump = false;
					} else {
						z.keyPos = math::SmoothToward(z.keyPos, V(z.keyTarget), dt, kKeyTau);
					}
				}

				const bool haveActor = z.rippleTargetAmp > 0.0f;
				z.ripple = math::StepRipple(z.ripple, z.rippleTargetAmp, haveActor, z.rippleTargetX, z.rippleTargetY, dt,
					s.impulseDecaySeconds, math::kRipplePosTau);

				auto& in = inputs[k];
				used[k] = true;
				++n;
				in.rect = z.mask ? z.mask->bounds : z.rect;
				in.mask = z.mask;
				for (std::size_t e = 0; e < 2; ++e) {
					in.useLitExtra[e] = z.mask && !z.mask->litMips[e + 1].empty() && z.extra[e].ref != 0 && z.litExtraRef[e] == z.extra[e].ref &&
					                    (!z.extra[e].sun || DegreesBetween(z.litExtraDir[e], { z.extra[e].pos[0], z.extra[e].pos[1], z.extra[e].pos[2] }) < kSunUseDegrees);
				}
				in.keyFalloff = z.keyFalloff;
				in.useLit = z.mask && !z.mask->litMips[0].empty() &&
				            ((z.keyIsStatic && z.keyPlaneKey != 0 && z.litKeyRef == z.keyPlaneKey) ||
								(z.keySun && z.litKeyRef == kSunLitRef && DegreesBetween(z.litSunDir, z.keyTarget) < kSunUseDegrees));
				// a light that gets a lit plane (the automatic zone's lamps and sun) reflects nothing until its plane is
				// up: not everywhere for a moment (the flash through walls). Moving lights have no plane: unshadowed.
				// (a sun / moon plane cast for another direction is not waiting: the pass reflects it unshadowed until renewed)
				in.planePending[0] = z.automatic && z.keyValid &&
				                     (z.keySun ? (z.litKeyRef != kSunLitRef || (!in.useLit && PlaneRecasting(kSunLitRef))) :
				                                 (z.keyIsStatic && z.keyPlaneKey != 0 && !in.useLit && !PlaneWobbly(z.keyPlaneKey)));
				for (std::size_t e = 0; e < 2; ++e) {
					const auto& x = z.extra[e];
					in.planePending[e + 1] = z.automatic && x.ref != 0 && x.level > 0.0f && !in.useLitExtra[e] &&
					                         (x.sun ? (z.litExtraRef[e] != x.ref || PlaneRecasting(x.ref)) : !PlaneWobbly(x.ref));
				}
				in.waterZ = z.cfg.waterZ;
				in.height = z.cfg.height;
				in.spread = math::ReflectionSpread(z.agitation);  // = tan(2 sigma) of the smoothed slope (agitation = slope 3..14 deg)
				in.ambient = z.ambientRel;
				in.ambientHue = z.ambientHue;
				in.ambientPos = z.dalcPosRel;
				in.ambientNeg = z.dalcNegRel;
				in.absorb = z.absorb;
				in.occlusion = true;
				in.distFade = z.distFade;
				in.rippleContrast = Settings::Get().rippleContrast;  // CausticLight.ini
				in.floorScale = Settings::Get().floorStrength;
				in.pixelFadeDistance = CausticDistance();  // each pixel fades as the shadows do
				in.sun = z.keySun;
				in.light = z.keyPos;
				in.lightRadius = z.keyRadius;
				in.lightColor = z.color;
				in.level = z.keyValid ? z.level : 0.0f;  // no key light yet: contribute nothing
				for (std::size_t e = 0; e < in.extra.size(); ++e) {
					in.extra[e] = z.extra[e];
					in.extra[e].level *= in.level;
				}
				in.brightness = z.cfg.brightness;
				in.weights = z.weights;
				in.frameClock = z.frameClock;  // the hook overwrites it every frame
				in.rippleX = z.ripple.x;
				in.rippleY = z.ripple.y;
				in.rippleAmp = z.ripple.amplitude;
				in.rippleRadius = math::kRippleRadius;
				in.debugTint = g_debugTint;
				in.debugTintStrength = s.debugTintStrength;
				in.debugLight = g_debugLight;
				in.debugDirect = g_debugDirect;
				in.strength = g_strength;

				if (logNow && s.debug) {
					const std::string who = z.rippleTargetAmp <= 0.0f ? std::string("none") :
					                        z.rippleActorID == 0x14   ? std::string("player") :
					                                                    std::format("{:08X}", z.rippleActorID);
					logger::info(
						"[{}] slot {} rect ({:.0f},{:.0f})-({:.0f},{:.0f}) waterZ {:.1f} cam {:.0f} fade {:.2f} | agitation {:.2f} "
						"(base {:.2f} weather {:.2f} impulse {:.2f}, {} actor(s)) w {:.2f}/{:.2f}/{:.2f} speed {:.2f} clock {:.1f}",
						z.cfg.name, k, z.rect.minX, z.rect.minY, z.rect.maxX, z.rect.maxY, z.cfg.waterZ, z.cameraDistance, z.distFade,
						z.agitation, z.cfg.baseAgitation, weather, z.impulse, z.inActors, z.weights.calm, z.weights.normal,
						z.weights.rough, z.speed, z.frameClock);
					logger::info("[{}] key light {} {} ({:.1f}, {:.1f}, {:.1f}) radius {:.0f} | light source {} level {:.2f} colour "
								 "({:.2f},{:.2f},{:.2f}) x brightness {:.2f} | ripple {} ({:.0f}, {:.0f}) amp {:.2f}",
						z.cfg.name, z.keyValid ? z.keySource.data() : "none yet", z.keySun ? "dir" : "pos", z.keyPos[0], z.keyPos[1],
						z.keyPos[2], z.keyRadius, z.cell->IsExteriorCell() ? "sky" : z.lightSource, z.level, z.color.r, z.color.g,
						z.color.b, z.cfg.brightness, who, z.ripple.x, z.ripple.y, z.ripple.amplitude);
					if (z.cell->IsExteriorCell()) {
						auto*       sky = RE::Sky::GetSingleton();
						auto*       sl = sky && sky->sun ? sky->sun->light.get() : nullptr;
						const auto  d = sl ? sl->GetLightRuntimeData().diffuse : RE::NiColor{};
						const auto  sc = sky ? sky->skyColor[RE::TESWeather::ColorTypes::kSunlight] : RE::NiColor{};
						const float elev = std::asin(std::clamp(z.keyTarget.z / std::max(z.keyTarget.Length(), 1e-6f), -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
						logger::info("[{}] sky: direct beam {:.2f} (weather{}{}{}{}, transition {:.2f}), sun light colour lum {:.3f}, sky sunlight colour lum {:.3f}, "
									 "elevation {:.1f} deg{}",
							z.cfg.name, g_ext.beam, g_ext.rainy ? " rainy" : "", g_ext.snowy ? " snowy" : "", g_ext.cloudy ? " cloudy" : "",
							(g_ext.rainy || g_ext.snowy || g_ext.cloudy) ? "" : " clear", g_ext.pct, 0.2126f * d.red + 0.7152f * d.green + 0.0722f * d.blue,
							0.2126f * sc.red + 0.7152f * sc.green + 0.0722f * sc.blue, elev, g_sunDown ? " (the moon)" : "");
					}
					logger::info("[{}] {} zone, water texels {}, spread {:.2f}, extra lights {:.2f} / {:.2f}, ambient {:.3f} + directional "
								 "+x {:.3f} -x {:.3f} +y {:.3f} -y {:.3f} +z {:.3f} -z {:.3f} (key units, key intensity {:.2f}), floor e-fold {:.0f}, lit plane {}",
						z.cfg.name, z.automatic ? "automatic" : "manual", z.mask ? z.mask->waterTexels : 0, in.spread, in.extra[0].level,
						in.extra[1].level, in.ambient, in.ambientPos[0], in.ambientNeg[0], in.ambientPos[1], in.ambientNeg[1], in.ambientPos[2],
						in.ambientNeg[2], z.keyIntensity, in.absorb,
						std::format("{}/{}/{}", in.useLit ? "on" : "off", in.useLitExtra[0] ? "on" : "off", in.useLitExtra[1] ? "on" : "off"));
				}
			}

			DrawPass::Stage(inputs, used);
			if (logNow && s.debug) {
				logger::info("draw pass: {} active zone(s), pass {}, calm atlas {}", n, DrawPass::Ready() ? "ready" : "NOT ready",
					DrawPass::AtlasReady() ? "loaded" : "missing");
				DrawPass::LogSecond();
			}
		}

		// ---- events --------------------------------------------------------------------

		class CellLoadedSink final : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
		{
		public:
			static CellLoadedSink* Get()
			{
				static CellLoadedSink s;
				return &s;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* e,
				RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
			{
				if (e && e->cell) {
					g_forcePlacement.store(true);  // flag only; the task checks the zones
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// ---- authoring (console): CL_Corner twice = two corners of a water rect, CL_Remove removes

		constexpr float kAuthorRayLength = 1500.0f;
		constexpr float kRemoveRadius = 300.0f;

		struct PendingCorner
		{
			float              x{ 0.0f };
			float              y{ 0.0f };
			float              waterZ{ 0.0f };
			bool               waterFromCell{ false };
			RE::TESObjectCELL* cell{ nullptr };
		};
		std::optional<PendingCorner> g_pendingCorner;

		bool MenuBlocksInput()
		{
			auto* ui = RE::UI::GetSingleton();
			return !ui || ui->GameIsPaused() || ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen() ||
			       ui->IsModalMenuOpen() || ui->IsMenuOpen(RE::Console::MENU_NAME);
		}

		struct CameraRay
		{
			bool         ok{ false };
			RE::NiPoint3 origin;
			RE::NiPoint3 dir;
		};

		// Forward = the camera root node's local +Y in world space (second matrix column).
		CameraRay GetCameraRay()
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (!cam || !cam->cameraRoot) {
				return {};
			}
			const auto&  w = cam->cameraRoot->world;
			RE::NiPoint3 dir{ w.rotate.entry[0][1], w.rotate.entry[1][1], w.rotate.entry[2][1] };
			const float  len = dir.Length();
			if (len < 1e-4f) {
				return {};
			}
			return { true, w.translate, dir * (1.0f / len) };
		}

		struct RayHit
		{
			bool         ok{ false };
			RE::NiPoint3 point;
			int          skippedActors{ 0 };
		};

		// Havok ray through the cell's physics world; actors are skipped by re-casting past them.
		RayHit CastRay(RE::TESObjectCELL* cell, RE::NiPoint3 from, const RE::NiPoint3& dir, float length)
		{
			RayHit out;
			auto*  world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return out;
			}
			const float s = RE::bhkWorld::GetWorldScale();
			float       remaining = length;

			for (int attempt = 0; attempt < 5 && remaining > 1.0f; ++attempt) {
				const RE::NiPoint3 to = from + dir * remaining;

				RE::bhkPickData pick;
				pick.rayInput.from = RE::hkVector4(from.x * s, from.y * s, from.z * s, 0.0f);
				pick.rayInput.to = RE::hkVector4(to.x * s, to.y * s, to.z * s, 0.0f);
				pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
				{
					RE::BSReadLockGuard lock(world->worldLock);
					world->PickObject(pick);
				}
				if (!pick.rayOutput.HasHit()) {
					return out;
				}
				const float        f = pick.rayOutput.hitFraction;
				const RE::NiPoint3 p = from + (to - from) * f;

				RE::TESObjectREFR* ref = nullptr;
				if (pick.rayOutput.rootCollidable) {
					ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
				}
				if (ref && ref->As<RE::Actor>()) {
					++out.skippedActors;
					remaining -= remaining * f + 8.0f;
					from = p + dir * 8.0f;
					continue;
				}
				out.ok = true;
				out.point = p;
				return out;
			}
			return out;
		}

		bool WaterHeightAt(RE::TESObjectCELL* cell, const RE::NiPoint3& at, float& height)
		{
			if (cell->GetWaterHeight(at, height) && std::isfinite(height) && std::fabs(height) < 1e7f) {
				return true;
			}
			if (cell->IsInteriorCell() && cell->cellFlags.any(RE::TESObjectCELL::Flag::kHasWater)) {
				height = cell->GetRuntimeData().waterHeight;
				return std::isfinite(height) && std::fabs(height) < 1e7f;
			}
			return false;
		}

		std::string FormKey(const RE::TESForm* form, bool preferEditorID)
		{
			if (!form) {
				return {};
			}
			if (preferEditorID) {
				const char* ed = form->GetFormEditorID();
				if (ed && *ed) {
					return ed;
				}
			}
			const auto* file = form->GetFile(0);
			if (!file) {
				return std::format("0x{:X}~?", form->GetFormID());
			}
			return std::format("0x{:X}~{}", form->GetLocalFormID(), file->GetFilename());
		}

		std::string UniqueZoneName(const RE::TESObjectCELL* cell)
		{
			const char*       ed = cell->GetFormEditorID();
			const std::string stem = (ed && *ed) ? std::string(ed) : std::format("{:08X}", cell->GetFormID());
			for (int n = 1;; ++n) {
				auto       name = std::format("{}_{}", stem, n);
				const bool taken = std::any_of(g_zones.begin(), g_zones.end(), [&](const Zone& z) { return z.cfg.name == name; });
				if (!taken) {
					return name;
				}
			}
		}

		// Crosshair hit and the cell it belongs to (exterior: the cell containing the hit).
		bool AimAt(RE::NiPoint3& hitOut, RE::TESObjectCELL*& cellOut)
		{
			auto*      player = RE::PlayerCharacter::GetSingleton();
			auto*      cell = player ? player->GetParentCell() : nullptr;
			const auto ray = GetCameraRay();
			if (!cell || !cell->IsAttached() || !ray.ok) {
				logger::warn("authoring: no camera or cell");
				return false;
			}
			const auto hit = CastRay(cell, ray.origin, ray.dir, kAuthorRayLength);
			if (!hit.ok) {
				logger::info("authoring: nothing hit within {:.0f} units", kAuthorRayLength);
				return false;
			}
			if (cell->IsExteriorCell()) {
				auto* tes = RE::TES::GetSingleton();
				auto* hitCell = tes ? tes->GetCell(hit.point) : nullptr;
				if (hitCell && hitCell->IsExteriorCell() &&
					hitCell->GetRuntimeData().worldSpace == cell->GetRuntimeData().worldSpace) {
					cell = hitCell;
				} else {
					logger::warn("authoring: could not look up the cell at the hit point - using the player's cell");
				}
			}
			hitOut = hit.point;
			cellOut = cell;
			return true;
		}

		void CornerAction()
		{
			if (MenuBlocksInput()) {
				return;
			}
			RE::NiPoint3       hit;
			RE::TESObjectCELL* cell = nullptr;
			if (!AimAt(hit, cell)) {
				return;
			}
			float      water = 0.0f;
			const bool hasWater = WaterHeightAt(cell, hit, water);

			if (!g_pendingCorner || g_pendingCorner->cell != cell) {
				if (g_pendingCorner) {
					logger::info("authoring: second corner is in another cell - starting over with this point as the first corner");
				}
				g_pendingCorner = PendingCorner{ hit.x, hit.y, hasWater ? water : hit.z, hasWater, cell };
				logger::info("authoring: first corner ({:.1f}, {:.1f}) in cell {:08X}, waterZ {:.1f} ({}) - aim at the opposite corner and set CL_Corner to 1 again",
					hit.x, hit.y, cell->GetFormID(), g_pendingCorner->waterZ, hasWater ? "cell water height" : "hit z, no water in cell");
				return;
			}

			const PendingCorner first = *g_pendingCorner;
			g_pendingCorner.reset();

			const auto rect = math::RectFromCorners(first.x, first.y, hit.x, hit.y);
			if (!rect) {
				logger::info("authoring: rect ({:.1f}, {:.1f})-({:.1f}, {:.1f}) is smaller than {:.0f} units on a side - not added; start again",
					first.x, first.y, hit.x, hit.y, math::kMinZoneSide);
				return;
			}
			for (const auto& z : g_zones) {
				if (!z.automatic && SameSpace(z, cell) && math::RectsOverlap(z.rect, *rect)) {
					logger::info("authoring: the new rect overlaps zone '{}' - not added", z.cfg.name);
					return;
				}
			}

			ZoneConfig cfg;
			cfg.name = UniqueZoneName(cell);
			cfg.cell = FormKey(cell, cell->IsInteriorCell());
			if (cell->IsExteriorCell()) {
				cfg.worldspace = FormKey(cell->GetRuntimeData().worldSpace, false);
			}
			cfg.waterZ = first.waterZ;
			cfg.rect = { rect->minX, rect->minY, rect->maxX, rect->maxY };

			const bool saved = Settings::Get().AppendZone(cfg);
			g_zones.push_back(MakeZone(cfg, g_zones.size()));
			PollActive();
			logger::info("authoring: added zone '{}' cell '{}' rect ({:.1f}, {:.1f})-({:.1f}, {:.1f}) waterZ {:.1f} ({}) | {}", cfg.name,
				cfg.cell, rect->minX, rect->minY, rect->maxX, rect->maxY, cfg.waterZ,
				first.waterFromCell ? "cell water height" : "first hit z", saved ? "saved to zones.json" : "NOT saved (see error above)");
		}

		void RemoveAction()
		{
			if (MenuBlocksInput()) {
				return;
			}
			RE::NiPoint3       hit;
			RE::TESObjectCELL* cell = nullptr;
			if (!AimAt(hit, cell)) {
				return;
			}
			std::size_t best = g_zones.size();
			float       bestDist = kRemoveRadius;
			for (std::size_t i = 0; i < g_zones.size(); ++i) {
				const auto& z = g_zones[i];
				if (z.automatic || !SameSpace(z, cell)) {
					continue;
				}
				const float d = math::DistanceToRect2D(z.rect, hit.x, hit.y);  // 0 when inside
				if (d < bestDist || (d == 0.0f && bestDist > 0.0f)) {
					bestDist = d;
					best = i;
				}
			}
			if (best == g_zones.size()) {
				logger::info("authoring: no zone contains or is within {:.0f} units of ({:.0f}, {:.0f})", kRemoveRadius, hit.x, hit.y);
				return;
			}
			const std::string name = g_zones[best].cfg.name;
			g_zones.erase(g_zones.begin() + static_cast<std::ptrdiff_t>(best));
			PollActive();  // slots index g_zones: recompute right away
			const bool saved = Settings::Get().RemoveZone(name);
			logger::info("authoring: removed zone '{}' ({:.0f} units from the crosshair hit) | {}", name, bestDist,
				saved ? "zones.json updated" : "zones.json NOT updated (see error above)");
		}

		// ESP forms are optional now (the plugin places nothing); logged for the load order.
		void LogBaseForms(const char* a_when)
		{
			if (g_panelBase && g_lightBase) {
				return;
			}
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			const auto* file = dh->LookupModByName(kPluginName);
			const auto  panelID = dh->LookupFormID(kPanelLocalID, kPluginName);
			const auto  lightID = dh->LookupFormID(kLightLocalID, kPluginName);
			auto*       panel = panelID ? RE::TESForm::LookupByID(panelID) : nullptr;
			auto*       light = lightID ? RE::TESForm::LookupByID(lightID) : nullptr;
			g_panelBase = panel ? panel->As<RE::TESBoundObject>() : nullptr;
			g_lightBase = light ? light->As<RE::TESBoundObject>() : nullptr;
			logger::info("[{}] {} (optional): file {}; forms 0x{:X} {} / 0x{:X} {}", a_when, kPluginName, file ? "found" : "not loaded",
				kPanelLocalID, g_panelBase ? "found" : "absent", kLightLocalID, g_lightBase ? "found" : "absent");
		}

		// ---- automatic zone: a water mask from a ray grid around the player -------------------
		//
		// While the player's cell has water and no manual zone of that cell is active, rays are cast
		// straight down on a 64 x 64 grid (2560 x 2560 units) around the player, starting a little
		// above the water: a ray that reaches the water plane or goes below it is over water, one that
		// stops on a floor above it is dry. Coarse to fine (math::ScanOrder): every 4th texel first, and
		// the zone goes up from that preview; texels between samples that all agree are filled without a
		// ray, so rays go only along the shore (~1/3 of the grid in the offline pool test). While a grid is
		// being scanned one step runs every frame and casts rays for at most kAutoStepBudgetMs. After the
		// player moves 640 units the grid is re-centred and the overlap reused: only the new strip is cast.
		// The zone is runtime only: never written to zones.json.

		constexpr int   kAutoSize = math::kMaskSize;
		constexpr float kAutoHalf = 1280.0f;
		constexpr float kAutoRescan = 640.0f;
		constexpr double kAutoStepBudgetMs = 2.0;    // ray time per frame while a grid is being scanned
		constexpr double kAutoFirstBudgetMs = 25.0;  // first step after a load (the screen is still fading in)
		constexpr float kAutoRayStart = 40.0f;
		constexpr float kAutoRayLength = 1500.0f;
		constexpr float kAutoMaxDepth = 350.0f;  // a floor deeper than this under the water plane (5 m) is another room, not a pool
		constexpr float kAutoMaxAboveWater = 1500.0f;  // player this far above the water: nothing to light
		constexpr float kAutoOutRayLength = 4000.0f;   // outdoors: no depth limit (a lake bottom can be far down)

		// Where a scan or zone is, remembered without keeping a cell pointer to read later (an exterior cell
		// the player left can be unloaded): the interior cell's address (compared only), or the worldspace.
		struct Space
		{
			const RE::TESObjectCELL*  interior{ nullptr };
			const RE::TESWorldSpace*  world{ nullptr };
			bool operator==(const Space&) const = default;
			explicit operator bool() const { return interior || world; }
		};

		// cell must be live (the player's cell, or one just checked)
		Space SpaceOf(const RE::TESObjectCELL* cell)
		{
			if (!cell) {
				return {};
			}
			if (cell->IsExteriorCell()) {
				return { nullptr, cell->GetRuntimeData().worldSpace };
			}
			return { cell, nullptr };
		}

		struct AutoScan
		{
			bool                      running{ false };
			RE::TESObjectCELL*        cell{ nullptr };
			float                     waterZ{ 0.0f };
			math::Rect                bounds;
			RE::NiPoint3              center;
			int                       next{ 0 };     // position in math::ScanOrder()
			std::vector<std::int8_t>  states;        // -1 unknown, 0 dry, 1 water
			bool                      previewed{ false };
			int                       inferred{ 0 };
			std::vector<std::int8_t>  lastStates;    // the finished previous grid (reused after a move)
			math::Rect                lastBounds;
			Space                     space;         // of the running scan (cell is refreshed to the player's)
			Space                     lastSpace;
			float                     lastWaterZ{ 0.0f };
			bool                      haveDone{ false };
			Space                     doneSpace;
			bool                      doneHadWater{ false };
			Space                     zoneSpace;     // of the automatic zone
			RE::NiPoint3              doneCenter;
			bool                      fresh{ true };  // no step yet since the last load
			// shadow pass: per reflected light, one ray from each water texel to it. The states outlive a grid move
			// (shifted with it) and a zone that drops out, so only new water is cast again.
			struct LitTrack
			{
				LightKey                 ref{ 0 };   // kSunLitRef: the sun / moon
				RE::NiPoint3             at;         // the light's position, or the direction toward the sun
				std::vector<std::int8_t> st;         // -1 not cast, 0 in shadow, 1 lit (grid texels)
				int                      next{ 0 };  // scan position
				bool                     dirty{ false };
				bool                     published{ false };  // cast through once for this light: its plane can be put on a mask
				bool                     recast{ false };     // the sun moved: cast every water texel again (the old states stand in meanwhile)
				int                      rays{ 0 };
				double                   ms{ 0.0 };
				std::uint32_t            version{ 0 };       // a new serial at each publish (the planes on the mask follow it)
				RE::NiPoint3             castAt;             // where the published states were cast for (at moves as soon as a recast starts)
				int                      litCount{ -1 };     // water texels it reaches at the last publish (-1 not known)
				std::uint32_t            lastWanted{ 0 };    // the shadow step it was last wanted in (the oldest parked one is reused)
				bool                     darkLogged{ false };
				int                      restarts{ 0 };       // moved again before its recast was done (PlaneWobbly)
				std::uint32_t            movedTick{ 0 };      // the shadow step of its last move
			};
			bool shadowRunning{ false };  // a lit plane has texels still to cast
			// a pool keyed by the light, not by the slot: a light that moves to another slot (the extras trade places, a
			// lamp becomes the key) takes its planes along, and one that drops out is parked, not forgotten.
			// 8 is assumed: every light that can want a plane at once (the sun, three stationary and three scanned lamps,
			// the spare key) plus one, so a lamp left out for reaching no water is not forgotten and cast again.
			std::array<LitTrack, 8> tracks;
			std::uint32_t           shadowTick{ 0 };
			std::uint32_t           planeSerial{ 0 };
			std::shared_ptr<const math::WaterMask>             alignedMask;  // the mask the planes were last put on
			std::array<std::pair<LightKey, std::uint32_t>, 3> onMask{};     // ... the light and version on each plane
			math::Rect              litBounds;
			Space                   litSpace;
			int                       rays{ 0 };
			int                       hitSurface{ 0 }, hitBottom{ 0 }, hitDry{ 0 }, tooDeep{ 0 }, noHit{ 0 };  // ray outcomes (log)
			int                       landAbove{ 0 }, otherLevel{ 0 }, notLoaded{ 0 };  // outdoors: decided without a ray (log)
			double                    rayMs{ 0.0 };
			std::chrono::steady_clock::time_point started{};
		};
		AutoScan g_auto;

		int AutoZoneIndex()
		{
			for (std::size_t i = 0; i < g_zones.size(); ++i) {
				if (g_zones[i].automatic) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		bool DisableAutoZone(const char* why)
		{
			const int i = AutoZoneIndex();
			if (i < 0 || !g_zones[static_cast<std::size_t>(i)].cell) {
				return false;
			}
			auto& z = g_zones[static_cast<std::size_t>(i)];
			logger::info("[{}] automatic zone off: {}", z.cfg.name, why);
			z.cell = nullptr;  // kept in place (slots index g_zones); PollActive skips it
			z.mask.reset();
			return true;
		}


		// Disabled for a reason other than "no water found": forget the last grid, so the zone is
		// rebuilt as soon as the reason goes away (even on the same spot).
		bool StopAutoZone(const char* why)
		{
			g_auto.running = false;
			g_auto.haveDone = false;
			return DisableAutoZone(why);
		}

		std::string ScanTiming()
		{
			const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_auto.started).count();
			return std::format("{} rays (surface {}, pool bottom {}, dry floor {}, too deep {}, no hit {}) + {} inferred + outdoors without a ray "
							   "(land above the water {}, another water level {}, not loaded {}) in {:.1f} ms of ray time ({:.1f} us each), done {:.2f} s after the scan started",
				g_auto.rays, g_auto.hitSurface, g_auto.hitBottom, g_auto.hitDry, g_auto.tooDeep, g_auto.noHit, g_auto.inferred, g_auto.landAbove,
				g_auto.otherLevel, g_auto.notLoaded, g_auto.rayMs,
				g_auto.rays ? 1000.0 * g_auto.rayMs / g_auto.rays : 0.0, wall);
		}

		bool PublishAutoZone(std::shared_ptr<const math::WaterMask> mask, const char* what);

		// The automatic zone's cell: the player's own while in the same space, dropped when the space is left,
		// so no exterior cell the player walked away from is ever read. true: the zone was dropped.
		bool RefreshAutoZoneCell(RE::TESObjectCELL* cell)
		{
			const int ai = AutoZoneIndex();
			if (ai < 0 || !cell) {
				return false;
			}
			auto& az = g_zones[static_cast<std::size_t>(ai)];
			if (!az.cell || az.cell == cell) {
				return false;
			}
			if (g_auto.zoneSpace == SpaceOf(cell)) {
				if (cell->IsExteriorCell()) {
					az.cell = cell;
				}
				return false;
			}
			if (g_auto.zoneSpace.world) {
				DisableAutoZone("left the worldspace");
				g_auto.zoneSpace = {};
				return true;
			}
			return false;
		}

		bool RefreshAutoZoneCell()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			return RefreshAutoZoneCell(player ? player->GetParentCell() : nullptr);
		}

		// One step of the automatic zone. true: the zone list changed (re-poll the active set).
		bool AutoZoneStep()
		{
			if (!Settings::Get().autoZones) {
				return StopAutoZone("autoZones is off in zones.json");
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!cell || !cell->IsAttached()) {
				return false;
			}
			if (cell->IsExteriorCell()) {
				if (!Settings::Get().autoZonesOutdoors) {
					return StopAutoZone("outdoors is off (autoZonesOutdoors in zones.json)");
				}
				// the player walks from cell to cell: the scan and the zone use the player's own (loaded) cell
				if (g_auto.running && g_auto.space == SpaceOf(cell)) {
					g_auto.cell = cell;
				}
			}
			if (RefreshAutoZoneCell(cell)) {
				return true;
			}
			const RE::NiPoint3 pos = player->GetPosition();

			if (!g_auto.running) {
				for (const auto& z : g_zones) {
					if (!z.automatic && z.active && SameSpace(z, cell)) {
						return StopAutoZone("a manual zone covers this cell");
					}
				}
				float      wz = 0.0f;
				const bool known = g_auto.haveDone && g_auto.doneHadWater && g_auto.doneSpace == SpaceOf(cell);
				if (!WaterHeightAt(cell, pos, wz)) {
					// cells with placed water planes answer only over a plane: off it, keep the level found before
					if (!known) {
						return StopAutoZone("no water plane under the player (none found here yet)");
					}
					wz = g_auto.lastWaterZ;
				}
				if (pos.z - wz > kAutoMaxAboveWater) {
					return StopAutoZone("far above the water");
				}
				if (g_auto.haveDone && g_auto.doneSpace == SpaceOf(cell) && wz == g_auto.lastWaterZ && std::hypot(pos.x - g_auto.doneCenter.x, pos.y - g_auto.doneCenter.y) < kAutoRescan) {
					return false;  // the last grid still covers the player
				}
				g_auto.running = true;
				g_auto.cell = cell;
				g_auto.space = SpaceOf(cell);
				g_auto.waterZ = wz;
				const float tex = 2.0f * kAutoHalf / kAutoSize;
				g_auto.center = { std::round(pos.x / tex) * tex, std::round(pos.y / tex) * tex, wz };  // whole texels: a move shifts the grid exactly
				g_auto.bounds = { g_auto.center.x - kAutoHalf, g_auto.center.y - kAutoHalf, g_auto.center.x + kAutoHalf, g_auto.center.y + kAutoHalf };
				if (!g_auto.lastStates.empty() && g_auto.lastSpace == SpaceOf(cell) && g_auto.lastWaterZ == wz) {
					const int dx = static_cast<int>(std::lround((g_auto.bounds.minX - g_auto.lastBounds.minX) / tex));
					const int dy = static_cast<int>(std::lround((g_auto.bounds.minY - g_auto.lastBounds.minY) / tex));
					g_auto.states = math::ShiftGrid(g_auto.lastStates, dx, dy);
					if (g_auto.litSpace == g_auto.space && g_auto.litBounds == g_auto.lastBounds) {
						for (auto& t : g_auto.tracks) {
							t.st = math::ShiftGrid(t.st, dx, dy);  // the shadows found so far move with the grid
							t.next = 0;
							t.litCount = -1;  // new water may be in its light: not 'reaches no water' until cast again
							t.dirty = t.ref != 0;  // ... and published again once the new water is cast
						}
						g_auto.litBounds = g_auto.bounds;
					}
				} else {
					g_auto.states.assign(static_cast<std::size_t>(kAutoSize) * kAutoSize, -1);
					for (auto& t : g_auto.tracks) {
						t = {};  // another place: what each light reached there says nothing here
					}
					g_auto.alignedMask.reset();
					g_auto.onMask = {};
				}
				for (int i = 0; i < kAutoSize; ++i) {  // border ring: dry, never cast
					g_auto.states[static_cast<std::size_t>(i)] = g_auto.states[static_cast<std::size_t>((kAutoSize - 1) * kAutoSize + i)] = 0;
					g_auto.states[static_cast<std::size_t>(i * kAutoSize)] = g_auto.states[static_cast<std::size_t>(i * kAutoSize + kAutoSize - 1)] = 0;
				}
				g_auto.previewed = false;
				g_auto.inferred = 0;
				g_auto.next = 0;
				g_auto.rays = 0;
				g_auto.hitSurface = g_auto.hitBottom = g_auto.hitDry = g_auto.tooDeep = g_auto.noHit = 0;
				g_auto.landAbove = g_auto.otherLevel = g_auto.notLoaded = 0;
				g_auto.rayMs = 0.0;
				g_auto.started = std::chrono::steady_clock::now();
				return false;
			}

			if (g_auto.space != SpaceOf(cell)) {
				g_auto.running = false;  // left the cell mid-scan: start over next step
				return false;
			}
			const float tex = 2.0f * kAutoHalf / kAutoSize;
			const int   total = kAutoSize * kAutoSize;
			const auto   t0 = std::chrono::steady_clock::now();
			const double budget = g_auto.fresh ? kAutoFirstBudgetMs : kAutoStepBudgetMs;
			auto         elapsedMs = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
			g_auto.fresh = false;
			const auto& order = math::ScanOrder();
			int         cast = 0;
			for (; g_auto.next < total; ++g_auto.next) {
				const std::size_t t = order[static_cast<std::size_t>(g_auto.next)];
				if (g_auto.states[t] >= 0) {
					continue;  // border, or reused from the previous grid
				}
				const int         ix = static_cast<int>(t) % kAutoSize, iy = static_cast<int>(t) / kAutoSize;
				const std::int8_t v = math::InferTexel(g_auto.states, ix, iy);
				if (v >= 0) {
					g_auto.states[t] = v;
					++g_auto.inferred;
					continue;
				}
				if ((cast++ & 7) == 0 && elapsedMs() > budget) {
					break;  // out of time for this frame; this texel is cast next frame
				}
				const RE::NiPoint3 from{ g_auto.bounds.minX + (ix + 0.5f) * tex, g_auto.bounds.minY + (iy + 0.5f) * tex,
					g_auto.waterZ + kAutoRayStart };
				const bool outdoors = g_auto.cell->IsExteriorCell();
				if (outdoors) {
					// outdoors a ray from above the water can start inside a hill (the ground is one-sided and lets it
					// through): ask the game first. Terrain above the water, another water level (another cell's lake),
					// or a cell not loaded: dry, no ray.
					auto*              tes = RE::TES::GetSingleton();
					RE::TESObjectCELL* at = tes ? tes->GetCell(from) : nullptr;
					float              wzHere = 0.0f, land = 0.0f;
					std::int8_t        known = -1;
					if (!at || !at->IsAttached()) {
						++g_auto.notLoaded;
						known = 0;
					} else if (!WaterHeightAt(at, from, wzHere) || std::fabs(wzHere - g_auto.waterZ) > 3.0f) {
						++g_auto.otherLevel;
						known = 0;
					} else if (tes->GetLandHeight(from, land) && land > g_auto.waterZ + 3.0f) {
						++g_auto.landAbove;
						known = 0;
					}
					if (known >= 0) {
						g_auto.states[t] = known;
						continue;
					}
				}
				const auto hit = CastRay(g_auto.cell, from, { 0.0f, 0.0f, -1.0f }, outdoors ? kAutoOutRayLength : kAutoRayLength);
				// wet: the ray reaches the water plane (or its surface) and the floor below is a pool bottom. A ray that
				// started inside rock and fell through to a room or void far below is not water.
				const bool wet = hit.ok ? (hit.point.z <= g_auto.waterZ + 3.0f && (outdoors || hit.point.z >= g_auto.waterZ - kAutoMaxDepth)) :
				                          outdoors;  // no floor: open water outside, void inside
				if (!hit.ok) {
					++g_auto.noHit;
				} else if (hit.point.z > g_auto.waterZ + 3.0f) {
					++g_auto.hitDry;
				} else if (hit.point.z >= g_auto.waterZ - 3.0f) {
					++g_auto.hitSurface;
				} else if (wet) {
					++g_auto.hitBottom;  // outdoors at any depth
				} else {
					++g_auto.tooDeep;
				}
				g_auto.states[t] = wet ? 1 : 0;
				++g_auto.rays;
			}
			g_auto.rayMs += elapsedMs();
			if (g_auto.next < total) {
				// the coarse pass is done and no zone shows here yet: put up the preview now
				const int zi = AutoZoneIndex();
				const bool shown = zi >= 0 && g_zones[static_cast<std::size_t>(zi)].cell && g_auto.zoneSpace == g_auto.space;
				if (!g_auto.previewed && g_auto.next >= math::kScanCoarseCount && !shown) {
					g_auto.previewed = true;
					if (auto preview = math::MaskFromGrid(g_auto.bounds, kAutoSize, math::GridFromStates(g_auto.states))) {
						return PublishAutoZone(std::move(preview), "preview");
					}
				}
				return false;
			}

			g_auto.running = false;
			g_auto.haveDone = true;
			g_auto.doneSpace = g_auto.space;
			g_auto.doneCenter = g_auto.center;
			g_auto.lastStates = g_auto.states;
			g_auto.lastBounds = g_auto.bounds;
			g_auto.lastSpace = g_auto.space;
			g_auto.lastWaterZ = g_auto.waterZ;
			auto mask = math::MaskFromGrid(g_auto.bounds, kAutoSize, math::GridFromStates(g_auto.states));
			g_auto.states = {};
			g_auto.doneHadWater = mask != nullptr;
			if (!mask) {
				const bool changed = DisableAutoZone("no water near the player");
				logger::info("automatic zone: no water within {:.0f} of ({:.0f}, {:.0f}) | {}", kAutoHalf, g_auto.center.x, g_auto.center.y, ScanTiming());
				return changed;
			}
			return PublishAutoZone(std::move(mask), "grid");
		}

		std::vector<std::uint8_t> LitOf(const std::vector<std::int8_t>& st)
		{
			std::vector<std::uint8_t> lit(st.size(), 0);
			for (std::size_t i = 0; i < st.size(); ++i) {
				lit[i] = st[i] > 0 ? 1 : 0;  // not cast yet counts as shadow: no light where it is not known to reach
			}
			return lit;
		}

		struct PlaneTarget
		{
			LightKey     ref{ 0 };
			bool         sun{ false };
			RE::NiPoint3 at;
		};

		// what each plane should hold now: 0 the key (a placed lamp or the sun; while a moving light is the key, the
		// nearest placed lamp, so its plane is ready when it takes the key back), 1 and 2 the extras with planes
		std::array<PlaneTarget, 3> WantedPlanes(const Zone& z)
		{
			std::array<PlaneTarget, 3> want{};
			if (z.keySun && z.keyValid) {
				want[0] = { kSunLitRef, true, z.keyTarget };
			} else if (z.keyValid && z.keyIsStatic && z.keyPlaneKey != 0) {
				want[0] = { z.keyPlaneKey, false, z.keyTarget };  // the chosen stationary key (a placed lamp or a Light Placer light)
			} else if (z.staticKeyValid) {
				want[0] = { z.staticKeyRef, false, z.staticKeyPos };
			}
			for (std::size_t e = 0; e < 2; ++e) {
				if (z.extra[e].ref != 0 && z.extra[e].level > 0.0f) {
					want[e + 1] = { z.extra[e].ref, z.extra[e].sun, { z.extra[e].pos[0], z.extra[e].pos[1], z.extra[e].pos[2] } };
				}
			}
			return want;
		}

		AutoScan::LitTrack* FindTrack(LightKey ref)
		{
			for (auto& t : g_auto.tracks) {
				if (ref != 0 && t.ref == ref) {
					return &t;
				}
			}
			return nullptr;
		}

		// The mask with each wanted light's finished plane in its slot (moved there without a ray when the light
		// changed slots); a light still being cast gets none (it then reflects nothing: planePending).
		std::shared_ptr<const math::WaterMask> AlignedPlanes(Zone& z, std::shared_ptr<const math::WaterMask> mask)
		{
			if (!mask || !z.cell || !(g_auto.litSpace == SpaceOf(z.cell)) || !(g_auto.litBounds == mask->bounds)) {
				return mask;
			}
			const auto                                         want = WantedPlanes(z);
			std::array<const AutoScan::LitTrack*, 3>           src{};
			std::array<std::pair<LightKey, std::uint32_t>, 3> now{};
			for (std::size_t k = 0; k < 3; ++k) {
				const auto* t = FindTrack(want[k].ref);
				if (t && t->published && !mask->mips.empty() && t->st.size() == mask->mips[0].size()) {
					src[k] = t;
					now[k] = { t->ref, t->version };
				}
			}
			if (mask == g_auto.alignedMask && now == g_auto.onMask) {
				return mask;  // already so
			}
			std::array<std::vector<std::uint8_t>, 3>        lits;
			std::array<const std::vector<std::uint8_t>*, 3> ptr{};
			for (std::size_t k = 0; k < 3; ++k) {
				if (src[k]) {
					lits[k] = LitOf(src[k]->st);
					ptr[k] = &lits[k];
				}
			}
			mask = math::WithLitPlanes(*mask, ptr);
			z.litKeyRef = src[0] ? src[0]->ref : 0;
			z.litSunDir = src[0] ? src[0]->castAt : RE::NiPoint3{};
			for (std::size_t e = 0; e < 2; ++e) {
				z.litExtraRef[e] = src[e + 1] ? src[e + 1]->ref : 0;
				z.litExtraDir[e] = src[e + 1] ? src[e + 1]->castAt : RE::NiPoint3{};
			}
			if (Settings::Get().debug && now != g_auto.onMask) {
				auto name = [](LightKey r) { return r == 0 ? std::string("-") : r == kSunLitRef ? std::string("sun/moon") : std::format("{:08X}", r); };
				logger::info("[{}] lit planes now {} / {} / {} (moved without rays)", z.cfg.name, name(now[0].first), name(now[1].first), name(now[2].first));
			}
			g_auto.alignedMask = mask;
			g_auto.onMask = now;
			return mask;
		}

		// The mask with every lit plane known for its grid, and the zone told which lights they belong to.
		std::shared_ptr<const math::WaterMask> WithTrackedLight(std::shared_ptr<const math::WaterMask> mask, Zone& z)
		{
			z.litKeyRef = 0;  // a new mask: no planes on it yet
			z.litExtraRef = {};
			mask = AlignedPlanes(z, std::move(mask));
			for (auto& t : g_auto.tracks) {
				t.next = 0;  // new water may need casting
			}
			return mask;
		}

		bool PlaneRecasting(LightKey ref)
		{
			const auto* t = FindTrack(ref);
			return t && t->recast;
		}

		bool PlaneWobbly(LightKey ref)
		{
			const auto* t = FindTrack(ref);
			// ... and moved within the last 5 shadow steps (about a second): once it rests it is cast once and waits for that
			return t && !t->published && t->restarts >= 2 && g_auto.shadowTick - t->movedTick <= 5;
		}

		bool PlaneDark(const Zone& z, const LightCand& c)
		{
			if (!z.automatic || c.kind == LightCand::Kind::kDynamic || c.ref == 0) {
				return false;
			}
			auto* t = FindTrack(c.ref);
			if (!t || !t->published || t->litCount != 0) {
				return false;
			}
			const bool sun = c.kind == LightCand::Kind::kSun;
			const bool moved = sun ? DegreesBetween(t->at, c.at) >= SunRecastDegrees(z.sunIntensity) : t->at.GetDistance(c.at) > 20.0f;
			if (moved) {
				t->litCount = -1;  // from where it is now it may reach water: a candidate again (recast once it has a slot)
				return false;
			}
			t->lastWanted = g_auto.shadowTick;  // still a candidate: kept, not the first reused
			if (!t->darkLogged) {
				t->darkLogged = true;
				logger::info("[{}] {} reaches no water: its light counts on the walls only (its reflection slot is freed)", z.cfg.name,
					sun ? std::string(g_sunDown ? "the moon" : "the sun") : std::format("{:08X}", c.ref));
			}
			return true;
		}

		// Puts the automatic zone up (or updates it in place) with this mask.
		bool PublishAutoZone(std::shared_ptr<const math::WaterMask> mask, const char* what)
		{
			const int total = kAutoSize * kAutoSize;
			ZoneConfig cfg;
			const char* ed = g_auto.cell->GetFormEditorID();
			cfg.name = std::format("auto_{}", (ed && *ed) ? std::string(ed) : std::format("{:08X}", g_auto.cell->GetFormID()));
			cfg.cell = FormKey(g_auto.cell, g_auto.cell->IsInteriorCell());
			if (g_auto.cell->IsExteriorCell()) {
				cfg.worldspace = FormKey(g_auto.cell->GetRuntimeData().worldSpace, false);
			}
			cfg.waterZ = g_auto.waterZ;
			// distances and the light search use where the water is; the GPU gets the whole mask (in.rect)
			cfg.rect = { mask->waterBounds.minX, mask->waterBounds.minY, mask->waterBounds.maxX, mask->waterBounds.maxY };

			const int idx = AutoZoneIndex();
			if (idx >= 0 && g_zones[static_cast<std::size_t>(idx)].cell && g_auto.zoneSpace == g_auto.space) {
				auto&      cur = g_zones[static_cast<std::size_t>(idx)];
				cur.cell = g_auto.cell;
				const bool moved = cur.cfg.rect != cfg.rect;
				cur.cfg.rect = cfg.rect;
				cur.cfg.waterZ = cfg.waterZ;
				cur.rect = { cfg.rect[0], cfg.rect[1], cfg.rect[2], cfg.rect[3] };
				cur.center = { 0.5f * (cur.rect.minX + cur.rect.maxX), 0.5f * (cur.rect.minY + cur.rect.maxY), cfg.waterZ };
				mask = WithTrackedLight(std::move(mask), cur);  // the shadows found so far, moved with the grid
				cur.mask = mask;
				if (cur.active && moved) {
					ScanInteriorLights(cur);  // the water moved with the player: re-pick the lights around it
				}
				logger::info("automatic zone '{}' {} updated: {} of {} grid texels are water, centre ({:.0f}, {:.0f}) | {}", cur.cfg.name, what,
					mask->waterTexels, total, g_auto.center.x, g_auto.center.y, ScanTiming());
				return true;
			}
			Zone z = MakeZone(cfg, idx >= 0 ? static_cast<std::size_t>(idx) : g_zones.size());
			z.cell = g_auto.cell;
			g_auto.zoneSpace = g_auto.space;
			z.automatic = true;
			z.mask = WithTrackedLight(mask, z);
			mask = z.mask;
			if (idx >= 0) {
				g_zones[static_cast<std::size_t>(idx)] = std::move(z);
			} else {
				g_zones.push_back(std::move(z));
			}
			logger::info("automatic zone '{}' up from the {}: {} of {} grid texels are water, waterZ {:.1f}, centre ({:.0f}, {:.0f}) | {}", cfg.name, what,
				mask->waterTexels, total, cfg.waterZ, g_auto.center.x, g_auto.center.y, ScanTiming());
			return true;
		}

		// Which water each reflected light reaches: once the grid is done and the zone is active, one ray per water
		// texel from just above the water to the light (stopping 30 short of a lamp, so its own fixture does not
		// count), or toward the sun / moon. Lights 0 (the key: a placed lamp or the sun) and 1, 2 (the extras,
		// placed lamps) each get a lit plane; the pass then reflects each light only off water it reaches, so a lamp
		// in the next room does not reflect through the wall. The states are kept per light and moved with the
		// grid, so after a move or a zone that dropped out only new water is cast. Same frame budget as the grid.
		void AutoShadowStep()
		{
			g_auto.shadowRunning = false;
			const int idx = AutoZoneIndex();
			if (idx < 0) {
				return;
			}
			auto& z = g_zones[static_cast<std::size_t>(idx)];
			if (!z.cell || !z.active || !z.mask || !z.cell->IsAttached()) {
				return;
			}
			const auto  keep = z.mask;  // z.mask is replaced as planes finish: read the grid from this one
			const auto& m = *keep;
			const int   total = static_cast<int>(m.mips[0].size());
			if (!(g_auto.litSpace == SpaceOf(z.cell)) || !(g_auto.litBounds == m.bounds)) {
				for (auto& t : g_auto.tracks) {
					t = {};  // another grid: nothing known
				}
				g_auto.litSpace = SpaceOf(z.cell);
				g_auto.litBounds = m.bounds;
			}

			// each wanted light's track from the pool (found by the light, else a free or the longest parked one)
			const auto                         want = WantedPlanes(z);
			const std::uint32_t                tick = ++g_auto.shadowTick;
			std::array<AutoScan::LitTrack*, 3> slot{};
			for (std::size_t k = 0; k < 3; ++k) {
				if (want[k].ref == 0) {
					continue;
				}
				auto* t = FindTrack(want[k].ref);
				if (!t) {
					for (auto& c : g_auto.tracks) {
						const bool wanted = std::ranges::any_of(want, [&](const PlaneTarget& w) { return w.ref != 0 && w.ref == c.ref; });
						if (!wanted && (!t || c.ref == 0 || (t->ref != 0 && c.lastWanted < t->lastWanted))) {
							t = &c;
							if (c.ref == 0) {
								break;
							}
						}
					}
					if (!t) {
						continue;
					}
					if (t->ref != 0) {
						logger::info("[{}] lit plane pool full: {:08X} forgotten (cast again if it comes back)", z.cfg.name, t->ref);
					}
					*t = {};
					t->ref = want[k].ref;
					t->at = want[k].at;
					t->st.assign(static_cast<std::size_t>(total), -1);
				}
				if (t->lastWanted != tick) {  // a light in two slots (the nearest lamp as a spare key and an extra): checked once
					t->lastWanted = tick;
					// the moon so faint it lights nothing (the engine swings it round at dusk) is not recast every few degrees
					if (want[k].sun && DegreesBetween(t->at, want[k].at) >= SunRecastDegrees(z.sunIntensity)) {
						t->at = want[k].at;  // cast again toward the new direction; the old shadows stand in until done
						t->recast = true;
						t->next = 0;
					} else if (!want[k].sun && t->at.GetDistance(want[k].at) > 20.0f) {
						t->at = want[k].at;  // a 'stationary' light that moved (a dropped lamp): cast again from where it is
						t->recast = true;
						t->next = 0;
						t->restarts += t->published ? 0 : 1;  // still being cast after the last move
						t->movedTick = tick;
						t->published = false;  // its old shadows no longer fit: not used until the recast is done
					}
				}
				slot[k] = t;
			}

			const float tx = (m.bounds.maxX - m.bounds.minX) / m.size, ty = (m.bounds.maxY - m.bounds.minY) / m.size;
			const auto  t0 = std::chrono::steady_clock::now();
			auto        elapsedMs = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
			int         cast = 0;
			bool        outOfTime = false;
			for (std::size_t k = 0; k < 3 && !outOfTime; ++k) {
				if (!slot[k] || (k > 0 && slot[k] == slot[k - 1]) || (k > 1 && slot[k] == slot[0])) {
					continue;  // none, or already cast for an earlier slot
				}
				auto& t = *slot[k];
				if (t.ref == 0 || t.st.size() != static_cast<std::size_t>(total)) {
					continue;
				}
				if (PlaneWobbly(t.ref)) {
					continue;  // reflected unshadowed while it keeps moving: no rays spent on shadows it would outrun
				}
				const double k0 = elapsedMs();
				for (; t.next < total; ++t.next) {
					const auto i = static_cast<std::size_t>(t.next);
					if (m.mips[0][i] < 128 || (t.st[i] >= 0 && !t.recast)) {
						continue;  // dry, or known
					}
					if ((cast++ & 7) == 0 && elapsedMs() > kAutoStepBudgetMs) {
						outOfTime = true;
						break;
					}
					const RE::NiPoint3 from{ m.bounds.minX + (t.next % m.size + 0.5f) * tx, m.bounds.minY + (t.next / m.size + 0.5f) * ty, z.cfg.waterZ + 2.0f };
					t.dirty = true;
					if (t.ref == kSunLitRef) {  // parallel rays toward the sun / moon
						RE::NiPoint3 sd = t.at;
						const float  sl = sd.Length();
						if (sl < 1e-6f || sd.z <= 0.0f) {
							t.st[i] = 0;
							continue;
						}
						sd *= 1.0f / sl;
						t.st[i] = CastRay(z.cell, from, sd, kSunShadowLength).ok ? 0 : 1;
						++t.rays;
						continue;
					}
					RE::NiPoint3 dir = t.at - from;
					const float  len = dir.Length();
					if (len < 40.0f) {
						t.st[i] = 1;
						continue;
					}
					dir *= 1.0f / len;
					t.st[i] = CastRay(z.cell, from, dir, len - 30.0f).ok ? 0 : 1;
					++t.rays;
				}
				t.ms += elapsedMs() - k0;
				if (t.next < total) {
					g_auto.shadowRunning = true;
					continue;
				}
				if (!t.dirty) {
					continue;  // nothing new since this plane was put on the mask
				}
				t.dirty = false;
				t.published = true;
				t.recast = false;
				t.version = ++g_auto.planeSerial;  // never reused, even by a track taken over for another light
				t.castAt = t.at;
				t.darkLogged = false;
				t.restarts = 0;
				t.litCount = 0;
				for (int i = 0; i < total; ++i) {
					t.litCount += (t.st[static_cast<std::size_t>(i)] > 0 && m.mips[0][static_cast<std::size_t>(i)] >= 128) ? 1 : 0;
				}
				const char* role = k == 0 ? "key light" : (k == 1 ? "extra light 1" : "extra light 2");
				logger::info("[{}] shadow pass for the {} ({}): {} of {} water texels reach it ({} rays, {:.1f} ms of ray time)", z.cfg.name, role,
					t.ref == kSunLitRef ? std::format("the {} toward ({:.2f}, {:.2f}, {:.2f})", g_sunDown ? "moon" : "sun", t.at.x, t.at.y, t.at.z) :
										  std::format("{:08X}", t.ref),
					t.litCount, m.waterTexels, t.rays, t.ms);
				t.rays = 0;
				t.ms = 0.0;
			}
			if (outOfTime) {
				g_auto.shadowRunning = true;
			}
			z.mask = AlignedPlanes(z, z.mask);  // the finished planes, each in its light's slot
		}

		// ---- console control: five GLOB records in CausticLight.esp -------------------------
		//   set CL_Enabled to 0/1    draw pass off/on
		//   set CL_DebugTint to 0/1  flat debug tint off/on (overrides zones.json while the game runs)
		//   set CL_DebugTint to 2    light mask: where the reflected light lands (no Fresnel, no pattern)
		//   set CL_DebugTint to 3    direct light: how much the reflected lamps light each face themselves
		//   set CL_Strength to N     caustic strength, x the physical reflection (0..50, ESP default 6.4, chosen to show; 2.14 fits Cycles)
		//   set CL_Corner to 1       authoring: a corner at the crosshair (twice = a new water rect)
		//   set CL_Remove to 1       authoring: remove the zone under the crosshair
		// Polled from the task at kGlobalHz; CL_Corner / CL_Remove are written back to 0 once done.
		// Globals are stored in the save, so CL_Enabled / CL_DebugTint / CL_Strength keep their value per save.

		constexpr float kGlobalHz = 5.0f;

		struct ConsoleGlobal
		{
			RE::FormID     localID;
			const char*    name;
			RE::TESGlobal* form{ nullptr };
			float          last{ 0.0f };
			bool           seen{ false };  // read once since the last game load
		};
		std::array<ConsoleGlobal, 5> g_globals{ { { 0x802, "CL_Enabled" }, { 0x803, "CL_DebugTint" }, { 0x804, "CL_Corner" },
			{ 0x805, "CL_Remove" }, { 0x806, "CL_Strength" } } };
		enum
		{
			kGlobEnabled,
			kGlobDebugTint,
			kGlobCorner,
			kGlobRemove,
			kGlobStrength
		};

		void ResolveGlobals(const char* a_when)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			for (auto& g : g_globals) {
				if (g.form) {
					continue;
				}
				const auto id = dh->LookupFormID(g.localID, kPluginName);
				auto*      form = id ? RE::TESForm::LookupByID(id) : nullptr;
				g.form = form ? form->As<RE::TESGlobal>() : nullptr;
				if (g.form) {
					logger::info("[{}] console global {} found ({:08X}, value {})", a_when, g.name, id, g.form->value);
				} else {
					logger::warn("[{}] console global {} (0x{:X} in {}) NOT found{} - that console control is unavailable", a_when, g.name,
						g.localID, kPluginName, form ? " (form is not a GLOB)" : "");
				}
			}
		}

		void PollGlobals()
		{
			for (std::size_t i = 0; i < g_globals.size(); ++i) {
				auto& g = g_globals[i];
				if (!g.form) {
					continue;
				}
				float v = g.form->value;
				if (!std::isfinite(v)) {
					v = 0.0f;
				}

				if (!g.seen) {
					g.seen = true;
					if (i == kGlobDebugTint && Settings::Get().debugTint && v == 0.0f) {
						g.form->value = v = 1.0f;  // zones.json asks for the tint: start the console value from it
						logger::info("console: CL_DebugTint set to 1 (zones.json debugTint true)");
					} else if (i == kGlobStrength && Settings::Get().iniStrength >= 0.0f) {
						// CausticLight.ini sets the strength: it wins over the save's value at every load (the console still
						// changes it for this session)
						g.form->value = v = Settings::Get().iniStrength;
						logger::info("console: CL_Strength = {:.2f} from CausticLight.ini fStrength", v);
					} else if (i == kGlobStrength && std::ranges::any_of(math::kOldDefaultStrengths, [&](float o) { return std::fabs(v - o) < 1e-3f; })) {
						// globals live in the save: one made before the refit still holds the old default. Rewritten once per
						// load, so the console shows the value in use and a later 'set CL_Strength to 2.85' is taken as asked
						const float was = v;
						g.form->value = v = math::kDefaultStrength;
						logger::info("console: CL_Strength {:.2f} from the save is an old default - set to {} (the physical match is {}; the default is chosen to show)",
							was, math::kDefaultStrength, math::kFittedStrength);
					} else {
						logger::info("console: {} = {} (from the save / ESP default)", g.name, v);
					}
				} else if (v != g.last) {
					logger::info("console: {} {} -> {}", g.name, g.last, v);
				}
				g.last = v;

				switch (i) {
				case kGlobEnabled:
					if (DrawPass::Enabled() != (v != 0.0f)) {
						DrawPass::SetEnabled(v != 0.0f);
						logger::info("draw pass {} (CL_Enabled)", v != 0.0f ? "ON" : "OFF");
					}
					break;
				case kGlobDebugTint:
					{
						const bool direct = v >= 2.5f;          // 3: direct light
						const bool light = v >= 1.5f && !direct;  // 2: light mask
						const bool tint = v != 0.0f && !light && !direct;
						if (g_debugTint != tint || g_debugLight != light || g_debugDirect != direct) {
							g_debugTint = tint;
							g_debugLight = light;
							g_debugDirect = direct;
							logger::info("debug view {} (CL_DebugTint, tint strength {:.2f})",
								direct ? "DIRECT LIGHT" : light ? "LIGHT MASK" : tint ? "FLAT TINT" : "OFF", Settings::Get().debugTintStrength);
						}
					}
					break;
				case kGlobStrength:
					{
						const float st = std::clamp(v, 0.0f, math::kMaxStrength);
						if (st != g_strength) {
							g_strength = st;
							logger::info("caustic strength x{:.2f} (CL_Strength{})", st, st != v ? ", clamped to 0..50" : "");
						}
					}
					break;
				case kGlobCorner:
				case kGlobRemove:
					if (v != 0.0f && !MenuBlocksInput()) {  // a menu is open: leave it set, run when it closes
						if (i == kGlobCorner) {
							CornerAction();
						} else {
							RemoveAction();
						}
						g.form->value = 0.0f;
						g.last = 0.0f;
					}
					break;
				default:
					break;
				}
			}
		}
	}

	bool Initialize()
	{
		if (!RE::TESDataHandler::GetSingleton()) {
			return false;
		}
		LogBaseForms("data loaded");
		ResolveGlobals("data loaded");
		g_debugTint = Settings::Get().debugTint;

		g_zones.clear();
		for (const auto& cfg : Settings::Get().zones) {
			g_zones.push_back(MakeZone(cfg, g_zones.size()));
		}
		g_actorScratch.reserve(256);

		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESCellFullyLoadedEvent>(CellLoadedSink::Get());
		}
		g_ready = true;
		return true;
	}

	void Reset()
	{
		g_generation.fetch_add(1);
		g_worldReady.store(false);

		for (auto& z : g_zones) {
			z.active = false;
			z.slot = -1;
			z.keyValid = false;
			z.keySnap = true;
			z.impulse = 0.0f;
			z.ripple = {};
			z.rippleTargetAmp = 0.0f;
		}
		g_slotZone.fill(-1);
		g_activeCount = 0;
		g_activeExterior = 0;
		g_extValid = false;
		g_haveLastFrame = false;
		g_placeAccum = 1e9f;
		g_weatherAccum = 1e9f;
		g_pendingCorner.reset();
		g_auto = {};
		DisableAutoZone("game loading");
		for (auto& g : g_globals) {
			g.seen = false;  // the next save has its own values
		}
		g_globalsAccum = 1e9f;
		DrawPass::ClearStaging();  // nothing is drawn during the load
		g_taskPending.store(false);
	}

	void OnGameReady()
	{
		LogBaseForms("game ready");
		ResolveGlobals("game ready");
		g_worldReady.store(true);
		g_forcePlacement.store(true);
	}

	namespace
	{
		struct DueWork
		{
			std::uint32_t generation{ 0 };
			bool          place{ false };
			bool          weather{ false };
			bool          actors{ false };
			bool          output{ false };
			bool          logNow{ false };
			bool          globals{ false };
			bool          autoScan{ false };  // an automatic-zone grid is being scanned: one step this frame
			float         outDt{ 0.0f };
		};

		bool LoadingScreenUp()
		{
			auto* ui = RE::UI::GetSingleton();
			return !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		void RunDueWork(const DueWork& w)
		{
			if (w.generation != g_generation.load()) {
				return;  // queued before a load: Reset() already cleared g_taskPending, a newer task may be pending
			}
			if (!g_worldReady.load()) {
				g_taskPending.store(false);
				return;
			}
			DrawPass::EnsureCreated();
			DrawPass::ReportRenderInfo();

			if (LoadingScreenUp()) {
				g_slotZone.fill(-1);        // the last cell's zones are not drawn over the next one (doors send no load event)
				DrawPass::ClearStaging();
				g_forcePlacement.store(true);
				g_auto.fresh = true;  // the first scan step after this loading screen may take the longer budget
				g_taskPending.store(false);
				return;
			}
			DrawPass::EnsureTextures();  // not during the loading screen's own texture streaming

			const std::size_t zonesBefore = g_zones.size();
			bool              autoChanged = false;
			if (w.globals) {
				PollGlobals();  // may add or remove a zone (and re-poll the active set)
			}
			if (w.globals || w.autoScan) {
				autoChanged = AutoZoneStep();
				if (!g_auto.running) {
					AutoShadowStep();
				}
			}
			const bool zonesChanged = g_zones.size() != zonesBefore || autoChanged;
			if (w.place || autoChanged) {
				PollActive();
			}
			if (g_activeCount > 0) {
				if (w.weather && RefreshEnb()) {
					for (const int zi : g_slotZone) {  // the placed lamps' strengths were taken at the old ENB multipliers
						if (zi >= 0 && g_zones[static_cast<std::size_t>(zi)].cell) {
							ScanInteriorLights(g_zones[static_cast<std::size_t>(zi)]);
						}
					}
				}
				if (g_activeExterior > 0 && (w.weather || !g_extValid)) {
					ReadExterior();
				}
				if (w.actors) {
					ScanActors();
					UpdateKeyLights();
				}
			}
			// Restage whenever slots may have changed, so the hook's slot -> zone map and the staged
			// records always agree (also stages zeros when nothing is active).
			if (w.output || w.place || zonesChanged) {
				OutputTick(w.output ? w.outDt : 0.0f, w.logNow);
			}
			g_taskPending.store(false);
		}
	}

	// Runs inside the render routine: time bookkeeping, per-slot atlas clocks, and the GPU
	// upload + bind. Everything else is queued as one task.
	void OnFrame()
	{
		DrawPass::BeginFrame();
		if (!g_ready) {
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		float      dt = g_haveLastFrame ? std::chrono::duration<float>(now - g_lastFrame).count() : 0.0f;
		g_lastFrame = now;
		g_haveLastFrame = true;
		dt = std::clamp(dt, 0.0f, 0.25f);

		auto*      ui = RE::UI::GetSingleton();
		const bool paused = !ui || ui->GameIsPaused();
		const bool live = g_worldReady.load();

		if (live && !paused) {
			for (std::size_t k = 0; k < math::kMaxDrawZones; ++k) {
				const int zi = g_slotZone[k];
				if (zi < 0 || static_cast<std::size_t>(zi) >= g_zones.size()) {
					continue;
				}
				auto& z = g_zones[static_cast<std::size_t>(zi)];
				z.frameClock = math::AdvanceFrameClock(z.frameClock, dt, z.speed);
				DrawPass::SetFrameClock(k, z.frameClock);
			}
		}

		if (!live || paused || g_taskPending.load()) {
			return;
		}

		g_placeAccum += dt;
		g_globalsAccum += dt;
		if (g_activeCount > 0) {
			g_outAccum += dt;
			g_actorAccum += dt;
			g_weatherAccum += dt;
			g_debugAccum += dt;
		}

		const auto& s = Settings::Get();
		DueWork     w;
		w.place = g_placeAccum >= 1.0f / s.placementHz || g_forcePlacement.load();
		if (g_activeCount > 0) {
			w.weather = g_weatherAccum >= 1.0f / s.weatherHz;
			w.actors = g_actorAccum >= 1.0f / s.actorHz;
			w.output = g_outAccum >= 1.0f / s.tickHz;
		}
		w.globals = g_globalsAccum >= 1.0f / kGlobalHz;
		w.autoScan = g_auto.running || g_auto.shadowRunning;
		if (!w.place && !w.weather && !w.actors && !w.output && !w.globals && !w.autoScan) {
			return;
		}

		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;  // nothing consumed
		}

		w.generation = g_generation.load();
		if (w.globals) {
			g_globalsAccum = 0.0f;
		}
		if (w.place) {
			g_placeAccum = 0.0f;
			g_forcePlacement.store(false);
		}
		if (w.weather) {
			g_weatherAccum = 0.0f;
			// the player edited CausticLight.ini with the game running: its values take effect now
			if (Settings::Get().ReloadIniIfChanged()) {
				auto& g = g_globals[kGlobStrength];
				if (g.form && Settings::Get().iniStrength >= 0.0f) {
					g.form->value = Settings::Get().iniStrength;  // PollGlobals applies and logs it
				}
			}
		}
		if (w.actors) {
			g_actorAccum = 0.0f;
		}
		if (w.output) {
			w.outDt = std::min(g_outAccum, 0.5f);
			g_outAccum = 0.0f;
			w.logNow = s.debug && g_debugAccum >= 1.0f;
			if (w.logNow) {
				g_debugAccum = 0.0f;
			}
		}

		g_taskPending.store(true);
		tasks->AddTask([w]() { RunDueWork(w); });
	}
}
