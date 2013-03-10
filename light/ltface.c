/*  Copyright (C) 1996-1997  Id Software, Inc.

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

#include <light/light.h>
#include <light/entities.h>

static const float scalecos = 0.5;
static const vec3_t bsp_origin = { 0, 0, 0 };

/* ======================================================================== */

typedef struct {
    vec3_t data[3];	/* permuted 3x3 matrix */
    int row[3];		/* row permutations */
    int col[3];		/* column permutations */
} pmatrix3_t;

/*
 * To do arbitrary transformation of texture coordinates to world
 * coordinates requires solving for three simultaneous equations. We
 * set up the LU decomposed form of the transform matrix here.
 */
#define ZERO_EPSILON (0.001)
static qboolean
PMatrix3_LU_Decompose(pmatrix3_t *matrix)
{
    int i, j, k, tmp;
    vec_t max;
    int max_r, max_c;

    /* Do gauss elimination */
    for (i = 0; i < 3; ++i) {
	max = 0;
	max_r = max_c = i;
	for (j = i; j < 3; ++j) {
	    for (k = i; k < 3; ++k) {
		if (fabs(matrix->data[j][k]) > max) {
		    max = fabs(matrix->data[j][k]);
		    max_r = j;
		    max_c = k;
		}
	    }
	}

	/* Check for parallel planes */
	if (max < ZERO_EPSILON)
	    return false;

	/* Swap rows/columns if necessary */
	if (max_r != i) {
	    for (j = 0; j < 3; ++j) {
		max = matrix->data[i][j];
		matrix->data[i][j] = matrix->data[max_r][j];
		matrix->data[max_r][j] = max;
	    }
	    tmp = matrix->row[i];
	    matrix->row[i] = matrix->row[max_r];
	    matrix->row[max_r] = tmp;
	}
	if (max_c != i) {
	    for (j = 0; j < 3; ++j) {
		max = matrix->data[j][i];
		matrix->data[j][i] = matrix->data[j][max_c];
		matrix->data[j][max_c] = max;
	    }
	    tmp = matrix->col[i];
	    matrix->col[i] = matrix->col[max_c];
	    matrix->col[max_c] = tmp;
	}

	/* Do pivot */
	for (j = i + 1; j < 3; ++j) {
	    matrix->data[j][i] /= matrix->data[i][i];
	    for (k = i + 1; k < 3; ++k)
		matrix->data[j][k] -= matrix->data[j][i] * matrix->data[i][k];
	}
    }

    return true;
}

static void
Solve3(const pmatrix3_t *matrix, const vec3_t rhs, vec3_t out)
{
    /* Use local short names just for readability (should optimize away) */
    const vec3_t *data = matrix->data;
    const int *r = matrix->row;
    const int *c = matrix->col;
    vec3_t tmp;

    /* forward-substitution */
    tmp[0] = rhs[r[0]];
    tmp[1] = rhs[r[1]] - data[1][0] * tmp[0];
    tmp[2] = rhs[r[2]] - data[2][0] * tmp[0] - data[2][1] * tmp[1];

    /* back-substitution */
    out[c[2]] = tmp[2] / data[2][2];
    out[c[1]] = (tmp[1] - data[1][2] * out[c[2]]) / data[1][1];
    out[c[0]] = (tmp[0] - data[0][1] * out[c[1]] - data[0][2] * out[c[2]])
	/ data[0][0];
}

/*
 * ============================================================================
 * SAMPLE POINT DETERMINATION
 * void SetupBlock (dface_t *f) Returns with surfpt[] set
 *
 * This is a little tricky because the lightmap covers more area than the face.
 * If done in the straightforward fashion, some of the sample points will be
 * inside walls or on the other side of walls, causing false shadows and light
 * bleeds.
 *
 * To solve this, I only consider a sample point valid if a line can be drawn
 * between it and the exact midpoint of the face.  If invalid, it is adjusted
 * towards the center until it is valid.
 *
 * FIXME: This doesn't completely work; I think what we really want is to move
 *        the light point to the nearst sample point that is on the polygon;
 * ============================================================================
 */

typedef struct {
    pmatrix3_t transform;
    const texinfo_t *texinfo;
    vec_t planedist;
} texorg_t;

typedef struct {
    vec3_t normal;
    vec_t dist;
} plane_t;

