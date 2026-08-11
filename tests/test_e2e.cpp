// test_e2e.cpp  --  a whole demo in, the reference's own .wrpath out.
//
// Every other harness here tests one layer. test_mtv reads a container,
// test_lzma decodes a stream, test_dp scans a body and runs the dynamic
// program over it, test_wrpath writes a file and reads it back. All four pass
// happily while the four are wired together wrongly -- a body handed on with
// the container's seventeen bytes still on the front, a tick interval taken
// from the wrong field, markers anchored against a point list that had already
// been trimmed. Seam bugs. A per-layer harness cannot see a seam by
// construction, because it owns both ends of every one it has.
//
// So this owns none of them. It hands WrDemoProcess a file and compares the
// bytes that come out against the bytes wrpath_extract.py produced from the
// same file -- one demo, one expected answer, the whole pipeline in between.
//
// WHY A COMMITTED GOLDEN AND NOT A COMPUTATION
//
// Because a fixture this file computed for itself would prove that two of my
// programs agree, which is the one thing already known. kE2eWrpath came out of
// the reference, and the reference is what the port is a port of.
//
// The 6,249-demo parity run makes the same comparison and makes it far better;
// it also needs a game install, a demo library and a specific interpreter, so
// it runs on one machine in the world and never in CI. This is the part of it
// that fits in thirty kilobytes of committed C, and it runs on every push.
//
// WHAT THE FIXTURE WAS BUILT TO REACH
//
// tests\make_fixture.py plants a helix in a body of 0xFF -- filler that cannot
// be mistaken for a coordinate, since a word of all ones has a biased exponent
// of 255 and the scan admits [117, 141] -- and then writes the run stats to
// MATCH what the reference made of that body. That last step is the point: a
// demo with no maxHorizontalSpeed takes the first chain it finds and never asks
// the speed oracle anything, and the speed oracle is the whole reason this
// extractor can claim to have found the player rather than something shaped
// like the player. The generator asserts the reference identified by speed,
// placed its marker, and found the start, before it will write the header.
//
// Build:  tests\build.bat
// Run:    tests\test_e2e.exe

#include "wr_demo.h"
#include "wr_dp.h"
#include "wr_mtv.h"
#include "wr_path.h"
#include "wr_extract.h"
#include "wr_log.h"

#include "fixture_e2e.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// wr_path.cpp reaches outside itself for this; nothing here needs a camera.
bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }

static unsigned char *Slurp(const char *path, size_t *lenOut)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n)
    {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b)
        *lenOut = (size_t)n;
    return b;
}

static bool Spill(const char *path, const unsigned char *b, size_t n)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    const bool ok = fwrite(b, 1, n, f) == n;
    fclose(f);
    return ok;
}

