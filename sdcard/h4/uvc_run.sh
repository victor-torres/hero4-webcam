#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Keeps h4uvc (the UVC webcam server) running. If it exits, the gadget leaves
# the bus and USB Ethernet goes with it, so restart it right away. Runs from
# /tmp: the RTOS can take the card away from Linux.
#
# Logs go to RAM (/tmp/h4uvc), sparing the card. With h4/debug on the card they
# go to h4/uvc on the card instead, the previous run's are kept, and the kernel
# log is appended every 5 s: after a USB wedge the only way back is a power
# cycle, which loses everything in RAM.
SD=/tmp/fuse_d
L=/tmp/h4uvc.d
DEBUG=
if [ -e $SD/h4/debug ]; then
	DEBUG=1
	L=$SD/h4/uvc
fi
mkdir -p $L
[ -x /tmp/h4uvc ] || { cp $SD/h4/h4uvc /tmp/ && chmod 755 /tmp/h4uvc; }
if [ -n "$DEBUG" ]; then
	[ -f $L/h4uvc.log ] && mv $L/h4uvc.log $L/h4uvc.prev.log
	[ -f $L/kernel.log ] && mv $L/kernel.log $L/kernel.prev.log
	dmesg > $L/kernel.log
	(while true; do
		sleep 5
		[ $(wc -c < $L/kernel.log) -lt 1000000 ] && dmesg -c | grep -v gpStreamA9 >> $L/kernel.log
		sync
	done) &
fi
echo "uvc_run start up=$(cut -d' ' -f1 /proc/uptime)" > $L/h4uvc.log
while true; do
	# -p: leave USB mode and apply the 1280x720 preset before opening the device.
	/tmp/h4uvc -p /dev/video0 2>> $L/h4uvc.log
	echo "h4uvc exited rc=$? up=$(cut -d' ' -f1 /proc/uptime)" >> $L/h4uvc.log
	sleep 2
done
