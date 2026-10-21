#!/usr/bin/env python3
"""The save format record: write it, and check the tree against it.

A save is the bytes of a few C structs copied to the cartridge's flash
chip. A build reads a save back through its own copy of those structs, so a
save made before a change to one of them is read from the wrong places
afterwards. This tool writes down exactly how the compiler lays the save
out, in test/save_format.txt (the "record"), and checks a tree against it.
test/tests/test_save_format.lua runs it in the suite. CLAUDE.md's
"Breaking changes" section says what to do when it fails.

  python3 test/tools/save_format.py
      Check the tree against the record. Exit 0 when they match; exit 1,
      printing the differences and what to do, when they don't; exit 2,
      saying what's missing, when it can't run.
  python3 test/tools/save_format.py --write
      Write the tree's save format into the record. Only for a change made
      on purpose: a breaking change, which its PR must say it is.
  --root DIR      the tree to read (default: the repo this tool is in). It
                  needs include/ and tools/agbcc, as `make firered` does.
  --record FILE   the record (default: test/save_format.txt in the repo this
                  tool is in).

HOW IT WORKS. The C compiler already knows the exact layout: with -g it
writes it into the object file as DWARF debug information, a standard
description of every type (each struct's size, each member's offset, type,
array sizes and bitfield bits). So the tool compiles a small "probe" C file
that includes the save's headers, and reads the result:

  1. The probe names each root type below and holds the flash format's
     constants as an enum, so the compiler works out their values.
  2. It is compiled the way `make firered` compiles a C file for FireRed
     v1.0 with agbcc: the host `cc -E` with the Makefile's flags, then
     tools/agbcc/bin/agbcc with -g, then arm-none-eabi-as.
     agbcc leaves the DWARF abbreviation table (the table of the debug
     entries' shapes) without its closing zero byte, and readelf refuses
     the file without it, so the tool adds that byte to the assembly.
  3. `arm-none-eabi-readelf --debug-dump=info` prints the debug entries as
     text. The tool reads them into a tree and walks every struct, union,
     array, typedef and enum type reachable from the roots, without
     following pointers (a pointer's own 4 bytes are saved, never what it
     points at). Types no save holds are left out.
  4. It writes them as plain text, one section per type, in a fixed order,
     so the same tree always gives the same bytes.

It needs Python's standard library only.
"""

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# The repo this tool is in: test/tools/save_format.py -> the repo root.
REPO = Path(__file__).resolve().parents[2]
DEFAULT_RECORD = REPO / "test" / "save_format.txt"
TOOL_COMMAND = "python3 test/tools/save_format.py"

# The headers the probe includes: every root type and constant is in one.
PROBE_HEADERS = [
    "global.h",
    "save.h",
    "pokemon_storage_system.h",
    "cereader_tool.h",
    "hall_of_fame.h",
    "poke_radar.h",
]

# Every type the game writes to flash, directly. Everything they hold is
# found from them. A new struct laid over reserved bytes (as struct
# PokeRadar is over SaveBlock1's unused_348C) must be added here, in the
# same PR (CLAUDE.md, "Breaking changes").
ROOTS = [
    "struct SaveBlock1",            # the player, the bag, flags, vars ...
    "struct SaveBlock2",            # name, options, Pokedex, ...
    "struct PokemonStorage",        # the PC boxes
    "struct SaveSector",            # one 4 KiB flash sector, with its footer
    "struct HallofFameTeam",        # the Hall of Fame sectors
    "struct EReaderTrainerTowerSet",  # the Trainer Tower e-Reader sectors
    "struct PokeRadar",             # laid over SaveBlock1.unused_348C
]

# The flash format's constants, recorded by value.
CONSTANTS = [
    # include/save.h: sector sizes, the signature, the sector map
    "SECTOR_DATA_SIZE",
    "SECTOR_FOOTER_SIZE",
    "SECTOR_SIZE",
    "NUM_SAVE_SLOTS",
    "SECTOR_SIGNATURE",
    "SPECIAL_SECTOR_SENTINEL",
    "SECTOR_ID_SAVEBLOCK2",
    "SECTOR_ID_SAVEBLOCK1_START",
    "SECTOR_ID_SAVEBLOCK1_END",
    "SECTOR_ID_PKMN_STORAGE_START",
    "SECTOR_ID_PKMN_STORAGE_END",
    "NUM_SECTORS_PER_SLOT",
    "SECTOR_ID_HOF_1",
    "SECTOR_ID_HOF_2",
    "SECTOR_ID_TRAINER_TOWER_1",
    "SECTOR_ID_TRAINER_TOWER_2",
    "SECTORS_COUNT",
    "NUM_HOF_SECTORS",
    # include/hall_of_fame.h
    "HALL_OF_FAME_MAX_TEAMS",
    # include/poke_radar.h: marks SaveBlock1.unused_348C as holding a radar
    "POKE_RADAR_MAGIC",
]

