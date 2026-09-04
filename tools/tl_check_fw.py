#!/usr/bin/env python3

# =======================================================================
# OTA-CRITICAL. Prove any change below on the SWire bench before it
# reaches a live fixture.
#
# This builds the image that deployed fixtures install. A mistake here
# is not a bad build, it is a fixture that will not boot or will not
# rejoin, and such a fixture CANNOT be recovered over the air. The only
# way back is SWire, with the light physically down from the ceiling.
# That has already cost this project one fixture: INCIDENT_2026-08-15.md
#
# The host suites cannot catch it. They never perform a transfer, so
# green tests say nothing about whether an update still installs.
#
# Before this reaches any live device:
#   1. OTA_TEST_PLAN.md, "Phase 0 - bench unit. Mandatory."
#   2. MOES_EDITING_GUIDE.md S1, the four invariants.
#   3. A real OTA onto the bench fixture, then a power cycle, then a
#      rejoin. An image that boots once is not proof.
# =======================================================================

# NOTE THE NAME IS A LIE. This does not check anything: it PADS the file
# and APPENDS a crc32, printing "Firmware patched!". Running it on an
# already-built .zigbee corrupts that image (213138 -> 213156 bytes,
# observed 2026-09-04). Delete and rebuild the artifact if you do.
# To VERIFY an artifact, use tools/verify_artifact.py instead.

import binascii
import sys
# https://github.com/pvvx/ATC_MiThermometer/issues/186#issuecomment-1030410603
with open(sys.argv[1], 'rb') as f:
    firmware = bytearray(f.read(-1))

if firmware[6:8] != b'\x5d\x02':
    # Ensure FW size is multiple of 16
    padding = 16 - len(firmware) % 16
    if padding < 16:
        firmware += b'\xFF' * padding
    # Fix FW length
    firmware[0x18:0x1c] = (len(firmware)+4).to_bytes(4, byteorder='little')
    # Add magic constant
    firmware[6:8] = b'\x5d\x02'
    # Add CRC
    crc = binascii.crc32(firmware) ^ 0xffffffff
    firmware += crc.to_bytes(4, byteorder='little')
    # Write the new firmware back to the file
    with open(sys.argv[1], 'wb') as f:
        f.write(firmware)
    print("Firmware patched!")
else:
    print("Firmware already patched!")
