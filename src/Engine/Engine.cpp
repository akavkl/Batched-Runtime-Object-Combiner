#include "Engine/Engine.h"

#include <intrin.h>
#include <unordered_set>

namespace RC::Engine
{
	namespace
	{
		// Ids (OG 1.10.163), addresses as resolved there.
		constexpr std::uint64_t kCloneID = 604942;             // NiObject::Clone(void)                 0x141B94B00
		constexpr std::uint64_t kCreateTriShapeID = 99624;     // Renderer::CreateTriShape(uint&, void*, u64, u16*, uint) 0x141D0BC70
		constexpr std::uint64_t kDecRefTriShapeID = 1039714;   // Renderer::DecRef(TriShape*)          0x141D0C5D0
		constexpr std::uint64_t kRendererID = 1378294;         // the BSGraphics::Renderer object       0x1461E0900
		constexpr std::uint64_t kSetRendererDataID = 444114;   // BSGeometry::SetRendererData(void*)   0x141BB15E0
		constexpr std::uint64_t kNodeCtorID = 20633;           // NiNode::NiNode(uint)                 0x141B98920
		constexpr std::uint64_t kFadeNodeCtorID = 668955;      // BSFadeNode::BSFadeNode()             0x1427D04C0
		constexpr std::uint64_t kFadeRangeID = 1417061;        // ConfigureFadeNodeRange(BSFadeNode*, float uGrids, bool) 0x1401CFAC0
		constexpr std::uint64_t kUGridsToLoadID = 504589;      // uGridsToLoad:General value (int)     0x1436D9B50
		constexpr std::uint64_t kFadeFrameID = 734919;         // the fade code's frame number (int)   0x1438C4E6C
		constexpr std::uint64_t kUpdateID = 121052;            // NiAVObject::Update(NiUpdateData&)    0x141BA3BE0
		constexpr std::uint64_t kUseCombinedID = 267057;       // bUseCombinedObjects:General value    0x1436E9C00
		constexpr std::uint64_t kPrevisEnabledID = 652211;     // BSPreCulledObjects::QWantEnabled          0x142809E60
		constexpr std::uint64_t kPrevisActiveID = 917969;      // BSPreCulledObjects::QEnabled             0x142809E30
		constexpr std::uint64_t kSetRangeID = 1164193;         // BSFadeNode::SetRange(near, far)          0x1427D1CF0
		constexpr std::uint64_t kFadeMultsID = 804710;         // float[]: fade-out multiplier per LOD-mult type 0x1438C4E18
		constexpr std::uint64_t kFadeDistMultID = 515159;      // float: fDistanceMultiplier:LOD           0x1438C4E64
		constexpr std::uint64_t kFadingOnID = 1220201;         // byte: fading enabled                     0x1438C4E44
		constexpr std::uint64_t kTESID = 1194835;              // TES* singleton
		// The previs query and its dynamic objects (FO4-ENGINE-NOTES 5.5b, 5.5d).
		constexpr std::uint64_t kRenderPreUIID = 984743;       // DrawWorld::Render_PreUI                  0x142857480
		constexpr std::size_t   kPrevisQuerySite = 0x80;       //   call 1264353 (E8 rel32)
		constexpr std::uint64_t kPrevisQueryID = 1264353;      // the per-frame previs query (if active)   0x14284F1D0
		constexpr std::uint64_t kVisibilityID = 787073;        // MultiCellVisibilityData* (global)        0x1458D0AA8
		constexpr std::uint64_t kRegisterDynamicID = 272586;   // MultiCellVisibilityData::RegisterDynamicObject   0x1427AF460
		constexpr std::uint64_t kUnregisterDynamicID = 697930; // MultiCellVisibilityData::UnregisterDynamicObject 0x1427AF590
		constexpr std::uint64_t kGridGetID = 1330136;          // GridCellArray::Get(x, y)
		constexpr std::uint32_t kCombinedRefsExtra = 0xC5;     // ExtraCombinedRefs (vtable 130663)
		// A save load clears every cell, then purges the cell buffer (FO4-ENGINE-NOTES 7.12).
		constexpr std::uint64_t kGameResetID = 124452;         // Main::PerformGameReset                   0x140D3B800
		constexpr std::size_t   kGameResetPurgeSite = 0x2ED;   //   call 1075115 (E8 rel32), after TES::ClearAllCells
		constexpr std::uint64_t kPurgeCellsID = 1075115;       // TES::PurgeBufferedCells                  0x1400F70A0
		// The loading menu closes in TES::HideLoadingMenu (FO4-ENGINE-NOTES 7.15).
		constexpr std::uint64_t kHideLoadingMenuID = 874976;   // TES::HideLoadingMenu                     0x1400FB360
		constexpr std::size_t   kCanMergeSlot = 0x31;          // BSShaderProperty::CanMerge(BSShaderProperty*)

		// TES and the exterior cell grid (layouts from CBRO's Compat.h).
		struct GridCellArray
		{
			void*         vtable;     // 00
			std::int32_t  centerX;    // 08
			std::int32_t  centerY;    // 0C
			std::uint32_t dimension;  // 10
		};
		struct TES
		{
			std::uint8_t       pad000[0x18];
			GridCellArray*     gridCells;     // 018
			std::uint8_t       pad020[0x58 - 0x20];
			RE::TESObjectCELL* interiorCell;  // 058
		};
		static_assert(offsetof(TES, gridCells) == 0x18 && offsetof(TES, interiorCell) == 0x58);

