#!/usr/bin/env python3
# ====================================================================================
# swi2remid.py — SID-Wizard .swi → reMID .conf converter
#
# Goals / design choices
# ----------------------
# 1) Fidelity first: Follow how SID-Wizard instruments evolve over time.
#    - Respect FE pointer jumps in tables (WF/ARP, PW, Filter).
#    - Avoid infinite loops when FE jumps form self/2-row loops (watchdogs).
#    - Decode PW absolute and sweeps correctly; hold single absolute PW.
#    - Detect filter "control" rows reliably; render control/absolute/sweep rows.
#    - ARP bytes are interpreted per SW player behavior (see ARP decoder).
# 2) Stable arpeggios:
#    - Loop AFTER the initial ARP seed so we don't reseed every cycle.
#    - Insert a single wrap-correction before looping so per-cycle pitch sum is 0.
# 3) Conservative: do NOT force a waveform; only clear TEST bit.
#
# Output format
# -------------
# A single .conf file with a minimal [channels]/[programs] header and one
# instrument block. Emits only when values change frame-to-frame.
#
# NOTE
# ----
# This script assumes reMID's "v1_*" commands (voice 1) and filter ops:
#   - v1_control <sid ctrl byte>
#   - v1_pulse   <12-bit PW>
#   - v1_freq_hs <semitones>    (relative pitch in half-steps)
#   - filter_cutoff <0..0x7FF>
#   - filter_mode, fr_vic (packed res/routing)
#
# ====================================================================================

from pathlib import Path
import argparse, re

# -------------------------------
# SWI instrument-local byte map (SID-Wizard v1.7 layout)
# -------------------------------
WFTABLEPOS = 0x10     # waveform/arp table base (inside the instrument payload)
AD = 0x03             # ADSR: attack/decay pair
SR = 0x04             # ADSR: sustain/release pair
ARPS = 0x07           # WF/ARP step timing byte (low 6 bits), SID-Wizard tempo domain
PWPT = 0x0A           # pointer (byte offset within payload) to PW table
FLPT = 0x0B           # pointer (byte offset within payload) to Filter table
WF0  = 0x0F           # initial control register value for first frame (SID ctrl byte)

# ====================================================================================
# I/O helpers
# ====================================================================================

def read_payload(p: Path) -> bytes:
    """
    Read the .swi file and strip a 2-byte PRG load address if present.
    Many .swi files are stored as PRG with a plausible load address.
    """
    b = p.read_bytes()
    if len(b) < 0x20:
        raise SystemExit("bad .swi (too small)")
    load_addr = b[0] | (b[1] << 8)
    # If there is a valid C64-ish load address and enough bytes, drop it.
    return b[2:] if 0x0300 <= load_addr <= 0xC000 and len(b) >= 34 else b

def rows(buf: bytes, off: int):
    """
    Read triplets starting at 'off' until 0xFF (terminator) is seen as the first byte.
    Each logical row is 3 bytes: (left, right, third). 0xFF ends the table.
    """
    out = []
    i = off
    while i + 2 < len(buf):
        if buf[i] == 0xFF:
            break
        out.append((buf[i], buf[i+1], buf[i+2]))
        i += 3
    return out

def cut_at_ff(buf: bytes, off: int, maxlen=512):
    """
    Return raw bytes from 'off' up to and including the first 0xFF (or maxlen).
    Useful for diagnostic comments at the end of the .conf.
    """
    o = bytearray()
    i = off
    while i < len(buf) and len(o) < maxlen:
        b = buf[i]
        o.append(b)
        if b == 0xFF:
            break
        i += 1 if b == 0xFF else 3
    return bytes(o)

def hexb(bs: bytes) -> str:
    """Format a bytes object as space-separated hex string."""
    return " ".join(f"{x:02X}" for x in bs)

# ====================================================================================
# Small utility helpers
# ====================================================================================

