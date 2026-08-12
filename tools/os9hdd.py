#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Build/extract EconoPET .hdd files: flat Super-OS/9 hard-disk REL containers
# (format: see fw/src/ieee/diskimage.h, diskimage_type_hdd).
#
# Commands:
#   extract <image.d80> <out.hdd> [name]   pull a REL container out of a d80
#                                          (default name "OS9 DRIVE A")
#   fromdsk <rbf.dsk> <out.hdd>            wrap a raw 256-byte-sector RBF
#                                          image (e.g. ToolShed 'os9 format')
#   todsk   <in.hdd> <out.dsk>             unwrap back to raw RBF sectors
#   info    <in.hdd>                       print LSN0 summary

import sys

MAX_TOT = 32766          # stock CbmDsk passes 16-bit LSNs (rec = 2*LSN+1)
PAD = b'\x0d'            # record pad byte, as written by FORMAT.OS/9


def d80_offset(track, sector):
    def spt(t):
        return 29 if t <= 39 else 27 if t <= 53 else 25 if t <= 64 else 23
    off = 0
    for t in range(1, track):
        off += spt(t) * 256
    return off + sector * 256


def d80_extract_rel(data, want):
    track, sector = 39, 1
    start = None
    while track:
        blk = data[d80_offset(track, sector):d80_offset(track, sector) + 256]
        for i in range(8):
            e = blk[2 + i * 32: 2 + i * 32 + 30]
            if e[0] & 0x80 and (e[0] & 7) == 4:          # closed REL
                name = e[3:19].rstrip(b'\xa0').decode('latin1')
                if name.upper() == want.upper():
                    start = (e[1], e[2])
        track, sector = blk[0], blk[1]
    if start is None:
        raise SystemExit(f"REL file '{want}' not found")
    track, sector = start
    raw = b''
    while track:
        blk = data[d80_offset(track, sector):d80_offset(track, sector) + 256]
        nt, ns = blk[0], blk[1]
        raw += blk[2:256] if nt else blk[2:2 + blk[1] - 1]
        track, sector = nt, ns
    if len(raw) % 258:
        raise SystemExit(f"container is {len(raw)} bytes -- not whole sector-record pairs")
    return raw


def sectors_to_records(sectors):
    out = bytearray()
    for i in range(0, len(sectors), 256):
        sec = sectors[i:i + 256].ljust(256, b'\0')
        out += sec[0:128] + PAD + sec[128:256] + PAD
    return bytes(out)


def records_to_sectors(records):
    out = bytearray()
    for i in range(0, len(records), 258):
        pair = records[i:i + 258]
        out += pair[0:128] + pair[129:257]
    return bytes(out)


def lsn0_info(hdd):
    l0 = records_to_sectors(hdd[:258])
    tot = int.from_bytes(l0[0:3], 'big')
    name = bytes(c & 0x7f for c in l0[0x1f:0x3f].split(b'\0')[0]).decode('latin1', 'replace')
    return tot, name


def main():
    if len(sys.argv) < 3 or (sys.argv[1] in ("extract", "fromdsk", "todsk") and len(sys.argv) < 4):
        raise SystemExit("see header for usage")
    cmd = sys.argv[1]
    if cmd == 'extract':
        data = open(sys.argv[2], 'rb').read()
        name = sys.argv[4] if len(sys.argv) > 4 else 'OS9 DRIVE A'
        raw = d80_extract_rel(data, name)
        open(sys.argv[3], 'wb').write(raw)
        tot, vol = lsn0_info(raw)
        print(f"{sys.argv[3]}: {len(raw)} bytes, {len(raw)//258} sectors, "
              f"DD.TOT={tot}, volume '{vol}'")
    elif cmd == 'fromdsk':
        sectors = open(sys.argv[2], 'rb').read()
        if len(sectors) % 256:
            raise SystemExit("dsk is not whole 256-byte sectors")
        if len(sectors) // 256 > MAX_TOT:
            raise SystemExit(f"{len(sectors)//256} sectors exceeds the stock "
                             f"driver's 16-bit LSN ceiling ({MAX_TOT})")
        hdd = sectors_to_records(sectors)
        open(sys.argv[3], 'wb').write(hdd)
        tot, vol = lsn0_info(hdd)
        print(f"{sys.argv[3]}: {len(hdd)} bytes, {len(sectors)//256} sectors, "
              f"DD.TOT={tot}, volume '{vol}'")
        if tot > len(sectors) // 256:
            print(f"WARNING: DD.TOT={tot} exceeds the image's {len(sectors)//256} sectors")
        if tot > MAX_TOT:
            print(f"WARNING: DD.TOT={tot} exceeds the 16-bit LSN ceiling ({MAX_TOT})")
    elif cmd == 'todsk':
        hdd = open(sys.argv[2], 'rb').read()
        if len(hdd) % 258:
            raise SystemExit("hdd is not whole 258-byte record pairs")
        open(sys.argv[3], 'wb').write(records_to_sectors(hdd))
        print(f"{sys.argv[3]}: {len(hdd)//258} sectors")
    elif cmd == 'info':
        hdd = open(sys.argv[2], 'rb').read()
        tot, vol = lsn0_info(hdd)
        print(f"{sys.argv[2]}: {len(hdd)} bytes, {len(hdd)//258} sectors, "
              f"DD.TOT={tot}, volume '{vol}'")
    else:
        raise SystemExit(f"unknown command '{cmd}'")


if __name__ == '__main__':
    main()
