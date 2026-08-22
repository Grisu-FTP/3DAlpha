#!/usr/bin/env python3
"""Read, dump and semantically compare NBT files.

    tools/nbtdiff.py dump     <file>
    tools/nbtdiff.py diff     <a> <b>       [--ignore NAME ...]
    tools/nbtdiff.py difftree <dirA> <dirB> [--ignore NAME ...]

`difftree` walks two world folders and compares every *.dat in them, which is
the world round-trip test: load a real save, write it back, and require that
nothing changed.

Files may be gzip, zlib or raw; the wrapper is detected. Works on both Alpha
chunk files and level.dat.

This is written from the NBT specification rather than from src/core/nbt, on
purpose. Round-tripping a file through our own reader and writer only proves
they agree with each other. Checking the bytes with a separate implementation
is what proves they agree with the format -- and the same script is what the
world round-trip test uses to require semantic equality against a real save.

Comparison ignores key order within compounds, because the original game stores
compounds in a HashMap and writes them in hash order. It does not ignore list
order, which is significant.
"""

import gzip
import os
import struct
import sys
import zlib

END, BYTE, SHORT, INT, LONG, FLOAT, DOUBLE, BYTE_ARRAY, STRING, LIST, COMPOUND, \
    INT_ARRAY, LONG_ARRAY = range(13)

TYPE_NAMES = {
    END: "End", BYTE: "Byte", SHORT: "Short", INT: "Int", LONG: "Long",
    FLOAT: "Float", DOUBLE: "Double", BYTE_ARRAY: "ByteArray", STRING: "String",
    LIST: "List", COMPOUND: "Compound", INT_ARRAY: "IntArray",
    LONG_ARRAY: "LongArray",
}

SCALARS = {
    BYTE: ">b", SHORT: ">h", INT: ">i", LONG: ">q", FLOAT: ">f", DOUBLE: ">d",
}


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, count):
        if self.pos + count > len(self.data):
            raise ValueError(f"truncated at offset {self.pos}")
        chunk = self.data[self.pos:self.pos + count]
        self.pos += count
        return chunk

    def scalar(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]

    def string(self):
        # Java modified UTF-8. Decoded leniently: key comparison only needs the
        # ASCII range, and refusing to read a file over a stray byte in a sign
        # would make this useless for diagnosing exactly that.
        return self.take(self.scalar(">H")).decode("utf-8", errors="replace")

    def value(self, tag):
        if tag in SCALARS:
            return self.scalar(SCALARS[tag])
        if tag == BYTE_ARRAY:
            return self.take(self.scalar(">i"))
        if tag == STRING:
            return self.string()
        if tag == INT_ARRAY:
            return [self.scalar(">i") for _ in range(self.scalar(">i"))]
        if tag == LONG_ARRAY:
            return [self.scalar(">q") for _ in range(self.scalar(">i"))]
        if tag == LIST:
            element = self.scalar(">b")
            count = self.scalar(">i")
            if element == END and count != 0:
                raise ValueError("list of TAG_End with a non-zero count")
            return [self.value(element) for _ in range(count)]
        if tag == COMPOUND:
            out = {}
            while True:
                child = self.scalar(">b")
                if child == END:
                    return out
                name = self.string()
                if name in out:
                    raise ValueError(f"duplicate tag {name!r} in one compound")
                out[name] = (child, self.value(child))
        raise ValueError(f"unknown tag type {tag}")


def unwrap(raw):
    if raw[:2] == b"\x1f\x8b":
        return gzip.decompress(raw)
    try:
        return zlib.decompress(raw)
    except zlib.error:
        return raw


def load(path):
    with open(path, "rb") as handle:
        reader = Reader(unwrap(handle.read()))
    tag = reader.scalar(">b")
    if tag != COMPOUND:
        raise ValueError("root tag is not a compound")
    name = reader.string()
    value = reader.value(COMPOUND)
    if reader.pos != len(reader.data):
        raise ValueError(f"{len(reader.data) - reader.pos} trailing bytes")
    return name, value


