// wr_phase.h  --  is the player in the air, on a ramp, or on the ground?
//
// WHY THIS CAN BE ANSWERED AT ALL
//
// WrLines has no entity access, no netvars, no trace and no collision data. It
// reads a camera out of a matrix and it reads files. So "what surface is the
// player touching" looks like a question it cannot ask, and for a long time it
// did not: wr_energy.cpp's ground test is |vz| < 30 held for 50 ms, which fires
// at the apex of EVERY jump, and wr_energy.h records that as the reason it was
// taken out of the anchor logic. There has never been an airborne flag, a ramp,
// a surface normal or a slope anywhere in this project.
//
// The answer does not need geometry. In free flight the only force is gravity,
// so the vertical acceleration is EXACTLY -sv_gravity. Anything else means
// something is pushing back, and the only thing that pushes back is a surface.
//
// Measured across this machine's library, and then re-measured with this code
// over the whole of it -- 2,239 runs, 8,926,314 ticks:
//
//     median dvz/dt in free flight              -800.0 u/s^2   (theory -800)
//     p05                                       -801
//     the split                                 68.7% air, 31.3% contact
//     teleports rejected                        4,116
//
//     sustained contact segments fitted         38,008
//     ramp angle       p10 47.3   p50 54.1   p90 83.2 degrees
//     steeper than Source's standable cut       94.8%
//
//     boards found                              27,453  (12.3 per run)
//     graded  perfect 31%  good 30%  okay 18%  bad 20%  terrible 2%
//
// The stored velocities read gravity back to 0.1%. So the test below has a noise
// floor of about 1 u/s^2 against a signal of hundreds to thousands. That is not
// a marginal statistical call, it is a clean split, and it is why this file is
// worth having at all.
//
// That 94.8% is the line worth reading twice: nearly everything a surfer is in
// sustained contact with, at speed, is a surface they could not stand on. Which
// is what a surf map is.
//
// THE MISTAKE THAT IS EASIEST TO MAKE HERE, ALREADY MADE
//
// The obvious test is "is |a - g| large". It is WRONG, and it fails in the one
// place that matters. Source's air acceleration is purely horizontal, so a
// player air-strafing at full tilt has a - g pointing sideways with a magnitude
// up to 2000 u/s^2 while a_z is still exactly -g. Thresholding the magnitude put
// 36% of all samples into a spurious "normal is horizontal" spike -- that spike
// was air-strafing, not surfaces.
//
// THE TEST MUST BE ON THE VERTICAL COMPONENT ALONE. Air strafing cannot touch
// it; a surface always does, because a surface a player can stand on or surf has
// a normal with a vertical component and the constraint force runs along it.
//
// WHERE THE SURFACE NORMAL COMES FROM
//
// Source clips velocity against a plane with ClipVelocity, and airborne clips
// use overbounce 1.0 -- which makes the clip a PURE ORTHOGONAL PROJECTION:
//
//     v_out = v_in - n * dot(v_in, n)
//
// so v_out - v_in runs along n, and once gravity's contribution over the tick is
// removed, the normal is just the direction of what is left. No trace, no BSP,
// no brush data. The sign matters and is easy to get backwards: the normal force
// pushes AWAY from the surface, so it is +normalize(a - g). Negating it put
// 97.9% of recovered normals underground.
//
// Orthogonal projection also collapses every board statistic onto two scalars:
//
//     intoPlane = sqrt(inSpeed^2 - outSpeed^2)
//     approach  = asin(outSpeed / inSpeed)      90 deg = parallel = ideal
//     lossPct   = 1 - sin(approach)
//
// Checked against the reconstruction on real demos, lossPct == 1 - sin(approach)
// holds to a median error of 0.0056. The plane normal is redundant information
// for everything except the ramp's own angle.
//
// WHAT THIS IS NOT
//
// It is not a trace and it cannot see geometry the player has not touched. It
// reports the surface that was actually hit -- which includes displacements,
// moving brushes and ramp-bug recoveries, none of which a static BSP read would
// get right without reimplementing Source's movement code -- but it cannot tell
// you about the ramp ahead. That is what a .bsp reader would be for, and it is a
// separate subsystem.
//
// It is also measured on DEMO data, where velocity is a central difference of
// exact recorded positions and good to about 0.1%. Live velocity is differenced
// from camera motion over a 40 ms window with about 320 ms of end-to-end lag,
// and wr_stress.h records what that did to live efficiency colouring: wrong way
// up 26% of the time, and "not jitter that a filter could take out". Contact is
// a far larger signal than air-strafe efficiency so it will probably survive the
// trip, but probably is not the standard here. Anything live gets re-measured
// against the same harness before it is believed.
//
// static inline in a header, with the rest of this project's pure logic --
// wr_scale.h, wr_stress.h, wr_smooth.h, wr_budget.h, wr_pacing.h. Like those it
// includes nothing but math.h and takes plain floats rather than Vec3, which is
// what lets tests\test_phase.exe link nothing at all.

