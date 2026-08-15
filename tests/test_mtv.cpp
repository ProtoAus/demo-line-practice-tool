// test_mtv.cpp  --  the demo container, and every way it can be wrong.
//
// wr_mtv.cpp is the first thing that touches a file somebody else's program
// wrote, which makes it the one layer of the extractor where "what if this
// byte is not what I expect" is the main question rather than a footnote. Most
// of what is below is therefore about refusal: which headers are turned away,
// with which words, and -- the part that is easy to get wrong -- which ones are
// NOT turned away even though they look odd.
//
// THE WORDS MATTER AS MUCH AS THE VERDICT
//
// A refusal here becomes a line in paths\<map>\_failed.txt, and that file is
// read back by both implementations to decide whether a demo is worth trying
// again. A record whose reason string differs from the one the reference wrote
// is a record that reads as a different failure. So the messages are asserted
// exactly, not by prefix, wherever the reference produces one.
//
// WHAT IS SYNTHETIC AND WHY
//
// tests\make_fixture.py builds two whole demos -- the same demo at the two JSON
// offsets that exist in the wild -- because a real one is somebody's run and
// this repository does not carry those. Everything else below is those two
// fixtures with bytes deliberately broken, in memory, one case at a time. That
// is the only way to reach most of these paths: nothing in a real library has
// ever failed a header gate.
//
// Build:  tests\build.bat
// Run:    tests\test_mtv.exe

#include "wr_mtv.h"

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

// Keep in step with payload() in tests\make_fixture.py and Payload() in
// tests\test_lzma.cpp.
static void Payload(unsigned char *out, int n)
{
    unsigned int s = 12345;
    for (int i = 0; i < n; i++)
    {
        s = s * 1664525u + 1013904223u;
        if (i >= 1000 && (i / 137) % 3 == 0)
            out[i] = out[i - 1000];
        else if ((i / 61) % 5 == 0)
            out[i] = 0x41;
        else
            out[i] = (unsigned char)((s >> 24) & 0xFF);
    }
}

// A scratch copy of the good fixture, for a case that has to break one byte of
// it. Never mutate kMtvV1 itself: the cases below are read in order and a
// leaked edit would turn a later failure into a mystery.
static unsigned char g_buf[sizeof(kMtvV1)];

static size_t Fresh(void)
{
    memcpy(g_buf, kMtvV1, sizeof(kMtvV1));
    return sizeof(kMtvV1);
}

static void PutU32(size_t at, unsigned int v)
{
    g_buf[at + 0] = (unsigned char)(v & 0xFF);
    g_buf[at + 1] = (unsigned char)((v >> 8) & 0xFF);
    g_buf[at + 2] = (unsigned char)((v >> 16) & 0xFF);
    g_buf[at + 3] = (unsigned char)((v >> 24) & 0xFF);
}

// Refuse, and say exactly this. Returns true when both halves hold.
static bool Refused(size_t len, const char *want)
{
    WrMtvHeader h;
    char err[192] = "";
    if (WrMtvParseHeader(g_buf, len, &h, err, sizeof(err)))
    {
        printf("       (accepted, expected: %s)\n", want);
        return false;
    }
    if (strcmp(err, want) != 0)
    {
        printf("       (said: %s)\n", err);
        printf("       (want: %s)\n", want);
        return false;
    }
    return true;
}

static const char *kMapName = "surf_fixture";
static const char *kHash = "0123456789abcdef0123456789abcdef01234567";
static const size_t kBodyOff = WR_FIXTURE_MTV_JSON_V1 + WR_FIXTURE_MTV_JSON_LEN;

