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
#include <math.h>

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
        // The displacement half. 72 and 232 were measured off the corpus as
        // gcds long before the layouts were known; what was missing was where
        // the fields inside them sit, which no length can tell you. See the
        // FACES row of wr_bsp.cpp's table.
        { WR_BSP_L_VERTEXES,    25, 0, 12, "VERTEXES v25" },
        { WR_BSP_L_EDGES,       20, 0,  4, "EDGES v20" },
        { WR_BSP_L_EDGES,       25, 1,  8, "EDGES v25" },
        { WR_BSP_L_SURFEDGES,   25, 0,  4, "SURFEDGES v25" },
        { WR_BSP_L_FACES,       20, 1, 56, "FACES v20 lump 1" },
        { WR_BSP_L_FACES,       25, 2, 72, "FACES v25 lump 2" },
        { WR_BSP_L_DISPINFO,    20, 0, 176, "DISPINFO v20 lump 0" },
        { WR_BSP_L_DISPINFO,    25, 1, 232, "DISPINFO v25 lump 1" },
        { WR_BSP_L_DISPVERTS,   25, 0, 20, "DISPVERTS v25" },
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

    // The displacement half of the same rule, and it is worth its own line
    // because these two are the rows most recently added and the ones a future
    // Strata bump will move again. Four v25 maps in the library carry DISPINFO
    // lump version 0, all with a zero-length lump; a NON-empty one would be a
    // layout nobody here has seen, and it has to stay a refusal.
    Check(WrBspStride(WR_BSP_L_DISPINFO, 25, 0) == 0,
          "DISPINFO lump version 0 is refused on BSP 25");
    Check(WrBspStride(WR_BSP_L_FACES, 25, 1) == 0,
          "FACES lump version 1 is refused on BSP 25");
    Check(WrBspStride(WR_BSP_L_FACES, 20, 2) == 0,
          "FACES lump version 2 is refused on BSP 20");
    Check(WrBspStride(WR_BSP_L_EDGES, 20, 1) == 0,
          "EDGES lump version 1 is refused on BSP 20");

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
// The clip pipeline, end to end
// ---------------------------------------------------------------------------
//
// tests\test_bspgeom.exe already drives the clipper against shapes checked by
// hand. What is new here is the wiring: real lumps, the worldspawn filter, the
// contents filter, and the arithmetic that turns a run of sides into polygons.
// Every number below can be worked out on paper from the fixture's own
// comment, which is the only reason they are worth asserting.
//
//     the floor slab   1024 x 1024 x 64, so 2*1024^2 + 4*1024*64 = 2359296
//     the ramp prism   bottom 225*512 = 115200, back 512*300 = 153600,
//                      slant 375*512 = 192000, two ends 2*(225*300/2) = 67500
//                                                          total  528300
//     everything       2887596, of which 192000 is in the surf band

