/* seh32.inc — x86 (32-bit) structured exception handling: the fs:[0] registration chain (EXCEPTION_REGISTRATION {next@0, handler@4}),
 * RaiseException / RtlUnwind, msvcrt's _except_handler3 / _except_handler4_common (__try/__except/__finally scope tables),
 * _local_unwind2/4, _global_unwind2 and __CxxFrameHandler (MSVC x86 C++ EH: FuncInfo arrives in eax from the per-function thunk).
 * Handlers are cdecl: disposition handler(EXCEPTION_RECORD*, frame, CONTEXT*, dispatcher). Filters/funclets run with ebp = the
 * owning function's ebp. Follows Wine's dlls/ntdll/signal_i386.c, dlls/msvcrt/except_i386.c and cppexcept.c. Included by win32.c. */
enum { CONT_DISPATCH32 = 320, CONT_UNWIND32, CONT_EH3, CONT_CXX32 };
#define EXREC32_SIZE 80
#define CTX32_SIZE 0x2CC
#define CHAIN_END 0xffffffffull
enum { PH3_FILTER = 1, PH3_GUNWIND, PH3_LUNWIND, PH3_LUNWIND_ONLY };
enum { PX_COPY = 1, PX_GUNWIND, PX_LUNWIND, PX_LUNWIND_ONLY, PX_CATCHRUN, PX_DTOR };

static u64 fs0(Cpu *c) { return rd32(c, c->fs_base); }
static void set_fs0(Cpu *c, u64 v) { wr32(c, c->fs_base, (u32)v); }
static void stack_bounds32(Cpu *c, u64 *lo, u64 *hi) { Win32 *w = W; Thread *t = &w->th[w->cur_th]; if (t->stack && t->stack_size) { *lo = t->stack; *hi = t->stack + t->stack_size; } else { *lo = G_STACK_BASE; *hi = G_STACK_BASE + G_STACK_SIZE; } }
static int frame_ok32(Cpu *c, u64 f) { u64 lo, hi; stack_bounds32(c, &lo, &hi); return f != CHAIN_END && f >= lo && f + 8 <= hi && !(f & 3); }

/* CONTEXT (x86): ContextFlags@0 SegGs@0x8C SegFs@0x90 SegEs@0x94 SegDs@0x98 Edi@0x9C Esi@0xA0 Ebx@0xA4 Edx@0xA8 Ecx@0xAC Eax@0xB0 Ebp@0xB4 Eip@0xB8 SegCs@0xBC EFlags@0xC0 Esp@0xC4 SegSs@0xC8 ExtendedRegisters(FXSAVE)@0xCC */
static void ctx32_capture(Cpu *c, u64 a) {
  u8 *p = mem_ptr(c->mem, a, CTX32_SIZE, 1); if (!p) return; e_memset(p, 0, CTX32_SIZE);
  wr32(c, a, 0x1003f);
  wr32(c, a + 0x90, 0x3b); wr32(c, a + 0x94, 0x23); wr32(c, a + 0x98, 0x23); wr32(c, a + 0xBC, 0x1b); wr32(c, a + 0xC8, 0x23);
  wr32(c, a + 0x9C, (u32)c->r[RDI]); wr32(c, a + 0xA0, (u32)c->r[RSI]); wr32(c, a + 0xA4, (u32)c->r[RBX]); wr32(c, a + 0xA8, (u32)c->r[RDX]);
  wr32(c, a + 0xAC, (u32)c->r[RCX]); wr32(c, a + 0xB0, (u32)c->r[RAX]); wr32(c, a + 0xB4, (u32)c->r[RBP]); wr32(c, a + 0xB8, (u32)c->rip);
  wr32(c, a + 0xC0, (u32)c->rflags); wr32(c, a + 0xC4, (u32)c->r[RSP]);
  wr32(c, a + 0xCC + 24, c->mxcsr);
  for (int i = 0; i < 8; i++) { wr64(c, a + 0xCC + 160 + 16 * i, c->x[i].q[0]); wr64(c, a + 0xCC + 168 + 16 * i, c->x[i].q[1]); }
}
static void ctx32_load(Cpu *c, u64 a) {
  c->r[RDI] = rd32(c, a + 0x9C); c->r[RSI] = rd32(c, a + 0xA0); c->r[RBX] = rd32(c, a + 0xA4); c->r[RDX] = rd32(c, a + 0xA8);
  c->r[RCX] = rd32(c, a + 0xAC); c->r[RAX] = rd32(c, a + 0xB0); c->r[RBP] = rd32(c, a + 0xB4); c->rip = rd32(c, a + 0xB8);
  c->rflags = (c->rflags & ~0x8d5ull) | (rd32(c, a + 0xC0) & 0x8d5u); c->r[RSP] = rd32(c, a + 0xC4);
  if (rd32(c, a) & 0x20) for (int i = 0; i < 8; i++) { c->x[i].q[0] = rd64(c, a + 0xCC + 160 + 16 * i); c->x[i].q[1] = rd64(c, a + 0xCC + 168 + 16 * i); }
}

