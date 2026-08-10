// test_api.cpp  --  the leaderboard API layer, and the file it writes.
//
// This is the first harness in the project whose subject can reach the network,
// and it never does. Every request goes through tests\api_tape.cpp, which
// answers from a checked-in recording and treats a URL it does not hold as an
// error rather than as a reason to go and fetch it. The request count is
// asserted at the end, so "one escaped" is a failure and not a possibility
// nobody looked at.
//
// The recording under tests\fixtures\api is SYNTHETIC. A real one would be a
// hundred strangers' names and SteamID64s committed to a public repository,
// which is the same reason wrlines_data is gitignored at any depth. It is also
// a better fixture than a real one: the entries in it are the awkward cases --
// a name with tabs in it, a name that is only control characters, a null user,
// an entry with no replayHash, a leap day, an unparseable date -- and a real
// board is a hundred rows of nothing interesting.
//
// WHAT IS ACTUALLY AT RISK HERE
//
// Four things, and they are the four sections below:
//
//   - A URL that differs by one character asks a different question, and the
//     answer still looks like a leaderboard. Nothing downstream would notice.
//   - _epoch is strptime plus timegm plus their range checks, and getting it
//     wrong by an hour, a day or a leap year is a silently wrong date in every
//     row of every board.
//   - _clean is what stops a player's name becoming a replay hash. A tab that
//     survives shifts every column after it on that row.
//   - The written file has to be byte-identical to the reference's, which means
//     the meta lines in a fixed order, %.6f, CRLF, and a rank sort that is
//     STABLE over insertion order. That last one is the trap; test_board holds
//     the writer to it directly and the end-to-end run here proves the fetcher
//     feeds it in the right order.
//
// Build:  tests\build.bat
// Run:    tests\test_api.exe

#include "wr_api.h"
#include "wr_board.h"
#include "api_tape.h"

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

static void CheckStr(const char *got, const char *want, const char *what)
{
    bool ok = (got && want && strcmp(got, want) == 0);
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
    {
        g_failures++;
        printf("       want: %s\n       got:  %s\n", want ? want : "(null)",
               got ? got : "(null)");
    }
}

static void CheckI64(long long got, long long want, const char *what)
{
    bool ok = (got == want);
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
    {
        g_failures++;
        printf("       want: %lld\n       got:  %lld\n", want, got);
    }
}

// --- what the run printed ---------------------------------------------------

#define MAX_LINES 64
static char g_lines[MAX_LINES][256];
static int g_lineCount = 0;

static void Collect(const char *line)
{
    if (g_lineCount < MAX_LINES)
        strncpy_s(g_lines[g_lineCount], sizeof(g_lines[0]), line, _TRUNCATE);
    g_lineCount++;
}

static const char *Line(int i)
{
    return (i >= 0 && i < g_lineCount && i < MAX_LINES) ? g_lines[i] : "";
}

// --- files ------------------------------------------------------------------

static char *ReadText(const char *path, size_t *lenOut)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (lenOut)
        *lenOut = got;
    return buf;
}

// The whole file, compared as one string. Brittle on purpose: this is the
// artefact the reference also writes, and every byte of it is the subject.
static void CheckFile(const char *path, const char *want, const char *what)
{
    char *got = ReadText(path, NULL);
    bool ok = (got && strcmp(got, want) == 0);
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
    {
        g_failures++;
        printf("       ---- want ----\n%s\n       ---- got ----\n%s\n",
               want, got ? got : "(no file)");
    }
    free(got);
}

static void Wipe(const char *map, int mode, int type, int num)
{
    char path[MAX_PATH];
    WrBoardCachePath(path, sizeof(path), map, mode, type, num);
    DeleteFileA(path);
}

static int RunBoard(WrApiBoardArgs *a)
{
    g_lineCount = 0;
    return WrApiBoard(a, Collect, NULL, NULL);
}

