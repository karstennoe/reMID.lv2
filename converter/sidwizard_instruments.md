# SID-Wizard instrument playback: a practical deep-dive

This is a developer-oriented guide to how SID-Wizard (SW) drives a single instrument over time. It distills what you need when converting SW instruments to other formats (e.g., reMID) while staying faithful to the player logic you shared. Where implementation details vary by version, that is called out explicitly.

---

## 1) Instrument payload layout (per-instrument, SW 1.7 family)

An exported \`.swi\` contains a small header followed by three byte-triplet tables. Offsets below are *relative to the start of the instrument payload* (after removing an optional 2-byte PRG load address).

| Byte/field | Meaning | Notes |
|---|---|---|
| 0x03 | \`AD\` | Attack nibble \|\| Decay nibble (C64 SID ADSR) |
| 0x04 | \`SR\` | Sustain level nibble \|\| Release nibble |
| 0x05..0x06 | \`PW init\` (common layout) | Often initial 12-bit pulse width, lo then hi (mask 0x0FFF) |
| 0x07 | \`ARP speed\` | Low 6 bits typically used as frames-per-step; value 0 is commonly treated as 0 or 1 depending on build |
| 0x08 | \`Default chord / aux\` | Used by chord opcodes in some builds; not always present in \`.swi\` |
| 0x09 | \`Octave / aux\` | Build-dependent |
| 0x0A | \`PW table ptr\` | Byte offset of PW table within the instrument payload |
| 0x0B | \`Filter table ptr\` | Byte offset of Filter table |
| 0x0C..0x0E | \`Gate-off indices\` (optional) | Some builds indicate when to release ADSR relative to WF/PW/F tables |
| 0x0F | \`WF0\` | First frame’s SID control register value (waveform flags etc.) |
| 0x10.. | \`WF/ARP table\` | Triplets begin here: \`(left, right, third)\` per row |
| \`PW ptr\`.. | \`PW table\` | Triplets \`(left, right, third)\` per row |
| \`FL ptr\`.. | \`Filter table\` | Triplets \`(left, right, third)\` per row |

Each row in any table is 3 bytes. Table end is encoded by a row whose first byte is \`0xFF\`. Jumps within a table are encoded with \`0xFE <lo> <hi>\` and use absolute addressing.

---

## 2) SID control register byte (what \`WF\` rows write)

The control byte combines waveform selection, gate, test, sync, ring. Bits are standard SID:

| Bit | Hex | Meaning |
|---:|---:|---|
| 0 | 0x01 | GATE (1 = key down) |
| 1 | 0x02 | SYNC (osc2 → osc1) |
| 2 | 0x04 | RING (osc2→osc1 ringmod) |
| 3 | 0x08 | TEST (forces osc to reset, mutes waveform output) |
| 4 | 0x10 | TRIANGLE |
| 5 | 0x20 | SAW |
| 6 | 0x40 | PULSE |
| 7 | 0x80 | NOISE |

Typical idioms:
- Noise tick at note-on: first row uses NOISE+GATE (e.g., \`0x81\`), then switches to tonal waveform (e.g., \`0x41\` = PULSE+GATE).
- Hard restart: briefly toggling TEST+GATE or similar to re-phase the oscillator before real attack (player-dependent).

Converter tip: when mirroring control writes, clear TEST unless you intentionally emulate a hard-restart. Do **not** force a waveform if none is set.

---

## 3) Per-frame scheduling: timing and step progression

At the instrument level, the WF/ARP table is advanced in discrete steps, each consuming “step frames” of audio time:

- “Step frames” = function of the “ARP speed” header byte (low 6 bits). Many builds interpret this as “wait N frames per row”. Some authoring styles assume \+1 frame; if your output sounds uniformly slower than SW, try “step frames = arp speed \+ 1” for those instruments only.
- On each frame:
  1) If the WF/ARP step timer expires, fetch the next WF/ARP row (or process its FE/FF).
  2) Apply WF changes (SID control byte).
  3) Apply ARP pitch change for the new row (see section 5).
  4) Update PW (pulse-width) engine using the PW table (which can be on its own schedule; SW evaluates PW every frame with its own row durations).
  5) Update Filter (cutoff/mode/res/routing) engine from the Filter table.
  6) Write effective values to SID registers; ADSR envelope naturally progresses per SID.

The three tables are independent state machines that advance according to their own row encodings.

---

## 4) FE/FF row semantics and pointer math

Every table (WF/ARP, PW, Filter) recognizes two meta-rows:

- \`FF xx xx\` → End: stop advancing this table; keep last value (HOLD).
- \`FE <lo> <hi>\` → Jump to absolute byte address inside the *same table*. To convert FE to a row index:
  - \`ptr = (hi << 8) \| lo\`
  - \`row_index = (ptr - table_base) / 3\`  (must be integer; otherwise ignore or clamp)
  - \`table_base = 0x10\` for WF/ARP; \`PW ptr\` for PW; \`FL ptr\` for Filter.

Corner cases to handle explicitly:
- FE that points to itself or two FEs that jump between each other → infinite loop with no time consumed. Guard with a watchdog and treat as HOLD for the remainder of the horizon.
- FE that points before \`table_base\` or past the last row → invalid; either ignore (fall through) or loop to the first row.

---

## 5) WF/ARP row: waveform write \+ arpeggio opcode

Each WF/ARP row holds three bytes “(left, right, third)”:
- “left” is the SID control byte.
- “right” is the ARP byte.
- “third” is an auxiliary byte (used in some builds, and as FE jump hi-byte).

### 5.1 ARP byte decoding (spec-style mapping)

The ARP byte can encode *relative* pitch, “no op”, *absolute* note selection, or a *chord call*. A robust mapping seen across SW player code is:

- \`0x00 .. 0x7E\` → **relative up** by \+a semitones (from the played note).
- \`0x80\` → **NOP** (hold current relative offset).
- \`0x81 .. 0xDF\` → **absolute note index** into the player’s pitch table. This selects an absolute pitch independent of the played key. A converter that only supports relative deltas cannot reproduce this faithfully inside an instrument; safest fallback is to keep the previous offset for that row.
- \`0xE0 .. 0xFF\` → **relative down** by (a \- 256) semitones (two’s complement range \-32..\−1).
- \`0x7F\` → **Chord call**: invoke a chord sequence defined in the song/instrument. Requires access to external chord data; not shipped inside minimal \`.swi\` exports in many cases. When unavailable, hold the previous offset.

Important: The WF/ARP table also uses \`FE\` and \`FF\` in “left”. Do not mix these with the “right” byte’s range checks.

### 5.2 Loop stability: seed, delta, wrap

For a repeating arpeggio to avoid pitch drift:
- Apply any **initial seed** (first row’s absolute or relative offset) once.
- Ensure the loop body (from the row after the seed to the loop target) sums to **zero semitone delta** per cycle. If not, insert a one-shot **wrap correction** at the loop end: “add (target absolute offset \- current absolute offset)” semitones, then jump.
- When FE loops to the very first row, jump back **after** the seed so it is not re-applied each cycle.

---

## 6) PW (pulse width) table: absolute sets and sweeps

PW rows are “(left, right, third)” with these encodings:

- **Absolute set**: “left & 0x80 != 0”  
  “PW = ((left & 0x0F) << 8) \| right”.  
  Consumes 1 frame, then **holds** until another row changes it. This is the most common way to establish an initial audible PW.

- **Sweep**: “left & 0x80 == 0” and “left not in {0xFE, 0xFF}”  
  “duration = left” frames, “slope = int8(right)” per frame.  
  Each frame: “PW \+= slope” (clamp to 1..0xFFF).

- **FE** and **FF** as in section 4.

Edge cases:
- A table with a **single absolute** row and no sweeps should be treated as **HOLD** forever (no chatter from re-emitting the same value).
- Some instruments imply an initial PW in header bytes 5/6; prefer the **first absolute row** if present, else fall back to header guess, then to “0x800”.

---

## 7) Filter table: control/set/sweep rows

Filter rows are “(left, right, third)”. There are three functional types plus FE/FF:

- **CONTROL row**: either “left & 0x80 != 0” *or* “third & 0x80 != 0”  
  Decode:
  - “band = (left >> 4) & 0x07” → mode mapping: 1 = LP, 2 = BP, 3 = HP (others clamp to LP).
  - “res = left & 0x0F”.
  - “route = third & 0x07” (voice routing bits; if zero, players often default to voice 1).
  - “fine = (third >> 4) & 0x07”.
  - **Cutoff set**: “cutoff = (right << 3) \| fine” (approximate 11-bit domain).
  The row **consumes one frame**.

- **ABSOLUTE cutoff set**: “left == 0x00”  
  “cutoff = (right << 3)”, consumes one frame.

- **SWEEP**: otherwise  
  “duration = left” frames, “slope = int8(right) * 8” per frame → “cutoff \+= slope”, clamp to 0..0x7FF.

- **FE / FF**: as in section 4.

Converter tips:
- The audible “pop” on plucked sounds is a short “open then clamp” filter motion: a CONTROL or ABSOLUTE set to high cutoff followed by a brief negative SWEEP. Reproduce the timing faithfully.
- If you detect a waveform switch from NOISE→tonal on the first two WF rows, place the first meaningful cutoff set **on the tonal frame** to avoid a micro-mute between attack and sustain.

---

## 8) ADSR envelope and gate off

ADSR follows the SID’s hardware behavior using the “AD” and “SR” nibbles:
- Attack 0 is instantaneous; high sustain levels keep tone present after the initial decay.
- Release begins when GATE is cleared. Some SW builds support explicit gate-off indices in the instrument header, e.g., “turn GATE off when the WF/PW/FLT tables reach certain rows”. If those header fields are unused, gate-off is controlled by pattern data (note off) in the song.

Converter tip: Do not toggle GATE unless you are explicitly emulating a hard-restart or a scripted release; rely on note off from the host.

---

## 9) Vibrato, detune, and other per-instrument pitch mod

Some SW versions support an instrument vibrato (with parameters such as depth and delay) applied as an LFO on pitch. Its state machine runs alongside ARP; in many shipped patches it is not used. If you need parity:
- Implement a simple triangle/sine LFO updated per frame, started after a delay counter, adding to the current semitone offset before writing frequency.
- If you lack vibrato parameters in the \`.swi\`, leave it disabled.

---

## 10) Per-frame update order (safe approximation)

A practical ordering that matches audible results:

1) If step timer expired, fetch next WF/ARP row (or process its FE/FF).
2) Apply “control” (SID waveform byte), taking care with TEST and GATE.
3) Decode ARP byte; if it yields a valid relative offset, apply as “semitones” change. For absolute/chord opcodes with no external tables, hold prior offset.
4) Step the PW table once (may consume multiple frames if sweep duration \> 1).
5) Step the Filter table once (control/set/sweep).
6) Write PW, cutoff, and control to SID; the hardware ADSR evolves automatically.
7) Handle WF/ARP loop: apply a single wrap correction to make the absolute ARP offset at loop entry match the target, then jump. When the loop returns to row 0, jump to the line *after* applying the initial seed.

---

## 11) Common pitfalls and how to avoid them

- **Wrong FE semantics** → Sequences drift or hang. Always convert FE pointer to row index: “(ptr − base) \/ 3”. Guard against self-loops and out-of-range FE.
- **Reseeding every loop** → Arpeggios descend by the seed each cycle. Loop back **after** the initial seed.
- **Non-zero loop sum** → Arpeggios climb or fall over time. Insert a one-shot **wrap correction** before “goto”.
- **Forcing a waveform** → Clearing TEST is fine; forcing PULSE when none is set can mute NOISE/SAW/TRI phases incorrectly.
- **PW too small** → Audible only as clicks. Honor the first absolute set and clamp PW to “\>= 1”.
- **Filter clamped to zero** → Silence then fade-in. Place the first significant cutoff set on the tonal frame (post-noise), and don’t impose global floors unless a specific patch requires it.
- **Assuming global speed multipliers** → Many SW instruments are authored for per-row frame counts baked into “ARP speed”. Changing host tick rate globally can make everything feel slow or fast.

---

## 12) Translating to reMID (one voice) reliably

- Map SID control writes 1:1, but clear TEST unless intentionally emulated.
- Implement ARP as “v1\_freq\_hs” relative semitone deltas:
  - On frame 0: emit the seed if non-zero.
  - Within the loop: emit only deltas between frames.
  - Before “goto”: emit wrap correction “target − current”.
- Emit PW only when it changes (avoid chatter); treat single absolute as HOLD.
- Emit Filter cutoff on changes; pack mode/res/routing when CONTROL rows appear.
- If WF table lacks a musical loop (e.g., one-shot pop→sustain), don’t force a WF loop; instead, hold the last waveform and keep evolving PW/Filter in a small sustain loop.
- Keep “program\_speed” equal to PAL 50Hz unless you have evidence a patch is authored for a different step pacing.

---

## 13) Debugging checklist for a suspect conversion

- Does the WF/ARP loop “goto” jump to **after** the seed on row 0? If not, fix the loop target.
- Does the per-cycle sum of “v1\_freq\_hs” deltas equal 0? If not, add a wrap correction.
- Are there any “FE” rows pointing to themselves or each other? If yes, guard and fall back to HOLD.
- Is the first audible PW a reasonable nonzero value? If not, prefer the first absolute set; otherwise use header PW or 0x800.
- Does the first meaningful Filter set land on the tone frame (not the noise frame)? If not, move it to avoid a micro dip.

---

## 14) Minimal reference of row encodings

WF/ARP table (“left,right,third”):

- “left == 0xFF” → end; hold.
- “left == 0xFE” → jump to “(right,third)” absolute address.
- else:
  - write “control = left”.
  - decode ARP “right”: rel up \[0x00..0x7E\], NOP \[0x80\], ABS \[0x81..0xDF\], chord \[0x7F\], rel down \[0xE0..0xFF\].
  - auxiliary “third” is FE hi-byte for FE rows; otherwise build-dependent.

PW table:

- “left == 0xFF” → end; hold.
- “left == 0xFE” → FE jump.
- “left & 0x80” → absolute set: “PW = ((left & 0x0F) << 8) \| right” (1 frame).
- else → sweep: “duration = left”, “slope = int8(right)” per frame.

Filter table:

- “left == 0xFF” → end; hold.
- “left == 0xFE” → FE jump.
- CONTROL row if “(left & 0x80) or (third & 0x80)”: set mode/res/route and cutoff “(right << 3) \| ((third >> 4) & 7)”.
- ABS set if “left == 0x00”: cutoff “right << 3”.
- else SWEEP: “duration = left”, “slope = int8(right) * 8”.

---

## 15) Version notes and caveats

- Exact ARP opcode ranges and meanings come from SW player code you shared and can vary slightly between releases. The mapping above works for the instruments you provided.
- Absolute-note and chord opcodes cannot be represented purely with relative pitch inside a reMID instrument; reproducing them bit-exactly requires either host-side note retuning or pre-expanding chord data.
- Gate-off indices in the header (if present) let the player release ADSR automatically at a specific step; when absent, note off from the pattern controls release.

---

## 16) TL;DR for implementers

- Treat all three tables as independent finite state machines with FE/FF.
- Convert FE pointers to row indices via “(ptr − base) \/ 3”.
- Decode ARP opcodes; emit only relative semitone deltas; fix loops with one wrap step and loop after the seed.
- PW: first absolute wins; sweeps are “duration, slope per frame”.
- Filter: control rows set mode/res/routing and cutoff; sweeps use “int8(right) * 8”.
- Don’t force waveforms; only clear TEST.
- Keep global timing conservative (50Hz; no multipliers) unless a specific patch proves otherwise.

This reference should give you everything you need to reason about how an SW instrument evolves in time and to produce faithful conversions that neither drift in pitch nor “breathe” incorrectly.
