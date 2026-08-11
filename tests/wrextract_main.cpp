// wrextract_main.cpp  --  the extractor, on a command line. NOT SHIPPED.
//
// The same functions the DLL calls, driven from a console instead of from a
// button, so that what the port produces can be diffed against what
// wrpath_extract.py produces. That comparison is the only real evidence the
// port is correct, and without this there is no way to make it: you cannot pipe
// a game's overlay into fc.
//
// DELIBERATELY NOT IN THE RELEASE ZIP. A second unsigned executable next to an
// unsigned injector would undo a good part of the work that went into making
// the download explicable, and nothing a user does needs this. It is built by
// tests\build.bat and it stays in tests\.
//
// Its flags are wrpath_extract.py's, with the same spelling, so the parity
// driver can hand the same argv to both:
//
//     --map / --file / --all       extract
//     --index-maps                 the map index
//     --board                      a leaderboard window
//     --fetch                      demos onto disk
//     --dump-body / --dump-cands   one layer of one demo, in a file you can diff
//     --dump-chain / --dump-info
//     --api-record / --api-replay  a recorded conversation
//
// THE DUMPS ARE THE REASON ANY OF THIS WORKED. The pipeline is container ->
// LZMA -> a scan for float triples -> a dynamic program over them -> a scoring
// pass -> reassembly -> a file. Six layers, and only the last one is visible
// from outside. Port all six and compare the .wrpath, and a mismatch tells you
// that a byte differs somewhere in half a million floats. Every layer has an
// exact oracle available, and none of them is reachable without a flag that
// prints it:
//
//     --dump-body    bytes. No floats are involved yet, so this isolates the
//                    whole container and LZMA question with no ambiguity at
//                    all. Run it first, over everything.
//     --dump-cands   the scan. Pins the bit <-> (phase, q, word) mapping and
//                    the end bound, which is the subtlest thing in the port.
//     --dump-chain   the chosen chain and the harvested segments, as bit
//                    positions. Pins the DP and the origin oracle.
//     --dump-info    everything a run decided, one TSV row per demo. Also how
//                    the memory budget in wr_jobs.h was measured across a real
//                    library rather than guessed at.
//
// Floats are %.17g in the chain and info dumps and %.9g in the candidate dump,
// and the difference is deliberate. A float32 round-trips exactly through
// %.9g and the candidates ARE float32; the others are doubles, and a divergence
// in a compensated summation shows up in the tenth digit, which is precisely
// what %.9g would hide.
//
// --board is the same idea one level up. A leaderboard changes under you, so it
// is not an oracle by itself; --api-replay makes it one, by answering both
// implementations from the same recorded bytes. See tests\api_tape.h.
//
// There is no --out, deliberately, and it is the one flag this refuses rather
// than accepts. The reference derives its output directory from it; this one
// uses WrDataPath, which puts everything under wrlines_data\ next to the .exe.
// Accepting --out and ignoring it would be the single thing this file must
// never do -- quietly behave differently from the flag it was handed.
// parity.ps1 gives --out to the reference only, runs this from a scratch
// directory of its own, and compares the two trees wherever each of them
// landed.
//
// Build:  tests\build.bat
// Run:    tests\wrextract.exe --game "<install>" --index-maps

#include "wr_api.h"
#include "wr_demo.h"
#include "wr_dp.h"
#include "wr_extract.h"
#include "wr_fetch.h"
#include "wr_maps.h"
#include "wr_msml.h"
#include "wr_mtv.h"
#include "wr_log.h"
#include "api_tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The one stub, and it is the same short block the harnesses use. The .wrpath
// writer lives beside its reader in wr_path.cpp, which drags in the run store,
// which drags in the energy sampler, which asks the engine where the camera is
// looking. Nothing on this path ever calls it.
bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }

// Progress goes to stdout, unbuffered per line. The DLL points the same emit
// hook at the panel's line ring; this is the other end of that indirection, and
// the reason stdout parity is testable at all.
static void EmitStdout(const char *line)
{
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// The reference does os.makedirs(dirname(abspath(dest))) before it writes a
// dump, so a driver can name an output path in a directory that does not exist
// yet. Same here, one component at a time.
static void MakeTree(const char *path)
{
    char buf[MAX_PATH];
    strcpy_s(buf, sizeof(buf), path);
    for (char *p = buf; *p; p++)
    {
        if (*p != '\\' && *p != '/')
            continue;
        char was = *p;
        *p = '\0';
        if (buf[0] && !(buf[1] == ':' && buf[2] == '\0'))
            CreateDirectoryA(buf, NULL);
        *p = was;
    }
}

static void Usage(void)
{
    printf("wrextract -- the wrlines extractor, without the game.\n"
           "\n"
           "  --game PATH        the Momentum install (required)\n"
           "\n"
           "  --map NAME         extract every demo of this map\n"
           "  --file PATH        ... or exactly this one\n"
           "  --all              ... or every demo on disk\n"
           "  --skip-existing    skip demos already done, and recorded failures\n"
           "  --retry-failed     with --skip-existing, try the failures anyway\n"
           "  --verify           extract and report, but write nothing\n"
           "  --jobs N           workers; 0 decides, 1 is serial and comparable\n"
           "  --timeout N        give up on one demo after N seconds (0: never)\n"
           "  --limit N          stop after N demos\n"
           "\n"
           "  --index-maps       rebuild the map index from the game's cache\n"
           "  --dump-body PATH   write that demo's decompressed run body\n"
           "  --dump-cands PATH  ... its coordinate-triple candidates\n"
           "  --dump-chain PATH  ... its chosen chain and harvested segments\n"
           "  --dump-info PATH   what each run decided, one TSV row per demo\n"
           "\n"
           "  --board            cache a window of a map's leaderboard\n"
           "  --map NAME         which map\n"
           "  --map-id N         its Momentum id, if the catalogue has no name\n"
           "  --gamemode N       1 is surf\n"
           "  --track-type N     0 main, 1 stage, 2 bonus\n"
           "  --track-num N      which stage or bonus\n"
           "  --from-rank N      first place to take, 1-based\n"
           "  --count N          how many places\n"
           "  --slowest          take the LAST places instead of the first\n"
           "  --spread N         sample N places evenly across the board\n"
           "  --friends          look up wrlines_data\\friends.txt instead\n"
           "  --refresh          discard what is cached rather than adding\n"
           "\n"
           "  --fetch            download demos this machine does not have\n"
           "  --dry-run          with --fetch, list them and stop\n"
           "  --ranks SPEC       with --fetch, these places from the cache\n"
           "  --ranks-file PATH  the same, read from a file\n"
           "  --top N            with --fetch, consider the top N\n"
           "  --into-game        also copy each demo where the game can see it\n"
           "\n"
           "  --api-record DIR   save every leaderboard reply under DIR\n"
           "  --api-replay DIR   answer every request from DIR, never the net\n"
           "\n"
           "Not shipped. Flags mirror wrpath_extract.py so the two can be run\n"
           "against each other. --out is REFUSED rather than ignored: output\n"
           "goes under wrlines_data next to this .exe, and a flag that is\n"
           "accepted and then not obeyed is the one thing this must never do.\n");
}

// wrpath_extract.py --dump-body, including the line it prints and the code it
// exits with. The reference does not catch a header failure on this path, so it
// dies with a traceback where this prints one line -- tests\parity.ps1 treats a
// reference traceback as "no oracle for this demo" rather than as a mismatch,
// and checks only that both sides refused.
static int DumpBody(const char *file, const char *dest)
{
    char err[256] = "";
    size_t len = 0;
    unsigned char *data = WrMtvReadFile(file, &len, err, sizeof(err));
    if (!data)
    {
        printf("[!] %s: %s\n", file, err);
        return 1;
    }

    WrMtvHeader h;
    if (!WrMtvParseHeader(data, len, &h, err, sizeof(err)))
    {
        printf("[!] %s: %s\n", file, err);
        free(data);
        return 1;
    }

    if (h.codec == WR_MTV_CODEC_ZSTD)
    {
        // The reference's own line, verbatim. It is the one place a skip is
        // printed on this path, and a parity run over a real library meets it
        // on about one demo in thirty.
        printf("[!] zstd body and no zstandard installed\n");
        free(data);
        return 1;
    }

    size_t bodyLen = 0;
    unsigned char *body = WrMtvBody(data, len, &h, &bodyLen, err, sizeof(err));
    free(data);
    if (!body)
    {
        printf("[!] %s: %s\n", file, err);
        return 1;
    }

    MakeTree(dest);
    FILE *f = NULL;
    if (fopen_s(&f, dest, "wb") != 0 || !f)
    {
        printf("[!] could not write %s\n", dest);
        free(body);
        return 1;
    }
    bool wrote = (fwrite(body, 1, bodyLen, f) == bodyLen);
    fclose(f);
    free(body);
    if (!wrote)
    {
        printf("[!] short write to %s\n", dest);
        return 1;
    }

    printf("%zu bytes -> %s\n", bodyLen, dest);
    return 0;
}

// ---------------------------------------------------------------------------
// The three dumps above --dump-body
// ---------------------------------------------------------------------------

// Binary mode on purpose. The reference opens these with newline="\n", so the
// files are LF even on Windows, and text mode here would make every line differ
// by one byte.
static FILE *OpenDump(const char *dest)
{
    MakeTree(dest);
    FILE *f = NULL;
    if (fopen_s(&f, dest, "wb") != 0 || !f)
    {
        printf("[!] could not write %s\n", dest);
        return NULL;
    }
    return f;
}

// Everything up to the decompressed body, with the reference's own refusals.
static unsigned char *ReadBody(const char *file, WrMtvHeader *h, size_t *lenOut,
                               size_t *fileLenOut)
{
    char err[256] = "";
    size_t len = 0;
    unsigned char *data = WrMtvReadFile(file, &len, err, sizeof(err));
    if (!data)
    {
        printf("[!] %s: %s\n", file, err);
        return NULL;
    }
    if (fileLenOut)
        *fileLenOut = len;

    if (!WrMtvParseHeader(data, len, h, err, sizeof(err)))
    {
        printf("[!] %s: %s\n", file, err);
        free(data);
        return NULL;
    }
    if (h->codec == WR_MTV_CODEC_ZSTD)
    {
        printf("[!] zstd body and no zstandard installed\n");
        free(data);
        return NULL;
    }
    unsigned char *body = WrMtvBody(data, len, h, lenOut, err, sizeof(err));
    free(data);
    if (!body)
        printf("[!] %s: %s\n", file, err);
    return body;
}

static int DumpCands(const char *file, const char *dest)
{
    WrMtvHeader h;
    size_t bodyLen = 0;
    unsigned char *body = ReadBody(file, &h, &bodyLen, NULL);
    if (!body)
        return 1;

    WrDpCand *cands = NULL;
    int n = 0, stop = 0;
    char err[256] = "";
    if (!WrDpScan(body, bodyLen, WR_DP_SCAN_START_BYTE, &cands, &n, &stop,
                  NULL, NULL, err, sizeof(err)))
    {
        printf("[!] %s: %s\n", file, err);
        free(body);
        return 1;
    }
    free(body);

    FILE *f = OpenDump(dest);
    if (!f)
    {
        free(cands);
        return 1;
    }
    fprintf(f, "# bit\tx\ty\tz\n");
    fprintf(f, "# count\t%d\n", n);
    for (int i = 0; i < n; i++)
        fprintf(f, "%u\t%.9g\t%.9g\t%.9g\n", cands[i].bit, (double)cands[i].x,
                (double)cands[i].y, (double)cands[i].z);
    fclose(f);
    free(cands);

    printf("%d candidates -> %s\n", n, dest);
    return 0;
}

// The reference's _dump_val: a bool is 1 or 0, a float is %.17g, None is "-".
static void DumpReal(char *out, int cap, bool have, double v)
{
    if (have)
        _snprintf_s(out, (size_t)cap, _TRUNCATE, "%.17g", v);
    else
        strcpy_s(out, (size_t)cap, "-");
}

static int DumpChain(const char *file, const char *dest, double timeout)
{
    WrMtvHeader h;
    size_t bodyLen = 0;
    unsigned char *body = ReadBody(file, &h, &bodyLen, NULL);
    if (!body)
        return 1;

    char err[256] = "";
    WrDemoJson js;
    // The JSON has to be re-read here because ReadBody freed the file buffer it
    // lived in; cheaper than keeping a megabyte alive for one number.
    memset(&js, 0, sizeof(js));
    {
        size_t len = 0;
        unsigned char *data = WrMtvReadFile(file, &len, err, sizeof(err));
        if (data)
        {
            WrDemoParseJson((const char *)data + h.jsonStart, h.jsonLen, &js);
            free(data);
        }
    }

    WrDpArgs a;
    memset(&a, 0, sizeof(a));
    a.body = body;
    a.bodyLen = bodyLen;
    a.tickInterval = (double)h.tickInterval;
    a.ticks = h.ticks;
    a.haveRef = js.haveRef;
    a.refMaxHoriz = js.refMaxHoriz;
    a.timeoutSeconds = timeout;
    a.keepDetail = true;

    WrDpResult r;
    bool cancelled = false;
    const bool got = WrDpExtract(&a, &r, &cancelled, err, sizeof(err));
    WrDemoFreeJson(&js);
    free(body);
    if (!got)
    {
        printf("[!] %s: %s\n", file, err);
        return 1;
    }

    FILE *f = OpenDump(dest);
    if (!f)
    {
        WrDpFree(&r);
        return 1;
    }

    char v[64];
    fprintf(f, "# identified_by\t%s\n",
            r.info.identifiedBy[0] ? r.info.identifiedBy : "-");
    fprintf(f, "# rounds\t%d\n", r.info.rounds);
    DumpReal(v, sizeof(v), r.info.haveDeriv, r.info.derivRate);
    fprintf(f, "# deriv_rate\t%s\n", v);
    if (r.info.haveDeriv)
        fprintf(f, "# deriv_offset\t%d\n", r.info.derivOffset);
    else
        fprintf(f, "# deriv_offset\t-\n");
    fprintf(f, "# chain\t%d\n", r.chainCount);
    for (int i = 0; i < r.chainCount; i++)
        fprintf(f, "c\t%u\n", r.chain[i]);
    fprintf(f, "# segments\t%d\n", r.segCount);
    for (int i = 0; i < r.segCount; i++)
    {
        const unsigned int *seg = r.segBits + r.segOff[i];
        const int n = r.segLen[i];
        fprintf(f, "s\t%d\t%d\t%d\t%d\n", i, n, n ? (int)seg[0] : -1,
                n ? (int)seg[n - 1] : -1);
        for (int k = 0; k < n; k++)
            fprintf(f, "b\t%d\t%u\n", i, seg[k]);
    }
    fclose(f);

    printf("chain %d, %d segments -> %s\n", r.chainCount, r.segCount, dest);
    WrDpFree(&r);
    return 0;
}

static const char *kInfoKeys =
    "candidates\tticks\trounds\tconfident\tidentified_by\tderiv_rate\t"
    "deriv_offset\tsegments\tfirst_segment\tref_max_horiz\tchain_max_horiz\t"
    "match_error\tsamples\tcoverage\tpath_length\tmarkers\tmarkers_ok\t"
    "start_index\tstart_ok\tpreroll\tflagged\twhy_flagged";

#define WR_INFO_KEY_COUNT 22

static void InfoRow(FILE *f, const char *name, const WrDemoResult *r)
{
    const WrDpInfo *i = &r->dp.info;
    char rate[64], off[64], ref[64];
    DumpReal(rate, sizeof(rate), i->haveDeriv, i->derivRate);
    if (i->haveDeriv)
        _snprintf_s(off, sizeof(off), _TRUNCATE, "%d", i->derivOffset);
    else
        strcpy_s(off, sizeof(off), "-");
    DumpReal(ref, sizeof(ref), i->haveRef, i->refMaxHoriz);

    fprintf(f, "%s\t%zu\t%zu\tok\t"
               "%d\t%u\t%d\t%d\t%s\t%s\t%s\t%d\t%d\t%s\t%.17g\t%.17g\t"
               "%d\t%.17g\t%.17g\t%d\t%d\t%d\t%d\t%.17g\t%d\t%s\n",
            name, r->fileBytes, r->bodyBytes,
            i->candidates, i->ticks, i->rounds, i->confident ? 1 : 0,
            i->identifiedBy[0] ? i->identifiedBy : "-", rate, off,
            i->segments, i->firstSegment, ref, i->chainMaxHoriz, i->matchError,
            i->samples, i->coverage, i->pathLength,
            r->markerCount, r->markersOk ? 1 : 0, r->startIndex,
            r->startOk ? 1 : 0, r->preroll, r->flagged ? 1 : 0,
            r->whyFlagged[0] ? r->whyFlagged : "-");
}

static void InfoErrorRow(FILE *f, const char *name, long long fileBytes,
                         const char *why)
{
    char clean[256];
    strcpy_s(clean, sizeof(clean), why);
    for (char *p = clean; *p; p++)
        if (*p == '\t' || *p == '\n')
            *p = ' ';
    fprintf(f, "%s\t%lld\t0\t%s", name, fileBytes, clean);
    for (int k = 0; k < WR_INFO_KEY_COUNT; k++)
        fprintf(f, "\t-");
    fprintf(f, "\n");
}

// _dump_targets: the same selection cmd_extract makes, WITHOUT the skip rules,
// and sorted rather than left in walk order. Two different orders on purpose --
// a dump is a diff of one file per row and wants a stable sort; an extraction
// is a progress line per demo and wants the order the reference processes them
// in.
struct InfoTargets
{
    char **v;
    int n, cap;
    const char *map;
};

static void InfoCollect(void *user, const char *path, long long)
{
    InfoTargets *t = (InfoTargets *)user;
    if (t->map && *t->map)
    {
        WrMtvHeader hdr;
        char why[128];
        WrMtvPeek(path, &hdr, why, sizeof(why));
        if (_stricmp(hdr.map, t->map) != 0)
            return;
    }
    if (t->n == t->cap)
    {
        int grown = t->cap ? t->cap * 2 : 256;
        char **bigger = (char **)realloc(t->v, sizeof(char *) * (size_t)grown);
        if (!bigger)
            return;
        t->v = bigger;
        t->cap = grown;
    }
    t->v[t->n] = _strdup(path);
    if (t->v[t->n])
        t->n++;
}

static int CompareStr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static long long SizeOf(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &ad))
        return -1;
    return ((long long)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
}

