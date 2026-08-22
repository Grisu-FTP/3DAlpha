#!/usr/bin/env python3
"""Recover the block table from an original Minecraft client jar.

    tools/extract_blocks.py <client.jar> [--json out.json]
    tools/extract_blocks.py <client.jar> --verify data/a1.1.2/blocks.json

MAINTAINER TOOL -- PLAYERS NEVER RUN THIS, AND NEITHER DOES THE BUILD.

Its output, data/<version>/blocks.json, is checked into the repository and
compiled into the binary as a constexpr table. The game ships knowing every
block's id, hardness, blast resistance, light level and render type, and is
fully playable with no jar anywhere in sight. The only thing a player may
optionally supply is *textures* (see docs/assets.md); game data is never
something we ask them to dump.

This runs once when a version's table is first built, and again when somebody
wants to re-check it. Both are maintainer jobs on a maintainer's machine.

Reads the obfuscated `Block` class with `javap` and parses its static
initialiser, which constructs every block in order:

    new           <class>
    dup
    bipush        7            <- block id
    bipush        17           <- texture index (atlas tile), when the ctor takes one
    getstatic     Material.x   <- material
    invokespecial <class>.<init>
    ldc -1.0f  invokevirtual setHardness
    ldc 6000000.0f invokevirtual setResistance
    ...
    putstatic     Block.A

Why bother: a hand-written block table looks right and is wrong in small
places, and every one of those becomes a rendering or physics bug that is
tedious to trace back. The jar is the only authority for which blocks a1.1.2
actually had, what their ids are, and what the engine was told about them.

**This reads a jar the maintainer already owns and copies nothing out of it.**
The output is factual data -- numeric ids, hardness values, material grouping --
of the same kind any wiki table carries. No Mojang code or asset is copied into
this repository, and the jar is never redistributed. See docs/assets.md.

The jar has no block *names*: setBlockName arrived after the alpha era, and
a1.1.2's Block class carries no strings at all. Names in
data/<version>/blocks.json are ours, and are the one column this tool cannot
check. `--verify` checks every other one, and is how a block table stays
trustworthy after somebody edits it by hand.

Textures are recovered differently, by *running* the bytecode rather than
matching it -- see the second half of this file and tools/javap.py. Reading the
static initialiser as a pattern cannot answer either question that matters for
rendering: a block that sets its own texture in its own constructor (grass,
logs, chests, doors) has no texture in the initialiser at all, and the dozen
classes that give each face a different tile do it with a branch on the face
index. Pattern matching also gets some of them actively *wrong*: where a
subclass constructor is `(int id, boolean flag)`, reading "the second int
argument" turns the flag into a texture index, which is how the furnace came to
be recorded with texture 0.

Two limitations worth knowing:

  * Only *constant-returning* overrides are recovered for the boolean and
    render-type columns. `BlockStep.isOpaqueCube` returns
    `blockID == doubleSlabId`, which is a branch, so the slab inherits the base
    `true` here and has to be corrected by hand. (Textures do not have this
    problem any more; they are interpreted.)
  * The obfuscator reuses method names across unrelated signatures, so methods
    are identified by behaviour rather than by name. getRenderType is found as
    the ()I method whose overrides spread across the small integers; the
    booleans carry no such signal and are all reported for a human to map.
"""

import json
import re
import subprocess
import sys
import zipfile

import javap

# The Block class is the one holding the step-sound names. They are the only
# strings in it, which is exactly why the block names have to come from us.
STEP_SOUND_MARKERS = (b"gravel", b"cloth", b"grass")

INSTRUCTION = re.compile(r"^\s*(\d+):\s+(\S+)\s*(\S*)\s*(?://\s*(.*))?$")

CONST_VALUES = {
    "iconst_m1": -1, "iconst_0": 0, "iconst_1": 1, "iconst_2": 2,
    "iconst_3": 3, "iconst_4": 4, "iconst_5": 5,
    "fconst_0": 0.0, "fconst_1": 1.0, "fconst_2": 2.0,
    "lconst_0": 0, "lconst_1": 1, "dconst_0": 0.0, "dconst_1": 1.0,
}


