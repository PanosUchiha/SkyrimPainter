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

#include "Wall.h"

// The painter's things: crafted items, easels set up in the world, finished paintings in the pack.
//   Drop a Blank Canvas (with an Easel in your pack)  -> the easel stands where you are, canvas on it
//   Activate the easel (with Palette and Paints)       -> the studio opens
//   Drop a Painting                                    -> hang it on a wall
namespace Items
{
	bool Init();          // kDataLoaded: forms, recipe station, events
	void OnGameLoaded();  // re-applies easel canvases

	// What the studio works on.
	struct Easel
	{
		RE::ObjectRefHandle ref;
		std::string         wipPng;  // UTF-8 path of the painting in progress ("" = blank canvas)
	};
	std::optional<Easel> TakeStudioRequest();  // any thread: set when the player activated an easel

	// Paintings waiting in the pack, for the picker (any thread).
	std::vector<Wall::Painting> Unhung();
	bool                        TakePickerRequest();  // a Painting was dropped and there's more than one to choose from

	// ---- studio results (any thread; the game work is queued for the main thread)
	// Steps away: the painting in progress is kept on the easel (shown on its canvas).
	void StepAway(RE::ObjectRefHandle a_easel, std::vector<std::uint8_t> a_rgba);
	// Finished: the painting goes into the pack, the easel folds up into the pack too.
	void Finish(RE::ObjectRefHandle a_easel, std::vector<std::uint8_t> a_rgba, std::string a_name);
	// Packs the easel and the canvas back up (the painting in progress is thrown away).
	void PackUp(RE::ObjectRefHandle a_easel);
	// Picker: hang this painting / put it back.
	void Hang(const std::string& a_id);
	void CancelPick();

	// A painting goes back into the pack (taken down, hanging cancelled). Main thread.
	void ReturnPainting(const Wall::Painting& a_painting);

	// Activate pressed with nothing else to do: an easel or a hung painting in the line of sight? (any thread)
	void RequestActivate();

	// co-save
	void InstallSerialization();
	void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_s);
	bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& a_s);
}
