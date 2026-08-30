# Executes the real light/moes_color.c on the host. That file is deliberately
# free of SDK/ZCL/hardware dependencies so this needs no shim at all.

.PHONY: check
check: test_color
	./test_color

CC     ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1 -g

test_color: test_color.c ../../light/moes_color.c ../../light/moes_color.h
	$(CC) $(CFLAGS) -o $@ test_color.c ../../light/moes_color.c

.PHONY: clean
clean:
	rm -f test_color
