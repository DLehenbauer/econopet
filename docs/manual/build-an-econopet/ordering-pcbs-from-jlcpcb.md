# Ordering PCBs from JLCPCB

This page describes how to order PCBs from JLCPCB. JLCPCB is a major PCB manufacturer that also offers low-volume PCB assembly (PCBA), which makes it popular for prototyping and hobby builds.

## How the process works

To place an order, you will upload a set of manufacturing files to JLCPCB's website:

- [Gerber files (Gerber ZIP)](/econopet/downloads/EconoPET_408096_A.zip): Fabrication data for the PCB
- <a href="/econopet/downloads/jlcpcb-rev-a-bom.csv" download>Bill of Materials (BOM)</a>: the list of SMT components to be assembled (for PCBA)
- <a href="/econopet/downloads/jlcpcb-rev-a-cpl.csv" download>Component Placement List (CPL)</a>: the component location and orientation (for PCBA)

JLCPCB will generate a quote based on your PCB options, how many boards you want assembled, and component cost.  After you submit the order, JLCPCB engineers will run a fabrication/assembly review and notify you if there are any issues with your order.

Once your order is accepted, JLCPCB's factory will fabricate the PCB.  After the PCB is fabricated and checked by QA, the PCB will be sent to the assembly line where SMT components will be placed and soldered.  After a final inspection, the completed boards will be shipped to you.

This project does not use JLCPCB's through-hole assembly service. You will order the through-hole parts separately from Mouser and solder them yourself when your PCBs arrive.

### Minimum quantities

For PCB fabrication, there is a fixed amount of work to prepare and run a job (CAM checks, tooling/drill setup, panelization, and solder mask/silkscreen processing). Similarly, for PCB assembly (PCBA) there are one-time setup costs (assembly programming, feeder setup, and often a stencil).

To amortize these costs, PCB manufacturers typically have minimum order quantities:

| Service | Minimum order |
| --- | --- |
| PCB fabrication | 5 PCBs |
| SMT assembly | 2 assembled PCBs |

Please place a small initial test order to verify the project works for you before committing to a larger quantity.

### Sourcing the FPGA

The FPGA used by the EconoPET (T20Q144C3) is not a standard JLCPCB part.  You will need to pre-purchase the FPGA using JLCPCB's Global Sourcing service before you can finalize your PCB/PCBA order.

When you use JLCPCB's global sourcing service, you buy the part through JLCPCB's website and JLCPCB sources it from a distributor such as CoreStaff or Digi-Key.  The distributor ships the component directly to JLCPCB's warehouse, where JLCPCB holds the part for you until you submit a PCBA order that uses it.

### Out-of-stock parts

When you upload the BOM, JLCPCB will check their inventory and warn you if any parts are out of stock. If this happens, you have a couple options:

1. If available, buy "idle parts" (extra parts pre-ordered by other JLCPCB customers who no longer need them).
2. Pre-order the part and wait for it to arrive at JLCPCB before finalizing your PCB/PCBA order.

Idle parts can be used immediately because they are already in JLCPCB's warehouse. This is generally the best option when it is available.

Pre-ordering parts is similar to Global Sourcing, but does not involve a third-party distributor which typically reduces the per-unit cost.  The trade-off is that there may be a minimum order quantity and lead time. Depending on the MOQ and schedule, Global Sourcing may be worth considering.

## Ordering the PCB

