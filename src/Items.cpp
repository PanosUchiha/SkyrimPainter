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

#include "Items.h"

#include "Art.h"
#include "Dds.h"
#include "Forms.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include <stb_image_write.h>

namespace fs = std::filesystem;

namespace Items
{
	namespace
	{
		constexpr std::uint32_t kSerializationID = 'SKPT';
		constexpr std::uint32_t kRecordEasels = 'EASL';
		constexpr std::uint32_t kRecordUnhung = 'UNHG';
		constexpr int           kWipW = 512;  // the painting in progress on the easel's canvas
		constexpr int           kWipH = 384;

		RE::TESObjectMISC* g_canvas = nullptr;
		RE::TESObjectMISC* g_easelItem = nullptr;
		RE::TESObjectMISC* g_palette = nullptr;
		RE::TESObjectMISC* g_paints = nullptr;
		RE::TESObjectMISC* g_painting = nullptr;
		RE::TESObjectACTI* g_easelStand = nullptr;

		struct EaselState
		{
			std::string wipId;   // name of its files
			std::string wipPng;  // UTF-8 path, "" = blank canvas
			std::string wipDds;  // game path of the texture on its canvas, "" = blank
		};

		std::mutex                                   g_mtx;  // guards the state below
		std::unordered_map<RE::FormID, EaselState>   g_easels;
		std::vector<Wall::Painting>                  g_unhung;
		std::optional<Easel>                         g_studioRequest;
		std::atomic<bool>                            g_pickerRequest{ false };
		std::unordered_map<RE::FormID, int>          g_ignoreRemovals;  // our own RemoveItem calls (main thread)

		bool IsOurs(RE::FormID a_base)
		{
			for (auto* f : { g_canvas, g_easelItem, g_palette, g_paints, g_painting }) {
				if (f && f->GetFormID() == a_base) {
					return true;
				}
			}
			return false;
		}

