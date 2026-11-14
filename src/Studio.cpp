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
#include "Live.h"
#include "Overlay.h"
#include "Paint.h"
#include "PaletteSpots.h"
#include "Spectral.h"
#include "Store.h"
#include "Surface.h"
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
		ImVec2                    g_dir{ 1.0f, 0.0f };         // smoothed stroke direction, canvas pixels (for the cursor)
		ImVec2                    g_lastCanvas{ -1e9f, -1e9f };
		// painting on the canvas in the world (the default) or in the flat close-up
		bool                      g_closeUp = false;
		std::string               g_liveDds;          // the canvas's own texture for this session
		double                    g_openedAt = 0.0;
		bool                      g_liveWarned = false;
		bool                      g_overCanvas = false;
		bool                      g_hideHud = false;  // Tab: only the canvas stays up
		bool                      g_guide = false;    // the painter's guide is open (F1)
		int                       g_placeFrames = 3;     // > 0: the panels are put in their places (a few frames, while they size themselves)
		bool                      g_canvasPlace = true;  // the close-up window is put in its place the next time it shows
		float                     g_ui = 0.85f;          // HUD size ([Studio] fHudSize)
		// the hand (Krita: pressure, opacity, smoothing) and the view (zoom, pan, mirror)
		float                     g_body = 1.0f;    // paint thickness: thin glaze .. full body
		bool                      g_rag = false;    // the rag is in hand (lifts paint off the canvas)
		Paint::Brush              g_ragHand;        // stands in for a brush while the rag is used
		Paint::Stroker            g_stroker;
		std::vector<Paint::StrokePoint> g_points;
		float                     g_zoom = 1.0f;    // 1 = the whole canvas fits
		ImVec2                    g_pan{ 0.0f, 0.0f };
		bool                      g_mirror = false;
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
		float U(float a_v) { return Theme::S(a_v) * g_ui; }  // sizes in the panels follow the HUD size

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
				doc.layers.push_back({ l.name, l.visible, l.opacity, l.px, static_cast<int>(l.blend), l.lockAlpha });
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

		void Close(bool a_saving = false)
		{
			// the canvas in the world keeps showing the paint until the saved picture takes over
			Live::Detach(a_saving);
			if (g_open) {
				Live::RestoreView();
			}
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
			const bool save = g_paint.Changed();
			if (save) {
				SaveAndThen(false, {});
			}
			Close(save);
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
		Paint::Brush& Held() { return g_rag ? g_ragHand : g_brush; }

		void PutDownBrush()
		{
			if (g_brush.type >= 0) {
				g_rack[g_brush.type] = g_brush;  // put down with whatever paint is on it
			}
			g_brush = Paint::Brush{};
		}

		void ToggleRag()
		{
			if (g_rag) {
				g_rag = false;
			} else {
				PutDownBrush();
				g_rag = true;
				Hint("The rag lifts paint off the canvas - press harder to take more");
			}
		}

		ImVec2 ToScreen(const Surface& a_s, float a_x, float a_y)
		{
			ImVec2 o;
			a_s.ToScreen(a_x, a_y, o.x, o.y);
			return o;
		}
		ImVec2 ToCanvas(const Surface& a_s, const ImVec2& a_p)
		{
			ImVec2 o;
			a_s.ToCanvas(a_p.x, a_p.y, o.x, o.y);
			return o;
		}

		// The brush's footprint as an outline, laid on the canvas (so in the world it is seen in
		// perspective): a circle for the round brushes, an ellipse / flat oval for the filbert and the
		// flats, turned with the stroke; the rag is a soft round wad.
		void DrawFootprint(ImDrawList* a_dl, const Surface& a_s, const ImVec2& a_c)
		{
			float rA, rL;
			if (g_rag) {
				rA = rL = 24.0f * (0.6f + 0.4f * g_pressure);
			} else if (g_brush.type >= 0) {
				const auto& bt = Paint::kBrushes[g_brush.type];
				rA = bt.radius * Paint::kSizes[std::clamp(g_brush.size, 0, Paint::kSizeCount - 1)] * (0.45f + 0.55f * g_pressure);  // across the stroke
				rL = rA / bt.aspect;                                                                                                   // along it
			} else {
				return;
			}
			const float minPx = 1.5f / std::max(0.01f, a_s.ScaleAt(a_c.x, a_c.y));
			rA = std::max(rA, minPx);
			rL = std::max(rL, minPx);
			constexpr int kN = 40;
			ImVec2        pts[kN];
			for (int i = 0; i < kN; ++i) {
				const float t = 6.2831853f * i / kN;
				const float along = std::cos(t) * rL, across = std::sin(t) * rA;
				pts[i] = ToScreen(a_s, a_c.x + g_dir.x * along - g_dir.y * across, a_c.y + g_dir.y * along + g_dir.x * across);
			}
			a_dl->AddPolyline(pts, kN, IM_COL32(0, 0, 0, 120), ImDrawFlags_Closed, 2.4f);
			ImU32 col = IM_COL32(255, 255, 255, 150);  // clean: it will blend
			if (g_rag) {
				col = IM_COL32(225, 214, 190, 200);
			} else if (g_brush.Loaded()) {
				col = Col(g_brush.color, 230);
			}
			a_dl->AddPolyline(pts, kN, col, ImDrawFlags_Closed, 1.3f);
		}

		// Painting on a surface (the canvas in the world, or the close-up). a_canStart: a click here may
		// start a stroke (the mouse is over this surface and nothing else); a_hovered: show the cursor.
		void PaintOn(const Surface& a_s, bool a_canStart, bool a_hovered)
		{
			const auto&  io = ImGui::GetIO();
			const float  W = static_cast<float>(g_paint.Width()), H = static_cast<float>(g_paint.Height());
			const ImVec2 m = ToCanvas(a_s, io.MousePos);
			const bool   inside = m.x >= 0.0f && m.y >= 0.0f && m.x < W && m.y < H;

			// which way the brush is travelling across the canvas (the flat brushes lie across it)
			if (g_lastCanvas.x > -1e8f) {
				const ImVec2 d = m - g_lastCanvas;
				const float  len = std::sqrt(d.x * d.x + d.y * d.y);
				if (len > 0.75f) {
					g_dir = g_dir * 0.75f + d / len * 0.25f;
					const float l2 = std::sqrt(g_dir.x * g_dir.x + g_dir.y * g_dir.y);
					g_dir = l2 > 0.01f ? g_dir / l2 : ImVec2(1.0f, 0.0f);
				}
			}
			g_lastCanvas = m;

			const double now = ImGui::GetTime();
			if (!g_painting && a_canStart && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				if (io.KeyCtrl && !g_rag) {
					// dip the brush into the paint on the canvas (the colour picker)
					if (g_brush.type < 0) {
						Hint("Pick up a brush first: click one below the palette");
					} else {
						g_brush.color = g_paint.ColorAt(static_cast<int>(m.x), static_cast<int>(m.y));
						g_brush.load = 1.0f;
					}
				} else if (!g_rag && g_brush.type < 0) {
					Hint("Pick up a brush first: click one below the palette");
				} else {
					// painting on a hidden layer would leave no mark you can see: show it again
					if (const int cur = g_paint.Current(); !g_paint.Layers()[cur].visible) {
						g_paint.SetLayerVisible(cur, true);
						static std::string msg;
						msg = fmt::format("\"{}\" was hidden - it's showing again", g_paint.Layers()[cur].name);
						Hint(msg.c_str());
					}
					Paint::StrokeStyle style;
					style.body = g_body;
					style.lift = g_rag;
					const auto& bt = Paint::kBrushes[std::max(0, g_brush.type)];
					// the string is measured on screen, so it feels the same near or far
					g_stroker.string = 0.0f;  // the brush is exactly where the mouse is
					g_stroker.taper = g_rag ? 8.0f : bt.radius * Paint::kSizes[std::clamp(g_brush.size, 0, Paint::kSizeCount - 1)] * 1.5f;
					g_stroker.Begin(m.x, m.y, now);
					g_paint.BeginStroke(Held(), m.x, m.y, 0.3f * g_pressure, style);
					g_painting = true;
				}
			}
			if (g_painting) {
				g_points.clear();
				g_stroker.Move(m.x, m.y, now, g_pressure, g_points);
				const bool up = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
				if (up) {
					g_stroker.Finish(g_points);  // lift off: the stroke tapers away
				}
				for (const auto& pt : g_points) {
					g_paint.StrokeTo(Held(), pt.x, pt.y, pt.pressure);
				}
				if (up) {
					g_paint.EndStroke(Held());
					g_painting = false;
				}
			}

			// the cursor: the brush's footprint, right at the mouse
			g_overCanvas = g_painting || (a_hovered && inside && !Overlay::Looking());
			if (g_overCanvas) {
				DrawFootprint(ImGui::GetForegroundDrawList(), a_s, m);
			}
		}

		// The canvas in the world: found on screen from its corners each frame, painted where the mouse is.
		void DrawWorldCanvas(bool a_looking)
		{
			const auto& io = ImGui::GetIO();
			float       q[4][2];
			Surface     s;
			if (g_work.canvas && Live::CanvasOnScreen(g_work.canvas, g_work.portrait, io.DisplaySize.x, io.DisplaySize.y, q)) {
				s = Surface::FromQuad(q, static_cast<float>(g_paint.Width()), static_cast<float>(g_paint.Height()));
			}
			// a window over the whole screen, behind the panels, to catch the mouse over the canvas
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
			ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
			ImGui::Begin("##world", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::SetCursorScreenPos(ImVec2(0, 0));
			ImGui::InvisibleButton("##worldcanvas", io.DisplaySize, ImGuiButtonFlags_MouseButtonLeft);
			// (the press makes this button active on the same frame - hovering must still count then)
			const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
			                     ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			if (s.ok) {
				if (hovered && io.MouseWheel != 0.0f) {
					g_pressure = std::clamp(g_pressure + 0.05f * io.MouseWheel, 0.05f, 1.0f);
				}
				PaintOn(s, hovered && !a_looking, hovered);
			} else {
				if (g_painting) {  // the canvas went out of view mid-stroke
					g_paint.EndStroke(Held());
					g_painting = false;
				}
				const char*  msg = "Turn to the canvas (hold the right mouse button), or press C for a close-up";
				const ImVec2 ts = ImGui::CalcTextSize(msg);
				ImGui::GetWindowDrawList()->AddText(ImVec2((io.DisplaySize.x - ts.x) * 0.5f, io.DisplaySize.y * 0.12f), IM_COL32(240, 228, 200, 220), msg);
			}
			ImGui::End();
		}

		// The flat close-up: fitted into a_size, then zoomed / panned / mirrored. Middle button or Space +
		// left button pans, Ctrl + wheel zooms, the wheel alone sets the touch.
		void DrawCanvas(const ImVec2& a_size)
		{
			const float  W = static_cast<float>(g_paint.Width()), H = static_cast<float>(g_paint.Height());
			const ImVec2 r0 = ImGui::GetCursorScreenPos();
			const ImVec2 r1 = r0 + a_size;
			const float  fit = std::min(a_size.x / W, a_size.y / H);
			const float  scale = fit * g_zoom;
			const ImVec2 size(W * scale, H * scale);
			const ImVec2 origin = r0 + a_size * 0.5f + g_pan - size * 0.5f;
			auto*        dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(r0, r1, true);
			dl->AddRectFilled(origin + ImVec2(Theme::S(5), Theme::S(6)), origin + size + ImVec2(Theme::S(5), Theme::S(6)), IM_COL32(0, 0, 0, 70), 2.0f);
			dl->AddImage(reinterpret_cast<ImTextureID>(g_srv), origin, origin + size, g_mirror ? ImVec2(1, 0) : ImVec2(0, 0), g_mirror ? ImVec2(0, 1) : ImVec2(1, 1));
			dl->AddRect(origin, origin + size, IM_COL32(60, 44, 30, 255), 0.0f, 0, Theme::S(2.0f));
			dl->PopClipRect();

			ImGui::SetCursorScreenPos(r0);
			ImGui::InvisibleButton("##canvas", a_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
			const bool  hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			const bool  active = ImGui::IsItemActive();
			const auto& io = ImGui::GetIO();

			const bool panning = active && !g_painting && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsKeyDown(ImGuiKey_Space));
			if (panning) {
				g_pan = g_pan + io.MouseDelta;
			}
			if (hovered && io.MouseWheel != 0.0f) {
				if (io.KeyCtrl) {  // zoom about the point under the cursor
					const float  nz = std::clamp(g_zoom * std::pow(1.15f, io.MouseWheel), 0.5f, 8.0f);
					const ImVec2 mid = r0 + a_size * 0.5f;
					const ImVec2 rel = (io.MousePos - (mid + g_pan)) * (nz / g_zoom);
					g_pan = io.MousePos - mid - rel;
					g_zoom = nz;
				} else {
					g_pressure = std::clamp(g_pressure + 0.05f * io.MouseWheel, 0.05f, 1.0f);
				}
			}
			const Surface s = Surface::FromRect(origin.x, origin.y, scale, W, g_mirror);
			PaintOn(s, hovered && !ImGui::IsKeyDown(ImGuiKey_Space) && !Overlay::Looking(), hovered && !panning);
		}

		void DrawHand(float a_w)
		{
			Theme::SectionLabel("Your hand");
			const float lw = U(92.0f);
			auto        row = [&](const char* a_label, const char* a_id, float* a_v, float a_min, float a_max, const char* a_fmt, const char* a_tip) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(a_label);
                ImGui::SameLine(lw);
                float pc = *a_v * 100.0f;
                if (Theme::SliderFloat(a_id, &pc, a_min, a_max, a_fmt, a_w - lw)) {
                    *a_v = pc / 100.0f;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", a_tip);
                }
			};
			row("Touch", "##touch", &g_pressure, 5.0f, 100.0f, "%.0f%%",
				"Light: thin, broken strokes that only catch the canvas weave. Heavy: the bristles splay and lay the paint down thick.\nThe mouse wheel over the canvas changes it too.");
			row("Paint", "##body", &g_body, 10.0f, 100.0f, "%.0f%%",
				"How thick the paint is. Thin: a glaze you can see through, however much you go over it in one stroke. Full: covers what's under it.");
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
			const float blobR = U(17.0f);
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
				dl->AddCircle(p0 + ImVec2(s.x * a_w, s.y * ph), blobR + 3.0f, Theme::Gold(), 0, U(2.0f));
				ImGui::SetTooltip("%s", s.mixed ? "Your mix (Ctrl + click to scrape it off)" : kPigments[std::min(hoverSpot, PaletteSpots::kCount - 1)].name);
			}
			if (click && overPalette) {
				if (hoverSpot >= 0 && io.KeyCtrl && g_spots[hoverSpot].mixed) {
					g_spots.erase(g_spots.begin() + hoverSpot);
					SaveSpots();
				} else if (hoverSpot >= 0) {
				if (g_rag) {
					Hint("Put the rag down and pick up a brush to load paint");
				} else if (g_brush.type < 0) {
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
			ImGui::Dummy(ImVec2(0, U(6)));
			Theme::SectionLabel("Brushes");
			const float  cell = (a_w - U(70.0f)) / Paint::kBrushCount;
			const float  rowH = U(96.0f);
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
						g_rag = false;
						const bool same = g_brush.type == i;
						PutDownBrush();
						if (!same) {
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
			const ImVec2 r0 = b0 + ImVec2(cell * Paint::kBrushCount + U(6), U(14));
			const ImVec2 r1 = r0 + ImVec2(U(60), rowH - U(28));
			dl->AddRectFilled(r0, r1, IM_COL32(205, 196, 176, 255), U(8));
			for (int k = 1; k < 4; ++k) {
				dl->AddLine(r0 + ImVec2(0, (r1.y - r0.y) * k / 4), r0 + ImVec2(r1.x - r0.x, (r1.y - r0.y) * k / 4 + U(4)), IM_COL32(160, 150, 130, 255), 1.0f);
			}
			dl->AddText(ImVec2(r0.x + U(14), r1.y + U(14)), Theme::InkSoft(), "Rag");
			if (ImGui::IsMouseHoveringRect(r0, r1)) {
				ImGui::SetTooltip("%s", g_brush.type >= 0 ? "Click: wipe the brush clean (a clean brush blends the wet paint)"
				                  : g_rag              ? "Click: put the rag down"
				                                       : "Click: take the rag - it lifts paint off the canvas (E)");
				if (click) {
					if (g_brush.type >= 0) {
						g_brush.load = 0.0f;
						g_brush.color = {};
					} else {
						ToggleRag();
					}
				}
			}
			if (g_rag) {
				dl->AddRect(r0 - ImVec2(3, 3), r1 + ImVec2(3, 3), Theme::GoldDim(), U(8), 0, 1.0f);
			}

			// ---- what's in hand
			ImGui::Dummy(ImVec2(0, U(4)));
			if (g_rag) {
				Theme::TextColoredWrapped(Theme::InkSoft(), "The rag in hand - wipe paint off the canvas.");
			} else if (g_brush.type < 0) {
				Theme::TextColoredWrapped(Theme::InkSoft(), "No brush in hand.");
			} else {
				const ImVec2 sw = ImGui::GetCursorScreenPos();
				if (g_brush.Loaded()) {
					dl->AddRectFilled(sw, sw + ImVec2(U(22), U(22)), Col(g_brush.color), 4.0f);
				} else {
					dl->AddRect(sw, sw + ImVec2(U(22), U(22)), Theme::InkSoft(), 4.0f);
				}
				ImGui::Dummy(ImVec2(U(26), U(22)));
				ImGui::SameLine();
				static constexpr const char* kSizeNames[Paint::kSizeCount] = { "very small", "small", "", "large", "very large" };
				const char* sz = kSizeNames[std::clamp(g_brush.size, 0, Paint::kSizeCount - 1)];
				ImGui::Text("%s%s%s - %s", sz, *sz ? " " : "", Paint::kBrushes[g_brush.type].name, g_brush.Loaded() ? "loaded" : "clean, blends");
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("[ and ] take a smaller or larger brush of the same kind");
				}
			}
			ImGui::Dummy(ImVec2(0, U(6)));
			DrawHand(a_w);
			ImGui::Dummy(ImVec2(0, U(4)));
			Theme::TextColoredWrapped(Theme::InkSoft(), "F1: how everything works");
		}

		// ---- the painter's guide: everything the studio can do, in the studio
		struct GuideLine
		{
			const char* what;  // a key or an action ("" = a plain paragraph)
			const char* text;
		};

		void GuideTab(const char* a_name, const char* a_intro, std::initializer_list<GuideLine> a_lines)
		{
			if (!ImGui::BeginTabItem(a_name)) {
				return;
			}
			ImGui::Dummy(ImVec2(0, Theme::S(4)));
			if (a_intro && *a_intro) {
				Theme::TextColoredWrapped(Theme::InkSoft(), a_intro);
				ImGui::Dummy(ImVec2(0, Theme::S(6)));
			}
			const float col = Theme::S(150.0f);
			const float wrap = ImGui::GetContentRegionAvail().x;
			for (const auto& l : a_lines) {
				if (!*l.what) {
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
					ImGui::TextUnformatted(l.text);
					ImGui::PopTextWrapPos();
				} else {
					ImGui::TextColored(ImVec4(0.92f, 0.80f, 0.52f, 1.0f), "%s", l.what);
					ImGui::SameLine(col);
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap - col);
					ImGui::TextUnformatted(l.text);
					ImGui::PopTextWrapPos();
				}
				ImGui::Dummy(ImVec2(0, Theme::S(3)));
			}
			ImGui::EndTabItem();
		}

		void DrawGuide(bool a_looking)
		{
			if (!g_guide) {
				return;
			}
			if (a_looking) {
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f);
			}
			const ImVec2 screen = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(screen * 0.5f, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(Theme::S(640.0f), Theme::S(520.0f)), ImGuiCond_Appearing);
			if (Theme::BeginWindow("##guide", nullptr, a_looking ? ImGuiWindowFlags_NoInputs : 0)) {
				ImGui::TextColored(ImVec4(0.92f, 0.85f, 0.68f, 1.0f), "The Painter's Guide");
				ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - Theme::S(90.0f));
				if (Theme::Button("Close", ImVec2(Theme::S(90.0f), 0))) {
					g_guide = false;
				}
				ImGui::Separator();
				ImGui::BeginChild("##guidebody", ImVec2(0, 0), false);
				if (ImGui::BeginTabBar("##guidetabs")) {
					GuideTab("Starting", "Paint the way a painter does: from the back to the front. Block in the big shapes first, then paint the middle and finish with the details on top.",
						{
							{ "Pick up a brush", "Click one of the brushes below the palette. Click it again to put it down." },
							{ "Load it", "Click a paint on the palette. Click a second paint to mix it into what's on the brush." },
							{ "Paint", "Drag on the canvas standing on the easel with the left mouse button - the paint goes straight onto it. Press C for a flat close-up instead." },
							{ "Keep a mix", "With paint on the brush, click a bare spot on the palette to put the mix down there. Ctrl + click a mix to scrape it off." },
							{ "Clean the brush", "Click the rag." },
							{ "Finish", "Finish painting names it and takes it off the easel as a painting you can hang. Step away (Esc) leaves it on the easel for later." },
						});
					GuideTab("Brushes", "Each brush leaves its own mark. The outline on the canvas shows where it will touch and how big it is.",
						{
							{ "Detail round", "Fine lines, edges, highlights, signatures." },
							{ "Round", "General painting and soft shapes." },
							{ "Filbert", "Rounded flat: blending, petals, leaves, skin." },
							{ "Wide flat", "Skies, washes, large areas. It lies across the way you move it." },
							{ "[  and  ]", "A smaller or larger brush of the same kind." },
							{ "Never runs out", "A loaded brush keeps its paint until you wipe it on the rag. Each brush keeps its own paint when you put it down." },
							{ "Dirty brush", "Dragging through wet paint picks some of it up, so colours shift as you work, like the real thing." },
						});
					GuideTab("Your hand", "The two sliders under the palette.",
						{
							{ "Touch", "How hard you press. Light: thin, broken strokes that only catch the weave of the canvas. Heavy: the bristles splay and lay the paint down thick. The mouse wheel over the canvas changes it too." },
							{ "Paint", "How thick the paint is. Thin is a glaze you can see through, however much you go over it in one stroke. Full covers what's under it." },
							{ "Strokes", "Every stroke starts light and comes up to full pressure. A quick flick lays down less paint." },
						});
					GuideTab("Blending", "Wet paint on the canvas mixes the way pigments do: blue and yellow make green.",
						{
							{ "Wet into wet", "Paint over wet paint and the two mix on the canvas." },
							{ "Blend", "Wipe the brush on the rag, then drag the clean brush through the paint to soften and smear it." },
							{ "Lift paint", "Take the rag (click it with an empty hand, or press E) and rub the canvas to wipe paint off. Press harder to take more." },
							{ "Take a colour", "Ctrl + click on the canvas dips the brush into the colour there." },
						});
					GuideTab("Seeing", "You paint on the canvas on the easel, with the world around it: paint what's in front of you.",
						{
							{ "Right mouse (hold)", "Look around: the mouse turns your view instead of moving the cursor. Turn back to the canvas to paint on it." },
							{ "Tab", "Hide the panel." },
							{ "C / Close-up", "A flat close-up of the canvas in a window, for fine detail. Press C again to paint on the easel." },
							{ "Move the close-up", "Drag its top bar. Drag its bottom corner to make it bigger or smaller." },
							{ "Ctrl + wheel", "Zoom in and out where the cursor is." },
							{ "Middle mouse / Space", "Hold and drag to move around a zoomed-in canvas." },
							{ "M", "In the close-up: see the painting in a mirror. Painters do this to catch mistakes in the drawing." },
							{ "Step back / Ctrl+0", "The whole canvas again, the right way round." },
							{ "Reset", "Puts the panel and the close-up back where they started." },
						});
					GuideTab("Keys", "",
						{
							{ "Left mouse", "Pick up, load, paint" },
							{ "Right mouse (hold)", "Look around" },
							{ "Mouse wheel", "Touch (pressure)" },
							{ "Ctrl + click", "Take the colour from the canvas" },
							{ "Ctrl + wheel", "Zoom (close-up)" },
							{ "Middle / Space + drag", "Move the canvas (close-up)" },
							{ "[  ]", "Brush size" },
							{ "E", "Take / put down the rag" },
							{ "C", "Close-up / paint on the easel" },
							{ "M", "Mirror (close-up)" },
							{ "Ctrl+Z / Ctrl+Y", "Undo / redo" },
							{ "Ctrl+0", "Whole canvas" },
							{ "Tab", "Hide the panel" },
							{ "F1", "This guide" },
							{ "Esc", "Close the guide, or step away from the easel" },
						});
					ImGui::EndTabBar();
				}
				ImGui::EndChild();
			}
			Theme::EndWindow();
			if (a_looking) {
				ImGui::PopStyleVar();
			}
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
		Live::Tick();
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
					layers.push_back({ l.name, l.visible, l.opacity, std::move(l.px), static_cast<Paint::Blend>(l.blend), l.lockAlpha });
				}
				// carry on with the top layer that is showing (painting on a hidden one shows nothing)
				int top = static_cast<int>(layers.size()) - 1;
				while (top > 0 && !layers[top].visible) {
					--top;
				}
				g_paint.SetCurrent(top);
				g_paint.RecompositeAll();
				g_paint.MarkSaved();
				// one layer of paint now: a painting made with layers is merged (hidden ones too, nothing is lost)
				if (g_paint.Flatten()) {
					spdlog::info("Studio: merged {} layer(s) into one", doc->layers.size());
				}
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
		g_placeFrames = 3;  // portrait and landscape lay out differently
		g_canvasPlace = true;
		{
			char buf[32]{};
			::GetPrivateProfileStringA("Studio", "fHudSize", "0.85", buf, sizeof buf, Overlay::IniPath().c_str());
			g_ui = std::clamp(static_cast<float>(std::atof(buf)), 0.6f, 1.2f);
		}
		g_hideHud = false;
		g_lastCanvas = ImVec2(-1e9f, -1e9f);
		g_closeUp = false;
		g_openedAt = ImGui::GetTime();
		g_liveWarned = false;
		// paint straight onto the canvas on the easel: it gets a texture of its own for this session,
		// which the studio's picture is then lent to (Live)
		g_liveDds.clear();
		if (a_work.canvas) {
			g_liveDds = fmt::format("textures\\SkyrimPainter\\InProgress\\{}_{}.dds", Art::ToUtf8(a_work.dir.filename()), Art::NewId());
			Items::ShowLive(a_work.easel, g_liveDds, g_paint.Composite(), W, H);
			Live::FaceCanvas(a_work.canvas, a_work.portrait);
		} else {
			g_closeUp = true;
		}
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

		// the canvas on the easel shows the studio's picture as it is painted
		if (!g_liveDds.empty()) {
			Live::Attach(g_work.canvas, g_liveDds, g_tex, g_srv);
			if (!Live::Attached() && !g_liveWarned && ImGui::GetTime() - g_openedAt > 6.0) {
				g_liveWarned = true;  // couldn't hook up: the close-up still works
				spdlog::warn("Studio: could not paint on the canvas in the world - using the close-up");
				g_closeUp = true;
				Hint("The canvas in the world couldn't be painted on - painting in the close-up");
			}
		}
		if (!g_closeUp) {
			DrawWorldCanvas(looking);
			if (ImGui::GetTime() < g_hintUntil) {  // (the close-up shows hints in its title bar)
				auto*        fg = ImGui::GetForegroundDrawList();
				const ImVec2 ts = ImGui::CalcTextSize(g_hint.c_str());
				const ImVec2 p((screen.x - ts.x) * 0.5f, screen.y * 0.06f);
				const float  pad = Theme::S(10.0f);
				fg->AddRectFilled(p - ImVec2(pad, pad * 0.6f), p + ts + ImVec2(pad, pad * 0.6f), IM_COL32(10, 9, 8, 190), Theme::S(6.0f));
				fg->AddText(p, IM_COL32(242, 222, 170, 255), g_hint.c_str());
			}
		}

		// No backdrop: the world stays in view so you can paint what's in front of you. The canvas sits on
		// the left (drag its top bar to move it, the corner to resize), the palette and the tools on the right.
		// panels keep wherever you put them; it is put in its place on opening and by Reset
		const ImGuiCond cond = g_placeFrames > 0 ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
		const float     panelW = U(330.0f), margin = Theme::S(14.0f);
		if (g_closeUp) {
			const float aspect = static_cast<float>(g_paint.Width()) / static_cast<float>(g_paint.Height());
			const float bar = ImGui::GetFrameHeight() + Theme::S(8.0f);
			float       ch = screen.y * 0.70f, cw = ch * aspect;
			const float maxW = screen.x - panelW - margin * 3.0f;
			if (cw > maxW) {
				cw = maxW;
				ch = cw / aspect;
			}
			const ImGuiCond ccond = (g_placeFrames > 0 || g_canvasPlace) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
			g_canvasPlace = false;
			ImGui::SetNextWindowPos(ImVec2(margin, (screen.y - ch - bar) * 0.45f), ccond);
			ImGui::SetNextWindowSize(ImVec2(cw + Theme::S(16.0f), ch + bar + Theme::S(16.0f)), ccond);
			ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::S(240.0f), Theme::S(200.0f)), screen);
			ImGui::SetNextWindowBgAlpha(looking ? 0.0f : 0.35f);
			if (ImGui::Begin("##easelcanvas", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
				// the top bar: grab it to move the canvas
				const std::string title = g_work.slot >= 0 ? fmt::format("\"{}\"", g_work.name) : std::string("A fresh canvas");
				ImGui::TextColored(ImVec4(0.92f, 0.85f, 0.68f, looking ? 0.4f : 1.0f), "%s", title.c_str());
				if (g_mirror || std::fabs(g_zoom - 1.0f) > 0.01f || g_pan.x != 0.0f || g_pan.y != 0.0f) {
					ImGui::SameLine();
					const std::string view = fmt::format("{}{}", g_mirror ? "  [in the mirror]" : "", std::fabs(g_zoom - 1.0f) > 0.01f ? fmt::format("  x{:.1f}", g_zoom) : std::string());
					ImGui::TextColored(ImVec4(0.75f, 0.72f, 0.65f, 1.0f), "%s", view.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton("Step back")) {
						g_zoom = 1.0f;
						g_pan = ImVec2(0.0f, 0.0f);
						g_mirror = false;
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("See the whole canvas again, the right way round (Ctrl+0)");
					}
				}
				if (ImGui::GetTime() < g_hintUntil) {
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.55f, 1.0f), "  %s", g_hint.c_str());
				}
				DrawCanvas(ImGui::GetContentRegionAvail());
			}
			ImGui::End();
		}

		if (!g_hideHud) {
			// While you look around (right mouse held) the panels stay where they are, faded and out of
			// the way of the mouse - they never close, so nothing moves or resets.
			const int faded = looking ? ImGuiWindowFlags_NoInputs : 0;
			if (looking) {
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f);
			}
			// one panel on the right edge: the palette, brushes and rag, your hand, then the history, finishing and the view
			ImGui::SetNextWindowPos(ImVec2(screen.x - margin, screen.y * 0.5f), cond, ImVec2(1.0f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f), ImGuiCond_Always);
			if (Theme::BeginWindow("##studiopanel", nullptr, faded)) {
				ImGui::SetWindowFontScale(g_ui);
				const float w = ImGui::GetContentRegionAvail().x;
				const float sp = ImGui::GetStyle().ItemSpacing.x;
				const float w2 = (w - sp) * 0.5f, w3 = (w - sp * 2.0f) / 3.0f;
				DrawPalettePanel(w);

				ImGui::Dummy(ImVec2(0, U(4)));
				ImGui::Separator();
				ImGui::Dummy(ImVec2(0, U(2)));
				ImGui::BeginDisabled(!g_paint.CanUndo());
				if (Theme::Button("Undo", ImVec2(w3, 0))) {
					g_paint.Undo();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(!g_paint.CanRedo());
				if (Theme::Button("Redo", ImVec2(w3, 0))) {
					g_paint.Redo();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (Theme::Button("Scrape", ImVec2(w3, 0))) {
					g_openClear = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Scrape all the paint off the canvas");
				}

				if (Theme::Button("Finish painting", ImVec2(w2, 0), true)) {
					g_openFinish = true;
				}
				ImGui::SameLine();
				if (Theme::Button("Step away", ImVec2(w2, 0))) {
					stepAway = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Leave the painting on the easel and come back to it later (Esc)");
				}

				if (Theme::Button(g_closeUp ? "Easel" : "Close-up", ImVec2(w3, 0))) {
					g_closeUp = !g_closeUp;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", g_closeUp ? "Paint straight onto the canvas standing on the easel (C)" : "A flat close-up of the canvas, to zoom in on details or see it in a mirror (C)");
				}
				ImGui::SameLine();
				if (Theme::Button(g_guide ? "Close guide" : "Guide", ImVec2(w3, 0))) {
					g_guide = !g_guide;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("How everything works (F1)");
				}
				ImGui::SameLine();
				if (Theme::Button("Reset", ImVec2(w3, 0))) {
					g_placeFrames = 3;
					g_canvasPlace = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Put the panel (and the close-up) back in place");
				}
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("HUD size");
				ImGui::SameLine();
				float size = g_ui * 100.0f;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::SliderFloat("##hudsize", &size, 60.0f, 120.0f, "%.0f%%")) {
					g_ui = std::clamp(size / 100.0f, 0.6f, 1.2f);
				}
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					::WritePrivateProfileStringA("Studio", "fHudSize", fmt::format("{:.2f}", g_ui).c_str(), Overlay::IniPath().c_str());
					g_placeFrames = 3;  // the panel changes size: put it back against the edge
				}
			}
			Theme::EndWindow();
			if (looking) {
				ImGui::PopStyleVar();
			}
		}
		if (g_placeFrames > 0) {
			--g_placeFrames;
		}

		DrawGuide(looking);

		// ---- popups
		if (g_openClear) {
			ImGui::OpenPopup("##clear");
			g_openClear = false;
		}
		if (ConfirmPopup("##clear", "Scrape all the paint off the canvas? (Undo can bring it back.)", "Scrape")) {
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
				Close(true);
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
			if (!g_painting) {
				if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket) && g_brush.type >= 0) {
					g_brush.size = std::max(0, g_brush.size - 1);
				}
				if (ImGui::IsKeyPressed(ImGuiKey_RightBracket) && g_brush.type >= 0) {
					g_brush.size = std::min(Paint::kSizeCount - 1, g_brush.size + 1);
				}
				if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
					ToggleRag();
				}
				if (ImGui::IsKeyPressed(ImGuiKey_M, false) && g_closeUp) {
					g_mirror = !g_mirror;
				}
				if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.KeyCtrl) {
					g_closeUp = !g_closeUp;
				}
				if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) {
					g_zoom = 1.0f;
					g_pan = ImVec2(0.0f, 0.0f);
				}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
				g_guide = !g_guide;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				if (g_guide) {
					g_guide = false;  // Esc closes the guide first
				} else {
					stepAway = true;
				}
			}
		}

		// the cursor: over the canvas the brush's footprint takes its place; anywhere else it is the normal cursor
		if (g_open && g_overCanvas && (g_brush.type >= 0 || g_rag) && !looking) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		}
		if (stepAway && g_open) {
			StepAway();
		}
		return g_open;
	}
}
