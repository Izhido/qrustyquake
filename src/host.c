// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

// host.c -- coordinates spawning and killing of local servers

#include "quakedef.h"
#include "r_local.h"

/*
A server can allways be started, even if the system started out as a client
to a remote system.
A client can NOT be started if the system started as a dedicated server.
Memory is cleared / released when a server or client begins, not when they end.
*/

quakeparms_t host_parms;
qboolean host_initialized; // true if into command execution
qboolean isDedicated;
double host_frametime;
double host_rawframetime;
float host_netinterval;
double host_time;
double realtime; // without any filtering or bounding
double oldrealtime; // last frame run
int host_framecount;
int host_hunklevel;
int minimum_memory;
client_t *host_client; // current client
jmp_buf host_abortserver;
byte *host_basepal;
byte *host_colormap;

cvar_t host_framerate = { "host_framerate", "0", false, false, 0, NULL };
cvar_t host_speeds = { "host_speeds", "0", false, false, 0, NULL };
cvar_t sys_ticrate = { "sys_ticrate", "0.05", false, false, 0, NULL };
cvar_t serverprofile = { "serverprofile", "0", false, false, 0, NULL };
cvar_t fraglimit = { "fraglimit", "0", false, true, 0, NULL };
cvar_t timelimit = { "timelimit", "0", false, true, 0, NULL };
cvar_t teamplay = { "teamplay", "0", false, true, 0, NULL };
cvar_t samelevel = { "samelevel", "0", false, false, 0, NULL };
cvar_t noexit = { "noexit", "0", false, true, 0, NULL };
cvar_t developer = { "developer", "0", false, false, 0, NULL };
cvar_t skill = { "skill", "1", false, false, 0, NULL };
cvar_t deathmatch = { "deathmatch", "0", false, false, 0, NULL };
cvar_t coop = { "coop", "0", false, false, 0, NULL };
cvar_t pausable = { "pausable", "1", false, false, 0, NULL };
cvar_t temp1 = { "temp1", "0", false, false, 0, NULL };
cvar_t host_maxfps = {"host_maxfps", "72", true, false, 0, NULL}; //johnfitz
cvar_t host_timescale = {"host_timescale", "0", CVAR_NONE}; //johnfitz
cvar_t max_edicts = {"max_edicts", "8192", false, false, 0, NULL}; //johnfitz //ericw -- changed from 2048 to 8192, removed CVAR_ ARCHIVE

extern void IN_MLookDown();
extern void Cbuf_Waited();
extern void CL_AccumulateCmd();
    
static void Max_Fps_f (cvar_t *var)
{
	if (var->value > 72 || var->value <= 0) {
		if (!host_netinterval)
			Con_Printf ("Using renderer/network isolation.\n");
		host_netinterval = 1.0/72;
	}
	else {
		if (host_netinterval)
			Con_Printf ("Disabling renderer/network isolation.\n");
		host_netinterval = 0;
		if (var->value > 72)
			Con_Printf ("host_maxfps above 72 breaks physics.\n");
	}
}

void Host_EndGame(char *message, ...)
{
	va_list argptr;
	char string[1024];
	va_start(argptr, message);
	vsprintf(string, message, argptr);
	va_end(argptr);
	Con_DPrintf("Host_EndGame: %s\n", string);
	if (sv.active)
		Host_ShutdownServer(false);
	if (cls.state == ca_dedicated) // dedicated servers exit
		Sys_Error("Host_EndGame: %s\n", string);
	cls.demonum != -1 ? CL_NextDemo() : CL_Disconnect();
	longjmp(host_abortserver, 1);
}

void Host_Error(char *error, ...)
{
	va_list argptr;
	char string[1024];
	static qboolean inerror = false;
	if (inerror)
		Sys_Error("Host_Error: recursively entered");
	inerror = true;
	SCR_EndLoadingPlaque();	// reenable screen updates
	va_start(argptr, error);
	vsprintf(string, error, argptr);
	va_end(argptr);
	Con_Printf("Host_Error: %s\n", string);
	if (sv.active)
		Host_ShutdownServer(false);
	if (cls.state == ca_dedicated) // dedicated servers exit
		Sys_Error("Host_Error: %s\n", string);
	CL_Disconnect();
	cls.demonum = -1;
	inerror = false;
	longjmp(host_abortserver, 1);
}

