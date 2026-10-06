// Host test for src/ps2/ps2_kbd_map.h (PS2-151): dumps the key and text tables, tools/ps2/kbd_map_hosttest.py checks them.
#include <stdio.h>
#include "../../src/ps2/ps2_kbd_map.h"

int main(void)
{
	int u, shift, caps, num;

	for (u = 0; u < 256; u++)
		printf("K %d %d\n", u, PS2Kbd_UsageToKey((unsigned char)u));
	for (u = 0; u < 256; u++)
		for (shift = 0; shift < 2; shift++)
			for (caps = 0; caps < 2; caps++)
				for (num = 0; num < 2; num++)
					printf("T %d %d %d %d %d\n", u, shift, caps, num, (int)(unsigned char)PS2Kbd_UsageToText((unsigned char)u, shift, caps, num));
	return 0;
}
