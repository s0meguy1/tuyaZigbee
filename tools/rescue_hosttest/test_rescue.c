/********************************************************************************************************
 * @file    test_rescue.c
 *
 * @brief   Host-side test for the boot-probation / rescue-mode state machine
 *          and the two-layer wedge liveness monitor.
 *
 * This compiles the REAL light/moes_rescue.c and light/moes_liveness.c - not
 * copies, not models - against a shim that stands in for the handful of SDK
 * symbols they use (NV read/write, zb_isDeviceJoinedNwk, TL_ZB_TIMER_SCHEDULE,
 * drv_hwTmr, SYSTEM_RESET). Everything the firmware would do to flash becomes
 * a byte in this file, every TL_ZB_TIMER becomes a manual tick on a cooperative
 * "task" list, every drv_hwTmr becomes a manual tick on a separate "hw" list,
 * and every reset becomes a counter.
 *
 * The two timer lists are what make the build-09 liveness design testable: the
 * progress ticker lives on the task list (which host_wedge() can freeze), while
 * the reset sampler lives on the hw list (which keeps ticking through a wedge),
 * exactly like ev_timer vs drv_hwTmr on the device.
 *
 *     cd tools/rescue_hosttest && make && ./test_rescue
 *
 * @date    2026
 *******************************************************************************************************/

#include <stdio.h>
#include <string.h>
#include "shim/tl_common.h"
#include "../../light/moes_rescue.h"
#include "../../light/moes_liveness.h"

/* ------------------------------------------------------------------ */
/* Simulated NV                                                        */

#define SIM_NV_ITEM_PROBATION   0x71

static int  nv_present;          /* has the item ever been written? */
static u8   nv_value;
static int  nv_writes;           /* flash-wear counter for the whole run */
static int  nv_readFails;        /* force reads to fail */
static int  nv_writeFails;       /* force writes to fail */

nv_sts_t nv_flashReadNew(u8 single, u8 id, u8 itemId, u16 len, u8 *buf)
{
	(void)single; (void)len;
	if(id != NV_MODULE_APP || itemId != SIM_NV_ITEM_PROBATION){
		return NV_ITEM_NOT_FOUND;
	}
	if(nv_readFails || !nv_present){
		return NV_ITEM_NOT_FOUND;
	}
	*buf = nv_value;
	return NV_SUCC;
}

nv_sts_t nv_flashWriteNew(u8 single, u8 id, u8 itemId, u16 len, u8 *buf)
{
	(void)single; (void)len;
	if(id != NV_MODULE_APP || itemId != SIM_NV_ITEM_PROBATION){
		return NV_ITEM_NOT_FOUND;
	}
	nv_writes++;
	if(nv_writeFails){
		return NV_ITEM_NOT_FOUND;
	}
	nv_present = 1;
	nv_value = *buf;
	return NV_SUCC;
}

/* ------------------------------------------------------------------ */
/* Simulated network + two timer substrates                            */

static int joined;

bool zb_isDeviceJoinedNwk(void){ return joined ? TRUE : FALSE; }

/* Cooperative ev_timer list. The real pool has 24 entries; the units under
 * test own one each (rescue's minute clock, liveness' progress ticker). Four
 * slots is plenty. host_wedge() freezes this whole list, which is the honest
 * model of "an earlier ev_timer callback never returns, or ev_poll is stuck". */
#define SIM_TIMER_SLOTS 4

struct ev_timer_event_s { int live; };

static struct {
	ev_timer_event_t      evt;
	ev_timer_callback_t   cb;
	int                   live;
} sim_timers[SIM_TIMER_SLOTS];

static int timer_allocFails;

ev_timer_event_t *host_timerSchedule(ev_timer_callback_t cb, void *arg, u32 ms)
{
	(void)arg; (void)ms;
	if(timer_allocFails){
		return NULL;            /* the pool was full */
	}
	for(int i = 0; i < SIM_TIMER_SLOTS; i++){
		if(!sim_timers[i].live){
			sim_timers[i].live = 1;
			sim_timers[i].evt.live = 1;
			sim_timers[i].cb = cb;
			return &sim_timers[i].evt;
		}
	}
	return NULL;
}

static int sim_timerCount(void)
{
	int n = 0;
	for(int i = 0; i < SIM_TIMER_SLOTS; i++){
		if(sim_timers[i].live){
			n++;
		}
	}
	return n;
}

static int sim_anyTimerLive(void)
{
	return sim_timerCount() > 0;
}

/* The hardware-timer IRQ list (drv_hwTmr). Indexed by TIMER_IDX_* exactly like
 * the real driver, so drv_hwTmr_init/drv_hwTmr_set map 1:1. */
static struct {
	timerCb_t cb;
	void      *arg;
	int        live;
} sim_hwTimers[TIMER_NUM];

/* The raw Timer0/1/2 register surface the build-10 liveness callback writes
 * when it re-arms the sampler itself (chip_8258/timer.h: timer_stop,
 * timer_set_init_tick, timer_set_cap_tick, timer_start). This is the
 * observable the new scenario asserts on: every sample must drive a fresh
 * stop->init->capture->start, not a fire-and-forget callback. */
static struct {
	u32 initTick;
	u32 capTick;
	int enabled;
	int rearmCount;          /* timer_start() calls on this index */
} sim_hwRegs[TIMER_NUM];

void drv_hwTmr_init(u8 tmrIdx, u8 mode)
{
	(void)mode;
	if(tmrIdx >= TIMER_NUM){
		return;
	}
	memset(&sim_hwTimers[tmrIdx], 0, sizeof(sim_hwTimers[tmrIdx]));
	memset(&sim_hwRegs[tmrIdx], 0, sizeof(sim_hwRegs[tmrIdx]));
}

hw_timer_sts_t drv_hwTmr_set(u8 tmrIdx, u32 t_us, timerCb_t func, void *arg)
{
	if(tmrIdx >= TIMER_NUM){
		return HW_TIMER_INVALID;
	}
	/* Model the real hwTmr_setAbs() sequence: load init+capture, then start.
	 * t_us is still "period-agnostic" for the fire-N-times scheduler below,
	 * but recording the ticks lets the re-arm scenario prove the callback
	 * reloads the same capture each time. */
	sim_hwTimers[tmrIdx].cb   = func;
	sim_hwTimers[tmrIdx].arg  = arg;
	sim_hwTimers[tmrIdx].live = 1;

	sim_hwRegs[tmrIdx].initTick = 0;
	sim_hwRegs[tmrIdx].capTick  = t_us * TIMER_TICK_1US_GET(tmrIdx);
	sim_hwRegs[tmrIdx].enabled  = 1;
	sim_hwRegs[tmrIdx].rearmCount++;
	return HW_TIMER_SUCC;
}

