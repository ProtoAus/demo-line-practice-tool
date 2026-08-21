# make_bsp_fixture.py  --  generate tests\fixture_bsp.h.
#
# Run by hand, never by tests\build.bat and never by CI:
#
#     py -3 tests\make_bsp_fixture.py
#
# It writes tests\fixture_bsp.h, which is committed. Re-run and commit the
# result if the fixture needs to grow a case.
#
# WHY A SYNTHETIC MAP AND NOT A REAL ONE
#
# The same reason tests\make_fixture.py is synthetic: a real .bsp is somebody
# else's work, and the smallest one in the library is 2 MB. This one is under
# four kilobytes and every number in it was chosen so the right answer can be
# worked out on paper.
#
# WHY THE SAME WORLD IS EMITTED TWICE
#
# Once as BSP v20 with no compression, and once as v25 with every lump
# LZMA-compressed. That pair is the whole point of the fixture. v20 and v25
# disagree about the size of four of the seven structs -- NODES 32 vs 48,
# LEAFS 32 vs 56, LEAFBRUSHES 2 vs 4, BRUSHSIDES 8 vs 16 -- and about the
# offsets of the fields inside them, so a reader that got v25 wrong would still
# parse, still produce polygons, and produce DIFFERENT ONES. Encoding one world
# both ways turns that into an equality the harness can assert.
#
# It also covers the two file shapes in the real library, which are not a
# spectrum: measured over 1,304 maps, 723 have all seven collision lumps
# LZMA-compressed and 581 have none of them. No map mixes.
#
# WHICH FIELDS ARE MEASURED AND WHICH ARE FILLER
#
# Every field wr_bsp.cpp actually reads was confirmed against the whole
# library -- 117,031,588 index references checked with these offsets, none out
# of range:
#
#     NODES        children[2]                    int32 at 4 and 8, all versions
#     LEAFS        firstleafbrush/numleafbrushes  u16 at 24/26 (stock)
#                                                 i32 at 44/48 (v25)
#     LEAFBRUSHES  the index itself               u16 (stock) / u32 (v25)
#     BRUSHES      firstside/numsides/contents    int32 at 0/4/8, all versions
#     BRUSHSIDES   planenum                       u16 at 0 (stock) / i32 (v25)
#     PLANES       normal/dist                    float[3]+float at 0, all
#     MODELS       headnode                       int32 at 36, all versions
#
# The rest of each struct is filled in below with values consistent with the
# layouts read off a real v25 map (bounding boxes went from short to float in
# Strata's, because node 0 of bhop_canals spans -61448..57352 and a short
# cannot hold that). Those bytes are never read by wr_bsp.cpp, and this comment
# is here so nobody later mistakes the filler for something that was verified.
#
# THE WORLD
#
#     brush 0   floor slab, mins (-512,-512,-64) maxs (512,512,0)
#               its top face is n.z = 1.0, so it is NOT in the surf band
#     brush 1   a triangular prism ramp: x in [0,225], y in [-256,256],
#               sloping from z = 300 at x = 0 down to z = 0 at x = 225.
#               Its slanted face has normal (0.8, 0, 0.6) exactly, which is
#               53.130102 degrees, and area 375 * 512 = 192000 exactly.
#     brush 2   the same prism translated to x in [-600,-375], with the SAME
#               contents value as the other two -- CONTENTS_SOLID -- and owned
#               by model 1 rather than by the world.
#
# Brush 2 is the fixture's real subject. Trigger volumes in Source carry
# CONTENTS_SOLID verbatim and nothing in the file distinguishes them from a
# ramp except which model owns them; on surf_greensway 47% of solid brushes are
# teleport volumes and 76% of surf-band area is entity-owned. So the map here
# has exactly two identical slanted faces, one owned by the world and one not,
# and the total surf-band area is 192000 if the walk works and 384000 if it
# does not. A factor of two is hard to misread.
#
# Plane 16 is (0,0,0) with distance 0 and is referenced by nothing. Real maps
# have these -- 807 of them across four v25 maps in the library -- and a reader
# that normalises without checking divides by zero on one.

import lzma
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

