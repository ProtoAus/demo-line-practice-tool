// wr_json.h  --  a JSON reader that walks, rather than one that builds.
//
// WHY THIS IS HAND-WRITTEN AND NOT A LIBRARY
//
// There are four JSON documents in this project: the run-stats blob inside a
// .mtv header, the game's map catalogue, a leaderboard page, and a friends
// lookup. All four are shallow, all four have a schema we know, and between
// them they need about a dozen field reads.
//
// The README's case for trusting this DLL is a list of things it does not do
// that you verify by READING it. Nineteen thousand lines of general-purpose
// JSON, ninety percent of which is a writer and a mutable document model that
// nothing here would ever call, is the single worst thing that list could
// acquire. Three hundred lines you can read in a sitting is not.
//
// It is also not merely smaller. Python's json gives back Python objects, and
// the extractor tests them with isinstance -- read_map_catalogue takes a map's
// id only if it is an INT, so a catalogue claiming "id": 265.0 is skipped. A
// general parser hands you a number and you rebuild that distinction on top of
// it; a reader that already knows whether it saw a '.' has it for free. Every
// place this file is used, matching the reference implementation exactly is the
// requirement, and the requirement is easier to meet from below.
//
// WHY A CURSOR AND NOT A TREE
//
// The map catalogue is twelve megabytes of decompressed JSON: 2135 maps with 21
// leaderboards each, around a million nodes. A document model would allocate all
// of them inside the game's heap to answer three questions per map. This walks
// the text once and allocates nothing at all -- the only buffer involved is the
// decompressed bytes the caller already had.
//
// The shape of a walk:
//
//     WrJson j;
//     WrJsonInit(&j, text, len);
//     if (WrJsonEnterArray(&j))
//         while (WrJsonNextElement(&j))
//             if (WrJsonEnterObject(&j))
//                 while (WrJsonNextMember(&j, key, sizeof(key)))
//                 {
//                     if (!strcmp(key, "id"))   id = WrJsonInt(&j, 0, &ok);
//                     else                      WrJsonSkip(&j);
//                 }
//
// A member you do not read is skipped for you: the next Next* call notices the
// value is still sitting there and steps over it. Forgetting to consume is the
// obvious mistake and it is not a mistake here.
//
// SAFETY
//
// One `end` pointer, checked before every read, and no recursion -- nesting is
// counted, not stacked, so a document of ten thousand open brackets costs ten
// thousand increments and not ten thousand frames. Depth is capped anyway.
// Malformed input sets a sticky error flag and every subsequent call is a
// no-op returning the caller's default, so a walk over rubbish ends quietly
// instead of part way through a struct.
//
// The one thing this does NOT reproduce: Python decodes the catalogue with
// errors="replace", so genuinely invalid UTF-8 becomes U+FFFD there and passes
// through unchanged here. Every string we read is a map name or a player alias;
// the first are ASCII and the second arrive as valid UTF-8 from an API that
// serialises them itself. If that ever stops being true it shows up as one
// differing byte in a parity run, which is where it should show up.

#ifndef WR_JSON_H
#define WR_JSON_H

#include "wr_common.h"

// How deep a document may nest before it is rejected. The deepest of the four
// real ones is the run-stats blob at five.
#define WR_JSON_MAX_DEPTH 32

enum WrJsonKind
{
    WR_JSON_NONE = 0,       // nothing there, or the reader has already failed
    WR_JSON_NULL,
    WR_JSON_BOOL,
    WR_JSON_INT,            // no '.', no exponent. Python's isinstance(x, int)
    WR_JSON_REAL,
    WR_JSON_STRING,
    WR_JSON_ARRAY,
    WR_JSON_OBJECT
};

struct WrJson
{
    const char *p;
    const char *end;
    const char *value;      // an unconsumed value sits here; NULL once taken
    int depth;
    bool bad;
};

void WrJsonInit(WrJson *j, const char *text, size_t len);

// True if anything has gone wrong. Sticky: once set, every call below is a
// no-op and the walk unwinds on its own.
bool WrJsonFailed(const WrJson *j);

// What the value under the cursor is, without consuming it.
WrJsonKind WrJsonPeek(WrJson *j);

// Step into a container. False (and nothing consumed) if it is not one.
bool WrJsonEnterArray(WrJson *j);
bool WrJsonEnterObject(WrJson *j);

// Advance to the next element or member, skipping any value the caller left.
// False at the closing bracket, which they consume.
bool WrJsonNextElement(WrJson *j);
bool WrJsonNextMember(WrJson *j, char *key, int keyCap);

// Read the value under the cursor, consuming it. Each returns `def` and leaves
// `*ok` false when the value is a different kind -- which is the isinstance
// test the reference implementation makes, in the place it makes it.
//
// `ok` may be NULL.
long long WrJsonInt(WrJson *j, long long def, bool *ok);
double WrJsonReal(WrJson *j, double def, bool *ok);   // accepts INT too
bool WrJsonBool(WrJson *j, bool def, bool *ok);
bool WrJsonString(WrJson *j, char *out, int cap);     // false if not a string

// Consume and discard whatever is there, however big.
void WrJsonSkip(WrJson *j);

#endif // WR_JSON_H
