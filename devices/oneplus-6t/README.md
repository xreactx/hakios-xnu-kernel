# Device: OnePlus 6T (fajita, A6013)

The second Qualcomm phone of HakiOS, after the POCO X5 Pro (`devices/poco-x5-pro/`). Everything that belongs to this
phone is here; the kernel, loader and user space are the common ones. `device.conf` holds the state of each part.

| What | Where |
|---|---|
| SoC | Qualcomm Snapdragon 845 (SDM845, "napali"; `qcom,msm-id <0x141 0x20001>`, `qcom,board-id <8 0>`) |
| UEFI for the phone | `uefi/`: `fajitaPkg`, Mu-Silicium's `enchiladaPkg` (OnePlus 6, the same board) with the 6T's name, model and 1080x2340 panel. Its vendor drivers are Mu-Silicium's own `Binaries/enchilada`: nothing to extract from the phone |
| The phone's device tree | `uefi/files/Resources/DTBs/fajita.dtb`: the phone's own, with `/chosen` emptied (the raw `fdt` pulled over adb carries the serial number and is git-ignored) |
| SoC boot-args | `soc.args`, from `pull-soc.sh` / `tools/soc-args.py` (13 of the 16; see below) |
| Run boot-args | `run.args`: added by `xl run` to every run on this phone (`DEVICE=oneplus-6t`), unless `XL_CMDLINE` gives the same name |
| The phone's firmware | **not in the repository**: `pull-firmware.sh` pulls it over adb into `fw-*/` at the top |

## How it differs from redwood

| | OnePlus 6T | POCO X5 Pro | What it means for the port |
|---|---|---|---|
| GIC, core numbering | GICv3 at `0x17A00000`/`0x17A60000`, `cpu@0..cpu@700` | the same | the loader's `platform_fajita` is redwood's with another name |
| UEFI framebuffer | `0x9D400000` (cont_splash_region) | `0xE1000000` | how the loader tells the two apart |
| UEFI load address | `0x9FC00000`, 3 MB | `0x9FD00000`, 2 MB | `mkxnuimg.py`'s table |
| Storage | UFS 2.1 at `0x1d84000` | UFS 3.1 | the same Qualcomm host controller; check `hakios.ufshc.c`'s addresses |
| Touch | Synaptics S3706B (RMI4), I2C on GPIO 49/50 (address `0x20`, IRQ GPIO 125, reset GPIO 99, 1.8 V GPIO 88, 3.0 V LDO28) | Goodix on SPI | `hakios.synaptics.c` (`synaptics=3`): I2C done by hand, the 3.0 V through RPMh |
| Buttons | all on the PM8998: power its PON's KPDPWR (`0x800`), volume up GPIO6, volume down **GPIO5** | PMK8350 PON and a PM7325 GPIO | `hakios.buttons.c`'s second layout (`buttons=2`); the same PMIC arbiter (v5, same addresses) |
| CPU clocks | the older OSM (`0x17d43000` small, `0x17d45800` big): big cores left at 825 MHz | EPSS | `hakios.cpufreq.c`'s `cpufreq_osm=2` |
| Temperatures | tsens v2, 21 sensors (TM `0xc263000`, `0xc265000`) | | `hakios.thermal.c` (`thermal=1`), reading only |
| GPU | Adreno 630 (chipid `0x6030001`) | Adreno 642L | `hakios.adreno.c`'s steps hold; registers, GMU firmware and RPMh levels are a630's (Linux `a6xx_gmu.c`, the a630 paths) |
| Wi-Fi | WCN3990, `icnss` | WCN6750 | Linux's ath10k_snoc, not ath11k: a new back end |
| Audio | ADSP, Q6 APR, WCD934x "tavil" | ADSP | rework of `hakios.adsp.c` / `hakios.sound.c` |
| Battery, charging | PMI8998 (fuel gauge, SMB2) | through the ADSP | through the loader from UEFI, as redwood's first way was, but with other protocols (step 7) |
| Haptics | PMI8998 haptics block (slave 3, peripheral `0xc0`), an LRA | Awinic AW86927 on I2C | `hakios.pmi_haptics.c` (`pmi_haptics=4`), through the PMIC arbiter; iOS's side is redwood's (`/dev/hakios-haptic`) |
| Cellular | IPA 3.5.1; no IPCC; the modem loaded through the MBA (`qcom,pil-q6v55-mss`) | IPA 4.11; IPCC; TrustZone PAS | the largest piece, last |