VBSP = 0x50534256
LUMP_SLOTS = 64
HEADER_SIZE = 8 + LUMP_SLOTS * 16 + 4

L_PLANES, L_NODES, L_LEAFS = 1, 5, 10
L_MODELS, L_LEAFBRUSHES, L_BRUSHES, L_BRUSHSIDES = 14, 17, 18, 19
L_VERTEXES, L_FACES, L_EDGES, L_SURFEDGES = 3, 7, 12, 13
L_DISPINFO, L_DISPVERTS = 26, 33

CONTENTS_SOLID = 0x1

WORLD_MINS = (-1024.0, -1024.0, -512.0)
WORLD_MAXS = (1024.0, 1024.0, 1024.0)


# ---------------------------------------------------------------------------
# The world, as numbers
# ---------------------------------------------------------------------------
#
# Source's convention: a plane is (normal, dist) and a point is INSIDE the
# half-space when dot(v, n) - dist <= 0. A brush is the intersection of the
# insides of its sides.

PLANES = [
    # brush 0, the floor slab
    (0.0, 0.0, 1.0, 0.0),        # 0   top at z = 0
    (0.0, 0.0, -1.0, 64.0),      # 1   bottom at z = -64
    (1.0, 0.0, 0.0, 512.0),      # 2
    (-1.0, 0.0, 0.0, 512.0),     # 3
    (0.0, 1.0, 0.0, 512.0),      # 4
    (0.0, -1.0, 0.0, 512.0),     # 5
    # brush 1, the ramp
    (0.8, 0.0, 0.6, 180.0),      # 6   the slant. 0.8*0 + 0.6*300 = 180
    (-1.0, 0.0, 0.0, 0.0),       # 7   x >= 0
    (0.0, 1.0, 0.0, 256.0),      # 8
    (0.0, -1.0, 0.0, 256.0),     # 9
    (0.0, 0.0, -1.0, 0.0),       # 10  z >= 0
    # brush 2, the trigger: the same prism, 600 units back along -x
    (0.8, 0.0, 0.6, -300.0),     # 11  180 - 0.8*600
    (-1.0, 0.0, 0.0, 600.0),     # 12  x >= -600
    (0.0, 1.0, 0.0, 256.0),      # 13
    (0.0, -1.0, 0.0, 256.0),     # 14
    (0.0, 0.0, -1.0, 0.0),       # 15
    # referenced by nothing, and real
    (0.0, 0.0, 0.0, 0.0),        # 16
]

# planenum per brushside, in brush order
BRUSHSIDE_PLANE = [0, 1, 2, 3, 4, 5,
                   6, 7, 8, 9, 10,
                   11, 12, 13, 14, 15]

# (firstside, numsides, contents) -- note all three are CONTENTS_SOLID
BRUSHES = [(0, 6, CONTENTS_SOLID),
           (6, 5, CONTENTS_SOLID),
           (11, 5, CONTENTS_SOLID)]

LEAFBRUSHES = [0, 1, 2]

# (firstleafbrush, numleafbrushes)
LEAFS = [(0, 2),    # leaf 0: the world's two brushes
         (2, 0),    # leaf 1: empty
         (2, 1),    # leaf 2: the trigger, reachable only from model 1
         (3, 0)]    # leaf 3: empty

# (planenum, child0, child1). A negative child is -(leaf + 1).
NODES = [(0, -1, -2),
         (0, -3, -4)]

# (headnode,) -- model 0 is the world by definition
MODELS = [(0, WORLD_MINS, WORLD_MAXS),
          (1, (-600.0, -256.0, 0.0), (-375.0, 256.0, 300.0))]


