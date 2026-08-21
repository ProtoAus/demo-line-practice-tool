// wr_bsp.cpp  --  see wr_bsp.h.

#include "wr_bsp.h"
#include "wr_mtv.h"     // WrMtvLzmaDecode: the container is Valve's, both times

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The file's own directory
// ---------------------------------------------------------------------------

#define BSP_IDENT       0x50534256u     // 'VBSP', little-endian
#define BSP_LUMP_SLOTS  64
#define BSP_HEADER_SIZE (8 + BSP_LUMP_SLOTS * 16 + 4)

// The Valve LZMA container, byte for byte the same one wr_mtv.h describes:
// "LZMA", the decompressed size, the compressed size, five property bytes, and
// then a raw LZMA1 stream with no end marker. Seventeen bytes of header.
#define BSP_LZMA_HDR 17

const int WrBspLumpIndex[WR_BSP_L_COUNT] =
{
    1,      // LUMP_PLANES
    5,      // LUMP_NODES
    10,     // LUMP_LEAFS
    14,     // LUMP_MODELS
    17,     // LUMP_LEAFBRUSHES
    18,     // LUMP_BRUSHES
    19,     // LUMP_BRUSHSIDES

    3,      // LUMP_VERTEXES     the displacement half starts here
    12,     // LUMP_EDGES
    13,     // LUMP_SURFEDGES
    7,      // LUMP_FACES
    26,     // LUMP_DISPINFO
    33,     // LUMP_DISPVERTS
    0       // LUMP_ENTITIES
};

const char *WrBspLumpName[WR_BSP_L_COUNT] =
{
    "PLANES", "NODES", "LEAFS", "MODELS", "LEAFBRUSHES", "BRUSHES", "BRUSHSIDES",
    "VERTEXES", "EDGES", "SURFEDGES", "FACES", "DISPINFO", "DISPVERTS",
    "ENTITIES"
};

// Which test refused a displacement, for the panel and the sweep. Implicitly
// sized and guarded, because a table one short of its enum is how the quick
// panel came to hand ImGui a null label -- see WR_TABLE_IS_FULL.
const char *WrBspDispDropName[] =
{
    "power", "face index", "vert start", "not a quad", "face vertex",
    "corner", "budget"
};
WR_TABLE_IS_FULL(WrBspDispDropName, WR_DISP_DROP__COUNT);

// FACES IS READ, AND THE HEADER'S OBJECTION TO IT STILL STANDS
//
// "FACES ARE NOT USED, AND THAT IS NOT AN OVERSIGHT" is about finding SURFACES:
// faces are a rendering structure, they do not exist for collision with no
// material on it, and surf_inner reads 0% brush-backed by faces against 100% by
// brushes. None of that changes, and nothing here uses a face to decide that
// something is a ramp.
//
// A displacement is the one case where the face is not an approximation of the
// collision -- it IS the collision's definition. ddispinfo_t names a face, and
// that face's four corners are the quad the displacement grid is built on.
// There is no brush to read instead; displacement collision appears nowhere in
// the BRUSHES lump, which is why a displacement-built map reads as almost empty.

// ---------------------------------------------------------------------------
// The stride table
// ---------------------------------------------------------------------------
//
// Every row here was measured on the 1,304 maps in momentum\maps, and measured
// TWICE: once by asking which strides divide every lump length in the corpus,
// and once by walking the resulting structures and checking that every index
// they contain points somewhere real. The second check is the one that counts.
// Divisibility alone cannot single out a stride -- 40 divides every PLANES
// lump just as 20 does -- so a reader that inferred a stride from a length
// would have a one-in-two chance of producing geometry that looks fine and is
// half the map. 236,252,579 index references were checked across the corpus
// with the strides below and not one was out of range.
//
// The key is (lump version, BSP version) in that order of importance, because
// the lump version is the field that actually tracks the struct. See the
// header. A pair not listed returns 0, which every caller treats as a refusal.

