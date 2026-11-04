/* APIs the Delphi RTL / Inno Setup bootstrap use at start-up: SAFEARRAY (oleaut32), VariantChangeType for the simple
   numeric/string cases, locale & volume queries, SID → string, CharUpperBuffW/CharLowerBuffW. 32-bit and 64-bit layouts. */
/* SAFEARRAY: cDims u16 @0, fFeatures u16 @2, cbElements u32 @4, cLocks u32 @8, pvData ptr @(x86 12 / x64 16), rgsabound[] after (cElements u32, lLbound s32) */
#define SA_PVDATA(c) ((c)->mode32 ? 12u : 16u)
#define SA_BOUNDS(c) ((c)->mode32 ? 16u : 24u)
static u32 vt_elem_size(u32 vt) { switch (vt & 0xfff) { case 2: case 11: case 18: return 2; case 3: case 4: case 19: case 22: case 23: case 10: return 4; case 5: case 6: case 7: case 20: case 21: return 8; case 16: case 17: return 1; case 8: case 9: case 13: return 0xFFFF; /* pointer-sized */ case 12: return 0xFFFE; /* VARIANT */ case 14: return 16; default: return 4; } }
static u32 sa_elem_bytes(Cpu *c, u32 vt) { u32 s = vt_elem_size(vt); return s == 0xFFFF ? (u32)PSZ : s == 0xFFFE ? VARSZ(c) : s; }
static void s_SafeArrayCreate(Cpu *c) { u32 vt = (u32)ARG(0), dims = (u32)ARG(1); u64 bounds = ARG(2); if (!dims || dims > 16) { RET(0); return; }
  u32 es = sa_elem_bytes(c, vt); u64 total = 1; for (u32 d = 0; d < dims; d++) total *= rd32(c, bounds + d * 8); if (total > (64u << 20)) { RET(0); return; }
  u64 sa = w32_heap_alloc(W, SA_BOUNDS(c) + dims * 8, 1); if (!sa) { RET(0); return; } u64 data = w32_heap_alloc(W, total * es + 16, 1); if (!data) { RET(0); return; }
  u8 *p = mem_ptr(c->mem, data, total * es + 16, 1); if (p) e_memset(p, 0, (size_t)(total * es + 16));
  wr16(c, sa, (u16)dims); wr16(c, sa + 2, vt == 8 ? 0x100 /* FADF_BSTR */ : vt == 12 ? 0x800 /* FADF_VARIANT */ : 0); wr32(c, sa + 4, es); wr32(c, sa + 8, 0); wrp(c, sa + SA_PVDATA(c), data);
  /* rgsabound is stored with the LAST dimension first (Windows order): the caller gives rgsabound[0] = leftmost */
  for (u32 d = 0; d < dims; d++) { wr32(c, sa + SA_BOUNDS(c) + d * 8, rd32(c, bounds + (dims - 1 - d) * 8)); wr32(c, sa + SA_BOUNDS(c) + d * 8 + 4, rd32(c, bounds + (dims - 1 - d) * 8 + 4)); }
  RET(sa); }
