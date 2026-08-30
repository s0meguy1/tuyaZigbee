# Executes the real light/moes_eui.c on the host.  The parser is deliberately
# SDK-free so the factory display-order versus MAC-PIB byte order contract is
# tested without a device or a hardware shim.

.PHONY: check clean

CC     ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1 -g

check: test_eui
	./test_eui

test_eui: test_eui.c ../../light/moes_eui.c ../../light/moes_eui.h
	$(CC) $(CFLAGS) -o $@ test_eui.c ../../light/moes_eui.c

clean:
	rm -f test_eui
