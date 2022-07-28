/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/
// tjunc.c

#include <qbsp/qbsp.hh>
#include <qbsp/map.hh>
#include <atomic>

struct tjunc_stats_t
{
	// # of degenerate edges reported (with two identical input vertices)
	std::atomic<size_t> degenerate;
	// # of new edges created to close a tjunction
	// (also technically the # of points detected that lay on other faces' edges)
	std::atomic<size_t> tjunctions;
	// # of faces that were created as a result of splitting faces that are too large
	// to be contained on a single face
	std::atomic<size_t> faceoverflows;
	// # of faces that were degenerate and were just collapsed altogether.
	std::atomic<size_t> facecollapse;
	// # of faces that were able to be fixed just by rotating the start point.
	std::atomic<size_t> rotates;
	// # of faces that weren't able to be fixed with start point rotation
	std::atomic<size_t> norotates;
	// # of faces that could be successfully retopologized
	std::atomic<size_t> retopology;
	// # of faces generated by retopologization
	std::atomic<size_t> faceretopology;
	// # of faces that were successfully topologized by MWT
	std::atomic<size_t> mwt;
	// # of triangles computed by MWT
	std::atomic<size_t> trimwt;
	// # of faces added by MWT
	std::atomic<size_t> facemwt;
};

inline std::optional<vec_t> PointOnEdge(const qvec3d &p, const qvec3d &edge_start, const qvec3d &edge_dir, float start = 0, float end = 1)
{
	qvec3d delta = p - edge_start;
	vec_t dist = qv::dot(delta, edge_dir);

	// check if off an end
	if (dist <= start || dist >= end) {
		return std::nullopt;
	}

	qvec3d exact = edge_start + (edge_dir * dist);
	qvec3d off = p - exact;
	vec_t error = qv::length(off);

	// brushbsp-fixme: this was 0.5 in Q2, check?
	if (fabs(error) > DEFAULT_ON_EPSILON) {
		// not on the edge
		return std::nullopt;
	}

	return dist;
}

/*
==========
TestEdge

Can be recursively reentered
==========
*/
inline void TestEdge(vec_t start, vec_t end, size_t p1, size_t p2, size_t startvert, const std::vector<size_t> &edge_verts,
	const qvec3d &edge_start, const qvec3d &edge_dir, std::vector<size_t> &superface, tjunc_stats_t &stats)
{
	if (p1 == p2) {
		// degenerate edge
		stats.degenerate++;
		return;
	}

	for (size_t k = startvert; k < edge_verts.size(); k++) {
		size_t j = edge_verts[k];

		if (j == p1 || j == p2) {
			continue;
		}

		auto dist = PointOnEdge(map.bsp.dvertexes[j], edge_start, edge_dir, start, end);

		if (!dist.has_value()) {
			continue;
		}

		// break the edge
		stats.tjunctions++;

		TestEdge (start, dist.value(), p1, j, k + 1, edge_verts, edge_start, edge_dir, superface, stats);
		TestEdge (dist.value(), end, j, p2, k + 1, edge_verts, edge_start, edge_dir, superface, stats);
		return;
	}

	// the edge p1 to p2 is now free of tjunctions
	superface.push_back(p1);
}

/*
==========
FindEdgeVerts_BruteForce

Force a dumb check of everything
==========
*/
static void FindEdgeVerts_BruteForce(const node_t *, const node_t *, const qvec3d &, const qvec3d &, std::vector<size_t> &verts)
{
	verts.resize(map.bsp.dvertexes.size());

	for (size_t i = 0; i < verts.size(); i++) {
		verts[i] = i;
	}
}

