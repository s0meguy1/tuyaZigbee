/*
 * Build 38. The render chain's arithmetic, executed on the host.
 *
 * The observation: a `transition:` fade on a TS0505B rendered as visible
 * steps. Two independent causes, both ours. The ramp only advanced ten times a
 * second (covered by tools/build19_hosttest/test_level.c), and the output
 * stage threw away almost all of its resolution before it reached the PWM
 * compare register - which is what this file covers.
 *
 * The old chain quantized three times: the colour-temperature split produced
 * u8 C and W, the brightness curve rounded through whole percents, and only
 * then was a duty computed. Measured over a full 254->0 fade at 230 mireds it
 * produced 78 distinct PWM values out of ~12000 available, and at the bottom
 * ZCL levels 3,4,5 and 6 all rendered at the SAME output. That is the jerk.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../../light/moes_dim.h"

/* 48 MHz system clock / 4 kHz PWM, as hwLight_init() computes it on this part
 * (CLOCK_SYS_CLOCK_HZ in common/comm_cfg.h for MCU_CORE_8258, and
 * MOES_PWM_FREQUENCY_DEFAULT in device_config/light_ts0505b.h). */
#define PWM_MAX_TICK        12000u

/* The fixtures' physical colour-temperature endpoints (tuyaLightEpCfg.c), and
 * the mired value the kitchen actually sits at. */
#define CT_MIN_MIREDS       153u
#define CT_MAX_MIREDS       500u
#define CT_KITCHEN_MIREDS   230u

#define ZCL_LEVEL_MIN       1
#define ZCL_LEVEL_MAX       254

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

/*
 * The pre-build-38 output chain, kept here as the reference the new one is
 * compared against. This is a copy of the code build 37 shipped
 * (moes_levelCurve/moes_duty/pwmSetDuty in tuyaLightCtrl.c) and exists only so
 * the "curve shape is unchanged" claim can be measured rather than asserted.
 */
static unsigned int legacy_curve(unsigned int v)
{
    unsigned int pct;

    if (v == 0) return 0;
    pct = MOES_BRIGHT_MIN_PCT + ((v * (MOES_BRIGHT_MAX_PCT - MOES_BRIGHT_MIN_PCT)) / 255u);
    return (pct * 255u) / 100u;
}

static unsigned int legacy_tick(unsigned int v)
{
    /* moes_duty() scaled by PWM_FULL_DUTYCYCLE(100) and pwmSetDuty() divided
     * by ZCL_LEVEL_ATTR_MAX_LEVEL(254) * 100. */
    return ((legacy_curve(v) * 100u) * PWM_MAX_TICK) / (254u * 100u);
}

static void legacy_split(unsigned int level, unsigned int *C, unsigned int *W)
{
    *W = ((CT_KITCHEN_MIREDS - CT_MIN_MIREDS) * level) / (CT_MAX_MIREDS - CT_MIN_MIREDS);
    *C = level - *W;
}

/* The build-38 chain, using the real firmware functions throughout. */
static unsigned int render_tick(unsigned short level256)
{
    unsigned short C256 = 0, W256 = 0;

    moes_dimSplitCW256(CT_KITCHEN_MIREDS, CT_MIN_MIREDS, CT_MAX_MIREDS, level256, &C256, &W256);
    return moes_dimCmpTick(moes_dimCurve256(C256), PWM_MAX_TICK);
}

/* ---------------------------------------------------------------- */

static void test_curve_is_monotone_and_hits_both_end_stops(void)
{
    unsigned int v;
    unsigned int prev = 0;

    check(moes_dimCurve256(0) == 0,
          "curve256(0) must be 0: brightmin is a floor for a LIT channel, not "
          "an output floor, and Off is driven through this same function");
    check(moes_dimCurve256(MOES_DIM_MAX256) == MOES_DIM_MAX256,
          "curve256(full) must be full scale");

    for (v = 1; v <= MOES_DIM_MAX256; v++) {
        unsigned int out = moes_dimCurve256(v);
        if (out < prev) {
            printf("  FAIL  curve256 not monotone at v=%u (%u < %u)\n", v, out, prev);
            failures++;
            return;
        }
        prev = out;
    }
}

