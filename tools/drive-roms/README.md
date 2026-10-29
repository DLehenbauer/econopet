# Drive ROM source reconstruction and verification

These tools reconstruct 6502 assembly source for the approved Commodore
4040 revision-2 and full-size 8050/8250 DOS 2.7 ROMs, using the available
`mist64/cbmsrc` source and original ROM binaries. They assemble that source
and compare the resulting bytes against the original binaries.

Where archived source is available, the tools translate its assembler syntax
and apply explicit, manifest-recorded source adaptations where needed.
Selected controller and embedded payloads are reconstructed by disassembling
the original binaries with declared code/data boundaries and entry labels.

Verification has two scopes: source-based checks compare emitted ROM bytes
except declared checksum placeholders and report uncovered regions. Binary
payload reconstructions must reassemble to exact byte-for-byte equality across
the entire selected payload. Passing the checks does not mean every rebuilt
source image is identical to an entire original ROM.

## Run the complete checks

Install Bash, Git, curl, Python 3, ACME and the CC65 tools (`da65`, `ca65`,
`ld65`), then run:

```sh
tools/drive-roms/verify.sh
```

Paths are relative to the repository, not the caller's working directory:

| Path | Contents |
| --- | --- |
| `.tmp/ai/cbmsrc/` | Sparse clone at the source commit pinned in the manifest |
| `.tmp/ai/drive-roms/download/` | Downloaded chip binaries, checked for size and SHA-256 |
| `.tmp/ai/drive-roms/out/` | All generated artifacts (reconstructed source, assembled ROMs, reports, logs and `summary.json`) |

The script accesses GitHub and the ROM URLs in
[the manifest](drive-emulation-roms.json). It reuses only a clean source
checkout at the pinned origin/commit and hash-verified ROM downloads.
The output directory must not exist (move prior results aside to rerun).
All `.tmp/ai/` contents are ignored.

Each check has its own output subdirectory. Assembled images are retained as
`source.prg` (with the CBM load-address header) and `source-fill.prg` for
source comparisons, or `rebuilt.bin` for reconstructed binary payloads.

The Python runner performs eight checks: two original-source comparisons,
two explicit source mappings and four classified payload reconstructions.
The original 4040 source must produce the recorded revision mismatch,
including exact counts. An arbitrary failure is not an accepted negative control.

Require `8/8 checks passed` and exit code 0.

## Individual tools and tests

For pre-acquired external inputs, invoke the Python runner directly:

```sh
python3 tools/drive-roms/verify.py \
  --source-root /path/to/cbmsrc \
  --rom-dir /path/to/drive-roms \
  --output-dir /path/to/new-private-output
```

`drive_rom_verify.py` rebuilds unmodified pinned source. `drive_rom_map.py`
applies declared source overlays or reconstructs classified binary payloads.
Both accept explicit external inputs and support `--help`.

The focused tests use synthetic inputs, not proprietary ROMs:

```sh
python3 -m unittest discover -s tools/drive-roms -p 'test_*.py' -v
```
