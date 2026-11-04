/* Shims needed by the REAL Visual C++ runtime DLLs (msvcp140*.dll, vcruntime140_1.dll, concrt140, vcomp140 from the
 * VC++ redistributable) when they run on top of the emulator's kernel32/ucrt. These are UCRT/vcruntime internals the
 * STL binary imports: locale locks, the _Getdays/_Getmonths/_Gettnames time tables, _Strftime/_Wcsftime, wide stdio,
 * __AdjustPointer, __uncaught_exceptions … Only the C++ library itself calls them. */
static void s_lock_locales(Cpu *c) { RET(0); }
static u64 rt_scratch(Win32 *w) { if (!w->rt_scratch) w->rt_scratch = w32_heap_alloc(w, 256, 1); return w->rt_scratch; }   /* zeroed guest block: ___lc_locale_name (6 x wchar_t* = NULL → "C"), __processing_throw counter */
static void s_lc_locale_name_func(Cpu *c) { RET(rt_scratch(W)); }
static void s_processing_throw(Cpu *c) { RET(rt_scratch(W) + 64); }
static void s_invoke_watson(Cpu *c) { con_out(W, 2, "_invoke_watson: invalid parameter\n", 34); do_exit(c, 3); }
static void s_strncnt(Cpu *c) { u64 n = ARG(1), i = 0; while (i < n && rd8(c, ARG(0) + i)) i++; RET(i); }
static void s_wcsdup(Cpu *c) { u64 s = ARG(0); if (!s) { RET(0); return; } u64 n = 0; while (rd16(c, s + n * 2)) n++; u64 a = w32_heap_alloc(W, (n + 1) * 2, 0); for (u64 i = 0; i <= n; i++) wr16(c, a + i * 2, rd16(c, s + i * 2)); RET(a); }
static void s_iswxdigit(Cpu *c) { u32 ch = (u32)ARG(0); RET(ch < 128 && ((ch >= '0' && ch <= '9') || ((ch | 32) >= 'a' && (ch | 32) <= 'f'))); }
static void s_iswalnum(Cpu *c) { u32 ch = (u32)ARG(0); RET(ch < 128 && ((ch >= '0' && ch <= '9') || ((ch | 32) >= 'a' && (ch | 32) <= 'z'))); }
static void s_btowc(Cpu *c) { u32 ch = (u32)ARG(0); RET(ch == 0xffffffffu ? 0xffffu : ch < 128 ? ch : 0xffffu); }
static void s_strtof(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (!s) { c->x[0].f32[0] = 0; return; } char *e; double v = e_strtod(s, &e); if (ARG(1)) wrp(c, ARG(1), ARG(0) + (u64)(e - s)); c->x[0].f32[0] = (float)v; }
/* wide stdio on byte streams (the STL only uses these for wfstream/wcout on narrow files) */
static void s_fgetwc(Cpu *c) { s_fgetc(c); if (c->status != RUN_OK) return; if (c->r[RAX] == ~0ull) c->r[RAX] = 0xffff; }
static void s_fputwc(Cpu *c) { u32 wc = (u32)ARG(0); char ch = (char)(wc < 256 ? wc : '?'); int wr = stream_write(c, ARG(1), &ch, 1); if (!wr) return; RET(wr < 0 ? 0xffffull : wc); }
static void s_ungetwc(Cpu *c) { s_ungetc(c); if (c->r[RAX] == ~0ull) c->r[RAX] = 0xffff; }
/* time tables of the "C" locale, malloc'd (the STL frees them with free()) */
static const char *const rt_days = ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday";
static const char *const rt_months = ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:Nov:November:Dec:December";
static void s_Getdays(Cpu *c) { RET(w32_guest_strdup(W, rt_days)); }
static void s_Getmonths(Cpu *c) { RET(w32_guest_strdup(W, rt_months)); }
static u64 rt_wide_dup(Cpu *c, const char *s) { size_t n = e_strlen(s); u64 a = w32_heap_alloc(W, (n + 1) * 2, 0); for (size_t i = 0; i <= n; i++) wr16(c, a + i * 2, (u8)s[i]); return a; }
static void s_W_Getdays(Cpu *c) { RET(rt_wide_dup(c, rt_days)); }
static void s_W_Getmonths(Cpu *c) { RET(rt_wide_dup(c, rt_months)); }
/* <fenv.h> over the emulated MXCSR (MSVC's FE_* flag bits equal the MXCSR exception bits; rounding = MXCSR.RC << 5 → 0x000/0x100/0x200/0x300).
   fenv_t is { unsigned long _Fe_ctl, _Fe_stat }.  Only flags the emulated SSE/x87 units actually raise are visible (honest limit:
   the interpreter sets few of them, so numpy-style "divide by zero" warnings are mostly silent). */
