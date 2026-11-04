/* PE32+ loader and static inspector */
#include "emu.h"

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 le64(const u8 *p) { return (u64)le32(p) | (u64)le32(p + 4) << 32; }

typedef struct {
  const u8 *d; size_t len;
  u32 pe; u16 machine, nsec, optsz, chars, magic, subsystem, dllchars;
  u64 image_base; u32 entry, salign, falign, size_image, size_headers;
  u64 stack_reserve; u32 ndd; u32 dd[16][2];
  u32 sec_off;
} PeHdr;

static int pe_parse(PeHdr *h, const u8 *d, size_t len) {
  e_memset(h, 0, sizeof *h); h->d = d; h->len = len;
  if (len < 0x40 || d[0] != 'M' || d[1] != 'Z') return -1;
  h->pe = le32(d + 0x3c);
  if (h->pe + 24 > len || le32(d + h->pe) != 0x00004550) return -2;
  const u8 *c = d + h->pe + 4;
  h->machine = le16(c); h->nsec = le16(c + 2); h->optsz = le16(c + 16); h->chars = le16(c + 18);
  const u8 *o = d + h->pe + 24;
  if ((size_t)(h->pe + 24 + h->optsz) > len) return -3;
  h->magic = le16(o);
  if (h->magic == 0x20b) {
    h->entry = le32(o + 16); h->image_base = le64(o + 24); h->salign = le32(o + 32); h->falign = le32(o + 36);
    h->size_image = le32(o + 56); h->size_headers = le32(o + 60); h->subsystem = le16(o + 68); h->dllchars = le16(o + 70);
    h->stack_reserve = le64(o + 72); h->ndd = le32(o + 108);
    for (u32 i = 0; i < h->ndd && i < 16; i++) { h->dd[i][0] = le32(o + 112 + i * 8); h->dd[i][1] = le32(o + 116 + i * 8); }
  } else if (h->magic == 0x10b) {
    h->entry = le32(o + 16); h->image_base = le32(o + 28); h->salign = le32(o + 32); h->falign = le32(o + 36);
    h->size_image = le32(o + 56); h->size_headers = le32(o + 60); h->subsystem = le16(o + 68); h->dllchars = le16(o + 70);
    h->ndd = le32(o + 92);
    for (u32 i = 0; i < h->ndd && i < 16; i++) { h->dd[i][0] = le32(o + 96 + i * 8); h->dd[i][1] = le32(o + 100 + i * 8); }
  } else return -4;
  h->sec_off = h->pe + 24 + h->optsz;
  if ((size_t)h->sec_off + 40u * h->nsec > len) return -5;
  return 0;
}
/* RVA → file offset (for static inspection) */
static const u8 *pe_rva(PeHdr *h, u32 rva, u32 need) {
  if (rva < h->size_headers && (size_t)rva + need <= h->len) return h->d + rva;
  for (int i = 0; i < h->nsec; i++) {
    const u8 *s = h->d + h->sec_off + 40 * i; u32 vs = le32(s + 8), va = le32(s + 12), rs = le32(s + 16), rp = le32(s + 20);
    u32 span = vs > rs ? vs : rs;
    if (rva >= va && rva + need <= va + span) { if (rva - va + need > rs) return 0; if ((size_t)rp + (rva - va) + need > h->len) return 0; return h->d + rp + (rva - va); }
  }
  return 0;
}

/* 0 native, 1 managed assembly (CLR header; x64 or IL-only AnyCPU), 2 .NET apphost launcher (native stub that loads hostfxr.dll),
   3 mixed-mode (native + managed, unsupported) */