## soc.args

`tools/soc-args.py` reads SDM845's downstream names as well as SC7280's (`qcom,qmp-mbox`, `qcom,tcs-drv` with `qcom,drv-id
2`, `*_region` reserved memory, `qcom,smem`'s own `reg`). Three values cannot come from this tree and are left to the kernel's
defaults, which are redwood's and wrong here, so **do not pass `modem=1` on this phone yet**:

- `soc_ipcc`: the SDM845 has no IPCC; GLINK to the modem goes through the APCS mailbox (`0x17990000`). The kernel's modem path
  needs that first.
- `soc_cmd_db`: the tree only points at where AOP leaves its address at run time (`qcom,cmd-db` reg `0xc3f000c`).
  Read on the phone (the touch driver does, for the 3.0 V): `0x85fe0000`, 3520 bytes, as mainline Linux has it.
- `soc_pas_mpss`: the modem is not a TrustZone PAS image here; it is started through the MBA.

## Bring-up, in order

Each step is one run; `logs/` keeps what it said. Nothing is flashed: `fastboot boot` puts everything in RAM, and a reboot
gives the phone back to Android. Before the first one, back up `persist`, `modemst1`, `modemst2` and `fsg` from the phone
(root, `dd`) — the drivers write to hardware, and those partitions are the ones a phone cannot get back.

**1. A loader that builds.** The loader needs a newer LLVM than Ubuntu 22.04's clang 14 (its inline assembly fails with
"fixup value out of range" there, with or without this port): `sudo apt install clang-15 lld-15` and put
`/usr/lib/llvm-15/bin` first on `PATH`. The kernel (`make kernel`) and the iOS images (`make fetch images`) as in the main
README; check them in QEMU (`make qemu`) before the phone.

**2. The UEFI alone.**

```sh
devices/oneplus-6t/uefi/apply.sh        # Mu-Silicium/ with HakiOS's hooks, redwood's and fajita's platforms
sudo apt install python3.11 python3.11-venv     # build_uefi.py needs 3.11 (tomllib); Ubuntu 22.04's python3 is 3.10
python3.11 -m venv venv && venv/bin/pip install -r Mu-Silicium/pip-requirements.txt
cd Mu-Silicium && source ../venv/bin/activate && python3 build_uefi.py -d fajita -r RELEASE && cd ..
```

If `mu_nasm` fails to download (it comes through NuGet and wants Mono), point it at the system's nasm as
`devices/poco-x5-pro/uefi/README.md` shows.

Success: the HakiOS logo on the 6T's screen, then the loader's text. If the screen stays black, try Mu-Silicium's plain
`enchilada` build on the phone first: if that does not start either, the problem is the UEFI, not HakiOS.

**3. The loader on the phone.**

```sh
export DEVICE=oneplus-6t
./xl enter                  # Android -> fastboot -> UEFI -> the loader, which answers on USB as "xnuloader"
./xl memmap                 # the phone's memory map as UEFI reports it: 0xF0000000 must be free memory
```

The loader's log must say `platform fajita`. If it says `qemu-virt`, UEFI's framebuffer is not at `0x9D400000`: the log's
`display ... at ...` line gives the address to put in `xnuloader/loader.c`.

Done on 30 October 2026: `platform fajita`, display 1080 x 2340, and the payload's place (`0xF0000000`, 64 MB) inside free
memory (`a1769000 - 27c8a0000`, `logs/fajita-memmap.txt`).

The OnePlus 6's USB driver differs from redwood's in three ways, which the loader handles where `usb_old_driver` is set
(`platform_fajita`): it repeats "connected" each time the host opens the device (taken as a new cable, this dropped every
other command); it can leave a send unreported; and it never starts a reply queued a few milliseconds after the command
(`oem log`, which composes 2 MB first, now replies before composing). **Any other command that replies late will hang the
same way** — `getvar result` (which waits for the kernel) and the end of a download are the ones to watch in step 4.

`oem log` with no kernel running used to compose the loader's own log alone, and so lost the log of a kernel that had
just returned if the host asked before fetching it (`XL_FOLLOW` does, and `xl log` always). It now stages that log
again until the next run starts: `XL_FOLLOW` and `xl log` after a run are safe.

