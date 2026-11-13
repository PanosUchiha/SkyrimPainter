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

// Keeps painted textures on the "SPCanvas" shape of our easels and framed paintings: converts
// pictures to DDS in the background, puts the texture on as soon as a reference's 3D exists, and
// puts it back whenever the game rebuilds the 3D (cell loads, moves).
namespace Art
{
	inline constexpr int kPaintingW = 1024;  // the painting's pixel size (4:3)
	inline constexpr int kPaintingH = 768;

	std::string                ToUtf8(const std::filesystem::path& a_path);
	std::filesystem::path      FromUtf8(const std::string& a_text);
	std::filesystem::path      DataPath(const std::string& a_gamePath);  // "textures\\x.dds" -> <game>\Data\textures\x.dds
	std::filesystem::path      UserFolder();  // Documents\My Games\Skyrim Special Edition\SkyrimPainter
	std::string                NewId();       // unique name for a painting ("20261113_154501_3f2a")

	// Top-left game notification. Any thread.
	void Notify(const std::string& a_msg);

	// Puts a_dds on a_ref's canvas (and keeps it there). Main thread.
	void Show(RE::FormID a_ref, const std::string& a_dds);
	void Forget(RE::FormID a_ref);
	void ForgetAll();

	// Makes sure a_dds exists, converting the PNG a_png at a_w x a_h in the background if it doesn't.
	// References showing it are updated once it's ready. Any thread.
	void EnsureFromPng(const std::string& a_dds, const std::filesystem::path& a_png, int a_w, int a_h);
	// Converts pixels (RGBA8, kPaintingW x kPaintingH) to a_dds in the background, then shows it on a_ref. Any thread.
	void ConvertAndShow(std::vector<std::uint8_t> a_rgba, const std::string& a_dds, int a_w, int a_h, RE::FormID a_ref);
	bool IsReady(const std::string& a_dds);

	// Main thread: applies textures to references whose 3D appeared, checks a few for lost textures.
	void Tick();
	void RegisterEvents();
	// The game's bitmap on a reference's canvas right now (main thread). Used by the hanging preview.
	bool ApplyNow(RE::TESObjectREFR* a_ref, const std::string& a_dds);

	// Deletes a texture file (when nothing uses it any more). Any thread.
	void DeleteTexture(const std::string& a_dds);
}
