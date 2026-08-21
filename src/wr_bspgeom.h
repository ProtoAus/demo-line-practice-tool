// wr_bspgeom.h  --  a brush turned into polygons, and the queries over them.
//
// WHY THIS IS A SEPARATE FILE FROM THE READER
//
// Everything here is arithmetic on floats: no file, no thread, no allocation.
// That is the same boundary wr_mtv.h draws between "is this the right byte" and
// everything downstream of it. A byte is right or it is not and a test can say
// so instantly; geometry is where the mistakes are subtle, and the only way to
// catch those is to run it against shapes whose answer you already know. So the
// clipper and the ray live here, static inline, and tests\test_bspgeom.exe
// drives them with hand-checkable brushes while linking nothing at all -- the
// same arrangement as wr_scale.h, wr_stress.h and wr_phase.h.
//
// WHAT THE GEOMETRY IS FOR
//
// wr_phase.h can tell you what surface the player DID touch, exactly, from the
// fact that free flight has vertical acceleration -g. What it cannot do is say
// anything about a ramp nobody has hit yet, because there is no measurement of
// a surface you have not collided with. Its own header says so. This is the
// other half: the map file already contains every plane in the level, and
// reading it is ordinary read-only file I/O of exactly the kind this tool
// already does.
//
// BRUSHES, NOT FACES
//
// A Source map stores both, and they do not agree. Measured across the corpus:
// surf_inner reads 0% brush-backed by faces and 100% by brushes, and
// surf_ethereal has 2,692 surf-band brushsides against zero surf-band faces.
// FACES is a rendering structure -- it does not exist for surfaces with no
// visible material, which on a surf map includes a great deal of what you
// actually ride. BRUSHES/BRUSHSIDES is the collision structure, and it is also
// the one Strata changed least.
//
// A brush is an intersection of half-spaces, so its faces are not stored at
// all: each side is a plane, and the polygon is what you get by starting with
// an enormous quad on that plane and clipping it against every other side. That
// is what WrBspStartQuad and WrBspClipToPlane are for, and it is why bevel
// sides need no flag to exclude them. A bevel is tangent to an edge of the
// brush rather than bounding a face, so clipping reduces it to a slver or to
// nothing on its own -- 50 to 65% of all brushsides are bevels, and dropping
// them is a consequence of doing the clip properly rather than a rule anybody
// has to trust.

#ifndef WR_BSPGEOM_H
#define WR_BSPGEOM_H

#include <math.h>

// ---------------------------------------------------------------------------
// What counts as a ramp
// ---------------------------------------------------------------------------

// The surf band, as a normal's z component.
//
// The upper bound is not a taste of ours: it is WR_PHASE_STANDABLE, which is
// 0.7 because CGameMovement refuses to stand on a plane below it -- that is
// what makes a surf ramp a surf ramp. The same number has to mean the same
// thing on both sides of this tool, or the kinematic reading and the geometry
// reading would disagree about what a ramp IS before they ever disagreed about
// an angle. 0.7 is 45.6 degrees; 0.1 is 84.3, past which it is a wall rather
// than something you could board.
//
// The header is deliberately not included -- this one links nothing -- so the
// constant is repeated and tests\test_bspgeom.exe asserts the two agree.
#define WR_BSP_BAND_LO 0.10f
#define WR_BSP_BAND_HI 0.70f

// STRICTLY POSITIVE z, which a |n.z| band is not, and the difference is not
// cosmetic. A brush's underside has n.z = -0.63 and sits inside the |n.z| band
// while being a ceiling. Measured over 25 maps: 39,376 up-facing surf-band
// polygons against 19,702 down-facing ones, so taking the absolute value would
// have doubled the drawn set with surfaces nobody can ride.
static inline bool WrBspIsSurfBand(float nz)
{
    return nz >= WR_BSP_BAND_LO && nz <= WR_BSP_BAND_HI;
}

