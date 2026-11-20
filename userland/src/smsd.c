/*
 * smsd: the 6T's received texts, from the kernel to the screen and into the conversations.
 *
 * The kernel takes each incoming text from the modem (QMI WMS, drivers/modem/hakios.mpss.c) and keeps the last ones in
 * kern.hakios.sms_inbox: a line each -- a sequence number, the sender, and for a part of a long text its reference,
 * part and parts -- then the text in UTF-8 (\\, \n and \t escaped). CommCenter, which hands texts to imagent and
 * Messages on an iPhone, does not run here (it speaks Apple's baseband protocol), so smsd takes them: the parts of a
 * long text are put together again (or, if one never comes, what came is taken after PART_WAIT seconds), each text is
 * written into the conversations the Texts application shows (/var/mobile/Library/HakiOS/texts.txt: a line each --
 * seconds since 1970, "in", the number, the text escaped as above) and shown as an alert through CFUserNotification
 * (SpringBoard's SBUserNotification): the sender, by the contact's name when there is one (src/calld_contacts.m), as
 * the title, the text as the message (signed with com.apple.springboard.CFUserNotification and for the address book:
 * src/smsd.entitlements).
 *
 * Without the sysctls (another phone, or sms=0) it ends at once.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* (in iOS's CoreFoundation, served by SpringBoard, but marked unavailable in the SDK's header: declared by its symbol) */
extern SInt32 notice(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef icon, CFURLRef sound, CFURLRef localization,
    CFStringRef header, CFStringRef message, CFStringRef button) __asm__("_CFUserNotificationDisplayNotice");
#define PLAIN_ALERT 3   /* kCFUserNotificationPlainAlertLevel */

extern int contact_name(const char *number, char *out, size_t size);     /* src/calld_contacts.m */

#define TEXTS_DIR       "/private/var/mobile/Library/HakiOS"
#define TEXTS           TEXTS_DIR "/texts.txt"
#define MOBILE          501
#define INBOX_BYTES     (64 << 10)
#define PENDING         16
#define PART_WAIT       60              /* seconds a long text waits for a missing part */
#define TEXT_BYTES      4096

/* the parts of a long text, as they come */
static struct {
	int used;
	char from[24];
	unsigned ref, parts, have;
	time_t first;
	char *part[256];
} pending[PENDING];

static void
unescape(char *s)
{
	char *o = s;

	for (; *s != 0; s++) {
		if (*s == '\\' && s[1] != 0) {
			s++;
			*o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
		} else {
			*o++ = *s;
		}
	}
	*o = 0;
}

/* a text into the conversations file, escaped */
static void
keep(const char *from, const char *text)
{
	FILE *f;

	(void)mkdir(TEXTS_DIR, 0755);
	(void)chown(TEXTS_DIR, MOBILE, MOBILE);
	f = fopen(TEXTS, "a");
	if (f == NULL) {
		perror("smsd: " TEXTS);
		return;
	}
	fprintf(f, "%ld\tin\t%s\t", (long)time(NULL), from);
	for (const char *c = text; *c != 0; c++) {
		if (*c == '\\' || *c == '\n' || *c == '\t') {
			fputc('\\', f);
			fputc(*c == '\n' ? 'n' : *c == '\t' ? 't' : '\\', f);
		} else {
			fputc(*c, f);
		}
	}
	fputc('\n', f);
	fclose(f);
	(void)chown(TEXTS, MOBILE, MOBILE);
	(void)chmod(TEXTS, 0644);
}

/*
 * A data message (the kernel's "\001DATA:" and hex): on this phone an MMS notification -- a WAP push from the carrier
 * (from a number of its own) saying a picture or long message waits on its MMS server. Its sender is in it, as
 * "+NUMBER/TYPE=PLMN"; MMS itself (fetched over a data connection of its own) is not done here, so it is said so.
 * 1 when it was one (and taken care of).
 */
static int
data_message(char *from, const char **text)
{
	static char said[96];
	unsigned char bytes[2048];
	size_t n = 0;
	const char *hex = *text + 6, *plmn;

	if (strncmp(*text, "\001DATA:", 6) != 0) {
		return 0;
	}
	while (hex[0] != 0 && hex[1] != 0 && n < sizeof(bytes) - 1) {
		unsigned v;

		if (sscanf(hex, "%2x", &v) != 1) {
			break;
		}
		bytes[n++] = (unsigned char)v;
		hex += 2;
	}
	bytes[n] = 0;
	plmn = (const char *)memmem(bytes, n, "/TYPE=PLMN", 10);
	if (plmn != NULL) {
		const char *start = plmn;

		while (start > (const char *)bytes && ((start[-1] >= '0' && start[-1] <= '9') || start[-1] == '+')) {
			start--;
		}
		if (plmn - start >= 3 && (size_t)(plmn - start) < 24) {
			memcpy(from, start, (size_t)(plmn - start));
			from[plmn - start] = 0;
		}
	}
	printf("smsd: a data message of %zu bytes: %s, from %s\n", n, plmn != NULL ? "an MMS notification" :
	    "not an MMS notification", from);
	snprintf(said, sizeof(said), "[A picture or long message (MMS): HakiOS cannot receive those yet]");
	*text = said;
	return 1;
}

