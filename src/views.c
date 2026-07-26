/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

inline void calc_total(struct xxxid_stats_arr *cs,double *read,double *write) {
	int i;

	*read=*write=0;

	for (i=0;i<cs->length;i++) {
		if (!config.f.accumulated) {
			*read+=cs->arr[i]->read_val;
			*write+=cs->arr[i]->write_val;
		} else {
			*read+=cs->arr[i]->read_val_acc;
			*write+=cs->arr[i]->write_val_acc;
		}
	}
}

#define RRV(to,from) (((to)<(from))?(~0ULL)-(to)+(from):(to)-(from))
#define RRVf(pto,pfrom,fld) RRV(pto->fld,pfrom->fld)

inline void calc_a_total(struct act_stats *act,double *read,double *write,double time_s) {
	*read=*write=0;

	if (act->have_o) {
		uint64_t r=act->read_bytes;
		uint64_t w=act->write_bytes;

		r=RRV(r,act->read_bytes_o);
		w=RRV(w,act->write_bytes_o);
		*read=(double)r/time_s;
		*write=(double)w/time_s;
	}
}

inline int value2scale(double val,double mx) {
	val=100.0*val/mx;

	if (val>75)
		return 4;
	if (val>50)
		return 3;
	if (val>25)
		return 2;
	if (val>0)
		return 1;
	return 0;
}

int create_diff(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,double time_s,filter_callback_w cb,int width,int *cnt) {
	int n=0;

	if (cnt)
		*cnt=0;
	for (n=0;cs->arr&&n<cs->length;n++) {
		struct xxxid_stats *c;
		struct xxxid_stats *p;
		double rv,wv;
		char temp[12];

		c=cs->arr[n];
		p=arr_find(ps,c->tid);

		if (!p) { // new process or task
			c->blkio_val=0;
			c->swapin_val=0;
			c->read_val=0;
			c->write_val=0;
			c->read_val_acc=0;
			c->write_val_acc=0;

			snprintf(temp,sizeof temp,"%i",c->tid);
			maxpidlen=maxpidlen<(int)strlen(temp)?(int)strlen(temp):maxpidlen;
			continue;
		}

		c->blkio_val=(double)RRVf(c,p,blkio_delay_total)/(time_s*10000000.0);
		if (c->blkio_val>100)
			c->blkio_val=100;

		c->swapin_val=(double)RRVf(c,p,swapin_delay_total)/(time_s*10000000.0);
		if (c->swapin_val>100)
			c->swapin_val=100;

		rv=(double)RRVf(c,p,read_bytes);
		wv=(double)RRVf(c,p,write_bytes);

		c->read_val=rv/time_s;
		c->write_val=wv/time_s;

		c->read_val_acc=p->read_val_acc+rv;
		c->write_val_acc=p->write_val_acc+wv;

		if (!config.f.batch_mode) {
			memcpy(c->iohist+1,p->iohist,sizeof c->iohist-sizeof *c->iohist);
			c->iohist[0]=value2scale(c->blkio_val,100.0);
		}

		snprintf(temp,sizeof temp,"%i",c->tid);
		maxpidlen=maxpidlen<(int)strlen(temp)?(int)strlen(temp):maxpidlen;
	}
	for (n=0;ps&&ps->arr&&n<ps->length;n++) {
		if (ps->arr[n]->exited||!arr_find(cs,ps->arr[n]->tid)) {
			struct xxxid_stats *p;
			struct xxxid_stats *src=ps->arr[n];

			src->exited++;
			if (src->exited>HISTORY_CNT)
				continue;
			src->blkio_val=0;
			src->swapin_val=0;
			src->read_val=0;
			src->write_val=0;
			src->read_val_acc=0;
			src->write_val_acc=0;
			p=alloc_stats();
			if (p) {
				*p=*src;
				p->threads=NULL;
				p->pool_next=NULL;
				/* Own heap copies — free_stats on ps must not double-free. */
				p->cmdline2=src->cmdline2?strdup(src->cmdline2):NULL;
				p->pw_name=src->pw_name?strdup(src->pw_name):NULL;
				if (!config.f.batch_mode) {
					memmove(p->iohist+1,p->iohist,sizeof p->iohist-sizeof *p->iohist);
					p->iohist[0]=0;
				}
				if (arr_add(cs,p))
					free_stats(p);
			}
		}
	}
	/* Reattach exited threads to their process for curses tree display. */
	if (!config.f.batch_mode) {
		for (n=0;cs->arr&&n<cs->length;n++) {
			struct xxxid_stats *c=cs->arr[n];

			if (!c||c->pid==c->tid||!c->exited)
				continue;
			{
				struct xxxid_stats *par=arr_find(cs,c->pid);

				if (!par)
					continue;
				if (!par->threads)
					par->threads=arr_alloc();
				if (!par->threads)
					continue;
				arr_add(par->threads,c);
			}
		}
	}
	for (n=0;cs->arr&&n<cs->length;n++) {
		struct xxxid_stats *c=cs->arr[n];
		if (cb&&c&&!cb(c,width))
			if (cnt)
				(*cnt)++;
	}

	return cs->length;
}

