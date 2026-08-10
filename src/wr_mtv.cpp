// wr_mtv.cpp  --  see wr_mtv.h.

#include "wr_mtv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "LzmaDec.h"

#define MTV_MAGIC "MMTV"
static const unsigned char kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};

// Fixed field offsets. Keep in sync with OFF_* in wrpath_extract.py.
#define OFF_VERSION   0x04
#define OFF_DATE_MS   0x08
#define OFF_MAPNAME   0x10
#define OFF_MAPHASH   0x50
#define OFF_GAMEMODE  0x79
#define OFF_TICKRATE  0x7B
#define OFF_STEAMID   0x7F
#define OFF_PLAYER    0x87
#define OFF_TRACKTYPE 0xA7
#define OFF_TRACKNUM  0xA8
#define OFF_RUNTIME   0xA9
#define OFF_TICKS     0xB1
#define OFF_END       0xB5      // one past the last fixed field

// The window the JSON blob is looked for in, and the largest length prefix that
// will be believed. Both are the reference's numbers.
#define JSON_SCAN_LO  0xB0
#define JSON_SCAN_HI  0x200
#define JSON_MAX_LEN  (1u << 20)

// ---------------------------------------------------------------------------
// Little bits
// ---------------------------------------------------------------------------
//
// Every multi-byte read goes through one of these rather than through a cast.
// Two of the fields are unaligned -- run_time is a double at 0xA9 and the JSON
// length prefix lands wherever the blob does -- and reading those as *(double*)
// is undefined behaviour that happens to work on x86 until a compiler notices.

static unsigned int Le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned long long Le64(const unsigned char *p)
{
    return (unsigned long long)Le32(p) |
           ((unsigned long long)Le32(p + 4) << 32);
}

