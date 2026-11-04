// Synchronization.
//
// Handle-based objects (NtCreateEvent, NtCreateSemaphore) are real Windows objects.
// Dispatcher objects that live in Xbox memory (KEVENT, KTIMER, KSEMAPHORE, critical sections) keep their
// state in the Xbox structure (SignalState is the truth, game code may read or write it directly); a host
// event per object address is only used to wake waiters.
#include "kernel/kexports.h"
#include "kernel/nt.h"
#include "xthread.h"

#include <algorithm>
#include <intrin.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

CRITICAL_SECTION g_disp; // protects SignalState transitions and the tables below
std::unordered_map<const void *, HANDLE> g_wake;     // manual-reset wake events for dispatcher objects
std::unordered_map<const void *, uint64_t> g_gen;    // signal count per object: a notification-type signal
                                                     // releases every thread waiting at that moment
std::unordered_map<const void *, HANDLE> g_cs_event; // auto-reset events for critical sections
struct InitDisp {
    InitDisp() { InitializeCriticalSection(&g_disp); }
} g_init_disp;
// Event handles handed to kernel-mode code as KEVENTs (ObReferenceObjectByHandle with ExEventObjectType: XOnline signals
// a task's event with KeSetEvent): a KEVENT standing for each host event, set / waited through the handle
std::unordered_map<HANDLE, uint32_t> g_event_type;   // NtCreateEvent's EventType per handle
std::unordered_map<HANDLE, XKEVENT *> g_event_proxy;
std::unordered_map<const void *, HANDLE> g_proxy_handle;

HANDLE wake_event(const void *obj) // caller holds g_disp
{
    auto it = g_wake.find(obj);
    if (it != g_wake.end()) return it->second;
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake[obj] = h;
    return h;
}

HANDLE cs_event(const void *cs)
{
    EnterCriticalSection(&g_disp);
    auto it = g_cs_event.find(cs);
    HANDLE h;
    if (it != g_cs_event.end()) {
        h = it->second;
    } else {
        h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_cs_event[cs] = h;
    }
    LeaveCriticalSection(&g_disp);
    return h;
}

// Try to satisfy a wait on obj; caller holds g_disp.
bool try_consume(XDISPATCHER_HEADER *h)
{
    switch (h->Type) {
    case XOBJ_EVENT_NOTIFICATION:
    case XOBJ_TIMER_NOTIFICATION:
        return h->SignalState > 0;
    case XOBJ_EVENT_SYNCHRONIZATION:
    case XOBJ_TIMER_SYNCHRONIZATION:
        if (h->SignalState > 0) {
            h->SignalState = 0;
            return true;
        }
        return false;
    case XOBJ_SEMAPHORE:
    case XOBJ_MUTANT:
        if (h->SignalState > 0) {
            h->SignalState--;
            return true;
        }
        return false;
    default:
        return h->SignalState > 0;
    }
}

std::string oa_name(const XOBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName) return {};
    std::string n = xstr(oa->ObjectName);
    for (char &c : n)
        if (c == '\\') c = '_';
    return n.empty() ? n : "xbox_" + n;
}

NTSTATUS wait_result(DWORD r)
{
    switch (r) {
    case WAIT_OBJECT_0: return XSTATUS_SUCCESS;
    case WAIT_TIMEOUT: return XSTATUS_TIMEOUT;
    case WAIT_IO_COMPLETION: return XSTATUS_USER_APC;
    case WAIT_ABANDONED: return 0x80;
    default: return XSTATUS_INVALID_HANDLE;
    }
}

} // namespace