# The names the probe gives its roots and constants. A constant's enum
# member needs a name of its own: one spelled like the macro would be
# replaced by the macro's value.
ROOT_VARIABLE = "saveFormatRoot{}"
CONSTANT_PREFIX = "SAVE_FORMAT_PROBE_"

# The Makefile's flags for a FireRed v1.0 English C file built by agbcc
# (MODERN=0, no POKEMON_DEBUG). Paths are from the tree's root.
CPP_FLAGS = [
    "-iquote", "include", "-Wno-trigraphs", "-DFIRERED", "-DREVISION=0",
    "-DENGLISH", "-DMODERN=0", "-I", "tools/agbcc/include", "-I", "tools/agbcc",
    "-nostdinc", "-undef", "-std=gnu89",
]
AGBCC_FLAGS = ["-mthumb-interwork", "-O2", "-fhex-asm", "-g"]
AS_FLAGS = ["-mcpu=arm7tdmi"]

# The closing zero byte agbcc leaves off the DWARF abbreviation table.
ABBREV_TERMINATOR = "\t.section\t.debug_abbrev\n\t.byte\t0\n"

RECORD_HEADER = """\
# The save format: how the compiler lays out everything the game saves to
# flash. test/tools/save_format.py writes this file, and
# test/tests/test_save_format.lua fails the suite when the tree no longer
# matches it.
#
# Never edit it by hand. A change to it is a breaking change (CLAUDE.md,
# "Breaking changes"): make one only on purpose, by running
#     python3 test/tools/save_format.py --write
# and committing this file, and say so in the PR. After a merge conflict in
# it, run --write again on the merged tree.
#
# It describes FireRed v1.0 English built with agbcc (make firered).
# "constants" are flash format values, from include/save.h and others.
# Each struct or union gives its size, then one line per member: its offset
# in bytes, its name and its type as written. For a bitfield the type ends
# in :width, then the bits it uses in the unit at that offset, counted from
# the lowest. A type without a name of its own is named by where it sits
# (Parent.member). An enum lists its values; "typedefs" gives each type
# name used, with what it stands for.
"""

# Exit codes.
MATCHES, DIFFERS, CANT_RUN = 0, 1, 2


class CantRun(Exception):
    """The tool can't do its job: its message says what's missing."""


# ---------------------------------------------------------------------------
# Building the probe
# ---------------------------------------------------------------------------

def probe_source():
    """The probe's C source: the headers, one pointer per root, the enum."""
    lines = ['#include "{}"'.format(header) for header in PROBE_HEADERS]
    lines.append("")
    # A pointer is enough: the compiler describes every struct the headers
    # define, and the pointer says which one each root is.
    for i, root in enumerate(ROOTS):
        lines.append("{} *{};".format(root, ROOT_VARIABLE.format(i)))
    lines.append("")
    lines.append("enum SaveFormatProbeConstants {")
    for name in CONSTANTS:
        lines.append("    {}{} = ({}),".format(CONSTANT_PREFIX, name, name))
    lines.append("};")
    lines.append("enum SaveFormatProbeConstants saveFormatConstants;")
    return "\n".join(lines) + "\n"


def find_program(name):
    """`name`'s full path: on PATH, or in $TOOLCHAIN/bin or $DEVKITARM/bin,
    where the Makefile looks too. None when it isn't there."""
    found = shutil.which(name)
    if found:
        return found
    for var in ("TOOLCHAIN", "DEVKITARM"):
        base = os.environ.get(var)
        if base:
            found = shutil.which(name, path=os.path.join(base, "bin"))
            if found:
                return found
    return None


