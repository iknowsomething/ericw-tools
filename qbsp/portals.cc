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
// portals.c

#include <qbsp/brush.hh>
#include <qbsp/portals.hh>

#include <qbsp/map.hh>
#include <qbsp/brushbsp.hh>
#include <qbsp/qbsp.hh>
#include <qbsp/outside.hh>

#include <atomic>

#include "tbb/task_group.h"

contentflags_t ClusterContents(const node_t *node)
{
    /* Pass the leaf contents up the stack */
    if (node->planenum == PLANENUM_LEAF)
        return node->contents;

    return options.target_game->cluster_contents(
        ClusterContents(node->children[0]), ClusterContents(node->children[1]));
}

/*
=============
Portal_VisFlood

Returns true if the portal is empty or translucent, allowing
the PVS calculation to see through it.
The nodes on either side of the portal may actually be clusters,
not leafs, so all contents should be ored together
=============
*/
bool Portal_VisFlood(const portal_t *p)
{
    if (!p->onnode) {
        return false; // to global outsideleaf
    }

    contentflags_t contents0 = ClusterContents(p->nodes[0]);
    contentflags_t contents1 = ClusterContents(p->nodes[1]);

    /* Can't see through func_illusionary_visblocker */
    if (contents0.illusionary_visblocker || contents1.illusionary_visblocker)
        return false;

    // Check per-game visibility
    return options.target_game->portal_can_see_through(contents0, contents1, options.transwater.value(), options.transsky.value());
}

/*
===============
Portal_EntityFlood

The entity flood determines which areas are
"outside" on the map, which are then filled in.
Flowing from side s to side !s
===============
*/
bool Portal_EntityFlood(const portal_t *p, int32_t s)
{
    if (p->nodes[0]->planenum != PLANENUM_LEAF
        || p->nodes[1]->planenum != PLANENUM_LEAF) {
        FError("Portal_EntityFlood: not a leaf");
    }

    // can never cross to a solid
    if (p->nodes[0]->contents.is_any_solid(options.target_game)
        || p->nodes[1]->contents.is_any_solid(options.target_game)) {
        return false;
    }

    // can flood through everything else
    return true;
}

/*
=============
AddPortalToNodes
=============
*/
static void AddPortalToNodes(portal_t *p, node_t *front, node_t *back)
{
    if (p->nodes[0] || p->nodes[1])
        FError("portal already included");

    p->nodes[0] = front;
    p->next[0] = front->portals;
    front->portals = p;

    p->nodes[1] = back;
    p->next[1] = back->portals;
    back->portals = p;
}

/*
=============
RemovePortalFromNode
=============
*/
static void RemovePortalFromNode(portal_t *portal, node_t *l)
{
    portal_t **pp, *t;

    // remove reference to the current portal
    pp = &l->portals;
    while (1) {
        t = *pp;
        if (!t)
            FError("Portal not in leaf");

        if (t == portal)
            break;

        if (t->nodes[0] == l)
            pp = &t->next[0];
        else if (t->nodes[1] == l)
            pp = &t->next[1];
        else
            FError("Portal not bounding leaf");
    }

    if (portal->nodes[0] == l) {
        *pp = portal->next[0];
        portal->nodes[0] = NULL;
    } else if (portal->nodes[1] == l) {
        *pp = portal->next[1];
        portal->nodes[1] = NULL;
    }
}

/*
================
MakeHeadnodePortals

The created portals will face the global outside_node
================
*/
void MakeHeadnodePortals(tree_t *tree)
{
    int i, j, n;
    portal_t *p, *portals[6];
    qbsp_plane_t bplanes[6];
    planeside_t side;

    // pad with some space so there will never be null volume leafs
    aabb3d bounds = tree->bounds.grow(SIDESPACE);

    tree->outside_node.planenum = PLANENUM_LEAF;
    tree->outside_node.contents = options.target_game->create_solid_contents();
    tree->outside_node.portals = NULL;

    // create 6 portals forming a cube around the bounds of the map.
    // these portals will have `outside_node` on one side, and headnode on the other.
    for (i = 0; i < 3; i++)
        for (j = 0; j < 2; j++) {
            n = j * 3 + i;

            p = new portal_t{};
            portals[n] = p;

            qplane3d &pl = bplanes[n] = {};

            if (j) {
                pl.normal[i] = -1;
                pl.dist = -bounds[j][i];
            } else {
                pl.normal[i] = 1;
                pl.dist = bounds[j][i];
            }
            p->planenum = FindPlane(pl, &side);

            p->winding = BaseWindingForPlane(pl);
            if (side)
                AddPortalToNodes(p, &tree->outside_node, tree->headnode);
            else
                AddPortalToNodes(p, tree->headnode, &tree->outside_node);
        }

    // clip the basewindings by all the other planes
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 6; j++) {
            if (j == i)
                continue;
            portals[i]->winding = portals[i]->winding->clip(bplanes[j], options.epsilon.value(), true)[SIDE_FRONT];
        }
    }
}

