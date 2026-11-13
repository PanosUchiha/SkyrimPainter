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

// Hanging finished paintings on walls (adapted from Hearthframe's wall frames).
namespace Wall
{
	// A finished painting.
	struct Painting
	{
		std::string id;    // file name without extension (Paintings\<id>.png)
		std::string name;  // what the player called it
		std::string png;   // UTF-8 full path of the painting
		int         style = 0;  // frame
	};

	std::string DdsFor(const std::string& a_id);  // game path of the painting's wall texture

	// kDataLoaded: finds the frame records in SkyrimPainter.esp.
	bool Init();
	void OnGameLoaded();

	// ---- placement (aim at a wall, left click to hang)
	void BeginPlacement(const Painting& a_painting, RE::ObjectRefHandle a_moving = {}, float a_scale = 1.0f);
	bool IsPlacing();
	struct Status
	{
		std::string name;
		std::string frame;
		float       scale = 1.0f;
		float       distance = 0.0f;
	};
	bool GetStatus(Status& a_out);  // any thread
	void Tick();                    // every frame (queues the preview update on the main thread)
	void CycleStyle(int a_delta);
	void AdjustScale(int a_steps);
	void AdjustOffset(int a_steps);
	void RequestPlace();
	void CancelPlacement(bool a_notify = true);

	// ---- hung paintings
	struct MenuInfo
	{
		RE::ObjectRefHandle ref;
		Painting            painting;
		float               scale = 1.0f;
	};
	// Main thread: the painting in the line of sight gets its menu (TakeActivated). False if none.
	bool                    Examine(bool a_respectCrosshair);
	std::optional<MenuInfo> TakeActivated();  // any thread
	void                    TakeDown(RE::ObjectRefHandle a_ref);      // back into the inventory
	void                    SetScale(RE::ObjectRefHandle a_ref, float a_scale);
	void                    ChangeFrame(RE::ObjectRefHandle a_ref, int a_style);
	void                    Move(RE::ObjectRefHandle a_ref);
	int                     HungCount();

	// co-save
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version);
	void Revert();
}
