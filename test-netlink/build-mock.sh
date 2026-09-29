#!/bin/sh
set -e
cd "$(dirname "$0")"
make -C ../build-gui -j8 mgba >/dev/null
M=..
DEFS="-DM_CORE_GBA -DM_CORE_GB -DBUILD_STATIC -DENABLE_VFS -DENABLE_DIRECTORIES -DUSE_PTHREADS -DUSE_PNG -DUSE_ZLIB -DENABLE_NETLINK -D_GNU_SOURCE"
gcc -g -O1 -std=c11 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter $DEFS -I$M/include -I$M/build-gui/include -I$M/src \
	mock-gui.c \
	$M/src/feature/gui/gui-runner.c $M/src/feature/gui/netlink.c $M/src/feature/gui/gui-config.c $M/src/feature/gui/cheats.c $M/src/feature/gui/remap.c \
	$M/src/util/gui.c $M/src/util/gui/file-select.c $M/src/util/gui/font.c $M/src/util/gui/font-metrics.c $M/src/util/gui/menu.c \
	$M/build-gui/libmgba.a -lpng -lz -lfreetype -lm -lpthread -o mock-gui 2>&1 | grep -v "gui-runner.c.*pointer-to-int\|stateId\|^ *|\|^ *[0-9]* |\|In function\|note:" || true
ls -l mock-gui
