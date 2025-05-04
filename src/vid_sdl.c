#include "quakedef.h"
#include "d_local.h"

#define NUM_OLDMODES 9
#define MAX_MODE_LIST 30
#define VID_ROW_SIZE 3
#define MAX_COLUMN_SIZE 5
#define MODE_AREA_HEIGHT (MAX_COLUMN_SIZE + 6)
#define MAX_MODEDESCS (MAX_COLUMN_SIZE * 3)
#define NO_MODE -1

unsigned int oldmodes[NUM_OLDMODES * 2] = {
	320, 240, 640, 480, 800, 600,
	320, 200, 320, 240, 640, 350,
	640, 400, 640, 480, 800, 600
};

char modelist[NUM_OLDMODES][8];	// "320x240" etc. for menus
SDL_Window *window;
SDL_Surface *windowSurface;
SDL_Renderer *renderer;
SDL_Surface *argbbuffer;
SDL_Texture *texture;
SDL_Rect blitRect;
SDL_Rect destRect;
SDL_Surface *scaleBuffer;
SDL_Surface *screen;
unsigned int force_old_render = 0;
unsigned int SDLWindowFlags;
unsigned int uiscale;
unsigned int vimmode;
unsigned int VGA_width;
unsigned int VGA_height;
unsigned int VGA_rowbytes;
unsigned int VGA_bufferrowbytes;
unsigned char *VGA_pagebase;
int vid_line;
int lockcount;
int vid_modenum;
int vid_testingmode;
int vid_realmode;
int vid_default;
double vid_testendtime;
unsigned char vid_curpal[256 * 3]; // save for mode changes
qboolean vid_initialized;
qboolean palette_changed;
int VID_highhunkmark;
viddef_t vid; // global video state

cvar_t vid_mode = { "vid_mode", "0", 0, 0, 0, 0 };
cvar_t _vid_default_mode_win = { "_vid_default_mode_win", "3", 1, 0, 0, 0 };
cvar_t scr_uiscale = { "scr_uiscale", "1", 1, 0, 0, 0 };
cvar_t sensitivityyscale = { "sensitivityyscale", "1.0", 1, 0, 0, 0 };
cvar_t _windowed_mouse = { "_windowed_mouse", "0", 1, 0, 0, 0 };
cvar_t newoptions = { "newoptions", "1", 1, 0, 0, 0 };
cvar_t aspectr = { "aspectr", "0", 1, 0, 0, 0 }; // 0 - auto
cvar_t realwidth = { "realwidth", "0", 0, 0, 0, 0 }; // 0 - auto
cvar_t realheight = { "realheight", "0", 0, 0, 0, 0 }; // 0 - auto

void VID_CalcScreenDimensions(cvar_t *cvar);
void VID_AllocBuffers();

int VID_GetDefaultMode()
{
	// CyanBun96: _vid_default_mode_win gets read from config.cfg only after
	// video is initialized. To avoid creating a window and textures just to
	// destroy them right away and only then replace them with valid ones,
	// this function reads the default mode independently of the cvar status
	char line[256];
	int vid_default_mode = -1;
	FILE *file = fopen("id1/config.cfg", "r");
	if (!file) {
		printf("Failed to open id1/config.cfg");
		return -2;
	}
	while (fgets(line, sizeof(line), file)) {
		char *key = "_vid_default_mode_win";
		char *found = strstr(line, key);
		if (found) {
			char *start = strchr(found, '"');
			if (start) {
				char *end = strchr(start + 1, '"');
				if (end) {
					*end = '\0';
					vid_default_mode = atoi(start + 1);
					break;
				}
			}
		}
	}
	fclose(file);
	if (vid_default_mode != -1)
		printf("_vid_default_mode_win: %d\n", vid_default_mode);
	else
		printf("_vid_default_mode_win not found in id1/config.cfg\n");
	return vid_default_mode;
}

int VID_DetermineMode()
{
	int win = !(SDLWindowFlags & (SDL_WINDOW_FULLSCREEN
				      | SDL_WINDOW_FULLSCREEN_DESKTOP));
	for (int i = 0; i < NUM_OLDMODES; ++i) {
		if (i > 2 ? !win : win && vid.width == oldmodes[i * 2]
		    && vid.height == oldmodes[i * 2 + 1])
			return i;
	}
	return -1;
}

void VID_SetPalette(unsigned char *palette)
{
	SDL_Color colors[256];
	palette_changed = true;
	if (palette != vid_curpal)
		memcpy(vid_curpal, palette, sizeof(vid_curpal));
	for (int i = 0; i < 256; ++i) {
		colors[i].r = *palette++;
		colors[i].g = *palette++;
		colors[i].b = *palette++;
	}
	SDL_SetPaletteColors(screen->format->palette, colors, 0, 256);
}

