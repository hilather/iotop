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
#include <unistd.h>
#include <errno.h>

#define TIMEDIFF_IN_S(sta,end) ((((sta)==(end))||(sta)==0)?0.0001:(((end)-(sta))/1000.0))

static inline void view_batch(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,struct act_stats *act, int diff_len, double time_s) {
	double total_a_read,total_a_write;
	char str_a_read[4],str_a_write[4];
	double total_read,total_write;
	char str_read[4],str_write[4];
	int i;

	(void)ps;

	if (!cs)
		return;

	calc_total(cs,&total_read,&total_write);
	calc_a_total(act,&total_a_read,&total_a_write,time_s);

	humanize_val(&total_read,str_read,1);
	humanize_val(&total_write,str_write,1);
	humanize_val(&total_a_read,str_a_read,0);
	humanize_val(&total_a_write,str_a_write,0);

	printf(HEADER1_FORMAT,total_read,str_read,"",total_write,str_write,"");

	if (config.f.timestamp) {
		time_t t=time(NULL);

		printf(" | %s",ctime(&t));
	} else
		printf("\n");

	printf(HEADER2_FORMAT,total_a_read,str_a_read,"",total_a_write,str_a_write,"");

	printf("\n");

	if (!config.f.quiet)
		printf("%6s %6s %6s %4s %8s %11s %11s %11s %11s %5s %8s %8s %8s %8s %8s %s\n","PID","TID","PPID","PRIO","USER","DISK READ","DISK WRITE","CANCELLEDW","RSS","MJRFLT","SWAPIN","IO","CDELAY","UTIME","STIME","COMMAND");
	arr_sort(cs,iotop_sort_cb);

	if (diff_len<0)
		diff_len=0;
	if (cs->length<diff_len)
		diff_len=cs->length;

	for (i=0;cs->sor&&i<diff_len;i++) {
		struct xxxid_stats *s=cs->sor[i];
		double read_val,write_val,canceled_val,coremem_val;
		uint64_t mf_val;
		char read_str[4],write_str[4],canceled_str[4],coremem_str[4];
		char *pw_name;
		char utime[16],stime[16];
		struct tm *ptm;

		if (!s)
			continue;

		read_val=config.f.accumulated?s->read_val_acc:s->read_val;
		write_val=config.f.accumulated?s->write_val_acc:s->write_val;
		canceled_val=config.f.accumulated?s->cancelled_write_bytes_val_acc:s->cancelled_write_bytes_val;
		coremem_val=s->coremem_val;
		mf_val=config.f.accumulated?s->ac_majflt_total:s->ac_majflt;

		/* show only processes, if configured */
		if (config.f.processes&&s->pid!=s->tid)
			continue;
		if (config.f.only&&!read_val&&!write_val&&!s->ac_utime_val_acc&&!s->ac_stime_val_acc&&!s->cpu_run_virtual_total_val_acc&&!s->cpu_run_real_total_val_acc)
			continue;
		if (params.samplerate==1000&&s->exited) /* do not show exited processes in batch view */
			continue;

		humanize_val(&read_val,read_str,1);
		humanize_val(&write_val,write_str,1);
		humanize_val(&canceled_val,canceled_str,1);
		humanize_val(&coremem_val,coremem_str,1);

		pw_name=u8strpadt(s->pw_name,10);

		/* Never dereference a NULL tm* (GetTimeAndDate is always valid now, but keep guard). */
		ptm=GetTimeAndDate(s->ac_utime_val_acc);
		if (ptm)
			snprintf(utime,sizeof utime,"%02d:%02d:%02d",ptm->tm_hour,ptm->tm_min,ptm->tm_sec);
		else
			snprintf(utime,sizeof utime,"??:??:??");

		ptm=GetTimeAndDate(s->ac_stime_val_acc);
		if (ptm)
			snprintf(stime,sizeof stime,"%02d:%02d:%02d",ptm->tm_hour,ptm->tm_min,ptm->tm_sec);
		else
			snprintf(stime,sizeof stime,"??:??:??");

		printf("%6i %6i %6i %4s %s %7.2f %-3.3s %7.2f %-3.3s %7.2f %-3.3s %7.2f %-3.3s %4llu %6.2f %% %6.2f %% %6.2f %% %8s %8s %s\n",
			s->pid,s->tid,(int)s->ac_ppid,str_ioprio(s->io_prio),pw_name?pw_name:"(null)",
			read_val,read_str,write_val,write_str,
			canceled_val,canceled_str,coremem_val,coremem_str,
			(unsigned long long)mf_val,s->swapin_val,s->blkio_val,s->cpu_delay_total_val,
			utime,stime,s->cmdline1?s->cmdline1:"(unknown)");

		reset_pid(s);
		if (pw_name)
			free(pw_name);
	}
}

