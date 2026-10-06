// PS2-110 (OPT6-F): -ftest-* diagnostic hooks of the content systems (see ps2_ftest.c)
#ifndef PS2_FTEST_H
#define PS2_FTEST_H

#include "../doomtype.h"

void PS2FTest_Level(void); // after a level was loaded: -ftest-level
void PS2FTest_Startup(void); // after the start-up files and add-ons are loaded; -ftest-quit ends the run
UINT32 PS2FTest_CRC32(UINT32 crc, const void *data, size_t size);

#endif
