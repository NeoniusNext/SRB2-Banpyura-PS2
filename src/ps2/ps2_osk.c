// PS2-135: see ps2_osk.h. Layout: four rows of characters and a row of commands.
//   Cross: type / run the command, D-pad: move, Square: Shift, Triangle: Backspace, Start: Enter (OK), Circle: close the keyboard.
#include "../doomdef.h"
#include "../d_main.h"
#include "../d_event.h"
#include "../g_input.h"
#include "../i_system.h"
#include "../i_time.h"
#include "../v_video.h"
#include "../z_zone.h"
#include "../m_menu.h"

#include "ps2_osk.h"
#include "ps2_uiicons.h"

#define OSK_COLS 10
#define OSK_ROWS 5

static const char *const rows_lower[4] = {"1234567890", "qwertyuiop", "asdfghjkl.", "zxcvbnm:/-"};
static const char *const rows_upper[4] = {"!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM;?+"};
// the command row: 5 buttons of two cells each
static const char *const cmd_names[5] = {"Shift", "Space", "Bksp", "Enter", "Close"};

static boolean active, upper;
static INT32 curx, cury;
static INT32 held_dir = -1; // 0..3 = up/down/left/right of the D-pad being held
static tic_t held_since, held_last;

boolean PS2OSK_Active(void)
{
	return active;
}

static void Post(evtype_t type, INT32 key)
{
	event_t e;

	e.type = type;
	e.key = key;
	e.x = e.y = 0;
	e.repeated = false;
	D_PostEvent(&e);
}

static void Press(INT32 key)
{
	Post(ev_keydown, key);
	Post(ev_keyup, key);
}

static char CharAt(INT32 x, INT32 y)
{
	return (upper ? rows_upper : rows_lower)[y][x];
}

static void Activate(void)
{
	if (cury < 4)
	{
		Post(ev_text, (unsigned char)CharAt(curx, cury));
		if (upper)
			upper = false; // one capital at a time: the usual on-screen keyboard behaviour
	}
	else
	{
		switch (curx / 2)
		{
			case 0: upper = !upper; break;
			case 1: Post(ev_text, ' '); break;
			case 2: Press(KEY_BACKSPACE); break;
			case 3: Press(KEY_ENTER); active = false; break;
			default: active = false; break;
		}
	}
}

static void Move(INT32 dir)
{
	switch (dir)
	{
		case 0: cury = (cury + OSK_ROWS - 1) % OSK_ROWS; break;
		case 1: cury = (cury + 1) % OSK_ROWS; break;
		case 2: curx = (curx + OSK_COLS - 1) % OSK_COLS; break;
		default: curx = (curx + 1) % OSK_COLS; break;
	}
}

void PS2OSK_TestOpen(void)
{
	active = true;
	upper = false;
	curx = cury = 0;
	held_dir = -1;
}

boolean PS2OSK_Responder(const event_t *ev)
{
	if (!active)
	{
		// Triangle opens it on a text field (the menu maps Triangle to 'n' otherwise: of no use in a text field)
		if (ev->type == ev_keydown && ev->key == KEY_JOY1 + 3 && M_PS2TextFieldActive())
		{
			active = true;
			upper = false;
			curx = cury = 0;
			held_dir = -1;
			return true;
		}
		return false;
	}

	if (ev->type == ev_keydown)
	{
		const INT32 k = ev->key;

		if (k >= KEY_HAT1 && k <= KEY_HAT1 + 3)
		{
			held_dir = k - KEY_HAT1;
			held_since = held_last = I_GetTime();
			Move(held_dir);
			return true;
		}
		if (k == KEY_JOY1)
			Activate();
		else if (k == KEY_JOY1 + 1)
			active = false;
		else if (k == KEY_JOY1 + 2)
			upper = !upper;
		else if (k == KEY_JOY1 + 3)
			Press(KEY_BACKSPACE);
		else if (k == KEY_JOY1 + 7)
		{
			Press(KEY_ENTER);
			active = false;
		}
		else if (k >= KEY_JOY1 && k < KEY_JOY1 + 32)
			;
		else
			return false; // the engine's own events (ev_text / KEY_BACKSPACE we posted) go on to the menu
		return true;
	}
	if (ev->type == ev_keyup)
	{
		if (ev->key >= KEY_HAT1 && ev->key <= KEY_HAT1 + 3)
		{
			if (held_dir == ev->key - KEY_HAT1)
				held_dir = -1;
			return true;
		}
		return ev->key >= KEY_JOY1 && ev->key < KEY_JOY1 + 32;
	}
	// the sticks do not move the menu cursor under the keyboard
	return ev->type == ev_joystick;
}

void PS2OSK_Draw(void)
{
	INT32 x0 = 40, y0 = 92, r, c;

	if (!active)
		return;

	// auto-repeat of a held D-pad direction
	if (held_dir >= 0 && I_GetTime() >= held_since + 12 && I_GetTime() >= held_last + 3)
	{
		Move(held_dir);
		held_last = I_GetTime();
	}

	V_DrawFill(x0 - 6, y0 - 14, 252, 108, 31);
	V_DrawFill(x0 - 6, y0 - 14, 252, 1, 0);
	V_DrawFill(x0 - 6, y0 + 93, 252, 1, 0);
	V_DrawCenteredString(160, y0 - 11, V_YELLOWMAP, "KEYBOARD");
	for (r = 0; r < 4; r++)
		for (c = 0; c < OSK_COLS; c++)
		{
			char s[2] = {CharAt(c, r), 0};
			const boolean cur = (r == cury && c == curx);
			const INT32 x = x0 + c * 24, y = y0 + r * 14;

			if (cur)
				V_DrawFill(x, y - 2, 22, 12, 73);
			V_DrawCenteredString(x + 11, y, V_ALLOWLOWERCASE | (cur ? V_YELLOWMAP : 0), s);
		}
	for (c = 0; c < 5; c++)
	{
		const boolean cur = (cury == 4 && curx / 2 == c);
		const INT32 x = x0 + c * 48, y = y0 + 4 * 14 + 4;

		if (cur)
			V_DrawFill(x, y - 2, 46, 12, 73);
		else if (c == 0 && upper)
			V_DrawFill(x, y - 2, 46, 12, 54);
		V_DrawCenteredString(x + 23, y, V_ALLOWLOWERCASE | (cur ? V_YELLOWMAP : 0), cmd_names[c]);
	}
	// PS2-336: the buttons by their icons (thin font: the five labels and icons fit the 252 px of the panel)
	V_DrawCenteredThinString(160, y0 + 78, V_ALLOWLOWERCASE, PS2I_CROSS " type  " PS2I_SQUARE " shift  " PS2I_TRIANGLE " del  " PS2I_START " ok  " PS2I_CIRCLE " close");
}
