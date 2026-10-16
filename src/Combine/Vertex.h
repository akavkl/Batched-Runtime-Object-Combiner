#pragma once

// Engine-free: the packed vertex format and the bake's transform. Built into the offline test as well.

#include <cstddef>
#include <cstdint>

namespace RC::Vertex
{
	// BSGraphics::VertexDesc: stride / 4 in bits 0-3, each attribute's offset / 4 in the nibble at bits 4a+4,
	// attribute flags from bit 44. BSGraphics::Utility::UnpackVertexData (0x141D2A450) decodes it this way:
	// position = 4 halves (4 floats with full precision), w = bitangent x; normal = 4 bytes, w = bitangent y;
	// tangent = 4 bytes, w = bitangent z; a byte b means b / 255 * 2 - 1.
	namespace Flag
	{
		inline constexpr std::uint64_t kVertex = 1ull << 0;
		inline constexpr std::uint64_t kUV = 1ull << 1;
		inline constexpr std::uint64_t kUV2 = 1ull << 2;
		inline constexpr std::uint64_t kNormal = 1ull << 3;
		inline constexpr std::uint64_t kTangent = 1ull << 4;
		inline constexpr std::uint64_t kColors = 1ull << 5;
		inline constexpr std::uint64_t kSkinned = 1ull << 6;
		inline constexpr std::uint64_t kLandData = 1ull << 7;
		inline constexpr std::uint64_t kEyeData = 1ull << 8;
		inline constexpr std::uint64_t kInstance = 1ull << 9;
		inline constexpr std::uint64_t kFullPrecision = 1ull << 10;
	}

	struct Layout
	{
		std::uint32_t stride{ 0 };
		std::uint32_t position{ 0 };
		std::uint32_t normal{ 0 };   // 0 = none
		std::uint32_t tangent{ 0 };  // 0 = none
		bool          fullPrecision{ false };

		[[nodiscard]] std::uint32_t PositionBytes() const noexcept { return fullPrecision ? 16 : 8; }
	};

	// False for formats the bake does not handle: no position, a position not at offset 0 (the input layout always
	// reads it there, FO4-ENGINE-NOTES 7.7), skinned, landscape, eye or instance data, or attribute offsets that
	// don't fit the stride.
	[[nodiscard]] bool Decode(std::uint64_t a_desc, Layout& a_out) noexcept;

	// The chunks' format: a_desc with positions as 4 floats (VF_FULLPREC, FO4-ENGINE-NOTES 7.7), so the stride and
	// every attribute offset after the position grow by 8. A source mesh's halves are relative to its own model
	// space; a chunk's are relative to its origin, often 1,000+ units away, where a half's step is 1-2 units.
	// Written back as halves, nearly coplanar surfaces of different meshes (a window and the wall behind it) swap
	// depth order. False when the wider stride doesn't fit the desc.
	[[nodiscard]] bool FullPrecision(std::uint64_t a_desc, std::uint64_t& a_out) noexcept;

	// True when Transform can write a_from's vertices as a_to's: the same attributes, only the position's
	// precision may differ.
	[[nodiscard]] bool Convertible(const Layout& a_from, const Layout& a_to) noexcept;

	// x' = linear * x + translate for positions; normals, tangents and bitangents use `rotation` (orthonormal,
	// so lengths are kept and an identity transform writes the input bytes back unchanged).
	struct Affine
	{
		float linear[3][3]{};  // x'_i = sum_j linear[i][j] * x_j
		float translate[3]{};
		float rotation[3][3]{};
	};

	// x' = a_outer(a_inner(x)).
	[[nodiscard]] Affine Compose(const Affine& a_outer, const Affine& a_inner) noexcept;
	// The inverse of a world transform: a rotation times a_scale (uniform, not 0) plus a translation.
	[[nodiscard]] Affine Inverse(const Affine& a_affine, float a_scale) noexcept;
	// A point through the transform.
	void Apply(const Affine& a_affine, const float a_in[3], float a_out[3]) noexcept;

	// Writes a_count vertices from a_src (a_from) into a_dst (a_to; Convertible). Attributes other than position,
	// normal and tangent are copied unchanged. a_min / a_max grow to include every written position.
	void Transform(const Layout& a_from, const std::byte* a_src, const Layout& a_to, std::byte* a_dst, std::uint32_t a_count, const Affine& a_affine, float a_min[3], float a_max[3]) noexcept;

	// One vertex's position (x, y, z, w = bitangent x).
	void ReadPosition(const std::byte* a_vertex, const Layout& a_layout, float a_out[4]) noexcept;

	// The packed byte <-> float mapping of normals and tangents.
	[[nodiscard]] float         ByteToUnit(std::uint8_t a_byte) noexcept;
	[[nodiscard]] std::uint8_t  UnitToByte(float a_value) noexcept;
}
