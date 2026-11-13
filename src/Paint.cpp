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

#include "Paint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Paint
{
	const BrushType kBrushes[kBrushCount] = {
		{ "Detail round", 3.5f, 1.0f, 0.75f, 0.6f },
		{ "Round", 10.0f, 1.0f, 0.65f, 1.0f },
		{ "Filbert", 19.0f, 1.6f, 0.60f, 1.1f },
		{ "Wide flat", 34.0f, 3.2f, 0.55f, 1.2f },
	};

	namespace
	{
		constexpr int kUndoDepth = 20;
		constexpr int kTip = 64;  // tip mask resolution

		std::uint32_t Hash(std::uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7feb352dU;
			x ^= x >> 15;
			x *= 0x846ca68bU;
			x ^= x >> 16;
			return x;
		}
		float Rand(std::uint32_t a_seed) { return (Hash(a_seed) & 0xFFFFFF) / float(0xFFFFFF); }

		float Smooth(float e0, float e1, float x)
		{
			const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		// Bristle tips: a soft-edged ellipse made of bristles. Across the stroke (u) the bristles leave
		// gaps and differ in strength, which is what draws the streaks; along it (v) they are continuous.
		struct Tip
		{
			std::array<float, kTip * kTip> mask{};
		};

		const Tip& TipFor(int a_type)
		{
			static std::array<Tip, kBrushCount> tips;
			static bool                         made = false;
			if (!made) {
				made = true;
				for (int b = 0; b < kBrushCount; ++b) {
					auto&     m = tips[b].mask;
					const int bristles = 10 + b * 14;
					std::vector<float> across(kTip, 0.0f);
					for (int k = 0; k < bristles; ++k) {
						const float c = Rand(b * 977 + k * 31 + 1) * 2.0f - 1.0f;   // position across the tip
						const float w = 0.6f / bristles + Rand(b * 977 + k * 31 + 2) * 1.2f / bristles;
						const float s = 0.55f + 0.45f * Rand(b * 977 + k * 31 + 3);
						for (int i = 0; i < kTip; ++i) {
							const float u = (i + 0.5f) / kTip * 2.0f - 1.0f;
							const float d = std::fabs(u - c) / (w * 2.0f);
							if (d < 1.0f) {
								across[i] = std::max(across[i], s * (1.0f - d * d));
							}
						}
					}
					for (int y = 0; y < kTip; ++y) {
						for (int x = 0; x < kTip; ++x) {
							const float u = (x + 0.5f) / kTip * 2.0f - 1.0f, v = (y + 0.5f) / kTip * 2.0f - 1.0f;
							const float r = std::sqrt(u * u + v * v);
							const float edge = 1.0f - Smooth(0.7f, 1.0f, r);
							// a little grain along the bristles too
							const float grain = 0.85f + 0.15f * Rand(b * 7919 + y * 131 + x);
							m[y * kTip + x] = std::clamp(edge * (0.5f + 0.5f * across[x]) * grain, 0.0f, 1.0f);
						}
					}
				}
			}
			return tips[std::clamp(a_type, 0, kBrushCount - 1)];
		}

		float SampleTip(const Tip& a_tip, float a_u, float a_v)  // u, v in -1..1
		{
			const float fx = (a_u + 1.0f) * 0.5f * kTip - 0.5f, fy = (a_v + 1.0f) * 0.5f * kTip - 0.5f;
			const int   x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
			const float tx = fx - x0, ty = fy - y0;
			auto        at = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= kTip || y >= kTip) {
                    return 0.0f;
                }
                return a_tip.mask[y * kTip + x];
			};
			const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
			return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
		}
	}

	void Rect::Add(int a_x0, int a_y0, int a_x1, int a_y1)
	{
		if (Empty()) {
			*this = { a_x0, a_y0, a_x1, a_y1 };
			return;
		}
		x0 = std::min(x0, a_x0);
		y0 = std::min(y0, a_y0);
		x1 = std::max(x1, a_x1);
		y1 = std::max(y1, a_y1);
	}

	void Painting::Reset(int a_w, int a_h, const std::vector<std::uint8_t>& a_blank)
	{
		m_w = a_w;
		m_h = a_h;
		m_blank = a_blank;
		m_layers.clear();
		m_layers.push_back({ "Background", true, 1.0f, std::vector<std::uint8_t>(static_cast<std::size_t>(a_w) * a_h * 4, 0) });
		m_current = 0;
		m_undo.clear();
		m_redo.clear();
		m_changed = false;
		m_stroking = false;
		RecompositeAll();
	}

	void Painting::SetCurrent(int a_i) { m_current = std::clamp(a_i, 0, static_cast<int>(m_layers.size()) - 1); }

	bool Painting::AddLayer(const std::string& a_name)
	{
		if (static_cast<int>(m_layers.size()) >= kMaxLayers) {
			return false;
		}
		m_layers.insert(m_layers.begin() + m_current + 1, Layer{ a_name, true, 1.0f, std::vector<std::uint8_t>(static_cast<std::size_t>(m_w) * m_h * 4, 0) });
		++m_current;
		m_changed = true;
		return true;
	}

	void Painting::DeleteLayer(int a_i)
	{
		if (m_layers.size() <= 1 || a_i < 0 || a_i >= static_cast<int>(m_layers.size())) {
			return;
		}
		m_layers.erase(m_layers.begin() + a_i);
		m_current = std::clamp(m_current - (a_i <= m_current ? 1 : 0), 0, static_cast<int>(m_layers.size()) - 1);
		m_undo.clear();  // undo steps point at layers by position
		m_redo.clear();
		m_changed = true;
		RecompositeAll();
	}

	void Painting::MoveLayer(int a_i, int a_delta)
	{
		const int j = a_i + a_delta;
		if (a_i < 0 || j < 0 || a_i >= static_cast<int>(m_layers.size()) || j >= static_cast<int>(m_layers.size())) {
			return;
		}
		std::swap(m_layers[a_i], m_layers[j]);
		if (m_current == a_i) {
			m_current = j;
		} else if (m_current == j) {
			m_current = a_i;
		}
		m_undo.clear();
		m_redo.clear();
		m_changed = true;
		RecompositeAll();
	}

	void Painting::SetLayerVisible(int a_i, bool a_on)
	{
		if (a_i >= 0 && a_i < static_cast<int>(m_layers.size()) && m_layers[a_i].visible != a_on) {
			m_layers[a_i].visible = a_on;
			RecompositeAll();
		}
	}

	void Painting::SetLayerOpacity(int a_i, float a_opacity)
	{
		if (a_i >= 0 && a_i < static_cast<int>(m_layers.size())) {
			m_layers[a_i].opacity = std::clamp(a_opacity, 0.0f, 1.0f);
			m_changed = true;
			RecompositeAll();
		}
	}

	void Painting::ClearLayer(int a_i)
	{
		if (a_i < 0 || a_i >= static_cast<int>(m_layers.size())) {
			return;
		}
		PushUndo(a_i);
		std::fill(m_layers[a_i].px.begin(), m_layers[a_i].px.end(), 0);
		m_changed = true;
		RecompositeAll();
	}

	void Painting::PushUndo(int a_layer)
	{
		m_undo.push_back({ a_layer, m_layers[a_layer].px });
		while (static_cast<int>(m_undo.size()) > kUndoDepth) {
			m_undo.pop_front();
		}
		m_redo.clear();
	}

	void Painting::Undo()
	{
		if (m_undo.empty()) {
			return;
		}
		auto step = std::move(m_undo.back());
		m_undo.pop_back();
		if (step.layer >= static_cast<int>(m_layers.size())) {
			return;
		}
		m_redo.push_back({ step.layer, std::move(m_layers[step.layer].px) });
		m_layers[step.layer].px = std::move(step.px);
		m_changed = true;
		RecompositeAll();
	}

	void Painting::Redo()
	{
		if (m_redo.empty()) {
			return;
		}
		auto step = std::move(m_redo.back());
		m_redo.pop_back();
		if (step.layer >= static_cast<int>(m_layers.size())) {
			return;
		}
		m_undo.push_back({ step.layer, std::move(m_layers[step.layer].px) });
		m_layers[step.layer].px = std::move(step.px);
		m_changed = true;
		RecompositeAll();
	}

	Rgb Painting::ColorAt(int a_x, int a_y) const
	{
		a_x = std::clamp(a_x, 0, m_w - 1);
		a_y = std::clamp(a_y, 0, m_h - 1);
		const auto* p = &m_composite[(static_cast<std::size_t>(a_y) * m_w + a_x) * 4];
		return { p[0], p[1], p[2] };
	}

	void Painting::RecompositeAll()
	{
		m_composite.resize(static_cast<std::size_t>(m_w) * m_h * 4);
		Recomposite({ 0, 0, m_w, m_h });
	}

	void Painting::Recomposite(const Rect& a_r)
	{
		const int x0 = std::max(0, a_r.x0), y0 = std::max(0, a_r.y0), x1 = std::min(m_w, a_r.x1), y1 = std::min(m_h, a_r.y1);
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				const std::size_t i = (static_cast<std::size_t>(y) * m_w + x) * 4;
				float             r = m_blank[i], g = m_blank[i + 1], b = m_blank[i + 2];
				for (const auto& l : m_layers) {  // back to front
					if (!l.visible) {
						continue;
					}
					const float a = l.px[i + 3] / 255.0f * l.opacity;
					if (a <= 0.0f) {
						continue;
					}
					r += (l.px[i] - r) * a;
					g += (l.px[i + 1] - g) * a;
					b += (l.px[i + 2] - b) * a;
				}
				m_composite[i] = static_cast<std::uint8_t>(r + 0.5f);
				m_composite[i + 1] = static_cast<std::uint8_t>(g + 0.5f);
				m_composite[i + 2] = static_cast<std::uint8_t>(b + 0.5f);
				m_composite[i + 3] = 255;
			}
		}
		m_dirty.Add(x0, y0, x1, y1);
	}

	Rect Painting::TakeDirty()
	{
		const Rect r = m_dirty;
		m_dirty = {};
		return r;
	}

	void Painting::BeginStroke(Brush& a_brush, float a_x, float a_y, float a_pressure)
	{
		if (m_layers.empty() || !a_brush.Loaded()) {
			return;
		}
		PushUndo(m_current);
		m_stroking = true;
		m_lastX = a_x;
		m_lastY = a_y;
		m_carry = 0.0f;
		m_dabs = 0;
		m_moved = false;
		m_firstPressure = a_pressure;
	}

	void Painting::StrokeTo(Brush& a_brush, float a_x, float a_y, float a_pressure)
	{
		if (!m_stroking) {
			return;
		}
		const float dx = a_x - m_lastX, dy = a_y - m_lastY;
		const float dist = std::sqrt(dx * dx + dy * dy);
		if (dist < 0.01f) {
			return;
		}
		// the tip turns with the stroke (smoothed, so a shaky hand doesn't spin it)
		const float nx = dx / dist, ny = dy / dist;
		if (!m_moved) {  // first movement: the tip faces this way from the start, and the stroke begins here
			m_moved = true;
			m_dirX = nx;
			m_dirY = ny;
			Dab(a_brush, m_lastX, m_lastY, m_firstPressure, m_dirX, m_dirY);
		}
		m_dirX = m_dirX * 0.7f + nx * 0.3f;
		m_dirY = m_dirY * 0.7f + ny * 0.3f;
		const float dl = std::sqrt(m_dirX * m_dirX + m_dirY * m_dirY);
		if (dl > 1e-4f) {
			m_dirX /= dl;
			m_dirY /= dl;
		}
		const auto& bt = kBrushes[std::clamp(a_brush.type, 0, kBrushCount - 1)];
		const float radius = bt.radius * (0.45f + 0.55f * a_pressure);
		const float step = std::max(0.75f, radius * 0.18f);
		float       t = step - m_carry;
		while (t <= dist) {
			Dab(a_brush, m_lastX + nx * t, m_lastY + ny * t, a_pressure, m_dirX, m_dirY);
			t += step;
		}
		m_carry = dist - (t - step);
		m_lastX = a_x;
		m_lastY = a_y;
	}

	void Painting::EndStroke(Brush& a_brush)
	{
		if (m_stroking && !m_moved) {
			Dab(a_brush, m_lastX, m_lastY, m_firstPressure, m_dirX, m_dirY);  // a click without moving: one dab
		}
		m_stroking = false;
	}

	void Painting::Dab(Brush& a_brush, float a_x, float a_y, float a_pressure, float a_dirX, float a_dirY)
	{
		if (!a_brush.Loaded()) {
			return;
		}
		const auto& bt = kBrushes[std::clamp(a_brush.type, 0, kBrushCount - 1)];
		const float p = std::clamp(a_pressure, 0.05f, 1.0f);
		const float rAcross = bt.radius * (0.45f + 0.55f * p);    // pressing harder splays the bristles
		const float rAlong = rAcross / bt.aspect;
		const float reach = std::max(rAcross, rAlong) + 1.0f;
		const int   x0 = std::max(0, static_cast<int>(std::floor(a_x - reach)));
		const int   y0 = std::max(0, static_cast<int>(std::floor(a_y - reach)));
		const int   x1 = std::min(m_w - 1, static_cast<int>(std::ceil(a_x + reach)));
		const int   y1 = std::min(m_h - 1, static_cast<int>(std::ceil(a_y + reach)));
		if (x0 > x1 || y0 > y1) {
			return;
		}
		auto& layer = m_layers[m_current].px;

		// Wet paint under the brush: what was on this layer before the stroke began (the undo snapshot).
		// Lower layers count as dry - you paint back to front, letting each layer set.
		const auto& before = m_undo.empty() ? layer : m_undo.back().px;
		float       under[3]{}, wet = 0.0f, wsum = 0.0f;
		for (int k = 0; k < 5; ++k) {
			const int sx = std::clamp(static_cast<int>(a_x + (k == 1 ? rAcross * 0.5f : k == 2 ? -rAcross * 0.5f : 0.0f)), 0, m_w - 1);
			const int sy = std::clamp(static_cast<int>(a_y + (k == 3 ? rAcross * 0.5f : k == 4 ? -rAcross * 0.5f : 0.0f)), 0, m_h - 1);
			const std::size_t i = (static_cast<std::size_t>(sy) * m_w + sx) * 4;
			const float       a = before[i + 3] / 255.0f;
			for (int c = 0; c < 3; ++c) {
				under[c] += before[i + c] * a;
			}
			wsum += a;
			wet += std::sqrt(a);  // even a thin film of wet paint mixes in
		}
		wet /= 5.0f;
		Rgb deposit = a_brush.color;
		if (wet > 0.15f && wsum > 0.0f) {
			const Rgb u{ static_cast<std::uint8_t>(under[0] / wsum), static_cast<std::uint8_t>(under[1] / wsum), static_cast<std::uint8_t>(under[2] / wsum) };
			if ((m_dabs++ & 3) == 0) {
				// the brush picks up some of the wet paint it drags through
				a_brush.color = Spectral::Mix(a_brush.color, u, 0.12f * wet);
			}
			deposit = Spectral::Mix(a_brush.color, u, 0.6f * wet * (1.0f - 0.3f * p));
		}

		// How much paint the brush still has: a dry brush only marks with its strongest bristles.
		const float load = a_brush.load;
		const float thr = (1.0f - std::min(1.0f, load / 0.6f)) * 0.75f;
		const float flow = bt.flow * (0.25f + 0.75f * p) * (0.35f + 0.65f * Smooth(0.0f, 0.5f, load));
		const Tip&  tip = TipFor(a_brush.type);
		const float ca = a_dirX, sa = a_dirY;  // stroke direction; u runs across it
		const float dr = deposit[0], dg = deposit[1], db = deposit[2];
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				const float px = x + 0.5f - a_x, py = y + 0.5f - a_y;
				const float along = px * ca + py * sa;
				const float across = -px * sa + py * ca;
				const float u = across / rAcross, v = along / rAlong;
				if (u * u + v * v >= 1.0f) {
					continue;
				}
				float m = SampleTip(tip, u, v);
				if (thr > 0.0f) {
					m = Smooth(thr, thr + 0.25f, m) * m;
				}
				const float a = std::clamp(m * flow, 0.0f, 1.0f);
				if (a <= 0.002f) {
					continue;
				}
				const std::size_t i = (static_cast<std::size_t>(y) * m_w + x) * 4;
				const float       da = layer[i + 3] / 255.0f;
				const float       oa = a + da * (1.0f - a);
				const float       k = a / oa;  // share of the new paint in the pixel's colour
				layer[i] = static_cast<std::uint8_t>(layer[i] + (dr - layer[i]) * k + 0.5f);
				layer[i + 1] = static_cast<std::uint8_t>(layer[i + 1] + (dg - layer[i + 1]) * k + 0.5f);
				layer[i + 2] = static_cast<std::uint8_t>(layer[i + 2] + (db - layer[i + 2]) * k + 0.5f);
				layer[i + 3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
			}
		}
		// paint used up: more with a bigger brush and a heavier hand
		a_brush.load = std::max(0.0f, a_brush.load - 0.0002f * bt.thirst * rAcross * (0.4f + 0.6f * p));
		m_changed = true;
		Recomposite({ x0, y0, x1 + 1, y1 + 1 });
	}

	void Painting::Rotate(bool a_clockwise, const std::vector<std::uint8_t>& a_newBlank)
	{
		for (auto& l : m_layers) {
			l.px = Rotate90(l.px, m_w, m_h, a_clockwise);
		}
		std::swap(m_w, m_h);
		m_blank = a_newBlank;
		m_undo.clear();
		m_redo.clear();
		m_changed = true;
		RecompositeAll();
	}

	std::vector<std::uint8_t> Rotate90(const std::vector<std::uint8_t>& a_px, int a_w, int a_h, bool a_clockwise)
	{
		std::vector<std::uint8_t> out(a_px.size());
		for (int y = 0; y < a_h; ++y) {
			for (int x = 0; x < a_w; ++x) {
				// clockwise: (x, y) -> (h - 1 - y, x) in a picture a_h wide
				const int nx = a_clockwise ? a_h - 1 - y : y;
				const int ny = a_clockwise ? x : a_w - 1 - x;
				std::memcpy(&out[(static_cast<std::size_t>(ny) * a_h + nx) * 4], &a_px[(static_cast<std::size_t>(y) * a_w + x) * 4], 4);
			}
		}
		return out;
	}
}
