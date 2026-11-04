/* seh.inc — x64 structured exception handling: table-based unwinding (.pdata / UNWIND_INFO), RaiseException dispatch
 * (phase 1), RtlUnwindEx (phase 2, with STATUS_UNWIND_CONSOLIDATE callbacks as used by MSVC's __CxxFrameHandler),
 * RtlCaptureContext / RtlRestoreContext / RtlLookupFunctionEntry / RtlVirtualUnwind / RtlPcToFileHeader for guest unwinders
 * (libgcc, libunwind, vcruntime), and a native __C_specific_handler (__try/__except/__finally scope tables).
 * Handlers, filters and funclets are guest functions: every call goes through the continuation stack. Included by win32.c. */

enum { CONT_DISPATCH = 300, CONT_UNWIND, CONT_CONSOLIDATE, CONT_CSPEC };
#define CTX_SIZE 1232
#define EXREC_SIZE 152
#define DC_SIZE 80
#define STATUS_UNWIND 0xC0000027u
#define STATUS_UNWIND_CONSOLIDATE 0x80000029u
#define EXC_UNWINDING 2u
#define EXC_EXIT_UNWIND 4u
#define EXC_TARGET_UNWIND 0x20u
#define EXC_UNWIND_ANY 0x66u

typedef struct { u64 rip, rsp, r[16]; Xmm x[16]; } UCtx;
static void restore_context(Cpu *c, u64 ctx, u64 rec);
static void raise_exception32(Cpu *c, u32 code, u32 flags, u32 nargs, const u64 *args, u64 address);
static void start_unwind32(Cpu *c, u64 target, u64 target_ip, u64 rec, u64 retval, int internal);
static void seh_log(const char *fmt, ...) { char m[240]; __builtin_va_list ap; __builtin_va_start(ap, fmt); e_vsnprintf(m, sizeof m, fmt, ap); __builtin_va_end(ap); host_write(3, m, e_strlen(m)); }   /* the registers virtual unwinding cares about */

static void uctx_read(Cpu *c, u64 a, UCtx *x) {
  for (int i = 0; i < 16; i++) x->r[i] = rd64(c, a + 0x78 + 8 * i);
  x->rsp = x->r[RSP]; x->rip = rd64(c, a + 0xf8);
  for (int i = 0; i < 16; i++) { x->x[i].q[0] = rd64(c, a + 0x1a0 + 16 * i); x->x[i].q[1] = rd64(c, a + 0x1a8 + 16 * i); }
}
static void uctx_write(Cpu *c, u64 a, const UCtx *x) {
  for (int i = 0; i < 16; i++) wr64(c, a + 0x78 + 8 * i, i == RSP ? x->rsp : x->r[i]);
  wr64(c, a + 0xf8, x->rip);
  for (int i = 0; i < 16; i++) { wr64(c, a + 0x1a0 + 16 * i, x->x[i].q[0]); wr64(c, a + 0x1a8 + 16 * i, x->x[i].q[1]); }
}
/* CONTEXT of the code that called the current shim (return address already popped: rip/rsp are the caller's) */
static void ctx_capture(Cpu *c, u64 a) {
  u8 *p = mem_ptr(c->mem, a, CTX_SIZE, 1); if (!p) return; e_memset(p, 0, CTX_SIZE);
  wr32(c, a + 0x30, 0x10000f); wr32(c, a + 0x34, c->mxcsr); wr16(c, a + 0x38, 0x33); wr16(c, a + 0x42, 0x2b); wr32(c, a + 0x44, (u32)c->rflags);
  UCtx x; for (int i = 0; i < 16; i++) x.r[i] = c->r[i]; x.rsp = c->r[RSP]; x.rip = c->rip; for (int i = 0; i < 16; i++) x.x[i] = c->x[i];
  uctx_write(c, a, &x);
}
static void ctx_load(Cpu *c, u64 a) {
  UCtx x; uctx_read(c, a, &x);
  for (int i = 0; i < 16; i++) c->r[i] = x.r[i]; c->r[RSP] = x.rsp; c->rip = x.rip; for (int i = 0; i < 16; i++) c->x[i] = x.x[i];
  c->rflags = (c->rflags & ~0x8d5ull) | (rd32(c, a + 0x44) & 0x8d5u);
}