		struct Addresses
		{
			std::uintptr_t clone{ 0 };
			std::uintptr_t createTriShape{ 0 };
			std::uintptr_t decRefTriShape{ 0 };
			std::uintptr_t renderer{ 0 };
			std::uintptr_t setRendererData{ 0 };
			std::uintptr_t nodeCtor{ 0 };
			std::uintptr_t fadeNodeCtor{ 0 };
			std::uintptr_t fadeRange{ 0 };
			std::uintptr_t uGrids{ 0 };
			std::uintptr_t fadeFrame{ 0 };
			std::uintptr_t update{ 0 };
			std::uintptr_t tes{ 0 };
			std::uintptr_t gridGet{ 0 };
			std::uintptr_t useCombined{ 0 };
			std::uintptr_t previsEnabled{ 0 };
			std::uintptr_t previsActive{ 0 };
			std::uintptr_t setRange{ 0 };
			std::uintptr_t fadeMults{ 0 };
			std::uintptr_t fadeDistMult{ 0 };
			std::uintptr_t fadingOn{ 0 };
			std::uintptr_t previsQuerySite{ 0 };
			std::uintptr_t previsQuery{ 0 };
			std::uintptr_t visibility{ 0 };
			std::uintptr_t registerDynamic{ 0 };
			std::uintptr_t unregisterDynamic{ 0 };
			bool           previsFeed{ false };  // every previs address resolved
			std::uintptr_t triShapeVtable{ 0 };
			std::uintptr_t meshLODTriShapeVtable{ 0 };
			std::uintptr_t nodeVtable{ 0 };
			std::uintptr_t fadeNodeVtable{ 0 };
			std::uintptr_t lightingShaderVtable{ 0 };
			std::uintptr_t effectShaderVtable{ 0 };
		};
		Addresses g;

		std::uintptr_t Resolve(std::uint64_t a_id, const char* a_name, bool& a_ok)
		{
			const auto address = OG(a_id).address();
			if (!address) {
				logger::error("engine id {} ({}) did not resolve", a_id, a_name);
				a_ok = false;
			}
			return address;
		}

		std::uintptr_t VtableOf(std::span<const REL::ID> a_ids, const char* a_name, bool& a_ok)
		{
			const auto address = a_ids.empty() ? 0 : a_ids[0].address();
			if (!address) {
				logger::error("vtable of {} did not resolve", a_name);
				a_ok = false;
			}
			return address;
		}

		[[nodiscard]] std::uintptr_t VtableOfObject(const void* a_object) noexcept
		{
			return a_object ? *static_cast<const std::uintptr_t*>(a_object) : 0;
		}
	}

	bool Init()
	{
		bool ok = true;
		g.clone = Resolve(kCloneID, "NiObject::Clone", ok);
		g.createTriShape = Resolve(kCreateTriShapeID, "Renderer::CreateTriShape", ok);
		g.decRefTriShape = Resolve(kDecRefTriShapeID, "Renderer::DecRef(TriShape)", ok);
		g.renderer = Resolve(kRendererID, "Renderer", ok);
		g.setRendererData = Resolve(kSetRendererDataID, "BSGeometry::SetRendererData", ok);
		g.nodeCtor = Resolve(kNodeCtorID, "NiNode::NiNode", ok);
		g.fadeNodeCtor = Resolve(kFadeNodeCtorID, "BSFadeNode::BSFadeNode", ok);
		g.fadeRange = Resolve(kFadeRangeID, "ConfigureFadeNodeRange", ok);
		g.uGrids = Resolve(kUGridsToLoadID, "uGridsToLoad", ok);
		g.fadeFrame = Resolve(kFadeFrameID, "fade frame number", ok);
		g.update = Resolve(kUpdateID, "NiAVObject::Update", ok);
		g.tes = Resolve(kTESID, "TES", ok);
		g.gridGet = Resolve(kGridGetID, "GridCellArray::Get", ok);
		g.useCombined = Resolve(kUseCombinedID, "bUseCombinedObjects", ok);
		g.previsEnabled = Resolve(kPrevisEnabledID, "previs query", ok);
		g.previsActive = Resolve(kPrevisActiveID, "previs active query", ok);
		g.setRange = Resolve(kSetRangeID, "BSFadeNode::SetRange", ok);
		g.fadeMults = Resolve(kFadeMultsID, "fade multipliers", ok);
		g.fadeDistMult = Resolve(kFadeDistMultID, "fDistanceMultiplier", ok);
		g.fadingOn = Resolve(kFadingOnID, "fading enabled", ok);
		// Optional: without them chunks are not drawn while previs draws the main view (the originals are shown then).
		bool feed = true;
		const auto renderPreUI = Resolve(kRenderPreUIID, "Render_PreUI", feed);
		g.previsQuerySite = renderPreUI ? renderPreUI + kPrevisQuerySite : 0;
		g.previsQuery = Resolve(kPrevisQueryID, "previs query", feed);
		g.visibility = Resolve(kVisibilityID, "MultiCellVisibilityData", feed);
		g.registerDynamic = Resolve(kRegisterDynamicID, "RegisterDynamicObject", feed);
		g.unregisterDynamic = Resolve(kUnregisterDynamicID, "UnregisterDynamicObject", feed);
		g.previsFeed = feed;
		g.triShapeVtable = VtableOf(RE::VTABLE::BSTriShape, "BSTriShape", ok);
		g.meshLODTriShapeVtable = VtableOf(RE::VTABLE::BSMeshLODTriShape, "BSMeshLODTriShape", ok);
		g.nodeVtable = VtableOf(RE::VTABLE::NiNode, "NiNode", ok);
		g.fadeNodeVtable = VtableOf(RE::VTABLE::BSFadeNode, "BSFadeNode", ok);
		g.lightingShaderVtable = VtableOf(RE::VTABLE::BSLightingShaderProperty, "BSLightingShaderProperty", ok);
		g.effectShaderVtable = VtableOf(RE::VTABLE::BSEffectShaderProperty, "BSEffectShaderProperty", ok);
		return ok;
	}

