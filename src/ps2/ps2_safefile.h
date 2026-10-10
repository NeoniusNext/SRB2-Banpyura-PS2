// OPT13-IO (RS-08): saving files so that a power-off, a full card or a pulled stick cannot destroy the old copy.
// FIL_WriteFile used to open the file with "w+b" (the old content is gone at once) and nobody looked at the result of fwrite and fclose. Here the new content goes to
// NAME.tmp first; only a complete, flushed and closed temporary file replaces NAME: the old file moves to NAME.bak, the temporary file takes its name, NAME.bak is
// removed. Whatever moment the power goes, NAME or NAME.bak holds a whole file (PS2Safe_Recover puts NAME.bak back). A medium that cannot rename (the memory card
// driver, a host: device) makes the commit fail; the content is then written over NAME as before (the temporary file has told that there is room).
#ifndef PS2_SAFEFILE_H
#define PS2_SAFEFILE_H

#include <stdio.h>
#include <stddef.h>
#include "../doomtype.h"

// The operations the module uses (the real C library ones by default); a host test replaces them with ones that fail or "lose the power" at a chosen step.
typedef struct
{
	FILE *(*fopen)(const char *name, const char *mode);
	size_t (*fwrite)(const void *p, size_t size, size_t n, FILE *f);
	int (*fflush)(FILE *f);
	int (*fclose)(FILE *f);
	int (*rename)(const char *from, const char *to);
	int (*remove)(const char *name);
} ps2safe_ops_t;
void PS2Safe_SetOps(const ps2safe_ops_t *ops); // NULL: the C library

// Writes length bytes to name (replacing the old file as above). True when the new content is in the file.
boolean PS2Safe_Write(const char *name, const void *source, size_t length);

// Streamed writing (the config file): Begin opens NAME.tmp ("w": text) and returns it, End closes it, checks the stream and commits it to name; false means the new
// content did NOT arrive (the old file is untouched or was put back).
FILE *PS2Safe_Begin(const char *name, char *tmpname, size_t tmpsize);
boolean PS2Safe_End(FILE *f, const char *tmpname, const char *name);

// To be called before name is read: a power-off between the two renames of a commit leaves NAME.bak and no NAME; the old file comes back.
void PS2Safe_Recover(const char *name);

#endif
