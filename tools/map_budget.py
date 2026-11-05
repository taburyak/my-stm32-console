#!/usr/bin/env python3
"""
map_budget.py - Linker .map file memory budget analyzer for STM32F407 Console OS & SDK apps.

Parses:
  - build/<preset>/my-stm32-console.map (OS kernel: FLASH, RAM, CCMRAM, APP_RAM, EXTSRAM)
  - build/<preset>/sdk_apps/*.map       (SDK apps: APP_RAM 28 KB code+data+bss, EXT_RAM 512 KB)

Checks region utilization against budgets, breaks down top symbols/modules per region,
and exits with a non-zero code when --strict is passed and a budget threshold is exceeded.

Usage:
  python tools/map_budget.py
  python tools/map_budget.py --build-dir build/Debug --top 10 --strict
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


@dataclass
class Region:
    name: str
    origin: int
    length: int
    used: int = 0
    sections: List[Tuple[str, int, int]] = field(default_factory=list)

    @property
    def pct(self) -> float:
        return (self.used * 100.0 / self.length) if self.length > 0 else 0.0

    @property
    def free(self) -> int:
        return max(0, self.length - self.used)


@dataclass
class SymbolEntry:
    section: str
    addr: int
    size: int
    obj: str


MEM_CFG_RE = re.compile(
    r"^\s*([A-Za-z0-9_*]+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+([a-zA-Z]+)"
)
TOP_SEC_RE = re.compile(
    r"^(\.[A-Za-z0-9_.*]+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)(?:\s+load address\s+(0x[0-9a-fA-F]+))?"
)
TOP_SEC_NAME_ONLY_RE = re.compile(r"^(\.[A-Za-z0-9_.*]+)\s*$")
TOP_SEC_CONT_RE = re.compile(
    r"^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)(?:\s+load address\s+(0x[0-9a-fA-F]+))?\s*$"
)
SUB_SEC_ONE_LINE_RE = re.compile(
    r"^\s+(\.[A-Za-z0-9_.*]+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+)$"
)
SUB_SEC_NAME_ONLY_RE = re.compile(r"^\s+(\.[A-Za-z0-9_.*]+)\s*$")
SUB_SEC_CONT_RE = re.compile(r"^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+)$")
NOLOAD_SECTIONS = {".bss", ".tbss", ".ccmram", "._user_heap_stack", ".sram_data", ".ext_bss"}


def _account_top_section(
    regions: Dict[str, Region], sec_name: str, vma: int, size: int, lma: Optional[int]
) -> None:
    if size <= 0:
        return
    for r in regions.values():
        if r.origin <= vma < r.origin + r.length:
            r.used += size
            r.sections.append((sec_name, vma, size))
            break
    if lma is not None and lma != vma and sec_name not in NOLOAD_SECTIONS:
        for r in regions.values():
            if r.origin <= lma < r.origin + r.length:
                r.used += size
                r.sections.append((f"{sec_name} (LMA)", lma, size))
                break


def parse_map_file(map_path: pathlib.Path) -> Tuple[Dict[str, Region], List[SymbolEntry]]:
    lines = map_path.read_text(encoding="utf-8", errors="replace").splitlines()

    regions: Dict[str, Region] = {}
    symbols: List[SymbolEntry] = []

    in_mem_cfg = False
    in_mem_map = False
    pending_topsec: Optional[str] = None
    pending_subsec: Optional[str] = None

    for line in lines:
        if line.startswith("Memory Configuration"):
            in_mem_cfg = True
            in_mem_map = False
            continue
        if line.startswith("Linker script and memory map"):
            in_mem_cfg = False
            in_mem_map = True
            continue

        if in_mem_cfg:
            m = MEM_CFG_RE.match(line)
            if m and m.group(1) != "*default*":
                rname = m.group(1)
                origin = int(m.group(2), 16)
                length = int(m.group(3), 16)
                regions[rname] = Region(name=rname, origin=origin, length=length)
            continue

        if not in_mem_map:
            continue

        if line.startswith("OUTPUT(") or line.startswith(".debug_") or line.startswith(".comment"):
            break

        if pending_topsec is not None:
            mtc = TOP_SEC_CONT_RE.match(line)
            if mtc:
                vma = int(mtc.group(1), 16)
                size = int(mtc.group(2), 16)
                lma = int(mtc.group(3), 16) if mtc.group(3) else None
                _account_top_section(regions, pending_topsec, vma, size, lma)
            pending_topsec = None
            continue

        # Top-level output section (single-line or wrapped)
        mt = TOP_SEC_RE.match(line)
        if mt:
            sec_name = mt.group(1)
            vma = int(mt.group(2), 16)
            size = int(mt.group(3), 16)
            lma = int(mt.group(4), 16) if mt.group(4) else None
            pending_subsec = None
            _account_top_section(regions, sec_name, vma, size, lma)
            continue

        mtn = TOP_SEC_NAME_ONLY_RE.match(line)
        if mtn:
            pending_topsec = mtn.group(1)
            pending_subsec = None
            continue

        # Subsection on a single line
        ms = SUB_SEC_ONE_LINE_RE.match(line)
        if ms:
            pending_subsec = None
            sub_name = ms.group(1)
            addr = int(ms.group(2), 16)
            size = int(ms.group(3), 16)
            obj = ms.group(4).strip()
            if size > 0 and addr > 0:
                symbols.append(SymbolEntry(section=sub_name, addr=addr, size=size, obj=obj))
            continue

        # Wrapped subsection name line
        mn = SUB_SEC_NAME_ONLY_RE.match(line)
        if mn:
            pending_subsec = mn.group(1)
            continue

        if pending_subsec is not None:
            mc = SUB_SEC_CONT_RE.match(line)
            if mc:
                addr = int(mc.group(1), 16)
                size = int(mc.group(2), 16)
                obj = mc.group(3).strip()
                if size > 0 and addr > 0:
                    symbols.append(SymbolEntry(section=pending_subsec, addr=addr, size=size, obj=obj))
            pending_subsec = None

    return regions, symbols


def fmt_kb(n: int) -> str:
    return f"{n / 1024.0:6.1f} KB ({n:7d} B)"


def bar(pct: float, width: int = 20) -> str:
    filled = int(round(min(100.0, max(0.0, pct)) * width / 100.0))
    return "[" + ("#" * filled) + ("-" * (width - filled)) + "]"


def short_obj_name(obj: str) -> str:
    obj = obj.replace("\\", "/")
    if "(" in obj and obj.endswith(")"):
        archive, member = obj.rsplit("(", 1)
        return f"{pathlib.Path(archive).name}({member}"
    return pathlib.Path(obj).name


def print_os_report(
    map_path: pathlib.Path,
    top_n: int,
    warn_pct: float,
) -> bool:
    regions, symbols = parse_map_file(map_path)
    budget_ok = True

    print("=" * 78)
    print(f"  STM32F407 CONSOLE OS MEMORY BUDGET ({map_path.as_posix()})")
    print("=" * 78)
    print(f"{'Region':<10} {'Origin':<12} {'Used':>18} {'Size':>10} {'Free':>10}  {'Usage'}")
    print("-" * 78)

    for rname in ("FLASH", "RAM", "CCMRAM", "APP_RAM", "EXTSRAM"):
        r = regions.get(rname)
        if not r:
            continue
        status = ""
        if rname in ("FLASH", "RAM", "CCMRAM") and r.pct > warn_pct:
            status = "  [WARN]"
            budget_ok = False
        print(
            f"{r.name:<10} 0x{r.origin:08X}   {r.used:7d} B ({r.used/1024:5.1f}K) "
            f"{r.length//1024:7d} KB {r.free//1024:7d} KB  {bar(r.pct, 14)} {r.pct:5.1f}%{status}"
        )

    if top_n > 0:
        for rname in ("RAM", "CCMRAM", "FLASH"):
            r = regions.get(rname)
            if not r:
                continue
            rsyms = [s for s in symbols if r.origin <= s.addr < r.origin + r.length]
            rsyms.sort(key=lambda s: s.size, reverse=True)
            if not rsyms:
                continue
            print(f"\nTop {min(top_n, len(rsyms))} consumers in {rname}:")
            for s in rsyms[:top_n]:
                print(f"  0x{s.addr:08X}  {s.size:7d} B ({s.size/1024:5.1f} KB)  {s.section:<28} {short_obj_name(s.obj)}")

    print()
    return budget_ok


def print_sdk_apps_report(sdk_dir: pathlib.Path, warn_pct: float) -> bool:
    map_files = sorted(sdk_dir.glob("*.map"))
    if not map_files:
        return True

    budget_ok = True
    print("=" * 78)
    print(f"  SDK APPS SANDBOX BUDGET (APP_RAM = 28 KB code+data+bss, EXT_RAM = 512 KB)")
    print("=" * 78)
    print(f"{'App':<18} {'Image (.bin)':>14} {'BSS':>10} {'APP_RAM Total':>18} {'EXT_RAM (.ext_bss)':>20}")
    print("-" * 78)

    for mp in map_files:
        regions, _ = parse_map_file(mp)
        app_ram = regions.get("APP_RAM")
        ext_ram = regions.get("EXT_RAM")
        if not app_ram:
            continue

        bin_path = mp.with_suffix(".bin")
        img_bytes = bin_path.stat().st_size if bin_path.is_file() else 0
        bss_bytes = sum(sz for name, _, sz in app_ram.sections if name.startswith(".bss"))
        app_used = app_ram.used
        ext_used = ext_ram.used if ext_ram else 0
        ext_len = ext_ram.length if ext_ram else (512 * 1024)

        flag = ""
        if app_ram.pct > warn_pct or (ext_ram and ext_ram.pct > warn_pct):
            flag = " !"
            if app_ram.pct > 100.0 or (ext_ram and ext_ram.pct > 100.0):
                budget_ok = False

        print(
            f"{mp.stem:<18} {img_bytes:9d} B    {bss_bytes:7d} B  "
            f"{app_used:6d}/{app_ram.length//1024}K ({app_ram.pct:4.1f}%)   "
            f"{ext_used/1024:6.1f}/{ext_len//1024}K ({(ext_used*100.0/ext_len):4.1f}%){flag}"
        )

    print("=" * 78)
    return budget_ok


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Analyze STM32 Console OS & SDK apps linker .map budgets.")
    ap.add_argument(
        "--build-dir",
        type=pathlib.Path,
        default=pathlib.Path("build/Debug"),
        help="CMake build directory containing my-stm32-console.map (default: build/Debug)",
    )
    ap.add_argument("--top", type=int, default=8, help="Number of largest symbols per region to display (default: 8)")
    ap.add_argument("--warn-pct", type=float, default=90.0, help="Warning threshold percentage (default: 90.0)")
    ap.add_argument("--strict", action="store_true", help="Exit with code 1 if any OS region exceeds --warn-pct")
    args = ap.parse_args(argv)

    os_map = args.build_dir / "my-stm32-console.map"
    if not os_map.is_file():
        print(f"Error: map file not found: {os_map}", file=sys.stderr)
        return 1

    ok_os = print_os_report(os_map, args.top, args.warn_pct)
    ok_apps = print_sdk_apps_report(args.build_dir / "sdk_apps", args.warn_pct)

    if args.strict and not (ok_os and ok_apps):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
