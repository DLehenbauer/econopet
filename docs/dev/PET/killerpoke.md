# Killer Poke

Early PET models accessed video memory during the same clock cycle as the CPU.  To prevent visual artifacts caused by simultaneous access, Commodore delayed video memory updates until the next vertical blanking interval.  This was done by polling the VIA's PB5 pin, which was connected to the timing generation for the VERT signal.

This approach avoided visual artifacts but slowed down screen updates. Users discovered that reconfiguring PB5 as an output tricked the system into thinking it was always in a vertical blank interval, bypassing the delay.  The BASIC command to do this was:

```basic
POKE 59458,62
```

However, on later PET models, driving PB5 in contention sufficiently loaded the VERT signal to causing the CRT drive circuitry to behave strangely.  This created concern that older PET programs using this trick could damage the machine.

## Mitigation

In this our open hardware design, the VERT signal is buffered by the level shifter and easily wins against the VIA.  Our primary concern is damage to the VIA from prolonged contention.

The W65C22N datasheet notes that the 'N' variant has built-in current limiting resistors, but does not indicate their size or if they are intended to protect against transient shorts.

I asked about this in a Facebook group, and received this reply from Bill Mensch:

> The "N" version was created to be similar to the NMOS replacement version for IO voltage s for TTL compatibility and pull-up resistor characteristics. Shorting out the pins is never a good idea, that said this design is very forgiving.

Based on the above, I've added a 470 ohm series resistor to relieve the stress on the VIA and further attenuate any effect on the VERT signal.

## Reference

* [André Fachat's article](http://www.6502.org/users/andre/petindex/poke/index.html)
* [Tech Tangent video](https://www.youtube.com/watch?v=7bMJ0NIuWU0)
* [Wikipedia article](https://en.wikipedia.org/wiki/Killer_poke#Commodore_PET)