/* Allow space for 4x4 oversampling */
#define SINGLEMAP (18*18*4*4)

typedef struct {
    const modelinfo_t *modelinfo;
    plane_t plane;

    /* FIXME - to be removed... */
    vec_t exactmid[2];
    int texmins[2], texsize[2];

    int numpoints;
    vec3_t points[SINGLEMAP];
} lightsurf_t;

typedef struct {
    int style;
    lightsample_t samples[SINGLEMAP];
} lightmap_t;

/*
 * ================
 * CreateFaceTransform
 * Fills in the transform matrix for converting tex coord <-> world coord
 * ================
 */
static void
CreateFaceTransform(const dface_t *face, pmatrix3_t *transform)
{
    const dplane_t *plane;
    const texinfo_t *tex;
    int i;

    /* Prepare the transform matrix and init row/column permutations */
    plane = &dplanes[face->planenum];
    tex = &texinfo[face->texinfo];
    for (i = 0; i < 3; i++) {
	transform->data[0][i] = tex->vecs[0][i];
	transform->data[1][i] = tex->vecs[1][i];
	transform->data[2][i] = plane->normal[i];
	transform->row[i] = transform->col[i] = i;
    }
    if (face->side)
	VectorSubtract(vec3_origin, transform->data[2], transform->data[2]);

    /* Decompose the matrix. If we can't, texture axes are invalid. */
    if (!PMatrix3_LU_Decompose(transform)) {
	const vec_t *p = dvertexes[dedges[face->firstedge].v[0]].point;
	Error("Bad texture axes on face:\n"
	      "   face point at (%5.3f, %5.3f, %5.3f)\n", p[0], p[1], p[2]);
    }
}

static void
TexCoordToWorld(vec_t s, vec_t t, const texorg_t *texorg, vec3_t world)
{
    vec3_t rhs;

    rhs[0] = s - texorg->texinfo->vecs[0][3];
    rhs[1] = t - texorg->texinfo->vecs[1][3];
    rhs[2] = texorg->planedist + 1; /* one "unit" in front of surface */

    Solve3(&texorg->transform, rhs, world);
}

static void
WorldToTexCoord(const vec3_t world, const texinfo_t *tex, vec_t coord[2])
{
    int i;

    for (i = 0; i < 2; i++)
	coord[i] =
	    world[0] * tex->vecs[i][0] +
	    world[1] * tex->vecs[i][1] +
	    world[2] * tex->vecs[i][2] + tex->vecs[i][3];
}

/*
 * Functions to aid in calculation of polygon centroid
 */
static void
TriCentroid(const dvertex_t *v0, const dvertex_t *v1, const dvertex_t *v2,
	    vec3_t out)
{
    int i;

    for (i = 0; i < 3; i++)
	out[i] = (v0->point[i] + v1->point[i] + v2->point[i]) / 3.0;
}

static vec_t
TriArea(const dvertex_t *v0, const dvertex_t *v1, const dvertex_t *v2)
{
    int i;
    vec3_t edge0, edge1, cross;

    for (i =0; i < 3; i++) {
	edge0[i] = v1->point[i] - v0->point[i];
	edge1[i] = v2->point[i] - v0->point[i];
    }
    CrossProduct(edge0, edge1, cross);

    return VectorLength(cross) * 0.5;
}

static void
FaceCentroid(const dface_t *f, vec3_t out)
{
    int i, e;
    dvertex_t *v0, *v1, *v2;
    vec3_t centroid, poly_centroid;
    vec_t area, poly_area;

    VectorCopy(vec3_origin, poly_centroid);
    poly_area = 0;

    e = dsurfedges[f->firstedge];
    if (e >= 0)
	v0 = dvertexes + dedges[e].v[0];
    else
	v0 = dvertexes + dedges[-e].v[1];

    for (i = 1; i < f->numedges - 1; i++) {
	e = dsurfedges[f->firstedge + i];
	if (e >= 0) {
	    v1 = dvertexes + dedges[e].v[0];
	    v2 = dvertexes + dedges[e].v[1];
	} else {
	    v1 = dvertexes + dedges[-e].v[1];
	    v2 = dvertexes + dedges[-e].v[0];
	}

	area = TriArea(v0, v1, v2);
	poly_area += area;

	TriCentroid(v0, v1, v2, centroid);
	VectorMA(poly_centroid, area, centroid, poly_centroid);
    }

    VectorScale(poly_centroid, 1.0 / poly_area, out);
}

