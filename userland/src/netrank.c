/*
 * netrank: the 6T's network services there, and its cellular interface (en1) ranked last among them, so that Wi-Fi
 * carries the traffic whenever it is joined.
 *
 * The services: configd configures an interface only through a network service of the current set in
 * /private/var/preferences/SystemConfiguration/preferences.plist, and iOS's Setup Assistant, which makes that set on a
 * real phone, never runs here. configd was left to make it itself, and some boots it did not: run 162303 had a
 * preferences.plist of nothing but its version, no service for en0 or en1, neither interface up although the radio
 * had joined and the data session was up -- Safari "offline", an MMS "Network is unreachable". So: once en0 or en1
 * exists, a current set ("Automatic") is made if there is none, and SCNetworkSetEstablishDefaultConfiguration() gives
 * it a service for each interface it lacks (DHCP for both), Wi-Fi first in the service order.
 *
 * The rank: en1 is an Ethernet interface to Darwin (drivers/modem/hakios.cell.c), and configd ranks wired Ethernet
 * ahead of Wi-Fi: with both up, every connection went out over cellular (run 120526: Safari "the server stopped
 * responding" on Wi-Fi, with a SIM whose data plan reaches only the carrier's own services). PrimaryRank Last -- what a
 * phone's cellular interface has -- is set on en1's service (kept in preferences.plist) and on the interface itself
 * (SCNetworkInterfaceSetPrimaryRank, asserted for as long as this lives): en1 is primary only when nothing else is up,
 * and still has its scoped route (MMS binds to en1 and keeps working on Wi-Fi).
 *
 * Kept running: both looked at again every minute (en1 may come and go with the data session).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef const struct __SCNetworkInterface *SCNetworkInterfaceRef;
typedef const struct __SCNetworkService *SCNetworkServiceRef;
typedef const struct __SCNetworkSet *SCNetworkSetRef;
typedef const struct __SCPreferences *SCPreferencesRef;
enum { kSCNetworkServicePrimaryRankDefault = 0, kSCNetworkServicePrimaryRankFirst = 1, kSCNetworkServicePrimaryRankLast = 2 };

extern SCNetworkInterfaceRef _SCNetworkInterfaceCreateWithBSDName(CFAllocatorRef allocator, CFStringRef bsdName, uint32_t flags);
extern Boolean SCNetworkInterfaceSetPrimaryRank(SCNetworkInterfaceRef interface, uint32_t rank);
extern uint32_t SCNetworkInterfaceGetPrimaryRank(SCNetworkInterfaceRef interface);
extern CFStringRef SCNetworkInterfaceGetBSDName(SCNetworkInterfaceRef interface);
extern SCPreferencesRef SCPreferencesCreate(CFAllocatorRef allocator, CFStringRef name, CFStringRef prefsID);
extern Boolean SCPreferencesLock(SCPreferencesRef prefs, Boolean wait);
extern Boolean SCPreferencesUnlock(SCPreferencesRef prefs);
extern Boolean SCPreferencesCommitChanges(SCPreferencesRef prefs);
extern Boolean SCPreferencesApplyChanges(SCPreferencesRef prefs);
extern SCNetworkSetRef SCNetworkSetCopyCurrent(SCPreferencesRef prefs);
extern SCNetworkSetRef SCNetworkSetCreate(SCPreferencesRef prefs);
extern Boolean SCNetworkSetSetName(SCNetworkSetRef set, CFStringRef name);
extern Boolean SCNetworkSetSetCurrent(SCNetworkSetRef set);
extern Boolean SCNetworkSetEstablishDefaultConfiguration(SCNetworkSetRef set);
extern CFArrayRef SCNetworkSetCopyServices(SCNetworkSetRef set);
extern Boolean SCNetworkSetSetServiceOrder(SCNetworkSetRef set, CFArrayRef newOrder);
extern SCNetworkInterfaceRef SCNetworkServiceGetInterface(SCNetworkServiceRef service);
extern CFStringRef SCNetworkServiceGetServiceID(SCNetworkServiceRef service);
extern uint32_t SCNetworkServiceGetPrimaryRank(SCNetworkServiceRef service);
extern Boolean SCNetworkServiceSetPrimaryRank(SCNetworkServiceRef service, uint32_t rank);
extern int SCError(void);
extern const char *SCErrorString(int status);

/* the service of `set` whose interface is `bsd`, or NULL (from `services`, SCNetworkSetCopyServices's) */
static SCNetworkServiceRef
service_of(CFArrayRef services, const char *bsd)
{
	CFStringRef name = CFStringCreateWithCString(NULL, bsd, kCFStringEncodingUTF8);
	SCNetworkServiceRef found = NULL;

	for (CFIndex i = 0; services != NULL && i < CFArrayGetCount(services) && found == NULL; i++) {
		SCNetworkServiceRef service = CFArrayGetValueAtIndex(services, i);
		SCNetworkInterfaceRef interface = SCNetworkServiceGetInterface(service);
		CFStringRef its = interface != NULL ? SCNetworkInterfaceGetBSDName(interface) : NULL;

		if (its != NULL && CFEqual(its, name)) {
			found = service;
		}
	}
	CFRelease(name);
	return found;
}

