/*
 * calld: the 6T's ringing screen.
 *
 * The kernel follows the modem's calls (QMI VOICE, drivers/modem/hakios.mpss.c) and keeps the state of the last one
 * in sysctls: kern.hakios.call_state (0 none, 1 dialling, 2 incoming, 3 talking, 4 on hold, 5 ringing there, 9 ended)
 * and kern.hakios.call_number. CommCenter and the Phone app do not run here (CommCenter speaks Apple's baseband
 * protocol), so an incoming call is shown as an alert through CFUserNotification (SpringBoard's SBUserNotification,
 * signed with com.apple.springboard.CFUserNotification: src/calld.entitlements), with Answer and Decline, while a
 * ringtone plays; once answered, an alert with End stays up for the call. A call made from the dialer (src/PhonePad.m,
 * through kern.hakios.call_dial) is shown as "Calling..." with End while it rings at the other end. What is pressed goes back to the kernel
 * through kern.hakios.call_action (1 answer, 2 end); the kernel then has the modem answer or end the call, and the
 * call's audio goes through the audio DSP (drivers/audio/hakios.mic.c). An alert is taken down when the call ends on
 * its own (the other side hangs up).
 *
 * Without the sysctls (another phone, or no modem) it ends at once.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* (in iOS's CoreFoundation, served by SpringBoard, but not in the SDK's headers: declared by their symbols) */
typedef struct __CFUserNotification *UserNotificationRef;
extern UserNotificationRef notification_create(CFAllocatorRef allocator, CFTimeInterval timeout, CFOptionFlags flags,
    SInt32 *error, CFDictionaryRef dictionary) __asm__("_CFUserNotificationCreate");
extern SInt32 notification_response(UserNotificationRef notification, CFTimeInterval timeout, CFOptionFlags *response)
    __asm__("_CFUserNotificationReceiveResponse");
extern SInt32 notification_cancel(UserNotificationRef notification) __asm__("_CFUserNotificationCancel");
#define NOTE_ALERT              1       /* kCFUserNotificationNoteAlertLevel */
#define DEFAULT_RESPONSE        0       /* kCFUserNotificationDefaultResponse: the first button */
#define ALTERNATE_RESPONSE      1       /* kCFUserNotificationAlternateResponse: the second */

#define RINGTONE "/Library/Ringtones/Opening.m4r"

static int
call_state(int *ok)
{
	int state = 0;
	size_t size = sizeof(state);

	if (sysctlbyname("kern.hakios.call_state", &state, &size, NULL, 0) != 0) {
		*ok = 0;
	}
	return state;
}

static void
call_number(char *out, size_t size)
{
	size_t length = size - 1;

	if (sysctlbyname("kern.hakios.call_number", out, &length, NULL, 0) != 0) {
		length = 0;
	}
	out[length < size ? length : size - 1] = 0;
	if (out[0] == 0) {
		strlcpy(out, "Unknown", size);
	}
}

static void
call_action(int action)
{
	if (sysctlbyname("kern.hakios.call_action", NULL, NULL, &action, sizeof(action)) != 0) {
		perror("calld: kern.hakios.call_action");
	}
}

/* An alert with one or two buttons; NULL if SpringBoard does not take it. */
static UserNotificationRef
alert(const char *header, const char *message, CFStringRef first, CFStringRef second)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef h = CFStringCreateWithCString(NULL, header, kCFStringEncodingUTF8);
	CFStringRef m = CFStringCreateWithCString(NULL, message, kCFStringEncodingUTF8);
	UserNotificationRef n;
	SInt32 error = 0;

	CFDictionarySetValue(d, CFSTR("AlertHeader"), h);
	CFDictionarySetValue(d, CFSTR("AlertMessage"), m);
	CFDictionarySetValue(d, CFSTR("DefaultButtonTitle"), first);
	if (second != NULL) {
		CFDictionarySetValue(d, CFSTR("AlternateButtonTitle"), second);
	}
	/* (SpringBoard's own keys: shown over the lock screen too, and not dismissed by the home gesture) */
	CFDictionarySetValue(d, CFSTR("SBUserNotificationDismissOnLock"), kCFBooleanFalse);
	CFDictionarySetValue(d, CFSTR("SBUserNotificationAllowMenuButtonDismissal"), kCFBooleanFalse);
	CFDictionarySetValue(d, CFSTR("AlertTopMost"), kCFBooleanTrue);
	n = notification_create(NULL, 0, NOTE_ALERT, &error, d);
	if (n == NULL) {
		printf("calld: the alert \"%s\" was not taken (error %d)\n", header, (int)error);
	}
	CFRelease(d);
	CFRelease(h);
	CFRelease(m);
	return n;
}

/* ---- the ringtone: played again whenever it ends, while the call rings ---- */

static SystemSoundID ringtone;
static atomic_int ringing, ring_playing;

static void
ring_once(void)
{
	atomic_store(&ring_playing, 1);
	AudioServicesPlayAlertSoundWithCompletion(ringtone, ^{
		atomic_store(&ring_playing, 0);
	});
}

