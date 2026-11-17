// renderthread.dll: the render thread of BFME2 Accelerator (https://github.com/OhadBaehr/bfme2-accelerator, MIT
// licence, see LICENSE.accelerator), carved out to run next to the letterbox d3d9 proxy under Wine.
// Only aotr_rt.inc, aotr_rt_gen.inc and aotr_rtmirror.inc and the helpers those files use are here; the copies are made
// from the untouched originals by carve.py. What the accelerator does only on the one game.dat build it knows (its
// engine hooks) stays inert on any other build by its own byte checks.
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <setjmp.h>

// ---- structured exception handling, which this compiler does not have for 32-bit code -------------------------
// The accelerator guards a few memory probes with __try / __except (EXCEPTION_EXECUTE_HANDLER). Here a guarded block
// records where it is (setjmp) in a per-thread chain; a vectored handler sends a fault inside such a block to the
// block's handler by restoring that record in the exception's context. Leaving the block any way (also by return)
// takes the record off again.
struct RtGuard { jmp_buf jb; RtGuard* prev; };
static DWORD g_rtGuardTls = TLS_OUT_OF_INDEXES;               // a TLS slot below 64: read straight from the thread block
static __forceinline RtGuard* rtGuardCur() { return (RtGuard*)(ULONG_PTR)__readfsdword(0xE10 + 4 * g_rtGuardTls); }
static __forceinline void rtGuardSet(RtGuard* g) { __writefsdword(0xE10 + 4 * g_rtGuardTls, (DWORD)(ULONG_PTR)g); }
struct RtGuardScope {
    RtGuard g; bool on;
    RtGuardScope() { on = g_rtGuardTls < 64; if (on) { g.prev = rtGuardCur(); rtGuardSet(&g); } }
    ~RtGuardScope() { if (on) rtGuardSet(g.prev); }
};
static LONG CALLBACK rtGuardVeh(EXCEPTION_POINTERS* ep) {
    if ((ep->ExceptionRecord->ExceptionCode >> 30) != 3 || g_rtGuardTls >= 64) return EXCEPTION_CONTINUE_SEARCH;   // errors only
    RtGuard* g = rtGuardCur();
    if (!g) return EXCEPTION_CONTINUE_SEARCH;
    const _JUMP_BUFFER* jb = (const _JUMP_BUFFER*)g->jb;
    CONTEXT* c = ep->ContextRecord;
    rtGuardSet(g->prev);
    c->Ebp = jb->Ebp; c->Ebx = jb->Ebx; c->Edi = jb->Edi; c->Esi = jb->Esi; c->Esp = jb->Esp; c->Eip = jb->Eip; c->Eax = 1;
    __writefsdword(0, jb->Registration);                      // the exception-handler chain as it was at the setjmp
    return EXCEPTION_CONTINUE_EXECUTION;
}
#define __try if (RtGuardScope rtGs_; !setjmp(rtGs_.g.jb))
#define __except(x) else

// was loaded from, so the whole thing can be dropped anywhere. Resolved once in DllMain, before any logging.
static char g_dir[MAX_PATH] = "";
static char kLogPath[MAX_PATH] = "";
static volatile LONG g_engineHooks = 0;   // may this build's absolute addresses be patched?

static volatile DWORD g_lpPanTick = 0;                                    // when the camera was last scrolled (GetTickCount; aotr_scroll.inc tells)
static void aotrSetDir(HMODULE self) {
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) lstrcpynA(g_dir, ".", MAX_PATH);
    else {
        while (n && p[n - 1] != '\\' && p[n - 1] != '/') --n;
        p[n ? n - 1 : 0] = 0;
        lstrcpynA(g_dir, p, MAX_PATH);
    }
    wsprintfA(kLogPath, "%s\\bfme2_accel.log", g_dir);
}

#ifdef AOTR_PROD
#define PSTAT(...) ((void)0)
#else
#define PSTAT(...) do { __VA_ARGS__; } while (0)
#endif

static CRITICAL_SECTION g_logCs;

static void logf(const char* fmt, ...) {
    char line[1100];                                 // wvsprintfA writes at most 1024 characters: a longer line is cut short, never past the buffer
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = wsprintfA(line, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(line + n, fmt, ap);
    va_end(ap);
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_logCs);
    HANDLE h = CreateFileA(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

#include "aotr_capture.inc"

// ---------------------------------------------------------------- inline detour (5-byte jmp rel32)
static BYTE* makeTrampoline(BYTE* target, int stolen) {
    BYTE* t = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, target, stolen);
    t[stolen] = 0xE9;
    *(DWORD*)(t + stolen + 1) = (DWORD)(ULONG_PTR)(target + stolen) - (DWORD)(ULONG_PTR)(t + stolen + 5);
    return t;
}

static BOOL patchJmp(BYTE* target, void* dest, int stolen) {
    DWORD oldProt = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProt)) return FALSE;
    target[0] = 0xE9;
    *(DWORD*)(target + 1) = (DWORD)(ULONG_PTR)dest - (DWORD)(ULONG_PTR)(target + 5);
    for (int i = 5; i < stolen; ++i) target[i] = 0x90;
    VirtualProtect(target, stolen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return TRUE;
}

// Freeze every other thread while the 5 bytes are swapped, so we can never patch
// underneath a thread that is mid-instruction at the target.
static void suspendOthers(HANDLE* out, int* n, int max) {
    *n = 0;
    return;      // port: the few bytes of each patch are written with the other threads running, as the proxy's own patches are
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me && *n < max) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (h) { SuspendThread(h); out[(*n)++] = h; }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}
static void resumeAll(HANDLE* h, int n) {
    for (int i = 0; i < n; ++i) { ResumeThread(h[i]); CloseHandle(h[i]); }
}

static FARPROC* iatSlot(BYTE* base, const char* dll, const char* fn) {
    HMODULE hmod = GetModuleHandleA(dll);
    if (!hmod) return 0;
    FARPROC tgt = GetProcAddress(hmod, fn);
    if (!tgt) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; ft->u1.Function; ++ft) {
            if ((FARPROC)(ULONG_PTR)ft->u1.Function == tgt) return (FARPROC*)&ft->u1.Function;
        }
    }
    return 0;
}
static bool hookSlot(FARPROC* slot, void* fn, void** orig) {
    DWORD op;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op)) return false;
    if (orig) *orig = (void*)*slot;
    *slot = (FARPROC)fn;
    VirtualProtect(slot, sizeof(void*), op, &op);
    return true;
}