// The KEVENT for an event handle (ObReferenceObjectByHandle), made on first use
void *ksync_event_object(HANDLE handle)
{
    EnterCriticalSection(&g_disp);
    auto it = g_event_proxy.find(handle);
    XKEVENT *e = it != g_event_proxy.end() ? it->second : nullptr;
    if (!e) {
        auto t = g_event_type.find(handle);
        if (t != g_event_type.end()) {
            e = new XKEVENT();
            e->Header.Type = (uint8_t)(t->second == 0 ? XOBJ_EVENT_NOTIFICATION : XOBJ_EVENT_SYNCHRONIZATION);
            e->Header.Size = sizeof(XKEVENT) / 4;
            e->Header.WaitListHead.Flink = e->Header.WaitListHead.Blink = &e->Header.WaitListHead;
            g_event_proxy[handle] = e;
            g_proxy_handle[e] = handle;
        }
    }
    LeaveCriticalSection(&g_disp);
    return e;
}

void ksync_handle_closed(HANDLE handle)
{
    EnterCriticalSection(&g_disp);
    g_event_type.erase(handle);
    auto it = g_event_proxy.find(handle);
    if (it != g_event_proxy.end()) { // the KEVENT stays allocated: kernel-mode code may still hold it
        g_proxy_handle.erase(it->second);
        g_event_proxy.erase(it);
    }
    LeaveCriticalSection(&g_disp);
}

HANDLE proxy_handle(const void *object)
{
    EnterCriticalSection(&g_disp);
    auto it = g_proxy_handle.find(object);
    HANDLE h = it != g_proxy_handle.end() ? it->second : nullptr;
    LeaveCriticalSection(&g_disp);
    return h;
}

NTSTATUS ksync_wait_object(void *object, bool alertable, const LARGE_INTEGER *timeout)
{
    auto *h = (XDISPATCHER_HEADER *)object;
    if (HANDLE ph = proxy_handle(object)) return wait_result(WaitForSingleObjectEx(ph, ktimeout_ms(timeout), alertable));
    if (h->Type == XOBJ_THREAD) {
        if (XThread *t = xthread_from_kthread(object))
            return wait_result(WaitForSingleObjectEx(t->host_handle, ktimeout_ms(timeout), alertable));
    }
    DWORD ms = ktimeout_ms(timeout);
    // the deadline on a 1 ms clock: on GetTickCount64 (~15.6 ms steps) a 1-2 ms timeout ran until the next step.
    // The game's and XAPI's own KeWaitForSingleObject calls all wait without a timeout (docs/07 #6).
    uint64_t deadline = ms == INFINITE ? 0 : kclock_ms() + ms;
    bool notification = h->Type == XOBJ_EVENT_NOTIFICATION || h->Type == XOBJ_TIMER_NOTIFICATION;
    EnterCriticalSection(&g_disp);
    uint64_t gen0 = g_gen[object];
    LeaveCriticalSection(&g_disp);
    for (;;) {
        EnterCriticalSection(&g_disp);
        if (try_consume(h) || (notification && g_gen[object] != gen0)) {
            LeaveCriticalSection(&g_disp);
            return XSTATUS_SUCCESS;
        }
        HANDLE ev = wake_event(object);
        ResetEvent(ev); // object is not signaled: any earlier wake-up is stale
        LeaveCriticalSection(&g_disp);
        DWORD wait_ms = INFINITE;
        if (ms != INFINITE) {
            uint64_t now = kclock_ms();
            if (now >= deadline) return XSTATUS_TIMEOUT;
            wait_ms = (DWORD)(deadline - now);
        }
        DWORD r = WaitForSingleObjectEx(ev, wait_ms, alertable);
        if (r == WAIT_IO_COMPLETION) return XSTATUS_USER_APC;
    }
}

void ksync_signal_object(void *object)
{
    auto *h = (XDISPATCHER_HEADER *)object;
    EnterCriticalSection(&g_disp);
    h->SignalState = 1;
    g_gen[object]++;
    auto it = g_wake.find(object);
    if (it != g_wake.end()) SetEvent(it->second);
    LeaveCriticalSection(&g_disp);
}

