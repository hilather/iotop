/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#ifndef __IOTOP_H__
#define __IOTOP_H__

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <sys/types.h>
#include <stdint.h>

/* Product identity for this fork (distinct from upstream iotop-c). */
#define PRODUCT_NAME "iotop-perf"
#define VERSION "1.18.0"
#define VERSION_EXTRA "hilather/perf"

/*
 * Kernel taskstats ABI (see Tomas-M/iotop 1.29+).
 * Layout of struct taskstats is versioned; v15 broke field offsets vs v14.
 * We parse via vendored taskstats-v14.h / taskstats-v15.h, not build-host
 * kernel headers for field access.
 */
#define IOTOP_TASKSTATS_MINVER 4
#define IOTOP_TASKSTATS_VERSION 15

/* taskstats ac_comm[] length (TS_COMM_LEN); fixed storage avoids heap (P1). */
#define IOTOP_COMM_LEN 32

extern unsigned taskstats_ver; /* first-seen kernel taskstats version (0=none yet) */

typedef union {
	struct _flags {
		int batch_mode;
		int only;
		int processes;
		int accumulated;
		int kilobytes;
		int timestamp;
		int quiet;
		int fullcmdline;
		int hidepid;
		int hideprio;
		int hideuser;
		int hideread;
		int hidewrite;
		int hideswapin;
		int hideio;
		int hidegraph;
		int hidecmd;
		int sort_by;
		int sort_order;
		int deadx;
	} f;
	int opts[19];
} config_t;

typedef struct {
	int iter;
	int delay;
	int pid;
	long samplerate;
	int user_id;
	/* Performance knobs (Tier 1/2) */
	int walk_threads; /* 1 = scan /proc/pid/task and fold (slow); 0 = process-only */
	int use_tgid;     /* 1 = TASKSTATS_CMD_ATTR_TGID for process leaders */
	int top_n;        /* 0 = print/sort all; >0 = top N by current sort key */
	int perf;         /* 1 = emit PERF timing lines on stderr */
	/*
	 * Print-time enrichment (Phase R1) — never on fetch path.
	 * batch_enrich=1: resolve USER (getpwuid cache) + PRIO (ioprio_get) for
	 * rows about to be printed. fullcmdline (-c) loads /proc/pid/cmdline lazily.
	 */
	int batch_enrich;
	/* Optional print-time dirty via smaps_rollup (-D). Off by default. */
	int track_dirty;
} params_t;

extern config_t config;
extern params_t params;
extern int maxpidlen;
/* 1 if /proc/pid/smaps_rollup exists (kernel >= 4.14 / Rocky 8+). */
extern int have_smaps_rollup;

/* Optional cycle counters when params.perf is set */
extern uint64_t perf_fetch_ms;
extern uint64_t perf_diff_ms;
extern uint64_t perf_print_ms;
extern uint64_t perf_n_netlink;
extern uint64_t perf_n_proc;
extern uint64_t perf_n_getpwuid;
extern uint64_t perf_n_cmdline;
extern uint64_t perf_n_ioprio;
extern uint64_t perf_n_status;
extern uint64_t perf_n_smaps;

#define HISTORY_POS 60
#define HISTORY_CNT (HISTORY_POS*2)

struct xxxid_stats;

struct xxxid_stats_arr {
	struct xxxid_stats **arr;
	struct xxxid_stats **sor;
	/* Open-addressed tid hash for O(1) arr_find (P17); rebuilt after sample. */
	struct xxxid_stats **hash;
	int hash_mask; /* size-1, size power of two */
	int length;
	int size;
};

/*
 * Hot sample object: raw counters + derived rates used by batch.
 * Intentionally no heap string pointers (P1/P2), no thread list (folded),
 * no freepages/cpu_run_* (P19 — not product-critical for this fork).
 */
struct xxxid_stats {
	pid_t pid;
	pid_t tid;
	pid_t ac_ppid;

	/* ---- raw counters from taskstats ---- */
	uint64_t swapin_delay_total; /* ns */
	uint64_t blkio_delay_total;  /* ns */
	uint64_t freepages_delay_total;
	uint64_t thrashing_delay_total;
	uint64_t read_bytes;
	uint64_t write_bytes;
	uint64_t cancelled_write_bytes;
	uint64_t ac_utime; /* usec */
	uint64_t ac_stime;
	uint64_t ac_majflt;
	uint64_t ac_minflt;
	uint64_t nvcsw;  /* voluntary context switches */
	uint64_t nivcsw; /* involuntary */
	uint64_t cpu_delay_total;
	uint64_t ac_btime;
	uint64_t hiwater_rss; /* KB peak */

