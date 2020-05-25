# Influxdb Metrics for ZFS Pools
The _zpool_influxdb_ program produces 
[influxdb](https://github.com/influxdata/influxdb) line protocol
compatible metrics from zpools. In the UNIX tradition, _zpool_influxdb_
does one thing: read statistics from a pool and print them to
stdout. In many ways, this is a metrics-friendly output of 
statistics normally observed via the `zpool` command.

## ZFS Versions
There are many implementations of ZFS on many OSes. The current
version is tested to work on:
* [ZFSonLinux](https://github.com/zfsonlinux/zfs) version 0.7 and later
* [cstor](https://github.com/openebs/cstor) for userland ZFS (uZFS)

This should compile and run on other ZFS versions, though many 
do not have the latency histograms. Pull requests are welcome.

## Usage
When run without arguments, _zpool_influxdb_ runs once, reading data
from all imported pools, and prints to stdout.
```shell
zpool_influxdb [options] [poolname]
```
If no poolname is specified, then all pools are sampled.

| option | short option | description |
|---|---|---|
| --execd | -e | For use with telegraf's `execd` plugin. When [enter] is pressed, the pools are sampled. To exit, use [ctrl+D] |
| --no-histogram | -n | Do not print histogram information |
| --sum-histogram-buckets | -s | Sum histogram bucket values |
| --help | -h | Print a short usage message |

#### Histogram Bucket Values
The histogram data collected by ZFS is stored as independent bucket values.
This works well out-of-the-box with an influxdb data source and grafana's
heatmap visualization. The influxdb query for a grafana heatmap 
visualization looks like:
```
field(disk_read) last() non_negative_derivative(1s)
```

Another method for storing histogram data sums the values for lower-value
buckets. For example, a latency bucket tagged "le=10" includes the values
in the bucket "le=1".
This method is often used for prometheus histograms.
The `zpool_influxdb --sum-histogram-buckets` option presents the data from ZFS
as summed values.

## Measurements
The following measurements are collected:

| measurement | description | zpool equivalent |
|---|---|---|
| zpool_stats | general size and data | zpool list |
| zpool_scan_stats | scrub, rebuild, and resilver statistics (omitted if no scan has been requested) | zpool status |
| zpool_vdev_stats | per-vdev statistics | zpool iostat -q |
| zpool_io_size | per-vdev I/O size histogram | zpool iostat -r |
| zpool_latency | per-vdev I/O latency histogram | zpool iostat -w |
| zpool_vdev_queue | per-vdev instantaneous queue depth | zpool iostat -q |

### zpool_stats Description
zpool_stats contains top-level summary statistics for the pool.
Performance counters measure the I/Os to the pool's devices.

#### zpool_stats Tags

| label | description |
|---|---|
| name | pool name |
| state | pool state, as shown by _zpool status_ |

#### zpool_stats Fields

| field | units | description |
|---|---|---|
| alloc | bytes | allocated space |
| free | bytes | unallocated space |
| size | bytes | total pool size |
| read_bytes | bytes | bytes read since pool import |
| read_errors | count | number of read errors |
| read_ops | count | number of read operations |
| write_bytes | bytes | bytes written since pool import |
| write_errors | count | number of write errors |
| write_ops | count | number of write operations |

### zpool_scan_stats Description
Once a pool has been scrubbed, resilvered, or rebuilt, the zpool_scan_stats
contain information about the status and performance of the operation.
Otherwise, the zpool_scan_stats do not exist in the kernel, and therefore
cannot be reported by this collector.

#### zpool_scan_stats Tags

| label | description |
|---|---|
| name | pool name |
| function | name of the scan function running or recently completed |
| state | scan state, as shown by _zpool status_ |

#### zpool_scan_stats Fields

| field | units | description |
|---|---|---|
| errors | count | number of errors encountered by scan |
| examined | bytes | total data examined during scan |
| to_examine | bytes | prediction of total bytes to be scanned |
| pass_examined | bytes | data examined during current scan pass |
| processed | bytes | data reconstructed during scan |
| to_process | bytes | total bytes to be repaired |
| rate | bytes/sec | examination rate |
| start_ts | epoch timestamp | start timestamp for scan |
| pause_ts | epoch timestamp | timestamp for a scan pause request |
| end_ts | epoch timestamp | completion timestamp for scan |
| paused_t | seconds | elapsed time while paused |
| remaining_t | seconds | estimate of time remaining for scan |

### zpool_vdev_stats Description
The ZFS I/O (ZIO) scheduler uses five queues to schedule I/Os to each vdev.
These queues are further divided into active and pending states.
An I/O is pending prior to being issued to the vdev. An active
I/O has been issued to the vdev. The scheduler and its tunable
parameters are described at the 
[ZFS on Linux wiki.](https://github.com/zfsonlinux/zfs/wiki/ZIO-Scheduler)
The ZIO scheduler reports the queue depths as gauges where the value 
represents an instantaneous snapshot of the queue depth at 
the sample time. Therefore, it is not unusual to see all zeroes
for an idle pool.

#### zpool_vdev_stats Tags
| label | description |
|---|---|
| name | pool name |
| vdev | vdev name (root = entire pool) |

#### zpool_vdev_stats Fields
| field | units | description |
|---|---|---|
| sync_r_active_queue | entries | synchronous read active queue depth |
| sync_w_active_queue | entries | synchronous write active queue depth |
| async_r_active_queue | entries | asynchronous read active queue depth |
| async_w_active_queue | entries | asynchronous write active queue depth |
| async_scrub_active_queue | entries | asynchronous scrub active queue depth |
| sync_r_pend_queue | entries | synchronous read pending queue depth |
| sync_w_pend_queue | entries | synchronous write pending queue depth |
| async_r_pend_queue | entries | asynchronous read pending queue depth |
| async_w_pend_queue | entries | asynchronous write pending queue depth |
| async_scrub_pend_queue | entries | asynchronous scrub pending queue depth |

### zpool_latency Histogram
ZFS tracks the latency of each I/O in the ZIO pipeline. This latency can
be useful for observing latency-related issues that are not easily observed
using the averaged latency statistics.

The histogram fields show cumulative values from lowest to highest.
The largest bucket is tagged "le=+Inf", representing the total count
of I/Os by type and vdev.

#### zpool_latency Histogram Tags
| label | description |
|---|---|
| le | bucket for histogram, latency is less than or equal to bucket value in seconds |
| name | pool name |
| path | for leaf vdevs, the device path name, otherwise omitted |
| vdev | vdev name (root = entire pool) |

#### zpool_latency Histogram Fields
| field | units | description |
|---|---|---|
| total_read | operations | read operations of all types |
| total_write | operations | write operations of all types |
| disk_read | operations | disk read operations |
| disk_write | operations | disk write operations |
| sync_read | operations | ZIO sync reads |
| sync_write | operations | ZIO sync writes |
| async_read | operations | ZIO async reads|
| async_write | operations | ZIO async writes |
| scrub | operations | ZIO scrub/scan reads |
| trim | operations | ZIO trim (aka unmap) writes |

### zpool_io_size Histogram
ZFS tracks I/O throughout the ZIO pipeline. The size of each I/O is used
to create a histogram of the size by I/O type and vdev. For example, a
4KiB write to mirrored pool will show a 4KiB write to the top-level vdev
(root) and a 4KiB write to each of the mirror leaf vdevs.

The ZIO pipeline can aggregate I/O operations. For example, a contiguous
series of writes can be aggregated into a single, larger I/O to the leaf
vdev. The independent I/O operations reflect the logical operations and
the aggregated I/O operations reflect the physical operations.

The histogram fields show cumulative values from lowest to highest.
The largest bucket is tagged "le=+Inf", representing the total count
of I/Os by type and vdev.

Note: trim I/Os can be larger than 16MiB, but the larger sizes are 
accounted in the 16MiB bucket.

#### zpool_io_size Histogram Tags
| label | description |
|---|---|
| le | bucket for histogram, I/O size is less than or equal to bucket value in bytes |
| name | pool name |
| path | for leaf vdevs, the device path name, otherwise omitted |
| vdev | vdev name (root = entire pool) |

#### zpool_io_size Histogram Fields
| field | units | description |
|---|---|---|
| sync_read_ind | blocks | independent sync reads |
| sync_write_ind | blocks | independent sync writes |
| async_read_ind | blocks | independent async reads |
| async_write_ind | blocks | independent async writes |
| scrub_read_ind | blocks | independent scrub/scan reads |
| trim_write_ind | blocks | independent trim (aka unmap) writes |
| sync_read_agg | blocks | aggregated sync reads |
| sync_write_agg | blocks | aggregated sync writes |
| async_read_agg | blocks | aggregated async reads |
| async_write_agg | blocks | aggregated async writes |
| scrub_read_agg | blocks | aggregated scrub/scan reads |
| trim_write_agg | blocks | aggregated trim (aka unmap) writes |

#### About unsigned integers
Telegraf v1.6.2 and later support unsigned 64-bit integers which more 
closely matches the uint64_t values used by ZFS. By default, zpool_influxdb
will mask ZFS' uint64_t values and use influxdb line protocol integer type.
Eventually the monitoring world will catch up to the times and support 
unsigned integers. To support unsigned, define SUPPORT_UINT64 and compile
as described in `CMakeLists.txt`

## Building
Building is simplified by using cmake.
It is as simple as possible, but no simpler.
By default, [ZFSonLinux](https://github.com/zfsonlinux/zfs) 
installs the necessary header and library files in _/usr/local_.
If you place those files elsewhere, either edit _CMakeLists.txt_ and
change the _ZFS_INSTALL_BASE_ or pass it with `-D ZFS_INSTALL_BASE=/usr`
on the cmake command line:
```bash
cmake .
make
```
If successful, the _zpool_influxdb_ executable is created.

## Installing
Installation is left as an exercise for the reader because
there are many different methods that can be used.
Ultimately the method depends on how the local metrics collection is 
implemented and the local access policies.

To install the _zpool_influxdb_ executable in _INSTALL_DIR_, use
```bash
make install
```

The simplest method is to use the exec agent in telegraf. For convenience,
a sample config file is _zpool_influxdb.conf_ which can be placed in the
telegraf config-directory (often /etc/telegraf/telegraf.d). Telegraf can
be restarted to read the config-directory files.

## Caveat Emptor
* Like the _zpool_ command, _zpool_influxdb_ takes a reader 
  lock on spa_config for each imported pool. If this lock blocks,
  then the command will also block indefinitely and might be
  unkillable. This is not a normal condition, but can occur if 
  there are bugs in the kernel modules. 
  For this reason, care should be taken:
  * avoid spawning many of these commands hoping that one might 
    finish
  * avoid frequent updates or short sample time
    intervals, because the locks can interfere with the performance
    of other instances of _zpool_ or _zpool_influxdb_

## Other collectors
There are a few other collectors for zpool statistics roaming around
the Internet. Many attempt to screen-scrape `zpool` output in various 
ways. The screen-scrape method works poorly for `zpool` output because
of its human-friendly nature. Also, they suffer from the same caveats
as this implementation. This implementation is optimized for directly
collecting the metrics and is much more efficient than the screen-scrapers.

## Feedback Encouraged
Pull requests and issues are greatly appreciated. Visit
https://github.com/richardelling/zpool_influxdb
