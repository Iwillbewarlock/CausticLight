#include "PCH.h"

#include "Settings.h"

#include "CausticMath.h"

#include <charconv>
#include <fstream>
#include <nlohmann/json.hpp>

namespace cl
{
	namespace
	{
		constexpr auto kZonesPath = "Data/SKSE/Plugins/CausticLight/zones.json"sv;
		constexpr auto kIniPath = "Data/SKSE/Plugins/CausticLight.ini"sv;
		std::filesystem::file_time_type g_iniTime{};

		using json = nlohmann::json;

		// "rect": [minX, minY, maxX, maxY]; nullopt when missing or malformed.
		std::optional<std::array<float, 4>> ReadRect(const json& j)
		{
			if (!j.contains("rect")) {
				return std::nullopt;
			}
			const auto& v = j.at("rect");
			if (!v.is_array() || v.size() != 4) {
				return std::nullopt;
			}
			for (const auto& e : v) {
				if (!e.is_number()) {
					return std::nullopt;
				}
			}
			return std::array<float, 4>{ v[0].get<float>(), v[1].get<float>(), v[2].get<float>(), v[3].get<float>() };
		}

		template <class T>
		T ReadOr(const json& j, const char* key, T fallback)
		{
			if (!j.contains(key) || j.at(key).is_null()) {
				return fallback;
			}
			try {
				return j.at(key).get<T>();
			} catch (const std::exception&) {
				logger::warn("zones.json: '{}' has the wrong type - using the default", key);
				return fallback;  // one mistyped value does not throw away the whole file
			}
		}

	}

