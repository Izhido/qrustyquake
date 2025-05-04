// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

// r_local.h -- private refresh defs

#include "r_shared.h"

#define ALIAS_BASE_SIZE_RATIO (1.0 / 11.0) // normalizing factor so player model
	// works out to about 1 pixel per triangle
#define BMODEL_FULLY_CLIPPED 0x10 // value returned by R_BmodelCheckBBox () if
	// bbox is trivially rejected
#define XCENTERING (1.0 / 2.0)
#define YCENTERING (1.0 / 2.0)
#define CLIP_EPSILON 0.001
#define BACKFACE_EPSILON 0.01
#define DIST_NOT_SET 98765
#define NEAR_CLIP 0.01
#define ALIAS_Z_CLIP_PLANE 0.1 // CyanBun96: was 5, lowered for high FOV guns
#define MAX_BTOFPOLYS 5000 // FIXME: tune this
#define AMP 8*0x10000 // turbulence stuff
#define AMP2 3
#define SPEED 20
#define LIT_LUT_RES 64
#define QUANT(x) (((x) * (LIT_LUT_RES - 1)) / 255)

typedef struct { // viewmodel lighting
	int ambientlight;
	int shadelight;
	float *plightvec;
} alight_t;

typedef struct bedge_s // clipped bmodel edges
{
	mvertex_t *v[2];
	struct bedge_s *pnext;
} bedge_t;

typedef struct {
	float fv[3]; // viewspace x, y
} auxvert_t;

typedef struct clipplane_s
{
	vec3_t normal;
	float dist;
	struct clipplane_s *next;
	byte leftedge;
	byte rightedge;
	byte reserved[2];
} clipplane_t;

typedef struct btofpoly_s {
	int clipflags;
	msurface_t *psurf;
} btofpoly_t;

extern cvar_t r_draworder;
extern cvar_t r_speeds;
extern cvar_t r_timegraph;
extern cvar_t r_graphheight;
extern cvar_t r_clearcolor;
extern cvar_t r_waterwarp;
extern cvar_t r_fullbright;
extern cvar_t r_drawentities;
extern cvar_t r_aliasstats;
extern cvar_t r_dspeeds;
extern cvar_t r_drawflat;
extern cvar_t r_ambient;
extern cvar_t r_reportsurfout;
extern cvar_t r_maxsurfs;
extern cvar_t r_numsurfs;
extern cvar_t r_reportedgeout;
extern cvar_t r_maxedges;
extern cvar_t r_numedges;
extern clipplane_t view_clipplanes[4];
extern mplane_t screenedge[4];
extern vec3_t r_origin;
extern vec3_t r_entorigin;
extern float screenAspect;
extern float verticalFieldOfView;
extern float xOrigin, yOrigin;
extern unsigned int r_visframecount;
extern int vstartscan;
extern qboolean insubmodel; // current entity info
extern vec3_t r_worldmodelorg; // current entity info
extern int c_faceclip;
extern int r_polycount;
extern int r_wholepolycount;
extern model_t *cl_worldmodel;
extern int *pfrustum_indexes[4];
extern int ubasestep, errorterm, erroradjustup, erroradjustdown;
extern int vstartscan;
extern fixed16_t sadjust, tadjust;
extern fixed16_t bbextents, bbextentt;
extern mvertex_t *r_ptverts, *r_ptvertsmax;
extern vec3_t sbaseaxis[3], tbaseaxis[3];
extern float entity_rotation[3][3];
extern int reinit_surfcache;
extern int r_currentkey;
extern int r_currentbkey;
extern int numbtofpolys;
extern btofpoly_t *pbtofpolys;
extern int numverts;
extern int a_skinwidth;
extern mtriangle_t *ptriangles;
extern int numtriangles;
extern aliashdr_t *paliashdr;
extern mdl_t *pmdl;
extern float leftclip, topclip, rightclip, bottomclip;
extern int r_acliptype;
extern finalvert_t *pfinalverts;
extern auxvert_t *pauxverts;
extern int r_amodels_drawn;
extern edge_t *auxedges;
extern int r_numallocatededges;
extern edge_t *r_edges, *edge_p, *edge_max;
extern edge_t *newedges[MAXHEIGHT];
extern edge_t *removeedges[MAXHEIGHT];
extern int screenwidth;
extern edge_t edge_head; // FIXME: make stack vars when debugging done
extern edge_t edge_tail;
extern edge_t edge_aftertail;
extern int r_bmodelactive;
extern float aliasxscale, aliasyscale, aliasxcenter, aliasycenter;
extern float r_aliastransition, r_resfudge;
extern int r_outofsurfaces;
extern int r_outofedges;
extern mvertex_t *r_pcurrentvertbase;
extern int r_maxvalidedgeoffset;
extern float r_time1;
extern float dp_time1, dp_time2, db_time1, db_time2, rw_time1, rw_time2;
extern float se_time1, se_time2, de_time1, de_time2, dv_time1, dv_time2;
extern int r_frustum_indexes[4*6];
extern int r_maxsurfsseen, r_maxedgesseen, r_cnumsurfs;
extern qboolean r_surfsonstack;
extern cshift_t cshift_water;
extern qboolean r_dowarpold, r_viewchanged;
extern mleaf_t *r_viewleaf, *r_oldviewleaf;
extern vec3_t r_emins, r_emaxs;
extern mnode_t *r_pefragtopnode;
extern int r_clipflags;
extern unsigned int r_dlightframecount;
extern qboolean r_fov_greater_than_90;
extern cvar_t r_wateralpha, r_slimealpha, r_lavaalpha, r_telealpha;
extern int r_pass;
extern cvar_t r_twopass;
extern cvar_t r_entalpha;
extern unsigned char lit_lut[LIT_LUT_RES*LIT_LUT_RES*LIT_LUT_RES];
extern int lit_lut_initialized;
extern int color_lightmap;
extern int lmonly;

