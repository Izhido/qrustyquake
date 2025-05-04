// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.
// Copyright (C) 2002-2009 John Fitzgibbons and others
// Copyright (C) 2010-2014 QuakeSpasm developers

// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include "quakedef.h"
#include "r_local.h"

#define	MAX_MOD_KNOWN 32768 /*johnfitz -- was 512 */
#define ANIM_CYCLE 2
#define NL_PRESENT 0 // values for model_t's needload
#define NL_NEEDS_LOADED 1
#define NL_UNREFERENCED 2

typedef struct vispatch_s // External VIS file support
{
	char mapname[32];
	int filelen; // length of data after header (VIS+Leafs)
} vispatch_t;
#define VISPATCH_HEADER_LEN 36

aliashdr_t *pheader;
stvert_t stverts[MAXALIASVERTS];
mtriangle_t triangles[MAXALIASTRIS];
// a pose is a single set of vertexes. a frame may be an animating sequence of poses
trivertx_t *poseverts[MAXALIASFRAMES];
static int posenum;

model_t*	loadmodel;
static char	loadname[32];	// for hunk tags
static byte	*mod_base;

static void Mod_LoadSpriteModel (model_t *mod, void *buffer);
static void Mod_LoadBrushModel (model_t *mod, void *buffer);
static void Mod_LoadAliasModel (model_t *mod, void *buffer);
static model_t *Mod_LoadModel (model_t *mod, qboolean crash);

static void Mod_Print ();

static cvar_t	external_ents = {"external_ents", "1", CVAR_ARCHIVE};
static cvar_t	external_vis = {"external_vis", "1", CVAR_ARCHIVE};
static cvar_t	external_textures = {"external_textures", "1", CVAR_ARCHIVE};

static byte	*mod_novis;
static int	mod_novis_capacity;

static byte	*mod_decompressed;
static int	mod_decompressed_capacity;

static model_t	mod_known[MAX_MOD_KNOWN];
static int		mod_numknown;

extern texture_t	*r_notexture_mip;
texture_t	*r_notexture_mip2; //johnfitz -- used for non-lightmapped surfs with a missing texture

void Mod_Init ()
{
	Cvar_RegisterVariable (&external_vis);
	Cvar_RegisterVariable (&external_ents);
	Cvar_RegisterVariable (&external_textures);
	Cmd_AddCommand ("mcache", Mod_Print);
	//johnfitz -- create notexture miptex
	r_notexture_mip = (texture_t *) Hunk_AllocName (sizeof(texture_t), "r_notexture_mip");
	strcpy (r_notexture_mip->name, "notexture");
	r_notexture_mip->height = r_notexture_mip->width = 32;
	r_notexture_mip2 = (texture_t *) Hunk_AllocName (sizeof(texture_t), "r_notexture_mip2");
	strcpy (r_notexture_mip2->name, "notexture2");
	r_notexture_mip2->height = r_notexture_mip2->width = 32;
	//johnfitz
}

void *Mod_Extradata (model_t *mod)
{ // Caches the data if needed
	void *r = Cache_Check (&mod->cache);
	if (r)
		return r;
	Mod_LoadModel (mod, true);
	if (!mod->cache.data)
		Sys_Error ("Mod_Extradata: caching failed");
	return mod->cache.data;
}

mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model)
{
	if (!model || !model->nodes)
		Sys_Error ("Mod_PointInLeaf: bad model");
	mnode_t *node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node;
		mplane_t *plane = node->plane;
		float d = DotProduct (p,plane->normal) - plane->dist;
		node = node->children[d > 0 ? 0 : 1];
	}
	return NULL; // never reached
}

static byte *Mod_DecompressVis (byte *in, model_t *model)
{
	int row = (model->numleafs+7)>>3;
	if (mod_decompressed == NULL || row > mod_decompressed_capacity) {
		mod_decompressed_capacity = row;
		mod_decompressed = (byte *) realloc (mod_decompressed, mod_decompressed_capacity);
		if (!mod_decompressed)
			Sys_Error ("Mod_DecompressVis: realloc() failed on %d bytes", mod_decompressed_capacity);
	}
	byte *out = mod_decompressed;
	byte *outend = mod_decompressed + row;
	if (!in) { // no vis info, so make all visible
		while (row) {
			*out++ = 0xff;
			row--;
		}
		return mod_decompressed;
	}
	do {
		if (*in) {
			*out++ = *in++;
			continue;
		}
		int c = in[1];
		in += 2;
		while (c) {
			if (out == outend) {
				if(!model->viswarn) {
					model->viswarn = true;
					printf("Mod_DecompressVis: output overrun on model \"%s\"\n", model->name);
				}
				return mod_decompressed;
			}
			*out++ = 0;
			c--;
		}
	} while (out - mod_decompressed < row);
	return mod_decompressed;
}

byte *Mod_LeafPVS (mleaf_t *leaf, model_t *model)
{
	if (leaf == model->leafs)
		return Mod_NoVisPVS (model);
	return Mod_DecompressVis (leaf->compressed_vis, model);
}


byte *Mod_NoVisPVS (model_t *model)
{
	int pvsbytes = (model->numleafs+7)>>3;
	if (mod_novis == NULL || pvsbytes > mod_novis_capacity) {
		mod_novis_capacity = pvsbytes;
		mod_novis = (byte *) realloc (mod_novis, mod_novis_capacity);
		if (!mod_novis)
			Sys_Error ("Mod_NoVisPVS: realloc() failed on %d bytes", mod_novis_capacity);
		memset(mod_novis, 0xff, mod_novis_capacity);
	}
	return mod_novis;
}

void Mod_ClearAll ()
{
	int i = 0;
	model_t *mod = mod_known;
	for (; i < mod_numknown; i++, mod++) {
                mod->needload = NL_UNREFERENCED;
                if (mod->type == mod_sprite)
                        mod->cache.data = NULL;
        }
}

void Mod_ResetAll ()
{
	//ericw -- free alias model VBOs
	//GLMesh_DeleteVertexBuffers ();
	int i = 0;
	model_t *mod = mod_known;
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++) {
	//	if (!mod->needload) //otherwise Mod_ClearAll() did it already
	//		TexMgr_FreeTexturesForOwner (mod);
		memset(mod, 0, sizeof(model_t));
	}
	mod_numknown = 0;
}

static model_t *Mod_FindName (const char *name)
{
	if (!name[0])
		Sys_Error ("Mod_FindName: NULL name"); //johnfitz -- was "Mod_ForName"
	// search the currently loaded models
	int i = 0;
	model_t *mod = mod_known;
	for (; i<mod_numknown ; i++, mod++)
		if (!strcmp (mod->name, name))
			break;
	if (i == mod_numknown) {
		if (mod_numknown == MAX_MOD_KNOWN)
			Sys_Error ("mod_numknown == MAX_MOD_KNOWN");
		q_strlcpy (mod->name, name, MAX_QPATH);
		mod->needload = true;
		mod_numknown++;
	}
	return mod;
}

void Mod_TouchModel (const char *name)
{
	model_t	*mod = Mod_FindName (name);
	if (!mod->needload == NL_PRESENT && mod->type == mod_alias)
		Cache_Check (&mod->cache);
}


static model_t *Mod_LoadModel (model_t *mod, qboolean crash)
{ // Loads a model into the cache
	byte stackbuf[1024]; // avoid dirtying the cache heap
	if (mod->type == mod_alias) {
		if (Cache_Check (&mod->cache)) {
			mod->needload = NL_PRESENT;
			return mod;
		}
	}
	else if (mod->needload == NL_PRESENT)
			return mod;
	// because the world is so huge, load it one piece at a time
	byte *buf = COM_LoadStackFile (mod->name, stackbuf, sizeof(stackbuf), &mod->path_id);
	if (!buf) {
		if (crash) //johnfitz -- was "Mod_NumForName"
			Host_Error ("Mod_LoadModel: %s not found", mod->name);
		return NULL;
	}
	// allocate a new model
	COM_FileBase (mod->name, loadname, sizeof(loadname));
	loadmodel = mod;
	// fill it in, call the apropriate loader
	mod->needload = false;
	int mod_type = (buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24));
	switch (mod_type) {
	case IDPOLYHEADER:
		Mod_LoadAliasModel (mod, buf);
		break;
	case IDSPRITEHEADER:
		Mod_LoadSpriteModel (mod, buf);
		break;
	default:
		Mod_LoadBrushModel (mod, buf);
		break;
	}
	return mod;
}

model_t *Mod_ForName (const char *name, qboolean crash)
{ // Loads in a model for the given name
	model_t	*mod = Mod_FindName (name);
	return Mod_LoadModel (mod, crash);
}

static wad_t *Mod_LoadWadFiles (model_t *mod)
{ // load all of the wads listed in the worldspawn "wad" field
	char key[128], value[4096];
	if (!external_textures.value)
		return NULL;
	// disregard if this isn't the world model
	if (strcmp (mod->name, sv.modelname))
		return NULL;
	const char *data = COM_Parse (mod->entities);
	if (!data)
		return NULL; // error
	if (com_token[0] != '{')
		return NULL; // error
	while (1) {
		data = COM_Parse (data);
		if (!data)
			return NULL; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_ParseEx (data, CPE_ALLOWTRUNC);
		if (!data)
			return NULL; // error
		q_strlcpy (value, com_token, sizeof (value));
		if (!strcmp ("wad", key)) {
			return W_LoadWadList (value);
		}
	}
	return NULL;
}

static texture_t *Mod_LoadWadTexture (model_t *mod, wad_t *wads, const char *name, qboolean *out_pal, int *out_pixels)
{ // look for an external texture in any of the loaded map wads
	// look for the lump in any of the loaded wads
	wad_t *wad;
	lumpinfo_t *info = W_GetLumpinfoList (wads, name, &wad);
	// ensure we're dealing with a miptex
	if (!info || (info->type != TYP_MIPTEX && (wad->id != WADID_VALVE || info->type != TYP_MIPTEX_PALETTE)))
	{
		printf ("Missing texture %s in %s!\n", name, mod->name);
		return NULL;
	}
	// override the texture from the bsp file
	miptex_t mt;
	FS_fseek (&wad->fh, info->filepos, SEEK_SET);
	FS_fread (&mt, 1, sizeof (miptex_t), &wad->fh);
	mt.width = LittleLong (mt.width);
	mt.height = LittleLong (mt.height);
	for (int i = 0; i < MIPLEVELS; i++)
		mt.offsets[i] = LittleLong (mt.offsets[i]);
	if (mt.width == 0 || mt.height == 0) {
		printf ("Zero sized texture %s in %s!\n", mt.name, wad->name);
		return NULL;
	}
	if ((mt.width & 15) || (mt.height & 15)) {
		if (mod->bspversion != BSPVERSION_QUAKE64)
			printf ("Texture %s (%d x %d) is not 16 aligned\n", mt.name, mt.width, mt.height);
		return NULL;
	}
	qboolean pal = wad->id == WADID_VALVE && info->type == TYP_MIPTEX_PALETTE;
	int pixels = mt.width * mt.height; // only copy the first mip, the rest are auto-generated
	int pixels_pos = info->filepos + sizeof (miptex_t);
	// valve textures have a color palette immediately following the pixels
	int palette, tex_bytes, lump_bytes, palette_pos;
	if (pal) {
		palette_pos = pixels_pos + pixels / 64 * 85;
		// add space for the color count
		palette = 2;
		if ((pixels / 64 * 85 + 2) <= info->size) {
			// the palette is basically garunteed to be 256 colors but,
			// we might as well use the value since it *does* exist
			FS_fseek (&wad->fh, palette_pos, SEEK_SET);
			unsigned short colors;
			FS_fread (&colors, 1, 2, &wad->fh);
			colors = LittleShort (colors);
			// add space for the color palette
			palette += colors * 3;
		}
		tex_bytes = pixels + palette;
		lump_bytes = pixels / 64 * 85 + palette;
	}
	else {
		palette_pos = 0;
		palette = 0;
		tex_bytes = pixels;
		lump_bytes = pixels;
	}
	texture_t *tx = (texture_t *)Hunk_AllocName (sizeof (texture_t) + tex_bytes, loadname);
	memcpy (tx->name, mt.name, sizeof (tx->name));
	tx->width = mt.width;
	tx->height = mt.height;
	// the pixels immediately follow the structures
	// check for pixels extending past the end of the lump
	if (lump_bytes > info->size) {
		printf ("Texture %s extends past end of lump\n", mt.name);
		lump_bytes = info->size;
		if (pal)
			palette = q_min(palette, q_max(0, lump_bytes - pixels / 64 * 85));
		pixels = q_min(pixels, lump_bytes);
	}
	tx->update_warp = false; //johnfitz
	//tx->warpimage = NULL; //johnfitz
	//tx->fullbright = NULL; //johnfitz
	tx->shift = 0;	// Q64 only
	FS_fseek (&wad->fh, pixels_pos, SEEK_SET);
	FS_fread (tx + 1, 1, pixels, &wad->fh);
	if (pal && palette) {
		FS_fseek (&wad->fh, palette_pos, SEEK_SET);
		FS_fread ((byte *)(tx + 1) + pixels, 1, palette, &wad->fh);
	}
	*out_pal = pal;
	*out_pixels = pixels;
	return tx;
}

