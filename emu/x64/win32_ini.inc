/* kernel32 private-profile (.ini) API on the virtual disk + a few small USER32 helpers installers import.
   Semantics follow Windows: section/key lookup is case-insensitive, writing NULL key deletes the section,
   NULL value deletes the key, missing file is created, a relative name means C:\Windows. */
static int ini_arg(Cpu *c, u64 a, int wide, char *out, size_t cap) { if (!a) { out[0] = 0; return 0; } if (wide) wstr_to_utf8(c, a, out, cap, -1); else { const char *s = rd_str(c, a, (u32)cap - 1); e_snprintf(out, cap, "%s", s ? s : ""); } return 1; }
static void ini_path(const char *name, char *out, size_t cap) { if (!name[0]) { e_snprintf(out, cap, "C:\\Windows\\win.ini"); return; } int abs = (name[0] && name[1] == ':') || name[0] == '\\' || name[0] == '/'; if (abs) e_snprintf(out, cap, "%s", name); else e_snprintf(out, cap, "C:\\Windows\\%s", name); }
/* whole file as a NUL-terminated, newline-normalised buffer (UTF-16 files are converted to UTF-8) */
static char *ini_load(const char *path, size_t *len) {
  u64 sz = 0; if (vfs_stat(path, &sz) != 0 || sz > (64u << 20)) { *len = 0; return 0; } int i = vfs_find_node(path); const u8 *d = i >= 0 ? vfs_node_data(i) : 0; if (!d) { *len = 0; return 0; }
  char *b = emu_alloc((size_t)sz * 3 + 2); size_t o = 0;
  if (sz >= 2 && d[0] == 0xFF && d[1] == 0xFE) { for (u64 k = 2; k + 1 < sz; k += 2) { u32 cp = d[k] | (d[k + 1] << 8); if (cp >= 0xD800 && cp < 0xDC00 && k + 3 < sz) { u32 lo = d[k + 2] | (d[k + 3] << 8); cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); k += 2; }
      if (cp < 0x80) b[o++] = (char)cp; else if (cp < 0x800) { b[o++] = (char)(0xC0 | (cp >> 6)); b[o++] = (char)(0x80 | (cp & 0x3f)); } else if (cp < 0x10000) { b[o++] = (char)(0xE0 | (cp >> 12)); b[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[o++] = (char)(0x80 | (cp & 0x3f)); } else { b[o++] = (char)(0xF0 | (cp >> 18)); b[o++] = (char)(0x80 | ((cp >> 12) & 0x3f)); b[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[o++] = (char)(0x80 | (cp & 0x3f)); } } }
  else { u64 k = 0; if (sz >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) k = 3; for (; k < sz; k++) if (d[k] != '\r') b[o++] = (char)d[k]; }
  b[o] = 0; *len = o; return b; }
static int ini_save(const char *path, const char *data, size_t len) { int fd = vfs_open(path, 2, 2 /* CREATE_ALWAYS */, 0); if (fd < 0) return 0; /* CRLF like Windows writes it */ for (size_t i = 0, s = 0; i <= len; i++) if (i == len || data[i] == '\n') { if (i > s) vfs_write(fd, (const u8 *)data + s, i - s); if (i < len) vfs_write(fd, (const u8 *)"\r\n", 2); s = i + 1; } vfs_close(fd); return 1; }
static const char *ini_trim(const char *s, const char *e, const char **end) { while (s < e && (*s == ' ' || *s == '\t')) s++; while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--; *end = e; return s; }
/* section bounds: [start of first line after the header, end of the last line before the next header) */
static int ini_find_section(const char *b, size_t len, const char *sec, size_t *s0, size_t *s1, size_t *hdr) {
  size_t i = 0; while (i < len) { size_t e = i; while (e < len && b[e] != '\n') e++; const char *te; const char *t = ini_trim(b + i, b + e, &te);
    if (t < te && *t == '[') { const char *cl = t + 1; while (cl < te && *cl != ']') cl++; size_t nl = (size_t)(cl - t - 1); if (nl == e_strlen(sec) && !e_strnicmp(t + 1, sec, nl)) { *hdr = i; *s0 = e < len ? e + 1 : len; size_t j = *s0; while (j < len) { size_t je = j; while (je < len && b[je] != '\n') je++; const char *tt = ini_trim(b + j, b + je, &te); if (tt < te && *tt == '[') break; j = je < len ? je + 1 : len; } *s1 = j; return 1; } }
    i = e < len ? e + 1 : len; }
  return 0; }
/* key line inside [s0,s1): returns 1 and the value span */
static int ini_find_key(const char *b, size_t s0, size_t s1, const char *key, size_t *l0, size_t *l1, const char **v0, const char **v1) {
  size_t i = s0; while (i < s1) { size_t e = i; while (e < s1 && b[e] != '\n') e++; const char *te; const char *t = ini_trim(b + i, b + e, &te);
    if (t < te && *t != ';' && *t != '#') { const char *eq = t; while (eq < te && *eq != '=') eq++; if (eq < te) { const char *ke; const char *k = ini_trim(t, eq, &ke); if ((size_t)(ke - k) == e_strlen(key) && !e_strnicmp(k, key, (size_t)(ke - k))) { const char *ve; const char *v = ini_trim(eq + 1, te, &ve); if (ve - v >= 2 && ((*v == '"' && ve[-1] == '"') || (*v == '\'' && ve[-1] == '\''))) { v++; ve--; } *l0 = i; *l1 = e < s1 ? e + 1 : s1; *v0 = v; *v1 = ve; return 1; } } }
    i = e < s1 ? e + 1 : s1; }
  return 0; }
/* result strings go out as ANSI or UTF-16; a double-NUL list when key/section is NULL (like Windows) */
static u32 ini_out(Cpu *c, u64 dst, u32 cap_units, int wide, const char *s, size_t n, int list) {
  if (!dst || !cap_units) return 0; u32 o = 0; size_t i = 0; u32 lim = list ? (cap_units >= 2 ? cap_units - 2 : 0) : cap_units - 1;
  while (i < n && o < lim) { u32 b = (u8)s[i++], cp = b; int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0; if (extra) cp = b & (0x3f >> extra); for (int k = 0; k < extra && i < n && ((u8)s[i] & 0xC0) == 0x80; k++) cp = (cp << 6) | ((u8)s[i++] & 0x3f);
    if (wide) { if (cp >= 0x10000) { if (o + 1 >= lim) break; cp -= 0x10000; wr16(c, dst + o++ * 2, (u16)(0xD800 | (cp >> 10))); wr16(c, dst + o++ * 2, (u16)(0xDC00 | (cp & 0x3ff))); } else wr16(c, dst + o++ * 2, (u16)cp); } else wr8(c, dst + o++, cp < 0x100 ? (u8)cp : '?'); }
  if (wide) { wr16(c, dst + o * 2, 0); if (list) wr16(c, dst + o * 2 + 2, 0); } else { wr8(c, dst + o, 0); if (list) wr8(c, dst + o + 1, 0); }
  return o; }
static void ini_get_string(Cpu *c, int wide, int filearg) {
  char sec[256], key[256], def[1024], file[1024], path[1200]; int has_sec = ini_arg(c, ARG(0), wide, sec, sizeof sec), has_key = ini_arg(c, ARG(1), wide, key, sizeof key); ini_arg(c, ARG(2), wide, def, sizeof def); ini_arg(c, filearg >= 0 ? ARG(filearg) : 0, wide, file, sizeof file); ini_path(file, path, sizeof path);
  u64 dst = ARG(3); u32 cap = (u32)ARG(4); size_t len; char *b = ini_load(path, &len); char *acc = 0; size_t an = 0; u32 r = 0;
  if (!has_sec || !has_key) { /* list of section names or key names, double-NUL terminated */ acc = emu_alloc(len + 2); size_t i = 0; size_t s0 = 0, s1 = 0, h = 0; int in = !has_sec ? 1 : (b && ini_find_section(b, len, sec, &s0, &s1, &h)); if (!has_sec) { s0 = 0; s1 = len; }
    if (in && b) { i = s0; while (i < s1) { size_t e = i; while (e < s1 && b[e] != '\n') e++; const char *te; const char *t = ini_trim(b + i, b + e, &te); if (t < te) { if (!has_sec) { if (*t == '[') { const char *cl = t + 1; while (cl < te && *cl != ']') cl++; e_memcpy(acc + an, t + 1, (size_t)(cl - t - 1)); an += (size_t)(cl - t - 1); acc[an++] = 0; } } else if (*t != ';' && *t != '#' && *t != '[') { const char *eq = t; while (eq < te && *eq != '=') eq++; if (eq < te) { const char *ke; const char *k = ini_trim(t, eq, &ke); e_memcpy(acc + an, k, (size_t)(ke - k)); an += (size_t)(ke - k); acc[an++] = 0; } } } i = e < s1 ? e + 1 : s1; } }
    /* ini_out stops at NUL bytes: emit each entry separately */ u32 o = 0; u32 lim = cap >= 2 ? cap - 2 : 0; for (size_t p = 0; p < an && o < lim; ) { size_t l = e_strlen(acc + p); u32 w2 = ini_out(c, dst + (u64)o * (wide ? 2 : 1), lim - o + 1, wide, acc + p, l, 0); o += w2 + 1; p += l + 1; if (w2 < l) { o = lim; break; } }
    if (cap) { if (wide) { wr16(c, dst + (u64)o * 2, 0); if (o + 1 < cap) wr16(c, dst + (u64)o * 2 + 2, 0); } else { wr8(c, dst + o, 0); if (o + 1 < cap) wr8(c, dst + o + 1, 0); } } r = o; emu_free(acc); }
  else { size_t s0, s1, h, l0, l1; const char *v0, *v1; if (b && ini_find_section(b, len, sec, &s0, &s1, &h) && ini_find_key(b, s0, s1, key, &l0, &l1, &v0, &v1)) r = ini_out(c, dst, cap, wide, v0, (size_t)(v1 - v0), 0); else r = ini_out(c, dst, cap, wide, def, e_strlen(def), 0); }
  if (b) emu_free(b); RET(r); }
static void ini_get_int(Cpu *c, int wide, int filearg) {
  char sec[256], key[256], file[1024], path[1200]; ini_arg(c, ARG(0), wide, sec, sizeof sec); ini_arg(c, ARG(1), wide, key, sizeof key); ini_arg(c, filearg >= 0 ? ARG(filearg) : 0, wide, file, sizeof file); ini_path(file, path, sizeof path);
  size_t len; char *b = ini_load(path, &len); u32 r = (u32)ARG(2); size_t s0, s1, h, l0, l1; const char *v0, *v1;
  if (b && ini_find_section(b, len, sec, &s0, &s1, &h) && ini_find_key(b, s0, s1, key, &l0, &l1, &v0, &v1)) { int neg = 0; const char *p = v0; if (p < v1 && (*p == '-' || *p == '+')) { neg = *p == '-'; p++; } u32 v = 0; int any = 0; while (p < v1 && *p >= '0' && *p <= '9') { v = v * 10 + (u32)(*p - '0'); p++; any = 1; } if (any) r = neg ? (u32)-(s32)v : v; }
  if (b) emu_free(b); RET(r); }
static void ini_write_string(Cpu *c, int wide, int filearg) {
  char sec[256], key[256], val[2048], file[1024], path[1200]; ini_arg(c, ARG(0), wide, sec, sizeof sec); int has_key = ini_arg(c, ARG(1), wide, key, sizeof key), has_val = ini_arg(c, ARG(2), wide, val, sizeof val); ini_arg(c, filearg >= 0 ? ARG(filearg) : 0, wide, file, sizeof file); ini_path(file, path, sizeof path);
  if (!sec[0]) { W->last_error = 87; RET(0); return; }
  size_t len; char *b = ini_load(path, &len); if (!b) { b = emu_alloc(1); b[0] = 0; len = 0; }
  size_t cap = len + e_strlen(sec) + e_strlen(key) + e_strlen(val) + 16; char *n = emu_alloc(cap); size_t s0, s1, h, on = 0;
  int have = ini_find_section(b, len, sec, &s0, &s1, &h);
  if (!has_key) { /* delete the whole section */ if (have) { e_memcpy(n, b, h); on = h; e_memcpy(n + on, b + s1, len - s1); on += len - s1; } else { e_memcpy(n, b, len); on = len; } }
  else if (!have) { if (!has_val) { e_memcpy(n, b, len); on = len; } else { e_memcpy(n, b, len); on = len; if (on && n[on - 1] != '\n') n[on++] = '\n'; on += (size_t)e_snprintf(n + on, cap - on, "[%s]\n%s=%s\n", sec, key, val); } }
  else { size_t l0, l1; const char *v0, *v1; int hk = ini_find_key(b, s0, s1, key, &l0, &l1, &v0, &v1);
    if (hk) { e_memcpy(n, b, l0); on = l0; if (has_val) on += (size_t)e_snprintf(n + on, cap - on, "%s=%s\n", key, val); e_memcpy(n + on, b + l1, len - l1); on += len - l1; }
    else if (!has_val) { e_memcpy(n, b, len); on = len; }
    else { /* append at the end of the section (before trailing blank lines) */ size_t ins = s1; while (ins > s0) { size_t ls = ins - 1; while (ls > s0 && b[ls - 1] != '\n') ls--; const char *te; const char *t = ini_trim(b + ls, b + ins - 1, &te); if (t < te) break; ins = ls; } e_memcpy(n, b, ins); on = ins; if (on && n[on - 1] != '\n') n[on++] = '\n'; on += (size_t)e_snprintf(n + on, cap - on, "%s=%s\n", key, val); e_memcpy(n + on, b + ins, len - ins); on += len - ins; } }
  int ok = ini_save(path, n, on); emu_free(n); emu_free(b); if (!ok) W->last_error = 5; RET(ok); }
static void s_GetPrivateProfileStringA(Cpu *c) { ini_get_string(c, 0, 5); }
static void s_GetPrivateProfileStringW(Cpu *c) { ini_get_string(c, 1, 5); }
static void s_GetPrivateProfileIntA(Cpu *c) { ini_get_int(c, 0, 3); }
static void s_GetPrivateProfileIntW(Cpu *c) { ini_get_int(c, 1, 3); }
static void s_WritePrivateProfileStringA(Cpu *c) { ini_write_string(c, 0, 3); }
static void s_WritePrivateProfileStringW(Cpu *c) { ini_write_string(c, 1, 3); }
/* GetPrivateProfileSectionNames(buf, cap, file) → GetPrivateProfileString(NULL, NULL, "", buf, cap, file) */
static void ini_section_names(Cpu *c, int wide) { u64 buf = ARG(0), cap = ARG(1), file = ARG(2); char f[1024], path[1200]; ini_arg(c, file, wide, f, sizeof f); ini_path(f, path, sizeof path); size_t len; char *b = ini_load(path, &len); u32 o = 0, lim = cap >= 2 ? (u32)cap - 2 : 0;
  if (b) { size_t i = 0; while (i < len && o < lim) { size_t e = i; while (e < len && b[e] != '\n') e++; const char *te; const char *t = ini_trim(b + i, b + e, &te); if (t < te && *t == '[') { const char *cl = t + 1; while (cl < te && *cl != ']') cl++; u32 w2 = ini_out(c, buf + (u64)o * (wide ? 2 : 1), lim - o + 1, wide, t + 1, (size_t)(cl - t - 1), 0); o += w2 + 1; } i = e < len ? e + 1 : len; } emu_free(b); }
  if (cap) { if (wide) { wr16(c, buf + (u64)o * 2, 0); if (o + 1 < cap) wr16(c, buf + (u64)o * 2 + 2, 0); } else { wr8(c, buf + o, 0); if (o + 1 < cap) wr8(c, buf + o + 1, 0); } } RET(o); }
static void s_GetPrivateProfileSectionNamesA(Cpu *c) { ini_section_names(c, 0); }
static void s_GetPrivateProfileSectionNamesW(Cpu *c) { ini_section_names(c, 1); }
/* GetPrivateProfileSection(sec, buf, cap, file): "key=value" entries, double-NUL terminated */
static void ini_section(Cpu *c, int wide) { char sec[256], f[1024], path[1200]; ini_arg(c, ARG(0), wide, sec, sizeof sec); ini_arg(c, ARG(3), wide, f, sizeof f); ini_path(f, path, sizeof path); u64 buf = ARG(1), cap = ARG(2); size_t len; char *b = ini_load(path, &len); u32 o = 0, lim = cap >= 2 ? (u32)cap - 2 : 0; size_t s0, s1, h;
  if (b && ini_find_section(b, len, sec, &s0, &s1, &h)) { size_t i = s0; while (i < s1 && o < lim) { size_t e = i; while (e < s1 && b[e] != '\n') e++; const char *te; const char *t = ini_trim(b + i, b + e, &te); if (t < te && *t != ';' && *t != '#') { u32 w2 = ini_out(c, buf + (u64)o * (wide ? 2 : 1), lim - o + 1, wide, t, (size_t)(te - t), 0); o += w2 + 1; } i = e < s1 ? e + 1 : s1; } }
  if (b) emu_free(b); if (cap) { if (wide) { wr16(c, buf + (u64)o * 2, 0); if (o + 1 < cap) wr16(c, buf + (u64)o * 2 + 2, 0); } else { wr8(c, buf + o, 0); if (o + 1 < cap) wr8(c, buf + o + 1, 0); } } RET(o); }
static void s_GetPrivateProfileSectionA(Cpu *c) { ini_section(c, 0); }
static void s_GetPrivateProfileSectionW(Cpu *c) { ini_section(c, 1); }
/* GetProfileString/Int/WriteProfileString = the same on win.ini */
/* GetProfileString(app, key, def, buf, cap) has no file argument: win.ini */
static void s_GetProfileStringA(Cpu *c) { ini_get_string(c, 0, -1); }
static void s_GetProfileStringW(Cpu *c) { ini_get_string(c, 1, -1); }
static void s_GetProfileIntA(Cpu *c) { ini_get_int(c, 0, -1); }
static void s_GetProfileIntW(Cpu *c) { ini_get_int(c, 1, -1); }
/* USER32 helpers */
static void s_MulDiv(Cpu *c) { s64 a = (s32)ARG(0), b = (s32)ARG(1), d = (s32)ARG(2); if (d == 0) { RET((u32)-1); return; } s64 p = a * b; s64 r = (p < 0) == (d < 0) ? (p + d / 2) / d : (p - d / 2) / d; if (r > 0x7fffffff || r < -0x7fffffffll - 1) { RET((u32)-1); return; } RET((u32)(s32)r); }
static void s_CharNextW(Cpu *c) { u64 p = ARG(0); if (!p || !rd16(c, p)) { RET(p); return; } u32 u = rd16(c, p); RET(u >= 0xD800 && u < 0xDC00 && rd16(c, p + 2) >= 0xDC00 && rd16(c, p + 2) < 0xE000 ? p + 4 : p + 2); }
static void s_CharPrevW(Cpu *c) { u64 start = ARG(0), p = ARG(1); if (p <= start) { RET(start); return; } u64 q = p - 2; u32 u = rd16(c, q); if (u >= 0xDC00 && u < 0xE000 && q - 2 >= start && rd16(c, q - 2) >= 0xD800 && rd16(c, q - 2) < 0xDC00) q -= 2; RET(q); }
static void s_CharNextExA(Cpu *c) { u64 p = ARG(1); RET(p && rd8(c, p) ? p + 1 : p); }
#define INI_SHIMS S(GetPrivateProfileStringA), S(GetPrivateProfileStringW), S(GetPrivateProfileIntA), S(GetPrivateProfileIntW), S(WritePrivateProfileStringA), S(WritePrivateProfileStringW), \
  S(GetPrivateProfileSectionNamesA), S(GetPrivateProfileSectionNamesW), S(GetPrivateProfileSectionA), S(GetPrivateProfileSectionW), S(GetProfileStringA), S(GetProfileStringW), S(GetProfileIntA), S(GetProfileIntW), \
  S(MulDiv), S(CharNextW), S(CharPrevW), S(CharNextExA)
static void s_WriteProfileStringA(Cpu *c) { ini_write_string(c, 0, -1); }
static void s_WriteProfileStringW(Cpu *c) { ini_write_string(c, 1, -1); }
#define INI_SHIMS2 S(WriteProfileStringA), S(WriteProfileStringW)
