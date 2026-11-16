# IEEE-488 Disk Drive

This directory contains disk images for the EconoPET IEEE-488 disk-drive
emulation. `.d64` and `.d80` images are supported.

To mount a disk image automatically, add an entry like the following to a
configuration in `config.yaml`. The `file` value is a path relative to
`/disk`:

```yaml
      - action: "mount"
        device: 8
        drive: 0
        file: "mydisk.d80"
```

Mounting an image enables the emulated IEEE-488 drives. See `CONFIG-YAML.md`
for the full configuration.

Disk image files are writable while mounted. To preserve an original image,
keep a copy outside the SD card.