// AND THE |n.z| VERSION, WHICH IS NOT THE SAME QUESTION.
//
// The one above answers "may this be drawn as a ramp", and there the sign is
// load-bearing: a ceiling is not something anybody rides. This one answers "is
// this the plane a board should be graded against", and there the sign carries
// no information at all, for two separate reasons.
//
// A displacement triangle's normal is a cross product of its grid winding
// (wr_bsp.cpp, the disp builder) and nothing orients it, so half of a
// displacement ramp can come out facing down while being the surface you are
// standing on. Refusing those would refuse displacement ramps at random.
//
// And a board does not need the sign anyway: the two faces of one plane make
// the same angle with an incoming velocity, and WrPhaseBoard orients the normal
// towards whoever arrived on it before it grades anything.
//
// This is the same test WrEnergyTickBoards already applies to the normal it is
// handed. It is written here so the query that CHOOSES the polygon and the gate
// that later JUDGES it cannot drift apart -- picking a wall and then refusing it
// is how a real board becomes silence.
static inline bool WrBspIsRampPlane(float nz)
{
    const float a = nz < 0.0f ? -nz : nz;
    return a >= WR_BSP_BAND_LO && a <= WR_BSP_BAND_HI;
}

// Tilt from horizontal, in degrees. 0 is a floor, 90 a wall. Unlike
// WrPhaseSurfaceAngle this does NOT fold the sign, because here we know which
// way the surface faces and a ceiling is not a ramp.
static inline float WrBspSurfaceAngle(const float n[3])
{
    float nz = n[2];
    if (nz > 1.0f) nz = 1.0f;
    if (nz < -1.0f) nz = -1.0f;
    return (float)(acos(nz) * 57.29577951308232);
}

// ---------------------------------------------------------------------------
// Brush -> polygons
// ---------------------------------------------------------------------------

// Vertices one clipped side may have before it is refused. A convex polygon
// cannot have more vertices than the brush has other sides, and the widest
// worldspawn brush measured across the corpus has 62.
#define WR_BSP_MAX_POLY_VERTS 128

// Half-width of the starting quad. Source's world limit is +-16384 and Strata's
// maps reach past 31000 (see WR_WORLD_LIMIT), so this is comfortably larger
// than any coordinate a plane can pass through, and a float still resolves
// about 0.004 units at this magnitude.
#define WR_BSP_HUGE 65536.0f

// Valve's own ON_EPSILON, and the value vbsp itself chops windings with.
// Matched rather than tightened: at 65536-scale a smaller epsilon is below what
// a float can represent, so tightening it starts keeping rounding error as
// vertices instead of removing them.
#define WR_BSP_ON_EPSILON 0.1f

// Below this a polygon is discarded, in square units. This is what turns
// "exclude bevel sides" from a flag into a consequence: a bevel is tangent to
// a brush edge, so clipping it against the real sides leaves a sliver or
// nothing.
#define WR_BSP_MIN_AREA 1.0f

// ---------------------------------------------------------------------------
// Which corner a displacement grid starts at
// ---------------------------------------------------------------------------

// How far startPosition may sit from the corner it names, as a fraction of the
// base quad's own longest separation, squared.
//
// RELATIVE TO THE QUAD, AND THAT IS A FIX RATHER THAN A LOOSENING. The rule
// this replaces was "within 0.25 square units", written on the premise -- stated
// in the comment it replaced -- that vbsp copies startPosition out of the corner
// itself, so a real match is exact to floating point. Measured over the library
// that premise is false: of 172,759 displacements across 1,304 maps only
// 134,883 (78.1%) match a corner exactly, 172,671 are inside 0.25, and 88 are
// not. Worst miss 10.67 units, surf_simpsons2 disp 158 and 170.
//
// Those 88 are 0.051% of the library and they were expensive out of all
// proportion: they fall on 16 maps, and a single one of them switched the live
// map query off for that whole level, because WrBspGeometryComplete refuses any
// map that dropped a displacement. surf_kvas dropped 4 of 756 and lost two
// ~290,000 sq unit ramp faces out of an otherwise fully built ramp complex --
// which on screen was the strafe readout saying "no surface" on every ramp.
//
// The absolute cut was judging a quantity that scales with the face. surf_kvas's
// base quads are 512x704 and 833x605 units, so its one-unit miss is 0.12% of the
// diagonal and the face was thrown away for it. Against the quad's own diagonal
// all 172,759 pass with 2.7x headroom: the worst real case in the library is
// 1.42e-05 of diagSq, 0.376% of its own diagonal, on surf_happyhands3 disp 476.
//
// AND IT IS STILL THE STRIDE CHECK IT WAS WRITTEN TO BE, which is the half that
// had to be measured before this could be relaxed. Re-read the whole library
// with startPosition taken four bytes late -- exactly the "the struct prefix
// moved and these fields are not where this thinks" failure the test exists for
// -- and this rule refuses 172,793 of 172,801, passing only the same 8
// degenerate coincidences the 0.25 rule also passes. Eight against eight: the
// anti-stride property is not weakened at all. Under that wrong read the worst
// case is 4.46e+07 of diagSq, twelve orders of magnitude clear of the worst
// correct one, so the two populations do not touch.
#define WR_DISP_CORNER_SLACK 1e-4f