static void
ring_start(void)
{
	if (ringtone == 0) {
		CFURLRef url = CFURLCreateWithFileSystemPath(NULL, CFSTR(RINGTONE), kCFURLPOSIXPathStyle, false);
		OSStatus status = AudioServicesCreateSystemSoundID(url, &ringtone);

		CFRelease(url);
		if (status != 0) {
			printf("calld: the ringtone %s cannot be had (%d): the call rings in silence\n", RINGTONE, (int)status);
			ringtone = 0;
			return;
		}
	}
	atomic_store(&ringing, 1);
	ring_once();
}

static void
ring_tick(void)
{
	if (ringtone != 0 && atomic_load(&ringing) && !atomic_load(&ring_playing)) {
		ring_once();
	}
}

static void
ring_stop(void)
{
	atomic_store(&ringing, 0);
	if (ringtone != 0) {
		/* (disposing of the sound stops it where it is) */
		AudioServicesDisposeSystemSoundID(ringtone);
		ringtone = 0;
	}
}

/* ---- the screens ---- */

/* The incoming call's alert, until a button is pressed or the call stops ringing. */
static void
incoming(void)
{
	char number[64], message[128];
	UserNotificationRef n;
	int ok = 1;

	call_number(number, sizeof(number));
	snprintf(message, sizeof(message), "%s\n(VoLTE)", number);
	printf("calld: an incoming call from %s: ringing\n", number);
	n = alert("Incoming Call", message, CFSTR("Answer"), CFSTR("Decline"));
	ring_start();
	for (;;) {
		CFOptionFlags response = 0;
		int state;

		if (n != NULL && notification_response(n, 0.25, &response) == 0) {
			ring_stop();
			if ((response & 3) == DEFAULT_RESPONSE) {
				printf("calld: Answer pressed\n");
				call_action(1);
			} else {
				printf("calld: Decline pressed\n");
				call_action(2);
			}
			break;
		}
		if (n == NULL) {
			usleep(250000);
		}
		ring_tick();
		state = call_state(&ok);
		if (state != 2) {
			printf("calld: the call stopped ringing on its own (state %d)\n", state);
			ring_stop();
			if (n != NULL) {
				notification_cancel(n);
			}
			break;
		}
	}
	if (n != NULL) {
		CFRelease(n);
	}
}

/* The call's alert while it talks, until End is pressed or the call ends. */
static void
talking(void)
{
	char number[64], message[128];
	UserNotificationRef n;
	int ok = 1;

	call_number(number, sizeof(number));
	snprintf(message, sizeof(message), "%s", number);
	printf("calld: on the call with %s\n", number);
	n = alert("On Call", message, CFSTR("End"), NULL);
	for (;;) {
		CFOptionFlags response = 0;
		int state;

		if (n != NULL && notification_response(n, 0.25, &response) == 0) {
			printf("calld: End pressed\n");
			call_action(2);
			break;
		}
		if (n == NULL) {
			usleep(250000);
		}
		state = call_state(&ok);
		if (state != 3 && state != 4) {
			printf("calld: the call ended (state %d)\n", state);
			if (n != NULL) {
				notification_cancel(n);
			}
			break;
		}
	}
	if (n != NULL) {
		CFRelease(n);
	}
}

/* An outgoing call's alert while it is dialled and rings there, until End is pressed or it is answered or ends. */
static void
outgoing(void)
{
	char number[64];
	UserNotificationRef n;
	int ok = 1;

	call_number(number, sizeof(number));
	printf("calld: calling %s\n", number);
	n = alert("Calling...", number, CFSTR("End"), NULL);
	for (;;) {
		CFOptionFlags response = 0;
		int state;

		if (n != NULL && notification_response(n, 0.25, &response) == 0) {
			printf("calld: End pressed while calling\n");
			call_action(2);
			break;
		}
		if (n == NULL) {
			usleep(250000);
		}
		state = call_state(&ok);
		if (state != 1 && state != 5) {
			printf("calld: the outgoing call is %s (state %d)\n", state == 3 ? "answered" : "over", state);
			if (n != NULL) {
				notification_cancel(n);
			}
			break;
		}
	}
	if (n != NULL) {
		CFRelease(n);
	}
}

int
main(void)
{
	int ok = 1, last;

	setvbuf(stdout, NULL, _IOLBF, 0);
	last = call_state(&ok);
	if (!ok) {
		printf("calld: no calls in this kernel (kern.hakios.call_state): nothing to do\n");
		return 0;
	}
	printf("calld: waiting for calls\n");
	for (;;) {
		int state = call_state(&ok);

		if (state == 2 && last != 2) {
			incoming();
			state = call_state(&ok);
		}
		if ((state == 1 || state == 5) && last != 1 && last != 5) {
			outgoing();
			state = call_state(&ok);
		}
		if ((state == 3 || state == 4) && last != 3 && last != 4) {
			talking();
			state = call_state(&ok);
		}
		last = state;
		usleep(200000);
	}
}