static void s_SafeArrayCreateVector(Cpu *c) { u32 vt = (u32)ARG(0); s32 lb = (s32)ARG(1); u32 n = (u32)ARG(2); u64 b = w32_heap_alloc(W, 8, 1); wr32(c, b, n); wr32(c, b + 4, (u32)lb); g_a32_n = 0; /* re-dispatch through SafeArrayCreate(vt, 1, &bound) */ call_shim(c, s_SafeArrayCreate, vt, 1, b, 0); w32_heap_free(W, b); }
static void s_SafeArrayDestroy(Cpu *c) { u64 sa = ARG(0); if (!sa) { RET(0); return; } if (rd32(c, sa + 8)) { RET(0x8002000D); return; } u64 data = rdp(c, sa + SA_PVDATA(c)); if (data) w32_heap_free(W, data); w32_heap_free(W, sa); RET(0); }
static void s_SafeArrayGetDim(Cpu *c) { RET(rd16(c, ARG(0))); }
static void s_SafeArrayGetElemsize(Cpu *c) { RET(rd32(c, ARG(0) + 4)); }
static void s_SafeArrayGetLBound(Cpu *c) { u64 sa = ARG(0); u32 dim = (u32)ARG(1), dims = rd16(c, sa); if (!dim || dim > dims) { RET(0x8002000B); return; } wr32(c, ARG(2), rd32(c, sa + SA_BOUNDS(c) + (dims - dim) * 8 + 4)); RET(0); }
static void s_SafeArrayGetUBound(Cpu *c) { u64 sa = ARG(0); u32 dim = (u32)ARG(1), dims = rd16(c, sa); if (!dim || dim > dims) { RET(0x8002000B); return; } u64 b = sa + SA_BOUNDS(c) + (dims - dim) * 8; wr32(c, ARG(2), (u32)((s32)rd32(c, b + 4) + (s32)rd32(c, b) - 1)); RET(0); }
static void s_SafeArrayAccessData(Cpu *c) { u64 sa = ARG(0); wr32(c, sa + 8, rd32(c, sa + 8) + 1); wrp(c, ARG(1), rdp(c, sa + SA_PVDATA(c))); RET(0); }
static void s_SafeArrayUnaccessData(Cpu *c) { u64 sa = ARG(0); u32 l = rd32(c, sa + 8); if (l) wr32(c, sa + 8, l - 1); RET(0); }
static void s_SafeArrayLock(Cpu *c) { u64 sa = ARG(0); wr32(c, sa + 8, rd32(c, sa + 8) + 1); RET(0); }
static void s_SafeArrayUnlock(Cpu *c) { u64 sa = ARG(0); u32 l = rd32(c, sa + 8); if (l) wr32(c, sa + 8, l - 1); RET(0); }
/* element address from indices (rightmost index varies fastest in the stored order) */
static int sa_elem_addr(Cpu *c, u64 sa, u64 idx, u64 *out) { u32 dims = rd16(c, sa), es = rd32(c, sa + 4); u64 off = 0, stride = es;
  for (u32 d = 0; d < dims; d++) { u64 b = sa + SA_BOUNDS(c) + d * 8; u32 n = rd32(c, b); s32 lb = (s32)rd32(c, b + 4); s32 i = (s32)rd32(c, idx + (dims - 1 - d) * 4) - lb; if (i < 0 || (u32)i >= n) return 0; off += (u64)(u32)i * stride; stride *= n; }
  *out = rdp(c, sa + SA_PVDATA(c)) + off; return 1; }
static void s_SafeArrayPtrOfIndex(Cpu *c) { u64 a; if (!sa_elem_addr(c, ARG(0), ARG(1), &a)) { RET(0x8002000B); return; } wrp(c, ARG(2), a); RET(0); }
static void sa_copy_elem(Cpu *c, u64 sa, u64 dst, u64 src, int in) { u16 f = rd16(c, sa + 2); u32 es = rd32(c, sa + 4);
  if (f & 0x100) { /* BSTR: copy the string */ u64 s = in ? src : rdp(c, src); u64 b = 0; if (s) { u32 n = rd32(c, s - 4) / 2; b = bstr_alloc(c, 0, s, n); } if (in) { u64 old = rdp(c, dst); if (old) w32_heap_free(W, old - 4); wrp(c, dst, b); } else wrp(c, dst, b); return; }
  if (f & 0x800) { /* VARIANT: VariantCopy */ if (in) call_shim(c, s_VariantCopy, dst, src, 0, 0); else call_shim(c, s_VariantCopy, dst, src, 0, 0); return; }
  if (f & 0x200 /* UNKNOWN */ || f & 0x400 /* DISPATCH */) { wrp(c, dst, rdp(c, src)); return; }
  for (u32 i = 0; i < es; i++) wr8(c, dst + i, rd8(c, src + i)); }
