#ifndef WR_ARGS_H
#define WR_ARGS_H

// Option parsing for wrinject.exe.
//
// It lives in a header of its own for one reason: injector.cpp is a translation
// unit with main() in it and nothing else, so there is no way to drive its
// parsing from a test harness without linking two mains together. Everything
// here is a pure function of argv -- no Windows, no files, no globals -- which
// is what lets tests\test_args.cpp cover the awkward shapes properly.
//
// The one hard rule: the two forms that existed before --wait did
//
//     wrinject.exe
//     wrinject.exe path\to\wrlines.dll
//
// must parse exactly as they always did. Those are what every doc, every
// screenshot and every habit in the field say to type.

#include <string.h>
#include <stdlib.h>

// Two minutes. Long enough for a cold Source start on a slow disk inside a Wine
// prefix, short enough that a launch option pointing at a game that is never
// coming does not sit there for the rest of the session.
#define WR_WAIT_DEFAULT_SECONDS 120
#define WR_WAIT_MIN_SECONDS       1
#define WR_WAIT_MAX_SECONDS    3600

struct WrInjectArgs
{
    const char *dll;            // the positional argument, or NULL for "beside the exe"
    bool        wait;           // --wait was given
    int         waitSeconds;    // its value, or WR_WAIT_DEFAULT_SECONDS
    bool        help;           // --help was given; nothing else matters
    const char *bad;            // the argument that made no sense, or NULL
};

// A flag is a dash followed by something. A bare "-" is not a flag, it is a
// (silly) filename, and treating it as one keeps the positional rule simple.
static inline bool WrArgIsFlag(const char *s)
{
    return s && s[0] == '-' && s[1] != '\0';
}

static inline bool WrArgAllDigits(const char *s)
{
    if (!s || !*s)
        return false;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

// "120" -> 120. Anything else -- empty, signed, trailing junk, or a value
// outside the range -- is a refusal rather than a clamp. A typo in a launch
// option should say so rather than quietly wait for a different length of time
// than the one that was asked for, because on Linux nobody is watching the
// console when this runs.
static inline bool WrArgSeconds(const char *s, int *out)
{
    if (!WrArgAllDigits(s))
        return false;
    long v = strtol(s, NULL, 10);
    if (v < WR_WAIT_MIN_SECONDS || v > WR_WAIT_MAX_SECONDS)
        return false;
    *out = (int)v;
    return true;
}

static inline void WrParseInjectArgs(int argc, char **argv, WrInjectArgs *out)
{
    out->dll = NULL;
    out->wait = false;
    out->waitSeconds = WR_WAIT_DEFAULT_SECONDS;
    out->help = false;
    out->bad = NULL;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!a)
            continue;

        if (!WrArgIsFlag(a))
        {
            // A second path is a mistake worth naming. The old code ignored
            // everything after argv[1] in silence, which meant a typo'd flag
            // landed as a path and then vanished.
            if (out->dll) { out->bad = a; return; }
            out->dll = a;
            continue;
        }

        if (strcmp(a, "-h") == 0 || strcmp(a, "-?") == 0 || strcmp(a, "--help") == 0)
        {
            out->help = true;
            return;
        }

        if (strcmp(a, "-w") == 0 || strcmp(a, "--wait") == 0)
        {
            out->wait = true;
            // A run of digits after the flag is its value. Anything else is the
            // next argument in its own right, so "--wait wrlines.dll" means the
            // default wait and that path, not a broken number.
            //
            // Digits are consumed even when out of range, so "--wait 99999"
            // is an error about 99999 rather than a silent default wait with
            // 99999 mistaken for a filename.
            if (i + 1 < argc && WrArgAllDigits(argv[i + 1]))
            {
                i++;
                if (!WrArgSeconds(argv[i], &out->waitSeconds)) { out->bad = argv[i]; return; }
            }
            continue;
        }

        if (strncmp(a, "--wait=", 7) == 0)
        {
            out->wait = true;
            if (!WrArgSeconds(a + 7, &out->waitSeconds)) { out->bad = a; return; }
            continue;
        }

        out->bad = a;
        return;
    }
}

#endif // WR_ARGS_H
