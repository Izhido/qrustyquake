// Copyright (C) 1996-2001 Id Software, Inc.
// Copyright (C) 2002-2005 John Fitzgibbons and others
// Copyright (C) 2007-2008 Kristian Duske
// GPLv3 See LICENSE for details.

int Loop_Init ();
void Loop_Listen (qboolean state);
void Loop_SearchForHosts (qboolean xmit);
qsocket_t *Loop_Connect (const char *host);
qsocket_t *Loop_CheckNewConnections ();
int Loop_GetMessage (qsocket_t *sock);
int Loop_SendMessage (qsocket_t *sock, sizebuf_t *data);
int Loop_SendUnreliableMessage (qsocket_t *sock, sizebuf_t *data);
qboolean Loop_CanSendMessage (qsocket_t *sock);
qboolean Loop_CanSendUnreliableMessage (qsocket_t *sock);
void Loop_Close (qsocket_t *sock);
void Loop_Shutdown ();
