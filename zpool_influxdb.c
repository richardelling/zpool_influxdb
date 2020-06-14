/*
 * Gather top-level ZFS pool and resilver/scan statistics and print using
 * influxdb line protocol
 * usage: [options] [pool_name]
 * where options are:
 *   --execd, -e           run in telegraf execd input plugin mode, [CR] on
 *                         stdin causes a sample to be printed and wait for
 *                         the next [CR]
 *   --no-histograms, -n   don't print histogram data (reduces cardinality
 *                         if you don't care about histograms)
 *   --sum-histogram-buckets, -s sum histogram bucket values
 *
 * To integrate into telegraf use one of:
 * 1. the `inputs.execd` plugin with the `--execd` option
 * 2. the `inputs.exec` plugin to simply run with no options
 *
 * NOTE: libzfs is an unstable interface. YMMV.
 * For Linux compile with:
 *    cmake . && make && make install
 *
 * The design goals of this software include:
 * + be as lightweight as possible
 * + reduce the number of external dependencies as far as possible, hence
 *   there is no dependency on a client library for managing the metric
 *   collection -- info is printed, KISS
 * + broken pools or kernel bugs can cause this process to hang in an
 *   unkillable state. For this reason, it is best to keep the damage limited
 *   to a small process like zpool_influxdb rather than a larger collector.
 *
 * Copyright 2018-2020 Richard Elling
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
#include <libzfs.h>
#include <string.h>
#include <getopt.h>

#define POOL_MEASUREMENT        "zpool_stats"
#define SCAN_MEASUREMENT        "zpool_scan_stats"
#define VDEV_MEASUREMENT        "zpool_vdev_stats"
#define POOL_LATENCY_MEASUREMENT        "zpool_latency"
#define POOL_QUEUE_MEASUREMENT  "zpool_vdev_queue"
#define MIN_LAT_INDEX        10  /* minimum latency index 10 = 1024ns */
#define POOL_IO_SIZE_MEASUREMENT        "zpool_io_size"
#define MIN_SIZE_INDEX        9  /* minimum size index 9 = 512 bytes */

/*
 * telegraf 1.6.4 can handle uint64, which is the native ZFS type
 * telegraf also handles the input of uint64 and will convert to match
 * influxdb via the outputs.influxdb plugin. This is the easiest method
 * for future compatibility. If is it not possible to use telegraf as a
 * metrics broker and unsigned 64-bit is not possible, then consider
 * defining SUPPORT_UINT64 in the CMakeLists.txt or Makefile.
 *
 * influxdb 1.x requires an option to enable uint64
 * influxdb 2.x supports uint64
 */
#ifdef SUPPORT_UINT64
#define IFMT "%luu"
#define MASK_UINT64(x) (x)
#else
#define IFMT "%lui"
#define MASK_UINT64(x) ((x) & INT64_MAX)
#endif

/* global options */
int execd_mode = 0;
int no_histograms = 0;
int sum_histogram_buckets = 0;
uint64_t timestamp = 0;

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
	char *t = (char *) malloc(ZFS_MAX_DATASET_NAME_LEN * 2);
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
 * TODO: update to include issued scan data
 */
int
print_scan_status(nvlist_t *nvroot, const char *pool_name) {
	uint_t c;
	int64_t elapsed;
	uint64_t examined, pass_exam, paused_time, paused_ts, rate;
	uint64_t remaining_time;
	pool_scan_stat_t *ps = NULL;
	double pct_done;
	char *state[DSS_NUM_STATES] = {"none", "scanning", "finished",
	                               "canceled"};
	char *func;

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
	    MASK_UINT64(ps->pss_end_time),
	    MASK_UINT64(ps->pss_errors),
	    MASK_UINT64(examined),
	    MASK_UINT64(pass_exam),
	    MASK_UINT64(paused_ts),
	    MASK_UINT64(paused_time),
	    pct_done,
	    MASK_UINT64(ps->pss_processed),
	    MASK_UINT64(rate),
	    MASK_UINT64(remaining_time),
	    MASK_UINT64(ps->pss_start_time),
	    MASK_UINT64(ps->pss_to_examine),
	    MASK_UINT64(ps->pss_to_process)
	);
	(void) printf("%lu\n", timestamp);
	return (0);
}

