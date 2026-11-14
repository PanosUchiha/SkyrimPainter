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
#include "Forms.h"
#include "Place.h"
#include "Store.h"

namespace fs = std::filesystem;

namespace Items
{
	namespace
	{
		constexpr std::uint32_t kSerializationID = 'SKPT';
		constexpr std::uint32_t kRecordSlots = 'SLOT';
		constexpr std::uint32_t kRecordEasels = 'EASL';
		constexpr int           kWipLong = 512;  // the painting in progress on the easel's canvas
		constexpr int           kWipShort = 384;

		RE::TESObjectMISC* g_canvas = nullptr;
		RE::TESObjectMISC* g_palette = nullptr;
		RE::TESObjectMISC* g_paints = nullptr;
		RE::TESObjectBOOK* g_easelBook = nullptr;
		RE::TESObjectACTI* g_stand = nullptr;
		RE::TESObjectACTI* g_easelCanvas[2]{};
		std::array<RE::TESObjectBOOK*, Forms::kPaintingSlots> g_books{};

		struct Slot
		{
			bool          used = false;
			std::string   id;  // folder under Paintings (unique, so saves never share files)
			std::string   name;
			bool          portrait = false;
			std::uint32_t version = 0;
		};

		struct Easel
		{
			RE::FormID  canvas = 0;  // canvas reference on it (0 = none)
			bool        portrait = false;
			int         slot = -1;   // editing this finished painting
			std::string workId;      // folder under InProgress
			std::string wipDds;      // texture on the canvas while it is being painted
			std::string name;
			bool        painted = false;
		};

		std::mutex                              g_mtx;  // guards the state below
		std::array<Slot, Forms::kPaintingSlots> g_slots;
		std::unordered_map<RE::FormID, Easel>   g_easels;  // stand -> state
		std::optional<EaselMenuInfo>            g_menuRequest;
		std::optional<Work>                     g_studioRequest;

		std::string SlotDds(const Slot& a_s)
		{
			return fmt::format("textures\\SkyrimPainter\\Paintings\\{}_{}.dds", a_s.id, a_s.version);
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

		void Give(RE::TESBoundObject* a_form, int a_count = 1)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && a_form && a_count > 0) {
				player->AddObjectToContainer(a_form, nullptr, a_count, nullptr);
			}
		}

