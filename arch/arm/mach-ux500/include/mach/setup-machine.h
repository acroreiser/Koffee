/*
 * Copyright (C) 2017 Shilin Victor <chrono.monochrome@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This is a device-specific extension to the function parse_tag_cmdline
 */

#ifndef _SETUP_MACHINE
#define _SETUP_MACHINE

#include <linux/export.h>
#include <linux/moduleparam.h>

unsigned int bootmode = 0;
EXPORT_SYMBOL(bootmode);

unsigned int is_lpm = 0;
module_param(is_lpm, uint, 0444);
EXPORT_SYMBOL(is_lpm);

static unsigned int setup_debug = 0;
module_param(setup_debug, uint, 0644);

int lcdtype = 0;
EXPORT_SYMBOL(lcdtype);

extern int charger_mode;

#define PARSE_CMDLINE_MACHINE_EXT						\
	pr_err("Bootloader command line: %s\n", tag->u.cmdline.cmdline);	\
	strlcat(default_command_line, " ", COMMAND_LINE_SIZE);			\
										\
	if (strstr(tag->u.cmdline.cmdline, "lpm_boot=1") != NULL) {		\
		pr_err("LPM boot from bootloader\n");				\
										\
		is_lpm = 1;							\
		strlcat(default_command_line,                                   \
                     "lpm_boot=1 ", COMMAND_LINE_SIZE);                         \
	} else {								\
		strlcat(default_command_line, "lpm_boot=0 ", COMMAND_LINE_SIZE);\
	}									\
										\
	if (strstr(tag->u.cmdline.cmdline, "bootmode=2") != NULL) {		\
                if (!is_lpm)                                                    \
                      charger_mode = 1;                                         \
		pr_err("Recovery boot from bootloader\n");			\
		strlcat(default_command_line, "bootmode=2 ", COMMAND_LINE_SIZE);\
	}									\
										\
	strlcat(default_command_line, "logo.  ", COMMAND_LINE_SIZE);		\
										\
	if (strstr(tag->u.cmdline.cmdline, "lcdtype=4") != NULL) {		\
               pr_err("LCD type WS2401 from bootloader\n");			\
	       lcdtype = 4;							\
               strlcat(default_command_line, "lcdtype=4 ", COMMAND_LINE_SIZE);	\
	} else if (strstr(tag->u.cmdline.cmdline, "lcdtype=7") != NULL) {	\
               pr_err("LCD type s6e63m0 from bootloader\n");			\
               lcdtype = 8;							\
               strlcat(default_command_line, "lcdtype=7 ", COMMAND_LINE_SIZE);	\
	} else if (strstr(tag->u.cmdline.cmdline, "lcdtype=8") != NULL) {	\
               pr_err("LCD type S6D from bootloader\n");			\
               lcdtype = 8;							\
               strlcat(default_command_line, "lcdtype=8 ", COMMAND_LINE_SIZE);	\
        } else if (strstr(tag->u.cmdline.cmdline, "lcdtype=13") != NULL) {	\
               pr_err("LCD type S6D from bootloader\n");			\
               lcdtype = 13;							\
               strlcat(default_command_line, "lcdtype=13 ", COMMAND_LINE_SIZE);	\
        }

#endif /* _SETUP_MACHINE */
