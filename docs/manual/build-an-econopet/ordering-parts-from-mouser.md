# Ordering parts from Mouser

This section lists the through-hole parts needed to build an EconoPET mainboard.  These parts will be hand-soldered onto the PCB after receiving it from JLCPCB.

## What to buy from Mouser (vs elsewhere)

All items below can be sourced from Mouser, but some are generic and often cheaper in bulk from other sources:

- Break-away pin headers (2.54mm pitch, 1x and 2x)
- Jumper caps (2.54mm pitch)
- Small tactile push buttons
- Right-angle push button (menu)
- PCB mounting pillars (nylon, adhesive)
- DC power jack (5.5mm OD / 2.1mm ID)

If you do buy headers from Mouser, consider ordering a few spares. Cutting headers to length is easy to get wrong on the first try.

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
EEU-FM1V331|667-EEU-FM1V331|1|330uF 35V low-ESR capacitor (C89)|[datasheet](https://industrial.panasonic.com/cdbs/www-data/pdf/RDF0000/ABA0000C1018.pdf)

Next: [Assembling the board](assembling-the-board.md)
