// test_sha256.cpp  --  the hash, against the vectors that define it.
//
// src\wr_sha256.h argues that hand-writing SHA-256 is safe where hand-writing a
// decompressor would not be, and the argument rests entirely on this file: the
// claim is that a wrong implementation cannot be subtly wrong, it fails the
// first published vector outright. That is only worth anything if the vectors
// are actually run, so they are, and so is every seam around them:
//
//   1. The FIPS 180-2 appendix vectors. "abc" is one block, the 56-byte string
//      is the case where the length field does not fit beside the padding and a
//      second block is needed, and the million 'a's is the multi-block path.
//   2. The empty message. Nothing to compress, only padding, and the case a
//      loop written as do/while gets wrong.
//   3. Chunked feeding must equal the one-shot. This is the one that matters
//      for the updater, because WrSha256File reads 64 KB at a time and the
//      chunk boundary lands wherever the file size puts it -- so the same bytes
//      are fed in every awkward split and must agree every time.
//   4. WrSha256File against bytes just written to disk, and the digest of a
//      file exactly 64 bytes long -- the size where the last block is full and
//      padding starts a new one.
//   5. WrSha256HexEqual is case-insensitive but not length-blind.
//
// Build:  tests\build.bat
// Run:    tests\test_sha256.exe

#include "wr_sha256.h"

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

static void HexOf(const void *data, size_t len, char *out)
{
    unsigned char d[WR_SHA256_BYTES];
    WrSha256Buffer(data, len, d);
    WrSha256Hex(d, out, WR_SHA256_HEX);
}

static void CheckHash(const void *data, size_t len, const char *want,
                      const char *what)
{
    char got[WR_SHA256_HEX];
    HexOf(data, len, got);
    bool ok = strcmp(got, want) == 0;
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
    {
        printf("      want %s\n", want);
        printf("      got  %s\n", got);
        g_failures++;
    }
}

