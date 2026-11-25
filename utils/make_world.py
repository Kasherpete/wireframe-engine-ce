#!/usr/bin/env python3
"""Builds a world AppVar (.8xv) for the engine: the file format that src/world.h describes.

Usage: python3 utils/make_world.py [out.8xv] [--preset PRESET] [--name NAME]
                                   [--appvar WORLD] [--ram] [--bin out.bin]

  --preset   test     (default) a 6 x 6 chunk world with features across chunk borders
             dense    every chunk near the 255 voxel limit (benchmarks, worst case)
             random   random boxes of random size, colour and height scattered over the ground plane
             tutorial 24 x 24 chunks: a hub village, paths to a forest, a lake, a hill and a tower
             islands  24 x 24 chunks of sea with small islands (huts, trees, a lighthouse)
             plains   24 x 24 chunks of open grassland with far-apart landmarks
             The last three are large but sparse (a few features in view at once), and stay
             under BIG_WORLD_MAX bytes so Save & Quit can unarchive them in free RAM
  --appvar   the AppVar name on the calculator (the engine lists every AppVar starting with "VXWD", whatever its name)
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
    40  1  1: a saved player position follows (written by the game's Save & Quit), 0: none
    41  2  saved cell x (int16)
    43  2  saved cell z (int16)
    45  1  saved feet cell y (int8)
    46  1  saved yaw (256 steps per turn)
    47  1  reserved, 0
  chunk index, 4 bytes per chunk, chunk (cx, cz) at entry cz * size_x + cx
    0   2  offset of its records from the start of the file
    2   1  record count (0 .. 255): voxels and runs
    3   1  run orientation: 0 none, 1 x (runs of +x / -x faces), 2 z (runs of +z / -z faces)
  records, 4 bytes each: a voxel or a run
    0   1  x + 8 z, x and z within the chunk (0 .. 7): world cell (8 cx + x, 8 cz + z)
    1   1  y cell (int8, -96 .. 95). y grows downward and the ground is at y = 0, so a floor
           tile is y = 0 and a block standing on it y = -1
    voxel:
    2   1  material (0 .. 15, a palette colour)
    3   1  exposed faces: +x 1, -x 2, +z 4, -z 8, top 16, bottom 32 (engine.h FACE_*): the
           faces whose neighbouring cell is empty (outside the world counts as empty). Plus 64
           (FACE_RUN): every exposed face of the chunk orientation is drawn by a run
    run (merged faces, src/chunks.h):
    2   1  material + 16 (length - 1), length 1 .. 8 cells
    3   1  128 + the one face (FACE_*) of the chunk orientation: the faces of the cells (x, y,
           z + i) for an x run, (x + i, y, z) for a z run, i = 0 .. length - 1, all in the chunk

A chunk is a column: 8 x 8 cells, any height, at most 255 records, its voxels first, then its runs.

Runs: in each chunk the coplanar exposed side faces of one orientation (the one that saves the most
faces) are merged along the horizontal into runs of up to 8 faces of one material, one cell tall.
Rows of fewer than 3 faces stay faces (a run costs about what 2 save). A voxel any of whose faces of
that orientation is in a run of 3 or more gets FACE_RUN, and runs of
one for its other exposed faces of that orientation, so the engine can leave all of them out of the
voxel. With no room (voxels + runs > 255) the chunk has no runs. Cells at y = 0 are in no run: the
engine never draws them as level of detail rectangles, and runs stop at that distance.

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
VERSION = 3
NO_GROUND = 255
FACE_RUN = 0x40                 # a voxel record: its exposed faces of the chunk orientation are in runs
RUN = 0x80                      # a run record
RUN_MAX = 8                     # cells per run (a chunk row)
RUN_MIN = 3                     # shorter rows of faces stay faces: a run costs about what 2 save
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

    def runs(self, cells, orient):
        """The runs of one chunk (cells: (x, y, z) world -> material) for orientation 1 (x faces,
        along z) or 2 (z faces, along x): (records, faces saved), records as (x, y, z, material,
        length, face) plus the voxels flagged FACE_RUN, or ([], 0)"""
        bits = (0x01, 0x02) if orient == 1 else (0x04, 0x08)
        runs, covered = [], set()
        for bit in bits:
            lines = {}              # (fixed coordinate, y, material) -> positions along the run axis
            for (x, y, z), mat in cells.items():
                if y != 0 and self.faces(x, y, z) & bit:     # y = 0: never a rectangle (project.S)
                    key, pos = ((x, y, mat), z) if orient == 1 else ((z, y, mat), x)
                    lines.setdefault(key, []).append(pos)
            for (fixed, y, mat), ps in lines.items():
                ps.sort()
                start = 0
                for i in range(1, len(ps) + 1):
                    if i < len(ps) and ps[i] == ps[i - 1] + 1:
                        continue
                    n = i - start           # ps[start .. i - 1] are adjacent
                    if n >= RUN_MIN:
                        pieces = (n + RUN_MAX - 1) // RUN_MAX
                        p0 = ps[start]
                        for k in range(pieces):  # balanced: lengths differ by at most 1
                            a, b = p0 + n * k // pieces, p0 + n * (k + 1) // pieces
                            runs.append((fixed, y, a, mat, b - a, bit))
                            for p in range(a, b):
                                covered.add(((fixed, y, p), bit))
                    start = i
        saved = sum(r[4] - 1 for r in runs)
        if not saved:
            return [], 0, set()
        # every voxel with a face in a run: its other exposed faces of the orientation as runs of one
        flagged = set()
        for (fixed, y, p), bit in list(covered):
            cell = (fixed, y, p) if orient == 1 else (p, y, fixed)
            flagged.add(cell)
        for cell in flagged:
            x, y, z = cell
            fixed, p = (x, z) if orient == 1 else (z, x)
            for bit in bits:
                if self.faces(x, y, z) & bit and ((fixed, y, p), bit) not in covered:
                    runs.append((fixed, y, p, cells[cell], 1, bit))
        recs = []
        for fixed, y, p, mat, n, bit in runs:
            x, z = (fixed, p) if orient == 1 else (p, fixed)
            recs.append((x, y, z, mat, n, bit))
        return recs, saved, flagged

    def pack(self):
        by_chunk = {}
        for (x, y, z), mat in self.cells.items():
            by_chunk.setdefault((x // CHUNK, z // CHUNK), {})[(x, y, z)] = mat
        chunks, orients = [], []
        self.saved = 0
        for cz in range(self.size_z):
            for cx in range(self.size_x):
                cells = by_chunk.get((cx, cz), {})
                if len(cells) > CHUNK_MAX:
                    raise SystemExit(f"chunk ({cx}, {cz}) has {len(cells)} voxels, at most {CHUNK_MAX}")
                best = ([], 0, set(), 0)
                for orient in (1, 2):
                    recs, saved, flagged = self.runs(cells, orient)
                    if saved > best[1] and len(cells) + len(recs) <= CHUNK_MAX:
                        best = (recs, saved, flagged, orient)
                recs, saved, flagged, orient = best
                self.saved += saved
                c = []
                for (x, y, z), mat in sorted(cells.items()):
                    faces = self.faces(x, y, z) | (FACE_RUN if (x, y, z) in flagged else 0)
                    c.append((x % CHUNK + CHUNK * (z % CHUNK), y, mat, faces))
                for x, y, z, mat, n, bit in sorted(recs):
                    assert (z if orient == 1 else x) % CHUNK + n <= CHUNK
                    c.append((x % CHUNK + CHUNK * (z % CHUNK), y, mat + 16 * (n - 1), RUN | bit))
                chunks.append(c)
                orients.append(orient)

        name = self.name.encode("ascii")
        assert len(name) <= 15, "the name is at most 15 characters"
        sx, sz, syaw = self.spawn
        index_size = 4 * len(chunks)
        body = bytearray()
        index = bytearray()
        offset = HEADER_SIZE + index_size
        for c, orient in zip(chunks, orients):
            index += struct.pack("<HBB", offset, len(c), orient)
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


# ---- the big, sparse worlds ----
# Large in area, but with features far apart: from anywhere only a few are within the render
# distance (24), so the box count stays near the test world's
BIG = 24                # chunks per side: 192 x 192 cells
BIG_WORLD_MAX = 30000   # bytes: Save & Quit unarchives the world, which needs that much free RAM

DARK_BLUE, PURPLE, DARK_GREEN, BROWN, DARK_GREY = 1, 2, 3, 4, 5
GREY, WHITE, RED, ORANGE, YELLOW, GREEN, BLUE, LAVENDER, PINK, SAND = 6, 7, 8, 9, 10, 11, 12, 13, 14, 15


def house(w, x0, z0, wd, dp, h=3, wall=ORANGE, roof=PURPLE, door="-z", base=0):
    """walls h high around [x0, x0 + wd) x [z0, z0 + dp) standing on y = base, a 2-high door in
    the middle of one side ("-z", "+z", "-x", "+x"), a flat roof"""
    x1, z1 = x0 + wd - 1, z0 + dp - 1
    for y in range(base - 1, base - h - 1, -1):
        w.box(x0, x1, y, y, z0, z0, wall)
        w.box(x0, x1, y, y, z1, z1, wall)
        w.box(x0, x0, y, y, z0, z1, wall)
        w.box(x1, x1, y, y, z0, z1, wall)
    dx, dz = {"-z": ((x0 + x1) // 2, z0), "+z": ((x0 + x1) // 2, z1),
              "-x": (x0, (z0 + z1) // 2), "+x": (x1, (z0 + z1) // 2)}[door]
    for y in (base - 1, base - 2):
        w.cells.pop((dx, y, dz), None)
    w.box(x0, x1, base - h - 1, base - h - 1, z0, z1, roof)


def big_tree(w, x, z, height=4, base=0):
    """a trunk on y = base and a 3 x 3 crown topped by a cross"""
    for y in range(base - 1, base - height - 1, -1):
        w.set(x, y, z, BROWN)
    top = base - height - 1
    w.box(x - 1, x + 1, top, top, z - 1, z + 1, GREEN)
    for dx, dz in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
        w.set(x + dx, top - 1, z + dz, GREEN)


def path(w, a, b, mat=SAND):
    """a 1-wide path on the ground from cell a to cell b (along x, then along z)"""
    (xa, za), (xb, zb) = a, b
    for x in range(min(xa, xb), max(xa, xb) + 1):
        w.cells.setdefault((x, 0, za), mat)
    for z in range(min(za, zb), max(za, zb) + 1):
        w.cells.setdefault((xb, 0, z), mat)


def hill(w, cx, cz, radii, mats):
    """a stepped hill: level k a square of half-size radii[k], one cell high"""
    for level, r in enumerate(radii):
        y = -1 - level
        w.box(cx - r, cx + r, y, y, cz - r, cz + r, mats[min(level, len(mats) - 1)])


def tower(w, cx, cz, r=2, h=8, wall=GREY, top=RED, battlements=True, base=0):
    """a hollow square tower (walls r from the centre) on y = base, a door facing -z, a roof"""
    for y in range(base - 1, base - h - 1, -1):
        for i in range(-r, r + 1):
            for x, z in ((cx + i, cz - r), (cx + i, cz + r), (cx - r, cz + i), (cx + r, cz + i)):
                w.set(x, y, z, wall)
    for y in (base - 1, base - 2):
        w.cells.pop((cx, y, cz - r), None)
    w.box(cx - r, cx + r, base - h - 1, base - h - 1, cz - r, cz + r, top)
    if battlements:
        for i in range(-r, r + 1, 2):
            for x, z in ((cx + i, cz - r), (cx + i, cz + r), (cx - r, cz + i), (cx + r, cz + i)):
                w.set(x, base - h - 2, z, wall)


def scatter(rng, n, spacing, margin, avoid, jitter=None):
    """cells on a jittered grid, spacing apart, margin from the edges, outside the circles in
    avoid ((x, z, radius))"""
    jitter = spacing // 3 if jitter is None else jitter
    out = []
    for gx in range(margin, n - margin, spacing):
        for gz in range(margin, n - margin, spacing):
            x, z = gx + rng.randint(-jitter, jitter), gz + rng.randint(-jitter, jitter)
            if all((x - ax) ** 2 + (z - az) ** 2 > ar * ar for ax, az, ar in avoid):
                out.append((x, z))
    return out


def preset_tutorial():
    """24 x 24 chunks on grass: a village around a well in the middle, sand paths to four areas (a
    forest to the north (+z), a lake to the east (+x), a stepped hill to the south, a tower and
    ruins to the west), pillars in the corners and lone trees between. Spawn on the plaza, facing
    the well and the forest path beyond"""
    import random
    rng = random.Random(1)
    w = World(BIG, BIG, "Tutorial", ground=DARK_GREEN)
    n, c = BIG * CHUNK, BIG * CHUNK // 2         # 192 cells, centre 96

    # the plaza: a grey ring of tiles around a well with a roof
    for x in range(c - 5, c + 6):
        for z in range(c - 5, c + 6):
            if max(abs(x - c), abs(z - c)) in (4, 5):
                w.set(x, 0, z, GREY)
    w.box(c - 1, c + 1, -1, -1, c - 1, c + 1, GREY)
    w.cells.pop((c, -1, c), None)
    w.set(c, 0, c, BLUE)
    for dx, dz in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        w.box(c + dx, c + dx, -3, -2, c + dz, c + dz, BROWN)
    w.box(c - 1, c + 1, -4, -4, c - 1, c + 1, PURPLE)

    # four houses on the diagonals, doors towards the plaza, and two lamps
    house(w, c - 14, c - 14, 6, 5, door="+z")
    house(w, c + 9, c - 14, 5, 6, wall=SAND, roof=RED, door="-x")
    house(w, c - 14, c + 9, 5, 6, wall=SAND, roof=RED, door="+x")
    house(w, c + 9, c + 10, 6, 5, door="-z")
    for x, z in ((c - 8, c - 9), (c + 8, c + 9)):
        w.box(x, x, -2, -1, z, z, YELLOW)

    # the paths
    path(w, (c, c + 6), (c, c + 40))
    path(w, (c + 6, c), (c + 40, c))
    path(w, (c, c - 6), (c, c - 40))
    path(w, (c - 6, c), (c - 40, c))

    # north: the forest, either side of the path's end
    for x, z in scatter(rng, n, 7, 0, []):
        if 2 < abs(x - c) < 30 and c + 44 <= z <= c + 80:
            big_tree(w, x, z, rng.randint(3, 5))

    # east: a lake with a sand shore, a jetty and a boat
    lx, lz = c + 52, c
    for x in range(lx - 9, lx + 10):
        for z in range(lz - 9, lz + 10):
            d = (x - lx) ** 2 + (z - lz) ** 2
            if d <= 49:
                w.set(x, 0, z, BLUE)
            elif d <= 81:
                w.set(x, 0, z, SAND)
    w.box(lx - 9, lx - 3, -1, -1, lz, lz, BROWN)
    w.box(lx, lx + 4, -1, -1, lz - 3, lz - 3, BROWN)
    w.box(lx + 1, lx + 3, -1, -1, lz - 4, lz - 2, BROWN)
    w.cells.pop((lx + 2, -1, lz - 3), None)

    # south: a stepped hill with a flag on top
    hx, hz = c, c - 54
    hill(w, hx, hz, (6, 5, 4, 3, 2, 1, 0), (GREEN, GREEN, GREEN, GREY, GREY, WHITE))
    w.box(hx, hx, -11, -8, hz, hz, BROWN)
    w.box(hx + 1, hx + 2, -11, -10, hz, hz, RED)

    # west: a tower, and ruined walls around it
    tx, tz = c - 52, c
    tower(w, tx, tz, r=2, h=9)
    for i in range(-10, 11):
        if rng.random() < 0.55:
            w.box(tx + i, tx + i, -rng.randint(1, 3), -1, tz - 10, tz - 10, DARK_GREY)
        if rng.random() < 0.55:
            w.box(tx - 10, tx - 10, -rng.randint(1, 3), -1, tz + i, tz + i, DARK_GREY)

    # the corners: tall coloured pillars, seen from afar
    for (x, z), mat in zip(((16, 16), (n - 18, 16), (16, n - 18), (n - 18, n - 18)),
                           (RED, YELLOW, PINK, LAVENDER)):
        w.box(x, x + 1, -10, -1, z, z + 1, mat)

    # lone trees and rocks elsewhere, off the paths
    avoid = [(c, c, 20), (c, c + 62, 36), (lx, lz, 14), (hx, hz, 12), (tx, tz, 16)]
    for x, z in scatter(rng, n, 16, 6, avoid):
        if abs(x - c) <= 2 or abs(z - c) <= 2:
            continue
        if rng.random() < 0.8:
            big_tree(w, x, z, rng.randint(3, 5))
        else:
            w.box(x, x + 1, -1, -1, z, z + rng.randint(0, 1), GREY)

    w.spawn = (c, c - 8, 0)
    return w


def preset_islands():
    """24 x 24 chunks of sea (the ground plane) with small sandy islands one cell high, on a
    jittered grid 30 cells apart, each with a tree, a hut or a rock; a lighthouse island in the
    middle, where the player starts. The sea can be walked on"""
    import random
    rng = random.Random(2)
    w = World(BIG, BIG, "Islands", ground=DARK_BLUE)   # BLUE is the sky colour
    n, c = BIG * CHUNK, BIG * CHUNK // 2

    def island(cx, cz, r):
        for x in range(cx - r, cx + r + 1):
            for z in range(cz - r, cz + r + 1):
                d = (x - cx) ** 2 + (z - cz) ** 2
                if d <= r * r + r:
                    w.set(x, -1, z, SAND if d > (r - 1.5) ** 2 else GREEN)

    island(c, c, 6)
    tower(w, c, c + 2, r=1, h=8, wall=WHITE, top=RED, battlements=False, base=-1)
    w.set(c, -11, c + 2, YELLOW)
    w.box(c, c, -1, -1, c - 12, c - 7, BROWN)             # a jetty to the south
    for x, z in scatter(rng, n, 30, 12, [(c, c, 20)], jitter=7):
        r = rng.randint(3, 5)
        island(x, z, r)
        k = rng.random()
        if k < 0.5:
            big_tree(w, x, z, rng.randint(3, 4), base=-1)
        elif k < 0.75 and r >= 4:
            house(w, x - 2, z - 2, 4, 4, h=2, wall=ORANGE, roof=RED, base=-1,
                  door=rng.choice(("-z", "+z", "-x", "+x")))
        else:
            w.box(x, x + 1, -3, -2, z, z, GREY)
    w.spawn = (c, c - 4, 0)
    return w


def preset_plains():
    """24 x 24 chunks of open grass with far-apart landmarks: a stepped pyramid, a stone circle, a
    ruined wall, a white tower, a pond, a field of dunes, lone trees; a camp at the spawn"""
    import math
    import random
    rng = random.Random(3)
    w = World(BIG, BIG, "Plains", ground=DARK_GREEN)
    n, c = BIG * CHUNK, BIG * CHUNK // 2

    # a stepped pyramid to the north-east
    px, pz = c + 48, c + 48
    hill(w, px, pz, (5, 4, 3, 2, 1, 0), (SAND, SAND, SAND, SAND, SAND, YELLOW))

    # a stone circle with lintels to the north-west
    sx, sz = c - 48, c + 44
    stones = [(round(sx + 7 * math.cos(2 * math.pi * k / 12)),
               round(sz + 7 * math.sin(2 * math.pi * k / 12))) for k in range(12)]
    for k, (x, z) in enumerate(stones):
        w.box(x, x, -3, -1, z, z, GREY)
        if k % 2 == 0:                  # a lintel on each pair of stones
            w.set(x, -4, z, GREY)
            w.set(stones[k + 1][0], -4, stones[k + 1][1], GREY)
    w.box(sx, sx, -1, -1, sz - 1, sz + 1, DARK_GREY)

    # a ruined wall across the south
    for x in range(c - 30, c + 30):
        if rng.random() < 0.7:
            w.box(x, x, -rng.randint(1, 3), -1, c - 50, c - 50, DARK_GREY)

    # a white tower to the east, a pond to the west
    tower(w, c + 60, c - 10, r=2, h=10, wall=WHITE, top=RED)
    qx, qz = c - 56, c - 12
    for x in range(qx - 6, qx + 7):
        for z in range(qz - 6, qz + 7):
            if (x - qx) ** 2 + (z - qz) ** 2 <= 30:
                w.set(x, 0, z, BLUE)

    # low sand dunes in the far south-east
    for x, z in scatter(rng, n, 9, 0, []):
        if x > c + 40 and z < c - 40:
            w.box(x, x + 2, -1, -1, z, z + 1, SAND)
            if rng.random() < 0.4:
                w.set(x + 1, -2, z, SAND)

    # the camp at the spawn: a fire and two tents
    w.set(c, 0, c + 3, ORANGE)
    w.set(c, -1, c + 3, RED)
    for tx in (c - 4, c + 3):
        w.box(tx, tx + 1, -1, -1, c + 6, c + 8, WHITE)
        w.box(tx, tx + 1, -2, -2, c + 6, c + 8, LAVENDER)

    # lone trees and bushes
    avoid = [(c, c, 12), (px, pz, 10), (sx, sz, 12), (c + 60, c - 10, 8), (qx, qz, 10)]
    for x, z in scatter(rng, n, 18, 6, avoid):
        if x > c + 36 and z < c - 36:
            continue
        if rng.random() < 0.7:
            big_tree(w, x, z, rng.randint(3, 6))
        else:
            w.box(x, x + 1, -1, -1, z, z + 1, GREEN)
    w.spawn = (c, c, 0)
    return w


PRESETS = {"test": preset_test, "dense": preset_dense, "random": preset_random,
           "tutorial": preset_tutorial, "islands": preset_islands, "plains": preset_plains}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", nargs="?", default="bin/WORLD.8xv")
    ap.add_argument("--preset", default="test", choices=tuple(PRESETS))
    ap.add_argument("--name")
    ap.add_argument("--appvar", default="WORLD")
    ap.add_argument("--ram", action="store_true")
    ap.add_argument("--bin")
    a = ap.parse_args()

    w = PRESETS[a.preset]()
    if a.name:
        w.name = a.name
    data, chunks = w.pack()
    if w.size_x >= BIG:
        assert len(data) <= BIG_WORLD_MAX, f"{len(data)} bytes: too big for Save & Quit"
    with open(a.out, "wb") as f:
        f.write(to_8xv(data, a.appvar, not a.ram))
    if a.bin:
        with open(a.bin, "wb") as f:
            f.write(data)
    counts = [len(c) for c in chunks]
    print(f"wrote {a.out}: \"{w.name}\", {w.size_x} x {w.size_z} chunks, {len(w.cells)} voxels, "
          f"{sum(counts)} records with runs (at most {max(counts)} per chunk), {w.saved} faces merged, "
          f"{len(data)} bytes")


if __name__ == "__main__":
    main()