namespace xk {

NTSTATUS __stdcall NtCreateEvent(HANDLE *EventHandle, XOBJECT_ATTRIBUTES *ObjectAttributes, uint32_t EventType,
                                 BOOLEAN InitialState)
{
    std::string name = oa_name(ObjectAttributes);
    HANDLE h = CreateEventA(nullptr, EventType == 0 /* NotificationEvent = manual reset */, InitialState,
                            name.empty() ? nullptr : name.c_str());
    DWORD err = GetLastError();
    KTRACE("%p, '%s', %u, %u) -> %p (", EventHandle, name.c_str(), EventType, InitialState, h);
    if (!h) return XSTATUS_NO_MEMORY;
    *EventHandle = h;
    EnterCriticalSection(&g_disp);
    g_event_type[h] = EventType;
    LeaveCriticalSection(&g_disp);
    return err == ERROR_ALREADY_EXISTS ? 0x40000000 /* STATUS_OBJECT_NAME_EXISTS */ : XSTATUS_SUCCESS;
}

NTSTATUS __stdcall NtSetEvent(HANDLE EventHandle, int32_t *PreviousState)
{
    KTRACE("%p", EventHandle);
    LONG prev = 0;
    NTSTATUS s = host::NtSetEvent(EventHandle, &prev);
    if (PreviousState) *PreviousState = prev;
    return s;
}

NTSTATUS __stdcall NtClearEvent(HANDLE EventHandle)
{
    KTRACE("%p", EventHandle);
    return ResetEvent(EventHandle) ? XSTATUS_SUCCESS : XSTATUS_INVALID_HANDLE;
}

// Mutants are Windows mutexes (Ninja Gaiden 2004's update.xbe takes one); STATUS_MUTANT_NOT_OWNED when the caller
// does not hold it
NTSTATUS __stdcall NtCreateMutant(HANDLE *MutantHandle, XOBJECT_ATTRIBUTES *ObjectAttributes, BOOLEAN InitialOwner)
{
    std::string name = oa_name(ObjectAttributes);
    HANDLE h = CreateMutexA(nullptr, InitialOwner, name.empty() ? nullptr : name.c_str());
    DWORD err = GetLastError();
    KTRACE("%p, '%s', %u) -> %p (", MutantHandle, name.c_str(), InitialOwner, h);
    if (!h) return XSTATUS_NO_MEMORY;
    *MutantHandle = h;
    return err == ERROR_ALREADY_EXISTS ? 0x40000000 /* STATUS_OBJECT_NAME_EXISTS */ : XSTATUS_SUCCESS;
}

NTSTATUS __stdcall NtReleaseMutant(HANDLE MutantHandle, int32_t *PreviousCount)
{
    KTRACE("%p", MutantHandle);
    if (PreviousCount) *PreviousCount = 0;
    return ReleaseMutex(MutantHandle) ? XSTATUS_SUCCESS : 0xC0000046 /* STATUS_MUTANT_NOT_OWNED */;
}

NTSTATUS __stdcall NtCreateSemaphore(HANDLE *SemaphoreHandle, XOBJECT_ATTRIBUTES *ObjectAttributes, int32_t InitialCount,
                                     int32_t MaximumCount)
{
    std::string name = oa_name(ObjectAttributes);
    HANDLE h = CreateSemaphoreA(nullptr, InitialCount, MaximumCount, name.empty() ? nullptr : name.c_str());
    KTRACE("%p, '%s', %d, %d) -> %p (", SemaphoreHandle, name.c_str(), InitialCount, MaximumCount, h);
    if (!h) return XSTATUS_INVALID_PARAMETER;
    *SemaphoreHandle = h;
    return XSTATUS_SUCCESS;
}

NTSTATUS __stdcall NtReleaseSemaphore(HANDLE SemaphoreHandle, int32_t ReleaseCount, int32_t *PreviousCount)
{
    KTRACE("%p, %d", SemaphoreHandle, ReleaseCount);
    LONG prev = 0;
    NTSTATUS s = host::NtReleaseSemaphore(SemaphoreHandle, ReleaseCount, &prev);
    if (PreviousCount) *PreviousCount = prev;
    return s;
}

NTSTATUS __stdcall NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, LARGE_INTEGER *Timeout)
{
    KTRACE("%p, %u, %lld", Handle, Alertable, Timeout ? Timeout->QuadPart : -1LL);
    return host::NtWaitForSingleObject(Handle, Alertable, Timeout);
}

