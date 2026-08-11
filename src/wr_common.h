// wr_common.h  --  shared types and small helpers for WrLines.
//
// Deliberately tiny. No STL, no exceptions, no classes with behaviour: this code
// runs inside somebody else's process on their render thread, and everything it
// does should be obvious from reading it.
//
// Source's world axes, for reference while reading the maths elsewhere:
//   +X forward, +Y left, +Z up, units are inches-ish.

#ifndef WR_COMMON_H
#define WR_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>

#include "wr_version.h"

// How far from the origin a real world coordinate can be.
//
// NOT the 16384 that every Source reference quotes. Strata's maps are bigger:
// measured from the demos themselves, surf_colin_blaster_69000 reaches -31295
// on X. While the player was out past 16384 the matrix oracle rejected the real
// matrix every single frame, so that map had no lines and no energy readout at
// all, whatever was ticked.
//
// Keep in sync with WORLD_LIMIT in wrpath_extract.py, which filters candidate
// coordinates out of the demo netstream with the same number. That one is a
// real filter and mattered even more: with 16384 the origin stream on that map
// was chopped into fragments, and 66 of its 141 demos could not be extracted.
#define WR_WORLD_LIMIT 65536.0f

// ---------------------------------------------------------------------------
// Maths
// ---------------------------------------------------------------------------

struct Vec3
{
    float x, y, z;
};

// Source's VMatrix is row-major: m[row][col]. WorldToScreenMatrix() hands back a
// combined world->clip matrix, so row 3 is the w row and doubles as the (scaled)
// view forward vector.
struct VMatrix
{
    float m[4][4];
};

static inline Vec3 WrVec(float x, float y, float z)
{
    Vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

static inline Vec3 WrSub(const Vec3 &a, const Vec3 &b)
{
    return WrVec(a.x - b.x, a.y - b.y, a.z - b.z);
}

static inline Vec3 WrAdd(const Vec3 &a, const Vec3 &b)
{
    return WrVec(a.x + b.x, a.y + b.y, a.z + b.z);
}

static inline Vec3 WrScale(const Vec3 &a, float s)
{
    return WrVec(a.x * s, a.y * s, a.z * s);
}

static inline float WrDot(const Vec3 &a, const Vec3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float WrLength(const Vec3 &v);
float WrDist(const Vec3 &a, const Vec3 &b);
float WrDistSqr(const Vec3 &a, const Vec3 &b);
Vec3 WrNormalize(const Vec3 &v);

static inline float WrClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int WrClampI(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// True only for values that are finite and not absurd. Every float that comes
// out of the game gets run through this before we do anything with it.
bool WrSaneFloat(float f);
bool WrSaneVec(const Vec3 &v);

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Absolute path to "<directory containing wrlines.dll>\wrlines_data\<rel>".
// Creates the directory tree on demand.
//
// This is where WrLines writes. The one exception is deliberate and opt-in: a
// demo copied into the game's own replay folder so the game can play it, either
// by the --into-game flag or by the per-run send button. See wr_intogame.h --
// that path is recorded before it is written and is the only thing that can be
// removed again.
const char *WrDataPath(const char *rel);

// Directory the DLL itself was loaded from.
const char *WrModuleDir(void);

// The game install root, derived from the running executable
// (<root>\bin\win64\momentum.exe -> <root>). Empty if it cannot be determined.
const char *WrGameDir(void);

// Are we running under Wine (which is what Proton is)?
//
// There is no native Linux build of the game -- the install ships bin\win64 and
// no .so at all -- so Linux users run the Windows game under Proton, and this
// Windows DLL is what loads into it. That works, and it is the supported path.
// It is worth KNOWING though, because it changes what several other diagnostics
// mean: which d3d11.dll is loaded, and whether a path the game printed is a
// Windows path or a Z: mapping of a Linux one.
//
// It used to change a third one, and that it no longer does is the best single
// thing v0.7.0 did for Proton. Extraction was a Python child process launched
// from inside the game, so it needed an interpreter on the PREFIX'S PATH -- and
// a Python installed on the Linux side is not on it. Now there is nothing to
// find: extraction runs in this DLL, which is inside the prefix by definition.
//
// Detected the standard way, by asking ntdll for an export only Wine has. No
// call is made through it and nothing is loaded; this is a GetProcAddress and a
// null test.
bool WrIsWine(void);

// mkdir -p, one CreateDirectory per component. True if the directory exists
// afterwards, however it got there.
//
// One copy, because there were three -- in wr_log.cpp, wr_fetch.cpp and
// wr_intogame.cpp -- and they disagreed about whether a forward slash counts as
// a separator and about what to do with the "C:" at the front. That was
// harmless while each had one caller on one thread; extraction now creates
// directories from a worker pool, and three subtly different answers to "does
// this path exist yet" is not a thing to keep.
//
// Racing with itself is fine: a component another thread has just created comes
// back ERROR_ALREADY_EXISTS, which is not an error here.
bool WrMakeTree(const char *dir);

// ---------------------------------------------------------------------------
// The one thing v0.7.0 made worse
// ---------------------------------------------------------------------------
//
// Every path in this project is char* and every file call is the -A form, which
// on a non-Unicode locale cannot name a path holding a byte >= 0x80. A library
// under D:\Игры\ has therefore always been broken here -- the demo counter has
// never been able to see it.
//
// What CHANGED is who else could. Until v0.6.1 the extractor was a Python
// script, and Python opens files with wide paths, so it read that install
// perfectly and wrote perfectly good .wrpath files that the DLL then loaded by
// map name and drew. Extraction moving in-process took that away. It is the
// only respect in which the port is worse than what it replaces, and it is
// recorded as such rather than discovered by a user.
//
// So: DETECT AND SAY SO. This does not fix anything. It converts "the button
// did nothing and found no demos" into a sentence naming the path and the
// reason, in the log and on the panel, which is the difference between a bug
// report that can be answered and one that cannot.
//
// GetShortPathNameA is not the workaround it looks like -- 8.3 name generation
// is off by default on modern volumes, so it returns the long path unchanged on
// exactly the machines that would need it. The real fix is a -W conversion of
// the whole project, which is a change of a different size.
//
// Bounded, at least: demo file names are hex and map names are ASCII by the
// game's own rules, so only the install path and the user profile are exposed.
bool WrPathIsAscii(const char *s);

// os.path.splitext's stem: the basename with its extension removed, where
// "extension" means what Python means by it and not what strrchr means.
//
// THE RULE IS NOT "CUT AT THE LAST DOT". genericpath._splitext walks forward
// from the start of the basename and, if it reaches the last dot having passed
// nothing but dots, decides there is no extension at all -- so splitext(".mtv")
// is (".mtv", "") and the stem is ".mtv", where a plain strrchr leaves the
// empty string. Same for "..mtv"; but ".a.mtv" does split, into ".a".
//
// It lives out here rather than twice in two files because the two callers have
// to agree or nothing works: wr_extract keys the failure record and the
// --skip-existing lookup on this string, and wr_demo names the output file and
// packs src_sha1 with it. Two implementations of a rule this fiddly is a bug
// waiting for a filename nobody expected -- and a demo called ".mtv" is
// accepted by both sides' directory walks, so it is reachable rather than
// theoretical.
void WrFileStem(const char *path, char *out, int cap);

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

// Unix seconds, unless WRLINES_FAKE_NOW pins them. The reference's _now().
//
// Every timestamp written into a file goes through this one function so that
// setting one environment variable makes the whole program's output
// reproducible. See the comment on the definition in wr_log.cpp.
long long WrNowEpoch(void);

extern HMODULE g_wrSelf;

#endif // WR_COMMON_H
