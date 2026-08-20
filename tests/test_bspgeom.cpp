// test_bspgeom.cpp  --  a brush turned into polygons, checked against shapes
// whose answers are known by hand.
//
// WHAT GOES WRONG WITHOUT THIS
//
// A wrong struct stride in the BSP reader does not produce an error. It
// produces GEOMETRY -- plausible-looking planes, in plausible-looking places,
// with plausible-looking angles -- and the only thing that catches it is
// asking whether the shape closes. So the closure assertion is the load-bearing
// check here and it is tested first: a brush is the intersection of its
// half-spaces, so no face of it can stick out through another of its sides.
//
// The other three are mistakes I would have made and one I did:
//
// WINDING. WrBspStartQuad has to produce a quad whose own normal is the plane's.
// Get the vertex order wrong and every polygon in the map is inside out --
// which still draws, still has an angle, and silently inverts the front-face
// test that stops a ramp's underside being reported as the ramp.
//
// THE BAND IS SIGNED. A ceiling at n.z = -0.63 is inside a |n.z| band and is
// not a ramp. Measured over 25 maps, taking the absolute value would have
// doubled the drawn set with surfaces nobody can ride.
//
// BEVELS ARE NOT A FLAG. Half to two thirds of all brushsides are bevel planes
// tangent to an edge rather than bounding a face. Nothing marks them; they fall
// out of a correct clip as slivers or as nothing, and the test below builds one
// deliberately to prove the clipper removes it.
//
// THIS LINKS NOTHING
//
// wr_bspgeom.h is static inline over math.h, like wr_scale.h, wr_stress.h and
// wr_phase.h.
//
// Build:  tests\build.bat
// Run:    tests\test_bspgeom.exe

#include "wr_bspgeom.h"
#include "wr_phase.h"       // to assert the two agree about what a ramp is

#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

typedef float Poly[WR_BSP_MAX_POLY_VERTS][3];

// Clip a brush's side into its polygon: the standard pipeline, written out here
// so the test drives exactly what the reader will.
static int SideToPoly(const float (*sides)[4], int numSides, int which,
                      float out[][3])
{
    static float a[WR_BSP_MAX_POLY_VERTS][3];
    static float b[WR_BSP_MAX_POLY_VERTS][3];

    int n = WrBspStartQuad(sides[which], a);
    if (!n)
        return 0;

    float (*cur)[3] = a, (*nxt)[3] = b;
    for (int s = 0; s < numSides && n; s++)
    {
        if (s == which)
            continue;
        n = WrBspClipToPlane(cur, n, sides[s], nxt);
        float (*t)[3] = cur; cur = nxt; nxt = t;
    }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++)
            out[i][k] = cur[i][k];
    return n;
}

// An axis-aligned box as six planes, in Source's convention: inside is behind.
static void MakeBox(float (*s)[4], const float mins[3], const float maxs[3])
{
    memset(s, 0, sizeof(float) * 4 * 6);
    for (int k = 0; k < 3; k++)
    {
        s[k * 2 + 0][k] = 1.0f;  s[k * 2 + 0][3] = maxs[k];
        s[k * 2 + 1][k] = -1.0f; s[k * 2 + 1][3] = -mins[k];
    }
}

