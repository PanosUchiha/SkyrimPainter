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
#include "Place.h"

// The small screens: the easel's menu, a hung painting's menu and the placing HUD. Render thread.
namespace Menus
{
	void OpenEasel(const Items::EaselMenuInfo& a_info);
	bool DrawEasel();  // false once closed

	void OpenPainting(const Place::MenuInfo& a_info);
	bool DrawPainting();

	void DrawPlaceHud(const Place::Status& a_status);
	void Release();
	void Flush();
}
