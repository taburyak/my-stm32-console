#!/usr/bin/env python3
"""
uart_upload.py - Upload .bin apps, .c C-scripts, or .bmp assets to the STM32F407 Console over USART3.

Supports two modes:
  1. Fast Zero-Touch Framed Upload (default):
     - Sends the 6-byte wake sequence `"UPLD!\\n"` so the console automatically exits any running
       app and enters UART receive mode without pressing any buttons on the board.
     - Sends filename, flags (`--save`, `--run`), exact byte length, and IEEE CRC-32.
     - Streams 1 KB chunks with per-chunk ACK and verifies end-to-end CRC-32.
  2. Standard XMODEM-CRC / XMODEM-1K (`--xmodem`):
     - Compatible with manual "UART Rx" mode in Games Hub.

Examples:
  python tools/uart_upload.py --ls --port COM9
  python tools/uart_upload.py --pull appdata/sram_test/report.txt --port COM9
  python tools/uart_upload.py sdk_apps/stack_up.bmp build/Debug/sdk_apps/stack_up.bin --port COM9 --run
  python tools/uart_upload.py --delete games/stuck_up.bin --port COM9
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import time
import zlib
from typing import Any, List, Optional

try:
    import serial  # type: ignore
    import serial.tools.list_ports as list_ports  # type: ignore
except ImportError:
    serial = None
    list_ports = None

UPLD_FLAG_SAVE_SD   = 1 << 0
UPLD_FLAG_RUN_RAM   = 1 << 1
UPLD_FLAG_DELETE_SD = 1 << 2
UPLD_FLAG_LIST_SD   = 1 << 3
UPLD_FLAG_PULL_SD   = 1 << 4
UPLD_FLAG_MORE      = 1 << 7
UPLD_CHUNK_SIZE     = 1024

SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18


def crc16_ccitt(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else ((crc << 1) & 0xFFFF)
    return crc


def auto_detect_port() -> Optional[str]:
    if list_ports is None:
        return None
    ports = list(list_ports.comports())
    if not ports:
        return None
    # Prefer USB-UART bridges (CH340, CP210x, FTDI, STLink VCP)
    preferred_keywords = ("ch340", "cp210", "ftdi", "usb", "stlink", "uart", "serial")
    for p in ports:
        desc = f"{p.description} {p.manufacturer or ''}".lower()
        if any(k in desc for k in preferred_keywords):
            return p.device
    return ports[0].device


def wake_console(ser: Any) -> None:
    ser.reset_input_buffer()
    ser.write(b"UPLD!\n")
    ser.flush()

    t0 = time.monotonic()
    buf = bytearray()
    while (time.monotonic() - t0) < 2.5:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf.extend(chunk)
            if b"RDY\n" in buf:
                break

    time.sleep(0.03)
    ser.reset_input_buffer()


def read_ack_byte(ser: Any, timeout_s: float = 3.0) -> bytes:
    t0 = time.monotonic()
    while (time.monotonic() - t0) < timeout_s:
        b = ser.read(1)
        if not b:
            break
        if b in (b"A", b"E", b"K"):
            return b
    return b""


def read_exact(ser: Any, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        chunk = ser.read(n - len(out))
        if not chunk:
            break
        out.extend(chunk)
    return bytes(out)


def upload_framed(
    ser: Any,
    payload: bytes,
    remote_name: str,
    save_sd: bool,
    run_ram: bool,
    more: bool = False,
    need_wake: bool = True,
) -> bool:
    name_bytes = remote_name.encode("ascii", errors="replace")[:60]
    flags = (
        (UPLD_FLAG_SAVE_SD if save_sd else 0)
        | (UPLD_FLAG_RUN_RAM if run_ram else 0)
        | (UPLD_FLAG_MORE if more else 0)
    )
    crc32 = zlib.crc32(payload) & 0xFFFFFFFF

    if need_wake:
        wake_console(ser)

    # Send "UPLD" + flags(1B) + name_len(1B) + size(4B LE) + crc32(4B LE) + name
    hdr = struct.pack("<4sBBII", b"UPLD", flags, len(name_bytes), len(payload), crc32) + name_bytes
    ser.write(hdr)
    ser.flush()

    ack = read_ack_byte(ser)
    if ack != b"A":
        print(f"[uart_upload] Header rejected by console (got {ack!r})", file=sys.stderr)
        return False

    # Stream payload in 1 KB chunks
    sent = 0
    total = len(payload)
    t_start = time.monotonic()
    while sent < total:
        piece = payload[sent : sent + UPLD_CHUNK_SIZE]
        ser.write(piece)
        ser.flush()
        ack = ser.read(1)
        if ack != b"A":
            print(f"\n[uart_upload] Chunk error at {sent}/{total} bytes (got {ack!r})", file=sys.stderr)
            return False
        sent += len(piece)
        pct = (sent * 100) // total
        print(f"\r[uart_upload] Sent {sent}/{total} bytes ({pct}%)", end="", flush=True)

    # Final CRC verification byte ('K' = OK)
    final = ser.read(1)
    dt = max(0.001, time.monotonic() - t_start)
    print()
    if final != b"K":
        print(f"[uart_upload] CRC verification failed on console (got {final!r})", file=sys.stderr)
        return False

    print(
        f"[uart_upload] SUCCESS: '{remote_name}' ({total} B, CRC 0x{crc32:08X}) in {dt:.2f}s ({total/1024/dt:.1f} KB/s)"
    )
    return True


def delete_framed(ser: Any, remote_name: str, more: bool = False, need_wake: bool = True) -> bool:
    name_bytes = remote_name.encode("ascii", errors="replace")[:60]
    flags = UPLD_FLAG_DELETE_SD | (UPLD_FLAG_MORE if more else 0)

    if need_wake:
        wake_console(ser)

    hdr = struct.pack("<4sBBII", b"UPLD", flags, len(name_bytes), 0, 0) + name_bytes
    ser.write(hdr)
    ser.flush()

    ack = read_ack_byte(ser)
    if ack != b"A":
        print(f"[uart_upload] Delete header rejected by console (got {ack!r})", file=sys.stderr)
        return False

    final = ser.read(1)
    if final != b"K":
        print(f"[uart_upload] Delete failed on console (got {final!r})", file=sys.stderr)
        return False

    print(f"[uart_upload] DELETED '{remote_name}' from SD card.")
    return True


def read_chunked_stream(ser: Any, total_size: int) -> Optional[bytes]:
    out = bytearray()
    while len(out) < total_size:
        hdr = read_exact(ser, 6)
        if len(hdr) != 6:
            return None
        chunk_len, exp_crc = struct.unpack("<HI", hdr)
        if chunk_len == 0 or chunk_len > 256:
            return None
        chunk = read_exact(ser, chunk_len)
        if len(chunk) != chunk_len:
            return None
        calc_crc = zlib.crc32(chunk) & 0xFFFFFFFF
        if calc_crc == exp_crc:
            out.extend(chunk)
            ser.write(b"A")
            ser.flush()
        else:
            ser.write(b"N")
            ser.flush()
    return bytes(out)


def list_sd_framed(ser: Any, path: str = "/", more: bool = False, need_wake: bool = True) -> bool:
    name_bytes = path.encode("ascii", errors="replace")[:60]
    flags = UPLD_FLAG_LIST_SD | (UPLD_FLAG_MORE if more else 0)

    if need_wake:
        wake_console(ser)

    hdr = struct.pack("<4sBBII", b"UPLD", flags, len(name_bytes), 0, 0) + name_bytes
    ser.write(hdr)
    ser.flush()

    ack = read_ack_byte(ser)
    if ack != b"A":
        print(f"[uart_upload] List header rejected by console (got {ack!r})", file=sys.stderr)
        return False

    meta = read_exact(ser, 8)
    if len(meta) < 8:
        print("[uart_upload] Timeout reading directory listing header", file=sys.stderr)
        return False
    size, exp_crc = struct.unpack("<II", meta)
    data = read_chunked_stream(ser, size)
    if data is None or len(data) != size:
        print(f"[uart_upload] Incomplete listing ({0 if data is None else len(data)}/{size} B)", file=sys.stderr)
        return False
    calc_crc = zlib.crc32(data) & 0xFFFFFFFF
    if calc_crc != exp_crc:
        print(f"[uart_upload] Listing CRC mismatch (0x{calc_crc:08X} != 0x{exp_crc:08X})", file=sys.stderr)
        return False

    print("\n" + "=" * 54)
    print(f"  CONSOLE SD CARD TREE ({path})")
    print("=" * 54)
    print(data.decode("utf-8", errors="replace"), end="")
    print("=" * 54)
    return True


def pull_sd_framed(
    ser: Any,
    remote_path: str,
    local_out: Optional[pathlib.Path],
    more: bool = False,
    need_wake: bool = True,
) -> bool:
    name_bytes = remote_path.encode("ascii", errors="replace")[:60]
    flags = UPLD_FLAG_PULL_SD | (UPLD_FLAG_MORE if more else 0)

    if need_wake:
        wake_console(ser)

    hdr = struct.pack("<4sBBII", b"UPLD", flags, len(name_bytes), 0, 0) + name_bytes
    ser.write(hdr)
    ser.flush()

    ack = read_ack_byte(ser)
    if ack != b"A":
        print(f"[uart_upload] Pull rejected (file not found on SD: '{remote_path}', got {ack!r})", file=sys.stderr)
        return False

    meta = read_exact(ser, 8)
    if len(meta) < 8:
        print("[uart_upload] Timeout reading file header from console", file=sys.stderr)
        return False
    size, exp_crc = struct.unpack("<II", meta)
    data = read_chunked_stream(ser, size)
    if data is None or len(data) != size:
        print(f"[uart_upload] Incomplete file read ({0 if data is None else len(data)}/{size} B)", file=sys.stderr)
        return False
    calc_crc = zlib.crc32(data) & 0xFFFFFFFF
    if calc_crc != exp_crc:
        print(f"[uart_upload] File CRC mismatch (0x{calc_crc:08X} != 0x{exp_crc:08X})", file=sys.stderr)
        return False

    if local_out is not None:
        local_out.parent.mkdir(parents=True, exist_ok=True)
        local_out.write_bytes(data)
        print(f"[uart_upload] PULLED '{remote_path}' -> '{local_out}' ({size} B, CRC 0x{calc_crc:08X})")
    else:
        print(f"\n--- {remote_path} ({size} bytes) ---")
        print(data.decode("utf-8", errors="replace"))
        print("--- END ---")
    return True


def upload_xmodem(ser: Any, payload: bytes) -> bool:
    print("[uart_upload] Waiting for XMODEM-CRC 'C' poll from console...")
    t0 = time.monotonic()
    while (time.monotonic() - t0) < 30.0:
        b = ser.read(1)
        if b == b"C":
            break
    else:
        print("[uart_upload] Timeout waiting for XMODEM 'C' poll.", file=sys.stderr)
        return False

    blk_size = 1024
    padded = payload + b"\x1a" * ((blk_size - (len(payload) % blk_size)) % blk_size)
    if not padded:
        padded = b"\x1a" * blk_size

    seq = 1
    offset = 0
    while offset < len(padded):
        block = padded[offset : offset + blk_size]
        crc = crc16_ccitt(block)
        pkt = struct.pack(">BBB", STX, seq & 0xFF, 0xFF - (seq & 0xFF)) + block + struct.pack(">H", crc)
        ser.write(pkt)
        ser.flush()

        resp = ser.read(1)
        if resp == bytes([ACK]):
            offset += blk_size
            seq = (seq + 1) & 0xFF
            print(f"\r[uart_upload] XMODEM-1K {min(offset, len(payload))}/{len(payload)} bytes", end="", flush=True)
        elif resp == bytes([CAN]):
            print("\n[uart_upload] Cancelled by console.", file=sys.stderr)
            return False
        else:
            print(f"\n[uart_upload] Retrying block {seq} (got {resp!r})...")

    ser.write(bytes([EOT]))
    ser.flush()
    ser.read(1)
    print("\n[uart_upload] XMODEM transfer complete.")
    return True


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Upload, list, pull, or delete files on STM32 Console over USART3.")
    ap.add_argument("files", type=pathlib.Path, nargs="*", help="Local file(s) to upload in a single session (.bin, .c, .bmp, .raw)")
    ap.add_argument("-p", "--port", type=str, default=None, help="Serial port (e.g. COM9; auto-detected if omitted)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    ap.add_argument("--dest", type=str, default=None, help="Target filename on console (when uploading a single file)")
    ap.add_argument("--ls", action="store_true", help="List SD card directory tree (2-level expansion of all folders)")
    ap.add_argument("--ls-dir", type=str, default=None, help="List a specific directory on SD card (e.g. games or appdata/sram_test)")
    ap.add_argument("--pull", type=str, default=None, help="Download a file from SD card (e.g. appdata/sram_test/report.txt)")
    ap.add_argument("-o", "--output", type=pathlib.Path, default=None, help="Local destination path for --pull (prints to stdout if omitted)")
    ap.add_argument("--delete", action="append", default=[], help="Delete file(s) on console SD card (can be passed multiple times)")
    ap.add_argument("--run", dest="run", action="store_true", default=True, help="Launch last uploaded .bin/.c in RAM (default)")
    ap.add_argument("--no-run", dest="run", action="store_false", help="Save to SD card only without launching")
    ap.add_argument("--ram-only", action="store_true", help="Do not write to SD card; only run in RAM")
    ap.add_argument("--xmodem", action="store_true", help="Use standard XMODEM-1K CRC instead of zero-touch framed protocol")
    args = ap.parse_args(argv)

    if serial is None:
        print("Error: pyserial is required (`pip install pyserial`).", file=sys.stderr)
        return 1

    port = args.port or auto_detect_port()
    if not port:
        print("Error: no serial port found. Specify --port COMx.", file=sys.stderr)
        return 1

    for f in args.files:
        if not f.is_file():
            print(f"Error: file not found: {f}", file=sys.stderr)
            return 1

    # Build ordered list of batch operations to run over a single serial session
    ops: List[tuple] = []
    if args.ls:
        ops.append(("ls", "/"))
    if args.ls_dir is not None:
        ops.append(("ls", args.ls_dir))
    for d in args.delete:
        ops.append(("del", d))
    if args.pull is not None:
        ops.append(("pull", args.pull, args.output))
    for f in args.files:
        ops.append(("up", f))

    if not ops:
        ap.print_help()
        return 1

    print(f"[uart_upload] Opening {port} @ {args.baud} baud ({len(ops)} operation(s))...")
    with serial.Serial(port=port, baudrate=args.baud, timeout=3.0) as ser:
        if args.xmodem and len( args.files ) == 1:
            return 0 if upload_xmodem(ser, args.files[0].read_bytes()) else 1

        for idx, op in enumerate(ops):
            is_first = (idx == 0)
            has_more = (idx + 1 < len(ops))
            kind = op[0]

            if kind == "ls":
                ok = list_sd_framed(ser, op[1], more=has_more, need_wake=is_first)
            elif kind == "del":
                ok = delete_framed(ser, op[1], more=has_more, need_wake=is_first)
            elif kind == "pull":
                ok = pull_sd_framed(ser, op[1], op[2], more=has_more, need_wake=is_first)
            elif kind == "up":
                fpath: pathlib.Path = op[1]
                payload = fpath.read_bytes()
                remote_name = (args.dest if (args.dest and len(args.files) == 1) else fpath.name)
                save_sd = not args.ram_only
                # Only launch in RAM if this is the final operation in the batch
                run_ram = (not has_more) and args.run and remote_name.lower().endswith((".bin", ".c"))
                ok = upload_framed(
                    ser,
                    payload,
                    remote_name,
                    save_sd=save_sd,
                    run_ram=run_ram,
                    more=has_more,
                    need_wake=is_first,
                )
            else:
                ok = False

            if not ok:
                return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
