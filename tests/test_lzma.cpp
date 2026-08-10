// test_lzma.cpp  --  the decoder underneath every demo.
//
// A .mtv's run body is a raw LZMA1 stream, and everything the extractor does
// afterwards is arithmetic on the bytes that come out of it. If this layer is
// wrong, nothing above it can be right and nothing above it will say so
// clearly: a wrong byte in the body becomes a slightly different float, which
// becomes a chain the dynamic program scores slightly differently, which
// becomes a line in the world that bends. So this is checked on its own,
// against a payload whose every byte is known by formula rather than by
// sitting next to the answer in the same file.
//
// WHAT IS ACTUALLY UNDER TEST
//
// Not LzmaDec. That is public-domain code by the person who designed the
// format and it is not this project's business to re-verify it. What is under
// test is the half-page in wr_mtv.cpp that decides HOW to call it, and every
// line of that half-page is a decision that could have gone the other way:
//
//   - LZMA_FINISH_ANY rather than LZMA_FINISH_END. The reference asks its
//     decompressor for at most `actual` bytes and then checks it got them all;
//     it never requires the stream to end. Sections below pin both halves of
//     that -- asking for fewer bytes must succeed with a prefix, and asking for
//     more must fail as a short read.
//   - the five property bytes handed over verbatim instead of being unpacked
//     into lc/lp/pb by hand.
//   - a dictionary size out of the file being harmless rather than clamped.
//   - which SRes becomes which message, because those messages end up in
//     _failed.txt next to records the reference wrote.
//
// Build:  tests\build.bat
// Run:    tests\test_lzma.exe

#include "wr_mtv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_lzma.h"

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// The same twelve lines as payload() in tests\make_fixture.py, which is what
// makes the comparison below worth making: the expected bytes are derived here
// and the compressed bytes were derived there, and the only thing that can make
// them agree is the decoder.
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

