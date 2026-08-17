// wr_sha256.cpp  --  see wr_sha256.h.
//
// FIPS 180-2 section 6.2, transcribed. The names below are the specification's
// own -- k, w, a..h, the four sigmas -- because the only useful review of this
// file is somebody reading it beside the document, and renaming things to be
// friendlier would make that harder rather than easier.

#include "wr_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The first thirty-two bits of the fractional parts of the cube roots of the
// first sixty-four primes. FIPS 180-2, section 4.2.2.
static const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

// _rotr exists in <intrin.h> and MSVC recognises this shape anyway, but written
// out it is checkable against the spec and needs no header.
static inline uint32_t Ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void Compress(uint32_t *h, const unsigned char *p)
{
    uint32_t w[64];

    // Big-endian, and stated explicitly rather than cast through a uint32_t*.
    // x64 is little-endian, so a cast would be wrong here in a way that still
    // produces a plausible-looking digest.
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4 + 0] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8)  |  (uint32_t)p[i * 4 + 3];

    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 64; i++)
    {
        uint32_t S1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        uint32_t S0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;

        hh = g; g = f; f = e; e = d + t1;
        d = c;  c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void WrSha256Init(WrSha256 *s)
{
    if (!s)
        return;

    // Fractional parts of the square roots of the first eight primes.
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;

    s->bits = 0;
    s->used = 0;
    memset(s->block, 0, sizeof(s->block));
}

void WrSha256Update(WrSha256 *s, const void *data, size_t len)
{
    if (!s || (!data && len))
        return;

    const unsigned char *p = (const unsigned char *)data;
    s->bits += (uint64_t)len * 8u;

    // Top up a partial block first, then run whole blocks straight out of the
    // caller's buffer, then keep the remainder. This is the part that has to
    // agree with the one-shot call however the caller chops the input up, and
    // it is what the chunked case in tests\test_sha256.cpp exists to pin.
    if (s->used)
    {
        size_t want = 64u - s->used;
        size_t take = len < want ? len : want;
        memcpy(s->block + s->used, p, take);
        s->used += (unsigned int)take;
        p += take;
        len -= take;
        if (s->used < 64u)
            return;
        Compress(s->h, s->block);
        s->used = 0;
    }

    while (len >= 64u)
    {
        Compress(s->h, p);
        p += 64;
        len -= 64;
    }

    if (len)
    {
        memcpy(s->block, p, len);
        s->used = (unsigned int)len;
    }
}

void WrSha256Final(WrSha256 *s, unsigned char *out)
{
    if (!s || !out)
        return;

    uint64_t bits = s->bits;

    // 0x80, then zeros, then the length as a big-endian 64-bit count of bits.
    // Padding goes through Update so the block bookkeeping has one owner --
    // note bits is taken above, because these calls move it.
    unsigned char one = 0x80;
    WrSha256Update(s, &one, 1);
    static const unsigned char zeros[64] = {0};
    while (s->used != 56u)
        WrSha256Update(s, zeros, 1);

    unsigned char len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (unsigned char)(bits >> (56 - i * 8));
    WrSha256Update(s, len, 8);

    for (int i = 0; i < 8; i++)
    {
        out[i * 4 + 0] = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
}

void WrSha256Buffer(const void *data, size_t len, unsigned char *out)
{
    WrSha256 s;
    WrSha256Init(&s);
    WrSha256Update(&s, data, len);
    WrSha256Final(&s, out);
}

void WrSha256Hex(const unsigned char *digest, char *out, int cap)
{
    if (!out || cap < 1)
        return;
    out[0] = 0;
    if (!digest || cap < WR_SHA256_HEX)
        return;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < WR_SHA256_BYTES; i++)
    {
        out[i * 2 + 0] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[WR_SHA256_BYTES * 2] = 0;
}

bool WrSha256File(const char *path, char *hexOut, int cap)
{
    if (!path || !hexOut || cap < WR_SHA256_HEX)
        return false;

    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;

    // 64 KB at a time. The release binaries are megabytes and a demo can be
    // 48 MB; reading a whole file in to hash it would be a pointless allocation
    // inside the game's heap, and this runs on a worker either way.
    static const size_t CHUNK = 64u * 1024u;
    unsigned char *buf = (unsigned char *)malloc(CHUNK);
    if (!buf)
    {
        fclose(f);
        return false;
    }

    WrSha256 s;
    WrSha256Init(&s);

    bool ok = true;
    for (;;)
    {
        size_t got = fread(buf, 1, CHUNK, f);
        if (got)
            WrSha256Update(&s, buf, got);
        if (got < CHUNK)
        {
            ok = feof(f) != 0;       // a short read that is not EOF is an error
            break;
        }
    }

    free(buf);
    fclose(f);
    if (!ok)
        return false;

    unsigned char digest[WR_SHA256_BYTES];
    WrSha256Final(&s, digest);
    WrSha256Hex(digest, hexOut, cap);
    return true;
}

bool WrSha256HexEqual(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    for (int i = 0; i < WR_SHA256_BYTES * 2; i++)
    {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'F') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F') cb = (char)(cb - 'A' + 'a');
        if (ca != cb || !ca)
            return false;
    }
    // Both must end here. A 65-character "digest" is not a longer match.
    return a[WR_SHA256_BYTES * 2] == 0 && b[WR_SHA256_BYTES * 2] == 0;
}