	bool IsExactTriShape(const RE::NiAVObject* a_object) noexcept
	{
		return VtableOfObject(a_object) == g.triShapeVtable;
	}

	bool IsExactMeshLODTriShape(const RE::NiAVObject* a_object) noexcept
	{
		return VtableOfObject(a_object) == g.meshLODTriShapeVtable;
	}

	bool IsExactNode(const RE::NiAVObject* a_object) noexcept
	{
		const auto vtable = VtableOfObject(a_object);
		return vtable == g.nodeVtable || vtable == g.fadeNodeVtable;
	}

	bool IsPlainNode(const RE::NiAVObject* a_object) noexcept
	{
		return VtableOfObject(a_object) == g.nodeVtable;
	}

	bool IsLightingShader(const void* a_property) noexcept
	{
		return VtableOfObject(a_property) == g.lightingShaderVtable;
	}

	bool IsEffectShader(const void* a_property) noexcept
	{
		return VtableOfObject(a_property) == g.effectShaderVtable;
	}

	bool CanMerge(void* a_property, void* a_other)
	{
		using func_t = bool (*)(void*, void*);
		const auto vtable = *static_cast<func_t* const*>(a_property);
		return vtable[kCanMergeSlot](a_property, a_other);
	}

	RE::NiAVObject* Clone(RE::NiAVObject* a_object, bool a_plain)
	{
		using func_t = RE::NiObject* (*)(RE::NiObject*);
		const auto clone = static_cast<RE::NiAVObject*>(reinterpret_cast<func_t>(g.clone)(a_object));
		// BSMeshLODTriShape is BSTriShape plus three u32 after it; its own virtuals only stream, clone and pick the
		// drawn LOD prefix, and both free through the same allocator. Becoming the base class: vtable and type byte
		// as BSTriShape's CreateClone sets them, mesh-LOD flag cleared (FO4-ENGINE-NOTES 7.11).
		if (a_plain && clone && IsExactMeshLODTriShape(clone)) {
			*reinterpret_cast<std::uintptr_t*>(clone) = g.triShapeVtable;
			*Field<std::uint8_t>(clone, Offset::kGeometryType) = 3;
			clone->flags.flags &= ~ObjectFlag::kMeshLOD;
		}
		return clone;
	}

	void* CreateTriShape(const void* a_vertices, std::uint32_t a_vertexBytes, std::uint64_t a_desc, const std::uint16_t* a_indices, std::uint32_t a_indexCount)
	{
		using func_t = void* (*)(void*, std::uint32_t&, const void*, std::uint64_t, const std::uint16_t*, std::uint32_t);
		std::uint32_t bytes = a_vertexBytes;
		return reinterpret_cast<func_t>(g.createTriShape)(reinterpret_cast<void*>(g.renderer), bytes, a_vertices, a_desc, a_indices, a_indexCount);
	}

	void ReleaseTriShape(void* a_triShape)
	{
		if (a_triShape) {
			using func_t = void (*)(void*, void*);
			reinterpret_cast<func_t>(g.decRefTriShape)(reinterpret_cast<void*>(g.renderer), a_triShape);
		}
	}

	bool TriShapeReady(const void* a_triShape) noexcept
	{
		for (const auto offset : { Offset::kVertexBuffer, Offset::kIndexBuffer }) {
			const auto buffer = *Field<const std::byte* const>(a_triShape, offset);
			if (buffer && std::atomic_ref(*Field<std::uint32_t>(buffer, Offset::kBufferPending)).load(std::memory_order_acquire) != 0) {
				return false;
			}
		}
		return true;
	}

	void SetGeometry(RE::NiAVObject* a_shape, void* a_triShape, std::uint32_t a_vertices, std::uint32_t a_triangles)
	{
		// What BSTriShape::CopyMembers does when the renderer data changes: release the old, install the new.
		const auto old = *Field<void*>(a_shape, Offset::kRendererData);
		using func_t = void (*)(RE::NiAVObject*, void*);
		reinterpret_cast<func_t>(g.setRendererData)(a_shape, a_triShape);  // also copies the vertex desc to +0x150
		ReleaseTriShape(old);
		*Field<std::uint32_t>(a_shape, Offset::kTriangles) = a_triangles;
		*Field<std::uint16_t>(a_shape, Offset::kVertices) = static_cast<std::uint16_t>(a_vertices);
	}