int main(void)
{
    printf("\n=== wrlines lzma ===\n");

    const int raw = WR_FIXTURE_LZMA_RAW;
    unsigned char *want = (unsigned char *)malloc((size_t)raw);
    unsigned char *got = (unsigned char *)malloc((size_t)raw + 64);
    if (!want || !got)
    {
        printf("  out of memory\n");
        return 2;
    }
    Payload(want, raw);

    // -----------------------------------------------------------------------
    printf("\nthe stream decodes to the bytes the fixture was made from\n");
    {
        char err[128] = "untouched";
        memset(got, 0xCC, (size_t)raw);
        bool ok = WrMtvLzmaDecode(kLzmaStream, sizeof(kLzmaStream),
                                  got, (size_t)raw, kLzmaProps, err, sizeof(err));
        Check(ok, "it decodes");
        Check(ok && err[0] == '\0', "and says nothing on the way");
        Check(memcmp(got, want, (size_t)raw) == 0,
              "all 16384 bytes, including the 1000-byte back-references");

        // The payload is built to repeat runs of 137 bytes from a thousand back,
        // which is what makes the encoder emit long-distance matches -- the case
        // a decoder with a broken distance path gets wrong while still producing
        // plausible-looking noise. Asserted here so that changing the formula in
        // make_fixture.py to something tamer cannot quietly weaken this file.
        int repeats = 0;
        for (int i = 1000; i < raw; i++)
            if (want[i] == want[i - 1000] && want[i] != 0x41)
                repeats++;
        Check(repeats > 2000, "and the fixture really does repeat from far back");
    }

    // -----------------------------------------------------------------------
    printf("\nthe output length is the caller's, not the stream's\n");
    {
        // LZMA_FINISH_ANY, stated as a behaviour. The reference's
        // decompress(data, actual) stops at actual and does not care that the
        // stream had more to give; this must do the same, because the container
        // it reads carries no end marker to stop at.
        char err[128] = "untouched";
        memset(got, 0xCC, (size_t)raw);
        bool ok = WrMtvLzmaDecode(kLzmaStream, sizeof(kLzmaStream),
                                  got, 4000, kLzmaProps, err, sizeof(err));
        Check(ok, "asking for 4000 of 16384 bytes succeeds");
        Check(ok && memcmp(got, want, 4000) == 0, "and they are the first 4000");
        Check(got[4000] == 0xCC, "and it wrote no further");
    }

    // -----------------------------------------------------------------------
    printf("\nasking for more than the stream holds is a short read\n");
    {
        char err[128] = "";
        bool ok = WrMtvLzmaDecode(kLzmaStream, sizeof(kLzmaStream),
                                  got, (size_t)raw + 32, kLzmaProps,
                                  err, sizeof(err));
        Check(!ok, "it fails");
        Check(strcmp(err, "LZMA short read: 16384 of 16416") == 0,
              "with the reference's own wording and numbers");
    }

    // -----------------------------------------------------------------------
    printf("\na truncated stream is a short read too, not a corruption\n");
    {
        char err[128] = "";
        bool ok = WrMtvLzmaDecode(kLzmaStream, sizeof(kLzmaStream) / 2,
                                  got, (size_t)raw, kLzmaProps, err, sizeof(err));
        Check(!ok, "half a stream fails");
        Check(strncmp(err, "LZMA short read: ", 17) == 0,
              "as a short read, because running out of input is not corruption");

        // Under RC_INIT_SIZE the decoder cannot even start. It must still come
        // back as the same kind of failure rather than as a different one.
        char err2[128] = "";
        bool ok2 = WrMtvLzmaDecode(kLzmaStream, 3, got, (size_t)raw, kLzmaProps,
                                   err2, sizeof(err2));
        Check(!ok2 && strcmp(err2, "LZMA short read: 0 of 16384") == 0,
              "and three bytes of input is a short read of zero");
    }

    // -----------------------------------------------------------------------
    printf("\na dictionary size out of the file is not something to clamp\n");
    {
        // The claim in wr_mtv.h's header: LzmaDecode() decompresses into the
        // caller's buffer and allocates only the probability model, so the
        // dictSize in the props buys an attacker nothing. If that were ever to
        // stop being true -- a future SDK that allocated it -- this would take
        // four gigabytes and the harness would stop rather than pass.
        unsigned char props[5];
        memcpy(props, kLzmaProps, 5);
        props[1] = props[2] = props[3] = props[4] = 0xFF;

        char err[128] = "";
        memset(got, 0xCC, (size_t)raw);
        bool ok = WrMtvLzmaDecode(kLzmaStream, sizeof(kLzmaStream),
                                  got, (size_t)raw, props, err, sizeof(err));
        Check(ok, "a stream claiming a 4 GB dictionary still decodes");
        Check(ok && memcmp(got, want, (size_t)raw) == 0, "to the same bytes");
    }

    // -----------------------------------------------------------------------
    printf("\nthe property byte is validated where the reference validates it\n");
    {
        // The SDK refuses a first property byte of 225 or more, and 225 is
        // 9*5*5 -- the same boundary Python's filter validation refuses, for
        // the same reason. So this case produces a failure in both, and the
        // wording is liblzma's because that is what is already written in
        // people's _failed.txt.
        unsigned char props[5];
        memcpy(props, kLzmaProps, 5);
        props[0] = 225;

        char err[128] = "";
        bool ok = WrMtvLzmaDecode(kLzmaStream, sizeof(kLzmaStream),
                                  got, (size_t)raw, props, err, sizeof(err));
        Check(!ok, "lc/lp/pb packed as 225 is refused");
        Check(strcmp(err, "Invalid or unsupported options") == 0,
              "with the wording str(LZMAError) would have given");

        char err2[128] = "";
        props[0] = 224;     // pb=4 lp=4 lc=8: legal, and not what made this
        bool ok2 = WrMtvLzmaDecode(kLzmaStream, sizeof(kLzmaStream),
                                   got, (size_t)raw, props, err2, sizeof(err2));
        Check(!ok2 && strcmp(err2, "Invalid or unsupported options") != 0,
              "and 224 is refused for being the wrong options, not for existing");
    }

    // -----------------------------------------------------------------------
    printf("\nnothing in, nothing out\n");
    {
        // The reference's decompress(data, 0) returns b"" and its length check
        // passes, so a zero-length body is a success there. It has to be one
        // here too, and it has to happen before the decoder is involved --
        // LzmaDecode with an empty destination is not a case worth finding out
        // about the hard way.
        char err[128] = "untouched";
        bool ok = WrMtvLzmaDecode(NULL, 0, NULL, 0, NULL, err, sizeof(err));
        Check(ok, "a zero-length body succeeds without touching the decoder");
        Check(err[0] == '\0', "and reports nothing");
    }

    free(want);
    free(got);

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
