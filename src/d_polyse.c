// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

// d_polyset.c: routines for drawing sets of polygons sharing the same
// texture (used for Alias models)

#include "quakedef.h"
#include "d_local.h"
#include "r_local.h"

// TODO: put in span spilling to shrink list size
#define DPS_MAXSPANS MAXHEIGHT+1 // 1 extra for spanpackage that marks end

typedef struct {
	int quotient;
	int remainder;
} adivtab_t;

typedef struct {
	void *pdest;
	short *pz;
	int count;
	byte *ptex;
	int sfrac, tfrac, light, zi;
} spanpackage_t;

typedef struct {
	int isflattop;
	int numleftedges;
	int *pleftedgevert0;
	int *pleftedgevert1;
	int *pleftedgevert2;
	int numrightedges;
	int *prightedgevert0;
	int *prightedgevert1;
	int *prightedgevert2;
} edgetable;

int r_p0[6], r_p1[6], r_p2[6];
byte *d_pcolormap;
int d_aflatcolor;
int d_xdenom;
edgetable *pedgetable;
// FIXME: some of these can become statics
int a_sstepxfrac, a_tstepxfrac, r_lstepx, a_ststepxwhole;
int r_sstepx, r_tstepx, r_lstepy, r_sstepy, r_tstepy;
int r_zistepx, r_zistepy;
int d_aspancount, d_countextrastep;
spanpackage_t *a_spans;
spanpackage_t *d_pedgespanpackage;
byte *d_pdest, *d_ptex;
short *d_pz;
int d_sfrac, d_tfrac, d_light, d_zi;
int d_ptexextrastep, d_sfracextrastep;
int d_tfracextrastep, d_lightextrastep, d_pdestextrastep;
int d_lightbasestep, d_pdestbasestep, d_ptexbasestep;
int d_sfracbasestep, d_tfracbasestep;
int d_ziextrastep, d_zibasestep;
int d_pzextrastep, d_pzbasestep;
byte *skintable[MAX_LBM_HEIGHT];
int skinwidth;
byte *skinstart;
static int ystart;
float cur_ent_alpha = 1;

extern unsigned char color_mix_lut[256][256][FOG_LUT_LEVELS];
extern int fog_lut_built;
extern void build_color_mix_lut();
extern vec3_t lightcolor;
extern int colored_aliaslight;

static adivtab_t adivtab[32 * 32] = {
#include "adivtab.h"
};

edgetable edgetables[12] = {
	{ 0, 1, r_p0, r_p2, NULL, 2, r_p0, r_p1, r_p2 },
	{ 0, 2, r_p1, r_p0, r_p2, 1, r_p1, r_p2, NULL },
	{ 1, 1, r_p0, r_p2, NULL, 1, r_p1, r_p2, NULL },
	{ 0, 1, r_p1, r_p0, NULL, 2, r_p1, r_p2, r_p0 },
	{ 0, 2, r_p0, r_p2, r_p1, 1, r_p0, r_p1, NULL },
	{ 0, 1, r_p2, r_p1, NULL, 1, r_p2, r_p0, NULL },
	{ 0, 1, r_p2, r_p1, NULL, 2, r_p2, r_p0, r_p1 },
	{ 0, 2, r_p2, r_p1, r_p0, 1, r_p2, r_p0, NULL },
	{ 0, 1, r_p1, r_p0, NULL, 1, r_p1, r_p2, NULL },
	{ 1, 1, r_p2, r_p1, NULL, 1, r_p0, r_p1, NULL },
	{ 1, 1, r_p1, r_p0, NULL, 1, r_p2, r_p0, NULL },
	{ 0, 1, r_p0, r_p2, NULL, 1, r_p0, r_p1, NULL },
};

