/* Executes the real light/moes_fxwire.c, not a host-side reimplementation.
 *
 * The frames below are built the way zigbee-herdsman builds them
 * (writeListTuyaDataPointValues: dp, type, len16 BE, data; sendDataPointValue
 * sends 4-byte big-endian values, sendDataPointEnum 1 byte) so a mismatch
 * between what z2m emits and what the chip reads shows up here, not on the
 * ceiling. */
#include <stdio.h>
#include <string.h>

#include "../../light/moes_fxwire.h"

#define EF_MAX 15   /* MOES_EF_MAX in build 36: 0..14 */

static int failures;

static void check(const char *what, int condition)
{
	if(!condition){
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

/* --- frame builder, mirrors herdsman --- */
static unsigned char frame[256];
static unsigned int flen;

static void fstart(unsigned short seq)
{
	flen = 0;
	frame[flen++] = (unsigned char)(seq >> 8);
	frame[flen++] = (unsigned char)seq;
}

static void fdp(unsigned char dp, unsigned char type, const unsigned char *data, unsigned int n)
{
	frame[flen++] = dp;
	frame[flen++] = type;
	frame[flen++] = (unsigned char)(n >> 8);
	frame[flen++] = (unsigned char)n;
	memcpy(frame + flen, data, n);
	flen += n;
}

static void fvalue(unsigned char dp, unsigned int v)   /* sendDataPointValue: 4 bytes BE */
{
	unsigned char d[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
	fdp(dp, MOES_DPT_VALUE, d, 4);
}

static void fenum(unsigned char dp, unsigned char v)   /* sendDataPointEnum: 1 byte */
{
	fdp(dp, MOES_DPT_ENUM, &v, 1);
}

/* --- cue-load capture --- */
static unsigned char cueStart, cueN, cueCalls;
static unsigned char cueBytes[MOES_FX_CUE_MAX * MOES_FX_CUE_WIRE_LEN];
static int cueAccept = 1;

static int cueFn(void *ctx, unsigned char start, const unsigned char *entries, unsigned char n)
{
	(void)ctx;
	cueCalls++;
	cueStart = start;
	cueN = n;
	memcpy(cueBytes, entries, (size_t)n * MOES_FX_CUE_WIRE_LEN);
	return cueAccept;
}

static void cueEntry(unsigned char *w, unsigned short t, unsigned char effect, unsigned char speed,
                     unsigned short hue, unsigned char sat, unsigned char level, unsigned char fade)
{
	w[0] = (unsigned char)(t >> 8); w[1] = (unsigned char)t;
	w[2] = effect; w[3] = speed;
	w[4] = (unsigned char)(hue >> 8); w[5] = (unsigned char)hue;
	w[6] = sat; w[7] = level; w[8] = fade;
}

int main(void)
{
	moes_fxFrame_t f;
	moes_fxWireStatus_e st;

	printf("=== a build-27 single-datapoint frame still parses exactly as before ===\n");
	{
		fstart(7); fenum(MOES_DP_EFFECT, 12);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("effect enum frame is OK", st == MOES_FXW_OK);
		check("only the effect bit is present", f.present == MOES_FXF_EFFECT);
		check("effect is burst (12)", f.effect == 12);

		fstart(8); fvalue(MOES_DP_SPEED, 73);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("4-byte value frame is OK", st == MOES_FXW_OK && f.present == MOES_FXF_SPEED && f.speed == 73);

		fstart(9); fvalue(MOES_DP_HUE, 220);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("hue 220 explicit", st == MOES_FXW_OK && f.hue == 220);
	}

	printf("=== several datapoints in one frame land as one atomic unit ===\n");
	{
		fstart(10);
		fvalue(MOES_DP_DELAY, 400);
		fvalue(MOES_DP_LEVEL, 254);
		fvalue(MOES_DP_HUE, 30);
		fvalue(MOES_DP_SAT, 100);
		fvalue(MOES_DP_SPEED, 100);
		fenum(MOES_DP_EFFECT, 13);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("six-datapoint frame is OK", st == MOES_FXW_OK);
		check("all six bits present", f.present == (MOES_FXF_DELAY | MOES_FXF_LEVEL | MOES_FXF_HUE | MOES_FXF_SAT | MOES_FXF_SPEED | MOES_FXF_EFFECT));
		check("delay 400", f.delay == 400);
		check("level 254", f.level == 254);
		check("effect explode (13)", f.effect == 13);
		printf("  %u bytes on the wire for a complete armed cue\n", flen);
		check("a complete cue fits one unfragmented frame", flen <= 80);
	}

	printf("=== one bad value rejects the whole frame ===\n");
	{
		fstart(11);
		fenum(MOES_DP_EFFECT, 2);
		fvalue(MOES_DP_SPEED, 101);   /* out of range */
		memset(&f, 0xAA, sizeof f);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("speed 101 rejects as INVALID", st == MOES_FXW_INVALID);

		fstart(12); fenum(MOES_DP_EFFECT, EF_MAX);
		check("effect == MOES_EF_MAX rejects", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		fstart(13); fenum(MOES_DP_EFFECT, EF_MAX - 1);
		check("effect == MOES_EF_MAX-1 (solid) accepts", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_OK);

		fstart(14); fvalue(0x50, 1);   /* unknown id */
		check("unknown datapoint rejects (a newer converter fails loudly)", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);

		fstart(15); fvalue(MOES_DP_TAKEOVER, 2);
		check("takeover 2 rejects", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		fstart(16); fvalue(MOES_DP_CUE_RUN, 3);
		check("cue_run 3 rejects", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		fstart(17); fvalue(MOES_DP_DENSITY, 101);
		check("density 101 rejects", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		fstart(18); fvalue(MOES_DP_INDEX, 256);
		check("index 256 rejects", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		fstart(19); fvalue(MOES_DP_INDEX, 255);
		check("index 255 (none) accepts", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_OK && f.index == 255);
	}

	printf("=== sentinels ===\n");
	{
		fstart(20); fvalue(MOES_DP_HUE, 360);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("hue 360 means follow", st == MOES_FXW_OK && f.hue == MOES_FX_HUE_FOLLOW);
		fstart(21); fvalue(MOES_DP_HUE, 359);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("hue 359 is explicit", st == MOES_FXW_OK && f.hue == 359);
		fstart(22); fvalue(MOES_DP_LEVEL, 255);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("level 255 means follow", st == MOES_FXW_OK && f.level == MOES_FX_LEVEL_FOLLOW);
		fstart(23); fvalue(MOES_DP_LEVEL, 0);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("level 0 is an explicit blackout, not follow", st == MOES_FXW_OK && f.level == 0);
	}

	printf("=== malformed structure is rejected without reading past the end ===\n");
	{
		fstart(24);
		check("seq only is malformed", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_MALFORMED);
		fstart(25); fvalue(MOES_DP_SPEED, 50);
		check("a truncated value is malformed", moes_fxWireParse(frame, flen - 1, EF_MAX, &f, cueFn, NULL) == MOES_FXW_MALFORMED);
		/* The build-26 overflow: len16 0xFFFF must not wrap the bound. */
		fstart(26);
		frame[flen++] = MOES_DP_SPEED; frame[flen++] = MOES_DPT_VALUE; frame[flen++] = 0xFF; frame[flen++] = 0xFF;
		frame[flen++] = 50;
		check("len 0xFFFF is malformed, not a 4-byte read past the ASDU", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_MALFORMED);
		fstart(27);
		frame[flen++] = MOES_DP_SPEED; frame[flen++] = MOES_DPT_VALUE; frame[flen++] = 0; frame[flen++] = 0;
		check("a zero-length value is malformed", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_MALFORMED);
		check("NULL payload is malformed", moes_fxWireParse(NULL, 10, EF_MAX, &f, cueFn, NULL) == MOES_FXW_MALFORMED);
	}

	printf("=== cue-list upload ===\n");
	{
		unsigned char raw[1 + 3 * MOES_FX_CUE_WIRE_LEN];
		moes_fxCue_t c;

		raw[0] = 5;   /* start index */
		cueEntry(raw + 1, 0, 12, 80, 220, 100, 254, 0);
		cueEntry(raw + 1 + 9, 1450, 0xFF, 0, 0xFFFF, 0xFF, 0xFF, 0);
		cueEntry(raw + 1 + 18, 33500, 0, 0, 400, 0xFF, 0, 20);
		cueCalls = 0;
		fstart(28); fdp(MOES_DP_CUE_LOAD, MOES_DPT_RAW, raw, sizeof raw);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("a frame of only cue loads is EMPTY (nothing to apply)", st == MOES_FXW_EMPTY);
		check("the load callback ran once", cueCalls == 1);
		check("start index 5", cueStart == 5);
		check("three entries", cueN == 3);
		moes_fxCueDecode(cueBytes, &c);
		check("entry 0 decodes: t 0, burst, speed 80, hue 220, sat 100, level 254", c.tMs == 0 && c.effect == 12 && c.speed == 80 && c.hue == 220 && c.sat == 100 && c.level == 254 && c.fade == 0);
		moes_fxCueDecode(cueBytes + 9, &c);
		check("entry 1 decodes as keep-everything at 1450 ms", c.tMs == 1450 && c.effect == MOES_FX_KEEP8 && c.speed == 0 && c.hue == MOES_FX_KEEP16 && c.sat == MOES_FX_KEEP8 && c.level == MOES_FX_KEEP8);
		moes_fxCueDecode(cueBytes + 18, &c);
		check("entry 2 decodes: t 33500, stop, hue 400 (follow), level 0, fade 2 s", c.tMs == 33500 && c.effect == 0 && c.hue == 400 && c.level == 0 && c.fade == 20);
		printf("  three entries + header = %u bytes on the wire\n", flen);

		/* load + run in one frame: the load is applied during the parse, the
		 * run comes back in the frame */
		fstart(29); fdp(MOES_DP_CUE_LOAD, MOES_DPT_RAW, raw, sizeof raw); fenum(MOES_DP_CUE_RUN, 1);
		st = moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL);
		check("load + run is OK with cue_run present", st == MOES_FXW_OK && (f.present & MOES_FXF_CUE_RUN) && f.cueRun == 1);

		raw[0] = 30;   /* 30 + 3 > 32 */
		fstart(30); fdp(MOES_DP_CUE_LOAD, MOES_DPT_RAW, raw, sizeof raw);
		check("a load past the 32-entry table is INVALID", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		raw[0] = 0;
		fstart(31); fdp(MOES_DP_CUE_LOAD, MOES_DPT_RAW, raw, sizeof raw - 1);
		check("a load that is not whole entries is INVALID", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		cueAccept = 0;
		fstart(32); fdp(MOES_DP_CUE_LOAD, MOES_DPT_RAW, raw, sizeof raw);
		check("a load the engine refuses rejects the frame", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_INVALID);
		cueAccept = 1;
		fstart(33); fdp(MOES_DP_CUE_LOAD, MOES_DPT_RAW, raw, sizeof raw);
		check("a load with no callback rejects the frame", moes_fxWireParse(frame, flen, EF_MAX, &f, NULL, NULL) == MOES_FXW_INVALID);
		{
			/* Six entries per frame is what the converter sends: prove it fits. */
			unsigned char big[1 + 6 * MOES_FX_CUE_WIRE_LEN];
			memset(big, 0, sizeof big);
			fstart(34); fdp(MOES_DP_CUE_LOAD, MOES_DPT_RAW, big, sizeof big);
			printf("  six entries per upload frame = %u bytes on the wire\n", flen);
			check("six entries per frame stay under the group-broadcast payload", flen <= 80);
			check("six entries parse", moes_fxWireParse(frame, flen, EF_MAX, &f, cueFn, NULL) == MOES_FXW_EMPTY && cueN == 6);
		}
	}

	printf("=== report encodes every field and decodes with the same table ===\n");
	{
		unsigned char buf[MOES_FX_REPORT_MAX_LEN];
		moes_fxReport_t st0 = { 12, 80, 90, MOES_FX_HUE_FOLLOW, 100, 7, 19, 254, 1, 5000, 35, 2, 12 };
		moes_fxFrame_t back;
		unsigned int n = moes_fxWireReportBuild(buf, sizeof buf, 0x1234, &st0);

		printf("  full report = %u bytes\n", n);
		check("report builds", n > 0);
		check("report fits an unfragmented unicast (<= 80 bytes of ZCL payload)", n <= 80);
		check("seq is first, big-endian", buf[0] == 0x12 && buf[1] == 0x34);
		/* Everything but cue_count is a datapoint the parser knows, so the report
		 * must parse back through the same code path with the same values. Strip
		 * the trailing cue_count datapoint (5 bytes) for that round trip. */
		st = moes_fxWireParse(buf, n - 5, EF_MAX, &back, cueFn, NULL);
		check("report parses back", st == MOES_FXW_OK);
		check("effect round-trips", back.effect == 12);
		check("speed round-trips", back.speed == 80);
		check("phase round-trips", back.phase == 90);
		check("follow hue reports as 360 and parses back as follow", back.hue == MOES_FX_HUE_FOLLOW);
		check("index round-trips", back.index == 7);
		check("spread round-trips", back.spread == 19);
		check("level round-trips", back.level == 254);
		check("takeover round-trips", back.takeover == 1);
		check("duration round-trips", back.duration == 5000);
		check("density round-trips", back.density == 35);
		check("cue_run round-trips", back.cueRun == 2);
		check("cue_count is the last datapoint", buf[n - 5] == MOES_DP_CUE_COUNT && buf[n - 1] == 12);
		check("too small a buffer yields 0, never a truncated report", moes_fxWireReportBuild(buf, 40, 0, &st0) == 0);
		st0.hue = 220;
		n = moes_fxWireReportBuild(buf, sizeof buf, 1, &st0);
		st = moes_fxWireParse(buf, n - 5, EF_MAX, &back, cueFn, NULL);
		check("explicit hue round-trips", st == MOES_FXW_OK && back.hue == 220);
	}

	if(failures){
		printf("\n%d FAILING check(s)\n", failures);
		return 1;
	}
	printf("\nall wire-format checks passed (real moes_fxwire.c)\n");
	return 0;
}