/*
 * ================
 * CalcFaceExtents
 * Fills in surf->texmins[], surf->texsize[] and sets surf->exactmid[]
 * ================
 */
static void
CalcFaceExtents(const dface_t *face, const vec3_t offset, lightsurf_t *surf)
{
    vec_t mins[2], maxs[2], texcoord[2];
    vec3_t world, centroid;
    int i, j, edge, vert;
    const dvertex_t *dvertex;
    const texinfo_t *tex;

    mins[0] = mins[1] = VECT_MAX;
    maxs[0] = maxs[1] = -VECT_MAX;
    tex = &texinfo[face->texinfo];

    for (i = 0; i < face->numedges; i++) {
	edge = dsurfedges[face->firstedge + i];
	vert = (edge >= 0) ? dedges[edge].v[0] : dedges[-edge].v[1];
	dvertex = &dvertexes[vert];

	VectorAdd(dvertex->point, offset, world);
	WorldToTexCoord(world, tex, texcoord);
	for (j = 0; j < 2; j++) {
	    if (texcoord[j] < mins[j])
		mins[j] = texcoord[j];
	    if (texcoord[j] > maxs[j])
		maxs[j] = texcoord[j];
	}
    }

    FaceCentroid(face, centroid);

    VectorAdd(centroid, offset, world);
    WorldToTexCoord(world, tex, surf->exactmid);
    for (i = 0; i < 2; i++) {
	mins[i] = floor(mins[i] / 16);
	maxs[i] = ceil(maxs[i] / 16);
	surf->texmins[i] = mins[i];
	surf->texsize[i] = maxs[i] - mins[i];
	if (surf->texsize[i] > 17)
	    Error("Bad surface extents");
    }
}

/*
 * =================
 * CalcPoints
 * For each texture aligned grid point, back project onto the plane
 * to get the world xyz value of the sample point
 * =================
 */
static void
CalcPoints(const dmodel_t *model, const texorg_t *texorg, lightsurf_t *surf)
{
    int i;
    int s, t;
    int w, h, step;
    vec_t starts, startt, us, ut;
    vec_t *point;
    vec_t mids, midt;
    vec3_t facemid, move;

    /* fill in surforg                                         */
    /* the points are biased towards the center of the surface */
    /* to help avoid edge cases just inside walls              */

    point = surf->points[0];
    mids = surf->exactmid[0];
    midt = surf->exactmid[1];

    TexCoordToWorld(mids, midt, texorg, facemid);

    h = (surf->texsize[1] + 1) * oversample;
    w = (surf->texsize[0] + 1) * oversample;
    starts = (surf->texmins[0] - 0.5 + (0.5 / oversample)) * 16;
    startt = (surf->texmins[1] - 0.5 + (0.5 / oversample)) * 16;
    step = 16 / oversample;

    surf->numpoints = w * h;
    for (t = 0; t < h; t++) {
	for (s = 0; s < w; s++, point += 3) {
	    us = starts + s * step;
	    ut = startt + t * step;

	    /* if a line can be traced from surf to facemid, point is good */
	    for (i = 0; i < 6; i++) {
		TexCoordToWorld(us, ut, texorg, point);

		if (TestLineModel(model, facemid, point))
		    break;	/* got it */
		if (i & 1) {	// i is odd
		    if (us > mids) {
			us -= 8;
			if (us < mids)
			    us = mids;
		    } else {
			us += 8;
			if (us > mids)
			    us = mids;
		    }
		} else {
		    if (ut > midt) {
			ut -= 8;
			if (ut < midt)
			    ut = midt;
		    } else {
			ut += 8;
			if (ut > midt)
			    ut = midt;
		    }
		}

		/* move surf 8 pixels towards the center */
		VectorSubtract(facemid, point, move);
		VectorNormalize(move);
		VectorMA(point, 8, move, point);
	    }
	}
    }
}

