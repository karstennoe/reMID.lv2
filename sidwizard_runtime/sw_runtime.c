#include "sw_runtime.h"

#include <string.h>

static uint16_t u16_wrap(uint16_t v, uint16_t mask)
{
  return (uint16_t)(v & mask);
}

static bool read_u8(const uint8_t* payload, size_t payload_len, uint16_t off, uint8_t* out)
{
  if ((size_t)off >= payload_len) return false;
  *out = payload[off];
  return true;
}

static uint16_t count_rows_ff(const uint8_t* payload, size_t payload_len, uint8_t base)
{
  if ((size_t)base >= payload_len) return 0;
  const size_t max_rows = (payload_len - (size_t)base) / 3u;
  uint16_t rows = 0;
  for (size_t i = 0; i < max_rows && i < 0xFFFFu; ++i)
  {
    const size_t off = (size_t)base + i * 3u;
    if (payload[off] == 0xFFu) break;
    rows++;
  }
  return rows;
}

static uint8_t clamp_pitch(uint8_t p)
{
  if (p > 95u) return 95u;
  return p;
}

// SID-Wizard player tables extracted from SID-Wizard-1.7/sources/include/player.asm
static const uint8_t SW_FREQTBL[96] = {
  0x07u, 0x16u, 0x27u, 0x38u, 0x4Bu, 0x5Eu, 0x73u, 0x89u, 0xA1u, 0xBAu, 0xD4u, 0xF0u,
  0x0Du, 0x2Cu, 0x4Eu, 0x71u, 0x96u, 0xBDu, 0xE7u, 0x13u, 0x42u, 0x74u, 0xA8u, 0xE0u,
  0x1Bu, 0x59u, 0x9Cu, 0xE2u, 0x2Cu, 0x7Bu, 0xCEu, 0x27u, 0x84u, 0xE8u, 0x51u, 0xC0u,
  0x36u, 0xB3u, 0x38u, 0xC4u, 0x59u, 0xF6u, 0x9Du, 0x4Eu, 0x09u, 0xD0u, 0xA2u, 0x81u,
  0x6Du, 0x67u, 0x70u, 0x88u, 0xB2u, 0xEDu, 0x3Au, 0x9Cu, 0x13u, 0xA0u, 0x44u, 0x02u,
  0xDAu, 0xCEu, 0xE0u, 0x11u, 0x64u, 0xDAu, 0x75u, 0x38u, 0x26u, 0x40u, 0x89u, 0x04u,
  0xB4u, 0x9Cu, 0xC0u, 0x22u, 0xC8u, 0xB4u, 0xEBu, 0x71u, 0x4Cu, 0x80u, 0x12u, 0x08u,
  0x68u, 0x38u, 0x80u, 0x45u, 0x90u, 0x68u, 0xD6u, 0xE3u, 0x98u, 0x00u, 0x24u, 0x10u,
};

static const uint8_t SW_FREQTBH[96] = {
  0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u,
  0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u,
  0x04u, 0x04u, 0x04u, 0x04u, 0x05u, 0x05u, 0x05u, 0x06u, 0x06u, 0x06u, 0x07u, 0x07u,
  0x08u, 0x08u, 0x09u, 0x09u, 0x0Au, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Du, 0x0Eu, 0x0Fu,
  0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x17u, 0x18u, 0x1Au, 0x1Bu, 0x1Du, 0x1Fu,
  0x20u, 0x22u, 0x24u, 0x27u, 0x29u, 0x2Bu, 0x2Eu, 0x31u, 0x34u, 0x37u, 0x3Au, 0x3Eu,
  0x41u, 0x45u, 0x49u, 0x4Eu, 0x52u, 0x57u, 0x5Cu, 0x62u, 0x68u, 0x6Eu, 0x75u, 0x7Cu,
  0x83u, 0x8Bu, 0x93u, 0x9Cu, 0xA5u, 0xAFu, 0xB9u, 0xC4u, 0xD0u, 0xDDu, 0xEAu, 0xF8u,
};