//============================================================================

/*
================
BaseWindingForNode

Creates a winding from the given node plane, clipped by all parent nodes.
================
*/
#define	BASE_WINDING_EPSILON	0.001
#define	SPLIT_WINDING_EPSILON	0.001

std::optional<winding_t> BaseWindingForNode(node_t *node)
{
    auto plane = map.planes.at(node->planenum);

    std::optional<winding_t> w = BaseWindingForPlane(plane);

    // clip by all the parents
    for (node_t *np = node->parent; np && w; )
    {
        plane = map.planes.at(np->planenum);

        const planeside_t keep = (np->children[0] == node) ?
            SIDE_FRONT : SIDE_BACK;

        w = w->clip(plane, BASE_WINDING_EPSILON, false)[keep];

        node = np;
        np = np->parent;
    }

    return w;
}

/*
==================
MakeNodePortal

create the new portal by taking the full plane winding for the cutting plane
and clipping it by all of parents of this node, as well as all the other
portals in the node.
==================
*/
void MakeNodePortal(node_t *node, portalstats_t &stats)
{
    auto w = BaseWindingForNode(node);

    // clip the portal by all the other portals in the node
    int side = -1;
    for (auto *p = node->portals; p && w; p = p->next[side]) {
        qbsp_plane_t plane;
        if (p->nodes[0] == node)
        {
            side = 0;
            plane = map.planes.at(p->planenum);
        }
        else if (p->nodes[1] == node)
        {
            side = 1;
            plane = -map.planes.at(p->planenum);
        }
        else
            Error("CutNodePortals_r: mislinked portal");

        w = w->clip(plane, 0.1, false)[SIDE_FRONT];
    }

    if (!w)
    {
        return;
    }

    if (WindingIsTiny(*w))
    {
        stats.c_tinyportals++;
        return;
    }

    auto *new_portal = new portal_t{};
    new_portal->planenum = node->planenum;
    new_portal->onnode = node;
    new_portal->winding = w;
    AddPortalToNodes(new_portal, node->children[0], node->children[1]);
}

/*
==============
SplitNodePortals

Move or split the portals that bound node so that the node's
children have portals instead of node.
==============
*/
void SplitNodePortals(node_t *node, portalstats_t &stats)
{
    const auto plane = map.planes.at(node->planenum);
    node_t *f = node->children[0];
    node_t *b = node->children[1];

    portal_t *next_portal = nullptr;
    for (portal_t *p = node->portals; p ; p = next_portal)
    {
        planeside_t side;
        if (p->nodes[SIDE_FRONT] == node)
            side = SIDE_FRONT;
        else if (p->nodes[SIDE_BACK] == node)
            side = SIDE_BACK;
        else
            FError("CutNodePortals_r: mislinked portal");
        next_portal = p->next[side];

        node_t *other_node = p->nodes[!side];
        RemovePortalFromNode(p, p->nodes[0]);
        RemovePortalFromNode(p, p->nodes[1]);

        //
        // cut the portal into two portals, one on each side of the cut plane
        //
        auto [frontwinding, backwinding] = p->winding->clip(plane, SPLIT_WINDING_EPSILON, true);

        if (frontwinding && WindingIsTiny(*frontwinding))
        {
            frontwinding = {};
            stats.c_tinyportals++;
        }

        if (backwinding && WindingIsTiny(*backwinding))
        {
            backwinding = {};
            stats.c_tinyportals++;
        }

        if (!frontwinding && !backwinding)
        {	// tiny windings on both sides
            continue;
        }

        if (!frontwinding)
        {
            if (side == SIDE_FRONT)
                AddPortalToNodes(p, b, other_node);
            else
                AddPortalToNodes(p, other_node, b);
            continue;
        }
        if (!backwinding)
        {
            if (side == SIDE_FRONT)
                AddPortalToNodes(p, f, other_node);
            else
                AddPortalToNodes(p, other_node, f);
            continue;
        }

        // the winding is split
        auto *new_portal = new portal_t{*p};
        new_portal->winding = backwinding;
        p->winding = frontwinding;

        if (side == SIDE_FRONT)
        {
            AddPortalToNodes(p, f, other_node);
            AddPortalToNodes(new_portal, b, other_node);
        }
        else
        {
            AddPortalToNodes(p, other_node, f);
            AddPortalToNodes(new_portal, other_node, b);
        }
    }

    node->portals = nullptr;
}

