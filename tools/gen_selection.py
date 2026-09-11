#!/usr/bin/env python3
"""MAINTAINER TOOL -- PLAYERS NEVER RUN THIS, AND NEITHER DOES THE BUILD.

    tools/gen_selection.py [--check]

Turns tests/collision_box_vectors.hpp -- which came out of a running a1.1.2 jar
via tools/genref.java --collision -- into data/<version>/selection.json, the
table the build compiles into .rodata.

**Why a derived file rather than a hand-written one.** The selection box is the
shape Block.collisionRayTrace tests a ray against, and it is not the collision
box: a torch has no collision box at all and still has a selection box, which is
why a torch can be aimed at and broken but not stood on. There are eighteen
families across the seventy blocks, seven of which change with metadata, and
transcribing those by hand is exactly the kind of work that produces a wrong
number nobody notices for a year. So nothing is transcribed: the fixture is the
source, this reduces it, and tests/collision_box_test.cpp checks the compiled
table back against the same fixture.

The reduction is a de-duplication and nothing more. 1,120 (id, metadata) pairs
hold only 40 distinct boxes, so the table ships as those 40 plus a byte index --
about 5 KB rather than 53 KB, which matters on a console with 40 MB of heap.
Every bound is a float in the jar (setBlockBounds takes floats), so storing them
as floats is lossless and the script refuses to emit one that is not.
"""

import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests" / "collision_box_vectors.hpp"
OUT = ROOT / "data" / "a1.1.2" / "selection.json"
PLACEMENT_FIXTURE = ROOT / "tests" / "placement_vectors.hpp"
PLACEMENT_OUT = ROOT / "data" / "a1.1.2" / "placement.json"


def load():
    """The three per-(id, metadata) columns the fixture carries.

    `kTargetable` and `kTargetableWithLiquids` are emitted as two runs of bare
    `true,`/`false,` lines, in that order and both one flag per box, so the flat
    scan is split down the middle rather than matched per array.

    **The liquids column varies with metadata and the other does not.**
    `canCollideCheck(meta, false)` is `isCollidable()` for every block this
    version constructs, which cannot vary; `canCollideCheck(meta, true)` is
    `hitLiquids && meta == 0` for water and lava, which does. So one collapses
    to a bool per block and the other keeps all sixteen bits as a mask. The
    script asserts the first of those rather than assuming it.
    """
    text = FIXTURE.read_text(encoding="utf-8")
    rows = re.findall(r"^    \{([^}]*)\},   // (\d+) meta (\d+)$", text, re.M)
    allflags = re.findall(r"^    (true|false),$", text, re.M)
    if len(allflags) != 2 * len(rows):
        sys.exit(f"{FIXTURE.name}: {len(rows)} selection boxes but {len(allflags)} flags; "
                 "expected two columns of one flag per box")
    flags = allflags[:len(rows)]
    liquid_flags = allflags[len(rows):]

    shapes = []
    index = {}
    targetable = {}
    liquid_mask = {}
    for (values, block, meta), flag, liquid in zip(rows, flags, liquid_flags):
        liquid_mask.setdefault(block, 0)
        if liquid == "true":
            liquid_mask[block] |= 1 << int(meta)
        box = [float(v) for v in values.split(", ")]
        for v in box:
            if struct.unpack("f", struct.pack("f", v))[0] != v:
                sys.exit(f"block {block} metadata {meta}: {v} does not survive a float")
        if box not in shapes:
            shapes.append(box)
        index.setdefault(block, [0] * 16)[int(meta)] = shapes.index(box)
        if block in targetable and targetable[block] != (flag == "true"):
            sys.exit(f"block {block}: canCollideCheck(meta, false) varies with metadata, "
                     "and it is stored as one bool per block")
        targetable[block] = flag == "true"
    return shapes, index, targetable, liquid_mask


