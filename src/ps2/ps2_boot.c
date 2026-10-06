// PS2 start-up: IOP modules (embedded IRX from libps2_drivers), arguments, storage roots, exit.
// The module order matters: sio2man/padman first (libpad hangs in EXECCMD if USB/BDM went first,
// PLAN 4.3a), then iomanX/fileXio, poweroff, cdfs. PCSX2's host: is not reachable from the IOP loader,
// so every IRX comes from the ELF (SifExecModuleBuffer).
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <libpwroff.h>
#include <libcdvd.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ps2_boot.h"

// embedded modules (libps2_drivers)
extern unsigned char sio2man_irx[], padman_irx[], iomanX_irx[], fileXio_irx[], poweroff_irx[], cdfs_irx[];
#define IRX_SIZE __attribute__((section(".data"))) /* PS2-92: -G8 would address a 4-byte extern through $gp; the blob objects keep it in .data */
extern unsigned int IRX_SIZE size_sio2man_irx, size_padman_irx, size_iomanX_irx, size_fileXio_irx, size_poweroff_irx, size_cdfs_irx;
extern unsigned char audsrv_irx[];
extern unsigned int IRX_SIZE size_audsrv_irx;

ps2boot_info_t ps2boot;

static volatile int power_requested;
static boolean poweroff_allowed = true;
static boolean poweroff_forced;

static boolean HasFlag(int argc, char **argv, const char *flag)
{
	int i;
	for (i = 0; i < argc; i++)
		if (argv[i] && !strcasecmp(argv[i], flag))
			return true;
	return false;
}

// "dev:" prefix means a program path; PCSX2 -elf passes no program name at all (argv[0] is the
// first -gameargs token), a real loader passes e.g. "cdrom0:\SRB2.ELF;1" or "mass:/SRB2.ELF"
static boolean LooksLikePath(const char *s)
{
	const char *c;
	if (!s || !*s || *s == '-' || *s == '+' || *s == '@')
		return false;
	c = strchr(s, ':');
	return c && c != s && (c - s) <= 10 && !strchr(s, ' ');
}

static void Normalise(char *s)
{
	for (; *s; s++)
		if (*s == '\\')
			*s = '/';
}

// Splits "dev:rest" into the device root and the directory of rest (rest names a file unless isdir)
static void SplitBootPath(const char *path, boolean isdir, char *datadir, size_t size)
{
	char tmp[PS2BOOT_PATHMAX], *colon, *slash;
	size_t n;

	strlcpy(tmp, path, sizeof tmp);
	Normalise(tmp);
	colon = strchr(tmp, ':');
	if (!colon)
	{
		strlcpy(datadir, "host:", size);
		return;
	}
	slash = isdir ? tmp + strlen(tmp) : strrchr(colon + 1, '/');
	if (!slash)
		slash = colon + 1;
	*slash = '\0';
	// no trailing separator: the engine joins with "%s/%s"
	for (n = strlen(tmp); n > (size_t)(colon - tmp) + 1 && tmp[n - 1] == '/'; n--)
		tmp[n - 1] = '\0';
	if (!strncasecmp(tmp, "cdrom", 5))
		snprintf(datadir, size, "cdfs:%s", colon + 1); // read through cdfs.irx, see LoadModules
	else
		strlcpy(datadir, tmp, size);
}

static boolean IsRoot(const char *dir, const char *name)
{
	return !strncasecmp(dir, name, strlen(name));
}

static void ChoosePaths(int argc, char **argv)
{
	char cwd[PS2BOOT_PATHMAX];
	const boolean haveprog = argc > 0 && LooksLikePath(argv[0]);

	if (haveprog)
	{
		strlcpy(ps2boot.bootpath, argv[0], sizeof ps2boot.bootpath);
		SplitBootPath(argv[0], false, ps2boot.datadir, sizeof ps2boot.datadir);
	}
	else if (getcwd(cwd, sizeof cwd) && LooksLikePath(cwd))
	{
		strlcpy(ps2boot.bootpath, cwd, sizeof ps2boot.bootpath);
		SplitBootPath(cwd, true, ps2boot.datadir, sizeof ps2boot.datadir);
	}
	else
	{
		strlcpy(ps2boot.bootpath, "host:", sizeof ps2boot.bootpath);
		strlcpy(ps2boot.datadir, "host:", sizeof ps2boot.datadir);
	}

	ps2boot.host = IsRoot(ps2boot.datadir, "host");

	// Saves, config, replays: next to the data when that device is writable. A disc is not;
	// the memory card is the plan (PLAN 9) but its modules are not wired yet.
	if (IsRoot(ps2boot.datadir, "cdfs"))
	{
		strlcpy(ps2boot.homedir, "mc0:", sizeof ps2boot.homedir);
		printf("PS2BOOT WARNING: boot device is read-only; home is mc0: but mcman/mcserv are not loaded yet\n");
	}
	else
		strlcpy(ps2boot.homedir, ps2boot.datadir, sizeof ps2boot.homedir);
}

