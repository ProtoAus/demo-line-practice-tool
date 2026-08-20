// bsp_sweep.cpp  --  the BSP reader, run over every map on the machine.
//
// Every number in wr_bsp.h's header came from this program, the same way every
// number in wr_phase.h's came from phase_sweep.cpp. It is committed so they can
// be checked rather than believed.
//
// It is NOT part of the harness run. It needs a game install -- 1,304 maps and
// about 2.5 GB of other people's work, which is not in this repository, for the
// same reason tests\parity.ps1 is a separate script. tests\test_bsp.exe is the
// part that runs everywhere, on a synthetic map whose answers are known by
// hand. Built by tests\build.bat so it cannot rot: a change to the reader that
// breaks the tool that measures the reader fails the build.
//
//   tests\bsp_sweep.exe [maps dir] [--verbose] [--limit N]
//
// WHAT IT IS ACTUALLY ASKING
//
// Not "does it parse" -- a wrong struct stride parses. The questions are the
// ones that have an answer the file itself can be held to:
//
//   Does every index resolve? A stride that is wrong by even four bytes turns
//   the whole of a lump into a shifted reading of itself, and the very first
//   thing that shows up is an index pointing at nothing. Across the library
//   that is over a hundred million chances to be caught.
//
//   Does the worldspawn tree reach a sensible fraction of the brushes? A tree
//   walked through the wrong field offsets terminates immediately and reports
//   almost nothing owned, which looks exactly like a map with no world geometry
//   rather than like a bug.
//
// The default maps directory is momentum\maps, so this can be run from a game
// install root with no arguments.

#include "wr_bsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <math.h>

static bool g_verbose = false;

// The map the world owns least of. Worth naming rather than counting: the
// panel has to say something honest on a map where model 0 holds almost
// nothing, and knowing how bad that gets is what decides the wording.
static float g_worstPct = 1e9f;
static char  g_worstMap[128] = "none";

// The map that costs the most to keep resident, and how many maps had a face
// that did not close. Both are budget questions rather than curiosities: the
// first sets WR_BSP_MAX_RESIDENT, and the second is the only signal a struct
// stride is wrong in a way every index check passed.
static float g_biggestBytes = 0.0f;
static char  g_biggestMap[128] = "none";
static int   g_unclosedMaps = 0;

// ---------------------------------------------------------------------------
// Small tallies
// ---------------------------------------------------------------------------

struct Hist
{
    int key[16];
    int n[16];
    int used;
};

static void HistAdd(Hist *h, int key)
{
    for (int i = 0; i < h->used; i++)
        if (h->key[i] == key) { h->n[i]++; return; }
    if (h->used < 16)
    {
        h->key[h->used] = key;
        h->n[h->used] = 1;
        h->used++;
    }
}

static void HistPrint(const Hist *h, const char *label, int total)
{
    for (int i = 0; i < h->used; i++)
        for (int j = i + 1; j < h->used; j++)
            if (h->n[j] > h->n[i])
            {
                int tk = h->key[i], tn = h->n[i];
                ((Hist *)h)->key[i] = h->key[j]; ((Hist *)h)->n[i] = h->n[j];
                ((Hist *)h)->key[j] = tk;        ((Hist *)h)->n[j] = tn;
            }
    printf("%s\n", label);
    for (int i = 0; i < h->used; i++)
        printf("    %-6d %5d  %5.1f%%\n", h->key[i], h->n[i],
               total ? 100.0 * h->n[i] / total : 0.0);
}

// Distinct refusal messages, with the first map that produced each. The
// MESSAGE is the interesting output: a sweep that says "18 maps failed" is
// telling you nothing, and one that says "18 maps said LEAFS lump version 3 is
// not one this reads" is telling you what to go and measure.
#define MAX_REASONS 32
static char g_reason[MAX_REASONS][192];
static char g_reasonMap[MAX_REASONS][96];
static int  g_reasonN[MAX_REASONS];
static int  g_reasons = 0;

static void NoteReason(const char *msg, const char *map)
{
    for (int i = 0; i < g_reasons; i++)
        if (strcmp(g_reason[i], msg) == 0) { g_reasonN[i]++; return; }
    if (g_reasons >= MAX_REASONS)
        return;
    _snprintf_s(g_reason[g_reasons], sizeof(g_reason[0]), _TRUNCATE, "%s", msg);
    _snprintf_s(g_reasonMap[g_reasons], sizeof(g_reasonMap[0]), _TRUNCATE,
                "%s", map);
    g_reasonN[g_reasons] = 1;
    g_reasons++;
}

