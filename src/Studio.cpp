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
#include "Theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <stb_image.h>  // implementation lives in Dds.cpp

namespace Studio
{
	namespace
	{
		constexpr int W = Art::kPaintingW;
		constexpr int H = Art::kPaintingH;
		constexpr int kUndoDepth = 25;
		constexpr int kMaxMixed = 24;

		struct Pigment
		{
			const char* name;
			float       rgb[3];
		};
		// The paints in the Paints item. Everything else comes from mixing.
		constexpr Pigment kPigments[] = {
			{ "Bonemeal White", { 0.96f, 0.95f, 0.92f } },
			{ "Soot Black", { 0.11f, 0.10f, 0.10f } },
			{ "Dragon's Blood Red", { 0.69f, 0.13f, 0.12f } },
			{ "Madder Rose", { 0.78f, 0.31f, 0.39f } },
			{ "Sun Gold", { 0.93f, 0.77f, 0.16f } },
			{ "Yellow Ochre", { 0.77f, 0.59f, 0.24f } },
			{ "Lapis Blue", { 0.14f, 0.24f, 0.63f } },
			{ "Frost Blue", { 0.12f, 0.27f, 0.43f } },
			{ "Pine Green", { 0.16f, 0.43f, 0.31f } },
			{ "Burnt Sienna", { 0.54f, 0.27f, 0.16f } },
			{ "Raw Umber", { 0.39f, 0.31f, 0.23f } },
			{ "Nightshade Violet", { 0.39f, 0.22f, 0.47f } },
		};

		enum class Tool
		{
			Brush,
			Eraser,
			Fill,
			Picker
		};

		struct Color
		{
			float r = 0, g = 0, b = 0;
			bool  operator==(const Color& o) const { return std::fabs(r - o.r) < 1e-3f && std::fabs(g - o.g) < 1e-3f && std::fabs(b - o.b) < 1e-3f; }
		};

		// ---- state (render thread)
		bool                           g_open = false;
		Items::Easel                   g_easel;
		std::vector<std::uint8_t>      g_px;     // the painting, RGBA8
		std::vector<std::uint8_t>      g_blank;  // bare canvas (what the eraser brings back)
		std::deque<std::vector<std::uint8_t>> g_undo, g_redo;
		std::vector<std::uint8_t>      g_mask;   // coverage of the current stroke (0..255)
		bool                           g_stroking = false;
		ImVec2                         g_last{};
		bool                           g_changed = false;  // anything painted since opening

		ID3D11Texture2D*          g_tex = nullptr;
		ID3D11ShaderResourceView* g_srv = nullptr;
		int                       g_dx0 = W, g_dy0 = H, g_dx1 = 0, g_dy1 = 0;  // dirty rect

		Tool  g_tool = Tool::Brush;
		float g_size = 24.0f;      // brush diameter in canvas pixels
		float g_opacity = 1.0f;
		float g_softness = 0.35f;
		Color g_color{ 0.11f, 0.10f, 0.10f };

		std::vector<Color> g_mixed;      // colours the player mixed (kept in the INI)
		bool               g_mixedLoaded = false;
		std::optional<Color> g_well[2];  // mixing wells
		int                g_armed = -1;  // a well waiting for a swatch click
		float              g_ratio = 0.5f;

		char g_name[64]{};
		bool g_openFinish = false, g_openPackUp = false, g_openClear = false;

		// ------------------------------------------------------------ colour helpers
		float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
		float ToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
		std::uint8_t Byte(float c) { return static_cast<std::uint8_t>(std::clamp(c * 255.0f + 0.5f, 0.0f, 255.0f)); }
		ImVec4       Vec(const Color& c) { return ImVec4(c.r, c.g, c.b, 1.0f); }

		// ------------------------------------------------------------ the bare canvas
		float Hash(int x, int y)
		{
			std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
		}

