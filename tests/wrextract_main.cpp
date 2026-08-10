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
// Its flags are a subset of wrpath_extract.py's, and the same spelling, so the
// parity driver can hand the same argv to both. The subset grows one verb per
// phase; anything not yet ported says so and exits non-zero rather than
// pretending. What is here now:
//
//     --index-maps                 the map index          (P1)
//     --file PATH --dump-body OUT  one demo's run body    (P2)
//     --board                      a leaderboard window   (P3)
//     --api-record / --api-replay  a recorded conversation
//
// --dump-body is the most valuable of the four dumps and the reason the others
// can wait. Everything above it in the extractor is arithmetic on floats, where
// a difference has to be argued about; the body is bytes, so a difference is a
// difference. Run it over a whole library and the container and the decoder are
// either right or they are not.
//
// --board is the same idea one level up. A leaderboard changes under you, so it
// is not an oracle by itself; --api-replay makes it one, by answering both
// implementations from the same recorded bytes. See tests\api_tape.h.
//
// There is no --map and no --all yet, because demo discovery is a piece of the
// reference this phase has not ported -- iter_demos walks two named subtrees
// and sorts, and getting that subtly wrong would quietly change which demos a
// parity run covered. tests\parity.ps1 does the enumerating instead and hands
// both sides one --file at a time, which is a comparison with nothing hidden
// in it.
//
// There is no --out either, deliberately. The reference derives the boards
// directory from it; this one uses WrDataPath, which for an .exe under tests\
// lands in tests\wrlines_data\ -- covered by .gitignore at any depth, so
// nothing this writes escapes into the working tree. Accepting --out and
// ignoring it would be the one thing this file must never do: quietly behave
// differently from the flag it was handed. parity.ps1 gives --out to the
// reference only, and compares the two files wherever each of them landed.
//
// Build:  tests\build.bat
// Run:    tests\wrextract.exe --game "<install>" --index-maps

#include "wr_api.h"
#include "wr_maps.h"
#include "wr_msml.h"
#include "wr_mtv.h"
#include "wr_log.h"
#include "api_tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
           "  --index-maps       rebuild the map index from the game's cache\n"
           "  --file PATH        the demo to work on\n"
           "  --dump-body PATH   write that demo's decompressed run body\n"
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
           "  --api-record DIR   save every leaderboard reply under DIR\n"
           "  --api-replay DIR   answer every request from DIR, never the net\n"
           "\n"
           "Not shipped. Flags mirror wrpath_extract.py so the two can be\n"
           "run against each other; verbs not yet ported are refused rather\n"
           "than quietly skipped.\n");
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

int main(int argc, char **argv)
{
    const char *game = NULL;
    const char *file = NULL;
    const char *dumpBody = NULL;
    const char *apiRecord = NULL;
    const char *apiReplay = NULL;
    bool indexMaps = false;

    bool board = false, slowest = false, friends = false, refresh = false;
    const char *map = NULL;
    int mapId = 0, gamemode = 1, trackType = 0, trackNum = 1;
    int fromRank = 0, count = 0, spread = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--game") == 0 && i + 1 < argc)
            game = argv[++i];
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            file = argv[++i];
        else if (strcmp(argv[i], "--dump-body") == 0 && i + 1 < argc)
            dumpBody = argv[++i];
        else if (strcmp(argv[i], "--index-maps") == 0)
            indexMaps = true;
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

    if (dumpBody)
    {
        if (!file)
        {
            printf("[!] --dump-body needs --file: this build has no demo "
                   "discovery yet\n");
            return 2;
        }
        return DumpBody(file, dumpBody);
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

    Usage();
    printf("\n[!] pick a verb\n");
    return 1;
}
