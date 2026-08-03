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
#include "wr_hook.h"
#include "wr_log.h"

#include "imgui.h"

#include <float.h>

#include <stdio.h>
#include <string.h>

static char g_uiMap[72] = {0};

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
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("runs", 9, flags, ImVec2(0.0f, tableHeight)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On");
        ImGui::TableSetupColumn("Col");
        ImGui::TableSetupColumn("Track");
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Delta");
        ImGui::TableSetupColumn("Near");
        ImGui::TableSetupColumn("Pts");
        ImGui::TableSetupColumn("Splits");
        ImGui::TableHeadersRow();

        for (int i = 0; i < WrRunCount(); i++)
        {
            WrRun *r = WrRunAt(i);
            if (!r)
                continue;
            if (s_nearOnly && !(r->nearestDist >= 0.0f &&
                                r->nearestDist <= s_nearRadius))
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

    ImGui::SeparatorText("Ramp speeds");
    ImGui::Checkbox("Speed at ramp bottoms", &g_render.drawDipSpeeds);
    ImGui::SliderInt("Max per run", &g_render.maxDipsPerRun, 1, 100);
    ImGui::TextDisabled("Horizontal speed where a line stops falling and climbs");
    ImGui::TextDisabled("again, in that run's own colour.");

    ImGui::SeparatorText("Split markers");
    ImGui::Checkbox("Draw split markers", &g_render.drawMarkers);
    ImGui::SliderFloat("Marker size", &g_render.markerRadius, 2.0f, 16.0f, "%.1f px");
    ImGui::TextDisabled("Markers are only drawn for runs whose splits could be");
    ImGui::TextDisabled("anchored to the path with confidence.");

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

static void DrawEnergyTab(void)
{
    ImGui::TextWrapped(
        "Energy is how high you could still get if you redirected everything "
        "you have straight up, measured from the last ground you were standing "
        "on:  E = height above it + v squared / 2g.  Standing still it reads 0. "
        "It says what horizontal speed alone cannot -- whether trading height "
        "for speed on a ramp actually gained you anything.");
    ImGui::TextWrapped(
        "The second figure is the same energy written as a speed, so it reads in "
        "the units you are used to.");
    ImGui::TextWrapped(
        "The map's own shape cancels out. Drop 18000 units down a surf map and "
        "convert every unit of it into speed and this still reads 0 -- what is "
        "left is what you WASTED getting there. So on a descending map everyone "
        "reads negative; that is friction and imperfect ramp entries, not you "
        "doing badly. What matters is being less negative than the run you are "
        "chasing, which is what the gap below is for.");
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
                        WrEnergyOnGround() ? "   (on it now)" : "");
        else
            ImGui::TextDisabled("measured from where you are -- no ground seen yet");
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

    if (ImGui::Button("Reset"))
        WrEnergyReset();
    ImGui::SameLine();
    if (ImGui::Button("Measure from here"))
        WrEnergyRearm();
    ImGui::SameLine();
    HelpMarker("Re-arms the reference at your current height instead of waiting "
               "for the next time you touch ground.");

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
            if (ImGui::BeginTabItem("Display"))    { DrawDisplayTab();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Energy"))     { DrawEnergyTab();      ImGui::EndTabItem(); }
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
