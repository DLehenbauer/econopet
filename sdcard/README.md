# EconoPET Release Notes

For the user manual, firmware updates, and more, visit the [project page](https://dlehenbauer.github.io/econopet).

## Updating Firmware

1. Copy the contents of this archive to a FAT32-formatted microSD card.
2. Insert the microSD card into the EconoPET board.
3. Power on. The firmware will update automatically.

For detailed instructions, see the [user manual](https://dlehenbauer.github.io/econopet).

## Changelog

### v0.2.0 (2026-02-22)

- High-Speed Virtual Datasette (see `/prgs/README.md`)
- HDMI output now supports limited CRTC emulation (R1, R6, R9, R12-R13)
- Optionally load a default configuration when powered on (set `default:` in `config.yaml`)
- ColourPET preview (RGBI output on HDMI, color buffer starts at $8800)

### v0.1.0 (2025-08-29)

- Minimal viable product

## License

This project is released under the [CC0 1.0 Universal](LICENSE) (CC0) license, placing it in the public domain.

**Exception:** external dependencies are subject to their own licenses as noted in their respective source code and in [NOTICE.md](NOTICE.md).
