#!/system/bin/sh
# Magisk's Action button for this module: restart into HakiOS. Slot a is made the active slot through the phone's
# own boot control (Qualcomm's bootctrl.sdm845.so, as an OTA update switches slots: android/hakios-switch/hal.c),
# then the phone restarts. (Run from /data/local/tmp: Android's linker lets a program there load the vendor's
# library, and not one in this module's folder -- "dlopen failed".)
MODDIR=${0%/*}
RUN=/data/local/tmp/hakios-bootctl
echo "Switching to HakiOS..."
cp "$MODDIR/hakios-bootctl" "$RUN" && chmod 755 "$RUN"
if "$RUN" set 0; then
	rm -f "$RUN"
	echo "Restarting in 3 seconds."
	sleep 3
	/system/bin/svc power reboot 2>/dev/null || reboot
else
	rm -f "$RUN"
	echo "Not switched. (See above.)"
fi