void D_PolysetDrawSpans8(spanpackage_t * pspanpackage);
void D_PolysetCalcGradients(int skinwidth);
void D_DrawSubdiv();
void D_DrawNonSubdiv();
void D_PolysetRecursiveTriangle(int *p1, int *p2, int *p3);
void D_PolysetSetEdgeTable();
void D_RasterizeAliasPolySmooth();
void D_PolysetScanLeftEdge(int height);

void D_PolysetDraw()
{
	if (r_alphastyle.value == 0 && cur_ent_alpha != 1 && !fog_lut_built)
		build_color_mix_lut();
	if (r_alphastyle.value == 1 && cur_ent_alpha != 1) {
		if (cur_ent_alpha >= 0.83f) dither_pat = 6;
		else if (cur_ent_alpha >= 0.75f) dither_pat = 5;
		else if (cur_ent_alpha >= 0.66f) dither_pat = 4;
		else if (cur_ent_alpha >= 0.50f) dither_pat = 3;
		else if (cur_ent_alpha >= 0.33f) dither_pat = 2;
		else if (cur_ent_alpha >= 0.25f) dither_pat = 1;
		else dither_pat = 0;
	}
	spanpackage_t spans[DPS_MAXSPANS + 1 +
		((CACHE_SIZE - 1) / sizeof(spanpackage_t)) + 1]/*
		__attribute__((aligned(CACHE_SIZE)))*/;
	a_spans = (spanpackage_t *)
		(((uintptr_t) & spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
	if (r_affinetridesc.drawtype)
		D_DrawSubdiv();
	else
		D_DrawNonSubdiv();
}

void D_PolysetDrawFinalVerts(finalvert_t *fv, int numverts)
{
	for (int i = 0; i < numverts; i++, fv++) {
		// valid triangle coordinates for filling can include the bottom and
		// right clip edges, due to the fill rule; these shouldn't be drawn
		if ((fv->v[0] < r_refdef.vrectright) &&
				(fv->v[1] < r_refdef.vrectbottom)) {
			int z = fv->v[5] >> 16;
			short *zbuf = zspantable[fv->v[1]] + fv->v[0];
			if (zbuf < d_pzbuffer || // FIXME properly
				zbuf >= (d_pzbuffer+vid.width*vid.height*2)) {
				printf("ZBUF SEGFAULT 0 %lx\n", zbuf);
				continue;
			}
			if (!(z >= *zbuf))
				continue;
			int pix;
			*zbuf = z;
			pix = skintable[fv->v[3] >> 16][fv->v[2] >> 16];
			if (!r_rgblighting.value || !colored_aliaslight)
				pix = ((byte *) acolormap)[pix + (fv->v[4] & 0xFF00)];
			else {
				pix = ((byte *) acolormap)[pix];
				unsigned char tr = vid_curpal[pix * 3 + 0];
				unsigned char tg = vid_curpal[pix * 3 + 1];
				unsigned char tb = vid_curpal[pix * 3 + 2];
				unsigned char rr = (tr * lightcolor[0]) / 255.0f;
				unsigned char gg = (tg * lightcolor[1]) / 255.0f;
				unsigned char bb = (tb * lightcolor[2]) / 255.0f;
				pix = lit_lut[ QUANT(rr)
					+ QUANT(gg)*LIT_LUT_RES
					+ QUANT(bb)*LIT_LUT_RES*LIT_LUT_RES ];
			}
			if (r_alphastyle.value == 0 && cur_ent_alpha != 1) {
				int curpix = d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]];
				d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]] =
					color_mix_lut[curpix][pix]
					[(int)((1-cur_ent_alpha)*FOG_LUT_LEVELS)];
			}
			else if (r_alphastyle.value == 1 && cur_ent_alpha != 1) {
				if (D_Dither(&d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]]))
					d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]] = pix;
			}
			else
				d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]] = pix;
		}
	}
}

