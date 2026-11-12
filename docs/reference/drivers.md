<!-- Made by tools/docs-gen.py from drivers/*/conf and the SoC and device repositories' drivers. Do not edit: change that, then run tools/docs-gen.py. -->


# Drivers

Every driver the kernel can be built with. `CONFIG_HAKIOS_<NAME>=y` in a defconfig builds it in; [Drivers in the configuration](../drivers-config.md) says how, and [Writing a driver](../porting/drivers.md) how to add one.

## Always built

| File | What |
|---|---|
| `hakios.audio.c` | /dev/hakios-audio, the sound Darwin plays, to whatever plays it; and where the camera, flashlight, motor and speaker drivers are started from |
| `hakios.version.c` | kern.hakios.version, and the version in the boot log |
| `hakios.platform.c` | what the drivers share (the locks of the PMIC arbiter, the always-on processor, RPMh and the SMMU), and kern.hakios.drivers, the list of the drivers built in |
| `hakios.stubs.c` | what the rest of the kernel calls in a driver that is left out: nothing, and "not there" |
| `hakios.stubs.cpp` | the same, for the calls that are C++ (the display asking whether the GPU is done with a surface) (with XNU's `iokitcpp`) |
| `hakios.input.c` | /dev/hakios-touch, the queue of touches and buttons, and where the phone's drivers are started from |
| `hakios.net.c` | en0, an interface that is Ethernet to the rest of the kernel, for whatever link carries its frames |
| `hakios.image.c` | a file of one of the RAM disks by path, for the drivers that load firmware before there is a root |
| `hakios.sparse.c` | /dev/rsp0, a block device over an image that holds only some of its blocks (apfs_sparse=) |
| `hakios.apfs_check.c` | a walk of the APFS volume on /dev/rvblk0 through VFS, to compare with tools/apfs/apfs.py (with XNU's `apfs`) |

## audio

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_ADSP`**<br>the audio DSP started and linked (adsp=1, adsp_step=N); the battery's state comes through it too. Needs the DSP's firmware from the phone (fw-audio/) | `hakios.adsp.c` |  | `adsp=`, `adsp_after=`, `adsp_again=`, `adsp_apr_wait=`, `adsp_keep=`, `adsp_listen=`, `adsp_new=`, `adsp_pause=` and 12 more | built |  | built | |
| **`HAKIOS_SPEAKER`**<br>the two speaker amplifiers on I2C, and their watch of temperature and battery | `hakios.speaker.c` |  | `speaker_probe=` | built |  | built | |
| **`HAKIOS_SOUND`**<br>the speakers playing (sound=N): what the DSP's services are told, the feeder, and the speakers' guard that limits what the amplifiers get | `hakios.sound.c`<br>`hakios.guard.c` | `HAKIOS_ADSP`, `HAKIOS_SPEAKER` | `soc_audio_stream=`, `soc_sound_bits=`, `soc_sound_clock=`, `soc_sound_port=`, `sound=`, `sound_below=`, `sound_earpiece=`, `sound_gain=` and 4 more | built |  | built | |
| **`HAKIOS_MIC`**<br>the microphones, through the audio DSP (mic=N) | `hakios.mic.c` | `HAKIOS_ADSP`, `HAKIOS_SOUND` | `btslim_rate=`, `mic=`, `mic_adc=`, `mic_input=`, `mic_main=`, `mic_secs=`, `mic_tap=`, `mic_test=` and 1 more | built |  | built | |
| **`HAKIOS_VIRTIO_SOUND`**<br>QEMU's virtio sound device as the player of /dev/hakios-audio (virtio_sound=1). QEMU only | `hakios.virtio_sound.c` |  | `virtio_sound=` | built | built | built | |

## bluetooth

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BT`**<br>the Bluetooth half of the WCN6750: step one, a read-only look at its three pins (bt=1) | `hakios.bt.c` |  | `bt=`, `bt_again=` |  |  | built | |

## camera

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_CAMERA`**<br>the cameras (cam=N): the sensors on the CCI bus and the ISP's interface. The sensors, their tables (fw-camera/) and the pins are the POCO X5 Pro's | `hakios.camera.c` |  | `cam_after=`, `cam_fake=`, `cam_fake_fps=`, `cam_frames=`, `cam_gain=`, `cam_keep_mb=`, `cam_mode=`, `cam_pattern=` and 4 more |  |  | built | |
| **`HAKIOS_TORCH`**<br>the flashlight: the flash LED module of the PM8350C (torch=1); it stays off while the battery is too warm | `hakios.torch.c` |  | `torch_max_s=`, `torch_secs=` |  |  | built | |

## core

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SOCINFO`**<br>the chip's identity from the firmware's shared memory: kern.hakios.soc_* (socinfo=1). Any Qualcomm SoC that keeps SMEM item 137; where the shared memory is comes from soc_smem= | `hakios.socinfo.c` |  | `soc_smem=`, `soc_smem_size=`, `socinfo=` |  |  | built | |

## gpu

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_ADRENO`**<br>the Adreno 642L of the SC7280 woken step by step (adreno=N), and HakiosGPU, the IOKit service Mesa, Metal and the window server draw through (gpu=1, adreno_keep=1; docs/reference/metal.md). Needs the GPU's firmware from the phone (fw-gpu/) | `hakios.adreno.c`<br>`hakios.gpu.cpp` |  | `adreno=`, `adreno_blind=`, `adreno_dirty=`, `adreno_fine=`, `adreno_gfx=`, `adreno_keep=`, `adreno_only=`, `adreno_pause=` and 13 more | built |  | built | |

## haptic

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_HAPTIC`**<br>the vibration motor: an Awinic AW86927 driving a linear resonant actuator (haptic=N), its waveforms from fw-haptic/ | `hakios.haptic.c` |  | `haptic=`, `haptic_effect=` |  |  | built | |

## input

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_INPUT`**<br>QEMU's virtio tablet and keyboard as the touch screen and the buttons (virtio_input=1). QEMU only | `hakios.virtio_input.c` |  | `virtio_input=` | built | built | built | |
| **`HAKIOS_GOODIX`**<br>the touch screen: a Goodix GT9916 ("Berlin-D") or a FocalTech FT3680 on SPI (touchscreen=1). The bus, pins and supplies are the POCO X5 Pro's; another phone with one of these controllers changes those | `hakios.goodix.c` |  | `touch_flip=`, `touch_poll_us=`, `touch_rate=`, `touch_spi_hz=`, `touchscreen=` |  |  | built | |
| **`HAKIOS_BUTTONS`**<br>the power and volume keys, read from the PMIC through the SPMI arbiter (buttons=1) | `hakios.buttons.c` |  | `buttons=`, `buttons_after=`, `buttons_poll_ms=`, `buttons_steps=`, `press_power=`, `press_power_ms=`, `touchscreen=` | built |  | built | |

## modem

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_QMI`**<br>QMI messages over the IPC router: how the Wi-Fi firmware's service and the modem's are spoken to | `hakios.qmi.c` |  |  | built |  | built | |
| **`HAKIOS_MODEM`**<br>the modem (modem=1): started, its file systems served from the flash, the IPA's data path as en1, the SIM and the network registration. Research; needs fw-modem/ from the phone | `hakios.mpss.c`<br>`hakios.cell.c`<br>`hakios.nas.c`<br>`hakios.ipa.c` | `HAKIOS_QMI`, `HAKIOS_UFS` | `cell_dns=`, `cell_fake=`, `cell_ping=`, `cell_ping_size=`, `cell_ping_to=`, `modem=`, `modem_after=`, `modem_again=` and 63 more | built |  | built | |

## net

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_NET`**<br>QEMU's virtio network device as the link behind en0 (virtio_net=1). QEMU only | `hakios.virtio_net.c` |  | `virtio_net=` | built | built | built | |

## power

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BATTERY`**<br>the battery: kern.hakios.battery_*, and HakiosBattery, the IOPMPowerSource Darwin reads the charge from (the state comes from the audio DSP's battery manager, with adsp=1) | `hakios.battery.c`<br>`hakios.battery.cpp` |  | `battery_shared_pa=` | built |  | built | |
| **`HAKIOS_CPUFREQ`**<br>the clock of the cores the kernel runs on (cpufreq=1), and the memory bus's votes (ddr=) | `hakios.cpufreq.c` |  | `cpufreq=`, `cpufreq_osm=`, `cpufreq_osm_mhz=`, `cpuload=`, `ddr=`, `ddr_rest=`, `ddr_vote=`, `membench=` and 2 more | built |  | built | |
| **`HAKIOS_THERMAL`**<br>the SoC's temperature sensors (thermal=1): kern.hakios.thermal_*, and the run handed back to the loader when the phone gets too hot (thermal_stop=) | `hakios.thermal.c` |  | `thermal=`, `thermal_log=`, `thermal_poll=`, `thermal_stop=`, `thermal_tm0=`, `thermal_tm1=` |  |  | built | |
| **`HAKIOS_PMIC`**<br>the blocks of the power-management chips the SPMI arbiter shows, and the PMIC's clock (pmic=1); read only | `hakios.pmic.c` |  | `pmic=`, `pmic_list=` |  |  | built | |
| **`HAKIOS_BATTGUARD`**<br>the run handed back to the loader before the battery is flat (battery_stop=<percent>) | `hakios.battguard.c` |  | `battery_stop=`, `battery_stop_poll=` |  |  | built | |

## storage

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_UFS`**<br>the phone's UFS flash: a block device for each GPT partition (ufs=1, ufs=2), and the UFS host controller driven from the kernel (ufs_native=). THE ONLY CODE OF THE KERNEL THAT MAY WRITE TO THE PHONE (ufs_write=1, the partition `darwin` only): see tools/provenance.allow | `hakios.ufs.c`<br>`hakios.ufshc.c` |  | `ufs_bench=`, `ufs_check=`, `ufs_native_bench=`, `ufs_native_buffer=`, `ufs_native_check=`, `ufs_native_hang=`, `ufs_native_leave=`, `ufs_native_slots=` and 3 more | built |  | built | |
| **`HAKIOS_VIRTIO_BLK`**<br>a disk image of the host as /dev/rvblk0 (virtio_blk=1). QEMU only | `hakios.virtio_blk.c` |  | `virtio_blk=` | built | built | built | |

## wifi

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_WIFI`**<br>Wi-Fi of the WCN6750 (wifi=1, wifi_step=N): the DSP that runs the radio's firmware, Linux's ath11k (its object, made by tools/mk-ath11k.sh, is linked in only with this), a station, and the Apple80211 surface Darwin's wifid talks to. Needs fw-wifi/ from the phone | `hakios.wifi.c`<br>`hakios.wlfw.c`<br>`hakios.lx.c`<br>`hakios.sta.c`<br>`hakios.apple80211.c`<br>`hakios.wlan.cpp` | `HAKIOS_QMI` | `wifi=`, `wifi_again=`, `wifi_ask=`, `wifi_cal=`, `wifi_dirty=`, `wifi_file=`, `wifi_fine=`, `wifi_keep=` and 14 more | built |  | built | |

## audio (devices/fajita/soc)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SLIM`**<br>the DSP's SLIMbus (QMI service 0x301), the NGD, and the WCD934x: the earpiece (sound_receiver=1, sound_earpiece=N) and the handset microphone (mic_wcd=1) through it (slim=N) | `hakios.slim.c` | `HAKIOS_ADSP` | `btslim=`, `btslim_rate=`, `slim=` | built | built | built | |

## bt (devices/fajita/soc)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BT6T`**<br>the WCN3990's Bluetooth over its UART: bt6t=1..4 bring-up steps (firmware, address, scans); bt6t=5 hands the controller to Darwin as /dev/cu.bluetooth (H4) and the IOService "bluetooth" bluetoothd looks for | `hakios.bt6t.c`<br>`hakios.bt6t_ioreg.cpp` |  | `bt6t=`, `bt6t_at=`, `bt6t_baud=`, `bt6t_bdaddr=`, `bt6t_cts=`, `bt6t_esco=`, `bt6t_nvm=`, `bt6t_nvmfile=` and 2 more | built | built | built | |

## power (devices/fajita/soc)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_TSENS845`**<br>tsens v2's 21 sensors, read and logged (thermal=1); reading only. The common THERMAL driver is the SC7280's | `hakios.tsens845.c` |  | `thermal=`, `thermal_every=` | built | built | built | |

## wifi (devices/fajita/soc)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_WIFI6T`**<br>ath10k on the WCN3990 (wifi6t=1, with the kernel linked with tools/mk-ath10k.sh's object: ATH=ath10k) and a stand-in for its first exchange with the firmware (wifi10=N); started when the modem announces WLFW | `hakios.wifi6t.c`<br>`hakios.wlfw10.c` | `HAKIOS_MODEM`, `HAKIOS_QMI`, `HAKIOS_WIFI` | `wifi10=`, `wifi10_msa=`, `wifi6t=` | built | built | built | |

## audio (devices/fajita)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_TFA9894`**<br>the NXP TFA9894 on bit-banged I2C, set up by replaying what Android's driver did (tfa9894=N; sound=3 and up on the 6T's MI2S port switch it on and off). Needs fw/tfa9894-coldboot.replay in the small image | `hakios.tfa9894.c` | `HAKIOS_SOUND` | `tfa9894=` | built | built | built | |

## camera (devices/fajita)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_CAM6T`**<br>the cameras' first steps (cam6t=N): the IMX519, IMX376K and IMX371 powered as Android powers them and asked their chip ID over the CCI pins done by hand | `hakios.cam6t.c` |  | `cam6t_after=`, `cam6t_exposure=`, `cam6t_focus=`, `cam6t_gain=`, `cam6t_mode=`, `cam6t_sensor=`, `cam6t_settle=`, `cam6t_xo=` | built | built | built | |

## haptic (devices/fajita)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_PMI_HAPTICS`**<br>the PMI8998's haptics block through the PMIC arbiter, for Darwin's haptics (pmi_haptics=N) | `hakios.pmi_haptics.c` |  | `pmi_haptics=`, `pmi_haptics_after=`, `pmi_haptics_ms=`, `pmi_haptics_tap_ms=` | built | built | built | |

## input (devices/fajita)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SYNAPTICS`**<br>the touch screen: a Synaptics S3706B (RMI4) on I2C done by hand (synaptics=N) | `hakios.synaptics.c` |  | `synaptics=`, `synaptics_after=` | built | built | built | |
| **`HAKIOS_SLIDER`**<br>the alert slider (oneplus,tri-state-key) as Darwin's ring/silent switch (slider=N) | `hakios.slider.c` |  | `slider=` | built | built | built | |

## power (devices/fajita)

| Driver | Files | Requires | Boot arguments | fajita | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BQ27541`**<br>the fuel gauge Android reads (TI bq27541/bq27411 at I2C 0x55, GPIO 55/56): the battery's charge, voltage, current and temperature handed to Darwin's battery every 10 s (bq27541=1, the default; reading only) | `hakios.bq27541.c` | `HAKIOS_PMI_CHARGER` | `bq27541=`, `bq27541_every=` | built | built | built | |
| **`HAKIOS_PMI_CHARGER`**<br>the PMI8998's SMB2 charger through the PMIC arbiter: its state, input limit and watchdog logged, and on a computer's port its input limit raised to 900 mA for the run (pmi_charger=2, the default; =1 only looks) | `hakios.pmi_charger.c` |  | `pmi_charger=`, `pmi_charger_after=`, `pmi_charger_every=`, `pmi_charger_fcc_ma=` | built | built | built | |

The columns on the right are the devices' defconfigs: "built" means that defconfig builds the driver in.