/* ---- phase 1: walk the chain, call every handler until one continues execution or jumps away ---- */
/* cont data: 0 rec 1 ctx 2 current node 3 dispatcher scratch 4 EXCEPTION_POINTERS 5 stage (0 chain, 1 vectored, 2 top-level filter) */
static void dispatch32_step(Cpu *c, int ci) {
  Win32 *w = W; u64 rec = w->cont[ci].data[0], ctx = w->cont[ci].data[1], node = w->cont[ci].data[2];
  if (!frame_ok32(c, node)) {
    u32 code = rd32(c, rec); u64 addr = rd32(c, rec + 12);
    if (w->uef && w->cont[ci].data[5] != 2) {
      w->cont[ci].data[5] = 2;
      seh_log("[x64] unhandled exception 0x%08x -> top-level filter 0x%llx\n", code, (unsigned long long)w->uef);
      guest_call(c, w->uef, w->cont[ci].data[4], 0, 0, 0); return;
    }
    const char *hw = (w->hw_msg[0] && code == w->hw_code) ? w->hw_msg : "";
    w32_report(w, "unhandled exception 0x%08x (%s) raised at 0x%llx%s%s%s", code, exc_name(code), (unsigned long long)addr, hw[0] ? " (" : "", hw, hw[0] ? ")" : "");
    cpu_fault(c, "unhandled exception 0x%08x (%s) at 0x%llx — no frame caught it%s%s%s", code, exc_name(code), (unsigned long long)addr, hw[0] ? " (" : "", hw, hw[0] ? ")" : ""); c->err_rip = addr; return;
  }
  u64 handler = rd32(c, node + 4);
  seh_log("[x64]   frame 0x%llx handler 0x%llx\n", (unsigned long long)node, (unsigned long long)handler);
  guest_call(c, handler, rec, node, ctx, w->cont[ci].data[3]);
}
static void raise_exception32(Cpu *c, u32 code, u32 flags, u32 nargs, const u64 *args, u64 address) {
  Win32 *w = W; if (nargs > 15) nargs = 15;
  u64 octx_rip = c->rip, octx_rsp = c->r[RSP];
  u64 sc = seh_scratch(c, EXREC32_SIZE + CTX32_SIZE + 64);
  u64 rec = sc, ctx = (sc + EXREC32_SIZE + 15) & ~15ull, ptrs = ctx + CTX32_SIZE, dc = ptrs + 8;
  u8 *rp = mem_ptr(c->mem, rec, EXREC32_SIZE, 1); if (!rp) { cpu_fault(c, "RaiseException: no stack"); return; }
  e_memset(rp, 0, EXREC32_SIZE); wr32(c, rec, code); wr32(c, rec + 4, flags & 1); wr32(c, rec + 12, (u32)address); wr32(c, rec + 16, nargs);
  for (u32 i = 0; i < nargs; i++) wr32(c, rec + 20 + 4 * i, (u32)args[i]);
  { u64 srip = c->rip, srsp = c->r[RSP]; c->rip = octx_rip; c->r[RSP] = octx_rsp; ctx32_capture(c, ctx); c->rip = srip; c->r[RSP] = srsp; }
  wr32(c, ptrs, (u32)rec); wr32(c, ptrs + 4, (u32)ctx);
  int ci = cont_push(c, CONT_DISPATCH32); if (ci < 0) return;
  w->cont[ci].data[0] = rec; w->cont[ci].data[1] = ctx; w->cont[ci].data[2] = fs0(c); w->cont[ci].data[3] = dc; w->cont[ci].data[4] = ptrs; w->cont[ci].data[5] = 0;
  seh_log("[x64] RaiseException 0x%08x (%s) at 0x%llx (x86 chain head 0x%llx)\n", code, exc_name(code), (unsigned long long)address, (unsigned long long)fs0(c));
  if (code == 0xc0000005u) seh_log("[x64]   fault addr %llx write=%d exc_info %llx\n", (unsigned long long)c->mem->fault_addr, c->mem->fault_write, (unsigned long long)c->exc_info);
  if (code == 0xc0000005u) seh_log("[x64]   eax=%llx ecx=%llx edx=%llx ebx=%llx esp=%llx ebp=%llx esi=%llx edi=%llx [esp]=%x [esp+4]=%x [esp+8]=%x [esp+c]=%x\n", (unsigned long long)c->r[RAX], (unsigned long long)c->r[RCX], (unsigned long long)c->r[RDX], (unsigned long long)c->r[RBX], (unsigned long long)c->r[RSP], (unsigned long long)c->r[RBP], (unsigned long long)c->r[RSI], (unsigned long long)c->r[RDI], rd32(c, c->r[RSP]), rd32(c, c->r[RSP] + 4), rd32(c, c->r[RSP] + 8), rd32(c, c->r[RSP] + 12));
  if (code == 0xc0000005u) { u64 bp = c->r[RBP]; for (int k = 0; k < 8 && bp; k++) { seh_log("[x64]   frame ebp=%llx ret=%x args %x %x %x %x\n", (unsigned long long)bp, rd32(c, bp + 4), rd32(c, bp + 8), rd32(c, bp + 12), rd32(c, bp + 16), rd32(c, bp + 20)); bp = rd32(c, bp); } }
  if (address == 0 && code == 0xC0000005) seh_log("[x64]   call to NULL from 0x%x (esp 0x%llx)\n", rd32(c, c->r[RSP]), (unsigned long long)c->r[RSP]);
  if (address == 0 && code == 0xC0000005) { char hb[600]; int n = 0; for (int k = 24; k >= 1; k--) n += e_snprintf(hb + n, sizeof hb - n, " %llx", (unsigned long long)c->hist[(c->hist_i - k) & 4095]); seh_log("[x64]   last blocks:%s\n", hb); }
  if (w->nveh) { w->cont[ci].data[5] = 1; w->cont[ci].idx = 1; guest_call(c, w->veh[0], ptrs, 0, 0, 0); return; }
  dispatch32_step(c, ci);
}

