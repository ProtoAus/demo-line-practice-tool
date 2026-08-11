// test_wrpath.cpp  --  the file format, through both of its implementations.
//
// There were two statements of this format: write_wrpath in the reference and
// LoadOne in wr_path.cpp. The reference has stopped shipping, so there are two
// again -- WrPathWrite and LoadOne -- and they are now in the same file for the
// reason this harness exists to enforce: a format whose halves can drift will.
//
// Nothing tested LoadOne before the port. It had no harness at all, because
// there was no way to produce a file without running Python. There is now, so
// this writes one with the real writer and reads it back with the REAL loader,
// which means every offset in the header is asserted twice from opposite ends
// and a disagreement about any of them cannot pass.
//
// Four things it pins that a round trip alone would not:
//
//   THE UNALIGNED DOUBLE AT 0x1C. The run time is eight bytes at an offset
//   divisible by four and not by eight. Both sides memcpy rather than cast,
//   and a compiler that decided otherwise on some future target would fault
//   rather than mis-read -- but the value being right is what is checked.
//
//   THE CRC COVERS THE WHOLE FILE, header included. A file with one byte
//   changed anywhere is refused, not read leniently.
//
//   t = index * tick_interval, IN DOUBLE, rounded once on the way into the
//   file. Accumulating a float drifts, and the drift is invisible until a run
//   is long enough for it to matter.
//
//   THE FIXED-WIDTH FIELDS GO THROUGH A UTF-8 DECODE. The reference reads a
//   player name out of a demo with errors="replace" and writes it back with
//   encode(), so an invalid byte has become U+FFFD -- three bytes -- by the
//   time it reaches the file. That is reachable on real data: the name field
//   in a .mtv is 32 bytes and the game truncates into it, so a name ending in
//   a multi-byte character arrives cut in half.
//
// And one that is the whole reason WRLINES_FAKE_NOW exists: writing the same
// input twice produces the same bytes. The header carries a timestamp at 0xF4
// and the CRC covers it, so without pinning the clock two runs of the same
// implementation over the same demos differ -- and then "did the port write the
// same file" needs a tool that knows to skip four bytes and recompute a CRC,
// which is one more thing that can be wrong.
//
// Build:  tests\build.bat
// Run:    tests\test_wrpath.exe

#include "wr_dp.h"
#include "wr_path.h"
#include "wr_extract.h"
#include "wr_log.h"

#include <math.h>
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

#define TEST_MAP "wrlines_test_map"

static const double kTick = 0.015;
static const int kPoints = 300;

static void BuildPoints(WrDpPoint *p)
{
    for (int i = 0; i < kPoints; i++)
    {
        const double t = i * 0.02;
        p[i].x = 2000.0 + 300.0 * cos(t);
        p[i].y = 3000.0 + 300.0 * sin(t);
        p[i].z = 1500.0 + 5.0 * i;
        p[i].vx = -300.0 * sin(t);
        p[i].vy = 300.0 * cos(t);
        p[i].vz = 5.0 / kTick;
    }
}

static void FillArgs(WrPathWriteArgs *a, const char *outPath,
                     const WrDpPoint *pts, const WrPathWriteMarker *mk, int nmk)
{
    memset(a, 0, sizeof(*a));
    a->outPath = outPath;
    a->tickInterval = (float)kTick;
    a->runTime = 61.25;
    a->steamid64 = 0x0110000112345678ULL;
    a->dateMs = 1717171717171LL;
    a->map = TEST_MAP;
    a->mapHash = "0123456789abcdef0123456789abcdef01234567";
    a->srcSha1 = "fedcba9876543210fedcba9876543210fedcba98";
    a->player = "a player";
    a->flags = WRPATH_FLAG_HAS_VELOCITY | WRPATH_FLAG_FROM_EXTRACTOR
             | WRPATH_FLAG_MARKERS_OK;
    a->gamemode = 1;
    a->trackType = 2;
    a->trackNum = 3;
    a->startIndex = 41;
    a->startOk = true;
    a->points = pts;
    a->pointCount = kPoints;
    a->markers = mk;
    a->markerCount = nmk;
}

static unsigned char *Slurp(const char *path, size_t *lenOut)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n);
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