static LARGE_INTEGER g_pqpf;
static __forceinline LONG64 qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static LONG64 rd64(volatile LONG64* p) { for (;;) { LONG64 a = *p; LONG64 b = *p; if (a == b) return a; } }

static DWORD  g_mkTid = 0;
static volatile LONG g_mkFrames = 0, g_mkBegins = 0, g_mkEnds = 0, g_mkUnderflow = 0;

static char* msStr(LONG64 ticks, LONG64 div, char* buf) {
    if (div <= 0) div = 1;
    LONG64 us = ticks * 1000000 / g_pqpf.QuadPart / div;
    if (us < 0) { wsprintfA(buf, "-%d.%03d", (int)(-us / 1000), (int)(-us % 1000)); return buf; }
    wsprintfA(buf, "%d.%03d", (int)(us / 1000), (int)(us % 1000)); return buf;
}
static char* pctStr(LONG64 num, LONG64 den, char* buf) {
    if (den <= 0) den = 1; LONG64 p10 = num * 1000 / den;
    wsprintfA(buf, "%d.%d", (int)(p10 / 10), (int)(p10 % 10)); return buf;
}
static char* perStr(LONG64 num, LONG64 den, char* buf) {       // num/den with one decimal
    if (den <= 0) den = 1; LONG64 v10 = num * 10 / den;
    wsprintfA(buf, "%d.%d", (int)(v10 / 10), (int)(v10 % 10)); return buf;
}

// names from parts of the accelerator that are not in this carve-out
static volatile unsigned __int64 g_lrPresentTsc = 0;      // the logic limiter's frame mark
static const LONG g_dgTracing = 0;                        // draw-generation tracing: off
template <class... T> static inline void dgTraceAdd(T...) {}
static void rtPortAfterInstall(void** dvt, void** tvt, void** svt);
#include "aotr_rt.inc"

