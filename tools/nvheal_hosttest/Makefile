# Compile and execute the real first-boot NV ownership/heal implementation on
# the host. Flash and the post-stack NV API are simulated in test_nvheal.c.

.PHONY: check clean

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -O1 -g
CPPFLAGS = -I. -Ishim -D__PROJECT_TL_DIMMABLE_LIGHT__=1 \
           -DMCU_CORE_8258=1 -DBUILD_TS0505B=1

check: test_nvheal
	./test_nvheal

test_nvheal: test_nvheal.c ../../light/moes_nvheal.c \
             ../../light/moes_nvheal.h ../../light/moes_nvitems.h \
             shim/tl_common.h shim/zb_api.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_nvheal.c ../../light/moes_nvheal.c

clean:
	rm -f test_nvheal
