xargs -L1 git checkout HEAD < ours.txt

HARD_RESET="arch/arm/mach-s3c2410 arch/arm/mach-s3c64xx drivers/media sound include/sound drivers/usb drivers/regulator drivers/spi include/linux/regulator* include/linux/spi* drivers/gpio drivers/hwmon drivers/staging/zram" 

echo $HARD_RESET | xargs -L1 rm -fr
echo $HARD_RESET | xargs -L1 git checkout HEAD
echo $HARD_RESET | xargs -L1 git add