/* ---- function tables ---- */
static int seh_module(Win32 *w, u64 pc, u64 *base, u32 *pdata, u32 *psize) {
  if (pc - w->image_base < w->image_size) { *base = w->image_base; *pdata = w->exe_pdata_rva; *psize = w->exe_pdata_size; return 1; }
  for (int i = 0; i < w->nmods; i++) if (pc - w->mods[i].base < w->mods[i].size) { *base = w->mods[i].base; *pdata = w->mods[i].pdata_rva; *psize = w->mods[i].pdata_size; return 1; }
  return 0;
}
/* RUNTIME_FUNCTION containing pc (guest address of the 12-byte entry) or 0; *base = image base */
static u64 seh_lookup(Cpu *c, u64 pc, u64 *base) {
  Win32 *w = W; u32 pdata, psize; if (!seh_module(w, pc, base, &pdata, &psize) || !pdata) return 0;
  u32 rva = (u32)(pc - *base); u64 tab = *base + pdata; u32 lo = 0, hi = psize / 12;
  while (lo < hi) {
    u32 mid = (lo + hi) / 2; u64 e = tab + 12ull * mid; u32 b = rd32(c, e), en = rd32(c, e + 4);
    if (rva < b) hi = mid; else if (rva >= en) lo = mid + 1; else { while (rd32(c, e + 8) & 1) e = *base + (rd32(c, e + 8) & ~1u); return e; }   /* indirect entry → chained parent */
  }
  return 0;
}
/* epilogue detection: `add rsp, imm` / `lea rsp, [fp+disp]`, then pops of non-volatiles, then ret (or jmp = tail call) */
static int seh_in_epilog(Cpu *c, u64 pc, UCtx *x) {
  u64 p = pc; u8 b0 = rd8(c, p); int rex = 0;
  if ((b0 & 0xf0) == 0x40) { rex = b0; b0 = rd8(c, ++p); }
  u64 nrsp = x->rsp; int moved = 0;
  if ((rex & 0x48) == 0x48 && b0 == 0x83 && rd8(c, p + 1) == 0xc4) { nrsp += (s8)rd8(c, p + 2); p += 3; moved = 1; }
  else if ((rex & 0x48) == 0x48 && b0 == 0x81 && rd8(c, p + 1) == 0xc4) { nrsp += (s32)rd32(c, p + 2); p += 6; moved = 1; }
  else if ((rex & 0x48) == 0x48 && b0 == 0x8d) { u8 m = rd8(c, p + 1); int reg = (m & 7) | ((rex & 1) << 3); if (((m >> 3) & 7) != 4 || (m & 7) == 4) return 0;
    if ((m & 0xc0) == 0x40) { nrsp = x->r[reg] + (s8)rd8(c, p + 2); p += 3; } else if ((m & 0xc0) == 0x80) { nrsp = x->r[reg] + (s32)rd32(c, p + 2); p += 6; } else return 0; moved = 1; }
  else p = pc;
  /* pops */
  u64 q = p; int npop = 0; int regs[16];
  while (npop < 15) { u8 b = rd8(c, q); if (b == 0x41) { b = rd8(c, q + 1); if ((b & 0xf8) != 0x58) break; regs[npop++] = 8 | (b & 7); q += 2; continue; }
    if ((b & 0xf8) == 0x58) { regs[npop++] = b & 7; q += 1; continue; } break; }
  u8 t = rd8(c, q); if (t == 0xc2) t = 0xc3; if (t != 0xc3 && t != 0xe9 && !(t == 0xff && (rd8(c, q + 1) & 0x38) == 0x20) && !(t == 0x48 && rd8(c, q + 1) == 0xff)) return 0;
  if (!moved && !npop) return 0;   /* a bare ret: treat as body (unwind codes handle it) */
  /* emulate the rest of the epilogue */
  x->rsp = nrsp; for (int i = 0; i < npop; i++) { x->r[regs[i]] = rd64(c, x->rsp); x->rsp += 8; }
  x->rip = rd64(c, x->rsp); x->rsp += 8; return 1;
}
/* RtlVirtualUnwind: returns the language handler if (flags & type) and pc is in the body, updates *x to the caller's frame */
/* cp: guest KNONVOLATILE_CONTEXT_POINTERS (0 = none) — Xmm0..15 slots at +0, Rax..R15 at +128; the CLR's GC reads them to find
   where callee-saved registers were spilled so it can report/update object references in those stack slots */
