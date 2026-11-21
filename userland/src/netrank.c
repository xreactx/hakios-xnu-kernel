/*
 * netrank: the 6T's cellular interface (en1) ranked last among the network services, so that Wi-Fi carries the traffic
 * whenever it is joined.
 *
 * en1 is an Ethernet interface to Darwin (drivers/modem/hakios.cell.c), and configd ranks wired Ethernet ahead of Wi-Fi:
 * with both up, every connection went out over cellular (run 120526: Safari "the server stopped responding" on Wi-Fi,
 * with a SIM whose data plan reaches only the carrier's own services). SCNetworkInterfaceSetPrimaryRank(en1, Last) --
 * what a phone's cellular interface has -- leaves en1 primary only when nothing else is up, and still gives it its scoped
 * route (MMS binds to en1 and keeps working on Wi-Fi). Kept running: the rank is asserted for as long as this lives, and
 * set again every minute (en1 may come and go with the data session).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <unistd.h>

typedef const struct __SCNetworkInterface *SCNetworkInterfaceRef;
enum { kSCNetworkServicePrimaryRankDefault = 0, kSCNetworkServicePrimaryRankFirst = 1, kSCNetworkServicePrimaryRankLast = 2 };
extern SCNetworkInterfaceRef _SCNetworkInterfaceCreateWithBSDName(CFAllocatorRef allocator, CFStringRef bsdName, uint32_t flags);
extern Boolean SCNetworkInterfaceSetPrimaryRank(SCNetworkInterfaceRef interface, uint32_t rank);
extern uint32_t SCNetworkInterfaceGetPrimaryRank(SCNetworkInterfaceRef interface);

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "en1";
	CFStringRef bsd = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	int said = -1;

	for (;;) {
		SCNetworkInterfaceRef interface = _SCNetworkInterfaceCreateWithBSDName(NULL, bsd, 0);
		int ok = -1;

		if (interface != NULL) {
			ok = SCNetworkInterfaceGetPrimaryRank(interface) == kSCNetworkServicePrimaryRankLast ||
			    SCNetworkInterfaceSetPrimaryRank(interface, kSCNetworkServicePrimaryRankLast);
			/* (kept: releasing the interface may end its rank assertion) */
		}
		if (ok != said) {
			said = ok;
			printf("netrank: %s: %s\n", name, ok < 0 ? "not there yet" : ok ? "ranked last (Wi-Fi first when it is up)" :
			    "THE RANK WAS NOT TAKEN");
			fflush(stdout);
		}
		sleep(ok == 1 ? 60 : 5);
	}
}
