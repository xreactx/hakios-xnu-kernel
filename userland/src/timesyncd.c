/*
 * timesyncd: the clock set from the network (SNTP, RFC 4330), for a phone started with no host.
 *
 * A run started by xl gets the host's time (epoch= on the command line); a boot with no host (xl standalone) gets only the
 * firmware's clock, which on the OnePlus 6T is not the time of day -- and with a wrong date every HTTPS certificate looks
 * invalid, and Safari reaches nothing. iOS's own timed takes its time from CommCenter, which does not run here.
 *
 * So: at start, a clock that is behind the last time saved here is moved to it (better than 1970 until the network is up);
 * then the time is asked of an NTP server every 10 s until one answers (cellular or Wi-Fi, whichever is up), the clock set
 * when it is more than a second off, and asked again every 3 hours; the time is saved every 10 minutes.
 */
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SAVED           "/private/var/mobile/Library/HakiOS/time.last"
#define NTP_TO_UNIX     2208988800u
#define SANE            1735689600L     /* 2025-01-01: anything earlier is not the time */

static const char *const servers[] = { "time.google.com", "time.apple.com", "pool.ntp.org", "time.cloudflare.com" };

static void
say(const char *what, long t)
{
	char when[64];
	time_t tt = t;
	struct tm tm;

	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", gmtime_r(&tt, &tm));
	printf("timesyncd: %s: %s\n", what, when);
	fflush(stdout);
}

static void
save(void)
{
	FILE *f = fopen(SAVED, "w");

	if (f != NULL) {
		fprintf(f, "%ld\n", (long)time(NULL));
		fclose(f);
	}
}

static void
from_saved(void)
{
	FILE *f = fopen(SAVED, "r");
	long t = 0;

	if (f == NULL) {
		return;
	}
	if (fscanf(f, "%ld", &t) == 1 && t > SANE && time(NULL) < t) {
		struct timeval tv = { .tv_sec = t, .tv_usec = 0 };

		if (settimeofday(&tv, NULL) == 0) {
			say("the clock was behind the last saved time: set to it until the network answers", t);
		}
	}
	fclose(f);
}

/* One server asked: 1 and the clock set (or already right), 0 if it did not answer. */
static int
ask(const char *server)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai = NULL;
	unsigned char packet[48];
	struct timeval sent, got, timeout = { .tv_sec = 3, .tv_usec = 0 };
	int s, ok = 0;

	if (getaddrinfo(server, "123", &hints, &ai) != 0 || ai == NULL) {
		return 0;
	}
	s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (s < 0) {
		freeaddrinfo(ai);
		return 0;
	}
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	memset(packet, 0, sizeof(packet));
	packet[0] = 0x23;                       /* no leap warning, version 4, client */
	gettimeofday(&sent, NULL);
	if (sendto(s, packet, sizeof(packet), 0, ai->ai_addr, ai->ai_addrlen) == sizeof(packet) &&
	    recv(s, packet, sizeof(packet), 0) == sizeof(packet) && (packet[0] & 7) == 4 && packet[1] != 0) {
		/* the server's transmit time, plus half the round trip */
		uint32_t secs = (uint32_t)packet[40] << 24 | (uint32_t)packet[41] << 16 | (uint32_t)packet[42] << 8 | packet[43];
		uint32_t frac = (uint32_t)packet[44] << 24 | (uint32_t)packet[45] << 16 | (uint32_t)packet[46] << 8 | packet[47];
		double now, trip;

		gettimeofday(&got, NULL);
		trip = (got.tv_sec - sent.tv_sec) + (got.tv_usec - sent.tv_usec) / 1e6;
		now = (double)(secs - NTP_TO_UNIX) + frac / 4294967296.0 + trip / 2;
		if (now > SANE) {
			double ours = got.tv_sec + got.tv_usec / 1e6, off = now - ours;

			if (off > 1 || off < -1) {
				struct timeval tv = { .tv_sec = (time_t)now, .tv_usec = (suseconds_t)((now - (long)now) * 1e6) };

				if (settimeofday(&tv, NULL) == 0) {
					printf("timesyncd: %s: the clock was %.0f s off\n", server, off);
					say("set", (long)now);
				} else {
					printf("timesyncd: %s answered, but the clock could not be set\n", server);
				}
			}
			ok = 1;
		}
	}
	close(s);
	freeaddrinfo(ai);
	return ok;
}

int
main(void)
{
	unsigned since_save = 0;

	from_saved();
	for (;;) {
		int done = 0;

		for (unsigned i = 0; i < sizeof(servers) / sizeof(servers[0]) && !done; i++) {
			done = ask(servers[i]);
		}
		if (done) {
			save();
		}
		/* until an answer every 10 s; then every 3 hours, the time saved every 10 minutes */
		for (unsigned waited = 0; waited < (done ? 3 * 3600u : 10u); waited += 10) {
			sleep(10);
			if (done && (since_save += 10) >= 600) {
				since_save = 0;
				save();
			}
		}
	}
}