static int DumpInfo(const char *game, const char *map, const char *file,
                    int limit, double timeout, const char *dest)
{
    InfoTargets t;
    memset(&t, 0, sizeof(t));
    t.map = map;

    if (file)
        InfoCollect(&t, file, 0);
    else
    {
        // The map filter goes through t.map, not through here.
        WrExtractWalkDemos(game, InfoCollect, &t);
        if (t.n > 1)
            qsort(t.v, (size_t)t.n, sizeof(char *), CompareStr);
    }
    if (limit > 0 && t.n > limit)
        t.n = limit;

    if (t.n == 0)
    {
        printf("[!] nothing selected -- pass --file PATH, or --map NAME, or "
               "--all\n");
        return 1;
    }

    FILE *f = OpenDump(dest);
    if (!f)
        return 1;
    fprintf(f, "demo\tfile_bytes\tbody_bytes\tstatus\t%s\n", kInfoKeys);

    int rows = 0;
    for (int i = 0; i < t.n; i++)
    {
        const char *name = t.v[i];
        for (const char *p = t.v[i]; *p; p++)
            if (*p == '\\' || *p == '/')
                name = p + 1;

        WrDemoArgs a;
        memset(&a, 0, sizeof(a));
        a.outDir = "";
        a.verify = true;            // report everything, write nothing
        a.timeoutSeconds = timeout;

        WrDemoResult r;
        const WrDemoOutcome oc = WrDemoProcess(t.v[i], &a, &r);
        if (oc == WR_DEMO_OK)
        {
            InfoRow(f, name, &r);
        }
        else
        {
            // The reference raises MtvError("skip: zstd body") on THIS path
            // rather than returning a skip, so the status column says that and
            // not what the extractor's own skip line says.
            InfoErrorRow(f, name, SizeOf(t.v[i]),
                         oc == WR_DEMO_SKIP ? "skip: zstd body" : r.message);
        }
        WrDemoFree(&r);

        rows++;
        if (rows % 100 == 0)
        {
            printf("[%d/%d]\n", rows, t.n);
            fflush(stdout);
        }
    }
    fclose(f);

    for (int i = 0; i < t.n; i++)
        free(t.v[i]);
    free(t.v);

    printf("%d demos -> %s\n", rows, dest);
    return 0;
}