static const uint8_t SW_EXPTBASE[107] = {
  0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
  0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x02u,
  0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u, 0x04u,
  0x04u, 0x04u, 0x04u, 0x05u, 0x05u, 0x05u, 0x06u, 0x06u, 0x06u, 0x07u, 0x07u, 0x08u,
  0x08u, 0x09u, 0x09u, 0x0Au, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Du, 0x0Eu, 0x0Fu, 0x10u,
  0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x17u, 0x18u, 0x1Au, 0x1Bu, 0x1Du, 0x1Fu, 0x20u,
  0x22u, 0x24u, 0x27u, 0x29u, 0x2Bu, 0x2Eu, 0x31u, 0x34u, 0x37u, 0x3Au, 0x3Eu, 0x41u,
  0x45u, 0x49u, 0x4Eu, 0x52u, 0x57u, 0x5Cu, 0x62u, 0x68u, 0x6Eu, 0x75u, 0x7Cu, 0x83u,
  0x8Bu, 0x93u, 0x9Cu, 0xA5u, 0xAFu, 0xB9u, 0xC4u, 0xD0u, 0xDDu, 0xEAu, 0xF8u,
};

enum { SW_EXPTRESHOLD = 107, SW_MAXSLID = 203 };

size_t sw_swi_payload_offset(const uint8_t* file_bytes, size_t file_len)
{
  if (!file_bytes || file_len < 4) return 0;
  const uint16_t load_addr = (uint16_t)(file_bytes[0] | ((uint16_t)file_bytes[1] << 8));
  if (load_addr >= 0x0300u && load_addr <= 0xC000u && file_len >= 34u) return 2;
  return 0;
}

static bool sw_is_packed_swi_payload(const uint8_t* payload, size_t payload_len, uint8_t* out_size)
{
  if (!payload || payload_len < (size_t)(1u + SW_INST_NAME_LEN)) return false;
  const size_t size_index = payload_len - (size_t)(1u + SW_INST_NAME_LEN);
  const uint8_t size_byte = payload[size_index];

  // Packed instruments store exactly: [0..size-1][size][name(8)].
  if ((size_t)size_byte != size_index) return false;
  if (payload_len != (size_t)size_byte + (size_t)(1u + SW_INST_NAME_LEN)) return false;
  if (size_byte > (uint8_t)(SW_MAX_INSTSIZE - SW_INST_NAME_LEN - 1u)) return false;

  if (out_size) *out_size = size_byte;
  return true;
}

bool sw_swi_unpack_128(const uint8_t* file_bytes, size_t file_len, uint8_t out_inst[SW_MAX_INSTSIZE], sw_swi_info_t* out_info)
{
  if (!file_bytes || !out_inst || file_len < (size_t)(1u + SW_INST_NAME_LEN)) return false;

  if (out_info) memset(out_info, 0, sizeof(*out_info));

  const uint8_t* payload = file_bytes;
  size_t payload_len = file_len;
  uint16_t load_addr = 0;

  // Prefer stripping PRG load address if it yields a well-formed packed instrument.
  if (file_len >= (size_t)(2u + 1u + SW_INST_NAME_LEN))
  {
    const uint16_t cand_load = (uint16_t)(file_bytes[0] | ((uint16_t)file_bytes[1] << 8));
    const uint8_t* cand_payload = file_bytes + 2;
    const size_t cand_len = file_len - 2u;
    if (sw_is_packed_swi_payload(cand_payload, cand_len, NULL))
    {
      load_addr = cand_load;
      payload = cand_payload;
      payload_len = cand_len;
    }
  }

  // If not a packed payload after PRG stripping, try treating the whole file as packed.
  if (!sw_is_packed_swi_payload(payload, payload_len, NULL))
  {
    if (sw_is_packed_swi_payload(file_bytes, file_len, NULL))
    {
      payload = file_bytes;
      payload_len = file_len;
      load_addr = 0;
    }
  }

  memset(out_inst, 0xFF, SW_MAX_INSTSIZE);

  uint8_t packed_size = 0;
  if (sw_is_packed_swi_payload(payload, payload_len, &packed_size))
  {
    // Copy instrument bytes 0..size-1.
    if (packed_size)
    {
      memcpy(out_inst, payload, (size_t)packed_size);
    }
    // Restore the final table terminator.
    out_inst[packed_size] = 0xFFu;
    // Copy name to the fixed in-memory name area.
    memcpy(out_inst + (SW_MAX_INSTSIZE - SW_INST_NAME_LEN),
           payload + (size_t)packed_size + 1u,
           SW_INST_NAME_LEN);

    if (out_info)
    {
      out_info->prg_load_addr = load_addr;
      out_info->packed_size = packed_size;
      memcpy(out_info->name, out_inst + (SW_MAX_INSTSIZE - SW_INST_NAME_LEN), SW_INST_NAME_LEN);
    }
    return true;
  }

  // Fallback: treat as raw/unpacked image (with optional PRG load address stripped).
  size_t off = sw_swi_payload_offset(file_bytes, file_len);
  const uint8_t* raw = file_bytes + off;
  const size_t raw_len = file_len - off;
  if (raw_len < 0x20u) return false;

  const size_t copy_n = (raw_len < SW_MAX_INSTSIZE) ? raw_len : (size_t)SW_MAX_INSTSIZE;
  memcpy(out_inst, raw, copy_n);
  if (out_info)
  {
    out_info->prg_load_addr = (off == 2u) ? (uint16_t)(file_bytes[0] | ((uint16_t)file_bytes[1] << 8)) : 0;
    memcpy(out_info->name, out_inst + (SW_MAX_INSTSIZE - SW_INST_NAME_LEN), SW_INST_NAME_LEN);
  }
  return true;
}