int main(void)
{
    printf("\n=== wrlines brush geometry ===\n\n");

    // -----------------------------------------------------------------------
    printf("the starting quad is wound to the plane's own normal\n");
    {
        // Six axis planes and a couple of awkward ones. Getting this backwards
        // inverts every polygon in the map while still producing geometry.
        float planes[8][4] = {
            { 1, 0, 0, 100 }, { -1, 0, 0, 100 },
            { 0, 1, 0, 100 }, { 0, -1, 0, 100 },
            { 0, 0, 1, 100 }, { 0, 0, -1, 100 },
            { 0.70710678f, 0.0f, 0.70710678f, 50.0f },
            { 0.36f, -0.48f, 0.80f, -25.0f },
        };
        bool allRight = true, allBig = true;
        for (int i = 0; i < 8; i++)
        {
            float q[WR_BSP_MAX_POLY_VERTS][3];
            int n = WrBspStartQuad(planes[i], q);
            if (n != 4) { allRight = false; continue; }

            float got[3];
            if (!WrBspPolyNormal(q, n, got)) { allRight = false; continue; }
            if (WrBspAngleBetween(got, planes[i]) > 0.5f) allRight = false;

            // And it really does lie on the plane.
            for (int v = 0; v < 4; v++)
                if (fabs(WrBspPlaneDist(planes[i], q[v])) > 1.0f) allRight = false;
            if (WrBspPolyArea(q, n) < WR_BSP_HUGE) allBig = false;
        }
        Check(allRight, "eight planes: quad on the plane, normal matching");
        Check(allBig, "and large enough to survive clipping to any brush");

        float bad[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float q[WR_BSP_MAX_POLY_VERTS][3];
        Check(WrBspStartQuad(bad, q) == 0,
              "a degenerate plane is refused -- 807 of them are real");
    }

    // -----------------------------------------------------------------------
    printf("\na box clips to six squares of the size it should be\n");
    {
        float mins[3] = { -64.0f, -32.0f, 0.0f };
        float maxs[3] = { 64.0f, 32.0f, 16.0f };
        float sides[6][4];
        MakeBox(sides, mins, maxs);

        // 2*64 x 2*32 top and bottom, 128x16 front and back, 64x16 ends.
        float want[6] = { 64.0f * 16.0f, 64.0f * 16.0f,
                          128.0f * 16.0f, 128.0f * 16.0f,
                          128.0f * 64.0f, 128.0f * 64.0f };
        bool areas = true, closed = true, quads = true;
        float total = 0.0f;
        for (int s = 0; s < 6; s++)
        {
            float poly[WR_BSP_MAX_POLY_VERTS][3];
            int n = SideToPoly(sides, 6, s, poly);
            if (n != 4) quads = false;
            float a = WrBspPolyArea(poly, n);
            total += a;
            if (fabs(a - want[s]) > 1.0f) areas = false;
            if (!WrBspPolyClosed(poly, n, sides, 6, s)) closed = false;
        }
        Check(quads, "every side of a box clips to exactly four vertices");
        Check(areas, "and to the area the box's dimensions say it should");
        Check(fabs(total - 2.0f * (128 * 64 + 128 * 16 + 64 * 16)) < 4.0f,
              "the six add up to the box's surface area");
        Check(closed, "THE CLOSURE ASSERTION: no face sticks out of its brush");
    }

    // -----------------------------------------------------------------------
    printf("\na bevel plane falls out on its own -- no flag, no list\n");
    {
        // A wedge, plus a seventh plane tangent to one of its edges. That is
        // what a bevel IS, and half to two thirds of real brushsides are these.
        float sides[8][4];
        float mins[3] = { -64.0f, -64.0f, 0.0f };
        float maxs[3] = { 64.0f, 64.0f, 64.0f };
        MakeBox(sides, mins, maxs);

        // The real cut: a 45 degree plane that slices the +x +z corner off, so
        // it genuinely bounds a face. It has to be INSIDE the corner -- placed
        // exactly through the edge it is tangent instead, and cuts nothing.
        sides[6][0] = 0.70710678f; sides[6][1] = 0.0f; sides[6][2] = 0.70710678f;
        sides[6][3] = 0.70710678f * 128.0f - 20.0f;

        // The bevel: tangent to the vertical +x +y edge, touching the shape
        // along a line and bounding no face at all. This is what a bevel plane
        // IS, and vbsp emits them by the thousand -- nothing in the file marks
        // them, so the only thing that removes them is the clip itself.
        sides[7][0] = 0.70710678f; sides[7][1] = 0.70710678f; sides[7][2] = 0.0f;
        sides[7][3] = 0.70710678f * 128.0f;

        float poly[WR_BSP_MAX_POLY_VERTS][3];
        int nCut = SideToPoly(sides, 8, 6, poly);
        float aCut = WrBspPolyArea(poly, nCut);
        int nBev = SideToPoly(sides, 8, 7, poly);
        float aBev = WrBspPolyArea(poly, nBev);

        Check(nCut >= 3 && aCut > WR_BSP_MIN_AREA,
              "the real cutting plane keeps a face with area");
        Check(aBev < WR_BSP_MIN_AREA,
              "the bevel beside it clips away to nothing");
    }

    // -----------------------------------------------------------------------
    printf("\nthe surf band is signed, because a ceiling is not a ramp\n");
    {
        Check(WrBspIsSurfBand(0.586f), "54 degrees, the measured median, is a ramp");
        Check(!WrBspIsSurfBand(-0.586f), "the same slope facing DOWN is not");
        Check(!WrBspIsSurfBand(0.95f), "a floor is not a ramp");
        Check(!WrBspIsSurfBand(0.02f), "a wall is not a ramp");

        // The two halves of the tool must agree about what a ramp is before
        // they can be compared about an angle.
        Check(WR_BSP_BAND_HI == WR_PHASE_STANDABLE,
              "the band's top is Source's standable cut, shared with wr_phase.h");
        Check(WR_BSP_BAND_LO == WR_PHASE_MIN_RAMP_NZ,
              "and its bottom is the same wall cut wr_phase.h uses");

        float n[3] = { 0.810f, 0.0f, 0.586f };
        float a = WrBspSurfaceAngle(n);
        Check(a > 53.8f && a < 54.4f, "and it reads back 54.1 degrees");

        float down[3] = { 0.810f, 0.0f, -0.586f };
        Check(WrBspSurfaceAngle(down) > 125.0f,
              "an inverted normal reads past 90, rather than folding");
    }

    // -----------------------------------------------------------------------
    printf("\nthe ray hits the front and refuses the back\n");
    {
        // A 45 degree ramp face centred on the origin.
        //
        // WOUND TO MATCH THE PLANE, which is a requirement of the inside test
        // rather than a nicety -- it decides insideness by which side of each
        // edge the point falls on, using the plane's normal as the reference.
        // The clip pipeline guarantees this by construction; a hand-built
        // polygon has to be checked, and the first version of this one was
        // backwards, which rejected every ray that should have hit.
        float p[4] = { -0.70710678f, 0.0f, 0.70710678f, 0.0f };
        float v[4][3] = {
            { -32.0f, -32.0f, -32.0f }, { 32.0f, -32.0f, 32.0f },
            { 32.0f, 32.0f, 32.0f }, { -32.0f, 32.0f, -32.0f },
        };
        float wound[3];
        Check(WrBspPolyNormal(v, 4, wound) &&
              WrBspAngleBetween(wound, p) < 0.5f,
              "the test's own polygon is wound to its plane");
        float t = 0.0f;

        float from[3] = { -100.0f, 0.0f, 100.0f };      // in front
        float dir[3] = { 0.70710678f, 0.0f, -0.70710678f };
        Check(WrBspRayPoly(from, dir, p, v, 4, &t), "a ray from the front hits");
        Check(t > 100.0f && t < 180.0f, "at about the distance it should");

        float behind[3] = { 100.0f, 0.0f, -100.0f };
        float back[3] = { -0.70710678f, 0.0f, 0.70710678f };
        Check(!WrBspRayPoly(behind, back, p, v, 4, &t),
              "a ray from BEHIND is refused -- an underside is not a ramp");

        float par[3] = { 0.70710678f, 0.0f, 0.70710678f };
        Check(!WrBspRayPoly(from, par, p, v, 4, &t), "a parallel ray misses");

        float wide[3] = { -100.0f, 500.0f, 100.0f };
        Check(!WrBspRayPoly(wide, dir, p, v, 4, &t),
              "and one aimed past the edge misses rather than hitting the plane");
    }

    // -----------------------------------------------------------------------
    printf("\npoint-to-polygon distance, on the face and off the end\n");
    {
        float p[4] = { 0.0f, 0.0f, 1.0f, 0.0f };            // z = 0
        float v[4][3] = {
            { -50.0f, -50.0f, 0.0f }, { 50.0f, -50.0f, 0.0f },
            { 50.0f, 50.0f, 0.0f }, { -50.0f, 50.0f, 0.0f },
        };
        float over[3] = { 0.0f, 0.0f, 64.0f };
        Check(fabs(WrBspPointPolyDist(over, p, v, 4) - 64.0f) < 0.01f,
              "straight above the middle it is the plane distance");

        float off[3] = { 150.0f, 0.0f, 0.0f };
        Check(fabs(WrBspPointPolyDist(off, p, v, 4) - 100.0f) < 0.01f,
              "past the edge it is the distance to the edge, not the plane");

        float corner[3] = { 90.0f, 90.0f, 0.0f };
        float want = (float)sqrt(40.0f * 40.0f + 40.0f * 40.0f);
        Check(fabs(WrBspPointPolyDist(corner, p, v, 4) - want) < 0.01f,
              "past a corner it is the distance to the corner");
    }

    // -----------------------------------------------------------------------
    printf("\nthe grid indexes, and the walk agrees with brute force\n");
    {
        float mins[3] = { -4096.0f, -2048.0f, -512.0f };
        float maxs[3] = { 4096.0f, 2048.0f, 512.0f };
        WrBspGridDesc g;
        WrBspGridFit(mins, maxs, &g);

        Check(g.dims[0] > 1 && g.dims[1] > 1 && g.dims[2] >= 1,
              "a long thin map gets more than one cell on its long axis");
        Check(g.dims[0] <= WR_BSP_GRID_MAX_DIM &&
              g.dims[1] <= WR_BSP_GRID_MAX_DIM &&
              g.dims[2] <= WR_BSP_GRID_MAX_DIM, "and is clamped per axis");

        int c[3];
        Check(WrBspGridCell(&g, mins, c) && c[0] == 0 && c[1] == 0 && c[2] == 0,
              "the minimum corner is cell zero");
        float outside[3] = { -99999.0f, 0.0f, 0.0f };
        Check(!WrBspGridCell(&g, outside, c),
              "a point outside is reported as outside, and clamped");

        // Every cell index is distinct and in range.
        bool distinct = true;
        int total = WrBspGridCellCount(&g);
        for (int z = 0; z < g.dims[2] && distinct; z++)
            for (int y = 0; y < g.dims[1] && distinct; y++)
                for (int x = 0; x < g.dims[0]; x++)
                {
                    int i = WrBspGridIndex(&g, x, y, z);
                    if (i < 0 || i >= total) { distinct = false; break; }
                }
        Check(distinct, "every cell index lands inside the array");

        // The DDA must visit the cell every sample along the ray falls in.
        // Brute force is the point: the walk is an optimisation of this.
        float start[3] = { -4000.0f, -2000.0f, -400.0f };
        float dir[3] = { 0.8f, 0.5f, 0.2f };
        float dl = (float)sqrt(WrBspDot(dir, dir));
        for (int k = 0; k < 3; k++) dir[k] /= dl;
        const float far = 6000.0f;

        WrBspDda w;
        bool began = WrBspDdaBegin(&g, start, dir, far, &w);
        Check(began, "the walk starts on a ray that crosses the grid");

        static unsigned char seen[WR_BSP_GRID_MAX_DIM * WR_BSP_GRID_MAX_DIM *
                                  WR_BSP_GRID_MAX_DIM];
        memset(seen, 0, sizeof(seen));
        int steps = 0;
        if (began)
        {
            seen[WrBspGridIndex(&g, w.c[0], w.c[1], w.c[2])] = 1;
            while (WrBspDdaNext(&g, &w) && ++steps < 100000)
                seen[WrBspGridIndex(&g, w.c[0], w.c[1], w.c[2])] = 1;
        }

        int missed = 0, sampled = 0;
        for (float t = 0.0f; t <= far; t += 4.0f)
        {
            float p[3];
            for (int k = 0; k < 3; k++) p[k] = start[k] + dir[k] * t;
            int cc[3];
            if (!WrBspGridCell(&g, p, cc))
                continue;
            sampled++;
            if (!seen[WrBspGridIndex(&g, cc[0], cc[1], cc[2])])
                missed++;
        }
        printf("    walked %d cells, sampled %d points along the ray\n",
               steps + 1, sampled);
        Check(sampled > 100, "the ray really does cross a lot of the grid");
        Check(missed == 0, "the walk visits every cell the ray passes through");

        float away[3] = { 0.0f, 0.0f, 1.0f };
        float high[3] = { 0.0f, 0.0f, 99999.0f };
        Check(!WrBspDdaBegin(&g, high, away, far, &w),
              "a ray that misses the grid entirely does not start");
    }

    printf("\n");
    if (g_failures)
    {
        printf("=== %d FAILED ===\n\n", g_failures);
        return 1;
    }
    printf("=== all brush geometry checks passed ===\n\n");
    return 0;
}
