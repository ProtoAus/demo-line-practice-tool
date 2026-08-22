// wr_gametimer.cpp  --  see wr_gametimer.h.
//
// This uses Source's versioned, read-only client interfaces rather than fixed
// offsets. VClient018 supplies the receive-table list and
// VClientEntityList003 supplies the live entities. The field offsets are then
// looked up by name (DT_MOM_Player::m_hPrimaryTimer and
// DT_MomentumTimerInstance::{m_state,m_dNetworkRunTime}), so an ordinary class
// layout change does not turn into a wrong clock.
//
// The three vtable indices used below are not probes. They are the documented
// methods of those exact interface versions, inherited unchanged from Source's
// public interfaces. Every returned pointer, table name, class and value is
// validated before it is used; a mismatch disables the reader.

#include "wr_gametimer.h"
#include "wr_common.h"
#include "wr_engine.h"
#include "wr_pe.h"
#include "wr_probe.h"
#include "wr_log.h"

#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef void *(*CreateInterfaceFn)(const char *name, int *retCode);

#define CLIENT_IFACE "VClient018"
#define ENTITYLIST_IFACE "VClientEntityList003"

// IBaseClientDLL::GetAllClasses in VClient017/018.
#define VT_GET_ALL_CLASSES 8

// IClientEntityList, VClientEntityList003.
#define VT_GET_NETWORKABLE 0
#define VT_GET_NETWORKABLE_FROM_HANDLE 1
#define VT_GET_HIGHEST_ENTITY 6

// IClientNetworkable.
#define VT_NETWORKABLE_GET_CLASS 2
#define VT_NETWORKABLE_GET_BASE 11

struct RawRecvTable;

// x64 layout from Source's public dt_recv.h. The installed Strata client uses
// the same 0x60-byte RecvProp (also proved by the table constructor itself).
struct RawRecvProp
{
    const char *name;               // 0x00
    int type;                       // 0x08
    int flags;                      // 0x0c
    int stringBufferSize;           // 0x10
    bool insideArray;               // 0x14
    unsigned char pad0[3];
    const void *extraData;          // 0x18
    const RawRecvProp *arrayProp;   // 0x20
    const void *arrayLengthProxy;   // 0x28
    const void *proxyFn;            // 0x30
    const void *dataTableProxyFn;   // 0x38
    const RawRecvTable *dataTable;  // 0x40
    int offset;                     // 0x48
    int elementStride;              // 0x4c
    int elements;                   // 0x50
    int pad1;
    const char *parentArrayName;    // 0x58
};

struct RawRecvTable
{
    const RawRecvProp *props;
    int propCount;
    int pad0;
    const void *decoder;
    const char *name;
    bool initialized;
    bool inMainList;
};

struct RawClientClass
{
    const void *createFn;
    const void *createEventFn;
    const char *networkName;
    const RawRecvTable *recvTable;
    const RawClientClass *next;
    int classId;
};

static_assert(sizeof(RawRecvProp) == 0x60, "unexpected RecvProp layout");
static_assert(offsetof(RawRecvProp, dataTable) == 0x40,
              "unexpected RecvProp table offset");
static_assert(offsetof(RawRecvProp, offset) == 0x48,
              "unexpected RecvProp value offset");

static HMODULE g_clientMod = NULL;
static void *g_client = NULL;
static void *g_entityList = NULL;
static const RawClientClass *g_playerClass = NULL;
static const RawClientClass *g_timerClass = NULL;
static int g_primaryTimerOff = -1;
static int g_timerStateOff = -1;
static int g_timerSecondsOff = -1;
static int g_timerRawSecondsOff = -1;
static int g_timerTickstampOff = -1;
static int g_playerOriginOff = -1;
static void **g_globalsSlot = NULL;
static int g_playerIndex = -1;
static bool g_loggedReady = false;
static char g_status[192] = "not resolved yet";

// The receive proxy normally gives us a fresh runtime snapshot plus the tick
// on which it arrived. Some local-server paths, however, refresh that tickstamp
// while leaving the snapshot untouched. Treating each such pair as a complete
// new time kept returning exactly zero. This small tracker uses the same game
// tick as Momentum to carry an unchanged snapshot forward. The instant a real
// snapshot changes (including a save-state jump), it re-bases to that value.
static bool g_clockHave = false;
static unsigned int g_clockIdentity = 0;
static int g_clockState = -1;
static int g_clockBaseTick = 0;
static int g_clockLastTick = 0;
static int g_clockLastReceivedTick = 0;
static double g_clockBaseSeconds = 0.0;
static double g_clockSeconds = 0.0;
static double g_clockLastRaw = 0.0;
static double g_clockLastPublic = 0.0;
static bool g_clockLoggedStall = false;

