#!/usr/bin/env python3
"""
render_cave.py: draw a whole map to a PNG with the game's own tile graphics.

It draws either a vanilla map straight from the repo, or a map dumped from
the running ROM (for example a generated Mt. Moon cave), at 16 x 16 pixels
per map tile, with a ruler of map tile coordinates (every 4 tiles) along the
top and left edges. The README's Mt. Moon before/after pictures are made
with it.

USAGE (python3 standard library only; run from anywhere)

    python3 -I test/tools/render_cave.py INPUT -o OUTPUT.png [--layout NAME]

INPUT is one of:
  * a layout's map.bin from the repo, e.g.
        python3 -I test/tools/render_cave.py data/layouts/MtMoon_B2F/map.bin -o b2f.png
  * a map dump written by test/tools/dump_mtmoon_grid.lua (a text file that
    names its own layout), e.g.
        python3 -I test/tools/render_cave.py /tmp/mgba-test.XXXXXX/MtMoon_B2F_64f4.txt -o after.png
    (that tool's header has the commands that make such a dump);
  * any other file in map.bin format, with --layout naming the layout it
    belongs to, e.g. --layout MtMoon_1F (the layout's directory name under
    data/layouts/, its "name", or its "id" in data/layouts/layouts.json).

WHAT IT READS (source files only, so it works without a build):
  * data/layouts/layouts.json: the layout's width, height and its two tilesets.
  * src/data/tilesets/headers.h, src/data/tilesets/graphics.h and
    src/graphics.c: which directory holds each tileset's files (for the
    cave: data/tilesets/secondary/cave/).
  * include/fieldmap.h: how many tiles, metatiles and palettes belong to the
    primary tileset (NUM_TILES_IN_PRIMARY, ...).
  * In each tileset directory: tiles.png (the 8 x 8 pixel tiles),
    palettes/NN.pal (the colours) and metatiles.bin (how tiles make up each
    16 x 16 metatile).

GBA BACKGROUND, for readers new to it:
  * A map is a grid of METATILES, 16 x 16 pixel blocks. In map.bin, and in the
    game's RAM, each map cell is one 16-bit little-endian number:
        metatile id | collision << 10 | elevation << 12
    Only the low 10 bits (the metatile id) matter for drawing.
  * A metatile is built from eight 8 x 8 pixel TILES in two layers: four for
    the bottom layer (top-left, top-right, bottom-left, bottom-right), then
    four for the top layer, drawn over it. Each tile reference in
    metatiles.bin is 16 bits: tile number (bits 0-9), flipped left-right
    (bit 10), flipped upside down (bit 11), palette slot (bits 12-15).
  * Tile pixels are 4-bit colour numbers (0-15) into the palette slot the
    reference names; colour 0 is transparent. Where both layers are
    transparent, the screen shows the backdrop colour (colour 0 of palette
    slot 0).
  * Every map uses two tilesets: a PRIMARY one (tile and metatile numbers
    below 640, palette slots 0-6) and a SECONDARY one (the rest; for caves,
    data/tilesets/secondary/cave).
  * The GBA stores colours as 5 bits per channel, so each .pal colour
    (8 bits per channel) is cut to 5 bits, as the build does, and stretched
    back to 8 bits for the PNG, giving the colours the game shows.
  * A metatile id with no data in either tileset is drawn as magenta and
    black diagonal stripes, so a broken map cell stands out.

The output is pixel for pixel the same as the prototype renderer the
Mt. Moon design was done with (issue #22), which read the build's converted
files (tiles.4bpp, .gbapal) instead of their sources.
"""

import argparse
import json
import os
import re
import struct
import sys
import zlib

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

TILE_PIXELS = 8          # a tile is 8 x 8 pixels
METATILE_PIXELS = 16     # a metatile is 16 x 16 pixels (2 x 2 tiles)
RULER = 14               # pixels of coordinate ruler above and left of the map
RULER_EVERY = 4          # label every 4th map tile
RULER_BACKGROUND = (40, 40, 40)
RULER_TEXT = (255, 255, 0)
MISSING = ((255, 0, 255), (0, 0, 0))  # stripes for a metatile id with no data

DUMP_MAGIC = "map-dump"


def fail(message):
    sys.exit("render_cave.py: " + message)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def repo_path(relative):
    return os.path.join(REPO, relative)


# ---------------------------------------------------------------------------
# Finding the layout and its tilesets
# ---------------------------------------------------------------------------

