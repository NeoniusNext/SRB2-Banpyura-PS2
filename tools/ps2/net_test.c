// Network probe (OPT6-S): brings up DEV9/SMAP/netman/ps2ip (EE lwIP) through libps2_drivers, gets an address,
// and exchanges UDP datagrams with an echo server on the host.
// usage (gameargs): <server-ip> [port] [static-ip gateway netmask]
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <ps2_eeip_driver.h>
#include <ps2_dev9_driver.h>
#include <ps2_smap_driver.h>
#include <ps2_netman_driver.h>
#include <ps2_network_driver.h>
#include <libpwroff.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

static void progress(enum EEIP_PROGRESS_EVENT ev, void *user)
{
	(void)user;
	printf("NT progress %d\n", (int)ev);
}

int main(int argc, char **argv)
{
	const char *srv = argc > 1 ? argv[argc - 2] : "192.168.1.2"; // PCSX2 -gameargs: argv[0] is the first token
	int port = argc > 1 ? atoi(argv[argc - 1]) : 5029;
	struct ip4_addr ip, nm, gw;
	eeip_network_config_t cfg;
	int s, i, rc, got = 0;
	struct sockaddr_in to, from;
	socklen_t fl;
	char buf[128];

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("NT start argc=%d srv=%s port=%d\n", argc, srv, port);
	sceSifInitRpc(0);
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();
	rc = init_eeip_driver(true);
	printf("NT init_eeip_driver=%d\n", rc);
	eeip_network_config_default_dhcp(&cfg);
	cfg.on_progress = progress;
	cfg.timeout_seconds = 20;
	rc = configure_eeip_network(&cfg);
	printf("NT configure=%d\n", rc);
	eeip_get_current_config(&ip, &nm, &gw);
	printf("NT ip=%s ", inet_ntoa(ip));
	printf("nm=%s ", inet_ntoa(nm));
	printf("gw=%s\n", inet_ntoa(gw));

	s = socket(AF_INET, SOCK_DGRAM, 0);
	printf("NT socket=%d\n", s);
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = inet_addr(srv);
	for (i = 0; i < 5 && !got; i++)
	{
		struct timeval tv = {1, 0};
		fd_set rf;
		snprintf(buf, sizeof buf, "hello from PS2 %d", i);
		rc = sendto(s, buf, strlen(buf), 0, (struct sockaddr *)&to, sizeof to);
		printf("NT sendto=%d errno=%d\n", rc, errno);
		FD_ZERO(&rf);
		FD_SET(s, &rf);
		rc = select(s + 1, &rf, NULL, NULL, &tv);
		printf("NT select=%d\n", rc);
		if (rc > 0)
		{
			fl = sizeof from;
			rc = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
			if (rc > 0)
			{
				buf[rc] = 0;
				printf("NT recv '%s' from %s:%d\n", buf, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
				got = 1;
			}
		}
	}
	printf("NT COMPLETE got=%d\n", got);
	poweroffInit();
	poweroffShutdown();
	SleepThread();
	return 0;
}