static qboolean Mod_CheckFullbrights (byte *pixels, int count)
{ // -- johnfitz
	for (int i = 0; i < count; i++) {
		if (*pixels++ > 223)
			return true;
	}
	return false;
}

static qboolean Mod_CheckFullbrightsValve (char *name, byte *pixels, int count)
{
	if (name[0] == '~' || (name[2] == '~' && name[0] == '+'))
		return Mod_CheckFullbrights (pixels, count);
	return false;
}


static qboolean Mod_CheckAnimTextureArrayQ64(texture_t *anims[], int numTex)
{ // Quake64 bsp - check if we have any missing textures in the array

	for (int i = 0; i < numTex; i++)
		if (!anims[i])
			return false;
	return true;
}

void Mod_LoadTextures(lump_t *l)
{
        texture_t *anims[10];
        texture_t *altanims[10];
        if (!l->filelen) {
                loadmodel->textures = NULL;
                return;
        }
        dmiptexlump_t *m = (dmiptexlump_t *) (mod_base + l->fileofs);
        m->nummiptex = LittleLong(m->nummiptex);
        loadmodel->numtextures = m->nummiptex;
        loadmodel->textures = Hunk_AllocName(m->nummiptex *
                        sizeof(*loadmodel->textures), loadname);
        for (int i = 0; i < m->nummiptex; i++) {
                m->dataofs[i] = LittleLong(m->dataofs[i]);
                if (m->dataofs[i] == -1)
                        continue;
                miptex_t *mt = (miptex_t *) ((byte *) m + m->dataofs[i]);
                mt->width = LittleLong(mt->width);
                mt->height = LittleLong(mt->height);
                for (int j = 0; j < MIPLEVELS; j++)
                        mt->offsets[j] = LittleLong(mt->offsets[j]);
                if ((mt->width & 15) || (mt->height & 15))
                        Sys_Error("Texture %s is not 16 aligned", mt->name);
                int pixels = mt->width * mt->height / 64 * 85;
                texture_t *tx = Hunk_AllocName(sizeof(texture_t) + pixels, loadname);
                loadmodel->textures[i] = tx;
                memcpy(tx->name, mt->name, sizeof(tx->name));
                tx->width = mt->width;
                tx->height = mt->height;
                for (int j = 0; j < MIPLEVELS; j++)
                        tx->offsets[j] = mt->offsets[j] + sizeof(texture_t) -
                                sizeof(miptex_t);
                // the pixels immediately follow the structures
                memcpy(tx + 1, mt + 1, pixels);
                if (!Q_strncmp(mt->name, "sky", 3))
                        R_InitSky(tx);
        }
        for (int i = 0; i < m->nummiptex; i++) { // sequence the animations
                texture_t *tx = loadmodel->textures[i];
                if (!tx || tx->name[0] != '+')
                        continue;
                if (tx->anim_next)
                        continue; // allready sequenced
                // find the number of frames in the animation
                memset(anims, 0, sizeof(anims));
                memset(altanims, 0, sizeof(altanims));
                int max = tx->name[1];
                int altmax = 0;
                if (max >= 'a' && max <= 'z')
                        max -= 'a' - 'A';
                if (max >= '0' && max <= '9') {
                        max -= '0';
                        altmax = 0;
                        anims[max] = tx;
                        max++;
                } else if (max >= 'A' && max <= 'J') {
                        altmax = max - 'A';
                        max = 0;
                        altanims[altmax] = tx;
                        altmax++;
                } else
                        Sys_Error("Bad animating texture %s", tx->name);
                for (int j = i + 1; j < m->nummiptex; j++) {
                        texture_t *tx2 = loadmodel->textures[j];
                        if (!tx2 || tx2->name[0] != '+')
                                continue;
                        if (strcmp(tx2->name + 2, tx->name + 2))
                                continue;
                        int num = tx2->name[1];
                        if (num >= 'a' && num <= 'z')
                                num -= 'a' - 'A';
                        if (num >= '0' && num <= '9') {
                                num -= '0';
                                anims[num] = tx2;
                                if (num + 1 > max)
                                        max = num + 1;
                        } else if (num >= 'A' && num <= 'J') {
                                num = num - 'A';
                                altanims[num] = tx2;
                                if (num + 1 > altmax)
                                        altmax = num + 1;
                        } else
                                Sys_Error("Bad animating texture %s", tx->name);
                }
                for (int j = 0; j < max; j++) { // link them all together
                        texture_t *tx2 = anims[j];
                        if (!tx2)
                                Sys_Error("Missing frame %i of %s", j,tx->name);
                        tx2->anim_total = max * ANIM_CYCLE;
                        tx2->anim_min = j * ANIM_CYCLE;
                        tx2->anim_max = (j + 1) * ANIM_CYCLE;
                        tx2->anim_next = anims[(j + 1) % max];
                        if (altmax)
                                tx2->alternate_anims = altanims[0];
                }
                for (int j = 0; j < altmax; j++) {
                        texture_t *tx2 = altanims[j];
                        if (!tx2)
                                Sys_Error("Missing frame %i of %s", j,tx->name);
                        tx2->anim_total = altmax * ANIM_CYCLE;
                        tx2->anim_min = j * ANIM_CYCLE;
                        tx2->anim_max = (j + 1) * ANIM_CYCLE;
                        tx2->anim_next = altanims[(j + 1) % altmax];
                        if (max)
                                tx2->alternate_anims = anims[0];
                }
        }
}

/*TODO
static void Mod_LoadTextures (lump_t *l)
{
	int		i, j, pixels, num, maxanim, altmax, tex_bytes, lump_bytes;
	miptex_t	*mt;
	texture_t	*tx, *tx2;
	texture_t	*anims[10];
	texture_t	*altanims[10];
	dmiptexlump_t	*m;
//johnfitz -- more variables
	char		texturename[64];
	int			nummiptex;
	src_offset_t		offset;
	int			mark, fwidth, fheight;
	char		filename[MAX_OSPATH], mapname[MAX_OSPATH];
	byte		*data;
	extern byte *hunk_base;
//johnfitz
	unsigned int	flags;
	wad_t		*wads;
	qboolean from_wad;
	enum srcformat fmt;
	qboolean fbright;
	const char *source_file;
#ifdef BSP29_VALVE
	int palette;
	byte *palette_p;
	unsigned short colors;
#endif
	qboolean pal;

	//johnfitz -- don't return early if no textures; still need to create dummy texture
	if (!l->filelen)
	{
		Con_Printf ("Mod_LoadTextures: no textures in bsp file\n");
		nummiptex = 0;
		m = NULL; // avoid bogus compiler warning
	}
	else
	{
		m = (dmiptexlump_t *)(mod_base + l->fileofs);
		m->nummiptex = LittleLong (m->nummiptex);
		nummiptex = m->nummiptex;
	}
	//johnfitz

	loadmodel->numtextures = nummiptex + 2; //johnfitz -- need 2 dummy texture chains for missing textures
	loadmodel->textures = (texture_t **) Hunk_AllocName (loadmodel->numtextures * sizeof(*loadmodel->textures) , loadname);

	// load any wads this map may need to load external textures from
	wads = Mod_LoadWadFiles (loadmodel);

	for (i=0 ; i<nummiptex ; i++)
	{
		m->dataofs[i] = LittleLong(m->dataofs[i]);
		if (m->dataofs[i] == -1)
			continue;
		mt = (miptex_t *)((byte *)m + m->dataofs[i]);
		mt->width = LittleLong (mt->width);
		mt->height = LittleLong (mt->height);
		for (j=0 ; j<MIPLEVELS ; j++)
			mt->offsets[j] = LittleLong (mt->offsets[j]);

		if (mt->width == 0 || mt->height == 0)
		{
			printf ("Zero sized texture %s in %s!\n", mt->name, loadmodel->name);
			continue;
		}

		if ( (mt->width & 15) || (mt->height & 15) )
		{
			if (loadmodel->bspversion != BSPVERSION_QUAKE64)
				printf ("Texture %s (%d x %d) is not 16 aligned\n", mt->name, mt->width, mt->height);
		}
		// an offset of zero indicates an external texture
		from_wad = mt->offsets[0] == 0;

		if (from_wad)
		{
			tx = (texture_t *)Mod_LoadWadTexture (loadmodel, wads, mt->name, &pal, &pixels);
			if (!tx)
				continue;
			loadmodel->textures[i] = tx;
			goto _load_texture;
		}

#ifdef BSP29_VALVE
		pal = loadmodel->bspversion == BSPVERSION_VALVE;
#else
		pal = false;
#endif

		pixels = mt->width*mt->height; // only copy the first mip, the rest are auto-generated
#ifdef BSP29_VALVE
		// valve textures have a color palette immediately following the pixels
		if (pal)
		{
			palette_p = (byte *)(mt + 1) + pixels / 64 * 85;

			// add space for the color count
			palette = 2;

			if ((palette_p + 2) <= (mod_base + l->fileofs + l->filelen))
			{
				// the palette is basically garunteed to be 256 colors but,
				// we might as well use the value since it *does* exist
				memcpy (&colors, palette_p, 2);
				colors = LittleShort (colors);
				// add space for the color palette
				palette += colors * 3;
			}

			tex_bytes = pixels + palette;
			lump_bytes = pixels / 64 * 85 + palette;
		}
		else
#endif
		{
#ifdef BSP29_VALVE
			palette_p = NULL;
			palette = 0;
#endif
			tex_bytes = pixels;
			lump_bytes = pixels;
		}
		tx = (texture_t *) Hunk_AllocName (sizeof(texture_t) +tex_bytes, loadname );
		loadmodel->textures[i] = tx;

		memcpy (tx->name, mt->name, sizeof(tx->name));
		tx->width = mt->width;
		tx->height = mt->height;
		// the pixels immediately follow the structures

		// ericw -- check for pixels extending past the end of the lump.
		// appears in the wild; e.g. jam2_tronyn.bsp (func_mapjam2),
		// kellbase1.bsp (quoth), and can lead to a segfault if we read past
		// the end of the .bsp file buffer
		if (((byte*)(mt+1) + lump_bytes) > (mod_base + l->fileofs + l->filelen))
		{
			Con_DPrintf("Texture %s extends past end of lump\n", mt->name);
			lump_bytes = q_max(0L, (long)((mod_base + l->fileofs + l->filelen) - (byte*)(mt+1)));
#ifdef BSP29_VALVE
			if (pal)
				palette = q_min(palette, q_max(0, lump_bytes - pixels / 64 * 85));
#endif
			pixels = q_min(pixels, lump_bytes);
		}

		tx->update_warp = false; //johnfitz
		tx->warpimage = NULL; //johnfitz
		tx->fullbright = NULL; //johnfitz
		tx->shift = 0;	// Q64 only

		if (loadmodel->bspversion != BSPVERSION_QUAKE64)
		{
			memcpy ( tx+1, mt+1, pixels);
#ifdef BSP29_VALVE
			if (pal && palette)
				memcpy ((byte *)(tx + 1) + pixels, palette_p, palette);
#endif
		}
		else
		{ // Q64 bsp
			miptex64_t *mt64 = (miptex64_t *)mt;
			tx->shift = LittleLong (mt64->shift);
			memcpy ( tx+1, mt64+1, pixels);
		}

_load_texture:

		//johnfitz -- lots of changes
		if (!isDedicated) //no texture uploading for dedicated server
		{
#ifdef BSP29_VALVE
			if (loadmodel->bspversion != BSPVERSION_VALVE && !strncasecmp(tx->name,"sky",3))
#else
			if (!strncasecmp(tx->name,"sky",3)) //sky texture //also note -- was Q_strncmp, changed to match qbsp
#endif
			{
				if (loadmodel->bspversion == BSPVERSION_QUAKE64)
					Sky_LoadTextureQ64 (loadmodel, tx);
				else
					Sky_LoadTexture (loadmodel, tx);
			}
			else if (tx->name[0] == '*' || tx->name[0] == '!') //warping texture
			{
				//external textures -- first look in "textures/mapname/" then look in "textures/"
				mark = Hunk_LowMark();
				COM_StripExtension (loadmodel->name + 5, mapname, sizeof(mapname));
				snprintf (filename, sizeof(filename), "textures/%s/#%s", mapname, tx->name+1); //this also replaces the '*' with a '#'
				data = Image_LoadImage (filename, &fwidth, &fheight);
				if (!data)
				{
					snprintf (filename, sizeof(filename), "textures/#%s", tx->name+1);
					data = Image_LoadImage (filename, &fwidth, &fheight);
				}

				//now load whatever we found
				if (data) //load external image
				{
					q_strlcpy (texturename, filename, sizeof(texturename));
					tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, fwidth, fheight,
						SRC_RGBA, data, filename, 0, TEXPREF_NONE);
				}
				else //use the texture from the bsp file
				{
					snprintf (texturename, sizeof(texturename), "%s:%s", loadmodel->name, tx->name);

					if (from_wad || pal)
					{
						source_file = "";
						offset = (src_offset_t)(tx + 1);
					}
					else
					{
						source_file = loadmodel->name;
						offset = (src_offset_t)(mt+1) - (src_offset_t)mod_base;
					}

					fmt = pal ? SRC_INDEXED_PALETTE : SRC_INDEXED;

					tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
						fmt, (byte *)(tx+1), source_file, offset, TEXPREF_NONE);
				}

				//now create the warpimage, using dummy data from the hunk to create the initial image
				Hunk_Alloc (gl_warpimagesize*gl_warpimagesize*4); //make sure hunk is big enough so we don't reach an illegal address
				Hunk_FreeToLowMark (mark);
				snprintf (texturename, sizeof(texturename), "%s_warp", texturename);
				flags = TEXPREF_NOPICMIP | TEXPREF_WARPIMAGE;
				if (GL_GenerateMipmap)
					flags |= TEXPREF_MIPMAP;
				tx->warpimage = TexMgr_LoadImage (loadmodel, texturename, gl_warpimagesize,
					gl_warpimagesize, SRC_RGBA, hunk_base, "", (src_offset_t)hunk_base, flags);
				tx->update_warp = true;
			}
			else //regular texture
			{
				// ericw -- fence textures
				int	extraflags;

				extraflags = 0;
				if (tx->name[0] == '{')
					extraflags |= TEXPREF_ALPHA;
				// ericw

				//external textures -- first look in "textures/mapname/" then look in "textures/"
				mark = Hunk_LowMark ();
				COM_StripExtension (loadmodel->name + 5, mapname, sizeof(mapname));
				snprintf (filename, sizeof(filename), "textures/%s/%s", mapname, tx->name);
				data = Image_LoadImage (filename, &fwidth, &fheight);
				if (!data)
				{
					snprintf (filename, sizeof(filename), "textures/%s", tx->name);
					data = Image_LoadImage (filename, &fwidth, &fheight);
				}

				//now load whatever we found
				if (data) //load external image
				{
					char filename2[MAX_OSPATH];
					tx->gltexture = TexMgr_LoadImage (loadmodel, filename, fwidth, fheight,
						SRC_RGBA, data, filename, 0, TEXPREF_MIPMAP | extraflags );

					//now try to load glow/luma image from the same place
					Hunk_FreeToLowMark (mark);
					snprintf (filename2, sizeof(filename2), "%s_glow", filename);
					data = Image_LoadImage (filename2, &fwidth, &fheight);
					if (!data)
					{
						snprintf (filename2, sizeof(filename2), "%s_luma", filename);
						data = Image_LoadImage (filename2, &fwidth, &fheight);
					}

					if (data)
						tx->fullbright = TexMgr_LoadImage (loadmodel, filename2, fwidth, fheight,
							SRC_RGBA, data, filename2, 0, TEXPREF_MIPMAP | extraflags );
				}
				else //use the texture from the bsp file
				{
					snprintf (texturename, sizeof(texturename), "%s:%s", loadmodel->name, tx->name);

					if (from_wad || pal)
					{
						source_file = "";
						offset = (src_offset_t)(tx + 1);
					}
					else
					{
						source_file = loadmodel->name;
						offset = (src_offset_t)(mt+1) - (src_offset_t)mod_base;
					}

					if (pal)
					{
						fmt = SRC_INDEXED_PALETTE;
						fbright = Mod_CheckFullbrightsValve (tx->name, (byte *)(tx + 1), pixels);
					}
					else
					{
						fmt = SRC_INDEXED;
						fbright = Mod_CheckFullbrights ((byte *)(tx + 1), pixels);
					}

					if (fbright)
					{
						tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
							fmt, (byte *)(tx+1), source_file, offset, TEXPREF_MIPMAP | TEXPREF_NOBRIGHT | extraflags);
						snprintf (texturename, sizeof(texturename), "%s:%s_glow", loadmodel->name, tx->name);
						tx->fullbright = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
							fmt, (byte *)(tx+1), source_file, offset, TEXPREF_MIPMAP | TEXPREF_FULLBRIGHT | extraflags);
					}
					else
					{
						tx->gltexture = TexMgr_LoadImage (loadmodel, texturename, tx->width, tx->height,
							fmt, (byte *)(tx+1), source_file, offset, TEXPREF_MIPMAP | extraflags);
					}
				}
				Hunk_FreeToLowMark (mark);
			}
		}
		//johnfitz
	}

	// we no longer need the wads after this point
	//FIXMEW_FreeWadList (wads);

	//johnfitz -- last 2 slots in array should be filled with dummy textures
	loadmodel->textures[loadmodel->numtextures-2] = r_notexture_mip; //for lightmapped surfs
	loadmodel->textures[loadmodel->numtextures-1] = r_notexture_mip2; //for SURF_DRAWTILED surfs

//
// sequence the animations
//
	for (i=0 ; i<nummiptex ; i++)
	{
		tx = loadmodel->textures[i];
		if (!tx || tx->name[0] != '+')
			continue;
		if (tx->anim_next)
			continue;	// already sequenced

	// find the number of frames in the animation
		memset (anims, 0, sizeof(anims));
		memset (altanims, 0, sizeof(altanims));

		maxanim = tx->name[1];
		altmax = 0;
		if (maxanim >= 'a' && maxanim <= 'z')
			maxanim -= 'a' - 'A';
		if (maxanim >= '0' && maxanim <= '9')
		{
			maxanim -= '0';
			altmax = 0;
			anims[maxanim] = tx;
			maxanim++;
		}
		else if (maxanim >= 'A' && maxanim <= 'J')
		{
			altmax = maxanim - 'A';
			maxanim = 0;
			altanims[altmax] = tx;
			altmax++;
		}
		else
			Sys_Error ("Bad animating texture %s", tx->name);

		for (j=i+1 ; j<nummiptex ; j++)
		{
			tx2 = loadmodel->textures[j];
			if (!tx2 || tx2->name[0] != '+')
				continue;
			if (strcmp (tx2->name+2, tx->name+2))
				continue;

			num = tx2->name[1];
			if (num >= 'a' && num <= 'z')
				num -= 'a' - 'A';
			if (num >= '0' && num <= '9')
			{
				num -= '0';
				anims[num] = tx2;
				if (num+1 > maxanim)
					maxanim = num + 1;
			}
			else if (num >= 'A' && num <= 'J')
			{
				num = num - 'A';
				altanims[num] = tx2;
				if (num+1 > altmax)
					altmax = num+1;
			}
			else
				Sys_Error ("Bad animating texture %s", tx->name);
		}

		if (loadmodel->bspversion == BSPVERSION_QUAKE64 && !Mod_CheckAnimTextureArrayQ64(anims, maxanim))
			continue; // Just pretend this is a normal texture

#define	ANIM_CYCLE	2
	// link them all together
		for (j=0 ; j<maxanim ; j++)
		{
			tx2 = anims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s",j, tx->name);
			tx2->anim_total = maxanim * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = anims[ (j+1)%maxanim ];
			if (altmax)
				tx2->alternate_anims = altanims[0];
		}
		for (j=0 ; j<altmax ; j++)
		{
			tx2 = altanims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s",j, tx->name);
			tx2->anim_total = altmax * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = altanims[ (j+1)%altmax ];
			if (maxanim)
				tx2->alternate_anims = anims[0];
		}
	}
}TODO*/