static u64 seh_virtual_unwind_cp(Cpu *c, u32 type, u64 base, u64 pc, u64 fe, UCtx *x, u64 *handler_data, u64 *establisher, u64 cp);
static u64 seh_virtual_unwind(Cpu *c, u32 type, u64 base, u64 pc, u64 fe, UCtx *x, u64 *handler_data, u64 *establisher) { return seh_virtual_unwind_cp(c, type, base, pc, fe, x, handler_data, establisher, 0); }
static u64 seh_virtual_unwind_cp(Cpu *c, u32 type, u64 base, u64 pc, u64 fe, UCtx *x, u64 *handler_data, u64 *establisher, u64 cp) {
  *handler_data = 0; u64 est = x->rsp; int machframe = 0, in_prolog = 0, first = 1; u64 handler = 0;
  u32 begin = rd32(c, fe), uinfo = rd32(c, fe + 8);
  u32 pcoff = (u32)(pc - (base + begin));
  for (int chain = 0; chain < 8; chain++) {
    u64 ui = base + uinfo; u8 vf = rd8(c, ui), flags = vf >> 3, ver = vf & 7, prolog = rd8(c, ui + 1), count = rd8(c, ui + 2), fr = rd8(c, ui + 3) & 15, foff = rd8(c, ui + 3) >> 4;
    u64 codes = ui + 4, after = codes + 2 * (((u32)count + 1) & ~1u);
    if (first) {
      in_prolog = pcoff < prolog;
      if (!in_prolog && seh_in_epilog(c, pc, x)) { *establisher = est; return 0; }
      if (fr) { /* frame pointer established? (SET_FPREG executed) */
        int done = !in_prolog; if (!done) for (u32 i = 0; i < count; i++) { if ((rd8(c, codes + 2 * i + 1) & 15) == 3 && rd8(c, codes + 2 * i) <= pcoff) done = 1; }
        est = done ? x->r[fr] - 16ull * foff : x->rsp;
      }
    }
    for (u32 i = 0; i < count;) {
      u8 off = rd8(c, codes + 2 * i), op = rd8(c, codes + 2 * i + 1) & 15, info = rd8(c, codes + 2 * i + 1) >> 4;
      int skip = first && in_prolog && off > pcoff;
      switch (op) {
        case 0: if (!skip) { if (cp) wr64(c, cp + 128 + 8 * info, x->rsp); x->r[info] = rd64(c, x->rsp); x->rsp += 8; } i += 1; break;                      /* PUSH_NONVOL */
        case 1: { u64 sz; if (info == 0) { sz = 8ull * rd16(c, codes + 2 * (i + 1)); i += 2; } else { sz = rd32(c, codes + 2 * (i + 1)); i += 3; } if (!skip) x->rsp += sz; break; }
        case 2: if (!skip) x->rsp += 8ull * info + 8; i += 1; break;                                            /* ALLOC_SMALL */
        case 3: if (!skip) x->rsp = x->r[fr] - 16ull * foff; i += 1; break;                                      /* SET_FPREG */
        case 4: { u64 o = 8ull * rd16(c, codes + 2 * (i + 1)); if (!skip) { x->r[info] = rd64(c, est + o); if (cp) wr64(c, cp + 128 + 8 * info, est + o); } i += 2; break; }
        case 5: { u64 o = rd32(c, codes + 2 * (i + 1)); if (!skip) { x->r[info] = rd64(c, est + o); if (cp) wr64(c, cp + 128 + 8 * info, est + o); } i += 3; break; }
        case 6: i += ver == 1 ? 2 : 1; break;                                                                    /* v1 SAVE_XMM (deprecated) / v2 EPILOG */
        case 7: i += ver == 1 ? 3 : 2; break;
        case 8: { u64 o = 16ull * rd16(c, codes + 2 * (i + 1)); if (!skip) { x->x[info].q[0] = rd64(c, est + o); x->x[info].q[1] = rd64(c, est + o + 8); if (cp) wr64(c, cp + 8 * info, est + o); } i += 2; break; }
        case 9: { u64 o = rd32(c, codes + 2 * (i + 1)); if (!skip) { x->x[info].q[0] = rd64(c, est + o); x->x[info].q[1] = rd64(c, est + o + 8); if (cp) wr64(c, cp + 8 * info, est + o); } i += 3; break; }
        case 10: if (!skip) { if (info) x->rsp += 8; x->rip = rd64(c, x->rsp); x->rsp = rd64(c, x->rsp + 24); machframe = 1; } i += 1; break;
        default: i = count; break;
      }
    }
    if (flags & 4) { uinfo = rd32(c, after + 8); first = 0; continue; }   /* UNW_FLAG_CHAININFO: parent's codes follow */
    if (!in_prolog && (flags & type)) { handler = base + rd32(c, after); *handler_data = after + 4; }
    break;
  }
  if (!machframe) { x->rip = rd64(c, x->rsp); x->rsp += 8; }
  *establisher = est; return handler;
}
static int seh_is_cont_addr(u64 rip) { return rip == SLOT_ADDR(SLOT_CONT) || rip == SLOT_ADDR(SLOT_STARTUP) || rip == 0; }
/* the walk reached the return address into a shim continuation: continue in the guest frame that continuation belongs to.
 * Returns 0 at the top of the thread (program / thread start). */
static int seh_cross_cont(Cpu *c, int ci, UCtx *x) {
  Win32 *w = W;
  for (int k = ci - 1; k >= 0; k--) {
    if (w->cont[k].rsp + 8 < x->rsp) continue;
    switch (w->cont[k].kind) {
      case CONT_STARTUP: case CONT_TSTART: return 0;
      case CONT_DISPATCH: uctx_read(c, w->cont[k].data[1], x); return 1;   /* the raise point: like walking through KiUserExceptionDispatcher's machine frame */
      case CONT_UNWIND: uctx_read(c, w->cont[k].data[5], x); return 1;
      case CONT_CONSOLIDATE: uctx_read(c, w->cont[k].data[0], x); return 1;
      case CONT_CSPEC: if (w->cont[k].data[5]) { uctx_read(c, w->cont[k].data[5], x); return 1; } continue;
      case 310 /* CONT_CXX */: if (w->cont[k].data[5]) { uctx_read(c, w->cont[k].data[5], x); return 1; } continue;
      case 311 /* CONT_CXXCATCH */: continue;   /* the catch funclet's logical parent is the frame the consolidate continuation restores */
      default: x->rip = w->cont[k].ret_addr; x->rsp = w->cont[k].rsp; return 1;   /* qsort comparator, window procedure, DllMain, … → the shim's caller */
    }
  }
  return 0;
}
static void cont_gc(Cpu *c) { Win32 *w = W; while (w->ncont && w->cont[w->ncont - 1].rsp < c->r[RSP]) w->ncont--; }   /* drop continuations abandoned by a non-local exit */
static void dc_write(Cpu *c, u64 dc, u64 control_pc, u64 base, u64 fe, u64 est, u64 target_ip, u64 ctx, u64 handler, u64 hdata) {
  wr64(c, dc, control_pc); wr64(c, dc + 8, base); wr64(c, dc + 16, fe); wr64(c, dc + 24, est); wr64(c, dc + 32, target_ip);
  wr64(c, dc + 40, ctx); wr64(c, dc + 48, handler); wr64(c, dc + 56, hdata); wr64(c, dc + 64, 0); wr32(c, dc + 72, 0); wr32(c, dc + 76, 0);
}
static const char *exc_name(u32 code) {
  switch (code) { case 0xE06D7363: return "C++ exception (no matching catch)"; case 0xC0000005: return "access violation"; case 0xC0000094: return "integer divide by zero";
    case 0xC00000FD: return "stack overflow"; case 0x80000003: return "breakpoint"; case 0xC000001D: return "illegal instruction"; case 0x40010006: return "DBG_PRINTEXCEPTION"; case 0x406D1388: return "thread naming (MSVC)"; default: return "exception"; }
}

