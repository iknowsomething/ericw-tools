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

#include <qbsp/winding.hh>
#include <common/qvec.hh>

#include <atomic>
#include <list>
#include <optional>
#include <memory>

struct bspbrush_t;
struct node_t;
struct face_t;
class mapentity_t;
struct tree_t;

bool WindingIsTiny(const winding_t &w, double size = 0.2);
std::unique_ptr<bspbrush_t> BrushFromBounds(const aabb3d &bounds);
std::unique_ptr<tree_t> BrushBSP(std::vector<std::unique_ptr<bspbrush_t>> brushlist);

// compatibility version
std::unique_ptr<tree_t> BrushBSP(mapentity_t *entity);