#ifndef WR_PHASE_H
#define WR_PHASE_H

#include <math.h>

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

enum
{
    WR_PHASE_UNKNOWN = 0,   // a teleport, a zero-length tick, the ends of a run
    WR_PHASE_AIR,           // free flight: vertical acceleration is gravity
    WR_PHASE_RAMP,          // touching a surface too steep to stand on
    WR_PHASE_GROUND         // touching a surface you could stand on
};

// How far dvz/dt may sit from -gravity and still count as free flight.
//
// 60 u/s^2 against a measured spread of about 1. It is deliberately loose: the
// cost of a slightly wide band is a few contact ticks called air at the very
// start and end of a ramp, and the cost of a tight one is every tick of noise
// becoming a phantom surface. The measured p05 of dvz/dt in free flight is -801
// against a theoretical -800, so 60 is sixty times the noise and still a
// thirteenth of the smallest real contact signal.
#define WR_PHASE_GRAVITY_TOL 60.0f

// Source's own standable cut. CGameMovement refuses to stand on a plane whose
// normal.z is below 0.7, which is what makes a surf ramp a surf ramp, so the
// same number is what separates WR_PHASE_RAMP from WR_PHASE_GROUND here rather
// than a taste of ours.
#define WR_PHASE_STANDABLE 0.7f

// A position step larger than this is not motion, it is a teleport -- a fail
// trigger, a save-loc load, a stage change. Differencing across one produces an
// acceleration of tens of thousands and would be graded as a spectacular board.
// 65 of them turned up across 40 runs, so this is a real filter and not a
// theoretical one.
#define WR_PHASE_TELEPORT_K   1.6f    // times the distance the speed implies
#define WR_PHASE_TELEPORT_PAD 32.0f   // plus this, for slow ticks

// True when the step from one sample to the next is too large to be motion.
// speed is |v| at the FIRST sample and h is the interval between them.
static inline bool WrPhaseIsTeleport(float step, float speed, float h)
{
    if (!(h > 0.0f))
        return true;
    return step > (speed * h) * WR_PHASE_TELEPORT_K + WR_PHASE_TELEPORT_PAD;
}

// Is a surface being touched? vz0 and vz1 are the vertical velocities of two
// consecutive samples h apart, and gravity is positive (sv_gravity, 800).
//
// This is the whole of the air/contact split, and it is one comparison.
static inline bool WrPhaseIsContact(float vz0, float vz1, float h, float gravity)
{
    if (!(h > 1e-6f))
        return false;
    float az = (vz1 - vz0) / h;
    float d = az + gravity;
    if (d < 0.0f) d = -d;
    return d >= WR_PHASE_GRAVITY_TOL;
}

// ---------------------------------------------------------------------------
// The surface
// ---------------------------------------------------------------------------

// The normal of the surface that produced this velocity change.
//
// out is the unit normal, pointing away from the surface. Returns false when
// there is nothing to normalise, which for a caller means "no usable reading"
// rather than "flat floor" -- those must not be drawn the same way.
//
// HOW GOOD IT IS, MEASURED AGAINST SOMETHING THAT IS NOT THIS
//
// Everything else here is self-consistent -- the projection identity, the
// grade spread, the board count -- which is worth something and is not the
// same as being right. tests\bsp_sweep.exe --verify-normals is the outside
// check: the same planes read straight out of the .bsp, sharing no code, no
// input and no assumption with this. Over 39 maps and 2,294 runs:
//
//     AT A BOARD, meaning this function called on the tick that arrives out
//     of free flight, so exactly one surface is acting:
//         normal against normal   p50 1.19 deg, 91.4% within 15, 1.0% gross
//         slope alone             p50 0.64 deg
//         SIGNED                  p50 +0.00 deg -- no bias in either direction
//
//     MID-RIDE, on any hard tick wherever it lands:
//         normal against normal   p50 4.91 deg, 12.0% gross
//
// The mid-ride figure is not this function being worse there. It is corners
// and seams: two surfaces pushing at once makes (a - g) their SUM, and no
// amount of arithmetic recovers two planes from one vector. That is a question
// with no single answer rather than a wrong answer, and it is the reason the
// board detector reads the transition rather than sampling the ride.
//
// So: trust this at a transition, and treat a mid-ride reading as indicative.
static inline bool WrPhaseNormal(const float vIn[3], const float vOut[3],
                                 float h, float gravity, float out[3])
{
    if (!(h > 1e-6f))
        return false;

    // a - g, in units of velocity change over the tick. Gravity only touches z.
    float cx = vOut[0] - vIn[0];
    float cy = vOut[1] - vIn[1];
    float cz = (vOut[2] - vIn[2]) + gravity * h;

    float m = (float)sqrt(cx * cx + cy * cy + cz * cz);
    if (!(m > 1e-6f))
        return false;

    out[0] = cx / m;
    out[1] = cy / m;
    out[2] = cz / m;
    return true;
}

