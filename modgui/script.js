(function () {
  "use strict";

  if (window.__remid_modgui_loaded) return;
  window.__remid_modgui_loaded = true;

  // MOD only serves files under modgui:resourcesDirectory, exposed via /resources/.
  // The {{{ns}}} tag ensures the correct per-plugin query string is used across hosts.
  const BANK_INDEX_CANDIDATES = [
    // Recommended in MOD docs:
    "/resources/banks_index.json{{{ns}}}",
    // Some setups resolve relative resources better:
    "banks_index.json{{{ns}}}",
    // Fallbacks if templating isn't applied to JS:
    "/resources/banks_index.json",
    "banks_index.json",
  ];

  function $(id) {
    return document.getElementById(id);
  }

  function setStatus(text) {
    const el = $("remid-status");
    if (el) el.textContent = text;
  }

  function normalizePath(p) {
    return String(p || "").replace(/\\/g, "/");
  }

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

  function getProgramOverrideValueFromMirror(ch) {
    const el = $(`remid-ch${ch}-val`);
    if (!el) return null;
    const v = parseInt(String(el.textContent || "").trim(), 10);
    if (!Number.isFinite(v)) return null;
    return Math.max(0, Math.min(128, v));
  }

  function getProgramOverrideValue(ch) {
    const mv = getProgramOverrideValueFromMirror(ch);
    if (mv !== null) return mv;
    const input = document.querySelector(`.remid-program[mod-port-symbol="ch${ch}_program"]`);
    if (!input) return 0;
    const v = parseInt(String(input.value || "").trim(), 10);
    if (!Number.isFinite(v)) return 0;
    return Math.max(0, Math.min(128, v));
  }

  function setChannelLabel(ch, text) {
    const el = $(`remid-ch${ch}-label`);
    if (el) el.textContent = text;
  }

  function labelForOverride(programMap, ov) {
    if (!Number.isFinite(ov) || ov <= 0) return "MIDI Program Change";
    const p = ov - 1;
    const v = programMap[String(p)] ?? programMap[p];
    return prettyName(v);
  }

  async function main() {
    const bankSelect = $("remid-bank-select");
    if (!bankSelect) return;

    let index;
    try {
      setStatus("Loading banks...");
      let lastErr = null;
      for (const url of BANK_INDEX_CANDIDATES) {
        try {
          index = await fetchJson(url);
          setStatus(`Loaded banks_index.json from ${url}`);
          break;
        } catch (e) {
          lastErr = e;
        }
      }
      if (!index) throw lastErr || new Error("banks_index.json load failed");
    } catch (e) {
      bankSelect.innerHTML = `<option value="">(banks_index.json missing)</option>`;
      setStatus(`Failed to load banks_index.json: ${String(e && e.message ? e.message : e)}`);
      return;
    }

    const options = [];
    for (const [bankId, pages] of Object.entries(index.banks || {})) {
      (pages || []).forEach((p) => {
        const file = normalizePath(p.file);
        options.push({
          label: p.name || file || bankId,
          key: file,
          programs: p.programs || {},
        });
      });
    }

    // Include drumkit presets listed in the index (file paths only; program mapping isn't needed).
    if (index.drumkit_gm) {
      options.push({
        label: "DRUMKIT: GM (example)",
        key: normalizePath(index.drumkit_gm),
        programs: { 0: "DRUMKIT:gm" },
      });
    }
    if (index.drumkit_remid) {
      options.push({
        label: "DRUMKIT: reMID (built-in)",
        key: normalizePath(index.drumkit_remid),
        programs: { 0: "DRUMKIT:remid" },
      });
    }

    options.sort((a, b) => a.label.localeCompare(b.label));
    bankSelect.innerHTML = options.map((o) => `<option value="${o.key}">${o.label}</option>`).join("");
    if (!options.length) {
      bankSelect.innerHTML = `<option value="">(no banks in index)</option>`;
      setStatus("banks_index.json loaded but contained zero banks");
      return;
    }

    const defaultKey = "instruments/banks/bank-all-0.swibank";
    const hasDefault = options.some((o) => o.key === defaultKey);
    bankSelect.value = hasDefault ? defaultKey : (options[0] ? options[0].key : "");

    let programMap = {};

    function loadSelectedBank() {
      const key = bankSelect.value;
      const found = options.find((o) => o.key === key);
      programMap = found ? (found.programs || {}) : {};
      for (let ch = 1; ch <= 16; ch++) {
        setChannelLabel(ch, labelForOverride(programMap, getProgramOverrideValue(ch)));
      }
    }

    function hookChannel(ch) {
      const mirror = $(`remid-ch${ch}-val`);
      const input = document.querySelector(`.remid-program[mod-port-symbol="ch${ch}_program"]`);

      const update = () => {
        setChannelLabel(ch, labelForOverride(programMap, getProgramOverrideValue(ch)));
      };

      update();
      if (mirror) {
        try {
          new MutationObserver(update).observe(mirror, { childList: true, characterData: true, subtree: true });
        } catch (_) {
          // Ignore; MOD's webview should support MutationObserver, but don't crash if it doesn't.
        }
      }
      if (input) {
        input.addEventListener("input", update);
        input.addEventListener("change", update);
      }
    }

    for (let ch = 1; ch <= 16; ch++) hookChannel(ch);
    bankSelect.addEventListener("change", loadSelectedBank);

    loadSelectedBank();
    setStatus(`Banks loaded (${options.length} UI entries)`);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => {
      setStatus("JS loaded, starting...");
      main().catch((e) => setStatus(`JS error: ${String(e && e.message ? e.message : e)}`));
    });
  } else {
    setStatus("JS loaded, starting...");
    main().catch((e) => setStatus(`JS error: ${String(e && e.message ? e.message : e)}`));
  }
})();
