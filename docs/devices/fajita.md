<!-- Made by tools/docs-gen.py from devices/fajita/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# OnePlus 6T (A6013) (`fajita`)

| | |
|---|---|
| Vendor | OnePlus |
| SoC | `sdm845` |
| Configuration | `make fajita_defconfig` |
| Maintainer | xreactx |
| Repository | [`devices/fajita/`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main/devices/fajita) |
| Firmware | `devices/fajita/pull-firmware.sh` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | works | the panel (command mode) through UEFI's splash memory, three buffers flipped through VIG0 (fb_flip=1 fb_map_mb=30 soc_display=845); Darwin draws with the GPU |
| cpu | works | 8 cores (bootcpu=7), GICv3, the virtual timer on intid 19 (soc_vtimer=19); the big cores' clocks raised through the OSM (cpufreq_osm=2) |
| storage | works | UFS 2.1 read through the loader and the kernel's own driver (ufs=2); /private/var kept at the end of system_a (ufs_write=1 var_store=1) |
| touch | works | Synaptics S3706B (RMI4) on I2C done by hand (synaptics=3) |
| buttons | works | power, volume up and down on the PM8998 (buttons=2); the alert slider as the ring/silent switch (slider=3) |
| gpu | works | Adreno 630: Mesa freedreno "FD630" (gpu=1 soc_gpu=630 adreno_speed=7) |
| audio | works | the audio DSP (adsp=1); the loudspeaker through QUAT MI2S and the TFA9894 (sound=4); the earpiece (sound_receiver=1) and the handset microphone (mic=4 mic_wcd=1) through the WCD934x over SLIMbus (slim=3) |
| battery | works | the bq27411 fuel gauge Android reads, on I2C (bq27541=1); the PMI8998's charger charges during a run (pmi_charger=2: 900 mA in on a computer's port, 500 mA into the battery); UEFI's ChargerEx (battery=1) for the charger's presence |
| haptic | works | the PMI8998's haptics block (pmi_haptics=4) |
| thermal | works | reading only: tsens v2, 21 sensors (thermal=1) |
| wifi | works | WCN3990: the modem starts its Wi-Fi domain; Linux's ath10k (ATH=ath10k) scans, joins a WPA2 network and carries Darwin's traffic (wifi6t=1 wifi_join=1, in the configuration) |
| cellular | partly | the modem boots and registers on LTE with a SIM (modem=1, in the configuration); the data session comes up and packets go out, none come back (the test SIM had no data under Android either) |
| bluetooth | works | WCN3990 over UART (bt6t=5): iOS's own bluetoothd; pairing, A2DP, and a headset's call audio both ways through the second SLIMbus (btslim=6) |
| camera | works | Camera.app with both cameras (cam6t=6, the default): the rear IMX519 over three C-PHY trios with its lens motor, the front IMX371 over four D-PHY lanes, live preview, switching, photographs; frames through CSID-lite, IFE-lite and an SMMU context of ours into /dev/hakios-camera, redwood's ISP in mediaserverd |

The state is what has been seen on this device, written down in `devices/fajita/device.conf`; a part the table does not name has not been looked at.