NTSTATUS __stdcall NtWaitForSingleObjectEx(HANDLE Handle, uint8_t WaitMode, BOOLEAN Alertable, LARGE_INTEGER *Timeout)
{
    KTRACE("%p, %u, %u, %lld", Handle, WaitMode, Alertable, Timeout ? Timeout->QuadPart : -1LL);
    return host::NtWaitForSingleObject(Handle, Alertable, Timeout);
}

// Ninja Gaiden Black's bundled NES / SNES programs (NinjaTomoe.xbe, NinjaTrilogy.xbe) import it; Xbox WaitAll = 0 and
// WaitAny = 1 are the NT values.
NTSTATUS __stdcall NtWaitForMultipleObjectsEx(uint32_t Count, HANDLE *Handles, uint32_t WaitType, uint8_t WaitMode, BOOLEAN Alertable,
                                             LARGE_INTEGER *Timeout)
{
    KTRACE("%u, %p, %u, %u, %u, %lld", Count, Handles, WaitType, WaitMode, Alertable, Timeout ? Timeout->QuadPart : -1LL);
    return host::NtWaitForMultipleObjects(Count, Handles, (int)WaitType, Alertable, Timeout);
}

int32_t __stdcall KeSetEvent(XKEVENT *Event, int32_t Increment, BOOLEAN Wait)
{
    KTRACE("%p, %d, %u", Event, Increment, Wait);
    EnterCriticalSection(&g_disp);
    int32_t prev = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    g_gen[Event]++;
    auto it = g_wake.find(Event);
    if (it != g_wake.end()) SetEvent(it->second);
    auto ph = g_proxy_handle.find(Event);
    if (ph != g_proxy_handle.end()) SetEvent(ph->second); // an event handle's KEVENT: the handle's waiters
    LeaveCriticalSection(&g_disp);
    return prev;
}

// Diagnosis (press-file "waits"): total blocked time per (object, game call site) of KeWaitForSingleObject, to find
// what a slow frame waits for. The call site is the first three .text return addresses above the kernel thunk.
namespace {
struct WaitStat { void *obj; uint32_t type, site[3]; double ms; uint32_t n; };
std::mutex g_wait_mu;
std::vector<WaitStat> g_wait_stats;
}

// Diagnosis (docs/07 #6): waits with a finite timeout, by the timeout asked for (whole ms, 32 = longer): how many
// timed out and how long they actually blocked. Logged every 30 s while some come.
namespace {
struct TimedWait { uint32_t n, timed_out, site; double ms, max_ms; };
TimedWait g_timed[33];
LARGE_INTEGER g_timed_since;
}

int64_t ktimeout_req_ms(const LARGE_INTEGER *timeout)
{
    int64_t t = timeout->QuadPart;
    if (t < 0) return (-t + 9999) / 10000;
    int64_t left = t - ktime_now_100ns();
    return left <= 0 ? 0 : (left + 9999) / 10000;
}

void note_timed_wait(int64_t req_ms, double ms, bool timed_out, uint32_t site)
{
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    std::lock_guard<std::mutex> lk(g_wait_mu);
    TimedWait &w = g_timed[req_ms < 0 ? 0 : req_ms > 32 ? 32 : req_ms];
    if (!w.n) w.site = site;
    w.n++;
    w.timed_out += timed_out;
    if (timed_out) {
        w.ms += ms;
        if (ms > w.max_ms) w.max_ms = ms;
    }
    if (!g_timed_since.QuadPart) g_timed_since = now;
    if (now.QuadPart - g_timed_since.QuadPart < 30 * f.QuadPart) return;
    g_timed_since = now;
    for (int i = 0; i <= 32; i++)
        if (g_timed[i].n)
            logf("[k] timed waits asking %s%d ms: %u, %u timed out after %.2f ms on average (max %.2f), first from %08x",
                 i == 32 ? ">=" : "", i, g_timed[i].n, g_timed[i].timed_out,
                 g_timed[i].timed_out ? g_timed[i].ms / g_timed[i].timed_out : 0.0, g_timed[i].max_ms, g_timed[i].site);
    memset(g_timed, 0, sizeof g_timed);
}