	namespace
	{
		void SettlePreviousWorld(RE::NiAVObject* a_object)
		{
			a_object->previousWorld = a_object->world;
			if (const auto node = a_object->IsNode()) {
				for (const auto& child : node->children) {
					if (child) {
						SettlePreviousWorld(child.get());
					}
				}
			}
		}
	}

	void UpdateStatic(RE::NiAVObject* a_object)
	{
		RE::NiUpdateData data{};
		using func_t = void (*)(RE::NiAVObject*, RE::NiUpdateData&);
		reinterpret_cast<func_t>(g.update)(a_object, data);
		SettlePreviousWorld(a_object);
	}

	RE::NiNode* NewNode(std::uint16_t a_children)
	{
		auto memory = RE::aligned_alloc(alignof(RE::NiNode), sizeof(RE::NiNode));
		if (!memory) {
			return nullptr;
		}
		std::memset(memory, 0, sizeof(RE::NiNode));
		using func_t = RE::NiNode* (*)(void*, std::uint32_t);
		return reinterpret_cast<func_t>(g.nodeCtor)(memory, a_children);
	}

	FadeRange ReadFadeRange(const RE::NiAVObject* a_root) noexcept
	{
		FadeRange range;
		if (VtableOfObject(a_root) == g.fadeNodeVtable) {
			const auto node = static_cast<const RE::BSFadeNode*>(a_root);
			range.nearDistance = *Field<const float>(node, Offset::kFadeNear);
			range.farDistance = *Field<const float>(node, Offset::kFadeFar);
			range.type = *Field<const std::uint8_t>(node, Offset::kFadeType);
			if (!std::isfinite(range.nearDistance) || !std::isfinite(range.farDistance)) {
				range.farDistance = 0.0F;  // Init's FLT_MAX is finite; anything else is no range
			}
		}
		return range;
	}

	float FadeStartDistance(const FadeRange& a_range) noexcept
	{
		constexpr float kNever = std::numeric_limits<float>::infinity();
		// Type 6 only fades in; the table has entries up to type 10 (TES::InitFadeOutMultipliers, 7.9).
		if (!a_range.Valid() || a_range.type == 6 || a_range.type > 10 || *reinterpret_cast<const std::uint8_t*>(g.fadingOn) == 0) {
			return kNever;
		}
		const float mult = reinterpret_cast<const float*>(g.fadeMults)[a_range.type];
		if (!(mult > 0.0F)) {
			return kNever;  // ComputeFadeAmount snaps such nodes to 1
		}
		const auto camera = RE::Main::WorldRootCamera();
		float      lodAdjust = camera ? *Field<const float>(camera, 0x194) : 1.0F;
		lodAdjust = lodAdjust > 0.0F ? lodAdjust : 1.0F;
		float distanceMult = *reinterpret_cast<const float*>(g.fadeDistMult);
		distanceMult = distanceMult > 0.0F ? distanceMult : 1.0F;
		// ComputeFadeAmount: s = lodAdjust / mult x distance; fading starts once s passes distanceMult x near.
		return distanceMult * a_range.nearDistance * mult / lodAdjust;
	}

	float LoadedReach() noexcept
	{
		const auto uGrids = std::clamp(*reinterpret_cast<const std::int32_t*>(g.uGrids), 1, 63);
		return static_cast<float>(uGrids + 1) * 2048.0F * 1.4142F;
	}

