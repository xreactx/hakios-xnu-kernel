/*
 * smsd's MMS: a picture or long message, fetched when its notification comes (src/smsd.c).
 *
 * An MMS arrives as an SMS of data from the carrier: a WAP push (WSP, OMA-WAP-PUSH) carrying an m-notification-ind
 * (OMA-MMS-ENC) -- the sender, a transaction id and the message's address on the carrier's MMS server (its
 * Content-Location). The message is then fetched from there with an HTTP GET over the cellular data connection (en1:
 * the socket is bound to it, since Wi-Fi cannot reach the server) through the carrier's WAP proxy, and is an
 * m-retrieve-conf: headers, then a multipart body (WSP's application/vnd.wap.multipart.*) of text, pictures and a SMIL
 * layout. The text parts become the text; each picture is saved under /var/mobile/Library/HakiOS/mms and named in the
 * text as a line "\001IMG:PATH", which the Texts application shows as the picture. The server is then told the message
 * was retrieved (m-notifyresp-ind, POSTed), so it does not send the notification again.
 *
 * The carrier's MMS settings are AT&T's (the 6T's SIM: APN nxtgenphone) unless /var/mobile/Library/HakiOS/mms.conf
 * says otherwise: lines "proxy A.B.C.D:PORT" and "mmsc URL".
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MMS_DIR         "/private/var/mobile/Library/HakiOS/mms"
#define MMS_CONF        "/private/var/mobile/Library/HakiOS/mms.conf"
#define MOBILE          501
#define CELL_IF         "en1"                   /* the cellular data connection (drivers/modem/hakios.cell.c) */
#define MAX_MMS         (2u << 20)

static char proxy_ip[32] = "172.26.39.1", mmsc[256] = "http://mmsc.mobile.att.net";
static int proxy_port = 80;

static void
settings(void)
{
	FILE *f = fopen(MMS_CONF, "r");
	char line[300];

	if (f == NULL) {
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char ip[32];
		int port;

		line[strcspn(line, "\r\n")] = 0;
		if (sscanf(line, "proxy %31[0-9.]:%d", ip, &port) == 2) {
			strlcpy(proxy_ip, ip, sizeof(proxy_ip));
			proxy_port = port;
		} else if (strncmp(line, "mmsc ", 5) == 0) {
			strlcpy(mmsc, line + 5, sizeof(mmsc));
		}
	}
	fclose(f);
}

/* ---- WSP's encodings ---- */

static size_t
uintvar(const unsigned char *p, size_t n, size_t *at)
{
	size_t v = 0;

	while (*at < n) {
		unsigned char b = p[(*at)++];

		v = v << 7 | (b & 0x7f);
		if (!(b & 0x80)) {
			break;
		}
	}
	return v;
}

/* past one header's value (short integer, value-length and its bytes, or a string to its NUL) */
static void
skip_value(const unsigned char *p, size_t n, size_t *at)
{
	unsigned char v;

	if (*at >= n) {
		return;
	}
	v = p[*at];
	if (v >= 0x80) {
		(*at)++;
	} else if (v <= 0x1e) {
		*at += 1u + v;
	} else if (v == 0x1f) {
		size_t length;

		(*at)++;
		length = uintvar(p, n, at);
		*at += length;
	} else {
		while (*at < n && p[*at] != 0) {
			(*at)++;
		}
		(*at)++;
	}
}

/* a text-string value at *at (its quote dropped) into out */
static void
text_value(const unsigned char *p, size_t n, size_t at, char *out, size_t size)
{
	size_t o = 0;

	if (at < n && (p[at] == 0x7f || p[at] == '"')) {
		at++;
	}
	while (at < n && p[at] != 0 && o + 1 < size) {
		out[o++] = (char)p[at++];
	}
	out[o] = 0;
}

/* "+NUMBER/TYPE=PLMN" anywhere in [from, to) into number */
static void
plmn_number(const unsigned char *p, size_t from, size_t to, char *number, size_t size)
{
	const char *plmn = memmem(p + from, to - from, "/TYPE=PLMN", 10), *start;

	if (plmn == NULL) {
		return;
	}
	start = plmn;
	while (start > (const char *)p + from && ((start[-1] >= '0' && start[-1] <= '9') || start[-1] == '+')) {
		start--;
	}
	if (plmn - start >= 3 && (size_t)(plmn - start) < size) {
		memcpy(number, start, (size_t)(plmn - start));
		number[plmn - start] = 0;
	}
}

