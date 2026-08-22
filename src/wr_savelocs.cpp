// wr_savelocs.cpp  --  see wr_savelocs.h.

#include "wr_savelocs.h"
#include "wr_energy.h"      // the banked figures stamped onto a new save-loc
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAVELOCS 256

// How close a position has to be to count as "the same save-loc". Momentum
// restores the exact stored origin, so this only has to absorb the difference
// between the player origin it stores and the camera we see -- which is why the
// test below is horizontal, with a generous vertical allowance.
#define MATCH_RADIUS 24.0f
#define MATCH_VERTICAL 96.0f

// And how close is close enough to mean "the game just PUT you here".
//
// Momentum restores the exact stored origin and the view sits directly above
// it, so a frame after a load the camera's x and y are the save-loc's x and y
// to a fraction of a unit. A unit of slack covers the camera interpolating and
// the tenth-of-a-unit rounding in our own sidecar key, and nothing else: at
// surf speed the camera crosses fifty units in a frame, so landing inside a
// one-unit circle by moving is not something that happens by accident.
//
// HORIZONTAL, like the test above, and for a better reason than symmetry. The
// obvious sharper version is cam.z == pos.z + eyeHeight, and it is wrong twice
// over: eyeHeight is a SETTING with a slider, fixed at 64 because that is the
// standing view offset, and a save-loc made or loaded while ducked has a
// different one. Two coordinates agreeing to a fraction of a unit already say
// everything a third would, and ducking cannot defeat them.
#define MATCH_EXACT_RADIUS 1.0f

// A velocity out of the game's file past this is not believed. The same number
// wr_energy.cpp calls MAX_SANE_SPEED, kept separate rather than shared because
// the two are guarding different things: that one rejects a camera difference
// this one rejects a corrupt field, and neither should quietly change when the
// other is tuned. The fastest of the 3239 records here is 6058 u/s.
#define WR_SAVELOC_MAX_SPEED 10000.0f

// Sidecar format version. 1 had no header line and keyed on position alone; its
// times were also written by the proximity bug, so they load as `suspect`.
// 3 appends the energy the readout had banked when the loc was made, and marks
// times that were typed in by hand. A v2 file still loads: its rows simply have
// no energy and were not typed, which is exactly what they were.
#define SIDECAR_VERSION 3
#define SIDECAR_TAG "wrlines-savelocs"

// Row flags in a v3 sidecar. A bitfield rather than more columns, so a later
// version can add one without moving anything already written.
#define SIDE_BY_HAND     1      // the time was typed in, not stamped
#define SIDE_HAS_ENERGY  2      // gained/lost/peak on this row mean something

struct Saveloc
{
    Vec3 pos;
    Vec3 vel;           // the game's own record of how fast you were going
    bool haveVel;       // false when it was absent, or not a sane number
    bool fromCps;       // a real save-loc, not an entry from "startmarks"
    float ourTime;      // seconds, or -1 when we have never timed it
    int ordinal;        // among entries sharing this position, in file order
    int gameIndex;      // index in Momentum's cps list; -1 for a startmark
    bool suspect;       // time came from a v1 sidecar
    bool byHand;        // time was typed in rather than stamped on creation

    // What the energy readout had banked at the moment this loc was made. Only
    // ever written by us, so it is absent for every loc made before v0.4.1 --
    // which is the normal case, and reads as "reset the accumulators", exactly
    // what happened before this existed.
    bool haveEnergy;
    float gained, lost, peak;
};

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

static char g_map[72] = {0};
static Saveloc g_locs[MAX_SAVELOCS];
static int g_count = 0;
static int g_timed = 0;
static int g_current = -1;       // Momentum's exact current `cur` cps index
static volatile LONG g_loadSerial = 0;
static char g_status[160] = "not looked yet";

static long long g_mtime = 0;
static volatile LONG g_busy = 0;
static HANDLE g_thread = NULL;

// Has a read for THIS map finished at least once?
//
// This used to be inferred as "g_count == 0 && g_timed == 0", meaning "we hold
// nothing, so this must be the first look". On a map that has no save-locs yet
// that is true after the first read, and after the second, and for ever -- so
// the first save-loc you ever make on a map was never stamped, which is exactly
// the one you make while working out whether the feature works at all.
static bool g_readOnce = false;

// A change to the game's file that arrived while a read was already running.
//
// The mtime used to be committed before the busy guard below, so a change that
// found the reader busy was recorded as seen and then dropped: the file will
// not change again by itself, so nothing ever re-read it and that save-loc was
// never stamped. Two save-locs made in quick succession is all it takes.
static bool g_rereadPending = false;

// Bumped every time the table is replaced. Read by wr_timer.cpp, which has to
// tell "the camera arrived on a save-loc" apart from "a save-loc appeared under
// a camera that has not moved" -- the second being what making one where you
// stand looks like from the outside.
static volatile LONG g_generation = 0;

// The run clock as it stood when a change to the game's file was noticed. This
// is what a newly created save-loc is stamped with, and capturing it here rather
// than when the read finishes is the point: the read is on a background thread
// and takes as long as it takes.
static float g_stampClock = -1.0f;
static bool g_stampValid = false;
static float g_stampGained = 0.0f, g_stampLost = 0.0f, g_stampPeak = 0.0f;
static bool g_stampEnergy = false;

