/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#define TIMEDIFF_IN_S(sta,end) ((((sta)==(end))||(sta)==0)?0.0001:(((end)-(sta))/1000.0))

/* P20: growable print buffer — single fwrite per sample block. */
struct printbuf {
	char *data;
	size_t len;
	size_t cap;
};

static int pb_reserve(struct printbuf *pb,size_t need) {
	char *n;
	size_t ncap;

	if (pb->len+need+1<=pb->cap)
		return 0;
	ncap=pb->cap?pb->cap*2:65536;
	while (ncap<pb->len+need+1)
		ncap*=2;
	n=realloc(pb->data,ncap);
	if (!n)
		return -1;
	pb->data=n;
	pb->cap=ncap;
	return 0;
}

static void pb_printf(struct printbuf *pb,const char *fmt,...) {
	va_list ap;
	int n;

	for (;;) {
		size_t avail=pb->cap>pb->len?pb->cap-pb->len:0;
		va_start(ap,fmt);
		n=vsnprintf(pb->data?pb->data+pb->len:NULL,avail,fmt,ap);
		va_end(ap);
		if (n<0)
			return;
		if ((size_t)n<avail) {
			pb->len+=(size_t)n;
			return;
		}
		if (pb_reserve(pb,(size_t)n+1))
			return;
	}
}

uint64_t perf_fetch_ms;
uint64_t perf_diff_ms;
uint64_t perf_print_ms;

static inline void view_batch(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,struct act_stats *act,int diff_len,double time_s,struct printbuf *pb) {
	double total_a_read,total_a_write;
	char str_a_read[4],str_a_write[4];
	double total_read,total_write;
	char str_read[4],str_write[4];
	int i,limit;

	(void)ps;
	if (!cs||!pb)
		return;

	calc_total(cs,&total_read,&total_write);
	calc_a_total(act,&total_a_read,&total_a_write,time_s);

	humanize_val(&total_read,str_read,1);
	humanize_val(&total_write,str_write,1);
	humanize_val(&total_a_read,str_a_read,0);
	humanize_val(&total_a_write,str_a_write,0);

	pb_printf(pb,HEADER1_FORMAT,total_read,str_read,"",total_write,str_write,"");
	if (config.f.timestamp) {
		time_t t=time(NULL);
		pb_printf(pb," | %s",ctime(&t));
	} else
		pb_printf(pb,"\n");

	pb_printf(pb,HEADER2_FORMAT,total_a_read,str_a_read,"",total_a_write,str_a_write,"");
	pb_printf(pb,"\n");

	if (!config.f.quiet)
		pb_printf(pb,"%6s %6s %6s %4s %8s %11s %11s %11s %11s %5s %8s %8s %8s %8s %8s %s\n",
			"PID","TID","PPID","PRIO","USER","DISK READ","DISK WRITE","CANCELLEDW","RSS","MJRFLT","SWAPIN","IO","CDELAY","UTIME","STIME","COMMAND");

	/* P11/P18: sort with optional top-N */
	arr_sort_top(cs,iotop_sort_cb,params.top_n);

	if (diff_len<0)
		diff_len=0;
	if (cs->length<diff_len)
		diff_len=cs->length;
	limit=diff_len;
	if (params.top_n>0&&params.top_n<limit)
		limit=params.top_n;

	for (i=0;cs->sor&&i<limit;i++) {
		struct xxxid_stats *s=cs->sor[i];
		double read_val,write_val,canceled_val,coremem_val;
		uint64_t mf_val;
		char read_str[4],write_str[4],canceled_str[4],coremem_str[4];
		char utime[16],stime[16];

		if (!s)
			continue;

		read_val=config.f.accumulated?s->read_val_acc:s->read_val;
		write_val=config.f.accumulated?s->write_val_acc:s->write_val;
		canceled_val=config.f.accumulated?s->cancelled_write_bytes_val_acc:s->cancelled_write_bytes_val;
		coremem_val=s->coremem_val;
		mf_val=config.f.accumulated?s->ac_majflt_total:s->ac_majflt;

		if (config.f.processes&&s->pid!=s->tid)
			continue;
		if (config.f.only&&!read_val&&!write_val&&!s->ac_utime_val_acc&&!s->ac_stime_val_acc)
			continue;
		if (params.samplerate==1000&&s->exited)
			continue;

		humanize_val(&read_val,read_str,1);
		humanize_val(&write_val,write_str,1);
		humanize_val(&canceled_val,canceled_str,1);
		humanize_val(&coremem_val,coremem_str,1);

		/* P3: USER always blank on hot path — fixed width, no malloc */
		/* P10: integer duration format */
		format_duration(s->ac_utime_val_acc,utime,sizeof utime);
		format_duration(s->ac_stime_val_acc,stime,sizeof stime);

		pb_printf(pb,"%6i %6i %6i %4s %-8s %7.2f %-3.3s %7.2f %-3.3s %7.2f %-3.3s %7.2f %-3.3s %4llu %6.2f %% %6.2f %% %6.2f %% %8s %8s %s\n",
			s->pid,s->tid,(int)s->ac_ppid,str_ioprio(s->io_prio),"-",
			read_val,read_str,write_val,write_str,
			canceled_val,canceled_str,coremem_val,coremem_str,
			(unsigned long long)mf_val,s->swapin_val,s->blkio_val,s->cpu_delay_total_val,
			utime,stime,s->cmdline1[0]?s->cmdline1:"(unknown)");

		reset_pid(s);
	}
}

