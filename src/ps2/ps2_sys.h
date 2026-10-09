// Internal interfaces between the PS2 system-layer files (i_system.c, i_joy.c, i_main.c).
#ifndef PS2_SYS_H
#define PS2_SYS_H

#include "../doomtype.h"

// i_joy.c
void PS2Joy_Prewarm(void);     // PS2-LOAD-3: opens libpad at once (main), so that the controller detection overlaps the engine start-up
void PS2Joy_Poll(void);        // both pads: read libpad, post engine events (called from I_OsPolling)
void PS2Joy_Shutdown(void);    // neutral events for everything held, pads released
void PS2Joy_WaitStart(void);   // block until Start is pressed (or the power button): fatal error screens

// i_system.c: sleep without the SDK alarm library (DelayThread): the thread drops to the lowest priority and reads the clock
void PS2_SleepUs(UINT32 us);

// i_video.c (optional): called by I_Error with the final message. A weak reference: if the video
// layer does not provide it the message only goes to the console log.
void PS2Video_FatalError(const char *message) __attribute__((weak));

#endif
