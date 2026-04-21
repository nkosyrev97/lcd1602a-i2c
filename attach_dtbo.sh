#!/bin/bash

# Execute script with root permissions.
# Kernel should be built with:
# CONFIG_CONFIGFS_FS=y
# CONFIG_OF_OVERLAY=y
# CONFIG_OF_CONFIGFS=y
# Check your config before executing this script!

mount -t configfs none /sys/kernel/config
mkdir -p /sys/kernel/config/device-tree/overlays/lcd1602a
cat /home/user/lcd1602a/vf2-lcd1602a-i2c.dtbo | tee /sys/kernel/config/device-tree/overlays/lcd1602a/dtbo > /dev/null

STATUS=$(cat /sys/kernel/config/device-tree/overlays/my_overlay/status)
if [ "$STATUS" != "applied" ]; then
    echo "Error: Overlay not applied. Current status: $STATUS"
    rmdir /sys/kernel/config/device-tree/overlays/lcd1602a
    dmesg | tail -n 10
    exit 1
fi

exit 0