	/* ---- derived (filled on delta / print) ---- */
	double blkio_val;
	double swapin_val;
	double freepages_val;
	double thrashing_val;
	double read_val;
	double write_val;
	double read_val_acc;
	double write_val_acc;
	double cancelled_write_bytes_val;
	double cancelled_write_bytes_val_acc;
	double cpu_delay_total_val;
	double cpu_delay_total_val_acc;
	double ac_utime_val;
	double ac_stime_val;
	uint64_t ac_utime_val_acc;
	uint64_t ac_stime_val_acc;
	uint64_t ac_majflt_total;
	uint64_t ac_minflt_total;
	uint64_t nvcsw_delta;
	uint64_t nivcsw_delta;
	double time_s_acc;
	double coremem_val; /* hiwater_rss scaled for display */

	/* Print-time memory snapshot (/proc/pid/status + optional smaps_rollup) */
	uint64_t vm_rss_kb;
	uint64_t vm_swap_kb;
	uint64_t private_dirty_kb;
	char proc_state; /* R/S/D/Z/... from /proc/pid/stat */

	int euid;
	int io_prio; /* kept 0 on hot path; curses expects the field */

	/* Fixed command name from taskstats.ac_comm — no malloc (P1). */
	char cmdline1[IOTOP_COMM_LEN];

	int diffs;
	int samples;
	int exited;

	/*
	 * Graph history for curses. Batch skips updating these (P4) when
	 * config.f.batch_mode is set.
	 */
	uint8_t iohist[HISTORY_CNT];

	/* Unused in perf fetch (threads folded); kept NULL for curses compat. */
	struct xxxid_stats_arr *threads;

	/* Freelist link (P8) — not a public API field. */
	struct xxxid_stats *pool_next;
};

#define PROC_LIST_SZ_INI 16
#define PROC_LIST_SZ_INC 2048

struct act_stats {
	uint64_t read_bytes;
	uint64_t write_bytes;
	uint64_t read_bytes_o;
	uint64_t write_bytes_o;
	uint64_t cancelled_write_bytes;
	uint64_t cancelled_write_bytes_o;

	uint64_t ac_utime;
	uint64_t ac_stime;
	uint64_t ac_majflt;
	uint64_t coremem;

	uint64_t ac_utime_o;
	uint64_t ac_stime_o;
	uint64_t ac_majflt_o;
	uint64_t coremem_o;

	uint64_t ts_c;
	uint64_t ts_o;
	uint64_t ts_r;
	uint8_t have_o;
};

inline void nl_init(void);
inline void nl_fini(void);
void warn_taskstats_version(void);

inline int nl_xxxid_info(pid_t tid,pid_t pid,struct xxxid_stats *stats);

typedef int (*filter_callback)(struct xxxid_stats *);
typedef int (*filter_callback_w)(struct xxxid_stats *,int width);

inline struct xxxid_stats_arr *fetch_data(filter_callback);
inline struct xxxid_stats_arr *fetch_batch_data(struct xxxid_stats **p);
inline void free_stats(struct xxxid_stats *s);
struct xxxid_stats *alloc_stats(void);
void stats_pool_clear(void);

typedef void (*view_loop)(void);
typedef void (*view_init)(void);
typedef void (*view_fini)(void);

inline void view_batch_loop(void);
inline void view_batch_init(void);
inline void view_batch_fini(void);

inline void view_curses_loop(void);
inline void view_curses_init(void);
inline void view_curses_fini(void);

inline unsigned int curses_sleep(unsigned int seconds);

/* utils.c */

void find_cmd_and_ppid(int pid,struct xxxid_stats *s);

inline int64_t monotime(void);
inline char *u8strpadt(const char *s,ssize_t len);
inline char *esc_low_ascii(char *p);

typedef void (*pg_cb)(pid_t pid,pid_t tid,void *hint1,void *hint2,void *p);
inline void pidgen_cb(pg_cb cb,void *hint1,void *hint2,void *p);

inline int is_a_file(const char *p);
inline int is_a_dir(const char *p);
inline int is_a_process(pid_t tid);

/* Format accumulated usec-like counters as HH:MM:SS without localtime (P10). */
void format_duration(uint64_t units,char *buf,size_t buflen);

/* batch_enrich.c — print-time only */
const char *batch_uid_name(uid_t uid);
int batch_read_comm(pid_t pid,char *buf,size_t buflen);
int batch_read_cmdline(pid_t pid,char *buf,size_t buflen);
int batch_resolve_ioprio(pid_t tid);
void batch_enrich_reset_perf(void);

