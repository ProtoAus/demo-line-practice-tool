// api_tape.h  --  a recorded leaderboard conversation. NOT SHIPPED.
//
// A leaderboard is not a fixture. It changes under you: ranks move as runs
// land, so a board fetched twice an hour apart produces two different .tsv
// files that are both correct. That makes "did the port write the same bytes as
// the reference" unanswerable against a live server, and it makes the whole
// --board path untestable in CI, because a build machine should not be calling
// somebody's API at all.
//
// So: record once, replay for ever. The recording is a directory of numbered
// response bodies and an index.txt of "url<TAB>file" in request order, and it
// is the SAME FORMAT the reference implementation writes -- which is the point.
// tests\parity.ps1 records one conversation with Python and then replays that
// one recording into both implementations, so the two are answering the same
// bytes rather than two live boards that happen to be a minute apart.
//
// A URL that is not in the recording is an ERROR and never a fetch. Silently
// going to the network during a comparison would make the comparison a lie, and
// the message is the reference's own wording so that the failure reads the same
// on both sides.
//
// WHY THIS FILE IS UNDER tests\ AND NOT UNDER src\
//
// Because that is the enforcement. src\wr_api.cpp holds one function pointer
// and a null check; everything that could make a request come from somewhere
// other than the network lives here, in a file the DLL does not compile. There
// is no build flag to get this into a shipped binary and no runtime switch that
// reaches it -- it is simply not in the link line.

#ifndef WR_API_TAPE_H
#define WR_API_TAPE_H

// Open a recording. `record` fetches for real and saves what comes back;
// otherwise every request is answered from what is already there. Returns false
// having printed why.
bool WrTapeOpen(const char *dir, bool record);

// Point wr_api at it. Pacing stays on for a recording -- that is a real
// conversation with somebody's server -- and off for a replay, where the four
// hundred milliseconds are owed to a file on disk.
void WrTapeInstall(void);

// How many requests have gone through it. The harness asserts on this, which is
// how "a request escaped to the network" would be noticed rather than merely
// unlikely.
int WrTapeRequests(void);

void WrTapeClose(void);

#endif // WR_API_TAPE_H
