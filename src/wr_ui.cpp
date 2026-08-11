// wr_ui.cpp  --  the panel. INSERT toggles it.
//
// Deliberately plain: this is a tool for looking at lines, not a thing to look
// at itself. The Diagnostics tab is not decoration -- against a closed-source
// engine, being able to see exactly which vtable index resolved and which oracle
// accepted it is the difference between "it broke" and "it broke here".

#include "wr_ui.h"
#include "wr_render.h"
#include "wr_path.h"
#include "wr_engine.h"
#include "wr_scan.h"
#include "wr_steam.h"
#include "wr_energy.h"
#include "wr_limit.h"
#include "wr_extract.h"
#include "wr_timer.h"
#include "wr_savelocs.h"
#include "wr_maps.h"
#include "wr_board.h"
#include "wr_profile.h"
#include "wr_intogame.h"
#include "wr_start.h"
#include "wr_stress.h"
#include "wr_hook.h"
#include "wr_settings.h"
#include "wr_log.h"

#include "imgui.h"

#include <float.h>
#include <math.h>
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_uiMap[72] = {0};

// The four keys that work WITHOUT opening the panel.
//
// Read by the hotkey thread; plain ints, written only from the panel, so no
// synchronisation is needed beyond the atomicity of an aligned 32-bit store.
// Everything they reach is a thing you want to change mid-run, which is exactly
// when a panel is the wrong answer -- and each one is rebindable, because we
// cannot know what the player has bound. The thread READS the key rather than
// swallowing it, so a collision means the game acts on it too rather than
// anything being broken.
static int g_hudCycleKey = VK_NEXT;         // page down: next centre-box mode
static int g_hudCycleBackKey = VK_PRIOR;    // page up: previous
static int g_pickToggleKey = VK_HOME;       // the "whose line is this" plate
static int g_overlayToggleKey = VK_END;     // the corner block
static int g_quickKey = VK_DELETE;          // the quick menu

int WrUiHudCycleKey(void) { return g_hudCycleKey; }
int WrUiHudCycleBackKey(void) { return g_hudCycleBackKey; }
int WrUiPickToggleKey(void) { return g_pickToggleKey; }
int WrUiOverlayToggleKey(void) { return g_overlayToggleKey; }
int WrUiQuickKey(void) { return g_quickKey; }

// The one list of bindable keys, and the one place a virtual key is turned into
// a name. There used to be two near-identical copies of this, one per binding,
// which is how you end up with a key that is offered in one combo and not the
// other for no reason anybody chose.
static const struct { int vk; const char *name; } kBindKeys[] = {
    { 0,          "(none)"    },
    { VK_HOME,    "Home"      },
    { VK_END,     "End"       },
    { VK_PRIOR,   "Page Up"   },
    { VK_NEXT,    "Page Down" },
    { VK_DELETE,  "Delete"    },
    { VK_F1,      "F1"        },
    { VK_F2,      "F2"        },
    { VK_F3,      "F3"        },
    { VK_F4,      "F4"        },
    { VK_F6,      "F6"        },
    { VK_F7,      "F7"        },
    { VK_F8,      "F8"        },
    { VK_OEM_3,   "`"         },
    { VK_OEM_4,   "["         },
    { VK_OEM_6,   "]"         },
    { VK_OEM_5,   "\\"        },
};
static const int kBindKeyCount = (int)(sizeof(kBindKeys) / sizeof(kBindKeys[0]));

const char *WrUiKeyName(int vk)
{
    for (int i = 0; i < kBindKeyCount; i++)
        if (kBindKeys[i].vk == vk)
            return kBindKeys[i].name;
    return "(unlisted)";
}

// One combo, and one collision check across ALL FOUR bindings rather than the
// pairwise one this used to be -- with four keys, pairwise means three of the
// six possible clashes go unmentioned.
static void KeyBindCombo(const char *label, int *key)
{
    int sel = 0;
    for (int i = 0; i < kBindKeyCount; i++)
        if (kBindKeys[i].vk == *key) { sel = i; break; }

    const char *names[kBindKeyCount];
    for (int i = 0; i < kBindKeyCount; i++)
        names[i] = kBindKeys[i].name;

    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo(label, &sel, names, kBindKeyCount))
        *key = kBindKeys[sel].vk;

    if (!*key)
        return;

    // Five now, and the quick menu is in the list rather than being a fixed key
    // for a reason worth stating: Delete was ALREADY offered here for the four
    // HUD toggles, so a hard-coded Delete would have been the one collision this
    // check could not mention.
    const int *others[4];
    const char *what[4];
    int n = 0;
    if (key != &g_hudCycleKey)     { others[n] = &g_hudCycleKey;     what[n++] = "next mode"; }
    if (key != &g_hudCycleBackKey) { others[n] = &g_hudCycleBackKey; what[n++] = "previous mode"; }
    if (key != &g_pickToggleKey)   { others[n] = &g_pickToggleKey;   what[n++] = "the line plate"; }
    if (key != &g_overlayToggleKey) { others[n] = &g_overlayToggleKey; what[n++] = "the corner block"; }
    if (key != &g_quickKey)        { others[n] = &g_quickKey;        what[n++] = "the quick menu"; }

    for (int i = 0; i < n; i++)
    {
        if (*others[i] != *key)
            continue;
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%s is also bound to %s -- one press does both.",
                           WrUiKeyName(*key), what[i]);
        break;
    }
}

// "Near me" means within this many units of the camera. 4096 comfortably covers
// one stage of a surf map without reaching into the next one.
static float s_nearRadius = 4096.0f;
static bool s_nearOnly = false;

static void WrSendForget(void);

void WrUiOnMapChanged(const char *map)
{
    strcpy_s(g_uiMap, sizeof(g_uiMap), map ? map : "");
    // Whatever the last send said was about a run on the map you have left.
    WrSendForget();
}

// Forget the last send message -- see below. Declared here because the map
// change above is the earliest thing that has to drop it.
// A "(?)" that explains a control on hover, for the settings whose reason is
// not obvious from the label.
static void HelpMarker(const char *text)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Case-insensitive substring. Up here rather than beside its first user because
// three different lists filter with it now.
static bool StrIContains(const char *hay, const char *needle)
{
    if (!needle || !*needle)
        return true;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (_strnicmp(p, needle, n) == 0)
            return true;
    return false;
}

static void FormatTime(double t, char *out, int outLen)
{
    int mins = (int)(t / 60.0);
    double secs = t - mins * 60.0;
    if (mins > 0)
        _snprintf_s(out, outLen, _TRUNCATE, "%d:%06.3f", mins, secs);
    else
        _snprintf_s(out, outLen, _TRUNCATE, "%.3f", secs);
}

// FormatTime read backwards: "1:02.310" or "62.31", either of which this panel
// might have printed. Refuses anything it cannot read completely rather than
// taking the leading digits, because a time that silently became 1 instead of
// 1:02 would be indistinguishable from one that was entered that way.
static bool ParseTime(const char *s, float *out)
{
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return false;

    char *end = NULL;
    double a = strtod(s, &end);
    if (end == s || a < 0.0) return false;

    double secs;
    if (*end == ':')
    {
        const char *rest = end + 1;
        char *end2 = NULL;
        double b = strtod(rest, &end2);
        if (end2 == rest || b < 0.0 || b >= 60.0) return false;
        secs = a * 60.0 + b;
        end = end2;
    }
    else
    {
        secs = a;
    }

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return false;                 // trailing rubbish
    if (!(secs > 0.0) || secs > 100.0 * 3600.0) return false;
    if (out) *out = (float)secs;
    return true;
}

// Which save-loc row is having a time typed into it, if any. One at a time, so
// one buffer.
static int s_editLoc = -1;
static bool s_editFocus = false;
static char s_editBuf[32] = "";

static ImVec4 UnpackColour(unsigned int c)
{
    return ImVec4(((c >> 0) & 0xFF) / 255.0f,
                  ((c >> 8) & 0xFF) / 255.0f,
                  ((c >> 16) & 0xFF) / 255.0f,
                  ((c >> 24) & 0xFF) / 255.0f);
}