void VID_Init(unsigned char *palette)
{
	int pnum;
	int flags;
	int winmode;
	int defmode = -1;
	char caption[50];
	Cvar_RegisterVariable(&_windowed_mouse);
	Cvar_RegisterVariable(&_vid_default_mode_win);
	Cvar_RegisterVariable(&vid_mode);
	Cvar_RegisterVariable(&scr_uiscale);
	Cvar_RegisterVariable(&sensitivityyscale);
	Cvar_RegisterVariable(&newoptions);
	Cvar_RegisterVariable(&aspectr);
	Cvar_RegisterVariable(&realwidth);
	Cvar_RegisterVariable(&realheight);
	Cvar_SetCallback(&aspectr, VID_CalcScreenDimensions);
	Cvar_SetCallback(&realwidth, VID_CalcScreenDimensions);
	Cvar_SetCallback(&realheight, VID_CalcScreenDimensions);
	// Set up display mode (width and height)
	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.width = 320;
	vid.height = 240;
	winmode = 0;
	if (!(COM_CheckParm("-width") || COM_CheckParm("-height")
	   || COM_CheckParm("-window") || COM_CheckParm("-fullscreen")
	   || COM_CheckParm("-winsize"))) {
		defmode = VID_GetDefaultMode();
		if (defmode >= 0 && defmode < NUM_OLDMODES) {
			vid.width = oldmodes[defmode * 2];
			vid.height = oldmodes[defmode * 2 + 1];
			winmode = defmode < 3;
		}
	}
	if ((pnum = COM_CheckParm("-winsize"))) {
		if (pnum >= com_argc - 2)
			Sys_Error("VID: -winsize <width> <height>\n");
		vid.width = Q_atoi(com_argv[pnum + 1]);
		vid.height = Q_atoi(com_argv[pnum + 2]);
		if (!vid.width || !vid.height)
			Sys_Error("VID: Bad window width/height\n");
	}
	if ((pnum = COM_CheckParm("-width"))) {
		if (pnum >= com_argc - 1)
			Sys_Error("VID: -width <width>\n");
		vid.width = Q_atoi(com_argv[pnum + 1]);
		if (!vid.width)
			Sys_Error("VID: Bad window width\n");
	}
	if ((pnum = COM_CheckParm("-height"))) {
		if (pnum >= com_argc - 1)
			Sys_Error("VID: -height <height>\n");
		vid.height = Q_atoi(com_argv[pnum + 1]);
		if (!vid.height)
			Sys_Error("VID: Bad window height\n");
	}
	// In windowed mode, the window can be resized at runtime
	flags = SDL_WINDOW_RESIZABLE;
	// Set the highdpi flag - this makes a big difference on Macs with
	// retina displays, especially when using small window sizes.
	flags |= SDL_WINDOW_ALLOW_HIGHDPI;
	// CyanBun96: what most modern games call "Fullscreen borderless
	// can be achieved by passing -fullscreen_desktop and -borderless
	// -fullscreen will try to change the display resolution
	if (COM_CheckParm("-fullscreen"))
		flags |= SDL_WINDOW_FULLSCREEN;
	else if (COM_CheckParm("-fullscreen_desktop"))
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	else if (COM_CheckParm("-window"))
		flags &= ~SDL_WINDOW_FULLSCREEN;
	else if (winmode == 0)
		flags |= SDL_WINDOW_FULLSCREEN;
	else if (winmode == 1)
		flags &= ~SDL_WINDOW_FULLSCREEN;
	if (COM_CheckParm("-borderless"))
		flags |= SDL_WINDOW_BORDERLESS;
	if (COM_CheckParm("-forceoldrender"))
		force_old_render = 1;
	if (COM_CheckParm("-vimmode"))
		vimmode = 1;
	if (vid.width > 1280 || vid.height > 1024)
		Sys_Printf("vanilla maximum resolution is 1280x1024\n");
	aspectr.value = 1.333333;
	realwidth.value = vid.width;
	realheight.value = (int)(vid.width / aspectr.value + 0.5);
	window = SDL_CreateWindow(NULL, SDL_WINDOWPOS_UNDEFINED,
				  SDL_WINDOWPOS_UNDEFINED,
				  realwidth.value, realheight.value, flags);
	screen = SDL_CreateRGBSurfaceWithFormat(0, vid.width, vid.height,
		8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	renderer = SDL_CreateRenderer(window, -1, 0);
	if (!force_old_render) {
		argbbuffer = SDL_CreateRGBSurfaceWithFormatFrom(NULL, vid.width,
			vid.height, 0, 0, SDL_PIXELFORMAT_ARGB8888);
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, vid.width, vid.height);
	} else {
		scaleBuffer = SDL_CreateRGBSurfaceWithFormat(0, vid.width,
			vid.height, 8, SDL_GetWindowPixelFormat (window));
	}
	windowSurface = SDL_GetWindowSurface(window);
	sprintf(caption, "QrustyQuake - Version %4.2f", VERSION);
	SDL_SetWindowTitle(window, (const char *)&caption);
	SDLWindowFlags = SDL_GetWindowFlags(window);
	VGA_width = vid.conwidth = vid.width;
	VGA_height = vid.conheight = vid.height;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
	VGA_pagebase = vid.buffer = screen->pixels;
	VGA_rowbytes = vid.rowbytes = screen->pitch;
	vid.conbuffer = vid.buffer;
	vid.conrowbytes = vid.rowbytes;
	vid.direct = (pixel_t *) screen->pixels;
	VID_AllocBuffers(); // allocate z buffer, surface cache and the fbuffer
	SDL_ShowCursor(0); // initialize the mouse
	vid_initialized = true;
	if (defmode >= 0)
		vid_modenum = defmode;
	else
		vid_modenum = VID_DetermineMode();
	if (vid_modenum < 0)
		Con_Printf("WARNING: non-standard video mode\n");
	else
		Con_Printf("Detected video mode %d\n", vid_modenum);
	realwidth.value = 0;
	VID_CalcScreenDimensions(0);
}