/* a whole text: kept and shown */
static void
arrived(const char *sender, const char *text)
{
	char name[96], title[160], from[24];
	CFStringRef header, message;

	strlcpy(from, sender, sizeof(from));
	(void)data_message(from, &text);

	keep(from, text);
	if (contact_name(from, name, sizeof(name))) {
		snprintf(title, sizeof(title), "%s", name);
	} else {
		snprintf(title, sizeof(title), "Text from %s", from);
	}
	printf("smsd: a text from %s (%zu bytes): kept and shown\n", from, strlen(text));
	header = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);
	message = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
	if (message == NULL) {
		message = CFStringCreateWithCString(NULL, text, kCFStringEncodingISOLatin1);
	}
	/* (no wait for the button: the next text may come while this one is up) */
	(void)notice(0, PLAIN_ALERT, NULL, NULL, NULL, header, message, CFSTR("OK"));
	if (header != NULL) {
		CFRelease(header);
	}
	if (message != NULL) {
		CFRelease(message);
	}
}

/* a long text's parts put together (what is missing left out) */
static void
assemble(int p)
{
	char *text = calloc(1, TEXT_BYTES);

	for (unsigned k = 1; k <= pending[p].parts && text != NULL; k++) {
		if (pending[p].part[k] != NULL) {
			/* (a data message's parts: one "\001DATA:" in front, the hex of each after it) */
			int data = k > 1 && strncmp(pending[p].part[k], "\001DATA:", 6) == 0;

			strlcat(text, pending[p].part[k] + (data ? 6 : 0), TEXT_BYTES);
			free(pending[p].part[k]);
			pending[p].part[k] = NULL;
		}
	}
	if (pending[p].have < pending[p].parts) {
		printf("smsd: a long text from %s came without %u of its %u parts\n", pending[p].from,
		    pending[p].parts - pending[p].have, pending[p].parts);
	}
	if (text != NULL) {
		arrived(pending[p].from, text);
		free(text);
	}
	pending[p].used = 0;
}

static void
part_of(const char *from, unsigned ref, unsigned part, unsigned parts, const char *text)
{
	int p, free_slot = -1;

	for (p = 0; p < PENDING; p++) {
		if (pending[p].used && pending[p].ref == ref && pending[p].parts == parts && strcmp(pending[p].from, from) == 0) {
			break;
		}
		if (!pending[p].used && free_slot < 0) {
			free_slot = p;
		}
	}
	if (p == PENDING) {
		if (free_slot < 0) {
			arrived(from, text);    /* (no room: alone) */
			return;
		}
		p = free_slot;
		memset(&pending[p], 0, sizeof(pending[p]));
		pending[p].used = 1;
		strlcpy(pending[p].from, from, sizeof(pending[p].from));
		pending[p].ref = ref;
		pending[p].parts = parts;
		pending[p].first = time(NULL);
	}
	if (part >= 1 && part <= parts && pending[p].part[part] == NULL) {
		pending[p].part[part] = strdup(text);
		pending[p].have++;
	}
	if (pending[p].have == pending[p].parts) {
		assemble(p);
	}
}

int
main(void)
{
	char *inbox = malloc(INBOX_BYTES);
	unsigned seen = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (inbox == NULL) {
		return 1;
	}
	{
		size_t size = INBOX_BYTES;

		if (sysctlbyname("kern.hakios.sms_inbox", inbox, &size, NULL, 0) != 0) {
			printf("smsd: no texts in this kernel (kern.hakios.sms_inbox): nothing to do\n");
			return 0;
		}
	}
	printf("smsd: waiting for texts\n");
	for (;;) {
		size_t size = INBOX_BYTES - 1;
		char *line, *next;

		if (sysctlbyname("kern.hakios.sms_inbox", inbox, &size, NULL, 0) != 0) {
			size = 0;
		}
		inbox[size] = 0;
		/* (what this boot's kernel received, oldest first: each taken once) */
		for (line = inbox; line != NULL && *line != 0; line = next) {
			char *f[6];
			unsigned seq, n = 0;

			next = strchr(line, '\n');
			if (next != NULL) {
				*next++ = 0;
			}
			for (char *c = line; n < 6; n++) {
				f[n] = c;
				if (n < 5) {
					c = strchr(c, '\t');
					if (c == NULL) {
						break;
					}
					*c++ = 0;
				}
			}
			if (n < 5) {
				continue;
			}
			seq = (unsigned)strtoul(f[0], NULL, 10);
			if (seq <= seen) {
				continue;
			}
			seen = seq;
			unescape(f[5]);
			if (strtoul(f[4], NULL, 10) > 1) {
				part_of(f[1], (unsigned)strtoul(f[2], NULL, 10), (unsigned)strtoul(f[3], NULL, 10),
				    (unsigned)strtoul(f[4], NULL, 10), f[5]);
			} else {
				arrived(f[1], f[5]);
			}
		}
		for (int p = 0; p < PENDING; p++) {
			if (pending[p].used && time(NULL) - pending[p].first > PART_WAIT) {
				assemble(p);
			}
		}
		sleep(1);
	}
}