def load_layouts():
    with open(repo_path("data/layouts/layouts.json")) as f:
        return [layout for layout in json.load(f)["layouts"] if layout]


def layout_dir_name(layout):
    """MtMoon_B2F for data/layouts/MtMoon_B2F/map.bin."""
    return os.path.basename(os.path.dirname(layout["blockdata_filepath"]))


def find_layout(name):
    """The layouts.json entry whose directory name, name or id is `name`."""
    for layout in load_layouts():
        if name in (layout_dir_name(layout), layout["name"], layout["id"]):
            return layout
    fail("no layout called %r in data/layouts/layouts.json" % name)


def find_layout_by_map_bin(path):
    """The layouts.json entry whose map.bin is the file at `path`, or None."""
    for layout in load_layouts():
        map_bin = repo_path(layout["blockdata_filepath"])
        if os.path.exists(map_bin) and os.path.samefile(map_bin, path):
            return layout
    return None


def read_defines(relative_path):
    """`#define NAME number` lines of a header, as {NAME: number}."""
    defines = {}
    with open(repo_path(relative_path)) as f:
        for line in f:
            m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", line)
            if m:
                defines[m.group(1)] = int(m.group(2), 0)
    return defines


# Where the tilesets' tile arrays are defined: most in graphics.h, the
# primary tilesets' in src/graphics.c.
TILE_ARRAY_SOURCES = ("src/data/tilesets/graphics.h", "src/graphics.c")


def tileset_dir(tileset_symbol):
    """The directory holding a tileset's files, e.g. gTileset_Cave ->
    data/tilesets/secondary/cave. headers.h names the tileset's tile array
    (.tiles = gTilesetTiles_Cave), and that array is INCBINed (copied into the
    ROM at build time) from a file in the tileset's directory."""
    with open(repo_path("src/data/tilesets/headers.h")) as f:
        headers = f.read()
    m = re.search(r"\b%s\s*=\s*\{(.*?)\};" % re.escape(tileset_symbol), headers, re.S)
    if not m:
        fail("tileset %s not found in src/data/tilesets/headers.h" % tileset_symbol)
    tiles = re.search(r"\.tiles\s*=\s*(\w+)", m.group(1)).group(1)
    for source in TILE_ARRAY_SOURCES:
        with open(repo_path(source)) as f:
            m = re.search(r"\b%s\[\]\s*=\s*INCBIN_\w+\(\"([^\"]+)\"\)" % re.escape(tiles), f.read())
        if m:
            return os.path.dirname(m.group(1))
    fail("%s (the tiles of %s) not found in %s" % (tiles, tileset_symbol, " or ".join(TILE_ARRAY_SOURCES)))


# ---------------------------------------------------------------------------
# Reading the tileset files
# ---------------------------------------------------------------------------

