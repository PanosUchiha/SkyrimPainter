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

#include "Art.h"
#include "Forms.h"
#include "Items.h"
#include "Overlay.h"
#include "Theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <stb_image.h>  // implementation lives in Dds.cpp

namespace Menus
{
	namespace
	{
		struct Thumb
		{
			ID3D11ShaderResourceView* srv = nullptr;
			int                       w = 0, h = 0;
		};
		std::map<std::string, Thumb> g_thumbs;   // painting png -> thumbnail
		std::vector<IUnknown*>       g_release;  // freed at the start of the next frame

		// Loads a painting as a small texture (once). Render thread.
		const Thumb& ThumbFor(const std::string& a_png)
		{
			auto it = g_thumbs.find(a_png);
			if (it != g_thumbs.end()) {
				return it->second;
			}
			Thumb t;
			int   w = 0, h = 0, n = 0;
			if (auto* px = stbi_load(a_png.c_str(), &w, &h, &n, 4)) {
				const int                 tw = (std::min)(w, 384), th = (std::max)(1, h * tw / (std::max)(1, w));
				std::vector<std::uint8_t> small(static_cast<std::size_t>(tw) * th * 4);
				for (int y = 0; y < th; ++y) {
					for (int x = 0; x < tw; ++x) {
						// box filter over the source block
						const int     x0 = x * w / tw, x1 = (std::max)(x0 + 1, (x + 1) * w / tw);
						const int     y0 = y * h / th, y1 = (std::max)(y0 + 1, (y + 1) * h / th);
						std::uint32_t acc[4]{}, cnt = 0;
						for (int yy = y0; yy < y1; ++yy) {
							for (int xx = x0; xx < x1; ++xx) {
								const auto* p = &px[(static_cast<std::size_t>(yy) * w + xx) * 4];
								for (int k = 0; k < 4; ++k) {
									acc[k] += p[k];
								}
								++cnt;
							}
						}
						for (int k = 0; k < 4; ++k) {
							small[(static_cast<std::size_t>(y) * tw + x) * 4 + k] = static_cast<std::uint8_t>(acc[k] / cnt);
						}
					}
				}
				stbi_image_free(px);
				t.srv = Overlay::MakeSRV(small.data(), tw, th);
				t.w = tw;
				t.h = th;
			} else {
				spdlog::warn("Thumbnail: could not read {}", a_png);
			}
			return g_thumbs.emplace(a_png, t).first->second;
		}

		// Draws a painting (or a placeholder) fitted into a_size.
		void Picture(const std::string& a_png, const ImVec2& a_size)
		{
			const auto&  t = ThumbFor(a_png);
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			auto*        dl = ImGui::GetWindowDrawList();
			if (t.srv) {
				const float  s = (std::min)(a_size.x / t.w, a_size.y / t.h);
				const ImVec2 sz(t.w * s, t.h * s);
				const ImVec2 a = pos + (a_size - sz) * 0.5f;
				dl->AddImage(reinterpret_cast<ImTextureID>(t.srv), a, a + sz);
				dl->AddRect(a, a + sz, Theme::GoldDim(), 0.0f, 0, Theme::S(1.0f));
			} else {
				dl->AddRectFilled(pos, pos + a_size, Theme::Well(), 4.0f);
				const char*  msg = "picture missing";
				const ImVec2 ts = ImGui::CalcTextSize(msg);
				dl->AddText(pos + (a_size - ts) * 0.5f, Theme::Muted(), msg);
			}
			ImGui::Dummy(a_size);
		}

		// ---- painting menu
		Wall::MenuInfo g_menu;
		int            g_sizePct = 100;
		bool           g_menuOpen = false;

		// ---- picker
		std::vector<Wall::Painting> g_pick;
		bool                        g_pickOpen = false;
	}

	void OpenPainting(const Wall::MenuInfo& a_info)
	{
		g_menu = a_info;
		g_sizePct = static_cast<int>(std::lround(a_info.scale * 100.0f));
		g_menuOpen = true;
	}