void Host_FindMaxClients()
{
	svs.maxclients = 1;
	int i = COM_CheckParm("-dedicated");
	if (i) {
		cls.state = ca_dedicated;
		if (i != (com_argc - 1)) {
			svs.maxclients = Q_atoi(com_argv[i + 1]);
		} else
			svs.maxclients = 8;
	} else
		cls.state = ca_disconnected;

	i = COM_CheckParm("-listen");
	if (i) {
		if (cls.state == ca_dedicated)
			Sys_Error
			    ("Only one of -dedicated or -listen can be specified");
		if (i != (com_argc - 1))
			svs.maxclients = Q_atoi(com_argv[i + 1]);
		else
			svs.maxclients = 8;
	}
	if (svs.maxclients < 1)
		svs.maxclients = 8;
	else if (svs.maxclients > MAX_SCOREBOARD)
		svs.maxclients = MAX_SCOREBOARD;
	svs.maxclientslimit = svs.maxclients;
	if (svs.maxclientslimit < 4)
		svs.maxclientslimit = 4;
	svs.clients =
	    Hunk_AllocName(svs.maxclientslimit * sizeof(client_t), "clients");
	if (svs.maxclients > 1)
		Cvar_SetValue("deathmatch", 1.0);
	else
		Cvar_SetValue("deathmatch", 0.0);
}

void Host_InitLocal()
{
	Host_InitCommands();
	Cvar_RegisterVariable(&host_framerate);
	Cvar_RegisterVariable(&host_speeds);
	Cvar_RegisterVariable(&sys_ticrate);
	Cvar_RegisterVariable(&serverprofile);
	Cvar_RegisterVariable(&fraglimit);
	Cvar_RegisterVariable(&timelimit);
	Cvar_RegisterVariable(&teamplay);
	Cvar_RegisterVariable(&samelevel);
	Cvar_RegisterVariable(&noexit);
	Cvar_RegisterVariable(&skill);
	Cvar_RegisterVariable(&developer);
	Cvar_RegisterVariable(&deathmatch);
	Cvar_RegisterVariable(&coop);
	Cvar_RegisterVariable(&pausable);
	Cvar_RegisterVariable(&temp1);
	Cvar_RegisterVariable(&host_maxfps); // johnfitz
	Cvar_SetCallback (&host_maxfps, Max_Fps_f);
	Max_Fps_f (&host_maxfps);
	Cvar_RegisterVariable(&host_timescale); // johnfitz
	Cvar_RegisterVariable(&max_edicts); //johnfitz
	Host_FindMaxClients();
	host_time = 1.0; // so a think at time 0 won't get called
}

void Host_WriteConfiguration()
{ // Writes key bindings and archived cvars to config.cfg
	// dedicated servers initialize the host but don't parse and set the
	// config.cfg cvars
	if (host_initialized & !isDedicated) {
		FILE *f = fopen(va("%s/config.cfg", com_gamedir), "w");
		if (!f) {
			Con_Printf("Couldn't write config.cfg.\n");
			return;
		}

		Key_WriteBindings(f);
		Cvar_WriteVariables(f);

		fclose(f);
	}
}

void SV_ClientPrintf(const char *fmt, ...) // Sends text across to be displayed 
{ // FIXME: make this just a stuffed echo?
	va_list argptr;
	char string[1024];
	va_start(argptr, fmt);
	vsprintf(string, fmt, argptr);
	va_end(argptr);
	MSG_WriteByte(&host_client->message, svc_print);
	MSG_WriteString(&host_client->message, string);
}

void SV_BroadcastPrintf(const char *fmt, ...)
{ // Sends text to all active clients
	va_list argptr;
	char string[1024];
	va_start(argptr, fmt);
	vsprintf(string, fmt, argptr);
	va_end(argptr);
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].active && svs.clients[i].spawned) {
			MSG_WriteByte(&svs.clients[i].message, svc_print);
			MSG_WriteString(&svs.clients[i].message, string);
		}
}