int pe_managed_kind(const u8 *bytes, size_t len) {
  PeHdr h; if (pe_parse(&h, bytes, len)) return 0;
  if (h.dd[14][1]) { const u8 *ch = pe_rva(&h, h.dd[14][0], 24); int il = ch ? (le32(ch + 16) & 1) : 0; return (h.machine == 0x8664 || (il && h.machine == 0x14c)) ? 1 : 3; }
  if (h.machine != 0x8664 || (h.chars & 0x2000)) return 0;
  static const u8 needle[] = { 'h',0,'o',0,'s',0,'t',0,'f',0,'x',0,'r',0,'.',0,'d',0,'l',0,'l',0 };   /* UTF-16 "hostfxr.dll" */
  for (size_t i = 0; i + sizeof needle <= len; i += 2) if (bytes[i] == 'h' && bytes[i + 1] == 0 && !e_memcmp(bytes + i, needle, sizeof needle)) return 2;
  return 0;
}
/* DLLs that the emulator provides itself (shims); anything else is a "private" DLL to be loaded from the virtual disk */
int pe_is_system_dll(const char *dll) {
  static const char *sys[] = { "kernel32", "kernelbase", "ntdll", "user32", "gdi32", "msvcrt", "ucrtbase", "vcruntime140", "vcruntime140_1", "msvcp140", "msvcp140_1", "msvcp140_2", "msvcp140_atomic_wait", "msvcp140_codecvt_ids", "concrt140",
    "advapi32", "shell32", "ole32", "oleaut32", "ws2_32", "wsock32", "comctl32", "comdlg32", "shlwapi", "psapi", "winmm", "imm32", "version", "crypt32", "secur32", "bcrypt", "setupapi", "dbghelp", "iphlpapi", "userenv", "uxtheme", "dwmapi",
    "dxgi", "opengl32", "glu32", "dinput8", "dsound", "wininet", "winhttp", "mpr", "netapi32", "rpcrt4", "wintrust", "powrprof", "hid", "cfgmgr32", "normaliz", "msimg32", "winspool", "avrt", "mmdevapi", "msacm32", "ncrypt", "wtsapi32", "propsys", "shcore", "api-ms", "ext-ms", "d3d", "xinput", "xaudio2", "gdiplus", "msvcr", "msvcp", "libgcc_s_seh-1", "libstdc++-6", "libwinpthread-1", 0 };
  char b[64]; int n = 0; for (const char *p = dll; *p && n < 63; p++) { char ch = *p; if (ch >= 'A' && ch <= 'Z') ch += 32; b[n++] = ch; } b[n] = 0;
  if (n > 4 && !e_strcmp(b + n - 4, ".dll")) b[n - 4] = 0; if (n > 4 && !e_strcmp(b + n - 4, ".drv")) b[n - 4] = 0;
  for (int i = 0; sys[i]; i++) { size_t l = e_strlen(sys[i]); if (!e_strcmp(b, sys[i])) return 1; if ((!e_strcmp(sys[i], "api-ms") || !e_strcmp(sys[i], "ext-ms") || !e_strcmp(sys[i], "d3d") || !e_strcmp(sys[i], "xinput") || !e_strcmp(sys[i], "xaudio2") || !e_strcmp(sys[i], "msvcr") || !e_strcmp(sys[i], "msvcp")) && !e_strncmp(b, sys[i], l)) return 1; }
  return 0;
}
/* "Native over builtin" (the Wine notion): for these system DLLs a real file on the virtual disk (next to the exe, in the
 * DLL search path or in C:\Windows\System32 — e.g. from the VC++ redistributable) is preferred over the shims, because
 * they are pure user-mode libraries that run fine on top of kernel32/ucrt shims: the C++ standard library (msvcp140*),
 * the older CRTs (msvcr100/110/120 + msvcp100/110/120), concrt/vcomp/mfc, vcruntime140_1 (__CxxFrameHandler4), the
 * mingw runtime DLLs, GDI+ and the Direct3D shader compilers. Core OS DLLs (kernel32, ntdll, user32, ws2_32, ucrtbase,
 * vcruntime140, api-ms-*, d3d11/dxgi …) are always the emulator's own. */
