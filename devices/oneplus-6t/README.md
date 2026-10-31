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
| Audio | ADSP (TrustZone PAS 1, 26 MB at `0x8c500000`), Q6 APR; the loudspeaker a TFA9894 on QUAT MI2S; WCD934x "tavil" on SLIMbus for the rest | ADSP; AW88261s on TDM | rework of `hakios.adsp.c` / `hakios.sound.c`, a TFA9894 driver (step 10) |
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
buttons=2 battery=1 pmi_haptics=4 synaptics=3 slider=3 adsp=1 sound=4` and the audio's `soc_*` -- so `DEVICE=oneplus-6t ./xl run` is enough (`XL_CMDLINE="-v cpus=1"`,
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

**9. GPU** (`gpu=1`): run `adreno=N` step by step with a630 values, as `hakios.adreno.c` describes. Started 30 October
2026; what the phone's tree says, against redwood's (Adreno 642L, SC7280):

| | OnePlus 6T (Adreno 630) | redwood |
|---|---|---|
| the 3D core | `0x5000000` (256 KB), CX debug `0x5061000`, `cx_misc` `0x509e000`; chip ID `0x6030001` | `0x3d00000` |
| the GMU | `0x506a000` (192 KB), its RPMh PDC `0xb200000`; SMMU contexts 4 and 5 of the GPU's SMMU | `0x3d69000` |
| the GPU's clock controller | `0x5090000` (`gpucc-sdm845-v2`); power switches CX `+0x106c`, GX `+0x100c` (Linux `gpucc-sdm845.c`) | `0x3d90000`, CX `0x3d9106c` |
| the GPU's SMMU | `0x5040000` (`kgsl-smmu-v2`) | `0x3da0000` |
| the zap shader | TrustZone image 13, into `gpu_region` (`0x8c415000`, 8 KB, reserved by the loader) | |
| firmware (`fw-gpu/`, from `/vendor/firmware` with root) | `a630_sqe.fw` (32 KB), `a630_gmu.bin` (32 KB), `a630_zap.mdt`/`.b00`-`.b02` | `a660_*` |

The real difference is the GMU: the a630's is the older one (`qcom,gpu-quirk-hfi-use-reg`; in Linux's `a6xx_gmu.c` the
`legacy` paths): its firmware is a plain image for its instruction memory, its messages the first HFI. Steps 1 to 12
(reads, the CX power switch, clocks, the SMMU, TrustZone's zap check) want new numbers; steps 13 to 17 (the GMU's
firmware, start, HFI, the 3D core's power) a630 code, from `a6xx_gmu.c`'s legacy branches.

`soc_gpu=630` (with `soc_cmd_db=0x85fe0000`) has `hakios.adreno.c` use the SDM845's numbers and stop after the steps
ported so far (`PORTED_A630`). Steps 1 to 3 work (run 235420): the GPU's clocks in the global clock controller where
`gcc-sdm845.c` has them (the PLL vote at `0x52004` with the GPU's bits set, the memory's clock at `0x44038`, off);
RPMh's `gfx.lvl` (levels up to 416), `cx.lvl`, `mx.lvl`; no fuse (Linux reads no speed bin for the a630); and the GPU's
clock controller as UEFI leaves it -- both power switches off (`0x222001`), the GX clamp on, the GMU's clocks off: the
GPU entirely down, nothing in the way of step 4. (A first try also read `gpu_iref` at `0x8c010`, past the 512 KB the
driver maps of the clock controller: a panic of our own making. That run's log came back only through `logdump`.)

Step 4 works (run 235850): the collapse bit of `gpu_cx_gdsc` cleared, the switch reads `0xf8222000` and its
hardware-control register `0x80001fe0` (bit 31 of both: on); 20 s alive after it, then switched off again at the
steps' end. On the a630 step 4 now notes that it switched CX on, and the end switches it off (redwood only did that
with the GMU's address space up); a CX found on is only switched off first, not taken down by `stale_stop()`, whose
GMU, sequencer and SMMU are redwood's addresses.

Step 5 works (run 000137, 31 October): the 300 MHz tap asked for (bit 16 of `0x52004`), memnoc and the memory's clock
on (both voted: their status is not required), snoc on; the GMU's clock source to the tap / 1.5 (`0x602`: 200 MHz, the
same as redwood's); `cxo` and `cx_gmu` running (their status bits are halt checks Linux trusts). The SDM845 has no hub,
AHB or CB clock in the GPU's controller, and redwood's frequency counter is its vendor kernel's: not used here.

**10. Audio** (started 30 October 2026).

The loudspeaker needs neither the WCD934x codec nor SLIMbus: it is an NXP **TFA9894** smart amplifier on I2C (the
serial engine at `0x890000`, address `0x34`, reset GPIO 69; the `max98927` beside it in the tree is disabled), fed by
the audio DSP over the quaternary MI2S (Android's mixer paths: every speaker playback is `QUAT_MI2S_RX`). The codec
is for the earpiece, the headphones and the microphones, later. So, in this order:

1. the audio DSP up: `hakios.adsp.c` with the SDM845's numbers. TrustZone PAS image 1, as redwood's; its memory
   `0x8c500000`, 26 MB (reserved-memory `adsp_region`); last words SMEM item 423; GLINK's shared memory at
   `0x86000000`. No IPCC: every doorbell is a bit of the APCS IPC register `0x1799000c` -- GLINK bit 8
   (interrupt 156), SMP2P bit 10 (interrupt 158; SMP2P "master-kernel"/"slave-kernel" with processor 2), the AOP's
   mailbox (`0xc300000`) bit 0. Its rail is CX (`vdd_cx`, level 385), not LCX/LMX, and it goes through RPMh, so not
   before the 25 s that UEFI uses it (step 6).
2. `hakios.sound.c`'s AFE port on QUAT MI2S instead of redwood's first TDM.
3. the TFA9894 set up from Android's container `/vendor/etc/firmware/tfa98xx.cnt` (in `fw-audio/`).

The DSP's firmware (`adsp.mdt`, `adsp.b00`-`b13`, 11 MB) comes off the flash with `xl flash-read` (see Firmware).

`hakios.adsp.c` takes the SDM845's numbers from `soc.args` (`soc_adsp_mem`, `soc_adsp_mem_size`, `soc_apcs_ipc`,
`soc_adsp_cx`, with `soc_smem`, `soc_rsc`, `soc_aoss`; `soc_cmd_db=0x85fe0000` by hand) and, on a SoC without IPCC,
does only the steps ported so far (`PORTED_APCS`). `xl run` does not pass `soc.args`, so give them in `XL_CMDLINE`,
with a root built `ADSP=1 REST=1 userland/build.sh` (the firmware in the small image):

    XL_SECS=45 XL_CMDLINE="-v adsp_step=2 soc_smem=0x86000000 soc_cmd_db=0x85fe0000 soc_rsc=0x179e0000 soc_aoss=0xc300000
      soc_adsp_mem=0x8c500000 soc_adsp_mem_size=0x1a00000 soc_apcs_ipc=0x1799000c soc_adsp_cx=1" ./xl run

Steps 1 and 2 work (30 October 2026, run 201813): the partition of the application processor and the DSP is in the
shared memory (`0x861af000`, 192 KB) but empty -- the DSP has not run since power-on, its doorbells are quiet -- and the
image is whole, 13 segments filling the 26 MB exactly. Unlike redwood's, the partition has **no GLINK items (478,
479)**: on this phone our side must allocate them (as Linux's `qcom_glink_smem` does), which step 6 has to learn.
Step 3 works too (run 202214): from step 3 on nothing is done before `adsp_after` (25) seconds from the driver's start,
as touch waits out UEFI; then the AOP's message RAM at `0xc300000` reads "MAIL", version 1, our mailbox at `0x128`
(`0x2d8` bytes), and link and channel open with every acknowledgement, the doorbell being the APCS's bit 0. The system
starts on to the home screen as before. Step 7 (RPMh, TrustZone) waits the same way.

Before step 5 writes the DSP's memory: Mu-Silicium's UEFI reports nearly all of Android's carveouts here as free
(`0x8ab00000`-`0x97c00000`, type 7 in `logs/fajita-memmap.txt`). The kernel never gets them (its window is the largest
free block, above `0xa1769000`), but UEFI runs on during a run and could allocate there. So the loader claims the
thirteen that lie wholly in free memory -- qseecom, camera, ips_fw, ipa_gsi, gpu, adsp, wlan_fw, modem, video, cdsp,
mba, slpi, pil_spss -- as reserved at its start (`fajita_reserved` in `xnuloader/loader.c`); its log says
"reserved: 13 of the platform's regions claimed from UEFI, 0 not", and `xl memmap` (after `xl reload`: after a run
`oem log` gives the kernel's log) shows them as type 0.

Step 4 works as it is (run 202443): TrustZone takes the header of image 1 (`0x42000201`, the signature checked) and
the place, 26 MB at `0x8c500000` (`0x42000202`), both answering 0. The mailbox, opened by the run before, is found
open.

Step 5 works (run 203147, with the regions reserved): the 13 segments into `0x8c500000`, 11.1 MB of the image and
16.1 MB of zeroes, read back with no word different. One change: the 6T's segment 9 is 84 bytes, so a segment's last
word of eight may now run past its end (into the gap before the next one, which starts at a page).

Step 6 works (run 203504): under the shared memory's hardware lock (`0x1f43000`, the same as redwood's: 8 locks of
`0x1000`, the shared memory's is 3), our SMP2P item 429 is made at `0x861af030`, and -- what redwood's boot loader does
for it, and this phone's does not -- GLINK's descriptor 478 (32 bytes) and our queue 479 (16 KB), zeroed, as Linux's
`qcom_glink_smem_register()` makes them. They stay in the shared memory until the phone restarts; a later run finds
and uses them.

**Step 7: the audio DSP runs** (run 204013). CX through RPMh -- the tree asks for 385, the command database's levels
end at 384, so the highest (position 7), as Linux takes it; never given back --, "adsp: on" to the AOP, and TrustZone
authenticates the image and lets the DSP out of reset. Its SMP2P item appears after 3 ms, "up" (0x2) after 10, "my own
power" (0x6) after 18; it makes its items 443, 480 (its GLINK queue) and 606. At the steps' end it acknowledges the
stop within 2 ms, TrustZone shuts it down, and the AOP hears "off"; the system goes on to the home screen. Touch's
requests to RPMh now take the same lock as the DSP's (one command slot, TCS 0, for both), and the DSP's request sets
the slot's completion enable as touch's does. Not yet seen: the DSP's doorbells to us (GIC 156 and 158 never pending
here); the link polls the shared memory every millisecond all the same.

**Steps 8 to 11: the audio services are up** (run 204424, all eleven steps on this phone). GLINK: versions agreed, and
of the six channels the DSP opens, "IPCRTR" and "apr_audio_svc" are open both ways. QRTR: hello both ways, the DSP is
node 5 with 7 services. APR: the DSP's core service says the audio services are up after 2.2 s and lists them. Then
it stops when asked (1.4 ms) and is shut down. Started a second time in the same power-on, it says it has restarted
(SMP2P flag), which is answered. Run as

    XL_SECS=100 XL_CMDLINE="-v adsp_step=11 adsp_new=8 soc_smem=0x86000000 soc_cmd_db=0x85fe0000 soc_rsc=0x179e0000
      soc_aoss=0xc300000 soc_adsp_mem=0x8c500000 soc_adsp_mem_size=0x1a00000 soc_apcs_ipc=0x1799000c soc_adsp_cx=1" ./xl run

That run's log did not come back over USB ("usb: no completion for a send in 1 s: aborted" at the return, and
`oem log` then gave the loader's own); the loader had written it to the `logdump` partition, read back with
`./xl flash-read logdump 0 e4000 F` and `strings`. Next: the speaker's path, `hakios.sound.c` on QUAT MI2S.

What `hakios.sound.c` needs instead of redwood's numbers (its first TDM, a group, port `0x9000`), from the phone's tree
(`qcom,msm-dai-q6-mi2s-quat`, the `quat_mi2s*_active` pins, `qcom,msm-audio-ion`) and Linux's `pinctrl-sdm845.c`:

| | OnePlus 6T | redwood |
|---|---|---|
| interface, port | quaternary MI2S, AFE port `0x1006` (QUAT_MI2S_RX), no group; configured by AFE's I2S parameter (`0x1020d`) | primary TDM, port `0x9000` in group `0x9100` |
| lines | playback on SD1 (`qcom,msm-mi2s-rx-lines = 2`), the amplifier's feedback on SD0 (`tx-lines = 1`) | |
| bit clock | QUAD_MI2S_IBIT (`0x106`), 48 kHz x 16 bits x 2 = 1.536 MHz (below) | PRI_TDM_IBIT (`0x200`), 3.072 MHz |
| pins | GPIO 58 bit clock, 59 frame sync (output high), 60 SD0, 61 SD1: function 1 (`qua_mi2s`), 8 mA, no pull; north tile (`0x3400000 + 0x500000`, a page each) | GPIO 97-100, function 1 |
| the DSP's view of our memory | SMMU stream `0x1821` (mask `0xf`: bit 32 of its addresses, as redwood's `0x1801`) | `0x1801` |

The format is **16 bits**, seen on the running phone: Android's TFA driver logs every start's `hw_params` --
"Requested rate: 48000, sample size: 16, physical size: 16" -- although `/vendor/etc/audio_platform_info.xml` gives the
speaker `bit_width="24"` (the HAL's, not what reaches the port). So `soc_sound_bits=16`: 1.536 MHz.

**The speaker's path up to the amplifier works** (30 October 2026). `hakios.sound.c` takes the port by boot-args
(`soc_sound_port=0x1006 soc_sound_clock=0x106 soc_sound_bits=24 soc_audio_stream=0x1821`): a port in `0x1000`-`0x10ff`
is MI2S, no group, configured by AFE's I2S parameter, and its pins are the quaternary MI2S's, a page each; there only
`sound=1` and `sound=2` (levels 3 and 4 are redwood's amplifiers). With `adsp=1` (the DSP on its own thread, kept up;
the step limit no longer turns that into `adsp_step`):

- `sound=1` (run 225530): the bit clock at 3.072 MHz, the port's I2S parameter and its start all done; at the pins the
  frame sync changes 1552 times in 16.2 ms -- 48 kHz -- and the bit clock too fast to follow; the pins put back as UEFI
  left them (`0x1`, pulled down).
- `sound=2` (run 230051): the SDM845's SMMU is half redwood's size (2 x 64 pages of 4 KB, 512 KB; now accepted); stream
  `0x1821` had nothing set up, match register 8 and context 3 were free and took our tables; the DSP maps our periods,
  the device path (COPP 0), the stream and its routing are done, and a 1 kHz tone plays for 3 s: 159 periods written
  and given back, no error, no underflow, 48443 frames a second, the SMMU refusing nothing. Everything is put back.

    XL_SECS=120 XL_CMDLINE="-v adsp=1 sound=2 soc_sound_port=0x1006 soc_sound_clock=0x106 soc_sound_bits=24
      soc_audio_stream=0x1821 soc_smem=0x86000000 soc_cmd_db=0x85fe0000 soc_rsc=0x179e0000 soc_aoss=0xc300000
      soc_adsp_mem=0x8c500000 soc_adsp_mem_size=0x1a00000 soc_apcs_ipc=0x1799000c soc_adsp_cx=1" ./xl run

Nothing is heard: the amplifier (TFA9894) is next. (Those runs had 24 bits; it is 16, as below.)

**The amplifier under Android, rooted** (30 October 2026; `magisk_patched-31100_llnZs.img`, the phone's own 2021-11
build, booted once with `fastboot boot`, nothing flashed). NXP's driver v6.5.2 has it on I2C bus 2 (the serial engine at
`0x890000`), address `0x34`: a TFA9894 (register 3 = `0x1a94`), container `tfa98xx.cnt` with five profiles --
`MUSIC_48000` (what music plays with), `VOICE_48000`, `MMITest_48000`, `dsp_bypass_P` -- loaded at boot, when its first
start sends the "cold boot patch". `tools/tfa_regs.c` reads its 16-bit registers (toybox's `i2cget` reads bytes); the
dumps idle and while a tone plays through QUAT_MI2S (`tinyplay`, MultiMedia1) are in `fw-audio/tfa-dumps/`. Playing
changes only the control register `0x00` (`0xa275` to `0xa27c`: out of power-down, amplifier and boost on), `0x04`
(`0x40` to 0) and status registers: everything else was set up at boot. HakiOS starts the phone cold, so it must send
that whole set-up itself.

**The set-up, recorded.** The phone is rooted on flash (`boot_b` is that Magisk image, byte for byte). Android's init
switches tracing off at 4.6 s, and the amplifier's set-up comes at 7 s; a soft reset of the chip (register 0, bit 1)
does not make the driver do it again (it fails to start, ten times, until the phone restarts), and unbinding and
binding its driver brought the phone down 26 s later. What worked: for one boot, a Magisk script in
`/data/adb/post-fs-data.d/` switched the I2C trace (bus 2) back on at 6.0 s, and removed straight after. From the
soft reset (`00=0002`) to the amplifier on (`00=a27c`), 0.19 s: about 25 register writes; the patch, seven blocks of
up to 250 bytes into register `0x91` (with `0x90` choosing the chip's DSP memory); twelve parameter messages to the
chip's DSP, each a block into `0x92`, the trigger `90=0112`, `0x93` read back as `0x0300`, the answer from `0x92`
(`000000`); then `e1`/`e0` and `00=a27c`. Each later start is `04=0000 00=a274 01=15d6 e0=1 e1=1 00=a27c`, each stop
`04=0040 00=a27d 00=a275`.

`tools/tfa-trace2replay.py` turns the trace into a file of those writes and the reads with what Android got back
(`fw-audio/tfa9894-coldboot.replay`: 139 writes, 121 reads; the vendor's, not committed). The trace prints 64 bytes of
a transfer at most: the twelve longer ones (the patch's blocks, five messages) are taken whole from `tfa98xx.cnt`,
where they lie as sent -- one message is there twice, differently, and the copy that follows the one before it is
taken, as the driver reads a profile's items in order.

For the kernel's driver: the amplifier's bus is the serial engine at `0x890000` on GPIO 89 and 90 (function `qup4`,
2 mA, no pull; Linux's `pinctrl-sdm845.c`: south tile), done by hand as the touch screen's; its reset GPIO 69 (east
tile); and the port at 16 bits, the format Android plays.

**Sound** (30 October 2026, run 233354): `hakios.tfa9894.c` replays that set-up over I2C done by hand on GPIO 89 (data)
and 90 (clock), after a pulse of the reset (GPIO 69 high for 1 ms, as the driver's probe does), from
`fw/tfa9894-coldboot.replay` in the small image (`ADSP=1 userland/build.sh` puts it there). `tfa9894=1` looks (the pins,
register 3 = `0x1a94`), `tfa9894=2` sets it up and powers it down (run 233243: 139 writes taken, 10 of 121 reads not
as Android's -- register `0x10` one bit apart while there are no clocks, `0x93` `0x0300` for `0x0200` three times --
and the registers then as in Android's idle dump). With `sound=3` (and `soc_sound_bits=16`) the sound's path sets it
up, switches it on as Android does once the port's clocks run -- register `0x10` reads `0x785e`, Android's value while
it plays -- and a 1 kHz tone at -20 dBFS plays for 3 s: **heard from the speaker**. 159 periods, no error, 48400 frames
a second; then the amplifier is muted and powered down and everything put back.

    XL_SECS=120 XL_CMDLINE="-v adsp=1 sound=3 soc_sound_port=0x1006 soc_sound_clock=0x106 soc_sound_bits=16
      soc_audio_stream=0x1821 soc_smem=0x86000000 soc_cmd_db=0x85fe0000 soc_rsc=0x179e0000 soc_aoss=0xc300000
      soc_adsp_mem=0x8c500000 soc_adsp_mem_size=0x1a00000 soc_apcs_ipc=0x1799000c soc_adsp_cx=1" ./xl run

**iOS's sound through the speaker** (`sound=4`, run 233909 with `RC_JOBS=rc.jobs.speakers`): the amplifier is set up
once, switched on when iOS writes something that is not silence (29-59 ms after it, register `0x10` at `0x785e` every
time) and off after three seconds of silence, as redwood's two are; a tone, the lock sound, keyboard clicks and a
fanfare, 8162 periods without an error or an underflow. The amplifier has no volume of ours, so the volume
(`sound_below`, 30 dB under Android's level by default, and iOS's own setting) is applied to the samples after the
guard (`gain_of()` in `hakios.sound.c`); the speaker's protection is the amplifier's own DSP's.

Since then `adsp=1 sound=4` and the `soc_*` they need are in `run.args`: every run has sound, given a small image
built `ADSP=1 REST=1 userland/build.sh` (the DSP's firmware and the amplifier's set-up are in it; without them the DSP
says so and the rest of the system starts as before). A plain run (234407) brings the DSP up at 30 s and the sound's
path after it. The amplifier (TFA9894) comes after: a tone through the port with the DSP answering every command is the first mark.

**11. Then** Wi-Fi; cellular last.

## Firmware

```sh
devices/oneplus-6t/pull-firmware.sh     # the phone in Android, on adb
```

Pulls the GPU (`a630_*`), Wi-Fi (`wlanmdsp.mbn`, board files), the audio DSP and the modem (with `mba.mbn` and its TFTP
`image/`). The vibration motor needs none; the cameras are not ported. On a phone without root, the firmware
partition is not readable from adb; the loader reads it instead (`xl flash-read modem_b ...`, the script's comment
says how): that is how `fw-audio/`, `fw-wifi/` and `fw-modem/` were filled on 30 October 2026.