void Host_ClientCommands(char *fmt, ...)
{ // Send text over to the client to be executed
	va_list argptr;
	char string[1024];
	va_start(argptr, fmt);
	vsprintf(string, fmt, argptr);
	va_end(argptr);
	MSG_WriteByte(&host_client->message, svc_stufftext);
	MSG_WriteString(&host_client->message, string);
}


void SV_DropClient(qboolean crash)
{ // Called when the player is getting totally kicked off the host
  // if (crash = true), don't bother sending signofs
	int i;
	client_t *client;
	if (!crash) {
		// send any final messages (don't check for errors)
		if (NET_CanSendMessage(host_client->netconnection)) {
			MSG_WriteByte(&host_client->message, svc_disconnect);
			NET_SendMessage(host_client->netconnection,
					&host_client->message);
		}
		if (host_client->edict && host_client->spawned) {
			// call the prog function for removing a client
			// this will set the body to a dead frame, among other things
			int saveSelf = pr_global_struct->self;
			pr_global_struct->self =
			    EDICT_TO_PROG(host_client->edict);
			PR_ExecuteProgram(pr_global_struct->ClientDisconnect);
			pr_global_struct->self = saveSelf;
		}
		Sys_Printf("Client %s removed\n", host_client->name);
	}
	// break the net connection
	NET_Close(host_client->netconnection);
	host_client->netconnection = NULL;
	// free the client (the body stays around)
	host_client->active = false;
	host_client->name[0] = 0;
	host_client->old_frags = -999999;
	net_activeconnections--;
	// send notification to all clients
	for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++) {
		if (!client->active)
			continue;
		MSG_WriteByte(&client->message, svc_updatename);
		MSG_WriteByte(&client->message, host_client - svs.clients);
		MSG_WriteString(&client->message, "");
		MSG_WriteByte(&client->message, svc_updatefrags);
		MSG_WriteByte(&client->message, host_client - svs.clients);
		MSG_WriteShort(&client->message, 0);
		MSG_WriteByte(&client->message, svc_updatecolors);
		MSG_WriteByte(&client->message, host_client - svs.clients);
		MSG_WriteByte(&client->message, 0);
	}
}

void Host_ShutdownServer(qboolean crash)
{ // This only happens at the end of a game, not between levels
	unsigned char message[4];
	int i;
	if (!sv.active)
		return;
	sv.active = false;
	if (cls.state == ca_connected) // stop all client sounds immediately
		CL_Disconnect();
	// flush any pending messages - like the score!!!
	double start = Sys_FloatTime();
	int count;
	do {
		count = 0;
		for (i = 0, host_client = svs.clients; i < svs.maxclients;
		     i++, host_client++) {
			if (host_client->active && host_client->message.cursize) {
				if (NET_CanSendMessage
				    (host_client->netconnection)) {
					NET_SendMessage(host_client->
							netconnection,
							&host_client->message);
					SZ_Clear(&host_client->message);
				} else {
					NET_GetMessage(host_client->
						       netconnection);
					count++;
				}
			}
		}
		if ((Sys_FloatTime() - start) > 3.0)
			break;
	}
	while (count);
	// make sure all the clients know we're disconnecting
	sizebuf_t buf;
	buf.data = message;
	buf.maxsize = 4;
	buf.cursize = 0;
	MSG_WriteByte(&buf, svc_disconnect);
	count = NET_SendToAll(&buf, 5);
	if (count)
		Con_Printf
		    ("Host_ShutdownServer: NET_SendToAll failed for %u clients\n",
		     count);
	for (i = 0, host_client = svs.clients; i < svs.maxclients;
	     i++, host_client++)
		if (host_client->active)
			SV_DropClient(crash);
	memset(&sv, 0, sizeof(sv)); // clear structures
	memset(svs.clients, 0, svs.maxclientslimit * sizeof(client_t));
}

void Host_ClearMemory()
{ // This clears all the memory used by both the client and server, but does
  // not reinitialize anything.
	Con_DPrintf("Clearing memory\n");
	D_FlushCaches();
	Mod_ClearAll();
	if (host_hunklevel)
		Hunk_FreeToLowMark(host_hunklevel);

	cls.signon = 0;
	memset(&sv, 0, sizeof(sv));
	memset(&cl, 0, sizeof(cl));
}