# ---------------------------------------------------------------------------
# The displacement, for the second pair of fixtures
# ---------------------------------------------------------------------------
#
# WHY A SECOND PAIR RATHER THAN ADDING THIS TO THE FIRST
#
# The v20/v25 pair above has exact expected values baked into the harness --
# 11 polygons, 192000 square units in band, bounds ending at z = 300 -- and
# every one of them was measured. Growing that world would move all of them at
# once, and the rule this repository holds itself to is that a bound only moves
# where there is a measurement of THAT bound. So the displacement gets its own
# pair and the first pair stays byte for byte what it was.
#
# WHAT IT IS FOR
#
# dface_t is the struct that moved between stock Source and Strata, and it moved
# in a way no lump length can see:
#
#                   stock 19/20/21          v25
#     firstedge     i32 @4                  i32 @8
#     numedges      i16 @8                  i32 @12
#     total         56                      72
#
# 72 was measurable from the corpus and always had been -- the gcd of the v25
# FACES lengths. Where firstedge went was not, and reading it at 4 on a v25 map
# gives a number that is in range, indexes a real surfedge, and builds a
# displacement somewhere else entirely. That is the failure this pair exists to
# make impossible to ship: the same displacement written in both layouts, which
# is only the same displacement if both are read correctly.
#
# THE SURFACE, CHOSEN SO THE ANSWER IS EXACT
#
# A flat quad in the plane 0.8x + 0.6z = 300 -- the same 53.130102 degrees as
# the prism's slant, so it is squarely in the surf band:
#
#     c0 (0, -256, 500)      c1 (150, -256, 300)
#     c3 (0,  256, 500)      c2 (150,  256, 300)
#
# 250 units along the slope by 512 across = 128000 square units, exactly, and it
# sits above the prism rather than through it.
#
# Every one of the 25 displacement vertices pushes along the plane's own normal
# by the same 100 units, so the built surface is that quad TRANSLATED by
# (80, 0, 60): still flat, still 53.130102 degrees, still exactly 128000 square
# units, and 4x4x2 = 32 triangles. A uniform push is deliberate -- it exercises
# the dispvert arithmetic with non-zero values while keeping every expected
# number exact. Which grid point gets which dispvert is not what this pair is
# testing, and DISPVERTS did not change between the two layouts anyway.
#
# The winding walks c0 -> c1 -> c2 -> c3 through SURFEDGES, and the LAST of the
# four surfedges is NEGATIVE. A surfedge's sign is which end of the edge to
# take, and a reader that dropped it would hand back a quad with two corners
# swapped -- so the fixture spends one of its four edges proving the sign is
# read, in both layouts.

# TWO displacements, and the second one is the whole reason there are two.
#
# wr_bsp.cpp takes a displacement triangle's normal from the cross product of its
# own grid traversal -- row i runs c0 -> c1, column j runs c0 -> c3 -- so the
# CYCLIC ORDER of a quad's four corners decides whether the built surface reads
# as a ramp or as a ceiling. Rotating the list does not change that (which is why
# WrBspDispBaseCorner is allowed to rotate); reversing it does.
#
# A real map settles which order it means with dface_t.side: set, and the face
# sits on the BACK of its plane, so its winding runs the other way. Measured over
# the 543,488 displacement triangles in this library, that flag predicts the
# handedness exactly -- 0.0% of side=0 faces disagree with their own plane, and
# 100.0% of side=1 faces do.
#
# So: quad A is wound outward with side clear, quad B is the same surface 700
# units along -x, wound INWARD with side set. A reader that honours the flag
# builds both pointing out, at the same 53.130102 degrees. A reader that ignores
# it -- which this one did, for as long as it has read displacements at all --
# builds B inside out and then discards all 32 of its triangles as ceilings.

_A = [(0.0, -256.0, 500.0),             # c0, and startPosition
      (0.0, 256.0, 500.0),              # c1, along the top edge
      (150.0, 256.0, 300.0),            # c2
      (150.0, -256.0, 300.0)]           # c3, down the slope from c0

# The same quad translated, then REVERSED -- so it is inside out until the side
# flag is read. Corner 0 is unchanged, so startPosition still names a corner and
# the two differ only in handedness.
_B = [(x - 700.0, y, z) for (x, y, z) in [_A[0], _A[3], _A[2], _A[1]]]

# (corners, dface_t.side)
DISP_QUADS = [(_A, 0), (_B, 1)]

DISP_POWER = 2                          # 5x5 vertices, 4x4x2 = 32 triangles each
DISP_DIR = (0.8, 0.0, 0.6)              # the quads' own normal
DISP_DIST = 100.0
DISP_SIDE = (1 << DISP_POWER) + 1
DISP_VERTS_EACH = DISP_SIDE * DISP_SIDE


