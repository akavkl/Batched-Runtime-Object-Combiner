// Offline test of the engine-free bake: vertex transform, full-precision widening, clustering, chunk build.
// Build: tools\tests\build_test.bat (cl, from a VS x64 prompt).

#include "Combine/Bake.h"
#include "Combine/Vertex.h"

#include <DirectXPackedVector.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace RC;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;

namespace
{
	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		if (!a_ok) {
			std::printf("FAIL: %s\n", a_what);
			++g_failures;
		}
	}

	// desc for: position (half or full), uv, normal, tangent, colors.
	std::uint64_t MakeDesc(bool a_full)
	{
		const std::uint32_t pos = a_full ? 16 : 8;
		const std::uint32_t uv = pos, normal = uv + 4, tangent = normal + 4, color = tangent + 4, stride = color + 4;
		std::uint64_t desc = stride / 4;
		desc |= static_cast<std::uint64_t>(0) << 4;            // position at 0
		desc |= static_cast<std::uint64_t>(uv / 4) << 8;        // VA_TEXCOORD0
		desc |= static_cast<std::uint64_t>(normal / 4) << 16;   // VA_NORMAL
		desc |= static_cast<std::uint64_t>(tangent / 4) << 20;  // VA_BINORMAL (tangent)
		desc |= static_cast<std::uint64_t>(color / 4) << 24;    // VA_COLOR
		std::uint64_t flags = Vertex::Flag::kVertex | Vertex::Flag::kUV | Vertex::Flag::kNormal | Vertex::Flag::kTangent | Vertex::Flag::kColors;
		if (a_full) {
			flags |= Vertex::Flag::kFullPrecision;
		}
		return desc | (flags << 44);
	}

	void WritePos(std::byte* a_v, bool a_full, const float a_p[4])
	{
		if (a_full) {
			std::memcpy(a_v, a_p, 16);
		} else {
			std::uint16_t h[4];
			for (int i = 0; i < 4; ++i) {
				h[i] = XMConvertFloatToHalf(a_p[i]);
			}
			std::memcpy(a_v, h, 8);
		}
	}

	void ReadPos(const std::byte* a_v, bool a_full, float a_p[4])
	{
		if (a_full) {
			std::memcpy(a_p, a_v, 16);
		} else {
			std::uint16_t h[4];
			std::memcpy(h, a_v, 8);
			for (int i = 0; i < 4; ++i) {
				a_p[i] = XMConvertHalfToFloat(h[i]);
			}
		}
	}

	Vertex::Affine Identity()
	{
		Vertex::Affine a;
		for (int i = 0; i < 3; ++i) {
			a.linear[i][i] = 1.0F;
			a.rotation[i][i] = 1.0F;
		}
		return a;
	}

	void TestDecode()
	{
		for (const bool full : { false, true }) {
			Vertex::Layout layout;
			Check(Vertex::Decode(MakeDesc(full), layout), "decode accepts the test format");
			Check(layout.stride == (full ? 32u : 24u), "stride");
			Check(layout.position == 0 && layout.normal == (full ? 20u : 12u) && layout.tangent == (full ? 24u : 16u), "offsets");
			Check(layout.fullPrecision == full, "precision flag");
		}
		Vertex::Layout layout;
		Check(!Vertex::Decode(MakeDesc(false) | (Vertex::Flag::kSkinned << 44), layout), "skinned rejected");
		Check(!Vertex::Decode(MakeDesc(false) & ~(Vertex::Flag::kVertex << 44), layout), "no position rejected");
		Check(!Vertex::Decode(MakeDesc(false) | (2ull << 4), layout), "position not at offset 0 rejected");
	}

	void TestIdentityRoundTrip()
	{
		std::mt19937 rng{ 7 };
		for (const bool full : { false, true }) {
			Vertex::Layout layout;
			Check(Vertex::Decode(MakeDesc(full), layout), "decode");
			const std::uint32_t   count = 500;
			std::vector<std::byte> src(count * layout.stride), dst(src.size());
			for (auto& b : src) {
				b = static_cast<std::byte>(rng() & 0xFF);
			}
			// Valid finite positions (random bytes could be NaN halves).
			for (std::uint32_t v = 0; v < count; ++v) {
				const float p[4]{ float(rng() % 2000) - 1000.0F, float(rng() % 2000) - 1000.0F, float(rng() % 500), float(rng() % 200) / 100.0F - 1.0F };
				WritePos(src.data() + v * layout.stride, full, p);
			}
			float lo[3]{ 1e30F, 1e30F, 1e30F }, hi[3]{ -1e30F, -1e30F, -1e30F };
			Vertex::Transform(layout, src.data(), layout, dst.data(), count, Identity(), lo, hi);
			Check(src == dst, full ? "identity keeps every byte (full precision)" : "identity keeps every byte (half precision)");
		}
	}

	void TestRotation()
	{
		// 90 degrees about z: x -> y, y -> -x. linear = scale 2, translate (10, 20, 30).
		Vertex::Layout layout;
		Check(Vertex::Decode(MakeDesc(true), layout), "decode");
		Vertex::Affine a;
		const float    r[3][3]{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				a.rotation[i][j] = r[i][j];
				a.linear[i][j] = 2.0F * r[i][j];
			}
		}
		a.translate[0] = 10;
		a.translate[1] = 20;
		a.translate[2] = 30;

		std::vector<std::byte> src(layout.stride), dst(layout.stride);
		const float            p[4]{ 1, 0, 0, 1 };  // w = bitangent x = 1
		WritePos(src.data(), true, p);
		const std::uint8_t normal[4]{ 255, 128, 128, 128 };   // normal ~ (1, 0, 0), bitangent y ~ 0
		const std::uint8_t tangent[4]{ 128, 255, 128, 128 };  // tangent ~ (0, 1, 0), bitangent z ~ 0
		std::memcpy(src.data() + layout.normal, normal, 4);
		std::memcpy(src.data() + layout.tangent, tangent, 4);
		float lo[3]{ 1e30F, 1e30F, 1e30F }, hi[3]{ -1e30F, -1e30F, -1e30F };
		Vertex::Transform(layout, src.data(), layout, dst.data(), 1, a, lo, hi);

		float q[4];
		ReadPos(dst.data(), true, q);
		Check(std::abs(q[0] - 10) < 1e-4F && std::abs(q[1] - 22) < 1e-4F && std::abs(q[2] - 30) < 1e-4F, "position rotated, scaled, moved");
		Check(std::abs(q[3] - 0.0F) < 0.02F, "bitangent x rotated (1,0,0 -> 0,1,0)");
		std::uint8_t n[4], t[4];
		std::memcpy(n, dst.data() + layout.normal, 4);
		std::memcpy(t, dst.data() + layout.tangent, 4);
		Check(std::abs(Vertex::ByteToUnit(n[0])) < 0.01F && Vertex::ByteToUnit(n[1]) > 0.99F, "normal rotated to +y");
		Check(Vertex::ByteToUnit(t[0]) < -0.99F && std::abs(Vertex::ByteToUnit(t[1])) < 0.01F, "tangent rotated to -x");
		Check(Vertex::ByteToUnit(n[3]) > 0.99F, "bitangent y = 1 after rotation");
		Check(lo[0] == 10 && hi[1] == 22, "bounds grow");
	}

	void TestCompose()
	{
		// a: 90 degrees about z (x -> y), scale 2, translate (10, 20, 30).
		Vertex::Affine a;
		const float    r[3][3]{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				a.rotation[i][j] = r[i][j];
				a.linear[i][j] = 2.0F * r[i][j];
			}
		}
		a.translate[0] = 10;
		a.translate[1] = 20;
		a.translate[2] = 30;
		const auto near = [](const float a_p[3], float a_x, float a_y, float a_z) {
			return std::abs(a_p[0] - a_x) < 1e-4F && std::abs(a_p[1] - a_y) < 1e-4F && std::abs(a_p[2] - a_z) < 1e-4F;
		};
		const float x[3]{ 1, 0, 0 };
		float       p[3];
		Vertex::Apply(a, x, p);
		Check(near(p, 10, 22, 30), "apply: rotated, scaled, moved");
		Vertex::Apply(Vertex::Compose(a, a), x, p);
		Check(near(p, -34, 40, 90), "compose: a after a");
		const auto  inverse = Vertex::Inverse(a, 2.0F);
		const float q[3]{ 3, 4, 5 };
		Vertex::Apply(Vertex::Compose(inverse, a), q, p);
		Check(near(p, 3, 4, 5), "inverse after a is the identity");
		Vertex::Apply(Vertex::Compose(a, inverse), q, p);
		Check(near(p, 3, 4, 5), "a after inverse is the identity");
		const auto both = Vertex::Compose(a, inverse);
		Check(std::abs(both.rotation[0][0] - 1) < 1e-6F && std::abs(both.rotation[0][1]) < 1e-6F && std::abs(both.linear[1][1] - 1) < 1e-6F, "rotation and linear parts cancel");
	}

	void TestClusterAndBuild()
	{
		Vertex::Layout layout;
		Check(Vertex::Decode(MakeDesc(false), layout), "decode");

		// Three meshes in one grid cell, one far away, one huge (alone in its cell).
		std::vector<std::byte>     vertices(3 * layout.stride);
		const std::uint16_t        indices[3]{ 0, 1, 2 };
		const float                corners[3][4]{ { 0, 0, 0, 0 }, { 10, 0, 0, 0 }, { 0, 10, 0, 0 } };
		for (int v = 0; v < 3; ++v) {
			WritePos(vertices.data() + v * layout.stride, false, corners[v]);
		}
		std::vector<Bake::Member> members;
		for (const float x : { 100.0F, 300.0F, 500.0F, 50000.0F }) {
			Bake::Member m;
			m.vertices = vertices.data();
			m.vertexCount = 3;
			m.indices = indices;
			m.triangleCount = 1;
			m.toWorld = Identity();
			m.toWorld.translate[0] = x;
			m.center[0] = x + 3;
			m.center[1] = 3;
			m.radius = 8;
			members.push_back(m);
		}
		auto chunks = Bake::Cluster(members, 1024.0F, 2);
		Check(chunks.size() == 2 && chunks[0].members.size() == 3 && !chunks[0].solo, "three neighbours form one chunk");
		Check(chunks.size() == 2 && chunks[1].solo && chunks[1].members.size() == 1 && chunks[1].members[0] == 3 && chunks[1].origin[0] == 50003.0F,
			"the far one is a solo chunk at its bound centre, after the others");
		if (chunks.size() == 2) {
			Check(Bake::Build(layout, layout, members, chunks[1], 2) && chunks[1].baked.size() == 1 && chunks[1].vertexCount == 0, "a solo writes no data");
		}
		std::vector<Bake::Member> unmergeable = members;
		unmergeable[1].vertexCount = 0;  // no CPU copy: still drawn by a clone
		const auto solos = Bake::Solos(unmergeable);
		Check(solos.size() == 4 && std::ranges::all_of(solos, &Bake::Chunk::solo) && solos[1].members[0] == 1 && solos[3].origin[0] == 50003.0F,
			"Solos: one solo chunk per member, whatever its data, at its bound centre");
		if (chunks.size() == 2) {
			auto& chunk = chunks[0];
			Check(Bake::Build(layout, layout, members, chunk, 2), "build");
			Check(chunk.vertexCount == 9 && chunk.triangleCount == 3, "counts");
			Check(chunk.indices[3] == 3 && chunk.indices[8] == 8, "indices rebased");
			float p[4];
			ReadPos(chunk.vertices.data() + 3 * layout.stride, false, p);  // second member's first vertex: x = 300 - origin
			Check(std::abs(p[0] - (300.0F - chunk.origin[0])) < 0.5F, "positions relative to the chunk origin");
			Check(chunk.boundRadius > 200.0F && chunk.boundRadius < 300.0F, "bound covers the members");
			Check(chunk.lodSizes[0] == 3 && chunk.lodSizes[1] == 0 && chunk.lodSizes[2] == 0, "members without LOD: one segment");
		}

		// Mesh LOD: two members with segments {1, 1, 1} around one without (2 triangles). The chunk's indices are every
		// member's first segment, then every second, then every third.
		{
			const std::uint16_t       lodIndices[9]{ 0, 1, 2, 1, 2, 0, 2, 0, 1 };
			const std::uint16_t       plainIndices[6]{ 0, 1, 2, 0, 2, 1 };
			std::vector<Bake::Member> lod(3, members[0]);
			for (const std::size_t i : { 0u, 2u }) {
				lod[i].indices = lodIndices;
				lod[i].triangleCount = 3;
				lod[i].lodSizes[0] = lod[i].lodSizes[1] = lod[i].lodSizes[2] = 1;
			}
			lod[1].indices = plainIndices;
			lod[1].triangleCount = 2;
			Bake::Chunk chunk;
			chunk.members = { 0, 1, 2 };
			Check(Bake::Build(layout, layout, lod, chunk, 2), "build with mesh LOD");
			const std::vector<std::uint16_t> expected{ 0, 1, 2, 3, 4, 5, 3, 5, 4, 6, 7, 8, 1, 2, 0, 7, 8, 6, 2, 0, 1, 8, 6, 7 };
			Check(chunk.indices == expected, "indices in segment order");
			Check(chunk.lodSizes[0] == 4 && chunk.lodSizes[1] == 2 && chunk.lodSizes[2] == 2 && chunk.triangleCount == 8, "chunk segments");
		}

		// Four meshes on a diamond (radius 200): the box's corners are empty, so the bound is the farthest vertex's
		// distance (~210), not the box's half diagonal (~297).
		std::vector<Bake::Member> diamond;
		for (const auto [x, y] : { std::pair{ 200.0F, 0.0F }, std::pair{ 0.0F, 200.0F }, std::pair{ -200.0F, 0.0F }, std::pair{ 0.0F, -200.0F } }) {
			auto m = members[0];
			m.toWorld.translate[0] = x;
			m.toWorld.translate[1] = y;
			m.center[0] = x + 3;
			m.center[1] = y + 3;
			diamond.push_back(m);
		}
		auto round = Bake::Cluster(diamond, 1024.0F, 2);
		Check(round.size() == 1 && Bake::Build(layout, layout, diamond, round[0], 2) && round[0].boundRadius > 200.0F && round[0].boundRadius < 215.0F,
			"bound radius: the farthest vertex from the box centre");

		// Vertex limit: 30 meshes of 3000 vertices split into chunks of at most 65535.
		std::vector<std::byte>     big(3000 * layout.stride);
		std::vector<std::uint16_t> bigIndices(3000);
		for (std::uint16_t i = 0; i < 3000; ++i) {
			bigIndices[i] = i;
		}
		std::vector<Bake::Member> many;
		for (int i = 0; i < 30; ++i) {
			Bake::Member m;
			m.vertices = big.data();
			m.vertexCount = 3000;
			m.indices = bigIndices.data();
			m.triangleCount = 1000;
			m.toWorld = Identity();
			m.center[0] = float(i);
			m.radius = 1;
			many.push_back(m);
		}
		const auto split = Bake::Cluster(many, 1024.0F, 2);
		std::size_t total = 0;
		bool        within = true;
		for (const auto& c : split) {
			total += c.members.size();
			within = within && c.members.size() * 3000 <= Bake::kMaxVertices;
		}
		Check(split.size() == 2 && total == 30 && within, "vertex limit splits a grid cell");

		// Clusters are compact, not grid cells: two meshes 100 apart across a multiple of 1024 form a chunk; one 2900
		// away from both stays solo.
		std::vector<Bake::Member> scattered;
		for (const float x : { 1000.0F, 1100.0F, 4000.0F }) {
			Bake::Member m = members[0];
			m.center[0] = x;
			scattered.push_back(m);
		}
		const auto compact = Bake::Cluster(scattered, 1024.0F, 2);
		Check(compact.size() == 2 && !compact[0].solo && compact[0].members.size() == 2 && compact[1].solo && compact[1].members[0] == 2,
			"neighbours across a grid line form a chunk, a mesh beyond half the chunk size stays apart");

		// Stragglers get a second pass within twice the chunk size: two meshes 1500 apart form a chunk, one 3500 away
		// from the nearest stays solo.
		std::vector<Bake::Member> stragglers;
		for (const float x : { 0.0F, 1500.0F, 5000.0F }) {
			Bake::Member m = members[0];
			m.center[0] = x;
			stragglers.push_back(m);
		}
		const auto wide = Bake::Cluster(stragglers, 1024.0F, 2);
		Check(wide.size() == 2 && !wide[0].solo && wide[0].members == std::vector<std::uint32_t>{ 0, 1 } && wide[1].solo && wide[1].members[0] == 2,
			"stragglers within twice the chunk size form a chunk, one beyond stays solo");

		// A straggler whose neighbours were all taken joins the nearest chunk within twice the chunk size: 0 and 100
		// form a chunk in the compact pass, 1500 (1450 from its centre) is left alone by both passes and joins it.
		std::vector<Bake::Member> late;
		for (const float x : { 0.0F, 100.0F, 1500.0F }) {
			Bake::Member m = members[0];
			m.center[0] = x;
			late.push_back(m);
		}
		std::uint32_t joined = 0;
		const auto    joinedChunks = Bake::Cluster(late, 1024.0F, 2, 0.0F, &joined);
		Check(joinedChunks.size() == 1 && joinedChunks[0].members == std::vector<std::uint32_t>{ 0, 1, 2 } && joined == 1,
			"a straggler joins the nearest chunk within twice the chunk size");

		// Density (a_spread 3): two big meshes (radius 200) 400 apart merge (reach 600 <= 3 x 252); small ones (radius
		// 8) 400 apart stay apart (reach 408 > 256 floor, > 3 x 10); small ones 100 apart merge (reach 108 <= floor).
		const auto density = [&](float a_radius, float a_gap) {
			std::vector<Bake::Member> pair;
			for (const float x : { 0.0F, a_gap }) {
				Bake::Member m = members[0];
				m.center[0] = x;
				m.radius = a_radius;
				pair.push_back(m);
			}
			return Bake::Cluster(pair, 1024.0F, 2, 3.0F);
		};
		const auto large = density(200.0F, 400.0F);
		const auto small = density(8.0F, 400.0F);
		const auto close = density(8.0F, 100.0F);
		Check(large.size() == 1 && !large[0].solo, "density: big meshes 400 apart merge");
		Check(small.size() == 2 && small[0].solo && small[1].solo, "density: small meshes 400 apart stay solos");
		Check(close.size() == 1 && !close[0].solo, "density: small meshes within a quarter chunk merge");

		// Sizing: two meshes 3000 apart stay solos near the eye (beyond twice the chunk size) and merge 8192 away with
		// growth 2048 (x4: the second pass reaches 8192).
		const auto sized = [&](float a_eyeX) {
			std::vector<Bake::Member> pair;
			for (const float x : { 0.0F, 3000.0F }) {
				Bake::Member m = members[0];
				m.center[0] = x;
				pair.push_back(m);
			}
			Bake::Sizing sizing;
			sizing.eye[0] = a_eyeX;
			sizing.growth = 2048.0F;
			return Bake::Cluster(pair, 1024.0F, 2, 0.0F, nullptr, sizing);
		};
		const auto nearEye = sized(1500.0F);
		const auto farEye = sized(-8192.0F);
		Check(nearEye.size() == 2 && nearEye[0].solo && nearEye[1].solo, "sizing: near the eye, meshes 3000 apart stay solos");
		Check(farEye.size() == 1 && !farEye[0].solo, "sizing: far from the eye, the same meshes merge");
		Bake::Sizing off;
		const float  at[3]{ 100000.0F, 0.0F, 0.0F };
		Check(off.At(at) == 1.0F, "sizing: growth 0 keeps x1");

		// A member with an out-of-range index is left out.
		std::uint16_t bad[3]{ 0, 1, 7 };
		members[1].indices = bad;
		auto again = Bake::Cluster(members, 1024.0F, 2);
		Check(again.size() == 2 && Bake::Build(layout, layout, members, again[0], 2) && again[0].baked.size() == 2, "bad indices drop only that member");
	}

	void TestFullPrecision()
	{
		std::uint64_t wide = 0;
		Check(Vertex::FullPrecision(MakeDesc(false), wide) && wide == MakeDesc(true), "half desc widens to the full-precision desc");
		Check(Vertex::FullPrecision(MakeDesc(true), wide) && wide == MakeDesc(true), "full desc unchanged");
		Check(!Vertex::FullPrecision((MakeDesc(false) & ~0xFull) | 14, wide), "a stride above 52 bytes can't widen");
		Vertex::Layout half, full;
		Check(Vertex::Decode(MakeDesc(false), half) && Vertex::Decode(MakeDesc(true), full), "decode");
		Check(Vertex::Convertible(half, full) && Vertex::Convertible(half, half) && !Vertex::Convertible(half, Vertex::Layout{}), "convertible");

		// A window 0.25 units in front of a wall, 1,500 units from the chunk origin: there a half's step is 1, so
		// only full precision keeps the two apart.
		std::vector<std::byte> wall(3 * half.stride), window(3 * half.stride);
		const float            corners[3][4]{ { 0, 0, 0, 0.5F }, { 0, 10, 0, 0.5F }, { 0, 0, 10, 0.5F } };
		for (int v = 0; v < 3; ++v) {
			WritePos(wall.data() + v * half.stride, false, corners[v]);
			WritePos(window.data() + v * half.stride, false, corners[v]);
			std::memset(window.data() + v * half.stride + 8, 0x5A, 4);   // uv
			std::memset(window.data() + v * half.stride + 20, 0xC3, 4);  // colour
		}
		const std::uint16_t       indices[3]{ 0, 1, 2 };
		std::vector<Bake::Member> members(2);
		for (int i = 0; i < 2; ++i) {
			auto& m = members[i];
			m.vertices = i == 1 ? window.data() : wall.data();
			m.vertexCount = 3;
			m.indices = indices;
			m.triangleCount = 1;
			m.toWorld = Identity();
			m.toWorld.translate[0] = i == 1 ? 1500.25F : 1500.0F;
			m.radius = 8;
		}
		Bake::Chunk chunk;  // origin (0, 0, 0)
		chunk.members = { 0, 1 };
		Check(Bake::Build(half, full, members, chunk, 2) && chunk.vertexCount == 6, "build half into full");
		Check(chunk.vertices.size() == 6u * full.stride, "chunk uses the wider stride");
		float p[4];
		ReadPos(chunk.vertices.data() + 3 * full.stride, true, p);
		Check(p[0] == 1500.25F && p[3] == 0.5F, "window kept 0.25 in front of the wall, bitangent x kept");
		Check(std::memcmp(chunk.vertices.data() + 3 * full.stride + 16, window.data() + 8, 4) == 0 &&
				  std::memcmp(chunk.vertices.data() + 3 * full.stride + 28, window.data() + 20, 4) == 0,
			"attributes after the position copied to their new offsets");

		Bake::Chunk halves;
		halves.members = { 0, 1 };
		Check(Bake::Build(half, half, members, halves, 2), "build half into half");
		ReadPos(halves.vertices.data() + 3 * half.stride, false, p);
		Check(p[0] == 1500.0F, "control: written as halves, the window lands on the wall");
	}
}

int main()
{
	TestDecode();
	TestIdentityRoundTrip();
	TestRotation();
	TestCompose();
	TestClusterAndBuild();
	TestFullPrecision();
	std::printf(g_failures ? "%d FAILURES\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