int main(void)
{
    printf("\n=== wrlines mtv ===\n");

    // -----------------------------------------------------------------------
    printf("\nthe fixed header is eleven fields at eleven constant offsets\n");
    {
        WrMtvHeader h;
        char err[192] = "untouched";
        bool ok = WrMtvParseHeader(kMtvV1, sizeof(kMtvV1), &h, err, sizeof(err));

        Check(ok, "the fixture parses");
        Check(ok && err[0] == '\0', "and says nothing on the way");
        Check(h.version == 2, "version");
        Check(h.dateMs == 1700000000123LL, "date, as a signed millisecond count");
        Check(strcmp(h.map, kMapName) == 0, "map name, up to its NUL");
        Check(strcmp(h.mapHash, kHash) == 0, "map hash, all forty characters");
        Check(h.gamemode == 1, "gamemode");
        Check(h.tickInterval == 0.015f, "tick interval");
        Check(h.steamid64 == 0x0110000112345678ULL, "steamid64");
        Check(strcmp(h.player, "fixture player") == 0, "player name");
        Check(h.trackType == 2 && h.trackNum == 3, "track type and number");

        // At 0xA9, which is odd, so this is an unaligned eight-byte read and
        // the only reason the field is memcpy'd rather than cast.
        Check(h.runTime == 61.25, "run time, unaligned at 0xA9");
        Check(h.ticks == 4083, "tick count");
    }

    // -----------------------------------------------------------------------
    printf("\nthe JSON blob is found by its neighbours, not by its offset\n");
    {
        WrMtvHeader v1, v2;
        char e1[192] = "", e2[192] = "";
        bool o1 = WrMtvParseHeader(kMtvV1, sizeof(kMtvV1), &v1, e1, sizeof(e1));
        bool o2 = WrMtvParseHeader(kMtvV2, sizeof(kMtvV2), &v2, e2, sizeof(e2));

        Check(o1 && v1.jsonStart == WR_FIXTURE_MTV_JSON_V1,
              "the v1 container's blob is at 0xC6");
        Check(o2 && v2.jsonStart == WR_FIXTURE_MTV_JSON_V2,
              "the v2 container's blob is at 0xC7, and nothing said so");
        Check(o1 && v1.jsonLen == WR_FIXTURE_MTV_JSON_LEN, "its length is the u32 before it");
        Check(o1 && v1.codec == WR_MTV_CODEC_LZMA, "the codec is the magic after it");
        Check(o1 && v1.bodyOff == kBodyOff, "and the body starts where the JSON ends");
        Check(o2 && v2.bodyOff == v1.bodyOff + 1, "one byte later in v2, throughout");
    }

    // -----------------------------------------------------------------------
    printf("\na stray brace before the real one is stepped over\n");
    {
        // This is not a hypothetical. Taking the first '{' on faith is what
        // produced "implausible JSON length 1076353433" on two demos, and the
        // padding either side of the player name is arbitrary bytes.
        size_t n = Fresh();
        g_buf[0xBA] = '{';
        PutU32(0xB6, 0x7FFFFFFFu);          // too big to be a JSON length

        WrMtvHeader h;
        char err[192] = "";
        bool ok = WrMtvParseHeader(g_buf, n, &h, err, sizeof(err));
        Check(ok && h.jsonStart == WR_FIXTURE_MTV_JSON_V1,
              "a brace with an implausible length prefix is not the blob");

        // The other half: a length that IS plausible, but whose tail is not a
        // codec magic. Both checks have to be doing work, or a demo whose
        // padding happens to hold a small number would be misread.
        n = Fresh();
        g_buf[0xBA] = '{';
        PutU32(0xB6, 0x10);
        ok = WrMtvParseHeader(g_buf, n, &h, err, sizeof(err));
        Check(ok && h.jsonStart == WR_FIXTURE_MTV_JSON_V1,
              "nor is one whose length lands somewhere that is not a codec");
    }

    // -----------------------------------------------------------------------
    printf("\nand when there is no blob, it says which lengths it tried\n");
    {
        // The window cleared so that the braces are exactly the ones put here.
        // A real demo has braces in the compressed body too -- the fixtures do
        // -- which is why the success cases above are the ones that use them.
        size_t n = Fresh();
        memset(g_buf + 0xB0, 0, 0x200 - 0xB0);
        Check(Refused(n, "no '{' in header window"),
              "no brace at all is its own message");

        n = Fresh();
        memset(g_buf + 0xB0, 0, 0x200 - 0xB0);
        g_buf[0xC6] = '{';
        PutU32(0xC2, 0xFFFFFFFFu);
        Check(Refused(n, "no JSON blob in header window (lengths tried: 4294967295)"),
              "one candidate is named");

        // Five candidates, four printed, in the order they were tried. The
        // reference slices tried[:4] and this has to slice the same four.
        n = Fresh();
        memset(g_buf + 0xB0, 0, 0x200 - 0xB0);
        for (int i = 0; i < 5; i++)
        {
            size_t at = 0xC0 + (size_t)i * 8;
            g_buf[at] = '{';
            PutU32(at - 4, (unsigned int)(i + 1));
        }
        Check(Refused(n, "no JSON blob in header window (lengths tried: 1, 2, 3, 4)"),
              "five candidates print as the first four");
    }

    // -----------------------------------------------------------------------
    printf("\nthe sanity gates, and the words they use\n");
    {
        size_t n = Fresh();
        g_buf[0] = 'X';
        Check(Refused(n, "not an MMTV file"), "wrong magic");

        n = Fresh();
        Check(Refused(0x1FF, "not an MMTV file"), "a file one byte under 0x200");

        n = Fresh();
        memset(g_buf + 0x10, 0, 64);
        Check(Refused(n, "empty map name field"), "an empty map name");

        // 0.5 and 0.0 are exact in float32, so what repr() prints of them is
        // not in doubt -- which is the point. The ".0" on the second is the
        // whole reason wr_mtv.cpp reimplements repr instead of using "%g":
        // Python never prints a float without one.
        n = Fresh();
        {
            float bad = 0.5f;
            memcpy(g_buf + 0x7B, &bad, 4);
        }
        Check(Refused(n, "tick interval 0.5 out of range"), "a tick interval of 0.5");

        n = Fresh();
        memset(g_buf + 0x7B, 0, 4);
        Check(Refused(n, "tick interval 0.0 out of range"),
              "a tick interval of zero, printed as Python prints it");

        // AND THE ONES 0.5 AND 0.0 CANNOT CATCH, which is why they were the
        // only two here while PyRepr was wrong.
        //
        // repr picks fixed or exponential from where the decimal point falls
        // -- exponential only when decpt <= -4 or decpt > 16 -- and NOT from
        // how many digits the shortest round-trip needs. "%.*g" decides it the
        // other way, so 20.0 came out "2e+01". Every value below round-trips
        // in fewer digits than it has integer places, which is exactly the set
        // the old code got wrong, and they are the values a moved OFF_TICKRATE
        // would actually land on: a ticks-per-SECOND field rather than an
        // interval.
        static const struct { float v; const char *repr; } kTicks[] = {
            { 20.0f,      "20.0" },          // was "2e+01"
            { 100.0f,     "100.0" },         // was "1e+02"
            { 1000.0f,    "1000.0" },        // was "1e+03"
            { 128.0f,     "128.0" },
            { 0.25f,      "0.25" },          // fixed, and stays fixed
            // Powers of two, because these two are about the EXPONENTIAL side
            // and every value here is widened from float32 before it is
            // printed: 1e-5f is not 1e-5, it is 1.0000000116860974e-05, and a
            // test written with the decimal literal asserts the wrong string
            // for the right reason. A power of two is exact in both formats.
            { 9.5367431640625e-07f, "9.5367431640625e-07" },   // 2^-20, decpt -6
            { 1152921504606846976.0f, "1.152921504606847e+18" }, // 2^60, decpt 19
            { -1.0f,      "-1.0" },          // negative, and still gets its .0
        };
        for (int i = 0; i < (int)(sizeof(kTicks) / sizeof(kTicks[0])); i++)
        {
            char want[64];
            _snprintf_s(want, sizeof(want), _TRUNCATE,
                        "tick interval %s out of range", kTicks[i].repr);
            n = Fresh();
            memcpy(g_buf + 0x7B, &kTicks[i].v, 4);
            Check(Refused(n, want), want);
        }

        // The map name survives a file too short to parse.
        //
        // peek_map reads 0x50 bytes, checks the magic and nothing else, and it
        // is what CHOOSES the targets -- so the reference selects a truncated
        // demo, names it, and then fails it with a reason that reaches
        // _failed.txt. Returning before the name was read made such a file
        // vanish from a --map run entirely.
        {
            Fresh();
            WrMtvHeader h2;
            char e2[128];
            bool ok = WrMtvParseFixed(g_buf, 0x50, 0x50, &h2, e2, sizeof(e2));
            Check(!ok && strcmp(e2, "not an MMTV file") == 0,
                  "a 0x50-byte file is still refused, with the same words");
            Check(strcmp(h2.map, kMapName) == 0,
                  "but its map name came out, the way peek_map's does");

            Fresh();
            ok = WrMtvParseFixed(g_buf, 0x14, 0x14, &h2, e2, sizeof(e2));
            Check(!ok && strcmp(h2.map, "surf") == 0,
                  "and a file that stops mid-name gives the part that was there");

            Fresh();
            ok = WrMtvParseFixed(g_buf, 4, 4, &h2, e2, sizeof(e2));
            Check(!ok && h2.map[0] == '\0',
                  "while four bytes of magic and nothing else names nothing");
        }

        // 0x83 is the low byte of the HIGH word: the field is little-endian at
        // 0x7F, so 0x7F..0x82 carry 0x12345678 and 0x83..0x86 carry 0x01100001.
        n = Fresh();
        g_buf[0x83] = 0x02;                 // 0x01100001 -> 0x01100002
        Check(Refused(n, "steamid64 high word wrong (0x1100002)"),
              "a SteamID64 that is not an individual account");
    }

    // -----------------------------------------------------------------------
    printf("\na refused header still says which map it belonged to\n");
    {
        // The counter depends on this. A demo whose header does not survive the
        // gates is still a demo FOR a map, and has to be counted against that
        // map rather than vanishing out of the total.
        size_t n = Fresh();
        memset(g_buf + 0x7B, 0, 4);

        WrMtvHeader h;
        char err[192] = "";
        bool ok = WrMtvParseHeader(g_buf, n, &h, err, sizeof(err));
        Check(!ok, "the gate refuses it");
        Check(strcmp(h.map, kMapName) == 0, "and the map name read before it survives");

        // The other kind of failure leaves nothing behind, because there was
        // nothing to read.
        n = Fresh();
        g_buf[0] = 'X';
        WrMtvParseHeader(g_buf, n, &h, err, sizeof(err));
        Check(h.map[0] == '\0', "where \"not a demo\" leaves the struct empty");
    }

    // -----------------------------------------------------------------------
    printf("\nthe body comes back as the bytes it went in as\n");
    {
        WrMtvHeader h;
        char err[192] = "";
        bool ok = WrMtvParseHeader(kMtvV1, sizeof(kMtvV1), &h, err, sizeof(err));

        size_t bodyLen = 0;
        unsigned char *body = ok ? WrMtvBody(kMtvV1, sizeof(kMtvV1), &h, &bodyLen,
                                             err, sizeof(err)) : NULL;
        Check(body != NULL, "it decompresses");
        Check(bodyLen == WR_FIXTURE_MTV_BODY, "to the length the container claimed");

        unsigned char *want = (unsigned char *)malloc(WR_FIXTURE_MTV_BODY);
        if (want)
            Payload(want, WR_FIXTURE_MTV_BODY);
        Check(body && want && memcmp(body, want, WR_FIXTURE_MTV_BODY) == 0,
              "and to the right bytes");
        free(want);
        free(body);

        // Same demo, one byte further along. If the two disagreed, every offset
        // in the container would be suspect.
        WrMtvHeader h2;
        size_t len2 = 0;
        ok = WrMtvParseHeader(kMtvV2, sizeof(kMtvV2), &h2, err, sizeof(err));
        unsigned char *b2 = ok ? WrMtvBody(kMtvV2, sizeof(kMtvV2), &h2, &len2,
                                           err, sizeof(err)) : NULL;
        Check(b2 && len2 == WR_FIXTURE_MTV_BODY, "and the v2 container gives the same");
        free(b2);
    }

    // -----------------------------------------------------------------------
    printf("\nand refuses rather than reaching past the end of the file\n");
    {
        WrMtvHeader h;
        char err[192] = "";
        WrMtvParseHeader(kMtvV1, sizeof(kMtvV1), &h, err, sizeof(err));

        size_t bodyLen = 0;
        err[0] = '\0';
        Check(WrMtvBody(kMtvV1, kBodyOff + 20, &h, &bodyLen, err, sizeof(err)) == NULL &&
              strcmp(err, "LZMA block runs past EOF") == 0,
              "a file that stops inside the compressed data");

        // The reference reads the two u32s at bodyOff+4 without checking there
        // are twelve bytes there, and a file truncated here makes it raise a
        // struct.error that its own handler does not catch -- taking the whole
        // run down rather than the demo. Same verdict here, one demo's worth.
        err[0] = '\0';
        Check(WrMtvBody(kMtvV1, kBodyOff + 8, &h, &bodyLen, err, sizeof(err)) == NULL &&
              strcmp(err, "LZMA block runs past EOF") == 0,
              "and one that stops inside the seventeen-byte container");

        size_t n = Fresh();
        PutU32(kBodyOff + 4, 0x40000000u);      // one gigabyte
        WrMtvHeader big;
        WrMtvParseHeader(g_buf, n, &big, err, sizeof(err));
        err[0] = '\0';
        Check(WrMtvBody(g_buf, n, &big, &bodyLen, err, sizeof(err)) == NULL &&
              strcmp(err, "body claims 1073741824 bytes, over the 536870912 byte "
                          "limit") == 0,
              "and a body too big for the scan to address it later");
    }

    // -----------------------------------------------------------------------
    printf("\nzstd is a codec the locator recognises\n");
    {
        // This section used to end "asking for the body anyway is refused by
        // name", and it passed for two releases while the refusal was the whole
        // behaviour. It is kept pointed at the LOCATOR, which is this file's
        // subject: that the magic four bytes past the JSON select the codec and
        // put bodyOff at the frame rather than past a container that is not
        // there.
        //
        // What comes out of that frame is tests\test_zstd.exe's business, and
        // it drives a real one -- these four bytes are a magic number with no
        // frame behind them, which is exactly the input the size call has to
        // refuse rather than trust.
        size_t n = Fresh();
        g_buf[kBodyOff + 0] = 0x28;
        g_buf[kBodyOff + 1] = 0xB5;
        g_buf[kBodyOff + 2] = 0x2F;
        g_buf[kBodyOff + 3] = 0xFD;

        WrMtvHeader h;
        char err[192] = "";
        bool ok = WrMtvParseHeader(g_buf, n, &h, err, sizeof(err));
        Check(ok, "a zstd body parses as a header");
        Check(ok && h.codec == WR_MTV_CODEC_ZSTD, "and reports its codec");
        Check(ok && h.bodyOff == kBodyOff, "at the same offset an LZMA body would be");

        // A truncated frame header, not a body. It must fail, and it must fail
        // WITHOUT the words the LZMA arm uses -- "LZMA block runs past EOF" on
        // a zstd demo would send somebody reading the wrong half of the file.
        size_t bodyLen = 1;
        err[0] = '\0';
        Check(WrMtvBody(g_buf, n, &h, &bodyLen, err, sizeof(err)) == NULL &&
              bodyLen == 0 && strstr(err, "LZMA") == NULL,
              "and a frame with nothing behind it is refused in zstd's terms");
    }

    // -----------------------------------------------------------------------
    printf("\npeeking reads the header and nothing else\n");
    {
        const char *path = "tests\\_fixdemo.mtv";
        FILE *f = NULL;
        bool wrote = (fopen_s(&f, path, "wb") == 0 && f &&
                      fwrite(kMtvV1, 1, sizeof(kMtvV1), f) == sizeof(kMtvV1));
        if (f)
            fclose(f);
        Check(wrote, "the fixture can be written out to peek at");

        WrMtvHeader h;
        char err[192] = "untouched";
        bool ok = WrMtvPeek(path, &h, err, sizeof(err));
        Check(ok && err[0] == '\0', "it reads");
        Check(ok && strcmp(h.map, kMapName) == 0 && h.tickInterval == 0.015f &&
              h.steamid64 == 0x0110000112345678ULL,
              "and the fields the counter needs are the same ones");
        Check(ok && h.codec == WR_MTV_CODEC_NONE && h.bodyOff == 0,
              "the blob is NOT located, because that would need the whole file");

        // A stub too short to be a demo. `got` from the one read doubles as the
        // size gate, which is why there is no separate stat call.
        const char *stub = "tests\\_fixstub.mtv";
        if (fopen_s(&f, stub, "wb") == 0 && f)
        {
            fwrite(kMtvV1, 1, 0x100, f);
            fclose(f);
        }
        err[0] = '\0';
        Check(!WrMtvPeek(stub, &h, err, sizeof(err)) &&
              strcmp(err, "not an MMTV file") == 0,
              "a 256-byte file is refused on its length");

        // And it is refused WITH ITS NAME, which is the opposite of what this
        // asserted for one release. peek_map -- the reference function this
        // stands in for, and the one that picks the targets -- reads 0x50
        // bytes and checks only the magic, so the reference selects a
        // truncated demo, prints a FAIL line for it and records it under its
        // map. Handing back an empty name made the same file disappear from a
        // --map run with nothing said anywhere.
        Check(strcmp(h.map, kMapName) == 0,
              "and named anyway, so it can be reported rather than vanish");

        err[0] = '\0';
        Check(!WrMtvPeek("tests\\_no_such_demo.mtv", &h, err, sizeof(err)) &&
              err[0] != '\0', "and a missing file says so rather than crashing");

        // WrMtvReadFile, on the way past: the other end of the same file.
        size_t whole = 0;
        unsigned char *all = WrMtvReadFile(path, &whole, err, sizeof(err));
        Check(all && whole == sizeof(kMtvV1) &&
              memcmp(all, kMtvV1, sizeof(kMtvV1)) == 0,
              "reading the whole file gives back what was written");
        free(all);

        DeleteFileA(path);
        DeleteFileA(stub);
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
