# Executes the real light/moes_dim.c on the host. That file is deliberately
# free of SDK/ZCL/hardware dependencies so this needs no shim at all.
#
# This is the build-38 companion to build19_hosttest: that one proves the ramp
# TIMING through the real zcl_levelCb.c, this one proves the output
# ARITHMETIC. Neither can prove PWM; only an eye on a fixture can.

.PHONY: check
check: test_level_curve
	./test_level_curve

CC     ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1 -g

test_level_curve: test_level_curve.c ../../light/moes_dim.c ../../light/moes_dim.h
	$(CC) $(CFLAGS) -o $@ test_level_curve.c ../../light/moes_dim.c

.PHONY: clean
clean:
	rm -f test_level_curve
