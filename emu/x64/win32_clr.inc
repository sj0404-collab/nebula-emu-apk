/* win32_clr.inc — Win32 surface needed by the official .NET runtime (hostfxr/hostpolicy/coreclr/System.*.dll, win-x64).
   Included from win32.c after the other shim files; CLR_SHIMS is appended at the END of shims[] so its entries win. */

/* ---- wide CRT conversions (UCRT api-ms-win-crt-convert / time / runtime) ---- */
static long long clr_wcstoll(Cpu *c, u64 s, u64 endp, int base, int *ok) {
  char buf[128]; wstr_to_utf8(c, s, buf, sizeof buf, 127); char *e; long long v = e_strtoll(buf, &e, base);
  if (endp) wrp(c, endp, s + (u64)(e - buf) * 2); if (ok) *ok = e != buf; return v;
}
static void s_wcstoul(Cpu *c)  { RET((u64)(u32)clr_wcstoll(c, ARG(0), ARG(1), (int)ARG(2), 0)); }
static void s_wcstol(Cpu *c)   { RET((u64)(s64)(s32)clr_wcstoll(c, ARG(0), ARG(1), (int)ARG(2), 0)); }
static void s_wcstoll(Cpu *c)  { RET((u64)clr_wcstoll(c, ARG(0), ARG(1), (int)ARG(2), 0)); }
static void s_wcstoull(Cpu *c) { RET((u64)clr_wcstoll(c, ARG(0), ARG(1), (int)ARG(2), 0)); }
static void s_wtoi(Cpu *c)     { RET((u64)(s64)(s32)clr_wcstoll(c, ARG(0), 0, 10, 0)); }
static void s_wtoi64(Cpu *c)   { RET((u64)clr_wcstoll(c, ARG(0), 0, 10, 0)); }
static void s_wcstod(Cpu *c)   { char buf[128]; wstr_to_utf8(c, ARG(0), buf, sizeof buf, 127); char *e; double v = e_strtod(buf, &e); if (ARG(1)) wrp(c, ARG(1), ARG(0) + (u64)(e - buf) * 2); c->x[0].f64[0] = v; }
/* wcsftime(buf, count, fmt, tm): format through the narrow implementation */
static void s_wcsftime(Cpu *c) { char f[256]; wstr_to_utf8(c, ARG(2), f, sizeof f, -1); char out[512]; int n = fmt_tm(c, out, sizeof out, f, ARG(3)); if ((u64)n + 1 > ARG(1)) { RET(0); return; } put_w(c, ARG(0), out, 0); RET(n); }
static void s_wcserror_s(Cpu *c) { put_w(c, ARG(0), errstr((int)ARG(2)), (u32)ARG(1)); RET(0); }
static void s_register_thread_local_exe_atexit_callback(Cpu *c) { RET(0); }
static void s_stdio_common_vsnwprintf_s(Cpu *c) { vsprintf_common(c, ARG(0) | 2, ARG(1), ARG(2) < ARG(3) ? ARG(2) : ARG(3) + 1, ARG(4), ARG(6), 1); }

