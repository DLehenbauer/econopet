# Rev B. TODOs

* Design
  * Consider [SMD SRAM](https://jlcpcb.com/partdetail/444095-IS61WV1288EEBLL10TLI/C443418) such as the IS61WV1288EEBLL-10TLI.
* Layout
  * Move PMOD connectors closer to board edge.
* Footprints
  * Remove in-pad vias from RP2040 and LM317 (solder bleeds through)
* Silkscreen
  * Front:
    * Swap Config labels so ON is above OFF
    * Consider adding boxes for SSN (`[] [] [] [] [] - [] []`).
  * Back:
    * Label remaining ports (IEEE, USER, KEYBOARD, etc.)
    * Label remaining pins (C[7:0], R[9:0], etc)