		void Take(RE::TESBoundObject* a_form, int a_count = 1)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && a_form && a_count > 0) {
				player->RemoveItem(a_form, a_count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			}
		}

		int SlotOfBook(const RE::TESForm* a_form)
		{
			for (int i = 0; i < Forms::kPaintingSlots; ++i) {
				if (a_form && g_books[i] == a_form) {
					return i;
				}
			}
			return -1;
		}

		// The painting items carry the painting's name. Main thread.
		void NameBook(int a_slot)
		{
			if (a_slot < 0 || a_slot >= Forms::kPaintingSlots || !g_books[a_slot]) {
				return;
			}
			std::string name;
			{
				std::lock_guard lk(g_mtx);
				name = g_slots[a_slot].used ? g_slots[a_slot].name : std::string("Painting");
			}
			g_books[a_slot]->fullName = name.c_str();
		}

		fs::path WorkDir(const std::string& a_id) { return Store::WorkFolder() / Art::FromUtf8(a_id); }
		fs::path SlotDir(const std::string& a_id) { return Store::PaintingsFolder() / Art::FromUtf8(a_id); }

		void DeleteRef(RE::FormID a_id)
		{
			if (auto* ref = a_id ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id) : nullptr) {
				Art::Forget(a_id);
				ref->Disable();
				ref->SetDelete(true);
			}
		}

		// The canvas reference sits exactly where the stand is, turned the same way. Main thread.
		RE::FormID SpawnCanvas(RE::TESObjectREFR* a_stand, bool a_portrait)
		{
			auto* base = g_easelCanvas[a_portrait ? 1 : 0];
			if (!a_stand || !base) {
				return 0;
			}
			auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(
				base, a_stand->GetPosition(), a_stand->GetAngle(), a_stand->GetParentCell(), a_stand->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), true, true);
			auto ref = handle.get();
			return ref ? ref->GetFormID() : 0;
		}

		RE::TESObjectREFR* StandOf(RE::TESObjectREFR* a_ref)  // the stand, for a stand or a canvas on an easel
		{
			if (!a_ref) {
				return nullptr;
			}
			std::lock_guard lk(g_mtx);
			if (g_easels.contains(a_ref->GetFormID())) {
				return a_ref;
			}
			for (const auto& [id, e] : g_easels) {
				if (e.canvas == a_ref->GetFormID()) {
					return RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				}
			}
			return nullptr;
		}

		void RequestEaselMenu(RE::TESObjectREFR* a_stand)  // main thread
		{
			EaselMenuInfo info;
			info.easel = a_stand->GetHandle();
			{
				std::lock_guard lk(g_mtx);
				const auto      it = g_easels.find(a_stand->GetFormID());
				if (it == g_easels.end()) {
					return;
				}
				const auto& e = it->second;
				info.hasCanvas = e.canvas != 0;
				info.portrait = e.portrait;
				info.slot = e.slot;
				info.name = e.name;
				info.painted = e.painted;
				for (const auto& s : g_slots) {
					info.freeSlots += s.used ? 0 : 1;
				}
			}
			info.haveCanvasItem = CountOf(g_canvas) > 0;
			info.havePalette = CountOf(g_palette) > 0;
			info.havePaints = CountOf(g_paints) > 0;
			for (int i = 0; i < Forms::kPaintingSlots; ++i) {
				SlotView v;
				if (GetSlot(i, v) && CountOf(g_books[i]) > 0) {
					info.paintingsInPack.emplace_back(i, v.name);
				}
			}
			std::lock_guard lk(g_mtx);
			g_menuRequest = std::move(info);
		}

		void ShowOnCanvas(const Easel& a_e)  // main thread
		{
			if (!a_e.canvas) {
				return;
			}
			if (!a_e.wipDds.empty()) {
				Art::EnsureFromPng(a_e.wipDds, WorkDir(a_e.workId) / L"flat.png", a_e.portrait ? kWipShort : kWipLong, a_e.portrait ? kWipLong : kWipShort);
				Art::Show(a_e.canvas, a_e.wipDds);
			} else if (a_e.slot >= 0) {
				SlotView v;
				if (GetSlot(a_e.slot, v)) {
					EnsureSlotTexture(a_e.slot);
					Art::Show(a_e.canvas, v.dds);
				}
			}
		}

		// Runs a_fn(stand, state) on the main thread and stores the state it leaves.
		template <class F>
		void WithEasel(RE::ObjectRefHandle a_easel, F a_fn)
		{
			SKSE::GetTaskInterface()->AddTask([a_easel, fn = std::move(a_fn)]() mutable {
				auto stand = a_easel.get();
				if (!stand) {
					return;
				}
				Easel e;
				{
					std::lock_guard lk(g_mtx);
					const auto      it = g_easels.find(stand->GetFormID());
					if (it == g_easels.end()) {
						return;
					}
					e = it->second;
				}
				fn(stand.get(), e);
				std::lock_guard lk(g_mtx);
				if (auto it = g_easels.find(stand->GetFormID()); it != g_easels.end()) {
					it->second = e;
				}
			});
		}

		void ClearCanvas(Easel& a_e)  // main thread
		{
			DeleteRef(a_e.canvas);
			Art::DeleteTexture(a_e.wipDds);
			if (!a_e.workId.empty()) {
				const auto dir = WorkDir(a_e.workId);
				std::thread([dir] { Store::RemoveDoc(dir); }).detach();
			}
			a_e = Easel{};
		}

		void DeleteTextureLater(std::string a_dds)
		{
			if (a_dds.empty()) {
				return;
			}
			std::thread([a_dds] {
				std::this_thread::sleep_for(std::chrono::seconds(3));
				Art::DeleteTexture(a_dds);
			}).detach();
		}

		// ---- events
		class EventSink final :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || !a_event->opening) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					Place::Cancel(false);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->menuName != RE::BookMenu::MENU_NAME) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto*     book = RE::BookMenu::GetTargetForm();
				const int slot = SlotOfBook(book);
				if (!book || (book != g_easelBook && slot < 0)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// "Reading" our item: close the book (and the inventory behind it) and start placing.
				const bool inWorld = RE::BookMenu::GetTargetReference() != nullptr;
				if (auto* q = RE::UIMessageQueue::GetSingleton()) {
					q->AddMessage(RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					if (!inWorld) {
						q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
						q->AddMessage(RE::FavoritesMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
						q->AddMessage(RE::TweenMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					}
				}
				if (inWorld) {
					Art::Notify("Pick it up first, then use it from your inventory");
					return RE::BSEventNotifyControl::kContinue;
				}
				SKSE::GetTaskInterface()->AddTask([slot] {
					if (Place::IsPlacing()) {
						Place::Cancel(false);
					}
					if (slot < 0) {
						Place::BeginEasel();
						return;
					}
					SlotView v;
					if (!GetSlot(slot, v)) {
						Art::Notify("This canvas has nothing on it");
						return;
					}
					Place::BeginPainting(slot);
				});
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				if (!a_event || !a_event->objectActivated || !a_event->actionRef || a_event->actionRef.get() != RE::PlayerCharacter::GetSingleton()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto  target = a_event->objectActivated;
				auto* base = target->GetBaseObject();
				if (Place::IsWallPainting(base)) {
					Place::OpenMenu(target.get());
				} else if (base && (base == g_stand || base == g_easelCanvas[0] || base == g_easelCanvas[1])) {
					auto handle = target->GetHandle();
					SKSE::GetTaskInterface()->AddTask([handle] {
						if (auto ref = handle.get()) {
							if (auto* stand = StandOf(ref.get())) {
								RequestEaselMenu(stand);
							}
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EventSink g_sink;

		// ---- co-save
		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			{
				std::lock_guard lk(g_mtx);
				if (a_intfc->OpenRecord(kRecordSlots, 1)) {
					a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_slots.size()));
					for (const auto& s : g_slots) {
						a_intfc->WriteRecordData(static_cast<std::uint8_t>(s.used));
						WriteString(a_intfc, s.id);
						WriteString(a_intfc, s.name);
						a_intfc->WriteRecordData(static_cast<std::uint8_t>(s.portrait));
						a_intfc->WriteRecordData(s.version);
					}
				}
				if (a_intfc->OpenRecord(kRecordEasels, 1)) {
					a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_easels.size()));
					for (const auto& [id, e] : g_easels) {
						a_intfc->WriteRecordData(id);
						a_intfc->WriteRecordData(e.canvas);
						a_intfc->WriteRecordData(static_cast<std::uint8_t>(e.portrait));
						a_intfc->WriteRecordData(static_cast<std::int32_t>(e.slot));
						WriteString(a_intfc, e.workId);
						WriteString(a_intfc, e.wipDds);
						WriteString(a_intfc, e.name);
						a_intfc->WriteRecordData(static_cast<std::uint8_t>(e.painted));
					}
				}
			}
			Place::Save(a_intfc);
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0, version = 0, length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == kRecordSlots && version == 1) {
					std::uint32_t n = 0;
					a_intfc->ReadRecordData(n);
					std::lock_guard lk(g_mtx);
					for (std::uint32_t i = 0; i < n; ++i) {
						Slot         s;
						std::uint8_t used = 0, portrait = 0;
						if (a_intfc->ReadRecordData(used) != 1 || !ReadString(a_intfc, s.id) || !ReadString(a_intfc, s.name) ||
							a_intfc->ReadRecordData(portrait) != 1 || a_intfc->ReadRecordData(s.version) != sizeof(s.version)) {
							break;
						}
						s.used = used != 0;
						s.portrait = portrait != 0;
						if (i < g_slots.size()) {
							g_slots[i] = s;
						}
					}
				} else if (type == kRecordEasels && version == 1) {
					std::uint32_t n = 0;
					a_intfc->ReadRecordData(n);
					std::lock_guard lk(g_mtx);
					for (std::uint32_t i = 0; i < n; ++i) {
						RE::FormID   stand = 0, canvas = 0;
						Easel        e;
						std::uint8_t portrait = 0, painted = 0;
						std::int32_t slot = -1;
						if (a_intfc->ReadRecordData(stand) != sizeof(stand) || a_intfc->ReadRecordData(canvas) != sizeof(canvas) ||
							a_intfc->ReadRecordData(portrait) != 1 || a_intfc->ReadRecordData(slot) != sizeof(slot) || !ReadString(a_intfc, e.workId) ||
							!ReadString(a_intfc, e.wipDds) || !ReadString(a_intfc, e.name) || a_intfc->ReadRecordData(painted) != 1) {
							break;
						}
						RE::FormID newStand = 0, newCanvas = 0;
						if (!a_intfc->ResolveFormID(stand, newStand)) {
							continue;
						}
						if (canvas && a_intfc->ResolveFormID(canvas, newCanvas)) {
							e.canvas = newCanvas;
						}
						e.portrait = portrait != 0;
						e.slot = slot;
						e.painted = painted != 0;
						g_easels[newStand] = e;
					}
				} else {
					Place::Load(a_intfc, type, version);
				}
			}
			spdlog::info("Co-save loaded: {} easel(s)", g_easels.size());
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			{
				std::lock_guard lk(g_mtx);
				g_slots = {};
				g_easels.clear();
				g_menuRequest.reset();
				g_studioRequest.reset();
			}
			Art::ForgetAll();
			Place::Revert();
		}
	}

	bool Init()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return false;
		}
		g_canvas = dh->LookupForm<RE::TESObjectMISC>(Forms::kCanvas, Forms::kPlugin);
		g_palette = dh->LookupForm<RE::TESObjectMISC>(Forms::kPalette, Forms::kPlugin);
		g_paints = dh->LookupForm<RE::TESObjectMISC>(Forms::kPaints, Forms::kPlugin);
		g_easelBook = dh->LookupForm<RE::TESObjectBOOK>(Forms::kEasel, Forms::kPlugin);
		g_stand = dh->LookupForm<RE::TESObjectACTI>(Forms::kEaselStand, Forms::kPlugin);
		for (int i = 0; i < 2; ++i) {
			g_easelCanvas[i] = dh->LookupForm<RE::TESObjectACTI>(Forms::kEaselCanvas[i], Forms::kPlugin);
		}
		int books = 0;
		for (int i = 0; i < Forms::kPaintingSlots; ++i) {
			g_books[i] = dh->LookupForm<RE::TESObjectBOOK>(Forms::kPaintingFirst + i, Forms::kPlugin);
			books += g_books[i] ? 1 : 0;
		}
		const bool ok = g_canvas && g_palette && g_paints && g_easelBook && g_stand && g_easelCanvas[0] && g_easelCanvas[1] && books == Forms::kPaintingSlots;
		spdlog::info("Items: canvas {} palette {} paints {} easel {} stand {} canvases {}/{} painting slots {}/{}", g_canvas != nullptr, g_palette != nullptr,
			g_paints != nullptr, g_easelBook != nullptr, g_stand != nullptr, g_easelCanvas[0] != nullptr, g_easelCanvas[1] != nullptr, books, Forms::kPaintingSlots);

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
		}

		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink);
		}
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESActivateEvent>(&g_sink);
		}
		return ok;
	}

	void OnGameLoaded()
	{
		for (int i = 0; i < Forms::kPaintingSlots; ++i) {
			NameBook(i);
		}
		std::vector<Easel> easels;
		{
			std::lock_guard lk(g_mtx);
			for (const auto& [id, e] : g_easels) {
				easels.push_back(e);
			}
		}
		for (const auto& e : easels) {
			ShowOnCanvas(e);
		}
	}

	void InstallSerialization()
	{
		const auto* ser = SKSE::GetSerializationInterface();
		ser->SetUniqueID(kSerializationID);
		ser->SetSaveCallback(OnSave);
		ser->SetLoadCallback(OnLoad);
		ser->SetRevertCallback(OnRevert);
	}

	bool GetSlot(int a_slot, SlotView& a_out)
	{
		std::lock_guard lk(g_mtx);
		if (a_slot < 0 || a_slot >= Forms::kPaintingSlots || !g_slots[a_slot].used) {
			return false;
		}
		const auto& s = g_slots[a_slot];
		a_out.name = s.name;
		a_out.portrait = s.portrait;
		a_out.dds = SlotDds(s);
		a_out.dir = SlotDir(s.id);
		return true;
	}

	void RenamePainting(int a_slot, std::string a_name)
	{
		while (!a_name.empty() && std::isspace(static_cast<unsigned char>(a_name.back()))) {
			a_name.pop_back();
		}
		if (a_name.empty() || a_slot < 0 || a_slot >= Forms::kPaintingSlots) {
			return;
		}
		{
			std::lock_guard lk(g_mtx);
			if (!g_slots[a_slot].used) {
				return;
			}
			g_slots[a_slot].name = a_name;  // kept in the co-save with the slot
		}
		SKSE::GetTaskInterface()->AddTask([a_slot] { NameBook(a_slot); });
		spdlog::info("Items: painting {} renamed to \"{}\"", a_slot, a_name);
	}

	void EnsureSlotTexture(int a_slot)
	{
		Slot s;
		{
			std::lock_guard lk(g_mtx);
			if (a_slot < 0 || a_slot >= Forms::kPaintingSlots || !g_slots[a_slot].used) {
				return;
			}
			s = g_slots[a_slot];
		}
		Art::EnsureFromPng(SlotDds(s), SlotDir(s.id) / L"flat.png", Art::Width(s.portrait), Art::Height(s.portrait));
	}

	void TakePaintingItem(int a_slot)
	{
		if (a_slot >= 0 && a_slot < Forms::kPaintingSlots) {
			Take(g_books[a_slot]);
		}
	}

	void GivePaintingItem(int a_slot)
	{
		if (a_slot >= 0 && a_slot < Forms::kPaintingSlots) {
			NameBook(a_slot);
			Give(g_books[a_slot]);
		}
	}

	RE::TESObjectACTI* EaselStandForm() { return g_stand; }

	void EaselPlaced(RE::TESObjectREFR* a_stand)
	{
		Take(g_easelBook);
		{
			std::lock_guard lk(g_mtx);
			g_easels[a_stand->GetFormID()] = Easel{};
		}
		spdlog::info("Easel {:08X} set up", a_stand->GetFormID());
		Art::Notify("Easel set up - activate it to put a canvas on");
	}

	std::optional<EaselMenuInfo> TakeEaselMenuRequest()
	{
		std::lock_guard lk(g_mtx);
		auto            out = std::move(g_menuRequest);
		g_menuRequest.reset();
		return out;
	}

	void SetCanvas(RE::ObjectRefHandle a_easel, bool a_portrait)
	{
		WithEasel(a_easel, [a_portrait](RE::TESObjectREFR* a_stand, Easel& e) {
			if (e.canvas) {
				return;
			}
			if (CountOf(g_canvas) <= 0) {
				Art::Notify("You need a Blank Canvas");
				return;
			}
			Take(g_canvas);
			e = Easel{};
			e.portrait = a_portrait;
			e.workId = Art::NewId();
			e.canvas = SpawnCanvas(a_stand, a_portrait);
		});
	}

	void EditPainting(RE::ObjectRefHandle a_easel, int a_slot)
	{
		WithEasel(a_easel, [a_slot](RE::TESObjectREFR* a_stand, Easel& e) {
			Slot s;
			{
				std::lock_guard lk(g_mtx);
				if (e.canvas || a_slot < 0 || a_slot >= Forms::kPaintingSlots || !g_slots[a_slot].used) {
					return;
				}
				s = g_slots[a_slot];
			}
			if (CountOf(g_books[a_slot]) <= 0) {
				Art::Notify("That painting isn't in your pack");
				return;
			}
			Take(g_books[a_slot]);
			e = Easel{};
			e.portrait = s.portrait;
			e.slot = a_slot;
			e.name = s.name;
			e.painted = true;
			e.workId = Art::NewId();
			e.canvas = SpawnCanvas(a_stand, s.portrait);
			const auto from = SlotDir(s.id), to = WorkDir(e.workId);
			std::thread([from, to] { Store::CopyDoc(from, to); }).detach();
			ShowOnCanvas(e);
		});
	}

	void TurnCanvas(RE::ObjectRefHandle a_easel)
	{
		WithEasel(a_easel, [a_easel](RE::TESObjectREFR*, Easel& e) {
			if (!e.canvas) {
				return;
			}
			const bool toPortrait = !e.portrait;
			const auto dir = WorkDir(e.workId);
			const bool waitForCopy = e.slot >= 0;
			// turn the pictures on disk first, then swap the canvas on the easel
			std::thread([a_easel, dir, toPortrait, waitForCopy] {
				if (waitForCopy && !Store::HasDoc(dir)) {
					std::this_thread::sleep_for(std::chrono::milliseconds(500));  // a painting's copy may still be on the way
				}
				Store::RotateDoc(dir, toPortrait);
				auto flat = std::make_shared<std::vector<std::uint8_t>>();
				int  w = 0, h = 0;
				const bool haveFlat = Store::LoadFlat(dir, *flat, w, h);
				WithEasel(a_easel, [toPortrait, haveFlat, flat, w, h](RE::TESObjectREFR* a_stand, Easel& e) {
					DeleteRef(e.canvas);
					e.portrait = toPortrait;
					e.canvas = SpawnCanvas(a_stand, toPortrait);
					DeleteTextureLater(e.wipDds);
					e.wipDds.clear();
					if (haveFlat) {
						e.wipDds = fmt::format("textures\\SkyrimPainter\\InProgress\\{}_{}.dds", e.workId, Art::NewId());
						Art::ConvertAndShow(std::move(*flat), w, h, e.wipDds, toPortrait ? kWipShort : kWipLong, toPortrait ? kWipLong : kWipShort, e.canvas);
					}
				});
			}).detach();
		});
	}

	void TakeCanvasOff(RE::ObjectRefHandle a_easel)
	{
		WithEasel(a_easel, [](RE::TESObjectREFR*, Easel& e) {
			if (!e.canvas) {
				return;
			}
			const int slot = e.slot;
			ClearCanvas(e);
			if (slot >= 0) {
				GivePaintingItem(slot);  // the painting as it was finished (later changes are dropped)
				Art::Notify("Painting taken off the easel");
			} else {
				Give(g_canvas);
				Art::Notify("Canvas scraped clean and taken off the easel");
			}
		});
	}

	void ScrapePainting(RE::ObjectRefHandle a_easel)
	{
		WithEasel(a_easel, [](RE::TESObjectREFR*, Easel& e) {
			if (!e.canvas || e.slot < 0) {
				return;
			}
			const int slot = e.slot;
			Slot      s;
			{
				std::lock_guard lk(g_mtx);
				s = g_slots[slot];
				g_slots[slot] = Slot{};
			}
			NameBook(slot);
			DeleteTextureLater(SlotDds(s));
			const auto dir = SlotDir(s.id);
			std::thread([dir] { Store::RemoveDoc(dir); }).detach();
			ClearCanvas(e);
			Give(g_canvas);
			Art::Notify("Painting scraped off - you have a blank canvas again");
		});
	}

	void PickUpEasel(RE::ObjectRefHandle a_easel)
	{
		SKSE::GetTaskInterface()->AddTask([a_easel] {
			auto stand = a_easel.get();
			if (!stand) {
				return;
			}
			{
				std::lock_guard lk(g_mtx);
				const auto      it = g_easels.find(stand->GetFormID());
				if (it == g_easels.end() || it->second.canvas) {
					return;
				}
				g_easels.erase(it);
			}
			DeleteRef(stand->GetFormID());
			Give(g_easelBook);
			Art::Notify("Easel folded up");
		});
	}

	void Paint(RE::ObjectRefHandle a_easel)
	{
		WithEasel(a_easel, [a_easel](RE::TESObjectREFR*, Easel& e) {
			if (!e.canvas) {
				return;
			}
			if (CountOf(g_palette) <= 0 || CountOf(g_paints) <= 0) {
				Art::Notify("You need a Painter's Palette and Paints to paint");
				return;
			}
			std::lock_guard lk(g_mtx);
			g_studioRequest = Work{ a_easel, WorkDir(e.workId), e.portrait, e.slot, e.name, e.canvas };
		});
	}

	std::optional<Work> TakeStudioRequest()
	{
		std::lock_guard lk(g_mtx);
		auto            out = std::move(g_studioRequest);
		g_studioRequest.reset();
		return out;
	}

	void StepAway(RE::ObjectRefHandle a_easel, std::vector<std::uint8_t> a_flat, int a_w, int a_h)
	{
		auto flat = std::make_shared<std::vector<std::uint8_t>>(std::move(a_flat));
		WithEasel(a_easel, [flat, a_w, a_h](RE::TESObjectREFR*, Easel& e) {
			if (!e.canvas) {
				return;
			}
			DeleteTextureLater(e.wipDds);
			// a new name each time: the game keeps textures by name
			e.wipDds = fmt::format("textures\\SkyrimPainter\\InProgress\\{}_{}.dds", e.workId, Art::NewId());
			e.painted = true;
			const bool portrait = a_h > a_w;
			Art::ConvertAndShow(std::move(*flat), a_w, a_h, e.wipDds, portrait ? kWipShort : kWipLong, portrait ? kWipLong : kWipShort, e.canvas);
		});
	}

	void ShowLive(RE::ObjectRefHandle a_easel, std::string a_dds, std::vector<std::uint8_t> a_flat, int a_w, int a_h)
	{
		auto flat = std::make_shared<std::vector<std::uint8_t>>(std::move(a_flat));
		WithEasel(a_easel, [flat, dds = std::move(a_dds), a_w, a_h](RE::TESObjectREFR*, Easel& e) {
			if (!e.canvas) {
				return;
			}
			DeleteTextureLater(e.wipDds);
			e.wipDds = dds;  // unique to this session: nothing else in the game uses it
			const bool portrait = a_h > a_w;
			Art::ConvertAndShow(std::move(*flat), a_w, a_h, e.wipDds, portrait ? kWipShort : kWipLong, portrait ? kWipLong : kWipShort, e.canvas);
		});
	}

	void Finish(RE::ObjectRefHandle a_easel, std::string a_name, std::vector<std::uint8_t> a_flat, int a_w, int a_h)
	{
		auto flat = std::make_shared<std::vector<std::uint8_t>>(std::move(a_flat));
		SKSE::GetTaskInterface()->AddTask([a_easel, name = std::move(a_name), flat, a_w, a_h] {
			auto stand = a_easel.get();
			if (!stand) {
				return;
			}
			Easel       e;
			int         slot = -1;
			Slot        s;
			std::string oldDds;
			{
				std::lock_guard lk(g_mtx);
				const auto      it = g_easels.find(stand->GetFormID());
				if (it == g_easels.end() || !it->second.canvas) {
					return;
				}
				e = it->second;
				slot = e.slot;
				if (slot < 0) {
					for (int i = 0; i < Forms::kPaintingSlots; ++i) {
						if (!g_slots[i].used) {
							slot = i;
							break;
						}
					}
				}
				if (slot < 0) {
					Art::Notify("You have 50 paintings already - scrape one off at an easel to finish another");
					return;
				}
				s = g_slots[slot];
				if (s.used) {
					oldDds = SlotDds(s);
				} else {
					s = Slot{ true, Art::NewId(), {}, false, 0 };
				}
				s.name = name.empty() ? std::string("Untitled") : name;
				s.portrait = a_h > a_w;
				++s.version;
				g_slots[slot] = s;     // reserved now
				it->second = Easel{};  // the easel stands empty again
			}
			DeleteRef(e.canvas);
			DeleteTextureLater(e.wipDds);
			DeleteTextureLater(oldDds);
			// files: the layers move to the painting's own folder, the DDS is written at full size
			const auto work = WorkDir(e.workId), dest = SlotDir(s.id);
			const auto dds = SlotDds(s);
			std::thread([work, dest, dds, flat, a_w, a_h, slot, s] {
				Store::CopyDoc(work, dest);
				Store::RemoveDoc(work);
				Art::ConvertNow(flat->data(), a_w, a_h, dds, a_w, a_h);
				SKSE::GetTaskInterface()->AddTask([slot, s] {
					GivePaintingItem(slot);
					Place::RefreshSlot(slot);
					spdlog::info("Finished \"{}\" (slot {}, {})", s.name, slot + 1, SlotDds(s));
					Art::Notify(fmt::format("\"{}\" is finished - use it from your inventory to hang it", s.name));
				});
			}).detach();
		});
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