def find_class(jar_path, markers):
    """The class whose bytes contain every marker. Obfuscated names change
    between versions, so the class is identified by content, not by name."""
    hits = []
    with zipfile.ZipFile(jar_path) as jar:
        for entry in jar.namelist():
            if not entry.endswith(".class") or "/" in entry:
                continue
            blob = jar.read(entry)
            if all(marker in blob for marker in markers):
                hits.append((len(blob), entry))
    if not hits:
        sys.exit("extract_blocks: no class matched; is this an alpha-era client jar?")
    hits.sort(reverse=True)
    return hits[0][1]


def disassemble(workdir, class_name):
    result = subprocess.run(
        ["javap", "-p", "-c", "-constants", class_name],
        cwd=workdir, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"extract_blocks: javap failed\n{result.stderr}")
    return result.stdout


def parse_instructions(text):
    """The `static {}` body as (opcode, operand, comment) triples."""
    body = []
    inside = False
    for line in text.splitlines():
        if "static {}" in line:
            inside = True
            continue
        if not inside:
            continue
        match = INSTRUCTION.match(line)
        if match:
            body.append((match.group(2), match.group(3), (match.group(4) or "").strip()))
        elif line.strip().startswith("}") or (line and not line[0].isspace()):
            if body:
                break
    return body


def literal(opcode, operand, comment):
    """The value an instruction pushes, or None if it pushes nothing simple."""
    if opcode in CONST_VALUES:
        return CONST_VALUES[opcode]
    if opcode in ("bipush", "sipush"):
        return int(operand)
    if opcode == "ldc" or opcode == "ldc_w" or opcode == "ldc2_w":
        # Comments read "float 1.5f", "int 300", "String stone".
        parts = comment.split(None, 1)
        if len(parts) == 2:
            kind, value = parts
            if kind == "float" or kind == "double":
                return float(value.rstrip("fd"))
            if kind in ("int", "long"):
                return int(value.rstrip("lL"))
            if kind == "String":
                return value
    return None


def method_name_and_descriptor(comment):
    """"Method ly.a:(F)Lly;" -> ("a", "(F)Lly;").

    The obfuscator reuses one letter for unrelated methods -- `a:(F)Lly;` is
    setLightValue while `a:(Lbb;)Lly;` is setStepSound -- so the descriptor is
    part of the identity, not decoration.
    """
    body = comment.replace("Method ", "").strip()
    name, _, descriptor = body.partition(":")
    return name.split(".")[-1], descriptor


def parse_blocks(instructions):
    """Walk the initialiser, emitting one record per constructed block."""
    blocks = []
    pending = None

    for opcode, operand, comment in instructions:
        if opcode == "new":
            pending = {"class": comment.replace("class ", "").strip(),
                       "args": [], "material": None, "setters": [],
                       "constructed": False, "stack": []}
            continue

        if pending is None:
            continue

        if opcode == "getstatic":
            # Before the constructor this is the material; after it, the
            # argument to a chained setter (the step sound).
            field = comment.replace("Field ", "").strip()
            if pending["constructed"]:
                pending["stack"].append(field)
            elif pending["material"] is None:
                pending["material"] = field
            continue

        value = literal(opcode, operand, comment)
        if value is not None:
            pending["stack"].append(value)
            continue

        if opcode == "invokespecial" and "<init>" in comment:
            pending["args"] = pending["stack"]
            pending["stack"] = []
            pending["signature"] = method_name_and_descriptor(comment)[1]
            pending["constructed"] = True
            continue

        if opcode == "invokevirtual" and pending["constructed"]:
            name, descriptor = method_name_and_descriptor(comment)
            args = pending["stack"]
            pending["stack"] = []
            pending["setters"].append((f"{name}{descriptor}",
                                       args[0] if args else None))
            continue

        # checkcast sits between the last setter and putstatic when the field
        # is typed as the block's own subclass, which is why grass and the
        # flowers went missing the first time this ran.
        if opcode == "checkcast":
            continue

        if opcode == "putstatic":
            field = comment.replace("Field ", "").strip()
            if pending["constructed"] and pending["args"]:
                pending["field"] = field.split(":")[0]
                pending.pop("stack", None)
                blocks.append(pending)
            pending = None
            continue

    return blocks


# ------------------------------------------------------------------ textures
#
# Everything below runs bytecode instead of reading it. The whole block table is
# built by running Block's class initialiser, which constructs all 70 blocks in
# order -- so a block that reads another block's texture (the stairs take their
# model's, the crafting table asks the planks) sees the real value, because the
# block it asks about was constructed a few instructions earlier.

