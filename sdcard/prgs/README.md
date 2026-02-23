# High-Speed Virtual Datasette

Place `.prg` files in this directory on the SD card to make them available
for loading on the PET.

## Usage

Use the standard BASIC `LOAD` command to load a program:

```
LOAD "FILENAME"
```

If a matching `filename.prg` file is found on the SD card, it will be loaded.
If no match is found, the normal tape loading routine will run (showing "PRESS PLAY ON TAPE").

Wildcard prefix matching is supported with `*`:

```
LOAD "GA*"
```

This loads the first file whose name starts with "GA" (for example,
`game.prg`).

Filename matching is case-insensitive. Do not include the `.prg` extension in the `LOAD` command.

Note that `LOAD` without a filename or `LOAD "*"` will always run the normal tape loading routine.

## How It Works

When you type `LOAD`, the EconoPET intercepts the KERNAL's load dispatch before
it reaches the tape routines. If a matching `.prg` file is found on the SD
card, the MCU pauses the CPU, reads the file from the SD card, and copies
the program data directly into SRAM at the load address specified in the PRG
header. It then resumes the CPU at the end of the KERNAL's tape loading
routine, so BASIC sees a normal completed load.
