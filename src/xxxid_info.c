/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/taskstats.h>
#include <linux/genetlink.h>

#include "taskstats-v14.h"
#include "taskstats-v15.h"

#define GENLMSG_DATA(glh)	((void *)((char*)NLMSG_DATA(glh)+GENL_HDRLEN))
#define GENLMSG_PAYLOAD(glh)	(NLMSG_PAYLOAD(glh,0)-GENL_HDRLEN)
#define NLA_DATA(na)		((void *)((char*)(na)+NLA_HDRLEN))
#define NLA_PAYLOAD(len)	(len-NLA_HDRLEN)

#define MAX_MSG_SIZE 1024

struct msgtemplate {
	struct nlmsghdr n;
	struct genlmsghdr g;
	char buf[MAX_MSG_SIZE];
};

static int nl_sock=-1;
static int nl_fam_id=0;

/* P8: freelist of xxxid_stats nodes */
static struct xxxid_stats *stats_freelist;

uint64_t perf_n_netlink;
uint64_t perf_n_proc;

struct xxxid_stats *alloc_stats(void) {
	struct xxxid_stats *s;

	if (stats_freelist) {
		s=stats_freelist;
		stats_freelist=s->pool_next;
		memset(s,0,sizeof *s);
		return s;
	}
	return calloc(1,sizeof *s);
}

inline void free_stats(struct xxxid_stats *s) {
	if (!s)
		return;
	/* Interactive heap identity (batch leaves these NULL). */
	if (s->cmdline2) {
		free(s->cmdline2);
		s->cmdline2=NULL;
	}
	if (s->pw_name) {
		free(s->pw_name);
		s->pw_name=NULL;
	}
	/* Thread list owns no items (shared with main array). */
	if (s->threads) {
		arr_free_noitem(s->threads);
		s->threads=NULL;
	}
	s->pool_next=stats_freelist;
	stats_freelist=s;
}

void stats_pool_clear(void) {
	while (stats_freelist) {
		struct xxxid_stats *s=stats_freelist;
		stats_freelist=s->pool_next;
		free(s);
	}
}

inline int send_cmd(int sock_fd,__u16 nlmsg_type,__u32 nlmsg_pid,__u8 genl_cmd,__u16 nla_type,void *nla_data,int nla_len) {
	struct nlattr *na;
	struct sockaddr_nl nladdr;
	int r,buflen;
	char *buf;
	struct msgtemplate msg;

	memset(&msg,0,sizeof msg);
	memset(msg.buf,0,sizeof msg.buf);

	msg.n.nlmsg_len=NLMSG_LENGTH(GENL_HDRLEN);
	msg.n.nlmsg_type=nlmsg_type;
	msg.n.nlmsg_flags=NLM_F_REQUEST;
	msg.n.nlmsg_seq=0;
	msg.n.nlmsg_pid=nlmsg_pid;
	msg.g.cmd=genl_cmd;
	msg.g.version=0x1;

	na=(struct nlattr *)GENLMSG_DATA(&msg);
	na->nla_type=nla_type;
	na->nla_len=nla_len+NLA_HDRLEN;
	memcpy(NLA_DATA(na),nla_data,nla_len);
	msg.n.nlmsg_len+=NLMSG_ALIGN(na->nla_len);

	buf=(char *)&msg;
	buflen=msg.n.nlmsg_len;
	memset(&nladdr,0,sizeof nladdr);
	nladdr.nl_family=AF_NETLINK;
	while ((r=sendto(sock_fd,buf,buflen,0,(struct sockaddr *)&nladdr,sizeof nladdr))<buflen) {
		if (r>0) {
			buf+=r;
			buflen-=r;
		} else if (errno!=EAGAIN)
			return -1;
	}
	return 0;
}

inline int get_family_id(int sock_fd) {
	struct msgtemplate answ;
	static char name[sizeof TASKSTATS_GENL_NAME];
	struct nlattr *na;
	ssize_t rep_len;
	int id=0;

	snprintf(name,sizeof name,"%s",TASKSTATS_GENL_NAME);
	if (send_cmd(sock_fd,GENL_ID_CTRL,getpid(),CTRL_CMD_GETFAMILY,CTRL_ATTR_FAMILY_NAME,(void *)name,strlen(TASKSTATS_GENL_NAME)+1))
		return 0;

	rep_len=recv(sock_fd,&answ,sizeof answ,0);
	if (rep_len<0||!NLMSG_OK((&answ.n),(size_t)rep_len)||answ.n.nlmsg_type==NLMSG_ERROR)
		return 0;

	na=(struct nlattr *)GENLMSG_DATA(&answ);
	na=(struct nlattr *)((char *)na+NLA_ALIGN(na->nla_len));
	if (na->nla_type==CTRL_ATTR_FAMILY_ID)
		id=*(__u16 *)NLA_DATA(na);
	return id;
}

