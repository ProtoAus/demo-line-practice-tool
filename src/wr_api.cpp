// wr_api.cpp  --  see wr_api.h.

#include "wr_api.h"
#include "wr_http.h"
#include "wr_json.h"
#include "wr_msml.h"
#include "wr_log.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *WrApiUserAgent(void)
{
    // Formed once. The reference has "0.4.0" frozen into it by hand and has
    // been wrong about its own version for two releases; this reads the one
    // place the version lives. A deliberate divergence, and the only one that
    // cannot show up in a comparison -- nothing the server sends back depends
    // on it.
    static char ua[128] = {0};
    if (!ua[0])
        _snprintf_s(ua, sizeof(ua), _TRUNCATE,
                    "WrLines/%s (+https://github.com/ProtoAus/"
                    "demo-line-practice-tool)", WRLINES_VERSION);
    return ua;
}

// ---------------------------------------------------------------------------
// The transport, and the pause
// ---------------------------------------------------------------------------

static WrApiTransportFn g_transport = NULL;
static void *g_transportUser = NULL;
static bool g_pace = true;

void WrApiSetTransport(WrApiTransportFn fn, void *user, bool pace)
{
    g_transport = fn;
    g_transportUser = user;
    g_pace = pace;
}

// The pause between requests, in slices, so Stop does not have to wait it out.
// False means the caller was asked to stop.
bool WrApiPace(WrApiAbortFn abort, void *user)
{
    if (!g_pace)
        return !(abort && abort(user));
    for (int slept = 0; slept < WR_API_DELAY_MS; slept += 50)
    {
        if (abort && abort(user))
            return false;
        Sleep(50);
    }
    return true;
}

bool WrApiGet(const char *url, unsigned char **out, size_t *lenOut,
              char *err, int errCap)
{
    if (g_transport)
        return g_transport(g_transportUser, url, out, lenOut, err, errCap);
    return WrHttpGet(url, WrApiUserAgent(), out, lenOut, NULL, err, errCap);
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

void WrApiLeaderboardUrl(char *out, int cap, int mapId, int gamemode,
                         int trackType, int trackNum, int take, int skip)
{
    _snprintf_s(out, (size_t)cap, _TRUNCATE,
                WR_API_BASE "/maps/%d/leaderboard?gamemode=%d&trackType=%d"
                "&trackNum=%d&take=%d&skip=%d",
                mapId, gamemode, trackType, trackNum, take, skip < 0 ? 0 : skip);
}

void WrApiFriendsUrl(char *out, int cap, int mapId, int gamemode, int trackType,
                     int trackNum, const unsigned long long *ids, int n)
{
    int used = _snprintf_s(out, (size_t)cap, _TRUNCATE,
                           WR_API_BASE "/maps/%d/leaderboard?gamemode=%d"
                           "&trackType=%d&trackNum=%d&take=%d&steamIDs=",
                           mapId, gamemode, trackType, trackNum, WR_API_PAGE);
    if (used < 0)
        return;
    for (int i = 0; i < n; i++)
    {
        int wrote = _snprintf_s(out + used, (size_t)(cap - used), _TRUNCATE,
                                "%s%llu", i ? "," : "", ids[i]);
        if (wrote < 0)
            break;              // truncated; WR_API_FRIEND_BATCH keeps it short
        used += wrote;
    }
}

void WrApiLatestReleaseUrl(char *out, int cap)
{
    _snprintf_s(out, (size_t)cap, _TRUNCATE,
                WR_GITHUB_API "/repos/" WR_GITHUB_REPO "/releases/latest");
}

// ---------------------------------------------------------------------------
// _epoch
// ---------------------------------------------------------------------------

static bool IsLeap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int DaysInMonth(int y, int m)
{
    static const int len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12)
        return 0;
    if (m == 2 && IsLeap(y))
        return 29;
    return len[m - 1];
}

