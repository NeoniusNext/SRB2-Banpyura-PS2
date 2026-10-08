// PS2 pad button icons in text and menus (PS2-333..337, OPT11 NETUI). The pictures are the PlayStation button sheet of assets/ps2ui (credit: "By: Max_the_Viking"),
// cut, reduced to 13..16 px and converted to Doom patches in the game palette by tools/ps2/ui_icons.py; the patches are in the ELF (ps2_uiicons_data.inc), so there is
// nothing to load, nothing in a pack (an extra pack would show up in the file list of a network game) and nothing that can be missing.
//
// In text: one control character stands for one icon, e.g. V_DrawString(x, y, flags, PS2I_CIRCLE " Cancel"). V_DrawString/V_DrawCenteredString/V_DrawRight...,
// V_StringWidth and the word wrap (v_video.c, under PS2) know the characters: the icon is drawn like a letter (its width, the colour of the string does not apply to it).
// The codes are 0x01..0x08, 0x0B, 0x0C, 0x0E..0x15 (never \t \n \r); what the unpatched code would do with them is draw a space.
#ifndef __PS2_UIICONS_H__
#define __PS2_UIICONS_H__

#include "../doomtype.h"
#include "../r_defs.h"

// the icons of ps2_uiicons_data.inc in the order of the sheet; names are BTN_<name>
typedef enum
{
	PS2UI_SELECT, PS2UI_START,
	PS2UI_TRIANGLE, PS2UI_SQUARE, PS2UI_CIRCLE, PS2UI_CROSS,
	PS2UI_L2, PS2UI_R2, PS2UI_L1, PS2UI_R1,
	PS2UI_LSTICK, PS2UI_DPAD_DOWN, PS2UI_DPAD_LEFT, PS2UI_DPAD_RIGHT, PS2UI_DPAD_UP, PS2UI_DPAD_LR, PS2UI_DPAD_ALL, PS2UI_DPAD_UD,
	PS2UI_RSTICK, PS2UI_RSTICK_DOWN, PS2UI_RSTICK_LEFT, PS2UI_RSTICK_RIGHT, PS2UI_RSTICK_UP, PS2UI_RSTICK_ALL, PS2UI_RSTICK_UD, PS2UI_RSTICK_LR,
	PS2UI_L3, PS2UI_LSTICK_DOWN, PS2UI_LSTICK_LEFT, PS2UI_LSTICK_RIGHT, PS2UI_LSTICK_UP, PS2UI_LSTICK_ALL, PS2UI_LSTICK_UD, PS2UI_LSTICK_LR,
	PS2UI_R3, PS2UI_LSTICK_ROT, PS2UI_RSTICK_ROT,
	PS2UI_ARROW_LEFT, PS2UI_ARROW_RIGHT, PS2UI_ARROW_DOWN, PS2UI_ARROW_UP,
	PS2UI_NUMICONS
} ps2ui_icon_t;

// text tokens
#define PS2I_CROSS      "\x01"
#define PS2I_CIRCLE     "\x02"
#define PS2I_SQUARE     "\x03"
#define PS2I_TRIANGLE   "\x04"
#define PS2I_L1         "\x05"
#define PS2I_R1         "\x06"
#define PS2I_L2         "\x07"
#define PS2I_R2         "\x08"
#define PS2I_START      "\x0B"
#define PS2I_SELECT     "\x0C"
#define PS2I_DPAD_UP    "\x0E"
#define PS2I_DPAD_DOWN  "\x0F"
#define PS2I_DPAD_LEFT  "\x10"
#define PS2I_DPAD_RIGHT "\x11"
#define PS2I_L3         "\x12"
#define PS2I_R3         "\x13"
#define PS2I_DPAD_UD    "\x14"
#define PS2I_DPAD_LR    "\x15"

// the icon of a control character of a string, or -1 (an ordinary character)
INT32 PS2UI_TokenIcon(UINT8 c);
// the icon as a patch (created on first use, kept), NULL when there is no room for it
patch_t *PS2UI_Patch(INT32 icon);
// the width in pixels that a token takes in a string (the icon and a pixel of air)
INT32 PS2UI_TokenWidth(UINT8 c);
// the icon of a joystick key number of the engine (KEY_JOY1+n / KEY_HAT1+n, also the second pad), or -1; the text token of it ("" when none)
INT32 PS2UI_KeyIcon(INT32 keynum);
const char *PS2UI_KeyToken(INT32 keynum);
// the text of a message box for the console: ESC / ENTER as words become the Circle / Cross icon, "Press a key" becomes "Press <Cross> or <Circle>" (a zone copy: Z_Free it)
char *PS2UI_Message(const char *src);
// the console command "ps2_icons" (a test card of all icons, drawn by M_Drawer: PS2UI_Card)
void PS2UI_RegisterCommands(void);
void PS2UI_Card(void);

#endif
