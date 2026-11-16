/* letterbox d3d9.dll -- for SAGE games (Battle for Middle-earth II) under Wine on a Mac whose display
 * must never change mode (Dell U4025QW on an M5 Mac Studio: a real mode switch can kernel-panic it).
 *
 * The game asks Direct3D for an exclusive fullscreen device at its own resolution (say 2880x1620, 16:9).
 * This proxy creates the real device at the CURRENT desktop mode instead (3840x1620) with the refresh rate
 * left open, so wined3d sees "already in that mode" and never calls ChangeDisplaySettings. The game renders
 * into an offscreen render target of its own size, which it is handed as "the back buffer"; at Present that
 * target is copied into the middle of the real back buffer, with black bars either side. Mouse coordinates
 * are mapped the other way and the cursor is kept inside the picture, so edge scrolling works.
 *
 * Build: zig cc -target x86-windows-gnu -shared -O2 -o d3d9.dll letterbox.c -luser32 -lkernel32
 * Install: next to game.dat, with a Wine DLL override d3d9=native,builtin for game.dat.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <wchar.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* ---- exports: everything except Direct3DCreate9 jumps straight to the real d3d9 ---- */
#define FWD(X) X(D3DPERF_BeginEvent) X(D3DPERF_EndEvent) X(D3DPERF_GetStatus) X(D3DPERF_QueryRepeatFrame) \
    X(D3DPERF_SetMarker) X(D3DPERF_SetOptions) X(D3DPERF_SetRegion) X(DebugSetLevel) X(DebugSetMute) \
    X(Direct3DCreate9Ex) X(Direct3DShaderValidatorCreate9) X(PSGPError) X(PSGPSampleTexture)
#define PTR(n) void *real_##n;
FWD(PTR)
/* the C names differ from the exported ones because d3d9.h already declares those with their real types */
#define STUB(n) __declspec(dllexport) void stub_##n(void) __asm__("_" #n); \
    __attribute__((naked)) void stub_##n(void) { __asm__("jmp *_real_" #n); }
FWD(STUB)
__declspec(dllexport) void stub_Direct3DCreate9(void) __asm__("_Direct3DCreate9");
__attribute__((naked)) void stub_Direct3DCreate9(void) { __asm__("jmp _lb_Direct3DCreate9@4"); }

static HMODULE g_real;
static IDirect3D9 *(WINAPI *real_Direct3DCreate9)(UINT);
static WCHAR g_dir[MAX_PATH];

static struct {
    BOOL on;                 /* letterboxing is active */
    BOOL full;               /* the game's picture is exactly the screen: nothing to fit, but the cursor is still kept on it */
    UINT gw, gh;             /* the game's (virtual) resolution */
    UINT sw, sh;             /* the real back buffer = current desktop mode */
    RECT dst;                /* where the game picture sits in the real back buffer */
    UINT dw, dh;
    IDirect3DSurface9 *rt;   /* what the game believes is the back buffer */
    HWND hwnd;
    WNDPROC prev_proc;
    BOOL unicode;
    BOOL clipped;
} g;

/* letterbox.ini next to the dll: [letterbox] vsync=-1 keep the game's choice, 0 off, 1 on */
static int cfg_vsync = -1;
static int cfg_watch = 1;        /* watch=0: no watchdog thread; 1: it reports when no frame was finished for 4 s; 2: it also samples frames that take over 60 ms */
static int cfg_sample;           /* sample=1 (or the test command "sample 1"): sampling profiler, see sample_tick */
static int cfg_dark;             /* dark=1 (test aid): show a black screen instead of the picture (letterboxed pictures only); screenshots still work */
static int cfg_showfps = 0;      /* showfps=1 or Ctrl+Shift+F in the game: frame rate in the top-left corner (in the black bar when there is one) */
/* topinset=N in letterbox.ini: rows at the top of the screen to leave black (the camera housing of a MacBook display).
 * The launch wrapper measures it for the current main display and writes it before every start. */
static UINT cfg_topinset = 0;
static int cfg_valign = 0;       /* valign = 0: picture centred between the bars, 1: at the top (right under the inset) */

/* measurements, written to letterbox.log every 10 s */
static struct {
    DWORD moves, move_gap_max_ms, last_move_tick;   /* WM_MOUSEMOVE */
    DWORD setpos, clip, getpos;                      /* the game's own SetCursorPos / ClipCursor / GetCursorPos calls */
    DWORD wheel;
    DWORD present_us, present_max_us, blit_us;       /* time inside the real Present / inside our copy */
    DWORD b_fast, b_mid, b_slow, b_hitch;            /* frame time buckets: <12.5 ms, <25 ms, <50 ms, more */
    double pace_sum, pace_sq; DWORD pace_n, pace_min, pace_late;   /* frame-to-frame interval: mean, spread, late frames */
} st;

/* ---- log ---- */
static void logf(const char *fmt, ...)
{
    char buf[512];
    WCHAR path[MAX_PATH];
    va_list ap;
    HANDLE f;
    DWORD done;
    int n;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\letterbox.log");
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, buf, n, &done, 0);
    CloseHandle(f);
}

/* ---- what a crash looked like ----
 * When the game itself faults (an access violation with eip inside game.dat's code) the registers, the code bytes
 * at eip, the top of the stack and the return addresses found on it are written to the log before anything else
 * handles the exception. Nothing is changed: this only looks and passes the exception on. (watch=0 switches it off.) */
static DWORD g_code_lo = 0x401000, g_code_hi = 0xbd0000;
static BOOL after_call(DWORD a)            /* is the instruction before this address a call? */
{
    const BYTE *p = (const BYTE *)(ULONG_PTR)a;
    if (a < g_code_lo + 8 || a >= g_code_hi) return FALSE;
    if (p[-5] == 0xE8) return TRUE;                                         /* call rel32 */
    if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return TRUE;   /* call [abs] / call [reg+disp32] */
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return TRUE;               /* call [reg+disp8] */
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return TRUE;   /* call reg / call [reg] */
    return FALSE;
}
static BOOL mem_ok(DWORD a, DWORD bytes)   /* committed, readable, and the whole range inside one region */
{
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || a + bytes < a || !VirtualQuery((const void *)(ULONG_PTR)a, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return FALSE;
    return a + bytes <= (DWORD)(ULONG_PTR)mi.BaseAddress + mi.RegionSize;
}
static int g_names_left;
static void note_text(DWORD a, const char *how)     /* a run of at least 5 printable characters within 48 bytes of a */
{
    const BYTE *p = (const BYTE *)(ULONG_PTR)a;
    char s[52];
    int i, start = -1, best = 0, best_at = 0;
    if (g_names_left <= 0 || !mem_ok(a, 48)) return;
    for (i = 0; i <= 48; i++)
    {
        BOOL pr = i < 48 && p[i] >= 0x20 && p[i] < 0x7f;
        if (pr && start < 0) start = i;
        if (!pr && start >= 0) { if (i - start > best) { best = i - start; best_at = start; } start = -1; }
    }
    if (best < 5) return;
    memcpy(s, p + best_at, best); s[best] = 0;
    g_names_left--;
    logf("    %s -> \"%s\"", how, s);
}
static void note_names(DWORD obj)         /* text within three pointer steps of an object: what kind of thing it is */
{
    char how[64];
    DWORD i, j, k, a, b2, c2;
    if (!mem_ok(obj, 64 * 4)) return;
    for (i = 0; i < 64 && g_names_left > 0; i++)
    {
        a = ((const DWORD *)(ULONG_PTR)obj)[i];
        if (!mem_ok(a, 24 * 4)) continue;
        wsprintfA(how, "+%x", i * 4); note_text(a, how);
        for (j = 0; j < 24 && g_names_left > 0; j++)
        {
            b2 = ((const DWORD *)(ULONG_PTR)a)[j];
            if (!mem_ok(b2, 12 * 4)) continue;
            wsprintfA(how, "+%x +%x", i * 4, j * 4); note_text(b2, how);
            for (k = 0; k < 12 && g_names_left > 0; k++)
            {
                c2 = ((const DWORD *)(ULONG_PTR)b2)[k];
                wsprintfA(how, "+%x +%x +%x", i * 4, j * 4, k * 4); note_text(c2, how);
            }
        }
    }
}
static LONG CALLBACK crash_note(EXCEPTION_POINTERS *x)
{
    static LONG said;
    EXCEPTION_RECORD *r = x->ExceptionRecord;
    CONTEXT *c = x->ContextRecord;
    MEMORY_BASIC_INFORMATION mi;
    char line[500];
    const DWORD *sp;
    const BYTE *ip;
    DWORD i, n;
    int len;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || c->Eip < g_code_lo || c->Eip >= g_code_hi) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&said) > 8) return EXCEPTION_CONTINUE_SEARCH;      /* the game may catch some itself */
    ip = mem_ok(c->Eip, 8) ? (const BYTE *)(ULONG_PTR)c->Eip : (const BYTE *)"\0\0\0\0\0\0\0";
    logf("game fault: access violation at %08x %s address %08x, thread %u", c->Eip,
         r->NumberParameters < 2 || !r->ExceptionInformation[0] ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing",
         r->NumberParameters >= 2 ? (DWORD)r->ExceptionInformation[1] : 0, GetCurrentThreadId());
    logf("  eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x | code %02x %02x %02x %02x %02x %02x %02x %02x",
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
    sp = (const DWORD *)(ULONG_PTR)c->Esp;
    if (!mem_ok(c->Esp, 4) || !VirtualQuery(sp, &mi, sizeof(mi))) return EXCEPTION_CONTINUE_SEARCH;
    n = (DWORD)(((BYTE *)mi.BaseAddress + mi.RegionSize - (BYTE *)sp) / 4);
    if (n > 1024) n = 1024;
    for (len = wsprintfA(line, "  stack:"), i = 0; i < 24 && i < n; i++) len += wsprintfA(line + len, " %08x", sp[i]);
    logf("%s", line);
    for (len = wsprintfA(line, "  return addresses (offset from esp):"), i = 0; i < n && len < 460; i++)
        if (after_call(sp[i])) len += wsprintfA(line + len, " %x(+%x)", sp[i], i * 4);
    logf("%s", line);
    {   /* what the registers point at (the object the game was working on) */
        static const char *const name[7] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp" };
        const DWORD reg[7] = { c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp };
        int j;
        for (j = 0; j < 7; j++)
        {
            const DWORD *p = (const DWORD *)(ULONG_PTR)(reg[j] & ~3u);
            if (reg[j] < 0x10000 || !VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
            n = (DWORD)(((BYTE *)mi.BaseAddress + mi.RegionSize - (BYTE *)p) / 4);
            if (n > 48) n = 48;
            for (len = wsprintfA(line, "  [%s]:", name[j]), i = 0; i < n; i++) len += wsprintfA(line + len, " %08x", p[i]);
            logf("%s", line);
        }
    }
    /* 0xB6DD43: the collision manager's sweep found the end of an object's extent before its start (eax = the
     * manager's record of the object: six sorted end points at +24h.., floats at +34h +48h (x), +5Ch +70h (y),
     * +84h +98h (z); the game object that owns it at +A0h). Say which object that is. */
    if (c->Eip == 0xb6dd43 && mem_ok(c->Eax, 0xa8))
    {
        DWORD own = *(const DWORD *)(ULONG_PTR)(c->Eax + 0xa0);
        if (mem_ok(own, 64 * 4))
        {
            const DWORD *p = (const DWORD *)(ULONG_PTR)own;
            int part;
            for (part = 0; part < 2; part++)
            {
                for (len = wsprintfA(line, "  owner %08x +%02x:", own, part * 0x80), i = 0; i < 32; i++) len += wsprintfA(line + len, " %08x", p[part * 32 + i]);
                logf("%s", line);
            }
            g_names_left = 60;
            logf("  text near the owner (offsets of the pointers followed):");
            note_names(own);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void crash_note_install(void)
{
    static BOOL done;
    const BYTE *m = (const BYTE *)0x400000;
    if (done || !cfg_watch) return;
    done = TRUE;
    if (!IsBadReadPtr(m, 0x400) && m[0] == 'M' && m[1] == 'Z')
    {
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
        const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
        if (nt->Signature == IMAGE_NT_SIGNATURE && (s->Characteristics & IMAGE_SCN_MEM_EXECUTE))
        {
            g_code_lo = 0x400000 + s->VirtualAddress;
            g_code_hi = g_code_lo + s->Misc.VirtualSize;
        }
    }
    logf("crash notes: %s (game code %08x-%08x)", AddVectoredExceptionHandler(1, crash_note) ? "on" : "NOT installed", g_code_lo, g_code_hi);
}

static void patch_slot(void **slot, void *fn, void **orig)
{
    DWORD old;
    if (*slot == fn) return;
    if (orig) *orig = *slot;
    VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

/* ---- mouse: the game lives in a gw x gh "screen" ---- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void to_virtual(POINT *pt)
{
    pt->x = clampi(MulDiv(pt->x - g.dst.left, g.gw, g.dw), 0, (int)g.gw - 1);
    pt->y = clampi(MulDiv(pt->y - g.dst.top, g.gh, g.dh), 0, (int)g.gh - 1);
}

static void enforce_clip(BOOL force)
{
    if ((!g.on && !g.full) || !g.hwnd) return;
    if (GetForegroundWindow() == g.hwnd)
    {
        if (force || !g.clipped) { ClipCursor(&g.dst); g.clipped = TRUE; }
    }
    else if (g.clipped) { ClipCursor(NULL); g.clipped = FALSE; }
}

static BOOL WINAPI h_GetCursorPos(POINT *pt)
{
    BOOL r = GetCursorPos(pt);
    st.getpos++;
    if (r && g.on) to_virtual(pt);
    return r;
}
static BOOL WINAPI h_SetCursorPos(int x, int y)
{
    st.setpos++;
    if (g.on) { x = g.dst.left + MulDiv(x, g.dw, g.gw); y = g.dst.top + MulDiv(y, g.dh, g.gh); }
    return SetCursorPos(x, y);
}
static BOOL WINAPI h_ClipCursor(const RECT *rc)
{
    st.clip++;
    /* the cursor stays inside the picture while we have focus; only touch the real clip when it is not set yet */
    if (g.on || g.full) { enforce_clip(FALSE); return TRUE; }
    return ClipCursor(rc);
}
static BOOL WINAPI h_GetClientRect(HWND h, RECT *rc)
{
    if (g.on && h == g.hwnd && rc) { SetRect(rc, 0, 0, g.gw, g.gh); return TRUE; }
    return GetClientRect(h, rc);
}
static BOOL WINAPI h_GetWindowRect(HWND h, RECT *rc)
{
    if (g.on && h == g.hwnd && rc) { SetRect(rc, 0, 0, g.gw, g.gh); return TRUE; }
    return GetWindowRect(h, rc);
}
static int WINAPI h_GetSystemMetrics(int i)
{
    if (g.on && i == SM_CXSCREEN) return g.gw;
    if (g.on && i == SM_CYSCREEN) return g.gh;
    return GetSystemMetrics(i);
}

static LRESULT CALLBACK lb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEMOVE)
    {
        DWORD now = GetTickCount();
        /* gaps only count while the mouse is moving (under 250 ms apart) */
        if (st.last_move_tick && now - st.last_move_tick < 250 && now - st.last_move_tick > st.move_gap_max_ms)
            st.move_gap_max_ms = now - st.last_move_tick;
        st.last_move_tick = now;
        st.moves++;
    }
    else if (m == WM_MOUSEWHEEL) st.wheel++;
    else if (m == WM_KEYDOWN && w == 'F' && !(l & 0x40000000) && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000))
        cfg_showfps = !cfg_showfps;                       /* Ctrl+Shift+F: frame-rate counter on/off */
    if (g.on)
    {
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST)
        {
            /* the window sits at 0,0 so client and screen coordinates are the same thing */
            POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
            to_virtual(&pt);
            l = MAKELPARAM(pt.x, pt.y);
        }
    }
    if ((g.on || g.full) && (m == WM_KILLFOCUS || (m == WM_ACTIVATEAPP && !w)))
    {
        if (g.clipped) { ClipCursor(NULL); g.clipped = FALSE; }       /* another program is in front: let the cursor go */
    }
    return g.unicode ? CallWindowProcW(g.prev_proc, h, m, w, l) : CallWindowProcA(g.prev_proc, h, m, w, l);
}

/* Redirect one of the game's imports. Matched by the function's address, not its name: this game.dat's
 * import table was rebuilt without the name list (OriginalFirstThunk = 0), so at run time the table only
 * holds addresses. */
static void iat_hook(HMODULE mod, const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    HMODULE lib = GetModuleHandleA(dll);
    void *target = lib ? (void *)GetProcAddress(lib, name) : NULL;
    int n = 0;
    if (!d.VirtualAddress || !target) { logf("NOT hooked (%s): %s!%s", target ? "no import table" : "not found", dll, name); return; }
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + d.VirtualAddress); imp->Name && imp->FirstThunk; imp++)
    {
        void **iat = (void **)(base + imp->FirstThunk);
        for (; *iat; iat++)
            if (*iat == target) { patch_slot(iat, fn, NULL); n++; }
    }
    logf("%s %s!%s", n ? "hooked" : "NOT hooked (not imported):", dll, name);
}