/* ---- HTTP through the carrier's proxy, over the cellular connection ---- */

/* the body of an answer to `request` (malloc'd, *length its bytes), or NULL */
static unsigned char *
http(const char *request, size_t request_length, size_t *length, int *status)
{
	struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons((uint16_t)proxy_port) };
	unsigned int index = if_nametoindex(CELL_IF);
	struct timeval timeout = { 30, 0 };
	unsigned char *all = malloc(MAX_MMS + 1), *body, *out;
	size_t got = 0, header_length;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	char *end;

	*status = 0;
	if (s < 0 || all == NULL) {
		free(all);
		return NULL;
	}
	inet_pton(AF_INET, proxy_ip, &to.sin_addr);
	if (index != 0 && setsockopt(s, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) != 0) {
		perror("smsd: mms: bound to " CELL_IF);
	}
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0) {
		printf("smsd: mms: the proxy %s:%d over %s (index %u) cannot be reached: %s\n", proxy_ip, proxy_port, CELL_IF, index,
		    strerror(errno));
		close(s);
		free(all);
		return NULL;
	}
	for (size_t sent = 0; sent < request_length;) {
		ssize_t w = write(s, request + sent, request_length - sent);

		if (w <= 0) {
			break;
		}
		sent += (size_t)w;
	}
	for (;;) {
		ssize_t r = read(s, all + got, MAX_MMS - got);

		if (r <= 0) {
			break;
		}
		got += (size_t)r;
		if (got == MAX_MMS) {
			break;
		}
	}
	close(s);
	all[got] = 0;
	end = memmem(all, got, "\r\n\r\n", 4);
	if (end == NULL || sscanf((char *)all, "HTTP/%*s %d", status) != 1) {
		printf("smsd: mms: no answer from the proxy (%zu bytes)\n", got);
		free(all);
		return NULL;
	}
	header_length = (size_t)(end - (char *)all) + 4;
	body = all + header_length;
	*length = got - header_length;
	if (strcasestr((char *)all, "\r\ntransfer-encoding: chunked") != NULL && strcasestr((char *)all, "\r\n\r\n") != NULL) {
		/* (chunked: put together) */
		size_t at = 0, o = 0;

		while (at < *length) {
			size_t chunk = strtoul((char *)body + at, NULL, 16);
			unsigned char *line_end = memmem(body + at, *length - at, "\r\n", 2);

			if (line_end == NULL || chunk == 0) {
				break;
			}
			at = (size_t)(line_end - body) + 2;
			if (at + chunk > *length) {
				chunk = *length - at;
			}
			memmove(body + o, body + at, chunk);
			o += chunk;
			at += chunk + 2;
		}
		*length = o;
	}
	out = malloc(*length + 1);
	if (out != NULL) {
		memcpy(out, body, *length);
		out[*length] = 0;
	}
	free(all);
	return out;
}

/* ---- the message ---- */

static const char *
image_extension(const char *type)
{
	return strstr(type, "png") ? "png" : strstr(type, "gif") ? "gif" : strstr(type, "jpeg") || strstr(type, "jpg") ? "jpg" :
	    strstr(type, "heic") ? "heic" : NULL;
}

/* a part's content type (a well-known number, a string, or either inside a value-length) into type */
static void
part_type(const unsigned char *p, size_t n, size_t at, char *type, size_t size)
{
	static const char *const known[] = { [0x03] = "text/plain", [0x1d] = "image/gif", [0x1e] = "image/jpeg",
		[0x20] = "image/png", [0x33] = "application/vnd.wap.multipart.related" };

	type[0] = 0;
	if (at >= n) {
		return;
	}
	if (p[at] <= 0x1e) {
		at++;
	} else if (p[at] == 0x1f) {
		at++;
		(void)uintvar(p, n, &at);
	}
	if (at < n && p[at] >= 0x80) {
		unsigned code = p[at] & 0x7f;

		strlcpy(type, code < sizeof(known) / sizeof(known[0]) && known[code] != NULL ? known[code] : "?", size);
	} else {
		text_value(p, n, at, type, size);
	}
}

