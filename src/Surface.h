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

#include <cmath>

// Where the canvas is on screen: a projective map from canvas pixels to screen pixels (and back). It
// covers both the canvas standing in the world, seen in perspective, and the flat close-up.
struct Surface
{
	float m[9]{}, inv[9]{};
	bool  ok = false;

	void Map(const float a_t[9], float a_x, float a_y, float& a_ox, float& a_oy) const
	{
		const float w = a_t[6] * a_x + a_t[7] * a_y + a_t[8];
		const float iw = std::fabs(w) > 1e-9f ? 1.0f / w : 0.0f;
		a_ox = (a_t[0] * a_x + a_t[1] * a_y + a_t[2]) * iw;
		a_oy = (a_t[3] * a_x + a_t[4] * a_y + a_t[5]) * iw;
	}
	void ToScreen(float a_x, float a_y, float& a_sx, float& a_sy) const { Map(m, a_x, a_y, a_sx, a_sy); }
	void ToCanvas(float a_sx, float a_sy, float& a_x, float& a_y) const { Map(inv, a_sx, a_sy, a_x, a_y); }
	// screen pixels per canvas pixel around a point
	float ScaleAt(float a_x, float a_y) const
	{
		float ax, ay, bx, by, cx, cy;
		ToScreen(a_x, a_y, ax, ay);
		ToScreen(a_x + 1.0f, a_y, bx, by);
		ToScreen(a_x, a_y + 1.0f, cx, cy);
		return 0.5f * (std::hypot(bx - ax, by - ay) + std::hypot(cx - ax, cy - ay));
	}

	bool Finish()
	{
		const float* a = m;
		const float  det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
		if (std::fabs(det) < 1e-12f) {
			return ok = false;
		}
		const float id = 1.0f / det;
		inv[0] = (a[4] * a[8] - a[5] * a[7]) * id;
		inv[1] = (a[2] * a[7] - a[1] * a[8]) * id;
		inv[2] = (a[1] * a[5] - a[2] * a[4]) * id;
		inv[3] = (a[5] * a[6] - a[3] * a[8]) * id;
		inv[4] = (a[0] * a[8] - a[2] * a[6]) * id;
		inv[5] = (a[2] * a[3] - a[0] * a[5]) * id;
		inv[6] = (a[3] * a[7] - a[4] * a[6]) * id;
		inv[7] = (a[1] * a[6] - a[0] * a[7]) * id;
		inv[8] = (a[0] * a[4] - a[1] * a[3]) * id;
		return ok = true;
	}

	// The picture (a_w x a_h) seen as the quad a_q on screen: top-left, top-right, bottom-right, bottom-left.
	static Surface FromQuad(const float a_q[4][2], float a_w, float a_h)
	{
		// unit square -> quad (Heckbert), then canvas pixels -> unit square
		const float x0 = a_q[0][0], y0 = a_q[0][1], x1 = a_q[1][0], y1 = a_q[1][1];
		const float x2 = a_q[2][0], y2 = a_q[2][1], x3 = a_q[3][0], y3 = a_q[3][1];
		const float sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
		float       a, b, c, d, e, f, g = 0.0f, h = 0.0f;
		if (std::fabs(sx) < 1e-6f && std::fabs(sy) < 1e-6f) {
			a = x1 - x0, b = x3 - x0, c = x0;
			d = y1 - y0, e = y3 - y0, f = y0;
		} else {
			const float dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
			const float den = dx1 * dy2 - dx2 * dy1;
			if (std::fabs(den) < 1e-9f) {
				return {};
			}
			g = (sx * dy2 - dx2 * sy) / den;
			h = (dx1 * sy - sx * dy1) / den;
			a = x1 - x0 + g * x1, b = x3 - x0 + h * x3, c = x0;
			d = y1 - y0 + g * y1, e = y3 - y0 + h * y3, f = y0;
		}
		Surface s;
		const float iw = 1.0f / a_w, ih = 1.0f / a_h;
		const float t[9] = { a * iw, b * ih, c, d * iw, e * ih, f, g * iw, h * ih, 1.0f };
		for (int i = 0; i < 9; ++i) {
			s.m[i] = t[i];
		}
		s.Finish();
		return s;
	}

	// The flat close-up: the picture's top-left at (a_ox, a_oy), a_scale screen px per canvas px,
	// optionally seen in a mirror (left and right swapped).
	static Surface FromRect(float a_ox, float a_oy, float a_scale, float a_w, bool a_mirror)
	{
		Surface s;
		const float t[9] = { a_mirror ? -a_scale : a_scale, 0.0f, a_mirror ? a_ox + a_w * a_scale : a_ox, 0.0f, a_scale, a_oy, 0.0f, 0.0f, 1.0f };
		for (int i = 0; i < 9; ++i) {
			s.m[i] = t[i];
		}
		s.Finish();
		return s;
	}
};