def pack_vertexes():
    return b"".join(struct.pack("<3f", *v) for q, _ in DISP_QUADS for v in q)


# (v0, v1) per edge, four per quad. The LAST edge of each quad is stored
# c0 -> c3 and referenced backwards, so a surfedge's sign is exercised: it is
# which end of the edge to take, and a reader that dropped it hands back a quad
# with two corners swapped.
def disp_edges():
    out = []
    for d, _ in enumerate(DISP_QUADS):
        b = d * 4
        out += [(b + 0, b + 1), (b + 1, b + 2), (b + 2, b + 3), (b + 0, b + 3)]
    return out


def disp_surfedges():
    out = []
    for d, _ in enumerate(DISP_QUADS):
        b = d * 4
        out += [b + 0, b + 1, b + 2, -(b + 3)]
    return out


def pack_edges(v25):
    fmt = "<2I" if v25 else "<2H"
    return b"".join(struct.pack(fmt, a, b) for a, b in disp_edges())


def pack_surfedges():
    return b"".join(struct.pack("<i", s) for s in disp_surfedges())


def pack_faces(v25):
    # One face per displacement. planenum 6 is the prism's slant, which has the
    # same normal -- wr_bsp.cpp does not read a face's plane, only its `side`
    # flag, and a planenum that is at least in range is better filler than zero.
    out = b""
    for d, (_, sideflag) in enumerate(DISP_QUADS):
        firstedge, numedges, dispinfo = d * 4, 4, d
        if v25:
            out += struct.pack("<I", 6)                     # planenum    @0
            out += struct.pack("<2B", sideflag, 1)          # side/onNode @4
            out += b"\0\0"                                  # padding     @6
            out += struct.pack("<i", firstedge)             #             @8
            out += struct.pack("<i", numedges)              #             @12
            out += struct.pack("<i", 0)                     # texinfo     @16
            out += struct.pack("<i", dispinfo)              #             @20
            out += struct.pack("<i", -1)                    # fog volume  @24
            out += struct.pack("<4B", 0, 255, 255, 255)     # styles      @28
            out += struct.pack("<i", -1)                    # lightofs    @32
            out += struct.pack("<f", 128000.0)              # area        @36
            out += struct.pack("<2i", 0, 0)                 # lm mins     @40
            out += struct.pack("<2i", 0, 0)                 # lm size     @48
            out += struct.pack("<i", -1)                    # origFace    @56
            out += struct.pack("<I", 0)                     # shadows/prims
            out += struct.pack("<I", 0)                     # firstPrimID @64
            out += struct.pack("<I", 0)                     # smoothing   @68
        else:
            out += struct.pack("<H", 6)                     # planenum    @0
            out += struct.pack("<2B", sideflag, 1)          # side/onNode @2
            out += struct.pack("<i", firstedge)             #             @4
            out += struct.pack("<h", numedges)              #             @8
            out += struct.pack("<h", 0)                     # texinfo     @10
            out += struct.pack("<h", dispinfo)              #             @12
            out += struct.pack("<h", -1)                    # fog volume  @14
            out += struct.pack("<4B", 0, 255, 255, 255)     # styles      @16
            out += struct.pack("<i", -1)                    # lightofs    @20
            out += struct.pack("<f", 128000.0)              # area        @24
            out += struct.pack("<2i", 0, 0)                 # lm mins     @28
            out += struct.pack("<2i", 0, 0)                 # lm size     @36
            out += struct.pack("<i", -1)                    # origFace    @44
            out += struct.pack("<2H", 0, 0)                 # prims       @48
            out += struct.pack("<I", 0)                     # smoothing   @52
    return out