// How many samples the fit will look at. A contact segment is tens of ticks;
// anything longer is strided down rather than truncated, so a long ride is still
// fitted across its whole length instead of just its first second.
#define WR_PHASE_FIT_MAX 128

// Below this the samples are all pointing the same way and no plane is
// determined by them. sin(0.057 degrees) -- deliberately tiny, because the
// caller should get a refusal rather than a normal made of rounding error.
#define WR_PHASE_FIT_MIN_SPREAD 1e-3f

// The normal of a plane fitted to a run of velocities.
//
// Better conditioned than a single clip whenever there is a sustained contact to
// fit, because while a player rides a ramp every velocity lies IN the plane, so
// the plane's normal is perpendicular to all of them at once.
//
// THAT ARGUMENT IS SOUND AND THE ADVANTAGE DID NOT SHOW UP. Measured against
// the .bsp's own planes over 22,351 sustained rides -- see bsp_sweep.exe
// --verify-normals -- this fit reads a median of 6.76 degrees off, against
// 4.91 for a single hard clip and 1.19 for a single clip at a board. It is the
// worst of the three, not the best.
//
// The reason is in the paragraph below about eigenvectors, and it applies here
// too: a player riding a ramp turns slowly, so consecutive velocities are
// nearly parallel and a cross product between them amplifies any out-of-plane
// error by one over the sine of a small angle. Having more samples does not
// help when they all point the same way. Whatever the estimator, a sustained
// ride is simply a worse place to ask this question than a transition is --
// and a ride can also be against two surfaces at once, where there is no
// single plane to find.
//
// It is kept because it is the only thing that can answer "what is this
// player riding right now" away from a transition, and because measuring it
// honestly is more useful than removing it. A caller who has a transition
// available should use WrPhaseNormal at the transition instead.
//
// WHY THIS IS NOT AN EIGENVECTOR SOLVE
//
// The textbook answer is the eigenvector of the smallest eigenvalue of
// sum(v v^T), reached by power-iterating on tr(S)*I - S. That was written first
// and it does not work here. Its convergence rate is set by the ratio of the two
// LARGEST eigenvalues of S, which is the ratio of the spread of the velocities
// along the ramp to their spread across it -- and a player riding a ramp turns
// slowly, so the velocities stay nearly collinear and that ratio sits just above
// 1. Sixty-four iterations were not close to enough, and no fixed count is:
// the harder the fit, the slower it converges, which is exactly backwards.
//
// So it is done with cross products instead, which have no convergence
// behaviour at all. Any two non-parallel in-plane velocities give the normal
// exactly; the pair that is most orthogonal gives it most stably; and averaging
// every pair, sign-aligned against that seed, uses all the data without letting
// a nearly-parallel pair dominate.
//
// vels is 3*n floats. Sign is resolved upwards, since the fit cannot tell a ramp
// from the same ramp seen from beneath and a surf ramp's normal points up.
static inline bool WrPhaseFitNormal(const float *vels, int n, float out[3])
{
    if (n < 2 || vels == 0)
        return false;

    // Unit directions. The magnitudes say nothing about the plane, and leaving
    // them in weights the fit towards wherever the player happened to be
    // fastest, which on a ramp is its bottom.
    float u[WR_PHASE_FIT_MAX][3];
    int m = 0;
    int stride = (n + WR_PHASE_FIT_MAX - 1) / WR_PHASE_FIT_MAX;
    if (stride < 1)
        stride = 1;

    for (int i = 0; i < n && m < WR_PHASE_FIT_MAX; i += stride)
    {
        const float *v = vels + 3 * i;
        float len = (float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(len > 1e-6f))
            continue;
        u[m][0] = v[0] / len;
        u[m][1] = v[1] / len;
        u[m][2] = v[2] / len;
        m++;
    }
    if (m < 2)
        return false;

    // The most orthogonal pair, as a seed to resolve every other pair's sign
    // against. |a x b| is sin of the angle between them, so this is simply the
    // widest-apart pair of directions in the set.
    float seed[3] = { 0.0f, 0.0f, 0.0f };
    float best = -1.0f;
    for (int i = 0; i < m; i++)
        for (int j = i + 1; j < m; j++)
        {
            float c[3];
            c[0] = u[i][1] * u[j][2] - u[i][2] * u[j][1];
            c[1] = u[i][2] * u[j][0] - u[i][0] * u[j][2];
            c[2] = u[i][0] * u[j][1] - u[i][1] * u[j][0];
            float l = (float)sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (l > best)
            {
                best = l;
                seed[0] = c[0] / l;
                seed[1] = c[1] / l;
                seed[2] = c[2] / l;
            }
        }

    if (best < WR_PHASE_FIT_MIN_SPREAD)
        return false;

    // Every pair, aligned and summed. A nearly-parallel pair contributes a short
    // vector and so weights itself out, which is the behaviour wanted.
    float acc[3] = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < m; i++)
        for (int j = i + 1; j < m; j++)
        {
            float c[3];
            c[0] = u[i][1] * u[j][2] - u[i][2] * u[j][1];
            c[1] = u[i][2] * u[j][0] - u[i][0] * u[j][2];
            c[2] = u[i][0] * u[j][1] - u[i][1] * u[j][0];

            float s = c[0] * seed[0] + c[1] * seed[1] + c[2] * seed[2];
            if (s < 0.0f)
            {
                c[0] = -c[0];
                c[1] = -c[1];
                c[2] = -c[2];
            }
            acc[0] += c[0];
            acc[1] += c[1];
            acc[2] += c[2];
        }

    float len = (float)sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    if (!(len > 1e-12f))
        return false;

    acc[0] /= len;
    acc[1] /= len;
    acc[2] /= len;

    if (acc[2] < 0.0f)
    {
        acc[0] = -acc[0];
        acc[1] = -acc[1];
        acc[2] = -acc[2];
    }

    out[0] = acc[0];
    out[1] = acc[1];
    out[2] = acc[2];
    return true;
}

