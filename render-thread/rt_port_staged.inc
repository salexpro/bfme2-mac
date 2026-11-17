// ---- port: one map for a run of staged buffer writes ------------------------------------------------------------
// The accelerator's worker does Lock + copy + Unlock for every staged write (several hundred a frame in a battle).
// Under Wine on macOS each buffer Lock is a round trip to Wine's own command thread, which first has to issue every
// draw queued before it: worker and command thread end up taking turns instead of running side by side. A write made
// with NOOVERWRITE promises not to touch what earlier draws use, so it may reach the buffer early: when the worker
// comes to a staged write, the NOOVERWRITE writes to the same buffer that are already queued behind it (up to the
// buffer's next DISCARD) go into the same map, and their own records later only do their bookkeeping.
static volatile LONG g_rtPortBatchOn = 1, g_rtPortNMap = 0, g_rtPortNJoined = 0;
static void rtPortExecStaged(RtRec* r, DWORD* a) {
    typedef DWORD (__stdcall* LockF)(DWORD, DWORD, DWORD, void**, DWORD);
    if (!a[6]) {
        DWORD lo = a[1], hi = a[1] + a[2];
        RtRec* join[96]; int nj = 0;
        if (g_rtPortBatchOn && a[2]) {
            DWORD head = g_rtHead;                                   // everything before it is complete
            DWORD pos = (DWORD)((BYTE*)r - g_rtBuf) + r->size;
            for (int n = 0; n < 3000 && pos != head && nj < 96; ++n) {
                RtRec* q = (RtRec*)(g_rtBuf + pos);
                if (q->kind == 2 || !q->size) break;                 // the ring wraps here: far enough
                if (q->kind == 3) {
                    DWORD* qa = rtArgs(q);
                    if (qa[0] == a[0]) {
                        if (q->fn != r->fn || (qa[3] & 0x3010) != 0x1000 || qa[6] || !qa[2]) break;   // only NOOVERWRITE writes ride along; a DISCARD is new storage
                        DWORD ql = qa[1], qh = qa[1] + qa[2];
                        if (ql < hi && qh > lo) break;                                               // touches what is already in this map
                        if ((qh > hi ? qh : hi) - (ql < lo ? ql : lo) > (8u << 20)) break;
                        join[nj++] = q; if (ql < lo) lo = ql; if (qh > hi) hi = qh;
                    }
                }
                pos += q->size;
            }
        }
        void* mapped = NULL;
        if ((LONG)((LockF)r->fn)(a[0], lo, hi - lo, &mapped, a[3]) >= 0 && mapped) {
            memcpy((BYTE*)mapped + (a[1] - lo), (const void*)(ULONG_PTR)a[5], a[2]);
            for (int i = 0; i < nj; ++i) { DWORD* qa = rtArgs(join[i]); memcpy((BYTE*)mapped + (qa[1] - lo), (const void*)(ULONG_PTR)qa[5], qa[2]); qa[6] = 1; }
            g_rtPortNJoined += nj;
        }
        ((DWORD(__stdcall*)(DWORD))(ULONG_PTR)a[4])(a[0]);
        g_rtPortNMap++;
    }
    if (r->objMask & 1) ((DWORD(__stdcall*)(DWORD))(*(DWORD**)(ULONG_PTR)a[0])[2])(a[0]);
}