def pack_dispinfo(v25):
    # startPosition, m_iDispVertStart, m_iDispTriStart, power, minTess,
    # smoothingAngle, contents, m_iMapFace -- and those first 40 bytes are at
    # the SAME offsets in both layouts. Only m_iMapFace's width changed, from
    # u16 to u32 in place. Everything past it grew, which is the whole of the
    # difference between 176 and 232.
    out = b""
    for d, (quad, _) in enumerate(DISP_QUADS):
        rec = struct.pack("<3f", *quad[0])                  # startPosition @0
        rec += struct.pack("<i", d * DISP_VERTS_EACH)       # dispVertStart @12
        rec += struct.pack("<i", 0)                         # dispTriStart  @16
        rec += struct.pack("<i", DISP_POWER)                # power         @20
        rec += struct.pack("<i", 0)                         # minTess       @24
        rec += struct.pack("<f", 0.0)                       # smoothingAngle @28
        rec += struct.pack("<i", CONTENTS_SOLID)            # contents      @32
        if v25:
            rec += struct.pack("<I", d)                     # m_iMapFace    @36
            rec += struct.pack("<2i", 0, 0)                 # lightmap      @40
            # DispNeighbor_t[4]: two DispSubNeighbor_t each, a u32 index plus
            # three bytes and one of padding -- 8 a piece, 16 a neighbour.
            rec += (struct.pack("<I", 0xFFFFFFFF) + b"\0\0\0\0") * 8
            # DispCornerNeighbors_t[4]: four u32 and a count, padded to 20.
            rec += (struct.pack("<4I", 0xFFFFFFFF, 0xFFFFFFFF,
                                0xFFFFFFFF, 0xFFFFFFFF) + b"\0\0\0\0") * 4
        else:
            rec += struct.pack("<H", d)                     # m_iMapFace    @36
            rec += b"\0\0"                                  # padding       @38
            rec += struct.pack("<2i", 0, 0)                 # lightmap      @40
            # u16 index plus three bytes, padded to 6; 12 a neighbour.
            rec += (struct.pack("<H", 0xFFFF) + b"\0\0\0\0") * 8
            # four u16 and a count, padded to 10.
            rec += (struct.pack("<4H", 0xFFFF, 0xFFFF,
                                0xFFFF, 0xFFFF) + b"\0\0") * 4
        rec += struct.pack("<10I", *([0xFFFFFFFF] * 10))    # m_AllowedVerts
        assert len(rec) == (232 if v25 else 176), len(rec)
        out += rec
    return out


def pack_dispverts():
    # CDispVert: Vector vec, float dist, float alpha. Unchanged on every
    # version, which is why there is no v25 branch here.
    one = struct.pack("<3f", *DISP_DIR) + struct.pack("<2f", DISP_DIST, 0.0)
    return one * (DISP_VERTS_EACH * len(DISP_QUADS))


# ---------------------------------------------------------------------------
# Packing each lump, per version
# ---------------------------------------------------------------------------

def pack_planes():
    out = b""
    for n0, n1, n2, d in PLANES:
        # dplane_t: Vector normal, float dist, int type. The type is a hint
        # vbsp uses to pick an axial fast path and nothing here reads it.
        axis = 3
        for k, v in enumerate((n0, n1, n2)):
            if v == 1.0 or v == -1.0:
                axis = k
        out += struct.pack("<4fi", n0, n1, n2, d, axis)
    return out


def pack_nodes(v25):
    out = b""
    for planenum, c0, c1 in NODES:
        if v25:
            # 48: planenum, children, mins/maxs as FLOATS, firstface,
            # numfaces, area, padding. Strata widened the box because a short
            # cannot hold Strata's world extents.
            out += struct.pack("<3i", planenum, c0, c1)
            out += struct.pack("<3f", *WORLD_MINS)
            out += struct.pack("<3f", *WORLD_MAXS)
            out += struct.pack("<2i", 0, 0)
            out += struct.pack("<2h", 0, 0)
        else:
            out += struct.pack("<3i", planenum, c0, c1)
            out += struct.pack("<3h", -1024, -1024, -512)
            out += struct.pack("<3h", 1024, 1024, 1024)
            out += struct.pack("<2H", 0, 0)
            out += struct.pack("<2h", 0, 0)
    return out