static unsigned int PackColour(const ImVec4 &v)
{
    unsigned int r = (unsigned int)(WrClampF(v.x, 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned int g = (unsigned int)(WrClampF(v.y, 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned int b = (unsigned int)(WrClampF(v.z, 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned int a = (unsigned int)(WrClampF(v.w, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// ---------------------------------------------------------------------------

// Click-to-sort for the run table.
//
// The run store is deliberately NOT reordered. Two things depend on its order
// being fastest-first: "max runs drawn" means the fastest N, and the delta
// column finds the best run of a track by taking the first match. So sorting
// happens on a list of indices that exists only for display.
enum
{
    RUNCOL_TRACK = 1,
    RUNCOL_PLAYER,
    RUNCOL_TIME,
    RUNCOL_NEAR,
    RUNCOL_POINTS,
    RUNCOL_SPLITS,
};

static const ImGuiTableSortSpecs *g_sortSpecs = NULL;

static int CompareOneColumn(const WrRun *a, const WrRun *b, ImGuiID col)
{
    switch (col)
    {
    case RUNCOL_TRACK:
        if (a->trackType != b->trackType)
            return a->trackType < b->trackType ? -1 : 1;
        if (a->trackNum != b->trackNum)
            return a->trackNum < b->trackNum ? -1 : 1;
        return 0;
    case RUNCOL_PLAYER:
        return _stricmp(a->player, b->player);
    case RUNCOL_TIME:
        if (a->runTime != b->runTime)
            return a->runTime < b->runTime ? -1 : 1;
        return 0;
    case RUNCOL_NEAR:
    {
        // "Not measured yet" sorts last either way round rather than pretending
        // to be nearer than everything else.
        float da = a->nearestDist < 0.0f ? 3.0e38f : a->nearestDist;
        float db = b->nearestDist < 0.0f ? 3.0e38f : b->nearestDist;
        if (da != db)
            return da < db ? -1 : 1;
        return 0;
    }
    case RUNCOL_POINTS:
        if (a->pointCount != b->pointCount)
            return a->pointCount < b->pointCount ? -1 : 1;
        return 0;
    case RUNCOL_SPLITS:
        if (a->markerCount != b->markerCount)
            return a->markerCount < b->markerCount ? -1 : 1;
        return 0;
    default:
        return 0;
    }
}

static int __cdecl CompareRunRows(const void *pa, const void *pb)
{
    const WrRun *a = WrRunAt(*(const int *)pa);
    const WrRun *b = WrRunAt(*(const int *)pb);
    if (!a || !b || !g_sortSpecs)
        return 0;

    for (int s = 0; s < g_sortSpecs->SpecsCount; s++)
    {
        const ImGuiTableColumnSortSpecs *spec = &g_sortSpecs->Specs[s];
        int c = CompareOneColumn(a, b, spec->ColumnUserID);
        if (c != 0)
            return spec->SortDirection == ImGuiSortDirection_Ascending ? c : -c;
    }
    // Ties fall back to the run time, so the order never wobbles between frames.
    if (a->runTime != b->runTime)
        return a->runTime < b->runTime ? -1 : 1;
    return 0;
}

static void SortRunOrder(int *order, int count, ImGuiTableSortSpecs *specs)
{
    if (!specs || specs->SpecsCount <= 0 || count < 2)
        return;
    g_sortSpecs = specs;
    qsort(order, (size_t)count, sizeof(int), CompareRunRows);
    g_sortSpecs = NULL;
}

// The fastest run of the same leg. Runs are already sorted fastest-first, so the
// first match is it. Comparing a stage-3 time against a full-map time would be
// meaningless, which is why the delta column cannot just use row 0.
static WrRun *BestOfSameTrack(const WrRun *r)
{
    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *c = WrRunAt(i);
        if (c && c->trackType == r->trackType && c->trackNum == r->trackNum)
            return c;
    }
    return NULL;
}

static char s_runFilter[64] = "";

// Defined below; shared by the Runs and Board tabs.
// The answer to the last send, and WHICH run asked for it.
//
// This used to be one shared string inside wr_intogame.cpp printed by both tabs,
// so an answer about one run stayed on screen while you looked at another -- and
// two perfectly true per-run answers ("the game already has that one" for a demo
// the fetcher had already copied in, "no .mtv on disk for that run" for one
// whose demo had been deleted in game) read as the tool contradicting itself.
static char s_sendWho[48] = {0};
static char s_sendText[384] = {0};
static bool s_sendBad = false;
// Which tab produced it. DrawIntoGameLine is shared, so without this an answer
// about a Runs-tab run is printed above the Board list, attributed to a player
// who is not in it.
static int s_sendTab = -1;

static void WrSendForget(void)
{
    s_sendText[0] = '\0';
    s_sendWho[0] = '\0';
    s_sendBad = false;
    s_sendTab = -1;
}

// Where each loaded run's .mtv actually is, worked out once per map rather than
// per row per frame.
//
// It has to be worked out at all because two states are invisible otherwise.
// 35 of the 1,749 .wrpath files on this machine have no .mtv anywhere: deleting
// a demo in game leaves the path cache behind, so the run lists for ever with
// nothing to send, and pressing send was the only way to find out. Another 202
// came from the player's own recordings in momtv\local, which the game can
// already see -- "send" is not a thing those need.
//
// Rebuilt when the run store changes, which is a map load. Four stats per run,
// two dozen runs: once, not forty file-system round trips a frame.
static unsigned char s_runSrc[WR_MAX_RUNS];
static unsigned int s_runSrcGen = 0xFFFFFFFFu;
static int s_runSrcMapId = -1;
static int s_runSrcCount = -1;

static void RefreshRunSources(const char *map, int mapId)
{
    unsigned int gen = WrRunStoreGeneration();
    int count = WrRunCount();
    // The COUNT as well as the generation, and that is not belt and braces. A
    // map's runs stream in a few files per frame and the generation is bumped
    // once at the start and once when the last one lands -- so keying on the
    // generation alone latches the answer after the first handful of runs and
    // every run that arrives afterwards keeps the PREVIOUS map's classification
    // until loading finishes. On a busy map that is a couple of hundred frames
    // of rows saying "no demo" about runs that have one.
    if (gen == s_runSrcGen && mapId == s_runSrcMapId && count == s_runSrcCount)
        return;
    s_runSrcGen = gen;
    s_runSrcMapId = mapId;
    s_runSrcCount = count;

    int n = WrRunCount();
    for (int i = 0; i < n && i < WR_MAX_RUNS; i++)
    {
        const WrRun *r = WrRunAt(i);
        s_runSrc[i] = r ? (unsigned char)WrIntoGameSourceOf(
                              r->map[0] ? r->map : map, mapId, r->srcSha1,
                              NULL, 0)
                        : (unsigned char)WR_DEMO_NONE;
    }
}

static void WrSendFromRow(WrIntoGameWhere where, const char *map, int mapId,
                          const char *hash, const char *who, int tab)
{
    char detail[384];
    WrIntoGameResult r = WrIntoGameSendTo(where, map, mapId, hash, detail,
                                          sizeof(detail));
    s_sendTab = tab;
    strncpy_s(s_sendWho, sizeof(s_sendWho), (who && *who) ? who : "that run",
              _TRUNCATE);
    strncpy_s(s_sendText, sizeof(s_sendText), detail, _TRUNCATE);
    s_sendBad = (r != WR_SEND_OK && r != WR_SEND_ALREADY &&
                 r != WR_SEND_ALREADY_LOCAL);
}

// Put the console command that plays this demo on the clipboard.
//
// This is the mechanism that does not depend on the game's lists agreeing that a
// demo exists, and it is what Momentum's own end-of-run screen uses. It needs
// the file to be somewhere the game's filesystem can see, so a run that has not
// been copied in yet is copied first -- into the LOCAL tree, recorded in the
// manifest and removable exactly like every other copy, and only ever because
// this button was pressed.
static void WrWatchCmdFromRow(const char *map, int mapId, const char *hash,
                              const char *who, int tab)
{
    s_sendTab = tab;
    strncpy_s(s_sendWho, sizeof(s_sendWho), (who && *who) ? who : "that run",
              _TRUNCATE);

    // Already somewhere the game's filesystem can name? Then nothing is copied
    // -- which covers a run that was sent to Downloaded, AND the player's own
    // recordings, whose names are not hashes at all and which are already in the
    // local tree under a name only the game chose.
    char cmd[MAX_PATH + 64];
    if (!WrIntoGameWatchCommand(map, mapId, hash, cmd, sizeof(cmd)))
    {
        // Not there yet. One copy into the local tree, recorded and removable,
        // and only because this button was pressed.
        char detail[384];
        WrIntoGameResult r = WrIntoGameSendTo(WR_INTO_LOCAL, map, mapId, hash,
                                              detail, sizeof(detail));
        if (r != WR_SEND_OK && r != WR_SEND_ALREADY &&
            r != WR_SEND_ALREADY_LOCAL)
        {
            strncpy_s(s_sendText, sizeof(s_sendText), detail, _TRUNCATE);
            s_sendBad = true;
            return;
        }
        if (!WrIntoGameWatchCommand(map, mapId, hash, cmd, sizeof(cmd)))
        {
            strncpy_s(s_sendText, sizeof(s_sendText),
                      "the copy went in but the game has no path for it -- "
                      "please report this, it should not happen", _TRUNCATE);
            s_sendBad = true;
            return;
        }
    }

    ImGui::SetClipboardText(cmd);
    _snprintf_s(s_sendText, sizeof(s_sendText), _TRUNCATE,
                "copied to the clipboard -- paste it in the console:  %s", cmd);
    s_sendBad = false;
}

#define WR_SEND_TAB_RUNS  0
#define WR_SEND_TAB_BOARD 1
static void DrawIntoGameLine(const char *map, int mapId, int tab);
static void WrSendFromRow(WrIntoGameWhere where, const char *map, int mapId,
                          const char *hash, const char *who, int tab);
static void WrWatchCmdFromRow(const char *map, int mapId, const char *hash,
                              const char *who, int tab);
static void WrSendForget(void);

// Does this run match what was typed?
//
// Three fields, because all three are things you would search for and only one
// of them is on the row. The Steam name matters in particular: the name tags in
// the world show the CURRENT persona, so filtering only on the name recorded in
// the demo would fail to find the player you can see written on the line.
static bool RunMatchesFilter(const WrRun *r, const char *needle)
{
    if (!needle || !*needle)
        return true;
    if (StrIContains(r->player, needle))
        return true;
    const char *persona = WrSteamPersona(r->steamId);
    if (persona && *persona && StrIContains(persona, needle))
        return true;
    // WrTrackName hands back a pointer into one shared static buffer, so this
    // has to be the last thing done with it and nothing may call it in between.
    return StrIContains(WrTrackName(r), needle);
}

// One button per leg the store holds, with a count and a tick when all of that
// leg is already on.
//
// Grouped on (trackType, trackNum), the same pair ComputeRanks places within, so
// "bonus 4" here means exactly what "bonus 4" means everywhere else in the tool.
static void DrawTrackChips(void)
{
    struct Group { unsigned char type, num; int total, on; };
    Group g[64];
    int nGroups = 0;

    for (int i = 0; i < WrRunCount(); i++)
    {
        const WrRun *r = WrRunAt(i);
        if (!r || r->pointCount < 2)
            continue;
        int k = 0;
        for (; k < nGroups; k++)
            if (g[k].type == r->trackType && g[k].num == r->trackNum)
                break;
        if (k == nGroups)
        {
            if (nGroups >= (int)ARRAYSIZE(g))
                continue;       // more than 64 distinct legs is not a real map
            g[nGroups].type = r->trackType;
            g[nGroups].num = r->trackNum;
            g[nGroups].total = 0;
            g[nGroups].on = 0;
            nGroups++;
        }
        g[k].total++;
        if (r->enabled)
            g[k].on++;
    }

    if (nGroups < 2)
        return;             // one leg, so a group button is the All button

    // Main first, then stages in order, then bonuses. Insertion sort: this runs
    // once a frame over at most a few dozen entries.
    for (int i = 1; i < nGroups; i++)
    {
        Group key = g[i];
        int j = i - 1;
        while (j >= 0 && (g[j].type > key.type ||
                          (g[j].type == key.type && g[j].num > key.num)))
        {
            g[j + 1] = g[j];
            j--;
        }
        g[j + 1] = key;
    }

    ImGui::TextUnformatted("Turn on a whole leg:");
    ImGui::SameLine();
    HelpMarker(
        "Every leg the loaded runs cover, with how many of it are on. Pressing "
        "one turns that leg on; pressing it again when all of it is already on "
        "turns it off. They add up, so two legs is two presses.\n\n"
        "Momentum records a separate run per stage and per bonus, so on a "
        "staged map most of what you have is not the full-map track. That is "
        "why these are worth having: \"bonus 4\" is otherwise sixteen "
        "individual checkboxes.");

    float avail = ImGui::GetContentRegionAvail().x;
    float x = 0.0f;
    for (int k = 0; k < nGroups; k++)
    {
        char label[48];
        // WrTrackName's static buffer again: copied into label immediately, and
        // the only call in this expression.
        const WrRun *any = NULL;
        for (int i = 0; i < WrRunCount() && !any; i++)
        {
            const WrRun *r = WrRunAt(i);
            if (r && r->trackType == g[k].type && r->trackNum == g[k].num)
                any = r;
        }
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%s %d/%d",
                    any ? WrTrackName(any) : "?", g[k].on, g[k].total);

        float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (k > 0 && x + w < avail)
            ImGui::SameLine();
        else
            x = 0.0f;
        x += w + ImGui::GetStyle().ItemSpacing.x;

        bool allOn = (g[k].on == g[k].total);
        if (allOn)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushID(k);
        if (ImGui::Button(label))
        {
            for (int i = 0; i < WrRunCount(); i++)
            {
                WrRun *r = WrRunAt(i);
                if (r && r->trackType == g[k].type && r->trackNum == g[k].num)
                    r->enabled = !allOn;
            }
        }
        ImGui::PopID();
        if (allOn)
            ImGui::PopStyleColor();
    }
}

static void DrawRunsTab(void)
{
    const char *map = WrLevelName();
    bool inMap = (map && *map);
    bool overridden = (WrMapOverride()[0] != '\0');

    if (!inMap)
    {
        ImGui::TextDisabled("No map detected.");
        ImGui::TextWrapped("Load a map, or pick one below.");
    }
    else
    {
        ImGui::Text("Map: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", map);
        ImGui::SameLine();
        if (overridden)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(manual)");
        ImGui::SameLine();
        ImGui::TextDisabled("|  %d run%s cached  |  %d drawn",
                            WrRunCount(), WrRunCount() == 1 ? "" : "s",
                            WrRunEnabledCount());
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Reload"))
        WrPathLoadMap(map);

    // Manual picker. Auto-detection reads the map out of demoheader.tmp, which
    // is reliable, but never leave the user with no way to get at their data if
    // it ever is not.
    ImGui::SameLine();
    if (ImGui::SmallButton("Pick map..."))
    {
        WrScanAvailableMaps();
        ImGui::OpenPopup("pickmap");
    }
    if (ImGui::BeginPopup("pickmap"))
    {
        ImGui::TextDisabled("maps with extracted paths");
        ImGui::Separator();
        // The list runs to 256 entries once a library has been built up, which
        // is more than a popup can usefully scroll through.
        static char pickFilter[64] = "";
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##pickfilter", "filter", pickFilter,
                                 sizeof(pickFilter));
        if (WrAvailableMapCount() == 0)
            ImGui::TextDisabled("(none yet -- press \"Extract new demos\")");
        int matched = 0;
        for (int i = 0; i < WrAvailableMapCount(); i++)
        {
            if (!StrIContains(WrAvailableMapAt(i), pickFilter))
                continue;
            matched++;
            char label[128];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "%s  (%d)",
                        WrAvailableMapAt(i), WrAvailableMapRuns(i));
            if (ImGui::Selectable(label))
            {
                WrSetMapOverride(WrAvailableMapAt(i));
                WrPathLoadMap(WrLevelName());
                ImGui::CloseCurrentPopup();
            }
        }
        if (matched == 0 && WrAvailableMapCount() > 0)
            ImGui::TextDisabled("(nothing matches \"%s\")", pickFilter);
        if (overridden)
        {
            ImGui::Separator();
            if (ImGui::Selectable("Back to auto-detect"))
            {
                WrSetMapOverride("");
                WrPathLoadMap(WrLevelName());
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // Say plainly what kind of library this map has. On a staged map the run
    // list is dominated by single-leg runs that are nowhere near you, and
    // without this the only symptom is "the lines are missing".
    int nMain = 0, nStage = 0, nBonus = 0, nNear = 0;
    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *r = WrRunAt(i);
        if (!r) continue;
        if (r->trackType == 0) nMain++;
        else if (r->trackType == 1) nStage++;
        else nBonus++;
        if (r->nearestDist >= 0.0f && r->nearestDist <= s_nearRadius) nNear++;
    }

    if (WrRunCount() > 0)
    {
        ImGui::Separator();
        ImGui::Text("%d full-map", nMain);
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); ImGui::Text("%d stage", nStage);
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); ImGui::Text("%d bonus", nBonus);
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextColored(nNear ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f)
                                 : ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%d near you", nNear);

        if (nStage + nBonus > nMain && nMain <= 2)
        {
            ImGui::TextWrapped(
                "This map's demos are mostly individual stages, which start in "
                "different places across the map. The fastest runs overall are "
                "usually different stages, so use \"near me\" to get the ones "
                "covering where you are.");
        }
    }

    // --- extraction ---------------------------------------------------------
    //
    // Above the "no paths for this map" message, and not inside it. This panel
    // used to sit below an early return taken whenever the map had no runs
    // loaded -- which is the one case where you most need it, and it left the
    // only route into extraction being the command line the message printed.
    ImGui::Separator();
    if (inMap)
    {
        int loadDone = 0, loadTotal = 0;
        if (WrPathLoading(&loadDone, &loadTotal))
        {
            ImGui::Text("loading runs %d / %d", loadDone, loadTotal);
            ImGui::SameLine();
            HelpMarker("Spread over frames on purpose. Reading a .wrpath costs a "
                       "few milliseconds, and doing 125 of them in one go inside "
                       "Present would stall the game for most of a second every "
                       "time you loaded a map.");
        }

        int total = 0, done = 0, fresh = 0, bad = 0;
        bool haveCounts = WrExtractCounts(&total, &done, &fresh, &bad);
        bool running = WrExtractRunning();

        if (running)
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "extracting...");
        else if (!haveCounts)
            ImGui::TextDisabled("counting demos for this map...");
        else if (total == 0)
            ImGui::TextDisabled("no demos downloaded for this map");
        else if (fresh > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "%d demo%s for this map, %d extracted, %d new",
                               total, total == 1 ? "" : "s", done, fresh);
        else
            ImGui::Text("%d demo%s for this map, %d extracted", total,
                        total == 1 ? "" : "s", done);

        // Demos that have already been tried and could not be read. Shown
        // separately because they are not work waiting to be done -- they are
        // work already done, that failed, and repeating it costs the same
        // minutes it cost the first time.
        if (haveCounts && bad > 0)
        {
            ImGui::TextDisabled("%d could not be extracted and are being "
                                "skipped", bad);
            ImGui::SameLine();
            HelpMarker("Some demos cannot be read. The extractor finds the path "
                       "by locating the player's origin stream in an "
                       "undocumented netstream and confirming it against the "
                       "run's own recorded top speed; when a map's stream is too "
                       "fragmented for that, it says so rather than writing a "
                       "plausible-looking line that is wrong.\n\n"
                       "Which ones failed, and why, is recorded in "
                       "_failed.txt next to that map's paths, so the same "
                       "minutes are not spent reaching the same answer every "
                       "time. Improving the extractor clears the record "
                       "automatically.\n\n"
                       "A few can be ruled out without trying: a demo whose "
                       "header does not make sense -- a tick interval that is "
                       "not one, a player ID that is not one -- has had its "
                       "layout moved and is counted here straight away.");
        }

        if (running)
            ImGui::BeginDisabled();
        if (ImGui::Button(fresh > 0 ? "Extract new demos" : "Re-run extractor"))
            WrExtractRun(false);
        ImGui::SameLine();
        HelpMarker("Reads the demos and writes the lines, inside the DLL, on a "
                   "pool of background threads -- below-normal priority and "
                   "marked as background work, so on a modern CPU they are put "
                   "on the efficiency cores and leave the game alone. It only "
                   "processes demos that have no path file yet, so pressing it "
                   "again costs seconds.\n\n"
                   "Nothing is installed and nothing is launched. It is never "
                   "run automatically -- only when you press this.");
        if (bad > 0)
        {
            ImGui::SameLine();
            if (ImGui::Button("Retry the failures"))
                WrExtractRun(true);
            ImGui::SameLine();
            HelpMarker("Tries the recorded failures again. Worth doing after a "
                       "game update, and not otherwise -- nothing about them "
                       "changes on its own, so this is the slow path by "
                       "definition.");
        }
        if (running)
            ImGui::EndDisabled();

        // OUTSIDE the disabled pair above, which is the whole point: while a
        // run was in flight this section had no live control at all.
        if (running)
        {
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                WrExtractStop();
            ImGui::SameLine();
            HelpMarker(
                "Stops whatever is running -- an extraction, a leaderboard "
                "fetch, a download. There is one slot, so this is it whichever "
                "tab started it.\n\n"
                "It is not instant, and it says so rather than pretending. Each "
                "worker has to reach its next checkpoint before it can look at "
                "the flag, which on a large demo can be a second or two. The "
                "alternative is killing a thread inside the game, which would "
                "leave a lock held and hang the game on its next allocation.\n\n"
                "Nothing is lost. Every file already written is complete -- "
                "they are written to a temporary name and moved into place -- "
                "and the record of which demos failed is saved as they fail, "
                "so a stop does not make you pay those timeouts again.");
        }

        ImGui::SetNextItemWidth(180.0f);
        int tmo = WrExtractTimeout();
        if (ImGui::SliderInt("Give up on a demo after", &tmo, 0, 300,
                             tmo > 0 ? "%d s" : "no limit"))
            WrExtractSetTimeout(tmo);
        ImGui::SameLine();
        HelpMarker(
            "Measured across 4388 demos here: the median is 58 KB and extracts "
            "in about a second, and the slowest normal one took seven. But the "
            "tail is enormous -- the 99th percentile is 5.8 MB and the largest "
            "is 47 MB -- and 6.5% are over 700 KB, which is the size that "
            "actually hit the old three-minute limit.\n\n"
            "So 30 seconds is four times the slowest normal extraction and "
            "turns a pathological demo from three minutes of apparent hang "
            "into half a minute. A demo that runs out is recorded as an "
            "ordinary failure and skipped next time, so it is never paid for "
            "twice.\n\n"
            "0 means no limit, and then only Stop will end a bad one.");

        int nLines = WrExtractLineCount();
        if (nLines > 0)
        {
            if (ImGui::BeginChild("extractlog", ImVec2(0.0f, 120.0f),
                                  ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_HorizontalScrollbar))
            {
                for (int i = 0; i < nLines; i++)
                    ImGui::TextUnformatted(WrExtractLine(i));
                if (running)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }
        if (!WrExtractRunning() && !haveCounts)
            ImGui::TextDisabled("%s", WrExtractStatus());
    }

    if (WrRunCount() == 0)
    {
        ImGui::Separator();
        if (!inMap)
        {
            ImGui::TextDisabled("Load a map, or pick one above.");
            return;
        }
        // There was a "Copy command" box here, offering a python command line
        // to run in a terminal instead. There is no script to run any more, and
        // the button above is now the only way there ever was.
        ImGui::TextWrapped(
            "No cached paths for this map yet. The button above reads them out "
            "of the demos the game has already downloaded -- and the Board tab "
            "will fetch more.");
        return;
    }

    ImGui::Separator();

    // The filtered set, built once and shared by the bulk buttons, the track
    // chips and the table below.
    //
    // It used to live inside the table, which meant "All" turned on every run in
    // the store while the list in front of you showed six. Board's tick-all has
    // always worked over its filtered order for exactly this reason; this brings
    // the two into line.
    static int order[WR_MAX_RUNS];
    int shown = 0;
    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *r = WrRunAt(i);
        if (!r)
            continue;
        if (s_nearOnly && !(r->nearestDist >= 0.0f &&
                            r->nearestDist <= s_nearRadius))
            continue;
        if (s_runFilter[0] && !RunMatchesFilter(r, s_runFilter))
            continue;
        order[shown++] = i;
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##runfilter", "find a player, or a track",
                             s_runFilter, sizeof(s_runFilter));
    ImGui::SameLine();
    if (ImGui::Button("Clear##runfilter"))
        s_runFilter[0] = '\0';
    ImGui::SameLine();
    HelpMarker(
        "Matches the player's name, their current Steam name if it is known, "
        "and the track -- so \"bonus 4\" narrows the list to that leg and "
        "\"stage\" to every stage run.\n\n"
        "Everything below works on what the filter leaves: All, None, and the "
        "track buttons. Filter to a name, press All, and you are watching that "
        "player's lines and nobody else's.");

    // One button per leg the store actually holds.
    //
    // Ticking sixteen bonus-4 runs by hand was the reported problem, and the
    // information needed to fix it was already there -- ComputeRanks groups on
    // exactly this pair. A button turns its whole group on, or off again if all
    // of it is already on, so two legs at once is two clicks rather than a fight
    // with the exclusive helpers below.
    DrawTrackChips();

    // Staged maps are the normal case, and they break the obvious behaviour.
    // Momentum records a separate demo per stage, so on surf_tensor2 only 2 of
    // 32 downloaded runs are full-map runs -- the rest are individual stages and
    // bonuses, starting in ten different places across the map. Enabling the
    // three fastest runs there gives you three different stages, none of them
    // where you are standing, and it looks exactly like the lines are broken.
    //
    // So the default action is "best runs for where I am", not "best runs".
    if (ImGui::Button("Best 3 near me"))
        WrEnableBestNearby(3, s_nearRadius);
    ImGui::SameLine();
    if (ImGui::Button("Best near me"))
        WrEnableBestNearby(1, s_nearRadius);
    ImGui::SameLine();
    if (ImGui::Button("Fastest 3 anywhere"))
    {
        for (int i = 0; i < WrRunCount(); i++)
        {
            WrRun *r = WrRunAt(i);
            if (r) r->enabled = (i < 3);
        }
    }
    ImGui::SameLine();
    // All and None act on what the filter left, not on the store. Anything else
    // means a button that says "All" changes runs you cannot see.
    if (ImGui::Button("All"))
    {
        for (int k = 0; k < shown; k++)
        {
            WrRun *r = WrRunAt(order[k]);
            if (r) r->enabled = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("None"))
    {
        for (int k = 0; k < shown; k++)
        {
            WrRun *r = WrRunAt(order[k]);
            if (r) r->enabled = false;
        }
    }
    if (shown != WrRunCount())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(of the %d shown)", shown);
    }

    ImGui::Checkbox("Only show runs near me", &s_nearOnly);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("within", &s_nearRadius, 512.0f, 16384.0f, "%.0f u");

    // Momentum's numeric map id, which is what names the replay directory. Not
    // derivable from the map name, so without the map index the Watch column has
    // nowhere to write and says so per row rather than failing on the press.
    int mapIdx = WrMapsFind(map);
    const WrMapInfo *mapInfo = (mapIdx >= 0) ? WrMapsAt(mapIdx) : NULL;
    int mapId = mapInfo ? mapInfo->id : 0;

    RefreshRunSources(map, mapId);
    DrawIntoGameLine(map, mapId, WR_SEND_TAB_RUNS);

    // The list takes whatever height is left, so dragging the window taller
    // shows more runs instead of a taller empty panel around a fixed 320-pixel
    // box. One line is held back for the "enabled but only N drawn" warning
    // underneath, which is the one thing that must not be pushed off the bottom.
    float reserve = ImGui::GetTextLineHeightWithSpacing() +
                    ImGui::GetStyle().ItemSpacing.y;
    float tableHeight = ImGui::GetContentRegionAvail().y - reserve;
    if (tableHeight < 140.0f)
        tableHeight = 140.0f;    // below this the header eats the whole list

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti;
    if (ImGui::BeginTable("runs", 10, flags, ImVec2(0.0f, tableHeight)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Sorting is on the columns where an order means something. "On" and
        // "Col" are per-row controls, not values, so they are left alone.
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Col", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Track", 0, 0.0f, RUNCOL_TRACK);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch,
                                0.0f, RUNCOL_PLAYER);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_DefaultSort, 0.0f,
                                RUNCOL_TIME);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Near", 0, 0.0f, RUNCOL_NEAR);
        ImGui::TableSetupColumn("Pts", 0, 0.0f, RUNCOL_POINTS);
        ImGui::TableSetupColumn("Splits", 0, 0.0f, RUNCOL_SPLITS);
        // Fixed and wide enough for the three buttons a row can carry, because a
        // table clips its cells: sized to the content, "watch" was drawn cut in
        // half and looked like a rendering fault rather than a narrow column.
        ImGui::TableSetupColumn("Watch", ImGuiTableColumnFlags_NoSort |
                                         ImGuiTableColumnFlags_WidthFixed,
                                168.0f);
        ImGui::TableHeadersRow();

        // The store itself stays sorted by time -- "max runs drawn" means the
        // fastest N, and the delta column looks up the best of each track by
        // taking the first match. So the click-to-sort order is a separate list
        // of indices used only for display, filtered above and sorted here,
        // because the sort specs only exist inside the table.
        SortRunOrder(order, shown, ImGui::TableGetSortSpecs());

        // A clipper, for the same reason the leaderboard has one. The store now
        // holds up to a thousand runs, and every row here is nine columns of
        // real work: a checkbox, a colour picker, and a delta that scans the
        // store for the best run on that row's track. Doing that for a thousand
        // rows to show forty of them is per-frame cost inside a Present hook.
        //
        // Safe because every row is exactly one line tall -- the "!" beside a
        // low-confidence run is drawn with SameLine, not under it -- which is
        // the assumption a clipper makes.
        ImGuiListClipper clipper;
        clipper.Begin(shown);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            {
                int i = order[row];
                WrRun *r = WrRunAt(i);
                if (!r)
                    continue;
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("##on", &r->enabled);

                ImGui::TableSetColumnIndex(1);
                ImVec4 col = UnpackColour(r->colour);
                if (ImGui::ColorEdit4("##col", (float *)&col,
                                      ImGuiColorEditFlags_NoInputs |
                                      ImGuiColorEditFlags_NoLabel |
                                      ImGuiColorEditFlags_AlphaPreview))
                    r->colour = PackColour(col);

                ImGui::TableSetColumnIndex(2);
                if (r->trackType == 0)
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "main");
                else
                    ImGui::TextColored(ImVec4(0.85f, 0.8f, 0.55f, 1.0f), "%s",
                                       WrTrackName(r));

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(r->player[0] ? r->player : "(unknown)");
                if (r->flags & WRPATH_FLAG_LOW_CONFIDENCE)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "!");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "The extractor could not confirm this path against the\n"
                            "run's own recorded max speed. It is probably right, but\n"
                            "it is not proven.");
                }

                ImGui::TableSetColumnIndex(4);
                char buf[64];
                FormatTime(r->runTime, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableSetColumnIndex(5);
                WrRun *best = BestOfSameTrack(r);
                if (best && best != r)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.6f, 1.0f), "+%.3f",
                                       r->runTime - best->runTime);
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "best");

                // The column that makes a staged map make sense: how far away
                // this run actually is from where you are standing right now.
                ImGui::TableSetColumnIndex(6);
                if (r->nearestDist < 0.0f)
                    ImGui::TextDisabled("-");
                else if (r->nearestDist <= s_nearRadius)
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%.0f",
                                       r->nearestDist);
                else
                    ImGui::TextDisabled("%.0f", r->nearestDist);

                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%d", r->pointCount);

                ImGui::TableSetColumnIndex(8);
                if (r->markerCount && (r->flags & WRPATH_FLAG_MARKERS_OK))
                    ImGui::Text("%d", r->markerCount);
                else if (r->markerCount)
                    ImGui::TextDisabled("unanchored");
                else
                    ImGui::TextDisabled("-");

                // Put this one demo where the game's own replay viewer looks.
                // The viewer lists ten, so being able to choose which ten is
                // the whole point -- see wr_intogame.h.
                ImGui::TableSetColumnIndex(9);
                // The manifest and a cached per-map answer, never the disk on
                // this path: this runs per visible row per frame, and a
                // GetFileAttributes each would be forty file-system round trips
                // a frame -- translated ones, under Proton.
                unsigned char kind = (i >= 0 && i < WR_MAX_RUNS)
                                         ? s_runSrc[i]
                                         : (unsigned char)WR_DEMO_NONE;
                // The one button that works in every state where a demo exists
                // at all, and the only mechanism here that does not depend on
                // the game's lists agreeing that it does. Drawn first because it
                // is the answer -- see its tooltip.
                bool haveDemo = (kind != WR_DEMO_NONE);
                if (haveDemo)
                {
                    if (ImGui::SmallButton("watch"))
                        WrWatchCmdFromRow(r->map, mapId, r->srcSha1, r->player,
                                          WR_SEND_TAB_RUNS);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Copies the console command that PLAYS this demo:\n"
                            "  mom_tv_replay_watch \"momtv/local/<map>/<hash>.mtv\"\n"
                            "Paste it in the console and it plays, whatever the\n"
                            "Downloaded and Local tabs happen to show.\n\n"
                            "This is what the game's own end-of-run screen does.\n"
                            "It takes a PATH, so it needs no leaderboard row and\n"
                            "no list -- which is the whole problem with the two\n"
                            "buttons beside it. The Downloaded tab is built from\n"
                            "online leaderboard rows that have a cached file, not\n"
                            "from the folder, so a file dropped in there shows up\n"
                            "only if the game already listed that run.\n\n"
                            "If the demo is not somewhere the game can see yet,\n"
                            "one copy goes into the local folder first -- recorded\n"
                            "like every other, and \"take out\" removes it.");
                    ImGui::SameLine();
                }

                if (WrIntoGameMine(mapId, r->srcSha1))
                {
                    if (ImGui::SmallButton("take out"))
                    {
                        WrIntoGameRemoveOne(mapId, r->srcSha1);
                        // The answer to the send that put it there is now a
                        // claim about a file that has just been deleted.
                        WrSendForget();
                    }
                }
                else if (kind == WR_DEMO_NONE)
                {
                    // The demo this line came from is gone -- deleted in game,
                    // most likely -- and only the path cache is left. Say so
                    // instead of offering a button that cannot work.
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "no demo");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "The .mtv this line was extracted from is not on disk\n"
                            "any more, in our folder or either of the game's. The\n"
                            "line itself is unaffected -- it is read from the\n"
                            ".wrpath -- but there is nothing to send or to watch.\n\n"
                            "35 of the 1749 lines on this machine are in this state.");
                }
                else if (kind == WR_DEMO_GAME_ONLINE)
                {
                    // Already at the destination and not ours -- the game
                    // downloaded it. A send button here can only ever answer
                    // "already has that one", which is the "send did nothing"
                    // complaint restated. The state was worked out; say it.
                    ImGui::TextDisabled("in game");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Already in the game's downloaded folder, and not\n"
                            "one of ours -- the game fetched it itself. There is\n"
                            "nothing to send and nothing here may remove it.");
                }
                else if (kind == WR_DEMO_GAME_LOCAL)
                {
                    ImGui::TextDisabled("yours");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "One of your own recordings, already sitting in the\n"
                            "game's local replay folder. There is nowhere to send\n"
                            "it that it is not already.");
                }
                else
                {
                    // No numeric id is no longer a dead end: it only names the
                    // ONLINE directory, and the local tree is named by the map.
                    // So the send button goes and the local one stays.
                    if (mapId > 0)
                    {
                        if (ImGui::SmallButton("send"))
                            WrSendFromRow(WR_INTO_ONLINE, r->map, mapId,
                                          r->srcSha1, r->player,
                                          WR_SEND_TAB_RUNS);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "Copy it into the game's downloaded folder.\n\n"
                                "Now known to be the weaker of the two: that tab\n"
                                "lists online leaderboard rows that have a cached\n"
                                "file rather than reading the folder, so this only\n"
                                "lights up a run the game had already listed. Use\n"
                                "\"watch\" if the row does not appear.");
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("local"))
                        WrSendFromRow(WR_INTO_LOCAL, r->map, mapId, r->srcSha1,
                                      r->player, WR_SEND_TAB_RUNS);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Put this ONE demo in the game's local replay folder\n"
                            "-- the tab that lists the runs you recorded yourself.\n\n"
                            "It also puts the file somewhere the game's own\n"
                            "filesystem can name, which is what \"watch\" needs.\n\n"
                            "It is one press, it is recorded like everything else,\n"
                            "and \"take out\" removes it again.");
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (WrRunEnabledCount() > WR_MAX_RUNS_DRAWN)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%d enabled; the fastest %d of them are drawn.",
                           WrRunEnabledCount(), WR_MAX_RUNS_DRAWN);
}

// The count of what we have put in the game's replay folder, and the one button
// that takes it back out. Shared by the Runs and Board tabs, which both have a
// per-row "send".
static void DrawIntoGameLine(const char *map, int mapId, int tab)
{
    // Re-checked here rather than trusted: a game cache clear takes our copies
    // with everything else, and a number that still counted them would be wrong
    // about the only thing this feature is about. Handed the map so it can also
    // adopt copies the fetcher put there without telling anyone.
    WrIntoGameRefresh(map, mapId);
    int n = WrIntoGameCount();

    ImGui::Text("In the game's replay viewer:");
    ImGui::SameLine();
    if (n == 0)
        ImGui::TextDisabled("none of ours");
    else
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                           "%d of ours", n);
    ImGui::SameLine();
    HelpMarker(
        "\"Watch\" is the one to reach for. It copies the console command that "
        "PLAYS a demo -- mom_tv_replay_watch \"momtv/local/<map>/<hash>.mtv\" -- "
        "which is exactly what the game's own end-of-run screen runs, and it "
        "takes a PATH. No leaderboard row, no list, nothing to agree with.\n\n"
        "WHY THAT MATTERS, and this is now known rather than guessed. The "
        "game's Downloaded tab is not a listing of the folder. Its entries are "
        "online leaderboard rows that have a cached file -- the game's own UI "
        "calls that state ONLINE_CACHED -- so a demo copied into "
        "momentum\\momtv\\online\\<map id>\\ appears there only if the game had "
        "already listed that run. That is why \"the game already has that one\" "
        "and \"it is not in my list\" were both true at once.\n\n"
        "The filename does not matter, incidentally: the engine reads a "
        "replay's metadata out of the file, and says \"Invalid run metadata for "
        "replay file\" when it cannot.\n\n"
        "\"Send\" and \"local\" still copy the .mtv into the game's two replay "
        "trees. They are copies, so your line stays whatever happens to them.\n\n"
        "REMOVING ONLY EVER TOUCHES OURS. Every file sent is written into "
        "wrlines_data\\into_game.txt first, and nothing outside that list can be "
        "deleted from here. The demos the game downloaded by itself -- 4268 of "
        "them on the machine this was built on -- are not in that list and are "
        "not reachable. A copy already there is adopted into the list only if "
        "our own fetched .mtv of the same hash exists, which is what makes it "
        "ours rather than the game's.\n\n"
        "Nothing here runs anything. \"Watch\" puts a command on your clipboard "
        "for you to paste; WrLines executes no console command and sets no "
        "cvar, and copying a file is the most it ever does to the game.");
    if (n > 0)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove ours"))
        {
            WrIntoGameRemoveAll();
            WrSendForget();
            strncpy_s(s_sendText, sizeof(s_sendText), WrIntoGameStatus(),
                      _TRUNCATE);
            s_sendTab = tab;
        }
    }

    // The answer belongs to the run that asked for it, on the tab that asked.
    if (s_sendText[0] && s_sendTab == tab)
    {
        ImVec4 col = s_sendBad ? ImVec4(1.0f, 0.6f, 0.4f, 1.0f)
                               : ImVec4(0.6f, 0.9f, 1.0f, 1.0f);
        if (s_sendWho[0])
            ImGui::TextColored(col, "%s: %s", s_sendWho, s_sendText);
        else
            ImGui::TextColored(col, "%s", s_sendText);
        ImGui::SameLine();
        if (ImGui::SmallButton("ok##sendmsg"))
            s_sendText[0] = '\0';
    }
}

// One row of checkboxes choosing what a label says. The `on` flag stays separate
// from the content mask so that turning a site off and back on remembers what it
// was showing.
static void LabelPicker(const char *title, unsigned int *mask, bool *on)
{
    ImGui::PushID(title);
    ImGui::Checkbox(title, on);
    if (*on)
    {
        static const struct { unsigned int bit; const char *name; } kBits[4] = {
            { WR_LABEL_SPEED,  "speed" },
            { WR_LABEL_ENERGY, "energy" },
            { WR_LABEL_TIME,   "time" },
            { WR_LABEL_DELTA,  "vs you" },
        };
        ImGui::Indent();
        for (int i = 0; i < 4; i++)
        {
            bool set = (*mask & kBits[i].bit) != 0;
            if (i) ImGui::SameLine();
            if (ImGui::Checkbox(kBits[i].name, &set))
                *mask = set ? (*mask | kBits[i].bit) : (*mask & ~kBits[i].bit);
        }
        ImGui::Unindent();
    }
    ImGui::PopID();
}

// Two stacked panes sharing whatever height is left.
//
// The Runs tab has always sized its list this way; everywhere else was a fixed
// pixel height, so maximising the window drew the same 320-pixel table with
// most of a screen of nothing underneath it. Returns the height for the TOP
// pane, and the bottom one is then drawn with a height of 0, which ImGui reads
// as "take what is left" for both a child and a scrolling table.
//
// `reserve` is room for anything between or after them -- a SeparatorText, a
// caption -- and the two floors keep a short window usable rather than
// collapsing one pane to its header.
static float SplitHeight(float reserve, float frac, float topFloor,
                         float bottomFloor)
{
    float body = ImGui::GetContentRegionAvail().y - reserve;
    float top = body * frac;
    if (top > body - bottomFloor)
        top = body - bottomFloor;
    if (top < topFloor)
        top = topFloor;
    return top;
}

// One pane taking the rest, with a floor. Used where the parent may already be
// scrolling, in which case what is "left" can be nothing at all and a bare
// fill would collapse to a sliver.
static float FillHeight(float reserve, float floorH)
{
    float h = ImGui::GetContentRegionAvail().y - reserve;
    return h < floorH ? floorH : h;
}

// The vertical space one SeparatorText costs, near enough to reserve for.
static float SeparatorHeight(void)
{
    return ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
}

// How tall the Python output box is, in the tabs that have one.
//
// FIXED, and the list above it takes everything else. It used to be the other
// way round -- SplitHeight gave the list a fraction and the log soaked up the
// remainder -- so making the window taller grew the log and the list lost rows
// off the bottom. Reported as the two fighting each other, which is exactly
// what a proportional split between a list and a log is. A log does not get
// more useful with height; a list of players does.
#define WR_OUTPUT_HEIGHT 132.0f

// The list pane above a fixed-height output box.
static float ListHeightAbove(bool showingOutput)
{
    if (!showingOutput)
        return 0.0f;            // ImGui reads 0 as "take what is left"
    return FillHeight(WR_OUTPUT_HEIGHT + SeparatorHeight(), 140.0f);
}

// Every map Momentum knows about, what we hold for it, and a way to get more.
//
// The listing costs nothing: the game caches the whole catalogue on disk and
// wr_maps.h reads an index made from it. Fetching is the one part that reaches
// outside this machine, and it is off until you turn it on.
static bool g_fetchEnabled = false;
static bool g_intoGame = false;         // also put downloads where the game looks
static int g_fetchTop = 25;
static int g_fetchTrackType = 0;
static int g_fetchTrackNum = 1;
static char g_mapFilter[64] = {0};

// Defined with the rest of the Board tab, below. Hands it a map and asks it to
// come to the front, which is what the per-row "board" button does.
static void BoardShow(const char *map, int trackType, int trackNum);

// The Maps table used to pass ImGuiTableFlags_Sortable and implement nothing:
// no column carried a user id and TableGetSortSpecs was never called, so
// clicking a header drew the arrow and changed nothing at all. These are the
// ids that make it real.
enum
{
    MAPCOL_NAME = 1,
    MAPCOL_TIER,
    MAPCOL_DEMOS,
    MAPCOL_LINES,
};