// Something worth showing on screen for a moment.
//
// The age is a real timestamp, not a counter. It used to advance by 1/200 per
// call and the only caller is once per rendered frame, so "two seconds" was two
// hundred frames -- 6.7 real seconds at 60 fps and 1.7 at 240. A note that
// borrows a row on the crosshair readout has to expire in seconds, not in
// however long a frame happens to take.
static char g_recent[96] = {0};
static DWORD g_recentAt = 0;
static WrSavelocNote g_recentKind = WR_NOTE_NONE;

static void NoteNow(void)
{
    g_recentAt = GetTickCount();
    if (!g_recentAt)
        g_recentAt = 1;         // 0 means "nothing has happened"
}

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

static const char *GamePath(void)
{
    static char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\momentum\\savedlocs.txt",
                WrGameDir());
    return path;
}

static const char *SidecarPath(const char *map)
{
    static char rel[MAX_PATH];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "savelocs\\%s.txt", map);
    return WrDataPath(rel);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
//
// Not a general KeyValues parser, and deliberately so. We need two things: the
// "pos" and the "vel" of every entry inside one named map's block. So this
// tracks brace depth, notes the depth at which the map's block opened, and reads
// those two keys below it. A malformed file yields no save-locs rather than a
// crash.
//
// THE VELOCITY WAS ALWAYS THERE
//
// Only "pos" was read until v0.4.1, and the rest of the record -- vel, ang,
// zone, track, gravityScale, crouched -- fell through the tokenizer and was
// discarded. That made loading a save-loc look, to the energy readout, exactly
// like any other 400-unit teleport: every filter reset, and about a third of a
// second of climbing back to a number the file had known all along.
//
// Measured across the 3239 save-locs of 261 maps on this machine: "vel" is
// present and finite in ALL of them, 62% were saved above 250 u/s and 46% above
// 1000, and "predictedVel" disagrees with "vel" in two records out of 3239. So
// the field is both universally available and unambiguous. See WrEnergySeed.

