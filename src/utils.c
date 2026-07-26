/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <wchar.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

void find_cmd_and_ppid(int pid,struct xxxid_stats *s) {
	char path[64];
	int fd;
	int gc;
	int ppid;
	char str2[2];
	char str1[IOTOP_COMM_LEN];

	if (!s)
		return;
	s->cmdline1[0]=0;
	snprintf(path,sizeof path,"/proc/%d/stat",pid);
	fd=open(path,O_RDONLY);
	if (fd!=-1) {
		char buf[BUFSIZ+1];
		ssize_t n=read(fd,buf,BUFSIZ);
		close(fd);
		if (n>0) {
			buf[n]=0;
			if (sscanf(buf,"%i %31s %1s %i",&gc,str1,str2,&ppid)>=2) {
				/* str1 is like (cmd) — store as-is truncated */
				snprintf(s->cmdline1,sizeof s->cmdline1,"%s",str1);
			}
		}
	}
	if (!s->cmdline1[0])
		snprintf(s->cmdline1,sizeof s->cmdline1,"<unknown>");
}

/*
 * Interactive full/short cmdline from /proc (heap; caller frees).
 * isshort: basename of argv0; else full cmdline with NULs as spaces.
 * Falls back to /proc/pid/comm when cmdline is empty (kernel threads).
 */
char *read_cmdline(int pid,int isshort) {
	char path[64];
	char *dbuf;
	ssize_t n,p=0,sz=BUFSIZ;
	int fd;

	snprintf(path,sizeof path,"/proc/%d/cmdline",pid);
	fd=open(path,O_RDONLY);
	if (fd!=-1) {
		dbuf=malloc((size_t)sz+1);
		if (!dbuf) {
			close(fd);
			return NULL;
		}
		do {
			n=read(fd,dbuf+p,(size_t)(sz-p));
			if (n==sz-p) {
				char *t=realloc(dbuf,(size_t)sz+BUFSIZ+1);
				if (!t) {
					close(fd);
					free(dbuf);
					return NULL;
				}
				dbuf=t;
				sz+=BUFSIZ;
			}
			if (n>0)
				p+=n;
		} while (n>0);
		close(fd);

		if (p>0) {
			ssize_t k;
			dbuf[p]=0;
			if (isshort) {
				char *ep,*shortc;

				shortc=strdup(dbuf);
				free(dbuf);
				if (!shortc)
					return NULL;
				ep=strrchr(shortc,'/');
				if (ep&&ep[1]) {
					char *t=strdup(ep+1);
					if (t) {
						free(shortc);
						shortc=t;
					}
				}
				return shortc;
			}
			for (k=0;k<p;k++)
				if (!dbuf[k])
					dbuf[k]=' ';
			return dbuf;
		}
		free(dbuf);
	}

	/* Kernel threads / empty cmdline — use comm. */
	snprintf(path,sizeof path,"/proc/%d/comm",pid);
	fd=open(path,O_RDONLY);
	if (fd==-1)
		return NULL;
	dbuf=malloc(BUFSIZ+1);
	if (!dbuf) {
		close(fd);
		return NULL;
	}
	n=read(fd,dbuf,BUFSIZ);
	close(fd);
	if (n<=0) {
		free(dbuf);
		return NULL;
	}
	while (n>0&&(dbuf[n-1]=='\n'||dbuf[n-1]=='\r'))
		n--;
	dbuf[n]=0;
	return dbuf;
}

/*
 * Walk /proc. Main process first so pid_cb can cache the parent.
 *
 * P13: when params.walk_threads==0, do not open /proc/pid/task (process-only).
 * P14 pairs this with TGID netlink for leaders.
 */
inline void pidgen_cb(pg_cb cb,void *hint1,void *hint2,void *p) {
	DIR *pr;

	if (!(pr=opendir("/proc")))
		return;

	for (;;) {
		struct dirent *de=readdir(pr);
		char *eol=NULL;
		char path[64];
		pid_t pid;
		DIR *tr;

		if (!de)
			break;

		pid=strtol(de->d_name,&eol,10);
		if (eol==de->d_name||*eol!='\0'||pid<=0)
			continue;

		if (!is_a_process(pid))
			continue;

		cb(pid,pid,hint1,hint2,p);

		if (!params.walk_threads)
			continue;

		snprintf(path,sizeof path,"/proc/%d/task",pid);
		if (!(tr=opendir(path)))
			continue;

		for (;;) {
			struct dirent *tde=readdir(tr);
			pid_t tid;

			if (!tde)
				break;
			eol=NULL;
			tid=strtol(tde->d_name,&eol,10);
			if (eol==tde->d_name||*eol!='\0'||tid<=0)
				continue;
			if (pid==tid)
				continue;
			/* P6: no is_a_process on threads — netlink ESRCH handles races. */
			cb(pid,tid,hint1,hint2,p);
		}
		closedir(tr);
	}
	closedir(pr);
}

