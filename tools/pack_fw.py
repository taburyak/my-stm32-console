#!/usr/bin/env python3
"""Pack raw STM32F407 application .bin into a verified fw_update.bin image with 4WFU header."""

from __future__ import annotations

import argparse
import pathlib
import struct
import zlib

FW_UPDATE_MAGIC = 0x55465734
FW_UPDATE_HDR_SIZE = 32
FW_UPDATE_APP_ADDR = 0x08010000


def main() -> int:
    ap = argparse.ArgumentParser(description="Create 4WFU firmware update package")
    ap.add_argument("input_bin", type=pathlib.Path, help="Raw application .bin")
    ap.add_argument("output_fw", type=pathlib.Path, help="Output fw_update.bin")
    ap.add_argument("--version", default="1.2.0", help="Version string (max 11 chars)")
    args = ap.parse_args()

    payload = args.input_bin.read_bytes()
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    ver_bytes = args.version.encode("ascii", errors="ignore")[:11].ljust(12, b"\x00")
    hdr = struct.pack("<IIIII12s", FW_UPDATE_MAGIC, FW_UPDATE_HDR_SIZE, len(payload), crc, FW_UPDATE_APP_ADDR, ver_bytes)

    args.output_fw.parent.mkdir(parents=True, exist_ok=True)
    args.output_fw.write_bytes(hdr + payload)
    print(f"Packed {args.output_fw}: {len(payload)} B, CRC=0x{crc:08X}, ver={args.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
