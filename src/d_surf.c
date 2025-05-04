// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

// d_surf.c: rasterization driver surface heap manager

#include "quakedef.h"
#include "d_local.h"
#include "r_local.h"

#define GUARDSIZE       4

float surfscale;
qboolean r_cache_thrash; // set if surface cache is thrashing
unsigned long sc_size;
surfcache_t *sc_rover, *sc_base;
int lmonly; // render lightmap only, for lit water

int D_SurfaceCacheForRes(int width, int height)
{
	int size;
	if (COM_CheckParm("-surfcachesize")) {
		size = Q_atoi(com_argv[COM_CheckParm("-surfcachesize")+1])*1024;
		return size;
	}
	size = SURFCACHE_SIZE_AT_320X200;
	int pix = width * height;
	if (pix > 64000)
		size += (pix - 64000) * 3;
	return size;
}

void D_ClearCacheGuard()
{
	byte *s = (byte *) sc_base + sc_size;
	for (int i = 0; i < GUARDSIZE; i++)
		s[i] = (byte) i;
}

void D_InitCaches(void *buffer, int size)
{
	Con_Printf("%ik surface cache\n", size / 1024);
	sc_size = size - GUARDSIZE;
	sc_base = (surfcache_t *) buffer;
	sc_rover = sc_base;
	sc_base->next = NULL;
	sc_base->owner = NULL;
	sc_base->size = sc_size;
	D_ClearCacheGuard();
}

void D_FlushCaches(void)
{
	surfcache_t *c;
	if (!sc_base)
		return;
	for (c = sc_base; c; c = c->next)
		if (c->owner)
			*c->owner = NULL;
	sc_rover = sc_base;
	sc_base->next = NULL;
	sc_base->owner = NULL;
	sc_base->size = sc_size;
}

surfcache_t *D_SCAlloc(int width, uintptr_t size)
{
	if ((width < 0) || (width > 256))
		Sys_Error("D_SCAlloc: bad cache width %d\n", width);
	if ((size <= 0) || (size > 0x10000))
		Sys_Error("D_SCAlloc: bad cache size %d\n", size);
	size = (uintptr_t) & ((surfcache_t *) 0)->data[size];
	size = (size + 3) & ~3;
	if (size > sc_size)
		Sys_Error("D_SCAlloc: %i > cache size", size);
	// if there is not size bytes after the rover, reset to the start
	qboolean wrapped_this_time = false;
	if (!sc_rover || (unsigned long)((byte *) sc_rover - (byte *) sc_base)
			> sc_size - size) {
		if (sc_rover) {
			wrapped_this_time = true;
		}
		sc_rover = sc_base;
	}
	// colect and free surfcache_t blocks until rover block is large enough
	surfcache_t *new = sc_rover;
	if (sc_rover->owner)
		*sc_rover->owner = NULL;
	while (new->size < (int)size) {
		sc_rover = sc_rover->next; // free another
		if (!sc_rover)
			Sys_Error("D_SCAlloc: hit the end of memory");
		if (sc_rover->owner)
			*sc_rover->owner = NULL;

		new->size += sc_rover->size;
		new->next = sc_rover->next;
	}
	if (new->size - size > 256) { // create a fragment out of any leftovers
		sc_rover = (surfcache_t *) ((byte *) new + size);
		sc_rover->size = new->size - size;
		sc_rover->next = new->next;
		sc_rover->width = 0;
		sc_rover->owner = NULL;
		new->next = sc_rover;
		new->size = size;
	} else
		sc_rover = new->next;
	new->width = width;
	if (width > 0) // DEBUG
		new->height = (size - sizeof(*new) + sizeof(new->data)) / width;
	new->owner = NULL; // should be set properly after return
	if (d_roverwrapped && (wrapped_this_time || sc_rover >=d_initial_rover))
			r_cache_thrash = true;
	else if (wrapped_this_time)
		d_roverwrapped = true;
	return new;
}

surfcache_t *D_CacheSurface(msurface_t *surface, int miplevel)
{
	// if the surface is animating or flashing, flush the cache
	r_drawsurf.texture = R_TextureAnimation(surface->texinfo->texture);
	r_drawsurf.lightadj[0] = d_lightstylevalue[surface->styles[0]];
	r_drawsurf.lightadj[1] = d_lightstylevalue[surface->styles[1]];
	r_drawsurf.lightadj[2] = d_lightstylevalue[surface->styles[2]];
	r_drawsurf.lightadj[3] = d_lightstylevalue[surface->styles[3]];
	// see if the cache holds apropriate data
	surfcache_t *cache = surface->cachespots[miplevel];
	if (cache && !cache->dlight && surface->dlightframe != r_framecount
	    && cache->texture == r_drawsurf.texture
	    && cache->lightadj[0] == r_drawsurf.lightadj[0]
	    && cache->lightadj[1] == r_drawsurf.lightadj[1]
	    && cache->lightadj[2] == r_drawsurf.lightadj[2]
	    && cache->lightadj[3] == r_drawsurf.lightadj[3])
		return cache;
	surfscale = 1.0 / (1 << miplevel); // determine shape of surface
	r_drawsurf.surfmip = miplevel;
	r_drawsurf.surfwidth = surface->extents[0] >> miplevel;
	r_drawsurf.rowbytes = r_drawsurf.surfwidth;
	r_drawsurf.surfheight = surface->extents[1] >> miplevel;
	// allocate memory if needed
	if (!cache) // if a texture just animated, don't reallocate it
	{
		cache = D_SCAlloc(r_drawsurf.surfwidth,
				  r_drawsurf.surfwidth * r_drawsurf.surfheight);
		surface->cachespots[miplevel] = cache;
		cache->owner = &surface->cachespots[miplevel];
		cache->mipscale = surfscale;
	}
	cache->dlight = (surface->dlightframe == r_framecount);
	r_drawsurf.surfdat = (pixel_t *) cache->data;
	cache->texture = r_drawsurf.texture;
	cache->lightadj[0] = r_drawsurf.lightadj[0];
	cache->lightadj[1] = r_drawsurf.lightadj[1];
	cache->lightadj[2] = r_drawsurf.lightadj[2];
	cache->lightadj[3] = r_drawsurf.lightadj[3];
	r_drawsurf.surf = surface; // draw and light the surface texture
	c_surf++;
	R_DrawSurface();
	return surface->cachespots[miplevel];
}
