#!/usr/bin/env python3
"""Builds a world AppVar (.8xv) for the engine: the file format that src/world.h describes.

Usage: python3 utils/make_world.py [out.8xv] [--preset test|dense|random] [--name NAME]
                                   [--appvar WORLD] [--ram] [--bin out.bin]

  --preset   test   (default) a 6 x 6 chunk world with features across chunk borders
             dense  every chunk near the 255 voxel limit (benchmarks, worst case)
             random random boxes of random size, colour and height scattered over the ground plane
  --appvar   the AppVar name on the calculator (the engine opens WORLD_APPVAR, "WORLD")
  --ram      send it to RAM instead of the archive
  --bin      also write the raw AppVar contents

File format (all little endian, src/world.h has the same as C structs):

  header, 48 bytes
    0   4  magic "VXWD"
    4   1  format version (2)
    5   4  CRC-32 (zlib's) of every byte after this field, to the end of the file
    9  16  world name, NUL terminated (at most 15 characters)
    25  7  last played: year (2 bytes), month, day, hour, minute, second. Year 0: never
    32  1  size_x: chunks along x
    33  1  size_z: chunks along z
    34  2  spawn cell x (int16)
    36  2  spawn cell z (int16)
    38  1  spawn yaw (256 steps per turn, 0 looks along +z)
    39  1  ground plane material (0 .. 15), or 255: none
    40  8  reserved, 0
  chunk index, 3 bytes per chunk, chunk (cx, cz) at entry cz * size_x + cx
    0   2  offset of its voxels from the start of the file
    2   1  voxel count (0 .. 255)
  voxels, 4 bytes each
    0   1  x + 8 z, x and z within the chunk (0 .. 7): world cell (8 cx + x, 8 cz + z)
    1   1  y cell (int8, -96 .. 95). y grows downward and the ground is at y = 0, so a floor
           tile is y = 0 and a block standing on it y = -1
    2   1  material (0 .. 15, a palette colour)
    3   1  exposed faces: +x 1, -x 2, +z 4, -z 8, top 16, bottom 32 (engine.h FACE_*): the
           faces whose neighbouring cell is empty (outside the world counts as empty)

A chunk is a column: 8 x 8 cells, any height, at most 255 voxels.

With a ground plane, every cell at y >= 0 is solid ground of that material, drawn by the engine as
the background below the horizon, for free. The file then holds no voxels of the ground material
at y >= 0 and none below y = 0 (they could never be seen); voxels of other materials at y = 0
(paths, patches) are drawn over the ground, top face only (the ground counts as solid for the
exposed faces).
"""

import argparse
import struct
import zlib

CHUNK = 8
CHUNK_MAX = 255
Y_MIN, Y_MAX = -96, 95
HEADER_SIZE = 48
VERSION = 2
NO_GROUND = 255
FACES = ((1, 0, 0, 0x01), (-1, 0, 0, 0x02), (0, 0, 1, 0x04), (0, 0, -1, 0x08),
         (0, -1, 0, 0x10), (0, 1, 0, 0x20))         # neighbour offset, FACE_* (y grows downward)


