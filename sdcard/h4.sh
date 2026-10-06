#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Started as root by the patched sd_script.sh on the camera's Linux side,
# every time the SD card is mounted. Takes the USB port from the RTOS and
# boots the camera as a webcam + microphone + USB Ethernet.
#
# Optional files in h4/ on the card:
#   ether_only  USB Ethernet only (no webcam)
#   shell       root shell on the USB link (nc 169.254.77.1 2323), no password
#   debug       logs on the card instead of RAM
SD=/tmp/fuse_d
LOG=/tmp/h4_usb.log

# Log to RAM. Copied to the card only with h4/debug there (spares the card;
# the RTOS also takes the card away from Linux while USB is in MTP mode).
save() { [ -e $SD/h4/debug ] && cp $LOG $SD/h4_usb.log 2> /dev/null && sync; }

echo "h4 $(date '+%F %T') up=$(cut -d' ' -f1 /proc/uptime) pid=$$" >> $SD/h4.log

# /tmp is restored from the hibernation image on every boot, so this lock
# only lasts for the current power cycle.
if [ -e /tmp/h4.lock ]; then
	# Same Linux session: the camera was "off" and Linux resumed, or the RTOS
	# re-mounted the card. The resume restores the stock firewall, so put back
	# what the first run set up. The USB gadget itself survives.
	iptables -C INPUT -i usb0 -j ACCEPT 2> /dev/null || iptables -I INPUT -i usb0 -j ACCEPT
	iptables -C OUTPUT -o usb0 -j ACCEPT 2> /dev/null || iptables -I OUTPUT -o usb0 -j ACCEPT
	# /tmp comes from the hibernation snapshot, taken before the first run copied
	# h4csi; without key 0x43 the RTOS suspends Linux once the camera leaves USB mode.
	[ -x /tmp/h4csi ] || { cp $SD/h4/h4csi /tmp/ && chmod 755 /tmp/h4csi; }
	/tmp/h4csi set 0x43 1 > /dev/null
	[ -e $SD/h4/shell ] && ! ps | grep -q "[t]cpsvd -v 169.254.77.1 2323" &&
		tcpsvd -v 169.254.77.1 2323 sh -c 'exec sh 2>&1' > /dev/null 2>&1 &
	if [ -c /dev/video0 ] && ! ps | grep -q "[u]vc_run.sh"; then
		cp $SD/h4/uvc_run.sh /tmp/ && sh /tmp/uvc_run.sh > /dev/null 2>&1 &
	fi
	echo "h4 re-armed up=$(cut -d' ' -f1 /proc/uptime)" >> $SD/h4.log
	save
	exit 0
fi
touch /tmp/h4.lock

say() { echo "[$(cut -d' ' -f1 /proc/uptime)] $*" >> $LOG; }
step() {
	say "+ $*"
	save
	"$@" >> $LOG 2>&1
	rc=$?
	say "  rc=$rc"
	dmesg | tail -6 >> $LOG
	save
	return $rc
}

sleep 15
say "===== boot, $(uname -r)"

step insmod $SD/h4/usb-common.ko
step insmod $SD/h4/udc-core.ko
# Patched UDC driver (src/ambarella_udc): isochronous IN for h4cam's microphone.
# The stock one still runs the webcam and Ethernet, without the microphone.
step insmod $SD/h4/ambarella_udc_h4.ko || step insmod $SD/h4/ambarella_udc.ko
for m in libcomposite h4_udc_dev; do
	step insmod $SD/h4/$m.ko
done
step ls /sys/bus/platform/devices/ambarella-udc/
step cat /proc/ambarella/udc
# USB mode. Default: webcam (h4cam = UVC H.264 camera + microphone + USB
# Ethernet, served by h4uvc). Ethernet only (g_ether) if h4/ether_only exists
# on the card or the webcam modules fail to load. Same MACs either way, so the
# Mac's network interface stays the same.
WEBCAM=
if [ ! -e $SD/h4/ether_only ] && step insmod $SD/h4/videodev.ko &&
	step insmod $SD/h4/h4cam.ko dev_addr=02:48:34:00:00:01 host_addr=02:48:34:00:00:02; then
	WEBCAM=1
	[ -e /dev/video0 ] || mknod /dev/video0 c 81 0
else
	step insmod $SD/h4/g_ether.ko dev_addr=02:48:34:00:00:01 host_addr=02:48:34:00:00:02
fi
step ifconfig usb0 169.254.77.1 netmask 255.255.0.0 up
dmesg | tail -40 >> $LOG
save

# The stock iptables rules drop anything but the GoPro ports (ping too);
# trust the USB cable.
step iptables -I INPUT -i usb0 -j ACCEPT
step iptables -I OUTPUT -o usb0 -j ACCEPT

# Tools in RAM, so they keep working while the RTOS holds the card.
step cp $SD/h4/h4csi /tmp/
chmod 755 /tmp/h4csi

# Once the RTOS leaves its own USB mode (any mode command) with Wi-Fi idle, it
# suspends Linux: the USB link stays "active" but nothing answers. Ask it not to.
step /tmp/h4csi set 0x43 1

# Root shell, only with h4/shell on the card, bound to the USB link address
# only (not Wi-Fi), no password:
#   nc 169.254.77.1 2323
# No prompt. Not "sh -i": the hook inherits the console as controlling tty,
# and an interactive shell gets stopped by job control before it says anything.
[ -e $SD/h4/shell ] && step tcpsvd -v 169.254.77.1 2323 sh -c 'exec sh 2>&1' &

# The gadget connects as soon as h4cam loads; h4uvc reconnects it once it can
# answer the host (after its ~6 s preset). Run from a RAM copy: the card's filesystem won't replace a file that's open.
[ -n "$WEBCAM" ] && cp $SD/h4/uvc_run.sh /tmp/ && sh /tmp/uvc_run.sh > /dev/null 2>&1 &

n=0
while [ $n -lt 60 ]; do
	n=$(expr $n + 1)
	sleep 5
	say "poll $n"
	cat /proc/ambarella/udc >> $LOG 2>&1
	grep -iE "udc|usb" /proc/interrupts >> $LOG 2>&1
	ifconfig usb0 2>&1 | grep -E "RUNNING|RX packets|TX packets" >> $LOG
	dmesg | tail -3 >> $LOG
	save
done
say "done"
save
