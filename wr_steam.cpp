// wr_steam.cpp  --  see wr_steam.h for why this is allowed to call into a game
// DLL when wr_engine deliberately does not.
//
// The whole thing is a per-SteamID state machine ticked from the render thread:
//
//   WANTED -> RequestUserInformation -> poll GetMediumFriendAvatar each frame
//          -> GetImageSize + GetImageRGBA -> CreateTexture2D + SRV -> READY
//
// Nothing here blocks. The avatar for a given player simply appears a second or
// two after the run list loads, or never, and the caller draws a coloured dot
// in the meantime.

#include "wr_steam.h"
#include "wr_hook.h"
#include "wr_log.h"

#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The flat API, declared exactly as steam_api64.dll exports it
// ---------------------------------------------------------------------------
//
// Deliberately not including the Steamworks SDK headers: this needs seven
// functions and two opaque pointers, and vendoring an SDK to get them would be
// the tail wagging the dog. CSteamID is passed by value as a uint64, which is
// what the flat API does.

typedef void *(*GetInterfaceFn)(void);
typedef bool (*RequestUserInfoFn)(void *self, unsigned long long id, bool nameOnly);
typedef int (*GetAvatarFn)(void *self, unsigned long long id);
typedef const char *(*GetPersonaFn)(void *self, unsigned long long id);
typedef bool (*GetImageSizeFn)(void *self, int image, unsigned int *w, unsigned int *h);
typedef bool (*GetImageRGBAFn)(void *self, int image, unsigned char *dst, int destSize);

static void *g_friends = NULL;
static void *g_utils = NULL;
static RequestUserInfoFn g_requestUserInfo = NULL;
static GetAvatarFn g_getAvatar = NULL;
static GetPersonaFn g_getPersona = NULL;
static GetImageSizeFn g_getImageSize = NULL;
static GetImageRGBAFn g_getImageRGBA = NULL;

static bool g_tried = false;
static bool g_ready = false;
static bool g_enabled = true;
static char g_status[192] = "not started";

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

#define WR_STEAM_MAX 96
#define WR_STEAM_MAX_POLLS 600      // ~10 s at 60 fps before giving up on an id
#define WR_STEAM_PER_FRAME 2        // ids advanced per frame

enum WrSteamState
{
    ST_EMPTY = 0,
    ST_WANTED,      // queued, not yet asked for
    ST_ASKED,       // RequestUserInformation sent, polling for the image
    ST_READY,       // texture built
    ST_UNAVAILABLE, // Steam does not have it
};

struct WrSteamEntry
{
    unsigned long long id;
    int state;
    int polls;
    int size;
    ID3D11ShaderResourceView *srv;
    char persona[64];
};

static WrSteamEntry g_cache[WR_STEAM_MAX];
static int g_cacheCount = 0;
static int g_cursor = 0;

