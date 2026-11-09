/*
 * libhakios.bluetool.dylib: inside BlueTool only (etc/inject.conf), MobileGestalt's ReleaseType answered "Vendor".
 *
 * BlueTool boots the Bluetooth chip for bluetoothd ("boot" over XPC). It knows Apple's and Broadcom's chips by the
 * platform's ProductHash, and for any other it has no built-in script (run 180453: "fwPos = -1"). A script of the
 * platform's own, /etc/bluetool/<ProductHash>.boot.script, it takes only on a vendor or an internal install: in
 * runCommandForCurrentConfig it asks MGCopyAnswer("ReleaseType") and compares it with "Vendor" and "Internal"
 * (20F66, at 0x10001625c); a customer install's answer skips the override altogether. On the 6T the kernel does the
 * chip's boot itself (devices/fajita/soc/drivers/bt/hakios.bt6t.c, bt6t=5) and the script only opens the tty and
 * resets the controller (userland/bluetool.boot.script), so BlueTool, and only BlueTool, is told "Vendor". Every
 * other question but WifiVendor (below) goes to MobileGestalt as it is.
 */
#include <CoreFoundation/CoreFoundation.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);

/*
 * And WifiVendor answered "none" at once: BlueTool asks it (twice for each script) only to pick among Broadcom
 * modules, which this phone has not got, and MobileGestalt takes 5 s for each -- or, early in the boot, never
 * answers: BlueTool hung in the "boot" script, bluetoothd waited on it with its stack "busy", and logged that
 * hundreds of times a second over the screen (run 211622).
 */
static CFTypeRef
our_answer(CFStringRef key)
{
	if (key != NULL && CFEqual(key, CFSTR("ReleaseType"))) {
		return CFRetain(CFSTR("Vendor"));
	}
	if (key != NULL && CFEqual(key, CFSTR("WifiVendor"))) {
		return NULL;
	}
	return MGCopyAnswer(key);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} mg_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)our_answer, (const void *)MGCopyAnswer
};