		int CountOf(RE::TESBoundObject* a_form)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !a_form) {
				return 0;
			}
			const auto counts = player->GetInventoryCounts([a_form](RE::TESBoundObject& a_obj) { return &a_obj == a_form; });
			const auto it = counts.find(a_form);
			return it == counts.end() ? 0 : it->second;
		}

		void Give(RE::TESBoundObject* a_form, int a_count)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && a_form && a_count > 0) {
				player->AddObjectToContainer(a_form, nullptr, a_count, nullptr);
			}
		}

		void Take(RE::TESBoundObject* a_form, int a_count)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && a_form && a_count > 0) {
				g_ignoreRemovals[a_form->GetFormID()] += a_count;
				player->RemoveItem(a_form, a_count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			}
		}

		fs::path PaintingsFolder() { return Art::UserFolder() / L"Paintings"; }
		fs::path WipFolder() { return Art::UserFolder() / L"InProgress"; }

		bool WritePng(const fs::path& a_path, const std::vector<std::uint8_t>& a_rgba, std::string& a_error)
		{
			std::error_code ec;
			fs::create_directories(a_path.parent_path(), ec);
			auto tmp = a_path;
			tmp += L".tmp";
			const auto utf8 = Art::ToUtf8(tmp);
			if (!stbi_write_png(utf8.c_str(), Art::kPaintingW, Art::kPaintingH, 4, a_rgba.data(), Art::kPaintingW * 4)) {
				a_error = "could not write " + utf8;
				return false;
			}
			if (!::MoveFileExW(tmp.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				a_error = fmt::format("rename failed ({})", ::GetLastError());
				return false;
			}
			return true;
		}

		// Ground height under (x, y), looking down from a bit above a_z. a_z itself if nothing is hit.
		float GroundZ(float a_x, float a_y, float a_z)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return a_z;
			}
			const float     s = RE::bhkWorld::GetWorldScale();
			const float     top = a_z + 90.0f, bottom = a_z - 300.0f;
			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4(a_x * s, a_y * s, top * s, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_x * s, a_y * s, bottom * s, 0.0f);
			RE::CFilter filter{};
			player->GetCollisionFilterInfo(filter);
			pick.rayInput.filterInfo.filter = (filter.filter & 0xFFFF0000) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
			world->PickObject(pick);
			if (!pick.rayOutput.HasHit()) {
				return a_z;
			}
			return top + (bottom - top) * pick.rayOutput.hitFraction;
		}

		void RemoveEaselRef(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}
			EaselState st;
			{
				std::lock_guard lk(g_mtx);
				if (const auto it = g_easels.find(a_ref->GetFormID()); it != g_easels.end()) {
					st = it->second;
					g_easels.erase(it);
				}
			}
			Art::Forget(a_ref->GetFormID());
			a_ref->Disable();
			a_ref->SetDelete(true);
			std::error_code ec;
			if (!st.wipPng.empty()) {
				fs::remove(Art::FromUtf8(st.wipPng), ec);
			}
			Art::DeleteTexture(st.wipDds);
			spdlog::info("Easel {:08X} removed", a_ref->GetFormID());
		}

		// ---- dropping our items (main thread)
		void SetUpEasel()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !g_easelStand) {
				return;
			}
			const float        h = player->GetAngleZ();
			const RE::NiPoint3 p = player->GetPosition();
			RE::NiPoint3       at{ p.x + std::sin(h) * 85.0f, p.y + std::cos(h) * 85.0f, p.z };
			at.z = GroundZ(at.x, at.y, p.z);
			const RE::NiPoint3 angle{ 0.0f, 0.0f, h + 3.14159265f };  // the canvas faces the player
			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				g_easelStand, at, angle, player->GetParentCell(), player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), true, true);
			auto ref = handle.get();
			if (!ref) {
				Art::Notify("SkyrimPainter: could not set up the easel here");
				Give(g_canvas, 1);
				Give(g_easelItem, 1);
				return;
			}
			{
				std::lock_guard lk(g_mtx);
				g_easels[ref->GetFormID()] = EaselState{ Art::NewId(), {}, {} };
			}
			spdlog::info("Easel {:08X} set up at ({:.0f}, {:.0f}, {:.0f})", ref->GetFormID(), at.x, at.y, at.z);
			Art::Notify("Easel set up. Activate it to paint (you need a Palette and Paints)");
		}

		void HandleDrop(RE::FormID a_base, int a_count, RE::ObjectRefHandle a_dropped)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* form = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
			if (!player || !form) {
				return;
			}
			// the dropped item in the world (the event usually names it; otherwise the nearest one)
			RE::NiPointer<RE::TESObjectREFR> dropped = a_dropped.get();
			if (!dropped) {
				if (auto* tes = RE::TES::GetSingleton()) {
					float best = 1e30f;
					tes->ForEachReferenceInRange(player, 500.0f, [&](RE::TESObjectREFR* a_ref) {
						if (a_ref && a_ref->GetBaseObject() == form && !a_ref->IsDisabled()) {
							const float d = a_ref->GetPosition().GetDistance(player->GetPosition());
							if (d < best) {
								best = d;
								dropped = RE::NiPointer<RE::TESObjectREFR>(a_ref);
							}
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
			}
			if (!dropped) {
				return;  // not a drop (sold, stolen, removed by a script...)
			}
			dropped->Disable();
			dropped->SetDelete(true);
			spdlog::info("Dropped {} x{}", form->GetName(), a_count);

			if (form == g_canvas) {
				Give(g_canvas, a_count - 1);
				if (CountOf(g_easelItem) <= 0) {
					Give(g_canvas, 1);
					Art::Notify("You need an Easel to set up a canvas");
					return;
				}
				Take(g_easelItem, 1);
				SetUpEasel();
			} else if (form == g_painting) {
				std::vector<Wall::Painting> list;
				{
					std::lock_guard lk(g_mtx);
					list = g_unhung;
				}
				if (list.empty()) {
					Give(g_painting, a_count);
					Art::Notify("That painting is blank - paint one at your easel first");
					return;
				}
				Give(g_painting, a_count - 1);
				if (list.size() == 1 || Wall::IsPlacing()) {
					if (Wall::IsPlacing()) {
						Give(g_painting, 1);
						return;
					}
					{
						std::lock_guard lk(g_mtx);
						g_unhung.erase(g_unhung.begin());
					}
					Wall::BeginPlacement(list.front());
				} else {
					g_pickerRequest = true;  // the picker opens once the inventory is closed
				}
			} else {
				Give(form, a_count);
				Art::Notify(form == g_easelItem ? "Drop a Blank Canvas to set up your easel" : "Keep it in your pack - you'll need it at the easel");
			}
		}

		// ---- activation (main thread)
		bool TryEasel()
		{
			if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
				if (auto target = pick->target.get(); target && target->GetBaseObject() != g_easelStand) {
					return false;
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
			std::vector<std::pair<RE::FormID, std::string>> easels;
			{
				std::lock_guard lk(g_mtx);
				for (const auto& [id, st] : g_easels) {
					easels.emplace_back(id, st.wipPng);
				}
			}
			RE::TESObjectREFR* best = nullptr;
			std::string        bestWip;
			float              bestT = 1e30f;
			for (const auto& [id, wip] : easels) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!ref || ref->IsDisabled() || !ref->Get3D()) {
					continue;
				}
				auto c = ref->GetPosition();
				c.z += 78.0f;  // middle of the canvas
				const RE::NiPoint3 v{ c.x - from.x, c.y - from.y, c.z - from.z };
				const float        t = v.x * dir.x + v.y * dir.y + v.z * dir.z;
				if (t < 0.0f || t > 400.0f) {
					continue;
				}
				const RE::NiPoint3 off{ v.x - dir.x * t, v.y - dir.y * t, v.z - dir.z * t };
				const float        miss = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
				if (miss <= 45.0f && t < bestT) {
					best = ref;
					bestT = t;
					bestWip = wip;
				}
			}
			if (!best) {
				return false;
			}
			if (CountOf(g_palette) <= 0 || CountOf(g_paints) <= 0) {
				Art::Notify("You need a Painter's Palette and Paints to paint");
				return true;
			}
			std::lock_guard lk(g_mtx);
			g_studioRequest = Easel{ best->GetHandle(), bestWip };
			spdlog::info("Studio: easel {:08X}", best->GetFormID());
			return true;
		}

		class EventSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!a_event || a_event->oldContainer != 0x14 || a_event->newContainer != 0 || !IsOurs(a_event->baseObj)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto base = a_event->baseObj;
				const int  count = (std::max)(1, a_event->itemCount);
				const auto dropped = a_event->reference;
				SKSE::GetTaskInterface()->AddTask([base, count, dropped] {
					if (auto it = g_ignoreRemovals.find(base); it != g_ignoreRemovals.end() && it->second > 0) {
						it->second -= count;  // our own RemoveItem
						if (it->second <= 0) {
							g_ignoreRemovals.erase(it);
						}
						return;
					}
					HandleDrop(base, count, dropped);
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EventSink g_sink;

		// ---- co-save
		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			{
				std::lock_guard lk(g_mtx);
				if (a_intfc->OpenRecord(kRecordEasels, 1)) {
					a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_easels.size()));
					for (const auto& [id, st] : g_easels) {
						a_intfc->WriteRecordData(id);
						WriteString(a_intfc, st.wipId);
						WriteString(a_intfc, st.wipPng);
						WriteString(a_intfc, st.wipDds);
					}
				}
				if (a_intfc->OpenRecord(kRecordUnhung, 1)) {
					a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_unhung.size()));
					for (const auto& p : g_unhung) {
						WriteString(a_intfc, p.id);
						WriteString(a_intfc, p.name);
						WriteString(a_intfc, p.png);
						a_intfc->WriteRecordData(static_cast<std::int32_t>(p.style));
					}
				}
			}
			Wall::Save(a_intfc);
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0, version = 0, length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == kRecordEasels && version == 1) {
					std::uint32_t n = 0;
					a_intfc->ReadRecordData(n);
					std::lock_guard lk(g_mtx);
					for (std::uint32_t i = 0; i < n; ++i) {
						RE::FormID oldID = 0, newID = 0;
						EaselState st;
						if (a_intfc->ReadRecordData(oldID) != sizeof(oldID) || !ReadString(a_intfc, st.wipId) || !ReadString(a_intfc, st.wipPng) ||
							!ReadString(a_intfc, st.wipDds)) {
							break;
						}
						if (a_intfc->ResolveFormID(oldID, newID)) {
							g_easels[newID] = st;
						}
					}
					spdlog::info("Co-save: {} easel(s)", g_easels.size());
				} else if (type == kRecordUnhung && version == 1) {
					std::uint32_t n = 0;
					a_intfc->ReadRecordData(n);
					std::lock_guard lk(g_mtx);
					for (std::uint32_t i = 0; i < n; ++i) {
						Wall::Painting p;
						std::int32_t   style = 0;
						if (!ReadString(a_intfc, p.id) || !ReadString(a_intfc, p.name) || !ReadString(a_intfc, p.png) ||
							a_intfc->ReadRecordData(style) != sizeof(style)) {
							break;
						}
						p.style = style;
						g_unhung.push_back(std::move(p));
					}
					spdlog::info("Co-save: {} painting(s) in the pack", g_unhung.size());
				} else {
					Wall::Load(a_intfc, type, version);
				}
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			{
				std::lock_guard lk(g_mtx);
				g_easels.clear();
				g_unhung.clear();
				g_studioRequest.reset();
			}
			g_pickerRequest = false;
			Art::ForgetAll();
			Wall::Revert();
		}
	}

	bool Init()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return false;
		}
		auto misc = [&](std::uint32_t a_id) { return dh->LookupForm<RE::TESObjectMISC>(a_id, Forms::kPlugin); };
		g_canvas = misc(Forms::kCanvas);
		g_easelItem = misc(Forms::kEaselItem);
		g_palette = misc(Forms::kPalette);
		g_paints = misc(Forms::kPaints);
		g_painting = misc(Forms::kPainting);
		g_easelStand = dh->LookupForm<RE::TESObjectACTI>(Forms::kEaselStand, Forms::kPlugin);
		const bool ok = g_canvas && g_easelItem && g_palette && g_paints && g_painting && g_easelStand;
		spdlog::info("Items: canvas {} easel {} palette {} paints {} painting {} easel stand {}", g_canvas != nullptr, g_easelItem != nullptr,
			g_palette != nullptr, g_paints != nullptr, g_painting != nullptr, g_easelStand != nullptr);

		// The recipes use the tanning rack's keyword by form ID; check it really is CraftingTanningRack.
		auto* rack = RE::TESForm::LookupByID<RE::BGSKeyword>(Forms::kTanningRackKeyword);
		if (!rack || _stricmp(rack->GetFormEditorID(), "CraftingTanningRack") != 0) {
			rack = nullptr;
			for (auto* kw : dh->GetFormArray<RE::BGSKeyword>()) {
				if (kw && _stricmp(kw->GetFormEditorID(), "CraftingTanningRack") == 0) {
					rack = kw;
					break;
				}
			}
			if (rack) {
				for (int i = 0; i < Forms::kRecipeCount; ++i) {
					if (auto* cobj = dh->LookupForm<RE::BGSConstructibleObject>(Forms::kRecipeFirst + i, Forms::kPlugin)) {
						cobj->benchKeyword = rack;
					}
				}
				spdlog::warn("Recipes moved to the tanning rack keyword {:08X}", rack->GetFormID());
			} else {
				spdlog::error("CraftingTanningRack keyword not found - the recipes may not show up");
			}
		} else {
			spdlog::info("Recipes: tanning rack keyword OK");
		}

		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESContainerChangedEvent>(&g_sink);
		}
		return ok;
	}

	void OnGameLoaded()
	{
		std::vector<std::pair<RE::FormID, EaselState>> easels;
		{
			std::lock_guard lk(g_mtx);
			easels.assign(g_easels.begin(), g_easels.end());
		}
		for (const auto& [id, st] : easels) {
			if (st.wipDds.empty()) {
				continue;
			}
			Art::EnsureFromPng(st.wipDds, Art::FromUtf8(st.wipPng), kWipW, kWipH);  // re-made if the texture was deleted
			Art::Show(id, st.wipDds);
		}
	}

	std::optional<Easel> TakeStudioRequest()
	{
		std::lock_guard lk(g_mtx);
		auto            out = g_studioRequest;
		g_studioRequest.reset();
		return out;
	}

	std::vector<Wall::Painting> Unhung()
	{
		std::lock_guard lk(g_mtx);
		return g_unhung;
	}

	bool TakePickerRequest() { return g_pickerRequest.exchange(false); }

	void StepAway(RE::ObjectRefHandle a_easel, std::vector<std::uint8_t> a_rgba)
	{
		auto ref = a_easel.get();
		if (!ref) {
			return;
		}
		const auto id = ref->GetFormID();
		EaselState st;
		{
			std::lock_guard lk(g_mtx);
			const auto      it = g_easels.find(id);
			if (it == g_easels.end()) {
				return;
			}
			st = it->second;
		}
		std::thread([id, st, px = std::move(a_rgba)] {
			static std::atomic<std::uint32_t> serial{ 0 };
			const auto  png = WipFolder() / Art::FromUtf8(st.wipId + ".png");
			const auto  dds = fmt::format("textures\\SkyrimPainter\\InProgress\\{}_{}.dds", st.wipId, ++serial);  // new name: the game caches textures by name
			std::string err;
			if (!WritePng(png, px, err)) {
				spdlog::error("Painting in progress not saved: {}", err);
				Art::Notify("SkyrimPainter: could not save the painting in progress (see SkyrimPainter.log)");
				return;
			}
			if (!Dds::ConvertPixels(px.data(), Art::kPaintingW, Art::kPaintingH, kWipW, kWipH, Art::DataPath(dds), err)) {
				spdlog::error("Easel texture not written: {}", err);
			}
			const auto pngUtf8 = Art::ToUtf8(png);
			SKSE::GetTaskInterface()->AddTask([id, pngUtf8, dds, old = st.wipDds] {
				{
					std::lock_guard lk(g_mtx);
					const auto      it = g_easels.find(id);
					if (it == g_easels.end()) {
						return;  // packed up meanwhile
					}
					it->second.wipPng = pngUtf8;
					it->second.wipDds = dds;
				}
				Art::Show(id, dds);
				Art::DeleteTexture(old);
				spdlog::info("Easel {:08X}: painting in progress saved ({})", id, pngUtf8);
			});
		}).detach();
	}

	void Finish(RE::ObjectRefHandle a_easel, std::vector<std::uint8_t> a_rgba, std::string a_name)
	{
		std::thread([a_easel, px = std::move(a_rgba), name = std::move(a_name)] {
			Wall::Painting p;
			p.id = Art::NewId();
			p.name = name.empty() ? "Untitled" : name;
			const auto  png = PaintingsFolder() / Art::FromUtf8(p.id + ".png");
			std::string err;
			if (!WritePng(png, px, err)) {
				spdlog::error("Painting not saved: {}", err);
				Art::Notify("SkyrimPainter: could not save the painting (see SkyrimPainter.log)");
				return;
			}
			p.png = Art::ToUtf8(png);
			// the wall texture, ready before it's hung
			std::string derr;
			Dds::ConvertPixels(px.data(), Art::kPaintingW, Art::kPaintingH, Art::kPaintingW, Art::kPaintingH, Art::DataPath(Wall::DdsFor(p.id)), derr);
			SKSE::GetTaskInterface()->AddTask([a_easel, p] {
				{
					std::lock_guard lk(g_mtx);
					g_unhung.push_back(p);
				}
				Give(g_painting, 1);
				Give(g_easelItem, 1);
				if (auto ref = a_easel.get()) {
					RemoveEaselRef(ref.get());
				}
				spdlog::info("Finished painting \"{}\" ({})", p.name, p.png);
				Art::Notify(fmt::format("\"{}\" is finished - drop the Painting from your pack to hang it", p.name));
			});
		}).detach();
	}

	void PackUp(RE::ObjectRefHandle a_easel)
	{
		SKSE::GetTaskInterface()->AddTask([a_easel] {
			if (auto ref = a_easel.get()) {
				RemoveEaselRef(ref.get());
				Give(g_easelItem, 1);
				Give(g_canvas, 1);
				Art::Notify("Easel and canvas packed up");
			}
		});
	}

	void Hang(const std::string& a_id)
	{
		SKSE::GetTaskInterface()->AddTask([a_id] {
			std::optional<Wall::Painting> p;
			{
				std::lock_guard lk(g_mtx);
				for (auto it = g_unhung.begin(); it != g_unhung.end(); ++it) {
					if (it->id == a_id) {
						p = *it;
						g_unhung.erase(it);
						break;
					}
				}
			}
			if (p) {
				Wall::BeginPlacement(*p);
			} else {
				Give(g_painting, 1);
			}
		});
	}

	void CancelPick()
	{
		SKSE::GetTaskInterface()->AddTask([] { Give(g_painting, 1); });
	}

	void ReturnPainting(const Wall::Painting& a_painting)
	{
		{
			std::lock_guard lk(g_mtx);
			g_unhung.push_back(a_painting);
		}
		Give(g_painting, 1);
	}

	void RequestActivate()
	{
		SKSE::GetTaskInterface()->AddTask([] {
			if (!TryEasel()) {
				Wall::Examine(true);
			}
		});
	}

	void InstallSerialization()
	{
		const auto* ser = SKSE::GetSerializationInterface();
		ser->SetUniqueID(kSerializationID);
		ser->SetSaveCallback(OnSave);
		ser->SetLoadCallback(OnLoad);
		ser->SetRevertCallback(OnRevert);
	}

	void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
	{
		const auto len = static_cast<std::uint16_t>((std::min)(a_s.size(), std::size_t{ 0xFFFF }));
		a_intfc->WriteRecordData(len);
		a_intfc->WriteRecordData(a_s.data(), len);
	}

	bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& a_s)
	{
		std::uint16_t len = 0;
		if (a_intfc->ReadRecordData(len) != sizeof(len)) {
			return false;
		}
		a_s.resize(len);
		return len == 0 || a_intfc->ReadRecordData(a_s.data(), len) == len;
	}
}
