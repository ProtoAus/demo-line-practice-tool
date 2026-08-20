// test_bsp.cpp  --  the map file layer, and mostly the exact words it refuses
// with.
//
// WHY REFUSALS ARE THE SUBJECT
//
// Everything downstream of this file is geometry, and geometry has no failure
// mode that looks like a failure. A wrong struct stride does not crash and does
// not return an error: it produces planes, at angles, in places, and the only
// way anybody finds out is that a ramp is drawn somewhere there is no ramp. So
// the reader is written to refuse anything it is not certain of, and this
// harness exists to make sure the refusals are reachable, that they say which
// lump, and that they cannot be quietly replaced later by a branch that assumes
// the layout "is probably still the same".
//
// THE ONE CHECK THAT IS NOT ABOUT REFUSING
//
// tests\fixture_bsp.h holds the same little map twice -- once as BSP v20 with
// nothing compressed, once as v25 with all seven collision lumps
// LZMA-compressed. v25 disagrees with v20 about the size of four of the seven
// structs, so reading both and requiring the counts to match is a check on the
// stride table that no amount of testing v20 could give.
//
// Build:  tests\build.bat
// Run:    tests\test_bsp.exe

#include "wr_bsp.h"

#include "fixture_bsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// ---------------------------------------------------------------------------
// A scratch file, because the thing under test opens one
// ---------------------------------------------------------------------------
//
// The same choice tests\test_peek.cpp makes and for the same reason: what is
// being checked is a claim about a FILE, so a stub handing back bytes we
// invented would be a test of the stub. Written into tests\bspscratch\ and
// deleted at the end.

#define SCRATCH "tests\\bspscratch"

static const char *WriteScratch(const char *name, const unsigned char *data,
                                size_t len)
{
    static char path[512];
    CreateDirectoryA(SCRATCH, NULL);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", SCRATCH, name);

    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return NULL;
    if (len)
        fwrite(data, 1, len, f);
    fclose(f);
    return path;
}

// A mutable copy of one of the fixtures, plus the accessors for poking at its
// lump directory. Offsets are the format's: the directory starts at 8 and each
// entry is fileofs, filelen, version, fourCC.
struct Bsp
{
    unsigned char *b;
    size_t len;
};

static Bsp Copy(const unsigned char *src, size_t len)
{
    Bsp m;
    m.b = (unsigned char *)malloc(len);
    m.len = len;
    memcpy(m.b, src, len);
    return m;
}

static void Drop(Bsp *m) { free(m->b); m->b = NULL; }

static unsigned char *Entry(Bsp *m, int lumpIndex)
{
    return m->b + 8 + lumpIndex * 16;
}

static void Put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned int Get32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

// The seven lumps' slots in the file's own 64-entry directory. Spelled out
// here rather than taken from WrBspLumpIndex so that a typo in the reader's
// table is a disagreement rather than a shared mistake.
#define IX_PLANES      1
#define IX_NODES       5
#define IX_LEAFS      10
#define IX_MODELS     14
#define IX_LEAFBRUSH  17
#define IX_BRUSHES    18
#define IX_BRUSHSIDES 19

// Read a mutated fixture and return the refusal it produced. "" means it was
// accepted, which for most of the cases below is itself the failure.
static const char *ReadBack(Bsp *m, const char *name, WrBspRaw *out)
{
    static char err[256];
    err[0] = '\0';
    WrBspRaw scratch;
    WrBspRaw *dst = out ? out : &scratch;
    const char *path = WriteScratch(name, m->b, m->len);
    if (!path)
        return "could not write the scratch file";
    if (WrBspReadRaw(path, dst, err, (int)sizeof(err)))
    {
        if (!out)
            WrBspFreeRaw(dst);
        return "";
    }
    return err;
}

static void Refuses(const unsigned char *src, size_t len,
                    void (*mutate)(Bsp *), const char *expect, const char *what)
{
    Bsp m = Copy(src, len);
    mutate(&m);
    const char *got = ReadBack(&m, "case.bsp", NULL);
    bool ok = strcmp(got, expect) == 0;
    Check(ok, what);
    if (!ok)
        printf("      expected: %s\n      got     : %s\n", expect,
               got[0] ? got : "(accepted)");
    Drop(&m);
}