/*
 * top-level summary stats are at the pool level
 */
int
print_top_level_summary_stats(nvlist_t *nvroot, const char *pool_name) {
	uint_t c;
	vdev_stat_t *vs;

	if (nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS, (uint64_t **) &vs, &c) != 0) {
		return (1);
	}
	(void) printf("%s,name=%s,state=%s ", POOL_MEASUREMENT, pool_name,
	    zpool_state_to_name((vdev_state_t) vs->vs_state,
	            (vdev_aux_t) vs->vs_aux));
	(void) printf("alloc="IFMT",free="IFMT",size="IFMT","
	              "read_bytes="IFMT",read_errors="IFMT",read_ops="IFMT","
	              "write_bytes="IFMT",write_errors="IFMT",write_ops="IFMT","
	              "checksum_errors="IFMT",fragmentation="IFMT"",
	    MASK_UINT64(vs->vs_alloc),
	    MASK_UINT64(vs->vs_space - vs->vs_alloc),
	    MASK_UINT64(vs->vs_space),
	    MASK_UINT64(vs->vs_bytes[ZIO_TYPE_READ]),
	    MASK_UINT64(vs->vs_read_errors),
	    MASK_UINT64(vs->vs_ops[ZIO_TYPE_READ]),
	    MASK_UINT64(vs->vs_bytes[ZIO_TYPE_WRITE]),
	    MASK_UINT64(vs->vs_write_errors),
	    MASK_UINT64(vs->vs_ops[ZIO_TYPE_WRITE]),
	    MASK_UINT64(vs->vs_checksum_errors),
	    MASK_UINT64(vs->vs_fragmentation));
	(void) printf(" %lu\n", timestamp);
	return (0);
}
/*
 * get a vdev name that corresponds to the top-level vdev names
 * printed by `zpool status`
 */
char *
get_vdev_name(nvlist_t *nvroot, const char *parent_name) {
    static char vdev_name[256];
    char *vdev_type = NULL;
    uint64_t vdev_id = 0;

    if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE,
                             &vdev_type) != 0) {
        vdev_type = "unknown";
    }
    if (nvlist_lookup_uint64(
        nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
        vdev_id = UINT64_MAX;
    }
    if (parent_name == NULL) {
        (void) snprintf(vdev_name, sizeof(vdev_name), "%s",
                        vdev_type);
    } else {
        (void) snprintf(vdev_name, sizeof(vdev_name),
                        "%s/%s-%lu",
                        parent_name, vdev_type, vdev_id);
    }
    return (vdev_name);
}

/*
 * get a string suitable for an influxdb tag that describes this vdev
 *
 * By default only the vdev hierarchical name is shown, separated by '/'
 * If the vdev has an associated path, which is typical of leaf vdevs,
 * then the path is added.
 * It would be nice to have the devid instead of the path, but under
 * Linux we cannot be sure a devid will exist and we'd rather have
 * something than nothing, so we'll use path instead.
 */
char *
get_vdev_desc(nvlist_t *nvroot, const char *parent_name) {
    static char vdev_desc[256];
    char *vdev_type = NULL;
    uint64_t vdev_id = 0;
    char vdev_value[256];
    char *vdev_path = NULL;
    char *s, *t;

    if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE, &vdev_type) != 0) {
        vdev_type = "unknown";
    }
    if (nvlist_lookup_uint64(nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
        vdev_id = UINT64_MAX;
    }
    if (nvlist_lookup_string(
        nvroot, ZPOOL_CONFIG_PATH, &vdev_path) != 0) {
        vdev_path = NULL;
    }

    if (parent_name == NULL) {
        s = escape_string(vdev_type);
        (void) snprintf(vdev_value, sizeof(vdev_value), "vdev=%s", s);
        free(s);
    } else {
        s = escape_string((char *)parent_name);
        t = escape_string(vdev_type);
        (void) snprintf(vdev_value, sizeof(vdev_value),
                        "vdev=%s/%s-%lu", s, t, vdev_id);
        free(s);
        free(t);
    }
    if (vdev_path == NULL) {
        (void) snprintf(vdev_desc, sizeof(vdev_desc), "%s",
                        vdev_value);
    } else {
        s = escape_string(vdev_path);
        (void) snprintf(vdev_desc, sizeof(vdev_desc), "path=%s,%s",
                        s, vdev_value);
        free(s);
    }
    return (vdev_desc);
}

