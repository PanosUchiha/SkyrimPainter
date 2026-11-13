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

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

// Paint-like colour mixing: a C++ port of spectral.js by Ronald van Wijnen (MIT License,
// https://github.com/rvanwijnen/spectral.js). Each colour becomes a reflectance curve over 38
// wavelengths, curves are mixed with Kubelka-Munk theory, so blue + yellow makes green.
// Standalone (no game headers), so it can be tested outside the game.
namespace Spectral
{
	using Rgb = std::array<std::uint8_t, 3>;  // sRGB, 0..255

	// Mixes colours by weight (like spectral.js mix([color, factor], ...)), gamut-mapped like its toString().
	Rgb Mix(const std::vector<std::pair<Rgb, double>>& a_colors);
	// a_t = share of b (0..1), like spectral.js mix([a, 1 - t], [b, t]).
	Rgb Mix(const Rgb& a_a, const Rgb& a_b, double a_t);
}
