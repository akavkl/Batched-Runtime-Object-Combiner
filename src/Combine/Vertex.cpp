#include "Combine/Vertex.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace RC::Vertex
{
	using DirectX::PackedVector::XMConvertFloatToHalf;
	using DirectX::PackedVector::XMConvertHalfToFloat;

	void ReadPosition(const std::byte* a_vertex, const Layout& a_layout, float a_out[4]) noexcept
	{
		const auto at = a_vertex + a_layout.position;
		if (a_layout.fullPrecision) {
			std::memcpy(a_out, at, sizeof(float) * 4);
		} else {
			std::uint16_t half[4];
			std::memcpy(half, at, sizeof(half));
			for (int i = 0; i < 4; ++i) {
				a_out[i] = XMConvertHalfToFloat(half[i]);
			}
		}
	}

	namespace
	{
		void WritePosition(std::byte* a_vertex, const Layout& a_layout, const float a_in[4]) noexcept
		{
			const auto at = a_vertex + a_layout.position;
			if (a_layout.fullPrecision) {
				std::memcpy(at, a_in, sizeof(float) * 4);
			} else {
				std::uint16_t half[4];
				for (int i = 0; i < 4; ++i) {
					half[i] = XMConvertFloatToHalf(a_in[i]);
				}
				std::memcpy(at, half, sizeof(half));
			}
		}

		void Rotate(const float a_m[3][3], const float a_in[3], float a_out[3]) noexcept
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i] = a_m[i][0] * a_in[0] + a_m[i][1] * a_in[1] + a_m[i][2] * a_in[2];
			}
		}

		// Rotates the xyz bytes at a_bytes in place (the w byte is the caller's).
		void RotateBytes(std::byte* a_bytes, const float a_rotation[3][3]) noexcept
		{
			std::uint8_t raw[3];
			std::memcpy(raw, a_bytes, 3);
			const float in[3]{ ByteToUnit(raw[0]), ByteToUnit(raw[1]), ByteToUnit(raw[2]) };
			float       out[3];
			Rotate(a_rotation, in, out);
			for (int i = 0; i < 3; ++i) {
				raw[i] = UnitToByte(out[i]);
			}
			std::memcpy(a_bytes, raw, 3);
		}
	}

	namespace
	{
		// ByteToUnit of every byte, computed once: Transform reads six a vertex.
		const std::array<float, 256> kUnits = [] {
			std::array<float, 256> units{};
			for (std::size_t i = 0; i < units.size(); ++i) {
				units[i] = static_cast<float>(i) / 255.0F * 2.0F - 1.0F;
			}
			return units;
		}();
	}

	float ByteToUnit(std::uint8_t a_byte) noexcept
	{
		return kUnits[a_byte];
	}

	std::uint8_t UnitToByte(float a_value) noexcept
	{
		// Rounded half away from zero, as std::lround did, without its CRT call (eight a vertex took most of the bake):
		// scaled lies in [0, 255], where + 0.5 is exact in double and the cast truncates to the floor. NaN gives 0, as
		// lround's out-of-range result clamped to 0 did.
		const float scaled = (std::clamp(a_value, -1.0F, 1.0F) + 1.0F) * 127.5F;
		return scaled >= 0.0F ? static_cast<std::uint8_t>(static_cast<double>(scaled) + 0.5) : std::uint8_t{ 0 };
	}

	bool Decode(std::uint64_t a_desc, Layout& a_out) noexcept
	{
		const auto flags = a_desc >> 44;
		if (!(flags & Flag::kVertex) || (flags & (Flag::kSkinned | Flag::kLandData | Flag::kEyeData | Flag::kInstance))) {
			return false;
		}
		Layout layout;
		layout.stride = static_cast<std::uint32_t>(a_desc & 0xF) * 4;
		layout.position = static_cast<std::uint32_t>((a_desc >> 2) & 0x3C);
		layout.fullPrecision = (flags & Flag::kFullPrecision) != 0;
		if (flags & Flag::kNormal) {
			layout.normal = static_cast<std::uint32_t>((a_desc >> 14) & 0x3C);
		}
		if (flags & Flag::kTangent) {
			layout.tangent = static_cast<std::uint32_t>((a_desc >> 18) & 0x3C);
		}
		const std::uint32_t positionBytes = layout.fullPrecision ? 16 : 8;
		if (layout.stride == 0 || layout.position != 0 || positionBytes > layout.stride ||
			(layout.normal && layout.normal + 4 > layout.stride) ||
			(layout.tangent && layout.tangent + 4 > layout.stride) ||
			((flags & Flag::kNormal) && layout.normal == 0) ||
			((flags & Flag::kTangent) && (layout.tangent == 0 || layout.normal == 0))) {
			return false;
		}
		a_out = layout;
		return true;
	}

	bool FullPrecision(std::uint64_t a_desc, std::uint64_t& a_out) noexcept
	{
		if ((a_desc >> 44) & Flag::kFullPrecision) {
			a_out = a_desc;
			return true;
		}
		// Nibbles in 4-byte units: the stride at bits 0-3, attribute a's offset at bits 4a+4.
		const auto stride = a_desc & 0xF;
		const auto position = (a_desc >> 4) & 0xF;
		if (stride + 2 > 0xF) {
			return false;
		}
		std::uint64_t desc = (a_desc & ~0xFull) | (stride + 2);
		for (std::uint32_t attribute = 1; attribute < 9; ++attribute) {  // VA_TEXCOORD0 .. VA_EYEDATA
			const auto shift = 4 * attribute + 4;
			const auto offset = (a_desc >> shift) & 0xF;
			if (offset > position) {
				if (offset + 2 > 0xF) {
					return false;
				}
				desc = (desc & ~(0xFull << shift)) | ((offset + 2) << shift);
			}
		}
		a_out = desc | (Flag::kFullPrecision << 44);
		return true;
	}

	bool Convertible(const Layout& a_from, const Layout& a_to) noexcept
	{
		// Offsets after the position move by the difference in position size (unsigned wrap-around is exact).
		const auto moved = [&](std::uint32_t a_offset) {
			return a_offset > a_from.position ? a_offset + a_to.PositionBytes() - a_from.PositionBytes() : a_offset;
		};
		return a_to.position == a_from.position && a_to.stride == moved(a_from.stride) &&
		       a_to.normal == (a_from.normal ? moved(a_from.normal) : 0) &&
		       a_to.tangent == (a_from.tangent ? moved(a_from.tangent) : 0);
	}

	Affine Compose(const Affine& a_outer, const Affine& a_inner) noexcept
	{
		Affine out;
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				out.linear[i][j] = a_outer.linear[i][0] * a_inner.linear[0][j] + a_outer.linear[i][1] * a_inner.linear[1][j] + a_outer.linear[i][2] * a_inner.linear[2][j];
				out.rotation[i][j] = a_outer.rotation[i][0] * a_inner.rotation[0][j] + a_outer.rotation[i][1] * a_inner.rotation[1][j] + a_outer.rotation[i][2] * a_inner.rotation[2][j];
			}
		}
		Apply(a_outer, a_inner.translate, out.translate);
		return out;
	}

	Affine Inverse(const Affine& a_affine, float a_scale) noexcept
	{
		// The rotation is orthonormal: its inverse is its transpose.
		Affine out;
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				out.rotation[i][j] = a_affine.rotation[j][i];
				out.linear[i][j] = a_affine.rotation[j][i] / a_scale;
			}
		}
		for (int i = 0; i < 3; ++i) {
			out.translate[i] = -(out.linear[i][0] * a_affine.translate[0] + out.linear[i][1] * a_affine.translate[1] + out.linear[i][2] * a_affine.translate[2]);
		}
		return out;
	}

	void Apply(const Affine& a_affine, const float a_in[3], float a_out[3]) noexcept
	{
		float out[3];
		for (int i = 0; i < 3; ++i) {
			out[i] = a_affine.linear[i][0] * a_in[0] + a_affine.linear[i][1] * a_in[1] + a_affine.linear[i][2] * a_in[2] + a_affine.translate[i];
		}
		std::memcpy(a_out, out, sizeof(out));
	}

	void Transform(const Layout& a_from, const std::byte* a_src, const Layout& a_to, std::byte* a_dst, std::uint32_t a_count, const Affine& a_affine, float a_min[3], float a_max[3]) noexcept
	{
		const auto tail = a_from.stride - a_from.position - a_from.PositionBytes();  // the attributes after the position
		for (std::uint32_t v = 0; v < a_count; ++v) {
			const auto source = a_src + static_cast<std::size_t>(v) * a_from.stride;
			const auto vertex = a_dst + static_cast<std::size_t>(v) * a_to.stride;
			// (nothing before the position: Decode takes it only at offset 0)
			std::memcpy(vertex + a_to.position + a_to.PositionBytes(), source + a_from.position + a_from.PositionBytes(), tail);

			float position[4];
			ReadPosition(source, a_from, position);
			float moved[4];
			for (int i = 0; i < 3; ++i) {
				moved[i] = a_affine.linear[i][0] * position[0] + a_affine.linear[i][1] * position[1] + a_affine.linear[i][2] * position[2] + a_affine.translate[i];
				a_min[i] = std::min(a_min[i], moved[i]);
				a_max[i] = std::max(a_max[i], moved[i]);
			}
			moved[3] = position[3];

			if (a_to.normal) {
				RotateBytes(vertex + a_to.normal, a_affine.rotation);
			}
			if (a_to.tangent) {
				RotateBytes(vertex + a_to.tangent, a_affine.rotation);
				// Bitangent = (position w, normal w, tangent w).
				std::uint8_t normalW, tangentW;
				std::memcpy(&normalW, vertex + a_to.normal + 3, 1);
				std::memcpy(&tangentW, vertex + a_to.tangent + 3, 1);
				const float in[3]{ position[3], ByteToUnit(normalW), ByteToUnit(tangentW) };
				float       out[3];
				Rotate(a_affine.rotation, in, out);
				moved[3] = out[0];
				normalW = UnitToByte(out[1]);
				tangentW = UnitToByte(out[2]);
				std::memcpy(vertex + a_to.normal + 3, &normalW, 1);
				std::memcpy(vertex + a_to.tangent + 3, &tangentW, 1);
			}
			WritePosition(vertex, a_to, moved);
		}
	}
}
