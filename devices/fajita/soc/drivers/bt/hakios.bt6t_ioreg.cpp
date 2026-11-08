/*
 * HakiosBluetooth6T: the IOService by which iOS's bluetoothd finds the 6T's Bluetooth controller (bt6t=5).
 *
 * bluetoothd (20F66) asks IOServiceGetMatchingService(IOServiceNameMatching("marconi-bt")), else "bluetooth", and
 * reads, from that service on (IORegistryEntrySearchCFProperty, plane IODeviceTree), as data:
 *   transport-encoding   u32: 0 USB, 1 H4, 2 H4DS, 3 H4BC, ... (its switch at 0x1001560a8; anything above 7 is USB)
 *   transport-speed      u32: the UART's rate
 *   local-mac-address    the controller's address, as it reads (most significant octet first)
 * With 1 (H4) it opens /dev/cu.bluetooth, else /dev/uart.bluetooth: the tty that hakios.bt6t.c makes over the UART
 * once the controller's firmware is in. This file only publishes the service; nothing matches it and it drives nothing.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <sys/errno.h>

extern "C" int HakiosBt6tPublish(const uint8_t * address_msb_first, uint32_t speed);

class HakiosBluetooth6T : public IOService
{
	OSDeclareDefaultStructors(HakiosBluetooth6T);
};

OSDefineMetaClassAndStructors(HakiosBluetooth6T, IOService);

static HakiosBluetooth6T * published;

/* Once, from hakios.bt6t.c's thread when the tty is there; 0 when the service is up, else an errno. */
int
HakiosBt6tPublish(const uint8_t * address_msb_first, uint32_t speed)
{
	HakiosBluetooth6T * made;
	IOService * platform;
	uint32_t encoding = 1;          /* H4 */

	if (published != NULL) {
		return 0;
	}
	made = OSTypeAlloc(HakiosBluetooth6T);
	if ((platform = IOService::getPlatform()) == NULL || made == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		return ENXIO;
	}
	made->setProperty("transport-encoding", &encoding, sizeof(encoding));
	made->setProperty("transport-speed", &speed, sizeof(speed));
	if (address_msb_first != NULL) {
		made->setProperty("local-mac-address", (void *) address_msb_first, 6);
	}
	if (!made->attach(platform)) {
		made->release();
		return EIO;
	}
	made->setName("bluetooth");
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return EIO;
	}
	made->registerService();
	published = made;
	IOLog("HakiosBluetooth6T: \"bluetooth\" published: H4 at %u baud\n", speed);
	return 0;
}