static bool ParseFile(const char *path, const char *map, Saveloc *out,
                      int maxOut, int *count, int *current)
{
    *count = 0;
    if (current) *current = -1;

    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f)
        return false;

    char line[512];
    int depth = 0;
    int mapDepth = -1;
    bool inMap = false;
    char pendingKey[128] = {0};

    // Which record the block currently open created, or -1 for none yet.
    //
    // The velocity has to attach to the save-loc it belongs to and to no other.
    // Valve writes these blocks alphabetically so "pos" does come before "vel"
    // in every one of the 3239 records here -- but a parser that relies on that
    // would, on the day it stopped being true, silently hand each save-loc the
    // PREVIOUS one's velocity. Which is the worst possible failure: a plausible
    // number, from the wrong place, with nothing on screen to say so.
    int blockIndex = -1;
    int entryGameIndex = -1;

    // Which section of the map's block we are inside: "cps", or "startmarks".
    //
    // A map block has more than one list of positions in it. Alongside "cps" --
    // the save-locs -- Momentum keeps "startmarks", the place a run was started
    // from, and those carry a pos with no vel, no time, no zone and no track.
    // There are 16 of them across 12 maps here.
    //
    // Nothing used to distinguish them, so a startmark has always been read in
    // as a save-loc. Harmless while only the position was read; not harmless
    // now, because it is a position that can be landed on and has no velocity to
    // land with, and because "an entry we have never seen before" is the signal
    // that stamps a newly created save-loc with the clock.
    char section[64] = {0};

    while (fgets(line, sizeof(line), f))
    {
        // Trim.
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;

        if (*p == '{')
        {
            depth++;
            if (!inMap && pendingKey[0] && _stricmp(pendingKey, map) == 0)
            {
                inMap = true;
                mapDepth = depth;
            }
            // The block one level inside the map's is the section: "cps" or
            // "startmarks". One level further in is an entry.
            if (inMap && depth == mapDepth + 1)
                strncpy_s(section, sizeof(section), pendingKey, _TRUNCATE);
            // The numbered block one level inside `cps` is the index exposed by
            // Momentum's `cur` field. Unlike position, this distinguishes two
            // save states made at the exact same respawn point.
            if (inMap && depth == mapDepth + 2)
            {
                entryGameIndex = -1;
                if (_stricmp(section, "cps") == 0)
                {
                    char *end = NULL;
                    long v = strtol(pendingKey, &end, 10);
                    if (end && end != pendingKey && *end == '\0' &&
                        v >= 0 && v <= 1000000)
                        entryGameIndex = (int)v;
                }
            }
            pendingKey[0] = '\0';
            blockIndex = -1;        // a new block owns nothing yet
            continue;
        }
        if (*p == '}')
        {
            if (inMap && depth == mapDepth)
                break;                  // finished this map's block
            if (inMap && depth == mapDepth + 1)
                section[0] = '\0';
            if (inMap && depth == mapDepth + 2)
                entryGameIndex = -1;
            depth--;
            pendingKey[0] = '\0';
            blockIndex = -1;
            continue;
        }

        // "key"  "value"   or   "key" alone (a block name on the next line).
        if (*p != '"')
            continue;
        p++;
        char key[128];
        int n = 0;
        while (*p && *p != '"' && n < (int)sizeof(key) - 1)
            key[n++] = *p++;
        key[n] = '\0';
        if (*p != '"')
            continue;
        p++;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '"')
        {
            strcpy_s(pendingKey, sizeof(pendingKey), key);
            continue;
        }
        p++;
        char val[160];
        n = 0;
        while (*p && *p != '"' && n < (int)sizeof(val) - 1)
            val[n++] = *p++;
        val[n] = '\0';
        pendingKey[0] = '\0';

        if (inMap && depth == mapDepth && _stricmp(key, "cur") == 0)
        {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            if (current && end && end != val && *end == '\0' &&
                v >= -1 && v <= 1000000)
                *current = (int)v;
        }
        else if (inMap && _stricmp(key, "pos") == 0)
        {
            // Cleared first, so a commit that fails cannot leave a later "vel"
            // pointing at the previous record. That matters past maxOut, where
            // positions stop being committed but the velocity lines keep coming.
            blockIndex = -1;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (*count < maxOut && sscanf_s(val, "%f %f %f", &x, &y, &z) == 3)
            {
                Vec3 p = WrVec(x, y, z);
                // sscanf_s reads "-nan(ind)" happily and returns 3 for it, so
                // the field count is not the guard here; WrSaneFloat's
                // self-comparison is. This file holds thirteen such values.
                if (WrSaneVec(p))
                {
                    // Zeroed rather than assigned field by field. The caller
                    // parses into a static array that is reused every read, so
                    // anything left untouched here would silently inherit
                    // whatever the LAST parse put at this index -- which for a
                    // velocity would mean a startmark quietly acquiring the
                    // speed of whichever save-loc held the slot before it.
                    memset(&out[*count], 0, sizeof(out[*count]));
                    out[*count].pos = p;
                    out[*count].ourTime = -1.0f;
                    out[*count].fromCps = (_stricmp(section, "cps") == 0);
                    out[*count].gameIndex = out[*count].fromCps
                                                ? entryGameIndex : -1;
                    blockIndex = *count;
                    (*count)++;
                }
            }
        }
        else if (inMap && blockIndex >= 0 && _stricmp(key, "vel") == 0)
        {
            // _stricmp, not a prefix test: "predictedVel" is a real key in every
            // one of these blocks and is not the field the game restores from.
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (sscanf_s(val, "%f %f %f", &x, &y, &z) == 3)
            {
                Vec3 v = WrVec(x, y, z);
                // Refused rather than clamped. This file genuinely does contain
                // garbage -- a "stamina" of -nan(ind) turned up in the 3239
                // records here -- and a refused velocity costs only the old
                // behaviour, where a wrong one is a wrong readout that looks
                // exactly as confident as a right one.
                if (WrSaneVec(v) && WrLength(v) <= WR_SAVELOC_MAX_SPEED)
                {
                    out[blockIndex].vel = v;
                    out[blockIndex].haveVel = true;
                }
            }
        }
    }
    fclose(f);
    return true;
}

static bool ParseGameFile(const char *map, Saveloc *out, int maxOut,
                          int *count, int *current)
{
    return ParseFile(GamePath(), map, out, maxOut, count, current);
}

static void AssignOrdinals(Saveloc *locs, int count);

// The same parser, over a file of the caller's choosing, reported in the public
// struct. Exists so the harness can drive the section and velocity rules against
// a fixture: those rules are about the SHAPE of Momentum's file, and a test that
// re-implemented the shape would only ever agree with itself.
bool WrSavelocParseFile(const char *path, const char *map,
                        WrSavelocHit *out, int maxOut, int *count)
{
    if (count) *count = 0;
    if (!path || !map || !out || maxOut <= 0 || !count)
        return false;

    Saveloc *tmp = (Saveloc *)calloc((size_t)maxOut, sizeof(Saveloc));
    if (!tmp)
        return false;

    int n = 0;
    bool ok = ParseFile(path, map, tmp, maxOut, &n, NULL);
    if (ok)
    {
        AssignOrdinals(tmp, n);
        for (int i = 0; i < n; i++)
        {
            memset(&out[i], 0, sizeof(out[i]));
            out[i].pos = tmp[i].pos;
            out[i].vel = tmp[i].vel;
            out[i].haveVel = tmp[i].haveVel;
            out[i].fromCps = tmp[i].fromCps;
            out[i].seconds = tmp[i].ourTime;
            out[i].ordinal = tmp[i].ordinal;
            out[i].gameIndex = tmp[i].gameIndex;
        }
        *count = n;
    }
    free(tmp);
    return ok;
}

// ---------------------------------------------------------------------------
// Our sidecar
// ---------------------------------------------------------------------------