int pe_dll_native_ok(const char *dll) {
  static const char *pref[] = { "msvcp", "msvcr", "concrt", "vcomp", "vcamp", "vccorlib", "mfc", "vcruntime140_1", "vcruntime140_threads", "libgcc_s", "libstdc++", "libwinpthread", "gdiplus", "d3dcompiler_", "d3dx9_", "d3dx10_", "d3dx11_", "d3dcsx", "x3daudio", "xaudio2_", "xapofx", "xactengine", 0 };
  char b[64]; int n = 0; for (const char *p = dll; *p && n < 63; p++) { char ch = *p; if (ch >= 'A' && ch <= 'Z') ch += 32; b[n++] = ch; } b[n] = 0;
  for (int i = 0; pref[i]; i++) if (!e_strncmp(b, pref[i], e_strlen(pref[i]))) return 1;
  return 0;
}

/* DLLs that only exist as real files: the Direct3D shader compilers and D3DX/D3DCSX/X3DAudio helpers of the DirectX End-User
 * Runtime ("DirectX pack", installed on demand into C:\Windows\System32). The emulator has no shims for them, so without
 * the file LoadLibrary fails with ERROR_MOD_NOT_FOUND instead of handing out a module whose every GetProcAddress is NULL. */
int pe_dll_native_only(const char *dll) {
  static const char *pref[] = { "d3dcompiler_", "d3dx9_", "d3dx10_", "d3dx11_", "d3dcsx", "x3daudio", "xaudio2_", "xapofx", "xactengine", 0 };
  char b[64]; int n = 0; for (const char *p = dll; *p && n < 63; p++) { char ch = *p; if (ch >= 'A' && ch <= 'Z') ch += 32; b[n++] = ch; } b[n] = 0;
  for (int i = 0; pref[i]; i++) if (!e_strncmp(b, pref[i], e_strlen(pref[i]))) return 1;
  return 0;
}

/* ---------- import helper shared by loader and inspector ---------- */
typedef void (*ImportCb)(void *ctx, const char *dll, const char *name, u32 ordinal, u32 iat_rva);
static int pe_walk_imports(PeHdr *h, ImportCb cb, void *ctx) {
  u32 rva = h->dd[1][0]; if (!rva) return 0;
  int total = 0;
  for (u32 off = 0;; off += 20) {
    const u8 *e = pe_rva(h, rva + off, 20); if (!e) break;
    u32 ilt = le32(e), name = le32(e + 12), iat = le32(e + 16);
    if (!name && !iat) break;
    const u8 *dn = pe_rva(h, name, 1); const char *dll = dn ? (const char *)dn : "?";
    u32 thunk = ilt ? ilt : iat; int esz = h->magic == 0x20b ? 8 : 4;
    for (u32 k = 0;; k++) {
      const u8 *t = pe_rva(h, thunk + k * esz, esz); if (!t) break;
      u64 v = esz == 8 ? le64(t) : le32(t); if (!v) break;
      int by_ord = esz == 8 ? (v >> 63) : (v >> 31);
      if (by_ord) cb(ctx, dll, 0, (u32)(v & 0xffff), iat + k * esz);
      else { const u8 *hn = pe_rva(h, (u32)(v & 0x7fffffff) + 2, 1); cb(ctx, dll, hn ? (const char *)hn : "?", 0, iat + k * esz); }
      total++;
      if (k > 100000) break;
    }
  }
  return total;
}

/* ---------- loader ---------- */
u64 w32_data_import(Win32 *w, const char *name);
int w32_data_import_off(const char *name);
u64 w32_resolve_import(Win32 *w, const char *dll, const char *name, u32 ordinal);
typedef struct { Win32 *w; u8 *img; u64 base; int unknown; } LoadCtx;
static void load_import(void *ctx, const char *dll, const char *name, u32 ordinal, u32 iat_rva) {
  LoadCtx *lc = ctx; char tmp[32]; const char *n = name;
  if (!n) { e_snprintf(tmp, sizeof tmp, "#%u", ordinal); n = tmp; }
  u64 slot = w32_data_import(lc->w, n); if (!slot) slot = w32_resolve_import(lc->w, dll, n, ordinal);
  u8 *p = lc->img + iat_rva; p[0] = slot; p[1] = slot >> 8; p[2] = slot >> 16; p[3] = slot >> 24; if (!lc->w->mode32) { p[4] = slot >> 32; p[5] = slot >> 40; p[6] = slot >> 48; p[7] = slot >> 56; }
}

