/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020,2021  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <pwd.h>
#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static const char *progname=NULL;
/* Set when user passes -T/--threads (batch: walk+fold; interactive already walks). */
static int opt_threads=0;
int maxpidlen=5;
/* First-seen kernel taskstats version; 0 until the first successful sample. */
unsigned taskstats_ver=0;

config_t config;
params_t params;

view_init v_init_cb=view_curses_init;
view_fini v_fini_cb=view_curses_fini;
view_loop v_loop_cb=view_curses_loop;

inline void init_params(void) {
	params.iter=-1;
	params.delay=1;
	params.samplerate=1000;
	params.pid=-1;
	params.user_id=-1;
	/* Sampling policy filled by apply_mode_defaults() after flags parse. */
	params.walk_threads=0;
	params.use_tgid=1;
	params.top_n=0;
	params.perf=0;
	/* Print-time USER/PRIO enrichment on by default for batch UX. */
	params.batch_enrich=1;
	params.track_dirty=0; /* -D: smaps_rollup Private_Dirty at print */
}

static const char str_opt[]="boPaktqc123456789x";

static inline void print_version(void) {
	printf("%s %s (%s)\n", PRODUCT_NAME, VERSION, VERSION_EXTRA);
}

static inline void print_help(void) {
	printf(
		"Usage: %s [OPTIONS]\n\n"
		"%s %s — performance-oriented fork of iotop-c (hilather/iotop).\n"
		"Not the distro/upstream iotop; binary name is %s.\n\n"
		"DISK READ and DISK WRITE are the block I/O bandwidth used during the sampling\n"
		"period. SWAPIN and IO are the percentages of time the thread spent respectively\n"
		"while swapping in and waiting on I/O more generally. PRIO is the I/O priority\n"
		"at which the thread is running (set using the ionice command).\n\n"
		"Options:\n",
		progname, PRODUCT_NAME, VERSION, PRODUCT_NAME
	);
	printf(

		"  -v, --version         show program's version number and exit\n"
		"  -h, --help            show this help message and exit\n"
		"  -o, --only            only show processes or threads actually doing I/O\n"
		"  -b, --batch           non-interactive mode\n"
		"  -n NUM, --iter=NUM    number of iterations before ending [infinite]\n"
		"  -d SEC, --delay=SEC   delay between iterations [1 second]\n"
		"  -s MS,                sampling rate to look for pids [1000 ms]\n"
		"  -p PID, --pid=PID     processes/threads to monitor [all]\n"
		"  -u USER, --user=USER  users to monitor [all]\n"
		"  -P, --processes       only show processes, not all threads\n"
		"  -T, --threads         batch: walk all threads and fold (slower; default process-only)\n"
		"  -N NUM, --top=NUM     show only top NUM rows after sort (0=all)\n"
		"  -f, --perf            emit PERF timing lines on stderr\n"
		"  -E, --no-enrich       skip print-time USER/PRIO lookup (max batch speed)\n"
		"  -D, --dirty           print Private_Dirty from smaps_rollup (Rocky 8+; costly)\n"
		"  -c, --fullcmdline     show full command line (batch: print-time; interactive: live)\n"
		"  -a, --accumulated     show accumulated I/O instead of bandwidth\n"
		"  -k, --kilobytes       use kilobytes instead of a human friendly unit\n"
		"  -t, --time            add a timestamp on each line (implies --batch)\n"
		"  -1, --hide-pid        hide PID/TID column\n"
		"  -2, --hide-prio       hide PRIO column\n"
		"  -3, --hide-user       hide USER column\n"
		"  -4, --hide-read       hide DISK READ column\n"
		"  -5, --hide-write      hide DISK WRITE column\n"
		"  -6, --hide-swapin     hide SWAPIN column\n"
		"  -7, --hide-io         hide IO column\n"
		"  -8, --hide-graph      hide GRAPH column\n"
		"  -9, --hide-command    hide COMMAND column\n"
		"  -q, --quiet           suppress some lines of header (implies --batch)\n"
		"  -x, --dead-x          show dead processes/threads with letter x\n"
	);
}