static void s_SafeArrayPutElement(Cpu *c) { u64 a; u64 sa = ARG(0); if (!sa_elem_addr(c, sa, ARG(1), &a)) { RET(0x8002000B); return; } u16 f = rd16(c, sa + 2); u64 src = ARG(2);
  if (f & 0x100) { u64 b = 0; if (src) b = bstr_alloc(c, 0, src, rd32(c, src - 4) / 2); u64 old = rdp(c, a); if (old) w32_heap_free(W, old - 4); wrp(c, a, b); RET(0); return; }
  if (f & 0x800) { call_shim(c, s_VariantCopy, a, src, 0, 0); RET(0); return; }
  if (f & 0x600) { wrp(c, a, src); RET(0); return; }
  u32 es = rd32(c, sa + 4); for (u32 i = 0; i < es; i++) wr8(c, a + i, rd8(c, src + i)); RET(0); }
static void s_SafeArrayGetElement(Cpu *c) { u64 a; u64 sa = ARG(0); if (!sa_elem_addr(c, sa, ARG(1), &a)) { RET(0x8002000B); return; } u16 f = rd16(c, sa + 2); u64 dst = ARG(2);
  if (f & 0x100) { u64 s = rdp(c, a); wrp(c, dst, s ? bstr_alloc(c, 0, s, rd32(c, s - 4) / 2) : 0); RET(0); return; }
  if (f & 0x800) { call_shim(c, s_VariantCopy, dst, a, 0, 0); RET(0); return; }
  u32 es = rd32(c, sa + 4); for (u32 i = 0; i < es; i++) wr8(c, dst + i, rd8(c, a + i)); RET(0); }
static void s_SafeArrayCopy(Cpu *c) { u64 sa = ARG(0); u32 dims = rd16(c, sa), es = rd32(c, sa + 4); u64 total = 1; for (u32 d = 0; d < dims; d++) total *= rd32(c, sa + SA_BOUNDS(c) + d * 8);
  u64 n = w32_heap_alloc(W, SA_BOUNDS(c) + dims * 8, 1); u64 data = w32_heap_alloc(W, total * es + 16, 1); if (!n || !data) { RET(0x8007000E); return; }
  for (u32 i = 0; i < SA_BOUNDS(c) + dims * 8; i++) wr8(c, n + i, rd8(c, sa + i)); wr32(c, n + 8, 0); wrp(c, n + SA_PVDATA(c), data);
  u64 src = rdp(c, sa + SA_PVDATA(c)); u16 f = rd16(c, sa + 2); if (f & 0x900) { u8 *p = mem_ptr(c->mem, data, total * es, 1); if (p) e_memset(p, 0, (size_t)(total * es)); for (u64 i = 0; i < total; i++) sa_copy_elem(c, sa, data + i * es, src + i * es, 0); } else for (u64 i = 0; i < total * es; i++) wr8(c, data + i, rd8(c, src + i));
  wrp(c, ARG(1), n); RET(0); }
static void s_SafeArrayRedim(Cpu *c) { u64 sa = ARG(0), nb = ARG(1); u32 dims = rd16(c, sa), es = rd32(c, sa + 4); u64 b0 = sa + SA_BOUNDS(c); /* the leftmost dimension (stored last) changes */ u64 old_total = 1, new_total = 1; for (u32 d = 0; d < dims; d++) old_total *= rd32(c, b0 + d * 8);
  u32 nn = rd32(c, nb); s32 nl = (s32)rd32(c, nb + 4); u32 on = rd32(c, b0 + (dims - 1) * 8); new_total = old_total / (on ? on : 1) * nn;
  u64 data = w32_heap_alloc(W, new_total * es + 16, 1); if (!data) { RET(0x8007000E); return; } u8 *p = mem_ptr(c->mem, data, new_total * es + 16, 1); if (p) e_memset(p, 0, (size_t)(new_total * es + 16));
  u64 src = rdp(c, sa + SA_PVDATA(c)); u64 keep = (old_total < new_total ? old_total : new_total) * es; for (u64 i = 0; i < keep; i++) wr8(c, data + i, rd8(c, src + i)); if (src) w32_heap_free(W, src);
  wrp(c, sa + SA_PVDATA(c), data); wr32(c, b0 + (dims - 1) * 8, nn); wr32(c, b0 + (dims - 1) * 8 + 4, (u32)nl); RET(0); }
