// test_saveloc.cpp  --  the velocity in Momentum's file, and the readout that
// starts from it.
//
// Two things that cannot be checked by looking at them. A parser that hands a
// save-loc the PREVIOUS one's velocity produces a plausible number from the
// wrong place; a seed that is quietly wrong produces a fast readout that is
// worse than the slow one it replaced. Both look completely fine on screen.
//
// THE NUMBERS HERE ARE MEASURED, from the 3255 position records of 261 maps in
// the savedlocs.txt on the development machine:
//
//   - 3239 are save-locs ("cps"); the other 16 are "startmarks", a separate
//     section that carries a pos and NO vel, in 12 of those maps
//   - vel is present and finite in all 3239 save-locs
//   - 62% were saved above 250 u/s and 46% above 1000; only 17% standing still
//   - the fastest is 6058 u/s
//   - "time" is -1 in every single one, which is why WrLines keeps its own
//   - 13 fields in the file hold -nan(ind), and sscanf_s parses that happily
//
// Build:  tests\build.bat
// Run:    tests\test_saveloc.exe

#include "wr_savelocs.h"
#include "wr_energy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// wr_savelocs.cpp reaches for these; none matters to the parsing under test.
bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }
bool WrCameraOrigin(Vec3 *out) { (void)out; return false; }

// ---------------------------------------------------------------------------

static const char *kFixture = "tests\\fixture_savedlocs.txt";

