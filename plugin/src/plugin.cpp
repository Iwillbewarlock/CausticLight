#include "PCH.h"

#include "DrawPass.h"
#include "Hooks.h"
#include "Settings.h"
#include "Zones.h"

namespace
{
	void InitLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= "CausticLight.log";
		// keep the last session's log (a crash is only looked at after the game has been started again)
		std::error_code ec;
		if (std::filesystem::exists(*path, ec)) {
			auto prev = *path;
			prev.replace_filename("CausticLight-previous.log");
			std::filesystem::copy_file(*path, prev, std::filesystem::copy_options::overwrite_existing, ec);
		}

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void OnDataLoaded()
	{
		cl::Settings::Get().Load();
		cl::Settings::Get().LoadIni();

		if (!cl::Zones::Initialize()) {
			logger::error("CausticLight disabled (see errors above)");
			return;
		}
		cl::Hooks::Install();
		cl::DrawPass::WatchTidesEarly();
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 0, 0 },
	.Name = "CausticLight"sv,
	.Author = "CausticLight"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogging();
	SKSE::Init(a_skse);
	SKSE::AllocTrampoline(1 << 7);  // two 5-byte call hooks

	const auto* decl = SKSE::PluginDeclaration::GetSingleton();
	logger::info("{} v{} loading", decl->GetName(), decl->GetVersion().string("."sv));

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging) {
		logger::error("No messaging interface");
		return false;
	}

	messaging->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			cl::Zones::Reset();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			cl::Zones::OnGameReady();
			break;
		case SKSE::MessagingInterface::kNewGame:
			cl::Zones::Reset();
			cl::Zones::OnGameReady();
			break;
		default:
			break;
		}
	});

	return true;
}
