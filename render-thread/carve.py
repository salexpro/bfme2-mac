# Makes the port's copies of the accelerator's render-thread sources from the untouched originals (repeatable).
import sys, os
src, out = sys.argv[1], sys.argv[2]
def lines(name): return open(os.path.join(src, name), encoding='utf-8', errors='replace').read().split('\n')
rt = lines('aotr_rt.inc')
def expect(n, text):
    if text not in rt[n - 1]: raise SystemExit('aotr_rt.inc line %d is not what the carve-out expects: %r' % (n, rt[n - 1][:80]))
def off(a, b, why):                       # lines a..b (1-based, inclusive) compiled out
    rt[a - 1] = '#if 0   /* port: ' + why + ' */\n' + rt[a - 1]
    rt[b - 1] = rt[b - 1] + '\n#endif'
expect(662, 'static void rtExec(RtRec* r) {'); expect(734, 'if (r->kind == 3) {'); expect(740, 'return;'); expect(741, '}')
for i in range(735, 740): rt[i - 1] = ''                 # the staged write: Lock, copy, Unlock, release
rt[734] = '        rtPortExecStaged(r, a);                           /* port: see rt_port_staged.inc */'
rt[661] = '#include "rt_port_staged.inc"\n' + rt[661]
expect(2630, 'CreateThread(NULL, 0, rtKeyThread'); rt[2629] = '    /* port: no hotkey thread */'
expect(2724, 'GetExceptionCode()'); rt[2723] = rt[2723].replace('(unsigned long)GetExceptionCode()', '0ul')
expect(2729, 'static DWORD WINAPI rtKeyThread'); expect(2765, '}'); off(2729, 2765, 'the Scroll Lock hotkey thread')
expect(2768, 'static DWORD WINAPI rtReport'); expect(2964, '}'); off(2768, 2964, 'the statistics thread reports on the whole accelerator')
expect(2714, '// release the probes'); rt[2713] = '        if (ok) rtPortAfterInstall();     /* port: see rt_port.cpp */\n' + rt[2713]
expect(3145, 'static void capFrameTick'); expect(3151, '}'); off(3145, 3151, 'needs the engine hooks')
expect(1596, 'static DWORD __stdcall rt_surf_13(void* self, DWORD a0, DWORD a1, DWORD a2) {')
rt[1595] += '\n    { DWORD pr_; if (rtPortSurfLockTry(self, a0, a1, a2, &pr_)) return pr_; }   /* port */'
expect(1659, 'static DWORD __stdcall rt_surf_14(void* self) {')
rt[1658] += '\n    if (rtPortSurfUnlockTry(self)) return 0;   /* port */'
open(os.path.join(out, 'aotr_rt.inc'), 'w', encoding='utf-8').write('\n'.join(rt))
gen = lines('aotr_rt_gen.inc')
def gexpect(n, text):
    if text not in gen[n - 1]: raise SystemExit('aotr_rt_gen.inc line %d is not what the carve-out expects: %r' % (n, gen[n - 1][:80]))
gexpect(46, 'static DWORD __stdcall rt_dev_3(void* self) {'); gexpect(53, 'g_rtNLocked++; return r;')
gen[45] += '\n    { DWORD pr_; if (rtPortTCLTry(&pr_)) return pr_; }   /* port */'
gen[52] = gen[52].replace('g_rtNLocked++; return r;', 'g_rtNLocked++; rtPortTCLResult(r); return r;')
gexpect(1782, 'static DWORD __stdcall rt_tex_0(void* self, DWORD a0, DWORD a1) {')
gen[1781] += '\n    { DWORD pr_; if (rtPortTexQITry(self, a0, a1, &pr_)) return pr_; }   /* port */'
open(os.path.join(out, 'aotr_rt_gen.inc'), 'w', encoding='utf-8').write('\n'.join(gen))
for name in ('aotr_rtmirror.inc', 'aotr_capture.inc'):
    open(os.path.join(out, name), 'w', encoding='utf-8').write('\n'.join(lines(name)))