/* P10: format duration without localtime (units treated as milliseconds). */
void format_duration(uint64_t units,char *buf,size_t buflen) {
	uint64_t sec,m,h;

	if (!buf||!buflen)
		return;
	sec=units/1000ULL;
	h=sec/3600ULL;
	m=(sec%3600ULL)/60ULL;
	sec=sec%60ULL;
	if (h>99ULL)
		h=99ULL;
	snprintf(buf,buflen,"%02llu:%02llu:%02llu",
		(unsigned long long)h,(unsigned long long)m,(unsigned long long)sec);
}

inline int is_a_file(const char *p) {
	struct stat st;

	if (!p||lstat(p,&st))
		return 0;
	return (st.st_mode&S_IFMT)==S_IFREG;
}

inline int is_a_dir(const char *p) {
	struct stat st;

	if (!p||lstat(p,&st))
		return 0;
	return (st.st_mode&S_IFMT)==S_IFDIR;
}

/* Prefer /proc/<tid>/stat (regular file) over the directory — more reliable. */
inline int is_a_process(pid_t tid) {
	char path[64];

	if (tid<=0)
		return 0;
	snprintf(path,sizeof path,"/proc/%d/stat",tid);
	return is_a_file(path);
}

inline int64_t monotime(void) {
	struct timespec ts;
	int64_t res;

	clock_gettime(CLOCK_MONOTONIC,&ts);
	res=ts.tv_sec*1000;
	res+=ts.tv_nsec/1000000;
	return res;
}

inline const char *esc_low_ascii1(char c) {
	static char ehex[0x20][6];
	static int initialized=0;
	unsigned char uc=(unsigned char)c;

	/* char may be signed; only ASCII control bytes 0x00..0x1f need escaping. */
	if (uc>=0x20)
		return NULL;
	if (!initialized) {
		int i;

		for (i=0;i<0x20;i++)
			sprintf(ehex[i],"\\0x%02x",i);
		initialized=1;
	}
	switch (uc) {
		case 0x00: /* shorter form */
			return "\\0";
		case 0x07:
			return "\\a";
		case 0x08:
			return "\\b";
		case 0x09:
			return "\\t";
		case 0x0a:
			return "\\n";
		case 0x0b:
			return "\\v";
		case 0x0c:
			return "\\f";
		case 0x0d:
			return "\\r";
		case 0x1b:
			return "\\e";
		default:
			return ehex[uc];
	}
}

inline char *esc_low_ascii(char *p) {
	char *s=p,*res,*rp;
	int rc=0;

	if (!p)
		return NULL;

	// count
	while (*s) {
		const char *rs=esc_low_ascii1(*s++);

		if (!rs)
			rc++;
		else
			rc+=strlen(rs);
	}
	res=malloc(rc+1);
	if (!res)
		return NULL;
	// copy, start over from the beginning
	// two-pass over the string is faster than using realloc
	s=p;
	rp=res;
	while (*s) {
		const char *rs=esc_low_ascii1(*s++);

		if (!rs)
			*rp++=s[-1];
		else
			while (*rs)
				*rp++=*rs++;
	}
	*rp=0;
	return res;
}

#define UBLEN 1024

inline char *u8strpadt(const char *s,ssize_t rlen) {
	char *d=malloc(UBLEN);
	size_t dl=UBLEN;
	size_t si=0;
	size_t di=0;
	size_t tl=0;
	size_t len;
	size_t sl;
	wchar_t w;

	if (rlen<0)
		len=0;
	else
		len=rlen;
	if (!d)
		return NULL;
	if (!s)
		s="(null)";

	sl=strlen(s);
	mbtowc(NULL,NULL,0);
	for (;;) {
		int cl;
		int tw;

		if (!s[si])
			break;

		cl=mbtowc(&w,s+si,sl-si);
		if (cl<=0) {
			si++;
			continue;
		}
		if (dl-di<(size_t)cl+1) {
			char *t;

			dl+=UBLEN;
			t=realloc(d,dl);
			if (!t) {
				free(d);
				return NULL;
			}
			d=t;
		}
		tw=wcwidth(w);
		if (tw<0) {
			si+=cl;
			continue;
		}
		if (tw&&tw+tl>len)
			break;
		memcpy(d+di,s+si,cl);
		di+=cl;
		si+=cl;
		tl+=tw;
		d[di]=0;
	}
	while (tl<len) {
		if (dl-di<1+1) {
			char *t;

			dl+=UBLEN;
			t=realloc(d,dl);
			if (!t) {
				free(d);
				return NULL;
			}
			d=t;
		}
		d[di++]=' ';
		d[di]=0;
		tl++;
	}
	return d;
}

