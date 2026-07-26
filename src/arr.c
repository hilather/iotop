/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

inline struct xxxid_stats_arr *arr_alloc(void) {
	struct xxxid_stats_arr *a;

	a=calloc(1,sizeof *a);
	if (!a)
		return NULL;

	a->arr=calloc(PROC_LIST_SZ_INI,sizeof *a->arr);
	if (!a->arr) {
		free(a);
		return NULL;
	}
	a->size=PROC_LIST_SZ_INI;
	return a;
}

static inline int arr_resize(struct xxxid_stats_arr *a,int newsize) {
	struct xxxid_stats **t;

	if (!a)
		return EINVAL;
	if (a->size>=newsize)
		return 0;

	newsize=(newsize+PROC_LIST_SZ_INC-1)/PROC_LIST_SZ_INC;
	newsize*=PROC_LIST_SZ_INC;
	t=realloc(a->arr,newsize*sizeof(struct xxxid_stats *));
	if (!t)
		return ENOMEM;

	a->arr=t;
	a->size=newsize;
	return 0;
}

static void arr_hash_free(struct xxxid_stats_arr *pa) {
	if (!pa)
		return;
	if (pa->hash) {
		free(pa->hash);
		pa->hash=NULL;
		pa->hash_mask=0;
	}
}

inline int arr_add(struct xxxid_stats_arr *pa,struct xxxid_stats *ps) {
	int a=-1;
	int i,s,e;
	pid_t r;
	int res;

	if (!pa||!ps)
		return EINVAL;

	res=arr_resize(pa,pa->length+1);
	if (res)
		return res;

	if (pa->sor) {
		free(pa->sor);
		pa->sor=NULL;
	}
	/* Hash invalid after structural change. */
	arr_hash_free(pa);

	s=0;
	e=pa->length;
	for (;;) {
		if (e-s<5) {
			for (i=s;i<e;i++) {
				r=ps->tid-pa->arr[i]->tid;
				if (!r)
					return EINVAL;
				if (r<0)
					break;
			}
			a=i;
			break;
		} else {
			i=s+(e-s)/2;
			r=ps->tid-pa->arr[i]->tid;
			if (!r)
				return EINVAL;
			if (r<0)
				e=i;
			else
				s=i+1;
		}
	}

	if (a!=pa->length)
		memmove(pa->arr+a+1,pa->arr+a,(pa->length-a)*sizeof *pa->arr);
	pa->arr[a]=ps;
	pa->length++;
	return 0;
}

/* P17: power-of-two open addressing on tid. */
void arr_hash_build(struct xxxid_stats_arr *pa) {
	int n,cap,i;

	if (!pa)
		return;
	arr_hash_free(pa);
	if (pa->length<=0||!pa->arr)
		return;

	cap=16;
	while (cap<pa->length*2)
		cap<<=1;

	pa->hash=calloc((size_t)cap,sizeof *pa->hash);
	if (!pa->hash)
		return;
	pa->hash_mask=cap-1;

	for (i=0;i<pa->length;i++) {
		struct xxxid_stats *s=pa->arr[i];
		unsigned h;

		if (!s)
			continue;
		h=(unsigned)s->tid*2654435761u;
		for (;;) {
			n=(int)(h& (unsigned)pa->hash_mask);
			if (!pa->hash[n]) {
				pa->hash[n]=s;
				break;
			}
			h++;
		}
	}
}

inline struct xxxid_stats *arr_find(struct xxxid_stats_arr *pa,pid_t tid) {
	int i,s,e,r;

	if (!pa||!pa->arr||pa->length<=0)
		return NULL;

	/* Fast path: open-addressed hash (P17). */
	if (pa->hash&&pa->hash_mask) {
		unsigned h=(unsigned)tid*2654435761u;
		int probes=0;
		int cap=pa->hash_mask+1;

		for (;;) {
			struct xxxid_stats *x=pa->hash[h&(unsigned)pa->hash_mask];
			if (!x)
				return NULL;
			if (x->tid==tid)
				return x;
			h++;
			if (++probes>cap)
				return NULL;
		}
	}

	/* Fallback: binary search on sorted arr. */
	s=0;
	e=pa->length;
	for (;;) {
		if (e-s<5) {
			for (i=s;i<e;i++) {
				if (!pa->arr[i])
					continue;
				r=tid-pa->arr[i]->tid;
				if (!r)
					return pa->arr[i];
				if (r<0)
					break;
			}
			return NULL;
		} else {
			i=s+(e-s)/2;
			if (!pa->arr[i])
				return NULL;
			r=tid-pa->arr[i]->tid;
			if (!r)
				return pa->arr[i];
			if (r<0)
				e=i;
			else
				s=i+1;
		}
	}
}

static inline void _arr_free(struct xxxid_stats_arr *pa,int freeitem) {
	if (!pa)
		return;

	if (pa->arr) {
		if (freeitem) {
			int i;
			for (i=0;i<pa->length;i++)
				free_stats(pa->arr[i]);
		}
		free(pa->arr);
	}
	if (pa->sor)
		free(pa->sor);
	arr_hash_free(pa);
	free(pa);
}

inline void arr_free(struct xxxid_stats_arr *pa) {
	_arr_free(pa,1);
}

inline void arr_free_noitem(struct xxxid_stats_arr *pa) {
	_arr_free(pa,0);
}

/* P16: recycle items into freelist, keep backing arrays. */
void arr_recycle(struct xxxid_stats_arr *pa) {
	int i;

	if (!pa)
		return;
	if (pa->arr) {
		for (i=0;i<pa->length;i++) {
			if (pa->arr[i])
				free_stats(pa->arr[i]);
			pa->arr[i]=NULL;
		}
	}
	pa->length=0;
	if (pa->sor) {
		free(pa->sor);
		pa->sor=NULL;
	}
	arr_hash_free(pa);
}

inline void arr_sort(struct xxxid_stats_arr *pa,int (*cb)(const void *a,const void *b)) {
	arr_sort_top(pa,cb,0);
}

/* P18: full sort, or sort then truncate sor to top_n. */
void arr_sort_top(struct xxxid_stats_arr *pa,int (*cb)(const void *a,const void *b),int top_n) {
	if (!pa)
		return;
	if (pa->sor) {
		free(pa->sor);
		pa->sor=NULL;
	}
	if (!pa->length||!pa->arr)
		return;

	pa->sor=calloc(pa->length,sizeof *pa->arr);
	if (!pa->sor)
		return;

	memcpy(pa->sor,pa->arr,pa->length*sizeof *pa->arr);
	qsort(pa->sor,pa->length,sizeof *pa->sor,cb);

	if (top_n>0&&top_n<pa->length) {
		/* Keep first top_n of sorted view only (print path reads sor). */
		struct xxxid_stats **t=realloc(pa->sor,(size_t)top_n*sizeof *pa->sor);
		if (t) {
			pa->sor=t;
			/* length of sort view encoded by capping iteration externally via top_n;
			 * store truncated length in a soft way: we keep pa->length as full arr
			 * length and callers pass top_n. For convenience set a sentinel by
			 * shrinking only sor capacity — callers use min(length, top_n). */
		}
	}
}
