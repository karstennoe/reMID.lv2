# Converter tools (direct `.swi` playback branch)

This branch of `reMID.lv2` plays **SID-Wizard `.swi`** instruments directly at runtime.

The primary “conversion” task is bank generation:

- `converter/build_banks.py`: generates `.swibank` files under `instruments/banks/`
- `converter/bank_rules.json`: category/bank rules (lead/bass/etc) and paging

## Bank generation

From the repo root:

- `python converter/build_banks.py`

Outputs:

- `instruments/banks/bank-<category>-<page>.swibank`
- `instruments/banks/bank-all-<page>.swibank`
- `instruments/banks/banks_index.json`
- `instruments/banks/drumkit-gm.swibank` (example note→instrument mapping)

`.swibank` files reference `.swi` files using `base=...` (usually `../swi`), and the install step places raw `.swi` exports into the LV2 bundle under `instruments/swi/`.

## Legacy (old `.conf` workflow)

The repo may still contain `swi2remid.py` / `convert_all.py` for the older reMID `.conf` “instrument language” workflow, but the LV2 plugin does not use `.conf` instruments on this branch.

