/*
 * tfa_regs: the 16-bit registers of an I2C chip, read on a rooted Android phone -- the OnePlus 6T's
 * TFA9894 amplifier (bus 2, address 0x34) while Android plays, to set it up the same way from HakiOS
 * (devices/fajita/README.md, step 10). Toybox's i2cget reads bytes only.
 *
 *   tfa_regs BUS ADDRESS [FIRST [COUNT]]      (numbers in hexadecimal; default all 256 registers)
 *   tfa_regs BUS ADDRESS w REG VALUE          one register written
 *
 * One line per register: "REG VALUE". Each read is one I2C transfer (the register's number written,
 * two bytes read, the most significant first), through /dev/i2c-BUS; the kernel serialises it with the
 * chip's own driver. The one write there is (w) is for putting the chip back to its power-on state
 * (bit 1 of the TFA9894's register 0, I2CR), so that Android's driver sets it up from the start again
 * while the kernel's I2C trace records it.
 *
 * Built freestanding, with no C library, so that one binary runs on any arm64 Android:
 *   clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector -nostdlib -static \
 *       -fuse-ld=lld -o tfa_regs tools/tfa_regs.c
 */
typedef unsigned long u64;
typedef unsigned short u16;
typedef unsigned char u8;

#define SYS_openat      56
#define SYS_ioctl       29
#define SYS_write       64
#define SYS_exit        93
#define AT_FDCWD        (-100)
#define O_RDWR          2
#define I2C_RDWR        0x0707
#define I2C_M_RD        0x0001

struct i2c_msg {
	u16 addr, flags, len;
	u8 *buf;
};
struct i2c_rdwr_ioctl_data {
	struct i2c_msg *msgs;
	unsigned nmsgs;
};

static long
sys(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
	    x3 __asm__("x3") = d;

	__asm__ volatile ("svc #0" : "+r" (x0) : "r" (x8), "r" (x1), "r" (x2), "r" (x3) : "memory");
	return x0;
}

static void
put(const char *s, int n)
{
	sys(SYS_write, 1, (long)s, n, 0);
}

static unsigned
hex(const char *s)
{
	unsigned v = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	for (; *s; s++) {
		v = v * 16 + (unsigned)(*s <= '9' ? *s - '0' : (*s | 0x20) - 'a' + 10);
	}
	return v;
}

int __attribute__((used)) main_(int argc, char **argv);

int __attribute__((used))
main_(int argc, char **argv)
{
	static const char digits[] = "0123456789abcdef";
	char path[16] = "/dev/i2c-", line[16];
	unsigned bus, address, first = 0, count = 256;
	long fd;

	if (argc < 3) {
		put("usage: tfa_regs BUS ADDRESS [FIRST [COUNT]] (hexadecimal)\n", 58);
		return 2;
	}
	bus = hex(argv[1]);
	address = hex(argv[2]);
	if (argc == 6 && argv[3][0] == 'w' && argv[3][1] == 0) {
		u8 bytes[3] = { (u8)hex(argv[4]), (u8)(hex(argv[5]) >> 8), (u8)hex(argv[5]) };
		struct i2c_msg msg = { (u16)address, 0, 3, bytes };
		struct i2c_rdwr_ioctl_data data = { &msg, 1 };

		path[9] = (char)('0' + bus % 10);
		path[10] = 0;
		fd = sys(SYS_openat, AT_FDCWD, (long)path, O_RDWR, 0);
		if (fd < 0 || sys(SYS_ioctl, fd, I2C_RDWR, (long)&data, 0) < 0) {
			put("tfa_regs: not written\n", 22);
			return 1;
		}
		put("written\n", 8);
		return 0;
	}
	if (argc > 3) {
		first = hex(argv[3]);
	}
	if (argc > 4) {
		count = hex(argv[4]);
	}
	path[9] = (char)('0' + bus % 10);
	path[10] = 0;
	fd = sys(SYS_openat, AT_FDCWD, (long)path, O_RDWR, 0);
	if (fd < 0) {
		put("tfa_regs: cannot open the bus\n", 30);
		return 1;
	}
	for (unsigned r = first; r < first + count && r < 256; r++) {
		u8 reg = (u8)r, value[2] = { 0, 0 };
		struct i2c_msg msgs[2] = { { (u16)address, 0, 1, &reg }, { (u16)address, I2C_M_RD, 2, value } };
		struct i2c_rdwr_ioctl_data data = { msgs, 2 };
		int n = 0;

		line[n++] = digits[r >> 4];
		line[n++] = digits[r & 15];
		line[n++] = ' ';
		if (sys(SYS_ioctl, fd, I2C_RDWR, (long)&data, 0) < 0) {
			line[n++] = '-';
		} else {
			unsigned v = (unsigned)value[0] << 8 | value[1];

			for (int shift = 12; shift >= 0; shift -= 4) {
				line[n++] = digits[v >> shift & 15];
			}
		}
		line[n++] = '\n';
		put(line, n);
	}
	return 0;
}

void __attribute__((naked, noreturn))
_start(void)
{
	/* argc at [sp], argv at sp + 8 */
	__asm__ volatile ("ldr x0, [sp]\n add x1, sp, #8\n bl main_\n mov x8, #93\n svc #0\n");
}
