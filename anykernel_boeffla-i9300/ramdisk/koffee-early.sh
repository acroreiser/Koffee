#!/system/xbin/bash
# Koffee's EARLY startup script
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
/sbin/busybox chmod 0644 /system/lib/hw/gralloc.exynos4.so
/sbin/busybox chmod 0644 /system/lib/hw/hwcomposer.exynos4.so

# 2. Pyramid
/sbin/busybox echo "pyramid" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 3. SELinux contexts
/system/bin/restorecon -FRD /data/data

# 4. zRam
# Enable total 400 MB zRam on 1 device as default
/sbin/busybox echo "1" > /sys/block/zram0/reset
/sbin/busybox echo "209715200" > /sys/block/zram0/disksize
/sbin/busybox mkswap /dev/block/zram0
/sbin/busybox swapon /dev/block/zram0

/sbin/busybox echo "1" > /sys/block/zram1/reset
/sbin/busybox echo "lzo" > /sys/block/zram1/comp_algorithm
/sbin/busybox echo "419430400" > /sys/block/zram1/disksize
/sbin/busybox mkswap /dev/block/zram1
/sbin/busybox swapon /dev/block/zram1
/sbin/busybox echo "100" > /proc/sys/vm/swappiness

# 5. BFQ and deadline
/sbin/busybox echo "bfq" > /sys/block/mmcblk0/queue/scheduler
/sbin/busybox echo "deadline" > /sys/block/mmcblk1/queue/scheduler

# 6. Switch to fq_codel on mobile data and wlan
/system/bin/tc qdisc add dev rmnet0 root fq_codel
/system/bin/tc qdisc add dev wlan0 root fq_codel

# 7. Enable network security enhacements from O
/sbin/busybox echo 1 > /proc/sys/net/ipv4/conf/all/drop_unicast_in_l2_multicast
/sbin/busybox echo 1 > /proc/sys/net/ipv6/conf/all/drop_unicast_in_l2_multicast
/sbin/busybox echo 1 > /proc/sys/net/ipv4/conf/all/drop_gratuitous_arp
/sbin/busybox echo 1 > /proc/sys/net/ipv6/conf/all/drop_unsolicited_na

# 8. Tweak scheduler
/sbin/busybox echo 1 > /proc/sys/kernel/sched_child_runs_first

# 9. Enlarge nr_requests for emmc
/sbin/busybox echo 2048 > /sys/block/mmcblk0/queue/nr_requests

# 10. Sdcard buffer tweaks
/sbin/busybox echo 2048 > /sys/block/mmcblk0/bdi/read_ahead_kb
/sbin/busybox echo 1024 > /sys/block/mmcblk1/bdi/read_ahead_kb

# 11. Strict request affinity for internal storage
/sbin/busybox echo 2 > /sys/block/mmcblk0/queue/rq_affinity

# 12. Try to speed up booting using readahead
/system/bin/toybox readahead /system/lib/*.so
/system/bin/toybox readahead /data/dalvik-cache/arm/*.dex

# 13. FS Trim
/sbin/busybox fstrim -v /data

# 14. min free kbytes
/sbin/busybox echo 40960 > /proc/sys/vm/min_free_kbytes


# Exiting
/sbin/busybox mount -o remount,ro /
exit 0