static void test_curve_shape_is_unchanged(void)
{
    /* Section 3 of the brief: precision only, never shape. The wide curve is
     * the exact real-valued function the integer-percent code approximated, so
     * it differs from build 37's rendered output by at most 3/255 of full
     * scale. Anything larger would be a shape change and would move every
     * stored scene away from the stock fixtures in the same rooms. */
    unsigned int v;
    unsigned int worst = 0;

    for (v = 1; v <= 255; v++) {
        unsigned int wide = moes_dimCurve256(v * 256u) / 256u;
        unsigned int old  = legacy_curve(v);
        unsigned int diff = (wide > old) ? (wide - old) : (old - wide);

        if (diff > worst) worst = diff;
    }

    printf("  worst curve delta vs build 37: %u/255\n", worst);
    check(worst <= 3, "curve must stay within 3/255 of the build-37 output");
}

static void test_bottom_end_no_longer_collapses(void)
{
    /* THE defect that made a fade look worse at the end than at the start.
     * On build 37, at 230 mireds, levels 3,4,5,6 all produced compare tick 236
     * and levels 1,2 both produced 94, so the last second of a fade was four
     * visible jumps. Every whole level must now move the output. */
    int v;
    int legacy_collapses = 0;

    for (v = ZCL_LEVEL_MIN; v < 16; v++) {
        unsigned int a = render_tick((unsigned short)(v * 256));
        unsigned int b = render_tick((unsigned short)((v + 1) * 256));
        unsigned int lc, lw, la, lb;

        legacy_split((unsigned int)v, &lc, &lw);
        la = legacy_tick(lc);
        legacy_split((unsigned int)v + 1, &lc, &lw);
        lb = legacy_tick(lc);
        if (la == lb) legacy_collapses++;

        if (a >= b) {
            printf("  FAIL  levels %d and %d both render at tick %u\n", v, v + 1, a);
            failures++;
        }
    }

    printf("  adjacent-level collapses below level 16: build 37 had %d, now 0\n",
           legacy_collapses);
    check(legacy_collapses > 0,
          "the legacy reference must still reproduce the defect, or this test "
          "is no longer measuring anything");
}

/*
 * One tick of a ramp, mirroring light_applyUpdate() in tuyaLightCtrl.c.
 *
 * The real function is exercised against the real zcl_levelCb.c in
 * tools/build19_hosttest/test_level.c; it cannot be linked here because it
 * pulls in the whole ZCL attribute surface. What matters here is only that the
 * 8.8 accumulator this file consumes advances the way the firmware advances
 * it.
 */
static int fade_distinct_ticks(int total_steps, unsigned int *last, int last_len)
{
    long cur256 = (long)ZCL_LEVEL_MAX * 256;
    long step256 = (((long)ZCL_LEVEL_MIN - ZCL_LEVEL_MAX) * 256) / total_steps;
    int i;
    int distinct = 0;
    unsigned int prev = 0xFFFFFFFFu;
    unsigned int *seen = calloc((size_t)total_steps, sizeof(unsigned int));

    for (i = 0; i < total_steps; i++) {
        int j, dup = 0;
        unsigned int t;

        cur256 += step256;
        if (cur256 < (long)ZCL_LEVEL_MIN * 256) cur256 = (long)ZCL_LEVEL_MIN * 256;

        t = render_tick((unsigned short)cur256);
        for (j = 0; j < distinct; j++) if (seen[j] == t) { dup = 1; break; }
        if (!dup) seen[distinct++] = t;

        if (i >= total_steps - last_len) last[i - (total_steps - last_len)] = t;
        prev = t;
    }
    (void)prev;
    free(seen);
    return distinct;
}

