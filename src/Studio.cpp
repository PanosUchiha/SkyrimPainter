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

#include "Studio.h"

#include "Art.h"
#include "Overlay.h"
#include "Paint.h"
#include "PaletteSpots.h"
#include "Spectral.h"
#include "Store.h"
#include "Theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <stb_image.h>  // implementation lives in Dds.cpp

namespace fs = std::filesystem;

namespace Studio
{
	namespace
	{
		using Rgb = Spectral::Rgb;
		constexpr int kMaxMixed = 24;

		struct Pigment
		{
			const char* name;
			Rgb         rgb;
		};
		// The Paints: what is on the palette when you start. Everything else comes from mixing.
		constexpr Pigment kPigments[PaletteSpots::kCount] = {
			{ "Bonemeal White", { 245, 242, 234 } },
			{ "Sun Gold", { 237, 196, 41 } },
			{ "Yellow Ochre", { 196, 150, 61 } },
			{ "Burnt Sienna", { 138, 69, 41 } },
			{ "Dragon's Blood Red", { 176, 33, 31 } },
			{ "Madder Rose", { 199, 79, 99 } },
			{ "Nightshade Violet", { 99, 56, 120 } },
			{ "Lapis Blue", { 36, 61, 161 } },
			{ "Frost Blue", { 31, 69, 110 } },
			{ "Pine Green", { 41, 110, 79 } },
			{ "Raw Umber", { 99, 79, 59 } },
			{ "Soot Black", { 28, 26, 26 } },
		};

		struct Spot
		{
			float x, y;  // 0..1 on the palette picture
			Rgb   c;
			bool  mixed;
		};

		struct Picture
		{
			ID3D11ShaderResourceView* srv = nullptr;
			int                       w = 0, h = 0;
			bool                      tried = false;
		};

		// ---- state (render thread)
		bool                      g_open = false;
		Items::Work               g_work;
		Paint::Painting           g_paint;
		Paint::Brush              g_brush;  // in hand
		Paint::Brush              g_rack[Paint::kBrushCount];  // the brushes put down keep the paint that was on them
		ImVec2                    g_dir{ 1.0f, 0.0f };         // smoothed stroke direction on the canvas (for the cursor)
		ImVec2                    g_lastMouse{ -1.0f, -1.0f };
		bool                      g_overCanvas = false;
		bool                      g_hideHud = false;  // Tab: only the canvas stays up
		bool                      g_layoutReset = true;
		float                     g_pressure = 0.6f;
		std::vector<Spot>         g_spots;
		bool                      g_spotsLoaded = false;
		ID3D11Texture2D*          g_tex = nullptr;
		ID3D11ShaderResourceView* g_srv = nullptr;
		std::vector<IUnknown*>    g_release;  // freed at the start of the next frame
		Picture                   g_palettePic, g_brushPic;
		char                      g_name[64]{};
		bool                      g_openFinish = false, g_openClear = false;
		std::string               g_hint;
		double                    g_hintUntil = 0.0;
		bool                      g_painting = false;  // left button held on the canvas

		void Hint(const char* a_text)
		{
			g_hint = a_text;
			g_hintUntil = ImGui::GetTime() + 3.0;
		}

		ImU32 Col(const Rgb& c, int a = 255) { return IM_COL32(c[0], c[1], c[2], a); }

		// ---- the bare canvas: plain white primed canvas with a fine weave (as canvas_blank.dds)
		float Hash(int x, int y)
		{
			std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
		}

