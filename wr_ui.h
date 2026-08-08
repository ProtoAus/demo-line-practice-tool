// wr_ui.h  --  the WrLines panel.

#ifndef WR_UI_H
#define WR_UI_H

#include "wr_common.h"

void WrUiDraw(void);

// Called when the loaded map changes so the Runs tab can reload.
void WrUiOnMapChanged(const char *map);

// Virtual-key code that cycles the crosshair readout, or 0 for none. Polled by
// the hotkey thread; chosen in the Energy tab because we cannot know what the
// player has already bound.
int WrUiHudCycleKey(void);

// And the one that turns the "whose line is this" plate off and on. Defaults to
// Home. Same list, same caveat: read, never swallowed.
int WrUiPickToggleKey(void);

#endif // WR_UI_H