int main(void)
{
    printf("\n=== FIPS 180-2 vectors ===\n");

    CheckHash("", 0,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "the empty message");

    CheckHash("abc", 3,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "\"abc\"");

    CheckHash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "the 56-byte string (two blocks)");

    {
        // A million 'a'. The length field alone is 8000000 bits, which is the
        // case that catches a 32-bit bit counter.
        size_t n = 1000000;
        char *a = (char *)malloc(n);
        memset(a, 'a', n);
        CheckHash(a, n,
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                  "one million 'a' (15625 blocks)");
        free(a);
    }

    printf("\n=== block boundaries ===\n");

    {
        // 55, 56, 57, 63, 64 and 65 bytes: either side of the point where the
        // 8-byte length no longer fits in the block the message ends in.
        static const struct { size_t len; const char *want; } cases[] = {
            { 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
            { 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
            { 57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6" },
            { 63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
            { 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
            { 65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
        };
        for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
        {
            char msg[80];
            memset(msg, 'a', cases[i].len);
            char what[64];
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%zu bytes of 'a'",
                        cases[i].len);
            CheckHash(msg, cases[i].len, cases[i].want, what);
        }
    }

    printf("\n=== chunked feeding equals one-shot ===\n");

    {
        // 4096 bytes of something that is not all the same byte, fed in every
        // chunk size from 1 to 100 and again in two uneven halves. Any of these
        // that disagrees is a bug in the partial-block bookkeeping, which is
        // the only stateful part of the file.
        size_t n = 4096;
        unsigned char *buf = (unsigned char *)malloc(n);
        for (size_t i = 0; i < n; i++)
            buf[i] = (unsigned char)(i * 31u + (i >> 5));

        char want[WR_SHA256_HEX];
        HexOf(buf, n, want);

        int bad = 0;
        for (size_t chunk = 1; chunk <= 100; chunk++)
        {
            WrSha256 s;
            WrSha256Init(&s);
            for (size_t at = 0; at < n; at += chunk)
            {
                size_t take = n - at < chunk ? n - at : chunk;
                WrSha256Update(&s, buf + at, take);
            }
            unsigned char d[WR_SHA256_BYTES];
            char got[WR_SHA256_HEX];
            WrSha256Final(&s, d);
            WrSha256Hex(d, got, sizeof(got));
            if (strcmp(got, want) != 0)
                bad++;
        }
        Check(bad == 0, "every chunk size from 1 to 100 agrees");

        {
            // The 64 KB read in WrSha256File, in miniature: one big piece and
            // an awkward tail.
            WrSha256 s;
            WrSha256Init(&s);
            WrSha256Update(&s, buf, 4000);
            WrSha256Update(&s, buf + 4000, 96);
            unsigned char d[WR_SHA256_BYTES];
            char got[WR_SHA256_HEX];
            WrSha256Final(&s, d);
            WrSha256Hex(d, got, sizeof(got));
            Check(strcmp(got, want) == 0, "4000 + 96 agrees");
        }

        {
            // Zero-length updates in the middle must change nothing.
            WrSha256 s;
            WrSha256Init(&s);
            WrSha256Update(&s, buf, 0);
            WrSha256Update(&s, buf, 1000);
            WrSha256Update(&s, buf + 1000, 0);
            WrSha256Update(&s, buf + 1000, n - 1000);
            unsigned char d[WR_SHA256_BYTES];
            char got[WR_SHA256_HEX];
            WrSha256Final(&s, d);
            WrSha256Hex(d, got, sizeof(got));
            Check(strcmp(got, want) == 0, "zero-length updates change nothing");
        }

        free(buf);
    }

    printf("\n=== WrSha256File ===\n");

    {
        char path[MAX_PATH];
        char tmp[MAX_PATH];
        GetTempPathA(sizeof(tmp), tmp);
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%swr_sha256_test.bin", tmp);

        // Something bigger than one block and not a multiple of one, so the
        // file path exercises the same tail the buffer path does.
        size_t n = 200000;
        unsigned char *buf = (unsigned char *)malloc(n);
        for (size_t i = 0; i < n; i++)
            buf[i] = (unsigned char)(i ^ (i >> 8));

        FILE *f = NULL;
        fopen_s(&f, path, "wb");
        if (f)
        {
            fwrite(buf, 1, n, f);
            fclose(f);
        }
        Check(f != NULL, "wrote the temporary file");

        char want[WR_SHA256_HEX];
        HexOf(buf, n, want);

        char got[WR_SHA256_HEX];
        Check(WrSha256File(path, got, sizeof(got)), "WrSha256File succeeded");
        Check(strcmp(got, want) == 0, "the file hashes to what the buffer does");

        // Exactly 64 KB: the chunk size, so the last read returns 0 bytes and
        // the loop has to end on feof rather than on a short read.
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%swr_sha256_test64k.bin",
                    tmp);
        fopen_s(&f, path, "wb");
        if (f)
        {
            fwrite(buf, 1, 65536, f);
            fclose(f);
        }
        HexOf(buf, 65536, want);
        Check(WrSha256File(path, got, sizeof(got)) &&
              strcmp(got, want) == 0, "exactly one 64 KB chunk");
        DeleteFileA(path);

        _snprintf_s(path, sizeof(path), _TRUNCATE, "%swr_sha256_test.bin", tmp);
        DeleteFileA(path);
        free(buf);

        char none[WR_SHA256_HEX] = "untouched";
        Check(!WrSha256File("Z:\\no\\such\\file\\anywhere.bin", none,
                            sizeof(none)),
              "a missing file is false, not a crash");
        Check(strcmp(none, "untouched") == 0, "and it did not write the buffer");
    }

    printf("\n=== WrSha256HexEqual ===\n");

    {
        const char *lower = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        const char *upper = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
        const char *other = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b854";
        char longer[80];
        _snprintf_s(longer, sizeof(longer), _TRUNCATE, "%s0", lower);

        Check(WrSha256HexEqual(lower, lower), "a digest equals itself");
        Check(WrSha256HexEqual(lower, upper), "case does not matter");
        Check(!WrSha256HexEqual(lower, other), "one nibble apart is not equal");
        Check(!WrSha256HexEqual(lower, longer), "a longer string is not equal");
        Check(!WrSha256HexEqual(lower, ""), "empty is not equal");
        Check(!WrSha256HexEqual(NULL, lower), "NULL is not equal");
    }

    printf("\n%s  (%d failure%s)\n\n", g_failures ? "FAILED" : "all ok",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
