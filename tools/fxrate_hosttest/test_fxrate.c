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
			on1   += moes_fxBurstActive(t, 1,   0) ? 1 : 0;
			on50  += moes_fxBurstActive(t, 50,  0) ? 1 : 0;
			on100 += moes_fxBurstActive(t, 100, 0) ? 1 : 0;
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
			int first = moes_fxBurstActive(base, 50, 0);
			unsigned int off;
			for(off = 0; off < MOES_FX_BURST_SLOT_MS; off += MOES_FX_TICK_MS){
				if(moes_fxBurstActive(base + off, 50, 0) != first){
					unstable++;
					break;
				}
			}
		}
		printf("  500 slots sampled every tick, unstable slots: %d\n", unstable);
		check("the burst decision never changes inside a slot", unstable == 0);
	}

	printf("=== burst is deterministic, and phase decorrelates fixtures ===\n");
	{
		int same = 1, agree = 0;
		unsigned int i;
		for(i = 0; i < 1000u; i++){
			unsigned int t = i * MOES_FX_BURST_SLOT_MS;
			if(moes_fxBurstActive(t, 50, 0) != moes_fxBurstActive(t, 50, 0)){ same = 0; }
			/* Neighbouring phases must not merely shift the same sequence:
			 * two fixtures one degree apart have to burst independently. */
			if(moes_fxBurstActive(t, 50, 0) == moes_fxBurstActive(t, 50, 1)){ agree++; }
		}
		printf("  phase 0 vs phase 1 agree on %u/1000 slots (chance ~85%% at this density)\n", (unsigned)agree);
		check("same inputs give the same answer", same);
		check("a different phase gives a different sequence", agree < 1000);
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
