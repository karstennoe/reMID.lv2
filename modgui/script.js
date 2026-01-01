(function () {
  "use strict";

  // MOD GUI resources are served from <bundle>/modgui/, so bundle-relative paths must use "../".
  const BUNDLE_PREFIX = "../";
  const BANK_INDEX_PATH = BUNDLE_PREFIX + "instruments/banks/banks_index.json";

  function prettyName(v) {
    if (!v) return "N/A";
    if (String(v).toUpperCase().startsWith("DRUMKIT:")) return v;
    const s = String(v).replace(/^.*[\\/]/, "").replace(/\.swi$/i, "");
    return s.replace(/[-_]+/g, " ");
  }

  function parseSwibank(text) {
    const programs = {};
    let section = "";
    text.split(/\r?\n/).forEach((line) => {
      line = line.trim();
      if (!line || line.startsWith("#") || line.startsWith(";")) return;
      if (line.startsWith("[") && line.endsWith("]")) {
        section = line.slice(1, -1).trim().toLowerCase();
        return;
      }
      const eq = line.indexOf("=");
      if (eq < 0) return;
      const key = line.slice(0, eq).trim();
      const val = line.slice(eq + 1).trim();
      if (section === "programs") {
        const p = parseInt(key, 10);
        if (!Number.isFinite(p) || p < 0 || p > 127) return;
        programs[p] = val;
      }
    });
    return programs;
  }

  async function fetchJson(path) {
    const r = await fetch(path, { cache: "no-cache" });
    if (!r.ok) throw new Error(`HTTP ${r.status} for ${path}`);
    return await r.json();
  }

  async function fetchText(path) {
    const r = await fetch(path, { cache: "no-cache" });
    if (!r.ok) throw new Error(`HTTP ${r.status} for ${path}`);
    return await r.text();
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
      const v = programMap[p];
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
        options.push({ label: p.name || p.file, file: p.file });
      });
    }

    // Include drumkit presets listed in the index.
    if (index.drumkit_gm) options.push({ label: "DRUMKIT: GM (example)", file: index.drumkit_gm });
    if (index.drumkit_remid) options.push({ label: "DRUMKIT: reMID (built-in)", file: index.drumkit_remid });

    options.sort((a, b) => a.label.localeCompare(b.label));
    bankSelect.innerHTML = options
      .map((o) => `<option value="${BUNDLE_PREFIX + o.file}">${o.label}</option>`)
      .join("");

    const defaultBank = BUNDLE_PREFIX + "instruments/banks/bank-all-0.swibank";
    const hasDefault = options.some((o) => (BUNDLE_PREFIX + o.file) === defaultBank);
    bankSelect.value = hasDefault ? defaultBank : (options[0] ? (BUNDLE_PREFIX + options[0].file) : "");

    let programMap = {};

    async function loadSelectedBank() {
      const file = bankSelect.value;
      if (!file) return;
      try {
        const text = await fetchText(file);
        programMap = parseSwibank(text);
        updateLabels(programMap);
      } catch (e) {
        for (let ch = 1; ch <= 16; ch++) setChannelLabel(ch, "N/A");
      }
    }

    bankSelect.addEventListener("change", loadSelectedBank);
    document.querySelectorAll(".mod-program").forEach((el) => {
      el.addEventListener("input", () => updateLabels(programMap));
      el.addEventListener("change", () => updateLabels(programMap));
    });

    await loadSelectedBank();
    updateLabels(programMap);
  }

  window.addEventListener("load", () => {
    main().catch(() => {});
  });
})();