def pack_leafs(v25, lumpver):
    out = b""
    for firstbrush, numbrush in LEAFS:
        if v25:
            # 56: contents, cluster, area/flags, mins/maxs float[3],
            # firstleafface, numleaffaces, firstleafbrush, numleafbrushes,
            # leafWaterDataID, padding.
            out += struct.pack("<3i", CONTENTS_SOLID, -1, 0)
            out += struct.pack("<3f", *WORLD_MINS)
            out += struct.pack("<3f", *WORLD_MAXS)
            out += struct.pack("<2i", 0, 0)
            out += struct.pack("<2i", firstbrush, numbrush)
            out += struct.pack("<2h", -1, -1)
        else:
            # 32 (lump version 1) or 56 (lump version 0, with the ambient
            # light cube inline). The fields this reads are at the same
            # offsets in both, which is why one branch does both.
            out += struct.pack("<i", CONTENTS_SOLID)
            out += struct.pack("<2h", -1, 0)
            out += struct.pack("<3h", -1024, -1024, -512)
            out += struct.pack("<3h", 1024, 1024, 1024)
            out += struct.pack("<2H", 0, 0)
            out += struct.pack("<2H", firstbrush, numbrush)
            out += struct.pack("<h", -1)
            if lumpver == 0:
                out += b"\0" * 24        # CompressedLightCube
            out += struct.pack("<h", 0)
    return out


def pack_models():
    out = b""
    for headnode, mins, maxs in MODELS:
        out += struct.pack("<3f", *mins)
        out += struct.pack("<3f", *maxs)
        out += struct.pack("<3f", 0.0, 0.0, 0.0)
        out += struct.pack("<3i", headnode, 0, 0)
    return out


def pack_leafbrushes(v25):
    fmt = "<I" if v25 else "<H"
    return b"".join(struct.pack(fmt, b) for b in LEAFBRUSHES)


def pack_brushes():
    return b"".join(struct.pack("<3i", *b) for b in BRUSHES)


def pack_brushsides(v25):
    out = b""
    for pn in BRUSHSIDE_PLANE:
        if v25:
            # planenum, texinfo, dispinfo, then bevel and thin as two bools
            # and two bytes of padding.
            out += struct.pack("<3i", pn, 0, -1)
            out += struct.pack("<4B", 0, 1, 0, 0)
        else:
            out += struct.pack("<Hhhh", pn, 0, -1, 0)
    return out


# ---------------------------------------------------------------------------
# Compression
# ---------------------------------------------------------------------------

def valve_lzma(data, lc=3, lp=0, pb=2, dict_size=1 << 16):
    """The seventeen-byte Valve container and a raw LZMA1 stream, as a .bsp
    carries them -- byte for byte the same container a .mtv body has, which is
    why wr_bsp.cpp calls WrMtvLzmaDecode rather than owning a second decoder.

    The directory's fourCC field carries the decompressed size too, and
    wr_bsp.cpp requires the two to agree. They agreed on all 5,061 compressed
    lumps in the library, so this writes them from the same variable."""
    filt = [{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": lp, "pb": pb,
             "dict_size": dict_size}]
    c = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=filt)
    comp = c.compress(data) + c.flush()

    props = bytes([(pb * 5 + lp) * 9 + lc]) + dict_size.to_bytes(4, "little")

    d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt)
    assert d.decompress(comp, len(data)) == data, "fixture does not round-trip"

    body = b"LZMA" + struct.pack("<II", len(data), len(comp)) + props + comp
    return body, len(data)


# ---------------------------------------------------------------------------
# The file
# ---------------------------------------------------------------------------

