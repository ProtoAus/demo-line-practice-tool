// test_seam.cpp  --  the typed request still produces the old command line.
//
// TEMPORARY BY DESIGN. It goes when the python backend goes.
//
// WHY IT EXISTS
//
// Replacing WrExtractRunArgs(const char *args, bool needsMap) with a struct was
// a pure refactor: nine call sites stopped formatting fragments of argv with
// _snprintf_s and started filling in fields, and one function in wr_extract.cpp
// now turns the struct back into exactly the argv that used to be built by hand.
//
// A pure refactor has no observable effect, which means it also has no natural
// test -- you find out you broke it when a leaderboard fetch silently asks for
// the wrong track, or a browse downloads instead of listing because --dry-run
// went missing. Both of those are one absent word in a string nobody reads.
//
// So the strings are written out here, by hand, copied from the call sites as
// they were before the change, and the harness holds the new code to them.
// It is the only gate a refactor of this shape can have.
//
// The numbers are the ones on the developer's machine, from the tour
// screenshots: surf_tropic's board at gamemode 1 main track, surf_utopia's 273
// cached demos, the Maps tab's default of 25 leaderboard places.
//
// Build:  tests\build.bat
// Run:    tests\test_seam.exe

#include "wr_extract.h"
#include "wr_log.h"

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

// Build the argv for one request and compare it to the string the old call site
// produced. Prints both on a mismatch, because "they differ" is useless and the
// difference is always one word.
static void Same(const WrExtractRequest *req, bool wantMap, const char *want,
                 const char *what)
{
    char got[1024];
    bool needsMap = false;
    bool ok = WrExtractTestPythonArgs(req, got, sizeof(got), &needsMap);

    if (!ok)
    {
        printf("  %-58s %s\n", what, "FAILED");
        printf("     the request could not be expressed at all\n");
        g_failures++;
        return;
    }
    if (needsMap != wantMap)
    {
        printf("  %-58s %s\n", what, "FAILED");
        printf("     needsMap %d, wanted %d\n", (int)needsMap, (int)wantMap);
        g_failures++;
        return;
    }
    if (strcmp(got, want) != 0)
    {
        printf("  %-58s %s\n", what, "FAILED");
        printf("     want: %s\n", want);
        printf("     got : %s\n", got);
        g_failures++;
        return;
    }
    printf("  %-58s %s\n", what, "ok");
}