/*
==========
FindEdgeVerts_FaceBounds_R

Recursive function for matching nodes that intersect the aabb
for vertex checking.
==========
*/
static void FindEdgeVerts_FaceBounds_R(const node_t *node, const aabb3d &aabb, std::vector<size_t> &verts)
{
	if (node->is_leaf) {
		return;
	} else if (node->bounds.disjoint(aabb, 0.0)) {
		return;
	}

	for (auto &face : node->facelist) {
		for (auto &v : face->original_vertices) {
			if (aabb.containsPoint(map.bsp.dvertexes[v])) {
				verts.push_back(v);
			}
		}
	}
	
	FindEdgeVerts_FaceBounds_R(node->children[0].get(), aabb, verts);
	FindEdgeVerts_FaceBounds_R(node->children[1].get(), aabb, verts);
}

/*
==========
FindEdgeVerts_FaceBounds

Use a loose AABB around the line and only capture vertices that intersect it.
==========
*/
static void FindEdgeVerts_FaceBounds(const node_t *headnode, const qvec3d &p1, const qvec3d &p2, std::vector<size_t> &verts)
{
	// magic number, average of "usual" points per edge
	verts.reserve(8);

	FindEdgeVerts_FaceBounds_R(headnode, (aabb3d{} + p1 + p2).grow(qvec3d(1.0, 1.0, 1.0)), verts);
}

/*
==================
SplitFaceIntoFragments

The face was created successfully, but may have way too many edges.
Cut it down to the minimum amount of faces that are within the
max edge count.

Modifies `superface`. Adds the results to the end of `output`.
==================
*/
inline void SplitFaceIntoFragments(std::vector<size_t> &superface, std::list<std::vector<size_t>> &output, tjunc_stats_t &stats)
{
	const int32_t &maxedges = qbsp_options.maxedges.value();

	// split into multiple fragments, because of vertex overload
	while (superface.size() > maxedges) {
		stats.faceoverflows++;

		// copy MAXEDGES from our current face
		std::vector<size_t> &newf = output.emplace_back(maxedges);
		std::copy_n(superface.begin(), maxedges, newf.begin());

		// remove everything in-between from the superface
		// except for the last edge we just wrote (0 and MAXEDGES-1)
		std::copy(superface.begin() + maxedges - 1, superface.end(), superface.begin() + 1);

		// resize superface; we need enough room to store the two extra verts
		superface.resize(superface.size() - maxedges + 2);
	}

	// move the first face to the end, since that's logically where it belongs now
	output.splice(output.end(), output, output.begin());
}

float AngleOfTriangle(const qvec3d &a, const qvec3d &b, const qvec3d &c)
{
    vec_t num = (b[0]-a[0])*(c[0]-a[0])+(b[1]-a[1])*(c[1]-a[1])+(b[2]-a[2])*(c[2]-a[2]);
    vec_t den = sqrt(pow((b[0]-a[0]),2)+pow((b[1]-a[1]),2)+pow((b[2]-a[2]),2))*
                sqrt(pow((c[0]-a[0]),2)+pow((c[1]-a[1]),2)+pow((c[2]-a[2]),2));
 
    return acos(num / den) * (180.0 / 3.141592653589793238463);
}

// Check whether the given input triangle would be valid
// on the given face and not have any other points
// intersecting it.
inline bool TriangleIsValid(size_t v0, size_t v1, size_t v2, vec_t angle_epsilon)
{
	if (AngleOfTriangle(map.bsp.dvertexes[v0], map.bsp.dvertexes[v1], map.bsp.dvertexes[v2]) < angle_epsilon ||
		AngleOfTriangle(map.bsp.dvertexes[v1], map.bsp.dvertexes[v2], map.bsp.dvertexes[v0]) < angle_epsilon ||
		AngleOfTriangle(map.bsp.dvertexes[v2], map.bsp.dvertexes[v0], map.bsp.dvertexes[v1]) < angle_epsilon) {
		return false;
	}

	return true;
}

