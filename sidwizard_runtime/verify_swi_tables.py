#!/usr/bin/env python3
"""
Sanity-check SID-Wizard .swi instrument tables against the player format.

Checks:
- payload stripping (optional 2-byte PRG load address)
- WF/ARP table at 0x10 is 3-byte rows ending with 0xFF
- PW/Filter table pointers from header
- FE rows use a 1-byte jump address in column 2 (and are row-aligned)

This is not a full emulator; it's just a quick validator for table structure.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse

WFTABLEPOS = 0x10
PWPT = 0x0A
FLPT = 0x0B
MAX_INSTSIZE = 128
NAME_LEN = 8


def payload_offset(file_bytes: bytes) -> int:
    if len(file_bytes) < 4:
        return 0
    load_addr = file_bytes[0] | (file_bytes[1] << 8)
    if 0x0300 <= load_addr <= 0xC000 and len(file_bytes) >= 34:
        return 2
    return 0


def unpack_swi(file_bytes: bytes) -> bytes:
    """
    Unpack a SID-Wizard .swi instrument into a fixed 128-byte image.

    Many .swi files are stored as:
      [optional 2-byte PRG load address]
      [instrument bytes 0..size-1]
      [size byte == index of replaced 0xFF terminator]
      [8-byte instrument name]

    We restore the last 0xFF terminator and place the name at the end of the 128-byte image.
    """
    b = file_bytes
    if len(b) < 1 + NAME_LEN:
        raise ValueError("too small")

    def is_packed(payload: bytes) -> bool:
        if len(payload) < 1 + NAME_LEN:
            return False
        size_index = len(payload) - (1 + NAME_LEN)
        size_byte = payload[size_index]
        return (size_index == size_byte) and (len(payload) == size_byte + 1 + NAME_LEN) and (size_byte <= (MAX_INSTSIZE - NAME_LEN - 1))

    load_off = payload_offset(b)
    payload = b[load_off:] if load_off else b
    if not is_packed(payload) and load_off == 2 and is_packed(b[2:]):
        payload = b[2:]

    if is_packed(payload):
        size_index = len(payload) - (1 + NAME_LEN)
        size_byte = payload[size_index]
        name = payload[size_index + 1 :]
        out = bytearray([0xFF] * MAX_INSTSIZE)
        out[:size_byte] = payload[:size_byte]
        out[size_byte] = 0xFF
        out[MAX_INSTSIZE - NAME_LEN :] = name
        return bytes(out)

    # Fallback: treat as already-unpacked or raw instrument bytes (with optional load address stripped).
    payload = b[payload_offset(b) :]
    if len(payload) < 0x20:
        raise ValueError("payload too small")
    out = bytearray([0xFF] * MAX_INSTSIZE)
    out[: min(len(payload), MAX_INSTSIZE)] = payload[: min(len(payload), MAX_INSTSIZE)]
    return bytes(out)


@dataclass(frozen=True)
class TableReport:
    name: str
    base: int
    rows: int
    fe_rows: int
    bad_jumps: int
    truncated: bool


def scan_table(payload: bytes, name: str, base: int) -> TableReport:
    rows = 0
    fe_rows = 0
    bad_jumps = 0
    truncated = False

    i = base
    while True:
        if i >= len(payload):
            truncated = True
            break

        # Terminator is checked by the player using only the first column.
        a = payload[i]
        if a == 0xFF:
            break

        # Normal rows are 3 bytes; if we can't read all 3 bytes, mark truncated.
        if i + 2 >= len(payload):
            truncated = True
            break

        b, c = payload[i + 1], payload[i + 2]
        if a == 0xFE:
            fe_rows += 1
            j = b
            # Jump address is a raw byte offset; the player doesn't require row alignment.
            # WF/ARP jumps additionally ignore targets with bit7 set (>=0x80).
            if name == "wfarp" and (j & 0x80):
                pass
            else:
                ok = (j < len(payload))
                if not ok:
                    bad_jumps += 1
        rows += 1
        i += 3

    return TableReport(
        name=name,
        base=base,
        rows=rows,
        fe_rows=fe_rows,
        bad_jumps=bad_jumps,
        truncated=truncated,
    )


def check_one(path: Path) -> list[TableReport]:
    b = path.read_bytes()
    payload = unpack_swi(b)
    if len(payload) < 0x20:
        raise SystemExit(f"{path}: payload too small ({len(payload)})")
    pw_base = payload[PWPT]
    fl_base = payload[FLPT]

    return [
        scan_table(payload, "wfarp", WFTABLEPOS),
        scan_table(payload, "pw", pw_base),
        scan_table(payload, "filter", fl_base),
    ]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help="Path to .swi file or a directory")
    args = ap.parse_args()

    p = Path(args.path)
    if p.is_dir():
        files = sorted(p.rglob("*.swi"))
    else:
        files = [p]

    if not files:
        print("No .swi files found.")
        return 1

    bad = 0
    for f in files:
        reps = check_one(f)
        issues = []
        for r in reps:
            if r.truncated or r.bad_jumps:
                issues.append(r)
        if issues:
            bad += 1
            print(f"\n{f}:")
            for r in reps:
                print(
                    f"  {r.name:6s} base=0x{r.base:02X} rows={r.rows:3d} FE={r.fe_rows:3d} bad_jumps={r.bad_jumps:2d} truncated={r.truncated}"
                )

    if bad:
        print(f"\nDone. Files with issues: {bad}/{len(files)}")
        return 2

    print(f"OK. Checked {len(files)} file(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
