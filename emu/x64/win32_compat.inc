/* win32_compat.inc — shims added while bringing up real third-party 64-bit applications
   (7-Zip 25.01, zstd 1.5.7, SQLite 3.50, jq 1.8.1): OLEAUT32 BSTR/VARIANT, ADVAPI32 security/registry
   failures that applications treat as "feature unavailable", calendar conversions done properly
   (FILETIME <-> SYSTEMTIME <-> DOS date), disk-space queries, CRT odds and ends and the long tail of libm.
   Included by win32.c; COMPAT_SHIMS is matched before the generic table so it may override older stubs. */

/* ---------- calendar helpers (proleptic Gregorian, days-from-civil) ---------- */
static s64 days_from_civil(s64 y, int m, int d) { y -= m <= 2; s64 era = (y >= 0 ? y : y - 399) / 400; s64 yoe = y - era * 400; s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + doe - 719468; }
static void civil_from_days(s64 z, int *y, int *m, int *d) { z += 719468; s64 era = (z >= 0 ? z : z - 146096) / 146097; s64 doe = z - era * 146097; s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; s64 yy = yoe + era * 400; s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100); s64 mp = (5 * doy + 2) / 153; *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yy + (*m <= 2)); }
#define FT_EPOCH 116444736000000000ull
/* SYSTEMTIME: wYear wMonth wDayOfWeek wDay wHour wMinute wSecond wMilliseconds (8 x u16) */
static void ft_to_systemtime(Cpu *c, u64 ft, u64 st) {
  u64 ms = ft / 10000; s64 days = (s64)(ms / 86400000), rem = (s64)(ms % 86400000); days -= 134774; /* 1601 -> 1970 */
  int y, m, d; civil_from_days(days, &y, &m, &d); int wd = (int)((days % 7 + 11) % 7); /* 1970-01-01 was Thursday(4) */
  wr16(c, st, (u16)y); wr16(c, st + 2, (u16)m); wr16(c, st + 4, (u16)wd); wr16(c, st + 6, (u16)d);
  wr16(c, st + 8, (u16)(rem / 3600000)); wr16(c, st + 10, (u16)(rem / 60000 % 60)); wr16(c, st + 12, (u16)(rem / 1000 % 60)); wr16(c, st + 14, (u16)(rem % 1000));
}
static u64 systemtime_to_ft(Cpu *c, u64 st) {
  int y = rd16(c, st), m = rd16(c, st + 2), d = rd16(c, st + 6); if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
  s64 days = days_from_civil(y, m, d) + 134774;
  u64 ms = (u64)days * 86400000ull + (u64)rd16(c, st + 8) * 3600000ull + (u64)rd16(c, st + 10) * 60000ull + (u64)rd16(c, st + 12) * 1000ull + rd16(c, st + 14);
  return ms * 10000;
}
static void s_FileTimeToSystemTime2(Cpu *c) { ft_to_systemtime(c, rd64(c, ARG(0)), ARG(1)); RET(1); }
static void s_SystemTimeToFileTime2(Cpu *c) { u64 ft = systemtime_to_ft(c, ARG(0)); if (!ft) { W->last_error = 87; RET(0); return; } wr64(c, ARG(1), ft); RET(1); }
static void s_SystemTimeToTzSpecificLocalTime(Cpu *c) { u8 *s = mem_ptr(c->mem, ARG(1), 16, 0), *d = mem_ptr(c->mem, ARG(2), 16, 1); if (s && d) e_memcpy(d, s, 16); RET(1); }   /* UTC timezone */
static void s_TzSpecificLocalTimeToSystemTime(Cpu *c) { s_SystemTimeToTzSpecificLocalTime(c); }
static void s_FileTimeToDosDateTime(Cpu *c) {
  u64 ft = rd64(c, ARG(0)); u64 ms = ft / 10000; s64 days = (s64)(ms / 86400000) - 134774, rem = (s64)(ms % 86400000);
  int y, m, d; civil_from_days(days, &y, &m, &d); if (y < 1980 || y > 2107) { W->last_error = 87; RET(0); return; }
  wr16(c, ARG(1), (u16)(((y - 1980) << 9) | (m << 5) | d)); wr16(c, ARG(2), (u16)(((rem / 3600000) << 11) | ((rem / 60000 % 60) << 5) | (rem / 1000 % 60 / 2))); RET(1);
}
static void s_DosDateTimeToFileTime(Cpu *c) {
  u32 dd = (u32)ARG(0) & 0xffff, dt = (u32)ARG(1) & 0xffff; int y = 1980 + (dd >> 9), m = (dd >> 5) & 15, d = dd & 31;
  if (m < 1 || m > 12 || d < 1) { W->last_error = 87; RET(0); return; }
  s64 days = days_from_civil(y, m, d) + 134774; u64 ms = (u64)days * 86400000ull + (u64)(dt >> 11) * 3600000ull + (u64)((dt >> 5) & 63) * 60000ull + (u64)(dt & 31) * 2000ull;
  wr64(c, ARG(2), ms * 10000); RET(1);
}
static void s_CompareFileTime(Cpu *c) { u64 a = rd64(c, ARG(0)), b = rd64(c, ARG(1)); RET(a < b ? (u64)(s64)-1 : a > b ? 1 : 0); }
static void s_mkgmtime64(Cpu *c) { u64 tm = ARG(0); int sec = (int)rd32(c, tm), min = (int)rd32(c, tm + 4), hour = (int)rd32(c, tm + 8), d = (int)rd32(c, tm + 12), m = (int)rd32(c, tm + 16) + 1, y = (int)rd32(c, tm + 20) + 1900;
  s64 days = days_from_civil(y, m, d); RET((u64)(days * 86400 + hour * 3600 + min * 60 + sec)); }

