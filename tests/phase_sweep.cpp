// phase_sweep.cpp  --  the measurements behind wr_phase.h, re-derivable.
//
// Every number in wr_phase.h's header and in WrEnergyPhase's came from this
// program run over a real .wrpath library. It is committed so they can be
// checked rather than believed, and so that changing the classifier is a thing
// with a measurable consequence rather than a matter of taste.
//
// It is NOT part of the harness run. It needs thousands of real runs on disk,
// which are other people's demos and are not in this repository -- the same
// reason tests\parity.ps1 is a separate script. test_phase.exe is the part that
// runs everywhere, on synthetic trajectories where the answer is known exactly.
//
//   tests\phase_sweep.exe [wrlines_data\paths] [--live]
//
// Two questions, one program.
//
// THE CORPUS SWEEP asks what the classifier finds on stored runs, where the
// velocity is a central difference of exact recorded positions and reads gravity
// back to 0.1%. This is the demo-line case and it is close to exact.
//
// THE LIVE SIMULATION asks whether the same test survives being fed a velocity
// differenced from CAMERA positions. It resamples each run at 200 Hz, adds view
// bob, and pushes it through the real wr_smooth.h estimator with the real
// settings out of wr_energy.cpp -- so what is being measured is the shipped
// filter chain and not an idealisation of it. This is the method wr_stress.h
// used to decide that live efficiency colouring could not be shipped.

#include "wr_phase.h"
#include "wr_smooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

// sv_gravity. Not a setting here: the whole point is to measure against the
// value the runs were recorded at, and every surf map on the leaderboard is 800.
static const float G = 800.0f;

// The live filter chain, from wr_energy.cpp's defaults. Kept as named constants
// so a drift between this and the shipped values is visible in a diff.
static const float VEL_WINDOW = 0.040f;     // VEL_WINDOW_SECONDS
static const float VEL_TAU = 0.060f;        // VEL_TAU
static const float FPS = 200.0f;

struct Pt { float x, y, z, vx, vy, vz, t; };

// The .wrpath layout, from wr_path.cpp: a 0x100 header then 28-byte points of
// x y z vx vy vz t. Read directly rather than through the loader so this program
// links nothing but the two pure headers it is measuring.
static Pt *Load(const char *path, int *outN)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char head[0x100];
    if (fread(head, 1, 0x100, f) != 0x100) { fclose(f); return 0; }
    if (memcmp(head, "WRPATH\0\0", 8) != 0) { fclose(f); return 0; }
    unsigned int n = *(unsigned int *)(head + 0x10);
    if (n < 64 || n > 5000000) { fclose(f); return 0; }
    Pt *p = (Pt *)malloc(sizeof(Pt) * n);
    if (!p) { fclose(f); return 0; }
    if (fread(p, sizeof(Pt), n, f) != n) { free(p); fclose(f); return 0; }
    fclose(f);
    *outN = (int)n;
    return p;
}

