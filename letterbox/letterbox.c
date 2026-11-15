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
    if (!g.on || !g.hwnd) return;
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
    if (g.on) { enforce_clip(FALSE); return TRUE; }
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
        else if (m == WM_KILLFOCUS || (m == WM_ACTIVATEAPP && !w))
        {
            if (g.clipped) { ClipCursor(NULL); g.clipped = FALSE; }
        }
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
    if ((int)(ret - g_vt_last_ret) < 0) ret = g_vt_last_ret;
    g_vt_last_ret = ret;
    return ret;
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
    if (!g.gw || !g.gh || (g.gw == g.sw && g.gh == g.sh)) return FALSE;
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

/* ---- where a frame's time goes (for the slow-frame lines in the log) ---- */
static struct {
    LARGE_INTEGER frame_start, scene_start, lock_ret;
    DWORD scene_us, scenes, draws, vb_locks, vb_lock_call_us, vb_fill_us, vb_creates, tex_creates, first_scene_us;
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
static HRESULT WINAPI lb_DIP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT start, UINT prims)
{ fr.draws++; return o_DIP(dev, t, base, minv, nv, start, prims); }
static HRESULT WINAPI lb_DP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT start, UINT prims)
{ fr.draws++; return o_DP(dev, t, start, prims); }
static HRESULT WINAPI lb_VBLock(IDirect3DVertexBuffer9 *vb, UINT off, UINT size, void **data, DWORD flags)
{
    LARGE_INTEGER t;
    HRESULT hr;
    QueryPerformanceCounter(&t);
    hr = o_VBLock(vb, off, size, data, flags);
    QueryPerformanceCounter(&fr.lock_ret);
    fr.vb_lock_call_us += us_between(&t, &fr.lock_ret);
    fr.vb_locks++;
    return hr;
}
static HRESULT WINAPI lb_VBUnlock(IDirect3DVertexBuffer9 *vb)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (fr.lock_ret.QuadPart) fr.vb_fill_us += us_between(&fr.lock_ret, &t);
    return o_VBUnlock(vb);
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
typedef void (__attribute__((thiscall)) *win_getsize_fn)(void *win, int *w, int *h);
typedef void (__attribute__((thiscall)) *tint_play_fn)(void *self, const float *color, UINT attack, UINT decay, UINT sustain);

static void __attribute__((thiscall)) h_RadialGetSize(void *win, int *w, int *h)
{
    ((win_getsize_fn)0x71548d)(win, w, h);
    if (g.gw && g.gh)
    {
        int w2 = MulDiv(*w, g.gh * 4, g.gw * 3);          /* undo the 4:3 -> picture-shape stretch */
        /* in that routine the locals are  w [ebp-2Ch]  h [ebp-28h]  x [ebp-24h]  y [ebp-20h] */
        if (w2 > 0 && w2 < *w) { w[2] += (*w - w2) / 2; *w = w2; }
    }
}

