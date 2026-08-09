// wr_log.h  --  logging for WrLines.
//
// Two sinks, always both: wrlines_data\wrlines.log on disk (flushed on every
// write, because the interesting failures are the ones that end with the process
// dying) and an in-memory ring the Diagnostics tab renders.
//
// The ring is a fixed-size array of fixed-size lines and never allocates after
// init, so it is safe to log from the render thread every frame.

#ifndef WR_LOG_H
#define WR_LOG_H

#include "wr_common.h"

#define WR_LOG_LINES 512
#define WR_LOG_LINE_MAX 256

void WrLogInit(void);
void WrLogf(const char *fmt, ...);

// Snapshot access for the UI. Lines are returned oldest-first; count is however
// many are currently held (<= WR_LOG_LINES).
int WrLogCount(void);
const char *WrLogLine(int index);

#endif // WR_LOG_H