int WrBspStride(int lump, int bspVersion, int lumpVersion)
{
    switch (lump)
    {
    case WR_BSP_L_PLANES:
        // dplane_t: Vector normal, float dist, int type.
        if (lumpVersion == 0) return 20;
        break;

    case WR_BSP_L_NODES:
        // dnode_t. Strata widened it; the lump version says so.
        if (lumpVersion == 0 && bspVersion <= 21) return 32;
        if (lumpVersion == 1 && bspVersion == 25) return 48;
        break;

    case WR_BSP_L_LEAFS:
        // dleaf_t, and the one that moves BOTH ways. Version 0 carries the
        // ambient lighting cube inline (56); version 1 is after it moved out
        // into LUMP_LEAF_AMBIENT_LIGHTING (32); Strata's version 2 is 56
        // again. A reader keyed on "is this Strata" gets v25 wrong by 24 bytes
        // an entry, which is a walk that visits nothing real.
        if (lumpVersion == 0) return 56;
        if (lumpVersion == 1) return 32;
        if (lumpVersion == 2 && bspVersion == 25) return 56;
        break;

    case WR_BSP_L_MODELS:
        // dmodel_t: mins, maxs, origin, headnode, firstface, numfaces.
        // Unchanged on all four versions -- confirmed on every map in the
        // library by requiring headnode in range and mins <= maxs, and model
        // 0's headnode came out 0 on all 1,304.
        if (lumpVersion == 0) return 48;
        break;

    case WR_BSP_L_LEAFBRUSHES:
        // A bare index. unsigned short until Strata made it unsigned int,
        // which is also what lets a v25 map hold more than 65,536 brushes.
        if (lumpVersion == 0 && bspVersion <= 21) return 2;
        if (lumpVersion == 1 && bspVersion == 25) return 4;
        break;

    case WR_BSP_L_BRUSHES:
        // dbrush_t: firstside, numsides, contents. Never changed.
        if (lumpVersion == 0) return 12;
        break;

    case WR_BSP_L_BRUSHSIDES:
        // dbrushside_t: planenum, texinfo, dispinfo, bevel. Shorts until
        // Strata widened all four to ints.
        if (lumpVersion == 0 && bspVersion <= 21) return 8;
        if (lumpVersion == 1 && bspVersion == 25) return 16;
        break;

    // ---- the displacement half -------------------------------------------
    //
    // Measured the same way as the rows above: every lump length in the 1,304
    // maps installed here, grouped by (bsp version, lump version), against the
    // candidate stride. The v19/20/21 rows divide every single map. The v25
    // rows are the ones NOT written here, and the reason is in the case bodies.

    case WR_BSP_L_VERTEXES:
        // A bare Vector. Never changed -- 12 divides all 1,304, v25 included.
        if (lumpVersion == 0) return 12;
        break;

    case WR_BSP_L_EDGES:
        // dedge_t: two vertex indices. unsigned short until Strata widened them
        // to int, the same change LEAFBRUSHES and BRUSHSIDES got and for the
        // same reason -- a map with more than 65,536 vertices.
        //
        // Divisibility CANNOT tell these apart, since 8 is divisible by 4. The
        // lump version does: every v25 map carries version 1 here and every
        // older one version 0, and the greatest common divisor of the v25 lump
        // lengths is 8 rather than 4.
        if (lumpVersion == 0 && bspVersion <= 21) return 4;
        if (lumpVersion == 1 && bspVersion == 25) return 8;
        break;

    case WR_BSP_L_SURFEDGES:
        // A signed index into EDGES, where the sign is which way round to read
        // the edge. Never changed.
        if (lumpVersion == 0) return 4;
        break;

    case WR_BSP_L_FACES:
        // dface_t. Only three fields of it are wanted -- firstedge, numedges and
        // the displacement index -- but the stride has to be exact anyway.
        //
        // 72 FOR v25, AND THE 72 WAS NEVER THE PART THAT WAS MISSING. The gcd of
        // the v25 FACES lengths measured 72 here long before this row existed,
        // and the comment that used to sit in its place said so and refused
        // anyway -- because a stride is not an offset. Knowing the struct is
        // sixteen bytes longer says nothing about where firstedge went, and
        // guessing that produces plausible windings in plausible places, which
        // is the exact failure this table exists to prevent.
        //
        // What closed it is Strata publishing the struct:
        // wiki.stratasource.org/modding/formats/bsp-v25. Every field widened and
        // two moved:
        //
        //             stock 19/20/21              v25
        //     planenum      u16 @0            u32 @0
        //     side/onNode   u8 @2, u8 @3      u8 @4, u8 @5  (+2 pad)
        //     firstedge     i32 @4            i32 @8
        //     numedges      i16 @8            i32 @12
        //     texinfo       i16 @10           i32 @16
        //     dispinfo      i16 @12           i32 @20
        //                   ... = 56          ... = 72
        //
        // VERIFIED AGAINST THE FILES, not taken on the wiki's word, and by a
        // probe sharing no code with this reader. Over all 77 v25 maps and the
        // 4,966 displacements on the 27 that have any: power in [2,4] 4966/4966;
        // m_iMapFace in range 4966/4966; dface_t.dispinfo at the new offset 20
        // pointing back at the displacement that named the face 4966/4966;
        // numedges at the new offset 12 reading exactly 4 on 4966/4966. Read at
        // the OLD offset instead, numedges says 4 on three of them.
        //
        // And the one that is not an index check. startPosition is a POINT, and
        // it landed on a corner of the quad the face walks out to -- through
        // SURFEDGES, EDGES and VERTEXES, three lumps this row does not touch --
        // on 4966 of 4966, worst distance 0.111 units. Two lumps cannot agree on
        // a position in space through a field offset that is wrong.
        if (lumpVersion == 1 && bspVersion <= 21) return 56;
        if (lumpVersion == 2 && bspVersion == 25) return 72;
        break;

    case WR_BSP_L_DISPINFO:
        // ddispinfo_t. Four fields are wanted and all four are in the first
        // forty bytes: startPosition at 0, m_iDispVertStart at 12, power at 20,
        // m_iMapFace at 36.
        //
        // 232 on v25, and the interesting part is that ALL FOUR OF THOSE OFFSETS
        // ARE UNCHANGED. The struct grew from 176 to 232 entirely in its tail:
        // m_iMapFace widened from u16 to u32 in place, and the neighbour arrays
        // behind it widened their own indices the same way, taking
        // DispSubNeighbor_t from 6 bytes to 8 and DispCornerNeighbors_t from 10
        // to 20. 48 + 4*16 + 4*20 + 40 = 232, which is the gcd this reader had
        // already measured -- the arithmetic and the corpus arriving at the same
        // number from opposite directions.
        //
        // So the prefix did stay put after all. That was not knowable from a
        // length, which is why the row this replaces was right to refuse: "the
        // tail grew" and "the prefix moved" are the same gcd.
        //
        // m_iMapFace still has to be read as 32 bits rather than 16. Little-endian
        // hides it below 65,536 faces and v25 exists precisely because maps went
        // past limits like that one.
        //
        // Four v25 maps carry lump version 0 here rather than 1 -- df_cavernish,
        // df_elco-gbparadise, df_gpl-strangeland, df_precision2 -- and all four
        // have a ZERO-LENGTH dispinfo lump, which is an absent lump's default
        // version showing through rather than a third layout. They are refused by
        // this row and lose nothing, because there is nothing in them to read.
        if (lumpVersion == 0 && bspVersion <= 21) return 176;
        if (lumpVersion == 1 && bspVersion == 25) return 232;
        break;

    case WR_BSP_L_DISPVERTS:
        // CDispVert: a direction, a distance along it, and an alpha. Unchanged
        // on all four versions -- 20 divides every map that has the lump,
        // v25 included.
        if (lumpVersion == 0) return 20;
        break;

    case WR_BSP_L_ENTITIES:
        // Not a struct array at all: the entity lump is a block of text, one
        // { "key" "value" } block per entity. Stride 1 means "count is bytes",
        // which is what the reader needs to know to size it.
        //
        // Still gated on the version, and the harness is right to insist: an
        // unrecognised lump version has to be refused HERE even when the format
        // is self-describing, because a version bump is the file saying it is
        // not what this reader thinks it is.
        if (lumpVersion == 0) return 1;
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Little bits
// ---------------------------------------------------------------------------

static void Fail(char *err, int errCap, const char *fmt, ...)
{
    if (!err || errCap <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(err, (size_t)errCap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static unsigned int Rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static bool ReadAt(HANDLE h, long long ofs, void *dst, unsigned int len)
{
    LARGE_INTEGER li;
    li.QuadPart = ofs;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return false;

    unsigned char *out = (unsigned char *)dst;
    unsigned int done = 0;
    while (done < len)
    {
        DWORD chunk = (DWORD)((len - done) > 0x400000u ? 0x400000u : (len - done));
        DWORD got = 0;
        if (!ReadFile(h, out + done, chunk, &got, NULL) || got == 0)
            return false;
        done += got;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Reading the seven
// ---------------------------------------------------------------------------
//
// TWO PASSES, AND THE SPLIT IS THE POINT. The first pass reads nothing but the
// directory and, for a compressed lump, its seventeen-byte container header --
// enough to know every final size before a single byte is allocated. Only when
// all seven have passed every gate does the second pass allocate anything.
//
// That is what makes "never partially succeeds" true rather than aspirational,
// and it is also what puts the budget check somewhere it can do its job: a
// total computed after six allocations is a total computed too late.

struct LumpPlan
{
    long long ofs;          // where the bytes are in the file
    unsigned int diskLen;   // what the directory says is there
    unsigned int outLen;    // what it will be once decompressed
    unsigned int lzmaLen;   // 0 when it is not compressed
    unsigned char props[5];
    int stride;
};

bool WrBspReadRaw(const char *path, WrBspRaw *out, char *err, int errCap)
{
    if (err && errCap > 0)
        err[0] = '\0';
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    if (!path || !*path)
    {
        Fail(err, errCap, "no map file named");
        return false;
    }

    // Shared every way, because the engine has this file open while the map is
    // running -- which is precisely when we want to read it.
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Fail(err, errCap, "could not open the map file");
        return false;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < (long long)BSP_HEADER_SIZE)
    {
        CloseHandle(h);
        Fail(err, errCap, "the BSP header is truncated");
        return false;
    }
    const long long fileSize = size.QuadPart;

    unsigned char hdr[BSP_HEADER_SIZE];
    if (!ReadAt(h, 0, hdr, (unsigned int)sizeof(hdr)))
    {
        CloseHandle(h);
        Fail(err, errCap, "the BSP header is truncated");
        return false;
    }

    if (Rd32(hdr) != BSP_IDENT)
    {
        CloseHandle(h);
        Fail(err, errCap, "not a VBSP file");
        return false;
    }

    const int version = (int)Rd32(hdr + 4);
    if (version != 19 && version != 20 && version != 21 && version != 25)
    {
        CloseHandle(h);
        Fail(err, errCap, "unsupported BSP version %d", version);
        return false;
    }

    // -----------------------------------------------------------------------
    // Pass one: every gate, no allocation
    // -----------------------------------------------------------------------

    LumpPlan plan[WR_BSP_L_COUNT];
    memset(plan, 0, sizeof(plan));
    unsigned int total = 0;
    bool anyCompressed = false;

    for (int i = 0; i < WR_BSP_L_COUNT; i++)
    {
        const unsigned char *e = hdr + 8 + WrBspLumpIndex[i] * 16;
        const int ofs        = (int)Rd32(e);
        const int diskLen    = (int)Rd32(e + 4);
        const int lumpVer    = (int)Rd32(e + 8);
        const unsigned int fourCC = Rd32(e + 12);
        const char *nm = WrBspLumpName[i];

        // An optional lump that is absent, or that this reader has no stride
        // for, is skipped rather than refused -- see the enum. Its data pointer
        // stays null and every consumer already has to cope with that, because
        // most of the library genuinely has no displacements.
        const bool optional = (i >= WR_BSP_L_REQUIRED);
        if (optional &&
            (diskLen == 0 || WrBspStride(i, version, lumpVer) == 0))
            continue;

        if (ofs < 0 || diskLen < 0)
        {
            CloseHandle(h);
            Fail(err, errCap, "%s lump has a negative offset or length", nm);
            return false;
        }
        if (diskLen == 0)
        {
            CloseHandle(h);
            Fail(err, errCap, "%s lump is empty", nm);
            return false;
        }
        if ((long long)ofs + (long long)diskLen > fileSize)
        {
            CloseHandle(h);
            Fail(err, errCap, "%s lump runs past the end of the file", nm);
            return false;
        }

        plan[i].ofs = ofs;
        plan[i].diskLen = (unsigned int)diskLen;

        if (fourCC != 0)
        {
            // Compressed. The directory's fourCC field carries the
            // decompressed size, and so does the container seventeen bytes
            // later; requiring them to AGREE is free and it is the only cheap
            // structural check this format offers. Across 5,061 compressed
            // lumps in the library they agreed every time, so a disagreement
            // means something is wrong rather than something is unusual.
            anyCompressed = true;

            if ((unsigned int)diskLen < BSP_LZMA_HDR)
            {
                CloseHandle(h);
                Fail(err, errCap, "%s lump is compressed but too short to be", nm);
                return false;
            }

            unsigned char lz[BSP_LZMA_HDR];
            if (!ReadAt(h, ofs, lz, BSP_LZMA_HDR))
            {
                CloseHandle(h);
                Fail(err, errCap, "%s lump could not be read", nm);
                return false;
            }
            if (memcmp(lz, "LZMA", 4) != 0)
            {
                CloseHandle(h);
                Fail(err, errCap,
                     "%s lump is compressed with something that is not LZMA", nm);
                return false;
            }

            const unsigned int actual  = Rd32(lz + 4);
            const unsigned int lzmaLen = Rd32(lz + 8);
            if (actual != fourCC)
            {
                CloseHandle(h);
                Fail(err, errCap,
                     "%s lump: the directory says %u bytes and the container says %u",
                     nm, fourCC, actual);
                return false;
            }
            if ((unsigned long long)lzmaLen + BSP_LZMA_HDR >
                (unsigned long long)diskLen)
            {
                CloseHandle(h);
                Fail(err, errCap, "%s lump's LZMA stream runs past the lump", nm);
                return false;
            }

            plan[i].outLen = actual;
            plan[i].lzmaLen = lzmaLen;
            memcpy(plan[i].props, lz + 12, 5);
        }
        else
        {
            plan[i].outLen = (unsigned int)diskLen;
        }

        if (plan[i].outLen == 0)
        {
            CloseHandle(h);
            Fail(err, errCap, "%s lump is empty", nm);
            return false;
        }
        if (plan[i].outLen > WR_BSP_MAX_LUMP)
        {
            CloseHandle(h);
            Fail(err, errCap, "%s lump is %u bytes, over the %u byte limit",
                 nm, plan[i].outLen, (unsigned int)WR_BSP_MAX_LUMP);
            return false;
        }

        // Unsigned, and each term is already known to be <= WR_BSP_MAX_LUMP,
        // so seven of them cannot wrap a 32-bit accumulator.
        total += plan[i].outLen;
        if (total > WR_BSP_MAX_LUMPS_TOTAL)
        {
            CloseHandle(h);
            Fail(err, errCap,
                 "the collision lumps total %u bytes, over the %u byte limit",
                 total, (unsigned int)WR_BSP_MAX_LUMPS_TOTAL);
            return false;
        }

        const int stride = WrBspStride(i, version, lumpVer);
        if (stride == 0)
        {
            CloseHandle(h);
            Fail(err, errCap,
                 "%s lump version %d is not one this reads on BSP %d",
                 nm, lumpVer, version);
            return false;
        }
        if (plan[i].outLen % (unsigned int)stride != 0)
        {
            CloseHandle(h);
            Fail(err, errCap, "%s lump is %u bytes, not a multiple of %d",
                 nm, plan[i].outLen, stride);
            return false;
        }
        plan[i].stride = stride;
    }

    // -----------------------------------------------------------------------
    // Pass two: read, decompress, and hand it over
    // -----------------------------------------------------------------------

    WrBspRaw r;
    memset(&r, 0, sizeof(r));
    r.version = version;
    r.revision = (int)Rd32(hdr + 8 + BSP_LUMP_SLOTS * 16);
    r.totalBytes = total;
    r.compressed = anyCompressed;

    // Whether this map has displacements AT ALL, which is a different question
    // from whether they were read. The lump is now parsed where the layout is
    // known, and skipped on v25 -- so this length stays the honest answer to
    // "is there displacement geometry here", and the panel keeps using it to say
    // what is missing. No gate on it: refusing a whole map over the SIZE of a
    // lump would turn a note about coverage into a refusal.
    {
        const unsigned char *d = hdr + 8 +
                                 WrBspLumpIndex[WR_BSP_L_DISPINFO] * 16;
        const int dispLen = (int)Rd32(d + 4);
        r.dispInfoBytes = dispLen > 0 ? (unsigned int)dispLen : 0u;
    }

    bool ok = true;
    for (int i = 0; i < WR_BSP_L_COUNT && ok; i++)
    {
        const char *nm = WrBspLumpName[i];

        // Skipped in pass one -- an optional lump this map does not have, or one
        // whose layout this reader does not know. Left null, which is what the
        // displacement builder tests for.
        if (plan[i].stride == 0)
            continue;

        r.bytes[i] = plan[i].outLen;
        r.stride[i] = plan[i].stride;
        r.count[i] = (int)(plan[i].outLen / (unsigned int)plan[i].stride);
        r.data[i] = (unsigned char *)malloc(plan[i].outLen);
        if (!r.data[i])
        {
            Fail(err, errCap, "out of memory");
            ok = false;
            break;
        }

        if (plan[i].lzmaLen == 0)
        {
            if (!ReadAt(h, plan[i].ofs, r.data[i], plan[i].outLen))
            {
                Fail(err, errCap, "%s lump could not be read", nm);
                ok = false;
            }
            continue;
        }

        unsigned char *src = (unsigned char *)malloc(plan[i].lzmaLen);
        if (!src)
        {
            Fail(err, errCap, "out of memory");
            ok = false;
            break;
        }
        if (!ReadAt(h, plan[i].ofs + BSP_LZMA_HDR, src, plan[i].lzmaLen))
        {
            free(src);
            Fail(err, errCap, "%s lump could not be read", nm);
            ok = false;
            break;
        }

        char sub[128];
        sub[0] = '\0';
        // The same decoder the demo container uses, and for the same reason it
        // is called with LZMA_FINISH_ANY there: Valve's container carries no
        // end-of-stream marker, so "the stream must be finished" is a
        // condition these bytes were never built to satisfy. It produces
        // exactly outLen or it fails.
        bool got = WrMtvLzmaDecode(src, plan[i].lzmaLen, r.data[i],
                                   plan[i].outLen, plan[i].props,
                                   sub, (int)sizeof(sub));
        free(src);
        if (!got)
        {
            Fail(err, errCap, "%s lump: %s", nm, sub[0] ? sub : "LZMA failed");
            ok = false;
        }
    }

    CloseHandle(h);

    if (!ok)
    {
        WrBspFreeRaw(&r);
        return false;
    }

    *out = r;
    return true;
}

void WrBspFreeRaw(WrBspRaw *r)
{
    if (!r)
        return;
    for (int i = 0; i < WR_BSP_L_COUNT; i++)
    {
        free(r->data[i]);
        r->data[i] = NULL;
    }
    memset(r, 0, sizeof(*r));
}

// ---------------------------------------------------------------------------
// The fields
// ---------------------------------------------------------------------------
//
// One place where "is this Strata" is asked, and it is asked about the FILE
// rather than about a stride -- because 56-byte leaves happen on v19 too, with
// completely different field offsets, and a reader that keyed the offsets off
// the size would read a v19 map's leaf brush range out of its ambient light
// cube.

static bool Strata(const WrBspRaw *r) { return r->version == 25; }

static const unsigned char *Rec(const WrBspRaw *r, int lump, int i)
{
    if (i < 0 || i >= r->count[lump])
        return NULL;
    return r->data[lump] + (size_t)i * (size_t)r->stride[lump];
}

static int Rd16(const unsigned char *p)
{
    return (int)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}

static int RdI32(const unsigned char *p)
{
    return (int)Rd32(p);
}

// Through memcpy rather than a cast, which is what the plane reader already
// does: a .bsp is read into a byte buffer with no alignment guarantee, and
// *(const float *)p on an odd address is undefined even where it happens to
// work.
static float RdF(const unsigned char *p)
{
    float f;
    memcpy(&f, p, sizeof(f));
    return f;
}

// A node's child. Positive is a node, negative is -(leaf + 1). Returns 0 for
// an index out of range, which the walk treats as a refusal -- there is no
// sentinel a real file could not also contain, so the caller checks the index
// before calling rather than the value after.
int WrBspNodeChild(const WrBspRaw *r, int node, int which)
{
    const unsigned char *p = Rec(r, WR_BSP_L_NODES, node);
    if (!p || which < 0 || which > 1)
        return 0;
    return RdI32(p + 4 + which * 4);
}

bool WrBspLeafBrushRange(const WrBspRaw *r, int leaf, int *first, int *num)
{
    const unsigned char *p = Rec(r, WR_BSP_L_LEAFS, leaf);
    if (!p)
        return false;
    if (Strata(r))
    {
        *first = RdI32(p + 44);
        *num = RdI32(p + 48);
    }
    else
    {
        // Offset 24, and not 22: firstleafface and numleaffaces sit at 20 and
        // 22. Reading the pair two bytes early passes a divisibility check,
        // passes a plausibility glance, and gives leaf brush ranges that are
        // face ranges -- which is how this was wrong the first time.
        *first = Rd16(p + 24);
        *num = Rd16(p + 26);
    }
    return *first >= 0 && *num >= 0;
}

int WrBspLeafBrush(const WrBspRaw *r, int i)
{
    const unsigned char *p = Rec(r, WR_BSP_L_LEAFBRUSHES, i);
    if (!p)
        return -1;
    return Strata(r) ? RdI32(p) : Rd16(p);
}

bool WrBspBrush(const WrBspRaw *r, int i, int *firstSide, int *numSides,
                int *contents)
{
    const unsigned char *p = Rec(r, WR_BSP_L_BRUSHES, i);
    if (!p)
        return false;
    if (firstSide) *firstSide = RdI32(p);
    if (numSides)  *numSides = RdI32(p + 4);
    if (contents)  *contents = RdI32(p + 8);
    return true;
}

int WrBspBrushSidePlane(const WrBspRaw *r, int side)
{
    const unsigned char *p = Rec(r, WR_BSP_L_BRUSHSIDES, side);
    if (!p)
        return -1;
    return Strata(r) ? RdI32(p) : Rd16(p);
}

bool WrBspPlane(const WrBspRaw *r, int i, float out[4])
{
    const unsigned char *p = Rec(r, WR_BSP_L_PLANES, i);
    if (!p)
        return false;
    memcpy(out, p, 16);         // Vector normal, then float dist
    return true;
}

int WrBspModelHeadNode(const WrBspRaw *r, int model)
{
    const unsigned char *p = Rec(r, WR_BSP_L_MODELS, model);
    if (!p)
        return -1;
    return RdI32(p + 36);       // after mins, maxs and origin
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------
//
// Iterative, with its own stack, because the depth is somebody else's number.
// A recursive walk of a 10,000-node tree is fine on a well-formed file and is
// a stack overflow on a crafted one, and this runs inside another process.
//
// Termination does not rest on the file being a tree. A `seen` bitmap makes
// every node visitable once, so a child pointer that loops back is simply not
// followed again -- there is no arrangement of bytes that makes this run
// twice as long as it has nodes.

bool WrBspWorldBrushes(const WrBspRaw *r, unsigned char *owned, int *ownedOut,
                       char *err, int errCap)
{
    if (err && errCap > 0)
        err[0] = '\0';
    if (ownedOut)
        *ownedOut = 0;
    if (!r || !owned || !r->data[WR_BSP_L_NODES])
    {
        Fail(err, errCap, "nothing to walk");
        return false;
    }

    const int numNodes  = r->count[WR_BSP_L_NODES];
    const int numLeafs  = r->count[WR_BSP_L_LEAFS];
    const int numLB     = r->count[WR_BSP_L_LEAFBRUSHES];
    const int numBrush  = r->count[WR_BSP_L_BRUSHES];
    const int numSides  = r->count[WR_BSP_L_BRUSHSIDES];

    memset(owned, 0, (size_t)numBrush);

    const int head = WrBspModelHeadNode(r, 0);
    if (head < 0 || head >= numNodes)
    {
        Fail(err, errCap, "model 0's head node is %d of %d", head, numNodes);
        return false;
    }

    unsigned char *seen = (unsigned char *)calloc((size_t)numNodes, 1);
    int *stack = (int *)malloc((size_t)numNodes * sizeof(int));
    if (!seen || !stack)
    {
        free(seen);
        free(stack);
        Fail(err, errCap, "out of memory");
        return false;
    }

    bool ok = true;
    int top = 0;
    stack[top++] = head;
    seen[head] = 1;
    int found = 0;

    while (top > 0 && ok)
    {
        const int node = stack[--top];

        for (int c = 0; c < 2; c++)
        {
            const int child = WrBspNodeChild(r, node, c);

            if (child >= 0)
            {
                if (child >= numNodes)
                {
                    Fail(err, errCap, "node %d's child %d is node %d of %d",
                         node, c, child, numNodes);
                    ok = false;
                    break;
                }
                if (seen[child])
                    continue;           // not a tree; do not walk it twice
                seen[child] = 1;
                stack[top++] = child;
                continue;
            }

            const int leaf = -1 - child;
            if (leaf < 0 || leaf >= numLeafs)
            {
                Fail(err, errCap, "node %d's child %d is leaf %d of %d",
                     node, c, leaf, numLeafs);
                ok = false;
                break;
            }

            int first = 0, num = 0;
            if (!WrBspLeafBrushRange(r, leaf, &first, &num) ||
                first > numLB || num < 0 || first + num > numLB)
            {
                Fail(err, errCap, "leaf %d claims leafbrushes %d..%d of %d",
                     leaf, first, first + num, numLB);
                ok = false;
                break;
            }

            for (int i = 0; i < num; i++)
            {
                const int b = WrBspLeafBrush(r, first + i);
                if (b < 0 || b >= numBrush)
                {
                    Fail(err, errCap, "leafbrush %d references brush %d of %d",
                         first + i, b, numBrush);
                    ok = false;
                    break;
                }

                // A brush appears in every leaf it touches, so most of these
                // are repeats -- the bitmap is what makes the walk a set
                // union rather than a list with duplicates in it.
                if (owned[b])
                    continue;

                // Its sides are checked HERE, once, rather than at clip time
                // for every side of every brush. A brush whose side range
                // leaves the lump is a stride that is wrong somewhere, and
                // that is a fact about the file rather than about this brush.
                int fs = 0, ns = 0;
                if (!WrBspBrush(r, b, &fs, &ns, NULL) ||
                    fs < 0 || ns < 0 || fs + ns > numSides)
                {
                    Fail(err, errCap, "brush %d claims sides %d..%d of %d",
                         b, fs, fs + ns, numSides);
                    ok = false;
                    break;
                }

                owned[b] = 1;
                found++;
            }
        }
    }

    free(seen);
    free(stack);

    if (!ok)
    {
        memset(owned, 0, (size_t)numBrush);
        return false;
    }
    if (ownedOut)
        *ownedOut = found;
    return true;
}

// ---------------------------------------------------------------------------
// Brushes into polygons
// ---------------------------------------------------------------------------
//
// The pipeline itself is in wr_bspgeom.h, hand-checked there against shapes
// whose answers are known. What is here is the bookkeeping around it: which
// brushes to feed it, where to put the results, and what to do when a side
// comes back wrong.
//
// FOUR WAYS A SIDE IS DROPPED, AND ONLY TWO OF THEM ARE ERRORS
//
//   Clipped to nothing. That is a BEVEL, and half to two thirds of all
//   brushsides are one. A bevel is tangent to an edge of the brush rather than
//   bounding a face of it, so clipping it against the real sides leaves a
//   sliver or leaves nothing. Nothing in the file has to mark them -- Strata
//   has a bevel byte and stock Source has a bevel short, and neither is
//   trusted here, because a correct clip already knows.
//
//   A plane that is not a unit normal. 807 planes across four v25 maps in the
//   library are exactly (0, 0, 0) with distance 0. They are referenced by no
//   brushside and no node, so they are harmless -- to a reader that checks.
//
//   Failing the closure assertion. THIS one is an error, and it is the only
//   check anywhere in this file that can catch a struct stride wrong in a way
//   every index check passed. A brush is the intersection of its half-spaces,
//   so no face of it can stick out through another of its sides; if one does,
//   the sides were not the sides of that brush.
//
//   A vertex outside the world. Also an error, and a narrower one: it means
//   the clip never actually closed and the assertion above was satisfied by
//   too few sides, leaving part of the 65536-unit starting quad in the result.

// Grow a packed array, doubling. The first guess is deliberately small
// because the count that would size it exactly -- how many sides survive the
// clip -- is not knowable without doing the clip.
static bool Grow(void **buf, int *cap, int need, size_t elem, size_t *bytes)
{
    if (need <= *cap)
        return true;
    int want = *cap ? *cap * 2 : 1024;
    while (want < need)
        want *= 2;

    void *p = realloc(*buf, (size_t)want * elem);
    if (!p)
        return false;
    *bytes += (size_t)(want - *cap) * elem;
    *buf = p;
    *cap = want;
    return true;
}

// ---------------------------------------------------------------------------
// Brushes that belong to entities rather than to the world
// ---------------------------------------------------------------------------
//
// The worldspawn walk exists to keep trigger volumes out, and it has to: a
// teleport volume on a surf map is characteristically a big slanted box under
// the ramp, 47% of surf_greensway's solid brushes are teleport volumes, and
// drawing one produces a second ramp that is not there.
//
// Excluding every entity to achieve that is too blunt. Model 0 owns 84.1% of
// brushes across the library but only 3.6% on bhop_slope_v2, which is built
// almost entirely out of func_* entities and reads as ZERO surf-band polygons.
//
// The distinction is not world versus entity, it is what the entity IS -- and
// that is written down, in the ENTITIES lump, as a classname next to a
// "model" "*N" reference. So the classnames that are not solid, and every
// trigger, are named and skipped, and the rest of the entity brushes are read.
//
// A DENY LIST RATHER THAN AN ALLOW LIST, deliberately. Momentum, Portal and CS
// mods each add their own brush entities and an allow list would silently drop
// every one it had not heard of -- which is the failure this whole section is
// about. A trigger is recognisable from its name; anything unrecognised is
// solid geometry until something says otherwise.
static bool EntityClassIsSolid(const char *cls)
{
    if (!cls || !*cls)
        return false;

    // Everything Source spawns as a trigger begins with this, mod entities
    // included -- trigger_momentum_timer_start and the rest.
    if (_strnicmp(cls, "trigger_", 8) == 0)
        return false;

    static const char *kNotSolid[] = {
        "func_illusionary",     // explicitly non-solid by definition
        "func_smokevolume",
        "func_precipitation",
        "func_dustcloud",
        "func_dustmotes",
        "func_fog_volume",
        "func_areaportal",
        "func_areaportalwindow",
        "func_occluder",
        "func_viscluster",
        "func_instance_io_proxy",
        "func_ladder",          // a climbable volume, not a surface
        "func_buyzone",
        "func_bomb_target",
        "func_hostage_rescue",
        "func_nobuild",
        "func_lod",
        "env_",                 // prefix: every env_* brush entity is an effect
        "point_",
        "info_",
        "light",
        "filter_",
        "logic_"
    };

    for (int i = 0; i < (int)(sizeof(kNotSolid) / sizeof(kNotSolid[0])); i++)
    {
        const size_t n = strlen(kNotSolid[i]);
        if (_strnicmp(cls, kNotSolid[i], n) == 0)
            return false;
    }
    return true;
}

// Mark the brushes of every solid brush entity, on top of the worldspawn walk.
//
// Returns the number of models added. Never fails the build: an entity lump
// that cannot be parsed leaves the map exactly as the worldspawn walk left it,
// which is what every version before this did.
static int WrBspEntityBrushes(const WrBspRaw *r, unsigned char *owned,
                              int *brushesOut)
{
    if (brushesOut)
        *brushesOut = 0;
    if (!g_wrBspIncludeEntities || !r->data[WR_BSP_L_ENTITIES])
        return 0;

    const int numModels = r->count[WR_BSP_L_MODELS];
    const int numNodes  = r->count[WR_BSP_L_NODES];
    const int numBrush  = r->count[WR_BSP_L_BRUSHES];
    if (numModels <= 1)
        return 0;

    const char *text = (const char *)r->data[WR_BSP_L_ENTITIES];
    const int len = (int)r->bytes[WR_BSP_L_ENTITIES];

    unsigned char *want = (unsigned char *)calloc((size_t)numModels, 1);
    unsigned char *seen = (unsigned char *)calloc((size_t)numNodes, 1);
    int *stack = (int *)malloc((size_t)numNodes * sizeof(int));
    if (!want || !seen || !stack)
    {
        free(want); free(seen); free(stack);
        return 0;
    }

    // One pass over the text. Not a parser -- a scan for the two keys that
    // matter, reset at every brace, which is all the structure this needs.
    char cls[64];
    int model = -1;
    cls[0] = '\0';
    int models = 0;

    for (int i = 0; i < len; i++)
    {
        const char c = text[i];
        if (c == '{')
        {
            cls[0] = '\0';
            model = -1;
            continue;
        }
        if (c == '}')
        {
            if (model > 0 && model < numModels)
            {
                if (!EntityClassIsSolid(cls))
                {
                    // Counted, because "how many trigger volumes did this
                    // refuse" is the number that says the deny list is doing
                    // its job. surf_greensway is 47% teleport volumes by solid
                    // brush count and a teleport volume there is a big slanted
                    // box under the ramp -- exactly the shape that would read
                    // as a second ramp.
                    g_wrBspEntSkipped++;
                }
                else if (!want[model])
                {
                    want[model] = 1;
                    models++;
                }
            }
            cls[0] = '\0';
            model = -1;
            continue;
        }
        if (c != '"')
            continue;

        // "key" "value" -- read the key, then the value that follows it.
        const int keyStart = i + 1;
        int j = keyStart;
        while (j < len && text[j] != '"') j++;
        if (j >= len) break;
        const int keyLen = j - keyStart;

        int k = j + 1;
        while (k < len && text[k] != '"' && text[k] != '\n' && text[k] != '}') k++;
        if (k >= len || text[k] != '"') { i = j; continue; }
        const int valStart = k + 1;
        int e = valStart;
        while (e < len && text[e] != '"') e++;
        if (e >= len) break;
        const int valLen = e - valStart;
        i = e;

        if (keyLen == 9 && _strnicmp(text + keyStart, "classname", 9) == 0)
        {
            int n = valLen;
            if (n > (int)sizeof(cls) - 1) n = (int)sizeof(cls) - 1;
            memcpy(cls, text + valStart, (size_t)n);
            cls[n] = '\0';
        }
        else if (keyLen == 5 && _strnicmp(text + keyStart, "model", 5) == 0)
        {
            // "*12" is a brush model; anything else is a studio model and has
            // no brushes in this file at all.
            if (valLen >= 2 && text[valStart] == '*')
            {
                model = 0;
                for (int d = valStart + 1; d < valStart + valLen; d++)
                {
                    if (text[d] < '0' || text[d] > '9') { model = -1; break; }
                    model = model * 10 + (text[d] - '0');
                    if (model > 1 << 20) { model = -1; break; }
                }
            }
        }
    }

    // Now walk each wanted model's tree, exactly as the worldspawn walk does.
    int added = 0;
    for (int mdl = 1; mdl < numModels; mdl++)
    {
        if (!want[mdl])
            continue;
        const int head = WrBspModelHeadNode(r, mdl);
        if (head < 0 || head >= numNodes)
            continue;

        memset(seen, 0, (size_t)numNodes);
        int top = 0;
        stack[top++] = head;
        seen[head] = 1;

        while (top > 0)
        {
            const int node = stack[--top];
            for (int c = 0; c < 2; c++)
            {
                const int child = WrBspNodeChild(r, node, c);
                if (child >= 0)
                {
                    if (child < numNodes && !seen[child])
                    {
                        seen[child] = 1;
                        stack[top++] = child;
                    }
                    continue;
                }

                const int leaf = -child - 1;
                int first = 0, count = 0;
                if (!WrBspLeafBrushRange(r, leaf, &first, &count))
                    continue;
                for (int b = 0; b < count; b++)
                {
                    const int bi = WrBspLeafBrush(r, first + b);
                    if (bi >= 0 && bi < numBrush && !owned[bi])
                    {
                        owned[bi] = 1;
                        added++;
                    }
                }
            }
        }
    }

    free(want); free(seen); free(stack);
    if (brushesOut)
        *brushesOut = added;
    return models;
}

// ---------------------------------------------------------------------------
// Displacements
// ---------------------------------------------------------------------------
//
// A displacement is a quad from the FACES lump subdivided into a
// (2^power + 1)^2 grid, with every grid vertex pushed along its own direction by
// its own distance. None of that geometry appears in the BRUSHES lump, which is
// why a displacement-built map read as almost empty here: there was nothing to
// skip, because it was never in the input.
//
// Four fields of ddispinfo_t are wanted and all four are in the first forty
// bytes, which is the part that did not move between BSP 19, 20, 21 -- or, as
// it turned out when Strata published the struct, 25 either:
//
//     0   Vector startPosition      which CORNER of the quad the grid starts at
//     12  int    m_iDispVertStart   first row of DISPVERTS
//     20  int    power              2, 3 or 4
//     36  ushort m_iMapFace         the face whose winding is the quad
//                                   (uint on v25, same offset)
//
// dface_t is the one that moved, and it is read in two places below. See the
// FACES row of the stride table.
//
// and from CDispVert, 20 bytes: a unit direction at 0 and a distance at 12.
//
// EVERY INDEX IS CHECKED AGAINST ITS LUMP'S OWN COUNT before it is used, and a
// displacement that fails any check is dropped on its own rather than taking the
// map with it. That is the same standard the brush side of this file holds, and
// it matters more here: these structures reference four other lumps.

#define WR_DISP_MIN_POWER 2
#define WR_DISP_MAX_POWER 4

// Stop adding displacements past this share of the resident limit.
//
// surf_nyx is 1,964 displacements at 9x9, which is 251,392 triangles and about
// 17 MB -- inside the 64 MB cap, but the cap is a REFUSAL and refusing a whole
// map because its terrain is dense would be a worse answer than a partial one.
// Past this, displacements stop and the brush geometry is kept; dispDropped says
// how many were left out and the panel can say so.
#define WR_DISP_BUDGET (WR_BSP_MAX_RESIDENT / 2u)

// One vertex of a face's winding, through SURFEDGES and EDGES.
//
// A surfedge is a SIGNED index: positive reads the edge forwards and takes its
// first vertex, negative reads it backwards and takes its second. The sign is
// the winding, so dropping it gives a quad with two of its corners swapped.
static bool DispFaceVertex(const WrBspRaw *r, int surfEdgeIndex, float out[3])
{
    const int nSurf = r->count[WR_BSP_L_SURFEDGES];
    const int nEdge = r->count[WR_BSP_L_EDGES];
    const int nVert = r->count[WR_BSP_L_VERTEXES];
    if (surfEdgeIndex < 0 || surfEdgeIndex >= nSurf)
        return false;

    const int se = (int)Rd32(r->data[WR_BSP_L_SURFEDGES] +
                             (size_t)surfEdgeIndex * 4);

    const int edge = (se < 0) ? -se : se;
    const int which = (se < 0) ? 1 : 0;
    if (edge < 0 || edge >= nEdge)
        return false;

    const unsigned char *e = r->data[WR_BSP_L_EDGES] +
                             (size_t)edge * (size_t)r->stride[WR_BSP_L_EDGES];

    // Two shorts on 19/20/21, two ints on Strata -- the stride says which.
    int v;
    if (r->stride[WR_BSP_L_EDGES] == 8)
        v = (int)Rd32(e + which * 4);
    else
        v = (int)Rd16(e + which * 2);

    if (v < 0 || v >= nVert)
        return false;

    const unsigned char *p = r->data[WR_BSP_L_VERTEXES] + (size_t)v * 12;
    out[0] = RdF(p);
    out[1] = RdF(p + 4);
    out[2] = RdF(p + 8);
    return true;
}

static bool WrBspBuildDisplacements(const WrBspRaw *r, WrBspMap *m,
                                    int *vertCap, int *polyCap)
{
    // Any of these absent means this map has no displacements this reader can
    // build -- either it has none at all, or one of the six lumps declares a
    // version with no row in the stride table and was skipped. Both are reported
    // by hasDisplacements rather than pretended away.
    if (!g_wrBspBuildDisp)
        return true;
    if (!r->data[WR_BSP_L_DISPINFO] || !r->data[WR_BSP_L_DISPVERTS] ||
        !r->data[WR_BSP_L_FACES] || !r->data[WR_BSP_L_VERTEXES] ||
        !r->data[WR_BSP_L_EDGES] || !r->data[WR_BSP_L_SURFEDGES])
        return true;

    const int nDisp = r->count[WR_BSP_L_DISPINFO];
    const int nDV   = r->count[WR_BSP_L_DISPVERTS];
    const int nFace = r->count[WR_BSP_L_FACES];
    const int dStride = r->stride[WR_BSP_L_DISPINFO];
    const int fStride = r->stride[WR_BSP_L_FACES];
    const bool strata = Strata(r);

    // Before any test, so "4 of 756 were not built" can be said rather than
    // "4 were skipped", which reads as a rounding error and was not one.
    m->dispTotal = nDisp;

    for (int d = 0; d < nDisp; d++)
    {
        const unsigned char *di = r->data[WR_BSP_L_DISPINFO] +
                                  (size_t)d * (size_t)dStride;

        const float startPos[3] = { RdF(di), RdF(di + 4), RdF(di + 8) };
        const int vertStart = (int)Rd32(di + 12);
        const int power     = (int)Rd32(di + 20);

        // m_iMapFace: same offset on both layouts, different width. Reading a
        // v25 map's 32-bit field as 16 bits is correct on a little-endian
        // machine right up to the 65,536th face and then silently names the
        // wrong one -- and raising limits like that is what v25 is FOR.
        const int faceIndex = strata ? RdI32(di + 36) : Rd16(di + 36);

        if (power < WR_DISP_MIN_POWER || power > WR_DISP_MAX_POWER)
        {
            m->dispDropBy[WR_DISP_DROP_POWER]++;
            m->dispDropped++;
            continue;
        }
        if (faceIndex < 0 || faceIndex >= nFace)
        {
            m->dispDropBy[WR_DISP_DROP_FACEINDEX]++;
            m->dispDropped++;
            continue;
        }

        const int side = (1 << power) + 1;       // 5, 9 or 17
        const int need = side * side;
        if (vertStart < 0 || vertStart + need > nDV)
        {
            m->dispDropBy[WR_DISP_DROP_VERTSTART]++;
            m->dispDropped++;
            continue;
        }

        // The base quad. dface_t: firstedge and numedges, and BOTH of them moved
        // on v25 -- 4 and 8 on stock, 8 and 12 on Strata's, where numedges is
        // also an int rather than a short. See the FACES row of the stride table
        // for the full layout and for what was checked against the files.
        //
        // This is the one place the two layouts are read, and it is deliberately
        // driven off the FILE's version rather than off fStride: a stride is a
        // size and these are offsets, which is the whole reason v25 sat refused
        // for as long as it did.
        //
        // A displacement is always built on a four-sided face; anything else is
        // not one this knows how to subdivide.
        const unsigned char *f = r->data[WR_BSP_L_FACES] +
                                 (size_t)faceIndex * (size_t)fStride;
        const int firstEdge = strata ? (int)Rd32(f + 8)  : (int)Rd32(f + 4);
        const int numEdges  = strata ? (int)Rd32(f + 12) : Rd16(f + 8);

        // dface_t.side: the face uses the BACK of its plane. u8 at 2 on stock
        // and at 4 on v25, moved by the same widening that moved firstedge.
        //
        // THIS IS NOT COSMETIC, AND IT WAS WRONG HERE FOR AS LONG AS
        // DISPLACEMENTS HAVE BEEN READ. A displacement triangle's normal is
        // computed below from the cross product of the grid traversal, and the
        // traversal's handedness follows the base quad's winding -- so on a face
        // that sits on the back of its plane, every triangle came out pointing
        // INTO the surface. WrBspIsSurfBand wants a positive n.z, so those
        // triangles were built, stored, counted in solidArea, and then dropped
        // out of the surf band as though they were ceilings.
        //
        // Measured over the 543,488 displacement triangles in this library, by
        // comparing each built normal against the face's own oriented normal --
        // planenum through the PLANES lump, negated when this flag is set, which
        // is an answer the file gives directly and this builder never asked for:
        //
        //     side = 0    319,768 agree      90 flipped     0.0%
        //     side = 1         47 agree 217,377 flipped   100.0%
        //
        // 40% of every displacement triangle in the library, inverted, and the
        // flag that says so was four bytes away the whole time. It cost 384,068
        // square units of surf-band displacement area -- 53.5% of the total --
        // on maps whose ramps are made of nothing else.
        //
        // Rotating the base quad cannot fix it and WrBspDispBaseCorner is right
        // not to try: a rotation preserves cyclic order, so it preserves
        // handedness. Only a reversal changes the sign, which is what this does.
        const bool backSide = strata ? (f[4] != 0) : (f[2] != 0);
        if (numEdges != 4)
        {
            m->dispDropBy[WR_DISP_DROP_NOTQUAD]++;
            m->dispDropped++;
            continue;
        }

        float quad[4][3];
        bool got = true;
        for (int k = 0; k < 4 && got; k++)
            got = DispFaceVertex(r, firstEdge + k, quad[k]);
        if (!got)
        {
            m->dispDropBy[WR_DISP_DROP_FACEVERTEX]++;
            m->dispDropped++;
            continue;
        }

        // WHICH CORNER THE GRID STARTS AT -- the rule and its two-sided
        // measurement live in WrBspDispBaseCorner, wr_bspgeom.h. It moved there
        // because it is pure arithmetic on four corners and a point, which means
        // it can be driven against quads whose answer is known by hand with no
        // .bsp anywhere near it, and tests\test_bspgeom.exe now does.
        //
        // REFUSED BY NAME: startPosition names no corner of this quad. That is
        // what a moved struct prefix looks like from here, and it is the only
        // thing this test has ever been able to see.
        int base = 0;
        if (!WrBspDispBaseCorner(quad, startPos, &base, 0, 0))
        {
            m->dispDropBy[WR_DISP_DROP_CORNER]++;
            m->dispDropped++;
            continue;
        }

        // UNCHANGED, and it is the reason relaxing that tolerance is safe rather
        // than merely tolerable: the built surface comes from `quad`, so `base`
        // only decides which corner is index 0. A looser match cannot displace
        // geometry by a unit; it can only pick a different corner, and the
        // margin measurement says that choice is nowhere near close.
        float c[4][3];
        for (int k = 0; k < 4; k++)
            for (int a = 0; a < 3; a++)
                c[k][a] = quad[(base + k) & 3][a];

        // The grid, in a LOCAL. 17x17 is the largest a displacement can be, so
        // this is 3.4 KB of stack and it never has to grow.
        //
        // Deliberately not built inside m->verts: the grid is scaffolding, and a
        // polygon there owns a contiguous run of its own vertices, so every grid
        // point would be stored twice over -- once as scaffolding that nothing
        // ever reads again, and once per triangle that uses it. On surf_nyx that
        // is 159,000 vertices of pure waste. It also means m->verts cannot be
        // reallocated underneath a pointer into it, which the first draft of
        // this did do.
        float grid[(1 << WR_DISP_MAX_POWER) + 1][(1 << WR_DISP_MAX_POWER) + 1][3];

        // Row i runs from the c0->c1 edge to the c3->c2 edge, column j across
        // between them -- the traversal vbsp writes the vertices in, so
        // DISPVERTS is indexed i * side + j.
        for (int i = 0; i < side; i++)
        {
            const float it = (float)i / (float)(side - 1);
            for (int j = 0; j < side; j++)
            {
                const float jt = (float)j / (float)(side - 1);

                const unsigned char *dv = r->data[WR_BSP_L_DISPVERTS] +
                                          (size_t)(vertStart + i * side + j) * 20;
                const float dist = RdF(dv + 12);

                for (int a = 0; a < 3; a++)
                {
                    const float lhs = c[0][a] + (c[1][a] - c[0][a]) * it;
                    const float rhs = c[3][a] + (c[2][a] - c[3][a]) * it;
                    const float p = lhs + (rhs - lhs) * jt + RdF(dv + a * 4) * dist;
                    grid[i][j][a] = p;
                    if (p < m->mins[a]) m->mins[a] = p;
                    if (p > m->maxs[a]) m->maxs[a] = p;
                }
            }
        }

        // Two triangles a cell, each with its own plane -- a displacement's four
        // corners are not coplanar, and averaging them would smooth away the
        // very slope being measured.
        //
        // Grown once for the whole displacement so nothing reallocates mid-cell.
        const int cells = side - 1;
        const int maxTris = cells * cells * 2;
        if (!Grow((void **)&m->polys, polyCap, m->polyCount + maxTris,
                  sizeof(WrBspPoly), &m->bytes) ||
            !Grow((void **)&m->verts, vertCap, m->vertCount + maxTris * 3,
                  sizeof(float) * 3, &m->bytes))
            return false;

        for (int i = 0; i < cells; i++)
            for (int j = 0; j < cells; j++)
            {
                const float *q[4] = { grid[i][j], grid[i][j + 1],
                                      grid[i + 1][j + 1], grid[i + 1][j] };
                const int tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };

                for (int t = 0; t < 2; t++)
                {
                    // The reversal that puts a back-side face's normal the
                    // right way out. Swapping two corners flips the cross
                    // product AND the stored winding together, which is what
                    // keeps "the winding agrees with its own plane" true --
                    // negating the normal alone would leave every one of these
                    // triangles inside out instead.
                    const int k1 = backSide ? tri[t][2] : tri[t][1];
                    const int k2 = backSide ? tri[t][1] : tri[t][2];

                    const float *a  = q[tri[t][0]];
                    const float *b  = q[k1];
                    const float *cc = q[k2];

                    float u[3], v[3], nrm[3];
                    for (int k = 0; k < 3; k++)
                    {
                        u[k] = b[k] - a[k];
                        v[k] = cc[k] - a[k];
                    }
                    nrm[0] = u[1] * v[2] - u[2] * v[1];
                    nrm[1] = u[2] * v[0] - u[0] * v[2];
                    nrm[2] = u[0] * v[1] - u[1] * v[0];

                    const float len = (float)sqrt((double)nrm[0] * nrm[0] +
                                                  (double)nrm[1] * nrm[1] +
                                                  (double)nrm[2] * nrm[2]);
                    // The cross product's length is twice the area, and a
                    // degenerate cell has none. Dropping it loses nothing: there
                    // is no surface there to stand on either.
                    if (len < 1e-6f)
                        continue;
                    const float area = len * 0.5f;
                    if (area < WR_BSP_MIN_AREA)
                        continue;

                    for (int k = 0; k < 3; k++)
                        nrm[k] /= len;

                    WrBspPoly *p = &m->polys[m->polyCount++];
                    p->plane[0] = nrm[0];
                    p->plane[1] = nrm[1];
                    p->plane[2] = nrm[2];
                    p->plane[3] = nrm[0] * a[0] + nrm[1] * a[1] + nrm[2] * a[2];
                    p->first = m->vertCount;
                    p->count = 3;
                    p->area = area;
                    p->flags = WR_BSP_POLY_DISP;

                    const int wound[3] = { tri[t][0], k1, k2 };
                    for (int k = 0; k < 3; k++)
                        for (int aa = 0; aa < 3; aa++)
                            m->verts[m->vertCount + k][aa] = q[wound[k]][aa];
                    m->vertCount += 3;

                    m->dispPolys++;
                    m->solidArea += area;
                    if (WrBspIsSurfBand(nrm[2]))
                    {
                        m->surfPolys++;
                        m->surfArea += area;
                    }
                }
            }

        if (m->bytes > WR_DISP_BUDGET)
        {
            // Out of room. Everything already built stays; the rest are counted
            // so the panel can say the map is only partly here rather than
            // implying it is all of it.
            m->dispDropBy[WR_DISP_DROP_BUDGET] += (nDisp - d - 1);
            m->dispDropped += (nDisp - d - 1);
            break;
        }
    }

    return true;
}

// Include brushes that are PLAYERCLIP and not solid?
//
// ON, and it was off for as long as this reader existed. wr_bsp.h used to say
// "whether it should be included is a real question and bsp_sweep --contents
// counts them so it can be answered with a number; today it is not." It is now,
// and the number is large.
//
// The library holds 141,841 of them. They are invisible in game, which is why
// they were skipped -- and on a surf map a clip brush is very often the thing
// actually being ridden, because a mapper builds the visible ramp out of
// something non-solid and puts the collision on a clip beside it. Skipping them
// meant this reader had no polygon within 48 units of the player during genuine
// contact a quarter of the time.
//
// Measured with tests\phase_sweep.exe --live --maps [--clip], on maps with no
// displacements, letting the geometry veto a contact whose nearest surface is
// more than 24 units away:
//
//     as shipped, no veto            92.2%  (miss 0.3  fake 7.6)
//     veto, clip brushes skipped     73.8%  (miss 26.0 fake 0.2)
//     veto, clip brushes INCLUDED    98.3%  (miss  1.3 fake 0.4)
//
// So it is not a tuning knob, it is the difference between the geometry being
// usable and being actively harmful. It stays a knob only so the harness can
// show both rows.
//
// Drawing is a separate question and the answer there is still no: see
// WrBspSurfNear, which is the query the renderer uses and the one place a clip
// polygon is filtered back out.
bool g_wrBspIncludeClip = true;
bool g_wrBspDrawClip = true;
bool g_wrBspBuildDisp = true;
bool g_wrBspIncludeEntities = true;
int g_wrBspEntSkipped = 0;      // brush entities refused as non-solid

static bool BuildGrid(WrBspMap *m, char *err, int errCap);

bool WrBspBuild(const WrBspRaw *r, WrBspMap *out, char *err, int errCap)
{
    if (err && errCap > 0)
        err[0] = '\0';
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!r || !r->data[WR_BSP_L_PLANES])
    {
        Fail(err, errCap, "nothing to build from");
        return false;
    }

    const int numBrush = r->count[WR_BSP_L_BRUSHES];

    unsigned char *owned = (unsigned char *)malloc((size_t)numBrush);
    if (!owned)
    {
        Fail(err, errCap, "out of memory");
        return false;
    }
    int worldCount = 0;
    if (!WrBspWorldBrushes(r, owned, &worldCount, err, errCap))
    {
        free(owned);
        return false;
    }

    // Then the solid brush entities, on top of the same array. Triggers and the
    // non-solid func_* are named and skipped; see WrBspEntityBrushes.
    int entBrushes = 0;
    const int entModels = WrBspEntityBrushes(r, owned, &entBrushes);

    WrBspMap m;
    memset(&m, 0, sizeof(m));
    m.version = r->version;
    m.compressed = r->compressed;
    m.hasDisplacements = (r->dispInfoBytes != 0);
    m.brushTotal = numBrush;
    m.brushWorld = worldCount;
    m.entModels = entModels;
    m.entBrushes = entBrushes;
    for (int k = 0; k < 3; k++)
    {
        m.mins[k] = 1e30f;
        m.maxs[k] = -1e30f;
    }

    int vertCap = 0, polyCap = 0;
    bool ok = true;

    // Two ping-ponged working buffers and the brush's own side planes. All on
    // the stack and all fixed: a brush cannot have more sides than
    // WR_BSP_MAX_BRUSH_SIDES, and the walk already refused any that claimed a
    // side range outside the lump.
    static float bufA[WR_BSP_MAX_POLY_VERTS][3];
    static float bufB[WR_BSP_MAX_POLY_VERTS][3];
    float sides[WR_BSP_MAX_BRUSH_SIDES][4];

    for (int b = 0; b < numBrush && ok; b++)
    {
        if (!owned[b])
            continue;

        int firstSide = 0, sideCount = 0, contents = 0;
        if (!WrBspBrush(r, b, &firstSide, &sideCount, &contents))
            continue;

        const bool solid = (contents & WR_BSP_CONTENTS_SOLID) != 0;
        const bool clip = (contents & WR_BSP_CONTENTS_PLAYERCLIP) != 0;

        // Counted whether or not it is used, so the coverage line means the
        // same thing either way.
        if (!solid && clip)
            m.brushClipOnly++;

        if (!solid && !(g_wrBspIncludeClip && clip))
            continue;
        m.brushSolid++;

        if (sideCount < 4 || sideCount > WR_BSP_MAX_BRUSH_SIDES)
            continue;       // fewer than four half-spaces is not a volume

        // Gather the planes once. Reading them inside the clip loop would
        // fetch the same twenty bytes sideCount times for every side.
        bool haveAll = true;
        for (int s = 0; s < sideCount; s++)
        {
            const int pn = WrBspBrushSidePlane(r, firstSide + s);
            if (pn < 0 || !WrBspPlane(r, pn, sides[s]))
            {
                haveAll = false;
                break;
            }
        }
        if (!haveAll)
        {
            Fail(err, errCap, "brush %d has a side with no plane", b);
            ok = false;
            break;
        }

        m.sideTotal += sideCount;

        for (int s = 0; s < sideCount; s++)
        {
            int n = WrBspStartQuad(sides[s], bufA);
            if (!n)
            {
                m.sideDegenerate++;
                continue;
            }

            float (*cur)[3] = bufA, (*nxt)[3] = bufB;
            for (int o = 0; o < sideCount && n; o++)
            {
                if (o == s)
                    continue;
                n = WrBspClipToPlane(cur, n, sides[o], nxt);
                float (*t)[3] = cur; cur = nxt; nxt = t;
            }

            if (n < 3)
            {
                m.sideDropped++;        // a bevel, almost always
                continue;
            }

            const float area = WrBspPolyArea(cur, n);
            if (area < WR_BSP_MIN_AREA)
            {
                m.sideDropped++;        // a sliver, which is the same thing
                continue;
            }

            if (!WrBspPolyClosed(cur, n, sides, sideCount, s))
            {
                m.sideNotClosed++;
                continue;
            }

            bool sane = true;
            for (int i = 0; i < n && sane; i++)
                for (int k = 0; k < 3; k++)
                    if (!(cur[i][k] > -WR_WORLD_LIMIT &&
                          cur[i][k] < WR_WORLD_LIMIT))
                        sane = false;
            if (!sane)
            {
                m.sideTooFar++;
                continue;
            }

            if (!Grow((void **)&m.verts, &vertCap, m.vertCount + n,
                      sizeof(float) * 3, &m.bytes) ||
                !Grow((void **)&m.polys, &polyCap, m.polyCount + 1,
                      sizeof(WrBspPoly), &m.bytes))
            {
                Fail(err, errCap, "out of memory");
                ok = false;
                break;
            }

            if (m.bytes > WR_BSP_MAX_RESIDENT)
            {
                Fail(err, errCap,
                     "the geometry needs over %u bytes, which is the limit",
                     (unsigned int)WR_BSP_MAX_RESIDENT);
                ok = false;
                break;
            }

            WrBspPoly *p = &m.polys[m.polyCount++];
            memcpy(p->plane, sides[s], sizeof(p->plane));
            p->first = m.vertCount;
            p->count = n;
            p->area = area;
            p->flags = solid ? 0u : WR_BSP_POLY_CLIP;

            for (int i = 0; i < n; i++)
                for (int k = 0; k < 3; k++)
                {
                    const float v = cur[i][k];
                    m.verts[m.vertCount + i][k] = v;
                    if (v < m.mins[k]) m.mins[k] = v;
                    if (v > m.maxs[k]) m.maxs[k] = v;
                }
            m.vertCount += n;

            m.solidArea += area;
            if (WrBspIsSurfBand(sides[s][2]))
            {
                m.surfPolys++;
                m.surfArea += area;
                // Counted separately because the drawing query used to discard
                // exactly these, and the panel reported the total -- so a map
                // could honestly say "218 in the surf band" and honestly draw
                // nothing. A number nobody can act on is worse than no number.
                if (!solid)
                    m.surfClipPolys++;
            }
        }
    }

    free(owned);

    // The displacements, after the brushes and from a different set of lumps
    // entirely. A map with none, or one whose layout this reader does not know,
    // simply adds nothing here -- see WrBspBuildDisplacements.
    if (ok && !WrBspBuildDisplacements(r, &m, &vertCap, &polyCap))
    {
        Fail(err, errCap, "out of memory building displacements");
        ok = false;
    }

    if (!ok)
    {
        WrBspFreeMap(&m);
        return false;
    }

    if (m.polyCount == 0)
    {
        // Not an error. A map really can have almost no world geometry --
        // bhop_slope_v2 has 48 world brushes out of 1,351 -- and an empty
        // bounding box would put the grid somewhere arbitrary, so it is made
        // explicit rather than left at its sentinels.
        for (int k = 0; k < 3; k++)
        {
            m.mins[k] = 0.0f;
            m.maxs[k] = 1.0f;
        }
    }

    if (!BuildGrid(&m, err, errCap))
    {
        WrBspFreeMap(&m);
        return false;
    }

    *out = m;
    return true;
}

// ---------------------------------------------------------------------------
// The grid
// ---------------------------------------------------------------------------
//
// A polygon goes in every cell its bounding box touches AND whose box its
// plane actually passes through. The second half is not an optimisation of the
// first -- it is the difference between a workable index and one that is
// mostly wasted. A surf ramp is a thin sheet lying diagonally across a large
// box, so its AABB can cover a hundred cells while the ramp itself passes
// through a dozen. Testing the plane against each cell's own box costs eight
// dot products and removes the rest.
//
// It is still conservative: a cell can straddle the plane without containing
// any of the polygon. That is fine and it is the right direction to be wrong
// in -- a cell holding a polygon it does not really contain costs one extra
// test at query time, where a missing one costs a wrong answer.

static bool PlaneCrossesCell(const float p[4], const float lo[3],
                             const float hi[3])
{
    // The two corners furthest along and against the normal bracket every
    // other corner, so two dot products decide it rather than eight.
    float near_ = 0.0f, far_ = 0.0f;
    for (int k = 0; k < 3; k++)
    {
        if (p[k] >= 0.0f) { near_ += p[k] * lo[k]; far_ += p[k] * hi[k]; }
        else              { near_ += p[k] * hi[k]; far_ += p[k] * lo[k]; }
    }
    return near_ <= p[3] + WR_BSP_ON_EPSILON &&
           far_  >= p[3] - WR_BSP_ON_EPSILON;
}

static bool BuildGrid(WrBspMap *m, char *err, int errCap)
{
    WrBspGridFit(m->mins, m->maxs, &m->grid);
    const int cells = WrBspGridCellCount(&m->grid);
    if (cells <= 0)
    {
        Fail(err, errCap, "the map has no extent to index");
        return false;
    }

    int *counts = (int *)calloc((size_t)cells + 1, sizeof(int));
    if (!counts)
    {
        Fail(err, errCap, "out of memory");
        return false;
    }

    // Two passes over the same loop, because the exact item count is not
    // knowable without doing the work: pass 0 counts, pass 1 fills.
    long long total = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        if (pass == 1)
        {
            // Prefix sum turns the counts into starts. cellStart[cells] is the
            // terminator, so every cell's range is a subtraction.
            int running = 0;
            for (int c = 0; c < cells; c++)
            {
                const int n = counts[c];
                counts[c] = running;
                running += n;
            }
            counts[cells] = running;

            m->cellItems = (int *)malloc((size_t)(running ? running : 1)
                                         * sizeof(int));
            if (!m->cellItems)
            {
                free(counts);
                Fail(err, errCap, "out of memory");
                return false;
            }
            m->itemCount = running;
            m->bytes += (size_t)running * sizeof(int)
                      + (size_t)(cells + 1) * sizeof(int);
            if (m->bytes > WR_BSP_MAX_RESIDENT)
            {
                free(counts);
                Fail(err, errCap,
                     "the geometry needs over %u bytes, which is the limit",
                     (unsigned int)WR_BSP_MAX_RESIDENT);
                return false;
            }
        }

        for (int i = 0; i < m->polyCount; i++)
        {
            const WrBspPoly *p = &m->polys[i];
            const float (*v)[3] = m->verts + p->first;

            float bmin[3], bmax[3];
            for (int k = 0; k < 3; k++) { bmin[k] = v[0][k]; bmax[k] = v[0][k]; }
            for (int j = 1; j < p->count; j++)
                for (int k = 0; k < 3; k++)
                {
                    if (v[j][k] < bmin[k]) bmin[k] = v[j][k];
                    if (v[j][k] > bmax[k]) bmax[k] = v[j][k];
                }

            int lo[3], hi[3];
            WrBspGridCell(&m->grid, bmin, lo);
            WrBspGridCell(&m->grid, bmax, hi);

            for (int z = lo[2]; z <= hi[2]; z++)
                for (int y = lo[1]; y <= hi[1]; y++)
                    for (int x = lo[0]; x <= hi[0]; x++)
                    {
                        float cl[3], ch[3];
                        const int cc[3] = { x, y, z };
                        for (int k = 0; k < 3; k++)
                        {
                            cl[k] = m->grid.mins[k] + m->grid.cell[k] * cc[k];
                            ch[k] = cl[k] + m->grid.cell[k];
                        }
                        if (!PlaneCrossesCell(p->plane, cl, ch))
                            continue;

                        const int c = WrBspGridIndex(&m->grid, x, y, z);
                        if (pass == 0)
                        {
                            counts[c]++;
                            total++;
                        }
                        else
                        {
                            m->cellItems[counts[c]++] = i;
                        }
                    }
        }

        if (pass == 0 && total > 40LL * 1000LL * 1000LL)
        {
            free(counts);
            Fail(err, errCap, "the grid would need %lld entries", total);
            return false;
        }
    }

    // counts has been walked forward by the fill, so entry c now holds what
    // entry c+1 should. Shifting it back is cheaper than keeping two arrays.
    for (int c = cells; c > 0; c--)
        counts[c] = counts[c - 1];
    counts[0] = 0;

    m->cellStart = counts;
    return true;
}

// ---------------------------------------------------------------------------
// The queries
// ---------------------------------------------------------------------------

bool WrBspTraceRay(const WrBspMap *m, const float start[3], const float dir[3],
                   float maxDist, int *polyOut, float *tOut)
{
    if (!m || !m->cellStart || m->polyCount <= 0)
        return false;

    WrBspDda w;
    if (!WrBspDdaBegin(&m->grid, start, dir, maxDist, &w))
        return false;

    int best = -1;
    float bestT = maxDist;

    do
    {
        // The walk's cells only ever start further along, so once the nearest
        // hit is behind where this cell begins, nothing after it can beat it.
        if (best >= 0 && bestT < w.t)
            break;

        const int c = WrBspGridIndex(&m->grid, w.c[0], w.c[1], w.c[2]);
        for (int k = m->cellStart[c]; k < m->cellStart[c + 1]; k++)
        {
            const int i = m->cellItems[k];
            const WrBspPoly *p = &m->polys[i];
            float t;
            if (!WrBspRayPoly(start, dir, p->plane, m->verts + p->first,
                              p->count, &t))
                continue;
            if (t < bestT)
            {
                bestT = t;
                best = i;
            }
        }
    } while (WrBspDdaNext(&m->grid, &w));

    if (best < 0)
        return false;
    if (polyOut) *polyOut = best;
    if (tOut) *tOut = bestT;
    return true;
}

// The cell range a sphere touches, clamped into the grid.
static void CellBox(const WrBspMap *m, const float pt[3], float radius,
                    int lo[3], int hi[3])
{
    float a[3], b[3];
    for (int k = 0; k < 3; k++)
    {
        a[k] = pt[k] - radius;
        b[k] = pt[k] + radius;
    }
    WrBspGridCell(&m->grid, a, lo);
    WrBspGridCell(&m->grid, b, hi);
}

int WrBspSurfNear(const WrBspMap *m, const float pt[3], float radius,
                  int *out, int cap)
{
    if (!m || !m->cellStart || !out || cap <= 0)
        return 0;

    int lo[3], hi[3];
    CellBox(m, pt, radius, lo, hi);

    // Insertion sort into the output, nearest first, with the duplicate check
    // folded in. A polygon is listed in every cell its plane crosses, so
    // without the check a ramp lying across nine cells comes back nine times.
    //
    // The ceiling was 64, which silently overrode the panel's own 8..512 slider
    // and was invisible while a ramp was one or two large brush faces. A
    // displacement ramp is not: it is a grid of triangles about fifty units
    // across, so 64 of them is one small patch of one surface and the rest of
    // the ramp simply is not drawn. WR_BSP_SURF_MAX matches the slider.
    float dist[WR_BSP_SURF_MAX];
    if (cap > WR_BSP_SURF_MAX)
        cap = WR_BSP_SURF_MAX;
    int n = 0;

    for (int z = lo[2]; z <= hi[2]; z++)
        for (int y = lo[1]; y <= hi[1]; y++)
            for (int x = lo[0]; x <= hi[0]; x++)
            {
                const int c = WrBspGridIndex(&m->grid, x, y, z);
                for (int k = m->cellStart[c]; k < m->cellStart[c + 1]; k++)
                {
                    const int i = m->cellItems[k];
                    const WrBspPoly *p = &m->polys[i];
                    if (!WrBspIsSurfBand(p->plane[2]))
                        continue;

                    // THIS is the query that draws, so it is the one that gets
                    // to decide about clip brushes. It used to refuse them
                    // outright: a clip brush is collision with no material, and
                    // outlining one puts a ramp on screen that is not on screen
                    // in the game.
                    //
                    // The trouble is what that leaves on the maps built out of
                    // them, which is nothing at all -- while surfPolys, counted
                    // before this filter, went on reporting hundreds in the surf
                    // band. Two thirds of surf_ethereal's world brushes are
                    // clip-only. An invisible surface you ride is still the ramp,
                    // so it is drawn, in a style of its own, and it can be turned
                    // off. See g_wrBspDrawClip.
                    if (!g_wrBspDrawClip && (p->flags & WR_BSP_POLY_CLIP))
                        continue;

                    bool already = false;
                    for (int j = 0; j < n && !already; j++)
                        if (out[j] == i)
                            already = true;
                    if (already)
                        continue;

                    const float d = WrBspPointPolyDist(pt, p->plane,
                                                       m->verts + p->first,
                                                       p->count);
                    if (d > radius)
                        continue;

                    int at = n < cap ? n : cap - 1;
                    if (n == cap && d >= dist[cap - 1])
                        continue;
                    while (at > 0 && dist[at - 1] > d)
                    {
                        dist[at] = dist[at - 1];
                        out[at] = out[at - 1];
                        at--;
                    }
                    dist[at] = d;
                    out[at] = i;
                    if (n < cap)
                        n++;
                }
            }

    return n;
}

bool WrBspNearestFaceEx(const WrBspMap *m, const float pt[3], float radius,
                        int *polyOut, float *distOut,
                        int *rampOut, float *rampDistOut)
{
    if (rampOut)     *rampOut = -1;
    if (rampDistOut) *rampDistOut = -1.0f;

    if (!m || !m->cellStart)
        return false;

    int lo[3], hi[3];
    CellBox(m, pt, radius, lo, hi);

    int best = -1;
    float bestD = radius;

    // The nearest one that could be RIDDEN, tracked in the same walk. Two
    // answers out of one pass costs nothing and, more usefully, guarantees they
    // describe the same instant -- asking twice would let the player move
    // between the question about touching and the question about the ramp.
    int bestRamp = -1;
    float bestRampD = radius;

    for (int z = lo[2]; z <= hi[2]; z++)
        for (int y = lo[1]; y <= hi[1]; y++)
            for (int x = lo[0]; x <= hi[0]; x++)
            {
                const int c = WrBspGridIndex(&m->grid, x, y, z);
                for (int k = m->cellStart[c]; k < m->cellStart[c + 1]; k++)
                {
                    const int i = m->cellItems[k];
                    const WrBspPoly *p = &m->polys[i];
                    const float d = WrBspPointPolyDist(pt, p->plane,
                                                       m->verts + p->first,
                                                       p->count);
                    if (d < bestD)
                    {
                        bestD = d;
                        best = i;
                    }
                    if (d < bestRampD && WrBspIsRampPlane(p->plane[2]))
                    {
                        bestRampD = d;
                        bestRamp = i;
                    }
                }
            }

    if (best < 0)
        return false;
    if (polyOut) *polyOut = best;
    if (distOut) *distOut = bestD;
    if (bestRamp >= 0)
    {
        if (rampOut)     *rampOut = bestRamp;
        if (rampDistOut) *rampDistOut = bestRampD;
    }
    return true;
}

bool WrBspNearestFace(const WrBspMap *m, const float pt[3], float radius,
                      int *polyOut, float *distOut)
{
    return WrBspNearestFaceEx(m, pt, radius, polyOut, distOut, 0, 0);
}

void WrBspFreeMap(WrBspMap *m)
{
    if (!m)
        return;
    free(m->polys);
    free(m->verts);
    free(m->cellStart);
    free(m->cellItems);
    memset(m, 0, sizeof(*m));
}

int WrBspDispWorstDrop(const WrBspMap *m)
{
    if (!m)
        return WR_DISP_DROP_POWER;
    int best = 0;
    for (int i = 1; i < WR_DISP_DROP__COUNT; i++)
        if (m->dispDropBy[i] > m->dispDropBy[best])
            best = i;
    return best;
}

// A map can be fully world-owned and still have almost no surfable brush
// geometry, because its ramps are displacements. 51 maps in the library are like
// that. There is no ratio to test -- a bhop map legitimately has no surf band at
// all -- so this only reports ownership, and the caller decides what to do about
// a surf map with none.
bool WrBspCoverageThin(const WrBspMap *m)
{
    if (!m || m->brushTotal <= 0)
        return false;
    // Entity brushes count towards coverage now that they are read. Without
    // this, bhop_slope_v2 -- 3.6% world-owned and almost entirely func_* --
    // would still be called thin after the very change that reads it.
    const int have = m->brushWorld + m->entBrushes;
    return (float)have / (float)m->brushTotal < WR_BSP_THIN_OWNED;
}

bool WrBspGeometryComplete(const WrBspMap *m)
{
    if (!m || m->polyCount <= 0)
        return false;

    // Displacements used to disqualify a map outright, because none of them were
    // read and a veto that says "nothing is there" is worse than no veto when
    // half the map is missing. They are read now -- so the test is no longer
    // whether the map HAS them but whether any were LEFT OUT.
    //
    // dispDropped covers three things, and dispDropBy says which: a displacement
    // lump whose version has no row in the stride table, so it was skipped and
    // dispPolys is 0 -- which was every v25 map until Strata published the
    // structs, and is now a statement about the next bump rather than about
    // anything in this library; a map dense enough to run into WR_DISP_BUDGET;
    // and a displacement refused one at a time by one of the per-displacement
    // tests.
    //
    // That third used to be the common case and is now measured to zero across
    // the library. It was 88 refusals on 16 maps, every one of them the
    // startPosition corner test, and every one of them fixed by making that test
    // relative to the quad it judges -- see WR_DISP_CORNER_SLACK. Four of those
    // 88 were surf_kvas, where they cost the whole level its live map query and
    // the strafe readout said "no surface" on every ramp. If it comes back,
    // dispDropBy[WR_DISP_DROP_CORNER] names it instead of leaving the next
    // person to recover the cause arithmetically.
    if (m->hasDisplacements && (m->dispPolys <= 0 || m->dispDropped > 0))
        return false;

    return !WrBspCoverageThin(m);
}
