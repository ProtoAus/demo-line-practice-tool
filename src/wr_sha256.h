// wr_sha256.h  --  SHA-256, written out rather than linked or vendored.
//
// WHY THIS IS HERE AT ALL
//
// The updater downloads two binaries and has to be able to say whether what
// arrived is what the release published. That needs a hash, and this project
// had none.
//
// WHY NOT THE OBVIOUS ONES
//
// BCryptHashData would add BCRYPT.dll to the import list, and CryptCreateHash
// would add ADVAPI32.dll. Both are disqualified by the same sentence: the
// README tells a suspicious reader that this DLL imports exactly six system
// libraries and that ADVAPI32 is not among them, and .github\workflows
// \release.yml fails the build if that list ever changes. A hash is not worth
// spending the claim on.
//
// WHY HAND-WRITTEN IS SAFE HERE, WHEN IT IS NOT FOR A DECOMPRESSOR
//
// This project vendors miniz, the LZMA SDK and zstd instead of writing them,
// and third_party\VERSION.txt explains why: a decompressor that is subtly wrong
// does not fail, it produces slightly different bytes, and a slightly different
// demo body is a line drawn in the wrong place with nothing to notice it.
//
// SHA-256 is the opposite kind of code. It is one page of arithmetic against a
// published specification, and a wrong implementation does not produce a
// slightly wrong digest -- it fails the first test vector by every bit. FIPS
// 180-2 ships those vectors, tests\test_sha256.cpp runs them, and there is no
// state in between where it can be almost right. That is the same reasoning
// that put a JSON reader in src\ instead of a JSON library in third_party\.
//
// WHAT IT IS AND IS NOT FOR
//
// Integrity, not authenticity. It answers "did all the bytes arrive", and it
// lets the panel show a user the same digest the release page shows so they can
// compare by eye. It is not a signature and nothing here should imply it is:
// the manifest it is checked against arrives over the same connection as the
// files, so TLS to github.com is the trust anchor. See wr_update.h.

#ifndef WR_SHA256_H
#define WR_SHA256_H

#include "wr_common.h"

// 32 bytes raw, 64 characters printed, plus the terminator.
#define WR_SHA256_BYTES 32
#define WR_SHA256_HEX   65

struct WrSha256
{
    uint32_t h[8];
    uint64_t bits;              // message length so far, in bits
    unsigned char block[64];
    unsigned int used;          // bytes sitting in `block`
};

void WrSha256Init(WrSha256 *s);
void WrSha256Update(WrSha256 *s, const void *data, size_t len);

// Writes WR_SHA256_BYTES. The context is finished afterwards and must be
// re-initialised before it is used again.
void WrSha256Final(WrSha256 *s, unsigned char *out);

// The whole of a buffer, in one call.
void WrSha256Buffer(const void *data, size_t len, unsigned char *out);

// Lowercase hex, NUL-terminated. `cap` must be at least WR_SHA256_HEX.
void WrSha256Hex(const unsigned char *digest, char *out, int cap);

// A whole file, read in chunks so a 48 MB demo does not need 48 MB of heap.
// False if it could not be opened or read; `hexOut` is untouched then.
bool WrSha256File(const char *path, char *hexOut, int cap);

// Case-insensitive compare of two 64-character hex digests. Written out because
// the two sides come from different places -- one from WrSha256Hex, one off a
// line of somebody's SHA256SUMS.txt -- and a case difference in a manifest is
// not a mismatch.
bool WrSha256HexEqual(const char *a, const char *b);

#endif // WR_SHA256_H
