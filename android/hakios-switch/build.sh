#!/bin/bash
# android/hakios-switch/build.sh: the Magisk module "Restart into HakiOS" (hakios-switch.zip): hakios-slot (slot.c),
# built static for ARM64 Linux with no C library, and the module's action.sh.
# Install: adb push hakios-switch.zip /sdcard/ && adb shell su -c 'magisk --install-module /sdcard/hakios-switch.zip'
set -e
cd "$(dirname "$0")"
clang --target=aarch64-linux-gnu -nostdlib -static -ffreestanding -fno-builtin -fuse-ld=lld -O2 -Wall -Wno-unused-function \
	-o module/hakios-slot slot.c
rm -f hakios-switch.zip
(cd module && zip -qr ../hakios-switch.zip .)
echo "hakios-switch.zip: $(stat -c %s hakios-switch.zip) bytes"
