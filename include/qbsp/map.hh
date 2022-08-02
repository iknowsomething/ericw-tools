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

#pragma once

#include <qbsp/qbsp.hh>

#include <common/bspfile.hh>
#include <common/parser.hh>
#include "common/cmdlib.hh"

#include <optional>
#include <vector>
#include <utility>
#include <unordered_map>
#include <list>
#include <mutex>
#include <shared_mutex>

struct bspbrush_t;

struct mapface_t
{
    //qbsp_plane_t plane{};
    size_t planenum;
    std::array<qvec3d, 3> planepts{};
    std::string texname{};
    int texinfo = 0;
    int linenum = 0;

    surfflags_t flags{};

    // Q2 stuff
    contentflags_t contents{};
    int value = 0;

    // for convert
    std::optional<extended_texinfo_t> raw_info;

    bool set_planepts(const std::array<qvec3d, 3> &pts);

    const texvecf &get_texvecs() const;
    void set_texvecs(const texvecf &vecs);

    const qbsp_plane_t &get_plane() const;
};

enum class brushformat_t
{
    NORMAL,
    BRUSH_PRIMITIVES
};

class mapbrush_t
{
public:
    int firstface = 0;
    int numfaces = 0;
    brushformat_t format = brushformat_t::NORMAL;
    int contents = 0;

    const mapface_t &face(int i) const;
};

struct lumpdata
{
    int count;
    int index;
    void *data;
};

class mapentity_t
{
public:
    qvec3d origin{};

    int firstmapbrush = 0;
    int nummapbrushes = 0;

    // key/value pairs in the order they were parsed
    entdict_t epairs;

    aabb3d bounds;
    std::vector<std::unique_ptr<bspbrush_t>> brushes;

    int firstoutputfacenumber = -1;
    std::optional<size_t> outputmodelnumber = std::nullopt;

    int32_t areaportalnum = 0;
    std::array<int32_t, 2> portalareas = {};

    const mapbrush_t &mapbrush(int i) const;
};

struct maptexdata_t
{
    std::string name;
    surfflags_t flags;
    int32_t value;
    std::string animation;
    int32_t animation_miptex = -1;
};

#include <common/imglib.hh>

extern std::shared_mutex map_planes_lock;

struct hashvert_t
{
    qvec3d point;
    size_t num;
};

struct mapplane_t : qbsp_plane_t
{
    std::optional<size_t> outputnum;

    inline mapplane_t(const qbsp_plane_t &copy) : qbsp_plane_t(copy) { }
};

struct mapdata_t
{
    /* Arrays of actual items */
    std::vector<mapface_t> faces;
    std::vector<mapbrush_t> brushes;
    std::vector<mapentity_t> entities;

    // this vector stores all of the planes that can potentially be
    // output in the BSP, from the map's own sides. The positive planes
    // come first (are even-numbered, with 0 being even) and the negative
    // planes are odd-numbered.
    std::vector<mapplane_t> planes;

    // add the specified plane to the list
    inline size_t add_plane(const qplane3d &plane)
    {
        planes.reserve(planes.size() + 2);
        auto &positive = planes.emplace_back(plane);
        auto &negative = planes.emplace_back(-plane);

        if (positive.get_normal()[static_cast<int32_t>(positive.get_type()) % 3] < 0.0) {
            std::swap(positive, negative);
            return planes.size() - 1;
        }

        return planes.size() - 2;
    }
    
    // find the specified plane in the list if it exists. throws
    // if not.
    inline size_t find_plane(const qplane3d &plane)
    {
        for (size_t i = 0; i < planes.size(); i++) {
            if (qv::epsilonEqual(planes[i], plane)) {
                return i;
            }
        }

        throw std::bad_function_call();
    }

    // find the specified plane in the list if it exists, or
    // return a new one
    inline size_t add_or_find_plane(const qplane3d &plane)
    {
        for (size_t i = 0; i < planes.size(); i++) {
            if (qv::epsilonEqual(planes[i], plane)) {
                return i;
            }
        }

        return add_plane(plane);
    }

    inline const qbsp_plane_t &get_plane(size_t pnum)
    {
        return planes[pnum];
    }

    std::vector<maptexdata_t> miptex;
    std::vector<maptexinfo_t> mtexinfos;

    /* quick lookup for texinfo */
    std::map<maptexinfo_t, int> mtexinfo_lookup;

    /* map from plane hash code to list of indicies in `planes` vector */
    std::unordered_map<int, std::vector<int>> planehash;

    // hashed vertices; generated by EmitVertices
    std::map<qvec3i, std::list<hashvert_t>> hashverts;

    // find vector of points in hash closest to vec
    inline auto find_hash_vector(const qvec3d &vec)
    {
        return hashverts.find({floor(vec[0]), floor(vec[1]), floor(vec[2])});
    }

    // find output index for specified already-output vector.
    inline std::optional<size_t> find_emitted_hash_vector(const qvec3d &vert)
    {
        if (auto it = find_hash_vector(vert); it != hashverts.end()) {
            for (hashvert_t &hv : it->second) {
                if (qv::epsilonEqual(hv.point, vert, POINT_EQUAL_EPSILON)) {
                    return hv.num;
                }
            }
        }

        return std::nullopt;
    }

