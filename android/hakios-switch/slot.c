/*
 * hakios-slot: back to HakiOS from Android, without a computer (the Magisk module's Action, android/hakios-switch).
 *
 * The other half of HakiOS's "Android" application (xnuloader/loader.c, slot_to_android), done the same way: the slot
 * being left -- here Android's, boot_b -- is given the unbootable attribute (bit 55), and the slot wanted -- HakiOS's,
 * boot_a -- has its unbootable attribute cleared, its retries set to 7 (bits 51-53) and is marked successful (bit 54:
 * ABL falls back only to a successful slot), in the primary and the backup
 * GPT of the unit that holds them (/dev/block/sde on the OnePlus 6T), both CRC-32s made again and the primary read
 * back. At the next boot ABL finds the active slot unbootable and switches to the other one itself: the type GUIDs,
 * the boot LUN, all that set_active does. HakiOS's switch to Android clears boot_b's attribute again.
 *
 * Nothing is written unless: both GPT copies check out and are the same, boot_a and boot_b are there, and boot_a
 * would be bootable (it is the one the phone starts next).
 *
 *   hakios-slot status       the two slots' attributes
 *   hakios-slot to-hakios    the change above (root)
 *
 * Built static, with no C library (raw Linux system calls): android/hakios-switch/build.sh.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

static long
sys(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
	    x3 __asm__("x3") = d;

	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}
#define openat(p, f)            sys(56, -100, (long)(p), (f), 0)
#define pread(fd, b, n, o)      sys(67, (fd), (long)(b), (n), (o))
#define pwrite(fd, b, n, o)     sys(68, (fd), (long)(b), (n), (o))
#define fsync(fd)               sys(82, (fd), 0, 0, 0)
#define DEVICE                  "/dev/block/sde"
#define BS                      4096

static void
say(const char *s)
{
	long n = 0;

	while (s[n]) {
		n++;
	}
	sys(64, 1, (long)s, n, 0);
}

static void
hex(u64 v)
{
	char b[19] = "0x";

	for (int i = 0; i < 16; i++) {
		b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	}
	b[18] = 0;
	say(b);
}

static void *
memcpy(void *d, const void *s, u64 n)
{
	for (u64 i = 0; i < n; i++) {
		((u8 *)d)[i] = ((const u8 *)s)[i];
	}
	return d;
}

static void *
memset(void *d, int c, u64 n)
{
	for (u64 i = 0; i < n; i++) {
		((u8 *)d)[i] = (u8)c;
	}
	return d;
}

static u32
crc32(const u8 *p, u64 n)
{
	u32 c = 0xffffffffu;

	for (u64 i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

#define R32(p) (*(u32 *)(p))
#define R64(p) (*(u64 *)(p))

static u8 hdr[2][BS] __attribute__((aligned(4096)));
static u8 table[2][64 * 1024] __attribute__((aligned(4096)));

/* the GPT whose header is at block `lba` into hdr[i], table[i]; its table's bytes, 0 if it does not check out */
static u64
gpt_read(int fd, u64 lba, int i)
{
	u32 size, crc;
	u64 bytes;

	if (pread(fd, hdr[i], BS, lba * BS) != BS || R64(hdr[i]) != 0x5452415020494645ul) {
		return 0;
	}
	size = R32(hdr[i] + 12);
	crc = R32(hdr[i] + 16);
	if (size < 92 || size > 512) {
		return 0;
	}
	R32(hdr[i] + 16) = 0;
	if (crc32(hdr[i], size) != crc) {
		return 0;
	}
	R32(hdr[i] + 16) = crc;
	bytes = (u64)R32(hdr[i] + 80) * R32(hdr[i] + 84);
	if (R32(hdr[i] + 84) != 128 || bytes == 0 || (bytes + BS - 1) / BS * BS > sizeof(table[i]) ||
	    pread(fd, table[i], (bytes + BS - 1) / BS * BS, R64(hdr[i] + 72) * BS) != (long)((bytes + BS - 1) / BS * BS) ||
	    crc32(table[i], bytes) != R32(hdr[i] + 88)) {
		return 0;
	}
	return bytes;
}

static int
gpt_write(int fd, int i, u64 bytes)
{
	u32 size = R32(hdr[i] + 12);
	u64 len = (bytes + BS - 1) / BS * BS;

	R32(hdr[i] + 88) = crc32(table[i], bytes);
	R32(hdr[i] + 16) = 0;
	R32(hdr[i] + 16) = crc32(hdr[i], size);
	return pwrite(fd, table[i], len, R64(hdr[i] + 72) * BS) == (long)len &&
	    pwrite(fd, hdr[i], BS, R64(hdr[i] + 24) * BS) == BS;
}

