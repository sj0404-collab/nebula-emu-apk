/* MSVC C++ exception handling: __CxxFrameHandler / __CxxFrameHandler3 (msvcrt / vcruntime140, FuncInfo magic 0x19930520..22).
 * x64 tables are image-relative (RVA). Layouts:
 *   FuncInfo   : magic@0 nUnwind@4 unwindMap@8 nTry@12 tryMap@16 nIp@20 ipMap@24 unwindHelp@28 ESTypeList@32 EHFlags@36
 *   UnwindMap  : { s32 toState; u32 action; }                     (8)
 *   TryBlock   : { s32 tryLow, tryHigh, catchHigh; s32 n; u32 handlers; } (20)
 *   HandlerType: { u32 adjectives; u32 type; s32 catchObj; u32 handler; u32 frame; } (20)
 *   IpToState  : { u32 ip; s32 state; }                            (8)
 *   ThrowInfo  : { u32 attrs; u32 dtor; u32 fwdCompat; u32 catchableTypes; } (16)
 *   CatchableTypeArray { s32 n; u32 types[]; }   CatchableType { u32 props; u32 type; PMD {s32 mdisp,pdisp,vdisp}; s32 size; u32 copyCtor; } (28)
 *   TypeDescriptor { vtable@0; spare@8; char name[]@16 }
 * The throw record is RaiseException(0xE06D7363, NONCONTINUABLE, 4, { 0x19930520, object, ThrowInfo, imageBase }).
 * Logic follows Wine's dlls/msvcrt/except.c (cxx_frame_handler / find_catch_block / call_catch_block). */
enum { CONT_CXX = 310, CONT_CXXCATCH };
#define CXX_EXCEPTION 0xE06D7363u
enum { PH_UNWIND = 0, PH_SEARCH, PH_COPY, PH_CATCHRUN, PH_DTOR };
/* cont data: 0 rec 1 frame 2 ctx 3 dc 4 descr 5 from-ctx 6 orig_frame 7 trylevel(unwind loop) 8 last_level 9 scratch 10 exc_type
 *            11 phase 12 in_catch+1 13 base 14 pending handler 15 pending tryblock start level; idx = try block iterator */
#define CXX_MAGIC_VC6 0x19930520u
#define CXX_MAGIC_VC8 0x19930522u
#define FD_SYNCHRONOUS 1u
#define TF_CONST 1u
#define TF_VOLATILE 2u
#define TF_REFERENCE 8u
#define CT_SIMPLE 1u
#define CT_VBASE 4u