// ---- descriptions answered without asking Direct3D (port) ------------------------------------------------------
// The accelerator asks an object for its description (GetDesc, GetLevelDesc, GetLevelCount) on the game's thread
// wherever it needs a size or a pool: "immutable, so safe to call directly". Under Wine every Direct3D 9 call takes one
// global lock, and the worker holds that lock while it waits for Wine's own command thread, so each such question
// made the game's thread wait for the worker after all (54 % of its time in a battle). The answers are kept here per
// object, for the game's thread only. A new object can reuse a dead one's address, so everything is forgotten whenever
// anything is created or the device is reset (rare while playing): the creation calls are wrapped for that.
static volatile LONG g_rtPortGen = 1;
struct RtPortDescE { DWORD obj, key; LONG gen; DWORD hr; BYTE d[32]; };
static RtPortDescE g_rtPortDC[4][2048];                 // 0 surfaces, 1 vertex buffers, 2 index buffers, 3 texture levels / level count
static void* g_rtPortReal[8];
static volatile LONG g_rtPortDescHit = 0, g_rtPortDescMiss = 0;
static __forceinline RtPortDescE* rtPortDescFind(int k, DWORD obj, DWORD key) {
    RtPortDescE* e = &g_rtPortDC[k][((obj >> 4) ^ (key * 31)) & 2047];
    if (e->obj != obj || e->key != key || !e->gen) return NULL;
    return (k == 1 || k == 2 || e->gen == g_rtPortGen) ? e : NULL;      // buffers are forgotten one by one when their address is reused; the rest by generation
}
static __forceinline RtPortDescE* rtPortDescSlot(int k, DWORD obj, DWORD key) { return &g_rtPortDC[k][((obj >> 4) ^ (key * 31)) & 2047]; }
static __forceinline DWORD rtPortDesc2(int k, int real, int bytes, void* self, DWORD pDesc) {         // GetDesc(self, desc)
    if (!pDesc || __readfsdword(0x24) != g_rtMainTid) return ((DWORD(__stdcall*)(void*, DWORD))g_rtPortReal[real])(self, pDesc);
    RtPortDescE* e = rtPortDescFind(k, (DWORD)(ULONG_PTR)self, 0);
    if (e) { memcpy((void*)(ULONG_PTR)pDesc, e->d, bytes); g_rtPortDescHit++; return e->hr; }
    LONG gen = g_rtPortGen;
    DWORD hr = ((DWORD(__stdcall*)(void*, DWORD))g_rtPortReal[real])(self, pDesc); g_rtPortDescMiss++;
    if ((LONG)hr >= 0) { e = rtPortDescSlot(k, (DWORD)(ULONG_PTR)self, 0); e->gen = 0; e->obj = (DWORD)(ULONG_PTR)self; e->key = 0; e->hr = hr; memcpy(e->d, (void*)(ULONG_PTR)pDesc, bytes); e->gen = gen; }
    return hr;
}
static DWORD __stdcall rtPortSurfDesc(void* self, DWORD pDesc) { return rtPortDesc2(0, 0, 32, self, pDesc); }
static DWORD __stdcall rtPortVbDesc(void* self, DWORD pDesc)   { return rtPortDesc2(1, 1, 24, self, pDesc); }
static DWORD __stdcall rtPortIbDesc(void* self, DWORD pDesc)   { return rtPortDesc2(2, 2, 20, self, pDesc); }
static DWORD __stdcall rtPortTexLevelDesc(void* self, DWORD level, DWORD pDesc) {                     // GetLevelDesc(self, level, desc)
    if (!pDesc || __readfsdword(0x24) != g_rtMainTid) return ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_rtPortReal[3])(self, level, pDesc);
    RtPortDescE* e = rtPortDescFind(3, (DWORD)(ULONG_PTR)self, level + 1);
    if (e) { memcpy((void*)(ULONG_PTR)pDesc, e->d, 32); g_rtPortDescHit++; return e->hr; }
    LONG gen = g_rtPortGen;
    DWORD hr = ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_rtPortReal[3])(self, level, pDesc); g_rtPortDescMiss++;
    if ((LONG)hr >= 0) { e = rtPortDescSlot(3, (DWORD)(ULONG_PTR)self, level + 1); e->gen = 0; e->obj = (DWORD)(ULONG_PTR)self; e->key = level + 1; e->hr = hr; memcpy(e->d, (void*)(ULONG_PTR)pDesc, 32); e->gen = gen; }
    return hr;
}
static DWORD __stdcall rtPortTexLevelCount(void* self) {                                              // GetLevelCount(self)
    if (__readfsdword(0x24) != g_rtMainTid) return ((DWORD(__stdcall*)(void*))g_rtPortReal[4])(self);
    RtPortDescE* e = rtPortDescFind(3, (DWORD)(ULONG_PTR)self, 0x80000000u);
    if (e) { g_rtPortDescHit++; return e->hr; }
    LONG gen = g_rtPortGen;
    DWORD n = ((DWORD(__stdcall*)(void*))g_rtPortReal[4])(self); g_rtPortDescMiss++;
    e = rtPortDescSlot(3, (DWORD)(ULONG_PTR)self, 0x80000000u); e->gen = 0; e->obj = (DWORD)(ULONG_PTR)self; e->key = 0x80000000u; e->hr = n; e->gen = gen;
    return n;
}
// GetSurfaceLevel(self, level, &surface): the game asks a texture for its level surface again and again (the radar,
// the terrain pages). In Wine it is "take the global lock, look the surface up, count a reference". The surface of a
// level never changes while the texture lives, and counting a reference on it needs no lock, so a remembered answer is
// the same thing without the wait.
static DWORD __stdcall rtPortTexSurfLevel(void* self, DWORD level, DWORD ppSurf) {
    if (!ppSurf || level >= 0x1000 || __readfsdword(0x24) != g_rtMainTid) return ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_rtPortReal[5])(self, level, ppSurf);
    RtPortDescE* e = rtPortDescFind(3, (DWORD)(ULONG_PTR)self, 0x40000000u | level);
    if (e && e->hr) {
        ((DWORD(__stdcall*)(void*))g_rtO_surf[1])((void*)(ULONG_PTR)e->hr);                          // AddRef, as GetSurfaceLevel does
        *(DWORD*)(ULONG_PTR)ppSurf = e->hr; g_rtPortDescHit++; return 0;
    }
    LONG gen = g_rtPortGen;
    DWORD hr = ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_rtPortReal[5])(self, level, ppSurf); g_rtPortDescMiss++;
    if ((LONG)hr >= 0 && *(DWORD*)(ULONG_PTR)ppSurf) { e = rtPortDescSlot(3, (DWORD)(ULONG_PTR)self, 0x40000000u | level); e->gen = 0; e->obj = (DWORD)(ULONG_PTR)self; e->key = 0x40000000u | level; e->hr = *(DWORD*)(ULONG_PTR)ppSurf; e->gen = gen; }
    return hr;
}
// the creation calls: the real one, then everything remembered above is void
static void* g_rtPortRealNew[40];
#define RT_PORT_NEW(SLOT, ARGS, PASS) static DWORD __stdcall rtPortNew##SLOT ARGS { DWORD r = ((DWORD(__stdcall*) ARGS)g_rtPortRealNew[SLOT]) PASS; InterlockedIncrement(&g_rtPortGen); return r; }
RT_PORT_NEW(13, (void* s, DWORD a, DWORD b), (s, a, b))
RT_PORT_NEW(16, (void* s, DWORD a), (s, a))
RT_PORT_NEW(23, (void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h), (s, a, b, c, d, e, f, g, h))
RT_PORT_NEW(24, (void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h, DWORD i), (s, a, b, c, d, e, f, g, h, i))
RT_PORT_NEW(25, (void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g), (s, a, b, c, d, e, f, g))
// a new vertex or index buffer: only what was remembered for its address is void (they are created while playing)
// (its description is what it was created with, so it is remembered at once when the game's thread created it)
static void rtPortNewBuf(DWORD hr, DWORD pp, int k, DWORD length, DWORD usage, DWORD fmtOrFvf, DWORD pool) {
    if (!pp || IsBadReadPtr((void*)(ULONG_PTR)pp, 4)) return;
    DWORD o = *(DWORD*)(ULONG_PTR)pp; if (!o) return;
    for (int i = 1; i <= 2; ++i) { RtPortDescE* e = rtPortDescSlot(i, o, 0); if (e->obj == o) InterlockedExchange(&e->gen, 0); }
    if ((LONG)hr < 0 || __readfsdword(0x24) != g_rtMainTid) return;
    RtPortDescE* e = rtPortDescSlot(k, o, 0); DWORD* d = (DWORD*)e->d;
    e->gen = 0; e->obj = o; e->key = 0; e->hr = 0;
    if (k == 1) { d[0] = 100; d[1] = 6; d[2] = usage; d[3] = pool; d[4] = length; d[5] = fmtOrFvf; }      // D3DVERTEXBUFFER_DESC: VERTEXDATA, VERTEXBUFFER, usage, pool, size, FVF
    else        { d[0] = fmtOrFvf; d[1] = 7; d[2] = usage; d[3] = pool; d[4] = length; }                  // D3DINDEXBUFFER_DESC: format, INDEXBUFFER, usage, pool, size
    e->gen = g_rtPortGen;
}
static DWORD __stdcall rtPortNew26(void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f) { DWORD r = ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))g_rtPortRealNew[26])(s, a, b, c, d, e, f); rtPortNewBuf(r, e, 1, a, b, c, d); return r; }
static DWORD __stdcall rtPortNew27(void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f) { DWORD r = ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))g_rtPortRealNew[27])(s, a, b, c, d, e, f); rtPortNewBuf(r, e, 2, a, b, c, d); return r; }
// TestCooperativeLevel, which the game asks every frame: the accelerator runs it under its render lock, i.e. after
// whatever the worker is in the middle of. While the answer is "fine" it is repeated, and asked for real every 64th time.
static void* g_rtPortPrevTCL = NULL; static void* g_rtPortPrevTexQI = NULL;
static DWORD __stdcall rtPortTCL(void* self) {
    static DWORD last = 0, n = 0;
    if (g_rtActive && __readfsdword(0x24) == g_rtMainTid && !g_rtDirectScope && last == 0 && (++n & 63)) return 0;
    last = ((DWORD(__stdcall*)(void*))g_rtPortPrevTCL)(self);
    return last;
}
// A texture's QueryInterface, also per frame and also under the render lock in the accelerator: in Wine's Direct3D 9 it
// only compares the interface id and counts a reference (interlocked), so the game's thread calls it as it is.
static DWORD __stdcall rtPortTexQI(void* self, DWORD riid, DWORD ppv) {
    if (g_rtActive && __readfsdword(0x24) == g_rtMainTid && !g_rtDirectScope) return ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_rtO_tex[0])(self, riid, ppv);
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_rtPortPrevTexQI)(self, riid, ppv);
}
RT_PORT_NEW(28, (void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h), (s, a, b, c, d, e, f, g, h))
RT_PORT_NEW(29, (void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h), (s, a, b, c, d, e, f, g, h))
RT_PORT_NEW(36, (void* s, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f), (s, a, b, c, d, e, f))
// ---- small system-memory surfaces kept in memory (port) ---------------------------------------------------------
// The game writes its animated mouse pointer into a 64x64 system-memory surface every frame (LockRect, pixels,
// UnlockRect, then SetCursorProperties). The accelerator runs such a lock and its unlock "directly under the render
// lock": twice a frame the game's thread waits for whatever the worker is in the middle of, which under Wine is often a
// buffer map that waits for Wine's command thread (23 % of the game thread's time in a big fight). Here such a surface
// (system-memory pool, 32 bits per pixel, at most 256x256) gets a copy in memory the first time the game's thread
// locks it: one real read. From then on a lock hands out the copy and an unlock queues "LockRect, copy the rows,
// UnlockRect" (the accelerator's own record for staged rectangle writes), so the real surface holds the same pixels
// once the queue has run that far. A reference is kept so the surface cannot go away under the copy; a copy not used for
// some seconds is given up, and so is one whose surface is locked in any other way (D3DX, another thread).
struct RtPortSM { DWORD surf, w, h, l, t, r, b, lastFrame; BYTE* mem; BYTE live, locked, write; };
static RtPortSM g_rtPortSM[8];
static DWORD g_rtPortSMNo[32]; static int g_rtPortSMNoI = 0;
static volatile LONG g_rtPortSMAny = 0, g_rtPortSMLocks = 0, g_rtPortSMAdopt = 0, g_rtPortSMDrop = 0;
static void* g_rtPortPrevSurfLock = NULL; static void* g_rtPortPrevSurfUnlock = NULL;
static BYTE* g_rtPortSMTmp = NULL;
static RtPortSM* rtPortSMFind(DWORD s) { for (int i = 0; i < 8; ++i) if (g_rtPortSM[i].live && g_rtPortSM[i].surf == s) return &g_rtPortSM[i]; return NULL; }
static void rtPortSMGiveUp(RtPortSM* m) {                 // game's thread, outside a direct scope: the kept reference goes back through the queue
    m->live = 0; g_rtPortSMDrop++;
    int any = 0; for (int i = 0; i < 8; ++i) any |= g_rtPortSM[i].live; g_rtPortSMAny = any;
    rt_surf_2((void*)(ULONG_PTR)m->surf);
}
static RtPortSM* rtPortSMAdoptSurf(void* self) {
    DWORD s = (DWORD)(ULONG_PTR)self;
    for (int i = 0; i < 32; ++i) if (g_rtPortSMNo[i] == s) return NULL;
    BYTE d[32];
    if ((LONG)rtPortSurfDesc(self, (DWORD)(ULONG_PTR)d) < 0) return NULL;
    DWORD fmt = ((DWORD*)d)[0], pool = ((DWORD*)d)[3], w = ((DWORD*)d)[6], h = ((DWORD*)d)[7];      // D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height
    if (pool != 2 || (fmt != 21 && fmt != 22) || !w || !h || w > 256 || h > 256) { g_rtPortSMNo[g_rtPortSMNoI++ & 31] = s; return NULL; }
    RtPortSM* m = NULL;
    for (int i = 0; i < 8 && !m; ++i) if (!g_rtPortSM[i].live) m = &g_rtPortSM[i];
    for (int i = 0; i < 8 && !m; ++i) if (!g_rtPortSM[i].locked && (DWORD)(g_mkFrames - g_rtPortSM[i].lastFrame) > 600) { rtPortSMGiveUp(&g_rtPortSM[i]); m = &g_rtPortSM[i]; }
    if (!m) return NULL;
    if (!m->mem) m->mem = (BYTE*)VirtualAlloc(NULL, 256 * 256 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_rtPortSMTmp) g_rtPortSMTmp = (BYTE*)VirtualAlloc(NULL, 256 * 256 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m->mem || !g_rtPortSMTmp) return NULL;
    struct { INT pitch; void* bits; } lr = { 0, NULL };
    if ((LONG)((DWORD(__stdcall*)(void*, void*, void*, DWORD))g_rtPortPrevSurfLock)(self, &lr, NULL, 0x10) < 0 || !lr.bits) { g_rtPortSMNo[g_rtPortSMNoI++ & 31] = s; return NULL; }   // the one real read
    for (DWORD y = 0; y < h; ++y) memcpy(m->mem + y * w * 4, (BYTE*)lr.bits + (INT)y * lr.pitch, w * 4);
    ((DWORD(__stdcall*)(void*))g_rtPortPrevSurfUnlock)(self);
    ((DWORD(__stdcall*)(void*))g_rtO_surf[1])(self);                                                  // AddRef: kept while the copy lives
    m->surf = s; m->w = w; m->h = h; m->locked = 0; m->write = 0; m->lastFrame = g_mkFrames; m->live = 1; g_rtPortSMAny = 1; g_rtPortSMAdopt++;
    return m;
}
static DWORD __stdcall rtPortSurfLock(void* self, DWORD pLocked, DWORD pRect, DWORD flags) {
    DWORD s = (DWORD)(ULONG_PTR)self;
    if (g_rtActive && __readfsdword(0x24) == g_rtMainTid && !g_rtDirectScope) {
        RtPortSM* m = pLocked ? rtPortSMFind(s) : NULL;
        if (!m && pLocked) m = rtPortSMAdoptSurf(self);
        if (m) {
            if (m->locked) return 0x8876086Cu;                                                       // D3DERR_INVALIDCALL: it is locked already
            DWORD l = 0, t = 0, r = m->w, b = m->h;
            if (pRect) { const LONG* rc = (const LONG*)(ULONG_PTR)pRect; l = (DWORD)rc[0]; t = (DWORD)rc[1]; r = (DWORD)rc[2]; b = (DWORD)rc[3]; }
            if (l < r && t < b && r <= m->w && b <= m->h) {
                m->l = l; m->t = t; m->r = r; m->b = b; m->locked = 1; m->write = !(flags & 0x10); m->lastFrame = g_mkFrames;
                ((INT*)(ULONG_PTR)pLocked)[0] = (INT)(m->w * 4); ((void**)(ULONG_PTR)pLocked)[1] = m->mem + (t * m->w + l) * 4;
                g_rtPortSMLocks++;
                return 0;
            }
            rtPortSMGiveUp(m);                                                                        // a rectangle outside it: not for the copy
        }
    } else if (g_rtPortSMAny) {                                                                       // locked some other way: what the copy holds is no longer the truth
        RtPortSM* m = rtPortSMFind(s);
        if (m && !m->locked) { m->live = 0; g_rtPortSMDrop++; g_rtPortSMNo[g_rtPortSMNoI++ & 31] = s; }   // (its reference stays: rare, and a release here would not be in order)
    }
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))g_rtPortPrevSurfLock)(self, pLocked, pRect, flags);
}
static DWORD __stdcall rtPortSurfUnlock(void* self) {
    if (g_rtPortSMAny && g_rtActive && __readfsdword(0x24) == g_rtMainTid && !g_rtDirectScope) {
        RtPortSM* m = rtPortSMFind((DWORD)(ULONG_PTR)self);
        if (m && m->locked) {
            m->locked = 0;
            if (m->write) {
                DWORD w = m->r - m->l, h = m->b - m->t, row = w * 4, bytes = row * h;
                for (DWORD y = 0; y < h; ++y) memcpy(g_rtPortSMTmp + y * row, m->mem + ((m->t + y) * m->w + m->l) * 4, row);
                RtRec* rec = rtAlloc(9, bytes); rec->kind = 7; rec->op = 304;
                DWORD* A = rtArgs(rec); BYTE* P = rtPayload(rec); DWORD po = 0;
                A[0] = m->surf; A[1] = m->l; A[2] = m->t; A[3] = m->r; A[4] = m->b; A[5] = 4;
                A[6] = rtBytes(P, &po, (DWORD)(ULONG_PTR)g_rtPortSMTmp, bytes);
                A[7] = (DWORD)(ULONG_PTR)g_rtO_surf[13]; A[8] = (DWORD)(ULONG_PTR)g_rtO_surf[14];
                rtTouch(m->surf); rtCommit(rec, po);
            }
            return 0;
        }
    }
    return ((DWORD(__stdcall*)(void*))g_rtPortPrevSurfUnlock)(self);
}
static void rtPortSlot(void** slot, void* fn, void** prev) {
    DWORD op = 0; *prev = *slot;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op)) { *slot = fn; VirtualProtect(slot, sizeof(void*), op, &op); }
}
static void rtPortAfterInstall(void** dvt, void** tvt, void** svt) {  // the originals are in the accelerator's tables now: put the answering versions in their place
    if (dvt) rtPortSlot(&dvt[3], (void*)&rtPortTCL, &g_rtPortPrevTCL);
    if (tvt) rtPortSlot(&tvt[0], (void*)&rtPortTexQI, &g_rtPortPrevTexQI);
    char off[8]; off[0] = 0; char mk[MAX_PATH]; wsprintfA(mk, "%s\\renderthread.nosurfcopy", g_dir);
    if (svt && GetFileAttributesA(mk) == INVALID_FILE_ATTRIBUTES) { rtPortSlot(&svt[13], (void*)&rtPortSurfLock, &g_rtPortPrevSurfLock); rtPortSlot(&svt[14], (void*)&rtPortSurfUnlock, &g_rtPortPrevSurfUnlock); }
    g_rtPortReal[0] = g_rtO_surf[12]; g_rtO_surf[12] = (void*)&rtPortSurfDesc;
    g_rtPortReal[1] = g_rtO_vb[13];   g_rtO_vb[13]   = (void*)&rtPortVbDesc;
    g_rtPortReal[2] = g_rtO_ib[13];   g_rtO_ib[13]   = (void*)&rtPortIbDesc;
    g_rtPortReal[3] = g_rtO_tex[17];  g_rtO_tex[17]  = (void*)&rtPortTexLevelDesc;
    g_rtPortReal[4] = g_rtO_tex[13];  g_rtO_tex[13]  = (void*)&rtPortTexLevelCount;
    g_rtPortReal[5] = g_rtO_tex[18];  g_rtO_tex[18]  = (void*)&rtPortTexSurfLevel;
    static const int slots[10] = { 13, 16, 23, 24, 25, 26, 27, 28, 29, 36 };
    static void* const news[10] = { (void*)&rtPortNew13, (void*)&rtPortNew16, (void*)&rtPortNew23, (void*)&rtPortNew24, (void*)&rtPortNew25, (void*)&rtPortNew26, (void*)&rtPortNew27, (void*)&rtPortNew28, (void*)&rtPortNew29, (void*)&rtPortNew36 };
    for (int i = 0; i < 10; ++i) { g_rtPortRealNew[slots[i]] = g_rtO_dev[slots[i]]; g_rtO_dev[slots[i]] = news[i]; }
    logf("port: object descriptions are answered from memory on the game's thread (surface, buffer, texture level); creation and reset forget them.");
}