# a1.1.2's face numbering, which the renderer and the mesher share. 0 -Y, 1 +Y,
# 2 -Z, 3 +Z, 4 -X, 5 +X.
FACE_COUNT = 6

# Pre-Anvil metadata is a nibble.
METADATA_VALUES = range(16)


def build_block_table(workdir, block_class):
    """Run `Block.<clinit>` and return {id: Ref} for every block it builds.

    Blocks are recognised by carrying the id field, which every block passes
    through its constructor and nothing else in the class does.
    """
    machine = javap.Interpreter(javap.Classes(workdir))
    machine.call(block_class, "<clinit>", "()V", None, [])

    material_cls = material_class(workdir, block_class)
    id_field, texture_field, material_field = identify_fields(
        machine, block_class, material_cls)
    print(f"blockID is {block_class}.{id_field}, "
          f"blockIndexInTexture is {block_class}.{texture_field}, "
          f"material is {block_class}.{material_field} of type {material_cls}",
          file=sys.stderr)

    blocks = {}
    for value in machine.statics.values():
        if isinstance(value, javap.Ref) and id_field in value.fields:
            blocks[value.fields[id_field]] = value
    return machine, blocks, texture_field, material_field, material_cls


# Two values with nothing else to be confused with: a free block id and a
# texture index no block uses. Passed to Block's (id, texture, material)
# constructor, they come back out of whichever fields blockID and
# blockIndexInTexture happen to be obfuscated to.
ID_MARKER = 254
TEXTURE_MARKER = 249
# The material is an object, not a number, so it gets a symbolic marker rather
# than an out-of-the-way literal. Any name works as long as no real static
# shares it -- real ones are qualified, e.g. "gb.d".
MATERIAL_MARKER = "material"


def identify_fields(machine, block_class, material_cls):
    """(id field, texture field, material field) on the obfuscated Block class.

    Naming these by running the constructor beats hard-coding `bc` and `bb`,
    which are facts about one jar rather than about the game -- the same reason
    getRenderType is found by behaviour. The constructor cannot be run with a
    symbolic argument, because it indexes the block registry with the id and
    refuses a slot that is taken, so two out-of-the-way literals stand in: a
    free block id, and a texture index nothing uses.

    This has to run *after* the class initialiser, because the registry the
    constructor checks against is one of the statics that initialiser sets up.
    """
    instance = javap.Ref(block_class)
    machine.call(block_class, "<init>", f"(IIL{material_cls};)V", instance,
                 [ID_MARKER, TEXTURE_MARKER, javap.Static(MATERIAL_MARKER)])

    names = []
    for marker in (ID_MARKER, TEXTURE_MARKER, javap.Static(MATERIAL_MARKER)):
        found = [name for name, value in instance.fields.items()
                 if value == marker]
        if len(found) != 1:
            sys.exit(f"extract_blocks: {block_class}(int, int, Material) puts "
                     f"{marker} in {len(found)} fields ({found}), so the id, "
                     "texture and material fields cannot be told apart")
        names.append(found[0])
    return names[0], names[1], names[2]


def material_class(workdir, block_class):
    """The obfuscated Material class, from Block's own constructor signature.

    `(IILgb;)V` names it in its third parameter, so the class is read off the
    jar rather than written down -- the same reason the id and texture fields
    are found by running the constructor instead of being hard-coded.
    """
    for line in disassemble(workdir, block_class).splitlines():
        match = re.search(r"\(II(L[^;]+;)\)V", line)
        if match:
            return match.group(1)[1:-1]
    sys.exit(f"extract_blocks: no (int, int, Material) constructor on "
             f"{block_class}, so the material class cannot be identified")


def material_classes(workdir, material_cls):
    """{field name: class} for every Material singleton the class sets up.

    Material's own initialiser is a run of `new <class> ... putstatic <field>`,
    and the class is what decides the answers: the base class says solid and a
    handful of subclasses override it. Reading the pairs is enough -- the
    singletons never need to be constructed.
    """
    pending = None
    materials = {}
    for opcode, _, comment in parse_instructions(disassemble(workdir, material_cls)):
        if opcode == "new":
            pending = comment.replace("class ", "").strip()
        elif opcode == "putstatic" and pending is not None:
            field = comment.replace("Field ", "").strip().split(":")[0]
            materials[field.split(".")[-1]] = pending
            pending = None
    return materials


