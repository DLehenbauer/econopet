# FPGA

## Documentation

* [T8 Datasheet](https://www.efinixinc.com/support/docsdl.php?s=ef&pn=DST8)
* [T20 Datasheet](https://www.efinixinc.com/docs/trion20-ds-v5.10.pdf)
* [Trion Interfaces User Guide](https://www.efinixinc.com/support/docsdl.php?s=ef&pn=UG-TINTF)
* [Efinity Synthesis User Guide](https://www.efinixinc.com/support/docsdl.php?s=ef&pn=UG-EFN-SYNTH)
* [Efinity Timing Closure User Guide](https://www.efinixinc.com/support/docsdl.php?s=ef&pn=UG-EFN-TIMING)
* [AN 006: Configuring Trion FPGAs](https://www.efinixinc.com/docs/an006-configuring-trion-fpgas-v6.5.pdf)

## Driver

Directions for installing the driver are in [AN 006: Configuring Trion FPGAs](https://www.efinixinc.com/docs/an006-configuring-trion-fpgas-v6.5.pdf):

1. Connect the board to your computer with the appropriate cable and power it up.
2. Run the Zadig software.
Note:  To ensure that the USB driver is persistent across user sessions, run the
Zadig software as administrator.
3. Choose Options > List All Devices.
4. Repeat the following steps for each interface. The interface names end with (Interface N),
where N is the channel number.
• Select libusb-win32 in the Driver drop-down list.
• Click Replace Driver.
5. Close the Zadig software.

## T8

There are 24 BRAMs for ~12KB of RAM.

Supported access modes:

* 256 x 20
* 256 x 16
* 512 x 10
* 512 x 8
* 1024 x 5
* 1024 x 4
* 2048 x 2
* 4096 x 1

## T20

Pin-compatible with T8.  There are 204 BRAMs for ~102KB of RAM (+90KB).
