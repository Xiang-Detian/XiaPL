# PHH test fixtures

## Provenance

`dwan-ivey-2009.phh`, `antonius-blom-2009.phh`, and `alice-carol-wikipedia.phh`
are transcribed verbatim from the
[uoftcprg/phh-dataset](https://github.com/uoftcprg/phh-dataset) repository
(fetched 2026-08-10 from `raw.githubusercontent.com/uoftcprg/phh-dataset/main/data/`).

That dataset is distributed under the MIT License, © 2024-2025 University of
Toronto Computer Poker Research Group.

All other fixtures under this directory (including everything in `bad/`) are
hand-written for `xiapl` and are not derived from `phh-dataset`.

## `*.expected.json` golden convention

Some fixtures are paired with a `<name>.expected.json` file holding the
golden, machine-readable rendering of `parse_phh(<name>.phh)` (or, for a
`.phhs` collection, `parse_phh_all(<name>.phhs)`) as a JSON list of hand
dicts -- added in Task 2, consumed by `test_goldens_frozen` in
`python/test/test_phh.py`. Each `.expected.json` sits next to its `.phh` /
`.phhs` source and shares its basename. Regenerate with
`python python/test/gen_phh_golden.py`, human-review the diff (especially
`cards_text`), then commit -- from that point on the golden files are a
frozen regression baseline, not a live truth source.
