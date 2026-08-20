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

static bool g_verbose = false;

// The map the world owns least of. Worth naming rather than counting: the
// panel has to say something honest on a map where model 0 holds almost
// nothing, and knowing how bad that gets is what decides the wording.
static float g_worstPct = 1e9f;
static char  g_worstMap[128] = "none";

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

int main(int argc, char **argv)
{
    const char *root = "momentum\\maps";
    int limit = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--verbose") == 0) g_verbose = true;
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = atoi(argv[++i]);
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

    float *ownedPct = (float *)malloc(4096 * sizeof(float));
    int ownedN = 0;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
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

    printf("\nrefused: %d on read, %d on walk\n", refusedRead, refusedWalk);
    for (int i = 0; i < g_reasons; i++)
        printf("    %4d  %s\n            first: %s\n",
               g_reasonN[i], g_reason[i], g_reasonMap[i]);

    free(ownedPct);

    // The exit code is the whole assertion. Anything refused is a layout this
    // does not know about, and the point of running it is to find out.
    bool clean = (refusedRead == 0 && refusedWalk == 0 && g_reasons == 0);
    printf("\n%s\n", clean ? "every map read and walked"
                           : "SOME MAPS WERE REFUSED");
    return clean ? 0 : 1;
}
