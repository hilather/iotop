/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <pwd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/taskstats.h>
#include <linux/genetlink.h>

/*
 * Recent Linux kernels broke backwards compatibility in struct taskstats.
 * Field offsets differ between versions (notably v15 inserted max/min delay
 * fields). Build-host <linux/taskstats.h> is only used for command/type
 * constants and TS_COMM_LEN; payload field access uses pinned layouts.
 */
#include "taskstats-v14.h"
#include "taskstats-v15.h"

/*
 * Generic macros for dealing with netlink sockets. Might be duplicated
 * elsewhere. It is recommended that commercial grade applications use
 * libnl or libnetlink and use the interfaces provided by the library
 */
#define GENLMSG_DATA(glh)	   ((void *)((char*)NLMSG_DATA(glh) + GENL_HDRLEN))
#define GENLMSG_PAYLOAD(glh)	(NLMSG_PAYLOAD(glh, 0) - GENL_HDRLEN)
#define NLA_DATA(na)			((void *)((char*)(na) + NLA_HDRLEN))
#define NLA_PAYLOAD(len)		(len - NLA_HDRLEN)

#define MAX_MSG_SIZE 1024

struct msgtemplate {
	struct nlmsghdr n;
	struct genlmsghdr g;
	char buf[MAX_MSG_SIZE];
};

static int nl_sock=-1;
static int nl_fam_id=0;

inline int send_cmd(int sock_fd,__u16 nlmsg_type,__u32 nlmsg_pid,__u8 genl_cmd,__u16 nla_type,void *nla_data,int nla_len) {
	struct nlattr *na;
	struct sockaddr_nl nladdr;
	int r,buflen;
	char *buf;

	struct msgtemplate msg;

	memset(&msg,0,sizeof msg);
	// make cppcheck happier; hopefully the optimizer should remove this
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
		} else
			if (errno!=EAGAIN)
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

/*
 * Copy every field this fork still uses from the correct taskstats layout.
 * v15 is the odd one out; v4..v14 and v16+ use the v14 field offsets for the
 * members we care about (same policy as Tomas-M/iotop 1.30+).
 *
 * NLA payload is only NLA_HDRLEN-aligned; always memcpy into a local aligned
 * struct before reading u64 members.
 */
