/* Executes the REAL light/moes_color.c - not a reimplementation of it.
 *
 * Build 20 made XY colour commands actually reach the LEDs by converting CIE
 * xy into the HSV pair the output stage renders. That conversion is integer
 * arithmetic with a matrix, a divide by y, and clamps: precisely the kind of
 * code that is easy to get subtly wrong and painful to debug on a ceiling
 * fixture. These checks pin it down on the host instead. */
#include <stdio.h>
#include "../../light/moes_color.h"

static int failures;

#define XY(v) ((unsigned short)((v) * 65536.0))

/* ZCL hue is 0..0xFE across 0..360 degrees. */
#define DEG(h) ((int)(((long)(h) * 360) / MOES_COLOR_HUE_MAX))

static void check(const char *what, int cond)
{
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

/* Hue is circular: 358 degrees and 2 degrees are 4 apart, not 356. */
static int hue_delta_deg(int a, int b)
{
    int d = a - b;
    if (d < 0) d = -d;
    return (d > 180) ? (360 - d) : d;
}

static void expect_hue(const char *name, double x, double y,
                       int wantDeg, int tolDeg, int minSat)
{
    unsigned char hue = 0xAA, sat = 0xAA;
    int gotDeg, delta;

    moes_xyToHueSat(XY(x), XY(y), &hue, &sat);
    gotDeg = DEG(hue);
    delta  = hue_delta_deg(gotDeg, wantDeg);

    printf("  %-22s xy=(%.4f,%.4f) -> hue=%3u (%3d deg) sat=%3u\n",
           name, x, y, hue, gotDeg, sat);
    check(name, delta <= tolDeg);
    if (sat < minSat) { printf("  FAIL: %s saturation %u < %d\n", name, sat, minSat); failures++; }
}

int main(void)
{
    unsigned char hue, sat;
    unsigned long x, y;

    printf("=== primaries map to the right hue ===\n");
    /* sRGB primaries, CIE 1931. Tolerance is generous because the output stage
     * only has 8-bit hue and the matrix runs in per-mille fixed point. */
    expect_hue("sRGB red",    0.6400, 0.3300,   0, 20, 200);
    expect_hue("sRGB green",  0.3000, 0.6000, 120, 20, 200);
    expect_hue("sRGB blue",   0.1500, 0.0600, 240, 20, 200);
    /* The exact value Home Assistant sent when the user picked red (19:15:47). */
    expect_hue("HA red pick", 0.7347, 0.2653,   0, 20, 200);

    printf("=== D65 white is nearly unsaturated ===\n");
    moes_xyToHueSat(XY(0.3127), XY(0.3290), &hue, &sat);
    printf("  D65 white              -> hue=%3u sat=%3u\n", hue, sat);
    check("D65 white must be low saturation", sat < 40);

    printf("=== degenerate inputs must not divide by zero or wrap ===\n");
    moes_xyToHueSat(0, 0, &hue, &sat);
    printf("  x=0 y=0                -> hue=%3u sat=%3u\n", hue, sat);
    check("x=0,y=0 -> achromatic", hue == 0 && sat == 0);

    moes_xyToHueSat(XY(0.5), 0, &hue, &sat);
    printf("  y=0                    -> hue=%3u sat=%3u\n", hue, sat);
    check("y=0 -> achromatic", hue == 0 && sat == 0);

    moes_xyToHueSat(0xFFFF, 0xFFFF, &hue, &sat);
    printf("  x=y=0xFFFF             -> hue=%3u sat=%3u\n", hue, sat);
    check("x=y=max stays in range", hue <= MOES_COLOR_HUE_MAX && sat <= MOES_COLOR_SAT_MAX);

    printf("=== exhaustive sweep: outputs always in ZCL range ===\n");
    /* Every command that can arrive over the air, at 257-step resolution.
     * Guards against overflow in the matrix and a hue that wraps past 0xFE. */
    for (x = 0; x <= 0xFFFF; x += 257) {
        for (y = 0; y <= 0xFFFF; y += 257) {
            hue = 0xFF; sat = 0xFF;
            moes_xyToHueSat((unsigned short)x, (unsigned short)y, &hue, &sat);
            if (hue > MOES_COLOR_HUE_MAX || sat > MOES_COLOR_SAT_MAX) {
                printf("  FAIL: xy=(%lu,%lu) -> hue=%u sat=%u out of range\n", x, y, hue, sat);
                failures++;
                goto done;
            }
        }
    }
    printf("  65536 combinations, all within 0..%u\n", MOES_COLOR_HUE_MAX);
done:

    printf("=== show colour units: degrees/percent -> ZCL (build 27) ===\n");
    {
        unsigned int d;
        unsigned char prev;
        int nonmono = 0;

        printf("    0deg -> %u   120deg -> %u   240deg -> %u   359deg -> %u\n",
               moes_hueDegToZcl(0), moes_hueDegToZcl(120),
               moes_hueDegToZcl(240), moes_hueDegToZcl(359));
        printf("    0%% -> %u    50%% -> %u    100%% -> %u\n",
               moes_satPctToZcl(0), moes_satPctToZcl(50), moes_satPctToZcl(100));

        /* The wheel must close: 360 is the same colour as 0, not a wrap to
         * 0xFE, or a show ramping hue through a full turn would jump. */
        if (moes_hueDegToZcl(360) != moes_hueDegToZcl(0)) {
            printf("  FAIL: 360deg does not wrap onto 0deg\n"); failures++;
        }
        if (moes_hueDegToZcl(0) != 0) { printf("  FAIL: 0deg is not 0\n"); failures++; }
        if (moes_satPctToZcl(0) != 0) { printf("  FAIL: 0%% is not 0\n"); failures++; }
        if (moes_satPctToZcl(100) != MOES_COLOR_SAT_MAX) {
            printf("  FAIL: 100%% is not full saturation\n"); failures++;
        }
        if (moes_satPctToZcl(200) != moes_satPctToZcl(100)) {
            printf("  FAIL: saturation does not clamp\n"); failures++;
        }

        /* Monotonic and in range across every reachable input. */
        prev = moes_hueDegToZcl(0);
        for (d = 1; d < 360; d++) {
            unsigned char h2 = moes_hueDegToZcl((unsigned short)d);
            if (h2 < prev) { nonmono++; }
            if (h2 > MOES_COLOR_HUE_MAX) {
                printf("  FAIL: %udeg -> %u out of range\n", d, h2); failures++; break;
            }
            prev = h2;
        }
        printf("    degrees 0..359, non-monotonic steps: %d\n", nonmono);
        if (nonmono) { printf("  FAIL: hue mapping is not monotonic\n"); failures++; }

        for (d = 0; d <= 100; d++) {
            if (moes_satPctToZcl((unsigned char)d) > MOES_COLOR_SAT_MAX) {
                printf("  FAIL: %u%% out of range\n", d); failures++; break;
            }
        }
    }

    if (failures) { printf("\n%d FAILING check(s)\n", failures); return 1; }
    printf("\nall colour conversion checks passed (real moes_color.c)\n");
    return 0;
}