static const ImGuiTableSortSpecs *g_mapSpecs = NULL;

static int CompareMapColumn(const WrMapInfo *a, const WrMapInfo *b, ImGuiID col)
{
    switch (col)
    {
    case MAPCOL_NAME:
        return _stricmp(a->name, b->name);
    case MAPCOL_TIER:
        // Unknown tier sorts last either way rather than pretending to be a 0.
        {
            int ta = a->tier > 0 ? a->tier : 999;
            int tb = b->tier > 0 ? b->tier : 999;
            return ta == tb ? 0 : (ta < tb ? -1 : 1);
        }
    case MAPCOL_DEMOS:
        return a->demos == b->demos ? 0 : (a->demos < b->demos ? -1 : 1);
    case MAPCOL_LINES:
        return a->extracted == b->extracted ? 0
                                            : (a->extracted < b->extracted ? -1 : 1);
    default:
        return 0;
    }
}

static int __cdecl CompareMapRows(const void *pa, const void *pb)
{
    const WrMapInfo *a = WrMapsAt(*(const int *)pa);
    const WrMapInfo *b = WrMapsAt(*(const int *)pb);
    if (!a || !b || !g_mapSpecs)
        return 0;
    for (int s = 0; s < g_mapSpecs->SpecsCount; s++)
    {
        const ImGuiTableColumnSortSpecs *spec = &g_mapSpecs->Specs[s];
        int c = CompareMapColumn(a, b, spec->ColumnUserID);
        if (c != 0)
            return spec->SortDirection == ImGuiSortDirection_Ascending ? c : -c;
    }
    return _stricmp(a->name, b->name);      // ties never wobble
}

static void DrawMapsTab(void)
{
    if (!WrMapsReady())
    {
        WrMapsRefresh();
        ImGui::TextDisabled("Reading...");
        return;
    }

    ImGui::TextWrapped(
        "Every map Momentum knows about, from the catalogue the game already "
        "keeps on disk. Listing this asks nothing of anybody's server.");
    ImGui::TextDisabled("%s", WrMapsStatus());

    if (ImGui::Button("Rebuild the index"))
    {
        WrExtractRequest req = {WR_JOB_INDEX_MAPS};
        WrExtractSubmit(&req);
    }
    ImGui::SameLine();
    if (ImGui::Button("Recount what is on disk"))
        WrMapsRefresh();
    ImGui::SameLine();
    HelpMarker("The index comes from momentum\\_cache, which the game writes "
               "when it fetches the map list. Rebuilding reads that file and "
               "nothing else -- no network, and no Python: this one is done "
               "inside the DLL. If a map is missing, open the map selector in "
               "game once so the game refreshes its own copy.");

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", "filter by name", g_mapFilter,
                             sizeof(g_mapFilter));

    // Two thousand rows, and the one you are standing in is somewhere among
    // them. Typing its name is the thing you already know, which is the sign it
    // should be a button.
    //
    // Truncated into a buffer the same size as the filter rather than copied
    // blind: g_levelName holds 128 characters and this box holds 64, so a long
    // map name has to be cut somewhere, and cutting it HERE keeps the compare
    // below honest -- the lit state then means "the box holds what the button
    // would write", which is what makes pressing it again the way out.
    char wantFilter[64] = "";
    const char *standingIn = WrLevelName();
    if (standingIn && *standingIn)
        strncpy_s(wantFilter, sizeof(wantFilter), standingIn, _TRUNCATE);
    bool filteredHere = wantFilter[0] &&
                        _stricmp(g_mapFilter, wantFilter) == 0;

    ImGui::SameLine();
    if (!wantFilter[0])
        ImGui::BeginDisabled();
    if (filteredHere)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("This map"))
    {
        if (filteredHere)
            g_mapFilter[0] = '\0';
        else
            strcpy_s(g_mapFilter, sizeof(g_mapFilter), wantFilter);
    }
    if (filteredHere)
        ImGui::PopStyleColor();
    if (!wantFilter[0])
        ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%d maps", WrMapsCount());
    ImGui::SameLine();
    HelpMarker("Two thousand maps and you have demos for a few hundred of "
               "them, so the ones you hold nothing for are hidden until you "
               "type a name. That is why a map you are looking for can appear "
               "with a 0 beside it: the 0 is what is on your disk, not what "
               "exists.\n\n"
               "\"This map\" fills the box with the map you are standing in, "
               "and pressing it again empties it. It works even on a map you "
               "hold nothing for -- an empty filter hides the zeroes, so the "
               "row you want is one of the ones being hidden, and typing its "
               "name is exactly what brings it back.");

    // --- fetching -----------------------------------------------------------
    ImGui::SeparatorText("Download demos");
    ImGui::Checkbox("Allow downloading", &g_fetchEnabled);
    ImGui::SameLine();
    HelpMarker(
        "The second thing WrLines does that reaches outside your machine, and "
        "like the Steam name lookup it is a checkbox you control.\n\n"
        "It asks Momentum's public leaderboard for a map's top runs and "
        "downloads only the ones you do not already have. That last part is "
        "exact rather than approximate: a run's replay hash IS the demo's "
        "filename, so asking for the top fifty of a map you have forty-nine of "
        "downloads one file.\n\n"
        "One request at a time, a pause between them, and only when you press "
        "the button -- never on a map change, never in the background. Demos "
        "land in wrlines_data\\demos, not in the game install, which stays "
        "untouched.\n\n"
        "The endpoint needs no account and no token. Momentum's terms say "
        "nothing about automated access either way, so the pacing here is "
        "manners rather than a rule: this is free community infrastructure.");

    if (g_fetchEnabled)
    {
        ImGui::Checkbox("Also put them where the game can play them",
                        &g_intoGame);
        ImGui::SameLine();
        HelpMarker(
            "OFF BY DEFAULT, and the only thing WrLines ever writes into the "
            "game install.\n\n"
            "Downloads normally land in wrlines_data\\demos, which the game "
            "knows nothing about -- so you can draw them as lines but not watch "
            "them. With this on they are ALSO copied into "
            "momentum\\momtv\\online\\<map id>, which is the game's own replay "
            "folder, under the game's own filename: the replay hash, which is "
            "what our copies are already named. There is no index file beside "
            "them, so the game finds replays by scanning that folder.\n\n"
            "A copy, not a move. Your own tree keeps its copy, so if the game "
            "ever clears its cache your lines survive.\n\n"
            "Honest limit: this puts the file exactly where the game keeps its "
            "own downloads, but whether the in-game replay menu LISTS it or "
            "only finds it already downloaded when you pick that run from the "
            "leaderboard is not something this side can check. Worth trying "
            "once and seeing.");
        ImGui::SliderInt("Leaderboard places", &g_fetchTop, 5, 200);
        ImGui::SetNextItemWidth(120.0f);
        const char *kTracks[3] = { "main", "stage", "bonus" };
        ImGui::Combo("Track", &g_fetchTrackType, kTracks, 3);
        if (g_fetchTrackType != 0)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            ImGui::InputInt("number", &g_fetchTrackNum);
            if (g_fetchTrackNum < 1) g_fetchTrackNum = 1;
        }
    }

    // --- the list -----------------------------------------------------------
    //
    // Both counts are of THIS MACHINE. Neither is the leaderboard's, and the
    // difference was not obvious from a column head reading "demos": Momentum
    // holds thousands of runs per map and this table can show a 0 beside one of
    // them. Saying so costs two lines and stops the table reading as a claim
    // about what exists.
    ImGui::TextDisabled("demos = .mtv files you hold    lines = .wrpath files "
                        "extracted from them, which is what draws");
    ImGui::TextDisabled("Neither is the server's count -- that costs a request "
                        "per map, so it is on the browse button.");

    // The table and the output share what is left of the window, rather than
    // both being a fixed box with the rest of a maximised panel wasted under
    // them. Browsing a leaderboard 200 places deep is the case that wants the
    // room, so the map list takes every pixel the output box does not need.
    bool showOut = WrExtractRunning() || WrExtractLineCount() > 0;
    float tableH = ListHeightAbove(showOut);

    const char *here = standingIn;      // the green row, same map as the button
    if (ImGui::BeginTable("##maps", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                          ImVec2(0.0f, tableH)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("map", ImGuiTableColumnFlags_WidthStretch |
                                       ImGuiTableColumnFlags_DefaultSort,
                                0.0f, MAPCOL_NAME);
        ImGui::TableSetupColumn("tier", ImGuiTableColumnFlags_WidthFixed,
                                40.0f, MAPCOL_TIER);
        ImGui::TableSetupColumn("demos", ImGuiTableColumnFlags_WidthFixed,
                                55.0f, MAPCOL_DEMOS);
        ImGui::TableSetupColumn("lines", ImGuiTableColumnFlags_WidthFixed,
                                55.0f, MAPCOL_LINES);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_NoSort, 230.0f);
        ImGui::TableHeadersRow();

        // Filter first, then sort the indices that survived -- the store keeps
        // the order wr_maps read it in, exactly as the Runs tab leaves its own
        // store alone.
        static int order[4096];
        int shown = 0;
        for (int i = 0; i < WrMapsCount() && shown < 4096; i++)
        {
            const WrMapInfo *m = WrMapsAt(i);
            if (!m)
                continue;
            if (g_mapFilter[0] && !StrIContains(m->name, g_mapFilter))
                continue;
            // Anything with nothing on disk and no filter typed would be 2000
            // rows of zeroes, so those only appear once you look for them.
            if (!g_mapFilter[0] && m->demos == 0)
                continue;
            order[shown++] = i;
        }

        ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs();
        if (specs && specs->SpecsCount > 0 && shown > 1)
        {
            g_mapSpecs = specs;
            qsort(order, (size_t)shown, sizeof(int), CompareMapRows);
            g_mapSpecs = NULL;
        }

        for (int k = 0; k < shown; k++)
        {
            int i = order[k];
            const WrMapInfo *m = WrMapsAt(i);
            if (!m)
                continue;

            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableNextColumn();
            bool isHere = (here && _stricmp(here, m->name) == 0);
            if (isHere)
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", m->name);
            else
                ImGui::TextUnformatted(m->name);

            ImGui::TableNextColumn();
            if (m->tier > 0) ImGui::Text("%d", m->tier);
            else             ImGui::TextDisabled("-");

            ImGui::TableNextColumn();
            ImGui::Text("%d", m->demos);

            ImGui::TableNextColumn();
            if (m->extracted < m->demos)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%d",
                                   m->extracted);
            else
                ImGui::Text("%d", m->extracted);

            ImGui::TableNextColumn();
            // Always offered, because it costs nothing: it only opens the
            // board tab on this map, and whether anything is fetched there is
            // a separate decision behind the same toggle.
            if (ImGui::SmallButton("board"))
                BoardShow(m->name, g_fetchTrackType, g_fetchTrackNum);
            if (g_fetchEnabled && !WrExtractRunning())
            {
                WrExtractRequest req = {WR_JOB_FETCH};
                strncpy_s(req.map, sizeof(req.map), m->name, _TRUNCATE);
                req.mapId = m->id;
                req.top = g_fetchTop;
                req.trackType = g_fetchTrackType;
                req.trackNum = g_fetchTrackNum;
                req.intoGame = g_intoGame;

                // Browse first, download second. One request, nothing written,
                // and it prints the leaderboard's own total -- which is the
                // only place that number can come from.
                ImGui::SameLine();
                if (ImGui::SmallButton("browse"))
                {
                    req.dryRun = true;
                    WrExtractSubmit(&req);
                    req.dryRun = false;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("download"))
                    WrExtractSubmit(&req);
            }
            else if (isHere && m->extracted < m->demos)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("extract in Runs");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (showOut)
    {
        ImGui::SeparatorText("Output");
        // Zero height: the last pane takes whatever the table did not. Also a
        // horizontal scrollbar, because a leaderboard line is wider than a
        // narrow panel and wrapping a fixed-column listing makes it unreadable.
        if (ImGui::BeginChild("##fetchout", ImVec2(0.0f, WR_OUTPUT_HEIGHT),
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            // Hoisted, as the Runs pane does it. As the loop condition this
            // took the extractor's lock once per line per frame and could see
            // the ring shift underneath it half way down.
            int nLines = WrExtractLineCount();
            for (int i = 0; i < nLines; i++)
                ImGui::TextUnformatted(WrExtractLine(i));
            if (WrExtractRunning())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
}

// Set the energy colour range to what the enabled runs actually reach.
//
// Scanned on demand rather than kept on each run, for the reason wr_path.cpp
// gives for not caching per-point energy at all: E = z + |v|^2/2g moves with the
// gravity setting, so a stored range would go quietly stale the moment that
// slider moved and the colours would stop matching their own key.
//
// Strided. A thousand points per run is far more than enough to find the ends of
// a range that is then rounded to the nearest fifty anyway, and it keeps a press
// of this button off the frame budget even with a full store enabled.
static void FitEnergyRange(void)
{
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < WrRunCount(); i++)
    {
        const WrRun *r = WrRunAt(i);
        if (!r || !r->enabled || r->pointCount < 2)
            continue;
        int from = r->startIndex;
        int step = (r->pointCount - from) / 1000;
        if (step < 1)
            step = 1;
        for (int k = from; k < r->pointCount; k += step)
        {
            float e = WrEnergyOf(r->points[k].pos, r->points[k].vel);
            if (!WrSaneFloat(e))
                continue;
            if (e < lo) lo = e;
            if (e > hi) hi = e;
        }
    }
    if (lo > hi)
        return;             // nothing enabled; leave the range alone

    // A little margin, so the fastest point on the fastest line is not sitting
    // exactly on the end of the ramp where it stops varying.
    float pad = (hi - lo) * 0.05f + 25.0f;
    g_render.energyMin = floorf((lo - pad) / 50.0f) * 50.0f;
    g_render.energyMax = ceilf((hi + pad) / 50.0f) * 50.0f;
}

// The same, for the relative mode.
//
// Separate rather than a flag on the one above, because the quantity is
// different: this tracks how far each run strays from ITS OWN start, so a map
// whose runs all sit at z = 12000 fits to a band around zero here and to a band
// around twelve thousand there. Sharing a range between the two would make one
// of them useless whenever the other was fitted.
static void FitEnergyRelRange(void)
{
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < WrRunCount(); i++)
    {
        const WrRun *r = WrRunAt(i);
        if (!r || !r->enabled || r->pointCount < 2)
            continue;
        int from = r->startIndex;
        if (from < 0 || from >= r->pointCount)
            from = 0;
        float e0 = WrEnergyOf(r->points[from].pos, r->points[from].vel);
        if (!WrSaneFloat(e0))
            continue;
        int step = (r->pointCount - from) / 1000;
        if (step < 1)
            step = 1;
        for (int k = from; k < r->pointCount; k += step)
        {
            float e = WrEnergyOf(r->points[k].pos, r->points[k].vel) - e0;
            if (!WrSaneFloat(e))
                continue;
            if (e < lo) lo = e;
            if (e > hi) hi = e;
        }
    }
    if (lo > hi)
        return;

    float pad = (hi - lo) * 0.05f + 25.0f;
    g_render.energyRelMin = floorf((lo - pad) / 50.0f) * 50.0f;
    g_render.energyRelMax = ceilf((hi + pad) / 50.0f) * 50.0f;
}

static void DrawDisplayTab(void)
{
    ImGui::SeparatorText("Line");
    ImGui::SliderFloat("Thickness", &g_render.thickness, 0.5f, 10.0f, "%.1f px");
    ImGui::SliderFloat("Opacity", &g_render.alpha, 0.05f, 1.0f, "%.2f");
    ImGui::SliderFloat("Draw distance", &g_render.maxDrawDistance,
                       250.0f, 16000.0f, "%.0f u");
    ImGui::SliderFloat("Fade starts at", &g_render.fadeStartFraction,
                       0.0f, 1.0f, "%.2f of draw distance");
    ImGui::SliderFloat("Screen decimation", &g_render.pixelTolerance,
                       0.0f, 8.0f, "%.1f px");
    ImGui::Checkbox("Start each line where the run starts", &g_render.hidePreRoll);
    ImGui::SameLine();
    HelpMarker(
        "A demo starts recording before the run does. Measured over 500 demo "
        "headers here, the recording is a median of 2.06 seconds longer than the "
        "run -- the player walking into the start zone, and on the extracted "
        "line about three quarters of a second of it survives.\n\n"
        "That approach is what makes a replay look like it begins in an odd "
        "place, and it was also the graph's zero: the energy origin and both "
        "axes were taken from a point where the run had not started. Turning "
        "this on trims it from the lines; the graph is fixed either way.\n\n"
        "Not every run can be trimmed. The start is recovered from the run's "
        "own duration, cross-checked against the split times the game recorded, "
        "and on this machine that works for 61% of the files on disk. The rest "
        "draw in full, exactly as they always did -- the Runs tab marks which "
        "is which. Re-extracting fixes the remainder, because the extractor can "
        "match the start velocity the demo stores.");
    ImGui::SliderInt("Points per run", &g_render.pointBudget, 100, 6000);
    ImGui::TextDisabled("Caps per-frame work. Distance culling does the job on a big");
    ImGui::TextDisabled("open map, but a compact stage fits entirely inside the draw");
    ImGui::TextDisabled("distance, so nothing gets culled and cost scales with runs.");
    ImGui::TextDisabled("At most 256 enabled runs are drawn.");
    ImGui::SameLine();
    HelpMarker(
        "There was a slider here and it is gone, because it was never a "
        "preference -- it is a backstop on one button.\n\n"
        "The \"points per run\" budget above does NOT bound the total: it is per "
        "run, so the work is that budget times the number of lines. Distance "
        "culling is the only other thing that caps it, and a compact stage is "
        "exactly the case where culling rejects nothing.\n\n"
        "Measured on such a stage: 8 lines cost 0.24 ms a frame, 256 cost about "
        "8 ms, and 1000 cost 32 ms. A 60 Hz frame is 16.7 ms.\n\n"
        "It is not the same thing as the per-run ticks, which is why it exists: "
        "one press of All in Runs can enable up to a thousand, and this is "
        "applied afterwards, in the renderer. The Runs tab says so when you have "
        "ticked more than are being drawn.");

    ImGui::SeparatorText("Colour");
    const char *kRank[WR_RANK_MODE_COUNT] = {
        "off -- each run its own colour", "by placing", "by time behind the best"
    };
    if (g_render.rankColour < 0 || g_render.rankColour >= WR_RANK_MODE_COUNT)
        g_render.rankColour = 0;
    ImGui::Combo("Colour runs by rank", &g_render.rankColour, kRank,
                 WR_RANK_MODE_COUNT);
    ImGui::SameLine();
    HelpMarker(
        "Violet for the fastest run on each leg, then green through red for "
        "everyone behind it -- fastest to slowest. With a lot of runs enabled "
        "it tells you at a glance which line is worth following.\n\n"
        "Only FIRST is held out of the ramp. A gold, silver and bronze podium "
        "was the obvious thing and it did not survive contact with a screen: "
        "all three medals are warm mid-brightness colours living inside a ramp "
        "that already runs green to amber to red, so second and third "
        "disappeared into the field. Violet is the one hue the ramp never "
        "reaches. Second and third are now shaded like anyone else, which also "
        "says how close they were -- a silver medal never did.\n\n"
        "PLACED WITHIN EACH LEG. Momentum records a separate run per stage and "
        "per bonus, and the store is sorted by time across all of them, so the "
        "quickest time in the file is usually a stage rather than the main "
        "track. On bhop_futile here the main runs sit between 52.8 and 54.3 "
        "seconds and there is a bonus at 33.9 -- rank them together and the "
        "bonus takes first place from a track it was never racing. Each leg "
        "gets its own winner.\n\n"
        "BY PLACING spreads the colours evenly over the field, so it stays "
        "readable however tightly the times are packed. BY TIME BEHIND shades "
        "by how far off the record each run actually is, which on a board where "
        "everyone is within a second of the best leaves them all green -- "
        "truthful, and the more useful of the two for judging a field rather "
        "than picking a line.\n\n"
        "It replaces the run's colour everywhere: the line, its name tag, its "
        "ramp numbers, its checkpoints. That includes any colour you picked by "
        "hand in the Runs tab, which this necessarily overrides while it is on.");
    if (g_render.rankColour != WR_RANK_OFF)
    {
        if (g_render.rankColour == WR_RANK_BY_TIME)
            ImGui::SliderFloat("Full red at", &g_render.rankFullBehind,
                               2.0f, 200.0f, "+%.0f%% off the best");
        ImGui::Checkbox("Show the rank key on screen", &g_render.rankLegend);
    }

    // Radio, not three checkboxes. Only one quantity can be mapped onto a line's
    // colour, and the previous pair of booleans had an unwritten precedence:
    // ticking "colour by speed" while efficiency was on did nothing at all.
    ImGui::Text("Colour along each line by");
    ImGui::SameLine();
    HelpMarker(
        "One at a time, because a line has one colour and these are three "
        "different measurements of it. Rank colour above is separate and stacks "
        "with these: it decides what each run's BASE colour is, and efficiency "
        "shades away from that base.\n\n"
        "SPEED is the obvious one and the least informative on a surf map, "
        "where speed is mostly a function of where you are on the ramp.\n\n"
        "ENERGY is height plus speed squared over 2g -- what the run actually "
        "had to spend, whether it was holding it as altitude or as pace. It is "
        "an absolute figure, so the same colour is the same energy on every "
        "line and two runs can be read against each other directly. That is why "
        "there is a fit button: the range has to match the map before the "
        "middle of it means anything.\n\n"
        "EFFICIENCY is how much of the energy air strafing could physically "
        "have added was actually added. It needs the per-point data the "
        "extractor writes, so it works on demo lines and not on your own.");
    const char *kLine[WR_LINE_MODE_COUNT] = {
        "nothing -- one colour per run", "speed", "energy (absolute height)",
        "energy above its own start", "strafing efficiency"
    };
    if (g_render.lineColour < 0 || g_render.lineColour >= WR_LINE_MODE_COUNT)
        g_render.lineColour = WR_LINE_FLAT;
    ImGui::Combo("##linecolour", &g_render.lineColour, kLine, WR_LINE_MODE_COUNT);

    if (g_render.lineColour != WR_LINE_FLAT ||
        g_render.rankColour != WR_RANK_OFF)
    {
        ImGui::Checkbox("Scale the range to the runs that are on",
                        &g_render.autoScale);
        ImGui::SameLine();
        HelpMarker(
            "Takes the ends of the ramp from the runs currently enabled, on the "
            "leg they are on, instead of from the sliders below.\n\n"
            "The fixed range is 250 to 3500 u/s, which is right for the top of a "
            "board and mostly wasted on the rest of it: a run that lives between "
            "400 and 1200 occupies a fifth of the ramp and comes out one colour. "
            "Scaled, the ramp covers what the lines on screen actually did.\n\n"
            "Rank scales too, and that is the bigger change: with four lines on "
            "screen, colour-by-rank spends its whole ramp on those four rather "
            "than on the nine thousand runs they were picked from. The Runs "
            "list still reports the true leaderboard placing, which does not "
            "move.\n\n"
            "Your sliders are not touched. Turning this off puts the exact "
            "numbers you set back, because they were never overwritten.");
        if (g_render.autoScale)
        {
            float lo = 0.0f, hi = 0.0f;
            bool scaled = false;
            WrRenderColourRange(&lo, &hi, &scaled);
            if (g_render.lineColour != WR_LINE_FLAT &&
                g_render.lineColour != WR_LINE_EFFICIENCY)
                ImGui::TextDisabled("currently %.0f to %.0f", lo, hi);
        }
    }

    if (g_render.lineColour == WR_LINE_SPEED)
    {
        ImGui::SliderFloat("Slow", &g_render.speedMin, 0.0f, 3000.0f, "%.0f u/s");
        ImGui::SliderFloat("Fast", &g_render.speedMax, 100.0f, 6000.0f, "%.0f u/s");
        if (g_render.speedMax < g_render.speedMin + 50.0f)
            g_render.speedMax = g_render.speedMin + 50.0f;
        ImGui::Checkbox("Show the key on screen##spd", &g_render.lineKey);
        ImGui::TextDisabled("blue slow -> cyan -> green -> yellow -> red fast");
    }
    else if (g_render.lineColour == WR_LINE_ENERGY)
    {
        ImGui::SliderFloat("Low", &g_render.energyMin, -8000.0f, 16000.0f, "%.0f");
        ImGui::SliderFloat("High", &g_render.energyMax, -8000.0f, 32000.0f, "%.0f");
        if (g_render.energyMax < g_render.energyMin + 50.0f)
            g_render.energyMax = g_render.energyMin + 50.0f;
        if (ImGui::Button("Fit to the runs on screen"))
            FitEnergyRange();
        ImGui::SameLine();
        HelpMarker(
            "Energy is an absolute height in world units, so its useful range "
            "depends entirely on where the map sits in the world -- a map built "
            "at z = -8000 and one built at z = +12000 share no part of the "
            "scale. This walks the enabled runs and sets the ends to what they "
            "actually reach, with a little margin.\n\n"
            "Not done automatically, and not cached: the figure moves with the "
            "gravity setting, so a range fitted once and kept would quietly "
            "stop matching the lines it was fitted to.");
        ImGui::Checkbox("Show the key on screen##nrg", &g_render.lineKey);
        ImGui::TextDisabled("blue low -> cyan -> green -> yellow -> red high");
    }
    else if (g_render.lineColour == WR_LINE_ENERGY_REL)
    {
        ImGui::SliderFloat("Low", &g_render.energyRelMin, -4000.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat("High", &g_render.energyRelMax, -1000.0f, 8000.0f, "%.0f");
        if (g_render.energyRelMax < g_render.energyRelMin + 50.0f)
            g_render.energyRelMax = g_render.energyRelMin + 50.0f;
        if (ImGui::Button("Fit to the runs on screen##rel"))
            FitEnergyRelRange();
        ImGui::SameLine();
        HelpMarker(
            "The same energy as the mode above, less whatever each run started "
            "with -- so every line reads zero at its own start and the numbers "
            "sit either side of it. That is the scale the crosshair readout uses, "
            "which is why a range like -100 to 500 makes sense here and would be "
            "meaningless above.\n\n"
            "The trade is exact and worth knowing. Absolute energy means the "
            "same colour is the same energy on every line, so two runs can be "
            "read against each other directly. This does not: it is the same "
            "MARGIN over each run's own start. A line that begins four hundred "
            "units lower than another is not painted worse for it, which is the "
            "right question when what you want to know is who held on to what "
            "they had.");
        ImGui::Checkbox("Show the key on screen##rel", &g_render.lineKey);
        ImGui::TextDisabled("blue lost the most -> green -> red gained the most");
    }

    ImGui::SeparatorText("Aim at a line");
    ImGui::Checkbox("Say whose line I am looking at", &g_render.pickEnabled);
    ImGui::SameLine();
    HelpMarker(
        "Point the crosshair at a line and it thickens, gets a ring at the "
        "point you are aiming at, and a plate saying whose it is and what it "
        "was carrying there.\n\n"
        "It uses the crosshair, not the mouse, because there is no mouse while "
        "you are playing -- ImGui's pointer is parked the moment this panel "
        "closes.\n\n"
        "TWO THINGS IT CANNOT DO. Lines are drawn through walls, so a line "
        "behind the ramp you are standing on is just as pickable as one in "
        "front of it. And two runs that sit within a few pixels of each other "
        "for the whole visible stretch cannot be told apart by aiming -- when "
        "that happens the plate says how many others were equally close, so a "
        "coin toss is visible instead of being presented as an answer.\n\n"
        "The cost is about 0.3 ms a frame with 256 lines drawn, against the "
        "8 ms drawing them already costs. Diagnostics shows the real figure.");
    {
        KeyBindCombo("Toggle key", &g_pickToggleKey);
        ImGui::SameLine();
        HelpMarker(
            "Turns the plate off and on without opening this panel, which is the "
            "only way it is useful: the plate sits over the thing it is naming, "
            "so wanting it gone is a mid-run thought. It is OFF by default and "
            "this is how you get it.\n\n"
            "A list rather than a fixed key, because WrLines cannot see what you "
            "have bound. The key is read, not swallowed -- a collision means the "
            "game acts on it too.");
    }
    if (g_render.pickEnabled)
    {
        ImGui::SliderFloat("Aim tolerance", &g_render.pickRadiusPx, 8.0f, 160.0f,
                           "%.0f px");
        ImGui::SliderFloat("Prefer near lines", &g_render.pickDepthBias, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SameLine();
        HelpMarker(
            "How much a distant line is penalised against a near one at the "
            "same distance from the crosshair. At zero, two lines crossing on "
            "screen swap the pick on sub-pixel noise -- which is exactly the "
            "situation where you wanted an answer.");
        ImGui::SliderFloat("Thicker by", &g_render.pickThickBoost, 1.0f, 4.0f,
                           "%.2fx");
        ImGui::SliderFloat("Keep showing for", &g_render.pickHoldSeconds,
                           0.0f, 2.0f, "%.2f s");
        ImGui::SliderFloat("Plate clearance", &g_render.pickOffsetPx,
                           8.0f, 160.0f, "%.0f px");
        ImGui::SameLine();
        HelpMarker("Clear space between the point you are aiming at and the "
                   "nearest edge of the box. Which way the box sits does not "
                   "change -- it is always pushed away from the middle of the "
                   "screen, so aiming up and right puts it up and right -- only "
                   "how far.\n\n"
                   "This used to offset the box's CENTRE, which meant the "
                   "clearance you actually got depended on how many rows were in "
                   "it: a tall plate on a mostly-vertical aim ended up sitting "
                   "on top of the very thing it was labelling.");
        ImGui::Checkbox("Ring the point", &g_render.pickRing);

        // Written out rather than passed to LabelPicker: that helper pairs the
        // bits with an on/off checkbox, and here the only thing that could
        // switch off is the whole feature, which already has one above.
        ImGui::TextUnformatted("Plate shows");
        static const struct { unsigned int bit; const char *name; } kPick[4] = {
            { WR_LABEL_SPEED,  "speed" },
            { WR_LABEL_ENERGY, "energy" },
            { WR_LABEL_TIME,   "time" },
            { WR_LABEL_DELTA,  "vs you" },
        };
        for (int b = 0; b < 4; b++)
        {
            bool on = (g_render.pickLabel & kPick[b].bit) != 0;
            ImGui::SameLine();
            ImGui::PushID(b + 700);
            if (ImGui::Checkbox(kPick[b].name, &on))
                g_render.pickLabel = on ? (g_render.pickLabel | kPick[b].bit)
                                        : (g_render.pickLabel & ~kPick[b].bit);
            ImGui::PopID();
        }

        int pi = 0, tied = 0;
        float px = 0.0f;
        const WrRun *picked = WrPickedRun(&pi, &px, &tied);
        if (picked)
            ImGui::TextDisabled("now: %s, %.0f px off the crosshair",
                                picked->player[0] ? picked->player : "(unknown)", px);
        else
            ImGui::TextDisabled("now: nothing aimed at");
    }

    ImGui::SeparatorText("Who is who");
    ImGui::Checkbox("Name tags on lines", &g_render.drawTags);
    ImGui::SameLine();
    ImGui::Checkbox("Avatars", &g_render.tagAvatars);
    ImGui::SameLine();
    HelpMarker(
        "One setting for both places a runner is named: the tag on their line, "
        "and the plate you get for aiming at one.\n\n"
        "Pictures arrive from the Steam client a moment after they are asked "
        "for, so a runner shows a dot in their line's colour first and fills in. "
        "The cache holds 96 and never evicts -- on a map where you meet more "
        "faces than that, the ones met last keep their dot for the session. The "
        "space is reserved either way, so nothing shifts when a picture lands.");
    ImGui::SliderFloat("Tag size", &g_render.tagScale, 0.6f, 2.0f, "%.2fx");
    ImGui::SliderInt("Max tags on screen", &g_render.maxTags, 1, 32);
    ImGui::TextDisabled("Tags sit where each line crosses the fade distance, so");
    ImGui::TextDisabled("they spread along different lines instead of stacking.");
    if (WrRunEnabledCount() > g_render.maxTags)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%d runs enabled, %d tags shown -- the rest are unlabelled.",
                           WrRunEnabledCount(), g_render.maxTags);

    ImGui::SeparatorText("Numbers on the line");
    LabelPicker("At ramp bottoms", &g_render.dipLabel, &g_render.drawDipSpeeds);
    ImGui::SliderInt("Max ramps per run", &g_render.maxDipsPerRun, 1, 100);
    ImGui::Spacing();
    LabelPicker("At tops", &g_render.peakLabel, &g_render.drawPeaks);
    ImGui::SliderInt("Max tops per run", &g_render.maxPeaksPerRun, 1, 100);
    ImGui::SameLine();
    HelpMarker("The high points, found the same way the bottoms are: where the "
               "stored vertical velocity changes sign, after a climb of at least "
               "32 units so a wobble on a flat section does not become a label.\n\n"
               "A bottom and a top answer different halves of one question. The "
               "bottom says what a line carried THROUGH the ramp; the top says "
               "what it bought with it. Energy is the default here because that "
               "is the number that stays still between the two when nothing was "
               "wasted -- speed traded for height reads as a large change and no "
               "loss at all.\n\n"
               "Both ends default to energy now. The pair is worth more than "
               "either alone, because the DIFFERENCE between the top and the "
               "bottom is what the arc cost -- and speed at a ramp bottom "
               "cannot be compared with anything above it.\n\n"
               "It does mean twice as many numbers. Nothing from the walk-in is "
               "labelled any more, which takes one off every line, and name "
               "tags now keep a reserved share of the space so a crowd of "
               "numbers cannot push them off.");
    ImGui::TextDisabled("Exact on stored lines. Your own line has no marks yet --");
    ImGui::TextDisabled("that needs a second derivative of a camera-differenced");
    ImGui::TextDisabled("velocity and is not measured well enough to draw.");
    ImGui::Spacing();
    LabelPicker("At checkpoints", &g_render.markerLabel, &g_render.drawMarkers);
    ImGui::SliderFloat("Marker size", &g_render.markerRadius, 2.0f, 16.0f, "%.1f px");
    ImGui::Spacing();
    ImGui::TextDisabled("Numbers on screen are capped, and name tags keep a share");
    ImGui::TextDisabled("of the room so they cannot be crowded out.");
    ImGui::SameLine();
    HelpMarker("Numbers reserve their rectangle and skip if something is already "
               "there, so they never print on top of each other or of a name "
               "tag. Whatever does not fit is dropped rather than stacked.\n\n"
               "Two sliders that were here are gone. The checkpoint one could "
               "not express anything: it stopped at 64 and a run cannot hold "
               "more than 64 checkpoints. The label one is not clutter control "
               "-- numbers and name tags share one pool of 128 rectangles and "
               "numbers are placed first, so with no cap two or three runs' "
               "numbers take the lot and every later run silently loses its "
               "NAME.\n\n"
               "Time at a ramp bottom is only offered on runs whose recovered "
               "timing passed the trust test: point times are derived from the "
               "sample index, and on the worst map measured that runs from 0.36x "
               "to 10.32x. A checkpoint's split is measured by the game itself, "
               "so it is always shown.\n\n"
               "Your delta appears only where your own recorded path has "
               "actually been, and says nothing anywhere else.");
    ImGui::TextDisabled("Checkpoints are only drawn for runs whose splits could be");
    ImGui::TextDisabled("anchored to the path with confidence.");

    ImGui::SeparatorText("Where you are going");
    ImGui::Checkbox("Draw a velocity vector", &g_render.drawVelocity);
    ImGui::SameLine();
    HelpMarker("From your midsection, along the way you are actually moving, "
               "reaching as far as you will travel in the next quarter second. "
               "Length is time rather than an arbitrary scale, so the tip lands "
               "on the surface you are about to meet.\n\n"
               "GREEN means your energy is RISING, red means falling, grey "
               "means neither -- measured over 0.75 s. It is the same signal as "
               "the arrow beside your crosshair, so those two can never "
               "disagree.\n\n"
               "IT IS NOT THE SAME THING AS THE LINE COLOURS, even though it "
               "uses the same red and green. Those are efficiency: how much of "
               "what air strafing could add was actually added. This is a "
               "trend: which way your total is going. They disagree on purpose "
               "and the disagreement is the interesting case -- on a ramp you "
               "can be strafing beautifully, green on a demo line, while your "
               "own energy falls because the ramp is taking more than you can "
               "put in.\n\n"
               "It used to be live strafing efficiency, and that was measuring "
               "noise: on a path where energy is exactly constant it saturated "
               "red or green 14% of the time at 2000 u/s and 36% at 3200.");

    ImGui::SeparatorText("Strafing efficiency");
    ImGui::TextDisabled("Turned on in \"Colour along each line by\", above.");
    ImGui::SameLine();
    HelpMarker(
        "GREEN means energy is being added. RED means energy is being "
        "destroyed. The run's own colour, dimmed, means nothing is happening.\n\n"
        "It is not a score, and it is not a turn-rate meter.\n\n"
        "GREEN -- strafing is converting mouse movement into speed, near the "
        "fastest physics allows. Air acceleration can add at most 37 energy "
        "units per second, the same at 500 u/s as at 3500, and full colour is "
        "60% of that.\n\n"
        "DIM -- free flight. Falling, or riding a ramp without gaining. Nothing "
        "is wrong and nothing is being won. Measured on surf_demise: the world "
        "record is dim for 6% of its length, the slowest run for 34%. A line "
        "that is mostly dim means the player is barely strafing.\n\n"
        "RED -- energy left the system: a ramp entry, a wall clip, a landing. "
        "EVERY ramp entry costs some, even a perfect one, so red flecks between "
        "green stretches is what a good run looks like -- the world record is "
        "41% red. Red is NOT 'you turned too far'. Turning faster than air "
        "acceleration alone could manage happens in 10 to 24% of every world "
        "record measured, because the ramp does the turning.\n\n"
        "FADED -- no reading at all. A booster fired (they add energy for free "
        "and would otherwise look like perfect strafing), or the line teleports "
        "nearby. About 0.6% of points.\n\n"
        "DEMO LINES ONLY. Your own line is not coloured this way and will not "
        "be. A stored run differences a velocity the demo recorded; yours has "
        "to difference one estimated from where the camera was. Simulated "
        "against twelve real runs, that estimate colours the line correctly "
        "58% of the time over a 0.4 s window and points the WRONG WAY 24% of "
        "it -- and restricted to airborne samples, where the number actually "
        "means air strafing, it is 46% right and 32% backwards.\n\n"
        "It is not camera shake: with a perfectly steady camera the figures "
        "are the same. Air acceleration's whole budget is 37 energy units per "
        "second, and a velocity worked out from camera positions cannot "
        "resolve a number that small. A quarter of your line drawn backwards, "
        "beside demo lines that are exact, would be worse than drawing "
        "nothing.");

    if (g_render.lineColour == WR_LINE_EFFICIENCY)
    {
        ImGui::SliderInt("Measured over", &g_wrEffWindow, 1, 32,
                         "%d points either side");
        // Deactivation, not the drag: a rebuild is a full pass over every point
        // of every loaded run, and doing that on each frame of a slider drag is
        // a freeze rather than a preview.
        if (ImGui::IsItemDeactivatedAfterEdit())
            WrPathRefreshEfficiency();
        ImGui::SameLine();
        HelpMarker(
            "The window the figure is differenced over, in points either side. "
            "Four is about an eighth of a second at 66 tick.\n\n"
            "Narrow follows a ramp entry closely and carries the noise of a "
            "short difference; wide is steadier but smears the moment a line "
            "lost its energy over the points around it. Rebuilt when you let go "
            "of the slider, because it is a pass over every point of every "
            "loaded run.");
        ImGui::Checkbox("Show the key on screen", &g_render.lineKey);
        ImGui::SameLine();
        ImGui::Checkbox("Blue/orange instead", &g_render.effColourblind);
        ImGui::SliderFloat("Full colour at", &g_render.effSaturation,
                           0.2f, 1.0f, "eta %.2f");
        ImGui::SliderFloat("Dim below", &g_render.effNeutralBand,
                           0.0f, 0.5f, "eta %.2f");
        ImGui::TextDisabled("Measured over 137,006 samples on 50 surf_demise runs:");
        ImGui::TextDisabled("median eta +0.17, p75 +0.67. 11%% sit inside the dim band.");
    }

    ImGui::SeparatorText("Your own path");
    bool live = WrLiveEnabled();
    if (ImGui::Checkbox("Record my path", &live))
        WrLiveSetEnabled(live);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        WrLiveClear();
    ImGui::SameLine();
    HelpMarker("Kept until the next attempt actually starts, not until you fail.\n\n"
               "Failing a run drops you back on the pad, which is a long way, and "
               "the recorder used to read that as a teleport and wipe the buffer "
               "on the spot -- so by the time you opened the graph to see what "
               "went wrong there was nothing left to see, and it looked as though "
               "looking at it had cleared it.\n\n"
               "Now a restart HOLDS what you recorded. It clears when you leave "
               "the start zone, which is the moment the next attempt begins.");
    ImGui::Checkbox("Draw my path", &g_render.drawLive);
    int n = 0;
    WrLivePoints(&n);
    ImGui::SameLine();
    if (WrLiveHeld())
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                           "(%d points, held -- your last attempt, until you "
                           "leave the start)", n);
    else
        ImGui::TextDisabled("(%d points)", n);

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults"))
        WrRenderDefaults();
}

static void DrawFrameCapTab(void)
{
    ImGui::TextWrapped(
        "A frame cap, so you do not need a second overlay running just for one. "
        "Two tools drawing into the same swapchain is a fight neither wins -- "
        "and the other one usually cannot even be detected, because it replaces "
        "the swapchain with its own object rather than hooking Present.");

    ImGui::SeparatorText("Cap");
    ImGui::Checkbox("Limit the frame rate", &g_limit.enabled);

    // Both modes stay visible, so the number in force is always readable.
    int mode = g_limit.autoTarget ? 0 : 1;
    ImGui::RadioButton("Follow the display", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Custom", &mode, 1);
    g_limit.autoTarget = (mode == 0);

    if (!g_limit.autoTarget) ImGui::BeginDisabled();
    if (WrLimitRefreshHz() > 1.0f)
        ImGui::Text("display    %.0f Hz", WrLimitRefreshHz());
    else
        ImGui::TextDisabled("display    unknown yet (needs a frame to look)");
    ImGui::SliderFloat("Headroom", &g_limit.headroomHz, 0.0f, 15.0f, "%.0f Hz");
    ImGui::SameLine();
    HelpMarker("Subtracted from the refresh rate. On a variable-refresh display "
               "you want the cap a little under the panel's ceiling so a late "
               "frame never reaches it and drops out of the VRR window. Three is "
               "the usual recommendation.");
    if (!g_limit.autoTarget) ImGui::EndDisabled();

    if (g_limit.autoTarget) ImGui::BeginDisabled();
    int custom = (int)(g_limit.targetFps + 0.5f);
    if (ImGui::InputInt("Target fps", &custom, 1, 10))
        g_limit.targetFps = WrClampF((float)custom, WR_LIMIT_MIN_FPS, WR_LIMIT_MAX_FPS);
    // How people actually pick a cap, rather than dragging for it.
    static const int presets[] = { 60, 120, 144, 165, 240, 300, 360 };
    for (int i = 0; i < (int)(sizeof(presets) / sizeof(presets[0])); i++)
    {
        if (i) ImGui::SameLine();
        char lbl[16];
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "%d", presets[i]);
        if (ImGui::SmallButton(lbl))
        {
            g_limit.targetFps = (float)presets[i];
            g_limit.autoTarget = false;
        }
    }
    if (g_limit.autoTarget) ImGui::EndDisabled();

    ImGui::Text("capping at %.1f fps  (%.3f ms)", WrLimitTargetFps(),
                1000.0f / WrLimitTargetFps());

    // How the cap sits against the panel, which is the thing most likely to be
    // wrong and is invisible otherwise.
    //
    // A cap that is not a whole fraction of the refresh rate cannot be shown
    // evenly unless variable refresh is actually engaged. 240 on a 360 Hz panel
    // is 1.5 refreshes per frame: with a fixed 360 Hz the panel has to alternate
    // one refresh and two, so the frames arrive 2.8 ms, 5.6 ms, 2.8 ms, 5.6 ms
    // -- an average of exactly 240 that looks nothing like 240.
    float hz = WrLimitRefreshHz();
    if (hz > 1.0f)
    {
        float per = hz / WrLimitTargetFps();
        float nearest = (float)(int)(per + 0.5f);
        bool even = (nearest >= 1.0f) && (fabsf(per - nearest) < 0.04f);
        if (even)
            ImGui::TextDisabled("%.0f Hz / %.0f = %.2f refreshes per frame -- even",
                                hz, WrLimitTargetFps(), per);
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "%.0f Hz / %.0f = %.2f refreshes per frame",
                               hz, WrLimitTargetFps(), per);
            ImGui::TextWrapped(
                "That is not a whole number, so unless variable refresh is "
                "actually engaged the panel cannot show these frames evenly -- "
                "it has to alternate between holding a frame for one refresh and "
                "for two, which reads as far worse than the number suggests.");
            ImGui::TextDisabled("Even divisions of %.0f Hz:", hz);
            for (int d = 1; d <= 6; d++)
            {
                float f = hz / (float)d;
                if (f < WR_LIMIT_MIN_FPS || f > WR_LIMIT_MAX_FPS)
                    continue;
                ImGui::SameLine();
                char lbl[24];
                _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "%.0f", f);
                if (ImGui::SmallButton(lbl))
                {
                    g_limit.targetFps = f;
                    g_limit.autoTarget = false;
                }
            }
        }
    }

    ImGui::SeparatorText("Measured");
    float ms = WrLimitFrameMs();
    ImGui::Text("frame time %.3f ms   (%.1f fps)", ms, ms > 0.0f ? 1000.0f / ms : 0.0f);
    if (g_limit.enabled)
    {
        ImGui::Text("worst wobble %.3f ms over the last 120 frames", WrLimitJitterMs());
        ImGui::SameLine();
        HelpMarker("Largest deviation from the target interval, not an average "
                   "-- an average would hide exactly the spikes that read as "
                   "judder. Under about 0.2 ms is imperceptible.");
        ImGui::Text("spinning   %.1f%% of each frame", WrLimitSpinPercent());
        if (WrLimitCpuBound())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "Frames are arriving later than the cap asks for.");
            ImGui::TextWrapped(
                "The cap is not what is limiting you here -- the game or the GPU "
                "is. Lower the target until this goes away, or leave it; the "
                "limiter is not making anything worse, it just has nothing to do.");
        }
    }
    else
    {
        ImGui::TextDisabled("Cap is off. Frame time above is still live.");
    }

    ImGui::SeparatorText("Accuracy");
    ImGui::SliderFloat("Spin window", &g_limit.spinMs, 0.0f, 2.0f, "%.2f ms");
    ImGui::SameLine();
    HelpMarker("The wait is a high-resolution timer for the bulk of the frame "
               "and a busy-wait for the last fraction of a millisecond. The "
               "timer alone lands within about half a millisecond, which at 160 "
               "fps is 8% of a frame and visibly uneven. The spin closes that "
               "for a few percent of one core. Set it to 0 to never spin -- "
               "cheaper, looser.");
    if (ImGui::Button("Reset frame cap settings"))
        WrLimitDefaults();

    ImGui::SeparatorText("Worth knowing");
    ImGui::TextWrapped(
        "This paces when frames are handed to the display. It cannot make a "
        "frame that took 20 ms of GPU work arrive sooner, so a cap above what "
        "the machine sustains does nothing at all.");
    ImGui::TextWrapped(
        "If the game's own vsync is on, Present already blocks on the vertical "
        "blank and this sits on top of it. That is the normal variable-refresh "
        "setup -- vsync on, capped a few Hz below the panel -- and works fine.");
    ImGui::TextWrapped(
        "None of this is derived from any other limiter's code. Frame pacing is "
        "standard technique, and copying a GPL-licensed implementation would "
        "have relicensed this whole project by accident.");
}