/*
==================
CreateSuperFace

Generate a superface (the input face `f` but with all of the
verts in the world added that lay on the line) and return it
==================
*/
static std::vector<size_t> CreateSuperFace(node_t *headnode, face_t *f, tjunc_stats_t &stats)
{
	std::vector<size_t> superface;

	superface.reserve(f->original_vertices.size() * 2);

	// stores all of the verts in the world that are close to
	// being on a given edge
	std::vector<size_t> edge_verts;

	// find all of the extra vertices that lay on edges,
	// place them in superface
	for (size_t i = 0; i < f->original_vertices.size(); i++) {
		auto v1 = f->original_vertices[i];
		auto v2 = f->original_vertices[(i + 1) % f->original_vertices.size()];

		qvec3d edge_start = map.bsp.dvertexes[v1];
		qvec3d e2 = map.bsp.dvertexes[v2];

		edge_verts.clear();
		FindEdgeVerts_FaceBounds(headnode, edge_start, e2, edge_verts);

		vec_t len;
		qvec3d edge_dir = qv::normalize(e2 - edge_start, len);

		TestEdge(0, len, v1, v2, 0, edge_verts, edge_start, edge_dir, superface, stats);
	}

	return superface;
}

#include <common/bsputils.hh>
#include <fstream>

using qvectri = qvec<size_t, 3>;

// check if the given triangle exists in the set of triangles
// in any permutation
std::optional<size_t> triangle_exists(const std::vector<qvectri> &triangles, size_t a, size_t b, size_t c)
{
	for (size_t i = 0; i < triangles.size(); i++) {
		auto &tri = triangles[i];

		for (size_t s = 0; s < 3; s++) {
			if (tri[s] == a && tri[(s + 1) % 3] == b && tri[(s + 2) % 3] == c) {
				return i;
			}
		}
	}

	return std::nullopt;
}

// find the triangles best suited to create a
// fan out of in the given set of triangles.
std::vector<size_t> find_best_fan(const std::vector<qvectri> &triangles, size_t num_vertices)
{
	// find the triangle with the most fannable vertices.
	std::vector<size_t> best_triangles;

	for (auto &tri : triangles) {
		// try all three permutations
		for (size_t perm = 0; perm < 3; perm++) {
			size_t first = tri[perm];
			size_t mid = tri[(perm + 1) % 3];
			size_t last = tri[(perm + 2) % 3];

			std::vector<size_t> my_tri;

			// find any other that can be wound from this edge
			// TODO: can optimize by only looping around the verts
			// included in the triangle
			for (; last != first; last = (last + 1) % num_vertices) {
				auto ftri = triangle_exists(triangles, first, mid, last);

				// no triangle found for A B C, so try again
				// with A B D, etc.
				if (ftri == std::nullopt) {
					continue;
				}

				// found A B C, so go next (A C D)
				my_tri.push_back(ftri.value());
				mid = last;
			}

			if (best_triangles.empty() || my_tri.size() > best_triangles.size()) {
				best_triangles = std::move(my_tri);
			}
		}
	}

	return best_triangles;
}

// find the seed vertex (vertex referenced by the most edges) of
// the fan.
size_t find_seed_vertex(const std::vector<qvectri> &triangles, const std::vector<size_t> &fan)
{
	std::unordered_set<size_t> verts{triangles[fan[0]].begin(), triangles[fan[0]].end()};

	for (size_t i = 1; i < fan.size(); i++)
	{
		auto &tri = triangles[fan[i]];

		// produce intersection
		for (auto it = verts.begin(); it != verts.end(); ) {
			if (std::find(tri.begin(), tri.end(), *it) == tri.end()) {
				it = verts.erase(it);
			} else {
				it++;
			}
		}

		// if there's only one vert left it has to be that one
		if (verts.size() == 1) {
			return *verts.begin();
		}
	}

	// just pick whatever's left
	return *verts.begin();
}

