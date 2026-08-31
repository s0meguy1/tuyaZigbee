/********************************************************************************************************
 * @file    moes_bootmark.h
 *
 * @brief   Reset-reason breadcrumb: one byte that survives a reset and tells the
 *          next boot why the last one ended.
 *
 * WHY THIS EXISTS
 *
 * Every reset in this firmware was silent. All 26 ZB_EXCEPTION_POST sites in the
 * SDK funnel into tuyaLightSysException(), which resets and records nothing, and
 * z2m cannot see a reset at all - during the 2026-08-30 reset loop the pilot
 * logged leave_count 0, network_address_changes 0 and zero errors while
 * rebooting every 15 s. An exception reset, a watchdog reset and a brownout were
 * therefore indistinguishable, which is precisely what produced three confident
 * wrong diagnoses on 2026-08-29 and cost most of a day. The SDK author left a
 * "TODO: some information stored in NV" at ev.c:35 and never did it.
 *
 * WHY AN ANALOG REGISTER AND NOT NV
 *
 * NV is the wrong medium twice over. sys_exceptionPost() is called synchronously
 * from wherever the fault was detected - including from inside the NV layer
 * itself - so re-entering the flash writer there can leave a sector half
 * written; and a reset loop would grind the sector at hundreds of writes an hour
 * (see moes_rescue.c, which had exactly that bug).
 *
 * DEEP_ANA_REG1 (0x3b) is in the always-on domain. It is unclaimed on this
 * target - drv_pm.c only touches REG0 and REG4/REG6, and then only under
 * PM_ENABLE, which is off here - and its retention is exactly the semantics
 * wanted: it SURVIVES a soft reset and a watchdog reset, and a true power cycle
 * clears it to 0x00. So "0" honestly means "the last boot ended in a power cycle
 * or a reset that never reached our handler", which is itself the discriminator
 * that was missing.
 *
 * HOW TO READ IT IN THE FIELD - no programmer required
 *
 * The captured value is published as attribute 0x0004 on cluster 0xEF00:
 *     {"read":{"cluster":"manuSpecificTuya","attributes":[4]}}
 *   0        - last reset was NOT one of ours (power cycle, or watchdog/hardware)
 *   non-zero - value minus 1 is the SYS_EXCEPTTION_* code from proj/os/ev.h
 *
 * Note the asymmetry is deliberate and useful: a WATCHDOG reset leaves 0 here,
 * so "resetting repeatedly with 0 in this attribute" points at the watchdog or
 * the hardware, and a non-zero code points at a specific ZB_EXCEPTION_POST site.
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

/* Previous boot's reason, latched at startup: 0 = not an exception, otherwise
 * SYS_EXCEPTTION_* code + 1. Lives here so the ZCL attribute table can point
 * straight at it. */
extern u8 g_moesBootMarkPrev;

/* Latch the previous boot's value and clear the register for this boot. MUST be
 * the first thing user_init() does, before anything can fault. */
void moes_bootMarkInit(void);

/* Record the exception currently being handled, then return; the caller resets.
 * Reads the SDK's own T_evtExcept[] rather than taking an argument, because the
 * registered handler signature carries no parameters. */
void moes_bootMarkException(void);

/* Stamp an explicit reason for a deliberate reset, so the next boot can report
 * it as attribute 0x0004 exactly like an exception. Values must stay outside
 * the SYS_EXCEPTTION_* range - see MOES_BOOTMARK_NV_HEAL. */
void moes_bootMarkSet(u8 code);

#if defined(__cplusplus)
}
#endif
