/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_map.c

#include "tr_local.h"

#define JSON_IMPLEMENTATION
#include "json.h"
#undef JSON_IMPLEMENTATION

#include "tr_cache.h"
#include "tr_weather.h"
#include <vector>
#include <cmath>

/*

Loads and prepares a map file for scene rendering.

A single entry point:

void RE_LoadWorldMap( const char *name );

*/

static	world_t		s_worldData;
static	byte		*fileBase;

//===============================================================================

static void HSVtoRGB( float h, float s, float v, float rgb[3] )
{
	int i;
	float f;
	float p, q, t;

	h *= 5;

	i = floor( h );
	f = h - i;

	p = v * ( 1 - s );
	q = v * ( 1 - s * f );
	t = v * ( 1 - s * ( 1 - f ) );

	switch ( i )
	{
	case 0:
		rgb[0] = v;
		rgb[1] = t;
		rgb[2] = p;
		break;
	case 1:
		rgb[0] = q;
		rgb[1] = v;
		rgb[2] = p;
		break;
	case 2:
		rgb[0] = p;
		rgb[1] = v;
		rgb[2] = t;
		break;
	case 3:
		rgb[0] = p;
		rgb[1] = q;
		rgb[2] = v;
		break;
	case 4:
		rgb[0] = t;
		rgb[1] = p;
		rgb[2] = v;
		break;
	case 5:
		rgb[0] = v;
		rgb[1] = p;
		rgb[2] = q;
		break;
	}
}

/*
===============
R_ColorShiftLightingBytes

===============
*/
static	void R_ColorShiftLightingBytes( byte in[4], byte out[4] ) {
	int		shift, r, g, b;

	// shift the color data based on overbright range
	shift = Q_max( 0, r_mapOverBrightBits->integer - tr.overbrightBits );

	// shift the data based on overbright range
	r = in[0] << shift;
	g = in[1] << shift;
	b = in[2] << shift;
	
	// normalize by color instead of saturating to white
	if ( ( r | g | b ) > 255 ) {
		int		max;

		max = r > g ? r : g;
		max = max > b ? max : b;
		r = r * 255 / max;
		g = g * 255 / max;
		b = b * 255 / max;
	}

	out[0] = r;
	out[1] = g;
	out[2] = b;
	out[3] = in[3];
}


/*
===============
R_ColorShiftLightingFloats

===============
*/
static void R_ColorShiftLightingFloats(float in[4], float out[4], float scale )
{
	float r, g, b;

	scale *= pow(2.0f, r_mapOverBrightBits->integer - tr.overbrightBits);

	r = in[0] * scale;
	g = in[1] * scale;
	b = in[2] * scale;

	if (!glRefConfig.floatLightmap) 
	{
		if (r > 1.0f || g > 1.0f || b > 1.0f)
		{
			float high = Q_max(Q_max(r, g), b);

			r /= high;
			g /= high;
			b /= high;
		}
	}
	
	out[0] = r;
	out[1] = g;
	out[2] = b;
	out[3] = in[3];
}


// Modified from http://graphicrants.blogspot.jp/2009/04/rgbm-color-encoding.html
void ColorToRGBM(const vec3_t color, unsigned char rgbm[4])
{
	vec3_t          sample;
	float			maxComponent;

	VectorCopy(color, sample);

	maxComponent = MAX(sample[0], sample[1]);
	maxComponent = MAX(maxComponent, sample[2]);
	maxComponent = CLAMP(maxComponent, 1.0f/255.0f, 1.0f);

	rgbm[3] = (unsigned char) ceil(maxComponent * 255.0f);
	maxComponent = 255.0f / rgbm[3];

	VectorScale(sample, maxComponent, sample);

	rgbm[0] = (unsigned char) (sample[0] * 255);
	rgbm[1] = (unsigned char) (sample[1] * 255);
	rgbm[2] = (unsigned char) (sample[2] * 255);
}

void ColorToRGBA16F(const vec3_t color, unsigned short rgba16f[4])
{
	rgba16f[0] = FloatToHalf(color[0]);
	rgba16f[1] = FloatToHalf(color[1]);
	rgba16f[2] = FloatToHalf(color[2]);
	rgba16f[3] = FloatToHalf(1.0f);
}


/*
===============
R_LoadLightmaps

===============
*/
#define	DEFAULT_LIGHTMAP_SIZE	128
#define MAX_LIGHTMAP_PAGES 2
static	void R_LoadLightmaps( world_t *worldData, lump_t *l, lump_t *surfs ) {
	byte		*buf, *buf_p;
	dsurface_t  *surf;
	int			len;
	byte		*image;
	int			imageSize;
	int			i, j, numLightmaps, textureInternalFormat = 0;
	float maxIntensity = 0;
	double sumIntensity = 0;

	len = l->filelen;
	if ( !len ) {
		return;
	}
	buf = fileBase + l->fileofs;

	// we are about to upload textures
	R_IssuePendingRenderCommands();

	tr.lightmapSize = DEFAULT_LIGHTMAP_SIZE;
	numLightmaps = len / (tr.lightmapSize * tr.lightmapSize * 3);

	// check for deluxe mapping
	if (numLightmaps <= 1)
	{
		tr.worldDeluxeMapping = qfalse;
	}
	else
	{
		tr.worldDeluxeMapping = qtrue;

		// Check that none of the deluxe maps are referenced by any of the map surfaces.
		for( i = 0, surf = (dsurface_t *)(fileBase + surfs->fileofs);
			tr.worldDeluxeMapping && i < surfs->filelen / sizeof(dsurface_t);
			i++, surf++ ) {
			for ( int j = 0; j < MAXLIGHTMAPS; j++ )
			{
				int lightmapNum = LittleLong( surf->lightmapNum[j] );

				if ( lightmapNum >= 0 && (lightmapNum & 1) != 0 ) {
					tr.worldDeluxeMapping = qfalse;
					break;
				}
			}
		}
	}

	imageSize = tr.lightmapSize * tr.lightmapSize * 4 * 2;
	image = (byte *)R_Malloc(imageSize, TAG_BSP, qfalse);

	if (tr.worldDeluxeMapping)
		numLightmaps >>= 1;

	if (r_mergeLightmaps->integer)
	{
		const int targetLightmapsPerX = (int)ceilf(sqrtf(numLightmaps));

		int lightmapsPerX = 1;
		while (lightmapsPerX < targetLightmapsPerX)
			lightmapsPerX *= 2;
		
		tr.lightmapsPerAtlasSide[0] = lightmapsPerX;
		tr.lightmapsPerAtlasSide[1] = (int)ceilf((float)numLightmaps / lightmapsPerX);


		tr.lightmapAtlasSize[0] = tr.lightmapsPerAtlasSide[0] * LIGHTMAP_WIDTH;
		tr.lightmapAtlasSize[1] = tr.lightmapsPerAtlasSide[1] * LIGHTMAP_HEIGHT;

		tr.numLightmaps = 1;
	}
	else
	{
		tr.numLightmaps = numLightmaps;
	}

	tr.lightmaps = (image_t **)R_Hunk_Alloc( tr.numLightmaps * sizeof(image_t *), qtrue );

	if (tr.worldDeluxeMapping)
	{
		tr.deluxemaps = (image_t **)R_Hunk_Alloc( tr.numLightmaps * sizeof(image_t *), qtrue );
	}

	if (glRefConfig.floatLightmap)
		textureInternalFormat = GL_RGBA16F;
	else
		textureInternalFormat = GL_RGBA8;

	if (r_mergeLightmaps->integer)
	{
		for (i = 0; i < tr.numLightmaps; i++)
		{
			tr.lightmaps[i] = R_CreateImage(
				va("_lightmapatlas%d", i),
				NULL,
				tr.lightmapAtlasSize[0],
				tr.lightmapAtlasSize[1],
				0,
				IMGTYPE_COLORALPHA,
				IMGFLAG_NOLIGHTSCALE | IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE,
				textureInternalFormat);

			if (tr.worldDeluxeMapping)
			{
				tr.deluxemaps[i] = R_CreateImage(
					va("_fatdeluxemap%d", i),
					NULL,
					tr.lightmapAtlasSize[0],
					tr.lightmapAtlasSize[1],
					8,
					IMGTYPE_DELUXE,
					IMGFLAG_NOLIGHTSCALE | IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE,
					0);

			}
		}
	}

	for(i = 0; i < numLightmaps; i++)
	{
		int xoff = 0, yoff = 0;
		int lightmapnum = i;
		// expand the 24 bit on-disk to 32 bit

		if (r_mergeLightmaps->integer)
		{
			xoff = (i % tr.lightmapsPerAtlasSide[0]) * tr.lightmapSize;
			yoff = (i / tr.lightmapsPerAtlasSide[0]) * tr.lightmapSize;
			lightmapnum = 0;
		}

		// if (tr.worldLightmapping)
		{
			char filename[MAX_QPATH];
			byte *hdrLightmap = NULL;
			float *hdrL = NULL;
			int lightmapWidth = tr.lightmapSize;
			int lightmapHeight = tr.lightmapSize;
			int bppc;

			// look for hdr lightmaps
			if (r_hdr->integer)
			{
				Com_sprintf( filename, sizeof( filename ), "maps/%s/lm_%04d.hdr", worldData->baseName, i * (tr.worldDeluxeMapping ? 2 : 1) );
				//ri.Printf(PRINT_ALL, "looking for %s\n", filename);
				R_LoadImage(filename, &hdrLightmap, &lightmapWidth, &lightmapHeight, &bppc);
				
				if (hdrLightmap)
				{
					hdrL = (float *)hdrLightmap;
					int newImageSize = lightmapWidth * lightmapHeight * 4 * 2;
					if (r_mergeLightmaps->integer && (lightmapWidth != tr.lightmapSize || lightmapHeight != tr.lightmapSize))
					{
						ri.Printf(PRINT_ALL, "Error loading %s: non %dx%d lightmaps require r_mergeLightmaps 0.\n", filename, tr.lightmapSize, tr.lightmapSize);
						R_Free(hdrLightmap);
						hdrLightmap = NULL;
					}
					else if (newImageSize > imageSize)
					{
						R_Free(image);
						imageSize = newImageSize;
						image = (byte *)R_Malloc(imageSize, TAG_BSP, qfalse);
					}
				}
				if (!hdrLightmap)
				{
					lightmapWidth = tr.lightmapSize;
					lightmapHeight = tr.lightmapSize;
				}
			}

			if (hdrLightmap)
			{
				buf_p = hdrLightmap;

			}
			else
			{
				if (tr.worldDeluxeMapping)
					buf_p = buf + (i * 2) * tr.lightmapSize * tr.lightmapSize * 3;
				else
					buf_p = buf + i * tr.lightmapSize * tr.lightmapSize * 3;
			}

			for ( j = 0 ; j < lightmapWidth * lightmapHeight; j++ )
			{
				if (hdrLightmap)
				{
					vec4_t color;

					memcpy(color, &hdrL[j * 3], 12);

					color[3] = 1.0f;

					R_ColorShiftLightingFloats(color, color, 1.0f );

					//color[0] = sqrtf(color[0]);
					//color[1] = sqrtf(color[1]);
					//color[2] = sqrtf(color[2]);

					ColorToRGBA16F(color, (uint16_t *)(&image[j*8]));
				}
				else if (glRefConfig.floatLightmap)
				{
					vec4_t color;

					//hack: convert LDR lightmap to HDR one
					color[0] = MAX(buf_p[j*3+0], 0.499f);
					color[1] = MAX(buf_p[j*3+1], 0.499f);
					color[2] = MAX(buf_p[j*3+2], 0.499f);

					// if under an arbitrary value (say 12) grey it out
					// this prevents weird splotches in dimly lit areas
					if (color[0] + color[1] + color[2] < 12.0f)
					{
						float avg = (color[0] + color[1] + color[2]) * 0.3333f;
						color[0] = avg;
						color[1] = avg;
						color[2] = avg;
					}
					color[3] = 1.0f;

					R_ColorShiftLightingFloats(color, color, 1.0f/255.0f);

					ColorToRGBA16F(color, (unsigned short *)(&image[j*8]));
				}
				else
				{
					if ( r_lightmap->integer == 2 )
					{	// color code by intensity as development tool	(FIXME: check range)
						float r = buf_p[j*3+0];
						float g = buf_p[j*3+1];
						float b = buf_p[j*3+2];
						float intensity;
						float out[3] = {0.0, 0.0, 0.0};

						intensity = 0.33f * r + 0.685f * g + 0.063f * b;

						if ( intensity > 255 )
							intensity = 1.0f;
						else
							intensity /= 255.0f;

						if ( intensity > maxIntensity )
							maxIntensity = intensity;

						HSVtoRGB( intensity, 1.00, 0.50, out );

						image[j*4+0] = out[0] * 255;
						image[j*4+1] = out[1] * 255;
						image[j*4+2] = out[2] * 255;
						image[j*4+3] = 255;

						sumIntensity += intensity;
					}
					else
					{
						R_ColorShiftLightingBytes( &buf_p[j*3], &image[j*4] );
						image[j*4+3] = 255;
					}
				}
			}

			if (r_mergeLightmaps->integer)
				R_UpdateSubImage(
					tr.lightmaps[lightmapnum],
					image,
					xoff,
					yoff,
					lightmapWidth,
					lightmapHeight);
			else
				tr.lightmaps[i] = R_CreateImage(
					va("*lightmap%d", i),
					image,
					lightmapWidth,
					lightmapHeight,
					16,
					IMGTYPE_COLORALPHA,
					IMGFLAG_NOLIGHTSCALE |
					IMGFLAG_NO_COMPRESSION |
					IMGFLAG_CLAMPTOEDGE,
					0);

			//if (hdrLightmap)
				//R_Free(hdrLightmap);
		}

		if (tr.worldDeluxeMapping)
		{
			buf_p = buf + (i * 2 + 1) * tr.lightmapSize * tr.lightmapSize * 3;

			for ( j = 0 ; j < tr.lightmapSize * tr.lightmapSize; j++ ) {
				image[j*4+0] = buf_p[j*3+0];
				image[j*4+1] = buf_p[j*3+1];
				image[j*4+2] = buf_p[j*3+2];

				// make 0,0,0 into 127,127,127
				if ((image[j*4+0] == 0) && (image[j*4+1] == 0) && (image[j*4+2] == 0))
				{
					image[j*4+0] =
					image[j*4+1] =
					image[j*4+2] = 127;
				}

				image[j*4+3] = 255;
			}

			if (r_mergeLightmaps->integer)
			{
				R_UpdateSubImage(
					tr.deluxemaps[lightmapnum],
					image,
					xoff,
					yoff,
					tr.lightmapSize,
					tr.lightmapSize);
			}
			else
			{
				tr.deluxemaps[i] = R_CreateImage(
					va("*deluxemap%d", i),
					image,
					tr.lightmapSize,
					tr.lightmapSize,
					8,
					IMGTYPE_DELUXE,
					IMGFLAG_NOLIGHTSCALE |
					IMGFLAG_NO_COMPRESSION |
					IMGFLAG_CLAMPTOEDGE,
					0);
			}
		}
	}

	if ( r_lightmap->integer == 2 )	{
		ri.Printf( PRINT_ALL, "Brightest lightmap value: %d\n", ( int ) ( maxIntensity * 255 ) );
	}

	R_Free(image);
}


static float FatPackU(float input, int lightmapnum)
{
	if (lightmapnum < 0)
		return input;

	if (tr.worldDeluxeMapping)
		lightmapnum >>= 1;

	if (tr.lightmapAtlasSize[0] > 0)
	{
		const int lightmapXOffset = lightmapnum % tr.lightmapsPerAtlasSide[0];
		const float invLightmapSide = 1.0f / tr.lightmapsPerAtlasSide[0];

		return (lightmapXOffset * invLightmapSide) + (input * invLightmapSide);
	}

	return input;
}

static float FatPackV(float input, int lightmapnum)
{
	if (lightmapnum < 0)
		return input;

	if (tr.worldDeluxeMapping)
		lightmapnum >>= 1;

	if (tr.lightmapAtlasSize[1] > 0)
	{
		const int lightmapYOffset = lightmapnum / tr.lightmapsPerAtlasSide[0];
		const float invLightmapSide = 1.0f / tr.lightmapsPerAtlasSide[1];

		return (lightmapYOffset * invLightmapSide) + (input * invLightmapSide);
	}

	return input;
}


static int FatLightmap(int lightmapnum)
{
	if (lightmapnum < 0)
		return lightmapnum;

	if (tr.worldDeluxeMapping)
		lightmapnum >>= 1;

	if (tr.lightmapAtlasSize[0] > 0)
		return 0;
	
	return lightmapnum;
}

/*
=================
RE_SetWorldVisData

This is called by the clipmodel subsystem so we can share the 1.8 megs of
space in big maps...
=================
*/
void RE_SetWorldVisData( const byte *vis ) {
	tr.externalVisData = vis;
}


