/*
 * Gather top-level ZFS pool and resilver/scan statistics and print using
 * influxdb line protocol
 * usage: [pool_name]
 *
 * To integrate into telegraf, use the inputs.exec plugin
 *
 * NOTE: libzfs is an unstable interface. YMMV.
 * For Linux compile with: gcc -lzfs -lnvpair zpool_influxdb.c -o zpool_influxdb
 *
 * Copyright 2018-2019 Richard Elling
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/fs/zfs.h>
#include <time.h>
#include <libzfs.h>
#include <string.h>

#define POOL_MEASUREMENT        "zpool_stats"
#define SCAN_MEASUREMENT        "zpool_scan_stats"
#define VDEV_MEASUREMENT        "zpool_vdev_stats"

/*
 * printf format for 64-bit unsigned int as influxdb line protocol
 * prior to telegraf version 1.6.2, this needs to be "%lui"
 */
#define IFMT "%luu"

/*
 * in cases where ZFS is installed, but not the ZFS dev environment, copy in
 * the needed definitions from libzfs_impl.h
 */
#ifndef _LIBZFS_IMPL_H
struct zpool_handle {
    libzfs_handle_t *zpool_hdl;
    zpool_handle_t *zpool_next;
    char zpool_name[ZFS_MAX_DATASET_NAME_LEN];
    int zpool_state;
    size_t zpool_config_size;
    nvlist_t *zpool_config;
    nvlist_t *zpool_old_config;
    nvlist_t *zpool_props;
    diskaddr_t zpool_start_block;
};
#endif

/*
 * influxdb line protocol rules for escaping are important because the
 * zpool name can include characters that need to be escaped
 *
 * caller is responsible for freeing result
 */
char *
escape_string(char *s) {
	char *c, *d;
	char *t = (char *) malloc(ZFS_MAX_DATASET_NAME_LEN << 1);
	if (t == NULL) {
		fprintf(stderr, "error: cannot allocate memory\n");
		exit(1);
	}

	for (c = s, d = t; *c != '\0'; c++, d++) {
		switch (*c) {
			case ' ':
			case ',':
			case '=':
			case '\\':
				*d++ = '\\';
			default:
				*d = *c;
		}
	}
	*d = '\0';
	return (t);
}

/*
 * print_scan_status() prints the details as often seen in the "zpool status"
 * output. However, unlike the zpool command, which is intended for humans,
 * this output is suitable for long-term tracking in influxdb.
 */
int
print_scan_status(nvlist_t *nvroot, const char *pool_name, uint64_t ts) {
	uint_t c;
	int64_t elapsed;
	uint64_t examined, pass_exam, paused_time, paused_ts, rate;
	uint64_t remaining_time;
	pool_scan_stat_t *ps = NULL;
	double pct_done;
	char *state[DSS_NUM_STATES] = {"none", "scanning", "finished",
	                               "canceled"};
	char *func = "unknown_function";

	(void) nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_SCAN_STATS,
	    (uint64_t **) &ps, &c);

	/*
	 * ignore if there are no stats or state is bogus
	 */
	if (ps == NULL || ps->pss_state >= DSS_NUM_STATES ||
	    ps->pss_func >= POOL_SCAN_FUNCS)
		return (1);

	switch (ps->pss_func) {
		case POOL_SCAN_NONE:
			func = "none_requested";
			break;
		case POOL_SCAN_SCRUB:
			func = "scrub";
			break;
		case POOL_SCAN_RESILVER:
			func = "resilver";
			break;
#ifdef POOL_SCAN_REBUILD
		case POOL_SCAN_REBUILD:
				func = "rebuild";
				break;
#endif
		default:
			func = "scan";
	}

	/* overall progress */
	examined = ps->pss_examined ? ps->pss_examined : 1;
	pct_done = 0.0;
	if (ps->pss_to_examine > 0)
		pct_done = 100.0 * examined / ps->pss_to_examine;

#ifdef EZFS_SCRUB_PAUSED
	paused_ts = ps->pss_pass_scrub_pause;
			paused_time = ps->pss_pass_scrub_spent_paused;
#else
	paused_ts = 0;
	paused_time = 0;
#endif

	/* calculations for this pass */
	if (ps->pss_state == DSS_SCANNING) {
		elapsed = (int64_t) time(NULL) - (int64_t) ps->pss_pass_start -
		          (int64_t) paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		pass_exam = ps->pss_pass_exam ? ps->pss_pass_exam : 1;
		rate = pass_exam / elapsed;
		rate = (rate > 0) ? rate : 1;
		remaining_time = ps->pss_to_examine - examined / rate;
	} else {
		elapsed =
		    (int64_t) ps->pss_end_time - (int64_t) ps->pss_pass_start -
		    (int64_t) paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		pass_exam = ps->pss_pass_exam ? ps->pss_pass_exam : 1;
		rate = pass_exam / elapsed;
		remaining_time = 0;
	}
	rate = rate ? rate : 1;

	/* influxdb line protocol format: "tags metrics timestamp" */
	(void) printf("%s,function=%s,name=%s,state=%s ",
	    SCAN_MEASUREMENT, func, pool_name, state[ps->pss_state]);
	(void) printf("end_ts="IFMT",errors="IFMT",examined="IFMT","
	              "pass_examined="IFMT",pause_ts="IFMT",paused_t="IFMT","
	              "pct_done=%.2f,processed="IFMT",rate="IFMT","
	              "remaining_t="IFMT",start_ts="IFMT","
	              "to_examine="IFMT",to_process="IFMT" ",
	    ps->pss_end_time,
	    ps->pss_errors,
	    examined,
	    pass_exam,
	    paused_ts,
	    paused_time,
	    pct_done,
	    ps->pss_processed,
	    rate,
	    remaining_time,
	    ps->pss_start_time,
	    ps->pss_to_examine,
	    ps->pss_to_process
	);
	(void) printf("%lu\n", ts);
	return (0);
}