// ---------------------------------------------------------------------------
// The stride table on its own
// ---------------------------------------------------------------------------
//
// Every row below was measured across the 1,304 maps in momentum\maps. The
// combinations NOT listed matter just as much: the table has no default, so a
// lump version nobody has seen is a named refusal rather than a guess, and
// these assert that it stays that way.

static void TestStrideTable(void)
{
    printf("stride table\n");

    struct Row { int lump, bsp, lumpver, stride; const char *why; };
    static const Row rows[] =
    {
        { WR_BSP_L_PLANES,      19, 0, 20, "PLANES v19" },
        { WR_BSP_L_PLANES,      20, 0, 20, "PLANES v20" },
        { WR_BSP_L_PLANES,      25, 0, 20, "PLANES v25" },
        { WR_BSP_L_NODES,       20, 0, 32, "NODES v20 lump 0" },
        { WR_BSP_L_NODES,       25, 1, 48, "NODES v25 lump 1" },
        { WR_BSP_L_LEAFS,       19, 0, 56, "LEAFS v19 lump 0" },
        { WR_BSP_L_LEAFS,       20, 1, 32, "LEAFS v20 lump 1" },
        { WR_BSP_L_LEAFS,       21, 1, 32, "LEAFS v21 lump 1" },
        { WR_BSP_L_LEAFS,       25, 2, 56, "LEAFS v25 lump 2" },
        { WR_BSP_L_MODELS,      25, 0, 48, "MODELS v25" },
        { WR_BSP_L_LEAFBRUSHES, 20, 0,  2, "LEAFBRUSHES v20" },
        { WR_BSP_L_LEAFBRUSHES, 25, 1,  4, "LEAFBRUSHES v25" },
        { WR_BSP_L_BRUSHES,     25, 0, 12, "BRUSHES v25" },
        { WR_BSP_L_BRUSHSIDES,  20, 0,  8, "BRUSHSIDES v20" },
        { WR_BSP_L_BRUSHSIDES,  25, 1, 16, "BRUSHSIDES v25" },
    };

    bool all = true;
    for (int i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++)
        if (WrBspStride(rows[i].lump, rows[i].bsp, rows[i].lumpver)
            != rows[i].stride)
        {
            printf("      %s: got %d, wanted %d\n", rows[i].why,
                   WrBspStride(rows[i].lump, rows[i].bsp, rows[i].lumpver),
                   rows[i].stride);
            all = false;
        }
    Check(all, "every measured (lump, BSP version, lump version) row");

    // The LEAFS struct is the one that moves BOTH ways -- 56 bytes with the
    // ambient light cube inline, 32 after it moved out, and 56 again in
    // Strata's. A reader keyed on "is this Strata" reads v25 at 32 bytes and
    // walks a tree made of nothing.
    Check(WrBspStride(WR_BSP_L_LEAFS, 25, 2) == 56 &&
          WrBspStride(WR_BSP_L_LEAFS, 20, 1) == 32,
          "LEAFS grows back to 56 on v25 rather than staying at 32");

    // v25's widths on an older BSP, and an older BSP's on v25. Both are
    // combinations that have never been seen, so both are refusals.
    Check(WrBspStride(WR_BSP_L_NODES, 20, 1) == 0,
          "NODES lump version 1 is refused on BSP 20");
    Check(WrBspStride(WR_BSP_L_LEAFS, 20, 2) == 0,
          "LEAFS lump version 2 is refused on BSP 20");
    Check(WrBspStride(WR_BSP_L_BRUSHSIDES, 25, 0) == 0,
          "BRUSHSIDES lump version 0 is refused on BSP 25");
    Check(WrBspStride(WR_BSP_L_LEAFBRUSHES, 21, 1) == 0,
          "LEAFBRUSHES lump version 1 is refused on BSP 21");

    bool none = true;
    for (int l = 0; l < WR_BSP_L_COUNT; l++)
        for (int v = 3; v < 40; v++)
            if (WrBspStride(l, 20, v) != 0 || WrBspStride(l, 25, v) != 0)
                none = false;
    Check(none, "no lump version above 2 resolves to a stride on any BSP");
}

// ---------------------------------------------------------------------------
// The two happy paths
// ---------------------------------------------------------------------------