def sanitize(ctrl: int) -> int:
    """
    Clear TEST bit (0x08) only. DO NOT force a waveform if none are set.
    For fidelity we leave waveform/sync/ring/gate as authored in SWI.
    """
    return ctrl & ~0x08

def band_to_mode(n: int) -> int:
    """
    Map SW band nibble (1,2,3) to reMID filter_mode bitmask:
      1 -> low-pass, 2 -> band-pass, 3 -> high-pass
    Defaults to LP if outside range.
    """
    return {1:0x1, 2:0x2, 3:0x4}.get(n & 0xF, 0x1)

def fe_jump_index(lo: int, hi: int, table_base: int, rows_count: int):
    """
    Resolve FE ptr (lo,hi) -> row index within a table:
      ptr = (hi<<8)|lo; row_index = (ptr - table_base) // 3
    Returns row index or None if out of range/misaligned.
    """
    ptr = ((hi & 0xFF) << 8) | (lo & 0xFF)
    delta = ptr - table_base
    if delta % 3 != 0:
        return None
    idx = delta // 3
    return idx if 0 <= idx < rows_count else None

# ====================================================================================
# Pulse Width (PW) materializer
# ====================================================================================

def mat_pw(payload: bytes, pwrows: list, frames: int, pw_base: int) -> list[int]:
    """
    Build a per-frame list of 12-bit PW values (0x001..0xFFF) from the PW table.
    Behavior mirrors SID-Wizard tables:

    Row encodings:
      - Absolute set: (left & 0x80) != 0
            PW := ((left & 0x0F) << 8) | right
            Consumes 1 frame, then holds until a later row changes it.
      - Sweep: (left & 0x80) == 0 AND left not in {FE, FF}
            duration = left frames
            slope    = int8(right) per frame (coarse SID units → we apply directly)
      - FE: jump to an absolute row address (pointer stored in right/third)
      - FF: terminate/hold

    Loop safety:
      - If FE jumps to itself or cycles without consuming frames, we advance and
        eventually hold (watchdogs prevent infinite loops).

    Also:
      - If the table only contains a single absolute set and no sweep rows,
        we just HOLD that value for all frames.
      - If PW cannot be inferred, default to header guess (bytes 5/6) or 0x800.
    """
    # Header guess for initial PW (many SWI variants store PW lo@5, hi@6)
    hdr_guess = ((payload[6] << 8) | payload[5]) & 0x0FFF

    # Find first absolute PW set (preferred initial value)
    init_pw = None
    for (l, r, _t) in pwrows:
        if (l & 0x80) and l not in (0xFE, 0xFF):
            init_pw = ((l & 0x0F) << 8) | (r & 0xFF)
            break
    if init_pw is None:
        init_pw = hdr_guess if hdr_guess else 0x800
    if init_pw <= 0:
        init_pw = 1

    out = [init_pw] * max(1, frames)
    if not pwrows:
        return out

    # Quick exits for "no time-consuming rows"
    has_any_time = any(l not in (0xFE, 0xFF) for (l, _, __) in pwrows)
    if not has_any_time:
        return out
    has_sweep = any((l not in (0xFE, 0xFF)) and ((l & 0x80) == 0) for (l, _, __) in pwrows)
    abs_sets = sum(1 for (l, _, __) in pwrows if (l & 0x80) and l not in (0xFE, 0xFF))
    if not has_sweep and abs_sets <= 1:
        # Only one absolute row → hold it forever
        return out

    # March the table across 'frames', honoring FE/FF, with loop watchdogs
    i = 0               # row index
    f = 0               # frame index
    cur = init_pw
    spin = 0
    max_spin = len(pwrows) + 4
    last_f = -1
    no_progress = 0
    max_no_progress = len(pwrows) * 4 + 16

    while f < frames:
        # General watchdog: if frames don't advance for a while, hold and exit.
        if f == last_f:
            no_progress += 1
            if no_progress > max_no_progress:
                for k in range(f, frames):
                    out[k] = cur
                break
        else:
            no_progress = 0
            last_f = f

        # Wrap row index and protect against "spin"
        if i >= len(pwrows):
            i = 0
            spin += 1
            if spin > max_spin:
                for k in range(f, frames):
                    out[k] = cur
                break
            continue

        l, r, t = pwrows[i]

        # End: hold remaining frames
        if l == 0xFF:
            for k in range(f, frames):
                out[k] = cur
            break

        # FE: jump to pointer (right/third), with loop safety
        if l == 0xFE:
            j = fe_jump_index(r, t, pw_base, len(pwrows))
            if j is None or j == i:
                # Bad/degenerate jump → try next row, and eventually hold
                spin += 1
                if spin > max_spin:
                    for k in range(f, frames):
                        out[k] = cur
                    break
                i = (i + 1) % len(pwrows)
            else:
                i = j
                spin = 0
            continue

        # Time-consuming rows reset 'spin'
        spin = 0

        if (l & 0x80):
            # Absolute PW set (consumes 1 frame)
            cur = ((l & 0x0F) << 8) | (r & 0xFF)
            if cur <= 0:
                cur = 1
            out[f] = cur
            f += 1
            i += 1
        else:
            # Sweep: duration=l frames, slope=int8(r) per frame
            slope = r if r < 0x80 else r - 0x100
            dur = max(1, l)
            for _ in range(dur):
                if f >= frames:
                    break
                cur = max(1, min(0xFFF, cur + slope))
                out[f] = cur
                f += 1
            i += 1

    return out

