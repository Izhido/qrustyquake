#include "quakedef.h"

FILE *sys_handles[MAX_HANDLES];
extern double Host_GetFrameInterval();

void Sys_Printf(const char *fmt, ...)
{
	va_list argptr;
	char text[1024];
	va_start(argptr, fmt);
	vsprintf(text, fmt, argptr);
	va_end(argptr);
	fprintf(stderr, "%s", text);
}

void Sys_Quit()
{
	Host_Shutdown();
        Uint16 *screen; // erysdren (it/its)
        if (registered.value)
                screen = (Uint16 *)COM_LoadHunkFile("end2.bin", 0);
        else
                screen = (Uint16 *)COM_LoadHunkFile("end1.bin", 0);
        if (screen)
                vgatext_main(window, screen);
	exit(0);
}

void Sys_Error(const char *error, ...)
{
	va_list argptr;
	char string[1024];
	va_start(argptr, error);
	vsprintf(string, error, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", string);
#ifdef DEBUG
	__asm__("int3");
#endif
	Host_Shutdown();
	exit(1);
}

int findhandle()
{
	for (int i = 1; i < MAX_HANDLES; i++)
		if (!sys_handles[i])
			return i;
	Sys_Error("out of handles");
	return -1;
}

static int Qfilelength(FILE *f)
{
	int pos = ftell(f);
	fseek(f, 0, SEEK_END);
	int end = ftell(f);
	fseek(f, pos, SEEK_SET);
	return end;
}

static double Sys_WaitUntil (double endtime)
{
	static double estimate = 1e-3;
	static double mean = 1e-3;
	static double m2 = 0.0;
	static double count = 1.0;

	double now = Sys_DoubleTime ();
	double before, observed, delta, stddev;

	endtime -= 1e-6; // allow finishing 1 microsecond earlier than requested

	while (now + estimate < endtime)
	{
		before = now;
		SDL_Delay (1);
		now = Sys_DoubleTime ();

		// Determine Sleep(1) mean duration & variance using Welford's algorithm
		// https://blog.bearcats.nl/accurate-sleep-function/
		if (count < 1e6) // skip this if we already have more than enough samples
		{
			++count;
			observed = now - before;
			delta = observed - mean;
			mean += delta / count;
			m2 += delta * (observed - mean);
			stddev = sqrt (m2 / (count - 1.0));
			estimate = mean + 1.5 * stddev;

			// Previous frame-limiting code assumed a duration of 2 msec.
			// We don't want to burn more cycles in order to be more accurate
			// in case the actual duration is higher.
			estimate = q_min (estimate, 2e-3);
		}
	}

	while (now < endtime)
	{
#ifdef USE_SSE2
		_mm_pause (); _mm_pause (); _mm_pause (); _mm_pause ();
		_mm_pause (); _mm_pause (); _mm_pause (); _mm_pause ();
		_mm_pause (); _mm_pause (); _mm_pause (); _mm_pause ();
		_mm_pause (); _mm_pause (); _mm_pause (); _mm_pause ();
#endif
		now = Sys_DoubleTime ();
	}

	return now;
}

static double Sys_Throttle (double oldtime)
{
	return Sys_WaitUntil (oldtime + Host_GetFrameInterval ());
}

int Sys_FileOpenRead(const char *path, int *hndl)
{
	int i = findhandle();
	FILE *f = fopen(path, "rb");
	if (!f) {
		*hndl = -1;
		return -1;
	}
	sys_handles[i] = f;
	*hndl = i;
	return Qfilelength(f);
}


void Sys_FileClose(int handle)
{
	fclose(sys_handles[handle]);
	sys_handles[handle] = NULL;
}

void Sys_FileSeek(int handle, int position)
{
	fseek(sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead(int handle, void *dst, int count)
{
	return fread(dst, 1, count, sys_handles[handle]);
}

int Sys_FileTime(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f) {
		fclose(f);
		return 1;
	}
	return -1;
}

#ifndef _WIN32
int Sys_FileType (const char *path)
{
        /*
        if (access(path, R_OK) == -1)
                return 0;
        */
        struct stat     st;

        if (stat(path, &st) != 0)
                return FS_ENT_NONE;
        if (S_ISDIR(st.st_mode))
                return FS_ENT_DIRECTORY;
        if (S_ISREG(st.st_mode))
                return FS_ENT_FILE;

        return FS_ENT_NONE;
}

int Sys_FileOpenWrite(const char *path)
{
	int i = findhandle();
	FILE *f = fopen(path, "wb");
	if (!f)
		Sys_Error("Error opening %s: %s", path, strerror(errno));
	sys_handles[i] = f;
	return i;
}

int Sys_FileWrite(int handle, const void *src, int count)
{
	return fwrite(src, 1, count, sys_handles[handle]);
}

double Sys_FloatTime()
{ // CyanBun96: TODO move this to DoubleTime and remove FloatTime
	struct timeval tp;
	struct timezone tzp;
	static int secbase;
	gettimeofday(&tp, &tzp);
	if (!secbase) {
		secbase = tp.tv_sec;
		return tp.tv_usec / 1000000.0;
	}
	return (tp.tv_sec - secbase) + tp.tv_usec / 1000000.0;
}

double Sys_DoubleTime() { return Sys_FloatTime(); }

void Sys_mkdir(const char *path)
{
	mkdir(path, 0777);
}
#endif

int main(int c, char **v)
{
	if (SDL_Init(SDL_INIT_EVERYTHING) < 0)
		Sys_Error("SDL_Init failed: %s", SDL_GetError());
	quakeparms_t parms;
	host_parms.argc = c;
	host_parms.argv = v;
	COM_InitArgv(host_parms.argc, host_parms.argv);
	host_parms.memsize = DEFAULT_MEMORY;
	if (COM_CheckParm("-heapsize")) {
		int t = COM_CheckParm("-heapsize") + 1;
		if (t < c)
			host_parms.memsize = Q_atoi(v[t]) * 1024;
	}
	host_parms.membase = malloc(host_parms.memsize);
	host_parms.basedir = ".";
	Host_Init();
	double oldtime = Sys_FloatTime() - 0.1;
	while (1) {
		double newtime = Sys_Throttle(oldtime);
		double deltatime = newtime - oldtime;
		if (cls.state == ca_dedicated)
			deltatime = sys_ticrate.value;
		if (deltatime > sys_ticrate.value * 2)
			oldtime = newtime;
		else
			oldtime += deltatime;
		Host_Frame(deltatime);
	}
}