def find_tools(root):
    """The programs the probe needs, or CantRun naming every missing one."""
    missing = []
    agbcc = root / "tools" / "agbcc" / "bin" / "agbcc"
    if not os.access(str(agbcc), os.X_OK):
        missing.append(
            "tools/agbcc (agbcc, the compiler `make firered` uses) is not in "
            "{}. In a new worktree, link it from the main checkout: "
            "ln -s \"$(git worktree list | head -1 | cut -d' ' -f1)/tools/agbcc\" "
            "tools/agbcc".format(root))
    if not (root / "include").is_dir():
        missing.append("{} has no include/ folder".format(root))
    tools = {"agbcc": str(agbcc)}
    for key, name in (("cc", "cc"), ("as", "arm-none-eabi-as"),
                      ("readelf", "arm-none-eabi-readelf")):
        tools[key] = find_program(name)
        if tools[key] is None:
            missing.append("{} is not installed (or not on PATH)".format(name))
    if missing:
        raise CantRun("\n".join("- " + m for m in missing))
    return tools


def run_step(what, command, root, stdin_text=None):
    """Run one build step in `root`; its stdout, or CantRun with its errors."""
    result = subprocess.run(command, cwd=str(root), input=stdin_text,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True)
    if result.returncode != 0:
        errors = result.stderr.strip()
        missing = re.findall(r"([\w./-]+\.h): No such file or directory", errors)
        if missing:
            raise CantRun(
                "- a header is missing: {}. If it's one the build makes (such "
                "as include/constants/map_groups.h), run `make firered` "
                "first.\n{}".format(", ".join(sorted(set(missing))), errors))
        raise CantRun("- {} failed:\n{}".format(what, errors or result.stdout))
    return result.stdout


def compile_probe(root):
    """readelf's text dump of the probe's debug information."""
    tools = find_tools(root)
    with tempfile.TemporaryDirectory(prefix="save_format.") as work:
        work = Path(work)
        source = work / "probe.c"
        source.write_text(probe_source())
        preprocessed = run_step(
            "the preprocessor (cc -E)",
            [tools["cc"], "-E"] + CPP_FLAGS + [str(source)], root)
        assembly = work / "probe.s"
        run_step("agbcc",
                 [tools["agbcc"]] + AGBCC_FLAGS + ["-o", str(assembly)],
                 root, stdin_text=preprocessed)
        with assembly.open("a") as f:
            f.write(ABBREV_TERMINATOR)
        obj = work / "probe.o"
        run_step("arm-none-eabi-as",
                 [tools["as"]] + AS_FLAGS + ["-o", str(obj), str(assembly)], root)
        return run_step("arm-none-eabi-readelf",
                        [tools["readelf"], "--debug-dump=info", str(obj)], root)


# ---------------------------------------------------------------------------
# Reading the debug information
# ---------------------------------------------------------------------------
#
# readelf prints one "debug entry" (a DIE) per type, member, variable and so
# on, as a header line and then one line per attribute:
#
#  <1><95ec>: Abbrev Number: 28 (DW_TAG_structure_type)
#     <95f1>   DW_AT_name        : SaveBlock2
#     <95fc>   DW_AT_byte_size   : 3876
#  <2><9601>: Abbrev Number: 8 (DW_TAG_member)
#     <9602>   DW_AT_name        : playerName
#     <9610>   DW_AT_type        : <0x5805>
#     <9614>   DW_AT_data_member_location: 2 byte block: 23 0  (DW_OP_plus_uconst: 0)
#
# <1> is the entry's depth in the tree (a struct's members are its
# children, one deeper), <95ec> its offset, which other entries use to
# refer to it (DW_AT_type: <0x5805>). "Abbrev Number: 0" ends a list of
# children.

DIE_LINE = re.compile(r"^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: (\d+)(?: \((\w+)\))?")
ATTR_LINE = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s?(.*)$")


class Die:
    """One debug entry: its tag (what it is), attributes and children."""

    def __init__(self, offset, tag):
        self.offset = offset
        self.tag = tag
        self.attrs = {}
        self.children = []

    def name(self):
        return self.attrs.get("DW_AT_name")

    def type_ref(self):
        """The offset of the entry DW_AT_type points at, or None (void)."""
        value = self.attrs.get("DW_AT_type")
        if value is None:
            return None
        match = re.search(r"<0x([0-9a-f]+)>", value)
        return int(match.group(1), 16) if match else None

    def number(self, attr, default=None):
        value = self.attrs.get(attr)
        if value is None:
            return default
        match = re.match(r"\s*(-?(?:0x[0-9a-f]+|\d+))", value)
        return int(match.group(1), 0) if match else default

    def member_offset(self):
        """A member's byte offset in its struct (0 when not given, as in a
        union)."""
        value = self.attrs.get("DW_AT_data_member_location")
        if value is None:
            return 0
        match = re.search(r"DW_OP_plus_uconst: (\d+)", value)
        if match:
            return int(match.group(1))
        return self.number("DW_AT_data_member_location", 0)