/* ---------- OLEAUT32 (imported by ordinal; 7-Zip uses BSTR/VARIANT for its property system) ---------- */
static u64 bstr_alloc(Cpu *c, const u16 *src, u64 src_addr, u32 nchars) {
  u64 b = w32_heap_alloc(W, 4 + (u64)nchars * 2 + 2, 1); if (!b) return 0;
  wr32(c, b, nchars * 2); for (u32 i = 0; i < nchars; i++) wr16(c, b + 4 + (u64)i * 2, src ? src[i] : src_addr ? rd16(c, src_addr + (u64)i * 2) : 0);
  wr16(c, b + 4 + (u64)nchars * 2, 0); return b + 4;
}
static void s_SysAllocString(Cpu *c) { u64 s = ARG(0); if (!s) { RET(0); return; } u32 n = 0; while (rd16(c, s + (u64)n * 2) && n < (1u << 26)) n++; RET(bstr_alloc(c, 0, s, n)); }
static void s_SysAllocStringLen(Cpu *c) { RET(bstr_alloc(c, 0, ARG(0), (u32)ARG(1))); }
static void s_SysAllocStringByteLen(Cpu *c) { u32 bytes = (u32)ARG(1); u64 b = w32_heap_alloc(W, 4 + bytes + 2, 1); if (!b) { RET(0); return; } wr32(c, b, bytes);
  if (ARG(0)) { u8 *s = mem_ptr(c->mem, ARG(0), bytes, 0), *d = mem_ptr(c->mem, b + 4, bytes, 1); if (s && d) e_memcpy(d, s, bytes); } wr16(c, b + 4 + bytes, 0); RET(b + 4); }
static void s_SysFreeString(Cpu *c) { if (ARG(0)) w32_heap_free(W, ARG(0) - 4); }
static void s_SysStringLen(Cpu *c) { RET(ARG(0) ? rd32(c, ARG(0) - 4) / 2 : 0); }
static void s_SysStringByteLen(Cpu *c) { RET(ARG(0) ? rd32(c, ARG(0) - 4) : 0); }
static void s_SysReAllocStringLen(Cpu *c) { u64 pb = ARG(0); u64 nb = bstr_alloc(c, 0, ARG(1), (u32)ARG(2)); if (!nb) { RET(0); return; } u64 old = rd64(c, pb); if (old) w32_heap_free(W, old - 4); wr64(c, pb, nb); RET(1); }
#define VARSZ(c) ((c)->mode32 ? 16u : 24u)   /* VARIANT/PROPVARIANT: 16 bytes on x86, 24 on x64 (union at +8 in both) */
static void s_VariantInit(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(0), VARSZ(c), 1); if (p) e_memset(p, 0, VARSZ(c)); }
static void s_VariantClear(Cpu *c) { u64 v = ARG(0); u16 vt = rd16(c, v); if (vt == 8) { u64 b = rdp(c, v + 8); if (b) w32_heap_free(W, b - 4); } u8 *p = mem_ptr(c->mem, v, VARSZ(c), 1); if (p) e_memset(p, 0, VARSZ(c)); RET(0); }
static void s_VariantCopy(Cpu *c) { u64 d = ARG(0), s = ARG(1); if (d == s) { RET(0); return; } call_shim(c, s_VariantClear, d, 0, 0, 0); u8 *sp = mem_ptr(c->mem, s, VARSZ(c), 0), *dp = mem_ptr(c->mem, d, VARSZ(c), 1); if (sp && dp) e_memcpy(dp, sp, VARSZ(c));
  if (rd16(c, s) == 8) { call_shim(c, s_SysAllocString, rdp(c, s + 8), 0, 0, 0); wrp(c, d + 8, c->r[RAX]); } RET(0); }

/* ---------- ADVAPI32: privileges / security / registry → "not available", never a crash ---------- */
static void s_LookupPrivilegeValueW(Cpu *c) { W->last_error = 1313; RET(0); }      /* ERROR_NO_SUCH_PRIVILEGE */
static void s_AdjustTokenPrivileges(Cpu *c) { W->last_error = 0; RET(1); }
static void s_GetFileSecurityW(Cpu *c) { W->last_error = 50; RET(0); }            /* ERROR_NOT_SUPPORTED */
static void s_SetFileSecurityW(Cpu *c) { W->last_error = 50; RET(0); }

