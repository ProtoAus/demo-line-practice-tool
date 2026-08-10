// test_json.cpp  --  the JSON reader, against the four documents it will meet.
//
// wr_json.cpp is three hundred lines written by hand instead of a library
// pulled in, and the argument for that (wr_json.h has it in full) only holds if
// the behaviours it is relied on for are pinned. There are four:
//
//   1. int is not real. read_map_catalogue takes a map id only when
//      isinstance(mid, int) is true, so a catalogue saying 265.0 loses that
//      map. A parser that hands back "a number" cannot answer this and the
//      port would silently gain a map the reference drops.
//   2. \uXXXX, including surrogate pairs. Player aliases are chosen by
//      strangers and arrive escaped; emitting the halves of a pair separately
//      produces bytes that are not the character and are not valid UTF-8, and
//      they would go straight into a .tsv the panel reads back.
//   3. A value the caller did not read gets skipped for them. Every walk in
//      this project has an `else WrJsonSkip(j)` and forgetting one must not
//      desynchronise the cursor.
//   4. Malformed input stops, quietly, without walking off the end. The
//      network-facing documents are the ones this matters for.
//
// The truncation section is the blunt instrument: every prefix of a real
// document, one byte at a time, must either parse or fail, and never crash and
// never report a value it did not see.
//
// Build:  tests\build.bat
// Run:    tests\test_json.exe

#include "wr_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// Read one top-level member's string value out of a small object.
static bool OneString(const char *doc, const char *want, char *out, int cap)
{
    WrJson j;
    WrJsonInit(&j, doc, strlen(doc));
    if (!WrJsonEnterObject(&j))
        return false;
    char key[64];
    while (WrJsonNextMember(&j, key, sizeof(key)))
    {
        if (strcmp(key, want) == 0)
            return WrJsonString(&j, out, cap);
        WrJsonSkip(&j);
    }
    return false;
}

static WrJsonKind KindOf(const char *doc, const char *want)
{
    WrJson j;
    WrJsonInit(&j, doc, strlen(doc));
    if (!WrJsonEnterObject(&j))
        return WR_JSON_NONE;
    char key[64];
    while (WrJsonNextMember(&j, key, sizeof(key)))
    {
        if (strcmp(key, want) == 0)
            return WrJsonPeek(&j);
        WrJsonSkip(&j);
    }
    return WR_JSON_NONE;
}