static int
named(const u8 *e, const char *name)
{
	for (int k = 0; k < 36; k++) {
		u32 c = e[56 + 2 * k] | e[57 + 2 * k] << 8;

		if (c != (u8)name[k]) {
			return 0;
		}
		if (c == 0) {
			return 1;
		}
	}
	return 0;
}

static void
attrs(const char *name, u64 a)
{
	say(name);
	say(": ");
	hex(a);
	say((a >> 50 & 1) ? " active" : " inactive");
	say((a >> 54 & 1) ? ", successful" : ", not successful");
	say((a >> 55 & 1) ? ", UNBOOTABLE" : ", bootable");
	say(", retries ");
	{
		char d[3] = { (char)('0' + (a >> 51 & 7)), '\n', 0 };
		say(d);
	}
}

static int
run(int change)
{
	int fd = (int)openat(DEVICE, change ? 2 /* O_RDWR */ : 0);
	u64 bytes, bytes2;
	int a = -1, b = -1;
	u64 at, bt;

	if (fd < 0) {
		say("hakios-slot: " DEVICE " cannot be opened (root?)\n");
		return 1;
	}
	if ((bytes = gpt_read(fd, 1, 0)) == 0) {
		say("hakios-slot: the primary GPT does not check out: nothing written\n");
		return 1;
	}
	for (u64 k = 0; k < bytes / 128; k++) {
		if (named(table[0] + 128 * k, "boot_a")) {
			a = (int)k;
		} else if (named(table[0] + 128 * k, "boot_b")) {
			b = (int)k;
		}
	}
	if (a < 0 || b < 0) {
		say("hakios-slot: boot_a or boot_b is not in " DEVICE "'s table: nothing written\n");
		return 1;
	}
	at = R64(table[0] + 128 * a + 48);
	bt = R64(table[0] + 128 * b + 48);
	attrs("boot_a (HakiOS)", at);
	attrs("boot_b (Android)", bt);
	if (!change) {
		return 0;
	}
	bytes2 = gpt_read(fd, R64(hdr[0] + 32), 1);
	if (bytes2 != bytes) {
		say("hakios-slot: the backup GPT does not check out: nothing written\n");
		return 1;
	}
	for (u64 k = 0; k < bytes; k++) {
		if (table[0][k] != table[1][k]) {
			say("hakios-slot: the backup GPT's table is not the primary's: nothing written\n");
			return 1;
		}
	}
	/*
	 * successful as well (bit 54): ABL falls back from an unbootable slot only to one marked successful -- with boot_a
	 * bootable, retries 7 but not successful, it stopped in fastboot instead (2026-11-21); boot_b, successful, is
	 * taken. (HakiOS does not mark its slot successful itself.)
	 */
	at = (at & ~(1ul << 55) & ~(7ul << 51)) | 7ul << 51 | 1ul << 54;
	bt |= 1ul << 55;
	for (int i = 0; i < 2; i++) {
		R64(table[i] + 128 * a + 48) = at;
		R64(table[i] + 128 * b + 48) = bt;
	}
	/* the backup first: a reset in between leaves the primary, which ABL reads, as it was */
	if (!gpt_write(fd, 1, bytes) || fsync(fd) != 0 || !gpt_write(fd, 0, bytes) || fsync(fd) != 0) {
		say("hakios-slot: A WRITE FAILED\n");
		return 1;
	}
	memset(table[0], 0, sizeof(table[0]));
	if (gpt_read(fd, 1, 0) != bytes || R64(table[0] + 128 * a + 48) != at || R64(table[0] + 128 * b + 48) != bt) {
		say("hakios-slot: THE PRIMARY GPT DOES NOT READ BACK AS WRITTEN\n");
		return 1;
	}
	say("hakios-slot: done: the phone starts HakiOS at the next boot\n");
	attrs("boot_a (HakiOS)", at);
	attrs("boot_b (Android)", bt);
	return 0;
}

void
_start_c(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	int change = 0, rc;

	if (argc > 1 && argv[1][0] == 't') {            /* to-hakios */
		change = 1;
	} else if (argc > 1 && argv[1][0] != 's') {
		say("usage: hakios-slot status | to-hakios\n");
		sys(93, 2, 0, 0, 0);
	}
	rc = run(change);
	sys(93, rc, 0, 0, 0);
	for (;;) {
	}
}

__asm__(".globl _start\n_start:\n\tmov x0, sp\n\tbl _start_c\n");
