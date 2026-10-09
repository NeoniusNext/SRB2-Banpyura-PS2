// RSYS research probe (not for the product): logs every device-level read()/lseek() of the C library (_read/_lseek wrapped by the linker)
// so that the real number, size and order of IOP transfers of a run are known. "IO R fd pos n ret us" / "IO S fd off whence ret".
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <kernel.h>
#include <timer.h>

extern ssize_t __real__read(int fd, void *buf, size_t n);
extern off_t __real__lseek(int fd, off_t off, int whence);
extern int __real__open(const char *path, int flags, ...);
extern int __real__close(int fd);

unsigned long long ps2prof_sleep_cyc; // HEAD links ps2_prof.c only with --prof; the plain release build lacks this symbol (research note)
static long long fdpos[64];
static unsigned n_reads, n_seeks;
static unsigned long long bytes_read;
static int logon = 1;

ssize_t __wrap__read(int fd, void *buf, size_t n)
{
	unsigned long long t0 = GetTimerSystemTime();
	ssize_t r = __real__read(fd, buf, n);
	unsigned long long t1 = GetTimerSystemTime();

	if (fd >= 3 && fd < 64 && logon)
	{
		n_reads++;
		if (r > 0)
			bytes_read += (unsigned)r;
		printf("IO R %d %lld %u %d %u\n", fd, fdpos[fd], (unsigned)n, (int)r, (unsigned)((t1 - t0) * 1000000 / 147456000ULL));
		if (r > 0)
			fdpos[fd] += r;
	}
	return r;
}

off_t __wrap__lseek(int fd, off_t off, int whence)
{
	off_t r = __real__lseek(fd, off, whence);

	if (fd >= 3 && fd < 64 && logon)
	{
		n_seeks++;
		if (r >= 0)
			fdpos[fd] = r;
		printf("IO S %d %ld %d %ld\n", fd, (long)off, whence, (long)r);
	}
	return r;
}

void PS2IoLog_Summary(void)
{
	printf("IO SUM reads=%u seeks=%u bytes=%llu\n", n_reads, n_seeks, bytes_read);
}

int __wrap__open(const char *path, int flags, ...)
{
	int mode = 0666, fd;
	__builtin_va_list ap;

	__builtin_va_start(ap, flags);
	mode = __builtin_va_arg(ap, int);
	__builtin_va_end(ap);
	fd = __real__open(path, flags, mode);
	if (fd >= 3 && fd < 64 && logon)
	{
		fdpos[fd] = 0;
		printf("IO O %d %s\n", fd, path);
	}
	return fd;
}

int __wrap__close(int fd)
{
	if (fd >= 3 && fd < 64 && logon)
		printf("IO C %d\n", fd);
	return __real__close(fd);
}
