#!/bin/bash

VERSION=0.4
DEFCONFIG=boeffla_defconfig
TOOLCHAIN=
KCONFIG=false
CUST_CONF=no
BUILD_NUMBER=
DEVICE=m0
KCONF_REPLACE=false
KERNEL_NAME="Koffee"
BOEFFLA_VERSION="7.1"
SKIP_MODULES=true
DONTPACK=false
USER=$USER
DATE=`date`
BUILD_PATH=`pwd`
CLEAN=false


usage() {
	echo "Koffee build script v$VERSION"
	echo `date`
	echo "Written by A\$teroid <acroreiser@gmail.com>"
	echo ""
    echo "Usage:"
    echo ""
	echo "koffee-build.sh [-d/-D/-O <file>] [-K] [-U <user>] [-N <BUILD_NUMBER>] [-k] [-R] -t <toolchain_prefix>"
	echo ""
	echo "Main options:"
	echo "	-K 			call Kconfig (use only with ready config!)"
	echo "	-d 			koffee_defconfig"
	echo "	-D 			koffee_debug_defconfig - produce debugging kernel"
	echo "	-S 			set device codename (m0 for i9300 or t03g for n7100)"
	echo "	-O <file> 			external/other defconfig."
	echo "	-t <toolchain_prefix> 			toolchain prefix"
	echo ""
	echo "Extra options:"
	echo "	-j <number_of_cpus> 			set number of CPUs to use"
	echo "	-k 			make only zImage, do not pack into zip"
	echo "	-C 			cleanup before building"
	echo "	-R 			save your arguments to reuse (just run koffee-build.sh on next builds)"
	echo "	-U <username> 			set build user"
	echo "	-N <release_number> 			set release number"
	echo "	-v 			show build script version"
	echo "	-h 			show this help"
	
}

save_args()
{
	echo "DEFCONFIG=$DEFCONFIG" > `pwd`/.kb_args
	echo "TOOLCHAIN=$TOOLCHAIN" >> `pwd`/.kb_args
	echo "CUST_CONF=$CUST_CONF" >> `pwd`/.kb_args
	echo "USER=$USER" >> `pwd`/.kb_args
	echo "BUILD_NUMBER=$BUILD_NUMBER" >> `pwd`/.kb_args
}

restore_args()
{
	export $(cat `pwd`/.kb_args | grep DEFCONFIG)
	export $(cat `pwd`/.kb_args | grep TOOLCHAIN)
	export $(cat `pwd`/.kb_args | grep CUST_CONF)
	export $(cat `pwd`/.kb_args | grep USER)
	export $(cat `pwd`/.kb_args | grep BUILD_NUMBER)
}

prepare() 
{
 	make -j4 clean
}

make_config() 
{
	if [ "$CUST_CONF" != "no" ]; then
		cp $CUST_CONF `pwd`/.config
		echo "Using custom configuration from $CUST_CONF"
		DEFCONFIG=custom
	else
		if [ ! -f "`pwd`/.config" ] || [ "$KCONF_REPLACE" = true ]; then
			make ARCH=arm $JOBS $DEFCONFIG &>/dev/null
		fi
	fi
	if [ "$KCONFIG" = "true" ]; then
		make ARCH=arm $JOBS menuconfig
	fi
}

build_kernel()
{
	make ARCH=arm KBUILD_BUILD_VERSION=$BUILD_NUMBER $JOBS KBUILD_BUILD_USER=$USER CROSS_COMPILE=$TOOLCHAIN zImage
	if [ $? -eq 0 ]; then
		return 0
	else
		return 1
	fi
}

build_modules()
{
	make ARCH=arm KBUILD_BUILD_VERSION=$BUILD_NUMBER $JOBS KBUILD_BUILD_USER=$USER CROSS_COMPILE=$TOOLCHAIN modules
	if [ $? -eq 0 ]; then
		return 0
	else
		return 1
	fi
}

