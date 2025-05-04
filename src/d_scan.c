// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

// d_scan.c: Portable C scan-level rasterization code, all pixel depths.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

unsigned char *r_turb_pbase, *r_turb_pdest;
fixed16_t r_turb_s, r_turb_t, r_turb_sstep, r_turb_tstep;
int *r_turb_turb;
int r_turb_spancount;
short *pz; // Manoel Kasimier - translucent water
int izi, izistep;
int dither_pat = 0;
unsigned char *litwater_base;
int lwmark = 0;

extern cvar_t r_skyfog;
extern cvar_t r_alphastyle;
extern int fog_lut_built;
extern void build_color_mix_lut();

void D_DrawTurbulent8Span();

int D_Dither(byte *pos)
{
	unsigned long d = pos - vid.buffer;
	unsigned long x = d % vid.width;
	unsigned long y = d / vid.width;
	switch (dither_pat) {
		case 0: return !(d % 6); // 1/6
		case 1: return (y&1) && ((y&3) == 3 ? (x&1) : !(x&1)); // 1/4
		case 2: return !(d % 3); // 1/3
		case 3: return (x + y) & 1; // 1/2
		case 4: return d % 3; // 2/3
		case 5: return !((y&1) && ((y&3) == 3 ? (x&1) : !(x&1))); // 3/4
		default: case 6: return d % 6; // 5/6
	}
}

void D_WarpScreen() // this performs a slight compression of the screen at the
{ // same time as the sine warp, to keep the edges from wrapping
	int w = r_refdef.vrect.width;
	int h = r_refdef.vrect.height;
	float wratio = w / (float)scr_vrect.width;
	float hratio = h / (float)scr_vrect.height;
	byte *rowptr[MAXHEIGHT + (AMP2 * 2)];
	int column[MAXWIDTH + (AMP2 * 2)];
	for (int v = 0; v < scr_vrect.height + AMP2 * 2; v++)
		rowptr[v] = d_viewbuffer + (r_refdef.vrect.y * screenwidth) +
		    screenwidth * (int)((float)v * hratio * h / (h + AMP2 * 2));
	for (int u = 0; u < scr_vrect.width + AMP2 * 2; u++)
		column[u] = r_refdef.vrect.x + (int)((float)u * wratio * w /
						     (w + AMP2 * 2));
	int *turb = intsintable + ((int)(cl.time * SPEED) & (CYCLE - 1));
	byte *dest = vid.buffer + scr_vrect.y * vid.rowbytes + scr_vrect.x;
	for (int v = 0; v < scr_vrect.height; v++, dest += vid.rowbytes) {
		int *col = &column[turb[v]];
		byte **row = &rowptr[v];
		for (int u = 0; u < scr_vrect.width; u += 4) {
			dest[u + 0] = row[turb[u + 0]][col[u + 0]];
			dest[u + 1] = row[turb[u + 1]][col[u + 1]];
			dest[u + 2] = row[turb[u + 2]][col[u + 2]];
			dest[u + 3] = row[turb[u + 3]][col[u + 3]];
		}
	}
}

void D_DrawTurbulent8Span()
{
	if (!lmonly) {
		do {
			int s = ((r_turb_s + r_turb_turb[(r_turb_t >> 16) & (CYCLE - 1)]) >> 16) & 63;
			int t = ((r_turb_t + r_turb_turb[(r_turb_s >> 16) & (CYCLE - 1)]) >> 16) & 63;
			int pix = *(r_turb_pbase + (t << 6) + s);
			*r_turb_pdest++ = pix;
			r_turb_s += r_turb_sstep;
			r_turb_t += r_turb_tstep;
		} while (--r_turb_spancount > 0);
	} else { // lit water: render first and then apply the already drawn lightmap as a filter
		if (!lit_lut_initialized) R_BuildLitLUT();
		do {
			int s = ((r_turb_s + r_turb_turb[(r_turb_t >> 16) & (CYCLE - 1)]) >> 16) & 63;
			int t = ((r_turb_t + r_turb_turb[(r_turb_s >> 16) & (CYCLE - 1)]) >> 16) & 63;
			int pix = *(r_turb_pbase + (t << 6) + s);
			int lit = *(litwater_base+(r_turb_pdest-d_viewbuffer));
			unsigned char rp = host_basepal[pix * 3 + 0];
			unsigned char gp = host_basepal[pix * 3 + 1];
			unsigned char bp = host_basepal[pix * 3 + 2];
			unsigned char rl = host_basepal[lit * 3 + 0];
			unsigned char gl = host_basepal[lit * 3 + 1];
			unsigned char bl = host_basepal[lit * 3 + 2];
			int r = rp * rl / 255;
			int g = gp * gl / 255;
			int b = bp * bl / 255;
			*r_turb_pdest++ = lit_lut[ QUANT(r)
                                        + QUANT(g)*LIT_LUT_RES
                                        + QUANT(b)*LIT_LUT_RES*LIT_LUT_RES ];
			r_turb_s += r_turb_sstep;
			r_turb_t += r_turb_tstep;
		} while (--r_turb_spancount > 0);
	}
}

