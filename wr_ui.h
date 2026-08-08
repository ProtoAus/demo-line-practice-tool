// wr_ui.h  --  the WrLines panel.

#ifndef WR_UI_H
#define WR_UI_H

#include "wr_common.h"

void WrUiDraw(void);

// Called when the loaded map changes so the Runs tab can reload.
void WrUiOnMapChanged(const char *map);

// The four keys the hotkey thread polls, or 0 for none. All chosen in the panel
// because we cannot know what the player has already bound, and all READ rather
// than swallowed -- a collision means the game acts on the key too.
//
//   cycle / cycle back   the centre box's mode, Page Down and Page Up
//   pick toggle          the "whose line is this" plate, Home
//   overlay toggle       the corner block, End
int WrUiHudCycleKey(void);
int WrUiHudCycleBackKey(void);
int WrUiPickToggleKey(void);
int WrUiOverlayToggleKey(void);

// A printable name for one of those, for the About page. "(unlisted)" if it is
// not one of the bindable keys -- which a settings file edited by hand can be.
const char *WrUiKeyName(int vk);

#endif // WR_UI_H
