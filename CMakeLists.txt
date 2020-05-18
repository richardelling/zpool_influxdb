cmake_minimum_required (VERSION 3.7)
project (zpool_influxdb C)

# By default, ZFSonLinux installs the necessary header files and libraries
# in /usr/local. If this is not the case for your system, change INSTALL_DIR
set(INSTALL_DIR /usr/local)

# to support unsigned 64-bit ints properly, uncomment below to set the
# SUPPORT_UINT64 flag at compile time
set(CMAKE_C_FLAGS "-DSUPPORT_UINT64")

include_directories(${INSTALL_DIR}/include/libspl ${INSTALL_DIR}/include/libzfs)
link_directories(${INSTALL_DIR}/lib)
add_executable(zpool_influxdb zpool_influxdb.c)
target_link_libraries(zpool_influxdb zfs nvpair)
install(TARGETS zpool_influxdb DESTINATION ${INSTALL_DIR}/bin)
