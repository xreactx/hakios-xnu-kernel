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
#include <dirent.h>
#include <pthread.h>
#include <notify.h>
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
#define TOAST           TEXTS_DIR "/toast.txt"
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

/*
 * a text into the conversations file, escaped: its kind (in, out, failed), the conversation (the other side's number,
 * or a group's numbers comma-separated) and, in a group, who wrote it (a fifth field)
 */
static pthread_mutex_t keep_lock = PTHREAD_MUTEX_INITIALIZER;     /* (MMS are fetched and sent on threads of their own) */

static void
keep_kind(const char *kind, const char *from, const char *text, const char *sender)
{
	FILE *f;

	pthread_mutex_lock(&keep_lock);
	(void)mkdir(TEXTS_DIR, 0755);
	(void)chown(TEXTS_DIR, MOBILE, MOBILE);
	f = fopen(TEXTS, "a");
	if (f == NULL) {
		perror("smsd: " TEXTS);
		pthread_mutex_unlock(&keep_lock);
		return;
	}
	fprintf(f, "%ld\t%s\t%s\t", (long)time(NULL), kind, from);
	for (const char *c = text; *c != 0; c++) {
		if (*c == '\\' || *c == '\n' || *c == '\t') {
			fputc('\\', f);
			fputc(*c == '\n' ? 'n' : *c == '\t' ? 't' : '\\', f);
		} else {
			fputc(*c, f);
		}
	}
	if (sender != NULL && sender[0] != 0) {
		fprintf(f, "\t%s", sender);
	}
	fputc('\n', f);
	fclose(f);
	(void)chown(TEXTS, MOBILE, MOBILE);
	(void)chmod(TEXTS, 0644);
	pthread_mutex_unlock(&keep_lock);
}

static void
keep(const char *from, const char *text)
{
	keep_kind("in", from, text, NULL);
}

/*
 * The MMS the Texts application asks to send: a file in OUTBOX each (written whole, then renamed to .req) -- the number
 * on its first line, the picture's path on its second (empty: none), the text after. Sent (src/mms.c), written into
 * the conversations as out or failed (the picture as a "\001IMG:PATH" line), and the file removed.
 */
#define OUTBOX          TEXTS_DIR "/outbox"
extern int mms_send(const char *number, const char *text, const char *picture);

/* one request from OUTBOX (its contents, malloc'd: freed here): sent, and written into the conversations */
static void *
send_request(void *argument)
{
	char *request = argument, number[512] = "", picture[256] = "", *text, *body;
	size_t size = strlen(request);
	int ok;

	text = request;
	sscanf(text, "%511[^\n]", number);
	text = strchr(text, '\n');
	text = text != NULL ? text + 1 : request + size;
	if (*text != '\n') {
		sscanf(text, "%255[^\n]", picture);
	}
	text = strchr(text, '\n');
	text = text != NULL ? text + 1 : request + size;
	printf("smsd: an MMS to send to %s (%s, %zu bytes of text)\n", number, picture[0] != 0 ? "a picture" : "no picture",
	    strlen(text));
	ok = number[0] != 0 && mms_send(number, text, picture[0] != 0 ? picture : NULL);
	body = calloc(1, strlen(text) + strlen(picture) + 16);
	if (body != NULL) {
		strcpy(body, text);
		if (picture[0] != 0) {
			if (body[0] != 0) {
				strcat(body, "\n");
			}
			strcat(body, "\001IMG:");
			strcat(body, picture);
		}
		keep_kind(ok ? "out" : "failed", number, body, NULL);
		free(body);
	}
	free(request);
	return NULL;
}

static void
outbox(void)
{
	DIR *d = opendir(OUTBOX);
	struct dirent *e;

	if (d == NULL) {
		(void)mkdir(TEXTS_DIR, 0755);
		(void)chown(TEXTS_DIR, MOBILE, MOBILE);
		(void)mkdir(OUTBOX, 0755);
		(void)chown(OUTBOX, MOBILE, MOBILE);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[512], *request;
		size_t length = strlen(e->d_name);
		FILE *f;
		long size;

		if (length < 5 || strcmp(e->d_name + length - 4, ".req") != 0) {
			continue;
		}
		snprintf(path, sizeof(path), OUTBOX "/%s", e->d_name);
		f = fopen(path, "r");
		if (f == NULL) {
			continue;
		}
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		request = calloc(1, (size_t)size + 1);
		if (request != NULL) {
			fread(request, 1, (size_t)size, f);
		}
		fclose(f);
		unlink(path);
		if (request == NULL) {
			continue;
		}
		{
			pthread_t t;

			/* (sent on a thread of its own: the MMSC's answer can take a minute) */
			if (pthread_create(&t, NULL, send_request, request) == 0) {
				pthread_detach(t);
			} else {
				send_request(request);
			}
		}
	}
	closedir(d);
}

