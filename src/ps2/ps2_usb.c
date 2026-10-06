// PS2-150 (OPT9-K): shared USB stack loader, see ps2_usb.h.
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>

#include <stdio.h>

#include "ps2_usb.h"

// embedded modules (libps2_drivers); the sizes stay in .data like in ps2_boot.c (PS2-92: -G8 would address a 4-byte extern through $gp)
extern unsigned char usbd_irx[], ps2kbd_irx[], ps2mouse_irx[];
#define IRX_SIZE __attribute__((section(".data")))
extern unsigned int IRX_SIZE size_usbd_irx, size_ps2kbd_irx, size_ps2mouse_irx;

static unsigned usb_mask;
static unsigned usb_loads;
static boolean usb_tried;

static boolean LoadModule(const char *name, void *irx, unsigned int size)
{
	int ret = 0, id = SifExecModuleBuffer(irx, size, 0, NULL, &ret);

	usb_loads++;
	printf("PS2USB module %s id=%d ret=%d\n", name, id, ret);
	return id >= 0 && ret != 1; // 1 = the module did not stay resident
}

unsigned PS2USB_Init(void)
{
	if (usb_tried)
		return usb_mask;
	usb_tried = true; // set first: a failed load is not repeated (it can leave the IOP in a state a second try does not fix)

	sceSifInitRpc(0);
	if (!LoadModule("usbd", usbd_irx, size_usbd_irx))
		return usb_mask;
	usb_mask |= PS2USB_STACK;
	if (LoadModule("ps2kbd", ps2kbd_irx, size_ps2kbd_irx))
		usb_mask |= PS2USB_KBD;
	if (LoadModule("ps2mouse", ps2mouse_irx, size_ps2mouse_irx))
		usb_mask |= PS2USB_MOUSE;
	printf("PS2USB ready mask=%u loads=%u\n", usb_mask, usb_loads);
	return usb_mask;
}

unsigned PS2USB_Modules(void)
{
	return usb_mask;
}

unsigned PS2USB_LoadCount(void)
{
	return usb_loads;
}