def material_solidity(workdir, material_cls, solid_method, cache):
    """{field name: isSolid} for every Material singleton.

    Which boolean method is isSolid cannot be settled from bytecode -- booleans
    carry no spread to recognise them by, the same problem isOpaqueCube has --
    so it comes from MEMBER_MAP, resolved once per version by reading it. What
    *is* derived here is the answer: the method is looked up on each
    singleton's own class, walking up to Material itself when it does not
    override.
    """
    method = solid_method + "()Z"
    return {field: bool(resolve_inherited(workdir, class_name, method, cache, default=1))
            for field, class_name in material_classes(workdir, material_cls).items()}


def face_textures(machine, block, block_class, texture_field):
    """The six face textures of one block, plus what they depend on.

    Three overloads of getBlockTexture form a chain, and the most specific one
    that can answer without being handed a world wins:

        getBlockTexture(world, x, y, z, face)   grass (snow above), chest and
                                                furnace (which way they face)
        getBlockTexture(face, metadata)         wheat's growth, farmland's wet
        getBlockTexture(face)                   logs, slabs, the crafting table

    A `Probe` stands in for the world, so a block that answers without touching
    it gives an answer good everywhere, and one that touches it is *reported* as
    world-dependent rather than answered with a plausible guess. Metadata is
    tried across all sixteen values: a block whose texture moves with it is
    reported too, and the table records metadata 0.

    The world overload is only worth asking when the block overrides it. Block's
    own is `getBlockTexture(face, world.getBlockMetadata(x, y, z))` -- it would
    reach for the world every time and make all seventy blocks look
    world-dependent, which says nothing.
    """
    class_name = block.class_name
    faces = []
    world_dependent = []
    metadata_dependent = []

    owner, _ = machine.classes.resolve(class_name, "a", "(Lnm;IIII)I")
    overrides_world = owner is not None and owner.name != block_class

    for face in range(FACE_COUNT):
        if overrides_world:
            answer = attempt(machine, class_name, "a", "(Lnm;IIII)I", block,
                             [javap.Probe("world"), 0, 0, 0, face])
            if answer is not None:
                faces.append(answer)
                continue
            world_dependent.append(face)

        by_metadata = {attempt(machine, class_name, "a", "(II)I", block,
                               [face, meta]) for meta in METADATA_VALUES}
        if len(by_metadata) > 1:
            metadata_dependent.append(face)
        answer = attempt(machine, class_name, "a", "(II)I", block, [face, 0])

        if answer is None:
            answer = attempt(machine, class_name, "a", "(I)I", block, [face])
        if answer is None:
            answer = block.fields.get(texture_field)
        faces.append(answer)

    if any(value is None for value in faces):
        return None, world_dependent, metadata_dependent
    return faces, world_dependent, metadata_dependent


def add_textures(workdir, block_class, records):
    """Fill in `texture` and `faces` on records the initialiser pass produced.

    The two passes agree on which blocks exist and what class each one is; that
    agreement is checked rather than assumed, because they arrive at it by
    completely different routes and a disagreement means one of them is broken.
    """
    machine, blocks, texture_field, material_field, material_cls = build_block_table(
        workdir, block_class)

    interpreted = set(blocks)
    matched = {record["id"] for record in records}
    if interpreted != matched:
        sys.exit("extract_blocks: the two passes disagree about which blocks "
                 f"exist: {sorted(interpreted ^ matched)}")

    varied, world, metadata = 0, [], []
    for record in records:
        block = blocks[record["id"]]
        if block.class_name != record["class"]:
            sys.exit(f"extract_blocks: id {record['id']} is "
                     f"{record['class']} to one pass and {block.class_name} "
                     "to the other")

        record["texture"] = block.fields.get(texture_field)

        # The interpreter never runs Material's own initialiser, so a material
        # arrives as an unwritten static and carries its qualified name --
        # which is exactly the identity wanted. Every block passes one to
        # super(), so this reaches the 52 whose subclass constructors hide it
        # from the static-initialiser pass.
        record["material"] = material_of(block, material_field)
        faces, needs_world, needs_metadata = face_textures(
            machine, block, block_class, texture_field)
        if faces is None:
            sys.exit(f"extract_blocks: could not work out the face textures "
                     f"of id {record['id']} ({block.class_name})")
        record["faces"] = faces
        if len(set(faces)) > 1:
            varied += 1
        if needs_world:
            world.append(record["id"])
            record["worldDependentFaces"] = needs_world
        if needs_metadata:
            metadata.append(record["id"])
            record["metadataDependentFaces"] = needs_metadata

    print(f"textures interpreted for {len(records)} blocks; "
          f"{varied} have per-face textures", file=sys.stderr)
    if world:
        print(f"note: faces of {sorted(world)} depend on the surrounding world "
              "(orientation, snow cover); metadata 0 and no neighbours assumed",
              file=sys.stderr)
    if metadata:
        print(f"note: faces of {sorted(metadata)} depend on block metadata; "
              "the table records metadata 0", file=sys.stderr)
    return material_cls, material_field, blocks


