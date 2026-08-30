# Focused Build 19 source-contract checks. This is intentionally host-only:
# it verifies the firmware wiring and attribute tables but cannot prove PWM
# output; that requires visual LED confirmation or direct PWM-register reads.

.PHONY: check
check: test_onoff
	python3 test_build19_source.py
	./test_onoff

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -O1 -g
CPPFLAGS = -I. -Ionoff_shim -D__PROJECT_TL_DIMMABLE_LIGHT__=1

test_onoff: test_onoff.c ../../light/zcl_onOffCb.c ../../light/tuyaLight.h \
            ../../light/tuyaLightCtrl.h onoff_shim/tl_common.h \
            onoff_shim/zb_api.h onoff_shim/zcl_include.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_onoff.c ../../light/zcl_onOffCb.c

.PHONY: clean
clean:
	rm -f test_onoff