// Rotate the base quad so the corner startPosition names is first.
//
// ddispinfo_t's startPosition is a POINT, not an index, and it is one of the
// four corners of the face's winding. Getting this wrong does not produce
// nonsense, it produces a surface rotated a quarter turn on its own base, which
// is exactly the kind of plausible-looking wrong answer this reader refuses to
// ship -- so the match is checked rather than assumed.
//
// `slackOut` receives bestD / diagSq, the quantity the cut is applied to.
// `marginOut` receives secondD / bestD, how far the chosen corner beat the
// runner-up. That second one is not decoration: relaxing the cut is only safe
// while the CHOICE of corner stays unambiguous, and it is measured to be --
// 244,868x to 954,273x on the four surf_kvas cases, and never below 9.0x
// anywhere in the library (surf_epiphany disp 508). A future vbsp with more
// distorted base quads could close that gap, so test_bspgeom asserts on it and
// that degrades loudly rather than silently.
static inline bool WrBspDispBaseCorner(const float quad[4][3],
                                       const float startPos[3],
                                       int *baseOut, float *slackOut,
                                       float *marginOut)
{
    int base = 0;
    float bestD = 1e30f, secondD = 1e30f;
    for (int k = 0; k < 4; k++)
    {
        float d = 0.0f;
        for (int a = 0; a < 3; a++)
        {
            const float e = quad[k][a] - startPos[a];
            d += e * e;
        }
        if (d < bestD)      { secondD = bestD; bestD = d; base = k; }
        else if (d < secondD) secondD = d;
    }

    // The longest of all SIX pairwise separations, not the two diagonals. A
    // degenerate winding can put the longest span on an edge, and a cut that
    // shrank to nothing there would refuse a face for being thin rather than
    // for being the wrong face.
    float diagSq = 0.0f;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
        {
            float d = 0.0f;
            for (int a = 0; a < 3; a++)
            {
                const float e = quad[i][a] - quad[j][a];
                d += e * e;
            }
            if (d > diagSq) diagSq = d;
        }

    if (baseOut)   *baseOut   = base;
    if (slackOut)  *slackOut  = (diagSq > 0.0f) ? bestD / diagSq : 1e30f;
    if (marginOut) *marginOut = (bestD > 0.0f) ? secondD / bestD : 1e30f;

    // A quad with no extent is not a quad. Refused by name rather than divided
    // by: 0/0 here would accept every startPosition ever written, and the
    // refusal would silently turn into its opposite.
    if (!(diagSq > 0.0f))
        return false;

    return bestD <= WR_DISP_CORNER_SLACK * diagSq;
}

// A plane is float[4]: normal in 0..2, distance in 3, so a point is INSIDE the
// half-space when dot(v, n) - dist <= 0. That is Source's convention and the
// brush is the intersection of its sides' insides.

static inline float WrBspDot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline float WrBspPlaneDist(const float p[4], const float v[3])
{
    return WrBspDot(p, v) - p[3];
}

