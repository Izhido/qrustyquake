// Copyright (C) 1996-1997 Id Software, Inc. GPLv3 See LICENSE for details.

#define	MAX_HANDLES 32

// File IO
int Sys_FileType (const char *path);
int Sys_FileOpenRead (const char *path, int *hndl);
int Sys_FileOpenWrite (const char *path);
void Sys_FileClose (int handle);
void Sys_FileSeek (int handle, int position);
int Sys_FileRead (int handle, void *dest, int count);
int Sys_FileWrite (int handle, const void *data, int count);
int Sys_FileTime (const char *path);
void Sys_mkdir (const char *path);

// System IO
void Sys_Error (const char *error, ...);
void Sys_Printf (const char *fmt, ...);
void Sys_Quit ();
double Sys_FloatTime ();
double Sys_DoubleTime ();
