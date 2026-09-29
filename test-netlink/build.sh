#!/bin/sh
set -e
cd "$(dirname "$0")"
make -C ../build-linux -j8 >/dev/null
gcc -O2 -g -Wall -DM_CORE_GBA -DM_CORE_GB -DBUILD_STATIC -DENABLE_VFS -DENABLE_DIRECTORIES -DUSE_PTHREADS -DENABLE_NETLINK -I../include -I../build-linux/include -I../src harness.c ../build-linux/libmgba.a -lm -lpthread -o netlink-test
