// test_fetch.cpp  --  downloading demos: what to ask for, and where it lands.
//
// Three things here have no other way of being noticed.
//
// parse_ranks reads a string a USER TYPED, so the two implementations have to
// agree on what a typo means as well as on what a correct selection means. Its
// quirks are not tidy and they are all reproduced: a leading '-' is a sign and
// not a range separator, a reversed range is swapped rather than dropped, and
// a range is capped four thousand and ninety-six places FROM ITS START, so
// "1-999999" is 1..4097 and nobody is told.
//
// The dedupe is what keeps a fetch cheap. It has one edge that is easy to get
// backwards: a row with no downloadURL is counted in the "already here" number
// alongside the ones we really do hold, because the reference computes that
// figure as len(rows) - len(todo) and todo drops both.
//
// And the --into-game copy has to carry the SOURCE'S WRITE TIME. Nothing looks
// wrong when that breaks -- the download works, the demo plays, the lines
// appear. What stops working is the removal button, quietly, for every file
// copied after the day it broke, because wr_intogame.h decides what is ours by
// matching size AND write time. So it gets a test with a deliberately backdated
// source, which is the only way to tell a carried timestamp from a fresh one
// that happens to be in the same second.
//
// Build:  tests\build.bat
// Run:    tests\test_fetch.exe

#include "wr_fetch.h"

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

// A selection, rendered so a mismatch says what it actually was.
static void CheckRanks(const char *spec, const char *want)
{
    int v[8192];
    int n = WrFetchParseRanks(spec, v, 8192);

    char got[256];
    int used = 0;
    got[0] = '\0';
    for (int i = 0; i < n && used < (int)sizeof(got) - 16; i++)
        used += _snprintf_s(got + used, sizeof(got) - used, _TRUNCATE,
                            "%s%d", i ? "," : "", v[i]);
    if (used >= (int)sizeof(got) - 16)
        _snprintf_s(got + used, sizeof(got) - used, _TRUNCATE, ",...(%d)", n);

    bool ok = (strcmp(got, want) == 0);
    printf("  %-24s -> %-31s %s\n", spec, got, ok ? "ok" : "FAILED");
    if (!ok)
    {
        g_failures++;
        printf("       want: %s\n", want);
    }
}

