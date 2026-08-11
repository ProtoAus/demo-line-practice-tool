// test_peek.cpp  --  the demo index, and the four ways a cached answer goes bad.
//
// wr_peek.cpp exists to stop the extractor opening every .mtv in the library
// twice, which is where the twenty-second silence at the start of a run came
// from. The whole of its value rests on one judgement -- that (path, size,
// last-write-time) is enough to know an answer is still true -- so that is what
// most of this file is about.
//
// WHY THE CASES ARE ABOUT STALENESS RATHER THAN ABOUT HITS
//
// A cache that returns a wrong answer is worse than no cache. The failure is
// silent and it is durable: a demo whose header says surf_a, remembered as
// surf_b, is a run that never appears on the map it belongs to and never says
// why, on every run, until the file is deleted by hand. A cache that merely
// MISSES is only slow. So the interesting cases below are the ones where the
// file changed underneath a row, and each of them has to come back as a miss.
//
// THESE TOUCH THE DISK, ON PURPOSE
//
// Every other harness here is pure. This one cannot be: the thing under test is
// a claim about file metadata, and a stub that returned metadata we invented
// would be a test of the stub. So it writes real files into a scratch folder
// under tests\ and deletes them again. The demo it writes is the synthetic
// fixture -- nobody's run -- and .gitignore already covers *.mtv anywhere for
// exactly this reason.
//
// Build:  tests\build.bat
// Run:    tests\test_peek.exe

#include "wr_peek.h"
#include "wr_mtv.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_mtv.h"

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static const char *kMapName = "surf_fixture";

// ---------------------------------------------------------------------------
// Scratch files
// ---------------------------------------------------------------------------

static char g_dir[MAX_PATH];

static void MakeDir(void)
{
    _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "tests\\peekscratch");
    CreateDirectoryA(g_dir, NULL);
}

static void PathIn(char *out, int cap, const char *name)
{
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s\\%s", g_dir, name);
}

// Write `n` bytes of the fixture. Fewer than all of them makes a file that is
// still an MMTV by its magic but is a different size, which is one of the
// staleness cases.
static bool WriteDemo(const char *path, size_t n)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    fwrite(kMtvV1, 1, n, f);
    fclose(f);
    return true;
}

static bool StatOf(const char *path, long long *size, long long *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &ad))
        return false;
    *size = ((long long)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
    *mtime = ((long long)ad.ftLastWriteTime.dwHighDateTime << 32) |
             ad.ftLastWriteTime.dwLowDateTime;
    return true;
}

// Move a file's clock without touching a byte of it. The one case that size
// alone cannot catch: a re-download that happened to produce the same length.
static bool Backdate(const char *path, long long *mtimeOut)
{
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    FILETIME ft;
    SYSTEMTIME st;
    GetSystemTime(&st);
    st.wYear = (WORD)(st.wYear - 1);
    SystemTimeToFileTime(&st, &ft);
    const bool ok = SetFileTime(h, NULL, NULL, &ft) != 0;
    CloseHandle(h);

    if (ok && mtimeOut)
        *mtimeOut = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return ok;
}

static void Cleanup(void)
{
    char p[MAX_PATH];
    PathIn(p, sizeof(p), "a.mtv");     DeleteFileA(p);
    PathIn(p, sizeof(p), "b.mtv");     DeleteFileA(p);
    PathIn(p, sizeof(p), "junk.mtv");  DeleteFileA(p);
    RemoveDirectoryA(g_dir);
}

// ---------------------------------------------------------------------------