static WrSteamEntry *Find(unsigned long long id)
{
    for (int i = 0; i < g_cacheCount; i++)
        if (g_cache[i].id == id)
            return &g_cache[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

// Interface accessors are named after their version. Try the one the shipped
// DLL actually exports (v018 here) and a couple either side, so a Steamworks
// bump degrades to a log line rather than a silent loss of the feature.
static const char *kFriendsAccessors[] = {
    "SteamAPI_SteamFriends_v018",
    "SteamAPI_SteamFriends_v017",
    "SteamAPI_SteamFriends_v019",
    "SteamAPI_SteamFriends_v020",
};
static const char *kUtilsAccessors[] = {
    "SteamAPI_SteamUtils_v010",
    "SteamAPI_SteamUtils_v009",
    "SteamAPI_SteamUtils_v011",
};

static void *ResolveIface(HMODULE mod, const char **names, int count,
                          const char **which)
{
    for (int i = 0; i < count; i++)
    {
        GetInterfaceFn fn = (GetInterfaceFn)GetProcAddress(mod, names[i]);
        if (!fn)
            continue;
        void *iface = NULL;
        __try
        {
            iface = fn();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            iface = NULL;
        }
        if (iface)
        {
            if (which)
                *which = names[i];
            return iface;
        }
    }
    return NULL;
}

bool WrSteamInit(void)
{
    if (g_tried)
        return g_ready;
    g_tried = true;

    // GetModuleHandle only, never LoadLibrary -- same rule as d3d11 in
    // wr_hook.cpp. If the game has not loaded Steam, we do not want to be the
    // one that does.
    HMODULE mod = GetModuleHandleA("steam_api64.dll");
    if (!mod)
    {
        strcpy_s(g_status, sizeof(g_status), "steam_api64.dll not loaded");
        return false;
    }

    const char *fname = NULL, *uname = NULL;
    g_friends = ResolveIface(mod, kFriendsAccessors,
                             ARRAYSIZE(kFriendsAccessors), &fname);
    g_utils = ResolveIface(mod, kUtilsAccessors,
                           ARRAYSIZE(kUtilsAccessors), &uname);
    if (!g_friends || !g_utils)
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "no usable interface (friends %s, utils %s) -- version bump?",
                    g_friends ? "ok" : "missing", g_utils ? "ok" : "missing");
        WrLogf("[!] steam: %s", g_status);
        return false;
    }

    g_requestUserInfo = (RequestUserInfoFn)GetProcAddress(
        mod, "SteamAPI_ISteamFriends_RequestUserInformation");
    g_getAvatar = (GetAvatarFn)GetProcAddress(
        mod, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    g_getPersona = (GetPersonaFn)GetProcAddress(
        mod, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    g_getImageSize = (GetImageSizeFn)GetProcAddress(
        mod, "SteamAPI_ISteamUtils_GetImageSize");
    g_getImageRGBA = (GetImageRGBAFn)GetProcAddress(
        mod, "SteamAPI_ISteamUtils_GetImageRGBA");

    if (!g_requestUserInfo || !g_getAvatar || !g_getImageSize || !g_getImageRGBA)
    {
        strcpy_s(g_status, sizeof(g_status), "steam_api64.dll is missing an export");
        WrLogf("[!] steam: %s", g_status);
        return false;
    }

    g_ready = true;
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "ready (%s, %s)",
                fname, uname);
    WrLogf("steam: %s", g_status);
    return true;
}

// ---------------------------------------------------------------------------
// Avatar upload
// ---------------------------------------------------------------------------

static bool BuildTexture(WrSteamEntry *e, int handle)
{
    unsigned int w = 0, hgt = 0;
    bool got = false;
    __try
    {
        got = g_getImageSize(g_utils, handle, &w, &hgt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        got = false;
    }
    // Steam avatars are square and small; anything else means we misread it.
    if (!got || w == 0 || w != hgt || w > 256)
        return false;

    size_t bytes = (size_t)w * hgt * 4;
    unsigned char *rgba = (unsigned char *)malloc(bytes);
    if (!rgba)
        return false;

    bool ok = false;
    __try
    {
        ok = g_getImageRGBA(g_utils, handle, rgba, (int)bytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    if (!ok)
    {
        free(rgba);
        return false;
    }

    ID3D11Device *dev = WrDevice();
    if (!dev)
    {
        free(rgba);
        return false;
    }

    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = w;
    td.Height = hgt;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd;
    memset(&srd, 0, sizeof(srd));
    srd.pSysMem = rgba;
    srd.SysMemPitch = w * 4;

    ID3D11Texture2D *tex = NULL;
    HRESULT hr = dev->CreateTexture2D(&td, &srd, &tex);
    free(rgba);
    if (FAILED(hr) || !tex)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;

    hr = dev->CreateShaderResourceView(tex, &sd, &e->srv);
    tex->Release();          // the view keeps it alive
    if (FAILED(hr) || !e->srv)
    {
        e->srv = NULL;
        return false;
    }

    e->size = (int)w;
    return true;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void WrSteamWant(unsigned long long steamId)
{
    if (!steamId || !g_enabled)
        return;
    if (Find(steamId))
        return;
    if (g_cacheCount >= WR_STEAM_MAX)
        return;

    WrSteamEntry *e = &g_cache[g_cacheCount++];
    memset(e, 0, sizeof(*e));
    e->id = steamId;
    e->state = ST_WANTED;
}

void WrSteamTick(void)
{
    if (!g_enabled || !WrSteamInit())
        return;

    // Advance a couple of entries per frame. Forty RequestUserInformation calls
    // in one frame would be a visible hitch, and there is no hurry -- the answer
    // arrives asynchronously regardless.
    int worked = 0;
    for (int n = 0; n < g_cacheCount && worked < WR_STEAM_PER_FRAME; n++)
    {
        if (g_cursor >= g_cacheCount)
            g_cursor = 0;
        WrSteamEntry *e = &g_cache[g_cursor++];

        if (e->state == ST_READY || e->state == ST_UNAVAILABLE)
            continue;

        worked++;

        if (e->state == ST_WANTED)
        {
            __try
            {
                // false = we want the avatar too, not just the name.
                g_requestUserInfo(g_friends, e->id, false);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            e->state = ST_ASKED;
            continue;
        }

        // ST_ASKED: poll. Steam returns 0 while it is still fetching.
        int handle = 0;
        __try
        {
            handle = g_getAvatar(g_friends, e->id);
            if (g_getPersona && !e->persona[0])
            {
                const char *p = g_getPersona(g_friends, e->id);
                if (p && *p)
                    strcpy_s(e->persona, sizeof(e->persona), p);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            handle = 0;
        }

        if (handle > 0)
        {
            if (BuildTexture(e, handle))
            {
                e->state = ST_READY;
                continue;
            }
            e->state = ST_UNAVAILABLE;
            continue;
        }

        if (++e->polls > WR_STEAM_MAX_POLLS)
            e->state = ST_UNAVAILABLE;
    }
}

void *WrSteamAvatar(unsigned long long steamId, int *size)
{
    WrSteamEntry *e = Find(steamId);
    if (!e || e->state != ST_READY || !e->srv)
        return NULL;
    if (size)
        *size = e->size;
    return (void *)e->srv;
}

const char *WrSteamPersona(unsigned long long steamId)
{
    WrSteamEntry *e = Find(steamId);
    return (e && e->persona[0]) ? e->persona : NULL;
}

bool WrSteamAvailable(void) { return g_ready; }
const char *WrSteamStatus(void) { return g_status; }
bool WrSteamEnabled(void) { return g_enabled; }

void WrSteamSetEnabled(bool on)
{
    if (g_enabled == on)
        return;
    g_enabled = on;
    WrLogf("steam lookups %s", on ? "enabled" : "disabled");
}

int WrSteamAvatarCount(void)
{
    int n = 0;
    for (int i = 0; i < g_cacheCount; i++)
        if (g_cache[i].state == ST_READY)
            n++;
    return n;
}

int WrSteamPendingCount(void)
{
    int n = 0;
    for (int i = 0; i < g_cacheCount; i++)
        if (g_cache[i].state == ST_WANTED || g_cache[i].state == ST_ASKED)
            n++;
    return n;
}

void WrSteamShutdown(void)
{
    // Not called in normal operation -- WrLines never unloads -- but keeping the
    // release path correct costs nothing and documents the ownership.
    for (int i = 0; i < g_cacheCount; i++)
        if (g_cache[i].srv)
        {
            g_cache[i].srv->Release();
            g_cache[i].srv = NULL;
        }
    g_cacheCount = 0;
}