		std::vector<std::uint8_t> MakeBlank(int W, int H)
		{
			std::vector<std::uint8_t> blank(static_cast<std::size_t>(W) * H * 4);
			std::vector<float>        col(W), row(H);
			for (int x = 0; x < W; ++x) {
				col[x] = (Hash(x, 7) - 0.5f) * 3.0f;
			}
			for (int y = 0; y < H; ++y) {
				row[y] = (Hash(13, y) - 0.5f) * 3.0f;
			}
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					const float d = std::sin(x * 2.094f) * std::sin(y * 2.094f) * 3.0f + col[x] + row[y];
					auto*       p = &blank[(static_cast<std::size_t>(y) * W + x) * 4];
					p[0] = static_cast<std::uint8_t>(std::clamp(248.0f + d, 0.0f, 255.0f));
					p[1] = static_cast<std::uint8_t>(std::clamp(247.0f + d, 0.0f, 255.0f));
					p[2] = static_cast<std::uint8_t>(std::clamp(244.0f + d, 0.0f, 255.0f));
					p[3] = 255;
				}
			}
			return blank;
		}

		// ---- textures
		void LoadPicture(Picture& a_pic, const wchar_t* a_file)
		{
			if (a_pic.tried) {
				return;
			}
			a_pic.tried = true;
			std::error_code ec;
			const auto      path = Art::ToUtf8(fs::absolute(fs::path(L"Data\\SKSE\\Plugins\\SkyrimPainter\\UI") / a_file, ec));
			int             w = 0, h = 0, n = 0;
			if (auto* px = stbi_load(path.c_str(), &w, &h, &n, 4)) {
				a_pic.srv = Overlay::MakeSRV(px, w, h);
				a_pic.w = w;
				a_pic.h = h;
				stbi_image_free(px);
			} else {
				spdlog::warn("Studio: {} missing", path);
			}
		}

		bool CreateCanvasTexture()
		{
			auto* dev = Overlay::Device();
			if (!dev) {
				return false;
			}
			D3D11_TEXTURE2D_DESC d{};
			d.Width = static_cast<UINT>(g_paint.Width());
			d.Height = static_cast<UINT>(g_paint.Height());
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA sd{ g_paint.Composite().data(), static_cast<UINT>(g_paint.Width()) * 4, 0 };
			if (FAILED(dev->CreateTexture2D(&d, &sd, &g_tex)) || !g_tex) {
				g_tex = nullptr;
				return false;
			}
			if (FAILED(dev->CreateShaderResourceView(g_tex, nullptr, &g_srv)) || !g_srv) {
				g_tex->Release();
				g_tex = nullptr;
				return false;
			}
			return true;
		}

		void Upload()
		{
			const auto r = g_paint.TakeDirty();
			if (!g_tex || r.Empty()) {
				return;
			}
			D3D11_BOX   box{ static_cast<UINT>(r.x0), static_cast<UINT>(r.y0), 0, static_cast<UINT>(r.x1), static_cast<UINT>(r.y1), 1 };
			const auto* src = &g_paint.Composite()[(static_cast<std::size_t>(r.y0) * g_paint.Width() + r.x0) * 4];
			Overlay::Context()->UpdateSubresource(g_tex, 0, &box, src, static_cast<UINT>(g_paint.Width()) * 4, 0);
		}

		// ---- the palette's paint spots (pigments + what the player mixed, kept in the INI)
		void LoadSpots()
		{
			if (g_spotsLoaded) {
				return;
			}
			g_spotsLoaded = true;
			for (int i = 0; i < PaletteSpots::kCount; ++i) {
				g_spots.push_back({ PaletteSpots::kSpots[i][0], PaletteSpots::kSpots[i][1], kPigments[i].rgb, false });
			}
			char buf[2048]{};
			::GetPrivateProfileStringA("Palette", "sMixed", "", buf, sizeof buf, Overlay::IniPath().c_str());
			std::stringstream ss(buf);
			for (std::string item; std::getline(ss, item, ';');) {
				unsigned rgb = 0;
				float    x = 0, y = 0;
				if (std::sscanf(item.c_str(), "%6x@%f,%f", &rgb, &x, &y) == 3) {
					g_spots.push_back({ std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f),
						{ static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8), static_cast<std::uint8_t>(rgb) }, true });
				}
			}
		}

		void SaveSpots()
		{
			std::string out;
			for (const auto& s : g_spots) {
				if (s.mixed) {
					out += fmt::format("{}{:02X}{:02X}{:02X}@{:.4f},{:.4f}", out.empty() ? "" : ";", s.c[0], s.c[1], s.c[2], s.x, s.y);
				}
			}
			::WritePrivateProfileStringA("Palette", "sMixed", out.c_str(), Overlay::IniPath().c_str());
		}

		// ---- saving: the layers to the work folder, then the game side (worker thread)
		void SaveAndThen(bool a_finish, std::string a_name)
		{
			Store::Doc doc;
			doc.w = g_paint.Width();
			doc.h = g_paint.Height();
			for (const auto& l : g_paint.Layers()) {
				doc.layers.push_back({ l.name, l.visible, l.opacity, l.px });
			}
			std::vector<std::uint8_t> flat = g_paint.Composite();
			const auto                dir = g_work.dir;
			const auto                easel = g_work.easel;
			std::thread([doc = std::move(doc), flat = std::move(flat), dir, easel, a_finish, name = std::move(a_name)]() mutable {
				std::string err;
				if (!Store::SaveDoc(dir, doc, flat, err)) {
					spdlog::error("Studio: saving the painting failed: {}", err);
					Art::Notify("SkyrimPainter: could not save the painting (see SkyrimPainter.log)");
					return;
				}
				if (a_finish) {
					Items::Finish(easel, std::move(name), std::move(flat), doc.w, doc.h);
				} else {
					Items::StepAway(easel, std::move(flat), doc.w, doc.h);
				}
			}).detach();
		}

		void Close()
		{
			if (g_srv) {
				g_release.push_back(g_srv);
				g_srv = nullptr;
			}
			if (g_tex) {
				g_release.push_back(g_tex);
				g_tex = nullptr;
			}
			g_open = false;
			g_painting = false;
			g_paint = Paint::Painting{};
		}

		void StepAway()
		{
			if (g_paint.Changed()) {
				SaveAndThen(false, {});
			}
			Close();
		}

		// ---- drawing helpers
		void PaintBlob(ImDrawList* a_dl, const ImVec2& a_c, float a_r, const Rgb& a_col, std::uint32_t a_seed)
		{
			// a dollop of paint: a few overlapping lumps, a darker rim and a wet highlight
			auto dark = Col({ static_cast<std::uint8_t>(a_col[0] * 0.7f), static_cast<std::uint8_t>(a_col[1] * 0.7f), static_cast<std::uint8_t>(a_col[2] * 0.7f) });
			for (int k = 0; k < 4; ++k) {
				const float ang = (a_seed * 37 + k * 97) % 360 * 0.01745f;
				const float off = a_r * 0.28f;
				a_dl->AddCircleFilled(a_c + ImVec2(std::cos(ang) * off, std::sin(ang) * off), a_r * (0.72f - k * 0.04f) + 1.0f, dark, 20);
			}
			for (int k = 0; k < 4; ++k) {
				const float ang = (a_seed * 37 + k * 97) % 360 * 0.01745f;
				const float off = a_r * 0.25f;
				a_dl->AddCircleFilled(a_c + ImVec2(std::cos(ang) * off, std::sin(ang) * off), a_r * (0.7f - k * 0.04f), Col(a_col), 20);
			}
			a_dl->AddCircleFilled(a_c + ImVec2(-a_r * 0.25f, -a_r * 0.3f), a_r * 0.18f, IM_COL32(255, 255, 255, 90), 12);
		}

		void DrawBrushPic(ImDrawList* a_dl, const ImVec2& a_tip, float a_len, const Paint::Brush* a_paint, int a_alpha = 255)
		{
			// the picture's bristle tip goes to a_tip; a_len = picture width on screen
			if (!g_brushPic.srv) {
				a_dl->AddLine(a_tip, a_tip + ImVec2(-a_len * 0.7f, a_len * 0.4f), IM_COL32(60, 40, 25, a_alpha), Theme::S(4.0f));
				return;
			}
			const float  h = a_len / PaletteSpots::kBrushAspect;
			const ImVec2 origin = a_tip - ImVec2(PaletteSpots::kBrushTip[0] * a_len, PaletteSpots::kBrushTip[1] * h);
			a_dl->AddImage(reinterpret_cast<ImTextureID>(g_brushPic.srv), origin, origin + ImVec2(a_len, h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a_alpha));
			if (a_paint && a_paint->Loaded()) {
				const float r = Theme::S(5.0f) + a_len * 0.02f;
				a_dl->AddCircleFilled(a_tip + ImVec2(-r * 0.6f, r * 0.6f), r, Col(a_paint->color, a_alpha), 16);
			}
		}

		// ---- panels
		void DrawLayers(float a_w)
		{
			Theme::SectionLabel("Layers (front at the top)");
			auto&     layers = g_paint.Layers();
			const int n = static_cast<int>(layers.size());
			for (int i = n - 1; i >= 0; --i) {
				ImGui::PushID(i);
				bool vis = layers[i].visible;
				if (ImGui::Checkbox("##vis", &vis)) {
					g_paint.SetLayerVisible(i, vis);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Show / hide this layer");
				}
				ImGui::SameLine();
				if (ImGui::Selectable(layers[i].name.c_str(), g_paint.Current() == i, 0, ImVec2(a_w - ImGui::GetFrameHeight() - Theme::S(8), 0))) {
					g_paint.SetCurrent(i);
				}
				ImGui::PopID();
			}
			const float bw = (a_w - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
			ImGui::BeginDisabled(n >= Paint::kMaxLayers);
			if (Theme::Button("New", ImVec2(bw, 0))) {
				g_paint.AddLayer(fmt::format("Layer {}", n + 1));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(n <= 1);
			if (Theme::Button("Del", ImVec2(bw, 0))) {
				g_paint.DeleteLayer(g_paint.Current());
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (Theme::Button("Up", ImVec2(bw, 0))) {
				g_paint.MoveLayer(g_paint.Current(), +1);
			}
			ImGui::SameLine();
			if (Theme::Button("Down", ImVec2(bw, 0))) {
				g_paint.MoveLayer(g_paint.Current(), -1);
			}
			float op = layers[g_paint.Current()].opacity * 100.0f;
			if (Theme::SliderFloat("Layer opacity", &op, 0.0f, 100.0f, "%.0f%%", a_w)) {
				g_paint.SetLayerOpacity(g_paint.Current(), op / 100.0f);
			}
		}

		void DrawTools(float a_w)
		{
			DrawLayers(a_w);
			ImGui::Dummy(ImVec2(0, Theme::S(8)));
			Theme::SectionLabel("History");
			const float hw = (a_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
			ImGui::BeginDisabled(!g_paint.CanUndo());
			if (Theme::Button("Undo", ImVec2(hw, 0))) {
				g_paint.Undo();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!g_paint.CanRedo());
			if (Theme::Button("Redo", ImVec2(hw, 0))) {
				g_paint.Redo();
			}
			ImGui::EndDisabled();
			if (Theme::Button("Scrape this layer", ImVec2(a_w, 0))) {
				g_openClear = true;
			}
		}

		// The brush's footprint as an outline: a circle for the round brushes, an ellipse / flat oval for the
		// filbert and the flats, turned with the stroke.
		void DrawFootprint(ImDrawList* a_dl, const ImVec2& a_c, float a_scale)
		{
			const auto& bt = Paint::kBrushes[g_brush.type];
			const float rA = std::max(1.5f, bt.radius * (0.45f + 0.55f * g_pressure) * a_scale);  // across the stroke
			const float rL = std::max(1.0f, rA / bt.aspect);                                       // along it
			constexpr int kN = 40;
			ImVec2        pts[kN];
			for (int i = 0; i < kN; ++i) {
				const float t = 6.2831853f * i / kN;
				const float along = std::cos(t) * rL, across = std::sin(t) * rA;
				pts[i] = a_c + ImVec2(g_dir.x * along - g_dir.y * across, g_dir.y * along + g_dir.x * across);
			}
			a_dl->AddPolyline(pts, kN, IM_COL32(0, 0, 0, 120), ImDrawFlags_Closed, 2.4f);
			a_dl->AddPolyline(pts, kN, g_brush.Loaded() ? Col(g_brush.color, 230) : IM_COL32(255, 255, 255, 170), ImDrawFlags_Closed, 1.2f);
		}

		// Canvas: shows the painting fitted into a_size; the left mouse button paints.
		void DrawCanvas(const ImVec2& a_size)
		{
			const float  W = static_cast<float>(g_paint.Width()), H = static_cast<float>(g_paint.Height());
			const float  scale = std::min(a_size.x / W, a_size.y / H);
			const ImVec2 size(W * scale, H * scale);
			const ImVec2 origin = ImGui::GetCursorScreenPos() + (a_size - size) * 0.5f;
			auto*        dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(origin + ImVec2(Theme::S(5), Theme::S(6)), origin + size + ImVec2(Theme::S(5), Theme::S(6)), IM_COL32(0, 0, 0, 70), 2.0f);
			ImGui::SetCursorScreenPos(origin);
			ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft);
			const bool hovered = ImGui::IsItemHovered();
			const bool active = ImGui::IsItemActive();
			dl->AddImage(reinterpret_cast<ImTextureID>(g_srv), origin, origin + size);
			dl->AddRect(origin, origin + size, IM_COL32(60, 44, 30, 255), 0.0f, 0, Theme::S(2.0f));

			const auto&  io = ImGui::GetIO();
			const ImVec2 m = (io.MousePos - origin) / scale;
			if ((hovered || g_painting) && io.MouseWheel != 0.0f) {
				g_pressure = std::clamp(g_pressure + 0.05f * io.MouseWheel, 0.05f, 1.0f);
			}
			// which way the brush is travelling (the flat brushes lie across it)
			if (g_lastMouse.x >= 0.0f) {
				const ImVec2 d = io.MousePos - g_lastMouse;
				const float  len = std::sqrt(d.x * d.x + d.y * d.y);
				if (len > 1.5f) {
					g_dir = g_dir * 0.75f + d / len * 0.25f;
					const float l2 = std::sqrt(g_dir.x * g_dir.x + g_dir.y * g_dir.y);
					g_dir = l2 > 0.01f ? g_dir / l2 : ImVec2(1.0f, 0.0f);
				}
			}
			g_lastMouse = io.MousePos;

			if (ImGui::IsItemActivated()) {
				if (g_brush.type < 0) {
					Hint("Pick up a brush first: click one below the palette");
				} else if (!g_brush.Loaded()) {
					Hint("The brush is clean: click a paint on the palette to load it");
				} else {
					g_paint.BeginStroke(g_brush, m.x, m.y, g_pressure);
					g_painting = true;
				}
			} else if (active && g_painting) {
				g_paint.StrokeTo(g_brush, m.x, m.y, g_pressure);
			}
			if (g_painting && !active) {
				g_paint.EndStroke(g_brush);
				g_painting = false;
			}
			g_overCanvas = (hovered || active) && !Overlay::Looking();
			if (g_overCanvas && g_brush.type >= 0) {
				DrawFootprint(ImGui::GetForegroundDrawList(), io.MousePos, scale);
			}
		}

		void DrawPressure(float a_w)
		{
			Theme::SectionLabel("Pressure");
			float pr = g_pressure * 100.0f;
			if (Theme::SliderFloat("##pressure", &pr, 5.0f, 100.0f, "%.0f%%", a_w)) {
				g_pressure = pr / 100.0f;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Light: thin, sheer, broken strokes. Heavy: the bristles splay and lay down thick paint.\nThe mouse wheel over the canvas changes it too.");
			}
		}

		void DrawPalettePanel(float a_w)
		{
			const auto& io = ImGui::GetIO();
			auto*       dl = ImGui::GetWindowDrawList();
			const bool  click = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

			// ---- the palette
			const float  ph = a_w / PaletteSpots::kAspect;
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(a_w, ph));
			if (g_palettePic.srv) {
				dl->AddImage(reinterpret_cast<ImTextureID>(g_palettePic.srv), p0, p0 + ImVec2(a_w, ph));
			} else {
				dl->AddEllipseFilled(p0 + ImVec2(a_w, ph) * 0.5f, a_w * 0.48f, ph * 0.48f, IM_COL32(150, 100, 60, 255));
			}
			const float blobR = Theme::S(17.0f);
			int         hoverSpot = -1;
			for (int i = 0; i < static_cast<int>(g_spots.size()); ++i) {
				const ImVec2 c = p0 + ImVec2(g_spots[i].x * a_w, g_spots[i].y * ph);
				PaintBlob(dl, c, blobR, g_spots[i].c, static_cast<std::uint32_t>(i * 7 + 3));
				const ImVec2 d = io.MousePos - c;
				if (d.x * d.x + d.y * d.y < blobR * blobR) {
					hoverSpot = i;
				}
			}
			const bool overPalette = ImGui::IsMouseHoveringRect(p0, p0 + ImVec2(a_w, ph));
			if (hoverSpot >= 0) {
				const auto& s = g_spots[hoverSpot];
				dl->AddCircle(p0 + ImVec2(s.x * a_w, s.y * ph), blobR + 3.0f, Theme::Gold(), 0, Theme::S(2.0f));
				ImGui::SetTooltip("%s", s.mixed ? "Your mix (Ctrl + click to scrape it off)" : kPigments[std::min(hoverSpot, PaletteSpots::kCount - 1)].name);
			}
			if (click && overPalette) {
				if (hoverSpot >= 0 && io.KeyCtrl && g_spots[hoverSpot].mixed) {
					g_spots.erase(g_spots.begin() + hoverSpot);
					SaveSpots();
				} else if (hoverSpot >= 0) {
					if (g_brush.type < 0) {
						Hint("Pick up a brush first: click one below the palette");
					} else if (g_brush.Loaded() && g_brush.color != g_spots[hoverSpot].c) {
						g_brush.color = Spectral::Mix(g_brush.color, g_spots[hoverSpot].c, 0.5);  // mixes on the brush like real paint
						g_brush.load = 1.0f;
						Hint("Mixed into the paint on the brush - click the bare palette to put the mix down, the rag to wipe it");
					} else {
						g_brush.color = g_spots[hoverSpot].c;
						g_brush.load = 1.0f;
					}
				} else if (g_brush.Loaded()) {
					const float x = (io.MousePos.x - p0.x) / a_w, y = (io.MousePos.y - p0.y) / ph;
					if (static_cast<int>(g_spots.size()) - PaletteSpots::kCount >= kMaxMixed) {
						Hint("The palette is full - Ctrl + click a mix to scrape it off");
					} else {
						g_spots.push_back({ x, y, g_brush.color, true });
						SaveSpots();
					}
				}
			}

			// ---- brushes and the rag
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			Theme::SectionLabel("Brushes");
			const float  cell = (a_w - Theme::S(70.0f)) / Paint::kBrushCount;
			const float  rowH = Theme::S(96.0f);
			const ImVec2 b0 = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(a_w, rowH + ImGui::GetTextLineHeightWithSpacing()));
			for (int i = 0; i < Paint::kBrushCount; ++i) {
				const ImVec2 c0 = b0 + ImVec2(cell * i, 0);
				const bool   hov = ImGui::IsMouseHoveringRect(c0, c0 + ImVec2(cell, rowH));
				if (hov) {
					dl->AddRectFilled(c0, c0 + ImVec2(cell, rowH), IM_COL32(255, 255, 255, 18), 6.0f);
				}
				const char* label = Paint::kBrushes[i].name;
				const float tw = ImGui::CalcTextSize(label).x;
				dl->AddText(c0 + ImVec2((cell - tw) * 0.5f, rowH), Theme::InkSoft(), label);
				if (g_brush.type == i) {
					dl->AddRect(c0 + ImVec2(4, 4), c0 + ImVec2(cell - 4, rowH - 4), Theme::GoldDim(), 6.0f, 0, 1.0f);
					const char* inHand = "in hand";
					dl->AddText(c0 + ImVec2((cell - ImGui::CalcTextSize(inHand).x) * 0.5f, rowH * 0.45f), Theme::Muted(), inHand);
				} else {
					const float len = cell * (0.75f + 0.08f * i);
					DrawBrushPic(dl, c0 + ImVec2(cell * 0.5f + len * 0.4f, rowH * 0.5f - len * 0.24f), len, nullptr);
				}
				if (hov) {
					ImGui::SetTooltip("%s - %s", label, g_brush.type == i ? "click to put it down" : "click to pick it up");
					if (click) {
						if (g_brush.type >= 0) {
							g_rack[g_brush.type] = g_brush;  // put down with whatever paint is on it
						}
						if (g_brush.type == i) {
							g_brush = Paint::Brush{};
						} else {
							g_brush = g_rack[i];
							g_brush.type = i;
							if (!g_brush.Loaded()) {
								Hint("Click a paint to load the brush");
							}
						}
					}
				}
			}
			// the rag
			const ImVec2 r0 = b0 + ImVec2(cell * Paint::kBrushCount + Theme::S(6), Theme::S(14));
			const ImVec2 r1 = r0 + ImVec2(Theme::S(60), rowH - Theme::S(28));
			dl->AddRectFilled(r0, r1, IM_COL32(205, 196, 176, 255), Theme::S(8));
			for (int k = 1; k < 4; ++k) {
				dl->AddLine(r0 + ImVec2(0, (r1.y - r0.y) * k / 4), r0 + ImVec2(r1.x - r0.x, (r1.y - r0.y) * k / 4 + Theme::S(4)), IM_COL32(160, 150, 130, 255), 1.0f);
			}
			dl->AddText(ImVec2(r0.x + Theme::S(14), r1.y + Theme::S(14)), Theme::InkSoft(), "Rag");
			if (ImGui::IsMouseHoveringRect(r0, r1)) {
				ImGui::SetTooltip("Click: wipe the brush clean");
				if (click && g_brush.type >= 0) {
					g_brush.load = 0.0f;
					g_brush.color = {};
				}
			}

			// ---- what's in hand
			ImGui::Dummy(ImVec2(0, Theme::S(4)));
			if (g_brush.type < 0) {
				Theme::TextColoredWrapped(Theme::InkSoft(), "No brush in hand.");
			} else {
				const ImVec2 sw = ImGui::GetCursorScreenPos();
				if (g_brush.Loaded()) {
					dl->AddRectFilled(sw, sw + ImVec2(Theme::S(22), Theme::S(22)), Col(g_brush.color), 4.0f);
				} else {
					dl->AddRect(sw, sw + ImVec2(Theme::S(22), Theme::S(22)), Theme::InkSoft(), 4.0f);
				}
				ImGui::Dummy(ImVec2(Theme::S(26), Theme::S(22)));
				ImGui::SameLine();
				ImGui::Text("%s - %s", Paint::kBrushes[g_brush.type].name, g_brush.Loaded() ? "loaded" : "clean");
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			DrawPressure(a_w);
			ImGui::Dummy(ImVec2(0, Theme::S(4)));
			Theme::TextColoredWrapped(Theme::InkSoft(),
				"Click a brush to pick it up, a paint to load it (a second paint mixes in), the bare palette to put a mix down, "
				"the rag to wipe the brush. Drag on the canvas to paint. Hold the right mouse button to look around, Tab hides the panels.");
		}

		bool ConfirmPopup(const char* a_id, const char* a_text, const char* a_yes)
		{
			bool yes = false;
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal(a_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
				Theme::DrawChrome();
				ImGui::Dummy(ImVec2(Theme::S(380), Theme::S(4)));
				ImGui::PushTextWrapPos(Theme::S(380));
				ImGui::TextUnformatted(a_text);
				ImGui::PopTextWrapPos();
				ImGui::Dummy(ImVec2(0, Theme::S(10)));
				if (Theme::Button(a_yes, ImVec2(Theme::S(180), 0), true)) {
					yes = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (Theme::Button("Cancel", ImVec2(Theme::S(180), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			return yes;
		}
	}

	void Flush()
	{
		for (auto* p : g_release) {
			p->Release();
		}
		g_release.clear();
	}

	bool Open(const Items::Work& a_work)
	{
		Close();
		LoadSpots();
		LoadPicture(g_palettePic, L"palette.png");
		LoadPicture(g_brushPic, L"brush.png");
		g_work = a_work;
		const int W = Art::Width(a_work.portrait), H = Art::Height(a_work.portrait);
		g_paint.Reset(W, H, MakeBlank(W, H));
		if (auto doc = Store::LoadDoc(a_work.dir)) {
			if (doc->w == W && doc->h == H) {
				auto& layers = g_paint.Layers();
				layers.clear();
				for (auto& l : doc->layers) {
					layers.push_back({ l.name, l.visible, l.opacity, std::move(l.px) });
				}
				g_paint.SetCurrent(static_cast<int>(layers.size()) - 1);
				g_paint.RecompositeAll();
				g_paint.MarkSaved();
				spdlog::info("Studio: continuing {} ({} layer(s))", Art::ToUtf8(a_work.dir), layers.size());
			} else {
				spdlog::warn("Studio: saved painting is {}x{}, the canvas {}x{} - starting blank", doc->w, doc->h, W, H);
			}
		}
		g_paint.TakeDirty();
		if (!CreateCanvasTexture()) {
			spdlog::error("Studio: could not create the canvas texture");
			Close();
			return false;
		}
		std::snprintf(g_name, sizeof g_name, "%s", a_work.name.c_str());
		g_layoutReset = true;  // portrait and landscape lay out differently
		g_hideHud = false;
		g_lastMouse = ImVec2(-1.0f, -1.0f);
		g_open = true;
		return true;
	}

	bool Draw()
	{
		if (!g_open) {
			return false;
		}
		Upload();
		const auto&  io = ImGui::GetIO();
		const ImVec2 screen = io.DisplaySize;
		const bool   looking = Overlay::Looking();
		bool         stepAway = false;
		g_overCanvas = false;

		// No backdrop: the world stays in view so you can paint what's in front of you. The canvas sits on
		// the left (drag its top bar to move it, the corner to resize), the palette and the tools on the right.
		const ImGuiCond cond = g_layoutReset ? ImGuiCond_Always : ImGuiCond_Appearing;
		const float     panelW = Theme::S(400.0f), toolsW = Theme::S(260.0f), margin = Theme::S(18.0f);
		{
			const float aspect = static_cast<float>(g_paint.Width()) / static_cast<float>(g_paint.Height());
			const float bar = ImGui::GetFrameHeight() + Theme::S(8.0f);
			float       ch = screen.y * 0.70f, cw = ch * aspect;
			const float maxW = screen.x - panelW - toolsW - margin * 4.0f;
			if (cw > maxW) {
				cw = maxW;
				ch = cw / aspect;
			}
			ImGui::SetNextWindowPos(ImVec2(margin, (screen.y - ch - bar) * 0.45f), cond);
			ImGui::SetNextWindowSize(ImVec2(cw + Theme::S(16.0f), ch + bar + Theme::S(16.0f)), cond);
			ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::S(240.0f), Theme::S(200.0f)), screen);
			ImGui::SetNextWindowBgAlpha(looking ? 0.0f : 0.35f);
			if (ImGui::Begin("##easelcanvas", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
				// the top bar: grab it to move the canvas
				const std::string title = g_work.slot >= 0 ? fmt::format("\"{}\"", g_work.name) : std::string("A fresh canvas");
				ImGui::TextColored(ImVec4(0.92f, 0.85f, 0.68f, looking ? 0.4f : 1.0f), "%s", title.c_str());
				if (ImGui::GetTime() < g_hintUntil) {
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.55f, 1.0f), "  %s", g_hint.c_str());
				}
				DrawCanvas(ImGui::GetContentRegionAvail());
			}
			ImGui::End();
		}

		if (!g_hideHud && !looking) {
			// palette, brushes, rag, pressure: right edge, bottom
			ImGui::SetNextWindowPos(ImVec2(screen.x - margin, screen.y - margin), cond, ImVec2(1.0f, 1.0f));
			ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f), ImGuiCond_Always);
			if (Theme::BeginWindow("##palettewin", nullptr)) {
				DrawPalettePanel(ImGui::GetContentRegionAvail().x);
			}
			Theme::EndWindow();

			// layers, history, step away / finish: right edge, top
			ImGui::SetNextWindowPos(ImVec2(screen.x - margin, margin), cond, ImVec2(1.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(toolsW, 0.0f), ImGuiCond_Always);
			if (Theme::BeginWindow("##toolswin", nullptr)) {
				const float w = ImGui::GetContentRegionAvail().x;
				DrawTools(w);
				ImGui::Dummy(ImVec2(0, Theme::S(8)));
				if (Theme::Button("Finish painting", ImVec2(w, 0), true)) {
					g_openFinish = true;
				}
				if (Theme::Button("Step away", ImVec2(w, 0))) {
					stepAway = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Leave the painting on the easel and come back to it later (Esc)");
				}
				if (Theme::Button("Reset the layout", ImVec2(w, 0))) {
					g_layoutReset = true;
				} else {
					g_layoutReset = false;
				}
			}
			Theme::EndWindow();
		} else {
			g_layoutReset = false;
		}

		// ---- popups
		if (g_openClear) {
			ImGui::OpenPopup("##clear");
			g_openClear = false;
		}
		if (ConfirmPopup("##clear", "Scrape all the paint off this layer? (Undo can bring it back.)", "Scrape")) {
			g_paint.ClearLayer(g_paint.Current());
		}
		if (g_openFinish) {
			ImGui::OpenPopup("##finish");
			g_openFinish = false;
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		if (ImGui::BeginPopupModal("##finish", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
			Theme::DrawChrome();
			ImGui::Dummy(ImVec2(Theme::S(420), Theme::S(4)));
			ImGui::TextUnformatted("Name your painting");
			ImGui::SetNextItemWidth(Theme::S(420));
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			const bool enter = ImGui::InputText("##name", g_name, sizeof g_name, ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::Dummy(ImVec2(0, Theme::S(10)));
			bool done = enter;
			if (Theme::Button("Finish", ImVec2(Theme::S(200), 0), true)) {
				done = true;
			}
			ImGui::SameLine();
			if (Theme::Button("Keep painting", ImVec2(Theme::S(200), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				ImGui::CloseCurrentPopup();
			}
			if (done) {
				ImGui::CloseCurrentPopup();
				SaveAndThen(true, g_name);
				Close();
			}
			ImGui::EndPopup();
		}

		if (g_open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !io.WantTextInput) {
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
				g_paint.Undo();
			} else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
				g_paint.Redo();
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
				g_hideHud = !g_hideHud;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				stepAway = true;
			}
		}

		// the cursor: over the canvas only the brush's footprint (drawn by DrawCanvas); elsewhere the brush in hand
		if (g_open && g_brush.type >= 0 && !looking) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
			if (!g_overCanvas) {
				DrawBrushPic(ImGui::GetForegroundDrawList(), io.MousePos, Theme::S(150.0f), &g_brush);
			}
		}
		if (stepAway && g_open) {
			StepAway();
		}
		return g_open;
	}
}
