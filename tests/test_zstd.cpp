// test_zstd.cpp  --  the other decoder underneath a demo.
//
// This is test_lzma.cpp's sibling and exists for the same reason: a wrong byte
// out of the decompressor becomes a slightly different float, which becomes a
// chain the dynamic program scores slightly differently, which becomes a line
// in the world that bends. The difference is that this layer arrived in v0.9.4,
// two releases after the code around it, so it has no history of being right.
//
// WHAT IS ACTUALLY UNDER TEST
//
// Not the zstd library. That is Meta's, it is fuzzed continuously by OSS-Fuzz,
// and re-verifying it here would be theatre. What is under test is the page in
// wr_mtv.cpp that decides how to call it, and unlike the LZMA page every
// decision on it is NEW:
//
//   - the size comes from the FRAME rather than from a container field, so
//     ZSTD_getFrameContentSize's two different unknowns both have to become
//     refusals with their own words. Getting this wrong does not crash; it
//     allocates the wrong number of bytes.
//   - ZSTD_decompress returns an error CODE, and ZSTD_isError is the only way
//     to tell an error from a length -- both are size_t and an error is a very
//     large one. A missing ZSTD_isError reads a 2^64-sized body as success.
//   - the body is data[bodyOff..len), with no seventeen-byte Valve header,
//     because that is what the reference slices. Handing over seventeen bytes
//     too few or too many is the seam bug this catches.
//
// WHY THE REFUSALS MATTER AS MUCH AS THE SUCCESS
//
// A zstd body could not fail before v0.9.4 -- it was skipped before anything
// looked at it -- so every refusal below is a path that has never run in
// anger. They are also, unlike the LZMA ones, NOT the reference's wording:
// python-zstandard raises its own sentences and none of them has ever reached
// a _failed.txt, because until now this side never decoded these at all.
//
// Build:  tests\build.bat
// Run:    tests\test_zstd.exe

#include "wr_mtv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_zstd.h"
#include "fixture_mtv.h"

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// The same twelve lines as payload() in tests\make_fixture.py and as Payload()
// in test_lzma.cpp. Written out a third time rather than shared, and that is
// deliberate: the expected bytes are derived HERE and the compressed bytes were
// derived THERE, so the only thing that can make them agree is the decoder. A
// shared helper would let one mistake satisfy both sides.
static void Payload(unsigned char *out, int n)
{
    unsigned int s = 12345;
    for (int i = 0; i < n; i++)
    {
        s = s * 1664525u + 1013904223u;
        if (i >= 1000 && ((i / 137) % 3) == 0)
            out[i] = out[i - 1000];
        else if (((i / 61) % 5) == 0)
            out[i] = 0x41;
        else
            out[i] = (unsigned char)((s >> 24) & 0xFF);
    }
}

