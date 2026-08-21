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
// AND THEY AGREE. Over 39 maps and 2,294 recorded runs, at 1,862 boards -- a
// board being a tick that arrives out of free flight, so exactly one surface
// is involved -- the file's plane and the velocity trace's plane differ by a
// median of 1.19 degrees, 91.4% within 15, and 1.0% grossly. On slope alone,
// which is the only part of a normal anything here ever prints, the median is
// 0.64 degrees.
//
// The number that matters most is the SIGNED one: +0.00 degrees at the median.
// Not a small bias, none -- two measurements that share no code, no input and
// no assumption, centred exactly on each other. That is as close to external
// validation as this reader is ever going to get, and it says the stride
// table, the field offsets, the worldspawn walk and the clip are all reading
// the geometry somebody actually surfed on.
//
// Mid-ride the same estimator reads a median of 4.91 degrees with 12% gross,
// and that is not the reader either. It is corners and seams, where two
// surfaces push at once and the recovered normal is their sum -- a question
// with no single answer rather than a wrong answer. Face identification was
// the first suspect and was ruled out: matching by a downward ray instead of
// by proximity moved the gross rate from 13.0% to 12.4%.
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
// WHAT THE CLIP ACTUALLY PRODUCES
//
// Over the whole library: 33,857,967 brushsides in, 19,733,988 polygons out,
// 41.7% dropped as bevels or slivers, and -- the number this was all arranged
// to produce -- ZERO failures of the closure assertion. Not a low rate: none,
// across thirty-four million sides and four struct layouts. Nothing else in
// this file could have caught a stride wrong in a way every index check
// passed, and it had thirty-four million chances to.
//
// Two other tallies, both small and both meaning something. No brushside
// anywhere references one of the degenerate (0,0,0) planes, so those really
// are unreferenced rather than merely rare. And 15 sides on 3 maps have a
// vertex outside +-WR_WORLD_LIMIT; all three are maps with unusual extents,
// one of them surf_colin_blaster_69000, which is the map that forced
// WR_WORLD_LIMIT up to 65536 in the first place.
//
// THE UPPER EDGE OF THE SURF BAND IS DOING REAL WORK
//
// Area-weighted over every up-facing face on all 1,106 surf maps:
//
//      0 deg   85.6%   floors, which is what "up-facing" mostly means
//     45 deg    1.69%  the largest non-floor feature by a wide margin
//     51 deg    0.75%  the mode INSIDE the surf band
//     27 deg    0.72%
//
// The 45-degree spike is mappers taking the default slope, and a 45-degree
// slope has n.z = 0.7071, which is above Source's 0.7 standable cut -- so it
// is something you walk up, not something you surf, and WR_BSP_BAND_HI
// excludes it by a margin of one hundredth. Widening that bound by a rounding
// error would more than double the drawn set with surfaces nobody slides on.
// Inside the band the distribution runs from about 46 to 60 degrees and peaks
// at 51, which is the shape a surf map has.
//
// 6.8% of up-facing area is inside the band. That is the honest size of this
// feature: most of a map is floor.
//
// NOTHING SHAPED LIKE A NODE SURVIVES THE PARSE
//
// The tree is walked once, to decide which brushes the world owns, and then
// thrown away. What stays resident is polygons and floats and a uniform grid
// built here -- see wr_bspgeom.h for why a grid and not the file's own tree.
// So the version-sensitive structs exist for the length of one function call,
// which is the only window in which Strata changing dnode_t again can hurt.
//
// THE GRID IS CHECKED AGAINST THE THING IT ACCELERATES
//
// An index that is wrong does not report an error. A ray that should hit
// returns "nothing ahead", which is byte for byte what a ray that correctly
// finds nothing returns -- and this feature's whole job is answering "what is
// ahead of me", where "nothing" is an ordinary answer. So the only way to know
// is to ask without the grid and require the same result.
//
// 260,800 rays over all 1,304 maps, from inside each map's own bounds, in
// every direction: ZERO disagreements with brute force. 26.7% of them hit
// something, so the comparison is not vacuous. And it is worth having --
// 0.3 microseconds a ray against 115.7, which is 401x, and cheap enough that a
// query every frame is not a thing anybody has to think about.

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