static void Mod_LoadLighting (lump_t *l)
{ // johnfitz -- replaced with lit support code via lordhavoc
	int i, mark;
	byte *in, *out, *data;
	byte d, q64_b0, q64_b1;
	char litfilename[MAX_OSPATH];
	unsigned int path_id;

	loadmodel->lightdata = NULL;
	// LordHavoc: check for a .lit file
	q_strlcpy(litfilename, loadmodel->name, sizeof(litfilename));
	COM_StripExtension(litfilename, litfilename, sizeof(litfilename));
	q_strlcat(litfilename, ".lit", sizeof(litfilename));
	mark = Hunk_LowMark();
	data = (byte*) COM_LoadHunkFile (litfilename, &path_id);
	if (data) {
		// use lit file only from the same gamedir as the map
		// itself or from a searchpath with higher priority.
		if (path_id < loadmodel->path_id) {
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("ignored %s from a gamedir with lower priority\n", litfilename);
		}
		else if (data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T') {
			i = LittleLong(((int *)data)[1]);
			if (i == 1) {
				if (8+l->filelen*3 == com_filesize) {
					printf("%s loaded\n", litfilename);
					loadmodel->lightdata = data + 8;
					return;
				}
				Hunk_FreeToLowMark(mark);
				Con_Printf("Outdated .lit file (%s should be %u bytes, not %u)\n", litfilename, 8+l->filelen*3, com_filesize);
			}
			else {
				Hunk_FreeToLowMark(mark);
				Con_Printf("Unknown .lit file version (%d)\n", i);
			}
		}
		else {
			Hunk_FreeToLowMark(mark);
			Con_Printf("Corrupt .lit file (old version?), ignoring\n");
		}
	}
	// LordHavoc: no .lit found, expand the white lighting data to color
	if (!l->filelen)
		return;
	// Quake64 bsp lighmap data
	if (loadmodel->bspversion == BSPVERSION_QUAKE64) {
		// RGB lightmap samples are packed in 16bits.
		// RRRRR GGGGG BBBBBB

		loadmodel->lightdata = (byte *) Hunk_AllocName ( (l->filelen / 2)*3, litfilename);
		in = mod_base + l->fileofs;
		out = loadmodel->lightdata;

		for (i = 0;i < (l->filelen / 2) ;i++) {
			q64_b0 = *in++;
			q64_b1 = *in++;

			*out++ = q64_b0 & 0xf8;// 0b11111000
			*out++ = ((q64_b0 & 0x07) << 5) + ((q64_b1 & 0xc0) >> 5);// 0b00000111, 0b11000000
			*out++ = (q64_b1 & 0x3f) << 2;// 0b00111111
		}
		return;
	}

#ifdef BSP29_VALVE
	if (loadmodel->bspversion == BSPVERSION_VALVE)
	{
		// lightmap samples are already stored as rgb
		loadmodel->lightdata = (byte *)Hunk_AllocName (l->filelen, litfilename);
		memcpy (loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
		return;
	}
#endif

	loadmodel->lightdata = (byte *) Hunk_AllocName ( l->filelen*3, litfilename);
	in = loadmodel->lightdata + l->filelen*2; // place the file at the end, so it will not be overwritten until the very last write
	out = loadmodel->lightdata;
	memcpy (in, mod_base + l->fileofs, l->filelen);
	for (i = 0;i < l->filelen;i++)
	{
		d = *in++;
		*out++ = d;
		*out++ = d;
		*out++ = d;
	}
}

static void Mod_LoadVisibility (lump_t *l)
{
	loadmodel->viswarn = false;
	if (!l->filelen)
	{
		loadmodel->visdata = NULL;
		return;
	}
	loadmodel->visdata = (byte *) Hunk_AllocName (l->filelen, loadname);
	memcpy (loadmodel->visdata, mod_base + l->fileofs, l->filelen);
}

static void Mod_LoadEdges (lump_t *l, int bsp2)
{
        if (bsp2) {
                dledge_t *in = (dledge_t *)(mod_base + l->fileofs);
                if (l->filelen % sizeof(*in))
                        Sys_Error ("MOD_LoadEdges: funny lump size in %s",loadmodel->name);
                int count = l->filelen / sizeof(*in);
                medge_t *out = (medge_t *) Hunk_AllocName ((count + 13) * sizeof(*out), loadname);
                loadmodel->edges = out;
                loadmodel->numedges = count;
                for (int i = 0; i < count; i++, in++, out++) {
                        out->v[0] = LittleLong(in->v[0]);
                        out->v[1] = LittleLong(in->v[1]);
                }
        }
        else {
                dedge_t *in = (dedge_t *)(mod_base + l->fileofs);
                if (l->filelen % sizeof(*in))
                        Sys_Error ("MOD_LoadEdges: funny lump size in %s",loadmodel->name);
                int count = l->filelen / sizeof(*in);
                medge_t *out = (medge_t *) Hunk_AllocName
			((count + 13) * sizeof(*out), loadname);
                loadmodel->edges = out;
                loadmodel->numedges = count;
                for (int i = 0; i<count; i++, in++, out++) {
                        out->v[0] = (unsigned short)LittleShort(in->v[0]);
                        out->v[1] = (unsigned short)LittleShort(in->v[1]);
                }
        }
}

static void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t *in = (dvertex_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadVertexes: funny lump size in %s",loadmodel->name);
	int count = l->filelen / sizeof(*in);
	// Manoel Kasimier - skyboxes - extra for skybox 
	// Code taken from the ToChriS engine - Author: Vic
	mvertex_t *out = (mvertex_t*)Hunk_AllocName((count+8)*sizeof(*out),loadname);
	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;
	for (int i = 0; i < count; i++, in++, out++) {
		out->position[0] = LittleFloat (in->point[0]);
		out->position[1] = LittleFloat (in->point[1]);
		out->position[2] = LittleFloat (in->point[2]);
	}
}

static void Mod_LoadEntities (lump_t *l)
{
	char	basemapname[MAX_QPATH];
	char	entfilename[MAX_QPATH];
	char		*ents;
	int		mark;
	unsigned int	path_id;
	unsigned int	crc = 0;

	if (! external_ents.value)
		goto _load_embedded;

	mark = Hunk_LowMark();
	if (l->filelen > 0) {
		crc = CRC_Block(mod_base + l->fileofs, l->filelen - 1);
	}

	q_strlcpy(basemapname, loadmodel->name, sizeof(basemapname));
	COM_StripExtension(basemapname, basemapname, sizeof(basemapname));

	snprintf(entfilename, sizeof(entfilename), "%s@%04x.ent", basemapname, crc);
	//printf("trying to load %s\n", entfilename);
	ents = (char *) COM_LoadHunkFile (entfilename, &path_id);

	if (!ents)
	{
		snprintf(entfilename, sizeof(entfilename), "%s.ent", basemapname);
		//printf("trying to load %s\n", entfilename);
		ents = (char *) COM_LoadHunkFile (entfilename, &path_id);
	}

	if (ents)
	{
		// use ent file only from the same gamedir as the map
		// itself or from a searchpath with higher priority.
		if (path_id < loadmodel->path_id)
		{
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("ignored %s from a gamedir with lower priority\n", entfilename);
		}
		else
		{
			loadmodel->entities = ents;
			Con_DPrintf("Loaded external entity file %s\n", entfilename);
			return;
		}
	}

_load_embedded:
	if (!l->filelen)
	{
		loadmodel->entities = NULL;
		return;
	}
	loadmodel->entities = (char *) Hunk_AllocName ( l->filelen, loadname);
	memcpy (loadmodel->entities, mod_base + l->fileofs, l->filelen);
}

static void Mod_LoadTexinfo(lump_t *l)
{
        texinfo_t *in = (void *)(mod_base + l->fileofs);
        if (l->filelen % sizeof(*in))
                Sys_Error("MOD_LoadTexinfo: funny lump size in %s",
                                loadmodel->name);
        int count = l->filelen / sizeof(*in);
        mtexinfo_t *out = Hunk_AllocName((count+6) * sizeof(*out), loadname);
        loadmodel->texinfo = out;
        loadmodel->numtexinfo = count;
        for (int i = 0; i < count; i++, in++, out++) {
                for (int k = 0; k < 2; k++)
                        for (int j = 0; j < 4; j++)
                                out->vecs[k][j] = LittleFloat(in->vecs[k][j]);
                float len1 = Length(out->vecs[0]);
                float len2 = Length(out->vecs[1]);
                len1 = (len1 + len2) / 2;
                if (len1 < 0.32)
                        out->mipadjust = 4;
                else if (len1 < 0.49)
                        out->mipadjust = 3;
                else if (len1 < 0.99)
                        out->mipadjust = 2;
                else
                        out->mipadjust = 1;
                int miptex = LittleLong(in->miptex);
                out->flags = LittleLong(in->flags);
                if (!loadmodel->textures) {
                        out->texture = r_notexture_mip; // checkerboard texture
                        out->flags = 0;
                } else {
                        if (miptex >= loadmodel->numtextures)
                                Sys_Error("miptex >= loadmodel->numtextures");
                        out->texture = loadmodel->textures[miptex];
                        if (!out->texture) {
                                out->texture = r_notexture_mip; // texture not found
                                out->flags = 0;
                        }
                }
        }
}

static void CalcSurfaceExtents (msurface_t *s)
{ // Fills in s->texturemins[] and s->extents[]
	float	mins[2], maxs[2], val;
	int		i,j, e;
	mvertex_t	*v;
	mtexinfo_t	*tex;
	int		bmins[2], bmaxs[2];

	mins[0] = mins[1] = FLT_MAX;
	maxs[0] = maxs[1] = -FLT_MAX;

	tex = s->texinfo;

	for (i=0 ; i<s->numedges ; i++)
	{
		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

		for (j=0 ; j<2 ; j++)
		{
			// The following calculation is sensitive to floating-point
			// precision.  It needs to produce the same result that the
			// light compiler does, because R_BuildLightMap uses surf->
			// extents to know the width/height of a surface's lightmap,
			// and incorrect rounding here manifests itself as patches
			// of "corrupted" looking lightmaps.
			// Most light compilers are win32 executables, so they use
			// x87 floating point.  This means the multiplies and adds
			// are done at 80-bit precision, and the result is rounded
			// down to 32-bits and stored in val.
			// Adding the casts to double seems to be good enough to fix
			// lighting glitches when Quakespasm is compiled as x86_64
			// and using SSE2 floating-point.  A potential trouble spot
			// is the hallway at the beginning of mfxsp17.  -- ericw
			val =	((double)v->position[0] * (double)tex->vecs[j][0]) +
				((double)v->position[1] * (double)tex->vecs[j][1]) +
				((double)v->position[2] * (double)tex->vecs[j][2]) +
				(double)tex->vecs[j][3];

			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}

	for (i=0 ; i<2 ; i++)
	{
		bmins[i] = floor(mins[i]/16);
		bmaxs[i] = ceil(maxs[i]/16);

		s->texturemins[i] = bmins[i] * 16;
		s->extents[i] = (bmaxs[i] - bmins[i]) * 16;

		if ( !(tex->flags & TEX_SPECIAL) && s->extents[i] > 2000) //johnfitz -- was 512 in glquake, 256 in winquake
			Sys_Error ("Bad surface extents");
	}
}


/*
================
Mod_PolyForUnlitSurface -- johnfitz -- creates polys for unlightmapped surfaces (sky and water)

TODO: merge this into BuildSurfaceDisplayList?
================
*/
static void Mod_PolyForUnlitSurface (msurface_t *fa)
{/*TODO
	const int	numverts = fa->numedges;
	int		i, lindex;
	float		*vec;
	glpoly_t	*poly;
	float		texscale;

	if (fa->flags & (SURF_DRAWTURB | SURF_DRAWSKY))
		texscale = (1.0f/128.0f); //warp animation repeats every 128
	else
		texscale = (1.0f/32.0f); //to match r_notexture_mip

	poly = (glpoly_t *) Hunk_Alloc (sizeof(glpoly_t) + (numverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = NULL;
	fa->polys = poly;
	poly->numverts = numverts;
	for (i=0; i<numverts; i++)
	{
		lindex = loadmodel->surfedges[fa->firstedge + i];
		vec = (lindex > 0) ?
			loadmodel->vertexes[loadmodel->edges[lindex].v[0]].position :
			loadmodel->vertexes[loadmodel->edges[-lindex].v[1]].position;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = DotProduct(vec, fa->texinfo->vecs[0]) * texscale;
		poly->verts[i][4] = DotProduct(vec, fa->texinfo->vecs[1]) * texscale;
	}*/
}

static void Mod_CalcSurfaceBounds (msurface_t *s) // -- johnfitz
{ // calculate bounding box for per-surface frustum culling
	s->mins[0] = s->mins[1] = s->mins[2] = FLT_MAX;
	s->maxs[0] = s->maxs[1] = s->maxs[2] = -FLT_MAX;
	for (int i = 0; i < s->numedges; i++) {
		int e = loadmodel->surfedges[s->firstedge+i];
		mvertex_t *v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		if (s->mins[0] > v->position[0]) s->mins[0] = v->position[0];
		if (s->mins[1] > v->position[1]) s->mins[1] = v->position[1];
		if (s->mins[2] > v->position[2]) s->mins[2] = v->position[2];
		if (s->maxs[0] < v->position[0]) s->maxs[0] = v->position[0];
		if (s->maxs[1] < v->position[1]) s->maxs[1] = v->position[1];
		if (s->maxs[2] < v->position[2]) s->maxs[2] = v->position[2];
	}
}

static void Mod_LoadFaces (lump_t *l, qboolean bsp2)
{
	dface_t	*ins;
	dlface_t	*inl;
	msurface_t 	*out;
	int			i, count, surfnum, lofs;
	int			planenum, side, texinfon;

	if (bsp2)
	{
		ins = NULL;
		inl = (dlface_t *)(mod_base + l->fileofs);
		if (l->filelen % sizeof(*inl))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
		count = l->filelen / sizeof(*inl);
	}
	else
	{
		ins = (dface_t *)(mod_base + l->fileofs);
		inl = NULL;
		if (l->filelen % sizeof(*ins))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
		count = l->filelen / sizeof(*ins);
	}
	out = (msurface_t *)Hunk_AllocName ( (count+6)*sizeof(*out), loadname);

	//johnfitz -- warn mappers about exceeding old limits
	if (count > 32767 && !bsp2)
		printf ("%i faces exceeds standard limit of 32767.\n", count);
	//johnfitz

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	int foundcutouts = 0;
	for (surfnum=0 ; surfnum<count ; surfnum++, out++)
	{
		if (bsp2)
		{
			out->firstedge = LittleLong(inl->firstedge);
			out->numedges = LittleLong(inl->numedges);
			planenum = LittleLong(inl->planenum);
			side = LittleLong(inl->side);
			texinfon = LittleLong (inl->texinfo);
			for (i=0 ; i<MAXLIGHTMAPS ; i++)
				out->styles[i] = inl->styles[i];
			lofs = LittleLong(inl->lightofs);
			inl++;
		}
		else
		{
			out->firstedge = LittleLong(ins->firstedge);
			out->numedges = LittleShort(ins->numedges);
			planenum = LittleShort(ins->planenum);
			side = LittleShort(ins->side);
			texinfon = LittleShort (ins->texinfo);
			for (i=0 ; i<MAXLIGHTMAPS ; i++)
				out->styles[i] = ins->styles[i];
			lofs = LittleLong(ins->lightofs);
			ins++;
		}

		out->flags = 0;
		if (out->numedges < 3)
			printf("surfnum %d: bad numedges %d\n", surfnum, out->numedges);

		if (side)
			out->flags |= SURF_PLANEBACK;

		out->plane = loadmodel->planes + planenum;

		out->texinfo = loadmodel->texinfo + texinfon;

		CalcSurfaceExtents (out);

		Mod_CalcSurfaceBounds (out); //johnfitz -- for per-surface frustum culling

	// lighting info
		if (loadmodel->bspversion == BSPVERSION_QUAKE64)
			lofs /= 2; // Q64 samples are 16bits instead 8 in normal Quake 

		if (lofs == -1)
			out->samples = NULL;
#ifdef BSP29_VALVE
		else if (loadmodel->bspversion == BSPVERSION_VALVE)
			out->samples = loadmodel->lightdata + lofs; // accounts for RGB light data
#endif
		else
			out->samples = loadmodel->lightdata + (lofs * 3); //johnfitz -- lit support via lordhavoc (was "+ i")
                //out->samples = loadmodel->lightdata + lofs; // TODO colored lighting

		//johnfitz -- this section rewritten
		if (!q_strncasecmp(out->texinfo->texture->name,"sky",3)) // sky surface //also note -- was Q_strncmp, changed to match qbsp
		{
			out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
			Mod_PolyForUnlitSurface (out); //no more subdivision
		}
		else if (out->texinfo->texture->name[0] == '*' || out->texinfo->texture->name[0] == '!') // warp surface
		{
			out->flags |= SURF_DRAWTURB;
			if (out->texinfo->flags & TEX_SPECIAL)
				out->flags |= SURF_DRAWTILED;
			else if (out->samples && !loadmodel->haslitwater)
			{
				Con_DPrintf ("Map has lit water\n");
				loadmodel->haslitwater = true;
			}

		// detect special liquid types
			if (!strncmp (out->texinfo->texture->name, "*lava", 5))
				out->flags |= SURF_DRAWLAVA;
			else if (!strncmp (out->texinfo->texture->name, "*slime", 6))
				out->flags |= SURF_DRAWSLIME;
			else if (!strncmp (out->texinfo->texture->name, "*tele", 5))
				out->flags |= SURF_DRAWTELE;
			else out->flags |= SURF_DRAWWATER;

			// polys are only created for unlit water here.
			// lit water is handled in BuildSurfaceDisplayList
			if (out->flags & SURF_DRAWTILED)
			{
				for (i = 0; i < 2; i++) {
					out->extents[i] = 16384;
					out->texturemins[i] = -8192;
				}
				Mod_PolyForUnlitSurface (out);
				//GL_SubdivideSurface (out);
			}
		}
		else if (out->texinfo->texture->name[0] == '{') // ericw -- fence textures
		{
			out->flags |= SURF_DRAWCUTOUT;
			foundcutouts = 1;
		}
		else if (out->texinfo->flags & TEX_MISSING) // texture is missing from bsp
		{
			if (out->samples) //lightmapped
			{
				out->flags |= SURF_NOTEXTURE;
			}
			else // not lightmapped
			{
				out->flags |= (SURF_NOTEXTURE | SURF_DRAWTILED);
				Mod_PolyForUnlitSurface (out);
			}
		}
		//johnfitz
	}
	if (r_twopass.value < 2 && !strcmp (loadmodel->name, sv.modelname)) {
		if (foundcutouts)
			Cvar_SetValue("r_twopass", 1);
		else
			Cvar_SetValue("r_twopass", 0);
	}

}

static void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents < 0)
		return;
	Mod_SetParent (node->children[0], node);
	Mod_SetParent (node->children[1], node);
}