def load_placement():
    """Which metadata a block lands with, per face struck.

    The fixture sweeps sixteen player headings as well, and **no block answers
    differently for any of them**: a1.1.2 has no `Block.onBlockPlacedBy`, so a
    placement sees the struck face and nothing else. (The one thing that does
    read the heading is `ItemDoor.onItemUse`, which is in the item and not in
    the block; core/item/use.cpp carries it.) Column 0 of each face is
    therefore the whole answer rather than a sample of it.

    The lever's top face used to look heading-dependent here. It was not: the
    sweep punched each block after placing it, which flips a lever, and
    `BlockLever.onBlockAdded` rolls `nextInt(2)` for which way a floor lever
    lies. Both are fixed in the fixture's own generator. See
    docs/physics-a1.1.2.md.

    **Only a block with an onBlockPlaced gets a row.** The sweep measures
    onBlockAdded and onBlockPlaced together, against a single stone cube, and a
    furnace and a staircase vary by face there only because onBlockAdded turns
    them from that one neighbour. Neither class overrides onBlockPlaced, so
    their rows are zero -- a1.1.2 writes them with metadata 0 -- and the
    orientation is the tick behaviour's (`tick::blockAdded`), which reads the
    real neighbours rather than assuming there is only one. Which classes do
    override it comes from the fixture, which asked the jar.
    """
    text = PLACEMENT_FIXTURE.read_text(encoding="utf-8")
    hooks = re.search(r"kPlacementHooks\[kPlacementHookCount\] = \{([^}]*)\};", text)
    if hooks is None:
        sys.exit(f"{PLACEMENT_FIXTURE} lists no onBlockPlaced overrides; regenerate it")
    hooked = {block.strip() for block in hooks.group(1).split(",") if block.strip()}
    rows = re.findall(r"^    \{(\d+), \{(.*)\}\},", text, re.M)
    table = {}
    for block, body in rows:
        faces = re.findall(r"\{([^{}]*)\}", body)
        if len(faces) != 6:
            sys.exit(f"block {block}: {len(faces)} faces in the placement fixture, expected 6")
        if block not in hooked:
            table[block] = [0] * 6
            continue
        # Heading 0 of each face. -1 means the block deleted itself there; the
        # table stores 0, because a placement our code refuses never reaches it.
        table[block] = [max(int(f.split(", ")[0]), 0) for f in faces]
    return table


def main():
    shapes, index, targetable, liquid_mask = load()
    doc = {
        "$comment": [
            "GENERATED by tools/gen_selection.py from tests/collision_box_vectors.hpp.",
            "Do not edit by hand -- regenerate instead. See that file's header for where",
            "the numbers came from (a running a1.1.2 jar) and this script's for why they",
            "are de-duplicated.",
            "`shapes` are minX minY minZ maxX maxY maxZ in block-local coordinates, so a",
            "full cube is 0,0,0 -> 1,1,1. `index` maps a block id to its sixteen metadata",
            "values' shape indices. `targetable` is Block.canCollideCheck, which in a1.1.2",
            "is isCollidable(): false only for water, lava and fire.",
            "`targetableLiquids` is that same call with hitLiquids true -- the flag a bucket",
            "passes when it looks for something to scoop -- as a sixteen-bit mask over",
            "metadata, because BlockFluid answers it with `hitLiquids && metadata == 0` and",
            "so is the one column here that varies inside a block.",
        ],
        "version": "a1.1.2",
        "shapes": shapes,
        "index": index,
        "targetable": targetable,
        "targetableLiquids": liquid_mask,
    }
    text = json.dumps(doc, indent=2, sort_keys=False) + "\n"
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            sys.exit(f"{OUT} is stale; run tools/gen_selection.py")
        print(f"{OUT.name} is up to date")
        placement = load_placement()
        expected = json.loads(PLACEMENT_OUT.read_text(encoding="utf-8"))["metadata"]
        if expected != placement:
            sys.exit(f"{PLACEMENT_OUT} is stale; run tools/gen_selection.py")
        print(f"{PLACEMENT_OUT.name} is up to date")
        return
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT} -- {len(shapes)} distinct boxes over {len(index)} blocks")

    placement = load_placement()
    doc = {
        "$comment": [
            "GENERATED by tools/gen_selection.py from tests/placement_vectors.hpp.",
            "Do not edit by hand -- regenerate instead.",
            "`metadata` maps a block id to the metadata it lands with for each face struck,",
            "in mc::mesh::Face order: 0 -Y, 1 +Y, 2 -Z, 3 +Z, 4 -X, 5 +X. This is",
            "Block.onBlockPlaced, which ItemBlock runs after setBlockWithNotify.",
            "Only the six blocks whose class overrides it have a non-zero row. A furnace",
            "and a staircase orient in onBlockAdded, from their neighbours, which is the",
            "tick behaviour's job and not this table's.",
            "a1.1.2 has no Block.onBlockPlacedBy, so no block orients from the player's",
            "heading. ItemDoor does, in the item.",
        ],
        "version": "a1.1.2",
        "metadata": placement,
    }
    ptext = json.dumps(doc, indent=2) + "\n"
    if PLACEMENT_OUT.exists() and PLACEMENT_OUT.read_text(encoding="utf-8") == ptext:
        print(f"{PLACEMENT_OUT.name} is up to date")
    else:
        PLACEMENT_OUT.write_text(ptext, encoding="utf-8")
        print(f"wrote {PLACEMENT_OUT} -- {len(placement)} blocks")


if __name__ == "__main__":
    main()