# ====================================================================================
# Filter materializer
# ====================================================================================

def mat_filter(flrows: list, frames: int, fl_base: int):
    """
    Build per-frame filter cutoff values and return (cutoff_list, mode_bits, fr_vic).

    Row encodings:
      - CONTROL row: (left & 0x80) OR (third & 0x80)
          band  := (left>>4)&7  → filter_mode (LP/BP/HP)
          res   := left&0x0F
          route := third&0x07   (voice routing bits); default to v1 if 0
          fine  := (third>>4)&7 (fine cutoff addition)
          cutoff:= (right<<3) | fine
      - ABSOLUTE cutoff: left == 0
          cutoff := right<<3
      - SWEEP: otherwise
          duration = left
          slope    = int8(right) * 8  (scale to 11-bit domain)

      - FE: pointer jump; FF: end/hold.

    Loop safety like mat_pw(). We clamp cutoff to 0x000..0x7FF.
    """
    # Reasonable defaults if table is empty
    cutoff = 0x0600
    mode   = 0x1    # LP
    fr_vic = 0xF1   # packed: resonance + routing
    out = [cutoff] * max(1, frames)

    if not flrows:
        return out, mode, fr_vic

    if not any(l not in (0xFE, 0xFF) for (l, _, __) in flrows):
        return out, mode, fr_vic

    i = 0
    f = 0
    cur = cutoff
    spin = 0
    max_spin = len(flrows) + 4
    last_f = -1
    no_progress = 0
    max_no_progress = len(flrows) * 4 + 16

    while f < frames:
        # Watchdog for "no progress" scenarios
        if f == last_f:
            no_progress += 1
            if no_progress > max_no_progress:
                for k in range(f, frames):
                    out[k] = cur
                break
        else:
            no_progress = 0
            last_f = f

        if i >= len(flrows):
            i = 0
            spin += 1
            if spin > max_spin:
                for k in range(f, frames):
                    out[k] = cur
                break
            continue

        l, r, t = flrows[i]

        if l == 0xFF:
            for k in range(f, frames):
                out[k] = cur
            break

        if l == 0xFE:
            j = fe_jump_index(r, t, fl_base, len(flrows))
            if j is None or j == i:
                spin += 1
                if spin > max_spin:
                    for k in range(f, frames):
                        out[k] = cur
                    break
                i = (i + 1) % len(flrows)
            else:
                i = j
                spin = 0
            continue

        spin = 0  # time is going to advance

        # "Hybrid" control detect: some SW encodings use third's high bit as well.
        is_control = (l & 0x80) or (t & 0x80)

        if is_control:
            band  = (l >> 4) & 0x07
            res   =  l       & 0x0F
            route = (t & 0x07) or 0x1
            mode  = band_to_mode(band)
            fr_vic= ((res & 0xF) << 4) | (route & 0x7)
            fine  = (t >> 4) & 0x07
            cur   = min(0x7FF, ((r & 0xFF) << 3) | fine)
            out[f] = cur
            f += 1
            i += 1

        elif l == 0x00:
            # Absolute cutoff set
            cur = min(0x7FF, (r & 0xFF) << 3)
            out[f] = cur
            f += 1
            i += 1

        else:
            # Sweep: duration=l, slope=int8(r)*8
            dur   = max(1, l)
            slope = (r if r < 0x80 else r - 0x100) * 8
            for _ in range(dur):
                if f >= frames:
                    break
                cur = max(0x000, min(0x7FF, cur + slope))
                out[f] = cur
                f += 1
            i += 1

    return out, mode, fr_vic

