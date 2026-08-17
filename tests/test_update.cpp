// test_update.cpp  --  the three pieces of the updater that have exact answers.
//
// The updater's job is to decide whether to download and replace two binaries.
// Every part of that decision is made from text somebody else sent, and the
// three functions that read it are the whole of the risk:
//
//   1. The version compare. Lexical comparison says 0.9.10 is older than
//      0.9.9, which would strand everybody on a release the moment the patch
//      number reached double figures. And a tag this cannot fully parse must
//      come back BAD rather than be guessed at, because a guess of "newer" is
//      the one answer that starts a download.
//   2. The release parser, against a real GitHub /releases/latest reply. What
//      matters as much as reading it correctly is what happens when the two
//      loose binaries are NOT on the release -- every release before this
//      feature existed is that shape, and it has to come back as "no loose
//      assets" rather than as a failure or, worse, as a set of empty URLs.
//   3. The manifest reader, against SHA256SUMS.txt exactly as
//      .github\workflows\release.yml writes it: lowercase hex, two spaces, a
//      leaf name, CRLF line endings.
//
// Nothing here touches the network. The worker threads and the file swap are
// not driven from here -- they need a release and a writable install to be
// meaningful, and the plan's verification section walks them by hand.
//
// Build:  tests\build.bat
// Run:    tests\test_update.exe

#include "wr_update.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static void Cmp(const char *a, const char *b, int want)
{
    int got = WrUpdateCompareVersions(a, b);
    char what[96];
    _snprintf_s(what, sizeof(what), _TRUNCATE, "%-12s vs %-12s", a, b);
    bool ok = got == want;
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
    {
        printf("      want %d, got %d\n", want, got);
        g_failures++;
    }
}

// A GitHub reply, trimmed to the members this reads plus a few it must step
// over. The escapes and the \r\n in `body` are what the real one carries.
static const char *kRelease =
"{"
"  \"url\": \"https://api.github.com/repos/ProtoAus/demo-line-practice-tool/releases/1\","
"  \"assets_url\": \"https://api.github.com/repos/x/y/releases/1/assets\","
"  \"id\": 123456789,"
"  \"author\": { \"login\": \"ProtoAus\", \"id\": 42, \"site_admin\": false },"
"  \"tag_name\": \"v0.9.5\","
"  \"target_commitish\": \"main\","
"  \"name\": \"v0.9.5 - the update it can find\","
"  \"draft\": false,"
"  \"prerelease\": false,"
"  \"created_at\": \"2026-08-15T10:00:00Z\","
"  \"assets\": ["
"    {"
"      \"url\": \"https://api.github.com/repos/x/y/releases/assets/1\","
"      \"id\": 1,"
"      \"name\": \"SHA256SUMS.txt\","
"      \"content_type\": \"text/plain\","
"      \"size\": 337,"
"      \"download_count\": 0,"
"      \"browser_download_url\": \"https://github.com/x/y/releases/download/v0.9.5/SHA256SUMS.txt\""
"    },"
"    {"
"      \"id\": 2,"
"      \"name\": \"demo-line-practice-tool-v0.9.5.zip\","
"      \"size\": 999999,"
"      \"browser_download_url\": \"https://github.com/x/y/releases/download/v0.9.5/demo-line-practice-tool-v0.9.5.zip\""
"    },"
"    {"
"      \"id\": 3,"
"      \"name\": \"wrlines.dll\","
"      \"size\": 1572864,"
"      \"browser_download_url\": \"https://github.com/x/y/releases/download/v0.9.5/wrlines.dll\""
"    },"
"    {"
"      \"id\": 4,"
"      \"name\": \"wrinject.exe\","
"      \"size\": 131072,"
"      \"browser_download_url\": \"https://github.com/x/y/releases/download/v0.9.5/wrinject.exe\""
"    }"
"  ],"
"  \"body\": \"### What changed\\r\\n\\r\\n- an updater that asks rather than acts\\r\\n- \\\"quoted\\\" text survives\\r\\n\","
"  \"html_url\": \"https://github.com/ProtoAus/demo-line-practice-tool/releases/tag/v0.9.5\""
"}";