int main(void)
{
    printf("\n=== wrlines leaderboard API ===\n");

    // Pinned so the "fetched" line is a constant. See WrNowEpoch: this is a
    // feature of the program and not only a test hook.
    SetEnvironmentVariableA("WRLINES_FAKE_NOW", "1700000000");

    // -----------------------------------------------------------------------
    printf("\nthe URL is the question, and one character changes it\n");
    {
        char url[4096];

        WrApiLeaderboardUrl(url, sizeof(url), 265, 1, 0, 1, 100, 0);
        CheckStr(url, "https://api.momentum-mod.org/v1/maps/265/leaderboard"
                      "?gamemode=1&trackType=0&trackNum=1&take=100&skip=0",
                 "a page of a leaderboard");

        WrApiLeaderboardUrl(url, sizeof(url), 265, 4, 2, 3, 20, 9106);
        CheckStr(url, "https://api.momentum-mod.org/v1/maps/265/leaderboard"
                      "?gamemode=4&trackType=2&trackNum=3&take=20&skip=9106",
                 "a bonus track, deep into the board");

        // max(0, skip) in the reference. A negative skip is a caller bug, and
        // asking the server for one is a 400 rather than a page.
        WrApiLeaderboardUrl(url, sizeof(url), 1, 1, 0, 1, 1, -5);
        Check(strstr(url, "&skip=0") != NULL, "a negative skip is clamped to 0");

        unsigned long long ids[3] = {76561198000000001ULL, 76561198000000002ULL,
                                     76561198000000003ULL};
        WrApiFriendsUrl(url, sizeof(url), 265, 1, 0, 1, ids, 3);
        CheckStr(url, "https://api.momentum-mod.org/v1/maps/265/leaderboard"
                      "?gamemode=1&trackType=0&trackNum=1&take=100&steamIDs="
                      "76561198000000001,76561198000000002,76561198000000003",
                 "friends are named, comma separated, and take is the page max");

        WrApiFriendsUrl(url, sizeof(url), 9, 1, 0, 1, ids, 1);
        Check(strstr(url, "steamIDs=76561198000000001") != NULL &&
              strstr(url, ",") == NULL,
              "one friend has no trailing comma");
    }

    // -----------------------------------------------------------------------
    printf("\n_epoch is strptime and timegm, range checks included\n");
    {
        CheckI64(WrApiEpoch("2026-03-16T09:40:41.194Z"), 1773654041LL,
                 "the shape the API actually sends");
        CheckI64(WrApiEpoch("2026-03-16T09:40:41"), 1773654041LL,
                 "exactly nineteen characters, nothing after them");
        CheckI64(WrApiEpoch("2024-02-29T00:00:00Z"), 1709164800LL,
                 "a leap day in a leap year");
        CheckI64(WrApiEpoch("2000-02-29T00:00:00Z"), 951782400LL,
                 "2000 is a leap year, because 400 divides it");
        CheckI64(WrApiEpoch("1900-02-29T00:00:00Z"), 0,
                 "1900 is not, because 100 does");
        CheckI64(WrApiEpoch("2023-02-29T00:00:00Z"), 0,
                 "a 29th of February in a year that has none");
        CheckI64(WrApiEpoch("2026-04-31T00:00:00Z"), 0,
                 "a 31st of April");
        CheckI64(WrApiEpoch("2026-13-01T00:00:00Z"), 0, "a thirteenth month");
        CheckI64(WrApiEpoch("2026-00-01T00:00:00Z"), 0, "a zeroth month");
        CheckI64(WrApiEpoch("2026-01-00T00:00:00Z"), 0, "a zeroth day");
        CheckI64(WrApiEpoch("2026-01-01T24:00:00Z"), 0, "a twenty-fifth hour");
        CheckI64(WrApiEpoch("2026-01-01T00:60:00Z"), 0, "a sixtieth minute");

        // %S's own regex is 6[0-1]|[0-5]\d|\d, so a leap second parses there
        // and therefore here. 62 does not.
        CheckI64(WrApiEpoch("2026-01-01T00:00:61Z"), 1767225661LL,
                 "a leap second, which strptime allows and so does this");
        CheckI64(WrApiEpoch("2026-01-01T00:00:62Z"), 0, "but not a 62nd second");

        CheckI64(WrApiEpoch("2026-3-16T09:40:41Z"), 0,
                 "a one-digit month leaves data unconverted, which is an error");
        CheckI64(WrApiEpoch("2026-03-16 09:40:41Z"), 0, "a space where the T is");
        CheckI64(WrApiEpoch("2026-03-16T09:40:4"), 0, "eighteen characters");
        CheckI64(WrApiEpoch(""), 0, "an empty string");
        CheckI64(WrApiEpoch(NULL), 0, "not a string at all");
        CheckI64(WrApiEpoch("0000-01-01T00:00:00Z"), 0,
                 "year zero, which datetime.date refuses");

        // The reason this is hand-rolled rather than _mkgmtime64, which answers
        // -1 here where the reference answers a negative number.
        CheckI64(WrApiEpoch("1969-12-31T23:59:59Z"), -1LL,
                 "one second before the epoch is -1, not an error");
    }

    // -----------------------------------------------------------------------
    printf("\nan alias has to survive being a field\n");
    {
        char out[192];

        WrApiClean(NULL, out, sizeof(out));
        CheckStr(out, "?", "not a string at all");
        WrApiClean("", out, sizeof(out));
        CheckStr(out, "?", "an empty name");
        WrApiClean("nova", out, sizeof(out));
        CheckStr(out, "nova", "an ordinary name");

        WrApiClean("two\ttabs\there", out, sizeof(out));
        CheckStr(out, "two tabs here", "tabs become spaces, which is the point");
        WrApiClean("a\nb\rc", out, sizeof(out));
        CheckStr(out, "a b c", "so do newlines and carriage returns");
        WrApiClean("\x01\x02\x1f", out, sizeof(out));
        CheckStr(out, "?", "a name of nothing but control characters");

        WrApiClean("   spaced   ", out, sizeof(out));
        CheckStr(out, "spaced", "str.strip() takes the ends off");
        WrApiClean("in ner", out, sizeof(out));
        CheckStr(out, "in ner", "and leaves the middle alone");

        // The reason IsPyStrippable exists. str.isspace() is a Unicode
        // predicate, so these come off in the reference and would not come off
        // in any implementation that assumed it meant ASCII.
        WrApiClean("\xc2\xa0nova\xc2\xa0", out, sizeof(out));
        CheckStr(out, "nova", "a non-breaking space is whitespace to Python");
        WrApiClean("\xe3\x80\x80nova\xe3\x80\x80", out, sizeof(out));
        CheckStr(out, "nova", "and so is an ideographic space");
        WrApiClean("\xe2\x80\x83nova", out, sizeof(out));
        CheckStr(out, "nova", "and an em space");
        WrApiClean("\xc2\xa0\xe3\x80\x80", out, sizeof(out));
        CheckStr(out, "?", "a name of nothing but those");

        // And the one that looks like it should be and is not.
        WrApiClean("\xe2\x80\x8bzwsp", out, sizeof(out));
        CheckStr(out, "\xe2\x80\x8bzwsp",
                 "a zero width space is NOT whitespace to Python");

        WrApiClean("k\xc3\xa4se", out, sizeof(out));
        CheckStr(out, "k\xc3\xa4se", "an accented name is left alone");
    }

    // -----------------------------------------------------------------------
    printf("\none page, read as the reference reads it\n");
    {
        size_t len = 0;
        char *page = ReadText("tests\\fixtures\\api\\0001.bin", &len);
        if (!page)
        {
            printf("  could not read tests\\fixtures\\api\\0001.bin\n");
            return 2;
        }

        WrBoardCacheRow rows[16];
        int entries = 0;
        long long total = 0;
        bool haveTotal = false;
        int kept = WrApiParsePage(page, len, rows, 16, &entries, &total, &haveTotal);
        free(page);

        CheckI64(kept, 5, "five of the six entries become rows");
        CheckI64(entries, 6,
                 "but the pager is told six, because that is how far it moved");
        Check(haveTotal && total == 9108, "totalCount came back as an integer");

        CheckStr(rows[0].alias, "nova", "an ordinary alias");
        CheckStr(rows[0].steamId, "76561198000000001",
                 "a SteamID64 arrives as a string and stays one");
        CheckI64(rows[0].epoch, 1773654041LL, "createdAt became an epoch");
        Check(rows[0].time > 37.1234559 && rows[0].time < 37.1234561,
              "the time is a double all the way through");

        CheckStr(rows[1].alias, "two tabs here", "a name with tabs in it");
        CheckStr(rows[2].alias, "padded", "a name padded with unicode spaces");
        CheckI64(rows[2].epoch, 0, "a createdAt that will not parse is 0");
        CheckStr(rows[2].url, "", "an empty downloadURL is an empty field");

        CheckStr(rows[3].steamId, "0", "a null user leaves the steamid at 0");
        CheckStr(rows[3].alias, "?", "and the alias at ?");
        CheckStr(rows[4].hash, "AAAA0006", "the entry with no user at all");
        CheckStr(rows[4].url, "",
                 "a missing downloadURL is the same as an empty one");

        // The one thing an entry is refused for.
        for (int i = 0; i < kept; i++)
            if (rows[i].rank == 5)
                Check(false, "an entry with no replayHash was kept");
        Check(true, "an entry with no replayHash is dropped");
    }

    // -----------------------------------------------------------------------
    if (!WrTapeOpen("tests\\fixtures\\api", false))
        return 2;
    WrTapeInstall();

    // -----------------------------------------------------------------------
    printf("\na window, written out\n");
    {
        Wipe("surf_synthetic", 1, 0, 1);

        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.map = "surf_synthetic";
        a.mapId = 700;              // given, so the catalogue is never read
        a.gamemode = 1;
        a.trackType = 0;
        a.trackNum = 1;
        a.mode = WR_BOARD_WINDOW;
        a.count = 6;

        CheckI64(RunBoard(&a), 0, "it succeeds");
        CheckStr(Line(0), "map surf_synthetic (id 700), surf, track 0/1",
                 "the first line names the board");
        CheckStr(Line(1), "taking ranks 1-6, which is 1 request",
                 "and then what it is about to ask for");
        // "rows returned" is what _fetch_window RETURNED, which is the records
        // it accepted -- five, not the six entries the page held. The raw six
        // is what the pager moved by, and the two numbers are used in two
        // different places by the reference. See the next section.
        CheckStr(Line(2), "1 request, 5 rows returned, 5 new; 5 of 9108 now cached",
                 "and what it got");

        char path[MAX_PATH];
        WrBoardCachePath(path, sizeof(path), "surf_synthetic", 1, 0, 1);
        CheckFile(path,
            "# WrLines leaderboard cache -- the windows you asked for, not the whole board.\r\n"
            "# map\tsurf_synthetic\r\n"
            "# mapid\t700\r\n"
            "# gamemode\t1\r\n"
            "# track\t0\t1\r\n"
            "# total\t9108\r\n"
            "# fetched\t1700000000\r\n"
            "# rank\ttime\tsteamid\talias\thash\tepoch\turl\r\n"
            "1\t37.123456\t76561198000000001\tnova\tAAAA0001\t1773654041\thttps://cdn.example/AAAA0001.mtv\r\n"
            "2\t38.500000\t76561198000000002\ttwo tabs here\taaaa0002\t1709164800\thttps://cdn.example/aaaa0002.mtv\r\n"
            "3\t39.000001\t76561198000000003\tpadded\tAAAA0003\t0\t\r\n"
            "4\t40.000000\t0\t?\tAAAA0004\t1773654041\thttps://cdn.example/AAAA0004.mtv\r\n"
            "6\t42.000000\t76561198000000006\t?\tAAAA0006\t1767225600\t\r\n",
            "every byte of the file the reference would have written");
    }

    // -----------------------------------------------------------------------
    printf("\na second window adds to the first rather than replacing it\n");
    {
        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.map = "surf_synthetic";
        a.mapId = 700;
        a.gamemode = 1;
        a.trackType = 0;
        a.trackNum = 1;
        a.mode = WR_BOARD_WINDOW;
        a.fromRank = 101;
        a.count = 150;

        CheckI64(RunBoard(&a), 0, "it succeeds");
        CheckStr(Line(1), "5 rows already cached; this adds to them",
                 "it says what it is adding to");
        CheckStr(Line(2), "taking ranks 101-250, which is 2 requests",
                 "150 places is two pages");
        CheckStr(Line(3), "2 requests, 2 rows returned, 1 new; 6 of 9108 now cached",
                 "one of the two rows was a re-fetch, so only one is new");

        // The page held THREE entries and yielded two rows, and the second
        // request asked for skip=103. Two things are pinned by that number.
        // It is not 200, because the pager advances by what came back and not
        // by what it asked for -- otherwise a short page asks for the same
        // places for ever. And it is not 102, because it advances by the
        // ENTRIES and not by the rows it kept -- otherwise the entry with no
        // replayHash is requested again on every page, for ever. The recording
        // holds skip=103 and nothing else, so either mistake is "not in the
        // recording" rather than a subtly different board.
        CheckI64(WrTapeRequests(), 3, "three requests so far, and no more");

        char path[MAX_PATH];
        WrBoardCachePath(path, sizeof(path), "surf_synthetic", 1, 0, 1);
        CheckFile(path,
            "# WrLines leaderboard cache -- the windows you asked for, not the whole board.\r\n"
            "# map\tsurf_synthetic\r\n"
            "# mapid\t700\r\n"
            "# gamemode\t1\r\n"
            "# track\t0\t1\r\n"
            "# total\t9108\r\n"
            "# fetched\t1700000000\r\n"
            "# rank\ttime\tsteamid\talias\thash\tepoch\turl\r\n"
            "1\t37.123456\t76561198000000001\tnova\tAAAA0001\t1773654041\thttps://cdn.example/AAAA0001.mtv\r\n"
            "2\t38.500000\t76561198000000002\ttwo tabs here\taaaa0002\t1709164800\thttps://cdn.example/aaaa0002.mtv\r\n"
            "3\t39.000001\t76561198000000003\tpadded\tAAAA0003\t0\t\r\n"
            "6\t43.000000\t76561198000000004\tfound later\taaaa0004\t1773654041\thttps://cdn.example/AAAA0004.mtv\r\n"
            "6\t42.000000\t76561198000000006\t?\tAAAA0006\t1767225600\t\r\n"
            "101\t50.000000\t76561198000000101\tlate\tBBBB0101\t1767225600\thttps://cdn.example/BBBB0101.mtv\r\n",
            "the re-fetched row keeps its place and the tie is broken by it");
    }

    // -----------------------------------------------------------------------
    printf("\na spread samples evenly, and rounds the way Python rounds\n");
    {
        Wipe("surf_spread", 1, 0, 1);

        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.map = "surf_spread";
        a.mapId = 701;
        a.gamemode = 1;
        a.trackType = 0;
        a.trackNum = 1;
        a.mode = WR_BOARD_SPREAD;
        a.spread = 3;

        // The whole point of this case. With a board of 10 and three samples,
        // the middle one is 1 + round((10-1) * 0.5) = 1 + round(4.5). Python's
        // round() is half-to-EVEN, so that is 4 and the rank is 5; C's round()
        // is half-away-from-zero, so it would be 5 and the rank 6. The
        // recording holds skip=4 and not skip=5, so the wrong rounding does not
        // produce a subtly different sample -- it produces "not in the
        // recording" and this test fails loudly.
        CheckI64(RunBoard(&a), 0, "it succeeds");
        CheckStr(Line(1), "sampling 3 places across 10 runs",
                 "it says what it is sampling");
        CheckStr(Line(2), "3 requests, 3 rows returned, 3 new; 3 of 10 now cached",
                 "one request per sample, the first doubling as the probe");

        char path[MAX_PATH];
        WrBoardCachePath(path, sizeof(path), "surf_spread", 1, 0, 1);
        char *got = ReadText(path, NULL);
        Check(got && strstr(got, "\tCCCC0005\t") != NULL,
              "the middle sample is rank 5, so round() was half-to-even");
        free(got);
    }

    // -----------------------------------------------------------------------
    printf("\nthe slowest end costs two requests, and the probe is thrown away\n");
    {
        Wipe("surf_slowest", 1, 0, 1);

        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.map = "surf_slowest";
        a.mapId = 703;
        a.gamemode = 1;
        a.trackType = 0;
        a.trackNum = 1;
        a.mode = WR_BOARD_SLOWEST;
        a.count = 50;

        CheckI64(RunBoard(&a), 0, "it succeeds");
        CheckStr(Line(1), "the board holds 4 runs; taking ranks 1-4",
                 "a window wider than the board starts at rank 1");
        CheckStr(Line(2), "2 requests, 4 rows returned, 4 new; 4 of 4 now cached",
                 "the probe plus one window");

        char path[MAX_PATH];
        WrBoardCachePath(path, sizeof(path), "surf_slowest", 1, 0, 1);
        char *got = ReadText(path, NULL);
        Check(got && strstr(got, "PROBE001") == NULL,
              "the probe's own row never reaches the file");
        Check(got && strstr(got, "\tEEEE0004\t") != NULL,
              "the window's rows do");
        free(got);
    }

    // -----------------------------------------------------------------------
    printf("\nfriends are looked up by SteamID64, which needs no account\n");
    {
        Wipe("surf_friends", 1, 0, 1);

        FILE *f = NULL;
        if (fopen_s(&f, WrDataPath("friends.txt"), "w") == 0 && f)
        {
            fprintf(f, "# WrLines: SteamID64s from your Steam friends list.\n");
            fprintf(f, "76561198000000001\n");
            fprintf(f, "\n");
            fprintf(f, "not a number\n");
            fprintf(f, "-5\n");
            fprintf(f, "0\n");
            fprintf(f, "76561198000000009  a trailing comment token\n");
            fclose(f);
        }

        unsigned long long ids[16];
        char fpath[MAX_PATH];
        int n = WrApiReadFriends(ids, 16, fpath, sizeof(fpath));
        CheckI64(n, 2, "blank lines, comments, words, zero and negatives all go");
        CheckI64((long long)ids[1], 76561198000000009LL,
                 "and only the first whitespace-delimited token is read");

        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.map = "surf_friends";
        a.mapId = 702;
        a.gamemode = 1;
        a.trackType = 0;
        a.trackNum = 1;
        a.mode = WR_BOARD_FRIENDS;

        CheckI64(RunBoard(&a), 0, "it succeeds");
        CheckStr(Line(1), "2 friends to look up, 1 request",
                 "a hundred friends per request, so two is one");
        CheckStr(Line(2), "1 of them have a run on this track",
                 "the other has never touched the map, which is not an error");

        char path[MAX_PATH];
        WrBoardCachePath(path, sizeof(path), "surf_friends", 1, 0, 1);
        char *got = ReadText(path, NULL);
        Check(got && strstr(got, "4500\t99.250000\t76561198000000009\ta friend\t"
                                 "DDDD4500\t") != NULL,
              "a friend at rank 4500 is found without caching the 4499 above");
        // total stays out of the file: a friends lookup learns nothing about
        // how big the board is, so the reference leaves it as None.
        Check(got && strstr(got, "# total") == NULL,
              "and the board's size is still unknown, so no total line");
        free(got);
    }

    // -----------------------------------------------------------------------
    printf("\na URL not in the recording is an error, not a fetch\n");
    {
        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.map = "surf_nothing";
        a.mapId = 999;
        a.gamemode = 1;
        a.trackType = 0;
        a.trackNum = 1;
        a.mode = WR_BOARD_WINDOW;
        a.count = 6;

        CheckI64(RunBoard(&a), 1, "it fails");
        Check(strncmp(Line(2), "[!] leaderboard request failed: not in the "
                               "recording: ", 48) == 0,
              "with the reference's own wording for a replay miss");
    }

    // -----------------------------------------------------------------------
    printf("\nnothing went to the network\n");
    {
        // Nine recorded URLs, and the run above that asked for a tenth. If a
        // request had escaped the tape this number would be short, because the
        // tape is the only thing counting.
        CheckI64(WrTapeRequests(), 10, "every request went through the tape");
    }

    WrTapeClose();

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
