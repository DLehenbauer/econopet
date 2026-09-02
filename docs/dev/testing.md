# Test randomization

`ctest` picks one pseudo-random seed per run and shares it with every test, so
a run that trips a seed-sensitive bug can be replayed in full.

## How it works

CTest runs `seed.sh --write` as a pre-test hook (rather than a fixture) so the
seed prints even when every test passes. The hook picks a seed, prints it as
`Test seed: N`, and writes it to `build/test.seed`. Each test is handed that
path in `ECONOPET_TEST_SEED_FILE`.

`sim.sh` and `verilate.sh` call `seed.sh` to resolve the effective seed
(`ECONOPET_TEST_SEED` wins over the file) and pass it to the simulator as a
`+seed=N` plusarg. The `TB_INIT` macro in `gw/EconoPET/sim/tb.svh` reads the
plusarg and feeds it to `$urandom`. Verilator additionally gets
`+verilator+seed+N`, which also drives `RAND_RESET=2` power-up randomization
(see [verilator.md](verilator.md)).

The firmware tests receive `ECONOPET_TEST_SEED_FILE` as well, though no
firmware test consumes it yet.

## Replaying a run

Each bench echoes the seed it used, so `--output-on-failure` shows the value
alongside the failure. Set `ECONOPET_TEST_SEED` to replay:

```sh
ECONOPET_TEST_SEED=123456789 ctest --preset gw --parallel   # whole run
ECONOPET_TEST_SEED=123456789 ./verilate.sh top_tb           # one Verilator bench
ECONOPET_TEST_SEED=123456789 ./sim.sh top_tb                # one Icarus bench
```

The two simulators produce different values from the same seed, so a seed
reproduces a given bench under a given simulator, not across both.

Simulators accept `1..2147483647`. A seed outside that range fails the pre-test
hook and no tests run.

## Caveats

- Each `ctest` run draws a fresh seed, so a bench with a latent seed-sensitive
  bug presents as flaky. The reported seed is the reproducer.
- Running a bench directly (outside CTest, without `ECONOPET_TEST_SEED`) passes
  no seed at all, so the simulator's own default applies. Those runs repeat but
  do not report a seed.
- Icarus has no global RNG seed and its `$random()` stream ignores `$urandom`
  seeding, so benches must use `$urandom`/`$urandom_range` to be reproducible.
- The seed only affects benches that call `$urandom`/`$urandom_range` or run
  with `RAND_RESET=2`. Benches that draw no random values are unaffected.
