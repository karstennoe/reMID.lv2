(function () {
  "use strict";

  // MOD only serves files under modgui:resourcesDirectory. We ship a generated copy of
  // the bank index into modgui/banks_index.json at build/install time.
  const BANK_INDEX_PATH = "banks_index.json";

  function prettyName(v) {
    if (!v) return "N/A";
    if (String(v).toUpperCase().startsWith("DRUMKIT:")) return v;
    const s = String(v).replace(/^.*[\\/]/, "").replace(/\.swi$/i, "");
    return s.replace(/[-_]+/g, " ");
  }

  async function fetchJson(path) {
    const r = await fetch(path, { cache: "no-cache" });
    if (!r.ok) throw new Error(`HTTP ${r.status} for ${path}`);
    return await r.json();
  }

  function getProgramOverrideValue(ch) {
    const input = document.querySelector(`.mod-program[mod-port-symbol="ch${ch}_program"]`);
    if (!input) return 0;
    const v = parseInt(input.value, 10);
    if (!Number.isFinite(v)) return 0;
    return Math.max(0, Math.min(128, v));
  }

  function setChannelLabel(ch, text) {
    const el = document.querySelector(`.mod-inst[data-ch="${ch}"]`);
    if (el) el.textContent = text;
  }

  function updateLabels(programMap) {
    for (let ch = 1; ch <= 16; ch++) {
      const ov = getProgramOverrideValue(ch);
      if (ov <= 0) {
        setChannelLabel(ch, "MIDI Program Change");
        continue;
      }
      const p = ov - 1;
      const v = programMap[String(p)] ?? programMap[p];
      setChannelLabel(ch, prettyName(v));
    }
  }

  async function main() {
    const bankSelect = document.getElementById("remid-bank-select");
    if (!bankSelect) return;

    let index;
    try {
      index = await fetchJson(BANK_INDEX_PATH);
    } catch (e) {
      bankSelect.innerHTML = `<option value="">missing banks_index.json</option>`;
      return;
    }

    const options = [];
    for (const [bankId, pages] of Object.entries(index.banks || {})) {
      (pages || []).forEach((p) => {
        options.push({ label: p.name || p.file, key: p.file, programs: p.programs || {} });
      });
    }

    // Include drumkit presets listed in the index (file paths only; program mapping isn't needed).
    if (index.drumkit_gm) options.push({ label: "DRUMKIT: GM (example)", key: index.drumkit_gm, programs: { "0": "DRUMKIT:gm" } });
    if (index.drumkit_remid) options.push({ label: "DRUMKIT: reMID (built-in)", key: index.drumkit_remid, programs: { "0": "DRUMKIT:remid" } });

    options.sort((a, b) => a.label.localeCompare(b.label));
    bankSelect.innerHTML = options.map((o) => `<option value="${o.key}">${o.label}</option>`).join("");

    const defaultKey = "instruments/banks/bank-all-0.swibank";
    const hasDefault = options.some((o) => o.key === defaultKey);
    bankSelect.value = hasDefault ? defaultKey : (options[0] ? options[0].key : "");

    let programMap = {};

    function loadSelectedBank() {
      const key = bankSelect.value;
      const found = options.find((o) => o.key === key);
      programMap = found ? (found.programs || {}) : {};
      updateLabels(programMap);
    }

    bankSelect.addEventListener("change", loadSelectedBank);
    document.querySelectorAll(".mod-program").forEach((el) => {
      el.addEventListener("input", () => updateLabels(programMap));
      el.addEventListener("change", () => updateLabels(programMap));
    });

    loadSelectedBank();
    updateLabels(programMap);
  }

  window.addEventListener("load", () => {
    main().catch(() => {});
  });
})();