static bool Read(const void *src, void *dst, size_t n)
{
    return WrSafeReadBytes(src, dst, n);
}

static bool NameIs(const char *p, const char *want)
{
    char got[128];
    return WrSafeReadString(p, got, sizeof(got)) >= 0 &&
           strcmp(got, want) == 0;
}

static void *Vfn(void *object, int index)
{
    void **vt = NULL;
    void *fn = NULL;
    if (!object || index < 0 || !Read(object, &vt, sizeof(vt)) || !vt ||
        !Read(vt + index, &fn, sizeof(fn)) || !fn ||
        !WrIsCodeIn(g_clientMod, fn))
        return NULL;
    return fn;
}

static const RawClientClass *GetAllClasses(void)
{
    typedef const RawClientClass *(__fastcall *Fn)(void *);
    Fn fn = (Fn)Vfn(g_client, VT_GET_ALL_CLASSES);
    if (!fn)
        return NULL;
    __try { return fn(g_client); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static int HighestEntity(void)
{
    typedef int (__fastcall *Fn)(void *);
    Fn fn = (Fn)Vfn(g_entityList, VT_GET_HIGHEST_ENTITY);
    if (!fn)
        return -1;
    __try { return fn(g_entityList); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void *NetworkableAt(int index)
{
    typedef void *(__fastcall *Fn)(void *, int);
    Fn fn = (Fn)Vfn(g_entityList, VT_GET_NETWORKABLE);
    if (!fn)
        return NULL;
    __try { return fn(g_entityList, index); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static void *NetworkableFromHandle(unsigned int handle)
{
    typedef void *(__fastcall *Fn)(void *, unsigned int);
    Fn fn = (Fn)Vfn(g_entityList, VT_GET_NETWORKABLE_FROM_HANDLE);
    if (!fn)
        return NULL;
    __try { return fn(g_entityList, handle); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static const RawClientClass *ClassOf(void *networkable)
{
    typedef const RawClientClass *(__fastcall *Fn)(void *);
    Fn fn = (Fn)Vfn(networkable, VT_NETWORKABLE_GET_CLASS);
    if (!fn)
        return NULL;
    __try { return fn(networkable); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static void *TableBaseOf(void *networkable)
{
    typedef void *(__fastcall *Fn)(void *);
    Fn fn = (Fn)Vfn(networkable, VT_NETWORKABLE_GET_BASE);
    if (!fn)
        return NULL;
    __try { return fn(networkable); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static bool ReadTable(const RawRecvTable *at, RawRecvTable *out)
{
    if (!at || !Read(at, out, sizeof(*out)) || !out->props ||
        out->propCount < 0 || out->propCount > 1024 || !out->name)
        return false;
    char name[128];
    return WrSafeReadString(out->name, name, sizeof(name)) >= 0;
}

static const RawClientClass *FindClass(const RawClientClass *head,
                                       const char *tableName)
{
    const RawClientClass *p = head;
    for (int i = 0; p && i < 1024; i++)
    {
        RawClientClass c;
        RawRecvTable t;
        if (!Read(p, &c, sizeof(c)) || !c.recvTable ||
            !ReadTable(c.recvTable, &t))
            return NULL;
        if (NameIs(t.name, tableName))
            return p;
        p = c.next;
    }
    return NULL;
}

static int FindProp(const RawRecvTable *at, const char *propName,
                    int base, int depth, RawRecvProp *foundProp)
{
    if (depth > 12)
        return -1;
    RawRecvTable t;
    if (!ReadTable(at, &t))
        return -1;

    for (int i = 0; i < t.propCount; i++)
    {
        RawRecvProp p;
        if (!Read(t.props + i, &p, sizeof(p)) || !p.name ||
            p.offset < 0 || p.offset > 0x100000)
            continue;
        if (NameIs(p.name, propName))
        {
            if (foundProp) *foundProp = p;
            return base + p.offset;
        }
    }

    // Base classes and embedded receive tables. Search only after direct props
    // so a nested field with the same name cannot outrank the table's own one.
    for (int i = 0; i < t.propCount; i++)
    {
        RawRecvProp p;
        if (!Read(t.props + i, &p, sizeof(p)) || !p.dataTable ||
            p.offset < 0 || p.offset > 0x100000)
            continue;
        int found = FindProp(p.dataTable, propName, base + p.offset, depth + 1,
                             foundProp);
        if (found >= 0)
            return found;
    }
    return -1;
}

static int FindProp(const RawRecvTable *at, const char *propName)
{
    return FindProp(at, propName, 0, 0, NULL);
}

// Momentum's custom receive proxy preserves the raw server timer and the game
// tick on which it arrived. Its own HUD then returns
//
//   raw time + (current tick - received tick) * interval_per_tick
//
// while the timer is RUNNING. Reading only m_dNetworkRunTime happens to look
// right when the stock HUD has queried it that frame, because that query writes
// the extrapolated answer back there; without the HUD it advances only when a
// packet arrives. Resolve the proxy's two private destinations and gpGlobals
// reference from the proxy itself so this reader produces the same tick value
// independently of any panel being visible.
static bool ResolveTimerInterpolator(const RawRecvProp &timeProp)
{
    if (!timeProp.proxyFn || !WrIsCodeIn(g_clientMod, timeProp.proxyFn))
        return false;

    unsigned char code[80];
    if (!Read(timeProp.proxyFn, code, sizeof(code)))
        return false;

    int networkOff = -1, rawOff = -1, tickOff = -1;
    void **globalsSlot = NULL;
    for (int i = 0; i + 7 <= (int)sizeof(code); i++)
    {
        // movsd [rdx+disp32],xmm1 -- public/network runtime
        if (i + 8 <= (int)sizeof(code) && code[i] == 0xF2 &&
            code[i + 1] == 0x0F && code[i + 2] == 0x11 &&
            code[i + 3] == 0x8A)
            memcpy(&networkOff, code + i + 4, sizeof(networkOff));

        // movsd [rdx+disp32],xmm0 -- untouched server snapshot
        if (i + 8 <= (int)sizeof(code) && code[i] == 0xF2 &&
            code[i + 1] == 0x0F && code[i + 2] == 0x11 &&
            code[i + 3] == 0x82)
            memcpy(&rawOff, code + i + 4, sizeof(rawOff));

        // mov [rdx+disp32],eax -- receive tickstamp
        if (i + 6 <= (int)sizeof(code) && code[i] == 0x89 &&
            code[i + 1] == 0x82)
            memcpy(&tickOff, code + i + 2, sizeof(tickOff));

        // mov rax,[rip+disp32] -- the gpGlobals pointer slot. Require the
        // following code to read tickcount at +0x24, not merely any RIP load.
        if (i + 10 <= (int)sizeof(code) && code[i] == 0x48 &&
            code[i + 1] == 0x8B && code[i + 2] == 0x05 &&
            code[i + 7] == 0x8B && code[i + 8] == 0x40 &&
            code[i + 9] == 0x24)
        {
            int disp = 0;
            memcpy(&disp, code + i + 3, sizeof(disp));
            globalsSlot = (void **)((unsigned char *)timeProp.proxyFn +
                                     i + 7 + disp);
        }
    }

    if (networkOff != g_timerSecondsOff || rawOff <= networkOff ||
        rawOff - networkOff > 0x200 || tickOff <= rawOff ||
        tickOff - rawOff > 0x40 || !globalsSlot)
        return false;

    void *globals = NULL;
    int tick = 0;
    float interval = 0.0f;
    if (!Read(globalsSlot, &globals, sizeof(globals)) || !globals ||
        !Read((unsigned char *)globals + 0x24, &tick, sizeof(tick)) ||
        !Read((unsigned char *)globals + 0x28, &interval, sizeof(interval)) ||
        tick < 0 || !(interval > 0.0f) || interval > 0.1f)
        return false;

    g_timerRawSecondsOff = rawOff;
    g_timerTickstampOff = tickOff;
    g_globalsSlot = globalsSlot;
    return true;
}

static bool Resolve(void)
{
    if (g_client && g_entityList && g_playerClass && g_timerClass &&
        g_primaryTimerOff >= 0 && g_timerStateOff >= 0 &&
        g_timerSecondsOff >= 0 && g_timerRawSecondsOff >= 0 &&
        g_timerTickstampOff >= 0 && g_globalsSlot)
        return true;

    g_clientMod = GetModuleHandleA("client.dll");
    if (!g_clientMod)
    {
        strcpy_s(g_status, sizeof(g_status), "client.dll is not loaded");
        return false;
    }
    WrPeRegister(g_clientMod);

    CreateInterfaceFn ci =
        (CreateInterfaceFn)GetProcAddress(g_clientMod, "CreateInterface");
    if (!ci)
    {
        strcpy_s(g_status, sizeof(g_status), "client.dll has no CreateInterface");
        return false;
    }

    g_client = ci(CLIENT_IFACE, NULL);
    g_entityList = ci(ENTITYLIST_IFACE, NULL);
    if (!g_client || !g_entityList)
    {
        strcpy_s(g_status, sizeof(g_status),
                 "Momentum's client interfaces changed; timing is unavailable");
        return false;
    }

    const RawClientClass *head = GetAllClasses();
    g_playerClass = FindClass(head, "DT_MOM_Player");
    g_timerClass = FindClass(head, "DT_MomentumTimerInstance");
    if (!g_playerClass || !g_timerClass)
    {
        strcpy_s(g_status, sizeof(g_status),
                 "Momentum's timer receive tables were not found");
        return false;
    }

    RawClientClass pc, tc;
    if (!Read(g_playerClass, &pc, sizeof(pc)) ||
        !Read(g_timerClass, &tc, sizeof(tc)))
        return false;

    RawRecvProp timeProp;
    memset(&timeProp, 0, sizeof(timeProp));
    g_primaryTimerOff = FindProp(pc.recvTable, "m_hPrimaryTimer");
    g_playerOriginOff = FindProp(pc.recvTable, "m_vecOrigin");
    g_timerStateOff = FindProp(tc.recvTable, "m_state");
    g_timerSecondsOff = FindProp(tc.recvTable, "m_dNetworkRunTime", 0, 0,
                                 &timeProp);
    if (g_primaryTimerOff < 0 || g_timerStateOff < 0 ||
        g_timerSecondsOff < 0 || !ResolveTimerInterpolator(timeProp))
    {
        strcpy_s(g_status, sizeof(g_status),
                 "Momentum's timer layout changed; timing is unavailable");
        return false;
    }

    if (!g_loggedReady)
    {
        WrLogf("game timer: exact fields resolved -- player +0x%x, state "
               "+0x%x, time +0x%x, raw +0x%x, tick +0x%x",
               g_primaryTimerOff, g_timerStateOff, g_timerSecondsOff,
               g_timerRawSecondsOff, g_timerTickstampOff);
        g_loggedReady = true;
    }
    return true;
}

static bool TimerForPlayer(void *playerNetworkable, unsigned int *handleOut,
                           void **timerBaseOut)
{
    if (!playerNetworkable || ClassOf(playerNetworkable) != g_playerClass)
        return false;
    void *playerBase = TableBaseOf(playerNetworkable);
    unsigned int handle = 0xFFFFFFFFu;
    if (!playerBase ||
        !Read((unsigned char *)playerBase + g_primaryTimerOff,
              &handle, sizeof(handle)) || handle == 0xFFFFFFFFu)
        return false;

    void *timerNet = NetworkableFromHandle(handle);
    if (!timerNet || ClassOf(timerNet) != g_timerClass)
        return false;
    void *timerBase = TableBaseOf(timerNet);
    if (!timerBase)
        return false;
    if (handleOut) *handleOut = handle;
    if (timerBaseOut) *timerBaseOut = timerBase;
    return true;
}

static bool ChoosePlayer(void)
{
    int highest = HighestEntity();
    if (highest < 0 || highest > 32768)
    {
        strcpy_s(g_status, sizeof(g_status), "Momentum's entity list is unavailable");
        return false;
    }

    Vec3 cam;
    bool haveCam = WrCameraOrigin(&cam);
    int best = -1;
    float bestDist = 1.0e30f;

    for (int i = 0; i <= highest; i++)
    {
        void *net = NetworkableAt(i);
        unsigned int handle;
        void *timerBase;
        if (!TimerForPlayer(net, &handle, &timerBase))
            continue;

        float score = 0.0f;
        if (haveCam && g_playerOriginOff >= 0)
        {
            void *base = TableBaseOf(net);
            Vec3 pos;
            if (!base || !Read((unsigned char *)base + g_playerOriginOff,
                               &pos, sizeof(pos)) || !WrSaneVec(pos))
                continue;
            float dx = pos.x - cam.x, dy = pos.y - cam.y;
            float dz = cam.z - pos.z;
            // The local player is directly under the rendered camera. Remote
            // players and ghosts are not allowed to win merely by having a
            // valid timer entity of their own.
            if (dz < -16.0f || dz > 128.0f)
                continue;
            score = dx * dx + dy * dy + dz * dz * 0.01f;
        }
        if (best < 0 || score < bestDist)
        {
            best = i;
            bestDist = score;
        }
    }

    g_playerIndex = best;
    if (best < 0)
    {
        strcpy_s(g_status, sizeof(g_status),
                 "waiting for the local player and Momentum timer");
        return false;
    }
    return true;
}

// A timer handle remaining valid does not prove it still belongs to the player
// being viewed. Ghost/replay players use the same network class and can be the
// nearest candidate at a start pad. Recheck the latched player against the
// camera so it cannot leave us following a zeroed ghost timer for the run.
static bool PlayerStillObserved(void *networkable)
{
    if (!networkable || g_playerOriginOff < 0)
        return false;
    Vec3 cam, pos;
    void *base = TableBaseOf(networkable);
    if (!base || !WrCameraOrigin(&cam) ||
        !Read((unsigned char *)base + g_playerOriginOff, &pos, sizeof(pos)) ||
        !WrSaneVec(pos))
        return false;

    float dx = pos.x - cam.x, dy = pos.y - cam.y;
    float dz = cam.z - pos.z;
    return dx * dx + dy * dy <= 384.0f * 384.0f &&
           dz >= -32.0f && dz <= 192.0f;
}

static double AdvanceClock(unsigned int identity, int state, int tick,
                           int receivedTick, float interval, double snapshot,
                           double rawSeconds, double publicSeconds)
{
    const bool replaced = !g_clockHave || identity != g_clockIdentity;
    const bool stateChanged = !g_clockHave || state != g_clockState;
    const bool tickRewound = g_clockHave && tick < g_clockLastTick;

    if (replaced || stateChanged || tickRewound)
    {
        // A finished transition can arrive with a zero/stale snapshot on the
        // same local-server path. The last running tick is a better lower bound
        // than turning a completed run back into 0.000.
        if (!replaced && g_clockState == 2 && state == 3 &&
            snapshot + interval * 2.0 < g_clockSeconds)
            snapshot = g_clockSeconds;

        g_clockHave = true;
        g_clockIdentity = identity;
        g_clockState = state;
        g_clockBaseTick = tick;
        g_clockLastTick = tick;
        g_clockLastReceivedTick = receivedTick;
        g_clockBaseSeconds = snapshot;
        g_clockSeconds = snapshot;
        g_clockLastRaw = rawSeconds;
        g_clockLastPublic = publicSeconds;
        g_clockLoggedStall = false;

        WrLogf("game timer: entity %d handle %08x state %d, %.3fs at tick %d",
               g_playerIndex, identity, state, snapshot, tick);
        return snapshot;
    }

    if (state == 2)
    {
        // Rebase only when the underlying server/public value changed. The
        // extrapolated `snapshot` also changes with every tick, and a receive
        // tickstamp may be refreshed while a broken snapshot stays at zero;
        // neither is a new authoritative time. A real raw change is accepted
        // however large or in which direction, because save states can jump
        // forwards as well as backwards.
        const double epsilon = interval * 0.05;
        const bool sourceChanged =
            fabs(rawSeconds - g_clockLastRaw) > epsilon ||
            fabs(publicSeconds - g_clockLastPublic) > epsilon;
        if (sourceChanged)
        {
            g_clockBaseSeconds = snapshot;
            g_clockBaseTick = tick;
            g_clockSeconds = snapshot;
        }
        else
        {
            g_clockSeconds = g_clockBaseSeconds +
                (double)(tick - g_clockBaseTick) * interval;
            if (!g_clockLoggedStall &&
                receivedTick != g_clockLastReceivedTick &&
                tick > g_clockBaseTick)
            {
                WrLogf("game timer: runtime snapshot held at %.3fs; carrying it "
                       "with Momentum game ticks", snapshot);
                g_clockLoggedStall = true;
            }
        }
    }
    else if (state == 0 || state == 1)
    {
        // Disabled/primed genuinely owns zero and starts the next run's base.
        g_clockBaseSeconds = snapshot;
        g_clockBaseTick = tick;
        g_clockSeconds = snapshot;
    }
    else if (state == 3 && snapshot > g_clockSeconds)
    {
        g_clockSeconds = snapshot;
    }

    g_clockState = state;
    g_clockLastTick = tick;
    g_clockLastReceivedTick = receivedTick;
    g_clockLastRaw = rawSeconds;
    g_clockLastPublic = publicSeconds;
    return g_clockSeconds;
}

bool WrGameTimerRead(WrGameTimerSample *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!Resolve())
        return false;

    void *player = g_playerIndex >= 0 ? NetworkableAt(g_playerIndex) : NULL;
    unsigned int handle = 0;
    void *timerBase = NULL;
    if (!TimerForPlayer(player, &handle, &timerBase) ||
        !PlayerStillObserved(player))
    {
        g_playerIndex = -1;
        if (!ChoosePlayer())
            return false;
        player = NetworkableAt(g_playerIndex);
        if (!TimerForPlayer(player, &handle, &timerBase))
            return false;
    }

    unsigned int stateRaw = 0;
    double networkSeconds = 0.0, rawSeconds = 0.0;
    int receivedTick = 0, tick = 0;
    float interval = 0.0f;
    void *globals = NULL;
    if (!Read((unsigned char *)timerBase + g_timerStateOff,
              &stateRaw, sizeof(stateRaw)) ||
        !Read((unsigned char *)timerBase + g_timerSecondsOff,
              &networkSeconds, sizeof(networkSeconds)) ||
        !Read((unsigned char *)timerBase + g_timerRawSecondsOff,
              &rawSeconds, sizeof(rawSeconds)) ||
        !Read((unsigned char *)timerBase + g_timerTickstampOff,
              &receivedTick, sizeof(receivedTick)) ||
        !Read(g_globalsSlot, &globals, sizeof(globals)) || !globals ||
        !Read((unsigned char *)globals + 0x24, &tick, sizeof(tick)) ||
        !Read((unsigned char *)globals + 0x28, &interval, sizeof(interval)))
        return false;
    int state = (int)(stateRaw & 0xFFu);
    double snapshot = networkSeconds;
    if (state == 2 && tick >= receivedTick)
    {
        // This is Momentum's own live-HUD formula. It is tick based, not a
        // frame-delta clock, and therefore introduces neither smoothing nor a
        // second independently drifting notion of elapsed time.
        snapshot = rawSeconds + (double)(tick - receivedTick) * interval;
        // The public value is the receive proxy's non-negative copy, and some
        // client paths also keep it fresher than the private snapshot. Prefer
        // it when it is materially ahead.
        if (networkSeconds > snapshot + interval * 2.0)
            snapshot = networkSeconds;
    }
    if (snapshot < 0.0 && snapshot > -1.0)
        snapshot = 0.0;
    if (state < 0 || state > 3 || snapshot != snapshot ||
        networkSeconds != networkSeconds || rawSeconds != rawSeconds ||
        snapshot < 0.0 || snapshot > 10000000.0 ||
        networkSeconds < 0.0 || networkSeconds > 10000000.0 || tick < 0 ||
        receivedTick < 0 || !(interval > 0.0f) || interval > 0.1f)
    {
        strcpy_s(g_status, sizeof(g_status),
                 "Momentum returned an invalid timer value");
        return false;
    }

    const double seconds = AdvanceClock(handle, state, tick, receivedTick,
                                        interval, snapshot, rawSeconds,
                                        networkSeconds);

    if (out)
    {
        out->state = state;
        out->seconds = seconds;
        out->identity = handle;
        out->tick = tick;
        out->tickInterval = interval;
    }
    static const char *states[] = { "disabled", "primed", "running", "finished" };
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "Momentum timer: %s, %.3fs", states[state], seconds);
    return true;
}

void WrGameTimerReset(void)
{
    g_playerIndex = -1;
    g_clockHave = false;
    g_clockIdentity = 0;
    g_clockState = -1;
    g_clockBaseTick = g_clockLastTick = g_clockLastReceivedTick = 0;
    g_clockBaseSeconds = g_clockSeconds = 0.0;
    g_clockLastRaw = g_clockLastPublic = 0.0;
    g_clockLoggedStall = false;
}

const char *WrGameTimerStatus(void) { return g_status; }
