// SkyrimPainter - paint your own pictures in Skyrim Special Edition
// Copyright (C) 2026 evodube
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program
// (LICENSE.txt). If not, see <https://www.gnu.org/licenses/>.

#include "Art.h"
#include "Items.h"
#include "Overlay.h"
#include "Place.h"

namespace
{
	void SetupLog()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		{
			const auto      dir = *path;
			std::error_code ec;
			std::filesystem::remove(dir / "SkyrimPainter.1.log", ec);
			std::filesystem::rename(dir / "SkyrimPainter.log", dir / "SkyrimPainter.1.log", ec);
		}
		*path /= "SkyrimPainter.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	// ------------------------------------------------------------------ Present hook (from Hearthframe)
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	PresentFn         g_originalPresent = nullptr;
	std::atomic<bool> g_presentDisabled{ false };

	void SafeOnPresent(IDXGISwapChain* a_sc)
	{
		__try {
			Overlay::OnPresent(a_sc);
		} __except (Overlay::SehFilter(GetExceptionInformation(), "OnPresent")) {
			g_presentDisabled = true;
		}
	}

	HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_sc, UINT a_sync, UINT a_flags)
	{
		if (!g_presentDisabled) {
			SafeOnPresent(a_sc);
		}
		return g_originalPresent(a_sc, a_sync, a_flags);
	}

	bool PatchPresentSlot(IDXGISwapChain* a_sc)
	{
		auto** vtable = *reinterpret_cast<void***>(a_sc);
		if (vtable[8] == reinterpret_cast<void*>(&HookedPresent)) {
			return true;
		}
		DWORD old = 0;
		if (!::VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			spdlog::error("VirtualProtect failed ({})", ::GetLastError());
			return false;
		}
		g_originalPresent = reinterpret_cast<PresentFn>(vtable[8]);
		vtable[8] = reinterpret_cast<void*>(&HookedPresent);
		::VirtualProtect(&vtable[8], sizeof(void*), old, &old);
		return true;
	}

	IDXGISwapChain* GetGameSwapChain()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		return renderer ? reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
	}

	bool InstallPresentHook()
	{
		IDXGISwapChain* sc = nullptr;
		__try {
			sc = GetGameSwapChain();
		} __except (Overlay::SehFilter(GetExceptionInformation(), "GetGameSwapChain")) {
			sc = nullptr;
		}
		// No dummy swap chain fallback: an extra swap chain breaks ENB Frame Generation.
		return sc && PatchPresentSlot(sc);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			{
				const auto& dbg = Overlay::GetDebugSwitches();
				if (dbg.hookPresent) {
					spdlog::info(InstallPresentHook() ? "Present hook installed" : "Could not install the Present hook - the studio will not appear");
				}
				if (dbg.inputSink) {
					spdlog::info(Overlay::InstallInputSink() ? "Input sink registered" : "Could not register for input events");
				}
				if (!Items::Init()) {
					spdlog::error("Some item records are missing - is SkyrimPainter.esp enabled?");
				}
				if (!Place::Init()) {
					spdlog::error("The wall painting records are missing - is SkyrimPainter.esp enabled?");
				}
				Art::RegisterEvents();
				spdlog::info("Setup finished");
				break;
			}
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			{
				static bool blockerDone = false;
				if (!blockerDone) {
					blockerDone = true;
					Overlay::InstallInputBlocker();  // after every plugin that hooks input at the main menu
				}
				Place::Cancel(false);
				Items::OnGameLoaded();
				Place::OnGameLoaded();
				break;
			}
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	const auto ver = REL::Module::get().version();
	spdlog::info("SkyrimPainter {} loaded - game {} ({}), SKSE {}", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."sv),
		ver.string("."sv), REL::Module::IsAE() ? "AE" : "SE", REL::Version::unpack(a_skse->SKSEVersion()).string("."sv));
	Overlay::LoadSettings();
	Overlay::PrepareInputBlocker();
	Items::InstallSerialization();
	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		spdlog::error("Could not register with the SKSE messaging interface");
		return false;
	}
	return true;
}