/* ---------- KERNEL32 odds and ends ---------- */
static void s_GetLargePageMinimum(Cpu *c) { RET(0); }
static void s_OpenFileMappingW(Cpu *c) { W->last_error = 2; RET(0); }
static void s_GetLogicalDriveStringsW(Cpu *c) { u32 n = (u32)ARG(0); const u16 s[5] = { 'C', ':', '\\', 0, 0 }; if (n < 5) { RET(5); return; } for (int i = 0; i < 5; i++) wr16(c, ARG(1) + i * 2, s[i]); RET(4); }
static void s_GetLogicalDriveStringsA(Cpu *c) { u32 n = (u32)ARG(0); if (n < 5) { RET(5); return; } u8 *p = mem_ptr(c->mem, ARG(1), 5, 1); if (p) e_memcpy(p, "C:\\\0", 5); RET(4); }
static void s_GetDriveTypeW(Cpu *c) { RET(3); }                                     /* DRIVE_FIXED */
static void s_GetDiskFreeSpaceExW(Cpu *c) { if (ARG(1)) wr64(c, ARG(1), 2ull << 30); if (ARG(2)) wr64(c, ARG(2), 4ull << 30); if (ARG(3)) wr64(c, ARG(3), 2ull << 30); RET(1); }
static void s_GetDiskFreeSpaceW(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 8); if (ARG(2)) wr32(c, ARG(2), 512); if (ARG(3)) wr32(c, ARG(3), 524288); if (ARG(4)) wr32(c, ARG(4), 1048576); RET(1); }
static void s_GetDiskFreeSpaceA(Cpu *c) { s_GetDiskFreeSpaceW(c); }
static void s_CreateHardLinkW(Cpu *c) { W->last_error = 1; RET(0); }
static void s_FindFirstStreamW(Cpu *c) { W->last_error = 38; RET(~0ull); }         /* ERROR_HANDLE_EOF: no alternate streams */
static void s_FindNextStreamW(Cpu *c) { W->last_error = 38; RET(0); }
static void s_DeviceIoControl(Cpu *c) { W->last_error = 1; RET(0); }
static void s_GetLogicalProcessorInformation(Cpu *c) {
  /* one core, one NUMA node, L1d/L1i/L2/L3 caches, one package: 7 records; SYSTEM_LOGICAL_PROCESSOR_INFORMATION is
     32 bytes on x64 (ProcessorMask 8 + Relationship 4 + pad 4 + union 16) and 24 bytes on x86 (4 + 4 + 16) */
  int m32 = c->mode32; u32 rs = m32 ? 24 : 32, uo = m32 ? 8 : 16;   /* record size, offset of the union */
  u64 buf = ARG(0), plen = ARG(1); u32 need = 7 * rs; u32 have = plen ? rd32(c, plen) : 0;
  if (plen) wr32(c, plen, need);
  if (!buf || have < need) { W->last_error = 122; RET(0); return; }
  u8 *p = mem_ptr(c->mem, buf, need, 1); if (!p) { RET(0); return; } e_memset(p, 0, need);
  struct { u32 rel; u8 level, assoc; u16 line; u32 size; u32 type; } caches[4] = { { 2, 1, 8, 64, 32768, 1 }, { 2, 1, 8, 64, 32768, 2 }, { 2, 2, 8, 64, 1048576, 0 }, { 2, 3, 16, 64, 16777216, 0 } };
  u64 a = buf;
#define LPI_HDR(rel) do { wrp(c, a, 1); wr32(c, a + PSZ, (rel)); } while (0)
  LPI_HDR(0); wr8(c, a + uo, 0); a += rs;                                                                       /* RelationProcessorCore */
  LPI_HDR(1); wr32(c, a + uo, 0); a += rs;                                                                      /* RelationNumaNode */
  for (int i = 0; i < 4; i++) { LPI_HDR(2); wr8(c, a + uo, caches[i].level); wr8(c, a + uo + 1, caches[i].assoc); wr16(c, a + uo + 2, caches[i].line); wr32(c, a + uo + 4, caches[i].size); wr32(c, a + uo + 8, caches[i].type); a += rs; }
  LPI_HDR(3); a += rs;                                                                                          /* RelationProcessorPackage */
#undef LPI_HDR
  RET(1);
}
static void s_GetLogicalProcessorInformationEx(Cpu *c) { W->last_error = 122; if (ARG(2)) wr32(c, ARG(2), 0); RET(0); }

/* ---------- USER32 ---------- */
static void s_CharUpperW(Cpu *c) { u64 s = ARG(0); if (s < 0x10000) { u32 ch = (u32)s; RET(ch >= 'a' && ch <= 'z' ? ch - 32 : ch); return; }
  for (u64 i = 0;; i++) { u16 ch = rd16(c, s + i * 2); if (!ch || c->status) break; if (ch >= 'a' && ch <= 'z') wr16(c, s + i * 2, ch - 32); } RET(s); }
static void s_CharLowerW(Cpu *c) { u64 s = ARG(0); if (s < 0x10000) { u32 ch = (u32)s; RET(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); return; }
  for (u64 i = 0;; i++) { u16 ch = rd16(c, s + i * 2); if (!ch || c->status) break; if (ch >= 'A' && ch <= 'Z') wr16(c, s + i * 2, ch + 32); } RET(s); }
static void s_CharPrevExA(Cpu *c) { u64 start = ARG(1), cur = ARG(2); RET(cur > start ? cur - 1 : start); }
static void s_CharPrevA(Cpu *c) { u64 start = ARG(0), cur = ARG(1); RET(cur > start ? cur - 1 : start); }
static void s_CharNextA(Cpu *c) { RET(rd8(c, ARG(0)) ? ARG(0) + 1 : ARG(0)); }

/* ---------- SHLWAPI ---------- */
static void s_PathIsRelativeA(Cpu *c) { const char *s = rd_str(c, ARG(0), 1024); RET(!s || !s[0] ? 1 : (s[0] == '\\' || s[0] == '/' || (s[1] == ':')) ? 0 : 1); }
/* shlwapi colour helpers: HLS in the Windows 0..240 range */
static void rgb_to_hls(u32 cr, int *H, int *L, int *S) { int r = cr & 255, g = cr >> 8 & 255, b = cr >> 16 & 255; int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b); int sum = mx + mn, d = mx - mn; *L = (sum * 240 + 255) / 510; if (!d) { *S = 0; *H = 160; return; }
  *S = *L <= 120 ? (d * 240 + sum / 2) / sum : (d * 240 + (510 - sum) / 2) / (510 - sum); int rd = ((mx - r) * 40 + d / 2) / d, gd = ((mx - g) * 40 + d / 2) / d, bd = ((mx - b) * 40 + d / 2) / d; int h = r == mx ? bd - gd : g == mx ? 80 + rd - bd : 160 + gd - rd; h %= 240; if (h < 0) h += 240; *H = h; }
