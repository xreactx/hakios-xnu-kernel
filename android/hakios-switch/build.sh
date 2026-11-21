#!/bin/bash
# android/hakios-switch/build.sh: the Magisk module "Restart into HakiOS" (hakios-switch.zip): its Action runs
# hakios-bootctl (hal.c) -- the phone's own boot control library asked to make slot a active -- and restarts.
# hakios-bootctl is linked against the phone's libc.so and libdl.so, copied into sysroot/ (adb pull
# /system/lib64/libc.so /system/lib64/libdl.so); no NDK. slot.c (hakios-slot) is the earlier way, attribute bits
# alone, which ABL did not follow from slot b to slot a; kept for its read-only "status".
# Install: adb push hakios-switch.zip /sdcard/ && adb shell su -c 'magisk --install-module /sdcard/hakios-switch.zip'
set -e
cd "$(dirname "$0")"
[ -f sysroot/libc.so ] && [ -f sysroot/libdl.so ] || { mkdir -p sysroot; adb pull /system/lib64/libc.so sysroot/; adb pull /system/lib64/libdl.so sysroot/; }
clang --target=aarch64-linux-android30 -nostdlib -nostdinc -ffreestanding -fPIE -pie -fuse-ld=lld -O2 -Wall \
	-Wl,--dynamic-linker=/system/bin/linker64 -Wl,-z,now -o module/hakios-bootctl hal.c sysroot/libc.so sysroot/libdl.so
clang --target=aarch64-linux-gnu -nostdlib -static -ffreestanding -fno-builtin -fuse-ld=lld -O2 -Wall -Wno-unused-function \
	-o hakios-slot slot.c
rm -f module/hakios-slot hakios-switch.zip
(cd module && zip -qr ../hakios-switch.zip .)
echo "hakios-switch.zip: $(stat -c %s hakios-switch.zip) bytes"
