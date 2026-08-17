// wr_http.h  --  the only file in this DLL that can reach the network.
//
// READ THIS ONE FIRST
//
// If you are checking what this DLL does with your machine, this is the file to
// open, and it is short on purpose. Until v0.6.0 the answer was "nothing": the
// import list was five Windows system DLLs, none of them a network stack, and
// dumpbin proved it in one line. That claim is gone -- WINHTTP.dll is in the
// import list now -- so it has to be replaced with something a reader can check
// just as quickly. This file is that replacement, and the claim is now:
//
//   1. It is called from HERE and from nowhere else. `WinHttp` -- the prefix
//      every function in that API carries -- appears in src\ in exactly two
//      files, this one and wr_http.cpp, which grep will confirm faster than
//      reading either of them.
//   2. There is ONE function that opens a socket, it does GET, and it has no
//      arm that does anything else. No POST, no PUT, no cookies, no
//      credentials, no redirect to a scheme it did not start with.
//   3. The URLs it is ever handed are built in src\wr_api.cpp, all of them from
//      TWO constant hosts, plus -- from v0.6.1 -- the absolute downloadURL that
//      Momentum's own leaderboard reply contained, and -- from v0.9.5 -- the
//      absolute browser_download_url that GitHub's own release reply contained.
//      Nothing else in the project constructs a URL.
//
//      The second host is api.github.com and it arrived with the updater. It is
//      asked one question, "what is the newest release", and the answer is a
//      version number that gets compared against this build's own. See
//      wr_update.h. GitHub redirects an asset download to
//      objects.githubusercontent.com, which WinHTTP follows without being asked
//      to, so that name will show up in a packet capture and is named here so
//      that seeing it is not a surprise.
//   4. It sends no identifier of any kind. No machine id, no SteamID, no
//      installation guid, no counter. The one leaderboard call that names
//      SteamIDs names your FRIENDS', because you pressed a button that says it
//      will look them up, and it is the same list the Steam client already has.
//      GitHub is sent the User-Agent below and nothing else at all.
//   5. It is never called except from a job started by a button press. Nothing
//      here runs on a timer, at startup, or on a map change.
//
//      This one did not change when the updater arrived, and that is the point
//      of the updater's design rather than an accident of it: checking for a
//      new version is a button like every other caller here, there is no
//      setting to make it automatic, and nothing consults the network because
//      time passed or because the game did something.
//
// The honest cost of the port is that "no HTTP client is linked" was a stronger
// statement than any of that, because it needed no trust at all. What replaced
// it is smaller in scope but still checkable: not "this cannot reach the
// network" but "this reaches exactly one host, from one function, when you ask".
//
// WHY WINHTTP AND NOT WININET OR A VENDORED CLIENT
//
// WinHTTP is in the OS, needs no state from the user's browser, and has no
// per-user cookie jar or credential store to accidentally attach to a request
// -- WinINet has all three, which is why it is the wrong one for a program that
// wants to be able to say it sent nothing. Vendoring curl and a TLS stack would
// be tens of thousands of lines of third-party code on the network path, which
// is the exact opposite of what the rest of this project is arranged around.
//
// Wine implements WinHTTP, so this works under Proton as the rest does. One
// Wine-shaped detail: the session asks for automatic proxy DISCOVERY, which is
// a Windows 8.1 access type that older Wine refuses outright. Being refused
// used to take down the leaderboard, the demo fetch and the updater together
// behind a single unhelpful line, so it now falls back to the configured proxy
// setting and says so in the log. Neither mode sends anything a plain GET does
// not; claim 4 is unaffected either way.
//
// WHY A NEW SESSION PER REQUEST
//
// One WinHttpOpen/WinHttpConnect/WinHttpOpenRequest per GET, torn down when the
// bytes are in hand. Keeping a session alive would reuse the TLS connection and
// save maybe eighty milliseconds -- against a self-imposed four-hundred
// millisecond pause between requests, which makes it worth nothing. What it
// would cost is a handle that outlives the call and holds whatever state
// WinHTTP decides to keep in it. Nothing persists between requests here, and
// that is easier to verify than it is to optimise.

#ifndef WR_HTTP_H
#define WR_HTTP_H

#include "wr_common.h"

// The refusal point for one response.
//
// A leaderboard page is about 100 KB. This is sized for what comes next
// instead: --fetch downloads .mtv demos through this same function, and the
// largest in this library is 48.8 MB. A reply over this is refused by name
// rather than allowed to decide how much of the game's heap it gets.
#define WR_HTTP_MAX_BODY (64u * 1024u * 1024u)

// Seconds to wait, matching the reference implementation's urlopen(timeout=30).
// Applied to each of the four WinHTTP phases separately, as WinHTTP wants them.
#define WR_HTTP_TIMEOUT_SECONDS 30

// GET a URL. The body is malloc'd and the caller free()s it.
//
// `*out` is NUL-terminated one byte past `*lenOut`, which the terminator is not
// counted in. Nothing here needs that -- WrJson takes a pointer and a length --
// but a buffer of somebody else's bytes that is not terminated is a footgun
// left lying about for the next person, and the byte is free.
//
// `status` receives the HTTP status code when a reply was received at all, and
// 0 when the request never got that far. It is set even on failure, because
// "404" and "the DNS did not resolve" are different things to tell a user. May
// be NULL.
//
// A status outside 2xx is a FAILURE here, with err set to "HTTP %d". That
// matches the reference implementation: urllib.request raises HTTPError for
// those, and the caller prints the code. Redirects are followed by WinHTTP
// before this sees a status, exactly as urllib's redirect handler does.
bool WrHttpGet(const char *url, const char *userAgent,
               unsigned char **out, size_t *lenOut, int *status,
               char *err, int errCap);

#endif // WR_HTTP_H