/* static TLS: one slot per image in the per-thread pointer array (TEB+0x58 → arr[idx]); every live thread gets its own block */
void pe_tls_thread_init(Win32 *w, int th) {
  Cpu *c = w->cpu; u64 arr = w->th[th].tls_arr; if (!arr) return;
  for (int i = 0; i < w->ntls; i++) {
    if (w->th[th].tls_blk[i]) continue;
    u64 blk = w32_heap_alloc(w, w->tls[i].blk_size, 1); if (!blk) return;
    if (w->tls[i].tmpl_size) { const u8 *src = mem_ptr(w->mem, w->tls[i].tmpl, w->tls[i].tmpl_size, 0); u8 *dst = mem_ptr(w->mem, blk, w->tls[i].tmpl_size, 1); if (src && dst) e_memcpy(dst, src, (size_t)w->tls[i].tmpl_size); }
    wrp(c, arr + (w->mode32 ? 4 : 8) * i, blk); w->th[th].tls_blk[i] = blk;
  }
}
static int pe_tls_register(Win32 *w, u64 raw_start, u64 raw_end, u32 zero, u64 idx_addr) {
  if (w->ntls >= MAX_TLS_IMAGES) return -1;
  int idx = w->ntls++; w->tls[idx].tmpl = raw_start; w->tls[idx].tmpl_size = raw_end > raw_start ? raw_end - raw_start : 0; w->tls[idx].blk_size = w->tls[idx].tmpl_size + zero + 16;
  if (idx_addr) wr32(w->cpu, idx_addr, (u32)idx);
  for (int t = 0; t < THREAD_MAX; t++) if (w->th[t].used) pe_tls_thread_init(w, t);
  return idx;
}