/* ---- Direct3D ---- */
static HRESULT (WINAPI *o_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static HRESULT (WINAPI *o_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static HRESULT (WINAPI *o_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);

/* ---- even frame pacing ----
 * wined3d hands Present to its command thread and returns at once, so the game runs up to a frame ahead of the screen:
 * on a Mac that is fast enough for the display's refresh rate the frames then leave the game 4..14 ms apart although
 * they average 8.3 ms. The game positions everything by the clock at the start of each frame, so shown one refresh
 * apart those frames judder (birds, animals, anything moving steadily). With the pacing on, Present waits (through an
 * event query) until the command thread has really swapped, which locks the game's frames to the display's refresh.
 * letterbox.ini: syncpresent = -1 auto (on while the game holds the refresh rate, off when it cannot: waiting would
 * then halve the frame rate), 0 off, 1 always; refresh = the display's refresh rate, written by the launch wrapper. */
static int cfg_sync = -1;
static UINT cfg_refresh = 0;
static UINT cfg_syncsleep = 3000;
static int cfg_frameclock = 1;   /* frameclock = 1: regular per-frame clock while the pacing is locked (see below), 0 = off */   /* syncsleep = microseconds before the expected swap up to which the wait may sleep; 0 = never (poll only) */
static IDirect3DQuery9 *g_sync_q;
static BOOL g_sync_on, g_sync_bad, g_vt_valid;
static LARGE_INTEGER g_sync_last;
static DWORD g_sync_waits, g_sync_wait_us, g_sync_late_work, g_sync_late_wait, g_sync_work_max, g_sync_sleeps;
static LARGE_INTEGER g_qpf;

static void sync_release(void) { if (g_sync_q) { IDirect3DQuery9_Release(g_sync_q); g_sync_q = NULL; } }

/* The frame clock. The game (and SAGE Unlocked's scheduler) read timeGetTime at the start of every frame and place
 * everything by it. While the pacing is locked to the display, every frame is shown exactly one refresh after the
 * previous one, but the moment the swap wait returns still wobbles by a few ms. So while locked, timeGetTime is
 * "frame base + real time since the last swap", where the frame base advances by exactly one refresh interval per
 * shown frame (more after a missed refresh) and follows the real clock slowly. Time still flows inside a frame, never
 * runs backwards, and is the plain real clock whenever the pacing is off.
 * The game's own frame limiter (game.dat 0x63a196) must not add waits on top of the display's pace: its reading is
 * always at least one limiter period after its previous one. */
static DWORD (WINAPI *o_timeGetTime)(void);
static double g_vt_ms, g_vt_off;          /* frame base on the timeGetTime scale; (timeGetTime - QPC ms) */
static DWORD g_vt_last_ret, g_vt_resyncs, g_vt_skips, g_vt_lim_last, g_vt_lim_pushed;

static double qpc_ms(const LARGE_INTEGER *t) { return (double)t->QuadPart * 1000.0 / (double)g_qpf.QuadPart; }

static void frame_clock_tick(void)        /* right after a swap, g_sync_last = now */
{
    double interval = cfg_refresh ? 1000.0 / cfg_refresh : 8.333, real, e;
    if (!o_timeGetTime) return;
    if (!g_vt_off) g_vt_off = (double)o_timeGetTime() - qpc_ms(&g_sync_last);
    real = qpc_ms(&g_sync_last) + g_vt_off;
    if (!g_vt_valid) { g_vt_ms = real; g_vt_valid = TRUE; return; }
    g_vt_ms += interval;
    e = real - g_vt_ms;
    /* The swap wait returns anywhere within a frame or so of the refresh that showed the picture (the driver queues
     * a frame ahead), so only a lag that has really built up counts as a missed refresh. */
    if (e > 1.25 * interval && e < 4 * interval) { double n = (double)(int)(e / interval); g_vt_ms += n * interval; e = real - g_vt_ms; g_vt_skips++; }
    if (e < -1.25 * interval || e >= 4 * interval) { g_vt_ms = real; g_vt_resyncs++; return; }
    g_vt_ms += 0.02 * e;                  /* follow the real clock slowly */
}

__attribute__((noinline)) static DWORD WINAPI h_timeGetTime(void)
{
    DWORD ret = o_timeGetTime();
    if (g_sync_on && g_vt_valid && g_sync_last.QuadPart)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        ret = (DWORD)(g_vt_ms + (qpc_ms(&now) - qpc_ms(&g_sync_last)));
        if ((DWORD_PTR)__builtin_return_address(0) == 0x63a19c)       /* the frame limiter's first reading of a frame */
        {
            DWORD lim = cfg_refresh ? 1000 / cfg_refresh : 8;
            if (g_vt_lim_last && (int)(ret - (g_vt_lim_last + lim)) < 0 && (int)(g_vt_lim_last + lim - ret) < 50) { ret = g_vt_lim_last + lim; g_vt_lim_pushed++; }
            g_vt_lim_last = ret;
        }
    }
    else g_vt_lim_last = 0;
    {
        /* Never backwards - but only by a little. Holding the last value while the real clock is far behind it would
         * stop the clock for as long as the difference lasts, and the game's frame limiter waits in a loop on this
         * clock: with a value far ahead handed out once, that loop never ends. So: more than 100 ms ahead of the real
         * clock is a mistake, not a hold; drop the frame clock and go on from the real one. */
        DWORD real = o_timeGetTime();
        if ((int)(ret - real) > 100 || (int)(real - ret) > 100)
        {
            static DWORD said;
            if (said++ < 20) logf("frame clock: %u was %d ms off the real clock %u; dropped", ret, (int)(ret - real), real);
            ret = real; g_vt_valid = FALSE; g_vt_last_ret = real; g_vt_lim_last = 0;
        }
        if ((int)(ret - g_vt_last_ret) < 0) ret = (int)(g_vt_last_ret - ret) <= 100 ? g_vt_last_ret : ret;
    }
    g_vt_last_ret = ret;
    return ret;
}

/* The game's frame limiter (0x63a196 ff.) computes how long a frame should take, 1000 / (fps x factor) ms, and then
 * waits in a loop (Sleep(0), read the clock) until that much time has passed since the previous frame. If that number
 * ever comes out wrong (a bad floating-point result becomes 0x80000000 = 24 days) the game stands still for good with
 * one core busy. No frame is meant to take longer than 100 ms, so the wait is limited to that, and a wrong number is
 * written to the log with what it was made from. */
__attribute__((used)) static DWORD __stdcall lim_target(DWORD ms)
{
    if (ms > 100)
    {
        static DWORD said;
        BYTE *eng = *(BYTE **)0xde4324;
        if (said++ < 20)
            logf("frame limiter: asked to wait %u ms (0x%08x) for one frame; limited to 100. net factor bits %08x, engine fps %d, last frame at %u",
                 ms, ms, *(DWORD *)0xd9f498, eng && !IsBadReadPtr(eng + 0xc, 4) ? *(int *)(eng + 0xc) : -1, *(DWORD *)0xde4318);
        return 100;
    }
    return ms;
}
/* A hook that stands in for a call inside game.dat has to leave the registers exactly as the game's own function
 * would. game.dat was compiled as one unit: a caller knows which registers its callee really uses and keeps values
 * in the others across the call, although the calling convention would let the callee change them (at 0x675ed4 edx
 * stays live across TintEnvelope::play, at 0x675dd1 and 0x675e86 xmm4 does). A plain C function in that place
 * changes them. That was a real crash: the tint hook returned with a frame count in edx, the game wrote a colour
 * through it (access violation at 0x404A21, in both games of a network match at the same tick, 16 Nov 2026).
 * So every such hook is a small assembly routine, and the C part runs between HOOK_SAVE and HOOK_RESTORE, which
 * keep all general registers, the flags and the whole FPU/SSE state (fninit gives the C part an empty FPU stack and
 * default rounding, whatever the game had there; fxrstor puts the game's state back). After HOOK_SAVE ebp points at the saved
 * edi, esi, ebp, esp, ebx, edx, ecx, eax (+0 .. +28), the flags (+32), the return address (+36) and the stack
 * arguments (+40 ...). */
#define HOOK_SAVE    "pushfl\n" "pushal\n" "movl %esp, %ebp\n" "subl $512, %esp\n" "andl $-16, %esp\n" "fxsave (%esp)\n" "fninit\n"
#define HOOK_RESTORE "fxrstor (%esp)\n" "movl %ebp, %esp\n" "popal\n" "popfl\n"

__attribute__((naked)) static void h_LimTarget(void)       /* replaces: mov esi,eax / mov ecx,edi / sub ecx,[0xde4318] */
{
    __asm__(HOOK_SAVE
            "pushl 28(%ebp)\n"               /* eax: the wait in ms */
            "call _lim_target@4\n"           /* stdcall, takes its argument off the stack */
            "movl %eax, 28(%ebp)\n"          /* comes back as eax */
            HOOK_RESTORE
            "movl %eax, %esi\n"
            "movl %edi, %ecx\n"
            "subl 0xde4318, %ecx\n"
            "ret\n");
}

/* When to lock. Locking only helps while the game finishes every frame well inside one refresh; a frame that needs
 * longer then costs two refreshes instead of a little over one, which is worse than not locking (the main menu's
 * scene changes, big battles). So: lock only after 12 s of frames averaging the refresh rate (the menu's light scenes
 * are shorter than that), and let go when the game's own share of a frame exceeds a refresh three times in a row
 * (then not again for 15 s). A single long frame now and then is tolerated. */
static DWORD g_sync_holdoff, g_sync_over;      /* no re-lock before this tick count; recent frames that were too long */

static void sync_eval(DWORD us)   /* once per frame, with the time since the previous frame */
{
    static DWORD n, late, good;
    static double sum;
    DWORD interval;
    if (cfg_sync >= 0 || !cfg_refresh) { g_sync_on = cfg_sync == 1; return; }
    interval = 1000000 / cfg_refresh;
    n++; sum += us; if (us > interval * 5 / 4) late++;
    if (n < 120) return;
    if (!g_sync_on)
    {
        if (sum / n <= interval * 1.012 && (int)(GetTickCount() - g_sync_holdoff) >= 0)
        { if (++good >= 12) { g_sync_on = TRUE; good = 0; g_sync_over = 0; logf("pacing: on (frames average %u us at %u Hz, %u of %u late)", (DWORD)(sum / n), cfg_refresh, late, n); } }
        else good = 0;
    }
    n = late = 0; sum = 0;
}

static void sync_unlock(const char *why, DWORD value)
{
    g_sync_on = FALSE; g_vt_valid = FALSE; g_sync_holdoff = GetTickCount() + 15000;
    logf("pacing: off (%s %u us)", why, value);
}

static void sync_wait(IDirect3DDevice9 *dev)
{
    LARGE_INTEGER a, b;
    BOOL done = FALSE;
    DWORD interval = cfg_refresh ? 1000000 / cfg_refresh : 8333, us = 0;
    if (!g_sync_on || g_sync_bad) return;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (!g_sync_q && FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &g_sync_q)))
    { g_sync_q = NULL; g_sync_bad = TRUE; logf("pacing: event queries are not available, staying off"); return; }
    IDirect3DQuery9_Issue(g_sync_q, D3DISSUE_END);
    QueryPerformanceCounter(&a);
    if (g_sync_last.QuadPart)
    {
        /* the game's own share of this frame: from the previous swap to this Present */
        DWORD work = (DWORD)((a.QuadPart - g_sync_last.QuadPart) * 1000000 / g_qpf.QuadPart);
        if (work > g_sync_work_max) g_sync_work_max = work;
        if (work > interval) g_sync_late_work++;
        if (work > interval)
        {
            /* The game missed this refresh on its own. One such frame now and then is simply shown a refresh later
             * (keep waiting, the frame clock skips ahead with it). When it keeps happening - the main menu's scene
             * changes, a big battle - locking only makes things worse: let go for a while. */
            g_sync_over += 70;
            if (g_sync_over > 200 && cfg_sync < 0) { sync_unlock("the game kept needing more than a refresh, last", work); QueryPerformanceCounter(&g_sync_last); return; }
        }
        else if (g_sync_over) g_sync_over = g_sync_over > 3 ? g_sync_over - 3 : 0;
    }
    for (;;)
    {
        if (IDirect3DQuery9_GetData(g_sync_q, &done, sizeof(done), D3DGETDATA_FLUSH) != S_FALSE) break;
        QueryPerformanceCounter(&b);
        us = (DWORD)((b.QuadPart - a.QuadPart) * 1000000 / g_qpf.QuadPart);
        if (us > 100000) break;                      /* something is wrong (device lost?): do not hang */
        /* sleep while the swap is certainly more than ~3 ms away, then poll closely */
        if (cfg_syncsleep && g_sync_last.QuadPart && (DWORD)((b.QuadPart - g_sync_last.QuadPart) * 1000000 / g_qpf.QuadPart) + cfg_syncsleep < interval) { Sleep(1); g_sync_sleeps++; }
        else Sleep(0);
    }
    QueryPerformanceCounter(&b);
    if (g_sync_last.QuadPart && (DWORD)((b.QuadPart - g_sync_last.QuadPart) * 1000000 / g_qpf.QuadPart) > interval * 3 / 2
        && (DWORD)((a.QuadPart - g_sync_last.QuadPart) * 1000000 / g_qpf.QuadPart) <= interval) g_sync_late_wait++;
    g_sync_last = b;
    frame_clock_tick();
    g_sync_waits++; g_sync_wait_us += (DWORD)((g_sync_last.QuadPart - a.QuadPart) * 1000000 / g_qpf.QuadPart);
}
static HRESULT (WINAPI *o_GetBackBuffer)(IDirect3DDevice9 *, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **);
static HRESULT (WINAPI *o_GetSwapChain)(IDirect3DDevice9 *, UINT, IDirect3DSwapChain9 **);

/* Fill in what the real device gets. Returns TRUE when the picture has to be letterboxed. */
static BOOL prepare(const D3DPRESENT_PARAMETERS *pp, D3DPRESENT_PARAMETERS *real)
{
    DEVMODEW dm;
    *real = *pp;
    if (pp->Windowed) return FALSE;
    real->FullScreen_RefreshRateInHz = 0;          /* never ask for a refresh rate: wined3d then skips the mode set */
    if (cfg_vsync == 1) real->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    else if (cfg_vsync == 0) real->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (pp->BackBufferFormat == D3DFMT_R5G6B5 || pp->BackBufferFormat == D3DFMT_X1R5G5B5 || pp->BackBufferFormat == D3DFMT_A1R5G5B5
        || pp->BackBufferFormat == D3DFMT_UNKNOWN)
        real->BackBufferFormat = D3DFMT_X8R8G8B8;  /* a 16-bit mode would be a mode change too; UNKNOWN = the game found no such mode */
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth || !dm.dmPelsHeight)
    {
        logf("could not read the current display mode; passing the game's parameters through");
        return FALSE;
    }
    g.sw = dm.dmPelsWidth; g.sh = dm.dmPelsHeight;
    g.gw = pp->BackBufferWidth; g.gh = pp->BackBufferHeight;
    real->BackBufferWidth = g.sw; real->BackBufferHeight = g.sh;
    g.full = FALSE;
    if (g.gw && g.gw == g.sw && g.gh == g.sh)
    {
        /* The picture is the screen. With a second monitor next to it the cursor would wander off at that edge (the
         * game confines it once at start and never again), so keep it here the same way as with a letterboxed picture. */
        SetRect(&g.dst, 0, 0, g.sw, g.sh); g.dw = g.sw; g.dh = g.sh; g.full = TRUE;
        return FALSE;
    }
    if (!g.gw || !g.gh) return FALSE;
    {
        UINT inset = cfg_topinset < g.sh / 4 ? cfg_topinset : 0, ah = g.sh - inset;      /* usable height below the inset */
        if ((ULONGLONG)g.gw * ah <= (ULONGLONG)g.sw * g.gh) { g.dh = ah; g.dw = MulDiv(g.gw, ah, g.gh); }
        else { g.dw = g.sw; g.dh = MulDiv(g.gh, g.sw, g.gw); }
        g.dst.left = (g.sw - g.dw) / 2;
        g.dst.top = (g.sh - g.dh) / 2;                     /* centred on the panel ... */
        if ((UINT)g.dst.top < inset || cfg_valign == 1) g.dst.top = inset;   /* ... unless that would run under the camera housing (or valign=1) */
    }
    g.dst.right = g.dst.left + g.dw; g.dst.bottom = g.dst.top + g.dh;
    return TRUE;
}

static void create_rt(IDirect3DDevice9 *dev, const D3DPRESENT_PARAMETERS *real)
{
    HRESULT hr = IDirect3DDevice9_CreateRenderTarget(dev, g.gw, g.gh, real->BackBufferFormat, real->MultiSampleType,
            real->MultiSampleQuality, (real->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) ? TRUE : FALSE, &g.rt, NULL);
    if (FAILED(hr)) { g.rt = NULL; g.on = FALSE; logf("CreateRenderTarget %ux%u failed %08x; letterbox off", g.gw, g.gh, hr); return; }
    hr = IDirect3DDevice9_SetRenderTarget(dev, 0, g.rt);
    logf("letterbox on: game %ux%u -> %ux%u at (%d,%d) inside %ux%u (SetRenderTarget %08x)",
         g.gw, g.gh, g.dw, g.dh, g.dst.left, g.dst.top, g.sw, g.sh, hr);
}

static void drop_rt(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb;
    ULONG left;
    if (!g.rt) return;
    if (SUCCEEDED(o_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
    {
        IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
        IDirect3DSurface9_Release(bb);
    }
    left = IDirect3DSurface9_Release(g.rt);
    if (left) logf("warning: the game still holds %u reference(s) to its back buffer at Reset", left);
    g.rt = NULL;
}

static HRESULT WINAPI lb_GetBackBuffer(IDirect3DDevice9 *dev, UINT sc, UINT idx, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out)
{
    if (g.on && g.rt && sc == 0 && idx == 0 && out)
    {
        IDirect3DSurface9_AddRef(g.rt);
        *out = g.rt;
        return D3D_OK;
    }
    return o_GetBackBuffer(dev, sc, idx, type, out);
}

static HRESULT WINAPI lb_GetSwapChain(IDirect3DDevice9 *dev, UINT i, IDirect3DSwapChain9 **out)
{
    static BOOL said;
    if (!said) { said = TRUE; logf("note: the game called GetSwapChain(%u); swap chain calls are not letterboxed", i); }
    return o_GetSwapChain(dev, i, out);
}

/* ---- test automation ----
 * A text file letterbox.cmd next to the dll is picked up, deleted and run one line at a time:
 *   shot <name>      save the game's frame (1/3 size) to <name>.bmp; shot1 <name> = full size
 *   move <x> <y>     mouse move, in the game's own coordinates
 *   click <x> <y>    move, left button down, up (a few frames apart)
 *   down <x> <y> / up <x> <y>   press / release the left button (moves in between drag)
 *   wheel <notches>  mouse wheel at the last position; negative = towards the user
 *   budget <n>       terrain tiles baked in the frame of a camera jump (see set_jump_budget)
 *   wait <frames>
 * Lets the game be driven and looked at without macOS screen-recording permission. */
static char cmd_buf[2048], *cmd_pos;
static DWORD cmd_wait;
static int click_stage, click_x, click_y, btn_down;

static void post_mouse(UINT msg, WPARAM w, int vx, int vy)
{
    int rx = g.on ? g.dst.left + MulDiv(vx, g.dw, g.gw) : vx;
    int ry = g.on ? g.dst.top + MulDiv(vy, g.dh, g.gh) : vy;
    if (g.hwnd) PostMessageW(g.hwnd, msg, w, MAKELPARAM(rx, ry));
}

static int next_int(const char **p)
{
    int v = 0, neg = 0;
    while (**p == ' ') (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
    return neg ? -v : v;
}

static DWORD next_hex(const char **p)
{
    DWORD v = 0;
    while (**p == ' ') (*p)++;
    for (;; (*p)++)
    {
        char c = **p;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else break;
    }
    return v;
}
/* debugging aids: "peek <hexaddr> <n>" logs n dwords; "watch <hexaddr>" counts, per 10 s log line, how often that dword changed
 * between frames (e.g. a tick counter); "watch 0" stops. */
static DWORD *watch_addr, watch_last, watch_changes;

static char shot_bb_name[64];      /* "shotbb <name>": the whole screen picture (bars, frame-rate counter) of the next frame */
static BOOL shot_from_bb;
static void save_shot(IDirect3DDevice9 *dev, const char *name, int k)
{
    IDirect3DSurface9 *src = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    if (shot_from_bb) { if (FAILED(o_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &src))) return; }
    else if (g.on && g.rt) { src = g.rt; IDirect3DSurface9_AddRef(src); }
    else if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &src))) return;
    IDirect3DSurface9_GetDesc(src, &d);
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(dev, src, sys);
    if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr))
    {
        int w = d.Width / k, h = d.Height / k, stride = (w * 3 + 3) & ~3, x, y, i;
        BITMAPFILEHEADER fh;
        BITMAPINFOHEADER ih;
        BYTE *row = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, stride);
        WCHAR path[MAX_PATH], wname[64];
        HANDLE f;
        DWORD done;
        for (i = 0; name[i] && i < 60; i++) wname[i] = (WCHAR)name[i];
        wname[i] = 0;
        lstrcpyW(path, g_dir); lstrcatW(path, L"\\"); lstrcatW(path, wname); lstrcatW(path, L".bmp");
        f = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (row && f != INVALID_HANDLE_VALUE)
        {
            ZeroMemory(&fh, sizeof(fh)); ZeroMemory(&ih, sizeof(ih));
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + stride * h;
            ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
            WriteFile(f, &fh, sizeof(fh), &done, 0);
            WriteFile(f, &ih, sizeof(ih), &done, 0);
            for (y = h - 1; y >= 0; y--)
            {
                const BYTE *sp = (const BYTE *)lr.pBits + (y * k) * lr.Pitch;
                for (x = 0; x < w; x++) { row[x * 3] = sp[x * k * 4]; row[x * 3 + 1] = sp[x * k * 4 + 1]; row[x * 3 + 2] = sp[x * k * 4 + 2]; }
                WriteFile(f, row, stride, &done, 0);
            }
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (row) HeapFree(GetProcessHeap(), 0, row);
        IDirect3DSurface9_UnlockRect(sys);
    }
    logf("cmd: shot %s (%ux%u fmt %d) hr=%08x", name, d.Width, d.Height, d.Format, hr);
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(src);
}

/* ---- the game's terrain-tile budget ----
 * BFME II bakes the detailed terrain texture of each 16x16-cell tile on the CPU. Normally at most 2 tiles a frame
 * (game.dat 0x4e0a73), but when the camera jumps more than 20 units (minimap click, zoom, cut) it bakes every visible
 * tile in that one frame ("mov dword [esp+8], 0x63" at 0x4e0ad0) -- 30-40 tiles, a visible hitch under emulation.
 * jumpbudget=N in letterbox.ini caps that frame at N tiles; the rest follow at the normal 2 a frame. */
static int cfg_jumpbudget = 0;       /* 0 = leave the game alone */
static void set_jump_budget(int n)
{
    static const BYTE expect[4] = { 0xC7, 0x44, 0x24, 0x08 };
    BYTE *site = (BYTE *)0x4e0ad0;
    DWORD old;
    if (n < 1 || n > 99) n = 99;
    if (IsBadReadPtr(site, 8) || memcmp(site, expect, 4) || site[5] || site[6] || site[7]) { logf("jump budget: unexpected code at 004e0ad0, left alone"); return; }
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) { logf("jump budget: VirtualProtect failed (%u)", GetLastError()); return; }
    site[4] = (BYTE)n;
    VirtualProtect(site, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    logf("jump budget: %d tiles in the frame of a camera jump", n);
}

/* "rec <frames>" in letterbox.cmd: for that many frames append one line per frame to letterbox.rec:
 * time since the recording began (ms), time since the previous frame (us), average brightness of the picture (0-255),
 * textures created in the frame, whether the pacing lock is on. For looking at fades/flashes and stutter over time. */
static DWORD rec_left, rec_texcreates;
static RECT rec_rect; static BOOL rec_has_rect;      /* "rec <frames> x y w h": only that part of the picture */
static HANDLE rec_file = INVALID_HANDLE_VALUE;
static IDirect3DSurface9 *rec_rt, *rec_sys;
static LARGE_INTEGER rec_t0, rec_prev;

static void rec_stop(void)
{
    if (rec_file != INVALID_HANDLE_VALUE) { CloseHandle(rec_file); rec_file = INVALID_HANDLE_VALUE; }
    if (rec_rt) { IDirect3DSurface9_Release(rec_rt); rec_rt = NULL; }
    if (rec_sys) { IDirect3DSurface9_Release(rec_sys); rec_sys = NULL; }
    rec_left = 0;
}

static void rec_frame(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *src = NULL;
    D3DLOCKED_RECT lr;
    LARGE_INTEGER now, f;
    char line[96];
    DWORD done, lum = 0;
    if (!rec_left) return;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
    if (!rec_rt)
    {
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, 64, 36, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rec_rt, NULL)) ||
            FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 36, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &rec_sys, NULL))) { rec_stop(); return; }
        rec_t0 = rec_prev = now;
    }
    if (g.on && g.rt) { src = g.rt; IDirect3DSurface9_AddRef(src); }
    else if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &src))) src = NULL;
    if (src && SUCCEEDED(IDirect3DDevice9_StretchRect(dev, src, rec_has_rect ? &rec_rect : NULL, rec_rt, NULL, D3DTEXF_LINEAR)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rec_rt, rec_sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(rec_sys, &lr, NULL, D3DLOCK_READONLY)))
    {
        DWORD sum = 0; int x, y;
        for (y = 0; y < 36; y++) { const BYTE *p = (const BYTE *)lr.pBits + y * lr.Pitch; for (x = 0; x < 64; x++, p += 4) sum += (p[0] + p[1] * 4 + p[2] * 3) / 8; }
        lum = sum / (64 * 36);
        IDirect3DSurface9_UnlockRect(rec_sys);
    }
    if (src) IDirect3DSurface9_Release(src);
    {
        /* the engine object (game.dat global 0xde4324): logic phase at +34h, drawn interpolation value at +3Ch */
        DWORD phase = 0, interp = 0; void **eng = (void **)0xde4324;
        if (!IsBadReadPtr(eng, 4) && *eng && !IsBadReadPtr(*eng, 0x40)) { phase = *(DWORD *)((BYTE *)*eng + 0x34); interp = (DWORD)(*(float *)((BYTE *)*eng + 0x3c) * 100.0f); }
        wsprintfA(line, "%u %u %u %u %u %u %u\r\n", (DWORD)((now.QuadPart - rec_t0.QuadPart) * 1000 / f.QuadPart),
                  (DWORD)((now.QuadPart - rec_prev.QuadPart) * 1000000 / f.QuadPart), lum, rec_texcreates, (DWORD)g_sync_on, phase, interp);
    }
    WriteFile(rec_file, line, lstrlenA(line), &done, 0);
    rec_prev = now;
    if (!--rec_left) { rec_stop(); logf("cmd: rec finished"); }
}

