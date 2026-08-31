# Executes the real light/moes_fxrate.c on the host. That file is deliberately
# free of SDK/ZCL/hardware dependencies so this needs no shim at all.

.PHONY: check
check: test_fxrate
	./test_fxrate

CC     ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1 -g

test_fxrate: test_fxrate.c ../../light/moes_fxrate.c ../../light/moes_fxrate.h
	$(CC) $(CFLAGS) -o $@ test_fxrate.c ../../light/moes_fxrate.c

.PHONY: clean
clean:
	rm -f test_fxrate