// The starting quad: a WR_BSP_HUGE square lying on plane p, wound so that its
// own normal is p's. Returns 4, or 0 if p's normal is not usable.
//
// Degenerate planes are real and have to be refused here rather than crashed
// on: 807 planes across four v25 maps are exactly (0,0,0) with dist 0. They are
// referenced by no brushside and no node, so they are harmless -- but only to
// a reader that checks.
static inline int WrBspStartQuad(const float p[4], float out[][3])
{
    float len = (float)sqrt(WrBspDot(p, p));
    if (!(len > 0.9f && len < 1.1f))
        return 0;                       // not a unit normal: refuse it

    // An up vector that is not parallel to the normal, then orthogonalised.
    // Choosing on the MAJOR axis rather than the minor is what Valve does and
    // it is the stable choice: the axis with the largest component is the one
    // guaranteed not to be nearly parallel to the pair we build from.
    float up[3] = { 0.0f, 0.0f, 0.0f };
    float ax = p[0] < 0.0f ? -p[0] : p[0];
    float ay = p[1] < 0.0f ? -p[1] : p[1];
    float az = p[2] < 0.0f ? -p[2] : p[2];
    if (az > ax && az > ay) up[0] = 1.0f;
    else                    up[2] = 1.0f;

    float d = WrBspDot(up, p);
    up[0] -= d * p[0];
    up[1] -= d * p[1];
    up[2] -= d * p[2];
    float ul = (float)sqrt(WrBspDot(up, up));
    if (!(ul > 1e-6f))
        return 0;
    up[0] /= ul; up[1] /= ul; up[2] /= ul;

    // right = up x n, so that the winding below comes out with normal +n.
    float right[3];
    right[0] = up[1] * p[2] - up[2] * p[1];
    right[1] = up[2] * p[0] - up[0] * p[2];
    right[2] = up[0] * p[1] - up[1] * p[0];

    float org[3] = { p[0] * p[3], p[1] * p[3], p[2] * p[3] };
    for (int i = 0; i < 3; i++)
    {
        up[i] *= WR_BSP_HUGE;
        right[i] *= WR_BSP_HUGE;
    }

    // p0 -> p1 -> p2 has edges -2*up then +2*right, whose cross product is
    // -4*(up x right) = +4n. Getting this order wrong inverts every polygon in
    // the map, which is exactly the kind of thing that still looks plausible.
    for (int i = 0; i < 3; i++)
    {
        out[0][i] = org[i] - right[i] + up[i];
        out[1][i] = org[i] - right[i] - up[i];
        out[2][i] = org[i] + right[i] - up[i];
        out[3][i] = org[i] + right[i] + up[i];
    }
    return 4;
}

// Keep the half-space BEHIND plane p, which is the inside of a Source brush.
// Returns the new vertex count, which may be 0.
static inline int WrBspClipToPlane(const float in[][3], int n, const float p[4],
                                   float out[][3])
{
    if (n < 3 || n > WR_BSP_MAX_POLY_VERTS)
        return 0;

    float d[WR_BSP_MAX_POLY_VERTS + 1];
    signed char side[WR_BSP_MAX_POLY_VERTS + 1];   // -1 back, 0 on, +1 front
    int front = 0, back = 0;

    for (int i = 0; i < n; i++)
    {
        d[i] = WrBspPlaneDist(p, in[i]);
        if (d[i] > WR_BSP_ON_EPSILON)       { side[i] = 1;  front++; }
        else if (d[i] < -WR_BSP_ON_EPSILON) { side[i] = -1; back++; }
        else                                  side[i] = 0;
    }
    d[n] = d[0];
    side[n] = side[0];

    // Wholly inside. The COPY matters: the caller ping-pongs two buffers and
    // swaps after every call, so returning the count without writing `out`
    // hands back whatever the other buffer happened to hold two clips ago. It
    // still has the right vertex count and still looks like a polygon, which
    // is why it survived until a box came out the wrong size.
    if (!front)
    {
        if (!back)
            return 0;           // wholly ON the plane, so degenerate
        for (int i = 0; i < n; i++)
        {
            out[i][0] = in[i][0];
            out[i][1] = in[i][1];
            out[i][2] = in[i][2];
        }
        return n;
    }
    if (!back)
        return 0;               // wholly outside

    int m = 0;
    for (int i = 0; i < n; i++)
    {
        if (m >= WR_BSP_MAX_POLY_VERTS)
            return 0;           // refuse rather than truncate a shape

        if (side[i] != 1)       // keep back and on
        {
            out[m][0] = in[i][0]; out[m][1] = in[i][1]; out[m][2] = in[i][2];
            m++;
        }
        if (side[i] == 0 || side[i + 1] == 0 || side[i + 1] == side[i])
            continue;

        // Crossing. The interpolation is written so a vertex on the plane comes
        // out exactly on it rather than an epsilon off, which is what stops
        // successive clips from drifting.
        const float *a = in[i];
        const float *b = in[(i + 1) % n];
        float t = d[i] / (d[i] - d[i + 1]);
        if (m >= WR_BSP_MAX_POLY_VERTS)
            return 0;
        for (int k = 0; k < 3; k++)
        {
            if (p[k] == 1.0f)       out[m][k] = p[3];
            else if (p[k] == -1.0f) out[m][k] = -p[3];
            else                    out[m][k] = a[k] + t * (b[k] - a[k]);
        }
        m++;
    }
    return m < 3 ? 0 : m;
}