static s32 cxx_ip_to_state(Cpu *c, u64 descr, u64 ip, u64 base) {
  u32 n = rd32(c, descr + 20); u64 map = base + rd32(c, descr + 24); u32 i;
  for (i = 0; i < n; i++) if (base + rd32(c, map + 8ull * i) > ip) break;
  return i ? (s32)rd32(c, map + 8ull * (i - 1) + 4) : -1;
}
static u64 cxx_this_ptr(Cpu *c, u64 ct, u64 obj) {   /* adjust the object pointer to the (virtual) base the catch wants */
  if (!obj) return 0;
  s32 mdisp = (s32)rd32(c, ct + 8), pdisp = (s32)rd32(c, ct + 12), vdisp = (s32)rd32(c, ct + 16);
  if (pdisp >= 0) { obj += (s64)pdisp; obj += (s64)(s32)rd32(c, rd64(c, obj) + (s64)vdisp); }
  return obj + (s64)mdisp;
}
static int cxx_str_eq(Cpu *c, u64 a, u64 b) { for (int i = 0; i < 4096; i++) { u8 x = rd8(c, a++), y = rd8(c, b++); if (x != y) return 0; if (!x) return 1; } return 0; }
/* the CatchableType matching a catch clause (0 = none); catch_ti == 0 is catch(...) */
static u64 cxx_find_caught_type(Cpu *c, u64 exc_type, u64 base, u64 catch_ti, u32 catch_flags) {
  u64 tab = base + rd32(c, exc_type + 12); u32 n = rd32(c, tab), ef = rd32(c, exc_type);
  for (u32 i = 0; i < n && i < 64; i++) {
    u64 ct = base + rd32(c, tab + 4 + 4ull * i), ti = base + rd32(c, ct + 4);
    if (!catch_ti) return ct;
    if (catch_ti != ti && !cxx_str_eq(c, catch_ti + 16, ti + 16)) continue;
    if ((ef & TF_CONST) && !(catch_flags & TF_CONST)) continue;
    if ((ef & TF_VOLATILE) && !(catch_flags & TF_VOLATILE)) continue;
    return ct;
  }
  return 0;
}
/* copy the exception object into the catch variable; returns 1 if a guest copy constructor was called (resume later) */
static int cxx_copy_exception(Cpu *c, u64 obj, u64 dest, u32 catch_flags, u64 ct, u64 base) {
  u32 tf = rd32(c, ct); u32 size = rd32(c, ct + 20); u32 copy = rd32(c, ct + 24);
  if (catch_flags & TF_REFERENCE) { wr64(c, dest, cxx_this_ptr(c, ct, obj)); return 0; }
  if (tf & CT_SIMPLE) {
    u8 *s = mem_ptr(c->mem, obj, size, 0), *d = mem_ptr(c->mem, dest, size, 1); if (s && d) e_memmove(d, s, size);
    if (size == 8) wr64(c, dest, cxx_this_ptr(c, ct, rd64(c, dest)));
    return 0;
  }
  if (copy) { guest_call(c, base + copy, dest, cxx_this_ptr(c, ct, obj), (tf & CT_VBASE) ? 1 : 0, 0); return 1; }
  { u64 src = cxx_this_ptr(c, ct, obj); u8 *s = mem_ptr(c->mem, src, size, 0), *d = mem_ptr(c->mem, dest, size, 1); if (s && d) e_memmove(d, s, size); }
  return 0;
}
/* run the cleanup funclets from data[7] down to data[8] on frame data[6]; returns 1 while a funclet is executing */
static int cxx_local_unwind_step(Cpu *c, int ci) {
  Win32 *w = W; u64 *d = w->cont[ci].data; u64 descr = d[4], frame = d[6], base = d[13];
  s32 tl = (s32)(s64)d[7], last = (s32)(s64)d[8]; u32 nun = rd32(c, descr + 4); u64 ut = base + rd32(c, descr + 8); s32 uh = (s32)rd32(c, descr + 28);
  while (tl > last) {
    if (tl < 0 || (u32)tl >= nun) { cpu_fault(c, "C++ exception handling: invalid unwind state %d", tl); return 1; }
    s32 prev = (s32)rd32(c, ut + 8ull * (u32)tl); u32 h = rd32(c, ut + 8ull * (u32)tl + 4);
    tl = prev; d[7] = (u64)(s64)tl; wr32(c, frame + (s64)uh, (u32)tl);
    if (h) { seh_log("[x64] C++ cleanup -> state %d funclet 0x%llx\n", tl, (unsigned long long)(base + h)); guest_call(c, base + h, base + h, frame, 0x100, 0); return 1; }
  }
  wr32(c, frame + (s64)uh, (u32)tl);
  return 0;
}
static void cxx_step(Cpu *c, int ci);
static void cxx_do_catch(Cpu *c, int ci) {
  Win32 *w = W; u64 *d = w->cont[ci].data; u64 rec = d[0], frame = d[1], ctx = d[2], dc = d[3], descr = d[4], orig = d[6], cr = d[9];
  u8 *p = mem_ptr(c->mem, cr, EXREC_SIZE, 1); if (!p) { cpu_fault(c, "C++ catch: no stack"); return; }
  e_memset(p, 0, EXREC_SIZE);
  wr32(c, cr, STATUS_UNWIND_CONSOLIDATE); wr32(c, cr + 4, 1); wr32(c, cr + 24, 11);
  wr64(c, cr + 32 + 0 * 8, w32_shim_slot(w, "nebula", "__nebula_cxx_catch"));
  wr64(c, cr + 32 + 1 * 8, orig); wr64(c, cr + 32 + 2 * 8, descr); wr64(c, cr + 32 + 3 * 8, d[15]);
  wr64(c, cr + 32 + 5 * 8, d[14]); wr64(c, cr + 32 + 6 * 8, rec); wr64(c, cr + 32 + 7 * 8, ctx); wr64(c, cr + 32 + 8 * 8, d[13]);
  wr64(c, cr + 32 + 10 * 8, (u64)-1);
  seh_log("[x64] C++ catch in frame 0x%llx handler 0x%llx\n", (unsigned long long)orig, (unsigned long long)d[14]);
  w->ncont = ci;
  start_unwind(c, frame, rd64(c, dc), cr, 0, d[5]);
}
static void cxx_step(Cpu *c, int ci) {
  Win32 *w = W; u64 *d = w->cont[ci].data; u64 rec = d[0], descr = d[4], orig = d[6], base = d[13];
  switch ((int)d[11]) {
    case PH_UNWIND: if (cxx_local_unwind_step(c, ci)) return; cont_finish(c, 1); return;
    case PH_COPY: cxx_do_catch(c, ci); return;
    default: break;
  }
  /* PH_SEARCH: find_catch_block */
  u64 exc_type = d[10], obj = rd64(c, rec + 32 + 8), exc_base = rd32(c, rec + 24) >= 4 ? rd64(c, rec + 32 + 24) : 0;
  u32 ntry = rd32(c, descr + 12); u64 tt = base + rd32(c, descr + 16); s32 trylevel = (s32)(s64)d[7];
  s32 ic_end = 0, ic_catch = 0; int in_catch = (int)d[12] - 1;
  if (in_catch >= 0) { ic_end = (s32)rd32(c, tt + 20ull * in_catch + 4); ic_catch = (s32)rd32(c, tt + 20ull * in_catch + 8); }
  for (; (u32)w->cont[ci].idx < ntry; ) {
    u32 i = (u32)w->cont[ci].idx++; u64 tb = tt + 20ull * i;
    s32 lo = (s32)rd32(c, tb), hi = (s32)rd32(c, tb + 4); u32 ncatch = rd32(c, tb + 12); u64 cbs = base + rd32(c, tb + 16);
    if (trylevel < lo || trylevel > hi) continue;
    if (in_catch >= 0) { if (lo <= ic_end) continue; if (hi > ic_catch) continue; }
    for (u32 j = 0; j < ncatch; j++) {
      u64 cb = cbs + 20ull * j; u32 cflags = rd32(c, cb), ti = rd32(c, cb + 4); s32 off = (s32)rd32(c, cb + 8); u64 handler = base + rd32(c, cb + 12);
      if (exc_type) {
        u64 catch_ti = ti ? base + ti : 0, ct = cxx_find_caught_type(c, exc_type, exc_base, catch_ti, cflags);
        if (!ct) continue;
        d[14] = handler; d[15] = (u64)(s64)lo; d[11] = PH_COPY;
        if (catch_ti && rd8(c, catch_ti + 16) && off) { if (cxx_copy_exception(c, obj, orig + (s64)off, cflags, ct, exc_base)) return; }
      } else {
        if (ti) continue;   /* a non-C++ exception only reaches catch(...) */
        d[14] = handler; d[15] = (u64)(s64)lo; d[11] = PH_COPY;
      }
      cxx_do_catch(c, ci); return;
    }
  }
  cont_finish(c, 1);   /* no catch here: ExceptionContinueSearch */
}
/* __CxxFrameHandler(rec, frame, ctx, dc) — HandlerData is the RVA of FuncInfo */
static void cxx32_frame_handler(Cpu *c);
static void s___CxxFrameHandler(Cpu *c) {
  if (c->mode32) { cxx32_frame_handler(c); return; }
  Win32 *w = W; u64 rec = ARG(0), frame = ARG(1), ctx = ARG(2), dc = ARG(3);
  u64 base = rd64(c, dc + 8), control = rd64(c, dc), hd = rd64(c, dc + 56);
  u64 descr = base + rd32(c, hd); u32 magic = rd32(c, descr), code = rd32(c, rec), flags = rd32(c, rec + 4);
  if (magic < CXX_MAGIC_VC6 || magic > CXX_MAGIC_VC8) { seh_log("[x64] __CxxFrameHandler: unknown FuncInfo magic 0x%x\n", magic); RET(1); return; }
  int consolidate = code == STATUS_UNWIND_CONSOLIDATE && rd32(c, rec + 24) > 10 && rd64(c, rec + 32) == w32_shim_slot(w, "nebula", "__nebula_cxx_catch");
  if (magic >= CXX_MAGIC_VC8 && (rd32(c, descr + 36) & FD_SYNCHRONOUS) && code != CXX_EXCEPTION && !consolidate && code != 0x80000026u) { RET(1); return; }
  s32 trylevel = cxx_ip_to_state(c, descr, control, base);
  u32 ntry = rd32(c, descr + 12); u64 tt = base + rd32(c, descr + 16);
  u64 orig = frame; s32 unwindlevel = -1; int in_catch = -1;
  /* an exception raised inside a catch funclet: the establisher is the funclet's frame, the real frame is saved in it */
  for (u32 i = ntry; i > 0; i--) {
    u64 tb = tt + 20ull * (i - 1); s32 hi = (s32)rd32(c, tb + 4), ch = (s32)rd32(c, tb + 8);
    if (trylevel > hi && trylevel <= ch) {
      if (in_catch < 0) in_catch = (int)(i - 1);
      u64 fb = 0, fe = seh_lookup(c, control, &fb); u64 func = fe ? fb + rd32(c, fe) : 0;
      u32 n = rd32(c, tb + 12); u64 cbs = base + rd32(c, tb + 16);
      for (u32 j = 0; j < n; j++) { u64 cb = cbs + 20ull * j; if (base + rd32(c, cb + 12) == func) { unwindlevel = hi; orig = rd64(c, frame + (s64)(s32)rd32(c, cb + 16)); seh_log("[x64] C++ nested exception: funclet frame 0x%llx -> frame 0x%llx\n", (unsigned long long)frame, (unsigned long long)orig); } }
    }
  }
  int ci = cont_push(c, CONT_CXX); if (ci < 0) return;
  u64 *d = w->cont[ci].data;
  d[0] = rec; d[1] = frame; d[2] = ctx; d[3] = dc; d[4] = descr; d[6] = orig; d[13] = base;
  u64 from = 0; for (int k = ci - 1; k >= 0; k--) if (w->cont[k].kind == CONT_DISPATCH) { from = w->cont[k].data[1]; break; } else if (w->cont[k].kind == CONT_UNWIND) { from = w->cont[k].data[5]; break; }
  d[5] = from; d[9] = seh_scratch(c, EXREC_SIZE + 64); w->cont[ci].rsp = c->r[RSP];
  s32 uh = (s32)rd32(c, descr + 28);
  if (flags & (EXC_UNWINDING | EXC_EXIT_UNWIND)) {
    s32 cur = (s32)rd32(c, orig + (s64)uh); if (cur == -2) cur = trylevel;
    s32 last = (flags & EXC_TARGET_UNWIND) ? (consolidate ? (s32)(s64)rd64(c, rec + 32 + 3 * 8) : trylevel) : unwindlevel;
    d[7] = (u64)(s64)cur; d[8] = (u64)(s64)last; d[11] = PH_UNWIND;
    cxx_step(c, ci); return;
  }
  if (!ntry) { cont_finish(c, 1); return; }
  if (code == CXX_EXCEPTION && !rd64(c, rec + 32 + 8) && !rd64(c, rec + 32 + 16)) {   /* throw; — re-raise the exception being handled by the innermost catch */
    u64 prev = 0; for (int k = ci - 1; k >= 0; k--) if (w->cont[k].kind == CONT_CXXCATCH) { prev = w->cont[k].data[0]; break; }
    if (prev) { u8 *s = mem_ptr(c->mem, prev, EXREC_SIZE, 0), *t = mem_ptr(c->mem, rec, EXREC_SIZE, 1); if (s && t) e_memcpy(t, s, EXREC_SIZE); seh_log("[x64] C++ rethrow of record 0x%llx\n", (unsigned long long)prev); }
    else { cont_finish(c, 1); return; }
  }
  d[10] = code == CXX_EXCEPTION ? rd64(c, rec + 32 + 16) : 0;
  d[12] = (u64)(in_catch + 1); d[11] = PH_SEARCH; w->cont[ci].idx = 0;
  /* unwind_help[1] remembers the state across nested exceptions */
  s32 uh1 = (s32)rd32(c, orig + (s64)uh + 4);
  if (trylevel > uh1) { wr32(c, orig + (s64)uh, (u32)trylevel); wr32(c, orig + (s64)uh + 4, (u32)trylevel); } else trylevel = uh1;
  d[7] = (u64)(s64)trylevel;
  cxx_step(c, ci);
}
/* the consolidate callback (Wine's call_catch_block): run the catch funclet on the dispatcher stack, destroy the exception object, continue at its return value */
static void cxxcatch_step(Cpu *c, int ci, u64 rax) {
  Win32 *w = W; u64 *d = w->cont[ci].data; int ph = (int)d[11];
  if (ph == PH_CATCHRUN) {
    d[2] = rax;   /* continuation address returned by the funclet */
    u64 prev = d[0], obj = rd64(c, prev + 32 + 8), ti = rd64(c, prev + 32 + 16), eb = rd32(c, prev + 24) >= 4 ? rd64(c, prev + 32 + 24) : 0;
    int shared = 0; for (int k = ci - 1; k >= 0; k--) if (w->cont[k].kind == CONT_CXXCATCH && rd64(c, w->cont[k].data[0] + 32 + 8) == obj) shared = 1;
    u32 dtor = (obj && ti) ? rd32(c, ti + 4) : 0;
    d[11] = PH_DTOR;
    if (dtor && !shared) { guest_call(c, eb + dtor, obj, 0, 0, 0); return; }
  }
  u64 frame = d[6]; s32 uh = (s32)rd32(c, d[4] + 28);
  wr32(c, frame + (s64)uh, (u32)-2); wr32(c, frame + (s64)uh + 4, (u32)-1);
  seh_log("[x64] C++ catch done -> continue at 0x%llx\n", (unsigned long long)d[2]);
  cont_finish(c, d[2]);
}
static void s___nebula_cxx_catch(Cpu *c) {
  Win32 *w = W; u64 cr = ARG(0);
  int ci = cont_push(c, CONT_CXXCATCH); if (ci < 0) return;
  u64 *d = w->cont[ci].data;
  d[0] = rd64(c, cr + 32 + 6 * 8); d[1] = rd64(c, cr + 32 + 5 * 8); d[4] = rd64(c, cr + 32 + 2 * 8); d[6] = rd64(c, cr + 32 + 1 * 8); d[13] = rd64(c, cr + 32 + 8 * 8);
  d[11] = PH_CATCHRUN;
  guest_call(c, d[1], d[1], d[6], 0x100, 0);
}
static int cxx_cont_resume(Cpu *c, int kind, u64 rax) {
  Win32 *w = W; int ci = w->ncont - 1;
  if (kind == CONT_CXX) { cxx_step(c, ci); return 1; }
  if (kind == CONT_CXXCATCH) { cxxcatch_step(c, ci, rax); return 1; }
  return 0;
}
#define CXX_SHIMS \
  SA("__CxxFrameHandler", __CxxFrameHandler), SA("__CxxFrameHandler2", __CxxFrameHandler), SA("__CxxFrameHandler3", __CxxFrameHandler), SA("__nebula_cxx_catch", __nebula_cxx_catch),
