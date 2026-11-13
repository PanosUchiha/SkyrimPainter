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

#include "Wall.h"

#include "Art.h"
#include "Forms.h"
#include "Items.h"

namespace fs = std::filesystem;

namespace Wall
{
	namespace
	{
		constexpr std::uint32_t kRecordHung = 'HUNG';
		constexpr std::uint32_t kRecordPreview = 'PREV';
		constexpr float         kDefaultOffset = 0.3f;
		constexpr float         kMinOffset = -40.0f;  // wall collision often sits in front of the visible wall
		constexpr float         kMaxOffset = 12.0f;
		constexpr float         kMinScale = 0.3f;
		constexpr float         kMaxScale = 3.0f;
		constexpr float         kReach = 2000.0f;    // how far a painting can be hung
		constexpr float         kExamineReach = 600.0f;

		std::mutex                                g_mtx;  // guards the state below
		std::unordered_map<RE::FormID, Painting>  g_hung;
		std::array<RE::FormID, Forms::kFrameCount> g_frameForms{};

		struct Placement
		{
			Painting            painting;
			RE::ObjectRefHandle moving;
			float               scale = 1.0f;
			float               offset = kDefaultOffset;
		};
		std::optional<Placement> g_placement;
		std::atomic<bool>        g_placing{ false };
		std::atomic<bool>        g_placeQueued{ false };
		std::optional<MenuInfo>  g_activated;

		int StyleOf(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return -1;
			}
			const auto id = a_base->GetFormID();
			for (int i = 0; i < Forms::kFrameCount; ++i) {
				if (g_frameForms[i] && g_frameForms[i] == id) {
					return i;
				}
			}
			return -1;
		}

		RE::TESObjectACTI* FrameForm(int a_style)
		{
			const auto id = g_frameForms[std::clamp(a_style, 0, Forms::kFrameCount - 1)];
			return id ? RE::TESForm::LookupByID<RE::TESObjectACTI>(id) : nullptr;
		}

		void DeleteRef(RE::TESObjectREFR* a_ref, const char* a_why)
		{
			if (!a_ref) {
				return;
			}
			spdlog::info("Painting {:08X} {}", a_ref->GetFormID(), a_why);
			Art::Forget(a_ref->GetFormID());
			a_ref->Disable();
			a_ref->SetDelete(true);
		}

		void SetRefScale(RE::TESObjectREFR* a_ref, float a_scale)
		{
			a_ref->GetReferenceRuntimeData().refScale = static_cast<std::uint16_t>(std::lround(a_scale * 100.0f));
			a_ref->AddChange(1u << 4);  // CHANGE_REFR_SCALE - kept in the save
			if (auto* root = a_ref->Get3D()) {
				root->local.scale = a_scale;
				RE::NiUpdateData ud{};
				root->Update(ud);
			}
		}

		// ------------------------------------------------------------------ aiming
		struct Hit
		{
			RE::NiPoint3       pos;
			RE::NiPoint3       normal;
			RE::TESObjectREFR* ref = nullptr;
		};