double Host_GetFrameInterval ()
{
	if ((host_maxfps.value || cls.state==ca_disconnected) && !cls.timedemo){
		float maxfps;
		if (cls.state == ca_disconnected) {
			maxfps = 60;//TODO vid.refreshrate ? vid.refreshrate : 60.f;
			if (host_maxfps.value)
				maxfps = q_min (maxfps, host_maxfps.value);
			maxfps = CLAMP (10.f, maxfps, 10000000.f);
		}
		else {
			maxfps = CLAMP (10.f, host_maxfps.value, 10000000.f);
		}
		return 1.0 / maxfps;
	}
	return 0.0;
}

static void Host_AdvanceTime (double dt)
{
	realtime += dt;
	host_frametime = host_rawframetime = realtime - oldrealtime;
	oldrealtime = realtime;
	//johnfitz -- host_timescale is more intuitive than host_framerate
	if (host_timescale.value > 0)
		host_frametime *= host_timescale.value;
	//johnfitz
	else if (host_framerate.value > 0)
		host_frametime = host_framerate.value;
	else if (host_maxfps.value)// don't allow really long or short frames
		host_frametime = CLAMP (0.0001, host_frametime, 0.1); //johnfitz -- use CLAMP
}

void Host_ServerFrame()
{
	pr_global_struct->frametime = host_frametime; // run the world state
	SV_ClearDatagram(); // set the time and clear the general datagram
	SV_CheckForNewClients(); // check for new clients
	SV_RunClients(); // read client messages
	// move things around and think
	// always pause in single player if in console or menus
	if (!sv.paused && (svs.maxclients > 1 || key_dest == key_game))
		SV_Physics();
	SV_SendClientMessages(); // send all messages to the clients
}

static void Host_PrintTimes (const double times[], const char *names[], int count, qboolean showtotal)
{
	char line[1024];
	double total = 0.0;
	int i, worst;
	for (i = 0, worst = -1; i < count; i++) {
		if (worst == -1 || times[i] > times[worst])
			worst = i;
		total += times[i];
	}
	if (showtotal)
		q_snprintf (line, sizeof (line), "%5.2f tot | ", total * 1000.0);
	else
		line[0] = '\0';
	for (i = 0; i < count; i++) {
		char entry[256];
		q_snprintf (entry, sizeof (entry), "%5.2f %s", times[i] * 1000.0, names[i]);
		//if (i == worst)
		//    COM_TintString (entry, entry, sizeof (entry));
		if (i != 0)
			q_strlcat (line, " | ", sizeof (line));
		q_strlcat (line, entry, sizeof (line));
	}
	Con_Printf ("%s\n", line);
}

void _Host_Frame(float time)
{ // Runs all active servers
	static double accumtime = 0;
	static double time1, time2, time3;
	qboolean ranserver = false;
	if (setjmp(host_abortserver))
		return;	// something bad happened, or the server disconnected
	rand(); // keep the random time dependent
	accumtime += host_netinterval?CLAMP(0.0, (double)time, 0.2):0.0; // for renderer/server isolation
	Host_AdvanceTime (time);
	Sys_SendKeyEvents(); // get new key events
	Cbuf_Execute(); // process console commands
	NET_Poll();
	CL_AccumulateCmd();
	// Run the server+networking (client->server->client), at a different 
	// rate from everything else
	if (accumtime >= host_netinterval) {
		float realframetime = host_frametime;
		if (host_netinterval) {
			host_frametime = q_max(accumtime, (double)host_netinterval);
			accumtime -= host_frametime;
			if (host_timescale.value > 0)
				host_frametime *= host_timescale.value;
			else if (host_framerate.value)
				host_frametime = host_framerate.value;
		}
		else
			accumtime -= host_netinterval;
		CL_SendCmd ();
		if (sv.active)
			Host_ServerFrame ();
		host_frametime = realframetime;
		Cbuf_Waited();
		ranserver = true;
	}
	if (cls.state == ca_connected) // fetch results from server
		CL_ReadFromServer();
	if (host_speeds.value) // update video
		time1 = Sys_FloatTime();
	SCR_UpdateScreen();
	if (host_speeds.value)
		time2 = Sys_FloatTime();
	if (cls.signon == SIGNONS) { // update audio
		S_Update(r_origin, vpn, vright, vup);
		CL_DecayLights();
	} else
		S_Update(vec3_origin, vec3_origin, vec3_origin, vec3_origin);
	if (host_speeds.value)
	{
		static double pass[3] = {0.0, 0.0, 0.0};
		static double elapsed = 0.0;
		static int numframes = 0;
		static int numserverframes = 0;
		time1 = time2 - time1;
		time2 = time3 - time2;
		time3 = Sys_DoubleTime () - time3;
		if (ranserver || host_speeds.value < 0.f) {
			pass[0] += time1;
			numserverframes++;
		}
		numframes++;
		pass[1] += time2;
		pass[2] += time3;
		elapsed += time;
		if (elapsed >= host_speeds.value * 0.375) {
			const char *names[3] = {"server", "gfx", "snd"};
			pass[0] /= q_max (numserverframes, 1);
			pass[1] /= numframes;
			pass[2] /= numframes;
			Host_PrintTimes (pass, names, countof (pass), host_speeds.value < 0.f);
			pass[0] = pass[1] = pass[2] = elapsed = 0.0;
			numframes = numserverframes = 0;
		}
	}
	host_framecount++;
}

