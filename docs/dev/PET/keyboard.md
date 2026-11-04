# Keyboard

## Pinout

```text
  .-------- KIN0-KIN7 --------.   .---------- KSCAN0-KSCAN9 ----------.      GND
  |                           |   |                                   |       |
  A   B   C   D   E   F   H   J   1   2   3   4   5   6   7   8   9  10      12
.-------------------------------------------------------------------------------.
| o   o   o   o   o   o   o   o   o   o   o   o   o   o   o   o   o   o       o |
'-------------------------------------------------------------------------------'
```

Note that Commodore skips pin lables G and I (presumably to avoid confusion with
6 and 1).  The keyboard connector is keyed, hence there is no pin 11.

To scan the keyboard, the PET's [74LS145](https://www.ti.com/lit/gpn/SN74145)
([UC11](https://www.zimmers.net/anonftp/pub/cbm/schematics/computers/pet/8032/8032029-03.gif))
decoder pulls one of KSCAN0-KSCAN9 low at a time. Its open-collector outputs
sink current when selected and are high-impedance when inactive (they do not
drive high). Pull-up resistors hold KIN0-KIN7 high until a pressed key connects
an input to the selected scan line, pulling that input low. The selected KSCAN
line and the low KIN bit identify the pressed key's matrix position.

| Pins | Signals | Purpose |
| ---- | ------- | ------- |
| A-J | KIN0-KIN7 | Keyboard sense inputs (PIA1 port B), pulled high when inactive. |
| 1-10 | KSCAN0-KSCAN9 | Scan outputs, selected one at a time by driving low. |
| 11 (missing) | Key | Prevents reversed connector insertion. |
| 12 | GND | Ground reference. |

## Graphics Matrix

| Signal   | KSCAN0   | KSCAN1   | KSCAN2 | KSCAN3 | KSCAN4 | KSCAN5 | KSCAN6 | KSCAN7 | KSCAN8 | KSCAN9    | Pin   |
| -------- | -------- | -------- | ------ | ------ | ------ | ------ | ------ | ------ | ------ | --------- | ----- |
| **KIN0** | !        | "        | Q      | W      | A      | S      | Z      | X      | LSHIFT[^shift-lock] | OFF / RVS | **A** |
| **KIN1** | #        | $        | E      | R      | D      | F      | C      | V      | @      | [         | **B** |
| **KIN2** | %        | '        | T      | Y      | G      | H      | B      | N      | ]      | SPACE     | **C** |
| **KIN3** | &        | \\       | U      | I      | J      | K      | M      | ,      | *NONE* | <         | **D** |
| **KIN4** | (        | )        | O      | P      | L      | :      | ;      | ?      | >      | RUN STOP  | **E** |
| **KIN5** | ←        | *TAB*[^unwired] | ↑      | *NONE* | *NONE* | *NONE* | RETURN | *NONE* | RSHIFT | *REPEAT*[^unwired] | **F** |
| **KIN6** | CLR HOME | CRSR U/D | 7      | 8      | 4      | 5      | 1      | 2      | 0      | .         | **H** |
| **KIN7** | CRSR L/R | INST DEL | 9      | /      | 6      | *      | 3      | +      | -      | =         | **J** |
| **Pin**  | **1**    | **2**    | **3**  | **4**  | **5**  | **6**  | **7**  | **8**  | **9**  | **10**    |       |

Italic entries mark unwired positions. *NONE* means the ROM scanmap assigns
no key (`$FF`).

## Business Matrix

| Signal   | KSCAN0   | KSCAN1   | KSCAN2   | KSCAN3   | KSCAN4   | KSCAN5   | KSCAN6     | KSCAN7   | KSCAN8   | KSCAN9   | Pin   |
| -------- | -------- | -------- | -------- | -------- | -------- | -------- | ---------- | -------- | -------- | -------- | ----- |
| **KIN0** | 2        | 1        | ESC      | A        | TAB      | Q        | LSHIFT[^shift-lock] | Z        | RVS      | ←        | **A** |
| **KIN1** | 5        | 4        | S        | D        | W        | E        | C          | V        | X        | 3        | **B** |
| **KIN2** | 8        | 7        | F        | G        | R        | T        | B          | N        | SPACE    | 6        | **C** |
| **KIN3** | -        | 0        | H        | J        | Y        | U        | .          | ,        | M        | 9        | **D** |
| **KIN4** | 8 (KPAD) | 7 (KPAD) | ]        | RETURN   | \\       | CRSR U/D | . (KPAD)   | 0 (KPAD) | CLR HOME | RUN STOP | **E** |
| **KIN5** | CRSR L/R | ↑        | K        | L        | I        | O        | *CTRL-Y*[^unwired] | *CTRL-O*[^unwired] | *CTRL-U*[^unwired] | :        | **F** |
| **KIN6** | *CTRL-N*[^unwired] | *CTRL-F*[^unwired] | ;        | @        | P        | [        | RSHIFT     | REPEAT   | /        | *CTRL-D*[^unwired] | **H** |
| **KIN7** | *CTRL-E*[^unwired] | 9 (KPAD) | 5 (KPAD) | 6 (KPAD) | INST DEL | 4 (KPAD) | 3 (KPAD)   | 2 (KPAD) | 1 (KPAD) | *CTRL-V*[^unwired] | **J** |
| **Pin**  | **1**    | **2**    | **3**    | **4**    | **5**    | **6**    | **7**      | **8**    | **9**    | **10**   |       |

[^unwired]: This position has an assigned ROM code but no physical key switch.

[^shift-lock]: Engaging SHIFT-LOCK holds the LSHIFT matrix position closed.

## Missing Keys and Matrix Ghosting

Both business and graphics scanmaps contain unwired positions. Some are
reachable through matrix ghosting (pressing three corners of a rectangle
makes the fourth appear pressed). This may compensate for some keys the
business keyboard has but the graphics keyboard lacks.

The sequences below follow the wiring and ROM scanners, but have not been
verified on physical keyboards. Follow the stated press order and then release
the keys in the opposite order.

### Unwired Graphics Positions

The graphics scanmap assigns TAB and REPEAT to unwired positions. Release
the tapped key before the Shifts. TAB behavior depends on the editor ROM
(the BASIC 4 CRTC editors support tab stops).

| Position | Table code | Unshifted function | Shifted function | Chord |
| -------- | ---------- | ------------------ | ---------------- | -------------------------------- |
| KSCAN1/KIN5 | `$09` (TAB) | Advance to next tab stop | `$89`: set/clear tab stop | LSHIFT + RSHIFT, `"` (returns `$89`) |
| KSCAN9/KIN5 | `$10` (REPEAT) | Enable repeat | Enable repeat (modifier, no character code) | LSHIFT + RSHIFT, RVS[^repeat-rvs], then key to repeat |

[^repeat-rvs]: The scanner also returns shifted RVS (`$92`), turning reverse
    video off in normal editor mode. Adding another key can change the ghost
    and scanner result, so this is not a verified general-purpose REPEAT substitute.

There is no ESC entry, so ghosting cannot produce ESC. Steve Gray's [editor ROM
tables](https://github.com/sjgray/cbm-edit-rom/blob/master/keyboard-tables1.asm)
instead offer remappings: `KEYBOARD=5` replaces `@` with ESC and moves `@` to
back-arrow, while `KEYBOARD=9` replaces back-arrow with ESC.

### Unwired Business Positions

The standard QWERTY business matrix has eight unwired positions. `CTRL-*`
labels identify ROM codes, not physical keys or a CTRL modifier. Functions
below apply to the stock 80-column Commodore editor outside quote and insert
modes, not Waterloo or custom editors.

These sequences mask ordinary keys, allowing only reverse-off (`$92`) and
editor-ignored codes as side effects. Numbers refer to the main keyboard
unless marked keypad.

| Position | Table code | Unshifted function | Shifted function | Sequence |
| -------- | ---------- | ------------------ | ---------------- | ------------- |
| KSCAN0/KIN6 | `$0E` (CTRL-N) | Select text character mode | `$8E`: select graphics character mode | LSHIFT + RSHIFT, `2` (`$8E`) |
| KSCAN0/KIN7 | `$05` (CTRL-E) | No editor action | `$85`: no editor action | None found |
| KSCAN1/KIN6 | `$06` (CTRL-F) | No editor action | `$86`: no editor action | LSHIFT + RSHIFT, `1` (`$86`) |
| KSCAN6/KIN5 | `$19` (CTRL-Y) | Scroll window up | `$99`: scroll window down | LSHIFT + RSHIFT, `1`, Up-arrow. For `$19`, see the guard sequence below. |
| KSCAN7/KIN5 | `$0F` (CTRL-O) | Set window top-left to cursor | `$8F`: set window bottom-right to cursor | Use the window-bottom sequence below (`$8F`), or the guard sequence (`$0F`). |
| KSCAN8/KIN5 | `$15` (CTRL-U) | Delete current line | `$95`: insert line | RSHIFT, RVS[^repeat-rvs], `A`. `L` (`$95`). For `$15`, see below. |
| KSCAN9/KIN6 | `$04` (CTRL-D) | No editor action | `$84`: no editor action | LSHIFT + RSHIFT, Back-arrow (`$84`) |
| KSCAN9/KIN7 | `$16` (CTRL-V) | Erase to end of line | `$96`: erase to start of line | LSHIFT + RSHIFT, Back-arrow, keypad `3` (`$96`). For `$16`, see the guard sequence below. |

Scroll-down also emits ignored `$86`, and erase-to-start emits ignored `$84`.

#### Window Bottom

Hold both Shifts and Back-arrow (which produces ignored `$84`). Add `Z`,
`A`, then `L`, keeping them held. Release Back-arrow to produce `$8F`.
Press Back-arrow again to mask the other keys, then release `L`, `A`, `Z`,
Back-arrow, and finally the Shifts. The only other output is ignored `$84`.

#### Delete Line

Hold RSHIFT and RVS (reverse off), then release RSHIFT while keeping RVS
held. Hold `A` and press `L` to produce `$15`. Hold RSHIFT again before
releasing `L`, `A`, RVS, and finally RSHIFT. Releasing or restoring RSHIFT
alone does not enqueue a new character while the same raw key remains
selected. The only other output is reverse-off (`$92`).

#### Unshifted Controls Using an Ignored-Code Guard

These longer sequences use the ignored CTRL-D entry to mask ordinary
characters during setup and teardown:

1. Hold both Shifts, Back-arrow, ESC, then `;`. Release both Shifts, keeping
   Back-arrow, ESC, and `;` held. This setup emits only ignored `$84`.
2. Perform one of these actions:
   - **Scroll up (`$19`):** Hold `C`, `5`, then CRSR L/R. Release Back-arrow
     to produce `$19`, then press Back-arrow again. Release CRSR L/R, `5`,
     then `C`.
   - **Set window top-left (`$0F`):** Hold `Z`, `A`, then `L`. Release
     Back-arrow to produce `$0F`, then press Back-arrow again. Release `L`,
     `A`, then `Z`.
   - **Erase to end of line (`$16`):** Hold `3`, `4`, then keypad `9` to
     produce `$16`. Release keypad `9`, `4`, then `3`, without releasing
     Back-arrow.
3. Hold both Shifts again. Release `;`, ESC, Back-arrow, then the Shifts.

Apart from the requested action, these sequences emit only ignored `$84`
and `$04`. They can require six keys held at once.

The
[business scanner](https://github.com/sjgray/cbm-edit-rom/blob/master/keyscan-b.asm)
scans KSCAN0-KSCAN9, then KIN0-KIN7 within each column. The last non-modifier
entry wins, with Shift and REPEAT handled separately. Both Shifts do not
generally suppress typing, so changing the release order can expose unwanted
characters. These sequences account for scanner priority and raw-key change
detection at each settled state, without requiring presses between scans.

## Reference

* [PET Keyboard (Masswerk)](https://masswerk.at/nowgobang/2023/pet-keys-2001-edition)
* [Graphics Keyboard Matrix](https://archive.org/details/commodore-cbm-3016-keyboard-matrix)
* [Business Keyboard Matrix](https://zimmers.net/anonftp/pub/cbm/schematics/computers/pet/2001N/PETkeyboardMatrix.gif)
* [Business Keyboard Schematic](https://www.zimmers.net/anonftp/pub/cbm/schematics/computers/pet/8032/320284.gif)
* [PetIO.doc](http://www.6502.org/users/andre/petindex/local/pet-io-2.txt)
* [Steve Gray's PET Keyboard Replacements](http://www.cbmsteve.ca/petkeyboard/)
* [Steve Gray's Editor ROM Keyboard Tables](https://github.com/sjgray/cbm-edit-rom/blob/master/keyboard-tables1.asm)
* Steve Gray's Keyboard Scanners:
  [Graphics](https://github.com/sjgray/cbm-edit-rom/blob/master/keyscan-g.asm),
  [Business](https://github.com/sjgray/cbm-edit-rom/blob/master/keyscan-b.asm)