// The tilt of a surface from horizontal, in degrees. 0 is a floor, 90 a wall.
// Takes |n.z| because a surf ramp read from below has a downward normal and is
// the same ramp.
static inline float WrPhaseSurfaceAngle(const float n[3])
{
    float nz = n[2] < 0.0f ? -n[2] : n[2];
    if (nz > 1.0f) nz = 1.0f;
    return (float)(acos(nz) * 57.2957795131);
}

// Which phase a contact is, given its normal.
static inline int WrPhaseFromNormal(const float n[3])
{
    float nz = n[2] < 0.0f ? -n[2] : n[2];
    return (nz < WR_PHASE_STANDABLE) ? WR_PHASE_RAMP : WR_PHASE_GROUND;
}

// ---------------------------------------------------------------------------
// Boards
// ---------------------------------------------------------------------------

// A board is the moment a player in the air first touches a ramp, and how much
// it cost them. Everything here is a consequence of the orthogonal projection
// above, so it is derived from the two speeds and the normal and nothing else.
struct WrBoardStats
{
    float speedIn, speedOut;    // 3D speed, matching what surf HUDs quote
    float loss;                 // units per second, clamped at zero
    float lossPct;              // as a fraction of speedIn
    float approachDeg;          // angle off the normal, folded to [0, 90]
    float rampDeg;              // the ramp's own tilt from horizontal
    float intoPlane;            // how hard the ramp was hit, u/s
    unsigned char grade;
};

enum
{
    WR_GRADE_PERFECT = 0,
    WR_GRADE_GOOD,
    WR_GRADE_OKAY,
    WR_GRADE_BAD,
    WR_GRADE_TERRIBLE,
    WR_GRADE_COUNT
};

