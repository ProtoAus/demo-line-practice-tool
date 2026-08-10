// wr_mtv.h  --  the .mtv container: what a demo is before any of it is floats.
//
// A Momentum demo (momentum\momtv\**\*.mtv) is three things bolted together:
//
//     0x000  a fixed header. Magic "MMTV", then the map, the player, the
//            gamemode, the tick interval, the run time -- all at constant
//            offsets in both container versions seen so far.
//     ~0xC6  a JSON blob of run statistics. Its byte length sits in the four
//            bytes immediately before it. Its START is NOT at a constant
//            offset: 0xC6 on v1 and 0xC7 on v2, and there is no field that
//            says which. So it is found by scanning, and validated by the
//            triple around it (see below).
//     +len   the run body, compressed. Valve-LZMA on everything current, zstd
//            on a small tail of older files.
//
// This file is the whole of that, and nothing above it: no floats, no scan, no
// dynamic program. That boundary is deliberate. Everything downstream of the
// decompressed body is approximate in the sense that it has to be checked
// statistically, and everything here is exact -- a byte is the right byte or it
// is not. Keeping the two apart is what makes the port checkable in layers,
// and it is why --dump-body exists on both implementations.
//
// FINDING THE JSON, AND WHY EVERY '{' IS TRIED
//
// The locator looks for a '{' anywhere in [0xB0, 0x200) and accepts it only if
// the u32 immediately BEFORE it is a plausible length and the four bytes at
// that length PAST it are a codec magic ("LZMA" or the zstd frame magic). That
// triple is self-validating and holds for both container versions.
//
// It tries every '{' in the window, not the first. The player name and the
// padding around it are arbitrary bytes and a stray '{' before the real blob is
// perfectly possible -- taking the first on faith is what produced "implausible
// JSON length 1076353433" on two demos in this library. A wrong '{' fails the
// length or the codec check, so the fix is simply to keep looking.
//
// THE SANITY GATES
//
// A non-empty map name, a tick interval in [0.001, 0.1], and a SteamID64 whose
// high word is 0x01100001. If those do not hold, the offsets have moved and
// nothing read out of this header means anything -- so it is refused rather
// than reported. These are cheap and they need only the first 0xB5 bytes, which
// is what lets WrMtvPeek apply them to four thousand files in the time it takes
// to change map.
//
// VALVE-LZMA IS NOT .lzma AND NOT .xz
//
// The body's first seventeen bytes are Valve's own container:
//
//     0  4  "LZMA"
//     4  4  the decompressed size, u32
//     8  4  the compressed size, u32
//    12  5  the standard LZMA properties blob (lc/lp/pb packed, then dictSize)
//    17     a raw LZMA1 stream, with NO end-of-stream marker
//
// So the size is known up front and the stream simply stops. That is why this
// cannot be handed to a .lzma reader, and why the decoder is told exactly how
// many bytes to produce.
//
// Those five property bytes at offset 12 ARE the standard props blob, byte for
// byte, so they go straight to LzmaDecode() rather than being unpacked into
// lc/lp/pb by hand as the reference implementation does. Provably the same
// thing, and it deletes the one place where a hand-rolled //9 //5 could be
// wrong.
//
// There is no dictionary-size clamp here, and there was going to be one. The
// dictSize in those props comes out of somebody else's file and can claim four
// gigabytes -- but LzmaDecode() decompresses straight into the destination
// buffer and allocates only the probability model, so a lying dictSize buys an
// attacker nothing to allocate. It survives only as a bound on how far back a
// match may reach, which the decoder already checks against how much output
// actually exists. The clamp would have been dead code defending a door that
// is not there. (The reference implementation needs one: Python's
// LZMADecompressor takes dict_size as an allocation.)
//
// ZSTD IS RECOGNISED AND NOT DECODED
//
// About 3.5% of the demos here have a zstd body, and reading them needs a
// second decompressor for a format that no current demo uses. They are a SKIP
// and never an error, which is not a detail: the reference does not write skips
// to _failed.txt, and recording them as failures would put a permanent entry in
// every user's failure record that --retry-failed would re-fail forever.

#ifndef WR_MTV_H
#define WR_MTV_H

#include "wr_common.h"

// Enough to hold every fixed field, so WrMtvPeek can gate on all of them.
// OFF_TICKS at 0xB1 plus its four bytes is 0xB5; the window is rounded up to
// the 0x200 the format's own minimum-size check uses, which is also what the
// JSON locator would want if it could work from a peek.
#define WR_MTV_PEEK_BYTES 0x200

// A file shorter than this is not a demo. The reference checks the same number
// before it reads a single field.
#define WR_MTV_MIN_BYTES 0x200