static uint8_t sanitize_control(const sw_runtime_t* rt, uint8_t c)
{
  if (!rt->cfg.clear_test_bit) return c;
  return (uint8_t)(c & (uint8_t)~0x08u);
}

static void set_pitch_from_table(sw_runtime_t* rt, uint8_t dpitch)
{
  dpitch = clamp_pitch(dpitch);
  rt->freql = SW_FREQTBL[dpitch];
  rt->freqh = SW_FREQTBH[dpitch];
}

static void sw_setfmod(sw_runtime_t* rt, uint8_t amp_from_setvamp)
{
  if (!amp_from_setvamp)
  {
    rt->freqmodl = 0;
    rt->freqmodh = 0;
    return;
  }

  // CALCVIBRATO_ON behavior (SETFMOD): A is amplitude in 0..127-ish domain here.
  uint8_t a = amp_from_setvamp;
  a = (uint8_t)(a >> 1); // lsr

  uint16_t y = (uint16_t)a + (uint16_t)rt->dpitch_base;
  if (y > (uint16_t)SW_MAXSLID) y = (uint16_t)SW_MAXSLID;

  if (y >= (uint16_t)SW_EXPTRESHOLD)
  {
    uint16_t idx = y - (uint16_t)SW_EXPTRESHOLD;
    if (idx > 95u) idx = 95u;
    rt->freqmodl = SW_FREQTBL[idx];
    rt->freqmodh = SW_FREQTBH[idx];
  }
  else
  {
    rt->freqmodl = SW_EXPTBASE[y];
    rt->freqmodh = 0;
  }
}

static void sw_setvibrato(sw_runtime_t* rt, uint8_t vib_byte)
{
  // SETVIBR: rate nibble
  uint8_t rate = (uint8_t)(vib_byte & 0x0Fu);
  rt->vibfrequ = (uint8_t)(rate << 1); // ASL

  // Starting counter (SETVIBR logic).
  uint8_t start = (uint8_t)(rt->vibfrequ >> 1); // LSR
  if (rt->slidevib < 0x20u) start = (uint8_t)(start >> 1); // LSR for "normal vibrato"
  if (rt->slidevib == 0x30u) start = 0;
  rt->vibracnt = start;

  // SETVAMP: amplitude nibble -> A = (vib_byte & $F0) >> 1.
  uint8_t amp = (uint8_t)(vib_byte & 0xF0u);
  amp = (uint8_t)(amp >> 1);
  sw_setfmod(rt, amp);
}

bool sw_runtime_init(sw_runtime_t* rt, const uint8_t* payload, size_t payload_len, const sw_runtime_config_t* cfg)
{
  if (!rt || !payload || payload_len < SW_MAX_INSTSIZE) return false;
  memset(rt, 0, sizeof(*rt));
  rt->payload = payload;
  rt->payload_len = payload_len;

  rt->cfg.clear_test_bit = false;
  // Default to bypassing the filter routing; some instruments never set filter mode,
  // and routing a voice into a filter with mode=0 can effectively mute it.
  rt->cfg.default_filter_route = 0x00;
  rt->cfg.volume = 0x0F;
  if (cfg) rt->cfg = *cfg;

  rt->ins_ctrl = payload[0x00];
  rt->ad = payload[SW_AD];
  rt->sr = payload[SW_SR];
  rt->wf0 = payload[SW_WF0];
  rt->pw_base = payload[SW_PWPT];
  rt->fl_base = payload[SW_FLPT];
  rt->arp_speed = (uint8_t)(payload[SW_ARPS] & 0x3Fu);
  rt->octave_shift = (int8_t)payload[0x09];

  rt->wf_rows = count_rows_ff(payload, payload_len, (uint8_t)SW_WFTABLEPOS);
  rt->pw_rows = count_rows_ff(payload, payload_len, rt->pw_base);
  rt->fl_rows = count_rows_ff(payload, payload_len, rt->fl_base);

  rt->filter_route = (uint8_t)(rt->cfg.default_filter_route & 0x0Fu);
  rt->active = false;
  return true;
}