static void Describe(const WrBspRaw *r, const char *tag)
{
    printf("      %s: v%d rev %d, %s, %u bytes -- "
           "%d planes, %d nodes, %d leafs, %d models, %d leafbrushes, "
           "%d brushes, %d brushsides\n",
           tag, r->version, r->revision,
           r->compressed ? "LZMA" : "raw", r->totalBytes,
           r->count[WR_BSP_L_PLANES], r->count[WR_BSP_L_NODES],
           r->count[WR_BSP_L_LEAFS], r->count[WR_BSP_L_MODELS],
           r->count[WR_BSP_L_LEAFBRUSHES], r->count[WR_BSP_L_BRUSHES],
           r->count[WR_BSP_L_BRUSHSIDES]);
}

static bool CountsMatchFixture(const WrBspRaw *r)
{
    return r->count[WR_BSP_L_PLANES]      == WR_FIXTURE_BSP_PLANES &&
           r->count[WR_BSP_L_NODES]       == WR_FIXTURE_BSP_NODES &&
           r->count[WR_BSP_L_LEAFS]       == WR_FIXTURE_BSP_LEAFS &&
           r->count[WR_BSP_L_MODELS]      == WR_FIXTURE_BSP_MODELS &&
           r->count[WR_BSP_L_LEAFBRUSHES] == 3 &&
           r->count[WR_BSP_L_BRUSHES]     == WR_FIXTURE_BSP_BRUSHES &&
           r->count[WR_BSP_L_BRUSHSIDES]  == WR_FIXTURE_BSP_BRUSHSIDES;
}

static void TestHappy(void)
{
    printf("\nthe fixture, both ways\n");

    Bsp a = Copy(kBspV20, sizeof(kBspV20));
    WrBspRaw v20;
    const char *e20 = ReadBack(&a, "v20.bsp", &v20);
    Check(e20[0] == '\0', "the v20 fixture is accepted");
    if (e20[0])
        printf("      %s\n", e20);

    Bsp b = Copy(kBspV25, sizeof(kBspV25));
    WrBspRaw v25;
    const char *e25 = ReadBack(&b, "v25.bsp", &v25);
    Check(e25[0] == '\0', "the v25 fixture is accepted");
    if (e25[0])
        printf("      %s\n", e25);

    if (!e20[0] && !e25[0])
    {
        Describe(&v20, "v20");
        Describe(&v25, "v25");

        Check(v20.version == 20 && !v20.compressed, "v20 reads as v20, raw");
        Check(v25.version == 25 && v25.compressed, "v25 reads as v25, LZMA");
        Check(v20.revision == 1 && v25.revision == 1, "the map revision is read");

        Check(CountsMatchFixture(&v20), "v20 counts are what the fixture wrote");
        Check(CountsMatchFixture(&v25), "v25 counts are what the fixture wrote");

        // THE CHECK THIS FIXTURE EXISTS FOR. Same world, two struct layouts.
        bool same = true;
        for (int i = 0; i < WR_BSP_L_COUNT; i++)
            if (v20.count[i] != v25.count[i])
                same = false;
        Check(same, "v20 and v25 agree on every count, at different strides");

        Check(v20.stride[WR_BSP_L_NODES] == 32 &&
              v25.stride[WR_BSP_L_NODES] == 48 &&
              v20.stride[WR_BSP_L_LEAFS] == 32 &&
              v25.stride[WR_BSP_L_LEAFS] == 56 &&
              v20.stride[WR_BSP_L_LEAFBRUSHES] == 2 &&
              v25.stride[WR_BSP_L_LEAFBRUSHES] == 4 &&
              v20.stride[WR_BSP_L_BRUSHSIDES] == 8 &&
              v25.stride[WR_BSP_L_BRUSHSIDES] == 16,
              "and they got there at the four strides that differ");

        // The lumps that did not change carry identical BYTES, decompressed.
        // Anything else means the LZMA arm is not producing what the raw arm
        // reads, which is a thing no count could show.
        bool bytesSame =
            v20.bytes[WR_BSP_L_PLANES] == v25.bytes[WR_BSP_L_PLANES] &&
            memcmp(v20.data[WR_BSP_L_PLANES], v25.data[WR_BSP_L_PLANES],
                   v20.bytes[WR_BSP_L_PLANES]) == 0 &&
            memcmp(v20.data[WR_BSP_L_BRUSHES], v25.data[WR_BSP_L_BRUSHES],
                   v20.bytes[WR_BSP_L_BRUSHES]) == 0 &&
            memcmp(v20.data[WR_BSP_L_MODELS], v25.data[WR_BSP_L_MODELS],
                   v20.bytes[WR_BSP_L_MODELS]) == 0;
        Check(bytesSame,
              "PLANES, BRUSHES and MODELS decompress to the raw file's bytes");

        // Spot-check the one plane every later stage depends on: the ramp.
        const float *p6 = (const float *)(v25.data[WR_BSP_L_PLANES] + 6 * 20);
        Check(p6[0] == 0.8f && p6[1] == 0.0f && p6[2] == WR_FIXTURE_BSP_SURF_NZ
              && p6[3] == 180.0f,
              "the ramp plane survives LZMA exactly: (0.8, 0, 0.6) d 180");

        WrBspFreeRaw(&v20);
        WrBspFreeRaw(&v25);
    }

    Drop(&a);
    Drop(&b);
}