/*
=================
R_LoadVisibility
=================
*/
static void R_LoadVisibility( world_t *worldData, lump_t *l ) {
	int		len;
	byte	*buf;

	len = l->filelen;
	if ( !len ) {
		return;
	}
	buf = fileBase + l->fileofs;

	worldData->numClusters = LittleLong( ((int *)buf)[0] );
	worldData->clusterBytes = LittleLong( ((int *)buf)[1] );

	// CM_Load should have given us the vis data to share, so
	// we don't need to allocate another copy
	if ( tr.externalVisData ) {
		worldData->vis = tr.externalVisData;
	} else {
		byte	*dest;

		dest = (byte *)R_Hunk_Alloc( len - 8, qtrue);
		Com_Memcpy( dest, buf + 8, len - 8 );
		worldData->vis = dest;
	}
}

//===============================================================================


/*
===============
ShaderForShaderNum
===============
*/
static shader_t *ShaderForShaderNum( const world_t *worldData, int shaderNum, const int *lightmapNums, const byte *lightmapStyles, const byte *vertexStyles ) {
	shader_t	*shader;
	dshader_t	*dsh;
	const byte	*styles = lightmapStyles;

	int _shaderNum = LittleLong( shaderNum );
	if ( _shaderNum < 0 || _shaderNum >= worldData->numShaders ) {
		ri.Error( ERR_DROP, "ShaderForShaderNum: bad num %i", _shaderNum );
	}
	dsh = &worldData->shaders[ _shaderNum ];

	if ( lightmapNums[0] == LIGHTMAP_BY_VERTEX ) {
		styles = vertexStyles;
	}

	if ( r_vertexLight->integer ) {
		lightmapNums = lightmapsVertex;
		styles = vertexStyles;
	}

	if ( r_fullbright->integer ) {
		lightmapNums = lightmapsFullBright;
	}

	shader = R_FindShader( dsh->shader, lightmapNums, styles, qtrue );

	// if the shader had errors, just use default shader
	if ( shader->defaultShader ) {
		return tr.defaultShader;
	}

	return shader;
}

/*
===============
ParseFace
===============
*/
static void ParseFace( const world_t *worldData, dsurface_t *ds, drawVert_t *verts, float *hdrVertColors, msurface_t *surf, int *indexes  ) {
	int			i, j;
	srfBspSurface_t	*cv;
	glIndex_t  *tri;
	int			numVerts, numIndexes, badTriangles;
	int realLightmapNum[MAXLIGHTMAPS];

	for ( j = 0; j < MAXLIGHTMAPS; j++ )
	{
		realLightmapNum[j] = FatLightmap (LittleLong (ds->lightmapNum[j]));
	}

	surf->numSurfaceSprites = 0;
	surf->surfaceSprites = nullptr;

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader value
	surf->shader = ShaderForShaderNum( worldData, ds->shaderNum, realLightmapNum, ds->lightmapStyles, ds->vertexStyles);
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	numVerts = LittleLong(ds->numVerts);
	if (numVerts > MAX_FACE_POINTS) {
		ri.Printf( PRINT_WARNING, "WARNING: MAX_FACE_POINTS exceeded: %i\n", numVerts);
		numVerts = MAX_FACE_POINTS;
		surf->shader = tr.defaultShader;
	}

	numIndexes = LittleLong(ds->numIndexes);

	//cv = ri->Hunk_Alloc(sizeof(*cv), h_low);
	cv = (srfBspSurface_t *)surf->data;
	cv->surfaceType = SF_FACE;

	cv->numIndexes = numIndexes;
	cv->indexes = (glIndex_t *)R_Hunk_Alloc(numIndexes * sizeof(cv->indexes[0]), qtrue);

	cv->numVerts = numVerts;
	cv->verts = (srfVert_t *)R_Hunk_Alloc(numVerts * sizeof(cv->verts[0]), qtrue);

	// copy vertexes
	surf->cullinfo.type = CULLINFO_PLANE | CULLINFO_BOX;
	ClearBounds(surf->cullinfo.bounds[0], surf->cullinfo.bounds[1]);
	verts += LittleLong(ds->firstVert);
	for(i = 0; i < numVerts; i++)
	{
		vec4_t color;

		for(j = 0; j < 3; j++)
		{
			cv->verts[i].xyz[j] = LittleFloat(verts[i].xyz[j]);
			cv->verts[i].normal[j] = LittleFloat(verts[i].normal[j]);
		}

		AddPointToBounds(cv->verts[i].xyz, surf->cullinfo.bounds[0], surf->cullinfo.bounds[1]);

		for(j = 0; j < 2; j++)
		{
			cv->verts[i].st[j] = LittleFloat(verts[i].st[j]);
		}

		for ( j = 0; j < MAXLIGHTMAPS; j++ )
		{
			cv->verts[i].lightmap[j][0] = FatPackU(LittleFloat(verts[i].lightmap[j][0]), ds->lightmapNum[j]);
			cv->verts[i].lightmap[j][1] = FatPackV(LittleFloat(verts[i].lightmap[j][1]), ds->lightmapNum[j]);

			if (hdrVertColors)
			{
				color[0] = hdrVertColors[(ds->firstVert + i) * 3    ];
				color[1] = hdrVertColors[(ds->firstVert + i) * 3 + 1];
				color[2] = hdrVertColors[(ds->firstVert + i) * 3 + 2];
			}
			else
			{
				//hack: convert LDR vertex colors to HDR
				if (r_hdr->integer)
				{
					color[0] = MAX(verts[i].color[j][0], 0.499f);
					color[1] = MAX(verts[i].color[j][1], 0.499f);
					color[2] = MAX(verts[i].color[j][2], 0.499f);
				}
				else
				{
					color[0] = verts[i].color[j][0];
					color[1] = verts[i].color[j][1];
					color[2] = verts[i].color[j][2];
				}

			}
			color[3] = verts[i].color[j][3] / 255.0f;

			R_ColorShiftLightingFloats( color, cv->verts[i].vertexColors[j], 1.0f / 255.0f );
		}
	}

	// copy triangles
	badTriangles = 0;
	indexes += LittleLong(ds->firstIndex);
	for(i = 0, tri = cv->indexes; i < numIndexes; i += 3, tri += 3)
	{
		for(j = 0; j < 3; j++)
		{
			tri[j] = LittleLong(indexes[i + j]);

			if(tri[j] >= numVerts)
			{
				ri.Error(ERR_DROP, "Bad index in face surface");
			}
		}

		if ((tri[0] == tri[1]) || (tri[1] == tri[2]) || (tri[0] == tri[2]))
		{
			tri -= 3;
			badTriangles++;
		}
	}

	if (badTriangles)
	{
		ri.Printf(PRINT_WARNING, "Face has bad triangles, originally shader %s %d tris %d verts, now %d tris\n", surf->shader->name, numIndexes / 3, numVerts, numIndexes / 3 - badTriangles);
		cv->numIndexes -= badTriangles * 3;
	}

	// take the plane information from the lightmap vector
	for ( i = 0 ; i < 3 ; i++ ) {
		cv->cullPlane.normal[i] = LittleFloat( ds->lightmapVecs[2][i] );
	}
	cv->cullPlane.dist = DotProduct( cv->verts[0].xyz, cv->cullPlane.normal );
	SetPlaneSignbits( &cv->cullPlane );
	cv->cullPlane.type = PlaneTypeForNormal( cv->cullPlane.normal );
	surf->cullinfo.plane = cv->cullPlane;

	surf->data = (surfaceType_t *)cv;

	// Calculate tangent spaces
	{
		srfVert_t      *dv[3];

		for(i = 0, tri = cv->indexes; i < numIndexes; i += 3, tri += 3)
		{
			dv[0] = &cv->verts[tri[0]];
			dv[1] = &cv->verts[tri[1]];
			dv[2] = &cv->verts[tri[2]];

			R_CalcTangentVectors(dv);
		}
	}
}


/*
===============
ParseMesh
===============
*/
static void ParseMesh ( const world_t *worldData, dsurface_t *ds, drawVert_t *verts, float *hdrVertColors, msurface_t *surf ) {
	srfBspSurface_t	*grid;
	int				i, j;
	int				width, height, numPoints;
	srfVert_t points[MAX_PATCH_SIZE*MAX_PATCH_SIZE];
	vec3_t			bounds[2];
	vec3_t			tmpVec;
	static surfaceType_t	skipData = SF_SKIP;
	int realLightmapNum[MAXLIGHTMAPS];

	for ( j = 0; j < MAXLIGHTMAPS; j++ )
		realLightmapNum[j] = FatLightmap(LittleLong(ds->lightmapNum[j]));

	surf->numSurfaceSprites = 0;
	surf->surfaceSprites = nullptr;

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader value
	surf->shader = ShaderForShaderNum( worldData, ds->shaderNum, realLightmapNum, ds->lightmapStyles, ds->vertexStyles );
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	// we may have a nodraw surface, because they might still need to
	// be around for movement clipping
	if ( worldData->shaders[ LittleLong( ds->shaderNum ) ].surfaceFlags & SURF_NODRAW ) {
		surf->data = &skipData;
		return;
	}

	width = LittleLong( ds->patchWidth );
	height = LittleLong( ds->patchHeight );

	if(width < 0 || width > MAX_PATCH_SIZE || height < 0 || height > MAX_PATCH_SIZE)
		ri.Error(ERR_DROP, "ParseMesh: bad size");

	verts += LittleLong( ds->firstVert );
	numPoints = width * height;
	for(i = 0; i < numPoints; i++)
	{
		vec4_t color;

		for(j = 0; j < 3; j++)
		{
			points[i].xyz[j] = LittleFloat(verts[i].xyz[j]);
			points[i].normal[j] = LittleFloat(verts[i].normal[j]);
		}

		for(j = 0; j < 2; j++)
		{
			points[i].st[j] = LittleFloat(verts[i].st[j]);
		}

		for ( j = 0; j < MAXLIGHTMAPS; j++ )
		{
			points[i].lightmap[j][0] = FatPackU(LittleFloat(verts[i].lightmap[j][0]), ds->lightmapNum[j]);
			points[i].lightmap[j][1] = FatPackV(LittleFloat(verts[i].lightmap[j][1]), ds->lightmapNum[j]);

			if (hdrVertColors)
			{
				color[0] = hdrVertColors[(ds->firstVert + i) * 3    ];
				color[1] = hdrVertColors[(ds->firstVert + i) * 3 + 1];
				color[2] = hdrVertColors[(ds->firstVert + i) * 3 + 2];
			}
			else
			{
				//hack: convert LDR vertex colors to HDR
				if (r_hdr->integer)
				{
					color[0] = MAX(verts[i].color[j][0], 0.499f);
					color[1] = MAX(verts[i].color[j][1], 0.499f);
					color[2] = MAX(verts[i].color[j][2], 0.499f);
				}
				else
				{
					color[0] = verts[i].color[j][0];
					color[1] = verts[i].color[j][1];
					color[2] = verts[i].color[j][2];
				}
			}
			color[3] = verts[i].color[j][3] / 255.0f;

			R_ColorShiftLightingFloats( color, points[i].vertexColors[j], 1.0f / 255.0f );
		}
	}

	// pre-tesseleate
	grid = R_SubdividePatchToGrid( width, height, points );
	surf->data = (surfaceType_t *)grid;

	// copy the level of detail origin, which is the center
	// of the group of all curves that must subdivide the same
	// to avoid cracking
	for ( i = 0 ; i < 3 ; i++ ) {
		bounds[0][i] = LittleFloat( ds->lightmapVecs[0][i] );
		bounds[1][i] = LittleFloat( ds->lightmapVecs[1][i] );
	}
	VectorAdd( bounds[0], bounds[1], bounds[1] );
	VectorScale( bounds[1], 0.5f, grid->lodOrigin );
	VectorSubtract( bounds[0], grid->lodOrigin, tmpVec );
	grid->lodRadius = VectorLength( tmpVec );
}

/*
===============
ParseTriSurf
===============
*/
static void ParseTriSurf( const world_t *worldData, dsurface_t *ds, drawVert_t *verts, float *hdrVertColors, msurface_t *surf, int *indexes ) {
	srfBspSurface_t *cv;
	glIndex_t  *tri;
	int             i, j;
	int             numVerts, numIndexes, badTriangles;
	int realLightmapNum[MAXLIGHTMAPS];

	for (j = 0; j < MAXLIGHTMAPS; j++)
		realLightmapNum[j] = FatLightmap(LittleLong(ds->lightmapNum[j]));

	surf->numSurfaceSprites = 0;
	surf->surfaceSprites = nullptr;

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader
	surf->shader = ShaderForShaderNum(worldData, ds->shaderNum, realLightmapNum, ds->lightmapStyles, ds->vertexStyles);
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	numVerts = LittleLong(ds->numVerts);
	numIndexes = LittleLong(ds->numIndexes);

	//cv = ri->Hunk_Alloc(sizeof(*cv), h_low);
	cv = (srfBspSurface_t *)surf->data;
	cv->surfaceType = SF_TRIANGLES;

	cv->numIndexes = numIndexes;
	cv->indexes = (glIndex_t *)R_Hunk_Alloc(numIndexes * sizeof(cv->indexes[0]), qtrue);

	cv->numVerts = numVerts;
	cv->verts = (srfVert_t *)R_Hunk_Alloc(numVerts * sizeof(cv->verts[0]), qtrue);

	surf->data = (surfaceType_t *) cv;

	// copy vertexes
	surf->cullinfo.type = CULLINFO_BOX;
	ClearBounds(surf->cullinfo.bounds[0], surf->cullinfo.bounds[1]);
	verts += LittleLong(ds->firstVert);
	for(i = 0; i < numVerts; i++)
	{
		vec4_t color;

		for(j = 0; j < 3; j++)
		{
			cv->verts[i].xyz[j] = LittleFloat(verts[i].xyz[j]);
			cv->verts[i].normal[j] = LittleFloat(verts[i].normal[j]);
		}

		AddPointToBounds( cv->verts[i].xyz, surf->cullinfo.bounds[0], surf->cullinfo.bounds[1] );

		for(j = 0; j < 2; j++)
		{
			cv->verts[i].st[j] = LittleFloat(verts[i].st[j]);
		}

		for ( j = 0; j < MAXLIGHTMAPS; j++ )
		{
			cv->verts[i].lightmap[j][0] = FatPackU(LittleFloat(verts[i].lightmap[j][0]), ds->lightmapNum[j]);
			cv->verts[i].lightmap[j][1] = FatPackV(LittleFloat(verts[i].lightmap[j][1]), ds->lightmapNum[j]);

			if (hdrVertColors)
			{
				color[0] = hdrVertColors[(ds->firstVert + i) * 3    ];
				color[1] = hdrVertColors[(ds->firstVert + i) * 3 + 1];
				color[2] = hdrVertColors[(ds->firstVert + i) * 3 + 2];
			}
			else
			{
				//hack: convert LDR vertex colors to HDR
				if (r_hdr->integer)
				{
					color[0] = MAX(verts[i].color[j][0], 0.499f);
					color[1] = MAX(verts[i].color[j][1], 0.499f);
					color[2] = MAX(verts[i].color[j][2], 0.499f);
				}
				else
				{
					color[0] = verts[i].color[j][0];
					color[1] = verts[i].color[j][1];
					color[2] = verts[i].color[j][2];
				}
			}
			color[3] = verts[i].color[j][3] / 255.0f;

			R_ColorShiftLightingFloats( color, cv->verts[i].vertexColors[j], 1.0f / 255.0f );
		}
	}

	// copy triangles
	badTriangles = 0;
	indexes += LittleLong(ds->firstIndex);
	for(i = 0, tri = cv->indexes; i < numIndexes; i += 3, tri += 3)
	{
		for(j = 0; j < 3; j++)
		{
			tri[j] = LittleLong(indexes[i + j]);

			if(tri[j] >= numVerts)
			{
				ri.Error(ERR_DROP, "Bad index in face surface");
			}
		}

		if ((tri[0] == tri[1]) || (tri[1] == tri[2]) || (tri[0] == tri[2]))
		{
			tri -= 3;
			badTriangles++;
		}
	}

	if (badTriangles)
	{
		ri.Printf(PRINT_WARNING, "Trisurf has bad triangles, originally shader %s %d tris %d verts, now %d tris\n", surf->shader->name, numIndexes / 3, numVerts, numIndexes / 3 - badTriangles);
		cv->numIndexes -= badTriangles * 3;
	}

	// Calculate tangent spaces
	{
		srfVert_t      *dv[3];

		for(i = 0, tri = cv->indexes; i < numIndexes; i += 3, tri += 3)
		{
			dv[0] = &cv->verts[tri[0]];
			dv[1] = &cv->verts[tri[1]];
			dv[2] = &cv->verts[tri[2]];

			R_CalcTangentVectors(dv);
		}
	}
}

