// wr_http.cpp  --  see wr_http.h. One GET, and nothing else.

#include "wr_http.h"

#include <winhttp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Fail(char *err, int errCap, const char *fmt, ...)
{
    if (!err || errCap <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(err, (size_t)errCap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

// The URLs are ASCII and so is the user agent, but WinHTTP is a wide-only API,
// so both have to be widened. CP_UTF8 rather than CP_ACP: if a URL ever does
// carry a non-ASCII byte -- a downloadURL from the server is the only way that
// could happen -- the right reading of it is UTF-8, and whichever code page the
// machine happens to be set to is not.
static bool Widen(const char *s, wchar_t *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    return n > 0;
}

bool WrHttpGet(const char *url, const char *userAgent,
               unsigned char **out, size_t *lenOut, int *status,
               char *err, int errCap)
{
    if (out)    *out = NULL;
    if (lenOut) *lenOut = 0;
    if (status) *status = 0;

    if (!url || !*url || !out || !lenOut)
    {
        Fail(err, errCap, "no URL");
        return false;
    }

    // Everything the cleanup path touches is declared before the first goto,
    // which C++ requires -- a jump may not skip an initialisation.
    bool ok = false;
    unsigned char *buf = NULL;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    size_t cap = 64 * 1024;
    size_t used = 0;

    wchar_t wurl[4096];
    wchar_t wua[256];
    if (!Widen(url, wurl, 4096) ||
        !Widen(userAgent && *userAgent ? userAgent : "WrLines", wua, 256))
    {
        Fail(err, errCap, "URL too long");
        return false;
    }

    // Split it here rather than hand the whole thing to WinHTTP, because
    // WinHttpConnect wants a bare host and WinHttpOpenRequest wants the path.
    // Doing it with the OS's own parser rather than by hunting for slashes is
    // also the difference between rejecting a scheme we did not expect and
    // accidentally accepting it.
    wchar_t host[256] = {0};
    wchar_t path[4096] = {0};
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;      uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;       uc.dwUrlPathLength = 4096;
    // lpszExtraInfo is deliberately left null. With no buffer for it,
    // WinHttpCrackUrl leaves the query string on the end of the path -- and
    // every URL here has one, so that is exactly the string WinHttpOpenRequest
    // wants. Asking for it separately would mean gluing it back on by hand.
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
    {
        Fail(err, errCap, "not a URL: %s", url);
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP)
    {
        Fail(err, errCap, "not an http URL: %s", url);
        return false;
    }

    session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        Fail(err, errCap, "could not start WinHTTP (error %lu)", GetLastError());
        return false;
    }

    {
        const int ms = WR_HTTP_TIMEOUT_SECONDS * 1000;
        WinHttpSetTimeouts(session, ms, ms, ms, ms);
    }

    conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn)
    {
        Fail(err, errCap, "could not reach %S (error %lu)", host, GetLastError());
        goto done;
    }

    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS
                                 ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
    {
        Fail(err, errCap, "could not build the request (error %lu)", GetLastError());
        goto done;
    }

    // Belt and braces, and mostly a statement rather than a fix: the session is
    // opened and closed inside this function, so there is no cookie jar to
    // carry anything between requests and no credential to reuse. Saying so to
    // WinHTTP as well means it stays true if that ever stops being.
    //
    // Redirects are NOT disabled, because urllib follows them and this has to
    // agree with urllib. WinHTTP's default policy refuses an https -> http
    // downgrade, which is the behaviour to want and the reason not to touch it.
    {
        DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
    }

    // No extra headers at all. The user agent went in at WinHttpOpen and is the
    // only thing this sends that it did not have to.
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        Fail(err, errCap, "could not send to %S (error %lu)", host, GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL))
    {
        Fail(err, errCap, "no reply from %S (error %lu)", host, GetLastError());
        goto done;
    }

    {
        DWORD code = 0, size = sizeof(code);
        if (WinHttpQueryHeaders(req,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                                WINHTTP_NO_HEADER_INDEX))
        {
            if (status)
                *status = (int)code;
            if (code < 200 || code > 299)
            {
                // The wording the reference prints for an HTTPError, so a board
                // that 404s reads the same in the panel as it did before.
                Fail(err, errCap, "HTTP %d", (int)code);
                goto done;
            }
        }
    }

    // Content-Length only to size the first allocation. It is somebody else's
    // number, so it is capped like any other and the read loop does not trust
    // it to be right -- a reply longer than advertised grows the buffer, and a
    // shorter one just stops.
    {
        wchar_t lenText[32] = {0};
        DWORD size = sizeof(lenText);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, lenText, &size,
                                WINHTTP_NO_HEADER_INDEX))
        {
            unsigned long long advertised = _wcstoui64(lenText, NULL, 10);
            if (advertised > 0 && advertised <= WR_HTTP_MAX_BODY)
                cap = (size_t)advertised + 1;
        }
    }

    buf = (unsigned char *)malloc(cap);
    if (!buf)
    {
        Fail(err, errCap, "out of memory for the reply");
        goto done;
    }

    for (;;)
    {
        if (used + 1 >= cap)
        {
            size_t grown = cap * 2;
            if (grown > (size_t)WR_HTTP_MAX_BODY + 1)
                grown = (size_t)WR_HTTP_MAX_BODY + 1;
            if (grown <= cap)
            {
                Fail(err, errCap, "reply over the %u byte limit", WR_HTTP_MAX_BODY);
                goto done;
            }
            unsigned char *bigger = (unsigned char *)realloc(buf, grown);
            if (!bigger)
            {
                Fail(err, errCap, "out of memory for a %llu byte reply",
                     (unsigned long long)grown);
                goto done;
            }
            buf = bigger;
            cap = grown;
        }

        DWORD got = 0;
        // cap - used - 1 keeps room for the terminator, so the buffer is never
        // filled right to its end and the NUL below always has somewhere to go.
        if (!WinHttpReadData(req, buf + used, (DWORD)(cap - used - 1), &got))
        {
            Fail(err, errCap, "the reply stopped early (error %lu)", GetLastError());
            goto done;
        }
        if (got == 0)
            break;
        used += got;
    }

    buf[used] = '\0';
    *out = buf;
    *lenOut = used;
    buf = NULL;             // handed over
    ok = true;

done:
    free(buf);
    if (req)     WinHttpCloseHandle(req);
    if (conn)    WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return ok;
}
