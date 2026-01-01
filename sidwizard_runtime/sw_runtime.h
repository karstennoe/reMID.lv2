// Portable SID-Wizard-style instrument runtime for .swi instruments.
//
// This runtime mirrors the behavior in SID-Wizard 1.7's player (player.asm) for
// the instrument tables (WF/ARP, PW, Filter). It is intentionally instrument-only:
// it does not implement the full song/pattern/chord engine.
//
// It is designed to be reused in embedded projects (e.g. RP2040) by:
// - avoiding dynamic allocation
// - avoiding platform APIs
// - using the SID-Wizard player frequency tables (no floating point required)
//
// The runtime is intentionally "instrument-only": it does not implement a full
// SID-Wizard song player, pattern engine, or chord tables.

#ifndef SW_RUNTIME_H
#define SW_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------
// .swi layout (SW 1.7-ish)
// -----------------------

enum {
  SW_MAX_INSTSIZE = 128,
  SW_INST_NAME_LEN = 8,

  SW_WFTABLEPOS = 0x10, // WF/ARP table base (within payload, after optional PRG addr)
  SW_AD         = 0x03, // ADSR Attack/Decay byte
  SW_SR         = 0x04, // ADSR Sustain/Release byte
  SW_VIB        = 0x05, // vibrato amplitude/rate byte
  SW_VIB2       = 0x06, // vibrato delay/increment byte
  SW_ARPS       = 0x07, // ARP/WF step timing (low 6 bits)
  SW_PWPT       = 0x0A, // PW table pointer (byte offset)
  SW_FLPT       = 0x0B, // Filter table pointer (byte offset)
  SW_WF0        = 0x0F, // Initial control byte for first frame

  // Optional bytes observed in some SWI exports.
  SW_GO_WF      = 0x0C, // gate-off when WF table reaches row N
  SW_GO_PW      = 0x0D, // gate-off when PW table reaches row N
  SW_GO_FL      = 0x0E  // gate-off when FL table reaches row N
};

typedef struct sw_runtime_config {
  // If true, clear TEST bit (0x08) from control writes.
  // For bit-exact SID-Wizard behavior, leave this false.
  bool clear_test_bit;

  // Default filter routing bits for $d417 low nibble (1/2/4 or combinations).
  // Some instruments may override routing via filter table 3rd column.
  uint8_t default_filter_route;

  // Volume nibble used when composing MODE/VOL (0..15).
  uint8_t volume;
} sw_runtime_config_t;

typedef struct sw_sid_frame {
  // Voice 1 registers (SID offsets: 0x00..0x06)
  uint16_t freq_reg;   // 0x00/0x01
  uint16_t pulse_reg;  // 0x02/0x03 (12-bit used)
  uint8_t  control;    // 0x04
  uint8_t  ad;         // 0x05
  uint8_t  sr;         // 0x06

  // Filter/global registers (SID offsets: 0x15..0x18)
  uint16_t filter_cutoff; // 11-bit (0..0x7FF)
  uint8_t  fr_vic;        // resonance + routing (SID 0x17)
  uint8_t  mode_vol;      // (filter_mode<<4)|(vol&0xF) (SID 0x18)
} sw_sid_frame_t;