void sw_runtime_note_on(sw_runtime_t* rt, uint8_t midi_note, uint8_t velocity)
{
  (void)velocity;
  if (!rt || !rt->payload) return;

  rt->active = true;
  rt->midi_note = midi_note;

  // DPITCH in player is "discrete pitch" (0..95). MIDI note 0 == C-1 in SW tables, which is DPITCH 1.
  int16_t dp = (int16_t)midi_note + 1 + (int16_t)rt->octave_shift;
  if (dp < 0) dp = 0;
  if (dp > 95) dp = 95;
  rt->dpitch_base = (uint8_t)dp;

  rt->ptn_gate = 0xFFu;
  rt->detuner = 0x00u;

  // Frame-1 waveform/control
  rt->wfghost = (uint8_t)(sanitize_control(rt, rt->wf0) & rt->ptn_gate);

  // Reset WF table pointer and ARP counter.
  rt->wft_pos = (uint8_t)SW_WFTABLEPOS;
  rt->arps_cnt = -1; // $FF then DEC -> negative triggers immediate table load

  // Base pitch registers (FREQLO/FREQHI).
  set_pitch_from_table(rt, rt->dpitch_base);

  // PW defaults and pointer.
  rt->pwt_pos = rt->pw_base;
  rt->pw_sweep_cnt = 0;
  rt->pw_hi = 0x08u;
  rt->pw_lo = 0x00u;

  // Filter defaults and pointer.
  rt->flt_pos = rt->fl_base;
  rt->fl_sweep_cnt = 0;
  rt->cutoff_11 = 0;
  rt->filter_band = 0;
  rt->resonance_hi = 0;

  // Vibrato init (SETVIB0/SETVIB1)
  rt->slidevib = (uint8_t)(rt->ins_ctrl & 0x30u);
  rt->videlcnt = rt->payload[SW_VIB2];
  sw_setvibrato(rt, rt->payload[SW_VIB]);
}

void sw_runtime_note_off(sw_runtime_t* rt)
{
  if (!rt || !rt->payload) return;

  // Mirror NGATEOF behavior.
  const uint8_t go_wf = rt->payload[SW_GO_WF];
  if (go_wf) rt->wft_pos = go_wf;

  rt->ptn_gate = 0xFEu;
  rt->wfghost = (uint8_t)(rt->wfghost & 0xFEu);

  const uint8_t go_pw = rt->payload[SW_GO_PW];
  if (go_pw) rt->pwt_pos = go_pw;

  const uint8_t go_fl = rt->payload[SW_GO_FL];
  if (go_fl) rt->flt_pos = go_fl;
}

static void tick_vibrato(sw_runtime_t* rt)
{
  if (!rt->vibfrequ) return;
  if (!rt->freqmodl && !rt->freqmodh) return;

  // VIBSLIDE: only vibrato types are represented in SLIDEVIB by default.
  if (rt->slidevib == 0x00u)
  {
    // Increasing type: FREQMOD += VIDELCNT each frame.
    uint16_t fmod = (uint16_t)((uint16_t)rt->freqmodh << 8 | rt->freqmodl);
    fmod = (uint16_t)(fmod + (uint16_t)rt->videlcnt);
    rt->freqmodl = (uint8_t)(fmod & 0xFFu);
    rt->freqmodh = (uint8_t)(fmod >> 8);
  }
  else
  {
    // Normal types: delay counter counts down until it wraps to $FF.
    if ((rt->videlcnt & 0x80u) == 0u)
    {
      rt->videlcnt = (uint8_t)(rt->videlcnt - 1u);
      return;
    }
  }

  // DOVIBRA
  if (rt->vibracnt)
  {
    rt->vibracnt = (uint8_t)(rt->vibracnt - 1u);
  }
  else
  {
    rt->vibracnt = (uint8_t)(rt->vibfrequ - 1u);
  }

  const uint8_t doubled = (uint8_t)(rt->vibracnt << 1);
  const bool add_dir = (doubled < rt->vibfrequ);

  uint16_t freq = (uint16_t)((uint16_t)rt->freqh << 8 | rt->freql);
  const uint16_t fmod = (uint16_t)((uint16_t)rt->freqmodh << 8 | rt->freqmodl);

  freq = add_dir ? (uint16_t)(freq + fmod) : (uint16_t)(freq - fmod);

  rt->freql = (uint8_t)(freq & 0xFFu);
  rt->freqh = (uint8_t)(freq >> 8);
}

