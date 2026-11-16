# Virtual IEEE-488 Disk Drive

## Supported Images

Place D64, D80, and D82 disk images in this directory (`/disk` on the SD card).

Images are writable while mounted. To preserve an original, keep a backup
outside the SD card.

## Mounting an Image

1. Start the desired configuration (for example, PET 2001).
2. Open the EconoPET menu by holding the menu button or both Shift keys.
3. Select `Change Disk`.
4. Select a device and drive slot (typically device 8, drive 0).
5. Browse to the disk image and select it.
6. Select `Back` twice to resume the running configuration.

The image is now available at the selected device and drive.

## Listing and Loading Programs

At the BASIC prompt, list the directory on device 8, drive 0:

```basic
LOAD "$0",8
LIST
```

To load and run a BASIC program, replace `PROGRAM` with its name:

```basic
LOAD "0:PROGRAM",8
RUN
```

For another slot, replace `8` with the device number and `0` with the drive
number.

## Using the Physical IEEE-488 Port

Mounting any image disables access to all devices on the physical IEEE-488
port, even those at other device addresses.

To re-enable the port, open `Change Disk`, select each occupied slot, and
choose `[Eject]`. The port becomes available when all images are ejected.

## Automatic Mounting

To mount an image whenever a configuration starts, add a `mount` action to
that configuration's `setup` list in `config.yaml`:

```yaml
      - action: "mount"
        device: 8
        drive: 0
        file: "mydisk.d80"
```

The `file` path is relative to `/disk`. Devices 8 through 11 and drives 0
and 1 are available.

See [CONFIG-YAML.md](../CONFIG-YAML.md) for configuration details.
