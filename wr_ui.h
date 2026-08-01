// wr_ui.h  --  the WrLines panel.

#ifndef WR_UI_H
#define WR_UI_H

#include "wr_common.h"

void WrUiDraw(void);

// Called when the loaded map changes so the Runs tab can reload.
void WrUiOnMapChanged(const char *map);

#endif // WR_UI_H