/*
 * top-level summary stats are at the pool level
 */
int
print_top_level_summary_stats(nvlist_t *nvroot, const char *pool_name,
                              uint64_t ts) {
	uint_t c;
	vdev_stat_t *vs;

	if (nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS, (uint64_t **) &vs, &c) != 0) {
		return (1);
	}
	(void) printf("%s,name=%s,state=%s ", POOL_MEASUREMENT, pool_name,
	    zpool_state_to_name(vs->vs_state, vs->vs_aux));
	(void) printf("alloc="IFMT",free="IFMT",size="IFMT","
	              "read_bytes="IFMT",read_errors="IFMT",read_ops="IFMT","
	              "write_bytes="IFMT",write_errors="IFMT",write_ops="IFMT","
	              "checksum_errors="IFMT",fragmentation="IFMT"",
	    vs->vs_alloc,
	    vs->vs_space - vs->vs_alloc,
	    vs->vs_space,
	    vs->vs_bytes[ZIO_TYPE_READ],
	    vs->vs_read_errors,
	    vs->vs_ops[ZIO_TYPE_READ],
	    vs->vs_bytes[ZIO_TYPE_WRITE],
	    vs->vs_write_errors,
	    vs->vs_ops[ZIO_TYPE_WRITE],
	    vs->vs_checksum_errors,
	    vs->vs_fragmentation);
	(void) printf(" %lu\n", ts);
	return (0);
}

/*
 * top-level vdev stats are at the pool level
 */
int
print_top_level_vdev_stats(nvlist_t *nvroot, const char *pool_name,
                              uint64_t ts) {
	nvlist_t *nv_ex;
	uint64_t value;

	/* short_names become part of the metric name */
	struct queue_lookup {
	    char *name;
	    char *short_name;
	};
	struct queue_lookup queue_type[] = {
	    {ZPOOL_CONFIG_VDEV_SYNC_R_ACTIVE_QUEUE, "sync_r_active_queue"},
	    {ZPOOL_CONFIG_VDEV_SYNC_W_ACTIVE_QUEUE, "sync_w_active_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_R_ACTIVE_QUEUE, "async_r_active_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_W_ACTIVE_QUEUE, "async_w_active_queue"},
	    {ZPOOL_CONFIG_VDEV_SCRUB_ACTIVE_QUEUE, "async_scrub_active_queue"},
	    {ZPOOL_CONFIG_VDEV_SYNC_R_PEND_QUEUE, "sync_r_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_SYNC_W_PEND_QUEUE, "sync_w_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_R_PEND_QUEUE, "async_r_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_W_PEND_QUEUE, "async_w_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_SCRUB_PEND_QUEUE, "async_scrub_pend_queue"},
	    {NULL, NULL}
	};

	if (nvlist_lookup_nvlist(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
		return (6);
	}

	(void) printf("%s,name=%s,vdev=top ", VDEV_MEASUREMENT, pool_name);
	for (int i = 0; queue_type[i].name; i++) {
		if (nvlist_lookup_uint64(nv_ex,
		    queue_type[i].name, (uint64_t *) &value) != 0) {
			fprintf(stderr, "error: can't get %s\n",
			    queue_type[i].name);
			return (3);
		}
		if (i > 0)
			printf(",");
		printf("%s="IFMT, queue_type[i].short_name, value);
	}

	(void) printf(" %lu\n", ts);
	return (0);
}

/*
 * call-back to print the stats from the pool config
 *
 * Note: if the pool is broken, this can hang indefinitely
 */
int
print_stats(zpool_handle_t *zhp, void *data) {
	uint_t c, err;
	boolean_t missing;
	nvlist_t *nv, *nv_ex, *config, *nvroot;
	vdev_stat_t *vs;
	uint64_t *lat_array;
	uint64_t ts;
	struct timespec tv;
	char *pool_name;
	pool_scan_stat_t *ps = NULL;

	/* if not this pool return quickly */
	if (data &&
	    strncmp(data, zhp->zpool_name, ZFS_MAX_DATASET_NAME_LEN) != 0)
		return (0);

	if (zpool_refresh_stats(zhp, &missing) != 0)
		return (1);

	config = zpool_get_config(zhp, NULL);
	if (clock_gettime(CLOCK_REALTIME, &tv) != 0)
		ts = (uint64_t) time(NULL) * 1000000000;
	else
		ts =
		    ((uint64_t) tv.tv_sec * 1000000000) + (uint64_t) tv.tv_nsec;

	if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, &nvroot) !=
	    0) {
		return (2);
	}
	if (nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS,
	    (uint64_t **) &vs, &c) != 0) {
		return (3);
	}

	pool_name = escape_string(zhp->zpool_name);
	err = print_top_level_summary_stats(nvroot, pool_name, ts);

	if (err == 0)
		err = print_scan_status(nvroot, pool_name, ts);

	if (err == 0)
		err = print_top_level_vdev_stats(nvroot, pool_name, ts);

	free(pool_name);
	return (0);
}


int
main(int argc, char *argv[]) {
	libzfs_handle_t *g_zfs;
	if ((g_zfs = libzfs_init()) == NULL) {
		fprintf(stderr,
		    "error: cannot initialize libzfs. "
		    "Is the zfs module loaded or zrepl running?");
		exit(1);
	}
	if (argc > 1) {
		return (zpool_iter(g_zfs, print_stats, argv[1]));
	} else {
		return (zpool_iter(g_zfs, print_stats, NULL));
	}
}