/* ---- phase 1: RaiseException → walk the frames, call every EHANDLER until one unwinds ---- */
static void dispatch_step(Cpu *c, int ci) {
  Win32 *w = W; u64 rec = w->cont[ci].data[0], octx = w->cont[ci].data[1], uctx = w->cont[ci].data[2], pctx = w->cont[ci].data[3], dc = w->cont[ci].data[4];
  UCtx x; uctx_read(c, uctx, &x);
  for (int guard = 0; guard < 100000; guard++) {
    u64 stack_top = w->cur_th ? w->th[w->cur_th].stack + w->th[w->cur_th].stack_size : G_STACK_BASE + G_STACK_SIZE;
    if ((seh_is_cont_addr(x.rip) && !seh_cross_cont(c, ci, &x)) || x.rsp > stack_top) {
      u32 code = rd32(c, rec); u64 addr = rd64(c, rec + 16);
      if (w->uef && w->cont[ci].data[7] != 2) {   /* SetUnhandledExceptionFilter callback: EXCEPTION_CONTINUE_EXECUTION (-1) resumes, anything else terminates */
        w->cont[ci].data[7] = 2; u64 ptrs = w->cont[ci].data[6]; wr64(c, ptrs, rec); wr64(c, ptrs + 8, octx);
        seh_log("[x64] unhandled exception 0x%08x -> top-level filter 0x%llx\n", code, (unsigned long long)w->uef);
        guest_call(c, w->uef, ptrs, 0, 0, 0); return;
      }
      const char *hw = (w->hw_msg[0] && code == w->hw_code) ? w->hw_msg : "";
      w32_report(w, "unhandled exception 0x%08x (%s) raised at 0x%llx%s%s%s", code, exc_name(code), (unsigned long long)addr, hw[0] ? " (" : "", hw, hw[0] ? ")" : "");
      cpu_fault(c, "unhandled exception 0x%08x (%s) at 0x%llx — no frame caught it%s%s%s", code, exc_name(code), (unsigned long long)addr, hw[0] ? " (" : "", hw, hw[0] ? ")" : ""); c->err_rip = addr; return;
    }
    u64 base, fe = seh_lookup(c, x.rip, &base);
    if (!fe) { x.rip = rd64(c, x.rsp); x.rsp += 8; if (c->status != RUN_OK) return; continue; }   /* leaf function */
    UCtx prev = x; u64 hd, est; u64 handler = seh_virtual_unwind(c, 1, base, x.rip, fe, &x, &hd, &est);
    if (c->status != RUN_OK) return;
    seh_log("[x64]   frame pc 0x%llx fn 0x%llx est 0x%llx handler 0x%llx -> caller 0x%llx\n", (unsigned long long)prev.rip, (unsigned long long)(base + rd32(c, fe)), (unsigned long long)est, (unsigned long long)handler, (unsigned long long)x.rip);
    if (handler) {
      uctx_write(c, uctx, &x); uctx_write(c, pctx, &prev);
      dc_write(c, dc, prev.rip, base, fe, est, 0, octx, handler, hd);
      w->cont[ci].data[5] = est;
      guest_call(c, handler, rec, est, octx, dc); return;
    }
  }
  cpu_fault(c, "exception dispatch: runaway stack walk");
}
/* 1232-byte contexts + record + dispatcher context carved from the guest stack below rsp */
static u64 seh_scratch(Cpu *c, u64 bytes) { u64 a = (c->r[RSP] - 256 - bytes) & ~63ull; c->r[RSP] = a - 64; return a; }
static void raise_exception(Cpu *c, u32 code, u32 flags, u32 nargs, const u64 *args, u64 address) {
  Win32 *w = W;
  if (nargs > 15) nargs = 15;
  /* the exception context is the caller's (rip = return address of RaiseException) */
  u64 octx_rip = c->rip, octx_rsp = c->r[RSP];
  u64 sc = seh_scratch(c, EXREC_SIZE + 3 * CTX_SIZE + DC_SIZE + 64);
  u64 rec = sc, octx = (sc + EXREC_SIZE + 63) & ~63ull, uctx = octx + CTX_SIZE, pctx = uctx + CTX_SIZE, dc = pctx + CTX_SIZE, ptrs = dc + DC_SIZE;
  u8 *rp = mem_ptr(c->mem, rec, EXREC_SIZE, 1); if (!rp) { cpu_fault(c, "RaiseException: no stack"); return; }
  e_memset(rp, 0, EXREC_SIZE); wr32(c, rec, code); wr32(c, rec + 4, flags & 1); wr64(c, rec + 16, address); wr32(c, rec + 24, nargs);
  for (u32 i = 0; i < nargs; i++) wr64(c, rec + 32 + 8 * i, args[i]);
  { u64 srip = c->rip, srsp = c->r[RSP]; c->rip = octx_rip; c->r[RSP] = octx_rsp; ctx_capture(c, octx); c->rip = srip; c->r[RSP] = srsp; }
  u8 *s = mem_ptr(c->mem, octx, CTX_SIZE, 0); u8 *d = mem_ptr(c->mem, uctx, CTX_SIZE, 1); if (s && d) e_memcpy(d, s, CTX_SIZE);
  int ci = cont_push(c, CONT_DISPATCH); if (ci < 0) return;
  w->cont[ci].data[0] = rec; w->cont[ci].data[1] = octx; w->cont[ci].data[2] = uctx; w->cont[ci].data[3] = pctx; w->cont[ci].data[4] = dc; w->cont[ci].data[6] = ptrs; w->cont[ci].data[7] = 0;
  seh_log("[x64] RaiseException 0x%08x (%s) at 0x%llx\n", code, exc_name(code), (unsigned long long)address);
  if (w->nveh) { w->cont[ci].data[7] = 1; w->cont[ci].idx = 1; wr64(c, ptrs, rec); wr64(c, ptrs + 8, octx); guest_call(c, w->veh[0], ptrs, 0, 0, 0); return; }   /* vectored handlers run first */
  dispatch_step(c, ci);
}
/* a hardware fault of the interpreter (AV, divide by zero, ud2) becomes an SEH exception at the faulting instruction */
int w32_hw_exception(Cpu *c) {
  Win32 *w = W; u32 code = c->exc_code; if (!code || !w) return 0;
  int nested = 0; for (int k = 0; k < w->ncont; k++) if (w->cont[k].kind == CONT_DISPATCH) nested++;
  if (nested >= 16) { c->exc_code = 0; return 0; }   /* faults inside the handlers of faults inside … : give up with the plain report */
  e_snprintf(w->hw_msg, sizeof w->hw_msg, "%s", c->err); w->hw_code = code;
  c->exc_code = 0; c->status = RUN_OK; c->err[0] = 0;
  u64 args[2] = { c->exc_info, c->exc_addr };
  if (c->mode32) { raise_exception32(c, code, 0, code == 0xC0000005u ? 2 : 0, args, c->rip); return c->status == RUN_OK; }
  seh_log("[x64] hw exception 0x%08x tid=%u rip=0x%llx addr=0x%llx rsp=0x%llx [rsp]=0x%llx rcx=0x%llx rdx=0x%llx rax=0x%llx r10=0x%llx\n", code, (unsigned)cur_tid(w), (unsigned long long)c->rip, (unsigned long long)c->exc_addr, (unsigned long long)c->r[RSP], (unsigned long long)rd64(c, c->r[RSP]), (unsigned long long)c->r[RCX], (unsigned long long)c->r[RDX], (unsigned long long)c->r[RAX], (unsigned long long)c->r[R10]);
  raise_exception(c, code, 0, code == 0xC0000005u ? 2 : 0, args, c->rip);
  return c->status == RUN_OK;
}
static void s_RaiseException(Cpu *c) {
  u32 nargs = (u32)ARG(2); u64 args[15]; u64 pa = ARG(3); for (u32 i = 0; i < nargs && i < 15; i++) args[i] = pa ? (c->mode32 ? rd32(c, pa + 4 * i) : rd64(c, pa + 8 * i)) : 0;
  if (c->mode32) { raise_exception32(c, (u32)ARG(0), (u32)ARG(1), nargs, args, c->rip); return; }
  raise_exception(c, (u32)ARG(0), (u32)ARG(1), nargs, args, c->rip);
}
/* vcruntime!_CxxThrowException(pExceptionObject, pThrowInfo): the MSVC C++ throw = RaiseException(0xE06D7363, 1, 4, {magic, obj, throwinfo, imagebase}) */
static void s__CxxThrowException(Cpu *c) {
  if (c->mode32) { u64 obj = ARG(0), ti = ARG(1); if (!obj && !ti) { obj = W->cxx32_obj; ti = W->cxx32_ti; } u64 a32[3] = { 0x19930520, obj, ti }; raise_exception32(c, 0xE06D7363u, 1, 3, a32, c->rip); return; }
  u64 base = W->image_base; u32 pd, ps; u64 b2; if (seh_module(W, c->rip, &b2, &pd, &ps)) base = b2;
  u64 args[4] = { 0x19930520, ARG(0), ARG(1), base }; raise_exception(c, 0xE06D7363u, 1, 4, args, c->rip);
}