inline void nl_init(void) {
	struct sockaddr_nl addr;
	int sock_fd=socket(PF_NETLINK,SOCK_RAW,NETLINK_GENERIC);

	if (sock_fd<0)
		goto error;
	memset(&addr,0,sizeof addr);
	addr.nl_family=AF_NETLINK;
	if (bind(sock_fd,(struct sockaddr *)&addr,sizeof addr)<0)
		goto error;

	nl_sock=sock_fd;
	nl_fam_id=get_family_id(sock_fd);
	if (!nl_fam_id) {
		fprintf(stderr,"nl_init: couldn't get netlink family id\n");
		exit(EXIT_FAILURE);
	}
	return;

error:
	if (sock_fd>-1)
		close(sock_fd);
	fprintf(stderr,"nl_init: %s\n",strerror(errno));
	exit(EXIT_FAILURE);
}

static void copy_comm(char *dst,const char *src,size_t srclen) {
	size_t n=srclen<IOTOP_COMM_LEN?srclen:IOTOP_COMM_LEN-1;
	memcpy(dst,src,n);
	dst[n]=0;
	/* taskstats ac_comm is not always NUL-terminated within TS_COMM_LEN */
	dst[IOTOP_COMM_LEN-1]=0;
}

static void apply_taskstats_payload(struct xxxid_stats *stats,const void *data,size_t payload) {
	__u16 ver=0;

	if (!stats||!data||payload<sizeof ver)
		return;

	memcpy(&ver,data,sizeof ver);
	if (!taskstats_ver)
		taskstats_ver=ver;
	if (ver<IOTOP_TASKSTATS_MINVER)
		return;

	if (ver==15) {
		struct taskstats_v15 t15;
		size_t ncopy=payload<sizeof t15?payload:sizeof t15;
		memset(&t15,0,sizeof t15);
		memcpy(&t15,data,ncopy);
		stats->read_bytes=t15.read_bytes;
		stats->write_bytes=t15.write_bytes;
		stats->ac_ppid=(pid_t)t15.ac_ppid;
		stats->swapin_delay_total=t15.swapin_delay_total;
		stats->blkio_delay_total=t15.blkio_delay_total;
		stats->cancelled_write_bytes=t15.cancelled_write_bytes;
		stats->ac_utime=t15.ac_utime;
		stats->ac_stime=t15.ac_stime;
		stats->ac_majflt=t15.ac_majflt;
		stats->ac_minflt=t15.ac_minflt;
		stats->nvcsw=t15.nvcsw;
		stats->nivcsw=t15.nivcsw;
		stats->freepages_delay_total=t15.freepages_delay_total;
		stats->thrashing_delay_total=t15.thrashing_delay_total;
		stats->hiwater_rss=t15.hiwater_rss;
		stats->ac_btime=t15.ac_btime;
		stats->cpu_delay_total=t15.cpu_delay_total;
		stats->euid=(int)t15.ac_uid;
		copy_comm(stats->cmdline1,t15.ac_comm,sizeof t15.ac_comm);
	} else {
		struct taskstats_v14 t14;
		size_t ncopy=payload<sizeof t14?payload:sizeof t14;
		memset(&t14,0,sizeof t14);
		memcpy(&t14,data,ncopy);
		stats->read_bytes=t14.read_bytes;
		stats->write_bytes=t14.write_bytes;
		stats->ac_ppid=(pid_t)t14.ac_ppid;
		stats->swapin_delay_total=t14.swapin_delay_total;
		stats->blkio_delay_total=t14.blkio_delay_total;
		stats->cancelled_write_bytes=t14.cancelled_write_bytes;
		stats->ac_utime=t14.ac_utime;
		stats->ac_stime=t14.ac_stime;
		stats->ac_majflt=t14.ac_majflt;
		stats->ac_minflt=t14.ac_minflt;
		stats->nvcsw=t14.nvcsw;
		stats->nivcsw=t14.nivcsw;
		stats->freepages_delay_total=t14.freepages_delay_total;
		stats->thrashing_delay_total=t14.thrashing_delay_total;
		stats->hiwater_rss=t14.hiwater_rss;
		stats->ac_btime=t14.ac_btime;
		stats->cpu_delay_total=t14.cpu_delay_total;
		stats->euid=(int)t14.ac_uid;
		copy_comm(stats->cmdline1,t14.ac_comm,sizeof t14.ac_comm);
	}
}