// WR_BSP_MAX_RESIDENT is what the polygons AND the grid are allowed to cost
// together, and it is the one of the three with real data behind it. Measured
// over all 1,304 maps: p50 2.25 MB, p90 4.05 MB, p99 6.41 MB, worst 10.50 MB
// (bhop_canals, 95,955 faces). 64 MB is therefore about six times the worst
// map in the library, which is the right kind of margin for a number whose job
// is to stop rather than to fit.
#define WR_BSP_MAX_LUMP        (128u * 1024u * 1024u)
#define WR_BSP_MAX_LUMPS_TOTAL (256u * 1024u * 1024u)
#define WR_BSP_MAX_RESIDENT    (64u * 1024u * 1024u)

// Sides on one brush. vbsp's own MAXPOINTS is 128 and the widest worldspawn
// brush measured across the corpus has 62.
#define WR_BSP_MAX_BRUSH_SIDES 128

// ---------------------------------------------------------------------------
// The seven lumps, and six more that are allowed to be absent
// ---------------------------------------------------------------------------
//
// The first seven are the collision model and every one of them is REQUIRED: a
// map missing any is refused by name, because a brush model with no brushsides
// is not a map this can be careful about.
//
// The six after them are the displacement surfaces, and they are OPTIONAL in
// the strict sense that more than half the library has no DISPINFO lump at all
// and those maps are perfectly good. They are also optional in a second sense
// that matters more: a lump version with no row in the stride table is SKIPPED
// rather than refused, and the rest of the map is read exactly as before.
// Absent displacements are a KNOWN GAP that hasDisplacements already reports; a
// refusal would turn a partial answer into no answer.
//
// v25 used to be the standing example of that, and is not any more. Strata
// published dface_t and ddispinfo_t and both are read now -- 4,966
// displacements across the 27 v25 maps in this library that have any. What
// stays true is the shape of the arrangement: the next version bump lands in
// the same skip, and says so on the panel, without this reader needing to know
// its number in advance.
enum
{
    WR_BSP_L_PLANES = 0,
    WR_BSP_L_NODES,
    WR_BSP_L_LEAFS,
    WR_BSP_L_MODELS,
    WR_BSP_L_LEAFBRUSHES,
    WR_BSP_L_BRUSHES,
    WR_BSP_L_BRUSHSIDES,
    WR_BSP_L_REQUIRED,          // everything below here may be missing

    WR_BSP_L_VERTEXES = WR_BSP_L_REQUIRED,
    WR_BSP_L_EDGES,
    WR_BSP_L_SURFEDGES,
    WR_BSP_L_FACES,
    WR_BSP_L_DISPINFO,
    WR_BSP_L_DISPVERTS,
    WR_BSP_L_ENTITIES,
    WR_BSP_L_COUNT
};

// WHY a displacement was dropped, by name.
//
// dispDropped was a single int, and only one of these seven sites adds more than
// one at a time -- so "did this map run out of build budget, or refuse a hundred
// displacements one at a time" could not be answered from it at all. It had to
// be recovered arithmetically, by replaying the per-displacement tests and
// solving for the break index, which is refusal-by-inference in the file whose
// whole convention is refusal-by-name.
//
// Measured over the 626 displacement maps in the library: five of these seven
// counters are ZERO everywhere, 88 of the 191 drops were CORNER, and the other
// 103 were BUDGET on surf_outra alone. That census is worth being able to
// re-derive on demand rather than out of a commit message.
enum
{
    WR_DISP_DROP_POWER = 0,   // power outside [WR_DISP_MIN_POWER, MAX_POWER]
    WR_DISP_DROP_FACEINDEX,   // m_iMapFace is not inside the FACES lump
    WR_DISP_DROP_VERTSTART,   // the grid's rows are not inside DISPVERTS
    WR_DISP_DROP_NOTQUAD,     // the base face does not have exactly 4 edges
    WR_DISP_DROP_FACEVERTEX,  // a corner would not walk out of SURFEDGES
    WR_DISP_DROP_CORNER,      // startPosition named no corner of that quad
    WR_DISP_DROP_BUDGET,      // WR_DISP_BUDGET ran out; the rest were not tried
    WR_DISP_DROP__COUNT
};

