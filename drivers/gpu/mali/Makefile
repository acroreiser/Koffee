MALI_SUBFOLDER := mali400ko/driver/src/devicedrv/mali
MALIDRM_SUBFOLDER := mali400ko/x11/mali_drm/mali
MALI_FOLDER := $(srctree)/$(src)/$(MALI_SUBFOLDER)
ifeq ($(shell [ -d $(MALI_FOLDER) ] && echo "OK"), OK)
obj-$(CONFIG_GPU_MALI)  += $(MALI_SUBFOLDER)/
obj-$(CONFIG_MALI_DRM) += $(MALIDRM_SUBFOLDER)/
else
$(warning WARNING: mali: Could not find $(MALI_FOLDER) - mali device driver will not be built)
obj-n += ./
endif
