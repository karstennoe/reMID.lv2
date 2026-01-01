# `sidwizard_runtime`

Portable SID-Wizard-style instrument runtime for `.swi` instruments.

This module is used by `reMID.lv2` to run SID-Wizard instrument tables directly at runtime (no reMID `.conf` “instrument language”).

## What it is

- A small C (C99) runtime that:
  - unpacks a SID-Wizard `.swi` file to a 128-byte instrument image
  - advances the instrument state one “frame” at a time (typically 50Hz PAL)
  - outputs the SID register values you should write to reSID (or real SID)

It implements the three instrument tables as state machines, mirroring the logic in `SID-Wizard-1.7/sources/include/player.asm`:

- WF/ARP table at `0x10` (3-byte rows, supports `FE` jumps and `FF` terminator)
- PW table at `instrument[0x0A]` (supports absolute set, sweeps, `FE`, `FF`)
- Filter table at `instrument[0x0B]` (control/abs/sweep rows, `FE`, `FF`)

## What it is not (yet)

- Not a full SID-Wizard song player (no patterns, no `.swm` chords, no multi-voice song state)
- Chord-call (`0x7F`) in the ARP column is not supported (needs song chord tables)

## Minimal demo tool

`sidwizard_runtime/sw_dump.c` is a tiny CLI that dumps the first N frames of a `.swi` instrument.

Build example (Linux/macOS):

```sh
cc -O2 -std=c99 -Isidwizard_runtime sidwizard_runtime/sw_dump.c sidwizard_runtime/sw_runtime.c -lm -o sw_dump
```

Run:

```sh
./sw_dump path/to/instrument.swi --note 60 --frames 128
```

## SWI file packing note

Many `.swi` instrument files are stored in a packed form where the final `0xFF` table terminator is replaced by a size byte and followed by an 8-byte instrument name.

For correct playback/conversion, restore that `0xFF` by using `sw_swi_unpack_128()`.