// The names, for the panel and the sweep. Indexed by the enum above.
extern const char *WrBspDispDropName[WR_DISP_DROP__COUNT];

// Which of them refused the most, so one sentence can name a cause instead of
// listing seven counters that are nearly always zero. Ties go to the earliest,
// which is arbitrary and harmless: a map with two causes tied has bigger
// problems than which one gets named. Returns WR_DISP_DROP_POWER on a map that
// dropped nothing, so callers must check dispDropped first.
struct WrBspMap;
int WrBspDispWorstDrop(const WrBspMap *m);

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

    // The DISPINFO lump's length, and nothing else about it.
    //
    // This is the eighth lump the directory is read for and the only one that is
    // never opened. All that is wanted is whether it is EMPTY, because that is
    // the difference between "this reader saw the whole map" and "this reader
    // saw the part of the map that is brushes" -- and a caller that vetoes a
    // kinematic answer with geometry has to know which of those it is holding.
    // Reading a length out of a directory entry that is already in a local
    // costs nothing; parsing the lump would be the displacement work this
    // version does not do.
    unsigned int   dispInfoBytes;
};

// Read them, or fail with a reason a person can act on. Never partially
// succeeds: on false, nothing is allocated and *out is zeroed.
bool WrBspReadRaw(const char *path, WrBspRaw *out, char *err, int errCap);
void WrBspFreeRaw(WrBspRaw *r);

// ---------------------------------------------------------------------------
// The fields, per version
// ---------------------------------------------------------------------------
//
// Everything above this line is bytes. Everything below reads a FIELD out of
// one of those structs, and every one of these was confirmed against the whole
// library rather than read off a header file -- 117,031,588 index references
// checked, none out of range. What Strata changed is here and nowhere else:
//
//     node children       int32 at 4 and 8. The same on all four versions,
//                         which is the one piece of luck in this format.
//     leaf brush range    u16 at 24/26 on stock, int32 at 44/48 on v25.
//     leafbrush           u16 on stock, u32 on v25 -- which is also what lets
//                         a v25 map hold more than 65,536 brushes.
//     brushside planenum  u16 at 0 on stock, int32 at 0 on v25.
//     brush, plane, model unchanged everywhere.
//
// Every one of these RANGE CHECKS its index and returns a refusal value rather
// than reading out of bounds, because every index in this file came out of
// somebody else's bytes.

int  WrBspNodeChild(const WrBspRaw *r, int node, int which);
bool WrBspLeafBrushRange(const WrBspRaw *r, int leaf, int *first, int *num);
int  WrBspLeafBrush(const WrBspRaw *r, int i);
bool WrBspBrush(const WrBspRaw *r, int i, int *firstSide, int *numSides,
                int *contents);
int  WrBspBrushSidePlane(const WrBspRaw *r, int side);
bool WrBspPlane(const WrBspRaw *r, int i, float out[4]);
int  WrBspModelHeadNode(const WrBspRaw *r, int model);

// ---------------------------------------------------------------------------
// Which brushes the world owns
// ---------------------------------------------------------------------------
//
// THE REASON THIS FUNCTION EXISTS AT ALL. A trigger brush carries
// CONTENTS_SOLID, verbatim, exactly as a ramp does. There is no flag, no
// contents bit and no side property anywhere in the file that separates a
// trigger_teleport volume from geometry you can stand on -- the distinction
// lives entirely in which MODEL owns the brush, and model ownership is only
// discoverable by walking a tree.
//
// It is not a small correction. On surf_greensway 47% of solid brushes are
// teleport volumes and 76% of surf-band area is entity-owned, and a teleport
// volume on a surf map is characteristically a large slanted box sitting under
// the ramp -- which is to say, squarely inside the surf band, at a
// ramp-shaped angle, in exactly the place a ramp would be. Skipping the walk
// does not produce garbage. It produces a second ramp.
//
// The walk costs something real and it is stated rather than buried. Measured
// over all 1,304 maps, model 0 owns 84.1% of all brushes -- p10 68.7%, median
// 86.6%, p90 95.4%. On the surf maps the 16% dropped splits into roughly 7.7%
// triggers, which is the whole point, and 4.7% genuine func_* geometry, which
// is a real loss: a ramp built as a func_detail belongs to its own model and
// will not be seen.
//
// And it is not uniform. bhop_slope_v2 is built almost entirely out of
// entities and the world owns 48 of its 1,351 brushes -- 3.6%. A map like that
// will read as nearly empty, so the panel has to show coverage rather than
// only geometry: "no ramp here" and "nothing was read here" have to look
// different or the feature lies by omission.
//
// `owned` must have room for count[WR_BSP_L_BRUSHES] bytes.
bool WrBspWorldBrushes(const WrBspRaw *r, unsigned char *owned, int *ownedOut,
                       char *err, int errCap);

