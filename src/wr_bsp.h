// wr_bsp.h  --  the map file, read for the geometry the player has not hit yet.
//
// WHAT THIS IS AND WHAT IT IS NOT
//
// wr_phase.h recovers the surface a player DID touch, exactly, from the fact
// that free flight has vertical acceleration -g. It needs no map and works on
// every map, including displacements and moving brushes. What it cannot do is
// say anything about a ramp nobody has hit, because there is no measurement of
// a collision that did not happen.
//
// This is the other half. The .bsp already contains every plane in the level,
// and reading it is ordinary read-only file I/O of exactly the kind this tool
// already does -- CreateFileA, GENERIC_READ, shared, no writing and no calls
// into the game. Nothing here is a trace through the engine and nothing here
// asks the engine anything.
//
// The two are complementary and neither replaces the other. Where they overlap
// they can be CROSS-CHECKED, which is the most valuable thing about having
// both: a kinematically recovered normal and a normal read out of the file are
// two independent measurements of the same plane, and tests\bsp_sweep.exe
// --verify-normals is what compares them.
//
// THE FILE, AS ACTUALLY MEASURED
//
// 1,304 maps in momentum\maps on this machine, all of which parse:
//
//     v20   1,122   86.0%      the stock Source version, and the bulk of surf
//     v21      97    7.4%
//     v25      77    5.9%      Strata's own, and the one that moved structs
//     v19       8    0.6%
//
// COMPRESSION IS A PROPERTY OF THE FILE, NOT OF THE LUMP. 723 maps have all
// seven collision lumps LZMA-compressed and 581 have none of them; no map
// mixes. So the compressed arm is not an edge case to be handled grudgingly --
// it is the majority path, and it is exercised by more than half the library.
//
// Sizes are small. The seven lumps together are 1.55 MB at the median, 3.24 MB
// at p99, and 7.18 MB at the very worst (surf_ispy). The budget constants below
// are three orders of magnitude above that on purpose: they are not a fit to
// this data, they are a guard against a file that lies, and they should never
// fire on a real map.
//
// THE STRIDE TABLE IS KEYED ON (bspVersion, lumpVersion), AND HAS TO BE
//
// This is the single most important decision in the file. The obvious key is
// the BSP version, and it is wrong: Source's LEAFS struct shrank from 56 bytes
// to 32 when its ambient lighting moved into a lump of its own, and what
// records that is the LUMP's own version field, not the file's. Measured over
// the corpus, every lump version is exactly determined:
//
//     LEAFS         v19 -> 0 (56)   v20/21 -> 1 (32)   v25 -> 2 (56)
//     NODES         v19/20/21 -> 0 (32)                v25 -> 1 (48)
//     LEAFBRUSHES   v19/20/21 -> 0 (2)                 v25 -> 1 (4)
//     BRUSHSIDES    v19/20/21 -> 0 (8)                 v25 -> 1 (16)
//     PLANES, MODELS, BRUSHES   -> 0 everywhere, 20 / 48 / 12
//
// Note v25's LEAFS going back UP to 56. A reader keyed on "is it Strata"
// would have to know that; a reader keyed on the lump version is simply told.
//
// EVERY COMBINATION NOT IN THAT TABLE IS REFUSED BY NAME. There is no "it is
// probably still the same" branch and there is deliberately no stride inferred
// from the lump length -- the corpus has PLANES lumps divisible by 20 and by
// 40 and by 4, so length alone cannot single a stride out, and a reader that
// guessed would produce geometry rather than an error. That is the failure
// mode this whole file is arranged around: a wrong stride does not crash, it
// makes plausible planes at plausible angles in plausible places.
//
// WHAT IS READ AND WHY EACH ONE
//
//     PLANES        the normals and distances. The answer, ultimately.
//     BRUSHES       first side, side count, contents.
//     BRUSHSIDES    plane index per side.
//     MODELS        model 0 is the world; its headnode roots the walk.
//     NODES/LEAFS/LEAFBRUSHES   the walk itself.
//
// The walk is not an optimisation. TRIGGER BRUSHES CARRY CONTENTS_SOLID
// VERBATIM -- there is no bit anywhere in the file separating a
// trigger_teleport volume from a ramp, and on surf_greensway 47% of solid
// brushes are teleport volumes and 76% of surf-band area is entity-owned. A
// teleport volume on a surf map is typically a large slanted box under the
// ramp, so it lands squarely in the band and would be reported as a ramp. The
// only thing that distinguishes them is which model owns the brush, so the
// worldspawn tree has to be walked to find out.
//
// It costs something honest: worldspawn-only keeps 87.6% of brushes, correctly
// drops 7.7% as triggers, and loses 4.7% that are genuine func_* geometry. The
// panel says so.
//
// FACES ARE NOT USED, AND THAT IS NOT AN OVERSIGHT
//
// A Source map stores both brushes and faces and they do not agree. surf_inner
// reads 0% brush-backed by faces and 100% by brushes; surf_ethereal has 2,692
// surf-band brushsides against zero surf-band faces. FACES is a RENDERING
// structure and does not exist for surfaces with no visible material, which on
// a surf map includes a great deal of what people actually ride.
//
// DISPLACEMENTS ARE OUT OF SCOPE FOR THIS VERSION
//
// Over half of maps have some. Measured across all 1,106 surf maps, 95.5% of
// surfable area at real ramp angles is brush-backed and the median map is 100%
// -- but 51 maps are displacement-dominated and on those this reads almost
// nothing. That has to be SAID rather than discovered, because missing
// collision reads to a user as "there is no ramp ahead", which is worse than
// the feature not existing. Hence WrBspMap::coverage and the panel line.
//
// NOTHING SHAPED LIKE A NODE SURVIVES THE PARSE
//
// The tree is walked once, to decide which brushes the world owns, and then
// thrown away. What stays resident is polygons and floats and a uniform grid
// built here -- see wr_bspgeom.h for why a grid and not the file's own tree.
// So the version-sensitive structs exist for the length of one function call,
// which is the only window in which Strata changing dnode_t again can hurt.