void warn_taskstats_version(void) {
	if (!taskstats_ver)
		return;
	if (taskstats_ver<IOTOP_TASKSTATS_MINVER) {
		fprintf(stderr,
			"WARNING:\n"
			"\tThis kernel provides struct taskstats with version %u.\n"
			"\tThat does not contain the required data and should be %u or greater.\n",
			taskstats_ver,IOTOP_TASKSTATS_MINVER);
		return;
	}
	if (taskstats_ver>IOTOP_TASKSTATS_VERSION) {
		fprintf(stderr,
			"WARNING:\n"
			"\tThis kernel provides struct taskstats with version %u.\n"
			"\tiotop understands layouts through version %u (v15 special-cased;\n"
			"\tv16+ assumed v14-compatible for the fields this build uses).\n",
			taskstats_ver,IOTOP_TASKSTATS_VERSION);
	}
}

inline int nl_xxxid_info(pid_t tid,pid_t pid,struct xxxid_stats *stats) {
	__u16 attr;
	pid_t key;
	struct msgtemplate msg;
	ssize_t rv;

	if (nl_sock<0||nl_fam_id==0)
		return -1;

	/*
	 * P14: for process leaders, prefer TGID so the kernel returns
	 * thread-group aggregates without walking every tid.
	 */
	if (params.use_tgid&&pid==tid) {
		attr=TASKSTATS_CMD_ATTR_TGID;
		key=pid;
	} else {
		attr=TASKSTATS_CMD_ATTR_PID;
		key=tid;
	}

	if (send_cmd(nl_sock,nl_fam_id,tid,TASKSTATS_CMD_GET,attr,&key,sizeof key))
		return -1;

	if (params.perf)
		perf_n_netlink++;

	stats->pid=pid;
	stats->tid=tid;

	rv=recv(nl_sock,&msg,sizeof msg,0);
	if (rv<0||!NLMSG_OK((&msg.n),(size_t)rv)||msg.n.nlmsg_type==NLMSG_ERROR) {
		/* P12: stay quiet on the hot path (ESRCH is normal under churn). */
		return -1;
	}

	rv=GENLMSG_PAYLOAD(&msg.n);
	{
		char *genl_base=(char *)GENLMSG_DATA(&msg);
		int len=0;

		while (len<rv) {
			struct nlattr *na=(struct nlattr *)(genl_base+len);
			if (!na->nla_len)
				break;
			if (na->nla_type==TASKSTATS_TYPE_AGGR_TGID||na->nla_type==TASKSTATS_TYPE_AGGR_PID) {
				int aggr_len=NLA_PAYLOAD(na->nla_len);
				char *nested_base=(char *)NLA_DATA(na);
				int len2=0;
				while (len2<aggr_len) {
					struct nlattr *nna=(struct nlattr *)(nested_base+len2);
					if (!nna->nla_len)
						break;
					if (nna->nla_type==TASKSTATS_TYPE_STATS)
						apply_taskstats_payload(stats,NLA_DATA(nna),NLA_PAYLOAD(nna->nla_len));
					if (nna->nla_len<NLA_HDRLEN)
						break;
					len2+=NLA_ALIGN(nna->nla_len);
				}
			}
			if (na->nla_len<NLA_HDRLEN)
				break;
			len+=NLA_ALIGN(na->nla_len);
		}
	}

	/* Batch leaves io_prio 0; interactive make_stats fills it after fetch. */
	stats->io_prio=0;
	return 0;
}

inline void nl_fini(void) {
	if (nl_sock>-1)
		close(nl_sock);
	nl_sock=-1;
	stats_pool_clear();
}

/*
 * Batch hot path: netlink counters only. No getpwuid / cmdline / ioprio.
 * Process leaders are lstat-checked; threads rely on netlink ESRCH (P5/P6).
 */
static struct xxxid_stats *make_stats_batch(pid_t tid,pid_t pid) {
	struct xxxid_stats *s;

	if (pid==tid) {
		if (!is_a_process(tid))
			return NULL;
	}

	s=alloc_stats();
	if (!s)
		return NULL;

	if (nl_xxxid_info(tid,pid,s)) {
		free_stats(s);
		return NULL;
	}
	return s;
}

/*
 * Interactive path (original iotop-c style): full identity for curses.
 * USER, PRIO, short+long cmdline, and every tid is retained for thread view.
 */
static struct xxxid_stats *make_stats_interactive(pid_t tid,pid_t pid) {
	static const char unknown[]="<unknown>";
	struct xxxid_stats *s;
	const char *uname;
	int prio;
	char *cl;

	if (!is_a_process(tid))
		return NULL;

	s=alloc_stats();
	if (!s)
		return NULL;

	if (nl_xxxid_info(tid,pid,s)) {
		free_stats(s);
		return NULL;
	}