make_flashable()
{

	mkdir -p $REPACK_PATH
	# copy anykernel template over

	cp -R $BUILD_PATH/anykernel_boeffla/* $REPACK_PATH
	cd $REPACK_PATH
	# delete placeholder files
	find . -name placeholder -delete

	# copy kernel image
	cp $BUILD_PATH/arch/arm/boot/zImage $REPACK_PATH/zImage

	{
		# copy modules to either modules folder (CM and derivates) or directly in ramdisk (Samsung stock)
		if [ "true" == "$MODULES_IN_SYSTEM" ]; then
			MODULES_PATH=$REPACK_PATH/modules
		else
			MODULES_PATH=$REPACK_PATH/ramdisk/lib/modules
		fi

		mkdir -p $MODULES_PATH

		# copy generated modules
		find $BUILD_PATH -name '*.ko' -exec cp -av {} $MODULES_PATH \;

		# set module permissions
		chmod 0644 $MODULES_PATH/*

		# strip modules
		echo -e ">>> strip modules\n"
		${TOOLCHAIN}strip --strip-unneeded $MODULES_PATH/*

	} 2>/dev/null

	# replace variables in anykernel script
	cd $REPACK_PATH
	KERNELNAME="Flashing $KERNEL_NAME $BOEFFLA_VERSION"
	sed -i "s;###kernelname###;${KERNELNAME};" META-INF/com/google/android/update-binary;
	COPYRIGHT=$(echo '(c) A\$teroid × Lord Boeffla, 2018')
	sed -i "s;###copyright###;${COPYRIGHT};" META-INF/com/google/android/update-binary;
	BUILDINFO="Release ${BUILD_NUMBER}, $DATE"
	sed -i "s;###buildinfo###;${BUILDINFO};" META-INF/com/google/android/update-binary;
	SOURCECODE="Source code:  https://github.com/acroreiser/Koffee"
	sed -i "s;###sourcecode###;${SOURCECODE};" META-INF/com/google/android/update-binary;
	DEVNAME="device.name1=${DEVICE}"
	sed -i "s;###DEVICENAME###;${DEVNAME};" anykernel.sh;
	if [ "$DEVICE" = "m0" ]; then
		BOOTBLK="block=/dev/block/mmcblk0p5"
	fi
	if [ "$DEVICE" = "t03g" ]; then
		sed -i "s;###BOOTBLK###;/dev/block/mmcblk0p8;" anykernel.sh;
	fi

	if [ "$DEVICE" = "m0" ]; then
		sed -i "s;###SYSTEM_DEVICE###;"SYSTEM_DEVICE=\"/dev/block/mmcblk0p9\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###CACHE_DEVICE###;"CACHE_DEVICE=\"/dev/block/mmcblk0p8\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###DATA_DEVICE###;"DATA_DEVICE=\"/dev/block/mmcblk0p12\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###BOOT_DEVICE###;"BOOT_DEVICE=\"/dev/block/mmcblk0p5\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###RADIO_DEVICE###;"RADIO_DEVICE=\"/dev/block/mmcblk0p7\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###RECOVERY_DEVICE###;"RECOVERY_DEVICE=\"/dev/block/mmcblk0p6\"";" ramdisk/res/bc/bccontroller.sh;

		sed -i "s;###SYSTEM_DEVICE###;"SYSTEM_DEVICE=\"/dev/block/mmcblk0p9\"";" ramdisk/res/bc/boeffla-init-bc.sh;
		sed -i "s;###CACHE_DEVICE###;"CACHE_DEVICE=\"/dev/block/mmcblk0p8\"";" ramdisk/res/bc/boeffla-init-bc.sh;
		sed -i "s;###DATA_DEVICE###;"DATA_DEVICE=\"/dev/block/mmcblk0p12\"";" ramdisk/res/bc/boeffla-init-bc.sh;

		sed -i "s;###BOOTBLK###;"BOOTBLK=\"/dev/block/mmcblk0p5\"";" tools/ak2-core.sh;
	fi
	if [ "$DEVICE" = "t03g" ]; then
		sed -i "s;###SYSTEM_DEVICE###;"SYSTEM_DEVICE=\"/dev/block/mmcblk0p13\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###CACHE_DEVICE###;"CACHE_DEVICE=\"/dev/block/mmcblk0p12\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###DATA_DEVICE###;"DATA_DEVICE=\"/dev/block/mmcblk0p16\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###BOOT_DEVICE###;"BOOT_DEVICE=\"/dev/block/mmcblk0p8\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###RADIO_DEVICE###;"RADIO_DEVICE=\"/dev/block/mmcblk0p10\"";" ramdisk/res/bc/bccontroller.sh;
		sed -i "s;###RECOVERY_DEVICE###;"RECOVERY_DEVICE=\"/dev/block/mmcblk0p9\"";" ramdisk/res/bc/bccontroller.sh;

		sed -i "s;###SYSTEM_DEVICE###;"SYSTEM_DEVICE=\"/dev/block/mmcblk0p13\"";" ramdisk/res/bc/boeffla-init-bc.sh;
		sed -i "s;###CACHE_DEVICE###;"CACHE_DEVICE=\"/dev/block/mmcblk0p12\"";" ramdisk/res/bc/boeffla-init-bc.sh;
		sed -i "s;###DATA_DEVICE###;"DATA_DEVICE=\"/dev/block/mmcblk0p16\"";" ramdisk/res/bc/boeffla-init-bc.sh;

		sed -i "s;###BOOTBLK###;"BOOTBLK=\"/dev/block/mmcblk0p8\"";" tools/ak2-core.sh;
	fi
		# Creating recovery flashable zip
	echo -e ">>> create flashable zip\n"

	# create zip file
	zip -r9 ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}.zip * -x ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}.zip

	# sign recovery zip if there are keys available
	if [ -f "$BUILD_PATH/tools_boeffla/testkey.x509.pem" ]; then
		echo -e ">>> signing recovery zip\n"
		java -jar $BUILD_PATH/tools_boeffla/signapk.jar -w $BUILD_PATH/tools_boeffla/testkey.x509.pem $BUILD_PATH/tools_boeffla/testkey.pk8 ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}.zip ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-signed.zip
		cp ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-signed.zip $BUILD_PATH/${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE}-signed.zip
		md5sum $BUILD_PATH/${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE}-signed.zip > $BUILD_PATH/${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE}-signed.zip.md5
	else
		cp ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}.zip $BUILD_PATH/${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE}.zip
		md5sum $BUILD_PATH/${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE}.zip > $BUILD_PATH/${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE}.zip.md5
	fi



	cd $BUILD_PATH
	rm -fr .tmpzip
	return 0
}

# Pre

while getopts "hvO:j:KdkB:S:N:CU:Dt:R" opt
do
case $opt in
	h) usage; exit 0;;
	v) echo $VERSION; exit 0;;
	t) TOOLCHAIN=$OPTARG;;
	j) THREADS=$OPTARG;;
	O) CUST_CONF=$OPTARG; DEFCONFIG=custom; KCONF_REPLACE=true;;
	C) CLEAN=true;;
	N) BUILD_NUMBER=$OPTARG;;
	S) DEVICE=$OPTARG;;
	K) KCONFIG=true;;
	k) DONTPACK=true;;
	d) DEFCONFIG="koffee_defconfig"; KCONF_REPLACE=true;;
	D) DEFCONFIG="koffee_debug_defconfig"; KCONF_REPLACE=true;;
	R) REMEMBER=true;;
	U) USER=$OPTARG;;
	*) usage; exit 0;;
esac
done

if [ -z $THREADS ]; then
	JOBS="-j1"
	THREADS=1
else
	JOBS="-j$THREADS"
fi

if [ -d "`pwd`/.tmpzip" ]; then
	rm -fr "`pwd`/.tmpzip"
fi
REPACK_PATH=`pwd`/.tmpzip

# ENTRY POINT
echo "Koffee build script v$VERSION"
echo $DATE

if [ ! -f "$BUILD_PATH/.config" ]; then
	make_config 
fi

if [ "$REMEMBER" = "true" ]; then
	save_args
	echo "Arguments saved! For next build just type \"./make-koffee.sh\""
fi
if [ -f "`pwd`/.kb_args" ]; then
	restore_args
	echo "Your arguments restored!"
fi
 
if [ -z $TOOLCHAIN ] || [ -z $DEFCONFIG ]; then
	usage
	exit 0
fi

if [ "$CLEAN" = "true" ]; then
	prepare &>/dev/null
fi 
if [ "$KCONFIG" = "true" ]; then
	make_config
fi
TVERSION=$(${TOOLCHAIN}gcc --version | grep gcc)

if [ -z $BUILD_NUMBER ]; then 
	if [ -f "`pwd`/.version" ]; then
		BVERN=$(cat `pwd`/.version)
	else
		BVERN=1
	fi
else
	BVERN=$BUILD_NUMBER
fi

if [ $? -eq 0 ]; then
	echo "--------------------------------------"
	echo "| Build  date:	$DATE"
	echo "| Version:	$BOEFFLA_VERSION"
	echo "| Configuration file:	$DEFCONFIG"
	echo "| Release:	$BVERN"
	echo "| Building for:	$DEVICE"
	echo "| Build  user:	$USER"
	echo "| Build  host:	`hostname`"
	echo "| Build  toolchain:	$TVERSION"
	echo "| Number of threads:	$THREADS"
	echo "--------------------------------------"
else
	echo "*** CONFIGURATION FAILED ***"
	exit 1
fi
echo "*** NOW WE GO! ***"
echo "---- Stage 1: Building the kernel ----"
build_kernel
if [ $? -eq 0 ]; then
	echo "*** Kernel is ready! ***"
else
	echo "*** zImage BUILD FAILED ***"
	exit 1
fi

if [ "$SKIP_MODULES" = "false" ]; then
	echo "---- Stage 2: Building modules ----"
	build_modules
	if [ $? -eq 0 ]; then
		echo "*** Modules is ready! ***"
	else
		echo "*** MODULE BUILD FAILED ***"
		exit 1
	fi
else
	echo "---- Stage 2(skipped): Building modules ----"
fi

if [ "$DONTPACK" = "false" ]; then
	echo "---- Stage 3: Packing all stuff ----"
	make_flashable
	if [ $? -eq 0 ]; then
		echo "--------------------------------------"
		echo "--------------------------------------"
		echo "| Build  date:	$DATE"
		echo "| Version:	$BOEFFLA_VERSION"
		echo "| Configuration file:	$DEFCONFIG"
		echo "| Release:	$BVERN"
		echo "| Building for:	$DEVICE"
		echo "| Build  user:	$USER"
		echo "| Build  host:	`hostname`"
		echo "| Build  toolchain:	$TVERSION"
		echo "| Number of threads:	$THREADS"
		echo "> Flashable ZIP: $(ls | grep ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE} | grep .zip | head -n 1)"
		echo "> MD5sum: $(ls | grep ${KERNEL_NAME}${BOEFFLA_VERSION}r${BUILD_NUMBER}-${DEVICE} | grep .md5)"
		echo "--------------------------------------"
		echo "*** Koffee is ready! ***"
	else
		echo "*** KOFFEE ZIP BUILD FAILED ***"
		exit 1
	fi
else
	echo "---- Stage 3(skipped): Packing all stuff ----"
	echo "--------------------------------------"
	echo "--------------------------------------"
	echo "| Build  date:	$DATE"
	echo "| Version:	$BOEFFLA_VERSION"
	echo "| Configuration file:	$DEFCONFIG"
	echo "| Release:	$BVERN"
	echo "| Building for:	$DEVICE"
	echo "| Build  user:	$USER"
	echo "| Build  host:	`hostname`"
	echo "| Build  toolchain:	$TVERSION"
	echo "| Number of threads:	$THREADS"
	echo "> zImage: arch/arm/boot/zImage"
	echo "--------------------------------------"
	echo "*** Koffee is ready! ***"
fi