def parse_dies(text):
    """Every debug entry in readelf's dump, by offset."""
    dies = {}
    stack = []  # stack[d] is the latest entry at depth d
    current = None
    for line in text.splitlines():
        match = DIE_LINE.match(line)
        if match:
            depth, offset, _, tag = match.groups()
            depth, offset = int(depth), int(offset, 16)
            current = None
            if tag is None:  # "Abbrev Number: 0": end of a list of children
                continue
            current = Die(offset, tag)
            dies[offset] = current
            del stack[depth:]
            if depth > 0 and len(stack) >= depth:
                stack[depth - 1].children.append(current)
            stack.append(current)
            continue
        match = ATTR_LINE.match(line)
        if match and current is not None:
            current.attrs[match.group(1)] = match.group(2).rstrip()
    return dies


# ---------------------------------------------------------------------------
# Describing the save format
# ---------------------------------------------------------------------------

KIND = {
    "DW_TAG_structure_type": "struct",
    "DW_TAG_union_type": "union",
    "DW_TAG_enumeration_type": "enum",
}


def hex_size(size):
    return "{} byte{} (0x{:X})".format(size, "" if size == 1 else "s", size)


class Layout:
    """Walks the types reachable from the roots and writes their sections."""

    def __init__(self, dies):
        self.dies = dies
        self.names = {}       # offset of a struct/union/enum -> its name
        self.queue = []       # offsets of types still to describe
        self.seen = set()
        self.typedefs = {}    # typedef name -> what it stands for

    # -- types as written -------------------------------------------------

    def type_string(self, ref, where):
        """The type at offset `ref` as C writes it: a typedef's name,
        `struct X`, an element type with its array sizes. `where` names the
        place it sits, for a type without a name of its own. Every struct,
        union, enum and typedef met on the way is queued for a section."""
        if ref is None:
            return "void"
        die = self.dies[ref]
        tag = die.tag
        if tag == "DW_TAG_typedef":
            name = die.name()
            if name not in self.typedefs:
                self.typedefs[name] = None  # mark first: a typedef may refer back
                target = die.type_ref()
                text = self.type_string(target, name)
                self.typedefs[name] = "{}, {}".format(text, self.size_text(target))
            return name
        if tag in KIND:
            if ref not in self.names:
                self.names[ref] = die.name() or where
            if ref not in self.seen:
                self.seen.add(ref)
                self.queue.append(ref)
            return "{} {}".format(KIND[tag], self.names[ref])
        if tag == "DW_TAG_base_type":
            return die.name()
        if tag == "DW_TAG_const_type":
            return "const " + self.type_string(die.type_ref(), where)
        if tag == "DW_TAG_volatile_type":
            return "volatile " + self.type_string(die.type_ref(), where)
        if tag == "DW_TAG_array_type":
            dims = ""
            for sub in die.children:
                if sub.tag != "DW_TAG_subrange_type":
                    continue
                upper = sub.number("DW_AT_upper_bound")
                if upper is None:
                    dims += "[]"
                else:
                    # A zero-length array's upper bound is -1, which
                    # readelf may print as 0xffffffff.
                    dims += "[{}]".format(0 if upper >= 0xFFFFFFFF else upper + 1)
            return self.type_string(die.type_ref(), where) + dims
        if tag == "DW_TAG_pointer_type":
            return self.pointer_string(die.type_ref(), where) + " *"
        if tag == "DW_TAG_subroutine_type":
            return "function"
        return "<{}>".format(tag)

    def pointer_string(self, ref, where):
        """What a pointer points at, written without queueing it: the save
        holds the pointer's 4 bytes, never what it points at."""
        if ref is None:
            return "void"
        die = self.dies[ref]
        if die.tag in KIND:
            return "{} {}".format(KIND[die.tag], die.name() or where + "*")
        if die.tag in ("DW_TAG_typedef", "DW_TAG_base_type"):
            return die.name()
        if die.tag == "DW_TAG_const_type":
            return "const " + self.pointer_string(die.type_ref(), where)
        if die.tag == "DW_TAG_volatile_type":
            return "volatile " + self.pointer_string(die.type_ref(), where)
        if die.tag == "DW_TAG_pointer_type":
            return self.pointer_string(die.type_ref(), where) + " *"
        if die.tag == "DW_TAG_subroutine_type":
            return "function"
        return "<{}>".format(die.tag)

    def size_of(self, ref):
        """The size in bytes of the type at `ref`, or None if it has none."""
        if ref is None:
            return None
        die = self.dies[ref]
        size = die.number("DW_AT_byte_size")
        if size is not None:
            return size
        if die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
            return self.size_of(die.type_ref())
        if die.tag == "DW_TAG_pointer_type":
            return 4
        if die.tag == "DW_TAG_array_type":
            size = self.size_of(die.type_ref())
            for sub in die.children:
                upper = sub.number("DW_AT_upper_bound")
                if size is None or upper is None:
                    return None
                size *= 0 if upper >= 0xFFFFFFFF else upper + 1
            return size
        return None

    def size_text(self, ref):
        size = self.size_of(ref)
        return "no size" if size is None else hex_size(size)

    # -- sections -----------------------------------------------------------

    def section(self, ref):
        """The lines describing one struct, union or enum."""
        die = self.dies[ref]
        kind, name = KIND[die.tag], self.names[ref]
        if "DW_AT_declaration" in die.attrs:
            return ["{} {}, never defined".format(kind, name)]
        lines = ["{} {}, {}".format(kind, name, hex_size(die.number("DW_AT_byte_size", 0)))]
        if kind == "enum":
            for child in die.children:
                if child.tag == "DW_TAG_enumerator":
                    lines.append("  {} = {}".format(
                        child.name(), child.number("DW_AT_const_value")))
            return lines
        for member in die.children:
            if member.tag != "DW_TAG_member":
                continue
            member_name = member.name() or "(no name)"
            text = self.type_string(member.type_ref(), name + "." + member_name)
            bits = member.number("DW_AT_bit_size")
            if bits is not None:
                # DWARF 2 counts a bitfield's DW_AT_bit_offset from the top
                # bit of its unit; the GBA is little-endian, so its lowest bit
                # is unit bits - offset - width.
                unit_bits = 8 * member.number("DW_AT_byte_size", self.size_of(member.type_ref()))
                low = unit_bits - member.number("DW_AT_bit_offset", 0) - bits
                if bits == 1:
                    text += ":1, bit {}".format(low)
                else:
                    text += ":{}, bits {}-{}".format(bits, low, low + bits - 1)
            lines.append("  0x{:04X}  {:<28} {}".format(
                member.member_offset(), member_name, text))
        return lines