/*
================
CalcNodeBounds
================
*/
void CalcNodeBounds(node_t *node)
{
    // calc mins/maxs for both leafs and nodes
    node->bounds = aabb3d{};

    for (portal_t *p = node->portals; p ;) {
        int s = (p->nodes[1] == node);
        for (auto &point : *p->winding) {
            node->bounds += point;
        }
        p = p->next[s];
    }
}

/*
==================
MakeTreePortals_r
==================
*/
void MakeTreePortals_r(node_t *node, portalstats_t &stats)
{
    CalcNodeBounds(node);
    if (node->bounds.mins()[0] >= node->bounds.maxs()[0])
    {
        printf ("WARNING: node without a volume\n");

        // fixme-brushbsp: added this to work around leafs with no portals showing up in "qbspfeatures.map" among other
        // test maps. Not sure if correct or there's another underlying problem.
        node->bounds = { node->parent->bounds.mins(), node->parent->bounds.mins() };
    }

    for (int i = 0; i < 3; i++)
    {
        if (fabs(node->bounds.mins()[i]) > options.worldextent.value())
        {
            printf ("WARNING: node with unbounded volume\n");
            break;
        }
    }
    if (node->planenum == PLANENUM_LEAF)
        return;

    MakeNodePortal(node, stats);
    SplitNodePortals(node, stats);

    MakeTreePortals_r(node->children[0], stats);
    MakeTreePortals_r(node->children[1], stats);
}

/*
==================
MakeTreePortals
==================
*/
void MakeTreePortals(tree_t *tree)
{
    FreeTreePortals_r(tree->headnode);

    AssertNoPortals(tree->headnode);

    portalstats_t stats{};

    MakeHeadnodePortals(tree);
    MakeTreePortals_r(tree->headnode, stats);
}

void AssertNoPortals(node_t *node)
{
    Q_assert(!node->portals);

    if (node->planenum != PLANENUM_LEAF) {
        AssertNoPortals(node->children[0]);
        AssertNoPortals(node->children[1]);
    }
}

/*
==================
FreeTreePortals_r

==================
*/
void FreeTreePortals_r(node_t *node)
{
    portal_t *p, *nextp;

    if (node->planenum != PLANENUM_LEAF) {
        FreeTreePortals_r(node->children[0]);
        FreeTreePortals_r(node->children[1]);
    }

    for (p = node->portals; p; p = nextp) {
        if (p->nodes[0] == node)
            nextp = p->next[0];
        else
            nextp = p->next[1];
        RemovePortalFromNode(p, p->nodes[0]);
        RemovePortalFromNode(p, p->nodes[1]);
        delete p;
    }
    node->portals = nullptr;
}

/*
=========================================================

FLOOD AREAS

=========================================================
*/

static void ApplyArea_r(node_t *node)
{
    node->area = map.c_areas;

    if (node->planenum != PLANENUM_LEAF) {
        ApplyArea_r(node->children[0]);
        ApplyArea_r(node->children[1]);
    }
}

static mapentity_t *AreanodeEntityForLeaf(node_t *node)
{
    // if detail cluster, search the children recursively
    if (node->planenum != PLANENUM_LEAF) {
        if (auto *child0result = AreanodeEntityForLeaf(node->children[0]); child0result) {
            return child0result;
        }
        return AreanodeEntityForLeaf(node->children[1]);
    }

    for (auto &brush : node->original_brushes) {
        if (brush->func_areaportal) {
            return brush->func_areaportal;
        }
    }
    return nullptr;
}