def build(version, lumpvers, compress, disp=False):
    v25 = version == 25
    raw = {
        L_PLANES:      pack_planes(),
        L_NODES:       pack_nodes(v25),
        L_LEAFS:       pack_leafs(v25, lumpvers[L_LEAFS]),
        L_MODELS:      pack_models(),
        L_LEAFBRUSHES: pack_leafbrushes(v25),
        L_BRUSHES:     pack_brushes(),
        L_BRUSHSIDES:  pack_brushsides(v25),
    }

    # Check every lump tiles at the stride wr_bsp.cpp will pick, here rather
    # than in the harness, so a fixture that could not be read never lands.
    expect = {
        L_PLANES: 20,
        L_NODES: 48 if v25 else 32,
        L_LEAFS: {0: 56, 1: 32, 2: 56}[lumpvers[L_LEAFS]],
        L_MODELS: 48,
        L_LEAFBRUSHES: 4 if v25 else 2,
        L_BRUSHES: 12,
        L_BRUSHSIDES: 16 if v25 else 8,
    }
    counts = {L_PLANES: len(PLANES), L_NODES: len(NODES), L_LEAFS: len(LEAFS),
              L_MODELS: len(MODELS), L_LEAFBRUSHES: len(LEAFBRUSHES),
              L_BRUSHES: len(BRUSHES), L_BRUSHSIDES: len(BRUSHSIDE_PLANE)}

    if disp:
        nd = len(DISP_QUADS)
        raw.update({
            L_VERTEXES:  pack_vertexes(),
            L_EDGES:     pack_edges(v25),
            L_SURFEDGES: pack_surfedges(),
            L_FACES:     pack_faces(v25),
            L_DISPINFO:  pack_dispinfo(v25),
            L_DISPVERTS: pack_dispverts(),
        })
        expect.update({
            L_VERTEXES: 12,
            L_EDGES: 8 if v25 else 4,
            L_SURFEDGES: 4,
            L_FACES: 72 if v25 else 56,
            L_DISPINFO: 232 if v25 else 176,
            L_DISPVERTS: 20,
        })
        counts.update({
            L_VERTEXES: 4 * nd,
            L_EDGES: 4 * nd,
            L_SURFEDGES: 4 * nd,
            L_FACES: nd,
            L_DISPINFO: nd,
            L_DISPVERTS: DISP_VERTS_EACH * nd,
        })
    for li, blob in raw.items():
        assert len(blob) == expect[li] * counts[li], (
            "lump %d is %d bytes, expected %d * %d"
            % (li, len(blob), expect[li], counts[li]))

    directory = [(0, 0, 0, 0)] * LUMP_SLOTS
    body = b""
    for li in sorted(raw):
        blob = raw[li]
        if compress:
            packed, actual = valve_lzma(blob)
        else:
            packed, actual = blob, 0
        # Four-byte aligned, as vbsp writes them.
        while len(body) % 4:
            body += b"\0"
        directory[li] = (HEADER_SIZE + len(body), len(packed),
                         lumpvers.get(li, 0), actual)
        body += packed

    out = struct.pack("<ii", VBSP, version)
    for ofs, ln, lv, fourcc in directory:
        out += struct.pack("<iiiI", ofs, ln, lv, fourcc)
    out += struct.pack("<i", 1)             # mapRevision
    out += body
    return out


# ---------------------------------------------------------------------------
# Writing the header
# ---------------------------------------------------------------------------

def carray(name, blob):
    lines = ["static const unsigned char %s[%d] = {" % (name, len(blob))]
    for i in range(0, len(blob), 12):
        lines.append("    %s," % ", ".join("0x%02x" % b for b in blob[i:i + 12]))
    lines.append("};")
    return "\n".join(lines)


