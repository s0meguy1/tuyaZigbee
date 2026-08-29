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
import unittest
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))
import apply_sdk_patches  # noqa: E402


class SdkPatchTest(unittest.TestCase):
    archive: Path

    def extract_five_pristine_files(self, destination: Path) -> Path:
        manifest = apply_sdk_patches.load_manifest(apply_sdk_patches.DEFAULT_MANIFEST)
        with zipfile.ZipFile(self.archive) as archive:
            for relative in manifest["files"]:
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
            sdk_root = self.extract_five_pristine_files(Path(temporary))
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
