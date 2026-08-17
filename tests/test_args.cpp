// test_args.cpp  --  wrinject.exe's command line.
//
// This harness exists because of one promise and one platform.
//
// The promise: `wrinject.exe` and `wrinject.exe path\to\wrlines.dll` are what
// every doc, every screenshot and every habit in the field say to type, and they
// have to keep parsing exactly as they did before --wait was added. That is the
// first section below and it is the one that must never go red.
//
// The platform: on Linux this program runs from a Steam launch option, with no
// console anybody is looking at, started before the game by Proton. A mistyped
// argument there does not produce a visible error -- it produces a tool that
// silently never appears. So the awkward shapes are pinned here rather than
// discovered by somebody whose game just does not have a panel in it:
//
//   1. The two legacy forms, unchanged.
//   2. --wait with a value, without one, and with the value on either side of
//      the DLL path.
//   3. A run of digits after --wait is ALWAYS its value, even when out of
//      range -- so "--wait 99999" is an error about 99999 rather than a silent
//      default wait with 99999 quietly taken for a filename.
//   4. Anything that is not a number after --wait is the next argument in its
//      own right, so "--wait wrlines.dll" is a default wait and that path.
//   5. Refusals: an unknown flag, a second positional, an empty or non-numeric
//      value. The old code ignored everything after argv[1] in silence, which
//      meant a typo'd flag landed as a path and then vanished.
//
// Build:  tests\build.bat
// Run:    tests\test_args.exe

#include "wr_args.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static WrInjectArgs Run(int argc, const char **argv)
{
    WrInjectArgs a;
    WrParseInjectArgs(argc, (char **)argv, &a);
    return a;
}

// argv[0] is the program, which the parser skips, so every case below carries a
// realistic one rather than pretending arguments start at zero.
#define CASE(...) \
    const char *v[] = { "wrinject.exe", __VA_ARGS__ }; \
    WrInjectArgs a = Run((int)(sizeof(v) / sizeof(v[0])), v)