// ---------------------------------------------------------------------------
// Every refusal
// ---------------------------------------------------------------------------

static void MutIdent(Bsp *m)   { Put32(m->b, 0x42424242u); }
static void MutVersion(Bsp *m) { Put32(m->b + 4, 18u); }

static void MutPlanesEmpty(Bsp *m)    { Put32(Entry(m, IX_PLANES) + 4, 0); }
static void MutPlanesNegOfs(Bsp *m)   { Put32(Entry(m, IX_PLANES), 0x80000000u); }
static void MutNodesPastEnd(Bsp *m)   { Put32(Entry(m, IX_NODES) + 4, 0x1000000u); }
static void MutLeafsVer7(Bsp *m)      { Put32(Entry(m, IX_LEAFS) + 8, 7); }
static void MutLeafsVer2(Bsp *m)      { Put32(Entry(m, IX_LEAFS) + 8, 2); }
static void MutNodesVer1(Bsp *m)      { Put32(Entry(m, IX_NODES) + 8, 1); }

static void MutBrushesRagged(Bsp *m)
{
    // 3 brushes at 12 bytes is 36; 32 is not a multiple of 12 and still fits
    // inside the file, so the tiling gate is what has to catch it.
    Put32(Entry(m, IX_BRUSHES) + 4, 32);
}

// The compressed arm. These all operate on the v25 fixture, whose lumps carry
// a seventeen-byte Valve container: "LZMA", the decompressed size, the
// compressed size, and five property bytes.
static void MutFourCcDisagrees(Bsp *m)
{
    Put32(Entry(m, IX_PLANES) + 12, 999u);
}

static void MutNotLzma(Bsp *m)
{
    unsigned int ofs = Get32(Entry(m, IX_PLANES));
    memcpy(m->b + ofs, "ZSTD", 4);
}

static void MutTooShortToBeCompressed(Bsp *m)
{
    Put32(Entry(m, IX_PLANES) + 4, 10);
}

static void MutStreamPastLump(Bsp *m)
{
    unsigned int ofs = Get32(Entry(m, IX_PLANES));
    Put32(m->b + ofs + 8, 0x10000u);        // the container's compressed size
}

static void MutOverLumpBudget(Bsp *m)
{
    // Both places that carry the decompressed size, so the agreement check
    // passes and the BUDGET is what refuses. Patching only one of them would
    // test the wrong gate.
    unsigned int ofs = Get32(Entry(m, IX_PLANES));
    Put32(Entry(m, IX_PLANES) + 12, 200u * 1024u * 1024u);
    Put32(m->b + ofs + 4, 200u * 1024u * 1024u);
}

// 1680 is lcm(20, 48, 56) -- PLANES', NODES' and LEAFS' strides on v25 -- and
// 62000 of them is 99.3 MB. The multiple matters: the tiling gate runs on each
// lump before the running total has anything to refuse, so a round 100 MB is
// caught as "not a multiple of 48" and the total budget is never reached. A
// size that tiles is the only way to put the total gate under test.
#define BIG_TILED (1680u * 62000u)

static void MutOverTotalBudget(Bsp *m)
{
    // Three lumps of 99.3 MB. Each is comfortably under the per-lump limit and
    // each tiles at its own stride, so only the running total can refuse this
    // -- which is the point: a total computed after the reads would have
    // allocated 198 MB before noticing.
    static const int ix[3] = { IX_PLANES, IX_NODES, IX_LEAFS };
    for (int i = 0; i < 3; i++)
    {
        unsigned int ofs = Get32(Entry(m, ix[i]));
        Put32(Entry(m, ix[i]) + 12, BIG_TILED);
        Put32(m->b + ofs + 4, BIG_TILED);
    }
}