int main(void)
{
    printf("\n=== wrlines: the request and the command line it replaced ===\n");

    // -----------------------------------------------------------------------
    printf("\nextraction, which is the only verb about the map you are in\n");
    {
        WrExtractRequest req = {WR_JOB_EXTRACT};
        req.timeoutSeconds = 30;
        Same(&req, true, "--skip-existing --timeout 30",
             "the ordinary press");

        req.retryFailed = true;
        Same(&req, true, "--skip-existing --retry-failed --timeout 30",
             "retrying the recorded failures");

        req.retryFailed = false;
        req.timeoutSeconds = 0;
        Same(&req, true, "--skip-existing --timeout 0",
             "no limit is 0, not an absent flag");
    }

    // -----------------------------------------------------------------------
    printf("\nthe map index, which takes nothing at all\n");
    {
        WrExtractRequest req = {WR_JOB_INDEX_MAPS};
        Same(&req, false, "--index-maps", "one flag, and no --map");
    }

    // -----------------------------------------------------------------------
    printf("\nthe board, whose five buttons were five format strings\n");
    {
        WrExtractRequest base = {WR_JOB_BOARD};
        strcpy_s(base.map, sizeof(base.map), "surf_tropic");
        base.gamemode = 1;
        base.trackType = 0;
        base.trackNum = 1;

        const char *tail = " --map \"surf_tropic\" --gamemode 1 "
                           "--track-type 0 --track-num 1";
        char want[512];

        WrExtractRequest req = base;
        req.boardMode = WR_BOARD_WINDOW;
        req.fromRank = 1;
        req.count = 50;
        _snprintf_s(want, sizeof(want), _TRUNCATE,
                    "--board --from-rank 1 --count 50%s", tail);
        Same(&req, false, want, "Fastest 50");

        req = base;
        req.boardMode = WR_BOARD_SLOWEST;
        req.count = 50;
        _snprintf_s(want, sizeof(want), _TRUNCATE,
                    "--board --slowest --count 50%s", tail);
        Same(&req, false, want, "Slowest 50");

        // The same mode as Fastest, which is the point of there being one:
        // "Fastest N" is this with the rank pinned to 1.
        req = base;
        req.boardMode = WR_BOARD_WINDOW;
        req.fromRank = 370;
        req.count = 50;
        _snprintf_s(want, sizeof(want), _TRUNCATE,
                    "--board --from-rank 370 --count 50%s", tail);
        Same(&req, false, want, "From 370");

        req = base;
        req.boardMode = WR_BOARD_SPREAD;
        req.spread = 20;
        _snprintf_s(want, sizeof(want), _TRUNCATE, "--board --spread 20%s", tail);
        Same(&req, false, want, "Spread 20");

        req = base;
        req.boardMode = WR_BOARD_FRIENDS;
        _snprintf_s(want, sizeof(want), _TRUNCATE, "--board --friends%s", tail);
        Same(&req, false, want, "My friends' runs");
    }

    // -----------------------------------------------------------------------
    printf("\nthe Maps tab fetch: an id and a count, and no gamemode\n");
    {
        WrExtractRequest req = {WR_JOB_FETCH};
        strcpy_s(req.map, sizeof(req.map), "surf_utopia");
        req.mapId = 552;
        req.top = 25;
        req.trackType = 0;
        req.trackNum = 1;

        Same(&req, false,
             "--fetch --map \"surf_utopia\" --map-id 552 --top 25 "
             "--track-type 0 --track-num 1",
             "download");

        req.dryRun = true;
        Same(&req, false,
             "--fetch --map \"surf_utopia\" --map-id 552 --top 25 "
             "--track-type 0 --track-num 1 --dry-run",
             "browse is download plus --dry-run, in that order");

        // --into-game comes BEFORE --dry-run, because the base string was built
        // first and the browse button appended to it.
        req.intoGame = true;
        Same(&req, false,
             "--fetch --map \"surf_utopia\" --map-id 552 --top 25 "
             "--track-type 0 --track-num 1 --into-game --dry-run",
             "and --into-game comes before it");

        req.dryRun = false;
        Same(&req, false,
             "--fetch --map \"surf_utopia\" --map-id 552 --top 25 "
             "--track-type 0 --track-num 1 --into-game",
             "download, into the game's own replay folder");
    }

    // -----------------------------------------------------------------------
    printf("\nthe Board tab fetch: named places, and a gamemode\n");
    {
        // Where the selection has to land on disk for as long as the backend is
        // a separate process. Built the same way the writer builds it, because
        // the thing under test is the shape of the argv, not the path helper.
        char pick[MAX_PATH];
        strcpy_s(pick, sizeof(pick),
                 WrDataPath("boards\\surf_tropic_g1_t01.pick"));

        int ranks[7] = {48, 49, 50, 370, 371, 372, 373};

        WrExtractRequest req = {WR_JOB_FETCH};
        strcpy_s(req.map, sizeof(req.map), "surf_tropic");
        req.gamemode = 1;
        req.trackType = 0;
        req.trackNum = 1;
        req.ranks = ranks;
        req.rankCount = 7;

        char want[1024];
        _snprintf_s(want, sizeof(want), _TRUNCATE,
                    "--fetch --map \"surf_tropic\" --gamemode 1 --track-type 0 "
                    "--track-num 1 --ranks-file \"%s\"", pick);
        Same(&req, false, want, "Download 7 ticked");

        req.intoGame = true;
        _snprintf_s(want, sizeof(want), _TRUNCATE,
                    "--fetch --map \"surf_tropic\" --gamemode 1 --track-type 0 "
                    "--track-num 1 --ranks-file \"%s\" --into-game", pick);
        Same(&req, false, want, "... and into the game folder");

        // The file the flag points at. A selection that arrives empty or in the
        // wrong order downloads the wrong runs and says nothing about it.
        {
            FILE *f = NULL;
            char line[256];
            int got[16], n = 0;
            if (fopen_s(&f, pick, "r") == 0 && f)
            {
                while (n < 16 && fgets(line, sizeof(line), f))
                {
                    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                        continue;
                    got[n++] = atoi(line);
                }
                fclose(f);
            }
            Check(n == 7, "the pick file holds every ticked place");
            bool order = (n == 7);
            for (int i = 0; i < n && i < 7; i++)
                if (got[i] != ranks[i])
                    order = false;
            Check(order, "in the order they were ticked");
            printf("     %s\n", pick);
            remove(pick);
        }
    }

    // -----------------------------------------------------------------------
    printf("\na request with no job in it asks for nothing\n");
    {
        WrExtractRequest req = {WR_JOB_NONE};
        char got[64];
        bool needsMap = true;
        Check(!WrExtractTestPythonArgs(&req, got, sizeof(got), &needsMap),
              "WR_JOB_NONE is refused rather than run as something");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