static int CmpF(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// ---------------------------------------------------------------------------
// The corpus sweep
// ---------------------------------------------------------------------------

static long long g_air, g_contact, g_tele;
static float *g_ang; static int g_angN, g_angCap;
static int g_grades[WR_GRADE_COUNT];
static int g_boards, g_runs;

static void PushAngle(float v)
{
    if (g_angN == g_angCap)
    {
        g_angCap = g_angCap ? g_angCap * 2 : 1024;
        g_ang = (float *)realloc(g_ang, sizeof(float) * g_angCap);
    }
    g_ang[g_angN++] = v;
}

static void SweepRun(Pt *p, int n)
{
    if (n < 300) return;
    g_runs++;

    signed char *st = (signed char *)malloc(n);
    if (!st) return;
    for (int i = 0; i + 1 < n; i++)
    {
        Pt &a = p[i], &b = p[i + 1];
        float h = b.t - a.t;
        float s0 = sqrtf(a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
        float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        if (WrPhaseIsTeleport(sqrtf(dx * dx + dy * dy + dz * dz), s0, h))
        { st[i] = -1; g_tele++; continue; }
        bool c = WrPhaseIsContact(a.vz, b.vz, h, G);
        st[i] = c ? 1 : 0;
        if (c) g_contact++; else g_air++;
    }

    // Sustained contact -> a fitted plane -> the ramp's angle.
    for (int i = 0; i + 1 < n; )
    {
        if (st[i] != 1) { i++; continue; }
        int j = i;
        while (j + 1 < n && st[j] == 1) j++;
        if (j - i >= 8)
        {
            int m = j - i + 1;
            float *vs = (float *)malloc(sizeof(float) * 3 * (size_t)m);
            if (vs)
            {
                bool fast = true;
                for (int k = 0; k < m; k++)
                {
                    Pt &q = p[i + k];
                    vs[3 * k + 0] = q.vx; vs[3 * k + 1] = q.vy; vs[3 * k + 2] = q.vz;
                    if (sqrtf(q.vx * q.vx + q.vy * q.vy + q.vz * q.vz) < 250.0f)
                        fast = false;
                }
                float nrm[3];
                if (fast && WrPhaseFitNormal(vs, m, nrm))
                    PushAngle(WrPhaseSurfaceAngle(nrm));
                free(vs);
            }
        }
        i = j;
    }

    // Boards: the air-to-contact transition, the same rule wr_path.cpp uses.
    for (int i = 3; i + 7 < n - 1; i++)
    {
        if (!(st[i] == 1 && st[i - 1] == 0 && st[i - 2] == 0 && st[i - 3] == 0))
            continue;
        int stick = 0;
        for (int k = i; k < i + 6; k++) if (st[k] == 1) stick++;
        if (stick < 4) continue;

        Pt &a = p[i], &b = p[i + 1];
        float h = b.t - a.t;
        float vIn[3] = { a.vx, a.vy, a.vz };
        float vOut[3] = { b.vx, b.vy, b.vz };
        float nrm[3];
        if (!WrPhaseNormal(vIn, vOut, h, G, nrm)) continue;
        float nz = nrm[2] < 0 ? -nrm[2] : nrm[2];
        if (nz < WR_PHASE_MIN_RAMP_NZ || nz > WR_PHASE_STANDABLE) continue;

        WrBoardStats s;
        if (!WrPhaseBoard(vIn, vOut, nrm, &s)) continue;
        if (!WrPhaseBoardWorthReporting(&s)) continue;
        g_boards++;
        g_grades[s.grade]++;
    }
    free(st);
}

// ---------------------------------------------------------------------------
// The live simulation
// ---------------------------------------------------------------------------

struct Score { long long agree, total, falseAir, falseContact; };

#define NWIN 5
#define NTOL 4
static const float kWin[NWIN] = { 0.05f, 0.10f, 0.15f, 0.20f, 0.30f };
static const float kTol[NTOL] = { 150.0f, 250.0f, 320.0f, 500.0f };
static Score g_score[NWIN][NTOL];
static float g_bob = 2.0f;
static int g_simRuns = 0;

#define VZ_RING 8192

static void SimRun(Pt *p, int n)
{
    // Runs with a teleport in them are skipped whole. This measures the
    // ESTIMATOR; the teleport guard is already pinned in test_phase.
    for (int i = 0; i + 1 < n; i++)
    {
        float h = p[i + 1].t - p[i].t;
        float s0 = sqrtf(p[i].vx * p[i].vx + p[i].vy * p[i].vy + p[i].vz * p[i].vz);
        float dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y, dz = p[i + 1].z - p[i].z;
        if (WrPhaseIsTeleport(sqrtf(dx * dx + dy * dy + dz * dz), s0, h))
            return;
    }

    const float t0 = p[0].t, t1 = p[n - 1].t;
    if (t1 - t0 < 4.0f) return;
    g_simRuns++;

    WrVelWindow w;
    memset(&w, 0, sizeof(w));

    static float vzEst[VZ_RING], tEst[VZ_RING];
    int nEst = 0;
    float ema[3] = { 0.0f, 0.0f, 0.0f };
    bool emaOn = false;
    const float dt = 1.0f / FPS;
    int tick = 0;

    for (float t = t0; t <= t1 && nEst < VZ_RING; t += dt)
    {
        while (tick + 2 < n && p[tick + 1].t <= t) tick++;
        float span = p[tick + 1].t - p[tick].t;
        float u = span > 1e-6f ? (t - p[tick].t) / span : 0.0f;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;

        // The camera: the origin, plus an eye height, plus view bob. The
        // constant offset cancels in a difference. The bob does not, and it is
        // the thing everybody assumes is the problem -- so it is a parameter.
        float cx = p[tick].x + (p[tick + 1].x - p[tick].x) * u;
        float cy = p[tick].y + (p[tick + 1].y - p[tick].y) * u;
        float cz = p[tick].z + (p[tick + 1].z - p[tick].z) * u + 64.0f;
        if (g_bob > 0.0f)
            cz += g_bob * sinf(t * 9.4f);

        WrVelPush(&w, cx, cy, cz, dt);

        float ex, ey, ez, mx, my, mz;
        if (!WrVelEstimate(&w, VEL_WINDOW, &ex, &ey, &ez, &mx, &my, &mz))
            continue;

        float a = 1.0f - expf(-dt / VEL_TAU);
        if (!emaOn) { ema[0] = ex; ema[1] = ey; ema[2] = ez; emaOn = true; }
        else
        {
            ema[0] += (ex - ema[0]) * a;
            ema[1] += (ey - ema[1]) * a;
            ema[2] += (ez - ema[2]) * a;
        }
        vzEst[nEst] = ema[2];
        tEst[nEst] = t;
        nEst++;
    }

    for (int iw = 0; iw < NWIN; iw++)
    {
        int back = (int)(kWin[iw] * FPS + 0.5f);
        if (back < 1) back = 1;

        for (int i = back; i < nEst; i++)
        {
            float h = tEst[i] - tEst[i - back];
            if (!(h > 1e-6f)) continue;

            // The truth at the window's MIDPOINT, which is the instant a finite
            // difference actually describes -- the same argument wr_smooth.h
            // makes about pairing a velocity with a position.
            float mid = 0.5f * (tEst[i] + tEst[i - back]);
            int k = 0;
            while (k + 2 < n && p[k + 1].t <= mid) k++;
            float hh = p[k + 1].t - p[k].t;
            if (!(hh > 1e-6f)) continue;
            bool truth = WrPhaseIsContact(p[k].vz, p[k + 1].vz, hh, G);

            float az = (vzEst[i] - vzEst[i - back]) / h;
            float d = az + G;
            if (d < 0.0f) d = -d;

            for (int it = 0; it < NTOL; it++)
            {
                bool got = d >= kTol[it];
                Score &s = g_score[iw][it];
                s.total++;
                if (got == truth) s.agree++;
                else if (truth) s.falseAir++;
                else s.falseContact++;
            }
        }
    }
}

// ---------------------------------------------------------------------------

static bool g_live = false;
static int g_cap = 0;
static int g_seen = 0;

static void Walk(const char *dir)
{
    char pat[1024];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (g_cap && g_seen >= g_cap) break;
        if (fd.cFileName[0] == '.') continue;
        char full[1024];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { Walk(full); continue; }
        size_t L = strlen(fd.cFileName);
        if (L < 8 || strcmp(fd.cFileName + L - 7, ".wrpath") != 0) continue;
        // surf only: the phase split is a claim about surf maps, and a bhop
        // library would dilute it with a completely different kind of movement.
        if (strstr(dir, "surf_") == 0) continue;
        int n = 0;
        Pt *p = Load(full, &n);
        if (p)
        {
            if (g_live) SimRun(p, n); else SweepRun(p, n);
            free(p);
            g_seen++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main(int argc, char **argv)
{
    const char *root = "wrlines_data\\paths";
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--live") == 0) { g_live = true; g_cap = 250; }
        else if (strcmp(argv[i], "--bob") == 0 && i + 1 < argc) g_bob = (float)atof(argv[++i]);
        else root = argv[i];
    }

    if (!g_live)
    {
        printf("\n=== the corpus, with exact velocities ===\n\n");
        Walk(root);
        if (!g_runs)
        {
            printf("no runs found under %s\n"
                   "this needs a real .wrpath library; see the header.\n\n", root);
            return 1;
        }

        long long tot = g_air + g_contact;
        printf("runs %d   ticks %lld   teleports rejected %lld\n", g_runs, tot, g_tele);
        printf("air %.1f%%   contact %.1f%%\n", 100.0 * g_air / tot,
               100.0 * g_contact / tot);

        if (g_angN)
        {
            qsort(g_ang, g_angN, sizeof(float), CmpF);
            int steep = 0;
            for (int i = 0; i < g_angN; i++)
                if (g_ang[i] > 45.57f) steep++;
            printf("\nsustained-contact segments fitted: %d\n", g_angN);
            printf("  ramp angle  p10 %.1f  p50 %.1f  p90 %.1f deg\n",
                   g_ang[g_angN / 10], g_ang[g_angN / 2], g_ang[g_angN * 9 / 10]);
            printf("  steeper than Source's standable cut: %.1f%%\n",
                   100.0 * steep / g_angN);
        }

        printf("\nboards %d  (%.1f per run)\n", g_boards, (double)g_boards / g_runs);
        if (g_boards)
        {
            printf("  ");
            for (int i = 0; i < WR_GRADE_COUNT; i++)
                printf("%s %.0f%%  ", WrPhaseGradeName((unsigned char)i),
                       100.0 * g_grades[i] / g_boards);
            printf("\n");
        }
        printf("\n");
        return 0;
    }

    printf("\n=== live, through the real estimator ===\n\n");
    printf("view bob %.1f units, %.0f fps, velocity window %.3f s, tau %.3f s\n",
           g_bob, FPS, VEL_WINDOW, VEL_TAU);
    Walk(root);
    if (!g_simRuns)
    {
        printf("no runs found under %s\n"
               "this needs a real .wrpath library; see the header.\n\n", root);
        return 1;
    }
    printf("runs simulated: %d\n\n", g_simRuns);

    printf("%-8s", "window");
    for (int it = 0; it < NTOL; it++) printf("  tol %-19.0f", kTol[it]);
    printf("\n");
    for (int iw = 0; iw < NWIN; iw++)
    {
        printf("%-8.2f", kWin[iw]);
        for (int it = 0; it < NTOL; it++)
        {
            Score &s = g_score[iw][it];
            if (!s.total) { printf("  %-23s", "-"); continue; }
            printf("  %5.1f%% (miss%4.1f fake%4.1f)",
                   100.0 * s.agree / s.total,
                   100.0 * s.falseAir / s.total,
                   100.0 * s.falseContact / s.total);
        }
        printf("\n");
    }
    printf("\n  miss = it was contact and this said air   (a ramp gone unseen)\n");
    printf("  fake = it was air and this said contact   (a surface invented)\n");
    printf("\nthe shipped pair is window %.2f, tol %.0f -- see WrEnergyPhase.\n\n",
           0.10f, 250.0f);
    return 0;
}