static int hue_to_rgb(int n1, int n2, int hue) { hue %= 240; if (hue < 0) hue += 240; if (hue < 40) return n1 + ((n2 - n1) * hue + 20) / 40; if (hue < 120) return n2; if (hue < 160) return n1 + ((n2 - n1) * (160 - hue) + 20) / 40; return n1; }
static u32 hls_to_rgb(int h, int l, int s) { if (s == 0) { int v = (l * 255 + 120) / 240; return (u32)v | (u32)v << 8 | (u32)v << 16; } int n2 = l <= 120 ? (l * (240 + s) + 120) / 240 : l + s - (l * s + 120) / 240; int n1 = 2 * l - n2; int r = (hue_to_rgb(n1, n2, h + 80) * 255 + 120) / 240, g = (hue_to_rgb(n1, n2, h) * 255 + 120) / 240, b = (hue_to_rgb(n1, n2, h - 80) * 255 + 120) / 240; return (u32)r | (u32)g << 8 | (u32)b << 16; }
static void s_ColorRGBToHLS(Cpu *c) { int H, L, S; rgb_to_hls((u32)ARG(0), &H, &L, &S); if (ARG(1)) wr16(c, ARG(1), (u16)H); if (ARG(2)) wr16(c, ARG(2), (u16)L); if (ARG(3)) wr16(c, ARG(3), (u16)S); RET(0); }
static void s_ColorHLSToRGB(Cpu *c) { RET(hls_to_rgb((int)(u16)ARG(0), (int)(u16)ARG(1), (int)(u16)ARG(2))); }
static void s_ColorAdjustLuma(Cpu *c) { int H, L, S; rgb_to_hls((u32)ARG(0), &H, &L, &S); int n = (int)(s32)ARG(1); if (ARG(2)) { if (n > 0) L += (240 - L) * n / 1000; else L += L * n / 1000; } else L += n; if (L < 0) L = 0; if (L > 240) L = 240; RET(hls_to_rgb(H, L, S)); }
/* PathMatchSpec: ';'-separated wildcard list */
static void pm_in(Cpu *c, u64 a, int wide, char *out, size_t cap) { out[0] = 0; if (!a) return; if (wide) wstr_to_utf8(c, a, out, cap, -1); else { const char *s = rd_str(c, a, cap - 1); if (s) e_snprintf(out, cap, "%s", s); } }
static void path_match_spec(Cpu *c, int wide) { char file[1024], spec[1024]; pm_in(c, ARG(0), wide, file, sizeof file); pm_in(c, ARG(1), wide, spec, sizeof spec); const char *base = file; for (const char *p = file; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
  char *p = spec; while (*p) { while (*p == ' ') p++; char *e = p; while (*e && *e != ';') e++; char save = *e; *e = 0; if (*p && (vfs_glob_match(p, base) || vfs_glob_match(p, file))) { RET(1); return; } if (!save) break; p = e + 1; } RET(0); }
static void s_PathMatchSpecW(Cpu *c) { path_match_spec(c, 1); }
static void s_PathMatchSpecA(Cpu *c) { path_match_spec(c, 0); }
static void s_PathIsNetworkPathW(Cpu *c) { u16 a = ARG(0) ? rd16(c, ARG(0)) : 0, b = a ? rd16(c, ARG(0) + 2) : 0; RET(a == '\\' && b == '\\'); }
static void s_PathIsNetworkPathA(Cpu *c) { u8 a = ARG(0) ? rd8(c, ARG(0)) : 0, b = a ? rd8(c, ARG(0) + 1) : 0; RET(a == '\\' && b == '\\'); }
static void s_PathIsRelativeW(Cpu *c) { u16 a = rd16(c, ARG(0)), b = a ? rd16(c, ARG(0) + 2) : 0; RET(!a ? 1 : (a == '\\' || a == '/' || b == ':') ? 0 : 1); }

/* ---------- CRT ---------- */
static void s_XcptFilter(Cpu *c) { RET(0); }
static void s_dllonexit(Cpu *c) { s_atexit(c); RET(ARG(0)); }
static void s_cxx_terminate(Cpu *c) { con_out(W, 2, "terminate() called\n", 19); do_exit(c, 3); }
static void s_wcsstr(Cpu *c) { u64 h = ARG(0), n = ARG(1); u16 n0 = rd16(c, n); if (!n0) { RET(h); return; }
  for (u64 i = 0;; i++) { u16 ch = rd16(c, h + i * 2); if (!ch || c->status) { RET(0); return; } if (ch != n0) continue; u64 j = 1; for (;; j++) { u16 x = rd16(c, n + j * 2); if (!x) { RET(h + i * 2); return; } if (rd16(c, h + (i + j) * 2) != x) break; } } }
static void s_wcsnlen(Cpu *c) { u64 n = 0, m = ARG(1); while (n < m && rd16(c, ARG(0) + n * 2)) n++; RET(n); }
static void s_lock_file(Cpu *c) { (void)c; }
static void s_unlock_file(Cpu *c) { (void)c; }
static void s_utime64(Cpu *c) { RET(0); }
static void s_perror(Cpu *c) { const char *pfx = ARG(0) ? rd_str(c, ARG(0), 512) : 0; char buf[640]; int err = (int)rd32(c, W->ubuf + 0x258);
  int n = pfx && pfx[0] ? e_snprintf(buf, sizeof buf, "%s: %s\n", pfx, errstr(err)) : e_snprintf(buf, sizeof buf, "%s\n", errstr(err)); out_write(c, 2, (const u8 *)buf, n); }
static void s_assert_fail(Cpu *c) { const char *e = rd_str(c, ARG(0), 512), *f = rd_str(c, ARG(1), 512); char buf[1200];
  int n = e_snprintf(buf, sizeof buf, "Assertion failed: %s, file %s, line %u\n", e ? e : "?", f ? f : "?", (u32)ARG(2)); out_write(c, 2, (const u8 *)buf, n); do_exit(c, 3); }
static void s_wassert_fail(Cpu *c) { char e[512], f[512]; wstr_to_utf8(c, ARG(0), e, sizeof e, -1); wstr_to_utf8(c, ARG(1), f, sizeof f, -1); char buf[1200];
  int n = e_snprintf(buf, sizeof buf, "Assertion failed: %s, file %s, line %u\n", e, f, (u32)ARG(2)); out_write(c, 2, (const u8 *)buf, n); do_exit(c, 3); }
static void s_mbrtowc(Cpu *c) { u64 dst = ARG(0), src = ARG(1), n = ARG(2); if (!src) { RET(0); return; } if (!n) { RET((u64)-2); return; } u8 ch = rd8(c, src); if (dst) wr16(c, dst, ch); RET(ch ? 1 : 0); }
static void s_wcrtomb(Cpu *c) { u64 dst = ARG(0); u32 wc = (u32)ARG(1); if (dst) wr8(c, dst, wc < 256 ? (u8)wc : '?'); RET(1); }

/* ---------- libm long tail (double precision, built on the host sin/exp/log/pow via host_math) ---------- */
static double m_exp(double x) { return host_math(3, x, 0); }
static double m_log(double x) { return host_math(4, x, 0); }
static double m_pow(double a, double b) { return host_math(5, a, b); }
static double m_sin(double x) { return host_math(0, x, 0); }
static double m_cos(double x) { return host_math(1, x, 0); }
/* erf: Maclaurin series for |x| < 2.5 (converges to full double precision), continued fraction for erfc beyond that */
static double m_erfc_cf(double ax) { double f = ax; for (int n = 60; n >= 1; n--) f = ax + (n * 0.5) / f; return m_exp(-ax * ax) / (1.7724538509055159 * f); }
static double m_erf_series(double ax) { double sum = ax, term = ax, x2 = ax * ax; for (int n = 1; n < 80; n++) { term *= -x2 / n; double add = term / (2 * n + 1); sum += add; if (add < 1e-18 && add > -1e-18) break; } return sum * 1.1283791670955126; }
static double m_erf(double x) { double ax = x < 0 ? -x : x, r = ax < 2.5 ? m_erf_series(ax) : 1.0 - m_erfc_cf(ax); if (r > 1) r = 1; return x < 0 ? -r : r; }
static double m_erfc(double x) { double ax = x < 0 ? -x : x, y = ax < 2.5 ? 1.0 - m_erf_series(ax) : m_erfc_cf(ax); return x < 0 ? 2 - y : y; }
static double m_tgamma(double x) { if (x < 0.5) return 3.141592653589793 / (m_sin(3.141592653589793 * x) * m_tgamma(1 - x));
  static const double g[] = { 0.99999999999980993, 676.5203681218851, -1259.1392167224028, 771.32342877765313, -176.61502916214059, 12.507343278686905, -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7 };
  x -= 1; double a = g[0], t = x + 7.5; for (int i = 1; i < 9; i++) a += g[i] / (x + i); return 2.5066282746310002 * m_pow(t, x + 0.5) * m_exp(-t) * a; }
static double m_j0(double x) { double ax = x < 0 ? -x : x; if (ax < 8) { double y = x * x; return (57568490574.0 + y * (-13362590354.0 + y * (651619640.7 + y * (-11214424.18 + y * (77392.33017 + y * -184.9052456))))) / (57568490411.0 + y * (1029532985.0 + y * (9494680.718 + y * (59272.64853 + y * (267.8532712 + y))))); }
  double z = 8 / ax, y = z * z, xx = ax - 0.785398164; double p = 1 + y * (-0.1098628627e-2 + y * (0.2734510407e-4 + y * (-0.2073370639e-5 + y * 0.2093887211e-6))); double q = -0.1562499995e-1 + y * (0.1430488765e-3 + y * (-0.6911147651e-5 + y * (0.7621095161e-6 - y * 0.934935152e-7)));
  return e_sqrt(0.636619772 / ax) * (m_cos(xx) * p - z * m_sin(xx) * q); }
static double m_j1(double x) { double ax = x < 0 ? -x : x; if (ax < 8) { double y = x * x; return x * (72362614232.0 + y * (-7895059235.0 + y * (242396853.1 + y * (-2972611.439 + y * (15704.48260 + y * -30.16036606))))) / (144725228442.0 + y * (2300535178.0 + y * (18583304.74 + y * (99447.43394 + y * (376.9991397 + y))))); }
  double z = 8 / ax, y = z * z, xx = ax - 2.356194491; double p = 1 + y * (0.183105e-2 + y * (-0.3516396496e-4 + y * (0.2457520174e-5 + y * -0.240337019e-6))); double q = 0.04687499995 + y * (-0.2002690873e-3 + y * (0.8449199096e-5 + y * (-0.88228987e-6 + y * 0.105787412e-6)));
  double r = e_sqrt(0.636619772 / ax) * (m_cos(xx) * p - z * m_sin(xx) * q); return x < 0 ? -r : r; }
static double m_y0(double x) { if (x < 8) { double y = x * x; double a = -2957821389.0 + y * (7062834065.0 + y * (-512359803.6 + y * (10879881.29 + y * (-86327.92757 + y * 228.4622733)))); double b = 40076544269.0 + y * (745249964.8 + y * (7189466.438 + y * (47447.26470 + y * (226.1030244 + y)))); return a / b + 0.636619772 * m_j0(x) * m_log(x); }
  double z = 8 / x, y = z * z, xx = x - 0.785398164; double p = 1 + y * (-0.1098628627e-2 + y * (0.2734510407e-4 + y * (-0.2073370639e-5 + y * 0.2093887211e-6))); double q = -0.1562499995e-1 + y * (0.1430488765e-3 + y * (-0.6911147651e-5 + y * (0.7621095161e-6 - y * 0.934935152e-7)));
  return e_sqrt(0.636619772 / x) * (m_sin(xx) * p + z * m_cos(xx) * q); }
static double m_y1(double x) { if (x < 8) { double y = x * x; double a = x * (-0.4900604943e13 + y * (0.1275274390e13 + y * (-0.5153438139e11 + y * (0.7349264551e9 + y * (-0.4237922726e7 + y * 0.8511937935e4))))); double b = 0.2499580570e14 + y * (0.4244419664e12 + y * (0.3733650367e10 + y * (0.2245904002e8 + y * (0.1020426050e6 + y * (0.3549632885e3 + y))))); return a / b + 0.636619772 * (m_j1(x) * m_log(x) - 1 / x); }
  double z = 8 / x, y = z * z, xx = x - 2.356194491; double p = 1 + y * (0.183105e-2 + y * (-0.3516396496e-4 + y * (0.2457520174e-5 + y * -0.240337019e-6))); double q = 0.04687499995 + y * (-0.2002690873e-3 + y * (0.8449199096e-5 + y * (-0.88228987e-6 + y * 0.105787412e-6)));
  return e_sqrt(0.636619772 / x) * (m_sin(xx) * p + z * m_cos(xx) * q); }
static double m_jn(int n, double x) { if (n == 0) return m_j0(x); if (n == 1) return m_j1(x); if (x == 0) return 0; double a = m_j0(x), b = m_j1(x); for (int k = 1; k < n; k++) { double t = 2.0 * k / x * b - a; a = b; b = t; } return b; }
static double m_yn(int n, double x) { if (n == 0) return m_y0(x); if (n == 1) return m_y1(x); double a = m_y0(x), b = m_y1(x); for (int k = 1; k < n; k++) { double t = 2.0 * k / x * b - a; a = b; b = t; } return b; }
#define F0 c->x[0].f64[0]
#define F1 c->x[1].f64[0]
static void s_erf(Cpu *c) { F0 = m_erf(F0); }
static void s_erfc(Cpu *c) { F0 = m_erfc(F0); }
static void s_tgamma(Cpu *c) { F0 = m_tgamma(F0); }
static void s_lgamma(Cpu *c) { double g = m_tgamma(F0); F0 = m_log(g < 0 ? -g : g); }
static void s_expm1(Cpu *c) { double x = F0; F0 = (x < 1e-5 && x > -1e-5) ? x + x * x / 2 + x * x * x / 6 : m_exp(x) - 1; }
static void s_log1p(Cpu *c) { double x = F0; F0 = (x < 1e-4 && x > -1e-4) ? x - x * x / 2 + x * x * x / 3 : m_log(1 + x); }
static void s_acosh(Cpu *c) { double x = F0; F0 = m_log(x + e_sqrt(x * x - 1)); }
static void s_asinh(Cpu *c) { double x = F0; double ax = x < 0 ? -x : x; double r = m_log(ax + e_sqrt(ax * ax + 1)); F0 = x < 0 ? -r : r; }
static void s_atanh(Cpu *c) { double x = F0; F0 = 0.5 * m_log((1 + x) / (1 - x)); }
static void s_fdim(Cpu *c) { F0 = F0 > F1 ? F0 - F1 : 0; }
static void s_fma(Cpu *c) { F0 = F0 * F1 + c->x[2].f64[0]; }
static void s_logb(Cpu *c) { double x = F0; if (x == 0) { F0 = -1.0 / 0.0; return; } if (x < 0) x = -x; int e = 0; while (x >= 2) { x /= 2; e++; } while (x < 1) { x *= 2; e--; } F0 = e; }
static void s_remainder(Cpu *c) { double a = F0, b = F1; if (b == 0) { F0 = 0.0 / 0.0; return; } double q = a / b; double n = q < 0 ? -e_floor(-q + 0.5) : e_floor(q + 0.5); F0 = a - n * b; }
static void s_nextafter(Cpu *c) { double a = F0, b = F1; if (a != a || b != b) { F0 = a + b; return; } if (a == b) { F0 = b; return; } u64 bits; e_memcpy(&bits, &a, 8);
  if (a == 0) { bits = 1; double r; e_memcpy(&r, &bits, 8); F0 = b > 0 ? r : -r; return; } if ((a < b) == (a > 0)) bits++; else bits--; e_memcpy(&a, &bits, 8); F0 = a; }
static void s_j0(Cpu *c) { F0 = m_j0(F0); }
static void s_j1(Cpu *c) { F0 = m_j1(F0); }
static void s_y0(Cpu *c) { F0 = m_y0(F0); }
static void s_y1(Cpu *c) { F0 = m_y1(F0); }
static void s_jn(Cpu *c) { F0 = m_jn((int)ARG(0), F1); }
static void s_yn(Cpu *c) { F0 = m_yn((int)ARG(0), F1); }
#undef F0
#undef F1

/* MSVC C++ operator new/delete as exported by msvcrt.dll / msvcr*.dll (D3DCompiler_43, D3DX, older MSVC programs link them by
   mangled name): plain, array, nothrow and sized variants all map onto the process heap */
static void s_cxx_new(Cpu *c) { RET(w32_heap_alloc(W, ARG(0) ? ARG(0) : 1, 0)); }
static void s_cxx_delete(Cpu *c) { if (ARG(0)) w32_heap_free(W, ARG(0)); RET(0); }
static void s__mbstrlen(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 30); RET(s ? e_strlen(s) : 0); }
static void s__fpclass(Cpu *c) {   /* _FPCLASS_* bits: SNAN 1 QNAN 2 NINF 4 NN 8 ND 0x10 NZ 0x20 PZ 0x40 PD 0x80 PN 0x100 PINF 0x200 */
  double d = c->x[0].f64[0]; int neg = __builtin_signbit(d) != 0, r;
  if (d != d) r = 2; else if (__builtin_isinf(d)) r = neg ? 4 : 0x200; else if (d == 0) r = neg ? 0x20 : 0x40;
  else if (e_fabs(d) < 2.2250738585072014e-308) r = neg ? 0x10 : 0x80; else r = neg ? 8 : 0x100;
  RET(r); }
