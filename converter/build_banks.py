#!/usr/bin/env python3
"""
Build categorized "bank" instrument files from per-instrument .conf files.

Goal
----
- You have many individual instrument .conf files under `instruments/`.
- Each bank output is a valid reMID instrument config with up to 128 programs.
- Programs are mapped to renamed instruments so multiple source files can be merged safely.

Typical use
-----------
1) Re-run conversion (updates `instruments/*.conf`)
2) Rebuild banks:
   python3 converter/build_banks.py
3) Copy `instruments/banks/*.conf` into your MOD/MODEP plugin bundle (or run `make install`)

Notes
-----
- This does NOT update `src/remid.ttl` automatically.
- It tries to be conservative and only uses the INI-ish features reMID already supports.
"""

from __future__ import annotations

from dataclasses import dataclass
from fnmatch import fnmatch
from pathlib import Path
from typing import Dict, List, Tuple
import argparse
import json
import re


SPECIAL_SECTIONS = {"channels", "programs"}


@dataclass(frozen=True)
class ParsedConf:
    path: Path
    sections: Dict[str, List[Tuple[str, str]]]  # section -> [(key, value)]

    def non_special_sections(self) -> List[str]:
        return [s for s in self.sections.keys() if s not in SPECIAL_SECTIONS]

    def has_percussion(self) -> bool:
        for sec in self.non_special_sections():
            for k, v in self.sections.get(sec, []):
                if k.strip().lower() == "type" and v.strip().lower() == "percussion":
                    return True
        return False

    def primary_instrument(self) -> str:
        """
        Pick the "main" instrument name for program mapping.

        Prefer the lowest numeric program entry in [programs]. Fallback to first
        non-special section.
        """
        progs = self.sections.get("programs", [])
        numbered: List[Tuple[int, str]] = []
        for k, v in progs:
            ks = k.strip().lower()
            if ks == "format":
                continue
            if ks.isdigit():
                numbered.append((int(ks), v.strip()))
        if numbered:
            numbered.sort(key=lambda x: x[0])
            return numbered[0][1]

        non_special = self.non_special_sections()
        if not non_special:
            raise ValueError(f"No instruments found in {self.path}")
        return non_special[0]


