// reSID chip wrapper + SID-Wizard (.swi) instrument runtime integration.

#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>

#include "../lib/sid.h"
#include "midi.h"
#include "sid_chips.h"

extern "C" void sid_close(struct CHIPS* chips)
{
  if (!chips) return;

  if (chips->sid_chips)
  {
    for (int i = 0; chips->sid_chips[i]; i++) delete chips->sid_chips[i];
    free(chips->sid_chips);
  }

  free(chips->active);
  free(chips->prevx);
  free(chips->prevy);
  free(chips->err);

  if (chips->voices)
  {
    for (int i = 0; chips->voices[i]; ++i) free(chips->voices[i]);
    free(chips->voices);
  }
  free(chips->voice_insts);
  free(chips->voice_next_tick);
  free(chips->voice_velocity);

  free(chips->buf);
  free(chips);
}

extern "C" struct CHIPS* sid_init(int polyphony, int use_sid_volume, int chiptype, int debug)
{
  struct CHIPS* self = (struct CHIPS*)calloc(1, sizeof(struct CHIPS));
  if (!self) return NULL;

  self->sid_chips = (SID**)calloc((size_t)polyphony + 1u, sizeof(SID*));
  self->active = (int8_t*)calloc((size_t)polyphony, sizeof(int8_t));
  self->prevx = (int32_t*)calloc((size_t)polyphony, sizeof(int32_t));
  self->prevy = (int32_t*)calloc((size_t)polyphony, sizeof(int32_t));
  self->err = (int32_t*)calloc((size_t)polyphony, sizeof(int32_t));

  self->voices = (sw_runtime_t**)calloc((size_t)polyphony + 1u, sizeof(sw_runtime_t*));
  self->voice_insts = (uint8_t(*)[SW_MAX_INSTSIZE])calloc((size_t)polyphony, sizeof(*self->voice_insts));
  self->voice_next_tick = (uint32_t*)calloc((size_t)polyphony, sizeof(uint32_t));
  self->voice_velocity = (uint8_t*)calloc((size_t)polyphony, sizeof(uint8_t));

  if (!self->sid_chips || !self->active || !self->prevx || !self->prevy || !self->err || !self->voices ||
      !self->voice_insts || !self->voice_next_tick || !self->voice_velocity)
  {
    sid_close(self);
    return NULL;
  }

  for (int i = 0; i < polyphony; i++)
  {
    self->sid_chips[i] = new SID();

    self->chiptype = chiptype;
    if (chiptype == 6581)
      self->sid_chips[i]->set_chip_model(MOS6581);
    else
    {
      self->sid_chips[i]->set_chip_model(MOS8580);
      self->chiptype = 8580;
    }

    self->active[i] = 0;
    self->sid_chips[i]->reset();

    // Initialise SID volume to max if we're not doing volume at the SID level.
    if (!use_sid_volume) self->sid_chips[i]->write(0x18, 0x0f);
    self->use_sid_volume = use_sid_volume;

    self->voices[i] = (sw_runtime_t*)calloc(1, sizeof(sw_runtime_t));
    if (!self->voices[i])
    {
      sid_close(self);
      return NULL;
    }
    self->voice_next_tick[i] = 0;
    self->voice_velocity[i] = 0;
  }
  self->sid_chips[polyphony] = NULL;
  self->voices[polyphony] = NULL;

  printf("%i reSID chip polyphony system\n", polyphony);

  self->polyphony = polyphony;
  self->pt_debug = debug;
  self->rtime = 0;

  self->buf_length = (int)(sizeof(short) * 8192);
  self->buf = (short*)calloc(8192, sizeof(short));
  if (!self->buf)
  {
    sid_close(self);
    return NULL;
  }
  printf("%d bytes free in SID output buffer\n", self->buf_length);

  return self;
}

extern "C" void sid_set_srate(struct CHIPS* chips, int pal, double srate)
{
  if (!chips) return;
  chips->sample_freq = srate;

  chips->clock_freq = pal ? 985248 : 1022730;
  chips->freq_mult = chips->clock_freq / 16777216.0;
  printf("%s mode: clock frequency %.2f, frequency multiplier %f\n", (pal ? "PAL" : "NTSC"), chips->clock_freq, chips->freq_mult);

  chips->clocks_per_sample = chips->clock_freq / chips->sample_freq;

  for (int i = 0; chips->sid_chips[i]; i++)
  {
    chips->sid_chips[i]->set_sampling_parameters(chips->clock_freq, SAMPLE_FAST, chips->sample_freq);
  }
}

static void clear_key(midi_key_state_t** midi_keys, int key)
{
  midi_keys[key]->note_on = 0;
  midi_keys[key]->note_state_changed = 0;
}

static void write_frame(SID* sid, const sw_sid_frame_t* fr)
{
  sid->write(0x00, (uint8_t)(fr->freq_reg & 0xFFu));
  sid->write(0x01, (uint8_t)(fr->freq_reg >> 8));

  sid->write(0x02, (uint8_t)(fr->pulse_reg & 0xFFu));
  sid->write(0x03, (uint8_t)(fr->pulse_reg >> 8));

  sid->write(0x05, fr->ad);
  sid->write(0x06, fr->sr);
  sid->write(0x04, fr->control);

  sid->write(0x15, (uint8_t)(fr->filter_cutoff & 0x07u));
  sid->write(0x16, (uint8_t)(fr->filter_cutoff >> 3));
  sid->write(0x17, fr->fr_vic);
  sid->write(0x18, fr->mode_vol);

  // Only enable filter emulation when both:
  // - at least one voice is routed to the filter (low nibble of $d417)
  // - at least one filter mode is enabled (bits 4..6 of $d418)
  //
  // Otherwise, some instruments can end up effectively muted (routed to filter, but mode=0).
  sid->enable_filter(((fr->fr_vic & 0x0Fu) != 0) && ((fr->mode_vol & 0x70u) != 0));
}

