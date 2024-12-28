#include "quakedef.h"

qboolean mouse_avail;
float mouse_x;
float mouse_y;
int mouse_oldbuttonstate;

void Sys_SendKeyEvents(void)
{
	SDL_Event event;
	int sym, state;
	int modstate;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			sym = event.key.keysym.sym;
			state = event.key.state;
			modstate = SDL_GetModState();
			switch (sym) {
			case SDLK_DELETE:
				sym = K_DEL;
				break;
			case SDLK_BACKSPACE:
				sym = K_BACKSPACE;
				break;
			case SDLK_F1:
				sym = K_F1;
				break;
			case SDLK_F2:
				sym = K_F2;
				break;
			case SDLK_F3:
				sym = K_F3;
				break;
			case SDLK_F4:
				sym = K_F4;
				break;
			case SDLK_F5:
				sym = K_F5;
				break;
			case SDLK_F6:
				sym = K_F6;
				break;
			case SDLK_F7:
				sym = K_F7;
				break;
			case SDLK_F8:
				sym = K_F8;
				break;
			case SDLK_F9:
				sym = K_F9;
				break;
			case SDLK_F10:
				sym = K_F10;
				break;
			case SDLK_F11:
				sym = K_F11;
				break;
			case SDLK_F12:
				sym = K_F12;
				break;
				//case SDLK_BREAK:
			case SDLK_PAUSE:
				sym = K_PAUSE;
				break;

				// CyanBun96: vim-like keybinds that work in menus
			case SDLK_h:
				sym = vimmode ? K_LEFTARROW : 'h';
				break;
			case SDLK_j:
				sym = vimmode ? K_DOWNARROW : 'j';
				break;
			case SDLK_k:
				sym = vimmode ? K_UPARROW : 'k';
				break;
			case SDLK_l:
				sym = vimmode ? K_RIGHTARROW : 'l';
				break;

			case SDLK_UP:
				sym = K_UPARROW;
				break;
			case SDLK_DOWN:
				sym = K_DOWNARROW;
				break;
			case SDLK_RIGHT:
				sym = K_RIGHTARROW;
				break;
			case SDLK_LEFT:
				sym = K_LEFTARROW;
				break;
			case SDLK_INSERT:
				sym = K_INS;
				break;
			case SDLK_HOME:
				sym = K_HOME;
				break;
			case SDLK_END:
				sym = K_END;
				break;
			case SDLK_PAGEUP:
				sym = K_PGUP;
				break;
			case SDLK_PAGEDOWN:
				sym = K_PGDN;
				break;
			case SDLK_RSHIFT:
			case SDLK_LSHIFT:
				sym = K_SHIFT;
				break;
			case SDLK_RCTRL:
			case SDLK_LCTRL:
				sym = K_CTRL;
				break;
			case SDLK_RALT:
			case SDLK_LALT:
				sym = K_ALT;
				break;
			case SDLK_KP_0:
				if (modstate & KMOD_NUM)
					sym = K_INS;
				else
					sym = SDLK_0;
				break;
			case SDLK_KP_1:
				if (modstate & KMOD_NUM)
					sym = K_END;
				else
					sym = SDLK_1;
				break;
			case SDLK_KP_2:
				if (modstate & KMOD_NUM)
					sym = K_DOWNARROW;
				else
					sym = SDLK_2;
				break;
			case SDLK_KP_3:
				if (modstate & KMOD_NUM)
					sym = K_PGDN;
				else
					sym = SDLK_3;
				break;
			case SDLK_KP_4:
				if (modstate & KMOD_NUM)
					sym = K_LEFTARROW;
				else
					sym = SDLK_4;
				break;
			case SDLK_KP_5:
				sym = SDLK_5;
				break;
			case SDLK_KP_6:
				if (modstate & KMOD_NUM)
					sym = K_RIGHTARROW;
				else
					sym = SDLK_6;
				break;
			case SDLK_KP_7:
				if (modstate & KMOD_NUM)
					sym = K_HOME;
				else
					sym = SDLK_7;
				break;
			case SDLK_KP_8:
				if (modstate & KMOD_NUM)
					sym = K_UPARROW;
				else
					sym = SDLK_8;
				break;
			case SDLK_KP_9:
				if (modstate & KMOD_NUM)
					sym = K_PGUP;
				else
					sym = SDLK_9;
				break;
			case SDLK_KP_PERIOD:
				if (modstate & KMOD_NUM)
					sym = K_DEL;
				else
					sym = SDLK_PERIOD;
				break;
			case SDLK_KP_DIVIDE:
				sym = SDLK_SLASH;
				break;
			case SDLK_KP_MULTIPLY:
				sym = SDLK_ASTERISK;
				break;
			case SDLK_KP_MINUS:
				sym = SDLK_MINUS;
				break;
			case SDLK_KP_PLUS:
				sym = SDLK_PLUS;
				break;
			case SDLK_KP_ENTER:
				sym = SDLK_RETURN;
				break;
			case SDLK_KP_EQUALS:
				sym = SDLK_EQUALS;
				break;
			}
			// If we're not directly handled and still above 255
			// just force it to 0
			if (sym > 255)
				sym = 0;
			Key_Event(sym, state);
			break;
		// WinQuake behavior: Use Mouse OFF disables mouse input entirely
		// ON grabs the mouse, kinda like SetRelativeMouseMode(SDL_TRUE)
		// Fullscreen grabs the mouse unconditionally
		case SDL_MOUSEMOTION:
			if ((event.motion.x != ((int)vid.width / 2)) ||
			    (event.motion.y != ((int)vid.height / 2))) {
				mouse_x = event.motion.xrel * 10;
				mouse_y = event.motion.yrel * 10;
			}
			break;
		case SDL_WINDOWEVENT:
			switch (event.window.event) {
			case SDL_WINDOWEVENT_RESIZED:
			case SDL_WINDOWEVENT_SIZE_CHANGED:
				SDLWindowFlags = SDL_GetWindowFlags(window);
				windowSurface = SDL_GetWindowSurface(window);
				VID_CalcScreenDimensions();
				break;
			}
			break;
		case SDL_QUIT:
			CL_Disconnect();
			Host_ShutdownServer(false);
			Sys_Quit();
			break;
		default:
			break;
		}
	}
}

