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
#include "wr_profile.h"
#include "wr_stress.h"
#include "wr_hook.h"
#include "wr_log.h"

#include "imgui.h"

#include <float.h>
#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_uiMap[72] = {0};

// Which key cycles the crosshair readout. Read by the hotkey thread; a plain
// int, written only from the panel, so no synchronisation is needed beyond the
// atomicity of an aligned 32-bit store.
static int g_hudCycleKey = VK_END;

int WrUiHudCycleKey(void) { return g_hudCycleKey; }

// "Near me" means within this many units of the camera. 4096 comfortably covers
// one stage of a surf map without reaching into the next one.
static float s_nearRadius = 4096.0f;
static bool s_nearOnly = false;

void WrUiOnMapChanged(const char *map)
{
    strcpy_s(g_uiMap, sizeof(g_uiMap), map ? map : "");
}

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

static void FormatTime(double t, char *out, int outLen)
{
    int mins = (int)(t / 60.0);
    double secs = t - mins * 60.0;
    if (mins > 0)
        _snprintf_s(out, outLen, _TRUNCATE, "%d:%06.3f", mins, secs);
    else
        _snprintf_s(out, outLen, _TRUNCATE, "%.3f", secs);
}

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
        if (WrAvailableMapCount() == 0)
            ImGui::TextDisabled("(none yet -- run wrpath_extract.py)");
        for (int i = 0; i < WrAvailableMapCount(); i++)
        {
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
                       "automatically.");
        }

        if (running)
            ImGui::BeginDisabled();
        if (ImGui::Button(fresh > 0 ? "Extract new demos" : "Re-run extractor"))
            WrExtractRun(false);
        ImGui::SameLine();
        HelpMarker("Launches wrpath_extract.py in the background, at below-normal "
                   "priority so it does not fight the game for CPU. It only "
                   "processes demos that have no .wrpath yet, so pressing it "
                   "again costs seconds.\n\n"
                   "This starts a separate python process. It is never run "
                   "automatically -- only when you press this.");
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
            ImGui::TextDisabled("%s", WrExtractInterpreter());
    }

    if (WrRunCount() == 0)
    {
        ImGui::Separator();
        if (!inMap)
        {
            ImGui::TextDisabled("Load a map, or pick one above.");
            return;
        }
        ImGui::TextWrapped(
            "No cached paths for this map yet. The button above generates them "
            "from the demos the game already downloaded. If you would rather do "
            "it in a terminal:");
        ImGui::Spacing();
        char cmd[256];
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                    "python wrpath_extract.py --map %s --skip-existing", map);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##cmd", cmd, sizeof(cmd), ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Copy command"))
            ImGui::SetClipboardText(cmd);
        ImGui::SameLine();
        ImGui::TextDisabled("run it in the wrlines folder, then press Reload");
        return;
    }

    ImGui::Separator();

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
    if (ImGui::Button("All"))
    {
        for (int i = 0; i < WrRunCount(); i++)
        {
            WrRun *r = WrRunAt(i);
            if (r) r->enabled = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("None"))
    {
        for (int i = 0; i < WrRunCount(); i++)
        {
            WrRun *r = WrRunAt(i);
            if (r) r->enabled = false;
        }
    }

    ImGui::Checkbox("Only show runs near me", &s_nearOnly);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("within", &s_nearRadius, 512.0f, 16384.0f, "%.0f u");

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
    if (ImGui::BeginTable("runs", 9, flags, ImVec2(0.0f, tableHeight)))
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
        ImGui::TableHeadersRow();

        // The store itself stays sorted by time -- "max runs drawn" means the
        // fastest N, and the delta column looks up the best of each track by
        // taking the first match. So the click-to-sort order is a separate list
        // of indices used only for display.
        int order[WR_MAX_RUNS];
        int shown = 0;
        for (int i = 0; i < WrRunCount(); i++)
        {
            WrRun *r = WrRunAt(i);
            if (!r)
                continue;
            if (s_nearOnly && !(r->nearestDist >= 0.0f &&
                                r->nearestDist <= s_nearRadius))
                continue;
            order[shown++] = i;
        }
        SortRunOrder(order, shown, ImGui::TableGetSortSpecs());

        for (int row = 0; row < shown; row++)
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

            // The column that makes a staged map make sense: how far away this
            // run actually is from where you are standing right now.
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

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (WrRunEnabledCount() > g_render.maxRunsDrawn)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "%d enabled but only %d drawn -- raise the limit in Display.",
                           WrRunEnabledCount(), g_render.maxRunsDrawn);
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