**4. XNU, one core, no drivers.**

```sh
./xl ramdisk userland/system.apfs
XL_SECS=300 XL_CMDLINE="-v cpus=1" ./xl run
```

Not `tools/modem-cycle.sh` / `make phone` yet: they add `hv_wdt_pet=1` (the hypervisor's watchdog, called with redwood's
SMC) and redwood's driver arguments. Success is the lock screen drawn in software. Then `cpus=8 bootcpu=7`.

Done on 30 October 2026, as `XL_CMDLINE="-v cpus=8 bootcpu=7 soc_vtimer=19"`: SpringBoard's home screen, seven cores
online, the run back in the loader after its 300 s. `soc_vtimer=19` is not optional here: the SDM845's virtual timer is
intid 19, not the architecture's 27, and without it the kernel gets one tick and no more (on core 0 it stops after
`lo0`, on any other core sooner). Two loader and image fixes came with it: uploads are received 16 KB at a time
(this driver sends no reply after a receive of 29696 bytes or more), and `userland/build.sh` sets `umask 022`
(launchd refuses SpringBoard's plist when its group may write it).
These and everything after are now in `run.args` -- `cpus=8 bootcpu=7 soc_vtimer=19 cpufreq_osm=2 thermal=1
buttons=2 battery=1 pmi_haptics=4 synaptics=3` -- so `DEVICE=oneplus-6t ./xl run` is enough (`XL_CMDLINE="-v cpus=1"`,
`synaptics=0` and the like still win over them).

The home screen takes a while, and runs shorter than about 2 minutes end before it: the screen shows the Apple
logo with the boot's text over it until SpringBoard checks in. That wait is SpringBoard's Control Center
waiting on LaunchServices, which rebuilds its database at every boot (`/private/var` is a fresh memory
filesystem; `userland/build.sh`). It took 67-72 s from the logo at the boot loader's clocks and takes about
30 s at 2.4 GHz (step 7). The slower cores still join 28 s after the others (`small_cores_after`): on this
phone starting them at once made no measurable difference (three runs each: 39.1 and 38.9 s from launchd to
the home screen).

**5. Storage.**

Done in part on 30 October 2026: the system image is read from the phone's flash instead of uploaded over USB.
`ufs=1` (a look through UEFI's storage driver, reading only) sees all six units and their GPTs. The 6T has no
`super` (no dynamic partitions), and its `userdata` (229 GB, ext4) cannot be cut down without root, so redwood's
`darwin` partition is not to be had. But Android's two system slots are 2.79 GB each, and the image
(`userland/system.apfs`, 2.36 GB) fits: it is flashed into the slot Android does **not** boot from, with the
phone's own bootloader, nothing else written:

```sh
fastboot getvar current-slot                    # here b: so system_a is the free one
fastboot flash system_a userland/system.apfs    # 94 s; Android boots from b as before
./xl enter && ./xl ramdisk-flash system_a       # after every restart, instead of "xl ramdisk": 12 s, not 104
```

`xl ramdisk-flash PART [FILE]` has the loader read the first `FILE`-size bytes of `PART` into the resident area
through UEFI's `ReadBlocks` (`oem ramdisk flash`, then `oem ramdisk flash-next` 256 MB at a time; 2.36 GB in
4.6 s) and compares the CRC-32 of a sample -- the first 4 KB of every MB and the last 4 KB -- with the file's: the
resident area is slow to the loader's own loads (a CRC of all of it took 10 minutes; that full CRC, once, was the
file's). The OnePlus 6's USB driver loses a question asked while the loader is busy -- it is never answered, a new
one asked after is at once -- so xl asks again after each step. An OTA would overwrite system_a; flash it again.
What it costs: Android's other slot is not an Android any more (no falling back to it until an update rewrites it).

The rest -- the kernel reading the flash itself (`ufs=2`, `root_apfs=`), iOS's data kept between boots -- needs
a partition of the port's own, as redwood's `darwin` (Apple's root container alone is 5.6 GB).

**6. Touch and buttons.**

Done on 30 October 2026. The touch screen (`hakios.synaptics.c`, `synaptics=3`) is a Synaptics S3706B speaking RMI4:
the driver drives I2C by hand on GPIO 49/50 (the QUP engine is not used), switches the chip's 3.0 V (LDO28) on
through RPMh, and maps each pin's 4 KB page of the TLMM alone (one 12 MB mapping panicked at boot). It tells F01
the chip is configured, reads F12's register descriptors for where the fingers are (10, 8 bytes each), watches the
interrupt pin every 5 ms -- and looks at what is pending every 50 ms as well, as the chip had a report waiting with
the pin high about 10 times in 2 minutes -- and posts each finger to `/dev/hakios-touch`. The chip reports the
screen's own pixels (corners seen: 63..1048, 25..2330). Taps, swipes, the edge swipes and two fingers work in
SpringBoard. Each run's end says how many touches there were. The driver does nothing for its first
`synaptics_after` seconds (25), for the reason given under "Sharing the PMIC bus and RPMh" below.

The buttons are `hakios.buttons.c` with `buttons=2`: the SDM845 has redwood's PMIC arbiter (version 5, the same
addresses), and all three keys are on the PM8998. Power is its power-on block's KPDPWR (`0x800`, the older kind:
bit 0), volume up GPIO6 (`0xc500`), and volume down **GPIO5** (`0xc400`) -- not RESIN, which mainline Linux and
the device tree's `pon_2` give: a hand pressing the key moved GPIO5 alone. Nothing is written to the PMIC. All three
were pressed by hand: volume up shows the volume indicator, and power turns the screen off and wakes it to the
lock screen. The keys answer from about 25 s into a run (`buttons_after`, below); `buttons_poll_ms=N` changes how
often they are read (16 ms).

