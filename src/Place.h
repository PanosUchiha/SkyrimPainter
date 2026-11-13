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

// Placing things with a preview that follows your aim: the easel (on the floor) and finished
// paintings (on walls). Also keeps track of the paintings hanging in this save.
namespace Place
{
	enum class Mode
	{
		None,
		Easel,
		Painting
	};

	bool Init();  // kDataLoaded: forms
	void OnGameLoaded();

	void BeginEasel();  // from the Easel item
	void BeginPainting(int a_slot, RE::ObjectRefHandle a_moving = {}, float a_scale = 1.0f);
	bool IsPlacing();

	struct Status
	{
		Mode        mode = Mode::None;
		std::string name;      // painting name
		float       scale = 1.0f;
		float       distance = 0.0f;
		int         turnDeg = 0;  // easel
	};
	bool GetStatus(Status& a_out);  // any thread

	void Tick();               // every frame
	void Wheel(int a_dir, bool a_shift);  // easel: turn; painting: size
	void Nudge(int a_steps);   // painting: distance from the wall (Up / Down)
	void RequestPlace();
	void Cancel(bool a_notify = true);

	// ---- paintings on walls
	struct MenuInfo
	{
		RE::ObjectRefHandle ref;
		int                 slot = -1;
		float               scale = 1.0f;
	};
	bool                    IsWallPainting(const RE::TESForm* a_base);
	void                    OpenMenu(RE::TESObjectREFR* a_ref);  // the player activated a hung painting
	std::optional<MenuInfo> TakeActivated();
	void                    TakeDown(RE::ObjectRefHandle a_ref);
	void                    SetScale(RE::ObjectRefHandle a_ref, float a_scale);
	void                    Move(RE::ObjectRefHandle a_ref);
	void                    RefreshSlot(int a_slot);  // the slot's texture changed: hung copies show the new one
	bool                    SlotHung(int a_slot);

	// co-save
	void Save(SKSE::SerializationInterface* a_intfc);
	bool Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version);  // false: not ours
	void Revert();
}
