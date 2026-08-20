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

def build(version, lumpvers, compress):
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
        carray("kBspV20", v20),
        carray("kBspV25", v25),
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
    print("wrote %s  (v20 %d bytes, v25 %d bytes)" % (path, len(v20), len(v25)))


if __name__ == "__main__":
    main()
