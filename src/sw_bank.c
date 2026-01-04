#include "sw_bank.h"

#include "../sidwizard_runtime/sw_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SLOT_EMPTY = 0, SLOT_INSTRUMENT = 1, SLOT_DRUMKIT = 2 } slot_type_t;

typedef struct program_slot {
  slot_type_t type;
  int index; // instrument index or drumkit index
} program_slot_t;

typedef struct instrument_entry {
  char* path; // absolute/expanded
  char name[SW_INST_NAME_LEN + 1];
  uint8_t inst[SW_MAX_INSTSIZE];
} instrument_entry_t;

typedef struct drumkit_entry {
  char* name;
  int note_to_inst[128]; // instrument index, -1 unmapped
} drumkit_entry_t;

struct sw_bank {
  char* name;
  char* base_dir;
  program_slot_t programs[128];
  instrument_entry_t* instruments;
  size_t instrument_count;
  drumkit_entry_t* drumkits;
  size_t drumkit_count;
};

static char* sw_strdup(const char* s)
{
  if (!s) return NULL;
  const size_t n = strlen(s);
  char* out = (char*)malloc(n + 1u);
  if (!out) return NULL;
  memcpy(out, s, n + 1u);
  return out;
}

static void trim(char* s)
{
  if (!s) return;
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) s[--n] = 0;
  size_t i = 0;
  while (s[i] && isspace((unsigned char)s[i])) i++;
  if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static bool is_abs_path(const char* p)
{
  if (!p || !*p) return false;
  if (p[0] == '/' || p[0] == '\\') return true;
  if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
  return false;
}

static char* dirname_dup(const char* path)
{
  if (!path) return NULL;
  const char* last_slash = strrchr(path, '/');
  const char* last_back = strrchr(path, '\\');
  const char* last = last_slash;
  if (!last || (last_back && last_back > last)) last = last_back;
  if (!last) return sw_strdup(".");
  size_t len = (size_t)(last - path);
  if (!len) len = 1;
  char* out = (char*)malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, path, len);
  out[len] = 0;
  return out;
}

static char* path_join(const char* a, const char* b)
{
  if (!a || !*a) return sw_strdup(b ? b : "");
  if (!b || !*b) return sw_strdup(a);
  const size_t al = strlen(a);
  const size_t bl = strlen(b);
  const bool need_sep = (a[al - 1] != '/' && a[al - 1] != '\\');
  char* out = (char*)malloc(al + (need_sep ? 1 : 0) + bl + 1);
  if (!out) return NULL;
  memcpy(out, a, al);
  size_t pos = al;
  if (need_sep) out[pos++] = '/';
  memcpy(out + pos, b, bl);
  out[pos + bl] = 0;
  return out;
}

static char* resolve_path(const char* bank_path, const char* base_dir, const char* rel)
{
  if (!rel) return NULL;
  if (is_abs_path(rel)) return sw_strdup(rel);

  char* bank_dir = dirname_dup(bank_path);
  if (!bank_dir) return NULL;

  char* base = NULL;
  if (base_dir && *base_dir)
  {
    base = is_abs_path(base_dir) ? sw_strdup(base_dir) : path_join(bank_dir, base_dir);
  }
  else
  {
    base = sw_strdup(bank_dir);
  }

  free(bank_dir);
  if (!base) return NULL;
  char* out = path_join(base, rel);
  free(base);
  return out;
}

static bool read_file(const char* path, uint8_t** out_buf, size_t* out_len)
{
  *out_buf = NULL;
  *out_len = 0;
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return false;
  }
  long sz = ftell(f);
  if (sz <= 0 || sz > (long)(1024 * 1024))
  {
    fclose(f);
    return false;
  }
  if (fseek(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    return false;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf)
  {
    fclose(f);
    return false;
  }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
  {
    free(buf);
    fclose(f);
    return false;
  }
  fclose(f);
  *out_buf = buf;
  *out_len = (size_t)sz;
  return true;
}

static int find_instrument_by_path(const sw_bank_t* bank, const char* abs_path)
{
  for (size_t i = 0; i < bank->instrument_count; ++i)
  {
    if (!strcmp(bank->instruments[i].path, abs_path)) return (int)i;
  }
  return -1;
}