static void WriteFixture(const char *body)
{
    FILE *f = NULL;
    if (fopen_s(&f, kFixture, "w") != 0 || !f)
    {
        printf("  [!] could not write %s\n", kFixture);
        g_failures++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

static int Parse(const char *body, const char *map, WrSavelocHit *out, int maxOut)
{
    WriteFixture(body);
    int n = 0;
    if (!WrSavelocParseFile(kFixture, map, out, maxOut, &n))
        return -1;
    return n;
}

int main(void)
{
    printf("\n=== wrlines save-loc velocity and the seeded readout ===\n");
    WrEnergyDefaults();
    WrEnergyReset();

    WrSavelocHit hits[300];

    // -----------------------------------------------------------------------
    printf("\na velocity belongs to the save-loc in its own block\n");
    {
        const char *body =
            "\"MOMSavelocSystem\"\n{\n"
            "\t\"surf_test\"\n\t{\n"
            "\t\t\"cps\"\n\t\t{\n"
            "\t\t\t\"000000000\"\n\t\t\t{\n"
            "\t\t\t\t\"ang\"\t\t\"0 0 0\"\n"
            "\t\t\t\t\"pos\"\t\t\"100 200 300\"\n"
            "\t\t\t\t\"predictedVel\"\t\t\"9999 9999 9999\"\n"
            "\t\t\t\t\"vel\"\t\t\"10 20 30\"\n"
            "\t\t\t}\n"
            "\t\t\t\"000000001\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"400 500 600\"\n"
            "\t\t\t\t\"vel\"\t\t\"40 50 60\"\n"
            "\t\t\t}\n"
            "\t\t}\n\t}\n}\n";
        int n = Parse(body, "surf_test", hits, 300);
        Check(n == 2, "both save-locs found");
        if (n == 2)
        {
            Check(hits[0].haveVel && hits[0].vel.x == 10.0f &&
                  hits[0].vel.z == 30.0f, "the first keeps its own velocity");
            Check(hits[1].haveVel && hits[1].vel.x == 40.0f,
                  "and the second keeps its own");
            Check(hits[0].vel.x != 9999.0f,
                  "predictedVel is not mistaken for vel");
            Check(hits[0].fromCps && hits[1].fromCps, "both are marked as cps");
        }
    }

    // -----------------------------------------------------------------------
    printf("\na startmark is not a save-loc, and has no velocity to lend\n");
    {
        // The real shape: "startmarks" sits beside "cps" in the same map block
        // and its entries carry a pos with no vel. There are 16 of them across
        // 12 maps on the development machine, and before this they were read in
        // as save-locs indistinguishable from the real ones.
        const char *body =
            "\"MOMSavelocSystem\"\n{\n"
            "\t\"surf_test\"\n\t{\n"
            "\t\t\"cps\"\n\t\t{\n"
            "\t\t\t\"000000000\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"100 200 300\"\n"
            "\t\t\t\t\"vel\"\t\t\"1500 0 0\"\n"
            "\t\t\t}\n"
            "\t\t}\n"
            "\t\t\"cur\"\t\t\"-1\"\n"
            "\t\t\"startmarks\"\n\t\t{\n"
            "\t\t\t\"0-1\"\n\t\t\t{\n"
            "\t\t\t\t\"ang\"\t\t\"0 0 0\"\n"
            "\t\t\t\t\"crouched\"\t\t\"0\"\n"
            "\t\t\t\t\"pos\"\t\t\"700 800 900\"\n"
            "\t\t\t}\n"
            "\t\t}\n\t}\n}\n";
        int n = Parse(body, "surf_test", hits, 300);
        Check(n == 2, "both positions are still read");
        if (n == 2)
        {
            Check(hits[0].fromCps, "the save-loc is marked as one");
            Check(!hits[1].fromCps, "the startmark is not");
            Check(!hits[1].haveVel,
                  "and it did NOT inherit the save-loc's 1500 u/s");
        }
    }

    // -----------------------------------------------------------------------
    printf("\na block with a velocity and no position attaches to nothing\n");
    {
        const char *body =
            "\"MOMSavelocSystem\"\n{\n"
            "\t\"surf_test\"\n\t{\n"
            "\t\t\"cps\"\n\t\t{\n"
            "\t\t\t\"000000000\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"100 200 300\"\n"
            "\t\t\t\t\"vel\"\t\t\"11 22 33\"\n"
            "\t\t\t}\n"
            "\t\t\t\"000000001\"\n\t\t\t{\n"
            "\t\t\t\t\"vel\"\t\t\"7777 0 0\"\n"
            "\t\t\t}\n"
            "\t\t}\n\t}\n}\n";
        int n = Parse(body, "surf_test", hits, 300);
        Check(n == 1, "only the one with a position is a save-loc");
        if (n == 1)
            Check(hits[0].vel.x == 11.0f,
                  "and the stray velocity did not overwrite it");
    }

    // -----------------------------------------------------------------------
    printf("\nnonsense in the file is refused, not clamped\n");
    {
        // sscanf_s reads "-nan(ind)" and reports three fields, so the field
        // count is not the guard -- WrSaneFloat's self-comparison is. The
        // development machine's file holds thirteen such values today.
        const char *body =
            "\"MOMSavelocSystem\"\n{\n"
            "\t\"surf_test\"\n\t{\n"
            "\t\t\"cps\"\n\t\t{\n"
            "\t\t\t\"000000000\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"100 200 300\"\n"
            "\t\t\t\t\"vel\"\t\t\"-nan(ind) 0 0\"\n"
            "\t\t\t}\n"
            "\t\t\t\"000000001\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"110 200 300\"\n"
            "\t\t\t\t\"vel\"\t\t\"999999 0 0\"\n"
            "\t\t\t}\n"
            "\t\t\t\"000000002\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"120 200 300\"\n"
            "\t\t\t\t\"vel\"\t\t\"60 70\"\n"
            "\t\t\t}\n"
            "\t\t}\n\t}\n}\n";
        int n = Parse(body, "surf_test", hits, 300);
        Check(n == 3, "all three positions still load");
        if (n == 3)
        {
            Check(!hits[0].haveVel, "a NaN velocity is refused");
            Check(!hits[1].haveVel, "an absurd one is refused");
            Check(!hits[2].haveVel, "and a two-component one is refused");
        }
    }

    // -----------------------------------------------------------------------
    printf("\nanother map's save-locs are not this map's\n");
    {
        const char *body =
            "\"MOMSavelocSystem\"\n{\n"
            "\t\"surf_other\"\n\t{\n"
            "\t\t\"cps\"\n\t\t{\n"
            "\t\t\t\"000000000\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"1 1 1\"\n"
            "\t\t\t\t\"vel\"\t\t\"5000 0 0\"\n"
            "\t\t\t}\n"
            "\t\t}\n\t}\n"
            "\t\"surf_test\"\n\t{\n"
            "\t\t\"cps\"\n\t\t{\n"
            "\t\t\t\"000000000\"\n\t\t\t{\n"
            "\t\t\t\t\"pos\"\t\t\"2 2 2\"\n"
            "\t\t\t\t\"vel\"\t\t\"100 0 0\"\n"
            "\t\t\t}\n"
            "\t\t}\n\t}\n}\n";
        int n = Parse(body, "surf_test", hits, 300);
        Check(n == 1 && hits[0].vel.x == 100.0f, "only ours, with our velocity");
    }

    // -----------------------------------------------------------------------
    printf("\nmore save-locs than the array holds\n");
    {
        // Past the cap, positions stop being committed but velocity lines keep
        // arriving. Without clearing the block's target index first they would
        // all land on the last record that did fit.
        static char big[200000];
        int len = _snprintf_s(big, sizeof(big), _TRUNCATE,
                              "\"MOMSavelocSystem\"\n{\n\t\"surf_test\"\n\t{\n"
                              "\t\t\"cps\"\n\t\t{\n");
        for (int i = 0; i < 40; i++)
            len += _snprintf_s(big + len, sizeof(big) - len, _TRUNCATE,
                               "\t\t\t\"%09d\"\n\t\t\t{\n"
                               "\t\t\t\t\"pos\"\t\t\"%d 0 0\"\n"
                               "\t\t\t\t\"vel\"\t\t\"%d 0 0\"\n"
                               "\t\t\t}\n", i, i * 100, (i + 1) * 10);
        _snprintf_s(big + len, sizeof(big) - len, _TRUNCATE, "\t\t}\n\t}\n}\n");

        int n = Parse(big, "surf_test", hits, 10);
        Check(n == 10, "stops at the cap");
        if (n == 10)
            Check(hits[9].haveVel && hits[9].vel.x == 100.0f,
                  "and the last one kept its OWN velocity, not a later one's");
    }

    // -----------------------------------------------------------------------
    printf("\na truncated file yields nothing rather than crashing\n");
    {
        const char *body =
            "\"MOMSavelocSystem\"\n{\n\t\"surf_test\"\n\t{\n\t\t\"cps\"\n\t\t{\n"
            "\t\t\t\"000000000\"\n\t\t\t{\n\t\t\t\t\"pos\"\t\t\"1 2";
        int n = Parse(body, "surf_test", hits, 300);
        Check(n == 0, "no save-locs, no crash");
    }

    // -----------------------------------------------------------------------
    printf("\nthe seed starts the readout at the right number\n");
    {
        WrEnergyReset();
        Vec3 cam = WrVec(0.0f, 0.0f, 500.0f);
        Vec3 vel = WrVec(1800.0f, 0.0f, 240.0f);

        WrEnergySeed(cam, vel, "a test");
        Check(WrEnergyValid(), "there is a reading immediately");

        // Exactly, not nearly. This is the whole claim.
        float want = WrEnergyOf(cam, vel);
        Check(fabsf(WrEnergyNow() - want) < 0.001f,
              "and it is exactly z + |v|^2/2g for the file's velocity");
        Check(fabsf(WrEnergySpeed() - WrLength(vel)) < 0.5f,
              "the speed is the file's speed");
    }

    // -----------------------------------------------------------------------
    printf("\na seed that disagrees with the first measurement is thrown out\n");
    {
        // The guard-rail. A wrong seed must cost the ~35 ms an unseeded load
        // already costs, and not a moment more -- so it is checked against the
        // first velocity actually measured, and dropped if the two disagree.
        WrEnergyReset();

        const float dt = 1.0f / 200.0f;
        Vec3 pos = WrVec(0.0f, 0.0f, 500.0f);
        const Vec3 truth = WrVec(300.0f, 0.0f, 0.0f);   // what really happens

        // Somewhere to teleport from, so the sampler has a history.
        for (int i = 0; i < 40; i++)
        {
            WrEnergySample(pos, dt);
            pos = WrAdd(pos, WrScale(truth, dt));
        }

        // Land far away, and claim a wildly wrong velocity for it.
        pos = WrVec(9000.0f, 0.0f, 500.0f);
        WrEnergySample(pos, dt);
        WrEnergySeed(pos, WrVec(4000.0f, 0.0f, 0.0f), "a test");
        Check(fabsf(WrEnergySpeed() - 4000.0f) < 1.0f,
              "the wrong seed is believed at first");

        // Now actually move at the truth, and let the window fill.
        for (int i = 0; i < 30; i++)
        {
            pos = WrAdd(pos, WrScale(truth, dt));
            WrEnergySample(pos, dt);
        }

        WrEnergySeedInfo si;
        bool have = WrEnergySeedReport(&si);
        Check(have && si.rejected, "the check rejects it");
        Check(have && si.rejects == 1, "and counts it");
        Check(fabsf(WrEnergySpeed() - 300.0f) < 60.0f,
              "and the readout is back on the measured speed, not the claim");
    }

    // -----------------------------------------------------------------------
    printf("\na seed that agrees is kept\n");
    {
        WrEnergyReset();
        const float dt = 1.0f / 200.0f;
        Vec3 pos = WrVec(0.0f, 0.0f, 500.0f);
        const Vec3 truth = WrVec(1200.0f, 0.0f, 0.0f);

        for (int i = 0; i < 40; i++)
        {
            WrEnergySample(pos, dt);
            pos = WrAdd(pos, WrScale(truth, dt));
        }

        pos = WrVec(9000.0f, 0.0f, 500.0f);
        WrEnergySample(pos, dt);
        WrEnergySeed(pos, truth, "a test");
        for (int i = 0; i < 30; i++)
        {
            pos = WrAdd(pos, WrScale(truth, dt));
            WrEnergySample(pos, dt);
        }

        WrEnergySeedInfo si;
        bool have = WrEnergySeedReport(&si);
        Check(have && !si.rejected, "kept");
        Check(have && si.rejects == 0 && si.seeds == 1, "one seed, none thrown out");
        Check(fabsf(si.speedErr) < 60.0f,
              "and the file agreed with the measurement");
    }

    // -----------------------------------------------------------------------
    printf("\na save-loc saved standing still still gets a readout\n");
    {
        // 550 of the 3239 were made below 1 u/s. A seed must not arm the
        // bit-identical hold before a real reading exists -- if it did, a
        // perfectly repeating camera would freeze the readout on the seed and
        // the velocity window would never fill to confirm or correct it.
        WrEnergyReset();
        const float dt = 1.0f / 200.0f;
        Vec3 pos = WrVec(10.0f, 20.0f, 640.0f);

        WrEnergySeed(pos, WrVec(0.0f, 0.0f, 0.0f), "a test");
        Check(fabsf(WrEnergyNow() - pos.z) < 0.001f,
              "energy is height alone when the velocity is zero");

        for (int i = 0; i < 40; i++)
            WrEnergySample(pos, dt);        // bit-identical, over and over

        // Still right, and right from the first frame rather than after the
        // window filled. Nothing here is ever judged, and that is correct: the
        // camera never moves, so there is no independent measurement to judge it
        // against -- and none is needed, because a seed of zero and a
        // measurement of zero cannot disagree.
        Check(WrEnergyValid() && WrEnergySpeed() < 1.0f,
              "still reads nothing moving, rather than nothing at all");
        Check(fabsf(WrEnergyNow() - pos.z) < 0.001f,
              "and energy is still exactly the height");

        // Move, and it takes over normally.
        Vec3 walk = pos;
        for (int i = 0; i < 40; i++)
        {
            walk = WrAdd(walk, WrScale(WrVec(250.0f, 0.0f, 0.0f), dt));
            WrEnergySample(walk, dt);
        }
        Check(WrEnergySpeed() > 100.0f,
              "and moving off it measures again rather than staying stuck");
    }

    // -----------------------------------------------------------------------
    printf("\nheld frozen on the loaded position, the loaded values stay\n");
    {
        // Holding Momentum's load key puts you at the stored position and
        // freezes you there. Nothing is moving, so the only velocity that can be
        // measured is zero -- and the guard-rail, comparing the file's answer
        // against that zero, threw out every seed of a save-loc made at speed.
        // Which is 62% of them. This is that defect.
        WrEnergyReset();
        const float dt = 1.0f / 200.0f;
        Vec3 pos = WrVec(0.0f, 0.0f, 500.0f);
        const Vec3 fast = WrVec(2400.0f, 0.0f, 0.0f);

        for (int i = 0; i < 40; i++)
        {
            WrEnergySample(pos, dt);
            pos = WrAdd(pos, WrScale(WrVec(300.0f, 0.0f, 0.0f), dt));
        }

        Vec3 landed = WrVec(9000.0f, 0.0f, 500.0f);
        WrEnergySample(landed, dt);
        WrEnergySeed(landed, fast, "a test");

        // Frozen: the same position, over and over, for a full second.
        for (int i = 0; i < 200; i++)
            WrEnergySample(landed, dt);

        Check(fabsf(WrEnergySpeed() - 2400.0f) < 1.0f,
              "a second of being frozen still reads the loaded speed");
        WrEnergySeedInfo si;
        Check(!WrEnergySeedReport(&si) || si.seeds == 0,
              "and the seed has not been judged, because nothing has moved");

        // Released, and now genuinely moving at the speed the file promised.
        for (int i = 0; i < 40; i++)
        {
            landed = WrAdd(landed, WrScale(fast, dt));
            WrEnergySample(landed, dt);
        }
        bool have = WrEnergySeedReport(&si);
        Check(have && si.seeds == 1, "moving again, it is judged");
        Check(have && !si.rejected,
              "and kept -- the frozen stretch did not drag the measurement down");
        Check(fabsf(WrEnergySpeed() - 2400.0f) < 120.0f,
              "the readout never left the loaded speed");
    }

    // -----------------------------------------------------------------------
    printf("\nan absurd seed is refused outright\n");
    {
        WrEnergyReset();
        Vec3 cam = WrVec(0.0f, 0.0f, 100.0f);
        WrEnergySeed(cam, WrVec(1e30f, 0.0f, 0.0f), "a test");
        Check(!WrEnergyValid(), "nothing was seeded from an infinite velocity");
        float nan = sqrtf(-1.0f);
        WrEnergySeed(cam, WrVec(nan, 0.0f, 0.0f), "a test");
        Check(!WrEnergyValid(), "nor from a NaN one");
    }

    remove(kFixture);
    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
