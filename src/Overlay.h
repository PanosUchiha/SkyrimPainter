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

#pragma once

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

// The ImGui overlay drawn inside the game's Present (adapted from Hearthframe): the studio, the
// painting menu, the painting picker and the hanging HUD, plus the game input that drives them.
namespace Overlay
{
	struct DebugSwitches
	{
		bool hookPresent = true;  // [Debug] bHookPresent
		bool inputSink = true;    // [Debug] bInputSink
		bool blockInput = true;   // [Debug] bBlockInput: the game and other mods get no keys while a window is open
	};

	void                 LoadSettings();
	const DebugSwitches& GetDebugSwitches();
	std::string          IniPath();

	bool InstallInputSink();     // kDataLoaded
	void PrepareInputBlocker();  // plugin load
	void InstallInputBlocker();  // first kPostLoadGame / kNewGame

	// Called from inside the game's IDXGISwapChain::Present, on the render thread.
	void OnPresent(IDXGISwapChain* a_swapChain);

	// render thread helpers for the screens
	ID3D11Device*             Device();
	ID3D11DeviceContext*      Context();
	ID3D11ShaderResourceView* MakeSRV(const std::uint8_t* a_rgba, int a_w, int a_h);

	// __except filter: logs the exception and the faulting module+offset, then handles it.
	int SehFilter(EXCEPTION_POINTERS* a_info, const char* a_where);
}
