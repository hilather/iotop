/* SPDX-License-Identifier: GPL-2.0-or-later

Print-time identity enrichment for batch mode (Phase R1).

Never runs on the sample/fetch hot path — only for rows about to be printed.
  - UID → username cache (getpwuid once per distinct euid)
  - lazy /proc/<pid>/cmdline (optional, -c)
  - lazy get_ioprio for visible rows

*/

#include "iotop.h"

#include <pwd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

uint64_t perf_n_getpwuid;
uint64_t perf_n_cmdline;
uint64_t perf_n_ioprio;

/* Open-addressed UID → name cache (power-of-two). */
#define UID_CACHE_SZ 256
#define UID_CACHE_MASK (UID_CACHE_SZ-1)

struct uid_ent {
	int used;
	uid_t uid;
	char name[32];
};

static struct uid_ent uid_cache[UID_CACHE_SZ];

static unsigned uid_hash(uid_t u) {
	return ((unsigned)u*2654435761u)&UID_CACHE_MASK;
}

/* Returns pointer to stable cache storage (valid until process exit). */
const char *batch_uid_name(uid_t uid) {
	unsigned h=uid_hash(uid);
	unsigned i;

	for (i=0;i<UID_CACHE_SZ;i++) {
		unsigned idx=(h+i)&UID_CACHE_MASK;
		struct uid_ent *e=&uid_cache[idx];
		if (e->used&&e->uid==uid)
			return e->name;
		if (!e->used) {
			struct passwd *pw;
			if (params.perf)
				perf_n_getpwuid++;
			pw=getpwuid(uid);
			e->used=1;
			e->uid=uid;
			if (pw&&pw->pw_name)
				snprintf(e->name,sizeof e->name,"%s",pw->pw_name);
			else
				snprintf(e->name,sizeof e->name,"%u",(unsigned)uid);
			return e->name;
		}
	}
	/* Cache full — fall back without insert. */
	{
		static char fallback[32];
		struct passwd *pw;
		if (params.perf)
			perf_n_getpwuid++;
		pw=getpwuid(uid);
		if (pw&&pw->pw_name)
			snprintf(fallback,sizeof fallback,"%s",pw->pw_name);
		else
			snprintf(fallback,sizeof fallback,"%u",(unsigned)uid);
		return fallback;
	}
}

/*
 * Read /proc/pid/cmdline into buf; NULs become spaces.
 * Returns length written (excluding trailing NUL), or 0 on failure.
 */
/* /proc/pid/comm — short name; cheap fallback when taskstats ac_comm empty. */
int batch_read_comm(pid_t pid,char *buf,size_t buflen) {
	char path[64];
	int fd;
	ssize_t n;

	if (!buf||buflen<2)
		return 0;
	buf[0]=0;
	snprintf(path,sizeof path,"/proc/%d/comm",pid);
	fd=open(path,O_RDONLY);
	if (fd<0)
		return 0;
	if (params.perf)
		perf_n_cmdline++;
	n=read(fd,buf,(ssize_t)buflen-1);
	close(fd);
	if (n<=0) {
		buf[0]=0;
		return 0;
	}
	while (n>0&&(buf[n-1]=='\n'||buf[n-1]=='\r'))
		n--;
	buf[n]=0;
	return (int)n;
}

int batch_read_cmdline(pid_t pid,char *buf,size_t buflen) {
	char path[64];
	int fd;
	ssize_t n,i;

	if (!buf||buflen<2)
		return 0;
	buf[0]=0;
	snprintf(path,sizeof path,"/proc/%d/cmdline",pid);
	fd=open(path,O_RDONLY);
	if (fd<0)
		return batch_read_comm(pid,buf,buflen);
	if (params.perf)
		perf_n_cmdline++;
	n=read(fd,buf,(ssize_t)buflen-1);
	close(fd);
	if (n<=0) {
		buf[0]=0;
		return batch_read_comm(pid,buf,buflen);
	}
	for (i=0;i<n;i++) {
		if (buf[i]==0)
			buf[i]=' ';
	}
	while (n>0&&buf[n-1]==' ')
		n--;
	buf[n]=0;
	return (int)n;
}

int batch_resolve_ioprio(pid_t tid) {
	int p;
	if (params.perf)
		perf_n_ioprio++;
	p=get_ioprio(tid);
	return p<0?0:p;
}

void batch_enrich_reset_perf(void) {
	perf_n_getpwuid=0;
	perf_n_cmdline=0;
	perf_n_ioprio=0;
}