void ksync_dump_waits()
{
    std::lock_guard<std::mutex> lk(g_wait_mu);
    std::vector<WaitStat> v = g_wait_stats;
    std::sort(v.begin(), v.end(), [](const WaitStat &a, const WaitStat &b) { return a.ms > b.ms; });
    logf("[k] waits (blocked time since last dump, top %zu of %zu):", v.size() < 12 ? v.size() : (size_t)12, v.size());
    for (size_t i = 0; i < v.size() && i < 12; i++)
        logf("[k]   %8.1f ms %6u waits  object %p type %u  from %08x < %08x < %08x", v[i].ms, v[i].n, v[i].obj, v[i].type, v[i].site[0],
             v[i].site[1], v[i].site[2]);
    g_wait_stats.clear();
}

NTSTATUS __stdcall KeWaitForSingleObject(void *Object, uint32_t WaitReason, uint8_t WaitMode, BOOLEAN Alertable,
                                         LARGE_INTEGER *Timeout)
{
    KTRACE("%p (type %u), %u, %u, %u, %lld", Object, Object ? ((XDISPATCHER_HEADER *)Object)->Type : 0, WaitReason, WaitMode,
           Alertable, Timeout ? Timeout->QuadPart : -1LL);
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    NTSTATUS st = ksync_wait_object(Object, Alertable != 0, Timeout);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    if (Timeout) note_timed_wait(ktimeout_req_ms(Timeout), ms, st == XSTATUS_TIMEOUT, (uint32_t)(uintptr_t)_ReturnAddress());
    if (ms > 0.5) {
        uint32_t site[3] = {};
        // up to the thread's stack top: the Insignia Setup Assistant's network threads have 4 KB stacks and waited near
        // their top, where 1 KB more ran past it (an access violation the moment it went online, r969)
        const uint32_t *sp = (const uint32_t *)_AddressOfReturnAddress();
        const uint32_t *top = (const uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
        for (int i = 0, k = 0; i < 256 && k < 3 && sp + i < top; i++) {
            uint32_t v = sp[i];
            if (v >= 0x00011000 && v < 0x0037A480) site[k++] = v;
        }
        std::lock_guard<std::mutex> lk(g_wait_mu);
        WaitStat *w = nullptr;
        for (WaitStat &s : g_wait_stats)
            if (s.obj == Object && s.site[0] == site[0] && s.site[1] == site[1]) { w = &s; break; }
        if (!w && g_wait_stats.size() < 512) {
            g_wait_stats.push_back({Object, Object ? ((XDISPATCHER_HEADER *)Object)->Type : 0u, {site[0], site[1], site[2]}, 0, 0});
            w = &g_wait_stats.back();
        }
        if (w) { w->ms += ms; w->n++; }
    }
    return st;
}

// Critical sections use the Xbox layout (28 bytes): LockCount starts at -1, OwningThread is a KTHREAD*.

void __stdcall RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION *cs)
{
    KTRACE("%p", cs);
    cs->Synchronization.Type = XOBJ_EVENT_SYNCHRONIZATION;
    cs->Synchronization.Absolute = 0;
    cs->Synchronization.Size = sizeof(XKEVENT) / 4;
    cs->Synchronization.Inserted = 0;
    cs->Synchronization.SignalState = 0;
    cs->Synchronization.WaitListHead.Flink = cs->Synchronization.WaitListHead.Blink = &cs->Synchronization.WaitListHead;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = nullptr;
    EnterCriticalSection(&g_disp);
    auto it = g_cs_event.find(cs);
    if (it != g_cs_event.end()) ResetEvent(it->second);
    LeaveCriticalSection(&g_disp);
}