static std::list<std::vector<size_t>> compress_triangles_into_fans(std::vector<qvectri> &triangles, const std::vector<size_t> &vertices)
{
	std::list<std::vector<size_t>> tris_compiled;

	while (triangles.size()) {
		auto fan = find_best_fan(triangles, vertices.size());

		Q_assert(fan.size());

		// when we run into only 1 triangle fans left,
		// just add the rest directly.
		if (fan.size() == 1) {
			for (auto &tri : triangles) {
				tris_compiled.emplace_back(std::vector<size_t>{ vertices[tri[0]], vertices[tri[1]], vertices[tri[2]] });
			}

			triangles.clear();
			break;
		}

		// a fan can be made! find the seed vertex
		auto seed = find_seed_vertex(triangles, fan);

		struct tri_verts_less_pred
		{
			size_t seed, vert_count;

			bool operator()(const size_t &a, const size_t &b) const
			{
				size_t ka = a < seed ? vert_count + a : a;
				size_t kb = b < seed ? vert_count + b : b;

				return ka < kb;
			}
		};

		// add all the verts and order them so they match
		// the proper winding
		std::set<size_t, tri_verts_less_pred> verts(tri_verts_less_pred { seed, vertices.size() });

		for (auto tri_index : fan) {
			auto &tri = triangles[tri_index];

			for (auto &v : tri) {
				verts.insert(v);
			}
		}

		Q_assert(verts.size() >= 3);

		// add the new winding
		auto &out_tri = tris_compiled.emplace_back(verts.begin(), verts.end());

		for (auto &v : out_tri) {
			v = vertices[v];
		}

		// remove all of the fans from the triangle list
		std::sort(fan.begin(), fan.end(), [](auto &l, auto &r) { return l > r; });

		for (auto tri_index : fan) {
			triangles.erase(triangles.begin() + tri_index);
		}
	}
	
	return tris_compiled;
}

#include <queue>

// Function to calculate the weight of optimal triangulation of a convex polygon
// represented by a given set of vertices
std::vector<qvectri> minimum_weight_triangulation(const std::vector<size_t> &indices, const std::vector<qvec2d> &vertices)
{
    // get the number of vertices in the polygon
    size_t n = vertices.size();
 
    // create a table for storing the solutions to subproblems
    // `T[i][j]` stores the weight of the minimum-weight triangulation
    // of the polygon below edge `ij`
    std::vector<vec_t> T(n * n);
	std::vector<std::optional<size_t>> K(n * n);
 
    // fill the table diagonally using the recurrence relation
    for (size_t diagonal = 0; diagonal < n; diagonal++) {
        for (size_t i = 0, j = diagonal; j < n; i++, j++) {
            // If the polygon has less than 3 vertices, triangulation is not possible
            if (j < i + 2) {
                continue;
            }
 
            T[i + (j * n)] = std::numeric_limits<vec_t>::max();
 
            // consider all possible triangles `ikj` within the polygon
            for (size_t k = i + 1; k <= j - 1; k++) {
                // The weight of triangulation is the length of its perimeter
                vec_t weight;
				
				if (!TriangleIsValid(indices[i], indices[j], indices[k], 0.01)) {
					weight = std::nexttoward(std::numeric_limits<vec_t>::max(), 0.0);
				} else {
					weight = (qv::distance(vertices[i], vertices[j]) +
                            qv::distance(vertices[j], vertices[k]) +
                            qv::distance(vertices[k], vertices[i]))
							+ T[i + (k * n)] + T[k + (j * n)];
				}
				vec_t &t_weight = T[i + (j * n)];
 
                // choose vertex `k` that leads to the minimum total weight
				if (weight < t_weight) {
					t_weight = weight;
					K[i + (j * n)] = k;
				}
            }
        }
    }

	std::vector<qvectri> triangles;
	std::queue<qvec<size_t, 2>> edge_queue;
	edge_queue.emplace(0, n - 1);

	while (!edge_queue.empty()) {
		auto edge = edge_queue.front();
		edge_queue.pop();

		if (edge[0] == edge[1]) {
			continue;
		}
		
		auto &c = K[edge[0] + (edge[1] * n)];
		
		if (!c.has_value()) {
			continue;
		}

		qvectri tri { edge[0], edge[1], c.value() };
		std::sort(tri.begin(), tri.end());
		triangles.emplace_back(tri);

		edge_queue.emplace(edge[0], c.value());
		edge_queue.emplace(c.value(), edge[1]);
	}

	Q_assert(triangles.size() == n - 2);

	return triangles;
}

