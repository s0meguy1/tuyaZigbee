#!/usr/bin/env python3
"""Read-only verifier for Telink raw firmware and Zigbee OTA containers.

It intentionally never calls tl_check_fw.py: that helper mutates raw binaries
and must never be pointed at an outer .zigbee container.
"""

from __future__ import annotations

import argparse
import binascii
from dataclasses import dataclass
from pathlib import Path
import struct
import sys
from typing import Iterable, Optional


RAW_MAGIC = b"\x5d\x02"
RAW_KNLT = b"KNLT"
OTA_MAGIC = 0x0BEEF11E
OTA_HEADER_SIZE = 56
OTA_CHUNK_HEADER_SIZE = 6
DEFAULT_RAW_MAX_SIZE = 0x68000
DEFAULT_STAGING_BASE = 0x70000
DEFAULT_STAGING_LIMIT = 0xD8000
SECTOR_SIZE = 0x1000


class VerifyError(RuntimeError):
    pass


def auto_int(value: str) -> int:
    return int(value, 0)


@dataclass(frozen=True)
class RawImage:
    data: bytes
    file_version: int
    manufacturer_code: int
    image_type: int


def staging_erase_end(raw_size: int, base: int = DEFAULT_STAGING_BASE,
                      sector_size: int = SECTOR_SIZE) -> int:
    return base + ((raw_size + sector_size - 1) // sector_size) * sector_size


def parse_raw(path: Path, raw_max_size: int, staging_base: int,
              staging_limit: int) -> RawImage:
    data = path.read_bytes()
    if len(data) < 0x20:
        raise VerifyError(f"raw image is too short: {path}")
    if len(data) > raw_max_size:
        raise VerifyError(f"raw image exceeds staging capacity: {len(data):#x} > {raw_max_size:#x}")
    if data[6:8] != RAW_MAGIC:
        raise VerifyError(f"raw image lacks Telink 5d02 magic: {path}")
    if data[8:12] != RAW_KNLT:
        raise VerifyError(f"raw image lacks Telink KNLT flag: {path}")
    declared_length = struct.unpack_from("<I", data, 0x18)[0]
    if declared_length != len(data):
        raise VerifyError(
            f"raw declared length mismatch: header {declared_length:#x}, file {len(data):#x}"
        )
    recorded_crc = struct.unpack_from("<I", data, len(data) - 4)[0]
    calculated_crc = binascii.crc32(data[:-4]) ^ 0xFFFFFFFF
    if recorded_crc != calculated_crc:
        raise VerifyError(
            f"raw CRC mismatch: header {recorded_crc:#010x}, calculated {calculated_crc:#010x}"
        )
    erase_end = staging_erase_end(len(data), staging_base)
    if erase_end > staging_limit:
        raise VerifyError(
            f"raw staging erase crosses limit: [{staging_base:#x}, {erase_end:#x}) > {staging_limit:#x}"
        )
    return RawImage(
        data=data,
        file_version=struct.unpack_from("<I", data, 2)[0],
        manufacturer_code=struct.unpack_from("<H", data, 18)[0],
        image_type=struct.unpack_from("<H", data, 20)[0],
    )


def assert_expected_raw(raw: RawImage, expected_manufacturer: Optional[int],
                        expected_image_type: Optional[int],
                        expected_file_version: Optional[int],
                        expected_sw_build_id: Optional[str]) -> None:
    if expected_manufacturer is not None and raw.manufacturer_code != expected_manufacturer:
        raise VerifyError(
            f"raw manufacturer mismatch: {raw.manufacturer_code:#06x} != {expected_manufacturer:#06x}"
        )
    if expected_image_type is not None and raw.image_type != expected_image_type:
        raise VerifyError(f"raw image type mismatch: {raw.image_type:#06x} != {expected_image_type:#06x}")
    if expected_file_version is not None and raw.file_version != expected_file_version:
        raise VerifyError(
            f"raw file version mismatch: {raw.file_version:#010x} != {expected_file_version:#010x}"
        )
    if expected_sw_build_id is not None:
        encoded = expected_sw_build_id.encode("ascii")
        if len(encoded) > 16:
            raise VerifyError("expected software-build string exceeds Zigbee's 16-character attribute")
        marker = bytes((len(encoded),)) + encoded
        if marker not in raw.data:
            raise VerifyError(
                f"raw image does not contain the expected length-prefixed software-build string {expected_sw_build_id!r}"
            )


def parse_outer(path: Path, raw: RawImage, expected_manufacturer: Optional[int],
                expected_image_type: Optional[int], expected_file_version: Optional[int],
                outer_expected_manufacturer: Optional[int],
                outer_expected_image_type: Optional[int],
                outer_expected_file_version: Optional[int]) -> None:
    data = path.read_bytes()
    if len(data) < OTA_HEADER_SIZE + OTA_CHUNK_HEADER_SIZE:
        raise VerifyError(f"OTA container is too short: {path}")
    magic, header_version, header_size, field_control, manufacturer, image_type, file_version, stack_version = struct.unpack_from(
        "<I5HIH", data, 0
    )
    total_size = struct.unpack_from("<I", data, 52)[0]
    if magic != OTA_MAGIC:
        raise VerifyError(f"OTA magic mismatch: {magic:#010x}")
    if header_version != 0x0100 or header_size != OTA_HEADER_SIZE or field_control != 0:
        raise VerifyError(
            f"OTA header mismatch: version={header_version:#06x}, size={header_size}, field-control={field_control:#06x}"
        )
    if total_size != len(data):
        raise VerifyError(f"OTA total-size mismatch: header {total_size:#x}, file {len(data):#x}")
    chunk_type, payload_size = struct.unpack_from("<HI", data, OTA_HEADER_SIZE)
    payload = data[OTA_HEADER_SIZE + OTA_CHUNK_HEADER_SIZE:]
    if chunk_type != 0 or payload_size != len(raw.data):
        raise VerifyError(f"OTA payload header mismatch: type={chunk_type}, size={payload_size:#x}")
    if payload != raw.data:
        raise VerifyError("OTA payload differs from the supplied raw image")
    expected_outer_manufacturer = outer_expected_manufacturer
    expected_outer_image_type = outer_expected_image_type
    expected_outer_file_version = outer_expected_file_version
    if expected_outer_manufacturer is None:
        expected_outer_manufacturer = expected_manufacturer if expected_manufacturer is not None else raw.manufacturer_code
    if expected_outer_image_type is None:
        expected_outer_image_type = expected_image_type if expected_image_type is not None else raw.image_type
    if expected_outer_file_version is None:
        expected_outer_file_version = expected_file_version if expected_file_version is not None else raw.file_version
    if manufacturer != expected_outer_manufacturer:
        raise VerifyError(f"OTA manufacturer mismatch: {manufacturer:#06x} != {expected_outer_manufacturer:#06x}")
    if image_type != expected_outer_image_type:
        raise VerifyError(f"OTA image type mismatch: {image_type:#06x} != {expected_outer_image_type:#06x}")
    if file_version != expected_outer_file_version:
        raise VerifyError(f"OTA file version mismatch: {file_version:#010x} != {expected_outer_file_version:#010x}")
    _ = stack_version  # Parsed deliberately: it is part of the fixed header.


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", required=True, type=Path)
    parser.add_argument("--zigbee", type=Path, help="outer Zigbee OTA container to compare to --raw")
    parser.add_argument("--expected-manufacturer", type=auto_int)
    parser.add_argument("--expected-image-type", type=auto_int)
    parser.add_argument("--expected-file-version", type=auto_int)
    parser.add_argument("--expected-sw-build-id")
    parser.add_argument("--outer-expected-manufacturer", type=auto_int)
    parser.add_argument("--outer-expected-image-type", type=auto_int)
    parser.add_argument("--outer-expected-file-version", type=auto_int)
    parser.add_argument("--raw-max-size", type=auto_int, default=DEFAULT_RAW_MAX_SIZE)
    parser.add_argument("--staging-base", type=auto_int, default=DEFAULT_STAGING_BASE)
    parser.add_argument("--staging-limit", type=auto_int, default=DEFAULT_STAGING_LIMIT)
    args = parser.parse_args(argv)
    try:
        raw = parse_raw(args.raw, args.raw_max_size, args.staging_base, args.staging_limit)
        assert_expected_raw(
            raw,
            args.expected_manufacturer,
            args.expected_image_type,
            args.expected_file_version,
            args.expected_sw_build_id,
        )
        if args.zigbee is not None:
            parse_outer(
                args.zigbee,
                raw,
                args.expected_manufacturer,
                args.expected_image_type,
                args.expected_file_version,
                args.outer_expected_manufacturer,
                args.outer_expected_image_type,
                args.outer_expected_file_version,
            )
        print(
            f"verified raw={args.raw} size={len(raw.data)} version={raw.file_version:#010x} "
            f"manufacturer={raw.manufacturer_code:#06x} type={raw.image_type:#06x}"
        )
        if args.zigbee is not None:
            print(f"verified OTA container={args.zigbee}")
        return 0
    except (OSError, VerifyError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
