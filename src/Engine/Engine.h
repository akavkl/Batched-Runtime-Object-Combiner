#pragma once

// Engine access for Fallout 4 OG 1.10.163. Every id, offset and function here was read from the executable on
// 2026-10-02 (CBRO's FO4-ENGINE-NOTES.md 7.2, 7.4, 7.5 record the facts and their sources). Ids are OG-only:
// they resolve on 1.10.163 and fail safely (unknown id) elsewhere, so the plugin installs nothing there.

namespace RC::Engine
{
	[[nodiscard]] constexpr REL::ID OG(std::uint64_t a_og) noexcept
	{
		return REL::ID{ a_og, REL::ID::INVALID_ID };
	}

	namespace Offset
	{
		// NiObjectNET
		inline constexpr std::size_t kControllers = 0x18;     // NiTimeController* (NiPointer)
		// NiTimeController (FO4-ENGINE-NOTES 7.16)
		inline constexpr std::size_t kControllerFlags = 0x10;   // u16: 0x8 active, 0x20 driven by a controller manager
		inline constexpr std::size_t kControllerTarget = 0x38;  // NiObjectNET*
		inline constexpr std::size_t kControllerNext = 0x40;    // the next controller in the target's list
		// NiUpdateData
		inline constexpr std::size_t kUpdateDataFlags = 0x10;   // u32: 0x100000 a controller moved its target (7.16)
		// BSGeometry / BSTriShape
		inline constexpr std::size_t kModelBound = 0x120;     // NiBound (local)
		inline constexpr std::size_t kAlphaProperty = 0x130;  // NiAlphaProperty*
		inline constexpr std::size_t kShaderProperty = 0x138; // BSShaderProperty*
		inline constexpr std::size_t kSkinInstance = 0x140;
		inline constexpr std::size_t kRendererData = 0x148;   // BSGraphics::TriShape* (refcount at +0x18)
		inline constexpr std::size_t kVertexDesc = 0x150;
		inline constexpr std::size_t kGeometryType = 0x158;   // u8: 3 BSTriShape, 5 BSMeshLODTriShape (FO4-ENGINE-NOTES 7.11)
		inline constexpr std::size_t kTriangles = 0x160;      // u32 drawn triangles
		inline constexpr std::size_t kVertices = 0x164;       // u16
		inline constexpr std::size_t kLODSizes = 0x170;       // BSMeshLODTriShape: u32[3] triangle segments (7.11)
		// BSGraphics::TriShape
		inline constexpr std::size_t kVertexBuffer = 0x8;
		inline constexpr std::size_t kIndexBuffer = 0x10;
		// BSGraphics::Buffer
		inline constexpr std::size_t kBufferData = 0x8;      // the CPU copy the engine keeps
		inline constexpr std::size_t kBufferSize = 0x34;     // bytes
		inline constexpr std::size_t kBufferPending = 0x44;  // creation requests still queued on the resource thread
		// BSShaderProperty
		inline constexpr std::size_t kShaderAlpha = 0x28;     // float: draw-time cache, below 1 = blended path (7.8)
		inline constexpr std::size_t kShaderFlags = 0x30;     // u64
		inline constexpr std::size_t kRenderPasses = 0x38;    // BSRenderPass* list, built when the mesh is drawn (7.7)
		inline constexpr std::size_t kShaderFadeNode = 0x48;  // the BSFadeNode its fade alpha comes from (7.6)
		inline constexpr std::size_t kShaderMaterial = 0x58;  // BSShaderMaterial*
		inline constexpr std::size_t kShaderLODFade = 0x64;   // float, 1.0 except in object-LOD blocks (FO4-ENGINE-NOTES 7.8)
		// BSShaderMaterial
		inline constexpr std::size_t kMaterialAlpha = 0x80;   // float: the persistent alpha (+0x28 is a draw-time cache, 7.8)
		// BSFadeNode (FO4-ENGINE-NOTES 7.8, 7.9)
		inline constexpr std::size_t kFadeType = 0x11C;       // u8: LOD-mult type (fade table index)
		inline constexpr std::size_t kFadeNear = 0x198;       // float: plain distances, ComputeFadeAmount's units
		inline constexpr std::size_t kFadeFar = 0x19C;
		// NiAlphaProperty
		inline constexpr std::size_t kAlphaFlags = 0x28;      // u16, bit 0 = blending
		inline constexpr std::size_t kAlphaThreshold = 0x2A;  // u8
	}

