cp .config .config.bkp
make ARCH=arm CROSS_COMPILE=/android-kernel/ICS/toolchain/bin/arm-none-eabi- mrproper
cp .config.bkp .config