def attempt(machine, class_name, name, descriptor, block, args):
    """The method's value, or None if it needs something we did not supply."""
    try:
        value = machine.call(class_name, name, descriptor, block, args)
    except (javap.Bail, javap.Probed):
        return None
    return value if isinstance(value, int) else None


CONST_INT = {"iconst_m1": -1, **{f"iconst_{i}": i for i in range(6)}}
METHOD_DECL = re.compile(
    r"^  (?:public |private |protected |final |static )*(int|boolean) (\w+)\(\);")
CLASS_DECL = re.compile(r"^\s*(?:public |final |abstract )*class (\S+)(?: extends (\S+))?")


def scan_class(workdir, class_name, cache):
    """(superclass, {name: constant}) for every ()I method that returns a literal.

    getRenderType is one of these. It is not named in an obfuscated jar, so it
    is identified by behaviour: the only ()I method whose overrides spread
    across the small integers the renderer switches on.
    """
    if class_name in cache:
        return cache[class_name]

    result = subprocess.run(["javap", "-p", "-c", "-constants", class_name],
                            cwd=workdir, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        cache[class_name] = (None, {})
        return cache[class_name]

    superclass = None
    methods = {}
    current, body = None, []
    kind = None

    def flush():
        if current and len(body) == 2 and body[1][0] == "ireturn":
            opcode, operand = body[0]
            # boolean and int share ireturn; the declaration says which.
            key = current if kind == "int" else current + "()Z"
            if opcode in CONST_INT:
                methods[key] = CONST_INT[opcode]
            elif opcode in ("bipush", "sipush"):
                methods[key] = int(operand)

    for line in result.stdout.splitlines():
        header = CLASS_DECL.match(line)
        if header and superclass is None:
            superclass = header.group(2)
        declaration = METHOD_DECL.match(line)
        if declaration:
            flush()
            kind = declaration.group(1)
            current, body = declaration.group(2), []
            continue
        if current is not None:
            step = re.match(r"^\s*\d+: (\S+)\s*(\S*)", line)
            if step:
                body.append((step.group(1), step.group(2)))
            elif re.match(r"^  \S", line) and "Code:" not in line:
                flush()
                current, body = None, []
    flush()

    cache[class_name] = (superclass, methods)
    return cache[class_name]


def resolve_inherited(workdir, class_name, method, cache, default=0):
    """Walk up the superclass chain for the first class that overrides it."""
    seen = set()
    while class_name and class_name not in seen:
        seen.add(class_name)
        superclass, methods = scan_class(workdir, class_name, cache)
        if method in methods:
            return methods[method]
        class_name = superclass
    return default


def method_body(workdir, class_name, return_type, method):
    """(opcode, operand, comment) triples for one no-argument method, or None.

    scan_class only keeps methods whose whole body is `push; ireturn`, which is
    what makes it a *constant* reader. This is for the one method in a1.1.2's
    Block hierarchy that is not constant and still has to be recovered.
    """
    inside = False
    body = []
    for line in disassemble(workdir, class_name).splitlines():
        declaration = METHOD_DECL.match(line)
        if declaration:
            if inside:
                break
            inside = declaration.group(1) == return_type and declaration.group(2) == method
            continue
        if not inside:
            continue
        match = INSTRUCTION.match(line)
        if match:
            body.append((match.group(2), match.group(3), (match.group(4) or "").strip()))
        elif re.match(r"^  \S", line):
            break
    return body or None


def field_of(comment):
    """`Field bn:Lgb;` and `Field gb.f:Lgb;` -> `bn` and `gb.f`."""
    return comment.replace("Field ", "").strip().split(":")[0]


def material_branch(body):
    """`return this.<field> == <Material>.<name> ? a : b` -> (field, name, a, b).

    The one method shape the constant reader cannot see through, and in a1.1.2
    there is exactly one instance of it: BlockFluid's getRenderBlockPass, which
    puts water in the translucent pass and lava in the opaque one. That single
    bit decides which of a section's streams every fluid face lands in, so it is
    read out of the jar rather than written down -- and matched strictly, so a
    version that does something else here reports "unknown" instead of guessing.
    """
    shape = ["aload_0", "getfield", "getstatic", "if_acmpne", None, "goto", None, "ireturn"]
    if len(body) != len(shape):
        return None
    for (opcode, _, _), want in zip(body, shape):
        if want is not None and opcode != want:
            return None

    equal, unequal = literal(*body[4]), literal(*body[6])
    if not isinstance(equal, int) or not isinstance(unequal, int):
        return None
    return field_of(body[1][2]), field_of(body[2][2]), equal, unequal


def delegated_call(body, method):
    """`return this.<field>.<method>()` -> the field name, else None.

    A stairs block holds the block it is made of and forwards half its
    questions to it, this one included.
    """
    shape = ["aload_0", "getfield", "invokevirtual", "ireturn"]
    if len(body) != len(shape) or [op for op, _, _ in body] != shape:
        return None
    called = body[2][2].replace("Method ", "").strip().split(":")[0]
    if called.split(".")[-1] != method:
        return None
    return field_of(body[1][2])


def material_of(block, material_field):
    """The Material singleton's field name for an interpreted block."""
    value = block.fields.get(material_field)
    return value.name.split(".")[-1] if isinstance(value, javap.Static) else None


def render_pass(workdir, block, method, material_field, material_cls, cache, depth=4):
    """getRenderBlockPass for one block: 0 opaque, 1 translucent, None unknown.

    Every other property here is a constant this or that class returns.
    This one is not, in two different ways, and both are followed rather than
    assumed -- the answer decides which of a section's vertex streams every one
    of a block's faces lands in, so a silent 0 would be a whole render pass
    quietly missing.
    """
    if depth == 0 or block is None:
        return None

    class_name = block.class_name
    seen = set()
    while class_name and class_name not in seen:
        seen.add(class_name)
        superclass, methods = scan_class(workdir, class_name, cache)
        if method in methods:
            return methods[method]

        body = method_body(workdir, class_name, "int", method)
        if body is not None:
            # Declared here and not a constant, so it is one of the two shapes
            # below or we genuinely do not know.
            branch = material_branch(body)
            if branch is not None:
                field, against, equal, unequal = branch
                if field != material_field or not against.startswith(f"{material_cls}."):
                    return None
                material = material_of(block, material_field)
                return equal if material == against.split(".")[-1] else unequal

            field = delegated_call(body, method)
            target = block.fields.get(field) if field is not None else None
            if getattr(target, "class_name", None) is None:
                return None
            return render_pass(workdir, target, method, material_field, material_cls,
                               cache, depth - 1)

        class_name = superclass
    return None


def find_render_type_method(workdir, block_classes, cache):
    """The ()I method whose values spread across several small integers.

    Every candidate is checked rather than assumed, so a different obfuscation
    of a different version still resolves -- and if none does, the tool says so
    instead of quietly emitting zeroes.
    """
    tally = {}
    for class_name in block_classes:
        _, methods = scan_class(workdir, class_name, cache)
        for name, value in methods.items():
            tally.setdefault(name, set()).add(value)

    best, spread = None, 0
    for name, values in tally.items():
        if name.endswith("()Z"):
            continue
        plausible = {v for v in values if 0 <= v <= 20}
        if len(plausible) > spread:
            best, spread = name, len(plausible)
    return best if spread >= 4 else None


# How the obfuscated members map, per version. Established once by reading the
# bytecode (see the module docstring) and then relied on, so that re-running
# this tool is a check rather than another investigation.
MEMBER_MAP = {
    "a1.1.2": {
        "setters": {"c(F)Lly;": "hardness", "b(F)Lly;": "resistance",
                    "a(F)Lly;": "lightValue", "d(I)Lly;": "lightOpacity"},
        "opaque": "b",      # isOpaqueCube -- false for glass, leaves, fluids
        "fullCube": "c",    # renderAsNormalBlock -- false for stairs, torches
        # getRenderBlockPass. An ()I like getRenderType, but with no spread to
        # recognise it by: only BlockFluid overrides it and only to return 1,
        # so every candidate looks like a constant 0. Named by reading it.
        "renderPass": "g",
        # Material.isSolid, on the Material class rather than on Block. Named
        # by its one caller: the fluid renderer's corner-height helper asks it
        # whether a neighbouring cell dilutes the surface.
        "materialSolid": "a",
    },
}

# Blocks whose property is a branch on their own id, which the extractor
# cannot see. Listed rather than silently wrong. See the docstring.
KNOWN_CONDITIONAL = {"a1.1.2": {44: "isOpaqueCube depends on single vs double slab"}}

RENDER_NAMES = {-1: "none", 0: "cube", 1: "cross", 2: "torch", 3: "fire",
                4: "fluid", 5: "redstone_wire", 6: "crops", 7: "door",
                8: "ladder", 9: "rail", 10: "stairs", 11: "fence",
                12: "lever", 13: "cactus"}


def verify(records, table_path, version):
    """Check a checked-in blocks.json against what the jar actually says."""
    with open(table_path) as handle:
        table = json.load(handle)

    mapping = MEMBER_MAP.get(version)
    if mapping is None:
        sys.exit(f"extract_blocks: no member map for version {version!r}")

    ours = {entry["id"]: entry for entry in table["blocks"]}
    theirs = {record["id"]: record for record in records}
    conditional = KNOWN_CONDITIONAL.get(version, {})
    problems = []

    for missing in sorted(set(theirs) - set(ours)):
        problems.append(f"id {missing} is in the jar but not in blocks.json")
    for extra in sorted(set(ours) - set(theirs)):
        problems.append(f"id {extra} is in blocks.json but not in the jar")

    for bid in sorted(set(ours) & set(theirs)):
        mine, jar = ours[bid], theirs[bid]
        values = {}
        for name, value in jar["setters"]:
            if name in mapping["setters"] and value is not None:
                values[mapping["setters"][name]] = value

        opaque = bool(jar["booleans"][mapping["opaque"]])
        expected = {
            "render": RENDER_NAMES.get(jar.get("renderType", 0)),
            "material": jar.get("material"),
            "solid": jar.get("solid"),
            # getRenderBlockPass == 1. Only three of a1.1.2's blocks are, so the
            # table carries it the way it carries `faces`: present when it is
            # not the default, absent otherwise.
            "translucent": bool(jar.get("renderPass", 0)),
            "opaque": opaque,
            "fullCube": bool(jar["booleans"][mapping["fullCube"]]),
            "hardness": round(values.get("hardness", 0.0), 4),
            "resistance": round(values.get("resistance", 0.0), 4),
            "light": int(values.get("lightValue", 0.0) * 15),
            "opacity": int(values["lightOpacity"]) if "lightOpacity" in values
                       else (255 if opaque else 0),
        }
        expected["texture"] = jar["texture"]
        # A block whose six faces are the same carries no `faces` in the table:
        # the mesher falls back to `texture`, and 59 of the 70 would otherwise
        # be six copies of one number.
        if len(set(jar["faces"])) > 1:
            expected["faces"] = jar["faces"]
        elif "faces" in mine:
            problems.append(
                f"id {bid} ({mine.get('name', '?')}): has a faces list, but "
                f"the jar gives every face texture {jar['faces'][0]}")

        for key, want in expected.items():
            got = mine.get(key, False) if key == "translucent" else mine.get(key)
            conditional_key = key in ("opaque", "opacity", "fullCube")
            if got != want and not (bid in conditional and conditional_key):
                problems.append(
                    f"id {bid} ({mine.get('name', '?')}): {key} is {got!r}, "
                    f"jar says {want!r}")

    for bid, why in conditional.items():
        if bid in ours:
            print(f"note: id {bid} ({ours[bid].get('name')}) not checked -- {why}",
                  file=sys.stderr)

    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} disagreement(s) with the jar")
        return 1
    print(f"blocks.json agrees with the jar on all {len(ours)} blocks "
          f"(names not checked -- the jar has none)")
    return 0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    jar_path = sys.argv[1]
    out_path = None
    verify_path = None
    version = "a1.1.2"
    if "--json" in sys.argv:
        out_path = sys.argv[sys.argv.index("--json") + 1]
    if "--verify" in sys.argv:
        verify_path = sys.argv[sys.argv.index("--verify") + 1]
        with open(verify_path) as handle:
            version = json.load(handle).get("version", version)

    import tempfile
    with tempfile.TemporaryDirectory() as workdir:
        with zipfile.ZipFile(jar_path) as jar:
            jar.extractall(workdir)
        entry = find_class(jar_path, STEP_SOUND_MARKERS)
        block_class = entry[:-len(".class")]
        print(f"Block class: {block_class}", file=sys.stderr)
        text = disassemble(workdir, block_class)

        blocks = parse_blocks(parse_instructions(text))
        records = []
        for block in blocks:
            args = block["args"]
            # The initialiser also builds the step sounds, whose first argument
            # is a name, not an id. A block's first argument is always its id.
            if not args or not isinstance(args[0], int):
                continue
            records.append({
                "id": args[0],
                "field": block["field"],
                "class": block["class"],
                "signature": block.get("signature", ""),
                "setters": [[name, value] for name, value in block["setters"]],
            })

        material_cls, material_field, interpreted = add_textures(
            workdir, block_class, records)

        cache = {}
        classes = sorted({r["class"] for r in records} | {block_class})
        method = find_render_type_method(workdir, classes, cache)
        if method is None:
            print("warning: could not identify getRenderType; leaving it unset",
                  file=sys.stderr)
        else:
            print(f"getRenderType is {block_class}.{method}()", file=sys.stderr)
            for record in records:
                record["renderType"] = resolve_inherited(
                    workdir, record["class"], method, cache)

        # Every constant-returning boolean, inherited. Which one is
        # isOpaqueCube cannot be settled from bytecode alone -- booleans carry
        # no spread to recognise it by -- so they are all reported, and the
        # mapping is resolved once per version by reading them.
        boolean_names = sorted({
            name for class_name in classes
            for name in scan_class(workdir, class_name, cache)[1]
            if name.endswith("()Z")})
        for record in records:
            record["booleans"] = {
                name[:-3]: bool(resolve_inherited(workdir, record["class"], name,
                                                  cache, default=1))
                for name in boolean_names}

        # Which of the two terrain passes a block is drawn in. 0 is the opaque
        # pass, 1 is the sorted and blended one, and in a1.1.2 exactly two
        # blocks are in it -- still and flowing water. Lava is *not*: it shares
        # a class with water and differs only in its material, which is what
        # makes this the one method here that is not a constant.
        pass_method = MEMBER_MAP.get(version, {}).get("renderPass")
        if pass_method is None:
            print("warning: no renderPass in MEMBER_MAP for this version; "
                  "leaving it unset", file=sys.stderr)
        else:
            unknown = []
            for record in records:
                value = render_pass(workdir, interpreted[record["id"]], pass_method,
                                    material_field, material_cls, cache)
                if value is None:
                    unknown.append(record["id"])
                else:
                    record["renderPass"] = value
            translucent = sorted(r["id"] for r in records if r.get("renderPass"))
            print(f"getRenderBlockPass is {block_class}.{pass_method}(); "
                  f"translucent blocks are {translucent}", file=sys.stderr)
            if unknown:
                print(f"note: renderPass of {sorted(unknown)} could not be read "
                      "from the jar and is left unset", file=sys.stderr)

        # Solidity is a property of the material, not of the block: it is what
        # decides whether a neighbour dilutes a fluid's surface height, and it
        # is *not* the same question as isOpaqueCube -- glass and leaves are
        # solid materials that are not opaque cubes.
        solid_method = MEMBER_MAP.get(version, {}).get("materialSolid")
        if solid_method is None:
            print("warning: no materialSolid in MEMBER_MAP for this version; "
                  "leaving solidity unset", file=sys.stderr)
        else:
            classes_by_material = material_classes(workdir, material_cls)
            solidity = material_solidity(workdir, material_cls, solid_method, cache)
            print(f"material is {material_cls}, isSolid is {solid_method}():",
                  file=sys.stderr)
            for field in sorted(solidity):
                print(f"    {material_cls}.{field:<2} {classes_by_material[field]:<4} "
                      f"{'solid' if solidity[field] else 'NOT solid'}", file=sys.stderr)
            for record in records:
                if record.get("material") in solidity:
                    record["solid"] = solidity[record["material"]]

    records.sort(key=lambda r: r["id"])
    print(f"recovered {len(records)} blocks, "
          f"ids {records[0]['id']}..{records[-1]['id']}", file=sys.stderr)

    if verify_path:
        return verify(records, verify_path, version)

    text_out = json.dumps(records, indent=2)
    if out_path:
        with open(out_path, "w") as handle:
            handle.write(text_out + "\n")
    else:
        print(text_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
