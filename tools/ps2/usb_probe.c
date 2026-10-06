/* OPT9-M: standalone USB HID probe for the PCSX2-usb stand (tools/ps2/usb_run.py). Loads usbd/ps2kbd/ps2mouse from host:, then for
   -secs N seconds prints one line for every mouse sample and keyboard raw key, plus the device counts once a second.
   Not part of the game: it proves that the drivers load and which events the emulated HID devices produce. */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <delaythread.h>
#include <libmouse.h>
#include <libkbd.h>
#include <timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 count(void) { u32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }

static int module(const char *path, const char *args, int len)
{
	int ret = 0, id = SifLoadStartModule(path, len, args, &ret);
	printf("USBPROBE module %s id=%d ret=%d\n", path, id, ret);
	return id;
}

int main(int argc, char **argv)
{
	int i, secs = 30, tick = 0, nm, mode = 0;
	u32 lastenum = 0xFFFFFFFF, lastbtn = 0xFFFFFFFF;
	PS2MouseData md;
	PS2KbdRawKey rk;

	for (i = 0; i < argc; i++)
		if (!strcmp(argv[i], "-secs") && i + 1 < argc)
			secs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-abs"))
			mode = 1;
	printf("USBPROBE start secs=%d mode=%d\n", secs, mode);
	sceSifInitRpc(0);
	module("host:usbd.irx", NULL, 0);
	module("host:ps2kbd.irx", NULL, 0);
	module("host:ps2mouse.irx", NULL, 0);
	DelayThread(500000);
	printf("USBPROBE PS2KbdInit=%d\n", PS2KbdInit());
	printf("USBPROBE PS2MouseInit=%d\n", PS2MouseInit());
	PS2KbdSetReadmode(PS2KBD_READMODE_RAW);
	PS2KbdSetBlockingMode(PS2KBD_NONBLOCKING);
	PS2MouseSetReadMode(mode ? PS2MOUSE_READMODE_ABS : PS2MOUSE_READMODE_DIFF);
	printf("USBPROBE mouse version=%u readmode=%u\n", PS2MouseGetVersion(), PS2MouseGetReadMode());
	for (; tick < secs * 100; tick++)
	{
		u32 e = PS2MouseEnum();
		if (e != lastenum)
		{
			printf("USBPROBE t=%d mouse count %u\n", tick, e);
			lastenum = e;
		}
		nm = PS2MouseRead(&md);
		if (md.x || md.y || md.wheel || md.buttons != lastbtn)
			printf("USBPROBE t=%d mouse ret=%d x=%d y=%d wheel=%d btn=%x\n", tick, nm, (int)md.x, (int)md.y, (int)md.wheel, (unsigned)md.buttons);
		lastbtn = md.buttons;
		while (PS2KbdReadRaw(&rk) > 0)
			printf("USBPROBE t=%d kbd state=%02x key=%02x\n", tick, rk.state, rk.key);
		DelayThread(10000);
	}
	{
		// cost of the blocking libmouse/libkbd RPCs in EE cycles (cop0 Count)
		u32 t0, n, sum;
		PS2KbdRawKey k2;

		for (sum = n = 0; n < 200; n++)
		{
			t0 = count();
			PS2MouseRead(&md);
			sum += count() - t0;
		}
		printf("USBPROBE cost PS2MouseRead avg %u cycles over 200\n", (unsigned)(sum / 200));
		for (sum = n = 0; n < 200; n++)
		{
			t0 = count();
			PS2MouseEnum();
			sum += count() - t0;
		}
		printf("USBPROBE cost PS2MouseEnum avg %u cycles over 200\n", (unsigned)(sum / 200));
		for (sum = n = 0; n < 200; n++)
		{
			t0 = count();
			PS2KbdReadRaw(&k2);
			sum += count() - t0;
		}
		printf("USBPROBE cost PS2KbdReadRaw avg %u cycles over 200\n", (unsigned)(sum / 200));
	}
	printf("USBPROBE done\n");
	return 0;
}