static void run_commands(IDirect3DDevice9 *dev)
{
    static DWORD frame;
    char *line, *e;
    const char *a;
    frame++;
    if (cmd_wait) { cmd_wait--; return; }
    if (click_stage == 1) { post_mouse(WM_LBUTTONDOWN, MK_LBUTTON, click_x, click_y); click_stage = 2; cmd_wait = 4; return; }
    if (click_stage == 2) { post_mouse(WM_LBUTTONUP, 0, click_x, click_y); click_stage = 0; cmd_wait = 4; return; }
    if (!cmd_pos || !*cmd_pos)
    {
        WCHAR path[MAX_PATH];
        HANDLE f;
        DWORD n = 0;
        if (frame % 15) return;
        lstrcpyW(path, g_dir); lstrcatW(path, L"\\letterbox.cmd");
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (f == INVALID_HANDLE_VALUE) return;
        ReadFile(f, cmd_buf, sizeof(cmd_buf) - 1, &n, 0);
        CloseHandle(f);
        DeleteFileW(path);
        cmd_buf[n] = 0;
        cmd_pos = cmd_buf;
    }
    line = cmd_pos;
    for (e = line; *e && *e != '\n'; e++) ;
    if (*e) { *e = 0; cmd_pos = e + 1; } else cmd_pos = e;
    for (e = line; *e; e++) if (*e == '\r') *e = 0;
    if (!*line) return;
    if (!strncmp(line, "sample ", 7)) { a = line + 7; cfg_sample = next_int(&a); logf("cmd: sample %d", cfg_sample); return; }   /* sampling profiler on/off */
    if (!strncmp(line, "shot ", 5)) { save_shot(dev, line + 5, 3); return; }
    if (!strncmp(line, "shot1 ", 6)) { save_shot(dev, line + 6, 1); return; }
    if (!strncmp(line, "shotbb ", 7)) { lstrcpynA(shot_bb_name, line + 7, sizeof(shot_bb_name)); return; }
    if (!strncmp(line, "wait ", 5)) { a = line + 5; cmd_wait = next_int(&a); return; }
    if (!strncmp(line, "move ", 5)) { a = line + 5; click_x = next_int(&a); click_y = next_int(&a); post_mouse(WM_MOUSEMOVE, btn_down ? MK_LBUTTON : 0, click_x, click_y); logf("cmd: %s", line); return; }
    if (!strncmp(line, "down ", 5)) { a = line + 5; click_x = next_int(&a); click_y = next_int(&a); post_mouse(WM_MOUSEMOVE, 0, click_x, click_y); post_mouse(WM_LBUTTONDOWN, MK_LBUTTON, click_x, click_y); btn_down = 1; logf("cmd: %s", line); return; }
    if (!strncmp(line, "up ", 3)) { a = line + 3; click_x = next_int(&a); click_y = next_int(&a); post_mouse(WM_LBUTTONUP, 0, click_x, click_y); btn_down = 0; logf("cmd: %s", line); return; }
    if (!strncmp(line, "wheel ", 6)) { a = line + 6; post_mouse(WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(next_int(&a) * WHEEL_DELTA)), click_x, click_y); logf("cmd: %s", line); return; }
    if (!strncmp(line, "rec ", 4))
    {
        WCHAR path[MAX_PATH];
        a = line + 4; rec_stop();
        lstrcpyW(path, g_dir); lstrcatW(path, L"\\letterbox.rec");
        rec_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        rec_left = rec_file == INVALID_HANDLE_VALUE ? 0 : (DWORD)next_int(&a);
        { int x = next_int(&a), y = next_int(&a), w = next_int(&a), h = next_int(&a); rec_has_rect = w > 0 && h > 0; if (rec_has_rect) SetRect(&rec_rect, x, y, x + w, y + h); }
        logf("cmd: %s", line);
        return;
    }
    if (!strncmp(line, "peek ", 5))
    {
        DWORD addr, n, i; char out[600]; int len;
        a = line + 5; addr = next_hex(&a); n = (DWORD)next_int(&a); if (n < 1) n = 1; if (n > 48) n = 48;
        len = wsprintfA(out, "peek %08x:", addr);
        if (IsBadReadPtr((void *)(ULONG_PTR)addr, n * 4)) len += wsprintfA(out + len, " unreadable");
        else for (i = 0; i < n; i++) len += wsprintfA(out + len, " %08x", ((DWORD *)(ULONG_PTR)addr)[i]);
        logf("%s", out);
        return;
    }
    if (!strncmp(line, "watch ", 6))
    {
        DWORD addr; a = line + 6; addr = next_hex(&a);
        watch_addr = addr && !IsBadReadPtr((void *)(ULONG_PTR)addr, 4) ? (DWORD *)(ULONG_PTR)addr : NULL;
        watch_changes = 0; if (watch_addr) watch_last = *watch_addr;
        logf("cmd: %s -> %s", line, watch_addr ? "watching" : "off");
        return;
    }
    if (!strncmp(line, "budget ", 7)) { a = line + 7; set_jump_budget(next_int(&a)); return; }
    if (!strncmp(line, "cursor", 6))     /* where the cursor is, what it is confined to, how many monitors there are */
    {
        RECT c = { 0, 0, 0, 0 }; POINT p = { 0, 0 };
        GetClipCursor(&c); GetCursorPos(&p);
        logf("cursor: at %d,%d; confined to %d,%d..%d,%d; desktop %d,%d %dx%d on %d monitor(s); game window in front: %s; picture %d,%d..%d,%d (%s)",
             (int)p.x, (int)p.y, (int)c.left, (int)c.top, (int)c.right, (int)c.bottom,
             GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
             GetSystemMetrics(SM_CMONITORS), GetForegroundWindow() == g.hwnd ? "yes" : "no",
             (int)g.dst.left, (int)g.dst.top, (int)g.dst.right, (int)g.dst.bottom, g.on ? "letterboxed" : g.full ? "fills the screen" : "not set");
        return;
    }
    if (!strncmp(line, "click ", 6))
    {
        a = line + 6; click_x = next_int(&a); click_y = next_int(&a);
        post_mouse(WM_MOUSEMOVE, 0, click_x, click_y);
        click_stage = 1; cmd_wait = 6;
        logf("cmd: %s", line);
        return;
    }
    logf("cmd: unknown '%s'", line);
}

static WCHAR cfg_real[64];           /* real=<file in the game folder>: use that Direct3D 9 library instead of the system's (experiments) */
/* ---- watchdog: when the game's thread has not finished a frame for a few seconds, say where it is ----
 * A second thread looks once a second. After 4 s without a frame (and then every 10 s) it stops the game's thread
 * for a moment, reads its registers and the return addresses on its stack, and writes them to the log. A map load
 * also draws nothing for a while, so the first lines of a match are normal; a freeze shows as lines that never end. */
static HANDLE g_main_thread;
static volatile DWORD g_wd_frames;      /* bumped by the game's thread once per frame */
static void wd_describe(DWORD addr, char *out)
{
    HMODULE m = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    char name[MAX_PATH], *base;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)addr, &m) && m
        && GetModuleFileNameA(m, name, sizeof(name)))
    {
        base = strrchr(name, '\\'); base = base ? base + 1 : name;
        wsprintfA(out, "%s+%x", base, addr - (DWORD)(ULONG_PTR)m);
    }
    else if (VirtualQuery((void *)(ULONG_PTR)addr, &mbi, sizeof(mbi)))
        wsprintfA(out, "allocated memory (base %p, protect %x, type %x)", mbi.AllocationBase, (unsigned)mbi.Protect, (unsigned)mbi.Type);
    else lstrcpyA(out, "unmapped");
}
/* ---- sample=1: where the game's thread and its busiest helper thread spend their time ----
 * The watchdog thread stops a thread every few milliseconds, notes the instruction pointer (and, when that is inside
 * ntdll, the first return address on the stack that is not) and lets it go on. Every 10 s the log gets, for the
 * game's thread and for the other thread that used most processor time: the share of each module, the busiest places
 * (16-byte buckets, named after the nearest export) and the busiest callers into ntdll. */
static struct { DWORD base, size; char name[28]; } smod[64];
static int nsmod;
struct sampler { DWORD total, nomod, modn[64]; struct { DWORD key, n; } bucket[8192], caller[4096]; };
static struct sampler s_main, s_side_s;
static struct { DWORD esp[0x100]; } s_stack;
static int smod_find(DWORD addr, BOOL add)
{
    int i;
    for (i = 0; i < nsmod; i++) if (addr - smod[i].base < smod[i].size) return i;
    if (add && nsmod < 64)
    {
        HMODULE m = NULL; char path[MAX_PATH], *b;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)addr, &m)
            || !m || !GetModuleFileNameA(m, path, sizeof(path))) return -1;
        b = strrchr(path, '\\'); b = b ? b + 1 : path;
        smod[i].base = (DWORD)(ULONG_PTR)m;
        smod[i].size = ((IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew))->OptionalHeader.SizeOfImage;
        lstrcpynA(smod[i].name, b, sizeof(smod[i].name));
        return nsmod++;
    }
    return -1;
}
static void sbucket_add(void *table, DWORD mask, DWORD key)
{
    struct { DWORD key, n; } *tb = table; DWORD h, tries = 0;
    for (h = (key * 2654435761u >> 16) & mask; tries++ <= mask; h = (h + 1) & mask)
    {
        if (tb[h].n && tb[h].key == key) { tb[h].n++; return; }
        if (!tb[h].n) { tb[h].key = key; tb[h].n = 1; return; }
    }
}
/* "module!export+offset" for an address (the nearest export below it), or "module+offset" */
static void s_name(DWORD addr, char *out)
{
    int i = smod_find(addr, TRUE);
    BYTE *base; IMAGE_NT_HEADERS *nt; IMAGE_EXPORT_DIRECTORY *ex; DWORD *names, *funcs, k, best = 0, rva; WORD *ords; const char *bn = NULL;
    if (i < 0) { wsprintfA(out, "%x", addr); return; }
    base = (BYTE *)(ULONG_PTR)smod[i].base; rva = addr - smod[i].base;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size)
    {
        ex = (IMAGE_EXPORT_DIRECTORY *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
        names = (DWORD *)(base + ex->AddressOfNames); funcs = (DWORD *)(base + ex->AddressOfFunctions); ords = (WORD *)(base + ex->AddressOfNameOrdinals);
        for (k = 0; k < ex->NumberOfNames; k++)
        {
            DWORD f = funcs[ords[k]];
            if (f <= rva && f > best) { best = f; bn = (const char *)(base + names[k]); }
        }
    }
    if (bn && rva - best < 0x4000) wsprintfA(out, "%s!%.40s+%x", smod[i].name, bn, rva - best);
    else wsprintfA(out, "%s+%x", smod[i].name, rva);
}
static void sample_add(struct sampler *s, const CONTEXT *c)
{
    int i = smod_find(c->Eip, TRUE);
    s->total++;
    if (i < 0) { s->nomod++; return; }
    s->modn[i]++;
    sbucket_add(s->bucket, 8191, c->Eip >> 4);
    if (!lstrcmpiA(smod[i].name, "ntdll.dll"))
    {
        DWORD k; BOOL inner = FALSE;
        for (k = 0; k < 0x100; k++)       /* who called: the first return address inside ntdll and the first outside it (from the copy of the thread's stack) */
        {
            DWORD v = s_stack.esp[k];
            if (v < 0x10000) continue;
            if (v - smod[i].base < smod[i].size)
            {
                if (!inner && (v >> 4) != (c->Eip >> 4) && k < 0x40) { sbucket_add(s->caller, 4095, v >> 4); inner = TRUE; }
                continue;
            }
            if (smod_find(v, FALSE) >= 0) { sbucket_add(s->caller, 4095, v >> 4); break; }
        }
    }
}
static void s_top(struct sampler *s, void *table, int size, int count, const char *what)
{
    struct { DWORD key, n; } *tb = table; char line[1000], nm[120]; int len = 0, i, k;
    for (k = 0; k < count; k++)
    {
        int best = -1;
        for (i = 0; i < size; i++) if (tb[i].n && (best < 0 || tb[i].n > tb[best].n)) best = i;
        if (best < 0 || tb[best].n * 1000 < s->total * 5 || len > 860) break;          /* below 0.5 % */
        s_name(tb[best].key << 4, nm);
        len += wsprintfA(line + len, " %s %u.%u%%;", nm, tb[best].n * 100 / s->total, tb[best].n * 1000 / s->total % 10);
        tb[best].n = 0;
    }
    if (len) logf("    %s:%s", what, line);
}
static void sampler_report(struct sampler *s, const char *who)
{
    char line[900]; int len = 0, i, k;
    if (s->total)
    {
        for (k = 0; k < 9; k++)
        {
            int best = -1;
            for (i = 0; i < nsmod; i++) if (s->modn[i] && (best < 0 || s->modn[i] > s->modn[best])) best = i;
            if (best < 0) break;
            len += wsprintfA(line + len, " %s %u.%u%%", smod[best].name, s->modn[best] * 100 / s->total, s->modn[best] * 1000 / s->total % 10);
            s->modn[best] = 0;
        }
        logf("  sample: %u samples of %s:%s, outside any module %u.%u%%", s->total, who, line, s->nomod * 100 / s->total, s->nomod * 1000 / s->total % 10);
        s_top(s, s->bucket, 8192, 14, "busiest places");
        s_top(s, s->caller, 4096, 12, "who called into ntdll (inside it, and from outside)");
    }
    ZeroMemory(s, sizeof(*s));
}
/* the helper threads of the Direct3D implementation: the threads that were started inside wined3d.dll (its command
 * thread) or inside the library named by real= */
static HANDLE s_side[6]; static int s_nside; static char s_side_mod[28];
static DWORD s_side_eip, s_side_stack[0x60];      /* the helper thread's last sample inside ntdll: where it was and its stack */
static void pick_side_threads(void)
{
    static LONG (WINAPI *query)(HANDLE, int, void *, ULONG, ULONG *);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId(), main_tid = GetThreadId(g_main_thread);
    char real[64]; int i;
    if (snap == INVALID_HANDLE_VALUE) return;
    if (!query) query = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    for (i = 0; i < s_nside; i++) CloseHandle(s_side[i]);
    s_nside = 0;
    WideCharToMultiByte(CP_ACP, 0, cfg_real, -1, real, sizeof(real), NULL, NULL);
    te.dwSize = sizeof(te);
    if (query && Thread32First(snap, &te)) do
    {
        HANDLE h; DWORD start = 0; int m;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || te.th32ThreadID == main_tid || s_nside == 6) continue;
        if (!(h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))) continue;
        query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), NULL);
        m = start ? smod_find(start, TRUE) : -1;
        if (m >= 0 && (!lstrcmpiA(smod[m].name, "wined3d.dll") || (real[0] && !lstrcmpiA(smod[m].name, real))))
        { s_side[s_nside++] = h; lstrcpynA(s_side_mod, smod[m].name, sizeof(s_side_mod)); }
        else CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}
static BOOL sample_thread(HANDLE th, CONTEXT *c)
{
    ZeroMemory(c, sizeof(*c)); c->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(th) == (DWORD)-1) return FALSE;
    if (!GetThreadContext(th, c)) { ResumeThread(th); return FALSE; }
    {
        /* a thread that waits near the top of its stack has less than the full window above its stack pointer */
        MEMORY_BASIC_INFORMATION mbi; SIZE_T n = 0;
        ZeroMemory(&s_stack, sizeof(s_stack));
        if (c->Esp && VirtualQuery((void *)(ULONG_PTR)c->Esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            n = (SIZE_T)((BYTE *)mbi.BaseAddress + mbi.RegionSize - (BYTE *)(ULONG_PTR)c->Esp);
            if (n > sizeof(s_stack)) n = sizeof(s_stack);
            CopyMemory(&s_stack, (void *)(ULONG_PTR)c->Esp, n & ~3u);
        }
    }
    ResumeThread(th);
    return TRUE;
}
static void sample_tick(DWORD now)
{
    static DWORD last_report;
    CONTEXT c; int i;
    if (!last_report)
    {
        static const char *const pre[] = { "kernelbase.dll", "kernel32.dll", "wined3d.dll", "opengl32.dll", "win32u.dll", "user32.dll", "gdi32.dll", "msvcr71.dll", "ucrtbase.dll", "winemac.drv", "d3dx9_27.dll", "mss32.dll" };
        int k; HMODULE m;
        for (k = 0; k < (int)(sizeof(pre) / sizeof(pre[0])); k++) if ((m = GetModuleHandleA(pre[k]))) smod_find((DWORD)(ULONG_PTR)m + 0x1000, TRUE);
        if ((m = GetModuleHandleA("d3d9.dll"))) smod_find((DWORD)(ULONG_PTR)g_real + 0x1000, TRUE);
        last_report = now; pick_side_threads();
    }
    if (sample_thread(g_main_thread, &c)) sample_add(&s_main, &c);
    for (i = 0; i < s_nside; i++) if (sample_thread(s_side[i], &c))
    {
        int m = smod_find(c.Eip, FALSE);
        sample_add(&s_side_s, &c);
        if (m >= 0 && !lstrcmpiA(smod[m].name, "ntdll.dll")) { s_side_eip = c.Eip; CopyMemory(s_side_stack, s_stack.esp, sizeof(s_side_stack)); }
    }
    if (now - last_report >= 10000)
    {
        char who[200];
        sampler_report(&s_main, "the game's thread");
        wsprintfA(who, "the Direct3D helper thread%s (%d started in %s)", s_nside == 1 ? "" : "s", s_nside, s_nside ? s_side_mod : "-");
        sampler_report(&s_side_s, who);
        if (s_side_eip)
        {
            char line[1000], nm[120]; int len = 0, k, n = 0;
            s_name(s_side_eip, nm); len += wsprintfA(line, "%s <-", nm);
            for (k = 0; k < 0x60 && n < 9 && len < 880; k++)
                if (s_side_stack[k] >= 0x10000 && smod_find(s_side_stack[k], TRUE) >= 0) { s_name(s_side_stack[k], nm); len += wsprintfA(line + len, " %s", nm); n++; }
            logf("    the helper thread's last wait, with the code addresses on its stack: %s", line);
            s_side_eip = 0;
        }
        pick_side_threads();
        last_report = now;
    }
}

static BOOL wd_sample(CONTEXT *c, DWORD *rets, int max, int *n)
{
    DWORD i;
    ZeroMemory(c, sizeof(*c)); c->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER; *n = 0;
    if (SuspendThread(g_main_thread) == (DWORD)-1) return FALSE;
    if (!GetThreadContext(g_main_thread, c)) { ResumeThread(g_main_thread); return FALSE; }
    if (!IsBadReadPtr((void *)(ULONG_PTR)c->Esp, 0x800))
        for (i = 0; i < 0x200 && *n < max; i++)
        {
            DWORD v = ((DWORD *)(ULONG_PTR)c->Esp)[i];
            if (v >= 0x401000 && v < 0xbd0000) rets[(*n)++] = v;      /* looks like a return address into game.dat */
        }
    ResumeThread(g_main_thread);
    return TRUE;
}
static DWORD WINAPI wd_thread(void *arg)
{
    DWORD last = 0, last_change = GetTickCount(), said = 0, stall_lines = 0, last_line = 0, next_long = 4000;
    DWORD eips[8], rets[14], tick = 0; int ne = 0, nr = 0;
    for (;;)
    {
        DWORD now, still;
        Sleep(cfg_sample ? 4 : 20);
        if (!g_main_thread || !g_wd_frames) continue;
        now = GetTickCount();
        if (cfg_sample)
        {
            sample_tick(now);
            if (++tick % 5) continue;                 /* the stall and freeze checks below every 20 ms as before */
        }
        if (g_wd_frames != last)
        {
            still = now - last_change;
            if (said) logf("watchdog: frames again after %u ms", still);
            else if (ne && still >= 100 && stall_lines < 300 && now - last_line > 2000)
            {
                /* a frame that took long: where the game's thread was, sampled every 20 ms from 60 ms on */
                char line[700], where[400]; int len = 0, i;
                for (i = 0; i < ne; i++) len += wsprintfA(line + len, " %x", eips[i]);
                len += wsprintfA(line + len, " | return addresses at the last sample:");
                for (i = 0; i < nr; i++) len += wsprintfA(line + len, " %x", rets[i]);
                wd_describe(eips[ne - 1], where);
                logf("  stall %u ms: the game's thread was at%s | last = %s", still, line, where);
                stall_lines++; last_line = now;
            }
            last = g_wd_frames; last_change = now; said = 0; ne = 0; next_long = 4000;
            continue;
        }
        still = now - last_change;
        if (cfg_watch >= 2 && still >= 60 && ne < 8)
        {
            CONTEXT c;
            if (wd_sample(&c, rets, 14, &nr)) eips[ne++] = c.Eip;
        }
        if (still >= next_long && said < 60)
        {
            CONTEXT c; char where[400], line[400]; int len = 0, i;
            void **eng = (void **)0xde4324;
            next_long = still + 10000;
            logf("watchdog: no frame for %u s (now asking the game's thread where it is; if nothing follows, it could not be stopped)", still / 1000);
            if (!wd_sample(&c, rets, 14, &nr)) { logf("watchdog: no frame for %u s; could not read the game's thread (err %u)", still / 1000, GetLastError()); said++; continue; }
            line[0] = 0;
            for (i = 0; i < nr; i++) len += wsprintfA(line + len, " %x", rets[i]);
            wd_describe(c.Eip, where);
            logf("watchdog: no frame for %u s. The game's thread is at eip=%08x (%s) esp=%08x ebp=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x",
                 still / 1000, c.Eip, where, c.Esp, c.Ebp, c.Eax, c.Ecx, c.Edx, c.Ebx, c.Esi, c.Edi);
            logf("  stack, values inside game.dat's code:%s", line);
            if (!IsBadReadPtr(eng, 4) && *eng && !IsBadReadPtr(*eng, 0x40))
                logf("  engine: phase %u of %u", *(DWORD *)((BYTE *)*eng + 0x34), *(DWORD *)((BYTE *)*eng + 0x38));
            said++;
        }
    }
    return 0;
}
static void wd_start(void)
{
    static BOOL once;
    if (once) return;
    once = TRUE;
    if (!cfg_watch && !cfg_sample) return;
    g_main_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (!g_main_thread || !CreateThread(NULL, 0, wd_thread, NULL, 0, NULL)) logf("watchdog: not started (err %u)", GetLastError());
}

/* ---- profile=1: how much of the game thread's time goes into each Direct3D call ----
 * Every method of the device gets a small thunk (push slot / jmp common) that reads the time-stamp counter before and
 * after the real method; buffer and texture locks are timed in their own hooks. Times are exclusive (a call made from
 * inside another is not counted twice). Only the thread that created the device is measured; other threads go
 * straight through. A report per 10 s window goes to the log. Off unless asked for: the thunks cost frame rate. */
static int cfg_profile;
static int cfg_windowed;             /* windowed=1: the real device is a window that covers the screen (never a full-screen device) */
#define PROF_DEV 119                 /* IDirect3DDevice9 methods */
enum { PROF_VBLOCK = PROF_DEV, PROF_VBUNLOCK, PROF_IBLOCK, PROF_IBUNLOCK, PROF_TEXLOCK, PROF_TEXUNLOCK, PROF_VBFILL, PROF_IBFILL, PROF_N };
static const char *const prof_names[PROF_N] = {
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem", "EvictManagedResources", "GetDirect3D", "GetDeviceCaps",
    "GetDisplayMode", "GetCreationParameters", "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain",
    "GetNumberOfSwapChains", "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode", "SetGammaRamp", "GetGammaRamp", "CreateTexture",
    "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
    "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "StretchRect", "ColorFill", "CreateOffscreenPlainSurface",
    "SetRenderTarget", "GetRenderTarget", "SetDepthStencilSurface", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform",
    "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight", "LightEnable",
    "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "CreateStateBlock", "BeginStateBlock", "EndStateBlock",
    "SetClipStatus", "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState",
    "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette", "SetScissorRect",
    "GetScissorRect", "SetSoftwareVertexProcessing", "GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode", "DrawPrimitive",
    "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexDeclaration", "SetVertexDeclaration",
    "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader", "GetVertexShader", "SetVertexShaderConstantF",
    "GetVertexShaderConstantF", "SetVertexShaderConstantI", "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB",
    "SetStreamSource", "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices", "CreatePixelShader",
    "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF", "GetPixelShaderConstantF", "SetPixelShaderConstantI", "GetPixelShaderConstantI",
    "SetPixelShaderConstantB", "GetPixelShaderConstantB", "DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery",
    "VertexBuffer::Lock", "VertexBuffer::Unlock", "IndexBuffer::Lock", "IndexBuffer::Unlock", "Texture::LockRect", "Texture::UnlockRect",
    "(game writing vertices)", "(game writing indices)" };