/* ---- phase 2: RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue) — call the handlers below the target with EXCEPTION_UNWINDING, unlink them ---- */
/* cont data: 0 target 1 target_ip 2 rec 3 retval 4 ctx 5 dispatcher scratch 6 node being called 7 internal (1 = started by a handler of ours: resume it when done) 8 caller esp */
static void seh32_after_unwind(Cpu *c);
static void unwind32_step(Cpu *c, int ci) {
  Win32 *w = W; u64 target = w->cont[ci].data[0], rec = w->cont[ci].data[2];
  u64 node = fs0(c);
  if (node == target || !frame_ok32(c, node)) {
    if (target && node != target) { cpu_fault(c, "RtlUnwind: target frame 0x%llx not found in the fs:[0] chain (head 0x%llx)", (unsigned long long)target, (unsigned long long)node); return; }
    u32 fl = rd32(c, rec + 4) & ~(EXC_UNWINDING | EXC_EXIT_UNWIND); wr32(c, rec + 4, fl);
    if (w->cont[ci].data[7]) { w->ncont = ci; seh32_after_unwind(c); return; }
    u64 ret = w->cont[ci].ret_addr, retval = w->cont[ci].data[3], tip = w->cont[ci].data[1], esp = w->cont[ci].data[8];
    w->ncont = ci; ctx32_load(c, w->cont[ci].data[4]);
    c->r[RSP] = esp + 16; c->rip = tip ? tip : ret; c->r[RAX] = retval; cont_gc(c);
    seh_log("[x64] RtlUnwind done -> 0x%llx esp 0x%llx\n", (unsigned long long)c->rip, (unsigned long long)c->r[RSP]);
    return;
  }
  u64 handler = rd32(c, node + 4); w->cont[ci].data[6] = node;
  seh_log("[x64]   unwind frame 0x%llx handler 0x%llx\n", (unsigned long long)node, (unsigned long long)handler);
  guest_call(c, handler, rec, node, w->cont[ci].data[4], w->cont[ci].data[5]);
}
static void start_unwind32(Cpu *c, u64 target, u64 target_ip, u64 rec, u64 retval, int internal) {
  Win32 *w = W; u64 caller_rsp = c->r[RSP];
  u64 sc = seh_scratch(c, EXREC32_SIZE + CTX32_SIZE + 64);
  u64 ctx = (sc + 15) & ~15ull, nrec = ctx + CTX32_SIZE, dc = nrec + EXREC32_SIZE;
  if (!rec) { u8 *rp = mem_ptr(c->mem, nrec, EXREC32_SIZE, 1); if (rp) e_memset(rp, 0, EXREC32_SIZE); wr32(c, nrec, STATUS_UNWIND); wr32(c, nrec + 12, (u32)c->rip); rec = nrec; }
  u32 fl = rd32(c, rec + 4) | EXC_UNWINDING; if (!target) fl |= EXC_EXIT_UNWIND; wr32(c, rec + 4, fl);
  { u64 srsp = c->r[RSP]; c->r[RSP] = caller_rsp; ctx32_capture(c, ctx); c->r[RSP] = srsp; }
  int ci = cont_push(c, CONT_UNWIND32); if (ci < 0) return;
  w->cont[ci].data[0] = target; w->cont[ci].data[1] = target_ip; w->cont[ci].data[2] = rec; w->cont[ci].data[3] = retval; w->cont[ci].data[4] = ctx; w->cont[ci].data[5] = dc; w->cont[ci].data[7] = internal; w->cont[ci].data[8] = caller_rsp;
  seh_log("[x64] RtlUnwind target frame 0x%llx ip 0x%llx (head 0x%llx)\n", (unsigned long long)target, (unsigned long long)target_ip, (unsigned long long)fs0(c));
  unwind32_step(c, ci);
}