static std::list<std::vector<size_t>> mwt_face(const face_t *f, const std::vector<size_t> &vertices, tjunc_stats_t &stats)
{
	auto p = f->plane;

	if (f->plane_flipped) {
		p = -p;
	}

	auto [ u, v ] = qv::MakeTangentAndBitangentUnnormalized(p.normal);
	qv::normalizeInPlace(u);
	qv::normalizeInPlace(v);

	std::vector<qvec2d> points_2d(vertices.size());

	for (size_t i = 0; i < vertices.size(); i++) {
		points_2d[i] = { qv::dot(map.bsp.dvertexes[vertices[i]], u), qv::dot(map.bsp.dvertexes[vertices[i]], v) };
	}

	auto tris = minimum_weight_triangulation(vertices, points_2d);

	stats.trimwt += tris.size();

	return compress_triangles_into_fans(tris, vertices);
}

/*
==================
RetopologizeFace

A face has T-junctions that can't be resolved from rotation.
It's still a convex face with wound vertices, though, so we
can split it into several triangle fans.
==================
*/
static std::list<std::vector<size_t>> RetopologizeFace(const face_t *f, const std::vector<size_t> &vertices)
{
	std::list<std::vector<size_t>> result;
	// the copy we're working on
	std::vector<size_t> input(vertices);

	while (input.size()) {
		// failure if we somehow degenerated a triangle
		if (input.size() < 3) {
			return {};
		}

		size_t seed = 0;
		int64_t end = 0;

		// find seed triangle (allowing a wrap around,
		// because it's possible for only the last two triangles
		// to be valid).
		for (; seed < input.size(); seed++) {
			auto v0 = input[seed];
			auto v1 = input[(seed + 1) % input.size()];
			end = (seed + 2) % input.size();
			auto v2 = input[end];

			if (!TriangleIsValid(v0, v1, v2, 0.01)) {
				continue;
			}

			// if the next point lays on the edge of v0-v2, this next
			// triangle won't be valid.
			float len;
			qvec3d dir = qv::normalize(map.bsp.dvertexes[v0] - map.bsp.dvertexes[v2], len);
			auto dist = PointOnEdge(map.bsp.dvertexes[input[(end + 1) % input.size()]], map.bsp.dvertexes[v2], dir, 0, len);
			
			if (dist.has_value()) {
				continue;
			}

			// good one!
			break;
		}

		if (seed == input.size()) {
			// can't find a non-zero area triangle; failure
			return {};
		}

		// from the seed vertex, keep winding until we hit a zero-area triangle.
		// we know that triangle (seed, end - 1, end) is valid, so we wind from
		// end + 1 until we fully wrap around. We also can't include a triangle
		// that has a point after it laying on the final edge.
		size_t wrap = end;
		end = (end + 1) % input.size();

		for (; end != wrap; end = (end + 1) % input.size()) {
			auto v0 = input[seed];
			auto v1 = input[(end - 1) < 0 ? (input.size() - 1) : (end - 1)];
			auto v2 = input[end];

			// if the next point lays on the edge of v0-v2, this next
			// triangle won't be valid.
			float len;
			qvec3d dir = qv::normalize(map.bsp.dvertexes[v0] - map.bsp.dvertexes[v2], len);
			auto dist = PointOnEdge(map.bsp.dvertexes[input[(end + 1) % input.size()]], map.bsp.dvertexes[v2], dir, 0, len);

			if (dist.has_value()) {
				// the next point lays on this edge, so back up and stop
				end = (end - 1) < 0 ? input.size() - 1 : (end - 1);
				break;
			}
		}

		// now we have a fan from seed to end that is valid.
		// add it to the result, clip off all of the
		// points between it and restart the algorithm
		// using that edge.
		auto &tri = result.emplace_back();

		// the whole fan can just be moved; we're finished.
		if (seed == end) {
			tri = std::move(input);
			break;
		} else if (end == wrap) {
			// we successfully wrapped around, but the
			// seed vertex isn't at the start, so rotate it.
			// copy base -> end
			tri.resize(input.size());
			auto out = std::copy(input.begin() + seed, input.end(), tri.begin());
			// copy end -> base
			std::copy(input.begin(), input.begin() + seed, out);
			break;
		}
		
		if (end < seed) {
			// the end point is 'behind' the seed, so we're clipping
			// off two sides of the input
			size_t x = seed;
			bool first = true;

			while (true) {
				if (x == end && !first) {
					break;
				}

				tri.emplace_back(input[x]);
				x = (x + 1) % input.size();
				first = false;
			}
		} else {
			// simple case where the end point is ahead of the seed;
			// copy the range over to the output
			std::copy(input.begin() + seed, input.begin() + end + 1, std::back_inserter(tri));
		}

		Q_assert(seed != end);

		if (end < seed) {
			// slightly more complex case: the end point is behind the seed point.
			// 0 end 2 3 seed 5 6
			// end 2 3 seed
			// calculate new size
			size_t new_size = (seed + 1) - end;

			// move back the end to the start
			std::copy(input.begin() + end, input.begin() + seed + 1, input.begin());
			
			// clip
			input.resize(new_size);
		} else {
			// simple case: the end point is ahead of the seed point.
			// collapse the range after it backwards over top of the seed
			// and clip it off
			// 0 1 2 seed 4 5 6 end 8 9
			// 0 1 2 seed end 8 9
			// calculate new size
			size_t new_size = input.size() - (end - seed - 1);

			// move range
			std::copy(input.begin() + end, input.end(), input.begin() + seed + 1);
			
			// clip
			input.resize(new_size);
		}
	}

	// finished
	return result;
}