static int add_instrument(sw_bank_t* bank, const char* abs_path)
{
  const int existing = find_instrument_by_path(bank, abs_path);
  if (existing >= 0) return existing;

  uint8_t* buf = NULL;
  size_t len = 0;
  if (!read_file(abs_path, &buf, &len)) return -1;

  uint8_t inst[SW_MAX_INSTSIZE];
  sw_swi_info_t info;
  const bool ok = sw_swi_unpack_128(buf, len, inst, &info);
  free(buf);
  if (!ok) return -1;

  instrument_entry_t* next = (instrument_entry_t*)realloc(bank->instruments, (bank->instrument_count + 1) * sizeof(*next));
  if (!next) return -1;
  bank->instruments = next;

  instrument_entry_t* e = &bank->instruments[bank->instrument_count++];
  memset(e, 0, sizeof(*e));
  e->path = sw_strdup(abs_path);
  if (!e->path) return -1;
  memcpy(e->inst, inst, SW_MAX_INSTSIZE);

  // Name: from SWI info bytes (ASCII-ish), trimmed.
  for (int i = 0; i < SW_INST_NAME_LEN; ++i)
  {
    char c = (char)info.name[i];
    e->name[i] = (c >= 0x20 && c <= 0x7E) ? c : ' ';
  }
  e->name[SW_INST_NAME_LEN] = 0;
  trim(e->name);

  return (int)(bank->instrument_count - 1);
}

static int find_drumkit(sw_bank_t* bank, const char* name)
{
  for (size_t i = 0; i < bank->drumkit_count; ++i)
  {
    if (!strcmp(bank->drumkits[i].name, name)) return (int)i;
  }
  return -1;
}

static int add_drumkit(sw_bank_t* bank, const char* name)
{
  const int existing = find_drumkit(bank, name);
  if (existing >= 0) return existing;

  drumkit_entry_t* next = (drumkit_entry_t*)realloc(bank->drumkits, (bank->drumkit_count + 1) * sizeof(*next));
  if (!next) return -1;
  bank->drumkits = next;

  drumkit_entry_t* dk = &bank->drumkits[bank->drumkit_count++];
  memset(dk, 0, sizeof(*dk));
  dk->name = sw_strdup(name);
  if (!dk->name) return -1;
  for (int i = 0; i < 128; ++i) dk->note_to_inst[i] = -1;

  return (int)(bank->drumkit_count - 1);
}

static bool starts_with_ci(const char* s, const char* pfx)
{
  while (*pfx)
  {
    if (!*s) return false;
    if (tolower((unsigned char)*s) != tolower((unsigned char)*pfx)) return false;
    ++s;
    ++pfx;
  }
  return true;
}

static void init_defaults(sw_bank_t* bank)
{
  for (int i = 0; i < 128; ++i)
  {
    bank->programs[i].type = SLOT_EMPTY;
    bank->programs[i].index = -1;
  }
}

sw_bank_t* sw_bank_load(const char* bank_path)
{
  if (!bank_path) return NULL;
  FILE* f = fopen(bank_path, "rb");
  if (!f)
  {
    fprintf(stderr, "reMID.lv2: swibank: can't open %s: %s\n", bank_path, strerror(errno));
    return NULL;
  }

  sw_bank_t* bank = (sw_bank_t*)calloc(1, sizeof(*bank));
  if (!bank)
  {
    fclose(f);
    return NULL;
  }
  init_defaults(bank);

  int load_errors = 0;

  char section[128] = {0};

  char line[1024];
  while (fgets(line, sizeof(line), f))
  {
    trim(line);
    if (!line[0]) continue;
    if (line[0] == '#' || line[0] == ';') continue;

    if (line[0] == '[')
    {
      char* end = strchr(line, ']');
      if (!end) continue;
      *end = 0;
      strncpy(section, line + 1, sizeof(section) - 1);
      section[sizeof(section) - 1] = 0;
      trim(section);
      continue;
    }

    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char* key = line;
    char* val = eq + 1;
    trim(key);
    trim(val);
    if (!key[0]) continue;

    if (starts_with_ci(section, "meta"))
    {
      if (!strcmp(key, "name"))
      {
        free(bank->name);
        bank->name = sw_strdup(val);
      }
      else if (!strcmp(key, "base"))
      {
        free(bank->base_dir);
        bank->base_dir = sw_strdup(val);
      }
      continue;
    }

    if (starts_with_ci(section, "programs"))
    {
      int program = (int)strtol(key, NULL, 0);
      if (program < 0 || program > 127) continue;

      if (starts_with_ci(val, "DRUMKIT:"))
      {
        const char* dk_name = val + 8;
        while (*dk_name && isspace((unsigned char)*dk_name)) dk_name++;
        if (!*dk_name) continue;
        const int dk_idx = add_drumkit(bank, dk_name);
        if (dk_idx < 0) continue;
        bank->programs[program].type = SLOT_DRUMKIT;
        bank->programs[program].index = dk_idx;
      }
      else
      {
        char* abs = resolve_path(bank_path, bank->base_dir, val);
        if (!abs) continue;
        const int inst_idx = add_instrument(bank, abs);
        if (inst_idx < 0)
        {
          fprintf(stderr, "reMID.lv2: swibank %s: failed to load %s\n", bank_path, abs);
          load_errors++;
        }
        free(abs);
        if (inst_idx < 0) continue;
        bank->programs[program].type = SLOT_INSTRUMENT;
        bank->programs[program].index = inst_idx;
      }
      continue;
    }

    // drumkit section forms:
    //   [drumkit:gm]
    //   [drumkit gm]
    const char* dk_pfx1 = "drumkit:";
    const char* dk_pfx2 = "drumkit ";
    const char* dk_name = NULL;
    if (starts_with_ci(section, dk_pfx1))
    {
      dk_name = section + strlen(dk_pfx1);
    }
    else if (starts_with_ci(section, dk_pfx2))
    {
      dk_name = section + strlen(dk_pfx2);
    }
    if (dk_name)
    {
      while (*dk_name && isspace((unsigned char)*dk_name)) dk_name++;
      if (!*dk_name) continue;
      const int dk_idx = add_drumkit(bank, dk_name);
      if (dk_idx < 0) continue;

      const int note = (int)strtol(key, NULL, 0);
      if (note < 0 || note > 127) continue;

      char* abs = resolve_path(bank_path, bank->base_dir, val);
      if (!abs) continue;
      const int inst_idx = add_instrument(bank, abs);
      if (inst_idx < 0)
      {
        fprintf(stderr, "reMID.lv2: swibank %s: failed to load %s\n", bank_path, abs);
        load_errors++;
      }
      free(abs);
      if (inst_idx < 0) continue;

      bank->drumkits[dk_idx].note_to_inst[note] = inst_idx;
      continue;
    }
  }

  fclose(f);

  if (bank->instrument_count == 0)
  {
    fprintf(stderr, "reMID.lv2: swibank %s: no instruments loaded (check base= and instruments/swi/)\n", bank_path);
    sw_bank_free(bank);
    return NULL;
  }
  if (load_errors)
  {
    fprintf(stderr, "reMID.lv2: swibank %s: had %d load error(s)\n", bank_path, load_errors);
  }

  return bank;
}