void D_DrawSubdiv()
{
	finalvert_t *pfv = r_affinetridesc.pfinalverts;
	mtriangle_t *ptri = r_affinetridesc.ptriangles;
	int lnumtriangles = r_affinetridesc.numtriangles;
	for (int i = 0; i < lnumtriangles; i++) {
		finalvert_t *i0 = pfv + ptri[i].vertindex[0];
		finalvert_t *i1 = pfv + ptri[i].vertindex[1];
		finalvert_t *i2 = pfv + ptri[i].vertindex[2];
		if(((i0->v[1]-i1->v[1])*(i0->v[0]-i2->v[0])-(i0->v[0]-i1->v[0])
					*(i0->v[1]-i2->v[1]))>=0)
			continue;
		d_pcolormap = &((byte *) acolormap)[i0->v[4] & 0xFF00];
		if (ptri[i].facesfront) {
			D_PolysetRecursiveTriangle(i0->v, i1->v, i2->v);
		} else {
			int s0, s1, s2;
			s0 = i0->v[2];
			s1 = i1->v[2];
			s2 = i2->v[2];
			if (i0->flags & ALIAS_ONSEAM)
				i0->v[2] += r_affinetridesc.seamfixupX16;
			if (i1->flags & ALIAS_ONSEAM)
				i1->v[2] += r_affinetridesc.seamfixupX16;
			if (i2->flags & ALIAS_ONSEAM)
				i2->v[2] += r_affinetridesc.seamfixupX16;
			D_PolysetRecursiveTriangle(i0->v, i1->v, i2->v);
			i0->v[2] = s0;
			i1->v[2] = s1;
			i2->v[2] = s2;
		}
	}
}

void D_DrawNonSubdiv()
{
	finalvert_t *pfv = r_affinetridesc.pfinalverts;
	mtriangle_t *ptri = r_affinetridesc.ptriangles;
	int lnumtriangles = r_affinetridesc.numtriangles;
	for (int i = 0; i < lnumtriangles; i++, ptri++) {
		finalvert_t *i0 = pfv + ptri->vertindex[0];
		finalvert_t *i1 = pfv + ptri->vertindex[1];
		finalvert_t *i2 = pfv + ptri->vertindex[2];
		d_xdenom=(i0->v[1]-i1->v[1])*(i0->v[0]-i2->v[0])-
			(i0->v[0]-i1->v[0])*(i0->v[1]-i2->v[1]);
		if (d_xdenom >= 0)
			continue;
		r_p0[0] = i0->v[0]; // u
		r_p0[1] = i0->v[1]; // v
		r_p0[2] = i0->v[2]; // s
		r_p0[3] = i0->v[3]; // t
		r_p0[4] = i0->v[4]; // light
		r_p0[5] = i0->v[5]; // iz
		r_p1[0] = i1->v[0];
		r_p1[1] = i1->v[1];
		r_p1[2] = i1->v[2];
		r_p1[3] = i1->v[3];
		r_p1[4] = i1->v[4];
		r_p1[5] = i1->v[5];
		r_p2[0] = i2->v[0];
		r_p2[1] = i2->v[1];
		r_p2[2] = i2->v[2];
		r_p2[3] = i2->v[3];
		r_p2[4] = i2->v[4];
		r_p2[5] = i2->v[5];
		if (!ptri->facesfront) {
			if (i0->flags & ALIAS_ONSEAM)
				r_p0[2] += r_affinetridesc.seamfixupX16;
			if (i1->flags & ALIAS_ONSEAM)
				r_p1[2] += r_affinetridesc.seamfixupX16;
			if (i2->flags & ALIAS_ONSEAM)
				r_p2[2] += r_affinetridesc.seamfixupX16;
		}
		D_PolysetSetEdgeTable();
		D_RasterizeAliasPolySmooth();
	}
}

void D_PolysetRecursiveTriangle(int *lp1, int *lp2, int *lp3)
{
	int *temp;
	int new[6]; // keep here for OpenBSD compiler
	int d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;
	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;
	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1) {
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;
		goto split;
	}
	return; // entire tri is filled
