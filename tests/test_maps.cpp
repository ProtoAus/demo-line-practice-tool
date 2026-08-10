// test_maps.cpp  --  the map index, from the game's cache to the panel.
//
// This covers the whole of what used to be `wrpath_extract.py --index-maps`:
// inflate two cache files, parse a million-node JSON down to four fields a map,
// merge them, sort, write maps.txt, and read it back into the table the Maps tab
// draws. It links the real writer AND the real reader, because the thing worth
// checking is that they agree with each other and with the reference.
//
// THE EXPECTED OUTPUT IS NOT INVENTED
//
// Every line asserted below was produced by running the reference
// implementation over the same two fixture files:
//
//     py -3 wrpath_extract.py --game <fixture> --index-maps --out <tmp>\paths
//
// and copying what it wrote. That is the contract this phase of the port has to
// meet, so it is written down rather than described.
//
// The fixture is synthetic and committed as pre-compressed bytes, because the
// DLL is built with MINIZ_NO_DEFLATE_APIS and cannot make a zlib stream even to
// test itself. tests\make_fixture.py generates it and says why each map in it
// is shaped the way it is.
//
// Build:  tests\build.bat
// Run:    tests\test_maps.exe

#include "wr_maps.h"
#include "wr_msml.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_msml.h"

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// A game tree with nothing in it but a map cache. Under tests\, which .gitignore
// already covers, and torn down before the summary.
static const char *kGameDir = "tests\\_fixgame";

static bool PutFile(const char *path, const unsigned char *blob, size_t n)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    bool ok = (fwrite(blob, 1, n, f) == n);
    fclose(f);
    return ok;
}

static bool MakeFixtureTree(void)
{
    CreateDirectoryA(kGameDir, NULL);
    CreateDirectoryA("tests\\_fixgame\\momentum", NULL);
    CreateDirectoryA("tests\\_fixgame\\momentum\\_cache", NULL);
    return PutFile("tests\\_fixgame\\momentum\\_cache\\approved_1.dat",
                   kApprovedDat, sizeof(kApprovedDat)) &&
           PutFile("tests\\_fixgame\\momentum\\_cache\\submission_2.dat",
                   kSubmissionDat, sizeof(kSubmissionDat));
}

static void DropFixtureTree(void)
{
    DeleteFileA("tests\\_fixgame\\momentum\\_cache\\approved_1.dat");
    DeleteFileA("tests\\_fixgame\\momentum\\_cache\\submission_2.dat");
    RemoveDirectoryA("tests\\_fixgame\\momentum\\_cache");
    RemoveDirectoryA("tests\\_fixgame\\momentum");
    RemoveDirectoryA(kGameDir);
}

// What the reference wrote, verbatim.
static const char *kExpected[] = {
    "# WrLines map index, from the game's own _cache. No network.",
    "# id\tname\ttier\tapproved\tmodes",
    "7\tahop_bare\t0\t1\t",
    "383\tbhop_landmark2\t2\t1\t1,2",
    "1816\tbhop_luvinox\t0\t0\t2",
    "900\tsurf_modes\t4\t0\t3,7,12",
    "112\tsurf_overlap\t5\t0\t1",
    "901\tsurf_\xc3\xbcml\xc3\xa4ut\t6\t0\t1",
};