/* ---- phase 2: RtlUnwindEx ---- */
static void unwind_step(Cpu *c, int ci) {
  Win32 *w = W; u64 target = w->cont[ci].data[0], target_ip = w->cont[ci].data[1], rec = w->cont[ci].data[2], retval = w->cont[ci].data[3], uctx = w->cont[ci].data[4], pctx = w->cont[ci].data[5], dc = w->cont[ci].data[6];
  UCtx x; uctx_read(c, uctx, &x);
  for (int guard = 0; guard < 100000; guard++) {
    if (seh_is_cont_addr(x.rip)) {
      if (seh_cross_cont(c, ci, &x)) continue;
      cpu_fault(c, "RtlUnwindEx: target frame 0x%llx not found on the stack", (unsigned long long)target); return;
    }
    if (target && x.rsp > target) { cpu_fault(c, "RtlUnwindEx: passed the target frame 0x%llx (rsp 0x%llx)", (unsigned long long)target, (unsigned long long)x.rsp); return; }
    u64 base, fe = seh_lookup(c, x.rip, &base);
    if (!fe) { x.rip = rd64(c, x.rsp); x.rsp += 8; if (c->status != RUN_OK) return; continue; }
    UCtx prev = x; u64 hd, est; u64 handler = seh_virtual_unwind(c, 2, base, x.rip, fe, &x, &hd, &est);
    if (c->status != RUN_OK) return;
    int at_target = target && est == target;
    u32 fl = rd32(c, rec + 4); if (at_target) fl |= EXC_TARGET_UNWIND; else fl &= ~EXC_TARGET_UNWIND; wr32(c, rec + 4, fl);
    uctx_write(c, uctx, &x); prev.r[RAX] = retval; uctx_write(c, pctx, &prev);
    w->cont[ci].idx = at_target;
    if (handler) { dc_write(c, dc, prev.rip, base, fe, est, target_ip, pctx, handler, hd); w->cont[ci].data[7] = est; seh_log("[x64]   unwind frame pc 0x%llx est 0x%llx handler 0x%llx%s\n", (unsigned long long)prev.rip, (unsigned long long)est, (unsigned long long)handler, at_target ? " (target)" : ""); guest_call(c, handler, rec, est, pctx, dc); return; }
    if (at_target) break;
  }
  /* reached the target: resume there with the (possibly handler-modified) context of that frame */
  wr64(c, pctx + 0xf8, target_ip); wr64(c, pctx + 0x78, retval);
  w->ncont = ci;   /* this continuation is finished */
  restore_context(c, pctx, rec);
}
static void start_unwind(Cpu *c, u64 target, u64 target_ip, u64 rec, u64 retval, u64 from_ctx) {
  Win32 *w = W;
  u64 caller_rsp = c->r[RSP];   /* the walk starts in the caller's frame: capture its RSP BEFORE the scratch area lowers it (a GS cookie check in the first unwound frame reads garbage otherwise) */
  u64 sc = seh_scratch(c, (rec ? 0 : EXREC_SIZE) + 2 * CTX_SIZE + DC_SIZE + 64);
  u64 uctx = (sc + 63) & ~63ull, pctx = uctx + CTX_SIZE, dc = pctx + CTX_SIZE, nrec = dc + DC_SIZE;
  if (!rec) { u8 *rp = mem_ptr(c->mem, nrec, EXREC_SIZE, 1); if (rp) e_memset(rp, 0, EXREC_SIZE); wr32(c, nrec, STATUS_UNWIND); wr64(c, nrec + 16, from_ctx ? rd64(c, from_ctx + 0xf8) : c->rip); rec = nrec; }
  u32 fl = rd32(c, rec + 4) | EXC_UNWINDING; if (!target) fl |= EXC_EXIT_UNWIND; wr32(c, rec + 4, fl);
  if (from_ctx) { u8 *s = mem_ptr(c->mem, from_ctx, CTX_SIZE, 0); u8 *d = mem_ptr(c->mem, uctx, CTX_SIZE, 1); if (s && d) e_memcpy(d, s, CTX_SIZE); }
  else { ctx_capture(c, uctx); wr64(c, uctx + 0x98, caller_rsp); }
  int ci = cont_push(c, CONT_UNWIND); if (ci < 0) return;
  w->cont[ci].data[0] = target; w->cont[ci].data[1] = target_ip; w->cont[ci].data[2] = rec; w->cont[ci].data[3] = retval; w->cont[ci].data[4] = uctx; w->cont[ci].data[5] = pctx; w->cont[ci].data[6] = dc;
  seh_log("[x64] RtlUnwindEx target frame 0x%llx ip 0x%llx\n", (unsigned long long)target, (unsigned long long)target_ip);
  unwind_step(c, ci);
}
/* RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, OriginalContext, HistoryTable) — the caller's own frame is where the walk starts */
/* the caller's OriginalContext buffer is NOT used as scratch: libunwind passes the dispatcher's ContextRecord (our saved exception context) there */
static void s_RtlUnwindEx(Cpu *c) { start_unwind(c, ARG(0), ARG(1), ARG(2), ARG(3), 0); }
static void s_RtlUnwind(Cpu *c) { if (c->mode32) { start_unwind32(c, ARG(0), ARG(1), ARG(2), ARG(3), 0); return; } start_unwind(c, ARG(0), ARG(1), ARG(2), ARG(3), 0); }