// Loss and angle cut-offs per grade, best first. Both must hold.
//
// These are the surf community's numbers rather than ours, and they are worth
// keeping compatible so a player's idea of a "good board" means the same thing
// here as everywhere else. Note the two criteria are very nearly the same
// constraint: lossPct == 1 - sin(approach) exactly, so 85 deg implies 0.38% and
// 80 deg implies 1.52%. The pair is kept anyway because they come apart once a
// measurement has noise in it, which ours does.
static const float WR_GRADE_LOSS[WR_GRADE_COUNT]  = { 0.005f, 0.015f, 0.03f, 0.05f, 1.0f };
static const float WR_GRADE_ANGLE[WR_GRADE_COUNT] = { 85.0f,  80.0f,  75.0f, 60.0f, 0.0f };

// Above this speed the loss threshold is relaxed, because a given geometry costs
// proportionally more the faster you arrive. A grading convention, not physics:
// the underlying lossPct is speed-independent.
#define WR_GRADE_SPEED_REF 1500.0f

static inline unsigned char WrPhaseGrade(float lossPct, float approachDeg,
                                         float speedIn)
{
    float f = (float)sqrt(speedIn / WR_GRADE_SPEED_REF);
    if (f < 1.0f) f = 1.0f;

    for (int i = 0; i < WR_GRADE_COUNT; i++)
        if (lossPct <= WR_GRADE_LOSS[i] * f && approachDeg >= WR_GRADE_ANGLE[i])
            return (unsigned char)i;

    return (unsigned char)WR_GRADE_TERRIBLE;
}

static inline const char *WrPhaseGradeName(unsigned char g)
{
    switch (g)
    {
        case WR_GRADE_PERFECT:  return "perfect";
        case WR_GRADE_GOOD:     return "good";
        case WR_GRADE_OKAY:     return "okay";
        case WR_GRADE_BAD:      return "bad";
        default:                return "terrible";
    }
}

// Fill in a board from the velocities either side of the contact and the normal
// recovered from it. False when there is nothing worth reporting.
static inline bool WrPhaseBoard(const float vIn[3], const float vOut[3],
                                const float n[3], WrBoardStats *out)
{
    if (out == 0)
        return false;

    float s0 = (float)sqrt(vIn[0] * vIn[0] + vIn[1] * vIn[1] + vIn[2] * vIn[2]);
    float s1 = (float)sqrt(vOut[0] * vOut[0] + vOut[1] * vOut[1] + vOut[2] * vOut[2]);
    if (!(s0 > 1e-3f))
        return false;

    float dot = (vIn[0] * n[0] + vIn[1] * n[1] + vIn[2] * n[2]) / s0;
    if (dot < 0.0f) dot = -dot;
    if (dot > 1.0f) dot = 1.0f;

    out->speedIn = s0;
    out->speedOut = s1;
    out->loss = (s0 > s1) ? (s0 - s1) : 0.0f;
    out->lossPct = out->loss / s0;
    out->approachDeg = (float)(acos(dot) * 57.2957795131);
    out->rampDeg = WrPhaseSurfaceAngle(n);
    out->intoPlane = dot * s0;
    out->grade = WrPhaseGrade(out->lossPct, out->approachDeg, s0);
    return true;
}

// The two filters that stop ordinary surfing being reported as a board.
//
// A player riding a ramp is clipped every single tick, but only of the small
// into-plane component gravity accrued since the last one -- a handful of units
// a second. A board is a one-off large one. And below walking pace the whole
// question is uninteresting.
//
// These are NOT sufficient on their own, and reproducing the reference plugin's
// rule of "into-plane over 25 and a latch" against demo data graded 84% of
// events perfect, because it caught sustained surfing rather than boards. The
// caller must find the air-to-contact transition first; these only reject the
// small stuff once it has.
#define WR_BOARD_MIN_INTO_PLANE 25.0f
#define WR_BOARD_MIN_SPEED      100.0f

// The shallow end of "this is a ramp rather than a wall".
//
// WR_PHASE_STANDABLE is the other end and comes from Source itself. This one is
// a judgement: a normal with almost no vertical component is a wall, and hitting
// a wall is not a board however cleanly it is done. 0.1 is 84.3 degrees, so it
// excludes only the genuinely vertical -- the measured p90 of real surf ramps is
// 83.2, which sits just inside it.
#define WR_PHASE_MIN_RAMP_NZ 0.1f

static inline bool WrPhaseBoardWorthReporting(const WrBoardStats *b)
{
    return b->speedIn >= WR_BOARD_MIN_SPEED &&
           b->intoPlane >= WR_BOARD_MIN_INTO_PLANE;
}

#endif // WR_PHASE_H
