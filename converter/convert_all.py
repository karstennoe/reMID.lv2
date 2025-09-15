#!/usr/bin/env python3
"""
convert_all.py — batch-convert SID-Wizard .swi instruments to reMID .conf
and update LV2 presets in src/remid.ttl (add if not present).

Repo layout (run from anywhere):
  <repo>/
    converter/swi2remid.py
    converter/sidwizard_instruments/*.swi   (inputs)
    instruments/                            (outputs)
    src/remid.ttl                           (preferred manifest)
    remid.ttl                               (fallback)

Usage:
  python converter/convert_all.py
  python converter/convert_all.py --suffix _remid
  python converter/convert_all.py --force
  python converter/convert_all.py --manifest src/remid.ttl
  # pass-through sound options (see --help)
"""

from pathlib import Path
import argparse, importlib.util, sys, re

# --- utils -------------------------------------------------------------------

def load_converter(mod_path: Path):
    spec = importlib.util.spec_from_file_location("swi2remid", str(mod_path))
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot import converter from {mod_path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[attr-defined]
    return mod

def safe_label(s: str) -> str:
    return re.sub(r'\s+', ' ', s).strip() or "instrument"

def find_manifest(repo_root: Path, override: str | None) -> Path:
    if override:
        return (repo_root / override).resolve()
    pref = repo_root / "src" / "remid.ttl"
    if pref.exists():
        return pref
    return (repo_root / "remid.ttl").resolve()

def preset_block(filename_conf: str, label: str) -> str:
    subj = f"<http://github.com/ssj71/reMID.lv2/blob/master/instruments/{filename_conf}>"
    state_key = "<http://github.com/ssj71/reMID.lv2/blob/master/instruments/instruments.conf>"
    return (
        f"{subj}\n"
        f"\ta pset:Preset ;\n"
        f"\tlv2:appliesTo <http://github.com/ssj71/reMID.lv2> ;\n"
        f"\trdfs:label \"{label}\" ;\n"
        f"\tstate:state [\n"
        f"\t\t{state_key} <{filename_conf}>\n"
        f"\t] .\n"
    )

def manifest_has_preset(text: str, filename_conf: str) -> bool:
    return (f"/instruments/{filename_conf}>" in text) or \
           (f"instruments.conf> <{filename_conf}>" in text)

def append_presets_to_manifest(manifest_path: Path, additions: list[tuple[str, str]]) -> int:
    if not additions:
        return 0
    text = manifest_path.read_text(encoding="utf-8") if manifest_path.exists() else ""
    appended = 0
    blocks = []
    for filename_conf, label in additions:
        if manifest_has_preset(text, filename_conf):
            continue
        blocks.append(preset_block(filename_conf, label))
        appended += 1
    if appended:
        if text and not text.endswith("\n"): text += "\n"
        if text and not text.endswith("\n\n"): text += "\n"
        text += "\n".join(blocks)
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(text, encoding="utf-8")
    return appended

# --- main --------------------------------------------------------------------

def main():
    repo_root = Path(__file__).resolve().parent.parent
    print(f"Repo root: {repo_root}")

    converter_py = repo_root / "converter" / "swi2remid.py"
    in_dir       = repo_root / "converter" / "sidwizard_instruments"
    out_dir      = repo_root / "instruments"

    swi2remid = load_converter(converter_py)

    ap = argparse.ArgumentParser()
    ap.add_argument("--suffix", default="", help="Filename suffix before .conf (e.g. _remid)")
    ap.add_argument("--overwrite", action="store_true", help="Overwrite existing files")
    ap.add_argument("--force", action="store_true", help="Alias for --overwrite")
    ap.add_argument("--manifest", default=None, help="Path under repo root to remid.ttl")
    # pass-through converter options (sane defaults)
    ap.add_argument("--program-speed", type=int, default=50)
    ap.add_argument("--speed-mult", type=int, default=1)
    ap.add_argument("--arp-plus1", action="store_true")
    ap.add_argument("--strict-wf", action="store_true")
    ap.add_argument("--no-emit-arp", action="store_true")
    ap.add_argument("--no-hard-restart", action="store_true")
    ap.add_argument("--sustain-frames", type=int, default=64)
    args = ap.parse_args()

    overwrite = args.overwrite or args.force

    if not in_dir.is_dir():
        print(f"Input folder not found: {in_dir}", file=sys.stderr)
        sys.exit(1)
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest_path = find_manifest(repo_root, args.manifest)
    print(f"Manifest to update: {manifest_path}")

    swi_files = sorted(in_dir.glob("*.swi"))
    if not swi_files:
        print(f"No .swi files found in {in_dir}")
        return

    converted = skipped = failed = 0
    to_manifest: list[tuple[str, str]] = []

    for swi_path in swi_files:
        try:
            print(f"Processing {swi_path.name}...")
            payload = swi2remid.read_payload(swi_path)
            derived_name = (payload[-8:].decode("ascii", "ignore").strip() or swi_path.stem)
            conf_text = swi2remid.emit(
                derived_name, payload,
                program_speed=args.program_speed,
                speed_mult=args.speed_mult,
                arp_plus1=args.arp_plus1,
                strict_wf=args.strict_wf,
                emit_arp=not args.no_emit_arp,
                hard_restart=not args.no-hard_restart if hasattr(args, "no-hard_restart") else not args.no_hard_restart,
                sustain_frames=max(1, args.sustain_frames),
            )

            out_name = f"{swi_path.stem}{args.suffix}.conf"
            out_path = out_dir / out_name

            if out_path.exists() and not overwrite:
                print(f"SKIP (exists)  {swi_path.name} -> {out_path.relative_to(repo_root)}")
                skipped += 1
            else:
                out_path.write_text(conf_text, encoding="utf-8")
                print(f"OK            {swi_path.name} -> {out_path.relative_to(repo_root)}")
                converted += 1

            # Label = the readable instrument name
            to_manifest.append((out_name, safe_label(derived_name)))

        except Exception as e:
            print(f"FAIL          {swi_path.name}: {e}", file=sys.stderr)
            failed += 1

    # Dedup by filename; last label wins
    dedup = {}
    for fn, lb in to_manifest:
        dedup[fn] = lb
    additions = list(dedup.items())
    appended = append_presets_to_manifest(manifest_path, additions)

    print(f"\nDone. Converted: {converted}, Skipped: {skipped}, Failed: {failed}")
    if appended:
        print(f"Updated manifest: appended {appended} preset(s) → {manifest_path}")
    else:
        print("Manifest already had all presets; no changes made.")

if __name__ == "__main__":
    main()
