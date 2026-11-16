/*
 * smsd: the 6T's received texts, from the kernel to the screen.
 *
 * The kernel takes each incoming text from the modem (QMI WMS, drivers/modem/hakios.mpss.c) and keeps the count and
 * the last one in sysctls: kern.hakios.sms_received, sms_from, sms_text. CommCenter, which hands texts to imagent and
 * Messages on an iPhone, does not run here (it speaks Apple's baseband protocol), so for now each new text is shown
 * as an alert through CFUserNotification (SpringBoard's SBUserNotification): the sender as the title, the text as
 * the message (signed with com.apple.springboard.CFUserNotification: src/smsd.entitlements). Texts that arrive while one is shown wait their turn (the count is followed, not only the last one:
 * if several arrive between two looks, the ones before the last are only counted).
 *
 * Without the sysctls (another phone, or sms=0) it ends at once.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* (in iOS's CoreFoundation, served by SpringBoard, but marked unavailable in the SDK's header: declared by its symbol) */
extern SInt32 notice(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef icon, CFURLRef sound, CFURLRef localization,
    CFStringRef header, CFStringRef message, CFStringRef button) __asm__("_CFUserNotificationDisplayNotice");
#define PLAIN_ALERT 3   /* kCFUserNotificationPlainAlertLevel */

static int
received(int *ok)
{
	int n = 0;
	size_t size = sizeof(n);

	if (sysctlbyname("kern.hakios.sms_received", &n, &size, NULL, 0) != 0) {
		*ok = 0;
	}
	return n;
}

static void
string(const char *name, char *out, size_t size)
{
	size_t length = size - 1;

	if (sysctlbyname(name, out, &length, NULL, 0) != 0) {
		length = 0;
	}
	out[length < size ? length : size - 1] = 0;
}

int
main(void)
{
	int ok = 1, seen;

	setvbuf(stdout, NULL, _IOLBF, 0);
	seen = received(&ok);
	if (!ok) {
		printf("smsd: no texts in this kernel (kern.hakios.sms_received): nothing to do\n");
		return 0;
	}
	printf("smsd: waiting for texts (%d so far)\n", seen);
	for (;;) {
		char from[64], text[400], title[96];
		int now = received(&ok), missed;
		CFStringRef header, message;

		if (now == seen) {
			sleep(1);
			continue;
		}
		missed = now - seen - 1;
		seen = now;
		string("kern.hakios.sms_from", from, sizeof(from));
		string("kern.hakios.sms_text", text, sizeof(text));
		if (missed > 0) {
			snprintf(title, sizeof(title), "Text from %s (and %d before it)", from, missed);
		} else {
			snprintf(title, sizeof(title), "Text from %s", from);
		}
		printf("smsd: text %d shown (from %s, %zu characters)\n", now, from, strlen(text));
		header = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);
		message = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
		if (message == NULL) {
			message = CFStringCreateWithCString(NULL, text, kCFStringEncodingISOLatin1);
		}
		/* (no wait for the button: the next text may come while this one is up) */
		printf("smsd: the notice returned %d\n", (int)notice(0, PLAIN_ALERT, NULL, NULL, NULL, header, message,
		    CFSTR("OK")));
		if (header != NULL) {
			CFRelease(header);
		}
		if (message != NULL) {
			CFRelease(message);
		}
	}
}