static void MutCorruptStream(Bsp *m)
{
    unsigned int ofs = Get32(Entry(m, IX_PLANES));
    unsigned int len = Get32(Entry(m, IX_PLANES) + 4);
    for (unsigned int i = 17; i < len; i++)
        m->b[ofs + i] ^= 0xFF;
}

static void TestRefusals(void)
{
    printf("\nrefusing a file it cannot be sure of\n");

    const unsigned char *A = kBspV20;
    const size_t LA = sizeof(kBspV20);
    const unsigned char *B = kBspV25;
    const size_t LB = sizeof(kBspV25);

    // Not a file at all.
    {
        WrBspRaw r;
        char err[128] = { 0 };
        bool got = WrBspReadRaw(SCRATCH "\\nothing-here.bsp", &r, err,
                                (int)sizeof(err));
        Check(!got && strcmp(err, "could not open the map file") == 0,
              "a missing map file");
        got = WrBspReadRaw("", &r, err, (int)sizeof(err));
        Check(!got && strcmp(err, "no map file named") == 0,
              "an empty path");
    }

    // Too short to hold a directory. Every real header is 1036 bytes.
    {
        Bsp m = Copy(A, 900);
        const char *got = ReadBack(&m, "short.bsp", NULL);
        Check(strcmp(got, "the BSP header is truncated") == 0,
              "a file shorter than the lump directory");
        Drop(&m);
    }

    Refuses(A, LA, MutIdent, "not a VBSP file", "the wrong magic");
    Refuses(A, LA, MutVersion, "unsupported BSP version 18",
            "a BSP version outside 19, 20, 21, 25");

    Refuses(A, LA, MutPlanesEmpty, "PLANES lump is empty",
            "a required lump with nothing in it");
    Refuses(A, LA, MutPlanesNegOfs,
            "PLANES lump has a negative offset or length",
            "a negative lump offset");
    Refuses(A, LA, MutNodesPastEnd, "NODES lump runs past the end of the file",
            "a lump that claims more bytes than the file has");

    Refuses(A, LA, MutLeafsVer7,
            "LEAFS lump version 7 is not one this reads on BSP 20",
            "a lump version nobody has ever seen");
    Refuses(A, LA, MutLeafsVer2,
            "LEAFS lump version 2 is not one this reads on BSP 20",
            "a REAL lump version, on the wrong BSP version");
    Refuses(A, LA, MutNodesVer1,
            "NODES lump version 1 is not one this reads on BSP 20",
            "Strata's node width on a stock map");

    Refuses(A, LA, MutBrushesRagged,
            "BRUSHES lump is 32 bytes, not a multiple of 12",
            "a lump length that does not tile at its stride");

    printf("\n  the compressed arm\n");

    Refuses(B, LB, MutTooShortToBeCompressed,
            "PLANES lump is compressed but too short to be",
            "a compressed lump with no room for its container");
    Refuses(B, LB, MutNotLzma,
            "PLANES lump is compressed with something that is not LZMA",
            "a codec that is not the one Valve uses");
    Refuses(B, LB, MutFourCcDisagrees,
            "PLANES lump: the directory says 999 bytes and the container says 340",
            "the directory and the container disagreeing about the size");
    Refuses(B, LB, MutStreamPastLump,
            "PLANES lump's LZMA stream runs past the lump",
            "a stream longer than the lump holding it");
    Refuses(B, LB, MutOverLumpBudget,
            "PLANES lump is 209715200 bytes, over the 134217728 byte limit",
            "one lump over the per-lump budget");
    Refuses(B, LB, MutOverTotalBudget,
            "the collision lumps total 312480000 bytes, over the 268435456 byte limit",
            "three individually legal lumps over the total budget");

    // The decoder's own message, prefixed with the lump. The wording after the
    // colon is liblzma's and belongs to wr_mtv.cpp; what matters here is that
    // the failure names WHICH lump, because seven of them go through one
    // function and "Corrupt input data" on its own says nothing.
    {
        Bsp m = Copy(B, LB);
        MutCorruptStream(&m);
        const char *got = ReadBack(&m, "corrupt.bsp", NULL);
        bool ok = strncmp(got, "PLANES lump: ", 13) == 0;
        Check(ok, "a corrupt stream names the lump it was decoding");
        if (!ok)
            printf("      got: %s\n", got[0] ? got : "(accepted)");
        Drop(&m);
    }
}

