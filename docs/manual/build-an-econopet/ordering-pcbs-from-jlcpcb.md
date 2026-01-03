# Ordering PCBs from JLCPCB

This page describes how to order PCBs from JLCPCB. JLCPCB will manufacture the PCBs and assemble the surface-mount components.

JLCPCB will not assemble the through-hole parts in this project (IC sockets, headers, etc.). You will solder those parts yourself after the boards arrive.

## How the process works

If you have never ordered a PCB before, the basic flow is:

1. Upload manufacturing files, receive a quote, and place the order.
2. JLCPCB engineers review the order and will contact you with any issues.
3. Once approved, JLCPCB fabricates the PCB, assembles the SMT parts, performs basic QA, and ships.

### Minimum quantities

PCB fabrication has a minimum order quantity of 5 boards.

For SMT assembly, JLCPCB requires at least 2 assembled boards.

## Recommended ordering steps

1. Create a JLCPCB account
   - Sign up at <https://jlcpcb.com/>

2. Pre-order the FPGA (important)
   - Use JLCPCB Global Sourcing to pre-order the FPGA: <https://jlcpcb.com/user-center/smtPrivateLibrary/orderParts/?global=1>
   - Search for MFR Part #: T20Q144C3
   - Order one unit per assembled board
   - Pre-ordered parts will be shipped to JLCPCB and held in their warehouse until you submit your assembly order

3. Quote and order the bare PCB
   - Go to JLCPCB Instant Quote: <https://cart.jlcpcb.com/quote>
   - Upload the Gerber ZIP: [hw/rev-a/production/EconoPET_408096_A.zip](../../../hw/rev-a/production/EconoPET_408096_A.zip)
   - Use the default PCB options (for example: 2 layers, 1.6mm thickness, HASL finish).

4. Add SMT assembly
   - Select the SMT Assembly service for your PCB order.
   - Upload the BOM file: [hw/rev-a/production/jlcpcb-rev-a-bom.csv](../../../hw/rev-a/production/jlcpcb-rev-a-bom.csv)
   - Upload the CPL file: [hw/rev-a/production/jlcpcb-rev-a-cpl.csv](../../../hw/rev-a/production/jlcpcb-rev-a-cpl.csv)
   - JLCPCB will price parts and assembly and may prompt you to resolve part availability.
     - If a part is out of stock, check if there are "Idle Parts" available.  These are parts that other users have pre-ordered, did not use, and are selling to other JLCPCB users.
     - If no idle parts are available, you will need to pre-order a minimum quantity and wait before placing the order.

5. Place the order
   - Complete checkout.
   - JLCPCB will fabricate the PCB, assemble the SMT parts, test, and ship.

If you have questions or would like help reviewing your order before you place it, you can find me in the [Commodore PET/CBM Enthusiast](https://www.facebook.com/groups/214556078753960) group on Facebook.

Next: [Ordering parts from Mouser](ordering-parts-from-mouser.md)
