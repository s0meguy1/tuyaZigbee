# Focused Build 19 source-contract checks. This is intentionally host-only:
# it verifies the firmware wiring and attribute tables but cannot prove PWM
# output; that requires visual LED confirmation or direct PWM-register reads.

.PHONY: check
check: test_onoff test_level
	python3 test_build19_source.py
	./test_onoff
	./test_level

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -O1 -g
CPPFLAGS = -I. -Ionoff_shim -D__PROJECT_TL_DIMMABLE_LIGHT__=1

test_onoff: test_onoff.c ../../light/zcl_onOffCb.c ../../light/tuyaLight.h \
            ../../light/tuyaLightCtrl.h onoff_shim/tl_common.h \
            onoff_shim/zb_api.h onoff_shim/zcl_include.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_onoff.c ../../light/zcl_onOffCb.c

test_level: test_level.c ../../light/zcl_levelCb.c ../../light/tuyaLight.h \
            ../../light/tuyaLightCtrl.h onoff_shim/tl_common.h \
            onoff_shim/zb_api.h onoff_shim/zcl_include.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -DZCL_LEVEL_CTRL=1 -o $@ test_level.c ../../light/zcl_levelCb.c

.PHONY: clean
clean:
	rm -f test_onoff test_level
