# Host test for the real BENCH ONLY PWM/UART self-test scheduler.
# Nothing here touches a device.
#
#   make check

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O1 -g
CPPFLAGS  = -I. -Ishim -D__PROJECT_TL_DIMMABLE_LIGHT__=1 -DMCU_CORE_8258=1 \
            -DBUILD_TS0505B=1 -DMOES_BENCH_SELFTEST=1

test_bench_selftest: test_bench_selftest.c ../../light/moes_bench_selftest.c \
                     ../../light/moes_bench_selftest.h shim/tl_common.h

	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_bench_selftest.c \
		../../light/moes_bench_selftest.c

.PHONY: check clean
check: test_bench_selftest
	./test_bench_selftest

clean:
	rm -f test_bench_selftest
