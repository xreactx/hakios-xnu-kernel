#!/system/bin/sh
# Magisk's Action button for this module: restart into HakiOS (android/hakios-switch/slot.c).
MODDIR=${0%/*}
echo "Switching to HakiOS..."
if "$MODDIR/hakios-slot" to-hakios; then
	echo "Restarting in 3 seconds."
	sleep 3
	/system/bin/svc power reboot 2>/dev/null || reboot
else
	echo "Not switched: nothing was written. (See above.)"
fi