int main(void)
{
    printf("\n=== wrlines demo index ===\n\n");

    MakeDir();

    // Start from nothing, whatever a previous run left behind. WrPeekForget
    // deletes the file as well as the table, which is what makes the first
    // section below a genuine first sight rather than a hit from last time.
    WrPeekForget();

    char a[MAX_PATH], b[MAX_PATH], junk[MAX_PATH];
    PathIn(a, sizeof(a), "a.mtv");
    PathIn(b, sizeof(b), "b.mtv");
    PathIn(junk, sizeof(junk), "junk.mtv");

    if (!WriteDemo(a, sizeof(kMtvV1)))
    {
        printf("  could not write the scratch demo; is tests\\ writable?\n");
        return 1;
    }

    long long size = 0, mtime = 0;
    if (!StatOf(a, &size, &mtime))
    {
        printf("  could not stat the scratch demo\n");
        Cleanup();
        return 1;
    }

    printf("a demo it has never seen\n");
    {
        char map[65] = "";
        bool ok = false;
        const bool read = WrPeekMapOf(a, size, mtime, map, sizeof(map), &ok);
        Check(read, "is read off the disk, because there is nowhere else");
        Check(strcmp(map, kMapName) == 0, "and it says which map it is for");
        Check(ok, "with a header that parsed");
        Check(WrPeekCount() == 1, "and it is remembered");
    }
    printf("\n");

    printf("and the same demo a second time\n");
    {
        char map[65] = "";
        bool ok = false;
        const bool read = WrPeekMapOf(a, size, mtime, map, sizeof(map), &ok);
        Check(!read, "is not opened at all -- which is the entire point");
        Check(strcmp(map, kMapName) == 0, "and gives the same answer");
        Check(ok, "including that the header was fine");
        Check(WrPeekCount() == 1, "without a second row");
    }
    printf("\n");

    printf("the same path, spelled differently\n");
    {
        // Windows does not distinguish these and neither may we, or one demo
        // reached through two spellings becomes two rows that can disagree.
        char upper[MAX_PATH];
        strcpy_s(upper, sizeof(upper), a);
        for (char *p = upper; *p; p++)
        {
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            else if (*p == '\\')        *p = '/';
        }

        char map[65] = "";
        const bool read = WrPeekMapOf(upper, size, mtime, map, sizeof(map), NULL);
        Check(!read, "is the row we already have, not a new one");
        Check(WrPeekCount() == 1, "so the table still holds one demo");
    }
    printf("\n");

    printf("a file that changed size under a row we trusted\n");
    {
        // Truncated to something still long enough to be a demo. The remembered
        // answer would have been right about the map and wrong about the file.
        Check(WriteDemo(a, 0x400), "the demo is rewritten shorter");

        long long size2 = 0, mtime2 = 0;
        Check(StatOf(a, &size2, &mtime2), "and stats again");
        Check(size2 != size, "at a different size");

        char map[65] = "";
        const bool read = WrPeekMapOf(a, size2, mtime2, map, sizeof(map), NULL);
        Check(read, "so it is read again rather than believed");

        size = size2;
        mtime = mtime2;
    }
    printf("\n");

    printf("a file whose clock moved but whose length did not\n");
    {
        // The case size alone cannot see. A re-download that produced the same
        // number of bytes is still a different file, and the only evidence left
        // is the write time.
        long long moved = 0;
        Check(Backdate(a, &moved), "the demo is backdated a year, byte for byte");
        Check(moved != mtime, "so its write time is not the one we recorded");

        char map[65] = "";
        const bool read = WrPeekMapOf(a, size, moved, map, sizeof(map), NULL);
        Check(read, "and that alone is enough to make it read again");
        mtime = moved;
    }
    printf("\n");

    printf("something that is not a demo at all\n");
    {
        FILE *f = NULL;
        if (fopen_s(&f, junk, "wb") == 0 && f)
        {
            fputs("this is not an MMTV file", f);
            fclose(f);
        }

        long long js = 0, jm = 0;
        Check(StatOf(junk, &js, &jm), "a text file is written with an .mtv name");

        char map[65] = "zzz";
        bool ok = true;
        const bool read = WrPeekMapOf(junk, js, jm, map, sizeof(map), &ok);
        Check(read, "is read, since nothing is known about it yet");
        Check(map[0] == '\0', "and names no map");
        Check(!ok, "and says its header did not parse");

        // The part worth having a case for: a refusal is an ANSWER and is
        // remembered like any other. Re-opening every unreadable file on every
        // walk is exactly the cost this module exists to remove, and a library
        // with a few hundred of them is not unusual.
        char map2[65] = "zzz";
        bool ok2 = true;
        const bool read2 = WrPeekMapOf(junk, js, jm, map2, sizeof(map2), &ok2);
        Check(!read2, "and is not opened a second time to be told again");
        Check(map2[0] == '\0' && !ok2, "with the refusal remembered intact");
    }
    printf("\n");

    printf("across a save and a reload\n");
    {
        const int before = WrPeekCount();
        WrPeekSave();
        WrPeekShutdown();           // drops the table, keeps the file
        Check(WrPeekCount() == 0, "the table is empty once it has been dropped");

        char map[65] = "";
        bool ok = false;
        const bool read = WrPeekMapOf(a, size, mtime, map, sizeof(map), &ok);
        Check(!read, "and the demo is still not opened after a restart");
        Check(strcmp(map, kMapName) == 0, "because the file on disk knew it");
        Check(ok, "header verdict and all");
        Check(WrPeekCount() == before, "with every row back");
    }
    printf("\n");

    printf("and a save that has nothing to say writes nothing\n");
    {
        // A run over a fully indexed library must not rewrite a megabyte of
        // file for no reason. Loading is not a change; only a Put is.
        char path[MAX_PATH];
        strcpy_s(path, sizeof(path), WrDataPath("demoindex.txt"));

        WIN32_FILE_ATTRIBUTE_DATA before, after;
        Check(GetFileAttributesExA(path, GetFileExInfoStandard, &before) != 0,
              "the index is on disk to begin with");

        Sleep(30);
        WrPeekSave();

        Check(GetFileAttributesExA(path, GetFileExInfoStandard, &after) != 0 &&
              before.ftLastWriteTime.dwLowDateTime ==
                  after.ftLastWriteTime.dwLowDateTime &&
              before.ftLastWriteTime.dwHighDateTime ==
                  after.ftLastWriteTime.dwHighDateTime,
              "and an idle save leaves it untouched");
    }
    printf("\n");

    printf("two demos, told apart\n");
    {
        Check(WriteDemo(b, sizeof(kMtvV1)), "a second demo is written");

        long long bs = 0, bm = 0;
        Check(StatOf(b, &bs, &bm), "and stats");

        char map[65] = "";
        const bool read = WrPeekMapOf(b, bs, bm, map, sizeof(map), NULL);
        Check(read, "it is read, being new");
        Check(strcmp(map, kMapName) == 0, "and is for the same map as the first");
        Check(WrPeekCount() == 3, "so the table now holds three files");
    }
    printf("\n");

    printf("forgetting\n");
    {
        WrPeekForget();
        Check(WrPeekCount() == 0, "empties the table");

        char path[MAX_PATH];
        strcpy_s(path, sizeof(path), WrDataPath("demoindex.txt"));
        Check(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES,
              "and takes the file with it, so nothing survives to mislead");
    }
    printf("\n");

    Cleanup();
    WrPeekShutdown();

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
