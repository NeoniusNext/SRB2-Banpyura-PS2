// PS2-151 (OPT9-K): USB HID usage -> engine key / typed character. Pure functions, no engine state: ps2_kbd.c includes this, and so does the host test
// tools/ps2/kbd_map_hosttest.c that checks the table against src/sdl/i_video.c (Impl_SDL_Scancode_To_Keycode) and the text rules.
#ifndef PS2_KBD_MAP_H
#define PS2_KBD_MAP_H

#include "../keys.h"

#define HID_LCTRL  0xE0
#define HID_LSHIFT 0xE1
#define HID_LALT   0xE2
#define HID_LGUI   0xE3
#define HID_RCTRL  0xE4
#define HID_RSHIFT 0xE5
#define HID_RALT   0xE6
#define HID_RGUI   0xE7
#define HID_CAPS   0x39
#define HID_SCROLL 0x47
#define HID_NUMLK  0x53

// SDL scancode == HID usage, so this is Impl_SDL_Scancode_To_Keycode with the usages written out. 0 = the engine has no key for it.
static inline int PS2Kbd_UsageToKey(unsigned char u)
{
	if (u >= 0x04 && u <= 0x1D)
		return u - 0x04 + 'a';
	if (u >= 0x1E && u <= 0x26)
		return u - 0x1E + '1';
	if (u == 0x27)
		return '0';
	if (u >= 0x3A && u <= 0x43)
		return KEY_F1 + (u - 0x3A);
	switch (u)
	{
		case 0x44: return KEY_F11;
		case 0x45: return KEY_F12;

		case 0x62: return KEY_KEYPAD0;
		case 0x59: return KEY_KEYPAD1;
		case 0x5A: return KEY_KEYPAD2;
		case 0x5B: return KEY_KEYPAD3;
		case 0x5C: return KEY_KEYPAD4;
		case 0x5D: return KEY_KEYPAD5;
		case 0x5E: return KEY_KEYPAD6;
		case 0x5F: return KEY_KEYPAD7;
		case 0x60: return KEY_KEYPAD8;
		case 0x61: return KEY_KEYPAD9;

		case 0x28: return KEY_ENTER;
		case 0x29: return KEY_ESCAPE;
		case 0x2A: return KEY_BACKSPACE;
		case 0x2B: return KEY_TAB;
		case 0x2C: return KEY_SPACE;
		case 0x2D: return KEY_MINUS;
		case 0x2E: return KEY_EQUALS;
		case 0x2F: return '[';
		case 0x30: return ']';
		case 0x31: return '\\';
		case 0x32: return '#';
		case 0x33: return ';';
		case 0x34: return '\'';
		case 0x35: return '`';
		case 0x36: return ',';
		case 0x37: return '.';
		case 0x38: return '/';
		case HID_CAPS: return KEY_CAPSLOCK;
		case 0x46: return 0; // print screen: undefined in the engine too
		case HID_SCROLL: return KEY_SCROLLLOCK;
		case 0x48: return KEY_PAUSE;
		case 0x49: return KEY_INS;
		case 0x4A: return KEY_HOME;
		case 0x4B: return KEY_PGUP;
		case 0x4C: return KEY_DEL;
		case 0x4D: return KEY_END;
		case 0x4E: return KEY_PGDN;
		case 0x4F: return KEY_RIGHTARROW;
		case 0x50: return KEY_LEFTARROW;
		case 0x51: return KEY_DOWNARROW;
		case 0x52: return KEY_UPARROW;
		case HID_NUMLK: return KEY_NUMLOCK;
		case 0x54: return KEY_KPADSLASH;
		case 0x55: return '*';
		case 0x56: return KEY_MINUSPAD;
		case 0x57: return KEY_PLUSPAD;
		case 0x58: return KEY_ENTER;
		case 0x63: return KEY_KPADDEL;
		case 0x64: return '\\';
		case 0x65: return KEY_MENU; // application key: the SDL table leaves it out, the engine names it

		case HID_LSHIFT: return KEY_LSHIFT;
		case HID_RSHIFT: return KEY_RSHIFT;
		case HID_LCTRL:  return KEY_LCTRL;
		case HID_RCTRL:  return KEY_RCTRL;
		case HID_LALT:   return KEY_LALT;
		case HID_RALT:   return KEY_RALT;
		case HID_LGUI:   return KEY_LEFTWIN;
		case HID_RGUI:   return KEY_RIGHTWIN;
		default: break;
	}
	return 0;
}

// US layout: the character a key types, 0 = none. Letters follow shift XOR caps lock; the keypad types with num lock on and shift up (shift gives
// the navigation function, as on a PC) except / * - + which type always. Control characters (Enter, Tab, Backspace, Esc) type nothing: SDL_TEXTINPUT
// does not carry them either; they reach the engine as keys.
static inline char PS2Kbd_UsageToText(unsigned char u, int shift, int caps, int numlock)
{
	static const char digits_shift[] = "!@#$%^&*(";
	static const char plain[]   = {'-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/'};  // 0x2D..0x38
	static const char shifted[] = {'_', '+', '{', '}', '|',  '~', ':', '"',  '~', '<', '>', '?'};

	if (u >= 0x04 && u <= 0x1D)
		return (char)((u - 0x04) + (((shift != 0) != (caps != 0)) ? 'A' : 'a'));
	if (u >= 0x1E && u <= 0x26)
		return shift ? digits_shift[u - 0x1E] : (char)('1' + (u - 0x1E));
	if (u == 0x27)
		return shift ? ')' : '0';
	if (u == 0x2C)
		return ' ';
	if (u >= 0x2D && u <= 0x38)
		return shift ? shifted[u - 0x2D] : plain[u - 0x2D];
	if (u == 0x64)
		return shift ? '|' : '\\';
	switch (u)
	{
		case 0x54: return '/';
		case 0x55: return '*';
		case 0x56: return '-';
		case 0x57: return '+';
		default: break;
	}
	if (numlock && !shift)
	{
		if (u >= 0x59 && u <= 0x61)
			return (char)('1' + (u - 0x59));
		if (u == 0x62)
			return '0';
		if (u == 0x63)
			return '.';
	}
	return 0;
}

#endif