# ====================================================================================
# ARP decoder
# ====================================================================================

def arp_byte_to_offset(a: int) -> tuple[bool, int]:
    """
    Decode a single ARP byte as per SID-Wizard player:

      0x00..0x7E  => RELATIVE UP by +a semitones            → (True,  +a)
      0x80        => NOP (no pitch change)                  → (True,  0)
      0x81..0xDF  => ABSOLUTE note index (not representable
                     with reMID relative ops)                → (False, 0)
      0xE0..0xFF  => RELATIVE DOWN (two's-complement)       → (True,  a-256)
      0x7F        => CHORD CALL (requires external tables)  → (False, 0)

    Return:
      (is_valid_offset, semitone_offset)
      If !is_valid_offset, the caller should keep the previous offset (hold).
    """
    if a == 0x80:
        return True, 0          # NOP
    if a == 0x7F:
        return False, 0         # chord call → can't expand without chord tables
    if a < 0x80:                # relative up
        return True, a
    if a >= 0xE0:               # relative down (−32..−1)
        return True, a - 256
    # 0x81..0xDF → ABSOLUTE note → not representable with reMID instrument ops
    return False, 0

def materialize_arp_offsets(wf_triplets: list[tuple[int,int,int]], step_frames: int):
    """
    Read the WF/ARP table's ARP column and turn it into per-frame absolute
    semitone offsets (relative to the played note).

    Returns:
      arp_abs          : List[int] length == (#rows * step_frames)
      has_loop         : bool (WF table contains an FE jump)
      loop_start_frame : int  (frame index to loop back to, if has_loop)

    Notes:
    - We read the raw triplets so we can locate FE in the true source table.
    - For ABS and CHORD entries we *hold* the previous offset (safest fallback).
    """
    has_loop = False
    loop_row = None

    # Gather linear steps first (until FE/FF)
    steps = []
    for (w, a, x) in wf_triplets:
        if w == 0xFF:
            break
        if w == 0xFE:
            j = fe_jump_index(a, x, WFTABLEPOS, len(wf_triplets))
            loop_row = j if j is not None else 0
            has_loop = True
            break
        steps.append((w, a, x))

    # If the table is empty, just return zeros for one step worth of frames
    if not steps:
        return [0] * step_frames, False, 0

    # Decode per-row absolute offsets relative to base note
    arp_per_row = []
    for (_w, a, _x) in steps:
        ok, off = arp_byte_to_offset(a)
        if not ok:
            # For ABS/chord calls we can't compute a relative delta in reMID,
            # so we hold previous absolute offset (0 if first row).
            off = arp_per_row[-1] if arp_per_row else 0
        arp_per_row.append(off)

    # Compute the frame index where the loop re-enters
    loop_start_frame = (loop_row or 0) * step_frames if has_loop else 0

    # Expand row-granularity offsets to per-frame offsets
    arp_abs = []
    for off in arp_per_row:
        arp_abs += [off] * step_frames

    return arp_abs, has_loop, loop_start_frame