// Area of a convex polygon, by fan triangulation from its first vertex.
static inline float WrBspPolyArea(const float v[][3], int n)
{
    if (n < 3)
        return 0.0f;
    float total = 0.0f;
    for (int i = 1; i + 1 < n; i++)
    {
        float a[3], b[3], c[3];
        for (int k = 0; k < 3; k++)
        {
            a[k] = v[i][k] - v[0][k];
            b[k] = v[i + 1][k] - v[0][k];
        }
        c[0] = a[1] * b[2] - a[2] * b[1];
        c[1] = a[2] * b[0] - a[0] * b[2];
        c[2] = a[0] * b[1] - a[1] * b[0];
        total += 0.5f * (float)sqrt(WrBspDot(c, c));
    }
    return total;
}

// The normal a polygon's own winding implies, which is how a test tells whether
// WrBspStartQuad wound it the right way round. False when it is degenerate.
static inline bool WrBspPolyNormal(const float v[][3], int n, float out[3])
{
    if (n < 3)
        return false;
    float acc[3] = { 0.0f, 0.0f, 0.0f };
    for (int i = 1; i + 1 < n; i++)
    {
        float a[3], b[3];
        for (int k = 0; k < 3; k++)
        {
            a[k] = v[i][k] - v[0][k];
            b[k] = v[i + 1][k] - v[0][k];
        }
        acc[0] += a[1] * b[2] - a[2] * b[1];
        acc[1] += a[2] * b[0] - a[0] * b[2];
        acc[2] += a[0] * b[1] - a[1] * b[0];
    }
    float l = (float)sqrt(WrBspDot(acc, acc));
    if (!(l > 1e-9f))
        return false;
    out[0] = acc[0] / l; out[1] = acc[1] / l; out[2] = acc[2] / l;
    return true;
}

