/*
 * libhakios.loc.dylib: the 6T has a GPS (the modem's: drivers/modem/hakios.mpss.c, gnss; its fixes reach locationd
 * through src/gnssd.m), and MobileGestalt does not know it. Settings shows no Location Services without its
 * capability, so in the programs that ask (etc/inject.conf: Settings, Maps, locationd) MobileGestalt's answers to
 * "location-services" and "gps" -- by name or by their obfuscated names, base64(md5("MGCopyAnswer" + name)) cut to
 * 22 characters -- are yes. Everything else goes to MobileGestalt as it is. The questions about location are said on
 * the console, once each, to see which a program asks.
 *
 * Inside Settings it also opens the tweak loader (src/inject_tweaks.c), which was the one library inserted there.
 *
 * Settings itself asks no MobileGestalt question through what can be replaced here (run 233506: only locationd's
 * "gps" was seen): its privacy pane (PrivacySettingsUI's PUIPrivacyController, in the shared cache) keeps whether
 * location services are available (-_cachedLocationServicesAvailableValue, from refreshLocationServicesAvailable:
 * CoreLocation's -[CLLocationManager locationServicesAvailable]) and hides Location Services when they are not. In
 * Settings both answer yes, replaced through the Objective-C runtime as the classes appear (the pane's framework is
 * loaded when the pane is first shown); what CoreLocation said is on the console.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern CFTypeRef MGCopyAnswerWithError(CFStringRef key, void *unknown, int *error);
extern Boolean MGGetBoolAnswer(CFStringRef key);

static const char *const yes_keys[] = {
	"location-services", "0pY9r1XBV1duZ8HO3tBvFg",
	"gps", "ZiSlYxYSkyaYImU3lrSXWw",
};

static void
say(const char *text)
{
	static int console = -1;

	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	if (console >= 0) {
		write(console, text, strlen(text));
	}
}

static bool
ours(CFStringRef key, const char *how)
{
	char name[64];

	if (key == NULL || !CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(yes_keys) / sizeof(yes_keys[0]); i++) {
		if (strcmp(name, yes_keys[i]) == 0) {
			static unsigned told;

			if (!(told & (1u << i))) {
				char line[160];
				const char *program = getprogname();

				told |= 1u << i;
				snprintf(line, sizeof(line), "libhakios.loc: %s asked MobileGestalt %s(\"%s\"): yes\n", program ? program : "?",
				    how, name);
				say(line);
			}
			return true;
		}
	}
	return false;
}

static CFTypeRef
our_copy(CFStringRef key)
{
	return ours(key, "MGCopyAnswer") ? CFRetain(kCFBooleanTrue) : MGCopyAnswer(key);
}

static CFTypeRef
our_copy_error(CFStringRef key, void *unknown, int *error)
{
	if (ours(key, "MGCopyAnswerWithError")) {
		if (error != NULL) {
			*error = 0;
		}
		return CFRetain(kCFBooleanTrue);
	}
	return MGCopyAnswerWithError(key, unknown, error);
}

static Boolean
our_bool(CFStringRef key)
{
	return ours(key, "MGGetBoolAnswer") ? true : MGGetBoolAnswer(key);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} mg_interpose[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)our_copy, (const void *)MGCopyAnswer },
	{ (const void *)our_copy_error, (const void *)MGCopyAnswerWithError },
	{ (const void *)our_bool, (const void *)MGGetBoolAnswer },
};


/*
 * The compass, inside the apps that ask for headings (etc/inject.conf: Compass). locationd has no magnetometer of its
 * own here, and its simulation takes no headings, so CoreLocation's heading API is answered in the app: +headingAvailable
 * yes, -startUpdatingHeading starts deliveries ten times a second to the manager's delegate
 * (locationManager:didUpdateHeading:), -heading is the last one. The heading is the azimuth of the phone's top edge from
 * the SLPI's geomagnetic rotation vector (kern.hakios.rv_x/y/z/w x 10^6: magnetometer and accelerometer fused and
 * calibrated; Android's getRotationMatrixFromVector + getOrientation: atan2(R[1], R[4])). It is magnetic north;
 * trueHeading is given the same (no declination model here: a few degrees in most places).
 */