typedef struct sw_runtime {
  const uint8_t* payload;
  size_t payload_len;
  sw_runtime_config_t cfg;

  // Derived from header
  uint8_t ins_ctrl;
  uint8_t ad;
  uint8_t sr;
  uint8_t wf0;
  uint8_t pw_base;
  uint8_t fl_base;
  uint8_t arp_speed;
  int8_t octave_shift; // payload[0x09] (signed), added to base pitch

  // Table row counts (best-effort scan; used for bounds checks only)
  uint16_t wf_rows;
  uint16_t pw_rows;
  uint16_t fl_rows;

  // Runtime state
  bool active;
  uint8_t ptn_gate;     // $FF normal, $FE gate-bit off mask (mirrors PTNGATE behavior)
  uint8_t midi_note;    // 0..127
  uint8_t dpitch_base;  // SID-Wizard discrete pitch (0..95), from MIDI note + octave shift
  uint8_t detuner;      // detune byte (added to freq low at write time)

  // WF/ARP engine (mirrors player.asm WFARPTB behavior)
  int16_t arps_cnt;     // signed counter
  uint8_t wft_pos;      // byte offset within payload
  uint8_t wfghost;      // last control byte written (WFGHOST)

  // Pitch state (mirrors FREQLO/FREQHI in player; used so vibrato can run between WF updates).
  uint8_t freql;
  uint8_t freqh;

  // Vibrato (mirrors player.asm VIBSLIDE/SETVIBR/SETFMOD; per-instrument bytes at offsets 0x05/0x06).
  uint8_t slidevib;   // ins_ctrl & $30 (0x00 increasing, 0x10/0x20/0x30 normal variants)
  uint8_t videlcnt;   // vibrato delay/increment counter (byte, counts down to $FF)
  uint8_t vibfrequ;   // VIBFREQU (rate*2)
  uint8_t vibracnt;   // VIBRACNT
  uint8_t freqmodl;   // FREQMODL
  uint8_t freqmodh;   // FREQMODH

  // PW engine (mirrors SETPWID behavior)
  uint8_t pwt_pos;        // byte offset within payload
  uint8_t pw_sweep_cnt;   // PWEEPCNT
  uint8_t pw_hi;          // PWHIGHO (low nibble used)
  uint8_t pw_lo;

  // Filter engine (mirrors FilterProgram behavior for one SID)
  uint8_t flt_pos;        // byte offset within payload
  uint8_t fl_sweep_cnt;   // CWEPCNT
  uint16_t cutoff_11;     // 11-bit cutoff (0..2047)
  uint8_t filter_band;    // bits 4..6 for $d418
  uint8_t resonance_hi;   // upper nibble (RESONIB)
  uint8_t filter_route;   // low nibble for $d417
} sw_runtime_t;

typedef struct sw_swi_info {
  uint16_t prg_load_addr; // 0 if none detected
  uint8_t packed_size;    // 0 if not packed or unknown
  uint8_t name[SW_INST_NAME_LEN];
} sw_swi_info_t;

// Strip a 2-byte PRG load address if it looks like a C64-ish address.
// Returns the new offset (0 or 2) into the input buffer.
//
// Note: many .swi files are "packed instruments" where the last table terminator
// (0xFF) is replaced with a size byte. For correct playback, prefer
// sw_swi_unpack_128().
size_t sw_swi_payload_offset(const uint8_t* file_bytes, size_t file_len);

// Unpack a SID-Wizard .swi instrument into a fixed 128-byte in-memory image.
//
// Many .swi instrument files are stored as:
//   [optional 2-byte PRG load address]
//   [instrument bytes 0..size-1]
//   [size byte == index of replaced 0xFF terminator]
//   [8-byte instrument name]
//
// This function restores the last 0xFF terminator and writes the name to the
// last 8 bytes of out_inst.
bool sw_swi_unpack_128(const uint8_t* file_bytes, size_t file_len, uint8_t out_inst[SW_MAX_INSTSIZE], sw_swi_info_t* out_info);

// Initialize runtime with a .swi payload (already stripped of PRG load addr if needed).
// Returns false if payload is too small or malformed.
bool sw_runtime_init(sw_runtime_t* rt, const uint8_t* payload, size_t payload_len, const sw_runtime_config_t* cfg);

// Start playing a note (monophonic instance).
void sw_runtime_note_on(sw_runtime_t* rt, uint8_t midi_note, uint8_t velocity);

// Release note (clears gate on subsequent frames).
void sw_runtime_note_off(sw_runtime_t* rt);

// Advance by one "frame" (typically 1/50s PAL) and output the resulting SID regs.
// Returns false if rt is inactive or not initialized.
bool sw_runtime_tick(sw_runtime_t* rt, sw_sid_frame_t* out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SW_RUNTIME_H
