// PS2-103 (OPT8-F): storage devices for add-ons and the auto-load folders (see ps2_addons.h).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <malloc.h>
#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <loadfile.h>

#include "../doomdef.h"
#include "../d_main.h"
#include "../m_argv.h"
#include "../i_system.h"
#include "ps2_boot.h"
#include "ps2_addons.h"
#include "ps2_usb.h"
#include "ps2_sys.h" // PS2_SleepUs

// 0 = not tried, 1 = usable, -1 = failed (never retried: a failed IRX load can leave the IOP in a state a second try does not fix)
static int mc_state, usb_state;

// An IRX read from <data>/modules and started from memory (works whatever device the data is on).
static boolean LoadIrx(const char *name, const char *args, int arglen)
{
	char path[PS2BOOT_PATHMAX + 40];
	FILE *f;
	long size;
	void *buf;
	int id, ret = 0;

	snprintf(path, sizeof path, "%s/modules/%s", ps2boot.datadir, name);
	f = fopen(path, "rb");
	if (!f)
	{
		CONS_Alert(CONS_WARNING, "PS2 add-on storage: driver %s is missing\n", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = size > 0 ? memalign(64, ((size_t)size + 63) & ~(size_t)63) : NULL;
	if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size)
	{
		CONS_Alert(CONS_WARNING, "PS2 add-on storage: cannot read %s\n", path);
		free(buf);
		fclose(f);
		return false;
	}
	fclose(f);
	id = SifExecModuleBuffer(buf, (u32)size, (u32)arglen, args, &ret);
	free(buf);
	CONS_Printf("PS2 add-on storage: module %s id=%d ret=%d\n", name, id, ret);
	return id >= 0 && ret != 1; // 1 = did not stay resident
}

static boolean DeviceUp(const char *root)
{
	DIR *d = opendir(root);

	if (!d)
		return false;
	closedir(d);
	return true;
}

static boolean PrepareMC(void)
{
	if (!mc_state)
		mc_state = (LoadIrx("mcman.irx", NULL, 0) && LoadIrx("mcserv.irx", NULL, 0)) ? 1 : -1;
	return mc_state > 0;
}

static boolean PrepareUSB(void)
{
	if (!usb_state)
	{
		boolean ok = (PS2USB_Init() & PS2USB_STACK) && LoadIrx("bdm.irx", NULL, 0) && LoadIrx("bdmfs_fatfs.irx", NULL, 0) // PS2-150: usbd is the shared embedded one
			&& LoadIrx("usbmass_bd.irx", NULL, 0);

		if (ok)
		{
			int i;

			for (i = 0; i < 20 && !DeviceUp("mass:/"); i++) // a stick needs a moment to be detected: up to ~10 s
				PS2_SleepUs(500 * 1000); // OPT13-IO (RS-06)
			ok = i < 20;
		}
		usb_state = ok ? 1 : -1;
	}
	return usb_state > 0;
}

boolean PS2Addons_Prepare(const char *path)
{
	if (!path)
		return true;
	if (!strncasecmp(path, "mc0:", 4) || !strncasecmp(path, "mc1:", 4))
		return PrepareMC();
	if (!strncasecmp(path, "mass:", 5) || !strncasecmp(path, "usb:", 4))
		return PrepareUSB();
	return true;
}

static boolean IsAddonName(const char *name)
{
	const char *dot = strrchr(name, '.');

	if (!dot || name[0] == '.')
		return false;
	return !strcasecmp(dot, ".pk3") || !strcasecmp(dot, ".wad") || !strcasecmp(dot, ".soc") || !strcasecmp(dot, ".lua");
}

static int CmpNames(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

size_t PS2Addons_Scan(const char *dir, void (*add)(const char *path, void *ctx), void *ctx)
{
	DIR *d;
	struct dirent *e;
	char **names = NULL;
	size_t n = 0, cap = 0, i;
	char full[512];

	if (!PS2Addons_Prepare(dir) || !(d = opendir(dir)))
		return 0;
	while ((e = readdir(d)))
	{
		if (!IsAddonName(e->d_name))
			continue;
		if (n == cap)
		{
			char **grown = realloc(names, (cap ? cap * 2 : 16) * sizeof *names);

			if (!grown)
				break;
			names = grown;
			cap = cap ? cap * 2 : 16;
		}
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	if (n > 1)
		qsort(names, n, sizeof *names, CmpNames);
	for (i = 0; i < n; i++)
	{
		snprintf(full, sizeof full, "%s/%s", dir, names[i]);
		add(full, ctx);
		free(names[i]);
	}
	free(names);
	return n;
}

void PS2Addons_Autoload(void (*add)(const char *path, void *ctx), void *ctx)
{
	char dir[512];
	size_t total = 0;
	int i;

	if (M_CheckParm("-noautoload"))
		return;
	snprintf(dir, sizeof dir, "%s/autoload", srb2path);
	total += PS2Addons_Scan(dir, add, ctx);
	{
		char homedir[512];

		snprintf(homedir, sizeof homedir, "%s/autoload", srb2home);
		if (strcmp(homedir, dir)) // the same folder twice would load every file twice
			total += PS2Addons_Scan(homedir, add, ctx);
	}
	for (i = 1; i < myargc; i++)
		if (!strcasecmp(myargv[i], "-autoload") && i + 1 < myargc)
			total += PS2Addons_Scan(myargv[i + 1], add, ctx);
	if (total)
		CONS_Printf("PS2 add-ons: %u auto-load file(s)\n", (unsigned)total);
}