__attribute__((used)) static void *prof_orig[PROF_DEV];
__attribute__((used)) static DWORD prof_tid;
static struct { unsigned long long ticks; DWORD calls; } prof_acc[PROF_N];
static struct { void *ret; DWORD slot; unsigned long long t0, child; } prof_stack[64];
static int prof_sp, prof_fill_depth;
static unsigned long long prof_top_ticks, prof_fill_t0, prof_fill_top0;    /* prof_top_ticks: all time inside measured calls so far */
static DWORD prof_overhead_ticks, prof_thunked;                             /* per thunked call (measured once); thunked calls so far */
static DWORD g_tsc_per_us_x16;                                              /* time-stamp counter ticks per microsecond, x16 */
static inline unsigned long long tsc(void) { return __builtin_ia32_rdtsc(); }
static inline DWORD cur_tid(void) { DWORD t; __asm__("movl %%fs:0x24, %0" : "=r"(t)); return t; }
static DWORD ticks_us(unsigned long long ticks) { return g_tsc_per_us_x16 ? (DWORD)(ticks * 16 / g_tsc_per_us_x16) : 0; }

__attribute__((used)) static void * __cdecl prof_enter(DWORD slot, void *ret)
{
    int i = prof_sp++;
    prof_stack[i & 63].ret = ret; prof_stack[i & 63].slot = slot; prof_stack[i & 63].child = 0; prof_thunked++;
    prof_stack[i & 63].t0 = tsc();
    return prof_orig[slot];
}
__attribute__((used)) static void * __cdecl prof_leave(void)
{
    unsigned long long dt = tsc();
    int i = --prof_sp & 63;
    dt -= prof_stack[i].t0;
    prof_acc[prof_stack[i].slot].ticks += dt - prof_stack[i].child;
    prof_acc[prof_stack[i].slot].calls++;
    if (prof_sp > 0) prof_stack[(prof_sp - 1) & 63].child += dt; else prof_top_ticks += dt;
    return prof_stack[i].ret;
}
/* timed sections inside this file's own hooks (same accounting as the thunks) */
static void prof_begin(DWORD slot)
{
    if (cfg_profile && cur_tid() == prof_tid)
    { int i = prof_sp++ & 63; prof_stack[i].ret = 0; prof_stack[i].slot = slot; prof_stack[i].child = 0; prof_stack[i].t0 = tsc(); }
}
static void prof_end(void) { if (cfg_profile && cur_tid() == prof_tid) prof_leave(); }
/* the stretch between a buffer's Lock returning and its Unlock: the game writing vertices (less any calls in between) */
static void prof_fill_begin(void)
{
    if (cfg_profile && cur_tid() == prof_tid && prof_fill_depth++ == 0) { prof_fill_top0 = prof_top_ticks; prof_fill_t0 = tsc(); }
}
static void prof_fill_end(DWORD slot)
{
    if (cfg_profile && cur_tid() == prof_tid && prof_fill_depth > 0 && --prof_fill_depth == 0)
    { prof_acc[slot].ticks += (tsc() - prof_fill_t0) - (prof_top_ticks - prof_fill_top0); prof_acc[slot].calls++; }
}
__attribute__((naked, used)) static void prof_common(void)
{
    __asm__(
        "movl %fs:0x24, %eax\n"
        "cmpl _prof_tid, %eax\n"
        "jne 1f\n"
        "popl %ecx\n"                       /* slot */
        "popl %edx\n"                       /* the caller's return address */
        "pushl %edx\n"
        "pushl %ecx\n"
        "call _prof_enter\n"                /* -> the real method */
        "addl $8, %esp\n"
        "pushl $2f\n"
        "jmp *%eax\n"
        "1: popl %ecx\n"                    /* another thread: straight through */
        "movl _prof_orig(,%ecx,4), %eax\n"
        "jmp *%eax\n"
        "2: pushl %eax\n"                   /* the method's result */
        "call _prof_leave\n"                /* -> the caller's return address */
        "movl %eax, %edx\n"
        "popl %eax\n"
        "jmp *%edx\n");
}
static HRESULT WINAPI prof_dummy(void *p) { return (HRESULT)(ULONG_PTR)p; }
static void prof_install(void **vt)
{
    static BYTE *code;
    HRESULT (WINAPI *volatile f)(void *);
    unsigned long long a, b, c;
    LARGE_INTEGER q;
    int i;
    if (code || !cfg_profile) return;
    code = VirtualAlloc(NULL, PROF_DEV * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return;
    prof_tid = GetCurrentThreadId();
    for (i = 0; i < PROF_DEV; i++)
    {
        BYTE *p = code + i * 16;
        p[0] = 0x68; *(DWORD *)(p + 1) = i;                                          /* push slot */
        p[5] = 0xE9; *(DWORD *)(p + 6) = (DWORD)((BYTE *)prof_common - (p + 10));    /* jmp prof_common */
        prof_orig[i] = i < 3 ? (void *)prof_dummy : vt[i];
    }
    FlushInstructionCache(GetCurrentProcess(), code, PROF_DEV * 16);
    /* what one thunked call costs, and what QueryPerformanceCounter costs, in time-stamp ticks */
    f = prof_dummy; a = tsc(); for (i = 0; i < 20000; i++) f(0);
    f = (void *)code; b = tsc(); for (i = 0; i < 20000; i++) f(0);
    c = tsc(); for (i = 0; i < 2000; i++) QueryPerformanceCounter(&q);
    prof_overhead_ticks = (DWORD)(((c - b) - (b - a)) / 20000);
    logf("profile: on. One thunked call costs %u time-stamp ticks more than a plain one (plain %u); QueryPerformanceCounter costs %u ticks",
         prof_overhead_ticks, (DWORD)((b - a) / 20000), (DWORD)((tsc() - c) / 2000));
    ZeroMemory(prof_acc, sizeof(prof_acc)); prof_top_ticks = 0; prof_thunked = 0;
    for (i = 3; i < PROF_DEV; i++) patch_slot(&vt[i], code + i * 16, NULL);
}
/* per 10 s window: calibrate the time-stamp counter against the clock; with profile=1 write the report */
static void prof_window(DWORD frames)
{
    static unsigned long long last_tsc; static LARGE_INTEGER last_q;
    unsigned long long now = tsc(), d3d = 0, fill, over;
    LARGE_INTEGER q, fq;
    DWORD us, order[PROF_N], n = 0, i, j, calls = 0;
    char line[1000]; int len = 0;
    QueryPerformanceCounter(&q); QueryPerformanceFrequency(&fq);
    us = last_q.QuadPart ? (DWORD)((q.QuadPart - last_q.QuadPart) * 1000000 / fq.QuadPart) : 0;
    if (us > 1000000) g_tsc_per_us_x16 = (DWORD)((now - last_tsc) * 16 / us);
    last_tsc = now; last_q = q;
    if (!cfg_profile || !frames || !g_tsc_per_us_x16 || !prof_tid) { ZeroMemory(prof_acc, sizeof(prof_acc)); prof_thunked = 0; return; }
    for (i = 0; i < PROF_N; i++) if (prof_acc[i].calls) { order[n++] = i; if (i < PROF_VBFILL) { d3d += prof_acc[i].ticks; calls += prof_acc[i].calls; } }
    for (i = 1; i < n; i++) for (j = i; j > 0 && prof_acc[order[j]].ticks > prof_acc[order[j - 1]].ticks; j--) { DWORD x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
    fill = prof_acc[PROF_VBFILL].ticks + prof_acc[PROF_IBFILL].ticks;
    over = (unsigned long long)prof_thunked * prof_overhead_ticks;
    logf("  profile: frame %u us = Direct3D calls %u us (%u calls a frame) + game writing vertices/indices %u us + measuring %u us + everything else (game code) %u us",
         us / frames, ticks_us(d3d) / frames, calls / frames, ticks_us(fill) / frames, ticks_us(over) / frames,
         us / frames - ticks_us(d3d + fill + over) / frames);
    for (i = 0; i < n && i < 18; i++)
    {
        len += wsprintfA(line + len, "%s%s %u us (%u x)", i ? ", " : "", prof_names[order[i]], ticks_us(prof_acc[order[i]].ticks) / frames, prof_acc[order[i]].calls / frames);
        if (len > 850) break;
    }
    logf("    per frame: %s", line);
    for (len = 0, i = 18; i < n; i++)          /* the rest: how often per frame (x100 when rarer than once a frame) */
    {
        DWORD c100 = (DWORD)((unsigned long long)prof_acc[order[i]].calls * 100 / frames);
        if (c100 >= 100) len += wsprintfA(line + len, "%s%s x%u", len ? ", " : "", prof_names[order[i]], c100 / 100);
        else len += wsprintfA(line + len, "%s%s x0.%02u", len ? ", " : "", prof_names[order[i]], c100);
        if (len > 900) break;
    }
    if (len) logf("    also called: %s", line);
    ZeroMemory(prof_acc, sizeof(prof_acc)); prof_thunked = 0;
}

/* ---- spinwait=<microseconds>: the Direct3D layer's two threads hand work to each other without going to sleep ----
 * Wine's Direct3D runs the real drawing on a second thread. For a 32-bit game every vertex-buffer Lock and Unlock is
 * sent to that thread and waited for, well over a thousand times a frame in a battle. The helper thread goes to
 * sleep as soon as it has nothing queued, and waking it costs tens of microseconds each time: the game's thread
 * spent half its time waiting for that. With spinwait the helper thread stays awake (spinning) for up to that many
 * microseconds before it really sleeps, and the game's thread waits by spinning instead of by yield system calls. */
static int cfg_spinwait;
static LONG (WINAPI *o_w3d_Delay)(BOOLEAN, const LARGE_INTEGER *);
static DWORD g_spin_cv, g_spin_cv_slept, g_spin_delay, g_main_tid;
/* wined3d's command thread sleeps through a function pointer it looks up in ntdll at start (NtWaitForAlertByThreadId;
 * the pointer is a global read in wined3d_cs_run: mov eax,[ptr] / test eax,eax / je / push ecx / push 0 / call eax).
 * The replacement first polls with a zero timeout for up to the budget, then sleeps as before. */
static LONG (WINAPI *o_w3d_WaitAlert)(const void *, const LARGE_INTEGER *);
static LONG WINAPI h_w3d_WaitAlert(const void *addr, const LARGE_INTEGER *timeout)
{
    static const LARGE_INTEGER zero;
    unsigned long long budget = (unsigned long long)g_tsc_per_us_x16 * cfg_spinwait / 16;
    if (budget && (!timeout || timeout->QuadPart))
    {
        unsigned long long start = tsc();
        do
        {
            LONG r = o_w3d_WaitAlert(addr, &zero);
            int i;
            if (r != 0x102 /* STATUS_TIMEOUT */) { g_spin_cv++; return r; }
            for (i = 0; i < 12; i++) YieldProcessor();
        } while (tsc() - start < budget);
        g_spin_cv_slept++;
    }
    return o_w3d_WaitAlert(addr, timeout);
}
static BOOL spin_hook_wait(BYTE *base)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 20, *hit = NULL;
    void **slot;
    for (; p < end; p++)
        if (p[0] == 0xA1 && p[5] == 0x85 && p[6] == 0xC0 && p[7] == 0x0F && p[8] == 0x84 && p[13] == 0x51 && p[14] == 0x6A && p[15] == 0x00 && p[16] == 0xFF && p[17] == 0xD0)
        { if (hit) return FALSE; hit = p; }                 /* must be the only place */
    if (!hit) return FALSE;
    slot = *(void ***)(hit + 1);
    if (IsBadWritePtr(slot, 4) || !*slot) return FALSE;
    o_w3d_WaitAlert = *slot;
    *slot = h_w3d_WaitAlert;
    return TRUE;
}
static LONG WINAPI h_w3d_Delay(BOOLEAN alertable, const LARGE_INTEGER *timeout)
{
    static unsigned long long start, last;
    if (cfg_spinwait && !alertable && timeout && !timeout->QuadPart && g_tsc_per_us_x16 && cur_tid() == g_main_tid)
    {
        unsigned long long now = tsc();
        if (now - last > (unsigned long long)g_tsc_per_us_x16 * 4 / 16) start = now;
        if (now - start < (unsigned long long)g_tsc_per_us_x16 * 2000 / 16)      /* up to 2 ms of spinning, then real yields */
        {
            int i;
            for (i = 0; i < 8; i++) YieldProcessor();
            g_spin_delay++;
            last = tsc();
            return 0;
        }
        last = now;
    }
    return o_w3d_Delay(alertable, timeout);
}
static void spin_install(void)
{
    static BOOL once;
    HMODULE w = GetModuleHandleA("wined3d.dll");
    if (once || !cfg_spinwait || !w) return;
    once = TRUE;
    g_main_tid = GetCurrentThreadId();
    o_w3d_Delay = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtDelayExecution");
    if (o_w3d_Delay) iat_hook(w, "ntdll.dll", "NtDelayExecution", h_w3d_Delay);
    logf("spinwait: the Direct3D helper thread %s", spin_hook_wait((BYTE *)w) ? "polls before it sleeps" : "could not be hooked (its wait was not found)");
}

/* ---- noglcheck=1: Wine's Direct3D stops asking OpenGL for an error code after every call ----
 * wined3d wraps each OpenGL call in checkGLcall(), which calls glGetError() whenever error messages of its "d3d"
 * debug channel are on (they are by default) and the driver has no debug-output extension (macOS has none). Under
 * the emulator every OpenGL call is a costly crossing, and this doubles their number. The check only ever prints a
 * message. Clearing the channel's flags in wined3d's memory turns it off (the same as WINEDEBUG=-d3d, which this
 * runner does not start with). */
static int cfg_noglcheck = 1;
static void noglcheck_install(void)
{
    static BOOL once;
    BYTE *base = (BYTE *)GetModuleHandleA("wined3d.dll");
    IMAGE_NT_HEADERS *nt; IMAGE_SECTION_HEADER *sec; int i, n = 0;
    if (once || !cfg_noglcheck || !base) return;
    once = TRUE;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        BYTE *p, *end;
        if (memcmp(sec->Name, ".data", 6)) continue;
        p = base + sec->VirtualAddress; end = p + sec->Misc.VirtualSize - 16;
        for (; p < end; p += 8)                 /* struct __wine_debug_channel { unsigned char flags; char name[15]; } */
        {
            static const BYTE name[15] = "d3d";
            if (memcmp(p + 1, name, 15) || (p[0] != 0xff && p[0] > 0x0f)) continue;
            p[0] = 0; n++;
        }
    }
    logf("noglcheck: per-call OpenGL error checks of the Direct3D layer switched off (%d debug-channel entries)", n);
}

/* ---- heapkeep=1: the game's heap area stays accessible (cure for the total freeze) ----
 * The game's memory manager reserves large address ranges (VirtualAlloc MEM_RESERVE), commits them in steps as it
 * grows and decommits them again when it shrinks. Now and then game code writes through a pointer into a part of
 * such a range that is not committed (any more). On Windows that is an access violation. Under the FEX emulator it
 * never arrives: for a program without data-execution protection, like this one, FEX takes every write fault in
 * readable-writable memory for its own write protection of translated code, "repairs" the page protection without
 * looking at the result and runs the instruction again - for ever (the frozen game: one thread at 54,000 faults a
 * second on the same address). With heapkeep the ranges are committed as soon as they are reserved, and a decommit
 * throws the contents away but leaves the pages accessible, so such a write lands in memory nobody uses. Untouched
 * committed pages cost no real memory. */
static int cfg_heapkeep = 1;
static DWORD g_heap_reserved_kb, g_heap_decommits;
static LPVOID WINAPI h_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    if (cfg_heapkeep && (type & MEM_RESERVE) && !(type & MEM_COMMIT) && prot == PAGE_READWRITE)
    {
        LPVOID p = VirtualAlloc(addr, size, type | MEM_COMMIT, prot);
        if (p) { g_heap_reserved_kb += (DWORD)(size >> 10); return p; }
    }
    return VirtualAlloc(addr, size, type, prot);
}
/* ranges the game reserved before this DLL was loaded: commit what is still only reserved (large private
 * read-write reservations only: not thread stacks, not the small reserves of the system heaps) */
static void heapkeep_scan(void)
{
    static BOOL once;
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *p = (BYTE *)0x10000;
    DWORD kb = 0, n = 0;
    if (once || !cfg_heapkeep) return;
    once = TRUE;
    while ((ULONG_PTR)p < 0x7ffe0000 && VirtualQuery(p, &mbi, sizeof(mbi)))
    {
        if (mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE && mbi.AllocationProtect == PAGE_READWRITE && mbi.RegionSize >= 0x800000
            && VirtualAlloc(mbi.BaseAddress, mbi.RegionSize, MEM_COMMIT, PAGE_READWRITE))
        { kb += (DWORD)(mbi.RegionSize >> 10); n++; }
        p = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    g_heap_reserved_kb += kb;
    logf("heapkeep: %u reserved range(s) from before, %u MB, committed", n, kb >> 10);
}
static BOOL WINAPI h_VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    if (cfg_heapkeep && type == MEM_DECOMMIT && addr && size)
    {
        BOOL r = VirtualFree(addr, size, MEM_DECOMMIT);
        if (r) { VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE); g_heap_decommits++; }
        return r;
    }
    return VirtualFree(addr, size, type);
}

/* ---- cswait=1 (diagnostic): how the Direct3D layer's threads wait, by the function they wait in ---- */
static int cfg_cswait;
static struct { unsigned long long ticks[2]; DWORD calls[2], slow[2]; } g_csw[4];      /* [function][0 = game's thread, 1 = others] */
static LONG (WINAPI *o_csw_NtWait)(HANDLE, BOOLEAN, const LARGE_INTEGER *);
static DWORD (WINAPI *o_csw_Wait)(HANDLE, DWORD);
static void (WINAPI *o_csw_Enter)(CRITICAL_SECTION *);
static void csw_note(int f, unsigned long long t0)
{
    unsigned long long dt = tsc() - t0; int who = cur_tid() != g_main_tid;
    g_csw[f].ticks[who] += dt; g_csw[f].calls[who]++;
    if (g_tsc_per_us_x16 && dt > (unsigned long long)g_tsc_per_us_x16 * 20 / 16) g_csw[f].slow[who]++;      /* took more than 20 us */
}
static LONG WINAPI h_csw_NtWait(HANDLE h, BOOLEAN a, const LARGE_INTEGER *to) { unsigned long long t0 = tsc(); LONG r = o_csw_NtWait(h, a, to); csw_note(0, t0); return r; }
static DWORD WINAPI h_csw_Wait(HANDLE h, DWORD ms) { unsigned long long t0 = tsc(); DWORD r = o_csw_Wait(h, ms); csw_note(1, t0); return r; }
static void WINAPI h_csw_Enter(CRITICAL_SECTION *cs) { unsigned long long t0 = tsc(); o_csw_Enter(cs); csw_note(2, t0); }
static BOOL (WINAPI *o_csw_SleepCV)(CONDITION_VARIABLE *, CRITICAL_SECTION *, DWORD);
static DWORD g_csw_cv_ms;
static BOOL WINAPI h_csw_SleepCV(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) { unsigned long long t0 = tsc(); BOOL r = o_csw_SleepCV(cv, cs, ms); g_csw_cv_ms = ms; csw_note(3, t0); return r; }
static void cswait_install(void)
{
    static BOOL once;
    HMODULE w = GetModuleHandleA("wined3d.dll");
    if (once || !cfg_cswait || !w) return;
    once = TRUE;
    if (!g_main_tid) g_main_tid = GetCurrentThreadId();
    o_csw_NtWait = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtWaitForSingleObject");
    o_csw_Wait = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "WaitForSingleObject");
    o_csw_Enter = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "EnterCriticalSection");
    iat_hook(w, "ntdll.dll", "NtWaitForSingleObject", h_csw_NtWait);
    iat_hook(w, "kernel32.dll", "WaitForSingleObject", h_csw_Wait);
    iat_hook(w, "kernel32.dll", "EnterCriticalSection", h_csw_Enter);
    o_csw_SleepCV = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SleepConditionVariableCS");
    iat_hook(w, "kernel32.dll", "SleepConditionVariableCS", h_csw_SleepCV);
}
static void cswait_report(DWORD frames)
{
    static const char *const nm[4] = { "NtWaitForSingleObject", "WaitForSingleObject", "EnterCriticalSection", "SleepConditionVariableCS" };
    char line[600]; int len = 0, f, w;
    if (!cfg_cswait || !frames) return;
    for (f = 0; f < 4; f++) for (w = 0; w < 2; w++) if (g_csw[f].calls[w])
        len += wsprintfA(line + len, " %s by %s: %u calls, %u us, %u over 20 us;", nm[f], w ? "the helper thread" : "the game's thread",
                         g_csw[f].calls[w] / frames, ticks_us(g_csw[f].ticks[w]) / frames, g_csw[f].slow[w] / frames);
    logf("  cswait, per frame inside wined3d:%s (last condition-variable timeout %u ms)", line, g_csw_cv_ms);
    ZeroMemory(g_csw, sizeof(g_csw));
}

/* ---- where a frame's time goes (for the slow-frame lines in the log) ---- */
static struct {
    LARGE_INTEGER frame_start, scene_start;
    unsigned long long lock_ret_tsc;
    DWORD scene_us, scenes, draws, vb_locks, vb_lock_ticks, vb_fill_ticks, vb_creates, tex_creates, first_scene_us;
    DWORD tex_create_us, tex_locks, tex_lock_call_us, tex_fill_us, tex_unlock_us;
    DWORD dx_filter, dx_filter_us, dx_loadmem, dx_loadmem_us, dx_loadsurf, dx_loadsurf_us, dx_filter_flags, dx_loadmem_fmt, dx_loadmem_filter;
    LARGE_INTEGER tex_lock_ret, tex_created;
    DWORD tile_fill_us;
    struct { WORD w, h, levels, fmt; DWORD usage, pool, us, tid; } tex[6];
} fr;
static LARGE_INTEGER g_freq;
static DWORD us_between(const LARGE_INTEGER *a, const LARGE_INTEGER *b) { return (DWORD)((b->QuadPart - a->QuadPart) * 1000000 / g_freq.QuadPart); }

