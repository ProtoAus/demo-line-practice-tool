// wr_matrixlife.h  --  "is the address we picked still the matrix?", as pure
// arithmetic.
//
// Header-only and free of Windows, D3D and engine types on purpose: this is the
// logic that decides whether to keep drawing, and getting it wrong is invisible
// until someone changes map and everything silently stops. It is separated out
// so it can be run against a script of frames in a test harness rather than only
// against a running game -- see tests\test_matrixlife.cpp.
//
// TWO WAYS AN ADDRESS DIES, and they look nothing alike.
//
//   It stops passing the oracle. Easy to see: validation fails, frame after
//   frame. The only difficulty is not over-reacting, because a level load also
//   produces a long run of failures and that is not the address's fault.
//
//   It stops being written. Much harder: memory keeps its last contents, so a
//   dead slot goes on passing the oracle forever, perfectly valid and perfectly
//   motionless. Nothing distinguishes it from a player standing still, EXCEPT
//   the company it keeps -- if other candidate matrices are being rewritten,
//   something is rendering a world, and ours is not part of it.
//
// The second rule only applies after a map change (`proving`). Outside that, a
// motionless matrix means a motionless player, which is not a defect.

#ifndef WR_MATRIXLIFE_H
#define WR_MATRIXLIFE_H

// How long a resolved address may fail validation before we give it up.
//
// SECONDS, not frames, and that is the correction to a real bug: the original
// was 600 frames, described as "a long grace period" for menus and loading
// screens. It is not. A loading screen still presents at several hundred frames
// per second, so 600 frames was a second and a half, and a perfectly good
// address was declared stale 1.39 s into a level load and replaced with a junk
// one from the leftover candidate list.
#define WR_STALE_SECONDS 15.0f

// How much "the world is moving and this matrix is not" it takes, after a map
// change, to conclude the address did not survive the load.
#define WR_DEAD_SECONDS 4.0f

struct WrMatrixLife
{
    float staleSeconds;   // since it last passed the oracle
    float deadSeconds;    // frozen while the world around it moved
    bool proving;         // a map change is waiting to be proved survivable
};

static inline void WrMatrixLifeReset(WrMatrixLife *s)
{
    s->staleSeconds = 0.0f;
    s->deadSeconds = 0.0f;
    s->proving = false;
}

// A level load. The address is kept -- it does not move, and rescanning would
// only re-roll which of several equally valid matrices gets picked -- but it now
// has something to prove.
static inline void WrMatrixLifeOnMapChange(WrMatrixLife *s)
{
    s->staleSeconds = 0.0f;   // the load itself must not count against it
    s->deadSeconds = 0.0f;
    s->proving = true;
}

// One frame. Returns NULL to keep the address, or the reason to give it up --
// in which case the state is reset, so the caller can act on it once.
//
//   validated    the address still holds something the oracle accepts
//   chosenMoved  its contents differed from last frame
//   worldAlive   some OTHER candidate changed recently, i.e. a world is being
//                rendered. False during a loading screen, which is what stops a
//                slow load from being read as a dead address.
static inline const char *WrMatrixLifeTick(WrMatrixLife *s, float dt,
                                           bool validated, bool chosenMoved,
                                           bool worldAlive)
{
    if (validated)
        s->staleSeconds = 0.0f;
    else
        s->staleSeconds += dt;

    if (chosenMoved)
    {
        // It is alive. Nothing left to prove and nothing accumulated against it.
        s->proving = false;
        s->deadSeconds = 0.0f;
    }
    else if (s->proving && worldAlive)
    {
        s->deadSeconds += dt;
    }

    const char *why = 0;
    if (s->staleSeconds > WR_STALE_SECONDS)
        why = "stopped passing the oracle";
    else if (s->deadSeconds > WR_DEAD_SECONDS)
        why = "stopped being written while the world kept moving";

    if (why)
        WrMatrixLifeReset(s);
    return why;
}

#endif // WR_MATRIXLIFE_H