/*
=============
FloodAreas_r
=============
*/
static void FloodAreas_r(node_t *node)
{
    if ((node->planenum == PLANENUM_LEAF || node->detail_separator) && (ClusterContents(node).native & Q2_CONTENTS_AREAPORTAL)) {
        // grab the func_areanode entity
        mapentity_t *entity = AreanodeEntityForLeaf(node);

        if (entity == nullptr)
        {
            logging::print("WARNING: areaportal contents in node, but no entity found {} -> {}\n",
                node->bounds.mins(),
                node->bounds.maxs());
            return;
        }

        // this node is part of an area portal;
        // if the current area has allready touched this
        // portal, we are done
        if (entity->portalareas[0] == map.c_areas || entity->portalareas[1] == map.c_areas)
            return;

        // note the current area as bounding the portal
        if (entity->portalareas[1]) {
            logging::print("WARNING: areaportal entity {} touches > 2 areas\n  Entity Bounds: {} -> {}\n",
                entity - map.entities.data(), entity->bounds.mins(),
                entity->bounds.maxs());
            return;
        }

        if (entity->portalareas[0])
            entity->portalareas[1] = map.c_areas;
        else
            entity->portalareas[0] = map.c_areas;

        return;
    }

    if (node->area)
        return; // already got it

    node->area = map.c_areas;

    // propagate area assignment to descendants if we're a cluster
    if (!(node->planenum == PLANENUM_LEAF)) {
        ApplyArea_r(node);
    }

    int32_t s;

    for (portal_t *p = node->portals; p; p = p->next[s]) {
        s = (p->nodes[1] == node);
#if 0
		if (p->nodes[!s]->occupied)
			continue;
#endif
        if (!Portal_EntityFlood(p, s))
            continue;

        FloodAreas_r(p->nodes[!s]);
    }
}

/*
=============
FindAreas_r

Just decend the tree, and for each node that hasn't had an
area set, flood fill out from there
=============
*/
static void FindAreas(node_t *node)
{
    auto leafs = FindOccupiedClusters(node);
    for (auto *leaf : leafs) {
        if (leaf->area)
            continue;

        // area portals are always only flooded into, never
        // out of
        if (ClusterContents(leaf).native & Q2_CONTENTS_AREAPORTAL)
            continue;

        map.c_areas++;
        FloodAreas_r(leaf);
    }
}

/*
=============
SetAreaPortalAreas_r

Just decend the tree, and for each node that hasn't had an
area set, flood fill out from there
=============
*/
static void SetAreaPortalAreas_r(node_t *node)
{
    if (node->planenum != PLANENUM_LEAF) {
        SetAreaPortalAreas_r(node->children[0]);
        SetAreaPortalAreas_r(node->children[1]);
        return;
    }

    if (node->contents.native != Q2_CONTENTS_AREAPORTAL)
        return;

    if (node->area)
        return; // already set

    // grab the func_areanode entity
    mapentity_t *entity = AreanodeEntityForLeaf(node);

    if (!entity)
    {
        logging::print("WARNING: areaportal missing for node: {} -> {}\n",
            node->bounds.mins(), node->bounds.maxs());
        return;
    }

    node->area = entity->portalareas[0];
    if (!entity->portalareas[1]) {
        logging::print("WARNING: areaportal entity {} doesn't touch two areas\n  Entity Bounds: {} -> {}\n",
            entity - map.entities.data(),
            entity->bounds.mins(), entity->bounds.maxs());
        return;
    }
}

