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

// Smaller screens: a hung painting's menu, the "which painting?" picker and the hanging HUD. Render thread.
namespace Menus
{
	void OpenPainting(const Wall::MenuInfo& a_info);
	bool DrawPainting();  // false once closed

	void OpenPicker();
	bool DrawPicker();  // false once closed

	void DrawHangHud(const Wall::Status& a_status);

	void Release();  // frees thumbnails (window closed)
	void Flush();    // start of each frame
}