static HRESULT (WINAPI *o_BeginScene)(IDirect3DDevice9 *);
static HRESULT (WINAPI *o_EndScene)(IDirect3DDevice9 *);
static HRESULT (WINAPI *o_DIP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
static HRESULT (WINAPI *o_DP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static HRESULT (WINAPI *o_CreateVB)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
static HRESULT (WINAPI *o_CreateTex)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
static HRESULT (WINAPI *o_VBLock)(IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD);
static HRESULT (WINAPI *o_VBUnlock)(IDirect3DVertexBuffer9 *);

static HRESULT WINAPI lb_BeginScene(IDirect3DDevice9 *dev)
{
    QueryPerformanceCounter(&fr.scene_start);
    if (!fr.scenes && fr.frame_start.QuadPart) fr.first_scene_us = us_between(&fr.frame_start, &fr.scene_start);
    return o_BeginScene(dev);
}
static HRESULT WINAPI lb_EndScene(IDirect3DDevice9 *dev)
{
    HRESULT hr = o_EndScene(dev);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    fr.scene_us += us_between(&fr.scene_start, &t);
    fr.scenes++;
    return hr;
}
static DWORD g_holds, g_hold_frames, g_hold_longest, g_ticks_seen;   /* per window: ticks the engine held back */
static DWORD g_win_draws, g_win_vblocks, g_win_iblocks;      /* per 10 s window, for the fps line */
/* ---- the spinning ring of the loading screens ----
 * data\movies\loadingring.vp6 (128x128) is drawn as a square of the game's 4:3 layout, which the game stretches over
 * the wide picture: an oval ring. (The medallion around it is part of the loading screens' frame texture, which
 * widescreen-art/make-art.py rebuilds with a round one.) The texture the video is decoded into is recognised when it
 * is created right after the game opened that file. The quad drawn with it - two triangles with identity view and
 * projection, like all of the game's 2D drawing - gets its width multiplied by (4/3) / picture shape about the middle
 * of the screen, which is where the medallion is. ringround=0 switches it off. */
static int cfg_ringround = 1;
static DWORD g_ring_opened;                      /* tick count (never 0) when the game opened the ring video */
static IDirect3DTexture9 *g_ring_tex;            /* the texture its frames go into */
static DWORD g_ring_locked;                      /* number of the frame in which that texture was last written */
static DWORD g_frame_no;
static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (name)
    {
        int n = lstrlenA(name);
        if (n >= 15 && !lstrcmpiA(name + n - 15, "loadingring.vp6")) g_ring_opened = GetTickCount() | 1;
    }
    return CreateFileA(name, access, share, sa, disp, flags, tmpl);
}
static BOOL is_identity(IDirect3DDevice9 *dev, D3DTRANSFORMSTATETYPE which)
{
    static const D3DMATRIX id = {{{ 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }}};
    D3DMATRIX m;
    return SUCCEEDED(IDirect3DDevice9_GetTransform(dev, which, &m)) && !memcmp(&m, &id, sizeof(m));
}
static HRESULT ring_draw(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT start, UINT prims)
{
    static int said;
    IDirect3DBaseTexture9 *cur = NULL;
    D3DMATRIX world, m;
    HRESULT hr;
    float k;
    if (!g.gw || !g.gh || FAILED(IDirect3DDevice9_GetTexture(dev, 0, &cur))) return o_DIP(dev, t, base, minv, nv, start, prims);
    if (cur) IDirect3DBaseTexture9_Release(cur);
    k = (4.0f / 3.0f) * (float)g.gh / (float)g.gw;
    if (cur != (IDirect3DBaseTexture9 *)g_ring_tex || k > 0.999f) return o_DIP(dev, t, base, minv, nv, start, prims);
    if (!is_identity(dev, D3DTS_VIEW) || !is_identity(dev, D3DTS_PROJECTION) || FAILED(IDirect3DDevice9_GetTransform(dev, D3DTS_WORLD, &world)))
    {
        if (!(said & 2)) { said |= 2; logf("ring video: its quad is not drawn the way 2D drawing is (view or projection set), left as it is"); }
        return o_DIP(dev, t, base, minv, nv, start, prims);
    }
    m = world; m._11 *= k; m._21 *= k; m._31 *= k; m._41 *= k;      /* x of the result times k */
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &m);
    hr = o_DIP(dev, t, base, minv, nv, start, prims);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &world);
    if (!(said & 1)) { said |= 1; logf("ring video: drawn at %d/1000 of its width (round on this picture shape)", (int)(k * 1000)); }
    return hr;
}
static HRESULT WINAPI lb_DIP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT start, UINT prims)
{
    fr.draws++; g_win_draws++;
    if (g_ring_tex && prims == 2 && nv == 4 && g_frame_no - g_ring_locked < 4) return ring_draw(dev, t, base, minv, nv, start, prims);
    return o_DIP(dev, t, base, minv, nv, start, prims);
}
static HRESULT WINAPI lb_DP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT start, UINT prims)
{ fr.draws++; g_win_draws++; return o_DP(dev, t, start, prims); }
/* The game locks a vertex buffer well over a thousand times a frame in a battle, so these two must stay cheap: the
 * time-stamp counter, not QueryPerformanceCounter (a call into the emulated system DLLs each time). */
static HRESULT WINAPI lb_VBLock(IDirect3DVertexBuffer9 *vb, UINT off, UINT size, void **data, DWORD flags)
{
    unsigned long long t = tsc();
    HRESULT hr;
    prof_begin(PROF_VBLOCK);
    hr = o_VBLock(vb, off, size, data, flags);
    prof_end();
    fr.lock_ret_tsc = tsc();
    fr.vb_lock_ticks += (DWORD)(fr.lock_ret_tsc - t);
    fr.vb_locks++; g_win_vblocks++;
    prof_fill_begin();
    return hr;
}
static HRESULT WINAPI lb_VBUnlock(IDirect3DVertexBuffer9 *vb)
{
    HRESULT hr;
    if (fr.lock_ret_tsc) fr.vb_fill_ticks += (DWORD)(tsc() - fr.lock_ret_tsc);
    prof_fill_end(PROF_VBFILL);
    prof_begin(PROF_VBUNLOCK);
    hr = o_VBUnlock(vb);
    prof_end();
    return hr;
}
static HRESULT (WINAPI *o_IBLock)(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD);
static HRESULT (WINAPI *o_IBUnlock)(IDirect3DIndexBuffer9 *);
static HRESULT (WINAPI *o_CreateIB)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
static HRESULT WINAPI lb_IBLock(IDirect3DIndexBuffer9 *ib, UINT off, UINT size, void **data, DWORD flags)
{
    HRESULT hr;
    g_win_iblocks++;
    prof_begin(PROF_IBLOCK);
    hr = o_IBLock(ib, off, size, data, flags);
    prof_end();
    prof_fill_begin();
    return hr;
}
static HRESULT WINAPI lb_IBUnlock(IDirect3DIndexBuffer9 *ib)
{
    HRESULT hr;
    prof_fill_end(PROF_IBFILL);
    prof_begin(PROF_IBUNLOCK);
    hr = o_IBUnlock(ib);
    prof_end();
    return hr;
}
static HRESULT WINAPI lb_CreateIB(IDirect3DDevice9 *dev, UINT len, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *h)
{
    HRESULT hr = o_CreateIB(dev, len, usage, f, pool, out, h);
    if (SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out;
        patch_slot(&vt[11], lb_IBLock, (void **)&o_IBLock);
        patch_slot(&vt[12], lb_IBUnlock, (void **)&o_IBUnlock);
    }
    return hr;
}
static HRESULT WINAPI lb_CreateVB(IDirect3DDevice9 *dev, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *h)
{
    HRESULT hr = o_CreateVB(dev, len, usage, fvf, pool, out, h);
    fr.vb_creates++;
    if (SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out;
        patch_slot(&vt[11], lb_VBLock, (void **)&o_VBLock);
        patch_slot(&vt[12], lb_VBUnlock, (void **)&o_VBUnlock);
    }
    return hr;
}
/* the game's D3DX texture helpers (Wine's d3dx9 is x86 code too, so it runs under the emulator) */
static HRESULT (WINAPI *o_DXFilter)(void *, const void *, UINT, DWORD);
static HRESULT (WINAPI *o_DXLoadMem)(void *, const void *, const RECT *, const void *, D3DFORMAT, UINT, const void *, const RECT *, DWORD, D3DCOLOR);
static HRESULT (WINAPI *o_DXLoadSurf)(void *, const void *, const RECT *, void *, const void *, const RECT *, DWORD, D3DCOLOR);
/* Fast replacement for D3DXFilterTexture(tex, NULL, level, D3DX_FILTER_BOX) on plain 2D textures in the common
 * 16/32-bit formats: each mip level is the 2x2 average of the one above. Wine's D3DX does this through a generic
 * per-pixel float conversion, which under the x86 emulator costs ~32 ms for one 512x512 terrain tile -- and the game
 * bakes several tiles a frame while the camera moves. This takes well under a millisecond. Returns FALSE to fall
 * back to the real function for anything it does not handle. */
static int cfg_fastfilter = 1;
static DWORD g_fast_filters, g_slow_filters;

static BOOL fast_box_filter(IDirect3DBaseTexture9 *base, UINT src_level)
{
    IDirect3DTexture9 *t = (IDirect3DTexture9 *)base;
    D3DSURFACE_DESC d;
    UINT levels, l;
    int bpp, kind;
    if (IDirect3DBaseTexture9_GetType(base) != D3DRTYPE_TEXTURE) return FALSE;
    levels = IDirect3DTexture9_GetLevelCount(t);
    if (src_level == 0xffffffff) src_level = 0;
    if (src_level >= levels || FAILED(IDirect3DTexture9_GetLevelDesc(t, src_level, &d))) return FALSE;
    switch (d.Format)
    {
    case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: bpp = 2; kind = 1555; break;
    case D3DFMT_R5G6B5: bpp = 2; kind = 565; break;
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: bpp = 2; kind = 4444; break;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: bpp = 4; kind = 8888; break;
    default: return FALSE;
    }
    for (l = src_level; l + 1 < levels; l++)
    {
        D3DLOCKED_RECT s, o;
        D3DSURFACE_DESC sd, od;
        UINT x, y;
        if (FAILED(IDirect3DTexture9_GetLevelDesc(t, l, &sd)) || FAILED(IDirect3DTexture9_GetLevelDesc(t, l + 1, &od))) return l > src_level;
        if (FAILED(IDirect3DTexture9_LockRect(t, l, &s, NULL, D3DLOCK_READONLY))) return l > src_level;
        if (FAILED(IDirect3DTexture9_LockRect(t, l + 1, &o, NULL, 0))) { IDirect3DTexture9_UnlockRect(t, l); return l > src_level; }
        if (sd.Width == od.Width * 2 && sd.Height == od.Height * 2 && bpp == 2 && kind != 4444)
        {
            /* the usual case, two 5/6-bit fields at a time */
            const DWORD m1 = kind == 1555 ? 0x7c1f : 0xf81f, r1 = kind == 1555 ? 0x0802 : 0x1002;      /* red + blue */
            const DWORD m2 = kind == 1555 ? 0x83e0 : 0x07e0, r2 = kind == 1555 ? 0x10040 : 0x0040;     /* green (+ alpha: set if 2 of 4) */
            for (y = 0; y < od.Height; y++)
            {
                const WORD *p0 = (const WORD *)((const BYTE *)s.pBits + y * 2 * s.Pitch), *p1 = (const WORD *)((const BYTE *)p0 + s.Pitch);
                WORD *out = (WORD *)((BYTE *)o.pBits + y * o.Pitch);
                for (x = 0; x < od.Width; x++, p0 += 2, p1 += 2)
                {
                    DWORD a = p0[0], b = p0[1], c = p1[0], e = p1[1];
                    out[x] = (WORD)((((a & m1) + (b & m1) + (c & m1) + (e & m1) + r1) >> 2 & m1) | (((a & m2) + (b & m2) + (c & m2) + (e & m2) + r2) >> 2 & m2));
                }
            }
        }
        else
        for (y = 0; y < od.Height; y++)
        {
            UINT y0 = y * 2 < sd.Height ? y * 2 : sd.Height - 1, y1 = y * 2 + 1 < sd.Height ? y * 2 + 1 : sd.Height - 1;
            const BYTE *r0 = (const BYTE *)s.pBits + y0 * s.Pitch, *r1 = (const BYTE *)s.pBits + y1 * s.Pitch;
            BYTE *out = (BYTE *)o.pBits + y * o.Pitch;
            for (x = 0; x < od.Width; x++)
            {
                UINT x0 = x * 2 < sd.Width ? x * 2 : sd.Width - 1, x1 = x * 2 + 1 < sd.Width ? x * 2 + 1 : sd.Width - 1;
                if (bpp == 4)
                {
                    DWORD a = ((const DWORD *)r0)[x0], b = ((const DWORD *)r0)[x1], c = ((const DWORD *)r1)[x0], e = ((const DWORD *)r1)[x1];
                    DWORD rb = ((a & 0xff00ff) + (b & 0xff00ff) + (c & 0xff00ff) + (e & 0xff00ff) + 0x020002) >> 2 & 0xff00ff;
                    DWORD ag = (((a >> 8) & 0xff00ff) + ((b >> 8) & 0xff00ff) + ((c >> 8) & 0xff00ff) + ((e >> 8) & 0xff00ff) + 0x020002) >> 2 & 0xff00ff;
                    ((DWORD *)out)[x] = rb | (ag << 8);
                }
                else
                {
                    DWORD a = ((const WORD *)r0)[x0], b = ((const WORD *)r0)[x1], c = ((const WORD *)r1)[x0], e = ((const WORD *)r1)[x1], v;
                    if (kind == 1555)
                    {
                        DWORD al = (a >> 15) + (b >> 15) + (c >> 15) + (e >> 15);
                        DWORD r = ((a >> 10 & 31) + (b >> 10 & 31) + (c >> 10 & 31) + (e >> 10 & 31) + 2) >> 2;
                        DWORD gg = ((a >> 5 & 31) + (b >> 5 & 31) + (c >> 5 & 31) + (e >> 5 & 31) + 2) >> 2;
                        DWORD bl = ((a & 31) + (b & 31) + (c & 31) + (e & 31) + 2) >> 2;
                        v = (al >= 2 ? 0x8000 : 0) | r << 10 | gg << 5 | bl;
                    }
                    else if (kind == 565)
                    {
                        DWORD r = ((a >> 11) + (b >> 11) + (c >> 11) + (e >> 11) + 2) >> 2;
                        DWORD gg = ((a >> 5 & 63) + (b >> 5 & 63) + (c >> 5 & 63) + (e >> 5 & 63) + 2) >> 2;
                        DWORD bl = ((a & 31) + (b & 31) + (c & 31) + (e & 31) + 2) >> 2;
                        v = r << 11 | gg << 5 | bl;
                    }
                    else
                    {
                        DWORD al = ((a >> 12) + (b >> 12) + (c >> 12) + (e >> 12) + 2) >> 2;
                        DWORD r = ((a >> 8 & 15) + (b >> 8 & 15) + (c >> 8 & 15) + (e >> 8 & 15) + 2) >> 2;
                        DWORD gg = ((a >> 4 & 15) + (b >> 4 & 15) + (c >> 4 & 15) + (e >> 4 & 15) + 2) >> 2;
                        DWORD bl = ((a & 15) + (b & 15) + (c & 15) + (e & 15) + 2) >> 2;
                        v = al << 12 | r << 8 | gg << 4 | bl;
                    }
                    ((WORD *)out)[x] = (WORD)v;
                }
            }
        }
        IDirect3DTexture9_UnlockRect(t, l + 1);
        IDirect3DTexture9_UnlockRect(t, l);
    }
    return TRUE;
}

static HRESULT WINAPI h_DXFilter(void *tex, const void *pal, UINT level, DWORD filter)
{
    LARGE_INTEGER a, b; HRESULT hr;
    QueryPerformanceCounter(&a);
    if (fr.tex_created.QuadPart) { fr.tile_fill_us += us_between(&fr.tex_created, &a); fr.tex_created.QuadPart = 0; }
    /* 5 = D3DX_FILTER_BOX, 0xffffffff = D3DX_DEFAULT (box for mip chains) */
    if (cfg_fastfilter && tex && !pal && (filter == 5 || filter == 0xffffffff) && fast_box_filter((IDirect3DBaseTexture9 *)tex, level))
    {
        QueryPerformanceCounter(&b);
        g_fast_filters++;
        fr.dx_filter++; fr.dx_filter_us += us_between(&a, &b); fr.dx_filter_flags = filter;
        return D3D_OK;
    }
    g_slow_filters++;
    hr = o_DXFilter(tex, pal, level, filter); QueryPerformanceCounter(&b);
    fr.dx_filter++; fr.dx_filter_us += us_between(&a, &b); fr.dx_filter_flags = filter;
    return hr;
}
static HRESULT WINAPI h_DXLoadMem(void *dst, const void *dpal, const RECT *drc, const void *src, D3DFORMAT fmt, UINT pitch, const void *spal, const RECT *src_rc, DWORD filter, D3DCOLOR key)
{
    LARGE_INTEGER a, b; HRESULT hr;
    QueryPerformanceCounter(&a); hr = o_DXLoadMem(dst, dpal, drc, src, fmt, pitch, spal, src_rc, filter, key); QueryPerformanceCounter(&b);
    fr.dx_loadmem++; fr.dx_loadmem_us += us_between(&a, &b); fr.dx_loadmem_fmt = fmt; fr.dx_loadmem_filter = filter;
    return hr;
}
static HRESULT WINAPI h_DXLoadSurf(void *dst, const void *dpal, const RECT *drc, void *src, const void *spal, const RECT *src_rc, DWORD filter, D3DCOLOR key)
{
    LARGE_INTEGER a, b; HRESULT hr;
    QueryPerformanceCounter(&a); hr = o_DXLoadSurf(dst, dpal, drc, src, spal, src_rc, filter, key); QueryPerformanceCounter(&b);
    fr.dx_loadsurf++; fr.dx_loadsurf_us += us_between(&a, &b);
    return hr;
}
/* ---- two small corrections inside game.dat (Patch 1.06/1.09 layout; every site is checked before it is changed) ----
 * 1. Round command buttons. The round buttons over a selected building are game windows laid out in an 800x600
 *    reference, so on a 16:9 picture they are a third too wide (ovals). Their draw routine (0x4a5112) asks the window
 *    for its position and size; the two size calls (0x4a53ca, 0x4a5544) get the width corrected and the left edge
 *    moved so the button stays centred. (Only the drawing changes; the clickable area stays the wider one.)
 * 2. Tint flashes (the house-colour flash when something is selected, damage flashes, ...). TintEnvelope::play
 *    (0x674818) takes its attack/decay/sustain lengths in drawn frames and was written for 30 fps; at 120 fps the
 *    selection flash was far too short. The lengths are scaled by the real frame rate / 30 (only with SAGE Unlocked:
 *    gamefps > 30, written by the launch wrapper). */
static int cfg_roundbuttons = 1;
static int cfg_logicinterp = 1;
static int cfg_firstphase = 1;
static int cfg_ringtiming = 1;
static UINT cfg_gamefps = 30;
__attribute__((used)) static void __cdecl radial_fix(int *w)
{
    if (g.gw && g.gh)
    {
        int w2 = MulDiv(*w, g.gh * 4, g.gw * 3);          /* undo the 4:3 -> picture-shape stretch */
        /* in that routine the locals are  w [ebp-2Ch]  h [ebp-28h]  x [ebp-24h]  y [ebp-20h] */
        if (w2 > 0 && w2 < *w) { w[2] += (*w - w2) / 2; *w = w2; }
    }
}
__attribute__((used)) static const DWORD p_win_get_size = 0x71548d;
__attribute__((naked)) static void h_RadialGetSize(void)   /* thiscall (window; int *w, int *h) */
{
    __asm__("pushl 8(%esp)\n"                /* h */
            "pushl 8(%esp)\n"                /* w (the push above moved it) */
            "call *_p_win_get_size\n"        /* the game's own function; it leaves the registers as its callers expect */
            HOOK_SAVE
            "pushl 40(%ebp)\n"               /* w */
            "call _radial_fix\n"
            "addl $4, %esp\n"
            HOOK_RESTORE
            "ret $8\n");
}

static double g_avg_frame_ms;      /* running average of the real frame time (set in frame_len_tick) */
__attribute__((used)) static void __cdecl tint_scale(UINT *a)      /* a[0] attack, a[1] decay, a[2] sustain, in drawn frames */
{
    /* How many drawn frames one 30 fps frame is right now. The envelope spends attack frames rising, then stays at
     * the peak for sustain + 3 frames (one to arrive, one to switch to "sustain", the countdown, one to release),
     * then decay frames falling - measured at 30 fps: selection flash = 4 frames at the peak + 4 falling = 266 ms. */
    double k = g_avg_frame_ms > 1.0 ? 33.3333 / g_avg_frame_ms : (cfg_gamefps > 30 ? cfg_gamefps / 30.0 : 1.0);
    if (k < 1.0) k = 1.0;
    if (k > 8.0) k = 8.0;
    a[0] = (UINT)(a[0] * k + 0.5);
    a[1] = (UINT)(a[1] * k + 0.5);
    a[2] = (UINT)((a[2] + 3) * k - 3 + 0.5);
}
__attribute__((used)) static const DWORD p_tint_play = 0x674818;
__attribute__((naked)) static void h_TintPlay(void)        /* thiscall (envelope; const float *color, attack, decay, sustain) */
{
    __asm__(HOOK_SAVE
            "leal 44(%ebp), %eax\n"          /* the three lengths, changed where the caller pushed them */
            "pushl %eax\n"
            "call _tint_scale\n"
            "addl $4, %esp\n"
            HOOK_RESTORE
            "jmp *_p_tint_play\n");          /* on into the game's function with every register as the caller left it */
}

/* 3. The logic-side interpolation fraction. During the engine update the game sets its interpolation value to
 *    phase / framesPerTick (0x63256f) and things computed inside the update (the scripted screen fades - the white
 *    flashes between the main menu's scenes -, scripted camera moves, ...) use it: base + fraction * step. At 30 fps
 *    framesPerTick is 6 and the fraction runs 1/6..6/6 over a tick. SAGE Unlocked makes framesPerTick fps/5 (24 at
 *    120 fps) but still runs 6 phases a tick, so those values only moved a quarter of their step during a tick and
 *    then jumped: fades climbed in 200 ms steps and scripted cameras lagged behind. The fraction is phase/6 again
 *    (what SAGE Unlocked itself uses for the phases it dispatches); the value used for drawing is set later in the
 *    frame by SAGE Unlocked and is not touched. Keeps ecx like the original, which its callers rely on. */
__attribute__((used)) static const float k_sixth = 1.0f / 6.0f;
__attribute__((naked)) static void h_SetLogicInterp(void)
{
    __asm__("pushl %eax\n"
            "movl 0x34(%ecx), %eax\n"
            "cmpl $6, %eax\n"
            "jle 1f\n"
            "movl $6, %eax\n"
            "1:\n"
            "testl %eax, %eax\n"
            "jns 2f\n"
            "xorl %eax, %eax\n"
            "2:\n"
            "cvtsi2ssl %eax, %xmm0\n"
            "mulss _k_sixth, %xmm0\n"
            "movss %xmm0, 0x3c(%ecx)\n"
            "popl %eax\n"
            "ret\n");
}

/* 4. "First phase of the tick" once per tick. Everything the game interpolates between logic ticks (screen fades,
 *    model animation, ...) keeps a previous and a current value and refreshes the pair when the engine says it is in
 *    phase 1 (0x63252f): prev = cur; cur = new. At 30 fps phase 1 is exactly one drawn frame. With SAGE Unlocked a
 *    phase lasts several frames (4 at 120 fps), so on the second frame of phase 1 the pair was refreshed again, prev
 *    became equal to cur, and the value stood still for the rest of the tick: fades climbed in 200 ms steps, birds and
 *    animals animated at 5 fps. The check is true only on the first drawn frame of phase 1 now. */
