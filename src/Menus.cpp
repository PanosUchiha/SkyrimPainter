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

#include "Menus.h"

#include "Theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

namespace Menus
{
	namespace
	{
		Items::EaselMenuInfo g_easel;
		bool                 g_easelOpen = false;
		bool                 g_pickPainting = false;  // showing the list of paintings to put on
		int                  g_confirm = 0;           // 1 take off (painted), 2 scrape painting

		Place::MenuInfo g_painting;
		int             g_sizePct = 100;
		bool            g_paintingOpen = false;

		bool BeginSmall(const char* a_id, const char* a_title, const char* a_subtitle, float a_w)
		{
			const ImVec2 screen = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(screen * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(Theme::S(a_w), 0.0f), ImGuiCond_Always);
			const bool open = Theme::BeginWindow(a_id, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
			return open && !Theme::Header(a_title, a_subtitle, nullptr, 0, true);
		}

		bool Wide(const char* a_label, bool a_enabled = true, const char* a_why = nullptr, bool a_primary = false)
		{
			ImGui::BeginDisabled(!a_enabled);
			const bool clicked = Theme::Button(a_label, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), a_primary);
			ImGui::EndDisabled();
			if (!a_enabled && a_why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("%s", a_why);
			}
			return clicked && a_enabled;
		}
	}

	void OpenEasel(const Items::EaselMenuInfo& a_info)
	{
		g_easel = a_info;
		g_easelOpen = true;
		g_pickPainting = false;
		g_confirm = 0;
	}