static void s_SafeArrayGetVartype(Cpu *c) { u16 f = rd16(c, ARG(0) + 2); u32 es = rd32(c, ARG(0) + 4); u32 vt = (f & 0x100) ? 8 : (f & 0x800) ? 12 : (f & 0x200) ? 13 : (f & 0x400) ? 9 : es == 1 ? 17 : es == 2 ? 2 : es == 8 ? 5 : 3; wr16(c, ARG(1), (u16)vt); RET(0); }
static void s_SafeArrayAllocDescriptorEx(Cpu *c) { u32 dims = (u32)ARG(1); u64 sa = w32_heap_alloc(W, SA_BOUNDS(c) + dims * 8, 1); if (!sa) { RET(0x8007000E); return; } wr16(c, sa, (u16)dims); wr32(c, sa + 4, sa_elem_bytes(c, (u32)ARG(0))); wrp(c, ARG(2), sa); RET(0); }
static void s_SafeArrayAllocDescriptor(Cpu *c) { u32 dims = (u32)ARG(0); u64 sa = w32_heap_alloc(W, SA_BOUNDS(c) + dims * 8, 1); if (!sa) { RET(0x8007000E); return; } wr16(c, sa, (u16)dims); wrp(c, ARG(1), sa); RET(0); }
static void s_SafeArrayAllocData(Cpu *c) { u64 sa = ARG(0); u32 dims = rd16(c, sa), es = rd32(c, sa + 4); u64 total = 1; for (u32 d = 0; d < dims; d++) total *= rd32(c, sa + SA_BOUNDS(c) + d * 8); u64 data = w32_heap_alloc(W, total * es + 16, 1); if (!data) { RET(0x8007000E); return; } u8 *p = mem_ptr(c->mem, data, total * es + 16, 1); if (p) e_memset(p, 0, (size_t)(total * es + 16)); wrp(c, sa + SA_PVDATA(c), data); RET(0); }
static void s_SafeArrayDestroyData(Cpu *c) { u64 sa = ARG(0); u64 d = rdp(c, sa + SA_PVDATA(c)); if (d) w32_heap_free(W, d); wrp(c, sa + SA_PVDATA(c), 0); RET(0); }
static void s_SafeArrayDestroyDescriptor(Cpu *c) { if (ARG(0)) w32_heap_free(W, ARG(0)); RET(0); }

/* VariantChangeType(dst, src, flags, vt): integers ↔ doubles ↔ bool ↔ BSTR (decimal), enough for Delphi's VarAsType / string conversions */
static int var_as_f64(Cpu *c, u64 v, double *out, int *is_str) { u16 vt = rd16(c, v); u64 u = v + 8; *is_str = 0;
  switch (vt) { case 0: case 1: *out = 0; return 1; case 2: *out = (s16)rd16(c, u); return 1; case 3: case 22: *out = (s32)rd32(c, u); return 1; case 4: { u32 b = rd32(c, u); float f; e_memcpy(&f, &b, 4); *out = f; return 1; } case 5: case 7: { u64 b = rd64(c, u); double d; e_memcpy(&d, &b, 8); *out = d; return 1; }
    case 11: *out = (s16)rd16(c, u) ? -1 : 0; return 1; case 16: *out = (s8)rd8(c, u); return 1; case 17: *out = rd8(c, u); return 1; case 18: *out = rd16(c, u); return 1; case 19: case 23: *out = rd32(c, u); return 1; case 20: *out = (double)(s64)rd64(c, u); return 1; case 21: *out = (double)rd64(c, u); return 1;
    case 8: { *is_str = 1; u64 s = rdp(c, u); char buf[64]; int n = 0; if (s) for (; n < 63; n++) { u16 ch = rd16(c, s + n * 2); if (!ch) break; buf[n] = (char)(ch < 128 ? ch : '?'); } buf[n] = 0; *out = e_strtod(buf, 0); return 1; }
    default: return 0; } }
