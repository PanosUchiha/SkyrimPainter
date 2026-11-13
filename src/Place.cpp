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

#include "Place.h"

#include "Art.h"
#include "Forms.h"
#include "Items.h"

namespace Place
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
		constexpr float         kWallReach = 2000.0f;
		constexpr float         kFloorReach = 600.0f;
		constexpr float         kPi = 3.14159265f;

		std::mutex                          g_mtx;  // guards the state below
		std::unordered_map<RE::FormID, int> g_hung;  // painting on a wall -> slot
		RE::FormID                          g_wall[2]{};  // landscape, portrait

		struct Placement
		{
			Mode                mode = Mode::None;
			int                 slot = -1;
			RE::ObjectRefHandle moving;
			float               scale = 1.0f;
			float               offset = kDefaultOffset;
			float               turn = 0.0f;  // easel: extra turn (radians) on top of facing the player
		};
		std::optional<Placement> g_placement;
		std::atomic<bool>        g_placing{ false };
		std::atomic<bool>        g_placeQueued{ false };
		std::optional<MenuInfo>  g_activated;

		bool PortraitOf(int a_slot)
		{
			Items::SlotView v;
			return Items::GetSlot(a_slot, v) && v.portrait;
		}

		RE::TESBoundObject* BaseFor(const Placement& a_pl)
		{
			if (a_pl.mode == Mode::Easel) {
				return Items::EaselStandForm();
			}
			const auto id = g_wall[PortraitOf(a_pl.slot) ? 1 : 0];
			return id ? RE::TESForm::LookupByID<RE::TESBoundObject>(id) : nullptr;
		}

		void DeleteRef(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}
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
			const float        s = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData    pick{};
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
			float        angleZ = 0.0f;
		};

		std::optional<Pose> ComputePose(const Placement& a_pl, const char*& a_whyNot)
		{
			const bool easel = a_pl.mode == Mode::Easel;
			const auto hit = RaycastFromCamera(easel ? kFloorReach : kWallReach);
			if (!hit) {
				a_whyNot = easel ? "Aim at the floor closer to you" : "Nothing in reach - aim at a wall closer to you";
				return std::nullopt;
			}
			if (hit->ref && hit->ref->Is(RE::FormType::ActorCharacter)) {
				a_whyNot = "Not on someone";
				return std::nullopt;
			}
			const auto& n = hit->normal;
			Pose        pose;
			if (easel) {
				if (n.z < 0.7f) {
					a_whyNot = "That's not level ground - aim at the floor";
					return std::nullopt;
				}
				pose.pos = hit->pos;
				auto* player = RE::PlayerCharacter::GetSingleton();
				const auto pp = player ? player->GetPosition() : hit->pos;
				// the canvas side faces the player (Skyrim's Z angle turns local +Y towards (sin z, cos z))
				pose.angleZ = std::atan2(pp.x - hit->pos.x, pp.y - hit->pos.y) + a_pl.turn;
				return pose;
			}
			const float nlen = std::sqrt(n.x * n.x + n.y * n.y);
			if (std::fabs(n.z) > 0.6f || nlen < 0.3f) {
				a_whyNot = "That's not a wall - aim at a vertical surface";
				return std::nullopt;
			}
			const float nx = n.x / nlen, ny = n.y / nlen;
			pose.pos = { hit->pos.x + nx * a_pl.offset, hit->pos.y + ny * a_pl.offset, hit->pos.z };
			pose.angleZ = std::atan2(nx, ny);
			return pose;
		}

		// ------------------------------------------------------------------ preview (main thread)
		RE::ObjectRefHandle g_preview;
		RE::TESBoundObject* g_previewBase = nullptr;
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
				DeleteRef(ref.get());
			}
			g_preview = {};
			g_previewBase = nullptr;
			g_previewReady = g_previewTextured = g_previewVisible = false;
		}

		void CreatePreview(const Placement& a_pl)
		{
			DeletePreview();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* base = BaseFor(a_pl);
			if (!player || !base) {
				return;
			}
			const float        h = player->GetAngleZ();
			const RE::NiPoint3 p = player->GetPosition();
			const RE::NiPoint3 at{ p.x + std::sin(h) * 150.0f, p.y + std::cos(h) * 150.0f, p.z };
			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				base, at, { 0.0f, 0.0f, h + kPi }, player->GetParentCell(), player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), false, true);
			if (auto old = a_pl.moving.get(); old && old->Get3D()) {
				old->Get3D()->SetAppCulled(true);
			}
			if (auto ref = handle.get()) {
				ref->SetCollision(false);
				g_preview = handle;
				g_previewBase = base;
			}
		}

		void FinishPlacement()
		{
			{
				std::lock_guard lk(g_mtx);
				g_placement.reset();
			}
			g_placing = false;
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
					DeletePreview();
					Cancel(false);
					Art::Notify("Placing cancelled - you left the area");
					return;
				}
			}
			auto ref = g_preview.get();
			if (!ref || g_previewBase != BaseFor(*pl)) {
				CreatePreview(*pl);
				return;
			}
			auto* root = ref->Get3D();
			if (!root) {
				return;
			}
			if (!g_previewReady) {
				root->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);  // the aiming ray must not hit the preview
				g_previewReady = true;
			}
			if (pl->mode == Mode::Painting && !g_previewTextured) {
				Items::SlotView v;
				if (Items::GetSlot(pl->slot, v) && Art::IsReady(v.dds) && Art::ApplyNow(ref.get(), v.dds)) {
					g_previewTextured = true;
				}
			}
			const char* why = nullptr;
			const auto  pose = ComputePose(*pl, why);
			if (!pose) {
				if (g_previewVisible) {
					root->SetAppCulled(true);
					g_previewVisible = false;
				}
				return;
			}
			ref->data.location = pose->pos;
			ref->data.angle = { 0.0f, 0.0f, pose->angleZ };
			ref->Update3DPosition(true);
			if (pl->mode == Mode::Painting && std::fabs(root->local.scale - pl->scale) > 0.001f) {
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
			const auto  pose = ComputePose(*pl, why);
			if (!pose) {
				Art::Notify(why ? why : "Can't place it here");
				return;
			}
			auto* base = BaseFor(*pl);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!base || !player) {
				Art::Notify("SkyrimPainter: record missing - is SkyrimPainter.esp enabled?");
				return;
			}
			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				base, pose->pos, { 0.0f, 0.0f, pose->angleZ }, player->GetParentCell(), player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), true, true);
			auto ref = handle.get();
			if (!ref) {
				Art::Notify("SkyrimPainter: could not place it here");
				return;
			}
			if (pl->mode == Mode::Easel) {
				Items::EaselPlaced(ref.get());
				DeletePreview();
				FinishPlacement();
				return;
			}
			// a painting
			if (std::fabs(pl->scale - 1.0f) > 0.001f) {
				SetRefScale(ref.get(), pl->scale);
			}
			{
				std::lock_guard lk(g_mtx);
				g_hung[ref->GetFormID()] = pl->slot;
			}
			Items::SlotView v;
			if (Items::GetSlot(pl->slot, v)) {
				Art::Show(ref->GetFormID(), v.dds);
			}
			if (auto old = pl->moving.get()) {
				{
					std::lock_guard lk(g_mtx);
					g_hung.erase(old->GetFormID());
				}
				DeleteRef(old.get());
			} else {
				Items::TakePaintingItem(pl->slot);
			}
			DeletePreview();
			FinishPlacement();
			Art::Notify(fmt::format("\"{}\" hung", v.name));
		}
	}

	bool Init()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		for (int i = 0; i < 2; ++i) {
			auto* f = dh ? dh->LookupForm(Forms::kWallCanvas[i], Forms::kPlugin) : nullptr;
			g_wall[i] = f ? f->GetFormID() : 0;
		}
		return g_wall[0] && g_wall[1];
	}

	void OnGameLoaded()
	{
		g_preview = {};
		g_previewBase = nullptr;
		if (g_stalePreview) {
			if (auto* stale = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_stalePreview)) {
				stale->Disable();
				stale->SetDelete(true);
			}
			g_stalePreview = 0;
		}
		std::vector<std::pair<RE::FormID, int>> hung;
		{
			std::lock_guard lk(g_mtx);
			hung.assign(g_hung.begin(), g_hung.end());
		}
		for (const auto& [id, slot] : hung) {
			Items::SlotView v;
			if (Items::GetSlot(slot, v)) {
				Items::EnsureSlotTexture(slot);
				Art::Show(id, v.dds);
			}
		}
	}

	void BeginEasel()
	{
		g_placeCellReset = true;
		{
			std::lock_guard lk(g_mtx);
			g_placement = Placement{ Mode::Easel };
		}
		g_placing = true;
		Art::Notify("Aim at the floor and left-click to set up the easel. Wheel: turn it. Right-click cancels.");
	}

	void BeginPainting(int a_slot, RE::ObjectRefHandle a_moving, float a_scale)
	{
		g_placeCellReset = true;
		Items::EnsureSlotTexture(a_slot);
		{
			std::lock_guard lk(g_mtx);
			Placement p;
			p.mode = Mode::Painting;
			p.slot = a_slot;
			p.moving = a_moving;
			p.scale = std::clamp(a_scale, kMinScale, kMaxScale);
			g_placement = p;
		}
		g_placing = true;
		Art::Notify("Aim at a wall and left-click to hang. Wheel: size, Up/Down: distance from the wall. Right-click cancels.");
	}

	bool IsPlacing() { return g_placing; }

	bool GetStatus(Status& a_out)
	{
		std::lock_guard lk(g_mtx);
		if (!g_placement) {
			return false;
		}
		a_out.mode = g_placement->mode;
		a_out.scale = g_placement->scale;
		a_out.distance = g_placement->offset - kDefaultOffset;
		a_out.turnDeg = static_cast<int>(std::lround(g_placement->turn * 180.0f / kPi));
		Items::SlotView v;
		if (g_placement->mode == Mode::Painting && Items::GetSlot(g_placement->slot, v)) {
			a_out.name = v.name;
		}
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

	void Wheel(int a_dir, bool a_shift)
	{
		std::lock_guard lk(g_mtx);
		if (!g_placement) {
			return;
		}
		if (g_placement->mode == Mode::Easel) {
			g_placement->turn += a_dir * (a_shift ? 5.0f : 15.0f) * kPi / 180.0f;
		} else {
			g_placement->scale = std::clamp(std::round((g_placement->scale + (a_shift ? 0.02f : 0.1f) * a_dir) * 100.0f) / 100.0f, kMinScale, kMaxScale);
		}
	}

	void Nudge(int a_steps)
	{
		std::lock_guard lk(g_mtx);
		if (g_placement && g_placement->mode == Mode::Painting) {
			g_placement->offset = std::clamp(g_placement->offset + 0.25f * a_steps, kMinOffset, kMaxOffset);
		}
	}

	void RequestPlace()
	{
		if (g_placing && !g_placeQueued.exchange(true)) {
			SKSE::GetTaskInterface()->AddTask([] { DoPlace(); });
		}
	}

	void Cancel(bool a_notify)
	{
		std::optional<Placement> pl;
		{
			std::lock_guard lk(g_mtx);
			pl = g_placement;
			g_placement.reset();
		}
		if (!g_placing.exchange(false) || !pl) {
			return;
		}
		SKSE::GetTaskInterface()->AddTask([pl, a_notify] {
			DeletePreview();
			if (auto old = pl->moving.get(); old && old->Get3D()) {
				old->Get3D()->SetAppCulled(false);
			}
			if (a_notify) {
				Art::Notify("Placing cancelled");
			}
		});
	}

	bool IsWallPainting(const RE::TESForm* a_base)
	{
		return a_base && (a_base->GetFormID() == g_wall[0] || a_base->GetFormID() == g_wall[1]) && a_base->GetFormID() != 0;
	}

	void OpenMenu(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return;
		}
		std::lock_guard lk(g_mtx);
		const auto      it = g_hung.find(a_ref->GetFormID());
		if (it == g_hung.end()) {
			return;
		}
		g_activated = MenuInfo{ a_ref->GetHandle(), it->second, a_ref->GetReferenceRuntimeData().refScale / 100.0f };
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
			int slot = -1;
			{
				std::lock_guard lk(g_mtx);
				if (const auto it = g_hung.find(ref->GetFormID()); it != g_hung.end()) {
					slot = it->second;
					g_hung.erase(it);
				}
			}
			DeleteRef(ref.get());
			if (slot >= 0) {
				Items::GivePaintingItem(slot);
				Items::SlotView v;
				Items::GetSlot(slot, v);
				Art::Notify(fmt::format("\"{}\" taken down", v.name));
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

	void Move(RE::ObjectRefHandle a_ref)
	{
		SKSE::GetTaskInterface()->AddTask([a_ref] {
			auto ref = a_ref.get();
			if (!ref) {
				return;
			}
			int slot = -1;
			{
				std::lock_guard lk(g_mtx);
				if (const auto it = g_hung.find(ref->GetFormID()); it != g_hung.end()) {
					slot = it->second;
				}
			}
			if (slot >= 0) {
				BeginPainting(slot, a_ref, ref->GetReferenceRuntimeData().refScale / 100.0f);
			}
		});
	}

	void RefreshSlot(int a_slot)
	{
		Items::SlotView v;
		if (!Items::GetSlot(a_slot, v)) {
			return;
		}
		std::vector<RE::FormID> ids;
		{
			std::lock_guard lk(g_mtx);
			for (const auto& [id, s] : g_hung) {
				if (s == a_slot) {
					ids.push_back(id);
				}
			}
		}
		for (const auto id : ids) {
			Art::Show(id, v.dds);
		}
	}

	bool SlotHung(int a_slot)
	{
		std::lock_guard lk(g_mtx);
		return std::any_of(g_hung.begin(), g_hung.end(), [a_slot](const auto& e) { return e.second == a_slot; });
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		if (a_intfc->OpenRecord(kRecordHung, 1)) {
			a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_hung.size()));
			for (const auto& [id, slot] : g_hung) {
				a_intfc->WriteRecordData(id);
				a_intfc->WriteRecordData(static_cast<std::int32_t>(slot));
			}
		}
		if (auto preview = g_preview.get(); preview && a_intfc->OpenRecord(kRecordPreview, 1)) {
			a_intfc->WriteRecordData(preview->GetFormID());
		}
	}

	bool Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version)
	{
		if (a_type == kRecordPreview) {
			RE::FormID oldID = 0, newID = 0;
			if (a_intfc->ReadRecordData(oldID) == sizeof(oldID) && a_intfc->ResolveFormID(oldID, newID)) {
				g_stalePreview = newID;
			}
			return true;
		}
		if (a_type != kRecordHung) {
			return false;
		}
		if (a_version != 1) {
			return true;
		}
		std::uint32_t n = 0;
		a_intfc->ReadRecordData(n);
		std::lock_guard lk(g_mtx);
		for (std::uint32_t i = 0; i < n; ++i) {
			RE::FormID   oldID = 0, newID = 0;
			std::int32_t slot = -1;
			if (a_intfc->ReadRecordData(oldID) != sizeof(oldID) || a_intfc->ReadRecordData(slot) != sizeof(slot)) {
				break;
			}
			if (a_intfc->ResolveFormID(oldID, newID)) {
				g_hung[newID] = slot;
			}
		}
		spdlog::info("Co-save: {} painting(s) on walls", g_hung.size());
		return true;
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