def describe(tag, value):
    if tag == BYTE_ARRAY:
        return f"{len(value)} bytes"
    if tag in (INT_ARRAY, LONG_ARRAY, LIST):
        return f"{len(value)} entries"
    if tag == COMPOUND:
        return f"{len(value)} tags"
    return repr(value)


def dump(tag, value, name="", indent=0):
    pad = "  " * indent
    print(f"{pad}{TYPE_NAMES[tag]:<10} {name:<20} {describe(tag, value)}")
    if tag == COMPOUND:
        for key in sorted(value):
            dump(value[key][0], value[key][1], key, indent + 1)
    elif tag == LIST and value and isinstance(value[0], dict):
        for i, element in enumerate(value):
            dump(COMPOUND, element, f"[{i}]", indent + 1)


def compare(a, b, ignore, path="", out=None):
    out = [] if out is None else out
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key in ignore:
                continue
            where = f"{path}/{key}"
            if key not in a:
                out.append(f"+ {where} (only in the second file)")
            elif key not in b:
                out.append(f"- {where} (only in the first file)")
            else:
                (ta, va), (tb, vb) = a[key], b[key]
                if ta != tb:
                    out.append(f"~ {where}: {TYPE_NAMES[ta]} vs {TYPE_NAMES[tb]}")
                else:
                    compare(va, vb, ignore, where, out)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"~ {path}: {len(a)} vs {len(b)} entries")
        else:
            for i, (x, y) in enumerate(zip(a, b)):
                compare(x, y, ignore, f"{path}[{i}]", out)
    elif a != b:
        out.append(f"~ {path}: {describe(0, a)[:60]} vs {describe(0, b)[:60]}")
    return out


def walk_dat(root):
    """Relative paths of every *.dat under root, sorted for stable output."""
    found = []
    for directory, _, files in os.walk(root):
        for name in files:
            if name.endswith(".dat"):
                full = os.path.join(directory, name)
                found.append(os.path.relpath(full, root))
    return sorted(found)


def difftree(a, b, ignore):
    only_a = set(walk_dat(a))
    only_b = set(walk_dat(b))
    shared = sorted(only_a & only_b)

    problems = 0
    for name in sorted(only_a - only_b):
        print(f"- {name} (missing after)")
        problems += 1
    for name in sorted(only_b - only_a):
        print(f"+ {name} (appeared after)")
        problems += 1

    identical = 0
    for name in shared:
        try:
            differences = compare(load(os.path.join(a, name))[1],
                                  load(os.path.join(b, name))[1], ignore)
        except (ValueError, OSError) as error:
            print(f"! {name}: {error}")
            problems += 1
            continue
        if differences:
            problems += 1
            print(f"~ {name}")
            for line in differences[:8]:
                print(f"    {line}")
            if len(differences) > 8:
                print(f"    ... and {len(differences) - 8} more")
        else:
            identical += 1

    print(f"\n{identical}/{len(shared)} files semantically identical, "
          f"{problems} problem(s)")
    return 1 if problems else 0


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == "dump":
        name, value = load(args[1])
        print(f'root "{name}"')
        dump(COMPOUND, value, "", 1)
        return 0

    if len(args) >= 3 and args[0] in ("diff", "difftree"):
        ignore = set()
        rest = args[3:]
        while rest:
            if rest[0] == "--ignore" and len(rest) > 1:
                ignore.add(rest[1])
                rest = rest[2:]
            else:
                sys.exit(f"nbtdiff: unexpected argument {rest[0]!r}")

        if args[0] == "difftree":
            return difftree(args[1], args[2], ignore)

        differences = compare(load(args[1])[1], load(args[2])[1], ignore)
        for line in differences:
            print(line)
        if differences:
            print(f"{len(differences)} difference(s)")
            return 1
        print("semantically identical")
        return 0

    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
