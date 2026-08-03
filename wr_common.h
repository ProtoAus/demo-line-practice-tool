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

#define WRLINES_VERSION "0.2.1"

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
// Creates the directory tree on demand. Nothing is ever written into the game
// install; this is the only place WrLines writes at all.
const char *WrDataPath(const char *rel);

// Directory the DLL itself was loaded from.
const char *WrModuleDir(void);

// The game install root, derived from the running executable
// (<root>\bin\win64\momentum.exe -> <root>). Empty if it cannot be determined.
const char *WrGameDir(void);

extern HMODULE g_wrSelf;

#endif // WR_COMMON_H