// ---------------------------------------------------------------------------
// The fields, and the walk
// ---------------------------------------------------------------------------

// Where a lump's bytes are in the file image. Only useful on the raw fixture:
// the v25 one is compressed, so its lump contents cannot be poked at in place.
static unsigned char *LumpBytes(Bsp *m, int lumpIndex)
{
    return m->b + Get32(Entry(m, lumpIndex));
}

static void TestFields(void)
{
    printf("\nreading a field out of somebody else's struct\n");

    Bsp a = Copy(kBspV20, sizeof(kBspV20));
    Bsp b = Copy(kBspV25, sizeof(kBspV25));
    WrBspRaw v20, v25;
    if (ReadBack(&a, "v20.bsp", &v20)[0] || ReadBack(&b, "v25.bsp", &v25)[0])
    {
        Check(false, "the fixtures load");
        Drop(&a); Drop(&b);
        return;
    }

    const WrBspRaw *both[2] = { &v20, &v25 };
    const char *tag[2] = { "v20", "v25" };

    for (int w = 0; w < 2; w++)
    {
        const WrBspRaw *r = both[w];
        char what[128];

        // The plane the whole feature is about, read through the accessor.
        float p[4] = { 0, 0, 0, 0 };
        bool got = WrBspPlane(r, 6, p);
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: plane 6 is the ramp, (0.8, 0, 0.6) at 180", tag[w]);
        Check(got && p[0] == 0.8f && p[1] == 0.0f && p[2] == 0.6f
              && p[3] == 180.0f, what);

        // Plane 16 is (0,0,0) and real maps have hundreds of these. It is read
        // back rather than refused -- what refuses it is WrBspStartQuad, at
        // the point where a normal has to be a direction.
        float z[4] = { 1, 1, 1, 1 };
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: the degenerate plane reads back as (0,0,0) d 0", tag[w]);
        Check(WrBspPlane(r, 16, z) && z[0] == 0 && z[1] == 0 && z[2] == 0
              && z[3] == 0, what);

        // Every brushside's plane index, at u16 on stock and int32 on v25.
        static const int expect[16] = { 0, 1, 2, 3, 4, 5,
                                        6, 7, 8, 9, 10,
                                        11, 12, 13, 14, 15 };
        bool sides = true;
        for (int i = 0; i < 16; i++)
            if (WrBspBrushSidePlane(r, i) != expect[i])
                sides = false;
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: all 16 brushside plane indices", tag[w]);
        Check(sides, what);

        // The three brushes, and the fact that matters most about them.
        int fs = 0, ns = 0, ct = 0;
        bool brushes = WrBspBrush(r, 0, &fs, &ns, &ct) && fs == 0 && ns == 6;
        int ct1 = 0, ct2 = 0;
        brushes = brushes && WrBspBrush(r, 1, NULL, NULL, &ct1);
        brushes = brushes && WrBspBrush(r, 2, NULL, NULL, &ct2);
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: brush 0 is sides 0..6", tag[w]);
        Check(brushes && fs == 0 && ns == 6, what);
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: the trigger's contents are identical to the ramp's (%d)",
                    tag[w], ct2);
        Check(ct == ct1 && ct1 == ct2 && ct2 == 1, what);

        // Node children, leaf ranges, model head nodes.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: node 0's children are leaves -1 and -2", tag[w]);
        Check(WrBspNodeChild(r, 0, 0) == -1 && WrBspNodeChild(r, 0, 1) == -2,
              what);

        int first = -1, num = -1;
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: leaf 0 owns leafbrushes 0..2", tag[w]);
        Check(WrBspLeafBrushRange(r, 0, &first, &num) && first == 0 && num == 2,
              what);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: model 0 roots at node 0 and model 1 at node 1", tag[w]);
        Check(WrBspModelHeadNode(r, 0) == 0 && WrBspModelHeadNode(r, 1) == 1,
              what);

        // Out of range in both directions, on every accessor that takes an
        // index. These come out of somebody else's file and there is no value
        // a real one could not hold.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: every accessor refuses an index off either end", tag[w]);
        float junk[4];
        Check(!WrBspPlane(r, -1, junk) && !WrBspPlane(r, 17, junk) &&
              WrBspBrushSidePlane(r, 16) == -1 &&
              WrBspBrushSidePlane(r, -1) == -1 &&
              WrBspLeafBrush(r, 3) == -1 &&
              !WrBspBrush(r, 3, NULL, NULL, NULL) &&
              !WrBspLeafBrushRange(r, 4, &first, &num) &&
              WrBspModelHeadNode(r, 2) == -1 &&
              WrBspNodeChild(r, 2, 0) == 0, what);
    }

    WrBspFreeRaw(&v20);
    WrBspFreeRaw(&v25);
    Drop(&a);
    Drop(&b);
}