/* ---- wide ctype (ASCII + Latin-1 letters; the runtime uses its own Unicode tables for everything else) ---- */
static int clr_wupper(u32 ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7); }
static int clr_wlower(u32 ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 0xDF && ch <= 0xFF && ch != 0xF7); }
static void s_iswascii(Cpu *c) { RET((u32)ARG(0) < 128); }
static void s_iswupper(Cpu *c) { RET(clr_wupper((u32)ARG(0)) ? 1 : 0); }
static void s_iswlower(Cpu *c) { RET(clr_wlower((u32)ARG(0)) ? 1 : 0); }
static void s_iswcntrl(Cpu *c) { u32 ch = (u32)ARG(0); RET(ch < 32 || ch == 127 || (ch >= 0x80 && ch < 0xA0)); }
static void s_iswprint(Cpu *c) { u32 ch = (u32)ARG(0); RET((ch >= 32 && ch < 127) || (ch >= 0xA0 && ch <= 0xFF)); }
static void s_iswgraph(Cpu *c) { u32 ch = (u32)ARG(0); RET((ch > 32 && ch < 127) || (ch > 0xA0 && ch <= 0xFF)); }
static void s_iswpunct(Cpu *c) { u32 ch = (u32)ARG(0); int an = (ch >= '0' && ch <= '9') || ((ch | 32) >= 'a' && (ch | 32) <= 'z'); RET(ch > 32 && ch < 127 && !an); }
static void s_iswctype(Cpu *c) { u32 ch = (u32)ARG(0), m = (u32)ARG(1), r = 0;   /* _UPPER 1 _LOWER 2 _DIGIT 4 _SPACE 8 _PUNCT 0x10 _CONTROL 0x20 _BLANK 0x40 _HEX 0x80 _ALPHA 0x100 */
  if (clr_wupper(ch)) r |= 0x101; if (clr_wlower(ch)) r |= 0x102; if (ch >= '0' && ch <= '9') r |= 4; if (ch == ' ' || (ch >= 9 && ch <= 13)) r |= 8; if (ch == ' ' || ch == 9) r |= 0x40;
  if (ch < 32 || ch == 127) r |= 0x20; if (((ch | 32) >= 'a' && (ch | 32) <= 'f') || (ch >= '0' && ch <= '9')) r |= 0x80; if (ch > 32 && ch < 127 && !(r & 0x104)) r |= 0x10; RET((r & m) ? 1 : 0); }
/* GetCPInfoExW(cp, flags, CPINFOEXW*): single-byte for 1252/ANSI, UTF-8 = 4 bytes max */
static void s_GetCPInfoExW(Cpu *c) { u32 cp = (u32)ARG(0); u64 a = ARG(2); u8 *p = mem_ptr(c->mem, a, 0x1a4, 1); if (!p) { W->last_error = 87; RET(0); return; } e_memset(p, 0, 0x1a4);
  if (cp == 0 || cp == 1) cp = 1252; if (cp == 65001) { wr32(c, a, 4); } else if (cp == 1200 || cp == 1201) { W->last_error = 87; RET(0); return; } else wr32(c, a, 1);
  wr8(c, a + 4, '?'); wr32(c, a + 18, cp == 65001 ? 0xfffd : '?'); wr32(c, a + 22, cp); put_w(c, a + 26, cp == 65001 ? "65001 (UTF-8)" : "1252 (ANSI - Latin I)", 260); RET(1); }
/* ---- VirtualQuery that knows thread stacks: the CLR's stack-overflow detection looks for a PAGE_GUARD page at the bottom of the
   stack (Thread::DetermineIfGuardPagePresent) and treats "no guard page" as a stack overflow in progress → "Stack overflow." ---- */