static int CmpF(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static float Pct(float *v, int n, double p)
{
    if (n <= 0)
        return 0.0f;
    int i = (int)(p * n);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return v[i];
}

// ---------------------------------------------------------------------------

// Area-weighted, one bin a degree, over every UP-FACING face. Area-weighted
// and not face-counted, because a surf map is a handful of enormous ramps and
// several thousand small brushes, and counting faces measures the brushes.
static double g_angle[91];
static double g_angleArea = 0.0;

// ---------------------------------------------------------------------------
// The grid, against the thing it accelerates
// ---------------------------------------------------------------------------
//
// tests\test_bsp.exe already compares them, on a fixture whose grid is three
// cells by three by one. That is enough to catch an arithmetic mistake and not
// nearly enough to catch a WALK mistake: a DDA that skips a cell under some
// direction sign, or stops a step early at a boundary, needs a grid with
// somewhere to go wrong in. Real maps have grids of thousands of cells, and
// the answer is still checkable, because brute force does not care how many
// polygons there are -- only how long it takes.

static unsigned int g_seed = 987654321u;
static float Rnd(float lo, float hi)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g_seed >> 8) & 0xFFFFFF) / (float)0x1000000;
}

static bool BruteRay(const WrBspMap *m, const float s[3], const float d[3],
                     float maxDist, int *polyOut, float *tOut)
{
    int best = -1;
    float bestT = maxDist;
    for (int i = 0; i < m->polyCount; i++)
    {
        float t;
        if (!WrBspRayPoly(s, d, m->polys[i].plane, m->verts + m->polys[i].first,
                          m->polys[i].count, &t))
            continue;
        if (t < bestT) { bestT = t; best = i; }
    }
    if (best < 0)
        return false;
    if (polyOut) *polyOut = best;
    if (tOut) *tOut = bestT;
    return true;
}

static long long g_rayGrid = 0, g_rayBrute = 0;      // ticks
static long long g_rayCount = 0, g_rayHits = 0, g_rayMismatch = 0;
static char g_mismatchMap[128] = "none";