static void TestBuild(void)
{
    printf("\nbrushes into polygons\n");

    Bsp a = Copy(kBspV20, sizeof(kBspV20));
    Bsp b = Copy(kBspV25, sizeof(kBspV25));
    WrBspRaw v20, v25;
    if (ReadBack(&a, "v20.bsp", &v20)[0] || ReadBack(&b, "v25.bsp", &v25)[0])
    {
        Check(false, "the fixtures load");
        Drop(&a); Drop(&b);
        return;
    }

    WrBspMap map[2];
    bool built = true;
    for (int w = 0; w < 2; w++)
    {
        char err[192] = { 0 };
        if (!WrBspBuild(w ? &v25 : &v20, &map[w], err, (int)sizeof(err)))
        {
            printf("      %s\n", err);
            built = false;
        }
    }
    Check(built, "both fixtures build");
    if (!built)
    {
        WrBspFreeRaw(&v20); WrBspFreeRaw(&v25);
        Drop(&a); Drop(&b);
        return;
    }

    for (int w = 0; w < 2; w++)
    {
        const WrBspMap *m = &map[w];
        const char *tag = w ? "v25" : "v20";
        char what[160];

        printf("      %s: %d polys, %d verts, %.0f sq units solid, "
               "%.0f in band, %u bytes\n", tag, m->polyCount, m->vertCount,
               m->solidArea, m->surfArea, (unsigned int)m->bytes);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: 2 of 3 brushes are the world's, both solid", tag);
        Check(m->brushTotal == 3 && m->brushWorld == 2 && m->brushSolid == 2,
              what);

        // Six faces off the slab and five off the prism. Not "about eleven":
        // a clipper that lost a face or kept a bevel would land somewhere
        // else, and there is nowhere else to land.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: 11 faces -- six off the slab, five off the prism", tag);
        Check(m->polyCount == 11, what);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: nothing failed closure, nothing left the world", tag);
        Check(m->sideNotClosed == 0 && m->sideTooFar == 0 &&
              m->sideDegenerate == 0, what);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: total area is 2887596 to a part in ten thousand", tag);
        Check(fabs(m->solidArea - 2887596.0) < 289.0, what);

        // ONE surf-band face, and its area is the slant's exactly. Two would
        // mean the trigger got in; nought would mean the band test folded the
        // sign and threw the ramp out with the ceilings.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: exactly one face in the surf band", tag);
        Check(m->surfPolys == 1, what);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: and its area is 192000, not 384000", tag);
        Check(fabs(m->surfArea - WR_FIXTURE_BSP_SURF_AREA) < 20.0f, what);

        // Find it and check the angle a user would be shown.
        int found = -1;
        for (int i = 0; i < m->polyCount; i++)
            if (WrBspIsSurfBand(m->polys[i].plane[2]))
                found = i;
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: it reads 53.13 degrees", tag);
        Check(found >= 0 &&
              fabs(WrBspSurfaceAngle(m->polys[found].plane) - 53.130102) < 0.001,
              what);

        // The winding is the plane's. Get this wrong and every polygon in the
        // map is inside out, which still draws and silently inverts the
        // front-face test that keeps a ramp's underside off the screen.
        float wound[3] = { 0, 0, 0 };
        bool haveNormal = found >= 0 &&
            WrBspPolyNormal(WrBspPolyVerts(m, found), m->polys[found].count,
                            wound);
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: its winding agrees with its own plane", tag);
        Check(haveNormal && WrBspAngleBetween(wound, m->polys[found].plane)
              < 0.01f, what);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: the bounds are the slab's, raised to the prism's top",
                    tag);
        Check(m->mins[0] == -512.0f && m->maxs[0] == 512.0f &&
              m->mins[1] == -512.0f && m->maxs[1] == 512.0f &&
              m->mins[2] == -64.0f && m->maxs[2] == 300.0f, what);
    }

    // AND THE TWO AGREE. Different strides, different field offsets, one of
    // them through LZMA -- and the same polygons out the far end.
    bool identical = map[0].polyCount == map[1].polyCount &&
                     map[0].vertCount == map[1].vertCount &&
                     map[0].surfPolys == map[1].surfPolys;
    if (identical)
        for (int i = 0; i < map[0].vertCount && identical; i++)
            for (int k = 0; k < 3; k++)
                if (map[0].verts[i][k] != map[1].verts[i][k])
                    identical = false;
    Check(identical, "v20 and v25 produce the same vertices, to the last bit");

    WrBspFreeMap(&map[0]);
    WrBspFreeMap(&map[1]);
    WrBspFreeRaw(&v20);
    WrBspFreeRaw(&v25);
    Drop(&a);
    Drop(&b);
}

// ---------------------------------------------------------------------------
// The displacement, read through both dface_t layouts
// ---------------------------------------------------------------------------
//
// The gap this closes was a real one and it was open for a long time: v25's
// FACES lump was refused outright, so 27 maps in this library read none of their
// displacements. The stride was never the missing piece -- 72 had been measured
// off the corpus as a gcd all along -- it was where firstedge and numedges went
// inside those 72 bytes, and a length cannot answer that.
//
// Which is exactly why this test is a COMPARISON and not a set of magic numbers.
// Reading firstedge at 4 on a v25 face gives a value that is in range, indexes a
// real surfedge and builds a real quad somewhere else; every self-consistency
// check in the file would pass. The only thing that catches it is the same
// surface written in the other layout disagreeing.
//
// The numbers are exact rather than approximate because the fixture was chosen
// that way: a flat quad, 250 by 512, every vertex pushed 100 along the quad's own
// normal so the surface translates instead of deforming. See
// tests\make_bsp_fixture.py.