#ifndef WR_BSP_H
#define WR_BSP_H

#include "wr_common.h"
#include "wr_bspgeom.h"

// ---------------------------------------------------------------------------
// Budgets
// ---------------------------------------------------------------------------
//
// Guards against a file that lies about itself, not a fit to real maps. The
// largest single collision lump in the library is 3.3 MB and the largest total
// is 7.2 MB, so nothing below should ever fire on a map somebody plays. They
// exist because fileofs, filelen and the LZMA container's declared size all
// come out of somebody else's file and each of them is an allocation.

#define WR_BSP_MAX_LUMP        (128u * 1024u * 1024u)
#define WR_BSP_MAX_LUMPS_TOTAL (256u * 1024u * 1024u)
#define WR_BSP_MAX_RESIDENT    (64u * 1024u * 1024u)

// Sides on one brush. vbsp's own MAXPOINTS is 128 and the widest worldspawn
// brush measured across the corpus has 62.
#define WR_BSP_MAX_BRUSH_SIDES 128

// ---------------------------------------------------------------------------
// The seven lumps
// ---------------------------------------------------------------------------

enum
{
    WR_BSP_L_PLANES = 0,
    WR_BSP_L_NODES,
    WR_BSP_L_LEAFS,
    WR_BSP_L_MODELS,
    WR_BSP_L_LEAFBRUSHES,
    WR_BSP_L_BRUSHES,
    WR_BSP_L_BRUSHSIDES,
    WR_BSP_L_COUNT
};

// Their index in the file's own 64-entry directory, and the name used in every
// refusal message. Exposed so tests\test_bsp.exe can build a directory.
extern const int  WrBspLumpIndex[WR_BSP_L_COUNT];
extern const char *WrBspLumpName[WR_BSP_L_COUNT];

// The stride table, as a function so the refusal is in one place. Returns 0
// for a combination this does not read, which is a refusal and not a default.
int WrBspStride(int lump, int bspVersion, int lumpVersion);

// The seven lumps, decompressed, with their strides resolved and their lengths
// checked to tile exactly. This is the whole of the file layer: it knows about
// bytes and versions and budgets, and nothing at all about geometry.
struct WrBspRaw
{
    int version;
    int revision;

    unsigned char *data[WR_BSP_L_COUNT];
    unsigned int   bytes[WR_BSP_L_COUNT];
    int            stride[WR_BSP_L_COUNT];
    int            count[WR_BSP_L_COUNT];

    unsigned int   totalBytes;      // uncompressed, across the seven
    bool           compressed;      // was any of them LZMA?
};

// Read them, or fail with a reason a person can act on. Never partially
// succeeds: on false, nothing is allocated and *out is zeroed.
bool WrBspReadRaw(const char *path, WrBspRaw *out, char *err, int errCap);
void WrBspFreeRaw(WrBspRaw *r);

#endif // WR_BSP_H
