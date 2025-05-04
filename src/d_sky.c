// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#define SKY_SPAN_SHIFT	5
#define SKY_SPAN_MAX	(1 << SKY_SPAN_SHIFT)

void D_Sky_uv_To_st(int u, int v, fixed16_t *s, fixed16_t *t)
{
	float temp = r_refdef.vrect.width >= r_refdef.vrect.height ?
		(float)r_refdef.vrect.width : (float)r_refdef.vrect.height;
	float wu = 8192.0 * (float)(u - ((int)vid.width >> 1)) / temp;
	float wv = 8192.0 * (float)(((int)vid.height >> 1) - v) / temp;
	vec3_t end;
	end[0] = 4096 * vpn[0] + wu * vright[0] + wv * vup[0];
	end[1] = 4096 * vpn[1] + wu * vright[1] + wv * vup[1];
	end[2] = 4096 * vpn[2] + wu * vright[2] + wv * vup[2];
	end[2] *= 3;
	VectorNormalize(end);
	temp = skytime * skyspeed; // TODO: add D_SetupFrame & set this there
	*s = (int)((temp + 6 * (SKYSIZE / 2 - 1) * end[0]) * 0x10000);
	*t = (int)((temp + 6 * (SKYSIZE / 2 - 1) * end[1]) * 0x10000);
}

void D_DrawSkyScans8(espan_t *pspan)
{
	do {
		unsigned char *pdest = (unsigned char *)((byte *) d_viewbuffer +
					  (screenwidth * pspan->v) + pspan->u);
		int count = pspan->count;
		int u = pspan->u; // calculate the initial s & t
		int v = pspan->v;
		fixed16_t s, t, snext = 0, tnext = 0;
		D_Sky_uv_To_st(u, v, &s, &t);
		do {
			int spancount = count >= SKY_SPAN_MAX ?
				SKY_SPAN_MAX : count;
			count -= spancount;
			fixed16_t sstep = 0;
			fixed16_t tstep = 0;
			if (count) {
				u += spancount;
				// calculate s and t at far end of span,
				// calculate s and t steps across span by shifting
				D_Sky_uv_To_st(u, v, &snext, &tnext);
				sstep = (snext - s) >> SKY_SPAN_SHIFT;
				tstep = (tnext - t) >> SKY_SPAN_SHIFT;
			} else {
				// calculate s and t at last pixel in span,
				// calculate s and t steps across span by division
				int spancountminus1 = (float)(spancount - 1);
				if (spancountminus1 > 0) {
					u += spancountminus1;
					D_Sky_uv_To_st(u, v, &snext, &tnext);
					sstep = (snext - s) / spancountminus1;
					tstep = (tnext - t) / spancountminus1;
				}
			}
			do {
				*pdest++ = r_skysource[((t & R_SKY_TMASK) >> 8)+
						((s & R_SKY_SMASK) >> 16)];
				s += sstep;
				t += tstep;
			} while (--spancount > 0);
			s = snext;
			t = tnext;
		} while (count > 0);
	} while ((pspan = pspan->pnext) != NULL);
}