static void tick_pw(sw_runtime_t* rt)
{
  if (!rt->pw_rows) return;
  if ((size_t)rt->pwt_pos >= rt->payload_len) return;

  uint8_t col0;
  if (!read_u8(rt->payload, rt->payload_len, rt->pwt_pos, &col0)) return;

  // Sweep row if < $80.
  if ((col0 & 0x80u) == 0u)
  {
    if (col0 == rt->pw_sweep_cnt)
    {
      rt->pwt_pos = (uint8_t)(rt->pwt_pos + 3u);
      rt->pw_sweep_cnt = 0;
      return;
    }
    rt->pw_sweep_cnt++;

    uint8_t delta_u8;
    if (!read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->pwt_pos + 1u), &delta_u8)) return;
    const int8_t delta = (int8_t)delta_u8;

    uint16_t pw = (uint16_t)((((uint16_t)rt->pw_hi & 0x0Fu) << 8) | rt->pw_lo);
    pw = u16_wrap((uint16_t)((int32_t)pw + (int32_t)delta), 0x0FFFu);
    rt->pw_hi = (uint8_t)((pw >> 8) & 0x0Fu);
    rt->pw_lo = (uint8_t)(pw & 0xFFu);
    return;
  }

  if (col0 == 0xFFu)
  {
    return;
  }

  if (col0 == 0xFEu)
  {
    uint8_t j;
    if (!read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->pwt_pos + 1u), &j)) return;
    if (j == rt->pwt_pos) return;
    rt->pwt_pos = j;
    rt->pw_sweep_cnt = 0;

    if (!read_u8(rt->payload, rt->payload_len, rt->pwt_pos, &col0)) return;
    if ((col0 & 0x80u) == 0u || col0 == 0xFEu || col0 == 0xFFu) return;
  }

  // Absolute set row: col0 in $80..$FD.
  rt->pw_hi = (uint8_t)(col0 & 0x7Fu);
  read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->pwt_pos + 1u), &rt->pw_lo);
  rt->pwt_pos = (uint8_t)(rt->pwt_pos + 3u);
  rt->pw_sweep_cnt = 0;
}

static void tick_filter(sw_runtime_t* rt)
{
  if (!rt->fl_rows) return;
  if ((size_t)rt->flt_pos >= rt->payload_len) return;

  uint8_t col0;
  if (!read_u8(rt->payload, rt->payload_len, rt->flt_pos, &col0)) return;

  // Sweep row if < $80.
  if ((col0 & 0x80u) == 0u)
  {
    if (col0 == rt->fl_sweep_cnt)
    {
      rt->flt_pos = (uint8_t)(rt->flt_pos + 3u);
      rt->fl_sweep_cnt = 0;
      return;
    }
    rt->fl_sweep_cnt++;

    uint8_t delta_u8;
    if (!read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->flt_pos + 1u), &delta_u8)) return;
    const int8_t delta = (int8_t)delta_u8;

    rt->cutoff_11 = u16_wrap((uint16_t)((int32_t)rt->cutoff_11 + (int32_t)delta), 0x07FFu);
    return;
  }

  if (col0 == 0xFFu)
  {
    return;
  }

  if (col0 == 0xFEu)
  {
    uint8_t j;
    if (!read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->flt_pos + 1u), &j)) return;
    if (j == rt->flt_pos) return;
    rt->flt_pos = j;
    rt->fl_sweep_cnt = 0;

    if (!read_u8(rt->payload, rt->payload_len, rt->flt_pos, &col0)) return;
    if ((col0 & 0x80u) == 0u || col0 == 0xFEu || col0 == 0xFFu) return;
  }

  // Absolute set row: col0 in $80..$FD.
  rt->filter_band = (uint8_t)(col0 & 0x70u);
  rt->resonance_hi = (uint8_t)((col0 & 0x0Fu) << 4);

  uint8_t cutoff_hi;
  if (!read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->flt_pos + 1u), &cutoff_hi)) return;
  rt->cutoff_11 = (uint16_t)((uint16_t)cutoff_hi << 3); // fine sweep uses low 3 bits as 0 after set

  // Filter table third column: allow overriding filter route via $80..$8F (as in player).
  uint8_t col2;
  if (read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->flt_pos + 2u), &col2))
  {
    if ((col2 & 0xF0u) == 0x80u)
    {
      rt->filter_route = (uint8_t)(col2 & 0x0Fu);
    }
  }

  rt->flt_pos = (uint8_t)(rt->flt_pos + 3u);
  rt->fl_sweep_cnt = 0;
}