    // add vector to hash
    inline void add_hash_vector(const qvec3d &point, const size_t &num)
    {
        // insert each vert at floor(pos[axis]) and floor(pos[axis]) + 1 (for each axis)
        // so e.g. a vert at (0.99, 0.99, 0.99) shows up if we search at (1.01, 1.01, 1.01)
        // this is a bit wasteful..
        for (int32_t x = -1; x <= 1; x++) {
            for (int32_t y = -1; y <= 1; y++) {
                for (int32_t z = -1; z <= 1; z++) {
                    const qvec3i h{floor(point[0]) + x, floor(point[1]) + y, floor(point[2]) + z};
                    hashverts[h].push_front({point, num});
                }
            }
        }
    }

    // hashed edges; generated by EmitEdges
    std::map<std::pair<size_t, size_t>, int64_t> hashedges;

    inline void add_hash_edge(size_t v1, size_t v2, int64_t i) { hashedges.emplace(std::make_pair(v1, v2), i); }

    /* Misc other global state for the compile process */
    bool leakfile = false; /* Flag once we've written a leak (.por/.pts) file */

    // Final, exported BSP
    mbsp_t bsp;

    // bspx data
    std::vector<uint8_t> exported_lmshifts;
    bool needslmshifts = false;
    std::vector<uint8_t> exported_bspxbrushes;

    // Q2 stuff
    int32_t c_areas = 0;
    int32_t numareaportals = 0;
    // running total
    uint32_t brush_offset = 0;
    // Small cache for image meta in the current map
    std::unordered_map<std::string, std::optional<img::texture_meta>> meta_cache;
    // load or fetch image meta associated with the specified name
    const std::optional<img::texture_meta> &load_image_meta(const std::string_view &name);
    // whether we had attempted loading texture stuff
    bool textures_loaded = false;

    // helpers
    const std::string &miptexTextureName(int mt) const { return miptex.at(mt).name; }

    const std::string &texinfoTextureName(int texinfo) const { return miptexTextureName(mtexinfos.at(texinfo).miptex); }

    int skip_texinfo;

    mapentity_t *world_entity();

    void reset();
};

extern mapdata_t map;

void CalculateWorldExtent(void);

bool ParseEntity(parser_t &parser, mapentity_t *entity);

void ProcessExternalMapEntity(mapentity_t *entity);
void ProcessAreaPortal(mapentity_t *entity);
bool IsWorldBrushEntity(const mapentity_t *entity);
bool IsNonRemoveWorldBrushEntity(const mapentity_t *entity);
void LoadMapFile(void);
void ConvertMapFile(void);

struct quark_tx_info_t
{
    bool quark_tx1 = false;
    bool quark_tx2 = false;

    std::optional<extended_texinfo_t> info;
};

int FindMiptex(
    const char *name, std::optional<extended_texinfo_t> &extended_info, bool internal = false, bool recursive = true);
inline int FindMiptex(const char *name, bool internal = false, bool recursive = true)
{
    std::optional<extended_texinfo_t> extended_info;
    return FindMiptex(name, extended_info, internal, recursive);
}
int FindTexinfo(const maptexinfo_t &texinfo);

void PrintEntity(const mapentity_t *entity);

void WriteEntitiesToString();

qvec3d FixRotateOrigin(mapentity_t *entity);

/** Special ID for the collision-only hull; used for wrbrushes/Q2 */
constexpr int HULL_COLLISION = -1;

/* Create BSP brushes from map brushes */
void Brush_LoadEntity(mapentity_t *entity, const int hullnum);

std::list<face_t *> CSGFace(
    face_t *srcface, const mapentity_t *srcentity, const bspbrush_t *srcbrush, const node_t *srcnode);
void TJunc(node_t *headnode);
int MakeFaceEdges(node_t *headnode);
void EmitVertices(node_t *headnode);
void ExportClipNodes(mapentity_t *entity, node_t *headnode, const int hullnum);
void ExportDrawNodes(mapentity_t *entity, node_t *headnode, int firstface);

struct bspxbrushes_s
{
    std::vector<uint8_t> lumpdata;
};
void BSPX_Brushes_Finalize(struct bspxbrushes_s *ctx);
void BSPX_Brushes_Init(struct bspxbrushes_s *ctx);

void ExportObj_Faces(const std::string &filesuffix, const std::vector<const face_t *> &faces);
void ExportObj_Brushes(const std::string &filesuffix, const std::vector<const bspbrush_t *> &brushes);
void ExportObj_Nodes(const std::string &filesuffix, const node_t *nodes);
void ExportObj_Marksurfaces(const std::string &filesuffix, const node_t *nodes);

void WriteBspBrushMap(const fs::path &name, const std::vector<std::unique_ptr<bspbrush_t>> &list);

bool IsValidTextureProjection(const qvec3f &faceNormal, const qvec3f &s_vec, const qvec3f &t_vec);