void sw_bank_free(sw_bank_t* bank)
{
  if (!bank) return;
  free(bank->name);
  free(bank->base_dir);
  for (size_t i = 0; i < bank->instrument_count; ++i)
  {
    free(bank->instruments[i].path);
  }
  for (size_t i = 0; i < bank->drumkit_count; ++i)
  {
    free(bank->drumkits[i].name);
  }
  free(bank->instruments);
  free(bank->drumkits);
  free(bank);
}

const char* sw_bank_name(const sw_bank_t* bank)
{
  return bank ? bank->name : NULL;
}

static const char* path_basename_local(const char* path)
{
  if (!path) return NULL;
  const char* last_slash = strrchr(path, '/');
  const char* last_back = strrchr(path, '\\');
  const char* last = last_slash;
  if (!last || (last_back && last_back > last)) last = last_back;
  return last ? (last + 1) : path;
}

bool sw_bank_describe_program(const sw_bank_t* bank, uint8_t program, char* out, size_t out_len)
{
  if (!bank || !out || out_len == 0) return false;
  out[0] = 0;
  if (program > 127) return false;

  const program_slot_t slot = bank->programs[program];
  if (slot.type == SLOT_EMPTY || slot.index < 0) return false;

  if (slot.type == SLOT_INSTRUMENT)
  {
    if ((size_t)slot.index >= bank->instrument_count) return false;
    const instrument_entry_t* e = &bank->instruments[slot.index];
    if (e->name[0])
    {
      snprintf(out, out_len, "%s", e->name);
      return true;
    }
    const char* base = path_basename_local(e->path);
    if (!base) return false;
    // Strip .swi if present.
    const char* dot = strrchr(base, '.');
    if (dot && !strcmp(dot, ".swi"))
    {
      const size_t n = (size_t)(dot - base);
      if (n + 1 > out_len) return false;
      memcpy(out, base, n);
      out[n] = 0;
      return true;
    }
    snprintf(out, out_len, "%s", base);
    return true;
  }

  if (slot.type == SLOT_DRUMKIT)
  {
    if ((size_t)slot.index >= bank->drumkit_count) return false;
    const drumkit_entry_t* dk = &bank->drumkits[slot.index];
    if (!dk->name) return false;
    snprintf(out, out_len, "DRUMKIT:%s", dk->name);
    return true;
  }

  return false;
}

bool sw_bank_get_instrument(const sw_bank_t* bank, uint8_t program, uint8_t note, uint8_t out_inst[128])
{
  if (!bank || !out_inst) return false;

  const program_slot_t slot = bank->programs[program];
  if (slot.type == SLOT_EMPTY || slot.index < 0) return false;

  int inst_idx = -1;
  if (slot.type == SLOT_INSTRUMENT)
  {
    inst_idx = slot.index;
  }
  else if (slot.type == SLOT_DRUMKIT)
  {
    if ((size_t)slot.index >= bank->drumkit_count) return false;
    inst_idx = bank->drumkits[slot.index].note_to_inst[note];
  }

  if (inst_idx < 0 || (size_t)inst_idx >= bank->instrument_count) return false;
  memcpy(out_inst, bank->instruments[inst_idx].inst, 128);
  return true;
}