static double g_avg_frame_ms;      /* running average of the real frame time (set in frame_len_tick) */
static void __attribute__((thiscall)) h_TintPlay(void *self, const float *color, UINT attack, UINT decay, UINT sustain)
{
    /* How many drawn frames one 30 fps frame is right now. The envelope spends attack frames rising, then stays at
     * the peak for sustain + 3 frames (one to arrive, one to switch to "sustain", the countdown, one to release),
     * then decay frames falling - measured at 30 fps: selection flash = 4 frames at the peak + 4 falling = 266 ms. */
    double k = g_avg_frame_ms > 1.0 ? 33.3333 / g_avg_frame_ms : (cfg_gamefps > 30 ? cfg_gamefps / 30.0 : 1.0);
    if (k < 1.0) k = 1.0;
    if (k > 8.0) k = 8.0;
    ((tint_play_fn)0x674818)(self, color, (UINT)(attack * k + 0.5), (UINT)(decay * k + 0.5), (UINT)((sustain + 3) * k - 3 + 0.5));
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
static DWORD g_frame_no, g_p1_frame = 0xffffffff;
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

static void WINAPI h_SetFrameLen(float ms)
{
    if (ms > 1.0f && ms < 200.0f) g_fl_base = ms;        /* what the game asked for (33.3 normally) */
    *(DWORD *)0xdc7a8c = g_fl_ms;
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
typedef void (__attribute__((thiscall)) *set_view_plane_fn)(void *cam, float hfov, float vfov);
static void __attribute__((thiscall)) h_SetViewPlane(void *cam, float hfov, float vfov)
{
    if (vfov == -1.0f)
    {
        float a = *(float *)((BYTE *)cam + 0xe8);        /* the camera's width / height */
        if (a > 1.34f && a < 4.0f && hfov > 0.01f && hfov < 2.6f)
            hfov = 2.0f * (float)atan(tan(hfov * 0.5) * (a * 0.75));
    }
    ((set_view_plane_fn)0x533590)(cam, hfov, vfov);
}

static void patch_game_code(void)
{
    static const DWORD tint_sites[] = { 0x6757aa, 0x67597d, 0x675dd1, 0x675e1c, 0x675e86, 0x675ed4, 0x67910e };
    int i, n = 0;
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
    hr = o_TexLock(t, level, lr, rc, flags);
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
    hr = o_TexUnlock(t, level);
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
                 fr.scene_us / 1000, fr.draws, fr.vb_locks, fr.vb_lock_call_us, fr.vb_fill_us, fr.vb_creates, fr.tex_creates);
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
    rec_texcreates = fr.tex_creates;
    rec_frame(dev);
    {
        void **eng = (void **)0xde4324;      /* frame boundary for the first-phase latch */
        frame_len_tick();
        g_frame_no++;
        if (!IsBadReadPtr(eng, 4) && *eng && !IsBadReadPtr(*eng, 0x40) && *(DWORD *)((BYTE *)*eng + 0x34) != 1) g_seen_other_phase = TRUE;
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
            IDirect3DDevice9_StretchRect(dev, g.rt, NULL, bb, &g.dst,
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
        QueryPerformanceCounter(&t);
        hr = o_Present(dev, src, dst, wnd, dirty);
    }
    us = us_since(&t, &freq);
    st.present_us += us;
    if (us > st.present_max_us) st.present_max_us = us;
    if (SUCCEEDED(hr)) sync_wait(dev);
    return hr;
}

static HRESULT WINAPI lb_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    D3DPRESENT_PARAMETERS real;
    BOOL lb;
    HRESULT hr;
    drop_rt(dev);
    sync_release();
    lb = prepare(pp, &real);
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
    hr = o_CreateDevice(d3d, adapter, type, focus, flags, &real, out);
    logf("CreateDevice: game %ux%u fmt=%d windowed=%d refresh=%u interval=%08x swap=%d -> real %ux%u refresh=0 interval=%08x hr=%08x",
         pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed, pp->FullScreen_RefreshRateInHz,
         pp->PresentationInterval, pp->SwapEffect, real.BackBufferWidth, real.BackBufferHeight, real.PresentationInterval, hr);
    g.on = lb;
    if (SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out;
        pp->BackBufferCount = real.BackBufferCount;
        patch_slot(&vt[14], lb_GetSwapChain, (void **)&o_GetSwapChain);
        patch_slot(&vt[16], lb_Reset, (void **)&o_Reset);
        patch_slot(&vt[17], lb_Present, (void **)&o_Present);
        patch_slot(&vt[18], lb_GetBackBuffer, (void **)&o_GetBackBuffer);
        hook_d3dx();
        { static BOOL once; if (!once && cfg_jumpbudget) { once = TRUE; set_jump_budget(cfg_jumpbudget); } }
        patch_slot(&vt[23], lb_CreateTex, (void **)&o_CreateTex);
        patch_slot(&vt[26], lb_CreateVB, (void **)&o_CreateVB);
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
    /* the size the launch wrapper picks: the largest 16:9 picture that fits the usable area (the game lays its
     * widescreen picture and HUD out for exactly 16:9; any other shape gets squeezed) */
    cand[nc][0] = (ah * 16 / 9 < dm.dmPelsWidth ? ah * 16 / 9 : dm.dmPelsWidth) & ~1u;
    cand[nc][1] = (cand[nc][0] * 9 / 16) & ~1u; nc++;
    for (i = 0; i < 3; i++)
    {
        UINT w = (ah * aspects[i][0] / aspects[i][1]) & ~1u;
        if (w <= dm.dmPelsWidth && w != cand[0][0]) { cand[nc][0] = w; cand[nc][1] = ah; nc++; }
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
            cfg_roundbuttons = (int)GetPrivateProfileIntW(L"letterbox", L"roundbuttons", 1, ini);
            cfg_gamefps = GetPrivateProfileIntW(L"letterbox", L"gamefps", 30, ini);
            cfg_logicinterp = (int)GetPrivateProfileIntW(L"letterbox", L"logicinterp", 1, ini);
            cfg_firstphase = (int)GetPrivateProfileIntW(L"letterbox", L"firstphase", 1, ini);
            cfg_horplus = (int)GetPrivateProfileIntW(L"letterbox", L"horplus", 1, ini);
            cfg_steamoverlay = (int)GetPrivateProfileIntW(L"letterbox", L"steamoverlay", 0, ini);
            cfg_ringtiming = (int)GetPrivateProfileIntW(L"letterbox", L"ringtiming", 1, ini);
            cfg_netfps = (int)GetPrivateProfileIntW(L"letterbox", L"netfps", 1, ini);
            cfg_realframelen = (int)GetPrivateProfileIntW(L"letterbox", L"realframelen", 1, ini);
            cfg_sync = (int)GetPrivateProfileIntW(L"letterbox", L"syncpresent", -1, ini);
            cfg_refresh = GetPrivateProfileIntW(L"letterbox", L"refresh", 0, ini);
            cfg_syncsleep = GetPrivateProfileIntW(L"letterbox", L"syncsleep", 3000, ini);
            cfg_frameclock = (int)GetPrivateProfileIntW(L"letterbox", L"frameclock", 1, ini);
        }
        logf("letterbox d3d9 proxy loaded (vsync=%d fastfilter=%d jumpbudget=%d topinset=%u syncpresent=%d refresh=%u)",
             cfg_vsync, cfg_fastfilter, cfg_jumpbudget, cfg_topinset, cfg_sync, cfg_refresh);
        GetSystemDirectoryW(path, MAX_PATH);       /* syswow64 for a 32-bit game */
        lstrcatW(path, L"\\d3d9.dll");
        g_real = LoadLibraryW(path);
        if (!g_real || g_real == (HMODULE)inst) { logf("could not load the real d3d9.dll (err %u)", GetLastError()); return TRUE; }
        real_Direct3DCreate9 = (void *)GetProcAddress(g_real, "Direct3DCreate9");
#define RESOLVE(n) real_##n = (void *)GetProcAddress(g_real, #n);
        FWD(RESOLVE)
        iat_hook(exe, "user32.dll", "GetCursorPos", h_GetCursorPos);
        iat_hook(exe, "user32.dll", "SetCursorPos", h_SetCursorPos);
        iat_hook(exe, "user32.dll", "ClipCursor", h_ClipCursor);
        iat_hook(exe, "user32.dll", "GetClientRect", h_GetClientRect);
        iat_hook(exe, "user32.dll", "GetWindowRect", h_GetWindowRect);
        iat_hook(exe, "user32.dll", "GetSystemMetrics", h_GetSystemMetrics);
    }
    return TRUE;
}
