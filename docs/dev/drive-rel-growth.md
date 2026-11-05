# REL allocation and initialization primitives

This dependency slice follows the REL read foundation in #119. It adds pure
allocation preflight, native block-count arithmetic and empty-record page
initialization. It does not allocate sectors or enable public REL creation,
growth, writes, CLOSE or COPY. Production facade routing, the legacy engine
and flat HDD support remain unchanged.

## Included contracts

`dos_rel_growth_plan` uses FNDREL for the requested record and then applies
ADDREL's endpoint rounding and allocation preflight. `dos_rel_growth_plan_at`
accepts an existing endpoint for the DOS 2.7 CYEXT/ADDR1 path. Neither reads
media, reserves blocks, compares free space, changes a channel or publishes
status. `too_large` reports the small-file side bound, not a complete disk
capacity decision. Callers must still compare the retained low-word count
with NUMFRE and own all allocation and failure handling.

`dos_rel_fill` initializes one caller-owned page using NULBUF/ADDNR's zero
payload and $FF record-start markers, returning the retained byte-wide NR.
The caller supplies the previous NR and installs sector links separately.
Byte wrap is intentional: a wrapped incoming seed can place a marker in
link bytes. The helper does not promise link preservation for every seed.
Record sizes must be nonzero. No page, cursor overlay, heap allocation or
file-length cache is added.

`dos_rel_blocks` calculates CLSREL's native low-word count from an SSEND
group/side/index endpoint. The side comes from the six-entry side table.
Malformed endpoint subtraction still wraps at one byte. This helper does
not perform SCRUB, SSEND, directory publication or channel teardown.

## Native evidence

The targets remain 4040 DOS 2.0 revision 2 (`901468-14/-15/-16`) and full-size
8050 MPI single-sided / 8250 MPI double-sided DOS 2.7
(`901887-01/901888-01`). Both pinned master compositions were consulted.
The approved 4040 source overlays do not replace the arithmetic or NULBUF
routines selected here.

| Behavior | Target peripheral/ROM | Primary source and routine | Required observable result | EconoPET code and test | Corroboration |
|----------|-----------------------|----------------------------|----------------------------|------------------------|---------------|
| Empty-record initialization | All selected targets | [4040 nulbuf][4040-nulbuf] / [DOS 2.7 nulbuf][8250-nulbuf], NULBUF/NB10/NB20/NB30/ADDNR/AN05/AN10 | Zero bytes 2..255, place $FF at byte-wide record starts, retain carry/link skipping and BIT ER00's exact-wrap Z result. Size 254 with seed 2 yields one marker at 2 and NR 0. Size 255 with seed 255 wraps a marker to byte 0 | `fill_next/dos_rel_fill`, `test_fill_native_record_markers` checks complete pages for 12 boundary cases | [VICE REL growth][vice] independently fills $FF/$00 records across sectors, but its modulo record counter does not establish native ADC/BIT edge results |
| Endpoint rounding and side bound | All selected targets, super groups only for 8250 | [4040 addrel][4040-addrel] / [DOS 2.7 addrel][8250-addrel], ADDREL/ADDR1/AR10/AR20/AR25, respective fndrel and DOS 2.7 system BIGREL | Round only if RS-1+RELPTR carries. A wrapped pointer advances the side and restarts index 16. Side 6 is too large for 4040/8050, while 8250 carries to the next super group | `dos_rel_growth_plan/_at`, native-preflight and endpoint-span/group-carry tests across all models | VICE independently extends data/side/super-side structures. [MAME 4040][mame-4040] and [8050/8250][mame-8050] corroborate selected ROM/hardware modes, not a corrected capacity formula |
| Native preflight count | Selected small-file modes versus 8250 big REL | Both addrel AR25/AR30, DOS 2.7 ADDBIG/ADDBI1, respective [4040 ssutil][4040-ssutil] / [DOS 2.7 ssutil][8250-ssutil], SSCALC/SSSCAL | Preserve byte SBC borrow and decrement/sign loops, including entry $80 versus $81. A side-boundary extension can report one block rather than a corrected two-block count. Backward endpoints retain native low-word results | `dos_rel_growth_plan_at`, preflight and borrow/signed-loop tests | VICE uses a different sector-at-a-time capacity policy and therefore does not prove native undercounts. Both selected primary listings control small-file arithmetic, with the separate DOS 2.7 big-file branch |
| Existing endpoint input | DOS 2.7 CYEXT | [DOS 2.7 rename][8250-rename], CYEXT/CYEXT1/CYEXT2, addrel ADRELS/ADDR1 | Restore source SSEND side/index/group after preparing the target, use RELPTR zero and enter ADDR1 without inventing a record number | `dos_rel_growth_plan_at`, explicit endpoint span tests | VICE's high-level growth path does not implement this entry convention. MAME confirms the selected full-size shared-ROM models, not execution of this path |
| Native directory block count | All selected targets | [4040 close][4040-close] / [DOS 2.7 close][8250-close], CLSREL/CLSBIG, respective ssutil SSCALC/SSSCAL/ADDLIT | Count data and side pages, plus super page for 8250. Preserve byte subtraction and low-word wrap. Group 90/side 5/index 254 yields 531 in the 8250 low word | `dos_rel_blocks`, endpoint/low-word tests across all models | VICE independently includes data/side/super pages in directory counts, but derives them from cached file length rather than native SSEND byte arithmetic |

These are source-derived helper contracts. Independent implementations
corroborate layout and model selection, not every malformed-input or signed
arithmetic edge. No ROM binaries or disassembly are included.

## Validation and remaining work

The isolated candidate passes 312 focused REL cases (36 test groups) and
4729 total firmware cases (4682 ordinary plus 47 fatal/assert). Tests and
dependencies use Debug. The RP2040 production RelWithDebInfo build,
clang-format and whitespace checks pass. All 20 existing read helpers and
31 read test groups retain their executable tokens and registrations,
including final merged #119's selected-side SSTEST retest and geometry handoff.

Stateful reservation/publication, BAM ownership, new side/super pages,
creation/growth continuation, partial-write retry, public channel integration,
LISTEN/write completion, CLOSE and REL COPY remain separate. Existing
read/status/FORMAT behavior is unchanged. No fresh full-ROM controller
comparison, physical acceptance, timing or electrical claim is made.

[4040-nulbuf]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/nulbuf
[8250-nulbuf]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/nulbuf
[4040-addrel]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addrel
[8250-addrel]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addrel
[4040-ssutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssutil
[8250-ssutil]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssutil
[4040-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
[8250-close]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
[8250-rename]: https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
[vice]: https://github.com/VICE-Team/svn-mirror/blob/a0e14df25846cd7f06043f24b48677bf45c02149/vice/src/vdrive/vdrive-rel.c#L310-L630
[mame-4040]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c2040.cpp
[mame-8050]: https://github.com/mamedev/mame/blob/f379cd060ac3b2bd567ebbd0957aed059d6895ff/src/devices/bus/ieee488/c8050.cpp