static double heading_now = -1, heading_accuracy = 10;
static id heading_manager;
static dispatch_source_t heading_timer;
static Class heading_class;
static IMP original_start_heading, original_stop_heading;

static double
heading_magnetic(id self __unused, SEL selector __unused) { return heading_now; }

static double
heading_accuracy_get(id self __unused, SEL selector __unused) { return heading_accuracy; }

static double
heading_zero(id self __unused, SEL selector __unused) { return 0; }

static id
heading_timestamp(id self __unused, SEL selector __unused)
{
	return ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSDate"), sel_registerName("date"));
}

static id
heading_description(id self __unused, SEL selector __unused)
{
	char text[96];

	snprintf(text, sizeof(text), "magneticHeading %.1f trueHeading %.1f accuracy %.1f (HakiOS)", heading_now, heading_now,
	    heading_accuracy);
	return (id)CFAutorelease(CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8));
}

static int
rv_read(const char *name, int *ok)
{
	int v = 0;
	size_t size = sizeof(v);

	if (sysctlbyname(name, &v, &size, NULL, 0) != 0) {
		*ok = 0;
	}
	return v;
}

static id
heading_object(void)
{
	static id one;

	if (heading_class == Nil) {
		Class base = objc_getClass("CLHeading");

		if (base == Nil || (heading_class = objc_allocateClassPair(base, "HakiosHeading", 0)) == Nil) {
			return nil;
		}
		class_addMethod(heading_class, sel_registerName("magneticHeading"), (IMP)heading_magnetic, "d@:");
		class_addMethod(heading_class, sel_registerName("trueHeading"), (IMP)heading_magnetic, "d@:");
		class_addMethod(heading_class, sel_registerName("headingAccuracy"), (IMP)heading_accuracy_get, "d@:");
		class_addMethod(heading_class, sel_registerName("x"), (IMP)heading_zero, "d@:");
		class_addMethod(heading_class, sel_registerName("y"), (IMP)heading_zero, "d@:");
		class_addMethod(heading_class, sel_registerName("z"), (IMP)heading_zero, "d@:");
		class_addMethod(heading_class, sel_registerName("timestamp"), (IMP)heading_timestamp, "@@:");
		class_addMethod(heading_class, sel_registerName("description"), (IMP)heading_description, "@@:");
		objc_registerClassPair(heading_class);
	}
	if (one == nil) {
		one = ((id (*)(id, SEL))objc_msgSend)(class_createInstance(heading_class, 0), sel_registerName("init"));
	}
	return one;
}

static void
heading_tick(void)
{
	int ok = 1;
	double x = rv_read("kern.hakios.rv_x", &ok) / 1e6, y = rv_read("kern.hakios.rv_y", &ok) / 1e6,
	    z = rv_read("kern.hakios.rv_z", &ok) / 1e6, w = rv_read("kern.hakios.rv_w", &ok) / 1e6, r1, r4, azimuth;
	int samples = rv_read("kern.hakios.rv_samples", &ok), accuracy = rv_read("kern.hakios.rv_accuracy", &ok);
	id delegate, heading;
	SEL update = sel_registerName("locationManager:didUpdateHeading:");
	static int told;

	if (!ok || samples == 0 || heading_manager == nil) {
		return;
	}
	r1 = 2 * (x * y - z * w);
	r4 = 1 - 2 * (x * x + z * z);
	azimuth = atan2(r1, r4) * 180 / M_PI;
	heading_now = fmod(azimuth + 360, 360);
	{
		/* (the SLPI's calibration status as Apple's accuracy: unreliable 45 degrees, low 25, medium 15, high 5) */
		static const double by_status[4] = { 45, 25, 15, 5 };
		int status = rv_read("kern.hakios.rv_status", &ok);

		heading_accuracy = accuracy > 0 ? accuracy / 1e6 * 180 / M_PI : status >= 0 && status < 4 ? by_status[status] : 10;
	}
	if (heading_accuracy < 1) {
		heading_accuracy = 1;
	}
	if ((heading = heading_object()) == nil) {
		return;
	}
	delegate = heading_manager == (id)kCFNull ? nil : ((id (*)(id, SEL))objc_msgSend)(heading_manager, sel_registerName("delegate"));
	if (delegate != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(delegate, sel_registerName("respondsToSelector:"), update)) {
		((void (*)(id, SEL, id, id))objc_msgSend)(delegate, update, heading_manager, heading);
	}
	if (told++ % 100 == 0) {
		char line[96];

		snprintf(line, sizeof(line), "libhakios.loc: heading %.0f (accuracy %.0f, quaternion %.3f %.3f %.3f %.3f) given to the app\n",
		    heading_now, heading_accuracy, x, y, z, w);
		say(line);
	}
}