void VID_Shutdown()
{
	if (vid_initialized) {
		if (screen != NULL && lockcount > 0)
			SDL_UnlockSurface(screen);
		if (!force_old_render)
			SDL_UnlockTexture(texture);
		vid_initialized = 0;
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}
}

void VID_CalcScreenDimensions(cvar_t *cvar)
{
	uiscale = (vid.width / 320);
	if (uiscale * 200 > vid.height)
		uiscale = 1; // For weird resolutions like 800x200
	Cvar_SetValue("scr_uiscale", uiscale);
	// Scaling code, courtesy of ChatGPT
	// Original, pre-scale screen size
	blitRect.x = 0;
	blitRect.y = 0;
	blitRect.w = vid.width;
	blitRect.h = vid.height;
	// Get window dimensions
	int winW, winH;
	if (realwidth.value == 0 || realheight.value == 0) {
		winW = windowSurface->w;
		winH = windowSurface->h;
	}
	else {
		winW = realwidth.value;
		winH = realheight.value;
	}
	// Get scaleBuffer dimensions
	int bufW = vid.width;
	int bufH = vid.height;
	float bufAspect;
	if (aspectr.value == 0) {
		bufAspect = (float)bufW / bufH;
	}
	else
		bufAspect = aspectr.value;
	Cvar_SetValue("aspectr", bufAspect);
	// Calculate scaled dimensions
	int destW, destH;
	if ((float)winW / winH > bufAspect) {
		// Window is wider than buffer, black bars on sides
		destH = winH;
		destW = (int)(winH * bufAspect + 0.5);
	} else {
		// Window is taller than buffer, black bars on top/bottom
		destW = winW;
		destH = (int)(winW / bufAspect + 0.5);
	}
	// Center the destination rectangle
	destRect.x = (winW - destW) / 2;
	destRect.y = (winH - destH) / 2;
	destRect.w = destW;
	destRect.h = destH;
}

void VID_Update()
{
	// Machines without a proper GPU will try to simulate one with software,
	// adding a lot of overhead. In my tests software rendering accomplished
	// the same result with almost a 200% performance increase.
	if (force_old_render) {	// pure software rendering
		SDL_UpperBlit(screen, NULL, scaleBuffer, NULL);
		SDL_UpperBlitScaled(scaleBuffer, &blitRect, windowSurface,
				    &destRect);
		SDL_UpdateWindowSurface(window);
	} else { // hardware-accelerated rendering
		SDL_LockTexture(texture, &blitRect, &argbbuffer->pixels,
				&argbbuffer->pitch);
		SDL_LowerBlit(screen, &blitRect, argbbuffer, &blitRect);
		SDL_UnlockTexture(texture);
		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, NULL, &destRect);
		SDL_RenderPresent(renderer);
	}
	if (uiscale != scr_uiscale.value
	    && vid.width / 320 >= scr_uiscale.value && scr_uiscale.value > 0) {
		uiscale = scr_uiscale.value;
		vid.recalc_refdef = 1;
	}
	if (vid_testingmode) {
		if (realtime >= vid_testendtime) {
			VID_SetMode(vid_realmode, 0, 0, 0, vid_curpal);
			vid_testingmode = 0;
		}
	} else if ((int)vid_mode.value != vid_realmode) {
		VID_SetMode((int)vid_mode.value, 0, 0, 0, vid_curpal);
		Cvar_SetValue("vid_mode", (float)vid_modenum);
		vid_realmode = vid_modenum;
	}
}

