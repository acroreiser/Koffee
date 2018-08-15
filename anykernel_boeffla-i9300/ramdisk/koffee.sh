#!/system/xbin/bash
# Koffee's startup script
# running immediatelly after mounting /system
# do not edit!
/sbin/busybox mount -o remount,rw /

# 1. Systemless HWC
/sbin/busybox mkdir /libs
/sbin/busybox mount -t tmpfs tmpfs /libs
/sbin/busybox mv /res/koffee/* /libs
/system/bin/chcon u:object_r:system_file:s0 /libs/*.so 
/sbin/busybox mount -o bind /libs/gralloc.exynos4.so /system/lib/hw/gralloc.exynos4.so
/sbin/busybox mount -o bind /libs/hwcomposer.exynos4.so /system/lib/hw/hwcomposer.exynos4.so

#
/sbin/busybox rm /koffee.sh
/sbin/busybox mount -o remount,ro /
/system/bin/toybox setenforce 1