// ---------------------------------------------------------------------------
// The map, as polygons
// ---------------------------------------------------------------------------
//
// What survives the parse. No node, no leaf, no lump and no version -- a face
// is four floats of plane and a run of vertices, and that is the whole of what
// stays resident. See wr_bspgeom.h for why the queries are over a grid built
// here rather than over the tree the file came with.

// Contents bits, from Valve's bspflags.h.
//
// BOTH are tested now, and for a long time only the first was. A playerclip
// brush is invisible geometry a mapper put there to smooth a ramp or block a
// shortcut -- so drawing one as though it were the ramp would put a surface on
// screen that nobody can see in game, and it was left out on that basis. The
// note here used to end "whether it should be included is a real question and
// it can be answered with a number; today it is not."
//
// It is now. There are 141,841 of them, they carry 26% more surf-band area than
// the solid brushes alone, and leaving them out meant this reader had no polygon
// within 48 units of a player who was demonstrably touching something a QUARTER
// of the time. The distinction that matters is not which brushes to read but
// which query may use them: collision yes, drawing no. See WR_BSP_POLY_CLIP.
#define WR_BSP_CONTENTS_SOLID      0x00000001
#define WR_BSP_CONTENTS_PLAYERCLIP 0x00010000

// This polygon came off a brush that is PLAYERCLIP and not solid.
//
// It collides and it cannot be seen. That difference decides which queries may
// use it: "is there anything where the player is" must, because a clip brush is
// frequently the thing a surf map is actually ridden on, and "draw the ramp
// surface" must not, because drawing a clip brush puts a surface on screen that
// nobody can see in game. See WrBspSurfNear, which filters, and
// WrBspNearestFace and WrBspTraceRay, which do not.
#define WR_BSP_POLY_CLIP 0x1u

// This polygon is one triangle of a displacement rather than a brush side.
//
// It collides and it is visible, so no query filters it out. It is flagged
// because a displacement is the one place this reader builds geometry that has
// no brush behind it, and anything measuring where the map came from -- coverage
// in the panel, the normal cross-check in bsp_sweep -- has to be able to tell
// the two apart.
#define WR_BSP_POLY_DISP 0x2u

// The most polygons WrBspSurfNear will return, and the top of the panel's own
// "how many faces" slider. The two have to agree: they did not, and the query's
// private 64 quietly won.
#define WR_BSP_SURF_MAX 512

struct WrBspPoly
{
    float plane[4];     // the side's own plane: normal in 0..2, distance in 3
    int   first;        // index of its first vertex in WrBspMap::verts
    int   count;
    float area;
    unsigned int flags; // WR_BSP_POLY_*
};

struct WrBspMap
{
    int  version;
    bool compressed;

    // Does this map use displacements at all?
    //
    // Carried up from WrBspRaw::dispInfoBytes, and it is the difference between
    // two quite different silences. When this is false, a trace that finds
    // nothing found nothing because there is nothing -- every solid surface in
    // the level is a brush and every brush was considered. When it is true, a
    // trace that finds nothing may simply have looked at the half of the map
    // this version does not read.
    //
    // Only a caller using absence as evidence needs this. Drawing what was
    // found does not, which is why nothing needed it until now.
    bool hasDisplacements;