static void TestDisplacementLayouts(void)
{
    printf("\nthe same displacement, in both dface_t layouts\n");

    Bsp a = Copy(kBspDispV20, sizeof(kBspDispV20));
    Bsp b = Copy(kBspDispV25, sizeof(kBspDispV25));
    WrBspRaw v20, v25;
    if (ReadBack(&a, "d20.bsp", &v20)[0] || ReadBack(&b, "d25.bsp", &v25)[0])
    {
        Check(false, "the displacement fixtures load");
        Drop(&a); Drop(&b);
        return;
    }

    // The strides the table picked, before anything is built with them. A
    // fixture that landed on the wrong row would fail further down for reasons
    // that read like arithmetic, so say it here.
    Check(v20.stride[WR_BSP_L_FACES] == 56 &&
          v25.stride[WR_BSP_L_FACES] == 72,
          "FACES resolves to 56 on v20 and 72 on v25");
    Check(v20.stride[WR_BSP_L_DISPINFO] == 176 &&
          v25.stride[WR_BSP_L_DISPINFO] == 232,
          "DISPINFO resolves to 176 on v20 and 232 on v25");
    Check(v20.stride[WR_BSP_L_EDGES] == 4 && v25.stride[WR_BSP_L_EDGES] == 8,
          "EDGES resolves to 4 on v20 and 8 on v25");

    WrBspMap map[2];
    bool built = true;
    for (int w = 0; w < 2; w++)
    {
        char err[192] = { 0 };
        if (!WrBspBuild(w ? &v25 : &v20, &map[w], err, (int)sizeof(err)))
        {
            printf("      %s\n", err);
            built = false;
        }
    }
    Check(built, "both displacement fixtures build");
    if (!built)
    {
        WrBspFreeRaw(&v20); WrBspFreeRaw(&v25);
        Drop(&a); Drop(&b);
        return;
    }

    for (int w = 0; w < 2; w++)
    {
        const WrBspMap *m = &map[w];
        const char *tag = w ? "v25" : "v20";
        char what[176];

        printf("      %s: %d polys (%d from the displacement), %d verts, "
               "%.0f sq units in band\n", tag, m->polyCount, m->dispPolys,
               m->vertCount, m->surfArea);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: the map declares %d displacements", tag,
                    WR_FIXTURE_BSP_DISP_COUNT);
        Check(m->dispTotal == WR_FIXTURE_BSP_DISP_COUNT, what);

        // NOTHING DROPPED, and the corner test is why this line is worth
        // having. startPosition is a point, and the only way it lands on a
        // corner of the quad is if firstedge walked out to the right four
        // vertices -- which on v25 is the field that moved.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: and built it, refusing nothing", tag);
        Check(m->dispDropped == 0, what);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: %d triangles, two grids of 4x4x2", tag,
                    WR_FIXTURE_BSP_DISP_TRIS);
        Check(m->dispPolys == WR_FIXTURE_BSP_DISP_TRIS, what);

        // 11 brush faces plus the grids. A displacement that built at the wrong
        // power would land here rather than anywhere subtle.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: 11 brush faces and the grids on top", tag);
        Check(m->polyCount == 11 + WR_FIXTURE_BSP_DISP_TRIS, what);

        // THE LINE THE side FLAG PAYS FOR. Both grids are in the band only if
        // the back-side one was turned the right way out; ignore the flag and
        // this reads 192000 + 128000, with the second grid's 32 triangles built,
        // stored, and then discarded as ceilings.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: surf-band area is 192000 + 2 x 128000", tag);
        Check(fabs(m->surfArea - (WR_FIXTURE_BSP_SURF_AREA +
                                  WR_FIXTURE_BSP_DISP_AREA)) < 80.0f, what);

        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: and every triangle of both grids is a surf face", tag);
        Check(m->surfPolys == 1 + WR_FIXTURE_BSP_DISP_TRIS, what);

        // Every triangle is its quad's own plane. A push along the normal
        // translates the surface; it does not tilt it, so anything but +0.6
        // means the dispvert, the base quad or the side flag was read wrong --
        // and -0.6 specifically means the side flag.
        int up = 0, down = 0;
        for (int i = 0; i < m->polyCount; i++)
        {
            const float nz = m->polys[i].plane[2];
            if (fabs(nz - WR_FIXTURE_BSP_DISP_NZ) < 0.0005f)
                up++;
            else if (fabs(nz + WR_FIXTURE_BSP_DISP_NZ) < 0.0005f)
                down++;
        }
        // The prism's slant has the same normal by construction, so the count
        // is both grids plus one.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: every triangle reads n.z = +0.6, none inside out", tag);
        Check(up == WR_FIXTURE_BSP_DISP_TRIS + 1 && down == 0, what);

        // The surface is the quad pushed 100 along (0.8, 0, 0.6), so its
        // highest point is 500 + 60 and its lowest 300 + 60. Bounds are the
        // cheapest way to catch a grid built off the wrong corner.
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%s: the grid raised the world's top to 560", tag);
        Check(fabs(m->maxs[2] - 560.0f) < 0.01f, what);
    }

    // THE POINT OF THE PAIR. Different strides, different field offsets for
    // firstedge and numedges, one of them through LZMA -- and the same
    // triangles, to the bit.
    bool identical = map[0].polyCount == map[1].polyCount &&
                     map[0].vertCount == map[1].vertCount &&
                     map[0].dispPolys == map[1].dispPolys &&
                     map[0].surfPolys == map[1].surfPolys;
    if (identical)
        for (int i = 0; i < map[0].vertCount && identical; i++)
            for (int k = 0; k < 3; k++)
                if (map[0].verts[i][k] != map[1].verts[i][k])
                    identical = false;
    Check(identical,
          "both layouts produce the same displacement, to the last bit");

    WrBspFreeMap(&map[0]);
    WrBspFreeMap(&map[1]);
    WrBspFreeRaw(&v20);
    WrBspFreeRaw(&v25);
    Drop(&a);
    Drop(&b);
}