// Is every vertex behind every side of the brush it came from?
//
// THE CLOSURE ASSERTION, and the strongest single check the reader has. A brush
// is the intersection of its half-spaces, so a face of it cannot stick out
// through another side. If one does, the sides were not the sides of that
// brush -- which is what a wrong struct stride produces, and it produces it
// while still looking like geometry. skipSide is the plane the polygon lies on,
// which it is exactly on rather than behind.
static inline bool WrBspPolyClosed(const float v[][3], int n,
                                   const float (*sides)[4], int numSides,
                                   int skipSide)
{
    if (n < 3 || numSides < 4)
        return false;
    const float tol = 2.0f * WR_BSP_ON_EPSILON;
    for (int s = 0; s < numSides; s++)
    {
        if (s == skipSide)
            continue;
        for (int i = 0; i < n; i++)
            if (WrBspPlaneDist(sides[s], v[i]) > tol)
                return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

// Is a point on the plane inside the polygon? Convex, so it is inside when it
// is left of every edge with respect to the polygon's own normal.
static inline bool WrBspPointInPoly(const float pt[3], const float n[3],
                                    const float v[][3], int nv)
{
    for (int i = 0; i < nv; i++)
    {
        const float *a = v[i];
        const float *b = v[(i + 1) % nv];
        float e[3], r[3], c[3];
        for (int k = 0; k < 3; k++)
        {
            e[k] = b[k] - a[k];
            r[k] = pt[k] - a[k];
        }
        c[0] = e[1] * r[2] - e[2] * r[1];
        c[1] = e[2] * r[0] - e[0] * r[2];
        c[2] = e[0] * r[1] - e[1] * r[0];
        if (WrBspDot(c, n) < -WR_BSP_ON_EPSILON)
            return false;
    }
    return true;
}

// A ray against one convex polygon.
//
// FRONT FACES ONLY -- dot(dir, n) must be negative. That is the ordinary
// solid-from-outside rule, and here it does a second job worth naming: it is
// what stops a ramp's UNDERSIDE being reported as the ramp when the query
// starts below it.
static inline bool WrBspRayPoly(const float start[3], const float dir[3],
                                const float p[4], const float v[][3], int n,
                                float *t)
{
    if (n < 3)
        return false;
    float denom = WrBspDot(dir, p);
    if (denom > -1e-6f)
        return false;                   // parallel, or hitting the back

    float dist = -WrBspPlaneDist(p, start) / denom;
    if (dist < 0.0f)
        return false;                   // behind the start

    float hit[3];
    for (int k = 0; k < 3; k++)
        hit[k] = start[k] + dir[k] * dist;

    if (!WrBspPointInPoly(hit, p, v, n))
        return false;

    if (t)
        *t = dist;
    return true;
}

// Distance from a point to a convex polygon.
//
// The plane distance when the projection lands inside it, and the distance to
// the nearest edge otherwise. Both cases are needed: what this confirms is a
// kinematically recovered normal, and the contact point for that is a player
// ORIGIN a few units off the surface rather than a point on it.
static inline float WrBspPointPolyDist(const float pt[3], const float p[4],
                                       const float v[][3], int n)
{
    if (n < 3)
        return 1e30f;

    float sd = WrBspPlaneDist(p, pt);
    float proj[3];
    for (int k = 0; k < 3; k++)
        proj[k] = pt[k] - sd * p[k];

    if (WrBspPointInPoly(proj, p, v, n))
        return sd < 0.0f ? -sd : sd;

    float best = 1e30f;
    for (int i = 0; i < n; i++)
    {
        const float *a = v[i];
        const float *b = v[(i + 1) % n];
        float e[3], r[3];
        for (int k = 0; k < 3; k++)
        {
            e[k] = b[k] - a[k];
            r[k] = pt[k] - a[k];
        }
        float ee = WrBspDot(e, e);
        float t = ee > 1e-9f ? WrBspDot(r, e) / ee : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float dsq = 0.0f;
        for (int k = 0; k < 3; k++)
        {
            float c = r[k] - e[k] * t;
            dsq += c * c;
        }
        if (dsq < best)
            best = dsq;
    }
    return (float)sqrt(best);
}

// Degrees between two unit vectors. The whole of the cross-check's answer.
static inline float WrBspAngleBetween(const float a[3], const float b[3])
{
    float d = WrBspDot(a, b);
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    return (float)(acos(d) * 57.29577951308232);
}

// ---------------------------------------------------------------------------
// A uniform grid, as pure index arithmetic
// ---------------------------------------------------------------------------
//
// A grid, and NOT the file's own BSP tree, and that is a deliberate refusal.
// Walking NODES at query time would mean keeping the node struct resident and
// correct for every map version for ever -- and Strata already changed dnode_t
// from 32 bytes to 48. A grid we build ourselves has no version, is sixty lines,
// and can be checked against brute force in a test. Nothing shaped like a node
// survives the parse.
//
// Cells are NON-CUBIC: the dimensions are clamped per axis and the cell size is
// extent/dims, so a long thin map does not get a grid that is one cell wide.

#define WR_BSP_CELL_TARGET 512.0f
#define WR_BSP_GRID_MAX_DIM 64      // 64^3 cells is 1 MB of int, and enough

struct WrBspGridDesc
{
    float mins[3], cell[3];
    int dims[3];
};

static inline void WrBspGridFit(const float mins[3], const float maxs[3],
                                WrBspGridDesc *g)
{
    for (int k = 0; k < 3; k++)
    {
        float ext = maxs[k] - mins[k];
        if (!(ext > 1.0f))
            ext = 1.0f;
        int d = (int)(ext / WR_BSP_CELL_TARGET) + 1;
        if (d < 1) d = 1;
        if (d > WR_BSP_GRID_MAX_DIM) d = WR_BSP_GRID_MAX_DIM;
        g->dims[k] = d;
        g->mins[k] = mins[k];
        g->cell[k] = ext / (float)d;
    }
}

static inline int WrBspGridIndex(const WrBspGridDesc *g, int x, int y, int z)
{
    return x + g->dims[0] * (y + g->dims[1] * z);
}

static inline int WrBspGridCellCount(const WrBspGridDesc *g)
{
    return g->dims[0] * g->dims[1] * g->dims[2];
}

// The cell a point falls in, clamped into the grid. Returns 0 when the point
// was outside and had to be clamped, which a caller may want to know.
static inline int WrBspGridCell(const WrBspGridDesc *g, const float pt[3],
                                int out[3])
{
    int inside = 1;
    for (int k = 0; k < 3; k++)
    {
        int c = (int)((pt[k] - g->mins[k]) / g->cell[k]);
        if (c < 0) { c = 0; inside = 0; }
        if (c >= g->dims[k]) { c = g->dims[k] - 1; inside = 0; }
        out[k] = c;
    }
    return inside;
}

// One walk of a ray across the grid. Amanatides and Woo, with the ray clipped
// to the grid's own bounds first so a query that starts outside the map still
// visits the cells it passes through.
struct WrBspDda
{
    int c[3], step[3];
    float tMax[3], tDelta[3];
    float t, tEnd;
};

static inline bool WrBspDdaBegin(const WrBspGridDesc *g, const float start[3],
                                 const float dir[3], float maxDist,
                                 WrBspDda *w)
{
    // Slab clip against the grid box.
    float t0 = 0.0f, t1 = maxDist;
    for (int k = 0; k < 3; k++)
    {
        float lo = g->mins[k];
        float hi = g->mins[k] + g->cell[k] * (float)g->dims[k];
        if (dir[k] > -1e-9f && dir[k] < 1e-9f)
        {
            if (start[k] < lo || start[k] > hi)
                return false;
            continue;
        }
        float inv = 1.0f / dir[k];
        float a = (lo - start[k]) * inv;
        float b = (hi - start[k]) * inv;
        if (a > b) { float tmp = a; a = b; b = tmp; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1)
            return false;
    }

    float p[3];
    for (int k = 0; k < 3; k++)
        p[k] = start[k] + dir[k] * t0;

    WrBspGridCell(g, p, w->c);
    w->t = t0;
    w->tEnd = t1;

    for (int k = 0; k < 3; k++)
    {
        if (dir[k] > 1e-9f)
        {
            w->step[k] = 1;
            float next = g->mins[k] + g->cell[k] * (float)(w->c[k] + 1);
            w->tMax[k] = t0 + (next - p[k]) / dir[k];
            w->tDelta[k] = g->cell[k] / dir[k];
        }
        else if (dir[k] < -1e-9f)
        {
            w->step[k] = -1;
            float next = g->mins[k] + g->cell[k] * (float)w->c[k];
            w->tMax[k] = t0 + (next - p[k]) / dir[k];
            w->tDelta[k] = -g->cell[k] / dir[k];
        }
        else
        {
            w->step[k] = 0;
            w->tMax[k] = 1e30f;
            w->tDelta[k] = 1e30f;
        }
    }
    return true;
}

// Advance one cell. False when the walk has left the grid or run past tEnd.
static inline bool WrBspDdaNext(const WrBspGridDesc *g, WrBspDda *w)
{
    int k = 0;
    if (w->tMax[1] < w->tMax[k]) k = 1;
    if (w->tMax[2] < w->tMax[k]) k = 2;

    if (w->step[k] == 0 || w->tMax[k] > w->tEnd)
        return false;

    w->c[k] += w->step[k];
    if (w->c[k] < 0 || w->c[k] >= g->dims[k])
        return false;

    w->t = w->tMax[k];
    w->tMax[k] += w->tDelta[k];
    return true;
}

#endif // WR_BSPGEOM_H