void D_DrawTurbulent8SpanAlpha (float opacity)
{
	if (r_alphastyle.value == 0 && !lmonly) {
		if (!fog_lut_built) build_color_mix_lut();
		do {
			if (*pz <= (izi >> 16)) {
				int s=((r_turb_s+r_turb_turb[(r_turb_t>>16)&(CYCLE-1)])>>16)&63;
				int t=((r_turb_t+r_turb_turb[(r_turb_s>>16)&(CYCLE-1)])>>16)&63;
				*r_turb_pdest = color_mix_lut[*(r_turb_pbase + (t << 6) + s)]
					[*r_turb_pdest][(int)(opacity*FOG_LUT_LEVELS)];
			}
			r_turb_pdest++;
			izi += izistep;
			pz++;
			r_turb_s += r_turb_sstep;
			r_turb_t += r_turb_tstep;
		} while (--r_turb_spancount > 0);
		return;
	} else if (r_alphastyle.value == 0 && lmonly) {
		if (!fog_lut_built) build_color_mix_lut();
		if (!lit_lut_initialized) R_BuildLitLUT();
		do {
			if (*pz <= (izi >> 16)) {
				int s=((r_turb_s+r_turb_turb[(r_turb_t>>16)&(CYCLE-1)])>>16)&63;
				int t=((r_turb_t+r_turb_turb[(r_turb_s>>16)&(CYCLE-1)])>>16)&63;
				int pix = *(r_turb_pbase + (t << 6) + s);
				int lit = *(litwater_base+(r_turb_pdest-d_viewbuffer));
				unsigned char rp = host_basepal[pix * 3 + 0];
				unsigned char gp = host_basepal[pix * 3 + 1];
				unsigned char bp = host_basepal[pix * 3 + 2];
				unsigned char rl = host_basepal[lit * 3 + 0];
				unsigned char gl = host_basepal[lit * 3 + 1];
				unsigned char bl = host_basepal[lit * 3 + 2];
				int r = rp * rl / 255;
				int g = gp * gl / 255;
				int b = bp * bl / 255;
				pix = lit_lut[ QUANT(r)
						+ QUANT(g)*LIT_LUT_RES
						+ QUANT(b)*LIT_LUT_RES*LIT_LUT_RES ];
				*r_turb_pdest = color_mix_lut[pix]
					[*r_turb_pdest][(int)(opacity*FOG_LUT_LEVELS)];
			}
			r_turb_pdest++;
			izi += izistep;
			pz++;
			r_turb_s += r_turb_sstep;
			r_turb_t += r_turb_tstep;
		} while (--r_turb_spancount > 0);
		return;
	}
	if (opacity >= 0.83f) dither_pat = 0;
	else if (opacity >= 0.75f) dither_pat = 1;
	else if (opacity >= 0.66f) dither_pat = 2;
	else if (opacity >= 0.50f) dither_pat = 3;
	else if (opacity >= 0.33f) dither_pat = 4;
	else if (opacity >= 0.25f) dither_pat = 5;
	else dither_pat = 6;
	if (!lmonly) {
		do {
			if (*pz <= (izi >> 16)) {
				int s=((r_turb_s+r_turb_turb[(r_turb_t>>16)&(CYCLE-1)])>>16)&63;
				int t=((r_turb_t+r_turb_turb[(r_turb_s>>16)&(CYCLE-1)])>>16)&63;
				if (D_Dither(r_turb_pdest))
					*r_turb_pdest = *(r_turb_pbase + (t << 6) + s);
			}
			r_turb_pdest++;
			izi += izistep;
			pz++;
			r_turb_s += r_turb_sstep;
			r_turb_t += r_turb_tstep;
		} while (--r_turb_spancount > 0);
	} else {
		do {
			if (*pz <= (izi >> 16)) {
				int s=((r_turb_s+r_turb_turb[(r_turb_t>>16)&(CYCLE-1)])>>16)&63;
				int t=((r_turb_t+r_turb_turb[(r_turb_s>>16)&(CYCLE-1)])>>16)&63;
				if (D_Dither(r_turb_pdest)) {
					int pix = *(r_turb_pbase + (t << 6) + s);
					int lit = *(litwater_base+(r_turb_pdest-d_viewbuffer));
					unsigned char rp = host_basepal[pix * 3 + 0];
					unsigned char gp = host_basepal[pix * 3 + 1];
					unsigned char bp = host_basepal[pix * 3 + 2];
					unsigned char rl = host_basepal[lit * 3 + 0];
					unsigned char gl = host_basepal[lit * 3 + 1];
					unsigned char bl = host_basepal[lit * 3 + 2];
					int r = rp * rl / 255;
					int g = gp * gl / 255;
					int b = bp * bl / 255;
					*r_turb_pdest = lit_lut[ QUANT(r)
							+ QUANT(g)*LIT_LUT_RES
							+ QUANT(b)*LIT_LUT_RES*LIT_LUT_RES ];
				}
			}
			r_turb_pdest++;
			izi += izistep;
			pz++;
			r_turb_s += r_turb_sstep;
			r_turb_t += r_turb_tstep;
		} while (--r_turb_spancount > 0);
	}
}