extern "C" void sid_process(struct CHIPS* chips, midi_arrays_t* midi, sw_bank_t* bank, int num_samples, float* outl, float* outr)
{
  if (!chips || !midi || !outl || !outr) return;

  chips->rtime += (uint32_t)(1000000 * (uint32_t)num_samples / chips->sample_freq); // usec
  const uint32_t time_now = chips->rtime;

  for (int i = 0; i < num_samples; i++)
  {
    outr[i] = 0.0f;
    outl[i] = 0.0f;
  }

  const uint32_t tick_usec = (uint32_t)(1000000 / 50); // PAL 50Hz "frame"

  for (int i = 0; i < chips->polyphony; i++)
  {
    if (!(chips->active[i] || midi->midi_keys[i]->note_on)) continue;

    SID* sid = chips->sid_chips[i];
    sw_runtime_t* rt = chips->voices[i];
    uint8_t (*inst)[SW_MAX_INSTSIZE] = &chips->voice_insts[i];

    const int channel = midi->midi_keys[i]->channel;
    if (channel == -1)
    {
      clear_key(midi->midi_keys, i);
      continue;
    }

    const int program = midi->midi_channels[channel].program;
    if (program < 0 || program > 127)
    {
      clear_key(midi->midi_keys, i);
      continue;
    }

    // Handle new MIDI events (note on/off/voice stealing).
    if (midi->midi_keys[i]->note_state_changed)
    {
      if (midi->midi_keys[i]->needs_clearing)
      {
        sid->write(0x04, 0x00); // release voice
        midi->midi_keys[i]->needs_clearing = 0;
        chips->active[i] = -1;
      }
      else if (midi->midi_keys[i]->note_on)
      {
        if (!bank || !sw_bank_get_instrument(bank, (uint8_t)program, (uint8_t)midi->midi_keys[i]->note, *inst))
        {
          clear_key(midi->midi_keys, i);
          continue;
        }

        sw_runtime_config_t cfg = {
            .clear_test_bit = false,
            .default_filter_route = 0x00,
            .volume = (uint8_t)(chips->use_sid_volume ? (midi->midi_keys[i]->velocity / 8) : 0x0F),
        };

        if (!sw_runtime_init(rt, (const uint8_t*)*inst, SW_MAX_INSTSIZE, &cfg))
        {
          clear_key(midi->midi_keys, i);
          continue;
        }

        sw_runtime_note_on(rt, (uint8_t)midi->midi_keys[i]->note, (uint8_t)midi->midi_keys[i]->velocity);
        chips->voice_velocity[i] = (uint8_t)midi->midi_keys[i]->velocity;

        chips->active[i] = 1;
        chips->voice_next_tick[i] = time_now;
        midi->midi_keys[i]->note_state_changed = 0;
      }
      else if (!midi->midi_channels[channel].sustain && !midi->midi_keys[i]->note_on)
      {
        sw_runtime_note_off(rt);
        chips->active[i] = -1;
        midi->midi_keys[i]->note_state_changed = 0;
      }
    }

    // Run the SID-Wizard instrument at 50Hz.
    while (rt->active && time_now >= chips->voice_next_tick[i])
    {
      sw_sid_frame_t fr;
      if (!sw_runtime_tick(rt, &fr)) break;
      write_frame(sid, &fr);
      chips->voice_next_tick[i] += tick_usec;
    }

    // Clock and mix audio for this SID chip.
    int samples_received = 0;
    while (samples_received < num_samples)
    {
      cycle_count cycles = (cycle_count)chips->clocks_per_sample * (cycle_count)(num_samples - samples_received);
      samples_received += chips->sid_chips[i]->clock(cycles, chips->buf + samples_received, num_samples - samples_received);
    }

    unsigned short nz = 0;
    float gain = 1.0f;
    if (!chips->use_sid_volume)
    {
      gain *= (float)chips->voice_velocity[i] / 128.0f;
    }

    for (int j = 0; j < samples_received; j++)
    {
      // DC block
      chips->err[i] -= chips->prevx[i];
      chips->prevx[i] = (chips->buf[j]) << 15;
      chips->err[i] += chips->prevx[i];
      chips->err[i] -= (int32_t)(32.768 * chips->prevy[i]); // (1-.999)<<15
      chips->prevy[i] = chips->err[i] >> 15;
      chips->buf[j] = (short)chips->prevy[i];

      nz |= (unsigned short)chips->buf[j];

      const float a = ((float)chips->buf[j]) / 32768.0f;
      chips->buf[j] = 0;

      outl[j] += gain * a;
      outr[j] += gain * a;
    }

    if (chips->active[i] == -1 && !nz)
    {
      chips->active[i] = 0;
      chips->sid_chips[i]->enable_filter(false);
      chips->prevx[i] = chips->prevy[i] = chips->err[i] = 0;
      midi->midi_keys[i]->channel = -1;
      rt->active = false;
    }
  }
}