int main(int argc, char **argv)
{
    const char *game = NULL;
    const char *file = NULL;
    const char *dumpBody = NULL;
    const char *dumpCands = NULL;
    const char *dumpChain = NULL;
    const char *dumpInfo = NULL;
    const char *apiRecord = NULL;
    const char *apiReplay = NULL;
    bool indexMaps = false;

    bool all = false, verify = false, skipExisting = false, retryFailed = false;
    int jobs = 0, limit = 0;
    double timeout = 30.0;      // the reference's default

    bool board = false, slowest = false, friends = false, refresh = false;
    bool fetch = false, dryRun = false, intoGame = false;
    const char *map = NULL;
    const char *ranksSpec = NULL;
    const char *ranksFile = NULL;
    int mapId = 0, gamemode = 1, trackType = 0, trackNum = 1;
    int fromRank = 0, count = 0, spread = 0, top = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--game") == 0 && i + 1 < argc)
            game = argv[++i];
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            file = argv[++i];
        else if (strcmp(argv[i], "--dump-body") == 0 && i + 1 < argc)
            dumpBody = argv[++i];
        else if (strcmp(argv[i], "--dump-cands") == 0 && i + 1 < argc)
            dumpCands = argv[++i];
        else if (strcmp(argv[i], "--dump-chain") == 0 && i + 1 < argc)
            dumpChain = argv[++i];
        else if (strcmp(argv[i], "--dump-info") == 0 && i + 1 < argc)
            dumpInfo = argv[++i];
        else if (strcmp(argv[i], "--index-maps") == 0)
            indexMaps = true;
        else if (strcmp(argv[i], "--all") == 0)
            all = true;
        else if (strcmp(argv[i], "--verify") == 0)
            verify = true;
        else if (strcmp(argv[i], "--skip-existing") == 0)
            skipExisting = true;
        else if (strcmp(argv[i], "--retry-failed") == 0)
            retryFailed = true;
        else if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc)
            jobs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout = atof(argv[++i]);
        else if (strcmp(argv[i], "--board") == 0)
            board = true;
        else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc)
            map = argv[++i];
        else if (strcmp(argv[i], "--map-id") == 0 && i + 1 < argc)
            mapId = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gamemode") == 0 && i + 1 < argc)
            gamemode = atoi(argv[++i]);
        else if (strcmp(argv[i], "--track-type") == 0 && i + 1 < argc)
            trackType = atoi(argv[++i]);
        else if (strcmp(argv[i], "--track-num") == 0 && i + 1 < argc)
            trackNum = atoi(argv[++i]);
        else if (strcmp(argv[i], "--from-rank") == 0 && i + 1 < argc)
            fromRank = atoi(argv[++i]);
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--spread") == 0 && i + 1 < argc)
            spread = atoi(argv[++i]);
        else if (strcmp(argv[i], "--slowest") == 0)
            slowest = true;
        else if (strcmp(argv[i], "--friends") == 0)
            friends = true;
        else if (strcmp(argv[i], "--refresh") == 0)
            refresh = true;
        else if (strcmp(argv[i], "--fetch") == 0)
            fetch = true;
        else if (strcmp(argv[i], "--dry-run") == 0)
            dryRun = true;
        else if (strcmp(argv[i], "--into-game") == 0)
            intoGame = true;
        else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc)
            top = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ranks") == 0 && i + 1 < argc)
            ranksSpec = argv[++i];
        else if (strcmp(argv[i], "--ranks-file") == 0 && i + 1 < argc)
            ranksFile = argv[++i];
        else if (strcmp(argv[i], "--api-record") == 0 && i + 1 < argc)
            apiRecord = argv[++i];
        else if (strcmp(argv[i], "--api-replay") == 0 && i + 1 < argc)
            apiReplay = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            Usage();
            return 0;
        }
        else
        {
            // Not "ignored": a flag this does not know is a flag the reference
            // would have acted on, and a silent difference in behaviour is
            // exactly what a parity run must not contain.
            printf("[!] not supported yet: %s\n", argv[i]);
            return 2;
        }
    }

    // The reference's own check, and its own wording.
    if (apiRecord && apiReplay)
    {
        printf("[!] pick one of --api-record and --api-replay\n");
        return 1;
    }
    if (apiRecord || apiReplay)
    {
        if (!WrTapeOpen(apiRecord ? apiRecord : apiReplay, apiRecord != NULL))
            return 1;
        WrTapeInstall();
    }

    if (!game || !*game)
    {
        Usage();
        printf("\n[!] --game is required\n");
        return 1;
    }

    // The reference checks this before it dispatches, and prints this.
    if (GetFileAttributesA(game) == INVALID_FILE_ATTRIBUTES)
    {
        printf("[!] game directory not found: %s\n", game);
        return 1;
    }

    // The reference's own dispatch order: dumps, then the map index, the board,
    // the fetcher, and extraction last.
    if (dumpBody || dumpCands || dumpChain || dumpInfo)
    {
        if (dumpInfo)
            return DumpInfo(game, map, file, limit, timeout, dumpInfo);

        // The other three take exactly one demo, and the reference says so in
        // these words rather than picking the first of several.
        if (!file)
        {
            const char *which = dumpBody ? "body" : (dumpCands ? "cands" : "chain");
            if (!map && !all)
            {
                printf("[!] nothing selected -- pass --file PATH, or --map "
                       "NAME, or --all\n");
                return 1;
            }
            printf("[!] --dump-%s takes exactly one demo; use --file.\n", which);
            return 1;
        }
        if (dumpBody)  return DumpBody(file, dumpBody);
        if (dumpCands) return DumpCands(file, dumpCands);
        return DumpChain(file, dumpChain, timeout);
    }

    if (indexMaps)
        return (WrMapsWriteIndex(game, EmitStdout) > 0) ? 0 : 1;

    if (board)
    {
        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.gameDir = game;
        a.map = map;
        a.mapId = mapId;
        a.gamemode = gamemode;
        a.trackType = trackType;
        a.trackNum = trackNum;
        a.fromRank = fromRank;
        a.count = count;
        a.spread = spread;
        a.refresh = refresh;

        // The reference's dispatch order, which is not the order the flags are
        // listed in: --friends beats --spread beats --slowest beats a window.
        if (friends)        a.mode = WR_BOARD_FRIENDS;
        else if (spread > 0) a.mode = WR_BOARD_SPREAD;
        else if (slowest)   a.mode = WR_BOARD_SLOWEST;
        else                a.mode = WR_BOARD_WINDOW;

        int rc = WrApiBoard(&a, EmitStdout, NULL, NULL);
        WrTapeClose();
        return rc;
    }

    if (fetch)
    {
        // The selection, if there is one. --ranks-file wins over --ranks, as
        // it does in the reference: it overwrites ranks_spec rather than
        // adding to it.
        int *ranks = NULL;
        int rankCount = 0;
        if (ranksFile)
        {
            char err[256] = "";
            rankCount = WrFetchParseRanksFile(ranksFile, NULL, 0, err, sizeof(err));
            if (rankCount < 0)
            {
                printf("[!] cannot read the selection file: %s\n", err);
                return 1;
            }
            ranks = (int *)malloc(sizeof(int) * (size_t)(rankCount ? rankCount : 1));
            if (ranks)
                WrFetchParseRanksFile(ranksFile, ranks, rankCount, err, sizeof(err));
        }
        else if (ranksSpec)
        {
            rankCount = WrFetchParseRanks(ranksSpec, NULL, 0);
            ranks = (int *)malloc(sizeof(int) * (size_t)(rankCount ? rankCount : 1));
            if (ranks)
                WrFetchParseRanks(ranksSpec, ranks, rankCount);
        }

        WrFetchArgs a;
        memset(&a, 0, sizeof(a));
        a.gameDir = game;
        a.map = map;
        a.mapId = mapId;
        a.gamemode = gamemode;
        a.trackType = trackType;
        a.trackNum = trackNum;
        a.ranks = ranks;
        a.rankCount = ranks ? rankCount : 0;
        a.slowest = slowest;
        a.fromRank = fromRank;
        a.count = count;
        a.top = top;
        a.dryRun = dryRun;
        a.intoGame = intoGame;

        int rc = WrFetchRun(&a, EmitStdout, NULL, NULL);
        free(ranks);
        WrTapeClose();
        return rc;
    }

    if (!map && !file && !all)
    {
        Usage();
        printf("\n[!] pick --map NAME, --file PATH, or --all\n");
        return 1;
    }

    // A fractional timeout is refused rather than truncated. The panel's slider
    // is whole seconds and the request struct carries an int; accepting 12.5 and
    // acting on 12 would be a quiet difference in behaviour from the flag as
    // handed, which is the thing this front end exists not to do.
    if (timeout != (double)(int)timeout)
    {
        printf("[!] --timeout takes whole seconds here (%g given)\n", timeout);
        return 2;
    }

    WrExtractRequest req;
    memset(&req, 0, sizeof(req));
    req.kind = WR_JOB_EXTRACT;
    strcpy_s(req.gameDir, sizeof(req.gameDir), game);
    if (map)
        strcpy_s(req.map, sizeof(req.map), map);
    if (file)
        strcpy_s(req.file, sizeof(req.file), file);
    req.skipExisting = skipExisting;
    req.retryFailed = retryFailed;
    req.verify = verify;
    req.jobs = jobs;
    req.limit = limit;
    req.timeoutSeconds = (int)timeout;

    WrExtractSetEmit(EmitStdout);
    return WrExtractRunRequest(&req);
}
