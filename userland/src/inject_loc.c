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
#include <stdio.h>
#include <string.h>
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