int main(void)
{
    printf("\n=== wrlines end to end, against the reference's own file ===\n");

    // The .wrpath header carries a timestamp at 0xF4 and the CRC covers it, so
    // the golden is only a constant if the clock is. Same value the generator
    // ran the reference under.
    SetEnvironmentVariableA("WRLINES_FAKE_NOW", WR_FIXTURE_E2E_NOW);

    // The real layout, not a scratch one: <data>\paths is where the driver
    // sends a worker's output and the only place WrPathLoadMap looks, so
    // writing anywhere else would test the writer and then read the file back
    // through something other than the code that reads it in the game.
    //
    // WrDataPath hands out one of four rotating static buffers, so each answer
    // is copied before the next call -- which is the same rule the driver
    // follows for the same reason.
    char outDir[MAX_PATH];
    strcpy_s(outDir, sizeof(outDir), WrDataPath("paths"));
    WrMakeTree(outDir);

    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), WrDataPath(""));

    // .mtv is gitignored at any depth -- for real demos, which carry somebody's
    // name and SteamID64 -- so a crash that leaves this behind cannot put it in
    // front of `git add .` either. This one is synthetic and has neither.
    char demo[MAX_PATH];
    _snprintf_s(demo, sizeof(demo), _TRUNCATE, "%s\\%s.mtv", dir,
                WR_FIXTURE_E2E_STEM);
    if (!Spill(demo, kE2eMtv, sizeof(kE2eMtv)))
    {
        printf("  could not write %s\n", demo);
        return 1;
    }

    char wrote[MAX_PATH];
    _snprintf_s(wrote, sizeof(wrote), _TRUNCATE, "%s\\%s\\%s.wrpath",
                outDir, WR_FIXTURE_E2E_MAP, WR_FIXTURE_E2E_STEM);
    DeleteFileA(wrote);

    WrDemoArgs a;
    memset(&a, 0, sizeof(a));
    a.outDir = outDir;

    WrDemoResult r;
    memset(&r, 0, sizeof(r));
    const WrDemoOutcome got = WrDemoProcess(demo, &a, &r);

    // -----------------------------------------------------------------------
    printf("\nthe container, the codec and the header\n");
    {
        if (got != WR_DEMO_OK)
            printf("     refused: %s\n", r.message);
        Check(got == WR_DEMO_OK, "the demo is extracted at all");
        Check(strcmp(r.h.map, WR_FIXTURE_E2E_MAP) == 0,
              "the map name comes out of the fixed header");
        Check(r.h.ticks == 210, "and the tick count with it");
        Check(r.bodyBytes == 8192,
              "the body decompresses to the length the container claims");
        Check(r.fileBytes == sizeof(kE2eMtv),
              "against a file of the size on disk");
    }

    // -----------------------------------------------------------------------
    printf("\nwhat the dynamic program found in it\n");
    {
        Check(r.dp.pointCount == WR_FIXTURE_E2E_POINTS,
              "every planted point came back and nothing else did");
        Check(r.dp.info.segments == 1, "as one segment, not a stitch");
        Check(r.dp.info.confident, "confidently");
        Check(strcmp(r.dp.info.identifiedBy, "speed") == 0,
              "and by matching the run's own maxHorizontalSpeed");
        printf("     %d candidates, %d points, coverage %.4f, err %.6f\n",
               r.dp.info.candidates, r.dp.pointCount, r.dp.info.coverage,
               r.dp.info.matchError);
        Check(r.dp.info.matchError < 1e-9,
              "to the last place the reference reported");
    }

    // -----------------------------------------------------------------------
    printf("\nthe split marker and the start, off the run's own JSON\n");
    {
        Check(r.markerCount == 1, "the one subsegment placed a marker");
        Check(r.markersOk, "and it was anchored rather than guessed");
        Check(r.startOk, "the start velocity fingerprint matched a point");
        Check(r.startIndex == WR_FIXTURE_E2E_START,
              "at the index it was taken from");
        Check(!r.flagged, "and nothing about the run is flagged");
    }

    // -----------------------------------------------------------------------
    printf("\nand the file is the reference's, byte for byte\n");
    {
        size_t n = 0;
        unsigned char *mine = Slurp(wrote, &n);
        Check(mine != NULL, "a .wrpath was written where the driver expects it");
        if (mine)
        {
            Check(n == sizeof(kE2eWrpath), "of the length the reference wrote");
            if (n == sizeof(kE2eWrpath))
            {
                size_t at = 0;
                bool same = true;
                for (size_t i = 0; i < n; i++)
                    if (mine[i] != kE2eWrpath[i])
                    {
                        at = i;
                        same = false;
                        break;
                    }
                if (!same)
                    printf("     first difference at 0x%02X: %02X, "
                           "reference has %02X\n",
                           (unsigned)at, mine[at], kE2eWrpath[at]);
                Check(same, "and identical for every one of them");
            }
            Check((long long)n == r.bytes,
                  "which is the byte count the driver reports having written");
            free(mine);
        }
    }

    // -----------------------------------------------------------------------
    // The bytes matching proves the writer. This proves the pair: a file the
    // port produced is one the shipped loader accepts, CRC and all. LoadOne is
    // the only consumer of this format inside the game.
    printf("\nand the shipped loader reads what the shipped writer wrote\n");
    {
        WrPathLoadMap(WR_FIXTURE_E2E_MAP);
        int guard = 0;
        while (WrPathLoading(NULL, NULL) && ++guard < 10000)
            WrPathLoadTick();
        WrPathLoadTick();

        Check(WrRunCount() == 1, "the loader takes it back");
        const WrRun *run = WrRunCount() ? WrRunAt(0) : NULL;
        Check(run != NULL && run->pointCount == WR_FIXTURE_E2E_POINTS,
              "with every point still there");
        Check(run != NULL && run->markerCount == 1,
              "and the marker still on it");
        Check(run != NULL && run->startIndex == WR_FIXTURE_E2E_START,
              "and the start index the extractor found");
        Check(run != NULL && run->startTrusted,
              "marked as recovered rather than assumed");
    }

    WrDemoFree(&r);
    DeleteFileA(demo);
    DeleteFileA(wrote);

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
