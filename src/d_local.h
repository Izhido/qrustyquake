// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

// d_local.h: private rasterization driver defs

#include "r_shared.h"

// TODO: fine-tune this; it's based on providing some overage even if there
// is a 2k-wide scan, with subdivision every 8, for 256 spans of 12 bytes each
#define SCANBUFFERPAD 0x1000
#define R_SKY_SMASK 0x007F0000
#define R_SKY_TMASK 0x007F0000
#define DS_SPAN_LIST_END -128
#define SKYBOX_MAX_SIZE 1024
#define SURFCACHE_SIZE_AT_320X200 3000*1024 // CyanBun96: was 600*1024
#define FOG_LUT_LEVELS 32

typedef struct surfcache_s
{
	struct surfcache_s *next;
	struct surfcache_s **owner; // NULL is an empty chunk of memory
	int lightadj[MAXLIGHTMAPS]; // checked for strobe flush
	int dlight;
	int size; // including header
	unsigned width;
	unsigned height; // DEBUG only needed for debug
	float mipscale;
	struct texture_s *texture; // checked for animating textures
	byte data[4]; // width*height elements
} surfcache_t;

typedef struct sspan_s
{
	int u, v, count;
} sspan_t;

extern float scale_for_mip;
extern qboolean d_roverwrapped;
extern surfcache_t *sc_rover;
extern surfcache_t *d_initial_rover;
extern float d_sdivzstepu, d_tdivzstepu, d_zistepu;
extern float d_sdivzstepv, d_tdivzstepv, d_zistepv;
extern float d_sdivzorigin, d_tdivzorigin, d_ziorigin;
extern fixed16_t sadjust, tadjust;
extern fixed16_t bbextents, bbextentt;
extern short *d_pzbuffer;
extern unsigned int d_zrowbytes, d_zwidth;
extern int *d_pscantable;
extern int d_scantable[MAXHEIGHT];
extern int d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;
extern int d_y_aspect_shift, d_pix_min, d_pix_max, d_pix_shift;
extern pixel_t *d_viewbuffer;
extern short *zspantable[MAXHEIGHT];
extern int d_minmip;
extern float d_scalemip[3];
extern int r_pass;
extern float winquake_surface_liquid_alpha;
extern cvar_t r_wateralpha;
extern cvar_t r_slimealpha;
extern cvar_t r_lavaalpha;
extern cvar_t r_telealpha;
extern cvar_t r_twopass;
extern cvar_t fog;
extern cvar_t r_entalpha;
extern cvar_t r_alphastyle;
extern unsigned char color_mix_lut[256][256][FOG_LUT_LEVELS];
extern unsigned char fog_pal_index;
extern int lmonly;
extern unsigned char *litwater_base;
extern int lwmark;

void D_DrawSpans8(espan_t *pspans);
void D_DrawTransSpans8(espan_t *pspans, float opacity);
void D_DrawZSpans(espan_t *pspans);
void Turbulent8(espan_t *pspan, float opacity);
void D_SpriteDrawSpans(sspan_t *pspan);
void D_DrawSkyScans8(espan_t *pspan);
void D_DrawSkyboxScans8(espan_t *pspans);
void R_ShowSubDiv();
extern void(*prealspandrawer)();
surfcache_t *D_CacheSurface(msurface_t *surface, int miplevel);
extern int D_MipLevelForScale(float scale);
extern int dither_pat;
extern int D_Dither(byte *pos);