static void s_fetestexcept(Cpu *c) { RET(c->mxcsr & (u32)ARG(0) & 0x3f); }
static void s_feclearexcept(Cpu *c) { c->mxcsr &= ~((u32)ARG(0) & 0x3f); RET(0); }
static void s_feraiseexcept(Cpu *c) { c->mxcsr |= (u32)ARG(0) & 0x3f; RET(0); }
static void s_fegetexceptflag(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), c->mxcsr & (u32)ARG(1) & 0x3f); RET(0); }
static void s_fesetexceptflag(Cpu *c) { u32 m = (u32)ARG(1) & 0x3f; c->mxcsr = (c->mxcsr & ~m) | (rd32(c, ARG(0)) & m); RET(0); }
static void s_fegetround(Cpu *c) { RET((c->mxcsr >> 5) & 0x300); }
static void s_fesetround(Cpu *c) { u32 r = (u32)ARG(0); if (r & ~0x300u) { RET(1); return; } c->mxcsr = (c->mxcsr & ~0x6000u) | (r << 5); c->fpu_cw = (u16)((c->fpu_cw & ~0xc00u) | (r << 2)); RET(0); }
static void s_fegetenv(Cpu *c) { if (ARG(0)) { wr32(c, ARG(0), c->mxcsr & ~0x3fu); wr32(c, ARG(0) + 4, c->mxcsr & 0x3f); } RET(0); }
static void s_fesetenv(Cpu *c) { if (ARG(0)) c->mxcsr = (rd32(c, ARG(0)) & ~0x3fu) | (rd32(c, ARG(0) + 4) & 0x3f); RET(0); }
static void s_feholdexcept(Cpu *c) { s_fegetenv(c); c->mxcsr = (c->mxcsr & ~0x3fu) | 0x1f80; RET(0); }
static void s_feupdateenv(Cpu *c) { u32 fl = c->mxcsr & 0x3f; s_fesetenv(c); c->mxcsr |= fl; RET(0); }
static u32 sw_of_mxcsr(u32 m) { return ((m & 0x20) ? 0x1 : 0) | ((m & 0x10) ? 0x2 : 0) | ((m & 0x8) ? 0x4 : 0) | ((m & 0x4) ? 0x8 : 0) | ((m & 0x1) ? 0x10 : 0) | ((m & 0x2) ? 0x80000 : 0); }
static void s_statusfp(Cpu *c) { RET(sw_of_mxcsr(c->mxcsr)); }
static void s_clearfp(Cpu *c) { RET(sw_of_mxcsr(c->mxcsr)); c->mxcsr &= ~0x3fu; }
static void s_statusfp2(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), sw_of_mxcsr(c->mxcsr)); if (ARG(1)) wr32(c, ARG(1), sw_of_mxcsr(c->mxcsr)); RET(0); }
/* _get_stream_buffer_pointers(FILE*, char ***base, char ***ptr, int **cnt): the STL's basic_filebuf points its get/put
   areas straight into the FILE (_base @0x10, _ptr @0x0, _cnt @0x8 — the classic msvcrt layout our FILE slots keep zeroed,
   so the filebuf sees "no buffer" and falls back to fgetc/fputc).  The old ret0 stub left the out-pointers NULL and the
   filebuf destructor dereferenced them at exit (crash in cout's filebuf). */
static void s_get_stream_buffer_pointers(Cpu *c) { u64 f = ARG(0);
  if (!f) { RET((u64)22); return; }
  if (c->mode32) { if (ARG(1)) wr32(c, ARG(1), (u32)f + 8); if (ARG(2)) wr32(c, ARG(2), (u32)f); if (ARG(3)) wr32(c, ARG(3), (u32)f + 4); RET(0); return; }   /* 32-bit FILE: _ptr@0 _cnt@4 _base@8 */
  if (ARG(1)) wr64(c, ARG(1), f + 0x10); if (ARG(2)) wr64(c, ARG(2), f); if (ARG(3)) wr64(c, ARG(3), f + 8); RET(0); }