	RE::BSFadeNode* NewChunkFadeNode(RE::NiAVObject* a_shape, float a_fade, const FadeRange& a_range)
	{
		static_assert(sizeof(RE::BSFadeNode) == 0x1C0);
		const auto uGrids = *reinterpret_cast<const std::int32_t*>(g.uGrids);
		if (!a_range.Valid() && (uGrids < 1 || uGrids > 63)) {
			static bool logged = false;
			if (!std::exchange(logged, true)) {
				logger::error("uGridsToLoad reads {}: chunks are left as they are", uGrids);
			}
			return nullptr;
		}
		auto memory = RE::aligned_alloc(0x10, sizeof(RE::BSFadeNode));  // as ProcessTriShape: 0x1C0 bytes, 16-aligned
		if (!memory) {
			return nullptr;
		}
		std::memset(memory, 0, sizeof(RE::BSFadeNode));
		using ctor_t = RE::BSFadeNode* (*)(void*);
		const auto node = reinterpret_cast<ctor_t>(g.fadeNodeCtor)(memory);
		node->local.MakeIdentity();

		if (a_range.Valid()) {
			// The members' own range and type: the chunk fades out where they would (7.9).
			*Field<std::uint8_t>(node, Offset::kFadeType) = a_range.type;
			using range_t = void (*)(RE::BSFadeNode*, float, float);
			reinterpret_cast<range_t>(g.setRange)(node, a_range.nearDistance, a_range.farDistance);
		} else {
			// The range first, while the node's world bound is still 0: near / far then come out as a precombined
			// chunk's, whatever its radius.
			using range_t = void (*)(RE::BSFadeNode*, float, bool);
			reinterpret_cast<range_t>(g.fadeRange)(node, static_cast<float>(uGrids), false);
		}

		node->flags.flags |= ObjectFlag::kPickChildren | (a_shape->flags.flags & ObjectFlag::kMeshLOD);
		if (a_shape->flags.flags & ObjectFlag::kMeshLOD) {
			// A BSMeshLODTriShape draws the LOD prefix of this node's level, nothing at level 0: start at full detail,
			// settled, as DetermineMeshLODLevel leaves a node with mesh LOD off (FO4-ENGINE-NOTES 7.11).
			node->meshLODFadingLevel = 2;
			node->currentMeshLODLevel = 3;
			node->previousMeshLODLevel = 3;
		}
		node->previousMaxA = 1.0F;
		if (a_fade >= 1.0F) {
			// Faded in, as TESObjectCELL::UpdateFadeNodes leaves a cell's fade nodes after a load.
			node->currentFade = 1.0F;
			node->currentDecalFade = 1.0F;
			node->flags.flags |= ObjectFlag::kFadeComplete;
		} else {
			// Mid fade-in, as AttachCombinedObjectArt starts a chunk (at 0) and then OnVisibleFrameOnly.
			node->currentFade = std::max(a_fade, 0.0F);
			node->currentDecalFade = node->currentFade;
			node->flags.flags &= ~ObjectFlag::kFadeComplete;
			node->frameCounter = *reinterpret_cast<const std::int32_t*>(g.fadeFrame);
		}
		node->AttachChild(a_shape, true);
		return node;
	}

	RE::BSFadeNode* NewProbe()
	{
		auto memory = RE::aligned_alloc(0x10, sizeof(RE::BSFadeNode));
		if (!memory) {
			return nullptr;
		}
		std::memset(memory, 0, sizeof(RE::BSFadeNode));
		using ctor_t = RE::BSFadeNode* (*)(void*);
		const auto node = reinterpret_cast<ctor_t>(g.fadeNodeCtor)(memory);
		node->local.MakeIdentity();
		node->name = RE::BSFixedString("RuntimeCombiner probe");
		// Init leaves near and far at FLT_MAX, so ComputeFadeAmount returns 1 at any distance (7.9).
		node->currentFade = 1.0F;
		node->currentDecalFade = 1.0F;
		node->previousMaxA = 1.0F;
		node->flags.flags |= ObjectFlag::kAlwaysDraw | ObjectFlag::kFadeComplete;
		return node;
	}

	bool TakeProbeSeen(RE::NiAVObject* a_probe) noexcept
	{
		auto&      flags = a_probe->flags.flags;
		const bool seen = (flags & ObjectFlag::kAccumulated) != 0;
		flags &= ~ObjectFlag::kAccumulated;
		return seen;
	}

	const char* ClassName(const RE::NiAVObject* a_object)
	{
		const auto rtti = a_object ? a_object->GetRTTI() : nullptr;
		return rtti && rtti->GetName() ? rtti->GetName() : "?";
	}

	float FadeIn(const RE::NiAVObject* a_root) noexcept
	{
		if (VtableOfObject(a_root) != g.fadeNodeVtable || (a_root->flags.flags & ObjectFlag::kFadeComplete)) {
			return 1.0F;
		}
		return std::clamp(static_cast<const RE::BSFadeNode*>(a_root)->currentFade, 0.0F, 1.0F);
	}

	std::vector<RE::TESObjectCELL*> LoadedCells()
	{
		std::vector<RE::TESObjectCELL*> cells;
		const auto                      tes = *reinterpret_cast<TES**>(g.tes);
		if (!tes) {
			return cells;
		}
		const auto attached = [](RE::TESObjectCELL* a_cell) {
			return a_cell && a_cell->cellState.get() == RE::TESObjectCELL::CELL_STATE::kAttached;
		};
		if (tes->interiorCell) {
			if (attached(tes->interiorCell)) {
				cells.push_back(tes->interiorCell);
			}
			return cells;
		}
		const auto grid = tes->gridCells;
		if (!grid) {
			return cells;
		}
		using func_t = RE::TESObjectCELL** (*)(const GridCellArray*, std::uint32_t, std::uint32_t);  // GridCell { cell }
		for (std::uint32_t x = 0; x < grid->dimension; ++x) {
			for (std::uint32_t y = 0; y < grid->dimension; ++y) {
				const auto gridCell = reinterpret_cast<func_t>(g.gridGet)(grid, x, y);
				if (gridCell && attached(*gridCell)) {
					cells.push_back(*gridCell);
				}
			}
		}
		return cells;
	}

	bool InInterior() noexcept
	{
		const auto tes = *reinterpret_cast<TES**>(g.tes);
		return tes && tes->interiorCell;
	}

