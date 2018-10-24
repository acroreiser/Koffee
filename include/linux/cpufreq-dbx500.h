/*
 * Copyright (C) ST-Ericsson SA 2011
 *
 * License Terms: GNU General Public License v2
 */
#ifndef __CPUFREQ_DBX500_H
#define __CPUFREQ_DBX500_H

#include <linux/cpufreq.h>

int dbx500_cpufreq_get_limits(int cpu, int r,
			      unsigned int *min, unsigned int *max);

int dbx500_cpufreq_percent2freq(int percent);

#endif