/* m-retrieve-conf's parts: the text into text, the pictures saved and named in it; the sender into from */
static int
unpack(const unsigned char *m, size_t n, char *from, size_t from_size, char *text, size_t text_size)
{
	size_t at = 0, parts, pictures = 0;
	unsigned long when = (unsigned long)time(NULL);

	text[0] = 0;
	/* the headers, to the content type (the last of them) */
	while (at < n) {
		unsigned char field = m[at++];

		if (field == 0x89) {            /* From */
			size_t start = at;

			skip_value(m, n, &at);
			plmn_number(m, start, at, from, from_size);
			continue;
		}
		if (field == 0x84) {            /* Content-Type: the body follows it */
			skip_value(m, n, &at);
			break;
		}
		if (field < 0x80) {
			/* (an application header: a name and a value, both strings) */
			while (at < n && m[at] != 0) {
				at++;
			}
			at++;
		}
		skip_value(m, n, &at);
	}
	if (at >= n) {
		return 0;
	}
	parts = uintvar(m, n, &at);
	mkdir(MMS_DIR, 0755);
	chown(MMS_DIR, MOBILE, MOBILE);
	for (size_t k = 0; k < parts && at < n; k++) {
		size_t headers = uintvar(m, n, &at), data = uintvar(m, n, &at), data_at = at + headers;
		char type[96];
		const char *extension;

		if (data_at + data > n) {
			break;
		}
		part_type(m, n, at, type, sizeof(type));
		printf("smsd: mms: part %zu: %s, %zu bytes\n", k + 1, type, data);
		if (strncmp(type, "text/plain", 10) == 0) {
			size_t used = strlen(text);

			if (used != 0 && used + 1 < text_size) {
				strlcat(text, "\n", text_size);
				used++;
			}
			if (used + data < text_size) {
				memcpy(text + used, m + data_at, data);
				text[used + data] = 0;
			}
		} else if ((extension = image_extension(type)) != NULL) {
			char path[160], line[180];
			FILE *f;

			snprintf(path, sizeof(path), MMS_DIR "/%lu-%zu.%s", when, ++pictures, extension);
			f = fopen(path, "wb");
			if (f != NULL) {
				fwrite(m + data_at, 1, data, f);
				fclose(f);
				chown(path, MOBILE, MOBILE);
				chmod(path, 0644);
				snprintf(line, sizeof(line), "%s\001IMG:%s", text[0] != 0 ? "\n" : "", path);
				strlcat(text, line, text_size);
			}
		}
		at = data_at + data;
	}
	return text[0] != 0;
}

/*
 * An MMS notification's bytes (the WAP push, after the SMS's own header): the message fetched and unpacked into text
 * (see above), the sender into from; 0 when it could not be.
 */