int main(void)
{
    printf("\n=== wrlines fetching ===\n");

    // -----------------------------------------------------------------------
    printf("\nwhat a typed selection means\n");
    {
        CheckRanks("5", "5");
        CheckRanks("5,9,7", "5,7,9");
        CheckRanks("120-124", "120,121,122,123,124");
        CheckRanks("5,9,120-123", "5,9,120,121,122,123");
        CheckRanks(" 5 , 9 ", "5,9");
        CheckRanks("5,5,5", "5");
        CheckRanks("", "");
        CheckRanks(",,", "");
        CheckRanks("banana", "");
        CheckRanks("5,banana,9", "5,9");
        CheckRanks("3-", "");
        CheckRanks("-", "");

        // A reversed range is swapped, not dropped.
        CheckRanks("124-120", "120,121,122,123,124");

        // '-' is only a separator from index 1 onward, so this is one negative
        // number and not a range with an empty left half. It parses, which is
        // why it comes back at all -- the fetch path is what discards a rank
        // that cannot exist.
        CheckRanks("-5", "-5");
        CheckRanks("-5,-3", "-5,-3");
        CheckRanks("-5-3", "-5,-4,-3,-2,-1,0,1,2,3");
    }

    // -----------------------------------------------------------------------
    printf("\nthe range cap is counted from the start, and is silent\n");
    {
        // range(lo, min(hi, lo + 4096) + 1) -- so 4097 places, and the number
        // to check is the LAST one rather than how many there are. A cap on the
        // width would give 1..4096 and a cap on the count would give the same;
        // only this reading gives 4097.
        int *v = (int *)malloc(sizeof(int) * 8192);
        int n = WrFetchParseRanks("1-999999", v, 8192);
        Check(n == 4097, "\"1-999999\" is 4097 places");
        Check(v[0] == 1 && v[n - 1] == 4097, "1 to 4097 inclusive");

        n = WrFetchParseRanks("1000-999999", v, 8192);
        Check(n == 4097 && v[0] == 1000 && v[n - 1] == 5096,
              "and from 1000 it is 1000 to 5096, not 1 to 4097");
        free(v);
    }

    // -----------------------------------------------------------------------
    printf("\na selection too big for a command line, from a file\n");
    {
        const char *path = "tests\\_ranks_tmp.txt";
        FILE *f = NULL;
        fopen_s(&f, path, "wb");
        fprintf(f, "# the places ticked in the Board tab\n");
        fprintf(f, "5\n");
        fprintf(f, "\n");
        fprintf(f, "9\n");
        fprintf(f, "   # an indented comment\n");
        fprintf(f, "12,14-16\n");        // a line may itself be a spec
        fclose(f);

        int v[64];
        char err[128] = "";
        int n = WrFetchParseRanksFile(path, v, 64, err, sizeof(err));
        Check(n == 6, "six places out of five lines");
        Check(v[0] == 5 && v[1] == 9 && v[2] == 12 && v[3] == 14 &&
              v[4] == 15 && v[5] == 16, "and they are the right six");

        n = WrFetchParseRanksFile("tests\\_no_such_selection.txt", v, 64, err,
                                  sizeof(err));
        Check(n == -1, "a missing file is -1, not an empty selection");
        Check(err[0] != '\0', "and it says so");
        remove(path);
    }

    // -----------------------------------------------------------------------
    printf("\nwhat is already on disk is a hash lookup, not a request\n");
    {
        WrFetchHeld h;

        // Over a tree we control: a temporary game directory with one demo
        // several levels down, because the reference walks these RECURSIVELY
        // and a flat scan of momtv\ would find nothing at all.
        char root[MAX_PATH];
        _snprintf_s(root, sizeof(root), _TRUNCATE, "tests\\_fetch_game");
        char deep[MAX_PATH];
        _snprintf_s(deep, sizeof(deep), _TRUNCATE, "%s\\momentum\\momtv\\online\\265", root);
        CreateDirectoryA("tests\\_fetch_game", NULL);
        CreateDirectoryA("tests\\_fetch_game\\momentum", NULL);
        CreateDirectoryA("tests\\_fetch_game\\momentum\\momtv", NULL);
        CreateDirectoryA("tests\\_fetch_game\\momentum\\momtv\\online", NULL);
        CreateDirectoryA(deep, NULL);

        char demo[MAX_PATH];
        _snprintf_s(demo, sizeof(demo), _TRUNCATE, "%s\\ABCD1234.mtv", deep);
        FILE *f = NULL;
        fopen_s(&f, demo, "wb");
        fwrite("MMTV", 1, 4, f);
        fclose(f);

        // And the SAME hash in our own tree, which is what --into-game leaves
        // behind: one run, two files. The reference counts hashes in a set and
        // prints how many there are, so a list would report a number that grew
        // every time somebody copied a demo they already had.
        char mine[MAX_PATH];
        _snprintf_s(mine, sizeof(mine), _TRUNCATE, "%s\\_fetchtest",
                    WrDataPath("demos"));
        CreateDirectoryA(WrDataPath("demos"), NULL);
        CreateDirectoryA(mine, NULL);
        char dup[MAX_PATH];
        _snprintf_s(dup, sizeof(dup), _TRUNCATE, "%s\\abcd1234.mtv", mine);
        fopen_s(&f, dup, "wb");
        fwrite("MMTV", 1, 4, f);
        fclose(f);

        // Two of the player's OWN recordings, under the long names the game
        // gives those, differing only at character 50. A fixed-width key wide
        // enough for a 40-character replay hash merges them, and the visible
        // consequence of a false "we already have that" is a demo that never
        // downloads and never says why.
        const char *longA = "surf_utopia_bonus1_2024_08_11_kaboom_attempt_00147_a";
        const char *longB = "surf_utopia_bonus1_2024_08_11_kaboom_attempt_00147_b";
        for (int i = 0; i < 2; i++)
        {
            char p[MAX_PATH];
            _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s.mtv", deep,
                        i ? longB : longA);
            fopen_s(&f, p, "wb");
            fwrite("MMTV", 1, 4, f);
            fclose(f);
        }

        WrFetchHeldBuild(&h, root);
        Check(h.n >= 1, "the walk found it, several directories down");
        Check(WrFetchHeldHas(&h, longA) && WrFetchHeldHas(&h, longB),
              "two long names alike for 50 characters stay two demos");
        Check(WrFetchHeldHas(&h, "ABCD1234"), "and it is held");
        Check(WrFetchHeldHas(&h, "abcd1234"),
              "under any case, because the reference keys on hash.lower()");
        Check(!WrFetchHeldHas(&h, "ABCD1235"), "a run we do not have is not");

        int copies = 0;
        for (int i = 0; i < h.n; i++)
            if (strcmp(h.hash[i], "abcd1234") == 0)
                copies++;
        Check(copies == 1, "in both trees at once, it is still one demo");
        WrFetchHeldFree(&h);

        DeleteFileA(dup);
        RemoveDirectoryA(mine);
        for (int i = 0; i < 2; i++)
        {
            char p[MAX_PATH];
            _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s.mtv", deep,
                        i ? longB : longA);
            DeleteFileA(p);
        }
        DeleteFileA(demo);
        RemoveDirectoryA(deep);
        RemoveDirectoryA("tests\\_fetch_game\\momentum\\momtv\\online");
        RemoveDirectoryA("tests\\_fetch_game\\momentum\\momtv");
        RemoveDirectoryA("tests\\_fetch_game\\momentum");
        RemoveDirectoryA("tests\\_fetch_game");
    }

    // -----------------------------------------------------------------------
    printf("\nthe copy into the game's folder carries the source's write time\n");
    {
        // The one contract in this file whose failure has no symptom. See
        // wr_fetch.h, and ADOPTION in wr_intogame.h: a file in the game's
        // replay folder is recognised as ours by matching size AND write time,
        // because the same run can also be downloaded by the game afterwards
        // and then "we hold that hash" is true of a file that is not ours.
        const char *src = "tests\\_copy_src.mtv";
        const char *dst = "tests\\_copy_dst.mtv";
        DeleteFileA(dst);

        FILE *f = NULL;
        fopen_s(&f, src, "wb");
        fwrite("MMTV and then some bytes", 1, 24, f);
        fclose(f);

        // Backdated a year, deliberately. A copy that quietly took a FRESH
        // timestamp would still pass a test written against a source made a
        // moment ago -- both would land in the same second and look identical.
        // Only a source that could not possibly share a timestamp with "now"
        // tells the two apart.
        SYSTEMTIME st;
        memset(&st, 0, sizeof(st));
        st.wYear = 2019; st.wMonth = 7; st.wDay = 4;
        st.wHour = 13; st.wMinute = 37; st.wSecond = 11;
        FILETIME want;
        SystemTimeToFileTime(&st, &want);
        {
            HANDLE h = CreateFileA(src, FILE_WRITE_ATTRIBUTES, 0, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            Check(h != INVALID_HANDLE_VALUE && SetFileTime(h, NULL, NULL, &want),
                  "a source backdated to 2019");
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }

        char err[128] = "";
        Check(WrFetchCopyWithTime(src, dst, err, sizeof(err)),
              "the copy succeeds");

        WIN32_FILE_ATTRIBUTE_DATA a, b;
        bool gotA = GetFileAttributesExA(src, GetFileExInfoStandard, &a) != 0;
        bool gotB = GetFileAttributesExA(dst, GetFileExInfoStandard, &b) != 0;
        Check(gotA && gotB, "both files are there afterwards");
        if (gotA && gotB)
        {
            Check(b.nFileSizeLow == a.nFileSizeLow, "the same size");
            Check(CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0,
                  "and the same write time, to the tick");

            // What would have happened without the SetFileTime: the
            // destination would carry now. Stated as its own check so the
            // failure reads as "it took a fresh stamp" rather than as an
            // opaque inequality.
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER n, w;
            n.LowPart = now.dwLowDateTime;   n.HighPart = now.dwHighDateTime;
            w.LowPart = b.ftLastWriteTime.dwLowDateTime;
            w.HighPart = b.ftLastWriteTime.dwHighDateTime;
            Check(n.QuadPart - w.QuadPart > 10000000ULL * 86400ULL,
                  "which is a year ago and not a moment ago");
        }

        // And the temp file it goes through does not survive.
        Check(GetFileAttributesA("tests\\_copy_dst.mtv.tmp") ==
              INVALID_FILE_ATTRIBUTES,
              "the temp file it lands through is gone");

        DeleteFileA(src);
        DeleteFileA(dst);
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