static void s_Gettnames(Cpu *c) { RET(w32_heap_alloc(W, 1024, 1)); }   /* __lc_time_data: opaque to the STL, passed back to _Strftime */
/* _Strftime(buf, max, fmt, tm, lc_time_data) / _Wcsftime(wbuf, max, wfmt, tm, lc_time_data) */
static void s_Strftime(Cpu *c) { s_strftime(c); }
static void s_Wcsftime(Cpu *c) { char f[512]; wstr_to_utf8(c, ARG(2), f, sizeof f, -1); char out[1024]; int n = fmt_tm(c, out, sizeof out, f, ARG(3)); if ((u64)n + 1 > ARG(1)) { RET(0); return; } for (int i = 0; i <= n; i++) wr16(c, ARG(0) + (u64)i * 2, (u8)out[i]); RET((u64)n); }
/* vcruntime internals */
static void s_uncaught_exceptions(Cpu *c) { RET(0); }
/* __AdjustPointer(void *this, const PMD *pmd): this + mdisp, plus the virtual-base displacement when pdisp >= 0 */
static void s_AdjustPointer(Cpu *c) { u64 p = ARG(0), pmd = ARG(1); if (!p) { RET(0); return; } s32 mdisp = (s32)rd32(c, pmd), pdisp = (s32)rd32(c, pmd + 4), vdisp = (s32)rd32(c, pmd + 8);
  u64 r = p + (s64)mdisp; if (pdisp >= 0) { u64 vbtable = rdp(c, p + (s64)pdisp); r += (s64)(s32)rd32(c, vbtable + (s64)vdisp); } RET(r); }