    float mins[3], maxs[3];

    WrBspPoly *polys;
    int        polyCount;
    float    (*verts)[3];
    int        vertCount;

    // What was read, and -- the half that matters more -- what was not.
    // A panel that shows geometry without showing coverage is telling
    // somebody there is no ramp ahead when what happened is that nothing was
    // read. On a map like bhop_slope_v2 that is the difference between a
    // feature and a lie.
    int brushTotal;         // brushes in the file
    int brushWorld;         // owned by model 0
    int brushSolid;         // ... and CONTENTS_SOLID, so actually considered
    int brushClipOnly;      // playerclip and not solid: deliberately skipped
    int sideTotal;          // sides of those brushes
    int sideDropped;        // clipped away to nothing -- bevels, mostly
    int sideDegenerate;     // the plane was not a unit normal
    int sideNotClosed;      // failed the closure assertion, so refused
    int sideTooFar;         // a vertex outside the world, so refused

    int   surfPolys;        // in the surf band, facing up
    int   surfClipPolys;    // ...and of those, how many are clip-only

    int   dispPolys;        // triangles built from displacement surfaces
    int   dispDropped;      // ...and how many were skipped, all causes together
    int   dispTotal;        // how many the file declares, before any test
    int   dispDropBy[WR_DISP_DROP__COUNT];  // ...and which test refused each

    int   entModels;        // solid brush entities whose brushes were included
    int   entBrushes;       // and how many brushes that added on top of model 0
    float surfArea;
    float solidArea;

    // The uniform grid, as a bucketed index. cellStart has one entry per cell
    // plus a terminator, so cell c holds cellItems[cellStart[c]..cellStart[c+1]).
    // See wr_bspgeom.h for why a grid we build and not the tree the file came
    // with.
    WrBspGridDesc grid;
    int *cellStart;
    int *cellItems;
    int  itemCount;

    size_t bytes;           // what all of the above cost
};

// Turn the lumps into polygons. Reads nothing from disk; the caller owns `r`
// and may free it the moment this returns.
bool WrBspBuild(const WrBspRaw *r, WrBspMap *out, char *err, int errCap);
void WrBspFreeMap(WrBspMap *m);

// The threshold below which the panel shouts rather than mentions.
//
// Not a round number pulled out of the air. Across the library model 0 owns
// 84.1% of brushes with p10 at 68.7%, so two thirds is comfortably inside the
// ordinary range and nothing normal trips this. What does trip it is a map like
// bhop_slope_v2 at 3.6%, which is the case this exists for.
#define WR_BSP_THIN_OWNED 0.55f

// MAY ABSENCE BE USED AS EVIDENCE ON THIS MAP?
//
// True only when a map is built, world ownership is not thin, and every
// displacement in the file was actually built. Both halves take a map rather
// than reaching for the loaded one, because they are properties OF A MAP and
// because the switch that this whole reader's live behaviour turns on had, until
// it moved here, no test coverage in any harness at all -- WrBspMap is a plain
// struct and tests\test_bsp.exe can now fill seven ints in by hand.
//
// The loader keeps its no-argument wrappers; see wr_bspload.h for what a false
// answer does and, more importantly, does not mean.
bool WrBspCoverageThin(const WrBspMap *m);
bool WrBspGeometryComplete(const WrBspMap *m);

// Whether WrBspBuild keeps PLAYERCLIP brushes that are not also solid. False,
// which is what has always happened. A knob rather than a constant because the
// library holds 141,841 of them and whether they belong here is a question with
// a measurable answer -- see the note at its definition.
extern bool g_wrBspIncludeClip;

// Whether WrBspSurfNear -- the drawing query, and only that one -- will return
// those clip polygons as well.
//
// The reasoning above says drawing a clip brush puts a surface on screen that
// cannot be seen in game, and that is true. What it did not weigh is what the
// alternative shows: on the maps built that way there is nothing else to draw.
// surf_ethereal is 233 of its 342 world brushes playerclip-only and
// surf_greensway 317 of 579, and on those the panel would report a healthy
// surf-band count while the screen stayed empty -- because surfPolys counts them
// and this query threw them away.
//
// An invisible brush that you ride is still the ramp. Drawn in its own style so
// it is not mistaken for a face you could see.
extern bool g_wrBspDrawClip;

