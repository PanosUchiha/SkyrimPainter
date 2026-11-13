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

#include "Live.h"

#include "CanvasGeom.h"

#include <chrono>
#include <cmath>
#include <optional>

namespace Live
{
	namespace
	{
		constexpr const char* kCanvasShape = "SPCanvas";

		// The game's texture we lent our picture to, and what it had before.
		struct Patch
		{
			std::uint32_t                         canvas = 0;
			RE::NiPointer<RE::NiSourceTexture>    tex;  // kept alive while it shows ours
			REX::W32::ID3D11Resource*             oldRes = nullptr;
			REX::W32::ID3D11ShaderResourceView*   oldSrv = nullptr;
			ID3D11Texture2D*                      ourTex = nullptr;  // one reference each, held by us
			ID3D11ShaderResourceView*             ourSrv = nullptr;
		};
		std::optional<Patch>                  g_patch;
		bool                                  g_detaching = false;
		std::chrono::steady_clock::time_point g_detachBy;
		bool                                  g_wasThirdPerson = false;
		bool                                  g_viewChanged = false;

		RE::TESObjectREFR* Canvas(std::uint32_t a_id)
		{
			auto* ref = a_id ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id) : nullptr;
			return ref && !ref->IsDisabled() && !ref->IsDeleted() ? ref : nullptr;
		}

		RE::BSGeometry* CanvasShape(RE::TESObjectREFR* a_ref)
		{
			auto* root = a_ref ? a_ref->Get3D() : nullptr;
			auto* obj = root ? root->GetObjectByName(RE::BSFixedString(kCanvasShape)) : nullptr;
			return obj ? obj->AsGeometry() : nullptr;
		}

		RE::BSLightingShaderMaterialBase* Material(RE::BSGeometry* a_geom)
		{
			auto* shader = a_geom ? netimmerse_cast<RE::BSLightingShaderProperty*>(a_geom->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
			return shader && shader->material ? static_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
		}

		bool SamePath(const char* a_a, const std::string& a_b)
		{
			if (!a_a) {
				return false;
			}
			// the game may keep it with or without "textures\" in front
			std::string a = a_a, b = a_b;
			auto        strip = [](std::string& s) {
                std::replace(s.begin(), s.end(), '/', '\\');
                if (s.size() > 9 && _strnicmp(s.c_str(), "textures\\", 9) == 0) {
                    s.erase(0, 9);
                }
			};
			strip(a);
			strip(b);
			return _stricmp(a.c_str(), b.c_str()) == 0;
		}

		void Restore()
		{
			if (!g_patch) {
				return;
			}
			auto& p = *g_patch;
			if (p.tex && p.tex->rendererTexture) {
				auto* rt = p.tex->rendererTexture;
				if (rt->resourceView == reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(p.ourSrv)) {
					rt->resourceView = p.oldSrv;
					rt->texture = p.oldRes;
				}
			}
			if (p.ourSrv) {
				p.ourSrv->Release();
			}
			if (p.ourTex) {
				p.ourTex->Release();
			}
			spdlog::info("Live: canvas {:08X} shows its own texture again", p.canvas);
			g_patch.reset();
			g_detaching = false;
		}

		RE::NiPoint3 Corner(const RE::NiTransform& a_world, bool a_portrait, int a_i)
		{
			const auto& c = a_portrait ? CanvasGeom::kPortrait[a_i] : CanvasGeom::kLandscape[a_i];
			return a_world * RE::NiPoint3(c[0], c[1], c[2]);
		}
	}

	bool CanvasOnScreen(std::uint32_t a_canvas, bool a_portrait, float a_screenW, float a_screenH, float a_out[4][2])
	{
		auto* camera = RE::Main::WorldRootCamera();
		auto* geom = CanvasShape(Canvas(a_canvas));
		if (!camera || !geom) {
			return false;
		}
		const RE::NiTransform world = geom->world;
		for (int i = 0; i < 4; ++i) {
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (!camera->WorldPtToScreenPt3(Corner(world, a_portrait, i), x, y, z, 1e-5f) || z < 0.0f) {
				return false;  // behind the camera
			}
			a_out[i][0] = x * a_screenW;
			a_out[i][1] = (1.0f - y) * a_screenH;
		}
		return true;
	}

