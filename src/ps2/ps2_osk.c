// PS2-135: see ps2_osk.h. Layout: four rows of characters and a row of commands.
//   Cross: type / run the command, D-pad: move, Square: Shift, Triangle: Backspace, Start: Enter (OK), Circle: close the keyboard,
//   Select (OPT14-CHAT): the symbols layer (punctuation and brackets, which the two letter layers have little of).
// OPT14-CHAT: on the chat line of a netgame (hu_stuff.c) the keyboard sits at the TOP of the picture (the chat window and the line being typed stay in view at the bottom),
//   Start sends, Circle drops the message (the chat line closes), L1 / R1 scroll the chat log, L2 / R2 move the text cursor, and the game goes on under it: the sticks still
//   steer the player (the original chat does not stop them either), and the release of a button that was pressed before the keyboard came up reaches the game.
#include "../doomdef.h"
#include "../d_main.h"
#include "../d_event.h"
#include "../g_input.h"
#include "../g_game.h" // cv_consolechat (OLDCHAT)
#include "../i_system.h"
#include "../i_time.h"
#include "../hu_stuff.h"
#include "../keys.h"
#include "../v_video.h"
#include "../z_zone.h"
#include "../m_menu.h"

#include "ps2_osk.h"
#include "ps2_uiicons.h"
#include "ps2_menuhints.h"

#define OSK_COLS 10
#define OSK_ROWS 5

static const char *const rows_lower[4] = {"1234567890", "qwertyuiop", "asdfghjkl.", "zxcvbnm:/-"};
static const char *const rows_upper[4] = {"!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM;?+"};
static const char *const rows_sym[4] = {".,!?'\":;-_", "()[]{}<>=+", "@#$%&*/\\|~", "1234567890"};
// the command row: 5 buttons of two cells each
static const char *const cmd_names[5] = {"Shift", "Space", "Bksp", "Enter", "Close"};
static const char *const cmd_names_chat[5] = {"Shift", "Space", "Bksp", "Send", "Cancel"};

static boolean active, upper, sym;
static boolean chatmode;   // the keyboard is on the chat line of a netgame: top of the picture, the game goes on
static boolean chatteam;
static boolean last_pad;   // the last button pressed was one of a pad (not of a USB keyboard): the chat line brings the keyboard up
static INT32 curx, cury;
static INT32 held_dir = -1; // 0..3 = up/down/left/right of the D-pad being held
static tic_t held_since, held_last;
static UINT32 oskdown;     // chat mode: the pad buttons (bit n = KEY_JOY1+n, bit 16+n = KEY_HAT1+n) that were pressed while the keyboard was up: their release is ours, the others go to the game

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
	return (sym ? rows_sym : upper ? rows_upper : rows_lower)[y][x];
}

static void Open(void)
{
	active = true;
	upper = sym = false;
	curx = cury = 0;
	held_dir = -1;
	oskdown = 0;
	chatmode = chat_on;
}

static void Close(void)
{
	active = false;
	held_dir = -1;
	oskdown = 0;
}