/* kernel32 odds and ends imported by the runtime DLLs */
static void s_FlushProcessWriteBuffers(Cpu *c) { RET(0); }
/* RegisterWindowMessage(A/W): a stable id in the 0xC000..0xFFFF range derived from the name (MFC registers ~10 at start-up) */
/* win.ini profile API (MFC start-up reads a few ints): no win.ini → defaults */
/* LocalReAlloc / GlobalReAlloc(hMem, bytes, flags): LMEM_ZEROINIT 0x40, LMEM_MOVEABLE 0x02 (our handles are pointers) */
static void s_LocalReAlloc(Cpu *c) { RET(heap_realloc(W, ARG(0), ARG(1), !!(ARG(2) & 0x40))); }
/* _mbs* multibyte string family (ANSI MFC): single-byte semantics — the emulated ANSI code page is 1252/UTF-8 without lead bytes */
static void s_strncat_s(Cpu *c) { const char *d = gstr(c, ARG(0)); size_t dl = e_strlen(d); const char *s = gstr(c, ARG(2)); size_t l = e_strlen(s); u64 n = ARG(3); if (n != ~0ull && l > n) l = n; if (dl + l + 1 > ARG(1)) { RET(34); return; } u8 *p = mem_ptr(c->mem, ARG(0) + dl, l + 1, 1); if (p) { e_memmove(p, s, l); p[l] = 0; } RET(0); }
static void s_mbsinc(Cpu *c) { RET(ARG(0) + (rd8(c, ARG(0)) ? 1 : 0)); }
static void s_mbsdec(Cpu *c) { RET(ARG(1) > ARG(0) ? ARG(1) - 1 : 0); }
static void s_mbsnbcnt(Cpu *c) { u64 n = 0, m = ARG(1); while (n < m && rd8(c, ARG(0) + n)) n++; RET(n); }
static void s_mbclen(Cpu *c) { RET(1); }
static void s_ismbblead(Cpu *c) { RET(0); }
static void s_ismbbtrail(Cpu *c) { RET(0); }
static void s_mbsnbcmp(Cpu *c) { s_strncmp(c); }
static void s_mbsnbicmp(Cpu *c) { s_strnicmp(c); }
static void s_mbsnbcpy_s(Cpu *c) { s_strncpy_s(c); }
static void s_mbsnbcat_s(Cpu *c) { s_strncat_s(c); }
static void s_mbsnbcpy(Cpu *c) { u64 n = ARG(2), i = 0; for (; i < n; i++) { u8 ch = rd8(c, ARG(1) + i); wr8(c, ARG(0) + i, ch); if (!ch) break; } RET(ARG(0)); }
static void s_mbsnbcat(Cpu *c) { const char *d = gstr(c, ARG(0)); size_t dl = e_strlen(d); u64 n = ARG(2), i = 0; for (; i < n; i++) { u8 ch = rd8(c, ARG(1) + i); wr8(c, ARG(0) + dl + i, ch); if (!ch) break; } if (i == n) wr8(c, ARG(0) + dl + n, 0); RET(ARG(0)); }
static void s_mbsspn(Cpu *c) { const char *str = gstr(c, ARG(0)), *set = gstr(c, ARG(1)); u64 n = 0; for (; str[n]; n++) { int in = 0; for (const char *q = set; *q; q++) if (*q == str[n]) { in = 1; break; } if (!in) break; } RET(n); }
static void s_mbscspn(Cpu *c) { const char *str = gstr(c, ARG(0)), *set = gstr(c, ARG(1)); u64 n = 0; for (; str[n]; n++) { int in = 0; for (const char *q = set; *q; q++) if (*q == str[n]) { in = 1; break; } if (in) break; } RET(n); }
static void s_mbspbrk(Cpu *c) { s_mbscspn(c); u64 n = c->r[RAX]; RET(rd8(c, ARG(0) + n) ? ARG(0) + n : 0); }
static void s_mbsrev(Cpu *c) { u64 a = ARG(0); const char *str = gstr(c, a); size_t n = e_strlen(str); for (size_t i = 0; i < n / 2; i++) { u8 x = rd8(c, a + i), y = rd8(c, a + n - 1 - i); wr8(c, a + i, y); wr8(c, a + n - 1 - i, x); } RET(a); }
static void s_mbstok_s(Cpu *c) { cpu_fault(c, "_mbstok_s: not implemented"); }
/* SHLWAPI path helpers on UTF-16 strings (pointers into the caller's buffer) */
static u32 wlen16(Cpu *c, u64 a) { u32 n = 0; while (rd16(c, a + n * 2) && n < 32768) n++; return n; }
static void s_PathFindExtensionW(Cpu *c) { u64 a = ARG(0); u32 n = wlen16(c, a); u64 r = a + n * 2; for (u32 i = n; i-- > 0;) { u16 ch = rd16(c, a + i * 2); if (ch == '.') { r = a + i * 2; break; } if (ch == '\\' || ch == '/' || ch == ':') break; } RET(r); }
static void s_PathFindFileNameW(Cpu *c) { u64 a = ARG(0); u32 n = wlen16(c, a); u64 r = a; for (u32 i = 0; i + 1 < n; i++) { u16 ch = rd16(c, a + i * 2); if (ch == '\\' || ch == '/' || ch == ':') r = a + (i + 1) * 2; } RET(r); }
static void s_PathRemoveExtensionW(Cpu *c) { s_PathFindExtensionW(c); wr16(c, c->r[RAX], 0); }
static void s_PathIsUNCW(Cpu *c) { u64 a = ARG(0); RET(rd16(c, a) == '\\' && rd16(c, a + 2) == '\\' ? 1 : 0); }
static void s_PathStripPathW(Cpu *c) { u64 a = ARG(0); s_PathFindFileNameW(c); u64 f = c->r[RAX]; if (f != a) { u32 n = wlen16(c, f); for (u32 i = 0; i <= n; i++) wr16(c, a + i * 2, rd16(c, f + i * 2)); } }
static void s_PathAddBackslashW(Cpu *c) { u64 a = ARG(0); u32 n = wlen16(c, a); if (n && rd16(c, a + (n - 1) * 2) != '\\') { wr16(c, a + n * 2, '\\'); wr16(c, a + (n + 1) * 2, 0); n++; } RET(a + n * 2); }
static void s_PathRemoveBackslashW(Cpu *c) { u64 a = ARG(0); u32 n = wlen16(c, a); if (n > 1 && rd16(c, a + (n - 1) * 2) == '\\') { wr16(c, a + (n - 1) * 2, 0); n--; } RET(a + n * 2); }
static void s_PathRemoveFileSpecW(Cpu *c) { u64 a = ARG(0); s_PathFindFileNameW(c); u64 f = c->r[RAX]; if (f == a) { RET(0); return; } u64 cut = f - 2; if (cut > a + 2 && rd16(c, cut) == '\\' && rd16(c, cut - 2) != ':') wr16(c, cut, 0); else wr16(c, f, 0); RET(1); }
static void s_PathFileExistsW(Cpu *c) { RET(vfs_stat(name_w(c, ARG(0)), 0) != 0); }
static void s_PathIsDirectoryW(Cpu *c) { RET(vfs_stat(name_w(c, ARG(0)), 0) == 2 ? 0x10 : 0); }
/* ANSI twins */
static void s_PathFindExtensionA(Cpu *c) { u64 a = ARG(0); const char *str = gstr(c, a); size_t n = e_strlen(str); u64 r = a + n; for (size_t i = n; i-- > 0;) { char ch = str[i]; if (ch == '.') { r = a + i; break; } if (ch == '\\' || ch == '/' || ch == ':') break; } RET(r); }
static void s_PathFindFileNameA(Cpu *c) { u64 a = ARG(0); const char *str = gstr(c, a); size_t n = e_strlen(str); u64 r = a; for (size_t i = 0; i + 1 < n; i++) { char ch = str[i]; if (ch == '\\' || ch == '/' || ch == ':') r = a + i + 1; } RET(r); }
static void s_PathRemoveExtensionA(Cpu *c) { s_PathFindExtensionA(c); wr8(c, c->r[RAX], 0); }
static void s_PathIsUNCA(Cpu *c) { u64 a = ARG(0); RET(rd8(c, a) == '\\' && rd8(c, a + 1) == '\\' ? 1 : 0); }
static void s_PathStripPathA(Cpu *c) { u64 a = ARG(0); s_PathFindFileNameA(c); u64 f = c->r[RAX]; if (f != a) { const char *str = gstr(c, f); size_t n = e_strlen(str); for (size_t i = 0; i <= n; i++) wr8(c, a + i, (u8)str[i]); } }
static void s_PathFileExistsA(Cpu *c) { RET(vfs_stat(name_a(c, ARG(0)), 0) != 0); }
static void s_StrCmpIW(Cpu *c) { u64 a = ARG(0), b = ARG(1); for (u32 i = 0;; i++) { u16 x = rd16(c, a + i * 2), y = rd16(c, b + i * 2); if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) { RET((u64)(s64)((int)x - (int)y)); return; } if (!x) { RET(0); return; } } }
static void s_StrCmpNIW(Cpu *c) { u64 a = ARG(0), b = ARG(1); u32 n = (u32)ARG(2); for (u32 i = 0; i < n; i++) { u16 x = rd16(c, a + i * 2), y = rd16(c, b + i * 2); if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) { RET((u64)(s64)((int)x - (int)y)); return; } if (!x) break; } RET(0); }
static void s_RegisterWindowMessageW(Cpu *c) { const char *n = name_w(c, ARG(0)); u32 h = 2166136261u; for (const char *p = n; *p; p++) h = (h ^ (u8)*p) * 16777619u; RET(0xC000u + (h % 0x3FFFu)); }
static void s_RegisterWindowMessageA(Cpu *c) { const char *n = name_a(c, ARG(0)); u32 h = 2166136261u; for (const char *p = n; *p; p++) h = (h ^ (u8)*p) * 16777619u; RET(0xC000u + (h % 0x3FFFu)); }
static void s_GetStringTypeExW(Cpu *c) { call_shim(c, s_GetStringTypeW, ARG(1), ARG(2), ARG(3), ARG(4)); }   /* (locale, type, src, n, out) → GetStringTypeW(type, src, n, out) */
static void s_GetThreadPreferredUILanguages(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); if (ARG(3)) { u32 need = rd32(c, ARG(3)); if (ARG(2) && need >= 1) wr16(c, ARG(2), 0); wr32(c, ARG(3), 1); } RET(1); }
#define RT_SHIMS \
  SA("_lock_locales", lock_locales), SA("_unlock_locales", lock_locales), SA("___lc_locale_name_func", lc_locale_name_func), SA("__processing_throw", processing_throw), SA("_invoke_watson", invoke_watson), \
  SA("__strncnt", strncnt), SA("_wcsdup", wcsdup), S(iswxdigit), S(iswalnum), S(btowc), S(strtof), S(fgetwc), S(fputwc), S(ungetwc), \
  SA("_Getdays", Getdays), SA("_Getmonths", Getmonths), SA("_W_Getdays", W_Getdays), SA("_W_Getmonths", W_Getmonths), SA("_Gettnames", Gettnames), S(fetestexcept), S(feclearexcept), S(feraiseexcept), S(fegetexceptflag), S(fesetexceptflag), S(fegetround), S(fesetround), S(fegetenv), S(fesetenv), S(feholdexcept), S(feupdateenv), SA("_statusfp", statusfp), SA("_clearfp", clearfp), SA("_statusfp2", statusfp2), SA("_get_stream_buffer_pointers", get_stream_buffer_pointers), SA("_W_Gettnames", Gettnames), SA("_Strftime", Strftime), SA("_Wcsftime", Wcsftime), SA("_ldclass", dclass), \
  SA("__uncaught_exceptions", uncaught_exceptions), SA("__uncaught_exception", uncaught_exceptions), SA("__std_terminate", cxx_terminate), SA("__AdjustPointer", AdjustPointer), \
  S(FlushProcessWriteBuffers), S(RegisterWindowMessageW), S(RegisterWindowMessageA), SA("RegisterClipboardFormatW", RegisterWindowMessageW), SA("RegisterClipboardFormatA", RegisterWindowMessageA), S(LocalReAlloc), SA("GlobalReAlloc", LocalReAlloc), S(strncat_s), SA("_mbsinc", mbsinc), SA("_mbsdec", mbsdec), SA("_mbsnbcnt", mbsnbcnt), SA("_mbsnccnt", mbsnbcnt), SA("_mbclen", mbclen), SA("_ismbblead", ismbblead), SA("_ismbbtrail", ismbbtrail), SA("_ismbslead", ismbblead), SA("_ismbstrail", ismbblead), SA("_mbsnbcmp", mbsnbcmp), SA("_mbsnbicmp", mbsnbicmp), SA("_mbsncmp", mbsnbcmp), SA("_mbsnicmp", mbsnbicmp), SA("_mbsnbcpy_s", mbsnbcpy_s), SA("_mbsnbcat_s", mbsnbcat_s), SA("_mbsncpy_s", mbsnbcpy_s), SA("_mbsnbcpy", mbsnbcpy), SA("_mbsnbcat", mbsnbcat), SA("_mbsncpy", mbsnbcpy), SA("_mbsspn", mbsspn), SA("_mbscspn", mbscspn), SA("_mbspbrk", mbspbrk), SA("_mbsrev", mbsrev), SA("_mbstok_s", mbstok_s), SA("_mbscmp", strcmp), SA("_mbsicmp", stricmp), SA("_mbslen", strlen), SA("_mbschr", strchr), SA("_mbsrchr", strrchr), SA("_mbsstr", strstr), SA("_mbscpy_s", strcpy_s), SA("_mbscat_s", strcat_s), SA("_mbslwr", strlwr), SA("_mbsupr", strupr), SA("_mbslwr_s", ret0), SA("_mbsupr_s", ret0), SA("_mbsnlen", strnlen), SA("_mbsnbset_s", ret0), SA("_mbsnset_s", ret0), S(PathFindExtensionA), S(PathFindFileNameA), S(PathRemoveExtensionA), S(PathIsUNCA), S(PathStripPathA), S(PathFileExistsA), S(PathFindExtensionW), S(PathFindFileNameW), S(PathRemoveExtensionW), S(PathIsUNCW), S(PathStripPathW), S(PathAddBackslashW), S(PathRemoveBackslashW), S(PathRemoveFileSpecW), S(PathFileExistsW), S(PathIsDirectoryW), S(StrCmpIW), S(StrCmpNIW), SA("StrCmpW", wcscmp), SA("_SetWinRTOutOfMemoryExceptionCallback", ret0), S(GetStringTypeExW), S(GetThreadPreferredUILanguages), SA("SetThreadPreferredUILanguages", ret1),