/* The same fade rendered the way build 37 rendered it: 50 ticks, u8 chain. */
static int legacy_fade_distinct_ticks(int total_steps)
{
    long cur256 = (long)ZCL_LEVEL_MAX * 256;
    long step256 = (((long)ZCL_LEVEL_MIN - ZCL_LEVEL_MAX) * 256) / total_steps;
    int i, distinct = 0;
    unsigned int seen[512];

    for (i = 0; i < total_steps; i++) {
        unsigned int c, w, t;
        int j, dup = 0;

        cur256 += step256;
        if (cur256 < (long)ZCL_LEVEL_MIN * 256) cur256 = (long)ZCL_LEVEL_MIN * 256;

        legacy_split((unsigned int)(cur256 / 256), &c, &w);
        t = legacy_tick(c);
        for (j = 0; j < distinct; j++) if (seen[j] == t) { dup = 1; break; }
        if (!dup) seen[distinct++] = t;
    }
    return distinct;
}

static void test_five_second_fade_resolves(void)
{
    unsigned int last[12];
    int i;
    int now = fade_distinct_ticks(250, last, 12);
    int before = legacy_fade_distinct_ticks(50);

    printf("  5 s fade, distinct PWM values: build 37 = %d, build 38 = %d\n",
           before, now);
    printf("  last 12 compare ticks:");
    for (i = 0; i < 12; i++) printf(" %u", last[i]);
    printf("\n");

    /* Pin the old number so a regression is obvious rather than quiet. */
    check(before == 50, "the build-37 reference should still measure 50 distinct values");
    check(now >= 200, "a 5 s fade must resolve at least 200 distinct PWM values");
}

static void test_active_low_inversion_round_trips(void)
{
    unsigned int v;

    for (v = 0; v <= MOES_DIM_MAX256; v += 37) {
        check(moes_dimInvert256(v, 0) == v, "active-high must pass through");
        check(moes_dimInvert256(moes_dimInvert256(v, 1), 1) == v,
              "active-low inversion must round-trip in the wide domain");
    }
    check(moes_dimInvert256(0, 1) == MOES_DIM_MAX256, "active-low zero is full duty");
    check(moes_dimInvert256(MOES_DIM_MAX256, 1) == 0, "active-low full is zero duty");
}

static void test_cw_split_is_conservative(void)
{
    /* C + W must always reconstruct the level, at any mired value, or a fade
     * would change brightness as it changes colour. */
    unsigned int m;

    for (m = CT_MIN_MIREDS; m <= CT_MAX_MIREDS; m += 7) {
        unsigned int l;
        for (l = 0; l <= MOES_DIM_MAX256; l += 251) {
            unsigned short c = 0, w = 0;
            moes_dimSplitCW256((unsigned short)m, CT_MIN_MIREDS, CT_MAX_MIREDS,
                               (unsigned short)l, &c, &w);
            if ((unsigned int)c + w != l) {
                printf("  FAIL  split loses light: mireds=%u level=%u C=%u W=%u\n",
                       m, l, c, w);
                failures++;
                return;
            }
        }
    }

    /* A zero span would be a divide by zero, which on this part is a CPU hang
     * in the hardware divider's busy-wait, not a wrong answer. */
    {
        unsigned short c = 0, w = 0;
        moes_dimSplitCW256(300, 300, 300, 1000, &c, &w);
        check(c == 1000 && w == 0, "a zero mired span must not divide by zero");
    }
}

int main(void)
{
    printf("build 38 output-stage arithmetic (real light/moes_dim.c)\n");

    test_curve_is_monotone_and_hits_both_end_stops();
    test_curve_shape_is_unchanged();
    test_bottom_end_no_longer_collapses();
    test_five_second_fade_resolves();
    test_active_low_inversion_round_trips();
    test_cw_split_is_conservative();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all output-stage checks passed\n");
    return 0;
}
