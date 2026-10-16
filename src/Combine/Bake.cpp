#include "Combine/Bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>

#include <excpt.h>

namespace RC::Bake
{
	namespace
	{
		// Engine memory is read under SEH: a mesh freed or changed behind our back fails the copy, not the game.
		bool SafeCopy(void* a_dst, const void* a_src, std::size_t a_bytes) noexcept
		{
			__try {
				std::memcpy(a_dst, a_src, a_bytes);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		struct Key
		{
			std::int32_t x, y, z;
			auto         operator<=>(const Key&) const = default;
		};

		Chunk SoloOf(std::span<const Member> a_members, std::uint32_t a_index)
		{
			Chunk solo;
			solo.members.push_back(a_index);
			solo.solo = true;
			std::copy_n(a_members[a_index].center, 3, solo.origin);
			return solo;
		}

		// How far a cluster may reach for what it holds (Cluster's a_spread). Scattered small meshes merged into one wide
		// sphere are judged by that sphere: the sun's cascades file it and CBRO keeps its shadow wherever the sphere's
		// would land, though the meshes in it are small. RC's chunks of radius 256-512 took ~2.6x the precombines'
		// registrations of that size in the far cascade (run 34's probe), with fewer main-view draws.
		struct Density
		{
			float floor{ 0.0F };
			float factor{ 0.0F };

			[[nodiscard]] bool Fits(float a_reach, double a_volume) const noexcept
			{
				return factor <= 0.0F || a_reach <= std::max(floor, factor * static_cast<float>(std::cbrt(a_volume)));
			}
		};

		[[nodiscard]] double Cube(float a_radius) noexcept
		{
			return static_cast<double>(a_radius) * a_radius * a_radius;
		}

		// One greedy pass: a seed takes the free candidates whose bound centres lie within a_radius of its own, nearest
		// first, at most kMaxVertices vertices, while the cluster stays dense (a_density). Clusters of at least
		// a_minMembers go to a_chunks; the members of smaller ones are returned. The candidates are bucketed in cells of
		// the radius, so the 3x3x3 cells around a seed's hold every one in reach.
		std::vector<std::uint32_t> ClusterPass(std::span<const Member> a_members, std::vector<std::uint32_t> a_candidates, float a_radius,
			std::uint32_t a_minMembers, const Density& a_density, const Sizing& a_sizing, std::vector<Chunk>& a_chunks)
		{
			// Buckets as wide as the largest radius any seed gets, so the 3x3x3 around a seed's still hold its reach.
			float grown = 1.0F;
			for (const auto index : a_candidates) {
				grown = std::max(grown, a_sizing.At(a_members[index].center));
			}
			const float cell = a_radius * grown;
			const auto  cellOf = [&](const Member& a_m) {
				return Key{ static_cast<std::int32_t>(std::floor(a_m.center[0] / cell)), static_cast<std::int32_t>(std::floor(a_m.center[1] / cell)),
					static_cast<std::int32_t>(std::floor(a_m.center[2] / cell)) };
			};
			std::map<Key, std::vector<std::uint32_t>> cells;
			for (const auto index : a_candidates) {
				cells[cellOf(a_members[index])].push_back(index);
			}
			// Seeds in cell order, along x inside a cell: deterministic, and neighbours seed neighbours.
			std::ranges::sort(a_candidates, [&](std::uint32_t a_lhs, std::uint32_t a_rhs) {
				const auto lhs = cellOf(a_members[a_lhs]);
				const auto rhs = cellOf(a_members[a_rhs]);
				return lhs != rhs ? lhs < rhs : a_members[a_lhs].center[0] < a_members[a_rhs].center[0];
			});

			std::vector<std::uint32_t>                 alone;
			std::vector<bool>                          taken(a_members.size(), false);
			std::vector<std::pair<float, std::uint32_t>> inReach;
			for (const auto seed : a_candidates) {
				if (taken[seed]) {
					continue;
				}
				const auto& s = a_members[seed];
				const auto  home = cellOf(s);
				const float radius = a_radius * a_sizing.At(s.center);
				inReach.clear();
				for (std::int32_t dx = -1; dx <= 1; ++dx) {
					for (std::int32_t dy = -1; dy <= 1; ++dy) {
						for (std::int32_t dz = -1; dz <= 1; ++dz) {
							const auto found = cells.find(Key{ home.x + dx, home.y + dy, home.z + dz });
							if (found == cells.end()) {
								continue;
							}
							for (const auto index : found->second) {
								const auto& m = a_members[index];
								const float d[3]{ m.center[0] - s.center[0], m.center[1] - s.center[1], m.center[2] - s.center[2] };
								const float distance2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
								if (!taken[index] && index != seed && distance2 <= radius * radius) {
									inReach.emplace_back(distance2, index);
								}
							}
						}
					}
				}
				std::ranges::sort(inReach);
				Chunk         chunk;
				std::uint32_t vertices = s.vertexCount;
				float         reach = s.radius;
				double        volume = Cube(s.radius);
				taken[seed] = true;
				chunk.members.push_back(seed);
				for (const auto& [distance2, index] : inReach) {
					const auto& m = a_members[index];
					const float memberReach = std::max(reach, std::sqrt(distance2) + m.radius);
					if (vertices + m.vertexCount > kMaxVertices || !a_density.Fits(memberReach, volume + Cube(m.radius))) {
						continue;
					}
					taken[index] = true;
					chunk.members.push_back(index);
					vertices += m.vertexCount;
					reach = memberReach;
					volume += Cube(m.radius);
				}
				if (chunk.members.size() >= a_minMembers) {
					std::ranges::sort(chunk.members);
					a_chunks.push_back(std::move(chunk));
				} else {
					alone.insert(alone.end(), chunk.members.begin(), chunk.members.end());
				}
			}
			return alone;
		}
	}

	float Sizing::At(const float a_point[3]) const noexcept
	{
		if (growth <= 0.0F) {
			return 1.0F;
		}
		const float d[3]{ a_point[0] - eye[0], a_point[1] - eye[1], a_point[2] - eye[2] };
		return std::clamp(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / growth, 1.0F, kMaxGrowth);
	}

	std::vector<Chunk> Solos(std::span<const Member> a_members)
	{
		std::vector<Chunk> solos;
		for (std::uint32_t i = 0; i < a_members.size(); ++i) {
			solos.push_back(SoloOf(a_members, i));
		}
		return solos;
	}

	std::vector<Chunk> Cluster(std::span<const Member> a_members, float a_chunkSize, std::uint32_t a_minMembers, float a_spread, std::uint32_t* a_joined,
		const Sizing& a_sizing)
	{
		const Density density{ a_chunkSize * 0.25F, a_spread };
		std::vector<std::uint32_t> candidates;
		for (std::uint32_t i = 0; i < a_members.size(); ++i) {
			const auto& m = a_members[i];
			if (m.vertexCount != 0 && m.vertexCount <= kMaxVertices && m.triangleCount != 0) {
				candidates.push_back(i);
			}
		}
		// Compact clusters first, within half the chunk size: about the precombined chunks' typical size. The stragglers
		// get a second pass within twice the chunk size (about a cell; precombined chunks reach bound radius 5,200): left
		// as solo clones they were 10,461 of Runtime Combiner's 22,473 objects against 18,971 precombined chunks (run 20).
		std::vector<Chunk> chunks;
		auto               alone = ClusterPass(a_members, std::move(candidates), a_chunkSize * 0.5F, a_minMembers, density, a_sizing, chunks);
		alone = ClusterPass(a_members, std::move(alone), a_chunkSize * 2.0F, a_minMembers, density, a_sizing, chunks);

		// A member is left alone when every neighbour went to an earlier seed's chunk: it joins the chunk whose
		// members' centre is nearest, within the second pass's reach. Left as solos, such meshes made RC's scene ~15%
		// more objects near the camera than the precombines' at an equal count of chunks (run 27's census).
		struct Centre
		{
			float         at[3]{};
			std::uint32_t vertices{ 0 };
			float         reach{ 0.0F };  // the farthest member bound from the centre
			double        volume{ 0.0 };
		};
		const auto distance = [](const float a_a[3], const float a_b[3]) {
			const float d[3]{ a_a[0] - a_b[0], a_a[1] - a_b[1], a_a[2] - a_b[2] };
			return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		};
		std::vector<Centre> centres(chunks.size());
		for (std::size_t c = 0; c < chunks.size(); ++c) {
			for (const auto index : chunks[c].members) {
				for (int i = 0; i < 3; ++i) {
					centres[c].at[i] += a_members[index].center[i] / static_cast<float>(chunks[c].members.size());
				}
				centres[c].vertices += a_members[index].vertexCount;
				centres[c].volume += Cube(a_members[index].radius);
			}
			for (const auto index : chunks[c].members) {
				centres[c].reach = std::max(centres[c].reach, distance(a_members[index].center, centres[c].at) + a_members[index].radius);
			}
		}
		std::vector<Chunk> solos;
		for (const auto index : alone) {
			const auto& m = a_members[index];
			std::size_t best = chunks.size();
			float       bestDistance = std::numeric_limits<float>::max();
			for (std::size_t c = 0; c < chunks.size(); ++c) {
				const float d[3]{ m.center[0] - centres[c].at[0], m.center[1] - centres[c].at[1], m.center[2] - centres[c].at[2] };
				const float distance2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
				const float reach = a_chunkSize * 2.0F * a_sizing.At(centres[c].at);
				if (distance2 <= reach * reach && distance2 <= bestDistance && centres[c].vertices + m.vertexCount <= kMaxVertices &&
					density.Fits(std::max(centres[c].reach, distance(m.center, centres[c].at) + m.radius), centres[c].volume + Cube(m.radius))) {
					best = c;
					bestDistance = distance2;
				}
			}
			if (best == chunks.size()) {
				solos.push_back(SoloOf(a_members, index));
				continue;
			}
			chunks[best].members.insert(std::ranges::upper_bound(chunks[best].members, index), index);
			centres[best].vertices += m.vertexCount;
			centres[best].reach = std::max(centres[best].reach, distance(m.center, centres[best].at) + m.radius);
			centres[best].volume += Cube(m.radius);
			if (a_joined) {
				++*a_joined;
			}
		}

		// The origin is the centre of the members' bounding box: the smallest offsets, the finest position steps.
		for (auto& chunk : chunks) {
			float lo[3]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
			float hi[3]{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
			for (const auto index : chunk.members) {
				const auto& m = a_members[index];
				for (int i = 0; i < 3; ++i) {
					lo[i] = std::min(lo[i], m.center[i] - m.radius);
					hi[i] = std::max(hi[i], m.center[i] + m.radius);
				}
			}
			for (int i = 0; i < 3; ++i) {
				chunk.origin[i] = 0.5F * (lo[i] + hi[i]);
			}
		}
		std::ranges::move(solos, std::back_inserter(chunks));
		return chunks;
	}

	std::array<std::uint32_t, 3> Segments(const Member& a_member) noexcept
	{
		const auto& sizes = a_member.lodSizes;
		if (static_cast<std::uint64_t>(sizes[0]) + sizes[1] + sizes[2] == a_member.triangleCount) {
			return { sizes[0], sizes[1], sizes[2] };
		}
		return { a_member.triangleCount, 0, 0 };
	}

	bool Build(const Vertex::Layout& a_from, const Vertex::Layout& a_to, std::span<const Member> a_members, Chunk& a_chunk, std::uint32_t a_minMembers)
	{
		if (a_chunk.solo) {
			a_chunk.baked = a_chunk.members;
			return true;
		}
		if (!Vertex::Convertible(a_from, a_to)) {
			return false;
		}
		std::uint32_t vertexTotal = 0;
		std::uint32_t triangleTotal = 0;
		for (const auto index : a_chunk.members) {
			vertexTotal += a_members[index].vertexCount;
			triangleTotal += a_members[index].triangleCount;
		}
		if (vertexTotal > kMaxVertices) {
			return false;
		}

		a_chunk.vertices.assign(static_cast<std::size_t>(vertexTotal) * a_to.stride, std::byte{ 0 });
		a_chunk.indices.resize(static_cast<std::size_t>(triangleTotal) * 3);
		a_chunk.baked.clear();
		a_chunk.strays = 0;

		std::vector<std::byte>     source;
		std::vector<std::uint16_t> sourceIndices;
		float                      lo[3]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		float                      hi[3]{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
		std::uint32_t              vertexBase = 0;
		std::uint32_t              indexBase = 0;
		std::vector<std::uint32_t> starts;  // each baked member's first index, in member order
		for (const auto index : a_chunk.members) {
			const auto& m = a_members[index];
			const auto  vertexBytes = static_cast<std::size_t>(m.vertexCount) * a_from.stride;
			const auto  indexCount = static_cast<std::size_t>(m.triangleCount) * 3;
			source.resize(vertexBytes);
			sourceIndices.resize(indexCount);
			if (!SafeCopy(source.data(), m.vertices, vertexBytes) || !SafeCopy(sourceIndices.data(), m.indices, indexCount * sizeof(std::uint16_t))) {
				continue;
			}
			if (std::ranges::any_of(sourceIndices, [&](std::uint16_t a_index) { return a_index >= m.vertexCount; })) {
				continue;
			}

			// Model space -> chunk space: world, minus the chunk origin.
			auto affine = m.toWorld;
			for (int i = 0; i < 3; ++i) {
				affine.translate[i] -= a_chunk.origin[i];
			}
			float memberLo[3]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
			float memberHi[3]{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
			Vertex::Transform(a_from, source.data(), a_to, a_chunk.vertices.data() + static_cast<std::size_t>(vertexBase) * a_to.stride, m.vertexCount, affine, memberLo, memberHi);
			float offset = 0.0F;
			float extent = 0.0F;
			for (int i = 0; i < 3; ++i) {
				lo[i] = std::min(lo[i], memberLo[i]);
				hi[i] = std::max(hi[i], memberHi[i]);
				const float off = 0.5F * (memberLo[i] + memberHi[i]) + a_chunk.origin[i] - m.center[i];
				const float half = 0.5F * (memberHi[i] - memberLo[i]);
				offset += off * off;
				extent += half * half;
			}
			offset = std::sqrt(offset);
			extent = std::sqrt(extent);
			if (const float limit = 2.0F * m.radius + 64.0F; offset > limit || extent > limit) {
				if (a_chunk.strays++ == 0) {
					a_chunk.firstStray = index;
					a_chunk.strayOffset = offset;
					a_chunk.strayExtent = extent;
				}
			}
			for (std::size_t i = 0; i < indexCount; ++i) {
				a_chunk.indices[indexBase + i] = static_cast<std::uint16_t>(sourceIndices[i] + vertexBase);
			}
			starts.push_back(indexBase);
			vertexBase += m.vertexCount;
			indexBase += static_cast<std::uint32_t>(indexCount);
			a_chunk.baked.push_back(index);
		}
		if (a_chunk.baked.size() < a_minMembers || vertexBase == 0) {
			a_chunk.vertices.clear();
			a_chunk.indices.clear();
			return false;
		}
		a_chunk.vertices.resize(static_cast<std::size_t>(vertexBase) * a_to.stride);
		a_chunk.indices.resize(indexBase);
		a_chunk.vertexCount = vertexBase;
		a_chunk.triangleCount = indexBase / 3;

		// Segment order (see Chunk::lodSizes): each member's triangles keep their order inside a segment.
		std::vector<std::uint16_t> ordered;
		ordered.reserve(indexBase);
		for (std::uint32_t segment = 0; segment < 3; ++segment) {
			a_chunk.lodSizes[segment] = 0;
			for (std::size_t i = 0; i < a_chunk.baked.size(); ++i) {
				const auto sizes = Segments(a_members[a_chunk.baked[i]]);
				std::uint32_t first = starts[i];
				for (std::uint32_t s = 0; s < segment; ++s) {
					first += sizes[s] * 3;
				}
				ordered.insert(ordered.end(), a_chunk.indices.begin() + first, a_chunk.indices.begin() + first + sizes[segment] * 3);
				a_chunk.lodSizes[segment] += sizes[segment];
			}
		}
		a_chunk.indices = std::move(ordered);

		// Bound: the box centre, radius to the farthest written vertex. The box's half diagonal overshoots a cluster
		// by up to ~40% (its corners are mostly empty), and the sun's cascades file a chunk by its bound (run 25).
		for (int i = 0; i < 3; ++i) {
			a_chunk.boundCenter[i] = 0.5F * (lo[i] + hi[i]);
		}
		float radius = 0.0F;
		for (std::uint32_t v = 0; v < vertexBase; ++v) {
			float p[4];
			Vertex::ReadPosition(a_chunk.vertices.data() + static_cast<std::size_t>(v) * a_to.stride, a_to, p);
			const float d[3]{ p[0] - a_chunk.boundCenter[0], p[1] - a_chunk.boundCenter[1], p[2] - a_chunk.boundCenter[2] };
			radius = std::max(radius, d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		}
		a_chunk.boundRadius = std::sqrt(radius);
		return true;
	}
}