/* batch_mem.c — print-time status / optional dirty */
void batch_mem_probe(void);
/* Fills vm_rss_kb, vm_swap_kb, proc_state; optionally private_dirty_kb if track_dirty. */
int batch_read_mem(struct xxxid_stats *s,int want_dirty);

/* delayacct.c */
inline int has_task_delayacct(void);
inline int read_task_delayacct(void);
void warn_task_delayacct(void);

/* ioprio.c */

enum {
	IOPRIO_CLASS_NONE,
	IOPRIO_CLASS_RT,
	IOPRIO_CLASS_BE,
	IOPRIO_CLASS_IDLE,
	IOPRIO_CLASS_MAX,
	IOPRIO_CLASS_MIN=IOPRIO_CLASS_RT,
};

enum {
	IOPRIO_WHO_PROCESS=1,
	IOPRIO_WHO_PGRP,
	IOPRIO_WHO_USER
};

enum {
	SORT_BY_PID,
	SORT_BY_PRIO,
	SORT_BY_USER,
	SORT_BY_READ,
	SORT_BY_WRITE,
	SORT_BY_SWAPIN,
	SORT_BY_IO,
	SORT_BY_GRAPH,
	SORT_BY_COMMAND,
	SORT_BY_MAX
};

enum {
	SORT_DESC,
	SORT_ASC
};

extern const char *str_ioprio_class[];

inline int get_ioprio(pid_t pid);
inline const char *str_ioprio(int io_prio);
inline int ioprio_value(int class,int prio);
inline int set_ioprio(int which,int who,int ioprio_class,int ioprio_prio);
inline int ioprio2class(int ioprio);
inline int ioprio2prio(int ioprio);

/* vmstat.c */

inline int get_vm_counters(uint64_t *pgpgin,uint64_t *pgpgou);

/* checks.c */

inline int system_checks(void);

/* arr.c */

inline struct xxxid_stats_arr *arr_alloc(void);
inline int arr_add(struct xxxid_stats_arr *a,struct xxxid_stats *s);
inline struct xxxid_stats *arr_find(struct xxxid_stats_arr *pa,pid_t tid);
inline void arr_free(struct xxxid_stats_arr *pa);
inline void arr_free_noitem(struct xxxid_stats_arr *pa);
/* P16: release items to freelist, keep capacity for next sample. */
void arr_recycle(struct xxxid_stats_arr *pa);
/* P17: rebuild open-addressed tid hash after a full sample is built. */
void arr_hash_build(struct xxxid_stats_arr *pa);
inline void arr_sort(struct xxxid_stats_arr *pa,int (*cb)(const void *a,const void *b));
/* P18: sort and keep at most top_n entries in sor (0 = all). */
void arr_sort_top(struct xxxid_stats_arr *pa,int (*cb)(const void *a,const void *b),int top_n);

#define HEADER1_FORMAT "  Total DISK READ: %7.2f %s%s |   Total DISK WRITE: %7.2f %s%s"
#define HEADER2_FORMAT "Current DISK READ: %7.2f %s%s | Current DISK WRITE: %7.2f %s%s"

inline void calc_total(struct xxxid_stats_arr *cs,double *read,double *write);
inline void calc_a_total(struct act_stats *act,double *read,double *write,double time_s);
inline void humanize_val(double *value,char *str,int allow_accum);
inline int iotop_sort_cb(const void *a,const void *b);
/* keep_exited: when 0, skip copy_old_processes (P9 intermediate samples). */
int create_quick_diff(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,double time_s,filter_callback_w cb,int width,int *cnt,int flush,int *new_pids,int *untracked,int *ppid_miss,int keep_exited);
void copy_old_processes(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,int flush,int *untracked);
void perform_delta_accounting(struct xxxid_stats *c,struct xxxid_stats *p,double time_s);
void initialize_pid_values(struct xxxid_stats *p,int first_seen);
void zero_pid_values(struct xxxid_stats *p);
inline int create_diff(struct xxxid_stats_arr *cs,struct xxxid_stats_arr *ps,double time_s,filter_callback_w cb,int width,int *cnt);
inline void reset_pid(struct xxxid_stats *cs);
inline int value2scale(double val,double mx);
inline int filter1(struct xxxid_stats *s);

#ifndef KEY_CTRL_L
#define KEY_CTRL_L 014
#endif

#endif /* __IOTOP_H__ */
