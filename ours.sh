xargs -L1 git checkout HEAD < ours.txt

HARD_RESET="arch/arm/mach-exynos arch/arm/mach-s3c2410 arch/arm/mach-s3c64xx arch/arm/mach-s5pv210 arch/arm/plat-s5p drivers/media sound include/sound drivers/usb drivers/regulator drivers/spi include/linux/regulator* include/linux/spi* drivers/gpio drivers/hwmon drivers/staging/zram include/media drivers/staging/android drivers/input/touchscreen drivers/power include/linux/power* drivers/tty/serial/samsung*" 

echo $HARD_RESET | xargs -L1 rm -fr
echo $HARD_RESET | xargs -L1 git checkout HEAD
echo $HARD_RESET | xargs -L1 git add
