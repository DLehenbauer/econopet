---
description: 'SystemVerilog coding conventions and patterns for FPGA gateware development'
applyTo: 'gw/**/*.sv, gw/**/*.v'
---

# Gateware Development Guidelines

Instructions for developing SystemVerilog gateware for the Efinix FPGA.

## Module Organization

| Module | Purpose |
|--------|---------|
| `top.sv` | Normalizes hardware signals (inverts active-low pins) |
| `main.sv` | Contains core logic with active-high internal signals |

## Signal Conventions

- Use active-high signals internally
- The `top` module inverts active-low hardware pins at the boundary

## Testbenches

- Name testbenches with `*_tb.sv` suffix
- Place testbenches in [gw/EconoPET/sim/](gw/EconoPET/sim/)

## Development Workflow

1. `ctest --preset gw` - run Verilog simulations (Icarus Verilog)
2. Only if tests pass: `cmake --build --preset gw` - build bitstream

**Critical:** Never start gateware build when simulation tests are failing. The gateware build is slow (~2 min) and simulation catches most issues.

## External Dependencies

The `/gw/EconoPET/external` directory contains git submodules. Do not modify files in this path without explicit confirmation. When changes seem to require modifying submodule code:

1. Propose alternatives first (wrappers, adapters, compile flags)
2. Request confirmation before proceeding
