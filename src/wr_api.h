// wr_api.h  --  Momentum's public leaderboard, and the manners we keep with it.
//
// One host, one verb, and a set of rules nobody imposed on us.
//
// Momentum's backend is open source and GET /maps/:id/leaderboard carries
// @BypassJwtAuth, so this needs no account and no token: it is the same data
// the website shows anyone. Their terms say nothing about automated access in
// either direction, which makes everything below a question of manners rather
// than of permission. The rules, and they are self-imposed:
//
//   - one request at a time, never concurrent
//   - WR_API_DELAY_MS between them, always, except when replaying a recording
//   - a cap per invocation, so a button press cannot become a thousand requests
//   - a User-Agent that says what this is and links to where the source is
//   - never automatic. Every request here is downstream of a button.
//
// The cap is the one worth explaining. A board is paged at a hundred entries --
// take=200 is a 400 Bad Request -- so surf_boreas at 16993 runs is 170
// requests, a minute of sustained traffic against infrastructure somebody else
// pays for, for a table nobody scrolls. So you ask for a WINDOW and the cache
// accumulates: the top hundred, then the slowest hundred, then ranks 4000-4020,
// and you end up having browsed as much of the board as you actually looked at.
//
// WHY steamIDs= AND NOT filter=friends
//
// Momentum's own leaderboard has a friends filter and it answers 401 without a
// session, which is why the site cannot offer it to a logged-out visitor and
// why this cannot use it. Asking for specific SteamID64s is not gated at all,
// and it comes back better: each player's run arrives with its true GLOBAL
// rank, so a friend sitting at 4500 is found without caching the 4499 runs
// above them. One request per hundred friends, and friends with no run on the
// map are silently absent rather than an error.
//
// The list of ids comes from wrlines_data\friends.txt, which only the DLL can
// write -- it is injected into the game, so it has a live ISteamFriends. That
// file is the fence pointing the other way from maps.txt, and it stays a
// legible artefact on disk rather than becoming an in-memory hand-off, because
// the panel documents it to users and it is the thing you would want to look at
// before pressing a button that sends a hundred SteamIDs anywhere.
//
// WHY _clean EXISTS
//
// An alias is free text chosen by a stranger and it has to survive being one
// field of a tab-separated file. A single tab in a single name would shift
// every column after it on that row, and the reader would take the back half of
// somebody's name as a replay hash -- which then reads as "you already have
// this run", or downloads nothing, and is invisible in a table. Tabs, newlines
// and control characters all become spaces for that reason, and a name that is
// nothing but those becomes "?" rather than an empty column.

#ifndef WR_API_H
#define WR_API_H

#include "wr_common.h"
#include "wr_board.h"
#include "wr_extract.h"         // WrBoardFetchMode and WrEmitFn, which the job
                                // slot owns and this layer is driven by

#define WR_API_BASE "https://api.momentum-mod.org/v1"

// The API's own maximum page. Not a preference: take=101 is a 400.
#define WR_API_PAGE 100

// Places taken when --count is not given. Deliberately small.
#define WR_API_MAX_DEFAULT 50

// The pause between requests, in milliseconds. 0.4s in the reference.
#define WR_API_DELAY_MS 400

// SteamID64s per friends request. 200 in a 3704-character URL is accepted and
// answered correctly -- verified against the live API -- and 100 leaves
// generous headroom under any proxy's URL limit while still being one request
// for almost everybody's friends list.
#define WR_API_FRIEND_BATCH 100

// What this sends as its User-Agent, and the only thing it sends that it did
// not have to. Carries the version so a server operator seeing odd traffic can
// tell which build made it.
const char *WrApiUserAgent(void);

// ---------------------------------------------------------------------------
// The recorded-conversation seam
// ---------------------------------------------------------------------------
//
// A leaderboard is not a fixture. It changes under you -- ranks move as runs
// land -- so a board fetched twice an hour apart produces two different files
// that are both correct, which makes "did the port write the same bytes"
// unanswerable against a live server. It also makes this path untestable in CI
// at all, because a build machine should not be calling somebody's API.
//
// So: record once, replay for ever. This one function pointer is the whole
// mechanism on this side. It is NULL in the shipped DLL and there is no way to
// set it from anywhere the DLL links -- the record and replay implementations
// live in tests\api_tape.cpp, which nothing shipped includes. What ships is a
// null check.
//
// `pace` is false for a replay, because the four hundred milliseconds are owed
// to a server and not to a file on disk. It stays true for a recording, which
// is a real conversation.
typedef bool (*WrApiTransportFn)(void *user, const char *url,
                                 unsigned char **out, size_t *lenOut,
                                 char *err, int errCap);