/* RtlRestoreContext(ctx, rec): jump; STATUS_UNWIND_CONSOLIDATE calls rec->ExceptionInformation[0](rec) on the restored stack and continues at its return value */
static void restore_context(Cpu *c, u64 ctx, u64 rec) {
  Win32 *w = W;
  if (rec && rd32(c, rec) == STATUS_UNWIND_CONSOLIDATE && rd32(c, rec + 24) >= 1) {
    u64 cb = rd64(c, rec + 32);   /* the callback runs on the current (deep) stack like RtlRestoreContext does: the exception object in the thrower's frame must survive the catch block */
    int ci = cont_push(c, CONT_CONSOLIDATE); if (ci < 0) return; w->cont[ci].data[0] = ctx;
    seh_log("[x64] unwind consolidate -> callback 0x%llx\n", (unsigned long long)cb);
    guest_call(c, cb, rec, 0, 0, 0); return;
  }
  ctx_load(c, ctx); cont_gc(c);
  seh_log("[x64] resume at 0x%llx rsp 0x%llx\n", (unsigned long long)c->rip, (unsigned long long)c->r[RSP]);
}
static void s_RtlRestoreContext(Cpu *c) { restore_context(c, ARG(0), ARG(1)); }

/* ---- guest-visible table helpers ---- */
static void s_AddVectoredExceptionHandler(Cpu *c) { Win32 *w = W; u64 h = ARG(1); if (!h || w->nveh >= 16) { RET(0); return; } if (ARG(0)) { for (int i = w->nveh; i > 0; i--) w->veh[i] = w->veh[i - 1]; w->veh[0] = h; } else w->veh[w->nveh] = h; w->nveh++; RET(h); }
static void s_RemoveVectoredExceptionHandler(Cpu *c) { Win32 *w = W; u64 h = ARG(0); for (int i = 0; i < w->nveh; i++) if (w->veh[i] == h) { for (int j = i; j + 1 < w->nveh; j++) w->veh[j] = w->veh[j + 1]; w->nveh--; RET(1); return; } RET(0); }
static void s_SetUnhandledExceptionFilter(Cpu *c) { Win32 *w = W; u64 prev = w->uef; w->uef = ARG(0); RET(prev); }
/* UnhandledExceptionFilter(EXCEPTION_POINTERS*): runs the SetUnhandledExceptionFilter callback like Windows does (its result is
   returned to the caller); with no callback the exception is unhandled — Windows would show the WER dialog and end the process,
   so the process is terminated with the exception code (and the code is reported) instead of looping on the faulting instruction */