		std::optional<Hit> RaycastFromCamera(float a_maxDistance)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* camera = RE::PlayerCamera::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world || !camera || !camera->cameraRoot) {
				return std::nullopt;
			}
			const auto&        xf = camera->cameraRoot->world;
			const RE::NiPoint3 from = xf.translate;
			RE::NiPoint3       dir{ xf.rotate.entry[0][1], xf.rotate.entry[1][1], xf.rotate.entry[2][1] };
			const float        len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
			if (len < 1e-4f) {
				return std::nullopt;
			}
			dir = { dir.x / len, dir.y / len, dir.z / len };
			const RE::NiPoint3 p = player->GetPosition();
			const float        camToPlayer = std::sqrt((p.x - from.x) * (p.x - from.x) + (p.y - from.y) * (p.y - from.y));
			const float        dist = a_maxDistance + camToPlayer;
			const RE::NiPoint3 to{ from.x + dir.x * dist, from.y + dir.y * dist, from.z + dir.z * dist };

			const float     s = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4(from.x * s, from.y * s, from.z * s, 0.0f);
			pick.rayInput.to = RE::hkVector4(to.x * s, to.y * s, to.z * s, 0.0f);
			RE::CFilter filter{};
			player->GetCollisionFilterInfo(filter);
			pick.rayInput.filterInfo.filter = (filter.filter & 0xFFFF0000) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
			world->PickObject(pick);
			if (!pick.rayOutput.HasHit()) {
				return std::nullopt;
			}
			alignas(16) float n[4];
			_mm_store_ps(n, pick.rayOutput.normal.quad);
			const float f = pick.rayOutput.hitFraction;
			Hit         out;
			out.pos = { from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, from.z + (to.z - from.z) * f };
			out.normal = { n[0], n[1], n[2] };
			if (pick.rayOutput.rootCollidable) {
				out.ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
			}
			return out;
		}

		struct Pose
		{
			RE::NiPoint3 pos;
			float        nx = 0.0f, ny = 1.0f;
		};

		std::optional<Pose> ComputePose(const char*& a_whyNot, float a_offset)
		{
			const auto hit = RaycastFromCamera(kReach);
			if (!hit) {
				a_whyNot = "Nothing in reach - aim at a wall closer to you";
				return std::nullopt;
			}
			if (hit->ref && hit->ref->Is(RE::FormType::ActorCharacter)) {
				a_whyNot = "You can't hang a painting on someone";
				return std::nullopt;
			}
			const auto& nrm = hit->normal;
			const float nlen = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y);
			if (std::fabs(nrm.z) > 0.6f || nlen < 0.3f) {
				a_whyNot = "That's not a wall - aim at a vertical surface";
				return std::nullopt;
			}
			Pose pose;
			pose.nx = nrm.x / nlen;
			pose.ny = nrm.y / nlen;
			pose.pos = { hit->pos.x + pose.nx * a_offset, hit->pos.y + pose.ny * a_offset, hit->pos.z };
			return pose;
		}

		float AngleZ(const Pose& a_pose) { return std::atan2(a_pose.nx, a_pose.ny); }  // local +Y (front) along the wall normal

		// ------------------------------------------------------------------ preview (main thread)
		RE::ObjectRefHandle g_preview;
		bool                g_previewReady = false;
		bool                g_previewTextured = false;
		bool                g_previewVisible = false;
		std::atomic<bool>   g_previewQueued{ false };
		RE::TESObjectCELL*  g_placeCell = nullptr;
		RE::TESWorldSpace*  g_placeWorld = nullptr;
		std::atomic<bool>   g_placeCellReset{ true };
		RE::FormID          g_stalePreview = 0;

		void DeletePreview()
		{
			if (auto ref = g_preview.get()) {
				DeleteRef(ref.get(), "(preview) removed");
			}
			g_preview = {};
			g_previewReady = g_previewTextured = g_previewVisible = false;
		}

		void CreatePreview(const Placement& a_pl)
		{
			DeletePreview();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* base = FrameForm(a_pl.painting.style);
			if (!player || !base) {
				return;
			}
			const float        h = player->GetAngleZ();
			const RE::NiPoint3 p = player->GetPosition();
			const RE::NiPoint3 at{ p.x + std::sin(h) * 150.0f, p.y + std::cos(h) * 150.0f, p.z + 100.0f };
			const RE::NiPoint3 angle{ 0.0f, 0.0f, h + 3.14159265f };
			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				base, at, angle, player->GetParentCell(), player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), false, true);
			if (auto old = a_pl.moving.get(); old && old->Get3D()) {
				old->Get3D()->SetAppCulled(true);  // the original disappears while it is moved
			}
			if (auto ref = handle.get()) {
				ref->SetCollision(false);
				g_preview = handle;
				spdlog::info("Preview painting {:08X} ({})", ref->GetFormID(), Forms::kFrameNames[a_pl.painting.style]);
			}
		}

		void UpdatePreview()
		{
			g_previewQueued = false;
			std::optional<Placement> pl;
			{
				std::lock_guard lk(g_mtx);
				pl = g_placement;
			}
			if (!pl) {
				DeletePreview();
				return;
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				auto* cell = player->GetParentCell();
				auto* world = player->GetWorldspace();
				if (g_placeCellReset.exchange(false) || !g_placeCell) {
					g_placeCell = cell;
					g_placeWorld = world;
				} else if (cell && cell != g_placeCell && (cell->IsInteriorCell() || g_placeCell->IsInteriorCell() || world != g_placeWorld)) {
					spdlog::info("Hanging cancelled: the player changed cells");
					DeletePreview();
					CancelPlacement(false);
					Art::Notify("Hanging cancelled - you left the area");
					return;
				}
			}
			auto ref = g_preview.get();
			if (!ref || ref->GetBaseObject() != FrameForm(pl->painting.style)) {
				CreatePreview(*pl);
				return;
			}
			auto* root = ref->Get3D();
			if (!root) {
				return;
			}
			if (!g_previewReady) {
				root->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);
				g_previewReady = true;
			}
			if (!g_previewTextured) {
				const auto dds = DdsFor(pl->painting.id);
				if (Art::IsReady(dds) && Art::ApplyNow(ref.get(), dds)) {
					g_previewTextured = true;
				}
			}
			const char* why = nullptr;
			const auto  pose = ComputePose(why, pl->offset);
			if (!pose) {
				if (g_previewVisible) {
					root->SetAppCulled(true);
					g_previewVisible = false;
				}
				return;
			}
			ref->data.location = pose->pos;
			ref->data.angle = { 0.0f, 0.0f, AngleZ(*pose) };
			ref->Update3DPosition(true);
			if (std::fabs(root->local.scale - pl->scale) > 0.001f) {
				SetRefScale(ref.get(), pl->scale);
			}
			if (!g_previewVisible) {
				root->SetAppCulled(false);
				g_previewVisible = true;
			}
		}

		void DoPlace()
		{
			g_placeQueued = false;
			std::optional<Placement> pl;
			{
				std::lock_guard lk(g_mtx);
				pl = g_placement;
			}
			if (!pl) {
				return;
			}
			const char* why = nullptr;
			const auto  pose = ComputePose(why, pl->offset);
			if (!pose) {
				Art::Notify(why ? why : "Can't hang the painting here");
				return;
			}
			auto* base = FrameForm(pl->painting.style);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!base || !player) {
				Art::Notify("SkyrimPainter: frame record missing - is SkyrimPainter.esp enabled?");
				return;
			}
			const RE::NiPoint3 angle{ 0.0f, 0.0f, AngleZ(*pose) };
			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				base, pose->pos, angle, player->GetParentCell(), player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), true, true);
			auto ref = handle.get();
			if (!ref) {
				Art::Notify("SkyrimPainter: could not hang the painting here");
				return;
			}
			if (std::fabs(pl->scale - 1.0f) > 0.001f) {
				SetRefScale(ref.get(), pl->scale);
			}
			{
				std::lock_guard lk(g_mtx);
				g_hung[ref->GetFormID()] = pl->painting;
			}
			const auto dds = DdsFor(pl->painting.id);
			Art::EnsureFromPng(dds, Art::FromUtf8(pl->painting.png), Art::kPaintingW, Art::kPaintingH);
			Art::Show(ref->GetFormID(), dds);
			spdlog::info("Hung painting \"{}\" as {:08X} ({}, {:.0f}%)", pl->painting.name, ref->GetFormID(), Forms::kFrameNames[pl->painting.style], pl->scale * 100.0f);
			if (auto old = pl->moving.get()) {
				{
					std::lock_guard lk(g_mtx);
					g_hung.erase(old->GetFormID());
				}
				DeleteRef(old.get(), "moved away");
			}
			DeletePreview();
			{
				std::lock_guard lk(g_mtx);
				g_placement.reset();
			}
			g_placing = false;
			Art::Notify(fmt::format("\"{}\" hung", pl->painting.name));
		}
	}

	std::string DdsFor(const std::string& a_id)
	{
		return "textures\\SkyrimPainter\\Paintings\\" + a_id + ".dds";
	}

	bool Init()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		int   found = 0;
		for (int i = 0; i < Forms::kFrameCount; ++i) {
			auto* form = dh ? dh->LookupForm(Forms::kFrameFirst + i, Forms::kPlugin) : nullptr;
			g_frameForms[i] = form ? form->GetFormID() : 0;
			found += form ? 1 : 0;
		}
		spdlog::info("Frames: {}/{} found", found, Forms::kFrameCount);
		return found == Forms::kFrameCount;
	}

	void OnGameLoaded()
	{
		g_preview = {};
		if (g_stalePreview) {
			if (auto* stale = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_stalePreview)) {
				stale->Disable();
				stale->SetDelete(true);
			}
			g_stalePreview = 0;
		}
		std::vector<std::pair<RE::FormID, Painting>> hung;
		{
			std::lock_guard lk(g_mtx);
			hung.assign(g_hung.begin(), g_hung.end());
		}
		for (const auto& [id, p] : hung) {
			const auto dds = DdsFor(p.id);
			Art::EnsureFromPng(dds, Art::FromUtf8(p.png), Art::kPaintingW, Art::kPaintingH);  // re-made if deleted
			Art::Show(id, dds);
		}
	}

	void BeginPlacement(const Painting& a_painting, RE::ObjectRefHandle a_moving, float a_scale)
	{
		Painting p = a_painting;
		p.style = std::clamp(p.style, 0, Forms::kFrameCount - 1);
		g_placeCellReset = true;
		{
			std::lock_guard lk(g_mtx);
			g_placement = Placement{ p, a_moving, std::clamp(a_scale, kMinScale, kMaxScale) };
		}
		Art::EnsureFromPng(DdsFor(p.id), Art::FromUtf8(p.png), Art::kPaintingW, Art::kPaintingH);
		g_placing = true;
		Art::Notify("Aim at a wall and left-click to hang. Wheel: size, Shift + wheel: frame, Up/Down: distance. Right-click cancels.");
	}

	bool IsPlacing() { return g_placing; }

	bool GetStatus(Status& a_out)
	{
		std::lock_guard lk(g_mtx);
		if (!g_placement) {
			return false;
		}
		a_out.name = g_placement->painting.name;
		a_out.frame = Forms::kFrameNames[g_placement->painting.style];
		a_out.scale = g_placement->scale;
		a_out.distance = g_placement->offset - kDefaultOffset;
		return true;
	}

	void Tick()
	{
		if (!g_previewQueued.exchange(true)) {
			SKSE::GetTaskInterface()->AddTask([] {
				if (g_placing || g_preview) {
					UpdatePreview();
				} else {
					g_previewQueued = false;
				}
				Art::Tick();
			});
		}
	}

	void CycleStyle(int a_delta)
	{
		std::lock_guard lk(g_mtx);
		if (!g_placement) {
			return;
		}
		auto& s = g_placement->painting.style;
		s = ((s + a_delta) % Forms::kFrameCount + Forms::kFrameCount) % Forms::kFrameCount;
	}

	void AdjustScale(int a_steps)
	{
		std::lock_guard lk(g_mtx);
		if (g_placement) {
			g_placement->scale = std::clamp(std::round((g_placement->scale + 0.1f * a_steps) * 10.0f) / 10.0f, kMinScale, kMaxScale);
		}
	}

	void AdjustOffset(int a_steps)
	{
		std::lock_guard lk(g_mtx);
		if (g_placement) {
			g_placement->offset = std::clamp(g_placement->offset + 0.25f * a_steps, kMinOffset, kMaxOffset);
		}
	}

	void RequestPlace()
	{
		if (g_placing && !g_placeQueued.exchange(true)) {
			SKSE::GetTaskInterface()->AddTask([] { DoPlace(); });
		}
	}

	void CancelPlacement(bool a_notify)
	{
		std::optional<Placement> pl;
		{
			std::lock_guard lk(g_mtx);
			pl = g_placement;
			g_placement.reset();
		}
		const bool was = g_placing.exchange(false);
		if (!pl || !was) {
			return;
		}
		SKSE::GetTaskInterface()->AddTask([pl, a_notify] {
			DeletePreview();
			if (auto old = pl->moving.get()) {
				if (auto* root = old->Get3D()) {
					root->SetAppCulled(false);
				}
			} else {
				Items::ReturnPainting(pl->painting);  // a new painting goes back into the pack
			}
			if (a_notify) {
				Art::Notify("Hanging cancelled");
			}
		});
	}

	bool Examine(bool a_respectCrosshair)
	{
		if (auto* pick = RE::CrosshairPickData::GetSingleton(); pick && a_respectCrosshair) {
			if (auto target = pick->target.get(); target && StyleOf(target->GetBaseObject()) < 0) {
				return false;  // the crosshair is on something else (NPC, door...): the game handles it
			}
		}
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->cameraRoot) {
			return false;
		}
		const auto&  xf = camera->cameraRoot->world;
		const auto   from = xf.translate;
		RE::NiPoint3 dir{ xf.rotate.entry[0][1], xf.rotate.entry[1][1], xf.rotate.entry[2][1] };
		const float  len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		if (len < 1e-4f) {
			return false;
		}
		dir = { dir.x / len, dir.y / len, dir.z / len };
		std::vector<RE::FormID> ids;
		{
			std::lock_guard lk(g_mtx);
			for (const auto& [id, p] : g_hung) {
				ids.push_back(id);
			}
		}
		RE::TESObjectREFR* best = nullptr;
		float              bestT = 1e30f;
		for (const auto id : ids) {
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			if (!ref || ref->IsDisabled() || !ref->Get3D()) {
				continue;
			}
			const float z = ref->GetAngleZ();
			auto        c = ref->GetPosition();
			c.x += std::sin(z) * 2.0f;
			c.y += std::cos(z) * 2.0f;
			const RE::NiPoint3 v{ c.x - from.x, c.y - from.y, c.z - from.z };
			const float        t = v.x * dir.x + v.y * dir.y + v.z * dir.z;
			if (t < 0.0f || t > kExamineReach) {
				continue;
			}
			const RE::NiPoint3 off{ v.x - dir.x * t, v.y - dir.y * t, v.z - dir.z * t };
			const float        miss = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
			const float        radius = 0.5f * Forms::kCanvasH * (ref->GetReferenceRuntimeData().refScale / 100.0f) + 4.0f;
			if (miss <= radius && t < bestT) {
				best = ref;
				bestT = t;
			}
		}
		if (!best) {
			return false;
		}
		MenuInfo info;
		info.ref = best->GetHandle();
		info.scale = best->GetReferenceRuntimeData().refScale / 100.0f;
		std::lock_guard lk(g_mtx);
		info.painting = g_hung[best->GetFormID()];
		info.painting.style = (std::max)(0, StyleOf(best->GetBaseObject()));
		g_activated = info;
		spdlog::info("Examine: painting {:08X} \"{}\"", best->GetFormID(), info.painting.name);
		return true;
	}

	std::optional<MenuInfo> TakeActivated()
	{
		std::lock_guard lk(g_mtx);
		auto            out = g_activated;
		g_activated.reset();
		return out;
	}

	void TakeDown(RE::ObjectRefHandle a_ref)
	{
		SKSE::GetTaskInterface()->AddTask([a_ref] {
			auto ref = a_ref.get();
			if (!ref) {
				return;
			}
			Painting p;
			bool     known = false;
			{
				std::lock_guard lk(g_mtx);
				if (const auto it = g_hung.find(ref->GetFormID()); it != g_hung.end()) {
					p = it->second;
					known = true;
					g_hung.erase(it);
				}
			}
			p.style = (std::max)(0, StyleOf(ref->GetBaseObject()));
			DeleteRef(ref.get(), "taken down");
			if (known) {
				Items::ReturnPainting(p);
				Art::Notify(fmt::format("\"{}\" taken down", p.name));
			}
		});
	}

	void SetScale(RE::ObjectRefHandle a_ref, float a_scale)
	{
		SKSE::GetTaskInterface()->AddTask([a_ref, a_scale] {
			if (auto ref = a_ref.get()) {
				SetRefScale(ref.get(), std::clamp(a_scale, kMinScale, kMaxScale));
			}
		});
	}

	void ChangeFrame(RE::ObjectRefHandle a_ref, int a_style)
	{
		SKSE::GetTaskInterface()->AddTask([a_ref, a_style] {
			auto  old = a_ref.get();
			auto* base = FrameForm(a_style);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!old || !base || !player || StyleOf(old->GetBaseObject()) == a_style) {
				return;
			}
			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				base, old->GetPosition(), old->GetAngle(), old->GetParentCell(), old->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), true, true);
			auto ref = handle.get();
			if (!ref) {
				return;
			}
			SetRefScale(ref.get(), old->GetReferenceRuntimeData().refScale / 100.0f);
			Painting p;
			{
				std::lock_guard lk(g_mtx);
				p = g_hung[old->GetFormID()];
				g_hung.erase(old->GetFormID());
				p.style = a_style;
				g_hung[ref->GetFormID()] = p;
			}
			DeleteRef(old.get(), "re-framed");
			Art::Show(ref->GetFormID(), DdsFor(p.id));
			MenuInfo info{ handle, p, ref->GetReferenceRuntimeData().refScale / 100.0f };
			std::lock_guard lk(g_mtx);
			g_activated = info;  // the open menu now controls the new frame
		});
	}

	void Move(RE::ObjectRefHandle a_ref)
	{
		SKSE::GetTaskInterface()->AddTask([a_ref] {
			auto ref = a_ref.get();
			if (!ref) {
				return;
			}
			Painting p;
			{
				std::lock_guard lk(g_mtx);
				const auto      it = g_hung.find(ref->GetFormID());
				if (it == g_hung.end()) {
					return;
				}
				p = it->second;
			}
			p.style = (std::max)(0, StyleOf(ref->GetBaseObject()));
			BeginPlacement(p, a_ref, ref->GetReferenceRuntimeData().refScale / 100.0f);
		});
	}

	int HungCount()
	{
		std::lock_guard lk(g_mtx);
		return static_cast<int>(g_hung.size());
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		if (a_intfc->OpenRecord(kRecordHung, 1)) {
			a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_hung.size()));
			for (const auto& [id, p] : g_hung) {
				a_intfc->WriteRecordData(id);
				Items::WriteString(a_intfc, p.id);
				Items::WriteString(a_intfc, p.name);
				Items::WriteString(a_intfc, p.png);
			}
		}
		if (auto preview = g_preview.get(); preview && a_intfc->OpenRecord(kRecordPreview, 1)) {
			a_intfc->WriteRecordData(preview->GetFormID());
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version)
	{
		if (a_type == kRecordPreview) {
			RE::FormID oldID = 0, newID = 0;
			if (a_intfc->ReadRecordData(oldID) == sizeof(oldID) && a_intfc->ResolveFormID(oldID, newID)) {
				g_stalePreview = newID;
			}
			return;
		}
		if (a_type != kRecordHung || a_version != 1) {
			return;
		}
		std::uint32_t count = 0;
		a_intfc->ReadRecordData(count);
		std::lock_guard lk(g_mtx);
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::FormID oldID = 0, newID = 0;
			Painting   p;
			if (a_intfc->ReadRecordData(oldID) != sizeof(oldID) || !Items::ReadString(a_intfc, p.id) || !Items::ReadString(a_intfc, p.name) ||
				!Items::ReadString(a_intfc, p.png)) {
				spdlog::error("Co-save: hung paintings truncated after {}", i);
				break;
			}
			if (a_intfc->ResolveFormID(oldID, newID)) {
				g_hung[newID] = p;
			}
		}
		spdlog::info("Co-save: {} hung painting(s)", g_hung.size());
	}

	void Revert()
	{
		std::lock_guard lk(g_mtx);
		g_hung.clear();
		g_activated.reset();
		g_placement.reset();
		g_placing = false;
	}
}