static void Mod_LoadNodes_S (lump_t *l)
{
	dnode_t *in = (dnode_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadNodes_S: funny lump size in %s",loadmodel->name);
	int count = l->filelen / sizeof(*in);
	mnode_t *out = (mnode_t *) Hunk_AllocName ( count*sizeof(*out), loadname);
	if (count > 32767) //johnfitz -- warn mappers about exceeding old limits
		printf ("%i nodes exceeds standard limit of 32767.\n", count);
	loadmodel->nodes = out;
	loadmodel->numnodes = count;
	for (int i = 0; i < count; i++, in++, out++) {
		for (int j = 0; j < 3; j++) {
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}
		int p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;
		out->firstsurface = (unsigned short)LittleShort (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = (unsigned short)LittleShort (in->numfaces); //johnfitz -- explicit cast as unsigned short
		for (int j = 0; j < 2; j++) {
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = (unsigned short)LittleShort(in->children[j]);
			if (p < count)
				out->children[j] = loadmodel->nodes + p;
			else {
				p = 65535 - p; //note this uses 65535 intentionally, -1 is leaf 0
				if (p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else {
					Con_Printf("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			} //johnfitz
		}
	}
}

static void Mod_LoadNodes_L1 (lump_t *l)
{
	dl1node_t *in = (dl1node_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("Mod_LoadNodes: funny lump size in %s",loadmodel->name);
	int count = l->filelen / sizeof(*in);
	mnode_t *out = (mnode_t *)Hunk_AllocName ( count*sizeof(*out), loadname);
	loadmodel->nodes = out;
	loadmodel->numnodes = count;
	for (int i = 0; i < count; i++, in++, out++) {
		for (int j = 0; j < 3; j++) {
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}
		int p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;
		out->firstsurface = LittleLong (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = LittleLong (in->numfaces); //johnfitz -- explicit cast as unsigned short
		for (int j = 0; j < 2; j++) {
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = LittleLong(in->children[j]);
			if (p >= 0 && p < count)
				out->children[j] = loadmodel->nodes + p;
			else {
				p = 0xffffffff - p; //note this uses 65535 intentionally, -1 is leaf 0
				if (p >= 0 && p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else {
					Con_Printf("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			} //johnfitz
		}
	}
}

static void Mod_LoadNodes_L2 (lump_t *l)
{
	dl2node_t *in = (dl2node_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("Mod_LoadNodes: funny lump size in %s",loadmodel->name);
	int count = l->filelen / sizeof(*in);
	mnode_t *out = (mnode_t *)Hunk_AllocName ( count*sizeof(*out), loadname);
	loadmodel->nodes = out;
	loadmodel->numnodes = count;
	for (int i = 0; i < count; i++, in++, out++) {
		for (int j = 0; j < 3; j++) {
			out->minmaxs[j] = LittleFloat (in->mins[j]);
			out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
		}
		int p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;
		out->firstsurface = LittleLong (in->firstface); //johnfitz -- explicit cast as unsigned short
		out->numsurfaces = LittleLong (in->numfaces); //johnfitz -- explicit cast as unsigned short
		for (int j = 0; j < 2; j++) {
			//johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = LittleLong(in->children[j]);
			if (p > 0 && p < count)
				out->children[j] = loadmodel->nodes + p;
			else {
				p = 0xffffffff - p; //note this uses 65535 intentionally, -1 is leaf 0
				if (p >= 0 && p < loadmodel->numleafs)
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				else {
					Con_Printf("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, loadmodel->numleafs);
					out->children[j] = (mnode_t *)(loadmodel->leafs); //map it to the solid leaf
				}
			} //johnfitz
		}
	}
}

static void Mod_LoadNodes (lump_t *l, int bsp2)
{
	if (bsp2 == 2)
		Mod_LoadNodes_L2(l);
	else if (bsp2)
		Mod_LoadNodes_L1(l);
	else
		Mod_LoadNodes_S(l);
	Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

static void Mod_ProcessLeafs_S (dleaf_t *in, int filelen)
{
	if (filelen % sizeof(*in))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", loadmodel->name);
	int count = filelen / sizeof(*in);
	mleaf_t *out = (mleaf_t *) Hunk_AllocName ( count*sizeof(*out), loadname);
	if (count > 32767) // johnfitz
		Host_Error ("Mod_LoadLeafs: %i leafs exceeds limit of 32767.", count);
	loadmodel->leafs = out;
	loadmodel->numleafs = count;
	for (int i = 0; i < count; i++, in++, out++) {
		for (int j = 0; j < 3; j++) {
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}
		int p = LittleLong(in->contents);
		out->contents = p;
		out->firstmarksurface = loadmodel->marksurfaces + (unsigned short)LittleShort(in->firstmarksurface); //johnfitz -- unsigned short
		out->nummarksurfaces = (unsigned short)LittleShort(in->nummarksurfaces); //johnfitz -- unsigned short
		p = LittleLong(in->visofs);
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;
		out->efrags = NULL;
		for (int j = 0; j < 4; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];
		//johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

static void Mod_ProcessLeafs_L1 (dl1leaf_t *in, int filelen)
{
	if (filelen % sizeof(*in))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", loadmodel->name);
	int count = filelen / sizeof(*in);
	mleaf_t *out = (mleaf_t *) Hunk_AllocName (count * sizeof(*out), loadname);
	loadmodel->leafs = out;
	loadmodel->numleafs = count;
	for (int i = 0; i < count; i++, in++, out++) {
		for (int j = 0; j < 3; j++) {
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}
		int p = LittleLong(in->contents);
		out->contents = p;
		out->firstmarksurface = loadmodel->marksurfaces + LittleLong(in->firstmarksurface); //johnfitz -- unsigned short
		out->nummarksurfaces = LittleLong(in->nummarksurfaces); //johnfitz -- unsigned short
		p = LittleLong(in->visofs);
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;
		out->efrags = NULL;
		for (int j = 0; j < 4; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];
		//johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

static void Mod_ProcessLeafs_L2 (dl2leaf_t *in, int filelen)
{
	if (filelen % sizeof(*in))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", loadmodel->name);
	int count = filelen / sizeof(*in);
	mleaf_t *out = (mleaf_t *) Hunk_AllocName (count * sizeof(*out), loadname);
	loadmodel->leafs = out;
	loadmodel->numleafs = count;
	for (int i = 0; i < count; i++, in++, out++) {
		for (int j = 0; j < 3; j++) {
			out->minmaxs[j] = LittleFloat (in->mins[j]);
			out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
		}
		int p = LittleLong(in->contents);
		out->contents = p;
		out->firstmarksurface = loadmodel->marksurfaces + LittleLong(in->firstmarksurface); //johnfitz -- unsigned short
		out->nummarksurfaces = LittleLong(in->nummarksurfaces); //johnfitz -- unsigned short
		p = LittleLong(in->visofs);
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;
		out->efrags = NULL;
		for (int j = 0; j < 4; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];
		//johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

static void Mod_LoadLeafs (lump_t *l, int bsp2)
{
	void *in = (void *)(mod_base + l->fileofs);
	if (bsp2 == 2)
		Mod_ProcessLeafs_L2 ((dl2leaf_t *)in, l->filelen);
	else if (bsp2)
		Mod_ProcessLeafs_L1 ((dl1leaf_t *)in, l->filelen);
	else
		Mod_ProcessLeafs_S  ((dleaf_t *) in, l->filelen);
}

static void Mod_LoadClipnodes (lump_t *l, qboolean bsp2)
{
	dsclipnode_t *ins;
	dlclipnode_t *inl;
	int count;
	if (bsp2) {
		ins = NULL;
		inl = (dlclipnode_t *)(mod_base + l->fileofs);
		if (l->filelen % sizeof(*inl))
			Sys_Error ("Mod_LoadClipnodes: funny lump size in %s",loadmodel->name);
		count = l->filelen / sizeof(*inl);
	}
	else {
		ins = (dsclipnode_t *)(mod_base + l->fileofs);
		inl = NULL;
		if (l->filelen % sizeof(*ins))
			Sys_Error ("Mod_LoadClipnodes: funny lump size in %s",loadmodel->name);
		count = l->filelen / sizeof(*ins);
	}
	mclipnode_t *out; //johnfitz -- was dclipnode_t
	out = (mclipnode_t *) Hunk_AllocName ( count*sizeof(*out), loadname);
	if (count > 32767 && !bsp2) //johnfitz -- warn about exceeding old limits
		printf ("%i clipnodes exceeds standard limit of 32767.\n", count);
	loadmodel->clipnodes = out;
	loadmodel->numclipnodes = count;
	hull_t *hull = &loadmodel->hulls[1];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -16;
	hull->clip_mins[1] = -16;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 16;
	hull->clip_maxs[1] = 16;
	hull->clip_maxs[2] = 32;
	hull = &loadmodel->hulls[2];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -32;
	hull->clip_mins[1] = -32;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 32;
	hull->clip_maxs[1] = 32;
	hull->clip_maxs[2] = 64;
	if (bsp2) {
		for (int i = 0; i < count; i++, out++, inl++) {
			out->planenum = LittleLong(inl->planenum);
			//johnfitz -- bounds check
			if (out->planenum < 0 || out->planenum >= loadmodel->numplanes)
				Host_Error ("Mod_LoadClipnodes: planenum out of bounds");
			out->children[0] = LittleLong(inl->children[0]);
			out->children[1] = LittleLong(inl->children[1]);
			//Spike: FIXME: bounds check
		}
	}
	else {
		for (int i = 0; i < count; i++, out++, ins++) {
			out->planenum = LittleLong(ins->planenum);
			//johnfitz -- bounds check
			if (out->planenum < 0 || out->planenum >= loadmodel->numplanes)
				Host_Error ("Mod_LoadClipnodes: planenum out of bounds");
			//johnfitz -- support clipnodes > 32k
			out->children[0] = (unsigned short)LittleShort(ins->children[0]);
			out->children[1] = (unsigned short)LittleShort(ins->children[1]);
			if (out->children[0] >= count)
				out->children[0] -= 65536;
			if (out->children[1] >= count)
				out->children[1] -= 65536; //johnfitz
		}
	}
}


static void Mod_MakeHull0 ()
{ // Duplicate the drawing hull structure as a clipping hull
	hull_t *hull = &loadmodel->hulls[0];
	mnode_t *in = loadmodel->nodes;
	int count = loadmodel->numnodes;
	mclipnode_t *out = (mclipnode_t *) Hunk_AllocName
		( count*sizeof(*out), loadname); //johnfitz -- was dclipnode_t
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	for (int i = 0; i < count; i++, out++, in++) {
		out->planenum = in->plane - loadmodel->planes;
		for (int j = 0; j < 2; j++) {
			mnode_t *child = in->children[j];
			if (child->contents < 0)
				out->children[j] = child->contents;
			else
				out->children[j] = child - loadmodel->nodes;
		}
	}
}

static void Mod_LoadMarksurfaces (lump_t *l, int bsp2)
{
	if (bsp2) {
		unsigned int *in = (unsigned int *)(mod_base + l->fileofs);
		if (l->filelen % sizeof(*in))
			Host_Error ("Mod_LoadMarksurfaces: funny lump size in %s",loadmodel->name);
		int count = l->filelen / sizeof(*in);
		msurface_t **out = (msurface_t **)Hunk_AllocName ( count*sizeof(*out), loadname);
		loadmodel->marksurfaces = out;
		loadmodel->nummarksurfaces = count;
		for (int i = 0; i < count; i++) {
			unsigned long j = LittleLong(in[i]);
			if (j >= loadmodel->numsurfaces)
				Host_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = loadmodel->surfaces + j;
		}
	}
	else {
		short *in = (short *)(mod_base + l->fileofs);
		if (l->filelen % sizeof(*in))
			Host_Error ("Mod_LoadMarksurfaces: funny lump size in %s",loadmodel->name);
		int count = l->filelen / sizeof(*in);
		msurface_t **out = (msurface_t **)Hunk_AllocName ( count*sizeof(*out), loadname);
		loadmodel->marksurfaces = out;
		loadmodel->nummarksurfaces = count;
		if (count > 32767) //johnfitz -- warn mappers about exceeding old limits
			printf ("%i marksurfaces exceeds standard limit of 32767.\n", count);
		for (int i = 0; i < count; i++) {
			unsigned short j = (unsigned short)LittleShort(in[i]); //johnfitz -- explicit cast as unsigned short
			if (j >= loadmodel->numsurfaces)
				Sys_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = loadmodel->surfaces + j;
		}
	}
}

static void Mod_LoadSurfedges (lump_t *l)
{
	int *in = (int *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadSurfedges: funny lump size in %s",loadmodel->name);
	int count = l->filelen / sizeof(*in);
	int *out = (int *) Hunk_AllocName ( (count+24)*sizeof(*out), loadname);
	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;
	for (int i = 0; i < count; i++)
		out[i] = LittleLong (in[i]);
}


static void Mod_LoadPlanes (lump_t *l)
{
	dplane_t *in = (dplane_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadPlanes: funny lump size in %s",loadmodel->name);
	int count = l->filelen / sizeof(*in);
	mplane_t *out = (mplane_t *) Hunk_AllocName ( (count+6)*2*sizeof(*out), loadname);
	loadmodel->planes = out;
	loadmodel->numplanes = count;
	for (int i = 0; i < count; i++, in++, out++) {
		int bits = 0;
		for (int j = 0; j < 3; j++) {
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}
		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);
		out->signbits = bits;
	}
}

static float RadiusFromBounds (vec3_t min, vec3_t max)
{
	vec3_t corner;
	for (int i = 0; i < 3; i++)
		corner[i]=fabs(min[i])>fabs(max[i])?fabs(min[i]):fabs(max[i]);
	return VectorLength (corner);
}

static void Mod_LoadSubmodels (lump_t *l)
{
	dmodel_t *in = (dmodel_t *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadSubmodels: funny lump size in %s",loadmodel->name);
	int count = l->filelen / sizeof(*in);
	dmodel_t *out = (dmodel_t *) Hunk_AllocName ( count*sizeof(*out), loadname);
	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;
	for (int i = 0; i < count; i++, in++, out++) {
		for (int j = 0; j < 3; j++) { // spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
			out->origin[j] = LittleFloat (in->origin[j]);
		}
		for (int j = 0; j < MAX_MAP_HULLS; j++)
			out->headnode[j] = LittleLong (in->headnode[j]);
		out->visleafs = LittleLong (in->visleafs);
		out->firstface = LittleLong (in->firstface);
		out->numfaces = LittleLong (in->numfaces);
	}
	out = loadmodel->submodels;
	if (out->visleafs > 8192) // johnfitz -- check world visleafs -- adapted from bjp
		printf ("%i visleafs exceeds standard limit of 8192.\n", out->visleafs);
}

static FILE *Mod_FindVisibilityExternal()
{
	vispatch_t header;
	char visfilename[MAX_QPATH];
	const char* shortname;
	unsigned int path_id;
	FILE *f;
	long pos;
	size_t r;

	snprintf(visfilename, sizeof(visfilename), "maps/%s.vis", loadname);
	if (COM_FOpenFile(visfilename, &f, &path_id) < 0)
	{
		Con_DPrintf("%s not found, trying ", visfilename);
		snprintf(visfilename, sizeof(visfilename), "%s.vis", COM_SkipPath(com_gamedir));
		Con_DPrintf("%s\n", visfilename);
		if (COM_FOpenFile(visfilename, &f, &path_id) < 0)
		{
			Con_DPrintf("external vis not found\n");
			return NULL;
		}
	}
	if (path_id < loadmodel->path_id)
	{
		fclose(f);
		Con_DPrintf("ignored %s from a gamedir with lower priority\n", visfilename);
		return NULL;
	}

	Con_DPrintf("Found external VIS %s\n", visfilename);

	shortname = COM_SkipPath(loadmodel->name);
	pos = 0;
	while ((r = fread(&header, 1, VISPATCH_HEADER_LEN, f)) == VISPATCH_HEADER_LEN)
	{
		header.filelen = LittleLong(header.filelen);
		if (header.filelen <= 0) {	// bad entry -- don't trust the rest.
			fclose(f);
			return NULL;
		}
		if (!q_strcasecmp(header.mapname, shortname))
			break;
		pos += header.filelen + VISPATCH_HEADER_LEN;
		fseek(f, pos, SEEK_SET);
	}
	if (r != VISPATCH_HEADER_LEN) {
		fclose(f);
		Con_DPrintf("%s not found in %s\n", shortname, visfilename);
		return NULL;
	}

	return f;
}

static byte *Mod_LoadVisibilityExternal(FILE *f)
{
	int filelen = 0;
	if (!fread(&filelen, 4, 1, f)) return NULL;
	filelen = LittleLong(filelen);
	if (filelen <= 0) return NULL;
	Con_DPrintf("...%d bytes visibility data\n", filelen);
	byte *visdata = (byte *) Hunk_AllocName(filelen, "EXT_VIS");
	if (!fread(visdata, filelen, 1, f))
		return NULL;
	return visdata;
}

static void Mod_LoadLeafsExternal(FILE *f)
{
	int filelen = 0;
	if (!fread(&filelen, 4, 1, f)) return;
	filelen = LittleLong(filelen);
	if (filelen <= 0) return;
	Con_DPrintf("...%d bytes leaf data\n", filelen);
	void *in = Hunk_AllocName(filelen, "EXT_LEAF");
	if (!fread(in, filelen, 1, f))
		return;
	Mod_ProcessLeafs_S((dleaf_t *)in, filelen);
}

static void Mod_LoadBrushModel (model_t *mod, void *buffer)
{
	loadmodel->type = mod_brush;
	dheader_t *header = (dheader_t *)buffer;
	mod->bspversion = LittleLong (header->version);
	int bsp2;
	switch(mod->bspversion) {
	case BSPVERSION:
		bsp2 = false;
		break;
#ifdef BSP29_VALVE
	case BSPVERSION_VALVE:
		bsp2 = false;
		break;
#endif
	case BSP2VERSION_2PSB:
		bsp2 = 1;	//first iteration
		break;
	case BSP2VERSION_BSP2:
		bsp2 = 2;	//sanitised revision
		break;
	case BSPVERSION_QUAKE64:
		bsp2 = false;
		break;
	default:
		Sys_Error ("Mod_LoadBrushModel: %s has unsupported version number (%i)", mod->name, mod->bspversion);
		break;
	}
	// swap all the lumps
	mod_base = (byte *)header;
	for (int i = 0; i < (int)sizeof(dheader_t) / 4; i++)
		((int *)header)[i] = LittleLong ( ((int *)header)[i]);
	// load into heap
	Mod_LoadVertexes (&header->lumps[LUMP_VERTEXES]);
	Mod_LoadEdges (&header->lumps[LUMP_EDGES], bsp2);
	Mod_LoadSurfedges (&header->lumps[LUMP_SURFEDGES]);
	Mod_LoadEntities (&header->lumps[LUMP_ENTITIES]);
	Mod_LoadTextures (&header->lumps[LUMP_TEXTURES]);
	Mod_LoadLighting (&header->lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes (&header->lumps[LUMP_PLANES]);
	Mod_LoadTexinfo (&header->lumps[LUMP_TEXINFO]);
	Mod_LoadFaces (&header->lumps[LUMP_FACES], bsp2);
	Mod_LoadMarksurfaces (&header->lumps[LUMP_MARKSURFACES], bsp2);
	if (mod->bspversion == BSPVERSION && external_vis.value &&
			sv.modelname[0] && !q_strcasecmp(loadname, sv.name)) {
		FILE *fvis;
		Con_DPrintf("trying to open external vis file\n");
		fvis = Mod_FindVisibilityExternal();
		if (fvis) {
			int mark = Hunk_LowMark();
			loadmodel->leafs = NULL;
			loadmodel->numleafs = 0;
			Con_DPrintf("found valid external .vis file for map\n");
			loadmodel->visdata = Mod_LoadVisibilityExternal(fvis);
			if (loadmodel->visdata) {
				Mod_LoadLeafsExternal(fvis);
			}
			fclose(fvis);
			if (loadmodel->visdata && loadmodel->leafs && loadmodel->numleafs) {
				goto visdone;
			}
			Hunk_FreeToLowMark(mark);
			Con_DPrintf("External VIS data failed, using standard vis.\n");
		}
	}
	Mod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs (&header->lumps[LUMP_LEAFS], bsp2);
visdone:
	Mod_LoadNodes (&header->lumps[LUMP_NODES], bsp2);
	Mod_LoadClipnodes (&header->lumps[LUMP_CLIPNODES], bsp2);
	Mod_LoadSubmodels (&header->lumps[LUMP_MODELS]);
	Mod_MakeHull0 ();
	mod->numframes = 2;		// regular and alternate animation
	// set up the submodels (FIXME: this is confusing)
	// johnfitz -- okay, so that i stop getting confused every time i look at this loop, here's how it works:
	// we're looping through the submodels starting at 0.  Submodel 0 is the main model, so we don't have to
	// worry about clobbering data the first time through, since it's the same data.  At the end of the loop,
	// we create a new copy of the data to use the next time through.
	for (int i = 0; i < mod->numsubmodels; i++) {
		dmodel_t *bm = &mod->submodels[i];
		mod->hulls[0].firstclipnode = bm->headnode[0];
		for (int j = 1; j < MAX_MAP_HULLS; j++) {
			mod->hulls[j].firstclipnode = bm->headnode[j];
			mod->hulls[j].lastclipnode = mod->numclipnodes-1;
		}
		mod->firstmodelsurface = bm->firstface;
		mod->nummodelsurfaces = bm->numfaces;
		VectorCopy (bm->maxs, mod->maxs);
		VectorCopy (bm->mins, mod->mins);
		//johnfitz -- calculate rotate bounds and yaw bounds
		float radius = RadiusFromBounds (mod->mins, mod->maxs);
		mod->rmaxs[0] = mod->rmaxs[1] = mod->rmaxs[2] = mod->ymaxs[0] = mod->ymaxs[1] = mod->ymaxs[2] = radius;
		mod->rmins[0] = mod->rmins[1] = mod->rmins[2] = mod->ymins[0] = mod->ymins[1] = mod->ymins[2] = -radius;
		//johnfitz -- correct physics cullboxes so that outlying clip brushes on doors and stuff are handled right
		if (i > 0 || strcmp(mod->name, sv.modelname) != 0) //skip submodel 0 of sv.worldmodel, which is the actual world
		{ // start with the hull0 bounds
			VectorCopy (mod->maxs, mod->clipmaxs);
			VectorCopy (mod->mins, mod->clipmins);
			// process hull1 (we don't need to process hull2 becuase there's
			// no such thing as a brush that appears in hull2 but not hull1)
			//Mod_BoundsFromClipNode (mod, 1, mod->hulls[1].firstclipnode); // (disabled for now becuase it fucks up on rotating models)
		} //johnfitz
		mod->numleafs = bm->visleafs;
		if (i < mod->numsubmodels-1) { // duplicate the basic information
			char name[12];
			sprintf (name, "*%i", i+1);
			loadmodel = Mod_FindName (name);
			*loadmodel = *mod;
			strcpy (loadmodel->name, name);
			mod = loadmodel;
		}
	}
}

void *Mod_LoadAliasFrame(void *pin, int *pframeindex, int numv,
                trivertx_t *pbboxmin, trivertx_t *pbboxmax, aliashdr_t *pheader,
		char *name, maliasframedesc_t *frame, int recursed)
{
        daliasframe_t *pdaliasframe = (daliasframe_t *) pin;
        q_strlcpy(name, pdaliasframe->name, 16);
	if (!recursed) {
                frame->firstpose = posenum;
                frame->numposes = 1;
        }
        for (int i = 0; i < 3; i++) {
                // these are byte values, so we don't have to worry about
                // endianness
		frame->bboxmin.v[i] = pdaliasframe->bboxmin.v[i];
                frame->bboxmax.v[i] = pdaliasframe->bboxmax.v[i];
        }
        trivertx_t *pinframe = (trivertx_t *) (pdaliasframe + 1);
        trivertx_t *pframe = Hunk_AllocName(numv * sizeof(*pframe), loadname);
        *pframeindex = (byte *) pframe - (byte *) pheader;
	poseverts[posenum] = pinframe;
	posenum++;
        for (int j = 0; j < numv; j++) {
                // these are all byte values, so no need to deal with endianness
                pframe[j].lightnormalindex = pinframe[j].lightnormalindex;
                for (int k = 0; k < 3; k++) {
                        pframe[j].v[k] = pinframe[j].v[k];
                }
        }
        pinframe += numv;
        return (void *)pinframe;
}

void *Mod_LoadAliasGroup(void *pin, int *pframeindex, int numv,
                trivertx_t *pbboxmin, trivertx_t *pbboxmax,
                aliashdr_t *pheader, char *name, maliasframedesc_t *frame)
{
        daliasgroup_t *pingroup = (daliasgroup_t *) pin;
        int numframes = LittleLong(pingroup->numframes);
	frame->firstpose = posenum;
        maliasgroup_t *paliasgroup = Hunk_AllocName(sizeof(maliasgroup_t) + (numframes - 1) * sizeof(paliasgroup->frames[0]), loadname);
        paliasgroup->numframes = frame->numposes = numframes;
        for (int i = 0; i < 3; i++) {
                // these are byte values, so we don't have to worry about endianness
		frame->bboxmin.v[i] = pingroup->bboxmin.v[i];
                frame->bboxmax.v[i] = pingroup->bboxmax.v[i];
        }
        *pframeindex = (byte *) paliasgroup - (byte *) pheader;
        daliasinterval_t *pin_intervals = (daliasinterval_t *) (pingroup + 1);
	frame->interval = LittleFloat(pin_intervals->interval);
        float *poutintervals = Hunk_AllocName(numframes*sizeof(float),loadname);
        paliasgroup->intervals = (byte *) poutintervals - (byte *) pheader;
        for (int i = 0; i < numframes; i++) {
                *poutintervals = LittleFloat(pin_intervals->interval);
                if (*poutintervals <= 0.0)
                        Sys_Error("Mod_LoadAliasGroup: interval<=0");
                poutintervals++;
                pin_intervals++;
        }
        void *ptemp = (void *)pin_intervals;
        for (int i = 0; i < numframes; i++)
                ptemp = Mod_LoadAliasFrame(ptemp, &paliasgroup->frames[i].frame,
			numv, &paliasgroup->frames[i].bboxmin,
			&paliasgroup->frames[i].bboxmax, pheader, name,frame,1);
        return ptemp;
}

/* TODO
typedef struct
{
	short		x, y;
} floodfill_t;

// must be a power of 2
#define	FLOODFILL_FIFO_SIZE		0x1000
#define	FLOODFILL_FIFO_MASK		(FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP( off, dx, dy )				\
do {								\
	if (pos[off] == fillcolor)				\
	{							\
		pos[off] = 255;					\
		fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
		inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;	\
	}							\
	else if (pos[off] != 255) fdc = pos[off];		\
} while (0)

static void Mod_FloodFillSkin( byte *skin, int skinwidth, int skinheight )
{ // Fill background pixels so mipmapping doesn't have haloes - Ed
	byte		fillcolor = *skin; // assume this is the pixel to fill
	floodfill_t	fifo[FLOODFILL_FIFO_SIZE];
	int			inpt = 0, outpt = 0;
	int			filledcolor = -1;
	int			i;

	if (filledcolor == -1)
	{
		filledcolor = 0;
		// attempt to find opaque black
		for (i = 0; i < 256; ++i)
		{
			if (d_8to24table[i] == (255 << 0)) // alpha 1.0
			{
				filledcolor = i;
				break;
			}
		}
	}

	// can't fill to filled color or to transparent color (used as visited marker)
	if ((fillcolor == filledcolor) || (fillcolor == 255))
	{
		//printf( "not filling skin from %d to %d\n", fillcolor, filledcolor );
		return;
	}

	fifo[inpt].x = 0, fifo[inpt].y = 0;
	inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

	while (outpt != inpt)
	{
		int			x = fifo[outpt].x, y = fifo[outpt].y;
		int			fdc = filledcolor;
		byte		*pos = &skin[x + skinwidth * y];

		outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

		if (x > 0)				FLOODFILL_STEP( -1, -1, 0 );
		if (x < skinwidth - 1)	FLOODFILL_STEP( 1, 1, 0 );
		if (y > 0)				FLOODFILL_STEP( -skinwidth, 0, -1 );
		if (y < skinheight - 1)	FLOODFILL_STEP( skinwidth, 0, 1 );
		skin[x + skinwidth * y] = fdc;
	}
}
TODO*/

/*
static void *Mod_LoadAllSkins (int numskins, daliasskintype_t *pskintype)
{
	int			i, j, k, size, groupskins;
	char			name[MAX_QPATH];
	byte			*skin, *texels;
	daliasskingroup_t	*pinskingroup;
	daliasskininterval_t	*pinskinintervals;
	char			fbr_mask_name[MAX_QPATH]; //johnfitz -- added for fullbright support
	src_offset_t		offset; //johnfitz
	unsigned int		texflags = TEXPREF_PAD;

	skin = (byte *)(pskintype + 1);

	if (numskins < 1 || numskins > MAX_SKINS)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of skins: %d", numskins);

	size = pheader->skinwidth * pheader->skinheight;

	if (loadmodel->flags & MF_HOLEY)
		texflags |= TEXPREF_ALPHA;

	for (i=0 ; i<numskins ; i++)
	{
		if (pskintype->type == ALIAS_SKIN_SINGLE)
		{
			Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight );

			// save 8 bit texels for the player model to remap
			texels = (byte *) Hunk_AllocName(size, loadname);
			pheader->texels[i] = texels - (byte *)pheader;
			memcpy (texels, (byte *)(pskintype + 1), size);

			//johnfitz -- rewritten
			snprintf (name, sizeof(name), "%s:frame%i", loadmodel->name, i);
			offset = (src_offset_t)(pskintype+1) - (src_offset_t)mod_base;
			if (Mod_CheckFullbrights ((byte *)(pskintype+1), size))
			{
				pheader->gltextures[i][0] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
					SRC_INDEXED, (byte *)(pskintype+1), loadmodel->name, offset, texflags | TEXPREF_NOBRIGHT);
				snprintf (fbr_mask_name, sizeof(fbr_mask_name), "%s:frame%i_glow", loadmodel->name, i);
				pheader->fbtextures[i][0] = TexMgr_LoadImage (loadmodel, fbr_mask_name, pheader->skinwidth, pheader->skinheight,
					SRC_INDEXED, (byte *)(pskintype+1), loadmodel->name, offset, texflags | TEXPREF_FULLBRIGHT);
			}
			else
			{
				pheader->gltextures[i][0] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
					SRC_INDEXED, (byte *)(pskintype+1), loadmodel->name, offset, texflags);
				pheader->fbtextures[i][0] = NULL;
			}

			pheader->gltextures[i][3] = pheader->gltextures[i][2] = pheader->gltextures[i][1] = pheader->gltextures[i][0];
			pheader->fbtextures[i][3] = pheader->fbtextures[i][2] = pheader->fbtextures[i][1] = pheader->fbtextures[i][0];
			//johnfitz

			pskintype = (daliasskintype_t *)((byte *)(pskintype+1) + size);
		}
		else
		{
			// animating skin group.  yuck.
			pskintype++;
			pinskingroup = (daliasskingroup_t *)pskintype;
			groupskins = LittleLong (pinskingroup->numskins);
			pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

			pskintype = (daliasskintype_t *)(pinskinintervals + groupskins);

			for (j=0 ; j<groupskins ; j++)
			{
				Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight );
				if (j == 0) {
					texels = (byte *) Hunk_AllocName(size, loadname);
					pheader->texels[i] = texels - (byte *)pheader;
					memcpy (texels, (byte *)(pskintype), size);
				}

				//johnfitz -- rewritten
				snprintf (name, sizeof(name), "%s:frame%i_%i", loadmodel->name, i,j);
				offset = (src_offset_t)(pskintype) - (src_offset_t)mod_base; //johnfitz
				if (Mod_CheckFullbrights ((byte *)(pskintype), size))
				{
					pheader->gltextures[i][j&3] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
						SRC_INDEXED, (byte *)(pskintype), loadmodel->name, offset, texflags | TEXPREF_NOBRIGHT);
					snprintf (fbr_mask_name, sizeof(fbr_mask_name), "%s:frame%i_%i_glow", loadmodel->name, i,j);
					pheader->fbtextures[i][j&3] = TexMgr_LoadImage (loadmodel, fbr_mask_name, pheader->skinwidth, pheader->skinheight,
						SRC_INDEXED, (byte *)(pskintype), loadmodel->name, offset, texflags | TEXPREF_FULLBRIGHT);
				}
				else
				{
					pheader->gltextures[i][j&3] = TexMgr_LoadImage (loadmodel, name, pheader->skinwidth, pheader->skinheight,
						SRC_INDEXED, (byte *)(pskintype), loadmodel->name, offset, texflags);
					pheader->fbtextures[i][j&3] = NULL;
				}
				//johnfitz

				pskintype = (daliasskintype_t *)((byte *)(pskintype) + size);
			}
			k = j;
			for (; j < 4; j++)
				pheader->gltextures[i][j&3] = pheader->gltextures[i][j - k];
		}
	}

	return (void *)pskintype;
}
TODO
*/

static void Mod_CalcAliasBounds (aliashdr_t *a) // johnfitz -- calculate bounds
{ // of alias model for nonrotated, yawrotated, and fullrotated cases
	for (int i = 0; i < 3; i++) { //clear out all data
		loadmodel->mins[i] = loadmodel->ymins[i] = loadmodel->rmins[i] = 999999;
		loadmodel->maxs[i] = loadmodel->ymaxs[i] = loadmodel->rmaxs[i] = -999999;
	}
	float radius = 0;
	float yawradius = 0;
	for (int i = 0; i < a->numposes; i++) //process verts
		for (int j = 0; j < a->numverts; j++) {
			vec3_t v;
			for (int k = 0; k < 3; k++)
				v[k] = poseverts[i][j].v[k]*pheader->scale[k]+pheader->scale_origin[k];
			for (int k = 0; k < 3; k++) {
				loadmodel->mins[k] = fmin(loadmodel->mins[k], v[k]);
				loadmodel->maxs[k] = fmax(loadmodel->maxs[k], v[k]);
			}
			float dist = v[0] * v[0] + v[1] * v[1];
			if (yawradius < dist)
				yawradius = dist;
			dist += v[2] * v[2];
			if (radius < dist)
				radius = dist;
		}
}

static qboolean nameInList(const char *list, const char *name)
{
	char tmp[MAX_QPATH];
	const char *s = list;
	while (*s) {
		int i = 0; // make a copy until the next comma or end of string
		while (*s && *s != ',') {
			if (i < MAX_QPATH - 1)
				tmp[i++] = *s;
			s++;
		}
		tmp[i] = '\0';
		if (!strcmp(name, tmp)) //compare it to the model name
			return true;
		while (*s && *s == ',')
			s++; // search forwards to next comma or end of string
	}
	return false;
}

void Mod_SetExtraFlags (model_t *mod)
{ // johnfitz -- set up extra flags that aren't in the mdl
	extern cvar_t r_nolerp_list, r_noshadow_list;
	if (!mod || mod->type != mod_alias)
		return;
	mod->flags &= (0xFF | MF_HOLEY); //only preserve first byte, plus MF_HOLEY
	if (nameInList(r_nolerp_list.string, mod->name))
		mod->flags |= MOD_NOLERP;
	if (nameInList(r_noshadow_list.string, mod->name))
		mod->flags |= MOD_NOSHADOW;
	// fullbright hack (TODO: make this a cvar list)
	if (!strcmp (mod->name, "progs/flame2.mdl") ||
		!strcmp (mod->name, "progs/flame.mdl") ||
		!strcmp (mod->name, "progs/boss.mdl"))
			mod->flags |= MOD_FBRIGHTHACK;
}

void *Mod_LoadAliasSkin(void *pin, int *pskinindex, int skinsize,
		aliashdr_t *pheader)
{
        byte *pskin = Hunk_AllocName (skinsize, loadname);
        byte *pinskin = (byte *)pin;
        *pskinindex = (byte *)pskin - (byte *)pheader;
        memcpy (pskin, pinskin, skinsize);
        pinskin += skinsize;
        return ((void *)pinskin);
}

void *Mod_LoadAliasSkinGroup(void *pin, int *pskinindex, int skinsize,
		aliashdr_t *pheader)
{
        daliasskingroup_t *pinskingroup = (daliasskingroup_t *) pin;
        int numskins = LittleLong(pinskingroup->numskins);
        maliasskingroup_t *paliasskingroup =
                Hunk_AllocName(sizeof(maliasskingroup_t) + (numskins - 1) *
			sizeof(paliasskingroup->skindescs[0]), loadname);
        paliasskingroup->numskins = numskins;
        *pskinindex = (byte *) paliasskingroup - (byte *) pheader;
        daliasskininterval_t *pinskinintervals =
                (daliasskininterval_t *) (pinskingroup + 1);
        float *poutskinintervals = Hunk_AllocName(numskins * sizeof(float),
                                                        loadname);
        paliasskingroup->intervals = (byte*)poutskinintervals - (byte*)pheader;
        for (int i = 0; i < numskins; i++) {
                *poutskinintervals = LittleFloat(pinskinintervals->interval);
                if (*poutskinintervals <= 0)
                        Sys_Error("Mod_LoadAliasSkinGroup: interval<=0");
                poutskinintervals++;
                pinskinintervals++;
        }
        void *ptemp = (void *)pinskinintervals;
        for (int i = 0; i < numskins; i++) {
                ptemp = Mod_LoadAliasSkin(ptemp,
			&paliasskingroup->skindescs[i].skin, skinsize, pheader);
        }
        return ptemp;
}

void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
        int                                     i, numskins;
        mdl_t                           *pmodel, *pinmodel;
        stvert_t                        *pstverts, *pinstverts;
        mtriangle_t                     *ptri;
        dtriangle_t                     *pintriangles;
        int                                     version, numframes;
        int                                     size;
        daliasframetype_t       *pframetype;
        daliasskintype_t        *pskintype;
        maliasskindesc_t        *pskindesc;
        int                                     skinsize;
        int                                     start, end, total;

        start = Hunk_LowMark ();

        pinmodel = (mdl_t *)buffer;
        mod_base = (byte *)buffer; //johnfitz

        version = LittleLong (pinmodel->version);
        if (version != ALIAS_VERSION)
                Host_Error ("Mod_LoadAliasModel: %s has wrong version number (%d should be %d)",
                        mod->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
        size =  sizeof (aliashdr_t) +
                 (LittleLong (pinmodel->numframes) - 1) * sizeof (pheader->frames[0])
                        + sizeof (mdl_t) + LittleLong (pinmodel->numverts)
                                * sizeof (stvert_t)
                        + LittleLong (pinmodel->numtris) * sizeof (mtriangle_t)
                                ;
        pheader = (aliashdr_t *)Hunk_AllocName (size, loadname);
        pmodel = (mdl_t *) ((byte *)&pheader[1] + (LittleLong (pinmodel->numframes) - 1) * sizeof (pheader->frames[0]));

	pheader->flags = mod->flags = LittleLong(pinmodel->flags);
//
// endian-adjust and copy the data, starting with the alias model header
//
        pmodel->boundingradius =
                pheader->boundingradius = LittleFloat (pinmodel->boundingradius);
        pmodel->numskins =
                pheader->numskins = LittleLong (pinmodel->numskins);
        pmodel->skinwidth =
                pheader->skinwidth = LittleLong (pinmodel->skinwidth);
        pmodel->skinheight =
                pheader->skinheight = LittleLong (pinmodel->skinheight);

        if (pheader->skinheight > MAX_LBM_HEIGHT)
                Host_Error ("Mod_LoadAliasModel: model %s has a skin taller than %d", mod->name,
                        MAX_LBM_HEIGHT);

        pmodel->numverts =
                pheader->numverts = LittleLong (pinmodel->numverts);

        if (pheader->numverts <= 0)
                Host_Error ("Mod_LoadAliasModel: model %s has no vertices", mod->name);

        if (pheader->numverts > MAXALIASVERTS)
                Host_Error ("Mod_LoadAliasModel: model %s has too many vertices (%d, max = %d)", mod->name, pheader->numverts, MAXALIASVERTS);     //WINQUAKE

        if (pheader->numtris > MAXALIASTRIS) // Baker: This technically does not apply to WinQuake only GLQuake ... but for consistency ...
                Host_Error ("Mod_LoadAliasModel: model %s has too many triangles (%d, max = %d)", mod->name, pheader->numtris, MAXALIASTRIS);  //Spike -- added this check, because I'm segfaulting out.

	if (pmodel->numverts > MAXALIASVERTS)
		Host_Error ("Mod_LoadAliasModel: model %s has too many vertexes (%d, max = %d)", mod->name, pmodel->numverts, MAXALIASVERTS);

        pmodel->numtris =
                pheader->numtris = LittleLong (pinmodel->numtris);

        if (pheader->numtris <= 0)
                Host_Error ("Mod_LoadAliasModel: model %s has no triangles", mod->name);

        if (pheader->numtris > MAXALIASTRIS)
        {       //Spike -- added this check, because I'm segfaulting out.
                Host_Error ("model %s has too many triangles (%d > %d)", mod->name, pheader->numtris, MAXALIASTRIS);
        }


        pmodel->numframes =
                pheader->numframes = LittleLong (pinmodel->numframes);

        numframes = pheader->numframes;
        if (numframes < 1)
                Host_Error ("Mod_LoadAliasModel: Invalid # of frames: %d", numframes);

        pmodel->size =
                pheader->size = LittleFloat (pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
        mod->synctype = (synctype_t)LittleLong (pinmodel->synctype);
        mod->numframes = pheader->numframes;

        for (i=0 ; i<3 ; i++)
        {
                pmodel->scale[i] =
                        pheader->scale[i] = LittleFloat (pinmodel->scale[i]);
                pmodel->scale_origin[i] =
                        pheader->scale_origin[i] = LittleFloat (pinmodel->scale_origin[i]);
                pmodel->eyeposition[i] =
                        pheader->eyeposition[i] = LittleFloat (pinmodel->eyeposition[i]);
        }

        numskins = pmodel->numskins;

        if (pmodel->skinwidth & 0x03)
                Host_Error ("Mod_LoadAliasModel: skinwidth not multiple of 4");

        pheader->model = (byte *)pmodel - (byte *)pheader;

//
// load the skins
//
        skinsize = pmodel->skinheight * pmodel->skinwidth;

        if (numskins < 1)
                Host_Error ("Mod_LoadAliasModel: Invalid # of skins: %d", numskins);

        pskintype = (daliasskintype_t *)&pinmodel[1];
        pskindesc = Hunk_AllocName (numskins * sizeof (maliasskindesc_t), loadname);

        pheader->skindesc = (byte *)pskindesc - (byte *)pheader;

        for (i=0 ; i<numskins ; i++)
        {
                aliasskintype_t skintype;

                skintype = LittleLong (pskintype->type);
                pskindesc[i].type = skintype;

                if (skintype == ALIAS_SKIN_SINGLE)
                {
                        pskintype = (daliasskintype_t *) Mod_LoadAliasSkin (pskintype + 1,
                                                &pskindesc[i].skin, skinsize, pheader);
                }
                else
                {
                        pskintype = (daliasskintype_t *) Mod_LoadAliasSkinGroup (pskintype + 1,
                                                                                        &pskindesc[i].skin, skinsize, pheader);
                }
        }

//
// load base s and t vertices
//

// set base s and t vertices
        pstverts = (stvert_t *)&pmodel[1];
        pinstverts = (stvert_t *)pskintype;

        pheader->stverts = (byte *)pstverts - (byte *)pheader;

        for (i=0 ; i<pheader->numverts ; i++)
        {
                pstverts[i].onseam = LittleLong (pinstverts[i].onseam);
        // put s and t in 16.16 format
                pstverts[i].s = LittleLong (pinstverts[i].s) << 16;
                pstverts[i].t = LittleLong (pinstverts[i].t) << 16;
        }

//
// load triangle lists
//

// set up the triangles
        ptri = (mtriangle_t *)&pstverts[pmodel->numverts];
        pintriangles = (dtriangle_t *)&pinstverts[pmodel->numverts];

        pheader->triangles = (byte *)ptri - (byte *)pheader;

        for (i=0 ; i<pheader->numtris ; i++)
        {
                int             j;
                ptri[i].facesfront = LittleLong (pintriangles[i].facesfront);

                for (j=0 ; j<3 ; j++)
                {
                        ptri[i].vertindex[j] =
                                LittleLong (pintriangles[i].vertindex[j]);
                }
        }

//
// load the frames
//
        posenum = 0;
        pframetype = (daliasframetype_t *)&pintriangles[pmodel->numtris];

        for (i=0 ; i<numframes ; i++)
        {
                aliasframetype_t        frametype;
                frametype = (aliasframetype_t)LittleLong (pframetype->type);
                pheader->frames[i].type = frametype;

                if (frametype == ALIAS_SINGLE)
                        pframetype = (daliasframetype_t *) Mod_LoadAliasFrame (pframetype + 1, &pheader->frames[i].frame, pmodel->numverts, &pheader->frames[i].bboxmin, &pheader->frames[i].bboxmax, pheader, pheader->frames[i].name, &pheader->frames[i],0);
                else
                        pframetype = (daliasframetype_t *) Mod_LoadAliasGroup (pframetype + 1, &pheader->frames[i].frame, pmodel->numverts, &pheader->frames[i].bboxmin, &pheader->frames[i].bboxmax, pheader, pheader->frames[i].name, &pheader->frames[i]);
        }

        pheader->numposes = posenum; // Baker: Watch this and compare vs. Mark V

        mod->type = mod_alias;

        Mod_SetExtraFlags (mod); //johnfitz

        Mod_CalcAliasBounds (pheader); //johnfitz

#if 0 // Baker: That skeleton map shows this flaw, doesn't it?
// FIXME: do this right
        mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
        mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;
#endif

//
// move the complete, relocatable alias model to the cache
//
        end = Hunk_LowMark ();
        total = end - start;

        Cache_Alloc (&mod->cache, total, loadname);
        if (!mod->cache.data)
                return;
        memcpy (mod->cache.data, pheader, total);

        Hunk_FreeToLowMark (start);
}

void *Mod_LoadSpriteFrame(void *pin, mspriteframe_t **ppframe)
{
	int width, height, size, origin[2];
	dspriteframe_t *pinframe = (dspriteframe_t *) pin;
	width = LittleLong(pinframe->width);
	height = LittleLong(pinframe->height);
	size = width * height;
	mspriteframe_t *pspriteframe = Hunk_AllocName(sizeof(mspriteframe_t) +
			size * r_pixbytes, loadname);
	Q_memset(pspriteframe, 0, sizeof(mspriteframe_t) + size);
	*ppframe = pspriteframe;
	pspriteframe->width = width;
	pspriteframe->height = height;
	origin[0] = LittleLong(pinframe->origin[0]);
	origin[1] = LittleLong(pinframe->origin[1]);
	pspriteframe->up = origin[1];
	pspriteframe->down = origin[1] - height;
	pspriteframe->left = origin[0];
	pspriteframe->right = width + origin[0];
	if (r_pixbytes == 1)
		Q_memcpy(&pspriteframe->pixels[0], (byte *) (pinframe+1), size);
	else {
		Sys_Error ("Mod_LoadSpriteFrame: driver set invalid r_pixbytes: %d\n", r_pixbytes);
	}
	return (void *)((byte *) pinframe + sizeof(dspriteframe_t) + size);
}

void *Mod_LoadSpriteGroup(void *pin, mspriteframe_t **ppframe)
{
        dspritegroup_t *pingroup = (dspritegroup_t *) pin;
        int numframes = LittleLong(pingroup->numframes);
        mspritegroup_t *pspritegroup = Hunk_AllocName(sizeof(mspritegroup_t) +
                (numframes - 1) * sizeof(pspritegroup->frames[0]), loadname);
        pspritegroup->numframes = numframes;
        *ppframe = (mspriteframe_t *) pspritegroup;
        dspriteinterval_t *pin_intervals = (dspriteinterval_t *) (pingroup + 1);
        float *poutintervals = Hunk_AllocName(numframes*sizeof(float),loadname);
        pspritegroup->intervals = poutintervals;
        for (int i = 0; i < numframes; i++) {
                *poutintervals = LittleFloat(pin_intervals->interval);
                if (*poutintervals <= 0.0)
                        Sys_Error("Mod_LoadSpriteGroup: interval<=0");
                poutintervals++;
                pin_intervals++;
        }
        void *ptemp = (void *)pin_intervals;
        for (int i = 0; i < numframes; i++) {
                ptemp = Mod_LoadSpriteFrame(ptemp, &pspritegroup->frames[i]);
        }
        return ptemp;
}

void Mod_LoadSpriteModel(model_t *mod, void *buffer)
{
        dspriteframetype_t *pframetype;
        dsprite_t *pin = (dsprite_t *) buffer;
        int version = LittleLong(pin->version);
        if (version != SPRITE_VERSION)
                Sys_Error("%s has wrong version number "
                                "(%i should be %i)", mod->name, version,
                                SPRITE_VERSION);
        int numframes = LittleLong(pin->numframes);
        msprite_t *psprite;
        int size = sizeof(msprite_t) + (numframes - 1) * sizeof(psprite->frames);
        psprite = Hunk_AllocName(size, loadname);
        mod->cache.data = psprite;
        psprite->type = LittleLong(pin->type);
        psprite->maxwidth = LittleLong(pin->width);
        psprite->maxheight = LittleLong(pin->height);
        psprite->beamlength = LittleFloat(pin->beamlength);
        mod->synctype = LittleLong(pin->synctype);
        psprite->numframes = numframes;
        mod->mins[0] = mod->mins[1] = -psprite->maxwidth / 2;
        mod->maxs[0] = mod->maxs[1] = psprite->maxwidth / 2;
        mod->mins[2] = -psprite->maxheight / 2;
        mod->maxs[2] = psprite->maxheight / 2;
        // load the frames
        if (numframes < 1)
                Sys_Error("Mod_LoadSpriteModel: Invalid # of frames: %d\n",
                                numframes);
        mod->numframes = numframes;
        mod->flags = 0;
        pframetype = (dspriteframetype_t *) (pin + 1);
        for (int i = 0; i < numframes; i++) {
                spriteframetype_t frametype;
                frametype = LittleLong(pframetype->type);
                psprite->frames[i].type = frametype;
                if (frametype == SPR_SINGLE) {
                        pframetype = (dspriteframetype_t *)
                                Mod_LoadSpriteFrame(pframetype + 1,
                                                &psprite->frames[i].frameptr);
                } else {
                        pframetype = (dspriteframetype_t *)
                                Mod_LoadSpriteGroup(pframetype + 1,
                                                &psprite->frames[i].frameptr);
                }
        }
        mod->type = mod_sprite;
}

static void Mod_Print ()
{
	Con_SafePrintf ("Cached models:\n"); //johnfitz -- safeprint instead of print
	int i = 0;
	model_t *mod = mod_known;
	for (; i < mod_numknown; i++, mod++)
		Con_SafePrintf ("%8p : %s\n", mod->cache.data, mod->name); //johnfitz -- safeprint instead of print
	Con_Printf ("%i models\n",mod_numknown); //johnfitz -- print the total too
}

