#!/usr/bin/env python3
"""
Analyze and disassemble the WotS2 main ELF (SLUS_208.93).
Uses mipsel-linux-gnu-objdump --architecture=mips:5900 for accurate R5900 disassembly.
"""
import sys
import subprocess
import argparse
from pathlib import Path
from elftools.elf.elffile import ELFFile

ELF_PATH    = Path(__file__).parent.parent / "extracted" / "SLUS_208.93"
OBJDUMP     = "mipsel-linux-gnu-objdump"
MIPS_ARCH   = "mips:5900"

ENTRY_POINT = 0x100008
LOAD_ADDR   = 0x100000


def disassemble(start: int, stop: int | None = None, count: int = 80) -> None:
    """Disassemble using objdump, optionally limiting to an address range or count."""
    cmd = [OBJDUMP, "-d", f"--architecture={MIPS_ARCH}", str(ELF_PATH)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        return

    printing = False
    printed  = 0
    for line in result.stdout.splitlines():
        # objdump format: "  1001a8:  42000038  ei"
        parts = line.strip().split(":")
        if parts and len(parts) >= 1:
            try:
                addr = int(parts[0].strip(), 16)
                if addr >= start:
                    printing = True
                if stop and addr >= stop:
                    break
            except ValueError:
                pass

        if printing:
            print(line)
            printed += 1
            if not stop and printed >= count:
                break


def dump_sections(elf: ELFFile) -> None:
    print(f"  {'Name':<20} {'Type':<18} {'VAddr':<12} {'Size':<10} {'Offset'}")
    for sec in elf.iter_sections():
        print(f"  {sec.name:<20} {sec['sh_type']:<18} {sec['sh_addr']:#010x}  "
              f"{sec['sh_size']:<10} {sec['sh_offset']:#x}")


def dump_strings(minlen: int = 8) -> None:
    """Print embedded ASCII strings using mipsel-linux-gnu-objdump --full-contents."""
    cmd = [OBJDUMP, "-s", "--section=", str(ELF_PATH)]
    # Fallback: use 'strings' binary directly
    result = subprocess.run(["strings", "-n", str(minlen), "-t", "x", str(ELF_PATH)],
                            capture_output=True, text=True)
    for line in result.stdout.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2:
            offset, s = parts
            addr = LOAD_ADDR + int(offset, 16)
            print(f"  {addr:#010x}  {s!r}")


def main():
    ap = argparse.ArgumentParser(description="WotS2 ELF analyzer (R5900)")
    ap.add_argument("--sections", action="store_true", help="Print section table")
    ap.add_argument("--disasm", metavar="ADDR", type=lambda x: int(x, 0),
                    default=ENTRY_POINT, help="Start address to disassemble (default: entry)")
    ap.add_argument("--stop", metavar="ADDR", type=lambda x: int(x, 0), default=None,
                    help="Stop disassembly at this address")
    ap.add_argument("--count", type=int, default=80, help="Max instructions (default: 80)")
    ap.add_argument("--strings", action="store_true", help="Dump embedded strings")
    ap.add_argument("--minlen", type=int, default=8, help="Minimum string length for --strings")
    args = ap.parse_args()

    with open(ELF_PATH, "rb") as f:
        elf = ELFFile(f)
        print(f"ELF: {ELF_PATH.name}  entry={elf.header.e_entry:#010x}  "
              f"machine={elf.header.e_machine}  flags={elf.header.e_flags:#010x}")
        print()

        if args.sections:
            print("=== Sections ===")
            dump_sections(elf)
            print()

    if args.strings:
        print(f"=== Strings (min length {args.minlen}) ===")
        dump_strings(args.minlen)
    else:
        print(f"=== Disassembly @ {args.disasm:#010x} "
              f"({'stop=' + hex(args.stop) if args.stop else 'count=' + str(args.count)}) ===")
        disassemble(args.disasm, args.stop, args.count)


if __name__ == "__main__":
    main()
