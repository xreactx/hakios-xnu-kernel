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
 * Each call is written down when it is over (the call history Phone Pad shows as Recents): a line of
 * /var/mobile/Library/HakiOS/calls.txt -- its start (seconds since 1970), its kind (in, missed, declined, out), the
 * number and how long it talked (s; 0: an outgoing call not answered). /var is kept from run to run (var_store).
 * The caller's name comes from iOS's contacts (src/calld_contacts.m).
 *
 * Without the sysctls (another phone, or no modem) it ends at once.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
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
#define OTHER_RESPONSE          2       /* kCFUserNotificationOtherResponse: the third */
#define AUDIO_SPEAKER           1       /* kern.hakios.call_audio's bits */
#define AUDIO_MUTE              2

#define RINGTONE "/Library/Ringtones/Opening.m4r"
#define HISTORY_DIR "/private/var/mobile/Library/HakiOS"
#define HISTORY HISTORY_DIR "/calls.txt"
#define MOBILE                  501     /* the user Phone Pad runs as */

extern int contact_name(const char *number, char *out, size_t size);     /* src/calld_contacts.m */

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

/* what an alert says of the other side: the contact's name and the number, or the number */
static void
caller(char *out, size_t size)
{
	char number[64], name[96];

	call_number(number, sizeof(number));
	if (strcmp(number, "Unknown") != 0 && contact_name(number, name, sizeof(name))) {
		snprintf(out, size, "%s\n%s", name, number);
	} else {
		strlcpy(out, number, size);
	}
}

/* ---- the call history ---- */

static struct {
	int pending;
	time_t start, talk_start, talk_end;
	char kind[12], number[64];
} record;

static void
record_begin(const char *kind)
{
	memset(&record, 0, sizeof(record));
	record.pending = 1;
	record.start = time(NULL);
	strlcpy(record.kind, kind, sizeof(record.kind));
	call_number(record.number, sizeof(record.number));
}

static void
record_kind(const char *kind)
{
	strlcpy(record.kind, kind, sizeof(record.kind));
}

