# Building the EconoPET

I use [JLCPCB](https://jlcpcb.com/) to manufacture the PCB and assemble the surface-mount components. To reduce costs, I do not use JLCPCB's through-hole assembly service. Through-hole components (like connectors and sockets) must be soldered manually after receiving the board.

## Ordering from JLCPCB

> **⚠️ Warning:** Rev A isn't perfect. Before ordering, please review the project's [known issues](https://github.com/DLehenbauer/econopet/issues?q=state%3Aopen%20label%3A40%2F8096-A) and understand that some fixes may require minor component rework after boards are received from JLCPCB.

If you've never ordered a PCB before, it's pretty straightforward, but it might be worth getting in touch so we can walk through it together the first time.  You can find me on the [Commodore PET/CBM Enthusiast](https://www.facebook.com/groups/214556078753960) group on Facebook (highly recommended) or [open a GitHub issue](https://github.com/DLehenbauer/econopet/issues/new) on this repository with any questions.

1. You upload your design files (Gerber files for the PCB layout and CSV files for component placement) to a manufacturer, in this case JLCPCB.
2. The manufacturer fabricates the circuit board and automatically places and solders the surface-mount (SMT) components onto it
3. They perform quality testing and ship the boards to you

**Minimum Order Quantity:** JLCPCB requires ordering a minimum of 5 PCBs. You can choose to have 2-5 of these boards assembled with SMT components. The unpopulated bare PCBs will be shipped along with the assembled boards.

### Manufacturing Files

The manufacturing files for JLCPCB are located in `hw/rev-a/production/`:

- **[EconoPET_408096_A.zip](../hw/rev-a/production/EconoPET_408096_A.zip)** - Gerber files for PCB manufacturing
- **[jlcpcb-rev-a-bom.csv](../hw/rev-a/production/jlcpcb-rev-a-bom.csv)** - Bill of Materials for SMT assembly
- **[jlcpcb-rev-a-cpl.csv](../hw/rev-a/production/jlcpcb-rev-a-cpl.csv)** - Component positions for SMT assembly

### Ordering Steps

1. **Create a JLCPCB Account** - Visit [jlcpcb.com](https://jlcpcb.com) and sign up for an account

2. **Pre-order the FPGA** (Important!)
   - Use [JLCPCB Global Sourcing](https://jlcpcb.com/user-center/smtPrivateLibrary/orderParts/?global=1) to pre-order the FPGA (Part #T20Q144C3)
   - JLCPCB does not stock the FPGA and must source it from a distributor (I've sourced from Digi-Key and CoreStaff in the past.)
   - This step should be done before ordering the board to ensure the component will be available for assembly when you place your order

3. **Quote & Order PCB**
   - Go to [JLCPCB Instant Quote](https://jlcpcb.com/instant-quote)
   - Upload **[EconoPET_408096_A.zip](../hw/rev-a/production/EconoPET_408096_A.zip)** (Gerber files)
   - Use all default PCB options (2 layers, 1.6mm thickness, HASL finish, etc.)
   - Add to cart and proceed

4. **Add SMT Assembly**
   - Select **SMT Assembly** service on the order page
   - Upload **[jlcpcb-rev-a-bom.csv](../hw/rev-a/production/jlcpcb-rev-a-bom.csv)** for the Bill of Materials
   - Upload **[jlcpcb-rev-a-cpl.csv](../hw/rev-a/production/jlcpcb-rev-a-cpl.csv)** for component placement data
   - JLCPCB will calculate the total cost for the components and assembly

5. **Complete Your Order**
   - Review the order summary and component selections
   - Proceed to checkout and payment
   - JLCPCB will manufacture the PCB, assemble the components, and ship the boards to you

## Through-Hole Assembly

After receiving your boards from JLCPCB, you'll need to solder the through-hole components. You can source all through-hole components from [Mouser Electronics](https://www.mouser.com).

> **💡 Tip:** While Mouser part numbers are given for completeness, the following through-hole components are common parts available inexpensively in bulk from various sources (Amazon, AliExpress, eBay, etc.)
>
> - Break-away pin headers (2.54mm pitch, 1 and 2 rows)
> - Jumper caps (2.54mm pitch)
> - Tactile push buttons (6.0x3.5mm, 2 pin)
> - Right-angled push buttons (6mm)
> - PCB mounting pillars (nylon, 11mm height, adhesive base)
> - DC power jacks (5.5mm, 2.1mm center pin)

### BOM for Through-Hole Components

Mfr Part Number|Mouser Part Number|Order Quantity|Description|Datasheet URL
-|-|-|-|-
W65C02S6TPG-14|955-W65C02S6TPG-14|1|W65C02S CPU (DIP-40). Do not substitute other 6502 variants.|[datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)
W65C21N6TPG-14|955-W65C21N6TPG-14|2|W65C21N PIA (DIP-40). Do not substitute W65C21S (lacks current limiting resistors).|[datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf)
W65C22N6TPG-14|955-W65C22N6TPG-14|1|W65C22N VIA (DIP-40). Do not substitute W65C22S (lacks current limiting resistors).|[datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf)
AS6C1008-55PCN|913-AS6C1008-55PCN|1|128KB SRAM (DIP-32, 55ns)|[datasheet](https://www.mouser.com/datasheet/3/893/1/AS6C1008_Mar_2023V1.2.pdf)
1-2199299-5|571-1-2199299-5|4|DIP-40 IC Socket (optional but recommended)|[datasheet](https://www.mouser.com/catalog/additional/Tyco%20Electronics_1-1773906-9_DIP_Socket_QRG_012017.pdf)
DILB32P-223TLF|649-DILB32P223TLF|1|DIP-32 IC Socket (optional but recommended)|[datasheet](https://cdn.amphenol-cs.com/media/wysiwyg/files/drawing/10052485.pdf)
640384-9|571-6403849|1|PET power connector (optional if using stand-alone)|[datasheet](https://www.te.com/commerce/DocumentDelivery/DDEController?Action=srchrtrv&DocNm=640384&DocType=Customer+Drawing&DocLang=English&PartCntxt=640384-9&DocFormat=pdf)
10129378-936003BLF|649-1012937893603BLF|1|36x1 Header: PET keyboard/video (optional if using stand-alone)|[datasheet](https://cdn.amphenol-cs.com/media/wysiwyg/files/drawing/10129378.pdf)
AT-1220-TT-9-R|665-AT1220TT9R|1|Internal PCB mounted speaker (optional)|[datasheet](https://www.mouser.com/datasheet/3/360/1/specs_AT_1220_TT_9_R.pdf)
SJ-43504-SMT-TR|490-SJ-43504-SMT-TR|1|3.5mm Audio Jack (required for sound)|[datasheet](https://www.mouser.com/datasheet/3/6118/1/sj_43504_smt_tr.pdf)
DS04-254-1L-02BK|490-DS04-254-1L-02BK|1|DIP Switch (config display and keyboard)|[datasheet](https://www.mouser.com/datasheet/3/6118/1/ds04_254.pdf)
MSA-G|737-MSA-G|1|Jumper cap (config Video or 5V)|[datasheet](https://www.mouser.com/datasheet/3/6015/1/msa_g_data_sheet.pdf)
MJTP1117|642-MJTP1117|1|Right-angled Button (Menu)|[datasheet](https://www.mouser.com/catalog/specsheets/Apem_04-15-2025_MJTP%20Series-6MM.pdf)
PTS636SK43 LFS|611-PTS636SK43LFS|4|Push Buttons: Flash & Run required, Reset & NMI optional|[datasheet](https://www.ckswitches.com/media/2779/pts636.pdf)
10129381-950002BLF|649-1012938195002BLF|2|25x2 Header: bus, user, MCU, 5V, menu (optional breakouts)|[datasheet](https://cdn.amphenol-cs.com/media/wysiwyg/files/drawing/10129381.pdf)
10129381-940002BLF|649-1012938194002BLF|1|20x2 Header: IEEE, KeyEx (optional breakouts)|[datasheet](https://cdn.amphenol-cs.com/media/wysiwyg/files/drawing/10129381.pdf)
613012243121|710-613012243121|2|PMOD Connectors (6x2, right angle, optional)|[datasheet](https://www.we-online.com/components/products/datasheet/613012243121.pdf)
61201621621|710-61201621621|1|SWD/JTAG (8x2 pin, shrouded, optional)|[datasheet](https://www.we-online.com/components/products/datasheet/61201621621.pdf)
10129378-910002BLF|649-1012937891002BLF|2|10x1 Header: User config (required) and cassette, 6V, 9V (optional breakouts)|[datasheet](https://cdn.amphenol-cs.com/media/wysiwyg/files/drawing/10129378.pdf)
SC1628|358-SC1628|1|32GB microSD card (known good, but feel free to try what you have on hand)|[datasheet](https://www.mouser.com/catalog/additional/Raspberry_Pi_rpi_sdcard_EU_DoC.pdf)
RASPC722X|502-RASPC722X|1|DC Power Jack (optional: for standalone use)|[datasheet](https://www.mouser.com/datasheet/3/144/1/rapc722x_cd.pdf)
LCBSBM-7-01A-RT|144-LCBSBM-7-01A-RT|2 or 4|PCB Mounting Pillar (get 4 if using standalone)|[datasheet](https://www.essentracomponents.com/en-us/p/self-adhesive-pcb-support-pillars-non-locking/lcbsbm-7-01a-rt)
LCBSBM-7-01A-RT|144-LCBSBM-7-01A-RT|2 or 4|PCB Mounting Pillar (get 4 if using standalone)|[datasheet](https://www.essentracomponents.com/en-us/p/self-adhesive-pcb-support-pillars-non-locking/lcbsbm-7-01a-rt)
EEU-FM1H121LB|667-EEU-FM1H121LB|1|120uF 50V low-ESR capacitor (C89)|[datasheet](https://industrial.panasonic.com/cdbs/www-data/pdf/RDF0000/ABA0000C1018.pdf)

### Assembly

The back of the PCB lists a recommended assembly order, generally sorted by component height (shortest first). This order is not critical. It's perfectly fine to skip optional components (such as header pins) and add them later if desired.

> **⚠️ Warning: Silkscreen Error (rev 40/8096-A)**
>
> Steps 22-23 on the silkscreen incorrectly list "8x2 Header (2)", implying two 8x2 pin headers are needed. In reality, one of these is the shrouded JTAG/SWD connector (part #61201621621), not a standard pin header.
>
> **Do not cut an extra 8x2 header** from your 25x2 or 20x2 pin headers, as this will leave you short on parts.
>
> Additionally, since the shrouded JTAG/SWD connector is taller than standard headers, the assembly order should list it after the speaker (step 30.5) instead of 22-23 as is currently printed.

### Component Orientation

Most through-hole components are either symmetrical or can only be inserted one way, so orientation is straightforward. However, there are a few components that require careful attention:

- **ICs and IC Sockets:** These must be oriented correctly. Both the IC and its socket have a notch on one end that should face the top of the board, as indicated by the silkscreen markings.

- **DIP Switch:** The DIP switch should be oriented so the 'ON' position faces towards the FPGA to match the silkscreen markings.

- **Polarized Capacitors:** C89 is a polarized electrolytic capacitor. The longer lead is the positive (+) terminal, which should align with the '+' marking on the PCB silkscreen.  C89 has multiple holes for different capacitor sizes.  Ensure you use the correct holes for the capacitor you have.

- **JTAG/SWD Connector:** The shrouded JTAG/SWD connector has a keyed design to ensure correct orientation. The notch should face downward toward the HDMI connector, corresponding with the marking on the silkscreen.

For all other components, refer to the silkscreen markings on the PCB for guidance if you're unsure about polarity or orientation.

### Header Splitting & Keying

Break-away pin headers are sold in long strips that must be divided into the required sizes. To split a header, use side cutters or a razor saw to cut between pins at the indicated positions. When cut, pin headers can sometimes break unevenly or at the wrong point, so it's a good idea to order some spares.

#### 1x36 Header (PET Keyboard/Video)

**Note:** Mouser part (649-1012937893603BLF) is a slightly taller header that matches the pin length used by Commodore in my CBM 8032. Likely, a standard height header can be substituted.

```txt
               cut                                       cut
      Video     ↓                Keyboard                 ↓
|------ 7 ------|------------------ 20 -------------------|-------- 9 --------|
| O _ O O O O O | O _ O O O O O O O O O O O O O O O O O O | O O O O O O O O O |
    ↑               ↑
 remove          remove
  (key)           (key)
```

#### 25x2 Headers (Bus/User/MCU/5V/Menu)

**Note:** There are two 25x2 pin headers in the BOM. One is used uncut for the bus expansion breakout. The other is divided as shown below.

```txt
                     cut                     cut     cut
           MCU        ↓          User         ↓  5V   ↓ Menu
|--------- 10 --------|---------- 11 ---------|-- 3 --| 1 |
| O O O O O O O O O O | O O O O O O O O O O O | O O O | O |
| O O O O O O O O O O | O O O O O O O O O O O | O O O | O |
```

#### 20x2 Header (IEEE/KeyEx)

```txt
                 cut                 cut       
       KeyEx      ↓       IEEE        ↓
|------- 8 -------|-------- 9 --------|-- 3 --|
| O O O O O O O O | O O O O O O O O O | O O O |
| O O O O O O O O | O O O O O O O O O | O O O |
```

#### 10x1 Headers (User port cfg/Cassette/9V/6V)

**Note:** There are two 10x1 pin headers in the BOM. Both need to be divided differently, as shown below.

##### 10x1 Header #1 (User port config, cassette)

```txt
       cut           cut
   Cfg  ↓     Cass    ↓
|-- 3 --|----- 6 -----| 1 |
| O O O | O O O O O O | O |
```

##### 10x1 Header #2 (9V, cassette, 6V)

```txt
     cut           cut
  9V  ↓     Cass    ↓  6V
|- 2 -|----- 6 -----|- 2 -|
| O O | O O O O O O | O O |
```

#### PET Power Connector

The PET power connector is a larger 3.93mm pitch connector with 9 pins. Two pins must be removed to key the connector correctly.

```txt
         Power
|--------- 9 ---------|
| O O _ _ O O O O O O |
      ↑ ↑
    remove
    (key)
```

## Bootstrapping the Firmware

Normally, firmware upgrades are performed via the microSD card. However, the initial installation of the firmware requires a bootloader to be flashed onto the EconoPET via USB.

The full steps for bootstrapping the EconoPET firmware are:

1. Initialize a microSD card with the firmware files
1. Install the bootloader onto the EconoPET via USB (one time only)
1. Power on the EconoPET to load the firmware from the microSD card

Each of these steps is described in detail below.

> **⚠️ Note:** To avoid a "bright spot" on the CRT, it is best to install the bootloader and perform the *initial* firmware installation outside the CBM/PET machine using a separate 9V DC power supply.

### Initializing the microSD Card

The EconoPET uses a microSD card to configure the system at power on and reset. To prepare a microSD card for the EconoPET, or upgrade the firmware in the future, you will need to perform the following steps:

- Download and unzip the latest firmware from the EconoPET site: https://dlehenbauer.github.io/econopet/40-8096-A.html
- Format the microSD card with the FAT32 or exFAT filesystem
- Copy the unzipped contents of the firmware archive to the root of the microSD card

If you have trouble formatting the microSD card, try the SD Memory Card Formatter tool from the SD Association: https://www.sdcard.org/downloads/

> **⚠️ Note:** The microSD card reader uses a push-to-eject mechanism for card removal. Do not remove the card by pulling. Please be gentle with PCB mounted connectors.

### Installing the EconoPET Bootloader

1. With the board removed from your CBM/PET, connect a USB cable from the EconoPET's USB-C port to your computer.
1. Enter flash mode:
   - Press and hold the FLASH button on the EconoPET.
   - While holding FLASH, connect 9V DC power to the EconoPET (use DC jack or 2-pin breakout).
   - After a couple of seconds, you can release the FLASH button.
1. The EconoPET will appear on your PC as a USB mass storage device named RPI-RP2.
1. Copy the bootloader file: Drag and drop the bootloader.uf2 file into the RPI-RP2 drive. (bootloader.uf2 is contained in the same .zip used to initialize the microSD card.)
1. After the file is copied, the EconoPET will automatically reboot and finish the firmware update from the microSD card. (Green LED will blink rapidly during this process.)
1. When the flashing process is complete, you will hear a happy 4-note beep from the speaker.
1. Disconnect power and the USB cable and proceed to install in the PET.
