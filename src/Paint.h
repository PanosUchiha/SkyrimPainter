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

#include "Spectral.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// The paint engine: layers composited back to front over the canvas, and brushes that behave like
// real ones - a bristle tip that leaves streaks, a light touch that only catches the strongest bristles, pressure, and wet
// paint that mixes (spectrally) with what is already on the canvas. No game or UI code, so it can be
// tested on its own.
namespace Paint
{
	using Rgb = Spectral::Rgb;

	struct BrushType
	{
		const char* name;
		float       radius;     // canvas pixels at full pressure
		float       aspect;     // tip width across the stroke / along it (flat brushes > 1)
		float       flow;       // paint laid down per dab
		float       thirst;     // how fast it runs out of paint
	};
	inline constexpr int       kBrushCount = 4;
	extern const BrushType     kBrushes[kBrushCount];

	struct Brush
	{
		int   type = -1;     // -1 = no brush in hand
		Rgb   color{};       // paint on the brush
		float load = 0.0f;   // 0 = clean, 1 = paint on it (never runs out; the rag wipes it)
		bool  Loaded() const { return type >= 0 && load > 0.5f; }
	};

	struct Layer
	{
		std::string               name;
		bool                      visible = true;
		float                     opacity = 1.0f;
		std::vector<std::uint8_t> px;  // RGBA, straight alpha
	};

	struct Rect
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1)
		bool Empty() const { return x1 <= x0 || y1 <= y0; }
		void Add(int a_x0, int a_y0, int a_x1, int a_y1);
	};

	inline constexpr int kMaxLayers = 8;

	class Painting
	{
	public:
		// a_blank: the bare canvas (RGBA, a_w x a_h). Starts with one empty layer.
		void Reset(int a_w, int a_h, const std::vector<std::uint8_t>& a_blank);
		int  Width() const { return m_w; }
		int  Height() const { return m_h; }

		// ---- layers (painted back to front: layer 0 is at the back)
		std::vector<Layer>&       Layers() { return m_layers; }
		const std::vector<Layer>& Layers() const { return m_layers; }
		int                       Current() const { return m_current; }
		void                      SetCurrent(int a_i);
		bool                      AddLayer(const std::string& a_name);  // above the current one
		void                      DeleteLayer(int a_i);
		void                      MoveLayer(int a_i, int a_delta);
		void                      SetLayerVisible(int a_i, bool a_on);
		void                      SetLayerOpacity(int a_i, float a_opacity);
		void                      ClearLayer(int a_i);

		// ---- painting
		void BeginStroke(Brush& a_brush, float a_x, float a_y, float a_pressure);
		void StrokeTo(Brush& a_brush, float a_x, float a_y, float a_pressure);
		void EndStroke(Brush& a_brush);
		bool Stroking() const { return m_stroking; }
		// the visible colour at a pixel (for the colour picker / the paint under the brush)
		Rgb  ColorAt(int a_x, int a_y) const;

		// ---- undo (strokes and clears; per layer)
		bool CanUndo() const { return !m_undo.empty(); }
		bool CanRedo() const { return !m_redo.empty(); }
		void Undo();
		void Redo();

		// ---- what is seen: the canvas with every visible layer on it
		const std::vector<std::uint8_t>& Composite() const { return m_composite; }
		Rect                             TakeDirty();  // part of Composite() changed since the last call
		void                             RecompositeAll();
		bool                             Changed() const { return m_changed; }
		void                             MarkSaved() { m_changed = false; }

		// Rotates everything a quarter turn (clockwise or not): width and height swap.
		void Rotate(bool a_clockwise, const std::vector<std::uint8_t>& a_newBlank);

	private:
		struct UndoStep
		{
			int                       layer;
			std::vector<std::uint8_t> px;
		};

		void Dab(Brush& a_brush, float a_x, float a_y, float a_pressure, float a_dirX, float a_dirY);
		void Recomposite(const Rect& a_r);
		void PushUndo(int a_layer);

		int                       m_w = 0, m_h = 0;
		std::vector<std::uint8_t> m_blank;
		std::vector<std::uint8_t> m_composite;
		std::vector<Layer>        m_layers;
		int                       m_current = 0;
		Rect                      m_dirty;
		bool                      m_changed = false;
		std::deque<UndoStep>      m_undo, m_redo;

		bool  m_stroking = false;
		float m_lastX = 0, m_lastY = 0, m_dirX = 1, m_dirY = 0;
		float m_carry = 0;  // distance left over before the next dab
		int   m_dabs = 0;
		bool  m_moved = false;
		float m_firstPressure = 1.0f;
	};

	// Rotates RGBA pixels a quarter turn; a_w / a_h are the source size.
	std::vector<std::uint8_t> Rotate90(const std::vector<std::uint8_t>& a_px, int a_w, int a_h, bool a_clockwise);
}