void WrApiSetTransport(WrApiTransportFn fn, void *user, bool pace);

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------
//
// Polled before every request and inside the pause between them, so Stop lands
// within about a twentieth of a second rather than after the current page.
// Returning true means give up.
//
// What happens then is a deliberate improvement on the reference, which could
// not do it: the rows already fetched are MERGED AND WRITTEN rather than
// discarded. The reference is killed by TerminateJobObject and never reaches
// its writer, so pressing Stop after ninety pages threw away ninety pages. The
// cache is an accumulating artefact; there is nothing to be gained by making a
// partial fetch worth nothing. It cannot affect a parity run, which never
// stops.
typedef bool (*WrApiAbortFn)(void *user);

// ---------------------------------------------------------------------------
// --board
// ---------------------------------------------------------------------------

struct WrApiBoardArgs
{
    const char *gameDir;        // for the map catalogue, when mapId is 0
    const char *map;            // may be NULL or "" if mapId is given
    int mapId;                  // 0 means resolve the name in the catalogue
    int gamemode, trackType, trackNum;

    WrBoardFetchMode mode;
    int fromRank;               // WINDOW; 0 means 1
    int count;                  // WINDOW and SLOWEST; 0 means WR_API_MAX_DEFAULT
    int spread;                 // SPREAD
    bool refresh;               // start from nothing rather than from the cache
};

// Cache a window of a leaderboard. Returns the process exit code the reference
// would have returned -- 0 for success, 1 for a failure it has already printed
// -- because the panel shows that number and a user comparing two runs should
// not be able to tell which implementation produced it.
int WrApiBoard(const WrApiBoardArgs *a, WrEmitFn emit,
               WrApiAbortFn abort, void *abortUser);

// ---------------------------------------------------------------------------
// The pieces, exposed for tests\test_api.exe
// ---------------------------------------------------------------------------
//
// These are the parts with an exact answer that can be stated in a table, which
// is most of the risk in this file: a URL that differs by one character asks a
// different question, and a timestamp that differs by an hour is a silently
// wrong date in every row.

void WrApiLeaderboardUrl(char *out, int cap, int mapId, int gamemode,
                         int trackType, int trackNum, int take, int skip);
void WrApiFriendsUrl(char *out, int cap, int mapId, int gamemode, int trackType,
                     int trackNum, const unsigned long long *ids, int n);

// "2026-03-16T09:40:41.194Z" -> unix seconds. 0 if it will not parse.
//
// The reference is time.strptime + calendar.timegm, which validates: a month of
// 13, a 31st of February and a 25th hour are all errors there and 0 here.
long long WrApiEpoch(const char *iso);

// An alias, made safe to be a field. `s` NULL means the value was not a string
// at all, which the reference answers with "?" rather than with an empty field.
void WrApiClean(const char *s, char *out, int cap);

// One leaderboard page -> rows. Returns how many were KEPT, or -1 if the JSON
// will not parse. `totalCount` is only written when the page carried one as an
// integer, which is the isinstance check the reference makes.
//
// `entries` receives how many elements were in "data" at all, which is not the
// same number and is the one the pager advances by: the reference does
// `got += len(rows)` on the raw page, so a page of a hundred entries of which
// two lacked a replayHash still counts as a hundred places consumed. Using the
// kept count there would ask for the same two places for ever.
int WrApiParsePage(const char *json, size_t len, WrBoardCacheRow *out, int maxOut,
                   int *entries, long long *totalCount, bool *haveTotal);

// The SteamID64s in wrlines_data\friends.txt. Returns how many, and writes the
// path it looked at so the caller can name it when there are none.
int WrApiReadFriends(unsigned long long *out, int maxOut, char *pathOut, int pathCap);

#endif // WR_API_H
