#pragma once

// Engine-free: groups source meshes into chunks and writes each chunk's combined vertex and index data.
// Built into the offline test as well.

#include "Combine/Vertex.h"

#include <array>
#include <span>
#include <vector>

namespace RC::Bake
{
	inline constexpr std::uint32_t kMaxVertices = 0xFFFF;  // 16-bit indices

	// One source mesh as the main thread captured it. The pointers are the engine's CPU copies; the main thread
	// holds a reference on the mesh until the bake is finished.
	struct Member
	{
		const std::byte*     vertices{ nullptr };
		std::uint32_t        vertexCount{ 0 };
		const std::uint16_t* indices{ nullptr };
		std::uint32_t        triangleCount{ 0 };
		// A BSMeshLODTriShape's segments: its first lodSizes[0] triangles draw at mesh-LOD level 1, the next
		// lodSizes[1] join at level 2, the rest at level 3 (FO4-ENGINE-NOTES 7.11). All zero: one segment, always drawn.
		std::uint32_t        lodSizes[3]{};
		Vertex::Affine       toWorld;     // model space -> world space
		float                center[3]{};  // world bound
		float                radius{ 0.0F };
	};

	struct Chunk
	{
		// Input: the members drawn by this chunk (indices into the bucket's member list) and the world point its
		// vertices are written relative to (the chunk's local translate).
		std::vector<std::uint32_t> members;
		float                      origin[3]{};

		// Output.
		std::vector<std::byte>     vertices;
		std::vector<std::uint16_t> indices;
		std::uint32_t              vertexCount{ 0 };
		std::uint32_t              triangleCount{ 0 };
		// The indices are in segment order: every member's level-1 triangles, then every member's level-2 additions,
		// then the rest. A BSMeshLODTriShape with these sizes draws each member at its own level's detail.
		std::uint32_t              lodSizes[3]{};
		float                      boundCenter[3]{};  // relative to origin
		float                      boundRadius{ 0.0F };
		std::vector<std::uint32_t> baked;  // members actually written (one whose data failed the checks is left out)
		// Self-check (run 40, objects piled up): members whose written vertices lie off their world bound (box centre
		// beyond 2 x radius + 64 from the bound centre, or a box half diagonal beyond that), and the first of them.
		std::uint32_t strays{ 0 };
		std::uint32_t firstStray{ 0 };     // index into the bucket's members
		float         strayOffset{ 0.0F };  // its vertex box centre's distance from its bound centre
		float         strayExtent{ 0.0F };  // its vertex box's half diagonal
		// One member drawn as it is (a clone of its mesh sharing its data): no vertex data written. It still
		// replaces its original, so the reference can be hidden whole.
		bool solo{ false };
	};

	// How the chunk size grows with distance from the player: x1 within `growth`, then in proportion to the distance
	// (about the same size on screen), at most kMaxGrowth. growth 0 = x1 everywhere.
	inline constexpr float kMaxGrowth = 8.0F;
	struct Sizing
	{
		float eye[3]{};
		float growth{ 0.0F };

		[[nodiscard]] float At(const float a_point[3]) const noexcept;
	};

	// Compact clusters: each chunk holds the members whose bound centres lie within a_chunkSize / 2 of its seed's,
	// at most kMaxVertices vertices. The members of clusters smaller than a_minMembers get a second pass within
	// 2 x a_chunkSize; one still alone joins the chunk whose members' centre is nearest, within 2 x a_chunkSize
	// (a_joined counts them), else gives a solo chunk (origin: its bound centre). Solos come after the others.
	// a_spread > 0 also keeps every cluster dense: its reach (a member's distance from the seed, or from the chunk's
	// centre when joining, plus its radius) stays within a_chunkSize / 4 or within a_spread x the radius of one sphere
	// holding the volume of all its members' bounds; a member that would spread it further is left for another.
	// a_sizing scales every distance above by its factor at the seed (or at the chunk's centre when joining).
	[[nodiscard]] std::vector<Chunk> Cluster(std::span<const Member> a_members, float a_chunkSize, std::uint32_t a_minMembers, float a_spread = 0.0F,
		std::uint32_t* a_joined = nullptr, const Sizing& a_sizing = {});

	// One solo chunk per member, whatever its data: members that can't merge, drawn by clones of their meshes.
	[[nodiscard]] std::vector<Chunk> Solos(std::span<const Member> a_members);

	// A member's three segments: its lodSizes when they add up to its triangles, else all in the first.
	[[nodiscard]] std::array<std::uint32_t, 3> Segments(const Member& a_member) noexcept;

	// Writes the chunk's data: the members' vertices (a_from) in the chunk's format (a_to, Vertex::Convertible).
	// False when fewer than a_minMembers members could be written. A solo chunk only gets baked = members.
	[[nodiscard]] bool Build(const Vertex::Layout& a_from, const Vertex::Layout& a_to, std::span<const Member> a_members, Chunk& a_chunk, std::uint32_t a_minMembers);
}
