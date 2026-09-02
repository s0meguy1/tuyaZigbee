/* Executes the real light/moes_fxrate.c, not a host-side reimplementation. */
#include <stdio.h>

#include "../../light/moes_fxrate.h"

static int failures;

static void check(const char *what, int condition)
{
	if(!condition){
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

int main(void)
{
	unsigned int prev;
	int speed;

	printf("=== strobe is mapped by frequency and is actually renderable ===\n");
	{
		unsigned int slow = moes_fxStrobePeriodMs(1);
		unsigned int mid  = moes_fxStrobePeriodMs(50);
		unsigned int fast = moes_fxStrobePeriodMs(100);

		printf("  speed   1 -> %5u ms (%2u.%u Hz)\n", slow, 1000u / slow, (10000u / slow) % 10u);
		printf("  speed  50 -> %5u ms (%2u.%u Hz)\n", mid,  1000u / mid,  (10000u / mid)  % 10u);
		printf("  speed 100 -> %5u ms (%2u.%u Hz)\n", fast, 1000u / fast, (10000u / fast) % 10u);

		/* The bug this replaces: fxPeriod(1000) gave 50000 ms at speed 1 and
		 * 500 ms at speed 100, i.e. 0.02 Hz .. 2 Hz, while the code claimed
		 * 2 Hz .. 20 Hz. Pin the real range so it cannot silently regress. */
		check("speed 1 is ~1 Hz, not 0.02 Hz", slow >= 900u && slow <= 1100u);
		check("speed 100 reaches at least 10 Hz", fast <= 100u);
		check("speed 100 is faster than speed 1", fast < slow);
		check("speed 50 sits between the extremes", mid < slow && mid > fast);
	}

	printf("=== every strobe period is renderable at the tick rate ===\n");
	{
		int aliased = 0;
		for(speed = 0; speed <= 255; speed++){
			unsigned int p = moes_fxStrobePeriodMs((unsigned char)speed);
			/* A square wave needs a full tick on and a full tick off. */
			if(p < MOES_FX_MIN_PERIOD_MS || (p >> 1) < MOES_FX_TICK_MS){
				aliased++;
			}
		}
		printf("  all 256 speed values, aliasing periods: %d\n", aliased);
		check("no speed produces an unrenderable strobe period", aliased == 0);
	}

	printf("=== strobe is monotonic: more speed is never slower ===\n");
	{
		int violations = 0;
		prev = moes_fxStrobePeriodMs(1);
		for(speed = 2; speed <= 100; speed++){
			unsigned int p = moes_fxStrobePeriodMs((unsigned char)speed);
			if(p > prev){
				violations++;
			}
			prev = p;
		}
		printf("  speeds 1..100, non-monotonic steps: %d\n", violations);
		check("strobe period never increases with speed", violations == 0);
	}

	printf("=== speed guards match the firmware's own ===\n");
	{
		check("speed 0 is treated as the 50 default",
			  moes_fxStrobePeriodMs(0) == moes_fxStrobePeriodMs(50));
		check("speed above 100 clamps rather than wrapping",
			  moes_fxStrobePeriodMs(200) == moes_fxStrobePeriodMs(100));
		check("fade speed 0 is treated as the 50 default",
			  moes_fxPeriodMs(10000u, 0) == moes_fxPeriodMs(10000u, 50));
	}

	printf("=== fade periods keep their original behaviour ===\n");
	{
		/* These are unchanged by this work and are pinned so the refactor into
		 * moes_fxrate.c cannot have altered them: (slowMs * 100) / (2 * speed). */
		check("rainbow at speed 50 is a 10 s cycle", moes_fxPeriodMs(10000u, 50) == 10000u);
		check("rainbow at speed 100 is a 5 s cycle", moes_fxPeriodMs(10000u, 100) == 5000u);
		check("pulse at speed 50 is a 4 s cycle",    moes_fxPeriodMs(4000u, 50)  == 4000u);
		check("wave at speed 25 is a 12 s cycle",    moes_fxPeriodMs(6000u, 25)  == 12000u);
		check("a period is never zero",              moes_fxPeriodMs(1u, 100)    >= 1u);
	}

	printf("=== fade periods are monotonic too ===\n");
	{
		int violations = 0;
		prev = moes_fxPeriodMs(10000u, 1);
		for(speed = 2; speed <= 100; speed++){
			unsigned int p = moes_fxPeriodMs(10000u, (unsigned char)speed);
			if(p > prev){
				violations++;
			}
			prev = p;
		}
		printf("  speeds 1..100, non-monotonic steps: %d\n", violations);
		check("fade period never increases with speed", violations == 0);
	}

	printf("=== burst strobe: mostly dark, and denser as speed rises ===\n");
	{
		/* One hour of slots at each speed. */
		const unsigned int SPAN = 3600000u;
		unsigned int on1 = 0, on50 = 0, on100 = 0, slots = 0;
		unsigned int t;
		for(t = 0; t < SPAN; t += MOES_FX_BURST_SLOT_MS){
			slots++;
			on1   += moes_fxBurstActive(t, moes_fxBurstThreshold(1),   0) ? 1 : 0;
			on50  += moes_fxBurstActive(t, moes_fxBurstThreshold(50),  0) ? 1 : 0;
			on100 += moes_fxBurstActive(t, moes_fxBurstThreshold(100), 0) ? 1 : 0;
		}
		printf("  speed   1 -> %2u%% of slots burst\n", on1   * 100u / slots);
		printf("  speed  50 -> %2u%% of slots burst\n", on50  * 100u / slots);
		printf("  speed 100 -> %2u%% of slots burst\n", on100 * 100u / slots);

		/* The whole point of the effect is darkness with sparks in it. If any
		 * speed ever lights more than half the slots it stops reading as
		 * failing electronics and starts reading as a disco. */
		check("speed 1 is sparse", on1 * 100u / slots <= 8u);
		check("speed 50 stays mostly dark", on50 * 100u / slots <= 30u);
		check("even speed 100 is mostly dark", on100 * 100u / slots <= 50u);
		check("more speed means more bursts", on1 < on50 && on50 < on100);
		check("speed 1 still bursts sometimes", on1 > 0u);
	}

	printf("=== burst is stable inside a slot, not per-tick flicker ===\n");
	{
		/* A burst must be one coherent flash. If the decision changed within a
		 * slot the strobe would stutter in and out at the tick rate. */
		int unstable = 0;
		unsigned int slot;
		for(slot = 0; slot < 500u; slot++){
			unsigned int base = slot * MOES_FX_BURST_SLOT_MS;
			int first = moes_fxBurstActive(base, moes_fxBurstThreshold(50), 0);
			unsigned int off;
			for(off = 0; off < MOES_FX_BURST_SLOT_MS; off += MOES_FX_TICK_MS){
				if(moes_fxBurstActive(base + off, moes_fxBurstThreshold(50), 0) != first){
					unstable++;
					break;
				}
			}
		}
		printf("  500 slots sampled every tick, unstable slots: %d\n", unstable);
		check("the burst decision never changes inside a slot", unstable == 0);
	}

	printf("=== burst is deterministic, and the seed decorrelates fixtures ===\n");
	{
		int same = 1, agree = 0;
		unsigned int i;
		unsigned char th = moes_fxBurstThreshold(50);
		for(i = 0; i < 1000u; i++){
			unsigned int t = i * MOES_FX_BURST_SLOT_MS;
			if(moes_fxBurstActive(t, th, 0) != moes_fxBurstActive(t, th, 0)){ same = 0; }
			/* Neighbouring seeds must not merely shift the same sequence:
			 * two fixtures whose seeds differ by one have to burst independently. */
			if(moes_fxBurstActive(t, th, 0) == moes_fxBurstActive(t, th, 1)){ agree++; }
		}
		printf("  seed 0 vs seed 1 agree on %u/1000 slots (chance ~85%% at this density)\n", (unsigned)agree);
		check("same inputs give the same answer", same);
		check("a different seed gives a different sequence", agree < 1000);
	}

	printf("=== burst density is its own control (build 36) ===\n");
	{
		const unsigned int SPAN = 3600000u;
		unsigned int slots = 0, d1 = 0, d35 = 0, d70 = 0, d100 = 0, t;
		for(t = 0; t < SPAN; t += MOES_FX_BURST_SLOT_MS){
			slots++;
			d1   += moes_fxBurstActive(t, moes_fxDensityThreshold(1),   3) ? 1 : 0;
			d35  += moes_fxBurstActive(t, moes_fxDensityThreshold(35),  3) ? 1 : 0;
			d70  += moes_fxBurstActive(t, moes_fxDensityThreshold(70),  3) ? 1 : 0;
			d100 += moes_fxBurstActive(t, moes_fxDensityThreshold(100), 3) ? 1 : 0;
		}
		printf("  density   1 -> %3u%% of slots\n", d1 * 100u / slots);
		printf("  density  35 -> %3u%% of slots\n", d35 * 100u / slots);
		printf("  density  70 -> %3u%% of slots\n", d70 * 100u / slots);
		printf("  density 100 -> %3u%% of slots\n", d100 * 100u / slots);
		/* The field complaint: burst documented at ~35% of slots read as ~9%
		 * duty on a wattmeter because a flash is one tick of an 83 ms period.
		 * Density now spans the whole range so a texture is reachable. */
		check("density 35 lands near 35% of slots", d35 * 100u / slots >= 30u && d35 * 100u / slots <= 40u);
		check("density 70 lands near 70% of slots", d70 * 100u / slots >= 65u && d70 * 100u / slots <= 75u);
		check("density 100 is every slot", d100 == slots);
		check("density 1 is sparse but present", d1 > 0u && d1 * 100u / slots <= 3u);
		check("more density means more bursts", d1 < d35 && d35 < d70 && d70 < d100);
		check("speed-derived threshold at speed 100 matches ~35% density",
			  moes_fxBurstThreshold(100) == MOES_FX_BURST_MAX_THRESH);
	}

	printf("=== phase is a real offset into the period (build 36) ===\n");
	{
		check("180 degrees on a 4000 ms period is 2000 ms", moes_fxPhaseOffsetMs(4000u, 180u) == 2000u);
		check("0 degrees is no offset", moes_fxPhaseOffsetMs(4000u, 0u) == 0u);
		check("360 wraps to 0", moes_fxPhaseOffsetMs(4000u, 360u) == 0u);
		check("359 degrees is just under a period", moes_fxPhaseOffsetMs(4000u, 359u) == 3988u);
		/* The longest period any renderer uses: rainbow (10 s at speed 50) at
		 * speed 1 = 500 000 ms. Must not overflow. */
		check("longest period at 359 degrees does not overflow",
			  moes_fxPhaseOffsetMs(moes_fxPeriodMs(10000u, 1), 359u) == 498611u);
		check("no index: effective phase is the own phase", moes_fxEffectivePhase(30u, 0xFFu, 19u) == 30u);
		check("index 0 adds nothing", moes_fxEffectivePhase(30u, 0u, 19u) == 30u);
		check("index 18, spread 19 = 342 + own 30 wraps to 12", moes_fxEffectivePhase(30u, 18u, 19u) == 12u);
		check("index 254, spread 359 stays inside 0..359", moes_fxEffectivePhase(359u, 254u, 359u) < 360u);
		{
			/* Nineteen fixtures at spread 360/19 must land on nineteen distinct
			 * phases spanning the wheel: that is the whole point of a chase. */
			unsigned int seen[19], i, j, distinct = 1;
			for(i = 0; i < 19u; i++){ seen[i] = moes_fxEffectivePhase(0u, (unsigned char)i, 19u); }
			for(i = 0; i < 19u; i++){ for(j = i + 1; j < 19u; j++){ if(seen[i] == seen[j]){ distinct = 0; } } }
			check("19 indexed fixtures at spread 19 get 19 distinct phases", distinct);
			check("the last one sits at 342 degrees", seen[18] == 342u);
		}
	}

	printf("=== level fade is linear, monotonic and lands exactly (build 36) ===\n");
	{
		int mono = 1;
		unsigned int e;
		unsigned char prevL = moes_fxFadeLevel(0, 254, 0, 3000u);
		check("start of a fade is the origin", prevL == 0);
		check("end of a fade is the target", moes_fxFadeLevel(0, 254, 3000u, 3000u) == 254);
		check("past the end stays on the target", moes_fxFadeLevel(0, 254, 9000u, 3000u) == 254);
		check("half way is half", moes_fxFadeLevel(0, 254, 1500u, 3000u) == 127);
		check("a fade of 0 ms is a cut", moes_fxFadeLevel(0, 254, 0, 0) == 254);
		check("fading down works", moes_fxFadeLevel(254, 0, 1500u, 3000u) == 127);
		check("fade down ends at the target", moes_fxFadeLevel(254, 0, 3000u, 3000u) == 0);
		check("the maximum fade length does not overflow", moes_fxFadeLevel(0, 254, 65534u, 65535u) == 254);
		for(e = MOES_FX_TICK_MS; e <= 3000u; e += MOES_FX_TICK_MS){
			unsigned char L = moes_fxFadeLevel(0, 254, e, 3000u);
			if(L < prevL){ mono = 0; }
			prevL = L;
		}
		check("a fade never steps backwards at the tick rate", mono);
	}

	printf("=== burst flash is a spark, not a blink, and is renderable ===\n");
	{
		unsigned int p = moes_fxStrobePeriodMs(100);
		printf("  full-rate period %u ms, flash on-time %u ms (%u%% duty)\n",
			   p, MOES_FX_BURST_FLASH_MS, MOES_FX_BURST_FLASH_MS * 100u / p);
		/* One tick is the floor: the renderer cannot light anything shorter. */
		check("flash is at least one tick", MOES_FX_BURST_FLASH_MS >= MOES_FX_TICK_MS);
		check("flash is a whole number of ticks", MOES_FX_BURST_FLASH_MS % MOES_FX_TICK_MS == 0u);
		/* The bug this replaces: 50%% duty at 83 ms was ~41 ms of light. */
		check("flash is well under the old 50% duty", MOES_FX_BURST_FLASH_MS * 2u < p);
		check("flash still leaves a dark gap", MOES_FX_BURST_FLASH_MS < p);
	}

	printf("=== explosion ramp: faster at higher speed, and bounded ===\n");
	{
		unsigned int slow = moes_fxExplodeRampMs(1);
		unsigned int mid  = moes_fxExplodeRampMs(50);
		unsigned int fast = moes_fxExplodeRampMs(100);
		int violations = 0;
		printf("  speed   1 -> %4u ms swell\n", slow);
		printf("  speed  50 -> %4u ms\n", mid);
		printf("  speed 100 -> %4u ms flash\n", fast);
		/* Inverted against every other mapping here: more speed, SHORTER ramp. */
		check("more speed means a faster explosion", fast < mid && mid < slow);
		check("slowest is the documented max", slow == MOES_FX_EXPLODE_MAX_MS);
		check("fastest is the documented min", fast == MOES_FX_EXPLODE_MIN_MS);
		check("even the fastest ramp spans several ticks",
			  fast >= 10u * MOES_FX_TICK_MS);
		prev = moes_fxExplodeRampMs(1);
		for(speed = 2; speed <= 100; speed++){
			unsigned int r = moes_fxExplodeRampMs((unsigned char)speed);
			if(r > prev){ violations++; }
			prev = r;
		}
		printf("  speeds 1..100, non-monotonic steps: %d\n", violations);
		check("explosion ramp is monotonic", violations == 0);
		check("speed 0 is treated as the 50 default",
			  moes_fxExplodeRampMs(0) == moes_fxExplodeRampMs(50));
		check("speed above 100 clamps", moes_fxExplodeRampMs(200) == moes_fxExplodeRampMs(100));
	}

	if(failures){
		printf("\n%d FAILING check(s)\n", failures);
		return 1;
	}
	printf("\nall effect-rate checks passed (real moes_fxrate.c)\n");
	return 0;
}
