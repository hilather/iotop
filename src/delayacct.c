/* SPDX-License-Identifier: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020-2026  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * Kernel 5.14+ can gate delay accounting behind
 * /proc/sys/kernel/task_delayacct. When it is present and set to 0, SWAPIN/IO
 * delay fields from taskstats stay zero. We only warn (never write the sysctl)
 * so production hosts are not mutated by monitoring tools.
 */

static inline int _read_task_delayacct(int *da) {
	char buf[10],*t;
	ssize_t bs;
	int fd;

	if (!da)
		return EINVAL;

	if (-1==(fd=open("/proc/sys/kernel/task_delayacct",O_RDONLY)))
		return ENOENT;

	bs=read(fd,buf,sizeof buf-1);
	if (bs<0) {
		int e=errno;
		close(fd);
		return e;
	}
	if (bs==sizeof buf-1) {
		close(fd);
		return ENODATA;
	}
	buf[bs]=0;
	*da=strtol(buf,&t,10);
	if (*t!='\n'&&*t!='\0') {
		close(fd);
		return ENODATA;
	}
	close(fd);
	return 0;
}

inline int has_task_delayacct(void) {
	int da=0;

	if (_read_task_delayacct(&da))
		return 0;
	return 1;
}

inline int read_task_delayacct(void) {
	int da=0,r;

	r=_read_task_delayacct(&da);
	if (!r)
		return da;
	/* Sysctl absent (older kernels): treat as always-on accounting path. */
	if (r==ENOENT||r==ENODATA)
		return 1;
	return 0;
}

void warn_task_delayacct(void) {
	if (!has_task_delayacct())
		return; /* kernel has no toggle — nothing to warn about */

	if (read_task_delayacct())
		return;

	fprintf(stderr,
		"WARNING:\n"
		"\tKernel task delay accounting is disabled\n"
		"\t(/proc/sys/kernel/task_delayacct = 0).\n"
		"\tSWAPIN/IO delay percentages will be zero until it is enabled, e.g.:\n"
		"\t  sysctl kernel.task_delayacct=1\n"
		"\tThis build does not change the sysctl automatically.\n");
}