def main():
    # Stock: LEAFS lump version 1 (32 bytes), everything else 0.
    v20 = build(20, {L_LEAFS: 1}, compress=False)
    # Strata: NODES 1, LEAFS 2, LEAFBRUSHES 1, BRUSHSIDES 1.
    v25 = build(25, {L_NODES: 1, L_LEAFS: 2, L_LEAFBRUSHES: 1,
                     L_BRUSHSIDES: 1}, compress=True)

    # The same two, plus the displacement. The lump versions matter as much as
    # the bytes: the stride table is keyed on them, and it is FACES 1 on stock
    # against 2 on Strata, EDGES 0 against 1, DISPINFO 0 against 1. A fixture
    # that wrote v25 bytes under a stock lump version would be refused by the
    # table rather than read wrongly -- which is correct behaviour and would
    # also mean this pair tested nothing.
    dv20 = build(20, {L_LEAFS: 1, L_FACES: 1}, compress=False, disp=True)
    dv25 = build(25, {L_NODES: 1, L_LEAFS: 2, L_LEAFBRUSHES: 1,
                      L_BRUSHSIDES: 1, L_FACES: 2, L_EDGES: 1,
                      L_DISPINFO: 1}, compress=True, disp=True)

    why = """
One synthetic map, emitted twice: BSP v20 with nothing compressed, and BSP v25
with every collision lump LZMA-compressed. Both describe THE SAME WORLD, which
is the point -- the two versions disagree about the size of four of the seven
structs and about the field offsets inside them, so a reader that got v25 wrong
would still parse and would produce different geometry. See
tests\\make_bsp_fixture.py for the world and for which fields are measured
against the real library and which are filler.

The numbers the harness checks, all exact:

    the ramp's slanted face      normal (0.8, 0, 0.6), 53.130102 degrees
    its area                     375 * 512 = 192000
    the floor slab's top face    normal (0, 0, 1), NOT in the surf band
    surf-band area, world-owned  192000
    surf-band area if the walk fails and the trigger is kept   384000
"""

    parts = [
        "#define WR_FIXTURE_BSP_V20_VERSION 20",
        "#define WR_FIXTURE_BSP_V25_VERSION 25",
        "#define WR_FIXTURE_BSP_PLANES      %d" % len(PLANES),
        "#define WR_FIXTURE_BSP_BRUSHES     %d" % len(BRUSHES),
        "#define WR_FIXTURE_BSP_BRUSHSIDES  %d" % len(BRUSHSIDE_PLANE),
        "#define WR_FIXTURE_BSP_NODES       %d" % len(NODES),
        "#define WR_FIXTURE_BSP_LEAFS       %d" % len(LEAFS),
        "#define WR_FIXTURE_BSP_MODELS      %d" % len(MODELS),
        "",
        "// The world-owned surf-band area, in square units. Exactly the ramp's",
        "// slanted face: 375 along the slope by 512 across.",
        "#define WR_FIXTURE_BSP_SURF_AREA   192000.0f",
        "#define WR_FIXTURE_BSP_SURF_NZ     0.6f",
        "",
        "// The second pair: the same world with TWO displacements on it, in the",
        "// stock layout and in Strata's. dface_t is the struct that moved and it",
        "// moved where no lump length can see -- firstedge from 4 to 8, numedges",
        "// from a short at 8 to an int at 12 -- so the two blobs below are the",
        "// same surface only if both layouts are read correctly.",
        "//",
        "// The two displacements differ in dface_t.side and in the handedness of",
        "// their base quad, so they are the same surface only if the side flag is",
        "// honoured. Both must come out at +0.6, and both must be in the band.",
        "#define WR_FIXTURE_BSP_DISP_POWER  %d" % DISP_POWER,
        "#define WR_FIXTURE_BSP_DISP_COUNT  %d" % len(DISP_QUADS),
        "#define WR_FIXTURE_BSP_DISP_TRIS   %d"
        % ((1 << DISP_POWER) * (1 << DISP_POWER) * 2 * len(DISP_QUADS)),
        "",
        "// 250 units along the slope by 512 across, each pushed 100 along its own",
        "// normal, which translates it and leaves the area alone.",
        "#define WR_FIXTURE_BSP_DISP_AREA   %.1ff" % (128000.0 * len(DISP_QUADS)),
        "#define WR_FIXTURE_BSP_DISP_NZ     0.6f",
        carray("kBspV20", v20),
        carray("kBspV25", v25),
        carray("kBspDispV20", dv20),
        carray("kBspDispV25", dv25),
    ]

    path = os.path.join(HERE, "fixture_bsp.h")
    body = ["// fixture_bsp.h  --  generated by tests\\make_bsp_fixture.py. "
            "Do not hand-edit.", "//"]
    body += ["// " + l if l else "//" for l in why.strip().split("\n")]
    body += ["", "#ifndef WR_FIXTURE_BSP_H", "#define WR_FIXTURE_BSP_H", ""]
    for p in parts:
        body.append(p)
        body.append("")
    body += ["#endif // WR_FIXTURE_BSP_H", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(body))
    print("wrote %s  (v20 %d, v25 %d, disp v20 %d, disp v25 %d bytes)"
          % (path, len(v20), len(v25), len(dv20), len(dv25)))


if __name__ == "__main__":
    main()
