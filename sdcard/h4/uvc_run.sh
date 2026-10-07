#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Keeps h4uvc (the UVC webcam server) running. If it exits, the gadget leaves
# the bus and USB Ethernet goes with it, so restart it right away. Runs from
# /tmp: the RTOS can take the card away from Linux.
#
# Logs go to RAM (/tmp/h4uvc.d), sparing the card. With h4/debug on the card
# they're also copied to h4/uvc on the card every 5 s, the previous run's are
# kept, and the kernel log is appended: after a hang the only way back is a
# power cycle, which loses everything in RAM. (h4uvc can't log to the card
# directly: it keeps the file open, and the card only stores what was written
# once the file is closed.)
# H4HOME: where h4.sh runs from (the card, or internal flash); debug logs only
# ever go to the card.
SD=${H4HOME:-/tmp/fuse_d}
CARD=/tmp/fuse_d
R=/tmp/h4uvc.d
L=$R
DEBUG=
if [ -e $CARD/h4/debug ]; then
	DEBUG=1
	L=$CARD/h4/uvc
fi
mkdir -p $R $L
[ -x /tmp/h4uvc ] || { cp $SD/h4/h4uvc /tmp/ && chmod 755 /tmp/h4uvc; }
if [ -n "$DEBUG" ]; then
	# The card won't rename onto an existing file (the mv "succeeds" and nothing
	# changes), so clear the old copies first.
	[ -f $L/h4uvc.log ] && rm -f $L/h4uvc.prev.log && mv $L/h4uvc.log $L/h4uvc.prev.log
	[ -f $L/kernel.log ] && rm -f $L/kernel.prev.log && mv $L/kernel.log $L/kernel.prev.log
	dmesg > $L/kernel.log
	(while true; do
		sleep 5
		[ $(wc -c < $L/kernel.log) -lt 1000000 ] && dmesg -c | grep -v gpStreamA9 >> $L/kernel.log
		# Heartbeat: shows when Linux stopped, and whether a resume put the stock
		# firewall back (no usb0 rule).
		echo "hb $(date +%T) up=$(cut -d' ' -f1 /proc/uptime) usb0-rules=$(iptables -S INPUT | grep -c usb0)" >> $L/kernel.log
		cp $R/h4uvc.log $L/h4uvc.log
		sync
	done) &
fi
echo "uvc_run start up=$(cut -d' ' -f1 /proc/uptime)" > $R/h4uvc.log
while true; do
	# Optional overrides, one number per file, read on every (re)start:
	# h4/bitrate → setting 62, h4/res → setting 2.
	unset H4UVC_BITRATE H4UVC_RES
	[ -s $SD/h4/bitrate ] && export H4UVC_BITRATE=$(cat $SD/h4/bitrate)
	[ -s $SD/h4/res ] && export H4UVC_RES=$(cat $SD/h4/res)
	# -p: leave USB mode and apply the preset before opening the device.
	/tmp/h4uvc -p /dev/video0 2>> $R/h4uvc.log
	echo "h4uvc exited rc=$? up=$(cut -d' ' -f1 /proc/uptime)" >> $R/h4uvc.log
	sleep 2
done