	// Shader property flag bits (Fallout4ShaderPropertyFlags1/2 in nif.xml; the engine's own checks agree: the
	// precombine baker sets 37 when it inserts vertex colours, the combined-shape technique reads 57).
	namespace ShaderFlag
	{
		inline constexpr std::uint64_t Bit(unsigned a_bit) noexcept { return 1ull << a_bit; }
		// Shaders that move vertices, read model space, or render outside the opaque passes: never combined.
		inline constexpr std::uint64_t kNotCombinable =
			Bit(1) |                                   // skinned
			Bit(2) | Bit(15) | Bit(16) |               // refraction
			Bit(10) | Bit(18) | Bit(21) |              // face, hair, skin tint
			Bit(12) |                                  // model-space normals
			Bit(14) | Bit(33) | Bit(34) |              // landscape, LOD landscape, LOD objects
			Bit(25) |                                  // tessellation
			Bit(40) | Bit(47) |                        // dismemberment
			Bit(42) | Bit(43) | Bit(44) | Bit(45) |    // grass
			Bit(55) | Bit(60) |                        // menu screen, pipboy screen
			Bit(61);                                   // tree animation
	}

	// NiAVObject flag bits.
	namespace ObjectFlag
	{
		inline constexpr std::uint64_t kAppCulled = 1ull << 0;
		inline constexpr std::uint64_t kAlwaysDraw = 1ull << 11;   // Block::Add forces the entry visible (FO4-ENGINE-NOTES 5.3)
		inline constexpr std::uint64_t kNotVisible = 1ull << 39;   // previs / fade culler
		inline constexpr std::uint64_t kNoShadow = 1ull << 40;     // clear = shadow caster
		inline constexpr std::uint64_t kAccumulated = 1ull << 42;  // a cull found it visible (SeenByMainView)
		inline constexpr std::uint64_t kMeshLOD = 1ull << 12;       // gates BSFadeNode::DetermineMeshLODLevel
		inline constexpr std::uint64_t kFadeComplete = 1ull << 37;  // a fade node's fade-in is done (FO4-ENGINE-NOTES 7.8)
		inline constexpr std::uint64_t kPickChildren = 1ull << 44;  // picking skips the node's bound, tests its children
	}

