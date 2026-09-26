# Using a PmodUSBUART with the ACIA serial port

> **⚠️ Warning:** Do not use a 5V TTL serial adapter. This will damage the FPGA.

The EconoPET's ACIA serial port is available on the `PMOD2` header. It uses the
Digilent Pmod Type-4 UART pinout, so a Digilent
[PmodUSBUART](https://digilent.com/shop/pmod-usbuart-usb-to-uart-interface/)
provides a direct USB connection to the ACIA.

## Connecting the Pmod USBUART

1. Install the [FTDI VCP drivers](https://ftdichip.com/drivers/vcp-drivers/) on the host computer.
1. Verify that the Pmod USBUART jumper connects `LVL` to `VCC`. (Leave the
   `SYS` pin disconnected.)
1. With the EconoPET powered off, install the Pmod USBUART on the **TOP row** of
   pins on `PMOD2`.

   ```text
   Pmod USBUART board
       +---+---+---+---+---+---+
       | 6 | 5 | 4 | 3 | 2 | 1 |  <-- use this row
       +---+---+---+---+---+---+
       |12 |11 |10 | 9 | 8 | 7 |
       +---+---+---+---+---+---+
   ================================= EconoPET PCB
   ```

1. Connect the Pmod USBUART to the host computer with USB.

PMOD2 connects the signals below. The PmodUSBUART supplies the required
cross-over between the EconoPET and USB serial interface.

| PMOD2 pin | EconoPET signal | Pmod USBUART signal |
| --- | --- | --- |
| 1 | CTS input | RTS# |
| 2 | TXD output | RXD |
| 3 | RXD input | TXD |
| 4 | RTS# output | CTS# |

The ACIA is at `$EFF0-$EFF3`. Configure the host terminal to use the serial
settings required by the software running on the EconoPET.

## Testing with SuperPET OS-9

OS-9 provides a convenient way to verify the serial connection:

1. Boot the EconoPET in SuperPET 6809 mode, then boot OS-9.
1. At the main OS-9 terminal, start a terminal monitor for the serial port:

   ```text
   tsmon /t1 &
   ```

1. Configure the host terminal for **1200 baud, 8 data bits, no parity, and 1
   stop bit (8N1)**. Disable both hardware and software flow control.

The serial terminal should present an OS-9 login prompt after `tsmon` starts. If
output is garbled or absent, verify that the terminal's baud, data bits, parity,
and stop bits match the OS-9 configuration.

## Reference

- [Digilent PmodUSBUART Reference Manual](https://digilent.com/reference/_media/pmod:pmod:pmodusbuart_rm.pdf)
