# config.yaml

The `config.yaml` file describes the startup choices shown by the boot menu and the configuration steps taken when the user selects a configuration.

> **⚠️ Important — please read before customizing `config.yaml`:**
>
> - **Firmware updates overwrite `config.yaml`.** If you have customized the file, be sure to back it up before applying a firmware update.
> - **The format is not stable.** It is likely to change in future firmware revisions. Do not expect forward or backward compatibility between versions.
> - **The YAML parser is custom and does not support all YAML features.** Stick to the constructs you see used in the shipped `config.yaml`.

YAML is a general file format for serializing a tree of data. It is designed to be easy for people to read and edit. If you are new to YAML, the following references are helpful:

- [YAML Basic Syntax - Learn YAML Fundamentals | YAML.cc](https://yaml.cc/tutorial/basic-syntax.html) — a gentle introduction that builds up the syntax one concept at a time.
- [Learn YAML in Y Minutes](https://learnxinyminutes.com/yaml/) — a concise tour of the language with small examples of each feature.
- [YAML Ain't Markup Language (YAML™) revision 1.2.2](https://yaml.org/spec/1.2.2/#chapter-2-language-overview) — the official specification, for when you need the authoritative details.

## Top-level settings

The root of the YAML file has two properties:

| Property | Meaning |
| --- | --- |
| `default` | Optional. The `id` of the configuration to use on power-on. If omitted, the boot menu is shown at power-on. |
| `configs` | A list of configurations to show in the boot menu. |

## Configurations

Each item in `configs` describes one boot menu option:

| Property | Meaning |
| --- | --- |
| `id` | A unique string that identifies the configuration. This is what `default` refers to. |
| `name` | A human-readable name to show in the boot menu. |
| `setup` | A list of actions to perform when this configuration is selected. |

## Setup actions

Each item in a `setup` list is an action. The `action` property tells the
firmware what kind of action to perform:

| Action | Meaning |
| --- | --- |
| `load` | Copies a list of binary files from the SD card to memory addresses. This is used to initialize ROMs. |
| `patch` | Overwrites a range of bytes in memory with a hex string. This is used to overlay patches over ROMs. |
| `copy` | Copies a range of bytes in memory from one address to another. This is also used for patching ROMs. |
| `fix-checksum` | Modifies the byte at the target address so memory matches the desired Commodore checksum after patching. |
| `set` | Configures firmware options. |

Setup lists may also contain `if` / `then` / `else` entries. These are used when
a configuration needs different setup steps for different hardware, such as
loading different ROMs for the graphics and business keyboards.

## Firmware options

The `set` action can configure these firmware options:

| Option | Values | Meaning |
| --- | --- | --- |
| `columns` | `80` or `40` | Selects the screen width. |
| `video-ram-kb` | `1`, `2`, `3`[^vram-3], `4`, or `8`[^vram-8] | Selects the amount of video RAM. |
| `usb-keymap` | File name | Names the SD card file containing the USB HID code to PET keyboard matrix mapping. |
| `tape` | ROM-specific bytes | Tells the firmware how to intercept `LOAD` commands for the virtual tape drive. |

[^vram-3]: `video-ram-kb: 3` activates the experimental ColourPET 40-column mode.

[^vram-8]: On models with more than 4 KB of video RAM, the upper 4 KB is "write only": CPU writes and CRTC reads to `$9000-$9FFF` target video RAM, while CPU reads in this range return the option ROM.

## Example: *Attack of the PETSCII Robots*

The full version of [*Attack of the PETSCII Robots*](https://www.the8bitguy.com/product/petscii-robots/)
ships with a custom character ROM that replaces the stock PET font with the
graphics tiles used by the game. This example walks through adding a new boot
menu entry that loads that custom character ROM alongside the standard BASIC 4
ROMs for a 40-column PET.

### 1. Back up your existing `config.yaml`

Firmware updates overwrite `config.yaml`, so make a copy before editing:

```
/config.yaml          ← the live file
/config.backup.yaml   ← your copy
```

### 2. Copy the custom character ROM onto the SD card

Place the custom character ROM from the PETSCII Robots download onto the SD
card under `/roms/`. For this example we'll assume the file is named:

```
/roms/CHAR-ROM.bin
```

### 3. Pick an existing configuration to base the new entry on

The shipped `config.yaml` already contains a configuration named
`pet-40xx-60hz` ("PET 40xx (40 Col 60 Hz)") that loads BASIC 4, the editor
ROM, the kernal, and the standard character ROM. The only thing we need to
change is the character ROM file loaded at address `0x68000`.

### 4. Add a new configuration to `configs`

Open `config.yaml` and add a new entry to the `configs` list. The new entry
should be a sibling of the existing `- id: pet-40xx-60hz` block. Indentation
matters in YAML — line the new `- id:` up with the other `- id:` entries.

```yaml
  - id: pet-40xx-60hz-robots
    name: "PET 40xx + PETSCII Robots font"
    setup:
      - action: "load"
        files:
          - address: 0xb000
            file: "/roms/basic-4-b000.901465-23.bin"
          - address: 0xc000
            file: "/roms/basic-4-c000.901465-20.bin"
          - address: 0xd000
            file: "/roms/basic-4-d000.901465-21.bin"
          - address: 0xe000
            file: "/roms/edit-4-40-n-60Hz.901499-01.bin"
          - address: 0xf000
            file: "/roms/kernal-4.901465-22.bin"
          - address: 0x68000
            file: "/roms/CHAR-ROM.bin"   # Custom character ROM
      - action: "set"
        usb-keymap: "/ukm/us.bin"
        video-ram-kb: 1
        tape: "2ef415f4c9cad1d4da"
```

The only change from `pet-40xx-60hz` is the `file:` value at address
`0x68000`, which now points at the PETSCII Robots character ROM.

### 5. Save and reboot

Save `config.yaml` back to the SD card, reinsert it, and power-on the PET. The
new entry should now appear in the boot menu. Loading a BASIC program from the
game will now use the custom font.

**Tip:** If the new entry does not appear, or the machine fails to boot,
restore `config.backup.yaml` and re-check your indentation.