/* msvcrt.dll's std::exception (layout: vptr @0, const char *what @8, int dofree @16) — the D3DX / D3DCompiler DLLs and older
   MSVC programs import its constructors and what() by mangled name; the guest vtable holds the scalar deleting destructor and what() */
static u64 g_cxxexc_vt; static Win32 *g_cxxexc_owner;
static u64 cxxexc_vtable(Cpu *c) { Win32 *w = W; if (g_cxxexc_owner != w) { g_cxxexc_vt = 0; g_cxxexc_owner = w; }
  if (!g_cxxexc_vt) { u64 vt = w32_heap_alloc(w, 24, 1); if (!vt) return 0; wr64(c, vt, w32_shim_slot(w, "msvcrt", "??_Gexception@@UEAAPEAXI@Z")); wr64(c, vt + 8, w32_shim_slot(w, "msvcrt", "?what@exception@@UEBAPEBDXZ")); wr64(c, vt + 16, w32_shim_slot(w, "msvcrt", "?what@exception@@UEBAPEBDXZ")); g_cxxexc_vt = vt; } return g_cxxexc_vt; }
static void cxxexc_init(Cpu *c, u64 self, u64 msg, int copy) { wr64(c, self, cxxexc_vtable(c)); u64 m = msg; if (copy && msg) { const char *s = rd_str(c, msg, 1 << 16); if (s) { size_t l = e_strlen(s); m = w32_heap_alloc(W, l + 1, 0); u8 *p = m ? mem_ptr(c->mem, m, l + 1, 1) : 0; if (p) e_memcpy(p, s, l + 1); else m = msg; } } wr64(c, self + 8, m); wr32(c, self + 16, copy && m != msg); RET(self); }
static void s_cxxexc_ctor0(Cpu *c) { cxxexc_init(c, ARG(0), 0, 0); }                                  /* exception() */
static void s_cxxexc_ctor_msg(Cpu *c) { cxxexc_init(c, ARG(0), rd64(c, ARG(1)), 1); }               /* exception(const char *const &) */
static void s_cxxexc_ctor_msg_nocopy(Cpu *c) { cxxexc_init(c, ARG(0), rd64(c, ARG(1)), 0); }        /* exception(const char *const &, int) */
static void s_cxxexc_ctor_copy(Cpu *c) { cxxexc_init(c, ARG(0), rd64(c, ARG(1) + 8), 1); }          /* exception(const exception &) */
static void s_cxxexc_assign(Cpu *c) { if (ARG(0) != ARG(1)) { wr64(c, ARG(0) + 8, rd64(c, ARG(1) + 8)); wr32(c, ARG(0) + 16, 0); } RET(ARG(0)); }
static void s_cxxexc_dtor(Cpu *c) { RET(0); }
static void s_cxxexc_sdtor(Cpu *c) { if (ARG(1) & 1) w32_heap_free(W, ARG(0)); RET(ARG(0)); }
static void s_cxxexc_what(Cpu *c) { u64 m = rd64(c, ARG(0) + 8); if (!m) { static const char u[] = "Unknown exception"; m = w32_heap_alloc(W, sizeof u, 0); u8 *p = m ? mem_ptr(c->mem, m, sizeof u, 1) : 0; if (p) e_memcpy(p, u, sizeof u); wr64(c, ARG(0) + 8, m); } RET(m); }
static void vsprintf_common(Cpu *c, u64 opts, u64 buf, u64 len, u64 fmt, u64 va, int wide);
static void s__vsnwprintf(Cpu *c) { vsprintf_common(c, 1, ARG(0), ARG(1), ARG(2), ARG(3), 1); }
/* XInput: no controller is connected (ERROR_DEVICE_NOT_CONNECTED) — games fall back to keyboard/mouse; the Gamepad API bridge is future work */
static void s_XInputGetState(Cpu *c) { if (ARG(1)) { u8 *p = mem_ptr(c->mem, ARG(1), 16, 1); if (p) e_memset(p, 0, 16); } RET(1167); }
static void s_XInputNotConnected(Cpu *c) { RET(1167); }
static void s_XInputEnable(Cpu *c) { RET(0); }
#define DX_COMPAT_SHIMS \
  SA("??0exception@@QEAA@XZ", cxxexc_ctor0), SA("??0exception@@QEAA@AEBQEBD@Z", cxxexc_ctor_msg), SA("??0exception@@QEAA@AEBQEBDH@Z", cxxexc_ctor_msg_nocopy), SA("??0exception@@QEAA@AEBV0@@Z", cxxexc_ctor_copy), \
  SA("??4exception@@QEAAAEAV0@AEBV0@@Z", cxxexc_assign), SA("??1exception@@UEAA@XZ", cxxexc_dtor), SA("??_Gexception@@UEAAPEAXI@Z", cxxexc_sdtor), SA("?what@exception@@UEBAPEBDXZ", cxxexc_what), \
  S(_vsnwprintf), SA("RegOpenKeyA", RegOpenKeyExA), SA("RegOpenKeyW", RegOpenKeyExA), SA("IsBadReadPtr", ret0), SA("IsBadWritePtr", ret0), SA("IsBadCodePtr", ret0), SA("IsBadStringPtrA", ret0), SA("IsBadStringPtrW", ret0), \
  S(XInputGetState), SA("XInputGetStateEx", XInputGetState), SA("XInputSetState", XInputNotConnected), SA("XInputGetCapabilities", XInputNotConnected), \
  SA("XInputGetBatteryInformation", XInputNotConnected), SA("XInputGetKeystroke", XInputNotConnected), SA("XInputGetDSoundAudioDeviceGuids", XInputNotConnected), SA("XInputGetAudioDeviceIds", XInputNotConnected), S(XInputEnable),