// Number each entry among those sharing its position, in file order. Without
// this two save-locs at the same respawn point are indistinguishable, and the
// nearest-wins search below always picks whichever came first.
static void AssignOrdinals(Saveloc *locs, int count)
{
    for (int i = 0; i < count; i++)
    {
        int n = 0;
        for (int j = 0; j < i; j++)
        {
            float dx = locs[j].pos.x - locs[i].pos.x;
            float dy = locs[j].pos.y - locs[i].pos.y;
            float dz = locs[j].pos.z - locs[i].pos.z;
            if (dx * dx + dy * dy + dz * dz < 1.0f)
                n++;
        }
        locs[i].ordinal = n;
    }
}

static void LoadSidecar(const char *map, Saveloc *locs, int count, int *timed)
{
    *timed = 0;
    FILE *f = NULL;
    if (fopen_s(&f, SidecarPath(map), "r") != 0 || !f)
        return;

    int version = 1;        // no tag line means the original format
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '\n' || line[0] == '\r')
            continue;
        if (line[0] == '#')
        {
            int v = 0;
            if (sscanf_s(line, "# " SIDECAR_TAG " %d", &v) == 1 && v > 0)
                version = v;
            continue;
        }

        float x, y, z, t;
        int ord = 0, flags = 0;
        float gained = 0.0f, lost = 0.0f, peak = 0.0f;
        if (version >= 3)
        {
            // The v2 tail is optional even in a v3 file, so a row that was
            // written before the energy existed -- or trimmed by hand -- still
            // loads as a plain time rather than being dropped.
            int got = sscanf_s(line, "%f %f %f %d %f %d %f %f %f",
                               &x, &y, &z, &ord, &t, &flags,
                               &gained, &lost, &peak);
            if (got < 5)
                continue;
            if (got < 9)
                flags &= ~SIDE_HAS_ENERGY;
        }
        else if (version >= 2)
        {
            if (sscanf_s(line, "%f %f %f %d %f", &x, &y, &z, &ord, &t) != 5)
                continue;
        }
        else if (sscanf_s(line, "%f %f %f %f", &x, &y, &z, &t) != 4)
        {
            continue;
        }

        for (int i = 0; i < count; i++)
        {
            if (locs[i].ourTime >= 0.0f)
                continue;
            float dx = locs[i].pos.x - x, dy = locs[i].pos.y - y;
            float dz = locs[i].pos.z - z;
            if (dx * dx + dy * dy + dz * dz >= 1.0f)
                continue;
            // In v2 the ordinal disambiguates a shared position. In v1 there is
            // none, so the first untimed match takes it -- which is the
            // ambiguity that version exists to record.
            if (version >= 2 && locs[i].ordinal != ord)
                continue;
            locs[i].ourTime = t;
            // Against 2, not against SIDECAR_VERSION. `suspect` means one exact
            // thing -- written by v1's proximity bug, so it cannot be trusted --
            // and testing it against whatever the current version happens to be
            // would have re-marked every good time on disk as doubtful the next
            // time this number went up.
            locs[i].suspect = (version < 2);
            locs[i].byHand = (flags & SIDE_BY_HAND) != 0;
            if (flags & SIDE_HAS_ENERGY)
            {
                locs[i].haveEnergy = true;
                locs[i].gained = gained;
                locs[i].lost = lost;
                locs[i].peak = peak;
            }
            (*timed)++;
            break;
        }
    }
    fclose(f);
}

// Takes a snapshot rather than reading the shared array. It used to be called
// after LeaveCriticalSection and read g_locs/g_count/g_map directly, while the
// background reader could be memcpy-ing over all three -- and it does file I/O,
// so holding the lock across it instead would put a synchronous disk write
// inside Present with a background thread waiting on it.
static void SaveSidecar(const char *map, const Saveloc *locs, int count)
{
    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), WrDataPath("savelocs"));
    CreateDirectoryA(dir, NULL);

    FILE *f = NULL;
    if (fopen_s(&f, SidecarPath(map), "w") != 0 || !f)
        return;
    fprintf(f, "# " SIDECAR_TAG " %d\n", SIDECAR_VERSION);
    fprintf(f, "# WrLines: our own elapsed time at each of this map's save-locs.\n");
    fprintf(f, "# Momentum's savedlocs.txt has a \"time\" field but never fills\n");
    fprintf(f, "# it in, so this is where ours lives. Keyed on position (indices\n");
    fprintf(f, "# renumber when one is deleted) plus an ordinal, because several\n");
    fprintf(f, "# save-locs commonly share a respawn point.\n");
    fprintf(f, "# The velocity comes from the game's own file and needs nothing\n");
    fprintf(f, "# from here; what the energy readout had BANKED does not exist\n");
    fprintf(f, "# anywhere else, so it is recorded beside the time.\n");
    fprintf(f, "# x y z ordinal seconds flags gained lost peak\n");
    fprintf(f, "# flags: 1 = time typed in by hand, 2 = energy columns mean something\n");
    for (int i = 0; i < count; i++)
    {
        // Zero is refused. Stamping is enabled only while Momentum says its
        // timer is RUNNING, so 0.000 means the save arrived before a meaningful
        // run tick -- and every old 0.000 on disk came from that bug, then
        // restored itself for ever afterwards.
        if (locs[i].ourTime <= 0.0f)
            continue;
        int flags = 0;
        if (locs[i].byHand)     flags |= SIDE_BY_HAND;
        if (locs[i].haveEnergy) flags |= SIDE_HAS_ENERGY;
        fprintf(f, "%.1f %.1f %.1f %d %.3f %d %.1f %.1f %.1f\n",
                locs[i].pos.x, locs[i].pos.y, locs[i].pos.z,
                locs[i].ordinal, locs[i].ourTime, flags,
                locs[i].gained, locs[i].lost, locs[i].peak);
    }
    fclose(f);
}