/*
 * A data message (the kernel's "\001DATA:" and hex): on this phone an MMS notification -- a WAP push from the carrier
 * (from a number of its own) saying a picture or long message waits on its MMS server. It is fetched and unpacked
 * (src/mms.c): its text, and its pictures as "\001IMG:PATH" lines; the sender is the one inside it. When it cannot be
 * fetched, it is said so. 1 when it was one (and taken care of).
 */
extern int mms_fetch(const unsigned char *push, size_t n, char *from, size_t from_size, char *text, size_t text_size,
    char *people, size_t people_size);
static __thread char group[512];        /* the last MMS's people, comma-separated, when it was a group's */

static int
data_message(char *from, const char **text)
{
	static __thread char said[TEXT_BYTES];
	unsigned char bytes[2048];
	size_t n = 0;
	const char *hex = *text + 6;

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
	printf("smsd: a data message of %zu bytes\n", n);
	group[0] = 0;
	if (!mms_fetch(bytes, n, from, 24, said, sizeof(said), group, sizeof(group))) {
		snprintf(said, sizeof(said), "[A picture or long message (MMS) that could not be fetched]");
	}
	*text = said;
	return 1;
}

static void arrived_now(const char *sender, const char *text);

struct later {
	char *sender, *text;
};

static void *
arrived_thread(void *argument)
{
	struct later *l = argument;

	arrived_now(l->sender, l->text);
	free(l->sender);
	free(l->text);
	free(l);
	return NULL;
}

/*
 * a whole text: kept and shown -- an MMS notification on a thread of its own (its fetch, and the MMSC's answer to
 * "retrieved", can take a minute: the texts after it are not to wait, run 190926)
 */
static void
arrived(const char *sender, const char *text)
{
	pthread_t t;
	struct later *l;

	if (strncmp(text, "\001DATA:", 6) != 0 || (l = calloc(1, sizeof(*l))) == NULL) {
		arrived_now(sender, text);
		return;
	}
	l->sender = strdup(sender);
	l->text = strdup(text);
	if (l->sender == NULL || l->text == NULL || pthread_create(&t, NULL, arrived_thread, l) != 0) {
		arrived_now(sender, text);
		free(l->sender);
		free(l->text);
		free(l);
		return;
	}
	pthread_detach(t);
}

static void
arrived_now(const char *sender, const char *text)
{
	char name[96], title[160], from[24];

	strlcpy(from, sender, sizeof(from));
	group[0] = 0;
	(void)data_message(from, &text);

	if (strchr(group, ',') != NULL) {
		/* a group's: the conversation is its people, and who wrote it is said */
		keep_kind("in", group, text, from);
	} else {
		keep(from, text);
	}
	if (contact_name(from, name, sizeof(name))) {
		snprintf(title, sizeof(title), "%s%s", name, strchr(group, ',') != NULL ? " (group)" : "");
	} else {
		snprintf(title, sizeof(title), "Text from %s", from);
	}
	printf("smsd: a text from %s (%zu bytes): kept and shown\n", from, strlen(text));
	{
		/* (in the banner, a picture is said, not its file) */
		char *shown = strdup(text), *mark;

		while (shown != NULL && (mark = strstr(shown, "\001IMG:")) != NULL) {
			char *end = strchr(mark, '\n');

			memmove(mark + 9, end != NULL ? end : mark + strlen(mark), strlen(end != NULL ? end : mark + strlen(mark)) + 1);
			memcpy(mark, "[Picture]", 9);
		}
		/*
		 * a banner at the top of the screen, 5 s, inside SpringBoard (src/inject_toast.m): the title and the text in
		 * TOAST, then the Darwin notification com.hakios.toast
		 */
		FILE *f = fopen(TOAST ".tmp", "w");

		if (f != NULL) {
			fprintf(f, "%s\n%s", title, shown != NULL ? shown : text);
			fclose(f);
			chown(TOAST ".tmp", MOBILE, MOBILE);
			chmod(TOAST ".tmp", 0644);
			rename(TOAST ".tmp", TOAST);
			notify_post("com.hakios.toast");
		}
		free(shown);
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
		outbox();
		sleep(1);
	}
}