static void RayCheck(const WrBspMap *m, const char *name, int rays)
{
    if (m->polyCount <= 0)
        return;

    LARGE_INTEGER a, b, c;
    float (*S)[3] = (float (*)[3])malloc((size_t)rays * sizeof(float) * 3);
    float (*D)[3] = (float (*)[3])malloc((size_t)rays * sizeof(float) * 3);
    if (!S || !D) { free(S); free(D); return; }

    // Rays from inside the map's own box, in every direction. Starting inside
    // is what makes them interesting: a ray from outside enters the grid once
    // and the slab clip does most of the work, where one from inside exercises
    // the step.
    for (int i = 0; i < rays; i++)
    {
        for (int k = 0; k < 3; k++)
            S[i][k] = m->mins[k] + (m->maxs[k] - m->mins[k]) * Rnd(0.0f, 1.0f);
        float len;
        do {
            for (int k = 0; k < 3; k++)
                D[i][k] = Rnd(-1.0f, 1.0f);
            len = (float)sqrt(D[i][0]*D[i][0] + D[i][1]*D[i][1] + D[i][2]*D[i][2]);
        } while (len < 1e-3f);
        for (int k = 0; k < 3; k++)
            D[i][k] /= len;
    }

    const float reach = 4096.0f;

    int *pg = (int *)malloc((size_t)rays * sizeof(int));
    float *tg = (float *)malloc((size_t)rays * sizeof(float));
    unsigned char *hg = (unsigned char *)malloc((size_t)rays);

    QueryPerformanceCounter(&a);
    for (int i = 0; i < rays; i++)
    {
        pg[i] = -1; tg[i] = 0.0f;
        hg[i] = WrBspTraceRay(m, S[i], D[i], reach, &pg[i], &tg[i]) ? 1 : 0;
    }
    QueryPerformanceCounter(&b);
    for (int i = 0; i < rays; i++)
    {
        int pb = -1; float tb = 0.0f;
        bool hb = BruteRay(m, S[i], D[i], reach, &pb, &tb);
        g_rayCount++;
        if (hb) g_rayHits++;
        // The polygon index may legitimately differ where two faces meet at
        // exactly the same distance -- a floor and a wall sharing an edge --
        // so the DISTANCE is what has to agree. A skipped cell moves it.
        if ((hg[i] != 0) != hb || (hb && fabs(tg[i] - tb) > 0.05))
        {
            if (g_rayMismatch == 0)
                _snprintf_s(g_mismatchMap, sizeof(g_mismatchMap), _TRUNCATE,
                            "%s", name);
            g_rayMismatch++;
        }
    }
    QueryPerformanceCounter(&c);

    g_rayGrid  += b.QuadPart - a.QuadPart;
    g_rayBrute += c.QuadPart - b.QuadPart;

    free(S); free(D); free(pg); free(tg); free(hg);
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    const char *root = "momentum\\maps";
    const char *only = NULL;
    int limit = 0, rays = 0;
    bool build = false, angles = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--verbose") == 0) g_verbose = true;
        else if (strcmp(argv[i], "--closure") == 0) build = true;
        else if (strcmp(argv[i], "--angles") == 0) { build = true; angles = true; }
        else if (strcmp(argv[i], "--rays") == 0)
        { build = true; rays = (i + 1 < argc && argv[i + 1][0] != '-')
                               ? atoi(argv[++i]) : 200; }
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            only = argv[++i];
        else root = argv[i];
    }

    char pattern[1024];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.bsp", root);

    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
    {
        printf("no .bsp files under %s\n", root);
        printf("usage: bsp_sweep [maps dir] [--verbose] [--limit N]\n");
        return 2;
    }

    printf("bsp_sweep over %s\n\n", root);

    Hist versions = { { 0 }, { 0 }, 0 };
    int maps = 0, parsed = 0, walked = 0, refusedRead = 0, refusedWalk = 0;
    int compressed = 0;
    long long totalBrushes = 0, totalOwned = 0, totalBytes = 0;
    long long refs = 0;

    int built = 0, refusedBuild = 0;
    long long sideTotal = 0, sideDropped = 0, sideDegenerate = 0;
    long long sideNotClosed = 0, sideTooFar = 0, clipOnly = 0;
    long long polys = 0, verts = 0;
    double solidArea = 0.0, surfArea = 0.0;

    float *ownedPct = (float *)malloc(4096 * sizeof(float));
    int ownedN = 0;
    float *resident = (float *)malloc(4096 * sizeof(float));
    int residentN = 0;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (only && _strnicmp(fd.cFileName, only, strlen(only)) != 0)
            continue;
        if (limit && maps >= limit)
            break;
        maps++;

        char path[1024];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", root, fd.cFileName);

        WrBspRaw r;
        char err[192] = { 0 };
        if (!WrBspReadRaw(path, &r, err, (int)sizeof(err)))
        {
            refusedRead++;
            NoteReason(err, fd.cFileName);
            if (g_verbose)
                printf("  [read] %-44s %s\n", fd.cFileName, err);
            continue;
        }
        parsed++;
        HistAdd(&versions, r.version);
        if (r.compressed)
            compressed++;
        totalBytes += r.totalBytes;

        const int nb = r.count[WR_BSP_L_BRUSHES];
        unsigned char *owned = (unsigned char *)malloc((size_t)nb);
        int n = 0;
        if (!WrBspWorldBrushes(&r, owned, &n, err, (int)sizeof(err)))
        {
            refusedWalk++;
            NoteReason(err, fd.cFileName);
            if (g_verbose)
                printf("  [walk] %-44s %s\n", fd.cFileName, err);
        }
        else
        {
            walked++;
            totalBrushes += nb;
            totalOwned += n;
            if (nb > 0)
            {
                float pct = 100.0f * (float)n / (float)nb;
                if (ownedN < 4096)
                    ownedPct[ownedN++] = pct;
                if (pct < g_worstPct)
                {
                    g_worstPct = pct;
                    _snprintf_s(g_worstMap, sizeof(g_worstMap), _TRUNCATE,
                                "%s (%d of %d)", fd.cFileName, n, nb);
                }
            }

            // Every index in the file, not just the ones the walk followed.
            // The walk only visits brushes the world owns; a stride that is
            // wrong shows up just as readily in the ones it does not.
            for (int i = 0; i < r.count[WR_BSP_L_BRUSHSIDES]; i++)
            {
                int pn = WrBspBrushSidePlane(&r, i);
                refs++;
                if (pn < 0 || pn >= r.count[WR_BSP_L_PLANES])
                {
                    NoteReason("a brushside references a plane that is not there",
                               fd.cFileName);
                    break;
                }
            }
        }

        free(owned);

        if (build)
        {
            WrBspMap map;
            if (!WrBspBuild(&r, &map, err, (int)sizeof(err)))
            {
                refusedBuild++;
                NoteReason(err, fd.cFileName);
                if (g_verbose)
                    printf("  [build] %-43s %s\n", fd.cFileName, err);
            }
            else
            {
                built++;
                sideTotal      += map.sideTotal;
                sideDropped    += map.sideDropped;
                sideDegenerate += map.sideDegenerate;
                sideNotClosed  += map.sideNotClosed;
                sideTooFar     += map.sideTooFar;
                clipOnly       += map.brushClipOnly;
                polys          += map.polyCount;
                verts          += map.vertCount;
                solidArea      += map.solidArea;
                surfArea       += map.surfArea;
                if (residentN < 4096)
                    resident[residentN++] = (float)map.bytes / 1048576.0f;
                if ((float)map.bytes > g_biggestBytes)
                {
                    g_biggestBytes = (float)map.bytes;
                    _snprintf_s(g_biggestMap, sizeof(g_biggestMap), _TRUNCATE,
                                "%s (%d polys)", fd.cFileName, map.polyCount);
                }
                if ((map.sideNotClosed || map.sideTooFar) && g_unclosedMaps < 12)
                {
                    printf("  [closure] %-41s %d did not close, %d left the "
                           "world, of %d sides\n", fd.cFileName,
                           map.sideNotClosed, map.sideTooFar, map.sideTotal);
                    g_unclosedMaps++;
                }

                if (rays > 0)
                    RayCheck(&map, fd.cFileName, rays);

                if (angles)
                    for (int i = 0; i < map.polyCount; i++)
                    {
                        const float nz = map.polys[i].plane[2];
                        if (nz <= 0.0f)
                            continue;       // a ceiling is not a ramp
                        int bin = (int)(WrBspSurfaceAngle(map.polys[i].plane)
                                        + 0.5f);
                        if (bin < 0) bin = 0;
                        if (bin > 90) bin = 90;
                        g_angle[bin] += map.polys[i].area;
                        g_angleArea += map.polys[i].area;
                    }

                WrBspFreeMap(&map);
            }
        }

        WrBspFreeRaw(&r);
    } while (FindNextFileA(find, &fd));

    FindClose(find);
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;

    printf("%d maps, %d parsed, %d walked, in %.1f s (%.0f ms a map)\n\n",
           maps, parsed, walked, secs, maps ? 1000.0 * secs / maps : 0.0);

    HistPrint(&versions, "BSP version", parsed);
    printf("\n    %d of %d have their collision lumps LZMA-compressed"
           " (%.1f%%)\n", compressed, parsed,
           parsed ? 100.0 * compressed / parsed : 0.0);
    printf("    %.2f MB of lumps read, %.2f MB a map\n",
           totalBytes / 1048576.0, parsed ? totalBytes / 1048576.0 / parsed : 0.0);

    printf("\nworldspawn ownership\n");
    printf("    %lld of %lld brushes owned by model 0 (%.1f%%)\n",
           totalOwned, totalBrushes,
           totalBrushes ? 100.0 * totalOwned / totalBrushes : 0.0);
    if (ownedN)
    {
        qsort(ownedPct, ownedN, sizeof(float), CmpF);
        printf("    per map: p10 %.1f%%  p50 %.1f%%  p90 %.1f%%  min %.1f%%\n",
               Pct(ownedPct, ownedN, 0.10), Pct(ownedPct, ownedN, 0.50),
               Pct(ownedPct, ownedN, 0.90), ownedPct[0]);
        printf("    least world-owned map: %.1f%%  %s\n",
               g_worstPct, g_worstMap);
    }
    printf("    %lld brushside plane references checked\n", refs);

    if (build)
    {
        printf("\nthe clip: %d maps built\n", built);
        printf("    %lld sides in, %lld polygons out\n", sideTotal, polys);
        printf("    %lld dropped as bevels or slivers (%.1f%%)\n",
               sideDropped, sideTotal ? 100.0 * sideDropped / sideTotal : 0.0);
        printf("    %lld had a plane that was not a unit normal\n",
               sideDegenerate);
        printf("    %lld had a vertex outside the world\n", sideTooFar);
        printf("    %lld FAILED THE CLOSURE ASSERTION (%.4f%%)\n",
               sideNotClosed,
               sideTotal ? 100.0 * sideNotClosed / sideTotal : 0.0);
        printf("    %d maps had a side refused for either reason\n",
               g_unclosedMaps);
        printf("    %lld solid brushes were playerclip-only and skipped\n",
               clipOnly);
        printf("    %.0f sq units solid, %.0f in the surf band (%.1f%%)\n",
               solidArea, surfArea, solidArea ? 100.0 * surfArea / solidArea : 0.0);

        if (residentN)
        {
            qsort(resident, residentN, sizeof(float), CmpF);
            printf("\n    resident: p50 %.2f MB  p90 %.2f MB  p99 %.2f MB\n",
                   Pct(resident, residentN, 0.50), Pct(resident, residentN, 0.90),
                   Pct(resident, residentN, 0.99));
            printf("    largest: %.2f MB  %s\n", g_biggestBytes / 1048576.0f,
                   g_biggestMap);
            printf("    %lld vertices across the library\n", verts);
        }
    }

    if (rays > 0 && g_rayCount)
    {
        const double f = (double)freq.QuadPart;
        printf("\nthe grid against brute force\n");
        printf("    %lld rays, %lld of them hit something (%.1f%%)\n",
               g_rayCount, g_rayHits, 100.0 * g_rayHits / g_rayCount);
        printf("    %lld DISAGREED with brute force", g_rayMismatch);
        if (g_rayMismatch)
            printf("  (first on %s)", g_mismatchMap);
        printf("\n");
        printf("    grid  %.1f us a ray\n",
               1e6 * (g_rayGrid / f) / (double)g_rayCount);
        printf("    brute %.1f us a ray  -- %.0fx\n",
               1e6 * (g_rayBrute / f) / (double)g_rayCount,
               g_rayGrid ? (double)g_rayBrute / (double)g_rayGrid : 0.0);
    }

    if (angles && g_angleArea > 0.0)
    {
        printf("\nup-facing surface angle, weighted by area\n");
        printf("    0 is a floor, 90 a wall. 45.6 is Source's own standable\n"
               "    cut -- below it you slide, and that is what a surf ramp is.\n\n");
        double peak = 0.0;
        for (int i = 0; i <= 90; i++)
            if (g_angle[i] > peak)
                peak = g_angle[i];
        double band = 0.0;
        for (int i = 0; i <= 90; i++)
        {
            double frac = g_angle[i] / g_angleArea;
            if (WrBspIsSurfBand((float)cos(i * 3.14159265358979 / 180.0)))
                band += frac;
            if (frac < 0.002 && g_angle[i] < peak * 0.02)
                continue;
            int bars = (int)(60.0 * g_angle[i] / peak + 0.5);
            printf("    %2d deg  %5.2f%%  ", i, 100.0 * frac);
            for (int k = 0; k < bars; k++)
                putchar('#');
            putchar('\n');
        }
        printf("\n    %.1f%% of up-facing area is inside the surf band\n",
               100.0 * band);
    }

    printf("\nrefused: %d on read, %d on walk, %d on build\n",
           refusedRead, refusedWalk, refusedBuild);
    for (int i = 0; i < g_reasons; i++)
        printf("    %4d  %s\n            first: %s\n",
               g_reasonN[i], g_reason[i], g_reasonMap[i]);

    free(ownedPct);
    free(resident);

    // The exit code is the whole assertion. Anything refused is a layout this
    // does not know about, and the point of running it is to find out.
    bool clean = (refusedRead == 0 && refusedWalk == 0 &&
                  refusedBuild == 0 && sideNotClosed == 0 &&
                  g_rayMismatch == 0 && g_reasons == 0);
    printf("\n%s\n", clean ? "every map read and walked"
                           : "SOME MAPS WERE REFUSED");
    return clean ? 0 : 1;
}