/*
===============
ParseFlare
===============
*/
static void ParseFlare( const world_t *worldData, dsurface_t *ds, drawVert_t *verts, msurface_t *surf, int *indexes ) {
	srfFlare_t		*flare;
	int				i;

	surf->numSurfaceSprites = 0;
	surf->surfaceSprites = nullptr;

	// get fog volume
	surf->fogIndex = LittleLong( ds->fogNum ) + 1;

	// get shader
	surf->shader = ShaderForShaderNum( worldData, ds->shaderNum, lightmapsVertex, ds->lightmapStyles, ds->vertexStyles );
	if ( r_singleShader->integer && !surf->shader->isSky ) {
		surf->shader = tr.defaultShader;
	}

	//flare = ri->Hunk_Alloc( sizeof( *flare ), h_low );
	flare = (srfFlare_t *)surf->data;
	flare->surfaceType = SF_FLARE;

	surf->data = (surfaceType_t *)flare;

	for ( i = 0 ; i < 3 ; i++ ) {
		flare->origin[i] = LittleFloat( ds->lightmapOrigin[i] );
		flare->color[i] = LittleFloat( ds->lightmapVecs[0][i] );
		flare->normal[i] = LittleFloat( ds->lightmapVecs[2][i] );
	}
}


/*
=================
R_MergedWidthPoints

returns true if there are grid points merged on a width edge
=================
*/
int R_MergedWidthPoints(srfBspSurface_t *grid, int offset) {
	int i, j;

	for (i = 1; i < grid->width-1; i++) {
		for (j = i + 1; j < grid->width-1; j++) {
			if ( fabs(grid->verts[i + offset].xyz[0] - grid->verts[j + offset].xyz[0]) > .1) continue;
			if ( fabs(grid->verts[i + offset].xyz[1] - grid->verts[j + offset].xyz[1]) > .1) continue;
			if ( fabs(grid->verts[i + offset].xyz[2] - grid->verts[j + offset].xyz[2]) > .1) continue;
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
R_MergedHeightPoints

returns true if there are grid points merged on a height edge
=================
*/
int R_MergedHeightPoints(srfBspSurface_t *grid, int offset) {
	int i, j;

	for (i = 1; i < grid->height-1; i++) {
		for (j = i + 1; j < grid->height-1; j++) {
			if ( fabs(grid->verts[grid->width * i + offset].xyz[0] - grid->verts[grid->width * j + offset].xyz[0]) > .1) continue;
			if ( fabs(grid->verts[grid->width * i + offset].xyz[1] - grid->verts[grid->width * j + offset].xyz[1]) > .1) continue;
			if ( fabs(grid->verts[grid->width * i + offset].xyz[2] - grid->verts[grid->width * j + offset].xyz[2]) > .1) continue;
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
R_FixSharedVertexLodError_r

NOTE: never sync LoD through grid edges with merged points!

FIXME: write generalized version that also avoids cracks between a patch and one that meets half way?
=================
*/
void R_FixSharedVertexLodError_r( world_t *worldData, int start, srfBspSurface_t *grid1 ) {
	int j, k, l, m, n, offset1, offset2, touch;
	srfBspSurface_t *grid2;

	for ( j = start; j < worldData->numsurfaces; j++ ) {
		//
		grid2 = (srfBspSurface_t *) worldData->surfaces[j].data;
		// if this surface is not a grid
		if ( grid2->surfaceType != SF_GRID ) continue;
		// if the LOD errors are already fixed for this patch
		if ( grid2->lodFixed == 2 ) continue;
		// grids in the same LOD group should have the exact same lod radius
		if ( grid1->lodRadius != grid2->lodRadius ) continue;
		// grids in the same LOD group should have the exact same lod origin
		if ( grid1->lodOrigin[0] != grid2->lodOrigin[0] ) continue;
		if ( grid1->lodOrigin[1] != grid2->lodOrigin[1] ) continue;
		if ( grid1->lodOrigin[2] != grid2->lodOrigin[2] ) continue;
		//
		touch = qfalse;
		for (n = 0; n < 2; n++) {
			//
			if (n) offset1 = (grid1->height-1) * grid1->width;
			else offset1 = 0;
			if (R_MergedWidthPoints(grid1, offset1)) continue;
			for (k = 1; k < grid1->width-1; k++) {
				for (m = 0; m < 2; m++) {

					if (m) offset2 = (grid2->height-1) * grid2->width;
					else offset2 = 0;
					if (R_MergedWidthPoints(grid2, offset2)) continue;
					for ( l = 1; l < grid2->width-1; l++) {
					//
						if ( fabs(grid1->verts[k + offset1].xyz[0] - grid2->verts[l + offset2].xyz[0]) > .1) continue;
						if ( fabs(grid1->verts[k + offset1].xyz[1] - grid2->verts[l + offset2].xyz[1]) > .1) continue;
						if ( fabs(grid1->verts[k + offset1].xyz[2] - grid2->verts[l + offset2].xyz[2]) > .1) continue;
						// ok the points are equal and should have the same lod error
						grid2->widthLodError[l] = grid1->widthLodError[k];
						touch = qtrue;
					}
				}
				for (m = 0; m < 2; m++) {

					if (m) offset2 = grid2->width-1;
					else offset2 = 0;
					if (R_MergedHeightPoints(grid2, offset2)) continue;
					for ( l = 1; l < grid2->height-1; l++) {
					//
						if ( fabs(grid1->verts[k + offset1].xyz[0] - grid2->verts[grid2->width * l + offset2].xyz[0]) > .1) continue;
						if ( fabs(grid1->verts[k + offset1].xyz[1] - grid2->verts[grid2->width * l + offset2].xyz[1]) > .1) continue;
						if ( fabs(grid1->verts[k + offset1].xyz[2] - grid2->verts[grid2->width * l + offset2].xyz[2]) > .1) continue;
						// ok the points are equal and should have the same lod error
						grid2->heightLodError[l] = grid1->widthLodError[k];
						touch = qtrue;
					}
				}
			}
		}
		for (n = 0; n < 2; n++) {
			//
			if (n) offset1 = grid1->width-1;
			else offset1 = 0;
			if (R_MergedHeightPoints(grid1, offset1)) continue;
			for (k = 1; k < grid1->height-1; k++) {
				for (m = 0; m < 2; m++) {

					if (m) offset2 = (grid2->height-1) * grid2->width;
					else offset2 = 0;
					if (R_MergedWidthPoints(grid2, offset2)) continue;
					for ( l = 1; l < grid2->width-1; l++) {
					//
						if ( fabs(grid1->verts[grid1->width * k + offset1].xyz[0] - grid2->verts[l + offset2].xyz[0]) > .1) continue;
						if ( fabs(grid1->verts[grid1->width * k + offset1].xyz[1] - grid2->verts[l + offset2].xyz[1]) > .1) continue;
						if ( fabs(grid1->verts[grid1->width * k + offset1].xyz[2] - grid2->verts[l + offset2].xyz[2]) > .1) continue;
						// ok the points are equal and should have the same lod error
						grid2->widthLodError[l] = grid1->heightLodError[k];
						touch = qtrue;
					}
				}
				for (m = 0; m < 2; m++) {

					if (m) offset2 = grid2->width-1;
					else offset2 = 0;
					if (R_MergedHeightPoints(grid2, offset2)) continue;
					for ( l = 1; l < grid2->height-1; l++) {
					//
						if ( fabs(grid1->verts[grid1->width * k + offset1].xyz[0] - grid2->verts[grid2->width * l + offset2].xyz[0]) > .1) continue;
						if ( fabs(grid1->verts[grid1->width * k + offset1].xyz[1] - grid2->verts[grid2->width * l + offset2].xyz[1]) > .1) continue;
						if ( fabs(grid1->verts[grid1->width * k + offset1].xyz[2] - grid2->verts[grid2->width * l + offset2].xyz[2]) > .1) continue;
						// ok the points are equal and should have the same lod error
						grid2->heightLodError[l] = grid1->heightLodError[k];
						touch = qtrue;
					}
				}
			}
		}
		if (touch) {
			grid2->lodFixed = 2;
			R_FixSharedVertexLodError_r ( worldData, start, grid2 );
			//NOTE: this would be correct but makes things really slow
			//grid2->lodFixed = 1;
		}
	}
}

/*
=================
R_FixSharedVertexLodError

This function assumes that all patches in one group are nicely stitched together for the highest LoD.
If this is not the case this function will still do its job but won't fix the highest LoD cracks.
=================
*/
void R_FixSharedVertexLodError( world_t *worldData ) {
	int i;
	srfBspSurface_t *grid1;

	for ( i = 0; i < worldData->numsurfaces; i++ ) {
		//
		grid1 = (srfBspSurface_t *) worldData->surfaces[i].data;
		// if this surface is not a grid
		if ( grid1->surfaceType != SF_GRID )
			continue;
		//
		if ( grid1->lodFixed )
			continue;
		//
		grid1->lodFixed = 2;
		// recursively fix other patches in the same LOD group
		R_FixSharedVertexLodError_r( worldData, i + 1, grid1);
	}
}


/*
===============
R_StitchPatches
===============
*/
int R_StitchPatches( world_t *worldData, int grid1num, int grid2num ) {
	float *v1, *v2;
	srfBspSurface_t *grid1, *grid2;
	int k, l, m, n, offset1, offset2, row, column;

	grid1 = (srfBspSurface_t *) worldData->surfaces[grid1num].data;
	grid2 = (srfBspSurface_t *) worldData->surfaces[grid2num].data;
	for (n = 0; n < 2; n++) {
		//
		if (n) offset1 = (grid1->height-1) * grid1->width;
		else offset1 = 0;
		if (R_MergedWidthPoints(grid1, offset1))
			continue;
		for (k = 0; k < grid1->width-2; k += 2) {

			for (m = 0; m < 2; m++) {

				if ( grid2->width >= MAX_GRID_SIZE )
					break;
				if (m) offset2 = (grid2->height-1) * grid2->width;
				else offset2 = 0;
				for ( l = 0; l < grid2->width-1; l++) {
				//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[k + 2 + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if (m) row = grid2->height-1;
					else row = 0;
					grid2 = R_GridInsertColumn( grid2, l+1, row,
									grid1->verts[k + 1 + offset1].xyz, grid1->widthLodError[k+1]);
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
			for (m = 0; m < 2; m++) {

				if (grid2->height >= MAX_GRID_SIZE)
					break;
				if (m) offset2 = grid2->width-1;
				else offset2 = 0;
				for ( l = 0; l < grid2->height-1; l++) {
					//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[k + 2 + offset1].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if (m) column = grid2->width-1;
					else column = 0;
					grid2 = R_GridInsertRow( grid2, l+1, column,
										grid1->verts[k + 1 + offset1].xyz, grid1->widthLodError[k+1]);
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
		}
	}
	for (n = 0; n < 2; n++) {
		//
		if (n) offset1 = grid1->width-1;
		else offset1 = 0;
		if (R_MergedHeightPoints(grid1, offset1))
			continue;
		for (k = 0; k < grid1->height-2; k += 2) {
			for (m = 0; m < 2; m++) {

				if ( grid2->width >= MAX_GRID_SIZE )
					break;
				if (m) offset2 = (grid2->height-1) * grid2->width;
				else offset2 = 0;
				for ( l = 0; l < grid2->width-1; l++) {
				//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[grid1->width * (k + 2) + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[(l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if (m) row = grid2->height-1;
					else row = 0;
					grid2 = R_GridInsertColumn( grid2, l+1, row,
									grid1->verts[grid1->width * (k + 1) + offset1].xyz, grid1->heightLodError[k+1]);
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
			for (m = 0; m < 2; m++) {

				if (grid2->height >= MAX_GRID_SIZE)
					break;
				if (m) offset2 = grid2->width-1;
				else offset2 = 0;
				for ( l = 0; l < grid2->height-1; l++) {
				//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[grid1->width * (k + 2) + offset1].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if (m) column = grid2->width-1;
					else column = 0;
					grid2 = R_GridInsertRow( grid2, l+1, column,
									grid1->verts[grid1->width * (k + 1) + offset1].xyz, grid1->heightLodError[k+1]);
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
		}
	}
	for (n = 0; n < 2; n++) {
		//
		if (n) offset1 = (grid1->height-1) * grid1->width;
		else offset1 = 0;
		if (R_MergedWidthPoints(grid1, offset1))
			continue;
		for (k = grid1->width-1; k > 1; k -= 2) {

			for (m = 0; m < 2; m++) {

				if ( grid2->width >= MAX_GRID_SIZE )
					break;
				if (m) offset2 = (grid2->height-1) * grid2->width;
				else offset2 = 0;
				for ( l = 0; l < grid2->width-1; l++) {
				//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[k - 2 + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[(l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if (m) row = grid2->height-1;
					else row = 0;
					grid2 = R_GridInsertColumn( grid2, l+1, row,
										grid1->verts[k - 1 + offset1].xyz, grid1->widthLodError[k+1]);
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
			for (m = 0; m < 2; m++) {

				if (grid2->height >= MAX_GRID_SIZE)
					break;
				if (m) offset2 = grid2->width-1;
				else offset2 = 0;
				for ( l = 0; l < grid2->height-1; l++) {
				//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[k - 2 + offset1].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if (m) column = grid2->width-1;
					else column = 0;
					grid2 = R_GridInsertRow( grid2, l+1, column,
										grid1->verts[k - 1 + offset1].xyz, grid1->widthLodError[k+1]);
					if (!grid2)
						break;
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
		}
	}
	for (n = 0; n < 2; n++) {
		//
		if (n) offset1 = grid1->width-1;
		else offset1 = 0;
		if (R_MergedHeightPoints(grid1, offset1))
			continue;
		for (k = grid1->height-1; k > 1; k -= 2) {
			for (m = 0; m < 2; m++) {

				if ( grid2->width >= MAX_GRID_SIZE )
					break;
				if (m) offset2 = (grid2->height-1) * grid2->width;
				else offset2 = 0;
				for ( l = 0; l < grid2->width-1; l++) {
				//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[grid1->width * (k - 2) + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[(l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if (m) row = grid2->height-1;
					else row = 0;
					grid2 = R_GridInsertColumn( grid2, l+1, row,
										grid1->verts[grid1->width * (k - 1) + offset1].xyz, grid1->heightLodError[k+1]);
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
			for (m = 0; m < 2; m++) {

				if (grid2->height >= MAX_GRID_SIZE)
					break;
				if (m) offset2 = grid2->width-1;
				else offset2 = 0;
				for ( l = 0; l < grid2->height-1; l++) {
				//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;

					v1 = grid1->verts[grid1->width * (k - 2) + offset1].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) > .1)
						continue;
					if ( fabs(v1[1] - v2[1]) > .1)
						continue;
					if ( fabs(v1[2] - v2[2]) > .1)
						continue;
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * (l + 1) + offset2].xyz;
					if ( fabs(v1[0] - v2[0]) < .01 &&
							fabs(v1[1] - v2[1]) < .01 &&
							fabs(v1[2] - v2[2]) < .01)
						continue;
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if (m) column = grid2->width-1;
					else column = 0;
					grid2 = R_GridInsertRow( grid2, l+1, column,
										grid1->verts[grid1->width * (k - 1) + offset1].xyz, grid1->heightLodError[k+1]);
					grid2->lodStitched = qfalse;
					worldData->surfaces[grid2num].data = (surfaceType_t *) grid2;
					return qtrue;
				}
			}
		}
	}
	return qfalse;
}

/*
===============
R_TryStitchPatch

This function will try to stitch patches in the same LoD group together for the highest LoD.

Only single missing vertice cracks will be fixed.

Vertices will be joined at the patch side a crack is first found, at the other side
of the patch (on the same row or column) the vertices will not be joined and cracks
might still appear at that side.
===============
*/
int R_TryStitchingPatch( world_t *worldData, int grid1num ) {
	int j, numstitches;
	srfBspSurface_t *grid1, *grid2;

	numstitches = 0;
	grid1 = (srfBspSurface_t *) worldData->surfaces[grid1num].data;
	for ( j = 0; j < worldData->numsurfaces; j++ ) {
		//
		grid2 = (srfBspSurface_t *) worldData->surfaces[j].data;
		// if this surface is not a grid
		if ( grid2->surfaceType != SF_GRID ) continue;
		// grids in the same LOD group should have the exact same lod radius
		if ( grid1->lodRadius != grid2->lodRadius ) continue;
		// grids in the same LOD group should have the exact same lod origin
		if ( grid1->lodOrigin[0] != grid2->lodOrigin[0] ) continue;
		if ( grid1->lodOrigin[1] != grid2->lodOrigin[1] ) continue;
		if ( grid1->lodOrigin[2] != grid2->lodOrigin[2] ) continue;
		//
		while (R_StitchPatches(worldData, grid1num, j))
		{
			numstitches++;
		}
	}
	return numstitches;
}

/*
===============
R_StitchAllPatches
===============
*/
void R_StitchAllPatches( world_t *worldData ) {
	int i, stitched, numstitches;
	srfBspSurface_t *grid1;

	numstitches = 0;
	do
	{
		stitched = qfalse;
		for ( i = 0; i < worldData->numsurfaces; i++ ) {
			//
			grid1 = (srfBspSurface_t *) worldData->surfaces[i].data;
			// if this surface is not a grid
			if ( grid1->surfaceType != SF_GRID )
				continue;
			//
			if ( grid1->lodStitched )
				continue;
			//
			grid1->lodStitched = qtrue;
			stitched = qtrue;
			//
			numstitches += R_TryStitchingPatch( worldData, i );
		}
	}
	while (stitched);
	ri.Printf( PRINT_ALL, "stitched %d LoD cracks\n", numstitches );
}

/*
===============
R_MovePatchSurfacesToHunk
===============
*/
void R_MovePatchSurfacesToHunk( world_t *worldData ) {
	int i, size;
	srfBspSurface_t *grid, *hunkgrid;

	for ( i = 0; i < worldData->numsurfaces; i++ ) {
		//
		grid = (srfBspSurface_t *) worldData->surfaces[i].data;
		// if this surface is not a grid
		if ( grid->surfaceType != SF_GRID )
			continue;
		//
		size = sizeof(*grid);
		hunkgrid = (srfBspSurface_t *)R_Hunk_Alloc(size, qtrue);
		Com_Memcpy(hunkgrid, grid, size);

		hunkgrid->widthLodError = (float *)R_Hunk_Alloc( grid->width * 4, qtrue );
		Com_Memcpy( hunkgrid->widthLodError, grid->widthLodError, grid->width * 4 );

		hunkgrid->heightLodError = (float *)R_Hunk_Alloc( grid->height * 4, qtrue);
		Com_Memcpy( hunkgrid->heightLodError, grid->heightLodError, grid->height * 4 );

		hunkgrid->numIndexes = grid->numIndexes;
		hunkgrid->indexes = (glIndex_t *)R_Hunk_Alloc(grid->numIndexes * sizeof(glIndex_t), qtrue);
		Com_Memcpy(hunkgrid->indexes, grid->indexes, grid->numIndexes * sizeof(glIndex_t));

		hunkgrid->numVerts = grid->numVerts;
		hunkgrid->verts = (srfVert_t *)R_Hunk_Alloc(grid->numVerts * sizeof(srfVert_t), qtrue);
		Com_Memcpy(hunkgrid->verts, grid->verts, grid->numVerts * sizeof(srfVert_t));

		R_FreeSurfaceGridMesh( grid );

		worldData->surfaces[i].data = (surfaceType_t *) hunkgrid;
	}
}


/*
=================
BSPSurfaceCompare
compare function for qsort()
=================
*/
static int BSPSurfaceCompare(const void *a, const void *b)
{
	msurface_t   *aa, *bb;

	aa = *(msurface_t **) a;
	bb = *(msurface_t **) b;

	// shader first
	if(aa->shader->sortedIndex < bb->shader->sortedIndex)
		return -1;

	else if(aa->shader->sortedIndex > bb->shader->sortedIndex)
		return 1;

	// by fogIndex
	if(aa->fogIndex < bb->fogIndex)
		return -1;

	else if(aa->fogIndex > bb->fogIndex)
		return 1;

	// by cubemapIndex
	if(aa->cubemapIndex < bb->cubemapIndex)
		return -1;

	else if(aa->cubemapIndex > bb->cubemapIndex)
		return 1;


	return 0;
}

struct packedVertex_t
{
	vec3_t position;
	uint32_t normal;
	uint32_t tangent;
	vec2_t texcoords[1 + MAXLIGHTMAPS];
	vec4_t colors[MAXLIGHTMAPS];
	uint32_t lightDirection;
};

/*
===============
R_CreateWorldVBOs
===============
*/
static void R_CreateWorldVBOs( world_t *worldData )
{
	int             i, j, k;

	int             numVerts;
	packedVertex_t  *verts;

	int             numIndexes;
	glIndex_t      *indexes;

    int             numSortedSurfaces, numSurfaces;
	msurface_t   *surface, **firstSurf, **lastSurf, **currSurf;
	msurface_t  **surfacesSorted;

	VBO_t *vbo;
	IBO_t *ibo;

	int maxVboSize = 64 * 1024 * 1024;
	int maxIboSize = 16 * 1024 * 1024;

	int             startTime, endTime;

	startTime = ri.Milliseconds();

	// count surfaces
	numSortedSurfaces = 0;
	for(surface = &worldData->surfaces[0]; surface < &worldData->surfaces[worldData->numsurfaces]; surface++)
	{
		srfBspSurface_t *bspSurf;
		shader_t *shader = surface->shader;

		if (shader->isPortal)
			continue;

		if (shader->isSky)
			continue;

		if (ShaderRequiresCPUDeforms(shader))
			continue;

		// check for this now so we can use srfBspSurface_t* universally in the rest of the function
		if (!(*surface->data == SF_FACE || *surface->data == SF_GRID || *surface->data == SF_TRIANGLES))
			continue;

		bspSurf = (srfBspSurface_t *) surface->data;

		if (!bspSurf->numIndexes || !bspSurf->numVerts)
			continue;

		numSortedSurfaces++;
	}

	// presort surfaces
	surfacesSorted = (msurface_t **)R_Malloc(numSortedSurfaces * sizeof(*surfacesSorted), TAG_BSP, qfalse);

	j = 0;
	for(surface = &worldData->surfaces[0]; surface < &worldData->surfaces[worldData->numsurfaces]; surface++)
	{
		srfBspSurface_t *bspSurf;
		shader_t *shader = surface->shader;

		if (shader->isPortal)
			continue;

		if (shader->isSky)
			continue;

		if (ShaderRequiresCPUDeforms(shader))
			continue;

		// check for this now so we can use srfBspSurface_t* universally in the rest of the function
		if (!(*surface->data == SF_FACE || *surface->data == SF_GRID || *surface->data == SF_TRIANGLES))
			continue;

		bspSurf = (srfBspSurface_t *) surface->data;

		if (!bspSurf->numIndexes || !bspSurf->numVerts)
			continue;

		surfacesSorted[j++] = surface;
	}

	qsort(surfacesSorted, numSortedSurfaces, sizeof(*surfacesSorted), BSPSurfaceCompare);

	k = 0;
	for(firstSurf = lastSurf = surfacesSorted; firstSurf < &surfacesSorted[numSortedSurfaces]; firstSurf = lastSurf)
	{
		int currVboSize, currIboSize;

		// Find range of surfaces to merge by:
		// - Collecting a number of surfaces which fit under maxVboSize/maxIboSize, or
		// - All the surfaces with a single shader which go over maxVboSize/maxIboSize
		currVboSize = currIboSize = 0;
		while (currVboSize < maxVboSize && currIboSize < maxIboSize && lastSurf < &surfacesSorted[numSortedSurfaces])
		{
			int addVboSize, addIboSize, currShaderIndex;

			addVboSize = addIboSize = 0;
			currShaderIndex = (*lastSurf)->shader->sortedIndex;

			for(currSurf = lastSurf; currSurf < &surfacesSorted[numSortedSurfaces] && (*currSurf)->shader->sortedIndex == currShaderIndex; currSurf++)
			{
				srfBspSurface_t *bspSurf = (srfBspSurface_t *) (*currSurf)->data;

				addVboSize += bspSurf->numVerts * sizeof(srfVert_t);
				addIboSize += bspSurf->numIndexes * sizeof(glIndex_t);
			}

			if ((currVboSize != 0 && addVboSize + currVboSize > maxVboSize)
			 || (currIboSize != 0 && addIboSize + currIboSize > maxIboSize))
				break;

			lastSurf = currSurf;

			currVboSize += addVboSize;
			currIboSize += addIboSize;
		}

		// count verts/indexes/surfaces
		numVerts = 0;
		numIndexes = 0;
		numSurfaces = 0;
		for (currSurf = firstSurf; currSurf < lastSurf; currSurf++)
		{
			srfBspSurface_t *bspSurf = (srfBspSurface_t *) (*currSurf)->data;

			numVerts += bspSurf->numVerts;
			numIndexes += bspSurf->numIndexes;
			numSurfaces++;
		}

		ri.Printf(PRINT_ALL, "...calculating world VBO %d ( %i verts %i tris )\n", k, numVerts, numIndexes / 3);

		// create arrays
		verts = (packedVertex_t *)R_Malloc(numVerts * sizeof(packedVertex_t), TAG_TEMP_WORKSPACE, qfalse);
		indexes = (glIndex_t *)R_Malloc(numIndexes * sizeof(glIndex_t), TAG_TEMP_WORKSPACE, qfalse);

		// set up indices and copy vertices
		numVerts = 0;
		numIndexes = 0;
		for (currSurf = firstSurf; currSurf < lastSurf; currSurf++)
		{
			srfBspSurface_t *bspSurf = (srfBspSurface_t *) (*currSurf)->data;
			glIndex_t *surfIndex;

			bspSurf->firstIndex = numIndexes;
			bspSurf->minIndex = numVerts + bspSurf->indexes[0];
			bspSurf->maxIndex = numVerts + bspSurf->indexes[0];

			for(i = 0, surfIndex = bspSurf->indexes; i < bspSurf->numIndexes; i++, surfIndex++)
			{
				indexes[numIndexes++] = numVerts + *surfIndex;
				bspSurf->minIndex = MIN(bspSurf->minIndex, numVerts + *surfIndex);
				bspSurf->maxIndex = MAX(bspSurf->maxIndex, numVerts + *surfIndex);
			}

			bspSurf->firstVert = numVerts;

			for(i = 0; i < bspSurf->numVerts; i++)
			{
				packedVertex_t& vert = verts[numVerts++];

				VectorCopy (bspSurf->verts[i].xyz, vert.position);
				vert.normal = R_VboPackNormal (bspSurf->verts[i].normal);
				vert.tangent = R_VboPackTangent (bspSurf->verts[i].tangent);
				VectorCopy2 (bspSurf->verts[i].st, vert.texcoords[0]);

				for (int j = 0; j < MAXLIGHTMAPS; j++)
				{
					VectorCopy2 (bspSurf->verts[i].lightmap[j], vert.texcoords[1 + j]);
				}

				for (int j = 0; j < MAXLIGHTMAPS; j++)
				{
					VectorCopy4 (bspSurf->verts[i].vertexColors[j], vert.colors[j]);
				}

				vert.lightDirection = R_VboPackNormal (bspSurf->verts[i].lightdir);
			}
		}

		vbo = R_CreateVBO((byte *)verts, sizeof (packedVertex_t) * numVerts, VBO_USAGE_STATIC);
		ibo = R_CreateIBO((byte *)indexes, numIndexes * sizeof (glIndex_t), VBO_USAGE_STATIC);

		// Setup the offsets and strides
		vbo->offsets[ATTR_INDEX_POSITION] = offsetof(packedVertex_t, position);
		vbo->offsets[ATTR_INDEX_NORMAL] = offsetof(packedVertex_t, normal);
		vbo->offsets[ATTR_INDEX_TANGENT] = offsetof(packedVertex_t, tangent);
		vbo->offsets[ATTR_INDEX_TEXCOORD0] = offsetof(packedVertex_t, texcoords[0]);
		vbo->offsets[ATTR_INDEX_TEXCOORD1] = offsetof(packedVertex_t, texcoords[1]);
		vbo->offsets[ATTR_INDEX_TEXCOORD2] = offsetof(packedVertex_t, texcoords[2]);
		vbo->offsets[ATTR_INDEX_TEXCOORD3] = offsetof(packedVertex_t, texcoords[3]);
		vbo->offsets[ATTR_INDEX_TEXCOORD4] = offsetof(packedVertex_t, texcoords[4]);
		vbo->offsets[ATTR_INDEX_COLOR] = offsetof(packedVertex_t, colors);
		vbo->offsets[ATTR_INDEX_LIGHTDIRECTION] = offsetof(packedVertex_t, lightDirection);

		const size_t packedVertexSize = sizeof(packedVertex_t);
		vbo->strides[ATTR_INDEX_POSITION] = packedVertexSize;
		vbo->strides[ATTR_INDEX_NORMAL] = packedVertexSize;
		vbo->strides[ATTR_INDEX_TANGENT] = packedVertexSize;
		vbo->strides[ATTR_INDEX_TEXCOORD0] = packedVertexSize;
		vbo->strides[ATTR_INDEX_TEXCOORD1] = packedVertexSize;
		vbo->strides[ATTR_INDEX_TEXCOORD2] = packedVertexSize;
		vbo->strides[ATTR_INDEX_TEXCOORD3] = packedVertexSize;
		vbo->strides[ATTR_INDEX_TEXCOORD4] = packedVertexSize;
		vbo->strides[ATTR_INDEX_COLOR] = packedVertexSize;
		vbo->strides[ATTR_INDEX_LIGHTDIRECTION] = packedVertexSize;

		vbo->sizes[ATTR_INDEX_POSITION] = sizeof(verts->position);
		vbo->sizes[ATTR_INDEX_NORMAL] = sizeof(verts->normal);
		vbo->sizes[ATTR_INDEX_TEXCOORD0] = sizeof(verts->texcoords[0]);
		vbo->sizes[ATTR_INDEX_TEXCOORD1] = sizeof(verts->texcoords[0]);
		vbo->sizes[ATTR_INDEX_TEXCOORD2] = sizeof(verts->texcoords[0]);
		vbo->sizes[ATTR_INDEX_TEXCOORD3] = sizeof(verts->texcoords[0]);
		vbo->sizes[ATTR_INDEX_TEXCOORD4] = sizeof(verts->texcoords[0]);
		vbo->sizes[ATTR_INDEX_TANGENT] = sizeof(verts->tangent);
		vbo->sizes[ATTR_INDEX_LIGHTDIRECTION] = sizeof(verts->lightDirection);
		vbo->sizes[ATTR_INDEX_COLOR] = sizeof(verts->colors);

		// point bsp surfaces to VBO
		for (currSurf = firstSurf; currSurf < lastSurf; currSurf++)
		{
			srfBspSurface_t *bspSurf = (srfBspSurface_t *) (*currSurf)->data;

			bspSurf->vbo = vbo;
			bspSurf->ibo = ibo;
		}

		R_Free(indexes);
		R_Free(verts);

		k++;
	}

	R_Free(surfacesSorted);

	endTime = ri.Milliseconds();
	ri.Printf(PRINT_ALL, "world VBOs calculation time = %5.2f seconds\n", (endTime - startTime) / 1000.0);
}

/*
===============
R_LoadSurfaces
===============
*/
static	void R_LoadSurfaces( world_t *worldData, lump_t *surfs, lump_t *verts, lump_t *indexLump ) {
	dsurface_t	*in;
	msurface_t	*out;
	drawVert_t	*dv;
	int			*indexes;
	int			count;
	int			numFaces, numMeshes, numTriSurfs, numFlares;
	int			i;
	float *hdrVertColors = NULL;

	numFaces = 0;
	numMeshes = 0;
	numTriSurfs = 0;
	numFlares = 0;

	if (surfs->filelen % sizeof(*in))
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	count = surfs->filelen / sizeof(*in);

	dv = (drawVert_t *)(fileBase + verts->fileofs);
	if (verts->filelen % sizeof(*dv))
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);

	indexes = (int *)(fileBase + indexLump->fileofs);
	if ( indexLump->filelen % sizeof(*indexes))
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);

	out = (msurface_t *)R_Hunk_Alloc ( count * sizeof(*out), qtrue );	

	worldData->surfaces = out;
	worldData->numsurfaces = count;
	worldData->surfacesViewCount = (int *)R_Hunk_Alloc ( count * sizeof(*worldData->surfacesViewCount), qtrue );
	worldData->surfacesDlightBits = (int *)R_Hunk_Alloc ( count * sizeof(*worldData->surfacesDlightBits), qtrue);
	worldData->surfacesPshadowBits = (int *)R_Hunk_Alloc ( count * sizeof(*worldData->surfacesPshadowBits), qtrue);

	// load hdr vertex colors
	if (r_hdr->integer)
	{
		char filename[MAX_QPATH];
		int size;

		Com_sprintf( filename, sizeof( filename ), "maps/%s/vertlight.raw", worldData->baseName);
		//ri.Printf(PRINT_ALL, "looking for %s\n", filename);

		size = ri.FS_ReadFile(filename, (void **)&hdrVertColors);

		if (hdrVertColors)
		{
			//ri.Printf(PRINT_ALL, "Found!\n");
			if (size != sizeof(float) * 3 * (verts->filelen / sizeof(*dv)))
				ri.Error(ERR_DROP, "Bad size for %s (%i, expected %i)!", filename, size, (int)((sizeof(float)) * 3 * (verts->filelen / sizeof(*dv))));
		}
	}


	// Two passes, allocate surfaces first, then load them full of data
	// This ensures surfaces are close together to reduce L2 cache misses when using VBOs,
	// which don't actually use the verts and indexes
	in = (dsurface_t *)(fileBase + surfs->fileofs);
	out = worldData->surfaces;
	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		switch ( LittleLong( in->surfaceType ) ) {
			case MST_PATCH:
				// FIXME: do this
				break;
			case MST_TRIANGLE_SOUP:
				out->data = (surfaceType_t *)R_Hunk_Alloc( sizeof(srfBspSurface_t), qtrue);
				break;
			case MST_PLANAR:
				out->data = (surfaceType_t *)R_Hunk_Alloc( sizeof(srfBspSurface_t), qtrue);
				break;
			case MST_FLARE:
				out->data = (surfaceType_t *)R_Hunk_Alloc( sizeof(srfFlare_t), qtrue);
				break;
			default:
				break;
		}
	}

	in = (dsurface_t *)(fileBase + surfs->fileofs);
	out = worldData->surfaces;
	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		switch ( LittleLong( in->surfaceType ) ) {
		case MST_PATCH:
			ParseMesh ( worldData, in, dv, hdrVertColors, out );
			{
				srfBspSurface_t *surface = (srfBspSurface_t *)out->data;

				out->cullinfo.type = CULLINFO_BOX | CULLINFO_SPHERE;
				VectorCopy(surface->cullBounds[0], out->cullinfo.bounds[0]);
				VectorCopy(surface->cullBounds[1], out->cullinfo.bounds[1]);
				VectorCopy(surface->cullOrigin, out->cullinfo.localOrigin);
				out->cullinfo.radius = surface->cullRadius;
			}
			numMeshes++;
			break;
		case MST_TRIANGLE_SOUP:
			ParseTriSurf( worldData, in, dv, hdrVertColors, out, indexes );
			numTriSurfs++;
			break;
		case MST_PLANAR:
			ParseFace( worldData, in, dv, hdrVertColors, out, indexes );
			numFaces++;
			break;
		case MST_FLARE:
			ParseFlare( worldData, in, dv, out, indexes );
			{
				out->cullinfo.type = CULLINFO_NONE;
			}
			numFlares++;
			break;
		default:
			ri.Error( ERR_DROP, "Bad surfaceType" );
		}
	}

	if (hdrVertColors)
	{
		ri.FS_FreeFile(hdrVertColors);
	}

#ifdef PATCH_STITCHING
	R_StitchAllPatches(worldData);
#endif

	R_FixSharedVertexLodError(worldData);

#ifdef PATCH_STITCHING
	R_MovePatchSurfacesToHunk(worldData);
#endif

	ri.Printf( PRINT_ALL, "...loaded %d faces, %i meshes, %i trisurfs, %i flares\n", 
		numFaces, numMeshes, numTriSurfs, numFlares );
}



/*
=================
R_LoadSubmodels
=================
*/
/*
=================
R_LoadSubmodels
=================
*/
static void R_LoadSubmodels(world_t *worldData, int worldIndex, lump_t *l) {
	dmodel_t	*in;
	bmodel_t	*out;
	int			i, j, count;

	in = (dmodel_t *)(fileBase + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Error(ERR_DROP, "LoadMap: funny lump size in %s", worldData->name);
	count = l->filelen / sizeof(*in);

	worldData->numBModels = count;
	worldData->bmodels = out = (bmodel_t *)R_Hunk_Alloc(count * sizeof(*out), qtrue);

	for (i = 0; i<count; i++, in++, out++) {
		model_t *model;

		model = R_AllocModel();

		if (model == NULL) {
			ri.Error(ERR_DROP, "R_LoadSubmodels: R_AllocModel() failed");
		}

		model->type = MOD_BRUSH;
		model->data.bmodel = out;
		Com_sprintf(model->name, sizeof(model->name), "*%d", i);

		for (j = 0; j<3; j++) {
			out->bounds[0][j] = LittleFloat(in->mins[j]);
			out->bounds[1][j] = LittleFloat(in->maxs[j]);
		}

		CModelCache->InsertModelHandle(model->name, model->index);

		out->worldIndex = worldIndex;
		out->firstSurface = LittleLong(in->firstSurface);
		out->numSurfaces = LittleLong(in->numSurfaces);

		if (i == 0)
		{
			// Add this for limiting VBO surface creation
			worldData->numWorldSurfaces = out->numSurfaces;
		}
	}
}



//==================================================================

/*
=================
R_SetParent
=================
*/
static	void R_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents != -1)
		return;
	R_SetParent (node->children[0], node);
	R_SetParent (node->children[1], node);
}

/*
=================
R_LoadNodesAndLeafs
=================
*/
static	void R_LoadNodesAndLeafs (world_t *worldData, lump_t *nodeLump, lump_t *leafLump) {
	int			i, j, p;
	dnode_t		*in;
	dleaf_t		*inLeaf;
	mnode_t 	*out;
	int			numNodes, numLeafs;

	in = (dnode_t *)(fileBase + nodeLump->fileofs);
	if (nodeLump->filelen % sizeof(dnode_t) ||
		leafLump->filelen % sizeof(dleaf_t) ) {
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	}
	numNodes = nodeLump->filelen / sizeof(dnode_t);
	numLeafs = leafLump->filelen / sizeof(dleaf_t);

	out = (mnode_t *)R_Hunk_Alloc ( (numNodes + numLeafs) * sizeof(*out), qtrue);

	worldData->nodes = out;
	worldData->numnodes = numNodes + numLeafs;
	worldData->numDecisionNodes = numNodes;

	// load nodes
	for ( i=0 ; i<numNodes; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->mins[j] = LittleLong (in->mins[j]);
			out->maxs[j] = LittleLong (in->maxs[j]);
		}
	
		p = LittleLong(in->planeNum);
		out->plane = worldData->planes + p;

		out->contents = CONTENTS_NODE;	// differentiate from leafs

		for (j=0 ; j<2 ; j++)
		{
			p = LittleLong (in->children[j]);
			if (p >= 0)
				out->children[j] = worldData->nodes + p;
			else
				out->children[j] = worldData->nodes + numNodes + (-1 - p);
		}
	}
	
	// load leafs
	inLeaf = (dleaf_t *)(fileBase + leafLump->fileofs);
	for ( i=0 ; i<numLeafs ; i++, inLeaf++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->mins[j] = LittleLong (inLeaf->mins[j]);
			out->maxs[j] = LittleLong (inLeaf->maxs[j]);
		}

		out->cluster = LittleLong(inLeaf->cluster);
		out->area = LittleLong(inLeaf->area);

		if ( out->cluster >= worldData->numClusters ) {
			worldData->numClusters = out->cluster + 1;
		}

		out->firstmarksurface = LittleLong(inLeaf->firstLeafSurface);
		out->nummarksurfaces = LittleLong(inLeaf->numLeafSurfaces);
	}	

	// chain decendants
	R_SetParent (worldData->nodes, NULL);
}

//=============================================================================

/*
=================
R_LoadShaders
=================
*/
static	void R_LoadShaders( world_t *worldData, lump_t *l ) {	
	int		i, count;
	dshader_t	*in, *out;
	
	in = (dshader_t *)(fileBase + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	count = l->filelen / sizeof(*in);
	out = (dshader_t *)R_Hunk_Alloc ( count*sizeof(*out), qtrue);

	worldData->shaders = out;
	worldData->numShaders = count;

	Com_Memcpy( out, in, count*sizeof(*out) );

	for ( i=0 ; i<count ; i++ ) {
		out[i].surfaceFlags = LittleLong( out[i].surfaceFlags );
		out[i].contentFlags = LittleLong( out[i].contentFlags );
	}
}


/*
=================
R_LoadMarksurfaces
=================
*/
static	void R_LoadMarksurfaces (world_t *worldData, lump_t *l)
{	
	int		i, j, count;
	int		*in;
	int     *out;
	
	in = (int *)(fileBase + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	count = l->filelen / sizeof(*in);
	out = (int *)R_Hunk_Alloc ( count*sizeof(*out), qtrue);

	worldData->marksurfaces = out;
	worldData->nummarksurfaces = count;

	for ( i=0 ; i<count ; i++)
	{
		j = LittleLong(in[i]);
		out[i] = j;
	}
}


/*
=================
R_LoadPlanes
=================
*/
static	void R_LoadPlanes( world_t *worldData, lump_t *l ) {
	int			i, j;
	cplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;
	
	in = (dplane_t *)(fileBase + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	count = l->filelen / sizeof(*in);
	out = (cplane_t *)R_Hunk_Alloc ( count*2*sizeof(*out), qtrue);
	
	worldData->planes = out;
	worldData->numplanes = count;

	for ( i=0 ; i<count ; i++, in++, out++) {
		bits = 0;
		for (j=0 ; j<3 ; j++) {
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0) {
				bits |= 1<<j;
			}
		}

		out->dist = LittleFloat (in->dist);
		out->type = PlaneTypeForNormal( out->normal );
		out->signbits = bits;
	}
}

/*
=================
R_LoadFogs

=================
*/
static	void R_LoadFogs( world_t *worldData, lump_t *l, lump_t *brushesLump, lump_t *sidesLump ) {
	int			i;
	fog_t		*out;
	dfog_t		*fogs;
	dbrush_t 	*brushes, *brush;
	dbrushside_t	*sides;
	int			count, brushesCount, sidesCount;
	int			sideNum;
	int			planeNum;
	shader_t	*shader;
	float		d;
	int			firstSide;

	fogs = (dfog_t *)(fileBase + l->fileofs);
	if (l->filelen % sizeof(*fogs)) {
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	}
	count = l->filelen / sizeof(*fogs);

	// create fog strucutres for them
	worldData->numfogs = count + 1;
	worldData->fogs = (fog_t *)R_Hunk_Alloc ( worldData->numfogs*sizeof(*out), qtrue);
	worldData->globalFog = nullptr;
	out = worldData->fogs + 1;

	if ( !count ) {
		return;
	}

	brushes = (dbrush_t *)(fileBase + brushesLump->fileofs);
	if (brushesLump->filelen % sizeof(*brushes)) {
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	}
	brushesCount = brushesLump->filelen / sizeof(*brushes);

	sides = (dbrushside_t *)(fileBase + sidesLump->fileofs);
	if (sidesLump->filelen % sizeof(*sides)) {
		ri.Error (ERR_DROP, "LoadMap: funny lump size in %s",worldData->name);
	}
	sidesCount = sidesLump->filelen / sizeof(*sides);

	for ( i=0 ; i<count ; i++, fogs++) {
		out->originalBrushNumber = LittleLong( fogs->brushNum );

		if ( out->originalBrushNumber == -1 )
		{
			out->bounds[0][0] = out->bounds[0][1] = out->bounds[0][2] = MIN_WORLD_COORD;
			out->bounds[1][0] = out->bounds[1][1] = out->bounds[1][2] = MAX_WORLD_COORD;
			firstSide = -1;

			worldData->globalFog = worldData->fogs + i + 1;
		}
		else
		{
			if ( (unsigned)out->originalBrushNumber >= brushesCount ) {
				ri.Error( ERR_DROP, "fog brushNumber out of range" );
			}
			brush = brushes + out->originalBrushNumber;

			firstSide = LittleLong( brush->firstSide );

				if ( (unsigned)firstSide > sidesCount - 6 ) {
				ri.Error( ERR_DROP, "fog brush sideNumber out of range" );
			}

			// brushes are always sorted with the axial sides first
			sideNum = firstSide + 0;
			planeNum = LittleLong( sides[ sideNum ].planeNum );
			out->bounds[0][0] = -worldData->planes[ planeNum ].dist;

			sideNum = firstSide + 1;
			planeNum = LittleLong( sides[ sideNum ].planeNum );
			out->bounds[1][0] = worldData->planes[ planeNum ].dist;

			sideNum = firstSide + 2;
			planeNum = LittleLong( sides[ sideNum ].planeNum );
			out->bounds[0][1] = -worldData->planes[ planeNum ].dist;

			sideNum = firstSide + 3;
			planeNum = LittleLong( sides[ sideNum ].planeNum );
			out->bounds[1][1] = worldData->planes[ planeNum ].dist;

			sideNum = firstSide + 4;
			planeNum = LittleLong( sides[ sideNum ].planeNum );
			out->bounds[0][2] = -worldData->planes[ planeNum ].dist;

			sideNum = firstSide + 5;
			planeNum = LittleLong( sides[ sideNum ].planeNum );
			out->bounds[1][2] = worldData->planes[ planeNum ].dist;
		}

		// get information from the shader for fog parameters
		shader = R_FindShader( fogs->shader, lightmapsNone, stylesDefault, qtrue );

		out->parms = shader->fogParms;

		VectorSet4(out->color,
			shader->fogParms.color[0], 
			shader->fogParms.color[1], 
			shader->fogParms.color[2],
			1.0);

		d = shader->fogParms.depthForOpaque < 1 ? 1 : shader->fogParms.depthForOpaque;
		out->tcScale = 1.0f / d;

		// set the gradient vector
		sideNum = LittleLong( fogs->visibleSide );

		out->hasSurface = qtrue;
		if ( sideNum != -1 ) {
			planeNum = LittleLong( sides[ firstSide + sideNum ].planeNum );
			VectorSubtract( vec3_origin, worldData->planes[ planeNum ].normal, out->surface );
			out->surface[3] = -worldData->planes[ planeNum ].dist;
		}

		out++;
	}

}


/*
================
R_LoadLightGrid

================
*/
void R_LoadLightGrid( world_t *worldData, lump_t *l ) {
	int		i;
	vec3_t	maxs;
	float	*wMins, *wMaxs;

	worldData->lightGridInverseSize[0] = 1.0f / worldData->lightGridSize[0];
	worldData->lightGridInverseSize[1] = 1.0f / worldData->lightGridSize[1];
	worldData->lightGridInverseSize[2] = 1.0f / worldData->lightGridSize[2];

	wMins = worldData->bmodels[0].bounds[0];
	wMaxs = worldData->bmodels[0].bounds[1];

	for ( i = 0 ; i < 3 ; i++ ) {
		worldData->lightGridOrigin[i] = worldData->lightGridSize[i] * ceil( wMins[i] / worldData->lightGridSize[i] );
		maxs[i] = worldData->lightGridSize[i] * floor( wMaxs[i] / worldData->lightGridSize[i] );
		worldData->lightGridBounds[i] = (maxs[i] - worldData->lightGridOrigin[i])/worldData->lightGridSize[i] + 1;
	}

	int numGridDataElements = l->filelen / sizeof(*worldData->lightGridData);

	worldData->lightGridData = (mgrid_t *)R_Hunk_Alloc( l->filelen, qtrue);
	Com_Memcpy( worldData->lightGridData, (void *)(fileBase + l->fileofs), l->filelen );

	// deal with overbright bits
	for ( i = 0 ; i < numGridDataElements ; i++ ) 
	{
		for(int j = 0; j < MAXLIGHTMAPS; j++)
		{
			R_ColorShiftLightingBytes(
				worldData->lightGridData[i].ambientLight[j],
				worldData->lightGridData[i].ambientLight[j]);
			R_ColorShiftLightingBytes(
				worldData->lightGridData[i].directLight[j],
				worldData->lightGridData[i].directLight[j]);
		}
	}

	// load hdr lightgrid
	if (r_hdr->integer)
	{
		char filename[MAX_QPATH];
		float *hdrLightGrid;
		int size;

		Com_sprintf( filename, sizeof( filename ), "maps/%s/lightgrid.raw", worldData->baseName);
		//ri.Printf(PRINT_ALL, "looking for %s\n", filename);

		size = ri.FS_ReadFile(filename, (void **)&hdrLightGrid);

		if (hdrLightGrid)
		{
			float lightScale = pow(2.0f, r_mapOverBrightBits->integer - tr.overbrightBits);

			//ri.Printf(PRINT_ALL, "found!\n");

			if (size != sizeof(float) * 6 * numGridDataElements)
			{
				ri.Error(ERR_DROP, "Bad size for %s (%i, expected %i)!", filename, size, (int)(sizeof(float)) * 6 * numGridDataElements);
			}

			worldData->hdrLightGrid = (float *)R_Hunk_Alloc(size, qtrue);

			for (i = 0; i < numGridDataElements ; i++)
			{
				worldData->hdrLightGrid[i * 6    ] = hdrLightGrid[i * 6    ] * lightScale;
				worldData->hdrLightGrid[i * 6 + 1] = hdrLightGrid[i * 6 + 1] * lightScale;
				worldData->hdrLightGrid[i * 6 + 2] = hdrLightGrid[i * 6 + 2] * lightScale;
				worldData->hdrLightGrid[i * 6 + 3] = hdrLightGrid[i * 6 + 3] * lightScale;
				worldData->hdrLightGrid[i * 6 + 4] = hdrLightGrid[i * 6 + 4] * lightScale;
				worldData->hdrLightGrid[i * 6 + 5] = hdrLightGrid[i * 6 + 5] * lightScale;
			}
		}

		if (hdrLightGrid)
			ri.FS_FreeFile(hdrLightGrid);
	}
}

/*
================
R_LoadLightGridArray

================
*/
void R_LoadLightGridArray( world_t *worldData, lump_t *l ) {
	worldData->numGridArrayElements = worldData->lightGridBounds[0] * worldData->lightGridBounds[1] * worldData->lightGridBounds[2];

	if ( l->filelen != (int)(worldData->numGridArrayElements * sizeof(*worldData->lightGridArray)) ) {
		Com_Printf (S_COLOR_YELLOW  "WARNING: light grid array mismatch\n" );
		worldData->lightGridData = NULL;
		return;
	}

	worldData->lightGridArray = (unsigned short *)R_Hunk_Alloc( l->filelen, qfalse );
	memcpy( worldData->lightGridArray, (void *)(fileBase + l->fileofs), l->filelen );
}

/*
================
R_LoadEntities
================
*/
void R_LoadEntities( world_t *worldData, lump_t *l ) {
	const char *p;
	char *token;
	char vertexRemapShaderText[] = "vertexremapshader";
	char remapShaderText[] = "remapshader";
	char keyname[MAX_TOKEN_CHARS];
	char value[MAX_TOKEN_CHARS];
	world_t	*w;

	COM_BeginParseSession();

	w = worldData;
	w->lightGridSize[0] = 64;
	w->lightGridSize[1] = 64;
	w->lightGridSize[2] = 128;

	tr.distanceCull = 12000;//DEFAULT_DISTANCE_CULL;

	p = (char *)(fileBase + l->fileofs);

	// store for reference by the cgame
	w->entityString = (char *)R_Hunk_Alloc( l->filelen + 1, qtrue);
	strcpy( w->entityString, p );
	w->entityParsePoint = w->entityString;

	token = COM_ParseExt( &p, qtrue );
	if (!*token || *token != '{') {
		return;
	}

	// only parse the world spawn
	while ( 1 ) {	
		// parse key
		token = COM_ParseExt( &p, qtrue );

		if ( !*token || *token == '}' ) {
			break;
		}
		Q_strncpyz(keyname, token, sizeof(keyname));

		// parse value
		token = COM_ParseExt( &p, qtrue );

		if ( !*token || *token == '}' ) {
			break;
		}
		Q_strncpyz(value, token, sizeof(value));

		// check for remapping of shaders for vertex lighting
		/*s = vertexRemapShaderText;
		if (!Q_strncmp(keyname, s, strlen(s)) ) {
			s = strchr(value, ';');
			if (!s) {
				ri.Printf( PRINT_WARNING, "WARNING: no semi colon in vertexshaderremap '%s'\n", value );
				break;
			}
			*s++ = 0;
			if (r_vertexLight->integer) {
				R_RemapShader(value, s, "0");
			}
			continue;
		}
		// check for remapping of shaders
		s = remapShaderText;
		if (!Q_strncmp(keyname, s, strlen(s)) ) {
			s = strchr(value, ';');
			if (!s) {
				ri.Printf( PRINT_WARNING, "WARNING: no semi colon in shaderremap '%s'\n", value );
				break;
			}
			*s++ = 0;
			R_RemapShader(value, s, "0");
			continue;
		}*/
 		if (!Q_stricmp(keyname, "distanceCull")) {
			sscanf(value, "%f", &tr.distanceCull );
			continue;
		}
		// check for a different grid size
		if (!Q_stricmp(keyname, "gridsize")) {
			sscanf(value, "%f %f %f", &w->lightGridSize[0], &w->lightGridSize[1], &w->lightGridSize[2] );
			continue;
		}

		// check for auto exposure
		if (!Q_stricmp(keyname, "autoExposureMinMax")) {
			sscanf(value, "%f %f", &tr.autoExposureMinMax[0], &tr.autoExposureMinMax[1]);
			continue;
		}
	}
}

/*
=================
R_GetEntityToken
=================
*/
qboolean R_GetEntityToken(char *buffer, int size) {
	char	*s;
	world_t *worldData = &s_worldData;
	
	if (size == -1)
	{ //force reset 
		worldData->entityParsePoint = worldData->entityString;
		return qtrue;
	}

	s = COM_Parse((const char **)&worldData->entityParsePoint);
	Q_strncpyz(buffer, s, size);
	if (!worldData->entityParsePoint && !s[0]) {
		worldData->entityParsePoint = worldData->entityString;
		return qfalse;
	}
	else {
		return qtrue;
	}
}


#ifndef MAX_SPAWN_VARS
#define MAX_SPAWN_VARS 64
#endif

// derived from G_ParseSpawnVars() in g_spawn.c
qboolean R_ParseSpawnVars( char *spawnVarChars, int maxSpawnVarChars, int *numSpawnVars, char *spawnVars[MAX_SPAWN_VARS][2] )
{
	char    keyname[MAX_TOKEN_CHARS];
	char	com_token[MAX_TOKEN_CHARS];
	int		numSpawnVarChars = 0;

	*numSpawnVars = 0;

	// parse the opening brace
	if ( !R_GetEntityToken( com_token, sizeof( com_token ) ) ) {
		// end of spawn string
		return qfalse;
	}
	if ( com_token[0] != '{' ) {
		ri.Printf( PRINT_ALL, "R_ParseSpawnVars: found %s when expecting {\n",com_token );
		return qfalse;
	}

	// go through all the key / value pairs
	while ( 1 ) {  
		int keyLength, tokenLength;

		// parse key
		if ( !R_GetEntityToken( keyname, sizeof( keyname ) ) ) {
			ri.Printf( PRINT_ALL, "R_ParseSpawnVars: EOF without closing brace\n" );
			return qfalse;
		}

		if ( keyname[0] == '}' ) {
			break;
		}

		// parse value  
		if ( !R_GetEntityToken( com_token, sizeof( com_token ) ) ) {
			ri.Printf( PRINT_ALL, "R_ParseSpawnVars: EOF without closing brace\n" );
			return qfalse;
		}

		if ( com_token[0] == '}' ) {
			ri.Printf( PRINT_ALL, "R_ParseSpawnVars: closing brace without data\n" );
			return qfalse;
		}

		if ( *numSpawnVars == MAX_SPAWN_VARS ) {
			ri.Printf( PRINT_ALL, "R_ParseSpawnVars: MAX_SPAWN_VARS\n" );
			return qfalse;
		}

		keyLength = strlen(keyname) + 1;
		tokenLength = strlen(com_token) + 1;

		if (numSpawnVarChars + keyLength + tokenLength > maxSpawnVarChars)
		{
			ri.Printf( PRINT_ALL, "R_ParseSpawnVars: MAX_SPAWN_VAR_CHARS\n" );
			return qfalse;
		}

		strcpy(spawnVarChars + numSpawnVarChars, keyname);
		spawnVars[ *numSpawnVars ][0] = spawnVarChars + numSpawnVarChars;
		numSpawnVarChars += keyLength;

		strcpy(spawnVarChars + numSpawnVarChars, com_token);
		spawnVars[ *numSpawnVars ][1] = spawnVarChars + numSpawnVarChars;
		numSpawnVarChars += tokenLength;

		(*numSpawnVars)++;
	}

	return qtrue;
}

void R_LoadEnvironmentJson(const char *baseName)
{
	char filename[MAX_QPATH];

	union {
		char *c;
		void *v;
	} buffer;
	char *bufferEnd;

	const char *environmentArrayJson;
	int filelen, i;

	Com_sprintf(filename, MAX_QPATH, "cubemaps/%s/env.json", baseName);

	filelen = ri.FS_ReadFile(filename, &buffer.v);
	if (!buffer.c)
		return;
	bufferEnd = buffer.c + filelen;

	ri.Printf(PRINT_ALL, "Loaded Enviroment JSON: %s\n", filename);

	if (JSON_ValueGetType(buffer.c, bufferEnd) != JSONTYPE_OBJECT)
	{
		ri.Printf(PRINT_ALL, "Bad %s: does not start with a object\n", filename);
		ri.FS_FreeFile(buffer.v);
		return;
	}
	//-----------------------------CUBEMAPS------------------------------------
	environmentArrayJson = JSON_ObjectGetNamedValue(buffer.c, bufferEnd, "Cubemaps");
	if (!environmentArrayJson)
	{
		ri.Printf(PRINT_ALL, "Bad %s: no Cubemaps\n", filename);
		ri.FS_FreeFile(buffer.v);
		return;
	}

	if (JSON_ValueGetType(environmentArrayJson, bufferEnd) != JSONTYPE_ARRAY)
	{
		ri.Printf(PRINT_ALL, "Bad %s: Cubemaps not an array\n", filename);
		ri.FS_FreeFile(buffer.v);
		return;
	}

	tr.numCubemaps = JSON_ArrayGetIndex(environmentArrayJson, bufferEnd, NULL, 0);
	tr.cubemaps = (cubemap_t *)R_Hunk_Alloc(tr.numCubemaps * sizeof(*tr.cubemaps), qtrue);
	memset(tr.cubemaps, 0, tr.numCubemaps * sizeof(*tr.cubemaps));

	for (i = 0; i < tr.numCubemaps; i++)
	{
		cubemap_t *cubemap = &tr.cubemaps[i];
		const char *cubemapJson, *keyValueJson, *indexes[3];
		int j;

		cubemapJson = JSON_ArrayGetValue(environmentArrayJson, bufferEnd, i);

		keyValueJson = JSON_ObjectGetNamedValue(cubemapJson, bufferEnd, "Name");
		if (!JSON_ValueGetString(keyValueJson, bufferEnd, cubemap->name, MAX_QPATH))
			cubemap->name[0] = '\0';

		keyValueJson = JSON_ObjectGetNamedValue(cubemapJson, bufferEnd, "Position");
		JSON_ArrayGetIndex(keyValueJson, bufferEnd, indexes, 3);
		for (j = 0; j < 3; j++)
			cubemap->origin[j] = JSON_ValueGetFloat(indexes[j], bufferEnd);

		cubemap->parallaxRadius = 1000.0f;
		keyValueJson = JSON_ObjectGetNamedValue(cubemapJson, bufferEnd, "Radius");
		if (keyValueJson)
			cubemap->parallaxRadius = JSON_ValueGetFloat(keyValueJson, bufferEnd);
	}

	//-----------------------------LIGHTS------------------------------------
	environmentArrayJson = JSON_ObjectGetNamedValue(buffer.c, bufferEnd, "Lights");
	if (!environmentArrayJson)
	{
		ri.Printf(PRINT_ALL, "Bad %s: no Lights\n", filename);
		ri.FS_FreeFile(buffer.v);
		return;
	}

	if (JSON_ValueGetType(environmentArrayJson, bufferEnd) != JSONTYPE_ARRAY)
	{
		ri.Printf(PRINT_ALL, "Bad %s: Lights not an array\n", filename);
		ri.FS_FreeFile(buffer.v);
		return;
	}

	tr.numRealTimeLights = JSON_ArrayGetIndex(environmentArrayJson, bufferEnd, NULL, 0);
	tr.realTimeLights = (realTimeLight_t *)R_Hunk_Alloc(tr.numRealTimeLights * sizeof(*tr.realTimeLights), qtrue);
	memset(tr.realTimeLights, 0, tr.numRealTimeLights * sizeof(*tr.realTimeLights));

	for (i = 0; i < tr.numRealTimeLights; i++)
	{
		realTimeLight_t *light = &tr.realTimeLights[i];
		const char *lightJson, *keyValueJson, *indexes[3];
		int j;

		lightJson = JSON_ArrayGetValue(environmentArrayJson, bufferEnd, i);

		keyValueJson = JSON_ObjectGetNamedValue(lightJson, bufferEnd, "Position");
		JSON_ArrayGetIndex(keyValueJson, bufferEnd, indexes, 3);
		for (j = 0; j < 3; j++)
			light->position[j] = JSON_ValueGetFloat(indexes[j], bufferEnd);

		keyValueJson = JSON_ObjectGetNamedValue(lightJson, bufferEnd, "Color");
		JSON_ArrayGetIndex(keyValueJson, bufferEnd, indexes, 3);
		for (j = 0; j < 3; j++)
			light->color[j] = JSON_ValueGetFloat(indexes[j], bufferEnd);

		light->strength = 100.0f;
		keyValueJson = JSON_ObjectGetNamedValue(lightJson, bufferEnd, "Strength");
		if (keyValueJson)
			light->strength = JSON_ValueGetFloat(keyValueJson, bufferEnd);
	}

	ri.FS_FreeFile(buffer.v);
}

void R_LoadCubemapEntities(char *cubemapEntityName)
{
	char spawnVarChars[2048];
	int numSpawnVars;
	char *spawnVars[MAX_SPAWN_VARS][2];
	int numCubemaps = 0;

	if (!Q_strncmp(cubemapEntityName, "misc_skyportal", strlen("misc_skyportal")))
	{
		memset(&tr.skyboxCubemap, 0, sizeof(tr.skyboxCubemap));
		while (R_ParseSpawnVars(spawnVarChars, sizeof(spawnVarChars), &numSpawnVars, spawnVars))
		{
			int i;
			char name[MAX_QPATH];
			qboolean isCubemap = qfalse;
			qboolean originSet = qfalse;
			vec3_t origin;
			float parallaxRadius = 100000.0f;

			name[0] = '\0';
			for (i = 0; i < numSpawnVars; i++)
			{
				if (!Q_stricmp(spawnVars[i][0], "classname") && !Q_stricmp(spawnVars[i][1], cubemapEntityName))
					isCubemap = qtrue;

				if (!Q_stricmp(spawnVars[i][0], "origin"))
				{
					sscanf(spawnVars[i][1], "%f %f %f", &origin[0], &origin[1], &origin[2]);
					originSet = qtrue;
				}
			}

			if (isCubemap && originSet)
			{
				cubemap_t *cubemap = &tr.cubemaps[numCubemaps];
				Q_strncpyz(cubemap->name, "SKYBOX_CUBEMAP", MAX_QPATH);
				VectorCopy(origin, cubemap->origin);
				cubemap->parallaxRadius = parallaxRadius;
			}
		}
		return;
	}


	// count cubemaps
	numCubemaps = 0;
	while (R_ParseSpawnVars(spawnVarChars, sizeof(spawnVarChars), &numSpawnVars, spawnVars))
	{
		int i;

		for (i = 0; i < numSpawnVars; i++)
		{
			if (!Q_stricmp(spawnVars[i][0], "classname") && !Q_stricmp(spawnVars[i][1], cubemapEntityName))
				numCubemaps++;
		}
	}

	if (!numCubemaps)
		return;

	tr.numCubemaps = numCubemaps;
	tr.cubemaps = (cubemap_t *)R_Hunk_Alloc(tr.numCubemaps * sizeof(*tr.cubemaps), qtrue);
	memset(tr.cubemaps, 0, tr.numCubemaps * sizeof(*tr.cubemaps));

	numCubemaps = 0;
	while (R_ParseSpawnVars(spawnVarChars, sizeof(spawnVarChars), &numSpawnVars, spawnVars))
	{
		int i;
		char name[MAX_QPATH];
		qboolean isCubemap = qfalse;
		qboolean originSet = qfalse;
		vec3_t origin;
		float parallaxRadius = 1000.0f;

		name[0] = '\0';
		for (i = 0; i < numSpawnVars; i++)
		{
			if (!Q_stricmp(spawnVars[i][0], "classname") && !Q_stricmp(spawnVars[i][1], cubemapEntityName))
				isCubemap = qtrue;

			if (!Q_stricmp(spawnVars[i][0], "name"))
				Q_strncpyz(name, spawnVars[i][1], MAX_QPATH);

			if (!Q_stricmp(spawnVars[i][0], "origin"))
			{
				sscanf(spawnVars[i][1], "%f %f %f", &origin[0], &origin[1], &origin[2]);
				originSet = qtrue;
			}
			else if (!Q_stricmp(spawnVars[i][0], "radius"))
			{
				sscanf(spawnVars[i][1], "%f", &parallaxRadius);
			}
		}

		if (isCubemap && originSet)
		{
			cubemap_t *cubemap = &tr.cubemaps[numCubemaps];
			Q_strncpyz(cubemap->name, name, MAX_QPATH);
			VectorCopy(origin, cubemap->origin);
			cubemap->parallaxRadius = parallaxRadius;
			numCubemaps++;
		}
	}
}

void R_AssignCubemapsToWorldSurfaces(world_t *worldData)
{
	world_t	*w;
	int i;

	w = worldData;

	for (i = 0; i < w->numsurfaces; i++)
	{
		msurface_t *surf = &w->surfaces[i];
		vec3_t surfOrigin;

		if (surf->cullinfo.type & CULLINFO_SPHERE)
		{
			VectorCopy(surf->cullinfo.localOrigin, surfOrigin);
		}
		else if (surf->cullinfo.type & CULLINFO_BOX)
		{
			surfOrigin[0] = (surf->cullinfo.bounds[0][0] + surf->cullinfo.bounds[1][0]) * 0.5f;
			surfOrigin[1] = (surf->cullinfo.bounds[0][1] + surf->cullinfo.bounds[1][1]) * 0.5f;
			surfOrigin[2] = (surf->cullinfo.bounds[0][2] + surf->cullinfo.bounds[1][2]) * 0.5f;
		}
		else
		{
			//ri.Printf(PRINT_ALL, "surface %d has no cubemap\n", i);
			continue;
		}

		surf->cubemapIndex = R_CubemapForPoint(surfOrigin);
		//ri.Printf(PRINT_ALL, "surface %d has cubemap %d\n", i, surf->cubemapIndex);
	}
}

void R_LoadCubemaps(world_t *world)
{
	int i;

	for (i = 0; i < tr.numCubemaps; i++)
	{
		char filename[MAX_QPATH];
		cubemap_t *cubemap = &tr.cubemaps[i];

		Com_sprintf(filename, MAX_QPATH, "cubemaps/%s/%03d.dds", world->baseName, i);

		cubemap->image = R_FindImageFile(filename, IMGTYPE_COLORALPHA, IMGFLAG_CLAMPTOEDGE | IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE | IMGFLAG_CUBEMAP);
	}
}

void R_RenderMissingCubemaps()
{
	GLenum cubemapFormat = GL_RGBA8;

	if (r_hdr->integer)
	{
		cubemapFormat = GL_RGBA16F;
	}

	tr.skyboxCubemapped = qfalse;

	if (!tr.skyboxCubemap.image)
	{
		tr.skyboxCubemap.image = R_CreateImage("*skyboxCubemap", NULL, r_cubemapSize->integer, r_cubemapSize->integer, 0, IMGTYPE_COLORALPHA, IMGFLAG_MIPMAP | IMGFLAG_CUBEMAP, cubemapFormat);
		for (int j = 0; j < 6; j++)
		{
			RE_ClearScene();
			R_RenderCubemapSide(&tr.skyboxCubemap, j, qfalse, qfalse);
			R_IssuePendingRenderCommands();
			R_InitNextFrame();
		}
		tr.skyboxCubemapped = qtrue;
	}

	if (tr.cubemaps[0].image)
	{
		return;
	}

	int numberOfBounces = 2;
	for (int k = 0; k <= numberOfBounces; k++)
	{
		qboolean bounce = qboolean(k != 0);
		for (int i = 0; i < tr.numCubemaps; i++)
		{
			if (!bounce)
				tr.cubemaps[i].image = r_cubeMapping->integer > 1 ?
										R_CreateImage(
											va("*cubeMap%d", i), 
											NULL, 
											r_cubemapSize->integer * 4, 
											r_cubemapSize->integer * 2, 
											0, 
											IMGTYPE_COLORALPHA, 
											IMGFLAG_MIPMAP, 
											cubemapFormat) :
										R_CreateImage(
											va("*cubeMap%d", i), 
											NULL, 
											r_cubemapSize->integer, 
											r_cubemapSize->integer, 
											0, 
											IMGTYPE_COLORALPHA, 
											IMGFLAG_CLAMPTOEDGE | IMGFLAG_MIPMAP | IMGFLAG_CUBEMAP, 
											cubemapFormat);

			for (int j = 0; j < 6; j++)
			{
				RE_ClearScene();
				R_RenderCubemapSide(&tr.cubemaps[i], j, qfalse, bounce);
				R_IssuePendingRenderCommands();
				R_InitNextFrame();
			}

			if (r_cubeMapping->integer > 1)
			{
				RE_ClearScene();
				R_AddProjectCubemapCmd(&tr.cubemaps[i]);
				//R_AddConvolveCubemapCmd(&tr.cubemaps[i], 0);
				R_IssuePendingRenderCommands();
				R_InitNextFrame();
			}
			else
			{
				for (int j = 0; j < 6; j++)
				{
					RE_ClearScene();
					R_AddConvolveCubemapCmd(&tr.cubemaps[i], j);
					R_IssuePendingRenderCommands();
					R_InitNextFrame();
				}
			}
		}
	}
}


/*
=================
R_MergeLeafSurfaces

Merges surfaces that share a common leaf
=================
*/
static void R_MergeLeafSurfaces(world_t *worldData)
{
	int i, j, k;
	int numWorldSurfaces;
	int mergedSurfIndex;
	int numMergedSurfaces;
	int numUnmergedSurfaces;
	VBO_t *vbo;
	IBO_t *ibo;

	msurface_t *mergedSurf;

	glIndex_t *iboIndexes, *outIboIndexes;
	int numIboIndexes;

	int startTime, endTime;

	startTime = ri.Milliseconds();

	numWorldSurfaces = worldData->numWorldSurfaces;

	// use viewcount to keep track of mergers
	for (i = 0; i < numWorldSurfaces; i++)
	{
		worldData->surfacesViewCount[i] = -1;
	}

	// mark matching surfaces
	for (i = 0; i < worldData->numnodes - worldData->numDecisionNodes; i++)
	{
		mnode_t *leaf = worldData->nodes + worldData->numDecisionNodes + i;

		for (j = 0; j < leaf->nummarksurfaces; j++)
		{
			msurface_t *surf1;
			shader_t *shader1;
			int fogIndex1;
			int cubemapIndex1;
			int surfNum1;

			surfNum1 = *(worldData->marksurfaces + leaf->firstmarksurface + j);

			if (worldData->surfacesViewCount[surfNum1] != -1)
				continue;

			surf1 = worldData->surfaces + surfNum1;

			if ((*surf1->data != SF_GRID) && 
				(*surf1->data != SF_TRIANGLES) && 
				(*surf1->data != SF_FACE))
				continue;

			shader1 = surf1->shader;

			if(shader1->isSky)
				continue;

			if(shader1->isPortal)
				continue;

			if(ShaderRequiresCPUDeforms(shader1))
				continue;

			fogIndex1 = surf1->fogIndex;
			cubemapIndex1 = surf1->cubemapIndex;

			worldData->surfacesViewCount[surfNum1] = surfNum1;

			for (k = j + 1; k < leaf->nummarksurfaces; k++)
			{
				msurface_t *surf2;
				shader_t *shader2;
				int fogIndex2;
				int cubemapIndex2;
				int surfNum2;

				surfNum2 = *(worldData->marksurfaces + leaf->firstmarksurface + k);

				if (worldData->surfacesViewCount[surfNum2] != -1)
					continue;
				
				surf2 = worldData->surfaces + surfNum2;

				if ((*surf2->data != SF_GRID) && (*surf2->data != SF_TRIANGLES) && (*surf2->data != SF_FACE))
					continue;

				shader2 = surf2->shader;

				if (shader1 != shader2)
					continue;

				fogIndex2 = surf2->fogIndex;

				if (fogIndex1 != fogIndex2)
					continue;

				cubemapIndex2 = surf2->cubemapIndex;

				if (cubemapIndex1 != cubemapIndex2)
					continue;

				worldData->surfacesViewCount[surfNum2] = surfNum1;
			}
		}
	}

	// don't add surfaces that don't merge to any others to the merged list
	for (i = 0; i < numWorldSurfaces; i++)
	{
		qboolean merges = qfalse;

		if (worldData->surfacesViewCount[i] != i)
			continue;

		for (j = 0; j < numWorldSurfaces; j++)
		{
			if (j == i)
				continue;

			if (worldData->surfacesViewCount[j] == i)
			{
				merges = qtrue;
				break;
			}
		}

		if (!merges)
			worldData->surfacesViewCount[i] = -1;
	}	

	// count merged/unmerged surfaces
	numMergedSurfaces = 0;
	numUnmergedSurfaces = 0;
	for (i = 0; i < numWorldSurfaces; i++)
	{
		if (worldData->surfacesViewCount[i] == i)
		{
			numMergedSurfaces++;
		}
		else if (worldData->surfacesViewCount[i] == -1)
		{
			numUnmergedSurfaces++;
		}
	}

	// Allocate merged surfaces
	worldData->mergedSurfaces = (msurface_t *)R_Hunk_Alloc(sizeof(*worldData->mergedSurfaces) * numMergedSurfaces, qtrue);
	worldData->mergedSurfacesViewCount = (int *)R_Hunk_Alloc(sizeof(*worldData->mergedSurfacesViewCount) * numMergedSurfaces, qtrue);
	worldData->mergedSurfacesDlightBits = (int *)R_Hunk_Alloc(sizeof(*worldData->mergedSurfacesDlightBits) * numMergedSurfaces, qtrue);
	worldData->mergedSurfacesPshadowBits = (int *)R_Hunk_Alloc(sizeof(*worldData->mergedSurfacesPshadowBits) * numMergedSurfaces, qtrue);
	worldData->numMergedSurfaces = numMergedSurfaces;
	
	// view surfaces are like mark surfaces, except negative ones represent merged surfaces
	// -1 represents 0, -2 represents 1, and so on
	worldData->viewSurfaces = (int *)R_Hunk_Alloc(sizeof(*worldData->viewSurfaces) * worldData->nummarksurfaces, qtrue);

	// copy view surfaces into mark surfaces
	for (i = 0; i < worldData->nummarksurfaces; i++)
	{
		worldData->viewSurfaces[i] = worldData->marksurfaces[i];
	}

	// need to be synched here
	R_IssuePendingRenderCommands();

	// actually merge surfaces
	numIboIndexes = 0;
	mergedSurfIndex = 0;
	mergedSurf = worldData->mergedSurfaces;
	for (i = 0; i < numWorldSurfaces; i++)
	{
		msurface_t *surf1;

		vec3_t bounds[2];

		int numSurfsToMerge;
		int numIndexes;
		int numVerts;
		int firstIndex;

		srfBspSurface_t *vboSurf;

		if (worldData->surfacesViewCount[i] != i)
			continue;

		surf1 = worldData->surfaces + i;

		// retrieve vbo
		vbo = ((srfBspSurface_t *)(surf1->data))->vbo;

		// count verts, indexes, and surfaces
		numSurfsToMerge = 0;
		numIndexes = 0;
		numVerts = 0;
		for (j = i; j < numWorldSurfaces; j++)
		{
			msurface_t *surf2;
			srfBspSurface_t *bspSurf;

			if (worldData->surfacesViewCount[j] != i)
				continue;

			surf2 = worldData->surfaces + j;

			bspSurf = (srfBspSurface_t *) surf2->data;
			numIndexes += bspSurf->numIndexes;
			numVerts += bspSurf->numVerts;
			numSurfsToMerge++;
		}

		if (numVerts == 0 || numIndexes == 0 || numSurfsToMerge < 2)
		{
			continue;
		}

		// create ibo
		ibo = tr.ibos[tr.numIBOs++] = (IBO_t*)R_Hunk_Alloc(sizeof(*ibo), qtrue);
		memset(ibo, 0, sizeof(*ibo));
		numIboIndexes = 0;

		// allocate indexes
   		iboIndexes = outIboIndexes = (glIndex_t*)R_Malloc(numIndexes * sizeof(*outIboIndexes), TAG_BSP, qfalse);

		// Merge surfaces (indexes) and calculate bounds
		ClearBounds(bounds[0], bounds[1]);
		firstIndex = numIboIndexes;
		for (j = i; j < numWorldSurfaces; j++)
		{
			msurface_t *surf2;
			srfBspSurface_t *bspSurf;

			if (worldData->surfacesViewCount[j] != i)
				continue;

			surf2 = worldData->surfaces + j;

			AddPointToBounds(surf2->cullinfo.bounds[0], bounds[0], bounds[1]);
			AddPointToBounds(surf2->cullinfo.bounds[1], bounds[0], bounds[1]);

			bspSurf = (srfBspSurface_t *) surf2->data;
			for (k = 0; k < bspSurf->numIndexes; k++)
			{
				*outIboIndexes++ = bspSurf->indexes[k] + bspSurf->firstVert;
				numIboIndexes++;
			}
			break;
		}

		vboSurf = (srfBspSurface_t *)R_Hunk_Alloc(sizeof(*vboSurf), qtrue);
		memset(vboSurf, 0, sizeof(*vboSurf));
		vboSurf->surfaceType = SF_VBO_MESH;

		vboSurf->vbo = vbo;
		vboSurf->ibo = ibo;

		vboSurf->numIndexes = numIndexes;
		vboSurf->numVerts = numVerts;
		vboSurf->firstIndex = firstIndex;

		vboSurf->minIndex = *(iboIndexes + firstIndex);
		vboSurf->maxIndex = *(iboIndexes + firstIndex);

		for (j = 0; j < numIndexes; j++)
		{
			vboSurf->minIndex = MIN(vboSurf->minIndex, *(iboIndexes + firstIndex + j));
			vboSurf->maxIndex = MAX(vboSurf->maxIndex, *(iboIndexes + firstIndex + j));
		}

		VectorCopy(bounds[0], vboSurf->cullBounds[0]);
		VectorCopy(bounds[1], vboSurf->cullBounds[1]);

		VectorCopy(bounds[0], mergedSurf->cullinfo.bounds[0]);
		VectorCopy(bounds[1], mergedSurf->cullinfo.bounds[1]);

		mergedSurf->cullinfo.type = CULLINFO_BOX;
		mergedSurf->data          = (surfaceType_t *)vboSurf;
		mergedSurf->fogIndex      = surf1->fogIndex;
		mergedSurf->cubemapIndex  = surf1->cubemapIndex;
		mergedSurf->shader        = surf1->shader;

		// finish up the ibo
		qglGenBuffers(1, &ibo->indexesVBO);

		R_BindIBO(ibo);
		qglBufferData(GL_ELEMENT_ARRAY_BUFFER, numIboIndexes * sizeof(*iboIndexes), iboIndexes, GL_STATIC_DRAW);
		R_BindNullIBO();

		GL_CheckErrors();

   		R_Free(iboIndexes);

		// redirect view surfaces to this surf
		for (j = 0; j < numWorldSurfaces; j++)
		{
			if (worldData->surfacesViewCount[j] != i)
				continue;

			for (k = 0; k < worldData->nummarksurfaces; k++)
			{
				int *mark = worldData->marksurfaces + k;
				int *view = worldData->viewSurfaces + k;

				if (*mark == j)
					*view = -(mergedSurfIndex + 1);
			}
		}

		mergedSurfIndex++;
		mergedSurf++;
	}

	endTime = ri.Milliseconds();

	ri.Printf(PRINT_ALL, "Processed %d surfaces into %d merged, %d unmerged in %5.2f seconds\n", 
		numWorldSurfaces, numMergedSurfaces, numUnmergedSurfaces, (endTime - startTime) / 1000.0f);

	// reset viewcounts
	for (i = 0; i < numWorldSurfaces; i++)
	{
		worldData->surfacesViewCount[i] = -1;
	}
}


static void R_CalcVertexLightDirs( world_t *worldData )
{
	int i, k;
	msurface_t *surface;

	for(k = 0, surface = &worldData->surfaces[0]; k < worldData->numsurfaces /* worldData->numWorldSurfaces */; k++, surface++)
	{
		srfBspSurface_t *bspSurf = (srfBspSurface_t *) surface->data;

		switch(bspSurf->surfaceType)
		{
			case SF_FACE:
			case SF_GRID:
			case SF_TRIANGLES:
				for(i = 0; i < bspSurf->numVerts; i++)
					R_LightDirForPoint(
							bspSurf->verts[i].xyz,
							bspSurf->verts[i].lightdir,
							bspSurf->verts[i].normal,
							worldData );

				break;

			default:
				break;
		}
	}
}

struct sprite_t
{
	vec3_t position;
	vec3_t normal;
};

static uint32_t UpdateHash( const char *text, uint32_t hash )
{
	for ( int i = 0; text[i]; ++i )
	{
		char letter = tolower((unsigned char)text[i]);
		if (letter == '.') break;				// don't include extension
		if (letter == '\\') letter = '/';		// damn path names
		if (letter == PATH_SEP) letter = '/';		// damn path names

		hash += (uint32_t)(letter)*(i+119);
	}

	return (hash ^ (hash >> 10) ^ (hash >> 20));
}

static std::vector<sprite_t> R_CreateSurfaceSpritesVertexData(const srfBspSurface_t *bspSurf, float density)
{
	const srfVert_t *verts = bspSurf->verts;
	const glIndex_t *indexes = bspSurf->indexes;

	std::vector<sprite_t> sprites;
	sprites.reserve(10000);
	for ( int i = 0, numIndexes = bspSurf->numIndexes; i < numIndexes; i += 3 )
	{
		const srfVert_t *v0 = verts + indexes[i + 0];
		const srfVert_t *v1 = verts + indexes[i + 1];
		const srfVert_t *v2 = verts + indexes[i + 2];

		vec3_t p0, p1, p2;
		VectorCopy(v0->xyz, p0);
		VectorCopy(v1->xyz, p1);
		VectorCopy(v2->xyz, p2);

		vec3_t n0, n1, n2;
		VectorCopy(v0->normal, n0);
		VectorCopy(v1->normal, n1);
		VectorCopy(v2->normal, n2);

		const vec2_t p01 = {p1[0] - p0[0], p1[1] - p0[1]};
		const vec2_t p02 = {p2[0] - p0[0], p2[1] - p0[1]};

		const float zarea = std::fabs(p02[0]*p01[1] - p02[1]*p01[0]);
		if ( zarea <= 1.0 )
		{
			// Triangle's area is too small to consider.
			continue;
		}

		// Generate the positions of the surface sprites.
		//
		// Pick random points inside of each triangle, using barycentric
		// coordinates.
		const float step = density * Q_rsqrt(zarea);
		for ( float a = 0.0f; a < 1.0f; a += step )
		{
			for ( float b = 0.0f, bend = (1.0f - a); b < bend; b += step )
			{
				float x = flrand(0.0f, 1.0f)*step + a;
				float y = flrand(0.0f, 1.0f)*step + b;
				float z = 1.0f - x - y;

				// Ensure we're inside the triangle bounds.
				if ( x > 1.0f ) continue;
				if ( (x + y ) > 1.0f ) continue;

				// Calculate position inside triangle.
				// pos = (((p0*x) + p1*y) + p2*z)
				sprite_t sprite = {};
				VectorMA(sprite.position, x, p0, sprite.position);
				VectorMA(sprite.position, y, p1, sprite.position);
				VectorMA(sprite.position, z, p2, sprite.position);

				// x*x + y*y = 1.0
				// => y*y = 1.0 - x*x
				// => y = -/+sqrt(1.0 - x*x)
				float nx = flrand(-1.0f, 1.0f);
				float ny = std::sqrt(1.0f - nx*nx);
				ny *= irand(0, 1) ? -1 : 1;

				VectorSet(sprite.normal, nx, ny, 0.0f);

				// We have 4 copies for each corner of the quad
				sprites.push_back(sprite);
			}
		}
	}
	return sprites;
}

static void R_GenerateSurfaceSprites(const srfBspSurface_t *bspSurf, const shader_t *shader, const shaderStage_t *stage, srfSprites_t *out)
{
	const surfaceSprite_t *surfaceSprite = stage->ss;
	const textureBundle_t *bundle = &stage->bundle[0];

	uint32_t hash = 0;
	for ( int i = 0; bundle->image[i]; ++i )
		hash = UpdateHash(bundle->image[i]->imgName, hash);

	uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
	std::vector<sprite_t> sprites =
		R_CreateSurfaceSpritesVertexData(bspSurf, surfaceSprite->density);

	out->surfaceType = SF_SPRITES;
	out->sprite = surfaceSprite;
	out->numSprites = sprites.size();
	out->vbo = R_CreateVBO((byte *)sprites.data(),
			sizeof(sprite_t) * sprites.size(), VBO_USAGE_STATIC);

	out->ibo = R_CreateIBO((byte *)indices, sizeof(indices), VBO_USAGE_STATIC);

	// FIXME: Need a better way to handle this.
	out->shader = R_CreateShaderFromTextureBundle(va("*ss_%08x\n", hash),
			bundle, stage->stateBits);
	out->shader->cullType = shader->cullType;
	out->shader->stages[0]->glslShaderGroup = tr.spriteShader;
	out->shader->stages[0]->alphaTestCmp = stage->alphaTestCmp;
	out->shader->sort = SS_OPAQUE;

	out->numAttributes = 2;
	out->attributes = (vertexAttribute_t *)R_Hunk_Alloc(
			sizeof(vertexAttribute_t) * out->numAttributes, qtrue);

	out->attributes[0].vbo = out->vbo;
	out->attributes[0].index = ATTR_INDEX_POSITION;
	out->attributes[0].numComponents = 3;
	out->attributes[0].integerAttribute = qfalse;
	out->attributes[0].type = GL_FLOAT;
	out->attributes[0].normalize = GL_FALSE;
	out->attributes[0].stride = sizeof(sprite_t);
	out->attributes[0].offset = offsetof(sprite_t, position);
	out->attributes[0].stepRate = 1;

	out->attributes[1].vbo = out->vbo;
	out->attributes[1].index = ATTR_INDEX_NORMAL;
	out->attributes[1].numComponents = 3;
	out->attributes[1].integerAttribute = qfalse;
	out->attributes[1].type = GL_FLOAT;
	out->attributes[1].normalize = GL_FALSE;
	out->attributes[1].stride = sizeof(sprite_t);
	out->attributes[1].offset = offsetof(sprite_t, normal);
	out->attributes[1].stepRate = 1;
}

static void R_GenerateSurfaceSprites( const world_t *world )
{
	msurface_t *surfaces = world->surfaces;
	for ( int i = 0, numSurfaces = world->numsurfaces; i < numSurfaces; ++i )
	{
		msurface_t *surf = surfaces + i;
		const srfBspSurface_t *bspSurf = (srfBspSurface_t *)surf->data;
		switch ( bspSurf->surfaceType )
		{
			case SF_FACE:
			case SF_GRID:
			case SF_TRIANGLES:
			{
				const shader_t *shader = surf->shader;
				if ( !shader->numSurfaceSpriteStages )
					continue;

				surf->numSurfaceSprites = shader->numSurfaceSpriteStages;
				surf->surfaceSprites = (srfSprites_t *)R_Hunk_Alloc(
						sizeof(srfSprites_t) * surf->numSurfaceSprites, qtrue);

				int surfaceSpriteNum = 0;
				for ( int j = 0, numStages = shader->numUnfoggedPasses;
						j < numStages; ++j )
				{
					const shaderStage_t *stage = shader->stages[j];
					if ( !stage )
						break;

					if ( !stage->ss || stage->ss->type == SURFSPRITE_NONE )
						continue;

					srfSprites_t *sprite = surf->surfaceSprites + surfaceSpriteNum;
					R_GenerateSurfaceSprites(bspSurf, shader, stage, sprite);
					++surfaceSpriteNum;
				}
				break;
			}

			default:
				break;
		}
	}
}

static void R_BuildLightGridTextures(world_t *world)
{
	// Upload light grid as 3D textures
	byte *ambientBase = (byte *)R_Malloc(world->numGridArrayElements * sizeof(byte) * 4, TAG_TEMP_WORKSPACE, qtrue);
	byte *directionalBase = (byte *)R_Malloc(world->numGridArrayElements * sizeof(byte) * 4, TAG_TEMP_WORKSPACE, qtrue);
	byte *directionBase = (byte *)R_Malloc(world->numGridArrayElements * sizeof(byte) * 4, TAG_TEMP_WORKSPACE, qtrue);

	if (world->lightGridData)
	{
		byte *ambient = ambientBase;
		byte *directional = directionalBase;
		byte *direction = directionBase;
		for (int i = 0; i < world->numGridArrayElements; i++)
		{

			float lat, lng;
			float clat, slong, slat, clong;

			mgrid_t *data = world->lightGridData + world->lightGridArray[i];

			ambient[0] = data->ambientLight[0][0];
			ambient[1] = data->ambientLight[0][1];
			ambient[2] = data->ambientLight[0][2];
			ambient[3] = 0;

			directional[0] = data->directLight[0][0];
			directional[1] = data->directLight[0][1];
			directional[2] = data->directLight[0][2];
			directional[3] = 0;

			lat = (data->latLong[1] / 255.0f) * 2.0f * M_PI;
			lng = (data->latLong[0] / 255.0f) * 2.0f * M_PI;

			// decode X as cos( lat ) * sin( long )
			// decode Y as sin( lat ) * sin( long )
			// decode Z as cos( long )

			slat = sinf(lat);
			clat = cosf(lat);
			slong = sinf(lng);
			clong = cosf(lng);

			direction[0] = (byte)floorf(clat * slong);
			direction[1] = (byte)floorf(slat * slong);
			direction[2] = (byte)floorf(clong);
			direction[3] = 0;

			ambient += 4;
			directional += 4;
			direction += 4;
		}

		world->ambientLightImages[0] = R_CreateImage3D(
			"*bsp_ambientLightGrid", ambientBase,
			world->lightGridBounds[0],
			world->lightGridBounds[1],
			world->lightGridBounds[2],
			GL_RGB8);

		world->directionalLightImages[0] = R_CreateImage3D(
			"*bsp_directionalLightGrid", directionalBase,
			world->lightGridBounds[0],
			world->lightGridBounds[1],
			world->lightGridBounds[2],
			GL_RGB8);

		world->directionImages = R_CreateImage3D(
			"*bsp_directionsGrid", directionBase,
			world->lightGridBounds[0],
			world->lightGridBounds[1],
			world->lightGridBounds[2],
			GL_RGB8);
	}

	R_Free(ambientBase);
	R_Free(directionalBase);
	R_Free(directionBase);

	if (world->numGridArrayElements && world->lightGridData)
	{
		const float stepSize = 1;
		int numSphericalHarmonics = world->numGridArrayElements / stepSize;

		if (!numSphericalHarmonics)
			return;

		vec3_t *positions = (vec3_t *)R_Malloc(numSphericalHarmonics * sizeof(vec3_t), TAG_TEMP_WORKSPACE, qtrue);
		
		numSphericalHarmonics = 0;
		for (int x = 0; x < world->lightGridBounds[0] / stepSize; x++)
		{
			for (int y = 0; y < world->lightGridBounds[1] / stepSize; y++)
			{
				for (int z = 0; z < world->lightGridBounds[2] / stepSize; z++)
				{
					mgrid_t	*data;
					vec3_t	origin;
					int		pos[3];

					float posX = world->lightGridOrigin[0] + (x * world->lightGridSize[0] * stepSize);
					float posY = world->lightGridOrigin[1] + (y * world->lightGridSize[1] * stepSize);
					float posZ = world->lightGridOrigin[2] + (z * world->lightGridSize[2] * stepSize);

					VectorSet(origin, posX, posY, posZ);

					int		gridStep[3];
					gridStep[0] = 1;
					gridStep[1] = 1 * world->lightGridBounds[0];
					gridStep[2] = 1 * world->lightGridBounds[0] * world->lightGridBounds[1];

					VectorSubtract(origin, world->lightGridOrigin, origin);
					for (int i = 0; i < 3; i++) {
						pos[i] = floor(origin[i] * world->lightGridInverseSize[i]);
						if (pos[i] < 0) {
							pos[i] = 0;
						}
						else if (pos[i] >= world->lightGridBounds[i] - 1) {
							pos[i] = world->lightGridBounds[i] - 1;
						}
					}

					unsigned short	*startGridPos = 
						world->lightGridArray + 
						(
						pos[0] * gridStep[0] +
						pos[1] * gridStep[1] +
						pos[2] * gridStep[2]
						);

					data = world->lightGridData + *startGridPos;

					if (data->styles[0] == LS_NONE)
					{
						continue;	// ignore samples in walls
					}

					VectorSet(origin, posX, posY, posZ);
					VectorCopy(origin, positions[numSphericalHarmonics]);
					numSphericalHarmonics++;
				}
			}
		}
		tr.numSphericalHarmonics = numSphericalHarmonics;

		tr.sphericalHarmonicsCoefficients = (sphericalHarmonic_t *)R_Hunk_Alloc(numSphericalHarmonics * sizeof(*tr.sphericalHarmonicsCoefficients), qtrue);

		for (int i = 0; i < numSphericalHarmonics; i++)
		{
			VectorCopy(positions[i], tr.sphericalHarmonicsCoefficients[i].origin);
		}

		R_Free(positions);
		tr.numfinishedSphericalHarmonics = 0;
	}
	
	ri.Printf(PRINT_DEVELOPER, "Found %i positions for sphericalHarmonics\n", tr.numSphericalHarmonics);
	return;
}

world_t *R_LoadBSP(const char *name, int *bspIndex)
{
	union {
		byte *b;
		void *v;
	} buffer;

	world_t *worldData;
	int worldIndex = -1;
	if (bspIndex == nullptr)
	{
		worldData = &s_worldData;
	}
	else
	{
		if (tr.numBspModels >= MAX_SUB_BSP)
		{
			// too many
			return nullptr;
		}

		worldIndex = *bspIndex = tr.numBspModels;
		worldData = tr.bspModels[tr.numBspModels];
		++tr.numBspModels;
	}

	// load it
	ri.FS_ReadFile(name, &buffer.v);
	if (!buffer.b)
	{
		if (bspIndex == nullptr)
		{
			ri.Error(ERR_DROP, "RE_LoadWorldMap: %s not found", name);
		}

		return nullptr;
	}

	Com_Memset(worldData, 0, sizeof(*worldData));
	Q_strncpyz(worldData->name, name, sizeof(worldData->name));
	Q_strncpyz(worldData->baseName, COM_SkipPath(worldData->name), sizeof(worldData->name));

	COM_StripExtension(worldData->baseName, worldData->baseName, sizeof(worldData->baseName));

	byte *startMarker = (byte *)R_Hunk_Alloc(0, qtrue);
	dheader_t *header = (dheader_t *)buffer.b;
	fileBase = (byte *)header;

	int bspVersion = LittleLong(header->version);
	if (bspVersion != BSP_VERSION)
	{
		ri.Error(
			ERR_DROP,
			"R_LoadBSP: %s has wrong version number (%i should be %i)",
			name,
			bspVersion,
			BSP_VERSION);
	}

	// swap all the lumps
	for (int i = 0; i < sizeof(dheader_t) / 4; ++i)
	{
		((int *)header)[i] = LittleLong(((int *)header)[i]);
	}

	// load into heap
	R_LoadEntities(worldData, &header->lumps[LUMP_ENTITIES]);
	R_LoadShaders(worldData, &header->lumps[LUMP_SHADERS]);
	R_LoadLightmaps(
		worldData,
		&header->lumps[LUMP_LIGHTMAPS],
		&header->lumps[LUMP_SURFACES]);
	R_LoadPlanes(worldData, &header->lumps[LUMP_PLANES]);
	R_LoadFogs(
		worldData,
		&header->lumps[LUMP_FOGS],
		&header->lumps[LUMP_BRUSHES],
		&header->lumps[LUMP_BRUSHSIDES]);
	R_LoadSurfaces(
		worldData,
		&header->lumps[LUMP_SURFACES],
		&header->lumps[LUMP_DRAWVERTS],
		&header->lumps[LUMP_DRAWINDEXES]);
	R_LoadMarksurfaces(worldData, &header->lumps[LUMP_LEAFSURFACES]);
	R_LoadNodesAndLeafs(worldData, &header->lumps[LUMP_NODES], &header->lumps[LUMP_LEAFS]);
	R_LoadSubmodels(worldData, worldIndex, &header->lumps[LUMP_MODELS]);
	R_LoadVisibility(worldData, &header->lumps[LUMP_VISIBILITY]);
	R_LoadLightGrid(worldData, &header->lumps[LUMP_LIGHTGRID]);
	R_LoadLightGridArray(worldData, &header->lumps[LUMP_LIGHTARRAY]);

	R_BuildLightGridTextures(worldData);

	R_GenerateSurfaceSprites(worldData);

	// determine vertex light directions
	R_CalcVertexLightDirs(worldData);

	// load cubemaps
	if (r_cubeMapping->integer)
	{
		R_LoadEnvironmentJson(worldData->baseName);

		if (!tr.numCubemaps)
		{
			// use cubemap entities as cubemaps
			R_LoadCubemapEntities("misc_cubemap");
		}
		if (!tr.numCubemaps)
		{
			// use deathmatch spawn points as cubemaps
			R_LoadCubemapEntities("info_player_deathmatch");
		}

		if (!tr.numCubemaps)
		{
			// use spawn points as cubemaps
			R_LoadCubemapEntities("info_player_start");
		}

		R_LoadCubemapEntities("misc_skyportal");

		if (tr.numCubemaps)
		{
			R_AssignCubemapsToWorldSurfaces(worldData);
		}
	}

	// create static VBOS from the world
	R_CreateWorldVBOs(worldData);
	if (r_mergeLeafSurfaces->integer)
	{
		R_MergeLeafSurfaces(worldData);
	}

	worldData->dataSize = (byte *)R_Hunk_Alloc(0, qtrue) - startMarker;

	// make sure the VBO glState entries are safe
	R_BindNullVBO();
	R_BindNullIBO();

	ri.FS_FreeFile(buffer.v);

	return worldData;
}

/*
=================
RE_LoadWorldMap

Called directly from cgame
=================
*/
void RE_LoadWorldMap( const char *name ) {
	if (tr.worldMapLoaded)
	{
		ri.Error(ERR_DROP, "ERROR: attempted to redundantly load world map");
	}

	// set default map light scale
	tr.mapLightScale = 1.0f;
	tr.sunShadowScale = 0.5f;

	// clear the skyboxportal marker
	skyboxportal = qfalse;

	// set default sun direction to be used if it isn't
	// overridden by a shader
	tr.sunDirection[0] = 0.45f;
	tr.sunDirection[1] = 0.3f;
	tr.sunDirection[2] = 0.9f;

	VectorNormalize(tr.sunDirection);

	// set default autoexposure settings
	tr.autoExposureMinMax[0] = -2.0f;
	tr.autoExposureMinMax[1] = 2.0f;

	// set default tone mapping settings
	tr.toneMinAvgMaxLevel[0] = -8.0f;
	tr.toneMinAvgMaxLevel[1] = -2.0f;
	tr.toneMinAvgMaxLevel[2] = 0.0f;

	world_t *world = R_LoadBSP(name);
	if (world == nullptr)
	{
		// clear tr.world so the next/ try will not look at the partially
		// loaded version
		tr.world = nullptr;
		return;
	}

	tr.worldMapLoaded = qtrue;
	tr.world = world;

	R_InitWeatherForMap();

	// Render all cubemaps
	if (r_cubeMapping->integer && tr.numCubemaps)
	{
		R_LoadCubemaps(tr.world);
		R_RenderMissingCubemaps();
	}
}
