/*
 * hakios-bootctl: the phone's own boot control (Qualcomm's bootctrl.sdm845.so, the library Android's
 * android.hardware.boot service and its OTA updates use) asked directly -- for the way back to HakiOS from Android
 * (the Magisk module's Action). Android 11 has no bootctl, and Magisk's needs a newer Android.
 *
 *   hakios-bootctl status    the slots as the library sees them
 *   hakios-bootctl set N     setActiveBootSlot(N): what "fastboot set_active" does (0 = slot a, HakiOS)
 *
 * A dynamic executable for Android's linker, linked against the phone's own libc.so and libdl.so (copied into
 * sysroot/ by build.sh); no NDK. Its start is crtbegin's: __libc_init with this main.
 */
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned long uint64_t;
#define NULL ((void *)0)

extern int printf(const char *, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern int strcmp(const char *, const char *);
extern int atoi(const char *);
extern void __libc_init(void *raw_args, void (*onexit)(void), int (*main)(int, char **, char **), const void *structors);

struct hw_module_t {
	uint32_t tag;
	uint16_t module_api_version, hal_api_version;
	const char *id, *name, *author;
	void *methods, *dso;
	uint64_t reserved[32 - 7];
};

struct boot_control_module {
	struct hw_module_t common;
	void (*init)(struct boot_control_module *module);
	unsigned (*getNumberSlots)(struct boot_control_module *module);
	unsigned (*getCurrentSlot)(struct boot_control_module *module);
	int (*markBootSuccessful)(struct boot_control_module *module);
	int (*setActiveBootSlot)(struct boot_control_module *module, unsigned slot);
	int (*setSlotAsUnbootable)(struct boot_control_module *module, unsigned slot);
	int (*isSlotBootable)(struct boot_control_module *module, unsigned slot);
	const char *(*getSuffix)(struct boot_control_module *module, unsigned slot);
	int (*isSlotMarkedSuccessful)(struct boot_control_module *module, unsigned slot);
};

static int
main(int argc, char **argv, char **envp)
{
	void *lib = dlopen("/vendor/lib64/hw/bootctrl.sdm845.so", 2 /* RTLD_NOW */);
	struct boot_control_module *m;
	unsigned n;

	(void)envp;
	if (lib == NULL) {
		printf("hakios-bootctl: %s\n", dlerror());
		return 1;
	}
	m = dlsym(lib, "HMI");
	if (m == NULL || m->common.tag != 0x48574d54 /* HWMT */) {
		printf("hakios-bootctl: no boot control module in the library\n");
		return 1;
	}
	if (m->init != NULL) {
		m->init(m);
	}
	n = m->getNumberSlots(m);
	printf("hakios-bootctl: %s (%s): %u slots, the current one %u\n", m->common.name, m->common.author, n,
	    m->getCurrentSlot(m));
	for (unsigned s = 0; s < n && s < 4; s++) {
		printf("  slot %u (%s): bootable %d, successful %d\n", s, m->getSuffix(m, s), m->isSlotBootable(m, s),
		    m->isSlotMarkedSuccessful(m, s));
	}
	if (argc > 2 && strcmp(argv[1], "set") == 0) {
		unsigned slot = (unsigned)atoi(argv[2]);
		int r;

		if (slot >= n) {
			printf("hakios-bootctl: no slot %u\n", slot);
			return 1;
		}
		r = m->setActiveBootSlot(m, slot);
		printf("hakios-bootctl: setActiveBootSlot(%u): %d\n", slot, r);
		if (r != 0) {
			return 1;
		}
		for (unsigned s = 0; s < n && s < 4; s++) {
			printf("  slot %u (%s): bootable %d, successful %d\n", s, m->getSuffix(m, s), m->isSlotBootable(m, s),
			    m->isSlotMarkedSuccessful(m, s));
		}
	}
	return 0;
}

void _start_c(void *raw_args) __attribute__((noreturn));
void
_start_c(void *raw_args)
{
	static const struct { void (**preinit)(void), (**init)(void), (**fini)(void); } structors = { 0, 0, 0 };

	__libc_init(raw_args, NULL, main, &structors);
	__builtin_unreachable();
}

__asm__(".globl _start\n.type _start,%function\n_start:\n\tmov x0, sp\n\tb _start_c\n");
