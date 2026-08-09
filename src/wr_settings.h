// wr_settings.h  --  the panel's settings, written to disk and read back.
//
// WHY THIS EXISTS
//
// Until now nothing was persisted at all. WrRenderDefaults, WrEnergyDefaults,
// WrStartDefaults and WrLimitDefaults ran once from dllmain and that was the
// whole story, so every restart of the game was a fresh install and every
// setting anybody had chosen -- line thickness, the crosshair mode, the frame
// cap, which key does what -- was gone.
//
// ONE TABLE, TWO POPULATORS
//
// Every persisted field is registered once, in the file that owns the variable,
// and the writer and the reader both walk the same registration list. That is
// the whole design, and it is the point: a serialiser written out by hand over a
// hundred-odd fields goes out of step with the struct the first time somebody
// adds a field and only edits one of the two halves. Here there is only one
// half, and adding a setting is one line next to the setting.
//
// A registration carries a range, and the reader CLAMPS to it. A settings file
// is a text file a person can edit, so it is an untrusted input: nothing read
// from it may put the tool in a state its own sliders cannot reach.
//
// COMPATIBILITY IN BOTH DIRECTIONS
//
// An unknown key is ignored and a missing key keeps its default, so an old file
// loads into a new build and a new file loads into an old one. There is no
// version number to get wrong.
//
// WHAT IS IN IT
//
// Display settings, and nothing else. No player names, no SteamIDs, no map data,
// no paths, no history of what was watched. Those live elsewhere under
// wrlines_data, which is gitignored for exactly that reason. This file is safe
// to post in a bug report.

#ifndef WR_SETTINGS_H
#define WR_SETTINGS_H

#include "wr_common.h"

// --- registration ----------------------------------------------------------
//
// Called once at startup, from WrSettingsInit. Each module registers the fields
// it owns; the names are the file's keys and are the one thing here that must
// not change casually, since changing one silently resets that setting for
// everybody who already has a file.

void WrSettingsBool(const char *name, bool *p);
void WrSettingsInt(const char *name, int *p, int lo, int hi);
void WrSettingsUInt(const char *name, unsigned int *p);
void WrSettingsFloat(const char *name, float *p, float lo, float hi);

// Implemented in wr_ui.cpp, for the handful of settings the panel itself owns:
// the four key bindings and the Graphs tab's own toggles.
void WrUiRegisterSettings(void);

// --- the file --------------------------------------------------------------

// Register everything, then read wrlines_data\settings.cfg over the defaults.
// Call AFTER the four *Defaults() functions, so a missing key keeps its default.
void WrSettingsInit(void);

// Write now, whatever the debounce thinks. Returns false if the file could not
// be written -- which is worth showing, because a settings file that silently
// fails to save is the bug this whole thing exists to fix.
bool WrSettingsSave(void);

// Re-read the file, discarding unsaved changes.
bool WrSettingsLoad(void);

// Put every registered field back to what the *Defaults() functions say, and
// save that. The panel's "reset everything" button.
void WrSettingsResetAll(void);

// Called every frame. Notices a change, waits for it to settle, writes once.
//
// Debounced rather than immediate because a slider being dragged changes its
// value every frame, and a file written sixty to three hundred times a second
// is a real cost for no benefit.
void WrSettingsTick(float dt);

// For the panel: how many fields are registered, when the last write happened
// (seconds ago, negative if never), and whether anything is waiting to be
// written.
int WrSettingsFieldCount(void);
float WrSettingsSinceSave(void);
bool WrSettingsPending(void);

// How many keys the last read did not recognise. Non-zero means the file was
// written by a different build, which is allowed and worth saying.
int WrSettingsUnknownKeys(void);

// The absolute path, for the About tab.
const char *WrSettingsPath(void);

#endif // WR_SETTINGS_H