		// A plain white primed canvas with a fine weave (threads every 3 px, a little uneven).
		// Same look as Data/textures/SkyrimPainter/canvas_blank.dds (tools/textures.py).
		void MakeBlank()
		{
			if (!g_blank.empty()) {
				return;
			}
			g_blank.resize(static_cast<std::size_t>(W) * H * 4);
			std::vector<float> col(W), row(H);
			for (int x = 0; x < W; ++x) {
				col[x] = (Hash(x, 7) - 0.5f) * 3.0f;
			}
			for (int y = 0; y < H; ++y) {
				row[y] = (Hash(13, y) - 0.5f) * 3.0f;
			}
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					const float d = std::sin(x * 2.094f) * std::sin(y * 2.094f) * 3.0f + col[x] + row[y];
					auto*       p = &g_blank[(static_cast<std::size_t>(y) * W + x) * 4];
					p[0] = static_cast<std::uint8_t>(std::clamp(248.0f + d, 0.0f, 255.0f));
					p[1] = static_cast<std::uint8_t>(std::clamp(247.0f + d, 0.0f, 255.0f));
					p[2] = static_cast<std::uint8_t>(std::clamp(244.0f + d, 0.0f, 255.0f));
					p[3] = 255;
				}
			}
		}

		// ------------------------------------------------------------ texture
		void MarkDirty(int x0, int y0, int x1, int y1)
		{
			g_dx0 = (std::min)(g_dx0, (std::max)(0, x0));
			g_dy0 = (std::min)(g_dy0, (std::max)(0, y0));
			g_dx1 = (std::max)(g_dx1, (std::min)(W, x1));
			g_dy1 = (std::max)(g_dy1, (std::min)(H, y1));
		}

		void MarkAll() { MarkDirty(0, 0, W, H); }

		void Upload()
		{
			if (!g_tex || g_dx1 <= g_dx0 || g_dy1 <= g_dy0) {
				return;
			}
			D3D11_BOX box{ static_cast<UINT>(g_dx0), static_cast<UINT>(g_dy0), 0, static_cast<UINT>(g_dx1), static_cast<UINT>(g_dy1), 1 };
			const auto* src = &g_px[(static_cast<std::size_t>(g_dy0) * W + g_dx0) * 4];
			Overlay::Context()->UpdateSubresource(g_tex, 0, &box, src, W * 4, 0);
			g_dx0 = W;
			g_dy0 = H;
			g_dx1 = g_dy1 = 0;
		}

		bool CreateTexture()
		{
			auto* dev = Overlay::Device();
			if (!dev) {
				return false;
			}
			D3D11_TEXTURE2D_DESC d{};
			d.Width = W;
			d.Height = H;
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA sd{ g_px.data(), W * 4, 0 };
			if (FAILED(dev->CreateTexture2D(&d, &sd, &g_tex)) || !g_tex) {
				g_tex = nullptr;
				return false;
			}
			if (FAILED(dev->CreateShaderResourceView(g_tex, nullptr, &g_srv)) || !g_srv) {
				g_tex->Release();
				g_tex = nullptr;
				g_srv = nullptr;
				return false;
			}
			return true;
		}

		// The frame being drawn may still use the texture, so it is freed at the start of the next one (Flush).
		std::vector<IUnknown*> g_release;

		void ReleaseTexture()
		{
			if (g_srv) {
				g_release.push_back(g_srv);
				g_srv = nullptr;
			}
			if (g_tex) {
				g_release.push_back(g_tex);
				g_tex = nullptr;
			}
		}

		// ------------------------------------------------------------ undo
		void PushUndo()
		{
			g_undo.push_back(g_px);
			while (static_cast<int>(g_undo.size()) > kUndoDepth) {
				g_undo.pop_front();
			}
			g_redo.clear();
		}

		void Undo()
		{
			if (g_undo.empty()) {
				return;
			}
			g_redo.push_back(std::move(g_px));
			g_px = std::move(g_undo.back());
			g_undo.pop_back();
			MarkAll();
		}

		void Redo()
		{
			if (g_redo.empty()) {
				return;
			}
			g_undo.push_back(std::move(g_px));
			g_px = std::move(g_redo.back());
			g_redo.pop_back();
			MarkAll();
		}

		// ------------------------------------------------------------ painting
		// One dab of the brush (or eraser) at canvas pixel (cx, cy). The stroke's coverage mask keeps
		// overlapping dabs of one stroke from building up beyond the chosen opacity.
		void Dab(float cx, float cy)
		{
			const float r = (std::max)(0.5f, g_size * 0.5f);
			const int   x0 = (std::max)(0, static_cast<int>(std::floor(cx - r)));
			const int   y0 = (std::max)(0, static_cast<int>(std::floor(cy - r)));
			const int   x1 = (std::min)(W - 1, static_cast<int>(std::ceil(cx + r)));
			const int   y1 = (std::min)(H - 1, static_cast<int>(std::ceil(cy + r)));
			if (x0 > x1 || y0 > y1) {
				return;
			}
			const auto& base = g_undo.back();  // the canvas as it was when the stroke started
			const float hard = 1.0f - g_softness;
			const float cr = g_color.r * 255.0f, cg = g_color.g * 255.0f, cb = g_color.b * 255.0f;
			const bool  erase = g_tool == Tool::Eraser;
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					const float dx = (x + 0.5f - cx) / r, dy = (y + 0.5f - cy) / r;
					const float d = std::sqrt(dx * dx + dy * dy);
					if (d >= 1.0f) {
						continue;
					}
					float fall = 1.0f;
					if (d > hard) {
						const float t = (d - hard) / (std::max)(1e-3f, 1.0f - hard);
						fall = 1.0f - t * t * (3.0f - 2.0f * t);
					}
					const auto   i = static_cast<std::size_t>(y) * W + x;
					const auto   cov = static_cast<std::uint8_t>(std::clamp(g_opacity * fall * 255.0f, 0.0f, 255.0f));
					if (cov <= g_mask[i]) {
						continue;
					}
					g_mask[i] = cov;
					const float  a = cov / 255.0f;
					const auto*  b = &base[i * 4];
					auto*        p = &g_px[i * 4];
					const float  tr = erase ? g_blank[i * 4] : cr, tg = erase ? g_blank[i * 4 + 1] : cg, tb = erase ? g_blank[i * 4 + 2] : cb;
					p[0] = static_cast<std::uint8_t>(b[0] + (tr - b[0]) * a + 0.5f);
					p[1] = static_cast<std::uint8_t>(b[1] + (tg - b[1]) * a + 0.5f);
					p[2] = static_cast<std::uint8_t>(b[2] + (tb - b[2]) * a + 0.5f);
					p[3] = 255;
				}
			}
			MarkDirty(x0, y0, x1 + 1, y1 + 1);
		}

		void StrokeTo(ImVec2 a_to)
		{
			const float dx = a_to.x - g_last.x, dy = a_to.y - g_last.y;
			const float dist = std::sqrt(dx * dx + dy * dy);
			const float step = (std::max)(1.0f, g_size * 0.15f);
			const int   n = (std::max)(1, static_cast<int>(dist / step));
			for (int k = 1; k <= n; ++k) {
				const float t = static_cast<float>(k) / n;
				Dab(g_last.x + dx * t, g_last.y + dy * t);
			}
			g_last = a_to;
		}

		void Fill(int sx, int sy)
		{
			if (sx < 0 || sy < 0 || sx >= W || sy >= H) {
				return;
			}
			PushUndo();
			const auto  seed = static_cast<std::size_t>(sy) * W + sx;
			const int   sr = g_px[seed * 4], sg = g_px[seed * 4 + 1], sb = g_px[seed * 4 + 2];
			const auto  tol = 48;
			auto        same = [&](std::size_t i) {
				const auto* p = &g_px[i * 4];
				return std::abs(p[0] - sr) + std::abs(p[1] - sg) + std::abs(p[2] - sb) <= tol;
			};
			std::vector<std::uint8_t> seen(static_cast<std::size_t>(W) * H, 0);
			std::vector<std::pair<int, int>> stack{ { sx, sy } };
			const float a = g_opacity;
			const float cr = g_color.r * 255.0f, cg = g_color.g * 255.0f, cb = g_color.b * 255.0f;
			std::vector<std::size_t> hits;
			while (!stack.empty()) {
				auto [x, y] = stack.back();
				stack.pop_back();
				// scanline: walk left and right from (x, y)
				int l = x;
				while (l > 0 && !seen[static_cast<std::size_t>(y) * W + l - 1] && same(static_cast<std::size_t>(y) * W + l - 1)) {
					--l;
				}
				int r = x;
				while (r < W - 1 && !seen[static_cast<std::size_t>(y) * W + r + 1] && same(static_cast<std::size_t>(y) * W + r + 1)) {
					++r;
				}
				for (int i = l; i <= r; ++i) {
					const auto idx = static_cast<std::size_t>(y) * W + i;
					if (seen[idx] || !same(idx)) {
						continue;
					}
					seen[idx] = 1;
					hits.push_back(idx);
					if (y > 0 && !seen[idx - W] && same(idx - W)) {
						stack.emplace_back(i, y - 1);
					}
					if (y < H - 1 && !seen[idx + W] && same(idx + W)) {
						stack.emplace_back(i, y + 1);
					}
				}
			}
			for (const auto i : hits) {  // colour after the search, so the test above sees the old colours
				auto* p = &g_px[i * 4];
				p[0] = static_cast<std::uint8_t>(p[0] + (cr - p[0]) * a + 0.5f);
				p[1] = static_cast<std::uint8_t>(p[1] + (cg - p[1]) * a + 0.5f);
				p[2] = static_cast<std::uint8_t>(p[2] + (cb - p[2]) * a + 0.5f);
			}
			MarkAll();
			g_changed = true;
		}

		// ------------------------------------------------------------ mixed colours (INI)
		void LoadMixed()
		{
			if (g_mixedLoaded) {
				return;
			}
			g_mixedLoaded = true;
			char buf[1024]{};
			::GetPrivateProfileStringA("Palette", "sMixed", "", buf, sizeof buf, Overlay::IniPath().c_str());
			std::stringstream ss(buf);
			for (std::string hex; std::getline(ss, hex, ',');) {
				if (hex.size() == 6) {
					const auto v = std::strtoul(hex.c_str(), nullptr, 16);
					g_mixed.push_back({ ((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f });
				}
			}
		}

		void SaveMixed()
		{
			std::string out;
			for (const auto& c : g_mixed) {
				out += fmt::format("{}{:02X}{:02X}{:02X}", out.empty() ? "" : ",", Byte(c.r), Byte(c.g), Byte(c.b));
			}
			::WritePrivateProfileStringA("Palette", "sMixed", out.c_str(), Overlay::IniPath().c_str());
		}

		void AddMixed(const Color& c)
		{
			for (const auto& m : g_mixed) {
				if (Byte(m.r) == Byte(c.r) && Byte(m.g) == Byte(c.g) && Byte(m.b) == Byte(c.b)) {
					return;
				}
			}
			g_mixed.push_back(c);
			while (static_cast<int>(g_mixed.size()) > kMaxMixed) {
				g_mixed.erase(g_mixed.begin());
			}
			SaveMixed();
		}

		Color MixWells()
		{
			if (!g_well[0] || !g_well[1]) {
				return g_well[0] ? *g_well[0] : (g_well[1] ? *g_well[1] : g_color);
			}
			const float a[3]{ g_well[0]->r, g_well[0]->g, g_well[0]->b };
			const float b[3]{ g_well[1]->r, g_well[1]->g, g_well[1]->b };
			float       o[3];
			Mix(a, b, g_ratio, o);
			return { o[0], o[1], o[2] };
		}

		// ------------------------------------------------------------ UI pieces
		// A clickable swatch; also a drag source (ImGui's colour payload). Returns true when clicked.
		bool Swatch(const char* a_id, const Color& a_c, float a_size, const char* a_tip, bool a_selected)
		{
			ImGui::PushID(a_id);
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const bool   clicked = ImGui::ColorButton("##sw", Vec(a_c), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(a_size, a_size));
			if (ImGui::IsItemHovered() && a_tip) {
				ImGui::SetTooltip("%s", a_tip);
			}
			auto* dl = ImGui::GetWindowDrawList();
			if (a_selected) {
				dl->AddRect(pos - ImVec2(3, 3), pos + ImVec2(a_size + 3, a_size + 3), Theme::Gold(), 4.0f, 0, Theme::S(2.0f));
			}
			ImGui::PopID();
			return clicked;
		}

		void SetColorFromSwatch(const Color& c)
		{
			if (g_armed >= 0) {
				g_well[g_armed] = c;
				g_armed = -1;
			} else {
				g_color = c;
				if (g_tool == Tool::Eraser || g_tool == Tool::Picker) {
					g_tool = Tool::Brush;
				}
			}
		}

		void DrawTools(float a_w)
		{
			Theme::SectionLabel("Tools");
			struct T
			{
				const char* label;
				Tool        tool;
				const char* tip;
			};
			const T tools[] = { { "Brush", Tool::Brush, "Paint (B)" }, { "Eraser", Tool::Eraser, "Rub back to bare canvas (E)" },
				{ "Fill", Tool::Fill, "Flood an area with paint (F)" }, { "Pick", Tool::Picker, "Take a colour from the canvas (G)" } };
			for (int i = 0; i < 4; ++i) {
				if (i % 2) {
					ImGui::SameLine();
				}
				if (Theme::Chip(tools[i].label, g_tool == tools[i].tool)) {
					g_tool = tools[i].tool;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", tools[i].tip);
				}
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			Theme::SectionLabel("Brush");
			Theme::SliderFloat("Size", &g_size, 1.0f, 160.0f, "%.0f px", a_w);
			float op = g_opacity * 100.0f;
			if (Theme::SliderFloat("Opacity", &op, 5.0f, 100.0f, "%.0f%%", a_w)) {
				g_opacity = op / 100.0f;
			}
			float soft = g_softness * 100.0f;
			if (Theme::SliderFloat("Softness", &soft, 0.0f, 100.0f, "%.0f%%", a_w)) {
				g_softness = soft / 100.0f;
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			Theme::SectionLabel("Current colour");
			ImGui::ColorButton("##current", Vec(g_color), ImGuiColorEditFlags_NoTooltip, ImVec2(a_w, Theme::S(40)));
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			Theme::SectionLabel("History");
			ImGui::BeginDisabled(g_undo.empty());
			if (Theme::Button("Undo", ImVec2((a_w - Theme::S(8)) * 0.5f, 0))) {
				Undo();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(g_redo.empty());
			if (Theme::Button("Redo", ImVec2((a_w - Theme::S(8)) * 0.5f, 0))) {
				Redo();
			}
			ImGui::EndDisabled();
			if (Theme::Button("Clear canvas", ImVec2(a_w, 0))) {
				g_openClear = true;
			}
			ImGui::Dummy(ImVec2(0, Theme::S(10)));
			Theme::TextColoredWrapped(Theme::InkSoft(), "Wheel over the canvas: brush size.  Ctrl+Z / Ctrl+Y: undo / redo.  Esc: step away.");
		}

		void DrawWell(int a_i, float a_size)
		{
			ImGui::PushID(a_i);
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			bool         clicked = false;
			if (g_well[a_i]) {
				clicked = ImGui::ColorButton("##well", Vec(*g_well[a_i]), ImGuiColorEditFlags_NoTooltip, ImVec2(a_size, a_size));
			} else {
				clicked = ImGui::InvisibleButton("##well", ImVec2(a_size, a_size));
				auto* dl = ImGui::GetWindowDrawList();
				dl->AddRect(pos, pos + ImVec2(a_size, a_size), Theme::InkSoft(), 6.0f, 0, Theme::S(1.5f));
				const char*  plus = "+";
				const ImVec2 ts = ImGui::CalcTextSize(plus);
				dl->AddText(pos + (ImVec2(a_size, a_size) - ts) * 0.5f, Theme::InkSoft(), plus);
			}
			if (ImGui::BeginDragDropTarget()) {  // drop a swatch on a well
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_4F)) {
					const float* c = static_cast<const float*>(p->Data);
					g_well[a_i] = Color{ c[0], c[1], c[2] };
					g_armed = -1;
				}
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_3F)) {
					const float* c = static_cast<const float*>(p->Data);
					g_well[a_i] = Color{ c[0], c[1], c[2] };
					g_armed = -1;
				}
				ImGui::EndDragDropTarget();
			}
			if (clicked) {
				g_armed = g_armed == a_i ? -1 : a_i;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Click, then click a colour - or drag a colour here. Right-click empties it.");
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
				g_well[a_i].reset();
			}
			if (g_armed == a_i) {
				const float t = static_cast<float>(ImGui::GetTime());
				const auto  col = IM_COL32(230, 190, 90, static_cast<int>(150 + 100 * std::sin(t * 6.0f)));
				ImGui::GetWindowDrawList()->AddRect(pos - ImVec2(3, 3), pos + ImVec2(a_size + 3, a_size + 3), col, 6.0f, 0, Theme::S(2.5f));
			}
			ImGui::PopID();
		}

		void DrawPalette(float a_w)
		{
			const float sw = Theme::S(30.0f);
			const float gap = ImGui::GetStyle().ItemSpacing.x;
			const int   perRow = (std::max)(1, static_cast<int>((a_w + gap) / (sw + gap)));

			Theme::SectionLabel("Paints");
			for (int i = 0; i < static_cast<int>(std::size(kPigments)); ++i) {
				if (i % perRow) {
					ImGui::SameLine();
				}
				const Color c{ kPigments[i].rgb[0], kPigments[i].rgb[1], kPigments[i].rgb[2] };
				if (Swatch(kPigments[i].name, c, sw, kPigments[i].name, c == g_color)) {
					SetColorFromSwatch(c);
				}
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			Theme::SectionLabel("Mixed on the palette");
			if (g_mixed.empty()) {
				Theme::TextColoredWrapped(Theme::InkSoft(), "Nothing mixed yet - mix two colours below.");
			}
			int remove = -1;
			for (int i = 0; i < static_cast<int>(g_mixed.size()); ++i) {
				if (i % perRow) {
					ImGui::SameLine();
				}
				const auto tip = fmt::format("#{:02X}{:02X}{:02X}  (right-click to scrape off)", Byte(g_mixed[i].r), Byte(g_mixed[i].g), Byte(g_mixed[i].b));
				if (Swatch(fmt::format("mix{}", i).c_str(), g_mixed[i], sw, tip.c_str(), g_mixed[i] == g_color)) {
					SetColorFromSwatch(g_mixed[i]);
				}
				if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
					remove = i;
				}
			}
			if (remove >= 0) {
				g_mixed.erase(g_mixed.begin() + remove);
				SaveMixed();
			}

			ImGui::Dummy(ImVec2(0, Theme::S(10)));
			Theme::SectionLabel("Mixing");
			const float well = Theme::S(54.0f);
			DrawWell(0, well);
			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::Dummy(ImVec2(0, well * 0.5f - ImGui::GetTextLineHeight() * 0.5f));
			ImGui::TextUnformatted("+");
			ImGui::EndGroup();
			ImGui::SameLine();
			DrawWell(1, well);
			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::Dummy(ImVec2(0, well * 0.5f - ImGui::GetTextLineHeight() * 0.5f));
			ImGui::TextUnformatted("=");
			ImGui::EndGroup();
			ImGui::SameLine();
			const Color result = MixWells();
			ImGui::ColorButton("##result", Vec(result), ImGuiColorEditFlags_NoTooltip, ImVec2(well, well));
			const bool both = g_well[0] && g_well[1];
			ImGui::BeginDisabled(!both);
			float share = g_ratio * 100.0f;
			if (Theme::SliderFloat("Second colour", &share, 5.0f, 95.0f, "%.0f%%", a_w)) {
				g_ratio = share / 100.0f;
			}
			if (Theme::Button("Use", ImVec2((a_w - Theme::S(8)) * 0.5f, 0))) {
				g_color = result;
				if (g_tool != Tool::Brush && g_tool != Tool::Fill) {
					g_tool = Tool::Brush;
				}
			}
			ImGui::SameLine();
			if (Theme::Button("Add to palette", ImVec2((a_w - Theme::S(8)) * 0.5f, 0), true)) {
				AddMixed(result);
				g_color = result;
			}
			ImGui::EndDisabled();
			if (!both) {
				Theme::TextColoredWrapped(Theme::InkSoft(), "Put a colour in each well to mix them.");
			}
		}

		// The canvas: shows the painting fitted into a_size and handles painting with the mouse.
		void DrawCanvas(const ImVec2& a_size)
		{
			const float  scale = (std::min)(a_size.x / W, a_size.y / H);
			const ImVec2 size(W * scale, H * scale);
			const ImVec2 origin = ImGui::GetCursorScreenPos() + (a_size - size) * 0.5f;
			auto*        dl = ImGui::GetWindowDrawList();
			// easel-like shadow behind the canvas
			dl->AddRectFilled(origin + ImVec2(Theme::S(6), Theme::S(8)), origin + size + ImVec2(Theme::S(6), Theme::S(8)), IM_COL32(0, 0, 0, 90), 2.0f);
			ImGui::SetCursorScreenPos(origin);
			ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft);
			const bool hovered = ImGui::IsItemHovered();
			const bool active = ImGui::IsItemActive();
			dl->AddImage(reinterpret_cast<ImTextureID>(g_srv), origin, origin + size);
			dl->AddRect(origin, origin + size, IM_COL32(60, 44, 30, 255), 0.0f, 0, Theme::S(2.0f));

			const auto&  io = ImGui::GetIO();
			const ImVec2 m = (io.MousePos - origin) / scale;  // canvas pixels
			if (hovered && io.MouseWheel != 0.0f) {
				g_size = std::clamp(g_size * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 1.0f, 160.0f);
			}
			const bool painting = g_tool == Tool::Brush || g_tool == Tool::Eraser;
			if (painting) {
				if (ImGui::IsItemActivated()) {
					PushUndo();
					g_mask.assign(static_cast<std::size_t>(W) * H, 0);
					g_stroking = true;
					g_last = m;
					Dab(m.x, m.y);
					g_changed = true;
				} else if (active && g_stroking) {
					StrokeTo(m);
				}
				if (g_stroking && !active) {
					g_stroking = false;
				}
			} else if (ImGui::IsItemActivated()) {
				const int px = static_cast<int>(m.x), py = static_cast<int>(m.y);
				if (g_tool == Tool::Fill) {
					Fill(px, py);
				} else if (px >= 0 && py >= 0 && px < W && py < H) {
					const auto* p = &g_px[(static_cast<std::size_t>(py) * W + px) * 4];
					g_color = { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f };
					g_tool = Tool::Brush;
				}
			}
			if (hovered || active) {
				ImGui::SetMouseCursor(ImGuiMouseCursor_None);
				const ImVec2 c = io.MousePos;
				if (painting) {
					const float r = (std::max)(1.5f, g_size * 0.5f * scale);
					dl->AddCircle(c, r + 1.0f, IM_COL32(0, 0, 0, 160), 0, 1.5f);
					dl->AddCircle(c, r, IM_COL32(255, 255, 255, 200), 0, 1.0f);
				} else {
					dl->AddCircle(c, Theme::S(6), IM_COL32(255, 255, 255, 220), 0, 1.5f);
					dl->AddLine(c - ImVec2(Theme::S(10), 0), c + ImVec2(Theme::S(10), 0), IM_COL32(0, 0, 0, 200), 1.0f);
					dl->AddLine(c - ImVec2(0, Theme::S(10)), c + ImVec2(0, Theme::S(10)), IM_COL32(0, 0, 0, 200), 1.0f);
				}
			}
		}

		void Shortcuts()
		{
			if (ImGui::GetIO().WantTextInput) {
				return;
			}
			const bool ctrl = ImGui::GetIO().KeyCtrl;
			if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
				Undo();
			} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
				Redo();
			} else if (!ctrl) {
				if (ImGui::IsKeyPressed(ImGuiKey_B, false)) g_tool = Tool::Brush;
				if (ImGui::IsKeyPressed(ImGuiKey_E, false)) g_tool = Tool::Eraser;
				if (ImGui::IsKeyPressed(ImGuiKey_F, false)) g_tool = Tool::Fill;
				if (ImGui::IsKeyPressed(ImGuiKey_G, false)) g_tool = Tool::Picker;
				if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) g_size = (std::max)(1.0f, g_size / 1.15f);
				if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) g_size = (std::min)(160.0f, g_size * 1.15f);
			}
		}

		void Close()
		{
			ReleaseTexture();
			g_open = false;
			g_undo.clear();
			g_redo.clear();
			g_mask.clear();
			g_mask.shrink_to_fit();
			g_px.clear();
			g_px.shrink_to_fit();
			g_well[0].reset();
			g_well[1].reset();
			g_armed = -1;
			g_stroking = false;
		}

		void StepAway()
		{
			if (g_changed) {
				Items::StepAway(g_easel.ref, g_px);
			}
			Close();
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

	void Mix(const float a_a[3], const float a_b[3], float a_t, float a_out[3])
	{
		// Kubelka-Munk style: each paint absorbs (K) and scatters (S) light; mixing averages K/S,
		// so blue + yellow gives green like real paint instead of the grey of mixed light.
		auto ks = [](float r) {
			r = std::clamp(r, 0.002f, 1.0f);
			return (1.0f - r) * (1.0f - r) / (2.0f * r);
		};
		auto refl = [](float k) { return 1.0f + k - std::sqrt(k * k + 2.0f * k); };
		const float t = std::clamp(a_t, 0.0f, 1.0f);
		for (int i = 0; i < 3; ++i) {
			const float la = ToLinear(a_a[i]), lb = ToLinear(a_b[i]);
			const float km = refl((1.0f - t) * ks(la) + t * ks(lb));
			const float avg = (1.0f - t) * la + t * lb;
			a_out[i] = std::clamp(ToSrgb(km * 0.8f + avg * 0.2f), 0.0f, 1.0f);  // a little light mixing keeps tints from going muddy
		}
	}

	void Flush()
	{
		for (auto* p : g_release) {
			p->Release();
		}
		g_release.clear();
	}

	bool Open(const Items::Easel& a_easel)
	{
		Close();
		MakeBlank();
		LoadMixed();
		g_easel = a_easel;
		g_px = g_blank;
		if (!a_easel.wipPng.empty()) {
			int        w = 0, h = 0, n = 0;
			const auto path = a_easel.wipPng;
			if (auto* px = stbi_load(path.c_str(), &w, &h, &n, 4)) {
				if (w == W && h == H) {
					std::memcpy(g_px.data(), px, g_px.size());
				} else {
					for (int y = 0; y < H; ++y) {  // nearest-neighbour fit (only if the size ever changes)
						for (int x = 0; x < W; ++x) {
							std::memcpy(&g_px[(static_cast<std::size_t>(y) * W + x) * 4], &px[(static_cast<std::size_t>(y * h / H) * w + x * w / W) * 4], 4);
						}
					}
				}
				stbi_image_free(px);
				spdlog::info("Studio: continuing {}", path);
			} else {
				spdlog::warn("Studio: could not read {} ({}), starting on a blank canvas", path, stbi_failure_reason() ? stbi_failure_reason() : "?");
			}
		}
		if (!CreateTexture()) {
			spdlog::error("Studio: could not create the canvas texture");
			Close();
			return false;
		}
		g_changed = false;
		g_name[0] = 0;
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
		const ImVec2 size(screen.x * 0.94f, screen.y * 0.92f);
		ImGui::SetNextWindowPos((screen - size) * 0.5f, ImGuiCond_Always);
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		Theme::SetBackdrop();
		bool closeBox = false;
		if (Theme::BeginWindow("##studio", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
			closeBox = Theme::Header("Studio", "Paint on the canvas - mix new colours on your palette");
			const float side = Theme::S(250.0f), right = Theme::S(300.0f);
			const float footer = ImGui::GetFrameHeight() + Theme::S(18.0f);
			const float h = ImGui::GetContentRegionAvail().y - footer;
			const float gap = ImGui::GetStyle().ItemSpacing.x;

			if (Theme::BeginParchment("##tools", ImVec2(side, h), true)) {
				DrawTools(ImGui::GetContentRegionAvail().x);
			}
			Theme::EndParchment();
			ImGui::SameLine();
			const float centerW = ImGui::GetContentRegionAvail().x - right - gap;
			ImGui::BeginChild("##canvasarea", ImVec2(centerW, h), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			DrawCanvas(ImGui::GetContentRegionAvail());
			ImGui::EndChild();
			ImGui::SameLine();
			if (Theme::BeginParchment("##palette", ImVec2(right, h), true)) {
				DrawPalette(ImGui::GetContentRegionAvail().x);
			}
			Theme::EndParchment();

			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			const float bw = Theme::S(200.0f);
			if (Theme::Button("Step away", ImVec2(bw, 0))) {
				closeBox = true;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Leave the painting on the easel and come back to it later");
			}
			ImGui::SameLine();
			if (Theme::Button("Pack up", ImVec2(bw, 0))) {
				g_openPackUp = true;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Fold the easel and take the canvas back (the painting is scraped off)");
			}
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
			if (Theme::Button("Finish painting", ImVec2(bw, 0), true)) {
				g_openFinish = true;
			}

			// ---- popups
			if (g_openClear) {
				ImGui::OpenPopup("##clear");
				g_openClear = false;
			}
			if (ConfirmPopup("##clear", "Scrape the whole canvas clean? (Undo can bring it back.)", "Clear")) {
				PushUndo();
				g_px = g_blank;
				MarkAll();
			}
			if (g_openPackUp) {
				ImGui::OpenPopup("##packup");
				g_openPackUp = false;
			}
			if (ConfirmPopup("##packup", "Pack up the easel and canvas? The painting on it is scraped off.", "Pack up")) {
				Items::PackUp(g_easel.ref);
				Close();
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
				bool done = false;
				if (Theme::Button("Finish", ImVec2(Theme::S(200), 0), true) || enter) {
					done = true;
				}
				ImGui::SameLine();
				if (Theme::Button("Keep painting", ImVec2(Theme::S(200), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
					ImGui::CloseCurrentPopup();
				}
				if (done) {
					ImGui::CloseCurrentPopup();
					Items::Finish(g_easel.ref, g_px, g_name);
					Close();
				}
				ImGui::EndPopup();
			}

			if (g_open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
				Shortcuts();
				if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
					closeBox = true;
				}
			}
		}
		Theme::EndWindow();
		if (closeBox && g_open) {
			StepAway();
		}
		return g_open;
	}
}
