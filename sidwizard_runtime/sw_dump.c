// Minimal CLI to dump the first N frames of a .swi instrument as SID register values.
//
// Build example (Linux/macOS):
//   cc -O2 -std=c99 -Isidwizard_runtime sidwizard_runtime/sw_dump.c sidwizard_runtime/sw_runtime.c -lm -o sw_dump
//
// Usage:
//   ./sw_dump path/to/instrument.swi --note 60 --frames 128

#include "sw_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char* argv0)
{
  fprintf(stderr,
          "usage: %s <file.swi> [--note N] [--frames N]\n",
          argv0);
}

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    usage(argv[0]);
    return 2;
  }

  const char* path = argv[1];
  uint8_t note = 60;
  int frames = 128;

  for (int i = 2; i < argc; ++i)
  {
    if (!strcmp(argv[i], "--note") && i + 1 < argc)
    {
      note = (uint8_t)atoi(argv[++i]);
    }
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
    {
      frames = atoi(argv[++i]);
    }
    else
    {
      usage(argv[0]);
      return 2;
    }
  }

  FILE* f = fopen(path, "rb");
  if (!f)
  {
    perror("fopen");
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 1024 * 1024)
  {
    fprintf(stderr, "bad file size\n");
    fclose(f);
    return 1;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf)
  {
    fprintf(stderr, "oom\n");
    fclose(f);
    return 1;
  }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
  {
    fprintf(stderr, "read failed\n");
    free(buf);
    fclose(f);
    return 1;
  }
  fclose(f);

  sw_runtime_config_t cfg = {
      .clear_test_bit = false,
      .default_filter_route = 0x01,
      .volume = 0x0F,
  };

  uint8_t inst[SW_MAX_INSTSIZE];
  if (!sw_swi_unpack_128(buf, (size_t)sz, inst, NULL))
  {
    fprintf(stderr, "failed to unpack .swi\n");
    free(buf);
    return 1;
  }

  sw_runtime_t rt;
  if (!sw_runtime_init(&rt, inst, sizeof(inst), &cfg))
  {
    fprintf(stderr, "runtime init failed\n");
    free(buf);
    return 1;
  }

  sw_runtime_note_on(&rt, note, 127);

  for (int i = 0; i < frames; ++i)
  {
    sw_sid_frame_t fr;
    if (!sw_runtime_tick(&rt, &fr))
    {
      fprintf(stderr, "tick failed at frame %d\n", i);
      break;
    }
    printf("%d freq=%u pw=%u ctrl=0x%02X ad=0x%02X sr=0x%02X cut=0x%03X fr_vic=0x%02X mode_vol=0x%02X\n",
           i,
           (unsigned)fr.freq_reg,
           (unsigned)fr.pulse_reg,
           (unsigned)fr.control,
           (unsigned)fr.ad,
           (unsigned)fr.sr,
           (unsigned)fr.filter_cutoff,
           (unsigned)fr.fr_vic,
           (unsigned)fr.mode_vol);
  }

  free(buf);
  return 0;
}
