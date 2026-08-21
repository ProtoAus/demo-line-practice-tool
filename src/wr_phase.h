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
//
// THE COMPARISON IS STRICT ON THE FLOOR SIDE. TryPlayerMove writes
// `if (pm.plane.normal[2] > 0.7) blocked |= 1;` -- so a plane at EXACTLY 0.7 is
// not a floor to the engine, and must not be ground here either. See
// WrPhaseFromNz, which is the only place this is compared. A measure-zero
// difference that costs one character to get exactly right, and the comment
// above claims this number comes from Source.
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
// EXCEPT FOR ONE QUESTION, WHERE THIS IS STILL THE BEST ANSWER THERE IS, and it
// had to be measured to be believed because the paragraphs above read as though
// nothing should ever call this. The question is RAMP OR GROUND for a whole
// ride -- one verdict, held across every point of a segment, which is what
// wr_path.cpp's FindPhase colours a line with. Both estimators scored on the
// same segments against the same .bsp faces:
//
//     24,789 segments                 fit 94.7% right, entry tick 81.5%
//     the 1,801 whose entry impulse   fit 93.7% right, entry tick 93.9%
//     clears 300 u/s
//
// Being worse by angle and better by verdict is not a contradiction. The verdict
// is one bit either side of nz = 0.7, and this fit's error is largely in
// HEADING, which does not move nz at all -- measured separately at the same
// time: 1.97 degrees of heading against 2.55 of slope on a single hard tick. A
// single tick puts its error into the whole vector, nz included. So: angles from
// a transition, verdicts from the ride, and the two do not compete.
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

// Which phase a contact is, given nothing but the vertical component of its
// normal -- because that is genuinely all this ever depended on. Source's own
// standable test is a comparison against n.z and nothing else, and spelling that
// out lets a caller who has only read a plane's z out of a file ask the question
// without assembling a vector to be ignored.
// `<=` and not `<`: the engine's floor test is strict (`normal[2] > 0.7`), so a
// plane at exactly 0.7 is a ramp to Source and now to us. See WR_PHASE_STANDABLE.
static inline int WrPhaseFromNz(float nz)
{
    if (nz < 0.0f) nz = -nz;
    return (nz <= WR_PHASE_STANDABLE) ? WR_PHASE_RAMP : WR_PHASE_GROUND;
}