/*
==================
FixFaceEdges

If the face has any T-junctions, fix them here.
==================
*/
static void FixFaceEdges(node_t *headnode, face_t *f, tjunc_stats_t &stats)
{
	// we were asked not to bother fixing any of the faces.
	if (qbsp_options.tjunc.value() == settings::tjunclevel_t::NONE) {
		f->fragments.emplace_back(face_fragment_t { f->original_vertices });
		return;
	}

	std::vector<size_t> superface = CreateSuperFace(headnode, f, stats);

	if (superface.size() < 3) {
		// entire face collapsed
		stats.facecollapse++;
		return;
	} else if (superface.size() == 3) {
		// no need to adjust this either
		f->fragments.emplace_back(face_fragment_t { f->original_vertices });
		return;
	}

	// faces with 4 or more vertices can be done better.
	// temporary storage for result faces; stored as a list
	// since a resize may steal references out from underneath us
	// as the functions do their work.
	std::list<std::vector<size_t>> faces;
	
	// do MWT first; it will generate optimal results for everything.
	if (qbsp_options.tjunc.value() >= settings::tjunclevel_t::MWT) {
		faces = mwt_face(f, superface, stats);

		if (faces.size()) {
			stats.mwt++;
			stats.facemwt += faces.size() - 1;
		}
	}
	
	// brute force rotating the start point until we find a valid winding
	// that doesn't have any T-junctions
	if (!faces.size() && qbsp_options.tjunc.value() >= settings::tjunclevel_t::ROTATE) {
		size_t i = 0;

		for (; i < superface.size(); i++) {
			size_t x = 0;

			// try vertex i as the base, see if we find any zero-area triangles
			for (; x < superface.size() - 2; x++) {
				auto v0 = superface[i];
				auto v1 = superface[(i + x + 1) % superface.size()];
				auto v2 = superface[(i + x + 2) % superface.size()];

				if (!TriangleIsValid(v0, v1, v2, 0.01)) {
					break;
				}
			}

			if (x == superface.size() - 2) {
				// found one!
				break;
			}
		}

		if (i == superface.size()) {
			// can't simply rotate to eliminate zero-area triangles, so we have
			// to do a bit of re-topology.
			if (qbsp_options.tjunc.value() >= settings::tjunclevel_t::RETOPOLOGIZE) {
				if (auto retopology = RetopologizeFace(f, superface); retopology.size() > 1) {
					stats.retopology++;
					stats.faceretopology += retopology.size() - 1;
					faces = std::move(retopology);
				}
			}

			if (!faces.size()) {
				// unable to re-topologize, so just stick with the superface.
				// it's got zero-area triangles that fill in the gaps.
				stats.norotates++;
			}
		} else if (i != 0) {
			// was able to rotate the superface to eliminate zero-area triangles.
			stats.rotates++;

			auto &output = faces.emplace_back(superface.size());
			// copy base -> end
			auto out = std::copy(superface.begin() + i, superface.end(), output.begin());
			// copy end -> base
			std::copy(superface.begin(), superface.begin() + i, out);
		}
	}

	// the other techniques all failed, or we asked to not
	// try them. just move the superface in directly.
	if (!faces.size()) {
		faces.emplace_back(std::move(superface));
	}

	Q_assert(faces.size());

	// split giant superfaces into subfaces if we have an edge limit.
	if (qbsp_options.maxedges.value()) {
		for (auto &face : faces) {
			Q_assert(face.size() >= 3);
			SplitFaceIntoFragments(face, faces, stats);
			Q_assert(face.size() >= 3);
		}
	}

	// move the results into the face
	f->fragments.reserve(faces.size());

	for (auto &face : faces) {
		f->fragments.emplace_back(face_fragment_t { std::move(face) });
	}

	for (auto &frag : f->fragments) {
		Q_assert(frag.output_vertices.size() >= 3);
	}
}

