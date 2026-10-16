#pragma once

#include "Combine/Bake.h"

namespace RC::Combine
{
	// Why a reference or a mesh was left as it is.
	enum class Skip : std::uint32_t
	{
		// references
		kRefType,        // base object is not STAT / SCOL
		kRefCreated,     // made at run time (workshop)
		kRefDisabled,    // disabled or deleted
		kRefNo3D,        // no 3D loaded
		kRefHidden,      // 3D hidden by the game
		kRefFaded,       // a fade target below 1 (fadeAmount: an actor or script fade)
		kRefParent,      // no parent node, or a parent with a rotated or scaled world transform
		kRefParentType,  // parent is not a plain NiNode (a cell root or a room: their children have roles)
		// parts of a reference's 3D
		kBlocked,        // a part that renders on its own (other geometry, lights, special nodes, animation)
		kSkinned,
		kCollision,      // a mesh with its own collision object
		kNoData,         // no renderer data
		kFormat,         // vertex format the bake doesn't handle
		kStride,         // a stride too wide to grow by 8 for full-precision positions
		kNoCpuCopy,      // no CPU copy of the vertices / indices, or sizes that don't add up
		kShader,         // not BSLightingShaderProperty (a BSEffectShaderProperty: merged only with copies of its mesh)
		kShaderFlags,    // a shader that moves vertices, reads model space or renders outside the opaque passes
		kShaderAlpha,    // translucent: material alpha or LOD fade below 1 (not the load fade-in, which chunks redo)
		kAlphaBlend,     // alpha blending (draw order matters: merged only with copies of its mesh)
		kTooBig,         // bound radius above fMaxShapeRadius
		kTransform,      // world bound doesn't match the world transform (rotation convention check)
		kNonCaster,      // flag bit 40 (casts no shadow): Addictol's previs feed drops such records
		kCount
	};

	[[nodiscard]] constexpr std::string_view SkipName(Skip a_skip) noexcept
	{
		constexpr std::array<std::string_view, static_cast<std::size_t>(Skip::kCount)> names{
			"type"sv, "created"sv, "disabled"sv, "no3D"sv, "hidden"sv, "faded"sv, "parent"sv, "parentType"sv,
			"blocked"sv, "skinned"sv, "collision"sv, "noData"sv, "format"sv, "stride"sv, "noCpuCopy"sv,
			"shader"sv, "shaderFlags"sv, "shaderAlpha"sv, "alphaBlend"sv, "tooBig"sv, "transform"sv, "nonCaster"sv
		};
		return names[static_cast<std::size_t>(a_skip)];
	}

	// Why a mergeable mesh was left a solo, from the nearest other mesh of its material under the same parent.
	enum class Alone : std::uint32_t
	{
		kUnique,      // no other mesh of its material
		kFar,         // the nearest is beyond the second pass's reach (2 x fChunkSize)
		kSameGroup,   // the nearest is in its own bucket, in a chunk whose centre is out of reach or that is full
		kCopies,      // copies-only (effect shader, blending) and no copy of it in reach
		kFade,        // the nearest is in a bucket of another distance-fade class
		kFormat,      // ... another vertex format
		kAlpha,       // ... other alpha flags or threshold
		kShadowBits,  // ... other NiAVObject shadow bits
		kProperty,    // ... a property that doesn't merge with it (flags, +0x70 values, CanMerge)
		kCount
	};

	[[nodiscard]] constexpr std::string_view AloneName(Alone a_alone) noexcept
	{
		constexpr std::array<std::string_view, static_cast<std::size_t>(Alone::kCount)> names{
			"unique material"sv, "same material farther"sv, "own group's chunks farther or full"sv, "no copy near"sv, "fade class"sv, "vertex format"sv, "alpha"sv,
			"shadow bits"sv, "property"sv
		};
		return names[static_cast<std::size_t>(a_alone)];
	}

	struct Stats
	{
		std::array<std::uint32_t, static_cast<std::size_t>(Skip::kCount)> skips{};
		std::array<std::uint32_t, static_cast<std::size_t>(Skip::kCount)> cloned{};  // not mergeable, drawn by a solo clone
		std::array<std::uint32_t, static_cast<std::size_t>(Skip::kCount)> copies{};  // merged only with copies of the same mesh
		std::uint32_t references{ 0 };       // in the cell's list
		std::uint32_t candidates{ 0 };       // references with at least one captured mesh
		std::uint32_t capturedShapes{ 0 };
		std::uint32_t chunks{ 0 };           // combined meshes built
		std::uint32_t bakedShapes{ 0 };      // meshes they replace
		std::uint32_t solos{ 0 };            // meshes alone in their chunk: cloned as they are (Bake::Chunk::solo)
		std::uint32_t copyChunks{ 0 };       // chunks of same-mesh copies (Bucket::copiesOnly), among chunks
		std::uint32_t copyShapes{ 0 };       // the copies they replace, among bakedShapes
		std::uint32_t joined{ 0 };           // stragglers that joined a nearby chunk (Bake::Cluster)
		std::array<std::uint32_t, static_cast<std::size_t>(Alone::kCount)> alone{};  // mergeable solos by reason
		std::uint32_t precombinedRefs{ 0 };  // captured references the cell's precombines list (one fade class)
		std::uint32_t posedRefs{ 0 };        // of them, roots their own model poses: baked at their placement (Gather)
		std::uint32_t triangles{ 0 };
		std::uint32_t lodChunks{ 0 };        // chunks that keep their members' mesh LOD (bChunkMeshLOD, Manager)
		std::uint32_t lodTriangles{ 0 };     // their triangles drawn at mesh-LOD level 1 (the farthest)
		std::uint32_t lodFullTriangles{ 0 }; // and at level 3 (all)
		std::uint32_t vertices{ 0 };
		std::uint64_t vertexBytes{ 0 };      // their vertex data
		std::uint32_t wholeRefs{ 0 };        // references hidden whole (the scene walk skips them)
		std::uint32_t solidRefs{ 0 };        // of them, roots that own collision: left unculled, meshes hidden
		std::uint32_t partialRefs{ 0 };      // references with only some meshes hidden
		double        gatherMs{ 0.0 };
		double        bakeMs{ 0.0 };
		double        applyMs{ 0.0 };
		// What the 'blocked' parts are: class name (static NiRTTI strings, or "animated") and count.
		std::vector<std::pair<const char*, std::uint32_t>> blockedClasses;