	template <class T = std::byte>
	[[nodiscard]] inline T* Field(const void* a_base, std::size_t a_offset) noexcept
	{
		return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_offset);
	}

	// Resolves every address once (Plugin load). False when one is missing: install nothing.
	[[nodiscard]] bool Init();

	// Exact-class tests (never subclasses).
	[[nodiscard]] bool IsExactTriShape(const RE::NiAVObject* a_object) noexcept;
	[[nodiscard]] bool IsExactMeshLODTriShape(const RE::NiAVObject* a_object) noexcept;
	[[nodiscard]] bool IsExactNode(const RE::NiAVObject* a_object) noexcept;      // NiNode or BSFadeNode
	[[nodiscard]] bool IsPlainNode(const RE::NiAVObject* a_object) noexcept;      // exactly NiNode
	[[nodiscard]] bool IsLightingShader(const void* a_property) noexcept;         // BSLightingShaderProperty
	[[nodiscard]] bool IsEffectShader(const void* a_property) noexcept;           // BSEffectShaderProperty

	// BSLightingShaderProperty::CanMerge (vtable slot 0x31): same flags, same material name, BSShaderMaterial::IsCopy,
	// and the two lighting-specific values (+0xB8 colour, +0xC8 float).
	[[nodiscard]] bool CanMerge(void* a_property, void* a_other);

	// NiObject::Clone(): a deep copy (properties included); the renderer data is shared and its count raised.
	// With a_plain, a BSMeshLODTriShape's clone comes back a plain BSTriShape that draws all its triangles
	// (FO4-ENGINE-NOTES 7.11): what a combined chunk built from it needs. Without, it stays a BSMeshLODTriShape that
	// draws the LOD prefix its fade node's level picks.
	[[nodiscard]] RE::NiAVObject* Clone(RE::NiAVObject* a_object, bool a_plain = true);

	// BSGraphics::Renderer::CreateTriShape: copies the data into new engine buffers (CPU copy kept) and queues
	// their GPU creation on the resource thread. Returns a TriShape with one reference, owned by the caller.
	[[nodiscard]] void* CreateTriShape(const void* a_vertices, std::uint32_t a_vertexBytes, std::uint64_t a_desc, const std::uint16_t* a_indices, std::uint32_t a_indexCount);
	void                ReleaseTriShape(void* a_triShape);
	[[nodiscard]] bool  TriShapeReady(const void* a_triShape) noexcept;

	// Replaces a shape's renderer data (releasing the old one) and its counts.
	void SetGeometry(RE::NiAVObject* a_shape, void* a_triShape, std::uint32_t a_vertices, std::uint32_t a_triangles);

	// NiAVObject::Update(NiUpdateData&) for objects placed once that never move: world transforms and bounds down the
	// subtree, then up the parents (what TESObjectCELL::AttachCombinedObjectArt runs after attaching a precombined
	// chunk); then previousWorld = world on every object of the subtree. UpdateWorldData copies the old world into
	// previousWorld before it computes the new one, and the deferred pre-pass draws TAA's motion vectors from the two
	// (FO4-ENGINE-NOTES 7.10): after one Update a new object would move from wherever it was before placement.
	void UpdateStatic(RE::NiAVObject* a_object);

	// A plain NiNode made by the engine's constructor.
	[[nodiscard]] RE::NiNode* NewNode(std::uint16_t a_children);

	// A reference root's distance-fade setup: the range TESObjectCELL::AttachReference3D gave its BSFadeNode
	// (ConfigureFadeNodeRange from its radius, FO4-ENGINE-NOTES 7.9) and its LOD-mult type. Invalid when the root is
	// no fade node.
	struct FadeRange
	{
		float        nearDistance{ 0.0F };  // +0x198, +0x19C: ComputeFadeAmount's units (distance x lodAdjust / mult)
		float        farDistance{ 0.0F };
		std::uint8_t type{ 0 };             // +0x11C
		[[nodiscard]] bool Valid() const noexcept { return farDistance > 0.0F; }
	};
	[[nodiscard]] FadeRange ReadFadeRange(const RE::NiAVObject* a_root) noexcept;
	// Where such a node starts fading out, in game units from the camera, with the current fade multipliers and the
	// main camera's lodAdjust (ComputeFadeAmount, FO4-ENGINE-NOTES 7.9); infinity when it never fades out.
	[[nodiscard]] float FadeStartDistance(const FadeRange& a_range) noexcept;
	// The furthest a camera in the loaded grid's centre cell can be from loaded content (uGridsToLoad).
	[[nodiscard]] float LoadedReach() noexcept;

	// A BSFadeNode holding a_shape, set up the way LoadTask::ProcessTriShape sets up a precombined chunk's
	// (FO4-ENGINE-NOTES 7.9), flag bits 44 and 12, with a_range as its fade range and type (the members'); without a
	// valid one, the precombined chunk's range from uGridsToLoad. It starts at a_fade: 1 = faded in; below 1 its
	// fade-in carries on from there (stamped as seen this frame, so the engine's 20-frame unseen rule doesn't snap
	// it, 7.8). Its geometry list is built by the first Update after it is attached. Null on failure.
	[[nodiscard]] RE::BSFadeNode* NewChunkFadeNode(RE::NiAVObject* a_shape, float a_fade, const FadeRange& a_range);

	// How far a reference root's load fade-in has got: its BSFadeNode's currentFade, or 1 when it is complete or
	// the root is no fade node (FO4-ENGINE-NOTES 7.8).
	[[nodiscard]] float FadeIn(const RE::NiAVObject* a_root) noexcept;

	// A probe: a childless BSFadeNode with the always-draw bit. Hung next to the containers, the scene walk files it
	// whole into the same culling group, Block::Add forces it visible, and the main view's finish loop sets its flag
	// bit 42 in every frame it processes that group (FO4-ENGINE-NOTES 5.3, 7.9). While previs feeds the main view
	// that group is not processed and the bit stays clear. Faded in, with Init's endless fade range (it never
	// fades). Null on failure.
	[[nodiscard]] RE::BSFadeNode* NewProbe();
	// Whether the main view processed the probe's group since the last call; clears the bit for the next one.
	[[nodiscard]] bool TakeProbeSeen(RE::NiAVObject* a_probe) noexcept;

	// A model that poses its own root: every controller on a_root is a NiTransformController aimed at it (at least
	// one). Each update writes its interpolator's pose over the root's local transform, part by part, so the root no
	// longer stands at its reference's placement; the precombines bake such a reference at its placement
	// (FO4-ENGINE-NOTES 7.16).
	[[nodiscard]] bool PosedByOwnModel(const RE::NiAVObject* a_root) noexcept;
	// A reference's placement: the world transform AttachRef3DToParent3D gives its root (rotation
	// NiMatrix3::FromEulerAnglesXYZ of its angle, translation its location, scale TESObjectREFR::GetScale; 7.16).
	// Reads the reference only: any thread.
	[[nodiscard]] RE::NiTransform Placement(const RE::TESObjectREFR* a_ref);

	// Wraps NiTransformController::Update (vtable slot 0x2A), the one writer of a model's pose (FO4-ENGINE-NOTES
	// 7.16). For an active controller, not driven by a manager, aimed at a reference's 3D root that its model poses
	// (PosedByOwnModel) under an unrotated, unscaled parent, a_hold(reference) is asked first, on the calling thread:
	// the cells' animation jobs, not the game thread. When it says yes, every controller on the root is stopped
	// (NiTimeController::Stop clears the active flag; only effects and projectiles start controllers again) and the
	// root's local transform is set to the reference's placement, as the precombines draw it. False when the slot
	// isn't the engine's function (another plugin took it).
	[[nodiscard]] bool InstallPoseHook(bool (*a_hold)(const RE::TESObjectREFR* a_ref));
	// Roots held since the last call, and how many of them the pose had already moved (drawn posed until then).
	void TakePoseHolds(std::uint32_t& a_held, std::uint32_t& a_moved) noexcept;

	// The object's class name (NiRTTI), for the log.
	[[nodiscard]] const char* ClassName(const RE::NiAVObject* a_object);

	// Attached cells: the interior, or the exterior grid's attached cells (TES::gridCells; the same reading as
	// CBRO's scene survey).
	[[nodiscard]] std::vector<RE::TESObjectCELL*> LoadedCells();
	[[nodiscard]] bool                            InInterior() noexcept;  // TES's interior cell is set (portal culling)

	// The engine's precombine switch ([General] bUseCombinedObjects), read at every cell load.
	[[nodiscard]] bool PrecombinesEnabled() noexcept;
	// The cell's precombined references (extra 0xC5, ExtraCombinedRefs, built from its XCRI record whatever the
	// precombine switch says; FO4-ENGINE-NOTES 7.12), or null. Any thread (the extra list's read lock).
	[[nodiscard]] const void* CombinedRefs(const RE::TESObjectCELL* a_cell) noexcept;
	// Whether a_formID is one of them: a reference the precombines draw, with no fade of its own (7.9).
	[[nodiscard]] bool InCombinedRefs(const void* a_combinedRefs, std::uint32_t a_formID) noexcept;
	// Appends their form ids to a_out (none on a fault). The set itself is read without a lock, as the engine's
	// TESObjectCELL::IsCombinedObjectRef reads it after releasing the extra list's lock.
	void CombinedRefIDs(const void* a_combinedRefs, std::vector<std::uint32_t>& a_out);
	// The precombined chunks attached in a_cell (its loaded data's combined-object list, FO4-ENGINE-NOTES 7.12),
	// their world-bound radii appended to a_radii when given.
	[[nodiscard]] std::uint32_t PrecombinedChunks(const RE::TESObjectCELL* a_cell, std::vector<float>* a_radii = nullptr);
	void               SetPrecombinesEnabled(bool a_enabled) noexcept;

	// One group-0 entry of the scene walk (FO4-ENGINE-NOTES 5.3), for the census.
	struct WalkEntry
	{
		RE::NiAVObject* object{ nullptr };
		std::uint32_t   meshes{ 0 };           // BSGeometry under it that aren't AppCulled (what a fade node files)
		bool            precombined{ false };  // one of the cell's precombined chunks (7.12)
		bool            node9{ false };        // under cell child 9 (bit-30 precombined art)
	};
	// The walk's group-0 entries in a_cell's 3D: of the cell node's children 3 and 9, an exact NiNode child is a
	// container whose own children are entries, anything else is one. AppCulled ones the walk skips: counted in
	// a_hidden. Main thread.
	void WalkEntries(const RE::TESObjectCELL* a_cell, std::vector<WalkEntry>& a_out, std::uint32_t& a_hidden);

	// What the renderer made of one chunk at the last cull (read only, under SEH; FO4-ENGINE-NOTES 5.3, 7.6-7.9).
	struct ChunkView
	{
		bool  inView{ false };  // its fade node's OnVisible stamp is fresh (false without one: a mesh's bit 42 is no
		                        // signal at the game tick, FO4-ENGINE-NOTES 5.3)
		bool  moving{ false };  // previousWorld differs from world: TAA reads a motion (FO4-ENGINE-NOTES 7.10)
		bool  passes{ false };  // render passes exist: it has been drawn
		float alpha{ 1.0F };    // draw-time alpha (below 1: the blended path)
		float fade{ 1.0F };     // its fade node's currentFade (1 without one)
		bool  owner{ true };    // the mesh takes its fade alpha from its own fade node
		const char*   material{ nullptr };  // the shader property's name (the material path), for the log
		std::uint64_t shaderFlags{ 0 };
	};
	[[nodiscard]] bool ReadChunkView(const RE::NiAVObject* a_shape, const RE::BSFadeNode* a_fadeNode, ChunkView& a_out) noexcept;

	// Flag bit 42 of a previs dynamic object, read right after the previs query: whether the query's main-view pass
	// found it visible this frame (FO4-ENGINE-NOTES 5.5d). Later passes of the frame rewrite the bit (group 0's finish
	// loop, lamp and CBRO culls), so at the game tick it says nothing about the main view (5.2).
	[[nodiscard]] bool SeenByMainView(const RE::NiAVObject* a_object) noexcept;

	// Previs. PrevisEnabled is the wish (QWantEnabled); PrevisActive is what the cull uses (QEnabled: wished, INI,
	// not suspended). While it is active the main view draws previs's per-frame records instead of group 0
	// (FO4-ENGINE-NOTES 5.3, 5.5b).
	[[nodiscard]] bool PrevisEnabled();
	[[nodiscard]] bool PrevisActive();

	// ---- previs dynamic objects (FO4-ENGINE-NOTES 5.5d) -------------------------------------------------------------
	// MultiCellVisibilityData::RegisterDynamicObject / UnregisterDynamicObject, what a reference outside its cell's
	// previs list gets for its 3D (all of them with precombines off). While previs is active, every frame's query
	// tests each registered object (AppCulled ones are skipped) and adds its visible meshes to the main view, the
	// sun's cascades and the precipitation map; a fade node is faded as the main view would. Both calls only queue
	// the change for the engine's next MultiCellVisibilityData::Update. The engine keeps a raw pointer: the caller
	// keeps the object alive while it is registered (the queued removal holds it until the engine lets go).
	// False when previs's visibility object doesn't exist.
	[[nodiscard]] bool RegisterPrevisObject(RE::NiAVObject* a_object) noexcept;
	void               UnregisterPrevisObject(RE::NiAVObject* a_object) noexcept;
	// Unregisters a_object only when it is in previs's dynamic-object set now (what the engine registered: a
	// reference's 3D outside its cell's previs list). True when it was. Main thread (the set changes in
	// MultiCellVisibilityData::Update, at the start of the frame's render).
	[[nodiscard]] bool UnregisterIfDynamic(RE::NiAVObject* a_object) noexcept;
	// Objects in the dynamic-object set, which the previs query tests every frame in three views.
	[[nodiscard]] std::uint32_t PrevisDynamicObjects() noexcept;

	// Wraps the previs query call at Render_PreUI+0x80 (checked to be the engine's own call first): a_after runs
	// right after the query, on the main thread, before the cull. False when the site isn't what was expected, or an
	// address of the previs dynamic objects is missing.
	[[nodiscard]] bool InstallPrevisQueryHook(void (*a_after)());
	// Milliseconds the engine's previs query took since the last call, and how many times it ran (main thread).
	[[nodiscard]] double TakePrevisQueryTime(std::uint32_t& a_calls) noexcept;

	// GPU probe: D3D11 pipeline statistics over the whole frame in contiguous segments, switched at DrawWorld::
	// Render_PreUI's calls and issued on the renderer context the engine draws with (FO4-ENGINE-NOTES 3, 4.1). Read
	// only; results arrive a few frames late.
	enum GpuStage : std::size_t
	{
		kGpuOutside,       // forward's end to the next cull: post-processing, UI, present, other cameras' passes
		kGpuCull,          // +0x16A..+0x17A: the cull, rain occlusion, sun-cascade and g-buffer setup
		kGpuPrePass,       // +0x17F: the deferred pre-pass (z-prepass and g-buffer)
		kGpuAfterPrePass,  // +0x184..+0x1BA: clears, unbinds, HBAO
		kGpuLights,        // +0x1BF: sun cascades, lamps' shadow maps and the deferred lights
		kGpuComposite,     // +0x1C4: the deferred composite
		kGpuForward,       // +0x1C9: forward (alpha-blended) pass
		kGpuStageCount
	};
	struct GpuStageStats
	{
		std::uint64_t frames{ 0 };         // frames whose query was read
		std::uint64_t skipped{ 0 };        // frames not measured (all queries in flight, or another context)
		std::uint64_t iaPrimitives{ 0 };   // triangles submitted
		std::uint64_t cPrimitives{ 0 };    // triangles left after clipping and culling (rasterized)
		std::uint64_t vsInvocations{ 0 };  // vertices shaded
		std::uint64_t psInvocations{ 0 };  // pixels shaded
		std::uint64_t csInvocations{ 0 };  // compute-shader threads
		double        gpuMs{ 0.0 };        // GPU time between the segment's timestamps, idle included
		std::uint64_t timed{ 0 };          // frames whose timestamps were read
	};
	// GPU time of each image-space effect DrawWorld::DeferredComposite renders, from timestamps around its call.
	inline constexpr std::size_t kCompositeEffects = 5;
	struct CompositeEffectStats
	{
		char          name[96]{};  // the effect's class (RTTI), empty while it was never seen
		double        gpuMs{ 0.0 };
		std::uint64_t timed{ 0 };
	};
	[[nodiscard]] std::array<CompositeEffectStats, kCompositeEffects> TakeCompositeEffects() noexcept;
	// Wraps the boundary calls (any `call rel32` there, so other plugins' wrappers chain); returns how many (all or 0).
	[[nodiscard]] std::uint32_t InstallGpuStatsProbe();
	// Sums since the last call (any thread).
	[[nodiscard]] std::array<GpuStageStats, kGpuStageCount> TakeGpuStats() noexcept;
	[[nodiscard]] const char* GpuStageName(std::size_t a_stage) noexcept;


	// Wraps the cell-buffer purge in Main::PerformGameReset (a save load or new game): a_after runs once every cell
	// is cleared and purged, before the next world loads, the one point where the precombine switch may change
	// (FO4-ENGINE-NOTES 7.12). False when the site isn't the engine's own call.
	[[nodiscard]] bool InstallGameResetHook(void (*a_after)());

	// Wraps TES::HideLoadingMenu at the engine's call sites (save load, doors, fast travel, the frame's start;
	// FO4-ENGINE-NOTES 7.15): a_before runs on the calling thread while the loading menu still covers the screen,
	// before the menu is told to close and the fade-in from black starts. It gets the caller's name. Returns how
	// many sites were the engine's own call and are now wrapped.
	[[nodiscard]] std::uint32_t InstallLoadingMenuHook(void (*a_before)(const char* a_caller));
}
