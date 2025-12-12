# Building the EconoPET

I use [JLCPCB](https://jlcpcb.com/) to manufacture the PCB and assemble the surface-mount components. To reduce costs, I do not use JLCPCB's through-hole assembly service. Through-hole components (like connectors and sockets) must be soldered manually after receiving the board.

## Ordering from JLCPCB

If you've never ordered a PCB before, it's pretty straightforward, but it might be worth getting in touch so we can walk through it together the first time.

1. You upload your design files (Gerber files for the PCB layout and CSV files for component placement) to a manufacturer, in this case JLCPCB.
2. The manufacturer fabricates the circuit board and automatically places and solders the surface-mount (SMT) components onto it
3. They perform quality testing and mail the boards to you

**Minimum Order Quantity:** JLCPCB requires ordering a minimum of 5 PCBs. You can choose to have 2-5 of these boards assembled with SMT components. The unpopulated bare PCBs will be shipped along with the assembled boards.

### Manufacturing Files

The manufacturing files for JLCPCB are located in `hw/rev-a/production/`:

- **[EconoPET_408096_A.zip](../hw/rev-a/production/EconoPET_408096_A.zip)** - Gerber files for PCB manufacturing
- **[bom.csv](../hw/rev-a/production/bom.csv)** - Bill of Materials for SMT assembly
- **[positions.csv](../hw/rev-a/production/positions.csv)** - Component positions for SMT assembly

### Ordering Steps

1. **Create a JLCPCB Account** - Visit [jlcpcb.com](https://jlcpcb.com) and sign up for an account

2. **Pre-order the FPGA** (Important!)
   - Use [JLCPCB Global Sourcing](https://jlcpcb.com/user-center/smtPrivateLibrary/orderParts/?global=1) to pre-order the FPGA (Part #T20Q144C3)
   - JLCPCB does not stock the FPGA and must source it from a distributor (I've sourced from Digi-Key and CoreStaff in the past.)
   - This step should be done before ordering the board to ensure the component will be available for assembly when you place your order

3. **Quote & Order PCB**
   - Go to [JLCPCB Instant Quote](https://jlcpcb.com/instant-quote)
   - Upload `EconoPET_408096_A.zip` (Gerber files)
   - Use all default PCB options (2 layers, 1.6mm thickness, HASL finish, etc.)
   - Add to cart and proceed

4. **Add SMT Assembly**
   - Select **SMT Assembly** service on the order page
   - Upload `bom.csv` for the Bill of Materials
   - Upload `positions.csv` for component placement data
   - JLCPCB will calculate the total cost for the componants and assembly

5. **Complete Your Order**
   - Review the order summary and component selections
   - Proceed to checkout and payment
   - JLCPCB will manufacture the PCB, assembly components, and send to you in the mail

## Through-Hole Assembly

After receiving your boards from JLCPCB, you'll need to solder the through-hole components. You can source all through-hole components from [Mouser Electronics](https://www.mouser.com).

### Assembly Order

The back of the PCB lists a recommended assembly order, generally sorted by component height (shortest first). This order is not critical. It's perfectly fine to skip optional components (such as header pins) and add them later if desired.

### Component Orientation

Most through-hole components are either symmetrical or can only be inserted one way, so orientation is straightforward. However, there are a few components that require careful attention:

- **ICs and IC Sockets:** These must be oriented correctly. Both the IC and its socket have a notch on one end that should face the top of the board, as indicated by the silk screen markings.

- **Optional Polarized Capacitors:** There are two optional polarized capacitors that are typically not populated. If you choose to install them, the stripe on the capacitor (indicating the negative lead) should face the negative lead as indicated on the silkscreen.

For all other components, refer to the silkscreen markings on the PCB for guidance if you're unsure about polarity or orientation.
