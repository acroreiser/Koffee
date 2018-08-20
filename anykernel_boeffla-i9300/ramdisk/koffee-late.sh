#!/system/xbin/bash
# Koffee's LATE startup script
# running when system already booted
# do not edit!
/sbin/busybox mount -o remount,rw /

# 1. Fix Doze helper permissions
/sbin/busybox chmod 0755 /res/koffee/supolicy
/res/koffee/supolicy --live "allow kernel system_file file { execute_no_trans }"

# 2. Try to prefetch apps using readahead [2]
/system/bin/toybox readahead /system/lib/*.so
/system/bin/toybox readahead /data/dalvik-cache/arm/*.dex

# Clean up and fire up SELinux
/sbin/busybox rm /koffee-early.sh; /sbin/busybox rm /res/koffee/supolicy; /sbin/busybox mount -o remount,ro /libs; /sbin/busybox mount -o remount,ro /; /system/bin/toybox setenforce 1