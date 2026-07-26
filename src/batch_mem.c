/* SPDX-License-Identifier: GPL-2.0-or-later

Print-time memory snapshot for batch mode:
  - /proc/pid/status → VmRSS, VmSwap
  - /proc/pid/stat   → state (R/S/D/…)
  - /proc/pid/smaps_rollup → Private_Dirty when -D (kernel >= 4.14)

Never called from the sample/fetch path.

*/

#include "iotop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int have_smaps_rollup=0;
uint64_t perf_n_status;
uint64_t perf_n_smaps;

void batch_mem_probe(void) {
	/* Presence of the rollup file indicates kernel support. */
	have_smaps_rollup=is_a_file("/proc/self/smaps_rollup");
}

static int read_status_mem(pid_t pid,uint64_t *rss_kb,uint64_t *swap_kb) {
	char path[64],buf[1024];
	int fd;
	ssize_t n;
	char *p;

	*rss_kb=0;
	*swap_kb=0;
	snprintf(path,sizeof path,"/proc/%d/status",pid);
	fd=open(path,O_RDONLY);
	if (fd<0)
		return -1;
	if (params.perf)
		perf_n_status++;
	n=read(fd,buf,sizeof buf-1);
	close(fd);
	if (n<=0)
		return -1;
	buf[n]=0;
	p=buf;
	while (p&&*p) {
		char *nl=strchr(p,'\n');
		if (nl)
			*nl=0;
		if (!strncmp(p,"VmRSS:",6))
			*rss_kb=strtoull(p+6,NULL,10);
		else if (!strncmp(p,"VmSwap:",7))
			*swap_kb=strtoull(p+7,NULL,10);
		p=nl?nl+1:NULL;
	}
	return 0;
}

static char read_proc_state(pid_t pid) {
	char path[64],buf[512];
	int fd;
	ssize_t n;
	char *rparen;
	char st='?';

	snprintf(path,sizeof path,"/proc/%d/stat",pid);
	fd=open(path,O_RDONLY);
	if (fd<0)
		return '?';
	n=read(fd,buf,sizeof buf-1);
	close(fd);
	if (n<=0)
		return '?';
	buf[n]=0;
	/* format: pid (comm) state ... — comm may contain spaces/parens */
	rparen=strrchr(buf,')');
	if (rparen&&rparen[1]==' '&&rparen[2])
		st=rparen[2];
	return st;
}

static int read_private_dirty_kb(pid_t pid,uint64_t *dirty_kb) {
	char path[72],buf[2048];
	int fd;
	ssize_t n;
	char *p;

	*dirty_kb=0;
	if (!have_smaps_rollup)
		return -1;
	snprintf(path,sizeof path,"/proc/%d/smaps_rollup",pid);
	fd=open(path,O_RDONLY);
	if (fd<0)
		return -1;
	if (params.perf)
		perf_n_smaps++;
	n=read(fd,buf,sizeof buf-1);
	close(fd);
	if (n<=0)
		return -1;
	buf[n]=0;
	p=buf;
	while (p&&*p) {
		char *nl=strchr(p,'\n');
		if (nl)
			*nl=0;
		if (!strncmp(p,"Private_Dirty:",14)) {
			*dirty_kb=strtoull(p+14,NULL,10);
			return 0;
		}
		p=nl?nl+1:NULL;
	}
	return -1;
}

int batch_read_mem(struct xxxid_stats *s,int want_dirty) {
	if (!s)
		return -1;
	s->vm_rss_kb=0;
	s->vm_swap_kb=0;
	s->private_dirty_kb=0;
	s->proc_state=read_proc_state(s->pid);
	read_status_mem(s->pid,&s->vm_rss_kb,&s->vm_swap_kb);
	if (want_dirty&&have_smaps_rollup)
		read_private_dirty_kb(s->pid,&s->private_dirty_kb);
	return 0;
}