def load_rules(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def parse_conf(path: Path) -> ParsedConf:
    sections: Dict[str, List[Tuple[str, str]]] = {}
    cur: str | None = None
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#") or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]") and len(line) >= 3:
            cur = line[1:-1].strip()
            sections.setdefault(cur, [])
            continue
        if cur is None:
            continue
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        sections[cur].append((k.strip(), v.strip()))
    return ParsedConf(path=path, sections=sections)


def sanitize_group_name(name: str) -> str:
    # Keep it simple: g_key_file accepts plenty, but stay on safe ASCII.
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", name.strip())
    if not cleaned:
        cleaned = "instrument"
    if cleaned[0].isdigit():
        cleaned = "i_" + cleaned
    return cleaned[:80]


def unique_name(desired: str, used: set[str]) -> str:
    if desired not in used:
        used.add(desired)
        return desired
    for i in range(2, 9999):
        cand = f"{desired}_{i}"
        if cand not in used:
            used.add(cand)
            return cand
    raise RuntimeError("Unable to create unique name")


def classify(path: Path, parsed: ParsedConf, rules: dict) -> str:
    filename = path.name.lower()

    # Prefer explicit pattern rules first.
    for bank in rules.get("banks", []):
        for pat in bank.get("patterns", []):
            if fnmatch(filename, pat.lower()):
                return bank["id"]

    # If no match but it contains a percussion map, treat as drums.
    if parsed.has_percussion():
        return "drums"

    fb = rules.get("fallback_bank", {}).get("id", "misc")
    return fb


def write_bank_file(
    out_path: Path,
    items: List[ParsedConf],
    page: int,
    channels_default_program: int,
    channels_enable_all_16: bool,
) -> Dict[int, str]:
    """
    Write a single bank file with up to 128 programs.
    Returns {program_number: source_filename}.
    """
    used_groups: set[str] = set()
    program_map: Dict[int, str] = {}
    src_map: Dict[int, str] = {}

    # Build merged sections.
    merged_sections: List[Tuple[str, List[Tuple[str, str]]]] = []

    for idx, parsed in enumerate(items, start=1):
        primary = parsed.primary_instrument()
        stem = sanitize_group_name(parsed.path.stem.lower())

        rename: Dict[str, str] = {}
        for sec in parsed.non_special_sections():
            desired = sanitize_group_name(f"{stem}__{sec}")
            rename[sec] = unique_name(desired, used_groups)

        # Copy sections, rewriting percussion maps if needed.
        for sec in parsed.non_special_sections():
            new_sec = rename[sec]
            kvs: List[Tuple[str, str]] = []
            for k, v in parsed.sections.get(sec, []):
                if k.strip().lower() == "type" and v.strip().lower() == "percussion":
                    kvs.append((k, v))
                    continue
                # Percussion mappings are note-number keys with instrument-name values.
                if k.isdigit() and v in rename:
                    kvs.append((k, rename[v]))
                else:
                    kvs.append((k, v))
            merged_sections.append((new_sec, kvs))

        # Program map to the renamed primary instrument.
        if primary not in rename:
            # If [programs] pointed at something weird, fall back to first section.
            primary = parsed.non_special_sections()[0]
        program_map[idx] = rename[primary]
        src_map[idx] = parsed.path.name

    # Emit file.
    out_path.parent.mkdir(parents=True, exist_ok=True)
    lines: List[str] = []
    lines.append("# generated by converter/build_banks.py")
    lines.append(f"# bank page: {page}")
    lines.append("")

    # Channels: enable channel 1..16 with a safe default program.
    lines.append("[channels]")
    if channels_enable_all_16:
        for ch in range(1, 17):
            lines.append(f"{ch}={channels_default_program}")
    else:
        lines.append(f"1={channels_default_program}")
    lines.append("")

    # Programs: map 1..N to merged instruments.
    lines.append("[programs]")
    lines.append("format=0.0")
    for prg in sorted(program_map.keys()):
        lines.append(f"{prg}={program_map[prg]}")
    lines.append("")

    # Instruments.
    for sec, kvs in merged_sections:
        lines.append(f"[{sec}]")
        for k, v in kvs:
            lines.append(f"{k}={v}")
        lines.append("")

    out_path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")
    return src_map


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent

    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--rules",
        default=str(repo_root / "converter" / "bank_rules.json"),
        help="Path to JSON rules file",
    )
    ap.add_argument("--dry-run", action="store_true", help="Do not write output files")
    ap.add_argument(
        "--write-index",
        default=str(repo_root / "instruments" / "banks" / "banks_index.json"),
        help="Write a JSON index mapping bank/page/program to source file",
    )
    args = ap.parse_args()

    rules_path = Path(args.rules).resolve()
    rules = load_rules(rules_path)

    in_dir = (repo_root / rules.get("input_dir", "instruments")).resolve()
    out_dir = (repo_root / rules.get("output_dir", "instruments/banks")).resolve()
    max_per = int(rules.get("max_programs_per_file", 128))
    channels_default_program = int(rules.get("channels_default_program", 1))
    channels_enable_all_16 = bool(rules.get("channels_enable_all_16", True))

    if max_per < 1 or max_per > 128:
        raise SystemExit("max_programs_per_file must be 1..128")
    if channels_default_program < 1 or channels_default_program > 128:
        raise SystemExit("channels_default_program must be 1..128")

    src_files = sorted(in_dir.glob("*.conf"))
    if not src_files:
        print(f"No .conf files found in {in_dir}")
        return 1

    # Never re-ingest generated outputs.
    src_files = [p for p in src_files if p.name != "banks_index.json"]

    parsed_list: List[ParsedConf] = []
    for p in src_files:
        if p.name.lower() == "instruments.conf":
            continue
        parsed_list.append(parse_conf(p))

    # Classify.
    by_bank: Dict[str, List[ParsedConf]] = {}
    for parsed in parsed_list:
        bid = classify(parsed.path, parsed, rules)
        by_bank.setdefault(bid, []).append(parsed)

    # Deterministic ordering: by filename.
    for bid in by_bank:
        by_bank[bid].sort(key=lambda x: x.path.name.lower())

    # Always produce an "all" bank set too (useful for debugging).
    all_items = sorted(parsed_list, key=lambda x: x.path.name.lower())
    by_bank.setdefault("all", all_items)

    index: dict = {"rules": str(Path(args.rules)), "banks": {}}

    for bid in sorted(by_bank.keys()):
        items = by_bank[bid]
        pages: List[List[ParsedConf]] = [items[i : i + max_per] for i in range(0, len(items), max_per)]
        index["banks"][bid] = []

        for page_no, page_items in enumerate(pages):
            out_name = f"bank-{bid}-{page_no}.conf"
            out_path = out_dir / out_name

            if args.dry_run:
                src_map = {i + 1: page_items[i].path.name for i in range(len(page_items))}
            else:
                src_map = write_bank_file(
                    out_path=out_path,
                    items=page_items,
                    page=page_no,
                    channels_default_program=channels_default_program,
                    channels_enable_all_16=channels_enable_all_16,
                )

            index["banks"][bid].append(
                {
                    "page": page_no,
                    "file": str(Path("instruments/banks") / out_name),
                    "programs": {str(k): v for k, v in sorted(src_map.items())},
                }
            )

            print(f"{bid:8s} page {page_no}: {len(page_items):3d} -> {out_path.relative_to(repo_root)}")

    if not args.dry_run and args.write_index:
        idx_path = Path(args.write_index).resolve()
        idx_path.parent.mkdir(parents=True, exist_ok=True)
        idx_path.write_text(json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Wrote index: {idx_path}")

    if args.dry_run:
        print("Dry-run only; no files were written.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

