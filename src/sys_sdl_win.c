#include "sys_sdl.c"
int Sys_FileOpenWrite (const char *path) // CyanBun96: pasta from Quakespasm
{
	int i = findhandle ();
	FILE *f = fopen(path, "wb");
	if (!f) Sys_Error ("Error opening %s: %s", path, strerror(errno));
	sys_handles[i] = f;
	return i;
}

int Sys_FileWrite (int handle, const void *data, int count) // ditto
{
	return fwrite (data, 1, count, sys_handles[handle]);
}

void Sys_mkdir(const char *path)
{
	_mkdir(path);
}

double Sys_FloatTime()
{
	static int starttime = 0;
	if (!starttime)
		starttime = clock();
	return (clock() - starttime) * 1.0 / 1024;
}

double Sys_DoubleTime() { return Sys_FloatTime(); }

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif
int Sys_FileType (const char *path)
{
        DWORD result = GetFileAttributes(path);

        if (result == INVALID_FILE_ATTRIBUTES)
                return FS_ENT_NONE;
        if (result & FILE_ATTRIBUTE_DIRECTORY)
                return FS_ENT_DIRECTORY;

        return FS_ENT_FILE;
}
