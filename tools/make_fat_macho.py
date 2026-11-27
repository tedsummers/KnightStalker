#!/usr/bin/env python3
"""Combine one or more Mach-O binaries into a macOS Universal (fat) Mach-O binary."""

import struct
import sys

FAT_MAGIC = 0xCAFEBABE
MH_MAGIC_64 = 0xFEEDFACF
ALIGN_BITS = 14  # 16 KiB alignment (required for arm64 slices on Apple Silicon)
ALIGN_BYTES = 1 << ALIGN_BITS


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <output> <slice1> <slice2> [...]", file=sys.stderr)
        sys.exit(1)

    out_path = sys.argv[1]
    slice_paths = sys.argv[2:]

    slices = []
    for p in slice_paths:
        with open(p, "rb") as f:
            data = f.read()
        magic, cputype, cpusubtype = struct.unpack_from("<III", data, 0)
        if magic != MH_MAGIC_64:
            raise ValueError(f"{p} is not a 64-bit little-endian Mach-O binary (magic=0x{magic:08x})")
        slices.append((cputype, cpusubtype, data))

    header_size = 8 + 20 * len(slices)
    cur_off = (header_size + ALIGN_BYTES - 1) & ~(ALIGN_BYTES - 1)

    fat_arch_entries = []
    blobs = []
    for cputype, cpusubtype, data in slices:
        fat_arch_entries.append((cputype, cpusubtype, cur_off, len(data), ALIGN_BITS))
        blobs.append((cur_off, data))
        cur_off = (cur_off + len(data) + ALIGN_BYTES - 1) & ~(ALIGN_BYTES - 1)

    with open(out_path, "wb") as out:
        out.write(struct.pack(">II", FAT_MAGIC, len(slices)))
        for cputype, cpusubtype, offset, size, align in fat_arch_entries:
            out.write(struct.pack(">IIIII", cputype, cpusubtype, offset, size, align))
        for offset, data in blobs:
            pad = offset - out.tell()
            if pad > 0:
                out.write(b"\x00" * pad)
            out.write(data)

    import os
    os.chmod(out_path, 0o755)


if __name__ == "__main__":
    main()