static int clr_stack_of(Win32 *w, u64 addr, u64 *lo, u64 *hi) {
  if (addr - G_STACK_BASE < G_STACK_SIZE) { *lo = G_STACK_BASE; *hi = G_STACK_BASE + G_STACK_SIZE; return 1; }
  for (int i = 0; i < THREAD_MAX; i++) { Thread *t = &w->th[i]; if (t->used && t->stack && addr - t->stack < t->stack_size) { *lo = t->stack; *hi = t->stack + t->stack_size; return 1; } }
  return 0;
}
static void s_VirtualQuery(Cpu *c) {
  u64 addr = ARG(0), mbi = ARG(1), lo, hi; u32 sz = c->mode32 ? 28 : 48; if (ARG(2) < sz) sz = (u32)ARG(2); u8 *p = mem_ptr(c->mem, mbi, sz, 1); if (!p) { RET(0); return; } e_memset(p, 0, sz);   /* never clear more than the caller's struct: a 32-bit MBI is 28 bytes and sits right under the return address */
  if (c->mode32) {   /* MEMORY_BASIC_INFORMATION32: BaseAddress@0 AllocationBase@4 AllocationProtect@8 RegionSize@12 State@16 Protect@20 Type@24 (28 bytes) */
    u64 page = addr & ~0xfffull; Region *r = mem_region(c->mem, addr);
    if (!r) { wr32(c, mbi, (u32)page); wr32(c, mbi + 12, 0x1000); wr32(c, mbi + 16, 0x10000); wr32(c, mbi + 20, 1); RET(28); return; }
    wr32(c, mbi, (u32)page); wr32(c, mbi + 4, (u32)r->base); wr32(c, mbi + 8, 0x40); wr32(c, mbi + 12, (u32)(r->base + r->size - page)); wr32(c, mbi + 16, 0x1000); wr32(c, mbi + 20, 0x40); wr32(c, mbi + 24, r->base == W->image_base ? 0x1000000 : 0x20000); RET(28); return;
  }
  if (clr_stack_of(W, addr, &lo, &hi)) {
    u64 page = addr & ~0xfffull; int guard = page < lo + 0x2000;
    wr64(c, mbi, page); wr64(c, mbi + 8, lo); wr32(c, mbi + 16, 4); wr64(c, mbi + 24, guard ? lo + 0x2000 - page : hi - page); wr32(c, mbi + 32, 0x1000); wr32(c, mbi + 36, guard ? 0x104 : 4); wr32(c, mbi + 40, 0x20000); RET(48); return;
  }
  Region *r = mem_region(c->mem, addr);
  if (!r) { wr64(c, mbi, addr & ~0xfffull); wr64(c, mbi + 24, 0x1000); wr32(c, mbi + 32, 0x10000); wr32(c, mbi + 36, 1); RET(48); return; }
  wr64(c, mbi, addr & ~0xfffull); wr64(c, mbi + 8, r->base); wr32(c, mbi + 16, 0x40); wr64(c, mbi + 24, r->base + r->size - (addr & ~0xfffull)); wr32(c, mbi + 32, 0x1000); wr32(c, mbi + 36, 0x40); wr32(c, mbi + 40, r->base == W->image_base ? 0x1000000 : 0x20000); RET(48);
}
static void s_VirtualQueryEx_clr(Cpu *c) { call_shim(c, s_VirtualQuery, ARG(1), ARG(2), ARG(3), 0); }
/* ---- processor groups / NUMA: one group, one node ---- */
static void s_GetMaximumProcessorGroupCount(Cpu *c) { RET(1); }
static void s_GetCurrentProcessorNumberEx(Cpu *c) { if (ARG(0)) { wr16(c, ARG(0), 0); wr8(c, ARG(0) + 2, 0); } RET(0); }
static void s_GetNumaHighestNodeNumber(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), 0); RET(1); }
static void s_GetNumaProcessorNodeEx(Cpu *c) { if (ARG(1)) wr16(c, ARG(1), 0); RET(1); }
static void s_GetProcessGroupAffinity(Cpu *c) { u64 cnt = ARG(1); u16 n = cnt ? rd16(c, cnt) : 0; if (cnt) wr16(c, cnt, 1); if (n < 1) { W->last_error = 122; RET(0); return; } if (ARG(2)) wr16(c, ARG(2), 0); RET(1); }
static void s_GetThreadGroupAffinity(Cpu *c) { u64 ga = ARG(1); if (ga) { wr64(c, ga, 1); wr16(c, ga + 8, 0); wr16(c, ga + 10, 0); wr16(c, ga + 12, 0); wr16(c, ga + 14, 0); } RET(1); }
static void s_SetThreadGroupAffinity(Cpu *c) { if (ARG(2)) { wr64(c, ARG(2), 1); wr16(c, ARG(2) + 8, 0); } RET(1); }
static void s_SetThreadIdealProcessorEx(Cpu *c) { if (ARG(2)) { wr16(c, ARG(2), 0); wr8(c, ARG(2) + 2, 0); } RET(1); }
static void s_GetThreadIdealProcessorEx(Cpu *c) { if (ARG(1)) { wr16(c, ARG(1), 0); wr8(c, ARG(1) + 2, 0); } RET(1); }
static void s_VirtualAllocExNuma(Cpu *c) { s_VirtualAllocEx(c); }
static void s_QueryInformationJobObject(Cpu *c) { W->last_error = 87; RET(0); }
static void s_RaiseFailFastException(Cpu *c) { cpu_fault(c, "RaiseFailFastException: the program asked to be terminated (record 0x%llx)", (unsigned long long)ARG(0)); }
/* XState (AVX register state in CONTEXT): we have no AVX, so the legacy CONTEXT is the whole state */
static void s_GetEnabledXStateFeatures(Cpu *c) { RET(3); }
static void s_InitializeContext(Cpu *c) { u64 len = ARG(3); u32 need = 0x4d0 + 16; u32 have = len ? rd32(c, len) : 0; if (len) wr32(c, len, need); if (!ARG(0) || have < need) { W->last_error = 122; RET(0); return; } u64 ctx = (ARG(0) + 15) & ~15ull; u8 *p = mem_ptr(c->mem, ctx, 0x4d0, 1); if (p) e_memset(p, 0, 0x4d0); wr32(c, ctx + 0x30, (u32)ARG(1)); if (ARG(2)) wr64(c, ARG(2), ctx); RET(1); }
static void s_SetXStateFeaturesMask(Cpu *c) { RET(1); }
static void s_LocateXStateFeature(Cpu *c) { u32 f = (u32)ARG(1); if (f == 1) { if (ARG(2)) wr32(c, ARG(2), 512); RET(ARG(0) + 0x100); return; } if (ARG(2)) wr32(c, ARG(2), 0); RET(0); }
static void s_CopyContext(Cpu *c) { u8 *d = mem_ptr(c->mem, ARG(0), 0x4d0, 1); const u8 *s = mem_ptr(c->mem, ARG(2), 0x4d0, 0); if (!d || !s) { W->last_error = 87; RET(0); return; } e_memcpy(d, s, 0x4d0); wr32(c, ARG(0) + 0x30, (u32)ARG(1)); RET(1); }
static void s_WerRegisterRuntimeExceptionModule(Cpu *c) { RET(0); }
static void s_GetSystemDefaultLCID(Cpu *c) { RET(0x409); }
static void s_QueryUnbiasedInterruptTime(Cpu *c) { if (ARG(0)) wr64(c, ARG(0), (u64)((host_now_ms() - W->start_ms + 100000.0) * 10000.0)); RET(1); }
/* GetSystemTimes(idle, kernel, user): one virtual CPU that has been ~30% busy since the emulator started */
static void s_GetSystemTimes(Cpu *c) { u64 up = (u64)((host_now_ms() - W->start_ms + 100000.0) * 10000.0); if (ARG(0)) wr64(c, ARG(0), up * 7 / 10); if (ARG(1)) wr64(c, ARG(1), up * 8 / 10); if (ARG(2)) wr64(c, ARG(2), up * 2 / 10); RET(1); }
static void s_DisconnectNamedPipe(Cpu *c) { RET(1); }
/* ---- Get/SetThreadContext: the GC suspends managed threads and reads/redirects their registers ---- */
static int clr_in_shim(u64 rip) { return rip - G_SHIM_BASE < G_SHIM_SIZE; }
static void clr_ctx_write(Cpu *c, u64 a, Cpu *t, u32 want, int other) {
  u64 rip = t->rip, rsp = t->r[RSP]; int in_call = 0;
  if (other && clr_in_shim(rip)) { rip = rd64(c, rsp); rsp += 8; in_call = 1; }   /* parked inside a Win32 call: the caller's frame, as the kernel would report it */
  u32 flags = want & 0x10000f; if (want & 0x40000000) flags |= 0x80000000u | (in_call ? 0x10000000u : 0);   /* CONTEXT_EXCEPTION_REPORTING + SERVICE_ACTIVE */
  wr32(c, a + 0x30, flags); wr32(c, a + 0x34, t->mxcsr); wr16(c, a + 0x38, 0x33); wr16(c, a + 0x3a, 0x2b); wr16(c, a + 0x3c, 0x2b); wr16(c, a + 0x3e, 0x2b); wr16(c, a + 0x40, 0x53); wr16(c, a + 0x42, 0x2b); wr32(c, a + 0x44, (u32)t->rflags);
  for (int i = 0; i < 16; i++) wr64(c, a + 0x78 + 8 * i, i == RSP ? rsp : t->r[i]);
  wr64(c, a + 0xf8, rip);
  wr32(c, a + 0x100 + 0x18, t->mxcsr);   /* FltSave.MxCsr */
  for (int i = 0; i < 16; i++) { wr64(c, a + 0x1a0 + 16 * i, t->x[i].q[0]); wr64(c, a + 0x1a8 + 16 * i, t->x[i].q[1]); }
}
static void s_GetThreadContext(Cpu *c) {
  u64 a = ARG(1); if (!a) { W->last_error = 87; RET(0); return; }
  Thread *t = ARG(0) == ~1ull ? cur_thread(W) : thread_of_handle(c, ARG(0)); if (!t || !t->cpu) { W->last_error = 6; RET(0); return; }
  u32 want = rd32(c, a + 0x30); int other = t != cur_thread(W);
  if (other && t->state == 1 && t->suspend <= 0) { W->last_error = 5; RET(0); return; }   /* must be suspended first (as on Windows) */
  clr_ctx_write(c, a, other ? t->cpu : c, want, other); RET(1);
}
static void s_SetThreadContext(Cpu *c) {
  u64 a = ARG(1); if (!a) { W->last_error = 87; RET(0); return; }
  Thread *t = ARG(0) == ~1ull ? cur_thread(W) : thread_of_handle(c, ARG(0)); if (!t || !t->cpu) { W->last_error = 6; RET(0); return; }
  Cpu *x = t == cur_thread(W) ? c : t->cpu; u32 flags = rd32(c, a + 0x30);
  if (x != c && clr_in_shim(x->rip)) { x->rip = rd64(c, x->r[RSP]); x->r[RSP] += 8; }   /* leave the parked call: the new context replaces it */
  if (flags & 0x2) for (int i = 0; i < 16; i++) if (i != RSP) x->r[i] = rd64(c, a + 0x78 + 8 * i);
  if (flags & 0x1) { x->r[RSP] = rd64(c, a + 0x98); x->rip = rd64(c, a + 0xf8); x->rflags = (x->rflags & ~0x8d5ull) | (rd32(c, a + 0x44) & 0x8d5u); }
  if (flags & 0x8) { x->mxcsr = rd32(c, a + 0x34); for (int i = 0; i < 16; i++) { x->x[i].q[0] = rd64(c, a + 0x1a0 + 16 * i); x->x[i].q[1] = rd64(c, a + 0x1a8 + 16 * i); } }
  RET(1);
}
/* ---- ADVAPI32 tokens / SIDs ---- */
static void s_OpenThreadToken(Cpu *c) { W->last_error = 1008; RET(0); }   /* ERROR_NO_TOKEN: no impersonation */
static void s_RevertToSelf(Cpu *c) { RET(1); }
static void s_SetThreadToken(Cpu *c) { RET(1); }
static void s_SetKernelObjectSecurity(Cpu *c) { RET(1); }
static void s_GetSidSubAuthorityCount(Cpu *c) { RET(ARG(0) + 1); }
static void s_GetSidSubAuthority(Cpu *c) { RET(ARG(0) + 8 + ARG(1) * 4); }
/* ---- ole32 (COM is not provided: the runtime only touches it for interop/apartments) ---- */
static void s_CoTaskMemAlloc(Cpu *c) { RET(w32_heap_alloc(W, ARG(0) ? ARG(0) : 1, 0)); }
static void s_CoTaskMemRealloc(Cpu *c) { if (!ARG(1)) { if (ARG(0)) w32_heap_free(W, ARG(0)); RET(0); return; } RET(heap_realloc(W, ARG(0), ARG(1), 0)); }
static void s_CoCreateGuid(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(0), 16, 1); if (!p) { RET(0x80070057); return; } static u32 seed = 0x9e3779b9u; for (int i = 0; i < 16; i++) { seed = seed * 1103515245u + 12345u; p[i] = (u8)(seed >> 16); } p[7] = (p[7] & 0x0f) | 0x40; p[8] = (p[8] & 0x3f) | 0x80; RET(0); }
static void s_CoGetContextToken(Cpu *c) { RET(0x800401F0); }   /* CO_E_NOTINITIALIZED */
static void s_CoGetObjectContext(Cpu *c) { RET(0x800401F0); }
static void s_CoRegisterInitializeSpy(Cpu *c) { RET(0x80004001); }
static void s_CoRevokeInitializeSpy(Cpu *c) { RET(0); }
static void s_CoWaitForMultipleHandles(Cpu *c) { RET(0x80004001); }
static void s_CoNotImpl(Cpu *c) { RET(0x80004001); }   /* E_NOTIMPL */
/* ---- WinHttp: only proxy discovery (SocketsHttpHandler asks for the system proxy) — answer "direct connection" ---- */
static void s_WinHttpGetIEProxyConfigForCurrentUser(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(0), 32, 1); if (p) { e_memset(p, 0, 32); wr32(c, ARG(0), 0); } RET(1); }   /* fAutoDetect = FALSE, no URLs */
static void s_WinHttpGetDefaultProxyConfiguration(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(0), 24, 1); if (p) { e_memset(p, 0, 24); wr32(c, ARG(0), 1); } RET(1); }   /* WINHTTP_ACCESS_TYPE_NO_PROXY */
static void s_WinHttpOpen(Cpu *c) { RET(0x7aa0001); }
static void s_WinHttpGetProxyForUrl(Cpu *c) { W->last_error = 12180; RET(0); }   /* ERROR_WINHTTP_AUTODETECTION_FAILED */
static void s_WinHttpCloseHandle(Cpu *c) { RET(1); }
static void s_WinHttpFail(Cpu *c) { W->last_error = 12029; RET(0); }   /* ERROR_WINHTTP_CANNOT_CONNECT */
/* version.dll: no version resources are readable here */
static void s_GetFileVersionInfoSizeExW(Cpu *c) { if (ARG(2)) wr32(c, ARG(2), 0); W->last_error = 1813; RET(0); }
/* combase / WinRT: no Windows Runtime here */
static void s_RoInitialize(Cpu *c) { RET(0); }
static void s_RoGetActivationFactory(Cpu *c) { if (ARG(2)) wrp(c, ARG(2), 0); RET(0x80040154); }   /* REGDB_E_CLASSNOTREG */