	bool DrawPainting()
	{
		if (!g_menuOpen) {
			return false;
		}
		const ImVec2 screen = ImGui::GetIO().DisplaySize;
		const ImVec2 size(Theme::S(620.0f), Theme::S(700.0f));
		ImGui::SetNextWindowPos((screen - size) * 0.5f, ImGuiCond_Always);
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		bool close = false;
		if (Theme::BeginWindow("##paintingmenu", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
			close = Theme::Header("Painting", g_menu.painting.name.c_str());
			const float w = ImGui::GetContentRegionAvail().x;
			Picture(g_menu.painting.png, ImVec2(w, w * 0.75f * 0.6f));
			ImGui::Dummy(ImVec2(0, Theme::S(8)));
			if (Theme::SliderInt("Size", &g_sizePct, 30, 300, "%d%%", w) || ImGui::IsItemDeactivatedAfterEdit()) {
				Wall::SetScale(g_menu.ref, g_sizePct / 100.0f);
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			Theme::SectionLabel("Frame");
			for (int i = 0; i < Forms::kFrameCount; ++i) {
				if (i % 3) {
					ImGui::SameLine();
				}
				if (Theme::Chip(Forms::kFrameNames[i], g_menu.painting.style == i) && g_menu.painting.style != i) {
					g_menu.painting.style = i;
					Wall::ChangeFrame(g_menu.ref, i);
				}
			}
			ImGui::Dummy(ImVec2(0, Theme::S(14)));
			const float bw = (w - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
			if (Theme::Button("Move", ImVec2(bw, 0))) {
				Wall::Move(g_menu.ref);
				close = true;
			}
			ImGui::SameLine();
			if (Theme::Button("Take down", ImVec2(bw, 0))) {
				Wall::TakeDown(g_menu.ref);
				close = true;
			}
			ImGui::SameLine();
			if (Theme::Button("Close", ImVec2(bw, 0), true) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				close = true;
			}
		}
		Theme::EndWindow();
		if (close) {
			g_menuOpen = false;
		}
		return g_menuOpen;
	}

	void OpenPicker()
	{
		g_pick = Items::Unhung();
		g_pickOpen = !g_pick.empty();
		if (!g_pickOpen) {
			Items::CancelPick();
		}
	}

	bool DrawPicker()
	{
		if (!g_pickOpen) {
			return false;
		}
		const ImVec2 screen = ImGui::GetIO().DisplaySize;
		const ImVec2 size(screen.x * 0.7f, screen.y * 0.75f);
		ImGui::SetNextWindowPos((screen - size) * 0.5f, ImGuiCond_Always);
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		Theme::SetBackdrop();
		bool        cancel = false;
		std::string chosen;
		if (Theme::BeginWindow("##picker", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
			cancel = Theme::Header("Hang a painting", "Choose which painting to hang");
			const float footer = ImGui::GetFrameHeight() + Theme::S(18.0f);
			if (Theme::BeginParchment("##pickgrid", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), true)) {
				const float cell = Theme::S(240.0f);
				const float gap = ImGui::GetStyle().ItemSpacing.x;
				const int   perRow = (std::max)(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
				for (int i = 0; i < static_cast<int>(g_pick.size()); ++i) {
					if (i % perRow) {
						ImGui::SameLine();
					}
					ImGui::PushID(i);
					ImGui::BeginGroup();
					const ImVec2 pos = ImGui::GetCursorScreenPos();
					if (ImGui::InvisibleButton("##card", ImVec2(cell, cell * 0.75f + ImGui::GetTextLineHeightWithSpacing()))) {
						chosen = g_pick[i].id;
					}
					const bool hovered = ImGui::IsItemHovered();
					ImGui::SetCursorScreenPos(pos);
					Picture(g_pick[i].png, ImVec2(cell, cell * 0.75f));
					ImGui::TextUnformatted(g_pick[i].name.c_str());
					if (hovered) {
						ImGui::GetWindowDrawList()->AddRect(pos - ImVec2(4, 4), pos + ImVec2(cell + 4, cell * 0.75f + ImGui::GetTextLineHeightWithSpacing() + 4),
							Theme::Gold(), 4.0f, 0, Theme::S(2.0f));
					}
					ImGui::EndGroup();
					ImGui::PopID();
				}
			}
			Theme::EndParchment();
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			if (Theme::Button("Cancel", ImVec2(Theme::S(200), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				cancel = true;
			}
		}
		Theme::EndWindow();
		if (!chosen.empty()) {
			Items::Hang(chosen);
			g_pickOpen = false;
		} else if (cancel) {
			Items::CancelPick();
			g_pickOpen = false;
		}
		return g_pickOpen;
	}

	void DrawHangHud(const Wall::Status& a_status)
	{
		const ImVec2 screen = ImGui::GetIO().DisplaySize;
		const auto   line1 = fmt::format("Hanging \"{}\"  \xC2\xB7  {} frame  \xC2\xB7  Size {:.0f}%  \xC2\xB7  Distance {:+.2f}", a_status.name, a_status.frame,
			  a_status.scale * 100.0f, a_status.distance);
		const char*  line2 = "Click: hang   \xC2\xB7   Wheel or Left/Right: size   \xC2\xB7   Shift + Wheel: frame   \xC2\xB7   Up/Down: distance   \xC2\xB7   Right-click / Esc: cancel";
		ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - Theme::S(40.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.7f);
		ImGui::Begin("##hanghud", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextUnformatted(line1.c_str());
		ImGui::TextDisabled("%s", line2);
		ImGui::End();
	}

	void Release()
	{
		for (auto& [png, t] : g_thumbs) {
			if (t.srv) {
				g_release.push_back(t.srv);
			}
		}
		g_thumbs.clear();
		g_menuOpen = false;
		g_pickOpen = false;
	}

	void Flush()
	{
		for (auto* p : g_release) {
			p->Release();
		}
		g_release.clear();
	}
}
