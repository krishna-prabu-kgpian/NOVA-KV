# Provenance of the files in `supplied/`

| File | Origin | Status |
|---|---|---|
| `workload_gen.cpp` | Supplied by the instructor alongside `assignment2.pdf`. Imported at the repository root in commit `a3cccd8` and moved here with `git mv` (no content change). | **Byte-for-byte original.** SHA-256 `58c3c2c8f5203bffc20198515c9065d33dd46fd8cfc89fd83e610eda3ad08508`, 150 newline-terminated lines. Must never be edited. |
| `workload_gen.h` | **Not supplied as a standalone file.** Transcribed by hand from the listing in `assignment2.pdf`, Appendix A.2 (page 10). | Token-for-token transcription of the printed listing. Intra-line whitespace/alignment cannot be recovered from a PDF and is therefore not claimed to be identical. No field, default, or declaration was added, removed, or renamed. Replace with the instructor's original, unchanged, if one becomes available. |

`SHA256SUMS` pins both files. `scripts/verify_all.sh` runs `sha256sum -c SHA256SUMS`
on every verification pass, so any modification of either file fails the gate.

The provenance note lives in this file rather than in a comment inside
`workload_gen.h` so that the header stays a pure transcription.
