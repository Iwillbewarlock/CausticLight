#pragma once

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"
#include "SKSE/Logger.h"

#include <d3d11.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace logger = SKSE::log;