static void tick_wfarp(sw_runtime_t* rt)
{
  rt->arps_cnt -= 1;
  if (rt->arps_cnt >= 0)
  {
    return;
  }

  rt->arps_cnt = (int16_t)(rt->arp_speed & 0x3Fu);

  uint8_t wf;
  if (!read_u8(rt->payload, rt->payload_len, rt->wft_pos, &wf)) return;

  if (wf == 0xFFu)
  {
    return;
  }

  if (wf == 0xFEu)
  {
    uint8_t j;
    if (!read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->wft_pos + 1u), &j)) return;
    if (j & 0x80u) return; // player treats bit7 set jumps as end
    rt->wft_pos = j;
    if (!read_u8(rt->payload, rt->payload_len, rt->wft_pos, &wf)) return;
  }

  if (wf >= 0x10u && wf < 0xFEu)
  {
    rt->wfghost = (uint8_t)(sanitize_control(rt, wf) & rt->ptn_gate);
  }
  else if (wf < 0x10u)
  {
    rt->arps_cnt = (int16_t)wf;
  }

  // Read arp and detune columns
  uint8_t arp;
  if (!read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->wft_pos + 1u), &arp)) return;

  uint8_t det;
  if (read_u8(rt->payload, rt->payload_len, (uint16_t)(rt->wft_pos + 2u), &det))
  {
    if (det != 0xFFu) rt->detuner = det;
  }

  rt->wft_pos = (uint8_t)(rt->wft_pos + 3u);

  // Pitch decode (instrument-only: chord call is ignored).
  if (arp == 0x80u || arp == 0x7Fu)
  {
    return;
  }

  int16_t p;
  if (arp < 0x80u)
  {
    p = (int16_t)rt->dpitch_base + (int16_t)arp;
  }
  else if (arp >= 0xE0u)
  {
    p = (int16_t)rt->dpitch_base + (int16_t)(int8_t)arp;
  }
  else
  {
    p = (int16_t)(arp & 0x7Fu);
  }

  if (p < 0) p = 0;
  if (p > 95) p = 95;
  set_pitch_from_table(rt, (uint8_t)p);
}

bool sw_runtime_tick(sw_runtime_t* rt, sw_sid_frame_t* out)
{
  if (!rt || !out || !rt->payload || !rt->active) return false;

  // Ordering mirrors player CNTPLY2.
  tick_vibrato(rt);
  tick_filter(rt);
  tick_pw(rt);
  tick_wfarp(rt);

  // WRPITCH: apply detune to low byte only, carry into high.
  uint16_t base = (uint16_t)((uint16_t)rt->freqh << 8 | rt->freql);
  uint16_t detuned = (uint16_t)(base + (uint16_t)rt->detuner);

  out->freq_reg = detuned;
  out->pulse_reg = (uint16_t)((((uint16_t)rt->pw_hi & 0x0Fu) << 8) | rt->pw_lo);
  out->control = rt->wfghost;
  out->ad = rt->ad;
  out->sr = rt->sr;
  out->filter_cutoff = (uint16_t)(rt->cutoff_11 & 0x07FFu);
  out->fr_vic = (uint8_t)(rt->resonance_hi | (rt->filter_route & 0x0Fu));
  out->mode_vol = (uint8_t)(rt->filter_band | (rt->cfg.volume & 0x0Fu));
  return true;
}