/*
 * Compass itself takes its heading from CoreMotion, not from the location manager: CompassMotionManager's -deviceMotion,
 * a CMDeviceMotion whose heading and gravity it draws (run 203411: the headings given to its CLLocationManager moved
 * nothing). Compass has a test motion of its own (CompassTestDeviceMotion: a heading, a gravity, a calibration level),
 * so -deviceMotion answers one of those, filled with the heading above and the accelerometer's gravity in g, as iOS
 * has it (pointing down: lying face up, z is -1; Android's axes give +9.8 there).
 */
struct gravity_vector {
	double x, y, z;
};

static id
compass_device_motion(id self __unused, SEL selector __unused)
{
	static id motion;
	Class test = objc_getClass("CompassTestDeviceMotion");
	int ok = 1;
	struct gravity_vector g;
	static int told;

	if (test == Nil) {
		return nil;
	}
	if (motion == nil) {
		motion = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(test, sel_registerName("alloc")),
		    sel_registerName("init"));
	}
	g.x = -rv_read("kern.hakios.accel_x", &ok) / 9807.0;
	g.y = -rv_read("kern.hakios.accel_y", &ok) / 9807.0;
	g.z = -rv_read("kern.hakios.accel_z", &ok) / 9807.0;
	if (heading_manager == nil) {
		heading_manager = (id)kCFNull;  /* (heading_tick() needs a manager only to tell its delegate: none here) */
	}
	heading_tick();
	((void (*)(id, SEL, struct gravity_vector))objc_msgSend)(motion, sel_registerName("setGravity:"), g);
	((void (*)(id, SEL, double))objc_msgSend)(motion, sel_registerName("setHeading:"), heading_now < 0 ? 0 : heading_now);
	if (told++ == 0) {
		say("libhakios.loc: Compass's device motion comes from the SLPI's rotation vector and accelerometer\n");
	}
	return motion;
}

static void
compass_start_motion(id self __unused, SEL selector __unused, unsigned long long frame __unused)
{
}

static bool
compass_motion_hooks(void)
{
	Class manager = objc_getClass("CompassMotionManager");
	Method m;

	if (manager == Nil) {
		return false;
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("deviceMotion"))) != NULL) {
		method_setImplementation(m, (IMP)compass_device_motion);
	}
	/* (CoreMotion has no motion processor here to start: what it would start is not wanted) */
	if ((m = class_getInstanceMethod(manager, sel_registerName("startDeviceMotionUpdatesUsingReferenceFrame:"))) != NULL) {
		method_setImplementation(m, (IMP)compass_start_motion);
	}
	say("libhakios.loc: Compass's motion manager answered from here\n");
	return true;
}

static BOOL
heading_available(id self __unused, SEL selector __unused)
{
	return YES;
}

static void
start_heading(id self, SEL selector)
{
	heading_manager = self;
	if (heading_timer == NULL) {
		heading_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
		dispatch_source_set_timer(heading_timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 10, NSEC_PER_SEC / 50);
		dispatch_source_set_event_handler(heading_timer, ^{
			heading_tick();
		});
		dispatch_resume(heading_timer);
		say("libhakios.loc: the app asks for headings: the SLPI's rotation vector gives them\n");
	}
	if (original_start_heading != NULL) {
		((void (*)(id, SEL))original_start_heading)(self, selector);
	}
}

