#include "Combine/Manager.h"

#include "Combine/Gather.h"
#include "Engine/Engine.h"
#include "Settings.h"

namespace RC::Combine::Manager
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr std::uint32_t kFormDeletedOrDisabled = (1u << 5) | (1u << 11);
		constexpr std::size_t   kUpdateSlot = 0xCF;               // Actor::Update(float); PlayerCharacter's runs once per game frame
		constexpr std::uint32_t kNoChildCount = 0xFFFFFFFF;
		constexpr std::size_t   kCreateBytesPerFrame = 16u << 20;  // vertex + index data handed to the renderer per frame

		[[nodiscard]] double Ms(Clock::time_point a_since) noexcept
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - a_since).count();
		}

		// ---- transforms ---------------------------------------------------------------------------------------

		struct World
		{
			float values[13]{};  // rotation 3x3, translate, scale

			World() = default;
			explicit World(const RE::NiTransform& a_t) noexcept
			{
				for (int i = 0; i < 3; ++i) {
					for (int j = 0; j < 3; ++j) {
						values[i * 3 + j] = a_t.rotate.entry[i].pt[j];
					}
				}
				values[9] = a_t.translate.x;
				values[10] = a_t.translate.y;
				values[11] = a_t.translate.z;
				values[12] = a_t.scale;
			}
			[[nodiscard]] bool operator==(const World& a_other) const noexcept
			{
				return std::equal(std::begin(values), std::end(values), std::begin(a_other.values));
			}
		};

		// ---- state --------------------------------------------------------------------------------------------

		// An object hidden because a chunk draws it now: a reference's 3D root, or one of its meshes.
		struct Hidden
		{
			RE::NiPointer<RE::NiAVObject>    object;
			RE::NiPointer<RE::TESObjectREFR> ref;
			RE::NiPointer<RE::NiAVObject>    root;  // the reference's 3D at apply
			std::uint32_t                    formID{ 0 };
			World                            world;
			std::uint32_t                    children{ kNoChildCount };  // a hidden node root's child count
			bool                             unregistered{ false };  // taken out of previs's dynamic objects while hidden
			bool                             culled{ true };  // AppCulled by us; false: a root that owns collision (Apply)
		};

		// What the watchdog found.
		enum class Check
		{
			kOk,
			kStale,     // moved, unloaded, disabled, or shown again by the game
			kAttached,  // something was attached under a hidden root (a decal, an effect)
		};

		struct Container
		{
			RE::NiPointer<RE::NiNode> node;
			RE::NiNode*               parent{ nullptr };
		};

		constexpr std::uint8_t kNotAlone = 0xFF;  // ChunkRecord::alone of a combined chunk or a not-mergeable solo

		// One applied chunk (the container holds it): for previs and the summary's audit.
		struct ChunkRecord
		{
			RE::NiPointer<RE::NiAVObject> shape;
			RE::BSFadeNode*               fadeNode{ nullptr };  // its own fade node (bChunkFadeNodes), held by the container
			std::vector<std::uint32_t>    refs;                 // form IDs of the references it draws meshes of
			std::uint32_t                 members{ 0 };
			std::uint32_t                 triangles{ 0 };
			std::uint32_t                 group{ 0 };  // index into Applied::groups: the node it hangs under
			std::uint8_t                  alone{ kNotAlone };  // a mergeable solo: why it stayed alone (Alone)

			// What a view's culling group gets, and what previs tests: the fade node, or the mesh itself.
			[[nodiscard]] RE::NiAVObject* Fed() const noexcept { return fadeNode ? static_cast<RE::NiAVObject*>(fadeNode) : shape.get(); }
		};

		// The chunks of one chunk-grid square under one parent hang under a plain node (a container, straight under
		// the parent), and that node is what previs tests: its query recurses into a dynamic object's children only
		// when the object's bound is visible, so an off-screen square costs one test per pass instead of one per chunk
		// (FO4-ENGINE-NOTES 5.5d).
		struct ChunkGroup
		{
			RE::NiPointer<RE::NiNode> node;  // also in Applied::containers
			std::uint64_t             previsVisible{ 0 };   // the tick before the last query that found it visible
			bool                      registered{ false };  // a previs dynamic object (Engine::RegisterPrevisObject)
		};

		// Roots hidden whole wait under one hidden node per parent, hung on the cell node beside that parent: the scene
		// walk reads only the cell node's children 2, 3 and 9 (FO4-ENGINE-NOTES 5.3), so the walk and CBRO's cell-node
		// scans no longer step over each one every frame (~33k in 25 cells, run 28). The engine takes a root off its
		// node through the root's own parent or drops the whole cell node, and AttachChild takes a child off its old
		// parent first (7.13), so a parked root goes wherever the engine puts it.
		struct Parking
		{
			RE::NiPointer<RE::NiNode>                  node;
			RE::NiPointer<RE::NiNode>                  home;  // the roots' parent, where they go back
			std::vector<RE::NiPointer<RE::NiAVObject>> roots;
		};

		struct Applied
		{
			std::vector<Container>   containers;
			std::vector<ChunkGroup>  groups;
			std::vector<ChunkRecord> chunks;
			std::vector<Hidden>      hidden;
			std::vector<Parking>     parking;
			std::uint64_t            frame{ 0 };         // applied at this tick
			bool                     fallback{ false };  // originals shown again and chunks hidden: revert pending
			bool                     previsPending{ false };  // applied under the loading menu: roots leave previs later (TakeOutLoaded)
		};

		// Hangs beside a parent's containers and tells whether the main view draws what hangs there (Engine::NewProbe).
		// It stays while the cell is attached, through reverts, so it also tells when drawing resumes.
		struct Probe
		{
			RE::NiPointer<RE::BSFadeNode> node;
			RE::NiNode*                   parent{ nullptr };
			std::uint32_t                 seen{ 0 };    // consecutive ticks after a frame that drew it
			std::uint32_t                 unseen{ 0 };  // consecutive ticks after a frame that didn't
		};

		struct CellState
		{
			std::uint32_t            formID{ 0 };
			std::uint64_t            generation{ 0 };
			std::uint64_t            lastChange{ 0 };
			bool                     dirty{ true };
			bool                     building{ false };
			bool                     blind{ false };  // the main view doesn't draw the probes' groups: originals only
			bool                     sized{ false };  // sizing holds the player's position at the last gather
			Bake::Sizing             sizing;
			std::unique_ptr<Applied> applied;
			std::vector<Probe>       probes;
			std::vector<std::pair<const char*, std::uint32_t>> blocked;  // the last gather's blocked parts by class
		};

		struct Totals
		{
			std::uint64_t applies{ 0 };
			std::uint64_t reverts{ 0 };
			std::uint64_t discarded{ 0 };
			std::uint64_t watchdog{ 0 };
			std::uint64_t attached{ 0 };
			std::uint64_t blind{ 0 };  // reverts because the main view stopped drawing a cell's chunks
		};

		std::unordered_map<RE::TESObjectCELL*, CellState>     g_cells;
		std::unordered_map<std::uint32_t, RE::TESObjectCELL*> g_hiddenRefs;  // formID -> cell
		Gather::HiddenSet                                     g_ours;
		Gather::ParkedMap                                     g_parked;  // Parking::node -> Parking::home
		Gather::FormSet                                       g_meshByMesh;  // references whose root stays visible
		std::vector<std::unique_ptr<Job>>                     g_jobs;  // submitted: baking, or waiting for GPU buffers
		Totals                                                g_totals;

		std::uint64_t g_frame{ 0 };
		bool          g_active{ true };     // the on/off switch
		bool          g_workshop{ false };  // workshop mode shows the originals (highlights, scrapping)
		bool          g_halted{ false };    // a self-check failed: nothing more is combined
		bool          g_previsPaused{ false };  // previs active and no query hook: originals shown
		bool          g_cbroLoaded{ false };    // CBRO is loaded (read at the first tick)
		bool          g_previsHook{ false };    // the previs query hook is in (Engine::InstallPrevisQueryHook)
		bool          g_resetHook{ false };     // the game-reset hook is in (Engine::InstallGameResetHook)
		bool          g_configuredPrecombines{ true };  // the switch as the game's INIs set it (read at kGameDataReady)
		// What F3 wants the precombine switch to be, written at the next save load (-1: no change). Only then is no
		// cell loaded under the old value (FO4-ENGINE-NOTES 7.12).
		std::atomic<int> g_precombinesNext{ -1 };
		bool          g_keyDown{ false };
		std::size_t   g_watchCursor{ 0 };
		std::atomic<DWORD> g_mainThread{ 0 };  // the thread running PlayerCharacter::Update (set by the first tick)
		// Held by the tick, the previs callback, a direct object event and the loading-menu pass (LoadPass), which runs
		// on whatever thread closes the menu (the loader for a save load). The frame's callbacks only try it: a frame
		// that finds it held skips RC's work rather than wait. Recursive: an object event inside the tick is direct.
		std::recursive_timed_mutex g_stateMutex;
		std::atomic<bool>  g_resetPending{ false };
		std::uintptr_t g_originalUpdate{ 0 };
		Clock::time_point g_lastSummary{};
		constexpr auto    kSummaryInterval = std::chrono::seconds(10);
		// The size line and the census walk every attached cell: a ~6 ms tick each summary (run 38). They are logged
		// again only once the scene changed or the camera moved kCensusMove units.
		constexpr float   kCensusMove = 1000.0F;
		std::uint64_t     g_censusVersion{ ~0ull };
		RE::NiPoint3      g_censusEye{};

		// Frame times between ticks (gaps over a second, loading screens, left out) and the tick's own cost, per
		// summary interval: the log's FPS for each state of the A/B.
		struct FrameTimes
		{
			std::uint32_t frames{ 0 };
			double        frameMs{ 0.0 };
			double        frameMax{ 0.0 };
			double        tickMs{ 0.0 };
			double        tickMax{ 0.0 };
		};
		FrameTimes        g_frameTimes;
		Clock::time_point g_lastTick{};
		std::uint64_t g_blockedPrinted{ 0 };  // hash of the blocked-class totals last logged

		// Probe hysteresis (ticks): a cell's chunks go after this many frames in a row without a drawn probe, and
		// come back after this many frames in a row with every probe drawn.
		constexpr std::uint32_t kProbeLostTicks = 5;
		constexpr std::uint32_t kProbeBackTicks = 30;

		// ---- previs state (main thread: the tick and Render_PreUI) ----------------------------------------------

		std::uint64_t g_previsTick{ 0 };       // the tick of the last query that ran with previs active
		std::uint64_t g_previsFrames{ 0 };     // queries that ran with previs active, since the last summary
		std::uint32_t g_previsVisible{ 0 };    // chunk groups the last query's main-view pass found visible
		bool          g_previsVisibleRead{ false };  // ... read: previs drew the main view
		bool          g_group0Drawn{ false };  // the last tick saw a probe the main view drew

		// ---- bake thread ---------------------------------------------------------------------------------------

		std::mutex              g_queueMutex;
		std::condition_variable g_queueCv;
		std::deque<Job*>        g_queue;

		void WorkerMain()
		{
			for (;;) {
				Job* job = nullptr;
				{
					std::unique_lock lock{ g_queueMutex };
					g_queueCv.wait(lock, [] { return !g_queue.empty(); });
					job = g_queue.front();
					g_queue.pop_front();
				}
				const auto  start = Clock::now();
				const auto& settings = Settings::Get();
				for (auto& bucket : job->buckets) {
					bucket.chunks = bucket.cloneOnly ? Bake::Solos(bucket.members) :
					                                   Bake::Cluster(bucket.members, job->chunkSize, settings.minShapesPerChunk, settings.clusterSpread,
														   &job->stats.joined, job->sizing);
					for (auto& chunk : bucket.chunks) {
						if (!Bake::Build(bucket.layout, bucket.chunkLayout, bucket.members, chunk, settings.minShapesPerChunk)) {
							chunk.vertexCount = 0;
						}
					}
				}
				job->stats.bakeMs = Ms(start);
				job->baked.store(true, std::memory_order_release);  // the job is the main thread's again
			}
		}

		void Submit(Job* a_job)
		{
			{
				std::scoped_lock lock{ g_queueMutex };
				g_queue.push_back(a_job);
			}
			g_queueCv.notify_one();
		}

		// ---- helpers -------------------------------------------------------------------------------------------

		// Nothing is applied: switched off, workshop mode, halted, or previs drawing the main view.
		[[nodiscard]] bool Paused() noexcept
		{
			return !g_active || g_workshop || g_halted || g_previsPaused;
		}

		[[nodiscard]] bool CulledByGame(const RE::NiAVObject* a_object)
		{
			return a_object->GetAppCulled() && !g_ours.contains(a_object);
		}

		[[nodiscard]] bool IsCombinableBase(const RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref->GetObjectReference();
			return base && (base->Is(RE::ENUM_FORM_ID::kSTAT) || base->Is(RE::ENUM_FORM_ID::kSCOL));
		}

		// HoldsPose's lookup, read without a lock: every attached cell's form id and its precombined references' (cell,
		// reference) pairs, sorted. PollCells rebuilds it whenever the attached cells change (game thread or LoadPass,
		// under g_stateMutex) and swaps the pointer; a replaced table is freed on a later tick once no reader is inside
		// one (g_poseReaders: a reader counts itself in before it loads the pointer). Readers never wait.
		struct PoseTable
		{
			std::vector<std::uint32_t> cells;  // attached cells' form ids
			std::vector<std::uint64_t> refs;   // cell form id << 32 | reference form id
		};
		std::atomic<const PoseTable*>                 g_poseTable{ nullptr };
		std::atomic<std::uint32_t>                    g_poseReaders{ 0 };
		std::vector<std::unique_ptr<const PoseTable>> g_poseRetired;  // under g_stateMutex
		std::atomic<std::uint32_t>                    g_poseLookups{ 0 };
		std::atomic<std::uint32_t>                    g_poseLocked{ 0 };

		void RebuildPoseTable()
		{
			auto                       table = std::make_unique<PoseTable>();
			std::vector<std::uint32_t> ids;
			for (const auto& [cell, state] : g_cells) {
				table->cells.push_back(state.formID);
				ids.clear();
				Engine::CombinedRefIDs(Engine::CombinedRefs(cell), ids);
				for (const auto id : ids) {
					table->refs.push_back(static_cast<std::uint64_t>(state.formID) << 32 | id);
				}
			}
			std::ranges::sort(table->cells);
			std::ranges::sort(table->refs);
			if (const auto old = g_poseTable.exchange(table.release())) {
				g_poseRetired.emplace_back(old);
			}
		}

		void FreeRetiredPoseTables()
		{
			if (!g_poseRetired.empty() && g_poseReaders.load() == 0) {
				g_poseRetired.clear();
			}
		}

		// Engine::InstallPoseHook's question, on an animation job's thread: whether a reference whose model poses its
		// root stays at its placement. Yes for a precombined static while RC keeps the precombines off: the precombines
		// draw it there (FO4-ENGINE-NOTES 7.16), and its own 3D now draws from the first frame as they did, whether or
		// not a chunk replaces it yet (run 44: the pose drew it 8-25x too large until the cell was combined).
		[[nodiscard]] bool HoldsPose(const RE::TESObjectREFR* a_ref)
		{
			if (!Settings::Get().disablePrecombines || Engine::PrecombinesEnabled() || !IsCombinableBase(a_ref)) {
				return false;
			}
			const auto cell = a_ref->GetParentCell();
			if (!cell) {
				return false;
			}
			const std::uint32_t cellID = cell->GetFormID();
			const std::uint64_t key = static_cast<std::uint64_t>(cellID) << 32 | a_ref->GetFormID();
			int                 verdict = -1;  // the cell isn't in the table yet
			g_poseReaders.fetch_add(1);
			if (const auto table = g_poseTable.load(); table && std::ranges::binary_search(table->cells, cellID)) {
				verdict = std::ranges::binary_search(table->refs, key) ? 1 : 0;
			}
			g_poseReaders.fetch_sub(1);
			g_poseLookups.fetch_add(1, std::memory_order_relaxed);
			if (verdict >= 0) {
				return verdict == 1;
			}
			// A cell attached since the last poll (a new exterior cell while walking): the engine's set, behind the
			// extra list's read lock, until PollCells adds the cell.
			g_poseLocked.fetch_add(1, std::memory_order_relaxed);
			return Engine::InCombinedRefs(Engine::CombinedRefs(cell), a_ref->GetFormID());
		}

		void Notify(const std::string& a_text)
		{
			logger::info("{}", a_text);
			if (Settings::Get().notify) {
				RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false, false);  // nullptr: no sound (FO4-ENGINE-NOTES 2.2)
			}
		}

		void MarkChanged(CellState& a_state)
		{
			a_state.dirty = true;
			a_state.lastChange = g_frame;
		}

		// ---- distance sizing ------------------------------------------------------------------------------------
		// Meshes are merged coarser the farther they lie from the player (fChunkGrowth, Bake::Sizing): what the sun's far
		// cascade draws follows how many of RC's objects stand far away (run 35), and up close compact chunks keep the
		// main view's occlusion fine. Per grid cell (run 36) it drew less than the precombines; per distance the sizes
		// change smoothly inside a cell too. A cell is rebuilt once the player has moved enough to change the size at
		// its nearest or farthest point by kResizeRatio (checked every kResizeCheckFrames).
		constexpr std::uint64_t kResizeCheckFrames = 30;
		constexpr float         kResizeRatio = 2.0F;
		constexpr float         kCellUnits = 4096.0F;

		[[nodiscard]] Bake::Sizing CurrentSizing()
		{
			Bake::Sizing sizing;
			const auto   player = RE::PlayerCharacter::GetSingleton();
			if (!player || Engine::InInterior()) {
				return sizing;
			}
			const auto& at = player->data.location;
			sizing.eye[0] = at.x;
			sizing.eye[1] = at.y;
			sizing.eye[2] = at.z;
			sizing.growth = Settings::Get().chunkGrowth;
			return sizing;
		}

		// The size factors at an exterior cell's nearest and farthest point (its square, in x and y), or {1, 1}.
		[[nodiscard]] std::pair<float, float> FactorRange(RE::TESObjectCELL* a_cell, const Bake::Sizing& a_sizing)
		{
			if (a_sizing.growth <= 0.0F || !a_cell || a_cell->IsInterior()) {
				return { 1.0F, 1.0F };
			}
			const float x0 = static_cast<float>(a_cell->GetDataX()) * kCellUnits;
			const float y0 = static_cast<float>(a_cell->GetDataY()) * kCellUnits;
			const float nx = std::clamp(a_sizing.eye[0], x0, x0 + kCellUnits) - a_sizing.eye[0];
			const float ny = std::clamp(a_sizing.eye[1], y0, y0 + kCellUnits) - a_sizing.eye[1];
			const float fx = std::max(std::abs(a_sizing.eye[0] - x0), std::abs(a_sizing.eye[0] - x0 - kCellUnits));
			const float fy = std::max(std::abs(a_sizing.eye[1] - y0), std::abs(a_sizing.eye[1] - y0 - kCellUnits));
			const auto  factor = [&](float a_distance) { return std::clamp(a_distance / a_sizing.growth, 1.0F, Bake::kMaxGrowth); };
			return { factor(std::sqrt(nx * nx + ny * ny)), factor(std::sqrt(fx * fx + fy * fy)) };
		}

		void CheckSizing()
		{
			if (g_frame % kResizeCheckFrames != 0) {
				return;
			}
			// One rebuild at a time, the cell whose sizes are furthest off first, and none while any cell builds (a load):
			// at 1.5x, rebuilds of several cells at once followed every few steps (run 37: 184 applies in ~6 minutes).
			for (const auto& [cell, state] : g_cells) {
				if ((state.dirty && !state.blind) || state.building) {
					return;
				}
			}
			const auto now = CurrentSizing();
			const auto ratio = [](float a_a, float a_b) { return std::max(a_a, a_b) / std::min(a_a, a_b); };
			CellState* worst = nullptr;
			float      worstRatio = kResizeRatio;
			for (auto& [cell, state] : g_cells) {
				if (!state.sized || state.dirty) {
					continue;
				}
				const auto [nearNow, farNow] = FactorRange(cell, now);
				const auto [nearThen, farThen] = FactorRange(cell, state.sizing);
				if (const float off = std::max(ratio(nearNow, nearThen), ratio(farNow, farThen)); off >= worstRatio) {
					worst = &state;
					worstRatio = off;
				}
			}
			if (worst) {
				MarkChanged(*worst);
				logger::info("player moved: cell {:08X}'s chunk sizes are {:.1f}x off, rebuilt", worst->formID, worstRatio);
			}
		}

		// A hidden root goes back into previs's dynamic objects only while it is still its reference's 3D in an
		// attached cell: the engine unregisters a reference's 3D when it unloads or its cell detaches, and the set keeps
		// raw pointers (FO4-ENGINE-NOTES 5.5d).
		void Reregister(Hidden& a_hidden)
		{
			if (!std::exchange(a_hidden.unregistered, false)) {
				return;
			}
			const auto ref = a_hidden.ref.get();
			const auto root = a_hidden.root.get();
			const auto cell = ref ? ref->parentCell : nullptr;
			if (ref && ref->loadedData && ref->loadedData->data3D.get() == root && root->parent && cell &&
				cell->cellState.get() == RE::TESObjectCELL::CELL_STATE::kAttached) {
				(void)Engine::RegisterPrevisObject(root);
			}
		}

		// Moves a_cell's roots hidden whole (still in their parent, exact NiNode children of the cell node) under a
		// hidden node beside their parent. Detached by index: DetachChild scans the children with a reference count
		// taken and dropped on each, and AttachChild's own detach would do that scan per root (7.13).
		void Park(Applied& a_applied, RE::TESObjectCELL* a_cell, const std::vector<RE::NiAVObject*>& a_roots)
		{
			// (appended after child 9, the last child the walk reads, so no role slot of the cell node is taken)
			const auto loaded = a_cell->loadedData;
			const auto cell3D = loaded ? loaded->cell3D.get() : nullptr;
			if (!cell3D || cell3D->children.capacity() <= 9 || !cell3D->children[9]) {
				return;
			}
			std::unordered_map<RE::NiNode*, std::unordered_set<const RE::NiAVObject*>> byHome;
			for (const auto root : a_roots) {
				if (const auto home = root->parent; home && home->parent == cell3D && Engine::IsPlainNode(home)) {
					byHome[home].insert(root);
				}
			}
			for (const auto& [home, roots] : byHome) {
				const auto node = Engine::NewNode(static_cast<std::uint16_t>(std::min<std::size_t>(roots.size(), 0xFFFF)));
				if (!node) {
					continue;
				}
				node->name = RE::BSFixedString("RuntimeCombiner hidden");
				node->local = home->local;
				node->world = home->world;
				node->previousWorld = home->world;
				node->SetAppCulled(true);
				cell3D->AttachChild(node, false);
				auto& parking = a_applied.parking.emplace_back();
				parking.node.reset(node);
				parking.home.reset(home);
				g_parked[node] = home;
				for (auto i = home->children.capacity(); i-- > 0;) {
					if (const auto child = home->children[i].get(); child && roots.contains(child)) {
						RE::NiPointer<RE::NiAVObject> held;
						home->DetachChildAt(i, held);
						node->AttachChild(held.get(), false);
						parking.roots.push_back(std::move(held));
					}
				}
			}
		}

		// Puts the parked roots back in their parent while it still hangs on the same cell node; a root the engine
		// moved or dropped meanwhile is no longer under the parking node and is left alone.
		void Unpark(Applied& a_applied)
		{
			for (auto& parking : a_applied.parking) {
				const auto node = parking.node.get();
				const auto home = parking.home.get();
				const bool homeThere = node && home && home->parent && home->parent == node->parent;
				for (auto i = node ? node->children.capacity() : std::uint16_t{ 0 }; i-- > 0;) {
					if (!node->children[i]) {
						continue;
					}
					RE::NiPointer<RE::NiAVObject> held;
					node->DetachChildAt(i, held);
					if (homeThere && held) {
						home->AttachChild(held.get(), true);
					}
				}
				if (node && node->parent) {
					node->parent->DetachChild(node);
				}
				g_parked.erase(node);
			}
			a_applied.parking.clear();
		}

		// Shows the originals again and hides the chunks (main thread, the tick). Each group and chunk is hidden itself
		// too: previs tests its dynamic objects and their children by their own flag, not their parents' (5.5d).
		// With a_carry, the roots out of previs's dynamic objects stay out and are handed over there (Apply's rebuild).
		void Fallback(CellState& a_state, std::vector<Hidden>* a_carry = nullptr)
		{
			const auto applied = a_state.applied.get();
			if (!applied || applied->fallback) {
				return;
			}
			Unpark(*applied);
			for (auto& container : applied->containers) {
				if (container.node && !container.node->GetAppCulled()) {
					container.node->SetAppCulled(true);
				}
			}
			for (auto& group : applied->groups) {
				if (group.node && !group.node->GetAppCulled()) {
					group.node->SetAppCulled(true);
				}
			}
			for (auto& record : applied->chunks) {
				if (const auto fed = record.Fed(); fed && !fed->GetAppCulled()) {
					fed->SetAppCulled(true);
				}
			}
			for (auto& hidden : applied->hidden) {
				if (hidden.culled && hidden.object && hidden.object->GetAppCulled()) {
					hidden.object->SetAppCulled(false);
				}
				if (a_carry && hidden.unregistered) {
					a_carry->push_back(hidden);
					hidden.unregistered = false;
				} else {
					Reregister(hidden);
				}
			}
			applied->fallback = true;
		}

		// Fallback, then takes the chunks out of previs and the scene and drops every reference held (main thread,
		// outside engine callbacks that walk the parent's children). The engine's queued removal keeps each group, and
		// the chunks it holds, alive until previs lets go of it (FO4-ENGINE-NOTES 5.5d).
		void Revert(CellState& a_state, std::vector<Hidden>* a_carry = nullptr)
		{
			if (!a_state.applied) {
				return;
			}
			Fallback(a_state, a_carry);
			auto applied = std::move(a_state.applied);
			for (auto& group : applied->groups) {
				if (std::exchange(group.registered, false)) {
					Engine::UnregisterPrevisObject(group.node.get());
				}
			}
			for (const auto& hidden : applied->hidden) {
				g_ours.erase(hidden.object.get());
				g_hiddenRefs.erase(hidden.formID);
			}
			for (auto& container : applied->containers) {
				if (const auto node = container.node.get(); node && node->parent) {
					node->parent->DetachChild(node);
				}
			}
			++g_totals.reverts;
		}

		void RevertAll()
		{
			for (auto& [cell, state] : g_cells) {
				Revert(state);
				MarkChanged(state);
			}
		}

		// ---- probes --------------------------------------------------------------------------------------------

		void DetachProbes(CellState& a_state)
		{
			for (auto& probe : a_state.probes) {
				if (const auto node = probe.node.get(); node && node->parent) {
					node->parent->DetachChild(node);
				}
			}
			a_state.probes.clear();
			a_state.blind = false;
		}

		// Previs fed the last frame's views (the query ran with previs active and the query hook is in): the chunks,
		// registered as its dynamic objects, reach the main view through it while group 0 goes undrawn
		// (FO4-ENGINE-NOTES 5.3, 5.5d).
		[[nodiscard]] bool PrevisFeeds() noexcept
		{
			return g_previsHook && g_frame - g_previsTick <= 2;
		}

		// One probe per parent the job's buckets hang under (where its containers will go). False while any of them
		// is new, or neither drawn in the last frame nor covered by the previs feed: the cell waits.
		[[nodiscard]] bool EnsureProbes(CellState& a_state, const Job& a_job)
		{
			bool ready = !a_state.blind;
			for (const auto& bucket : a_job.buckets) {
				const auto parent = bucket.parent;
				const auto found = std::ranges::find(a_state.probes, parent, &Probe::parent);
				if (found != a_state.probes.end()) {
					ready = ready && (found->seen > 0 || PrevisFeeds());
					continue;
				}
				ready = false;
				RE::NiPointer<RE::BSFadeNode> node{ Engine::NewProbe() };
				if (!node) {
					continue;
				}
				parent->AttachChild(node.get(), false);  // appended, as the containers are
				Engine::UpdateStatic(node.get());
				a_state.probes.push_back({ std::move(node), parent });
			}
			return ready;
		}

		// The job's every parent has a probe the last frame drew, or the previs feed covers the main view.
		[[nodiscard]] bool ProbesDrawn(const CellState& a_state, const Job& a_job)
		{
			if (a_state.blind) {
				return false;
			}
			const bool feeds = PrevisFeeds();
			return std::ranges::all_of(a_job.buckets, [&](const Bucket& a_bucket) {
				const auto found = std::ranges::find(a_state.probes, a_bucket.parent, &Probe::parent);
				return found != a_state.probes.end() && (found->seen > 0 || feeds);
			});
		}

		// Each tick: did the last frame's main view draw each probe? Group 0 goes undrawn while previs feeds the main
		// view (FO4-ENGINE-NOTES 5.3); previs then draws the chunks as its dynamic objects. A cell whose probe went unseen for
		// kProbeLostTicks frames with no previs feed either shows its originals; it is rebuilt once every probe is
		// back for kProbeBackTicks frames, or the feed is. A probe the engine detached (its parent rebuilt) is
		// dropped and made again by the next gather.
		void WatchProbes()
		{
			std::size_t lost = 0;
			std::size_t back = 0;
			const bool  feeds = PrevisFeeds();
			g_group0Drawn = false;
			for (auto& [cell, state] : g_cells) {
				if (state.probes.empty()) {
					continue;
				}
				bool anyLost = false;
				bool allBack = true;
				for (auto it = state.probes.begin(); it != state.probes.end();) {
					auto& probe = *it;
					if (!probe.node || probe.node->parent != probe.parent) {
						if (probe.node && probe.node->parent) {
							probe.node->parent->DetachChild(probe.node.get());
						}
						it = state.probes.erase(it);
						MarkChanged(state);
						allBack = false;
						continue;
					}
					if (Engine::TakeProbeSeen(probe.node.get())) {
						++probe.seen;
						probe.unseen = 0;
						g_group0Drawn = true;
					} else {
						++probe.unseen;
						probe.seen = 0;
					}
					anyLost = anyLost || probe.unseen >= kProbeLostTicks;
					allBack = allBack && probe.seen >= kProbeBackTicks;
					++it;
				}
				if (state.probes.empty()) {
					state.blind = false;  // nothing left to tell: the next gather makes new probes and waits for them
					continue;
				}
				if (!state.blind && anyLost && !feeds) {
					state.blind = true;
					if (state.applied) {
						Revert(state);
						++g_totals.blind;
					}
					MarkChanged(state);
					++lost;
				} else if (state.blind && (allBack || feeds)) {
					state.blind = false;
					MarkChanged(state);
					++back;
				}
			}
			if (lost) {
				logger::info("the main view stopped drawing the combined meshes' group in {} cells and previs doesn't feed it: their original meshes are shown", lost);
			}
			if (back) {
				logger::info("the combined meshes are drawn again in {} cells (group 0 or the previs feed): combining them again", back);
			}
		}

		void ReleaseTriShapes(Job& a_job)
		{
			for (auto& triShape : a_job.triShapes) {
				Engine::ReleaseTriShape(triShape);
				triShape = nullptr;
			}
		}

		std::string CellName(RE::TESObjectCELL* a_cell, std::uint32_t a_formID)
		{
			const char* editorID = a_cell ? a_cell->GetFormEditorID() : nullptr;
			return editorID && *editorID ? std::format("{:08X} {}", a_formID, editorID) : std::format("{:08X}", a_formID);
		}

		std::string SkipSummary(const std::array<std::uint32_t, static_cast<std::size_t>(Skip::kCount)>& a_counts)
		{
			std::string text;
			for (std::size_t i = 0; i < a_counts.size(); ++i) {
				if (a_counts[i]) {
					text += std::format("{}{} {}", text.empty() ? "" : ", ", SkipName(static_cast<Skip>(i)), a_counts[i]);
				}
			}
			return text.empty() ? "none"s : text;
		}

		std::string AloneSummary(const std::array<std::uint32_t, static_cast<std::size_t>(Alone::kCount)>& a_counts)
		{
			std::string text;
			for (std::size_t i = 0; i < a_counts.size(); ++i) {
				if (a_counts[i]) {
					text += std::format("{}{} {}", text.empty() ? "" : ", ", AloneName(static_cast<Alone>(i)), a_counts[i]);
				}
			}
			return text.empty() ? "none"s : text;
		}

		// Why a_bucket's member a_index stayed alone: the nearest other member of its material under the same parent
		// (a_byName: material name -> bucket, member), and the first merge key that tells their buckets apart.
		Alone WhyAlone(const Job& a_job, std::uint32_t a_bucket, std::uint32_t a_index,
			const std::unordered_map<std::uintptr_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>& a_byName)
		{
			const auto& bucket = a_job.buckets[a_bucket];
			if (bucket.copiesOnly) {
				return Alone::kCopies;
			}
			const auto found = a_byName.find(*Engine::Field<std::uintptr_t>(bucket.property, 0x10));
			if (found == a_byName.end()) {
				return Alone::kUnique;
			}
			const auto&   self = bucket.members[a_index];
			float         best = std::numeric_limits<float>::max();
			const Bucket* nearest = nullptr;
			for (const auto& [b, m] : found->second) {
				const auto& other = a_job.buckets[b];
				if ((b == a_bucket && m == a_index) || other.parent != bucket.parent) {
					continue;
				}
				const auto& o = other.members[m];
				const float d[3]{ o.center[0] - self.center[0], o.center[1] - self.center[1], o.center[2] - self.center[2] };
				const float distance = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
				if (distance < best) {
					best = distance;
					nearest = &other;
				}
			}
			const float reach = a_job.chunkSize * 2.0F * a_job.sizing.At(self.center);
			if (!nearest) {
				return Alone::kUnique;
			}
			if (best > reach * reach) {
				return Alone::kFar;
			}
			if (nearest == &bucket) {
				return Alone::kSameGroup;
			}
			if (nearest->fadeKey != bucket.fadeKey) {
				return Alone::kFade;
			}
			if (nearest->desc != bucket.desc) {
				return Alone::kFormat;
			}
			if (nearest->alphaKey != bucket.alphaKey) {
				return Alone::kAlpha;
			}
			return nearest->objectKey != bucket.objectKey ? Alone::kShadowBits : Alone::kProperty;
		}

		void LogCell(const Job& a_job, std::string_view a_what)
		{
			if (!Settings::Get().logCells) {
				return;
			}
			const auto& s = a_job.stats;
			logger::info(
				"cell {} {} (chunk size {:.0f}-{:.0f}): {} references, {} with meshes captured ({} meshes), {} of them listed as precombined ({} posed by their own model: baked at their placement); {} chunks replace {} meshes ({} tris, {} verts, {:.1f} MB), "
				"{} of them keep mesh LOD ({} of their {} tris at the farthest level), {} of them copies of one mesh ({} meshes; copy-only meshes: {}), {} stragglers joined a nearby chunk, {} meshes alone cloned as they "
				"are (not mergeable: {}; mergeable, alone: {}); {} references hidden whole ({} of them by their meshes: the root owns collision), {} partly; gather {:.2f} ms, bake {:.2f} ms (worker), apply "
				"{:.2f} ms; left out: {}",
				CellName(a_job.cell, a_job.cellFormID), a_what, a_job.chunkSize * FactorRange(a_job.cell, a_job.sizing).first,
				a_job.chunkSize * FactorRange(a_job.cell, a_job.sizing).second, s.references, s.candidates, s.capturedShapes, s.precombinedRefs, s.posedRefs, s.chunks, s.bakedShapes,
				s.triangles, s.vertices, static_cast<double>(s.vertexBytes) / (1 << 20), s.lodChunks, s.lodTriangles, s.lodFullTriangles, s.copyChunks, s.copyShapes, SkipSummary(s.copies), s.joined,
				s.solos, SkipSummary(s.cloned), AloneSummary(s.alone), s.wholeRefs, s.solidRefs, s.partialRefs, s.gatherMs, s.bakeMs, s.applyMs, SkipSummary(s.skips));
		}

		// ---- previs --------------------------------------------------------------------------------------------

		// Render_PreUI, right after the previs query (main thread, before the cull): notes that previs fed this
		// frame's views. The query draws the chunks itself: each is one of its dynamic objects (Apply), tested and
		// added to the main view, the sun's cascades and the precipitation map as their originals were
		// (FO4-ENGINE-NOTES 5.5d). With CBRO in classic mode the main and sun records go unused (CBRO suspends previs
		// over the cull and the cascades) and group 0 draws the chunks instead.
		// Also reads, at the one moment it means that, which chunks the query's main-view pass found visible
		// (Engine::SeenByMainView): for the audit.
		void OnPrevisQuery() noexcept
		{
			std::unique_lock lock{ g_stateMutex, std::try_to_lock };
			if (!lock || !Engine::PrevisActive()) {
				return;
			}
			g_previsTick = g_frame;
			++g_previsFrames;
			// Read for the summary's audit and previs line only (bProbes): in the frames just before it, not every frame.
			// Only while previs draws the main view (group 0 undrawn): where CBRO draws group 0 it may skip the query, and
			// the bits are then stale (CBRO v1.66, FO4-ENGINE-NOTES 5.5d).
			if (!Settings::Get().probes || Clock::now() - g_lastSummary < kSummaryInterval - std::chrono::milliseconds(200)) {
				return;
			}
			g_previsVisibleRead = !g_group0Drawn;
			if (!g_previsVisibleRead) {
				return;
			}
			std::uint32_t visible = 0;
			for (auto& [cell, state] : g_cells) {
				if (!state.applied || state.applied->fallback) {
					continue;
				}
				// The query sets bit 42 on the registered object only: a group, for all its chunks.
				for (auto& group : state.applied->groups) {
					if (group.registered && Engine::SeenByMainView(group.node.get())) {
						group.previsVisible = g_frame;
						++visible;
					}
				}
			}
			g_previsVisible = visible;
		}

		// ---- apply ---------------------------------------------------------------------------------------------

		// The scene must still be what was captured: same 3D, same transforms, same meshes.
		[[nodiscard]] bool StillValid(const Job& a_job)
		{
			for (const auto& source : a_job.refs) {
				const auto ref = source.ref.get();
				const auto root = source.root.get();
				if (!ref || (ref->GetFormFlags() & kFormDeletedOrDisabled)) {
					return false;
				}
				const auto loaded = ref->loadedData;
				if (!loaded || loaded->data3D.get() != root || Gather::HomeOf(root->parent, g_parked) != source.parent || CulledByGame(root) ||
					!(World(root->world) == World(source.rootWorld))) {
					return false;
				}
			}
			for (const auto& source : a_job.shapes) {
				const auto shape = source.shape.get();
				if (!shape->parent || *Engine::Field<const void*>(shape, Engine::Offset::kRendererData) != source.rendererData ||
					!(World(shape->world) == World(source.world))) {
					return false;
				}
			}
			return true;
		}

		// Hands the chunks' data to the renderer, continuing where the last frame stopped, until a_budget bytes are
		// spent (the copy is the main-thread cost). Sets created once every chunk has its TriShape (or none).
		void CreateTriShapes(Job& a_job, std::size_t& a_budget)
		{
			std::size_t index = 0;
			for (auto& bucket : a_job.buckets) {
				for (auto& chunk : bucket.chunks) {
					if (index++ < a_job.triShapes.size()) {
						continue;
					}
					const auto bytes = chunk.vertices.size() + chunk.indices.size() * sizeof(std::uint16_t);
					if (a_budget == 0) {
						return;
					}
					a_budget = bytes >= a_budget ? 0 : a_budget - bytes;
					void* triShape = nullptr;
					if (chunk.vertexCount) {
						triShape = Engine::CreateTriShape(chunk.vertices.data(), static_cast<std::uint32_t>(chunk.vertices.size()), bucket.chunkDesc,
							chunk.indices.data(), static_cast<std::uint32_t>(chunk.indices.size()));
					}
					a_job.triShapes.push_back(triShape);
					// The engine copied the data (its buffers keep a CPU copy).
					std::vector<std::byte>().swap(chunk.vertices);
					std::vector<std::uint16_t>().swap(chunk.indices);
				}
			}
			a_job.created = true;
		}

		[[nodiscard]] bool TriShapesReady(const Job& a_job)
		{
			return std::ranges::all_of(a_job.triShapes, [](const void* a_triShape) { return !a_triShape || Engine::TriShapeReady(a_triShape); });
		}

		[[nodiscard]] std::size_t ChunkCount(const Job& a_job) noexcept
		{
			std::size_t count = 0;
			for (const auto& bucket : a_job.buckets) {
				count += bucket.chunks.size();
			}
			return count;
		}

		// a_loading: under the loading menu (LoadPass).
		void Apply(Job& a_job, CellState& a_state, bool a_loading)
		{
			const auto start = Clock::now();
			// One TriShape (or none) per chunk, in bucket-major order: anything else is a bookkeeping error, and
			// pairing a chunk with another chunk's data would draw the wrong mesh.
			if (a_job.triShapes.size() != ChunkCount(a_job)) {
				logger::error("cell {:08X}: {} TriShapes for {} chunks; not applied", a_job.cellFormID, a_job.triShapes.size(), ChunkCount(a_job));
				return;
			}
			// With bChunkFadeNodes, a cell gathered while its references are still fading in after attach: each chunk
			// carries on from the furthest fade among its originals, read before they are hidden. An original this
			// plugin hid is drawn by the old chunk, faded in (FO4-ENGINE-NOTES 7.8). Without, chunks are drawn at once,
			// as under the loading menu, where the engine snaps every loaded cell's fades after a load (7.8).
			const bool         fadeNodes = Settings::Get().chunkFadeNodes;
			std::vector<float> fades;
			for (const auto& bucket : a_job.buckets) {
				for (const auto& chunk : bucket.chunks) {
					if (!fadeNodes || a_loading) {
						fades.push_back(1.0F);
						continue;
					}
					float fade = 0.0F;
					for (const auto member : chunk.baked) {
						const auto& shape = a_job.shapes[bucket.shapes[member]];
						const auto  root = a_job.refs[shape.ref].root.get();
						const bool  ours = g_ours.contains(shape.shape.get()) || g_ours.contains(root);
						fade = std::max(fade, ours ? 1.0F : Engine::FadeIn(root));
					}
					fades.push_back(fade);
				}
			}

			// The previous version, if any, in the same frame: no gap. Its roots out of previs's dynamic objects stay out
			// for the new version to take over: registering and unregistering in one frame only queues both, so the check
			// below would find them not in the set and every rebuild left its roots in (run 37: 3,457 -> 25,288 objects,
			// the query 6.7 ms a frame while it rained).
			std::vector<Hidden> carried;
			Revert(a_state, &carried);

			auto applied = std::make_unique<Applied>();
			applied->frame = g_frame;
			std::map<std::tuple<RE::NiNode*, std::int32_t, std::int32_t>, std::uint32_t> groups;  // (parent, square) -> group
			// Group squares (fGroupSize, apart from fChunkSize). Previs's query tests each group and skips an off-screen
			// square in one test (5.5d, run 14). CBRO skips the query in its frames but scans each group node it is
			// offered every frame: with one group per parent, as the precombines' one combined-object node per cell, its
			// cell-node scans fell 0.55 -> 0.26 ms and the cull 3.53 -> 2.82 ms, below the precombines' 3.0 (run 31).
			// Auto (below 0): one group per parent with CBRO, 1024-unit squares without.
			const float setting = Settings::Get().groupSize;
			const float groupSize = setting >= 0.0F ? setting : g_cbroLoaded ? 0.0F : 1024.0F;
			const auto  square = [&](float a_at) { return groupSize > 0.0F ? static_cast<std::int32_t>(std::floor(a_at / groupSize)) : 0; };
			auto& stats = a_job.stats;

			// Mergeable members by material name, to tell why a solo stayed alone (WhyAlone).
			std::unordered_map<std::uintptr_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> byName;
			for (std::uint32_t b = 0; b < a_job.buckets.size(); ++b) {
				const auto& bucket = a_job.buckets[b];
				for (std::uint32_t m = 0; !bucket.cloneOnly && m < bucket.members.size(); ++m) {
					byName[*Engine::Field<std::uintptr_t>(bucket.property, 0x10)].emplace_back(b, m);
				}
			}

			// Self-check (bProbes; run 40, objects piled up): where each chunk must stand (its bake's bound, or a solo's
			// original's), and the members baked off their own world bounds.
			const bool               selfCheck = Settings::Get().probes;
			std::vector<RE::NiBound> expected;
			std::uint32_t            strays = 0;
			std::uint32_t            strayChunks = 0;
			std::string              straySample;

			std::size_t chunkIndex = 0;
			for (std::uint32_t bucketIndex = 0; bucketIndex < a_job.buckets.size(); ++bucketIndex) {
				auto& bucket = a_job.buckets[bucketIndex];
				for (auto& chunk : bucket.chunks) {
					auto& triShape = a_job.triShapes[chunkIndex++];
					// Parent world rotation is identity with unit scale (Gather checks), so local = world - parent.
					const auto&                   parentWorld = bucket.parent->world.translate;
					RE::NiPointer<RE::NiAVObject> clone;
					if (chunk.solo) {
						// A clone of its one mesh, sharing its data, where the mesh stands. With fade nodes an LOD mesh
						// keeps its LOD levels: its fade node's level picks them, as the original's did.
						if (chunk.baked.empty()) {
							continue;
						}
						const auto& source = a_job.shapes[bucket.shapes[chunk.baked.front()]];
						clone.reset(Engine::Clone(source.shape.get(), !fadeNodes));
						if (!clone || !(Engine::IsExactTriShape(clone.get()) || Engine::IsExactMeshLODTriShape(clone.get()))) {
							continue;
						}
						clone->local = source.draw;
						clone->local.translate = RE::NiPoint3{ source.draw.translate.x - parentWorld.x, source.draw.translate.y - parentWorld.y,
							source.draw.translate.z - parentWorld.z };
					} else {
						if (!triShape) {
							continue;
						}
						// The chunk's mesh is a clone of one member (its shader and alpha properties, flags and name)
						// with the combined data in place of the member's. With bChunkMeshLOD, a chunk whose members have
						// mesh-LOD segments is a clone of one such member that stays a BSMeshLODTriShape, with the chunk's
						// segments: its fade node's level picks the prefix, as the members' own nodes did (FO4-ENGINE-NOTES
						// 7.11). Its members share a mergeable property, so which member is cloned doesn't matter.
						const bool meshLOD = fadeNodes && Settings::Get().chunkMeshLOD && (chunk.lodSizes[1] || chunk.lodSizes[2]);
						auto       representative = a_job.shapes[bucket.shapes[chunk.baked.front()]].shape.get();
						for (const auto member : chunk.baked) {
							const auto shape = a_job.shapes[bucket.shapes[member]].shape.get();
							if (meshLOD && Engine::IsExactMeshLODTriShape(shape)) {
								representative = shape;
								break;
							}
						}
						clone.reset(Engine::Clone(representative, !meshLOD));
						const bool lodClone = meshLOD && clone && Engine::IsExactMeshLODTriShape(clone.get());
						if (!clone || !(lodClone || Engine::IsExactTriShape(clone.get()))) {
							Engine::ReleaseTriShape(triShape);
							triShape = nullptr;
							continue;
						}
						Engine::SetGeometry(clone.get(), triShape, chunk.vertexCount, chunk.triangleCount);
						triShape = nullptr;  // owned by the clone now
						if (lodClone) {
							std::copy_n(chunk.lodSizes, 3, Engine::Field<std::uint32_t>(clone.get(), Engine::Offset::kLODSizes));
							++stats.lodChunks;
							stats.lodTriangles += chunk.lodSizes[0];
							stats.lodFullTriangles += chunk.triangleCount;
						}

						auto& bound = *Engine::Field<RE::NiBound>(clone.get(), Engine::Offset::kModelBound);
						bound.center = RE::NiPoint3{ chunk.boundCenter[0], chunk.boundCenter[1], chunk.boundCenter[2] };
						bound.fRadius = chunk.boundRadius;
						clone->local.MakeIdentity();
						clone->local.translate = RE::NiPoint3{ chunk.origin[0] - parentWorld.x, chunk.origin[1] - parentWorld.y, chunk.origin[2] - parentWorld.z };
					}
					clone->flags.flags &= ~(Engine::ObjectFlag::kAppCulled | Engine::ObjectFlag::kNotVisible | Engine::ObjectFlag::kAccumulated);
					clone->fadeAmount = 1.0F;
					clone->meshLODFadingLevel = 0;
					clone->currentMeshLODLevel = 0;
					clone->previousMeshLODLevel = 0;

					// With bChunkFadeNodes, under its own fade node with its members' fade range (a bucket holds one fade
					// class, Gather): the engine fades it out where they would fade (FO4-ENGINE-NOTES 7.9). Otherwise
					// straight in the container.
					RE::NiPointer<RE::BSFadeNode> fadeNode;
					if (fadeNodes) {
						Engine::FadeRange range;
						if (bucket.fadeKey) {
							range.nearDistance = bucket.fadeNear;
							range.farDistance = bucket.fadeFar;
							range.type = bucket.fadeType;
						}
						fadeNode.reset(Engine::NewChunkFadeNode(clone.get(), fades[chunkIndex - 1], range));
						if (!fadeNode) {
							continue;
						}
					}

					// The group is the container, straight under the references' parent: the scene walk adds each child
					// of an exact NiNode there to group 0 on its own (FO4-ENGINE-NOTES 5.5b), so the cull and CBRO's
					// shadow tests still judge chunk by chunk. One node level deeper, the walk filed whole groups, and
					// the sun's nearest cascade took twice the precombines' draws (runs 15-18).
					const auto cell = std::make_tuple(bucket.parent, square(chunk.origin[0]), square(chunk.origin[1]));
					auto [slot, added] = groups.try_emplace(cell, static_cast<std::uint32_t>(applied->groups.size()));
					if (added) {
						const auto node = Engine::NewNode(16);
						if (!node) {
							groups.erase(slot);
							continue;
						}
						node->name = RE::BSFixedString("RuntimeCombiner");
						node->local.MakeIdentity();
						bucket.parent->AttachChild(node, false);  // appended: no empty slot of the parent is reused
						applied->containers.push_back({ RE::NiPointer<RE::NiNode>(node), bucket.parent });
						applied->groups.push_back({ RE::NiPointer<RE::NiNode>(node) });
					}
					applied->groups[slot->second].node->AttachChild(fadeNode ? static_cast<RE::NiAVObject*>(fadeNode.get()) : clone.get(), true);
					auto& record = applied->chunks.emplace_back();
					if (selfCheck) {
						auto& want = expected.emplace_back();
						if (chunk.solo) {
							const auto& member = bucket.members[chunk.baked.front()];
							want.center = RE::NiPoint3{ member.center[0], member.center[1], member.center[2] };
							want.fRadius = member.radius;
						} else {
							want.center = RE::NiPoint3{ chunk.origin[0] + chunk.boundCenter[0], chunk.origin[1] + chunk.boundCenter[1], chunk.origin[2] + chunk.boundCenter[2] };
							want.fRadius = chunk.boundRadius;
						}
					}
					if (selfCheck && !chunk.solo && chunk.strays) {
						strays += chunk.strays;
						if (strayChunks++ < 8) {
							const auto& source = a_job.shapes[bucket.shapes[chunk.firstStray]];
							const auto& member = bucket.members[chunk.firstStray];
							const auto& ref = a_job.refs[source.ref];
							const auto  base = ref.ref ? ref.ref->GetObjectReference() : nullptr;
							straySample += std::format("{}'{}' {} ({}, ref {:08X} {}, scale {:.2f}, desc {:016X}, {} verts {} tris): bound r={:.0f}, vertex box {:.0f} off its centre, half diagonal {:.0f}",
								straySample.empty() ? "" : " | ", source.shape->name.c_str(), reinterpret_cast<const RE::NiObjectNET*>(bucket.property)->name.c_str(),
								Engine::ClassName(source.shape.get()), ref.formID, base && base->Is(RE::ENUM_FORM_ID::kSCOL) ? "SCOL" : "STAT", source.draw.scale,
								bucket.desc, member.vertexCount, member.triangleCount, member.radius, chunk.strayOffset, chunk.strayExtent);
						}
					}
					record.shape = clone;
					record.fadeNode = fadeNode.get();
					record.group = slot->second;
					for (const auto member : chunk.baked) {
						const auto formID = a_job.refs[a_job.shapes[bucket.shapes[member]].ref].formID;
						if (std::ranges::find(record.refs, formID) == record.refs.end()) {
							record.refs.push_back(formID);
						}
					}
					record.members = static_cast<std::uint32_t>(chunk.baked.size());
					record.triangles = chunk.solo ? bucket.members[chunk.baked.front()].triangleCount : chunk.triangleCount;

					for (const auto member : chunk.baked) {
						auto& source = a_job.shapes[bucket.shapes[member]];
						source.baked = true;
						++a_job.refs[source.ref].baked;
					}
					if (chunk.solo) {
						++stats.solos;
						if (!bucket.cloneOnly) {
							const auto why = WhyAlone(a_job, bucketIndex, chunk.baked.front(), byName);
							++stats.alone[static_cast<std::size_t>(why)];
							record.alone = static_cast<std::uint8_t>(why);
						}
						continue;
					}
					++stats.chunks;
					stats.bakedShapes += static_cast<std::uint32_t>(chunk.baked.size());
					if (bucket.copiesOnly) {
						++stats.copyChunks;
						stats.copyShapes += static_cast<std::uint32_t>(chunk.baked.size());
					}
					stats.triangles += chunk.triangleCount;
					stats.vertices += chunk.vertexCount;
					stats.vertexBytes += static_cast<std::uint64_t>(chunk.vertexCount) * bucket.chunkLayout.stride;
				}
			}
			// Placed for good: their previousWorld is their world, or TAA would see each chunk move in from wherever its
			// clone was built (FO4-ENGINE-NOTES 7.10).
			for (auto& container : applied->containers) {
				Engine::UpdateStatic(container.node.get());
			}
			std::uint32_t misplaced = 0;
			std::string   misplacedSample;
			for (std::size_t i = 0; selfCheck && i < applied->chunks.size(); ++i) {
				const auto& record = applied->chunks[i];
				const auto& bound = record.shape->worldBound;
				const auto& want = expected[i];
				const float tolerance = 1.0F + 0.01F * want.fRadius;
				if (bound.center.GetDistance(want.center) > tolerance || std::abs(bound.fRadius - want.fRadius) > tolerance) {
					if (misplaced++ < 8) {
						misplacedSample += std::format("{}'{}' ({} members): at ({:.0f}, {:.0f}, {:.0f}) r={:.0f}, bake ({:.0f}, {:.0f}, {:.0f}) r={:.0f}",
							misplacedSample.empty() ? "" : " | ", record.shape->name.c_str(), record.members, bound.center.x, bound.center.y,
							bound.center.z, bound.fRadius, want.center.x, want.center.y, want.center.z, want.fRadius);
					}
				}
			}
			if (selfCheck) {
				logger::info("cell {} self-check: {} members in {} chunks baked off their own bounds{}{}; {} of {} chunks placed off their bake{}{}",
					CellName(a_job.cell, a_job.cellFormID), strays, strayChunks, straySample.empty() ? "" : ": ", straySample, misplaced,
					applied->chunks.size(), misplacedSample.empty() ? "" : ": ", misplacedSample);
			}

			// Hide what the chunks now draw: a reference whose every part was combined is hidden at its root (the
			// scene walk skips it), others mesh by mesh. A root that owns a collision object stays unculled and its
			// meshes are hidden instead, though it still counts as hidden whole (parked, out of previs): the camera's
			// collision cast ignores a body whose owner is AppCulled (FO4-ENGINE-NOTES 7.14). Gather keeps meshes
			// with collision visible, so a root is the only owner this could cull.
			const auto hide = [&](RE::NiAVObject* a_object, const SourceRef& a_ref, bool a_root, bool a_cull) {
				if (a_cull) {
					a_object->SetAppCulled(true);
				}
				auto& hidden = applied->hidden.emplace_back();
				hidden.culled = a_cull;
				hidden.object.reset(a_object);
				hidden.ref = a_ref.ref;
				hidden.root = a_ref.root;
				hidden.formID = a_ref.formID;
				hidden.world = World(a_object->world);
				if (const auto node = a_root ? a_object->IsNode() : nullptr) {
					hidden.children = node->children.size();
				}
				g_ours.insert(a_object);
				g_hiddenRefs[a_ref.formID] = a_job.cell;
			};
			std::vector<bool>            rootCulled(a_job.refs.size(), false);
			std::vector<RE::NiAVObject*> roots;
			for (std::size_t i = 0; i < a_job.refs.size(); ++i) {
				const auto& ref = a_job.refs[i];
				if (ref.baked && ref.whole && ref.baked == ref.captured) {
					const bool solid = ref.root->collisionObject != nullptr;
					hide(ref.root.get(), ref, true, !solid);
					roots.push_back(ref.root.get());
					rootCulled[i] = !solid;
					++stats.wholeRefs;
					stats.solidRefs += solid;
				} else if (ref.baked) {
					++stats.partialRefs;
				}
			}
			for (const auto& shape : a_job.shapes) {
				if (shape.baked && !rootCulled[shape.ref]) {
					hide(shape.shape.get(), a_job.refs[shape.ref], false, true);
				}
			}

			// While previs is active it draws the main view, the sun's cascades and the precipitation map from its
			// per-frame query, not group 0. A static reference outside its cell's previs list (all of them with
			// precombines off) is one of the query's dynamic objects, tested and drawn each frame: so is each chunk
			// group, in its originals' place; the query tests a group's chunks when the group is visible (5.5d).
			for (auto& group : applied->groups) {
				group.registered = Engine::RegisterPrevisObject(group.node.get());
			}
			// A root hidden whole is skipped by the query's test but still read in each of its three passes every frame:
			// with ~25k of them the query took 8 ms a frame (run 13). It leaves the set while its chunks stand in.
			// Under the loading menu the new cells' registrations are still queued and the set is the frame's to
			// change (5.5d): TakeOutLoaded takes the roots out two ticks on.
			std::unordered_set<const RE::NiAVObject*> carriedRoots;
			for (const auto& hidden : carried) {
				carriedRoots.insert(hidden.object.get());
			}
			for (auto& hidden : applied->hidden) {
				if (hidden.object == hidden.root) {
					hidden.unregistered = carriedRoots.erase(hidden.object.get()) > 0 || (!a_loading && Engine::UnregisterIfDynamic(hidden.object.get()));
				}
			}
			applied->previsPending = a_loading;
			for (auto& hidden : carried) {  // not hidden whole any more: back in, as Fallback would have
				if (carriedRoots.contains(hidden.object.get())) {
					Reregister(hidden);
				}
			}
			if (!applied->containers.empty() && Settings::Get().parkHidden) {
				Park(*applied, a_job.cell, roots);
			}
			stats.applyMs = Ms(start);
			if (!applied->containers.empty()) {
				a_state.applied = std::move(applied);
				++g_totals.applies;
			}
			LogCell(a_job, stats.chunks ? "combined"sv : "nothing combined (no chunk of enough meshes)"sv);
		}

		// ---- jobs ----------------------------------------------------------------------------------------------

		// a_loading: under the loading menu (LoadPass): no frame to spread the buffers over, and no frame to have drawn
		// the probes; WatchProbes judges them once frames are drawn.
		void ProcessJobs(bool a_loading = false)
		{
			const auto& settings = Settings::Get();
			std::size_t createBudget = a_loading ? SIZE_MAX : kCreateBytesPerFrame;
			for (auto it = g_jobs.begin(); it != g_jobs.end();) {
				auto& job = **it;
				if (!job.baked.load(std::memory_order_acquire)) {
					++it;
					continue;
				}
				const auto found = g_cells.find(job.cell);
				const bool current = found != g_cells.end() && found->second.generation == job.generation;
				if (!current || Paused()) {
					if (current) {
						found->second.building = false;
						MarkChanged(found->second);
					}
					ReleaseTriShapes(job);
					++g_totals.discarded;
					it = g_jobs.erase(it);
					continue;
				}
				auto& state = found->second;
				if (!settings.apply) {
					for (const auto& bucket : job.buckets) {
						for (const auto& chunk : bucket.chunks) {
							if (chunk.vertexCount) {
								++job.stats.chunks;
								job.stats.bakedShapes += static_cast<std::uint32_t>(chunk.baked.size());
								job.stats.triangles += chunk.triangleCount;
								job.stats.vertices += chunk.vertexCount;
								job.stats.vertexBytes += static_cast<std::uint64_t>(chunk.vertexCount) * bucket.chunkLayout.stride;
							}
						}
					}
					LogCell(job, "dry run (bApply=0)"sv);
					state.building = false;
					it = g_jobs.erase(it);
					continue;
				}
				if (!job.created) {
					CreateTriShapes(job, createBudget);
					if (!job.created) {
						++it;  // the frame's budget ran out: the rest next frame
						continue;
					}
				}
				if (!TriShapesReady(job)) {
					++it;
					continue;
				}
				state.building = false;
				if (!StillValid(job) || (!a_loading && !ProbesDrawn(state, job))) {
					ReleaseTriShapes(job);
					MarkChanged(state);
					++g_totals.discarded;
					it = g_jobs.erase(it);
					continue;
				}
				Apply(job, state, a_loading);
				ReleaseTriShapes(job);  // any left over (chunks that failed to apply)
				it = g_jobs.erase(it);
			}
		}

		// a_loading: under the loading menu (LoadPass): the load is done, so every cell at once, no settle.
		void StartJobs(bool a_loading = false)
		{
			const auto& settings = Settings::Get();
			const auto  start = Clock::now();
			for (auto& [cell, state] : g_cells) {
				if (!state.dirty || state.building || state.blind || (!a_loading && g_frame - state.lastChange < settings.settleFrames)) {
					continue;
				}
				if (!a_loading && Ms(start) > settings.gatherBudgetMs) {
					break;
				}
				state.dirty = false;
				auto job = std::make_unique<Job>();
				job->cell = cell;
				job->cellFormID = state.formID;
				job->generation = ++state.generation;
				job->started = Clock::now();
				job->chunkSize = settings.chunkSize;
				job->sizing = CurrentSizing();
				state.sizing = job->sizing;
				state.sized = true;
				Gather::Cell(cell, g_ours, g_parked, g_meshByMesh, *job);
				state.blocked = job->stats.blockedClasses;
				if (job->buckets.empty()) {
					LogCell(*job, "nothing to combine"sv);
					Revert(state);  // nothing to combine any more
					continue;
				}
				if (!EnsureProbes(state, *job) && !a_loading) {
					MarkChanged(state);  // a new probe, or one the last frame didn't draw: gather again after a settle
					continue;
				}
				state.building = true;
				Submit(job.get());
				g_jobs.push_back(std::move(job));
			}

			const auto votes = Gather::Votes();
			if (!g_halted && votes.disagree > 16 && votes.disagree > votes.agree) {
				g_halted = true;
				logger::error(
					"rotation self-check failed ({} meshes match the other convention, {} this one): combining stopped, everything restored",
					votes.disagree, votes.agree);
				RevertAll();
			}
		}

		// ---- watchdog ------------------------------------------------------------------------------------------

		[[nodiscard]] Check CheckHidden(const Hidden& a_hidden)
		{
			const auto ref = a_hidden.ref.get();
			if (!ref || (ref->GetFormFlags() & kFormDeletedOrDisabled)) {
				return Check::kStale;
			}
			const auto loaded = ref->loadedData;
			if (!loaded || loaded->data3D.get() != a_hidden.root.get()) {
				return Check::kStale;
			}
			const auto object = a_hidden.object.get();
			if ((a_hidden.culled && !object->GetAppCulled()) || !object->parent || !(World(object->world) == a_hidden.world)) {
				return Check::kStale;
			}
			if (a_hidden.children != kNoChildCount) {
				if (const auto node = object->IsNode(); node && node->children.size() > a_hidden.children) {
					return Check::kAttached;
				}
			}
			return Check::kOk;
		}

		// Re-checks a slice of the hidden objects each frame: a moved, unloaded, disabled or re-shown original
		// means the chunk is stale; the cell shows its originals and is rebuilt. Something attached under a
		// hidden root (a decal, an effect) would stay hidden with it: that reference keeps its root visible from
		// then on and has only its meshes hidden.
		void Watchdog()
		{
			auto budget = Settings::Get().watchdogRefsPerFrame;
			std::size_t total = 0;
			for (const auto& [cell, state] : g_cells) {
				total += state.applied ? state.applied->hidden.size() + state.applied->containers.size() : 0;
			}
			if (total == 0) {
				return;
			}
			if (g_watchCursor >= total) {
				g_watchCursor = 0;
			}
			std::size_t position = 0;
			for (auto& [cell, state] : g_cells) {
				if (!state.applied || budget == 0) {
					continue;
				}
				const auto& applied = *state.applied;
				const auto  count = applied.hidden.size() + applied.containers.size();
				if (position + count <= g_watchCursor) {
					position += count;
					continue;
				}
				bool stale = applied.fallback;
				for (auto i = g_watchCursor - position; i < count && budget > 0 && !stale; ++i, --budget, ++g_watchCursor) {
					if (i < applied.hidden.size()) {
						const auto& hidden = applied.hidden[i];
						const auto  result = CheckHidden(hidden);
						if (result == Check::kAttached) {
							g_meshByMesh.insert(hidden.formID);
							++g_totals.attached;
							logger::info("reference {:08X}: something was attached under its hidden root; from now on only its meshes are hidden", hidden.formID);
						}
						stale = result != Check::kOk;
					} else {
						const auto& container = applied.containers[i - applied.hidden.size()];
						stale = !container.node || container.node->parent != container.parent;
					}
				}
				position += count;
				if (stale) {
					++g_totals.watchdog;
					Revert(state);
					MarkChanged(state);
					state.lastChange = 0;  // rebuild at once: the originals are drawing one by one meanwhile
				}
			}
		}

		// ---- cells and events ----------------------------------------------------------------------------------

		// The attached cells are read from the engine every frame (TES's interior cell or exterior grid). Attach and
		// detach events arrive on the loading thread during loading screens; only their type values are logged.
		void PollCells()
		{
			const auto  loaded = Engine::LoadedCells();
			std::size_t added = 0;
			std::size_t removed = 0;
			for (const auto cell : loaded) {
				if (!g_cells.contains(cell)) {
					auto& state = g_cells[cell];
					state.formID = cell->GetFormID();
					MarkChanged(state);
					++added;
				}
			}
			for (auto it = g_cells.begin(); it != g_cells.end();) {
				if (std::ranges::find(loaded, it->first) == loaded.end()) {
					Revert(it->second);
					DetachProbes(it->second);
					it = g_cells.erase(it);
					++removed;
				} else {
					++it;
				}
			}
			if (added || removed) {
				const auto start = Clock::now();
				RebuildPoseTable();
				logger::info("attached cells: {} (+{}, -{}); pose table {} references, rebuilt in {:.2f} ms", g_cells.size(), added, removed,
					g_poseTable.load()->refs.size(), Ms(start));
			}
		}

		void OnObject(std::uint32_t a_formID, bool a_loaded)
		{
			if (const auto hidden = g_hiddenRefs.find(a_formID); hidden != g_hiddenRefs.end()) {
				if (const auto found = g_cells.find(hidden->second); found != g_cells.end()) {
					if (!a_loaded) {
						Fallback(found->second);  // the full revert waits for the next frame
					}
					MarkChanged(found->second);
				}
				return;
			}
			const auto form = RE::TESForm::GetFormByID(a_formID);
			if (!form || form->GetFormType() != RE::ENUM_FORM_ID::kREFR) {
				return;
			}
			const auto ref = static_cast<RE::TESObjectREFR*>(form);
			if (!IsCombinableBase(ref)) {
				return;
			}
			if (const auto found = g_cells.find(ref->GetParentCell()); found != g_cells.end()) {
				MarkChanged(found->second);
			}
		}

		struct PendingObject
		{
			std::uint32_t formID{ 0 };
			bool          loaded{ false };
		};
		std::mutex                 g_eventMutex;
		std::vector<PendingObject> g_events;
		std::atomic<std::uint32_t> g_cellEventsLogged{ 0 };

		class CellSink final : public RE::BSTEventSink<RE::CellAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::CellAttachDetachEvent& a_event, RE::BSTEventSource<RE::CellAttachDetachEvent>*) override
			{
				if (a_event.cell && g_cellEventsLogged.fetch_add(1) < 8) {
					logger::info("cell event: type {} for {:08X} (thread {})", static_cast<int>(a_event.type.get()), a_event.cell->GetFormID(), GetCurrentThreadId());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Object (un)load events: handled at once on the game thread, queued from any other (and while a loading-menu
		// pass on another thread holds the state).
		class ObjectSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent& a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				std::unique_lock<std::recursive_timed_mutex> state;
				if (GetCurrentThreadId() == g_mainThread.load(std::memory_order_relaxed)) {
					state = std::unique_lock{ g_stateMutex, std::try_to_lock };
				}
				if (state) {
					OnObject(a_event.formID, a_event.loaded);
				} else {
					std::scoped_lock lock{ g_eventMutex };
					g_events.push_back({ a_event.formID, a_event.loaded });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		CellSink   g_cellSink;
		ObjectSink g_objectSink;

		void DrainEvents()
		{
			std::vector<PendingObject> events;
			{
				std::scoped_lock lock{ g_eventMutex };
				events.swap(g_events);
			}
			for (const auto& event : events) {
				OnObject(event.formID, event.loaded);
			}
		}

		// ---- frame ---------------------------------------------------------------------------------------------

		// Off means the game as without Runtime Combiner: its precombines as configured. The engine reads the switch
		// when a cell loads, so loaded cells keep the old value until the next save load (FO4-ENGINE-NOTES 7.12).
		void Toggle()
		{
			g_active = !g_active;
			const bool wanted = (g_active && Settings::Get().disablePrecombines) ? false : g_configuredPrecombines;
			const bool change = g_resetHook && wanted != Engine::PrecombinesEnabled();
			g_precombinesNext.store(change ? static_cast<int>(wanted) : -1);
			const auto precombines = change ? std::format("; precombines {} after the next save load", wanted ? "back" : "replaced") : ""s;
			if (g_active) {
				for (auto& [cell, state] : g_cells) {
					state.dirty = true;
					state.lastChange = 0;  // settled: rebuild now
				}
				Notify("Runtime Combiner: on"s + precombines);
			} else {
				RevertAll();
				Notify("Runtime Combiner: off (original meshes)"s + precombines);
			}
		}

		// Inside Main::PerformGameReset, every cell cleared and purged, before the save's world loads.
		void OnGameReset()
		{
			const int next = g_precombinesNext.exchange(-1);
			if (next >= 0 && (next != 0) != Engine::PrecombinesEnabled()) {
				Engine::SetPrecombinesEnabled(next != 0);
				logger::info("save load: precombines switched {} (bUseCombinedObjects = {}) with no cell loaded", next ? "on" : "off", next);
			}
		}

		// Inside a cone of kAuditHalfCone around the camera's view direction (its world rotation's row 0,
		// FO4-ENGINE-NOTES 6.1), within kAuditRange units, with the camera outside the bound: on screen unless
		// something stands in front of it.
		constexpr float kAuditRange = 2000.0F;
		constexpr float kAuditHalfCone = 0.7F;  // radians, about 40 degrees

		[[nodiscard]] bool InView(const RE::NiCamera& a_camera, const RE::NiBound& a_bound)
		{
			const auto& eye = a_camera.world.translate;
			const auto& forward = a_camera.world.rotate.entry[0];
			const float dx = a_bound.center.x - eye.x;
			const float dy = a_bound.center.y - eye.y;
			const float dz = a_bound.center.z - eye.z;
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (distance <= a_bound.fRadius || distance - a_bound.fRadius > kAuditRange) {
				return false;
			}
			const float cosine = (dx * forward.pt[0] + dy * forward.pt[1] + dz * forward.pt[2]) / distance;
			return cosine >= std::cos(std::min(kAuditHalfCone + std::asin(a_bound.fRadius / distance), 3.14159F));
		}

		// What the renderer made of the applied chunks: drawn at least once (render passes exist) and blended, and
		// the chunks on screen that were never drawn (in view, applied over 120 frames ago), which should be none.
		void AuditChunks()
		{
			struct Suspect
			{
				float              distance{ 0.0F };
				const ChunkRecord* record{ nullptr };
				std::uint32_t      cell{ 0 };
				const char*        material{ nullptr };
				std::uint64_t      shaderFlags{ 0 };
			};
			std::size_t          total = 0, drawn = 0, blended = 0, inView = 0, fading = 0, foreign = 0, filed = 0, moving = 0, unreadable = 0;
			std::vector<Suspect> suspects;
			const auto           camera = RE::Main::WorldRootCamera();
			// A chunk in view is expected drawn when nothing culls it but the frustum (group 0 drawn, no CBRO, an
			// exterior: interiors cull by portals), or when the main view draws previs's records and the last query
			// found it visible. CBRO's own culling of group 0 can't be read here: no expectation then.
			const bool frustumOnly = g_group0Drawn && !g_cbroLoaded && !Engine::InInterior();
			const bool previsDraws = PrevisFeeds() && !g_group0Drawn;
			for (const auto& [cell, state] : g_cells) {
				if (!state.applied) {
					continue;
				}
				const bool settled = g_frame - state.applied->frame > 120;
				for (const auto& record : state.applied->chunks) {
					++total;
					Engine::ChunkView view;
					if (!record.shape || !Engine::ReadChunkView(record.shape.get(), record.fadeNode, view)) {
						++unreadable;
						continue;
					}
					drawn += view.passes ? 1 : 0;
					blended += view.passes && view.alpha < 1.0F ? 1 : 0;
					fading += view.fade < 1.0F ? 1 : 0;
					foreign += view.owner ? 0 : 1;
					filed += view.inView ? 1 : 0;
					moving += view.moving ? 1 : 0;
					const bool expected = frustumOnly || (previsDraws && g_frame - state.applied->groups[record.group].previsVisible <= 1);
					if (camera && settled && expected && InView(*camera, record.shape->worldBound)) {
						++inView;
						if (!view.passes) {
							const auto& eye = camera->world.translate;
							suspects.push_back({ record.shape->worldBound.center.GetDistance(eye), &record, state.formID, view.material,
								view.shaderFlags });
						}
					}
				}
			}
			if (total == 0) {
				return;
			}
			std::string fadeNodes;
			if (Settings::Get().chunkFadeNodes) {
				fadeNodes = std::format("; fade nodes: filed by a view at the last cull {}, fading {}, fade alpha from another node {}", filed,
					fading, foreign);
			}
			logger::info("chunk audit: {} chunks, drawn at least once {} (blended {}), moving for TAA {} (should be 0); on screen within {:.0f} "
						 "units and expected drawn {}, never drawn {}{}; unreadable {}",
				total, drawn, blended, moving, kAuditRange, inView, suspects.size(), fadeNodes, unreadable);
			const auto count = std::min<std::size_t>(suspects.size(), 3);
			std::partial_sort(suspects.begin(), suspects.begin() + count, suspects.end(), [](const Suspect& a_l, const Suspect& a_r) { return a_l.distance < a_r.distance; });
			for (std::size_t i = 0; i < count; ++i) {
				const auto& s = suspects[i];
				logger::warn("  on screen but never drawn: cell {:08X}, {:.0f} units from the camera, {} meshes (first reference {:08X}), {} tris, "
							 "mesh '{}', material '{}', shader flags {:016X}",
					s.cell, s.distance, s.record->members, s.record->refs.empty() ? 0u : s.record->refs.front(), s.record->triangles,
					s.record->shape->name.c_str(), s.material ? s.material : "", s.shaderFlags);
			}
		}

		// Previs since the last summary: frames whose query ran with previs active, and the chunks it tests as its
		// dynamic objects.
		void LogPrevis()
		{
			if (!g_previsHook) {
				return;
			}
			std::size_t registered = 0;
			std::size_t unregistered = 0;
			std::size_t chunks = 0;
			for (const auto& [cell, state] : g_cells) {
				if (state.applied) {
					registered += std::ranges::count_if(state.applied->groups, &ChunkGroup::registered);
					chunks += state.applied->chunks.size();
					unregistered += std::ranges::count_if(state.applied->hidden, &Hidden::unregistered);
				}
			}
			const auto visible = PrevisFeeds() && g_previsVisibleRead ? std::format("{} groups found visible by its last main-view pass", g_previsVisible) :
			                                                            "its main-view verdicts not read (group 0 drew the main view)"s;
			logger::info("previs: active in {} frames, {} dynamic objects (its query tests each every frame), of them {} chunk groups "
						 "holding {} chunks; {} hidden originals taken out; {}; group 0 drawn last frame: {} (yes: previs off, or suspended "
						 "over the cull as CBRO classic does)",
				std::exchange(g_previsFrames, 0), Engine::PrevisDynamicObjects(), registered, chunks, unregistered, visible,
				g_group0Drawn ? "yes" : "no");
		}

		// What the scene walk files into group 0 (Engine::WalkEntries), by kind and by nearest distance from the camera:
		// entries and the meshes under them. Run 26: RC's scene took ~35% more sun-cascade registrations than the
		// precombines' at an equal main view, and CBRO can't tell which objects; this tells the A/B's two states apart.
		void LogCensus()
		{
			const auto camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return;
			}
			struct Ours
			{
				std::string        kind;
				const ChunkRecord* record{ nullptr };
			};
			std::unordered_map<const RE::NiAVObject*, Ours> ours;    // what RC hangs in the scene
			std::unordered_set<const RE::NiAVObject*>       partly;  // ancestors of meshes RC hid one by one
			for (const auto& [cell, state] : g_cells) {
				for (const auto& probe : state.probes) {
					ours.emplace(probe.node.get(), Ours{ "RC probe" });
				}
				if (!state.applied) {
					continue;
				}
				for (const auto& record : state.applied->chunks) {
					auto kind = record.members > 1             ? "RC chunk"s :
					            record.alone == kNotAlone ? "RC solo (not mergeable)"s :
					                                        std::format("RC solo ({})", AloneName(static_cast<Alone>(record.alone)));
					ours.emplace(record.Fed(), Ours{ std::move(kind), &record });
				}
			}
			std::vector<std::pair<float, const ChunkRecord*>> nearSolos;  // solos under 1500 units, for a sample
			for (const auto object : g_ours) {
				for (auto node = object ? object->parent : nullptr; node && partly.insert(node).second;) {
					node = node->parent;
				}
			}

			constexpr std::array<float, 3> kRings{ 1500.0F, 4000.0F, 14000.0F };
			struct Row
			{
				std::array<std::uint32_t, 4> entries{};
				std::array<std::uint32_t, 4> meshes{};
				double                       nearRadius{ 0.0 };  // summed over the first two rings
			};
			std::map<std::string, Row>     rows;
			std::vector<Engine::WalkEntry> entries;
			std::uint32_t                  hidden = 0;
			const auto&                    eye = camera->world.translate;
			for (const auto& [cell, state] : g_cells) {
				entries.clear();
				Engine::WalkEntries(cell, entries, hidden);
				for (const auto& entry : entries) {
					const auto  object = entry.object;
					const auto  found = ours.find(object);
					std::string kind = found != ours.end() ? found->second.kind :
					                   entry.precombined       ? "precombined" :
					                   partly.contains(object) ? std::format("partly hidden {}", Engine::ClassName(object)) :
					                                             std::string(Engine::ClassName(object));
					if (entry.node9) {
						kind += " (node 9)";
					}
					const auto& bound = object->worldBound;
					const float distance = std::max(0.0F, eye.GetDistance(bound.center) - bound.fRadius);
					const auto  ring = static_cast<std::size_t>(std::ranges::upper_bound(kRings, distance) - kRings.begin());
					if (found != ours.end() && found->second.record && found->second.record->members <= 1 && ring == 0) {
						nearSolos.emplace_back(distance, found->second.record);
					}
					auto&       row = rows[kind];
					++row.entries[ring];
					row.meshes[ring] += entry.meshes;
					row.nearRadius += ring < 2 ? bound.fRadius : 0.0F;
				}
			}
			std::vector<std::pair<std::string, Row>> sorted(rows.begin(), rows.end());
			std::ranges::sort(sorted, [](const auto& a_l, const auto& a_r) { return a_l.second.meshes[1] + a_l.second.meshes[0] > a_r.second.meshes[1] + a_r.second.meshes[0]; });
			std::string text;
			for (const auto& [kind, row] : sorted) {
				const auto nearEntries = row.entries[0] + row.entries[1];
				text += std::format("{}{} {} ({}) / {} ({}) / {} ({}) / {} ({}), near radius {:.0f}", text.empty() ? "" : " | ", kind, row.entries[0], row.meshes[0],
					row.entries[1], row.meshes[1], row.entries[2], row.meshes[2], row.entries[3], row.meshes[3], nearEntries ? row.nearRadius / nearEntries : 0.0);
			}
			logger::info("walk census (group-0 entries (meshes) by nearest distance from the camera: under 1500 / 4000 / 14000 / beyond; near radius: mean "
						 "bound radius under 4000): {}; AppCulled entries skipped {}",
				text.empty() ? "none"s : text, hidden);
			// What the nearest solos are: the mesh's name, its material, its bound radius.
			std::ranges::sort(nearSolos, {}, &std::pair<float, const ChunkRecord*>::first);
			std::string sample;
			for (std::size_t i = 0; i < nearSolos.size() && i < 16; ++i) {
				const auto      record = nearSolos[i].second;
				Engine::ChunkView view;
				const bool      read = record->shape && Engine::ReadChunkView(record->shape.get(), record->fadeNode, view);
				sample += std::format("{}{:.0f}: '{}' {} r={:.0f} ({})", sample.empty() ? "" : " | ", nearSolos[i].first,
					record->shape ? record->shape->name.c_str() : "?", read && view.material ? view.material : "?",
					record->shape ? record->shape->worldBound.fRadius : 0.0F,
					record->alone == kNotAlone ? "not mergeable"sv : AloneName(static_cast<Alone>(record->alone)));
			}
			if (!sample.empty()) {
				logger::info("nearest solos (distance: mesh, material, radius, why alone): {}", sample);
			}
		}

		// The blocked parts of the attached cells by class: what keeps references from being hidden whole and
		// meshes from being combined. Logged when it changed.
		void LogBlocked()
		{
			std::vector<std::pair<std::string_view, std::uint64_t>> totals;
			for (const auto& [cell, state] : g_cells) {
				for (const auto& [name, count] : state.blocked) {
					const std::string_view key{ name };
					const auto             found = std::ranges::find(totals, key, &std::pair<std::string_view, std::uint64_t>::first);
					if (found != totals.end()) {
						found->second += count;
					} else {
						totals.emplace_back(key, count);
					}
				}
			}
			std::ranges::sort(totals, [](const auto& a_l, const auto& a_r) { return a_l.second > a_r.second; });
			std::uint64_t hash = 1469598103934665603ull;
			std::uint64_t all = 0;
			std::string   text;
			for (const auto& [name, count] : totals) {
				hash = (hash ^ std::hash<std::string_view>{}(name)) * 1099511628211ull;
				hash = (hash ^ count) * 1099511628211ull;
				all += count;
				if (text.size() < 400) {
					text += std::format("{}{} {}", text.empty() ? "" : ", ", name, count);
				}
			}
			if (hash == g_blockedPrinted) {
				return;
			}
			g_blockedPrinted = hash;
			logger::info("blocked parts in the attached cells: {} ({})", all, text.empty() ? "none"s : text);
		}

		// GPU probe (bProbes): work and time per render stage, per measured frame (counts in millions), and the
		// composite's image-space effects.
		void LogGpu()
		{
			std::string gpu;
			for (std::size_t i = 0; const auto& stage : Engine::TakeGpuStats()) {
				const double n = stage.frames ? 1e6 * static_cast<double>(stage.frames) : 1.0;
				gpu += std::format("{}{}: GPU {:.2f} ms ({} timed), triangles in {:.3f}, rasterized {:.3f}, vertices shaded {:.3f}, pixels shaded "
								   "{:.3f}, compute threads {:.3f} ({} read, {} skipped)",
					i ? " | " : "", Engine::GpuStageName(i), stage.timed ? stage.gpuMs / static_cast<double>(stage.timed) : 0.0, stage.timed,
					stage.iaPrimitives / n, stage.cPrimitives / n, stage.vsInvocations / n, stage.psInvocations / n, stage.csInvocations / n,
					stage.frames, stage.skipped);
				++i;
			}
			logger::info("GPU work per frame in millions ({}, precombines {}): {}", g_active ? "on" : "switched off",
				Engine::PrecombinesEnabled() ? "on" : "off", gpu);
			std::string effects;
			for (const auto& effect : Engine::TakeCompositeEffects()) {
				std::string_view name = effect.name;
				if (name.starts_with(".?AV")) {
					name.remove_prefix(4);
				}
				if (name.ends_with("@@")) {
					name.remove_suffix(2);
				}
				effects += std::format("{}{}: {:.2f} ms ({} timed)", effects.empty() ? "" : " | ", name.empty() ? "(none)" : name,
					effect.timed ? effect.gpuMs / static_cast<double>(effect.timed) : 0.0, effect.timed);
			}
			logger::info("composite effects, GPU time per frame: {}", effects);
		}

		void Summary()
		{
			const auto now = Clock::now();
			if (g_cells.empty() || now - g_lastSummary < kSummaryInterval) {
				return;
			}
			g_lastSummary = now;
			std::uint32_t queries = 0;
			const double  queryMs = Engine::TakePrevisQueryTime(queries);
			const auto    times = std::exchange(g_frameTimes, {});
			if (times.frames) {
				logger::info("frames: {} at {:.2f} ms avg ({:.1f} FPS), slowest {:.1f} ms; Runtime Combiner's tick {:.3f} ms avg, slowest "
							 "{:.2f} ms; previs query {:.3f} ms avg over {} calls ({}, precombines {})",
					times.frames, times.frameMs / times.frames, 1000.0 * times.frames / times.frameMs, times.frameMax,
					times.tickMs / times.frames, times.tickMax, queries ? queryMs / queries : 0.0, queries,
					g_active ? "on" : "switched off", Engine::PrecombinesEnabled() ? "on" : "off");
			}
			const bool probes = Settings::Get().probes;
			if (probes) {
				LogGpu();
			}
			std::size_t cells = 0, chunks = 0, hidden = 0, building = 0, blind = 0;
			for (const auto& [cell, state] : g_cells) {
				building += state.building ? 1 : 0;
				blind += state.blind ? 1 : 0;
				if (state.applied) {
					++cells;
					hidden += state.applied->hidden.size();
					chunks += state.applied->chunks.size();
				}
			}
			const auto votes = Gather::Votes();
			const auto camera = RE::Main::WorldRootCamera();
			const auto eye = camera ? camera->world.translate : RE::NiPoint3{};
			const auto version = g_totals.applies * 1000003 + g_totals.reverts * 1009 + g_cells.size() * 7 + (Engine::PrecombinesEnabled() ? 1 : 0);
			const bool census = probes && (version != g_censusVersion || eye.GetDistance(g_censusEye) >= kCensusMove);
			if (census) {
				g_censusVersion = version;
				g_censusEye = eye;
			}
			// The engine's own chunks and RC's, for the A/B: how many, and how big (a bound decides how often the sun's
			// cascades file an object, run 17).
			std::uint32_t      precombined = 0;
			std::vector<float> engineRadii, mergedRadii, soloRadii;
			for (const auto& [cell, state] : g_cells) {
				precombined += Engine::PrecombinedChunks(cell, census ? &engineRadii : nullptr);
				if (census && state.applied) {
					for (const auto& record : state.applied->chunks) {
						if (record.shape) {
							(record.members > 1 ? mergedRadii : soloRadii).push_back(record.shape->worldBound.fRadius);
						}
					}
				}
			}
			const auto sizes = [](std::vector<float>& a_radii) {
				if (a_radii.empty()) {
					return "none"s;
				}
				std::ranges::sort(a_radii);
				double sum = 0.0;
				for (const auto r : a_radii) {
					sum += r;
				}
				return std::format("{} (radius mean {:.0f}, median {:.0f}, 90% under {:.0f}, max {:.0f})", a_radii.size(), sum / a_radii.size(),
					a_radii[a_radii.size() / 2], a_radii[a_radii.size() * 9 / 10], a_radii.back());
			};
			if (census) {
				logger::info("object sizes: combined chunks {}; solos {}; precombined chunks {}", sizes(mergedRadii), sizes(soloRadii), sizes(engineRadii));
			}
			logger::info(
				"summary: {} of {} attached cells combined ({} building, {} not drawn by the main view), {} chunks in the scene, {} originals "
				"hidden; {}{}{}precombines {} ({} precombined chunks attached){}; applies {}, reverts {} (main view stopped drawing {}), "
				"discarded {}, watchdog {}, attached {}; rotation check {} agree / {} disagree",
				cells, g_cells.size(), building, blind, chunks, hidden, g_active ? "" : "switched off, ", g_workshop ? "workshop mode, " : "",
				g_previsPaused ? "previs active without the query hook (originals shown), " : "", Engine::PrecombinesEnabled() ? "on" : "off", precombined,
				g_precombinesNext.load() < 0 ? "" : g_precombinesNext.load() ? " (on after the next save load)" : " (off after the next save load)", g_totals.applies,
				g_totals.reverts, g_totals.blind, g_totals.discarded, g_totals.watchdog, g_totals.attached, votes.agree, votes.disagree);
			std::uint32_t posedMoved = 0, posedHeld = 0;
			Engine::TakePoseHolds(posedHeld, posedMoved);
			if (posedHeld) {
				logger::info("posed roots held at their placement since the last summary: {} ({} of them already posed: drawn so until then)",
					posedHeld, posedMoved);
			}
			if (const auto lookups = g_poseLookups.exchange(0, std::memory_order_relaxed)) {
				logger::info("pose hold lookups since the last summary: {} ({} of them through the engine's lock: a cell attached since the last poll)",
					lookups, g_poseLocked.exchange(0, std::memory_order_relaxed));
			}
			if (probes) {
				AuditChunks();
				LogPrevis();
			}
			LogBlocked();
			if (census) {
				LogCensus();
			}
		}

		void Reset()
		{
			RevertAll();
			for (auto& [cell, state] : g_cells) {
				DetachProbes(state);
			}
			g_cells.clear();
			g_hiddenRefs.clear();
			g_ours.clear();
			g_parked.clear();
			g_meshByMesh.clear();
			logger::info("game load: everything restored and forgotten");
		}

		// Roots hidden whole under the loading menu leave previs's dynamic objects two ticks on, once a frame's
		// MultiCellVisibilityData::Update has taken in the new cells' queued registrations (FO4-ENGINE-NOTES 5.5d).
		// Meanwhile the query skips them: they are AppCulled, or (owning collision) their meshes are.
		void TakeOutLoaded()
		{
			for (auto& [cell, state] : g_cells) {
				const auto& applied = state.applied;
				if (!applied || !applied->previsPending || applied->fallback || g_frame < applied->frame + 2) {
					continue;
				}
				applied->previsPending = false;
				for (auto& hidden : applied->hidden) {
					if (hidden.object == hidden.root && !hidden.unregistered) {
						hidden.unregistered = Engine::UnregisterIfDynamic(hidden.object.get());
					}
				}
			}
		}

		// TES::HideLoadingMenu, before the menu is told to close (Engine::InstallLoadingMenuHook): every attached cell
		// is gathered, baked, created and applied while the menu still covers the screen, so the first frames draw
		// chunks instead of every original. Runs on the caller's thread: the loader for a save load, when it has
		// attached the cells and no game frame runs (FO4-ENGINE-NOTES 3, 7.15). The probes can't be judged before a
		// frame is drawn: WatchProbes shows a cell's originals again if its probe stays unseen once frames run.
		// Cells not done within iLoadWaitMs finish after the menu, the usual way.
		void LoadPass(const char* a_caller)
		{
			const auto& settings = Settings::Get();
			if (!settings.combineDuringLoad || !g_active || g_halted) {
				return;
			}
			std::unique_lock lock{ g_stateMutex, std::defer_lock };
			if (!lock.try_lock_for(std::chrono::milliseconds(500))) {
				logger::warn("loading menu closing ({}, thread {}): the tick holds the state; combining after the menu", a_caller, GetCurrentThreadId());
				return;
			}
			const auto start = Clock::now();
			if (g_resetPending.exchange(false)) {
				Reset();
			}
			PollCells();
			DrainEvents();
			// What the tick decides (workshop, previs without the query hook): the menu changes neither.
			if (Paused() || (!g_previsHook && !g_cbroLoaded && Engine::PrevisActive())) {
				return;
			}
			const auto applies = g_totals.applies;
			StartJobs(true);
			const auto gathered = Ms(start);
			while (!g_jobs.empty() && Ms(start) < static_cast<double>(settings.loadWaitMs)) {
				ProcessJobs(true);
				if (!g_jobs.empty()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			std::size_t combined = 0;
			for (const auto& [cell, state] : g_cells) {
				combined += state.applied ? 1 : 0;
			}
			logger::info("loading menu closing ({}, thread {}): {} of {} attached cells combined ({} applied now) in {:.0f} ms (gather {:.0f} ms){}",
				a_caller, GetCurrentThreadId(), combined, g_cells.size(), g_totals.applies - applies, Ms(start), gathered,
				g_jobs.empty() ? std::string{} : std::format(", {} still building: they finish after the menu", g_jobs.size()));
		}

		void Tick()
		{
			++g_frame;
			if (g_frame == 1) {
				g_mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
				logger::info("first frame tick (thread {}): precombines {}, previs {}, CBRO {}", GetCurrentThreadId(),
					Engine::PrecombinesEnabled() ? "on" : "off", Engine::PrevisEnabled() ? "on" : "off", g_cbroLoaded ? "loaded" : "not loaded");
			}
			const auto& settings = Settings::Get();
			if (g_resetPending.exchange(false)) {
				Reset();
			}
			CheckSizing();
			PollCells();
			FreeRetiredPoseTables();
			DrainEvents();
			TakeOutLoaded();

			if (settings.toggleHotkey) {
				// Only while the game window has focus (a key pressed in another window doesn't count).
				const auto main = RE::Main::GetSingleton();
				const bool focused = main && GetForegroundWindow() == reinterpret_cast<HWND>(main->hwnd);
				const bool down = focused && (GetAsyncKeyState(static_cast<int>(settings.toggleHotkey)) & 0x8000) != 0;
				if (down && !g_keyDown) {
					Toggle();
				}
				g_keyDown = down;
			}

			static const RE::BSFixedString workshopMenu{ "WorkshopMenu" };
			const auto ui = RE::UI::GetSingleton();
			const bool workshop = ui && ui->GetMenuOpen(workshopMenu);
			if (workshop != g_workshop) {
				g_workshop = workshop;
				if (workshop) {
					RevertAll();
					logger::info("workshop mode: original meshes shown");
				} else {
					for (auto& [cell, state] : g_cells) {
						MarkChanged(state);
					}
				}
			}

			// Without the previs query hook (its site taken, or a previs address missing) nothing tells whether previs
			// drew a frame, and a missing address leaves the chunks out of it. While previs is active the main view
			// draws previs's records instead of group 0 (FO4-ENGINE-NOTES 5.3): show the originals until it is off.
			// Without CBRO nothing suspends previs around the cull, so the tick sees what the cull will; with CBRO the
			// probes catch it.
			const bool previs = !g_previsHook && !g_cbroLoaded && Engine::PrevisActive();
			if (previs != g_previsPaused) {
				g_previsPaused = previs;
				if (previs) {
					RevertAll();
					logger::info("previs is active and the previs query hook is missing: original meshes shown while it is");
				} else {
					for (auto& [cell, state] : g_cells) {
						MarkChanged(state);
					}
					logger::info("previs is off: combining again");
				}
			}

			WatchProbes();

			// Fallbacks from engine callbacks become full reverts here.
			for (auto& [cell, state] : g_cells) {
				if (state.applied && state.applied->fallback) {
					Revert(state);
					MarkChanged(state);
				}
			}

			ProcessJobs();
			if (!Paused()) {
				Watchdog();
				StartJobs();
			}
			Summary();
		}

		void UpdateHook(RE::PlayerCharacter* a_this, float a_delta)
		{
			using func_t = void (*)(RE::PlayerCharacter*, float);
			reinterpret_cast<func_t>(g_originalUpdate)(a_this, a_delta);
			std::unique_lock lock{ g_stateMutex, std::try_to_lock };
			if (!lock) {
				return;  // a loading-menu pass on another thread (LoadPass)
			}
			const auto start = Clock::now();
			try {
				Tick();
			} catch (const std::exception& e) {
				if (!g_halted) {
					g_halted = true;
					logger::error("tick failed ({}): combining stopped", e.what());
				}
			}
			const auto   end = Clock::now();
			const double frame = std::chrono::duration<double, std::milli>(start - g_lastTick).count();
			if (g_lastTick != Clock::time_point{} && frame < 1000.0) {
				const double tick = std::chrono::duration<double, std::milli>(end - start).count();
				auto&        times = g_frameTimes;
				++times.frames;
				times.frameMs += frame;
				times.frameMax = std::max(times.frameMax, frame);
				times.tickMs += tick;
				times.tickMax = std::max(times.tickMax, tick);
			}
			g_lastTick = start;
		}
	}

	void Install()
	{
		std::thread(WorkerMain).detach();  // lives as long as the process; never joined at exit
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE::PlayerCharacter[0] };
		g_originalUpdate = vtable.write_vfunc(kUpdateSlot, &UpdateHook);
		logger::info("PlayerCharacter::Update (slot 0x{:X}) hooked; bake thread started", kUpdateSlot);
		g_previsHook = Engine::InstallPrevisQueryHook(&OnPrevisQuery);
		if (g_previsHook) {
			logger::info("previs query hooked (Render_PreUI+0x80); chunks are registered as previs dynamic objects, previs stays as it is");
		}
		g_resetHook = Engine::InstallGameResetHook(&OnGameReset);
		if (g_resetHook) {
			logger::info("game reset hooked (Main::PerformGameReset+0x2ED): F3 changes the precombine switch at the next save load");
		} else {
			logger::warn("game reset not hooked: F3 only swaps chunks and originals; precombines stay as set at startup");
		}
		if (Engine::InstallPoseHook(&HoldsPose)) {
			logger::info("NiTransformController::Update hooked: a precombined static's model can't pose its root away from its placement "
						 "while precombines are off");
		} else {
			logger::warn("NiTransformController::Update not hooked: precombined statics whose model poses its root draw posed until combined");
		}
		if (Settings::Get().combineDuringLoad) {
			const auto sites = Engine::InstallLoadingMenuHook(&LoadPass);
			logger::info("TES::HideLoadingMenu hooked at {} call sites: cells are combined before the loading menu closes", sites);
		}
		if (Settings::Get().probes) {
			logger::info("GPU probe: {} of 4 Render_PreUI calls wrapped (pipeline statistics over the whole frame in each summary)",
				Engine::InstallGpuStatsProbe());
		} else {
			logger::info("probes off (bProbes=0): no GPU probe, census, chunk audit, previs line or self-check");
		}
	}

	void OnGameDataReady()
	{
		const auto& settings = Settings::Get();
		g_cbroLoaded = GetModuleHandleW(L"CBRO.dll") != nullptr;  // every plugin is loaded by now; read before a load pass
		g_configuredPrecombines = Engine::PrecombinesEnabled();
		if (settings.disablePrecombines) {
			if (Engine::PrecombinesEnabled()) {
				Engine::SetPrecombinesEnabled(false);
				logger::info("precombines switched off (bUseCombinedObjects = 0): every static loads its own 3D and is combined here");
			} else {
				logger::info("precombines already off");
			}
		} else {
			logger::info("precombines left as configured ({}): only references outside them are combined", Engine::PrecombinesEnabled() ? "on" : "off");
		}

		RE::CellAttachDetachEventSource::CellAttachDetachEventSourceSingleton::GetSingleton().source.RegisterSink(&g_cellSink);
		if (const auto source = RE::TESObjectLoadedEvent::GetEventSource()) {
			source->RegisterSink(&g_objectSink);
		}
		logger::info("cell attach/detach and object-loaded sinks registered");
	}

	// Messages arrive on the loading thread: the work waits for the next game frame.
	void OnPreLoadGame()
	{
		g_resetPending = true;
	}

}