split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;
split: // split this edge
	new[0] = (lp1[0] + lp2[0]) >> 1;
	new[1] = (lp1[1] + lp2[1]) >> 1;
	new[2] = (lp1[2] + lp2[2]) >> 1;
	new[3] = (lp1[3] + lp2[3]) >> 1;
	new[5] = (lp1[5] + lp2[5]) >> 1;
	if (lp2[1] > lp1[1]) // draw the point if splitting a leading edge
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;
	int z = new[5] >> 16;
	short *zbuf = zspantable[new[1]] + new[0];
	if (zbuf < d_pzbuffer || // FIXME properly
		zbuf >= (d_pzbuffer+vid.width*vid.height*2)) {
		printf("ZBUF SEGFAULT 1 %lx\n", zbuf);
		goto nodraw;
	}
	if (z >= *zbuf) {
		*zbuf = z;
		int pix = d_pcolormap[skintable[new[3] >> 16][new[2] >> 16]];
		if (!r_rgblighting.value || !colored_aliaslight)
			pix = d_pcolormap[skintable[new[3] >> 16][new[2] >> 16]];
		else {
			pix = d_pcolormap[skintable[new[3] >> 16][new[2] >> 16]];
			unsigned char tr = vid_curpal[pix * 3 + 0];
			unsigned char tg = vid_curpal[pix * 3 + 1];
			unsigned char tb = vid_curpal[pix * 3 + 2];
			unsigned char rr = (tr * lightcolor[0]) / 255.0f;
			unsigned char gg = (tg * lightcolor[1]) / 255.0f;
			unsigned char bb = (tb * lightcolor[2]) / 255.0f;
			pix = lit_lut[ QUANT(rr)
				+ QUANT(gg)*LIT_LUT_RES
				+ QUANT(bb)*LIT_LUT_RES*LIT_LUT_RES ];
		}
		if (r_alphastyle.value == 0 && cur_ent_alpha != 1) {
			int curpix = d_viewbuffer[d_scantable[new[1]] + new[0]];
			d_viewbuffer[d_scantable[new[1]] + new[0]] =
				color_mix_lut[curpix][pix]
				[(int)((1-cur_ent_alpha)*FOG_LUT_LEVELS)];
		}
		else if (r_alphastyle.value == 1 && cur_ent_alpha != 1) {
			if (D_Dither(&d_viewbuffer[d_scantable[new[1]] + new[0]]))
				d_viewbuffer[d_scantable[new[1]] + new[0]] = pix;
		}
		else
			d_viewbuffer[d_scantable[new[1]] + new[0]] = pix;
	}
nodraw: // recursively continue
	D_PolysetRecursiveTriangle(lp3, lp1, new);
	D_PolysetRecursiveTriangle(lp3, new, lp2);
}

void D_PolysetUpdateTables()
{
	if (r_affinetridesc.skinwidth != skinwidth ||
			r_affinetridesc.pskin != skinstart) {
		skinwidth = r_affinetridesc.skinwidth;
		skinstart = r_affinetridesc.pskin;
		byte *s = skinstart;
		for (int i = 0; i < MAX_LBM_HEIGHT; i++, s += skinwidth)
			skintable[i] = s;
	}
}

