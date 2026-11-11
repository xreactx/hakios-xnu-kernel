#!/bin/bash
# Pull the OnePlus 6T's own firmware out of its Android into the fw-*/ directories at the top of the
# repository (never committed: it is the vendor's). The phone is on adb, in Android (OxygenOS or a
# LineageOS-based ROM; the modem partition is mounted at /vendor/firmware_mnt).
#
# The SDM845's names differ from the POCO X5 Pro's: the GPU is an Adreno 630 (a630_*), Wi-Fi is the
# WCN3990, whose firmware (wlanmdsp.mbn) the modem loads, so there is no wpss; the vibration motor is
# the PMI8998's own haptics block and needs no firmware. Each set is
# listed on the phone (adb shell ls) rather than counted here, because the number of .bNN segments
# differs between firmware versions.
#
# Without root, OxygenOS does not let adb read /vendor/firmware_mnt ("Permission denied"). The loader
# reads the same files off the flash instead: the modem partition of the active slot (b on the 6T here)
# is a FAT16 file system of 120 MB, whose image/ directory is /vendor/firmware_mnt/image:
#   ./xl flash-read modem_b 0 7800000 modem_b.img && 7z x -omodem_b modem_b.img
# then adsp.* to fw-audio/, wlanmdsp.mbn to fw-wifi/, mba.mbn and modem.* to fw-modem/ and image/ whole to
# fw-modem/image/. The speaker amplifier's container is readable as it is: /vendor/etc/firmware/tfa98xx.cnt.
#
# The cameras' module and tuning files (tools/camsensor.py, tools/camtuning.py: userland/build.sh makes
# camera-<sensor>.tab of them) are in /vendor/lib/camera here, not lib64, and SELinux hides them from adb's
# shell even to list (vendor_file): they are copied through su (Magisk; fastboot boot the patched image
# for a rooted Android of that run only), or not at all.
set -e
cd "$(dirname "$0")/../.."
pull() { # DIR PATTERN...  (patterns are expanded on the phone; nothing found is told, not fatal)
	local dir=$1; shift
	mkdir -p "$dir"
	local files
	files=$(adb shell "ls $* 2>/dev/null" | tr -d '\r')
	[ -n "$files" ] || echo "pull-firmware.sh: $dir: none of $* on the phone"
	for f in $files; do adb pull "$f" "$dir/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: could not be read"; done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
}
adb get-state >/dev/null
img=/vendor/firmware_mnt/image
pull fw-gpu /vendor/firmware/a630_sqe.fw /vendor/firmware/a630_gmu.bin '/vendor/firmware/a630_zap.*'
pull fw-wifi $img/wlanmdsp.mbn '/vendor/firmware/wlan/qca_cld/*' '/vendor/etc/wifi/bdwlan*' "$img/bdwlan*"
pull fw-audio "$img/adsp.mdt" "$img/adsp.b*"
pull fw-modem "$img/mba.mbn" "$img/modem.mdt" "$img/modem.b*"
mkdir -p fw-modem/image && adb pull $img fw-modem/ >/dev/null 2>&1 && echo "pull-firmware.sh: fw-modem/image: the modem's own files (TFTP)"
if adb shell 'su -c true' >/dev/null 2>&1; then
	mkdir -p fw-camera
	adb exec-out "su -c 'cd /vendor/lib/camera && tar cf - com.qti.sensormodule.*.bin com.qti.tuned.*.bin'" | tar xf - -C fw-camera
	echo "pull-firmware.sh: fw-camera: $(ls -1 fw-camera | wc -l | tr -d ' ') files"
else
	echo "pull-firmware.sh: fw-camera: not pulled (needs root: /vendor/lib/camera is hidden from adb's shell)"
fi
echo "pull-firmware.sh: done; Wi-Fi also wants fw-wifi/mac (the radio's address) and fw-wifi/network (the network's name and passphrase)"
