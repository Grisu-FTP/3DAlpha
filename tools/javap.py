"""A very small JVM interpreter over `javap -c` output.

MAINTAINER TOOL -- see tools/extract_blocks.py. Nothing here runs at build time
or on a player's console.

Why this exists: `extract_blocks.py` used to read block properties by pattern
matching the static initialiser, which works only for values that appear in it
as literals. Two whole categories escaped that:

  * a block whose texture is set in its own constructor rather than passed to
    Block's -- grass, logs, chests, doors -- had no texture at all, and
  * the classes that override getBlockTexture(face) to give each side a
    different tile, which is a branch and not a constant. 24 of the 70 blocks
    turn out to have per-face textures.

Pattern matching also gets the *wrong* answer where a subclass constructor takes
`(int id, boolean flag)`: reading "the second int argument" turns the flag into
a texture index. That is how the furnace ended up with texture 0.

The fix is to stop guessing what the bytecode means and run it. What is modelled
is what these classes actually use: integer and floating-point arithmetic,
comparisons, branches and switches, instance fields, static fields, arrays, and
calls followed into their own bytecode. Nothing else -- no strings beyond
carrying a constant around, no exceptions, no java.* runtime beyond consuming a
call with the right stack effect.

Anything outside the subset evaluates to `UNKNOWN`, which propagates rather than
inventing a number. Branching on an UNKNOWN condition raises `Bail`, so a method
this cannot understand is reported, never silently mis-read. A `Probe` goes one
better: it stands in for an argument the caller genuinely does not have (the
world a block is being asked about) and raises `Probed` on any use, which is an
answer -- "this depends on something you did not supply" -- rather than a
failure.

The scope really is one jar's Block class. It is written to be re-read by
someone checking a fact, not to be a JVM.
"""

import re
import subprocess


class Bail(Exception):
    """This method uses something the interpreter does not model."""


class _Unknown:
    """A value the interpreter cannot pin down. Arithmetic on it stays unknown;
    branching on it is an error, because guessing a branch invents a fact."""

    def __repr__(self):
        return "UNKNOWN"

    def __bool__(self):
        raise Bail("branched on an unknown value")


UNKNOWN = _Unknown()


class Ref:
    """An object. `fields` is shared by identity, which is what makes
    `this.bb` visible to a subclass constructor after the super call sets it."""

    __slots__ = ("class_name", "fields")

    def __init__(self, class_name, fields=None):
        self.class_name = class_name
        self.fields = {} if fields is None else fields

    def __repr__(self):
        return f"<{self.class_name} {self.fields}>"


class Static:
    """A static field nobody has written, e.g. an enum-like Material singleton
    initialised in a class we never run. Compared by name, which is all
    `if_acmpne` needs, and all these constructors ask of a material."""

    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name

    def __eq__(self, other):
        return isinstance(other, Static) and other.name == self.name

    def __hash__(self):
        return hash(self.name)

    def __repr__(self):
        return f"@{self.name}"


class Probed(Exception):
    """Raised when interpretation reached a `Probe`. Deliberately not a `Bail`:
    a Bail is caught and turned into UNKNOWN, whereas this has to reach the
    caller, because "this answer depends on something you did not supply" is
    the result, not a failure."""

    def __init__(self, what):
        super().__init__(what)
        self.what = what


class Probe(Ref):
    """A stand-in for an argument the caller cannot provide -- the world a
    block is being asked about. Touching it raises `Probed`, so a method that
    answers without consulting it gives an answer good everywhere, and one that
    consults it is reported as world-dependent instead of being answered with a
    guess."""

    def __init__(self, name="probe"):
        super().__init__(name)


class Array:
    """A real array, because Block's constructor reads the registry it is in
    the middle of filling -- `if (blocksList[id] != null) throw` -- and a
    registry that answers UNKNOWN turns that check into a dead end."""

    __slots__ = ("length", "zero", "values")

    def __init__(self, length, zero):
        self.length = length
        self.zero = zero
        self.values = {}

    def get(self, index):
        if index is UNKNOWN or not isinstance(index, int):
            return UNKNOWN
        return self.values.get(index, self.zero)

    def put(self, index, value):
        if isinstance(index, int):
            self.values[index] = value

    def __repr__(self):
        return f"<array[{self.length}] {len(self.values)} set>"


# `newarray` names its element type; reference arrays start out null.
_ARRAY_ZERO = {"boolean": False, "byte": 0, "char": 0, "short": 0, "int": 0,
               "long": 0, "float": 0.0, "double": 0.0}


# ---------------------------------------------------------------- disassembly