static void
Lightsurf_Init(const modelinfo_t *modelinfo, const dface_t *face,
	       lightsurf_t *lightsurf)
{
    plane_t *plane;
    vec3_t planepoint;
    texorg_t texorg;

    memset(lightsurf, 0, sizeof(*lightsurf));
    lightsurf->modelinfo = modelinfo;

    /* Set up the plane, including model offset */
    plane = &lightsurf->plane;
    VectorCopy(dplanes[face->planenum].normal, plane->normal);
    plane->dist = dplanes[face->planenum].dist;
    VectorScale(plane->normal, plane->dist, planepoint);
    VectorAdd(planepoint, modelinfo->offset, planepoint);
    plane->dist = DotProduct(plane->normal, planepoint);
    if (face->side) {
	VectorSubtract(vec3_origin, plane->normal, plane->normal);
	plane->dist = -plane->dist;
    }

    /* Set up the texorg for coordinate transformation */
    CreateFaceTransform(face, &texorg.transform);
    texorg.texinfo = &texinfo[face->texinfo];
    texorg.planedist = plane->dist;

    /* Set up the surface points */
    CalcFaceExtents(face, modelinfo->offset, lightsurf);
    CalcPoints(modelinfo->model, &texorg, lightsurf);
}

static void
Lightmaps_Init(lightmap_t *lightmaps)
{
    int i;

    memset(lightmaps, 0, sizeof(lightmap_t) * MAXLIGHTMAPS);
    for (i = 0; i < MAXLIGHTMAPS; i++)
	lightmaps[i].style = 255;
}


/*
 * ============================================================================
 * FACE LIGHTING
 * ============================================================================
 */

static vec_t
GetLightValue(const lightsample_t *light, const entity_t *entity, vec_t dist)
{
    vec_t value;

    if (entity->formula == LF_INFINITE || entity->formula == LF_LOCALMIN)
	return light->light;

    value = scaledist * entity->atten * dist;
    switch (entity->formula) {
    case LF_INVERSE:
	return light->light / (value / LF_SCALE);
    case LF_INVERSE2A:
	value += LF_SCALE;
	/* Fall through */
    case LF_INVERSE2:
	return light->light / ((value * value) / (LF_SCALE * LF_SCALE));
    case LF_LINEAR:
	if (light->light > 0)
	    return (light->light - value > 0) ? light->light - value : 0;
	else
	    return (light->light + value < 0) ? light->light + value : 0;
    default:
	Error("Internal error: unknown light formula");
    }
}


/*
 * ================
 * SingleLightFace
 * ================
 */
static void
SingleLightFace(const entity_t *entity, const lightsample_t *light,
		const lightsurf_t *lightsurf, lightmap_t *lightmaps)
{
    const modelinfo_t *modelinfo = lightsurf->modelinfo;
    const plane_t *plane = &lightsurf->plane;
    vec_t dist;
    vec_t angle, spotscale;
    vec_t add;
    qboolean newmap, hit;
    int i, mapnum;
    const vec_t *surfpoint;
    lightsample_t *sample;
    lightmap_t newlightmap;

    dist = DotProduct(entity->origin, plane->normal) - plane->dist;

    /* don't bother with lights behind the surface */
    if (dist < 0)
	return;

    /* don't bother with light too far away */
    if (dist > entity->fadedist)
	return;

    /*
     * Find the lightmap with matching style
     */
    newmap = true;
    for (mapnum = 0; mapnum < MAXLIGHTMAPS; mapnum++) {
	if (lightmaps[mapnum].style == 255)
	    break;
	if (lightmaps[mapnum].style == entity->style) {
	    newmap = false;
	    sample = lightmaps[mapnum].samples;
	    break;
	}
    }
    if (newmap) {
	memset(&newlightmap, 0, sizeof(newlightmap));
	newlightmap.style = entity->style;
	sample = newlightmap.samples;
    }

    /*
     * Check it for real
     */
    hit = false;
    surfpoint = lightsurf->points[0];
    for (i = 0; i < lightsurf->numpoints; i++, sample++, surfpoint += 3) {
	vec3_t ray;

	VectorSubtract(entity->origin, surfpoint, ray);
	dist = VectorLength(ray);

	/* Quick distance check first */
	if (dist > entity->fadedist)
	    continue;

	/* Check spotlight cone */
	VectorScale(ray, 1.0 / dist, ray);
	angle = DotProduct(ray, plane->normal);
	spotscale = 1;
	if (entity->spotlight) {
	    vec_t falloff = DotProduct(entity->spotvec, ray);
	    if (falloff > entity->spotfalloff)
		continue;
	    if (falloff > entity->spotfalloff2) {
		/* Interpolate between the two spotlight falloffs */
		spotscale = falloff - entity->spotfalloff2;
		spotscale /= entity->spotfalloff - entity->spotfalloff2;
		spotscale = 1.0 - spotscale;
	    }
	}

	/* Test for line of sight */
	if (!TestLine(entity->origin, surfpoint))
	    continue;
	if (modelinfo->shadowself)
	    if (!TestLineModel(modelinfo->model, entity->origin, surfpoint))
		continue;

	angle = (1.0 - scalecos) + scalecos * angle;
	add = GetLightValue(light, entity, dist) * angle * spotscale;
	sample->light += add;
	if (colored)
	    VectorMA(sample->color, add / 255.0f, light->color, sample->color);

	/* Check if we really hit, ignore tiny lights */
	if (newmap && sample->light >= 1)
	    hit = true;
    }

    if (newmap && hit) {
	if (mapnum == MAXLIGHTMAPS) {
	    logprint("WARNING: Too many light styles on a face\n"
		     "   lightmap point near (%s)\n"
		     "   entity->origin (%s)\n",
		     VecStr(lightsurf->points[0]), VecStr(entity->origin));
	    return;
	}

	/* the new lightmap has some real data now */
	memcpy(&lightmaps[mapnum], &newlightmap, sizeof(newlightmap));
    }
}