// The trigger, put back. This is the counterfactual the whole design rests on,
// so it is worth measuring rather than asserting: give brush 2 to model 0 and
// the surf-band area doubles.
static void TestTriggerCounterfactual(void)
{
    printf("\nwhat including the trigger would have cost\n");

    Bsp m = Copy(kBspV20, sizeof(kBspV20));

    // Leaf 0 owns leafbrushes 0..2. Widen it to 0..3 and the world now owns
    // the trigger too -- exactly the map a reader without the walk sees.
    unsigned char *leaf0 = LumpBytes(&m, IX_LEAFS);
    leaf0[26] = 3; leaf0[27] = 0;

    WrBspRaw r;
    const char *e = ReadBack(&m, "trig.bsp", &r);
    if (e[0])
    {
        Check(false, "the mutated fixture loads");
        Drop(&m);
        return;
    }

    WrBspMap map;
    char err[192] = { 0 };
    bool ok = WrBspBuild(&r, &map, err, (int)sizeof(err));
    Check(ok, "it still builds -- nothing about it is malformed");
    if (ok)
    {
        printf("      with the trigger: %d brushes, %d band faces, %.0f sq units\n",
               map.brushSolid, map.surfPolys, map.surfArea);
        Check(map.brushSolid == 3 && map.surfPolys == 2 &&
              fabs(map.surfArea - 2.0 * WR_FIXTURE_BSP_SURF_AREA) < 40.0,
              "and reports twice the surf-band area, at the same angle");
        WrBspFreeMap(&map);
    }

    WrBspFreeRaw(&r);
    Drop(&m);
}

// ---------------------------------------------------------------------------
// The grid, against the thing it accelerates
// ---------------------------------------------------------------------------
//
// WHY BRUTE FORCE IS IN THE HARNESS
//
// An index that is wrong does not report an error. A ray that should hit
// returns "nothing ahead", which is byte for byte what a ray that correctly
// finds nothing returns -- and the feature this is for is "show me the ramp
// before I reach it", where "nothing ahead" is a perfectly ordinary answer. So
// the only way to know the grid is right is to ask the same question without
// it and require the same answer, on enough rays that a missed cell has to
// show up.

static bool BruteRay(const WrBspMap *m, const float start[3],
                     const float dir[3], float maxDist, int *polyOut,
                     float *tOut)
{
    int best = -1;
    float bestT = maxDist;
    for (int i = 0; i < m->polyCount; i++)
    {
        float t;
        if (!WrBspRayPoly(start, dir, m->polys[i].plane,
                          WrBspPolyVerts(m, i), m->polys[i].count, &t))
            continue;
        if (t < bestT) { bestT = t; best = i; }
    }
    if (best < 0)
        return false;
    if (polyOut) *polyOut = best;
    if (tOut) *tOut = bestT;
    return true;
}

