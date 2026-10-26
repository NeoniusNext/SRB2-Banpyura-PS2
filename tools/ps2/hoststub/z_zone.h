#ifndef HOSTSTUB_ZONE
#define HOSTSTUB_ZONE
#include <stdlib.h>
#define PU_STATIC 1
#define Z_Malloc(size, tag, user) malloc(size)
#define Z_Free(p) free(p)
#endif
