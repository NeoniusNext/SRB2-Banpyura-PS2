// PS2-135: on-screen keyboard for the text fields of the menus (server address, player name, cvar strings): there is no keyboard on the console.
// It sits between the pad events and M_Responder: while open it swallows the pad's buttons and feeds ev_text/ev_keydown events to the menu.
#ifndef __PS2_OSK_H__
#define __PS2_OSK_H__

#include "../doomtype.h"
#include "../d_event.h"

boolean PS2OSK_Responder(const event_t *ev); // true: the event was consumed (call first in M_Responder)
void PS2OSK_Draw(void); // after the menu was drawn
boolean PS2OSK_Active(void);
void PS2OSK_TestClose(void); // (the crawler of ps2_menuhints.c: the next menu starts without the keyboard)
void PS2OSK_ChatOpened(boolean team); // OPT14-CHAT: the chat line was opened (hu_stuff.c): the keyboard comes up on it when the last button came from the pad
void PS2OSK_TestOpen(void); // PS2-336: opens the keyboard without a text field (the test card "ps2_icons 2" takes its picture)

// m_menu.c: the highlighted menu item takes typed text
boolean M_PS2TextFieldActive(void);

#endif