int main(void)
{
    printf("=== the zstd decoder, and how wr_mtv.cpp calls it ===\n");

    unsigned char *want = (unsigned char *)malloc(WR_FIXTURE_ZSTD_RAW);
    Payload(want, WR_FIXTURE_ZSTD_RAW);

    // -----------------------------------------------------------------------
    printf("\nthe frame says how big it is, and it is right\n");
    {
        char err[192] = "";
        const size_t n = WrMtvZstdSize(kZstdFrame, sizeof(kZstdFrame),
                                       err, sizeof(err));
        Check(n == WR_FIXTURE_ZSTD_RAW, "the declared size is the payload size");
        Check(err[0] == '\0', "and nothing was said about it");

        unsigned char *got = (unsigned char *)malloc(n ? n : 1);
        err[0] = '\0';
        const bool ok = WrMtvZstdDecode(kZstdFrame, sizeof(kZstdFrame),
                                        got, n, err, sizeof(err));
        Check(ok, "the frame decodes");
        Check(ok && memcmp(got, want, WR_FIXTURE_ZSTD_RAW) == 0,
              "to every byte the formula says it should");
        free(got);
    }

    // -----------------------------------------------------------------------
    printf("\nthings that are not a frame are refused by name\n");
    {
        char err[192] = "";
        Check(WrMtvZstdSize(kZstdFrame, 3, err, sizeof(err)) == 0 &&
              strcmp(err, "zstd frame runs past EOF") == 0,
              "fewer bytes than a magic number");

        // The magic is the first four bytes and this breaks it. ZSTD_getFrame-
        // ContentSize answers CONTENTSIZE_ERROR, which is a different value
        // from CONTENTSIZE_UNKNOWN and has to stay a different sentence.
        unsigned char bad[64];
        memcpy(bad, kZstdFrame, sizeof(bad));
        bad[0] = 0x29;
        err[0] = '\0';
        Check(WrMtvZstdSize(bad, sizeof(bad), err, sizeof(err)) == 0 &&
              strcmp(err, "not a zstd frame") == 0,
              "a broken magic number");

        // A skippable frame: magic 0x184D2A5* and a length, which is legal zstd
        // and carries no content at all. ZSTD_getFrameContentSize reports zero
        // for it rather than an error, so without its own arm this would
        // malloc(1) and hand back an empty body as a success.
        const unsigned char skippable[8] = {
            0x50, 0x2A, 0x4D, 0x18, 0x00, 0x00, 0x00, 0x00
        };
        err[0] = '\0';
        Check(WrMtvZstdSize(skippable, sizeof(skippable), err, sizeof(err)) == 0 &&
              strcmp(err, "zstd frame declares an empty body") == 0,
              "a skippable frame, which is legal and holds nothing");
    }

    // -----------------------------------------------------------------------
    printf("\na truncated frame fails rather than short-reads\n");
    {
        // Half a frame. The size still reads -- it is in the header, which
        // survived -- so this is the case where the two calls disagree, and the
        // decode has to be the one that notices.
        char err[192] = "";
        const size_t n = WrMtvZstdSize(kZstdFrame, sizeof(kZstdFrame) / 2,
                                       err, sizeof(err));
        Check(n == WR_FIXTURE_ZSTD_RAW,
              "the header of a truncated frame still declares the full size");

        unsigned char *got = (unsigned char *)malloc(n);
        err[0] = '\0';
        const bool ok = WrMtvZstdDecode(kZstdFrame, sizeof(kZstdFrame) / 2,
                                        got, n, err, sizeof(err));
        Check(!ok, "and decoding it fails");
        Check(!ok && strncmp(err, "zstd: ", 6) == 0,
              "with the library's own reason, prefixed so it is placeable");
        free(got);
    }

    // -----------------------------------------------------------------------
    printf("\nthe whole container, which is NOT the LZMA one\n");
    {
        // The seam. A zstd demo has no seventeen-byte Valve header, so bodyOff
        // is the frame magic itself -- and handing WrMtvZstdDecode seventeen
        // bytes further in is the mistake that would not be caught anywhere
        // else, because every layer above this one would still be given
        // plausible bytes.
        WrMtvHeader h;
        char err[192] = "";
        const bool parsed = WrMtvParseHeader(kMtvZstd, sizeof(kMtvZstd), &h,
                                             err, sizeof(err));
        Check(parsed, "the zstd demo parses as a header");
        Check(parsed && h.codec == WR_MTV_CODEC_ZSTD, "and reports its codec");
        Check(parsed && h.jsonStart == WR_FIXTURE_MTV_JSON_V1,
              "with the JSON where the v1 container puts it");
        Check(parsed && kMtvZstd[h.bodyOff] == 0x28 &&
              kMtvZstd[h.bodyOff + 1] == 0xB5,
              "and bodyOff pointing AT the frame magic, not past a header");

        size_t bodyLen = 0;
        err[0] = '\0';
        unsigned char *body = WrMtvBody(kMtvZstd, sizeof(kMtvZstd), &h,
                                        &bodyLen, err, sizeof(err));
        Check(body != NULL, "the body comes out");
        Check(body && bodyLen == WR_FIXTURE_MTV_BODY,
              "at the length the run's own payload was");

        unsigned char *expect = (unsigned char *)malloc(WR_FIXTURE_MTV_BODY);
        Payload(expect, WR_FIXTURE_MTV_BODY);
        Check(body && bodyLen == WR_FIXTURE_MTV_BODY &&
              memcmp(body, expect, WR_FIXTURE_MTV_BODY) == 0,
              "and byte for byte what went in");
        free(expect);
        free(body);
    }

    // -----------------------------------------------------------------------
    printf("\nand the LZMA demo is untouched by any of it\n");
    {
        // The two arms share one function and one enum, so the cheapest way for
        // this release to have broken something is to have broken the arm that
        // already worked.
        WrMtvHeader h;
        char err[192] = "";
        const bool parsed = WrMtvParseHeader(kMtvV1, sizeof(kMtvV1), &h,
                                             err, sizeof(err));
        Check(parsed && h.codec == WR_MTV_CODEC_LZMA,
              "the LZMA demo still reports LZMA");

        size_t bodyLen = 0;
        unsigned char *body = WrMtvBody(kMtvV1, sizeof(kMtvV1), &h, &bodyLen,
                                        err, sizeof(err));
        Check(body && bodyLen == WR_FIXTURE_MTV_BODY,
              "and still decompresses to the same length");
        free(body);
    }

    free(want);

    printf("\n%s\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
