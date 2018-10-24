mcde-objs		+= mcde_mod.o
mcde-objs		+= mcde_hw.o
mcde-objs		+= mcde_dss.o
mcde-objs		+= mcde_display.o
mcde-objs		+= mcde_bus.o
mcde-objs		+= mcde_fb.o
mcde-objs		+= mcde_debugfs.o
obj-$(CONFIG_FB_MCDE)	+= mcde.o

obj-$(CONFIG_MCDE_DISPLAY_GENERIC_DSI)		+= display-generic_dsi.o
obj-$(CONFIG_MCDE_DISPLAY_SAMSUNG_S6D16D0)	+= display-samsung_s6d16d0.o
obj-$(CONFIG_MCDE_DISPLAY_SONY_ACX424AKP_DSI)   += display-sony_acx424akp_dsi.o
obj-$(CONFIG_MCDE_DISPLAY_VUIB500_DPI)		+= display-vuib500-dpi.o
obj-$(CONFIG_MCDE_DISPLAY_NASTEC_DPI)		+= display-nastec-dpi.o
obj-$(CONFIG_MCDE_DISPLAY_AB8500_DENC)		+= display-ab8500.o
obj-$(CONFIG_MCDE_DISPLAY_AV8100)		+= display-av8100.o
obj-$(CONFIG_DISPLAY_FICTIVE)			+= display-fictive.o

ifdef CONFIG_FB_MCDE_DEBUG
EXTRA_CFLAGS += -DDEBUG
endif
ifdef CONFIG_FB_MCDE_VDEBUG
EXTRA_CFLAGS += -DVERBOSE_DEBUG
endif
