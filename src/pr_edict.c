// Copyright (C) 1996-2001 Id Software, Inc.
// Copyright (C) 2002-2009 John Fitzgibbons and others
// GPLv3 See LICENSE for details.

// sv_edict.c -- entity dictionary

#include "quakedef.h"

#define MAX_FIELD_LEN 64
#define GEFV_CACHESIZE 2
#define PR_STRING_ALLOCSLOTS 256

typedef struct {
	ddef_t *pcache;
	char field[MAX_FIELD_LEN];
} gefv_cache;

dprograms_t *progs;
dfunction_t *pr_functions;
dstatement_t *pr_statements;
globalvars_t *pr_global_struct;
float *pr_globals; // same as pr_global_struct
int pr_edict_size; // in bytes
unsigned short pr_crc;

char *pr_strings; // no one should access this. not static
		// only for two stupid sv_main.c uses.
static int pr_stringssize;
static char **pr_knownstrings;
static int pr_maxknownstrings;
static int pr_numknownstrings;
static ddef_t *pr_fielddefs;
static ddef_t *pr_globaldefs;
static gefv_cache gefvCache[GEFV_CACHESIZE] = { { NULL, "" }, { NULL, "" } };

int type_size[8] = {
	1, // ev_void
	1, // sizeof(string_t) / 4 // ev_string
	1, // ev_float
	3, // ev_vector
	1, // ev_entity
	1, // ev_field
	1, // sizeof(func_t) / 4 // ev_function
	1 // sizeof(void *) / 4 // ev_pointer
};

cvar_t nomonsters = { "nomonsters", "0", false, false, 0, NULL };
cvar_t gamecfg = { "gamecfg", "0", false, false, 0, NULL };
cvar_t scratch1 = { "scratch1", "0", false, false, 0, NULL };
cvar_t scratch2 = { "scratch2", "0", false, false, 0, NULL };
cvar_t scratch3 = { "scratch3", "0", false, false, 0, NULL };
cvar_t scratch4 = { "scratch4", "0", false, false, 0, NULL };
cvar_t savedgamecfg = { "savedgamecfg", "0", true, false, 0, NULL };
cvar_t saved1 = { "saved1", "0", true, false, 0, NULL };
cvar_t saved2 = { "saved2", "0", true, false, 0, NULL };
cvar_t saved3 = { "saved3", "0", true, false, 0, NULL };
cvar_t saved4 = { "saved4", "0", true, false, 0, NULL };

ddef_t *ED_FieldAtOfs(int ofs);
qboolean ED_ParseEpair(void *base, ddef_t * key, char *s);

char *PR_GetString(int num)
{
	if (num >= 0 && num < pr_stringssize)
		return pr_strings + num;
	else if (num < 0 && num >= -pr_numknownstrings) {
		if (!pr_knownstrings[-1 - num]) {
			Host_Error ("PR_GetString: attempt to get a non-existant string %d\n", num);
			return "";
		}
		return pr_knownstrings[-1 - num];
	} else {
		//Host_Error("PR_GetString: invalid string offset %d\n", num);
		//Gibbon - Who cares about offsets here, it works fine :)
		return "";
	}
}


void ED_ClearEdict(edict_t *e) // Sets everything to NULL
{
	memset(&e->v, 0, progs->entityfields * 4);
	e->free = false;
}

// Either finds a free edict, or allocates a new one.
// Try to avoid reusing an entity that was recently freed, because it
// can cause the client to think the entity morphed into something else
// instead of being removed and recreated, which can cause interpolated
// angles and bad trails.
edict_t *ED_Alloc()
{
	int i;
	edict_t *e;
	for (i = svs.maxclients + 1; i < sv.num_edicts; i++) {
		e = EDICT_NUM(i);
		// the first couple seconds of server time can involve a lot of
		// freeing and allocating, so relax the replacement policy
		if (e->free && (e->freetime < 2 || sv.time - e->freetime > 0.5)) {
			ED_ClearEdict(e);
			return e;
		}
	}
	if (i == sv.max_edicts) //johnfitz -- use sv.max_edicts instead of MAX_EDICTS
		Host_Error("ED_Alloc: no free edicts (max_edicts is %i)", sv.max_edicts); //johnfitz -- was Sys_Error
	sv.num_edicts++;
	e = EDICT_NUM(i);
	ED_ClearEdict(e);
	return e;
}