_INSTRUCTION = re.compile(r"^\s*(\d+):\s+(\S+)([^/]*)(?://\s*(.*))?$")
# A switch spreads over several lines: `case: target` then `default: target`.
_SWITCH_CASE = re.compile(r"^\s*(-?\d+|default):\s*(\d+)\s*$")
_CLASS = re.compile(r"^\s*(?:public |final |abstract )*(?:class|interface) (\S+)"
                    r"(?: extends (\S+))?")
_MEMBER = re.compile(r"^  [^;]*?([\w.$]+)\((.*)\);\s*$")

_JAVA_KEYWORDS = {"public", "protected", "private", "static", "final",
                  "abstract", "synchronized", "native", "strictfp",
                  "transient", "volatile"}

# javap prints descriptors as Java source types. Only the shapes these classes
# actually use need to round-trip back into a descriptor.
_PRIMITIVE = {"int": "I", "boolean": "Z", "float": "F", "double": "D",
              "long": "J", "char": "C", "byte": "B", "short": "S",
              "void": "V"}


def _descriptor(param_text, return_text):
    def one(java_type):
        java_type = java_type.strip()
        arrays = 0
        while java_type.endswith("[]"):
            arrays += 1
            java_type = java_type[:-2]
        base = _PRIMITIVE.get(java_type) or "L" + java_type.replace(".", "/") + ";"
        return "[" * arrays + base

    params = [p for p in param_text.split(",") if p.strip()]
    return "(" + "".join(one(p) for p in params) + ")" + one(return_text)


def _parameter_widths(descriptor):
    """One local-slot width per declared parameter: 2 for long and double,
    1 for everything else."""
    inner = descriptor[descriptor.index("(") + 1:descriptor.rindex(")")]
    widths, i = [], 0
    while i < len(inner):
        char = inner[i]
        if char == "[":
            i += 1
            continue
        if char == "L":
            i = inner.index(";", i) + 1
        else:
            i += 1
        widths.append(2 if char in "JD" else 1)
    return widths


def parameter_count(descriptor):
    """How many values a call pops. One per parameter -- the operand stack
    holds a long or a double as a single Python value, unlike the locals."""
    return len(_parameter_widths(descriptor))


def _return_type(descriptor):
    return descriptor[descriptor.rindex(")") + 1:]


class Method:
    __slots__ = ("name", "descriptor", "code", "index", "is_static")

    def __init__(self, name, descriptor, is_static):
        self.name = name
        self.descriptor = descriptor
        self.is_static = is_static
        self.code = []          # (pc, opcode, operand, comment)
        self.index = {}         # pc -> position in self.code

    def seal(self):
        self.index = {pc: i for i, (pc, *_) in enumerate(self.code)}


class ClassFile:
    __slots__ = ("name", "superclass", "methods")

    def __init__(self, name, superclass):
        self.name = name
        self.superclass = superclass
        self.methods = {}       # (name, descriptor) -> Method

    def find(self, name, descriptor):
        return self.methods.get((name, descriptor))


def parse_class(text):
    """`javap -p -c -constants` output for one class."""
    class_name, superclass = None, None
    methods = {}
    current = None

    for line in text.splitlines():
        header = _CLASS.match(line)
        if header and class_name is None:
            class_name, superclass = header.group(1), header.group(2)
            continue

        if line.startswith("  ") and not line.startswith("   "):
            if current is not None:
                current.seal()
            current = None
            # javap spells the class initialiser `static {};`, with no
            # signature at all. It is the method the whole block table lives in.
            if line.strip() == "static {};":
                current = Method("<clinit>", "()V", True)
                methods[("<clinit>", "()V")] = current
                continue

            member = _MEMBER.match(line)   # fields have no parentheses
            if member:
                words = line.split("(")[0].split()
                name = member.group(1).split(".")[-1]
                # A constructor has no return type; javap prints the class name.
                modifiers = [w for w in words[:-1] if w in _JAVA_KEYWORDS]
                typed = [w for w in words[:-1] if w not in _JAVA_KEYWORDS]
                if typed:
                    return_text, name_out = typed[-1], name
                else:
                    return_text, name_out = "void", "<init>"
                descriptor = _descriptor(member.group(2), return_text)
                current = Method(name_out, descriptor, "static" in modifiers)
                methods[(name_out, descriptor)] = current
            continue

        if current is None:
            continue

        # A switch's case lines look like instructions with a `pc: target`
        # shape, so they are consumed by the switch rather than matched.
        if current.code and current.code[-1][1].endswith("switch"):
            case = _SWITCH_CASE.match(line)
            if case:
                current.code[-1][2][case.group(1)] = int(case.group(2))
                continue
            if line.strip() == "}":
                continue

        step = _INSTRUCTION.match(line)
        if step:
            opcode = step.group(2)
            operand = ({} if opcode.endswith("switch")
                       else step.group(3).strip())
            current.code.append([int(step.group(1)), opcode, operand,
                                 (step.group(4) or "").strip()])

    if current is not None:
        current.seal()

    if class_name is None:
        raise Bail("javap output had no class header")
    parsed = ClassFile(class_name, superclass)
    parsed.methods = methods
    return parsed