static void s_VariantChangeType(Cpu *c) { u64 dst = ARG(0), src = ARG(1); u32 vt = (u32)ARG(3) & 0xffff; double d; int is_str; u16 svt = rd16(c, src);
  if (svt == vt) { if (dst != src) call_shim(c, s_VariantCopy, dst, src, 0, 0); RET(0); return; }
  if (!var_as_f64(c, src, &d, &is_str)) { RET(0x80020005); return; }   /* DISP_E_TYPEMISMATCH */
  u8 tmp[24]; e_memset(tmp, 0, sizeof tmp); u64 u = 8; s64 i = (s64)(d < 0 ? d - 0.5 : d + 0.5);
  switch (vt) { case 2: *(s16 *)(tmp + u) = (s16)i; break; case 3: case 22: *(s32 *)(tmp + u) = (s32)i; break; case 4: { float f = (float)d; e_memcpy(tmp + u, &f, 4); break; } case 5: case 7: e_memcpy(tmp + u, &d, 8); break; case 11: *(s16 *)(tmp + u) = d != 0 ? -1 : 0; break; case 16: tmp[u] = (u8)(s8)i; break; case 17: tmp[u] = (u8)i; break; case 18: *(u16 *)(tmp + u) = (u16)i; break; case 19: case 23: *(u32 *)(tmp + u) = (u32)i; break; case 20: *(s64 *)(tmp + u) = (s64)d; break; case 21: *(u64 *)(tmp + u) = (u64)d; break;
    case 8: { char buf[64]; if (svt == 11) e_snprintf(buf, sizeof buf, "%s", d != 0 ? "True" : "False"); else if (d == (double)(s64)d && d > -1e18 && d < 1e18) e_snprintf(buf, sizeof buf, "%lld", (long long)d); else e_snprintf(buf, sizeof buf, "%g", d);
      u16 w16[64]; int n = 0; for (; buf[n]; n++) w16[n] = (u8)buf[n]; u64 tmpw = w32_heap_alloc(W, (u64)n * 2 + 2, 1); for (int k = 0; k <= n; k++) wr16(c, tmpw + k * 2, k < n ? w16[k] : 0); u64 b = bstr_alloc(c, 0, tmpw, (u32)n); w32_heap_free(W, tmpw);
      if (dst != src) call_shim(c, s_VariantClear, dst, 0, 0, 0); else call_shim(c, s_VariantClear, dst, 0, 0, 0); wr16(c, dst, 8); wrp(c, dst + 8, b); RET(0); return; }
    default: RET(0x80020005); return; }
  call_shim(c, s_VariantClear, dst, 0, 0, 0); u8 *p = mem_ptr(c->mem, dst, VARSZ(c), 1); if (p) { e_memcpy(p, tmp, VARSZ(c)); } wr16(c, dst, (u16)vt); RET(0); }
static void s_VariantChangeTypeEx(Cpu *c) { g_a32_n = 0; call_shim(c, s_VariantChangeType, ARG(0), ARG(1), ARG(3), ARG(4)); }

/* locale / system */
static void s_SetThreadLocale(Cpu *c) { RET(1); }
static void s_GetSystemDefaultUILanguage(Cpu *c) { RET(0x409); }
static void s_GetThreadUILanguage(Cpu *c) { RET(0x409); }
static void s_SetThreadUILanguage(Cpu *c) { RET(0x409); }
static void s_EnumCalendarInfoW(Cpu *c) { RET(1); }   /* no callback invocations: the Gregorian defaults stay */
static void s_EnumCalendarInfoA(Cpu *c) { RET(1); }
static void s_EnumCalendarInfoExW(Cpu *c) { RET(1); }
static void s_GetSystemWindowsDirectoryW(Cpu *c) { const char *d = "C:\\Windows"; size_t l = e_strlen(d); if ((u32)ARG(1) > l) for (size_t i = 0; i <= l; i++) wr16(c, ARG(0) + i * 2, (u8)d[i]); RET(l); }
static void s_GetSystemWindowsDirectoryA(Cpu *c) { const char *d = "C:\\Windows"; size_t l = e_strlen(d); if ((u32)ARG(1) > l) { u8 *p = mem_ptr(c->mem, ARG(0), l + 1, 1); if (p) e_memcpy(p, d, l + 1); } RET(l); }
/* GetVolumeInformation(root, name, namecap, serial*, maxcomp*, flags*, fsname, fscap): an NTFS volume "Nebula" */
static void vol_info(Cpu *c, int wide) { u64 name = ARG(1), ncap = ARG(2), ser = ARG(3), maxc = ARG(4), flags = ARG(5), fs = ARG(6), fcap = ARG(7); const char *vn = "Nebula", *fn = "NTFS";
  if (name && ncap) { size_t l = e_strlen(vn); if (l + 1 > ncap) l = (size_t)ncap - 1; for (size_t i = 0; i <= l; i++) { u8 ch = i < l ? (u8)vn[i] : 0; if (wide) wr16(c, name + i * 2, ch); else wr8(c, name + i, ch); } }
  if (ser) wr32(c, ser, 0x4E454255); if (maxc) wr32(c, maxc, 255); if (flags) wr32(c, flags, 0x00020000 | 0x4 | 0x2 | 0x1 | 0x8 | 0x40 /* NTFS-like: unicode, case preserved, ACLs, compression, named streams */);
  if (fs && fcap) { size_t l = e_strlen(fn); if (l + 1 > fcap) l = (size_t)fcap - 1; for (size_t i = 0; i <= l; i++) { u8 ch = i < l ? (u8)fn[i] : 0; if (wide) wr16(c, fs + i * 2, ch); else wr8(c, fs + i, ch); } }
  RET(1); }
