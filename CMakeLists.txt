cmake_minimum_required (VERSION 3.7)
project (zpool_influxdb C)

# By default, ZFSonLinux installs the necessary header files and
# libraries in /usr/local. If this is not the case for your system,
# set ZFS_INSTALL_BASE, e.g. for the Ubuntu zfs-on-linux library, use:
#      -D ZFS_INSTALL_BASE=/usr
# on the cmake command-line.
set(ZFS_INSTALL_BASE /usr/local CACHE STRING "zfs installation base directory")

# to support unsigned 64-bit ints properly, uncomment below to set the
# SUPPORT_UINT64 flag at compile time
set(CMAKE_C_FLAGS "-DSUPPORT_UINT64")

include_directories(${ZFS_INSTALL_BASE}/include/libspl ${ZFS_INSTALL_BASE}/include/libzfs)
link_directories(${ZFS_INSTALL_BASE}/lib)
add_executable(zpool_influxdb zpool_influxdb.c)
target_link_libraries(zpool_influxdb zfs nvpair)
install(TARGETS zpool_influxdb DESTINATION ${ZFS_INSTALL_BASE}/bin)
