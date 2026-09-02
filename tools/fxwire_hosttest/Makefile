# Executes the real light/moes_fxwire.c on the host. That file is deliberately
# free of SDK/ZCL/hardware dependencies so this needs no shim at all.

.PHONY: check
check: test_fxwire
	./test_fxwire

CC     ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1 -g

test_fxwire: test_fxwire.c ../../light/moes_fxwire.c ../../light/moes_fxwire.h
	$(CC) $(CFLAGS) -o $@ test_fxwire.c ../../light/moes_fxwire.c

.PHONY: clean
clean:
	rm -f test_fxwire