// Copy out under the lock, write outside it.
// `forMap` is the map the caller's data belongs to, and NULL means "whatever is
// current". The background reader passes the map it actually parsed: it takes as
// long as it takes, and a flush that straddled a map change wrote one map's
// times into another map's file.
static void FlushSidecar(const char *forMap)
{
    static Saveloc snap[MAX_SAVELOCS];
    char map[72];
    int n;

    EnterCriticalSection(&g_cs);
    n = g_count;
    memcpy(snap, g_locs, sizeof(Saveloc) * (size_t)n);
    if (forMap && *forMap)
        strcpy_s(map, sizeof(map), forMap);
    else
        strcpy_s(map, sizeof(map), g_map);
    bool stillOurs = (strcmp(map, g_map) == 0);
    LeaveCriticalSection(&g_cs);

    if (map[0] && stillOurs)
        SaveSidecar(map, snap, n);
}

// ---------------------------------------------------------------------------

static DWORD WINAPI ReadThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    char map[72];
    EnterCriticalSection(&g_cs);
    strcpy_s(map, sizeof(map), g_map);
    LeaveCriticalSection(&g_cs);

    // Snapshotted before the parse, because the commit below sets it and the
    // question "was this the first look at this map" has to be answered with
    // what was true when the read began.
    bool readOnceBefore = g_readOnce;

    static Saveloc found[MAX_SAVELOCS];
    int n = 0;
    int current = -1;
    bool ok = map[0] &&
              ParseGameFile(map, found, MAX_SAVELOCS, &n, &current);

    int timed = 0;
    if (ok)
    {
        AssignOrdinals(found, n);
        LoadSidecar(map, found, n, &timed);
    }

    bool dirty = false;
    EnterCriticalSection(&g_cs);

    // Did the map change while this read was in flight? If it did, everything
    // below is about a map we have left: it must not claim to have looked at
    // the new one, and it must not stamp anything.
    bool sameMap = (strcmp(map, g_map) == 0);
    if (sameMap)
    {
        // Set even when the read FAILED, a few lines down. "Have we looked
        // here" is a different question from "was there anything to find", and
        // on a machine with no savedlocs.txt at all the first read fails, the
        // first save-loc you ever make creates the file, and treating that read
        // as the first look means that save-loc is not stamped either.
        g_readOnce = true;
    }

    if (ok)
    {
        // A rewrite that keeps the same cps table and names a current slot is
        // Momentum loading/selecting a save state. Creation and overwrite both
        // change the table; deletion changes its size. This event reaches the
        // timer even when loading the current slot again moves the player zero
        // units and leaves the timer entity otherwise continuous.
        bool sameRows = sameMap && readOnceBefore && n == g_count;
        for (int i = 0; sameRows && i < n; i++)
        {
            if (found[i].fromCps != g_locs[i].fromCps ||
                found[i].gameIndex != g_locs[i].gameIndex ||
                WrDist(found[i].pos, g_locs[i].pos) >= 0.5f)
                sameRows = false;
        }

        bool firstForMap = !readOnceBefore || !sameMap;

        // Carry forward times for save-locs that are still there, so a re-read
        // triggered by the user making a new one does not lose the others, and
        // note which entries matched nothing we already knew.
        for (int i = 0; i < n; i++)
        {
            if (found[i].ourTime >= 0.0f)
                continue;
            bool matched = false;
            for (int j = 0; j < g_count; j++)
            {
                float dx = g_locs[j].pos.x - found[i].pos.x;
                float dy = g_locs[j].pos.y - found[i].pos.y;
                float dz = g_locs[j].pos.z - found[i].pos.z;
                if (dx * dx + dy * dy + dz * dz >= 1.0f)
                    continue;
                if (g_locs[j].ordinal != found[i].ordinal)
                    continue;
                matched = true;
                if (g_locs[j].ourTime >= 0.0f)
                {
                    found[i].ourTime = g_locs[j].ourTime;
                    found[i].suspect = g_locs[j].suspect;
                    found[i].byHand = g_locs[j].byHand;
                    found[i].haveEnergy = g_locs[j].haveEnergy;
                    found[i].gained = g_locs[j].gained;
                    found[i].lost = g_locs[j].lost;
                    found[i].peak = g_locs[j].peak;
                    timed++;
                }
                break;
            }

            // THIS is a save-loc that was just made: the game's file changed,
            // and the re-read turned up an entry that matches nothing we held.
            // Not "the player is standing near an untimed one", which is what
            // this used to test and which fires every time you walk past.
            //
            // Suppressed on the first read for a map, where "new" only means
            // "we have not looked here before".
            // `fromCps`, because a startmark is not a save-loc. It appears in
            // the same file, matches nothing we hold the first time it shows up,
            // and would otherwise be stamped with the clock as though the player
            // had just made a save-loc there.
            if (!matched && !firstForMap && sameMap && found[i].fromCps &&
                found[i].gameIndex == current && g_stampValid &&
                g_stampClock > 0.0f)
            {
                found[i].ourTime = g_stampClock;
                found[i].suspect = false;
                found[i].byHand = false;
                if (g_stampEnergy)
                {
                    found[i].haveEnergy = true;
                    found[i].gained = g_stampGained;
                    found[i].lost = g_stampLost;
                    found[i].peak = g_stampPeak;
                }
                timed++;
                dirty = true;
                WrLogf("saveloc: NEW save-loc %d at (%.0f %.0f %.0f) stamped "
                       "with %.2fs", current + 1, found[i].pos.x,
                       found[i].pos.y, found[i].pos.z, g_stampClock);
                _snprintf_s(g_recent, sizeof(g_recent), _TRUNCATE,
                            "save-loc saved at %.2fs", g_stampClock);
                NoteNow();
                g_recentKind = WR_NOTE_STAMPED;
            }
        }

        memcpy(g_locs, found, sizeof(Saveloc) * (size_t)n);
        g_count = n;
        g_current = current;
        if (sameRows && current >= 0)
            InterlockedIncrement(&g_loadSerial);
        InterlockedIncrement(&g_generation);
        g_timed = timed;
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "%d save-loc%s for this map, %d with a time",
                    n, n == 1 ? "" : "s", timed);
    }
    else
    {
        g_count = 0;
        g_timed = 0;
        g_current = -1;
        strcpy_s(g_status, sizeof(g_status),
                 "no savedlocs.txt, or none for this map");
    }
    LeaveCriticalSection(&g_cs);

    if (dirty)
        FlushSidecar(map);

    InterlockedExchange(&g_busy, 0);
    return 0;
}