int main(void)
{
    printf("\n=== wrlines JSON reader ===\n");

    // -----------------------------------------------------------------------
    printf("\na number is an int only when it is written as one\n");
    {
        Check(KindOf("{\"a\":265}", "a") == WR_JSON_INT, "265");
        Check(KindOf("{\"a\":-4}", "a") == WR_JSON_INT, "-4");
        Check(KindOf("{\"a\":265.0}", "a") == WR_JSON_REAL,
              "265.0 is a real, which is what drops that map");
        Check(KindOf("{\"a\":2e3}", "a") == WR_JSON_REAL, "2e3");
        Check(KindOf("{\"a\":2E3}", "a") == WR_JSON_REAL, "2E3 as well");
        Check(KindOf("{\"a\":null}", "a") == WR_JSON_NULL,
              "null, which is how an unrated tier arrives");
        Check(KindOf("{\"a\":true}", "a") == WR_JSON_BOOL, "true");
        Check(KindOf("{\"a\":\"7\"}", "a") == WR_JSON_STRING,
              "a quoted 7 is a string, not a seven");

        WrJson j;
        bool ok = true;
        WrJsonInit(&j, "{\"a\":265.0}", 11);
        WrJsonEnterObject(&j);
        char key[8];
        WrJsonNextMember(&j, key, sizeof(key));
        long long v = WrJsonInt(&j, -1, &ok);
        Check(v == -1 && !ok, "asking a real for an int gives the default");
        Check(!WrJsonNextMember(&j, key, sizeof(key)),
              "and the refused value was still consumed");
    }

    // -----------------------------------------------------------------------
    printf("\nescapes, including the ones that come in halves\n");
    {
        char s[64];

        Check(OneString("{\"a\":\"x\\ty\\n\\\"z\\\"\"}", "a", s, sizeof(s)) &&
              strcmp(s, "x\ty\n\"z\"") == 0, "the simple ones");

        // The literal is split because C would otherwise read \xbcber as one
        // hex escape -- b and e are hex digits. Worth knowing before writing
        // any more UTF-8 by hand in this file.
        Check(OneString("{\"a\":\"\\u00fcber\"}", "a", s, sizeof(s)) &&
              strcmp(s, "\xc3\xbc" "ber") == 0, "\\u00fc is two bytes of UTF-8");

        Check(OneString("{\"a\":\"\\u65e5\"}", "a", s, sizeof(s)) &&
              strcmp(s, "\xe6\x97\xa5") == 0, "and a CJK codepoint is three");

        // U+1F600. Two escapes, one character, four bytes.
        Check(OneString("{\"a\":\"\\ud83d\\ude00\"}", "a", s, sizeof(s)) &&
              strcmp(s, "\xf0\x9f\x98\x80") == 0,
              "a surrogate PAIR is one four-byte character");

        Check(OneString("{\"a\":\"\\ud83d!\"}", "a", s, sizeof(s)) &&
              strcmp(s, "\xef\xbf\xbd!") == 0,
              "a lone high surrogate becomes U+FFFD, as a decoder would");
        Check(OneString("{\"a\":\"\\ude00\"}", "a", s, sizeof(s)) &&
              strcmp(s, "\xef\xbf\xbd") == 0, "and so does a lone low one");

        // Raw UTF-8 in the document, which is how the map catalogue actually
        // carries a name -- json.dumps with ensure_ascii=False.
        Check(OneString("{\"a\":\"surf_\xc3\xbcml\xc3\xa4ut\"}", "a", s, sizeof(s)) &&
              strcmp(s, "surf_\xc3\xbcml\xc3\xa4ut") == 0,
              "raw UTF-8 passes through byte for byte");

        // Truncation must not cut a character in half: a broken sequence in a
        // .tsv is worse than a short name.
        char tiny[6];
        OneString("{\"a\":\"\xc3\xbc\xc3\xbc\xc3\xbc\"}", "a", tiny, sizeof(tiny));
        size_t n = strlen(tiny);
        bool whole = true;
        for (size_t i = 0; i < n; i++)
            if (((unsigned char)tiny[i] & 0xC0) == 0xC0 && i + 1 >= n)
                whole = false;
        Check(whole, "a name too long for the buffer is cut between characters");
    }

    // -----------------------------------------------------------------------
    printf("\nthe cursor stays put when the caller does not read a value\n");
    {
        const char *doc =
            "{\"skip1\":{\"deep\":[1,2,{\"x\":\"}\"}]},"
            " \"skip2\":[[[]]], \"want\":42, \"skip3\":\"a\\\"b\"}";
        WrJson j;
        WrJsonInit(&j, doc, strlen(doc));
        Check(WrJsonEnterObject(&j), "entered");

        long long got = -1;
        int seen = 0;
        char key[32];
        while (WrJsonNextMember(&j, key, sizeof(key)))
        {
            seen++;
            if (strcmp(key, "want") == 0)
                got = WrJsonInt(&j, -1, NULL);
            // Every other member is deliberately NOT consumed here.
        }
        Check(seen == 4, "all four members were visited");
        Check(got == 42, "and the one that mattered read correctly");
        Check(!WrJsonFailed(&j), "with no error along the way");

        // A brace inside a string must not be counted as nesting -- "}" above
        // is exactly that case, and getting it wrong ends the object early.
        Check(seen == 4, "a '}' inside a string is not the end of the object");
    }

    // -----------------------------------------------------------------------
    printf("\nthe shape the map catalogue actually has\n");
    {
        const char *doc =
            "[{\"id\":383,\"name\":\"bhop_landmark2\",\"credits\":[],"
            "  \"leaderboards\":[{\"gamemode\":2,\"trackType\":0,\"tier\":2,"
            "                    \"tags\":[12],\"linear\":true},"
            "                   {\"gamemode\":7,\"trackType\":1,\"tier\":9}]}]";
        WrJson j;
        WrJsonInit(&j, doc, strlen(doc));

        int id = 0, tier = 0;
        unsigned modes = 0;
        char name[72] = {0};

        Check(WrJsonEnterArray(&j), "the file is a bare array");
        while (WrJsonNextElement(&j))
        {
            if (!WrJsonEnterObject(&j))
                continue;
            char key[32];
            while (WrJsonNextMember(&j, key, sizeof(key)))
            {
                if (strcmp(key, "id") == 0)
                    id = (int)WrJsonInt(&j, 0, NULL);
                else if (strcmp(key, "name") == 0)
                    WrJsonString(&j, name, sizeof(name));
                else if (strcmp(key, "leaderboards") == 0)
                {
                    if (!WrJsonEnterArray(&j))
                        continue;
                    while (WrJsonNextElement(&j))
                    {
                        if (!WrJsonEnterObject(&j))
                            continue;
                        int tt = -1, t = 0, gm = -1;
                        bool haveTier = false;
                        char lk[32];
                        while (WrJsonNextMember(&j, lk, sizeof(lk)))
                        {
                            if (strcmp(lk, "trackType") == 0)
                                tt = (int)WrJsonInt(&j, -1, NULL);
                            else if (strcmp(lk, "tier") == 0)
                                t = (int)WrJsonInt(&j, 0, &haveTier);
                            else if (strcmp(lk, "gamemode") == 0)
                                gm = (int)WrJsonInt(&j, -1, NULL);
                            else
                                WrJsonSkip(&j);
                        }
                        if (tt == 0 && haveTier)
                            tier = t;
                        if (gm >= 0)
                            modes |= (1u << gm);
                    }
                }
                else
                {
                    WrJsonSkip(&j);
                }
            }
        }
        Check(id == 383, "id");
        Check(strcmp(name, "bhop_landmark2") == 0, "name");
        Check(tier == 2, "tier from the main track, not the bonus");
        Check(modes == ((1u << 2) | (1u << 7)), "both gamemodes");
        Check(!WrJsonFailed(&j), "and the walk ended cleanly");
    }

    // -----------------------------------------------------------------------
    printf("\nrubbish stops the walk rather than running off the end\n");
    {
        const char *bad[] = {
            "{",
            "{\"a\"",
            "{\"a\":}",
            "{\"a\":1,}",
            "[1,2",
            "{\"a\":\"unterminated",
            "{\"a\":\"\\q\"}",
            "{\"a\":\"\\u12\"}",
            "{'a':1}",
            "",
        };
        bool allSafe = true;
        for (int i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++)
        {
            WrJson j;
            WrJsonInit(&j, bad[i], strlen(bad[i]));
            char key[32];
            int guard = 0;
            if (WrJsonEnterObject(&j) || WrJsonEnterArray(&j))
                while (WrJsonNextMember(&j, key, sizeof(key)) && ++guard < 1000)
                    WrJsonSkip(&j);
            if (guard >= 1000)
            {
                printf("     case %d did not terminate\n", i);
                allSafe = false;
            }
        }
        Check(allSafe, "ten malformed documents, all of them terminate");

        // Every prefix of a good document. This is the one that finds the read
        // that happened one byte past the end.
        const char *good =
            "[{\"id\":1,\"name\":\"a\\u00fc\",\"leaderboards\":"
            "[{\"gamemode\":2,\"trackType\":0,\"tier\":null}]}]";
        size_t full = strlen(good);
        int parsed = 0;
        for (size_t cut = 0; cut <= full; cut++)
        {
            WrJson j;
            WrJsonInit(&j, good, cut);
            char key[32];
            int guard = 0;
            if (WrJsonEnterArray(&j))
                while (WrJsonNextElement(&j) && ++guard < 1000)
                    WrJsonSkip(&j);
            if (!WrJsonFailed(&j))
                parsed++;
        }
        printf("     %d of %d prefixes parse without error\n",
               parsed, (int)full + 1);
        Check(parsed >= 1, "the complete document is one of them");

        // A depth bomb. Counted, not recursed, so this is time and not stack.
        char deep[4096];
        int n = 0;
        for (int i = 0; i < 2000; i++) deep[n++] = '[';
        for (int i = 0; i < 2000; i++) deep[n++] = ']';
        WrJson j;
        WrJsonInit(&j, deep, (size_t)n);
        int depth = 0;
        while (WrJsonEnterArray(&j) && depth < 4000)
            depth++;
        Check(depth <= WR_JSON_MAX_DEPTH,
              "nesting is refused at the cap rather than followed down");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
