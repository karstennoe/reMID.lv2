# SID-Wizard (\.swi) ➜ reMID (\.conf) Converter

This repo contains:

- *swi2remid.py*: a high-fidelity converter that turns **SID-Wizard 1.7** instrument *.swi* files into **reMID** *.conf* presets.
- *converter/convert_all.py*: a batch driver that converts multiple *.swi* files, writes *.conf* files to the repo’s *instruments/* folder, and optionally updates *src/remid.ttl* with new *pset:Preset* entries.

---

## Repo layout & assumptions

Run tools from the repo root; expected structure:

- *converter/swi2remid.py*
- *converter/convert_all.py*
- *converter/sidwizard_instruments/*.swi* — inputs
- *instruments/* — outputs (*.conf*)
- *src/remid.ttl* — preset manifest to be updated by *convert_all.py* (optional)

---

## Quick start

1) Convert a single instrument:

- `python converter/swi2remid.py --in converter/sidwizard_instruments/arp-major.swi --out instruments/arp-major.conf`

2) Batch convert all *.swi* and update the manifest:

- `python converter/convert_all.py --force --ttl src/remid.ttl`

---

## swi2remid.py — usage

### Basic invocation

- `python converter/swi2remid.py --in INPUT.swi --out OUTPUT.conf`

### Arguments

| Flag | Type | Default | What it does |
|---|---:|---:|---|
| --in | path | *required* | Input *.swi* file (SW 1.7 layout assumed). |
| --out | path | *required* | Output *.conf* file (reMID preset). |
| --name | str | auto | Override preset name (otherwise read from trailing 8 bytes or file stem). |
| --program-speed | int | 50 | reMID program tick rate (50 ≈ PAL frames/s). |
| --speed-mult | int | 1 | Scales program speed (integer multiplier). |
| --arp-plus1 | flag | off | Adds +1 to SID-Wizard ARP step length per row (rarely needed). |
| --strict-wf | flag | off | Use waveform/control bytes verbatim; only TEST bit cleared when off. |
| --no-emit-arp | flag | off | Ignore ARP entirely (steady pitch). |
| --hard-restart | flag | off | Emit a short TEST+GATE jab at start. |
| --sustain-frames | int | 64 | Length of the “sustain horizon” for one-shot patches. |

**Fidelity toggles** (defaults chosen to reduce “noise→tone” pops and sustain repops):

| Flag | Type | Default | What it does |
|---|---:|---:|---|
| --no-filter-on-tonal | flag | off | When off, first \*filter\_cutoff\* is delayed until the first **tonal** frame (not NOISE). |
| --no-oneshot-if-steady-wf | flag | off | When off, if WF and ARP are steady after attack, treat as **one-shot** (don’t loop WF FE). |
| --respect-gateoff | flag | off | Honor \*gate-off\* indices from header (WF/PW/FL), auto-clears GATE mid-note. |

**Vibrato LFO** (optional):

| Flag | Type | Default | What it does |
|---|---:|---:|---|
| --enable-vibrato | flag | off | Enables header-derived vibrato. |
| --vib-depth | float | header | Override vibrato depth in semitones. |
| --vib-delay | int | header | Frames to wait before vibrato starts. |
| --vib-rate-frames | int | 4 | LFO speed control (bigger = slower; triangle default). |
| --vib-shape | tri\|sine | tri | LFO waveform. |

**Filter calibration** (to match your engine’s feel):

| Flag | Type | Default | What it does |
|---|---:|---:|---|
| --cutoff-scale | float | 1.0 | Multiplies all cutoff values; clamped to 0x000..0x7FF. |
| --res-scale | float | 1.0 | Scales resonance nibble before packing into \*fr\_vic\*. |

### What the converter actually does

- Parses **WF/ARP**, **PW**, **Filter** tables including **FE** pointer jumps and **FF** terminators.  
  Watchdogs prevent infinite loops on self/invalid FE pointers (fallback: HOLD).
- **PW**: absolute sets and duration/slope sweeps; single-absolute tables are held.  
- **Filter**: control/absolute/sweep rows; cutoff clamped; mode (LP/BP/HP) mapped; resonance/routing packed into \*fr\_vic\*.  
- **ARP**: relative up/down and NOP handled; **ABS** (0x81..0xDF) and **CHORD** (0x7F) are currently held (see deviations).  
- Emits minimal preset with \[channels]/\[programs] header and a single instrument block.  
- Loops **after** the initial ARP seed and inserts a single wrap-correction to avoid per-cycle pitch drift.

---

## convert_all.py — usage

Batch convert all *.swi* in *converter/sidwizard_instruments/* and write *.conf* to *instruments/*.

### Basic invocation

- `python converter/convert_all.py`

### Arguments

| Flag | Type | Default | What it does |
|---|---:|---:|---|
| --suffix | str | "" | Appended before *.conf* (e.g., \_remid). |
| --force | flag | off | Always overwrite existing *.conf* files. |
| --ttl | path | none | If given, update *src/remid.ttl* with any **new** presets (idempotent). |
| --converter | path | auto | Path to *swi2remid.py* if you keep it elsewhere. |
| --opts | str | "" | Extra args to pass verbatim to *swi2remid.py* (quote as one string). |

### Examples

1) Convert with conservative defaults:

- `python converter/convert_all.py`

2) Force overwrite and update manifest:

- `python converter/convert_all.py --force --ttl src/remid.ttl`

3) Pass options through to the converter:

- `python converter/convert_all.py --opts "--enable-vibrato --respect-gateoff --cutoff-scale 0.9"`

### TTL update behavior

When `--ttl` is provided, the batch script:

1. Parses *src/remid.ttl* for existing \*pset:Preset\* entries.  
2. For each generated *.conf*, adds a new preset block if not present:

- `lv2:appliesTo <http://github.com/ssj71/reMID.lv2>`  
- `rdfs:label "NAME"` taken from the *.conf* instrument block  
- `state:state [ <.../instruments/instruments.conf> <FILENAME.conf> ]`

It does not remove or reorder existing presets.

---

## Recommended recipes

- **Baseline** (musically safe):  
  `python converter/swi2remid.py --in X.swi --out X.conf`  
  (delays first filter set to the first tonal frame; one-shot sustain when WF/ARP are steady)

- **Plucks that auto-release**:  
  `... --respect-gateoff`

- **Classic “SID-ish” wobble**:  
  `... --enable-vibrato --vib-rate-frames 6`

- **Filter feels too bright/dark**:  
  `... --cutoff-scale 0.85` or `1.15`

- **Reproduce raw behavior** (if a patch regresses):  
  `... --no-filter-on-tonal --no-oneshot-if-steady-wf`

---

## What’s still missing / deviating from SID-Wizard

The converter aims for high fidelity but a few behaviors can’t be reproduced **inside a single reMID preset**:

1. **ABSOLUTE note ARP** (0x81..0xDF) — *held*  
   reMID instruments use **relative** pitch steps (`v1_freq_hs` deltas).  
   True absolute notes require either per-root preset banks or host retuning.  
   Converter currently **holds** pitch on those rows.

2. **CHORD call** (0x7F) — *held*  
   SW chord tables live in **.swm** projects, not in *.swi*.  
   Without the song’s chord table, chord steps can’t be expanded exactly; converter **holds**.

3. **SYNC/RING dependencies across oscillators**  
   Single-voice presets can’t replicate interactions that require another oscillator’s phase/pitch.  
   Multi-instance setups would be needed.

4. **Exact ADSR “bug” and hard-restart timing**  
   The famous SID envelope quirks and TEST timing differ by engine; converter’s optional `--hard-restart` is an approximate jab.

5. **Filter curve/resonance law**  
   Different engines map cutoff/resonance differently.  
   Use `--cutoff-scale` / `--res-scale` for per-project calibration.

6. **Dynamic per-row arp speed changes**  
   If a given SW build modulates arp timing inside the WF table, that’s not decoded yet; step length is currently uniform per instrument.

7. **Noise LFSR phase**  
   Subtle “grit”/randomness won’t bit-match a C64 SID. Engine detail, not addressable in the preset DSL.

---

## Troubleshooting

- **“Arp feels slow”** → confirm source arp speed byte; ensure host tempo vs `program_speed`; avoid `--arp-plus1` unless required.  
- **“Click then fade-in”** → ensure `filter-on-tonal` is enabled (default) and try `--respect-gateoff`.  
- **“Too bright/dull sweep”** → adjust `--cutoff-scale` (e.g., 0.85 or 1.15).  
- **“PWM inaudible”** → some patches set very low PW; the converter clamps PW ≥ 0x001 on frame 0.  
- **Batch didn’t update manifest** → pass `--ttl src/remid.ttl`; the script is idempotent and won’t duplicate entries.

---

## License

Same as the parent project unless specified otherwise.

**Contributions welcome**: If you can share a matching *.swm* for chord-call instruments (0x7F), we can add an optional `--chord-json` path to expand chords exactly.