// ---------------------------------------------------------------------------
// Graphs -- energy against distance or time, every enabled run at once
// ---------------------------------------------------------------------------
//
// The crosshair readout says what your energy is NOW, and that cannot answer the
// question this tab exists for: whether a surfer bled their energy away evenly
// across a stage or threw it all away at one ramp. Those look identical at the
// finish and want completely different practice.
//
// Everything here is relative to each series' OWN first point, which is not a
// display preference -- a stored run's points are a player's feet and your live
// line is your camera, 64 units apart forever. Subtracting each series' own
// start cancels that exactly. See wr_profile.h.

static bool g_gByTime = false;
static bool g_gNormalise = false;
static bool g_gBand = true;
static bool g_gTurns = true;
static bool g_gLive = true;
static int g_gMaxSeries = 12;

#define G_MAX_SERIES 32

struct GSeries
{
    const WrProfile *p;
    const WrRun *run;           // NULL for your own line
    unsigned int colour;
    const char *name;
};

static unsigned int GAlpha(unsigned int c, float a)
{
    unsigned int base = c & 0x00FFFFFFu;
    unsigned int al = (unsigned int)(((c >> 24) & 0xFFu) * a);
    if (al > 255) al = 255;
    return base | (al << 24);
}

// A grid step that lands on a number a person would choose, covering `range` in
// somewhere between four and ten lines.
static float GNiceStep(float range)
{
    if (!(range > 0.0f))
        return 1.0f;
    float rough = range / 6.0f;
    float mag = powf(10.0f, floorf(log10f(rough)));
    float norm = rough / mag;
    float step = (norm < 1.5f) ? 1.0f : (norm < 3.5f) ? 2.0f
               : (norm < 7.5f) ? 5.0f : 10.0f;
    return step * mag;
}

// A series' x for one bucket, in whatever the axis currently is.
static float GX(const GSeries &s, const WrProfileBucket &b)
{
    float raw = g_gByTime ? b.t : b.d;
    if (!g_gNormalise)
        return raw;
    float total = g_gByTime ? s.p->tTotal : s.p->dTotal;
    return total > 1e-4f ? 100.0f * raw / total : 0.0f;
}