static void s_UnhandledExceptionFilter(Cpu *c) { Win32 *w = W; u64 ptrs = ARG(0); u64 rec = ptrs ? rdp(c, ptrs) : 0; u32 code = rec ? rd32(c, rec) : 0; u64 addr = rec ? rdp(c, rec + (c->mode32 ? 12 : 16)) : 0;
  if (w->uef) { int ci = cont_push(c, CONT_RET_RAX); if (ci < 0) return; guest_call(c, w->uef, ptrs, 0, 0, 0); return; }
  char msg[200]; e_snprintf(msg, sizeof msg, "unhandled exception 0x%08x at 0x%llx (UnhandledExceptionFilter, no top-level filter) — process terminated", code, (unsigned long long)addr); w32_report(w, "%s", msg);
  seh_log("[x64] %s\n", msg); do_exit(c, (int)code); }
static void s_RtlLookupFunctionEntry(Cpu *c) { u64 base = 0; u64 fe = seh_lookup(c, ARG(0), &base); if (ARG(1)) wr64(c, ARG(1), base); RET(fe); }
static void s_RtlPcToFileHeader(Cpu *c) { u64 base = 0; u32 pd, ps; if (!seh_module(W, ARG(0), &base, &pd, &ps)) base = 0; if (ARG(1)) wr64(c, ARG(1), base); RET(base); }
static void s_RtlCaptureContext(Cpu *c) { ctx_capture(c, ARG(0)); }
/* RtlVirtualUnwind(HandlerType, ImageBase, ControlPc, FunctionEntry, ContextRecord, HandlerData*, EstablisherFrame*, ContextPointers) */
static void s_RtlVirtualUnwind(Cpu *c) {
  u64 ctx = ARG(4); UCtx x; uctx_read(c, ctx, &x); u64 hd = 0, est = 0;
  u64 h = seh_virtual_unwind_cp(c, (u32)ARG(0), ARG(1), ARG(2), ARG(3), &x, &hd, &est, ARG(7));
  uctx_write(c, ctx, &x); if (ARG(5)) wr64(c, ARG(5), hd); if (ARG(6)) wr64(c, ARG(6), est); RET(h);
}

