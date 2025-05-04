// Copyright (c) 2024-2025 erysdren (it/its) GPLv3 See LICENSE for details.
// Originally MIT License

#ifndef VGATEXT_H
#define VGATEXT_H

#include "quakedef.h"

// draw rendered vga text in the specified window. returns -1 on error.
int vgatext_main(SDL_Window *window, Uint16 *screen);

#endif