static void PowerCallback(void *arg)
{
	(void)arg;
	power_requested = 1; // the main loop sees it in I_OsPolling and goes through I_Quit
}

static INT32 LoadModule(const char *name, void *irx, unsigned int size)
{
	int ret = 0, id = SifExecModuleBuffer(irx, size, 0, NULL, &ret);
	printf("PS2BOOT module %s id=%d ret=%d\n", name, id, ret);
	if (id < 0 || ret == 1) // 1 = module did not stay resident
		return -1;
	return id;
}

boolean PS2Boot_LoadAudio(void)
{
	static boolean ready, attempted;
	int id;
	if (attempted) return ready;
	attempted = true;
	// Older ROM LOADFILE servers do not implement SEARCH_MOD_BY_NAME. They
	// can return an unrelated positive value, which is not a resident module
	// ID. This process owns audio loading; load once rather than guessing from
	// that unsupported RPC (audsrv_init otherwise binds forever to no server).
	id = SifLoadModule("rom0:LIBSD", 0, NULL);
	printf("PS2BOOT module rom0:LIBSD id=%d\n", id);
	if (id <= 0) return false;
	ready = LoadModule("audsrv", audsrv_irx, size_audsrv_irx) > 0;
	return ready;
}

static void LoadModules(boolean cdrom)
{
	ps2boot.sio2man = LoadModule("sio2man", sio2man_irx, size_sio2man_irx);
	ps2boot.padman = LoadModule("padman", padman_irx, size_padman_irx);

	ps2boot.iomanx = LoadModule("iomanX", iomanX_irx, size_iomanX_irx);
	if (ps2boot.iomanx > 0)
	{
		ps2boot.fileXio = LoadModule("fileXio", fileXio_irx, size_fileXio_irx);
		if (ps2boot.fileXio > 0 && fileXioInit() < 0)
			ps2boot.fileXio = -1;
	}

	ps2boot.poweroff = LoadModule("poweroff", poweroff_irx, size_poweroff_irx);
	if (ps2boot.poweroff > 0)
	{
		ps2boot.poweroff_ok = poweroffInit() >= 0;
		if (ps2boot.poweroff_ok)
			poweroffSetCallback(PowerCallback, NULL);
		printf("PS2BOOT poweroffInit ok=%d\n", ps2boot.poweroff_ok);
	}

	if (cdrom && ps2boot.fileXio > 0)
	{
		sceCdInit(SCECdINIT);
		ps2boot.cdfs = LoadModule("cdfs", cdfs_irx, size_cdfs_irx);
	}
}

static void AddArg(char ***v, int *n, int *cap, const char *s)
{
	if (*n + 1 >= *cap)
	{
		*cap = *cap ? *cap * 2 : 16;
		*v = realloc(*v, *cap * sizeof (char *));
		if (!*v)
		{
			printf("PS2BOOT FATAL: out of memory for arguments\n");
			exit(1);
		}
	}
	(*v)[(*n)++] = strdup(s);
	(*v)[*n] = NULL;
}

// one argument per line; empty lines and '#' comments are skipped
static int ReadArgFile(const char *path, char ***v, int *n, int *cap)
{
	char line[512];
	int added = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
	{
		char *s = line, *e = line + strlen(line);
		while (*s && isspace((unsigned char)*s))
			s++;
		while (e > s && isspace((unsigned char)e[-1]))
			*--e = '\0';
		if (!*s || *s == '#')
			continue;
		AddArg(v, n, cap, s);
		added++;
	}
	fclose(f);
	return added;
}