/* ---- msvcrt _except_handler3 / _except_handler4: frame = { prev@0 handler@4 scopetable@8 trylevel@12 } with ebp = frame+16, saved esp at [ebp-0x18] = frame-8,
 *      EXCEPTION_POINTERS* at [ebp-0x14] = frame-4; scope entries { prevLevel@0 filter@4 handler@8 } (EH4: 16-byte cookie header first, TRYLEVEL_NONE = -2) ---- */
/* cont data: 0 rec 1 frame 2 ctx 3 scopetable 4 trylevel (search) 5 phase 6 eh4 7 local-unwind stop level */
#define EH3_D(k) (w->cont[ci].data[k])
static s32 eh3_none(Win32 *w, int ci) { return EH3_D(6) ? -2 : -1; }
static u64 eh3_entry(Win32 *w, int ci, s32 lvl) { return EH3_D(3) + (EH3_D(6) ? 16 : 0) + 12ull * (u32)lvl; }
/* run the __finally funclets from frame->trylevel down to data[7]; 1 while a funclet is executing */
static int eh3_local_unwind(Cpu *c, int ci) {
  Win32 *w = W; u64 frame = EH3_D(1); s32 stop = (s32)EH3_D(7), none = eh3_none(w, ci);
  for (int guard = 0; guard < 4096; guard++) {
    s32 tl = (s32)rd32(c, frame + 12); if (tl == stop || tl == none) return 0;
    if (tl < 0) { cpu_fault(c, "_except_handler3: bad trylevel %d", tl); return 1; }
    u64 e = eh3_entry(w, ci, tl); wr32(c, frame + 12, rd32(c, e));
    if (!rd32(c, e + 4)) { u64 h = rd32(c, e + 8); seh_log("[x64]   __finally 0x%llx (level %d)\n", (unsigned long long)h, tl); c->r[RBP] = frame + 16; guest_call(c, h, 0, 0, 0, 0); return 1; }
  }
  cpu_fault(c, "_except_handler3: runaway local unwind"); return 1;
}
static void eh3_search(Cpu *c, int ci) {
  Win32 *w = W; u64 frame = EH3_D(1); s32 none = eh3_none(w, ci);
  for (int guard = 0; guard < 4096; guard++) {
    s32 tl = (s32)EH3_D(4); if (tl == none) { cont_finish(c, 1); return; }
    if (tl < 0) { cpu_fault(c, "_except_handler3: bad trylevel %d", tl); return; }
    u64 e = eh3_entry(w, ci, tl); u64 filter = rd32(c, e + 4);
    if (filter) {
      u64 ptrs = seh_scratch(c, 16); wr32(c, ptrs, (u32)EH3_D(0)); wr32(c, ptrs + 4, (u32)EH3_D(2)); wr32(c, frame - 4, (u32)ptrs);
      EH3_D(5) = PH3_FILTER; seh_log("[x64]   __except filter 0x%llx (level %d)\n", (unsigned long long)filter, tl);
      c->r[RBP] = frame + 16; guest_call(c, filter, 0, 0, 0, 0); return;
    }
    EH3_D(4) = (u64)(u32)rd32(c, e);
  }
  cpu_fault(c, "_except_handler3: runaway scope table");
}
static void eh3_jump(Cpu *c, int ci) {   /* enter the __except block: ebp/esp of the owning function, never returns to us */
  Win32 *w = W; u64 frame = EH3_D(1); s32 tl = (s32)EH3_D(4); u64 e = eh3_entry(w, ci, tl);
  wr32(c, frame + 12, rd32(c, e)); u64 h = rd32(c, e + 8);
  w->ncont = ci; c->r[RBP] = frame + 16; c->r[RSP] = rd32(c, frame - 8); c->rip = h; cont_gc(c);
  seh_log("[x64] __except block 0x%llx ebp 0x%llx esp 0x%llx\n", (unsigned long long)h, (unsigned long long)c->r[RBP], (unsigned long long)c->r[RSP]);
}
static void eh3_start(Cpu *c, u64 rec, u64 frame, u64 ctx, int eh4, u64 cookie) {
  Win32 *w = W; int ci = cont_push(c, CONT_EH3); if (ci < 0) return;
  EH3_D(0) = rec; EH3_D(1) = frame; EH3_D(2) = ctx; EH3_D(3) = (u32)(rd32(c, frame + 8) ^ (eh4 ? (u32)cookie : 0)); EH3_D(6) = eh4;
  u32 fl = rd32(c, rec + 4);
  if (fl & (EXC_UNWINDING | EXC_EXIT_UNWIND)) { EH3_D(7) = (u64)(u32)eh3_none(w, ci); EH3_D(5) = PH3_LUNWIND_ONLY; if (!eh3_local_unwind(c, ci)) cont_finish(c, 1); return; }
  EH3_D(4) = (u64)(u32)rd32(c, frame + 12); eh3_search(c, ci);
}
static void s__except_handler3(Cpu *c) { eh3_start(c, ARG(0), ARG(1), ARG(2), 0, 0); }
static void s__except_handler4_common(Cpu *c) { eh3_start(c, ARG(2), ARG(3), ARG(4), 1, rd32(c, ARG(0))); }
static void local_unwind_start(Cpu *c, u64 frame, s32 stop, int eh4, u64 cookie) {
  Win32 *w = W; int ci = cont_push(c, CONT_EH3); if (ci < 0) return;
  EH3_D(1) = frame; EH3_D(3) = (u32)(rd32(c, frame + 8) ^ (eh4 ? (u32)cookie : 0)); EH3_D(6) = eh4; EH3_D(7) = (u64)(u32)stop; EH3_D(5) = PH3_LUNWIND_ONLY;
  if (!eh3_local_unwind(c, ci)) cont_finish(c, 0);
}
static void s__local_unwind2(Cpu *c) { local_unwind_start(c, ARG(0), (s32)ARG(1), 0, 0); }
static void s__local_unwind4(Cpu *c) { local_unwind_start(c, ARG(1), (s32)ARG(2), 1, rd32(c, ARG(0))); }
static void s__global_unwind2(Cpu *c) { start_unwind32(c, ARG(0), 0, 0, 0, 0); }

