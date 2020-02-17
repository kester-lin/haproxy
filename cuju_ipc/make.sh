#!/bin/sh

if [ ! -e /proc/sysvipc/sem ]; then
    echo "Does your kernel support System V IPC?"
    exit 1
fi

CC=gcc

$CC -g -pthread -o ipc_handler ipc_handler.c
if [ $? != 0 ]; then
    echo "compile error"
    exit 1
fi


$CC -g -pthread -o netlink netlink.c
if [ $? != 0 ]; then
    echo "compile error"
    exit 1
fi

##$CC -g -pthread -o proc2 proc2.c
##if [ $? != 0 ]; then
##    echo "compile error"
##    exit 1
##fi

##./proc1 &
##sleep 1
##./proc2
##sleep 1
##./proc2
##sleep 1
##./proc2
##sleep 1
##./proc2 end