#define CXX_NEWDELETE_SHIMS DX_COMPAT_SHIMS \
  SA("??2@YAPEAX_K@Z", cxx_new), SA("??_U@YAPEAX_K@Z", cxx_new), SA("??2@YAPEAX_KAEBUnothrow_t@std@@@Z", cxx_new), SA("??_U@YAPEAX_KAEBUnothrow_t@std@@@Z", cxx_new), \
  SA("??3@YAXPEAX@Z", cxx_delete), SA("??_V@YAXPEAX@Z", cxx_delete), SA("??3@YAXPEAX_K@Z", cxx_delete), SA("??_V@YAXPEAX_K@Z", cxx_delete), \
  SA("??3@YAXPEAXAEBUnothrow_t@std@@@Z", cxx_delete), SA("??_V@YAXPEAXAEBUnothrow_t@std@@@Z", cxx_delete), S(_mbstrlen), S(_fpclass),
#define COMPAT_SHIMS CXX_NEWDELETE_SHIMS \
  SA("FileTimeToSystemTime", FileTimeToSystemTime2), SA("SystemTimeToFileTime", SystemTimeToFileTime2), S(SystemTimeToTzSpecificLocalTime), S(TzSpecificLocalTimeToSystemTime), \
  S(FileTimeToDosDateTime), S(DosDateTimeToFileTime), S(CompareFileTime), SA("_mkgmtime64", mkgmtime64), SA("_mkgmtime", mkgmtime64), \
  S(SysAllocString), S(SysAllocStringLen), S(SysAllocStringByteLen), S(SysFreeString), S(SysStringLen), S(SysStringByteLen), S(SysReAllocStringLen), S(VariantInit), S(VariantClear), S(VariantCopy), \
  S(LookupPrivilegeValueW), S(AdjustTokenPrivileges), S(GetFileSecurityW), S(SetFileSecurityW), \
  S(GetLargePageMinimum), S(OpenFileMappingW), S(GetLogicalDriveStringsW), S(GetLogicalDriveStringsA), S(GetDriveTypeW), S(GetDiskFreeSpaceExW), S(GetDiskFreeSpaceW), S(GetDiskFreeSpaceA), \
  S(CreateHardLinkW), S(FindFirstStreamW), S(FindNextStreamW), S(DeviceIoControl), S(GetLogicalProcessorInformation), S(GetLogicalProcessorInformationEx), \
  S(CharUpperW), S(CharLowerW), S(CharPrevExA), S(CharPrevA), S(CharNextA), S(PathIsRelativeA), S(PathIsRelativeW), S(PathIsNetworkPathW), S(PathIsNetworkPathA), S(PathMatchSpecW), S(PathMatchSpecA), S(ColorRGBToHLS), S(ColorHLSToRGB), S(ColorAdjustLuma), \
  SA("_XcptFilter", XcptFilter), SA("__dllonexit", dllonexit), SA("?terminate@@YAXXZ", cxx_terminate), SA("??1type_info@@UEAA@XZ", ret0), \
  S(wcsstr), S(wcsnlen), SA("_lock_file", lock_file), SA("_unlock_file", unlock_file), SA("_utime64", utime64), SA("_utime", utime64), S(perror), SA("_assert", assert_fail), SA("_wassert", wassert_fail), S(mbrtowc), S(wcrtomb), \
  SA("__intrinsic_setjmpex", setjmp), \
  S(erf), S(erfc), S(tgamma), S(lgamma), S(expm1), S(log1p), S(acosh), S(asinh), S(atanh), S(fdim), S(fma), S(logb), S(remainder), S(nextafter), \
  SA("_j0", j0), SA("_j1", j1), SA("_y0", y0), SA("_y1", y1), SA("_jn", jn), SA("_yn", yn), S(j0), S(j1), S(y0), S(y1), S(jn), S(yn),