/*
 * vdev latency stats are histograms stored as nvlist arrays of uint64.
 * Latency stats include the ZIO scheduler classes plus lower-level
 * vdev latencies.
 *
 * In many cases, the top-level "root" view obscures the underlying
 * top-level vdev operations. For example, if a pool has a log, special,
 * or cache device, then each can behave very differently. It is useful
 * to see how each is responding.
 */
int
print_vdev_latency_stats(nvlist_t *nvroot, const char *pool_name,
                         const char *parent_name) {
    uint_t c, end = 0;
    nvlist_t *nv_ex;
    char *vdev_desc = NULL;

    /* short_names become part of the metric name and are influxdb-ready */
    struct lat_lookup {
        char *name;
        char *short_name;
        uint64_t sum;
        uint64_t *array;
    };
    struct lat_lookup lat_type[] = {
        {ZPOOL_CONFIG_VDEV_TOT_R_LAT_HISTO,   "total_read", 0},
        {ZPOOL_CONFIG_VDEV_TOT_W_LAT_HISTO,   "total_write", 0},
        {ZPOOL_CONFIG_VDEV_DISK_R_LAT_HISTO,  "disk_read", 0},
        {ZPOOL_CONFIG_VDEV_DISK_W_LAT_HISTO,  "disk_write", 0},
        {ZPOOL_CONFIG_VDEV_SYNC_R_LAT_HISTO,  "sync_read", 0},
        {ZPOOL_CONFIG_VDEV_SYNC_W_LAT_HISTO,  "sync_write", 0},
        {ZPOOL_CONFIG_VDEV_ASYNC_R_LAT_HISTO, "async_read", 0},
        {ZPOOL_CONFIG_VDEV_ASYNC_W_LAT_HISTO, "async_write", 0},
        {ZPOOL_CONFIG_VDEV_SCRUB_LAT_HISTO,   "scrub", 0},
#ifdef ZPOOL_CONFIG_VDEV_TRIM_LAT_HISTO
        {ZPOOL_CONFIG_VDEV_TRIM_LAT_HISTO,    "trim", 0},
#endif
        {NULL,                                NULL}
    };

    if (nvlist_lookup_nvlist(nvroot,
                             ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
        return (6);
    }

    vdev_desc = get_vdev_desc(nvroot, parent_name);

    for (int i = 0; lat_type[i].name; i++) {
        if (nvlist_lookup_uint64_array(nv_ex,
                                       lat_type[i].name,
                                       &lat_type[i].array,
                                       &c) != 0) {
            fprintf(stderr, "error: can't get %s\n", lat_type[i].name);
            return (3);
        }
        /* end count count, all of the arrays are the same size */
        end = c - 1;
    }

    for (int bucket = 0; bucket <= end; bucket++) {
        if (bucket < MIN_LAT_INDEX) {
            /* don't print, but collect the sum */
            for (int i = 0; lat_type[i].name; i++) {
                lat_type[i].sum += lat_type[i].array[bucket];
            }
            continue;
        }
        if (bucket < end) {
            printf("%s,le=%0.6f,name=%s,%s ",
                POOL_LATENCY_MEASUREMENT, (float) (1ULL << bucket) * 1e-9,
                pool_name, vdev_desc);
        } else {
            printf("%s,le=+Inf,name=%s,%s ",
                         POOL_LATENCY_MEASUREMENT, pool_name, vdev_desc);
        }
        for (int i = 0; lat_type[i].name; i++) {
            if (bucket <= MIN_LAT_INDEX || sum_histogram_buckets) {
                lat_type[i].sum += lat_type[i].array[bucket];
            } else {
                lat_type[i].sum = lat_type[i].array[bucket];
            }
            printf("%s="IFMT, lat_type[i].short_name, lat_type[i].sum);
            if (lat_type[i + 1].name != NULL) {
                printf(",");
            }
        }
        printf(" %lu\n", timestamp);
    }
    return (0);
}

/*
 * vdev request size stats are histograms stored as nvlist arrays of uint64.
 * Request size stats include the ZIO scheduler classes plus lower-level
 * vdev sizes. Both independent (ind) and aggregated (agg) sizes are reported.
 *
 * In many cases, the top-level "root" view obscures the underlying
 * top-level vdev operations. For example, if a pool has a log, special,
 * or cache device, then each can behave very differently. It is useful
 * to see how each is responding.
 */
int
print_vdev_size_stats(nvlist_t *nvroot, const char *pool_name,
                      const char *parent_name) {
    uint_t c, end = 0;
    nvlist_t *nv_ex;
    char *vdev_desc = NULL;

    /* short_names become the field name */
    struct size_lookup {
        char *name;
        char *short_name;
        uint64_t sum;
        uint64_t *array;
    };
    struct size_lookup size_type[] = {
        {ZPOOL_CONFIG_VDEV_SYNC_IND_R_HISTO,   "sync_read_ind"},
        {ZPOOL_CONFIG_VDEV_SYNC_IND_W_HISTO,   "sync_write_ind"},
        {ZPOOL_CONFIG_VDEV_ASYNC_IND_R_HISTO,  "async_read_ind"},
        {ZPOOL_CONFIG_VDEV_ASYNC_IND_W_HISTO,  "async_write_ind"},
        {ZPOOL_CONFIG_VDEV_IND_SCRUB_HISTO,    "scrub_read_ind"},
        {ZPOOL_CONFIG_VDEV_SYNC_AGG_R_HISTO,   "sync_read_agg"},
        {ZPOOL_CONFIG_VDEV_SYNC_AGG_W_HISTO,   "sync_write_agg"},
        {ZPOOL_CONFIG_VDEV_ASYNC_AGG_R_HISTO,  "async_read_agg"},
        {ZPOOL_CONFIG_VDEV_ASYNC_AGG_W_HISTO,  "async_write_agg"},
        {ZPOOL_CONFIG_VDEV_AGG_SCRUB_HISTO,    "scrub_read_agg"},
#ifdef ZPOOL_CONFIG_VDEV_IND_TRIM_HISTO
        {ZPOOL_CONFIG_VDEV_IND_TRIM_HISTO,    "trim_write_ind"},
            {ZPOOL_CONFIG_VDEV_AGG_TRIM_HISTO,    "trim_write_agg"},
#endif
        {NULL,                                NULL}
    };

    if (nvlist_lookup_nvlist(nvroot,
                             ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
        return (6);
    }

    vdev_desc = get_vdev_desc(nvroot, parent_name);

    for (int i = 0; size_type[i].name; i++) {
        if (nvlist_lookup_uint64_array(nv_ex,
                                       size_type[i].name,
                                       &size_type[i].array,
                                       &c) != 0) {
            fprintf(stderr, "error: can't get %s\n", size_type[i].name);
            return (3);
        }
        /* end count count, all of the arrays are the same size */
        end = c - 1;
    }

   for (int bucket = 0; bucket <= end; bucket++) {
       if (bucket < MIN_SIZE_INDEX) {
           /* don't print, but collect the sum */
           for (int i = 0; size_type[i].name; i++) {
               size_type[i].sum += size_type[i].array[bucket];
           }
           continue;
       }

       if (bucket < end) {
            printf("%s,le=%llu,name=%s,%s ",
                   POOL_IO_SIZE_MEASUREMENT, 1ULL << bucket,
                   pool_name, vdev_desc);
       } else {
           printf("%s,le=+Inf,name=%s,%s ",
                  POOL_IO_SIZE_MEASUREMENT, pool_name, vdev_desc);
       }
       for (int i = 0; size_type[i].name; i++) {
           if (bucket <= MIN_SIZE_INDEX || sum_histogram_buckets) {
               size_type[i].sum += size_type[i].array[bucket];
           } else {
               size_type[i].sum = size_type[i].array[bucket];
           }
           printf("%s="IFMT, size_type[i].short_name, size_type[i].sum);
           if (size_type[i + 1].name != NULL) {
               printf(",");
           }
       }
       printf(" %lu\n", timestamp);
    }
    return (0);
}

/*
 * ZIO scheduler queue stats are stored as gauges. This is unfortunate
 * because the values can change very rapidly and any point-in-time
 * value will quickly be obsoleted. It is also not easy to downsample.
 * Thus only the top-level queue stats might be beneficial... maybe.
 */
int
print_queue_stats(nvlist_t *nvroot, const char *pool_name,
                  const char *parent_name) {
    nvlist_t *nv_ex;
    uint64_t value;

    /* short_names are used for the field name */
    struct queue_lookup {
        char *name;
        char *short_name;
    };
    struct queue_lookup queue_type[] = {
        {ZPOOL_CONFIG_VDEV_SYNC_R_ACTIVE_QUEUE,  "sync_r_active"},
        {ZPOOL_CONFIG_VDEV_SYNC_W_ACTIVE_QUEUE,  "sync_w_active"},
        {ZPOOL_CONFIG_VDEV_ASYNC_R_ACTIVE_QUEUE, "async_r_active"},
        {ZPOOL_CONFIG_VDEV_ASYNC_W_ACTIVE_QUEUE, "async_w_active"},
        {ZPOOL_CONFIG_VDEV_SCRUB_ACTIVE_QUEUE,  "async_scrub_active"},
        {ZPOOL_CONFIG_VDEV_SYNC_R_PEND_QUEUE,    "sync_r_pend"},
        {ZPOOL_CONFIG_VDEV_SYNC_W_PEND_QUEUE,    "sync_w_pend"},
        {ZPOOL_CONFIG_VDEV_ASYNC_R_PEND_QUEUE,   "async_r_pend"},
        {ZPOOL_CONFIG_VDEV_ASYNC_W_PEND_QUEUE,   "async_w_pend"},
        {ZPOOL_CONFIG_VDEV_SCRUB_PEND_QUEUE,     "async_scrub_pend"},
        {NULL,                                   NULL}
    };

    if (nvlist_lookup_nvlist(nvroot,
                             ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
        return (6);
    }

    printf("%s,name=%s,%s ",
           POOL_QUEUE_MEASUREMENT, pool_name,
           get_vdev_desc(nvroot, parent_name));
    for (int i = 0; queue_type[i].name; i++) {
        if (nvlist_lookup_uint64(nv_ex,
                                 queue_type[i].name, &value) != 0) {
            fprintf(stderr, "error: can't get %s\n",
                    queue_type[i].name);
            return (3);
        }
        printf("%s="IFMT, queue_type[i].short_name, value);
        if (queue_type[i + 1].name != NULL) {
            printf(",");
        }
    }
    printf(" %lu\n", timestamp);
    return (0);
}

/*
 * top-level vdev stats are at the pool level
 */
int
print_top_level_vdev_stats(nvlist_t *nvroot, const char *pool_name) {
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

	(void) printf("%s,name=%s,vdev=root ", VDEV_MEASUREMENT, pool_name);
	for (int i = 0; queue_type[i].name; i++) {
		if (nvlist_lookup_uint64(nv_ex,
                                 queue_type[i].name, &value) != 0) {
			fprintf(stderr, "error: can't get %s\n",
			    queue_type[i].name);
			return (3);
		}
		if (i > 0)
			printf(",");
		printf("%s="IFMT, queue_type[i].short_name, MASK_UINT64(value));
	}

	(void) printf(" %lu\n", timestamp);
	return (0);
}

/*
 * recursive stats printer
 */
typedef int (*stat_printer_f)(nvlist_t *, const char *, const char *);

int
print_recursive_stats(stat_printer_f func, nvlist_t *nvroot,
                      const char *pool_name, const char *parent_name,
                      int descend) {
    uint_t c, children;
    nvlist_t **child;
    char vdev_name[256];
    int err;

    err = func(nvroot, pool_name, parent_name);
    if (err)
        return (err);

    if (descend && nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
                                              &child, &children) == 0) {
        (void) strncpy(vdev_name, get_vdev_name(nvroot, parent_name),
                       sizeof(vdev_name));
        vdev_name[sizeof(vdev_name) - 1] = '\0';

        for (c = 0; c < children; c++) {
            print_recursive_stats(func, child[c], pool_name,
                                  vdev_name, descend);
        }
    }
    return (0);
}

/*
 * call-back to print the stats from the pool config
 *
 * Note: if the pool is broken, this can hang indefinitely and perhaps in an
 * unkillable state.
 */
int
print_stats(zpool_handle_t *zhp, void *data) {
	uint_t c;
	int err;
	boolean_t missing;
	nvlist_t *config, *nvroot;
	vdev_stat_t *vs;
	struct timespec tv;
	char *pool_name;

	/* if not this pool return quickly */
	if (data &&
	    strncmp(data, zhp->zpool_name, ZFS_MAX_DATASET_NAME_LEN) != 0) {
        zpool_close(zhp);
        return (0);
    }

	if (zpool_refresh_stats(zhp, &missing) != 0) {
        zpool_close(zhp);
        return (1);
    }

	config = zpool_get_config(zhp, NULL);
	if (clock_gettime(CLOCK_REALTIME, &tv) != 0)
		timestamp = (uint64_t) time(NULL) * 1000000000;
	else
		timestamp =
		    ((uint64_t) tv.tv_sec * 1000000000) + (uint64_t) tv.tv_nsec;

	if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, &nvroot) != 0) {
        zpool_close(zhp);
		return (2);
	}
	if (nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_VDEV_STATS,
	        (uint64_t **) &vs, &c) != 0) {
        zpool_close(zhp);
		return (3);
	}

	pool_name = escape_string(zhp->zpool_name);
	err = print_top_level_summary_stats(nvroot, pool_name);

	/* if any of these return an error, skip the rest */
	if (err == 0)
		err = print_scan_status(nvroot, pool_name);

	if (err == 0)
        err = print_top_level_vdev_stats(nvroot, pool_name);

	if (no_histograms == 0) {
        if (err == 0)
            err = print_recursive_stats(print_vdev_latency_stats, nvroot,
                                        pool_name, NULL, 1);
        if (err == 0)
            err = print_recursive_stats(print_vdev_size_stats, nvroot,
                                        pool_name, NULL, 1);
        if (err == 0)
            err = print_recursive_stats(print_queue_stats, nvroot,
                                        pool_name, NULL, 0);
    }
	free(pool_name);
    zpool_close(zhp);
	return (err);
}