#include <common/parallel.hh>

/*
==================
FixEdges_r
==================
*/
static void FindFaces_r(node_t *node, std::unordered_set<face_t *> &faces)
{
	if (node->is_leaf) {
		return;
	}

	for (auto &f : node->facelist) {
		// might have been omitted, so `original_vertices` will be empty
		if (f->original_vertices.size()) {
			faces.insert(f.get());
		}
	}
	
    FindFaces_r(node->children[0].get(), faces);
    FindFaces_r(node->children[1].get(), faces);
}

/*
===========
TJunc fixing entry point
===========
*/
void TJunc(node_t *headnode)
{
    logging::print(logging::flag::PROGRESS, "---- {} ----\n", __func__);

    tjunc_stats_t stats{};
	std::unordered_set<face_t *> faces;

	FindFaces_r(headnode, faces);
	
	logging::parallel_for_each(faces, [&](auto &face) {
		FixFaceEdges(headnode, face, stats);
	});

	if (stats.degenerate) {
		logging::print (logging::flag::STAT, "{:5} edges degenerated\n", stats.degenerate);
	}
	if (stats.facecollapse) {
		logging::print (logging::flag::STAT, "{:5} faces degenerated\n", stats.facecollapse);
	}
	if (stats.tjunctions) {
		logging::print (logging::flag::STAT, "{:5} edges added by tjunctions\n", stats.tjunctions);
	}
	if (stats.mwt) {
		logging::print (logging::flag::STAT, "{:5} faces ran through MWT\n", stats.mwt);
		logging::print (logging::flag::STAT, "{:5} new faces added via MWT (from {} triangles)\n", stats.facemwt, stats.trimwt);
	}
	if (stats.retopology) {
		logging::print (logging::flag::STAT, "{:5} faces re-topologized\n", stats.retopology);
		logging::print (logging::flag::STAT, "{:5} new faces added by re-topology\n", stats.faceretopology);
	}
	if (stats.rotates) {
		logging::print (logging::flag::STAT, "{:5} faces rotated\n", stats.rotates);
	}
	if (stats.norotates) {
		logging::print (logging::flag::STAT, "{:5} faces unable to be rotated or re-topologized\n", stats.norotates);
	}
	if (stats.faceoverflows) {
		logging::print (logging::flag::STAT, "{:5} faces added by splitting large faces\n", stats.faceoverflows);
	}
}
