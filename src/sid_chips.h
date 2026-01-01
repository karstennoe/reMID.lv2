#ifndef SID_CHIPS_H
#define SID_CHIPS_H
#include "midi.h"
#include "sw_bank.h"
#include "../sidwizard_runtime/sw_runtime.h"

struct SID;

struct CHIPS
{
    struct SID** sid_chips;

    int polyphony;
    int use_sid_volume;
    int pt_debug;
    int chiptype;
    int8_t *active;
    uint32_t rtime;

    short *buf;
    int buf_length;

    int32_t *prevx; //dc blocking filter
    int32_t *prevy;
    int32_t *err;

    double clock_freq;
    double freq_mult;
    double sample_freq;
    double clocks_per_sample;

    // One SWI runtime voice per reSID instance (this project uses "one SID per MIDI voice" polyphony).
    sw_runtime_t **voices;
    uint8_t (*voice_insts)[SW_MAX_INSTSIZE]; // storage backing for each voice runtime
    uint32_t *voice_next_tick;               // microsecond scheduler for 50Hz ticks
    uint8_t *voice_velocity;                 // last MIDI velocity (0..127)
};


#ifdef  __cplusplus
extern "C" {
#endif

struct CHIPS *sid_init(int polyphony, int use_sid_volume, int chiptype, int debug);
void sid_close(struct CHIPS *chips);
void sid_set_srate(struct CHIPS *chips, int pal, double sample_freq);
void sid_process(struct CHIPS *chips, struct midi_arrays* midi, sw_bank_t* bank, int num_samples, float* outl, float* outr);

#ifdef  __cplusplus
}
#endif

#endif