// Whether WrBspBuild subdivides displacement surfaces at all. On. A knob for
// the same reason the two above are knobs: whether the geometry it adds agrees
// with the geometry players actually collide with is a question with a
// measurable answer, and measuring it means being able to turn it off.
extern bool g_wrBspBuildDisp;

// Whether brushes owned by solid brush ENTITIES are read as well as the
// world's. On. Off is what every version before this did, and the difference is
// measurable per map -- see WrBspEntityBrushes.
extern bool g_wrBspIncludeEntities;

// Running total of brush entities refused as non-solid -- triggers, mostly.
// Read by bsp_sweep so the deny list can be checked against a corpus rather
// than trusted.
extern int g_wrBspEntSkipped;

// A polygon's vertices, for a caller that would rather not do the arithmetic.
static inline const float (*WrBspPolyVerts(const WrBspMap *m, int i))[3]
{
    return m->verts + m->polys[i].first;
}

static inline unsigned int WrBspPolyFlags(const WrBspMap *m, int i)
{
    return m->polys[i].flags;
}

// ---------------------------------------------------------------------------
// The three questions anybody asks of it
// ---------------------------------------------------------------------------
//
// All three go through the grid, and all three are also implemented brute
// force in tests\test_bsp.exe and compared -- an accelerator that disagrees
// with the thing it accelerates is the failure mode here, and it is a quiet
// one: a ray that misses returns "nothing ahead", which is exactly what a ray
// that correctly finds nothing returns.
//
// None of these calls into the game, and none of them is a trace. They are
// arithmetic over a list of polygons read off a file.

// WHAT IS AHEAD. The nearest front-facing polygon along a ray. This is the
// query behind "what angle is that ramp, before I reach it".
//
// Front faces only, which does a second job worth naming: it is what stops a
// ramp's UNDERSIDE being reported as the ramp when the query starts below it.
bool WrBspTraceRay(const WrBspMap *m, const float start[3], const float dir[3],
                   float maxDist, int *polyOut, float *tOut);

// WHAT IS AROUND. Surf-band polygons within `radius` of a point, deduplicated,
// nearest first. This is the query behind drawing the ramp surface itself.
// Returns how many were written, which may be less than found if cap is small.
int WrBspSurfNear(const WrBspMap *m, const float pt[3], float radius,
                  int *out, int cap);

// WHAT DID I TOUCH. The nearest polygon of any facing, for confirming a normal
// that wr_phase.h recovered from a velocity trace. The contact point for that
// is a player ORIGIN a few units off the surface rather than a point on it,
// which is why this measures distance to the polygon and not to its plane.
bool WrBspNearestFace(const WrBspMap *m, const float pt[3], float radius,
                      int *polyOut, float *distOut);

// THE SAME WALK, ALSO ANSWERING "WHICH OF THOSE COULD I BE RIDING".
//
// "Nearest of any facing" is the right question for whether the player is
// touching something, and it was measured as such: R = 24 scores 98.3% at
// wr_bspload.h. It is the WRONG question for which plane a board should be
// graded against, and the difference is not theoretical. Land on a ramp near a
// wall and the wall is nearer; the board's ramp-band gate then refuses it, and
// because there was no second candidate the whole board vanished with no
// message. Reported as ramps that "sometimes don't report numbers", landing
// from the side.
//
// So this reports both, from one pass: `polyOut`/`distOut` are byte-identical
// to WrBspNearestFace, because the touch verdict rests on a measurement taken
// with exactly that rule and may not move without a new one; `rampOut` is the
// nearest polygon that passes WrBspIsRampPlane, or -1 when none does.
//
// A ramp answer is not a claim that the player is on it. It is only the best
// candidate available -- the caller still owns the distance and the grade.
bool WrBspNearestFaceEx(const WrBspMap *m, const float pt[3], float radius,
                        int *polyOut, float *distOut,
                        int *rampOut, float *rampDistOut);

#endif // WR_BSP_H
