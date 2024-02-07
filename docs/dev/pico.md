# RP2040

## DVI

EconoPET uses the same DVI pinout as 'micromod_cfg' (See https://www.sparkfun.com/products/17718)

```cpp
static const struct dvi_serialiser_cfg micromod_cfg = {
	.pio = DVI_DEFAULT_PIO_INST,
	.sm_tmds = {0, 1, 2},
	.pins_tmds = {18, 20, 22},
	.pins_clk = 16,
	.invert_diffpairs = true
};
```