// The refusal point for a decompressed body, and the refusal point for a whole
// file read into memory.
//
// 512 MB is not an expectation. It is where a u32 bit position stops being able
// to address the body, which is the limit the scan downstream is built on, so a
// body over it has to become a named refusal here rather than a wrong answer
// four layers up.
//
// For scale: measured over the 400 largest demos in this library, the biggest
// LZMA body is 4.4 MB and the worst expansion is 3.07x. The largest file on
// disk is 48.8 MB, which would be 150 MB at that ratio -- and it is a zstd body
// anyway. 144 of those 400 are, including every one of the 60 biggest, which is
// the same observation as "zstd is what the very largest demos use".
#define WR_MTV_MAX_BODY (512u * 1024u * 1024u)

enum WrMtvCodec
{
    WR_MTV_CODEC_NONE = 0,
    WR_MTV_CODEC_LZMA,
    WR_MTV_CODEC_ZSTD           // recognised, deliberately not decoded
};

// Everything the container says. Sized from the reference's field widths: the
// map name field is 64 bytes, the hash 41, the player 32, and each gets one
// more for a terminator the file is not obliged to provide.
struct WrMtvHeader
{
    unsigned int version;
    long long dateMs;
    char map[65];
    char mapHash[42];
    int gamemode;
    float tickInterval;
    unsigned long long steamid64;
    char player[33];
    int trackType;
    int trackNum;
    double runTime;
    unsigned int ticks;

    // Filled in by the locator, which needs the whole file. Zero after a peek.
    WrMtvCodec codec;
    size_t jsonStart;
    size_t jsonLen;
    size_t bodyOff;             // where the codec container begins
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
//
// Every function below writes a reason into `err` when it returns false, and
// those strings are not decorative: they end up in paths\<map>\_failed.txt and
// have to match what the reference implementation would have written there,
// because a record with a different reason is a record that reads as a
// different failure.

// The fixed fields and the four gates, from the first WR_MTV_PEEK_BYTES bytes
// plus the file's total size. Leaves codec/jsonStart/jsonLen/bodyOff at zero.
//
// ON FAILURE, `out` STILL HOLDS WHATEVER WAS READ BEFORE THE GATE THAT REFUSED
// IT, and that is a promise callers rely on rather than an accident. There are
// two quite different failures here. "This is not a demo" happens first and
// leaves the struct zeroed -- there was nothing to read. A gate failure happens
// last, after every field has been read, and means the numbers are not
// believable, not that they are absent. The counter uses exactly that
// distinction: a demo with a broken header is still a demo belonging to the map
// whose name is in it, and should be counted against that map rather than
// disappearing from the total.
bool WrMtvParseFixed(const unsigned char *head, size_t headLen,
                     long long fileSize, WrMtvHeader *out, char *err, int errCap);

// The locator, which needs the whole file. Fills in the four remaining fields.
bool WrMtvLocateBlob(const unsigned char *data, size_t len, WrMtvHeader *io,
                     char *err, int errCap);

// Both of the above. This is parse_mtv_header.
bool WrMtvParseHeader(const unsigned char *data, size_t len, WrMtvHeader *out,
                      char *err, int errCap);

// The fixed half, read off disk. One 512-byte read and no allocation, for the
// counter -- which asks this of every .mtv in the install every time you change
// map, and must not decompress anything to do it.
bool WrMtvPeek(const char *path, WrMtvHeader *out, char *err, int errCap);

// ---------------------------------------------------------------------------
// The body
// ---------------------------------------------------------------------------

// Decompress it. malloc'd, and the caller free()s it.
//
// A zstd body is refused rather than decoded, and this is the ONE message in
// this file that is not the reference's wording: its version names a pip
// package, which is no longer advice anyone can act on. The difference is safe
// because that message cannot reach a failure record -- the reference turns a
// zstd body into a SKIP before it ever calls its own decompress_body, and skips
// are not written to _failed.txt. Callers that need to make the same
// distinction test h->codec instead of reading this; the refusal is the
// backstop, not the mechanism.
unsigned char *WrMtvBody(const unsigned char *data, size_t len,
                         const WrMtvHeader *h, size_t *lenOut,
                         char *err, int errCap);

// The codec on its own: a raw LZMA1 stream, five property bytes, and an output
// size known in advance. Exposed so tests\test_lzma.exe can drive it with a
// stream of its own and not have to build a container around it.
//
// Produces exactly dstLen bytes or fails. It does NOT require the stream to
// carry an end marker, and it does not mind trailing input -- the reference
// asks its decompressor for at most `actual` bytes and then checks it got them
// all, and this is that, in the same order.
bool WrMtvLzmaDecode(const unsigned char *src, size_t srcLen,
                     unsigned char *dst, size_t dstLen,
                     const unsigned char props[5], char *err, int errCap);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// Read a whole demo into memory. malloc'd, and the caller free()s it.
//
// Here rather than in the caller because the size limit belongs with the
// format: WR_MTV_MAX_BODY is the same number in both directions, and a file
// that cannot possibly be a demo should be turned away by the code that knows
// what a demo is.
unsigned char *WrMtvReadFile(const char *path, size_t *lenOut,
                             char *err, int errCap);

#endif // WR_MTV_H