void
usage(char* name) {
    fprintf(stderr, "usage: %s [--execd][--no-histograms]"
                    "[--sum-histogram-buckets] [poolname]\n", name);
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[]) {
    int opt;
    int ret = 8;
    char *line = NULL;
    size_t len = 0;
    struct option long_options[] = {
        {"execd", no_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {"no-histograms", no_argument, NULL, 'n'},
        {"sum-histogram-buckets", no_argument, NULL, 's'},
        {0, 0, 0, 0}
    };
    while ((opt = getopt_long(argc, argv, "ehns", long_options, NULL)) != -1) {
        switch (opt) {
            case 'e':
                execd_mode = 1;
                break;
            case 'n':
                no_histograms = 1;
                break;
            case 's':
                sum_histogram_buckets = 1;
                break;
            default:
                usage(argv[0]);
        }
    }

	libzfs_handle_t *g_zfs;
	if ((g_zfs = libzfs_init()) == NULL) {
		fprintf(stderr,
		    "error: cannot initialize libzfs. "
		    "Is the zfs module loaded or zrepl running?");
		exit(EXIT_FAILURE);
	}
	if (execd_mode == 0) {
        ret = zpool_iter(g_zfs, print_stats, argv[optind]);
        return (ret);
    }
    while (getline(&line, &len, stdin) != -1) {
        ret = zpool_iter(g_zfs, print_stats, argv[optind]);
        fflush(stdout);
	}
    return (ret);
}