static inline void parse_args(int argc,char *argv[]) {
	init_params();
	memset(&config,0,sizeof(config));
	config.f.sort_by=SORT_BY_GRAPH;
	config.f.sort_order=SORT_DESC;

	while (1) {
		static struct option long_options[]={
			{"version",no_argument,NULL,'v'},
			{"help",no_argument,NULL,'h'},
			{"batch",no_argument,NULL,'b'},
			{"only",no_argument,NULL,'o'},
			{"iter",required_argument,NULL,'n'},
			{"delay",required_argument,NULL,'d'},
			{"samplerate",required_argument,NULL,'s'},
			{"pid",required_argument,NULL,'p'},
			{"user",required_argument,NULL,'u'},
			{"processes",no_argument,NULL,'P'},
			{"accumulated",no_argument,NULL,'a'},
			{"kilobytes",no_argument,NULL,'k'},
			{"timestamp",no_argument,NULL,'t'},
			{"quiet",no_argument,NULL,'q'},
			{"fullcmdline",no_argument,NULL,'c'},
			{"hide-pid",no_argument,NULL,'1'},
			{"hide-prio",no_argument,NULL,'2'},
			{"hide-user",no_argument,NULL,'3'},
			{"hide-read",no_argument,NULL,'4'},
			{"hide-write",no_argument,NULL,'5'},
			{"hide-swapin",no_argument,NULL,'6'},
			{"hide-io",no_argument,NULL,'7'},
			{"hide-graph",no_argument,NULL,'8'},
			{"hide-command",no_argument,NULL,'9'},
			{"dead-x",no_argument,NULL,'x'},
			{"threads",no_argument,NULL,'T'},
			{"top",required_argument,NULL,'N'},
			{"perf",no_argument,NULL,'f'},
			{"no-enrich",no_argument,NULL,'E'},
			{"dirty",no_argument,NULL,'D'},
			{NULL,0,NULL,0}
		};

		int c=getopt_long(argc,argv,"vhbon:s:d:p:u:Paktqc123456789xTN:fED",long_options,NULL);

		if (c==-1) {
			if (optind<argc) {
				int i;

				for (i=optind;i<argc;i++)
					fprintf(stderr,"%s: unknown argument: %s\n",progname,argv[i]);
				exit(EXIT_FAILURE);
			}
			break;
		}

		switch (c) {
			case 'v':
				print_version();
				exit(EXIT_SUCCESS);
			case 'h':
				print_help();
				exit(EXIT_SUCCESS);
			case 'o':
			case 'b':
			case 'P':
			case 'a':
			case 'k':
			case 't':
			case 'q':
			case 'c':
			case '1' ... '9':
			case 'x':
				config.opts[(unsigned int)(strchr(str_opt,c)-str_opt)]=1;
				break;
			case 'n':
				params.iter=atoi(optarg);
				break;
			case 'd':
				params.delay=atoi(optarg);
				break;
			case 's':
				params.samplerate=atoi(optarg);
				break;
			case 'p':
				params.pid=atoi(optarg);
				break;
			case 'T':
				opt_threads=1;
				break;
			case 'N':
				params.top_n=atoi(optarg);
				if (params.top_n<0)
					params.top_n=0;
				break;
			case 'f':
				params.perf=1;
				break;
			case 'E':
				params.batch_enrich=0;
				break;
			case 'D':
				params.track_dirty=1;
				break;
			case 'u':
				if (optarg[0]=='+') // always interpret as numeric uid
					params.user_id=atoi(optarg+1);
				else {
					struct passwd *pwd=getpwnam(optarg);

					if (!pwd) {
						if (isdigit(optarg[0])) { // fallback to numeric uid
							params.user_id=atoi(optarg);
							break;
						}
						fprintf(stderr,"%s: user %s not found\n",progname,optarg);
						exit(EXIT_FAILURE);
					}
					params.user_id=pwd->pw_uid;
				}
				break;
			default:
				exit(EXIT_FAILURE);
		}
	}
	/*
	 * Sampling policy:
	 *  - Interactive (default): original iotop — walk every thread, per-tid
	 *    taskstats, full USER/PRIO/cmdline (see make_stats_interactive).
	 *  - Batch: process-only + TGID unless -T (fold path).
	 */
	if (config.f.batch_mode||config.f.timestamp||config.f.quiet) {
		if (opt_threads) {
			params.walk_threads=1;
			params.use_tgid=0;
		} else {
			params.walk_threads=0;
			params.use_tgid=1;
		}
	} else {
		params.walk_threads=1;
		params.use_tgid=0;
	}
}

inline void sig_handler(int signo) {
	if (signo==SIGINT) {
		v_fini_cb();
		nl_fini();
		exit(EXIT_SUCCESS);
	}
}

int main(int argc,char *argv[]) {
	progname=argv[0];

	parse_args(argc,argv);
	if (system_checks())
		return EXIT_FAILURE;

	/* Warn once if delay accounting is toggleable and currently off. */
	warn_task_delayacct();
	batch_mem_probe();
	if (params.track_dirty&&!have_smaps_rollup)
		fprintf(stderr,"WARNING: -D requested but /proc/pid/smaps_rollup is unavailable; dirty column will be 0.\n");

	setlocale(LC_ALL,"");
	nl_init();

	if (signal(SIGINT,sig_handler)==SIG_ERR)
		perror("signal");

	if (config.f.timestamp||config.f.quiet)
		config.f.batch_mode=1;

	if (config.f.batch_mode) {
		v_init_cb=view_batch_init;
		v_fini_cb=view_batch_fini;
		v_loop_cb=view_batch_loop;
	}

	v_init_cb();
	v_loop_cb();
	v_fini_cb();
	nl_fini();
	warn_taskstats_version();

	return 0;
}