inline void humanize_val(double *value,char *str,int allow_accum) {
	static const char u[]="BKMGTPEZY";
	const size_t ulen=sizeof u-1;
	size_t p=0;

	if (!value||!str)
		return;

	if (config.f.kilobytes) {
		p=1;
		*value/=1000.0;
	} else {
		while (*value>10000) {
			if (p+1<ulen) {
				*value/=1000.0;
				p++;
			} else
				break;
		}
	}

	snprintf(str,4,"%c%s",u[p],config.f.accumulated&&allow_accum?"  ":"/s");
}

inline void humanize_valavg(double *value,char *str,int allow_accum) {
	static const char u[]="BKMGTPEZY";
	const size_t ulen=sizeof u-1;
	size_t p=0;

	if (!value||!str)
		return;

	if (config.f.kilobytes) {
		p=1;
		*value/=1000.0;
	} else {
		while (*value>10000) {
			if (p+1<ulen) {
				*value/=1000.0;
				p++;
			} else
				break;
		}
	}

	snprintf(str,4,"%c%s",u[p],config.f.accumulated&&allow_accum?"  ":"av");
}

/*
 * P7: both cs and ps are sorted by tid — linear merge instead of arr_find.
 * keep_exited==0 skips copy_old_processes (P9 intermediate samples).
 */
int create_quick_diff(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,double time_s,filter_callback_w cb,int width,int *cnt,int flush,int *new_pids,int *untracked,int *ppid_miss,int keep_exited) {
	int i=0,j=0;
	int clen,plen;

	(void)cb;
	(void)width;

	if (cnt)
		*cnt=0;
	if (!cs)
		return 0;
	if (time_s<=0.0)
		time_s=0.0001;

	clen=cs->arr?cs->length:0;
	plen=(ps&&ps->arr)?ps->length:0;

	while (i<clen&&j<plen) {
		struct xxxid_stats *c=cs->arr[i];
		struct xxxid_stats *p=ps->arr[j];

		if (!c) {
			i++;
			continue;
		}
		if (!p) {
			j++;
			continue;
		}

		if (c->tid==p->tid) {
			if (p->ac_ppid!=c->ac_ppid&&p->ac_btime!=c->ac_btime) {
				if (ppid_miss)
					(*ppid_miss)++;
				initialize_pid_values(c,flush);
			} else {
				p->samples=flush;
				p->exited=0;
				perform_delta_accounting(c,p,time_s);
			}
			i++;
			j++;
		} else if (c->tid<p->tid) {
			if (new_pids)
				(*new_pids)++;
			initialize_pid_values(c,flush);
			i++;
		} else {
			/* p only — exited; handled in copy_old_processes when keep_exited */
			j++;
		}
	}
	while (i<clen) {
		struct xxxid_stats *c=cs->arr[i++];
		if (!c)
			continue;
		if (new_pids)
			(*new_pids)++;
		initialize_pid_values(c,flush);
	}

	if (keep_exited)
		copy_old_processes(cs,ps,flush,untracked);

	return cs->length;
}

