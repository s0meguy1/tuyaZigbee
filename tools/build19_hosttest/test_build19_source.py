#!/usr/bin/env python3
"""Focused host-only source contracts for the Build 19 light hardening.

This test deliberately inspects the real firmware source rather than trying to
emulate the complete Telink/ZCL stack. It proves control-flow and registration
contracts (timed-off cancellation, advisory rescue call sites, BDB liveness
arming, and z2m-required XY attributes). It does *not* prove physical LED/PWM
output; that still requires visual confirmation or direct PWM-register evidence.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]

# The branch this project actually publishes to. Its history is a deliberately
# disjoint, sanitized, code-only lineage; the detailed local history is never
# pushed. Used to prove the exemption below is not a leak.
PUBLIC_REF = "fork/moes-ts0505b"

# Forensic notes that record a real per-device EUI as evidence. These are kept
# locally on purpose and excluded from the code-only publication, so the EUI
# scan skips them - and a companion test proves they are genuinely unpublished
# rather than taking that on trust.
PRIVATE_EVIDENCE = frozenset({"bughunt/build15_runaway_cascade.md"})

# The EUI parser validates the a4:c1:38 Telink OUI, so its test vector is
# necessarily OUI-prefixed and is structurally indistinguishable from a real
# address. Allowing it by FILE would hand that whole file a standing licence to
# carry any identity; these exact byte strings are allowed instead, so pasting a
# real EUI into the very same test still fails. Compared case-insensitively, and
# every entry must still appear in the parser test (checked below) so a stale
# entry cannot linger as silent permission.
SYNTHETIC_TEST_EUIS = frozenset({"a4c1380f1e2d3c4b"})
EUI_VECTOR_SOURCE = "tools/eui_hosttest/test_eui.c"


def source(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def recorded_sdk_additions() -> str:
    """Return only additions from the tracked, reproducible SDK patch.

    The normal light/*.c scan below cannot see a hidden use in vendored SDK
    code. Checking additions (rather than raw patch text) avoids false hits
    from a removed line in a unified diff.
    """
    patch = source("sdk_patches/telink_zigbee_sdk_0d0859e2_moes.patch")
    return "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )


class EffectWireContract(unittest.TestCase):
    """The converter's EFFECTS index IS the datapoint value sent to the chip.

    Nothing enforced that it matched moes_effect_e, so reordering or inserting
    an entry on either side would silently fire the WRONG effect on every
    fixture - a burst where an explosion was asked for - with no error anywhere.
    Caught during build 26 review; the arrays happened to agree, but only by
    hand-checking them.
    """

    ENUM = REPO_ROOT / "light" / "light_effects.h"
    CONVERTER = REPO_ROOT / "converters" / "moes_ts0505b_light_show.js"

    def _firmware_effects(self):
        text = self.ENUM.read_text(encoding="utf-8")
        pairs = re.findall(r"MOES_EF_([A-Z_]+)\s*=\s*(\d+)", text)
        self.assertTrue(pairs, "no MOES_EF_* enumerators found")
        return {int(v): n.lower() for n, v in pairs}

    def _converter_effects(self):
        text = self.CONVERTER.read_text(encoding="utf-8")
        block = re.search(r"const EFFECTS = \[(.*?)\];", text, re.S)
        self.assertIsNotNone(block, "EFFECTS array not found in the converter")
        # Strip // comments FIRST: a quoted word inside a comment is not an
        # entry, and a naive scan mis-read one during build 26.
        clean = "\n".join(re.sub(r"//.*$", "", line) for line in block.group(1).splitlines())
        return [m.group(1) for m in re.finditer(r"'([a-z_]+)'", clean)]

    def test_converter_indices_match_the_firmware_enum(self):
        fw = self._firmware_effects()
        conv = self._converter_effects()
        self.assertEqual(len(conv), len(fw),
                         f"converter lists {len(conv)} effects, firmware defines {len(fw)}")
        for index, name in enumerate(conv):
            self.assertIn(index, fw, f"converter index {index} ({name}) has no enumerator")
            expected = fw[index]
            # MOES_EF_STEADY is exposed as "stop"; every other name matches.
            if expected == "steady":
                expected = "stop"
            self.assertEqual(name, expected,
                             f"index {index}: converter says {name!r}, firmware says {expected!r}")

    def test_enum_is_dense_and_zero_based(self):
        fw = self._firmware_effects()
        self.assertEqual(sorted(fw), list(range(len(fw))),
                         "moes_effect_e must stay dense and zero-based: the value is the wire format")


class Build19SourceContracts(unittest.TestCase):
    def test_bdb_success_arms_liveness_before_join_branch(self) -> None:
        text = source("light/zb_appCb.c")
        success = text.index("if(status == BDB_INIT_STATUS_SUCCESS)")
        arm = text.index("moes_livenessBooted();", success)
        joined = text.index("if(joinedNetwork)", success)
        self.assertLess(arm, joined)
        self.assertIn("including factory-new boots", text[success:joined])

    def test_plain_on_cancels_timed_off_but_timed_command_preserves_values(self) -> None:
        text = source("light/zcl_onOffCb.c")
        clear = re.search(
            r"if\(cancelTimedOff && cmd == ZCL_CMD_ONOFF_ON\)\{"
            r"\s*pOnOff->onTime = 0;"
            r"\s*pOnOff->offWaitTime = 0;"
            r"\s*tuyaLight_OnWithTimedOffTimerStop\(\);",
            text,
        )
        self.assertIsNotNone(clear)
        self.assertIn("tuyaLight_onoffApply(cmd, TRUE);", text)
        timed_start = text.index("static void tuyaLight_onoff_onWithTimedOffProcess")
        timed_end = text.index("static void tuyaLight_onoff_offWithEffectProcess", timed_start)
        timed = text[timed_start:timed_end]
        self.assertIn("tuyaLight_onoffApply(ZCL_CMD_ONOFF_ON, FALSE);", timed)

        epcfg = source("light/tuyaLightEpCfg.c")
        # zcl_nv_onOff_t contains only the two persistent fields. Timed-off
        # state must neither be written nor restored across a boot.
        self.assertNotIn("zcl_nv_onOff.onTime", epcfg)
        self.assertNotIn("zcl_nv_onOff.offWaitTime", epcfg)

    def test_rescue_flag_has_only_advisory_firmware_call_sites(self) -> None:
        allowed = {"light/moes_rescue.c", "light/zb_appCb.c"}
        seen: set[str] = set()
        tracked_c = subprocess.run(
            ["git", "ls-files", "*.c"],
            cwd=REPO_ROOT,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.splitlines()
        for relative in tracked_c:
            # Host harnesses call the function deliberately; this invariant is
            # for firmware source, including any future non-light directory.
            if relative.startswith("tools/"):
                continue
            path = REPO_ROOT / relative
            if re.search(r"\bmoes_rescueActive\s*\(", path.read_text(encoding="utf-8")):
                seen.add(relative)
        self.assertEqual(seen, allowed)

        app = source("light/zb_appCb.c")
        self.assertIn("OTA_QUERY", app)
        self.assertIn("light_blink_start(moes_rescueActive() ? 5 : 2", app)

        # The SDK watchdog loop used to gate its 600 ms policy on the storm
        # flag. That is a third behaviour change, outside the exact-two-effect
        # contract; ensure the clean SDK created from this recorded patch has
        # no hidden rescue dependency.
        self.assertNotIn("moes_rescueActive", recorded_sdk_additions())

    def test_no_full_factory_eui_is_added_to_the_public_worktree(self) -> None:
        # An OUI prefix is a product-family discriminator; a complete EUI-64
        # identifies one physical device and must never enter the published
        # tree. Scope note: docs/ holds upstream device dumps that are already
        # public in the parent project and are not ours to rewrite, so this
        # invariant covers the surface this project actually authors.
        public_paths = (
            "CMakeLists.txt",
            "README.md",
            "FALLBACK_DESIGN.md",
            "bughunt",
            "cmake",
            "common",
            "light",
            "sdk_patches",
            "tools",
        )
        full_eui = re.compile(r"(?i)(?:0x)?a4c138[0-9a-f]{10}\b")
        # Tracked files are not enough. A brand-new file is both the likeliest
        # place for an identity to be pasted in and invisible to `git ls-files`
        # until someone stages it - so this scan once passed while an untracked
        # new host test carried a real fixture EUI. Untracked-but-not-ignored
        # files are therefore scanned too, which catches a leak while it is
        # still being written rather than after it is staged.
        def listing(*args: str) -> list[str]:
            return subprocess.run(
                ["git", *args],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout.splitlines()

        candidates_all = sorted(
            set(listing("ls-files")) | set(listing("ls-files", "--others", "--exclude-standard"))
        )
        for relative in public_paths:
            candidates = (item for item in candidates_all if item == relative or item.startswith(f"{relative}/"))
            for candidate in candidates:
                if candidate in PRIVATE_EVIDENCE:
                    continue
                path = REPO_ROOT / candidate
                if not path.is_file():
                    continue
                try:
                    contents = path.read_text(encoding="utf-8")
                except UnicodeDecodeError:
                    continue
                for hit in full_eui.finditer(contents):
                    literal = hit.group(0).lower().removeprefix("0x")
                    self.assertIn(
                        literal,
                        SYNTHETIC_TEST_EUIS,
                        f"full EUI-64 in publishable source: {candidate}",
                    )

    def test_synthetic_eui_allowlist_cannot_rot_into_silent_permission(self) -> None:
        """Every allowed vector must still be the parser test's own vector.

        The allowlist above is the one place a real identity could be smuggled
        past the scan. Requiring each entry to appear in the parser test means a
        vector that is removed or edited there fails here instead of quietly
        remaining permitted everywhere.
        """
        vectors = source(EUI_VECTOR_SOURCE).lower()
        for allowed in SYNTHETIC_TEST_EUIS:
            self.assertIn(
                allowed,
                vectors,
                f"stale allowlist entry no longer used by {EUI_VECTOR_SOURCE}: {allowed}",
            )

    def test_private_evidence_files_exist_and_are_absent_from_the_public_branch(self) -> None:
        """The EUI exemption above is only safe while these stay unpublished.

        A per-device EUI inside a forensic note is real evidence and is kept
        deliberately, so the scan skips those files. That skip would silently
        become a leak if such a file were ever pushed, or would quietly rot
        into a blanket exemption if the path were renamed away. Both halves
        are therefore checked here rather than trusted.
        """
        tracked = set(
            subprocess.run(
                ["git", "ls-files"],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout.splitlines()
        )
        for relative in PRIVATE_EVIDENCE:
            if not (REPO_ROOT / relative).exists():
                # The sanitized publication tree legitimately does not carry
                # this evidence at all, so there is nothing to exempt and
                # nothing that could leak: the content scan above proves it by
                # finding no such file. Distinguish that from the dangerous
                # case - the file still present but no longer tracked under
                # this path, i.e. renamed out from under the exemption, which
                # would turn the skip into a blanket one. That still fails.
                continue
            self.assertIn(relative, tracked, f"stale exemption, path no longer tracked: {relative}")

        published = subprocess.run(
            ["git", "ls-tree", "-r", "--name-only", PUBLIC_REF],
            cwd=REPO_ROOT,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        if published.returncode != 0:
            # A fresh clone without the publication remote fetched cannot make
            # this comparison. The content scan above still ran; the real
            # pre-push gate is a scan of the sanitized worktree at push time.
            self.skipTest(f"publication ref {PUBLIC_REF} is not available locally")
        published_paths = set(published.stdout.splitlines())
        for relative in PRIVATE_EVIDENCE:
            self.assertNotIn(
                relative,
                published_paths,
                f"private forensic evidence is published on {PUBLIC_REF}: {relative}",
            )

    def test_advertised_xy_attributes_have_real_state_and_move_to_updates_it(self) -> None:
        attrs = source("light/tuyaLightEpCfg.c")
        header = source("light/tuyaLight.h")
        color = source("light/zcl_colorCtrlCb.c")

        self.assertIn("ZCL_COLOR_CAPABILITIES_BIT_X_Y_ATTRIBUTES", attrs)
        self.assertRegex(header, r"u16 currentX;\s*\n\s*u16 currentY;")
        self.assertRegex(
            attrs,
            r"ZCL_ATTRID_CURRENT_X,\s+ZCL_DATA_TYPE_UINT16,\s+ACCESS_CONTROL_READ \| ACCESS_CONTROL_REPORTABLE,\s+\(u8\*\)&g_zcl_colorCtrlAttrs\.currentX",
        )
        self.assertRegex(
            attrs,
            r"ZCL_ATTRID_CURRENT_Y,\s+ZCL_DATA_TYPE_UINT16,\s+ACCESS_CONTROL_READ \| ACCESS_CONTROL_REPORTABLE,\s+\(u8\*\)&g_zcl_colorCtrlAttrs\.currentY",
        )
        move_start = color.index("static void tuyaLight_moveToColorProcess")
        move_end = color.index("#if COLOR_CCT_SUPPORT", move_start)
        move = color[move_start:move_end]
        self.assertIn("pColor->currentX = cmd->colorX;", move)
        self.assertIn("pColor->currentY = cmd->colorY;", move)

        # The inherited MoveColor/StepColor stubs did not update a coordinate
        # and had no calibrated XY output transform. They must reject rather
        # than report a successful command with stale CurrentX/CurrentY.
        dispatch = color[color.index("status_t tuyaLight_colorCtrlCb"):]
        self.assertRegex(
            dispatch,
            r"case ZCL_CMD_LIGHT_COLOR_CONTROL_MOVE_COLOR:\s*"
            r"case ZCL_CMD_LIGHT_COLOR_CONTROL_STEP_COLOR:"
            r"[\s\S]*?return ZCL_STA_UNSUP_CLUSTER_COMMAND;",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