*Sharing the PMIC bus and RPMh.* The loader's core is not idle while the kernel runs, and to the PMIC arbiter it is
the same system as the kernel (0): two transfers of one system at once make the arbiter drop one. For a run's first
24 s that core still takes interrupts (`NOTE_SECONDS` in `xnuloader/loader.c`), in which UEFI's PMIC and RPMh drivers
do their own work; with `battery=1` it reads the fuel gauge through UEFI every 5 s for the whole run. A dropped
transfer of the kernel's is one look missed; one of UEFI's may not be survivable. Reading the keys every 1 ms from the
start left the loader's core answering nothing: the kernel ran on at the home screen, its run's end was never answered,
and the phone had to be reset with its power key. So, on this phone:

- the buttons leave the bus alone for their first `buttons_after` seconds (25), and the touch driver does nothing for
  its first `synaptics_after` (25): it switches the chip's 3.0 V through the first command slot of the application
  processor's RPMh controller, the very slot UEFI's RPMh driver uses (found set after every cold start). Two writers
  in one slot's registers could send the always-on processor a mixed command. At the run's end the touch driver says
  the slot's state before it uses it: 0 is its own, anything else somebody else's;
- with `battery=1` the loader sets `bus_busy` in the shared battery block, waits 5 ms, reads the fuel gauge and clears
  it; the buttons look at it before each transfer, with interrupts off, and wait while it is set (a transfer of
  theirs ends within its 2 ms timeout). Reading every 1 ms with `battery=1`: 180,009 reads, 0 failed, 14 waited;
- the haptics block's writes (step 8) keep the same rules;
- apart from all that, about one read in 20,000 is dropped all run long with the loader's core quiet: something else
  on the bus, probably another of the SoC's processors. It is asked again at once (at 1 ms: 9 dropped, 9 asked again,
  0 failed).

**Any new driver here that uses the PMIC bus or RPMh must do the same**: stay off them for the first 25 s, and check
`hakios_battery_loader_bus_busy()` before each PMIC transfer.

**7. Clocks, temperatures, battery.**

Done on 30 October 2026.

*Clocks* (`cpufreq_osm=2`, in `hakios.cpufreq.c`). The SDM845's cores are clocked by the OSM (Linux
`qcom-cpufreq-hw`'s first variant: a table at `0x110` every 32 bytes, the wanted row at `0x920`), not redwood's
EPSS. The boot loader leaves the big cores -- the kernel's own among them -- at the bottom of their table, 825 MHz
of 2803, and the small ones at 1612 of 1766. Each group is asked for its highest row at or under
`cpufreq_osm_mhz` (default 2400; measured: core 7 824 MHz before, 2399 after), and put back to the row it was
found at when the run ends (the OSM keeps what it was last told). `cpufreq_osm=1` only looks. Allowing the top
row got 2649 MHz (the OSM holds the boost rows back), no measurable gain and a warmer idle phone, so 2400 stays.