void WrSavelocRefresh(const char *map, float elapsed, bool running)
{
    EnsureCs();
    if (!map || !*map || !WrGameDir()[0])
        return;

    // Only when the file has actually changed, or the map has.
    WIN32_FILE_ATTRIBUTE_DATA fad;
    long long mtime = 0;
    if (GetFileAttributesExA(GamePath(), GetFileExInfoStandard, &fad))
        mtime = ((long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                fad.ftLastWriteTime.dwLowDateTime;

    bool mapChanged;
    EnterCriticalSection(&g_cs);
    mapChanged = (strcmp(map, g_map) != 0);
    if (mapChanged)
    {
        strcpy_s(g_map, sizeof(g_map), map);
        g_readOnce = false;         // a fresh map has not been looked at yet
    }
    LeaveCriticalSection(&g_cs);

    bool changed = mapChanged || mtime != g_mtime;
    if (!changed && !g_rereadPending)
        return;

    if (changed)
    {
        g_mtime = mtime;

        // Captured HERE, not when the read finishes. The read is on a background
        // thread and takes as long as it takes; the clock that belongs to a new
        // save-loc is the one at the instant the file changed. dllmain reads the
        // game timer first, so this is the current game tick rather than the
        // previous rendered frame.
        //
        // And captured only on a real change, never on the retry below: a retry
        // happens some frames later, and re-reading the clock then would stamp
        // the save-loc with when we got round to it rather than when it was made.
        g_stampClock = elapsed;
        g_stampValid = running && !mapChanged;

        // And the banked energy from the same instant, for the same reason. The
        // instantaneous figures do not need this -- they come back out of the
        // game's own file on the way in -- but what has been GAINED and LOST over
        // the attempt so far exists nowhere except in here.
        g_stampGained = WrEnergyGained();
        g_stampLost = WrEnergyLost();
        g_stampPeak = WrEnergyPeak();
        g_stampEnergy = WrEnergyValid();
    }

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
    {
        // A read is already running and this change is not in it. Remember to
        // come back: the mtime above has already been committed, so without
        // this the change is seen once, dropped, and never seen again.
        g_rereadPending = true;
        return;
    }
    g_rereadPending = false;
    if (g_thread)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_thread = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    if (!g_thread)
    {
        // The mtime is already committed, so without re-arming the pending flag
        // this change is seen once and never again -- the exact failure the
        // flag exists to prevent, reached by a different route.
        g_rereadPending = true;
        InterlockedExchange(&g_busy, 0);
    }
}

// The save-loc you just landed on, whether or not we have ever timed it.
//
// This replaced a search that skipped untimed entries, and the difference is
// not cosmetic. Two things now want to know which save-loc was loaded -- the
// clock, and the energy readout -- and the velocity that seeds the energy is in
// the GAME's file, so it exists for every save-loc, while our time exists for
// about four in a hundred. Two independent searches with different filters could
// therefore land on two different save-locs and restore a clock from one and a
// speed from the other. One match, both answers, and the caller decides what it
// can use.
//
// It also fixes a smaller wrong: landing on an untimed save-loc used to restore
// the clock from a TIMED one up to 24 units away. Nearest wins, then its time is
// used only if it has one.
static bool MatchWithin(const Vec3 &pos, float radius, WrSavelocHit *out)
{
    if (!g_csReady)
        return false;
    EnterCriticalSection(&g_cs);
    int chosen = -1;
    float bestD = radius * radius;

    // Momentum writes the exact active cps slot to `cur`. Prefer it whenever
    // it agrees with the landing position. Position alone cannot distinguish
    // two save states made at the same respawn point; the game index can.
    for (int i = 0; i < g_count; i++)
    {
        if (!g_locs[i].fromCps || g_locs[i].gameIndex != g_current)
            continue;
        float dz = g_locs[i].pos.z - pos.z;
        float dx = g_locs[i].pos.x - pos.x, dy = g_locs[i].pos.y - pos.y;
        if (dz <= MATCH_VERTICAL && dz >= -MATCH_VERTICAL &&
            dx * dx + dy * dy < bestD)
            chosen = i;
        break;
    }

    for (int i = 0; i < g_count; i++)
    {
        if (chosen >= 0)
            break;
        float dz = g_locs[i].pos.z - pos.z;
        if (dz > MATCH_VERTICAL || dz < -MATCH_VERTICAL)
            continue;
        float dx = g_locs[i].pos.x - pos.x, dy = g_locs[i].pos.y - pos.y;
        float d = dx * dx + dy * dy;
        if (d < bestD)
        {
            bestD = d;
            chosen = i;
        }
    }

    if (chosen >= 0 && out)
    {
        // Copied out under the lock. The background reader memcpy's over the
        // whole array, so a pointer into it would be a pointer into something
        // being rewritten.
        out->pos = g_locs[chosen].pos;
        out->vel = g_locs[chosen].vel;
        out->haveVel = g_locs[chosen].haveVel;
        out->fromCps = g_locs[chosen].fromCps;
        out->ordinal = g_locs[chosen].ordinal;
        out->gameIndex = g_locs[chosen].gameIndex;
        out->seconds = g_locs[chosen].ourTime;
        out->suspect = g_locs[chosen].suspect;
        out->byHand = g_locs[chosen].byHand;
        out->haveEnergy = g_locs[chosen].haveEnergy;
        out->gained = g_locs[chosen].gained;
        out->lost = g_locs[chosen].lost;
        out->peak = g_locs[chosen].peak;
    }
    LeaveCriticalSection(&g_cs);
    return chosen >= 0;
}

bool WrSavelocCurrent(WrSavelocHit *out)
{
    if (!g_csReady)
        return false;
    bool ok = false;
    EnterCriticalSection(&g_cs);
    for (int i = 0; g_current >= 0 && i < g_count; i++)
    {
        if (!g_locs[i].fromCps || g_locs[i].gameIndex != g_current)
            continue;
        if (out)
        {
            out->pos = g_locs[i].pos;
            out->vel = g_locs[i].vel;
            out->haveVel = g_locs[i].haveVel;
            out->fromCps = true;
            out->ordinal = g_locs[i].ordinal;
            out->gameIndex = g_locs[i].gameIndex;
            out->seconds = g_locs[i].ourTime;
            out->suspect = g_locs[i].suspect;
            out->byHand = g_locs[i].byHand;
            out->haveEnergy = g_locs[i].haveEnergy;
            out->gained = g_locs[i].gained;
            out->lost = g_locs[i].lost;
            out->peak = g_locs[i].peak;
        }
        ok = true;
        break;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

unsigned int WrSavelocLoadSerial(void)
{
    return (unsigned int)InterlockedCompareExchange(&g_loadSerial, 0, 0);
}

bool WrSavelocMatch(const Vec3 &pos, WrSavelocHit *out)
{
    return MatchWithin(pos, MATCH_RADIUS, out);
}

// "You are standing exactly where a save-loc says", which is what a load that
// did not move you far enough to look like a teleport leaves behind.
//
// Same loop, one twenty-fourth of the radius. The caller supplies the edge:
// being here is not an event, ARRIVING here is, and holding the load key parks
// you on the spot for as long as you hold it.
bool WrSavelocExactMatch(const Vec3 &pos, WrSavelocHit *out)
{
    return MatchWithin(pos, MATCH_EXACT_RADIUS, out);
}

void WrSavelocInstallForTest(const WrSavelocHit *rows, int n)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (n > MAX_SAVELOCS) n = MAX_SAVELOCS;
    if (n < 0) n = 0;
    g_count = n;
    g_timed = 0;
    InterlockedIncrement(&g_generation);
    for (int i = 0; i < n; i++)
    {
        memset(&g_locs[i], 0, sizeof(g_locs[i]));
        g_locs[i].pos = rows[i].pos;
        g_locs[i].vel = rows[i].vel;
        g_locs[i].haveVel = rows[i].haveVel;
        g_locs[i].fromCps = rows[i].fromCps;
        g_locs[i].ourTime = rows[i].seconds;
        g_locs[i].ordinal = rows[i].ordinal;
        g_locs[i].gameIndex = rows[i].gameIndex;
        if (rows[i].seconds >= 0.0f)
            g_timed++;
    }
    g_current = -1;
    LeaveCriticalSection(&g_cs);
}

void WrSavelocSetCurrentForTest(int gameIndex)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    g_current = gameIndex;
    LeaveCriticalSection(&g_cs);
}

bool WrSavelocTimeAt(const Vec3 &pos, float *seconds)
{
    WrSavelocHit hit;
    if (!WrSavelocMatch(pos, &hit) || hit.seconds < 0.0f)
        return false;
    if (seconds) *seconds = hit.seconds;
    return true;
}

unsigned int WrSavelocGeneration(void)
{
    return (unsigned int)InterlockedCompareExchange(&g_generation, 0, 0);
}

int WrSavelocCount(void) { return g_count; }
int WrSavelocTimedCount(void) { return g_timed; }
const char *WrSavelocStatus(void) { return g_status; }

bool WrSavelocAt(int index, WrSavelocRow *out)
{
    if (!g_csReady)
        return false;
    bool ok = false;
    EnterCriticalSection(&g_cs);
    if (index >= 0 && index < g_count)
    {
        if (out)
        {
            out->pos = g_locs[index].pos;
            out->seconds = g_locs[index].ourTime;
            out->suspect = g_locs[index].suspect;
            out->byHand = g_locs[index].byHand;
            out->haveVel = g_locs[index].haveVel;
            out->speed = g_locs[index].haveVel ? WrLength(g_locs[index].vel)
                                               : 0.0f;
        }
        ok = true;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

void WrSavelocForget(int index)
{
    if (!g_csReady)
        return;
    bool dirty = false;
    EnterCriticalSection(&g_cs);
    if (index >= 0 && index < g_count && g_locs[index].ourTime >= 0.0f)
    {
        g_locs[index].ourTime = -1.0f;
        g_locs[index].suspect = false;
        g_locs[index].byHand = false;
        g_locs[index].haveEnergy = false;
        if (g_timed > 0) g_timed--;
        dirty = true;
    }
    LeaveCriticalSection(&g_cs);
    if (dirty)
        FlushSidecar(NULL);
}

// A time typed in by hand.
//
// The auto-stamp only ever fires for a save-loc that is CREATED while the clock
// is running, and deliberately so: the version that stamped whichever untimed
// save-loc you happened to be standing near put twenty bogus entries at
// surf_hades2's spawn, one per lap. That cannot be loosened. But it leaves every
// save-loc made before this tool existed permanently untimed -- 3098 of the 3239
// on this machine -- and there is no way to work out what those times were.
//
// So they are typed. A number the player states is the one kind of answer this
// file has no business inferring.
void WrSavelocSetTime(int index, float seconds)
{
    if (!g_csReady || !(seconds > 0.0f) || seconds != seconds)
        return;
    bool dirty = false;
    EnterCriticalSection(&g_cs);
    if (index >= 0 && index < g_count)
    {
        if (g_locs[index].ourTime < 0.0f)
            g_timed++;
        g_locs[index].ourTime = seconds;
        g_locs[index].suspect = false;      // stated, not guessed at
        g_locs[index].byHand = true;
        // No banked energy goes with a typed time. We were not there.
        g_locs[index].haveEnergy = false;
        g_locs[index].gained = g_locs[index].lost = g_locs[index].peak = 0.0f;
        dirty = true;
    }
    LeaveCriticalSection(&g_cs);
    if (dirty)
    {
        FlushSidecar(NULL);
        WrLogf("saveloc: time %.2fs entered by hand for save-loc %d",
               seconds, index + 1);
    }
}

void WrSavelocForgetAll(void)
{
    if (!g_csReady)
        return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count; i++)
    {
        g_locs[i].ourTime = -1.0f;
        g_locs[i].suspect = false;
        g_locs[i].byHand = false;
        g_locs[i].haveEnergy = false;
    }
    g_timed = 0;
    LeaveCriticalSection(&g_cs);
    FlushSidecar(NULL);
    WrLogf("saveloc: every time for this map forgotten, on request");
}

const char *WrSavelocRecent(float *ageSeconds)
{
    if (ageSeconds)
        *ageSeconds = g_recentAt
                          ? (float)(GetTickCount() - g_recentAt) / 1000.0f
                          : 1e9f;
    return g_recent;
}

void WrSavelocNoteRestore(float seconds)
{
    _snprintf_s(g_recent, sizeof(g_recent), _TRUNCATE,
                "clock restored to %.2fs", seconds);
    NoteNow();
    g_recentKind = WR_NOTE_RESTORED;
}

WrSavelocNote WrSavelocRecentKind(void) { return g_recentKind; }

// A load we DID notice, on a save-loc we have never timed.
//
// Worth its own message because the alternative is silence, and silence here is
// ambiguous in the one way that matters: it looks identical to not having
// noticed the load at all. That ambiguity is most of why this feature read as
// broken -- 3098 of the 3239 save-locs on this machine have no time, so the
// common case was a correct answer that said nothing.
void WrSavelocNoteNoTime(void)
{
    _snprintf_s(g_recent, sizeof(g_recent), _TRUNCATE,
                "save-loc loaded -- no time saved for that one");
    NoteNow();
    g_recentKind = WR_NOTE_NO_TIME;
}

void WrSavelocShutdown(void) {}