class Classes:
    """Disassembles on demand and remembers. `javap` is slow enough that
    re-reading Block once per block would dominate the tool's runtime."""

    def __init__(self, workdir):
        self.workdir = workdir
        self._cache = {}

    def get(self, name):
        if name in self._cache:
            return self._cache[name]
        result = subprocess.run(
            ["javap", "-p", "-c", "-constants", name],
            cwd=self.workdir, capture_output=True, text=True, check=False)
        parsed = None
        if result.returncode == 0:
            try:
                parsed = parse_class(result.stdout)
            except Bail:
                parsed = None
        self._cache[name] = parsed
        return parsed

    def resolve(self, class_name, method_name, descriptor):
        """Virtual dispatch: the first class up the chain that defines it."""
        seen = set()
        while class_name and class_name not in seen:
            seen.add(class_name)
            owner = self.get(class_name)
            if owner is None:
                return None, None
            found = owner.find(method_name, descriptor)
            if found is not None:
                return owner, found
            class_name = owner.superclass
        return None, None


# --------------------------------------------------------------- interpreter

_CONSTANTS = {
    "iconst_m1": -1, "aconst_null": None,
    **{f"iconst_{i}": i for i in range(6)},
    **{f"lconst_{i}": i for i in range(2)},
    **{f"fconst_{i}": float(i) for i in range(3)},
    **{f"dconst_{i}": float(i) for i in range(2)},
}

_BINARY = {
    "iadd": lambda a, b: a + b, "isub": lambda a, b: a - b,
    "imul": lambda a, b: a * b,
    # Java's / truncates toward zero; Python's // floors.
    "idiv": lambda a, b: int(a / b) if b else UNKNOWN,
    "irem": lambda a, b: a - int(a / b) * b if b else UNKNOWN,
    "iand": lambda a, b: a & b, "ior": lambda a, b: a | b,
    "ixor": lambda a, b: a ^ b, "ishl": lambda a, b: a << (b & 31),
    "ishr": lambda a, b: a >> (b & 31),
}

# Float/double/long opcodes, keyed without their type prefix. `cmp` variants
# push an int, which is why they sit here rather than in _BINARY.
_REAL = {
    "add": lambda a, b: a + b, "sub": lambda a, b: a - b,
    "mul": lambda a, b: a * b,
    "div": lambda a, b: a / b if b else UNKNOWN,
    "rem": lambda a, b: a - int(a / b) * b if b else UNKNOWN,
    "neg": lambda a, b: -b,
    "cmpl": lambda a, b: (a > b) - (a < b), "cmpg": lambda a, b: (a > b) - (a < b),
    "cmp": lambda a, b: (a > b) - (a < b),
}


def _real(operation, left, right):
    if right is UNKNOWN or (operation != "neg" and left is UNKNOWN):
        return UNKNOWN
    return _REAL[operation](left, right)


_UNARY_COMPARE = {
    "ifeq": lambda v: v == 0, "ifne": lambda v: v != 0,
    "iflt": lambda v: v < 0, "ifge": lambda v: v >= 0,
    "ifgt": lambda v: v > 0, "ifle": lambda v: v <= 0,
    "ifnull": lambda v: v is None, "ifnonnull": lambda v: v is not None,
}

_BINARY_COMPARE = {
    "if_icmpeq": lambda a, b: a == b, "if_icmpne": lambda a, b: a != b,
    "if_icmplt": lambda a, b: a < b, "if_icmpge": lambda a, b: a >= b,
    "if_icmpgt": lambda a, b: a > b, "if_icmple": lambda a, b: a <= b,
    "if_acmpeq": lambda a, b: a is b or a == b,
    "if_acmpne": lambda a, b: not (a is b or a == b),
}

# Opcodes with no effect worth modelling. Conversions are lossy in ways that do
# not matter here: nothing we read back out of a block is a narrowed integer.
_NOPS = {"nop", "i2l", "i2f", "i2d", "l2i", "f2i", "d2i", "f2d", "d2f",
         "l2f", "l2d", "i2b", "i2c", "i2s", "checkcast"}

_MAX_STEPS = 20000