/* map an image (exe or DLL) into the process: sections, relocations, imports. Returns 0 or a negative code (reported). */
static int pe_map(Win32 *w, const u8 *bytes, size_t len, const char *name, int want_dll, PeHdr *hp, u64 *pbase, u64 *psize) {
  PeHdr h; int rc = pe_parse(&h, bytes, len);
  if (rc) { w32_report(w, "%s: not a PE file (code %d)", name, rc); return -1; }
  /* managed (.NET) images are mapped as data for the CLR, which loads them through LoadLibraryExW and reads the metadata from
     the mapped image: x64 PE32+ assemblies and AnyCPU (x86 PE32, IL-only) assemblies; their entry stub is never executed */
  int managed = h.dd[14][1] != 0, il_only = 0;
  if (managed) { const u8 *ch = pe_rva(&h, h.dd[14][0], 24); if (ch) il_only = le32(ch + 16) & 1; }
  int is32 = h.machine == 0x14c && h.magic == 0x10b;
  if (!want_dll && is32 && !managed) { extern void w32_enter_mode32(Win32 *w); w32_enter_mode32(w); }   /* 32-bit executable → 32-bit process */
  if (w->mode32) {
    if (!is32) { w32_report(w, "%s: a 32-bit process cannot load this %s image", name, h.machine == 0x8664 ? "x86-64" : "non-x86"); return -2; }
  } else {
    if (h.machine != 0x8664 && !(managed && il_only && h.machine == 0x14c)) { w32_report(w, "%s: unsupported machine 0x%x (x86-64 and x86 executables are emulated; this one is neither)", name, h.machine); return -2; }
    if (h.magic != 0x20b && !(managed && il_only && h.magic == 0x10b)) { w32_report(w, "%s: not PE32+", name); return -2; }
  }
  if (!want_dll && (h.chars & 0x2000)) { w32_report(w, "file is a DLL, not an executable"); return -3; }
  if (want_dll && !(h.chars & 0x2000) && !managed) { w32_report(w, "%s: not a DLL", name); return -3; }
  if (managed && !want_dll) { w32_report(w, "%s: .NET assembly — start it through the .NET runtime (dotnet.exe)", name); return -4; }
  u64 base = h.image_base; u64 size = (h.size_image + 0xffff) & ~0xffffull;
  /* collide with our fixed regions or with something already mapped? relocate */
  int collide = (base < G_SHIM_BASE + G_SHIM_SIZE && base + size > G_SHIM_BASE) || (base < G_STACK_BASE + G_STACK_SIZE && base + size > G_STACK_BASE) || (base < G_HEAP_BASE + 0x40000000ull && base + size > G_HEAP_BASE);
  if (!collide) for (u64 a = base; a < base + size; a += 0x10000) if (mem_region(w->mem, a)) { collide = 1; break; }
  if (managed && want_dll) collide = 1;
  if (base == 0 || collide) {
    if (!h.dd[5][1] && !(h.dllchars & 0x40) && !managed) { w32_report(w, "%s: image base 0x%llx collides and the file has no relocations", name, (unsigned long long)base); return -5; }
    if (w->mode32) {   /* everything below 2 GB: exe at 0x00400000 (or next free), DLLs from 0x60000000 upward (0x10000000 is our stack) */
      if (!want_dll) { base = 0x00400000; while (base + size <= 0x10000000 && mem_region(w->mem, base)) base += 0x100000; }
      else { if (!w->dll_next32) w->dll_next32 = 0x60000000ull; base = w->dll_next32; w->dll_next32 += size + 0x10000; }
    }
    else if (!want_dll) { base = 0x0000000140000000ull; if (base < G_HEAP_BASE + 0x40000000ull) base = 0x0000000180000000ull; }
    else { if (!w->dll_next) w->dll_next = 0x0000000190000000ull; base = w->dll_next; w->dll_next += size + 0x10000; }
  }
  u8 *img = mem_map(w->mem, base, size, want_dll ? "dll" : "image");
  if (!img) { w32_report(w, "%s: cannot map image (%llu bytes)", name, (unsigned long long)size); return -6; }
  e_memcpy(img, bytes, h.size_headers < len ? h.size_headers : len);
  for (int i = 0; i < h.nsec; i++) {
    const u8 *s = bytes + h.sec_off + 40 * i; u32 vs = le32(s + 8), va = le32(s + 12), rs = le32(s + 16), rp = le32(s + 20);
    if (va >= size) continue;
    u32 n = rs; if (vs && vs < n) n = vs; if ((u64)va + n > size) n = (u32)(size - va); if ((size_t)rp + n > len) n = rp < len ? (u32)(len - rp) : 0;
    e_memcpy(img + va, bytes + rp, n);
  }
  /* relocations */
  s64 delta = (s64)(base - h.image_base);
  if (delta && h.dd[5][1]) {
    u32 r = h.dd[5][0], end = r + h.dd[5][1];
    while (r + 8 <= end && r + 8 <= size) {
      u32 page = le32(img + r), bsz = le32(img + r + 4); if (bsz < 8) break;
      for (u32 k = 8; k + 2 <= bsz; k += 2) {
        u16 e = le16(img + r + k); u32 type = e >> 12, off = page + (e & 0xfff);
        if (type == 10 && off + 8 <= size) { u64 v = le64(img + off) + (u64)delta; e_memcpy(img + off, &v, 8); }
        else if (type == 3 && off + 4 <= size) { u32 v = le32(img + off) + (u32)delta; e_memcpy(img + off, &v, 4); }
      }
      r += bsz;
    }
  }
  if (!want_dll) { w->exe_exp_rva = h.dd[0][0]; w->exe_exp_size = h.dd[0][1]; }
  /* register before resolving imports so that circular DLL dependencies terminate */
  if (want_dll) {
    if (w->nmods >= MAX_MODULES) { w32_report(w, "%s: too many DLLs (max %d)", name, MAX_MODULES); return -7; }
    GuestModule *m = &w->mods[w->nmods++]; e_memset(m, 0, sizeof *m);
    const char *bn = name; for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/') bn = p + 1;
    e_snprintf(m->name, sizeof m->name, "%s", bn); m->base = base; m->size = size; m->entry = h.entry && !managed ? base + h.entry : 0; m->exp_rva = h.dd[0][0]; m->exp_size = h.dd[0][1]; m->pdata_rva = h.dd[3][0]; m->pdata_size = h.dd[3][1];
    if (managed) m->attached = 1;   /* nothing to run: the CLR takes it from here */
  }
  /* imports → shim slots or exports of other loaded DLLs (a pure managed image only imports mscoree!_CorDllMain for its stub) */
  LoadCtx lc = { w, img, base, 0 };
  if (!managed) pe_walk_imports(&h, load_import, &lc);
  *hp = h; *pbase = base; *psize = size; return 0;
}
int pe_load_dll(Win32 *w, const u8 *bytes, size_t len, const char *name) {
  PeHdr h; u64 base, size; int before = w->nmods;
  if (pe_map(w, bytes, len, name, 1, &h, &base, &size)) return -1;
  if (h.dd[9][0] && h.dd[9][0] + 40 <= size) {   /* DLL static TLS: register a slot (mingw DLLs carry one, MSVC DLLs with thread_local too) */
    const u8 *t = mem_ptr(w->mem, base + h.dd[9][0], 40, 0);
    if (t) { u64 rs, re, idx_addr; u32 zero; if (w->mode32) { rs = le32(t); re = le32(t + 4); idx_addr = le32(t + 8); zero = le32(t + 16); } else { rs = le64(t); re = le64(t + 8); idx_addr = le64(t + 16); zero = le32(t + 32); } if (pe_tls_register(w, rs, re, zero, idx_addr) < 0) w32_report(w, "%s: too many images with static TLS (max %d)", name, MAX_TLS_IMAGES); }
  }
  return before;   /* index of the module registered by pe_map */
}
int pe_load(Win32 *w, const u8 *bytes, size_t len, const char *name) {
  PeHdr h; u64 base, size; int rc = pe_map(w, bytes, len, name, 0, &h, &base, &size); if (rc) return rc;
  u8 *img = mem_ptr(w->mem, base, size, 1); if (!img) return -6;
  w->image_base = base; w->image_size = size; w->entry = base + h.entry; w->subsystem = h.subsystem; w->exe_pdata_rva = h.dd[3][0]; w->exe_pdata_size = h.dd[3][1];
  e_snprintf(w->exe_name, sizeof w->exe_name, "%s", name);
  /* TLS: register the template (slot 0 for the exe), build the main thread's block */
  Cpu *c = w->cpu;
  if (h.dd[9][0] && h.dd[9][0] + 40 <= size) {
    const u8 *t = img + h.dd[9][0];
    u64 raw_start, raw_end, idx_addr, cb_addr; u32 zero;
    if (w->mode32) { raw_start = le32(t); raw_end = le32(t + 4); idx_addr = le32(t + 8); cb_addr = le32(t + 12); zero = le32(t + 16); }
    else { raw_start = le64(t); raw_end = le64(t + 8); idx_addr = le64(t + 16); cb_addr = le64(t + 24); zero = le32(t + 32); }
    pe_tls_register(w, raw_start, raw_end, zero, idx_addr);
    /* callbacks: stored for the start-up sequence */
    if (cb_addr) { for (int k = 0; k < 16; k++) { u64 fn = rdp(c, cb_addr + k * PSZ); if (!fn) break; w->tls_callbacks[w->n_tls_cb++] = fn; } }
  }
  if (w->mode32) wr32(c, w->peb + 0x08, (u32)base); else wr64(c, w->peb + 0x10, base);
  return 0;
}