char *VID_GetModeDescription(int mode)
{
	static char pinfo[40];
	if ((mode < 0) || (mode >= NUM_OLDMODES))
		sprintf(pinfo, "Custom fullscreen");
	else if (mode >= 3)
		sprintf(pinfo, "%s fullscreen", modelist[mode]);
	else
		sprintf(pinfo, "%s windowed", modelist[mode]);
	return pinfo;
}

void VID_AllocBuffers()
{
	// allocate z buffer and surface cache
	int chunk = vid.width * vid.height * sizeof(*d_pzbuffer);
	int cachesize = D_SurfaceCacheForRes(vid.width, vid.height);
	chunk += cachesize;
	if (litwater_base) {
		D_FlushCaches();
		Hunk_FreeToHighMark(lwmark);
		litwater_base = NULL;
	}
	if (d_pzbuffer) {
		D_FlushCaches();
		Hunk_FreeToHighMark(VID_highhunkmark);
		d_pzbuffer = NULL;
	}
	VID_highhunkmark = Hunk_HighMark();
	d_pzbuffer = Hunk_HighAllocName(chunk, "video");
	if (d_pzbuffer == NULL)
		Sys_Error("Not enough memory for video mode\n");
	unsigned char *cache = (byte *) d_pzbuffer
	    + vid.width * vid.height * sizeof(*d_pzbuffer);
	D_InitCaches(cache, cachesize);
}

void VID_SetMode(int modenum, int customw, int customh, int customwinmode,
		 unsigned char *palette)
{
	if (modenum == vid_modenum)
		return;
	if (customw && customh) {
		vid.width = customw;
		vid.height = customh;
		vid_modenum = -1;
	} else {
		vid.width = oldmodes[modenum * 2];
		vid.height = oldmodes[modenum * 2 + 1];
		vid_modenum = modenum;
	}
	// CyanBun96: why do surfaces get freed but textures get destroyed :(
	SDL_FreeSurface(screen);
	screen = SDL_CreateRGBSurfaceWithFormat(0, vid.width, vid.height, 8,
		SDL_PIXELFORMAT_INDEX8);
	if (!force_old_render) {
		SDL_FreeSurface(argbbuffer);
		argbbuffer = SDL_CreateRGBSurfaceWithFormatFrom(NULL, vid.width,
			vid.height, 0, 0, SDL_PIXELFORMAT_ARGB8888);
		SDL_DestroyTexture(texture);
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, vid.width, vid.height);
	} else {
		SDL_FreeSurface(scaleBuffer);
		scaleBuffer = SDL_CreateRGBSurfaceWithFormat(0, vid.width,
			vid.height, 8, SDL_GetWindowPixelFormat (window));
	}
	VGA_width = vid.conwidth = vid.width;
	VGA_height = vid.conheight = vid.height;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
	VGA_pagebase = vid.buffer = screen->pixels;
	VGA_rowbytes = vid.rowbytes = screen->pitch;
	vid.conbuffer = vid.buffer;
	vid.conrowbytes = vid.rowbytes;
	vid.direct = (pixel_t *) screen->pixels;
	VID_AllocBuffers();
	vid.recalc_refdef = 1;
	VID_SetPalette(palette);
	if (!customw || !customh) {
		if (modenum <= 2) {
			SDL_SetWindowFullscreen(window, 0);
			SDL_SetWindowSize(window, realwidth.value, realheight.value);
			SDL_SetWindowPosition(window, SDL_WINDOWPOS_UNDEFINED,
				SDL_WINDOWPOS_UNDEFINED);
		} else {
			SDL_SetWindowFullscreen(window,
				SDL_WINDOW_FULLSCREEN_DESKTOP);
		}
	} else {
		if (customwinmode == 0) {
			SDL_SetWindowFullscreen(window, 0);
			SDL_SetWindowSize(window,
					realwidth.value, realheight.value);
			SDL_SetWindowPosition(window, SDL_WINDOWPOS_UNDEFINED,
				SDL_WINDOWPOS_UNDEFINED);
		} else if (customwinmode == 1) {
			SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
		} else {
			SDL_SetWindowFullscreen(window,
				SDL_WINDOW_FULLSCREEN_DESKTOP);
		}
	}
	realwidth.value = 0;
	VID_CalcScreenDimensions(0);
	Cvar_SetValue("vid_mode", (float)vid_modenum);
}