static DWORD g_p1_frame = 0xffffffff;
static BOOL g_seen_other_phase = TRUE;
__attribute__((used)) static DWORD __cdecl first_phase_once(void *engine)
{
    DWORD phase = *(DWORD *)((BYTE *)engine + 0x34);
    if (phase != 1) { g_seen_other_phase = TRUE; return 0; }
    if (g_seen_other_phase) { g_seen_other_phase = FALSE; g_p1_frame = g_frame_no; }
    return g_frame_no == g_p1_frame;
}
__attribute__((naked)) static void h_IsFirstPhase(void)      /* thiscall, engine in ecx; ecx and edx are kept */
{
    __asm__("pushl %ecx\n"
            "pushl %edx\n"
            "pushl %ecx\n"
            "call _first_phase_once\n"
            "addl $4, %esp\n"
            "popl %edx\n"
            "popl %ecx\n"
            "ret\n");
}

/* 5. Real frame length for the animation clock. The engine advances its W3D clock (scripted camera moves, model
 *    animation, water, ...) by a frame length in ms that the game client sets every frame (setter 0x44bf5c, value at
 *    0xdc7a8c): 33 at the original 30 fps. SAGE Unlocked replaces the setter with "value * 30 / fps", i.e. a fixed
 *    8 ms a frame at fps=120. That is only right while the game really draws 120 frames a second: in the main menu's
 *    heavy scenes (~70 fps) or on a slower Mac (~90 fps) cameras and animations ran at 60-75 % speed while the game
 *    logic kept real time (the menu camera never got down to the river). The setter is replaced once more: the frame
 *    length is the time the last frame really took (the regular frame clock while the pacing is locked), with the
 *    fraction carried over so nothing drifts. */
static int cfg_realframelen = 1;
static DWORD g_fl_ms = 8;                 /* what the animation clock gets this frame */
static double g_fl_acc, g_fl_base = 33.3333;
static LARGE_INTEGER g_fl_last;
static double g_fl_last_vt;
static BOOL g_fl_hooked;

__attribute__((used)) static void __cdecl frame_len_set(float ms)
{
    if (ms > 1.0f && ms < 200.0f) g_fl_base = ms;        /* what the game asked for (33.3 normally) */
    *(DWORD *)0xdc7a8c = g_fl_ms;
}
__attribute__((naked)) static void h_SetFrameLen(void)     /* stdcall (float ms); the game's own setter only changes eax */
{
    __asm__(HOOK_SAVE
            "pushl 40(%ebp)\n"
            "call _frame_len_set\n"
            "addl $4, %esp\n"
            "cvttss2si 40(%ebp), %eax\n"    /* the game's setter leaves (int)ms in eax */
            "movl %eax, 28(%ebp)\n"
            HOOK_RESTORE
            "ret $4\n");
}

static void frame_len_hook(void)          /* (re)install; SAGE Unlocked may patch the same place before or after us */
{
    BYTE *p = (BYTE *)0x44bf5c;
    DWORD old, rel = (DWORD)(ULONG_PTR)h_SetFrameLen - (0x44bf5c + 5);
    static const BYTE tail[] = { 0xA3, 0x8C, 0x7A, 0xDC, 0x00, 0xC2, 0x04, 0x00 };
    if (IsBadReadPtr(p, 14) || memcmp(p + 6, tail, 8)) { if (!g_fl_hooked) logf("real frame length: unexpected code at 0044bf5c, not applied"); cfg_realframelen = 0; return; }
    if (p[0] == 0xE9 && !memcmp(p + 1, &rel, 4)) return;                     /* already ours */
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0xE9; memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf("real frame length for the animation clock: %s", g_fl_hooked ? "re-applied (something else had patched it)" : "on");
    g_fl_hooked = TRUE;
}

static void frame_len_tick(void)          /* once per frame, at Present */
{
    LARGE_INTEGER now;
    double ms;
    static DWORD n;
    if (!cfg_realframelen || cfg_gamefps <= 30) return;
    if (!(n++ % 120)) frame_len_hook();
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&now);
    if (g_sync_on && g_vt_valid && g_fl_last_vt > 0 && g_vt_ms > g_fl_last_vt) ms = g_vt_ms - g_fl_last_vt;      /* locked: the regular clock */
    else ms = g_fl_last.QuadPart ? (double)(now.QuadPart - g_fl_last.QuadPart) * 1000.0 / (double)g_qpf.QuadPart : 1000.0 / cfg_gamefps;
    g_fl_last = now; g_fl_last_vt = g_sync_on && g_vt_valid ? g_vt_ms : 0;
    if (ms < 0) ms = 0;
    if (ms > 100) ms = 100;                /* a hitch or a loading pause must not make everything leap */
    g_avg_frame_ms = g_avg_frame_ms > 0 ? g_avg_frame_ms * 0.95 + ms * 0.05 : ms;
    g_fl_acc += ms * (g_fl_base / 33.3333);
    g_fl_ms = (DWORD)g_fl_acc;
    g_fl_acc -= g_fl_ms;
    if (g_fl_hooked) *(DWORD *)0xdc7a8c = g_fl_ms;
}

static BOOL retarget_call(DWORD site, DWORD expect_target, void *hook)   /* "call rel32" at site -> hook */
{
    BYTE *p = (BYTE *)(ULONG_PTR)site;
    DWORD old, rel;
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8) return FALSE;
    memcpy(&rel, p + 1, 4);
    if (site + 5 + rel != expect_target) return FALSE;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    rel = (DWORD)(ULONG_PTR)hook - (site + 5);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return TRUE;
}

/* 6. The selection ring under units (RadiusDecal update, 0x7329xx). Three things in it are counted in drawn frames
 *    assuming 30 fps: the opacity throb (period = OpacityThrobTime * 0.03 frames, against the client's frame counter),
 *    the rotation (angle = frame counter * RotationsPerMinute * 1/30 s ...) and the spiral-in when the ring appears
 *    (a fixed step per frame). At 120 fps the ring pulsed, turned and closed four times too fast. The two constants are
 *    read from our own copies (frames per ms and seconds per frame at the real rate) and the two per-frame steps are
 *    scaled by 30/fps. Uses the configured rate, not a measured one: the formulas multiply it by the absolute frame
 *    number, so it must not change while the game runs. */
__attribute__((used)) static float k_decal_fpms = 0.03f, k_decal_spf = 1.0f / 30.0f, k_decal_step = 1.0f;
__attribute__((naked)) static void h_DecalRpm(void)    { __asm__("movss 0x2c(%ecx), %xmm4\n" "mulss _k_decal_step, %xmm4\n" "ret\n"); }
__attribute__((naked)) static void h_DecalSpiral(void) { __asm__("movss 0x30(%ecx), %xmm3\n" "mulss _k_decal_step, %xmm3\n" "ret\n"); }

static BOOL patch_bytes(DWORD addr, const BYTE *expect, const BYTE *repl, DWORD n)
{
    BYTE *p = (BYTE *)(ULONG_PTR)addr;
    DWORD old;
    if (IsBadReadPtr(p, n) || memcmp(p, expect, n)) return FALSE;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy(p, repl, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return TRUE;
}

static void patch_selection_ring(void)
{
    static const BYTE fld_fpms[6] = { 0xD9, 0x05, 0x24, 0xF6, 0xD9, 0x00 }, fld_spf[6] = { 0xD9, 0x05, 0x2C, 0xF6, 0xD9, 0x00 };
    static const BYTE ld_rpm[5] = { 0xF3, 0x0F, 0x10, 0x61, 0x2C }, ld_spiral[5] = { 0xF3, 0x0F, 0x10, 0x59, 0x30 };
    BYTE r[6];
    DWORD a, rel;
    int n = 0;
    k_decal_fpms = cfg_gamefps / 1000.0f; k_decal_spf = 1.0f / cfg_gamefps; k_decal_step = 30.0f / cfg_gamefps;
    r[0] = 0xD9; r[1] = 0x05; a = (DWORD)(ULONG_PTR)&k_decal_fpms; memcpy(r + 2, &a, 4); n += patch_bytes(0x73297a, fld_fpms, r, 6);
    a = (DWORD)(ULONG_PTR)&k_decal_spf; memcpy(r + 2, &a, 4); n += patch_bytes(0x732b4f, fld_spf, r, 6);
    r[0] = 0xE8; rel = (DWORD)(ULONG_PTR)h_DecalRpm - (0x732a74 + 5); memcpy(r + 1, &rel, 4); n += patch_bytes(0x732a74, ld_rpm, r, 5);
    rel = (DWORD)(ULONG_PTR)h_DecalSpiral - (0x732a84 + 5); memcpy(r + 1, &rel, 4); n += patch_bytes(0x732a84, ld_spiral, r, 5);
    logf("selection ring timing: %d of 4 places", n);
}

/* 7. Wide picture without trimming ("Hor+"). The engine's camera takes a horizontal field of view (50 degrees for the
 *    game view) and derives the vertical one from the picture's shape (CameraClass::Set_View_Plane, 0x533590, with
 *    vfov = -1), so a 16:9 picture shows only 3/4 of the height a 4:3 picture shows. With horplus=1 the horizontal
 *    angle is widened by (shape / 4:3): the height shown is what the game was designed for at 4:3 and the extra width
 *    is added at the sides. Culling, picking and scrolling limits all come from the same camera. */
static int cfg_horplus = 1;
/* 8. Network games. The frame limiter multiplies the frame rate by a factor ([0xd9f498], 0.7 .. 1.0) that the game lowers
 *    while its network run-ahead buffer is empty (set once per tick at 0x63a06e ff.). At the original 30 fps that slowed
 *    the game logic down together with the frames. With the frame-rate unlock the logic keeps its own pace and waits for
 *    the network by itself, so the factor only costs drawn frames: the machine that waits for the other one in a match
 *    ran at about 0.7 x its cap (43 fps at a 60 cap). netfps=1 makes the limiter use 1.0. */
static int cfg_netfps = 1;
__attribute__((used)) static float k_one = 1.0f;
static int cfg_steamoverlay = 0;   /* steamoverlay=1: load NotProton's Steam bridge into the game so the Steam overlay (FPS counter) can attach */
double tan(double); double atan(double);     /* from the C runtime; <math.h> is not included because it declares logf() */
__attribute__((used)) static void __cdecl view_plane_widen(void *cam, float *arg)     /* arg[0] = hfov, arg[1] = vfov */
{
    if (arg[1] == -1.0f)
    {
        float a = *(float *)((BYTE *)cam + 0xe8);        /* the camera's width / height */
        float hfov = arg[0];
        if (a > 1.34f && a < 4.0f && hfov > 0.01f && hfov < 2.6f)
            arg[0] = 2.0f * (float)atan(tan(hfov * 0.5) * (a * 0.75));
    }
}
__attribute__((used)) static const DWORD p_set_view_plane = 0x533590;
__attribute__((naked)) static void h_SetViewPlane(void)    /* thiscall (camera; float hfov, float vfov) */
{
    __asm__(HOOK_SAVE
            "leal 40(%ebp), %eax\n"          /* hfov and vfov where the caller pushed them */
            "pushl %eax\n"
            "pushl 24(%ebp)\n"               /* the camera (ecx) */
            "call _view_plane_widen\n"
            "addl $8, %esp\n"
            HOOK_RESTORE
            "jmp *_p_set_view_plane\n");
}

/* ---- a unit far outside the map (farguard=1, the default) ----
 * In big matches a unit can end up billions of units outside the map (seen: a Goblin Cave Troll at x = 5.6e9,
 * y = -6.9e9; what moves it there is not known yet). Nothing happens until the game's collision manager next works
 * on it: with coordinates that large it files the unit's extent under a wild index (a write far outside its table:
 * the endless fault loop under FEX) and, because the extent has no width in single precision, later meets the end
 * of the extent before its start and unlinks an object it never linked (access violation at 0xB6DD43).
 *
 * The collision manager (the object at [0xde4360]; its work is done by the object at +10h of it) keeps the records
 * it has to work on in a list: first one at +8 of that object, next one at record+14h. A record has the collision
 * part of its game object at +4 (0 when the object is gone) and the game object itself at +A0h, whose position is
 * at +14h (x) and +24h (y), with a copy at +38h and +3Ch. The manager's update is a virtual function (0xb6c420,
 * the only pointer to it is the table entry at 0xd0bf30). That entry is pointed here: before every update the
 * list is walked, and a unit beyond +-100000 (maps are a few thousand wide; up to 256 units per update) is handed to the manager's own
 * "forget this object" routine (0xb6c480). The unit stays where it is, the collision manager just no longer
 * knows it; the log says which unit it was and where it stood the last time it moved. In a network game all
 * machines must run the same proxy, as for everything else that touches the game's logic. */
#define FAR_LIMIT 100000.0f
static int cfg_farguard = 1;
static struct far_slot { DWORD rec; float x, y; } g_far[2048];
static DWORD g_far_notes, g_far_removed;
typedef void (__attribute__((thiscall)) *coll_forget_fn)(void *mgr, void *part);
static int whole(float v)                  /* a float for wsprintf: whole units, clamped */
{
    if (v != v) return 0x7fffffff;
    if (v > 2.0e9f) return 2000000000;
    if (v < -2.0e9f) return -2000000000;
    return (int)v;
}
__attribute__((used)) static void __cdecl far_guard(DWORD mgr)
{
    static DWORD part[256];
    DWORD work, rec, walked = 0, n = 0, i;
    if (mgr < 0x10000) return;
    work = *(const DWORD *)(ULONG_PTR)(mgr + 0x10);
    if (work < 0x10000 || !*(const DWORD *)(ULONG_PTR)work) return;         /* the update itself does nothing then */
    for (rec = *(const DWORD *)(ULONG_PTR)(work + 8); rec >= 0x10000 && walked < 50000; rec = *(const DWORD *)(ULONG_PTR)(rec + 0x14), walked++)
    {
        DWORD pt = *(const DWORD *)(ULONG_PTR)(rec + 4), own = *(const DWORD *)(ULONG_PTR)(rec + 0xa0);
        struct far_slot *s = &g_far[(rec >> 3) & 2047];
        const DWORD *q;
        float x, y, cx, cy;
        BOOL known;
        if (pt < 0x10000 || own < 0x10000) continue;
        q = (const DWORD *)(ULONG_PTR)own;
        if (q[0] < g_code_hi || q[0] >= 0xe00000) continue;   /* a game object starts with a table of its functions inside game.dat */
        x = *(const float *)&q[5]; y = *(const float *)&q[9];
        cx = *(const float *)&q[14]; cy = *(const float *)&q[15];        /* the copy; either one far away will do */
        known = s->rec == rec;
        if (x != x || y != y || x > FAR_LIMIT || x < -FAR_LIMIT || y > FAR_LIMIT || y < -FAR_LIMIT ||
            cx != cx || cy != cy || cx > FAR_LIMIT || cx < -FAR_LIMIT || cy > FAR_LIMIT || cy < -FAR_LIMIT)
        {
            if (n >= 256) continue;                             /* more than that in one step: the rest at the next one */
            part[n++] = pt;
            if (g_far_notes < 40)
            {
                DWORD tmpl = q[1];
                g_far_notes++;
                logf("far object: a unit at x %d, y %d, z %d (%08x %08x %08x) is taken out of the collision manager; it last moved from x %d, y %d%s; record %08x, object %08x, id %u",
                     whole(x), whole(y), whole(*(const float *)&q[13]), q[5], q[9], q[13], known ? whole(s->x) : 0, known ? whole(s->y) : 0,
                     known ? "" : " (not seen moving before)", rec, own, *(const DWORD *)(ULONG_PTR)(rec + 0x9c));
                if (mem_ok(tmpl, 0x30)) { g_names_left = 1; note_text(*(const DWORD *)(ULONG_PTR)(tmpl + 0x2c), "its kind"); }
            }
        }
        s->rec = rec; s->x = x; s->y = y;
    }
    for (i = 0; i < n; i++) ((coll_forget_fn)0xb6c480)((void *)(ULONG_PTR)mgr, (void *)(ULONG_PTR)part[i]);
    g_far_removed += n;
}
__attribute__((used)) static const DWORD p_coll_update = 0xb6c420;
__attribute__((naked)) static void h_CollUpdate(void)      /* stands in the table for the manager's update; the manager is in ecx */
{
    __asm__(HOOK_SAVE
            "pushl 24(%ebp)\n"
            "call _far_guard\n"
            "addl $4, %esp\n"
            HOOK_RESTORE
            "jmp *_p_coll_update\n");
}
/* The same thing at its source. Every game object has a function the game calls whenever the object's place or
 * direction changes (0x69355d, entry +14h of the table at 0xc123d8; the object's matrix at +8 and its position at
 * +38h are already the new ones, and the function gets the old matrix, the old position and the old angle). The
 * game itself begins that function by destroying an object whose position is not a number. A position that is a
 * number but billions of units away gets through, and then more than the collision manager chokes on it (with the
 * unit only taken out of the collision manager the game froze three minutes later in another part of its logic).
 * So the table entry is pointed here: when an object that was on the map is about to be outside +-100000, its
 * matrix, position and angle are put back to the old ones before the game's function runs. The unit stays where
 * it was; the log says what it is and which code asked for the move. The test is done in a few instructions
 * without touching a register, because this is called for every unit that moves. */
static DWORD g_far_kept;
static void note_callers(const DWORD *sp)          /* the return addresses on the stack above sp, one log line */
{
    MEMORY_BASIC_INFORMATION mi;
    char line[500];
    DWORD i, n;
    int len;
    if (!mem_ok((DWORD)(ULONG_PTR)sp, 4) || !VirtualQuery(sp, &mi, sizeof(mi))) return;
    n = (DWORD)(((BYTE *)mi.BaseAddress + mi.RegionSize - (BYTE *)sp) / 4);
    if (n > 1024) n = 1024;
    for (len = wsprintfA(line, "    asked for by (return addresses, offset from the stack top):"), i = 0; i < n && len < 460; i++)
        if (after_call(sp[i])) len += wsprintfA(line + len, " %x(+%x)", sp[i], i * 4);
    logf("%s", line);
}
static BOOL off_map(float x, float y) { return x != x || y != y || x > FAR_LIMIT || x < -FAR_LIMIT || y > FAR_LIMIT || y < -FAR_LIMIT; }
__attribute__((used)) static void __cdecl obj_moved(DWORD obj, DWORD *arg, const DWORD *sp)     /* arg: old matrix, old position, old angle */
{
    DWORD *q = (DWORD *)(ULONG_PTR)obj;
    const DWORD *om, *op;
    float x, y, ox, oy;
    if (obj < 0x10000 || arg[0] < 0x10000 || arg[1] < 0x10000) return;
    x = *(const float *)&q[5]; y = *(const float *)&q[9];
    if (x != x || y != y || !off_map(x, y)) return;          /* not a number: the game destroys the object itself */
    om = (const DWORD *)(ULONG_PTR)arg[0]; op = (const DWORD *)(ULONG_PTR)arg[1];
    ox = *(const float *)&op[0]; oy = *(const float *)&op[1];
    if (off_map(ox, oy)) return;                              /* it was not on the map before either: nothing to go back to */
    g_far_kept++;
    if (g_far_kept <= 12 || !(g_far_kept % 1000))
    {
        DWORD tmpl = q[1];
        logf("far object: a unit at x %d, y %d was about to be moved to x %d, y %d, z %d (%08x %08x %08x) and is kept where it was; object %08x, time number %u of this kind",
             whole(ox), whole(oy), whole(x), whole(y), whole(*(const float *)&q[13]), q[5], q[9], q[13], obj, g_far_kept);
        if (mem_ok(tmpl, 0x30)) { g_names_left = 1; note_text(*(const DWORD *)(ULONG_PTR)(tmpl + 0x2c), "its kind"); }
        note_callers(sp);
        /* The knock-back's launch routine (0x792dbd) is on the stack when its call to 0x792997 returns to 0x793087
         * (seen at sp+DCh, its frame pointer saved just below): what it worked with. It lands the unit at
         * position + speed * N frames; N comes from fabs(2 * up-speed / (gravity * factor)) through a float-to-int
         * conversion, and a result that does not fit becomes 80000000h = 2147483648 frames. */
        if (mem_ok((DWORD)(ULONG_PTR)sp, 0xe0) && sp[0xdc / 4] == 0x793087 && mem_ok(sp[0xd8 / 4] - 0x30, 0x48))
        {
            const DWORD *f = (const DWORD *)(ULONG_PTR)sp[0xd8 / 4];           /* the launch routine's ebp */
            DWORD mod = f[-4], data = mem_ok(mod, 12) ? ((const DWORD *)(ULONG_PTR)mod)[1] : 0, gd = *(const DWORD *)0xde4364;
            logf("    knock-back launch: frames N %08x, up-speed used %08x, height above ground %08x, arc height %08x; landing %08x %08x %08x; factor %08x, gravity %08x, logic rate %d, frame rate %d",
                 f[-3], f[2], f[-1], f[-2], f[-7], f[-6], f[-5], mem_ok(data, 0x50) ? ((const DWORD *)(ULONG_PTR)data)[0x4c / 4] : 0xdeadbeef,
                 mem_ok(gd, 0xc8) ? *(const DWORD *)(ULONG_PTR)(gd + 0xc4) : 0xdeadbeef, *(const int *)0xd9f608, *(const int *)0xd9f60c);
        }
    }
    memcpy(q + 2, om, 48);                                    /* matrix, +8 .. +37h */
    q[14] = op[0]; q[15] = op[1]; q[16] = op[2];              /* position, +38h */
    q[17] = arg[2];                                           /* angle, +44h */
}
__attribute__((used)) static const DWORD p_obj_moved = 0x69355d;
__attribute__((naked)) static void h_ObjMoved(void)        /* thiscall (object; old matrix, old position, old angle) */
{
    __asm__("pushfl\n"
            "pushl %eax\n"
            "movl 0x14(%ecx), %eax\n"        /* |x| as bits: larger than those of 100000.0 (0x47C35000) = far, infinite or not a number */
            "andl $0x7fffffff, %eax\n"
            "cmpl $0x47c35000, %eax\n"
            "ja 1f\n"
            "movl 0x24(%ecx), %eax\n"
            "andl $0x7fffffff, %eax\n"
            "cmpl $0x47c35000, %eax\n"
            "ja 1f\n"
            "popl %eax\n"
            "popfl\n"
            "jmp *_p_obj_moved\n"
            "1:\n"
            "popl %eax\n"
            "popfl\n"
            HOOK_SAVE
            "leal 36(%ebp), %eax\n"          /* the stack as it was at entry */
            "pushl %eax\n"
            "leal 40(%ebp), %eax\n"          /* the three arguments */
            "pushl %eax\n"
            "pushl 24(%ebp)\n"               /* the object (ecx) */
            "call _obj_moved\n"
            "addl $12, %esp\n"
            HOOK_RESTORE
            "jmp *_p_obj_moved\n");
}
static void far_guard_install(void)
{
    DWORD *slot = (DWORD *)0xd0bf30, *moved = (DWORD *)0xc123ec, old;
    if (!cfg_farguard) return;
    if (!IsBadReadPtr(moved, 4) && *moved == 0x69355d && VirtualProtect(moved, 4, PAGE_READWRITE, &old))
    {
        *moved = (DWORD)(ULONG_PTR)h_ObjMoved;
        VirtualProtect(moved, 4, old, &old);
        logf("far-object guard (a unit is not moved off the map): on");
    }
    else logf("far-object guard (a unit is not moved off the map): NOT applied (unexpected code)");
    if (!IsBadReadPtr(slot, 4) && *slot == 0xb6c420 && VirtualProtect(slot, 4, PAGE_READWRITE, &old))
    {
        *slot = (DWORD)(ULONG_PTR)h_CollUpdate;
        VirtualProtect(slot, 4, old, &old);
        logf("far-object guard (collision manager): on");
    }
    else logf("far-object guard (collision manager): NOT applied (unexpected code)");
}

static void patch_game_code(void)
{
    static const DWORD tint_sites[] = { 0x6757aa, 0x67597d, 0x675dd1, 0x675e1c, 0x675e86, 0x675ed4, 0x67910e };
    int i, n = 0;
    {
        static const BYTE lim_old[10] = { 0x8B, 0xF0, 0x8B, 0xCF, 0x2B, 0x0D, 0x18, 0x43, 0xDE, 0x00 };
        BYTE r[10]; DWORD rel = (DWORD)(ULONG_PTR)h_LimTarget - (0x63a1b2 + 5);
        r[0] = 0xE8; memcpy(r + 1, &rel, 4); memset(r + 5, 0x90, 5);
        logf("frame limiter guard (a frame waits 100 ms at most): %s", patch_bytes(0x63a1b2, lim_old, r, 10) ? "on" : "NOT applied (unexpected code)");
    }
    far_guard_install();
    if (cfg_roundbuttons)
    {
        /* both size queries of the draw routine (picture, then ring/overlays) use the same four locals */
        BOOL a = retarget_call(0x4a53ca, 0x71548d, h_RadialGetSize), b = retarget_call(0x4a5544, 0x71548d, h_RadialGetSize);
        logf("round command buttons: %s", a && b ? "on" : "NOT fully applied (unexpected code)");
    }
    if (cfg_horplus)
    {
        static const DWORD vp_sites[] = { 0x487570, 0x48b909, 0x48b93d, 0x49b5d8, 0x504219, 0x504625, 0x5346f1, 0xb544e9 };
        int k = 0;
        for (i = 0; i < 8; i++) if (retarget_call(vp_sites[i], 0x533590, h_SetViewPlane)) k++;
        logf("wide view without trimming (4:3 height, wider sides): %d of 8 places", k);
    }
    if (cfg_gamefps > 30)
    {
        static const DWORD interp_sites[] = { 0x632642, 0x6326be, 0x6326e4, 0x632af0 };
        int m = 0;
        for (i = 0; i < 7; i++) if (retarget_call(tint_sites[i], 0x674818, h_TintPlay)) n++;
        logf("tint flashes: lengths x%u/30 at %d of 7 places", cfg_gamefps, n);
        if (cfg_logicinterp) for (i = 0; i < 4; i++) if (retarget_call(interp_sites[i], 0x63256f, h_SetLogicInterp)) m++;
        logf("logic-side interpolation fraction phase/6: %d of 4 places", m);
        if (cfg_ringtiming) patch_selection_ring();
        if (cfg_netfps)
        {
            static const BYTE fmul_net[6] = { 0xD8, 0x0D, 0x98, 0xF4, 0xD9, 0x00 };      /* fmul dword [0xd9f498] */
            BYTE r[6]; DWORD a = (DWORD)(ULONG_PTR)&k_one;
            r[0] = 0xD8; r[1] = 0x0D; memcpy(r + 2, &a, 4);
            logf("network frame-rate throttle: %s", patch_bytes(0x63a1a1, fmul_net, r, 6) ? "off" : "NOT changed (unexpected code)");
        }
        if (cfg_firstphase)
        {
            static const DWORD fp_sites[] = { 0x461ebd, 0x48e2d3, 0x4b51f4, 0x4b6be6, 0x4ed6ab, 0x64871c, 0x6487f3, 0x6759bb, 0x83ba12, 0x8a036e };
            for (m = 0, i = 0; i < 10; i++) if (retarget_call(fp_sites[i], 0x63252f, h_IsFirstPhase)) m++;
            logf("first-phase latch once per tick: %d of 10 places", m);
        }
    }
}

static void hook_d3dx(void)
{
    static BOOL done;
    HMODULE dx = GetModuleHandleA("d3dx9_27.dll");
    struct { void **slot; const char *name; void *fn; void **orig; } h[] = {
        { (void **)0xbd0a20, "D3DXFilterTexture", h_DXFilter, (void **)&o_DXFilter },
        { (void **)0xbd0a1c, "D3DXLoadSurfaceFromMemory", h_DXLoadMem, (void **)&o_DXLoadMem },
        { (void **)0xbd09f0, "D3DXLoadSurfaceFromSurface", h_DXLoadSurf, (void **)&o_DXLoadSurf },
    };
    int i;
    if (done || !dx) return;
    done = TRUE;
    patch_game_code();
    {
        /* game.dat's import slot for winmm!timeGetTime (1.06/1.09 layout), see the frame clock above */
        void **slot = (void **)0xbd0920;
        HMODULE mm = GetModuleHandleA("winmm.dll");
        if (cfg_frameclock && mm && !IsBadReadPtr(slot, 4) && *slot == (void *)GetProcAddress(mm, "timeGetTime"))
        { patch_slot(slot, h_timeGetTime, (void **)&o_timeGetTime); logf("hooked winmm!timeGetTime (frame clock)"); }
        else logf("frame clock not hooked (%s)", cfg_frameclock ? "unexpected import layout" : "frameclock=0");
    }
    for (i = 0; i < 3; i++)
    {
        /* only if the slot really holds that import (this is the 1.06/1.09 game.dat layout) */
        if (!IsBadReadPtr(h[i].slot, 4) && *h[i].slot == (void *)GetProcAddress(dx, h[i].name)) { patch_slot(h[i].slot, h[i].fn, h[i].orig); logf("hooked d3dx9_27!%s", h[i].name); }
        else logf("NOT hooked d3dx9_27!%s (unexpected import layout)", h[i].name);
    }
}

static HRESULT (WINAPI *o_TexLock)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
static HRESULT (WINAPI *o_TexUnlock)(IDirect3DTexture9 *, UINT);
static HRESULT WINAPI lb_TexLock(IDirect3DTexture9 *t, UINT level, D3DLOCKED_RECT *lr, const RECT *rc, DWORD flags)
{
    LARGE_INTEGER a;
    HRESULT hr;
    QueryPerformanceCounter(&a);
    if (t == g_ring_tex) g_ring_locked = g_frame_no;
    prof_begin(PROF_TEXLOCK);
    hr = o_TexLock(t, level, lr, rc, flags);
    prof_end();
    QueryPerformanceCounter(&fr.tex_lock_ret);
    fr.tex_lock_call_us += us_between(&a, &fr.tex_lock_ret);
    fr.tex_locks++;
    return hr;
}
static HRESULT WINAPI lb_TexUnlock(IDirect3DTexture9 *t, UINT level)
{
    LARGE_INTEGER a, b;
    HRESULT hr;
    QueryPerformanceCounter(&a);
    if (fr.tex_lock_ret.QuadPart) fr.tex_fill_us += us_between(&fr.tex_lock_ret, &a);
    prof_begin(PROF_TEXUNLOCK);
    hr = o_TexUnlock(t, level);
    prof_end();
    QueryPerformanceCounter(&b);
    fr.tex_unlock_us += us_between(&a, &b);
    return hr;
}
static HRESULT WINAPI lb_CreateTex(IDirect3DDevice9 *dev, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *sh)
{
    LARGE_INTEGER a, b;
    HRESULT hr;
    QueryPerformanceCounter(&a);
    hr = o_CreateTex(dev, w, h, lv, usage, f, pool, out, sh);
    QueryPerformanceCounter(&b);
    if (fr.tex_creates < 6)
    {
        fr.tex[fr.tex_creates].w = (WORD)w; fr.tex[fr.tex_creates].h = (WORD)h; fr.tex[fr.tex_creates].levels = (WORD)lv;
        fr.tex[fr.tex_creates].fmt = (WORD)(f > 0xffff ? ((f >> 24) & 0xff) : f); fr.tex[fr.tex_creates].usage = usage;
        fr.tex[fr.tex_creates].pool = pool; fr.tex[fr.tex_creates].us = us_between(&a, &b); fr.tex[fr.tex_creates].tid = GetCurrentThreadId();
    }
    fr.tex_create_us += us_between(&a, &b);
    fr.tex_creates++;
    fr.tex_created = b;
    if (SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out;
        patch_slot(&vt[19], lb_TexLock, (void **)&o_TexLock);
        patch_slot(&vt[20], lb_TexUnlock, (void **)&o_TexUnlock);
        if (cfg_ringround && w == 128 && h == 128 && g_ring_opened && GetTickCount() - g_ring_opened < 5000)
        {
            g_ring_tex = *out; g_ring_opened = 0; g_ring_locked = g_frame_no;
            logf("ring video: its frames go into texture %p (128x128, format %d, %u level(s))", (void *)*out, (int)f, lv);
        }
        else if (*out == g_ring_tex) g_ring_tex = NULL;      /* that address belongs to another texture now */
    }
    return hr;
}

static DWORD us_since(const LARGE_INTEGER *from, const LARGE_INTEGER *freq)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (DWORD)((now.QuadPart - from->QuadPart) * 1000000 / freq->QuadPart);
}

