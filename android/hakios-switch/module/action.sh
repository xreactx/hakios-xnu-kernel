#!/system/bin/sh
# Magisk's Action button for this module: restart into HakiOS. Slot a is made the active slot through the phone's
# own boot control (Qualcomm's bootctrl.sdm845.so, as an OTA update switches slots: android/hakios-switch/hal.c),
# then the phone restarts.
MODDIR=${0%/*}
echo "Switching to HakiOS..."
if "$MODDIR/hakios-bootctl" set 0; then
	echo "Restarting in 3 seconds."
	sleep 3
	/system/bin/svc power reboot 2>/dev/null || reboot
else
	echo "Not switched. (See above.)"
fi
