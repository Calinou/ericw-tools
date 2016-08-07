/*  Copyright (C) 2016 Eric Wasylishen
 
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

#ifndef __LIGHT_TRACE_EMBREE_H__
#define __LIGHT_TRACE_EMBREE_H__

#include <common/cmdlib.h>
#include <common/mathlib.h>
#include <common/bspfile.h>
#include <common/log.h>
#include <common/threads.h>
#include <common/polylib.h>

void Embree_TraceInit(const bsp2_t *bsp);
qboolean Embree_TestSky(const vec3_t start, const vec3_t dirn, const dmodel_t *self);
qboolean Embree_TestLight(const vec3_t start, const vec3_t stop, const dmodel_t *self);
hittype_t Embree_DirtTrace(const vec3_t start, const vec3_t dirn, vec_t dist, const dmodel_t *self, vec_t *hitdist_out, plane_t *hitplane_out, const bsp2_dface_t **face_out);
bool Embree_IntersectSingleModel(const vec3_t start, const vec3_t dir, vec_t dist, const dmodel_t *self, vec_t *hitdist_out);

raystream_t *Embree_MakeRayStream(int maxrays);

#endif /* __LIGHT_TRACE_EMBREE_H__ */