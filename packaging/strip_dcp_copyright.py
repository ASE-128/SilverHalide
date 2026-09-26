#!/usr/bin/env python3
"""Removes the Copyright tag (50942) from every DCP under resources/Camera.

Rewrites each profile as a byte-equivalent TIFF container without the
50942 entry: all other tags keep their raw values, IFD order and the
byte order / magic of the original file. Run from the repo root after
updating the bundled Adobe camera profiles:

    python packaging/strip_dcp_copyright.py
"""

import glob
import os
import struct
import sys

COPYRIGHT_TAG = 50942
TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 9: 4, 10: 8, 11: 4}


def parse(data):
    le = data[0:2] == b"II"
    bo = "<" if le else ">"
    magic = struct.unpack(bo + "H", data[2:4])[0]
    if magic not in (42, 0x4352):
        raise ValueError("not a TIFF/DCP container")
    ifds = []
    off = struct.unpack(bo + "I", data[4:8])[0]
    while off:
        count = struct.unpack(bo + "H", data[off:off + 2])[0]
        entries = []
        for i in range(count):
            e = off + 2 + i * 12
            tag, typ, cnt = struct.unpack(bo + "HHI", data[e:e + 8])
            raw = data[e + 8:e + 12]
            size = cnt * TYPE_SIZE.get(typ, 1)
            if size > 4:
                voff = struct.unpack(bo + "I", raw)[0]
                value = data[voff:voff + size]
                if len(value) != size:
                    raise ValueError(f"tag {tag}: truncated data")
            else:
                value = raw[:size]
            entries.append((tag, typ, cnt, value))
        next_off = struct.unpack(bo + "I", data[off + 2 + count * 12:
                                                off + 6 + count * 12])[0]
        ifds.append(entries)
        off = next_off
    return le, magic, ifds


def build(le, magic, ifds):
    bo = "<" if le else ">"
    out = bytearray()
    out += b"II" if le else b"MM"
    out += struct.pack(bo + "H", magic)
    out += struct.pack(bo + "I", 8)
    data_area = bytearray()
    # First pass sizes every IFD so data can be laid out after the last one.
    ifd_offsets = []
    cursor = 8
    for entries in ifds:
        ifd_offsets.append(cursor)
        cursor += 2 + len(entries) * 12 + 4
    data_base = cursor
    for idx, entries in enumerate(ifds):
        out += struct.pack(bo + "H", len(entries))
        for tag, typ, cnt, value in entries:
            out += struct.pack(bo + "HHI", tag, typ, cnt)
            if len(value) > 4:
                out += struct.pack(bo + "I", data_base + len(data_area))
                data_area += value
            else:
                out += value + b"\x00" * (4 - len(value))
        nxt = ifd_offsets[idx + 1] if idx + 1 < len(ifds) else 0
        out += struct.pack(bo + "I", nxt)
    out += data_area
    return bytes(out)


def main():
    root = os.path.join(os.path.dirname(__file__), "..", "resources", "Camera")
    root = os.path.normpath(root)
    files = sorted(glob.glob(os.path.join(root, "*", "*.dcp")))
    if not files:
        print("no DCP files found under", root)
        return 1
    stripped = skipped = 0
    for path in files:
        data = open(path, "rb").read()
        le, magic, ifds = parse(data)
        new_ifds = [[e for e in entries if e[0] != COPYRIGHT_TAG]
                    for entries in ifds]
        if all(len(a) == len(b) for a, b in zip(ifds, new_ifds)):
            skipped += 1
            continue
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(build(le, magic, new_ifds))
        os.replace(tmp, path)
        stripped += 1
    print(f"{stripped} files rewritten, {skipped} already clean "
          f"({len(files)} total)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