def find_root_types(dies):
    """The offset of each root type, from the probe's pointers to them."""
    variables = {die.name(): die for die in dies.values() if die.tag == "DW_TAG_variable"}
    found = []
    for i, root in enumerate(ROOTS):
        variable = variables.get(ROOT_VARIABLE.format(i))
        pointer = variable and dies.get(variable.type_ref())
        target = pointer and dies.get(pointer.type_ref())
        if not target or target.tag not in KIND or "DW_AT_declaration" in target.attrs:
            raise CantRun(
                "- the probe can't see {0}'s definition. If it moved to another "
                "header, add that header to PROBE_HEADERS in "
                "test/tools/save_format.py; if it was renamed, rename it in "
                "ROOTS too (a rename is a breaking change).".format(root))
        found.append(target.offset)
    return found


def constant_lines(dies):
    """The constants section's lines, from the probe's enum."""
    values = {}
    for die in dies.values():
        name = die.name() or ""
        if die.tag == "DW_TAG_enumerator" and name.startswith(CONSTANT_PREFIX):
            values[name[len(CONSTANT_PREFIX):]] = die.number("DW_AT_const_value")
    lines = ["constants"]
    for name in sorted(CONSTANTS):
        if name not in values:
            raise CantRun("- the probe's enum has no value for {}".format(name))
        value = values[name] & 0xFFFFFFFF
        lines.append("  {} = {} (0x{:X})".format(name, value, value))
    return lines


