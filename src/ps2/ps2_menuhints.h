// PS2 menu button hints (PS2-338, PS2-339, OPT11 NETUI): the pad buttons of the highlighted menu item, along the bottom edge of the screen - navigation at the lower left,
// back / extra actions at the lower right - drawn by M_Drawer (m_menu.c) as the last thing of a menu frame, in both renderers (plain 2D V_* calls).
//
// What is shown follows what the menu really does with the pad (src/ps2/ps2_padmap.c: Cross = JOY1, Circle = JOY2, Square = JOY3, Triangle = JOY4, D-pad = hat 0;
// M_Responder maps them for the menu: Cross = Enter, Circle = Escape, Square = Backspace, D-pad = the arrows) and by the kind of the item under the cursor
// (menuitem_t.status: M_PS2MenuKind() in m_menu.c classifies it). The sets are data (ps2_menuhints.c: one row per kind).
// The cvar menuhints (Options > Banpyura Options > Console > Menu Button Hints, saved) switches all of it off: nothing is drawn or noted then.
//
// NEVER OVER ANYTHING (PS2-339): every 2D draw of the frame from the HUD on (V_DrawFill, V_DrawStretchyFixedPatch = every letter, V_DrawCroppedPatch, V_DrawFadeFill) is noted
// in a one bit per pixel map of the 320x200 picture (v_video.c hooks, only while a menu is up); a group of hints is placed where the map is empty with a margin of 2 px
// all round and inside the safe area (6 px from the sides, 4 px from the bottom): in one line at the bottom, else with the two halves stacked, else as icons only, each of them
// also one and two rows higher; when nothing fits the group is not drawn at all. The test "-menuhintscheck" (software renderer) checks the result on the pixels of the
// frame: it draws the menu alone on a flat colour, finds the pixels that the menu itself covers and counts those within 2 px of a plate (MHCHECK lines); the crawler
// command "ps2_menucrawl" walks every menu definition and every item for it.
#ifndef __PS2_MENUHINTS_H__
#define __PS2_MENUHINTS_H__

#include "../doomtype.h"
#include "../command.h"
#include "../m_fixed.h"
#include "../r_defs.h"
#include "../m_menu.h" // menu_t

// the kind of the highlighted item (m_menu.c: M_PS2MenuKind)
typedef enum
{
	PS2MH_SELECT,     // an entry that opens a sub menu or does its thing (IT_CALL / IT_SUBMENU / others)
	PS2MH_MAIN,       // the main menu
	PS2MH_ARROWS,     // a value: a cvar or an item with arrows (left / right change it, Backspace puts the default back)
	PS2MH_TEXT,       // a text cvar (a name, a string): typed text, the on-screen keyboard on Triangle
	PS2MH_ADDRESS,    // the server address of the Multiplayer menu: Enter connects
	PS2MH_PLAYERNAME, // the name of the player setup
	PS2MH_CONTROL,    // a control of Setup Controls: Enter waits for a button, Backspace clears
	PS2MH_CAPTURE,    // Setup Controls waits for the new button
	PS2MH_YESNO,      // a Yes / No message box
	PS2MH_MESSAGE,    // a message box that Enter / Escape / Space / N / Y / Delete close
	PS2MH_SERVER,     // a line of the server list
	PS2MH_NONE,       // nothing: the menu is a picture over the whole screen
	PS2MH_NUMKINDS
} ps2mh_kind_t;

extern consvar_t cv_menuhints;
extern boolean ps2mh_recording; // v_video.c: a menu frame is being drawn, the 2D draws are noted

INT32 M_PS2MenuKind(void);                           // m_menu.c: the ps2mh_kind_t of the item under the cursor
INT32 M_PS2MenuList(INT32 i, menu_t **menu, const char **name); // m_menu.c: menu number i of every menu definition (for the crawler), the number of them
INT32 M_PS2MenuEnter(menu_t *m, INT32 index, INT32 total); // m_menu.c: the crawler brings up menu m (index of total in the list)
INT32 M_PS2MenuCursor(INT32 set);                    // m_menu.c: the item under the cursor (set >= 0: move it there first)

void PS2MenuHints_RegisterCvars(void);               // before the config file is read (i_video.c, with the other PS2 video cvars)
void PS2MenuHints_Begin(void);                       // d_main.c: the HUD is about to be drawn (a level frame with a menu on it)
void PS2MenuHints_MenuStart(void);                   // the start of M_Drawer
void PS2MenuHints_Draw(void);                        // the end of M_Drawer
boolean PS2MenuHints_NotePatch(fixed_t x, fixed_t y, fixed_t pscale, fixed_t vscale, INT32 scrn, const patch_t *patch); // v_video.c
boolean PS2MenuHints_NoteCropped(fixed_t x, fixed_t y, fixed_t pscale, fixed_t vscale, INT32 scrn, const patch_t *patch, fixed_t w, fixed_t h); // v_video.c (true: do not draw)
boolean PS2MenuHints_NoteRect(INT32 x, INT32 y, INT32 w, INT32 h, INT32 flags);                                         // v_video.c
void PS2MenuHints_Frame(void);                       // once per displayed frame (ps2_net.c PS2Net_Frame): the crawler's step; the -menuhintscheck options are read here
boolean PS2MenuHints_Checking(void);                 // M_Drawer is being run for the check: nothing is drawn or noted

#endif
