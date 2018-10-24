#
# * Copyright (C) 2010 ARM Limited. All rights reserved.
# *
# * This program is free software and is provided to you under the terms of the GNU General Public License version 2
# * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
# *
# * A copy of the licence is included with the program, and can also be obtained from Free Software
# * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#

#
# Makefile for the Mali drm device driver.  This driver provides support for the
# Direct Rendering Infrastructure (DRI) in XFree86 4.1.0 and higher.

# If KERNELRELEASE is defined, we've been invoked from the # kernel build system and can use its language.
ifneq ($(KERNELRELEASE),)
	obj-m := mali_drm.o
	mali_drm-objs := mali_drv.o
	EXTRA_CFLAGS += -I$(KBUILD_EXTMOD) -I$(KBUILD_EXTMOD)/include -I$(KBUILD_EXTMOD)/../drm/include/

# Otherwise we were called directly from the command # line; invoke the kernel build system.
else

	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

endif