void ED_Free(edict_t *ed) // Marks the edict as free
{ // FIXME: walk all entities and NULL out references to this entity
	SV_UnlinkEdict(ed); // unlink from world bsp
	ed->free = true;
	ed->v.model = 0;
	ed->v.takedamage = 0;
	ed->v.modelindex = 0;
	ed->v.colormap = 0;
	ed->v.skin = 0;
	ed->v.frame = 0;
	VectorCopy(vec3_origin, ed->v.origin);
	VectorCopy(vec3_origin, ed->v.angles);
	ed->v.nextthink = -1;
	ed->v.solid = 0;
	ed->freetime = sv.time;
}

ddef_t *ED_GlobalAtOfs(int ofs)
{
	for (int i = 0; i < progs->numglobaldefs; i++) {
		ddef_t *def = &pr_globaldefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

ddef_t *ED_FieldAtOfs(int ofs)
{
	for (int i = 0; i < progs->numfielddefs; i++) {
		ddef_t *def = &pr_fielddefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

ddef_t *ED_FindField(char *name)
{
	for (int i = 0; i < progs->numfielddefs; i++) {
		ddef_t *def = &pr_fielddefs[i];
		if (!strcmp(PR_GetString(def->s_name), name))
			return def;
	}
	return NULL;
}

ddef_t *ED_FindGlobal(char *name)
{
	for (int i = 0; i < progs->numglobaldefs; i++) {
		ddef_t *def = &pr_globaldefs[i];
		if (!strcmp(PR_GetString(def->s_name), name))
			return def;
	}
	return NULL;
}

dfunction_t *ED_FindFunction(char *name)
{
	for (int i = 0; i < progs->numfunctions; i++) {
		dfunction_t *func = &pr_functions[i];
		if (!strcmp(PR_GetString(func->s_name), name))
			return func;
	}
	return NULL;
}

eval_t *GetEdictFieldValue(edict_t *ed, char *field)
{
	static int rep = 0;
	ddef_t *def = NULL;
	for (int i = 0; i < GEFV_CACHESIZE; i++) {
		if (!strcmp(field, gefvCache[i].field)) {
			def = gefvCache[i].pcache;
			goto Done;
		}
	}
	def = ED_FindField(field);
	if (strlen(field) < MAX_FIELD_LEN) {
		gefvCache[rep].pcache = def;
		strcpy(gefvCache[rep].field, field);
		rep ^= 1;
	}
Done:
	if (!def)
		return NULL;
	return (eval_t *) ((char *)&ed->v + def->ofs * 4);
}


char *PR_ValueString(etype_t type, eval_t *val)
{ // Returns a string describing *data in a type specific manner
	static char line[256];
	type &= ~DEF_SAVEGLOBAL;
	switch (type) {
		case ev_string:
			sprintf(line, "%s", PR_GetString(val->string));
			break;
		case ev_entity:
			sprintf(line, "entity %i",
				NUM_FOR_EDICT(PROG_TO_EDICT(val->edict)));
			break;
		case ev_function:
			dfunction_t *f = pr_functions + val->function;
			sprintf(line, "%s()", PR_GetString(f->s_name));
			break;
		case ev_field:
			ddef_t *def = ED_FieldAtOfs(val->_int);
			sprintf(line, ".%s", PR_GetString(def->s_name));
			break;
		case ev_void:
			sprintf(line, "void");
			break;
		case ev_float:
			sprintf(line, "%5.1f", val->_float);
			break;
		case ev_vector:
			sprintf(line, "'%5.1f %5.1f %5.1f'", val->vector[0],
					val->vector[1], val->vector[2]);
			break;
		case ev_pointer:
			sprintf(line, "pointer");
			break;
		default:
			sprintf(line, "bad type %i", type);
			break;
	}
	return line;
}


char *PR_UglyValueString(etype_t type, eval_t *val) // Rets a string describing
{ // *data in a type specific manner, easier to parse than PR_ValueString
	static char line[256];
	type &= ~DEF_SAVEGLOBAL;
	switch (type) {
		case ev_string:
			sprintf(line, "%s", PR_GetString(val->string));
			break;
		case ev_entity:
			sprintf(line, "%i",
				NUM_FOR_EDICT(PROG_TO_EDICT(val->edict)));
			break;
		case ev_function:
			dfunction_t *f = pr_functions + val->function;
			sprintf(line, "%s", PR_GetString(f->s_name));
			break;
		case ev_field:
			ddef_t *def = ED_FieldAtOfs(val->_int);
			sprintf(line, "%s", PR_GetString(def->s_name));
			break;
		case ev_void:
			sprintf(line, "void");
			break;
		case ev_float:
			sprintf(line, "%f", val->_float);
			break;
		case ev_vector:
			sprintf(line, "%f %f %f", val->vector[0],
					val->vector[1], val->vector[2]);
			break;
		default:
			sprintf(line, "bad type %i", type);
			break;
	}
	return line;
}


char *PR_GlobalString(int ofs) // Returns a string with a description and the
{ // contents of a global, padded to 20 field width
	static char line[128];
	void *val = (void *)&pr_globals[ofs];
	ddef_t *def = ED_GlobalAtOfs(ofs);
	if (!def)
		sprintf(line, "%i(??\?)", ofs);
	else {
		char *s = PR_ValueString(def->type, (eval_t *) val);
		sprintf(line, "%i(%s)%s", ofs, PR_GetString(def->s_name), s);
	}
	int i = strlen(line);
	for (; i < 20; i++)
		strcat(line, " ");
	strcat(line, " ");
	return line;
}

char *PR_GlobalStringNoContents(int ofs)
{
	static char line[128];
	ddef_t *def = ED_GlobalAtOfs(ofs);
	if (!def)
		sprintf(line, "%i(??\?)", ofs);
	else
		sprintf(line, "%i(%s)", ofs, PR_GetString(def->s_name));
	int i = strlen(line);
	for (; i < 20; i++)
		strcat(line, " ");
	strcat(line, " ");
	return line;
}

void ED_Print(edict_t *ed)
{ // For debugging
	if (ed->free) {
		Con_Printf("FREE\n");
		return;
	}
	Con_SafePrintf("\nEDICT %i:\n", NUM_FOR_EDICT(ed)); //johnfitz -- was Con_Printf
	for (int i = 1; i < progs->numfielddefs; i++) {
		ddef_t *d = &pr_fielddefs[i];
		char *name = PR_GetString(d->s_name);
		int l = strlen(name);
		if (l > 1 && name[l - 2] == '_')
			continue; // skip _x, _y, _z vars
		int *v = (int *)((char *)&ed->v + d->ofs * 4);
		// if the value is still all 0, skip the field
		int type = d->type & ~DEF_SAVEGLOBAL;
		int j = 0;
		for (; j < type_size[type]; j++)
			if (v[j])
				break;
		if (j == type_size[type])
			continue;
		Con_SafePrintf("%s", name); //johnfitz -- was Con_Printf
		while (l++ < 15)
			Con_SafePrintf(" "); //johnfitz -- was Con_Printf
		Con_SafePrintf("%s\n", PR_ValueString(d->type, (eval_t *) v)); //johnfitz -- was Con_Printf
	}
}

void ED_Write(FILE *f, edict_t *ed)
{ // For savegames
	fprintf(f, "{\n");
	if (ed->free) {
		fprintf(f, "}\n");
		return;
	}
	for (int i = 1; i < progs->numfielddefs; i++) {
		ddef_t *d = &pr_fielddefs[i];
		char *name = PR_GetString(d->s_name);
		int j = strlen(name);
		if (j > 1 && name[j - 2] == '_')
			continue; // skip _x, _y, _z vars
		int *v = (int *)((char *)&ed->v + d->ofs * 4);
		// if the value is still all 0, skip the field
		int type = d->type & ~DEF_SAVEGLOBAL;
		for (j = 0; j < type_size[type]; j++)
			if (v[j])
				break;
		if (j == type_size[type])
			continue;
		fprintf(f, "\"%s\" ", name);
		fprintf(f, "\"%s\"\n", PR_UglyValueString(d->type, (eval_t*)v));
	}
	fprintf(f, "}\n");
}

void ED_PrintNum(int ent)
{
	ED_Print(EDICT_NUM(ent));
}

void ED_PrintEdicts()
{ // For debugging, prints all the entities in the current server
	int i;

	Con_Printf("%i entities\n", sv.num_edicts);
	for (i = 0; i < sv.num_edicts; i++)
		ED_PrintNum(i);
}

void ED_PrintEdict_f()
{ // For debugging, prints a single edicy
	int i;

	i = Q_atoi(Cmd_Argv(1));
	if (i >= sv.num_edicts) {
		Con_Printf("Bad edict number\n");
		return;
	}
	ED_PrintNum(i);
}

void ED_Count()
{ // For debugging
	int active, models, solid, step;
	active = models = solid = step = 0;
	for (int i = 0; i < sv.num_edicts; i++) {
		edict_t *ent = EDICT_NUM(i);
		if (ent->free)
			continue;
		active++;
		if (ent->v.solid)
			solid++;
		if (ent->v.model)
			models++;
		if (ent->v.movetype == MOVETYPE_STEP)
			step++;
	}
	Con_Printf("num_edicts:%3i\n", sv.num_edicts);
	Con_Printf("active :%3i\n", active);
	Con_Printf("view :%3i\n", models);
	Con_Printf("touch :%3i\n", solid);
	Con_Printf("step :%3i\n", step);
}

void ED_WriteGlobals(FILE *f)
{ // FIXME: need to tag constants, doesn't really work
	fprintf(f, "{\n");
	for (int i = 0; i < progs->numglobaldefs; i++) {
		ddef_t *def = &pr_globaldefs[i];
		int type = def->type;
		if (!(def->type & DEF_SAVEGLOBAL))
			continue;
		type &= ~DEF_SAVEGLOBAL;
		if (type != ev_string && type != ev_float && type != ev_entity)
			continue;
		char *name = PR_GetString(def->s_name);
		fprintf(f, "\"%s\" ", name);
		fprintf(f, "\"%s\"\n", PR_UglyValueString(type,
					(eval_t *) & pr_globals[def->ofs]));
	}
	fprintf(f, "}\n");
}

void ED_ParseGlobals(char *data)
{
	char keyname[64];
	while (1) {
		data = COM_Parse(data); // parse key
		if (com_token[0] == '}')
			break;
		if (!data)
			Sys_Error("ED_ParseEntity: EOF without closing brace");
		strcpy(keyname, com_token);
		data = COM_Parse(data); // parse value
		if (!data)
			Sys_Error("ED_ParseEntity: EOF without closing brace");
		if (com_token[0] == '}')
			Sys_Error("ED_ParseEntity: closing brace without data");
		ddef_t *key = ED_FindGlobal(keyname);
		if (!key) {
			Con_Printf("'%s' is not a global\n", keyname);
			continue;
		}
		if (!ED_ParseEpair((void *)pr_globals, key, com_token))
			Host_Error("ED_ParseGlobals: parse error");
	}
}

static string_t ED_NewString(const char *string)
{
	int l = strlen(string) + 1;
	char *new_p;
	string_t num = PR_AllocString(l, &new_p);
	for (int i = 0; i < l; i++) {
		if (string[i] == '\\' && i < l - 1) {
			i++;
			if (string[i] == 'n')
				*new_p++ = '\n';
			else
				*new_p++ = '\\';
		} else
			*new_p++ = string[i];
	}
	return num;
}


qboolean ED_ParseEpair(void *base, ddef_t *key, char *s)
{ // Can parse either fields or globals, returns false if error
	void *d = (void *)((int *)base + key->ofs);
	switch (key->type & ~DEF_SAVEGLOBAL) {
		case ev_string:
			*(string_t *) d = ED_NewString(s);
			break;
		case ev_float:
			*(float *)d = atof(s);
			break;
		case ev_vector:
			char string[128];
			strcpy(string, s);
			char *v = string;
			char *w = string;
			for (int i = 0; i < 3; i++) {
				while (*v && *v != ' ')
					v++;
				*v = 0;
				((float *)d)[i] = atof(w);
				w = v = v + 1;
			}
			break;
		case ev_entity:
			*(int *)d = EDICT_TO_PROG(EDICT_NUM(atoi(s)));
			break;
		case ev_field:
			ddef_t *def = ED_FindField(s);
			if (!def) {
				//johnfitz -- HACK -- suppress error becuase fog/sky fields might not be mentioned in defs.qc
				if (strncmp(s, "sky", 3) && strcmp(s, "fog"))
					Con_DPrintf("Can't find field %s\n", s);
				return false;
			}
			*(int *)d = G_INT(def->ofs);
			break;
		case ev_function:
			dfunction_t *func = ED_FindFunction(s);
			if (!func) {
				Con_Printf("Can't find function %s\n", s);
				return false;
			}
			*(func_t *) d = func - pr_functions;
			break;
		default:
			break;
	}
	return true;
}

// Parses an edict out of the given string, returning the new position
// ed should be a properly initialized empty edict.
// Used for initial level load and for savegames.
char *ED_ParseEdict(char *data, edict_t *ent)
{
	qboolean init = false;
	if (ent != sv.edicts) // hack
		memset(&ent->v, 0, progs->entityfields * 4); // clear it
	while (1) { // go through all the dictionary pairs
		data = COM_Parse(data); // parse key
		if (com_token[0] == '}')
			break;
		if (!data)
			Sys_Error("ED_ParseEntity: EOF without closing brace");
		// anglehack is to allow QuakeEd to write single scalar angles
		// and allow them to be turned into vectors. (FIXME...)
		qboolean anglehack = false;
		if (!strcmp(com_token, "angle")) {
			strcpy(com_token, "angles");
			anglehack = true;
		}
		// FIXME: change light to _light to get rid of this hack
		if (!strcmp(com_token, "light"))
			strcpy(com_token, "light_lev"); // hack for single light def
		char keyname[256];
		strcpy(keyname, com_token);
		// another hack to fix keynames with trailing spaces
		int n = strlen(keyname);
		while (n && keyname[n - 1] == ' ') {
			keyname[n - 1] = 0;
			n--;
		}
		data = COM_Parse(data); // parse value
		if (!data)
			Sys_Error("ED_ParseEntity: EOF without closing brace");
		if (com_token[0] == '}')
			Sys_Error("ED_ParseEntity: closing brace without data");
		init = true;
		// keynames with a leading underscore are used for utility
		// comments, and are immediately discarded by quake
		if (keyname[0] == '_')
			continue;
		ddef_t *key = ED_FindField(keyname);
		if (!key) {
			//johnfitz -- HACK -- suppress error becuase fog/sky/alpha fields might not be mentioned in defs.qc
			if (strncmp(keyname, "sky", 3) && strcmp(keyname, "fog")
					&& strcmp(keyname, "alpha"))
				Con_DPrintf("\"%s\" is not a field\n", keyname); //johnfitz -- was Con_Printf
			continue;
		}
		if (anglehack) {
			char temp[32];
			strcpy(temp, com_token);
			sprintf(com_token, "0 %s 0", temp);
		}
		if (!ED_ParseEpair((void *)&ent->v, key, com_token))
			Host_Error("ED_ParseEdict: parse error");
	}
	if (!init)
		ent->free = true;
	return data;
}

// The entities are directly placed in the array, rather than allocated with
// ED_Alloc, because otherwise an error loading the map would have entity
// number references out of order.
// Creates a server's entity / program execution context by
// parsing textual entity definitions out of an ent file.
// Used for both fresh maps and savegame loads. A fresh map would also need
// to call ED_CallSpawnFunctions () to let the objects initialize themselves.
void ED_LoadFromFile(char *data)
{
	edict_t *ent = NULL;
	int inhibit = 0;
	pr_global_struct->time = sv.time;
	while (1) { // parse ents
		data = COM_Parse(data); // parse the opening brace;
		if (!data)
			break;
		if (com_token[0] != '{')
			Sys_Error("ED_LoadFromFile: found %s when expecting {",
					com_token);
		ent = !ent ? EDICT_NUM(0) : ED_Alloc();
		data = ED_ParseEdict(data, ent);
		// remove things from different skill levels or deathmatch
		if (deathmatch.value) {
			if (((int)ent->v.spawnflags&SPAWNFLAG_NOT_DEATHMATCH)) {
				ED_Free(ent);
				inhibit++;
				continue;
			}
		} else
			if ((current_skill == 0 && ((int)ent->v.spawnflags &
				SPAWNFLAG_NOT_EASY)) || (current_skill == 1 &&
				((int)ent->v.spawnflags & SPAWNFLAG_NOT_MEDIUM))
				|| (current_skill>=2 && ((int)ent->v.spawnflags
				& SPAWNFLAG_NOT_HARD))) {
				ED_Free(ent);
				inhibit++;
				continue;
			} // immediately call spawn function
		if (!ent->v.classname) {
			Con_SafePrintf("No classname for:\n"); //johnfitz -- was Con_Printf
			ED_Print(ent);
			ED_Free(ent);
			continue;
		}
		// look for the spawn function
		dfunction_t *func = ED_FindFunction(PR_GetString(ent->v.classname));
		if (!func) {
			Con_SafePrintf("No spawn function for:\n"); //johnfitz -- was Con_Printf
			ED_Print(ent);
			ED_Free(ent);
			continue;
		}
		pr_global_struct->self = EDICT_TO_PROG(ent);
		PR_ExecuteProgram(func - pr_functions);
	}
	Con_DPrintf("%i entities inhibited\n", inhibit);
}

void PR_LoadProgs()
{
	// flush the non-C variable lookup cache
	for (int i = 0; i < GEFV_CACHESIZE; i++)
		gefvCache[i].field[0] = 0;
	CRC_Init(&pr_crc);
	progs = (dprograms_t *) COM_LoadHunkFile("progs.dat");
	if (!progs)
		Sys_Error("PR_LoadProgs: couldn't load progs.dat");
	Con_DPrintf("Programs occupy %iK.\n", com_filesize / 1024);
	for (int i = 0; i < com_filesize; i++)
		CRC_ProcessByte(&pr_crc, ((byte *) progs)[i]);
	for (int i = 0; i < (int)sizeof(*progs) / 4; i++) // byte swap the header
		((int *)progs)[i] = LittleLong(((int *)progs)[i]);
	if (progs->version != PROG_VERSION)
		Sys_Error ("progs.dat has wrong version number (%i should be %i)", progs->version, PROG_VERSION);
	if (progs->crc != PROGHEADER_CRC)
		Sys_Error ("progs.dat system vars have been modified, progdefs.h is out of date");
	pr_functions = (dfunction_t *) ((byte *) progs + progs->ofs_functions);
	pr_strings = (char *)progs + progs->ofs_strings;
	if (progs->ofs_strings + progs->numstrings >= com_filesize)
		Host_Error("progs.dat strings go past end of file\n");
	pr_numknownstrings = 0;
	pr_maxknownstrings = 0;
	pr_stringssize = progs->numstrings;
	if (pr_knownstrings)
		Z_Free(pr_knownstrings);
	pr_knownstrings = NULL;
	PR_SetEngineString(""); // initialize the strings
	pr_globaldefs = (ddef_t *) ((byte *) progs + progs->ofs_globaldefs);
	pr_fielddefs = (ddef_t *) ((byte *) progs + progs->ofs_fielddefs);
	pr_statements = (dstatement_t *)((byte *)progs + progs->ofs_statements);
	pr_global_struct = (globalvars_t *)((byte *)progs + progs->ofs_globals);
	pr_globals = (float *)pr_global_struct;
	for (int i = 0; i < progs->numstatements; i++) { // byte swap the lumps
		pr_statements[i].op = LittleShort(pr_statements[i].op);
		pr_statements[i].a = LittleShort(pr_statements[i].a);
		pr_statements[i].b = LittleShort(pr_statements[i].b);
		pr_statements[i].c = LittleShort(pr_statements[i].c);
	}
	for (int i = 0; i < progs->numfunctions; i++) {
		pr_functions[i].first_statement =
			LittleLong(pr_functions[i].first_statement);
		pr_functions[i].parm_start =
			LittleLong(pr_functions[i].parm_start);
		pr_functions[i].s_name = LittleLong(pr_functions[i].s_name);
		pr_functions[i].s_file = LittleLong(pr_functions[i].s_file);
		pr_functions[i].numparms = LittleLong(pr_functions[i].numparms);
		pr_functions[i].locals = LittleLong(pr_functions[i].locals);
	}
	for (int i = 0; i < progs->numglobaldefs; i++) {
		pr_globaldefs[i].type = LittleShort(pr_globaldefs[i].type);
		pr_globaldefs[i].ofs = LittleShort(pr_globaldefs[i].ofs);
		pr_globaldefs[i].s_name = LittleLong(pr_globaldefs[i].s_name);
	}
	for (int i = 0; i < progs->numfielddefs; i++) {
		pr_fielddefs[i].type = LittleShort(pr_fielddefs[i].type);
		if (pr_fielddefs[i].type & DEF_SAVEGLOBAL)
			Sys_Error ("PR_LoadProgs: pr_fielddefs[i].type & DEF_SAVEGLOBAL");
		pr_fielddefs[i].ofs = LittleShort(pr_fielddefs[i].ofs);
		pr_fielddefs[i].s_name = LittleLong(pr_fielddefs[i].s_name);
	}
	for (int i = 0; i < progs->numglobals; i++)
		((int *)pr_globals)[i] = LittleLong(((int *)pr_globals)[i]);
	pr_edict_size = progs->entityfields * 4 + sizeof(edict_t) - sizeof(entvars_t);
	// round off to next highest whole word address (esp for Alpha)
	// this ensures that pointers in the engine data area are always
	// properly aligned
	pr_edict_size += sizeof(void *) - 1;
	pr_edict_size &= ~(sizeof(void *) - 1);
}

void PR_Init()
{
	Cmd_AddCommand("edict", ED_PrintEdict_f);
	Cmd_AddCommand("edicts", ED_PrintEdicts);
	Cmd_AddCommand("edictcount", ED_Count);
	Cmd_AddCommand("profile", PR_Profile_f);
	Cvar_RegisterVariable(&nomonsters);
	Cvar_RegisterVariable(&gamecfg);
	Cvar_RegisterVariable(&scratch1);
	Cvar_RegisterVariable(&scratch2);
	Cvar_RegisterVariable(&scratch3);
	Cvar_RegisterVariable(&scratch4);
	Cvar_RegisterVariable(&savedgamecfg);
	Cvar_RegisterVariable(&saved1);
	Cvar_RegisterVariable(&saved2);
	Cvar_RegisterVariable(&saved3);
	Cvar_RegisterVariable(&saved4);
}

edict_t *EDICT_NUM(int n)
{
	if (n < 0 || n >= sv.max_edicts)
		Sys_Error("EDICT_NUM: bad number %i", n);
	return (edict_t *) ((byte *) sv.edicts + (n) * pr_edict_size);
}

int NUM_FOR_EDICT(edict_t *e)
{
	int b = (byte *) e - (byte *) sv.edicts;
	b = b / pr_edict_size;
	if (b < 0 || b >= sv.num_edicts)
		Sys_Error("NUM_FOR_EDICT: bad pointer");
	return b;
}

static void PR_AllocStringSlots()
{
	pr_maxknownstrings += PR_STRING_ALLOCSLOTS;
	Con_DPrintf("PR_AllocStringSlots: realloc'ing for slots\n",
			pr_maxknownstrings);
	pr_knownstrings = (char **)Z_Realloc(pr_knownstrings,
				pr_maxknownstrings * sizeof(char *));
}

int PR_SetEngineString(char *s)
{
	if (!s)
		return 0;
	if (s >= pr_strings && s <= pr_strings + pr_stringssize - 2)
		return (int)(s - pr_strings);
	int i;
	for (i = 0; i < pr_numknownstrings; i++) {
		if (pr_knownstrings[i] == s)
			return -1 - i;
	}
	if (i >= pr_maxknownstrings) // new unknown engine string
		PR_AllocStringSlots();
	pr_numknownstrings++;
	pr_knownstrings[i] = s;
	return -1 - i;
}

int PR_AllocString(int size, char **ptr)
{
	if (!size)
		return 0;
	int i;
	for (i = 0; i < pr_numknownstrings; i++)
		if (!pr_knownstrings[i])
			break;
	if (i >= pr_maxknownstrings)
		PR_AllocStringSlots();
	pr_numknownstrings++;
	pr_knownstrings[i] = (char *)Hunk_AllocName(size, "string");
	if (ptr)
		*ptr = pr_knownstrings[i];
	return -1 - i;
}