// A tiny deterministic generator. Not rand(): the run has to be identical on
// every machine, or "it passed here" means nothing.
static unsigned int g_seed = 12345u;
static float Rnd(float lo, float hi)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g_seed >> 8) & 0xFFFFFF) / (float)0x1000000;
}

static void TestGrid(void)
{
    printf("\nthe grid\n");

    Bsp a = Copy(kBspV20, sizeof(kBspV20));
    WrBspRaw raw;
    if (ReadBack(&a, "v20.bsp", &raw)[0])
    {
        Check(false, "the fixture loads");
        Drop(&a);
        return;
    }

    WrBspMap m;
    char err[192] = { 0 };
    if (!WrBspBuild(&raw, &m, err, (int)sizeof(err)))
    {
        Check(false, "the fixture builds");
        printf("      %s\n", err);
        WrBspFreeRaw(&raw);
        Drop(&a);
        return;
    }

    printf("      grid %dx%dx%d, cells of %.0f x %.0f x %.0f, %d entries\n",
           m.grid.dims[0], m.grid.dims[1], m.grid.dims[2],
           m.grid.cell[0], m.grid.cell[1], m.grid.cell[2], m.itemCount);

    Check(m.cellStart != NULL && m.itemCount >= m.polyCount,
          "every polygon is in at least one cell");

    // The buckets have to tile: cell c is cellStart[c]..cellStart[c+1], and if
    // the prefix sum is off by one anywhere the last cell reads past the end.
    {
        const int cells = WrBspGridCellCount(&m.grid);
        bool tiles = m.cellStart[0] == 0 && m.cellStart[cells] == m.itemCount;
        for (int c = 0; c < cells && tiles; c++)
            if (m.cellStart[c] > m.cellStart[c + 1])
                tiles = false;
        Check(tiles, "the buckets tile the item list exactly, start to end");
    }

    // A ray straight down onto the ramp. The slant is 0.8x + 0.6z = 180, so at
    // x = 112.5 it sits at z = (180 - 90) / 0.6 = 150. From z = 400 that is a
    // drop of 250, and there is nothing above it to get in the way.
    {
        const float start[3] = { 112.5f, 0.0f, 400.0f };
        const float down[3] = { 0.0f, 0.0f, -1.0f };
        int poly = -1;
        float t = 0.0f;
        bool hit = WrBspTraceRay(&m, start, down, 1000.0f, &poly, &t);
        Check(hit && fabs(t - 250.0) < 0.05, "a ray down onto the ramp, at 250");
        Check(hit && WrBspIsSurfBand(m.polys[poly].plane[2]),
              "and what it hit is the surf-band face, not the floor under it");
    }

    // The same ray from BELOW. The ramp's slant faces up, so from underneath
    // it is a back face and must not be reported -- otherwise a query made
    // from inside the world finds the ceiling of everything.
    {
        const float start[3] = { 112.5f, 0.0f, -32.0f };
        const float up[3] = { 0.0f, 0.0f, 1.0f };
        int poly = -1;
        bool hit = WrBspTraceRay(&m, start, up, 1000.0f, &poly, NULL);
        Check(!hit || !WrBspIsSurfBand(m.polys[poly].plane[2]),
              "from below, the ramp's underside is not reported as the ramp");
    }

    // Out over the edge of the slab, pointing away. Nothing to hit.
    {
        const float start[3] = { 900.0f, 900.0f, 500.0f };
        const float away[3] = { 0.7071f, 0.7071f, 0.0f };
        Check(!WrBspTraceRay(&m, start, away, 4000.0f, NULL, NULL),
              "a ray into empty space finds nothing");
    }

    // AND THE REAL CHECK. Two thousand rays from everywhere, in every
    // direction, both answers required to agree on the polygon AND the
    // distance. A cell the walk skips shows up here as a disagreement and
    // nowhere else.
    {
        int mismatch = 0, hits = 0;
        for (int i = 0; i < 2000; i++)
        {
            float s[3], d[3];
            s[0] = Rnd(-800.0f, 800.0f);
            s[1] = Rnd(-800.0f, 800.0f);
            s[2] = Rnd(-200.0f, 600.0f);
            d[0] = Rnd(-1.0f, 1.0f);
            d[1] = Rnd(-1.0f, 1.0f);
            d[2] = Rnd(-1.0f, 1.0f);
            float len = (float)sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (len < 1e-4f)
                continue;
            d[0] /= len; d[1] /= len; d[2] /= len;

            int pg = -1, pb = -1;
            float tg = 0.0f, tb = 0.0f;
            bool hg = WrBspTraceRay(&m, s, d, 4000.0f, &pg, &tg);
            bool hb = BruteRay(&m, s, d, 4000.0f, &pb, &tb);
            if (hb)
                hits++;
            if (hg != hb || (hb && (pg != pb || fabs(tg - tb) > 0.01)))
                mismatch++;
        }
        // A guard against the comparison being vacuous rather than a target.
        // The sampling box is much bigger than the two brushes in it, so about
        // a sixth of random rays finding something is what it should give --
        // what would be alarming is a handful.
        printf("      2000 rays, %d of them hit something\n", hits);
        Check(hits > 200, "the rays are actually hitting things");
        Check(mismatch == 0, "the grid agrees with brute force on all 2000");
    }

    // What is around. One surf-band face in the whole map, and it is found
    // once rather than once per cell it lies across.
    {
        const float on[3] = { 112.5f, 0.0f, 150.0f };
        int out[8];
        int n = WrBspSurfNear(&m, on, 64.0f, out, 8);
        Check(n == 1 && WrBspIsSurfBand(m.polys[out[0]].plane[2]),
              "the ramp is found once, not once per cell it crosses");

        const float far_[3] = { -400.0f, -400.0f, 200.0f };
        Check(WrBspSurfNear(&m, far_, 64.0f, out, 8) == 0,
              "and nothing is found where there is no ramp");
    }

    // What was touched. A player origin sits off the surface, so this has to
    // measure to the polygon rather than to its plane.
    {
        // Straight up 20 from a point on the slant. The plane is tilted, so
        // the perpendicular distance is 20 * n.z = 12, not 20.
        const float above[3] = { 112.5f, 0.0f, 150.0f + 20.0f };
        int poly = -1;
        float d = 0.0f;
        bool got = WrBspNearestFace(&m, above, 64.0f, &poly, &d);
        Check(got && fabs(d - 12.0) < 0.5,
              "a point 20 units above the ramp along z is 12 off the face");
        Check(got && WrBspIsSurfBand(m.polys[poly].plane[2]),
              "and the face it names is the ramp");

        // ...AND THE RAMP IS OFFERED SEPARATELY, because "nearest" and
        // "rideable" are not the same question.
        //
        // Reported as boards that sometimes produce no numbers, from landings
        // taken at the side of a ramp. The nearest polygon of any facing is the
        // right answer for "am I touching something" and it is what the 98.3%
        // was measured with, so it may not move -- but a board graded against
        // the wall the player landed beside is no board at all, and there was
        // no second candidate to fall back to. So the same walk now reports
        // both.
        int rp = -1;
        float rd = -1.0f;
        Check(WrBspNearestFaceEx(&m, above, 64.0f, 0, 0, &rp, &rd) &&
              rp == poly && fabs(rd - d) < 1e-4,
              "where the nearest IS the ramp, both answers are the same one");

        // Sweep for a point where they genuinely disagree, rather than guessing
        // one out of the fixture's coordinates. The sweep also counts them, so
        // the assertions below cannot quietly become vacuous if the fixture
        // changes shape.
        int diverged = 0, checked = 0;
        bool everWrong = false, alwaysBand = true, alwaysFurther = true;
        bool plainMoved = false;
        for (int ix = -40; ix <= 40 && !everWrong; ix++)
            for (int iz = -40; iz <= 40; iz++)
            {
                const float p[3] = { 90.0f + ix * 4.0f, 0.0f,
                                     150.0f + iz * 4.0f };
                int a1 = -1, r1 = -1;
                float d1 = 0.0f, dr1 = -1.0f;
                if (!WrBspNearestFaceEx(&m, p, 24.0f, &a1, &d1, &r1, &dr1))
                    continue;
                checked++;

                // The plain answer must be bit-identical to the old query.
                int a2 = -1;
                float d2 = 0.0f;
                if (!WrBspNearestFace(&m, p, 24.0f, &a2, &d2) ||
                    a2 != a1 || d2 != d1)
                    plainMoved = true;

                if (r1 < 0 || r1 == a1)
                    continue;
                diverged++;
                if (!WrBspIsRampPlane(m.polys[r1].plane[2]))
                    alwaysBand = false;
                if (dr1 < d1 - 1e-4f)
                    alwaysFurther = false;
                if (WrBspIsRampPlane(m.polys[a1].plane[2]))
                    everWrong = true;   // then the plain one WAS rideable
            }

        printf("      %d of %d sampled points had a nearer face that is not "
               "the ramp\n", diverged, checked);
        Check(!plainMoved,
              "the touch answer is untouched -- the same polygon, the same "
              "distance, everywhere");
        Check(diverged > 0,
              "the fixture really does put a non-ramp face nearer, somewhere");
        Check(alwaysBand,
              "and every ramp candidate offered is inside the ramp band");
        Check(alwaysFurther,
              "never nearer than the nearest -- it is a second choice, not a "
              "different measurement");
        Check(!everWrong,
              "they only differ where the nearest face was not rideable");
    }

    WrBspFreeMap(&m);
    WrBspFreeRaw(&raw);
    Drop(&a);
}

