# Assembling the board

This page covers through-hole assembly: a recommended order, orientation notes, header splitting/keying, and a final inspection checklist.

If you ordered your boards with SMT assembly from JLCPCB, the surface-mount parts should already be installed. The remaining work is installing any through-hole connectors, sockets, and other optional breakouts.

## Recommended assembly order

The back of the PCB includes a suggested assembly order, generally sorted by component height (shortest first). The exact order is not critical.

It is fine to skip optional components (for example, header breakouts) and add them later.

> **WARNING (rev 40/8096-A silkscreen):** Steps 22-23 on the silkscreen incorrectly list "8x2 Header (2)", implying two 8x2 pin headers are needed. In reality, one of these is the shrouded SWD/JTAG connector (part #61201621621), not a generic pin header.
>
> Do not cut an extra 8x2 header from your 25x2 or 20x2 headers, or you may end up short.

## Component orientation

Most through-hole parts are symmetrical or keyed, but pay attention to:

- ICs and IC sockets: align the notch with the silkscreen (notch toward the top of the board).
- DIP switch: orient so the "ON" side faces toward the FPGA, matching the silkscreen.
- Polarized capacitor (C89): the longer lead is positive (+). Match the PCB '+' marking. C89 has multiple hole patterns.  Use the closest pair of holes to match the capacitor lead spacing.
- Shrouded SWD/JTAG connector: the connector is keyed. Orient the notch toward the HDMI connector, matching the silkscreen.

## Header splitting and keying

Many connectors are supplied as break-away headers that must be cut to length.

- Cut between pins with side cutters or a razor saw.
- It is easy to cut the wrong length the first time.  Consider ordering a few spares.

### 1x36 header (PET keyboard/video)

This header is typically keyed by removing specific pins.

```txt

               cut                                       cut
      Video     v                Keyboard                 v
|------ 7 ------|------------------ 20 -------------------|-------- 9 --------|
| O _ O O O O O | O _ O O O O O O O O O O O O O O O O O O | O O O O O O O O O |

    ^               ^
 remove          remove
  (key)           (key)
```

### 25x2 headers (bus/user/MCU/5V/menu)

There are two 25x2 headers in the BOM. One is used uncut for the bus expansion breakout. The other is divided as shown below.

```txt
                     cut                     cut     cut
           MCU        v          User         v  5V   v Menu
|--------- 10 --------|---------- 11 ---------|-- 3 --| 1 |
| O O O O O O O O O O | O O O O O O O O O O O | O O O | O |
| O O O O O O O O O O | O O O O O O O O O O O | O O O | O |
```

### 20x2 header (IEEE/KeyEx)

```txt
                 cut                 cut
       KeyEx      v       IEEE        v
|------- 8 -------|-------- 9 --------|-- 3 --|
| O O O O O O O O | O O O O O O O O O | O O O |
| O O O O O O O O | O O O O O O O O O | O O O |
```

### 10x1 headers (user port config/cassette/9V/6V)

There are two 10x1 headers in the BOM, and they are cut differently.

10x1 header #1 (user port config, cassette)

```txt
       cut           cut
   Cfg  v     Cass    v
|-- 3 --|----- 6 -----| 1 |
| O O O | O O O O O O | O |
```

10x1 header #2 (9V, cassette, 6V)

```txt
     cut           cut
  9V  v     Cass    v  6V
|- 2 -|----- 6 -----|- 2 -|
| O O | O O O O O O | O O |
```

### PET power connector

The PET power connector is a 3.93mm pitch connector. Two pins must be removed to key the connector correctly.

```txt
         Power
|--------- 9 ---------|
| O O _ _ O O O O O O |
      ^ ^
    remove
    (key)
```

## Final inspection

Before the first power-on:

- Inspect all solder joints for bridges and cold joints.
- Verify the orientation of all ICs, sockets, and polarized parts.
- Confirm any keyed headers/connectors match the silkscreen.
- If you have a multimeter, do a quick continuity check for obvious shorts on power rails.

Next: [Installing the firmware](installing-the-firmware.md)
