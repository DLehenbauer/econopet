# Keyboard

## Pinout

```
  A   B   C   D   E   F   H   J   1   2   3   4   5   6   7   8   9   10     12
+-------------------------------------------------------------------------------+
| o   o   o   o   o   o   o   o   o   o   o   o   o   o   o   o   o   o       o |
+-------------------------------------------------------------------------------+
```

The keyboard connector is keyed, hence there is no pin 11.

## Graphics Matrix

| Signal   | KSCAN0   | KSCAN1   | KSCAN2 | KSCAN3 | KSCAN4 | KSCAN5 | KSCAN6 | KSCAN7 | KSCAN8 | KSCAN9    | Pin   |
| -------- | -------- | -------- | ------ | ------ | ------ | ------ | ------ | ------ | ------ | --------- | ----- |
| **KIN0** | !        | "        | Q      | W      | A      | S      | Z      | X      | LSHIFT | OFF / RVS | **A** |
| **KIN1** | #        | $        | E      | R      | D      | F      | C      | V      | @      | [         | **B** |
| **KIN2** | %        | '        | T      | Y      | G      | H      | B      | N      | ]      | SPACE     | **C** |
| **KIN3** | &        | \\       | U      | I      | J      | K      | M      | ,      |        | <         | **D** |
| **KIN4** | (        | )        | O      | P      | L      | :      | ;      | ?      | >      | RUN STOP  | **E** |
| **KIN5** | ←        |          | ↑      |        |        |        | RETURN |        | RSHIFT |           | **F** |
| **KIN6** | CLR HOME | CRSR U/D | 7      | 8      | 4      | 5      | 1      | 2      | 0      | .         | **H** |
| **KIN7** | CRSR L/R | INST DEL | 9      | /      | 6      | *      | 3      | +      | -      | =         | **J** |
| **Pin**  | **1**    | **2**    | **3**  | **4**  | **5**  | **6**  | **7**  | **8**  | **9**  | **10**    |       |

## Business Matrix

| Signal   | KSCAN0   | KSCAN1   | KSCAN2   | KSCAN3   | KSCAN4   | KSCAN5   | KSCAN6     | KSCAN7   | KSCAN8   | KSCAN9   | Pin   |
| -------- | -------- | -------- | -------- | -------- | -------- | -------- | ---------- | -------- | -------- | -------- | ----- |
| **KIN0** | 2        | 1        | ESC      | A        | TAB      | Q        | SHIFT LOCK | Z        | RVS      | ←        | **A** |
| **KIN1** | 5        | 4        | S        | D        | W        | E        | C          | V        | X        | 3        | **B** |
| **KIN2** | 8        | 7        | F        | G        | R        | T        | B          | N        | SPACE    | 6        | **C** |
| **KIN3** | -        | 0        | H        | J        | Y        | U        | .          | ,        | M        | 9        | **D** |
| **KIN4** | 8 (KPAD) | 7 (KPAD) | ]        | RETURN   | \\       | CRSR U/D | . (KPAD)   | 0 (KPAD) | CLR HOME | RUN STOP | **E** |
| **KIN5** | CRSR L/R | ↑        | K        | L        | I        | O        | CTRL-Y     | CTRL-O   | CTRL-U   | :        | **F** |
| **KIN6** | CTRL-N   | CTRL-F   | ;        | @        | P        | [        | SHIFT      | REPEAT   | /        | CTRL-D   | **H** |
| **KIN7** | CTRL-E   | 9 (KPAD) | 5 (KPAD) | 6 (KPAD) | INST DEL | 4 (KPAD) | 3 (KPAD)   | 2 (KPAD) | 1 (KPAD) | CTRL-V   | **J** |
| **Pin**  | **1**    | **2**    | **3**    | **4**    | **5**    | **6**    | **7**      | **8**    | **9**    | **10**   |       |

Reference

* [PET Keyboard (Masswerk)](https://masswerk.at/nowgobang/2023/pet-keys-2001-edition)
* [Graphics Keyboard Matrix](https://archive.org/details/commodore-cbm-3016-keyboard-matrix)
* [Business Keyboard Matrix](https://zimmers.net/anonftp/pub/cbm/schematics/computers/pet/2001N/PETkeyboardMatrix.gif)
* [Business Keyboard Schematic](https://www.zimmers.net/anonftp/pub/cbm/schematics/computers/pet/8032/320284.gif)
* [PetIO.doc](http://www.6502.org/users/andre/petindex/local/pet-io-2.txt)