/*
 * =============
 * SkyLightFace
 * =============
 */
static void
SkyLightFace(const lightsample_t *light, const lightsurf_t *lightsurf,
	     lightmap_t *lightmaps)
{
    const modelinfo_t *modelinfo = lightsurf->modelinfo;
    const plane_t *plane = &lightsurf->plane;
    const vec_t *surfpoint;
    int i, mapnum;
    vec3_t incoming;
    vec_t angle;
    lightsample_t *sample;

    /* Don't bother if surface facing away from sun */
    if (DotProduct(sunvec, plane->normal) < -ANGLE_EPSILON)
	return;

    /* if sunlight is set, use a style 0 light map */
    for (mapnum = 0; mapnum < MAXLIGHTMAPS; mapnum++) {
	if (lightmaps[mapnum].style == 0)
	    break;
	if (lightmaps[mapnum].style == 255) {
	    lightmaps[mapnum].style = 0;
	    break;
	}
    }
    if (mapnum == MAXLIGHTMAPS)
	return; /* oh well, too many lightmaps... */

    VectorCopy(sunvec, incoming);
    VectorNormalize(incoming);
    angle = DotProduct(incoming, plane->normal);
    angle = (1.0 - scalecos) + scalecos * angle;

    /* Check each point... */
    sample = lightmaps[mapnum].samples;
    surfpoint = lightsurf->points[0];
    for (i = 0; i < lightsurf->numpoints; i++, sample++, surfpoint += 3) {
	vec3_t skypoint;
	if (!TestSky(surfpoint, sunvec, skypoint))
	    continue;
	if (modelinfo->shadowself)
	    if (!TestLineModel(modelinfo->model, surfpoint, skypoint))
		continue;
	sample->light += angle * light->light;
	if (colored)
	    VectorMA(sample->color, angle * light->light / 255.0f, light->color,
		     sample->color);
    }
}

/*
 * ============
 * FixMinlight
 * ============
 */