inline void view_batch_init(void) {
	/* Batch defaults: process-only + TGID unless user forced -T */
}

inline void view_batch_fini(void) {
}

int msleep(long msec) {
	struct timespec ts;
	int res;

	if (msec<0) {
		errno=EINVAL;
		return -1;
	}
	ts.tv_sec=msec/1000;
	ts.tv_nsec=(msec%1000)*1000000;
	do {
		res=nanosleep(&ts,&ts);
	} while (res&&errno==EINTR);
	return res;
}

inline void reset_pid(struct xxxid_stats *cs) {
	zero_pid_values(cs);
}

void view_batch_loop(void) {
	struct xxxid_stats_arr *ps=arr_alloc();
	struct xxxid_stats_arr *cs=NULL;
	struct act_stats act={0};
	struct printbuf pb={0};
	int iters=0;
	int rests=0;
	int flush=0;
	double time_s;
	double time_diff=0;
	uint64_t sampler_rate=params.samplerate;
	uint64_t t_quick_diff=0;
	uint64_t next_print=0;
	struct xxxid_stats *p=NULL;
	int diff_len=0;
	int new_pids=0;
	int untracked=0;
	int ppid_miss=0;

	if (!ps) {
		fprintf(stderr,"view_batch_loop: out of memory\n");
		return;
	}

	/* Batch defaults for performance (P13/P14) unless -T was set. */
	if (!params.walk_threads)
		params.use_tgid=1;

	for (;;) {
		uint64_t t0,t1,t2,t3;

		p=NULL;
		t0=monotime();

		cs=fetch_batch_data(&p);
		if (!cs) {
			fprintf(stderr,"view_batch_loop: fetch_batch_data failed\n");
			msleep(params.samplerate>0?params.samplerate:100);
			continue;
		}
		t1=monotime();
		if (params.perf)
			perf_fetch_ms=t1-t0;

		act.ts_c=t1;
		if (!next_print)
			next_print=act.ts_c+params.delay*1000;

		if (act.ts_c>=next_print) {
			time_s=TIMEDIFF_IN_S(act.ts_r,act.ts_c);
			time_diff+=time_s;
			/* keep_exited=1 on print samples */
			diff_len=create_quick_diff(cs,ps,time_s,NULL,0,NULL,1,&new_pids,&untracked,&ppid_miss,1);
			t2=monotime();
			if (params.perf)
				perf_diff_ms=t2-t1;

			get_vm_counters(&act.read_bytes,&act.write_bytes);

			pb.len=0;
			pb_printf(&pb,"Samples=%i, Rests=%i, NewPIDs=%i, Miss=%i, PPIDMiss=%i, ArrLength=%i, Size=%i, Time Taken: %2.1f sec",
				iters,rests,new_pids,ppid_miss,untracked,
				(ps&&ps->arr)?ps->length:0,
				(ps&&ps->arr)?ps->size:0,
				time_diff);
			if (params.perf)
				pb_printf(&pb," | PERF fetch_ms=%llu diff_ms=%llu n_nl=%llu n_proc=%llu",
					(unsigned long long)perf_fetch_ms,
					(unsigned long long)perf_diff_ms,
					(unsigned long long)perf_n_netlink,
					(unsigned long long)perf_n_proc);
			pb_printf(&pb,"\n");

			view_batch(cs,ps,&act,diff_len,time_diff,&pb);
			t3=monotime();
			if (params.perf)
				perf_print_ms=t3-t2;

			/* P20: one write */
			if (pb.data&&pb.len)
				fwrite(pb.data,1,pb.len,stdout);
			fflush(stdout);

			if (params.perf)
				fprintf(stderr,"PERF,fetch_ms=%llu,diff_ms=%llu,print_ms=%llu,n_netlink=%llu,n_proc=%llu,arr=%i\n",
					(unsigned long long)perf_fetch_ms,
					(unsigned long long)perf_diff_ms,
					(unsigned long long)perf_print_ms,
					(unsigned long long)perf_n_netlink,
					(unsigned long long)perf_n_proc,
					cs?cs->length:0);

			perf_n_netlink=0;
			perf_n_proc=0;
			act.have_o=1;
			iters=rests=0;
			time_diff=0;
			flush=0;
			new_pids=0;
			untracked=0;
			ppid_miss=0;
			if ((params.iter>-1)&&((--params.iter)==0))
				break;
		} else {
			time_s=TIMEDIFF_IN_S(act.ts_r,act.ts_c);
			time_diff+=time_s;
			/* P9: keep_exited=0 on intermediate samples */
			create_quick_diff(cs,ps,time_s,NULL,0,NULL,flush,&new_pids,&untracked,&ppid_miss,0);
			flush+=40000;
			if (params.perf)
				perf_diff_ms=monotime()-t1;
		}

		t_quick_diff=monotime();
		if (iters==0) {
			act.ts_o=act.ts_c;
			act.read_bytes_o=act.read_bytes;
			act.write_bytes_o=act.write_bytes;
			next_print=t_quick_diff+params.delay*1000;
		}
		act.ts_r=act.ts_c;

		/* P8/P16: free_stats pushes nodes to freelist; drop previous shell */
		if (ps)
			arr_free(ps);
		ps=cs;
		cs=NULL;

		if ((t_quick_diff-act.ts_c)<sampler_rate) {
			msleep(params.samplerate);
			rests++;
		}
		iters++;
	}

	free(pb.data);
	if (cs)
		arr_free(cs);
	if (ps)
		arr_free(ps);
}
