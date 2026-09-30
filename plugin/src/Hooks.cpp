#include "PCH.h"

#include "Hooks.h"

#include "DrawPass.h"
#include "Zones.h"

namespace cl::Hooks
{
	namespace
	{
		// Per-frame update on the main thread: the depth-pass call inside the main render
		// routine (the same site the local DynamicTerrainDeformation plugin uses; write_call
		// chains with whatever target is already there).
		struct FrameUpdate
		{
			static void thunk(bool a1, bool a2)
			{
				Zones::OnFrame();
				func(a1, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// After opaque geometry: a call inside the world render routine (site taken from DTD,
		// which draws its snow particles there). Our pass runs right after the engine's call.
		struct PostOpaque
		{
			static void thunk(RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
			{
				func(a_accumulator, a_flags);
				DrawPass::Render();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool IsCall(std::uintptr_t a_site, const char* a_what)
		{
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(a_site);
			if (opcode != 0xE8) {
				logger::critical("{} call site is not a call (expected E8 at {:X}, found {:02X}). This game version is not supported "
								 "by this build; that hook was NOT written. Nothing was modified.",
					a_what, a_site, opcode);
				return false;
			}
			return true;
		}
	}

	bool Install()
	{
		auto& trampoline = SKSE::GetTrampoline();

		const auto frameSite = REL::RelocationID(35560, 36559).address() + REL::Relocate(0x395, 0x395);
		if (!IsCall(frameSite, "Per-frame")) {
			return false;
		}
		FrameUpdate::func = trampoline.write_call<5>(frameSite, FrameUpdate::thunk);
		logger::info("Per-frame update installed (call site {:X})", frameSite);

		const auto drawSite = REL::RelocationID(99938, 106583).address() + REL::Relocate(0x319, 0x308);
		if (!IsCall(drawSite, "Post-opaque")) {
			DrawPass::NoteHookInstalled(false);
			return true;  // zones keep running, nothing is drawn
		}
		PostOpaque::func = trampoline.write_call<5>(drawSite, PostOpaque::thunk);
		DrawPass::NoteHookInstalled(true);
		logger::info("draw pass installed (post-opaque call site {:X})", drawSite);
		return true;
	}
}