/* ---- __CxxFrameHandler (x86): frame = { prev@0 handler@4 trylevel@8 } with ebp = frame+12, saved esp at [ebp-0x10] = frame-4.
 *      FuncInfo: magic@0 maxState@4 unwindMap@8 nTry@12 tryMap@16 (pointers, not RVAs); UnwindMap {toState@0 action@4}; TryBlock {lo@0 hi@4 catchHi@8 n@12 handlers@16};
 *      HandlerType {adjectives@0 type@4 dispCatchObj@8 handler@12}; ThrowInfo {attrs@0 dtor@4 fwd@8 cta@12}; CatchableType {props@0 type@4 mdisp@8 pdisp@12 vdisp@16 size@20 copy@24}; type name @8 ---- */
/* cont data: 0 rec 1 frame 2 ctx 3 descr 4 phase 5 tryblock 6 catchblock 7 stop level 8 object 9 throwinfo 10 catchable type 11 continuation */
static u64 cxx32_this_ptr(Cpu *c, u64 ct, u64 obj) {
  if (!obj) return 0;
  s32 mdisp = (s32)rd32(c, ct + 8), pdisp = (s32)rd32(c, ct + 12), vdisp = (s32)rd32(c, ct + 16);
  if (pdisp >= 0) { obj = (u32)(obj + pdisp); obj = (u32)(obj + (s32)rd32(c, (u32)(rd32(c, obj) + vdisp))); }
  return (u32)(obj + mdisp);
}
static u64 cxx32_find_caught_type(Cpu *c, u64 ti_throw, u64 catch_ti, u32 catch_flags) {
  u64 tab = rd32(c, ti_throw + 12); if (!tab) return 0; u32 n = rd32(c, tab), ef = rd32(c, ti_throw);
  for (u32 i = 0; i < n && i < 64; i++) {
    u64 ct = rd32(c, tab + 4 + 4ull * i), ti = rd32(c, ct + 4);
    if (!catch_ti) return ct;
    if (catch_ti != ti && !cxx_str_eq(c, catch_ti + 8, ti + 8)) continue;
    if ((ef & TF_CONST) && !(catch_flags & TF_CONST)) continue;
    if ((ef & TF_VOLATILE) && !(catch_flags & TF_VOLATILE)) continue;
    return ct;
  }
  return 0;
}
static int cxx32_copy_exception(Cpu *c, u64 obj, u64 dest, u32 catch_flags, u64 ct) {
  u32 tf = rd32(c, ct); u32 size = rd32(c, ct + 20); u32 copy = rd32(c, ct + 24);
  if (catch_flags & TF_REFERENCE) { wr32(c, dest, (u32)cxx32_this_ptr(c, ct, obj)); return 0; }
  if (tf & CT_SIMPLE) {
    u8 *s = mem_ptr(c->mem, obj, size, 0), *d = mem_ptr(c->mem, dest, size, 1); if (s && d) e_memmove(d, s, size);
    if (size == 4) wr32(c, dest, (u32)cxx32_this_ptr(c, ct, rd32(c, dest)));
    return 0;
  }
  if (copy) { c->r[RCX] = dest; guest_call(c, copy, cxx32_this_ptr(c, ct, obj), (tf & CT_VBASE) ? 1 : 0, 0, 0); return 1; }   /* __thiscall copy constructor */
  { u64 src = cxx32_this_ptr(c, ct, obj); u8 *s = mem_ptr(c->mem, src, size, 0), *d = mem_ptr(c->mem, dest, size, 1); if (s && d) e_memmove(d, s, size); }
  return 0;
}
static int cxx32_local_unwind(Cpu *c, int ci) {   /* cleanup actions from frame->trylevel down to data[7]; 1 while one runs */
  Win32 *w = W; u64 frame = EH3_D(1), descr = EH3_D(3); s32 stop = (s32)EH3_D(7); u32 n = rd32(c, descr + 4); u64 map = rd32(c, descr + 8);
  for (int guard = 0; guard < 4096; guard++) {
    s32 tl = (s32)rd32(c, frame + 8); if (tl == stop || tl == -1) return 0;
    if (tl < 0 || (u32)tl >= n) { cpu_fault(c, "__CxxFrameHandler: bad state %d (max %u)", tl, n); return 1; }
    u64 e = map + 8ull * (u32)tl; wr32(c, frame + 8, rd32(c, e)); u64 action = rd32(c, e + 4);
    if (action) { seh_log("[x64]   cleanup 0x%llx (state %d)\n", (unsigned long long)action, tl); c->r[RBP] = frame + 12; guest_call(c, action, 0, 0, 0, 0); return 1; }
  }
  cpu_fault(c, "__CxxFrameHandler: runaway local unwind"); return 1;
}
static void cxx32_continue(Cpu *c, int ci) {
  Win32 *w = W; u64 frame = EH3_D(1), addr = EH3_D(11);
  w->ncont = ci; c->r[RBP] = frame + 12; c->r[RSP] = rd32(c, frame - 4); c->rip = addr; cont_gc(c);
  seh_log("[x64] catch done -> 0x%llx ebp 0x%llx esp 0x%llx\n", (unsigned long long)addr, (unsigned long long)c->r[RBP], (unsigned long long)c->r[RSP]);
}
static void cxx32_call_catch(Cpu *c, int ci) {
  Win32 *w = W; u64 frame = EH3_D(1), tb = EH3_D(5), cb = EH3_D(6);
  wr32(c, frame + 8, rd32(c, tb + 4) + 1);
  w->cxx32_obj = EH3_D(8); w->cxx32_ti = EH3_D(9);
  EH3_D(4) = PX_CATCHRUN; u64 h = rd32(c, cb + 12);
  seh_log("[x64]   catch block 0x%llx\n", (unsigned long long)h);
  c->r[RBP] = frame + 12; guest_call(c, h, 0, 0, 0, 0);
}
static void cxx32_find_catch(Cpu *c, int ci, s32 tl) {
  Win32 *w = W; u64 frame = EH3_D(1), descr = EH3_D(3); u32 n = rd32(c, descr + 12); u64 map = rd32(c, descr + 16);
  for (u32 j = 0; j < n && j < 4096; j++) {
    u64 tb = map + 20ull * j; s32 lo = (s32)rd32(c, tb), hi = (s32)rd32(c, tb + 4); if (tl < lo || tl > hi) continue;
    u32 nc = rd32(c, tb + 12); u64 hs = rd32(c, tb + 16);
    for (u32 k = 0; k < nc && k < 256; k++) {
      u64 cb = hs + 16ull * k; u32 cflags = rd32(c, cb), cti = rd32(c, cb + 4); u64 ct = 0;
      if (EH3_D(9)) { ct = cxx32_find_caught_type(c, EH3_D(9), cti, cflags); if (!ct) continue; }
      else if (cti) continue;   /* a non-C++ exception: only catch(...) */
      EH3_D(5) = tb; EH3_D(6) = cb; EH3_D(10) = ct; EH3_D(4) = PX_COPY;
      seh_log("[x64]   catch match: try %u clause %u type 0x%x\n", j, k, cti);
      s32 off = (s32)rd32(c, cb + 8);
      if (ct && cti && off && rd8(c, rd32(c, cb + 4) + 8)) { if (cxx32_copy_exception(c, EH3_D(8), (u32)(frame + 12 + off), cflags, ct)) return; }
      EH3_D(4) = PX_GUNWIND; start_unwind32(c, frame, 0, EH3_D(0), 0, 1); return;
    }
  }
  cont_finish(c, 1);
}
static void cxx32_frame_handler(Cpu *c) {
  Win32 *w = W; u64 rec = ARG(0), frame = ARG(1), ctx = ARG(2), descr = (u32)g_entry_eax;
  u32 magic = rd32(c, descr);
  if (magic < CXX_MAGIC_VC6 || magic > CXX_MAGIC_VC8) { seh_log("[x64] __CxxFrameHandler: unknown FuncInfo magic 0x%x (eax)\n", magic); RET(1); return; }
  int ci = cont_push(c, CONT_CXX32); if (ci < 0) return;
  EH3_D(0) = rec; EH3_D(1) = frame; EH3_D(2) = ctx; EH3_D(3) = descr;
  u32 fl = rd32(c, rec + 4);
  if (fl & (EXC_UNWINDING | EXC_EXIT_UNWIND)) { EH3_D(7) = (u64)(u32)-1; EH3_D(4) = PX_LUNWIND_ONLY; if (!cxx32_local_unwind(c, ci)) cont_finish(c, 1); return; }
  if (!rd32(c, descr + 12)) { cont_finish(c, 1); return; }
  u32 code = rd32(c, rec); u64 obj = 0, ti = 0;
  if (code == CXX_EXCEPTION) {
    u32 np = rd32(c, rec + 16), m = rd32(c, rec + 20);
    if (np < 3 || m < CXX_MAGIC_VC6 || m > CXX_MAGIC_VC8) { cont_finish(c, 1); return; }
    obj = rd32(c, rec + 24); ti = rd32(c, rec + 28);
    if (!ti) { if (!w->cxx32_ti) { cont_finish(c, 1); return; } obj = w->cxx32_obj; ti = w->cxx32_ti; }   /* rethrow */
  } else if (magic >= CXX_MAGIC_VC8 && (rd32(c, descr + 32) & FD_SYNCHRONOUS)) { cont_finish(c, 1); return; }
  EH3_D(8) = obj; EH3_D(9) = ti;
  cxx32_find_catch(c, ci, (s32)rd32(c, frame + 8));
}
static void s___CxxFrameHandler32(Cpu *c) { cxx32_frame_handler(c); }