	prio=get_ioprio(tid);
	s->io_prio=prio<0?0:prio;

	uname=batch_uid_name((uid_t)s->euid);
	s->pw_name=strdup(uname?uname:unknown);

	/* Prefer /proc cmdline short form over ac_comm when available. */
	cl=read_cmdline(tid,1);
	if (cl&&cl[0]) {
		snprintf(s->cmdline1,sizeof s->cmdline1,"%s",cl);
		free(cl);
	} else if (cl)
		free(cl);

	s->cmdline2=read_cmdline(tid,0);
	if (!s->cmdline2||!s->cmdline2[0]) {
		if (s->cmdline2)
			free(s->cmdline2);
		s->cmdline2=strdup(s->cmdline1[0]?s->cmdline1:unknown);
	}

	if (!s->pw_name)
		s->pw_name=strdup(unknown);

	return s;
}

/*
 * Batch callback: fold threads into the process leader and free thread nodes.
 * Cache *tp as the last main-process entry to avoid arr_find on every tid.
 */
static void pid_cb_batch(pid_t pid,pid_t tid,struct xxxid_stats_arr *a,filter_callback filter,struct xxxid_stats **tp) {
	struct xxxid_stats *s=make_stats_batch(tid,pid);
	struct xxxid_stats *p=NULL;

	(void)filter;

	if (!s)
		return;

	if (params.perf&&pid==tid)
		perf_n_proc++;

	if (tp&&*tp)
		p=*tp;

	if (pid!=tid) {
		if (!p||p->pid!=s->pid) {
			p=arr_find(a,s->pid);
			if (tp)
				*tp=p;
		}
		if (p) {
			p->swapin_delay_total+=s->swapin_delay_total;
			p->blkio_delay_total+=s->blkio_delay_total;
			p->freepages_delay_total+=s->freepages_delay_total;
			p->thrashing_delay_total+=s->thrashing_delay_total;
			p->read_bytes+=s->read_bytes;
			p->write_bytes+=s->write_bytes;
			p->cancelled_write_bytes+=s->cancelled_write_bytes;
			p->ac_utime+=s->ac_utime;
			p->ac_stime+=s->ac_stime;
			p->cpu_delay_total+=s->cpu_delay_total;
			p->ac_majflt+=s->ac_majflt;
			p->ac_minflt+=s->ac_minflt;
			p->nvcsw+=s->nvcsw;
			p->nivcsw+=s->nivcsw;
			if (s->hiwater_rss>p->hiwater_rss)
				p->hiwater_rss=s->hiwater_rss;
		}
		free_stats(s);
		return;
	}

	if (arr_add(a,s)) {
		free_stats(s);
		return;
	}
	if (tp)
		*tp=s;
}

/*
 * Interactive callback: keep every tid in the main array (Tomas-M style).
 * Thread lists are linked in a second pass after the walk.
 */
static void pid_cb_interactive(pid_t pid,pid_t tid,struct xxxid_stats_arr *a,filter_callback filter,struct xxxid_stats **tp) {
	struct xxxid_stats *s=make_stats_interactive(tid,pid);

	(void)tp;

	if (!s)
		return;

	if (filter&&filter(s)) {
		free_stats(s);
		return;
	}

	if (arr_add(a,s))
		free_stats(s);
}

/* Attach non-leader tids under their process for curses tree display. */
static void link_interactive_threads(struct xxxid_stats_arr *a) {
	int i;

	if (!a||!a->arr)
		return;

	for (i=0;i<a->length;i++) {
		struct xxxid_stats *s=a->arr[i];
		struct xxxid_stats *p;

		if (!s||s->pid==s->tid)
			continue;
		p=arr_find(a,s->pid);
		if (!p)
			continue;
		if (!p->threads)
			p->threads=arr_alloc();
		if (!p->threads)
			continue;
		arr_add(p->threads,s);
	}
}

/* Interactive fetch — original iotop behaviour for curses. */
inline struct xxxid_stats_arr *fetch_data(filter_callback filter) {
	struct xxxid_stats_arr *a=arr_alloc();
	struct xxxid_stats *cache=NULL;

	if (!a)
		return NULL;
	pidgen_cb((pg_cb)pid_cb_interactive,a,filter,&cache);
	arr_hash_build(a);
	link_interactive_threads(a);
	return a;
}

/* Batch fetch — performance path (fold threads, no identity). */
inline struct xxxid_stats_arr *fetch_batch_data(struct xxxid_stats **p) {
	struct xxxid_stats_arr *a=arr_alloc();

	if (!a)
		return NULL;
	pidgen_cb((pg_cb)pid_cb_batch,a,NULL,p);
	arr_hash_build(a);
	return a;
}