	void Attach(std::uint32_t a_canvas, const std::string& a_dds, ID3D11Texture2D* a_tex, ID3D11ShaderResourceView* a_srv)
	{
		if (!a_tex || !a_srv || a_dds.empty()) {
			return;
		}
		if (g_detaching) {
			Restore();
		}
		auto* geom = CanvasShape(Canvas(a_canvas));
		auto* material = Material(geom);
		auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
		if (g_patch) {
			// still ours? The game rebuilds the canvas's 3D now and then (it may load the texture anew)
			if (g_patch->canvas == a_canvas && diffuse == g_patch->tex.get() && diffuse && diffuse->rendererTexture &&
				diffuse->rendererTexture->resourceView == reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(g_patch->ourSrv)) {
				return;
			}
			Restore();
		}
		if (!diffuse || !diffuse->rendererTexture || !diffuse->rendererTexture->resourceView || !material->textureSet) {
			return;  // not loaded yet
		}
		if (!SamePath(material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse), a_dds)) {
			return;  // the canvas isn't showing the session's texture (yet): never touch any other
		}
		Patch p;
		p.canvas = a_canvas;
		p.tex.reset(diffuse);
		auto* rt = diffuse->rendererTexture;
		p.oldRes = rt->texture;
		p.oldSrv = rt->resourceView;
		a_tex->AddRef();
		a_srv->AddRef();
		p.ourTex = a_tex;
		p.ourSrv = a_srv;
		rt->texture = reinterpret_cast<REX::W32::ID3D11Resource*>(a_tex);
		rt->resourceView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(a_srv);
		g_patch = std::move(p);
		spdlog::info("Live: painting straight onto canvas {:08X}", a_canvas);
	}

	bool Attached() { return g_patch.has_value() && !g_detaching; }

	void Detach(bool a_wait)
	{
		if (!g_patch) {
			return;
		}
		if (!a_wait) {
			Restore();
			return;
		}
		g_detaching = true;
		g_detachBy = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	}

	void Tick()
	{
		if (!g_patch || !g_detaching) {
			return;
		}
		auto* geom = CanvasShape(Canvas(g_patch->canvas));
		auto* material = Material(geom);
		auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
		// the canvas has moved on to its new texture (or is gone): ours can go
		if (!geom || diffuse != g_patch->tex.get() || std::chrono::steady_clock::now() > g_detachBy) {
			Restore();
		}
	}

	void FaceCanvas(std::uint32_t a_canvas, bool a_portrait)
	{
		SKSE::GetTaskInterface()->AddTask([a_canvas, a_portrait] {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cam = RE::PlayerCamera::GetSingleton();
			auto* geom = CanvasShape(Canvas(a_canvas));
			if (!player || !cam || !geom) {
				return;
			}
			g_wasThirdPerson = !cam->IsInFirstPerson();
			g_viewChanged = true;
			if (g_wasThirdPerson) {
				cam->ForceFirstPerson();
			}
			// look at the middle of the canvas
			RE::NiPoint3 centre;
			for (int i = 0; i < 4; ++i) {
				centre += Corner(geom->world, a_portrait, i);
			}
			centre *= 0.25f;
			const RE::NiPoint3 eye = player->GetLookingAtLocation();
			const RE::NiPoint3 d = centre - eye;
			const float        flat = std::sqrt(d.x * d.x + d.y * d.y);
			if (flat < 1.0f) {
				return;
			}
			player->SetHeading(std::atan2(d.x, d.y));
			player->SetLooking(std::clamp(-std::atan2(d.z, flat), -1.2f, 1.2f));
		});
	}

	void RestoreView()
	{
		SKSE::GetTaskInterface()->AddTask([] {
			if (!g_viewChanged) {
				return;
			}
			g_viewChanged = false;
			if (g_wasThirdPerson) {
				if (auto* cam = RE::PlayerCamera::GetSingleton()) {
					cam->ForceThirdPerson();
				}
			}
		});
	}
}