void timer_set_init_tick(u8 tmrIdx, u32 initTick)
{
	if(tmrIdx < TIMER_NUM){
		sim_hwRegs[tmrIdx].initTick = initTick;
	}
}

void timer_set_cap_tick(u8 tmrIdx, u32 capTick)
{
	if(tmrIdx < TIMER_NUM){
		sim_hwRegs[tmrIdx].capTick = capTick;
	}
}

void timer_stop(u8 tmrIdx)
{
	if(tmrIdx < TIMER_NUM){
		sim_hwRegs[tmrIdx].enabled = 0;
	}
}

void timer_start(u8 tmrIdx)
{
	if(tmrIdx < TIMER_NUM){
		sim_hwRegs[tmrIdx].enabled  = 1;
		sim_hwRegs[tmrIdx].rearmCount++;
	}
}

static int hw_anyTimerLive(void)
{
	for(int i = 0; i < TIMER_NUM; i++){
		if(sim_hwTimers[i].live){
			return 1;
		}
	}
	return 0;
}

static int task_wedged;

/* Freeze the cooperative timer list: tick_all() stops servicing task callbacks
 * (the progress ticker and the rescue stable clock), exactly as a wedged scan
 * callback or a stuck ev_poll would. The hw list keeps ticking. */
void host_wedge(void)   { task_wedged = 1; }
void host_unwedge(void) { task_wedged = 0; }

/* Advance every live task timer one firing, n times; a < 0 return cancels that
 * timer. This is "fire n times", not a wall clock - each timer keeps its own
 * period semantics, which is all the scenarios need. */
static void tick_all(int n)
{
	for(int step = 0; step < n; step++){
		if(task_wedged){
			return;             /* the cooperative scheduler is stuck */
		}
		for(int i = 0; i < SIM_TIMER_SLOTS; i++){
			if(sim_timers[i].live && sim_timers[i].cb){
				if(sim_timers[i].cb(NULL) < 0){
					sim_timers[i].live = 0;
					sim_timers[i].evt.live = 0;
				}
			}
		}
	}
}

/* Advance every live hardware timer one firing, n times. This list is immune to
 * host_wedge(), matching drv_hwTmr's independence from ev_main. */
static void tick_hw_all(int n)
{
	for(int step = 0; step < n; step++){
		for(int i = 0; i < TIMER_NUM; i++){
			if(sim_hwTimers[i].live && sim_hwTimers[i].cb){
				if(sim_hwTimers[i].cb(sim_hwTimers[i].arg) < 0){
					sim_hwTimers[i].live = 0;
					sim_hwTimers[i].cb = NULL;
				}
			}
		}
	}
}

/* One second of concurrent progress: the task ticker fires once, then the hw
 * sampler samples once. The ordering (task first, hw second) matters for the
 * healthy scenarios - the sampler must see fresh progress each sample. */
static void tick_both(int n)
{
	for(int step = 0; step < n; step++){
		tick_all(1);
		tick_hw_all(1);
	}
}

/* ------------------------------------------------------------------ */
/* Simulated deliberate-reset markers                                   */

static int host_resetCount;         /* SYSTEM_RESET() calls */
static int host_skipWrites;         /* moes_resetSkipNextBoot() calls */
static int host_skipWritesAtReset;  /* skip-flag writes seen by the last reset */

void host_systemReset(void)
{
	host_resetCount++;
	host_skipWritesAtReset = host_skipWrites;
}

/* The real one lives in moes_flashcfg.c, which this harness does not link. It
 * is kept as a dummy so the scenarios can assert the liveness IRQ reset does
 * NOT call it (an IRQ-context NV write would be the bug). */
void moes_resetSkipNextBoot(void){ host_skipWrites++; }

/* ------------------------------------------------------------------ */
/* Test harness                                                        */

static int failures;