static void s_GetThreadIOPendingFlag(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(1); }   /* no outstanding I/O requests on this thread */
#define CLR_SHIMS \
  S(wcstoul), S(wcstol), S(wcstoll), S(wcstoull), SA("_wcstoui64", wcstoull), SA("_wcstoi64", wcstoll), SA("_wtoi", wtoi), SA("_wtol", wtoi), SA("_wtoi64", wtoi64), S(wcstod), S(wcsftime), SA("_wcserror_s", wcserror_s), \
  SA("_register_thread_local_exe_atexit_callback", register_thread_local_exe_atexit_callback), SA("__stdio_common_vsnwprintf_s", stdio_common_vsnwprintf_s), \
  S(iswascii), S(iswupper), S(iswlower), S(iswcntrl), S(iswprint), S(iswgraph), S(iswpunct), S(iswctype), SA("_iswctype_l", iswctype), S(GetCPInfoExW), SA("GetCPInfoExA", GetCPInfoExW), \
  S(VirtualQuery), SA("VirtualQueryEx", VirtualQueryEx_clr), \
  S(GetMaximumProcessorGroupCount), S(GetCurrentProcessorNumberEx), S(GetNumaHighestNodeNumber), S(GetNumaProcessorNodeEx), S(GetProcessGroupAffinity), S(GetThreadGroupAffinity), S(SetThreadGroupAffinity), \
  S(SetThreadIdealProcessorEx), S(GetThreadIdealProcessorEx), S(VirtualAllocExNuma), S(QueryInformationJobObject), S(RaiseFailFastException), S(GetEnabledXStateFeatures), S(InitializeContext), S(SetXStateFeaturesMask), \
  S(LocateXStateFeature), S(CopyContext), S(WerRegisterRuntimeExceptionModule), S(GetSystemDefaultLCID), S(GetSystemTimes), S(QueryUnbiasedInterruptTime), SA("QueryUnbiasedInterruptTimePrecise", QueryUnbiasedInterruptTime), SA("QueryInterruptTime", QueryUnbiasedInterruptTime), SA("QueryInterruptTimePrecise", QueryUnbiasedInterruptTime), S(DisconnectNamedPipe), \
  S(GetThreadContext), S(SetThreadContext), \
  S(OpenThreadToken), S(RevertToSelf), S(SetThreadToken), S(SetKernelObjectSecurity), S(GetSidSubAuthorityCount), S(GetSidSubAuthority), \
  S(CoTaskMemAlloc), S(CoTaskMemRealloc), S(CoCreateGuid), S(CoGetContextToken), S(CoGetObjectContext), S(CoRegisterInitializeSpy), S(CoRevokeInitializeSpy), S(CoWaitForMultipleHandles), \
  SA("CoCreateFreeThreadedMarshaler", CoNotImpl), SA("CoGetClassObject", CoNotImpl), SA("CoUnmarshalInterface", CoNotImpl), SA("CoMarshalInterface", CoNotImpl), SA("CoGetMarshalSizeMax", CoNotImpl), SA("CLSIDFromProgID", CoNotImpl), SA("CoReleaseMarshalData", CoNotImpl), \
  S(WinHttpGetIEProxyConfigForCurrentUser), S(WinHttpGetDefaultProxyConfiguration), S(WinHttpOpen), S(WinHttpGetProxyForUrl), S(WinHttpCloseHandle), SA("WinHttpConnect", WinHttpFail), SA("WinHttpOpenRequest", WinHttpFail), SA("WinHttpSendRequest", WinHttpFail), SA("WinHttpReceiveResponse", WinHttpFail), SA("WinHttpSetOption", ret1), SA("WinHttpQueryOption", WinHttpFail), SA("WinHttpSetStatusCallback", ret0), SA("WinHttpSetTimeouts", ret1), SA("WinHttpQueryHeaders", WinHttpFail), SA("WinHttpQueryDataAvailable", WinHttpFail), SA("WinHttpReadData", WinHttpFail), SA("WinHttpWriteData", WinHttpFail), SA("WinHttpAddRequestHeaders", WinHttpFail), SA("WinHttpCrackUrl", WinHttpFail), SA("WinHttpCreateUrl", WinHttpFail), SA("WinHttpResetAutoProxy", ret0), SA("WinHttpGetProxyForUrlEx", WinHttpFail), SA("WinHttpCreateProxyResolver", WinHttpFail), SA("WinHttpFreeProxyResult", ret0), \
  S(GetFileVersionInfoSizeExW), SA("GetFileVersionInfoSizeW", GetFileVersionInfoSizeExW), SA("GetFileVersionInfoExW", ret0), SA("GetFileVersionInfoW", ret0), SA("VerQueryValueW", ret0), \
  S(RoInitialize), SA("RoUninitialize", ret0), S(RoGetActivationFactory), SA("RoOriginateLanguageException", ret0), SA("RoOriginateError", ret0), SA("RoOriginateErrorW", ret0), SA("SetRestrictedErrorInfo", ret0), SA("GetRestrictedErrorInfo", ret1), SA("RoGetErrorReportingFlags", ret0), SA("RoSetErrorReportingFlags", ret0), SA("RoReportUnhandledError", ret0), \
  S(GetThreadIOPendingFlag),