static void
stop_heading(id self, SEL selector)
{
	if (heading_manager == self) {
		heading_manager = nil;
	}
	if (original_stop_heading != NULL) {
		((void (*)(id, SEL))original_stop_heading)(self, selector);
	}
}

static id
last_heading(id self __unused, SEL selector __unused)
{
	return heading_now >= 0 ? heading_object() : nil;
}

static bool
compass_hooks(void)
{
	Class manager = objc_getClass("CLLocationManager");
	Method m;
	int ok = 1;

	rv_read("kern.hakios.rv_samples", &ok);
	if (manager == Nil || !ok) {
		return manager != Nil;              /* (no rotation vector in this kernel: nothing to do once it is there) */
	}
	if ((m = class_getClassMethod(manager, sel_registerName("headingAvailable"))) != NULL) {
		method_setImplementation(m, (IMP)heading_available);
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("startUpdatingHeading"))) != NULL) {
		original_start_heading = method_setImplementation(m, (IMP)start_heading);
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("stopUpdatingHeading"))) != NULL) {
		original_stop_heading = method_setImplementation(m, (IMP)stop_heading);
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("heading"))) != NULL) {
		method_setImplementation(m, (IMP)last_heading);
	}
	say("libhakios.loc: CoreLocation's headings come from the SLPI in this app\n");
	return true;
}

static IMP original_available;

static id
yes_number(__unused id self, __unused SEL selector)
{
	return (id)kCFBooleanTrue;
}

static void
nothing(__unused id self, __unused SEL selector)
{
}

static bool
yes_available(id self, SEL selector)
{
	static bool told;

	if (!told && original_available != NULL) {
		char line[120];

		told = true;
		snprintf(line, sizeof(line), "libhakios.loc: CoreLocation says location services are %savailable: yes is answered\n",
		    ((bool (*)(id, SEL))original_available)(self, selector) ? "" : "NOT ");
		say(line);
	}
	return true;
}

static void
replace_classes(void)
{
	static bool privacy_done, location_done;
	Class c;
	Method m;

	if (!privacy_done && (c = objc_getClass("PUIPrivacyController")) != Nil) {
		privacy_done = true;
		if ((m = class_getInstanceMethod(c, sel_registerName("_cachedLocationServicesAvailableValue"))) != NULL) {
			method_setImplementation(m, (IMP)yes_number);
		}
		if ((m = class_getInstanceMethod(c, sel_registerName("refreshLocationServicesAvailable"))) != NULL) {
			method_setImplementation(m, (IMP)nothing);
		}
		say("libhakios.loc: Settings' privacy pane: location services available\n");
	}
	if (!location_done && (c = objc_getClass("CLLocationManager")) != Nil &&
	    (m = class_getInstanceMethod(c, sel_registerName("locationServicesAvailable"))) != NULL) {
		location_done = true;
		original_available = method_setImplementation(m, (IMP)yes_available);
	}
}


__attribute__((constructor)) static void
start(void)
{
	const char *program = getprogname();

	if (program != NULL && strcmp(program, "Compass") == 0) {
		dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
		static int tries;

		dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 10, NSEC_PER_SEC / 50);
		dispatch_source_set_event_handler(timer, ^{
			static bool location_done, motion_done;

			location_done = location_done || compass_hooks();
			motion_done = motion_done || compass_motion_hooks();
			if ((location_done && motion_done) || ++tries > 50) {
				dispatch_source_cancel(timer);
			}
		});
		dispatch_resume(timer);
	}
	if (program != NULL && strcmp(program, "Preferences") == 0) {
		if (access("/redwood/lib/libhakios.tweaks.dylib", R_OK) == 0) {
			dlopen("/redwood/lib/libhakios.tweaks.dylib", RTLD_NOW);
		}
		/* (looked for every half second for two minutes, off the main thread: the pane's framework comes when it is opened) */
		dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
		static int ticks;

		dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 2, NSEC_PER_SEC / 20);
		dispatch_source_set_event_handler(timer, ^{
			replace_classes();
			if (++ticks >= 240) {
				dispatch_source_cancel(timer);
			}
		});
		dispatch_resume(timer);
	}
}
