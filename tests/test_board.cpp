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
    printf("\nthe rank sort is stable over the order rows arrived in\n");
    {
        // Forty rows, all at the same rank, merged in a known order. This is
        // the trap the writer exists to avoid: the reference sorts a dict's
        // values, dicts iterate in insertion order, and sorted() is stable --
        // so ties come out in arrival order there. qsort is not stable and
        // would leave them in whatever order the partitioning happened to
        // produce, which is a difference of one line in a two-hundred-row file
        // and an afternoon to find.
        //
        // Forty rather than three because MSVC's qsort falls back to an
        // insertion sort below a threshold, and an insertion sort is stable by
        // accident. A three-row case would pass with a broken comparator.
        WrBoardCache c;
        memset(&c, 0, sizeof(c));
        for (int i = 0; i < 40; i++)
        {
            WrBoardCacheRow r;
            memset(&r, 0, sizeof(r));
            r.rank = 7;
            r.time = 1.0 + i;
            _snprintf_s(r.hash, sizeof(r.hash), _TRUNCATE, "h%02d", i);
            strcpy_s(r.steamId, sizeof(r.steamId), "1");
            strcpy_s(r.alias, sizeof(r.alias), "x");
            WrBoardCacheMerge(&c, &r);
        }
        strcpy_s(c.map, sizeof(c.map), "m");
        Check(WrBoardWriteCache(kPath, &c), "forty tied rows are written");
        WrBoardCacheFree(&c);

        WrBoardCache back;
        WrBoardReadCache(kPath, &back, 0);
        bool ordered = (back.count == 40);
        for (int i = 0; i < back.count && ordered; i++)
        {
            char want[16];
            _snprintf_s(want, sizeof(want), _TRUNCATE, "h%02d", i);
            if (strcmp(back.rows[i].hash, want) != 0)
                ordered = false;
        }
        Check(ordered, "and come back in the order they went in");
        WrBoardCacheFree(&back);
    }

    // -----------------------------------------------------------------------
    printf("\na re-fetched row keeps its place and takes the new value\n");
    {
        WrBoardCache c;
        memset(&c, 0, sizeof(c));
        const char *hashes[3] = {"AAAA", "BBBB", "CCCC"};
        for (int i = 0; i < 3; i++)
        {
            WrBoardCacheRow r;
            memset(&r, 0, sizeof(r));
            r.rank = i + 1;
            r.time = 10.0 + i;
            strcpy_s(r.hash, sizeof(r.hash), hashes[i]);
            strcpy_s(r.steamId, sizeof(r.steamId), "1");
            strcpy_s(r.alias, sizeof(r.alias), "x");
            WrBoardCacheMerge(&c, &r);
        }

        // The same run again, at a new rank, and spelled in a different case --
        // the reference keys its dict on hash.lower(), so this is the same row.
        WrBoardCacheRow again;
        memset(&again, 0, sizeof(again));
        again.rank = 3;
        again.time = 99.0;
        strcpy_s(again.hash, sizeof(again.hash), "bbbb");
        strcpy_s(again.steamId, sizeof(again.steamId), "1");
        strcpy_s(again.alias, sizeof(again.alias), "x");
        Check(!WrBoardCacheMerge(&c, &again), "a hash in a different case is not new");
        Check(c.count == 3, "so nothing was appended");
        Check(c.rows[1].time > 98.9 && strcmp(c.rows[1].hash, "bbbb") == 0,
              "and the row where it already sat took the new value whole");

        WrBoardWriteCache(kPath, &c);
        WrBoardCacheFree(&c);

        WrBoardCache back;
        WrBoardReadCache(kPath, &back, 0);
        Check(back.count == 3 &&
              strcmp(back.rows[0].hash, "AAAA") == 0 &&
              strcmp(back.rows[1].hash, "bbbb") == 0 &&
              strcmp(back.rows[2].hash, "CCCC") == 0,
              "the tie at rank 3 breaks the way insertion order says it should");
        WrBoardCacheFree(&back);
    }

    // -----------------------------------------------------------------------
    printf("\nthe header lines are a fixed order, and only what is set\n");
    {
        WrBoardCache c;
        memset(&c, 0, sizeof(c));
        strcpy_s(c.map, sizeof(c.map), "surf_demise");
        strcpy_s(c.mapId, sizeof(c.mapId), "265");
        strcpy_s(c.gamemode, sizeof(c.gamemode), "1");
        strcpy_s(c.track, sizeof(c.track), "0\t1");
        strcpy_s(c.total, sizeof(c.total), "9108");
        strcpy_s(c.fetched, sizeof(c.fetched), "1700000000");
        WrBoardWriteCache(kPath, &c);
        WrBoardCacheFree(&c);

        FILE *f = NULL;
        fopen_s(&f, kPath, "rb");
        char blob[1024] = {0};
        size_t got = f ? fread(blob, 1, sizeof(blob) - 1, f) : 0;
        if (f) fclose(f);
        blob[got] = '\0';

        Check(strcmp(blob,
            "# WrLines leaderboard cache -- the windows you asked for, not the whole board.\r\n"
            "# map\tsurf_demise\r\n"
            "# mapid\t265\r\n"
            "# gamemode\t1\r\n"
            "# track\t0\t1\r\n"
            "# total\t9108\r\n"
            "# fetched\t1700000000\r\n"
            "# rank\ttime\tsteamid\talias\thash\tepoch\turl\r\n") == 0,
            "the six keys in the reference's order, then the column line");

        // CRLF, and that is not a preference: the reference writes with
        // Python's default newline translation, and both readers open the file
        // in text mode. A binary write would put a stray \r on every url field.
        Check(strstr(blob, "\r\n") != NULL, "the lines end CRLF");

        // Absent is not the same as zero. A friends lookup learns nothing about
        // the board's size, so it must leave whatever was there rather than
        // writing a "# total 0" that reads as an empty leaderboard.
        WrBoardCache noTotal;
        memset(&noTotal, 0, sizeof(noTotal));
        strcpy_s(noTotal.map, sizeof(noTotal.map), "m");
        WrBoardWriteCache(kPath, &noTotal);
        WrBoardCacheFree(&noTotal);

        fopen_s(&f, kPath, "rb");
        memset(blob, 0, sizeof(blob));
        got = f ? fread(blob, 1, sizeof(blob) - 1, f) : 0;
        if (f) fclose(f);
        Check(strstr(blob, "# total") == NULL,
              "a key that is not set gets no line at all");
        Check(strstr(blob, "# map\tm\r\n") != NULL, "and the ones that are, do");
    }

    // -----------------------------------------------------------------------
    printf("\nthe time is a double all the way to the file\n");
    {
        WrBoardCache c;
        memset(&c, 0, sizeof(c));
        strcpy_s(c.map, sizeof(c.map), "m");

        WrBoardCacheRow r;
        memset(&r, 0, sizeof(r));
        r.rank = 1;
        // A float holds about seven significant digits, so this value as a
        // float prints "4567.891113" and as a double "4567.891234". The row
        // type the FETCHER uses is a double for exactly this reason -- the
        // table's WrBoardRow.time is a float, which is fine for a column and
        // wrong for a byte-for-byte comparison.
        r.time = 4567.891234;
        strcpy_s(r.hash, sizeof(r.hash), "h");
        strcpy_s(r.steamId, sizeof(r.steamId), "1");
        strcpy_s(r.alias, sizeof(r.alias), "x");
        WrBoardCacheMerge(&c, &r);

        r.rank = 2;
        r.time = 1.0 / 3.0;
        strcpy_s(r.hash, sizeof(r.hash), "h2");
        WrBoardCacheMerge(&c, &r);

        WrBoardWriteCache(kPath, &c);
        WrBoardCacheFree(&c);

        FILE *f = NULL;
        fopen_s(&f, kPath, "rb");
        char blob[512] = {0};
        size_t got = f ? fread(blob, 1, sizeof(blob) - 1, f) : 0;
        if (f) fclose(f);
        blob[got] = '\0';

        Check(strstr(blob, "1\t4567.891234\t") != NULL,
              "%.6f of a double keeps the sixth place");
        Check(strstr(blob, "4567.891113") == NULL,
              "which a float would have lost");
        Check(strstr(blob, "2\t0.333333\t") != NULL, "and rounds the last one");
    }

    // -----------------------------------------------------------------------
    printf("\nrows the display would not show still round-trip\n");
    {
        // WrBoardParseFile drops a row with no rank or no hash, because a table
        // cannot show one. The FETCHER must not: it reads, merges and writes
        // the same file, and dropping a row it did not understand would delete
        // it from somebody's cache without saying so.
        Write("# rank\ttime\tsteamid\talias\thash\tepoch\turl\n"
              "0\t1.000000\t1\tzerorank\tZZZZ\t0\t\n"
              "5\t2.000000\t1\tfine\tAAAA\t0\t\n");

        WrBoardCache c;
        WrBoardReadCache(kPath, &c, 0);
        Check(c.count == 2, "the reader keeps both");
        int shown = WrBoardParseFile(kPath, rows, 64, NULL, NULL, NULL);
        Check(shown == 1, "and the table shows one");
        WrBoardCacheFree(&c);
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
