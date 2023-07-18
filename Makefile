.PHONY: all, clean

CC = gcc
CFLAGS  = -Wall -Werror -DSUPPORT_UINT64 -m64
LDFLAGS = -lzfs -lnvpair
RM = /bin/rm -f
COMMAND = zpool_influxdb

all: zpool_influxdb 

clean:
	$(RM) *.o $(COMMAND)

OBJS = $(COMMAND).o
$(COMMAND): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