		void Count(Skip a_skip) noexcept { ++skips[static_cast<std::size_t>(a_skip)]; }
		void CountCloned(Skip a_skip) noexcept { ++cloned[static_cast<std::size_t>(a_skip)]; }
		void CountCopies(Skip a_skip) noexcept { ++copies[static_cast<std::size_t>(a_skip)]; }
		void CountBlocked(const char* a_class)
		{
			Count(Skip::kBlocked);
			const auto found = std::ranges::find(blockedClasses, a_class, &std::pair<const char*, std::uint32_t>::first);
			if (found != blockedClasses.end()) {
				++found->second;
			} else {
				blockedClasses.emplace_back(a_class, 1);
			}
		}
	};

	struct SourceRef
	{
		RE::NiPointer<RE::TESObjectREFR> ref;
		std::uint32_t                    formID{ 0 };
		RE::NiPointer<RE::NiAVObject>    root;
		RE::NiTransform                  rootWorld;
		RE::NiNode*                      parent{ nullptr };  // the root's parent at capture
		bool                             whole{ false };     // every visible part of its 3D was captured
		std::uint32_t                    captured{ 0 };   // meshes captured
		std::uint32_t                    baked{ 0 };      // ... written into an applied chunk
	};

	struct SourceShape
	{
		RE::NiPointer<RE::NiAVObject> shape;
		std::uint32_t                 ref{ 0 };  // index into Job::refs
		const void*                   rendererData{ nullptr };
		RE::NiTransform               world;  // at capture (the watchdog's reference)
		RE::NiTransform               draw;   // where its chunk draws it: world, or for a posed root its placement's (Gather)
		bool                          baked{ false };
	};

	struct Bucket
	{
		RE::NiNode*                parent{ nullptr };  // the references' parent: the chunks' container goes here
		std::uint64_t              desc{ 0 };       // the members' vertex format
		Vertex::Layout             layout;
		std::uint64_t              chunkDesc{ 0 };  // the chunks' (Vertex::FullPrecision)
		Vertex::Layout             chunkLayout;
		void*                      property{ nullptr };  // the shader property every member can merge with
		std::uint32_t              alphaKey{ 0 };        // NiAlphaProperty flags and threshold (0 = none)
		std::uint64_t              objectKey{ 0 };       // the meshes' NiAVObject shadow bits
		std::uint32_t              fadeKey{ 0 };         // the references' distance-fade class (Gather)
		float                      fadeNear{ 0.0F };     // the furthest fade range among the members' references
		float                      fadeFar{ 0.0F };
		std::uint8_t               fadeType{ 0 };        // their fade nodes' LOD-mult type
		bool                       cloneOnly{ false };   // members that can't merge: each is a solo chunk
		bool                       copiesOnly{ false };  // copies of one mesh (rendererData) that merge only with each other
		const void*                rendererData{ nullptr };
		std::vector<std::uint32_t> shapes;               // indices into Job::shapes, parallel to members
		std::vector<Bake::Member>  members;
		std::vector<Bake::Chunk>   chunks;               // the worker's output
	};

	// One cell's capture, bake and apply. Created and destroyed on the main thread; between submit and the
	// baked flag the worker owns buckets[].chunks, and nothing else is touched by it.
	struct Job
	{
		RE::TESObjectCELL*       cell{ nullptr };
		std::uint32_t            cellFormID{ 0 };
		std::uint64_t            generation{ 0 };
		float                    chunkSize{ 0.0F };  // fChunkSize at gather
		Bake::Sizing             sizing;             // the player's position at gather and fChunkGrowth
		std::vector<SourceRef>   refs;
		std::vector<SourceShape> shapes;
		std::vector<Bucket>      buckets;
		std::vector<void*>       triShapes;  // per chunk (bucket-major order), created on the main thread
		bool                     created{ false };
		Stats                    stats;
		std::atomic<bool>        baked{ false };
		std::chrono::steady_clock::time_point started;
	};
}
