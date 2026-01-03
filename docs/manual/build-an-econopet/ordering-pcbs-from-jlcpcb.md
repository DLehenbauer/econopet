# Ordering PCBs from JLCPCB

This page describes how to order PCBs from JLCPCB. JLCPCB will manufacture the PCBs and assemble the surface-mount components.

JLCPCB will not assemble the through-hole parts in this project (IC sockets, headers, etc.). You will solder those parts yourself after the boards arrive.

## How the process works

JLCPCB is a major PCB manufacturer that also offers low-volume PCB assembly (PCBA), which makes it popular for prototyping and hobby builds.

To place an order, you will upload manufacturing files to their website:

- [Gerber files (Gerber ZIP)](../../../hw/rev-a/production/EconoPET_408096_A.zip): the PCB fabrication data (copper layers, solder mask, silkscreen, drill files, etc.)
- [Bill of Materials (BOM)](../../../hw/rev-a/production/jlcpcb-rev-a-bom.csv): the list of components to be assembled (for PCBA)
- [Component Placement List (CPL)](../../../hw/rev-a/production/jlcpcb-rev-a-cpl.csv): the component location and orientation (for PCBA)

JLCPCB will generate a quote based on your PCB options, how many boards you want assembled, and component cost. After you submit the order, JLCPCB engineers will run a fabrication/assembly review and may ask you to approve substitutions or clarify issues. Once accepted, they fabricate the PCBs, assemble the SMT parts, and ship the boards.

### Minimum quantities

For PCB fabrication, there is a fixed amount of work to prepare and run a job (CAM checks, tooling/drill setup, panelization, solder mask/silkscreen processes). The per-board cost drops quickly once that setup is amortized across multiple boards, so fabs typically enforce a small minimum order quantity (MOQ).

For PCB assembly (PCBA), there is additional one-time setup (assembly programming, feeder setup, and often a stencil). Assembly services commonly require assembling at least a couple boards.

Service | Minimum order
-- | --
PCB fabrication | 5 PCBs
SMT assembly | 2 assembled PCBs

While the price-per-board drops quickly with larger orders, please place a small initial test order to verify the project works for you before committing to a larger quantity.

### Sourcing the FPGA

Most parts used in the EconoPET PCBA are commonly stocked by JLCPCB, but the FPGA (T20Q144C3) is not. Your PCBA order cannot be completed until all BOM line items are available, and JLCPCB may block checkout or prompt you to resolve shortages when a critical part is missing.

To avoid delays, pre-order the FPGA using JLCPCB's Global Sourcing service. In short: you buy the part through JLCPCB (they source it from a distributor such as CoreStaff or Digi-Key), it ships to JLCPCB's warehouse, and they hold it for your account until you submit a PCBA order that uses it.

### Component Availability

JLCPCB maintains an inventory of commonly used SMT parts. When you upload the BOM, they will check availability and warn you if any parts are out of stock.  When this happens, you have a few options:

1. If available, buy "idle parts" (parts other JLCPCB customers pre-ordered but did not use and are reselling).

2. Pre-order the part and wait for it to arrive at JLCPCB before finalizing your order.

Since idle parts are already in the JLCPCB warehouse, they can be used immediately. If they are available, this is generally the best option.

Pre-ordering parts is similar to Global Sourcing, but usually cheaper per unit because it may not involve a third-party distributor. The trade-off is that there may be a minimum order quantity and lead time. Depending on the MOQ and schedule, Global Sourcing may be worth considering.

## Ordering steps

JLCPCB is frequently improving their website and ordering flow.  Below are the general steps to place an order as of mid-2024.

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
   - Use the default PCB options (2 layers, 1.6mm thickness, HASL finish, green soldermask).

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

If you would like help reviewing your order before you place it, you can find me in the [Commodore PET/CBM Enthusiast](https://www.facebook.com/groups/214556078753960) group on Facebook.

Next: [Ordering parts from Mouser](ordering-parts-from-mouser.md)