// The same release as everything published before this feature existed: two
// zips and a manifest, no loose binaries.
static const char *kOldShape =
"{"
"  \"tag_name\": \"v0.9.4\","
"  \"html_url\": \"https://github.com/ProtoAus/demo-line-practice-tool/releases/tag/v0.9.4\","
"  \"assets\": ["
"    { \"name\": \"demo-line-practice-tool-v0.9.4.zip\", \"size\": 900000,"
"      \"browser_download_url\": \"https://example.invalid/a.zip\" },"
"    { \"name\": \"demo-line-practice-tool-v0.9.4-symbols.zip\", \"size\": 900000,"
"      \"browser_download_url\": \"https://example.invalid/b.zip\" },"
"    { \"name\": \"SHA256SUMS.txt\", \"size\": 200,"
"      \"browser_download_url\": \"https://example.invalid/SHA256SUMS.txt\" }"
"  ],"
"  \"body\": \"\""
"}";

// v0.9.4's own manifest, in the shape release.yml writes: lowercase, two
// spaces, leaf name. Its symbols line is kept deliberately -- that asset is no
// longer published, but a manifest naming files the updater does not download
// is the normal case, and it is what makes the lookup below a real test.
static const char *kSums =
"7f320f0269a8bab05e36591a107e2e9a8590b61ca353634e503966aa461eea06  demo-line-practice-tool-v0.9.4-symbols.zip\r\n"
"2f91d238a50ddbea4142ad0a28eca0997b74deec405e6d7f7a19c90e99e5c1fa  demo-line-practice-tool-v0.9.4.zip\r\n"
"2cde991d46eebcf1415da03bb6845e036902d1fbfedbce207787038d1f5d442c  wrinject.exe\r\n"
"2102aa16545ac5a91839cac10c817aaf4c4646fbe8c65365f213a9d2d2df86d1  wrlines.dll\r\n";

