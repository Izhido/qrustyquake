#include "quakedef.h"
#include "d_local.h"

#define RANDARR_SIZE 19937 // prime number to reduce unintended patterns

typedef struct {
	float l, a, b;
} lab;

int fog_initialized = 0;
static unsigned int lfsr = 0x1337; // non-zero seed
float fog_density;
static float fog_red; // CyanBun96: we store the actual RGB values in these,
static float fog_green; // but they get quantized to a single index in the color
static float fog_blue; // palette before use, stored in fog_pal_index
unsigned char fog_pal_index;
extern cvar_t r_fogstyle;
extern cvar_t r_nofog;
extern cvar_t r_labmixpal;
extern cvar_t r_fogbrightness;
extern unsigned int sb_updates; // if >= vid.numpages, no update needed
float randarr[RANDARR_SIZE];
unsigned char color_mix_lut[256][256][FOG_LUT_LEVELS];
int fog_lut_built = 0;
float gamma_lut[256];
int color_conv_initialized = 0;
lab lab_palette[256];

static inline float lab_f(float t)
{ // cube-root if > epsilon, linear otherwise
	const float epsilon = 0.008856f;
	const float kappa = 903.3f;
	return (t > epsilon) ? cbrtf(t) : ((kappa * t + 16.0f) / 116.0f);
}