void zero_pid_values(struct xxxid_stats *p) {
	if (!p)
		return;
	p->blkio_val=0;
	p->swapin_val=0;
	p->freepages_val=0;
	p->thrashing_val=0;
	p->read_val=0;
	p->write_val=0;
	p->read_val_acc=0;
	p->write_val_acc=0;
	p->cancelled_write_bytes_val=0;
	p->cancelled_write_bytes_val_acc=0;
	p->ac_utime_val=0;
	p->ac_stime_val=0;
	p->coremem_val=0;
	p->ac_utime_val_acc=0;
	p->ac_stime_val_acc=0;
	p->ac_majflt_total=0;
	p->ac_minflt_total=0;
	p->nvcsw_delta=0;
	p->nivcsw_delta=0;
	p->time_s_acc=0;
	p->cpu_delay_total_val=0;
	p->cpu_delay_total_val_acc=0;
	p->vm_rss_kb=0;
	p->vm_swap_kb=0;
	p->private_dirty_kb=0;
	p->proc_state='?';
}

void initialize_pid_values(struct xxxid_stats *p,int first_seen) {
	if (!p)
		return;
	zero_pid_values(p);
	p->exited=0;
	p->diffs=0;
	p->samples=first_seen;
}

void perform_delta_accounting(struct xxxid_stats *c,struct xxxid_stats *p,double time_s) {
	double rv,wv,cw;
	uint64_t st,ut,mf,mif;

	if (!c||!p)
		return;

	c->blkio_val=(double)RRVf(c,p,blkio_delay_total)/(time_s*10000000.0);
	if (c->blkio_val>100)
		c->blkio_val=100;

	c->swapin_val=(double)RRVf(c,p,swapin_delay_total)/(time_s*10000000.0);
	if (c->swapin_val>100)
		c->swapin_val=100;

	c->freepages_val=(double)RRVf(c,p,freepages_delay_total)/(time_s*10000000.0);
	if (c->freepages_val>100)
		c->freepages_val=100;

	c->thrashing_val=(double)RRVf(c,p,thrashing_delay_total)/(time_s*10000000.0);
	if (c->thrashing_val>100)
		c->thrashing_val=100;

	c->time_s_acc=p->time_s_acc+time_s;
	rv=(double)RRVf(c,p,read_bytes);
	wv=(double)RRVf(c,p,write_bytes);
	cw=(double)RRVf(c,p,cancelled_write_bytes);
	st=RRVf(c,p,ac_stime);
	ut=RRVf(c,p,ac_utime);
	mf=RRVf(c,p,ac_majflt);
	mif=RRVf(c,p,ac_minflt);

	c->ac_utime_val_acc+=ut;
	c->ac_stime_val_acc+=st;
	c->cpu_delay_total_val_acc+=c->cpu_delay_total-p->cpu_delay_total;

	c->read_val=rv/time_s;
	c->write_val=wv/time_s;
	c->ac_stime_val=c->ac_stime_val_acc/(c->time_s_acc*10000);
	c->ac_utime_val=c->ac_utime_val_acc/(c->time_s_acc*10000);
	c->cpu_delay_total_val=c->cpu_delay_total_val_acc/(c->time_s_acc*10000000);
	c->cancelled_write_bytes_val=cw/time_s;
	c->coremem_val=(double)(c->hiwater_rss*1000);

	c->read_val_acc=p->read_val_acc+rv;
	c->write_val_acc=p->write_val_acc+wv;
	c->cancelled_write_bytes_val_acc=p->cancelled_write_bytes_val_acc+cw;
	c->ac_majflt_total+=mf;
	c->ac_minflt_total+=mif;
	c->nvcsw_delta=RRVf(c,p,nvcsw);
	c->nivcsw_delta=RRVf(c,p,nivcsw);

	/* P4: skip graph history on batch path */
	if (!config.f.batch_mode) {
		memcpy(c->iohist+1,p->iohist,sizeof c->iohist-sizeof *c->iohist);
		c->iohist[0]=value2scale(c->blkio_val,100.0);
	}
	c->diffs++;
}