	bool DrawEasel()
	{
		if (!g_easelOpen) {
			return false;
		}
		const auto& e = g_easel;
		std::string sub = !e.hasCanvas ? std::string("No canvas on it") :
		                  e.slot >= 0  ? fmt::format("\"{}\" ({})", e.name, e.portrait ? "portrait" : "landscape") :
		                                 fmt::format("{} canvas{}", e.portrait ? "Portrait" : "Landscape", e.painted ? ", painting in progress" : ", blank");
		bool close = false;
		if (!BeginSmall("##easelmenu", "Easel", sub.c_str(), 460.0f)) {
			close = true;
		} else if (g_confirm) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(g_confirm == 1 ? (e.slot >= 0 ? "Take the painting off? Changes since you last finished it are lost." :
			                                                       "Take the canvas off? The paint on it is scraped off.") :
			                                        "Scrape this painting off for good? It frees one of your 50 painting slots and leaves a blank canvas.");
			ImGui::PopTextWrapPos();
			ImGui::Dummy(ImVec2(0, Theme::S(8)));
			if (Wide(g_confirm == 1 ? "Take it off" : "Scrape it off", true, nullptr, true)) {
				if (g_confirm == 1) {
					Items::TakeCanvasOff(e.easel);
				} else {
					Items::ScrapePainting(e.easel);
				}
				close = true;
			}
			if (Wide("Back")) {
				g_confirm = 0;
			}
		} else if (g_pickPainting) {
			Theme::SectionLabel("Put a painting on to work on it");
			for (const auto& [slot, name] : e.paintingsInPack) {
				if (Wide(name.c_str())) {
					Items::EditPainting(e.easel, slot);
					close = true;
				}
			}
			if (Wide("Back")) {
				g_pickPainting = false;
			}
		} else if (!e.hasCanvas) {
			const char* noCanvas = "You need a Blank Canvas (tanning rack: 1 Leather)";
			if (Wide("Put on a blank canvas - landscape", e.haveCanvasItem, noCanvas, true)) {
				Items::SetCanvas(e.easel, false);
				close = true;
			}
			if (Wide("Put on a blank canvas - portrait", e.haveCanvasItem, noCanvas)) {
				Items::SetCanvas(e.easel, true);
				close = true;
			}
			if (Wide("Put on one of your paintings...", !e.paintingsInPack.empty(), "You have no finished paintings with you (take one down from the wall first)")) {
				g_pickPainting = true;
			}
			if (Wide("Fold up the easel")) {
				Items::PickUpEasel(e.easel);
				close = true;
			}
		} else {
			const bool tools = e.havePalette && e.havePaints;
			if (Wide("Paint", tools, "You need a Painter's Palette and Paints", true)) {
				Items::Paint(e.easel);
				close = true;
			}
			if (Wide(e.portrait ? "Turn the canvas to landscape" : "Turn the canvas to portrait")) {
				Items::TurnCanvas(e.easel);
				close = true;
			}
			if (Wide(e.slot >= 0 ? "Take the painting off" : "Take the canvas off")) {
				if (e.painted && e.slot < 0) {
					g_confirm = 1;
				} else {
					Items::TakeCanvasOff(e.easel);
					close = true;
				}
			}
			if (e.slot >= 0 && Wide("Scrape the painting off (frees its slot)")) {
				g_confirm = 2;
			}
			if (!e.painted && e.freeSlots == 0) {
				Theme::TextColoredWrapped(Theme::Warning(), "All 50 painting slots are used: scrape an old painting off to finish a new one.");
			}
		}
		if (!close && g_easelOpen && Wide("Close")) {
			close = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			close = true;
		}
		Theme::EndWindow();
		if (close) {
			g_easelOpen = false;
		}
		return g_easelOpen;
	}

	void OpenPainting(const Place::MenuInfo& a_info)
	{
		g_painting = a_info;
		g_sizePct = static_cast<int>(std::lround(a_info.scale * 100.0f));
		g_paintingOpen = true;
	}

	bool DrawPainting()
	{
		if (!g_paintingOpen) {
			return false;
		}
		Items::SlotView v;
		Items::GetSlot(g_painting.slot, v);
		bool close = false;
		if (!BeginSmall("##paintingmenu", "Painting", v.name.c_str(), 440.0f)) {
			close = true;
		} else {
			if (Theme::SliderInt("Size", &g_sizePct, 30, 300, "%d%%", ImGui::GetContentRegionAvail().x)) {
				Place::SetScale(g_painting.ref, g_sizePct / 100.0f);
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			if (Wide("Move")) {
				Place::Move(g_painting.ref);
				close = true;
			}
			if (Wide("Take down")) {
				Place::TakeDown(g_painting.ref);
				close = true;
			}
			if (Wide("Close", true, nullptr, true)) {
				close = true;
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			close = true;
		}
		Theme::EndWindow();
		if (close) {
			g_paintingOpen = false;
		}
		return g_paintingOpen;
	}

	void DrawPlaceHud(const Place::Status& a_s)
	{
		const ImVec2 screen = ImGui::GetIO().DisplaySize;
		std::string  line1, line2;
		if (a_s.mode == Place::Mode::Easel) {
			line1 = fmt::format("Setting up the easel  \xC2\xB7  turned {:+d}\xC2\xB0", a_s.turnDeg);
			line2 = "Click: set it up   \xC2\xB7   Wheel or Left/Right: turn (Shift: finer)   \xC2\xB7   Right-click / Esc: cancel";
		} else {
			line1 = fmt::format("Hanging \"{}\"  \xC2\xB7  Size {:.0f}%  \xC2\xB7  Distance {:+.2f}", a_s.name, a_s.scale * 100.0f, a_s.distance);
			line2 = "Click: hang   \xC2\xB7   Wheel or Left/Right: size   \xC2\xB7   Up/Down: distance from the wall   \xC2\xB7   Right-click / Esc: cancel";
		}
		ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - Theme::S(40.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.7f);
		ImGui::Begin("##placehud", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextUnformatted(line1.c_str());
		ImGui::TextDisabled("%s", line2.c_str());
		ImGui::End();
	}

	void Release()
	{
		g_easelOpen = false;
		g_paintingOpen = false;
	}

	void Flush() {}
}
