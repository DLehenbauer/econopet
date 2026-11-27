# Datasheets

Revision-specific parts are marked `(Rev-A only)` or `(Rev-B only)`.
Other entries are shared references or design background.

## 6502 CPU

* [W65C02S](https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf) - Mostly 6502 compatible CPU that is still in production.

## 6520/6521 PIA

* [W65C21](https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf) - Modern PIA used for the PET keyboard, cassette, and IEEE-488 interfaces.
* [Rockwell R6520](http://archive.6502.org/datasheets/rockwell_r6520_pia.pdf) - Original PET PIA reference.

## 6522 VIA

* [W65C22](https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf) - Modern VIA used for the PET user port, cassette control, IEEE-488 signals, and CB2 sound output.

## 6545 CRTC

* [Rockwell R6545](http://archive.6502.org/datasheets/rockwell_r6545-1_crtc.pdf) - Original PET CRTC reference.

## FPGA

* [T20Q144C3](https://www.efinixinc.com/docs/trion20-ds-v5.0.pdf) - FPGA that manages the bus, memory, video, and emulated peripherals.

## Memory

* [AS6C1008](https://www.alliancememory.com/wp-content/uploads/AS6C1008_Mar_2023V1.2.pdf) (Rev-A only) - 128 KB external SRAM with 55 ns access time.
* [IS62WV1288BLL-55QLI](https://www.issi.com/WW/pdf/62WV1288ALL.pdf) (Rev-B only) - 128 KB external SRAM with 55 ns access time.
* [W25Q128JV](https://www.winbond.com/resource-files/w25q128jv_dtr%20revc%2003272018%20plus.pdf) - 16 MB Quad SPI flash used to store and execute RP2040 firmware.

## MCU

* [RP2040](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) - Firmware MCU that configures and controls the FPGA and handles SD-card storage and USB peripherals.
  * [ARM PrimeCell SSP (PL022)](https://developer.arm.com/documentation/ddi0194/h/introduction/about-the-arm-primecell-ssp--pl022-) - Register-level reference for the RP2040 SPI controllers.

## Keyboard

* [SN74LS145](https://www.ti.com/lit/ds/symlink/sn74ls145.pdf) - BCD-to-decimal decoder with open-collector outputs to select active keyboard column.

## IEEE

* [MC3446](http://pdf.datasheetcatalog.com/datasheets/400/501349_DS.pdf) - Original PET IEEE-488 transceiver reference.
* [SN74LS07](https://www.ti.com/lit/ds/symlink/sn74ls07.pdf) - Open-collector drivers for the IEEE-488 data and control lines.
* [SN74LS245](https://www.ti.com/lit/ds/symlink/sn74ls245.pdf) - Bus transceivers used as IEEE-488 receivers.

## Power

* TPS5430 (Rev-A only) - 3 A buck regulator that generates the board's 5 V supply from the unregulated input.
  * [Datasheet](https://www.ti.com/lit/ds/symlink/tps5430.pdf) - Operating limits and component-selection guidance for the 5 V regulator.
  * [User's Guide](https://www.ti.com/lit/ug/slvu157a/slvu157a.pdf) - Evaluation-board schematic, layout, and measurements for comparison with EconoPET's regulator design.
  * [Application Report](https://www.ti.com/lit/an/slva237c/slva237c.pdf) - Compensation guidance when using aluminum or ceramic output capacitors with the TPS5430.
* [TPS564247](https://www.ti.com/lit/ds/symlink/tps564247.pdf) (Rev-B only) - 4 A synchronous buck regulators for the 5 V board supply and 6.1 V cassette motor supply.
* [TLV1117LV](https://www.ti.com/lit/ds/symlink/tlv1117lv.pdf) (Rev-A only) - MLCC-compatible 3.3 V LDO substituted for the AMS1117 to work around incorrectly placed ESR-compensation resistors.
* [AMS1117-3.3](http://www.advanced-monolithic.com/pdf/ds1117.pdf) (Rev-B only) - 3.3 V LDO for the MCU, SRAM, and FPGA I/O supplies.
* [LM317A](https://en.unisonic.com.tw/uploadfiles/836/part_no_pdf/LM317A.pdf) - Adjustable regulator for the FPGA core supply on both revisions and the cassette motor supply on Rev-A.
* [ROHM Layout Guide](https://fscdn.rohm.com/en/products/databook/applinote/ic/power/switching_regulator/converter_pcb_layout_appli-e.pdf) - General buck-converter PCB layout guidance for power-loop routing and noise control.

## Other

* [SN74LVC4245A](https://www.ti.com/lit/ds/symlink/sn74lvc4245a.pdf) - Bus transceivers that translate between the 3.3 V CPU/FPGA side and the 5 V PIA/VIA side.
* [Card Edge Connector](https://files.edac.net/edac/content/305/305-024-500-501%20-%20EDAC%20Card%20Edge%20Connector.PDF) - Mechanical reference for PET-compatible card edges.

## Unused

Parts previously used in experiments, but not currently used in the EconoPET
board revisions.

* [IS61WV1288EEBLL](https://www.issi.com/WW/pdf/61-64WV1288EEBLL.pdf) - Used in experimental memory adapter.
* Op Amps - Background for evaluating single-supply audio amplifier designs.
  * [How to Bias Op Amps Correctly](https://www.renesas.com/en/document/apn/r13an0003-how-bias-op-amps-correctly) - Biasing and AC-coupling guidance relevant to potential op-amp audio circuits.
