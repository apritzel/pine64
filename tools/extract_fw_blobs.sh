#!/bin/sh

if [ "$#" -lt 1 ]
then
	echo "usage: $0 <device/image file>"
	echo "  This will extract boot0.bin and firmware.img"
	exit 1
fi

IMAGE="$1"

UBOOT=19096
PARTTAB=20480

dd if="$IMAGE" bs=8k skip=1 count=4 of=boot0.bin
dd if="$IMAGE" bs=1k skip=$UBOOT count=$((PARTTAB-UBOOT)) of=firmware.img

# We don't need the original partition table, but if people wonder ...
#dd if="$IMAGE" bs=1k skip=$PARTTAB count=64 of=parttab.bin