static void
record_write(void)
{
	long talked = record.talk_end > record.talk_start ? (long)(record.talk_end - record.talk_start) : 0;
	FILE *f;

	if (!record.pending) {
		return;
	}
	record.pending = 0;
	(void)mkdir(HISTORY_DIR, 0755);
	(void)chown(HISTORY_DIR, MOBILE, MOBILE);
	f = fopen(HISTORY, "a");
	if (f == NULL) {
		perror("calld: " HISTORY);
		return;
	}
	fprintf(f, "%ld\t%s\t%s\t%ld\n", (long)record.start, record.kind, record.number, talked);
	fclose(f);
	(void)chown(HISTORY, MOBILE, MOBILE);
	(void)chmod(HISTORY, 0644);
	printf("calld: in the history: %s call, %s, %ld s\n", record.kind, record.number, talked);
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
alert3(const char *header, const char *message, CFStringRef first, CFStringRef second, CFStringRef third)
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
	if (third != NULL) {
		CFDictionarySetValue(d, CFSTR("OtherButtonTitle"), third);
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

static UserNotificationRef
alert(const char *header, const char *message, CFStringRef first, CFStringRef second)
{
	return alert3(header, message, first, second, NULL);
}

/* ---- the ringtone: played again whenever it ends, while the call rings ---- */

static SystemSoundID ringtone;
static atomic_int ringing, ring_playing;
static uint64_t buzzed_us;              /* the last vibration's start */

#define BUZZ_EVERY_US                   1500000         /* (as an iPhone's ring: a buzz, a pause) */

static uint64_t
now_us(void)
{
	return clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000;
}

/* the phone vibrates while it rings, through iOS's own vibration (the audio plugin's motor: /dev/hakios-haptic) */
static void
buzz(void)
{
	buzzed_us = now_us();
	AudioServicesPlaySystemSound(kSystemSoundID_Vibrate);
}

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
	atomic_store(&ringing, 1);
	buzz();
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
	if (atomic_load(&ringing) && now_us() - buzzed_us >= BUZZ_EVERY_US) {
		buzz();
	}
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
	char number[64], who[192], message[256];
	UserNotificationRef n;
	int ok = 1;

	call_number(number, sizeof(number));
	caller(who, sizeof(who));
	snprintf(message, sizeof(message), "%s\n(VoLTE)", who);
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
				record_kind("in");
				call_action(1);
			} else {
				printf("calld: Decline pressed\n");
				record_kind("declined");
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
			record_kind(state == 3 || state == 4 ? "in" : "missed");
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

static int
call_audio_get(void)
{
	int v = 0;
	size_t size = sizeof(v);

	(void)sysctlbyname("kern.hakios.call_audio", &v, &size, NULL, 0);
	return v;
}

static void
call_audio_set(int v)
{
	if (sysctlbyname("kern.hakios.call_audio", NULL, NULL, &v, sizeof(v)) != 0) {
		perror("calld: kern.hakios.call_audio");
	}
}

/*
 * The call's alert while it talks: End, Speaker (or Earpiece) and Mute (or Unmute), until End is pressed or the call
 * ends. A press of Speaker or Mute takes the alert down (as any button does): the change is asked for
 * (kern.hakios.call_audio) and the alert shown again as things now are.
 */
static void
talking(void)
{
	char number[192], message[256];
	int ok = 1;

	if (!record.pending) {
		record_begin("in");     /* (a call already up when calld started) */
	}
	record.talk_start = time(NULL);
	caller(number, sizeof(number));
	printf("calld: on the call with %s\n", record.number);
	for (;;) {
		int audio = call_audio_get(), done = 0;
		UserNotificationRef n;

		snprintf(message, sizeof(message), "%s\n%s%s", number, audio & AUDIO_SPEAKER ? "Speaker" : "Earpiece",
		    audio & AUDIO_MUTE ? ", muted" : "");
		n = alert3("On Call", message, CFSTR("End"), audio & AUDIO_SPEAKER ? CFSTR("Earpiece") : CFSTR("Speaker"),
		    audio & AUDIO_MUTE ? CFSTR("Unmute") : CFSTR("Mute"));
		for (;;) {
			CFOptionFlags response = 0;
			int state;

			if (n != NULL && notification_response(n, 0.25, &response) == 0) {
				if ((response & 3) == DEFAULT_RESPONSE) {
					printf("calld: End pressed\n");
					call_action(2);
					done = 1;
				} else if ((response & 3) == ALTERNATE_RESPONSE) {
					printf("calld: %s pressed\n", audio & AUDIO_SPEAKER ? "Earpiece" : "Speaker");
					call_audio_set(audio ^ AUDIO_SPEAKER);
				} else {
					printf("calld: %s pressed\n", audio & AUDIO_MUTE ? "Unmute" : "Mute");
					call_audio_set(audio ^ AUDIO_MUTE);
				}
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
				done = 1;
				break;
			}
		}
		if (n != NULL) {
			CFRelease(n);
		}
		if (done) {
			record.talk_end = time(NULL);
			return;
		}
		usleep(300000);         /* (the kernel takes the change; the alert comes back with it) */
	}
}

/* An outgoing call's alert while it is dialled and rings there, until End is pressed or it is answered or ends. */
static void
outgoing(void)
{
	char number[192];
	UserNotificationRef n;
	int ok = 1;

	caller(number, sizeof(number));
	printf("calld: calling %s\n", record.number);
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
			record_begin("missed");
			incoming();
			state = call_state(&ok);
		}
		if ((state == 1 || state == 5) && last != 1 && last != 5) {
			record_begin("out");
			outgoing();
			state = call_state(&ok);
		}
		if ((state == 3 || state == 4) && last != 3 && last != 4) {
			talking();
			state = call_state(&ok);
		}
		if (state != 1 && state != 2 && state != 3 && state != 4 && state != 5) {
			record_write();         /* (the call is over: into the history) */
		}
		last = state;
		usleep(200000);
	}
}