static void TestWalk(void)
{
    printf("\nthe worldspawn walk\n");

    Bsp a = Copy(kBspV20, sizeof(kBspV20));
    Bsp b = Copy(kBspV25, sizeof(kBspV25));
    WrBspRaw v20, v25;
    if (ReadBack(&a, "v20.bsp", &v20)[0] || ReadBack(&b, "v25.bsp", &v25)[0])
    {
        Check(false, "the fixtures load");
        Drop(&a); Drop(&b);
        return;
    }

    // THE CHECK THE FIXTURE WAS BUILT FOR. Brush 2 is a slanted box with the
    // same CONTENTS_SOLID as the ramp, at the same angle, and owned by model 1
    // rather than by the world. Nothing but the tree can tell them apart.
    for (int w = 0; w < 2; w++)
    {
        const WrBspRaw *r = w ? &v25 : &v20;
        const char *tag = w ? "v25" : "v20";
        unsigned char owned[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
        int n = -1;
        char err[128] = { 0 };
        bool got = WrBspWorldBrushes(r, owned, &n, err, (int)sizeof(err));

        char what[128];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: the walk completes", tag);
        Check(got, what);
        if (!got)
            printf("      %s\n", err);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: the world owns the floor and the ramp, and not the "
                    "trigger", tag);
        Check(n == 2 && owned[0] && owned[1] && !owned[2], what);
    }

    // Brush 2 IS reachable -- from model 1's head node, which is exactly what
    // makes it a trigger rather than a stray. If this failed, the test above
    // would be passing because the brush is unreferenced rather than because
    // the walk excluded it.
    {
        int first = 0, num = 0;
        bool leaf2 = WrBspLeafBrushRange(&v20, 2, &first, &num);
        Check(leaf2 && num == 1 && WrBspLeafBrush(&v20, first) == 2,
              "and the trigger is excluded despite being reachable from model 1");
    }

    WrBspFreeRaw(&v20);
    WrBspFreeRaw(&v25);
    Drop(&a);
    Drop(&b);
}

// Every index the walk follows, pushed out of range. All of these are on the
// v20 fixture, whose lumps are uncompressed and so can be poked at in place.
static void WalkRefuses(void (*mutate)(Bsp *), const char *expect,
                        const char *what)
{
    Bsp m = Copy(kBspV20, sizeof(kBspV20));
    mutate(&m);

    WrBspRaw r;
    const char *e = ReadBack(&m, "walk.bsp", &r);
    if (e[0])
    {
        printf("  %-64s FAILED\n", what);
        printf("      the file itself was refused: %s\n", e);
        g_failures++;
        Drop(&m);
        return;
    }

    unsigned char owned[8];
    char err[192] = { 0 };
    bool got = WrBspWorldBrushes(&r, owned, NULL, err, (int)sizeof(err));
    bool ok = !got && strcmp(err, expect) == 0;
    Check(ok, what);
    if (!ok)
        printf("      expected: %s\n      got     : %s\n", expect,
               got ? "(accepted)" : err);

    // Nothing survives a refusal here either: a half-marked bitmap read as
    // "the world owns these" would be worse than no bitmap at all.
    if (!got)
    {
        bool clean = true;
        for (int i = 0; i < WR_FIXTURE_BSP_BRUSHES; i++)
            if (owned[i])
                clean = false;
        Check(clean, "      and the bitmap comes back empty");
    }

    WrBspFreeRaw(&r);
    Drop(&m);
}