void D_PolysetScanLeftEdge(int height)
{
	do {
		d_pedgespanpackage->pdest = d_pdest;
		d_pedgespanpackage->pz = d_pz;
		d_pedgespanpackage->count = d_aspancount;
		d_pedgespanpackage->ptex = d_ptex;
		d_pedgespanpackage->sfrac = d_sfrac;
		d_pedgespanpackage->tfrac = d_tfrac;
		// FIXME: need to clamp l, s, t, at both ends?
		d_pedgespanpackage->light = d_light;
		d_pedgespanpackage->zi = d_zi;
		d_pedgespanpackage++;
		errorterm += erroradjustup;
		if (errorterm >= 0) {
			d_pdest += d_pdestextrastep;
			d_pz += d_pzextrastep;
			d_aspancount += d_countextrastep;
			d_ptex += d_ptexextrastep;
			d_sfrac += d_sfracextrastep;
			d_ptex += d_sfrac >> 16;
			d_sfrac &= 0xFFFF;
			d_tfrac += d_tfracextrastep;
			if (d_tfrac & 0x10000) {
				d_ptex += r_affinetridesc.skinwidth;
				d_tfrac &= 0xFFFF;
			}
			d_light += d_lightextrastep;
			d_zi += d_ziextrastep;
			errorterm -= erroradjustdown;
		} else {
			d_pdest += d_pdestbasestep;
			d_pz += d_pzbasestep;
			d_aspancount += ubasestep;
			d_ptex += d_ptexbasestep;
			d_sfrac += d_sfracbasestep;
			d_ptex += d_sfrac >> 16;
			d_sfrac &= 0xFFFF;
			d_tfrac += d_tfracbasestep;
			if (d_tfrac & 0x10000) {
				d_ptex += r_affinetridesc.skinwidth;
				d_tfrac &= 0xFFFF;
			}
			d_light += d_lightbasestep;
			d_zi += d_zibasestep;
		}
	} while (--height);
}

void D_PolysetSetUpForLineScan(fixed8_t startvertu, fixed8_t startvertv,
		fixed8_t endvertu, fixed8_t endvertv)
{
	errorterm = -1;
	int tm = endvertu - startvertu;
	int tn = endvertv - startvertv;
	if (((tm <= 16) && (tm >= -15)) && ((tn <= 16) && (tn >= -15))) {
		adivtab_t *ptemp = &adivtab[((tm + 15) << 5) + (tn + 15)];
		ubasestep = ptemp->quotient;
		erroradjustup = ptemp->remainder;
		erroradjustdown = tn;
	} else {
		double dm = (double)tm;
		double dn = (double)tn;
		FloorDivMod(dm, dn, &ubasestep, &erroradjustup);
		erroradjustdown = dn;
	}
}

void D_PolysetCalcGradients(int skinwidth)
{
	float p00_minus_p20 = r_p0[0] - r_p2[0];
	float p01_minus_p21 = r_p0[1] - r_p2[1];
	float p10_minus_p20 = r_p1[0] - r_p2[0];
	float p11_minus_p21 = r_p1[1] - r_p2[1];
	float xstepdenominv = 1.0 / (float)d_xdenom;
	float ystepdenominv = -xstepdenominv;
	// ceil () for light so positive steps are exaggerated, negative steps
	// diminished, pushing us away from underflow toward overflow. Underflow is
	// very visible, overflow is very unlikely, because of ambient lighting
	float t0 = r_p0[4] - r_p2[4];
	float t1 = r_p1[4] - r_p2[4];
	r_lstepx = (int)ceil((t1*p01_minus_p21-t0*p11_minus_p21)*xstepdenominv);
	r_lstepy = (int)ceil((t1*p00_minus_p20-t0*p10_minus_p20)*ystepdenominv);
	t0 = r_p0[2] - r_p2[2];
	t1 = r_p1[2] - r_p2[2];
	r_sstepx = (int)((t1*p01_minus_p21-t0*p11_minus_p21)*xstepdenominv);
	r_sstepy = (int)((t1*p00_minus_p20-t0*p10_minus_p20)*ystepdenominv);
	t0 = r_p0[3] - r_p2[3];
	t1 = r_p1[3] - r_p2[3];
	r_tstepx = (int)((t1*p01_minus_p21-t0*p11_minus_p21)*xstepdenominv);
	r_tstepy = (int)((t1*p00_minus_p20-t0*p10_minus_p20)*ystepdenominv);
	t0 = r_p0[5] - r_p2[5];
	t1 = r_p1[5] - r_p2[5];
	r_zistepx = (int)((t1*p01_minus_p21-t0*p11_minus_p21)*xstepdenominv);
	r_zistepy = (int)((t1*p00_minus_p20-t0*p10_minus_p20)*ystepdenominv);
	a_sstepxfrac = r_sstepx & 0xFFFF;
	a_tstepxfrac = r_tstepx & 0xFFFF;
	a_ststepxwhole = skinwidth * (r_tstepx >> 16) + (r_sstepx >> 16);
}

