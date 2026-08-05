// test_board.cpp  --  the leaderboard cache parser.
//
// This reads a file written by a different language, whose fields include a
// player's alias -- free text, chosen by a stranger, in any script. The failure
// mode that matters is not a crash: it is a row that parses into the wrong
// columns and puts a name where a hash goes, which then reads as "you already
// have this run" or downloads nothing. That is invisible in a UI and obvious in
// a test.
//
// So the cases here are the ones a real file will actually contain: a short
// line, a line with too few fields, a name with a space in it, an empty rank, a
// file with no rows at all, and a file that does not exist.
//
// Build:  tests\build.bat
// Run:    tests\test_board.exe

#include "wr_board.h"

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

static const char *kPath = "tests\\_board_tmp.tsv";

static void Write(const char *body)
{
    FILE *f = NULL;
    if (fopen_s(&f, kPath, "wb") != 0 || !f)
    {
        printf("  could not write %s\n", kPath);
        exit(2);
    }
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

int main(void)
{
    static WrBoardRow rows[64];
    int total = 0, mapId = 0;
    long long fetched = 0;

    printf("\n=== wrlines leaderboard cache ===\n");

    // -----------------------------------------------------------------------
    printf("\na well-formed file parses into the right columns\n");
    {
        Write("# WrLines leaderboard cache\n"
              "# map\tsurf_demise\n"
              "# mapid\t265\n"
              "# gamemode\t1\n"
              "# track\t0\t1\n"
              "# total\t9108\n"
              "# fetched\t1785887634\n"
              "# rank\ttime\tsteamid\talias\thash\tepoch\turl\n"
              "1\t37.169999\t76561197994615740\t.x\t6a792483cefd88251781ae06e7a76f26355d86f2\t1765121931\thttps://cdn.momentum-mod.org/runs/6a792483\n"
              "9108\t79.080002\t76561198000000001\tsadE\tcc91e1bb47c54ff286d02df2d4971da149988a86\t1773000000\thttps://cdn.momentum-mod.org/runs/cc91e1bb\n");

        int n = WrBoardParseFile(kPath, rows, 64, &total, &fetched, &mapId);
        printf("     %d rows, total %d, mapid %d, fetched %lld\n",
               n, total, mapId, fetched);
        Check(n == 2, "both rows survive");
        Check(total == 9108, "the board's real size comes from the header");
        Check(mapId == 265, "and the map id, which locates the game's own tree");
        Check(fetched == 1785887634LL, "and the fetch time, for the staleness line");
        Check(rows[0].rank == 1 && rows[1].rank == 9108, "ranks");
        Check(rows[1].time > 79.07f && rows[1].time < 79.09f, "times");
        Check(strcmp(rows[0].alias, ".x") == 0, "aliases");
        Check(strcmp(rows[1].hash,
                     "cc91e1bb47c54ff286d02df2d4971da149988a86") == 0,
              "hashes, which are the .mtv filenames");
        Check(strncmp(rows[0].url, "https://cdn.", 12) == 0,
              "and the download URL the server gave us");
        Check(rows[0].steamId == 76561197994615740ULL, "steam ids survive 64 bits");
    }

    // -----------------------------------------------------------------------
    printf("\na name with spaces stays one field\n");
    {
        // Tabs separate; spaces do not. Python replaces any tab inside an alias
        // before writing for exactly this reason, and this is the other half of
        // that contract.
        Write("# rank\ttime\tsteamid\talias\thash\tepoch\turl\n"
              "12\t41.5\t765611980\t[ RNR ] big michael\tdeadbeef01\t100\thttp://x\n");
        int n = WrBoardParseFile(kPath, rows, 64, NULL, NULL, NULL);
        Check(n == 1, "one row");
        Check(strcmp(rows[0].alias, "[ RNR ] big michael") == 0,
              "the whole name, spaces and all");
        Check(strcmp(rows[0].hash, "deadbeef01") == 0,
              "and the hash is still the hash, not the tail of the name");
    }

    // -----------------------------------------------------------------------
    printf("\nrubbish is dropped, not half-kept\n");
    {
        Write("# rank\ttime\tsteamid\talias\thash\tepoch\turl\n"
              "1\t37.0\t765\tgood\taaaa\t100\thttp://x\n"
              "\n"
              "2\t38.0\t765\ttruncated\n"
              "0\t39.0\t765\tzerorank\tbbbb\t100\thttp://x\n"
              "4\t40.0\t765\tnohash\t\t100\thttp://x\n"
              "5\t41.0\t765\talso good\tcccc\t100\thttp://x\n");
        int n = WrBoardParseFile(kPath, rows, 64, NULL, NULL, NULL);
        printf("     %d of 5 candidate rows kept\n", n);
        Check(n == 2, "only the two complete rows");
        Check(rows[0].rank == 1 && rows[1].rank == 5,
              "and they are the right two");

        // A row missing its trailing fields must not be kept with a URL
        // inherited from anywhere: it is dropped entirely.
        bool anyTruncated = false;
        for (int i = 0; i < n; i++)
            if (strcmp(rows[i].alias, "truncated") == 0)
                anyTruncated = true;
        Check(!anyTruncated, "a short line is not salvaged into a bad record");
    }

    // -----------------------------------------------------------------------
    printf("\nthe row cap is a stop, not a crash\n");
    {
        FILE *f = NULL;
        fopen_s(&f, kPath, "wb");
        fprintf(f, "# rank\ttime\tsteamid\talias\thash\tepoch\turl\n");
        for (int i = 1; i <= 200; i++)
            fprintf(f, "%d\t%d.0\t765\tp%d\th%d\t100\thttp://x\n", i, i, i, i);
        fclose(f);

        int n = WrBoardParseFile(kPath, rows, 64, NULL, NULL, NULL);
        Check(n == 64, "stops at the caller's limit");
        Check(rows[63].rank == 64, "having kept the first 64 in file order");
    }

    // -----------------------------------------------------------------------
    printf("\nheader-only and missing files are not errors, and are different\n");
    {
        Write("# WrLines leaderboard cache\n# total\t9108\n");
        int n = WrBoardParseFile(kPath, rows, 64, &total, NULL, NULL);
        Check(n == 0, "a file with no rows parses to none");
        Check(total == 9108, "and still reports what the board holds");

        int m = WrBoardParseFile("tests\\_board_does_not_exist.tsv", rows, 64,
                                 &total, &fetched, &mapId);
        Check(m == -1, "a missing file is -1, which is not the same as empty");
        Check(total == 0 && fetched == 0 && mapId == 0,
              "and it clears the header outputs rather than leaving stale ones");
    }

    // -----------------------------------------------------------------------
    printf("\ngamemode names come from Momentum's own enum\n");
    {
        Check(strcmp(WrGamemodeName(1), "surf") == 0, "1 is surf");
        Check(strcmp(WrGamemodeName(2), "bhop") == 0, "2 is bhop");
        Check(strcmp(WrGamemodeName(7), "RJ") == 0, "7 is RJ");
        Check(strcmp(WrGamemodeName(13), "defrag VTG") == 0, "13 is defrag VTG");
        Check(strcmp(WrGamemodeName(0), "?") == 0, "0 is out of range");
        Check(strcmp(WrGamemodeName(14), "?") == 0, "so is 14");
    }

    remove(kPath);
    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
