# EconoPET Diagnostics

The `/test` directory is not intended for end-user operation. It contains files
for hardware diagnostics used by people building, bringing up, or repairing
EconoPET boards.

> &#x26A0;&#xFE0F; **Warning:** Some diagnostics are destructive and can erase disks or SD cards.
> They are intended to run in a test environment prepared by the firmware. For
> example, the drive diagnostic program formats all disks in all drives.

Leave this directory intact when copying the firmware package to a microSD
card.

## Accessing diagnostics

Run diagnostics from a serial terminal connected to the UART pins on the
16-pin (2x8) SWD/JTAG header (`J22`).

> &#x26A0;&#xFE0F; **Warning:** Use a **3.3 V** USB-to-UART adapter, such as a Raspberry Pi
> Debug Probe. A 5 V TTL adapter can damage the RP2040.

With the EconoPET powered off, connect the adapter to the `MCU` row
(odd-numbered pins) of `J22`:

| `J22` pin | Silkscreen | EconoPET signal | Serial adapter pin |
| --- | --- | --- | --- |
| 1 | `GND` | Ground | `GND` |
| 7 | `RX` | RP2040 UART0 RX (input) | `TX` (transmit) |
| 9 | `TX` | RP2040 UART0 TX (output) | `RX` (receive) |

Configure the terminal for **115200 baud, 8 data bits, no parity, and 1 stop
bit (8N1)**, with hardware and software flow control disabled. Power on the
EconoPET and press **Enter** to display the `econopet>` prompt.

Enter `test` to list diagnostic plans, or run a plan with:

```text
test <plan> [count]
```

The optional `count` is the number of passes, from `1` to `255` (default `1`).
Enter `help` to list all firmware command-line commands.

## Available diagnostics

| Plan | Command | Description |
| --- | --- | --- |
| Memory | `test memory` | Runs an extended March C- test on PET SRAM and character BRAM, overwriting memory. |
| Bring-up | `test bup` | Runs memory checks for surface-mount bring-up before through-hole parts are installed. |
| SD card | `test sd` | Prompts you to replace the current card with a sacrificial microSD card, then runs a destructive full-stack SD test. |
| Drive | `test drive` | Tests virtual drives using temporary scratch images in `/test/tmp`. Disconnect external IEEE-488 devices first. |

Diagnostics take exclusive control of the board and halt on completion or
failure. Reset or power-cycle the EconoPET to return to normal operation.

The drive plan loads its configuration profile before creating or mounting
scratch images. A missing required ROM is a fatal configuration error. After
successful profile loading, the plan mounts its scratch images before resetting
and booting the PET so the IEEE pins are parked correctly.