// chat mode: the message is dropped and the chat line closes (the key of the original chat for it: Escape)
static void CancelChat(void)
{
	Press(KEY_ESCAPE);
	Close();
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
			case 3: Press(KEY_ENTER); Close(); break;
			default:
				if (chatmode)
					CancelChat();
				else
					Close();
				break;
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

void PS2OSK_TestClose(void)
{
	Close();
}

void PS2OSK_TestOpen(void)
{
	Open();
	chatmode = false;
}

// OPT14-CHAT: the chat line was opened (the talk key, the menu item): a pad has no keys to type with, so the keyboard comes up on it; the player with a USB keyboard
// (the last button pressed was a key) types on that. Triangle opens the keyboard on the chat line later as on any text field.
void PS2OSK_ChatOpened(boolean team)
{
	chatteam = team;
	if (last_pad && !active)
	{
		Open();
		chatmode = true;
	}
}

// the bit of a button of the first pad in oskdown, or -1
static INT32 PadBit(INT32 key)
{
	if (key >= KEY_JOY1 && key < KEY_JOY1 + 16)
		return key - KEY_JOY1;
	if (key >= KEY_HAT1 && key < KEY_HAT1 + 4)
		return 16 + key - KEY_HAT1;
	return -1;
}

boolean PS2OSK_Responder(const event_t *ev)
{
	if (ev->type == ev_keydown && ev->key < KEY_MOUSE1)
		last_pad = false; // a key of the keyboard
	else if (ev->type == ev_keydown && ev->key >= KEY_JOY1 && ev->key < KEY_HAT1 + JOYHATS * 4)
		last_pad = true; // a button of the pad (or a direction of its D-pad)

	if (!active)
	{
		// Triangle opens it on a text field (the menu maps Triangle to 'n' otherwise: of no use in a text field)
		if (ev->type == ev_keydown && ev->key == KEY_JOY1 + 3 && M_PS2TextFieldActive())
		{
			Open();
			return true;
		}
		return false;
	}

	if (ev->type == ev_keydown)
	{
		const INT32 k = ev->key;
		const INT32 bit = PadBit(k);

		if (chatmode && bit >= 0)
			oskdown |= 1u << bit;
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
		{
			if (chatmode)
				CancelChat();
			else
				active = false;
		}
		else if (k == KEY_JOY1 + 2)
			upper = !upper;
		else if (k == KEY_JOY1 + 3)
			Press(KEY_BACKSPACE);
		else if (k == KEY_JOY1 + 6)
			sym = !sym;
		else if (k == KEY_JOY1 + 7)
		{
			Press(KEY_ENTER);
			Close();
		}
		else if (chatmode && k == KEY_JOY1 + 4)
			Press(KEY_UPARROW); // the chat log, one line back
		else if (chatmode && k == KEY_JOY1 + 5)
			Press(KEY_DOWNARROW);
		else if (chatmode && k == KEY_JOY1 + 10)
			Press(KEY_LEFTARROW); // the text cursor
		else if (chatmode && k == KEY_JOY1 + 11)
			Press(KEY_RIGHTARROW);
		else if (k >= KEY_JOY1 && k < KEY_JOY1 + 32)
			;
		else
			return false; // the engine's own events (ev_text / KEY_BACKSPACE we posted) go on to the menu
		return true;
	}
	if (ev->type == ev_keyup)
	{
		const INT32 bit = PadBit(ev->key);

		if (ev->key >= KEY_HAT1 && ev->key <= KEY_HAT1 + 3)
		{
			if (held_dir == ev->key - KEY_HAT1)
				held_dir = -1;
			if (chatmode && !(oskdown & (1u << bit)))
				return false; // pressed before the keyboard came up: the game must see it let go
			oskdown &= ~(1u << bit);
			return true;
		}
		if (chatmode && bit >= 0)
		{
			if (!(oskdown & (1u << bit)))
				return false;
			oskdown &= ~(1u << bit);
		}
		return ev->key >= KEY_JOY1 && ev->key < KEY_JOY1 + 32;
	}
	// the sticks do not move the menu cursor under the keyboard; on the chat line they still steer the player
	return ev->type == ev_joystick && !chatmode;
}

// the chat keyboard: the top of the picture, 90 px high, so that the chat window (its log starts at y = 93 with the default settings) and the line being typed stay in view
static void DrawChat(void)
{
	static const char hint1[] = PS2I_CROSS " type  " PS2I_SQUARE " shift  " PS2I_TRIANGLE " delete  " PS2I_SELECT " symbols";
	static const char hint2[] = PS2I_START " send  " PS2I_CIRCLE " cancel  " PS2I_L1 PS2I_R1 " scroll  " PS2I_L2 PS2I_R2 " cursor";
	const INT32 x0 = 40, y0 = 14, pitch = 11;
	INT32 r, c;

	V_DrawFill(x0 - 6, 1, 252, 90, 31);
	V_DrawFill(x0 - 6, 1, 252, 1, 0);
	V_DrawFill(x0 - 6, 90, 252, 1, 0);
	V_DrawCenteredString(160, 3, V_YELLOWMAP, chatteam ? "TEAM CHAT" : "CHAT");
	if (sym || upper)
		V_DrawRightAlignedThinString(x0 + 240, 5, V_ALLOWLOWERCASE | V_YELLOWMAP, sym ? "symbols" : "shift");
	for (r = 0; r < 4; r++)
		for (c = 0; c < OSK_COLS; c++)
		{
			char s[2] = {CharAt(c, r), 0};
			const boolean cur = (r == cury && c == curx);
			const INT32 x = x0 + c * 24, y = y0 + r * pitch;

			if (cur)
				V_DrawFill(x, y - 1, 22, 10, 73);
			V_DrawCenteredString(x + 11, y, V_ALLOWLOWERCASE | (cur ? V_YELLOWMAP : 0), s);
		}
	for (c = 0; c < 5; c++)
	{
		const boolean cur = (cury == 4 && curx / 2 == c);
		const INT32 x = x0 + c * 48, y = y0 + 4 * pitch + 3;

		if (cur)
			V_DrawFill(x, y - 1, 46, 10, 73);
		else if (c == 0 && upper)
			V_DrawFill(x, y - 1, 46, 10, 54);
		V_DrawCenteredString(x + 23, y, V_ALLOWLOWERCASE | (cur ? V_YELLOWMAP : 0), cmd_names_chat[c]);
	}
	PS2MenuHints_Log("osk", hint1); // PS2-341: the check for hints said twice
	PS2MenuHints_Log("osk", hint2);
	V_DrawCenteredThinString(160, 74, V_ALLOWLOWERCASE, hint1);
	V_DrawCenteredThinString(160, 82, V_ALLOWLOWERCASE, hint2);
}

void PS2OSK_Draw(void)
{
	INT32 x0 = 40, y0 = 92, r, c;

	if (!active)
		return;
	if (chatmode && (!chat_on || !netgame))
	{
		Close(); // the chat line closed under it (the server muted the player, the connection went, the level ended)
		return;
	}

	// auto-repeat of a held D-pad direction
	if (held_dir >= 0 && I_GetTime() >= held_since + 12 && I_GetTime() >= held_last + 3)
	{
		Move(held_dir);
		held_last = I_GetTime();
	}

	if (chatmode && !OLDCHAT)
	{
		DrawChat();
		return;
	}

	V_DrawFill(x0 - 6, y0 - 14, 252, 108, 31);
	V_DrawFill(x0 - 6, y0 - 14, 252, 1, 0);
	V_DrawFill(x0 - 6, y0 + 93, 252, 1, 0);
	V_DrawCenteredString(160, y0 - 11, V_YELLOWMAP, "KEYBOARD");
	if (sym)
		V_DrawRightAlignedThinString(x0 + 240, y0 - 9, V_ALLOWLOWERCASE | V_YELLOWMAP, "symbols");
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
		V_DrawCenteredString(x + 23, y, V_ALLOWLOWERCASE | (cur ? V_YELLOWMAP : 0), chatmode ? cmd_names_chat[c] : cmd_names[c]);
	}
	// PS2-336: the buttons by their icons (thin font: the five labels and icons fit the 252 px of the panel)
	{
		static const char hint[] = PS2I_CROSS " type  " PS2I_SQUARE " shift  " PS2I_TRIANGLE " del  " PS2I_START " ok  " PS2I_CIRCLE " close";
		static const char hint_sym[] = PS2I_SELECT " symbols";

		PS2MenuHints_Log("osk", hint); // PS2-341: the check for hints said twice
		V_DrawCenteredThinString(160, y0 + 78, V_ALLOWLOWERCASE, hint);
		V_DrawCenteredThinString(160, y0 + 86, V_ALLOWLOWERCASE, hint_sym);
	}
}