void R_DrawSprite();
void R_RenderFace(msurface_t *fa, int clipflags);
void R_RenderBmodelFace(bedge_t *pedges, msurface_t *psurf);
void R_TransformFrustum();
void R_SetSkyFrame();
void R_DrawSurfaceBlock16();
void R_DrawSurfaceBlock8();
texture_t *R_TextureAnimation(texture_t *base);
void R_GenSkyTile(void *pdest);
void R_GenSkyTile16(void *pdest);
void R_DrawSubmodelPolygons(model_t *pmodel, int clipflags);
void R_DrawSolidClippedSubmodelPolygons(model_t *pmodel);
void R_AddPolygonEdges(emitpoint_t *pverts, int numverts, int miplevel);
surf_t *R_GetSurf();
void R_AliasDrawModel(alight_t *plighting);
void R_BeginEdgeFrame();
void R_ScanEdges();
void D_DrawSurfaces();
void R_InsertNewEdges(edge_t *edgestoadd, edge_t *edgelist);
void R_StepActiveU(edge_t *pedge);
void R_RemoveEdges(edge_t *pedge);
extern void R_Surf8Start();
extern void R_Surf8End();
extern void R_Surf16Start();
extern void R_Surf16End();
extern void R_EdgeCodeStart();
extern void R_EdgeCodeEnd();
extern void R_RotateBmodel();
void R_InitTurb();
void R_ZDrawSubmodelPolys(model_t *clmodel);
qboolean R_AliasCheckBBox();
void R_DrawParticles();
void R_InitParticles();
void R_ClearParticles();
void R_ReadPointFile_f();
void R_AliasClipTriangle(mtriangle_t *ptri);
void R_RenderWorld();
void R_StoreEfrags(efrag_t **ppefrag);
void R_TimeRefresh_f();
void R_TimeGraph();
void R_PrintAliasStats();
void R_PrintTimes();
void R_PrintDSpeeds();
void R_AnimateLight();
int R_LightPoint(vec3_t p);
void R_SetupFrame();
void R_cshift_f();
void R_EmitEdge(mvertex_t *pv0, mvertex_t *pv1);
void R_ClipEdge(mvertex_t *pv0, mvertex_t *pv1, clipplane_t *clip);
void R_SplitEntityOnNode2(mnode_t *node);
void R_MarkLights(dlight_t *light, int bit, mnode_t *node);

extern cvar_t r_labmixpal;
extern cvar_t r_rgblighting;
extern unsigned char rgbtoi_lab(unsigned char r, unsigned char g, unsigned char b);
extern unsigned char rgbtoi(unsigned char r, unsigned char g, unsigned char b);
extern void init_color_conv();
extern void R_BuildLitLUT();