/* ---------- static inspection ---------- */
typedef struct { char *out; size_t sz, pos; int n, known, unknown, priv, pack; char dlls[64][48]; int ndll; int unknown_listed; } InsCtx;
static void ins_put(InsCtx *x, const char *fmt, ...) { if (x->pos >= x->sz) return; __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = e_vsnprintf(x->out + x->pos, x->sz - x->pos, fmt, ap); __builtin_va_end(ap); x->pos += n; if (x->pos > x->sz - 1) x->pos = x->sz - 1; }
int w32_have_shim(const char *dll, const char *name);
static void ins_import(void *ctx, const char *dll, const char *name, u32 ordinal, u32 iat_rva) {
  (void)iat_rva; InsCtx *x = ctx; x->n++;
  char tmp[32]; if (!name) { e_snprintf(tmp, sizeof tmp, "#%u", ordinal); name = tmp; }
  int found = 0; for (int i = 0; i < x->ndll; i++) if (!e_stricmp(x->dlls[i], dll)) { found = 1; break; }
  if (!found && x->ndll < 64) { e_snprintf(x->dlls[x->ndll++], 48, "%s", dll); }
  if (w32_have_shim(dll, name) || (name && w32_data_import_off(name))) x->known++;
  else if (!pe_is_system_dll(dll)) { x->priv++; x->known++; }   /* resolved from the private DLL next to the exe at load time */
  else if (pe_dll_native_only(dll)) { x->pack++; x->known++; }   /* real DLL from the DirectX pack (installed on demand) */
  else { x->unknown++; if (x->unknown_listed < 200) { ins_put(x, "%s%s!%s", x->unknown_listed ? ", " : "", dll, name); x->unknown_listed++; } else if (x->unknown_listed == 200) { ins_put(x, ", ..."); x->unknown_listed++; } }
}
int pe_inspect(const u8 *bytes, size_t len, char *out, size_t out_sz) {
  PeHdr h; int rc = pe_parse(&h, bytes, len);
  InsCtx x; e_memset(&x, 0, sizeof x); x.out = out; x.sz = out_sz;
  if (rc) { ins_put(&x, "{\"ok\":false,\"error\":\"not a PE executable (code %d)\"}", rc); return -1; }
  const char *arch = h.machine == 0x8664 ? "x86-64" : h.machine == 0x14c ? "x86 (32-bit)" : h.machine == 0xaa64 ? "ARM64" : h.machine == 0x1c0 ? "ARM" : "unknown";
  const char *sub = h.subsystem == 2 ? "GUI" : h.subsystem == 3 ? "console" : "other";
  int is_dll = !!(h.chars & 0x2000), is_clr = !!h.dd[14][1], il_only = 0;
  if (is_clr) { const u8 *ch = pe_rva(&h, h.dd[14][0], 24); if (ch) il_only = le32(ch + 16) & 1; }
  int mk = pe_managed_kind(bytes, len), clr_entry = 0;
  if (is_clr) { const u8 *ch = pe_rva(&h, h.dd[14][0], 24); if (ch) clr_entry = le32(ch + 20) != 0; }   /* EntryPointToken: an application, not a library */
  ins_put(&x, "{\"ok\":true,\"dotnet\":%s,", mk == 1 ? (clr_entry ? "\"managed\"" : "\"managed-dll\"") : mk == 2 ? "\"apphost\"" : "null");
  ins_put(&x, "\"arch\":\"%s\",\"machine\":%u,\"pe32plus\":%s,\"subsystem\":\"%s\",\"dll\":%s,\"clr\":%s,\"imageBase\":\"0x%llx\",\"sizeOfImage\":%u,\"entry\":\"0x%x\",\"sections\":%u,\"relocs\":%s,\"tls\":%s,\"exceptions\":%s,\"resources\":%u,",
    arch, h.machine, h.magic == 0x20b ? "true" : "false", sub, is_dll ? "true" : "false", is_clr ? "true" : "false", (unsigned long long)h.image_base, h.size_image, h.entry, h.nsec,
    h.dd[5][1] ? "true" : "false", h.dd[9][1] ? "true" : "false", h.dd[3][1] ? "true" : "false", h.dd[2][1]);
  /* section names */
  ins_put(&x, "\"sectionNames\":[");
  for (int i = 0; i < h.nsec && i < 32; i++) { const u8 *s = bytes + h.sec_off + 40 * i; char nm[9]; e_memcpy(nm, s, 8); nm[8] = 0; for (int k = 0; k < 8; k++) if (nm[k] == '"' || nm[k] == '\\') nm[k] = '_'; ins_put(&x, "%s\"%s\"", i ? "," : "", nm); }
  ins_put(&x, "],\"unknownImports\":\"");
  size_t mark = x.pos;
  pe_walk_imports(&h, ins_import, &x);
  (void)mark;
  ins_put(&x, "\",\"imports\":%d,\"knownImports\":%d,\"unknownImportsCount\":%d,\"privateDllImports\":%d,\"dlls\":[", x.n, x.known, x.unknown, x.priv);
  for (int i = 0; i < x.ndll; i++) ins_put(&x, "%s\"%s\"", i ? "," : "", x.dlls[i]);
  /* verdict */
  const char *verdict; const char *why;
  if (is_clr && (h.machine == 0x8664 || il_only) && !clr_entry) { verdict = "partial"; why = ".NET library (not an application) — usable by .NET programs; it has no entry point to run"; }
  else if (is_clr && (h.machine == 0x8664 || il_only)) { verdict = "likely"; why = ".NET assembly — runs on the official .NET runtime (the .NET pack, installed on demand); classic .NET Framework console apps run on it in compatibility mode"; }
  else if (is_clr) { verdict = "unsupported"; why = "mixed-mode .NET image (native + managed) — not supported"; }
  else if (mk == 2) { verdict = "likely"; why = ".NET apphost launcher — starts <name>.dll next to it on the official .NET runtime (the .NET pack, installed on demand)"; }
  else if (h.machine != 0x8664 && h.machine != 0x14c) { verdict = "unsupported"; why = "only x86-64 (PE32+) and x86 (PE32) code is emulated; ARM binaries are rejected"; }
  else if (h.machine == 0x14c && h.magic != 0x10b) { verdict = "unsupported"; why = "malformed PE32 header"; }
  else if (is_dll) { verdict = "unsupported"; why = "DLL, not an executable"; }
  else {
    int heavy = 0, d3d11 = 0; for (int i = 0; i < x.ndll; i++) { const char *d = x.dlls[i];
      if (!e_stricmp(d, "d3d11.dll") || !e_stricmp(d, "dxgi.dll")) { d3d11 = 1; continue; }   /* Direct3D 11 has a (partial) WebGL2 translation layer */
      if (!e_strncmp(d, "d3d", 3) || !e_strncmp(d, "D3D", 3) || !e_stricmp(d, "opengl32.dll") || !e_stricmp(d, "dxgi.dll") || !e_stricmp(d, "vulkan-1.dll") || !e_stricmp(d, "xinput1_4.dll") || !e_stricmp(d, "dsound.dll") || !e_stricmp(d, "xaudio2_9.dll") || !e_stricmp(d, "mono-2.0-bdwgc.dll") || !e_stricmp(d, "UnityPlayer.dll") || !e_stricmp(d, "GameAssembly.dll")) heavy = 1; }
    if (heavy) { verdict = "unsupported"; why = "needs GPU/audio subsystems (Direct3D 9/12, OpenGL, Vulkan, XAudio, Unity/Mono) that have no wasm64 implementation here"; }
    else if (d3d11) { verdict = "partial"; why = "Direct3D 11 through the WebGL2 translation layer (device, swap chain, buffers, textures, SM4/5 vertex + pixel shaders, Draw/DrawIndexed); compute/geometry/tessellation and many state paths are not implemented and stop with the method name"; }
    else if (x.unknown == 0 && x.pack) { verdict = "partial"; why = x.priv ? "needs DLLs from the DirectX pack (D3DCompiler/D3DX, installed on demand into C:\\Windows\\System32) and its own DLLs next to the exe; the rest of the imports have shims" : "needs DLLs from the DirectX pack (D3DCompiler/D3DX — installed on demand into C:\\Windows\\System32); the rest of the imports have shims"; }
    else if (x.unknown == 0 && x.priv) { verdict = "partial"; why = "all system imports have shims; the program also imports its own DLLs, which are loaded from the virtual disk if you add them next to the exe"; }
    else if (x.unknown == 0) { verdict = "likely"; why = "all imports have shims"; }
    else if (x.unknown * 5 <= x.n * 2) { verdict = "partial"; why = "most imports have shims (no network/GPU/audio); the program runs until the first unimplemented call"; }
    else { verdict = "unlikely"; why = "most imports are not implemented"; }
  }
  ins_put(&x, "],\"verdict\":\"%s\",\"why\":\"%s\"}", verdict, why);
  return 0;
}