void Turbulent8(espan_t *pspan, float opacity)
{
	r_turb_turb = sintable + ((int)(cl.time * SPEED) & (CYCLE - 1));
	r_turb_sstep = 0; // keep compiler happy
	r_turb_tstep = 0; // ditto
	r_turb_pbase = (unsigned char *)cacheblock;
	float sdivz16stepu = d_sdivzstepu * 16;
	float tdivz16stepu = d_tdivzstepu * 16;
	float zi16stepu = d_zistepu * 16;
	izistep = (int)(d_zistepu * 0x8000 * 0x10000);
	do {
		r_turb_pdest = (byte *) ((byte *) d_viewbuffer + (screenwidth * pspan->v) + pspan->u);
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u; // Manoel Kasimier - translucent water
		int count = pspan->count;
		float du = (float)pspan->u; // calculate the initial s/z, t/z,
		float dv = (float)pspan->v; // 1/z, s, and t and clamp
		float sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		float tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		float zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
		float z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
		izi = (int)(zi * 0x8000 * 0x10000); // Manoel Kasimier - translucent water
		r_turb_s = (int)(sdivz * z) + sadjust;
		if (r_turb_s > bbextents)
			r_turb_s = bbextents;
		else if (r_turb_s < 0)
			r_turb_s = 0;
		r_turb_t = (int)(tdivz * z) + tadjust;
		if (r_turb_t > bbextentt)
			r_turb_t = bbextentt;
		else if (r_turb_t < 0)
			r_turb_t = 0;
		do {
			r_turb_spancount = count; // calculate s and t
			if (count >= 16) // at the far end of the span
				r_turb_spancount = 16;
			count -= r_turb_spancount;
			fixed16_t snext, tnext;
			if (count) {
				// calculate s/z, t/z, zi->fixed s and t at far end of span,
				// calculate s and t steps across span by shifting
				sdivz += sdivz16stepu;
				tdivz += tdivz16stepu;
				zi += zi16stepu;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;
				// guard against round-off error on <0 steps
				r_turb_sstep = (snext - r_turb_s) >> 4;
				r_turb_tstep = (tnext - r_turb_t) >> 4;
			} else {
				// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
				// can't step off polygon), clamp, calculate s and t steps across
				// span by division, biasing steps low so we don't run off the
				// texture
				float spancountminus1 = (float)(r_turb_spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;// guard against round-off error on <0 steps
				if (r_turb_spancount > 1) {
					r_turb_sstep = (snext - r_turb_s) / (r_turb_spancount - 1);
					r_turb_tstep = (tnext - r_turb_t) / (r_turb_spancount - 1);
				}
			}
			r_turb_s = r_turb_s & ((CYCLE << 16) - 1);
			r_turb_t = r_turb_t & ((CYCLE << 16) - 1);
			if (r_wateralphapass && opacity < 1)
				D_DrawTurbulent8SpanAlpha(opacity);
			else
				D_DrawTurbulent8Span();
			r_turb_s = snext;
			r_turb_t = tnext;
		} while (count > 0);
	} while ((pspan = pspan->pnext) != NULL);
}

void D_DrawSkyboxScans8(espan_t *pspan)
{ // CyanBun96: this is exactly the same as DrawSpans8 except for the fog mixing part. consolidate. FIXME
	unsigned char *pbase = (unsigned char *)cacheblock;
	float sdivz8stepu = d_sdivzstepu * 8;
	float tdivz8stepu = d_tdivzstepu * 8;
	float zi8stepu = d_zistepu * 8;
	do {
		unsigned char *pdest = (unsigned char *)((byte *) d_viewbuffer +
				      (screenwidth * pspan->v) + pspan->u);
		int count = pspan->count;
		float du = (float)pspan->u; // calculate the initial s/z, t/z,
		float dv = (float)pspan->v; // 1/z, s, and t and clamp
		float sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		float tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		float zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
		float z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
		fixed16_t s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;
		fixed16_t t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;
		do {
			// calculate s and t at the far end of the span
			fixed16_t snext, tnext;
			fixed16_t sstep = 0; // keep compiler happy
			fixed16_t tstep = 0; // ditto
			int spancount = count;
			if (count >= 8)
				spancount = 8;
			count -= spancount;
			if (count) {
				// calculate s/z, t/z, zi->fixed s and t at far end of span,
				// calculate s and t steps across span by shifting
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8; // guard against round-off error on <0 steps
				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			} else {
				// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
				// can't step off polygon), clamp, calculate s and t steps across
				// span by division, biasing steps low so we don't run off the
				// texture
				float spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8; // guard against round-off error on <0 steps
				if (spancount > 1) {
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			int foglut = r_skyfog.value*FOG_LUT_LEVELS;
			do {
				unsigned char pix = *(pbase + (s >> 16) +
					(t >> 16) * cachewidth);
				pix = color_mix_lut[pix][fog_pal_index][foglut];
				if (pix != 0xff || !((int)r_twopass.value&1))
					*pdest = pix;
				pdest++;
				s += sstep;
				t += tstep;
			} while (--spancount > 0);
			s = snext;
			t = tnext;
		} while (count > 0);
	} while ((pspan = pspan->pnext) != NULL);
}

void D_DrawSpans8(espan_t *pspan)
{
	unsigned char *pbase = (unsigned char *)cacheblock;
	float sdivz8stepu = d_sdivzstepu * 8;
	float tdivz8stepu = d_tdivzstepu * 8;
	float zi8stepu = d_zistepu * 8;
	do {
		unsigned char *pdest = (unsigned char *)((byte *) d_viewbuffer +
				      (screenwidth * pspan->v) + pspan->u);
		if (lmonly) {
			if (!litwater_base) {
				lwmark = Hunk_HighMark();
				litwater_base = Hunk_HighAllocName(vid.width*vid.height, "waterlightmap");
			}
			pdest = (unsigned char *)((byte *) litwater_base +
				      (screenwidth * pspan->v) + pspan->u);
		}
		int count = pspan->count;
		float du = (float)pspan->u; // calculate the initial s/z, t/z,
		float dv = (float)pspan->v; // 1/z, s, and t and clamp
		float sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		float tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		float zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
		float z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
		fixed16_t s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;
		fixed16_t t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;
		do {
			// calculate s and t at the far end of the span
			fixed16_t snext, tnext;
			fixed16_t sstep = 0; // keep compiler happy
			fixed16_t tstep = 0; // ditto
			int spancount = count;
			if (count >= 8)
				spancount = 8;
			count -= spancount;
			if (count) {
				// calculate s/z, t/z, zi->fixed s and t at far end of span,
				// calculate s and t steps across span by shifting
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8; // guard against round-off error on <0 steps
				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			} else {
				// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
				// can't step off polygon), clamp, calculate s and t steps across
				// span by division, biasing steps low so we don't run off the
				// texture
				float spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8; // guard against round-off error on <0 steps
				if (spancount > 1) {
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do {
				unsigned char pix = *(pbase + (s >> 16) +
					(t >> 16) * cachewidth);
				if (pix != 0xff || !((int)r_twopass.value&1))
					*pdest = pix;
				pdest++;
				s += sstep;
				t += tstep;
			} while (--spancount > 0);
			s = snext;
			t = tnext;
		} while (count > 0);
	} while ((pspan = pspan->pnext) != NULL);
}

void D_DrawTransSpans8(espan_t *pspan, float opacity)
{ // CyanBun96: this is literally D_DrawSpans8 with a few extra lines. FIXME
	unsigned char *pbase = (unsigned char *)cacheblock;
	float sdivz8stepu = d_sdivzstepu * 8;
	float tdivz8stepu = d_tdivzstepu * 8;
	float zi8stepu = d_zistepu * 8;
	izistep = (int)(d_zistepu * 0x8000 * 0x10000);
	do {
		unsigned char *pdest = (unsigned char *)((byte *) d_viewbuffer +
				      (screenwidth * pspan->v) + pspan->u);
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;
		int count = pspan->count;
		float du = (float)pspan->u; // calculate the initial s/z, t/z,
		float dv = (float)pspan->v; // 1/z, s, and t and clamp
		float sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		float tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		float zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
		float z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
		izi = (int)(zi * 0x8000 * 0x10000);
		fixed16_t s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;
		fixed16_t t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;
		do {
			// calculate s and t at the far end of the span
			fixed16_t snext, tnext;
			fixed16_t sstep = 0; // keep compiler happy
			fixed16_t tstep = 0; // ditto
			int spancount = count;
			if (count >= 8)
				spancount = 8;
			count -= spancount;
			if (count) {
				// calculate s/z, t/z, zi->fixed s and t at far end of span,
				// calculate s and t steps across span by shifting
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8; // guard against round-off error on <0 steps
				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			} else {
				// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
				// can't step off polygon), clamp, calculate s and t steps across
				// span by division, biasing steps low so we don't run off the
				// texture
				float spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8; // prevent round-off error on <0 steps from
				// from causing overstepping & running off the
				// edge of the texture
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8; // guard against round-off error on <0 steps
				if (spancount > 1) {
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}
			int foglut = opacity*FOG_LUT_LEVELS;
			if (r_alphastyle.value == 0) {
				if (!fog_lut_built)
					build_color_mix_lut();
				do {
					if (*pz <= (izi >> 16)) {
						unsigned char pix = *(pbase + (s >> 16) +
							(t >> 16) * cachewidth);
						if (pix != 0xff) {
							pix = color_mix_lut[pix][*pdest][foglut];
							*pdest = pix;
						}
					}
					pdest++;
					izi += izistep;
					pz++;
					s += sstep;
					t += tstep;
				} while (--spancount > 0);
			}
			else {
				do {
					if (*pz <= (izi >> 16) && D_Dither(pdest)) {
						unsigned char pix = *(pbase + (s >> 16) +
							(t >> 16) * cachewidth);
						*pdest = pix;
					}
					pdest++;
					izi += izistep;
					pz++;
					s += sstep;
					t += tstep;
				} while (--spancount > 0);
			}
			s = snext;
			t = tnext;
		} while (count > 0);
	} while ((pspan = pspan->pnext) != NULL);
}

void D_DrawZSpans(espan_t *pspan)
{
	int izistep = (int)(d_zistepu * 0x8000 * 0x10000);
	do {
		short *pdest = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;
		int count = pspan->count;
		float du = (float)pspan->u; // calculate the initial 1/z
		float dv = (float)pspan->v;
		double zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
		int izi = (int)(zi * 0x8000 * 0x10000);
		if ((intptr_t) pdest & 0x02) {
			*pdest++ = (short)(izi >> 16);
			izi += izistep;
			count--;
		}
		int doublecount = count >> 1;
		if (doublecount > 0) {
			do {
				unsigned int ltemp = izi >> 16;
				izi += izistep;
				ltemp |= izi & 0xFFFF0000;
				izi += izistep;
				*(int *)pdest = ltemp;
				pdest += 2;
			} while (--doublecount > 0);
		}
		if (count & 1)
			*pdest = (short)(izi >> 16);
	} while ((pspan = pspan->pnext) != NULL);
}