#define CHECK(cond, ...) do {                                            \
		if(!(cond)){                                                     \
			failures++;                                                  \
			printf("    FAIL  " __VA_ARGS__);                            \
			printf("\n          at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while(0)

/* The units under test keep their state in file statics, which a real device
 * resets by rebooting. There is no reset entry point, so the test reloads the
 * module the only way C allows: each "boot" runs in a fresh process. The
 * parent forks a child per scenario. Simpler and more honest than adding a
 * test-only reset hook to production code. */
#include <unistd.h>
#include <sys/wait.h>

static void sim_powerOn(void)   { moes_rescueBootCheck(); }

/* A watchdog-style reboot is the hang case build 06 must close: a boot with no
 * clean-shutdown marker and no user/exception cause. The rescue state machine
 * does not read a boot-reason register (moes_rescue.c:38-74); every boot that
 * reaches moes_rescueBootCheck() is, by construction, an un-clean boot. So the
 * harness models a watchdog reboot as an ordinary power-on, and this wrapper
 * exists only to say that out loud. */
static void sim_watchdogBoot(void) { sim_powerOn(); }

static void sim_reset_sim(void)
{
	memset(sim_timers, 0, sizeof(sim_timers));
	memset(sim_hwTimers, 0, sizeof(sim_hwTimers));
	memset(sim_hwRegs, 0, sizeof(sim_hwRegs));
	joined = 0;
	host_resetCount = 0;
	host_skipWrites = 0;
	host_skipWritesAtReset = 0;
	task_wedged = 0;
}

/* ------------------------------------------------------------------ */

static void t_fresh_device(void)
{
	printf("  fresh device: first boot is normal and records one failure\n");
	nv_present = 0; nv_writes = 0;

	sim_powerOn();

	CHECK(moes_rescueActive() == FALSE, "a fresh device must boot normally");
	CHECK(moes_rescueFailCount() == 1, "count should be 1, got %d", moes_rescueFailCount());
	CHECK(nv_present && nv_value == 1, "NV should hold 1, holds %d (present=%d)", nv_value, nv_present);
	CHECK(nv_writes == 1, "exactly one NV write per boot, got %d", nv_writes);
}

static void t_latch_boundary(int storedCount, int expectRescue, int expectWrites)
{
	printf("  boot with stored count %d: rescue=%d, writes=%d\n",
		   storedCount, expectRescue, expectWrites);
	nv_present = 1; nv_value = (u8)storedCount; nv_writes = 0;

	sim_powerOn();

	CHECK(moes_rescueActive() == (expectRescue ? TRUE : FALSE),
		  "rescue should be %d", expectRescue);
	CHECK(nv_writes == expectWrites,
		  "expected %d NV writes, got %d", expectWrites, nv_writes);
}

static void t_below_threshold(void){ t_latch_boundary(MOES_RESCUE_FAIL_THRESHOLD - 1, 0, 1); }
static void t_at_threshold(void)   { t_latch_boundary(MOES_RESCUE_FAIL_THRESHOLD,     1, 0); }
static void t_above_threshold(void){ t_latch_boundary(MOES_RESCUE_FAIL_THRESHOLD + 5, 1, 0); }

static void t_stable_clears(void)
{
	printf("  joined + continuous progress for the full window clears the counter\n");
	nv_present = 1; nv_value = 3; nv_writes = 0; sim_reset_sim();

	sim_powerOn();
	CHECK(moes_rescueFailCount() == 4, "count should be 4, got %d", moes_rescueFailCount());
	nv_writes = 0;

	joined = 1;
	moes_livenessBootedOnNetwork();       /* arms the progress ticker */
	moes_rescueStableTimerStart();
	CHECK(sim_anyTimerLive(), "the stable timer should be running");

	/* 19 healthy ticks: countdown 20 -> 1, no clear yet. */
	tick_both(MOES_RESCUE_STABLE_MINUTES - 1);
	CHECK(moes_rescueFailCount() == 4, "must not clear early (got %d)", moes_rescueFailCount());
	CHECK(nv_writes == 0, "must not write early (got %d)", nv_writes);

	/* 20th tick: countdown reaches zero -> confirmation window, still no clear. */
	tick_both(1);
	CHECK(moes_rescueFailCount() == 4, "countdown-zero must not clear before the confirm tick");
	CHECK(nv_writes == 0, "countdown-zero must not write before the confirm tick");

	/* Confirmation tick: commit the clear. */
	tick_both(1);
	CHECK(moes_rescueFailCount() == 0, "should have cleared, got %d", moes_rescueFailCount());
	CHECK(nv_value == 0, "NV should hold 0, holds %d", nv_value);
	CHECK(nv_writes == 1, "exactly one clearing write, got %d", nv_writes);
	CHECK(sim_timerCount() == 1, "only the liveness ticker should remain, got %d", sim_timerCount());
	CHECK(host_resetCount == 0, "a healthy run must never trip the fuse, got %d", host_resetCount);
}

static void t_dropping_off_restarts_the_clock(void)
{
	printf("  falling off the network restarts the clock, it does not pause it\n");
	nv_present = 1; nv_value = 2; nv_writes = 0; sim_reset_sim();

	sim_powerOn();                       /* count 2 -> 3 */
	joined = 1;
	moes_livenessBootedOnNetwork();
	moes_rescueStableTimerStart();

	tick_both(MOES_RESCUE_STABLE_MINUTES - 2);   /* 18 healthy -> s_minsLeft 2 */
	joined = 0;
	tick_both(1);                                  /* drop: reloads the countdown to 21 */
	joined = 1;
	tick_both(MOES_RESCUE_STABLE_MINUTES);         /* 20 -> s_minsLeft 1 */
	CHECK(moes_rescueFailCount() != 0, "must not clear - the clock restarted");

	tick_both(1);                                  /* 21 -> countdown zero, confirm */
	CHECK(moes_rescueFailCount() != 0, "must not clear at countdown-zero");

	tick_both(1);                                  /* confirm tick -> clear */
	CHECK(moes_rescueFailCount() == 0, "should clear once a full window elapses");
}

static void t_gesture_clears(void)
{
	printf("  a completed 3-power-cycle gesture clears the counter\n");
	nv_present = 1; nv_value = 4; nv_writes = 0; sim_reset_sim();

	sim_powerOn();
	CHECK(moes_rescueFailCount() == 5, "count should be 5, got %d", moes_rescueFailCount());

	moes_rescueClear();
	CHECK(moes_rescueFailCount() == 0, "gesture must zero the count");
	CHECK(nv_value == 0, "and persist it");

	nv_writes = 0;
	moes_rescueClear();
	CHECK(nv_writes == 0, "clearing an already-zero counter must not write flash");
}

static void t_pairing_gesture_cannot_latch(void)
{
	printf("  the 3-power-cycle gesture never reaches the rescue threshold\n");
	CHECK(MOES_RESCUE_FAIL_THRESHOLD > 3,
		  "threshold %d must exceed the rstnum:3 gesture", MOES_RESCUE_FAIL_THRESHOLD);
	/* Three quick boots is the whole gesture, and factoryRst_handler() calls
	 * moes_rescueClear() when it completes - so the worst case is a count of
	 * 3, cleared. Modelled here as three boots then a clear. */
	nv_present = 0; nv_writes = 0;
	sim_powerOn();
	CHECK(moes_rescueActive() == FALSE, "boot 1 of the gesture must be normal");
}

static void t_garbage_nv_is_failsafe(void)
{
	printf("  garbage NV fails safe (towards rescue, never towards a reset)\n");
	nv_present = 1; nv_value = 0xFF; nv_writes = 0;

	sim_powerOn();

	CHECK(moes_rescueActive() == TRUE, "0xFF must clamp into rescue, not wrap");
	CHECK(moes_rescueFailCount() == MOES_RESCUE_FAIL_THRESHOLD,
		  "clamped to the threshold, got %d", moes_rescueFailCount());
	CHECK(nv_writes == 0, "a latching boot must not write flash, got %d", nv_writes);
}

static void t_unreadable_nv(void)
{
	printf("  an unreadable counter is treated as zero, not as garbage\n");
	nv_present = 1; nv_value = 200; nv_readFails = 1; nv_writes = 0;

	sim_powerOn();

	CHECK(moes_rescueActive() == FALSE, "read failure must not latch rescue");
	CHECK(moes_rescueFailCount() == 1, "count should be 1, got %d", moes_rescueFailCount());
	nv_readFails = 0;
}

static void t_unwritable_nv(void)
{
	printf("  an unwritable counter degrades to today's behaviour, no crash\n");
	nv_present = 0; nv_writeFails = 1; nv_writes = 0;

	sim_powerOn();

	CHECK(moes_rescueActive() == FALSE, "must still boot");
	CHECK(nv_writes == 1, "it must still try once, got %d", nv_writes);
	nv_writeFails = 0;
}

static void t_timer_pool_exhausted(void)
{
	printf("  no free TL_ZB_TIMER just means this boot does not clear\n");
	nv_present = 1; nv_value = 2; sim_reset_sim(); timer_allocFails = 1;

	sim_powerOn();
	joined = 1;
	moes_rescueStableTimerStart();

	CHECK(moes_rescueFailCount() == 3, "count unchanged, got %d", moes_rescueFailCount());
	timer_allocFails = 0;
}

static void t_no_timer_when_nothing_to_clear(void)
{
	printf("  a healthy light does not burn a timer slot it does not need\n");
	nv_present = 1; nv_value = 0; sim_reset_sim();

	sim_powerOn();
	/* count is now 1, so the timer IS wanted */
	joined = 1;
	moes_rescueStableTimerStart();
	CHECK(sim_anyTimerLive(), "count is 1, the clock should run");

	/* and it is idempotent */
	moes_rescueStableTimerStart();
	CHECK(sim_timerCount() == 1, "still exactly one timer");
}

static void t_flash_wear_bound(void)
{
	printf("  a permanent reset loop writes flash a bounded number of times\n");
	/* Model 200 consecutive unstable boots by carrying nv_value across
	 * fork()ed children - each child is one power-on. */
	nv_present = 0; nv_value = 0; nv_writes = 0;

	int totalWrites = 0;
	u8  carried = 0;
	int carriedPresent = 0;

	for(int boot = 0; boot < 200; boot++){
		int pipefd[2];
		if(pipe(pipefd) != 0){ CHECK(0, "pipe() failed"); return; }

		pid_t pid = fork();
		if(pid == 0){
			close(pipefd[0]);
			nv_present = carriedPresent; nv_value = carried; nv_writes = 0;
			sim_powerOn();
			u8 out[3] = { (u8)nv_writes, nv_value, (u8)nv_present };
			ssize_t w = write(pipefd[1], out, 3);
			(void)w;
			close(pipefd[1]);
			_exit(0);
		}
		close(pipefd[1]);
		u8 in[3] = {0,0,0};
		ssize_t r = read(pipefd[0], in, 3);
		(void)r;
		close(pipefd[0]);
		waitpid(pid, NULL, 0);

		totalWrites   += in[0];
		carried        = in[1];
		carriedPresent = in[2];
	}

	printf("        200 unstable boots -> %d NV writes, final count %d\n", totalWrites, carried);
	CHECK(totalWrites == MOES_RESCUE_FAIL_THRESHOLD,
		  "expected exactly %d writes over 200 boots, got %d",
		  MOES_RESCUE_FAIL_THRESHOLD, totalWrites);
	CHECK(carried == MOES_RESCUE_FAIL_THRESHOLD,
		  "counter should have parked at the threshold, got %d", carried);
}

static void t_latch_after_threshold_boots(void)
{
	printf("  N consecutive unstable boots latch rescue on boot N+1\n");
	u8  carried = 0;
	int carriedPresent = 0;
	int firstRescueBoot = -1;

	for(int boot = 1; boot <= MOES_RESCUE_FAIL_THRESHOLD + 3; boot++){
		int pipefd[2];
		if(pipe(pipefd) != 0){ CHECK(0, "pipe() failed"); return; }
		pid_t pid = fork();
		if(pid == 0){
			close(pipefd[0]);
			nv_present = carriedPresent; nv_value = carried;
			sim_powerOn();
			u8 out[3] = { (u8)(moes_rescueActive() ? 1 : 0), nv_value, (u8)nv_present };
			ssize_t w = write(pipefd[1], out, 3); (void)w;
			close(pipefd[1]);
			_exit(0);
		}
		close(pipefd[1]);
		u8 in[3] = {0,0,0};
		ssize_t r = read(pipefd[0], in, 3); (void)r;
		close(pipefd[0]);
		waitpid(pid, NULL, 0);

		if(in[0] && firstRescueBoot < 0){
			firstRescueBoot = boot;
		}
		carried = in[1]; carriedPresent = in[2];
	}

	printf("        first rescue boot: #%d (threshold %d)\n",
		   firstRescueBoot, MOES_RESCUE_FAIL_THRESHOLD);
	CHECK(firstRescueBoot == MOES_RESCUE_FAIL_THRESHOLD + 1,
		  "expected rescue on boot %d, got %d",
		  MOES_RESCUE_FAIL_THRESHOLD + 1, firstRescueBoot);
}

static void t_watchdog_hang_chain(void)
{
	printf("  hang -> watchdog reboot -> probation -> rescue -> stable clear\n");

	/* 1. A watchdog reboot increments the counter exactly like any other
	 * un-clean boot. */
	nv_present = 0; nv_writes = 0;
	sim_watchdogBoot();
	CHECK(moes_rescueActive() == FALSE, "first watchdog boot must be normal");
	CHECK(moes_rescueFailCount() == 1, "watchdog boot must count 1, got %d",
		  moes_rescueFailCount());
	CHECK(nv_present && nv_value == 1, "watchdog boot must persist 1");
	CHECK(nv_writes == 1, "watchdog boot must write once, got %d", nv_writes);

	/* 2. Six consecutive watchdog boots drive the counter to the threshold,
	 * and the next watchdog boot reads that value and latches rescue. Same
	 * N -> latch-on-boot-N+1 shape as moes_rescue.c:63-73. */
	u8  carried = 0;
	int carriedPresent = 0;
	int firstRescueBoot = -1;

	for(int boot = 1; boot <= MOES_RESCUE_FAIL_THRESHOLD + 1; boot++){
		int pipefd[2];
		if(pipe(pipefd) != 0){ CHECK(0, "pipe() failed"); return; }

		pid_t pid = fork();
		if(pid == 0){
			close(pipefd[0]);
			nv_present = carriedPresent; nv_value = carried; nv_writes = 0;
			sim_watchdogBoot();
			u8 out[3] = {
				(u8)(moes_rescueActive() ? 1 : 0),
				nv_value,
				(u8)nv_present
			};
			ssize_t w = write(pipefd[1], out, 3); (void)w;
			close(pipefd[1]);
			_exit(0);
		}
		close(pipefd[1]);
		u8 in[3] = {0,0,0};
		ssize_t r = read(pipefd[0], in, 3); (void)r;
		close(pipefd[0]);
		waitpid(pid, NULL, 0);

		if(in[0] && firstRescueBoot < 0){
			firstRescueBoot = boot;
		}
		if(boot <= MOES_RESCUE_FAIL_THRESHOLD){
			CHECK(!in[0], "watchdog boot %d must not latch rescue yet", boot);
		}
		if(boot == MOES_RESCUE_FAIL_THRESHOLD){
			CHECK(in[1] == MOES_RESCUE_FAIL_THRESHOLD,
				  "six watchdog boots must reach the threshold, got %d", in[1]);
		}
		carried        = in[1];
		carriedPresent = in[2];
	}

	CHECK(firstRescueBoot == MOES_RESCUE_FAIL_THRESHOLD + 1,
		  "watchdog streak must latch rescue on boot %d, got %d",
		  MOES_RESCUE_FAIL_THRESHOLD + 1, firstRescueBoot);
	CHECK(carried == MOES_RESCUE_FAIL_THRESHOLD,
		  "counter must park at the threshold, got %d", carried);

	/* 3. A joined-and-healthy run still clears probation after the streak
	 * parked the counter at the threshold. This is the path a recovered light
	 * takes: counter==threshold latches rescue on this boot, the joined
	 * callbacks start the stable clock anyway, and a full healthy window (now
	 * 20 minutes of joined + progress plus the one-minute confirmation) calls
	 * moes_rescueClear(). */
	{
		int pipefd[2];
		if(pipe(pipefd) != 0){ CHECK(0, "pipe() failed"); return; }

		pid_t pid = fork();
		if(pid == 0){
			close(pipefd[0]);
			nv_present = carriedPresent; nv_value = carried; nv_writes = 0;
			sim_reset_sim();
			sim_watchdogBoot();

			u8 out[6];
			out[0] = (u8)(moes_rescueActive() ? 1 : 0);
			joined = 1;
			moes_livenessBootedOnNetwork();   /* progress ticker for the clear gate */
			moes_rescueStableTimerStart();
			out[1] = (u8)(sim_anyTimerLive() ? 1 : 0);
			tick_both(MOES_RESCUE_STABLE_MINUTES + 2);   /* 20 + confirm + spare */
			out[2] = (u8)moes_rescueFailCount();
			out[3] = nv_value;
			out[4] = (u8)nv_writes;
			out[5] = (u8)host_resetCount;

			ssize_t w = write(pipefd[1], out, 6); (void)w;
			close(pipefd[1]);
			_exit(0);
		}
		close(pipefd[1]);
		u8 in[6] = {0,0,0,0,0,0};
		ssize_t r = read(pipefd[0], in, 6); (void)r;
		close(pipefd[0]);
		waitpid(pid, NULL, 0);

		CHECK(in[0] == 1, "a boot at the threshold must latch rescue");
		CHECK(in[1] == 1, "the stable clock must run while in rescue mode");
		CHECK(in[2] == 0, "healthy run must clear the count, got %d", in[2]);
		CHECK(in[3] == 0, "NV should hold 0, holds %d", in[3]);
		CHECK(in[4] == 1, "exactly one clearing write, got %d", in[4]);
		CHECK(in[5] == 0, "a healthy run must never trip the fuse, got %d", in[5]);
	}
}

/* ------------------------------------------------------------------ */
/* Liveness monitor (moes_liveness.c)                                  */

#define LIVENESS_FUSE_TICKS  (MOES_LIVENESS_PROGRESS_RESET_S * 1000U / MOES_LIVENESS_SAMPLE_MS)

static void t_wedge_silence_forces_reset(void)
{
	printf("  class-1 wedge (unjoined, ev_timer stuck) forces an unmarked reset at the fuse\n");
	sim_reset_sim();

	moes_livenessBootedOnNetwork();       /* bdbInitCb: joinedNetwork == 1 */
	CHECK(sim_anyTimerLive(), "arming must start the progress ticker");
	CHECK(hw_anyTimerLive(), "arming must start the hw sampler");

	host_wedge();                          /* the scan timer cb never returns */
	tick_hw_all(LIVENESS_FUSE_TICKS - 1);
	CHECK(host_resetCount == 0, "must not reset before the fuse, got %d", host_resetCount);

	tick_hw_all(1);
	CHECK(host_resetCount == 1, "fuse expiry must reset exactly once, got %d", host_resetCount);
	CHECK(host_skipWritesAtReset == 0, "the IRQ reset must be unmarked (no moes_resetSkipNextBoot)");
}

static void t_hw_sample_callback_explicitly_rearms(void)
{
	printf("  each hw sample explicitly reloads Timer0 (stop -> init -> capture -> start)\n");
	sim_reset_sim();

	moes_livenessBootedOnNetwork();
	CHECK(hw_anyTimerLive(), "arming must start the hw sampler");

	/* The initial drv_hwTmr_set() is one timer_start(). */
	CHECK(sim_hwRegs[TIMER_IDX_0].rearmCount == 1,
		  "initial arm must be one start, got %d", sim_hwRegs[TIMER_IDX_0].rearmCount);
	CHECK(sim_hwRegs[TIMER_IDX_0].enabled == 1,
		  "hw timer must be enabled after arming");

	u32 expectedCap = (u32)MOES_LIVENESS_SAMPLE_MS * 1000UL * TIMER_TICK_1US_GET(TIMER_IDX_0);
	CHECK(sim_hwRegs[TIMER_IDX_0].capTick == expectedCap,
		  "capTick must be the 1 s tick count, got %u", sim_hwRegs[TIMER_IDX_0].capTick);

	/* Fire the sampler three times. Each callback must re-arm by itself; a
	 * broken re-arm would leave the timer stopped and the fuse silent. */
	tick_hw_all(3);
	CHECK(sim_hwRegs[TIMER_IDX_0].rearmCount == 4,
		  "three samples must add three re-arms, got %d", sim_hwRegs[TIMER_IDX_0].rearmCount);
	CHECK(sim_hwRegs[TIMER_IDX_0].enabled == 1,
		  "the timer must still be enabled after the re-arm");
	CHECK(sim_hwRegs[TIMER_IDX_0].initTick == 0,
		  "each re-arm must reload init tick 0");
	CHECK(sim_hwRegs[TIMER_IDX_0].capTick == expectedCap,
		  "each re-arm must reload the 1 s capture");
	CHECK(host_resetCount == 0,
		  "three no-progress samples must not yet reach the 60-sample fuse");
}

static void t_liveness_class2_wedge_resets_while_joined(void)
{
	printf("  class-2 wedge (joined-but-silent) still forces an unmarked reset\n");
	sim_reset_sim();

	moes_livenessBootedOnNetwork();
	joined = 1;                            /* joined stays set through the wedge */
	tick_both(3);                          /* a few healthy seconds of progress */

	host_wedge();
	tick_hw_all(LIVENESS_FUSE_TICKS - 1);
	CHECK(host_resetCount == 0, "must not reset before the fuse, got %d", host_resetCount);
	CHECK(joined == 1, "joined must remain set throughout the wedge");

	tick_hw_all(1);
	CHECK(host_resetCount == 1, "joined-blind fuse must reset exactly once, got %d", host_resetCount);
	CHECK(joined == 1, "joined must still read 1 - the fuse ignores it");
	CHECK(host_skipWritesAtReset == 0, "reset must be unmarked");
}

static void t_stack_activity_suppresses_reset(void)
{
	printf("  BDB activity corroboration holds the fuse open while the ticker is wedged\n");
	sim_reset_sim();

	moes_livenessBootedOnNetwork();
	host_wedge();                          /* the primary ticker starves */

	/* A future wedge that still delivers BDB commissioning callbacks would keep
	 * advancing the progress counter via moes_livenessStackActivity(); model it
	 * with activity every half-fuse so s_progress never stays put for 60 s. This
	 * documents the S2.1 corroboration path (liveness_redesign.md F5), not the
	 * known wedges - in those no BDB callback fires at all. */
	for(int round = 0; round < 20; round++){
		tick_hw_all(LIVENESS_FUSE_TICKS / 2);
		moes_livenessStackActivity();
	}
	CHECK(host_resetCount == 0, "activity must hold the fuse open, got %d resets",
		  host_resetCount);
}

static void t_rejoin_success_restarts_the_fuse(void)
{
	printf("  resuming progress restarts the no-progress clock\n");
	sim_reset_sim();

	moes_livenessBootedOnNetwork();
	tick_both(3);                          /* healthy progress before the wedge */

	host_wedge();
	tick_hw_all(LIVENESS_FUSE_TICKS - 2);
	CHECK(host_resetCount == 0, "must not reset before the fuse");

	host_unwedge();                        /* scheduler recovers (rejoin completes) */
	tick_both(3);                          /* progress advances, sampler restarts its clock */

	host_wedge();                          /* wedge again */
	tick_hw_all(LIVENESS_FUSE_TICKS - 1);
	CHECK(host_resetCount == 0, "the clock must restart from the recovered progress");
	tick_hw_all(1);
	CHECK(host_resetCount == 1, "a full silent fuse after that must reset");
}

static void t_pairing_device_never_resets(void)
{
	printf("  factory-new pairing is never armed, so never reset\n");
	sim_reset_sim();

	/* No arm call: this boot has no credentials. */
	tick_both(5 * LIVENESS_FUSE_TICKS);
	CHECK(host_resetCount == 0, "an unarmed boot must never reset");
	CHECK(!sim_anyTimerLive(), "an unarmed boot must not burn a task timer slot");
	CHECK(!hw_anyTimerLive(), "an unarmed boot must not arm the hw sampler");

	/* The join completes: SUCCESS arms the monitor from here on. */
	moes_livenessJoined();
	CHECK(sim_anyTimerLive(), "join success must arm the ticker");
	CHECK(hw_anyTimerLive(), "join success must arm the hw sampler");
	host_wedge();
	tick_hw_all(LIVENESS_FUSE_TICKS);
	CHECK(host_resetCount == 1, "once armed, a silent wedge must reset");
}

static void t_liveness_timer_pool_exhausted(void)
{
	printf("  a full ev_timer pool still resets: no ticker, no progress, fuse fires\n");
	sim_reset_sim();
	timer_allocFails = 1;

	moes_livenessBootedOnNetwork();       /* ticker schedule fails; hw sampler still starts */
	CHECK(!sim_anyTimerLive(), "the ticker must not be scheduled when the pool is full");
	CHECK(hw_anyTimerLive(), "the hw sampler must still be armed");

	tick_hw_all(LIVENESS_FUSE_TICKS - 1);
	CHECK(host_resetCount == 0, "must not reset before the fuse, got %d", host_resetCount);
	tick_hw_all(1);
	CHECK(host_resetCount == 1, "no ticker -> no progress -> reset at the fuse, got %d",
		  host_resetCount);

	timer_allocFails = 0;
}

static void t_liveness_coordinator_offline_no_reset(void)
{
	printf("  a live scheduler never trips the fuse while the network is absent\n");
	sim_reset_sim();

	moes_livenessBootedOnNetwork();
	joined = 0;                            /* coordinator gone; scheduler still alive */

	for(int round = 0; round < 3; round++){
		tick_both(LIVENESS_FUSE_TICKS);
		joined = !joined;                  /* rejoin attempts toggling the joined bit */
	}
	CHECK(host_resetCount == 0, "a live scheduler must not reset, got %d", host_resetCount);
}

static void t_rescue_stable_no_clear_when_wedged(void)
{
	printf("  a wedged joined boot never clears its probation history\n");
	nv_present = 1; nv_value = 2; nv_writes = 0; sim_reset_sim();

	sim_powerOn();                         /* count 2 -> 3 */
	nv_writes = 0;                         /* isolate the clear write below */
	joined = 1;
	moes_livenessBootedOnNetwork();
	moes_rescueStableTimerStart();

	tick_both(MOES_RESCUE_STABLE_MINUTES - 1);   /* 19 healthy -> s_minsLeft 1 */

	host_wedge();                                /* class-2 wedge before the 20th tick */
	tick_both(LIVENESS_FUSE_TICKS - 1);
	CHECK(host_resetCount == 0, "must not reset before the fuse, got %d", host_resetCount);
	CHECK(moes_rescueFailCount() != 0, "the count must survive the wedged boot");
	CHECK(nv_writes == 0, "no clear write may happen while progress is stalled");

	tick_both(1);                                /* the hw fuse trips here */
	CHECK(host_resetCount == 1, "the liveness fuse must reset the wedged boot");
	CHECK(moes_rescueFailCount() != 0, "the count must still be non-zero after the reset");
	CHECK(nv_writes == 0, "the stable clock must never have written the clear");
}

static void t_rescue_stable_confirm_window_blocks_minute20_clear(void)
{
	printf("  a wedge landing exactly at minute 20 cannot clear the count\n");
	nv_present = 1; nv_value = 2; nv_writes = 0; sim_reset_sim();

	sim_powerOn();                         /* count 2 -> 3 */
	nv_writes = 0;
	joined = 1;
	moes_livenessBootedOnNetwork();
	moes_rescueStableTimerStart();

	tick_both(MOES_RESCUE_STABLE_MINUTES);       /* countdown reaches zero, confirm set */
	CHECK(moes_rescueFailCount() != 0, "countdown-zero must not clear yet");
	CHECK(nv_writes == 0, "countdown-zero must not write yet");

	host_wedge();                                /* wedge during the confirmation minute */
	tick_both(LIVENESS_FUSE_TICKS);
	CHECK(moes_rescueFailCount() != 0, "the wedge must block the confirmation clear");
	CHECK(nv_writes == 0, "no clear write may commit");
	CHECK(host_resetCount == 1, "the liveness fuse must reset the wedged boot");
}

static void t_unmarked_reset_does_not_accumulate_factory_reset(void)
{
	printf("  an unmarked ~60s wedge reset can never accumulate the 2s power count\n");
	/* Model of common/factory_reset.c factoryRst_init() (threshold 3, 2 s clear)
	 * for the *unmarked* liveness reset. Every boot: restore+clamp, no skip
	 * flag (the IRQ path deliberately does not write one), count++, then the
	 * 2 s timer clears it. The next wedge reset is ~60 s later
	 * (MOES_LIVENESS_PROGRESS_RESET_S), so the 2 s clear always runs before the
	 * next increment and the count can never reach the threshold. */
	int powerCnt = 0;
	int maxSeen = 0;

	for(int boot = 0; boot < 200; boot++){
		if(powerCnt >= 3){                 /* FACTORY_RESET_POWER_CNT_THRESHOLD */
			powerCnt = 0;                  /* clamp (factory_reset.c:111-113) */
		}
		powerCnt++;                        /* factory_reset.c:129 */
		if(powerCnt > maxSeen){
			maxSeen = powerCnt;
		}
		/* The 2 s clear timer (factory_reset.c:62-68) fires before the next
		 * ~60 s wedge reset. */
		powerCnt = 0;
	}

	CHECK(maxSeen == 1, "power count must never exceed 1, got %d", maxSeen);
	CHECK(maxSeen < 3, "must never reach the gesture threshold, got %d", maxSeen);
}

static void t_wedge_reset_rescue_chain(void)
{
	printf("  THE CHAIN: wedge -> unmarked reset -> probation -> rescue -> healthy clear\n");
	/* Every boot below runs the real moes_rescue.c boot check and the real
	 * moes_liveness.c fuse, in a fresh process per boot. The wedge is modelled
	 * as the captured class-1 shape: a working boot-time rejoin, a short on-air
	 * window, parent loss, then a frozen cooperative scheduler with zero
	 * progress ever again. The hw sampler (which survives the freeze) drives
	 * the unmarked reset. */
	u8  carried = 0;
	int carriedPresent = 0;
	int firstRescueBoot = -1;

	for(int boot = 1; boot <= MOES_RESCUE_FAIL_THRESHOLD + 1; boot++){
		int pipefd[2];
		if(pipe(pipefd) != 0){ CHECK(0, "pipe() failed"); return; }

		pid_t pid = fork();
		if(pid == 0){
			close(pipefd[0]);
			nv_present = carriedPresent; nv_value = carried; nv_writes = 0;
			sim_reset_sim();

			moes_rescueBootCheck();           /* every boot, after stack_init */
			u8 rescue = (u8)(moes_rescueActive() ? 1 : 0);

			moes_livenessBootedOnNetwork();   /* NV says we belong to a network */
			joined = 1;                       /* boot-time rejoin works */
			tick_both(14);                    /* ~14 s on-air, then the wedge: */
			joined = 0;                       /* parent lost, scan frozen, no  */
			                                  /* progress ever again          */
			host_wedge();
			int ticksToReset = 0;
			while(host_resetCount == 0 && ticksToReset < 4 * (int)LIVENESS_FUSE_TICKS){
				tick_hw_all(1);
				ticksToReset++;
			}

			u8 out[6] = {
				rescue,
				(u8)(host_resetCount == 1 ? 1 : 0),
				(u8)ticksToReset,
				(u8)(host_skipWritesAtReset >= 1 ? 1 : 0),
				nv_value,
				(u8)nv_present
			};
			ssize_t w = write(pipefd[1], out, 6); (void)w;
			close(pipefd[1]);
			_exit(0);
		}
		close(pipefd[1]);
		u8 in[6] = {0,0,0,0,0,0};
		ssize_t r = read(pipefd[0], in, 6); (void)r;
		close(pipefd[0]);
		waitpid(pid, NULL, 0);

		printf("        boot %d: rescue=%d reset=%d after %d samples, marked=%d, NV=%d\n",
			   boot, in[0], in[1], in[2], in[3], in[4]);

		CHECK(in[1] == 1, "boot %d: the wedge must trip the fuse", boot);
		CHECK(in[2] == LIVENESS_FUSE_TICKS,
			  "boot %d: reset must come at the fuse (%d samples), got %d",
			  boot, (int)LIVENESS_FUSE_TICKS, in[2]);
		CHECK(in[3] == 0, "boot %d: the forced reset must be unmarked", boot);
		if(boot <= MOES_RESCUE_FAIL_THRESHOLD){
			CHECK(in[0] == 0, "boot %d must not latch rescue yet", boot);
		}
		if(in[0] && firstRescueBoot < 0){
			firstRescueBoot = boot;
		}
		carried        = in[4];
		carriedPresent = in[5];
	}

	CHECK(firstRescueBoot == MOES_RESCUE_FAIL_THRESHOLD + 1,
		  "the wedge loop must latch rescue on boot %d, got %d",
		  MOES_RESCUE_FAIL_THRESHOLD + 1, firstRescueBoot);
	CHECK(carried == MOES_RESCUE_FAIL_THRESHOLD,
		  "counter must park at the threshold, got %d", carried);

	/* Recovery: a latched-rescue boot that gets a healthy network run must
	 * clear probation - the path a light takes once fixed firmware lands. */
	{
		int pipefd[2];
		if(pipe(pipefd) != 0){ CHECK(0, "pipe() failed"); return; }

		pid_t pid = fork();
		if(pid == 0){
			close(pipefd[0]);
			nv_present = carriedPresent; nv_value = carried; nv_writes = 0;
			sim_reset_sim();

			moes_rescueBootCheck();
			u8 rescue = (u8)(moes_rescueActive() ? 1 : 0);

			moes_livenessBootedOnNetwork();
			joined = 1;                       /* this network stays up */
			moes_rescueStableTimerStart();    /* the app does this on join */
			tick_both(MOES_RESCUE_STABLE_MINUTES + 2);

			u8 out[5] = {
				rescue,
				(u8)moes_rescueFailCount(),
				nv_value,
				(u8)host_resetCount,
				(u8)nv_writes
			};
			ssize_t w = write(pipefd[1], out, 5); (void)w;
			close(pipefd[1]);
			_exit(0);
		}
		close(pipefd[1]);
		u8 in[5] = {0,0,0,0,0};
		ssize_t r = read(pipefd[0], in, 5); (void)r;
		close(pipefd[0]);
		waitpid(pid, NULL, 0);

		CHECK(in[0] == 1, "a boot at the threshold must latch rescue");
		CHECK(in[1] == 0, "a healthy window must clear the count, got %d", in[1]);
		CHECK(in[2] == 0, "NV should hold 0, holds %d", in[2]);
		CHECK(in[3] == 0, "a healthy boot must never trip the fuse, got %d", in[3]);
		CHECK(in[4] == 1, "exactly one clearing write, got %d", in[4]);
	}
}

/* ------------------------------------------------------------------ */

typedef void (*testfn)(void);

static void run_isolated(const char *name, testfn fn)
{
	/* Each scenario gets a fresh process so the units' file statics are
	 * genuinely re-initialised, exactly as a reboot would do. */
	pid_t pid = fork();
	if(pid == 0){
		failures = 0;
		fn();
		_exit(failures ? 1 : 0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if(WEXITSTATUS(st) != 0){
		failures++;
		printf("  ** %s FAILED **\n", name);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* children _exit() without flushing */
	printf("moes_rescue + moes_liveness - host test\n");
	printf("  MOES_RESCUE_FAIL_THRESHOLD = %d\n", MOES_RESCUE_FAIL_THRESHOLD);
	printf("  MOES_RESCUE_STABLE_MINUTES = %d (+1 confirm minute)\n", MOES_RESCUE_STABLE_MINUTES);
	printf("  MOES_LIVENESS_PROGRESS_RESET_S = %d (%d hw samples @ %d ms)\n\n",
		   (int)MOES_LIVENESS_PROGRESS_RESET_S, (int)LIVENESS_FUSE_TICKS,
		   (int)MOES_LIVENESS_SAMPLE_MS);

	failures = 0;

	run_isolated("fresh_device",            t_fresh_device);
	run_isolated("below_threshold",         t_below_threshold);
	run_isolated("at_threshold",            t_at_threshold);
	run_isolated("above_threshold",         t_above_threshold);
	run_isolated("stable_clears",           t_stable_clears);
	run_isolated("drop_restarts_clock",     t_dropping_off_restarts_the_clock);
	run_isolated("gesture_clears",          t_gesture_clears);
	run_isolated("gesture_cannot_latch",    t_pairing_gesture_cannot_latch);
	run_isolated("garbage_nv_failsafe",     t_garbage_nv_is_failsafe);
	run_isolated("unreadable_nv",           t_unreadable_nv);
	run_isolated("unwritable_nv",           t_unwritable_nv);
	run_isolated("timer_pool_exhausted",    t_timer_pool_exhausted);
	run_isolated("timer_only_when_needed",  t_no_timer_when_nothing_to_clear);
	run_isolated("latch_after_threshold",   t_latch_after_threshold_boots);
	run_isolated("flash_wear_bound",        t_flash_wear_bound);
	run_isolated("watchdog_hang_chain",     t_watchdog_hang_chain);

	run_isolated("liveness_wedge_reset",        t_wedge_silence_forces_reset);
	run_isolated("liveness_hw_sample_rearms",   t_hw_sample_callback_explicitly_rearms);
	run_isolated("liveness_activity_suppresses",t_stack_activity_suppresses_reset);
	run_isolated("liveness_rejoin_restarts",    t_rejoin_success_restarts_the_fuse);
	run_isolated("liveness_pairing_never_reset",t_pairing_device_never_resets);
	run_isolated("liveness_pool_exhausted",     t_liveness_timer_pool_exhausted);
	run_isolated("wedge_reset_rescue_chain",    t_wedge_reset_rescue_chain);

	run_isolated("liveness_class2_wedge_resets_while_joined",  t_liveness_class2_wedge_resets_while_joined);
	run_isolated("rescue_stable_no_clear_when_wedged",         t_rescue_stable_no_clear_when_wedged);
	run_isolated("rescue_stable_confirm_window_blocks_minute20_clear",
	             t_rescue_stable_confirm_window_blocks_minute20_clear);
	run_isolated("liveness_coordinator_offline_no_reset",      t_liveness_coordinator_offline_no_reset);
	run_isolated("liveness_unmarked_reset_does_not_accumulate_factory_reset",
	             t_unmarked_reset_does_not_accumulate_factory_reset);

	printf("\n%s (%d failing scenario%s)\n",
		   failures ? "FAILED" : "all scenarios passed",
		   failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