// Every map Momentum knows about, what we hold for it, and a way to get more.
//
// The listing costs nothing: the game caches the whole catalogue on disk and
// wr_maps.h reads an index made from it. Fetching is the one part that reaches
// outside this machine, and it is off until you turn it on.
static bool g_fetchEnabled = false;
static int g_fetchTop = 25;
static int g_fetchTrackType = 0;
static int g_fetchTrackNum = 1;
static char g_mapFilter[64] = {0};

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
        WrExtractRunArgs("--index-maps", false);
    ImGui::SameLine();
    if (ImGui::Button("Recount what is on disk"))
        WrMapsRefresh();
    ImGui::SameLine();
    HelpMarker("The index comes from momentum\\_cache, which the game writes "
               "when it fetches the map list. Rebuilding reads that file and "
               "nothing else -- no network. If a map is missing, open the map "
               "selector in game once so the game refreshes its own copy.");

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", "filter by name", g_mapFilter,
                             sizeof(g_mapFilter));
    ImGui::SameLine();
    ImGui::TextDisabled("%d maps", WrMapsCount());
    ImGui::SameLine();
    HelpMarker("Two thousand maps and you have demos for a few hundred of "
               "them, so the ones you hold nothing for are hidden until you "
               "type a name. That is why a map you are looking for can appear "
               "with a 0 beside it: the 0 is what is on your disk, not what "
               "exists.");

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

    const char *here = WrLevelName();
    if (ImGui::BeginTable("##maps", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                          ImVec2(0.0f, 320.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("map", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("tier", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("demos", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("lines", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_NoSort, 170.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < WrMapsCount(); i++)
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
            if (g_fetchEnabled && !WrExtractRunning())
            {
                char args[256];
                _snprintf_s(args, sizeof(args), _TRUNCATE,
                            "--fetch --map \"%s\" --map-id %d --top %d "
                            "--track-type %d --track-num %d",
                            m->name, m->id, g_fetchTop, g_fetchTrackType,
                            g_fetchTrackNum);

                // Browse first, download second. One request, nothing written,
                // and it prints the leaderboard's own total -- which is the
                // only place that number can come from.
                if (ImGui::SmallButton("browse"))
                {
                    char dry[300];
                    _snprintf_s(dry, sizeof(dry), _TRUNCATE, "%s --dry-run", args);
                    WrExtractRunArgs(dry, false);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("download"))
                    WrExtractRunArgs(args, false);
            }
            else if (isHere && m->extracted < m->demos)
            {
                ImGui::TextDisabled("extract in Runs");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (WrExtractRunning() || WrExtractLineCount() > 0)
    {
        ImGui::SeparatorText("Output");
        if (ImGui::BeginChild("##fetchout", ImVec2(0.0f, 120.0f), true))
        {
            for (int i = 0; i < WrExtractLineCount(); i++)
                ImGui::TextUnformatted(WrExtractLine(i));
            if (WrExtractRunning())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
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
    ImGui::SliderInt("Points per run", &g_render.pointBudget, 100, 6000);
    ImGui::TextDisabled("Caps per-frame work. Distance culling does the job on a big");
    ImGui::TextDisabled("open map, but a compact stage fits entirely inside the draw");
    ImGui::TextDisabled("distance, so nothing gets culled and cost scales with runs.");
    ImGui::SliderInt("Max runs drawn", &g_render.maxRunsDrawn, 1, WR_MAX_RUNS);

    ImGui::SeparatorText("Colour");
    ImGui::Checkbox("Colour by speed", &g_render.colourBySpeed);
    if (g_render.colourBySpeed)
    {
        ImGui::SliderFloat("Slow", &g_render.speedMin, 0.0f, 3000.0f, "%.0f u/s");
        ImGui::SliderFloat("Fast", &g_render.speedMax, 100.0f, 6000.0f, "%.0f u/s");
        if (g_render.speedMax < g_render.speedMin + 50.0f)
            g_render.speedMax = g_render.speedMin + 50.0f;
        ImGui::TextDisabled("blue slow -> cyan -> green -> yellow -> red fast");
    }

    ImGui::SeparatorText("Who is who");
    ImGui::Checkbox("Name tags on lines", &g_render.drawTags);
    ImGui::SameLine();
    ImGui::Checkbox("Avatars", &g_render.tagAvatars);
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
               "Off by default: every arc has one of each, so turning both on "
               "puts twice as many numbers over the line.");
    ImGui::TextDisabled("Exact on stored lines. Your own line has no marks yet --");
    ImGui::TextDisabled("that needs a second derivative of a camera-differenced");
    ImGui::TextDisabled("velocity and is not measured well enough to draw.");
    ImGui::Spacing();
    LabelPicker("At checkpoints", &g_render.markerLabel, &g_render.drawMarkers);
    ImGui::SliderFloat("Marker size", &g_render.markerRadius, 2.0f, 16.0f, "%.1f px");
    ImGui::SliderInt("Max checkpoints per run", &g_render.maxMarkersPerRun, 1, 64);
    ImGui::Spacing();
    ImGui::SliderInt("Labels on screen", &g_render.maxLabelsPerFrame, 4, 200);
    ImGui::SameLine();
    HelpMarker("Numbers reserve their rectangle and skip if something is already "
               "there, so they never print on top of each other or of a name "
               "tag. Whatever does not fit is dropped rather than stacked -- "
               "Diagnostics shows how many were drawn.\n\n"
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
               "Its colour is the same rise/fall signal as the arrow beside your "
               "crosshair, so the two can never disagree. It used to be live "
               "strafing efficiency, and that was measuring noise: on a path "
               "where energy is exactly constant it saturated red or green 14% "
               "of the time at 2000 u/s and 36% at 3200.");

    ImGui::SeparatorText("Strafing efficiency");
    ImGui::Checkbox("Colour lines by strafing efficiency",
                    &g_render.colourByEfficiency);
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
        "nearby. About 0.6% of points.");

    if (g_render.colourByEfficiency)
    {
        if (g_render.colourBySpeed)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                               "Efficiency wins, so colour by speed is ignored.");
        ImGui::Checkbox("Show the key on screen", &g_render.effLegend);
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
    ImGui::Checkbox("Draw my path", &g_render.drawLive);
    int n = 0;
    WrLivePoints(&n);
    ImGui::SameLine();
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
    ImGui::SliderInt("Curves at once", &g_gMaxSeries, 1, G_MAX_SERIES);

    // --- gather -------------------------------------------------------------
    GSeries series[G_MAX_SERIES + 1];
    int nSeries = 0;
    int dropped = 0, untimed = 0, building = 0;

    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *r = WrRunAt(i);
        if (!r || !r->enabled || r->pointCount < 4)
            continue;
        if (nSeries >= g_gMaxSeries)
        {
            dropped++;
            continue;
        }
        const WrProfile *p = WrProfileFor(r);
        if (!p)
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
        series[nSeries].colour = r->colour;
        series[nSeries].name = r->player;
        nSeries++;
    }

    if (g_gLive)
    {
        const WrProfile *lp = WrProfileLive();
        if (lp && nSeries <= G_MAX_SERIES)
        {
            series[nSeries].p = lp;
            series[nSeries].run = NULL;
            series[nSeries].colour = g_render.liveColour;
            series[nSeries].name = "you";
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
    const float kLeft = 56.0f, kBottom = 20.0f, kTop = 8.0f, kRight = 10.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float ch = avail.y - 116.0f;            // leave room for the legend
    if (ch < 140.0f) ch = 140.0f;
    if (ch > 420.0f) ch = 420.0f;
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
    if (ImGui::BeginTable("##legend", 4, ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 96.0f)))
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
    ImGui::TextDisabled("Down triangles are ramp bottoms, up are tops. \"end\" is "
                        "where the curve finishes: how much");
    ImGui::TextDisabled("energy that run had thrown away by the time it got there.");

    #undef GPX
    #undef GPY
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
        "net energy", "carried %", "spent / banked", "gained / lost"
    };
    if (g_energy.hudMode < 0 || g_energy.hudMode >= WR_HUD_MODE_COUNT)
        g_energy.hudMode = 0;
    ImGui::Combo("Crosshair shows", &g_energy.hudMode, kModes,
                 WR_HUD_MODE_COUNT);

    static const struct { int vk; const char *name; } kKeys[] = {
        { 0, "none" }, { VK_END, "End" }, { VK_HOME, "Home" },
        { VK_NEXT, "Page Down" }, { VK_PRIOR, "Page Up" },
        { VK_DELETE, "Delete" }, { VK_F9, "F9" }, { VK_F10, "F10" },
    };
    const int kKeyCount = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
    int sel = 0;
    for (int i = 0; i < kKeyCount; i++)
        if (kKeys[i].vk == g_hudCycleKey) { sel = i; break; }
    const char *names[8];
    for (int i = 0; i < kKeyCount; i++) names[i] = kKeys[i].name;
    if (ImGui::Combo("Cycle key", &sel, names, kKeyCount))
        g_hudCycleKey = kKeys[sel].vk;
    ImGui::SameLine();
    HelpMarker("Switches the crosshair readout without opening this panel, "
               "which is the only way it is useful mid-run.\n\n"
               "It is a list rather than a fixed key because WrLines cannot see "
               "what you have bound. The key is read, not swallowed -- if it "
               "collides with something, the game still acts on it, so set this "
               "to none and use the box above instead.");

    // --- the anchor ---------------------------------------------------------
    ImGui::SeparatorText("Anchor");
    WrAnchorSource src = WrEnergyAnchorSource();
    if (src == WR_ANCHOR_RUN_START)
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                           "the start of the run you are chasing");
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
               "Nothing is ever written into the game install.");

    {
        float age = 0.0f;
        const char *recent = WrSavelocRecent(&age);
        if (recent && recent[0] && age < 4.0f)
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", recent);
    }

    int nLocs = WrSavelocCount();
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
                if (row.seconds >= 0.0f)
                {
                    char t[32];
                    FormatTime(row.seconds, t, sizeof(t));
                    if (row.suspect)
                        ImGui::TextDisabled("%s ?", t);
                    else
                        ImGui::Text("%s", t);
                }
                else
                {
                    ImGui::TextDisabled("--");
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
    ImGui::SliderFloat("Offset X", &g_energy.hudOffsetX, -400.0f, 400.0f, "%.0f px");
    ImGui::SameLine();
    HelpMarker("Negative puts it on the left of the crosshair and right-aligns "
               "it, so the block does not creep as the numbers get wider.");
    ImGui::SliderFloat("Offset Y", &g_energy.hudOffsetY, -400.0f, 400.0f, "%.0f px");
    ImGui::SliderFloat("Text scale", &g_energy.hudScale, 0.6f, 3.0f, "%.2f");
    ImGui::Checkbox("Dark plate behind it", &g_energy.hudBacking);

    ImGui::SeparatorText("Settings");
    ImGui::Checkbox("Also show the corner block", &g_energy.showOverlay);
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
    ImGui::SliderFloat("Smoothing", &g_energy.smoothSeconds, 0.05f, 1.0f, "%.2f s");
    ImGui::SameLine();
    HelpMarker("How long the headline figure takes to settle. The signal itself "
               "cannot rise faster than about 37 units a second -- that is the "
               "physical ceiling on what air strafing can add -- so a third of a "
               "second of filtering costs at most a dozen units of lag and "
               "removes almost all of the jitter.");
    ImGui::SliderFloat("Arrow window", &g_energy.trendSeconds, 0.3f, 2.0f, "%.2f s");
    ImGui::SliderFloat("Round to", &g_energy.quantiseStep, 0.0f, 25.0f, "%.0f");
    ImGui::SameLine();
    HelpMarker("Rounds the displayed figure, with hysteresis, so the last digit "
               "stops churning. Zero shows the raw value.");
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
    ImGui::Text("emit        %d segments from %d points in %.2f ms", segs, pts, ms);
    ImGui::Text("batches     %d AddPolyline calls", batches);
    ImGui::Text("frame       %.1f fps", ImGui::GetIO().Framerate);

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

    ImGui::SeparatorText("Log");
    if (ImGui::BeginChild("log", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders,
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
        "It writes nothing into the game install, sets no cvars, and never "
        "touches sv_cheats. All its files live in wrlines_data next to the DLL.");

    ImGui::SeparatorText("What it deliberately doesn't do");
    ImGui::BulletText("It never unloads. Restart the game to update the DLL.");
    ImGui::BulletText("It does not depend on the engine's debug overlay, which");
    ImGui::Indent();
    ImGui::TextDisabled("is gated behind sv_cheats and would taint your runs.");
    ImGui::Unindent();
    ImGui::BulletText("Lines have no depth test -- they draw through walls.");

    ImGui::SeparatorText("Keys");
    ImGui::BulletText("INSERT  -  show / hide this panel");
    ImGui::BulletText("ESC     -  close this panel");

    ImGui::SeparatorText("Files");
    ImGui::TextDisabled("%s", WrDataPath(""));
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