/* ---- msvcrt longjmp (x86 _JUMP_BUFFER: Ebp@0 Ebx@4 Edi@8 Esi@12 Esp@16 Eip@20 Registration@24 TryLevel@28 Cookie@32): global unwind to the setjmp frame first ---- */
enum { CONT_LONGJMP32 = 324 };
static void longjmp32_jump(Cpu *c, u64 a, u64 v) {
  c->r[RBP] = rd32(c, a); c->r[RBX] = rd32(c, a + 4); c->r[RDI] = rd32(c, a + 8); c->r[RSI] = rd32(c, a + 12); c->r[RSP] = rd32(c, a + 16); c->rip = rd32(c, a + 20);
  c->r[RAX] = v ? (u32)v : 1; cont_gc(c);
  seh_log("[x64] longjmp -> 0x%llx esp 0x%llx\n", (unsigned long long)c->rip, (unsigned long long)c->r[RSP]);
}
static void longjmp32(Cpu *c, u64 a, u64 v) {
  Win32 *w = W; u64 reg = rd32(c, a + 24);
  if (reg && reg != CHAIN_END && fs0(c) != reg && frame_ok32(c, reg)) {
    int ci = cont_push(c, CONT_LONGJMP32); if (ci < 0) return; w->cont[ci].data[0] = a; w->cont[ci].data[1] = v;
    start_unwind32(c, reg, 0, 0, 0, 1); return;
  }
  longjmp32_jump(c, a, v);
}
/* a handler-started RtlUnwind finished: continue the handler that is now on top */
static void seh32_after_unwind(Cpu *c) {
  Win32 *w = W; int ci = w->ncont - 1; if (ci < 0) { cpu_fault(c, "x86 unwind: no handler to resume"); return; }
  if (w->cont[ci].kind == CONT_LONGJMP32) { u64 a = w->cont[ci].data[0], v = w->cont[ci].data[1]; w->ncont = ci; longjmp32_jump(c, a, v); return; }
  if (w->cont[ci].kind == CONT_EH3 && EH3_D(5) == PH3_GUNWIND) { EH3_D(7) = EH3_D(4); EH3_D(5) = PH3_LUNWIND; if (!eh3_local_unwind(c, ci)) eh3_jump(c, ci); return; }
  if (w->cont[ci].kind == CONT_CXX32 && EH3_D(4) == PX_GUNWIND) { EH3_D(7) = (u64)(u32)rd32(c, EH3_D(5)); EH3_D(4) = PX_LUNWIND; if (!cxx32_local_unwind(c, ci)) cxx32_call_catch(c, ci); return; }
  cpu_fault(c, "x86 unwind: unexpected continuation %d on top", w->cont[ci].kind);
}
static int seh32_cont_resume(Cpu *c, int kind, u64 rax) {
  Win32 *w = W; int ci = w->ncont - 1;
  switch (kind) {
    case CONT_DISPATCH32: {
      if (w->cont[ci].data[5] == 1) {   /* a vectored handler returned */
        if ((s32)rax == -1) { u64 ctx = w->cont[ci].data[1]; w->ncont = ci; ctx32_load(c, ctx); cont_gc(c); return 1; }
        if (w->cont[ci].idx < w->nveh) { u64 h = w->veh[w->cont[ci].idx++]; guest_call(c, h, w->cont[ci].data[4], 0, 0, 0); return 1; }
        w->cont[ci].data[5] = 0; dispatch32_step(c, ci); return 1;
      }
      if (w->cont[ci].data[5] == 2) {   /* the top-level filter returned */
        if ((s32)rax == -1) { u64 ctx = w->cont[ci].data[1]; w->ncont = ci; ctx32_load(c, ctx); cont_gc(c); return 1; }
        dispatch32_step(c, ci); return 1;
      }
      u32 disp = (u32)rax;
      if (disp == 0) { u64 ctx = w->cont[ci].data[1]; w->ncont = ci; ctx32_load(c, ctx); cont_gc(c); seh_log("[x64] continue execution at 0x%llx\n", (unsigned long long)c->rip); return 1; }
      if (disp == 1 || disp == 2 || disp == 3) { w->cont[ci].data[2] = rd32(c, w->cont[ci].data[2]); dispatch32_step(c, ci); return 1; }
      cpu_fault(c, "x86 exception handler returned disposition %u", disp); return 1; }
    case CONT_UNWIND32: { u64 node = w->cont[ci].data[6]; set_fs0(c, rd32(c, node)); unwind32_step(c, ci); return 1; }
    case CONT_EH3: {
      switch ((int)EH3_D(5)) {
        case PH3_FILTER: { s32 r = (s32)(u32)rax; if (r < 0) { cont_finish(c, 0); return 1; }
          if (r == 0) { EH3_D(4) = (u64)(u32)rd32(c, eh3_entry(w, ci, (s32)EH3_D(4))); eh3_search(c, ci); return 1; }
          EH3_D(5) = PH3_GUNWIND; start_unwind32(c, EH3_D(1), 0, EH3_D(0), 0, 1); return 1; }
        case PH3_LUNWIND: if (!eh3_local_unwind(c, ci)) eh3_jump(c, ci); return 1;
        case PH3_LUNWIND_ONLY: if (!eh3_local_unwind(c, ci)) cont_finish(c, EH3_D(0) ? 1 : 0); return 1;
      }
      cpu_fault(c, "_except_handler3: bad phase"); return 1; }
    case CONT_CXX32: {
      switch ((int)EH3_D(4)) {
        case PX_COPY: EH3_D(4) = PX_GUNWIND; start_unwind32(c, EH3_D(1), 0, EH3_D(0), 0, 1); return 1;
        case PX_LUNWIND: if (!cxx32_local_unwind(c, ci)) cxx32_call_catch(c, ci); return 1;
        case PX_LUNWIND_ONLY: if (!cxx32_local_unwind(c, ci)) cont_finish(c, 1); return 1;
        case PX_CATCHRUN: { EH3_D(11) = (u32)rax; u64 ti = EH3_D(9), obj = EH3_D(8); u64 dtor = ti ? rd32(c, ti + 4) : 0;
          if (w->cxx32_obj == obj) { w->cxx32_obj = 0; w->cxx32_ti = 0; }
          if (dtor && obj) { EH3_D(4) = PX_DTOR; c->r[RCX] = obj; guest_call(c, dtor, 0, 0, 0, 0); return 1; }
          cxx32_continue(c, ci); return 1; }
        case PX_DTOR: cxx32_continue(c, ci); return 1;
      }
      cpu_fault(c, "__CxxFrameHandler: bad phase"); return 1; }
  }
  return 0;
}
#undef EH3_D
#define SEH32_SHIMS \
  SA("_except_handler3", _except_handler3), SA("_except_handler4_common", _except_handler4_common), SA("_local_unwind2", _local_unwind2), SA("_local_unwind4", _local_unwind4), SA("_global_unwind2", _global_unwind2), \
  SA("??1type_info@@UAE@XZ", ret0),