// ---------------------------------------------------------------------------
// Nothing is left behind on a refusal
// ---------------------------------------------------------------------------
//
// WrBspReadRaw promises it never partially succeeds. That is not decoration:
// the caller will be a worker thread whose result is published under a lock,
// and a struct holding four live pointers and three nulls is exactly the shape
// that gets published anyway by a caller checking the wrong field.

// The switch this reader's whole live behaviour turns on, which had no test at
// all until it moved somewhere a test could reach it.
//
// It decides whether the game may be told "there is no surface there". A false
// answer used to be read as something much larger -- dllmain stopped ASKING the
// map -- and on surf_kvas that took the ramp strafe ideal and the live boards
// away for a whole level over 4 unbuilt displacements out of 756. No fixture:
// WrBspMap is a plain struct and each clause is worth pinning on its own.
static void TestGeometryComplete(void)
{
    printf("\nmay absence be used as evidence\n");

    WrBspMap m;
    memset(&m, 0, sizeof(m));
    Check(!WrBspGeometryComplete(0), "no map at all is not complete");
    Check(!WrBspGeometryComplete(&m), "and neither is one with no polygons");

    // surf_kvas's real census, both sides of the fix.
    m.polyCount       = 87238;
    m.brushTotal      = 2741;
    m.brushWorld      = 2404;
    m.entBrushes      = 42;
    m.hasDisplacements = true;
    m.dispTotal       = 756;
    m.dispPolys       = 74784;
    m.dispDropped     = 0;
    Check(WrBspGeometryComplete(&m),
          "surf_kvas, every displacement built -- complete");

    m.dispDropped = 4;
    m.dispDropBy[WR_DISP_DROP_CORNER] = 4;
    m.dispPolys   = 74272;
    Check(!WrBspGeometryComplete(&m),
          "the same map missing 4 of 756 -- NOT complete, which is the veto");
    Check(WrBspDispWorstDrop(&m) == WR_DISP_DROP_CORNER,
          "and the cause is named rather than recovered arithmetically");

    // A v25 map: displacements declared, none built.
    m.dispDropped = 0;
    m.dispDropBy[WR_DISP_DROP_CORNER] = 0;
    m.dispPolys   = 0;
    Check(!WrBspGeometryComplete(&m),
          "a map whose displacements produced nothing is not complete");

    // Ownership, with entity brushes counted. bhop_slope_v2 gives the world 48
    // of 1,351 brushes and is complete only because the rest are func_*.
    memset(&m, 0, sizeof(m));
    m.polyCount  = 1000;
    m.brushTotal = 1161;
    m.brushWorld = 579;
    Check(!WrBspGeometryComplete(&m),
          "49.9% owned and no entity brushes -- thin, as surf_greensway is");

    m.brushTotal = 1351;
    m.brushWorld = 48;
    m.entBrushes = 1200;
    Check(WrBspGeometryComplete(&m),
          "3.6% world-owned but 92% once entities count -- not thin");
}

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
    TestBuild();
    TestDisplacementLayouts();
    TestTriggerCounterfactual();
    TestGrid();
    TestGeometryComplete();
    TestNoPartialSuccess();

    // Tidy up. Leaving these behind would be harmless and would also mean a
    // later run could read a stale one if a write ever failed silently.
    static const char *names[] = { "case.bsp", "v20.bsp", "v25.bsp",
                                   "short.bsp", "corrupt.bsp", "last.bsp",
                                   "walk.bsp", "trig.bsp",
                                   "d20.bsp", "d25.bsp" };
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
