// wr_update.h  --  finding out that a new version exists, and taking it.
//
// WHY THIS EXISTS
//
// Nobody running this can find out that a newer one shipped. It is an unsigned
// DLL that people unzip somewhere and forget about, so every fix made since the
// day they downloaded it is invisible to them.
//
// WHAT IT IS, EXACTLY
//
// Three buttons in the About tab, and nothing else. Each one does one thing and
// stops, and after each the state on disk is something a user can describe:
//
//   Check     one GET of the GitHub releases API. Reads a version number.
//             Writes nothing, anywhere.
//   Download  fetches the two binaries and the release's SHA256SUMS.txt, hashes
//             what arrived, and only if the digests match does anything reach
//             the disk -- as wrlines_data\update\, beside everything else this
//             program writes.
//   Install   moves the running pair aside as .old and puts the staged pair at
//             their paths. The new code runs the next time the game starts,
//             because this DLL never unloads.
//
// WHAT IT DELIBERATELY IS NOT
//
// It is not automatic, and the difference is the whole design. Nothing here
// runs on a timer, at startup, on a map change, or on any schedule at all --
// which is the same promise wr_http.h makes for the network as a whole, in
// claim 5, and this file is a caller like every other rather than an exception
// to it. There is no setting to make it automatic, because the setting is the
// feature and the feature is the thing being declined.
//
// The reason to care is not purity. An unsigned program that reaches out on its
// own and replaces its own executable is, byte for byte and behaviour for
// behaviour, what a dropper looks like -- and this project already carries a
// Wacatac verdict on both of its own files. Everything unattended is
// something that has to be argued away in a false-positive report. So none of
// it happens without a press, and the README can go on saying so.
//
// WHAT THE HASH PROVES, AND WHAT IT DOES NOT
//
// SHA256SUMS.txt comes down the same connection as the binaries, so matching a
// digest against it is not a signature and must never be shown as one. TLS to
// github.com is the actual trust anchor here. What the check buys is real but
// narrow: a truncated or corrupted transfer cannot reach your disk, a file that
// vanished between arriving and being written is noticed and named, and the
// digest goes on screen so a person who wants to compare it against the release
// page or the README can do it by eye. See wr_sha256.h.
//
// THE ONE DANGEROUS MOMENT, AND WHAT GUARDS IT
//
// Installing renames wrlines.dll aside and copies another file to its path.
// Between those two there is an instant with no DLL there, and a user whose
// antivirus eats the replacement in that instant would be left with neither
// version. That is not hypothetical for this project. So: the module directory
// is probed for writability BEFORE anything moves, the copy is re-hashed WHERE
// IT LANDED rather than where it came from, and any failure puts the .old files
// back. A press that goes wrong ends with exactly what it started with.

#ifndef WR_UPDATE_H
#define WR_UPDATE_H

#include "wr_common.h"
#include "wr_sha256.h"

// The two files a release replaces, by the name they have on disk and on the
// release page. SHA256SUMS.txt already lists both by these names.
#define WR_UPDATE_DLL   "wrlines.dll"
#define WR_UPDATE_EXE   "wrinject.exe"
#define WR_UPDATE_SUMS  "SHA256SUMS.txt"

// The suffix the outgoing pair is renamed to. Left on disk on purpose -- it is
// the undo, and it is swept on the next load rather than immediately, because
// the file that would do the sweeping is the one still mapped.
#define WR_UPDATE_OLD_SUFFIX ".old"

// A release body can be long. Enough to read what changed without turning this
// struct into something nobody wants to copy per frame.
#define WR_UPDATE_NOTES_MAX 3072

enum WrUpdateStage
{
    WR_UPDATE_IDLE = 0,     // nothing has been asked
    WR_UPDATE_CHECKING,
    WR_UPDATE_CURRENT,      // asked, and this is the newest there is
    WR_UPDATE_AVAILABLE,    // asked, and there is a newer one
    WR_UPDATE_DOWNLOADING,
    WR_UPDATE_STAGED,       // downloaded, hashed, sitting in wrlines_data\update
    WR_UPDATE_INSTALLING,
    WR_UPDATE_INSTALLED,    // in place; restart the game
    WR_UPDATE_FAILED
};

// What one release looks like once the JSON has been walked. Public because
// tests\test_update.cpp drives the parser directly against canned replies.
struct WrUpdateRelease
{
    char tag[48];                   // "v0.9.5", as GitHub gave it
    char htmlUrl[256];              // the release page, for the browser button
    char notes[WR_UPDATE_NOTES_MAX];

    char dllUrl[512];
    char exeUrl[512];
    char sumsUrl[512];
    long long dllBytes;
    long long exeBytes;

    // True only when all three assets are on the release. A release published
    // before this feature existed has the two zips and nothing else, and that
    // is not an error -- it is a release you download in a browser.
    bool loose;
};

// The whole of what the panel draws, copied out under the lock in one call so
// the UI thread never holds it.
struct WrUpdateInfo
{
    WrUpdateStage stage;
    char running[32];               // the version in this DLL
    char latest[48];                // the tag found, empty until a check lands
    char htmlUrl[256];
    char notes[WR_UPDATE_NOTES_MAX];

    char status[192];               // one line, always set once anything ran
    char detail[256];               // a second line when there is something to do

    long long bytes;                // the two binaries together, 0 if unknown
    char dllHex[WR_SHA256_HEX];     // filled once staged, for showing on screen
    char exeHex[WR_SHA256_HEX];

    bool loose;                     // the release carries the loose binaries
    bool antivirus;                 // the failure had the shape of a quarantine
};

// The three presses. Each starts a worker and returns at once; each is ignored
// while one is running or while the stage is not one it can follow.
void WrUpdateCheck(void);
void WrUpdateDownload(void);
void WrUpdateInstall(void);

bool WrUpdateBusy(void);
void WrUpdateGet(WrUpdateInfo *out);

// Deletes any <name>.old left in the module directory by a previous install.
// Called once from the init thread; the DLL that would have deleted them last
// time was the one still mapped at that path.
void WrUpdateSweepOld(void);

// ---------------------------------------------------------------------------
// The pieces, exposed for the harness
// ---------------------------------------------------------------------------

// -1 / 0 / 1 as a<b, a==b, a>b, and WR_UPDATE_VERSION_BAD when either side is
// not a plain dotted number. A leading 'v' is allowed on either.
//
// Numeric, not lexical, and that is not pedantry: 0.9.10 follows 0.9.9 and
// strcmp says the opposite. Anything with a suffix -- a release candidate, a
// build tag -- is BAD rather than guessed at, so a tag nobody anticipated can
// never be read as newer and can never start a download.
#define WR_UPDATE_VERSION_BAD (-2)
int WrUpdateCompareVersions(const char *a, const char *b);

// Walks a GitHub /releases/latest reply. False if it is not one.
bool WrUpdateParseRelease(const char *json, size_t len, WrUpdateRelease *out);

// Finds `name`'s digest in a SHA256SUMS.txt body. Lines are "<64 hex>  <name>";
// blank lines, CRLF and unknown names are all fine.
bool WrUpdateManifestLookup(const char *text, size_t len, const char *name,
                            char *hexOut, int cap);

#endif // WR_UPDATE_H