static void DrawGraphsTab(void)
{
    ImGui::TextWrapped(
        "Every enabled run's energy across the whole of it, so you can see the "
        "SHAPE of a loss rather than its total. A line that sags gently was "
        "leaking everywhere; a line with one cliff in it lost everything at one "
        "ramp, and only one of those is worth practising the same way.");
    ImGui::TextDisabled(
        "Each curve starts at zero -- its own start, not a shared one. Runs "
        "store feet and your line is a camera, so nothing else would line up.");

    // Resets the per-frame build budget as well as counting; see wr_profile.h.
    int pending = WrProfilePending();

    ImGui::Checkbox("Against time", &g_gByTime);
    ImGui::SameLine();
    ImGui::Checkbox("As a percentage", &g_gNormalise);
    ImGui::SameLine();
    HelpMarker("Distance is the honest axis: it is measured from the points "
               "themselves and does not care whether the extraction recovered "
               "every tick.\n\n"
               "Time is recovered rather than recorded -- point times are "
               "derived from the sample index, and a run whose recovered clock "
               "failed its trust test is left out of a time plot entirely "
               "instead of being drawn wrong. On the worst map measured that "
               "clock ran anywhere from 0.36x to 10.32x.\n\n"
               "As a percentage puts every run on 0-100 of its own length, "
               "which is how you compare a stage against the main track, or two "
               "runs that took different routes.");
    ImGui::Checkbox("Min/max band", &g_gBand);
    ImGui::SameLine();
    ImGui::Checkbox("Mark tops and bottoms", &g_gTurns);
    ImGui::SameLine();
    ImGui::Checkbox("Your own line", &g_gLive);
    ImGui::SetNextItemWidth(160.0f);
    // AlwaysClamp is not decoration. A slider's min/max bound the DRAG; ImGui
    // lets you CTRL+Click any slider and type a number straight past them, and
    // this one sizes nothing -- it is compared against an index into a fixed
    // 33-element stack array below. Typing 900 here wrote 900 entries into it.
    ImGui::SliderInt("Curves at once", &g_gMaxSeries, 1, G_MAX_SERIES, "%d",
                     ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Detail", &g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS,
                     "%d points", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    HelpMarker(
        "How many samples each curve is drawn from. This is the graph's own "
        "averaging window and it scales with the run: 480 points across a "
        "sixty-second run is an eighth of a second each.\n\n"
        "Fewer is a smoother, blunter curve. That is the right trade when the "
        "question is where a run lost its energy rather than what happened on "
        "one particular ramp.\n\n"
        "Changing it rebuilds the curves, a few per frame, so the plot settles "
        "over the next moment rather than instantly.");

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Smooth my curve", &g_wrProfileLiveSmooth, 0.0f, 0.50f,
                       "%.2f s", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    HelpMarker(
        "Your curve only. Nothing here touches a stored run.\n\n"
        "The two are built by identical rules and still do not look alike, and "
        "the reason is the velocity rather than the arithmetic. A demo carries "
        "the velocity Momentum recorded, which is exact. Yours is estimated by "
        "differencing camera positions, and that error is broadband -- a "
        "continuous wobble rather than the occasional two-tick lie the spike "
        "filter below is aimed at. Five frames at 200 fps is 25 ms, which is far "
        "too short a window to touch it, so the spike filter leaves your curve "
        "exactly as jumpy as it found it.\n\n"
        "A mean over a span of TIME is the matching tool. Time and not samples, "
        "because the recorder's rate is your frame rate gated on two units of "
        "movement -- a fixed sample count would be a different window at every "
        "speed.\n\n"
        "It also rounds off real ramp exits, which is why the legend says the "
        "curve is smoothed whenever this is above zero. Set it to 0 for the raw "
        "curve.");

    ImGui::Checkbox("Take out transient spikes", &g_wrProfileDespike);
    ImGui::SameLine();
    HelpMarker(
        "Momentum's recorded velocity jumps for a tick or two and comes straight "
        "back, at fixed places on a map. It is not something the player did: "
        "measured across the fourteen surf_fiellu bonus-4 runs here, 206 of 208 "
        "of these jumps are in the SPEED term and only two in height -- so it is "
        "not ducking -- and 88% of them are back where they started within two "
        "ticks. Across the whole library it is 0.13% of 7.4 million ticks, but "
        "76% of runs carry at least one.\n\n"
        "What proves they are not real: they happen in free fall, where energy "
        "is conserved. One run sits at 1476 units for twenty ticks, reads 2054 "
        "for exactly two, and returns to 1470, while its height falls smoothly "
        "throughout. Nothing gains 577 units and gives them back in 30 ms under "
        "gravity alone.\n\n"
        "A median of five removes a two-tick spike exactly and passes everything "
        "else through untouched -- an average would smear the spike across 75 ms "
        "and round off the real ramp exits, which are the steepest features on "
        "the curve and the reason to look at it.\n\n"
        "It changes the PICTURE and never the stored run, and it is not applied "
        "to the coloured lines: there the same spikes are two points in several "
        "thousand, a colour blip rather than a scale problem.");

    // --- gather -------------------------------------------------------------
    GSeries series[G_MAX_SERIES + 1];
    int nSeries = 0;
    int dropped = 0, untimed = 0, building = 0;

    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *r = WrRunAt(i);
        if (!r || !r->enabled || r->pointCount < 4)
            continue;
        // Both bounds, not just the setting. The clamp on the slider above is
        // the fix; this is the one that does not depend on an ImGui flag staying
        // where it was put, and it is the array's own bound.
        if (nSeries >= g_gMaxSeries || nSeries >= G_MAX_SERIES)
        {
            dropped++;
            continue;
        }
        const WrProfile *p = WrProfileFor(r);
        if (!p || !p->b || p->n < 2)
        {
            // Still building. Counted here rather than from WrProfilePending,
            // which counts every enabled run -- including the ones past the
            // curve cap that will never be built, and would leave a "building"
            // message on screen forever.
            building++;
            continue;                       // back next frame
        }
        if (g_gByTime && !p->timeUsable)
        {
            untimed++;
            continue;
        }
        series[nSeries].p = p;
        series[nSeries].run = r;
        // WrRunColour, not r->colour. Reading the stored colour directly meant
        // that with rank colouring on, a run's curve here and its line in the
        // world were different colours -- which is the exact confusion
        // WrRunColour exists to prevent, and this was the last site bypassing it.
        series[nSeries].colour = WrRunColour(r);
        series[nSeries].name = r->player;
        nSeries++;
    }

    if (g_gLive)
    {
        const WrProfile *lp = WrProfileLive();
        if (lp && lp->b && lp->n >= 2 && nSeries <= G_MAX_SERIES)
        {
            series[nSeries].p = lp;
            series[nSeries].run = NULL;
            series[nSeries].colour = g_render.liveColour;

            // Named for what it IS. A smoothed curve laid over unsmoothed ones
            // that does not say so is a comparison nobody can check, and the
            // filter rounds off exactly the ramp exits this graph is read for.
            // Static because `name` is a borrowed pointer, kept for the frame.
            static char liveName[40];
            if (lp->builtSmooth > 1e-4f)
            {
                _snprintf_s(liveName, sizeof(liveName), _TRUNCATE,
                            "you (smoothed %.2f s)", lp->builtSmooth);
                series[nSeries].name = liveName;
            }
            else
            {
                series[nSeries].name = "you";
            }
            nSeries++;
        }
    }

    (void)pending;
    if (building > 0)
        ImGui::TextDisabled("building %d more...", building);
    if (dropped > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%d more run%s enabled than will be drawn -- raise "
                           "\"curves at once\" or enable fewer.",
                           dropped, dropped == 1 ? " is" : "s are");
    if (untimed > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%d run%s left out: recovered timing failed its trust "
                           "test, so it has no clock to plot against.",
                           untimed, untimed == 1 ? "" : "s");

    // Say what the filter did. A filter that quietly rewrites the picture is
    // indistinguishable from the data having been that shape all along, and this
    // one moves samples by hundreds of units.
    if (g_wrProfileDespike)
    {
        int spikes = 0;
        for (int i = 0; i < nSeries; i++)
            if (series[i].p)
                spikes += series[i].p->despiked;
        if (spikes > 0)
            ImGui::TextDisabled("%d transient spike%s taken out of %s -- untick "
                                "above to see them", spikes,
                                spikes == 1 ? "" : "s",
                                nSeries == 1 ? "this curve" : "these curves");
    }

    if (nSeries == 0)
    {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Nothing to plot. Enable a run in the Runs tab, or turn on your own "
            "line so there is something of yours to compare against.");
        return;
    }

    // --- ranges -------------------------------------------------------------
    float xMax = 0.0f, eLo = 0.0f, eHi = 0.0f;
    for (int i = 0; i < nSeries; i++)
    {
        const WrProfile *p = series[i].p;
        float x = GX(series[i], p->b[p->n - 1]);
        if (x > xMax) xMax = x;
        if (p->eMin < eLo) eLo = p->eMin;
        if (p->eMax > eHi) eHi = p->eMax;
    }
    if (xMax < 1e-3f) xMax = 1.0f;
    if (eHi - eLo < 1.0f) { eHi += 0.5f; eLo -= 0.5f; }
    float pad = (eHi - eLo) * 0.06f;
    eHi += pad;
    eLo -= pad;

    // --- canvas -------------------------------------------------------------
    // The plot takes two thirds of what is left and the legend the rest, so a
    // maximised window buys a bigger curve rather than a bigger margin. There
    // is no upper clamp on purpose: a taller plot is the whole reason to
    // enlarge the panel, since vertical resolution is what separates a gentle
    // sag from a cliff.
    const float kLeft = 56.0f, kBottom = 20.0f, kTop = 8.0f, kRight = 10.0f;
    float caption = ImGui::GetTextLineHeightWithSpacing() * 2.0f +
                    ImGui::GetStyle().ItemSpacing.y;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float ch = SplitHeight(caption, 0.66f, 140.0f, 80.0f);
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec2 size(avail.x > 240.0f ? avail.x : 240.0f, ch);
    ImGui::InvisibleButton("##plot", size);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 c1(c0.x + size.x, c0.y + size.y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(c0, c1, IM_COL32(14, 14, 18, 255));
    dl->AddRect(c0, c1, IM_COL32(70, 70, 80, 255));

    float px0 = c0.x + kLeft, px1 = c1.x - kRight;
    float py0 = c0.y + kTop,  py1 = c1.y - kBottom;
    if (px1 - px0 < 40.0f || py1 - py0 < 40.0f)
        return;

    #define GPX(x) (px0 + (px1 - px0) * ((x) / xMax))
    #define GPY(e) (py1 - (py1 - py0) * (((e) - eLo) / (eHi - eLo)))

    dl->PushClipRect(c0, c1, true);

    // grid
    char buf[64];
    float eStep = GNiceStep(eHi - eLo);
    for (float e = ceilf(eLo / eStep) * eStep; e <= eHi; e += eStep)
    {
        float y = GPY(e);
        bool zero = (e > -eStep * 0.01f && e < eStep * 0.01f);
        dl->AddLine(ImVec2(px0, y), ImVec2(px1, y),
                    zero ? IM_COL32(110, 110, 125, 255) : IM_COL32(42, 42, 50, 255));
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0f", e);
        ImVec2 m = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(px0 - 6.0f - m.x, y - m.y * 0.5f),
                    IM_COL32(150, 150, 160, 255), buf);
    }

    float xStep = GNiceStep(xMax);
    for (float x = 0.0f; x <= xMax * 1.0001f; x += xStep)
    {
        float sx = GPX(x);
        dl->AddLine(ImVec2(sx, py0), ImVec2(sx, py1), IM_COL32(42, 42, 50, 255));
        if (g_gNormalise)      _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0f%%", x);
        else if (g_gByTime)    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0fs", x);
        else if (xMax > 4000.0f) _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1fk", x / 1000.0f);
        else                   _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0f", x);
        ImVec2 m = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(sx - m.x * 0.5f, py1 + 4.0f),
                    IM_COL32(150, 150, 160, 255), buf);
    }

    // --- the curves ---------------------------------------------------------
    static ImVec2 line[WR_PROFILE_BUCKETS];
    for (int i = 0; i < nSeries; i++)
    {
        const WrProfile *p = series[i].p;
        unsigned int col = series[i].colour;

        // The band, coalesced to one vertical span per pixel column. Buckets
        // are finer than the screen by design, so this is where the min and max
        // of each bucket actually earn their keep -- a single mid-line would
        // draw straight through a spike that a bucket caught.
        if (g_gBand)
        {
            int lastPx = -99999;
            float lo = 0.0f, hi = 0.0f;
            for (int k = 0; k < p->n; k++)
            {
                int cx = (int)GPX(GX(series[i], p->b[k]));
                if (cx != lastPx)
                {
                    if (lastPx > -99999 && hi - lo > 0.5f)
                        dl->AddLine(ImVec2((float)lastPx, GPY(hi)),
                                    ImVec2((float)lastPx, GPY(lo)),
                                    GAlpha(col, 0.30f));
                    lastPx = cx;
                    lo = p->b[k].eMin;
                    hi = p->b[k].eMax;
                }
                else
                {
                    if (p->b[k].eMin < lo) lo = p->b[k].eMin;
                    if (p->b[k].eMax > hi) hi = p->b[k].eMax;
                }
            }
            if (lastPx > -99999 && hi - lo > 0.5f)
                dl->AddLine(ImVec2((float)lastPx, GPY(hi)),
                            ImVec2((float)lastPx, GPY(lo)), GAlpha(col, 0.30f));
        }

        int n = p->n < WR_PROFILE_BUCKETS ? p->n : WR_PROFILE_BUCKETS;
        for (int k = 0; k < n; k++)
            line[k] = ImVec2(GPX(GX(series[i], p->b[k])), GPY(p->b[k].e));
        dl->AddPolyline(line, n, GAlpha(col, 1.0f), ImDrawFlags_None,
                        series[i].run ? 1.6f : 2.4f);

        // Turning points, mapped from point index straight to bucket. Tops
        // point up, bottoms point down, matching the ticks drawn in the world.
        if (g_gTurns && series[i].run)
        {
            const WrRun *r = series[i].run;
            for (int pass = 0; pass < 2; pass++)
            {
                const int *list = pass ? r->peaks : r->dips;
                int cnt = pass ? r->peakCount : r->dipCount;
                for (int j = 0; j < cnt; j++)
                {
                    int k = (int)(((long long)list[j] * p->n) / r->pointCount);
                    if (k < 0 || k >= p->n)
                        continue;
                    float sx = GPX(GX(series[i], p->b[k]));
                    float sy = GPY(p->b[k].e);
                    float d = pass ? -3.5f : 3.5f;
                    dl->AddTriangleFilled(ImVec2(sx - 3.5f, sy - d),
                                          ImVec2(sx + 3.5f, sy - d),
                                          ImVec2(sx, sy + d),
                                          GAlpha(col, 0.85f));
                }
            }
        }
    }

    // --- hover --------------------------------------------------------------
    if (hovered)
    {
        ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.x >= px0 && mp.x <= px1)
        {
            float xv = xMax * (mp.x - px0) / (px1 - px0);
            dl->AddLine(ImVec2(mp.x, py0), ImVec2(mp.x, py1),
                        IM_COL32(170, 170, 190, 160));

            ImGui::BeginTooltip();
            if (g_gNormalise)   ImGui::Text("%.1f%% of the way", xv);
            else if (g_gByTime) ImGui::Text("%.2f s", xv);
            else                ImGui::Text("%.0f units along", xv);
            ImGui::Separator();

            // Every series at once. Reading one curve at a time is what makes a
            // plot decorative; the comparison is the product.
            for (int i = 0; i < nSeries; i++)
            {
                const WrProfile *p = series[i].p;
                float native = xv;
                if (g_gNormalise)
                {
                    float total = g_gByTime ? p->tTotal : p->dTotal;
                    native = xv * total / 100.0f;
                }
                float e = 0.0f;
                ImGui::PushID(i);
                ImGui::ColorButton("##c",
                                   ImGui::ColorConvertU32ToFloat4(GAlpha(series[i].colour, 1.0f)),
                                   ImGuiColorEditFlags_NoTooltip |
                                   ImGuiColorEditFlags_NoDragDrop,
                                   ImVec2(10.0f, 10.0f));
                ImGui::PopID();
                ImGui::SameLine();
                if (WrProfileAt(p, native, g_gByTime, &e))
                {
                    float dy = GPY(e);
                    dl->AddCircleFilled(ImVec2(mp.x, dy), 3.5f,
                                        GAlpha(series[i].colour, 1.0f));
                    ImGui::Text("%-18s %+7.0f", series[i].name, e);
                }
                else
                {
                    // Past the end of this run, which must read as nothing
                    // rather than as a flat line at its final value.
                    ImGui::TextDisabled("%-18s   --", series[i].name);
                }
            }
            ImGui::EndTooltip();
        }
    }

    dl->PopClipRect();

    // --- legend -------------------------------------------------------------
    //
    // The caption goes above the table rather than below it, so the table can
    // be the last thing in the tab and take whatever height is left.
    ImGui::TextDisabled("Down triangles are ramp bottoms, up are tops. \"end\" is "
                        "where the curve finishes: how much");
    ImGui::TextDisabled("energy that run had thrown away by the time it got there.");

    if (ImGui::BeginTable("##legend", 4, ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupColumn("who", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("track", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("end", ImGuiTableColumnFlags_WidthFixed, 66.0f);
        ImGui::TableSetupColumn("length", ImGuiTableColumnFlags_WidthFixed, 82.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < nSeries; i++)
        {
            const WrProfile *p = series[i].p;
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableNextColumn();
            ImGui::ColorButton("##lc",
                               ImGui::ColorConvertU32ToFloat4(GAlpha(series[i].colour, 1.0f)),
                               ImGuiColorEditFlags_NoTooltip |
                               ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(10.0f, 10.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted(series[i].name);

            ImGui::TableNextColumn();
            if (series[i].run) ImGui::TextUnformatted(WrTrackName(series[i].run));
            else               ImGui::TextDisabled("live");

            ImGui::TableNextColumn();
            ImGui::Text("%+.0f", p->b[p->n - 1].e);

            ImGui::TableNextColumn();
            if (g_gByTime) ImGui::Text("%.1fs", p->tTotal);
            else           ImGui::Text("%.0f u", p->dTotal);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    #undef GPX
    #undef GPY
}

// ---------------------------------------------------------------------------
// Board -- as much of a map's leaderboard as you have asked for
// ---------------------------------------------------------------------------
//
// The fastest runs are the hardest to follow, which is the whole reason this
// exists: a 37-second surf_demise world record is not a line a learner can
// trace, and the 79-second run at rank 9108 is. See wr_board.h for why the
// cache is a window rather than the whole board.

static char g_bMap[72] = {0};
static int g_bMode = 1, g_bTrackType = 0, g_bTrackNum = 1;
static char g_bFilter[64] = {0};
static int g_bFromRank = 1, g_bCount = 50, g_bSpread = 20;
static bool g_bSelect[WR_BOARD_MAX];
static bool g_bWantFocus = false;       // the Maps tab asked to come here
static bool g_bLoaded = false;          // has this map/mode been asked for yet
static bool g_bFriendsOnly = false;
static bool g_bRefilter = false;        // something other than the text changed

// The display order, at file scope because the tick-all buttons work over the
// rows the filter is currently showing rather than the whole board.
static int g_bOrder[WR_BOARD_MAX];
static int g_bShown = 0;

// 64, because the whole selection travels as one "--ranks 5,9,120-140"
// argument on a 2048-byte command line, and because sixty-four demos is
// already a long download. Stated in the UI rather than silently truncating.
#define B_MAX_PICK 64

enum
{
    BCOL_RANK = 1,
    BCOL_TIME,
    BCOL_PLAYER,
    BCOL_DATE,
    BCOL_HAVE,
};

static const ImGuiTableSortSpecs *g_bSpecs = NULL;

static int CompareBoardColumn(const WrBoardRow *a, const WrBoardRow *b, ImGuiID col)
{
    switch (col)
    {
    case BCOL_RANK:
        return a->rank == b->rank ? 0 : (a->rank < b->rank ? -1 : 1);
    case BCOL_TIME:
        return a->time == b->time ? 0 : (a->time < b->time ? -1 : 1);
    case BCOL_PLAYER:
        return _stricmp(a->alias, b->alias);
    case BCOL_DATE:
        return a->dateEpoch == b->dateEpoch ? 0
                                            : (a->dateEpoch < b->dateEpoch ? -1 : 1);
    case BCOL_HAVE:
        return a->have == b->have ? 0 : (a->have ? 1 : -1);
    default:
        return 0;
    }
}

static int __cdecl CompareBoardRows(const void *pa, const void *pb)
{
    const WrBoardRow *a = WrBoardAt(*(const int *)pa);
    const WrBoardRow *b = WrBoardAt(*(const int *)pb);
    if (!a || !b || !g_bSpecs)
        return 0;
    for (int s = 0; s < g_bSpecs->SpecsCount; s++)
    {
        const ImGuiTableColumnSortSpecs *spec = &g_bSpecs->Specs[s];
        int c = CompareBoardColumn(a, b, spec->ColumnUserID);
        if (c != 0)
            return spec->SortDirection == ImGuiSortDirection_Ascending ? c : -c;
    }
    // Rank breaks every tie, so the order cannot wobble between frames.
    return a->rank == b->rank ? 0 : (a->rank < b->rank ? -1 : 1);
}

// Build "5,9,120-140" from the ticked rows. Ranges rather than a flat list
// because a contiguous block is the common case and it is four times shorter.
//
// Stops emitting at B_MAX_PICK but keeps counting, so the caller can say how
// many were ticked against how many will be asked for instead of truncating
// silently.
static int BuildRankArg(char *out, size_t cap, int *picked)
{
    out[0] = '\0';
    size_t used = 0;
    int n = 0, emitted = 0, runStart = -1, runEnd = -1;

    #define FLUSH()                                                            \
        do {                                                                   \
            if (runStart < 0) break;                                           \
            char part[32];                                                     \
            if (runEnd > runStart)                                             \
                _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%d-%d",          \
                            used ? "," : "", runStart, runEnd);                \
            else                                                               \
                _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%d",             \
                            used ? "," : "", runStart);                        \
            if (used + strlen(part) + 1 < cap) {                               \
                strcat_s(out, cap, part);                                      \
                used = strlen(out);                                            \
            }                                                                  \
            runStart = -1;                                                     \
        } while (0)

    // Walked in board order, not display order, so the ranges actually merge.
    for (int i = 0; i < WrBoardCount(); i++)
    {
        if (!g_bSelect[i])
            continue;
        const WrBoardRow *r = WrBoardAt(i);
        if (!r)
            continue;
        n++;
        emitted++;
        if (runStart >= 0 && r->rank == runEnd + 1)
        {
            runEnd = r->rank;
            continue;
        }
        FLUSH();
        runStart = runEnd = r->rank;
    }
    FLUSH();
    #undef FLUSH

    if (picked)
        *picked = n;
    return n;
}

// The ticked places, as an array of ranks.
//
// This used to write a file, because the whole selection travelled as one
// --ranks argument on a 2048-byte command line and "tick all" made a cap absurd
// rather than merely arbitrary. The request carries the array now; whether it
// still has to become a file is the fetcher's problem, not this tab's.
//
// Returns how many were ticked. Fills at most `cap` of them, and there is no
// caller that passes a cap smaller than the board.
static int CollectPicks(int *out, int cap)
{
    int n = 0;
    for (int i = 0; i < WrBoardCount() && n < cap; i++)
    {
        if (!g_bSelect[i])
            continue;
        const WrBoardRow *r = WrBoardAt(i);
        if (!r)
            continue;
        out[n++] = r->rank;
    }
    return n;
}

// Write down the friends we can see. Only this side can: it is inside the game,
// so it has a live ISteamFriends.
//
// It goes through a FILE even though wr_api.cpp -- the only thing that reads it
// -- now runs in this same process and could have been handed the array. That
// is deliberate and it is not the leftover it looks like. The panel documents
// this file to the user, and it is the thing you would want to be able to open
// and read before pressing a button that sends a hundred SteamID64s anywhere.
// An in-memory hand-off would be one less artefact and one less way to check.
static int WriteFriendsFile(void)
{
    WrSteamRefreshFriends();
    int n = WrSteamFriendCount();

    FILE *f = NULL;
    if (fopen_s(&f, WrDataPath("friends.txt"), "w") != 0 || !f)
        return -1;
    fprintf(f, "# WrLines: SteamID64s from your Steam friends list.\n");
    fprintf(f, "# Written here, where you can read it, so the leaderboard "
               "lookup has something to ask about.\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%llu\n", WrSteamFriendAt(i));
    fclose(f);
    return n;
}

static void BoardShow(const char *map, int trackType, int trackNum)
{
    if (map && *map)
        strcpy_s(g_bMap, sizeof(g_bMap), map);
    g_bTrackType = trackType;
    g_bTrackNum = trackNum < 1 ? 1 : trackNum;
    g_bLoaded = false;
    g_bWantFocus = true;
}

// The five fetch buttons on this tab differ only in which window of the board
// they ask for. They used to differ by formatting five different command-line
// fragments, which is the same thing said less clearly.
static void BoardRun(WrBoardFetchMode mode, int fromRank, int count, int spread)
{
    WrExtractRequest req = {WR_JOB_BOARD};
    strncpy_s(req.map, sizeof(req.map), g_bMap, _TRUNCATE);
    req.gamemode = g_bMode;
    req.trackType = g_bTrackType;
    req.trackNum = g_bTrackNum;
    req.boardMode = mode;
    req.fromRank = fromRank;
    req.count = count;
    req.spread = spread;
    WrExtractSubmit(&req);
}

static void DrawBoardTab(void)
{
    // Default to where you are standing, which is almost always the board you
    // want, and is the only sensible answer before anything has been picked.
    if (!g_bMap[0])
    {
        const char *here = WrLevelName();
        // Truncating, not asserting: g_levelName is 128 and this box is 72, and
        // strcpy_s answers an overlong name by killing the process.
        if (here && *here)
            strncpy_s(g_bMap, sizeof(g_bMap), here, _TRUNCATE);
    }

    ImGui::TextWrapped(
        "A map's leaderboard, as much of it as you have asked for. The fastest "
        "runs are the hardest to follow -- a 37-second surf_demise record is "
        "not a line you can trace, and the 79-second run at rank 9108 is.");

    // --- what board ---------------------------------------------------------
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("Map", g_bMap, sizeof(g_bMap)))
        g_bLoaded = false;

    // The default above only fires while the box is EMPTY, so one look at
    // somebody else's board and the way back is retyping your own map name.
    //
    // Always drawn, disabled when it would do nothing, rather than appearing
    // only when it applies: this row continues into the mode and track combos,
    // and a button that comes and goes would slide them sideways under the
    // cursor every time you changed map.
    const char *standingIn = WrLevelName();
    bool canGoBack = standingIn && *standingIn &&
                     _stricmp(g_bMap, standingIn) != 0;
    ImGui::SameLine();
    if (!canGoBack)
        ImGui::BeginDisabled();
    if (ImGui::Button("This map"))
    {
        strncpy_s(g_bMap, sizeof(g_bMap), standingIn, _TRUNCATE);
        g_bLoaded = false;
    }
    if (!canGoBack)
        ImGui::EndDisabled();
    if (canGoBack && ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to %s, where you are standing.", standingIn);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    const char *modeNames[WR_GAMEMODE_COUNT];
    for (int i = 0; i < WR_GAMEMODE_COUNT; i++)
        modeNames[i] = WrGamemodeName(i + 1);
    int modeIdx = g_bMode - 1;
    if (modeIdx < 0 || modeIdx >= WR_GAMEMODE_COUNT)
        modeIdx = 0;
    if (ImGui::Combo("Mode", &modeIdx, modeNames, WR_GAMEMODE_COUNT))
    {
        g_bMode = modeIdx + 1;
        g_bLoaded = false;
    }
    ImGui::SameLine();
    HelpMarker("Momentum gives nearly every map a leaderboard in nearly every "
               "mode -- all 546 surf maps in the local catalogue list twelve of "
               "them -- and most of those boards are empty. So the mode cannot "
               "be worked out from the map and has to be picked. If a fetch "
               "comes back with no runs, this is the first thing to check.");

    ImGui::SetNextItemWidth(120.0f);
    const char *kTracks[3] = { "main", "stage", "bonus" };
    if (ImGui::Combo("Track", &g_bTrackType, kTracks, 3))
        g_bLoaded = false;
    if (g_bTrackType != 0)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::InputInt("number", &g_bTrackNum))
            g_bLoaded = false;
        if (g_bTrackNum < 1)
            g_bTrackNum = 1;
    }

    // A fetch that has just finished has rewritten the cache, so re-read it.
    // Watched here rather than in dllmain because the extract slot is shared.
    //
    // On the generation counter rather than on WrExtractRunning() going false,
    // which had two holes: a job that started and ended between two frames was
    // never seen at all, and an EXTRACTION finishing threw away this tab's cache
    // for no reason. A counter cannot miss an edge and the kind says whose it
    // was -- BOARD writes the .tsv, FETCH marks rows as held.
    static unsigned int lastGen = 0;
    unsigned int gen = WrExtractRunGeneration();
    if (gen != lastGen)
    {
        lastGen = gen;
        WrJobKind kind = WrExtractLastKind();
        if (kind == WR_JOB_BOARD || kind == WR_JOB_FETCH)
            g_bLoaded = false;
    }

    if (!g_bLoaded && g_bMap[0] && !WrBoardBusy())
    {
        WrBoardLoad(g_bMap, g_bMode, g_bTrackType, g_bTrackNum);
        memset(g_bSelect, 0, sizeof(g_bSelect));
        g_bLoaded = true;
    }

    ImGui::TextDisabled("%s", WrBoardStatus());
    long long fetched = WrBoardFetched();
    if (fetched > 0)
    {
        long long age = (long long)time(NULL) - fetched;
        if (age < 0) age = 0;
        if (age < 90)        ImGui::TextDisabled("fetched %lld seconds ago", age);
        else if (age < 5400) ImGui::TextDisabled("fetched %lld minutes ago", age / 60);
        else if (age < 172800) ImGui::TextDisabled("fetched %lld hours ago", age / 3600);
        else                 ImGui::TextDisabled("fetched %lld days ago", age / 86400);
        ImGui::SameLine();
        HelpMarker("Ranks move as new runs land, so a cached board drifts. The "
                   "run itself does not -- it is keyed on its replay hash, so a "
                   "re-fetch updates the rank rather than duplicating the row.");
    }

    // --- fetching a window --------------------------------------------------
    ImGui::SeparatorText("Fetch part of the board");
    ImGui::Checkbox("Allow downloading", &g_fetchEnabled);
    ImGui::SameLine();
    HelpMarker("The same switch as in the Maps tab -- one setting, offered in "
               "both places so neither tab is a dead end.\n\n"
               "Nothing here reaches the network until it is on, and then only "
               "when a button is pressed. One request at a time, with a pause "
               "between them: this is free community infrastructure.\n\n"
               "Everything here is done inside the DLL and needs no Python -- "
               "reading the board, and downloading the demos too.");
    if (g_fetchEnabled)
    {
        bool busy = WrExtractRunning();
        if (busy)
            ImGui::BeginDisabled();

        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("from rank", &g_bFromRank);
        if (g_bFromRank < 1) g_bFromRank = 1;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("places", &g_bCount);
        if (g_bCount < 1) g_bCount = 1;
        if (g_bCount > 500) g_bCount = 500;

        // The cost, before it is paid. The API caps a page at 100.
        int reqs = (g_bCount + 99) / 100;
        char lbl[64];
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Fastest %d##fast", g_bCount);
        if (ImGui::Button(lbl))
            BoardRun(WR_BOARD_WINDOW, 1, g_bCount, 0);
        ImGui::SameLine();
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Slowest %d##slow", g_bCount);
        if (ImGui::Button(lbl))
            BoardRun(WR_BOARD_SLOWEST, 0, g_bCount, 0);
        ImGui::SameLine();
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "From %d##from", g_bFromRank);
        if (ImGui::Button(lbl))
            BoardRun(WR_BOARD_WINDOW, g_bFromRank, g_bCount, 0);
        ImGui::SameLine();
        ImGui::TextDisabled("%d request%s", reqs, reqs == 1 ? "" : "s");

        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("samples", &g_bSpread);
        if (g_bSpread < 2) g_bSpread = 2;
        if (g_bSpread > 100) g_bSpread = 100;
        ImGui::SameLine();
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Spread %d##spread", g_bSpread);
        if (ImGui::Button(lbl))
            BoardRun(WR_BOARD_SPREAD, 0, 0, g_bSpread);
        ImGui::SameLine();
        ImGui::TextDisabled("%d requests", g_bSpread);
        ImGui::SameLine();
        HelpMarker("Samples places evenly across the WHOLE board, one request "
                   "each. The cheap way to see the shape of a seventeen-"
                   "thousand-run leaderboard: twenty requests gives you a fast "
                   "one, a mid one and a slow one to lay over each other, where "
                   "caching the same board in full costs a hundred and seventy.");

        // --- friends --------------------------------------------------------
        if (!WrSteamCanListFriends())
        {
            ImGui::TextDisabled("Steam is not answering, so your friends list "
                                "cannot be read.");
        }
        else if (ImGui::Button("My friends' runs"))
        {
            int n = WriteFriendsFile();
            if (n > 0)
                BoardRun(WR_BOARD_FRIENDS, 0, 0, 0);
            else
                WrLogf("[!] board: no friends to look up (%d)", n);
        }
        ImGui::SameLine();
        HelpMarker(
            "Everyone on your Steam friends list who has a run on this track, "
            "at their real rank -- so a friend sitting at rank 14,491 of "
            "17,001 is found without caching the 14,490 runs above them.\n\n"
            "Momentum's own leaderboard has a friends filter and it answers 401 "
            "without an account, which is why the site does not offer you this. "
            "Asking for specific SteamID64s is not gated at all. Your friends "
            "are enumerated here, inside the game, because only this side has a "
            "live Steam connection -- and the list is written to "
            "wrlines_data\\friends.txt where you can read it before pressing "
            "this.\n\n"
            "One request per hundred friends. Friends with no run on the map "
            "cost nothing; they simply come back absent.");

        if (busy)
            ImGui::EndDisabled();

        ImGui::TextDisabled("Each press adds to what is cached -- fetch the top,");
        ImGui::TextDisabled("then the slowest, and both stay.");
    }

    // --- the table ----------------------------------------------------------
    ImGui::SeparatorText("Places");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##bfilter", "filter by player", g_bFilter,
                             sizeof(g_bFilter));
    ImGui::SameLine();
    if (ImGui::Checkbox("friends only", &g_bFriendsOnly))
        g_bRefilter = true;
    ImGui::SameLine();
    HelpMarker("Filters what is already cached down to your Steam friends. "
               "Costs nothing and asks nothing -- the board rows carry each "
               "runner's SteamID64 and your friends list is read locally.\n\n"
               "It can only show friends whose runs are IN the cache, so pair "
               "it with the \"My friends' runs\" button, which goes and gets "
               "exactly them.");

    int picked = 0;
    char ranks[600];
    BuildRankArg(ranks, sizeof(ranks), &picked);

    // Tick and untick over the FILTERED set, which is what makes them useful:
    // filter to a player, or to your friends, then take the lot.
    if (ImGui::SmallButton("tick all"))
    {
        for (int k = 0; k < g_bShown; k++)
        {
            const WrBoardRow *r = WrBoardAt(g_bOrder[k]);
            if (r && !r->have)
                g_bSelect[g_bOrder[k]] = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("tick none"))
        memset(g_bSelect, 0, sizeof(g_bSelect));
    ImGui::SameLine();
    HelpMarker("Over the rows the filter is currently showing, not the whole "
               "board. Runs you already hold are left alone -- they have no "
               "tick box, because downloading them again would do nothing.");
    ImGui::SameLine();

    if (picked > 0)
    {
        bool busy = WrExtractRunning() || !g_fetchEnabled;
        if (busy)
            ImGui::BeginDisabled();
        char lbl[64];
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Download %d ticked##dl", picked);
        if (ImGui::Button(lbl))
        {
            // The whole selection, however big. It used to have to become a file
            // on the way out because it travelled on a command line; the request
            // carries the array itself now.
            int *ranks = (int *)malloc(sizeof(int) * (size_t)picked);
            if (ranks)
            {
                WrExtractRequest req = {WR_JOB_FETCH};
                strncpy_s(req.map, sizeof(req.map), g_bMap, _TRUNCATE);
                req.gamemode = g_bMode;
                req.trackType = g_bTrackType;
                req.trackNum = g_bTrackNum;
                req.intoGame = g_intoGame;
                req.ranks = ranks;
                req.rankCount = CollectPicks(ranks, picked);
                if (req.rankCount > 0)
                    WrExtractSubmit(&req);
                free(ranks);
            }
        }
        if (busy)
            ImGui::EndDisabled();
        ImGui::SameLine();
        HelpMarker("Downloads straight from the cached board, which holds the "
                   "download URL the server itself handed back -- so this costs "
                   "no leaderboard requests at all, only the demo bodies.\n\n"
                   "Anything you already hold is skipped by hash, so pressing "
                   "it twice costs nothing.");
    }
    else
    {
        ImGui::TextDisabled("tick rows to download them");
    }

    // The display order is built and sorted only when something has actually
    // changed. A cached board can be twenty thousand rows, and re-filtering and
    // re-sorting all of them every frame for a table nobody is touching would
    // be the most expensive thing in the panel.
    static char lastFilter[64] = {0};
    static int lastCount = -1;
    static long long lastFetched = -1;
    bool needSort = false;

    if (g_bRefilter || strcmp(lastFilter, g_bFilter) != 0 ||
        lastCount != WrBoardCount() || lastFetched != WrBoardFetched())
    {
        // A reload can move every index, so a selection made against the old
        // one has to go rather than silently pointing at other runs. A mere
        // filter change must NOT clear it, or ticking across two filters is
        // impossible.
        if (lastCount != WrBoardCount() || lastFetched != WrBoardFetched())
            memset(g_bSelect, 0, sizeof(g_bSelect));

        g_bShown = 0;
        for (int i = 0; i < WrBoardCount(); i++)
        {
            const WrBoardRow *r = WrBoardAt(i);
            if (!r)
                continue;
            if (g_bFilter[0] && !StrIContains(r->alias, g_bFilter))
                continue;
            if (g_bFriendsOnly && !WrSteamIsFriend(r->steamId))
                continue;
            g_bOrder[g_bShown++] = i;
        }
        strcpy_s(lastFilter, sizeof(lastFilter), g_bFilter);
        lastCount = WrBoardCount();
        lastFetched = WrBoardFetched();
        g_bRefilter = false;
        needSort = true;
    }
    int *order = g_bOrder;
    int shown = g_bShown;

    DrawIntoGameLine(WrBoardMap(), WrBoardMapId(), WR_SEND_TAB_BOARD);

    bool showOut = WrExtractRunning() || WrExtractLineCount() > 0;
    float tableH = ListHeightAbove(showOut);

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_SortMulti;
    if (ImGui::BeginTable("##board", 7, flags, ImVec2(0.0f, tableH)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_NoSort, 22.0f);
        ImGui::TableSetupColumn("rank", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_DefaultSort,
                                58.0f, BCOL_RANK);
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed,
                                82.0f, BCOL_TIME);
        ImGui::TableSetupColumn("player", ImGuiTableColumnFlags_WidthStretch,
                                0.0f, BCOL_PLAYER);
        ImGui::TableSetupColumn("set", ImGuiTableColumnFlags_WidthFixed,
                                82.0f, BCOL_DATE);
        ImGui::TableSetupColumn("have", ImGuiTableColumnFlags_WidthFixed,
                                44.0f, BCOL_HAVE);
        // Wide enough for ALL THREE buttons. It was 66 when the cell held one,
        // and an ImGui table clips its cells, so "local" was drawn cut in half
        // and only the visible part of it was clickable -- on the tab where
        // trying it is the natural thing to do.
        ImGui::TableSetupColumn("watch", ImGuiTableColumnFlags_WidthFixed |
                                         ImGuiTableColumnFlags_NoSort, 168.0f);
        ImGui::TableHeadersRow();

        ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs();
        if (specs && specs->SpecsCount > 0 && shown > 1 &&
            (specs->SpecsDirty || needSort))
        {
            g_bSpecs = specs;
            qsort(order, (size_t)shown, sizeof(int), CompareBoardRows);
            g_bSpecs = NULL;
            specs->SpecsDirty = false;
        }

        // A clipper, because a cached board can be twenty thousand rows and
        // only the visible forty cost anything.
        ImGuiListClipper clipper;
        clipper.Begin(shown);
        while (clipper.Step())
        {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; k++)
            {
                int i = order[k];
                const WrBoardRow *r = WrBoardAt(i);
                if (!r)
                    continue;

                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                bool sel = g_bSelect[i];
                if (r->have)
                    ImGui::TextDisabled("-");
                else if (ImGui::Checkbox("##pick", &sel))
                    g_bSelect[i] = sel;

                ImGui::TableNextColumn();
                ImGui::Text("%d", r->rank);

                ImGui::TableNextColumn();
                char t[32];
                FormatTime(r->time, t, sizeof(t));
                ImGui::TextUnformatted(t);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r->alias);

                ImGui::TableNextColumn();
                if (r->dateEpoch > 0)
                {
                    time_t tt = (time_t)r->dateEpoch;
                    struct tm tmv;
                    if (gmtime_s(&tmv, &tt) == 0)
                        ImGui::TextDisabled("%04d-%02d-%02d", tmv.tm_year + 1900,
                                            tmv.tm_mon + 1, tmv.tm_mday);
                    else
                        ImGui::TextDisabled("-");
                }
                else
                {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableNextColumn();
                if (r->have)
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "yes");
                else
                    ImGui::TextDisabled("no");

                // Only offered for runs already on disk. Downloading from here
                // would need the fetcher, which is a whole different button and
                // already sitting above this table.
                ImGui::TableNextColumn();
                int bMapId = WrBoardMapId();
                if (!r->have)
                    ImGui::TextDisabled("-");
                else
                {
                    // The path-based command works here for exactly the reason
                    // it works in Runs, and it needs no map id -- the local tree
                    // is named by the map.
                    if (ImGui::SmallButton("watch"))
                        WrWatchCmdFromRow(WrBoardMap(), bMapId, r->hash,
                                          r->alias, WR_SEND_TAB_BOARD);
                    ImGui::SameLine();

                    if (WrIntoGameMine(bMapId, r->hash))
                    {
                        if (ImGui::SmallButton("take out"))
                        {
                            WrIntoGameRemoveOne(bMapId, r->hash);
                            WrSendForget();
                        }
                    }
                    else
                    {
                        if (bMapId > 0)
                        {
                            if (ImGui::SmallButton("send"))
                                WrSendFromRow(WR_INTO_ONLINE, WrBoardMap(),
                                              bMapId, r->hash, r->alias,
                                              WR_SEND_TAB_BOARD);
                            ImGui::SameLine();
                        }
                        if (ImGui::SmallButton("local"))
                            WrSendFromRow(WR_INTO_LOCAL, WrBoardMap(), bMapId,
                                          r->hash, r->alias, WR_SEND_TAB_BOARD);
                    }
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (showOut)
    {
        ImGui::SeparatorText("Output");
        if (ImGui::BeginChild("##boardout", ImVec2(0.0f, WR_OUTPUT_HEIGHT),
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            int nLines = WrExtractLineCount();     // hoisted; see ##fetchout
            for (int i = 0; i < nLines; i++)
                ImGui::TextUnformatted(WrExtractLine(i));
            if (WrExtractRunning())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
}

static void DrawEnergyTab(void)
{
    ImGui::TextWrapped(
        "Energy is how high you could still get if you redirected everything "
        "you have straight up, measured from the anchor:  E = height above the "
        "anchor + v squared / 2g.  Standing on the anchor it reads 0. It says "
        "what horizontal speed alone cannot -- whether trading height for speed "
        "on a ramp actually gained you anything.");
    ImGui::TextWrapped(
        "The second figure is the same energy written as a speed, so it reads in "
        "the units you are used to.");
    ImGui::TextWrapped(
        "Negative is energy you have WASTED, not height you have dropped. "
        "Falling converts height into speed at exactly the rate this figure is "
        "defined to hold constant, so a clean drop off the start pad does not "
        "move it at all -- it reads about 0 the whole way down, and that is the "
        "metric working rather than failing. What makes it move is speed "
        "changing by more or less than the height change accounts for: air "
        "strafing adds, a bad ramp entry or a wall takes away.");
    ImGui::TextWrapped(
        "Which is also why the map's own shape cancels out. Drop 18000 units "
        "down a surf map, convert every unit of it into speed, and this still "
        "reads 0. On a descending map everyone ends up negative, so the figure "
        "to watch is not this one climbing -- it is the GAP against the run you "
        "are chasing, below.");
    ImGui::TextDisabled(
        "Measured: across 51 surf_demise records, run time against this figure "
        "at the finish correlates -0.976. The fastest run wastes the least.");

    ImGui::SeparatorText("Now");
    if (!WrEnergyValid())
    {
        ImGui::TextDisabled("Waiting for a camera. Load into a map and move.");
    }
    else
    {
        ImGui::Text("energy        %.0f   (%.0f u/s)",
                    WrEnergyRelative(), WrEnergyEquivSpeed());
        ImGui::Text("speed         %.0f   (horizontal %.0f)",
                    WrEnergySpeed(), WrEnergyHorizontalSpeed());
        ImGui::Text("peak energy   %.0f", WrEnergyPeak());
        if (WrEnergyHaveRef())
            ImGui::Text("measured from z %.0f%s", WrEnergyRefZ(),
                        WrEnergyOnGround() ? "   (on ground now)" : "");
        else
            ImGui::TextDisabled(
                "no anchor -- measured from where you are, so it reads 0 by "
                "construction. Enable a run, or press Anchor here.");
        if (WrEnergyHaveGround())
            ImGui::Text("last jump     z %.0f   (since: %+.0f)",
                        WrEnergyGroundZ(), WrEnergySinceGround());
        else
            ImGui::TextDisabled("last jump     (jump from flat ground to set it)");
        ImGui::Text("since start   %+.0f", WrEnergySinceStart());
        ImGui::TextDisabled("absolute %.0f   trend %+.0f   on ground: %s",
                            WrEnergyNow(), WrEnergyTrend(),
                            WrEnergyOnGround() ? "yes" : "no");

        const float *hist = NULL;
        int n = WrEnergyHistory(&hist);
        if (n > 4)
            ImGui::PlotLines("##energyhist", hist, n, 0, NULL, FLT_MAX, FLT_MAX,
                             ImVec2(0.0f, 60.0f));
    }

    // --- the budget ---------------------------------------------------------
    ImGui::SeparatorText("Budget");
    WrEnergyBudget bud;
    if (!WrEnergyBudgetNow(&bud))
    {
        ImGui::TextDisabled("Needs an anchor to measure from.");
    }
    else
    {
        ImGui::Text("spent   %8.0f   height cashed in since the anchor", bud.spent);
        ImGui::Text("banked  %8.0f   what you still have, as a height", bud.banked);
        ImGui::Text("wasted  %8.0f   the difference", bud.wasted);
        if (bud.carriedValid)
            ImGui::TextColored(bud.carried >= 80.0f
                                   ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                   : ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                               "carried %7.0f%%  of the drop kept as speed",
                               bud.carried);
        else
            ImGui::TextDisabled("carried       --   (needs 500 units of drop first)");
        ImGui::SameLine();
        HelpMarker(
            "The same information as the energy figure above, with the big "
            "numbers going up instead of down.\n\n"
            "It is an identity, not a second measurement. With K = v^2/2g and "
            "H the height you have dropped, K - K_start = H + energy, so what "
            "you are carrying is exactly what you spent plus what you netted. "
            "'wasted' is the energy figure with its sign flipped -- the same "
            "number, to the last digit.\n\n"
            "Over 100% is not a bug. It means air strafing put in more than the "
            "map gave you, which happens wherever a map climbs -- the fastest "
            "surf_utopia run finishes at 293%.\n\n"
            "'spent' is not clamped to only go up, because maps go up too: "
            "measured median backtrack from the running maximum is 1,465 units "
            "on surf_demise and 31,160 on surf_vacant. Clamping would hide that "
            "and break the identity.\n\n"
            "Measured on 51 surf_demise records, 'carried' at the finish "
            "correlates -0.978 with run time.");

        ImGui::Text("gained  %8.0f   lost %.0f", WrEnergyGained(), WrEnergyLost());
        ImGui::SameLine();
        HelpMarker(
            "Gross energy added and thrown away, counted separately -- the "
            "number that only rises when your strafing works.\n\n"
            "A swing is banked only once the figure has come back by 50 units, "
            "so an excursion smaller than that contributes exactly zero. That "
            "matters more than it sounds: simply adding up every rise reads in "
            "the THOUSANDS on a path where energy is exactly constant, and it "
            "reads the same at 60 fps as at 500, so a frame-rate check calls "
            "that noise stable. tests\\test_energy.cpp runs both.\n\n"
            "Read it as a threshold, not a score. Median gained across 50 "
            "surf_demise runs, by quartile of run time: 2596, 979, 316, 197. "
            "Half the runs on that map gain under 500 in total, and every one "
            "of those is slower than 38.7 s.");
        if (WrEnergyBudgetSpliced())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                               "totals span a save-loc load");
    }

    ImGui::Spacing();
    const char *kModes[WR_HUD_MODE_COUNT] = {
        "net energy", "carried %", "spent / banked", "gained / lost",
        "strafe quality"
    };
    if (g_energy.hudMode < 0 || g_energy.hudMode >= WR_HUD_MODE_COUNT)
        g_energy.hudMode = 0;
    ImGui::Combo("Crosshair shows", &g_energy.hudMode, kModes,
                 WR_HUD_MODE_COUNT);

    if (g_energy.hudMode == WR_HUD_STRAFE)
    {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Gauge window", &g_energy.gaugeSeconds, 0.5f, 5.0f,
                           "%.1f s", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        HelpMarker(
            "How close your strafing is to the most air acceleration could "
            "physically add: +100% is the ceiling, 0 is free flight, negative is "
            "energy being destroyed. The same scale, and the same red and green, "
            "as the demo line colours -- so the two can never disagree.\n\n"
            "IT IS NOT A TURN-RATE METER, AND IT DOES NOT NEED THE RAMP'S ANGLE. "
            "Turn rate was the first attempt at this and it fires on a tenth to "
            "a quarter of the samples of RECORD-CLASS runs, because a ramp turns "
            "your velocity through its surface normal far faster than air "
            "acceleration ever can. What is measured instead is the consequence "
            "-- dE/dt -- against a ceiling of 37.5 energy units a second, which "
            "is the same at 500 u/s as at 3500 and the same on every angle of "
            "ramp. There is nothing about the geometry left to compensate for.\n\n"
            "Nor does the deadstrafe period need compensating at these settings. "
            "Source quarters your surface friction while you are airborne rising "
            "SLOWER than 140 u/s over a ramp, but that only bites if it drops "
            "acceleration below the 30 u/s wishspeed cap: at air accelerate 150 "
            "it goes 562 to 141, still miles above it. Measured on the demos "
            "here, p95 of energy gain is 38.07 inside that window across 69,916 "
            "samples and 37.99 outside it across 795,096.\n\n"
            "WHY THE WINDOW IS SO LONG. Your velocity is estimated by "
            "differencing camera positions, and against a 37-unit ceiling that "
            "estimate is coarse. Simulated against twelve real runs: at 0.25 s "
            "the reading agrees with the truth 45% of the time and points the "
            "WRONG WAY 26% of the time; at 0.40 s, 58% and 24%; at 2 s, 81.5% "
            "and 8.5%. Airborne only -- which is where it means strafing rather "
            "than a ramp collision -- 0.40 s is 45% and 32%. So this is one slow "
            "rolling number and your own drawn line stays uncoloured. Demo lines "
            "do not have the problem: their velocity is what Momentum recorded.\n\n"
            "Read it as a rolling average, never as a verdict on this instant.");
    }

    KeyBindCombo("Next mode", &g_hudCycleKey);
    KeyBindCombo("Previous mode", &g_hudCycleBackKey);
    ImGui::SameLine();
    HelpMarker("Page Down and Page Up switch the box's mode without opening this "
               "panel, which is the only way it is useful mid-run. Two keys "
               "rather than one, because there are five modes now and cycling "
               "forward past the one you wanted means four more presses.\n\n"
               "A list rather than a fixed key, because WrLines cannot see what "
               "you have bound. The key is read, not swallowed -- if it collides "
               "with something, the game still acts on it, so set this to (none) "
               "and use the box above instead.");
    KeyBindCombo("Corner block", &g_overlayToggleKey);
    ImGui::SameLine();
    HelpMarker("Turns the corner block off and on. It is off by default, so this "
               "is how you get it without opening the panel.");

    KeyBindCombo("Quick menu", &g_quickKey);
    ImGui::SameLine();
    HelpMarker("Opens the one-page panel: the top runs of each leg of the map "
               "you are on, with a tick box that downloads, extracts and draws "
               "each one for you.\n\n"
               "It is the short way round everything this panel can do at "
               "length. Both can be open at once, and Escape closes both.");

    // --- the anchor ---------------------------------------------------------
    ImGui::SeparatorText("Anchor");
    WrAnchorSource src = WrEnergyAnchorSource();
    if (src == WR_ANCHOR_RUN_START)
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                           "the start of the run you are chasing");
    else if (src == WR_ANCHOR_START_ZONE)
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                           "the start zone, fitted to every run on this leg");
    else if (src == WR_ANCHOR_MANUAL)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "set by hand");
    else
        ImGui::TextDisabled("none yet -- enable a run, or set one here");
    ImGui::SameLine();
    HelpMarker("Everything on this tab is measured from the anchor, and it never "
               "moves on its own.\n\n"
               "It used to: the reference was re-armed whenever a ground test "
               "passed, and that test passes at the APEX OF EVERY JUMP -- "
               "vertical speed sits near zero there for about a quarter of a "
               "second. So the zero point silently re-based itself at the top of "
               "every arc and stepped again on landing. The number was not "
               "noisy; its origin was moving.\n\n"
               "Anchoring at the run's own first point also makes the clock "
               "below exact, because their time and yours then start in the "
               "same place.\n\n"
               "A teleport back to the anchor -- a fail trigger, or the restart "
               "key -- is treated as a new attempt: the clock returns to zero "
               "and the peak is forgotten. The anchor itself stays put. This is "
               "the only way WrLines can see a restart at all, since it reads "
               "the camera and nothing else.");

    if (ImGui::Button("Anchor here"))
        WrEnergyRearm();
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
        WrEnergyReset();
    ImGui::SameLine();
    ImGui::Checkbox("Follow the run's start", &g_energy.anchorToRunStart);

    // --- the start zone -----------------------------------------------------
    ImGui::SeparatorText("Start zone");
    ImGui::Checkbox("Notice when I leave the start", &g_start.enabled);
    ImGui::SameLine();
    HelpMarker(
        "THIS IS NOT READING THE MAPPER'S ZONE. WrLines has no entity access, "
        "no netvars, and no sight of the game's timer -- it knows a camera and "
        "some files. It cannot see a trigger fire and it is not going to.\n\n"
        "What it does instead: every loaded run knows where its own run began, "
        "so a map with two hundred runs carries two hundred independent "
        "observations of where the start is. The circle drawn in the world is "
        "fitted to those, and the bright line across it is where their clocks "
        "actually started.\n\n"
        "The circle only decides when you count as standing in the start. The "
        "moment that fires is crossing the LINE outward, because the recorded "
        "points sit on the way out of the zone rather than in the middle of it "
        "-- firing on leaving the circle would start the clock late by the "
        "radius divided by your speed, which is half a second at 500 u/s.\n\n"
        "It arms only when you are inside, on the ground and slow, so looping "
        "back over the line at speed mid-run cannot re-fire it.");
    if (g_start.enabled)
    {
        ImGui::Checkbox("Re-anchor", &g_start.autoAnchor);
        ImGui::SameLine();
        ImGui::Checkbox("Zero the clock", &g_start.autoZeroClock);
        ImGui::SameLine();
        ImGui::Checkbox("Draw it", &g_start.showZone);

        ImGui::Text("state: ");
        ImGui::SameLine();
        WrStartState st = WrStartStateNow();
        ImVec4 col = (st == WR_START_ARMED)  ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f)
                   : (st == WR_START_INSIDE) ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                             : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        ImGui::TextColored(col, "%s", WrStartStateName());
        if (WrStartWhyNot()[0])
        {
            ImGui::SameLine();
            ImGui::TextDisabled("-- %s", WrStartWhyNot());
        }
        if (WrStartSince() >= 0.0f)
            ImGui::TextDisabled("last crossing %.1f s ago", WrStartSince());

        ImGui::SliderFloat("Circle size", &g_start.radiusScale, 0.4f, 3.0f, "%.2fx");
        ImGui::SliderFloat("Leaving at least", &g_start.leaveSpeed, 0.0f, 600.0f,
                           "%.0f u/s");
        ImGui::SliderFloat("Standing under", &g_start.stillSpeed, 20.0f, 600.0f,
                           "%.0f u/s");

        if (WrStartZoneCount() == 0)
        {
            ImGui::TextDisabled("No runs loaded, so there is nothing to say where");
            ImGui::TextDisabled("the start is. Nothing will fire, and nothing that");
            ImGui::TextDisabled("worked before this existed has changed.");
        }
        else if (ImGui::BeginTable("##zones", 6,
                                   ImGuiTableFlags_Borders |
                                   ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("leg");
            ImGui::TableSetupColumn("from");
            ImGui::TableSetupColumn("circle");
            ImGui::TableSetupColumn("line good to");
            ImGui::TableSetupColumn("away");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();

            const WrStartZone *here = WrStartZoneHere();
            for (int i = 0; i < WrStartZoneCount(); i++)
            {
                const WrStartZone *z = WrStartZoneAt(i);
                if (!z)
                    continue;
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                if (z == here)
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s",
                                       WrTrackNameOf(z->trackType, z->trackNum));
                else
                    ImGui::TextUnformatted(
                        WrTrackNameOf(z->trackType, z->trackNum));

                ImGui::TableSetColumnIndex(1);
                if (z->approx)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                       "%d runs, none placed", z->members);
                else
                    ImGui::Text("%d of %d runs", z->trusted, z->members);

                ImGui::TableSetColumnIndex(2);
                if (z->radiusCapped)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                       "%.0f capped", WrStartZoneRadius(z));
                else
                    ImGui::Text("%.0f u", WrStartZoneRadius(z));

                ImGui::TableSetColumnIndex(3);
                // The honest error bar on the trigger, in both the unit it was
                // measured in and the one that matters.
                ImGui::Text("+-%.0f u", z->alongSpread);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "How far apart the recorded starts sit along the "
                        "crossing direction. At 1600 u/s that is about %.0f ms "
                        "of uncertainty in when the clock zeroes.",
                        z->alongSpread / 1600.0f * 1000.0f);

                ImGui::TableSetColumnIndex(4);
                {
                    Vec3 cam;
                    if (WrCameraOrigin(&cam))
                    {
                        float dx = cam.x - z->centre.x, dy = cam.y - z->centre.y;
                        ImGui::Text("%.0f u", sqrtf(dx * dx + dy * dy));
                    }
                    else
                        ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(5);
                if (ImGui::SmallButton("anchor here"))
                    WrEnergyAnchorToStartZone(WrStartZoneAnchor(z));

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("There is no matching finish detector, on purpose: a");
        ImGui::TextDisabled("finish is a line crossed once at speed rather than a");
        ImGui::TextDisabled("place you wait in, so the same trick would be far");
        ImGui::TextDisabled("less reliable while looking just as confident.");
    }

    // --- the clock ----------------------------------------------------------
    ImGui::SeparatorText("Time");
    const WrRun *tref = WrEnergyReferenceRun();
    float ours = 0.0f, theirs = 0.0f, delta = 0.0f;
    if (WrTimerDelta(tref, &ours, &theirs, &delta))
    {
        char a[32], b[32];
        FormatTime(ours, a, sizeof(a));
        FormatTime(theirs, b, sizeof(b));
        ImGui::Text("you %s    them %s", a, b);
        ImGui::TextColored(delta <= 0.0f ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f)
                                         : ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                           "%+.2f s", delta);
    }
    else
    {
        if (WrTimerRunning())
        {
            char a[32];
            FormatTime(WrTimerElapsed(), a, sizeof(a));
            ImGui::Text("you %s", a);
        }
        ImGui::TextDisabled("%s", WrTimerWhyNot(tref));
    }
    if (ImGui::Button(WrTimerRunning() ? "Stop" : "Start"))
    {
        if (WrTimerRunning()) WrTimerStop(); else WrTimerStart();
    }
    ImGui::SameLine();
    if (ImGui::Button("Zero"))
        WrTimerZero();
    ImGui::SameLine();
    if (WrTimerManual())
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "started by hand");
    else if (WrTimerRunning())
        ImGui::TextDisabled("running from the anchor");
    else
        ImGui::TextDisabled("leave the anchor, or press Start");

    // --- save-locs ----------------------------------------------------------
    ImGui::SeparatorText("Save-locs");
    ImGui::TextDisabled("%s", WrSavelocStatus());
    ImGui::SameLine();
    HelpMarker("Momentum's save-locs record where you were but not how long it "
               "took to get there -- the file has a \"time\" field and it is "
               "-1 in every one of the 3213 entries on this machine.\n\n"
               "So WrLines keeps its own note, in wrlines_data\\savelocs. Load a "
               "save-loc and the clock goes back to what it said when you made "
               "it, so you can practise a map a section at a time and keep a "
               "running total.\n\n"
               "A time is recorded when a save-loc is CREATED. The first version "
               "of this stamped the nearest untimed save-loc you were standing "
               "near, which meant walking past one timed it -- that produced 111 "
               "stamps on this machine and left surf_hades2 with twenty entries "
               "at the spawn, each written by a different lap. Times from before "
               "that fix are marked with a ? because a good one cannot be told "
               "from a bad one; forget them and re-drive the route.\n\n"
               "That rule leaves every save-loc made before WrLines existed "
               "permanently untimed, and there is no way to work out what those "
               "times were -- guessing from proximity is the bug above. So they "
               "can be typed in: press \"set...\" on a row and enter either "
               "1:02.31 or 62.31. A typed time is marked with a * so it is never "
               "confused with a measured one.\n\n"
               "Energy is a different story. The velocity a save-loc was made at "
               "is in the game's OWN file, for every save-loc ever made, so "
               "loading one starts the readout at the right number instead of "
               "working it out again from scratch. That needs nothing from us "
               "and works on save-locs from years ago.\n\n"
               "Nothing is ever written into the game install.");

    {
        float age = 0.0f;
        const char *recent = WrSavelocRecent(&age);
        if (recent && recent[0] && age < 4.0f)
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", recent);
    }

    // What the last loaded save-loc did to the readout. The velocity comes out
    // of the game's own file, so this fires on save-locs made long before this
    // tool existed -- unlike the time beside it, which only exists if we were
    // running when the save-loc was created.
    {
        WrEnergySeedInfo si;
        if (WrEnergySeedReport(&si))
        {
            if (si.rejected)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                   "last load: the file said %.0f u/s and the "
                                   "first measurement disagreed -- measured it "
                                   "instead", si.seedSpeed);
            else
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                   "last load: started straight at %.0f u/s from "
                                   "the file (measured %+.0f u/s off)",
                                   si.seedSpeed, si.speedErr);
        }
    }

    int nLocs = WrSavelocCount();
    // A re-read can shorten the list under an open edit box -- deleting a
    // save-loc in game renumbers the rest -- and a row index left pointing past
    // the end would sit there invisibly and commit to the wrong entry the next
    // time the list grew.
    if (s_editLoc >= nLocs)
        s_editLoc = -1;
    if (nLocs > 0)
    {
        if (ImGui::BeginTable("##savelocs", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 150.0f)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("split", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("where", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            float prevTime = -1.0f;
            for (int i = 0; i < nLocs; i++)
            {
                WrSavelocRow row;
                if (!WrSavelocAt(i, &row))
                    continue;
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                ImGui::Text("%d", i + 1);

                ImGui::TableNextColumn();
                if (s_editLoc == i)
                {
                    // Typing one in. Accepts either form the panel itself
                    // prints, so a split read off this table can be typed
                    // straight back into it.
                    ImGui::SetNextItemWidth(-1.0f);
                    if (s_editFocus)
                    {
                        ImGui::SetKeyboardFocusHere();
                        s_editFocus = false;
                    }
                    if (ImGui::InputTextWithHint("##settime", "m:ss.xx",
                                                 s_editBuf, sizeof(s_editBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        float secs = 0.0f;
                        if (ParseTime(s_editBuf, &secs))
                            WrSavelocSetTime(i, secs);
                        s_editLoc = -1;
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                    {
                        s_editLoc = -1;
                    }
                }
                else if (row.seconds >= 0.0f)
                {
                    char t[32];
                    FormatTime(row.seconds, t, sizeof(t));
                    // Three provenances, three appearances: stamped when the
                    // save-loc was made, typed in by hand, or inherited from a
                    // sidecar written before the stamping bug was fixed.
                    if (row.suspect)
                        ImGui::TextDisabled("%s ?", t);
                    else if (row.byHand)
                        ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f),
                                           "%s *", t);
                    else
                        ImGui::Text("%s", t);
                    if (row.byHand && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Typed in, not measured.");
                }
                else
                {
                    if (ImGui::SmallButton("set..."))
                    {
                        s_editLoc = i;
                        s_editFocus = true;
                        s_editBuf[0] = '\0';
                    }
                }

                // Splits, so a chain of save-locs reads as a route rather than
                // as a column of absolute times.
                ImGui::TableNextColumn();
                if (row.seconds >= 0.0f && prevTime >= 0.0f &&
                    row.seconds > prevTime)
                    ImGui::TextDisabled("+%.2f", row.seconds - prevTime);
                else
                    ImGui::TextDisabled(" ");
                if (row.seconds >= 0.0f)
                    prevTime = row.seconds;

                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.0f %.0f %.0f", row.pos.x, row.pos.y,
                                    row.pos.z);
                if (row.seconds >= 0.0f)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("forget"))
                        WrSavelocForget(i);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (WrSavelocTimedCount() > 0 && ImGui::Button("Forget all times here"))
            WrSavelocForgetAll();
    }

    // --- the comparison -----------------------------------------------------
    ImGui::SeparatorText("Comparison");
    const WrRun *ref = g_energy.compareToRun ? WrEnergyReferenceRun() : NULL;
    if (!g_energy.compareToRun)
    {
        ImGui::TextDisabled("Off.");
    }
    else if (!ref)
    {
        ImGui::TextDisabled("No enabled run within %.0f units of you.",
                            g_energy.compareRadius);
        ImGui::TextDisabled("Enable one on the leg you are standing in, or raise");
        ImGui::TextDisabled("the radius below.");
    }
    else
    {
        float theirs = WrEnergyOf(ref->points[ref->nearestIndex].pos,
                                  ref->points[ref->nearestIndex].vel);
        float gap = (WrEnergyNow() - g_energy.eyeHeight) - theirs;
        ImGui::Text("%s  (%s)", ref->player[0] ? ref->player : "?",
                    WrTrackName(ref));
        ImGui::Text("their energy here  %.0f   gap %+.0f", theirs, gap);
        ImGui::TextDisabled("their line is %.0f units away, at point %d of %d",
                            ref->nearestDist, ref->nearestIndex, ref->pointCount);
    }

    ImGui::SeparatorText("Crosshair readout");
    ImGui::Checkbox("Show beside the crosshair", &g_energy.showHud);
    ImGui::SameLine();
    HelpMarker("Off by default since v0.9.0, so a fresh install draws lines and "
               "nothing else -- the lines are what people come for, and a box of "
               "numbers appearing at the crosshair before anybody asked for one "
               "is not.\n\n"
               "This is the glanceable readout; the corner block below is the "
               "detailed one and has END to itself. Turn either on here and it "
               "is remembered.");
    ImGui::SetNextItemWidth(150.0f);
    const char *kAlignX[3] = { "left of it", "centred on it", "right of it" };
    if (g_energy.hudAlignX < 0 || g_energy.hudAlignX > 2)
        g_energy.hudAlignX = WR_HUD_LEFT;
    ImGui::Combo("Across", &g_energy.hudAlignX, kAlignX, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    const char *kAnchorY[3] = { "centred", "above", "below" };
    if (g_energy.hudAnchorY < 0 || g_energy.hudAnchorY > 2)
        g_energy.hudAnchorY = WR_HUD_CENTRE_Y;
    ImGui::Combo("Down", &g_energy.hudAnchorY, kAnchorY, 3);
    ImGui::SameLine();
    HelpMarker(
        "Which part of the block the offsets below place, and it used to be "
        "neither of these: alignment was inferred from the sign of Offset X, so "
        "the block could hang left or right of the crosshair but never sit "
        "centred over it.\n\n"
        "\"Above\" and \"below\" pin the edge nearest the crosshair rather than "
        "the block's middle. That matters because the block grows and shrinks -- "
        "the comparison row, the bar and the clock all come and go -- and "
        "centred means every one of those nudges it up or down while you are "
        "trying to read it.");
    ImGui::SliderFloat("Offset X", &g_energy.hudOffsetX, -400.0f, 400.0f, "%.0f px");
    ImGui::SliderFloat("Offset Y", &g_energy.hudOffsetY, -400.0f, 400.0f, "%.0f px");
    ImGui::Checkbox("Show the run clock", &g_energy.showHudClock);
    ImGui::SameLine();
    HelpMarker(
        "The clock lived only in this panel, and the panel is shut whenever you "
        "are actually playing -- so loading a save-loc put the clock back to "
        "what it said when you made it and there was nothing on screen to show "
        "for it. That is why it looked like the feature was missing.\n\n"
        "It turns green for a moment when a save-loc restores it, so the restore "
        "is something you see rather than something you work out afterwards.\n\n"
        "Off is not silent: with the clock hidden, a restore still borrows the "
        "row for about two seconds and then gives it back, so the one moment it "
        "matters is visible without a permanent row for it.\n\n"
        "Grey means the clock is not running. It starts when you leave the "
        "anchor and is not the game's own timer -- WrLines cannot read that.");
    ImGui::Checkbox("Show the speed line", &g_energy.showHudSpeed);
    ImGui::SameLine();
    HelpMarker(
        "The second line of the box, in the default \"net\" mode: your energy "
        "written out as a speed.\n\n"
        "Off, because it is the number above it in different units and nothing "
        "more -- two ways of saying one thing, on a readout you glance at "
        "mid-ramp.\n\n"
        "Only that mode. The other three put figures on that line which appear "
        "nowhere else, and they are unaffected by this.");
    ImGui::SliderFloat("Text scale", &g_energy.hudScale, 0.6f, 3.0f, "%.2f");
    ImGui::SliderFloat("Box width", &g_energy.hudWidth, 0.0f, 400.0f,
                       g_energy.hudWidth > 0.0f ? "%.0f px" : "automatic");
    ImGui::SameLine();
    HelpMarker(
        "0 means as wide as the rows need, which is a fixed width per mode.\n\n"
        "Either way the compared player's NAME never sets the width -- it is cut "
        "to fit. It used to set it, and that was worse than untidy: the block is "
        "positioned by its own width when centred or right-aligned, so it moved "
        "as you passed between lines, and the lean bar's fill is a fraction of "
        "the width, so the same energy gap drew a longer bar for a player with a "
        "longer name. A bar you read a comparison off cannot have a scale that "
        "depends on somebody's Steam name.\n\n"
        "Raise it if you would rather see the whole name than have the narrowest "
        "box.");
    ImGui::Checkbox("Dark plate behind it", &g_energy.hudBacking);

    ImGui::SeparatorText("Settings");
    ImGui::Checkbox("Also show the corner block", &g_energy.showOverlay);
    ImGui::SameLine();
    HelpMarker("Off by default, and END turns it on and off without opening this "
               "panel.\n\n"
               "There was an edge-padding slider here. It was added for a block "
               "that looked cut off at the bottom of the screen and turned out "
               "not to be, so it is gone again -- the padding is back to the 18 "
               "pixels it always was. Both corner blocks are still clamped "
               "inside the screen, which costs nothing and does nothing at all "
               "unless a block genuinely would not fit.");
    const char *corners[] = { "top left", "top right", "bottom left", "bottom right" };
    ImGui::Combo("Corner", &g_energy.overlayCorner, corners, 4);
    ImGui::Checkbox("Compare against the fastest enabled run nearby",
                    &g_energy.compareToRun);
    ImGui::SliderFloat("Compare radius", &g_energy.compareRadius, 64.0f, 4096.0f,
                       "%.0f");
    ImGui::SameLine();
    HelpMarker("Momentum records a separate demo per stage, so without this the "
               "comparison happily picks a run several thousand units away on "
               "another stage and reports a confident, meaningless number.");
    ImGui::SliderFloat("Eye height", &g_energy.eyeHeight, 0.0f, 96.0f, "%.0f");
    ImGui::SameLine();
    HelpMarker("Only affects the comparison. Their points are the player origin "
               "-- your feet -- and WrLines only knows where your camera is, "
               "which sits about 64 units higher. Stand on the start pad next "
               "to a line's beginning: if the gap sits at a constant offset, "
               "this is the knob that nulls it.");
    ImGui::SliderFloat("Gravity", &g_energy.gravity, 200.0f, 1600.0f, "%.0f");
    ImGui::SliderFloat("Air accelerate", &g_energy.airAccelerate, 5.0f, 1000.0f,
                       "%.0f");
    ImGui::SliderFloat("Max speed", &g_energy.maxSpeed, 100.0f, 500.0f, "%.0f");
    ImGui::SameLine();
    HelpMarker(
        "sv_airaccelerate and sv_maxspeed. Settings, not reads -- WrLines "
        "touches no cvars. They set the ceiling the efficiency colours are "
        "measured against.\n\n"
        "They also decide whether Source's deadstrafe period matters to you. "
        "CategorizePosition quarters your surface friction while you are "
        "airborne rising slower than 140 u/s over a ramp, and AirAccelerate "
        "multiplies by it -- but that only bites if it drops the acceleration "
        "below the 30 u/s wishspeed cap. At air accelerate 150 the quartered "
        "value is still 141, so nothing changes; at CS:GO's 12 it falls to 12 "
        "and the ceiling drops by a third. The crossover is around 32.\n\n"
        "Checked against the demos on disk: the 95th percentile of energy gain "
        "is 38.07 units/s across 69,916 samples inside that window and 37.99 "
        "across 795,096 outside it. No depression, exactly as the arithmetic "
        "predicts for these settings.");
    {
        // The tick is NOT a constant. 482 of the 503 demos on disk are 0.015,
        // but all 21 bhop_futile runs are 0.01 -- so take it from the run being
        // compared against when there is one, and only fall back to the common
        // value when there is not.
        const WrRun *tr = WrEnergyReferenceRun();
        float tick = (tr && tr->tickInterval > 1e-4f) ? tr->tickInterval : 0.015f;
        float full = WrAirPowerCeilingEx(g_energy.gravity, tick,
                                         g_energy.airAccelerate,
                                         g_energy.maxSpeed, 1.0f);
        float dead = WrAirPowerCeilingEx(g_energy.gravity, tick,
                                         g_energy.airAccelerate,
                                         g_energy.maxSpeed, 0.25f);
        if (dead < full - 0.05f)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                               "ceiling %.1f/s at %.0f tick, but %.1f/s while "
                               "rising slowly over a ramp", full, 1.0f / tick,
                               dead);
        else
            ImGui::TextDisabled("ceiling %.1f energy/s at %.0f tick; the "
                                "deadstrafe period does not reduce it here",
                                full, 1.0f / tick);
    }
    ImGui::SeparatorText("Steadiness");

    // Three presets in front of the sliders, because the reading passes through
    // four filters and setting them one at a time means understanding all four.
    // The lag figure is the honest one: half the difference window plus the
    // output time constant.
    float lag = g_energy.velWindowSeconds * 0.5f + g_energy.smoothSeconds;
    ImGui::Text("Response");
    ImGui::SameLine();
    HelpMarker(
        "The readout is filtered four times over before you see it: the velocity "
        "is differenced over a window, that velocity is smoothed, the speed is "
        "smoothed again, and the energy figure is smoothed once more and then "
        "rounded. Only one of those was adjustable, which is why it could still "
        "feel slow with the smoothing slider already at its lowest.\n\n"
        "SNAPPY costs noise, and not in a small way: the velocity comes from a "
        "finite difference of camera positions, so halving the window doubles "
        "the noise in it. It is the right choice for judging a ramp exit and the "
        "wrong one for reading a number while standing still.\n\n"
        "BALANCED is what every version before this one used, exactly.");
    if (ImGui::Button("Snappy"))
    {
        g_energy.velWindowSeconds = 0.020f;
        g_energy.velTau = 0.030f;
        g_energy.speedTau = 0.050f;
        g_energy.smoothSeconds = 0.12f;
        g_energy.quantiseStep = 5.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Balanced"))
    {
        g_energy.velWindowSeconds = 0.040f;
        g_energy.velTau = 0.060f;
        g_energy.speedTau = 0.100f;
        g_energy.smoothSeconds = 0.30f;
        g_energy.quantiseStep = 5.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Smooth"))
    {
        g_energy.velWindowSeconds = 0.080f;
        g_energy.velTau = 0.120f;
        g_energy.speedTau = 0.200f;
        g_energy.smoothSeconds = 0.50f;
        g_energy.quantiseStep = 10.0f;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("about %.0f ms behind", lag * 1000.0f);

    ImGui::SliderFloat("Smoothing", &g_energy.smoothSeconds, 0.05f, 1.0f, "%.2f s");
    ImGui::SameLine();
    HelpMarker("How long the headline figure takes to settle. The signal itself "
               "cannot rise faster than about 37 units a second -- that is the "
               "physical ceiling on what air strafing can add -- so a third of a "
               "second of filtering costs at most a dozen units of lag and "
               "removes almost all of the jitter.");
    ImGui::SliderFloat("Velocity window", &g_energy.velWindowSeconds,
                       0.010f, 0.200f, "%.3f s");
    ImGui::SameLine();
    HelpMarker(
        "The window the velocity is differenced over, and the first thing in the "
        "chain -- everything downstream inherits its lag and its noise.\n\n"
        "It contributes about half its own length to the delay, so 40 ms costs "
        "20 ms. Below about 20 ms a two-unit view bob starts reading as real "
        "movement: over a single frame at 200 fps the same bob would come out as "
        "400 u/s.");
    ImGui::SliderFloat("Velocity smoothing", &g_energy.velTau, 0.005f, 0.400f,
                       "%.3f s");
    ImGui::SliderFloat("Speed smoothing", &g_energy.speedTau, 0.005f, 0.500f,
                       "%.3f s");
    ImGui::SliderFloat("Arrow window", &g_energy.trendSeconds, 0.3f, 2.0f, "%.2f s");
    ImGui::SliderFloat("Arrow dead band", &g_energy.arrowBand, 0.0f, 60.0f, "%.0f");
    ImGui::SameLine();
    HelpMarker("How far the trend has to move before the arrow commits to a "
               "direction at all. Under this it shows nothing, which is the "
               "truthful answer when the change is smaller than the noise.");
    ImGui::SliderFloat("Power window", &g_energy.powerSeconds, 0.05f, 1.5f, "%.2f s");
    ImGui::SameLine();
    HelpMarker("The window dE/dt is measured over, which is what the live "
               "energy-per-second figure reads. A derivative of a noisy signal "
               "is noise, so this one wants to be longer than it feels like it "
               "should.");
    ImGui::SliderFloat("Round to", &g_energy.quantiseStep, 0.0f, 25.0f, "%.0f");
    ImGui::SameLine();
    HelpMarker("Rounds the displayed figure, with hysteresis, so the last digit "
               "stops churning. Zero shows the raw value.\n\n"
               "Worth checking if the number feels sticky rather than slow: this "
               "makes it wait until the value has moved three quarters of a step "
               "before it redraws, which is a different complaint from lag and "
               "has a different fix.");
    ImGui::SeparatorText("Where the comparison is reading");
    ImGui::Checkbox("Ring the point being compared", &g_energy.showComparePoint);
    ImGui::SameLine();
    HelpMarker(
        "Every gap on screen -- the number, the bar -- is your energy against "
        "theirs AT ONE POINT of their line: the point of it nearest you, picked "
        "fresh every frame. This draws a ring exactly there.\n\n"
        "Without it a number that jumps has no explanation. You cannot tell a "
        "real difference from the reference sliding twenty units along a ramp, "
        "or from it having latched onto a different line entirely where two "
        "routes cross -- which happens constantly on a staged map.\n\n"
        "The ring is in the run's own colour, so it also says WHICH run won the "
        "comparison when several are enabled.");
    if (g_energy.showComparePoint)
    {
        ImGui::Checkbox("Draw a thread to it", &g_energy.comparePointLeader);
        ImGui::SameLine();
        HelpMarker("A faint line from you to that point. Worth having when "
                   "several runs are enabled and the ring is off in the "
                   "distance; worth turning off if you find it busy.");
    }

    ImGui::SeparatorText("Comparison bar");
    ImGui::Checkbox("Lean toward whoever is ahead", &g_energy.showBar);
    ImGui::SameLine();
    HelpMarker(
        "A centre-zero bar under the crosshair readout, filling toward you when "
        "you are ahead of the run you are chasing and away when you are not.\n\n"
        "Energy is the default because it discriminates better. Measured over "
        "28,243 matched samples on every map with at least ten clean runs, a "
        "slower run has more energy than the fastest one at 15.6% of points but "
        "more horizontal speed at 20.4% -- speed flatters you more often than "
        "it should.\n\n"
        "The maxima are the 90th percentile of the gap: 701 energy units and "
        "207 u/s. Past the end of the scale an arrowhead appears rather than "
        "the bar just sitting pinned, because the gap reaches 42,000 at the "
        "99th percentile wherever a booster is involved.\n\n"
        "It compares against the fastest ENABLED run near you, so enabling a "
        "mid-pack run flips which way it leans. The line above it names who.");
    if (g_energy.showBar)
    {
        const char *kBar[WR_BAR_MODE_COUNT] = { "energy", "horizontal speed" };
        if (g_energy.barMode < 0 || g_energy.barMode >= WR_BAR_MODE_COUNT)
            g_energy.barMode = 0;
        ImGui::Combo("Bar measures", &g_energy.barMode, kBar, WR_BAR_MODE_COUNT);
        if (g_energy.barMode == WR_BAR_ENERGY)
            ImGui::SliderFloat("Full lean at", &g_energy.barMaxEnergy,
                               100.0f, 3000.0f, "%.0f energy");
        else
            ImGui::SliderFloat("Full lean at", &g_energy.barMaxSpeed,
                               50.0f, 1500.0f, "%.0f u/s");
        ImGui::SliderFloat("Bar height", &g_energy.barHeight, 2.0f, 20.0f,
                           "%.0f px");
    }

    ImGui::SliderFloat("Corner block size", &g_energy.overlayScale, 0.6f, 3.0f, "%.2fx");
    ImGui::TextDisabled("sv_gravity. 800 is the Source default; change it only if");
    ImGui::TextDisabled("the map or gamemode does.");
    if (ImGui::Button("Reset energy settings"))
        WrEnergyDefaults();

    ImGui::SeparatorText("How exact is this?");
    ImGui::TextWrapped(
        "For the loaded runs, exact -- the .wrpath files store a real velocity "
        "per point.");
    ImGui::TextWrapped(
        "For you, approximate. WrLines only knows where the camera is, so your "
        "velocity is differenced from camera motion over a few frames and "
        "smoothed. Expect a few percent of error and a slight lag on sharp "
        "changes. The eye offset cancels out of the energy figure itself, since "
        "the reference height is a camera height too -- it only shows up in the "
        "comparison against a run, which is what the Eye height slider is for.");
}

static const char *SourceName(WrResolveSource s)
{
    switch (s)
    {
    case WR_SRC_HYPOTHESIS: return "probe";
    case WR_SRC_INI:        return "ini";
    case WR_SRC_WIDE:       return "wide";
    default:                return "-";
    }
}

static void DrawDiagnosticsTab(void)
{
    // --- how we are getting the matrix ------------------------------------
    ImGui::SeparatorText("World-to-screen matrix");

    bool haveScan = WrScanResolved();
    WrMatrixSource src = WrMatrixSourceNow();

    if (haveScan)
    {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                           "found by memory scan @ %p%s", WrScanAddress(),
                           WrScanTransposed() ? " (transposed)" : "");
        ImGui::TextDisabled("%s", WrScanNote());
    }
    else if (WrScanBusy())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "scanning...");
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f MB, %d candidate%s so far", WrScanMegabytes(),
                            WrScanCandidateCount(),
                            WrScanCandidateCount() == 1 ? "" : "s");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", WrScanStatus());
    }

    ImGui::Text("in use      %s",
                src == WR_MAT_SCAN   ? "memory scan"
              : src == WR_MAT_VTABLE ? "vtable method"
                                     : "none -- no lines will be drawn");
    ImGui::Text("candidates  %d live / %d found, %d updating, %.0f MB scanned",
                WrScanLiveCandidateCount(), WrScanCandidateCount(),
                WrScanUpdatingCount(), WrScanMegabytes());
    if (haveScan)
        ImGui::Text("travel      %.0f units followed", WrScanTravel());
    else
        ImGui::TextDisabled(
            "A candidate is only accepted once it is seen changing every frame "
            "and\nits camera moves through the map -- walk around for a second.");

    if (ImGui::Button("Re-scan"))
        WrScanRestart();
    ImGui::SameLine();
    ImGui::TextDisabled("read-only; cannot affect the game");

    // --- choosing between them by hand --------------------------------------
    //
    // The reason this exists: more than one matrix per frame is a genuine
    // world->clip matrix for this viewport, and no test available from outside
    // the engine separates the one the world is drawn with from one belonging to
    // another pass through the same eye. Picking wrong looks like the lines
    // being mapped slightly wrong -- correct at the crosshair, drifting towards
    // the edges -- which is easy to mistake for the tool being broken.
    if (WrScanCandidateCount() > 0)
    {
        ImGui::Spacing();
        if (ImGui::TreeNode("candidates", "Every matrix that passes the test (%d)",
                            WrScanLiveCandidateCount()))
        {
            ImGui::TextWrapped(
                "If the lines track the world but are mapped slightly wrong -- "
                "right where you are aiming and increasingly off towards the "
                "edges of the screen -- one of the others is the right one. Try "
                "them: the change is immediate, and Remember keeps it across "
                "restarts.");
            ImGui::Spacing();

            const char *pin = WrScanPinDescription();
            if (pin[0])
            {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                   "remembered: %s", pin);
                ImGui::SameLine();
                if (ImGui::SmallButton("Forget"))
                    WrScanForgetPin();
            }

            ImGuiTableFlags cflags = ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("cands", 7, cflags, ImVec2(0.0f, 180.0f)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Use");
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("FOV");
                ImGui::TableSetupColumn("Aspect");
                ImGui::TableSetupColumn("Travel");
                ImGui::TableSetupColumn("Hits");
                ImGui::TableSetupColumn("Jumps");
                ImGui::TableHeadersRow();

                for (int i = 0; i < WrScanCandidateCount(); i++)
                {
                    WrScanCandidateInfo c;
                    if (!WrScanCandidateAt(i, &c) || !c.alive)
                        continue;
                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    ImGui::TableSetColumnIndex(0);
                    if (c.chosen)
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "in use");
                    else if (ImGui::SmallButton("use"))
                        WrScanUseCandidate(i);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%p%s%s", c.addr, c.transposed ? " T" : "",
                                c.pinned ? " *" : "");

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.0f", c.fov);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.3f", c.aspect);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.0f", c.travel);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%d", c.hits);
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%d", c.jumps);

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (haveScan)
            {
                if (ImGui::Button("Remember this one"))
                {
                    if (!WrScanPinChosen())
                        ImGui::OpenPopup("nopin");
                }
                ImGui::SameLine();
                HelpMarker("Writes it to wrlines_data\\wrlines_matrix.ini as "
                           "module + offset, never as a bare address -- the "
                           "module moves every launch. Next time it is adopted "
                           "in the first second instead of being re-derived, "
                           "which also means the choice cannot silently come "
                           "out differently.\n\n"
                           "It is still validated every frame, so a game update "
                           "that moves it falls back to scanning rather than "
                           "pointing the renderer at nothing.");
                if (ImGui::BeginPopup("nopin"))
                {
                    ImGui::TextWrapped(
                        "That matrix is not inside a loaded module -- it lives on "
                        "the heap, at a different address every launch, so there "
                        "is nothing stable to remember.");
                    ImGui::EndPopup();
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Camera");
    Vec3 camNow;
    if (WrCameraOrigin(&camNow))
        ImGui::Text("origin  %.1f  %.1f  %.1f", camNow.x, camNow.y, camNow.z);
    else
        ImGui::TextDisabled("origin  (no matrix yet)");
    Vec3 fwdNow;
    if (WrCameraForward(&fwdNow))
        ImGui::Text("forward %.3f %.3f %.3f", fwdNow.x, fwdNow.y, fwdNow.z);

    // --- the dangerous fallback -------------------------------------------
    ImGui::SeparatorText("Vtable probing (fallback)");

    bool probing = WrEngineProbingEnabled();
    if (ImGui::Checkbox("Enable vtable probing", &probing))
        WrEngineSetProbing(probing);

    ImGui::TextWrapped(
        "Off by default, and only worth turning on if the scan above found "
        "nothing. Probing calls unknown methods on the live engine object; in "
        "testing that corrupted engine state and killed the game about a second "
        "later, after the call had already returned cleanly. Nothing here can "
        "guard against that, which is why the matrix is read out of memory "
        "instead.");

    if (!probing)
        ImGui::BeginDisabled();

    if (ImGui::BeginTable("methods", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Method");
        ImGui::TableSetupColumn("Index");
        ImGui::TableSetupColumn("VTable");
        ImGui::TableSetupColumn("Via");
        ImGui::TableSetupColumn("Oracle saw", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < WrMethodCount(); i++)
        {
            const WrMethodInfo *m = WrMethodAt(i);
            if (!m)
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (m->resolved)
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", m->name);
            else
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", m->name);
            ImGui::TableSetColumnIndex(1);
            if (m->index >= 0) ImGui::Text("%d", m->index); else ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", m->vtableSize);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(SourceName(m->source));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(m->note[0] ? m->note : "");
        }
        ImGui::EndTable();
    }

    // One index at a time, chosen by a human, is the least-bad way to probe.
    // Anything accepted here still has to pass the same oracle, so a wrong
    // number is rejected rather than believed.
    static int s_manual = 69;
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("##manualidx", &s_manual);
    ImGui::SameLine();
    if (ImGui::Button("Try this index as WorldToScreenMatrix"))
        WrEngineTryIndexFor("WorldToScreenMatrix", s_manual);
    ImGui::TextDisabled("Validated before use. Safer than the sweeps below --");
    ImGui::TextDisabled("it calls exactly one method instead of dozens.");

    ImGui::Spacing();
    if (ImGui::Button("Re-probe"))
        WrEngineRequestReprobe(false);
    ImGui::SameLine();
    if (ImGui::Button("Wide probe"))
        WrEngineRequestReprobe(true);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "expect a crash");
    ImGui::TextDisabled("Wide probe calls every method in the vtable, and some of them");
    ImGui::TextDisabled("are not getters. This is what was crashing the game.");

    if (!probing)
        ImGui::EndDisabled();

    ImGui::SeparatorText("Render");
    int bw = 0, bh = 0;
    WrBackbufferSize(&bw, &bh);
    int segs = 0, pts = 0, batches = 0;
    float ms = 0.0f;
    WrRenderStats(&segs, &pts, &batches, &ms);
    ImGui::Text("backbuffer  %d x %d", bw, bh);
    ImGui::Text("d3d11.dll   %s", WrD3D11Path());
    ImGui::Text("DXVK        %s", WrIsDxvk() ? "yes" : "no");
    if (WrIsWine())
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                           "platform    Wine/Proton -- the supported way to run "
                           "this on Linux");
    else
        ImGui::Text("platform    Windows");
    ImGui::Text("emit        %d segments from %d points in %.2f ms", segs, pts, ms);
    ImGui::Text("batches     %d AddPolyline calls", batches);
    {
        // The pick's real cost, because it was estimated rather than measured
        // when it was written and an estimate that never gets checked is a
        // guess with a number on it.
        int pc = 0, pp = 0;
        float pms = 0.0f;
        WrPickStats(&pc, &pp, &pms);
        ImGui::Text("aim         %d chunks, %d points, %.3f ms", pc, pp, pms);
    }
    ImGui::Text("frame       %.1f fps", ImGui::GetIO().Framerate);

    // --- was the save-loc velocity worth believing? --------------------------
    //
    // The one number that says whether seeding the readout from Momentum's file
    // actually works. Nothing on this side can check that the game applies the
    // velocity its own file records, so this does not claim it does -- it
    // compares every seed against the first velocity measured after it and
    // counts the ones that did not survive.
    {
        WrEnergySeedInfo si;
        if (WrEnergySeedReport(&si))
        {
            ImGui::SeparatorText("Save-loc seed");
            ImGui::Text("file said   %.0f u/s, energy %.0f", si.seedSpeed,
                        si.seedEnergy);
            ImGui::Text("measured    %+.0f u/s, %+.0f units, 35 ms later",
                        si.speedErr, si.energyErr);
            if (si.rejects > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                   "this map    %d seed%s, %d thrown out",
                                   si.seeds, si.seeds == 1 ? "" : "s",
                                   si.rejects);
            else
                ImGui::Text("this map    %d seed%s, none thrown out", si.seeds,
                            si.seeds == 1 ? "" : "s");
            ImGui::SameLine();
            HelpMarker(
                "Loading a save-loc used to leave the readout showing the "
                "energy you had when you FAILED, for as long as it took to "
                "measure a new velocity by differencing the camera -- and held "
                "the banked gain/loss figures for nearly a second on top.\n\n"
                "Momentum's savedlocs.txt records the velocity it is about to "
                "restore, so there is nothing to work out. But a number read "
                "from a file is a claim, and a fast wrong readout is worse than "
                "a slow right one -- so every seed is checked against the first "
                "velocity actually measured after it, about 35 ms later, and "
                "thrown out if the two disagree. A thrown-out seed costs those "
                "35 ms and nothing more.\n\n"
                "A few thrown out is fine -- landing near two save-locs at once "
                "will do it. A lot means the file is not saying what the game "
                "does, and this should be turned off.");
        }
    }

    // --- where the frame actually goes --------------------------------------
    ImGui::SeparatorText("Cost, per frame");
    float total = 0.0f;
    for (int s = 0; s < WR_STAGE_COUNT; s++)
        total += WrStageMillis((WrStage)s);
    for (int s = 0; s < WR_STAGE_COUNT; s++)
        ImGui::Text("%-13s %.3f ms", WrStageName((WrStage)s),
                    WrStageMillis((WrStage)s));
    ImGui::Text("%-13s %.3f ms", "total", total);

    unsigned int drawn = 0, skipped = 0;
    WrFrameCounts(&drawn, &skipped);
    ImGui::Text("frames      %u drawn, %u skipped", drawn, skipped);
    ImGui::SameLine();
    HelpMarker("A skipped frame is one where Present did nothing at all: no "
               "render target bound, no ImGui frame built, no device state "
               "touched.\n\n"
               "It happens whenever nothing would appear -- menus and loading "
               "screens always, and in a map when the panel is closed, no runs "
               "are ticked and the energy readouts are off.\n\n"
               "Note the energy readout is ON by default, so while you are "
               "playing this counter will normally sit still. That is working "
               "as intended: the cost is small and, more importantly, the same "
               "every frame. If another overlay's frame limiter is still "
               "unhappy, turning the energy readout off in the Energy tab makes "
               "Present a complete passthrough.");

    // --- is a world actually being rendered ----------------------------------
    ImGui::SeparatorText("World");
    float frozen = WrScanFrozenSeconds();
    float *frozenLimit = WrScanFrozenLimit();
    if (WrScanHoldingForPanel())
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                           "held  (unchanged for %.1f s, but this panel is open "
                           "-- using the panel means standing still)", frozen);
    else if (frozen > *frozenLimit)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "frozen for %.1f s -- treating this as \"not in a map\", "
                           "drawing nothing", frozen);
    else if (frozen > 0.05f)
        ImGui::Text("live  (unchanged for %.2f s)", frozen);
    else
        ImGui::Text("live");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Freeze cutoff", frozenLimit, 0.3f, 10.0f, "%.1f s");
    ImGui::SameLine();
    HelpMarker("Disconnecting to the main menu does not clear the camera matrix "
               "or stop it looking valid -- it just stops being written. Without "
               "this the whole route stayed drawn over the menu, frozen in "
               "place.\n\n"
               "Standing perfectly still in a map produces an identical matrix "
               "too, so the lines can pause while you are genuinely in the "
               "world. They come back the moment you move. Raise this if that "
               "bothers you.\n\n"
               "It is suspended entirely while this panel is open, since using "
               "the panel means standing still and the lines you are ticking on "
               "and off are the whole point. That hold is only taken if the "
               "matrix was live in the ten seconds before you opened it, so "
               "opening the panel at the main menu still draws nothing.");

    // --- who else is in the Present chain ------------------------------------
    ImGui::SeparatorText("Other overlays");
    ImGui::Text("Present     first bytes %s", WrPresentFirstBytes());
    if (WrPresentPreHooked())
        ImGui::TextWrapped(
            "Something had already hooked Present before we got there -- Steam "
            "overlay, RTSS, SpecialK or similar. That is normal and supported. "
            "If a frame limiter is misbehaving, the 'skipped' counter above is "
            "the thing to watch: while it climbs we are adding nothing to the "
            "frame for it to fight with.");
    else
        ImGui::TextDisabled("Nothing else appears to have hooked Present first.");
    ImGui::Text("window proc %s", WrWndProcInstalled() ? "installed (panel open)"
                                                       : "not installed");
    ImGui::SameLine();
    HelpMarker("The game's window procedure is only replaced while the panel is "
               "open, and restored when it closes -- other overlays watch for "
               "that being held permanently. If something subclasses on top of "
               "us we leave ours in place rather than cut theirs out; the log "
               "says so when that happens.");

    ImGui::SeparatorText("Steam");
    bool steamOn = WrSteamEnabled();
    if (ImGui::Checkbox("Look up names and avatars", &steamOn))
        WrSteamSetEnabled(steamOn);
    ImGui::Text("status      %s", WrSteamStatus());
    ImGui::Text("avatars     %d ready, %d pending",
                WrSteamAvatarCount(), WrSteamPendingCount());
    ImGui::TextWrapped(
        "This is the one thing WrLines does that reaches outside your machine: "
        "it asks the Steam client to fetch each runner's name and picture, the "
        "same lookup a scoreboard does. Turn it off and the tags fall back to "
        "the name stored in the .wrpath and a coloured dot.");

    // Last thing in the tab, so it takes the rest -- but with a floor, because
    // this tab is long enough to scroll and what is "left" can then be nothing.
    ImGui::SeparatorText("Log");
    if (ImGui::BeginChild("log", ImVec2(0.0f, FillHeight(0.0f, 180.0f)),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        int n = WrLogCount();
        for (int i = 0; i < n; i++)
            ImGui::TextUnformatted(WrLogLine(i));
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

static void DrawAboutTab(void)
{
    ImGui::TextWrapped("WrLines " WRLINES_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Draws the path other players' runs took, as a line in the world, in the "
        "map you are currently playing.");

    ImGui::SeparatorText("What it touches");
    ImGui::BulletText("Reads .wrpath files it generated from the game's own demos.");
    ImGui::BulletText("Hooks IDXGISwapChain::Present to draw, and the window proc");
    ImGui::Indent();
    ImGui::TextDisabled("for input while this panel is open.");
    ImGui::Unindent();
    ImGui::BulletText("Reads the world-to-screen matrix out of the game's own");
    ImGui::Indent();
    ImGui::TextDisabled("memory. Read-only -- it never calls into the engine.");
    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::TextWrapped(
        "It sets no cvars, runs no console commands, and never touches "
        "sv_cheats. Its own files all live in wrlines_data next to the DLL.");
    ImGui::TextWrapped(
        "One exception, and it is opt-in: pressing \"send\", \"local\" or "
        "\"watch\" on a run copies that one demo into the game's replay folder "
        "so the game can play it. Every copy is written into "
        "wrlines_data\\into_game.txt first, and nothing outside that list can be "
        "deleted from here -- the demos the game downloaded itself are not "
        "reachable. \"Take out\" removes ours again.");

    ImGui::SeparatorText("What it deliberately doesn't do");
    ImGui::BulletText("It never unloads. Restart the game to update the DLL.");
    ImGui::BulletText("It does not depend on the engine's debug overlay, which");
    ImGui::Indent();
    ImGui::TextDisabled("is gated behind sv_cheats and would taint your runs.");
    ImGui::Unindent();
    ImGui::BulletText("Lines have no depth test -- they draw through walls.");

    ImGui::SeparatorText("Keys");
    ImGui::TextDisabled("Read from the current bindings, not from the defaults,");
    ImGui::TextDisabled("so a key you have changed is shown as it is now.");
    ImGui::Spacing();
    ImGui::BulletText("INSERT  -  show / hide this panel");
    ImGui::BulletText("ESC     -  close this panel");
    ImGui::BulletText("%-7s -  the box at your crosshair: next mode",
                      WrUiKeyName(WrUiHudCycleKey()));
    ImGui::BulletText("%-7s -  ... and the previous one",
                      WrUiKeyName(WrUiHudCycleBackKey()));
    ImGui::BulletText("%-7s -  \"whose line am I looking at\" -- off by default",
                      WrUiKeyName(WrUiPickToggleKey()));
    ImGui::BulletText("%-7s -  the corner block -- off by default",
                      WrUiKeyName(WrUiOverlayToggleKey()));
    ImGui::Spacing();
    ImGui::TextDisabled("All four are rebindable -- the plate's in Display, the");
    ImGui::TextDisabled("other three in Energy. They are READ, never swallowed,");
    ImGui::TextDisabled("so a collision means the game still acts on the key.");

    ImGui::SeparatorText("Settings");
    if (WrSettingsPending())
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                           "%d settings, a change waiting to be written",
                           WrSettingsFieldCount());
    else if (WrSettingsSinceSave() >= 0.0f)
        ImGui::TextDisabled("%d settings, saved %.0f s ago",
                            WrSettingsFieldCount(), WrSettingsSinceSave());
    else
        ImGui::TextDisabled("%d settings, nothing written yet this session",
                            WrSettingsFieldCount());

    if (ImGui::Button("Save now"))
        WrSettingsSave();
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
        WrSettingsLoad();
    ImGui::SameLine();
    if (ImGui::Button("Reset everything"))
        WrSettingsResetAll();
    ImGui::SameLine();
    HelpMarker(
        "Saved by itself, a couple of seconds after you stop changing things, "
        "so a slider being dragged writes once rather than every frame.\n\n"
        "The file is plain text and safe to edit or delete -- deleting it puts "
        "everything back to its defaults. A key this build does not know is "
        "ignored and a missing key keeps its default, so an old file loads into "
        "a new build and the other way round, and every value is clamped to the "
        "range its own slider has.\n\n"
        "Display settings only: no names, no SteamIDs, no map or run data, no "
        "record of what you watched. Safe to paste into a bug report.\n\n"
        "The panel's own position and size are saved beside it, in imgui.ini -- "
        "ours, in our folder, never the game's momentum\\cfg\\imgui.ini.");

    if (WrSettingsUnknownKeys() > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%d key%s in the file this build does not know -- "
                           "written by a different version, and ignored.",
                           WrSettingsUnknownKeys(),
                           WrSettingsUnknownKeys() == 1 ? "" : "s");

    ImGui::SeparatorText("Files");
    ImGui::TextDisabled("%s", WrDataPath(""));
}

// The settings the PANEL owns, registered here rather than in wr_settings.cpp so
// that each one sits in the file that declares the variable. Adding a setting is
// one line, next to the setting; a table maintained somewhere else goes out of
// step the first time somebody adds a field and edits only one of the two.
void WrUiRegisterSettings(void)
{
    WrSettingsInt("key.hudCycleNext", &g_hudCycleKey, 0, 255);
    WrSettingsInt("key.hudCyclePrev", &g_hudCycleBackKey, 0, 255);
    WrSettingsInt("key.pickToggle", &g_pickToggleKey, 0, 255);
    WrSettingsInt("key.overlayToggle", &g_overlayToggleKey, 0, 255);
    WrSettingsInt("key.quickMenu", &g_quickKey, 0, 255);

    WrSettingsBool("graph.byTime", &g_gByTime);
    WrSettingsBool("graph.normalise", &g_gNormalise);
    WrSettingsBool("graph.band", &g_gBand);
    WrSettingsBool("graph.turns", &g_gTurns);
    WrSettingsBool("graph.live", &g_gLive);
    WrSettingsInt("graph.maxSeries", &g_gMaxSeries, 1, G_MAX_SERIES);

    WrSettingsFloat("runs.nearRadius", &s_nearRadius, 256.0f, 65536.0f);
    WrSettingsBool("runs.nearOnly", &s_nearOnly);
}

void WrUiDraw(void)
{
    ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60.0f, 60.0f), ImGuiCond_FirstUseEver);

    bool open = true;
    if (ImGui::Begin("WrLines", &open, ImGuiWindowFlags_NoCollapse))
    {
        if (ImGui::BeginTabBar("tabs"))
        {
            if (ImGui::BeginTabItem("Runs"))       { DrawRunsTab();        ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Maps"))       { DrawMapsTab();        ImGui::EndTabItem(); }
            {
                // The Maps tab's per-row "board" button asks to come here.
                ImGuiTabItemFlags bf = g_bWantFocus ? ImGuiTabItemFlags_SetSelected
                                                    : 0;
                g_bWantFocus = false;
                if (ImGui::BeginTabItem("Board", NULL, bf))
                { DrawBoardTab(); ImGui::EndTabItem(); }
            }
            if (ImGui::BeginTabItem("Display"))    { DrawDisplayTab();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Energy"))     { DrawEnergyTab();      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Graphs"))     { DrawGraphsTab();      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Frame cap"))  { DrawFrameCapTab();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Diagnostics")){ DrawDiagnosticsTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("About"))      { DrawAboutTab();       ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (!open)
        WrSetMenuOpen(false);
}