inline void copy_old_processes(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,int flush,int *untracked) {
	int n;

	if (!cs||!ps||!ps->arr)
		return;

	for (n=0;n<ps->length;n++) {
		struct xxxid_stats *old=ps->arr[n];
		struct xxxid_stats *p;

		if (!old)
			continue;
		if (old->samples==flush)
			continue;
		if (!old->diffs) {
			if (untracked)
				(*untracked)++;
			continue;
		}
		old->exited++;
		p=alloc_stats();
		if (!p)
			continue;
		*p=*old;
		p->threads=NULL;
		p->pool_next=NULL;
		p->cmdline2=old->cmdline2?strdup(old->cmdline2):NULL;
		p->pw_name=old->pw_name?strdup(old->pw_name):NULL;
		if (!config.f.batch_mode) {
			memmove(p->iohist+1,p->iohist,sizeof p->iohist-sizeof *p->iohist);
			p->iohist[0]=0;
		}
		/* Disambiguate recycled tid slots for sort uniqueness */
		if (p->tid)
			p->tid+=p->samples;
		if (arr_add(cs,p))
			free_stats(p);
	}
}

inline int iotop_sort_cb(const void *a,const void *b) {
	int order=config.f.sort_order?1:-1; // SORT_ASC is bit 0=1, else should reverse sort
	struct xxxid_stats **ppa=(struct xxxid_stats **)a;
	struct xxxid_stats **ppb=(struct xxxid_stats **)b;
	struct xxxid_stats *pa,*pb;
	int type=config.f.sort_by;
	static int grlen=0;
	int res=0;

	/* Setup call: arr_sort may pass a=NULL to configure graph history length. */
	if (!a) {
		grlen=(long)b;
		return 0;
	}

	pa=*ppa;
	pb=*ppb;

	switch (type) {
		case SORT_BY_GRAPH: {
			int aa=0,ab=0;
			int i;

			if (grlen==0)
				grlen=HISTORY_CNT;
			for (i=0;i<grlen;i++) {
				aa+=pa->iohist[i];
				ab+=pb->iohist[i];
			}
			res=aa-ab;
			break;
		}
		case SORT_BY_PRIO:
			res=pa->io_prio-pb->io_prio;
			break;
		case SORT_BY_COMMAND:
			if (config.f.fullcmdline&&pa->cmdline2&&pb->cmdline2)
				res=strcmp(pa->cmdline2,pb->cmdline2);
			else
				res=strcmp(pa->cmdline1,pb->cmdline1);
			break;
		case SORT_BY_PID:
			res=pa->tid-pb->tid;
			break;
		case SORT_BY_USER:
			if (pa->pw_name&&pb->pw_name)
				res=strcmp(pa->pw_name,pb->pw_name);
			else
				res=pa->euid-pb->euid;
			break;
		case SORT_BY_READ:
			if (config.f.accumulated)
				res=pa->read_val_acc>pb->read_val_acc?1:pa->read_val_acc<pb->read_val_acc?-1:0;
			else
				res=pa->read_val>pb->read_val?1:pa->read_val<pb->read_val?-1:0;
			break;
		case SORT_BY_WRITE:
			if (config.f.accumulated)
				res=pa->write_val_acc>pb->write_val_acc?1:pa->write_val_acc<pb->write_val_acc?-1:0;
			else
				res=pa->write_val>pb->write_val?1:pa->write_val<pb->write_val?-1:0;
			break;
		case SORT_BY_SWAPIN:
			res=pa->swapin_val>pb->swapin_val?1:pa->swapin_val<pb->swapin_val?-1:0;
			break;
		case SORT_BY_IO:
			res=pa->blkio_val>pb->blkio_val?1:pa->blkio_val<pb->blkio_val?-1:0;
			break;
	}
	res*=order;
	return res;
}

inline int filter1(struct xxxid_stats *s) {
	if ((params.user_id!=-1)&&(s->euid!=params.user_id))
		return 1;

	if ((params.pid!=-1)&&(s->tid!=params.pid))
		return 1;

	return 0;
}