*Temperatures* (`thermal=1`, `hakios.thermal.c`): the 21 tsens sensors, named as the device tree's thermal zones,
read only -- all at the start, the cores every 10 s, the most each reached at the end. A boot at 2.4 GHz goes from
about 33 C to 68-70 C at most (a big core) and settles at about 39 C. Nothing acts on them yet; the SoC's own
limits hardware (LMh) throttles by itself.

*Battery* (`battery=1`). As on redwood the loader asks UEFI's charger drivers and shares a read-only cache with
the kernel (`hakios.battery.c`), but the protocols differ and **were checked by disassembly before anything was
called**:

- `ChargerEx` (revision `0x10003`) has `xnuloader/efi.h`'s layout: battery and charger presence, voltage.
- `QcomCharger` (revision `0x10007`) is another release than redwood's: its fourth entry, the one redwood's loader
  calls to read the status, takes a 32-bit value and hands it to the PMIC's charger protocol -- a setter. **It is
  never called on this phone**, its `initialize()` included.
- The charge, current and temperature come from PmicDxe's fuel gauge (GUID `9aed0312-724a-4209-8620-f93de0bbd8fb`,
  revision `0x10016`, no header published): its third entry is `GetBatteryStatus(pmic, &info)`, and the 13
  functions under it reach the PMIC only through the SPMI protocol's `ReadLong`. The UEFI carries
  `PmicDxeWp.efi` -- `PmicDxeLa.inf` packages that one, not the `PmicDxeLa.efi` beside it -- and the loader calls it
  only when the revision and the entry's first instructions are the ones disassembled; the gauge is on PMIC 1.

"Charging" follows the current's sign (negative: into the battery). A computer's USB port does not keep up with the
phone at work: plugged in, it drew 716-735 mA from the battery while booting at 2.4 GHz.

**8. Vibration.**

Done on 30 October 2026. The motor is a linear resonant actuator on the PMI8998's haptics block (slave 3, peripheral
`0xc0`; type `0x15`, subtype `0x9`), `hakios.pmi_haptics.c`, in steps as the other drivers: `pmi_haptics=1` reads it
(arbiter channel 250, ours to write through; UEFI leaves Android's 2088 mV and 4255 us play rate in it), `=2` configures
it (800 mA, Android's brake pattern, direct play; each write read back, the registers put back at the run's end), `=3`
plays one pulse (`pmi_haptics_ms`, 100), and `=4` -- in `run.args` -- plays what iOS plays. iOS's side is redwood's: it
plays its haptics as waveforms on the audio plug-in's "Actuator" (`userland/src/audio_plugin.c`), which writes them to
`/dev/hakios-haptic`. The driver tells taps from vibrations as `hakios.haptic.c` does: a waveform after a pause is a tap,
a pulse of `pmi_haptics_tap_ms` (30); one going on past 60 ms holds the motor on while it lasts, 1 s at most, then as long
a rest. The strength is the block's maximum voltage, from five eighths of Android's 2088 mV for iOS's softest up to
2088 mV. `/dev/hakios-haptic` is made at start-up -- the plug-in opens it once, at iOS's first haptic -- but nothing is
played before the motor is configured at 25 s. Long-pressing icons and flipping switches in Settings, it was felt.

These are the only writes to a PMIC in this kernel. `hakios_spmi_write()` (`hakios.buttons.c`) refuses every
peripheral not in its `spmi_writable[]` list -- the haptics block alone -- and every channel this system does not
own; it keeps the bus rules above. A first version of the driver wrote to the console at the run's end, where
interrupts are off: that is a panic, and the panic's way out ran the run's end again, in a loop, until the loader gave
up (the motor had been stopped the first time). Whatever a driver says at the run's end goes to the loader's log only.

**9. GPU** (`gpu=1`): run `adreno=N` step by step with a630 values, as `hakios.adreno.c` describes.

**10. Then** the alert slider, Wi-Fi, audio; cellular last.

## Firmware

```sh
devices/oneplus-6t/pull-firmware.sh     # the phone in Android, on adb
```

Pulls the GPU (`a630_*`), Wi-Fi (`wlanmdsp.mbn`, board files), the audio DSP and the modem (with `mba.mbn` and its TFTP
`image/`). The vibration motor needs none; the cameras are not ported.
