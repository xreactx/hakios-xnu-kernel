/*
 * gnssd: the 6T's GPS fixes, from the kernel to locationd.
 *
 * The kernel asks the modem's location service for a fix every second (gnss=1, drivers/modem/hakios.mpss.c) and
 * keeps the last one in sysctls: kern.hakios.gnss_fixes (how many so far), gnss_fix (0 none, 1 intermediate,
 * 2 final), gnss_lat_e7 / gnss_lon_e7 (degrees x 10^7), gnss_acc_cm, gnss_alt_cm, gnss_speed_cmps (-1 unknown),
 * gnss_heading_e1 (degrees x 10, -1 unknown) and gnss_time_ms (UTC).
 *
 * locationd has no GPS of its own to read here (its receiver is Apple's chip, behind its own driver). What
 * Xcode's "simulate location" uses is open to us instead: CoreLocation's CLSimulationManager hands locationd
 * locations that it then gives every application as its own, for a program with the entitlement
 * com.apple.locationd.simulation (src/gnssd.entitlements). Each new fix replaces the one before: cleared,
 * appended, flushed, started (repeat behaviour 0: no looping over the list).
 *
 * Without the sysctls (another phone, or gnss=0) it ends at once. Lines on the console: the first fix, then one
 * every 60 (the position rounded to about 1 km: the log travels).
 */
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <unistd.h>

@interface CLSimulationManager : NSObject
@property (assign, nonatomic) uint8_t locationRepeatBehavior;
- (void)clearSimulatedLocations;
- (void)appendSimulatedLocation:(CLLocation *)location;
- (void)flush;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
@end

static long long
number(const char *name, int *ok)
{
	long long q = 0;
	int i = 0;
	size_t size = sizeof(q);

	if (sysctlbyname(name, &q, &size, NULL, 0) == 0 && size == sizeof(q)) {
		return q;
	}
	size = sizeof(i);
	if (sysctlbyname(name, &i, &size, NULL, 0) == 0 && size == sizeof(i)) {
		return i;
	}
	*ok = 0;
	return 0;
}

int
main(void)
{
	@autoreleasepool {
		int ok = 1;
		long long seen = 0, given = 0;
		CLSimulationManager *simulation;

		setvbuf(stdout, NULL, _IOLBF, 0);
		number("kern.hakios.gnss_fixes", &ok);
		if (!ok) {
			printf("gnssd: no GPS in this kernel (kern.hakios.gnss_fixes): nothing to do\n");
			return 0;
		}
		simulation = [[NSClassFromString(@"CLSimulationManager") alloc] init];
		if (simulation == nil) {
			printf("gnssd: no CLSimulationManager in CoreLocation\n");
			return 1;
		}
		simulation.locationRepeatBehavior = 0;
		printf("gnssd: waiting for the kernel's first fix\n");
		for (;;) {
			long long fixes, lat, lon, acc, alt, speed, heading, when;
			NSDate *timestamp;
			CLLocation *location;

			ok = 1;
			fixes = number("kern.hakios.gnss_fixes", &ok);
			if (!ok || fixes == seen) {
				usleep(250000);
				continue;
			}
			seen = fixes;
			lat = number("kern.hakios.gnss_lat_e7", &ok);
			lon = number("kern.hakios.gnss_lon_e7", &ok);
			acc = number("kern.hakios.gnss_acc_cm", &ok);
			alt = number("kern.hakios.gnss_alt_cm", &ok);
			speed = number("kern.hakios.gnss_speed_cmps", &ok);
			heading = number("kern.hakios.gnss_heading_e1", &ok);
			when = number("kern.hakios.gnss_time_ms", &ok);
			if (!ok || acc <= 0) {
				continue;
			}
			/* (the fix's own time if it looks right, else now: the modem's clock is GPS time, the system's the loader's) */
			timestamp = when > 1700000000000LL ? [NSDate dateWithTimeIntervalSince1970:when / 1000.0] : [NSDate date];
			if (fabs(timestamp.timeIntervalSinceNow) > 30) {
				timestamp = [NSDate date];
			}
			location = [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(lat / 1e7, lon / 1e7)
			    altitude:alt / 100.0 horizontalAccuracy:acc / 100.0 verticalAccuracy:(acc / 100.0) * 1.5
			    course:heading >= 0 ? heading / 10.0 : -1 speed:speed >= 0 ? speed / 100.0 : -1 timestamp:timestamp];
			[simulation stopLocationSimulation];
			[simulation clearSimulatedLocations];
			[simulation appendSimulatedLocation:location];
			[simulation flush];
			[simulation startLocationSimulation];
			if (given++ % 60 == 0) {
				printf("gnssd: fix %lld given to locationd (about %.2f, %.2f; within %.0f m)\n", fixes, lat / 1e7, lon / 1e7,
				    acc / 100.0);
			}
		}
	}
}