def describe(dies):
    """The record's text for these debug entries."""
    layout = Layout(dies)
    roots = find_root_types(dies)
    for ref in roots:
        layout.type_string(ref, None)
    sections = {}
    while layout.queue:  # describing a type may queue the types it holds
        ref = layout.queue.pop(0)
        sections[ref] = layout.section(ref)
    others = sorted((ref for ref in sections if ref not in roots),
                    key=lambda ref: (layout.names[ref], KIND[dies[ref].tag]))
    blocks = [constant_lines(dies)]
    blocks += [sections[ref] for ref in roots + others]
    blocks.append(["typedefs"] + ["  {} = {}".format(name, layout.typedefs[name])
                                  for name in sorted(layout.typedefs)])
    return RECORD_HEADER + "".join("\n" + "\n".join(block) + "\n" for block in blocks)


def save_format(root):
    """The tree's save format, as the record's text."""
    return describe(parse_dies(compile_probe(root)))


# ---------------------------------------------------------------------------
# Checking
# ---------------------------------------------------------------------------

def sections_of(text):
    """The record's sections, by their first line (comments left out)."""
    sections, current = {}, None
    for line in text.splitlines():
        if not line or line.startswith("#"):
            current = None
        elif not line.startswith(" "):
            current = line.split(",")[0]
            sections[current] = [line]
        elif current is not None:
            sections[current].append(line)
    return sections


def changed_sections(old, new):
    """The names of the sections that differ between two records."""
    old, new = sections_of(old), sections_of(new)
    return sorted(name for name in set(old) | set(new) if old.get(name) != new.get(name))


def shown_path(path):
    """`path` from the repo root when it's inside the repo, else as is."""
    try:
        return str(path.resolve().relative_to(REPO))
    except ValueError:
        return str(path)


def write_command(args):
    """The --write command for the tree and record this run used."""
    command = TOOL_COMMAND + " --write"
    if args.root is not None:
        command += " --root " + str(args.root)
    if args.record is not None:
        command += " --record " + str(args.record)
    return command


def explain_difference(record_name, old, new, args):
    """What a mismatch prints: the diff, what it means, the ways out."""
    diff = difflib.unified_diff(
        old.splitlines(), new.splitlines(),
        fromfile=record_name + " (the record)",
        tofile="the save format of " + (str(args.root) if args.root else "this tree"),
        lineterm="")
    return "\n".join([
        "The save format no longer matches {}.".format(record_name),
        "",
        "\n".join(diff),
        "",
        "Changed: " + "; ".join(changed_sections(old, new)),
        "",
        "What this means: a save made before this change would load wrongly,",
        "or not at all. The game would read it from the wrong places, with",
        "no error, because a save carries no version number.",
        "",
        "There are two ways out:",
        "  1. Undo the change.",
        "  2. Treat it as a breaking change, which the owner must approve.",
        "     Even a change old saves survive counts, such as reserved bytes",
        "     given a use. Run",
        "       " + write_command(args),
        "     commit {}, and say so in the PR, in a".format(record_name),
        "     \"## Breaking change\" section.",
        "",
        "See CLAUDE.md, \"Breaking changes\": what counts, and the compatible",
        "ways to prefer.",
    ])


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check the tree's save format against its record, "
                    "test/save_format.txt, or write it there.")
    parser.add_argument("--write", action="store_true",
                        help="write the tree's save format into the record")
    parser.add_argument("--root", type=Path,
                        help="the tree to read (default: this repo)")
    parser.add_argument("--record", type=Path,
                        help="the record (default: test/save_format.txt in this repo)")
    args = parser.parse_args(argv)

    root = (args.root or REPO).resolve()
    record = args.record or DEFAULT_RECORD
    record_name = shown_path(record)

    try:
        new = save_format(root)
        if args.write:
            record.write_text(new)
            print("Wrote the save format of {} into {}.".format(root, record_name))
            return MATCHES
        if not record.is_file():
            raise CantRun("- there is no record at {}. Write one with: {}".format(
                record_name, write_command(args)))
        old = record.read_text()
    except CantRun as problem:
        print("save_format.py can't check the save format:\n{}".format(problem))
        return CANT_RUN

    if old == new:
        print("The save format matches {}.".format(record_name))
        return MATCHES
    print(explain_difference(record_name, old, new, args))
    return DIFFERS


if __name__ == "__main__":
    sys.exit(main())