int main(void)
{
    printf("\n=== version compare ===\n");

    Cmp("v0.9.5", "0.9.4", 1);
    Cmp("0.9.4", "0.9.4", 0);
    Cmp("v0.9.3", "0.9.4", -1);
    Cmp("v0.10.0", "0.9.4", 1);
    Cmp("v1.0.0", "0.9.9", 1);

    // The one strcmp gets backwards, and the reason this is not a strcmp.
    Cmp("v0.9.10", "0.9.9", 1);
    Cmp("v0.9.9", "0.9.10", -1);

    // Short forms. A missing component is zero, so 1.0 == 1.0.0.
    Cmp("v1.0", "1.0.0", 0);
    Cmp("v1", "1.0.0", 0);
    Cmp("v1.1", "1.0.9", 1);

    printf("\n=== a tag it cannot read is never newer ===\n");

    Cmp("v1.0.0-rc1", "0.9.4", WR_UPDATE_VERSION_BAD);
    Cmp("nightly", "0.9.4", WR_UPDATE_VERSION_BAD);
    Cmp("v", "0.9.4", WR_UPDATE_VERSION_BAD);
    Cmp("", "0.9.4", WR_UPDATE_VERSION_BAD);
    Cmp("v1.2.3.4", "0.9.4", WR_UPDATE_VERSION_BAD);
    Cmp("v1..3", "0.9.4", WR_UPDATE_VERSION_BAD);
    Cmp("v1.2 beta", "0.9.4", WR_UPDATE_VERSION_BAD);
    Check(WrUpdateCompareVersions(NULL, "0.9.4") == WR_UPDATE_VERSION_BAD,
          "NULL is BAD, not a crash");

    printf("\n=== the release parser ===\n");

    {
        WrUpdateRelease r;
        Check(WrUpdateParseRelease(kRelease, strlen(kRelease), &r),
              "a real /releases/latest reply parses");
        Check(strcmp(r.tag, "v0.9.5") == 0, "tag_name");
        Check(strcmp(r.htmlUrl,
                     "https://github.com/ProtoAus/demo-line-practice-tool"
                     "/releases/tag/v0.9.5") == 0, "html_url");
        Check(strcmp(r.dllUrl, "https://github.com/x/y/releases/download/"
                               "v0.9.5/wrlines.dll") == 0, "wrlines.dll url");
        Check(strcmp(r.exeUrl, "https://github.com/x/y/releases/download/"
                               "v0.9.5/wrinject.exe") == 0, "wrinject.exe url");
        Check(strcmp(r.sumsUrl, "https://github.com/x/y/releases/download/"
                                "v0.9.5/SHA256SUMS.txt") == 0,
              "SHA256SUMS.txt url");
        Check(r.dllBytes == 1572864 && r.exeBytes == 131072, "asset sizes");
        Check(r.loose, "all three assets present");
        Check(strstr(r.notes, "an updater that asks rather than acts") != NULL,
              "the notes came through");
        Check(strstr(r.notes, "\"quoted\"") != NULL,
              "and their escapes were decoded");
        Check(strstr(r.notes, "\r\n") != NULL, "and their line breaks");
    }

    {
        WrUpdateRelease r;
        Check(WrUpdateParseRelease(kOldShape, strlen(kOldShape), &r),
              "a release with no loose binaries still parses");
        Check(strcmp(r.tag, "v0.9.4") == 0, "and its tag is read");
        Check(!r.loose, "but it is marked as not downloadable");
        Check(r.dllUrl[0] == 0 && r.exeUrl[0] == 0,
              "and no URL was invented for the missing files");
    }

    {
        // GitHub answers 404 with a JSON object that has no tag_name in it.
        const char *notFound = "{\"message\":\"Not Found\","
                               "\"documentation_url\":\"https://docs.github.com\"}";
        WrUpdateRelease r;
        Check(!WrUpdateParseRelease(notFound, strlen(notFound), &r),
              "a 404 body is not a release");

        Check(!WrUpdateParseRelease("[]", 2, &r), "an array is not a release");
        Check(!WrUpdateParseRelease("not json at all", 15, &r),
              "rubbish is not a release");

        // Every prefix of the real reply must either parse or fail, and never
        // read past the end. The same blunt instrument test_json.cpp uses.
        int crashes = 0;
        size_t full = strlen(kRelease);
        for (size_t n = 0; n < full; n++)
        {
            WrUpdateRelease t;
            if (WrUpdateParseRelease(kRelease, n, &t) && !t.tag[0])
                crashes++;
        }
        Check(crashes == 0, "every truncation of it is refused or complete");
    }

    printf("\n=== the manifest ===\n");

    {
        char hex[WR_SHA256_HEX];
        size_t n = strlen(kSums);

        Check(WrUpdateManifestLookup(kSums, n, "wrlines.dll", hex,
                                     sizeof(hex)) &&
              strcmp(hex, "2102aa16545ac5a91839cac10c817aaf4c4646fbe8c65365"
                          "f213a9d2d2df86d1") == 0, "wrlines.dll");

        Check(WrUpdateManifestLookup(kSums, n, "wrinject.exe", hex,
                                     sizeof(hex)) &&
              strcmp(hex, "2cde991d46eebcf1415da03bb6845e036902d1fbfedbce20"
                          "7787038d1f5d442c") == 0, "wrinject.exe");

        // The two zip names share a prefix with each other. A reader matching
        // on prefix would hand back the wrong digest for one of them.
        Check(WrUpdateManifestLookup(kSums, n,
                                     "demo-line-practice-tool-v0.9.4.zip", hex,
                                     sizeof(hex)) &&
              strcmp(hex, "2f91d238a50ddbea4142ad0a28eca0997b74deec405e6d7f"
                          "7a19c90e99e5c1fa") == 0,
              "a name that is a prefix of another");

        Check(!WrUpdateManifestLookup(kSums, n, "wrlines.pdb", hex,
                                      sizeof(hex)), "a name that is not there");
        Check(hex[0] == 0, "and nothing was written for it");

        Check(!WrUpdateManifestLookup(kSums, n, "wrlines.dl", hex, sizeof(hex)),
              "a name that is a prefix of one that is");

        // LF-only, uppercase, a blank line, a comment and the * that some
        // sha256sum output puts before a binary name. None of these are what
        // the workflow writes; none of them should break it either.
        const char *messy =
            "\n"
            "# a comment nobody asked for\n"
            "2102AA16545AC5A91839CAC10C817AAF4C4646FBE8C65365F213A9D2D2DF86D1 *wrlines.dll\n"
            "\n";
        Check(WrUpdateManifestLookup(messy, strlen(messy), "wrlines.dll", hex,
                                     sizeof(hex)),
              "uppercase, a '*', blank lines and a comment");
        Check(strcmp(hex, "2102aa16545ac5a91839cac10c817aaf4c4646fbe8c65365"
                          "f213a9d2d2df86d1") == 0,
              "and the digest comes back lowercased");

        // A line whose hash field is the wrong length is not a hash.
        const char *shortHash = "2102aa16  wrlines.dll\n";
        Check(!WrUpdateManifestLookup(shortHash, strlen(shortHash),
                                      "wrlines.dll", hex, sizeof(hex)),
              "a 8-character \"digest\" is not one");

        Check(!WrUpdateManifestLookup("", 0, "wrlines.dll", hex, sizeof(hex)),
              "an empty manifest");
    }

    printf("\n%s  (%d failure%s)\n\n", g_failures ? "FAILED" : "all ok",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