static bool SameStr(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

int main(void)
{
    printf("\n== wr_args: the two old forms ==\n\n");

    {
        const char *v[] = { "wrinject.exe" };
        WrInjectArgs a = Run(1, v);
        Check(a.dll == NULL, "no arguments: no path, so the default is used");
        Check(!a.wait && !a.help && !a.bad, "no arguments: nothing else set");
        Check(a.waitSeconds == WR_WAIT_DEFAULT_SECONDS,
              "no arguments: the default is still filled in");
    }
    {
        CASE("wrlines.dll");
        Check(SameStr(a.dll, "wrlines.dll"), "one path: it is the path");
        Check(!a.wait && !a.help && !a.bad, "one path: nothing else set");
    }
    {
        CASE("C:\\Program Files\\wrlines\\wrlines.dll");
        Check(SameStr(a.dll, "C:\\Program Files\\wrlines\\wrlines.dll"),
              "a path with spaces arrives whole");
    }
    {
        // Not a flag: a bare dash is a (silly) filename, and keeping it that way
        // is what makes the positional rule one sentence long.
        CASE("-");
        Check(SameStr(a.dll, "-") && !a.bad, "a bare dash is a path, not a flag");
    }

    printf("\n== --wait ==\n\n");

    {
        CASE("--wait");
        Check(a.wait, "--wait alone: waiting");
        Check(a.waitSeconds == WR_WAIT_DEFAULT_SECONDS, "--wait alone: default seconds");
        Check(a.dll == NULL && !a.bad, "--wait alone: no path, no complaint");
    }
    {
        CASE("-w");
        Check(a.wait && a.waitSeconds == WR_WAIT_DEFAULT_SECONDS, "-w is the same flag");
    }
    {
        CASE("--wait", "60");
        Check(a.wait && a.waitSeconds == 60, "--wait 60");
        Check(a.dll == NULL && !a.bad, "--wait 60: the 60 was eaten, not kept as a path");
    }
    {
        CASE("--wait=60");
        Check(a.wait && a.waitSeconds == 60, "--wait=60");
    }
    {
        CASE("--wait", "60", "wrlines.dll");
        Check(a.wait && a.waitSeconds == 60 && SameStr(a.dll, "wrlines.dll"),
              "--wait 60 path");
    }
    {
        CASE("wrlines.dll", "--wait", "60");
        Check(a.wait && a.waitSeconds == 60 && SameStr(a.dll, "wrlines.dll"),
              "path --wait 60, the other way round");
    }
    {
        CASE("--wait=60", "wrlines.dll");
        Check(a.wait && a.waitSeconds == 60 && SameStr(a.dll, "wrlines.dll"),
              "--wait=60 path");
    }
    {
        // The one that would bite hardest if it went the other way: the value is
        // optional, so a path directly after the flag must stay a path.
        CASE("--wait", "wrlines.dll");
        Check(a.wait && a.waitSeconds == WR_WAIT_DEFAULT_SECONDS,
              "--wait path: default seconds");
        Check(SameStr(a.dll, "wrlines.dll") && !a.bad,
              "--wait path: the path survived");
    }
    {
        CASE("--wait", "1");
        Check(a.wait && a.waitSeconds == WR_WAIT_MIN_SECONDS, "one second is allowed");
    }
    {
        CASE("--wait", "3600");
        Check(a.wait && a.waitSeconds == WR_WAIT_MAX_SECONDS, "an hour is allowed");
    }
    {
        // No --wait, so a number is just a filename. Consistent rather than
        // clever: only the flag gives digits a second meaning.
        CASE("60");
        Check(SameStr(a.dll, "60") && !a.wait && !a.bad,
              "a bare number with no --wait is a path");
    }
    {
        CASE("--wait", "60", "60");
        Check(a.wait && a.waitSeconds == 60 && SameStr(a.dll, "60"),
              "--wait 60 60: first is the value, second is the path");
    }

    printf("\n== refusals ==\n\n");

    {
        CASE("--wait", "0");
        Check(SameStr(a.bad, "0"), "zero seconds is refused, not clamped");
    }
    {
        CASE("--wait", "99999");
        Check(SameStr(a.bad, "99999"),
              "out of range is an error about the number, not a stray path");
    }
    {
        CASE("--wait=0");
        Check(SameStr(a.bad, "--wait=0"), "--wait=0 names the whole argument");
    }
    {
        CASE("--wait=abc");
        Check(SameStr(a.bad, "--wait=abc"), "--wait=abc is refused");
    }
    {
        CASE("--wait=");
        Check(SameStr(a.bad, "--wait="), "an empty value is refused");
    }
    {
        CASE("--wait=-5");
        Check(SameStr(a.bad, "--wait=-5"), "a negative value is refused");
    }
    {
        CASE("--wait=60x");
        Check(SameStr(a.bad, "--wait=60x"), "trailing junk is refused");
    }
    {
        CASE("--nope");
        Check(SameStr(a.bad, "--nope"), "an unknown flag is named");
    }
    {
        CASE("--exe", "mom_orig.exe");
        Check(SameStr(a.bad, "--exe"),
              "--exe does not exist and says so (see injector.cpp on why)");
    }
    {
        CASE("a.dll", "b.dll");
        Check(SameStr(a.bad, "b.dll"), "a second path is named rather than ignored");
    }
    {
        // The old parser took argv[1] as a path no matter what it looked like,
        // so this silently ran with a nonexistent DLL called "--wiat".
        CASE("--wiat", "60");
        Check(SameStr(a.bad, "--wiat"), "a typo'd flag is an error, not a filename");
    }

    printf("\n== --help ==\n\n");

    {
        CASE("--help");
        Check(a.help && !a.bad, "--help");
    }
    {
        CASE("-h");
        Check(a.help, "-h");
    }
    {
        CASE("-?");
        Check(a.help, "-?");
    }
    {
        // Help wins and stops: there is no point validating the rest of a line
        // whose answer is a usage block.
        CASE("--help", "--nonsense");
        Check(a.help && !a.bad, "--help stops before anything after it");
    }
    {
        CASE("--wait", "--help");
        Check(a.wait && a.help,
              "--wait --help: the flag is not eaten as a wait value");
    }

    printf("\n%s  (%d failure%s)\n\n", g_failures ? "FAILED" : "all ok",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
