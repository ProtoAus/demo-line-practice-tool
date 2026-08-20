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
    19      // LUMP_BRUSHSIDES
};

const char *WrBspLumpName[WR_BSP_L_COUNT] =
{
    "PLANES", "NODES", "LEAFS", "MODELS", "LEAFBRUSHES", "BRUSHES", "BRUSHSIDES"
};

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

    bool ok = true;
    for (int i = 0; i < WR_BSP_L_COUNT && ok; i++)
    {
        const char *nm = WrBspLumpName[i];

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
