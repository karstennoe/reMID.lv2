#ifndef SW_BANK_H
#define SW_BANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bank/mapping layer for selecting SID-Wizard .swi instruments via MIDI Program Change
// (and optional drumkit note->instrument mapping).
//
// This is intentionally *not* the reMID .conf "instrument language"; it only selects
// which .swi instrument image to run in sidwizard_runtime.

typedef struct sw_bank sw_bank_t;

// Load a .swibank file from disk (parses program mapping and preloads referenced .swi files).
// Returns NULL on error.
sw_bank_t* sw_bank_load(const char* bank_path);

void sw_bank_free(sw_bank_t* bank);

// Copy the selected instrument (128-byte in-memory image) into out_inst.
// - program: 0..127 (from MIDI Program Change)
// - note: 0..127 (used only when the program is a drumkit)
// Returns false if unmapped.
bool sw_bank_get_instrument(const sw_bank_t* bank, uint8_t program, uint8_t note, uint8_t out_inst[128]);

// Human-readable bank name (may be NULL).
const char* sw_bank_name(const sw_bank_t* bank);

// Returns a short human-readable description of what `program` maps to, e.g.
// an instrument name or "DRUMKIT:<name>".
// Returns false if the slot is empty or invalid.
bool sw_bank_describe_program(const sw_bank_t* bank, uint8_t program, char* out, size_t out_len);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