// Days since 1970-01-01, for any proleptic Gregorian date. Hand-rolled rather
// than _mkgmtime64, which answers -1 for anything before 1970 and would turn a
// pre-epoch date into "unparseable" where the reference returns a negative
// number. Nothing in a leaderboard is from 1969, but "matches except where it
// does not" is not a property worth having when the alternative is six lines.
static long long DaysFromCivil(int y, int m, int d)
{
    long long yy = y - (m <= 2 ? 1 : 0);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;                             // [0, 399]
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool Digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

static int Num(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v = v * 10 + (s[i] - '0');
    return v;
}

long long WrApiEpoch(const char *iso)
{
    // Not a string, or shorter than the field: the reference tests both before
    // it tries, and answers 0.
    if (!iso || strlen(iso) < 19)
        return 0;

    // strptime("%Y-%m-%dT%H:%M:%S") against the first nineteen characters.
    // Its regex would also match one-digit months and days, but the literal
    // separators mean no such string is exactly nineteen characters long with
    // nothing left over -- and "unconverted data remains" is an error there.
    // So the only shape that parses is this one.
    if (!Digits(iso, 4) || iso[4] != '-' || !Digits(iso + 5, 2) ||
        iso[7] != '-' || !Digits(iso + 8, 2) || iso[10] != 'T' ||
        !Digits(iso + 11, 2) || iso[13] != ':' || !Digits(iso + 14, 2) ||
        iso[16] != ':' || !Digits(iso + 17, 2))
        return 0;

    int y = Num(iso, 4), mo = Num(iso + 5, 2), d = Num(iso + 8, 2);
    int h = Num(iso + 11, 2), mi = Num(iso + 14, 2), s = Num(iso + 17, 2);

    // The ranges strptime's own regex and datetime.date enforce. Seconds run to
    // 61, not 59: %S accepts a leap second and the reference does not stop it.
    // The day is checked against the MONTH, because _strptime builds a
    // datetime.date to work out the day of the year, and a 30th of February
    // raises there -- so it is 0 here.
    if (y < 1 || mo < 1 || mo > 12 || d < 1 || d > DaysInMonth(y, mo) ||
        h > 23 || mi > 59 || s > 61)
        return 0;

    return DaysFromCivil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + s;
}

// ---------------------------------------------------------------------------
// _clean
// ---------------------------------------------------------------------------

// One UTF-8 code point. Returns its byte length and writes it to *cp; an
// invalid sequence is reported as one byte, which keeps it rather than dropping
// it -- the reference decodes the response with errors="replace", so its
// version of that byte is U+FFFD, which is not whitespace either. Same
// divergence wr_json.h already describes, and it ends the same way: as one
// differing byte in a parity run, which is where it should show up.
static int Utf8At(const unsigned char *p, const unsigned char *end, unsigned *cp)
{
    unsigned char c = *p;
    if (c < 0x80)                       { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && end - p >= 2 && (p[1] & 0xC0) == 0x80)
    {
        *cp = ((unsigned)(c & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && end - p >= 3 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80)
    {
        *cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) |
              (p[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && end - p >= 4 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
    {
        *cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(p[1] & 0x3F) << 12) |
              ((unsigned)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

// What Python's str.strip() takes off the ends.
//
// str.strip() with no argument strips every character whose str.isspace() is
// true, and that is a UNICODE set, not " \t\n\r". This is that set, minus
// everything below 0x20 -- the pass above has already turned those into
// spaces, so they arrive here as U+0020 and are stripped anyway. What is left
// is the reason this function exists at all: a player called "　nova"
// writes one way in Python and another way in any implementation that assumed
// isspace() meant ASCII.
//
// U+200B ZERO WIDTH SPACE is deliberately absent. It is not whitespace to
// Python, however much it looks like it should be.
static bool IsPyStrippable(unsigned cp)
{
    return cp == 0x20 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

void WrApiClean(const char *s, char *out, int cap)
{
    if (cap <= 0)
        return;
    // isinstance(s, str) is False -- the alias was missing, null, or a number.
    if (!s)
    {
        strcpy_s(out, (size_t)cap, "?");
        return;
    }

    // Pass one, over BYTES: tab, newline, carriage return and anything below
    // 0x20 become a space. Safe to do byte-wise because no UTF-8 continuation
    // byte is below 0x80, so a byte under 0x20 is always a whole code point.
    size_t n = strlen(s);
    char *work = (char *)malloc(n + 1);
    if (!work)
    {
        strcpy_s(out, (size_t)cap, "?");
        return;
    }
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        work[i] = (c == '\t' || c == '\n' || c == '\r' || c < 32) ? ' ' : (char)c;
    }
    work[n] = '\0';

    // Pass two: .strip(), one forward walk. `keep` is the first code point that
    // survives and `keepEnd` is one past the last.
    const unsigned char *p = (const unsigned char *)work;
    const unsigned char *end = p + n;
    size_t keep = n, keepEnd = 0;
    while (p < end)
    {
        unsigned cp = 0;
        int len = Utf8At(p, end, &cp);
        if (!IsPyStrippable(cp))
        {
            size_t at = (size_t)(p - (const unsigned char *)work);
            if (keep == n)
                keep = at;
            keepEnd = at + (size_t)len;
        }
        p += len;
    }

    if (keep >= keepEnd)
    {
        // "" or "" after stripping: the reference's `or "?"`.
        strcpy_s(out, (size_t)cap, "?");
        free(work);
        return;
    }

    size_t take = keepEnd - keep;
    if (take > (size_t)cap - 1)
    {
        // The reference does not truncate, so this is a divergence when it
        // fires -- which needs an alias longer than a Steam persona name can
        // be. Cut on a code-point boundary rather than through one, and say so
        // in the log, because a silently mangled name in a file is worse than a
        // loud one.
        take = (size_t)cap - 1;
        while (take > 0 && ((unsigned char)work[keep + take] & 0xC0) == 0x80)
            take--;
        WrLogf("[!] api: an alias was longer than %d bytes and was cut", cap - 1);
    }
    memcpy(out, work + keep, take);
    out[take] = '\0';
    free(work);
}

// ---------------------------------------------------------------------------
// One page of JSON
// ---------------------------------------------------------------------------

// _to_row. False means the entry has no usable replayHash, which is the one
// thing the reference refuses an entry for.
static bool ReadEntry(WrJson *j, WrBoardCacheRow *r)
{
    memset(r, 0, sizeof(*r));
    strcpy_s(r->steamId, sizeof(r->steamId), "0");

    char aliasRaw[512] = {0};
    bool aliasIsString = false;
    bool haveHash = false;

    if (!WrJsonEnterObject(j))
    {
        WrJsonSkip(j);
        return false;
    }

    char key[48];
    while (WrJsonNextMember(j, key, sizeof(key)))
    {
        if (!strcmp(key, "replayHash"))
        {
            char h[48];
            if (WrJsonString(j, h, sizeof(h)) && h[0])
            {
                strcpy_s(r->hash, sizeof(r->hash), h);
                haveHash = true;
            }
        }
        else if (!strcmp(key, "rank"))
        {
            // int(x or 0). A JSON real reaches int() through float, which
            // truncates toward zero rather than rounding.
            WrJsonKind k = WrJsonPeek(j);
            if (k == WR_JSON_INT)
                r->rank = (int)WrJsonInt(j, 0, NULL);
            else if (k == WR_JSON_REAL)
                r->rank = (int)WrJsonReal(j, 0.0, NULL);
            else
                WrJsonSkip(j);
        }
        else if (!strcmp(key, "time"))
        {
            r->time = WrJsonReal(j, 0.0, NULL);
        }
        else if (!strcmp(key, "createdAt"))
        {
            char iso[64];
            if (WrJsonString(j, iso, sizeof(iso)))
                r->epoch = WrApiEpoch(iso);
        }
        else if (!strcmp(key, "downloadURL"))
        {
            if (!WrJsonString(j, r->url, sizeof(r->url)))
                r->url[0] = '\0';
        }
        else if (!strcmp(key, "user"))
        {
            // `r.get("user") or {}` -- a null user is an empty one, which
            // leaves the defaults above in place.
            if (WrJsonPeek(j) != WR_JSON_OBJECT || !WrJsonEnterObject(j))
            {
                WrJsonSkip(j);
                continue;
            }
            char ukey[48];
            while (WrJsonNextMember(j, ukey, sizeof(ukey)))
            {
                if (!strcmp(ukey, "steamID"))
                {
                    // str(x or "0"). Momentum sends this as a STRING, because a
                    // SteamID64 does not survive a JSON number -- which is a
                    // double, and loses the low digits above 2^53.
                    WrJsonKind k = WrJsonPeek(j);
                    if (k == WR_JSON_STRING)
                    {
                        char id[24];
                        if (WrJsonString(j, id, sizeof(id)) && id[0])
                            strcpy_s(r->steamId, sizeof(r->steamId), id);
                    }
                    else if (k == WR_JSON_INT)
                    {
                        long long v = WrJsonInt(j, 0, NULL);
                        if (v != 0)
                            _snprintf_s(r->steamId, sizeof(r->steamId), _TRUNCATE,
                                        "%lld", v);
                    }
                    else
                    {
                        WrJsonSkip(j);
                    }
                }
                else if (!strcmp(ukey, "alias"))
                {
                    aliasIsString = WrJsonString(j, aliasRaw, sizeof(aliasRaw));
                }
                else
                {
                    WrJsonSkip(j);
                }
            }
        }
        else
        {
            WrJsonSkip(j);
        }
    }

    WrApiClean(aliasIsString ? aliasRaw : NULL, r->alias, sizeof(r->alias));
    return haveHash && !WrJsonFailed(j);
}

int WrApiParsePage(const char *json, size_t len, WrBoardCacheRow *out, int maxOut,
                   int *entries, long long *totalCount, bool *haveTotal)
{
    if (entries)   *entries = 0;
    if (haveTotal) *haveTotal = false;

    WrJson j;
    WrJsonInit(&j, json, len);
    if (!WrJsonEnterObject(&j))
        return -1;

    int kept = 0, seen = 0;
    char key[48];
    while (WrJsonNextMember(&j, key, sizeof(key)))
    {
        if (!strcmp(key, "data"))
        {
            if (WrJsonPeek(&j) != WR_JSON_ARRAY || !WrJsonEnterArray(&j))
            {
                WrJsonSkip(&j);         // `page.get("data") or []`
                continue;
            }
            while (WrJsonNextElement(&j))
            {
                seen++;
                WrBoardCacheRow r;
                if (ReadEntry(&j, &r) && kept < maxOut)
                    out[kept++] = r;
            }
        }
        else if (!strcmp(key, "totalCount"))
        {
            // isinstance(tc, int). A JSON 9108.0 is a real and is NOT a total,
            // which is the reference's test and not an oversight in it.
            bool ok = false;
            long long v = WrJsonInt(&j, 0, &ok);
            if (ok)
            {
                if (totalCount) *totalCount = v;
                if (haveTotal)  *haveTotal = true;
            }
        }
        else
        {
            WrJsonSkip(&j);
        }
    }

    if (WrJsonFailed(&j))
        return -1;
    if (entries)
        *entries = seen;
    return kept;
}

// ---------------------------------------------------------------------------
// friends.txt
// ---------------------------------------------------------------------------

int WrApiReadFriends(unsigned long long *out, int maxOut, char *pathOut, int pathCap)
{
    const char *path = WrDataPath("friends.txt");
    if (pathOut)
        strncpy_s(pathOut, (size_t)pathCap, path, _TRUNCATE);

    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f)
        return 0;

    int n = 0;
    char line[128];
    while (n < maxOut && fgets(line, sizeof(line), f))
    {
        // line.strip(), then the first whitespace-delimited token.
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p || *p == '#')
            continue;
        char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n')
            e++;
        *e = '\0';

        // int(token): digits and an optional sign, nothing else. A negative one
        // parses and is then dropped by the > 0 test, exactly as it is there.
        char *stop = NULL;
        unsigned long long v = _strtoui64(p, &stop, 10);
        if (!stop || stop == p || *stop != '\0')
            continue;
        if (p[0] == '-' || v == 0)
            continue;
        out[n++] = v;
    }
    fclose(f);
    return n;
}

// ---------------------------------------------------------------------------
// _resolve_map
// ---------------------------------------------------------------------------

// Python's repr() of the map name, for the one message that prints it.
//
// A map name out of the catalogue is [a-z0-9_], but the Board tab lets you TYPE
// one, so this is a user-supplied string and repr's quoting rules are worth
// reproducing: single quotes unless the string contains one and no double
// quote, backslash and the quote character escaped, and control characters as
// \xNN. Non-ASCII is the bound -- Python decides printability from the Unicode
// tables and this passes the bytes through, so a map name in Cyrillic would
// differ. It would also not be a map.
static void ReprName(const char *s, char *out, int cap)
{
    if (!s)
    {
        strcpy_s(out, (size_t)cap, "None");
        return;
    }

    char quote = '\'';
    if (strchr(s, '\'') && !strchr(s, '"'))
        quote = '"';

    int n = 0;
    if (n < cap - 1) out[n++] = quote;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < cap - 8; p++)
    {
        unsigned char c = *p;
        if (c == (unsigned char)quote || c == '\\')
        {
            out[n++] = '\\';
            out[n++] = (char)c;
        }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (c == '\t') { out[n++] = '\\'; out[n++] = 't'; }
        else if (c < 0x20 || c == 0x7F)
            n += _snprintf_s(out + n, (size_t)(cap - n), _TRUNCATE, "\\x%02x", c);
        else
            out[n++] = (char)c;
    }
    if (n < cap - 1) out[n++] = quote;
    out[n] = '\0';
}

// The same number as MAX_MAPS in wr_maps.cpp, and for the same reason: the
// catalogue holds about 2135 maps today and WrMsmlRead says so in the log
// rather than silently stopping if it ever exceeds this.
#define WR_API_MAX_CATALOGUE 4096

// (name, id), or false having said why. The reference's _resolve_map.
static bool ResolveMap(const WrApiBoardArgs *a, WrEmitFn emit,
                       char *nameOut, int nameCap, int *idOut)
{
    const char *name = (a->map && a->map[0]) ? a->map : NULL;
    int mapId = a->mapId;

    if (!mapId)
    {
        // The catalogue is read only when it is needed. The reference reads it
        // unconditionally and then ignores it when --map-id was given; skipping
        // that is twelve megabytes of inflate saved and changes no output,
        // because nothing is printed either way.
        WrMsmlMap *cat = NULL;
        int n = 0;
        if (name)
        {
            cat = (WrMsmlMap *)malloc(sizeof(WrMsmlMap) * WR_API_MAX_CATALOGUE);
            if (cat)
                n = WrMsmlRead(a->gameDir, cat, WR_API_MAX_CATALOGUE, NULL);
        }

        int at = -1;
        for (int i = 0; i < n; i++)
            if (strcmp(cat[i].name, name) == 0)     // a dict lookup is exact
            {
                at = i;
                break;
            }

        if (at < 0)
        {
            char repr[128], msg[224];
            ReprName(name, repr, sizeof(repr));
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "[!] don't know a map id for %s.", repr);
            if (emit)
            {
                emit(msg);
                emit("    Run --index-maps first, or pass --map-id.");
            }
            free(cat);
            return false;
        }
        mapId = cat[at].id;
        free(cat);
    }

    if (name)
        strncpy_s(nameOut, (size_t)nameCap, name, _TRUNCATE);
    else
        _snprintf_s(nameOut, (size_t)nameCap, _TRUNCATE, "map%d", mapId);
    *idOut = mapId;
    return true;
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------

static bool WindowAdd(WrApiWindow *w, const WrBoardCacheRow *r)
{
    if (w->count >= w->cap)
    {
        int cap = w->cap ? w->cap * 2 : 128;
        WrBoardCacheRow *grown =
            (WrBoardCacheRow *)realloc(w->rows, sizeof(*grown) * (size_t)cap);
        if (!grown)
            return false;
        w->rows = grown;
        w->cap = cap;
    }
    w->rows[w->count++] = *r;
    return true;
}

void WrApiWindowFree(WrApiWindow *w)
{
    free(w->rows);
    memset(w, 0, sizeof(*w));
}

typedef WrApiWindow Fetch;

// One page. False having filled f->err.
static bool Leaderboard(Fetch *f, const char *url, WrBoardCacheRow *page,
                        int *entries, long long *total, bool *haveTotal)
{
    unsigned char *body = NULL;
    size_t len = 0;
    if (!WrApiGet(url, &body, &len, f->err, sizeof(f->err)))
        return false;
    f->requests++;

    int kept = WrApiParsePage((const char *)body, len, page, WR_API_PAGE,
                              entries, total, haveTotal);
    free(body);
    if (kept < 0)
    {
        // json.loads raising is caught by cmd_board's bare except, which prints
        // CPython's own message. That string is not reproducible, so this says
        // the same thing in its own words.
        _snprintf_s(f->err, sizeof(f->err), _TRUNCATE,
                    "the reply was not a leaderboard page");
        return false;
    }
    for (int i = 0; i < kept; i++)
        WindowAdd(f, &page[i]);
    return true;
}

// _fetch_window: ranks [first, first+count), 1-based, paged at WR_API_PAGE.
void WrApiFetchWindow(WrApiWindow *f, int mapId, int gamemode, int trackType,
                      int trackNum, int first, int count,
                      WrApiAbortFn abort, void *abortUser)
{
    WrBoardCacheRow page[WR_API_PAGE];
    int got = 0;
    while (got < count)
    {
        if (abort && abort(abortUser)) { f->stopped = true; return; }

        int take = count - got;
        if (take > WR_API_PAGE)
            take = WR_API_PAGE;

        char url[512];
        WrApiLeaderboardUrl(url, sizeof(url), mapId, gamemode, trackType,
                            trackNum, take, first - 1 + got);

        int entries = 0;
        long long tc = 0;
        bool haveTc = false;
        if (!Leaderboard(f, url, page, &entries, &tc, &haveTc))
            return;
        if (!f->haveTotal && haveTc)
        {
            f->total = tc;
            f->haveTotal = true;
        }
        if (entries == 0)
            break;

        // The RAW page length, not how many were kept. See WrApiParsePage.
        got += entries;
        if (f->haveTotal && first - 1 + got >= f->total)
            break;
        if (got < count && !WrApiPace(abort, abortUser))
        {
            f->stopped = true;
            return;
        }
    }
}

// _fetch_spread: N rows sampled evenly across the whole board, one request
// each. The cheap way to SEE a seventeen-thousand-run distribution -- twenty
// requests gives a fast one, a mid one and a slow one to lay over each other,
// where caching the board to do the same costs a hundred and seventy. The first
// sample is rank 1, which doubles as the probe for totalCount, so this is
// exactly N requests rather than N+1.
static void FetchSpread(Fetch *f, const WrApiBoardArgs *a, int mapId, int n,
                        WrEmitFn emit, WrApiAbortFn abort, void *abortUser)
{
    WrBoardCacheRow page[WR_API_PAGE];

    char url[512];
    WrApiLeaderboardUrl(url, sizeof(url), mapId, a->gamemode, a->trackType,
                        a->trackNum, 1, 0);
    int entries = 0;
    long long tc = 0;
    bool haveTc = false;
    if (!Leaderboard(f, url, page, &entries, &tc, &haveTc))
        return;
    // `total = tc if isinstance(tc, int) else 0` -- unconditionally set here,
    // where the window's is only set the first time it is seen.
    f->total = haveTc ? tc : 0;
    f->haveTotal = true;

    if (f->total <= 1 || n <= 1)
        return;

    if (emit)
    {
        char msg[128];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "sampling %d places across %lld runs", n, f->total);
        emit(msg);
    }

    for (int i = 1; i < n; i++)
    {
        // int(round(x)) is round-HALF-TO-EVEN in Python, which C's round() is
        // not -- round(2.5) is 2 there and 3 in <math.h>. nearbyint under the
        // default FE_TONEAREST is the one that agrees, and /fp:precise is what
        // stops the multiply and the divide being reassociated into a different
        // double.
        double frac = (double)i / (double)(n - 1);
        long long rank = 1 + (long long)nearbyint((double)(f->total - 1) * frac);
        if (rank > f->total)
            rank = f->total;

        if (!WrApiPace(abort, abortUser)) { f->stopped = true; return; }

        WrApiLeaderboardUrl(url, sizeof(url), mapId, a->gamemode, a->trackType,
                            a->trackNum, 1, (int)(rank - 1));
        if (!Leaderboard(f, url, page, &entries, &tc, &haveTc))
            return;
    }
}

static void FetchFriends(Fetch *f, const WrApiBoardArgs *a, int mapId,
                         const unsigned long long *ids, int count,
                         WrApiAbortFn abort, void *abortUser)
{
    WrBoardCacheRow page[WR_API_PAGE];
    for (int i = 0; i < count; i += WR_API_FRIEND_BATCH)
    {
        if (abort && abort(abortUser)) { f->stopped = true; return; }

        int chunk = count - i;
        if (chunk > WR_API_FRIEND_BATCH)
            chunk = WR_API_FRIEND_BATCH;

        char url[4096];
        WrApiFriendsUrl(url, sizeof(url), mapId, a->gamemode, a->trackType,
                        a->trackNum, ids + i, chunk);

        int entries = 0;
        long long tc = 0;
        bool haveTc = false;
        if (!Leaderboard(f, url, page, &entries, &tc, &haveTc))
            return;

        if (i + WR_API_FRIEND_BATCH < count && !WrApiPace(abort, abortUser))
        {
            f->stopped = true;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// cmd_board
// ---------------------------------------------------------------------------

#define WR_API_MAX_FRIENDS 4096

static void Emitf(WrEmitFn emit, const char *fmt, ...)
{
    if (!emit)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    emit(buf);
}

int WrApiBoard(const WrApiBoardArgs *a, WrEmitFn emit,
               WrApiAbortFn abort, void *abortUser)
{
    char name[72];
    int mapId = 0;
    if (!ResolveMap(a, emit, name, sizeof(name), &mapId))
        return 1;

    char path[MAX_PATH];
    WrBoardCachePath(path, sizeof(path), name, a->gamemode, a->trackType,
                     a->trackNum);

    WrBoardCache held;
    memset(&held, 0, sizeof(held));
    if (!a->refresh)
        WrBoardReadCache(path, &held, 0);

    // GAMEMODES.get(n, "mode %d" % n), which is NOT what WrGamemodeName answers
    // for an unknown mode -- that one says "?", which is right for a table
    // column and wrong for a line the reference also prints.
    char modeName[24];
    if (a->gamemode >= 1 && a->gamemode <= WR_GAMEMODE_COUNT)
        strcpy_s(modeName, sizeof(modeName), WrGamemodeName(a->gamemode));
    else
        _snprintf_s(modeName, sizeof(modeName), _TRUNCATE, "mode %d", a->gamemode);

    Emitf(emit, "map %s (id %d), %s, track %d/%d", name, mapId,
          modeName, a->trackType, a->trackNum);
    if (held.count)
        Emitf(emit, "%d rows already cached; this adds to them", held.count);

    int count = a->count > 0 ? a->count : WR_API_MAX_DEFAULT;

    Fetch f;
    memset(&f, 0, sizeof(f));

    if (a->mode == WR_BOARD_FRIENDS)
    {
        unsigned long long *ids = (unsigned long long *)
            malloc(sizeof(unsigned long long) * WR_API_MAX_FRIENDS);
        char fpath[MAX_PATH] = "";
        int n = ids ? WrApiReadFriends(ids, WR_API_MAX_FRIENDS, fpath,
                                       sizeof(fpath)) : 0;
        if (n <= 0)
        {
            free(ids);
            WrBoardCacheFree(&held);
            Emitf(emit, "[!] no friends list at %s.", fpath);
            if (emit)
            {
                emit("    Press \"Refresh my friends\" in the Board tab -- only");
                emit("    the injected DLL can read your Steam friends, so it");
                emit("    has to write the list for this script to use.");
            }
            return 1;
        }
        int reqs = (n + WR_API_FRIEND_BATCH - 1) / WR_API_FRIEND_BATCH;
        Emitf(emit, "%d friend%s to look up, %d request%s",
              n, n == 1 ? "" : "s", reqs, n <= WR_API_FRIEND_BATCH ? "" : "s");
        FetchFriends(&f, a, mapId, ids, n, abort, abortUser);
        free(ids);
        if (!f.err[0])
            Emitf(emit, "%d of them have a run on this track", f.count);
        // A friends lookup learns nothing about the board's size -- the
        // reference leaves `total` as None here -- so f.haveTotal stays false
        // and whatever the file already knew survives the write.
    }
    else if (a->mode == WR_BOARD_SPREAD && a->spread > 0)
    {
        FetchSpread(&f, a, mapId, a->spread, emit, abort, abortUser);
    }
    else if (a->mode == WR_BOARD_SLOWEST)
    {
        // One probe for the size, then land on the tail. Two requests for the
        // hundred slowest runs of a nine-thousand-run board.
        WrBoardCacheRow probe[WR_API_PAGE];
        char url[512];
        WrApiLeaderboardUrl(url, sizeof(url), mapId, a->gamemode, a->trackType,
                            a->trackNum, 1, 0);
        int entries = 0;
        long long tc = 0;
        bool haveTc = false;

        // The probe's own rows are DISCARDED by the reference -- it unpacks
        // `_, tc` -- so rank 1 does not end up in the cache unless the window
        // below happens to include it. Reproduced by parsing into a scratch
        // buffer that is thrown away rather than into f.rows.
        unsigned char *body = NULL;
        size_t len = 0;
        if (!WrApiGet(url, &body, &len, f.err, sizeof(f.err)))
        {
            Emitf(emit, "[!] leaderboard request failed: %s", f.err);
            WrBoardCacheFree(&held);
            WrApiWindowFree(&f);
            return 1;
        }
        f.requests++;
        int kept = WrApiParsePage((const char *)body, len, probe, WR_API_PAGE,
                                  &entries, &tc, &haveTc);
        free(body);
        if (kept < 0)
        {
            Emitf(emit, "[!] leaderboard request failed: the reply was not a "
                        "leaderboard page");
            WrBoardCacheFree(&held);
            WrApiWindowFree(&f);
            return 1;
        }

        long long total = haveTc ? tc : 0;
        if (total <= 0)
        {
            Emitf(emit, "no runs on this track.");
            WrBoardCacheFree(&held);
            WrApiWindowFree(&f);
            return 0;
        }
        long long first = total - count + 1;
        if (first < 1)
            first = 1;
        Emitf(emit, "the board holds %lld runs; taking ranks %lld-%lld",
              total, first, total);

        if (!WrApiPace(abort, abortUser))
        {
            f.stopped = true;
            f.total = total;
            f.haveTotal = true;
        }
        else
        {
            // Deliberately NOT seeded with the probe's total before the window
            // runs. `total = t2 if isinstance(t2, int) else total` means the
            // window's own answer wins and the probe's is only the fallback,
            // and seeding would quietly invert that -- WrApiFetchWindow keeps
            // first total it sees and would then keep this one for ever.
            WrApiFetchWindow(&f, mapId, a->gamemode, a->trackType, a->trackNum,
                             (int)first, count, abort, abortUser);
            if (!f.haveTotal)
            {
                f.total = total;
                f.haveTotal = true;
            }
        }
    }
    else
    {
        int first = a->fromRank > 0 ? a->fromRank : 1;
        int reqs = (count + WR_API_PAGE - 1) / WR_API_PAGE;
        Emitf(emit, "taking ranks %d-%d, which is %d request%s",
              first, first + count - 1, reqs, count <= WR_API_PAGE ? "" : "s");
        WrApiFetchWindow(&f, mapId, a->gamemode, a->trackType, a->trackNum,
                         first, count, abort, abortUser);
    }

    if (f.err[0])
    {
        // urllib's HTTPError arm prints "HTTP %d", and WrHttpGet's message for
        // a non-2xx reply is exactly that, so both arms of the reference's
        // try/except come out of this one line.
        Emitf(emit, "[!] leaderboard request failed: %s", f.err);
        WrBoardCacheFree(&held);
        WrApiWindowFree(&f);
        return 1;
    }

    // Stop pressed before a single row arrived. Checked before the line below,
    // which would otherwise tell somebody who had just cancelled that their map
    // has no runs on it.
    if (f.stopped && f.count == 0)
    {
        Emitf(emit, "stopped after %d request%s; nothing had arrived yet",
              f.requests, f.requests == 1 ? "" : "s");
        WrBoardCacheFree(&held);
        WrApiWindowFree(&f);
        return 1;
    }

    if (f.count == 0 && held.count == 0)
    {
        if (emit)
            emit("no runs on this track. If the map has stages or bonuses, the "
                 "main track can be empty while the stages are not -- try "
                 "--track-type 1 --track-num 1, and check the gamemode.");
        WrBoardCacheFree(&held);
        WrApiWindowFree(&f);
        return 0;
    }

    // Deduped on the replay hash, NOT on rank. Ranks move as runs land, so the
    // same run cached twice a week apart would otherwise sit in the file twice
    // under two different numbers. The newer row wins, which also refreshes the
    // rank of anything re-fetched -- and keeps its position, which is what
    // makes the written order reproducible.
    int fresh = 0;
    for (int i = 0; i < f.count; i++)
        if (WrBoardCacheMerge(&held, &f.rows[i]))
            fresh++;

    _snprintf_s(held.map, sizeof(held.map), _TRUNCATE, "%s", name);
    _snprintf_s(held.mapId, sizeof(held.mapId), _TRUNCATE, "%d", mapId);
    _snprintf_s(held.gamemode, sizeof(held.gamemode), _TRUNCATE, "%d", a->gamemode);
    _snprintf_s(held.track, sizeof(held.track), _TRUNCATE, "%d\t%d",
                a->trackType, a->trackNum);
    if (f.haveTotal && f.total > 0)
        _snprintf_s(held.total, sizeof(held.total), _TRUNCATE, "%lld", f.total);
    _snprintf_s(held.fetched, sizeof(held.fetched), _TRUNCATE, "%lld", WrNowEpoch());

    bool wrote = WrBoardWriteCache(path, &held);

    // meta.get("total", ["?"])[0] -- the FIRST field of that line, where the
    // line itself keeps every field it had. They only differ on a hand-edited
    // file, and then the reference shows the first one too.
    char totalShown[32];
    strcpy_s(totalShown, sizeof(totalShown), held.total[0] ? held.total : "?");
    {
        char *tab = strchr(totalShown, '\t');
        if (tab)
            *tab = '\0';
    }

    Emitf(emit, "%d request%s, %d rows returned, %d new; %d of %s now cached",
          f.requests, f.requests == 1 ? "" : "s", f.count, fresh, held.count,
          totalShown);
    Emitf(emit, "-> %s", path);

    // Only reachable from the panel, and only after Stop. See the comment on
    // WrApiAbortFn: the rows already fetched are kept rather than thrown away,
    // which the reference could not do because it was being killed.
    if (f.stopped)
        Emitf(emit, "stopped after %d request%s; what had arrived was kept",
              f.requests, f.requests == 1 ? "" : "s");

    WrBoardCacheFree(&held);
    WrApiWindowFree(&f);
    return wrote ? 0 : 1;
}