/* ---- built-in frame-rate counter (the Steam overlay cannot attach to a non-Steam game on NotProton's FEX runner) ---- */
static void draw_fps(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    static const WORD font[10] = { 075557, 026222, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717 };   /* 3x5, octal rows */
    static DWORD shown, frames, t0;
    DWORD now = GetTickCount(), v, digits[4], n = 0, i;
    const int cell = g.sh >= 1400 ? 5 : 3;
    int x0, y0, d, row, col;
    RECT r;
    frames++;
    if (!t0) t0 = now;
    if (now - t0 >= 500) { shown = (frames * 1000 + (now - t0) / 2) / (now - t0); frames = 0; t0 = now; }
    v = shown > 999 ? 999 : shown;
    do { digits[n++] = v % 10; v /= 10; } while (v && n < 4);
    /* in the left bar if there is one, else in the top bar, else over the picture's corner */
    x0 = 8 * cell; y0 = (int)cfg_topinset + 2 * cell;
    if (g.dst.left < 14 * cell && g.dst.top - (int)cfg_topinset < 9 * cell) { x0 = g.dst.left + 3 * cell; y0 = g.dst.top + 3 * cell; }
    SetRect(&r, x0 - cell, y0 - cell, x0 + (int)n * 4 * cell, y0 + 6 * cell);
    IDirect3DDevice9_ColorFill(dev, bb, &r, 0xff000000);
    for (i = 0; i < n; i++)
    {
        d = digits[n - 1 - i];
        for (row = 0; row < 5; row++)
        {
            int bits = (font[d] >> ((4 - row) * 3)) & 7;
            for (col = 0; col < 3; col++)
                if (bits & (4 >> col))
                {
                    int run = 1;
                    while (col + run < 3 && (bits & (4 >> (col + run)))) run++;
                    SetRect(&r, x0 + (int)i * 4 * cell + col * cell, y0 + row * cell, x0 + (int)i * 4 * cell + (col + run) * cell, y0 + (row + 1) * cell);
                    IDirect3DDevice9_ColorFill(dev, bb, &r, 0xffffd24a);
                    col += run - 1;
                }
        }
    }
}

/* ---- SetThreadPriority, answered locally when nothing changes ----
 * The game's asset streamer (0xa37e50) sets its loader thread's priority at the end of every pump:
 * SetThreadPriority(thread, queue longer than a threshold ? NORMAL : BELOW_NORMAL), and the engine pumps it once per
 * update. On Windows that call is nearly free; under Wine every one is a round trip to the wineserver. A repeat of
 * the last value for the same handle within a second is answered here without the call.
 * (This is how the network catch-up crawl was found: a machine that was behind ran ~200,000 empty updates a second,
 * 100,000 of these calls in 6 s. The crawl itself is fixed in the patched SAGE Unlocked, 1.9.6+fexfix5.) */
static DWORD g_prio_skipped;
static BOOL WINAPI h_SetThreadPriority(HANDLE h, int prio)
{
    static struct { HANDLE h; int prio; DWORD tick; } last[8];
    static volatile LONG busy;
    DWORD now = GetTickCount();
    int i, slot = 0;
    BOOL r;
    if (InterlockedExchange(&busy, 1)) return SetThreadPriority(h, prio);      /* another thread is in here: just do it */
    for (i = 0; i < 8; i++)
    {
        if (last[i].h == h)
        {
            if (last[i].prio == prio && now - last[i].tick < 1000) { g_prio_skipped++; InterlockedExchange(&busy, 0); return TRUE; }
            slot = i; break;
        }
        if (now - last[i].tick > now - last[slot].tick) slot = i;                /* otherwise reuse the oldest entry */
    }
    last[slot].h = h; last[slot].prio = prio; last[slot].tick = now;
    InterlockedExchange(&busy, 0);
    r = SetThreadPriority(h, prio);
    if (!r) { last[slot].h = NULL; }
    return r;
}

/* ---- where the game thread waits ----
 * A frame that stands still for a long time before it starts to draw is waiting for something (the other player's
 * data in a network game, a timer). The game's own Sleep, WaitForSingleObject and select calls on the game thread are
 * timed per calling address; a frame that waited more than 100 ms before drawing gets a line saying where. */
static DWORD g_main_tid;
static int g_net_min, g_net_max; static DWORD g_net_n; static double g_net_sum;   /* network cushion samples of this window */
static struct { DWORD ra; char kind; DWORD us, calls; } g_waits[8];
static int (WINAPI *o_select)(int, void *, void *, void *, const void *);
static void wait_note(char kind, DWORD ra, DWORD us)
{
    int i, min = 0;
    for (i = 0; i < 8; i++)
    {
        if (g_waits[i].ra == ra && g_waits[i].kind == kind) { g_waits[i].us += us; g_waits[i].calls++; return; }
        if (!g_waits[i].calls) { g_waits[i].ra = ra; g_waits[i].kind = kind; g_waits[i].us = us; g_waits[i].calls = 1; return; }
        if (g_waits[i].us < g_waits[min].us) min = i;
    }
    if (us > g_waits[min].us) { g_waits[min].ra = ra; g_waits[min].kind = kind; g_waits[min].us = us; g_waits[min].calls = 1; }
}
static void WINAPI h_Sleep(DWORD ms)
{
    LARGE_INTEGER a, b;
    if (GetCurrentThreadId() != g_main_tid) { Sleep(ms); return; }
    QueryPerformanceCounter(&a); Sleep(ms); QueryPerformanceCounter(&b);
    wait_note('S', (DWORD)(ULONG_PTR)__builtin_return_address(0), us_between(&a, &b));
}
static DWORD WINAPI h_WaitForSingleObject(HANDLE h, DWORD ms)
{
    LARGE_INTEGER a, b; DWORD r;
    if (GetCurrentThreadId() != g_main_tid) return WaitForSingleObject(h, ms);
    QueryPerformanceCounter(&a); r = WaitForSingleObject(h, ms); QueryPerformanceCounter(&b);
    wait_note('W', (DWORD)(ULONG_PTR)__builtin_return_address(0), us_between(&a, &b));
    return r;
}
static int WINAPI h_select(int n, void *rd, void *wr, void *ex, const void *tv)
{
    LARGE_INTEGER a, b; int r;
    if (GetCurrentThreadId() != g_main_tid) return o_select(n, rd, wr, ex, tv);
    QueryPerformanceCounter(&a); r = o_select(n, rd, wr, ex, tv); QueryPerformanceCounter(&b);
    wait_note('N', (DWORD)(ULONG_PTR)__builtin_return_address(0), us_between(&a, &b));
    return r;
}
static void waits_report(DWORD before_scene_us)
{
    static DWORD last_tick;
    char line[400]; int i, n = 0;
    BYTE *eng = *(BYTE **)0xde4324;
    DWORD now = GetTickCount();
    if (before_scene_us < 100000 || now - last_tick < 2000) return;       /* at most one line every two seconds */
    last_tick = now;
    line[0] = 0;
    for (i = 0; i < 8 && n < 330; i++)
        if (g_waits[i].calls && g_waits[i].us >= 2000)
            n += wsprintfA(line + n, " %s from %08x: %u ms in %u calls;", g_waits[i].kind == 'S' ? "Sleep" : g_waits[i].kind == 'W' ? "WaitForSingleObject" : "select",
                           g_waits[i].ra, g_waits[i].us / 1000, g_waits[i].calls);
    logf("    stood still %u ms before drawing (repeated SetThreadPriority calls answered locally so far: %u). waits:%s | phase %d of %d, net factor bits %08x, limiter flag %u", before_scene_us / 1000, g_prio_skipped,
         n ? line : " none of the three kinds (busy, or another kind of wait)",
         eng && !IsBadReadPtr(eng + 0x38, 4) ? *(int *)(eng + 0x34) : -1, eng && !IsBadReadPtr(eng + 0x38, 4) ? *(int *)(eng + 0x38) : -1,
         *(DWORD *)0xd9f498, (UINT)*(BYTE *)0xde4320);
}