// Which phase a contact is, given its normal.
static inline int WrPhaseFromNormal(const float n[3])
{
    return WrPhaseFromNz(n[2]);
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
    float normal[3];            // the plane, so the cost can be re-expressed
    float velIn[3];             // and the arriving velocity, for the same reason
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
//
// Sized by the initialiser and checked against the enum. A short table here
// would not crash the way a short label table does -- it would quietly grade
// every board in the new bottom class as needing 0% loss at 0 degrees, which is
// worse, because nothing would ever say so.
static const float WR_GRADE_LOSS[]  = { 0.005f, 0.015f, 0.03f, 0.05f, 1.0f };
static const float WR_GRADE_ANGLE[] = { 85.0f,  80.0f,  75.0f, 60.0f, 0.0f };
// Spelled out rather than using wr_common.h's WR_TABLE_IS_FULL, because this
// header includes math.h and nothing else on purpose -- the test harnesses
// compile it on its own, the same arrangement as wr_scale.h and wr_stress.h.
static_assert(sizeof(WR_GRADE_LOSS) / sizeof(WR_GRADE_LOSS[0]) == WR_GRADE_COUNT,
              "WR_GRADE_LOSS is missing entries for WR_GRADE_COUNT");
static_assert(sizeof(WR_GRADE_ANGLE) / sizeof(WR_GRADE_ANGLE[0]) == WR_GRADE_COUNT,
              "WR_GRADE_ANGLE is missing entries for WR_GRADE_COUNT");

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
//
// THE LOSS IS PROJECTED, NOT SUBTRACTED, AND THAT IS THE WHOLE POINT
//
// The obvious way to measure what a board cost is |vIn| - |vOut|. This did that,
// and it was wrong: a player reported every board on screen reading "-0" while
// its grade and its approach angle both varied, which cannot be true of the same
// event -- an entry 57 degrees off the normal MUST shed 1 - sin(57) = 16% of its
// speed, and 16% of anything is not zero.
//
// Measured over 27,508 boards in 2,247 runs, four ways of asking:
//
//     loss measured   |v[i]|   - |v[i+1]|      p50   4.1 u/s    (what shipped)
//     loss wide       |v[i-1]| - |v[i+2]|      p50  13.3 u/s
//     loss clean      |v[i-1]| - |v[i+1]|      p50   0.0 u/s
//     loss projected  s0 * (1 - sin(approach)) p50  22.7 u/s
//
// 37.2% of boards printed as "-0". Note "clean" -- the pair the smear argument
// says should be exact -- is the WORST of them, which is what killed the smear
// explanation and found the real one:
//
//     speed change per tick with NO clip in it, at the same 27,508 events
//         in the air just before    p50  +7.1 u/s
//         on the ramp just after    p50 -11.6 u/s
//
// A board costs 4-23 u/s. The background it sits on moves 7-12 u/s PER TICK, in
// opposite directions either side of the event. Differencing two speeds measures
// the sum of the clip and the ride, and here the ride is the larger term -- so
// the sign of the answer is set by which ticks you happened to pick. That is not
// a precision problem that a better pair of indices fixes. It is the wrong
// instrument.
//
// THE ONE ASSUMPTION, WRITTEN DOWN BECAUSE EVERYTHING BELOW RESTS ON IT
//
// ClipVelocity is an orthogonal projection only at overbounce exactly 1, and a
// surf ramp does NOT take the branch that hard-codes 1. TryPlayerMove splits on
// the same 0.7:
//
//     if (planes[i][2] > 0.7)  ClipVelocity(..., 1)                    floor
//     else                     ClipVelocity(..., 1 + sv_bounce
//                                                * (1 - surfaceFriction))  wall
//
// A ramp is n.z < 0.7 by definition, so every board goes through the SECOND
// line. It still comes to 1.0, twice over: sv_bounce is 0 by default, and
// m_surfaceFriction is 1 outside the deadstrafe window -- either alone is enough
// to kill the term. But it is a default and not a law, and a server running
// sv_bounce non-zero would make every loss here read low without anything
// looking wrong.
//
// Left as arithmetic rather than parameterised: there is no evidence sv_bounce is
// ever anything but 0, nothing in this tool reads a cvar to find out, and adding
// an overbounce term to match a hypothesis would be inventing precision. The
// honest thing is to say what is assumed and where it would break.
//
// The same document confirms the projection from the other side. ClipVelocity's
// third step is
//
//     adjust = DotProduct(out, normal); if (adjust < 0) out -= normal * adjust;
//
// which is a no-op at overbounce 1, because `out` is already free of any
// component along the normal. A correction that never fires is a statement that
// the first two steps were an exact projection.
//
// The projection is the right one, and it is exact rather than approximate:
// Source's PM_ClipVelocity is backoff = dot(v, n) * overbounce with overbounce 1
// for surfing, which is an orthogonal projection and nothing else. So
// lossPct == 1 - sin(approach) is an identity of the engine's own arithmetic --
// the note above WR_GRADE_LOSS has said so all along -- and the only question is
// whether the normal is good enough to lean on. It is, twice over:
// bsp_sweep --verify-normals puts it p50 1.19 deg from the plane read out of the
// .bsp, and moving which ticks it is recovered from shifts the approach angle by
// p50 0.26 deg. The angle is the solid input here; the speed difference never
// was.
//
// vOut is still required and still read, for intoPlane's sign check and because
// a caller with no outgoing sample has no board. It no longer sets the loss.
static inline bool WrPhaseBoard(const float vIn[3], const float vOut[3],
                                const float n[3], WrBoardStats *out)
{
    if (out == 0)
        return false;

    float s0 = (float)sqrt(vIn[0] * vIn[0] + vIn[1] * vIn[1] + vIn[2] * vIn[2]);
    float s1 = (float)sqrt(vOut[0] * vOut[0] + vOut[1] * vOut[1] + vOut[2] * vOut[2]);
    if (!(s0 > 1e-3f))
        return false;
    if (!(s1 >= 0.0f))
        return false;

    // Orient the plane towards whoever arrived on it.
    //
    // A BSP normal points whichever way the brush side was written and the file
    // does not say which side a player is on, so this used to fold the sign away
    // with an abs and keep the unsigned angle -- which is all a grade needs. It
    // is not all a PER-AXIS readout needs: dv = -(v.n)n, and with the sign of n
    // unknown, the sign of every component of that is unknown too.
    //
    // The velocity settles it without any help from the map. You cannot arrive on
    // a surface while travelling away from it, so the physical case is v.n < 0,
    // and the normal that satisfies it is the one pointing back at the player.
    // Flipping to that leaves the angle exactly as it was and makes
    // dv = intoPlane * normal come out right in all three components.
    // IN DOUBLE, AND THAT IS NOT FUSSINESS.
    //
    // Everything a surfer cares about lives between 85 and 90 degrees, where
    // dot is under 0.09 and the whole grade is dot^2. A float32 dot product of
    // three terms with operands around 2500 carries an absolute error of order
    // 1e-4, which at 89 degrees is 0.6% of dot and so 1.2% of lossPct -- larger
    // than the difference between a good board and a great one, arriving from
    // nothing but the width of the arithmetic. The inputs are float32 and stay
    // that way; it is the CANCELLATION that needs the room.
    const double d0 = (double)s0;
    const double rawDot = ((double)vIn[0] * n[0] + (double)vIn[1] * n[1] +
                           (double)vIn[2] * n[2]) / d0;
    const float orient = (rawDot > 0.0) ? -1.0f : 1.0f;
    const float no[3] = { n[0] * orient, n[1] * orient, n[2] * orient };

    double dot = -(rawDot * orient);        // |rawDot|, by construction
    if (dot < 0.0) dot = 0.0;
    if (dot > 1.0) dot = 1.0;

    // sin(approach), straight from the dot rather than through acos and back --
    // the round trip costs accuracy exactly where the grade is decided, up at
    // 85 degrees where sin is flattest.
    const double dot2 = dot * dot;
    const double sinA = sqrt(1.0 - dot2);

    // AND THE LOSS WITHOUT SUBTRACTING TWO NUMBERS THAT ARE NEARLY EQUAL.
    //
    //     1 - sqrt(1 - x)  ==  x / (1 + sqrt(1 - x))
    //
    // an identity, not an approximation. The left side is what this used to
    // compute: at a 99.5%-perfect board sinA is 0.995 and `1 - sinA` throws
    // away most of the significant bits of the one quantity the whole readout
    // is about. The right side has no subtraction in it at all and is exact for
    // every board, including the glancing ones a good surfer spends their time
    // on. The same rearrangement carries `loss`, since loss is s0 * lossPct.
    const double lossPct = dot2 / (1.0 + sinA);

    out->speedIn = s0;
    out->speedOut = (float)(d0 * sinA);   // what the clip leaves, not what came next
    out->loss = (float)(d0 * lossPct);
    out->lossPct = (float)lossPct;
    out->approachDeg = (float)(acos(dot) * 57.2957795131);
    out->rampDeg = WrPhaseSurfaceAngle(n);
    out->intoPlane = (float)(dot * d0);
    out->normal[0] = no[0];
    out->normal[1] = no[1];
    out->normal[2] = no[2];
    out->velIn[0] = vIn[0];
    out->velIn[1] = vIn[1];
    out->velIn[2] = vIn[2];
    out->grade = WrPhaseGrade(out->lossPct, out->approachDeg, s0);
    return true;
}

// THE SAME BOARD, IN WHICHEVER UNITS YOU WANTED IT
//
// PM_ClipVelocity with overbounce 1 is an orthogonal projection, so the change
// it makes to your velocity is exactly
//
//     dv = -(v.n) n                          and (v.n) is `intoPlane`
//
// which means the cost in any unit at all is a line of arithmetic on one number
// and the plane, rather than a separate measurement with its own error. A clip
// changes no height, so the energy form is the kinetic term alone.
//
// Signs: every one of these is returned POSITIVE for a loss, because all four
// readouts print it behind a minus sign of their own.
static inline float WrBoardLoss3D(const WrBoardStats *b)
{
    return b->loss;
}

static inline float WrBoardLossHorizontal(const WrBoardStats *b)
{
    // |v_h| before, minus |v_h| after. With the normal oriented back at the
    // player, v.n is -intoPlane, so v' = v + intoPlane * n -- and only the
    // horizontal part of that lands here, which is why this is not just the 3D
    // figure scaled. On a shallow ramp most of the bite is vertical and the
    // horizontal loss is much the smaller number.
    //
    // Written as a difference of SQUARES over a sum, for the reason spelled out
    // at lossPct above: `before` and `after` differ by well under a unit out of
    // thousands on a shallow ramp, so subtracting them directly leaves almost
    // no significant bits in the answer. The squares differ by a quantity with
    // no cancellation in it, and dividing by the sum recovers the difference
    // exactly:  a - b == (a^2 - b^2) / (a + b).
    const double hx = b->velIn[0], hy = b->velIn[1];
    const double k = b->intoPlane;
    const double nx = b->normal[0], ny = b->normal[1];
    const double ox = hx + k * nx;
    const double oy = hy + k * ny;
    const double before = sqrt(hx * hx + hy * hy);
    const double after = sqrt(ox * ox + oy * oy);
    const double sum = before + after;
    if (!(sum > 1e-9))
        return 0.0f;
    const double diffSq = -2.0 * k * (hx * nx + hy * ny)
                        - k * k * (nx * nx + ny * ny);
    return (float)(diffSq / sum);
}

static inline float WrBoardLossEnergy(const WrBoardStats *b, float gravity)
{
    // E = z + |v|^2/2g and the clip does not move you, so dE is the kinetic part
    // alone: (|v|^2 - |v'|^2)/2g, and |v|^2 - |v'|^2 is exactly (v.n)^2.
    if (gravity < 1.0f)
        gravity = 1.0f;
    return (b->intoPlane * b->intoPlane) / (2.0f * gravity);
}

// What the clip did to each component of the velocity, signed, as a change and
// not as a loss: dv = -(v.n)n = +intoPlane * n once n points back at the player.
//
// The signs are worth reading rather than folding away. A clip PUSHES you along
// its normal, so on a ramp the z here is usually positive -- the board gave you
// height while taking speed, which is the whole trade a board is.
static inline void WrBoardDeltaAxes(const WrBoardStats *b, float out[3])
{
    out[0] = b->intoPlane * b->normal[0];
    out[1] = b->intoPlane * b->normal[1];
    out[2] = b->intoPlane * b->normal[2];
}

// How close to a board that cost nothing, as a percentage.
//
// 100% is velocity exactly parallel to the plane, which is the only board there
// is no such thing as improving on. This is `sinA` by another name: the share of
// the arriving speed the clip leaves behind.
static inline float WrBoardPerfectPct(const WrBoardStats *b)
{
    float pct = (1.0f - b->lossPct) * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
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