static void
FixMinlight(const lightsample_t *minlight, const lightsurf_t *lightsurf,
	    lightmap_t *lightmaps)
{
    const modelinfo_t *modelinfo = lightsurf->modelinfo;
    int mapnum, i, j, k;
    lightsample_t *sample;
    const entity_t *entity;
    const vec_t *surfpoint;

    /* Find a style 0 lightmap */
    for (mapnum = 0; mapnum < MAXLIGHTMAPS; mapnum++) {
	if (lightmaps[mapnum].style == 0)
	    break;
	if (lightmaps[mapnum].style == 255) {
	    lightmaps[mapnum].style = 0;
	    break;
	}
    }
    if (mapnum == MAXLIGHTMAPS)
	return; /* oh well... FIXME - should we warn? */

    sample = lightmaps[mapnum].samples;
    for (i = 0; i < lightsurf->numpoints; i++, sample++) {
	if (sample->light < minlight->light)
	    sample->light = minlight->light;
	if (colored) {
	    for (j = 0; j < 3; j++) {
		vec_t lightval = minlight->light * minlight->color[j] / 255.0f;
		if (sample->color[j] < lightval)
		    sample->color[j] = lightval;
	    }
	}
    }

    /* Cast rays for local minlight entities */
    for (i = 0, entity = entities; i < num_entities; i++, entity++) {
	if (entity->formula != LF_LOCALMIN)
	    continue;

	sample = lightmaps[mapnum].samples;
	surfpoint = lightsurf->points[0];
	for (j = 0; j < lightsurf->numpoints; j++, sample++, surfpoint += 3) {
	    qboolean trace = false;
	    if (sample->light < entity->light.light) {
		trace = TestLine(entity->origin, surfpoint);
		if (!trace)
		    continue;
		if (modelinfo->shadowself) {
		    trace = TestLineModel(modelinfo->model, entity->origin, surfpoint);
		    if (!trace)
			continue;
		}
		sample->light = entity->light.light;
	    }
	    if (!colored)
		continue;
	    for (k = 0; k < 3; k++) {
		if (sample->color[k] < entity->light.color[k]) {
		    if (!trace) {
			trace = TestLine(entity->origin, surfpoint);
			if (!trace)
			    break;
			if (modelinfo->shadowself) {
			    trace = TestLineModel(modelinfo->model,
						  entity->origin, surfpoint);
			    if (!trace)
				break;
			}
		    }
		    sample->color[k] = entity->light.color[k];
		}
	    }
	}
    }
}


/*
 * PositiveLight zeros negative light components.
 * NegativeLight zeros positive light components.
 */
static qboolean
PositiveLight(int light, const vec3_t color, lightsample_t *out)
{
    int i;
    qboolean positive = false;

    if (light > 0) {
	out->light = light;
	positive = true;
	for (i = 0; i < 3; i++)
	    if (color[i] > 0)
		out->color[i] = color[i];
	    else
		out->color[i] = 0;
    } else if (light < 0) {
	out->light = -light;
	for (i = 0; i < 3; i++)
	    if (color[i] < 0) {
		out->color[i] = -color[i];
		positive = true;
	    } else {
		out->color[i] = 0;
	    }
    }

    return positive;
}

static qboolean
NegativeLight(int light, const vec3_t color, lightsample_t *out)
{
    int i;
    qboolean negative = false;

    if (light < 0) {
	out->light = light;
	negative = true;
	for (i = 0; i < 3; i++)
	    if (color[i] > 0)
		out->color[i] = color[i];
	    else
		out->color[i] = 0;
    } else if (light > 0) {
	out->light = -light;
	for (i = 0; i < 3; i++)
	    if (color[i] < 0) {
		out->color[i] = -color[i];
		negative = true;
	    } else {
		out->color[i] = 0;
	    }
    }

    return negative;
}

static void
WriteLightmaps(dface_t *face, const lightsurf_t *lightsurf,
	       const lightmap_t *lightmaps)
{
    int numstyles, size, mapnum, width, s, t, i, j;
    const lightsample_t *sample;
    vec_t light;
    vec3_t color;
    byte *out, *lit;

    numstyles = 0;
    for (mapnum = 0; mapnum < MAXLIGHTMAPS; mapnum++) {
	face->styles[mapnum] = lightmaps[mapnum].style;
	if (lightmaps[mapnum].style != 255)
	    numstyles++;
    }
    if (!numstyles)
	return;

    size = (lightsurf->texsize[0] + 1) * (lightsurf->texsize[1] + 1);
    GetFileSpace(&out, &lit, size * numstyles);
    face->lightofs = out - filebase;

    width = (lightsurf->texsize[0] + 1) * oversample;
    for (mapnum = 0; mapnum < MAXLIGHTMAPS; mapnum++) {
	if (lightmaps[mapnum].style == 255)
	    break;

	sample = lightmaps[mapnum].samples;
	for (t = 0; t <= lightsurf->texsize[1]; t++) {
	    for (s = 0; s <= lightsurf->texsize[0]; s++) {

		/* Take the average of any oversampling */
		light = 0;
		VectorCopy(vec3_origin, color);
		for (i = 0; i < oversample; i++) {
		    for (j = 0; j < oversample; j++) {
			light += sample->light;
			if (colored)
			    VectorAdd(color, sample->color, color);
			sample++;
		    }
		    sample += width - oversample;
		}
		light /= oversample * oversample;
		VectorScale(color, 1.0 / oversample / oversample, color);

		/* Scale and clamp any out-of-range samples */
		light *= rangescale;
		if (colored) {
		    vec_t max = 0;
		    VectorScale(color, rangescale, color);
		    for (i = 0; i < 3; i++)
			if (color[i] > max)
			    max = color[i];
		    if (max > 255)
			VectorScale(color, 255.0f / max, color);
		}
		if (light > 255)
		    light = 255;
		else if (light < 0)
		    light = 0;

		if (colored) {
		    *lit++ = color[0];
		    *lit++ = color[1];
		    *lit++ = color[2];
		}
		*out++ = light;
		sample -= width * oversample - oversample;
	    }
	    sample += width * oversample - width;
	}
    }
}