static unsigned int U32At(const unsigned char *b, int off)
{
    unsigned int v;
    memcpy(&v, b + off, 4);
    return v;
}

int main(void)
{
    printf("\n=== wrlines .wrpath writer and loader ===\n");

    // Pinned so the header's timestamp is a constant. Set before anything is
    // written; WrNowEpoch reads the environment on every call.
    SetEnvironmentVariableA("WRLINES_FAKE_NOW", "1700000000");

    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), WrDataPath("paths\\" TEST_MAP));
    WrMakeTree(dir);

    char out[MAX_PATH];
    _snprintf_s(out, sizeof(out), _TRUNCATE, "%s\\demo0001.wrpath", dir);

    WrDpPoint *pts = (WrDpPoint *)malloc(sizeof(WrDpPoint) * kPoints);
    BuildPoints(pts);

    WrPathWriteMarker mk[3];
    memset(mk, 0, sizeof(mk));
    for (int i = 0; i < 3; i++)
    {
        mk[i].pointIndex = (unsigned int)(50 + 80 * i);
        mk[i].segment = (unsigned short)i;
        mk[i].minorNum = (unsigned short)(i + 1);
        mk[i].timeReached = 12.5 + 20.0 * i;
        mk[i].vx = 111.5f + i;
        mk[i].vy = -222.25f;
        mk[i].vz = 33.75f;
        mk[i].maxSpeed = 1234.5f;
    }

    // -----------------------------------------------------------------------
    printf("\nwhat the writer puts where\n");
    {
        WrPathWriteArgs a;
        FillArgs(&a, out, pts, mk, 3);
        char err[256] = "";
        const long long wrote = WrPathWrite(&a, err, sizeof(err));
        if (wrote < 0)
            printf("     %s\n", err);
        Check(wrote > 0, "a file is written");

        size_t len = 0;
        unsigned char *b = Slurp(out, &len);
        Check(b != NULL && (long long)len == wrote,
              "and it is exactly as long as the writer said");
        if (!b)
        {
            printf("\nSOME CHECKS FAILED\n\n");
            return 1;
        }

        Check(memcmp(b, "WRPATH\0\0", 8) == 0, "magic at 0");
        Check(U32At(b, 0x08) == 1, "version 1 at 0x08");
        Check(U32At(b, 0x10) == (unsigned)kPoints, "point count at 0x10");
        Check(U32At(b, 0x14) == 3, "marker count at 0x14");

        float dt;
        memcpy(&dt, b + 0x18, 4);
        Check(dt == (float)kTick, "tick interval at 0x18");

        double rt;
        memcpy(&rt, b + 0x1C, 8);       // NOT eight-aligned, hence the memcpy
        Check(rt == 61.25, "the run time is a double at 0x1C, unaligned");

        Check(memcmp(b + 0x24, &a.steamid64, 8) == 0, "steamid64 at 0x24");
        Check(memcmp(b + 0x2C, &a.dateMs, 8) == 0, "date at 0x2C");
        Check(strcmp((const char *)b + 0x34, TEST_MAP) == 0, "map at 0x34");
        Check(strcmp((const char *)b + 0x74, a.mapHash) != 0 &&
              memcmp(b + 0x74, a.mapHash, 39) == 0 && b[0x74 + 39] == 0,
              "the 40-char map hash is cut to 39 and NUL-terminated");
        Check(strcmp((const char *)b + 0xC4, "a player") == 0, "player at 0xC4");
        Check(U32At(b, 0xE8) == 41, "the start index at 0xE8");
        Check(U32At(b, 0xEC) == 1, "and the flag that says it means something");
        Check(U32At(b, 0xF4) == 1700000000u, "the pinned clock at 0xF4");
        Check(b[0xF8] == 1 && b[0xF9] == 2 && b[0xFA] == 3,
              "gamemode, track type and track number at 0xF8");
        Check(U32At(b, 0xFC) == WR_EXTRACTOR_REVISION, "the revision at 0xFC");

        // 0xFB is the one byte of the header nothing writes -- the three track
        // fields at 0xF8 are single bytes and the revision at 0xFC is aligned.
        // It stays zero, which is what makes a byte like it claimable later:
        // the start index at 0xE8 was exactly that until this revision.
        Check(b[0xFB] == 0, "the one spare byte at 0xFB is left zero");

        free(b);
    }

    // -----------------------------------------------------------------------
    printf("\nand what the real loader reads back\n");
    {
        WrPathLoadMap(TEST_MAP);
        int guard = 0;
        while (WrPathLoading(NULL, NULL) && ++guard < 10000)
            WrPathLoadTick();
        WrPathLoadTick();

        Check(WrRunCount() == 1, "exactly one run is loaded");
        const WrRun *r = WrRunCount() ? WrRunAt(0) : NULL;
        if (!r)
        {
            printf("\nSOME CHECKS FAILED\n\n");
            return 1;
        }

        Check(r->pointCount == kPoints, "every point survived the round trip");
        Check(r->runTime == 61.25 && r->tickInterval == (float)kTick,
              "the run time and tick interval came back");
        Check(r->steamId == 0x0110000112345678ULL, "the SteamID came back");
        Check(r->dateMs == 1717171717171LL, "the date came back");
        Check(strcmp(r->map, TEST_MAP) == 0, "the map name came back");
        Check(strcmp(r->player, "a player") == 0, "the player name came back");
        Check(r->gamemode == 1 && r->trackType == 2 && r->trackNum == 3,
              "the track fields came back");
        Check(r->startIndex == 41 && r->startTrusted,
              "and so did where the RUN starts, as opposed to the recording");
        Check(r->markerCount == 3, "all three markers came back");
        Check(r->markers[2].pointIndex == 210 &&
              r->markers[2].timeReached == 52.5 &&
              r->markers[2].minorNum == 3,
              "with their indices, times and minor numbers intact");

        // t = index * tick_interval, in double, rounded once. An accumulated
        // float would be about a millisecond out by point 300.
        bool timesExact = true;
        for (int i = 0; i < r->pointCount; i++)
            if (r->points[i].t != (float)((double)i * (double)(float)kTick))
                timesExact = false;
        Check(timesExact, "t is index * dt in double, not an accumulated float");

        // The positions are float32 in the file and were double in memory, so
        // the check is that the file's value is the correctly rounded one.
        bool posExact = true;
        for (int i = 0; i < r->pointCount; i++)
            if (r->points[i].pos.x != (float)pts[i].x ||
                r->points[i].pos.z != (float)pts[i].z ||
                r->points[i].vel.y != (float)pts[i].vy)
                posExact = false;
        Check(posExact, "every coordinate is the float32 of what went in");
    }

    // -----------------------------------------------------------------------
    printf("\nthe CRC covers the header too, and the clock is the only variable\n");
    {
        // Byte for byte identical when written twice. This is the property
        // WRLINES_FAKE_NOW exists for, and it is worth having on its own: a
        // user can re-extract a map and diff it against what they had.
        size_t n1 = 0, n2 = 0;
        unsigned char *first = Slurp(out, &n1);

        WrPathWriteArgs a;
        FillArgs(&a, out, pts, mk, 3);
        char err[256] = "";
        WrPathWrite(&a, err, sizeof(err));
        unsigned char *second = Slurp(out, &n2);

        Check(first && second && n1 == n2 && memcmp(first, second, n1) == 0,
              "writing the same input twice produces the same bytes");

        // ... and that it is the clock and nothing else. Unset the pin and the
        // stamp moves; everything before 0xF4 does not.
        SetEnvironmentVariableA("WRLINES_FAKE_NOW", "1800000001");
        WrPathWrite(&a, err, sizeof(err));
        size_t n3 = 0;
        unsigned char *third = Slurp(out, &n3);
        Check(third && n3 == n1 && memcmp(first, third, 0xF4) == 0 &&
              U32At(third, 0xF4) == 1800000001u &&
              memcmp(first + 0xF8, third + 0xF8, n1 - 0xF8 - 4) == 0,
              "and only the stamp at 0xF4 moves when the clock does");
        // int("...") ignores surrounding whitespace and so must this. Setting
        // the variable in a batch file the ordinary way -- `set WRLINES_FAKE_
        // NOW=1700000000` -- keeps the space cmd leaves before the newline,
        // and a strict terminator check quietly fell back to the live clock:
        // four differing bytes at 0xF4 and a differing CRC, in the one setting
        // whose entire job is to make two runs comparable.
        SetEnvironmentVariableA("WRLINES_FAKE_NOW", " 1700000000 ");
        Check(WrNowEpoch() == 1700000000LL, "a padded pin is still a pin");
        SetEnvironmentVariableA("WRLINES_FAKE_NOW", "\t1700000000\r\n");
        Check(WrNowEpoch() == 1700000000LL, "whatever the whitespace is");
        SetEnvironmentVariableA("WRLINES_FAKE_NOW", "1700000000x");
        Check(WrNowEpoch() != 1700000000LL,
              "but a typo is still ignored rather than half-read");

        SetEnvironmentVariableA("WRLINES_FAKE_NOW", "1700000000");
        WrPathWrite(&a, err, sizeof(err));

        // One byte anywhere, and the loader refuses the file rather than
        // reading it leniently.
        unsigned char *bent = (unsigned char *)malloc(n1);
        memcpy(bent, first, n1);
        bent[0x34] ^= 0x01;             // inside the map name, not the payload
        char bad[MAX_PATH];
        _snprintf_s(bad, sizeof(bad), _TRUNCATE, "%s\\demo0002.wrpath", dir);
        FILE *f = NULL;
        fopen_s(&f, bad, "wb");
        fwrite(bent, 1, n1, f);
        fclose(f);

        WrPathLoadMap(TEST_MAP);
        int guard = 0;
        while (WrPathLoading(NULL, NULL) && ++guard < 10000)
            WrPathLoadTick();
        WrPathLoadTick();
        Check(WrRunCount() == 1,
              "a file with one byte changed in the HEADER is refused");
        DeleteFileA(bad);

        free(first);
        free(second);
        free(third);
        free(bent);
    }

    // -----------------------------------------------------------------------
    printf("\nfixed-width fields, the way the reference's _fixed() fills them\n");
    {
        unsigned char f[16];

        WrPathFixedField(f, 8, "abc");
        Check(memcmp(f, "abc\0\0\0\0\0", 8) == 0, "a short string is NUL-padded");

        WrPathFixedField(f, 8, "abcdefghij");
        Check(memcmp(f, "abcdefg\0", 8) == 0,
              "a long one is cut to size-1, leaving room for the terminator");

        // Valid UTF-8 passes through unchanged.
        WrPathFixedField(f, 8, "\xE6\x97\xA5\xE6\x9C\xAC");
        Check(memcmp(f, "\xE6\x97\xA5\xE6\x9C\xAC\0\0", 8) == 0,
              "valid multi-byte UTF-8 is copied verbatim");

        // A three-byte character cut in half by the demo's own 32-byte field.
        // Python decodes that to one U+FFFD and encodes it back as three bytes.
        WrPathFixedField(f, 8, "ab\xE6\x97");
        Check(memcmp(f, "ab\xEF\xBF\xBD\0\0\0", 8) == 0,
              "a truncated sequence becomes ONE replacement character");

        // Two separate invalid bytes are two replacements, not one.
        WrPathFixedField(f, 12, "a\xFF\xFE" "b");
        Check(memcmp(f, "a\xEF\xBF\xBD\xEF\xBF\xBD" "b\0\0\0\0", 12) == 0,
              "two invalid start bytes are two of them");

        // A surrogate, which is not valid UTF-8 however it is spelled.
        WrPathFixedField(f, 8, "\xED\xA0\x80");
        Check(f[0] == 0xEF && f[1] == 0xBF && f[2] == 0xBD,
              "an encoded surrogate is rejected like any other bad sequence");

        // An overlong encoding of '/', the classic.
        WrPathFixedField(f, 8, "\xC0\xAF");
        Check(f[0] == 0xEF && f[1] == 0xBF && f[2] == 0xBD,
              "and so is an overlong form");

        // The truncation is applied to the ENCODED bytes and may cut one in
        // half, which is exactly what the reference's slice does.
        WrPathFixedField(f, 4, "a\xFF");
        Check(memcmp(f, "a\xEF\xBF\0", 4) == 0,
              "the cut lands wherever size-1 lands, mid-sequence or not");
    }

    // -----------------------------------------------------------------------
    // The other half of "what does this file format cost": a path this build
    // cannot name at all. Everything here is char* and every call is the -A
    // form, so a byte >= 0x80 anywhere in the install path is unreadable --
    // and until v0.7.0 the extractor was Python, which opens files with wide
    // paths and did not care. That makes it the one respect in which the port
    // is worse than what it replaces, so it is detected and reported rather
    // than left to look like "the button did nothing".
    printf("\na path this build cannot open, recognised as such\n");
    {
        Check(WrPathIsAscii("C:\\Program Files (x86)\\Steam\\steamapps"),
              "an ordinary install path is fine");
        Check(WrPathIsAscii(""), "so is an empty one");
        Check(WrPathIsAscii(NULL), "and a null one, rather than faulting");
        Check(!WrPathIsAscii("D:\\\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B\\Momentum"),
              "a Cyrillic folder name is not");
        Check(!WrPathIsAscii("C:\\caf\xC3\xA9"),
              "nor is one accented character at the end");

        // The bug this predicate is most likely to be written with: char is
        // signed on MSVC, so a test on the raw type misses every byte that has
        // the high bit set -- which is the whole set it is looking for.
        char high[3];
        high[0] = 'a';
        high[1] = (char)0xE9;
        high[2] = '\0';
        Check(!WrPathIsAscii(high), "and a high byte is read unsigned");
        Check(WrPathIsAscii("~\x7F"), "0x7F is still ASCII, and stays allowed");
    }

    // -----------------------------------------------------------------------
    printf("\nthe stem of a file name, which is os.path.splitext's and not strrchr's\n");
    {
        // Two callers depend on this agreeing with itself: wr_extract keys
        // _failed.txt and --skip-existing on the stem, wr_demo names the
        // .wrpath and packs src_sha1 with it. They were separate copies of a
        // one-line strrchr until the leading-dot rule turned out to be real.
        char s[64];

        WrFileStem("abc.mtv", s, sizeof(s));
        Check(strcmp(s, "abc") == 0, "the ordinary case");

        WrFileStem("C:\\demos\\surf\\deadbeef.mtv", s, sizeof(s));
        Check(strcmp(s, "deadbeef") == 0, "the directories come off first");

        WrFileStem("demos/surf/deadbeef.mtv", s, sizeof(s));
        Check(strcmp(s, "deadbeef") == 0, "and a forward slash separates too");

        WrFileStem("a.b.mtv", s, sizeof(s));
        Check(strcmp(s, "a.b") == 0, "only the LAST dot is the extension");

        WrFileStem("nodot", s, sizeof(s));
        Check(strcmp(s, "nodot") == 0, "no dot, no cut");

        // THE RULE. genericpath._splitext walks forward from the start of the
        // basename and, if it reaches the last dot having passed nothing but
        // dots, says there is no extension at all. A file called ".mtv" is
        // accepted by both sides' directory walks, so this is reachable: the
        // reference writes <map>\.mtv.wrpath and packs ".mtv" into src_sha1,
        // where cutting at the last dot writes <map>\.wrpath and packs
        // nothing.
        WrFileStem(".mtv", s, sizeof(s));
        Check(strcmp(s, ".mtv") == 0, "a name that is nothing but an extension keeps it");

        WrFileStem("..mtv", s, sizeof(s));
        Check(strcmp(s, "..mtv") == 0, "and so does one with two leading dots");

        WrFileStem(".a.mtv", s, sizeof(s));
        Check(strcmp(s, ".a") == 0, "but a leading dot with a name after it does split");

        WrFileStem("C:\\demos\\.mtv", s, sizeof(s));
        Check(strcmp(s, ".mtv") == 0, "the rule is about the BASENAME's dots, not the path's");

        WrFileStem(".", s, sizeof(s));
        Check(strcmp(s, ".") == 0, "a bare dot is all leading dots, so nothing is cut");

        WrFileStem(NULL, s, sizeof(s));
        Check(s[0] == '\0', "and a null name gives an empty stem rather than a fault");
    }

    // Leave nothing behind: this ran under tests\wrlines_data, which is
    // gitignored at any depth, but a stale run file would be loaded by the next
    // harness that asks for this map.
    DeleteFileA(out);

    free(pts);
    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