static float LeF32(const unsigned char *p)
{
    unsigned int u = Le32(p);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static double LeF64(const unsigned char *p)
{
    unsigned long long u = Le64(p);
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

// The reference's _cstr: bytes up to the first NUL, or the whole field.
//
// No validation of what those bytes are, deliberately. The reference decodes
// them as UTF-8 with errors="replace" and asks no further questions, and the
// gates below are what actually decide whether a header is believable. A
// printable-ASCII check here would refuse files the reference accepts, which is
// a difference in which demos exist rather than in how they are read.
static void CStr(const unsigned char *src, int size, char *out, int cap)
{
    int n = 0;
    while (n < size && n < cap - 1 && src[n] != 0)
        n++;
    memcpy(out, src, (size_t)n);
    out[n] = '\0';
}

static void Fail(char *err, int errCap, const char *fmt, ...)
{
    if (!err || errCap <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(err, (size_t)errCap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

// Python's repr() of a float, which is what the "%r" in the tick-interval
// message produced: the shortest decimal string that reads back as the same
// double.
//
// Worth the fifteen lines because this string is a failure REASON, and a
// failure reason is written into paths\<map>\_failed.txt and compared against
// the record the reference wrote. "%.17g" would round-trip too, and would
// disagree with every such record on the file.
//
// Two known edges. A negative NaN prints as MSVC spells it rather than as
// Python does -- unreachable without a header whose float field is a corrupt
// bit pattern, which the gate is about to refuse anyway. And both the printing
// and the reading here follow the CRT's locale, so a process that has selected
// a comma decimal point would produce a comma; nothing in this DLL calls
// setlocale, and the game has never been observed to.
static void PyRepr(double v, char *out, int cap)
{
    for (int prec = 1; prec <= 17; prec++)
    {
        _snprintf_s(out, (size_t)cap, _TRUNCATE, "%.*g", prec, v);
        if (strtod(out, NULL) == v)
            break;
    }
    // repr always shows a float as a float: repr(3.0) is "3.0", where "%g"
    // gives "3". The n/i are there to leave "nan" and "inf" alone.
    if (!strpbrk(out, ".eEni"))
        strcat_s(out, (size_t)cap, ".0");
}

// ---------------------------------------------------------------------------
// The fixed header
// ---------------------------------------------------------------------------

bool WrMtvParseFixed(const unsigned char *head, size_t headLen,
                     long long fileSize, WrMtvHeader *out, char *err, int errCap)
{
    if (err && errCap > 0)
        err[0] = '\0';
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    // The reference's first line, and its wording. A short file, a file with
    // the wrong magic, and a file we could not read enough of are all the same
    // answer: this is not one of these.
    if (!head || fileSize < WR_MTV_MIN_BYTES || headLen < OFF_END ||
        memcmp(head, MTV_MAGIC, 4) != 0)
    {
        Fail(err, errCap, "not an MMTV file");
        return false;
    }

    out->version     = Le32(head + OFF_VERSION);
    out->dateMs      = (long long)Le64(head + OFF_DATE_MS);
    CStr(head + OFF_MAPNAME, 64, out->map, sizeof(out->map));
    CStr(head + OFF_MAPHASH, 41, out->mapHash, sizeof(out->mapHash));
    out->gamemode    = head[OFF_GAMEMODE];
    out->tickInterval = LeF32(head + OFF_TICKRATE);
    out->steamid64   = Le64(head + OFF_STEAMID);
    CStr(head + OFF_PLAYER, 32, out->player, sizeof(out->player));
    out->trackType   = head[OFF_TRACKTYPE];
    out->trackNum    = head[OFF_TRACKNUM];
    out->runTime     = LeF64(head + OFF_RUNTIME);
    out->ticks       = Le32(head + OFF_TICKS);

    // The gates. If these fail the offsets have moved, and every value read
    // above is a number from the wrong place rather than a wrong number.
    if (!out->map[0])
    {
        Fail(err, errCap, "empty map name field");
        return false;
    }

    // Widened, because the reference compares a float32 that struct.unpack has
    // already turned into a double. A NaN fails this the same way in both: no
    // comparison against it is true, so the range test is false and the header
    // is refused.
    double tick = (double)out->tickInterval;
    if (!(tick >= 0.001 && tick <= 0.1))
    {
        char r[64];
        PyRepr(tick, r, sizeof(r));
        Fail(err, errCap, "tick interval %s out of range", r);
        return false;
    }

    unsigned int high = (unsigned int)(out->steamid64 >> 32);
    if (high != 0x01100001u)
    {
        Fail(err, errCap, "steamid64 high word wrong (0x%X)", high);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The JSON blob, and therefore where the body starts
// ---------------------------------------------------------------------------

bool WrMtvLocateBlob(const unsigned char *data, size_t len, WrMtvHeader *io,
                     char *err, int errCap)
{
    if (err && errCap > 0)
        err[0] = '\0';
    if (!data || !io || len < JSON_SCAN_HI)
    {
        Fail(err, errCap, "not an MMTV file");
        return false;
    }

    io->codec = WR_MTV_CODEC_NONE;
    io->jsonStart = io->jsonLen = io->bodyOff = 0;

    // Every length we read, for the message if none of them works out. Four,
    // because that is how many the reference prints -- and only four are kept,
    // because only four are ever wanted and a candidate that is not printed is
    // not otherwise remembered.
    unsigned int tried[4];
    int triedShown = 0;

    for (size_t probe = JSON_SCAN_LO; probe < JSON_SCAN_HI; probe++)
    {
        if (data[probe] != '{')
            continue;

        unsigned int n = Le32(data + probe - 4);
        if (triedShown < 4)
            tried[triedShown++] = n;

        // probe + n + 4 cannot wrap: probe is under 0x200 and n is a u32, so
        // the sum fits a size_t on any target this builds for.
        if (n == 0 || n >= JSON_MAX_LEN || probe + n + 4 > len)
            continue;

        const unsigned char *tail = data + probe + n;
        if (memcmp(tail, "LZMA", 4) == 0)
            io->codec = WR_MTV_CODEC_LZMA;
        else if (memcmp(tail, kZstdMagic, 4) == 0)
            io->codec = WR_MTV_CODEC_ZSTD;
        else
            continue;

        io->jsonStart = probe;
        io->jsonLen = n;
        io->bodyOff = probe + n;
        return true;
    }

    if (triedShown == 0)
    {
        Fail(err, errCap, "no '{' in header window");
        return false;
    }

    char list[128];
    list[0] = '\0';
    for (int i = 0; i < triedShown; i++)
    {
        char one[24];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%u", i ? ", " : "", tried[i]);
        strcat_s(list, sizeof(list), one);
    }
    Fail(err, errCap, "no JSON blob in header window (lengths tried: %s)", list);
    return false;
}

bool WrMtvParseHeader(const unsigned char *data, size_t len, WrMtvHeader *out,
                      char *err, int errCap)
{
    if (!WrMtvParseFixed(data, len, (long long)len, out, err, errCap))
        return false;
    return WrMtvLocateBlob(data, len, out, err, errCap);
}

// ---------------------------------------------------------------------------
// Peeking
// ---------------------------------------------------------------------------

bool WrMtvPeek(const char *path, WrMtvHeader *out, char *err, int errCap)
{
    if (err && errCap > 0)
        err[0] = '\0';
    if (out)
        memset(out, 0, sizeof(*out));
    if (!path || !*path || !out)
    {
        Fail(err, errCap, "not an MMTV file");
        return false;
    }

    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Fail(err, errCap, "could not open the file");
        return false;
    }

    // Looped rather than one ReadFile, which on a local disk file would always
    // fill the buffer. A short read is only really possible over SMB, and a
    // demo library on a network share is not a strange thing to have -- but the
    // failure it would cause is: the file would be refused as "not an MMTV
    // file", which makes the demo vanish from the count rather than appear as a
    // problem. Three lines to remove a silent wrong answer.
    unsigned char buf[WR_MTV_PEEK_BYTES];
    DWORD got = 0;
    BOOL ok = TRUE;
    while (ok && got < sizeof(buf))
    {
        DWORD n = 0;
        ok = ReadFile(h, buf + got, (DWORD)sizeof(buf) - got, &n, NULL);
        if (!ok || n == 0)
            break;              // n == 0 is end of file, not a failure
        got += n;
    }
    CloseHandle(h);
    if (!ok)
    {
        Fail(err, errCap, "could not read the file");
        return false;
    }

    // `got` doubles as the size gate and costs nothing extra. A file at least
    // WR_MTV_MIN_BYTES long fills this buffer exactly, and a shorter one is not
    // a demo, so how much more there might be past the window never matters
    // here -- which is the whole reason this is one read and no stat.
    return WrMtvParseFixed(buf, got, (long long)got, out, err, errCap);
}

// ---------------------------------------------------------------------------
// LZMA
// ---------------------------------------------------------------------------

static void *WrLzmaAlloc(ISzAllocPtr, size_t size) { return malloc(size); }
static void WrLzmaFree(ISzAllocPtr, void *p) { free(p); }

bool WrMtvLzmaDecode(const unsigned char *src, size_t srcLen,
                     unsigned char *dst, size_t dstLen,
                     const unsigned char props[5], char *err, int errCap)
{
    if (err && errCap > 0)
        err[0] = '\0';

    // The reference asks its decompressor for at most `actual` bytes and then
    // checks it got them all. Asking for zero returns nothing and passes, so
    // that case never reaches the decoder there and does not reach it here --
    // which also keeps malloc(0) out of the caller.
    if (dstLen == 0)
        return true;
    if (!src || !dst || !props)
    {
        Fail(err, errCap, "Corrupt input data");
        return false;
    }

    ISzAlloc alloc;
    alloc.Alloc = WrLzmaAlloc;
    alloc.Free = WrLzmaFree;

    SizeT destLen = (SizeT)dstLen;
    SizeT usedSrc = (SizeT)srcLen;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;

    // LZMA_FINISH_ANY, not LZMA_FINISH_END, and this is the one decision in
    // this file worth arguing about. Valve's container carries no end marker,
    // so "the stream must be finished after destLen bytes" is a condition the
    // data was never built to satisfy -- and, more to the point, it is not the
    // condition the reference imposes. It asks for `actual` bytes and requires
    // only that it got them. A stricter test here would turn demos the
    // reference extracts into demos this refuses, which is a difference in
    // output dressed up as a safety check.
    SRes res = LzmaDecode(dst, &destLen, src, &usedSrc, props, 5,
                          LZMA_FINISH_ANY, &status, &alloc);

    if (res == SZ_OK && destLen == (SizeT)dstLen)
        return true;

    // Short output is a short read whatever caused it -- a truncated stream, or
    // an end marker arriving early. Same message, same numbers, same order as
    // the reference's own length check.
    if (res == SZ_OK || res == SZ_ERROR_INPUT_EOF)
    {
        Fail(err, errCap, "LZMA short read: %llu of %llu",
             (unsigned long long)destLen, (unsigned long long)dstLen);
        return false;
    }

    // The rest are liblzma's wordings, because that is what str(LZMAError)
    // gives the reference and what is therefore already sitting in people's
    // _failed.txt files. SZ_ERROR_UNSUPPORTED lines up exactly: the SDK
    // refuses a properties byte >= 225, and 225 is also where Python's
    // filter validation refuses one, because both are 9*5*5.
    if (res == SZ_ERROR_MEM)
        Fail(err, errCap, "Cannot allocate memory");
    else if (res == SZ_ERROR_UNSUPPORTED)
        Fail(err, errCap, "Invalid or unsupported options");
    else
        Fail(err, errCap, "Corrupt input data");
    return false;
}

// ---------------------------------------------------------------------------
// The body
// ---------------------------------------------------------------------------

unsigned char *WrMtvBody(const unsigned char *data, size_t len,
                         const WrMtvHeader *h, size_t *lenOut,
                         char *err, int errCap)
{
    if (lenOut)
        *lenOut = 0;
    if (err && errCap > 0)
        err[0] = '\0';
    if (!data || !h)
    {
        Fail(err, errCap, "not an MMTV file");
        return NULL;
    }

    if (h->codec == WR_MTV_CODEC_ZSTD)
    {
        // Ours, not the reference's. Its message names a pip package, which is
        // no longer advice anyone can act on. Callers that care about the
        // difference between a skip and a failure test h->codec instead of
        // reading this -- see the header.
        Fail(err, errCap, "zstd body (this build has no zstd decoder)");
        return NULL;
    }
    if (h->codec != WR_MTV_CODEC_LZMA)
    {
        Fail(err, errCap, "unknown codec");
        return NULL;
    }

    size_t off = h->bodyOff;

    // The reference reads these two u32s without checking there are twelve
    // bytes to read, so a file truncated inside the container makes it raise
    // struct.error -- which its own handler does not catch, and which takes
    // the whole run down rather than the demo. Bounds-checked here, and
    // reported as what it is.
    if (off + 17 > len)
    {
        Fail(err, errCap, "LZMA block runs past EOF");
        return NULL;
    }

    unsigned int actual = Le32(data + off + 4);
    unsigned int comp = Le32(data + off + 8);
    const unsigned char *props = data + off + 12;

    if ((size_t)off + 17 + comp > len)
    {
        Fail(err, errCap, "LZMA block runs past EOF");
        return NULL;
    }

    // No equivalent in the reference, which would simply try the allocation and
    // die of a MemoryError that aborts the whole run. A body this size cannot
    // be addressed by the scan downstream anyway; refusing it by name here
    // costs one demo instead of the run it was in.
    if (actual > WR_MTV_MAX_BODY)
    {
        Fail(err, errCap, "body claims %u bytes, over the %u byte limit",
             actual, (unsigned int)WR_MTV_MAX_BODY);
        return NULL;
    }

    // One spare byte so a zero-length body still has an allocation to hand
    // back, and so nothing downstream has to special-case a NULL that means
    // success.
    unsigned char *out = (unsigned char *)malloc((size_t)actual + 1);
    if (!out)
    {
        Fail(err, errCap, "Cannot allocate memory");
        return NULL;
    }
    out[actual] = 0;

    if (!WrMtvLzmaDecode(data + off + 17, comp, out, actual, props, err, errCap))
    {
        free(out);
        return NULL;
    }

    if (lenOut)
        *lenOut = actual;
    return out;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

unsigned char *WrMtvReadFile(const char *path, size_t *lenOut,
                             char *err, int errCap)
{
    if (lenOut)
        *lenOut = 0;
    if (err && errCap > 0)
        err[0] = '\0';

    FILE *f = NULL;
    if (!path || !*path || fopen_s(&f, path, "rb") != 0 || !f)
    {
        Fail(err, errCap, "could not open the file");
        return NULL;
    }

    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (n <= 0)
    {
        fclose(f);
        Fail(err, errCap, "not an MMTV file");
        return NULL;
    }
    if (n > (long long)WR_MTV_MAX_BODY)
    {
        fclose(f);
        Fail(err, errCap, "file is %lld bytes, over the %u byte limit",
             n, (unsigned int)WR_MTV_MAX_BODY);
        return NULL;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf)
    {
        fclose(f);
        Fail(err, errCap, "Cannot allocate memory");
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n)
    {
        free(buf);
        Fail(err, errCap, "could not read the file");
        return NULL;
    }

    if (lenOut)
        *lenOut = got;
    return buf;
}
