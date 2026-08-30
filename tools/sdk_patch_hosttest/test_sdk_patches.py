#!/usr/bin/env python3
"""Host-only regression coverage for the fail-closed Telink SDK patch set."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))
import apply_sdk_patches  # noqa: E402


class SdkPatchTest(unittest.TestCase):
    archive: Path

    def extract_pristine_files(self, destination: Path, extra_files: tuple[str, ...] = ()) -> Path:
        manifest = apply_sdk_patches.load_manifest(apply_sdk_patches.DEFAULT_MANIFEST)
        with zipfile.ZipFile(self.archive) as archive:
            for relative in sorted(set(manifest["files"]) | set(extra_files)):
                member = f"tl_zigbee_sdk/{relative}"
                archive.extract(member, destination)
        return destination / "tl_zigbee_sdk"

    def run_applicator(self, sdk_root: Path, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOLS_DIR / "apply_sdk_patches.py"), "--sdk-root", str(sdk_root), *extra],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_pristine_patch_idempotency_and_drift_rejection(self) -> None:
        with tempfile.TemporaryDirectory(prefix="moes-sdk-patch-hosttest-") as temporary:
            sdk_root = self.extract_pristine_files(Path(temporary))
            first = self.run_applicator(sdk_root)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertIn("applied and verified", first.stdout)

            second = self.run_applicator(sdk_root)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertIn("patch set verified", second.stdout)

            drifted = sdk_root / "zigbee/ota/ota.c"
            drifted.write_bytes(drifted.read_bytes() + b"/* deliberate host-test drift */\r\n")
            rejected = self.run_applicator(sdk_root)
            self.assertEqual(rejected.returncode, 2)
            self.assertIn("mixed, missing, or locally modified", rejected.stderr)
            verify_rejected = self.run_applicator(sdk_root, "--verify-only")
            self.assertEqual(verify_rejected.returncode, 2)
            self.assertIn("requires the exact recorded MOES SDK patch set", verify_rejected.stderr)

    def test_maximum_size_erase_ends_at_nv_boundary_exclusive(self) -> None:
        self.assertEqual(apply_sdk_patches.ota_erase_end(0x68000), 0xD8000)
        self.assertEqual(apply_sdk_patches.ota_erase_end(0x68000 - 1), 0xD8000)
        with self.assertRaises(ValueError):
            apply_sdk_patches.ota_erase_end(0x68001)

    def test_ev_timer_self_cycle_fails_closed_and_external_timer_remains_valid(self) -> None:
        """Compile and execute the patched SDK timer source against a tiny shim.

        This is a functional guard test, not a text check: each public timer
        list walk is given a real self-cycle and must return after posting the
        normal SDK timer exception. It also schedules an external static event
        first, proving the guard did not incorrectly restrict the API to the
        24-entry pool (OTA uses that supported pattern).
        """
        shim = textwrap.dedent(
            """\
            #pragma once
            #include <stdint.h>
            #include <string.h>
            typedef uint8_t u8;
            typedef uint16_t u16;
            typedef uint32_t u32;
            typedef int32_t s32;
            typedef uint8_t bool;
            #define TRUE 1
            #define FALSE 0
            #define SUCCESS 1
            #define NO_TIMER_AVAIL 0
            #define S_TIMER_CLOCK_1US 1
            #define SYS_EXCEPTTION_COMMON_TIMER_EVEVT 1
            u32 clock_time(void);
            u32 drv_disable_irq(void);
            void drv_restore_irq(u32 state);
            void sys_exceptionPost(u32 line, u8 event);
            #define ZB_EXCEPTION_POST(event) sys_exceptionPost(__LINE__, (event))
            void host_systemReset(void);
            #define SYSTEM_RESET() host_systemReset()
            """
        )
        harness = textwrap.dedent(
            """\
            #include <stdio.h>
            #include "proj/tl_common.h"
            #include "proj/os/ev_timer.h"

            /* ev_timer.h exports neither of these even though both have
             * external linkage in ev_timer.c; ev.c reaches them the same way.
             * Declare them here: without a prototype, a 64-bit host build
             * implicitly types ev_timer_add() as int-returning and truncates
             * the pointer, which crashes the harness instead of testing the
             * guard. The compile below promotes that mistake to an error. */
            ev_timer_event_t *ev_timer_add(ev_timer_callback_t func, void *arg, u32 timeout);
            void ev_timer_executeCB(void);

            static int exceptions;
            static int resets;
            static int irq_depth;
            static u32 fake_clock;

            u32 clock_time(void) { return fake_clock; }
            u32 drv_disable_irq(void) { return (u32)irq_depth++; }
            void drv_restore_irq(u32 state) { irq_depth = (int)state; }
            void sys_exceptionPost(u32 line, u8 event) {
                (void)line;
                if (event == SYS_EXCEPTTION_COMMON_TIMER_EVEVT) {
                    exceptions++;
                }
            }
            void host_systemReset(void) { resets++; }
            static s32 keep_timer(void *arg) { (void)arg; return 0; }

            /* A callback can legitimately delete the current head while
             * executeCB is walking the list. This returns -1 to delete itself
             * after removing the old head, leaving a valid shorter list. */
            static s32 remove_head_then_stop(void *arg) {
                ev_unon_timer((ev_timer_event_t *)arg);
                return -1;
            }

            static ev_timer_event_t *make_self_cycle(void) {
                ev_timer_event_t *timer;
                ev_timer_init();
                exceptions = 0;
                resets = 0;
                timer = ev_timer_add(keep_timer, NULL, 1);
                if (!timer) {
                    return NULL;
                }
                timer->next = timer;
                return timer;
            }

            int main(void) {
                ev_timer_event_t external_a = {0};
                ev_timer_event_t external_b = {0};
                ev_timer_event_t external_c = {0};
                ev_timer_event_t callback_timer = {0};
                ev_timer_event_t removable_head = {0};
                ev_timer_event_t *timer;
                int i;

                /* 24 pooled + two caller-owned static events are legitimate:
                 * otaTimer and router secondTimer make the real limit 26. */
                ev_timer_init();
                exceptions = 0;
                for (i = 0; i < TIMER_EVENT_NUM; i++) {
                    if (!ev_timer_add(keep_timer, NULL, 5)) return 10;
                }
                ev_on_timer(&external_a, 5);
                ev_on_timer(&external_b, 5);
                if (!ev_timer_exist(&external_a) || !ev_timer_exist(&external_b) || exceptions) return 11;
                ev_timer_update(1);
                ev_timer_executeCB();
                if (exceptions || resets || irq_depth) return 12;
                ev_unon_timer(&external_a);
                ev_unon_timer(&external_b);
                if (ev_timer_exist(&external_a) || ev_timer_exist(&external_b) || exceptions || resets || irq_depth) return 13;

                /* The 27th node is beyond the documented 24-pool + two
                 * static-event capacity. Detect it through nearestUpdate and
                 * restore IRQ state before returning from ev_on_timer(). */
                ev_timer_init();
                exceptions = 0;
                resets = 0;
                for (i = 0; i < TIMER_EVENT_NUM; i++) {
                    if (!ev_timer_add(keep_timer, NULL, 5)) return 14;
                }
                ev_on_timer(&external_a, 5);
                ev_on_timer(&external_b, 5);
                ev_on_timer(&external_c, 5);
                if (exceptions != 1 || resets != 1 || irq_depth) return 15;

                /* The callback is the 26th node. Its legitimate head deletion
                 * starts a new list segment, so executeCB must reset its
                 * per-segment budget before walking the 24 remaining nodes. */
                ev_timer_init();
                exceptions = 0;
                resets = 0;
                callback_timer.cb = remove_head_then_stop;
                callback_timer.data = &removable_head;
                ev_on_timer(&callback_timer, 0);
                for (i = 0; i < TIMER_EVENT_NUM; i++) {
                    if (!ev_timer_add(keep_timer, NULL, 1)) return 16;
                }
                ev_on_timer(&removable_head, 1);
                ev_timer_executeCB();
                if (exceptions || resets || irq_depth || ev_timer_exist(&callback_timer) || ev_timer_exist(&removable_head)) return 17;
                ev_timer_update(1);
                if (exceptions || resets || irq_depth) return 18;

                timer = make_self_cycle(); if (!timer) return 20;
                ev_timer_update(1); if (exceptions != 1 || resets != 1 || irq_depth) return 21;

                timer = make_self_cycle(); if (!timer) return 30;
                if (ev_timer_exist(&external_a) || exceptions != 1 || resets != 1 || irq_depth) return 31;

                timer = make_self_cycle(); if (!timer) return 40;
                ev_on_timer(&external_a, 1); if (exceptions != 1 || resets != 1 || irq_depth) return 41;

                timer = make_self_cycle(); if (!timer) return 50;
                ev_unon_timer(&external_a); if (exceptions != 1 || resets != 1 || irq_depth) return 51;

                timer = make_self_cycle(); if (!timer) return 60;
                ev_on_timer(timer, 1); if (exceptions != 1 || resets != 1 || irq_depth) return 61;

                timer = make_self_cycle(); if (!timer) return 70;
                timer->timeout = 0;
                ev_timer_executeCB(); if (exceptions != 1 || resets != 1 || irq_depth) return 71;

                /* On hardware mcu_reset() is a register write that returns.
                 * ev_timer_process() must observe the corruption latch and
                 * never enter executeCB on the same corrupt list. */
                timer = make_self_cycle(); if (!timer) return 80;
                fake_clock = 1000;
                ev_timer_setPrevSysTick(0);
                ev_timer_process();
                if (exceptions != 1 || resets != 1 || irq_depth) return 81;

                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory(prefix="moes-ev-timer-hosttest-") as temporary:
            root = Path(temporary)
            sdk_root = self.extract_pristine_files(root, ("proj/os/ev_timer.h",))
            applied = self.run_applicator(sdk_root)
            self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
            (sdk_root / "proj" / "tl_common.h").write_text(shim, encoding="utf-8")
            harness_path = root / "test_ev_timer_guard.c"
            binary_path = root / "test_ev_timer_guard"
            harness_path.write_text(harness, encoding="utf-8")
            compiled = subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra",
                    # A missing prototype silently truncates a returned pointer
                    # on this 64-bit host and turns a guard test into a crash.
                    "-Werror=implicit-function-declaration", "-Werror=int-conversion",
                    "-DMOES_TS0505B=1", "-I", str(sdk_root),
                    str(harness_path), str(sdk_root / "proj/os/ev_timer.c"),
                    "-o", str(binary_path),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            try:
                executed = subprocess.run(
                    [str(binary_path)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    check=False, timeout=5,
                )
            except subprocess.TimeoutExpired as exc:
                self.fail(f"timer-cycle guard test hung instead of failing closed: {exc}")
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

            # The timer guard is deliberately TS0505B-only: other targets
            # sharing the archive must still compile the stock code path.
            legacy = subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-DMOES_TS0505B=0", "-I", str(sdk_root),
                    "-fsyntax-only", str(sdk_root / "proj/os/ev_timer.c"),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(legacy.returncode, 0, legacy.stdout + legacy.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--sdk-archive",
        type=Path,
        default=Path(os.environ.get("MOES_SDK_ARCHIVE", REPO_ROOT / "build/Zigbee_SDK.zip")),
        help="pinned local Zigbee_SDK.zip; this test never downloads",
    )
    args, remaining = parser.parse_known_args()
    if not args.sdk_archive.is_file():
        parser.error(f"pinned local SDK archive is required: {args.sdk_archive}")
    SdkPatchTest.archive = args.sdk_archive
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