// ---- the accelerator's other modules that the render thread talks to, absent here ------------------------------
// Draw merging (mg*), in-between frames (tw*), texture decoding on worker threads (txj*), shadow filtering (sp*):
// each answers "nothing to do". Their switches (g_mgAny, g_twBusy, g_twHold, g_txjOn) are never set.
static DWORD mgAlias(DWORD buf) { return buf; }
static void mgOnWriteLock(DWORD) {}
static void mgDropAll(const char*) {}
static void mgFrameTick() {}
static void spWrap() {}
static void twExecCtl(RtRec*) {}
static BYTE twFrameEnd() { return 0; }
static DWORD twIdleWaitMs() { return 2; }
static void twInitOnce() {}
static void twOnDirectWrite(int) {}
static void twOnReset() {}
static void twOnSync(int) {}
static void twResetRefused(DWORD) {}
static void twService() {}
static void twServicePictures() {}
static void twSpacePressure() {}
static bool twStretchOpenGt() { return false; }
static void twWrap() {}
static bool txjCheckReq(DWORD*, DWORD*) { return false; }
static bool txjCreateTexture(DWORD*, DWORD*) { return false; }
static bool txjCreateFromFile(DWORD*, DWORD*) { return false; }
static bool txjLoadSurface(DWORD*, DWORD*) { return false; }
static bool txjFilter(DWORD*, DWORD*) { return false; }
static bool txjLoadSurfaceFile(DWORD*, DWORD*) { return false; }
static bool txjFlushNow(DWORD) { return false; }
static void txjExecUpload(RtRec*) {}
static void txjOnInstall(void*) {}
static void txjOnReset() {}