	bool Settings::LoadIni()
	{
		std::ifstream in{ std::filesystem::path(kIniPath) };
		if (!in) {
			logger::info("{} not found - using zones.json and the defaults", kIniPath);
			return false;
		}
		std::error_code ec;
		g_iniTime = std::filesystem::last_write_time(std::filesystem::path(kIniPath), ec);
		// key = value lines; [sections] only group them for the reader; ';' or '#' start a comment
		std::string line, bad;
		while (std::getline(in, line)) {
			if (const auto c = line.find_first_of(";#"); c != std::string::npos) {
				line.erase(c);
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			auto trim = [](std::string t) {
				const auto a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
				return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
			};
			std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
			std::ranges::transform(key, key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			float f = 0.0f;
			const bool num = !val.empty() && std::from_chars(val.data(), val.data() + val.size(), f).ec == std::errc{} && std::isfinite(f);
			if (!num) {
				bad += (bad.empty() ? "" : ", ") + key;
				continue;
			}
			if (key == "fstrength") {
				iniStrength = std::clamp(f, 0.0f, math::kMaxStrength);
			} else if (key == "fripplecontrast") {
				rippleContrast = std::clamp(f, 0.0f, 3.0f);
			} else if (key == "ffloorstrength") {
				floorStrength = std::clamp(f, 0.0f, 5.0f);
			} else if (key == "bfollowshadowdistance") {
				maxDistanceFromShadows = f != 0.0f;
			} else if (key == "fmaxdistance") {
				maxDistance = std::max(f, 0.0f);
			} else if (key == "busesurfacetides") {
				useSurfaceTides = f != 0.0f;
			} else {
				bad += (bad.empty() ? "" : ", ") + key;
			}
		}
		logger::info("{}: strength {}, ripple contrast {:.2f}, floor {:.2f}, distance {}, Surface Tides {}{}", kIniPath,
			iniStrength >= 0.0f ? std::format("{:.2f}", iniStrength) : std::string("(save / ESP)"), rippleContrast, floorStrength,
			maxDistanceFromShadows ? std::string("= shadow distance") : std::format("{:.0f}", maxDistance), useSurfaceTides ? "on" : "off",
			bad.empty() ? "" : " (not understood: " + bad + ")");
		return true;
	}

	bool Settings::ReloadIniIfChanged()
	{
		std::error_code ec;
		const auto      t = std::filesystem::last_write_time(std::filesystem::path(kIniPath), ec);
		if (ec || t == g_iniTime) {
			return false;
		}
		return LoadIni();
	}

	Settings& Settings::Get()
	{
		static Settings s;
		return s;
	}

	bool Settings::Load()
	{
		zones.clear();

		std::ifstream in{ std::filesystem::path(kZonesPath) };
		if (!in) {
			logger::warn("{} not found - no caustic zones will be placed", kZonesPath);
			return false;
		}

		try {
			const json root = json::parse(in, nullptr, true, true);  // allow comments

			debug = ReadOr(root, "debug", debug);
			debugTint = ReadOr(root, "debugTint", debugTint);
			debugTintStrength = std::clamp(ReadOr(root, "debugTintStrength", debugTintStrength), 0.01f, math::kMaxDebugTint);
			tickHz = std::clamp(ReadOr(root, "tickHz", tickHz), 2.0f, 30.0f);
			weatherHz = std::clamp(ReadOr(root, "weatherHz", weatherHz), 0.1f, 10.0f);
			actorHz = std::clamp(ReadOr(root, "actorHz", actorHz), 0.5f, 20.0f);
			placementHz = std::clamp(ReadOr(root, "placementHz", placementHz), 0.2f, 10.0f);
			maxDistance = std::max(ReadOr(root, "maxDistance", maxDistance), 0.0f);
			maxDistanceFromShadows = ReadOr(root, "maxDistanceFromShadows", maxDistanceFromShadows);
			actorRange = ReadOr(root, "actorRange", actorRange);
			lightSearchRange = ReadOr(root, "lightSearchRange", lightSearchRange);
			impulseDecaySeconds = std::max(ReadOr(root, "impulseDecaySeconds", impulseDecaySeconds), 0.1f);
			autoZones = ReadOr(root, "autoZones", autoZones);
			autoZonesOutdoors = ReadOr(root, "autoZonesOutdoors", autoZonesOutdoors);
			renderer = ReadOr(root, "renderer", renderer);
			std::ranges::transform(renderer, renderer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (renderer != "auto" && renderer != "cs" && renderer != "enb" && renderer != "vanilla") {
				logger::warn("zones.json: renderer '{}' is not auto / cs / enb / vanilla - using auto", renderer);
				renderer = "auto";
			}
			tidesWatchUnderEnb = ReadOr(root, "tidesWatchUnderEnb", tidesWatchUnderEnb);

			const json empty = json::array();
			const json& list = root.contains("zones") && root.at("zones").is_array() ? root.at("zones") : empty;
			if (list.empty()) {
				logger::info("zones.json has no zones yet");
			}

			for (const auto& z : list) {
				ZoneConfig c;
				c.name = ReadOr<std::string>(z, "name", std::format("zone{}", zones.size()));
				c.cell = ReadOr<std::string>(z, "cell", "");
				c.worldspace = ReadOr<std::string>(z, "worldspace", "");
				const auto rect = ReadRect(z);
				if (!rect) {
					logger::warn("zone '{}' has no valid 'rect' [minX, minY, maxX, maxY] (old wall-panel entry?) - skipped", c.name);
					continue;
				}
				c.rect = { std::min((*rect)[0], (*rect)[2]), std::min((*rect)[1], (*rect)[3]), std::max((*rect)[0], (*rect)[2]),
					std::max((*rect)[1], (*rect)[3]) };
				c.waterZ = ReadOr(z, "waterZ", c.waterZ);
				c.height = std::max(ReadOr(z, "height", c.height), 1.0f);
				c.baseAgitation = std::clamp(ReadOr(z, "baseAgitation", c.baseAgitation), 0.0f, 1.0f);
				c.brightness = std::max(ReadOr(z, "brightness", c.brightness), 0.0f);

				if (c.cell.empty()) {
					logger::warn("zone '{}' has no 'cell' - skipped", c.name);
					continue;
				}
				zones.push_back(std::move(c));
			}
		} catch (const std::exception& e) {
			logger::error("zones.json parse error: {}", e.what());
			zones.clear();
			return false;
		}

		logger::info("zones.json: {} zone(s); outputs {} Hz, weather {} Hz, actors {} Hz, placement {} Hz; "
					 "maxDistance {}; debug {}; debugTint {} (strength {:.2f}); control: console globals CL_Enabled / CL_DebugTint / CL_Strength / "
					 "CL_Corner / CL_Remove",
			zones.size(), tickHz, weatherHz, actorHz, placementHz, maxDistance, debug, debugTint, debugTintStrength);
		return true;
	}

	namespace
	{
		json ZoneToJson(const ZoneConfig& c)
		{
			json z = json::object();
			z["name"] = c.name;
			z["cell"] = c.cell;
			if (!c.worldspace.empty()) {
				z["worldspace"] = c.worldspace;
			}
			z["waterZ"] = c.waterZ;
			z["rect"] = { c.rect[0], c.rect[1], c.rect[2], c.rect[3] };
			z["height"] = c.height;
			z["baseAgitation"] = c.baseAgitation;
			z["brightness"] = c.brightness;
			return z;
		}

		// Read the current file (or start empty), let `edit` change it, write it back via a
		// temporary file so a failed write never leaves a half-written zones.json.
		template <class F>
		bool EditFile(F&& edit)
		{
			const std::filesystem::path path(kZonesPath);
			auto                        tmp = path;
			tmp += ".tmp";

			// On any failure: old zones.json untouched, .tmp removed, error logged.
			auto fail = [&](std::string_view why) {
				std::error_code rmEc;
				std::filesystem::remove(tmp, rmEc);
				logger::error("zones.json NOT updated ({}); the previous file is unchanged", why);
				return false;
			};

			json root = json::object();
			try {
				if (std::ifstream in{ path }; in) {
					root = json::parse(in, nullptr, true, true);  // a parse error throws: never overwrite a file we could not read
				}
				if (!root.is_object()) {
					return fail("existing file is not a JSON object");
				}
				if (!root.contains("zones") || !root["zones"].is_array()) {
					root["zones"] = json::array();
				}
				edit(root["zones"]);

				std::error_code ec;
				std::filesystem::create_directories(path.parent_path(), ec);

				std::ofstream out{ tmp, std::ios::out | std::ios::trunc };
				if (!out) {
					return fail(std::format("cannot open {}", tmp.string()));
				}
				out << root.dump(4) << '\n';
				out.flush();
				if (!out.good()) {
					out.close();
					return fail("write/flush failed");
				}
				out.close();
				if (out.fail()) {
					return fail("close failed");
				}

				std::filesystem::rename(tmp, path, ec);
				if (ec) {
					return fail(std::format("cannot replace {}: {}", path.string(), ec.message()));
				}
			} catch (const std::exception& e) {
				return fail(e.what());
			}
			return true;
		}
	}

	bool Settings::AppendZone(const ZoneConfig& zone)
	{
		const bool ok = EditFile([&](json& list) { list.push_back(ZoneToJson(zone)); });
		if (ok) {
			zones.push_back(zone);
			logger::info("zones.json: appended zone '{}'", zone.name);
		}
		return ok;
	}

	bool Settings::RemoveZone(std::string_view name)
	{
		std::size_t removed = 0;
		const bool  ok = EditFile([&](json& list) {
            for (auto it = list.begin(); it != list.end();) {
                if (it->is_object() && it->contains("name") && (*it)["name"].is_string() &&
                    (*it)["name"].get<std::string>() == name) {
                    it = list.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
		});
		if (ok) {
			std::erase_if(zones, [&](const ZoneConfig& z) { return z.name == name; });
			logger::info("zones.json: removed {} entr{} named '{}'", removed, removed == 1 ? "y" : "ies", name);
		}
		return ok;
	}
}