void __stdcall RtlEnterCriticalSection(XRTL_CRITICAL_SECTION *cs)
{
    KTRACE("%p", cs);
    void *me = xthread_kthread(xthread_current());
    if (InterlockedIncrement((volatile LONG *)&cs->LockCount) == 0) {
        cs->OwningThread = me;
        cs->RecursionCount = 1;
        return;
    }
    if (cs->OwningThread == me) {
        cs->RecursionCount++;
        return;
    }
    WaitForSingleObject(cs_event(cs), INFINITE);
    cs->OwningThread = me;
    cs->RecursionCount = 1;
}

void __stdcall RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION *cs)
{
    KTRACE("%p", cs);
    if (--cs->RecursionCount != 0) {
        InterlockedDecrement((volatile LONG *)&cs->LockCount);
        return;
    }
    cs->OwningThread = nullptr;
    if (InterlockedDecrement((volatile LONG *)&cs->LockCount) >= 0) SetEvent(cs_event(cs));
}

// nxdk programs (the Insignia Setup Assistant) make and release these themselves; waits go through KeWaitFor*
void __stdcall KeInitializeEvent(XKEVENT *Event, uint32_t Type, BOOLEAN State)
{
    KTRACE("%p, %u, %u", Event, Type, State);
    Event->Header.Type = (uint8_t)(Type == 0 ? XOBJ_EVENT_NOTIFICATION : XOBJ_EVENT_SYNCHRONIZATION);
    Event->Header.Size = sizeof(XKEVENT) / 4;
    Event->Header.SignalState = State ? 1 : 0;
    Event->Header.WaitListHead.Flink = Event->Header.WaitListHead.Blink = &Event->Header.WaitListHead;
}

void __stdcall KeInitializeSemaphore(XKSEMAPHORE *Semaphore, int32_t Count, int32_t Limit)
{
    KTRACE("%p, %d, %d", Semaphore, Count, Limit);
    Semaphore->Header.Type = XOBJ_SEMAPHORE;
    Semaphore->Header.Size = sizeof(XKSEMAPHORE) / 4;
    Semaphore->Header.SignalState = Count;
    Semaphore->Header.WaitListHead.Flink = Semaphore->Header.WaitListHead.Blink = &Semaphore->Header.WaitListHead;
    Semaphore->Limit = Limit;
}

int32_t __stdcall KeReleaseSemaphore(XKSEMAPHORE *Semaphore, int32_t Increment, int32_t Adjustment, BOOLEAN Wait)
{
    KTRACE("%p, %d, %d, %u", Semaphore, Increment, Adjustment, Wait);
    EnterCriticalSection(&g_disp);
    int32_t prev = Semaphore->Header.SignalState;
    Semaphore->Header.SignalState = prev + Adjustment;
    g_gen[Semaphore]++;
    auto it = g_wake.find(Semaphore);
    if (it != g_wake.end()) SetEvent(it->second);
    LeaveCriticalSection(&g_disp);
    return prev;
}

void *__stdcall KeGetCurrentThread() { return xthread_kthread(xthread_current()); }

BOOLEAN __stdcall RtlTryEnterCriticalSection(XRTL_CRITICAL_SECTION *cs)
{
    KTRACE("%p", cs);
    void *me = xthread_kthread(xthread_current());
    if (_InterlockedCompareExchange((volatile long *)&cs->LockCount, 0, -1) == -1) {
        cs->OwningThread = me;
        cs->RecursionCount = 1;
        return TRUE;
    }
    if (cs->OwningThread == me) {
        InterlockedIncrement((volatile LONG *)&cs->LockCount);
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

} // namespace xk