# ====================================================================================
# Emitter: compose a .conf instrument from decoded tracks
# ====================================================================================

def emit(name: str, payload: bytes, *,
         program_speed=50, speed_mult=1, arp_plus1=False,
         strict_wf=False, emit_arp=True, hard_restart=False,
         sustain_frames=64):
    """
    Convert one .swi instrument payload to a reMID .conf string.

    Key details:
      - program_speed: global tick rate (50 ≈ PAL frame rate).
      - step_frames  : how many frames each WF/ARP row remains active.
                       Computed from the SWI ARP speed byte; optional +1.
      - strict_wf    : if True, use WF values exactly as in SWI; otherwise clear TEST.
      - emit_arp     : if False, ignore ARP offsets entirely.
      - hard_restart : if True, emit a TEST+GATE jab (0x09) at start (defaults OFF).
      - sustain_frames: how long to continue evolving PW/Filter in one-shot patches.
    """
    # Sanitize/normalize instrument name for the block header
    name = re.sub(r'[^A-Za-z0-9_-]+', '-', name.strip()) or "instrument"

    # Header parameters
    ad = payload[AD]
    sr = payload[SR]
    wf0 = payload[WF0]
    arp_byte = payload[ARPS] & 0x3F

    # Each WF/ARP row spans this many frames in our output
    step_frames = max(1, arp_byte + (1 if arp_plus1 else 0))

    # Parse source tables
    wfrows = rows(payload, WFTABLEPOS)
    pwrows = rows(payload, payload[PWPT])
    flrows = rows(payload, payload[FLPT])

    # --------------------------------
    # WF sequence + where the WF table loops (row index)
    # --------------------------------
    wf_steps = []         # [(control_byte, arp_byte, third)]
    wf_loop_row = None    # row index target if FE appears
    for (w, a, x) in wfrows:
        if w == 0xFF:
            break
        if w == 0xFE:
            # FE pointer: find the destination row
            j = fe_jump_index(a, x, WFTABLEPOS, len(wfrows))
            wf_loop_row = j if j is not None else 0
            break
        # 'strict_wf' keeps the byte verbatim; otherwise we only clear TEST bit
        wf_steps.append((w if strict_wf else sanitize(w), a, x))

    if not wf_steps:
        # Fallback: single frame with the initial control byte
        wf_steps = [((wf0 if strict_wf else sanitize(wf0)), 0x00, 0x00)]

    wf_has_loop = wf_loop_row is not None
    loop_start_frame = (wf_loop_row or 0) * step_frames

    # Total frames for the "attack" (one pass through WF rows)
    total_frames = step_frames * len(wf_steps)

    # Expand control values to per-frame list
    wf_abs = [wf_steps[i // step_frames][0] for i in range(total_frames)]

    # --------------------------------
    # ARP: materialize absolute semitone offsets per frame
    # --------------------------------
    if emit_arp:
        # Use RAW WF table (wfrows) so FE is interpreted correctly
        arp_abs, _unused_has_loop, _unused_loop_start = materialize_arp_offsets(wfrows, step_frames)
        # Trim/extend to match total_frames precisely
        if len(arp_abs) < total_frames:
            arp_abs += [arp_abs[-1] if arp_abs else 0] * (total_frames - len(arp_abs))
        else:
            arp_abs = arp_abs[:total_frames]
    else:
        arp_abs = [0] * total_frames

    # --------------------------------
    # Materialize PW and Filter tracks out to the "horizon"
    # --------------------------------
    horizon = total_frames + max(1, sustain_frames)  # allow sustain evolving
    pw_all  = mat_pw(payload, pwrows, horizon, payload[PWPT])
    fl_all, mode, fr_vic = mat_filter(flrows, horizon, payload[FLPT])

    # Optimization: in many patches PW doesn't move at all in the attack
    pw_static_attack = all(pw_all[i] == pw_all[0] for i in range(1, total_frames))

    # =================================================================================
    # Emit .conf lines
    # =================================================================================

    lines = []
    # Minimal header to make the instrument playable standalone
    lines += [
        "# generated by swi2remid (FE-pointer aware; ARP decoded; loop-safe; no drift)",
        "",
        "[channels]",
        "1=1",
        "",
        "[programs]",
        "format=0.0",
        f"1={name}",
        "",
        f"[{name}]",
        f"program_speed={program_speed * max(1, speed_mult)}",
        f"v1_ad=0x{ad:02X}",
        f"v1_sr=0x{sr:02X}",
        f"filter_mode=0x{mode:X}",
        f"fr_vic=0x{fr_vic:02X}",
        f"filter_cutoff=0x{fl_all[0]:04X}",
        f"v1_pulse=0x{max(1, pw_all[0]):03X}",
        ""
    ]

    t = 0  # line index within preset script

    # Optional "hard restart" (TEST+GATE jab). Default OFF for safety.
    if hard_restart:
        lines.append(f".{t}=v1_control 0x09"); t += 1
        lines.append(f".{t}=wait 1");          t += 1

    # We'll remember where each frame starts (line number) to jump precisely.
    frame_line = []
    # We'll also remember the line number right AFTER the initial seed.
    loop_entry_line_after_seed = None

    # ------------- Attack pass (one run across WF table) -------------
    for f in range(0, total_frames):
        frame_line.append(t)

        if f == 0:
            # First frame: set control byte (waveform + gate/sync/ring)
            lines.append(f".{t}=v1_control 0x{wf_abs[0]:02X}"); t += 1

            # ARP "seed" for frame 0 (absolute offset relative to played note)
            if emit_arp and arp_abs[0] != 0:
                lines.append(f".{t}=v1_freq_hs {arp_abs[0]}"); t += 1

            # IMPORTANT: Loop should re-enter AFTER we apply the seed
            loop_entry_line_after_seed = t

            # Initialize PW and filter at their first values
            lines.append(f".{t}=v1_pulse 0x{max(1, pw_all[0]):03X}"); t += 1
            lines.append(f".{t}=filter_cutoff 0x{fl_all[0]:04X}");   t += 1

        else:
            # Control changes only when the control byte actually changes
            if wf_abs[f] != wf_abs[f-1]:
                lines.append(f".{t}=v1_control 0x{wf_abs[f]:02X}"); t += 1

            # Avoid "PW chatter" if attack PW is static
            if (not pw_static_attack) and pw_all[f] != pw_all[f-1]:
                lines.append(f".{t}=v1_pulse 0x{pw_all[f]:03X}");   t += 1

            # Filter cutoff changes
            if fl_all[f] != fl_all[f-1]:
                lines.append(f".{t}=filter_cutoff 0x{fl_all[f]:04X}"); t += 1

            # ARP delta for this frame (relative change only)
            if emit_arp:
                d = arp_abs[f] - arp_abs[f-1]
                if d != 0:
                    lines.append(f".{t}=v1_freq_hs {d}"); t += 1

        # Each WF/ARP row frame consumes 1 tick
        lines.append(f".{t}=wait 1"); t += 1

    # ------------- Loop or Sustain -------------
    if wf_has_loop:
        # Ensure the absolute ARP offset at loop-start equals the target.
        if emit_arp:
            target = arp_abs[loop_start_frame] if loop_start_frame < len(arp_abs) else 0
            cur    = arp_abs[total_frames - 1]
            wrap   = target - cur
            if wrap != 0:
                lines.append(f".{t}=v1_freq_hs {wrap}"); t += 1

        # If FE jumps to row 0, re-enter AFTER the seed; else jump to that row's first frame.
        if loop_start_frame == 0 and loop_entry_line_after_seed is not None:
            loop_line = loop_entry_line_after_seed
        else:
            loop_line = (frame_line[loop_start_frame]
                         if loop_start_frame < len(frame_line)
                         else (frame_line[0] if frame_line else 0))

        lines.append(f".{t}=goto {loop_line}"); t += 1

    else:
        # One-shot: keep last waveform and evolve PW/Filter during sustain horizon.
        last_wf = wf_abs[-1] if wf_abs else sanitize(payload[WF0])

        # Return pitch to base if we had a non-zero offset at the end.
        if emit_arp and arp_abs[-1] != 0:
            lines.append(f".{t}=v1_freq_hs {-arp_abs[-1]}"); t += 1

        lines.append(f".{t}=v1_control 0x{last_wf:02X}"); t += 1

        sustain_start = t
        # Initialize sustain with the first values after the attack
        lines.append(f".{t}=v1_pulse 0x{max(1, pw_all[total_frames]):03X}"); t += 1
        lines.append(f".{t}=filter_cutoff 0x{fl_all[total_frames]:04X}");   t += 1
        lines.append(f".{t}=wait 1"); t += 1

        # Continue evolving PW/Filter through the sustain horizon
        for f in range(total_frames + 1, horizon):
            if pw_all[f] != pw_all[f-1]:
                lines.append(f".{t}=v1_pulse 0x{pw_all[f]:03X}");   t += 1
            if fl_all[f] != fl_all[f-1]:
                lines.append(f".{t}=filter_cutoff 0x{fl_all[f]:04X}"); t += 1
            lines.append(f".{t}=wait 1"); t += 1

        # Loop the sustain region
        lines.append(f".{t}=goto {sustain_start}"); t += 1

    # Diagnostics footer: raw decoded rows for easier debugging
    lines += [
        "",
        "# Raw WF rows: " + " | ".join(f"{w:02X},{a:02X},{x:02X}" for (w, a, x) in wfrows),
        f"# Raw PW bytes (@0x{payload[PWPT]:02X}): {hexb(cut_at_ff(payload, payload[PWPT]))}",
        f"# Raw FL bytes (@0x{payload[FLPT]:02X}): {hexb(cut_at_ff(payload, payload[FLPT]))}"
    ]

    return "\n".join(lines)

# ====================================================================================
# CLI
# ====================================================================================

def main():
    ap = argparse.ArgumentParser(description="Convert SID-Wizard .swi instrument to reMID .conf")
    ap.add_argument("--in",  dest="inp",  required=True, help="input .swi file")
    ap.add_argument("--out", dest="outp", required=True, help="output .conf file")
    ap.add_argument("--name", default=None, help="override instrument name")

    # Timing knobs
    ap.add_argument("--program-speed", type=int, default=50, help="global tick rate (50 ≈ PAL)")
    ap.add_argument("--speed-mult",    type=int, default=1,  help="multiply program_speed (conservative = 1)")
    ap.add_argument("--arp-plus1",     action="store_true",  help="add +1 to ARP step_frames (rarely needed)")

    # Behavior toggles
    ap.add_argument("--strict-wf",     action="store_true",  help="use WF bytes verbatim (don't clear TEST)")
    ap.add_argument("--no-emit-arp",   action="store_true",  help="ignore ARP entirely (force steady pitch)")
    ap.add_argument("--hard-restart",  action="store_true",  help="emit TEST+GATE jab at start (default OFF)")
    ap.add_argument("--sustain-frames", type=int, default=64, help="frames to evolve PW/Filter in one-shot patches")

    args = ap.parse_args()

    payload = read_payload(Path(args.inp))

    # Instrument display/name: try last 8 chars of payload, else file stem
    nm = payload[-8:].decode("ascii", "ignore").strip() or Path(args.inp).stem
    name = args.name or nm

    txt = emit(
        name, payload,
        program_speed=args.program_speed,
        speed_mult=args.speed_mult,
        arp_plus1=args.arp_plus1,
        strict_wf=args.strict_wf,
        emit_arp=not args.no_emit_arp,
        hard_restart=args.hard_restart,
        sustain_frames=max(1, args.sustain_frames),
    )

    Path(args.outp).write_text(txt, encoding="utf-8")
    print(f"Wrote {args.outp}")

if __name__ == "__main__":
    main()