class Interpreter:
    """Runs one method with concrete arguments.

    `statics` maps a `Class.field` name to a value, and is how a block that
    reads `Block.stone.blockIndexInTexture` sees the stone that the static
    initialiser built a moment earlier. Anything not in it becomes a `Static`
    marker: comparable, but opaque.
    """

    def __init__(self, classes, statics=None):
        self.classes = classes
        self.statics = {} if statics is None else statics

    def call(self, class_name, name, descriptor, this, args, depth=0):
        if depth > 8:
            raise Bail("call depth")
        owner, method = self.classes.resolve(class_name, name, descriptor)
        if method is None or not method.code:
            raise Bail(f"no code for {class_name}.{name}{descriptor}")
        return self._run(owner, method, this, args, depth)

    def _run(self, owner, method, this, args, depth):
        locals_ = {}
        slot = 0
        if not method.is_static:
            locals_[0] = this
            slot = 1
        # Longs and doubles occupy two local slots, so the widths come from the
        # descriptor rather than from the Python type of the value.
        for value, width in zip(args, _parameter_widths(method.descriptor)):
            locals_[slot] = value
            slot += width

        stack = []
        position = 0
        steps = 0

        while True:
            steps += 1
            if steps > _MAX_STEPS or position >= len(method.code):
                raise Bail("ran off the end of " + method.name)
            _, opcode, operand, comment = method.code[position]
            position += 1

            if opcode in _NOPS:
                continue

            if opcode in _CONSTANTS:
                stack.append(_CONSTANTS[opcode])
                continue

            if opcode in ("bipush", "sipush"):
                stack.append(int(operand))
                continue

            if opcode.startswith("ldc"):
                stack.append(_literal(comment))
                continue

            if opcode.startswith(("iload", "aload", "fload", "lload", "dload")):
                stack.append(locals_.get(_slot(opcode, operand), UNKNOWN))
                continue

            if opcode.startswith(("istore", "astore", "fstore", "lstore",
                                  "dstore")):
                locals_[_slot(opcode, operand)] = stack.pop()
                continue

            if opcode == "dup":
                stack.append(stack[-1])
                continue
            if opcode == "dup_x1":
                stack.insert(-2, stack[-1])
                continue
            if opcode in ("pop", "pop2"):
                stack.pop()
                continue

            if opcode in _BINARY:
                right, left = stack.pop(), stack.pop()
                stack.append(UNKNOWN
                             if UNKNOWN in (left, right)
                             else _BINARY[opcode](left, right))
                continue

            if opcode == "ineg":
                value = stack.pop()
                stack.append(UNKNOWN if value is UNKNOWN else -value)
                continue

            # Float, double and long arithmetic. Worth following rather than
            # discarding: setResistance stores `value * 3`, so a block's blast
            # resistance is only ever a computed float.
            if opcode[0] in "fdl" and opcode[1:] in _REAL:
                right = stack.pop()
                left = stack.pop() if opcode[1:] != "neg" else None
                stack.append(_real(opcode[1:], left, right))
                continue

            if opcode == "iinc":
                slot, delta = (int(part) for part in operand.split(","))
                value = locals_.get(slot, UNKNOWN)
                locals_[slot] = UNKNOWN if value is UNKNOWN else value + delta
                continue

            if opcode == "goto" or opcode == "goto_w":
                position = method.index[int(operand)]
                continue

            if opcode.endswith("switch"):
                key = _defined(stack.pop())
                target = operand.get(str(key), operand.get("default"))
                position = method.index[target]
                continue

            if opcode in _UNARY_COMPARE:
                if _UNARY_COMPARE[opcode](_defined(stack.pop())):
                    position = method.index[int(operand)]
                continue

            if opcode in _BINARY_COMPARE:
                right, left = _defined(stack.pop()), _defined(stack.pop())
                if _BINARY_COMPARE[opcode](left, right):
                    position = method.index[int(operand)]
                continue

            if opcode == "getstatic":
                stack.append(self._static(owner.name, comment))
                continue

            if opcode == "putstatic":
                name = comment.replace("Field ", "").strip().split(":")[0]
                self.statics[_qualify(owner.name, name)] = stack.pop()
                continue

            if opcode == "getfield":
                target = stack.pop()
                field = _member(comment)[0]
                if isinstance(target, Probe):
                    raise Probed(f"{target.class_name}.{field}")
                stack.append(target.fields.get(field, UNKNOWN)
                             if isinstance(target, Ref) else UNKNOWN)
                continue

            if opcode == "putfield":
                value, target = stack.pop(), stack.pop()
                if isinstance(target, Ref):
                    target.fields[_member(comment)[0]] = value
                continue

            if opcode == "new":
                stack.append(Ref(comment.replace("class ", "").strip()))
                continue

            if opcode in ("newarray", "anewarray"):
                length = stack.pop()
                stack.append(Array(length, _ARRAY_ZERO.get(operand.strip())))
                continue

            if opcode == "arraylength":
                array = stack.pop()
                stack.append(array.length if isinstance(array, Array)
                             else UNKNOWN)
                continue

            # The `?aload`/`?astore` family. `aload`/`astore` themselves are
            # local-variable access and were handled above.
            if len(opcode) == 6 and opcode.endswith("aload"):
                index, array = stack.pop(), stack.pop()
                stack.append(array.get(index) if isinstance(array, Array)
                             else UNKNOWN)
                continue

            if len(opcode) == 7 and opcode.endswith("astore"):
                value, index, array = stack.pop(), stack.pop(), stack.pop()
                if isinstance(array, Array):
                    array.put(index, value)
                continue

            if opcode.startswith("invoke"):
                self._invoke(opcode, comment, stack, depth, owner.name)
                continue

            if opcode == "ireturn" or opcode == "areturn" or opcode == "freturn":
                return stack.pop()

            if opcode == "return":
                return None

            if opcode == "athrow":
                raise Bail("threw")

            raise Bail("unmodelled opcode " + opcode)

    def _static(self, owner_name, comment):
        # javap leaves the class off a field in the class being disassembled
        # and spells it out for any other, so the owner has to be filled back
        # in before the name is a key.
        name = _qualify(owner_name,
                        comment.replace("Field ", "").strip().split(":")[0])
        if name in self.statics:
            return self.statics[name]
        return Static(name)

    def _invoke(self, opcode, comment, stack, depth, current_class):
        target = comment.split(None, 1)[-1].strip()
        owner_and_name, _, descriptor = target.partition(":")
        if "." in owner_and_name:
            owner_name, _, method_name = owner_and_name.rpartition(".")
        else:
            owner_name, method_name = current_class, owner_and_name
        method_name = method_name.strip('"')

        count = parameter_count(descriptor)
        args = [stack.pop() for _ in range(count)][::-1]
        receiver = None if opcode == "invokestatic" else stack.pop()

        returns = _return_type(descriptor)
        if isinstance(receiver, Probe):
            raise Probed(f"{receiver.class_name}.{method_name}{descriptor}")

        if owner_name.startswith("java/"):
            if returns != "V":
                stack.append(UNKNOWN)
            return

        # invokespecial is the non-virtual one: a constructor chaining to its
        # super, which must start at the named class. Dispatching it from the
        # receiver's own class instead would find the subclass constructor
        # again and recurse forever.
        if opcode == "invokespecial" or not isinstance(receiver, Ref):
            class_name = owner_name
        else:
            class_name = receiver.class_name
        try:
            value = self.call(class_name, method_name, descriptor,
                              receiver, args, depth + 1)
        except Bail:
            if returns != "V":
                # Alpha's setters return `this` so they can be chained. Putting
                # the receiver back keeps a chain from collapsing to UNKNOWN --
                # but only when the return type really is the receiver's own
                # type, or a call on someone else's object would be read as
                # having handed that object back.
                stack.append(receiver if self._returns_self(receiver, returns)
                             else UNKNOWN)
            return

        if returns != "V":
            stack.append(value if value is not None else UNKNOWN)

    def _returns_self(self, receiver, returns):
        if not isinstance(receiver, Ref) or not returns.startswith("L"):
            return False
        wanted = returns[1:-1]
        name, seen = receiver.class_name, set()
        while name and name not in seen:
            if name == wanted:
                return True
            seen.add(name)
            owner = self.classes.get(name)
            name = owner.superclass if owner else None
        return False


def _qualify(owner_name, field_name):
    return field_name if "." in field_name else f"{owner_name}.{field_name}"


def _defined(value):
    if value is UNKNOWN:
        raise Bail("branched on an unknown value")
    return value


def _slot(opcode, operand):
    if "_" in opcode:
        return int(opcode.rsplit("_", 1)[1])
    return int(operand)


def _member(comment):
    body = comment.replace("Field ", "").strip()
    name, _, descriptor = body.partition(":")
    return name.split(".")[-1], descriptor


def _literal(comment):
    parts = comment.split(None, 1)
    if len(parts) != 2:
        return UNKNOWN
    kind, value = parts
    if kind in ("int", "long"):
        return int(value.rstrip("lL"))
    if kind in ("float", "double"):
        return float(value.rstrip("fd"))
    if kind == "String":
        return value
    return UNKNOWN