static void s_GetVolumeInformationW(Cpu *c) { vol_info(c, 1); }
static void s_GetVolumeInformationA(Cpu *c) { vol_info(c, 0); }
static void s_GetVolumeInformationByHandleW(Cpu *c) { vol_info(c, 1); }
/* ConvertSidToStringSid: S-1-<auth>-<sub...> as the string the SID encodes */
static void sid_to_string(Cpu *c, int wide) { u64 sid = ARG(0), out = ARG(1); if (!sid || !out) { W->last_error = 87; RET(0); return; } u8 rev = rd8(c, sid), n = rd8(c, sid + 1); u64 auth = 0; for (int i = 0; i < 6; i++) auth = (auth << 8) | rd8(c, sid + 2 + i);
  char buf[256]; int o = e_snprintf(buf, sizeof buf, "S-%u-%llu", rev, (unsigned long long)auth); for (int i = 0; i < n && i < 15 && o < 240; i++) o += e_snprintf(buf + o, sizeof buf - o, "-%u", rd32(c, sid + 8 + i * 4));
  u64 s = wide ? guest_wstrdup(W, buf) : w32_guest_strdup(W, buf); wrp(c, out, s); RET(1); }
static void s_ConvertSidToStringSidW(Cpu *c) { sid_to_string(c, 1); }
static void s_ConvertSidToStringSidA(Cpu *c) { sid_to_string(c, 0); }
/* CharUpperBuffW / CharLowerBuffW: in place, n UTF-16 units; ASCII + Latin-1 + Cyrillic */
static u16 wch_upper(u16 ch) { if (ch >= 'a' && ch <= 'z') return ch - 32; if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) return ch - 32; if (ch >= 0x430 && ch <= 0x44F) return ch - 32; if (ch >= 0x450 && ch <= 0x45F) return ch - 80; return ch; }
static u16 wch_lower(u16 ch) { if (ch >= 'A' && ch <= 'Z') return ch + 32; if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return ch + 32; if (ch >= 0x410 && ch <= 0x42F) return ch + 32; if (ch >= 0x400 && ch <= 0x40F) return ch + 80; return ch; }
static void s_CharUpperBuffW(Cpu *c) { u64 s = ARG(0); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) wr16(c, s + i * 2, wch_upper(rd16(c, s + i * 2))); RET(n); }
static void s_CharLowerBuffW(Cpu *c) { u64 s = ARG(0); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) wr16(c, s + i * 2, wch_lower(rd16(c, s + i * 2))); RET(n); }
static void s_CharUpperBuffA(Cpu *c) { u64 s = ARG(0); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) { u8 ch = rd8(c, s + i); if (ch >= 'a' && ch <= 'z') wr8(c, s + i, ch - 32); } RET(n); }
static void s_CharLowerBuffA(Cpu *c) { u64 s = ARG(0); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) { u8 ch = rd8(c, s + i); if (ch >= 'A' && ch <= 'Z') wr8(c, s + i, ch + 32); } RET(n); }
static void s_VariantCopyInd(Cpu *c) { u64 d = ARG(0), s = ARG(1); u16 vt = rd16(c, s); if (vt & 0x4000) { /* VT_BYREF: dereference */ u16 base = vt & 0x3fff; u64 p = rdp(c, s + 8); u8 tmp[24]; e_memset(tmp, 0, sizeof tmp); u32 sz = base == 8 || base == 9 || base == 13 ? (u32)PSZ : base == 5 || base == 7 || base == 20 || base == 21 ? 8 : base == 2 || base == 11 || base == 18 ? 2 : base == 16 || base == 17 ? 1 : 4; for (u32 i = 0; i < sz; i++) tmp[8 + i] = rd8(c, p + i); call_shim(c, s_VariantClear, d, 0, 0, 0); u8 *dp = mem_ptr(c->mem, d, VARSZ(c), 1); if (dp) e_memcpy(dp, tmp, VARSZ(c)); wr16(c, d, base); if (base == 8) { u64 b = rdp(c, d + 8); wrp(c, d + 8, b ? bstr_alloc(c, 0, b, rd32(c, b - 4) / 2) : 0); } RET(0); return; } call_shim_tail(c, s_VariantCopy, d, s, 0, 0); }
static void s_CheckTokenMembership(Cpu *c) { /* (token, sid, &IsMember): the emulated user is an administrator (installers must be able to write Program Files) */ if (ARG(2)) wr32(c, ARG(2), 1); RET(1); }
/* Restart Manager: a session that never finds any process holding files */
static void s_RmStartSession(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), 1); if (ARG(2)) { const char *k = "RmSession000000000000000000000001"; for (int i = 0; i <= 32; i++) wr16(c, ARG(2) + i * 2, (u8)k[i]); } RET(0); }
static void s_RmRegisterResources(Cpu *c) { RET(0); }
static void s_RmGetList(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); if (ARG(2)) wr32(c, ARG(2), 0); if (ARG(4)) wr32(c, ARG(4), 0); RET(0); }
static void s_RmShutdown(Cpu *c) { RET(0); }
static void s_RmRestart(Cpu *c) { RET(0); }
static void s_RmEndSession(Cpu *c) { RET(0); }
static void s_RmJoinSession(Cpu *c) { RET(0); }
static void s_RmCancelCurrentTask(Cpu *c) { RET(0); }
#define INNO_SHIMS S(RmStartSession), S(RmRegisterResources), S(RmGetList), S(RmShutdown), S(RmRestart), S(RmEndSession), S(RmJoinSession), S(RmCancelCurrentTask), S(VariantCopyInd), S(SafeArrayCreate), S(SafeArrayCreateVector), S(SafeArrayDestroy), S(SafeArrayGetDim), S(SafeArrayGetElemsize), S(SafeArrayGetLBound), S(SafeArrayGetUBound), S(SafeArrayAccessData), S(SafeArrayUnaccessData), \
  S(SafeArrayLock), S(SafeArrayUnlock), S(SafeArrayPtrOfIndex), S(SafeArrayPutElement), S(SafeArrayGetElement), S(SafeArrayCopy), S(SafeArrayRedim), S(SafeArrayGetVartype), S(SafeArrayAllocDescriptorEx), S(SafeArrayAllocDescriptor), S(SafeArrayAllocData), S(SafeArrayDestroyData), S(SafeArrayDestroyDescriptor), \
  S(VariantChangeType), S(VariantChangeTypeEx), S(SetThreadLocale), S(GetSystemDefaultUILanguage), S(GetThreadUILanguage), S(SetThreadUILanguage), S(EnumCalendarInfoW), S(EnumCalendarInfoA), S(EnumCalendarInfoExW), \
  S(GetSystemWindowsDirectoryW), S(GetSystemWindowsDirectoryA), S(GetVolumeInformationW), S(GetVolumeInformationA), S(GetVolumeInformationByHandleW), S(ConvertSidToStringSidW), S(ConvertSidToStringSidA), S(CharUpperBuffW), S(CharLowerBuffW), S(CharUpperBuffA), S(CharLowerBuffA)