/*
 * Format an accumulated time counter as HH:MM:SS via localtime().
 *
 * Historical bug: this returned NULL whenever (ms % 1000) != 0, and callers
 * in view_batch() dereferenced the result → occasional SIGSEGV on Rocky 8
 * (and everywhere else) once any process had non-zero utime/stime that was
 * not an exact multiple of 1000.
 *
 * taskstats ac_utime/ac_stime are in microseconds; after delta accumulation
 * the value is almost never divisible by 1000, so the crash looked
 * "occasional" (only on print paths for processes with CPU time).
 */
struct tm* GetTimeAndDate(unsigned long long milliseconds)
{
	time_t seconds = (time_t)(milliseconds / 1000ULL);
	return localtime(&seconds);
}

inline void view_batch_init(void) {
}

inline void view_batch_fini(void) {
}

int msleep(long msec)
{
    struct timespec ts;
    int res;

    if (msec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

inline void reset_pid(struct xxxid_stats *cs){
	zero_pid_values(cs);
}

void view_batch_loop(void) {
	struct xxxid_stats_arr *ps=arr_alloc();
	struct xxxid_stats_arr *cs=NULL;
	struct act_stats act={0};
	int iters=0;
	int rests=0;
	int flush=0;
	double time_s;
	double time_diff=0;
	uint64_t sampler_rate=params.samplerate;
	uint64_t t_quick_diff=0;
	uint64_t next_print=0;
	/* Cache of last main-process pointer for pid_cb; not an owned stats object. */
	struct xxxid_stats *p=NULL;
	int diff_len=0;
	int new_pids=0;
	int untracked=0;
	int ppid_miss=0;

	if (!ps) {
		fprintf(stderr,"view_batch_loop: out of memory\n");
		return;
	}

	for (;;) {
		/* Reset cache each sample; pointers into the previous array are invalid after arr_free. */
		p=NULL;
		cs=fetch_batch_data(&p);
		if (!cs) {
			fprintf(stderr,"view_batch_loop: fetch_batch_data failed\n");
			msleep(params.samplerate>0?params.samplerate:100);
			continue;
		}

		act.ts_c=monotime();
		if (!next_print)
			next_print=act.ts_c+params.delay*1000;

		if (act.ts_c>=next_print) {
			time_s=TIMEDIFF_IN_S(act.ts_r,act.ts_c);
			time_diff+=time_s;
			diff_len=create_quick_diff(cs,ps,time_s,NULL,0,NULL,1,&new_pids,&untracked,&ppid_miss);
			get_vm_counters(&act.read_bytes,&act.write_bytes);
			printf("Samples=%i, Rests=%i, NewPIDs=%i, Miss=%i, PPIDMiss=%i, ArrLength=%i, Size=%i, Time Taken: %2.1f sec\n",
				iters,rests,new_pids,ppid_miss,untracked,
				(ps&&ps->arr)?ps->length:0,
				(ps&&ps->arr)?ps->size:0,
				time_diff);

			view_batch(cs,ps,&act,diff_len,time_diff);

			/*
			 * Critical for log shippers / pipes: deliver a full sample block
			 * before sleeping (Tomas-M 1.30+). Also keeps stderr warnings from
			 * interleaving mid-line with unflushed stdout.
			 */
			fflush(stdout);

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
			create_quick_diff(cs,ps,time_s,NULL,0,NULL,flush,&new_pids,&untracked,&ppid_miss);
			flush+=40000;
		}

		t_quick_diff=monotime();
		if (iters==0) {
			act.ts_o=act.ts_c;
			act.read_bytes_o=act.read_bytes;
			act.write_bytes_o=act.write_bytes;
			next_print=t_quick_diff+params.delay*1000;
		}
		act.ts_r=act.ts_c;

		if (ps)
			arr_free(ps);
		ps=cs;
		cs=NULL;

		if ((t_quick_diff-act.ts_c)<sampler_rate) {
			/* only winners rest */
			msleep(params.samplerate);
			rests++;
		}
		iters++;
	}
	if (cs)
		arr_free(cs);
	if (ps)
		arr_free(ps);
}