static void apply_taskstats_payload(struct xxxid_stats *stats,const void *data,size_t payload) {
	__u16 ver=0;

	if (!stats||!data||payload<sizeof ver)
		return;

	memcpy(&ver,data,sizeof ver);

	/* Record first-seen kernel ABI version for exit-time diagnostics. */
	if (!taskstats_ver)
		taskstats_ver=ver;

	if (ver<IOTOP_TASKSTATS_MINVER)
		return;

	if (ver==15) {
		struct taskstats_v15 t15;
		size_t ncopy=payload<sizeof t15?payload:sizeof t15;

		memset(&t15,0,sizeof t15);
		memcpy(&t15,data,ncopy);

		#define COPY15(field) do { stats->field = t15.field; } while (0)
		COPY15(read_bytes);
		COPY15(write_bytes);
		COPY15(ac_ppid);
		COPY15(swapin_delay_total);
		COPY15(blkio_delay_total);
		COPY15(cancelled_write_bytes);
		COPY15(ac_utime);
		COPY15(ac_stime);
		COPY15(ac_majflt);
		COPY15(coremem);
		COPY15(hiwater_rss);
		COPY15(freepages_delay_total);
		COPY15(ac_btime);
		COPY15(ac_etime);
		COPY15(cpu_delay_total);
		COPY15(cpu_run_real_total);
		COPY15(cpu_run_virtual_total);
		#undef COPY15
		stats->euid=t15.ac_uid;
		if (stats->cmdline1)
			free(stats->cmdline1);
		stats->cmdline1=strndup(t15.ac_comm,sizeof t15.ac_comm);
	} else {
		/* v4..v14 and expected v16+ layout for our fields */
		struct taskstats_v14 t14;
		size_t ncopy=payload<sizeof t14?payload:sizeof t14;

		memset(&t14,0,sizeof t14);
		memcpy(&t14,data,ncopy);

		#define COPY14(field) do { stats->field = t14.field; } while (0)
		COPY14(read_bytes);
		COPY14(write_bytes);
		COPY14(ac_ppid);
		COPY14(swapin_delay_total);
		COPY14(blkio_delay_total);
		COPY14(cancelled_write_bytes);
		COPY14(ac_utime);
		COPY14(ac_stime);
		COPY14(ac_majflt);
		COPY14(coremem);
		COPY14(hiwater_rss);
		COPY14(freepages_delay_total);
		COPY14(ac_btime);
		COPY14(ac_etime);
		COPY14(cpu_delay_total);
		COPY14(cpu_run_real_total);
		COPY14(cpu_run_virtual_total);
		#undef COPY14
		stats->euid=t14.ac_uid;
		if (stats->cmdline1)
			free(stats->cmdline1);
		stats->cmdline1=strndup(t14.ac_comm,sizeof t14.ac_comm);
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

	/*
	 * v15 is the known special layout. v16+ is assumed to follow v14 offsets
	 * for the fields we use (Tomas-M policy); note unknown majors so ops notice.
	 */
	if (taskstats_ver>IOTOP_TASKSTATS_VERSION) {
		fprintf(stderr,
			"WARNING:\n"
			"\tThis kernel provides struct taskstats with version %u.\n"
			"\tiotop understands layouts through version %u (v15 special-cased;\n"
			"\tv16+ assumed v14-compatible for the fields this build uses).\n"
			"\tIf counters look wrong, the taskstats ABI may have changed again.\n",
			taskstats_ver,IOTOP_TASKSTATS_VERSION);
	}
}

inline int nl_xxxid_info(pid_t tid,pid_t pid,struct xxxid_stats *stats) {
	if (nl_sock<0) {
		fprintf(stderr,"nl_xxxid_info: nl_sock is %d\n",nl_sock);
		exit(EXIT_FAILURE);
	}
	if (nl_fam_id==0) {
		fprintf(stderr,"nl_xxxid_info: nl_fam_id is 0\n");
		exit(EXIT_FAILURE);
	}

	if (send_cmd(nl_sock,nl_fam_id,tid,TASKSTATS_CMD_GET,TASKSTATS_CMD_ATTR_PID,&tid,sizeof tid)) {
		fprintf(stderr,"get_xxxid_info: %s\n",strerror(errno));
		return -1;
	}

	stats->pid=pid;
	stats->tid=tid;

	struct msgtemplate msg;
	ssize_t rv=recv(nl_sock,&msg,sizeof msg,0);

	if (rv<0||!NLMSG_OK((&msg.n),(size_t)rv)||msg.n.nlmsg_type==NLMSG_ERROR) {
		struct nlmsgerr *err=NLMSG_DATA(&msg);
		if (err->error!=-ESRCH)
			fprintf(stderr,"fatal reply error, %d\n",err->error);
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

					if (nna->nla_type==TASKSTATS_TYPE_STATS) {
						apply_taskstats_payload(stats,NLA_DATA(nna),NLA_PAYLOAD(nna->nla_len));
					}
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

	/* Hot path: skip get_ioprio (performance fork). */
	stats->io_prio=0;

	return 0;
}

inline void nl_fini(void) {
	if (nl_sock>-1)
		close(nl_sock);
}

inline void free_stats(struct xxxid_stats *s) {
	if (s->cmdline1)
		free(s->cmdline1);
	if (s->cmdline2)
		free(s->cmdline2);
	if (s->pw_name)
		free(s->pw_name);
	arr_free_noitem(s->threads);

	free(s);
}

inline struct xxxid_stats *make_stats(pid_t tid,pid_t pid) {
	struct xxxid_stats *s;

	/*
	 * Skip invisible / already-exited tasks before netlink (Tomas-M
	 * "Skip invisible processes"). Cheap lstat vs a full taskstats round-trip.
	 */
	if (!is_a_process(tid))
		return NULL;

	s=calloc(1,sizeof *s);
	if (!s)
		return NULL;

	if (nl_xxxid_info(tid,pid,s)) {
		/* Raced with exit between is_a_process and netlink — drop quietly. */
		if (!is_a_process(tid)) {
			free_stats(s);
			return NULL;
		}
		free_stats(s);
		return NULL;
	}

	/* Hot path: no getpwuid / full cmdline / get_ioprio (performance fork). */
	return s;
}

/*
 * Process/thread walk callback.
 *
 * Optimization vs upstream: cache the last main-process stats pointer in *tp
 * so consecutive threads of the same pid avoid arr_find().
 *
 * Important invariants (bugs fixed here):
 *  - *tp must always point at a main process (pid==tid) entry in `a`, never a
 *    thread-only stats object. The old code did `*tp = s` for threads, which
 *    leaked every thread allocation and aggregated into the wrong object.
 *  - Thread-only stats are always free_stats()'d after aggregation (or if the
 *    parent process is missing).
 */
static void pid_cb(pid_t pid,pid_t tid,struct xxxid_stats_arr *a,filter_callback filter, struct xxxid_stats** tp) {
	struct xxxid_stats *s=make_stats(tid,pid);
	struct xxxid_stats *p = NULL;

	(void)filter; /* batch path currently does not filter at fetch time */

	if (!s)
		return;

	if (tp && *tp)
		p = *tp;

	if (pid != tid) {
		/* Thread: fold into main process (tid == pid for the process entry). */
		if (!p || p->pid != s->pid) {
			p = arr_find(a, s->pid);
			if (tp)
				*tp = p;
		}
		if (p) {
			p->swapin_delay_total += s->swapin_delay_total;
			p->blkio_delay_total += s->blkio_delay_total;
			p->read_bytes += s->read_bytes;
			p->write_bytes += s->write_bytes;
			p->cancelled_write_bytes += s->cancelled_write_bytes;
			p->ac_utime += s->ac_utime;
			p->ac_stime += s->ac_stime;
			p->cpu_delay_total += s->cpu_delay_total;
			p->ac_majflt += s->ac_majflt;
		}
		/* Thread row is not retained in the array. */
		free_stats(s);
		return;
	}

	/* Main process (pid == tid). */
	if (arr_add(a, s)) {
		free_stats(s);
		return;
	}
	if (tp)
		*tp = s;
}

inline struct xxxid_stats_arr *fetch_data(filter_callback filter) {
	struct xxxid_stats_arr *a=arr_alloc();
	struct xxxid_stats *cache = NULL;

	if (!a)
		return NULL;

	/* Pass &cache (stats**), not a dummy allocation — matches pid_cb signature. */
	pidgen_cb((pg_cb)pid_cb, a, filter, &cache);
	return a;
}

inline struct xxxid_stats_arr *fetch_batch_data(struct xxxid_stats** p) {
	struct xxxid_stats_arr *a=arr_alloc();

	if (!a)
		return NULL;

	pidgen_cb((pg_cb)pid_cb, a, NULL, p);
	return a;
}