void PS2Boot_Init(int *argc, char ***argv)
{
	char **nv = NULL;
	int nn = 0, cap = 0, i, fromfile;
	char path[PS2BOOT_PATHMAX + 16];
	const boolean haveprog = *argc > 0 && LooksLikePath((*argv)[0]);
	boolean reset;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("PS2BOOT start argc=%d\n", *argc);
	sceSifInitRpc(0);

	ChoosePaths(*argc, *argv);
	poweroff_forced = HasFlag(*argc, *argv, "-poweroff");
	poweroff_allowed = !HasFlag(*argc, *argv, "-nopoweroff");

	// A real loader leaves the IOP in an unknown state: reboot it. Under host: (PCSX2, ps2link)
	// the IOP is fresh or must keep its host driver.
	reset = !ps2boot.host;
	if (HasFlag(*argc, *argv, "-iopreset"))
		reset = true;
	if (HasFlag(*argc, *argv, "-noiopreset"))
		reset = false;
	if (reset)
	{
		while (!SifIopReset("", 0)) {}
		while (!SifIopSync()) {}
		sceSifInitRpc(0);
		ps2boot.iopreset = true;
	}
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();
	LoadModules(IsRoot(ps2boot.datadir, "cdfs"));

	// arguments: program name, command line, then <datadir>/ps2args
	AddArg(&nv, &nn, &cap, haveprog ? (*argv)[0] : "SRB2.ELF");
	for (i = haveprog ? 1 : 0; i < *argc; i++)
		AddArg(&nv, &nn, &cap, (*argv)[i]);
	snprintf(path, sizeof path, "%s/ps2args", ps2boot.datadir);
	fromfile = ReadArgFile(path, &nv, &nn, &cap);

	*argc = nn;
	*argv = nv;
	printf("PS2BOOT boot=%s data=%s home=%s host=%d iopreset=%d args=%d (ps2args lines: %d)\n",
		ps2boot.bootpath, ps2boot.datadir, ps2boot.homedir, ps2boot.host, ps2boot.iopreset, nn, fromfile);
}

boolean PS2Boot_PowerRequested(void)
{
	return power_requested != 0;
}

void PS2Boot_Exit(INT32 code)
{
	const boolean off = ps2boot.poweroff_ok && poweroff_allowed && (ps2boot.host || poweroff_forced || power_requested);

	printf("PS2BOOT exit code=%d poweroff=%d\n", (int)code, off);
	fflush(NULL);
	if (off)
	{
		poweroffShutdown();
		SleepThread();
	}
	exit(code);
}

// PS2-73: PC sampler for the level load (-zsample, ps2_mem.c): EE timer 1 compare interrupt, the third argument of the INTC handler is the
// interrupted PC (PCSX2 raises it at the end of the running basic block: read the table as a ranking of functions). It lives in this unit because
// an interrupt handler must not reach its data through $gp, and this unit is built with -G0 (tools/ps2/opt_units.txt).
#include <timer.h>
#define SAMP_SLOTS 16384
static unsigned *samp_pc, *samp_cnt; // allocated by -zsample only: 128 KB of C heap, not of bss
static volatile unsigned samp_total, samp_dropped;
static int samp_id = -1, samp_running;

static int Samp_Handler(int cause, void *arg, void *addr)
{
	unsigned pc = (unsigned)(uintptr_t)addr, h;
	int n;

	(void)cause;
	(void)arg;
	*T1_MODE |= (1 << 10); // clear the compare flag
	samp_total++;
	h = (pc >> 2) & (SAMP_SLOTS - 1);
	for (n = 0; n < 64; n++, h = (h + 1) & (SAMP_SLOTS - 1))
	{
		if (samp_pc[h] == pc)
		{
			samp_cnt[h]++;
			return 0;
		}
		if (!samp_pc[h])
		{
			samp_pc[h] = pc;
			samp_cnt[h] = 1;
			return 0;
		}
	}
	samp_dropped++;
	return 0;
}

void PS2Boot_SampleStart(unsigned period)
{
	if (samp_running)
		return;
	if (!samp_pc)
	{
		samp_pc = calloc(SAMP_SLOTS, sizeof *samp_pc);
		samp_cnt = calloc(SAMP_SLOTS, sizeof *samp_cnt);
		if (!samp_pc || !samp_cnt)
		{
			free(samp_pc);
			free(samp_cnt);
			samp_pc = samp_cnt = NULL;
			return;
		}
	}
	samp_id = AddIntcHandler2(INTC_TIM1, Samp_Handler, -1, NULL);
	*T1_COUNT = 0;
	*T1_COMP = period;
	*T1_MODE = (1 << 7) | (1 << 8) | (1 << 6); // bus clock, count up, compare interrupt, reset on compare
	EnableIntc(INTC_TIM1);
	samp_running = 1;
}

void PS2Boot_SampleStop(unsigned *total, unsigned *dropped)
{
	if (samp_running)
	{
		DisableIntc(INTC_TIM1);
		*T1_MODE = 0;
		RemoveIntcHandler(INTC_TIM1, samp_id);
		samp_running = 0;
	}
	*total = samp_total;
	*dropped = samp_dropped;
}

int PS2Boot_SampleGet(unsigned i, unsigned *pc, unsigned *count)
{
	for (; samp_pc && i < SAMP_SLOTS; i++)
		if (samp_pc[i])
		{
			*pc = samp_pc[i];
			*count = samp_cnt[i];
			return (int)i + 1;
		}
	return 0;
}