/* The services there (and en1's ranked last): 1 when they are (nothing to do, or done), 0 if not (yet) */
static int
services(void)
{
	int wifi = if_nametoindex("en0") != 0, cell = if_nametoindex("en1") != 0, changed = 0, ok = 0;
	SCPreferencesRef prefs;
	SCNetworkSetRef set;
	CFArrayRef list;
	SCNetworkServiceRef w, c;

	if (!wifi && !cell) {
		return 0;
	}
	prefs = SCPreferencesCreate(NULL, CFSTR("netrank"), NULL);
	if (prefs == NULL || !SCPreferencesLock(prefs, true)) {
		printf("netrank: the network preferences cannot be had: %s\n", SCErrorString(SCError()));
		if (prefs != NULL) {
			CFRelease(prefs);
		}
		return 0;
	}
	set = SCNetworkSetCopyCurrent(prefs);
	if (set == NULL) {
		set = SCNetworkSetCreate(prefs);
		if (set == NULL || !SCNetworkSetSetName(set, CFSTR("Automatic")) || !SCNetworkSetSetCurrent(set)) {
			printf("netrank: no current network set, and none could be made: %s\n", SCErrorString(SCError()));
			goto out;
		}
		printf("netrank: there was no current network set: \"Automatic\" made\n");
		changed = 1;
	}
	list = SCNetworkSetCopyServices(set);
	if ((wifi && service_of(list, "en0") == NULL) || (cell && service_of(list, "en1") == NULL)) {
		if (list != NULL) {
			CFRelease(list);
		}
		if (!SCNetworkSetEstablishDefaultConfiguration(set)) {
			printf("netrank: the set's default services could not be made: %s\n", SCErrorString(SCError()));
		}
		list = SCNetworkSetCopyServices(set);
		printf("netrank: services made for the interfaces the set lacked: now %ld (en0 %s, en1 %s)\n",
		    list != NULL ? (long)CFArrayGetCount(list) : 0L, service_of(list, "en0") ? "has one" : "has NONE",
		    service_of(list, "en1") ? "has one" : "has NONE");
		changed = 1;
		w = service_of(list, "en0");
		c = service_of(list, "en1");
		if (w != NULL && c != NULL) {
			const void *order[] = { SCNetworkServiceGetServiceID(w), SCNetworkServiceGetServiceID(c) };
			CFArrayRef ids = CFArrayCreate(NULL, order, 2, &kCFTypeArrayCallBacks);

			SCNetworkSetSetServiceOrder(set, ids);
			CFRelease(ids);
		}
	}
	c = service_of(list, "en1");
	if (c != NULL && SCNetworkServiceGetPrimaryRank(c) != kSCNetworkServicePrimaryRankLast) {
		SCNetworkServiceSetPrimaryRank(c, kSCNetworkServicePrimaryRankLast);
		printf("netrank: en1's service ranked last\n");
		changed = 1;
	}
	ok = (!wifi || service_of(list, "en0") != NULL) && (!cell || service_of(list, "en1") != NULL);
	if (list != NULL) {
		CFRelease(list);
	}
	if (changed && !(SCPreferencesCommitChanges(prefs) && SCPreferencesApplyChanges(prefs))) {
		printf("netrank: the network preferences could not be saved: %s\n", SCErrorString(SCError()));
		ok = 0;
	}
out:
	if (set != NULL) {
		CFRelease(set);
	}
	SCPreferencesUnlock(prefs);
	CFRelease(prefs);
	fflush(stdout);
	return ok;
}

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "en1";
	CFStringRef bsd = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	int said = -1;

	for (;;) {
		SCNetworkInterfaceRef interface = _SCNetworkInterfaceCreateWithBSDName(NULL, bsd, 0);
		int ok = -1, there = services();

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
		sleep(ok == 1 && there ? 60 : 5);
	}
}