def read_png_tiles(path):
    """The 8 x 8 tiles of a 4-bit indexed PNG (how tiles.png is stored), as a
    list of 64 colour numbers per tile, tiles in reading order (the build's
    tiles.4bpp has them in the same order).

    A PNG stores its pixels zlib-compressed, row by row. Each row starts with
    a FILTER byte saying how its bytes were predicted from the bytes to the
    left and above (to compress better); undoing that gives the raw row, two
    pixels per byte here, the left one in the high 4 bits."""
    data = read_bytes(path)
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("%s is not a PNG" % path)
    pos, idat = 8, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    if (depth, color_type, interlace) != (4, 3, 0):
        fail("%s: expected a 4-bit indexed, non-interlaced PNG" % path)

    stride = (width * depth + 7) // 8
    raw = zlib.decompress(idat)
    pixels = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_type = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - 1] if x > 0 else 0          # bytes per pixel < 1: use 1
            up = previous[x]
            up_left = previous[x - 1] if x > 0 else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 255
            elif filter_type == 2:
                row[x] = (row[x] + up) & 255
            elif filter_type == 3:
                row[x] = (row[x] + (left + up) // 2) & 255
            elif filter_type == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                row[x] = (row[x] + predictor) & 255
        line = []
        for byte in row:
            line.append(byte >> 4)
            line.append(byte & 15)
        pixels.append(line[:width])
        previous = row

    tiles = []
    for ty in range(height // TILE_PIXELS):
        for tx in range(width // TILE_PIXELS):
            tiles.append([pixels[ty * TILE_PIXELS + py][tx * TILE_PIXELS + px]
                          for py in range(TILE_PIXELS) for px in range(TILE_PIXELS)])
    return tiles


def read_palette(path):
    """A JASC .pal file's 16 colours as they look on the GBA: each 8-bit
    channel cut to the GBA's 5 bits (as the build's .gbapal is), then
    stretched back to 8 bits by repeating its top bits ((v << 3) | (v >> 2))."""
    with open(path) as f:
        lines = f.read().split("\n")
    if lines[0].strip() != "JASC-PAL":
        fail("%s is not a JASC palette" % path)
    colours = []
    for line in lines[3:3 + 16]:
        channels = [int(v) >> 3 for v in line.split()]
        colours.append(tuple((v << 3) | (v >> 2) for v in channels))
    return colours


class LayoutTilesets:
    """The primary and secondary tilesets of one layout, merged the way the
    game loads them into video memory."""

    def __init__(self, layout):
        sizes = read_defines("include/fieldmap.h")
        self.primary_tiles = sizes["NUM_TILES_IN_PRIMARY"]
        self.primary_metatiles = sizes["NUM_METATILES_IN_PRIMARY"]
        primary_palettes = sizes["NUM_PALS_IN_PRIMARY"]

        primary = repo_path(tileset_dir(layout["primary_tileset"]))
        secondary = repo_path(tileset_dir(layout["secondary_tileset"]))
        self.tiles = (read_png_tiles(os.path.join(primary, "tiles.png")),
                      read_png_tiles(os.path.join(secondary, "tiles.png")))
        self.metatiles = (read_bytes(os.path.join(primary, "metatiles.bin")),
                          read_bytes(os.path.join(secondary, "metatiles.bin")))
        # Palette slots 0-6 come from the primary tileset, the rest from the
        # secondary one.
        self.palettes = [read_palette(os.path.join(primary if slot < primary_palettes else secondary,
                                                   "palettes", "%02d.pal" % slot))
                         for slot in range(16)]
        self.backdrop = self.palettes[0][0]
        self.cache = {}

    def tile(self, number):
        """Tile `number`'s 64 colour numbers; all transparent past the end."""
        if number < self.primary_tiles:
            tiles, index = self.tiles[0], number
        else:
            tiles, index = self.tiles[1], number - self.primary_tiles
        return tiles[index] if index < len(tiles) else [0] * 64

    def metatile(self, metatile_id):
        """Metatile `metatile_id` as 16 rows of 16 RGB colours."""
        if metatile_id in self.cache:
            return self.cache[metatile_id]
        if metatile_id < self.primary_metatiles:
            data, offset = self.metatiles[0], metatile_id * 16
        else:
            data, offset = self.metatiles[1], (metatile_id - self.primary_metatiles) * 16
        image = [[self.backdrop] * METATILE_PIXELS for _ in range(METATILE_PIXELS)]
        if offset + 16 > len(data):
            image = [[MISSING[(x + y) % 4 >= 2] for x in range(METATILE_PIXELS)]
                     for y in range(METATILE_PIXELS)]
        else:
            references = struct.unpack_from("<8H", data, offset)
            for i, reference in enumerate(references):   # bottom layer (0-3), then top (4-7)
                quarter = i % 4
                left, top = (quarter & 1) * TILE_PIXELS, (quarter >> 1) * TILE_PIXELS
                pixels = self.tile(reference & 0x3FF)
                flip_x, flip_y = (reference >> 10) & 1, (reference >> 11) & 1
                palette = self.palettes[(reference >> 12) & 15]
                for y in range(TILE_PIXELS):
                    for x in range(TILE_PIXELS):
                        colour = pixels[(7 - y if flip_y else y) * TILE_PIXELS + (7 - x if flip_x else x)]
                        if colour:
                            image[top + y][left + x] = palette[colour]
        self.cache[metatile_id] = image
        return image


# ---------------------------------------------------------------------------
# Reading the map
# ---------------------------------------------------------------------------

def read_map_bin(path, layout):
    data = read_bytes(path)
    count = layout["width"] * layout["height"]
    if len(data) != count * 2:
        fail("%s has %d cells, layout %s is %d x %d" % (
            path, len(data) // 2, layout["name"], layout["width"], layout["height"]))
    return list(struct.unpack("<%dH" % count, data))


def read_dump(path):
    """A dump from test/tools/dump_mtmoon_grid.lua:
        map-dump 1
        layout MtMoon_B2F          the layout's directory name
        size 48 40                 width and height in map tiles
        ...                        other "key value" lines, for information
        cells
        3281 3281 0681 ...         one line per map row, each map cell in hex
    Returns (layout, cells)."""
    with open(path) as f:
        lines = [line.strip() for line in f]
    header, rows = {}, None
    for i, line in enumerate(lines[1:], 1):
        if line == "cells":
            rows = [row for row in lines[i + 1:] if row]
            break
        key, _, value = line.partition(" ")
        header[key] = value
    if rows is None or "layout" not in header or "size" not in header:
        fail("%s: not a complete map dump" % path)
    layout = find_layout(header["layout"])
    width, height = (int(v) for v in header["size"].split())
    if (width, height) != (layout["width"], layout["height"]) or len(rows) != height:
        fail("%s: %d x %d with %d rows, layout %s is %d x %d" % (
            path, width, height, len(rows), header["layout"], layout["width"], layout["height"]))
    cells = []
    for row in rows:
        values = [int(v, 16) for v in row.split()]
        if len(values) != width:
            fail("%s: a row has %d cells, expected %d" % (path, len(values), width))
        cells.extend(values)
    return layout, cells


def read_input(path, layout_name):
    with open(path, "rb") as f:
        is_dump = f.read(len(DUMP_MAGIC)) == DUMP_MAGIC.encode()
    if is_dump:
        return read_dump(path)
    layout = find_layout(layout_name) if layout_name else find_layout_by_map_bin(path)
    if layout is None:
        fail("%s isn't a layout's map.bin in this repo: name its layout with --layout" % path)
    return layout, read_map_bin(path, layout)


# ---------------------------------------------------------------------------
# Drawing
# ---------------------------------------------------------------------------

# A 3 x 5 pixel font for the ruler's numbers: each glyph is 5 rows of 3 bits.
DIGITS = {
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111",
    "3": "111001111001111", "4": "101101111001001", "5": "111100111001111",
    "6": "111100111101111", "7": "111001001010010", "8": "111101111101111",
    "9": "111101111001111",
}


def draw_number(image, x, y, number):
    for digit in str(number):
        glyph = DIGITS[digit]
        for gy in range(5):
            for gx in range(3):
                if glyph[gy * 3 + gx] == "1":
                    image[y + gy][x + gx] = RULER_TEXT
        x += 4


def render(tileset, cells, width, height):
    """The map as rows of RGB colours, with the coordinate ruler."""
    image = [[RULER_BACKGROUND] * (RULER + width * METATILE_PIXELS)
             for _ in range(RULER + height * METATILE_PIXELS)]
    for ty in range(height):
        for tx in range(width):
            metatile = tileset.metatile(cells[ty * width + tx] & 0x3FF)
            left, top = RULER + tx * METATILE_PIXELS, RULER + ty * METATILE_PIXELS
            for y in range(METATILE_PIXELS):
                image[top + y][left:left + METATILE_PIXELS] = metatile[y]
    for tx in range(0, width, RULER_EVERY):
        draw_number(image, RULER + tx * METATILE_PIXELS + 2, 4, tx)
    for ty in range(0, height, RULER_EVERY):
        draw_number(image, 1, RULER + ty * METATILE_PIXELS + 5, ty)
    return image


def write_png(path, image):
    """An 8-bit RGB PNG: each row is a filter byte (0, none) and its pixels."""
    height, width = len(image), len(image[0])
    raw = bytearray()
    for row in image:
        raw.append(0)
        for colour in row:
            raw += bytes(colour)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(
        description="Draw a map (a vanilla map.bin or a dump from the running ROM) to a PNG.")
    parser.add_argument("input", help="a layout's map.bin, or a dump from test/tools/dump_mtmoon_grid.lua")
    parser.add_argument("-o", "--output", required=True, help="the PNG to write")
    parser.add_argument("--layout", help="the layout a map.bin-format INPUT belongs to (e.g. MtMoon_1F)")
    args = parser.parse_args()

    layout, cells = read_input(args.input, args.layout)
    tileset = LayoutTilesets(layout)
    write_png(args.output, render(tileset, cells, layout["width"], layout["height"]))
    print("%s: %s, %d x %d tiles" % (args.output, layout_dir_name(layout), layout["width"], layout["height"]))


if __name__ == "__main__":
    main()