Below are the general steps to place an order.  Please help improve this document by [opening a GitHub issue](https://github.com/DLehenbauer/econopet/issues) if you run into a problem or have questions about the process.

> **💡 Tip:** To get a rough cost estimate, you can do a "dry run" by uploading the Gerber, BOM, and CPL files to JLCPCB and stopping before checkout. You can do this even if you have not pre-ordered the FPGA yet.  Don't forget to add the price of the through-hole parts from Mouser to your total cost estimate.

1. Create a JLCPCB account

    - Sign up at <https://jlcpcb.com/>

2. Pre-order the FPGA (important)

    - Use [JLCPCB Global Sourcing](https://jlcpcb.com/user-center/smtPrivateLibrary/orderParts/?global=1) to pre-order the FPGA
    - Search for MFR Part #: T20Q144C3
    - Pick a distributor and add the part to your cart
        - Order one unit per assembled board (so order at least 2, or you won't be able to place a minimum PCBA order for 2 boards)
        - In the past, CoreStaff has had a nice price break if you order 5+ units
    - Complete the checkout process
        - Go to your cart and select the 'Global Sourcing Parts' tab
        - Check the row containing the T20Q144C3 part and click 'Secure Checkout' to complete your order
    - Pre-ordered parts will be shipped to JLCPCB and held in their warehouse until you submit your assembly order

3. Quote for the PCB

    - Go to JLCPCB's [Instant Quote page](https://cart.jlcpcb.com/quote)
    - Click 'Add Gerber File' and upload the Gerber ZIP: [EconoPET_408096_A.zip](/econopet/downloads/EconoPET_408096_A.zip)
    - Set the following PCB options:
        - PCB Qty: (choose how many PCBs you want fabricated)
    - Unless you know what you are doing, use the default values for other options (list below for reference):
        - Base Material: FR-4
        - Layers: 2
        - Dimensions: (detected from Gerber file)
        - Product Type: Industrial/Consumer Electronics
        - PCB Specifications:
            - Different Design: 1
            - Delivery Format: Single PCB
            - PCB Thickness: 1.6mm
            - PCB Color: Green
            - Silkscreen: White
            - Material Type: FR4 TG135
            - Surface Finish: HASL (with lead)
        - High-spec Options
            - Outer Copper Weight: 1oz
            - Via Covering: Tented
            - Via Plating Method: 0.3mm/(0.4/0.45mm)
            - Board Outline Tolerance: +/-0.2mm(Regular)
            - Confirm Production file: No
            - Mark on PCB: Remove Mark
            - Electrical Testing: Flying Probe Test
            - Gold Fingers: No
            - Castellated Holes: No
            - Edge Plating: No
            - Blind Slots: No
            - UL Marking: No

4. Add PCB Assembly

    - Turn on the "PCB Assembly" toggle switch on the Instant Quote page.
    - Set the following PCB Assembly options:
        - PCBA Qty: (choose how many boards you want assembled)
    - Unless you know what you are doing, use the default values for other options (list below for reference):
        - PCBA Type: Economic
        - Assembly Side: Top Side
        - Tooling holes: Added by JLCPCB
        - Confirm Parts Placement: No
        - Stencil Storage: No
        - Fixture Storage: No
        - Parts Selection: By Customer

5. Upload BOM and CPL files
    - Push "Next" to go to the PCB preview page.
    - Push "Next" again to go to the BOM/CPL upload page.
    - Upload the BOM file: <a href="/econopet/downloads/jlcpcb-rev-a-bom.csv" download>jlcpcb-rev-a-bom.csv</a>
    - Upload the CPL file: <a href="/econopet/downloads/jlcpcb-rev-a-cpl.csv" download>jlcpcb-rev-a-cpl.csv</a>
    - Click 'Process BOM & CPL' to continue.

6. Review BOM Data
    - JLCPCB will display the parts found in the BOM along with pricing and availablity.
    - If a part is out of stock, click the table row (not the magnifying glass) to see options for idle parts or pre-ordering.
    - Click 'Next' to continue to the Component Placement page.

7. Sanity check that the Component Placement looks right, then press 'Next' to continue to the Quote & Order tab.

8. Place your order
    - Select Reserch\Education\DIY\Entertainment / DYI - HS Code 902300
    - Click 'Save To Cart' (your card will show two items: PCB fabrication and PCB assembly)
    - Click 'Secure Checkout' to complete your order.

If you would like help reviewing your order before you place it, you can find me in the [Commodore PET/CBM Enthusiast](https://www.facebook.com/groups/214556078753960) group on Facebook.

Next: [Ordering parts from Mouser](ordering-parts-from-mouser.md)