/*
 * ============
 * LightFace
 * ============
 */
void
LightFace(dface_t *face, const modelinfo_t *modelinfo)
{
    int i, j, k;
    lightsample_t light;
    const entity_t *entity;
    lightsample_t *sample;
    lightsurf_t lightsurf;
    lightmap_t lightmaps[MAXLIGHTMAPS];

    /* some surfaces don't need lightmaps */
    face->lightofs = -1;
    for (i = 0; i < MAXLIGHTMAPS; i++)
	face->styles[i] = 255;
    if (texinfo[face->texinfo].flags & TEX_SPECIAL)
	return;

    Lightsurf_Init(modelinfo, face, &lightsurf);
    Lightmaps_Init(lightmaps);

    /* Under normal circumstances, the lighting procedure is:
     * - cast all light entities
     * - cast sky lighting
     * - do minlighting.
     *
     * However, if nominlimit is enabled then we need to do the following:
     * - cast _positive_ lights
     * - cast _positive_ skylight (if any)
     * - do minlighting
     * - cast _negative_ lights
     * - cast _negative_ sky light (if any)
     */

    if (nominlimit) {
	/* cast only positive lights */
	for (i = 0, entity = entities; i < num_entities; i++, entity++) {
	    if (entity->formula == LF_LOCALMIN)
		continue;
	    if (PositiveLight(entity->light.light, entity->light.color, &light))
		SingleLightFace(entity, &light, &lightsurf, lightmaps);
	}
	/* cast positive sky light */
	if (PositiveLight(sunlight, sunlight_color, &light))
	    SkyLightFace(&light, &lightsurf, lightmaps);
    } else {
	/* (!nominlimit) => cast all lights */
	for (i = 0, entity = entities; i < num_entities; i++, entity++) {
	    if (entity->formula == LF_LOCALMIN)
		continue;
	    if (entity->light.light)
		SingleLightFace(entity, &entity->light, &lightsurf, lightmaps);
	}
	/* cast sky light */
	if (sunlight) {
	    light.light = sunlight;
	    VectorCopy(sunlight_color, light.color);
	    SkyLightFace(&light, &lightsurf, lightmaps);
	}
    }

    /* Minimum lighting - Use the greater of global or model minlight. */
    if (modelinfo->minlight.light > worldminlight) {
	FixMinlight(&modelinfo->minlight, &lightsurf, lightmaps);
    } else {
	light.light = worldminlight;
	VectorCopy(minlight_color, light.color);
	FixMinlight(&light, &lightsurf, lightmaps);
    }

    if (nominlimit) {
	/* cast only negative lights */
	for (i = 0, entity = entities; i < num_entities; i++, entity++) {
	    if (entity->formula == LF_LOCALMIN)
		continue;
	    if (NegativeLight(entity->light.light, entity->light.color, &light))
		SingleLightFace(entity, &light, &lightsurf, lightmaps);
	}
	/* cast negative sky light */
	if (NegativeLight(sunlight, sunlight_color, &light))
	    SkyLightFace(&light, &lightsurf, lightmaps);

	/* Fix any negative values */
	for (i = 0; i < MAXLIGHTMAPS; i++) {
	    if (lightmaps[i].style == 255)
		break;
	    sample = lightmaps[i].samples;
	    for (j = 0; j < lightsurf.numpoints; j++, sample++) {
		if (sample->light < 0)
		    sample->light = 0;
		if (colored) {
		    for (k = 0; k < 3; k++) {
			if (sample->color[k] < 0) {
			    sample->color[k] = 0;
			}
		    }
		}
	    }
	}
    }

    WriteLightmaps(face, &lightsurf, lightmaps);
}