/*
=============
EmitAreaPortals

=============
*/
void EmitAreaPortals(node_t *headnode)
{
    logging::print(logging::flag::PROGRESS, "---- {} ----\n", __func__);

    map.bsp.dareaportals.emplace_back();
    map.bsp.dareas.emplace_back();

    for (size_t i = 1; i <= map.c_areas; i++) {
        darea_t &area = map.bsp.dareas.emplace_back();
        area.firstareaportal = map.bsp.dareaportals.size();

        for (auto &e : map.entities) {

            if (!e.areaportalnum)
                continue;
            dareaportal_t dp = {};

            if (e.portalareas[0] == i) {
                dp.portalnum = e.areaportalnum;
                dp.otherarea = e.portalareas[1];
            } else if (e.portalareas[1] == i) {
                dp.portalnum = e.areaportalnum;
                dp.otherarea = e.portalareas[0];
            }

            size_t j = 0;

            for (; j < map.bsp.dareaportals.size(); j++)
            {
                if (map.bsp.dareaportals[j] == dp)
                    break;
            }

            if (j == map.bsp.dareaportals.size())
                map.bsp.dareaportals.push_back(dp);
        }

        area.numareaportals = map.bsp.dareaportals.size() - area.firstareaportal;
    }

    logging::print(logging::flag::STAT, "{:5} numareas\n", map.bsp.dareas.size());
    logging::print(logging::flag::STAT, "{:5} numareaportals\n", map.bsp.dareaportals.size());
}

/*
=============
FloodAreas

Mark each leaf with an area, bounded by CONTENTS_AREAPORTAL
=============
*/
void FloodAreas(mapentity_t *entity, node_t *headnode)
{
    logging::print(logging::flag::PROGRESS, "---- {} ----\n", __func__);
    FindAreas(headnode);
    SetAreaPortalAreas_r(headnode);
    logging::print(logging::flag::STAT, "{:5} areas\n", map.c_areas);
}

//==============================================================

/*
============
FindPortalSide

Finds a brush side to use for texturing the given portal
============
*/
static void FindPortalSide(portal_t *p)
{
    // decide which content change is strongest
    // solid > lava > water, etc
    contentflags_t viscontents = options.target_game->visible_contents(p->nodes[0]->contents, p->nodes[1]->contents);
    if (viscontents.is_empty(options.target_game))
        return;

    int planenum = p->onnode->planenum;
    side_t *bestside = nullptr;
    float bestdot = 0;

    for (int j = 0; j < 2; j++)
    {
        node_t *n = p->nodes[j];
        auto p1 = map.planes.at(p->onnode->planenum);

        // iterate the n->original_brushes vector in reverse order, so later brushes
        // in the map file order are prioritized
        for (auto it = n->original_brushes.rbegin(); it != n->original_brushes.rend(); ++it)
        {
            auto *brush = *it;
            if (!options.target_game->contents_contains(brush->contents, viscontents))
                continue;
            for (auto &side : brush->sides)
            {
                // fixme-brushbsp: port these
//                if (side.bevel)
//                    continue;
//                if (side.texinfo == TEXINFO_NODE)
//                    continue;		// non-visible
                if (side.planenum == planenum)
                {	// exact match
                    bestside = &side;
                    goto gotit;
                }
                // see how close the match is
                auto p2 = map.planes.at(side.planenum);
                float dot = qv::dot(p1.normal, p2.normal);
                if (dot > bestdot)
                {
                    bestdot = dot;
                    bestside = &side;
                }
            }
        }
    }

gotit:
    if (!bestside)
        logging::print("WARNING: side not found for portal\n");

    p->sidefound = true;
    p->side = bestside;
}

/*
===============
MarkVisibleSides_r

===============
*/
static void MarkVisibleSides_r(node_t *node)
{
    if (node->planenum != PLANENUM_LEAF)
    {
        MarkVisibleSides_r(node->children[0]);
        MarkVisibleSides_r(node->children[1]);
        return;
    }

    // empty leafs are never boundary leafs
    if (node->contents.is_empty(options.target_game))
        return;

    // see if there is a visible face
    int s;
    for (portal_t *p=node->portals ; p ; p = p->next[!s])
    {
        s = (p->nodes[0] == node);
        if (!p->onnode)
            continue;		// edge of world
        if (!p->sidefound)
            FindPortalSide(p);
        if (p->side)
            p->side->visible = true;
    }
}

/*
=============
MarkVisibleSides

=============
*/
void MarkVisibleSides(tree_t *tree, mapentity_t* entity)
{
    logging::print("--- {} ---\n", __func__);

    // clear all the visible flags
    for (auto &brush : entity->brushes) {
        for (auto &face : brush->sides) {
            face.visible = false;
        }
    }

    // set visible flags on the sides that are used by portals
    MarkVisibleSides_r (tree->headnode);
}