	namespace
	{
		// TESObjectCELL +0xD0 loadedData; its +0xC0 / +0xD0 array of attached precombined chunks (7.12).
		bool ReadPrecombinedList(const RE::TESObjectCELL* a_cell, RE::NiAVObject* const*& a_list, std::uint32_t& a_count) noexcept
		{
			__try {
				const auto loaded = *reinterpret_cast<const std::byte* const*>(reinterpret_cast<const std::byte*>(a_cell) + 0xD0);
				a_list = loaded ? *reinterpret_cast<RE::NiAVObject* const* const*>(loaded + 0xC0) : nullptr;
				a_count = loaded && a_list ? *reinterpret_cast<const std::uint32_t*>(loaded + 0xD0) : 0;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool ReadRadius(RE::NiAVObject* const* a_list, std::uint32_t a_index, float& a_radius) noexcept
		{
			__try {
				const auto object = a_list[a_index];
				a_radius = object ? object->worldBound.fRadius : 0.0F;
				return object != nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	std::uint32_t PrecombinedChunks(const RE::TESObjectCELL* a_cell, std::vector<float>* a_radii)
	{
		RE::NiAVObject* const* list = nullptr;
		std::uint32_t          count = 0;
		if (!ReadPrecombinedList(a_cell, list, count)) {
			return 0;
		}
		for (std::uint32_t i = 0; a_radii && i < count; ++i) {
			if (float radius = 0.0F; ReadRadius(list, i, radius)) {
				a_radii->push_back(radius);
			}
		}
		return count;
	}

	namespace
	{
		std::uint32_t CountMeshes(RE::NiAVObject* a_object)
		{
			if (!a_object || a_object->GetAppCulled()) {
				return 0;
			}
			if (a_object->IsGeometry()) {
				return 1;
			}
			const auto    node = a_object->IsNode();
			std::uint32_t count = 0;
			if (node) {
				for (auto& child : node->children) {
					count += CountMeshes(child.get());
				}
			}
			return count;
		}
	}

	void WalkEntries(const RE::TESObjectCELL* a_cell, std::vector<WalkEntry>& a_out, std::uint32_t& a_hidden)
	{
		const auto loaded = a_cell ? a_cell->loadedData : nullptr;
		const auto root = loaded ? loaded->cell3D.get() : nullptr;
		if (!root || root->children.capacity() <= 9) {
			return;
		}
		std::unordered_set<const RE::NiAVObject*> precombined;
		RE::NiAVObject* const*                    list = nullptr;
		std::uint32_t                             count = 0;
		if (ReadPrecombinedList(a_cell, list, count)) {
			precombined.insert(list, list + count);
		}
		for (const std::uint16_t index : { std::uint16_t{ 3 }, std::uint16_t{ 9 } }) {
			const auto cellNode = root->children[index].get();
			const auto node = cellNode ? cellNode->IsNode() : nullptr;
			const auto add = [&](RE::NiAVObject* a_object) {
				if (!a_object) {
					return;
				}
				if (a_object->GetAppCulled()) {
					++a_hidden;
					return;
				}
				a_out.push_back({ a_object, CountMeshes(a_object), precombined.contains(a_object), index == 9 });
			};
			if (!node) {
				continue;
			}
			for (auto& pointer : node->children) {
				const auto child = pointer.get();
				if (child && IsPlainNode(child) && !child->GetAppCulled()) {
					for (auto& member : static_cast<RE::NiNode*>(child)->children) {
						add(member.get());
					}
				} else {
					add(child);
				}
			}
		}
	}

	const void* CombinedRefs(const RE::TESObjectCELL* a_cell) noexcept
	{
		const auto extras = a_cell ? a_cell->extraList.get() : nullptr;
		return extras ? extras->GetByType(static_cast<RE::EXTRA_DATA_TYPE>(kCombinedRefsExtra)) : nullptr;
	}

	bool InCombinedRefs(const void* a_combinedRefs, std::uint32_t a_formID) noexcept
	{
		if (!a_combinedRefs) {
			return false;
		}
		__try {
			// ExtraCombinedRefs +0x18: BSTSet<u32 formID>, CommonLibF4's BSTScatterTable layout (capacity +0x24,
			// sentinel +0x30, entries +0x40: what TESObjectCELL::AddReference reads, FO4-ENGINE-NOTES 7.12).
			return reinterpret_cast<const RE::BSTSet<std::uint32_t>*>(static_cast<const std::byte*>(a_combinedRefs) + 0x18)->contains(a_formID);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool PrecombinesEnabled() noexcept
	{
		return *reinterpret_cast<const volatile std::uint8_t*>(g.useCombined) != 0;
	}

	void SetPrecombinesEnabled(bool a_enabled) noexcept
	{
		*reinterpret_cast<volatile std::uint8_t*>(g.useCombined) = a_enabled ? 1 : 0;
	}

	bool ReadChunkView(const RE::NiAVObject* a_shape, const RE::BSFadeNode* a_fadeNode, ChunkView& a_out) noexcept
	{
		__try {
			const auto property = *Field<const std::byte* const>(a_shape, Offset::kShaderProperty);
			if (!property) {
				return false;
			}
			a_out.passes = *Field<const void* const>(property, Offset::kRenderPasses) != nullptr;
			a_out.alpha = *Field<const float>(property, Offset::kShaderAlpha);
			a_out.material = reinterpret_cast<const RE::NiObjectNET*>(property)->name.c_str();
			a_out.shaderFlags = *Field<const std::uint64_t>(property, Offset::kShaderFlags);
			const auto& now = a_shape->world;
			const auto& before = a_shape->previousWorld;
			a_out.moving = now.translate.x != before.translate.x || now.translate.y != before.translate.y ||
			               now.translate.z != before.translate.z || now.scale != before.scale;
			for (int i = 0; i < 3 && !a_out.moving; ++i) {
				for (int j = 0; j < 3 && !a_out.moving; ++j) {
					a_out.moving = now.rotate.entry[i].pt[j] != before.rotate.entry[i].pt[j];
				}
			}
			if (a_fadeNode) {
				// BSFadeNode::OnVisible stamps the fade frame number when a view files the node (7.8).
				const auto frame = *reinterpret_cast<const volatile std::int32_t*>(g.fadeFrame);
				a_out.inView = frame - a_fadeNode->frameCounter <= 3;
				a_out.fade = a_fadeNode->currentFade;
				a_out.owner = *Field<const void* const>(property, Offset::kShaderFadeNode) == a_fadeNode;
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool PrevisEnabled()
	{
		using func_t = bool (*)();
		return reinterpret_cast<func_t>(g.previsEnabled)();
	}

	bool PrevisActive()
	{
		using func_t = bool (*)();
		return reinterpret_cast<func_t>(g.previsActive)();
	}

	bool SeenByMainView(const RE::NiAVObject* a_object) noexcept
	{
		return a_object && (a_object->flags.flags & ObjectFlag::kAccumulated) != 0;
	}

	namespace
	{
		using DynamicFunc = void (*)(void*, RE::NiAVObject*);

		// The engine's MultiCellVisibilityData: what TESObjectREFR::RegisterWithPrecomputedVisibility passes (5.5d).
		[[nodiscard]] void* Visibility() noexcept
		{
			return g.previsFeed ? *reinterpret_cast<void* const*>(g.visibility) : nullptr;
		}
	}

	bool RegisterPrevisObject(RE::NiAVObject* a_object) noexcept
	{
		const auto visibility = Visibility();
		if (!visibility || !a_object) {
			return false;
		}
		reinterpret_cast<DynamicFunc>(g.registerDynamic)(visibility, a_object);
		return true;
	}

	void UnregisterPrevisObject(RE::NiAVObject* a_object) noexcept
	{
		if (const auto visibility = Visibility(); visibility && a_object) {
			reinterpret_cast<DynamicFunc>(g.unregisterDynamic)(visibility, a_object);
		}
	}

	namespace
	{
		// MultiCellVisibilityData's dynamic-object set: a BSTSet<NiAVObject*> whose table starts at +0x88 (capacity
		// +0x94, free +0x98, entries +0xB0; FO4-ENGINE-NOTES 5.5d), the layout of CommonLibF4's BSTScatterTable.
		constexpr std::size_t kDynamicSet = 0x88;
		using DynamicSet = RE::BSTSet<RE::NiAVObject*>;

		[[nodiscard]] bool InDynamicSet(const void* a_visibility, RE::NiAVObject* a_object) noexcept
		{
			__try {
				return reinterpret_cast<const DynamicSet*>(static_cast<const std::byte*>(a_visibility) + kDynamicSet)->contains(a_object);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	bool UnregisterIfDynamic(RE::NiAVObject* a_object) noexcept
	{
		const auto visibility = Visibility();
		if (!visibility || !a_object || !InDynamicSet(visibility, a_object)) {
			return false;
		}
		reinterpret_cast<DynamicFunc>(g.unregisterDynamic)(visibility, a_object);
		return true;
	}

	std::uint32_t PrevisDynamicObjects() noexcept
	{
		const auto visibility = static_cast<const std::byte*>(Visibility());
		if (!visibility) {
			return 0;
		}
		__try {
			const auto capacity = *reinterpret_cast<const std::uint32_t*>(visibility + kDynamicSet + 0x0C);
			const auto free = *reinterpret_cast<const std::uint32_t*>(visibility + kDynamicSet + 0x10);
			return capacity >= free ? capacity - free : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	namespace
	{
		using QueryFunc = void (*)();
		QueryFunc g_queryOriginal{ nullptr };
		void (*g_queryAfter)() { nullptr };

		std::chrono::steady_clock::duration g_queryTime{};  // main thread only
		std::uint32_t                       g_queryCalls{ 0 };

		void PrevisQueryThunk()
		{
			const auto start = std::chrono::steady_clock::now();
			g_queryOriginal();
			g_queryTime += std::chrono::steady_clock::now() - start;
			++g_queryCalls;
			g_queryAfter();
		}

		using PurgeFunc = void (*)(void*);
		PurgeFunc g_purgeOriginal{ nullptr };
		void (*g_resetAfter)() { nullptr };

		void GameResetPurgeThunk(void* a_tes)
		{
			g_purgeOriginal(a_tes);
			g_resetAfter();
		}

		// Every engine call to TES::HideLoadingMenu (E8 rel32; 7.15).
		struct HideSite
		{
			std::uint64_t id;
			std::size_t   offset;
			const char*   name;
		};
		constexpr HideSite kHideSites[] = {
			{ 1014572, 0x25A, "BGSSaveLoadManager::EndLoadGame" },
			{ 1096651, 0x496, "TES::LoadInterior" },
			{ 1386854, 0x4BA, "TES::LeaveInterior" },
			{ 146861, 0x104A, "id 146861" },
			{ 633524, 0x195, "Main::OnIdle" },
			{ 826184, 0x160, "Main::StartOfFrameUpdate" },
			{ 124452, 0x678, "Main::PerformGameReset" },
			{ 665510, 0xC5B, "Main::StartUp" },
			{ 933696, 0x6D, "id 933696" },
		};
		std::array<std::uintptr_t, std::size(kHideSites)> g_hideSites{};

		using HideFunc = void (*)(void*, void*, bool, float, bool);
		HideFunc g_hideOriginal{ nullptr };
		void (*g_hideBefore)(const char*) { nullptr };

		// TES::HideLoadingMenu(TES*, BGSLocation*, bool, float, bool): rcx is the TES singleton, the fifth argument is
		// on the stack (EndLoadGame+0x25A). The caller is told by the return address (the trampoline jumps).
		void HideLoadingMenuThunk(void* a_tes, void* a_location, bool a_flag, float a_fadeTime, bool a_flag2)
		{
			const auto  site = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - 5;
			const char* caller = "an unknown caller";
			for (std::size_t i = 0; i < std::size(kHideSites); ++i) {
				if (g_hideSites[i] == site) {
					caller = kHideSites[i].name;
				}
			}
			g_hideBefore(caller);
			g_hideOriginal(a_tes, a_location, a_flag, a_fadeTime, a_flag2);
		}

		// One trampoline for every call-site hook (allocating again would replace it).
		F4SE::Trampoline& Trampoline()
		{
			static const bool allocated = (F4SE::AllocTrampoline(256), true);
			(void)allocated;
			return F4SE::GetTrampoline();
		}

		// The engine's `call a_target` at a_site (E8 rel32), or 0 when another plugin took it.
		[[nodiscard]] std::uintptr_t CallTarget(std::uintptr_t a_site) noexcept
		{
			const auto site = reinterpret_cast<const std::uint8_t*>(a_site);
			return site[0] == 0xE8 ? a_site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1) : 0;
		}
	}

	bool InstallPrevisQueryHook(void (*a_after)())
	{
		if (!g.previsFeed || !a_after) {
			return false;
		}
		// The site must still be the engine's own `call 1264353` (another plugin may have taken it).
		const auto target = CallTarget(g.previsQuerySite);
		if (target != g.previsQuery) {
			logger::error("previs: Render_PreUI+0x{:X} is not the engine's call to the previs query (byte {:02X}, target {:X}); "
						  "the original meshes are shown while previs is active",
				kPrevisQuerySite, *reinterpret_cast<const std::uint8_t*>(g.previsQuerySite), target);
			return false;
		}
		g_queryAfter = a_after;
		g_queryOriginal = reinterpret_cast<QueryFunc>(Trampoline().write_call<5>(g.previsQuerySite, &PrevisQueryThunk));
		return true;
	}

	double TakePrevisQueryTime(std::uint32_t& a_calls) noexcept
	{
		a_calls = std::exchange(g_queryCalls, 0);
		return std::chrono::duration<double, std::milli>(std::exchange(g_queryTime, {})).count();
	}

	bool InstallGameResetHook(void (*a_after)())
	{
		bool       ok = true;
		const auto reset = Resolve(kGameResetID, "Main::PerformGameReset", ok);
		const auto purge = Resolve(kPurgeCellsID, "TES::PurgeBufferedCells", ok);
		if (!ok || !a_after) {
			return false;
		}
		const auto site = reset + kGameResetPurgeSite;
		if (CallTarget(site) != purge) {
			logger::error("Main::PerformGameReset+0x{:X} is not the engine's call to TES::PurgeBufferedCells (byte {:02X})",
				kGameResetPurgeSite, *reinterpret_cast<const std::uint8_t*>(site));
			return false;
		}
		g_resetAfter = a_after;
		g_purgeOriginal = reinterpret_cast<PurgeFunc>(Trampoline().write_call<5>(site, &GameResetPurgeThunk));
		return true;
	}

	std::uint32_t InstallLoadingMenuHook(void (*a_before)(const char*))
	{
		bool       ok = true;
		const auto hide = Resolve(kHideLoadingMenuID, "TES::HideLoadingMenu", ok);
		if (!ok || !a_before) {
			return 0;
		}
		g_hideBefore = a_before;
		std::uint32_t hooked = 0;
		for (std::size_t i = 0; i < std::size(kHideSites); ++i) {
			const auto& entry = kHideSites[i];
			bool        found = true;
			const auto  function = Resolve(entry.id, entry.name, found);
			if (!found) {
				continue;
			}
			const auto site = function + entry.offset;
			if (CallTarget(site) != hide) {
				logger::warn("{}+0x{:X} is not the engine's call to TES::HideLoadingMenu (byte {:02X}): that path closes the loading menu "
							 "before combining", entry.name, entry.offset, *reinterpret_cast<const std::uint8_t*>(site));
				continue;
			}
			g_hideOriginal = reinterpret_cast<HideFunc>(Trampoline().write_call<5>(site, &HideLoadingMenuThunk));
			g_hideSites[i] = site;
			++hooked;
		}
		return hooked;
	}
}
