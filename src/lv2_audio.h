#ifndef MY_LV2_AUDIO_H
#define MY_LV2_AUDIO_H

#include<lv2.h>
#include "midi.h"
#include "sid_chips.h" //this must appear out of order due to a circular dependency around the above typedef
#include "sw_bank.h"

//I'd really rather not put types in headers but it really simplifies the plugin version
struct super
{
    struct CHIPS* sid_bank;
    struct midi_arrays* midi;
    sw_bank_t* bank;

    struct midi_arrays* newmidi;
    sw_bank_t* new_bank;
    struct midi_arrays* oldmidi;
    sw_bank_t* old_bank;

    float* outl; //lv2 ports
	float* outr;

	// Optional per-channel program overrides (1..128). 0 means "no override".
	const float* chan_program_override[16];

	// LV2 bundle base path (always ends with '/'), used to resolve relative .swibank paths.
	char bundle_path[512];
};

void* init_lv2_audio(uint32_t fs, char* instr_file, const LV2_Feature * const* host_features);
int process(uint32_t nsamples, void* arg);
void cleanup_audio(void* arg);

#endif