void D_PolysetDrawSpans8(spanpackage_t *pspanpackage)
{
	do {
		int lcount = d_aspancount - pspanpackage->count;
		errorterm += erroradjustup;
		if (errorterm >= 0) {
			d_aspancount += d_countextrastep;
			errorterm -= erroradjustdown;
		} else {
			d_aspancount += ubasestep;
		}
		if (lcount) {
			byte *lpdest = pspanpackage->pdest;
			byte *lptex = pspanpackage->ptex;
			short *lpz = pspanpackage->pz;
			int lsfrac = pspanpackage->sfrac;
			int ltfrac = pspanpackage->tfrac;
			int llight = pspanpackage->light;
			int lzi = pspanpackage->zi;
			if (lpz + lcount - d_pzbuffer > vid.width * vid.height * sizeof(short)) {
				printf ("Invalid span length: %d %d\n", 
					lpz + lcount - d_pzbuffer, vid.width * vid.height * sizeof(short));
				break;
				// CyanBun96: i caused this bug and i can't be bothered to fix it. Here, have a
				// quick non-fix to stop it from segfaulting. gl hf whoever stumbles upon this.
			}
			do {
				if ((lzi >> 16) >= *lpz) {
					int pix;
					if (!r_rgblighting.value || !colored_aliaslight)
						pix = ((byte*)acolormap)[*lptex + (llight & 0xFF00)];
					else {
						pix = ((byte*)acolormap)[*lptex];
						unsigned char tr = vid_curpal[pix * 3 + 0];
						unsigned char tg = vid_curpal[pix * 3 + 1];
						unsigned char tb = vid_curpal[pix * 3 + 2];
						unsigned char rr = (tr * lightcolor[0]) / 255.0f;
						unsigned char gg = (tg * lightcolor[1]) / 255.0f;
						unsigned char bb = (tb * lightcolor[2]) / 255.0f;
						pix = lit_lut[ QUANT(rr)
							+ QUANT(gg)*LIT_LUT_RES
							+ QUANT(bb)*LIT_LUT_RES*LIT_LUT_RES ];
					}
					if (r_alphastyle.value == 0 && cur_ent_alpha != 1) {
						int curpix = *lpdest;
						*lpdest = color_mix_lut[curpix][pix]
							[(int)((1-cur_ent_alpha)*FOG_LUT_LEVELS)];
					}
					else if (r_alphastyle.value == 1 && cur_ent_alpha != 1) {
						if (D_Dither(lpdest))
							*lpdest = pix;
					}
					else
						*lpdest = pix;
					*lpz = lzi >> 16;
				}
				lpdest++;
				lzi += r_zistepx;
				lpz++;
				llight += r_lstepx;
				lptex += a_ststepxwhole;
				lsfrac += a_sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += a_tstepxfrac;
				if (ltfrac & 0x10000) {
					lptex += r_affinetridesc.skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--lcount);
		}
		pspanpackage++;
	} while (pspanpackage->count != -999999);
}

void D_RasterizeAliasPolySmooth()
{
	int *plefttop = pedgetable->pleftedgevert0;
	int *prighttop = pedgetable->prightedgevert0;
	int *pleftbottom = pedgetable->pleftedgevert1;
	int *prightbottom = pedgetable->prightedgevert1;
	int initialleftheight = pleftbottom[1] - plefttop[1];
	int initialrightheight = prightbottom[1] - prighttop[1];
	// set the s, t, and light gradients, which are consistent across the
	// triangle because being a triangle, things are affine
	D_PolysetCalcGradients(r_affinetridesc.skinwidth);
	// rasterize the polygon
	// scan out the top (and possibly only) part of the left edge
	d_pedgespanpackage = a_spans;
	ystart = plefttop[1];
	d_aspancount = plefttop[0] - prighttop[0];
	d_ptex = (byte *) r_affinetridesc.pskin + (plefttop[2] >> 16) +
		(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
	d_sfrac = plefttop[2] & 0xFFFF;
	d_tfrac = plefttop[3] & 0xFFFF;
	d_light = plefttop[4];
	d_zi = plefttop[5];
	d_pdest = (byte *) d_viewbuffer + ystart * screenwidth + plefttop[0];
	d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];
	int working_lstepx, originalcount;
	if (initialleftheight == 1) {
		d_pedgespanpackage->pdest = d_pdest;
		d_pedgespanpackage->pz = d_pz;
		d_pedgespanpackage->count = d_aspancount;
		d_pedgespanpackage->ptex = d_ptex;
		d_pedgespanpackage->sfrac = d_sfrac;
		d_pedgespanpackage->tfrac = d_tfrac;
		// FIXME: need to clamp l, s, t, at both ends?
		d_pedgespanpackage->light = d_light;
		d_pedgespanpackage->zi = d_zi;
		d_pedgespanpackage++;
	} else {
		D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
				pleftbottom[0], pleftbottom[1]);
		d_pzbasestep = d_zwidth + ubasestep;
		d_pzextrastep = d_pzbasestep + 1;
		d_pdestbasestep = screenwidth + ubasestep;
		d_pdestextrastep = d_pdestbasestep + 1;
		// TODO: can reuse partial expressions here
		// for negative steps in x along left edge, bias toward overflow
		// rather than underflow (sort of turning the floor () we did in
		// the gradient calcs into ceil (), but plus a little bit)
		if (ubasestep < 0)
			working_lstepx = r_lstepx - 1;
		else
			working_lstepx = r_lstepx;
		d_countextrastep = ubasestep + 1;
		d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
			((r_tstepy + r_tstepx * ubasestep) >> 16) *
			r_affinetridesc.skinwidth;
		d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
		d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
		d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
		d_zibasestep = r_zistepy + r_zistepx * ubasestep;
		d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >>
			16) + ((r_tstepy + r_tstepx * d_countextrastep) >> 16)
			* r_affinetridesc.skinwidth;
		d_sfracextrastep = (r_sstepy+r_sstepx*d_countextrastep)&0xFFFF;
		d_tfracextrastep = (r_tstepy+r_tstepx*d_countextrastep)&0xFFFF;
		d_lightextrastep = d_lightbasestep + working_lstepx;
		d_ziextrastep = d_zibasestep + r_zistepx;
		D_PolysetScanLeftEdge(initialleftheight);
	}
	// scan out the bottom part of the left edge, if it exists
	if (pedgetable->numleftedges == 2) {
		int height;
		plefttop = pleftbottom;
		pleftbottom = pedgetable->pleftedgevert2;
		height = pleftbottom[1] - plefttop[1];
		//TODO make this a function; modularize this function in general
		ystart = plefttop[1];
		d_aspancount = plefttop[0] - prighttop[0];
		d_ptex = (byte *) r_affinetridesc.pskin + (plefttop[2] >> 16) +
			(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
		d_sfrac = 0;
		d_tfrac = 0;
		d_light = plefttop[4];
		d_zi = plefttop[5];
		d_pdest = (byte*)d_viewbuffer+ystart*screenwidth+plefttop[0];
		d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];
		if (height == 1) {
			d_pedgespanpackage->pdest = d_pdest;
			d_pedgespanpackage->pz = d_pz;
			d_pedgespanpackage->count = d_aspancount;
			d_pedgespanpackage->ptex = d_ptex;
			d_pedgespanpackage->sfrac = d_sfrac;
			d_pedgespanpackage->tfrac = d_tfrac;
			// FIXME: need to clamp l, s, t, at both ends?
			d_pedgespanpackage->light = d_light;
			d_pedgespanpackage->zi = d_zi;
			d_pedgespanpackage++;
		} else {
			D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
					pleftbottom[0], pleftbottom[1]);
			d_pdestbasestep = screenwidth + ubasestep;
			d_pdestextrastep = d_pdestbasestep + 1;
			d_pzbasestep = d_zwidth + ubasestep;
			d_pzextrastep = d_pzbasestep + 1;
			working_lstepx = ubasestep < 0? r_lstepx - 1 : r_lstepx;
			d_countextrastep = ubasestep + 1;
			d_ptexbasestep=((r_sstepy+r_sstepx*ubasestep)>>16)+
				((r_tstepy+r_tstepx*ubasestep)>>16)*
				r_affinetridesc.skinwidth;
			d_sfracbasestep=(r_sstepy+r_sstepx*ubasestep)&0xFFFF;
			d_tfracbasestep=(r_tstepy+r_tstepx*ubasestep)&0xFFFF;
			d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
			d_zibasestep = r_zistepy + r_zistepx * ubasestep;
			d_ptexextrastep=((r_sstepy+r_sstepx*d_countextrastep)>>
				16)+((r_tstepy+r_tstepx*d_countextrastep)>>16)*
				r_affinetridesc.skinwidth;
			d_sfracextrastep=
				(r_sstepy+r_sstepx*d_countextrastep)&0xFFFF;
			d_tfracextrastep=
				(r_tstepy+r_tstepx*d_countextrastep)&0xFFFF;
			d_lightextrastep = d_lightbasestep + working_lstepx;
			d_ziextrastep = d_zibasestep + r_zistepx;
			D_PolysetScanLeftEdge(height);
		}
	}
	// scan out the top (and possibly only) part of the right edge, updating
	// the count field
	d_pedgespanpackage = a_spans;
	D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
			prightbottom[0], prightbottom[1]);
	d_aspancount = 0;
	d_countextrastep = ubasestep + 1;
	originalcount = a_spans[initialrightheight].count;
	a_spans[initialrightheight].count = -999999; // mark end of the spanpackages
	D_PolysetDrawSpans8(a_spans);
	// scan out the bottom part of the right edge, if it exists
	if (pedgetable->numrightedges == 2) {
		spanpackage_t *pstart;
		pstart = a_spans + initialrightheight;
		pstart->count = originalcount;
		d_aspancount = prightbottom[0] - prighttop[0];
		prighttop = prightbottom;
		prightbottom = pedgetable->prightedgevert2;
		int height = prightbottom[1] - prighttop[1];
		D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
				prightbottom[0], prightbottom[1]);
		d_countextrastep = ubasestep + 1;
		a_spans[initialrightheight + height].count = -999999;
		D_PolysetDrawSpans8(pstart); // mark end of the spanpackages
	}
}

void D_PolysetSetEdgeTable()
{
	int edgetableindex = 0; // assume the vertices are already in top to
		// bottom order determine which edges are right & left, and the
		// order in which to rasterize them
	if (r_p0[1] >= r_p1[1]) {
		if (r_p0[1] == r_p1[1]) {
			pedgetable=r_p0[1]<r_p2[1]?&edgetables[2]:&edgetables[5];
			return;
		} else
			edgetableindex = 1;
	}
	if (r_p0[1] == r_p2[1]) {
		pedgetable = edgetableindex ? &edgetables[8] : &edgetables[9];
		return;
	} else if (r_p1[1] == r_p2[1]) {
		pedgetable = edgetableindex ? &edgetables[10] : &edgetables[11];
		return;
	}
	if (r_p0[1] > r_p2[1])
		edgetableindex += 2;
	if (r_p1[1] > r_p2[1])
		edgetableindex += 4;
	pedgetable = &edgetables[edgetableindex];
}
