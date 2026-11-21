/*
 * sockdump: every process's IPv4/IPv6 sockets, said each time they change -- for what an application's connections
 * do when no shell can be opened on the phone (Safari "the server stopped responding" on Wi-Fi, 2026-11-21).
 *
 * sockdump SECONDS [NAME...]: for SECONDS, once a second, every TCP and UDP socket of the processes (all, or those whose
 * name begins with one of NAME) -- local and remote address, the TCP state, the bytes waiting in each direction, the
 * local address says which interface) -- printed when it appears, when it changes, and when it goes.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xnuhdr/libproc.h"

#define MAX_SOCKS 1024

static const char *const tcp_states[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED", "CLOSE_WAIT",
	"FIN_WAIT_1", "CLOSING", "LAST_ACK", "FIN_WAIT_2", "TIME_WAIT" };

struct seen {
	int pid, fd, live;
	char line[320];
};
static struct seen seen[MAX_SOCKS], now[MAX_SOCKS];
static int nseen, nnow;

static void
addr(char *out, size_t size, const struct in_sockinfo *in, int local)
{
	char a[INET6_ADDRSTRLEN] = "?";
	int port = ntohs(local ? in->insi_lport : in->insi_fport);

	if (in->insi_vflag & INI_IPV4) {
		inet_ntop(AF_INET, local ? (const void *)&in->insi_laddr.ina_46.i46a_addr4 :
		    (const void *)&in->insi_faddr.ina_46.i46a_addr4, a, sizeof(a));
	} else {
		inet_ntop(AF_INET6, local ? (const void *)&in->insi_laddr.ina_6 : (const void *)&in->insi_faddr.ina_6, a, sizeof(a));
	}
	snprintf(out, size, "%s:%d", a, port);
}

static int
wanted(const char *name, int argc, char **argv)
{
	if (argc <= 2) {
		return 1;
	}
	for (int i = 2; i < argc; i++) {
		if (strncmp(name, argv[i], strlen(argv[i])) == 0) {
			return 1;
		}
	}
	return 0;
}

static void
scan(int argc, char **argv)
{
	static int pids[4096];
	int n = proc_listallpids(pids, sizeof(pids));

	nnow = 0;
	for (int i = 0; i < n; i++) {
		char name[2 * MAXCOMLEN + 1] = "";
		struct proc_fdinfo fds[512];
		int size;

		proc_name(pids[i], name, sizeof(name));
		if (!wanted(name, argc, argv)) {
			continue;
		}
		size = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, fds, sizeof(fds));
		for (int f = 0; f < size / (int)sizeof(fds[0]) && nnow < MAX_SOCKS; f++) {
			struct socket_fdinfo si;
			struct in_sockinfo *in;
			char l[64], r[64];
			const char *what;

			if (fds[f].proc_fdtype != PROX_FDTYPE_SOCKET ||
			    proc_pidfdinfo(pids[i], fds[f].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof(si)) != sizeof(si)) {
				continue;
			}
			if (si.psi.soi_kind == SOCKINFO_TCP) {
				int s = si.psi.soi_proto.pri_tcp.tcpsi_state;

				in = &si.psi.soi_proto.pri_tcp.tcpsi_ini;
				what = s >= 0 && s <= 10 ? tcp_states[s] : "?";
			} else if (si.psi.soi_kind == SOCKINFO_IN) {
				in = &si.psi.soi_proto.pri_in;
				what = "UDP";
			} else {
				continue;
			}
			addr(l, sizeof(l), in, 1);
			addr(r, sizeof(r), in, 0);
			snprintf(now[nnow].line, sizeof(now[nnow].line), "%s[%d] fd %d: %s %s -> %s, in %u out %u bytes",
			    name, pids[i], fds[f].proc_fd, what, l, r, si.psi.soi_rcv.sbi_cc, si.psi.soi_snd.sbi_cc);
			now[nnow].pid = pids[i];
			now[nnow].fd = fds[f].proc_fd;
			nnow++;
		}
	}
}

int
main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 60;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("sockdump: for %d s\n", seconds);
	for (int t = 0; t < seconds; t++) {
		scan(argc, argv);
		for (int i = 0; i < nnow; i++) {
			int j;

			for (j = 0; j < nseen; j++) {
				if (seen[j].pid == now[i].pid && seen[j].fd == now[i].fd) {
					break;
				}
			}
			if (j == nseen || strcmp(seen[j].line, now[i].line) != 0) {
				printf("sockdump: %3d s: %s\n", t, now[i].line);
			}
		}
		for (int j = 0; j < nseen; j++) {
			int i;

			for (i = 0; i < nnow; i++) {
				if (seen[j].pid == now[i].pid && seen[j].fd == now[i].fd) {
					break;
				}
			}
			if (i == nnow) {
				printf("sockdump: %3d s: GONE %s\n", t, seen[j].line);
			}
		}
		memcpy(seen, now, sizeof(now[0]) * nnow);
		nseen = nnow;
		sleep(1);
	}
	return 0;
}