static HRESULT WINAPI lb_Present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    /* every 10 s: average fps, frame-time spread, time spent waiting in Present, and the mouse traffic */
    static DWORD t0, frames, max_us;
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER qpc, t;
    DWORD now = GetTickCount(), us;
    HRESULT hr;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpc);
    if (last.QuadPart)
    {
        us = (DWORD)((qpc.QuadPart - last.QuadPart) * 1000000 / freq.QuadPart);
        if (us > max_us) max_us = us;
        if (us < 12500) st.b_fast++; else if (us < 25000) st.b_mid++; else if (us < 50000) st.b_slow++; else st.b_hitch++;
        st.pace_sum += us; st.pace_sq += (double)us * us; st.pace_n++;
        sync_eval(us);
        if (!st.pace_min || us < st.pace_min) st.pace_min = us;
        if (us > 250000) logf("  long frame: %u ms", us / 1000);
        if (us > 25000)
        {
            static LARGE_INTEGER last_slow;
            logf("  slow frame %u ms (%u ms after the previous one): %u ms before the first scene, %u scene(s) %u ms, %u draws, "
                 "vb: %u locks (%u us in Lock, %u us filling), %u created, textures created %u",
                 us / 1000, last_slow.QuadPart ? us_between(&last_slow, &qpc) / 1000 : 0, fr.first_scene_us / 1000, fr.scenes,
                 fr.scene_us / 1000, fr.draws, fr.vb_locks, ticks_us(fr.vb_lock_ticks), ticks_us(fr.vb_fill_ticks), fr.vb_creates, fr.tex_creates);
            last_slow = qpc;
            if (fr.tex_creates)
            {
                UINT i;
                logf("    textures: %u created in %u us; %u locks (%u us in LockRect, %u us filling, %u us in UnlockRect); main thread %u",
                     fr.tex_creates, fr.tex_create_us, fr.tex_locks, fr.tex_lock_call_us, fr.tex_fill_us, fr.tex_unlock_us, GetCurrentThreadId());
                logf("    d3dx: FilterTexture %u calls %u us (filter %x), game filling the tiles before it %u us; LoadSurfaceFromMemory %u calls %u us (fmt %u filter %x); LoadSurfaceFromSurface %u calls %u us",
                     fr.dx_filter, fr.dx_filter_us, fr.dx_filter_flags, fr.tile_fill_us, fr.dx_loadmem, fr.dx_loadmem_us, fr.dx_loadmem_fmt, fr.dx_loadmem_filter, fr.dx_loadsurf, fr.dx_loadsurf_us);
                for (i = 0; i < fr.tex_creates && i < 2; i++)
                    logf("      %ux%u levels %u fmt %u usage %x pool %u: %u us on thread %u", fr.tex[i].w, fr.tex[i].h, fr.tex[i].levels,
                         fr.tex[i].fmt, fr.tex[i].usage, fr.tex[i].pool, fr.tex[i].us, fr.tex[i].tid);
            }
        }
    }
    waits_report(fr.first_scene_us);
    {
        /* network cushion: how many ticks of the other players' data are waiting (TheNetwork [0xde4468], vtbl+0x58; the
         * engine's own catch-up check at 0x63239d reads the same). More than 1 for long = this machine runs behind. */
        void **net = *(void ***)0xde4468;
        if (net && !IsBadReadPtr(net, 4) && !IsBadReadPtr(*net, 0x5c))
        {
            int n = ((int (__attribute__((thiscall)) *)(void *))((void **)*net)[0x58 / 4])(net);
            if (n < g_net_min || !g_net_n) g_net_min = n;
            if (n > g_net_max || !g_net_n) g_net_max = n;
            g_net_sum += n; g_net_n++;
        }
    }
    ZeroMemory(g_waits, sizeof(g_waits));
    g_main_tid = GetCurrentThreadId();
    rec_texcreates = fr.tex_creates;
    rec_frame(dev);
    {
        void **eng = (void **)0xde4324;      /* frame boundary for the first-phase latch */
        frame_len_tick();
        g_frame_no++;
        g_wd_frames++;
        if (!IsBadReadPtr(eng, 4) && *eng && !IsBadReadPtr(*eng, 0x40) && *(DWORD *)((BYTE *)*eng + 0x34) != 1) g_seen_other_phase = TRUE;
        if (!IsBadReadPtr(eng, 4) && *eng && !IsBadReadPtr(*eng, 0x60))
        {
            /* the engine's tick did not start this frame (phase counter above 6): how often, and for how many frames in a row */
            static DWORD run, last_tick; DWORD ph = *(DWORD *)((BYTE *)*eng + 0x34), tick = *(DWORD *)((BYTE *)*eng + 0x50);
            if (ph > 6) { g_hold_frames++; if (!run) g_holds++; run++; if (run > g_hold_longest) g_hold_longest = run; } else run = 0;
            if (tick != last_tick) { g_ticks_seen++; last_tick = tick; }
        }
    }
    ZeroMemory(&fr, sizeof(fr));
    fr.frame_start = qpc;
    g_freq = freq;
    last = qpc;
    if (!t0) t0 = now;
    frames++;
    if (now - t0 >= 10000)
    {
        logf("fps %u.%u | frames <12.5ms %u, <25ms %u, <50ms %u, longer %u (max %u ms) | per frame: present %u us (max %u), copy %u us | "
             "mouse: %u moves (max gap %u ms), game calls: SetCursorPos %u ClipCursor %u GetCursorPos %u, wheel %u",
             frames / 10, frames % 10, st.b_fast, st.b_mid, st.b_slow, st.b_hitch, max_us / 1000,
             frames ? st.present_us / frames : 0, st.present_max_us, frames ? st.blit_us / frames : 0,
             st.moves, st.move_gap_max_ms, st.setpos, st.clip, st.getpos, st.wheel);
        if (watch_addr) { logf("  watch %p: %u changes in this window, now %08x", watch_addr, watch_changes, IsBadReadPtr(watch_addr, 4) ? 0 : *watch_addr); watch_changes = 0; }
        if (st.pace_n > 1)
        {
            /* how evenly frames follow each other: a steady 120 fps is mean 8.33 ms with a spread well under 1 ms */
            double mean = st.pace_sum / st.pace_n, var = st.pace_sq / st.pace_n - mean * mean;
            DWORD sd = 0; if (var > 0) { double x = var, r = var; int k; for (k = 0; k < 30; k++) r = 0.5 * (r + x / r); sd = (DWORD)r; }
            logf("  pace: mean %u us, spread (sd) %u us, shortest %u us, longest %u us; vsync-locked %s (%u frames, waited %u us each; "
                 "game needed more than a refresh in %u, longest %u us; missed a refresh although the game was in time: %u; sleeps %u)",
                 (DWORD)mean, sd, st.pace_min, max_us, g_sync_on ? "on" : "off", g_sync_waits, g_sync_waits ? g_sync_wait_us / g_sync_waits : 0,
                 g_sync_late_work, g_sync_work_max, g_sync_late_wait, g_sync_sleeps);
            if (g_sync_on) logf("  frame clock: %s, %u missed refreshes, %u resyncs, limiter nudged %u times", o_timeGetTime ? "active" : "not hooked", g_vt_skips, g_vt_resyncs, g_vt_lim_pushed);
            g_sync_waits = g_sync_wait_us = g_sync_late_work = g_sync_late_wait = g_sync_work_max = g_sync_sleeps = g_vt_skips = g_vt_resyncs = g_vt_lim_pushed = 0;
        }
        if (g_fast_filters || g_slow_filters) { logf("  mip filters this window: %u fast, %u through d3dx", g_fast_filters, g_slow_filters); g_fast_filters = g_slow_filters = 0; }
        if (g_net_n) { logf("  network: ticks of the other side's data waiting: min %d, average %d.%d, max %d", g_net_min, (int)(g_net_sum / g_net_n), (int)(g_net_sum * 10 / g_net_n) % 10, g_net_max); g_net_n = 0; g_net_sum = 0; }
        if (frames) logf("  per frame: %u draws, %u vertex-buffer locks, %u index-buffer locks", g_win_draws / frames, g_win_vblocks / frames, g_win_iblocks / frames);
        g_win_draws = g_win_vblocks = g_win_iblocks = 0;
        { static DWORD kb_said, dec_said; if (g_heap_reserved_kb != kb_said || g_heap_decommits != dec_said) { logf("  heapkeep: %u MB of the game's reserved heap ranges kept committed so far, %u decommits kept accessible", g_heap_reserved_kb >> 10, g_heap_decommits); kb_said = g_heap_reserved_kb; dec_said = g_heap_decommits; } }
        logf("  logic: %u ticks started; a tick was held back %u times, %u frames in all, longest %u frames", g_ticks_seen, g_holds, g_hold_frames, g_hold_longest);
        g_holds = g_hold_frames = g_hold_longest = g_ticks_seen = 0;
        cswait_report(frames);
        prof_window(frames);
        if (cfg_spinwait && frames) { logf("  spinwait: per frame the helper thread stayed awake %u times and went to sleep %u times; the game's thread spun %u times", g_spin_cv / frames, g_spin_cv_slept / frames, g_spin_delay / frames); g_spin_cv = g_spin_cv_slept = g_spin_delay = 0; }
        { static DWORD prio_said; if (g_prio_skipped != prio_said) { logf("  repeated SetThreadPriority calls answered locally: %u in this window", g_prio_skipped - prio_said); prio_said = g_prio_skipped; } }
        ZeroMemory(&st, sizeof(st));
        t0 = now; frames = 0; max_us = 0;
    }

    if (watch_addr && !IsBadReadPtr(watch_addr, 4)) { DWORD v = *watch_addr; if (v != watch_last) { watch_changes++; watch_last = v; } }
    run_commands(dev);

    if (g.on && g.rt)
    {
        IDirect3DSurface9 *bb;
        QueryPerformanceCounter(&t);
        if (SUCCEEDED(o_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        {
            RECT r;
            if (g.dst.left > 0)
            {
                SetRect(&r, 0, 0, g.dst.left, g.sh); IDirect3DDevice9_ColorFill(dev, bb, &r, 0);
                SetRect(&r, g.dst.right, 0, g.sw, g.sh); IDirect3DDevice9_ColorFill(dev, bb, &r, 0);
            }
            if (g.dst.top > 0) { SetRect(&r, 0, 0, g.sw, g.dst.top); IDirect3DDevice9_ColorFill(dev, bb, &r, 0); }
            if (g.dst.bottom < (LONG)g.sh) { SetRect(&r, 0, g.dst.bottom, g.sw, g.sh); IDirect3DDevice9_ColorFill(dev, bb, &r, 0); }
            if (cfg_dark) IDirect3DDevice9_ColorFill(dev, bb, &g.dst, 0);      /* dark=1: the game draws as usual, the screen stays black */
            else IDirect3DDevice9_StretchRect(dev, g.rt, NULL, bb, &g.dst,
                    (g.dw == g.gw && g.dh == g.gh) ? D3DTEXF_NONE : D3DTEXF_LINEAR);
            if (cfg_showfps) draw_fps(dev, bb);
            if (shot_bb_name[0]) { shot_from_bb = TRUE; save_shot(dev, shot_bb_name, 3); shot_from_bb = FALSE; shot_bb_name[0] = 0; }
            IDirect3DSurface9_Release(bb);
        }
        st.blit_us += us_since(&t, &freq);
        enforce_clip(FALSE);
        QueryPerformanceCounter(&t);
        hr = o_Present(dev, NULL, NULL, wnd, NULL);
    }
    else
    {
        if (g.full)
        {
            IDirect3DSurface9 *bb = NULL;
            enforce_clip(FALSE);
            if ((cfg_showfps || shot_bb_name[0]) && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
            {
                if (cfg_showfps) draw_fps(dev, bb);
                if (shot_bb_name[0]) { shot_from_bb = TRUE; save_shot(dev, shot_bb_name, 3); shot_from_bb = FALSE; shot_bb_name[0] = 0; }
                IDirect3DSurface9_Release(bb);
            }
        }
        QueryPerformanceCounter(&t);
        hr = o_Present(dev, src, dst, wnd, dirty);
    }
    us = us_since(&t, &freq);
    st.present_us += us;
    if (us > st.present_max_us) st.present_max_us = us;
    if (SUCCEEDED(hr)) sync_wait(dev);
    return hr;
}

/* windowed=1: instead of a full-screen device at the current mode, a borderless window that covers the screen. A
 * Direct3D 9 library that would set a display mode for any full-screen device can then never touch the display. */
static void go_windowed(const D3DPRESENT_PARAMETERS *pp, D3DPRESENT_PARAMETERS *real, HWND wnd)
{
    MONITORINFO mi;
    if (!cfg_windowed || pp->Windowed || !wnd) return;
    real->Windowed = TRUE;
    real->FullScreen_RefreshRateInHz = 0;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    SetWindowLongW(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    logf("windowed: the game's window covers %d,%d %dx%d", (int)mi.rcMonitor.left, (int)mi.rcMonitor.top,
         (int)(mi.rcMonitor.right - mi.rcMonitor.left), (int)(mi.rcMonitor.bottom - mi.rcMonitor.top));
}
static HRESULT WINAPI lb_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    D3DPRESENT_PARAMETERS real;
    BOOL lb;
    HRESULT hr;
    drop_rt(dev);
    sync_release();
    lb = prepare(pp, &real);
    go_windowed(pp, &real, g.hwnd);
    hr = o_Reset(dev, &real);
    logf("Reset: game %ux%u windowed=%d -> real %ux%u hr=%08x", pp->BackBufferWidth, pp->BackBufferHeight,
         pp->Windowed, real.BackBufferWidth, real.BackBufferHeight, hr);
    g.on = lb;
    if (SUCCEEDED(hr))
    {
        pp->BackBufferCount = real.BackBufferCount;
        if (lb) create_rt(dev, &real);
    }
    return hr;
}

static HRESULT WINAPI lb_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    D3DPRESENT_PARAMETERS real;
    BOOL lb = prepare(pp, &real);
    HWND wnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    HRESULT hr;

    /* subclass before the real CreateDevice, so wined3d's own window proc sits above ours and puts ours back later */
    if (wnd && wnd != g.hwnd)
    {
        g.hwnd = wnd;
        g.unicode = IsWindowUnicode(wnd);
        g.prev_proc = g.unicode ? (WNDPROC)SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)lb_wndproc)
                                : (WNDPROC)SetWindowLongPtrA(wnd, GWLP_WNDPROC, (LONG_PTR)lb_wndproc);
    }
    if (cfg_steamoverlay && !GetModuleHandleA("lsteamclient.dll"))
    {
        /* NotProton's FEX runner brings the Steam overlay into a process through its Steam bridge (lsteamclient), which
         * only games that use the Steam API load. This game does not, so load the bridge ourselves. */
        HMODULE h = LoadLibraryA("lsteamclient.dll");
        logf("steam overlay: lsteamclient.dll %s (err %u)", h ? "loaded" : "NOT loaded", h ? 0 : GetLastError());
    }
    go_windowed(pp, &real, wnd);
    wd_start();
    hr = o_CreateDevice(d3d, adapter, type, focus, flags, &real, out);
    logf("CreateDevice: game %ux%u fmt=%d windowed=%d refresh=%u interval=%08x swap=%d -> real %ux%u refresh=0 interval=%08x hr=%08x",
         pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed, pp->FullScreen_RefreshRateInHz,
         pp->PresentationInterval, pp->SwapEffect, real.BackBufferWidth, real.BackBufferHeight, real.PresentationInterval, hr);
    g.on = lb;
    if (SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out;
        pp->BackBufferCount = real.BackBufferCount;
        prof_install(vt);
        spin_install();
        noglcheck_install();
        cswait_install();
        heapkeep_scan();
        crash_note_install();
        patch_slot(&vt[14], lb_GetSwapChain, (void **)&o_GetSwapChain);
        patch_slot(&vt[16], lb_Reset, (void **)&o_Reset);
        patch_slot(&vt[17], lb_Present, (void **)&o_Present);
        patch_slot(&vt[18], lb_GetBackBuffer, (void **)&o_GetBackBuffer);
        hook_d3dx();
        { static BOOL once; if (!once && cfg_jumpbudget) { once = TRUE; set_jump_budget(cfg_jumpbudget); } }
        patch_slot(&vt[23], lb_CreateTex, (void **)&o_CreateTex);
        patch_slot(&vt[26], lb_CreateVB, (void **)&o_CreateVB);
        patch_slot(&vt[27], lb_CreateIB, (void **)&o_CreateIB);
        patch_slot(&vt[41], lb_BeginScene, (void **)&o_BeginScene);
        patch_slot(&vt[42], lb_EndScene, (void **)&o_EndScene);
        patch_slot(&vt[81], lb_DP, (void **)&o_DP);
        patch_slot(&vt[82], lb_DIP, (void **)&o_DIP);
        if (lb) create_rt(*out, &real);
    }
    else g.on = FALSE;
    return hr;
}

/* The game only accepts a resolution it finds in the adapter's mode list, and the monitor has no 16:9 mode
 * at its own height. Add the sizes that fit inside the current desktop mode; they never reach the display. */
static UINT (WINAPI *o_GetAdapterModeCount)(IDirect3D9 *, UINT, D3DFORMAT);
static HRESULT (WINAPI *o_EnumAdapterModes)(IDirect3D9 *, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE *);
struct extras { BOOL valid; D3DFORMAT fmt; UINT count; D3DDISPLAYMODE modes[12]; };
static struct extras g_extras[6];
static UINT cfg_gamew, cfg_gameh;      /* the game resolution the launch wrapper picked (0 = not given) */

static struct extras *extras_for(IDirect3D9 *d3d, UINT adapter, D3DFORMAT fmt, UINT real_count)
{
    static const UINT aspects[][2] = { {16, 9}, {16, 10}, {4, 3} };
    static const UINT standard[][2] = { {3200, 1800}, {2560, 1440}, {2048, 1152}, {1920, 1080}, {1600, 900}, {1280, 720} };
    struct extras *e = NULL;
    UINT cand[12][2], nc = 0, i, c, ah;
    DEVMODEW dm;
    for (i = 0; i < 6; i++)
    {
        if (g_extras[i].valid && g_extras[i].fmt == fmt) return &g_extras[i];
        if (!g_extras[i].valid && !e) e = &g_extras[i];
    }
    if (!e) return &g_extras[0];
    e->valid = TRUE; e->fmt = fmt; e->count = 0;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth) return e;
    ah = dm.dmPelsHeight - (cfg_topinset < dm.dmPelsHeight / 4 ? cfg_topinset : 0);
    /* first the size the launch wrapper picked for this display (gamew/gameh in letterbox.ini: the display's own shape
     * between 4:3 and 16:9, with the HUD files built for it); without it the largest 16:9 picture that fits */
    if (cfg_gamew >= 640 && cfg_gameh >= 480 && cfg_gamew <= dm.dmPelsWidth && cfg_gameh <= ah)
    { cand[nc][0] = cfg_gamew; cand[nc][1] = cfg_gameh; nc++; }
    else
    {
        cand[nc][0] = (ah * 16 / 9 < dm.dmPelsWidth ? ah * 16 / 9 : dm.dmPelsWidth) & ~1u;
        cand[nc][1] = (cand[nc][0] * 9 / 16) & ~1u; nc++;
    }
    for (i = 0; i < 3; i++)
    {
        UINT w = (ah * aspects[i][0] / aspects[i][1]) & ~1u;
        if (w <= dm.dmPelsWidth && !(w == cand[0][0] && ah == cand[0][1])) { cand[nc][0] = w; cand[nc][1] = ah; nc++; }
    }
    for (i = 0; i < 6; i++)
        if (standard[i][0] <= dm.dmPelsWidth && standard[i][1] <= ah && !(standard[i][0] == cand[0][0] && standard[i][1] == cand[0][1]))
        { cand[nc][0] = standard[i][0]; cand[nc][1] = standard[i][1]; nc++; }
    for (c = 0; c < nc; c++)
    {
        D3DDISPLAYMODE m;
        BOOL dup = FALSE;
        for (i = 0; i < real_count && !dup; i++)
            if (SUCCEEDED(o_EnumAdapterModes(d3d, adapter, fmt, i, &m)) && m.Width == cand[c][0] && m.Height == cand[c][1]) dup = TRUE;
        if (dup) continue;
        e->modes[e->count].Width = cand[c][0];
        e->modes[e->count].Height = cand[c][1];
        e->modes[e->count].RefreshRate = dm.dmDisplayFrequency ? dm.dmDisplayFrequency : 60;
        e->modes[e->count].Format = fmt;
        e->count++;
    }
    logf("mode list for format %d: %u real + %u added (first added %ux%u)", fmt, real_count, e->count,
         e->count ? e->modes[0].Width : 0, e->count ? e->modes[0].Height : 0);
    return e;
}

static UINT WINAPI lb_GetAdapterModeCount(IDirect3D9 *d3d, UINT adapter, D3DFORMAT fmt)
{
    UINT n = o_GetAdapterModeCount(d3d, adapter, fmt);
    return n ? n + extras_for(d3d, adapter, fmt, n)->count : 0;
}

static HRESULT WINAPI lb_EnumAdapterModes(IDirect3D9 *d3d, UINT adapter, D3DFORMAT fmt, UINT i, D3DDISPLAYMODE *mode)
{
    UINT n = o_GetAdapterModeCount(d3d, adapter, fmt);
    struct extras *e;
    if (i < n || !n) return o_EnumAdapterModes(d3d, adapter, fmt, i, mode);
    e = extras_for(d3d, adapter, fmt, n);
    if (!mode || i - n >= e->count) return D3DERR_INVALIDCALL;
    *mode = e->modes[i - n];
    return D3D_OK;
}

__attribute__((used)) IDirect3D9 *WINAPI lb_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d;
    if (!real_Direct3DCreate9) { logf("the real d3d9.dll is not loaded"); return NULL; }
    d3d = real_Direct3DCreate9(sdk);
    if (d3d)
    {
        void **vt = *(void ***)d3d;
        patch_slot(&vt[6], lb_GetAdapterModeCount, (void **)&o_GetAdapterModeCount);
        patch_slot(&vt[7], lb_EnumAdapterModes, (void **)&o_EnumAdapterModes);
        patch_slot(&vt[16], lb_CreateDevice, (void **)&o_CreateDevice);
    }
    logf("Direct3DCreate9(%u) = %p", sdk, d3d);
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        WCHAR path[MAX_PATH], *slash;
        HMODULE exe = GetModuleHandleW(NULL);
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameW(inst, g_dir, MAX_PATH);
        if ((slash = wcsrchr(g_dir, L'\\'))) *slash = 0;
        lstrcpyW(path, g_dir); lstrcatW(path, L"\\letterbox.log");
        DeleteFileW(path);
        {
            WCHAR ini[MAX_PATH];
            lstrcpyW(ini, g_dir); lstrcatW(ini, L"\\letterbox.ini");
            cfg_vsync = (int)GetPrivateProfileIntW(L"letterbox", L"vsync", -1, ini);
            cfg_fastfilter = (int)GetPrivateProfileIntW(L"letterbox", L"fastfilter", 1, ini);
            cfg_jumpbudget = (int)GetPrivateProfileIntW(L"letterbox", L"jumpbudget", 0, ini);
            cfg_topinset = GetPrivateProfileIntW(L"letterbox", L"topinset", 0, ini);
            cfg_valign = (int)GetPrivateProfileIntW(L"letterbox", L"valign", 0, ini);
            cfg_showfps = (int)GetPrivateProfileIntW(L"letterbox", L"showfps", 0, ini);
            cfg_dark = (int)GetPrivateProfileIntW(L"letterbox", L"dark", 0, ini);
            cfg_profile = (int)GetPrivateProfileIntW(L"letterbox", L"profile", 0, ini);
            cfg_sample = (int)GetPrivateProfileIntW(L"letterbox", L"sample", 0, ini);
            cfg_spinwait = (int)GetPrivateProfileIntW(L"letterbox", L"spinwait", 0, ini);
            cfg_noglcheck = (int)GetPrivateProfileIntW(L"letterbox", L"noglcheck", 1, ini);
            cfg_watch = (int)GetPrivateProfileIntW(L"letterbox", L"watch", 1, ini);
            cfg_heapkeep = (int)GetPrivateProfileIntW(L"letterbox", L"heapkeep", 1, ini);
            cfg_cswait = (int)GetPrivateProfileIntW(L"letterbox", L"cswait", 0, ini);
            cfg_roundbuttons = (int)GetPrivateProfileIntW(L"letterbox", L"roundbuttons", 1, ini);
            cfg_gamefps = GetPrivateProfileIntW(L"letterbox", L"gamefps", 30, ini);
            cfg_gamew = GetPrivateProfileIntW(L"letterbox", L"gamew", 0, ini);
            cfg_gameh = GetPrivateProfileIntW(L"letterbox", L"gameh", 0, ini);
            cfg_logicinterp = (int)GetPrivateProfileIntW(L"letterbox", L"logicinterp", 1, ini);
            cfg_firstphase = (int)GetPrivateProfileIntW(L"letterbox", L"firstphase", 1, ini);
            cfg_horplus = (int)GetPrivateProfileIntW(L"letterbox", L"horplus", 1, ini);
            cfg_farguard = (int)GetPrivateProfileIntW(L"letterbox", L"farguard", 1, ini);
            cfg_ringround = (int)GetPrivateProfileIntW(L"letterbox", L"ringround", 1, ini);
            cfg_steamoverlay = (int)GetPrivateProfileIntW(L"letterbox", L"steamoverlay", 0, ini);
            cfg_ringtiming = (int)GetPrivateProfileIntW(L"letterbox", L"ringtiming", 1, ini);
            cfg_netfps = (int)GetPrivateProfileIntW(L"letterbox", L"netfps", 1, ini);
            cfg_realframelen = (int)GetPrivateProfileIntW(L"letterbox", L"realframelen", 1, ini);
            cfg_sync = (int)GetPrivateProfileIntW(L"letterbox", L"syncpresent", -1, ini);
            cfg_refresh = GetPrivateProfileIntW(L"letterbox", L"refresh", 0, ini);
            cfg_syncsleep = GetPrivateProfileIntW(L"letterbox", L"syncsleep", 3000, ini);
            cfg_frameclock = (int)GetPrivateProfileIntW(L"letterbox", L"frameclock", 1, ini);
            GetPrivateProfileStringW(L"letterbox", L"real", L"", cfg_real, 64, ini);
            cfg_windowed = (int)GetPrivateProfileIntW(L"letterbox", L"windowed", 0, ini);
        }
        logf("letterbox d3d9 proxy loaded (vsync=%d fastfilter=%d jumpbudget=%d topinset=%u syncpresent=%d refresh=%u)",
             cfg_vsync, cfg_fastfilter, cfg_jumpbudget, cfg_topinset, cfg_sync, cfg_refresh);
        if (cfg_real[0])
        {
            lstrcpyW(path, g_dir); lstrcatW(path, L"\\"); lstrcatW(path, cfg_real);
            g_real = LoadLibraryW(path);
            logf("real Direct3D 9 library: %ls %s (err %u)", cfg_real, g_real ? "loaded" : "NOT loaded, using the system's", g_real ? 0 : GetLastError());
        }
        if (!g_real)
        {
            GetSystemDirectoryW(path, MAX_PATH);       /* syswow64 for a 32-bit game */
            lstrcatW(path, L"\\d3d9.dll");
            g_real = LoadLibraryW(path);
        }
        if (!g_real || g_real == (HMODULE)inst) { logf("could not load the real d3d9.dll (err %u)", GetLastError()); return TRUE; }
        real_Direct3DCreate9 = (void *)GetProcAddress(g_real, "Direct3DCreate9");
#define RESOLVE(n) real_##n = (void *)GetProcAddress(g_real, #n);
        FWD(RESOLVE)
        iat_hook(exe, "user32.dll", "GetCursorPos", h_GetCursorPos);
        iat_hook(exe, "user32.dll", "SetCursorPos", h_SetCursorPos);
        iat_hook(exe, "user32.dll", "ClipCursor", h_ClipCursor);
        iat_hook(exe, "kernel32.dll", "SetThreadPriority", h_SetThreadPriority);
        if (cfg_heapkeep) { iat_hook(exe, "kernel32.dll", "VirtualAlloc", h_VirtualAlloc); iat_hook(exe, "kernel32.dll", "VirtualFree", h_VirtualFree); }
        if (cfg_ringround) iat_hook(exe, "kernel32.dll", "CreateFileA", h_CreateFileA);
        iat_hook(exe, "kernel32.dll", "Sleep", h_Sleep);                               /* wait recorder, see waits_report */
        iat_hook(exe, "kernel32.dll", "WaitForSingleObject", h_WaitForSingleObject);
        {
            HMODULE ws = GetModuleHandleA("ws2_32.dll");
            if (!ws) ws = GetModuleHandleA("wsock32.dll");
            if (ws && (o_select = (void *)GetProcAddress(ws, "select")) != NULL)
            { iat_hook(exe, "ws2_32.dll", "select", h_select); iat_hook(exe, "wsock32.dll", "select", h_select); }
        }
        iat_hook(exe, "user32.dll", "GetClientRect", h_GetClientRect);
        iat_hook(exe, "user32.dll", "GetWindowRect", h_GetWindowRect);
        iat_hook(exe, "user32.dll", "GetSystemMetrics", h_GetSystemMetrics);
    }
    return TRUE;
}
