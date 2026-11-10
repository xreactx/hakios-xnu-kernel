/*
 * devname: give the phone a computer name, once, when it has none.
 *
 * iOS's Setup Assistant names a new phone; it never runs here, so SystemConfiguration has no
 * ComputerName (mg_test: "ComputerName = (none)"), and Settings > Bluetooth said "This iPhone is
 * discoverable as "(null)"" -- bluetoothd's local name comes from it. This sets it through the same
 * API the Setup Assistant and Settings > General > About > Name use (SCPreferences, written to
 * /private/var/preferences/SystemConfiguration/preferences.plist, which var_store keeps), then applies
 * it so configd publishes it at once. A name already there -- one the user gave in Settings -- is left
 * as it is.
 *
 * usage: devname [NAME]     (default "iPhone", what MobileGestalt already answers for the device name)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef const struct __SCPreferences *SCPreferencesRef;
typedef const struct __SCDynamicStore *SCDynamicStoreRef;

/* SystemConfiguration (SCPreferencesSetSpecific.h, SCDynamicStoreCopySpecific.h: in the macOS headers only) */
extern SCPreferencesRef SCPreferencesCreate(CFAllocatorRef allocator, CFStringRef name, CFStringRef prefsID);
extern Boolean SCPreferencesLock(SCPreferencesRef prefs, Boolean wait);
extern Boolean SCPreferencesUnlock(SCPreferencesRef prefs);
extern Boolean SCPreferencesCommitChanges(SCPreferencesRef prefs);
extern Boolean SCPreferencesApplyChanges(SCPreferencesRef prefs);
extern Boolean SCPreferencesSetComputerName(SCPreferencesRef prefs, CFStringRef name, CFStringEncoding encoding);
extern CFStringRef SCDynamicStoreCopyComputerName(SCDynamicStoreRef store, CFStringEncoding *encoding);
extern CFDictionaryRef SCPreferencesPathGetValue(SCPreferencesRef prefs, CFStringRef path);
extern int SCError(void);
extern const char *SCErrorString(int status);

int
main(int argc, char *argv[])
{
	const char *wanted = argc > 1 ? argv[1] : "iPhone";
	CFStringRef name, new_name;
	CFDictionaryRef system;
	SCPreferencesRef prefs;
	char text[256] = "";
	int ok;

	/*
	 * The saved preferences are asked, not configd's store: configd may not be up yet, and a name the
	 * user gave must never be taken for none.
	 */
	prefs = SCPreferencesCreate(NULL, CFSTR("devname"), NULL);
	if (prefs == NULL) {
		printf("devname: no SystemConfiguration preferences: %s\n", SCErrorString(SCError()));
		return 1;
	}
	system = SCPreferencesPathGetValue(prefs, CFSTR("/System/System"));
	name = system != NULL ? CFDictionaryGetValue(system, CFSTR("ComputerName")) : NULL;
	if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID()) {
		CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
		printf("devname: the computer name is \"%s\": left as it is\n", text);
		CFRelease(prefs);
		return 0;
	}
	new_name = CFStringCreateWithCString(NULL, wanted, kCFStringEncodingUTF8);
	ok = SCPreferencesLock(prefs, TRUE) &&
	    SCPreferencesSetComputerName(prefs, new_name, kCFStringEncodingUTF8) &&
	    SCPreferencesCommitChanges(prefs) &&
	    SCPreferencesApplyChanges(prefs);
	if (!ok) {
		printf("devname: setting the computer name to \"%s\" failed: %s\n", wanted, SCErrorString(SCError()));
	}
	SCPreferencesUnlock(prefs);
	CFRelease(new_name);
	CFRelease(prefs);
	if (!ok) {
		return 1;
	}
	name = SCDynamicStoreCopyComputerName(NULL, NULL);
	if (name != NULL) {
		CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(name);
	}
	printf("devname: the computer name was not set: now \"%s\" (configd says \"%s\")\n", wanted, text);
	return 0;
}