static inline lab rgb_lin_to_lab(float r, float g, float b)
{
	float x = r * 0.4124f + g * 0.3576f + b * 0.1805f;
	float y = r * 0.2126f + g * 0.7152f + b * 0.0722f;
	float z = r * 0.0193f + g * 0.1192f + b * 0.9505f;
	x /= 0.95047f; // normalize by d65 white
	z /= 1.08883f;
	float fx = lab_f(x);
	float fy = lab_f(y);
	float fz = lab_f(z);
	lab lab = {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
	return lab;
}

void init_color_conv()
{
	if (color_conv_initialized) return;
	color_conv_initialized = 1;
	for (int i = 0; i < 256; ++i) {
		float v = i / 255.0f;
		gamma_lut[i] = (v > 0.04045f) ?
			powf((v + 0.055f) / 1.055f, 2.4f) : (v / 12.92f);
	}
	for (int i = 0, p = 0; i < 256; ++i, p += 3) {
		float lr = gamma_lut[host_basepal[p + 0]];
		float lg = gamma_lut[host_basepal[p + 1]];
		float lb = gamma_lut[host_basepal[p + 2]];
		lab_palette[i] = rgb_lin_to_lab(lr, lg, lb);
	}
}

unsigned char rgbtoi_lab(unsigned char R, unsigned char G, unsigned char B)
{
	lab lab0 = rgb_lin_to_lab(gamma_lut[R], gamma_lut[G], gamma_lut[B]);
	float bestdist = FLT_MAX;
	unsigned char besti = 0;
	for (int i = 0; i < 256; ++i) {
		float dl = lab0.l - lab_palette[i].l;
		float da = lab0.a - lab_palette[i].a;
		float db = lab0.b - lab_palette[i].b;
		float dist = dl * dl + da * da + db * db;
		if (dist < bestdist) {
			bestdist = dist;
			besti = (unsigned char)i;
			if (dist == 0.0f) break;
		}
	}
	return besti;
}

unsigned char rgbtoi(unsigned char r, unsigned char g, unsigned char b)
{
	unsigned char besti = 0;
	int bestdist = 0x7fffffff;
	unsigned char *p = host_basepal;
	for (int i = 0; i < 256; ++i, p += 3) {
		int dr = r - p[0]; // squared euclidean distance
		int dg = g - p[1];
		int db = b - p[2];
		int dist = dr * dr + dg * dg + db * db;
		if (dist < bestdist) {
			bestdist = dist;
			besti = (unsigned char)i;
			if (dist == 0) // exact match, return early
				return besti;
		}
	}
	return besti;
}

void Fog_SetPalIndex(cvar_t *cvar)
{
	fog_pal_index = rgbtoi_lab(fog_red*255.0f*r_fogbrightness.value,
			fog_green*255.0f*r_fogbrightness.value,
			fog_blue*255.0f*r_fogbrightness.value);
}

void Fog_FogCommand_f () // yanked from Quakespasm, mostly
{ // TODO time transitions
	float d, r, g, b, t;
	switch (Cmd_Argc()) {
		default:
		case 1:
			Con_Printf("usage:\n");
			Con_Printf("   fog <density>\n");
			Con_Printf("   fog <red> <green> <blue>\n");
			Con_Printf("   fog <density> <red> <green> <blue>\n");
			Con_Printf("current values:\n");
			Con_Printf("   \"density\" is \"%f\"\n", fog_density);
			Con_Printf("   \"red\" is \"%f\"\n", fog_red);
			Con_Printf("   \"green\" is \"%f\"\n", fog_green);
			Con_Printf("   \"blue\" is \"%f\"\n", fog_blue);
			return;
		case 2:
			d = Q_atof(Cmd_Argv(1));
			t = 0.0f;
			r = fog_red;
			g = fog_green;
			b = fog_blue;
			break;
		case 3: //TEST
			d = Q_atof(Cmd_Argv(1));
			t = Q_atof(Cmd_Argv(2));
			r = fog_red;
			g = fog_green;
			b = fog_blue;
			break;
		case 4:
			d = fog_density;
			t = 0.0f;
			r = Q_atof(Cmd_Argv(1));
			g = Q_atof(Cmd_Argv(2));
			b = Q_atof(Cmd_Argv(3));
			break;
		case 5:
			d = Q_atof(Cmd_Argv(1));
			r = Q_atof(Cmd_Argv(2));
			g = Q_atof(Cmd_Argv(3));
			b = Q_atof(Cmd_Argv(4));
			t = 0.0f;
			break;
		case 6: //TEST
			d = Q_atof(Cmd_Argv(1));
			r = Q_atof(Cmd_Argv(2));
			g = Q_atof(Cmd_Argv(3));
			b = Q_atof(Cmd_Argv(4));
			t = Q_atof(Cmd_Argv(5));
			break;
	}
	if      (d < 0.0f) d = 0.0f;
	if      (r < 0.0f) r = 0.0f;
	else if (r > 1.0f) r = 1.0f;
	if      (g < 0.0f) g = 0.0f;
	else if (g > 1.0f) g = 1.0f;
	if      (b < 0.0f) b = 0.0f;
	else if (b > 1.0f) b = 1.0f;
	fog_density = d;
	fog_red = r;
	fog_green = g;
	fog_blue = b;
	Fog_SetPalIndex(0);
	vid.recalc_refdef = 1;
	fog_initialized = 0;
}

void Fog_ParseWorldspawn () // from Quakespasm
{ // called at map load
	char key[128], value[4096];
	fog_density = 0; //initially no fog
	fog_red = 0;
	fog_green = 0;
	fog_blue = 0;
	/*TODOold_density = DEFAULT_DENSITY;
	  old_red = DEFAULT_GRAY;
	  old_green = DEFAULT_GRAY;
	  old_blue = DEFAULT_GRAY;
	  fade_time = 0.0;
	  fade_done = 0.0;*/
	const char *data = COM_Parse(cl.worldmodel->entities);
	if (!data || com_token[0] != '{')
		return; // error
	while (1) {
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy(key, com_token + 1, sizeof(key));
		else
			q_strlcpy(key, com_token, sizeof(key));
		while (key[0] && key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;
		data = COM_ParseEx(data, CPE_ALLOWTRUNC);
		if (!data)
			return; // error
		q_strlcpy(value, com_token, sizeof(value));
		if (!strcmp("fog", key))
			sscanf(value, "%f %f %f %f", &fog_density, &fog_red, &fog_green, &fog_blue);
	}
	Fog_SetPalIndex(0);
}

unsigned int lfsr_random() {
	lfsr ^= lfsr >> 7;
	lfsr ^= lfsr << 9;
	lfsr ^= lfsr >> 13;
	return lfsr;
}

int dither(int x, int y, float f) {
	int odd = y & 1;
	int threed = ((y % 3) == 0);
	f = 1 - f;
	if (f >= 1.00) return 0;
	else if (f > 0.83) { if(!(!(x%3) && odd)) return 0; }
	else if (f > 0.75) { if(!(x&1 && odd)) return 0; }
	else if (f > 0.66) { if(!(x%3 && odd)) return 0; }
	else if (f > 0.50) { if((x&1 && odd) || (!(x&1) && !odd)) return 0; }
	else if (f > 0.33) { if((x%3 && odd)) return 0; }
	else if (f > 0.25) { if(x&1 && odd) return 0; }
	else if (f > 0.16) { if(!(x%3) && odd) return 0; }
	else if (f > 0.04) { if(!(x%3) && odd && threed) return 0; }
	return 1; // don't draw
}

void build_color_mix_lut(cvar_t *cvar)
{
	unsigned char (*convfunc)(unsigned char, unsigned char, unsigned char);
	if (r_labmixpal.value == 1) {
		init_color_conv();
		convfunc = rgbtoi_lab;
	}
	else
		convfunc = rgbtoi;
	for (int c1 = 0; c1 < 256; c1++) {
		for (int c2 = 0; c2 < 256; c2++) {
			unsigned char r1 = host_basepal[c1*3+0];
			unsigned char g1 = host_basepal[c1*3+1];
			unsigned char b1 = host_basepal[c1*3+2];
			unsigned char r2 = host_basepal[c2*3+0];
			unsigned char g2 = host_basepal[c2*3+1];
			unsigned char b2 = host_basepal[c2*3+2];
			for (int level = 0; level < FOG_LUT_LEVELS; level++) {
				float factor = (float)level / (FOG_LUT_LEVELS-1);
				// lerp each RGB component
				unsigned char r = (unsigned char)(r1 + factor * (r2 - r1));
				unsigned char g = (unsigned char)(g1 + factor * (g2 - g1));
				unsigned char b = (unsigned char)(b1 + factor * (b2 - b1));
				unsigned char mixed_index = convfunc(r, g, b);
				color_mix_lut[c1][c2][level] = mixed_index;
			}
		}
	}
	fog_lut_built = 1;
}

void R_InitFog()
{
	for (int i = 0; i < RANDARR_SIZE; ++i) // fog bias array
		randarr[i] = (lfsr_random() & 0xFFFF) / 65535.0f; // LFSR random number normalized to [0,1]
	if (!fog_lut_built)
		build_color_mix_lut(0);
	Fog_SetPalIndex(0);
	fog_initialized = 1;
}

float compute_fog(int z) {
	const int fog_scale = 18;
	z = fog_scale > z ? fog_scale : z; // prevent distant objects from getting no fog
	z /= fog_scale;
	return expf(-(1.0f-fog_density) * (1.0f-fog_density) * (float)(z * z));
}

void R_DrawFog() {
	if (!fog_density || r_nofog.value)
		return;
	if (!fog_initialized)
		R_InitFog();
	sb_updates = 0; // draw sbar over fog
	int style = r_fogstyle.value;
	int j = 0;
        for (int y = scr_vrect.y; y < scr_vrect.y + scr_vrect.height; ++y) {
        for (int x = scr_vrect.x; x < scr_vrect.x + scr_vrect.width; ++x) {
		int i = x + y * vid.width;
		int bias = randarr[(scr_vrect.width*scr_vrect.height - j)%RANDARR_SIZE] * 10;
		++j;
		float fog_factor = compute_fog(d_pzbuffer[i] + bias);
		switch (style) {
			case 0: // noisy
				float random_val = (lfsr_random() & 0xFFFF) / 65535.0f;
				if (random_val < fog_factor)
					((unsigned char *)(screen->pixels))[i] = fog_pal_index;
				break;
			case 1: // dither
				if (dither(x, y, fog_factor))
					((unsigned char *)(screen->pixels))[i] = fog_pal_index;
				break;
			case 2: // dither + noise
				fog_factor += (randarr[i%RANDARR_SIZE] - 0.5f) * 0.2f;
				fog_factor = fog_factor < 0.1f ? 0 : fog_factor;
				if (dither(x, y, fog_factor))
					((unsigned char *)(screen->pixels))[i] = fog_pal_index;
				break;
			default:
			case 3: // mix
				if (fog_factor < 1) 
					((unsigned char *)(screen->pixels))[i] =
						color_mix_lut[((unsigned char *)(screen->pixels))[i]]
							[fog_pal_index][(int)(fog_factor*FOG_LUT_LEVELS)];
				break;
		}
	}
	}
}