void Host_Frame(float time)
{
	static double timetotal;
	static int timecount;
	if (!serverprofile.value) {
		_Host_Frame(time);
		return;
	}
	double time1 = Sys_FloatTime();
	_Host_Frame(time);
	double time2 = Sys_FloatTime();
	timetotal += time2 - time1;
	timecount++;
	if (timecount < 1000)
		return;
	int m = timetotal * 1000 / timecount;
	timecount = 0;
	timetotal = 0;
	int c = 0;
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].active)
			c++;
	Con_Printf("serverprofile: %2i clients %2i msec\n", c, m);
}

void Host_Init()
{
	com_argc = host_parms.argc;
	com_argv = host_parms.argv;
	Memory_Init(host_parms.membase, host_parms.memsize);
	Cbuf_Init();
	Cmd_Init();
	V_Init();
	Chase_Init();
	COM_Init();
	COM_InitFilesystem();
	Host_InitLocal();
	W_LoadWadFile();
	Key_Init();
	Con_Init();
	M_Init();
	PR_Init();
	Mod_Init();
	NET_Init();
	SV_Init();
	Con_Printf("Exe: " __TIME__ " " __DATE__ "\n");
	Con_Printf("%4.1f megabyte heap\n", host_parms.memsize / (1024 * 1024.0));
	R_InitTextures(); // needed even for dedicated servers
	if (cls.state != ca_dedicated) {
		host_basepal = (byte *) COM_LoadHunkFile("gfx/palette.lmp", NULL);
		if (!host_basepal)
			Sys_Error("Couldn't load gfx/palette.lmp");
		host_colormap = (byte *) COM_LoadHunkFile("gfx/colormap.lmp", NULL);
		if (!host_colormap)
			Sys_Error("Couldn't load gfx/colormap.lmp");
		IN_Init();
		VID_Init(host_basepal);
		Draw_Init();
		SCR_Init();
		R_Init();
		S_Init();
		Sbar_Init();
		CL_Init();
	}
	Cbuf_InsertText("exec quake.rc\n");
	IN_MLookDown();
	Hunk_AllocName(0, "-HOST_HUNKLEVEL-");
	host_hunklevel = Hunk_LowMark();
	host_initialized = true;
	Sys_Printf("========Quake Initialized=========\n");
}

// FIXME: this is a callback from Sys_Quit and Sys_Error. It would be better
// to run quit through here before the final handoff to the sys code.
void Host_Shutdown()
{
	static qboolean isdown = false;
	if (isdown) {
		printf("recursive shutdown\n");
		return;
	}
	isdown = true;
	// keep Con_Printf from trying to update the screen
	scr_disabled_for_loading = true;
	Host_WriteConfiguration();
	NET_Shutdown();
	S_Shutdown();
	if (cls.state != ca_dedicated) {
		IN_Shutdown();	// input is only initialized in Host_Init if we're not dedicated -- kristian
		VID_Shutdown();
	}
}