static void MutHeadNode(Bsp *m)
{
    // model 0's headnode, 36 bytes into a 48-byte dmodel_t
    Put32(LumpBytes(m, IX_MODELS) + 36, 999);
}

static void MutNodeChildNode(Bsp *m)
{
    Put32(LumpBytes(m, IX_NODES) + 4, 999);
}

static void MutNodeChildLeaf(Bsp *m)
{
    Put32(LumpBytes(m, IX_NODES) + 8, (unsigned int)(-999));
}

static void MutLeafBrushRange(Bsp *m)
{
    // leaf 0's numleafbrushes, a u16 at offset 26 of a 32-byte dleaf_t
    unsigned char *p = LumpBytes(m, IX_LEAFS) + 26;
    p[0] = 99; p[1] = 0;
}

static void MutLeafBrushTarget(Bsp *m)
{
    unsigned char *p = LumpBytes(m, IX_LEAFBRUSH);
    p[0] = 99; p[1] = 0;
}

static void MutBrushSides(Bsp *m)
{
    Put32(LumpBytes(m, IX_BRUSHES) + 4, 99);
}

static void TestWalkRefusals(void)
{
    printf("\nevery index the walk follows, pushed off the end\n");

    WalkRefuses(MutHeadNode, "model 0's head node is 999 of 2",
                "a model whose head node is not a node");
    WalkRefuses(MutNodeChildNode, "node 0's child 0 is node 999 of 2",
                "a node child pointing past the node lump");
    WalkRefuses(MutNodeChildLeaf, "node 0's child 1 is leaf 998 of 4",
                "a node child pointing past the leaf lump");
    WalkRefuses(MutLeafBrushRange, "leaf 0 claims leafbrushes 0..99 of 3",
                "a leaf whose brush range leaves the lump");
    WalkRefuses(MutLeafBrushTarget, "leafbrush 0 references brush 99 of 3",
                "a leafbrush pointing at no brush");
    WalkRefuses(MutBrushSides, "brush 0 claims sides 0..99 of 16",
                "a brush whose side range leaves the lump");
}

// ---------------------------------------------------------------------------
// Nothing is left behind on a refusal
// ---------------------------------------------------------------------------
//
// WrBspReadRaw promises it never partially succeeds. That is not decoration:
// the caller will be a worker thread whose result is published under a lock,
// and a struct holding four live pointers and three nulls is exactly the shape
// that gets published anyway by a caller checking the wrong field.

static void TestNoPartialSuccess(void)
{
    printf("\nfailing cleanly\n");

    // BRUSHSIDES is the LAST of the seven, so a failure there happens after
    // six lumps have already been read and allocated. If anything survives a
    // refusal, it survives this one.
    Bsp m = Copy(kBspV20, sizeof(kBspV20));
    Put32(Entry(&m, IX_BRUSHSIDES) + 8, 9);

    WrBspRaw r;
    memset(&r, 0xCD, sizeof(r));
    char err[128] = { 0 };
    const char *path = WriteScratch("last.bsp", m.b, m.len);
    bool got = WrBspReadRaw(path, &r, err, (int)sizeof(err));

    Check(!got, "a refusal on the last of the seven lumps");
    bool clean = true;
    for (int i = 0; i < WR_BSP_L_COUNT; i++)
        if (r.data[i] != NULL || r.bytes[i] != 0 || r.count[i] != 0)
            clean = false;
    Check(clean, "and the output struct comes back zeroed, not half-filled");
    Check(r.version == 0 && r.totalBytes == 0,
          "including the fields that were valid before the failure");

    Drop(&m);
}

// ---------------------------------------------------------------------------

int main(void)
{
    printf("test_bsp\n\n");

    TestStrideTable();
    TestHappy();
    TestRefusals();
    TestFields();
    TestWalk();
    TestWalkRefusals();
    TestNoPartialSuccess();

    // Tidy up. Leaving these behind would be harmless and would also mean a
    // later run could read a stale one if a write ever failed silently.
    static const char *names[] = { "case.bsp", "v20.bsp", "v25.bsp",
                                   "short.bsp", "corrupt.bsp", "last.bsp",
                                   "walk.bsp" };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    {
        char p[512];
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", SCRATCH, names[i]);
        DeleteFileA(p);
    }
    RemoveDirectoryA(SCRATCH);

    printf("\n%s\n", g_failures ? "FAILED" : "all good");
    return g_failures ? 1 : 0;
}