class World:
    def __init__(self, size_x, size_z, name, ground=None):
        self.size_x, self.size_z, self.name = size_x, size_z, name
        self.ground = ground            # ground plane material, or None
        self.cells = {}                 # (x, y, z) -> material
        self.spawn = (size_x * CHUNK // 2, size_z * CHUNK // 2, 0)

    def inside(self, x, z):
        return 0 <= x < self.size_x * CHUNK and 0 <= z < self.size_z * CHUNK

    def set(self, x, y, z, mat):
        if self.inside(x, z):
            assert Y_MIN <= y <= Y_MAX, (x, y, z)
            if self.ground is not None and (y > 0 or (y == 0 and mat == self.ground)):
                self.cells.pop((x, y, z), None)    # part of the ground plane
                return
            self.cells[(x, y, z)] = mat

    def occupied(self, x, y, z):
        return (x, y, z) in self.cells or (self.ground is not None and y >= 0)

    def faces(self, x, y, z):
        return sum(bit for dx, dy, dz, bit in FACES if not self.occupied(x + dx, y + dy, z + dz))

    def box(self, x0, x1, y0, y1, z0, z1, mat):
        """every cell with x0 <= x <= x1 and so on (inclusive)"""
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                for z in range(z0, z1 + 1):
                    self.set(x, y, z, mat)

    def tree(self, x, z, height=3, trunk=4, leaves=11):
        for y in range(-1, -height - 1, -1):
            self.set(x, y, z, trunk)
        top = -height - 1
        for dx, dz in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
            self.set(x + dx, top, z + dz, leaves)
        self.set(x, top - 1, z, leaves)

    def pack(self):
        chunks = [[] for _ in range(self.size_x * self.size_z)]
        for (x, y, z), mat in sorted(self.cells.items()):
            cx, cz = x // CHUNK, z // CHUNK
            chunks[cz * self.size_x + cx].append((x % CHUNK + CHUNK * (z % CHUNK), y, mat,
                                                  self.faces(x, y, z)))
        for i, c in enumerate(chunks):
            if len(c) > CHUNK_MAX:
                raise SystemExit(f"chunk ({i % self.size_x}, {i // self.size_x}) has {len(c)} "
                                 f"voxels, at most {CHUNK_MAX}")

        name = self.name.encode("ascii")
        assert len(name) <= 15, "the name is at most 15 characters"
        sx, sz, syaw = self.spawn
        index_size = 3 * len(chunks)
        body = bytearray()
        index = bytearray()
        offset = HEADER_SIZE + index_size
        for c in chunks:
            index += struct.pack("<HB", offset, len(c))
            for v in c:
                body += struct.pack("<BbBB", *v)
            offset += 4 * len(c)
        assert offset <= 65505, f"world too large for one AppVar: {offset} bytes"

        after_crc = (name.ljust(16, b"\0") + struct.pack("<HBBBBB", 0, 0, 0, 0, 0, 0) +
                     struct.pack("<BBhhBB", self.size_x, self.size_z, sx, sz, syaw,
                                 NO_GROUND if self.ground is None else self.ground) +
                     bytes(8) + index + body)
        data = b"VXWD" + bytes([VERSION]) + struct.pack("<I", zlib.crc32(after_crc)) + after_crc
        assert len(data) == offset
        return data, chunks


def to_8xv(data, appvar, archived):
    """a TI-83 Premium CE / TI-84 Plus CE variable file holding one AppVar"""
    name = appvar.encode("ascii")
    assert 1 <= len(name) <= 8
    var_data = struct.pack("<H", len(data)) + data
    entry = (struct.pack("<HHB", 13, len(var_data), 0x15) + name.ljust(8, b"\0") +
             bytes([0, 0x80 if archived else 0]) + struct.pack("<H", len(var_data)) + var_data)
    comment = b"voxel world".ljust(42, b"\0")
    return (b"**TI83F*\x1a\x0a\x00" + comment + struct.pack("<H", len(entry)) + entry +
            struct.pack("<H", sum(entry) & 0xffff))


def floor(w, a=3, b=11):
    for x in range(w.size_x * CHUNK):
        for z in range(w.size_z * CHUNK):
            w.set(x, 0, z, a if (x + z) % 2 == 0 else b)


def preset_test():
    """6 x 6 chunks (48 x 48 cells) on a green ground plane, spawn in the middle facing +z. Most
    features cross chunk borders (multiples of 8), to exercise the painter order between chunks"""
    w = World(6, 6, "Test World", ground=3)
    n = 6 * CHUNK

    # on the ground plane: a checkered plaza around the spawn and a sand path to the house door
    for x in range(18, 31):
        for z in range(18, 31):
            if (x + z) % 2:
                w.set(x, 0, z, 11)
    for z in range(22, 30):
        w.set(16, 0, z, 15)

    # a low wall around the edge of the world
    for i in range(n):
        for y in (-1, -2):
            for x, z in ((i, 0), (i, n - 1), (0, i), (n - 1, i)):
                w.set(x, y, z, 6)

    # around the spawn (24, 24): roughly the old single-scene layout
    w.box(21, 21, -2, -1, 26, 31, 6)          # a wall, across the z = 32 border... ends at 31
    w.box(27, 27, -4, -1, 24, 24, 8)          # a red pillar
    w.box(24, 26, -5, -5, 28, 28, 12)         # a floating slab
    w.box(25, 25, -5, -5, 29, 29, 12)
    w.tree(29, 29)

    # a house across the x = 16 border: walls 3 high with a door, a flat roof
    x0, x1, z0, z1 = 13, 19, 30, 35
    for y in (-1, -2, -3):
        w.box(x0, x1, y, y, z0, z0, 9)
        w.box(x0, x1, y, y, z1, z1, 9)
        w.box(x0, x0, y, y, z0, z1, 9)
        w.box(x1, x1, y, y, z0, z1, 9)
    for y in (-1, -2):
        w.cells.pop((16, y, z0), None)          # the door, facing -z
    w.box(x0, x1, -4, -4, z0, z1, 2)

    # a stepped hill centred on the chunk corner (40, 8): 4 chunks share it
    for level, r in enumerate((5, 4, 3, 2, 1)):
        y = -1 - level
        for x in range(40 - r, 40 + r):
            for z in range(8 - r, 8 + r):
                w.set(x, y, z, 15 if level < 4 else 10)

    # a row of pillars along x, one every 3 cells, crossing three chunks
    for x in range(6, 42, 3):
        w.box(x, x, -3, -1, 18, 18, 13)

    # a long wall with gaps along the z = 40 chunk border (cells 39 and 40)
    for x in range(4, 44):
        if x % 10 in (3, 4):
            continue
        w.box(x, x, -2, -1, 40, 40, 14)

    # trees
    for x, z in ((5, 5), (11, 9), (42, 30), (36, 36), (6, 28), (30, 6), (44, 44), (12, 44),
                 (40, 22), (26, 42)):
        w.tree(x, z)

    # a staircase going up towards +x, ending in a platform across the x = 8 / z = 8 corner
    for i in range(5):
        w.box(3 + i, 3 + i, -1 - i, -1, 12, 14, 7)
    w.box(8, 11, -5, -5, 10, 15, 7)

    # distant towers in the corners (level of detail, far plane)
    for x, z in ((2, 2), (45, 2), (2, 45), (45, 45)):
        w.box(x, x, -8, -1, z, z, 8)

    w.spawn = (24, 24, 0)
    return w


def preset_dense():
    """6 x 6 chunks, each close to the 255 voxel limit: a floor of voxels (no ground plane) plus
    columns of varying height"""
    w = World(6, 6, "Dense World")
    n = 6 * CHUNK
    floor(w)
    seed = 1
    for x in range(n):
        for z in range(n):
            seed = (seed * 1103515245 + 12345) & 0x7fffffff
            h = (seed >> 16) % 7                # 3 per column on average
            if h:
                w.box(x, x, -h, -1, z, z, 1 + (seed >> 8) % 15)
    # trim each chunk to the limit, highest voxels first
    by_chunk = {}
    for (x, y, z) in w.cells:
        by_chunk.setdefault((x // CHUNK, z // CHUNK), []).append((y, x, z))
    for cells in by_chunk.values():
        cells.sort()
        for y, x, z in cells[:max(0, len(cells) - CHUNK_MAX)]:
            del w.cells[(x, y, z)]
    w.spawn = (24, 24, 0)
    return w


def preset_random():
    """6 x 6 chunks on a ground plane with about 120 random boxes (1-3 cells wide and deep, 1-5
    high, random material), spawn in the middle with a clear patch around it. Fixed seed"""
    import random
    rng = random.Random(84)
    w = World(6, 6, "Random Boxes", ground=3)
    n = 6 * CHUNK
    for _ in range(120):
        x, z = rng.randrange(n), rng.randrange(n)
        if abs(x - 24) < 3 and abs(z - 24) < 3:
            continue
        dx, dz, h = rng.randint(0, 2), rng.randint(0, 2), rng.randint(1, 5)
        mat = rng.choice([m for m in range(1, 16) if m != 3])
        w.box(x, x + dx, -h, -1, z, z + dz, mat)
    w.spawn = (24, 24, 0)
    return w


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", nargs="?", default="bin/WORLD.8xv")
    ap.add_argument("--preset", default="test", choices=("test", "dense", "random"))
    ap.add_argument("--name")
    ap.add_argument("--appvar", default="WORLD")
    ap.add_argument("--ram", action="store_true")
    ap.add_argument("--bin")
    a = ap.parse_args()

    w = {"test": preset_test, "dense": preset_dense, "random": preset_random}[a.preset]()
    if a.name:
        w.name = a.name
    data, chunks = w.pack()
    with open(a.out, "wb") as f:
        f.write(to_8xv(data, a.appvar, not a.ram))
    if a.bin:
        with open(a.bin, "wb") as f:
            f.write(data)
    counts = [len(c) for c in chunks]
    print(f"wrote {a.out}: \"{w.name}\", {w.size_x} x {w.size_z} chunks, {sum(counts)} voxels "
          f"(at most {max(counts)} per chunk), {len(data)} bytes")


if __name__ == "__main__":
    main()