/* ---- __C_specific_handler(rec, frame, ctx, dc): SCOPE_TABLE { count; { begin, end, handler, target }[] } ---- */
static void cspec_step(Cpu *c, int ci, int have_result, u64 rax) {
  Win32 *w = W; u64 rec = w->cont[ci].data[0], frame = w->cont[ci].data[1], ctx = w->cont[ci].data[2], dc = w->cont[ci].data[3];
  u64 base = rd64(c, dc + 8), tab = rd64(c, dc + 56), control = rd64(c, dc) - base, target_pc = rd64(c, dc + 32) - base;
  u32 flags = rd32(c, rec + 4), count = rd32(c, tab); int *pi = &w->cont[ci].idx;
  int unwinding = (flags & EXC_UNWIND_ANY) != 0;
  if (have_result && !unwinding) {   /* a filter returned */
    s32 r = (s32)(u32)rax; u32 i = (u32)*pi - 1; u64 e = tab + 4 + 16ull * i;
    if (r > 0) { u64 code = rd32(c, rec); u64 tgt = base + rd32(c, e + 12); w->ncont = ci; start_unwind(c, frame, tgt, rec, code, w->cont[ci].data[5]); return; }   /* EXCEPTION_EXECUTE_HANDLER */
    if (r < 0) { cont_finish(c, 0); return; }                                                                 /* EXCEPTION_CONTINUE_EXECUTION */
  }
  for (; (u32)*pi < count; ) {
    u32 i = (u32)(*pi)++; u64 e = tab + 4 + 16ull * i; u32 b = rd32(c, e), en = rd32(c, e + 4), h = rd32(c, e + 8), t = rd32(c, e + 12);
    if (control < b || control >= en) continue;
    if (!unwinding) {
      if (!t) continue;                                                                                       /* __finally: not in dispatch */
      if (h == 1) { u64 code = rd32(c, rec); w->ncont = ci; start_unwind(c, frame, base + t, rec, code, w->cont[ci].data[5]); return; }
      u64 ptrs = w->cont[ci].data[6]; wr64(c, ptrs, rec); wr64(c, ptrs + 8, ctx);
      guest_call(c, base + h, ptrs, frame, 0, 0); return;                                                     /* filter(EXCEPTION_POINTERS*, frame) */
    } else {
      if ((flags & EXC_TARGET_UNWIND) && target_pc >= b && target_pc < en) break;                             /* the target is inside this scope: stop */
      if (t) continue;                                                                                        /* __except scopes have nothing to run */
      guest_call(c, base + h, 1, frame, 0, 0); return;                                                        /* __finally funclet(TRUE, frame) */
    }
  }
  cont_finish(c, 1);   /* ExceptionContinueSearch */
}
static void s___C_specific_handler(Cpu *c) {
  Win32 *w = W; int ci = cont_push(c, CONT_CSPEC); if (ci < 0) return;
  w->cont[ci].data[0] = ARG(0); w->cont[ci].data[1] = ARG(1); w->cont[ci].data[2] = ARG(2); w->cont[ci].data[3] = ARG(3);
  /* phase-2 walks started from here begin at the raise point (the dispatcher's saved exception context), not at this shim */
  u64 from = 0; for (int k = ci - 1; k >= 0; k--) if (w->cont[k].kind == CONT_DISPATCH) { from = w->cont[k].data[1]; break; } else if (w->cont[k].kind == CONT_UNWIND) { from = w->cont[k].data[5]; break; }
  w->cont[ci].data[5] = from; w->cont[ci].data[6] = seh_scratch(c, 16); w->cont[ci].rsp = c->r[RSP];
  cspec_step(c, ci, 0, 0);
}
static int seh_cont_resume(Cpu *c, int kind, u64 rax) {
  Win32 *w = W; int ci = w->ncont - 1;
  switch (kind) {
    case CONT_DISPATCH: {
      u32 disp = (u32)rax;
      if (w->cont[ci].data[7] == 1) {   /* a vectored handler returned */
        if ((s32)rax == -1) { u64 octx = w->cont[ci].data[1]; w->ncont = ci; restore_context(c, octx, 0); return 1; }
        if (w->cont[ci].idx < w->nveh) { u64 h = w->veh[w->cont[ci].idx++]; guest_call(c, h, w->cont[ci].data[6], 0, 0, 0); return 1; }
        w->cont[ci].data[7] = 0; dispatch_step(c, ci); return 1;
      }
      if (w->cont[ci].data[7] == 2) {   /* the top-level filter returned */
        if ((s32)rax == -1) { u64 octx = w->cont[ci].data[1]; w->ncont = ci; restore_context(c, octx, 0); return 1; }
        dispatch_step(c, ci); return 1;   /* reports the unhandled exception */
      }
      if (disp == 0) { u64 octx = w->cont[ci].data[1]; w->ncont = ci; restore_context(c, octx, 0); return 1; }   /* ExceptionContinueExecution */
      if (disp == 1) { dispatch_step(c, ci); return 1; }
      cpu_fault(c, "exception handler returned disposition %u (nested exception / collided unwind are not supported)", disp); return 1; }
    case CONT_UNWIND: {
      u32 disp = (u32)rax;
      if (disp == 1 || disp == 3) { if (w->cont[ci].idx) { u64 pctx = w->cont[ci].data[5], rec = w->cont[ci].data[2]; wr64(c, pctx + 0xf8, w->cont[ci].data[1]); wr64(c, pctx + 0x78, w->cont[ci].data[3]); w->ncont = ci; restore_context(c, pctx, rec); return 1; } unwind_step(c, ci); return 1; }
      cpu_fault(c, "unwind handler returned disposition %u", disp); return 1; }
    case CONT_CONSOLIDATE: { u64 ctx = w->cont[ci].data[0]; w->ncont = ci; ctx_load(c, ctx); c->rip = rax; cont_gc(c); return 1; }
    case CONT_CSPEC: cspec_step(c, ci, 1, rax); return 1;
  }
  return 0;
}
#define SEH_SHIMS \
  S(RaiseException), S(AddVectoredExceptionHandler), S(RemoveVectoredExceptionHandler), S(SetUnhandledExceptionFilter), S(UnhandledExceptionFilter), S(RtlUnwindEx), S(RtlUnwind), S(RtlRestoreContext), S(RtlLookupFunctionEntry), S(RtlPcToFileHeader), S(RtlCaptureContext), S(RtlVirtualUnwind), \
  SA("__C_specific_handler", __C_specific_handler), SA("_CxxThrowException", _CxxThrowException), SA("RtlCaptureStackBackTrace", ret0),