int
mms_fetch(const unsigned char *push, size_t n, char *from, size_t from_size, char *text, size_t text_size)
{
	size_t at = 0, headers, length = 0;
	char location[512] = "", transaction[128] = "", request[1024];
	unsigned char *m;
	int status = 0, ok;

	settings();
	/* WSP push: a transaction id, the PDU type (6), its headers (their length a uintvar), then the MMS PDU */
	if (n < 3 || push[1] != 0x06) {
		printf("smsd: mms: not a WAP push (type %#x)\n", n > 1 ? push[1] : 0);
		return 0;
	}
	at = 2;
	headers = uintvar(push, n, &at);
	at += headers;
	if (at + 2 > n || push[at] != 0x8c || push[at + 1] != 0x82) {
		printf("smsd: mms: not an m-notification-ind\n");
		return 0;
	}
	while (at < n) {
		unsigned char field = push[at++];

		if (field == 0x98) {
			text_value(push, n, at, transaction, sizeof(transaction));
		} else if (field == 0x83) {
			text_value(push, n, at, location, sizeof(location));
		} else if (field == 0x89) {
			size_t start = at;

			skip_value(push, n, &at);
			plmn_number(push, start, at, from, from_size);
			continue;
		}
		skip_value(push, n, &at);
	}
	if (location[0] == 0) {
		printf("smsd: mms: the notification has no address of the message\n");
		return 0;
	}
	printf("smsd: mms: from %s, transaction %s, at %s: fetched through %s:%d\n", from, transaction, location, proxy_ip, proxy_port);
	{
		const char *host = strstr(location, "://");
		char h[128];

		host = host != NULL ? host + 3 : location;
		strlcpy(h, host, sizeof(h));
		h[strcspn(h, "/:")] = 0;
		snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Android-Mms/2.0\r\n"
		    "Accept: */*, application/vnd.wap.mms-message, application/vnd.wap.sic\r\n"
		    "x-wap-profile: http://www.google.com/oha/rdf/ua-profile-kila.xml\r\nConnection: close\r\n\r\n", location, h);
	}
	m = http(request, strlen(request), &length, &status);
	if (m == NULL || status != 200) {
		printf("smsd: mms: the message was not fetched (HTTP %d, %zu bytes)\n", status, length);
		free(m);
		return 0;
	}
	printf("smsd: mms: %zu bytes fetched\n", length);
	ok = length > 2 && m[0] == 0x8c && m[1] == 0x84 && unpack(m, length, from, from_size, text, text_size);
	free(m);
	if (!ok) {
		printf("smsd: mms: what was fetched is not a message with text or pictures\n");
		return 0;
	}
	/* the server told it was retrieved: m-notifyresp-ind, version 1.2, status Retrieved */
	if (transaction[0] != 0) {
		unsigned char pdu[160];
		size_t p = 0, tl = strlen(transaction);
		char head[512];
		int head_length;
		char *whole;
		unsigned char *answer;
		size_t answer_length;
		const char *host = strstr(mmsc, "://");
		char h[128];

		host = host != NULL ? host + 3 : mmsc;
		strlcpy(h, host, sizeof(h));
		h[strcspn(h, "/:")] = 0;
		pdu[p++] = 0x8c; pdu[p++] = 0x83;
		pdu[p++] = 0x98;
		if (tl > sizeof(pdu) - 10) {
			tl = sizeof(pdu) - 10;
		}
		memcpy(pdu + p, transaction, tl);
		p += tl;
		pdu[p++] = 0;
		pdu[p++] = 0x8d; pdu[p++] = 0x92;
		pdu[p++] = 0x95; pdu[p++] = 0x81;
		head_length = snprintf(head, sizeof(head), "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Android-Mms/2.0\r\n"
		    "Content-Type: application/vnd.wap.mms-message\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", mmsc, h, p);
		whole = malloc((size_t)head_length + p);
		if (whole != NULL) {
			memcpy(whole, head, (size_t)head_length);
			memcpy(whole + head_length, pdu, p);
			answer = http(whole, (size_t)head_length + p, &answer_length, &status);
			printf("smsd: mms: the server told it is retrieved: HTTP %d\n", status);
			free(answer);
			free(whole);
		}
	}
	return 1;
}

/* ---- sending: m-send-req, POSTed to the MMSC ---- */

static size_t
put_uintvar(unsigned char *out, size_t v)
{
	unsigned char b[5];
	size_t n = 0;

	do {
		b[n++] = (unsigned char)(v & 0x7f);
		v >>= 7;
	} while (v != 0 && n < sizeof(b));
	for (size_t i = 0; i < n; i++) {
		out[i] = b[n - 1 - i] | (i + 1 < n ? 0x80 : 0);
	}
	return n;
}

/*
 * An MMS to `number` (digits, "+" in front for an international one): `text` (may be empty) and the JPEG at
 * `picture` (may be NULL). m-send-req, version 1.2: the sender left to the network (insert-address), the recipient
 * "NUMBER/TYPE=PLMN", a multipart.mixed body of a text/plain part (UTF-8) and an image/jpeg one. 1 when the MMSC
 * answers with m-send-conf's status Ok.
 */
