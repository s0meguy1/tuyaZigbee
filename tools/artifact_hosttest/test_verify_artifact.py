#!/usr/bin/env python3
"""Host-only tests for the non-mutating artifact verifier."""

from __future__ import annotations

import binascii
from pathlib import Path
import struct
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import verify_artifact  # noqa: E402


def make_raw(version: int, build_id: str) -> bytes:
    data = bytearray(b"\xff" * 0x80)
    struct.pack_into("<I", data, 2, version)
    data[6:8] = verify_artifact.RAW_MAGIC
    data[8:12] = verify_artifact.RAW_KNLT
    struct.pack_into("<H", data, 18, 0x6464)
    struct.pack_into("<H", data, 20, 0x0395)
    encoded = build_id.encode("ascii")
    data[0x30:0x31 + len(encoded)] = bytes((len(encoded),)) + encoded
    struct.pack_into("<I", data, 0x18, len(data))
    struct.pack_into("<I", data, len(data) - 4, binascii.crc32(data[:-4]) ^ 0xFFFFFFFF)
    return bytes(data)


def make_outer(raw: bytes, version: int) -> bytes:
    header = struct.pack(
        "<I5HIH32sI",
        verify_artifact.OTA_MAGIC, 0x0100, verify_artifact.OTA_HEADER_SIZE,
        0, 0x6464, 0x0395, version, 2, b"\0" * 32,
        verify_artifact.OTA_HEADER_SIZE + verify_artifact.OTA_CHUNK_HEADER_SIZE + len(raw),
    )
    return header + struct.pack("<HI", 0, len(raw)) + raw


class ArtifactVerifierTest(unittest.TestCase):
    def test_multidigit_build_id_and_matching_outer_container(self) -> None:
        version = 0x110C3003
        raw = make_raw(version, "v1.12s3.3")
        with tempfile.TemporaryDirectory(prefix="moes-artifact-hosttest-") as temporary:
            directory = Path(temporary)
            raw_path = directory / "firmware.bin"
            ota_path = directory / "firmware.zigbee"
            raw_path.write_bytes(raw)
            ota_path.write_bytes(make_outer(raw, version))
            parsed = verify_artifact.parse_raw(
                raw_path, verify_artifact.DEFAULT_RAW_MAX_SIZE,
                verify_artifact.DEFAULT_STAGING_BASE, verify_artifact.DEFAULT_STAGING_LIMIT,
            )
            verify_artifact.assert_expected_raw(parsed, 0x6464, 0x0395, version, "v1.12s3.3")
            verify_artifact.parse_outer(ota_path, parsed, 0x6464, 0x0395, version, None, None, None)

    def test_boundary_and_crc_fail_closed(self) -> None:
        self.assertEqual(verify_artifact.staging_erase_end(0x68000), 0xD8000)
        raw = bytearray(make_raw(0x110D3003, "v1.13s3.3"))
        raw[-1] ^= 1
        with tempfile.TemporaryDirectory(prefix="moes-artifact-hosttest-") as temporary:
            raw_path = Path(temporary) / "broken.bin"
            raw_path.write_bytes(raw)
            with self.assertRaises(verify_artifact.VerifyError):
                verify_artifact.parse_raw(
                    raw_path, verify_artifact.DEFAULT_RAW_MAX_SIZE,
                    verify_artifact.DEFAULT_STAGING_BASE, verify_artifact.DEFAULT_STAGING_LIMIT,
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
