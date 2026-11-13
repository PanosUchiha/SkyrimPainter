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

#include "Items.h"

// The painting screen: the canvas with its layers on the left, the palette (paint already on it) and
// the brushes on the right. Right-click a brush to pick it up, right-click a paint to load the brush
// (a second paint mixes in), right-click the bare palette to put the mix down, hold right-click on the
// canvas to paint. Render thread only.
namespace Studio
{
	bool Open(const Items::Work& a_work);  // false if the canvas couldn't be set up
	bool Draw();                           // false once the studio has closed
	void Flush();                          // start of each frame: frees textures the last frame used
}