int
mms_send(const char *number, const char *text, const char *picture)
{
	unsigned char *pdu, *answer;
	unsigned char *image = NULL;
	size_t image_length = 0, p = 0, text_length = strlen(text), answer_length = 0;
	char transaction[32], head[512], h[128];
	const char *host;
	int status = 0, ok = 0, head_length;

	settings();
	if (picture != NULL && picture[0] != 0) {
		FILE *f = fopen(picture, "rb");

		if (f != NULL) {
			fseek(f, 0, SEEK_END);
			image_length = (size_t)ftell(f);
			fseek(f, 0, SEEK_SET);
			image = malloc(image_length);
			if (image == NULL || fread(image, 1, image_length, f) != image_length) {
				free(image);
				image = NULL;
				image_length = 0;
			}
			fclose(f);
		}
		if (image == NULL) {
			printf("smsd: mms: the picture %s cannot be read\n", picture);
			return 0;
		}
	}
	pdu = malloc(1024 + text_length + image_length);
	if (pdu == NULL) {
		free(image);
		return 0;
	}
	snprintf(transaction, sizeof(transaction), "hakios%lx", (unsigned long)time(NULL));
	pdu[p++] = 0x8c; pdu[p++] = 0x80;                       /* X-Mms-Message-Type: m-send-req */
	pdu[p++] = 0x98;                                        /* X-Mms-Transaction-Id */
	memcpy(pdu + p, transaction, strlen(transaction) + 1);
	p += strlen(transaction) + 1;
	pdu[p++] = 0x8d; pdu[p++] = 0x92;                       /* X-Mms-MMS-Version: 1.2 */
	pdu[p++] = 0x89; pdu[p++] = 0x01; pdu[p++] = 0x81;      /* From: insert-address */
	p += (size_t)snprintf((char *)pdu + p, 64, "%c%s/TYPE=PLMN", 0x97, number) + 1;   /* To (and its NUL) */
	pdu[p++] = 0x84; pdu[p++] = 0xa3;                       /* Content-Type: application/vnd.wap.multipart.mixed */
	p += put_uintvar(pdu + p, (text_length != 0) + (image != NULL));
	if (text_length != 0) {
		static const unsigned char headers[] = { 0x03, 0x83, 0x81, 0xea,  /* text/plain; charset=utf-8 */
			0x8e, 't', 'e', 'x', 't', '.', 't', 'x', 't', 0 };          /* Content-Location: text.txt */

		p += put_uintvar(pdu + p, sizeof(headers));
		p += put_uintvar(pdu + p, text_length);
		memcpy(pdu + p, headers, sizeof(headers));
		p += sizeof(headers);
		memcpy(pdu + p, text, text_length);
		p += text_length;
	}
	if (image != NULL) {
		static const unsigned char headers[] = { 0x9e,                      /* image/jpeg */
			0x8e, 'i', 'm', 'a', 'g', 'e', '.', 'j', 'p', 'g', 0 };     /* Content-Location: image.jpg */

		p += put_uintvar(pdu + p, sizeof(headers));
		p += put_uintvar(pdu + p, image_length);
		memcpy(pdu + p, headers, sizeof(headers));
		p += sizeof(headers);
		memcpy(pdu + p, image, image_length);
		p += image_length;
	}
	free(image);
	host = strstr(mmsc, "://");
	host = host != NULL ? host + 3 : mmsc;
	strlcpy(h, host, sizeof(h));
	h[strcspn(h, "/:")] = 0;
	head_length = snprintf(head, sizeof(head), "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Android-Mms/2.0\r\n"
	    "x-wap-profile: http://www.google.com/oha/rdf/ua-profile-kila.xml\r\n"
	    "Content-Type: application/vnd.wap.mms-message\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", mmsc, h, p);
	{
		char *whole = malloc((size_t)head_length + p);

		if (whole != NULL) {
			memcpy(whole, head, (size_t)head_length);
			memcpy(whole + head_length, pdu, p);
			printf("smsd: mms: sending %zu bytes to %s (text %zu bytes, a picture of %zu)\n", p, number, text_length,
			    image_length);
			answer = http(whole, (size_t)head_length + p, &answer_length, &status);
			free(whole);
			if (answer != NULL && status == 200 && answer_length >= 2 && answer[0] == 0x8c && answer[1] == 0x81) {
				/* m-send-conf: its X-Mms-Response-Status (0x92): 0x80 Ok */
				for (size_t i = 2; i + 1 < answer_length; i++) {
					if (answer[i] == 0x92) {
						ok = answer[i + 1] == 0x80;
						printf("smsd: mms: the MMSC answers status %#x%s\n", answer[i + 1], ok ? " (Ok): SENT" : "");
						break;
					}
				}
			} else {
				printf("smsd: mms: no m-send-conf (HTTP %d, %zu bytes)\n", status, answer_length);
			}
			free(answer);
		}
	}
	free(pdu);
	return ok;
}
