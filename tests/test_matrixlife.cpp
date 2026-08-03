// test_matrixlife.cpp  --  run the "is this still the matrix" state machine
// against scripted frames.
//
// This logic decides whether the tool draws anything at all, and every one of
// its failure modes is silent: too eager and the lines stop for no reason, too
// slow and a map change kills them until you restart the game. The second one
// shipped. It was found by a user, not by me, because there was nothing to run.
//
// Build:  cl /nologo /EHsc /I.. tests\test_matrixlife.cpp /Fe:tests\test_matrixlife.exe
// Run:    tests\test_matrixlife.exe

#include "wr_matrixlife.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// Run `seconds` of frames at `fps`, returning the reason it gave up, or NULL.
struct Frame
{
    bool validated;
    bool chosenMoved;
    bool worldAlive;
};

static const char *Run(WrMatrixLife *s, float seconds, float fps, Frame f)
{
    const float dt = 1.0f / fps;
    int frames = (int)(seconds * fps + 0.5f);
    for (int i = 0; i < frames; i++)
    {
        const char *why = WrMatrixLifeTick(s, dt, f.validated, f.chosenMoved,
                                           f.worldAlive);
        if (why)
            return why;
    }
    return 0;
}

// The states a real session moves through.
static const Frame kPlaying   = { true,  true,  true  };   // in a map, moving
static const Frame kStanding  = { true,  false, true  };   // in a map, still
static const Frame kLoading   = { false, false, false };   // level load
static const Frame kMenu      = { true,  false, false };   // frozen, nothing else moving
static const Frame kDead      = { true,  false, true  };   // dead slot, world moving

int main(void)
{
    WrMatrixLife s;

    printf("\nthe address survives the load\n");
    {
        WrMatrixLifeReset(&s);
        Run(&s, 10.0f, 300.0f, kPlaying);
        WrMatrixLifeOnMapChange(&s);
        const char *a = Run(&s, 8.0f, 400.0f, kLoading);      // long load
        const char *b = Run(&s, 60.0f, 300.0f, kPlaying);     // then playing
        Check(a == 0, "an 8 s load at 400 fps does not kill it");
        Check(b == 0, "a minute of play afterwards does not kill it");
        Check(!s.proving, "probation is cleared once it is seen moving");
    }

    printf("\nthe address does not survive the load\n");
    {
        WrMatrixLifeReset(&s);
        Run(&s, 10.0f, 300.0f, kPlaying);
        WrMatrixLifeOnMapChange(&s);
        Run(&s, 6.0f, 400.0f, kLoading);
        const char *why = Run(&s, 30.0f, 300.0f, kDead);
        Check(why != 0, "frozen while the world moves is caught");
        Check(why && strstr(why, "stopped being written") != 0,
              "and reported as \"stopped being written\"");
    }

    printf("\nhow long that takes\n");
    {
        WrMatrixLifeReset(&s);
        WrMatrixLifeOnMapChange(&s);
        float t = 0.0f;
        const float dt = 1.0f / 300.0f;
        while (t < 30.0f)
        {
            t += dt;
            if (WrMatrixLifeTick(&s, dt, true, false, true))
                break;
        }
        printf("     caught after %.2f s (budget %.1f s)\n", t, WR_DEAD_SECONDS);
        Check(t > WR_DEAD_SECONDS && t < WR_DEAD_SECONDS + 0.1f,
              "fires just past the budget, not before and not much after");
    }

    printf("\nstanding still is not death\n");
    {
        // The case that matters most: no map change, so the rule must not apply
        // however long the player stands there.
        WrMatrixLifeReset(&s);
        Run(&s, 10.0f, 300.0f, kPlaying);
        const char *why = Run(&s, 600.0f, 300.0f, kStanding);
        Check(why == 0, "ten minutes motionless in a map is never called death");
    }

    printf("\nstanding still right after a load is not death either\n");
    {
        // Probation is live here, so this is the honest cost of the rule: stand
        // perfectly still for the whole budget immediately after a level load
        // and it re-picks. Verify it is at least not instant, and that moving
        // for even a moment clears it for good.
        WrMatrixLifeReset(&s);
        WrMatrixLifeOnMapChange(&s);
        Run(&s, 2.0f, 300.0f, kStanding);
        const char *moved = Run(&s, 0.05f, 300.0f, kPlaying);
        const char *after = Run(&s, 600.0f, 300.0f, kStanding);
        Check(moved == 0, "two seconds still, then moving, survives");
        Check(after == 0, "and it is never re-examined afterwards");
    }

    printf("\nthe menu\n");
    {
        // Disconnected: frozen, and nothing else moving either. No map change
        // happened, so nothing should be concluded -- this is the case the
        // separate freeze cutoff handles, by drawing nothing.
        WrMatrixLifeReset(&s);
        Run(&s, 10.0f, 300.0f, kPlaying);
        const char *why = Run(&s, 600.0f, 300.0f, kMenu);
        Check(why == 0, "ten minutes at the menu triggers no re-pick");
    }

    printf("\nfailing the oracle outright\n");
    {
        WrMatrixLifeReset(&s);
        Run(&s, 10.0f, 300.0f, kPlaying);
        const char *early = Run(&s, WR_STALE_SECONDS - 1.0f, 400.0f, kLoading);
        Check(early == 0, "a 14 s load at 400 fps is still tolerated");

        WrMatrixLifeReset(&s);
        const char *why = Run(&s, WR_STALE_SECONDS + 2.0f, 400.0f, kLoading);
        Check(why != 0, "but 17 s of failing validation is not");
        Check(why && strstr(why, "oracle") != 0,
              "and is reported as \"stopped passing the oracle\"");
    }

    printf("\nthe frame rate does not change the answer\n");
    {
        // The bug being fixed: the old rule counted frames, so a loading screen
        // running fast burned the whole budget in a fraction of the time.
        float caught[3] = {0, 0, 0};
        const float rates[3] = { 60.0f, 300.0f, 1000.0f };
        for (int r = 0; r < 3; r++)
        {
            WrMatrixLifeReset(&s);
            WrMatrixLifeOnMapChange(&s);
            float t = 0.0f, dt = 1.0f / rates[r];
            while (t < 30.0f)
            {
                t += dt;
                if (WrMatrixLifeTick(&s, dt, true, false, true))
                    break;
            }
            caught[r] = t;
            printf("     %6.0f fps -> %.3f s\n", rates[r], t);
        }
        float spread = caught[2] > caught[0] ? caught[2] - caught[0]
                                             : caught[0] - caught[2];
        Check(spread < 0.05f, "60 fps and 1000 fps agree to within 50 ms");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