int main(void)
{
    printf("\n=== wrlines map index ===\n");

    if (!MakeFixtureTree())
    {
        printf("  could not write the fixture cache under %s\n", kGameDir);
        return 2;
    }

    // -----------------------------------------------------------------------
    printf("\nthe cache reader takes the four fields it wants and skips the rest\n");
    {
        WrMsmlMap cat[32];
        int skipped = -1;
        int n = WrMsmlRead(kGameDir, cat, 32, &skipped);

        Check(n == 6, "six maps across two files, not seven");
        Check(skipped == 0, "neither file was unreadable");

        // The float id. Nothing else in the fixture is called this.
        bool sawFloat = false;
        for (int i = 0; i < n; i++)
            if (strcmp(cat[i].name, "surf_floatid") == 0)
                sawFloat = true;
        Check(!sawFloat, "an id of 265.0 is skipped, not rounded to 265");

        const WrMsmlMap *lm = NULL, *ov = NULL, *bare = NULL, *lux = NULL;
        for (int i = 0; i < n; i++)
        {
            if (strcmp(cat[i].name, "bhop_landmark2") == 0) lm = &cat[i];
            if (strcmp(cat[i].name, "surf_overlap") == 0)   ov = &cat[i];
            if (strcmp(cat[i].name, "ahop_bare") == 0)      bare = &cat[i];
            if (strcmp(cat[i].name, "bhop_luvinox") == 0)   lux = &cat[i];
        }

        Check(lm && lm->tier == 2, "the tier comes from the trackType 0 board");
        Check(lm && lm->modes == ((1u << 1) | (1u << 2)),
              "and the tier 7 bonus board contributes its mode but not its tier");
        Check(bare && bare->tier == 0 && bare->modes == 0,
              "a map with no leaderboards is tier 0 with no modes");
        Check(lux && lux->tier == 0, "\"tier\": null reads as 0, not as garbage");
        Check(ov && ov->id == 112 && !ov->approved,
              "the later cache file wins on a duplicate name");
    }

    // -----------------------------------------------------------------------
    printf("\nmaps.txt is what the reference implementation wrote\n");
    {
        int n = WrMapsWriteIndex(kGameDir, NULL);
        Check(n == 6, "the writer reports six");

        FILE *f = NULL;
        int line = 0;
        bool same = true, extra = false;
        if (fopen_s(&f, WrDataPath("maps.txt"), "rb") == 0 && f)
        {
            char buf[512];
            while (fgets(buf, sizeof(buf), f))
            {
                // Read in BINARY, so the line endings are visible rather than
                // silently collapsed. The reference writes in Python's text
                // mode and the shipping reader opens "r"; a writer that emitted
                // bare \n would work here and put a stray \r on the last field
                // of every row in the real one.
                size_t len = strlen(buf);
                bool crlf = (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n');
                if (!crlf)
                {
                    if (same)
                        printf("     line %d does not end CRLF\n", line + 1);
                    same = false;
                }
                if (len >= 2)
                    buf[len - 2] = '\0';

                if (line < (int)(sizeof(kExpected) / sizeof(kExpected[0])))
                {
                    if (strcmp(buf, kExpected[line]) != 0)
                    {
                        if (same)
                        {
                            printf("     line %d\n", line + 1);
                            printf("     want: %s\n", kExpected[line]);
                            printf("     got : %s\n", buf);
                        }
                        same = false;
                    }
                }
                else
                {
                    extra = true;
                }
                line++;
            }
            fclose(f);
        }
        else
        {
            printf("     could not read back %s\n", WrDataPath("maps.txt"));
            same = false;
        }

        Check(line == 8, "two header lines and six maps");
        Check(!extra, "and nothing after them");
        Check(same, "every line matches the reference byte for byte");
    }

    // -----------------------------------------------------------------------
    printf("\nand the panel's own reader gets the same table back\n");
    {
        // The refresh runs on a thread and also counts files on disk, which for
        // a fixture game tree is zero of everything. What matters is the parse.
        WrMapsRefresh();
        for (int i = 0; i < 500 && !WrMapsReady(); i++)
            Sleep(10);

        Check(WrMapsReady(), "the refresh finished");
        Check(WrMapsCount() == 6, "six rows in the table");

        int at = WrMapsFind("surf_overlap");
        const WrMapInfo *m = (at >= 0) ? WrMapsAt(at) : NULL;
        Check(m && m->id == 112 && m->tier == 5 && !m->approved,
              "surf_overlap round-tripped id, tier and approved");

        at = WrMapsFind("bhop_landmark2");
        m = (at >= 0) ? WrMapsAt(at) : NULL;
        Check(m && m->approved, "and an approved map is still approved");

        // The name is the field a mis-split would land in the wrong column, and
        // it is the one carrying multi-byte UTF-8.
        at = WrMapsFind("surf_\xc3\xbcml\xc3\xa4ut");
        Check(at >= 0, "a non-ASCII name survives the write and the read");

        at = WrMapsFind("ahop_bare");
        m = (at >= 0) ? WrMapsAt(at) : NULL;
        Check(m && m->tier == 0,
              "an empty modes field does not swallow the field before it");
    }

    DeleteFileA(WrDataPath("maps.txt"));
    DropFixtureTree();

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
