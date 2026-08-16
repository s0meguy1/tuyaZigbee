# Host test for the rescue-mode state machine and the rejoin-scan liveness
# monitor. Nothing here goes near a device.
#
#   make && ./test_rescue
#
CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O1 -g
# Compile the real firmware source, not a copy. The shim directory supplies the
# handful of SDK headers the units include; the -D flags are the ones the
# TS0505B target passes.
CPPFLAGS = -I. -Ishim -D__PROJECT_TL_DIMMABLE_LIGHT__=1 -DMCU_CORE_8258=1 \
           -DBUILD_TS0505B=1 -D_GNU_SOURCE

test_rescue: test_rescue.c ../../light/moes_rescue.c ../../light/moes_rescue.h \
             ../../light/moes_liveness.c ../../light/moes_liveness.h shim/tl_common.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_rescue.c ../../light/moes_rescue.c \
	    ../../light/moes_liveness.c

.PHONY: check clean
check: test_rescue
	./test_rescue

clean:
	rm -f test_rescue