void IN_Init(void)
{
	if (COM_CheckParm("-nomouse"))
		return;
	mouse_x = mouse_y = 0.0;
	mouse_avail = 1;
}

void IN_Shutdown(void)
{
	mouse_avail = 0;
}

void IN_Commands(void)
{
	int i;
	int mouse_buttonstate;

	if (!mouse_avail)
		return;

	i = SDL_GetMouseState(NULL, NULL);
	/* Quake swaps the second and third buttons */
	mouse_buttonstate = (i & ~0x06) | ((i & 0x02) << 1) | ((i & 0x04) >> 1);
	for (i = 0; i < 3; i++) {
		if ((mouse_buttonstate & (1 << i))
		    && !(mouse_oldbuttonstate & (1 << i)))
			Key_Event(K_MOUSE1 + i, true);

		if (!(mouse_buttonstate & (1 << i))
		    && (mouse_oldbuttonstate & (1 << i)))
			Key_Event(K_MOUSE1 + i, false);
	}
	mouse_oldbuttonstate = mouse_buttonstate;
}

void IN_Move(usercmd_t *cmd)
{
	if (!mouse_avail)
		return;
	if (!(SDLWindowFlags & (SDL_WINDOW_FULLSCREEN
				| SDL_WINDOW_FULLSCREEN_DESKTOP))
	    && !_windowed_mouse.value) {
		SDL_SetRelativeMouseMode(SDL_FALSE);
		return;
	}
	SDL_SetRelativeMouseMode(SDL_TRUE);

	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value * sensitivityyscale.value;

	if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
		cmd->sidemove += m_side.value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;
	if (in_mlook.state & 1)
		V_StopPitchDrift();

	if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	} else {
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}
	mouse_x = mouse_y = 0.0;
}