// ---- monitor (diagnostic): once a second, the queue's counters and where the game's and the worker's threads are --
static void rtPortWhere(DWORD tid, char* out) {            // "module+offset <- return addresses found on the stack"
    out[0] = 0;
    HANDLE h = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : NULL;
    if (!h) { lstrcpyA(out, "?"); return; }
    CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    DWORD st[400]; int ns = 0;
    DWORD code = 0; GetExitCodeThread(h, &code); DWORD alive = code == STILL_ACTIVE ? WAIT_TIMEOUT : 0;
    DWORD sus = SuspendThread(h);
    if (sus == (DWORD)-1 || alive != WAIT_TIMEOUT) wsprintfA(out, "(suspend %d err %u, thread %s, exit code %08X) ", (int)sus, GetLastError(), alive == WAIT_TIMEOUT ? "alive" : "ENDED", code);
    if (sus != (DWORD)-1) {
        if (!GetThreadContext(h, &c)) { wsprintfA(out, "(no context, err %u) ", GetLastError()); c.Eip = 0; }
        else {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)(ULONG_PTR)c.Esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
                DWORD end = (DWORD)(ULONG_PTR)mbi.BaseAddress + (DWORD)mbi.RegionSize;
                for (DWORD a = c.Esp & ~3u; a + 4 <= end && ns < 400; a += 4) st[ns++] = *(DWORD*)(ULONG_PTR)a;
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    char own[64]; rtOwner(c.Eip, own, 64);
    HMODULE hm = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)c.Eip, &hm);
    int n = lstrlenA(out); n += wsprintfA(out + n, "%s+%X", own, c.Eip - (DWORD)(ULONG_PTR)hm);
    int shown = 0;
    for (int i = 0; i < ns && shown < 9 && n < 380; ++i) {
        DWORD v = st[i];
        if (v < 0x00401000 || !rtIsCode(v)) continue;
        const BYTE* p = (const BYTE*)(ULONG_PTR)v;         // a return address follows a call instruction
        if (IsBadReadPtr(p - 7, 7)) continue;
        if (!(p[-5] == 0xE8 || p[-2] == 0xFF || p[-3] == 0xFF || p[-6] == 0xFF || p[-7] == 0xFF)) continue;
        hm = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)v, &hm);
        rtOwner(v, own, 64);
        n += wsprintfA(out + n, " < %s+%X", own, v - (DWORD)(ULONG_PTR)hm); ++shown;
    }
}
// Where is a thread, statistically: n samples of its instruction pointer and of the first return addresses on its
// stack (a scan for values that follow a call instruction: approximate, stale ones can slip in), most frequent first.
struct RtPortSite { DWORD a[7]; int n; };             // a[0] instruction pointer, a[1..5] callers by frame pointer, a[6] critical section waited for
static void rtPortSample(DWORD tid, const char* who, int n, int gapMs) {
    static RtPortSite site[256]; int ns = 0;
    HANDLE h = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : NULL;
    if (!h) return;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    DWORD waitLo = (DWORD)(ULONG_PTR)GetProcAddress(nt, "RtlpWaitForCriticalSection"), waitHi = (DWORD)(ULONG_PTR)GetProcAddress(nt, "RtlpUnWaitCriticalSection");
    for (int s = 0; s < n; ++s) {
        CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        DWORD cur[7] = { 0, 0, 0, 0, 0, 0, 0 };
        if (SuspendThread(h) != (DWORD)-1) {
            if (GetThreadContext(h, &c)) {
                MEMORY_BASIC_INFORMATION mbi; DWORD lo = c.Esp, hi = 0;
                if (VirtualQuery((void*)(ULONG_PTR)c.Esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) hi = (DWORD)(ULONG_PTR)mbi.BaseAddress + (DWORD)mbi.RegionSize;
                cur[0] = c.Eip; int k = 1; DWORD bp = c.Ebp;
                for (int f = 0; f < 24 && bp >= lo && bp + 12 <= hi && !(bp & 3); ++f) {       // the chain of saved frame pointers
                    DWORD ret = ((DWORD*)(ULONG_PTR)bp)[1], up = ((DWORD*)(ULONG_PTR)bp)[0];
                    if (ret >= waitLo && ret < waitHi && up >= lo && up + 12 <= hi) cur[6] = ((DWORD*)(ULONG_PTR)up)[2];   // RtlpWaitForCriticalSection's argument
                    if (k < 6) cur[k++] = ret;
                    if (up <= bp) break;
                    bp = up;
                }
            }
            ResumeThread(h);
        }
        int j = 0; for (; j < ns; ++j) if (!memcmp(site[j].a, cur, sizeof(cur))) break;
        if (j == ns) { if (ns < 256) { memcpy(site[ns].a, cur, sizeof(cur)); site[ns].n = 0; ++ns; } else j = -1; }
        if (j >= 0) site[j].n++;
        Sleep(gapMs);
    }
    CloseHandle(h);
    for (int k = 0; k < 14; ++k) {
        int bi = -1; for (int i = 0; i < ns; ++i) if (site[i].n > 0 && (bi < 0 || site[i].n > site[bi].n)) bi = i;
        if (bi < 0) break;
        char line[700]; int len = wsprintfA(line, "where: %s %3d of %d:", who, site[bi].n, n);
        for (int i = 0; i < 6 && site[bi].a[i]; ++i) {
            char own[64]; HMODULE hm = NULL; DWORD v = site[bi].a[i];
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)v, &hm); rtOwner(v, own, 64);
            len += wsprintfA(line + len, "%s%s+%X", i ? " < " : " ", own, v - (DWORD)(ULONG_PTR)hm);
        }
        if (site[bi].a[6]) {
            char own[64]; HMODULE hm = NULL; DWORD v = site[bi].a[6];
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)v, &hm) && hm) { rtOwner(v, own, 64); len += wsprintfA(line + len, " | waits for the lock at %s+%X", own, v - (DWORD)(ULONG_PTR)hm); }
            else len += wsprintfA(line + len, " | waits for the lock at %08X (not in a module)", v);
            if (v == (DWORD)(ULONG_PTR)&g_rtExecCs) len += wsprintfA(line + len, " = the render lock");
            if (!IsBadReadPtr((void*)(ULONG_PTR)v, sizeof(CRITICAL_SECTION))) len += wsprintfA(line + len, ", owner thread %u", (DWORD)(ULONG_PTR)((CRITICAL_SECTION*)(ULONG_PTR)v)->OwningThread);
        }
        logf("%s", line); site[bi].n = 0;
    }
}
static void rtPortStats() {                               // per period: what still makes the game's thread wait
    static LONG lP = 0, lD = 0, lSC[RT_SCN], lSync = 0; static LONG64 lDT = 0, lPW = 0, lOpT[512]; static DWORD lSeq = 0;
    LONG P = g_rtNPresent, D = g_rtDrains, S = g_rtNSync; LONG64 DT = rd64(&g_rtDrainTicks), PW = rd64(&g_rtPresentWaitTicks); DWORD seq = g_rtSeq;
    LONG f = P - lP; if (f <= 0) f = 1;
    char a[32], b[32], c[32], d[32], e[32];
    logf("stats: %d frames | per frame: %s records queued, %s waits for the queue (%s ms), %s ms waiting for the previous frame's Present, %s synchronous calls",
         (int)f, perStr((LONG64)(seq - lSeq), f, a), perStr(D - lD, f, b), msStr(DT - lDT, f, c), msStr(PW - lPW, f, d), perStr(S - lSync, f, e));
    { static LONG lM = 0, lJ = 0; LONG M = g_rtPortNMap, J = g_rtPortNJoined; char m1[32], m2[32];
      logf("stats:   staged buffer writes per frame: %s maps, %s more joined to them", perStr(M - lM, f, m1), perStr(J - lJ, f, m2)); lM = M; lJ = J; }
    { static LONG lLS[RT_LSN]; LONG dl[RT_LSN]; for (int i = 0; i < RT_LSN; ++i) { LONG n = g_rtLS[i].n; dl[i] = g_rtLS[i].ra ? n - lLS[i] : 0; lLS[i] = n; }
      for (int k = 0; k < 8; ++k) {
          int bi = -1; for (int i = 0; i < RT_LSN; ++i) if (dl[i] > 0 && (bi < 0 || dl[i] > dl[bi])) bi = i;
          if (bi < 0) break;
          RtLockStat& e = g_rtLS[bi]; char own[64], pf[16]; HMODULE hm = NULL;
          GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)e.ra, &hm); rtOwner(e.ra, own, 64);
          logf("stats:   lock of a %s from %s+%X: pool %d, format %d, usage %X, %dx%d, flags %X, %s, %s per frame", (e.flags & 0x80000000u) ? "texture" : "surface", own, e.ra - (DWORD)(ULONG_PTR)hm,
               (int)e.pool, (int)e.fmt, (unsigned)e.usage, (int)e.w, (int)e.h, (unsigned)(e.flags & 0x7FFFFFFFu), e.fast ? "run directly under the render lock" : "after waiting for the queue", perStr(dl[bi], f, pf));
          dl[bi] = 0;
      }
      logf("stats:   small system-memory surfaces kept in memory: %d locks served, %d adopted, %d given up", (int)g_rtPortSMLocks, (int)g_rtPortSMAdopt, (int)g_rtPortSMDrop);
      logf("stats:   texture mirrors: %d locks served, %d adopted, %d dropped, %d failed, %d without a free slot | calls under the render lock so far %d", (int)g_rtNTMLock, (int)g_rtNTMAdopt, (int)g_rtNTMDrop, (int)g_rtNTMFail, (int)g_rtNTMNoSlot, (int)g_rtNLocked); }
    lP = P; lD = D; lDT = DT; lPW = PW; lSeq = seq; lSync = S;
    LONG64 dOp[512]; for (int i = 0; i < 512; ++i) { LONG64 v = rd64(&g_rtDrainOpTicks[i]); dOp[i] = v - lOpT[i]; lOpT[i] = v; }
    for (int k = 0; k < 6; ++k) {
        int bi = -1; for (int i = 0; i < 512; ++i) if (dOp[i] > 0 && (bi < 0 || dOp[i] > dOp[bi])) bi = i;
        if (bi < 0) break;
        logf("stats:   wait by call: %s %s ms/frame", rtOpName(bi), msStr(dOp[bi], f, a)); dOp[bi] = 0;
    }
    LONG dn[RT_SCN]; for (int i = 0; i < RT_SCN; ++i) { LONG n = g_rtSC[i].n; dn[i] = n - lSC[i]; lSC[i] = n; }
    for (int k = 0; k < 10; ++k) {
        int bi = -1; for (int i = 0; i < RT_SCN; ++i) if (dn[i] > 0 && (bi < 0 || dn[i] > dn[bi])) bi = i;
        if (bi < 0) break;
        char own[64]; HMODULE hm = NULL; DWORD ra = g_rtSC[bi].ra;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)ra, &hm); rtOwner(ra, own, 64);
        logf("stats:   synchronous: %s from %s+%X (%08X), %s per frame", rtOpName(g_rtSC[bi].op), own, ra - (DWORD)(ULONG_PTR)hm, ra, perStr(dn[bi], f, a)); dn[bi] = 0;
    }
}
// Stack-depth watch (diagnostic): the game's thread died of a stack overflow some minutes into a fresh match. Every
// few milliseconds its stack pointer is read; the first times it is far deeper than usual, the return addresses on
// its stack are logged (the repeating part is the runaway chain).
static DWORD WINAPI rtPortStackWatch(LPVOID) {
    typedef LONG (WINAPI* NtQIT)(HANDLE, int, void*, ULONG, ULONG*);
    NtQIT q = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    while (!g_rtInstalled) Sleep(200);
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_rtMainTid);
    if (!h || !q) { logf("stack watch: cannot open the game's thread"); return 0; }
    struct { LONG status; void* teb; DWORD pid, tid; DWORD aff; LONG prio, base; } tbi; memset(&tbi, 0, sizeof(tbi));
    if (q(h, 0, &tbi, sizeof(tbi), NULL) < 0 || !tbi.teb) { logf("stack watch: no thread block"); return 0; }
    DWORD top = ((DWORD*)tbi.teb)[1], limit = ((DWORD*)tbi.teb)[2];      // NtTib.StackBase, StackLimit
    logf("stack watch: game thread %u, stack top %08X, committed down to %08X", g_rtMainTid, top, limit);
    DWORD deepest = 0; int reports = 0; DWORD nextDepth = 160 * 1024;
    static DWORD st[20000];
    for (;;) {
        Sleep(4);
        CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        int nst = 0; DWORD used = 0;
        if (SuspendThread(h) == (DWORD)-1) { logf("stack watch: the game's thread is gone (deepest seen %u KB)", deepest / 1024); return 0; }
        if (GetThreadContext(h, &c) && c.Esp && c.Esp < top) {
            used = top - c.Esp;
            if (used > nextDepth && reports < 4) for (DWORD a = c.Esp & ~3u; a + 4 <= top && nst < 20000; a += 4) st[nst++] = *(DWORD*)(ULONG_PTR)a;
        }
        ResumeThread(h);
        if (used > deepest) deepest = used;
        if (!nst) continue;
        ++reports; nextDepth = used + 96 * 1024;
        char own[64]; HMODULE hm = NULL; rtOwner(c.Eip, own, 64);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)c.Eip, &hm);
        logf("stack watch: %u KB of stack in use, at %s+%X; return addresses, innermost first:", used / 1024, own, c.Eip - (DWORD)(ULONG_PTR)hm);
        char line[900]; int len = 0, shown = 0;
        for (int i = 0; i < nst && shown < 220; ++i) {
            DWORD v = st[i];
            if (v < 0x00401000 || !rtIsCode(v)) continue;
            const BYTE* p = (const BYTE*)(ULONG_PTR)v;
            if (IsBadReadPtr(p - 7, 7)) continue;
            if (!(p[-5] == 0xE8 || p[-2] == 0xFF || p[-3] == 0xFF || p[-6] == 0xFF || p[-7] == 0xFF)) continue;
            hm = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)v, &hm); rtOwner(v, own, 64);
            len += wsprintfA(line + len, "%s+%X ", own, v - (DWORD)(ULONG_PTR)hm); ++shown;
            if (len > 760) { logf("stack watch:   %s", line); len = 0; }
        }
        if (len) logf("stack watch:   %s", line);
    }
}
static DWORD WINAPI rtPortMonitor(LPVOID) {
    char wm[420], ww[420];
    for (int i = 0; i < 100000; ++i) {
        Sleep(i < 40 ? 1000 : 10000);
        if (!g_rtInstalled) continue;
        if (i >= 40) { rtPortStats(); if (i % 3 == 0) { rtPortSample(g_rtMainTid, "game thread", 150, 12); rtPortSample(g_rtWorkerTid, "worker", 150, 12); } continue; }
        rtPortWhere(g_rtMainTid, wm); rtPortWhere(g_rtWorkerTid, ww);
        logf("mon: active %d | presents queued %d, waiting %d | records queued %u, run %u | drains %d, waits %d (timeouts %d), pumped %d | worker %s, last op %d kind %d | lock owner %u",
             (int)g_rtActive, (int)g_rtNPresent, (int)g_rtPendingPresents, g_rtSeq, g_rtExecSeq, (int)g_rtDrains, (int)g_rtWaitCalls, (int)g_rtWaitTimeouts, (int)g_rtNPump,
             g_rtWorkerSleeping ? "asleep" : "awake", (int)g_rtDg.op, (int)g_rtDg.kind, (DWORD)(ULONG_PTR)g_rtExecCs.OwningThread);
        logf("mon:   descriptions from memory %d, asked %d | staged writes: %d maps, %d writes joined to an earlier map", (int)g_rtPortDescHit, (int)g_rtPortDescMiss, (int)g_rtPortNMap, (int)g_rtPortNJoined);
        logf("mon:   game thread: sync op %d (%s), direct scope %d, D3DX depth %d, head %u tail %u local head %u", g_rtCurOp, rtOpName(g_rtCurOp & 511), g_rtDirectScope, g_rtXDepth, (DWORD)g_rtHead, (DWORD)g_rtTail, (DWORD)g_rtLocalHead);
        logf("mon:   game thread %u at %s", g_rtMainTid, wm);
        logf("mon:   worker %u at %s", g_rtWorkerTid, ww);
    }
    return 0;
}
static DWORD WINAPI rtPortInit(LPVOID) {
    logf("renderthread.dll loaded (carve-out of BFME2 Accelerator's render thread).");
    rtInit();
    if (!g_rtTried) rtInstallEngineHooks();      // per-pixel surface writes, shroud updates, radar texture mirrors: each checks the game's bytes itself
    logf("init done: %s", g_rtTried ? "the render thread will NOT start (see above)" : "waiting for the game's drawing thread to make its first effect calls");
    return 0;
}
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        aotrSetDir(hMod);
        wsprintfA(kLogPath, "%s\\renderthread.log", g_dir);
        DeleteFileA(kLogPath);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSectionAndSpinCount(&g_rtExecCs, 4000);
        InitializeCriticalSection(&g_rtInitCs);
        QueryPerformanceFrequency(&g_pqpf);
        g_rtGuardTls = TlsAlloc();
        AddVectoredExceptionHandler(1, rtGuardVeh);
        rtPortInit(NULL);        // the game has not created its device yet when the proxy loads this: hook before it can
        { char mk[MAX_PATH]; wsprintfA(mk, "%s\\renderthread.monitor", g_dir);
          if (GetFileAttributesA(mk) != INVALID_FILE_ATTRIBUTES) { g_rtDg.on = 1; HANDLE h = CreateThread(NULL, 0, rtPortMonitor, NULL, 0, NULL); if (h) CloseHandle(h); logf("monitor thread on (file renderthread.monitor is there)."); }
          wsprintfA(mk, "%s\\renderthread.stackwatch", g_dir);
          if (GetFileAttributesA(mk) != INVALID_FILE_ATTRIBUTES) { HANDLE h = CreateThread(NULL, 0, rtPortStackWatch, NULL, 0, NULL); if (h) CloseHandle(h); logf("stack watch on (file renderthread.stackwatch is there)."); } }
    }
    return TRUE;
}
