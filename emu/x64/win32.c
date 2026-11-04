/* Win32 personality: import trampolines, process environment, kernel32 + UCRT/msvcrt shims, guest callbacks. */
#include "emu.h"
#include <stdarg.h>

#define ARG(n) w32_arg(c, n)
#define RET(v) (c->r[RAX] = (u64)(v))
#define W (c->w)
#define PSZ_OF(cpu) ((cpu)->mode32 ? 4 : 8)
/* OVERLAPPED: Internal@0 InternalHigh@PSZ Offset@2*PSZ OffsetHigh@2*PSZ+4 hEvent@2*PSZ+8 (x64: 0/8/16/20/24, x86: 0/4/8/12/16) */
#define OV_HIGH(ov) ((ov) + PSZ_OF(c))
#define OV_OFFSET(ov) ((ov) + 2 * PSZ_OF(c))
#define OV_EVENT(ov) ((ov) + 2 * PSZ_OF(c) + 8)
static inline void ov_set(Cpu *c, u64 ov, u64 status, u64 info) { wrp(c, ov, status); wrp(c, ov + PSZ_OF(c), info); }
#define IOB_STRIDE (c->mode32 ? 0x20 : 0x30)   /* FILE records of stdin/stdout/stderr: 32-bit msvcrt FILE is 32 bytes */
/* extra guest environment variables ("NAME=value"), added by the host before the exe starts (x64run: X64_GUEST_ENV="A=1;B=2") */
static char g_extra_env_buf[32][2048]; static const char *g_extra_env[32]; static int g_nextra_env;
void w32_add_env(const char *kv) { if (g_nextra_env < 32 && kv && kv[0]) { e_snprintf(g_extra_env_buf[g_nextra_env], sizeof g_extra_env_buf[0], "%s", kv); g_extra_env[g_nextra_env] = g_extra_env_buf[g_nextra_env]; g_nextra_env++; } }

enum { CONT_STARTUP = 1, CONT_INITTERM, CONT_ATEXIT, CONT_RET_RAX, CONT_RET_VAL, CONT_CREATEWND, CONT_QSORT, CONT_BSEARCH, CONT_DLLMAIN };
static int attach_next_module(Cpu *c);
enum { SLOT_STARTUP = 0, SLOT_CONT = 1, SLOT_FIRST = 2 };
#define SLOT_ADDR(i) (G_SHIM_BASE + (u64)(i) * SHIM_SLOT_SIZE)
#define TEB_OFF   0x10000
#define PEB_OFF   0x14000
#define PARAMS_OFF 0x15000
#define STR_OFF   0x16000

static u64 g_a32[16]; static int g_a32_n;
static const char *shim_canonical(const char *dll, const char *name);
static const char *sig32_lookup(const char *name); static int clean32_of(const char *name, const char *sig);
u64 w32_arg(Cpu *c, int n) {
  if (c->mode32) { if (n < g_a32_n) return g_a32[n]; return rd32(c, c->r[RSP] + 4u * (u32)n); }   /* 32-bit: all arguments on the stack, esp → first argument (the return address was popped by the dispatcher) */
  switch (n) { case 0: return c->r[RCX]; case 1: return c->r[RDX]; case 2: return c->r[R8]; case 3: return c->r[R9]; default: return rd64(c, c->r[RSP] + 0x20 + (u64)(n - 4) * 8); }
}
static u64 shim_bump(Win32 *w, u64 n) { n = (n + 15) & ~15ull; u64 a = w->str_cur; if (a + n > G_SHIM_BASE + G_SHIM_SIZE) return 0; w->str_cur += n; return a; }
u64 w32_guest_strdup(Win32 *w, const char *s) { size_t n = e_strlen(s) + 1; u64 a = w32_heap_alloc(w, n, 0); u8 *p = mem_ptr(w->mem, a, n, 1); if (p) e_memcpy(p, s, n); return a; }
/* UTF-8 host string → UTF-16 guest string (command line, argv, environment) */
static u64 guest_wstrdup(Win32 *w, const char *s) { size_t n = e_strlen(s) + 1; u64 a = w32_heap_alloc(w, n * 2 + 2, 1); size_t i = 0, o = 0;
  while (i < n) { u32 b = (u8)s[i++], cp = b; int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0; if (extra) cp = b & (0x3f >> extra);
    for (int k = 0; k < extra && i < n && ((u8)s[i] & 0xC0) == 0x80; k++) cp = (cp << 6) | ((u8)s[i++] & 0x3f);
    if (cp >= 0x10000) { cp -= 0x10000; wr16(w->cpu, a + o++ * 2, (u16)(0xD800 | (cp >> 10))); wr16(w->cpu, a + o++ * 2, (u16)(0xDC00 | (cp & 0x3ff))); } else wr16(w->cpu, a + o++ * 2, (u16)cp); }
  return a; }

void w32_report(Win32 *w, const char *fmt, ...) {
  if (w->report_len >= (int)sizeof w->report - 2) return;
  va_list ap; va_start(ap, fmt); int n = e_vsnprintf(w->report + w->report_len, sizeof w->report - w->report_len - 1, fmt, ap); va_end(ap);
  w->report_len += n; if (w->report_len > (int)sizeof w->report - 2) w->report_len = sizeof w->report - 2;
  w->report[w->report_len++] = '\n'; w->report[w->report_len] = 0;
}

/* ---------- heap ----------
   Every block carries a 16-byte header in guest memory (payload size, magic, free flag); freed blocks are kept on
   size-class free lists (exact 16-byte classes up to 1 KB, power-of-two classes above), so malloc/free/realloc are
   O(1) however many blocks a program keeps alive (busybox sort of 10^5 lines allocates 2·10^5 blocks). */
#define HB_MAGIC 0x4E424C4Bu
#define HB_FREE  0x46524545u
static int heap_grow(Win32 *w, u64 need) {
  u64 sz = need > (16ull << 20) ? ((need + 0xffff) & ~0xffffull) : (16ull << 20);
  if (w->mode32 && w->heap_end + sz > 0x48000000ull) return 0;   /* 32-bit process: the heap ends where reservations begin */
  if (!mem_map(w->mem, w->heap_end, sz, "heap")) return 0;
  w->heap_end += sz; return 1;
}
#define HEAP_HUGE (16ull << 20)
/* size class: exact 16-byte classes up to 1 KB; power-of-two classes (blocks are rounded to exactly 2^k) up to 16 MB; one "huge" list above */
static int heap_bin(u64 size) { if (size <= 1024) return (int)(size / 16); if (size > HEAP_HUGE) return HEAP_BINS - 1; return 64 + (64 - __builtin_clzll(size - 1)); }
static u64 *heap_hdr(Win32 *w, u64 addr) { if (addr < G_HEAP_BASE + 16 || addr >= w->heap_end || (addr & 15)) return 0; u8 *p = mem_ptr(w->mem, addr - 16, 16, 1); if (!p) return 0; u64 *h = (u64 *)p; if ((u32)h[1] != HB_MAGIC) return 0; return h; }
static u64 heap_next(Win32 *w, u64 a) { u8 *p = mem_ptr(w->mem, a, 8, 0); u64 n = 0; if (p) e_memcpy(&n, p, 8); return n; }
static void heap_set_next(Win32 *w, u64 a, u64 n) { u8 *p = mem_ptr(w->mem, a, 8, 1); if (p) e_memcpy(p, &n, 8); }
u64 w32_heap_alloc(Win32 *w, u64 size, int zero) {
  if (!size) size = 1; size = (size + 15) & ~15ull;
  if (size > 1024 && size <= HEAP_HUGE) size = 1ull << (64 - __builtin_clzll(size - 1));
  int bin = heap_bin(size);
  /* reuse a freed block of the same class (huge list: first fit) */
  for (u64 prev = 0, a = w->bins[bin]; a; prev = a, a = heap_next(w, a)) {
    u64 *h = heap_hdr(w, a); if (!h) { if (prev) heap_set_next(w, prev, 0); else w->bins[bin] = 0; break; }
    if (h[0] < size) continue;
    u64 next = heap_next(w, a); if (prev) heap_set_next(w, prev, next); else w->bins[bin] = next;
    h[1] = HB_MAGIC; if (zero) { u8 *q = mem_ptr(w->mem, a, h[0], 1); if (q) e_memset(q, 0, (size_t)h[0]); } return a;
  }
  if (w->heap_cur + 16 + size > w->heap_end) {
    /* a block must live inside ONE region (host memory is per region): skip the tail of the old region */
    u64 old_end = w->heap_end;
    if (!heap_grow(w, size + 16)) return 0;
    w->heap_cur = old_end;
  }
  u64 a = w->heap_cur + 16; w->heap_cur += 16 + size;
  u8 *p = mem_ptr(w->mem, a - 16, 16 + size, 1); if (!p) return 0;
  u64 hdr[2] = { size, HB_MAGIC }; e_memcpy(p, hdr, 16); if (zero) e_memset(p + 16, 0, (size_t)size);
  return a;
}
int w32_heap_free(Win32 *w, u64 addr) {
  u64 *h = heap_hdr(w, addr); if (!h || (h[1] >> 32) == HB_FREE) return 0;
  int bin = heap_bin(h[0]);
  heap_set_next(w, addr, w->bins[bin]); w->bins[bin] = addr; h[1] = HB_MAGIC | ((u64)HB_FREE << 32); return 1;
}
u64 w32_heap_size(Win32 *w, u64 addr) { u64 *h = heap_hdr(w, addr); return h && (h[1] >> 32) != HB_FREE ? h[0] : 0; }
static u64 heap_realloc(Win32 *w, u64 addr, u64 size, int zero) {
  if (!addr) return w32_heap_alloc(w, size, zero);
  u64 old = w32_heap_size(w, addr); if (!old) return 0;
  if (old >= size) return addr;
  u64 n = w32_heap_alloc(w, size, zero); if (!n) return 0;
  u8 *s = mem_ptr(w->mem, addr, old, 0), *d = mem_ptr(w->mem, n, old, 1); if (s && d) e_memcpy(d, s, (size_t)old);
  w32_heap_free(w, addr); return n;
}

/* ---------- slots ---------- */
int w32_have_shim(const char *dll, const char *name);
static ShimFn find_shim(const char *dll, const char *name);
u64 w32_shim_slot(Win32 *w, const char *dll, const char *name) {
  for (int i = SLOT_FIRST; i < w->nslots; i++) if (!e_strcmp(w->slots[i].name, name) && !e_stricmp(w->slots[i].dll, dll)) return SLOT_ADDR(i);
  if (w->nslots >= MAX_SHIMS) return 0;
  int i = w->nslots++;
  size_t ln = e_strlen(name) + 1, ld = e_strlen(dll) + 1; char *n = emu_alloc(ln + ld); e_memcpy(n, name, ln); e_memcpy(n + ln, dll, ld);
  w->slots[i].name = n; w->slots[i].dll = n + ln; w->slots[i].fn = find_shim(dll, name); w->slots[i].calls = 0;
  { const char *cn = shim_canonical(dll, name); w->slots[i].sig32 = sig32_lookup(cn); w->slots[i].clean32 = (u16)clean32_of(cn, w->slots[i].sig32); }
  return SLOT_ADDR(i);
}

/* data imports (variables exported by msvcrt/ucrt) → real guest storage instead of a code slot */
int w32_data_import_off(const char *name) {
  static const struct { const char *n; u32 off; } tab[] = {
    { "__argc", 0x200 }, { "__argv", 0x208 }, { "__wargv", 0x218 }, { "_environ", 0x228 }, { "__initenv", 0x228 }, { "_wenviron", 0x230 }, { "__winitenv", 0x230 },
    { "_fmode", 0x240 }, { "_commode", 0x248 }, { "_pgmptr", 0x250 }, { "_wpgmptr", 0x258 }, { "_iob", 0x000 }, { "__mb_cur_max", 0x268 }, { "_timezone", 0x270 }, { "_daylight", 0x274 }, { "_tzname", 0x278 },
    { "_sys_nerr", 0x288 }, { "_sys_errlist", 0x290 }, { "_HUGE", 0x298 }, { "_acmdln", 0x220 }, { "_wcmdln", 0x210 }, { "__lc_codepage", 0x2a0 }, { "_pctype", 0x2a8 }, { "_osplatform", 0x2b0 }, { "_osver", 0x2b4 }, { "_winmajor", 0x2b8 }, { "_winminor", 0x2bc }, { "_winver", 0x2c0 }, { "_adjust_fdiv", 0x2c4 },
  };
  for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!e_strcmp(tab[i].n, name)) return (int)tab[i].off + 1;
  return 0;
}
u64 w32_data_import(Win32 *w, const char *name) { int o = w32_data_import_off(name); return o ? w->ubuf + o - 1 : 0; }

/* TEB of a thread: x64 layout (gs:) or the 32-bit layout (fs:) when the process is PE32 */
static void teb_init(Win32 *w, Cpu *c, u64 teb, u64 stack_lo, u64 stack_hi, u32 pid, u32 tid, u64 tls_arr) {
  if (w->mode32) {
    wr32(c, teb + 0x00, 0xffffffffu); wr32(c, teb + 0x04, (u32)stack_hi); wr32(c, teb + 0x08, (u32)stack_lo); wr32(c, teb + 0x18, (u32)teb);
    wr32(c, teb + 0x20, pid); wr32(c, teb + 0x24, tid); wr32(c, teb + 0x2C, (u32)tls_arr); wr32(c, teb + 0x30, (u32)w->peb);
  } else {
    wr64(c, teb + 0x08, stack_hi); wr64(c, teb + 0x10, stack_lo); wr64(c, teb + 0x30, teb); wr64(c, teb + 0x40, pid); wr64(c, teb + 0x48, tid); wr64(c, teb + 0x58, tls_arr); wr64(c, teb + 0x60, w->peb);
  }
}
/* TlsSlots: TEB+0x1480 (8-byte) in x64, TEB+0xE10 (4-byte) in the 32-bit TEB */
static u64 tls_slot_addr(Cpu *c, u64 teb, u32 i) { return c->mode32 ? teb + 0xE10 + i * 4 : teb + 0x1480 + i * 8; }
/* the main image is PE32: switch the process to 32-bit mode (called by the loader before w32_start) */
void w32_enter_mode32(Win32 *w) {
  Cpu *c = w->cpu; if (w->mode32) return; w->mode32 = 1; c->mode32 = 1;
  u8 *t = mem_ptr(w->mem, w->teb, 0x2000, 1); if (t) e_memset(t, 0, 0x2000);
  u8 *pb = mem_ptr(w->mem, w->peb, 0x1000, 1); if (pb) e_memset(pb, 0, 0x1000);
  teb_init(w, c, w->teb, G_STACK_BASE, G_STACK_BASE + G_STACK_SIZE, (u32)w->pid, (u32)w->th[0].tid, w->th[0].tls_arr);
  wr32(c, w->peb + 0x10, (u32)(G_SHIM_BASE + PARAMS_OFF)); wr32(c, w->peb + 0x18, (u32)w32_heap_alloc(w, 0x100, 1)); wr32(c, w->peb + 0x64, 2);
  wr32(c, w->peb + 0xA4, 10); wr32(c, w->peb + 0xA8, 0); wr16(c, w->peb + 0xAC, 19045); wr32(c, w->peb + 0xB0, 2);
  wr32(c, w->peb + 0x1D4, 1);   /* SessionId */
  wr32(c, w->ubuf + 0x278, (u32)(w->ubuf + 0x7c0)); wr32(c, w->ubuf + 0x27c, (u32)(w->ubuf + 0x7c0));   /* _tzname[2] as 4-byte pointers */
  wr32(c, w->ubuf + 0x2a8, (u32)(w->ubuf + 0x400));
}

/* ---------- continuations ---------- */
static void guest_call(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
  W->cont_seq++;   /* guest code is about to run: continuations pushed afterwards belong to a new step */
  if (c->mode32) {   /* 32-bit callback: arguments pushed right to left; the continuation's saved esp covers both stdcall and cdecl callees */
    c->r[RSP] &= ~15ull; cpu_push(c, a3); cpu_push(c, a2); cpu_push(c, a1); cpu_push(c, a0); cpu_push(c, SLOT_ADDR(SLOT_CONT)); c->rip = fn; return;
  }
  c->r[RCX] = a0; c->r[RDX] = a1; c->r[R8] = a2; c->r[R9] = a3;
  c->r[RSP] = (c->r[RSP] & ~15ull) - 32;
  cpu_push(c, SLOT_ADDR(SLOT_CONT));
  c->rip = fn;
}
static int cont_push(Cpu *c, int kind) {
  Win32 *w = W; while (w->ncont && w->cont[w->ncont - 1].rsp < c->r[RSP]) w->ncont--;   /* continuations abandoned by a non-local exit (exception caught above them) */
  if (w->ncont >= 64) { cpu_fault(c, "guest callback nesting too deep"); return -1; }
  int i = w->ncont++; w->cont[i].kind = kind; w->cont[i].ret_addr = c->rip; w->cont[i].rsp = c->r[RSP]; w->cont[i].idx = 0;
  if (w->api_trace) e_log("[cont] push kind=%d depth=%d rsp=%llx ret=%llx\n", kind, i + 1, (unsigned long long)c->r[RSP], (unsigned long long)c->rip);
  w->cont[i].data[0] = w->cont[i].data[1] = w->cont[i].data[2] = w->cont[i].data[3] = 0; w->cont[i].clean32 = 0; w->cont[i].seq = w->cont_seq;
  /* a second continuation pushed by the same shim step (no guest code ran in between) on the same frame chains into the first one */
  w->cont[i].chain = i > 0 && w->cont[i - 1].seq == w->cont_seq && w->cont[i - 1].rsp == c->r[RSP] && w->cont[i - 1].ret_addr == c->rip; return i;
}
void w32_cont_resume(Cpu *c);
static void cont_finish(Cpu *c, u64 rax) {
  Win32 *w = W; if (!w->ncont) { cpu_fault(c, "continuation underflow"); return; }
  int i = --w->ncont;
  if (w->ncont && w->cont[i].chain) {   /* chained continuation pushed by the same shim step: it receives the result */
    c->r[RAX] = rax; if (c->mode32) c->r[RDX] = rax >> 32; c->r[RSP] = w->cont[i].rsp; w32_cont_resume(c); return; }
  c->r[RSP] = w->cont[i].rsp + w->cont[i].clean32; c->rip = w->cont[i].ret_addr; c->r[RAX] = rax; if (c->mode32) c->r[RDX] = rax >> 32; w->cont_seq++;
}
static void do_exit(Cpu *c, int code) { W->exited = 1; c->exit_code = code; c->status = RUN_EXIT; }
static void retry_and_yield(Cpu *c, u64 slot) { cpu_push(c, c->rip); c->rip = slot; c->status = RUN_YIELD; }
static u64 g_cur_slot, g_entry_eax;
static void w32_marshal32_in(Cpu *c, ShimSlot *s); static void w32_marshal32_out(Cpu *c, ShimSlot *s);
static int thread_cont_resume(Cpu *c, int kind, u64 rax);
static int tp_cont_resume(Cpu *c, int kind, u64 rax);
static int audio_cont_resume(Cpu *c, int kind, u64 rax);
static int shell_cont_resume(Cpu *c, int kind, u64 rax);
static int seh_cont_resume(Cpu *c, int kind, u64 rax);
static int cxx_cont_resume(Cpu *c, int kind, u64 rax);
static int seh32_cont_resume(Cpu *c, int kind, u64 rax);
static void thread_startup(Cpu *c);
static void s_Sleep(Cpu *c);

/* qsort as a bottom-up merge sort driven by the guest comparator, one comparison per continuation round trip (n log n calls,
   constant guest stack). data: 0 base, 1 n, 2 size, 3 cmp, 4 width, 5 lo, 6 i<<32|j, 7 tmp buffer; idx = parity (0: base→tmp, 1: tmp→base) */
static void qsort_step(Cpu *c, int have_result, u64 rax) {
  Win32 *w = W; int ci = w->ncont - 1; u64 *d = w->cont[ci].data;
  u64 base = d[0], n = d[1], sz = d[2], cmp = d[3], tmp = d[7];
  for (;;) {
    u64 width = d[4], lo = d[5], i = d[6] >> 32, j = d[6] & 0xffffffffu; int par = w->cont[ci].idx;
    u64 src = par ? tmp : base, dst = par ? base : tmp;
    if (width >= n) {   /* done: result sits in src */
      if (src != base) { u8 *s = mem_ptr(w->mem, tmp, n * sz, 0), *t = mem_ptr(w->mem, base, n * sz, 1); if (s && t) e_memcpy(t, s, (size_t)(n * sz)); }
      w32_heap_free(w, tmp); cont_finish(c, 0); return;
    }
    u64 mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n, k = i + j - mid;
    if (have_result) {   /* take the smaller head (stable: left on ties) */
      have_result = 0; u64 from = (s32)(u32)rax <= 0 ? i : j;
      u8 *s = mem_ptr(w->mem, src + from * sz, sz, 0), *t = mem_ptr(w->mem, dst + k * sz, sz, 1); if (s && t) e_memcpy(t, s, (size_t)sz);
      if (from == i) i++; else j++; k++;
    }
    if (i < mid && j < hi) { d[6] = (i << 32) | j; guest_call(c, cmp, src + i * sz, src + j * sz, 0, 0); return; }
    /* drain the remaining run and advance to the next pair of runs */
    if (i < mid) { u8 *s = mem_ptr(w->mem, src + i * sz, (mid - i) * sz, 0), *t = mem_ptr(w->mem, dst + k * sz, (mid - i) * sz, 1); if (s && t) e_memcpy(t, s, (size_t)((mid - i) * sz)); }
    if (j < hi) { u8 *s = mem_ptr(w->mem, src + j * sz, (hi - j) * sz, 0), *t = mem_ptr(w->mem, dst + k * sz, (hi - j) * sz, 1); if (s && t) e_memcpy(t, s, (size_t)((hi - j) * sz)); }
    lo += 2 * width;
    if (lo >= n) { width *= 2; lo = 0; w->cont[ci].idx ^= 1; }
    d[4] = width; d[5] = lo; u64 nmid = lo + width < n ? lo + width : n; d[6] = (lo << 32) | nmid;
  }
}
void w32_cont_resume(Cpu *c) {
  Win32 *w = W; while (w->ncont && w->cont[w->ncont - 1].rsp < c->r[RSP]) w->ncont--;
  if (!w->ncont) { cpu_fault(c, "stray continuation"); return; }
  int i = w->ncont - 1; u64 rax = c->mode32 ? (u32)c->r[RAX] : c->r[RAX];   /* 32-bit callbacks return in eax */
  g_a32_n = 0;   /* continuation code reads its state from the cont record, never from a stale argument override */
  if (w->api_trace) e_log("[cont] resume kind=%d depth=%d rax=%llx\n", w->cont[i].kind, w->ncont, (unsigned long long)rax);
  w->cont[i].seq = ++w->cont_seq;   /* the resumed step owns a fresh generation: continuations it pushes on this frame chain into it */
  c->r[RSP] = w->cont[i].rsp;   /* every callback of one continuation starts from the same stack pointer (qsort with 10^6 comparisons must not drift) */
  switch (w->cont[i].kind) {
    case CONT_STARTUP:
      if (w->cont[i].idx == 0 && attach_next_module(c)) return;   /* DllMain of statically imported DLLs runs before TLS callbacks and the entry point */
      if (w->cont[i].idx < w->n_tls_cb) { u64 fn = w->tls_callbacks[w->cont[i].idx++]; guest_call(c, fn, w->image_base, 1, 0, 0); return; }
      if (w->cont[i].idx == w->n_tls_cb) { w->cont[i].idx++; guest_call(c, w->entry, 0, 0, 0, 0); return; }
      do_exit(c, (int)rax); return;
    case CONT_INITTERM: {
      if (w->cont[i].data[2] && (int)rax != 0) { cont_finish(c, rax); return; }
      while (w->cont[i].data[0] < w->cont[i].data[1]) { u64 fn = rdp(c, w->cont[i].data[0]); w->cont[i].data[0] += PSZ; if (fn) { guest_call(c, fn, 0, 0, 0, 0); return; } }
      cont_finish(c, 0); return; }
    case CONT_ATEXIT:
      if (w->n_at_exit > 0) { u64 fn = w->at_exit[--w->n_at_exit]; guest_call(c, fn, 0, 0, 0, 0); return; }
      if (w->cont[i].data[0]) { do_exit(c, (int)w->cont[i].data[1]); return; }
      cont_finish(c, 0); return;
    case CONT_RET_RAX: cont_finish(c, rax); return;
    case CONT_RET_VAL: cont_finish(c, w->cont[i].data[0]); return;
    case CONT_CREATEWND: { extern void gdi_cont_resume(Cpu *c, int kind, u64 rax); gdi_cont_resume(c, CONT_CREATEWND, rax); return; }
    case CONT_DLLMAIN: if (attach_next_module(c)) return; cont_finish(c, w->cont[i].data[0]); return;
    case CONT_QSORT: qsort_step(c, 1, rax); return;
    case CONT_BSEARCH: {
      /* data0=key data1=base data2=hi data3=size data4=cmp data5=lo; the last probe was mid=(lo+hi)/2 */
      u64 key = w->cont[i].data[0], base = w->cont[i].data[1], hi = w->cont[i].data[2], sz = w->cont[i].data[3], cmp = w->cont[i].data[4], lo = w->cont[i].data[5];
      u64 mid = lo + (hi - lo) / 2;
      if ((int)rax == 0) { cont_finish(c, base + mid * sz); return; }
      if ((int)rax < 0) hi = mid; else lo = mid + 1;
      if (lo >= hi) { cont_finish(c, 0); return; }
      w->cont[i].data[2] = hi; w->cont[i].data[5] = lo; mid = lo + (hi - lo) / 2;
      guest_call(c, cmp, key, base + mid * sz, 0, 0); return; }
  }
  if (seh_cont_resume(c, w->cont[i].kind, rax)) return;
  if (cxx_cont_resume(c, w->cont[i].kind, rax)) return;
  if (seh32_cont_resume(c, w->cont[i].kind, rax)) return;
  if (tp_cont_resume(c, w->cont[i].kind, rax)) return;
  if (audio_cont_resume(c, w->cont[i].kind, rax)) return;
  if (thread_cont_resume(c, w->cont[i].kind, rax)) return;
  if (shell_cont_resume(c, w->cont[i].kind, rax)) return;
  extern void gdi_cont_resume(Cpu *c, int kind, u64 rax);
  gdi_cont_resume(c, w->cont[i].kind, rax);
}
/* exported helpers for gdi.c */
void w32_retry_yield(Cpu *c) { retry_and_yield(c, g_cur_slot); }
u64 w32_cur_slot(void) { return g_cur_slot; }
void w32_push_cont(Cpu *c, int kind, u64 val) { int i = cont_push(c, kind); if (i >= 0) W->cont[i].data[0] = val; }
void w32_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) { guest_call(c, fn, a0, a1, a2, a3); }
void w32_guest_call6(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {   /* six arguments: SUBCLASSPROC and friends */
  W->cont_seq++;
  if (c->mode32) { c->r[RSP] &= ~15ull; cpu_push(c, a5); cpu_push(c, a4); cpu_push(c, a3); cpu_push(c, a2); cpu_push(c, a1); cpu_push(c, a0); cpu_push(c, SLOT_ADDR(SLOT_CONT)); c->rip = fn; return; }
  c->r[RCX] = a0; c->r[RDX] = a1; c->r[R8] = a2; c->r[R9] = a3; c->r[RSP] = (c->r[RSP] & ~15ull) - 48; wr64(c, c->r[RSP] + 32, a4); wr64(c, c->r[RSP] + 40, a5); cpu_push(c, SLOT_ADDR(SLOT_CONT)); c->rip = fn; }
void w32_cont_finish(Cpu *c, u64 rax) { cont_finish(c, rax); }

/* 32-bit call of a shim: stack arguments (read through ARG), stdcall cleanup when the shim returns — immediately, or later through the
   continuation it pushed (guest callback in progress); a retry/yield leaves the frame intact so the slot is re-entered unchanged.
   Doubles returned by the x64 shim in xmm0 go to x87 ST(0); 64-bit results to edx:eax. */
static void w32_call32(Cpu *c, ShimSlot *s, u64 ret) {
  Win32 *w = W; int nc0 = w->ncont; u64 sp0 = c->r[RSP]; g_a32_n = 0;   /* a call from guest code: arguments are on the stack (no override left over from a shim that started a guest call) */
  if (s->sig32) w32_marshal32_in(c, s);
  s->fn(c); g_a32_n = 0;
  if (c->status == RUN_OK && c->rip == ret && w->ncont == nc0 && c->r[RSP] == sp0) c->r[RSP] = (u32)(c->r[RSP] + s->clean32);
  else if (w->ncont > nc0 && c->rip != ret) w->cont[nc0].clean32 = s->clean32;
  c->r[RDX] = c->r[RAX] >> 32;
  if (s->sig32 && c->status == RUN_OK && c->rip == ret) w32_marshal32_out(c, s);
}
int w32_dispatch(Cpu *c, u64 addr) {
  Win32 *w = W; u64 off = addr - G_SHIM_BASE;
  if (off % SHIM_SLOT_SIZE || off / SHIM_SLOT_SIZE >= (u64)w->nslots) { cpu_fault(c, "jump into shim region at 0x%llx", (unsigned long long)addr); return RUN_FAULT; }
  int idx = (int)(off / SHIM_SLOT_SIZE);
  if (idx == SLOT_CONT) { w32_cont_resume(c); return c->status; }
  u64 ret = cpu_pop(c); c->rip = ret; g_cur_slot = addr;
  ShimSlot *s = &w->slots[idx]; s->calls++;
  if (idx == SLOT_STARTUP) { if (w->cur_th) thread_startup(c); else { cont_push(c, CONT_STARTUP); w32_cont_resume(c); } return c->status; }
  if (!s->fn) {
    w->unknown_import_calls++;
    int pack = pe_dll_native_only(s->dll);
    cpu_fault(c, "unimplemented API %s!%s (called from 0x%llx)%s", s->dll, s->name, (unsigned long long)ret, pack ? " — this DLL comes from the DirectX pack: install it into C:\\Windows\\System32" : "");
    w32_report(w, "STOP: unimplemented API %s!%s%s", s->dll, s->name, pack ? " (install the DirectX pack)" : "");
    return RUN_FAULT;
  }
  g_entry_eax = c->r[RAX]; c->r[RAX] = 0;   /* x86 __CxxFrameHandler thunks pass FuncInfo in eax */
  if (w->api_trace) e_log("[api] t%d %s(%llx, %llx, %llx, %llx, %llx, %llx) @%llx\n", w->th[w->cur_th].tid, s->name, (unsigned long long)ARG(0), (unsigned long long)ARG(1), (unsigned long long)ARG(2), (unsigned long long)ARG(3), (unsigned long long)ARG(4), (unsigned long long)ARG(5), (unsigned long long)ret);
  if (c->mode32) { w32_call32(c, s, ret); if (w->api_trace) e_log("[api]   -> %llx\n", (unsigned long long)c->r[RAX]); return c->status; }
  s->fn(c);
  if (w->api_trace) e_log("[api]   -> %llx\n", (unsigned long long)c->r[RAX]);
  return c->status;
}

/* ---------- guest printf ---------- */
typedef struct { Cpu *c; int kind; int idx; u64 va; } ArgSrc;   /* kind 0: register/stack args starting at idx; kind 1: va_list pointer */
static u64 arg_next(ArgSrc *s) { if (s->kind == 1) { u64 v = rdp(s->c, s->va); s->va += PSZ_OF(s->c); return v; } return w32_arg(s->c, s->idx++); }
/* 8-byte argument (long long / double): one slot in x64, two 4-byte slots in a 32-bit process */
static u64 arg_next64(ArgSrc *s) { if (!s->c->mode32) return arg_next(s); if (s->kind == 1) { u64 v = rd64(s->c, s->va); s->va += 8; return v; } u64 lo = w32_arg(s->c, s->idx++), hi = w32_arg(s->c, s->idx++); return lo | hi << 32; }
int wstr_to_utf8(Cpu *c, u64 a, char *out, size_t n, int maxchars) {
  size_t p = 0; for (int i = 0; maxchars < 0 || i < maxchars; i++) { u16 ch = rd16(c, a + (u64)i * 2); if (!ch || c->status) break;
    if (ch < 0x80) { if (p + 1 < n) out[p++] = (char)ch; } else if (ch < 0x800) { if (p + 2 < n) { out[p++] = (char)(0xC0 | ch >> 6); out[p++] = (char)(0x80 | (ch & 0x3f)); } }
    else { if (p + 3 < n) { out[p++] = (char)(0xE0 | ch >> 12); out[p++] = (char)(0x80 | ((ch >> 6) & 0x3f)); out[p++] = (char)(0x80 | (ch & 0x3f)); } } }
  out[p] = 0; return (int)p;
}
static int g_format(Cpu *c, char *out, size_t n, u64 fmt_addr, int fmt_wide, ArgSrc *src) {
  char fmtbuf[4096]; const char *fmt;
  if (fmt_wide) { wstr_to_utf8(c, fmt_addr, fmtbuf, sizeof fmtbuf, -1); fmt = fmtbuf; } else { fmt = rd_str(c, fmt_addr, 1 << 20); if (!fmt) { cpu_fault(c, "printf: bad format pointer 0x%llx", (unsigned long long)fmt_addr); return 0; } }
  size_t pos = 0;
#define OUTC(ch) do { if (pos + 1 < n) out[pos] = (ch); pos++; } while (0)
  for (const char *f = fmt; *f; f++) {
    if (*f != '%') { OUTC(*f); continue; }
    const char *spec_start = f; f++;
    char spec[32]; int sp = 0; spec[sp++] = '%';
    while (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0') { if (sp < 8) spec[sp++] = *f; f++; }
    int width = -1, prec = -1;
    if (*f == '*') { width = (int)arg_next(src); f++; } else if (*f >= '0' && *f <= '9') { width = 0; while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0'); }
    if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = (int)arg_next(src); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
    int size = 4, wide = fmt_wide; /* default int */
    for (;;) {
      if (*f == 'l') { if (f[1] == 'l') { size = 8; f += 2; } else { size = 4; wide = 1; f++; } }
      else if (*f == 'h') { if (f[1] == 'h') { size = 1; f += 2; } else { size = 2; wide = 0; f++; } }
      else if (*f == 'I') { if (f[1] == '6' && f[2] == '4') { size = 8; f += 3; } else if (f[1] == '3' && f[2] == '2') { size = 4; f += 3; } else { size = 8; f++; } }
      else if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'q' || *f == 'L') { size = 8; f++; }
      else if (*f == 'w') { wide = 1; f++; }
      else break;
    }
    char conv = *f; if (!conv) { f--; break; }
    if (width >= 0) { spec[sp++] = '*'; } if (prec >= 0) { spec[sp++] = '.'; spec[sp++] = '*'; }
    char tmp[1024]; int tl = 0;
    switch (conv) {
      case '%': OUTC('%'); break;
      case 'c': case 'C': { u64 v = arg_next(src); spec[sp++] = 'c'; spec[sp] = 0; if (width >= 0 && prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, prec, (int)(v & 0xff)); else if (width >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, (int)(v & 0xff)); else if (prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, prec, (int)(v & 0xff)); else tl = e_snprintf(tmp, sizeof tmp, spec, (int)(v & 0xff)); break; }
      case 's': case 'S': { u64 v = arg_next(src); const char *s; char wbuf[2048]; int is_wide = conv == 'S' ? !fmt_wide : wide;
        if (!v) s = "(null)"; else if (is_wide) { wstr_to_utf8(c, v, wbuf, sizeof wbuf, prec >= 0 ? prec : -1); s = wbuf; } else { s = rd_str(c, v, 1 << 20); if (!s) { s = "(bad ptr)"; } }
        spec[sp++] = 's'; spec[sp] = 0;
        if (width >= 0 && prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, prec, s); else if (width >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, s); else if (prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, prec, s); else tl = e_snprintf(tmp, sizeof tmp, spec, s);
        if (tl > (int)sizeof tmp - 1) tl = sizeof tmp - 1; break; }
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
        if (conv == 'p') size = PSZ; u64 v = size == 8 ? arg_next64(src) : arg_next(src); if (conv == 'p') { conv = 'x'; if (prec < 0) { prec = 2 * PSZ; spec[sp++] = '.'; spec[sp++] = '*'; } size = 8; }
        if (size == 1) v = (conv == 'd' || conv == 'i') ? (u64)(s64)(s8)v : (v & 0xff); else if (size == 2) v = (conv == 'd' || conv == 'i') ? (u64)(s64)(s16)v : (v & 0xffff); else if (size == 4) v = (conv == 'd' || conv == 'i') ? (u64)(s64)(s32)v : (v & 0xffffffff);
        spec[sp++] = 'l'; spec[sp++] = 'l'; spec[sp++] = conv; spec[sp] = 0;
        if (width >= 0 && prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, prec, v); else if (width >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, v); else if (prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, prec, v); else tl = e_snprintf(tmp, sizeof tmp, spec, v); break; }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        u64 bits = arg_next64(src); double v; e_memcpy(&v, &bits, 8); if (conv == 'a' || conv == 'A') conv = 'e';
        spec[sp++] = conv; spec[sp] = 0;
        if (width >= 0 && prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, prec, v); else if (width >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, width, v); else if (prec >= 0) tl = e_snprintf(tmp, sizeof tmp, spec, prec, v); else tl = e_snprintf(tmp, sizeof tmp, spec, v); break; }
      case 'n': { u64 p = arg_next(src); if (p) wr32(c, p, (u32)pos); break; }
      default: { for (const char *q = spec_start; q <= f; q++) OUTC(*q); break; }
    }
    if (tl > (int)sizeof tmp - 1) tl = sizeof tmp - 1;
    for (int k = 0; k < tl; k++) OUTC(tmp[k]);
  }
  if (n) out[pos < n ? pos : n - 1] = 0;
  return (int)pos;
#undef OUTC
}
static char g_fmt_out[1 << 16];
int w32_format_args(Cpu *c, char *out, size_t n, u64 fmt, int wide, int first_arg) { ArgSrc src = { c, 0, first_arg, 0 }; return g_format(c, out, n, fmt, wide, &src); }
Win32 *g_root_w32;   /* the console belongs to the first process */
static void con_out(Win32 *w, int fd, const char *s, size_t n) {
  if (g_root_w32) w = g_root_w32;
  host_write(fd, s, n);
  size_t room = sizeof w->con - 1 - w->con_len; if (n > room) { /* keep tail */ size_t drop = n - room; if (drop > (size_t)w->con_len) { w->con_len = 0; } else { e_memmove(w->con, w->con + drop, w->con_len - drop); w->con_len -= (int)drop; } room = sizeof w->con - 1 - w->con_len; }
  if (n > room) { s += n - room; n = room; }
  e_memcpy(w->con + w->con_len, s, n); w->con_len += (int)n; w->con[w->con_len] = 0;
}
static int fd_of_file(Win32 *w, u64 file) {   /* guest FILE* → 1/2, or 0 */
  u64 iob = w->ubuf, st = w->mode32 ? 0x20 : 0x30; if (file == iob + st) return 1; if (file == iob + 2 * st) return 2; if (file == iob) return -1; return 0;
}
static int fd_of_handle(u64 h) { if (h == 0x11 || h == 0x12) return (int)(h - 0x10); if (h == 0x10) return -1; return 0; }


/* ---------- misc helpers ---------- */
static u64 now_100ns(void) { return (u64)(host_wall_ms() * 10000.0) + 116444736000000000ull; }   /* FILETIME: 100 ns since 1601 */
static u64 unix_time(void) { return (u64)(host_wall_ms() / 1000.0); }
static void queue_apc(Win32 *w, u64 fn, u64 a0, u64 a1, u64 a2);
static u32 nt_to_dos(u32 st) { u32 e;
  if ((st & 0xFFFF0000u) == 0x80070000u) return st & 0xFFFFu;   /* NTSTATUS_FROM_WIN32(err): the Win32 code travels in the low 16 bits (libuv SET_REQ_ERROR) */
  switch (st) { case 0: e = 0; break; case 0x103: e = 997; break; case 0xC0000008: e = 6; break; case 0xC000000D: e = 87; break; case 0xC000000F: case 0xC0000034: e = 2; break; case 0xC000003A: e = 3; break;
    case 0xC0000022: case 0xC00000BA: e = 5; break; case 0xC0000011: e = 38; break; case 0xC0000035: e = 183; break; case 0xC000014B: e = 109; break; case 0xC00000B1: e = 232; break; case 0xC00000AD: case 0xC00000AE: e = 231; break;
    case 0xC0000120: e = 995; break; case 0xC0000103: e = 267; break; case 0xC0000005: e = 998; break; case 0xC0000017: e = 8; break; case 0xC0000023: e = 122; break; case 0xC0000004: e = 24; break; case 0xC00000BB: e = 50; break; case 0xC0000043: e = 32; break; case 0xC0000101: e = 145; break; case 0xC0000033: e = 123; break; case 0xC0000039: e = 161; break;
    default: e = (st & 0xC0000000u) == 0xC0000000u ? 31 : 0; }
  return e; }
static void s_RtlNtStatusToDosError(Cpu *c) { RET(nt_to_dos((u32)ARG(0))); }
static int iocp_rw(Cpu *c, int fd, int write, u64 buf, u32 len, u64 bytes_out, u64 ov);   /* win32_iocp.inc */
static void iocp_handle_closed(int pid, int fd); static void iocp_kobj_closed(int kobj); static int iocp_has_packets(int kobj);
#include "win32_fs.inc"
#include "win32_ini.inc"
#include "win32_nt.inc"
#include "win32_threads.inc"
#include "win32_iocp.inc"
#include "win32_net.inc"
#include "win32_node.inc"
#include "win32_tp.inc"
#include "win32_bun.inc"
#include "d3d11.inc"
#include "win32_audio.inc"
static void s_CancelIo(Cpu *c) { sock_cancel_io(c, ARG(0)); RET(1); }   /* pipes: parked ReadFile/WriteFile stay (libuv cancels those only on close) */
static void s_ret0(Cpu *c); static void s_atexit(Cpu *c); static void s_setjmp(Cpu *c);
#include "win32_compat.inc"
#include "win32_inno.inc"
#include "win32_shell.inc"
#include "seh.inc"
#include "cxx.inc"
#include "seh32.inc"
static void s_CxxFrameHandler4(Cpu *c) { seh_log("[x64] __CxxFrameHandler4 (VS2019 compressed EH tables) is not implemented: frame skipped\n"); RET(1); }

/* ================= kernel32 ================= */
static void s_GetLastError(Cpu *c) { RET(W->last_error); }
static void s_SetLastError(Cpu *c) { W->last_error = (u32)ARG(0); }
static void s_GetCurrentProcess(Cpu *c) { RET(~0ull); }
static void s_ExitProcess(Cpu *c) { do_exit(c, (int)ARG(0)); }
static void s_GetTickCount(Cpu *c) { RET((u32)(host_now_ms() - W->start_ms + 100000.0)); }
static void s_GetTickCount64(Cpu *c) { RET((u64)(host_now_ms() - W->start_ms + 100000.0)); }
static void s_QueryPerformanceCounter(Cpu *c) { wr64(c, ARG(0), (u64)(host_now_ms() * 10000.0)); RET(1); }
static void s_QueryPerformanceFrequency(Cpu *c) { wr64(c, ARG(0), 10000000); RET(1); }
static void s_GetSystemTimeAsFileTime(Cpu *c) { wr64(c, ARG(0), now_100ns()); }
static void s_GetSystemTimePreciseAsFileTime(Cpu *c) { s_GetSystemTimeAsFileTime(c); }
static void fill_systemtime(Cpu *c, u64 a) { u64 t = unix_time(); u64 days = t / 86400, rem = t % 86400;
  /* civil from days (Howard Hinnant) */ s64 z = (s64)days + 719468; s64 era = z / 146097; s64 doe = z - era * 146097; s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; s64 y = yoe + era * 400; s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100); s64 mp = (5 * doy + 2) / 153; s64 d = doy - (153 * mp + 2) / 5 + 1; s64 m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++;
  wr16(c, a, (u16)y); wr16(c, a + 2, (u16)m); wr16(c, a + 4, (u16)((days + 4) % 7)); wr16(c, a + 6, (u16)d); wr16(c, a + 8, (u16)(rem / 3600)); wr16(c, a + 10, (u16)(rem / 60 % 60)); wr16(c, a + 12, (u16)(rem % 60)); wr16(c, a + 14, 0); }
static void s_GetSystemTime(Cpu *c) { fill_systemtime(c, ARG(0)); }
static void s_GetLocalTime(Cpu *c) { fill_systemtime(c, ARG(0)); }
static void s_Sleep(Cpu *c) {
  Win32 *w = W; double now = host_now_ms();
  if (!w->sleeping) { w->sleeping = 1; w->sleep_until_ms = now + (double)(u32)ARG(0); w->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot); return; }
  if (now >= w->sleep_until_ms) { w->sleeping = 0; return; }
  w->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot);
}
double w32_sleep_remaining_ms(Win32 *w) { if (!w->sleeping) return 0; double r = w->sleep_until_ms - host_now_ms(); return r > 0 ? r : 0; }
/* alertable wait: deliver one queued user APC (Rust std's pipe reads use ReadFileEx + SleepEx(INFINITE, TRUE)) → WAIT_IO_COMPLETION */
static int deliver_apc(Cpu *c, u64 ret) { Win32 *w = W; if (!w->napc) return 0; u64 fn = w->apc[0].fn, a0 = w->apc[0].a0, a1 = w->apc[0].a1, a2 = w->apc[0].a2; for (int i = 1; i < w->napc; i++) w->apc[i - 1] = w->apc[i]; w->napc--; w32_push_cont(c, CONT_RET_VAL, ret); guest_call(c, fn, a0, a1, a2, 0); return 1; }
static void s_SleepEx(Cpu *c) { if ((u32)ARG(1) && !W->sleeping && deliver_apc(c, 0xC0)) return; s_Sleep(c); }
static void queue_apc(Win32 *w, u64 fn, u64 a0, u64 a1, u64 a2) { if (w->napc < 8) { w->apc[w->napc].fn = fn; w->apc[w->napc].a0 = a0; w->apc[w->napc].a1 = a1; w->apc[w->napc].a2 = a2; w->napc++; } }
static void s_GetStdHandle(Cpu *c) { u32 n = (u32)ARG(0); RET(n == 0xfffffff6u ? 0x10 : n == 0xfffffff5u ? 0x11 : n == 0xfffffff4u ? 0x12 : ~0ull); }
static void s_WriteConsoleA(Cpu *c) { s_WriteFile(c); }
static void s_WriteConsoleW(Cpu *c) { int fd = fd_of_handle(ARG(0)); u32 n = (u32)ARG(2); if (fd <= 0) { RET(0); return; } char buf[4096]; int m = wstr_to_utf8(c, ARG(1), buf, sizeof buf, (int)n); int wr = out_write(c, fd, (const u8 *)buf, m); if (!wr) return; if (ARG(3)) wr32(c, ARG(3), n); RET(wr > 0); }
static void s_GetConsoleMode(Cpu *c) { int fd = fd_of_handle(ARG(0)); if (fd && vfs_resolve(fd < 0 ? 0 : fd) < 3) { wr32(c, ARG(1), fd < 0 ? 0x1f7 : 3); RET(1); } else { W->last_error = 6; RET(0); } }   /* a redirected std handle (pipe/file) is not a console */
static void s_SetConsoleMode(Cpu *c) { RET(1); }
static void s_GetConsoleOutputCP(Cpu *c) { RET(65001); }
static void s_GetConsoleCP(Cpu *c) { RET(65001); }
static void s_SetConsoleOutputCP(Cpu *c) { RET(1); }
static void s_SetConsoleCP(Cpu *c) { RET(1); }
static void s_GetConsoleScreenBufferInfo(Cpu *c) { u64 a = ARG(1); wr16(c, a, 120); wr16(c, a + 2, 40); wr16(c, a + 4, 0); wr16(c, a + 6, 0); wr16(c, a + 8, 7); wr16(c, a + 10, 0); wr16(c, a + 12, 0); wr16(c, a + 14, 119); wr16(c, a + 16, 39); wr16(c, a + 18, 120); wr16(c, a + 20, 40); RET(1); }
static void s_SetConsoleTextAttribute(Cpu *c) { RET(1); }
static void s_SetConsoleTitleA(Cpu *c) { RET(1); }
static void s_SetConsoleTitleW(Cpu *c) { RET(1); }
static void s_GetConsoleTitleW(Cpu *c) { if (ARG(0) && ARG(1)) wr16(c, ARG(0), 0); RET(0); }
static void s_GetConsoleTitleA(Cpu *c) { if (ARG(0) && ARG(1)) wr8(c, ARG(0), 0); RET(0); }
static void s_GetConsoleCursorInfo(Cpu *c) { if (ARG(1)) { wr32(c, ARG(1), 25); wr32(c, ARG(1) + 4, 1); } RET(1); }
static void s_SetConsoleCursorInfo(Cpu *c) { RET(1); }
static void s_SetConsoleCursorPosition(Cpu *c) { RET(1); }
static void s_FillConsoleOutputCharacterW(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), (u32)ARG(2)); RET(1); }
static void s_FillConsoleOutputCharacterA(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), (u32)ARG(2)); RET(1); }
static void s_FillConsoleOutputAttribute(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), (u32)ARG(2)); RET(1); }
static void s_ScrollConsoleScreenBufferW(Cpu *c) { RET(1); }
static void s_ScrollConsoleScreenBufferA(Cpu *c) { RET(1); }
static void s_SetConsoleScreenBufferSize(Cpu *c) { RET(1); }
static void s_SetConsoleWindowInfo(Cpu *c) { RET(1); }
static void s_WriteConsoleOutputW(Cpu *c) { RET(1); }
static void s_WriteConsoleOutputA(Cpu *c) { RET(1); }
static void s_WriteConsoleOutputCharacterW(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), (u32)ARG(2)); RET(1); }
static void s_WriteConsoleOutputAttribute(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), (u32)ARG(2)); RET(1); }
static void s_ReadConsoleOutputW(Cpu *c) { RET(1); }
static void s_ReadConsoleOutputCharacterW(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), 0); RET(1); }
static void s_ReadConsoleOutputAttribute(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), 0); RET(1); }
static void s_GetLargestConsoleWindowSize(Cpu *c) { RET((u64)120 | ((u64)40 << 16)); }
static void s_WriteConsoleInputW(Cpu *c) { if (ARG(3)) wr32(c, ARG(3), (u32)ARG(2)); RET(1); }
static void s_GetConsoleProcessList(Cpu *c) { if (ARG(0) && ARG(1)) wr32(c, ARG(0), (u32)W->pid); RET(1); }
static void s_GetConsoleDisplayMode(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), 0); RET(1); }
static void s_GetConsoleOriginalTitleW(Cpu *c) { if (ARG(0) && ARG(1)) wr16(c, ARG(0), 0); RET(0); }
static void s_GetConsoleAliasesLengthW(Cpu *c) { RET(0); }
/* CONSOLE_SCREEN_BUFFER_INFOEX (96 B): cbSize, dwSize, dwCursorPosition, wAttributes, srWindow, dwMaximumWindowSize, wPopupAttributes, bFullscreenSupported, ColorTable[16] */
static void s_GetConsoleScreenBufferInfoEx(Cpu *c) { u64 a = ARG(1); if (!a) { W->last_error = 87; RET(0); return; } wr32(c, a, 96); call_shim(c, s_GetConsoleScreenBufferInfo, ARG(0), a + 4, 0, 0); wr16(c, a + 26, 7); wr32(c, a + 28, 0);
  static const u32 pal[16] = { 0x0c0c0c, 0xda3700, 0x0ea113, 0xdd963a, 0x1f0fc5, 0x981788, 0x009cc1, 0xcccccc, 0x767676, 0xff783b, 0x0cc616, 0xd6d661, 0x5648e7, 0x9e00b4, 0xa5f1f9, 0xf2f2f2 }; for (int i = 0; i < 16; i++) wr32(c, a + 32 + i * 4, pal[i]); RET(1); }
static void s_SetConsoleScreenBufferInfoEx(Cpu *c) { RET(1); }
/* CONSOLE_FONT_INFOEX: cbSize, nFont, dwFontSize (COORD), FontFamily, FontWeight, FaceName[32] */
static void s_GetCurrentConsoleFontEx(Cpu *c) { u64 a = ARG(2); if (!a) { RET(0); return; } wr32(c, a + 4, 0); wr16(c, a + 8, 8); wr16(c, a + 10, 16); wr32(c, a + 12, 54); wr32(c, a + 16, 400); static const char f[] = "Consolas"; for (int i = 0; i <= 8; i++) wr16(c, a + 20 + i * 2, (u8)f[i]); RET(1); }
static void s_GetCurrentConsoleFont(Cpu *c) { u64 a = ARG(2); if (a) { wr32(c, a, 0); wr16(c, a + 4, 8); wr16(c, a + 6, 16); } RET(1); }
static void s_SetCurrentConsoleFontEx(Cpu *c) { RET(1); }
static void s_GetConsoleFontSize(Cpu *c) { RET((u64)8 | ((u64)16 << 16)); }
static void s_GetConsoleWindow(Cpu *c) { RET(0); }
static void s_AllocConsole(Cpu *c) { RET(1); }
static void s_SetConsoleCtrlHandler(Cpu *c) { RET(1); }
static void s_FlushFileBuffers(Cpu *c) { RET(1); }

static void s_GetCommandLineA(Cpu *c) { RET(W->cmdline_a); }
static void s_GetCommandLineW(Cpu *c) { RET(W->cmdline_w); }
static void s_FreeEnvironmentStringsW(Cpu *c) { RET(1); }
static void s_FreeEnvironmentStringsA(Cpu *c) { RET(1); }
static void s_GetStartupInfoA(Cpu *c) { u64 a = ARG(0);
  if (c->mode32) { u8 *p = mem_ptr(c->mem, a, 68, 1); if (p) { e_memset(p, 0, 68); wr32(c, a, 68); wr32(c, a + 56, 0x10); wr32(c, a + 60, 0x11); wr32(c, a + 64, 0x12); } return; }   /* STARTUPINFO32: 68 bytes, hStdInput @56 */
  u8 *p = mem_ptr(c->mem, a, 104, 1); if (p) { e_memset(p, 0, 104); wr32(c, a, 104); wr64(c, a + 80, 0x10); wr64(c, a + 88, 0x11); wr64(c, a + 96, 0x12); } }
static void s_GetStartupInfoW(Cpu *c) { s_GetStartupInfoA(c); }
/* ---------- modules: private DLLs loaded from the virtual disk ---------- */
static u32 mod_atoi(const char *p) { u32 v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (u32)(*p++ - '0'); return v; }
static const char *mod_basename(const char *p) { const char *b = p; for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1; return b; }
static int mod_name_eq(const char *a, const char *b) {   /* case-insensitive, ".dll" optional on either side */
  for (;;) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) { if (!x && !e_stricmp(b, ".dll")) return 1; if (!y && !e_stricmp(a, ".dll")) return 1; return 0; } if (!x) return 1; a++; b++; }
}
static int mod_find(Win32 *w, const char *name) { const char *bn = mod_basename(name); for (int i = 0; i < w->nmods; i++) if (mod_name_eq(w->mods[i].name, bn)) return i; return -1; }
static int mod_by_base(Win32 *w, u64 base) { for (int i = 0; i < w->nmods; i++) if (w->mods[i].base == base) return i; return -1; }
/* find a DLL file on the virtual disk: as given, next to the exe (cwd), C:\nebula */
/* AddDllDirectory(PCWSTR) → opaque cookie; SetDllDirectory replaces slot 0 */
static void dll_dir_add(Win32 *w, const char *d, int set) { if (!d || !d[0]) { if (set) w->dll_dirs[0][0] = 0; return; } int i = set ? 0 : (w->ndll_dirs < 8 ? w->ndll_dirs : 7); e_snprintf(w->dll_dirs[i], sizeof w->dll_dirs[i], "%s", d); size_t l = e_strlen(w->dll_dirs[i]); if (l > 1 && w->dll_dirs[i][l - 1] == '\\') w->dll_dirs[i][l - 1] = 0; if (!set && w->ndll_dirs < 8) w->ndll_dirs++; else if (set && w->ndll_dirs == 0) w->ndll_dirs = 1; }
static void s_AddDllDirectory(Cpu *c) { char d[600]; wstr_to_utf8(c, ARG(0), d, sizeof d, -1); dll_dir_add(W, d, 0); RET(0x1000 + W->ndll_dirs); }
static void s_SetDllDirectoryW(Cpu *c) { char d[600]; if (ARG(0)) wstr_to_utf8(c, ARG(0), d, sizeof d, -1); else d[0] = 0; dll_dir_add(W, d, 1); RET(1); }
static void s_SetDllDirectoryA(Cpu *c) { const char *d = ARG(0) ? rd_str(c, ARG(0), 260) : 0; dll_dir_add(W, d ? d : "", 1); RET(1); }
/* search order (Windows "safe DLL search"): as given, the exe's directory, the AddDllDirectory/SetDllDirectory directories, C:\Windows\System32, cwd, C:\nebula, then PATH */
static const char *env_get(const char *name);   /* win32_fs.inc */
static int mod_load(Win32 *w, const char *name) {
  int m = mod_find(w, name); if (m >= 0) return m;
  char path[600]; const char *cands[48]; int nc = 0; cands[nc++] = name; const char *bn = mod_basename(name);
  char p0[600], p1[600], p2[600], p3[600]; char pbuf[8][600]; int np = 0;
  if (w->exe_path[0]) { e_snprintf(p0, sizeof p0, "%s", w->exe_path); char *sl = 0; for (char *q = p0; *q; q++) if (*q == '\\' || *q == '/') sl = q; if (sl) { size_t l = (size_t)(sl + 1 - p0); e_snprintf(p0 + l, sizeof p0 - l, "%s", bn); cands[nc++] = p0; } }
  /* directories of the DLLs already loaded from the virtual disk (coreclr.dll finds clrjit.dll / System.*.dll next to itself) */
  char pm[8][600]; int npm = 0;
  for (int i = w->nmods - 1; i >= 0 && npm < 8; i--) { if (!w->mods[i].dir[0]) continue; int dup = 0; for (int k = 0; k < npm; k++) if (!e_strnicmp_(pm[k], w->mods[i].dir, e_strlen(w->mods[i].dir))) { dup = 1; break; } if (dup) continue; e_snprintf(pm[npm], sizeof pm[npm], "%s\\%s", w->mods[i].dir, bn); cands[nc++] = pm[npm++]; }
  char pd[8][600]; for (int i = 0; i < w->ndll_dirs && i < 8; i++) if (w->dll_dirs[i][0]) { e_snprintf(pd[i], sizeof pd[i], "%s\\%s", w->dll_dirs[i], bn); cands[nc++] = pd[i]; }
  e_snprintf(p3, sizeof p3, "C:\\Windows\\System32\\%s", bn); cands[nc++] = p3;
  e_snprintf(p1, sizeof p1, "%s\\%s", vfs_cwd(), bn); cands[nc++] = p1; e_snprintf(p2, sizeof p2, "C:\\nebula\\%s", bn); cands[nc++] = p2;
  { const char *pv = env_get("PATH"); while (pv && *pv && np < 8) { const char *e = pv; while (*e && *e != ';') e++; size_t l = (size_t)(e - pv); if (l && l < 500) { e_memcpy(pbuf[np], pv, l); if (pbuf[np][l - 1] == '\\') l--; e_snprintf(pbuf[np] + l, sizeof pbuf[np] - l, "\\%s", bn); cands[nc++] = pbuf[np]; np++; } pv = *e ? e + 1 : e; } }
  for (int k = 0; k < nc; k++) {
    e_snprintf(path, sizeof path, "%s", cands[k]);
    int node = vfs_find_node(path); if (node < 0) { size_t l = e_strlen(path); if (l + 4 < sizeof path && e_stricmp(path + (l > 4 ? l - 4 : 0), ".dll")) { e_memcpy(path + l, ".dll", 5); node = vfs_find_node(path); } }
    if (node < 0) continue;
    VNodeInfo vi; if (!vfs_node_info(node, &vi) || vi.is_dir) continue; const u8 *img = vfs_node_data(node); if (!img) continue;
    int idx = pe_load_dll(w, img, (size_t)vi.size, mod_basename(path));
    if (idx >= 0) { size_t dl = (size_t)(mod_basename(path) - path); if (dl && dl < sizeof w->mods[idx].dir) { e_memcpy(w->mods[idx].dir, path, dl - 1); w->mods[idx].dir[dl - 1] = 0; } }
    if (idx >= 0) { char msg[400]; e_snprintf(msg, sizeof msg, "[x64] loaded DLL %s at 0x%llx (%llu KB)\n", w->mods[idx].name, (unsigned long long)w->mods[idx].base, (unsigned long long)(w->mods[idx].size >> 10)); host_write(3, msg, e_strlen(msg)); }
    return idx;
  }
  return -1;
}
/* export lookup by name or ordinal; follows forwarders ("KERNEL32.Foo") into the shim table */
static u64 mod_export(Win32 *w, int mi, const char *name, u32 ordinal, int depth);
static u64 mod_export_m(Win32 *w, GuestModule *m, const char *name, u32 ordinal, int depth) {
  if (!m->exp_rva || depth > 4) return 0; Cpu *c = w->cpu; u64 e = m->base + m->exp_rva;
  u32 ord_base = rd32(c, e + 16), nfunc = rd32(c, e + 20), nnames = rd32(c, e + 24), funcs = rd32(c, e + 28), names = rd32(c, e + 32), ords = rd32(c, e + 36);
  u32 idx = 0xffffffffu;
  if (name) { for (u32 i = 0; i < nnames && i < 65536; i++) { const char *en = rd_str(c, m->base + rd32(c, m->base + names + 4 * i), 256); if (en && !e_strcmp(en, name)) { idx = rd16(c, m->base + ords + 2 * i); break; } } }
  else if (ordinal >= ord_base) idx = ordinal - ord_base;
  if (idx >= nfunc) return 0;
  u32 rva = rd32(c, m->base + funcs + 4 * idx); if (!rva) return 0;
  if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size) {   /* forwarder */
    const char *fw = rd_str(c, m->base + rva, 256); if (!fw) return 0; char dll[80]; int k = 0; while (fw[k] && fw[k] != '.' && k < 70) { dll[k] = fw[k]; k++; } dll[k] = 0;
    if (fw[k] != '.') return 0; const char *fn = fw + k + 1; e_memcpy(dll + k, ".dll", 5);
    if (!pe_is_system_dll(dll) || pe_dll_native_ok(dll)) { int o = mod_load(w, dll); if (o >= 0) return mod_export(w, o, fn[0] == '#' ? 0 : fn, fn[0] == '#' ? (u32)mod_atoi(fn + 1) : 0, depth + 1); }
    return w32_shim_slot(w, dll, fn);
  }
  return m->base + rva; }
static u64 mod_export(Win32 *w, int mi, const char *name, u32 ordinal, int depth) { return mod_export_m(w, &w->mods[mi], name, ordinal, depth); }
/* exports of the main executable itself: GetProcAddress(GetModuleHandle(NULL), "napi_...") — Node-API addons delay-load their host's exports */
static u64 exe_export(Win32 *w, const char *name, u32 ordinal) { if (!w->exe_exp_rva) return 0; GuestModule m; e_memset(&m, 0, sizeof m); m.base = w->image_base; m.exp_rva = w->exe_exp_rva; m.exp_size = w->exe_exp_size; return mod_export_m(w, &m, name, ordinal, 0); 
}
static const char *ordinal_name(const char *dll, u32 ord);
u64 w32_resolve_import(Win32 *w, const char *dll, const char *name, u32 ordinal) {
  if (!pe_is_system_dll(dll) || pe_dll_native_ok(dll)) { int m = mod_load(w, dll); if (m >= 0) { u64 a = mod_export(w, m, name[0] == '#' ? 0 : name, name[0] == '#' ? ordinal : 0, 0); if (a) return a; } }
  if (name[0] == '#') { const char *n = ordinal_name(dll, ordinal); if (n) return w32_shim_slot(w, dll, n); }
  return w32_shim_slot(w, dll, name);
}
/* DllMain(base, DLL_PROCESS_ATTACH, 0) for every module not yet attached (one per continuation round trip) */
static int attach_next_module(Cpu *c) {
  Win32 *w = W; for (int i = 0; i < w->nmods; i++) if (!w->mods[i].attached) { w->mods[i].attached = 1; if (w->mods[i].entry) { guest_call(c, w->mods[i].entry, w->mods[i].base, 1, 0, 0); return 1; } }
  return 0;
}
/* pseudo handles for system DLLs served by shims: 0x7ff00000000 + slot<<12; the name is kept so GetProcAddress can resolve ordinals */
static char g_fake_names[64][40]; static int g_nfake;
static void fake_key(const char *n, char *out, size_t cap) { const char *b = n; for (const char *p = n; *p; p++) if (*p == '\\' || *p == '/') b = p + 1; size_t o = 0; for (; b[o] && o + 1 < cap; o++) out[o] = (b[o] >= 'A' && b[o] <= 'Z') ? (char)(b[o] + 32) : b[o]; out[o] = 0; if (o > 4 && !e_stricmp(out + o - 4, ".dll")) out[o - 4] = 0; }
static u64 fake_module(const char *n) { char k[40]; fake_key(n && n[0] ? n : "?", k, sizeof k);
  u64 base = (g_fs_w && g_fs_w->mode32) ? 0x7f000000ull : 0x7ff00000000ull;   /* a 32-bit process needs handles below 4 GB */
  for (int i = 0; i < g_nfake; i++) if (!e_stricmp(g_fake_names[i], k)) return base + ((u64)(i + 1) << 12);
  if (g_nfake < 64) { e_snprintf(g_fake_names[g_nfake], sizeof g_fake_names[0], "%s", k); g_nfake++; return base + ((u64)g_nfake << 12); }
  return base + (1ull << 12); }
static const char *fake_module_name(u64 h) { if (((h >> 32) != 0x7ff && (h >> 20) != 0x7f0) || (h & 0xfff)) return 0; u64 i = (h >> 12) & 0xff; return i >= 1 && i <= (u64)g_nfake ? g_fake_names[i - 1] : 0; }
/* export ordinals of the DLLs that are still imported by number (delay-loaded WSOCK32 in Bun, OLEAUT32 everywhere) */
#include "ordinals.inc"
/* numbered export of a system DLL (generated table from the Wine .spec files); dll may carry a path/extension */
static const char *ordinal_table_name(const char *dll, u32 ord) {
  if (!dll) return 0; const char *b = dll; for (const char *p = dll; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
  size_t bl = e_strlen(b); if (bl > 4 && !e_stricmp(b + bl - 4, ".dll")) bl -= 4;
  for (size_t i = 0; i < sizeof g_ordinals / sizeof g_ordinals[0]; i++) if (g_ordinals[i].ord == ord && !e_strnicmp_(g_ordinals[i].dll, b, bl) && !g_ordinals[i].dll[bl]) return g_ordinals[i].name;
  return 0; }
static const char *ordinal_name(const char *dll, u32 ord) {
  if (dll && !e_strnicmp_(dll, "xinput", 6)) { switch (ord) { case 100: return "XInputGetStateEx"; case 101: return "XInputWaitForGuideButton"; case 102: return "XInputCancelGuideButtonWait"; case 103: return "XInputPowerOffController"; default: return 0; } }   /* hidden exports used by Steam Input / SDL */
  if (!dll) return 0;
  if (!e_stricmp(dll, "ws2_32") || !e_stricmp(dll, "wsock32")) { static const char *const t[] = { 0, "accept", "bind", "closesocket", "connect", "getpeername", "getsockname", "getsockopt", "htonl", "htons", "ioctlsocket", "inet_addr", "inet_ntoa", "listen", "ntohl", "ntohs", "recv", "recvfrom", "select", "send", "sendto", "setsockopt", "shutdown", "socket" };
    if (ord < 24) return t[ord]; switch (ord) { case 51: return "gethostbyaddr"; case 52: return "gethostbyname"; case 53: return "getprotobyname"; case 54: return "getprotobynumber"; case 55: return "getservbyname"; case 56: return "getservbyport"; case 57: return "gethostname";
      case 111: return "WSAGetLastError"; case 112: return "WSASetLastError"; case 115: return "WSAStartup"; case 116: return "WSACleanup"; case 151: return "__WSAFDIsSet"; default: return ordinal_table_name(dll, ord); } }
  if (!e_stricmp(dll, "oleaut32")) { switch (ord) { case 2: return "SysAllocString"; case 3: return "SysReAllocString"; case 4: return "SysAllocStringLen"; case 5: return "SysReAllocStringLen"; case 6: return "SysFreeString"; case 7: return "SysStringLen"; case 8: return "VariantInit"; case 9: return "VariantClear"; case 149: return "SysStringByteLen"; case 150: return "SysAllocStringByteLen"; default: return 0; } }
  return ordinal_table_name(dll, ord); }
static void load_library(Cpu *c, const char *n) {
  if (!n || !n[0]) { W->last_error = 87; RET(0); return; }
  if (!pe_is_system_dll(n) || pe_dll_native_ok(n)) {
    int m = mod_load(W, n);
    if (m >= 0) { u64 base = W->mods[m].base; if (!W->mods[m].attached || W->nmods > m + 1) { int i = cont_push(c, CONT_DLLMAIN); if (i < 0) return; W->cont[i].data[0] = base; w32_cont_resume(c); return; } RET(base); return; }
    if (pe_dll_native_only(n)) { char msg[400]; e_snprintf(msg, sizeof msg, "[x64] LoadLibrary(%s): not on the virtual disk and not shimmed — install the DirectX pack (C:\\Windows\\System32)\n", n); host_write(3, msg, e_strlen(msg)); W->last_error = 126; RET(0); return; }
    /* not on the virtual disk: behave like a system DLL (shims by name); a real miss shows up as "unimplemented API dll!name" */
    if (!pe_is_system_dll(n)) { char msg[400]; e_snprintf(msg, sizeof msg, "[x64] LoadLibrary(%s): not found on the virtual disk, using shims\n", n); host_write(3, msg, e_strlen(msg)); }
  }
  RET(fake_module(n));
}
/* GetModuleHandle of a DLL that exists only as a real file (DirectX pack) and is not loaded: NULL, so the program goes on to LoadLibrary it */
static u64 fake_module_gmh(Win32 *w, const char *n) { if (pe_dll_native_only(mod_basename(n))) { w->last_error = 126; return 0; } return fake_module(n); }
static void s_GetModuleHandleA(Cpu *c) { const char *n = rd_str(c, ARG(0), 260); if (!n) { RET(W->image_base); return; } int m = mod_find(W, n); if (m >= 0) { RET(W->mods[m].base); return; } if ((mod_name_eq(mod_basename(n), W->exe_name) || mod_name_eq(mod_basename(n), "node.exe"))) { RET(W->image_base); return; } RET(fake_module_gmh(W, n)); }
static void s_GetModuleHandleW(Cpu *c) { if (!ARG(0)) { RET(W->image_base); return; } char n[260]; wstr_to_utf8(c, ARG(0), n, sizeof n, -1); c->r[RCX] = 0; int m = mod_find(W, n); if (m >= 0) { RET(W->mods[m].base); return; } if ((mod_name_eq(mod_basename(n), W->exe_name) || mod_name_eq(mod_basename(n), "node.exe"))) { RET(W->image_base); return; } RET(fake_module_gmh(W, n)); }
static void s_GetModuleHandleExW(Cpu *c) { u64 h = W->image_base; u32 flags = (u32)ARG(0);
  if (ARG(1)) { if (flags & 4) { for (int i = 0; i < W->nmods; i++) if (ARG(1) - W->mods[i].base < W->mods[i].size) h = W->mods[i].base; }   /* GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS */
    else { char n[260]; wstr_to_utf8(c, ARG(1), n, sizeof n, -1); int m = mod_find(W, n); h = m >= 0 ? W->mods[m].base : (mod_name_eq(mod_basename(n), W->exe_name) || mod_name_eq(mod_basename(n), "node.exe")) ? W->image_base : fake_module_gmh(W, n); } }
  if (ARG(2)) wrp(c, ARG(2), h); RET(1); }
static void s_GetModuleHandleExA(Cpu *c) { u64 h = W->image_base; u32 flags = (u32)ARG(0);
  if (ARG(1)) { if (flags & 4) { for (int i = 0; i < W->nmods; i++) if (ARG(1) - W->mods[i].base < W->mods[i].size) h = W->mods[i].base; }
    else { const char *n = rd_str(c, ARG(1), 260); int m = n ? mod_find(W, n) : -1; h = m >= 0 ? W->mods[m].base : (n && (mod_name_eq(mod_basename(n), W->exe_name) || mod_name_eq(mod_basename(n), "node.exe"))) ? W->image_base : fake_module_gmh(W, n); } }
  if (ARG(2)) wrp(c, ARG(2), h); RET(1); }
static void s_LoadLibraryA(Cpu *c) { load_library(c, rd_str(c, ARG(0), 260)); }
static void s_LoadLibraryW(Cpu *c) { char n[260]; wstr_to_utf8(c, ARG(0), n, sizeof n, -1); load_library(c, n); }
static void s_LoadLibraryExA(Cpu *c) { s_LoadLibraryA(c); }
static void s_LoadLibraryExW(Cpu *c) { s_LoadLibraryW(c); }
static void s_FreeLibrary(Cpu *c) { RET(1); }
static void s_GetProcAddress(Cpu *c) {
  int m = mod_by_base(W, ARG(0));
  if (m >= 0) { u64 a = ARG(1) < 0x10000 ? mod_export(W, m, 0, (u32)ARG(1), 0) : mod_export(W, m, rd_str(c, ARG(1), 256), 0, 0); if (!a) W->last_error = 127; RET(a); return; }
  if (ARG(0) == W->image_base) { u64 a = ARG(1) < 0x10000 ? exe_export(W, 0, (u32)ARG(1)) : exe_export(W, rd_str(c, ARG(1), 256), 0); if (a) { RET(a); return; } }
  const char *n = ARG(1) < 0x10000 ? ordinal_name(fake_module_name(ARG(0)), (u32)ARG(1)) : rd_str(c, ARG(1), 256);
  if (!n) { if (ARG(1) < 0x10000) { char msg[160]; e_snprintf(msg, sizeof msg, "[x64] GetProcAddress miss: %s!#%u\n", fake_module_name(ARG(0)) ? fake_module_name(ARG(0)) : "?", (u32)ARG(1)); host_write(3, msg, e_strlen(msg)); } W->last_error = 127; RET(0); return; }
  if (!w32_have_shim("", n)) { char msg[320]; e_snprintf(msg, sizeof msg, "[x64] GetProcAddress miss: %s\n", n); host_write(3, msg, e_strlen(msg)); W->last_error = 127; RET(0); return; }
  RET(w32_shim_slot(W, "dynamic", n)); }
static const char *mod_file_name(Win32 *w, u64 h) { int m = h ? mod_by_base(w, h) : -1; return m >= 0 ? w->mods[m].name : w->exe_name; }
static void mod_full_path(Win32 *w, u64 h, char *p, size_t cap) { int m = h ? mod_by_base(w, h) : -1; if (m < 0 && w->exe_path[0]) e_snprintf(p, cap, "%s", w->exe_path); else if (m >= 0 && w->mods[m].dir[0]) e_snprintf(p, cap, "%s\\%s", w->mods[m].dir, mod_file_name(w, h)); else e_snprintf(p, cap, "C:\\nebula\\%s", mod_file_name(w, h)); }
static void s_GetModuleFileNameA(Cpu *c) { char p[600]; mod_full_path(W, ARG(0), p, sizeof p); size_t l = e_strlen(p); u32 n = (u32)ARG(2); if (l + 1 > n) l = n ? n - 1 : 0; u8 *d = mem_ptr(c->mem, ARG(1), l + 1, 1); if (d) { e_memcpy(d, p, l); d[l] = 0; } RET(l); }
static void s_GetModuleFileNameW(Cpu *c) { char p[600]; mod_full_path(W, ARG(0), p, sizeof p); size_t l = e_strlen(p); u32 n = (u32)ARG(2); if (l + 1 > n) l = n ? n - 1 : 0; for (size_t i = 0; i < l; i++) wr16(c, ARG(1) + i * 2, (u8)p[i]); wr16(c, ARG(1) + l * 2, 0); RET(l); }
static void s_GetSystemDirectoryA(Cpu *c) { const char *d = "C:\\Windows\\System32"; size_t l = e_strlen(d); if ((u32)ARG(1) > l) { u8 *p = mem_ptr(c->mem, ARG(0), l + 1, 1); if (p) e_memcpy(p, d, l + 1); } RET(l); }
static void s_GetWindowsDirectoryA(Cpu *c) { const char *d = "C:\\Windows"; size_t l = e_strlen(d); if ((u32)ARG(1) > l) { u8 *p = mem_ptr(c->mem, ARG(0), l + 1, 1); if (p) e_memcpy(p, d, l + 1); } RET(l); }
static void s_GetVersion(Cpu *c) { RET(0x23f00206); }
static void s_GetVersionExA(Cpu *c) { u64 a = ARG(0); u32 sz = rd32(c, a); wr32(c, a + 4, 10); wr32(c, a + 8, 0); wr32(c, a + 12, 19045); wr32(c, a + 16, 2); if (sz >= 20) { u8 *p = mem_ptr(c->mem, a + 20, 128, 1); if (p) e_memset(p, 0, 128); } RET(1); }
static void s_GetVersionExW(Cpu *c) { s_GetVersionExA(c); }
static void s_GetApplicationRestartSettings(Cpu *c) { RET(0x80070490); /* E_ELEMENT_NOT_FOUND: nothing registered */ }
static void s_RegisterApplicationRestart(Cpu *c) { RET(0); }
static void s_UnregisterApplicationRestart(Cpu *c) { RET(0); }
static void s_GetProductInfo(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), 0x30 /* PRODUCT_PROFESSIONAL */); RET(1); }
static void s_IsProcessorFeaturePresent(Cpu *c) { u32 f = (u32)ARG(0); RET(f == 6 || f == 10 || f == 0 || f == 3 || f == 1 || f == 2 || f == 4 || f == 5 ? 1 : 0); /* sse, sse2, cmpxchg, rdtsc... ; no sse3/avx/xsave(17) */ }
static void s_IsDebuggerPresent(Cpu *c) { RET(0); }
static void s_IsDBCSLeadByte(Cpu *c) { RET(0); }
static void s_IsDBCSLeadByteEx(Cpu *c) { RET(0); }
static void s_GetACP(Cpu *c) { RET(65001); }
static void s_GetOEMCP(Cpu *c) { RET(65001); }
static void s_GetCPInfo(Cpu *c) { u64 a = ARG(1); wr32(c, a, 1); wr8(c, a + 4, '?'); wr8(c, a + 5, 0); for (int i = 0; i < 12; i++) wr8(c, a + 6 + i, 0); RET(1); }
static void s_GetActiveProcessorCount(Cpu *c) { RET(1); }
/* PROC_THREAD_ATTRIBUTE_LIST (subprocess passes handle lists): size query → ERROR_INSUFFICIENT_BUFFER, otherwise accept and ignore */
static void s_InitializeProcThreadAttributeList(Cpu *c) { if (ARG(3)) wrp(c, ARG(3), 64 + 32 * (ARG(1) ? ARG(1) : 1)); if (!ARG(0)) { W->last_error = 122; RET(0); return; } RET(1); }
static void s_UpdateProcThreadAttribute(Cpu *c) { RET(1); }
static void s_DeleteProcThreadAttributeList(Cpu *c) { RET(0); }   /* one logical processor, like GetSystemInfo */
static void s_GetSystemInfo(Cpu *c) { u64 a = ARG(0);
  if (c->mode32) { u8 *p = mem_ptr(c->mem, a, 36, 1); if (!p) return; e_memset(p, 0, 36); wr16(c, a, 0); wr32(c, a + 4, 4096); wr32(c, a + 8, 0x10000); wr32(c, a + 12, 0x7ffeffff); wr32(c, a + 16, 1); wr32(c, a + 20, 1); wr32(c, a + 24, 586); wr32(c, a + 28, 0x10000); return; }   /* SYSTEM_INFO32 (WOW64 view: PROCESSOR_ARCHITECTURE_INTEL) */
  u8 *p = mem_ptr(c->mem, a, 48, 1); if (!p) return; e_memset(p, 0, 48); wr16(c, a, 9); wr32(c, a + 4, 4096); wr64(c, a + 8, 0x10000); wr64(c, a + 16, 0x7ffffffeffff); wr64(c, a + 24, 1); wr32(c, a + 32, 1); wr32(c, a + 36, 586); wr32(c, a + 40, 0x10000); }
static void s_GetNativeSystemInfo(Cpu *c) { s_GetSystemInfo(c); if (c->mode32) wr16(c, ARG(0), 9); }   /* the native machine is x64 */
static void s_GlobalMemoryStatusEx(Cpu *c) { u64 a = ARG(0); wr32(c, a + 4, 30); wr64(c, a + 8, 4ull << 30); wr64(c, a + 16, 3ull << 30); wr64(c, a + 24, 4ull << 30); wr64(c, a + 32, 3ull << 30); wr64(c, a + 40, 8ull << 40); wr64(c, a + 48, 8ull << 40); RET(1); }
static void s_RtlAddFunctionTable(Cpu *c) { RET(1); }
static void s_RtlAddGrowableFunctionTable(Cpu *c) { if (ARG(0)) wr64(c, ARG(0), 0x4747); RET(0); }   /* V8 registers JIT unwind data; we never unwind through JIT frames */
static void s_RtlGrowFunctionTable(Cpu *c) { RET(0); }
static void s_RtlDeleteGrowableFunctionTable(Cpu *c) { RET(0); }
static void s_RtlInstallFunctionTableCallback(Cpu *c) { RET(1); }
static void s_RtlDeleteFunctionTable(Cpu *c) { RET(1); }
static void s_EncodePointer(Cpu *c) { RET(ARG(0) ^ 0xA5A5A5A5ull); }
static void s_DecodePointer(Cpu *c) { RET(ARG(0) ^ 0xA5A5A5A5ull); }
static void s_OutputDebugStringA(Cpu *c) { const char *s = rd_str(c, ARG(0), 4096); if (s) e_log("[dbg] %s", s); }
static void s_OutputDebugStringW(Cpu *c) { char b[1024]; wstr_to_utf8(c, ARG(0), b, sizeof b, -1); e_log("[dbg] %s", b); }
/* FormatMessage(flags, source, msgid, langid, buffer, size, args): FROM_SYSTEM texts for the common Win32 / HRESULT codes, FROM_STRING copies the source (inserts not expanded) */
static const char *win32_error_text(u32 code) {
  if ((code & 0xFFFF0000u) == 0x80070000u) code &= 0xFFFF;
  switch (code) {
    case 0: return "The operation completed successfully.";
    case 1: return "Incorrect function."; case 2: return "The system cannot find the file specified."; case 3: return "The system cannot find the path specified.";
    case 4: return "The system cannot open the file."; case 5: return "Access is denied."; case 6: return "The handle is invalid."; case 8: return "Not enough memory resources are available to process this command.";
    case 13: return "The data is invalid."; case 14: return "Not enough storage is available to complete this operation."; case 17: return "The system cannot move the file to a different disk drive.";
    case 18: return "There are no more files."; case 19: return "The media is write protected."; case 23: return "Data error (cyclic redundancy check)."; case 32: return "The process cannot access the file because it is being used by another process.";
    case 33: return "The process cannot access the file because another process has locked a portion of the file."; case 38: return "Reached the end of the file."; case 50: return "The request is not supported.";
    case 80: return "The file exists."; case 82: return "The directory or file cannot be created."; case 87: return "The parameter is incorrect."; case 109: return "The pipe has been ended."; case 110: return "The system cannot open the device or file specified.";
    case 111: return "The file name is too long."; case 112: return "There is not enough space on the disk."; case 120: return "This function is not supported on this system."; case 122: return "The data area passed to a system call is too small.";
    case 123: return "The filename, directory name, or volume label syntax is incorrect."; case 126: return "The specified module could not be found."; case 127: return "The specified procedure could not be found."; case 145: return "The directory is not empty.";
    case 183: return "Cannot create a file when that file already exists."; case 193: return "%1 is not a valid Win32 application."; case 206: return "The filename or extension is too long."; case 232: return "The pipe is being closed.";
    case 233: return "No process is on the other end of the pipe."; case 234: return "More data is available."; case 258: return "The wait operation timed out."; case 259: return "No more data is available."; case 267: return "The directory name is invalid.";
    case 998: return "Invalid access to memory location."; case 1008: return "An attempt was made to reference a token that does not exist."; case 1113: return "No mapping for the Unicode character exists in the target multi-byte code page.";
    case 1114: return "A dynamic link library (DLL) initialization routine failed."; case 1168: return "Element not found."; case 1223: return "The operation was canceled by the user."; case 1235: return "The request was aborted.";
    case 1314: return "A required privilege is not held by the client."; case 1392: return "The file or directory is corrupted and unreadable."; case 1450: return "Insufficient system resources exist to complete the requested service.";
    case 0x80004001: return "Not implemented"; case 0x80004002: return "No such interface supported"; case 0x80004003: return "Invalid pointer"; case 0x80004004: return "Operation aborted"; case 0x80004005: return "Unspecified error";
    case 0x8000FFFF: return "Catastrophic failure"; case 0x80070057: return "The parameter is incorrect."; case 0x8007000E: return "Not enough memory resources are available to complete this operation.";
    default: return 0;
  }
}
static void format_message(Cpu *c, int wide) {
  Win32 *w = W; u32 flags = (u32)ARG(0); u64 src = ARG(1); u32 id = (u32)ARG(2); u64 buf = ARG(4); u32 size = (u32)ARG(5);
  char tmp[512]; const char *text = 0;
  if (flags & 0x1000) text = win32_error_text(id);
  else if ((flags & 0x400) && src) { if (wide) { u32 i = 0; for (; i + 1 < sizeof tmp; i++) { u16 ch = rd16(c, src + 2 * i); if (!ch) break; tmp[i] = ch < 128 ? (char)ch : '?'; } tmp[i] = 0; } else { u32 i = 0; for (; i + 1 < sizeof tmp; i++) { u8 ch = rd8(c, src + i); if (!ch) break; tmp[i] = (char)ch; } tmp[i] = 0; } text = tmp; }
  if (!text) { if (flags & 0x1000) { e_snprintf(tmp, sizeof tmp, "Unknown error 0x%08x", id); text = tmp; } else { w->last_error = 317; RET(0); return; } }
  char msg[600]; e_snprintf(msg, sizeof msg, "%s%s", text, (flags & 0xFF) == 0 && (flags & 0x1000) ? "\r\n" : "");   /* FORMAT_MESSAGE_MAX_WIDTH_MASK 0 keeps the trailing CRLF */
  u32 n = (u32)e_strlen(msg);
  if (flags & 0x100) {   /* ALLOCATE_BUFFER: *buffer = LocalAlloc'd string */
    u64 a = w32_heap_alloc(w, (u64)(n + 1) * (wide ? 2 : 1), 1); if (!a) { RET(0); return; }
    for (u32 i = 0; i <= n; i++) { if (wide) wr16(c, a + 2 * i, (u8)msg[i]); else wr8(c, a + i, (u8)msg[i]); }
    wrp(c, buf, a); RET(n); return;
  }
  if (!buf || !size) { w->last_error = 122; RET(0); return; }
  if (n >= size) n = size - 1;
  for (u32 i = 0; i < n; i++) { if (wide) wr16(c, buf + 2 * i, (u8)msg[i]); else wr8(c, buf + i, (u8)msg[i]); }
  if (wide) wr16(c, buf + 2 * n, 0); else wr8(c, buf + n, 0);
  RET(n);
}
static void s_FormatMessageA(Cpu *c) { format_message(c, 0); }
static void s_FormatMessageW(Cpu *c) { format_message(c, 1); }
static void s_SetErrorMode(Cpu *c) { RET(0); }
static void s_GetErrorMode(Cpu *c) { RET(0); }
static void s_SetThreadErrorMode(Cpu *c) { RET(1); }
static void s_GetProcessAffinityMask(Cpu *c) { wrp(c, ARG(1), 1); wrp(c, ARG(2), 1); RET(1); }
static void s_SetThreadAffinityMask(Cpu *c) { RET(1); }
static void s_SetProcessAffinityMask(Cpu *c) { RET(1); }
static void s_GetSystemTimeAdjustment(Cpu *c) { wr32(c, ARG(0), 156250); wr32(c, ARG(1), 156250); wr32(c, ARG(2), 1); RET(1); }
static void s_SetSystemTime(Cpu *c) { W->last_error = 1314; RET(0); }   /* ERROR_PRIVILEGE_NOT_HELD */
static void s_GetThreadPriority(Cpu *c) { RET(0); }
static void s_SetThreadPriority(Cpu *c) { RET(1); }
static void s_SetPriorityClass(Cpu *c) { RET(1); }
static void s_GetPriorityClass(Cpu *c) { RET(0x20); }
static void s_GetThreadTimes(Cpu *c) { call_shim(c, s_GetProcessTimes, ~0ull, ARG(1), ARG(2), ARG(3)); }
static void s_GetProcessHeap(Cpu *c) { RET(0x9000); }
static void s_HeapCreate(Cpu *c) { RET(0x9000 + ++W->next_handle); }
static void s_HeapDestroy(Cpu *c) { RET(1); }
static void s_HeapAlloc(Cpu *c) { RET(w32_heap_alloc(W, ARG(2), !!(ARG(1) & 8))); }
static void s_HeapFree(Cpu *c) { if (ARG(2)) w32_heap_free(W, ARG(2)); RET(1); }
static void s_HeapReAlloc(Cpu *c) { RET(heap_realloc(W, ARG(2), ARG(3), !!(ARG(1) & 8))); }
static void s_HeapSize(Cpu *c) { RET(w32_heap_size(W, ARG(2))); }
static void s_HeapValidate(Cpu *c) { RET(1); }
static void s_HeapSetInformation(Cpu *c) { RET(1); }
static void s_HeapQueryInformation(Cpu *c) { RET(0); }
static void s_HeapCompact(Cpu *c) { RET(0); }
static void s_HeapLock(Cpu *c) { RET(1); }
static void s_HeapUnlock(Cpu *c) { RET(1); }
static void s_HeapWalk(Cpu *c) { RET(0); }
static void s_GlobalAlloc(Cpu *c) { RET(w32_heap_alloc(W, ARG(1), !!(ARG(0) & 0x40))); }
static void s_GlobalFree(Cpu *c) { w32_heap_free(W, ARG(0)); RET(0); }
static void s_GlobalLock(Cpu *c) { RET(ARG(0)); }
static void s_GlobalUnlock(Cpu *c) { RET(1); }
static void s_LocalAlloc(Cpu *c) { RET(w32_heap_alloc(W, ARG(1), !!(ARG(0) & 0x40))); }
static void s_LocalFree(Cpu *c) { w32_heap_free(W, ARG(0)); RET(0); }
/* VirtualAlloc: every reservation is tracked so that (a) MEM_COMMIT inside an earlier reservation succeeds, (b) an address hint
 * that points into the image / stack / heap FAILS with ERROR_INVALID_ADDRESS like on Windows (V8 probes hints near its embedded
 * builtins and would otherwise build its code-range bookkeeping on top of node.exe's .text), (c) a hint at a free address is honoured. */
#define VM_MAX 2048
static struct { u64 base, size, heap; int released, mapped; } g_vm[VM_MAX]; static int g_nvm;   /* heap: backing heap block (small), mapped: own region (large) */
static u64 g_vm_cursor = 0x600000000ull;   /* where large anonymous reservations are placed */
static u64 g_vm_cursor32 = 0x48000000ull;  /* 32-bit process: reservations between the heap (0x20000000+) and the DLL area (0x60000000) */
static int vm_find(u64 a) { int rel = -1; for (int i = 0; i < g_nvm; i++) if (a - g_vm[i].base < g_vm[i].size) { if (!g_vm[i].released) return i; rel = i; } return rel; }
static int vm_range_free(Mem *m, u64 a, u64 size) { for (int i = 0; i < m->n; i++) { Region *r = &m->r[i]; if (a < r->base + r->size && r->base < a + size) return 0; } return 1; }
static void vm_recycle(Win32 *w); static void vm_discard(Cpu *c, u64 addr, u64 size);
static int vm_add(u64 base, u64 size, u64 heap) { if (g_nvm >= VM_MAX) return -1; g_vm[g_nvm].base = base; g_vm[g_nvm].size = size; g_vm[g_nvm].heap = heap; g_vm[g_nvm].released = 0; g_vm[g_nvm].mapped = 0; return g_nvm++; }
static void s_VirtualAlloc(Cpu *c) { u64 addr = ARG(0), size = ARG(1);
  if (addr) {
    u64 a0 = addr & ~0xfffull, end = (addr + size + 0xfff) & ~0xfffull; int vi = vm_find(a0);
    if (vi >= 0 && !g_vm[vi].released) { if (end <= g_vm[vi].base + g_vm[vi].size) { if ((u32)ARG(2) & 0x80000) vm_discard(c, a0, end - a0); RET(a0); return; } W->last_error = 487; RET(0); return; }   /* MEM_RESET: contents may be dropped */
    if (vi >= 0) {   /* inside a RELEASED reservation: V8 re-reserves an aligned sub-range of a block it just freed — hand that sub-range out again */
      if (end <= g_vm[vi].base + g_vm[vi].size && vm_add(a0, end - a0, 0) >= 0) { u8 *p = mem_ptr(c->mem, a0, end - a0, 1); if (p) e_memset(p, 0, (size_t)(end - a0)); RET(a0); return; }
      W->last_error = 487; RET(0); return; }
    if (mem_ptr(c->mem, a0, 1, 0)) { W->last_error = 487; RET(0); return; }   /* image / stack / heap / DLL: not available (Windows: ERROR_INVALID_ADDRESS) */
    /* a hint into free address space: V8 scatters these randomly (ASLR) and falls back to VirtualAlloc(NULL) when they fail, so only
     * honour large ones (alignment-sensitive reservations); small pages would each cost a region (MAX_REGIONS) and slow down mem_ptr */
    u64 a64 = addr & ~0xffffull, len = (end - a64 + 0xffff) & ~0xffffull;
    if (len >= (1ull << 20) && c->mem->n < MAX_REGIONS - 8 && vm_range_free(c->mem, a64, len)) { int vi2 = vm_add(a64, len, 0); if (vi2 >= 0 && mem_map(c->mem, a64, len, "vm")) { g_vm[vi2].mapped = 1; RET(a64); return; } }
    W->last_error = 487; RET(0); return;
  }
  vm_recycle(W);
  size = (size + 0xffff) & ~0xffffull;
  if (size >= (1ull << 20) && c->mem->n < MAX_REGIONS - 8) {   /* large reservation (V8 heap / code range): own region, pages untouched until used (host zero pages stay lazy) */
    u64 base = c->mode32 ? g_vm_cursor32 : g_vm_cursor; int tries = 0; while (!vm_range_free(c->mem, base, size) && tries++ < 4096) base += (size + 0xffffff) & ~0xffffffull;
    if (c->mode32 && base + size > 0x60000000ull) tries = 4096;   /* no room below the DLL area: fall through to the heap (which fails cleanly when full) */
    if (tries < 4096) { int vi2 = vm_add(base, size, 0); if (vi2 >= 0 && mem_map(c->mem, base, size, "vm")) { g_vm[vi2].mapped = 1; if (c->mode32) g_vm_cursor32 = base + ((size + 0xffffff) & ~0xffffffull); else g_vm_cursor = base + ((size + 0xffffff) & ~0xffffffull); RET(base); return; } if (vi2 >= 0) g_nvm--; }
  }
  u64 a = w32_heap_alloc(W, size + 0x10000, 1); if (!a) { e_log("[x64] VirtualAlloc(NULL, %llu) failed: regions=%d vm=%d\n", (unsigned long long)size, c->mem->n, g_nvm); W->last_error = 8; RET(0); return; }
  u64 r = (a + 0xffff) & ~0xffffull; if (vm_add(r, size, a) < 0) e_log("[x64] VM table full (%d)\n", g_nvm);
  RET(r); }
static void s_VirtualAllocEx(Cpu *c) { call_shim(c, s_VirtualAlloc, ARG(1), ARG(2), ARG(3), ARG(4)); }
static void vm_recycle(Win32 *w) {   /* free released heap-backed reservations that no active reservation overlaps (deferred, so a hint can re-reserve a just-freed range) */
  for (int i = 0; i < g_nvm; i++) { if (!g_vm[i].released || (!g_vm[i].heap && !g_vm[i].mapped)) continue; int busy = 0;
    for (int j = 0; j < g_nvm; j++) if (j != i && !g_vm[j].released && g_vm[j].base < g_vm[i].base + g_vm[i].size && g_vm[i].base < g_vm[j].base + g_vm[j].size) { busy = 1; break; }
    if (busy) continue;
    u64 b = g_vm[i].base, e = b + g_vm[i].size; if (g_vm[i].heap) w32_heap_free(w, g_vm[i].heap); else mem_unmap(w->mem, b); g_vm[i] = g_vm[--g_nvm]; i = -1;   /* restart: drop released sub-entries inside it too */
    for (int j = 0; j < g_nvm; j++) if (g_vm[j].released && !g_vm[j].heap && !g_vm[j].mapped && g_vm[j].base >= b && g_vm[j].base < e) { g_vm[j] = g_vm[--g_nvm]; j--; } } }
static void vm_discard(Cpu *c, u64 addr, u64 size) {   /* MEM_DECOMMIT / MEM_RESET: JSC and mimalloc decommit whole heap blocks all the time — without this every page they ever touched stays resident */
  u64 a = addr & ~0xfffull; int vi = vm_find(a); if (vi < 0) return;
  u64 end = size ? (addr + size + 0xfff) & ~0xfffull : g_vm[vi].base + g_vm[vi].size; if (end > g_vm[vi].base + g_vm[vi].size) end = g_vm[vi].base + g_vm[vi].size; if (end <= a) return;
  u8 *p = mem_ptr(c->mem, a, end - a, 1); if (p) host_mem_discard(p, (size_t)(end - a)); }
static void s_VirtualFree(Cpu *c) { u64 addr = ARG(0), size = ARG(1); u32 type = (u32)ARG(2);
  if (type == 0x8000) { int vi = vm_find(addr); if (vi >= 0 && !g_vm[vi].released && g_vm[vi].base == addr) g_vm[vi].released = 1; }   /* MEM_RELEASE */
  else if (type & 0x4000) vm_discard(c, addr, size);   /* MEM_DECOMMIT */
  RET(1); }
static void s_VirtualProtect(Cpu *c) { if (ARG(3)) wr32(c, ARG(3), 0x40); RET(1); }
static void s_VirtualLock(Cpu *c) { RET(1); }
static void s_VirtualUnlock(Cpu *c) { RET(1); }
/* VirtualQuery: see win32_clr.inc (stack-aware) */
static void s_FlushInstructionCache(Cpu *c) { RET(1); }
static void s_CreateTimerQueue(Cpu *c) { RET(0); }
static void s_GetThreadLocale(Cpu *c) { RET(0x409); }
static void s_GetUserDefaultLCID(Cpu *c) { RET(0x409); }
static void s_GetUserDefaultLangID(Cpu *c) { RET(0x409); }
static void s_GetSystemDefaultLangID(Cpu *c) { RET(0x409); }
static void s_GetUserDefaultUILanguage(Cpu *c) { RET(0x409); }
static void s_GetLocaleInfoA(Cpu *c) { RET(0); }
static void s_GetLocaleInfoW(Cpu *c) { RET(0); }
static void s_GetLocaleInfoEx(Cpu *c) { RET(0); }
static void s_IsValidLocale(Cpu *c) { RET(1); }
static void s_IsValidCodePage(Cpu *c) { RET(1); }
static void s_EnumSystemLocalesW(Cpu *c) { RET(1); }
static void s_GetUserDefaultLocaleName(Cpu *c) { const char *n = "en-US"; for (int i = 0; i <= 5; i++) wr16(c, ARG(0) + i * 2, (u8)n[i]); RET(6); }
static void s_LCMapStringW(Cpu *c) { u32 flags = (u32)ARG(1); u64 src = ARG(2); int n = (int)(s32)ARG(3); u64 dst = ARG(4); int dn = (int)ARG(5);
  int len = n; if (n < 0) { len = 0; while (rd16(c, src + len * 2)) len++; len++; }
  if (dn == 0) { RET(len); return; } if (dn < len) { RET(0); return; }
  for (int i = 0; i < len; i++) { u16 ch = rd16(c, src + i * 2); if ((flags & 0x100) && ch >= 'a' && ch <= 'z') ch -= 32; if ((flags & 0x200) && ch >= 'A' && ch <= 'Z') ch += 32; wr16(c, dst + i * 2, ch); } RET(len); }
static void s_LCMapStringEx(Cpu *c) { s_LCMapStringW(c); }
static void s_LCMapStringA(Cpu *c) { u32 flags = (u32)ARG(1); u64 src = ARG(2); int n = (int)(s32)ARG(3); u64 dst = ARG(4); int dn = (int)ARG(5);
  int len = n; if (n < 0) { len = 0; while (rd8(c, src + len)) len++; len++; }
  if (dn == 0) { RET(len); return; } if (dn < len) { RET(0); return; }
  for (int i = 0; i < len; i++) { u8 ch = rd8(c, src + i); if ((flags & 0x100) && ch >= 'a' && ch <= 'z') ch -= 32; if ((flags & 0x200) && ch >= 'A' && ch <= 'Z') ch += 32; wr8(c, dst + i, ch); } RET(len); }
static void s_CompareStringW(Cpu *c) { u64 a = ARG(2); int an = (int)(s32)ARG(3); u64 b = ARG(4); int bn = (int)(s32)ARG(5); int i = 0;
  for (;; i++) { u16 x = (an < 0 || i < an) ? rd16(c, a + i * 2) : 0, y = (bn < 0 || i < bn) ? rd16(c, b + i * 2) : 0; if (ARG(1) & 1) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; } if (x != y) { RET(x < y ? 1 : 3); return; } if (!x) break; } RET(2); }
static void s_CompareStringA(Cpu *c) { u64 a = ARG(2); int an = (int)(s32)ARG(3); u64 b = ARG(4); int bn = (int)(s32)ARG(5); int i = 0;
  for (;; i++) { u8 x = (an < 0 || i < an) ? rd8(c, a + i) : 0, y = (bn < 0 || i < bn) ? rd8(c, b + i) : 0; if (ARG(1) & 1) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; } if (x != y) { RET(x < y ? 1 : 3); return; } if (!x) break; } RET(2); }
static void s_CompareStringEx(Cpu *c) { s_CompareStringW(c); }
/* CompareStringOrdinal(a, an, b, bn, ignoreCase) */
static void s_CompareStringOrdinal(Cpu *c) { u64 a = ARG(0); int an = (int)(s32)ARG(1); u64 b = ARG(2); int bn = (int)(s32)ARG(3); int ic = (int)ARG(4);
  for (int i = 0;; i++) { u16 x = (an < 0 || i < an) ? rd16(c, a + i * 2) : 0, y = (bn < 0 || i < bn) ? rd16(c, b + i * 2) : 0; if (ic) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; } if (x != y) { RET(x < y ? 1 : 3); return; } if (!x) break; } RET(2); }
static void s_GetStringTypeW(Cpu *c) { u32 type = (u32)ARG(0); u64 src = ARG(1); int n = (int)(s32)ARG(2); u64 dst = ARG(3);
  if (n < 0) { n = 0; while (rd16(c, src + n * 2)) n++; n++; }
  for (int i = 0; i < n; i++) { u16 ch = rd16(c, src + i * 2); u16 t = 0;
    if (type == 1) { if (ch >= 'A' && ch <= 'Z') t |= 0x1 | 0x100; if (ch >= 'a' && ch <= 'z') t |= 0x2 | 0x100; if (ch >= '0' && ch <= '9') t |= 0x4 | 0x200; if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 0x8 | 0x40; if (ch < 32 || ch == 127) t |= 0x20; if ((ch >= 33 && ch <= 47) || (ch >= 58 && ch <= 64) || (ch >= 91 && ch <= 96) || (ch >= 123 && ch <= 126)) t |= 0x10; if (ch >= 128) t |= 0x100 | 0x1; if (ch == ' ') t |= 0x48; }
    wr16(c, dst + i * 2, t); } RET(1); }
static void s_GetStringTypeA(Cpu *c) { u32 type = (u32)ARG(1); u64 src = ARG(2); int n = (int)(s32)ARG(3); u64 dst = ARG(4);
  if (n < 0) { n = 0; while (rd8(c, src + n)) n++; n++; }
  for (int i = 0; i < n; i++) { u8 ch = rd8(c, src + i); u16 t = 0; if (type == 1) { if (ch >= 'A' && ch <= 'Z') t |= 0x101; if (ch >= 'a' && ch <= 'z') t |= 0x102; if (ch >= '0' && ch <= '9') t |= 0x204; if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 0x48; if (ch < 32 || ch == 127) t |= 0x20; } wr16(c, dst + i * 2, t); } RET(1); }
static void s_MultiByteToWideChar(Cpu *c) { u64 src = ARG(2); int n = (int)(s32)ARG(3); u64 dst = ARG(4); int dn = (int)ARG(5);
  int si = 0, out = 0; for (;;) { if (n >= 0 && si >= n) break; u32 b = rd8(c, src + si); if (n < 0 && !b) { si++; if (dn) { if (out < dn) wr16(c, dst + out * 2, 0); else { W->last_error = 122; RET(0); return; } } out++; break; }
    u32 cp = b; int extra = 0; if (b >= 0xF0) { cp = b & 7; extra = 3; } else if (b >= 0xE0) { cp = b & 15; extra = 2; } else if (b >= 0xC0) { cp = b & 31; extra = 1; } si++;
    for (int k = 0; k < extra; k++) { if (n >= 0 && si >= n) break; u8 nb = rd8(c, src + si); if ((nb & 0xC0) != 0x80) break; cp = (cp << 6) | (nb & 0x3f); si++; }
    if (cp >= 0x10000) { cp -= 0x10000; if (dn) { if (out + 1 < dn) { wr16(c, dst + out * 2, 0xD800 | (cp >> 10)); wr16(c, dst + out * 2 + 2, 0xDC00 | (cp & 0x3ff)); } else { W->last_error = 122; RET(0); return; } } out += 2; }
    else { if (dn) { if (out < dn) wr16(c, dst + out * 2, (u16)cp); else { W->last_error = 122; RET(0); return; } } out++; }
    if (c->status) return; }
  RET(out); }
static void s_WideCharToMultiByte(Cpu *c) { u64 src = ARG(2); int n = (int)(s32)ARG(3); u64 dst = ARG(4); int dn = (int)ARG(5); u64 used = ARG(7);
  int si = 0, out = 0; char tmp[4];
  for (;;) { if (n >= 0 && si >= n) break; u32 ch = rd16(c, src + si * 2); si++; if (n < 0 && !ch) { if (dn) { if (out < dn) wr8(c, dst + out, 0); else { W->last_error = 122; RET(0); return; } } out++; break; }
    if (ch >= 0xD800 && ch < 0xDC00 && (n < 0 || si < n)) { u32 lo = rd16(c, src + si * 2); if (lo >= 0xDC00 && lo < 0xE000) { ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00); si++; } }
    int l; if (ch < 0x80) { tmp[0] = (char)ch; l = 1; } else if (ch < 0x800) { tmp[0] = (char)(0xC0 | ch >> 6); tmp[1] = (char)(0x80 | (ch & 0x3f)); l = 2; } else if (ch < 0x10000) { tmp[0] = (char)(0xE0 | ch >> 12); tmp[1] = (char)(0x80 | ((ch >> 6) & 0x3f)); tmp[2] = (char)(0x80 | (ch & 0x3f)); l = 3; } else { tmp[0] = (char)(0xF0 | ch >> 18); tmp[1] = (char)(0x80 | ((ch >> 12) & 0x3f)); tmp[2] = (char)(0x80 | ((ch >> 6) & 0x3f)); tmp[3] = (char)(0x80 | (ch & 0x3f)); l = 4; }
    if ((u32)ARG(0) != 65001 && ch >= 0x80) { tmp[0] = ch < 0x100 ? (char)ch : '?'; l = 1; }
    if (dn) { if (out + l <= dn) for (int k = 0; k < l; k++) wr8(c, dst + out + k, (u8)tmp[k]); else { W->last_error = 122; RET(0); return; } } out += l; if (c->status) return; }
  if (used) wr32(c, used, 0); RET(out); }
static void s_CharUpperA(Cpu *c) { u64 a = ARG(0); if (a < 0x10000) { u8 ch = (u8)a; RET(ch >= 'a' && ch <= 'z' ? ch - 32 : ch); return; } for (u64 i = 0;; i++) { u8 ch = rd8(c, a + i); if (!ch) break; if (ch >= 'a' && ch <= 'z') wr8(c, a + i, ch - 32); } RET(a); }
static void s_lstrlenA(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); RET(s ? e_strlen(s) : 0); }
static void s_lstrlenW(Cpu *c) { int n = 0; while (rd16(c, ARG(0) + n * 2)) n++; RET(n); }
static void s_lstrcpyA(Cpu *c) { const char *s = rd_str(c, ARG(1), 1 << 20); if (s) { size_t l = e_strlen(s) + 1; u8 *d = mem_ptr(c->mem, ARG(0), l, 1); if (d) e_memmove(d, s, l); } RET(ARG(0)); }
static void s_lstrcmpiA(Cpu *c) { const char *a = rd_str(c, ARG(0), 1 << 20), *b = rd_str(c, ARG(1), 1 << 20); RET(a && b ? e_stricmp(a, b) : 0); }
static void s_lstrcmpA(Cpu *c) { const char *a = rd_str(c, ARG(0), 1 << 20), *b = rd_str(c, ARG(1), 1 << 20); RET(a && b ? e_strcmp(a, b) : 0); }
static void s_InterlockedCompareExchange(Cpu *c) { u32 old = rd32(c, ARG(0)); if (old == (u32)ARG(2)) wr32(c, ARG(0), (u32)ARG(1)); RET(old); }
static void s_InterlockedExchange(Cpu *c) { u32 old = rd32(c, ARG(0)); wr32(c, ARG(0), (u32)ARG(1)); RET(old); }
static void s_InterlockedIncrement(Cpu *c) { u32 v = rd32(c, ARG(0)) + 1; wr32(c, ARG(0), v); RET(v); }
static void s_InterlockedDecrement(Cpu *c) { u32 v = rd32(c, ARG(0)) - 1; wr32(c, ARG(0), v); RET(v); }
static void s_InterlockedExchangeAdd(Cpu *c) { u32 v = rd32(c, ARG(0)); wr32(c, ARG(0), v + (u32)ARG(1)); RET(v); }
static void s_QueueUserAPC(Cpu *c) { if (ARG(1) != ~1ull) { W->last_error = 87; RET(0); return; } queue_apc(W, ARG(0), ARG(2), 0, 0); RET(1); }   /* only the current thread */
static void s_GetTimeZoneInformation(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(0), 172, 1); if (p) e_memset(p, 0, 172); RET(0); }
static void s_FileTimeToSystemTime(Cpu *c) { fill_systemtime(c, ARG(1)); RET(1); }
static void s_SystemTimeToFileTime(Cpu *c) { wr64(c, ARG(1), now_100ns()); RET(1); }
static void s_FileTimeToLocalFileTime(Cpu *c) { wr64(c, ARG(1), rd64(c, ARG(0))); RET(1); }
static void s_GetDateFormatA(Cpu *c) { RET(0); }
static void s_GetTimeFormatA(Cpu *c) { RET(0); }
static void s_BCryptGenRandom(Cpu *c) { Win32 *w = W; u64 a = ARG(1); u32 n = (u32)ARG(2); for (u32 i = 0; i < n; i++) { w->rand_state = w->rand_state * 1103515245u + 12345u; wr8(c, a + i, (u8)(w->rand_state >> 16)); } RET(0); }
static void s_SystemFunction036(Cpu *c) { Win32 *w = W; u64 a = ARG(0); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) { w->rand_state = w->rand_state * 1103515245u + 12345u; wr8(c, a + i, (u8)(w->rand_state >> 16)); } RET(1); }
static void s_GetComputerNameA(Cpu *c) { const char *n = "NEBULA"; u32 sz = rd32(c, ARG(1)); if (sz > 6) { u8 *p = mem_ptr(c->mem, ARG(0), 7, 1); if (p) e_memcpy(p, n, 7); wr32(c, ARG(1), 6); RET(1); } else RET(0); }
static void s_GetUserNameA(Cpu *c) { const char *n = "user"; u32 sz = rd32(c, ARG(1)); if (sz > 4) { u8 *p = mem_ptr(c->mem, ARG(0), 5, 1); if (p) e_memcpy(p, n, 5); wr32(c, ARG(1), 5); RET(1); } else RET(0); }
static void s_GetDriveTypeA(Cpu *c) { RET(3); }
static void s_GetLogicalDrives(Cpu *c) { RET(4); }
static void s_GetDiskFreeSpaceExA(Cpu *c) { RET(0); }
static void s_SetHandleCount(Cpu *c) { RET(ARG(0)); }
/* GetOverlappedResult(h, ov, &bytes, bWait): reads OVERLAPPED.Internal (NTSTATUS) / InternalHigh (bytes); with bWait the thread is
 * re-scheduled every 2 ms until the parked operation (pipe/file/socket) has been completed by the scheduler's I/O poll */
static void s_GetOverlappedResult(Cpu *c) { Win32 *w = W; u64 ov = ARG(1); if (!ov) { w->sleeping = 0; w->last_error = 87; RET(0); return; }
  u64 st = rdp(c, ov);
  if (st == 0x103) { if (ARG(3)) { w->sleeping = 1; w->sleep_until_ms = host_now_ms() + 2; w->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot); return; } w->sleeping = 0; w->last_error = 996; RET(0); return; }
  w->sleeping = 0; if (ARG(2)) wr32(c, ARG(2), (u32)rdp(c, OV_HIGH(ov)));
  if ((u32)st == 0 || ((u32)st & 0xC0000000u) != 0xC0000000u) { RET(1); return; }
  w->last_error = nt_to_dos((u32)st); RET(0); }
static void s_Beep(Cpu *c) { RET(1); }
static void s_DebugBreak(Cpu *c) { c->status = RUN_BREAK; }
/* PROCESS_MEMORY_COUNTERS(_EX): cb @0, PageFaultCount @4, PeakWorkingSetSize @8, WorkingSetSize @16, QuotaPeakPaged @24, QuotaPaged @32, QuotaPeakNonPaged @40,
   QuotaNonPaged @48, PagefileUsage @56, PeakPagefileUsage @64, PrivateUsage @72 (EX, 80 B). JSC/bmalloc derive "memory pressure" from this: an unfilled
   struct made the GC believe the process was critically low on memory and collect after every few KB. */
static void s_K32GetProcessMemoryInfo(Cpu *c) { u64 a = ARG(1); u32 cb = (u32)ARG(2); if (!a || cb < 72) { W->last_error = 24; RET(0); return; }
  u64 used = emu_alloc_used(); if (used < (64ull << 20)) used = 64ull << 20;
  wr32(c, a, cb); wr32(c, a + 4, 1000); wr64(c, a + 8, used); wr64(c, a + 16, used); wr64(c, a + 24, 65536); wr64(c, a + 32, 65536); wr64(c, a + 40, 16384); wr64(c, a + 48, 16384); wr64(c, a + 56, used); wr64(c, a + 64, used); if (cb >= 80) wr64(c, a + 72, used); RET(1); }

/* ================= UCRT / msvcrt ================= */
static void s_malloc(Cpu *c) { RET(w32_heap_alloc(W, ARG(0), 0)); }
static void s_calloc(Cpu *c) { RET(w32_heap_alloc(W, ARG(0) * ARG(1), 1)); }
static void s_realloc(Cpu *c) { if (!ARG(1)) { w32_heap_free(W, ARG(0)); RET(0); return; } RET(heap_realloc(W, ARG(0), ARG(1), 0)); }
static void s_free(Cpu *c) { if (ARG(0)) w32_heap_free(W, ARG(0)); }
static void s_msize(Cpu *c) { RET(w32_heap_size(W, ARG(0))); }
static void s_aligned_malloc(Cpu *c) { u64 al = ARG(1); if (al < 16) al = 16; u64 a = w32_heap_alloc(W, ARG(0) + al, 0); RET((a + al - 1) & ~(al - 1)); }
static void s_aligned_free(Cpu *c) { }
static void s_set_new_mode(Cpu *c) { RET(0); }
static void s_CryptAcquireContextA(Cpu *c) { if (ARG(0)) wrp(c, ARG(0), 0xC0FFEE); RET(1); }
static void s_CryptGenRandom(Cpu *c) { Win32 *w = W; u64 a = ARG(2); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) { w->rand_state = w->rand_state * 1103515245u + 12345u; wr8(c, a + i, (u8)(w->rand_state >> 16)); } RET(1); }
static void s_OpenProcessToken(Cpu *c) { if (ARG(2)) wrp(c, ARG(2), 0x7001); RET(1); }
/* GetTokenInformation: one administrator account. TokenUser → S-1-5-21-1000-2000-3000-500; TokenGroups → Administrators only; TokenElevation 1;
   TokenElevationType 2 (full); TokenIntegrityLevel high (S-1-16-12288); TokenSessionId 1; TokenType primary; everything else ERROR_NOT_SUPPORTED. */
static u32 sid_put(Cpu *c, u64 at, u32 nsub, const u32 *sub) { if (at) { wr8(c, at, 1); wr8(c, at + 1, (u8)nsub); for (int i = 0; i < 6; i++) wr8(c, at + 2 + i, i == 5 ? 5 : 0); for (u32 i = 0; i < nsub; i++) wr32(c, at + 8 + i * 4, sub[i]); } return 8 + nsub * 4; }
static void s_GetTokenInformation(Cpu *c) { u32 cls = (u32)ARG(1); u64 buf = ARG(2); u32 cap = (u32)ARG(3); u64 P = PSZ_OF(c); u32 need = 0; static const u32 user[5] = { 21, 1000, 2000, 3000, 500 }, admins[2] = { 32, 544 }, high[1] = { 12288 };
  switch (cls) {
    case 1: need = (u32)(2 * P) + 28; if (cap >= need && buf) { wrp(c, buf, buf + 2 * P); wr32(c, buf + P, 0); sid_put(c, buf + 2 * P, 5, user); } break;                       /* TOKEN_USER */
    case 2: need = (u32)(4 + 2 * P) + 16; if (cap >= need && buf) { wr32(c, buf, 1); u64 e = buf + P; wrp(c, e, buf + 4 + 2 * P); wr32(c, e + P, 0x7 | 0x20000000); sid_put(c, buf + 4 + 2 * P, 2, admins); } break;   /* TOKEN_GROUPS: Administrators, enabled+owner */
    case 4: case 5: need = (u32)P + 16; if (cap >= need && buf) { wrp(c, buf, buf + P); sid_put(c, buf + P, 2, admins); } break;                                    /* TOKEN_OWNER / PRIMARY_GROUP */
    case 8: need = 4; if (cap >= need && buf) wr32(c, buf, 1); break;                                                                                                /* TokenType = primary */
    case 12: need = 4; if (cap >= need && buf) wr32(c, buf, 1); break;                                                                                               /* TokenSessionId */
    case 18: need = 4; if (cap >= need && buf) wr32(c, buf, 2); break;                                                                                               /* TokenElevationType = full */
    case 20: need = 4; if (cap >= need && buf) wr32(c, buf, 1); break;                                                                                               /* TokenElevation */
    case 22: need = 4; if (cap >= need && buf) wr32(c, buf, 0); break;                                                                                               /* TokenVirtualizationEnabled */
    case 25: need = (u32)(2 * P) + 12; if (cap >= need && buf) { wrp(c, buf, buf + 2 * P); wr32(c, buf + P, 0x60); sid_put(c, buf + 2 * P, 1, high); } break;     /* TokenIntegrityLevel */
    case 26: need = 4; if (cap >= need && buf) wr32(c, buf, 0); break;                                                                                               /* TokenUIAccess */
    case 19: need = (u32)P + 16; if (cap >= need && buf) { wrp(c, buf, 0); } break;                                                                                  /* TokenLinkedToken: none */
    default: if (ARG(4)) wr32(c, ARG(4), 0); W->last_error = 50; RET(0); return; }
  if (ARG(4)) wr32(c, ARG(4), need); if (cap < need || !buf) { W->last_error = 122; RET(0); return; } RET(1); }
/* SDDL → SECURITY_DESCRIPTOR: the string is not parsed; a self-relative descriptor with owner = Administrators, no DACL (everyone full access) is returned. Installers
   use it to create protected temp directories — the protection itself does not exist here, the call succeeds so the directory gets created. */
static void s_ConvertStringSecurityDescriptorToSecurityDescriptorW(Cpu *c) { u64 out = ARG(2); if (!out) { W->last_error = 87; RET(0); return; } u64 sd = w32_heap_alloc(W, 64, 1); if (!sd) { RET(0); return; } static const u32 admins[2] = { 32, 544 };
  wr8(c, sd, 1); wr8(c, sd + 1, 0); wr16(c, sd + 2, 0x8000 /* SE_SELF_RELATIVE */); wr32(c, sd + 4, 20); wr32(c, sd + 8, 20); wr32(c, sd + 12, 0); wr32(c, sd + 16, 0); sid_put(c, sd + 20, 2, admins); wrp(c, out, sd); if (ARG(3)) wr32(c, ARG(3), 36); RET(1); }
static void s_ConvertStringSecurityDescriptorToSecurityDescriptorA(Cpu *c) { s_ConvertStringSecurityDescriptorToSecurityDescriptorW(c); }
static void s_retm1e(Cpu *c) { RET((u64)(s64)-1); wr32(c, W->ubuf + 0x258, 2); }
static void s_umask(Cpu *c) { RET(0); }
static void s_wcsnicmp(Cpu *c) { u64 n = ARG(2); u64 a = ARG(0), b = ARG(1); for (u64 i = 0; i < n; i++) { u16 x = rd16(c, a + i * 2), y = rd16(c, b + i * 2); if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) { RET((u64)(s64)(x - y)); return; } if (!x) break; } RET(0); }
static void s_telli64(Cpu *c) { RET(0); }
/* setlocale(category, name): only the "C" locale exists; NULL/""/"C"/".UTF-8"/anything → "C" (the UCRT returns the name actually selected) */
static void s_setlocale(Cpu *c) { Win32 *w = W; if (!w->loc_c) { w->loc_c = shim_bump(w, 16); u8 *p = mem_ptr(w->mem, w->loc_c, 2, 1); if (p) { p[0] = 'C'; p[1] = 0; } } RET(w->loc_c); }
static void s__wsetlocale(Cpu *c) { Win32 *w = W; if (!w->loc_wc) { w->loc_wc = shim_bump(w, 16); wr16(c, w->loc_wc, 'C'); wr16(c, w->loc_wc + 2, 0); } RET(w->loc_wc); }
static void s_localeconv(Cpu *c) { u64 b = W->ubuf + 0x700; if (!rd64(c, b)) { wr8(c, b + 0xa0, '.'); wr8(c, b + 0xa1, 0); wr8(c, b + 0xa2, 0); wr64(c, b, b + 0xa0); for (int i = 1; i < 9; i++) wr64(c, b + i * 8, b + 0xa2); for (int i = 0; i < 8; i++) wr8(c, b + 72 + i, 127); } RET(b); }
/* WS2_32: no network — sockets fail cleanly */
static void s_WSAStartup(Cpu *c) { u64 d = ARG(1); if (d) { wr16(c, d, 0x0202); wr16(c, d + 2, 0x0202); } RET(0); }
static void s_sock_err(Cpu *c) { W->last_error = 10093; RET((u64)(s64)-1); }
static void s_gethostname(Cpu *c) { const char *h = "nebula"; if ((u32)ARG(1) > 6) { u8 *p = mem_ptr(c->mem, ARG(0), 7, 1); if (p) e_memcpy(p, h, 7); } RET(0); }
static void s_htons(Cpu *c) { u16 v = (u16)ARG(0); RET((u16)((v >> 8) | (v << 8))); }
static void s_htonl(Cpu *c) { RET(__builtin_bswap32((u32)ARG(0))); }
static void s_VerSetConditionMask(Cpu *c) { u64 mask = ARG(0); u32 type = (u32)ARG(1), cond = (u32)ARG(2) & 7; u64 shift = 0; for (u32 b = 0; b < 32; b++) if (type & (1u << b)) { shift = (u64)b * 3; break; } RET(mask | ((u64)cond << shift)); }
static void s_InterlockedPushEntrySList(Cpu *c) { u64 head = ARG(0), e = ARG(1); u64 old = rdp(c, head); wrp(c, e, old); wrp(c, head, e); RET(old); }
static void s_InterlockedPopEntrySList(Cpu *c) { u64 head = ARG(0); u64 e = rdp(c, head); if (e) wrp(c, head, rdp(c, e)); RET(e); }
static void s_InterlockedFlushSList(Cpu *c) { u64 head = ARG(0); u64 e = rdp(c, head); wrp(c, head, 0); RET(e); }
static void s_GetUserNameW(Cpu *c) { u32 sz = rd32(c, ARG(1)); if (sz > 4) { const char *n = "user"; for (int i = 0; i < 5; i++) wr16(c, ARG(0) + i * 2, (u8)n[i]); wr32(c, ARG(1), 5); RET(1); } else RET(0); }
static void s_GetComputerNameW(Cpu *c) { u32 sz = rd32(c, ARG(1)); if (sz > 6) { const char *n = "NEBULA"; for (int i = 0; i < 7; i++) wr16(c, ARG(0) + i * 2, (u8)n[i]); wr32(c, ARG(1), 6); RET(1); } else RET(0); }
static void s_GetComputerNameExW(Cpu *c) { call_shim(c, s_GetComputerNameW, ARG(1), ARG(2), 0, 0); }
static void s_GetSystemDirectoryW(Cpu *c) { const char *d = "C:\\Windows\\System32"; size_t l = e_strlen(d); if ((u32)ARG(1) > l) for (size_t i = 0; i <= l; i++) wr16(c, ARG(0) + i * 2, (u8)d[i]); RET(l); }
static void s_LocalFileTimeToFileTime(Cpu *c) { wr64(c, ARG(1), rd64(c, ARG(0))); RET(1); }
static void s_GlobalMemoryStatus(Cpu *c) { u64 a = ARG(0); wr32(c, a + 4, 30); wr64(c, a + 8, 4ull << 30); wr64(c, a + 16, 3ull << 30); wr64(c, a + 24, 4ull << 30); wr64(c, a + 32, 3ull << 30); wr64(c, a + 40, 8ull << 40); wr64(c, a + 48, 8ull << 40); }
static void s_ret0(Cpu *c) { RET(0); }
static void s_ret1(Cpu *c) { RET(1); }
static void s_ret2(Cpu *c) { RET(2); }                  /* ERROR_FILE_NOT_FOUND for Reg* */
static void s_ret50(Cpu *c) { RET(50); }                /* ERROR_NOT_SUPPORTED */
static void s_ret_eunavail(Cpu *c) { W->last_error = 1168; RET(0); }   /* ERROR_NOT_FOUND */
/* node.exe / libuv extras: ETW tracing, event log, SIDs/ACLs for named pipes, window station queries, IP helper */
static void s_EventRegister(Cpu *c) { if (ARG(3)) wr64(c, ARG(3), 0x1234); RET(0); }
static void s_GetProcessWindowStation(Cpu *c) { RET(0x5c); }
/* GetUserObjectInformationW(h, index, buf, len, &needed): UOI_FLAGS 1 → USEROBJECTFLAGS { fInherit, fReserved, dwFlags = WSF_VISIBLE 1 } 12 B; UOI_NAME 2 */
static void s_GetUserObjectInformationW(Cpu *c) { u32 idx = (u32)ARG(1); u64 b = ARG(2); u32 len = (u32)ARG(3);
  if (idx == 1) { if (ARG(4)) wr32(c, ARG(4), 12); if (len < 12) { W->last_error = 122; RET(0); return; } wr32(c, b, 0); wr32(c, b + 4, 0); wr32(c, b + 8, 1); RET(1); return; }
  if (idx == 2) { static const char n[] = "WinSta0"; if (ARG(4)) wr32(c, ARG(4), 16); if (len < 16) { W->last_error = 122; RET(0); return; } for (int i = 0; i <= 7; i++) wr16(c, b + i * 2, (u8)n[i]); RET(1); return; }
  W->last_error = 87; RET(0); }
static void s_AllocateAndInitializeSid(Cpu *c) { u64 sid = w32_heap_alloc(W, 68, 1); if (!sid) { RET(0); return; } wr8(c, sid, 1); wr8(c, sid + 1, (u8)ARG(1)); u8 *p = mem_ptr(c->mem, sid + 2, 6, 1); const u8 *ia = mem_ptr(c->mem, ARG(0), 6, 0); if (p && ia) e_memcpy(p, ia, 6); for (u32 i = 0; i < (ARG(1) & 0xff) && i < 8; i++) wr32(c, sid + 8 + i * 4, (u32)ARG(2 + i)); if (ARG(10)) wrp(c, ARG(10), sid); RET(1); }
static void s_FreeSid(Cpu *c) { w32_heap_free(W, ARG(0)); RET(0); }
static void s_SetEntriesInAclA(Cpu *c) { if (ARG(3)) wrp(c, ARG(3), w32_heap_alloc(W, 64, 1)); RET(0); }
static void s_SetEntriesInAclW(Cpu *c) { s_SetEntriesInAclA(c); }
static void s_GetAdaptersAddresses(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), 0); RET(232); }   /* ERROR_NO_DATA: no interfaces */
static void s_GetUnicastIpAddressTable(Cpu *c) { RET(50); }
static void s_MapVirtualKeyW(Cpu *c) { RET(0); }
static void s_retm1(Cpu *c) { RET(~0ull); }
static void s_initterm(Cpu *c) { cont_push(c, CONT_INITTERM); Win32 *w = W; w->cont[w->ncont - 1].data[0] = ARG(0); w->cont[w->ncont - 1].data[1] = ARG(1); w->cont[w->ncont - 1].data[2] = 0; c->r[RAX] = 0; w32_cont_resume(c); }
static void s_initterm_e(Cpu *c) { cont_push(c, CONT_INITTERM); Win32 *w = W; w->cont[w->ncont - 1].data[0] = ARG(0); w->cont[w->ncont - 1].data[1] = ARG(1); w->cont[w->ncont - 1].data[2] = 1; c->r[RAX] = 0; w32_cont_resume(c); }
static void s_atexit(Cpu *c) { Win32 *w = W; if (w->n_at_exit < 64) w->at_exit[w->n_at_exit++] = ARG(0); RET(0); }
static void s_onexit(Cpu *c) { Win32 *w = W; if (w->n_at_exit < 64) w->at_exit[w->n_at_exit++] = ARG(0); RET(ARG(0)); }
static void s_register_onexit_function(Cpu *c) { Win32 *w = W; if (w->n_at_exit < 64) w->at_exit[w->n_at_exit++] = ARG(1); RET(0); }
static void s_exit(Cpu *c) { cont_push(c, CONT_ATEXIT); Win32 *w = W; w->cont[w->ncont - 1].data[0] = 1; w->cont[w->ncont - 1].data[1] = ARG(0); w32_cont_resume(c); }
static void s_cexit(Cpu *c) { cont_push(c, CONT_ATEXIT); W->cont[W->ncont - 1].data[0] = 0; w32_cont_resume(c); }
static void s__exit(Cpu *c) { do_exit(c, (int)ARG(0)); }
static void s_abort(Cpu *c) { con_out(W, 2, "abort() called\n", 15); do_exit(c, 3); }
static void s_p___argc(Cpu *c) { RET(W->ubuf + 0x200); }
static void s_p___argv(Cpu *c) { RET(W->ubuf + 0x208); }
static void s_p___wargv(Cpu *c) { RET(W->ubuf + 0x218); }
static void s_p__acmdln(Cpu *c) { RET(W->ubuf + 0x210); }
static void s_p__wcmdln(Cpu *c) { RET(W->ubuf + 0x220); }
static void s_p__fmode(Cpu *c) { RET(W->ubuf + 0x240); }
static void s_p__commode(Cpu *c) { RET(W->ubuf + 0x248); }
static void s_p__pgmptr(Cpu *c) { RET(W->ubuf + 0x250); }
static void s_errno(Cpu *c) { RET(W->ubuf + 0x258); }
static void s_set_errno(Cpu *c) { wr32(c, W->ubuf + 0x258, (u32)ARG(0)); RET(0); }
static void s_get_errno(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), rd32(c, W->ubuf + 0x258)); RET(0); }
/* mbstowcs / mbsrtowcs: the guest locale is "C" (single-byte), so every byte becomes one wchar */
static u64 mbs_to_wcs(Cpu *c, u64 dst, u64 src, u64 n) {
  u64 i = 0; if (!dst) { while (rd8(c, src + i)) i++; return i; }
  for (; i < n; i++) { u8 ch = rd8(c, src + i); wr16(c, dst + i * 2, ch); if (!ch) break; }
  return i;
}
static void s_mbstowcs(Cpu *c) { RET(mbs_to_wcs(c, ARG(0), ARG(1), ARG(2))); }
static void s_mbsrtowcs(Cpu *c) { u64 pp = ARG(1), src = pp ? rdp(c, pp) : 0; if (!src) { RET(0); return; }
  u64 n = mbs_to_wcs(c, ARG(0), src, ARG(2)); if (ARG(0) && pp) wrp(c, pp, n < ARG(2) ? 0 : src + n); RET(n); }
static void s_doserrno(Cpu *c) { RET(W->ubuf + 0x25c); }
static void s_acrt_iob_func(Cpu *c) { RET(W->ubuf + (u64)(u32)ARG(0) * IOB_STRIDE); }
static void s_iob_func(Cpu *c) { RET(W->ubuf); }
static void s_wgetmainargs(Cpu *c) { wr32(c, ARG(0), rd32(c, W->ubuf + 0x200)); wrp(c, ARG(1), rd64(c, W->ubuf + 0x218)); wrp(c, ARG(2), rd64(c, W->ubuf + 0x230)); RET(0); }
static void s_set_app_type(Cpu *c) { }
static void s_setusermatherr(Cpu *c) { }
static void s_controlfp(Cpu *c) { RET(0x9001f); }
static void s_controlfp_s(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), 0x9001f); RET(0); }
static void s_fpreset(Cpu *c) { }
static void s_set_fmode(Cpu *c) { RET(0); }
static void s_signal(Cpu *c) { RET(0); }
static void s_raise(Cpu *c) { cpu_fault(c, "raise(%d)", (int)ARG(0)); }

static int stream_write(Cpu *c, u64 file, const char *s, size_t n) { int fd = file_fd(c, file); if (!fd) { set_errno(c, 9); return -1; } return out_write(c, fd, (const u8 *)s, n); }
static void s_stdio_common_vfprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(4) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(2), 0, &src); if (c->status) return; int wr = stream_write(c, ARG(1), g_fmt_out, n < (int)sizeof g_fmt_out ? n : sizeof g_fmt_out - 1); if (!wr) return; RET(wr < 0 ? ~0ull : (u64)n); }
static void s_stdio_common_vfwprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(4) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(2), 1, &src); if (c->status) return; int wr = stream_write(c, ARG(1), g_fmt_out, n < (int)sizeof g_fmt_out ? n : sizeof g_fmt_out - 1); if (!wr) return; RET(wr < 0 ? ~0ull : (u64)n); }
static void vsprintf_common(Cpu *c, u64 opts, u64 buf, u64 len, u64 fmt, u64 va, int wide) {
  ArgSrc src = { c, 1, 0, va }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, fmt, wide, &src); if (c->status) return;
  int standard = (opts & 2) != 0, trunc = (opts & 1) != 0;
  if (buf && len) {
    u64 cp = (u64)n < len ? (u64)n : len - 1;
    if (!wide) { u8 *p = mem_ptr(c->mem, buf, cp + 1, 1); if (p) { e_memcpy(p, g_fmt_out, cp); p[cp] = 0; } }
    else { for (u64 i = 0; i < cp; i++) wr16(c, buf + i * 2, (u8)g_fmt_out[i]); wr16(c, buf + cp * 2, 0); }
    if ((u64)n >= len && !standard && !trunc) { RET((u64)-1); return; }
    if ((u64)n >= len && trunc && !standard) { RET((u64)-1); return; }
  }
  RET(n);
}
static void s_stdio_common_vsprintf(Cpu *c) { vsprintf_common(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(5), 0); }
static void s_stdio_common_vsprintf_s(Cpu *c) { vsprintf_common(c, ARG(0) | 2, ARG(1), ARG(2), ARG(3), ARG(5), 0); }
static void s_stdio_common_vsnprintf_s(Cpu *c) { vsprintf_common(c, ARG(0) | 2, ARG(1), ARG(2) < ARG(3) ? ARG(2) : ARG(3) + 1, ARG(4), ARG(6), 0); }
static void s_stdio_common_vswprintf(Cpu *c) { vsprintf_common(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(5), 1); }
static void s_stdio_common_vswprintf_s(Cpu *c) { vsprintf_common(c, ARG(0) | 2, ARG(1), ARG(2), ARG(3), ARG(5), 1); }
static void s_printf(Cpu *c) { ArgSrc src = { c, 0, 1, 0 }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(0), 0, &src); if (c->status) return; int wr = out_write(c, 1, (const u8 *)g_fmt_out, n < (int)sizeof g_fmt_out ? n : sizeof g_fmt_out - 1); if (!wr) return; RET(wr < 0 ? ~0ull : (u64)n); }
static void s_wprintf(Cpu *c) { ArgSrc src = { c, 0, 1, 0 }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(0), 1, &src); if (c->status) return; int wr = out_write(c, 1, (const u8 *)g_fmt_out, n < (int)sizeof g_fmt_out ? n : sizeof g_fmt_out - 1); if (!wr) return; RET(wr < 0 ? ~0ull : (u64)n); }
static void s_vprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(1) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(0), 0, &src); if (c->status) return; int wr = out_write(c, 1, (const u8 *)g_fmt_out, n < (int)sizeof g_fmt_out ? n : sizeof g_fmt_out - 1); if (!wr) return; RET(wr < 0 ? ~0ull : (u64)n); }
static void s_fprintf(Cpu *c) { ArgSrc src = { c, 0, 2, 0 }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(1), 0, &src); if (c->status) return; int wr = stream_write(c, ARG(0), g_fmt_out, n < (int)sizeof g_fmt_out ? n : sizeof g_fmt_out - 1); if (!wr) return; RET(wr < 0 ? ~0ull : (u64)n); }
static void s_vfprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(2) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(1), 0, &src); if (c->status) return; int wr = stream_write(c, ARG(0), g_fmt_out, n < (int)sizeof g_fmt_out ? n : sizeof g_fmt_out - 1); if (!wr) return; RET(wr < 0 ? ~0ull : (u64)n); }
static void s_sprintf(Cpu *c) { ArgSrc src = { c, 0, 2, 0 }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(1), 0, &src); if (c->status) return; u8 *p = mem_ptr(c->mem, ARG(0), n + 1, 1); if (p) e_memcpy(p, g_fmt_out, n + 1); RET(n); }
static void s_snprintf(Cpu *c) { ArgSrc src = { c, 0, 3, 0 }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(2), 0, &src); if (c->status) return; u64 len = ARG(1); if (len) { u64 cp = (u64)n < len ? (u64)n : len - 1; u8 *p = mem_ptr(c->mem, ARG(0), cp + 1, 1); if (p) { e_memcpy(p, g_fmt_out, cp); p[cp] = 0; } } RET(n); }
static void s_vsnprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(3) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(2), 0, &src); if (c->status) return; u64 len = ARG(1); if (len) { u64 cp = (u64)n < len ? (u64)n : len - 1; u8 *p = mem_ptr(c->mem, ARG(0), cp + 1, 1); if (p) { e_memcpy(p, g_fmt_out, cp); p[cp] = 0; } } RET(n); }
static void s_vsprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(2) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(1), 0, &src); if (c->status) return; u8 *p = mem_ptr(c->mem, ARG(0), n + 1, 1); if (p) e_memcpy(p, g_fmt_out, n + 1); RET(n); }
static void s_vscprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(1) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(0), 0, &src); if (c->status) return; RET(n); }
static void s_vscwprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(1) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(0), 1, &src); if (c->status) return; RET(n); }
static void s_puts(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (!s) { RET(~0ull); return; } static char pb[1 << 16]; size_t l = e_strlen(s); if (l + 1 > sizeof pb) l = sizeof pb - 1; e_memcpy(pb, s, l); pb[l] = '\n'; int wr = out_write(c, 1, (const u8 *)pb, l + 1); if (!wr) return; RET(wr < 0 ? ~0ull : 0); }
static void s_putws(Cpu *c) { char b[4096]; int n = wstr_to_utf8(c, ARG(0), b, sizeof b - 1, -1); b[n] = '\n'; int wr = out_write(c, 1, (const u8 *)b, (u64)n + 1); if (!wr) return; RET(wr < 0 ? ~0ull : 0); }
static void s_fputs(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (!s) { RET(~0ull); return; } int wr = stream_write(c, ARG(1), s, e_strlen(s)); if (!wr) return; RET(wr < 0 ? ~0ull : 0); }
static void s_putchar(Cpu *c) { char ch = (char)ARG(0); int wr = out_write(c, 1, (const u8 *)&ch, 1); if (!wr) return; RET(wr < 0 ? ~0ull : (ARG(0) & 0xff)); }
static void s_fputc(Cpu *c) { char ch = (char)ARG(0); int wr = stream_write(c, ARG(1), &ch, 1); if (!wr) return; RET(wr < 0 ? ~0ull : (ARG(0) & 0xff)); }
static void s_fflush(Cpu *c) { RET(0); }
static void s_setvbuf(Cpu *c) { RET(0); }
static void s_getc(Cpu *c) { RET(~0ull); }
static void s_time64(Cpu *c) { u64 t = unix_time(); if (ARG(0)) wr64(c, ARG(0), t); RET(t); }
static void s_clock(Cpu *c) { RET((u64)(host_now_ms() - W->start_ms)); }
static void s_localtime64(Cpu *c) { u64 t = rd64(c, ARG(0)); u64 a = W->ubuf + 0x300; u64 days = t / 86400, rem = t % 86400; s64 z = (s64)days + 719468; s64 era = z / 146097; s64 doe = z - era * 146097; s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; s64 y = yoe + era * 400; s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100); s64 mp = (5 * doy + 2) / 153; s64 d = doy - (153 * mp + 2) / 5 + 1; s64 m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++;
  wr32(c, a, (u32)(rem % 60)); wr32(c, a + 4, (u32)(rem / 60 % 60)); wr32(c, a + 8, (u32)(rem / 3600)); wr32(c, a + 12, (u32)d); wr32(c, a + 16, (u32)(m - 1)); wr32(c, a + 20, (u32)(y - 1900)); wr32(c, a + 24, (u32)((days + 4) % 7)); { static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334}; int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)); wr32(c, a + 28, (u32)(cum[m - 1] + d - 1 + (m > 2 && leap ? 1 : 0))); } wr32(c, a + 32, 0); RET(a); }
static void s_gmtime64(Cpu *c) { s_localtime64(c); }
static void s_wcschr(Cpu *c) { u64 a = ARG(0); u16 ch = (u16)ARG(1); for (;; a += 2) { u16 x = rd16(c, a); if (x == ch) { RET(a); return; } if (!x || c->status) { RET(0); return; } } }
static void s_wcsrchr(Cpu *c) { u64 a = ARG(0), last = 0; u16 ch = (u16)ARG(1); for (;; a += 2) { u16 x = rd16(c, a); if (x == ch) last = a; if (!x || c->status) break; } RET(last); }
static void s_wcstombs(Cpu *c) { u64 d = ARG(0), s = ARG(1), n = ARG(2); u64 i = 0; for (;; i++) { u16 x = rd16(c, s + 2 * i); if (d && i >= n) break; u8 b = x < 256 ? (u8)x : '?'; if (d) wr8(c, d + i, b); if (!x) break; } RET(i); }
static void s_localtime64_s(Cpu *c) { u64 dst = ARG(0); call_shim(c, s_localtime64, ARG(1), 0, 0, 0); u8 *s = mem_ptr(c->mem, c->r[RAX], 36, 0), *d = mem_ptr(c->mem, dst, 36, 1); if (s && d) e_memcpy(d, s, 36); RET(0); }
static void s_mktime64(Cpu *c) { RET(unix_time()); }
static int fmt_tm(Cpu *c, char *out, size_t cap, const char *f, u64 tmp) {
  static const char *wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
  static const char *mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
  int sec = (int)rd32(c, tmp), min = (int)rd32(c, tmp + 4), hour = (int)rd32(c, tmp + 8), mday = (int)rd32(c, tmp + 12), mon = (int)rd32(c, tmp + 16), year = (int)rd32(c, tmp + 20), wday = (int)rd32(c, tmp + 24), yday = (int)rd32(c, tmp + 28);
  if (wday < 0 || wday > 6) wday = 0; if (mon < 0 || mon > 11) mon = 0;
  size_t o = 0; char b[64];
  #define PUT(str) do { const char *q_ = (str); while (*q_ && o + 1 < cap) out[o++] = *q_++; } while (0)
  for (; *f; f++) {
    if (*f != '%') { if (o + 1 < cap) out[o++] = *f; continue; }
    f++; if (*f == 'E' || *f == 'O') f++; if (!*f) break;
    switch (*f) {
      case 'a': e_snprintf(b, sizeof b, "%.3s", wd[wday]); PUT(b); break;   case 'A': PUT(wd[wday]); break;
      case 'b': case 'h': e_snprintf(b, sizeof b, "%.3s", mn[mon]); PUT(b); break; case 'B': PUT(mn[mon]); break;
      case 'c': e_snprintf(b, sizeof b, "%.3s %.3s %2d %02d:%02d:%02d %d", wd[wday], mn[mon], mday, hour, min, sec, year + 1900); PUT(b); break;
      case 'C': e_snprintf(b, sizeof b, "%02d", (year + 1900) / 100); PUT(b); break;
      case 'd': e_snprintf(b, sizeof b, "%02d", mday); PUT(b); break;       case 'e': e_snprintf(b, sizeof b, "%2d", mday); PUT(b); break;
      case 'D': e_snprintf(b, sizeof b, "%02d/%02d/%02d", mon + 1, mday, year % 100); PUT(b); break;
      case 'F': e_snprintf(b, sizeof b, "%d-%02d-%02d", year + 1900, mon + 1, mday); PUT(b); break;
      case 'H': e_snprintf(b, sizeof b, "%02d", hour); PUT(b); break;       case 'I': e_snprintf(b, sizeof b, "%02d", hour % 12 ? hour % 12 : 12); PUT(b); break;
      case 'j': e_snprintf(b, sizeof b, "%03d", yday + 1); PUT(b); break;   case 'k': e_snprintf(b, sizeof b, "%2d", hour); PUT(b); break;
      case 'l': e_snprintf(b, sizeof b, "%2d", hour % 12 ? hour % 12 : 12); PUT(b); break;
      case 'm': e_snprintf(b, sizeof b, "%02d", mon + 1); PUT(b); break;    case 'M': e_snprintf(b, sizeof b, "%02d", min); PUT(b); break;
      case 'n': PUT("\n"); break; case 't': PUT("\t"); break; case '%': PUT("%"); break;
      case 'p': PUT(hour < 12 ? "AM" : "PM"); break; case 'P': PUT(hour < 12 ? "am" : "pm"); break;
      case 'r': e_snprintf(b, sizeof b, "%02d:%02d:%02d %s", hour % 12 ? hour % 12 : 12, min, sec, hour < 12 ? "AM" : "PM"); PUT(b); break;
      case 'R': e_snprintf(b, sizeof b, "%02d:%02d", hour, min); PUT(b); break;
      case 'S': e_snprintf(b, sizeof b, "%02d", sec); PUT(b); break;
      case 's': { s64 y = year + 1900, m2 = mon + 1; if (m2 <= 2) { y--; m2 += 12; } s64 era = (y >= 0 ? y : y - 399) / 400; s64 yoe = y - era * 400; s64 doy = (153 * (m2 - 3) + 2) / 5 + mday - 1; s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; s64 days = era * 146097 + doe - 719468; e_snprintf(b, sizeof b, "%lld", (long long)(days * 86400 + hour * 3600 + min * 60 + sec)); PUT(b); break; }
      case 'T': case 'X': e_snprintf(b, sizeof b, "%02d:%02d:%02d", hour, min, sec); PUT(b); break;
      case 'u': e_snprintf(b, sizeof b, "%d", wday ? wday : 7); PUT(b); break; case 'w': e_snprintf(b, sizeof b, "%d", wday); PUT(b); break;
      case 'x': e_snprintf(b, sizeof b, "%02d/%02d/%02d", mon + 1, mday, year % 100); PUT(b); break;
      case 'y': e_snprintf(b, sizeof b, "%02d", year % 100); PUT(b); break; case 'Y': case 'G': e_snprintf(b, sizeof b, "%d", year + 1900); PUT(b); break;
      case 'Z': PUT("UTC"); break; case 'z': PUT("+0000"); break;
      case 'U': case 'W': case 'V': e_snprintf(b, sizeof b, "%02d", (yday + 7 - (*f == 'W' ? (wday + 6) % 7 : wday)) / 7); PUT(b); break;
      default: if (o + 2 < cap) { out[o++] = '%'; out[o++] = *f; } break;
    }
  }
  #undef PUT
  out[o] = 0; return (int)o;
}
static void s_strftime(Cpu *c) { const char *f = rd_str(c, ARG(2), 1024); char out[1024]; int n = fmt_tm(c, out, sizeof out, f ? f : "", ARG(3)); if ((u64)n + 1 > ARG(1)) { RET(0); return; } u8 *p = mem_ptr(c->mem, ARG(0), n + 1, 1); if (p) e_memcpy(p, out, n + 1); RET(n); }
static void s_asctime(Cpu *c) { char out[64]; fmt_tm(c, out, sizeof out, "%c\n", ARG(0)); u64 a = W->ubuf + 0x3c0; u8 *p = mem_ptr(c->mem, a, 32, 1); if (p) e_memcpy(p, out, e_strlen(out) + 1); RET(a); }
static void s_ctime64(Cpu *c) { call_shim(c, s_localtime64, ARG(0), 0, 0, 0); call_shim(c, s_asctime, c->r[RAX], 0, 0, 0); }
static void s_tzset(Cpu *c) { }
static void s_timezone(Cpu *c) { RET(W->ubuf + 0x260); }
static void s_daylight(Cpu *c) { RET(W->ubuf + 0x264); }
static void s_tzname(Cpu *c) { RET(W->ubuf + 0x268); }
static void s_rand(Cpu *c) { Win32 *w = W; w->rand_state = w->rand_state * 214013u + 2531011u; RET((w->rand_state >> 16) & 0x7fff); }
static void s_srand(Cpu *c) { W->rand_state = (u32)ARG(0); }
static void s_strlen(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 30); if (!s) { cpu_fault(c, "strlen on bad pointer"); return; } RET(e_strlen(s)); }
static void s_wcslen(Cpu *c) { int n = 0; while (rd16(c, ARG(0) + (u64)n * 2) && !c->status) n++; RET(n); }
static void s_strnlen(Cpu *c) { u64 n = 0, m = ARG(1); while (n < m && rd8(c, ARG(0) + n)) n++; RET(n); }
static void s_strcmp(Cpu *c) { const char *a = rd_str(c, ARG(0), 1 << 30), *b = rd_str(c, ARG(1), 1 << 30); if (!a || !b) { cpu_fault(c, "strcmp bad pointer"); return; } RET((u64)(s64)e_strcmp(a, b)); }
static void s_stricmp(Cpu *c) { const char *a = rd_str(c, ARG(0), 1 << 30), *b = rd_str(c, ARG(1), 1 << 30); if (!a || !b) { cpu_fault(c, "stricmp bad pointer"); return; } RET((u64)(s64)e_stricmp(a, b)); }
static void s_strncmp(Cpu *c) { u64 n = ARG(2); u64 a = ARG(0), b = ARG(1); for (u64 i = 0; i < n; i++) { u8 x = rd8(c, a + i), y = rd8(c, b + i); if (x != y) { RET((u64)(s64)(x - y)); return; } if (!x) break; } RET(0); }
static void s_strnicmp(Cpu *c) { u64 n = ARG(2); u64 a = ARG(0), b = ARG(1); for (u64 i = 0; i < n; i++) { u8 x = rd8(c, a + i), y = rd8(c, b + i); if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) { RET((u64)(s64)(x - y)); return; } if (!x) break; } RET(0); }
static void s_wcsicmp(Cpu *c) { u64 a = ARG(0), b = ARG(1); for (u64 i = 0;; i++) { u16 x = rd16(c, a + i * 2), y = rd16(c, b + i * 2); if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) { RET((u64)(s64)(x < y ? -1 : 1)); return; } if (!x) break; } RET(0); }
static void s_wcsncmp(Cpu *c) { u64 a = ARG(0), b = ARG(1), n = ARG(2); for (u64 i = 0; i < n; i++) { u16 x = rd16(c, a + i * 2), y = rd16(c, b + i * 2); if (x != y) { RET((u64)(s64)(x < y ? -1 : 1)); return; } if (!x) break; } RET(0); }
/* wcsncpy_s(dst, dstsz, src, count): copies up to count chars (count == _TRUNCATE -1 → fit), always terminates; ERANGE 34 when it does not fit */
static void s_wcsncpy_s(Cpu *c) { u64 d = ARG(0), dn = ARG(1), s = ARG(2), n = ARG(3); if (!d || !dn) { RET(22); return; } if (!s) { wr16(c, d, 0); RET(22); return; }
  u64 l = 0; while (rd16(c, s + 2 * l) && (n == ~0ull || l < n)) l++;
  if (l + 1 > dn) { if (n == ~0ull) { l = dn - 1; } else { wr16(c, d, 0); RET(34); return; } }
  for (u64 i = 0; i < l; i++) wr16(c, d + 2 * i, rd16(c, s + 2 * i)); wr16(c, d + 2 * l, 0); RET(n == ~0ull && l + 1 > dn ? 80 : 0); }
static void s_wcscpy_s(Cpu *c) { u64 d = ARG(0), dn = ARG(1), s = ARG(2); if (!d || !dn) { RET(22); return; } if (!s) { wr16(c, d, 0); RET(22); return; } u64 l = 0; while (rd16(c, s + 2 * l)) l++; if (l + 1 > dn) { wr16(c, d, 0); RET(34); return; } for (u64 i = 0; i <= l; i++) wr16(c, d + 2 * i, rd16(c, s + 2 * i)); RET(0); }
static void s_wcscat_s(Cpu *c) { u64 d = ARG(0), dn = ARG(1), s = ARG(2); if (!d || !dn || !s) { RET(22); return; } u64 dl = 0; while (dl < dn && rd16(c, d + 2 * dl)) dl++; if (dl == dn) { RET(22); return; } u64 l = 0; while (rd16(c, s + 2 * l)) l++; if (dl + l + 1 > dn) { wr16(c, d, 0); RET(34); return; } for (u64 i = 0; i <= l; i++) wr16(c, d + 2 * (dl + i), rd16(c, s + 2 * i)); RET(0); }
static void s_wcscmp(Cpu *c) { u64 a = ARG(0), b = ARG(1); for (u64 i = 0;; i++) { u16 x = rd16(c, a + i * 2), y = rd16(c, b + i * 2); if (x != y) { RET((u64)(s64)(x < y ? -1 : 1)); return; } if (!x) break; } RET(0); }
static void s_strcpy(Cpu *c) { const char *s = rd_str(c, ARG(1), 1 << 30); if (!s) { cpu_fault(c, "strcpy bad pointer"); return; } size_t l = e_strlen(s) + 1; u8 *d = mem_ptr(c->mem, ARG(0), l, 1); if (!d) { cpu_fault(c, "strcpy bad dest"); return; } e_memmove(d, s, l); RET(ARG(0)); }
static void s_strncpy(Cpu *c) { u64 n = ARG(2), d = ARG(0), s = ARG(1); u64 i = 0; for (; i < n; i++) { u8 ch = rd8(c, s + i); wr8(c, d + i, ch); if (!ch) { i++; break; } } for (; i < n; i++) wr8(c, d + i, 0); RET(d); }
static void s_strcat(Cpu *c) { const char *d0 = rd_str(c, ARG(0), 1 << 30), *s = rd_str(c, ARG(1), 1 << 30); if (!d0 || !s) { cpu_fault(c, "strcat bad pointer"); return; } size_t dl = e_strlen(d0), sl = e_strlen(s) + 1; u8 *d = mem_ptr(c->mem, ARG(0) + dl, sl, 1); if (d) e_memmove(d, s, sl); RET(ARG(0)); }
static void s_strchr(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 30); if (!s) { RET(0); return; } char *p = e_strchr(s, (int)ARG(1)); RET(p ? ARG(0) + (u64)(p - s) : 0); }
static void s_strrchr(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 30); if (!s) { RET(0); return; } const char *last = 0; char ch = (char)ARG(1); for (const char *p = s;; p++) { if (*p == ch) last = p; if (!*p) break; } RET(last ? ARG(0) + (u64)(last - s) : 0); }
static void s_strstr(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 30), *n = rd_str(c, ARG(1), 1 << 30); if (!s || !n) { RET(0); return; } size_t nl = e_strlen(n); for (const char *p = s; *p; p++) if (!e_strncmp(p, n, nl)) { RET(ARG(0) + (u64)(p - s)); return; } RET(nl ? 0 : ARG(0)); }
/* sscanf subset: %d %i %u %x %o %p %s %c %[ %f/%g with h/l/ll/I64/z sizes, width, '*' suppression, literals and whitespace */
static int scan_isspace(int ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; }
static u64 scan_common(Cpu *c, const char *s, const char *f, ArgSrc *as) {
  int n = 0; const char *p = s;
  while (*f) {
    if (scan_isspace(*f)) { while (scan_isspace(*p)) p++; f++; continue; }
    if (*f != '%') { if (*p != *f) break; p++; f++; continue; }
    f++; int suppress = 0, width = 0, size = 0;   /* size: 0 int, 1 short, 2 long, 3 long long, 4 char */
    if (*f == '*') { suppress = 1; f++; }
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    if (*f == 'h') { size = 1; f++; if (*f == 'h') { size = 4; f++; } }
    else if (*f == 'l') { size = 2; f++; if (*f == 'l') { size = 3; f++; } }
    else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 3; f += 3; }
    else if (*f == 'z' || *f == 'j' || *f == 't') { size = 3; f++; }
    char conv = *f++; if (!conv) break;
    if (conv == '%') { if (*p != '%') break; p++; continue; }
    if (conv == 'n') { if (!suppress) wr32(c, arg_next(as), (u32)(p - s)); continue; }
    if (conv == 'c') { int w = width ? width : 1; for (int k = 0; k < w; k++) if (!p[k]) goto done; if (!suppress) { u64 d = arg_next(as); for (int k = 0; k < w; k++) wr8(c, d + (u64)k, (u8)p[k]); n++; } p += w; continue; }
    if (conv == 's' || conv == '[') {
      char set[256]; int neg = 0; e_memset(set, 0, sizeof set);
      if (conv == '[') { if (*f == '^') { neg = 1; f++; } if (*f == ']') { set[(u8)']'] = 1; f++; } while (*f && *f != ']') { if (f[1] == '-' && f[2] && f[2] != ']') { for (int ch = (u8)f[0]; ch <= (u8)f[2]; ch++) set[ch] = 1; f += 3; } else set[(u8)*f++] = 1; } if (*f == ']') f++; }
      else { while (scan_isspace(*p)) p++; }
      const char *st = p; int w = width ? width : 0x7fffffff;
      while (*p && (p - st) < w) { int in = conv == 's' ? !scan_isspace(*p) : (set[(u8)*p] != neg); if (!in) break; p++; }
      if (p == st) break;
      if (!suppress) { u64 d = arg_next(as); for (const char *q = st; q < p; q++) wr8(c, d + (u64)(q - st), (u8)*q); wr8(c, d + (u64)(p - st), 0); n++; }
      continue;
    }
    while (scan_isspace(*p)) p++;
    if (!*p) break;
    char tmp[128]; int tl = 0; int w = width ? width : 127; while (p[tl] && tl < w && tl < 127) { tmp[tl] = p[tl]; tl++; } tmp[tl] = 0;
    char *e = tmp;
    if (conv == 'f' || conv == 'g' || conv == 'e' || conv == 'a' || conv == 'F' || conv == 'G' || conv == 'E') {
      double v = e_strtod(tmp, &e); if (e == tmp) break; p += e - tmp;
      if (!suppress) { u64 d = arg_next(as); if (size >= 2) { u64 bits; e_memcpy(&bits, &v, 8); wr64(c, d, bits); } else { float fv = (float)v; u32 bits; e_memcpy(&bits, &fv, 4); wr32(c, d, bits); } n++; }
      continue;
    }
    int base = conv == 'd' || conv == 'u' ? 10 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : 0;
    long long v = e_strtoll(tmp, &e, base); if (e == tmp) break; p += e - tmp;
    if (!suppress) { u64 d = arg_next(as); if (conv == 'p') wrp(c, d, (u64)v); else if (size == 3) wr64(c, d, (u64)v); else if (size == 2 && 0) wr64(c, d, (u64)v); else if (size == 1) wr16(c, d, (u16)v); else if (size == 4) wr8(c, d, (u8)v); else wr32(c, d, (u32)v); n++; }
  }
done:
  return n == 0 && !*p ? ~0ull : (u64)(u32)n;
}
static void s_sscanf(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20), *f = rd_str(c, ARG(1), 65536); if (!s || !f) { RET(~0ull); return; } ArgSrc as = { c, 0, 2, 0 }; RET(scan_common(c, s, f, &as)); }
/* UCRT: __stdio_common_vsscanf(options, buffer, len, format, locale, va_list) — the va_list is a pointer to the packed args */
static void s_stdio_common_vsscanf(Cpu *c) { const char *s = rd_str(c, ARG(1), 1 << 20), *f = rd_str(c, ARG(3), 65536); if (!s || !f) { RET(~0ull); return; }
  u64 len = ARG(2); static char lim[1 << 16]; if (len < sizeof lim - 1 && e_strlen(s) > len) { e_memcpy(lim, s, len); lim[len] = 0; s = lim; }
  ArgSrc as = { c, 1, 0, ARG(5) }; RET(scan_common(c, s, f, &as)); }
/* __stdio_common_vfscanf(options, FILE*, format, locale, va_list): read one line from the stream and scan it (enough for the
   "fscanf(stdin, "%d")"-style use real programs make of it) */
static void s_stdio_common_vfscanf(Cpu *c) { u64 ln = ARG(1) ? w32_heap_alloc(W, 4096, 1) : 0; if (!ln) { RET(~0ull); return; }
  const char *f = rd_str(c, ARG(2), 65536); if (!f) { RET(~0ull); return; }
  call_shim(c, s_fgets, ln, 4095, ARG(1), 0); u64 got = c->r[RAX];
  if (c->status) return; if (!got) { RET(~0ull); return; }
  const char *s = rd_str(c, ln, 4096); ArgSrc as = { c, 1, 0, ARG(4) }; RET(s ? scan_common(c, s, f, &as) : ~0ull); w32_heap_free(W, ln); }
static void s_strdup(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 30); RET(s ? w32_guest_strdup(W, s) : 0); }
static void s_strtol(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (!s) { RET(0); return; } char *e; long long v = e_strtoll(s, &e, (int)ARG(2)); if (ARG(1)) wrp(c, ARG(1), ARG(0) + (u64)(e - s)); if (v > 0x7fffffff) v = 0x7fffffff; if (v < -0x7fffffffll - 1) v = -0x7fffffffll - 1; RET((u64)v); }
static void s_strtoul(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (!s) { RET(0); return; } char *e; long long v = e_strtoll(s, &e, (int)ARG(2)); if (ARG(1)) wrp(c, ARG(1), ARG(0) + (u64)(e - s)); RET((u64)(u32)v); }
static void s_strtoll(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (!s) { RET(0); return; } char *e; long long v = e_strtoll(s, &e, (int)ARG(2)); if (ARG(1)) wrp(c, ARG(1), ARG(0) + (u64)(e - s)); RET((u64)v); }
static void s_atoi(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); RET(s ? (u64)(u32)e_strtoll(s, 0, 10) : 0); }
static void s_atol(Cpu *c) { s_atoi(c); }
static void s_atoll(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); RET(s ? (u64)e_strtoll(s, 0, 10) : 0); }
static void s_strtod(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (!s) { RET(0); return; } char *e; double v = e_strtod(s, &e); if (ARG(1)) wrp(c, ARG(1), ARG(0) + (u64)(e - s)); c->x[0].f64[0] = v; }
static void s_atof(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); c->x[0].f64[0] = s ? e_strtod(s, 0) : 0; }
static void s_toupper(Cpu *c) { int ch = (int)ARG(0); RET(ch >= 'a' && ch <= 'z' ? ch - 32 : ch); }
static void s_tolower(Cpu *c) { int ch = (int)ARG(0); RET(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); }
static void s_isdigit(Cpu *c) { int ch = (int)ARG(0); RET(ch >= '0' && ch <= '9'); }
static void s_isalpha(Cpu *c) { int ch = (int)ARG(0) | 32; RET(ch >= 'a' && ch <= 'z'); }
static void s_isalnum(Cpu *c) { int ch = (int)ARG(0); RET((ch >= '0' && ch <= '9') || ((ch | 32) >= 'a' && (ch | 32) <= 'z')); }
static void s_isspace(Cpu *c) { int ch = (int)ARG(0); RET(ch == ' ' || (ch >= 9 && ch <= 13)); }
static void s_isupper(Cpu *c) { int ch = (int)ARG(0); RET(ch >= 'A' && ch <= 'Z'); }
static void s_islower(Cpu *c) { int ch = (int)ARG(0); RET(ch >= 'a' && ch <= 'z'); }
static void s_isprint(Cpu *c) { int ch = (int)ARG(0); RET(ch >= 32 && ch < 127); }
static void s_isxdigit(Cpu *c) { int ch = (int)ARG(0); RET((ch >= '0' && ch <= '9') || ((ch | 32) >= 'a' && (ch | 32) <= 'f')); }
static void s_ispunct(Cpu *c) { int ch = (int)ARG(0); RET((ch >= 33 && ch <= 47) || (ch >= 58 && ch <= 64) || (ch >= 91 && ch <= 96) || (ch >= 123 && ch <= 126)); }
static void s_pctype_func(Cpu *c) { RET(W->ubuf + 0x400); }
static void s_memcpy(Cpu *c) { u64 n = ARG(2); if (n) { u8 *d = mem_ptr(c->mem, ARG(0), n, 1); const u8 *s = mem_ptr(c->mem, ARG(1), n, 0); if (!d || !s) { cpu_fault(c, "memcpy fault"); return; } e_memmove(d, s, (size_t)n); } RET(ARG(0)); }
static void s_memset(Cpu *c) { u64 n = ARG(2); if (n) { u8 *d = mem_ptr(c->mem, ARG(0), n, 1); if (!d) { cpu_fault(c, "memset fault"); return; } e_memset(d, (int)ARG(1), (size_t)n); } RET(ARG(0)); }
static void s_memcmp(Cpu *c) { u64 n = ARG(2); if (!n) { RET(0); return; } const u8 *a = mem_ptr(c->mem, ARG(0), n, 0), *b = mem_ptr(c->mem, ARG(1), n, 0); if (!a || !b) { cpu_fault(c, "memcmp fault"); return; } RET((u64)(s64)e_memcmp(a, b, (size_t)n)); }
static void s_memchr(Cpu *c) { u64 n = ARG(2); const u8 *a = mem_ptr(c->mem, ARG(0), n, 0); if (!a) { RET(0); return; } for (u64 i = 0; i < n; i++) if (a[i] == (u8)ARG(1)) { RET(ARG(0) + i); return; } RET(0); }
static void s_qsort(Cpu *c) { u64 n = ARG(1), sz = ARG(2); if (n < 2 || !sz) return; Win32 *w = W; u64 tmp = w32_heap_alloc(w, n * sz, 0); if (!tmp) return;
  int i = cont_push(c, CONT_QSORT); if (i < 0) return; w->cont[i].data[0] = ARG(0); w->cont[i].data[1] = n; w->cont[i].data[2] = sz; w->cont[i].data[3] = ARG(3);
  w->cont[i].data[4] = 1; w->cont[i].data[5] = 0; w->cont[i].data[6] = (0ull << 32) | (1 < n ? 1 : n); w->cont[i].data[7] = tmp; w->cont[i].idx = 0;
  qsort_step(c, 0, 0); }
static void s_bsearch(Cpu *c) { u64 n = ARG(2); if (!n) { RET(0); return; } cont_push(c, CONT_BSEARCH); Win32 *w = W; int i = w->ncont - 1; w->cont[i].data[0] = ARG(0); w->cont[i].data[1] = ARG(1); w->cont[i].data[2] = n; w->cont[i].data[3] = ARG(3); w->cont[i].data[4] = ARG(4); w->cont[i].data[5] = 0;
  guest_call(c, ARG(4), ARG(0), ARG(1) + (n / 2) * ARG(3), 0, 0); }
static void s_setjmp(Cpu *c) { u64 a = ARG(0);
  if (c->mode32) { u64 reg = rd32(c, c->fs_base); wr32(c, a, (u32)c->r[RBP]); wr32(c, a + 4, (u32)c->r[RBX]); wr32(c, a + 8, (u32)c->r[RDI]); wr32(c, a + 12, (u32)c->r[RSI]); wr32(c, a + 16, (u32)c->r[RSP]); wr32(c, a + 20, (u32)c->rip); wr32(c, a + 24, (u32)reg); wr32(c, a + 28, reg != 0xffffffffu && reg ? rd32(c, reg + 12) : 0xffffffffu); wr32(c, a + 32, 0x56433230); wr32(c, a + 36, 0); RET(0); return; } wr64(c, a, 0); wr64(c, a + 8, c->r[RBX]); wr64(c, a + 16, c->r[RSP]); wr64(c, a + 24, c->r[RBP]); wr64(c, a + 32, c->r[RSI]); wr64(c, a + 40, c->r[RDI]); wr64(c, a + 48, c->r[R12]); wr64(c, a + 56, c->r[R13]); wr64(c, a + 64, c->r[R14]); wr64(c, a + 72, c->r[R15]); wr64(c, a + 80, c->rip); for (int k = 6; k < 16; k++) { wr64(c, a + 96 + (k - 6) * 16, c->x[k].q[0]); wr64(c, a + 104 + (k - 6) * 16, c->x[k].q[1]); } RET(0); }
static void s_longjmp(Cpu *c) { u64 a = ARG(0); u64 v = ARG(1);
  if (c->mode32) { longjmp32(c, a, v); return; } c->r[RBX] = rd64(c, a + 8); c->r[RSP] = rd64(c, a + 16); c->r[RBP] = rd64(c, a + 24); c->r[RSI] = rd64(c, a + 32); c->r[RDI] = rd64(c, a + 40); c->r[R12] = rd64(c, a + 48); c->r[R13] = rd64(c, a + 56); c->r[R14] = rd64(c, a + 64); c->r[R15] = rd64(c, a + 72); c->rip = rd64(c, a + 80); for (int k = 6; k < 16; k++) { c->x[k].q[0] = rd64(c, a + 96 + (k - 6) * 16); c->x[k].q[1] = rd64(c, a + 104 + (k - 6) * 16); } RET(v ? v : 1); }
static void s_invalid_parameter_noinfo(Cpu *c) { cpu_fault(c, "CRT invalid parameter"); }
static void s_div(Cpu *c) { s32 a = (s32)ARG(0), b = (s32)ARG(1); if (!b) { cpu_fault(c, "div by zero"); return; } RET(((u64)(u32)(a % b) << 32) | (u32)(a / b)); }
static void s_abs(Cpu *c) { s32 v = (s32)ARG(0); RET((u32)(v < 0 ? -v : v)); }
static void s_labs(Cpu *c) { s_abs(c); }
static void s_llabs(Cpu *c) { s64 v = (s64)ARG(0); RET((u64)(v < 0 ? -v : v)); }
/* math: double in xmm0 (and xmm1) */
#define MATH1(name, op) static void s_##name(Cpu *c) { c->x[0].f64[0] = host_math(op, c->x[0].f64[0], 0); }
#define MATH2(name, op) static void s_##name(Cpu *c) { c->x[0].f64[0] = host_math(op, c->x[0].f64[0], c->x[1].f64[0]); }
MATH1(sin, 0) MATH1(cos, 1) MATH1(tan, 2) MATH1(exp, 3) MATH1(log, 4) MATH2(pow, 5) MATH2(atan2, 6) MATH1(asin, 7) MATH1(acos, 8) MATH1(atan, 9) MATH1(sinh, 10) MATH1(cosh, 11) MATH1(tanh, 12) MATH1(log10, 13) MATH1(log2, 14) MATH1(cbrt, 15)
static void s_sqrt(Cpu *c) { c->x[0].f64[0] = e_sqrt(c->x[0].f64[0]); }
static void s_floor(Cpu *c) { c->x[0].f64[0] = e_floor(c->x[0].f64[0]); }
static void s_ceil(Cpu *c) { c->x[0].f64[0] = e_ceil(c->x[0].f64[0]); }
static void s_fabs(Cpu *c) { c->x[0].f64[0] = e_fabs(c->x[0].f64[0]); }
static void s_fmod(Cpu *c) { c->x[0].f64[0] = e_fmod(c->x[0].f64[0], c->x[1].f64[0]); }
static void s_round(Cpu *c) { double v = c->x[0].f64[0]; c->x[0].f64[0] = v < 0 ? -e_floor(-v + 0.5) : e_floor(v + 0.5); }
static void s_trunc(Cpu *c) { c->x[0].f64[0] = __builtin_trunc(c->x[0].f64[0]); }
static void s_ldexp(Cpu *c) { double v = c->x[0].f64[0]; int e = (int)ARG(1); while (e > 0) { v *= 2; e--; } while (e < 0) { v /= 2; e++; } c->x[0].f64[0] = v; }
static void s_frexp(Cpu *c) { double v = c->x[0].f64[0]; int e = 0; if (v != 0) { double a = v < 0 ? -v : v; while (a >= 1) { a /= 2; e++; } while (a < 0.5) { a *= 2; e--; } v = v < 0 ? -a : a; } wr32(c, ARG(1), (u32)e); c->x[0].f64[0] = v; }
static void s_modf(Cpu *c) { double v = c->x[0].f64[0]; double ip = __builtin_trunc(v); u64 bits; e_memcpy(&bits, &ip, 8); wr64(c, ARG(1), bits); c->x[0].f64[0] = v - ip; }
static void s_hypot(Cpu *c) { double a = c->x[0].f64[0], b = c->x[1].f64[0]; c->x[0].f64[0] = e_sqrt(a * a + b * b); }
static void s_sinf(Cpu *c) { c->x[0].f32[0] = (float)host_math(0, c->x[0].f32[0], 0); }
static void s_cosf(Cpu *c) { c->x[0].f32[0] = (float)host_math(1, c->x[0].f32[0], 0); }
static void s_tanf(Cpu *c) { c->x[0].f32[0] = (float)host_math(2, c->x[0].f32[0], 0); }
static void s_expf(Cpu *c) { c->x[0].f32[0] = (float)host_math(3, c->x[0].f32[0], 0); }
static void s_logf(Cpu *c) { c->x[0].f32[0] = (float)host_math(4, c->x[0].f32[0], 0); }
static void s_powf(Cpu *c) { c->x[0].f32[0] = (float)host_math(5, c->x[0].f32[0], c->x[1].f32[0]); }
static void s_atan2f(Cpu *c) { c->x[0].f32[0] = (float)host_math(6, c->x[0].f32[0], c->x[1].f32[0]); }
static void s_sqrtf(Cpu *c) { c->x[0].f32[0] = __builtin_sqrtf(c->x[0].f32[0]); }
static void s_floorf(Cpu *c) { c->x[0].f32[0] = __builtin_floorf(c->x[0].f32[0]); }
static void s_ceilf(Cpu *c) { c->x[0].f32[0] = __builtin_ceilf(c->x[0].f32[0]); }
static void s_fabsf(Cpu *c) { c->x[0].f32[0] = __builtin_fabsf(c->x[0].f32[0]); }
static void s_fmodf(Cpu *c) { c->x[0].f32[0] = (float)e_fmod(c->x[0].f32[0], c->x[1].f32[0]); }
static void s_roundf(Cpu *c) { float v = c->x[0].f32[0]; c->x[0].f32[0] = v < 0 ? -__builtin_floorf(-v + 0.5f) : __builtin_floorf(v + 0.5f); }
static void s_fmin(Cpu *c) { double a = c->x[0].f64[0], b = c->x[1].f64[0]; c->x[0].f64[0] = a != a ? b : b != b ? a : a < b ? a : b; }
static void s_fmax(Cpu *c) { double a = c->x[0].f64[0], b = c->x[1].f64[0]; c->x[0].f64[0] = a != a ? b : b != b ? a : a > b ? a : b; }
static void s_copysign(Cpu *c) { double a = e_fabs(c->x[0].f64[0]); c->x[0].f64[0] = (c->x[1].q[0] >> 63) ? -a : a; }
static void s_exp2(Cpu *c) { c->x[0].f64[0] = host_math(5, 2.0, c->x[0].f64[0]); }
static void s_lround(Cpu *c) { double v = c->x[0].f64[0]; RET((u64)(s64)(v < 0 ? -e_floor(-v + 0.5) : e_floor(v + 0.5))); }
/* UCRT _dclass/_fdclass: FP_NAN 2, FP_INFINITE 1, FP_ZERO 0, FP_SUBNORMAL -2, FP_NORMAL -1 (callers test the short in ax) */
static void s_dclass(Cpu *c) { double v = c->x[0].f64[0]; union { double d; u64 u; } b; b.d = v; u64 ex = (b.u >> 52) & 0x7ff, mant = b.u & ((1ull << 52) - 1);
  RET(ex == 0x7ff ? (mant ? 2 : 1) : ex == 0 ? (mant ? (u64)(s64)-2 : 0) : (u64)(s64)-1); }
static void s_fdclass(Cpu *c) { c->x[0].f64[0] = (double)c->x[0].f32[0]; s_dclass(c); }
static void s_dsign(Cpu *c) { RET(c->x[0].f64[0] < 0 ? 1 : 0); }

#include "win32_rt.inc"
#include "win32_math.inc"
#include "win32_clr.inc"
#include "win32_bcrypt.inc"
#include "win32_x86.inc"
/* ---------- table ---------- */
#define S(n) { "", #n, s_##n }
#define SA(api, fn) { "", api, s_##fn }
static const ShimDef shims[] = {
  INI_SHIMS, INI_SHIMS2, INNO_SHIMS,
  SHELL_SHIMS
  COMPAT_SHIMS
  X86_SHIMS
  RT_SHIMS
  MATH_SHIMS
  TP_SHIMS
  SEH_SHIMS
  CXX_SHIMS
  SEH32_SHIMS
  THREAD_SHIMS
  FS_SHIMS
  NT_SHIMS
  IOCP_SHIMS
  BUN_SHIMS
  D3D_SHIMS
  AUD_SHIMS
  S(GetLastError), S(SetLastError), S(GetCurrentProcess), S(GetCurrentProcessId), S(ExitProcess), S(TerminateProcess),
  S(GetTickCount), S(GetTickCount64), S(QueryPerformanceCounter), S(QueryPerformanceFrequency), S(GetSystemTimeAsFileTime), S(GetSystemTimePreciseAsFileTime), S(GetSystemTime), S(GetLocalTime), S(Sleep), S(SleepEx),
  S(GetStdHandle), S(WriteFile), S(WriteConsoleA), S(WriteConsoleW), S(ReadFile), S(ReadConsoleA), S(GetConsoleMode), S(SetConsoleMode), S(GetConsoleOutputCP), S(GetConsoleCP), S(SetConsoleOutputCP), S(SetConsoleCP), S(GetConsoleScreenBufferInfo), S(SetConsoleTextAttribute), S(SetConsoleTitleA), S(SetConsoleTitleW), S(GetConsoleTitleW), S(GetConsoleTitleA), S(GetConsoleCursorInfo), S(SetConsoleCursorInfo), S(SetConsoleCursorPosition), S(FillConsoleOutputCharacterW), S(FillConsoleOutputCharacterA), S(FillConsoleOutputAttribute), S(ScrollConsoleScreenBufferW), S(ScrollConsoleScreenBufferA), S(SetConsoleScreenBufferSize), S(SetConsoleWindowInfo), S(WriteConsoleOutputW), S(WriteConsoleOutputA), S(WriteConsoleOutputCharacterW), S(WriteConsoleOutputAttribute), S(ReadConsoleOutputW), S(ReadConsoleOutputCharacterW), S(ReadConsoleOutputAttribute), S(GetLargestConsoleWindowSize), S(WriteConsoleInputW), S(GetConsoleProcessList), S(GetConsoleDisplayMode), S(GetConsoleOriginalTitleW), S(GetConsoleAliasesLengthW), S(GetConsoleScreenBufferInfoEx), S(SetConsoleScreenBufferInfoEx), S(GetCurrentConsoleFontEx), S(GetCurrentConsoleFont), S(SetCurrentConsoleFontEx), S(GetConsoleFontSize), S(GetConsoleWindow), S(AllocConsole), S(SetConsoleCtrlHandler), S(FlushFileBuffers), S(GetFileType), S(CloseHandle),
  S(CreateFileA), S(CreateFileW), S(GetFileAttributesA), S(GetFileAttributesW), S(GetFileAttributesExW), S(FindFirstFileA), S(FindFirstFileW), S(FindClose), S(DeleteFileA), S(GetFullPathNameA), S(GetFinalPathNameByHandleA), S(GetLongPathNameW), S(GetLongPathNameA), S(GetShortPathNameW), S(GetVolumePathNameW), S(PathCchSkipRoot), S(PathCchCombineEx), S(PathCchCanonicalizeEx), S(CreateWaitableTimerW), S(CreateWaitableTimerA), S(CreateWaitableTimerExW), S(CreateWaitableTimerExA), S(SetWaitableTimer), S(SetWaitableTimerEx), S(CancelWaitableTimer), S(NtReadFile), S(NtWriteFile), S(GetFinalPathNameByHandleW), S(GetCurrentDirectoryA), S(GetCurrentDirectoryW), S(GetTempPathA), S(SetFilePointer), S(GetFileSize),
  S(GetCommandLineA), S(GetCommandLineW), S(GetEnvironmentStrings), S(GetEnvironmentStringsW), S(FreeEnvironmentStringsW), S(FreeEnvironmentStringsA), S(GetEnvironmentVariableA), S(GetEnvironmentVariableW), S(SetEnvironmentVariableA), S(ExpandEnvironmentStringsA), S(GetStartupInfoA), S(GetStartupInfoW),
  S(GetModuleHandleA), S(GetModuleHandleW), S(GetModuleHandleExW), S(GetModuleHandleExA), S(LoadLibraryA), S(LoadLibraryW), S(LoadLibraryExA), S(LoadLibraryExW), S(FreeLibrary), S(GetProcAddress), S(GetModuleFileNameA), S(GetModuleFileNameW), S(GetSystemDirectoryA), S(GetWindowsDirectoryA),
  S(GetVersion), S(GetVersionExA), S(GetVersionExW), S(GetProductInfo), S(GetApplicationRestartSettings), S(RegisterApplicationRestart), S(UnregisterApplicationRestart), S(IsProcessorFeaturePresent), S(IsDebuggerPresent), S(IsDBCSLeadByte), S(IsDBCSLeadByteEx), S(GetACP), S(GetOEMCP), S(GetCPInfo), S(GetSystemInfo), S(GetNativeSystemInfo), S(GlobalMemoryStatusEx),
  NODE_SHIMS,
  S(RtlAddFunctionTable), S(RtlAddGrowableFunctionTable), S(RtlGrowFunctionTable), S(RtlDeleteGrowableFunctionTable), S(RtlInstallFunctionTableCallback), S(RtlDeleteFunctionTable), S(EncodePointer), S(DecodePointer),
  S(OutputDebugStringA), S(OutputDebugStringW), S(FormatMessageA), S(FormatMessageW), S(SetErrorMode), S(GetErrorMode), S(SetThreadErrorMode), S(GetLogicalProcessorInformation), S(GetProcessAffinityMask), S(SetThreadAffinityMask), S(SetProcessAffinityMask), S(GetSystemTimeAdjustment), S(SetSystemTime), S(GetThreadPriority), S(SetThreadPriority), S(SetPriorityClass), S(GetPriorityClass), S(GetThreadTimes),
  S(GetProcessHeap), S(HeapCreate), S(HeapDestroy), S(HeapAlloc), S(HeapFree), S(HeapReAlloc), S(HeapSize), S(HeapValidate), S(HeapSetInformation), S(HeapQueryInformation), S(HeapCompact), S(HeapLock), S(HeapUnlock), S(HeapWalk), S(GlobalAlloc), S(GlobalFree), S(GlobalLock), S(GlobalUnlock), S(LocalAlloc), S(LocalFree),
  S(VirtualAlloc), S(VirtualAllocEx), S(VirtualFree), S(VirtualProtect), S(VirtualLock), S(VirtualUnlock), S(VirtualQuery), S(FlushInstructionCache),
  S(CreateTimerQueue),
  S(EventRegister), SA("EventUnregister", ret0), SA("EventSetInformation", ret0), SA("EventWriteTransfer", ret0), SA("EventWrite", ret0), SA("EventEnabled", ret0), SA("EventActivityIdControl", ret0),
  SA("RegisterEventSourceW", ret1), SA("RegisterEventSourceA", ret1), SA("DeregisterEventSource", ret1), SA("ReportEventW", ret1), SA("ReportEventA", ret1),
  S(GetProcessWindowStation), S(GetUserObjectInformationW), SA("GetUserObjectInformationA", GetUserObjectInformationW), S(MapVirtualKeyW),
  S(AllocateAndInitializeSid), S(FreeSid), S(SetEntriesInAclA), S(SetEntriesInAclW), SA("SetSecurityInfo", ret0), SA("SetNamedSecurityInfoW", ret0), SA("GetSecurityInfo", ret50), S(ConvertStringSecurityDescriptorToSecurityDescriptorW), S(ConvertStringSecurityDescriptorToSecurityDescriptorA),
  SA("RegEnumKeyExA", ret2), SA("RegEnumKeyExW", ret2), SA("RegEnumValueW", ret2), SA("RegEnumValueA", ret2), SA("RegGetValueA", RegGetValueW), SA("RegNotifyChangeKeyValue", ret0), SA("RegQueryInfoKeyA", RegQueryInfoKeyW), SA("RegDeleteKeyW", ret2), SA("RegDeleteValueW", ret2), SA("RegFlushKey", ret0),
  S(GetAdaptersAddresses), S(GetUnicastIpAddressTable), SA("FreeMibTable", ret0), SA("CancelMibChangeNotify2", ret0), SA("NotifyIpInterfaceChange", ret50), SA("ConvertInterfaceIndexToLuid", ret50), SA("ConvertInterfaceLuidToNameW", ret50), SA("ConvertInterfaceLuidToIndex", ret50), SA("GetBestInterfaceEx", ret50), SA("GetIfEntry2", ret50), SA("GetIpForwardTable2", ret50), SA("GetAdaptersInfo", ret50), SA("GetNetworkParams", ret50), SA("NotifyAddrChange", ret50), SA("NotifyRouteChange", ret50), SA("GetPerAdapterInfo", ret50), SA("if_nametoindex", ret0), SA("if_indextoname", ret0), SA("ConvertInterfaceLuidToGuid", ret50), SA("ConvertInterfaceGuidToLuid", ret50),
  S(GetThreadLocale), S(GetUserDefaultLCID), S(GetUserDefaultLangID), S(GetSystemDefaultLangID), S(GetUserDefaultUILanguage), S(GetLocaleInfoA), S(GetLocaleInfoW), S(GetLocaleInfoEx), S(IsValidLocale), S(IsValidCodePage), S(EnumSystemLocalesW), S(GetUserDefaultLocaleName), S(LCMapStringW), S(LCMapStringEx), S(LCMapStringA), S(CompareStringW), S(CompareStringA), S(CompareStringEx), S(CompareStringOrdinal), S(GetStringTypeW), S(GetStringTypeA), S(MultiByteToWideChar), S(WideCharToMultiByte), S(CharUpperA),
  S(lstrlenA), S(lstrlenW), S(lstrcpyA), S(lstrcmpiA), S(lstrcmpA), S(InterlockedCompareExchange), S(InterlockedExchange), S(InterlockedIncrement), S(InterlockedDecrement), S(InterlockedExchangeAdd), S(QueueUserAPC),
  S(CreateNamedPipeW), S(CreateNamedPipeA), S(ConnectNamedPipe), S(ReadFileEx), S(WriteFileEx), S(RtlNtStatusToDosError), S(CancelIo), SA("CancelIoEx", CancelIo),
  S(GetTimeZoneInformation), S(FileTimeToSystemTime), S(SystemTimeToFileTime), S(FileTimeToLocalFileTime), S(GetDateFormatA), S(GetTimeFormatA), S(SystemFunction036), SA("ProcessPrng", SystemFunction036), SA("BCryptGenRandom", BCryptGenRandom), S(GetComputerNameA), S(GetUserNameA), S(GetDriveTypeA), S(GetLogicalDrives), S(GetDiskFreeSpaceExA), S(SetHandleCount), S(GetHandleInformation), S(SetHandleInformation), S(DuplicateHandle), S(CreatePipe), S(PeekNamedPipe), S(GetOverlappedResult), S(Beep), S(DebugBreak), S(GetProcessId), S(K32GetProcessMemoryInfo), SA("GetProcessMemoryInfo", K32GetProcessMemoryInfo),
  /* file system stubs — no VFS yet: everything "does not exist" */
  SA("GetFileAttributesExA", GetFileAttributesA), SA("GetFileAttributesExW", GetFileAttributesA), SA("FindFirstFileExA", FindFirstFileA), SA("FindFirstFileExW", FindFirstFileA), SA("FindNextFileA", ret0), SA("FindNextFileW", ret0), SA("FindClose", ret1),
  SA("DeleteFileA", ret0), SA("DeleteFileW", ret0), SA("RemoveDirectoryA", ret0), SA("RemoveDirectoryW", ret0), SA("CreateDirectoryA", ret0), SA("CreateDirectoryW", ret0), SA("MoveFileA", ret0), SA("MoveFileW", ret0), SA("MoveFileExA", ret0), SA("MoveFileExW", ret0), SA("CopyFileA", ret0), SA("CopyFileW", ret0),
  SA("SetFileAttributesA", ret1), SA("SetFileAttributesW", ret1), SA("SetCurrentDirectoryA", ret0), SA("SetCurrentDirectoryW", ret0), SA("GetFileSizeEx", ret0), SA("GetFileSize", ret0), SA("SetFilePointer", ret0), SA("SetFilePointerEx", ret0), SA("SetEndOfFile", ret0), SA("FlushFileBuffers", ret1), SA("GetFileInformationByHandle", ret0), S(GetFileInformationByHandleEx), SA("GetFileTime", ret0), SA("SetFileTime", ret0),
  SA("CreateFileMappingA", ret0), SA("CreateFileMappingW", ret0), SA("MapViewOfFile", ret0), SA("UnmapViewOfFile", ret1), SA("LockFileEx", ret1), SA("UnlockFileEx", ret1), SA("LockFile", ret1), SA("UnlockFile", ret1), SA("GetTempPathA", ret0), SA("GetTempPathW", ret0), SA("GetTempFileNameA", ret0), SA("GetTempFileNameW", ret0),
  SA("CreateProcessA", ret0), SA("CreateProcessW", ret0), SA("GetExitCodeProcess", ret0), SA("OpenProcess", ret0), SA("CreatePipe", ret0), SA("CreateNamedPipeA", ret0), SA("ConnectNamedPipe", ret0), SA("PeekNamedPipe", ret0), SA("CreateToolhelp32Snapshot", ret0), SA("WaitForInputIdle", ret0), SA("SetConsoleCtrlHandler", ret1), SA("GenerateConsoleCtrlEvent", ret0),
  SA("_fullpath", fullpath), SA("_umask", umask), SA("_getpid", getpid), SA("_chdir", retm1e), SA("_spawnvp", retm1e), SA("_execve", retm1e), SA("_filbuf", retm1e), SA("_getch", retm1e), SA("_getche", retm1e), SA("_telli64", telli64), SA("_open_osfhandle", retm1e), SA("_creat", retm1e), SA("_chmod", retm1e), SA("_rmdir", retm1e), SA("_mktemp", ret0), SA("_wcsnicmp", wcsnicmp), SA("_ctime64", ctime64), S(asctime), SA("_ctime64_s", ret0), SA("_wmkdir", retm1e), SA("_wchdir", retm1e), SA("_wunlink", retm1e), SA("_wrmdir", retm1e), SA("_popen", popen), SA("_wpopen", wpopen), SA("_pclose", pclose), SA("system", system), SA("_wsystem", wsystem),
  SA("AreFileApisANSI", ret1), SA("SetFileApisToANSI", ret0), SA("SetFileApisToOEM", ret0),
  /* WS2_32 */
  NET_SHIMS
  SA("InitializeSListHead", ret0), S(VerSetConditionMask), SA("VerifyVersionInfoW", ret1), SA("VerifyVersionInfoA", ret1), SA("SetStdHandle", ret1), S(GlobalMemoryStatus), S(ReadConsoleW), SA("ReadConsoleA", ReadConsoleW), SA("GetDateFormatW", ret0), SA("GetTimeFormatW", ret0), SA("GetDateFormatEx", ret0), S(LocalFileTimeToFileTime), S(InterlockedPushEntrySList), S(InterlockedPopEntrySList), S(InterlockedFlushSList), SA("QueryDepthSList", ret0), S(GetSystemDirectoryW), SA("GetWindowsDirectoryW", GetSystemDirectoryW), SA("GetWindowsDirectoryA", GetSystemDirectoryA), S(SearchPathW), S(SearchPathA), S(GetUserNameW), S(GetComputerNameW), S(GetComputerNameExW), SA("GetComputerNameExA", GetComputerNameA), SA("GetCompressedFileSizeA", retm1), SA("GetCompressedFileSizeW", retm1),
  /* COMCTL32 */
  SA("InitCommonControls", ret0), SA("InitCommonControlsEx", ret1), SA("MakeDragList", ret1), SA("LBItemFromPt", retm1), SA("DrawInsert", ret0),
  /* ADVAPI32 (minimal) */
  S(CryptAcquireContextA), SA("CryptAcquireContextW", CryptAcquireContextA), S(CryptGenRandom), SA("CryptReleaseContext", ret1), S(OpenProcessToken), S(GetTokenInformation), S(CheckTokenMembership), SA("DuplicateToken", ret0), SA("EqualSid", ret0), SA("AccessCheck", ret0), SA("GetSecurityInfo", ret1), SA("SaferCreateLevel", ret0), SA("SaferComputeTokenFromLevel", ret0), SA("SetTokenInformation", ret0),
  /* CRT */
  S(malloc), S(calloc), S(realloc), S(free), SA("_msize", msize), SA("_aligned_malloc", aligned_malloc), SA("_aligned_free", aligned_free), SA("_set_new_mode", set_new_mode), SA("_callnewh", ret0), SA("_query_new_handler", ret0), SA("_set_new_handler", ret0),
  SA("_initterm", initterm), SA("_initterm_e", initterm_e), S(atexit), SA("_onexit", onexit), SA("_crt_atexit", atexit), SA("_crt_at_quick_exit", atexit), SA("_register_onexit_function", register_onexit_function), SA("_initialize_onexit_table", ret0), SA("_execute_onexit_table", ret0),
  S(exit), SA("_Exit", _exit), SA("_exit", _exit), SA("_cexit", cexit), SA("_c_exit", ret0), S(abort), SA("quick_exit", _exit),
  SA("__p___argc", p___argc), SA("__p___argv", p___argv), SA("__p___wargv", p___wargv), SA("__p__acmdln", p__acmdln), SA("__p__wcmdln", p__wcmdln), SA("__p__environ", p__environ), SA("__p__wenviron", p__wenviron), SA("__p__fmode", p__fmode), SA("__p__commode", p__commode), SA("__p__pgmptr", p__pgmptr), SA("_get_initial_narrow_environment", get_initial_narrow_environment), SA("_get_initial_wide_environment", get_initial_wide_environment), SA("_errno", errno), SA("__doserrno", doserrno), SA("_get_errno", get_errno), SA("_set_errno", set_errno), S(mbstowcs), S(mbsrtowcs),
  SA("__acrt_iob_func", acrt_iob_func), SA("__iob_func", iob_func), SA("__getmainargs", getmainargs), SA("__wgetmainargs", wgetmainargs), SA("__set_app_type", set_app_type), SA("_set_app_type", set_app_type), SA("__setusermatherr", setusermatherr), SA("_controlfp", controlfp), SA("_controlfp_s", controlfp_s), SA("_fpreset", fpreset), SA("_set_fmode", set_fmode), S(signal), S(raise), SA("_amsg_exit", abort),
  SA("_configure_narrow_argv", ret0), SA("_configure_wide_argv", ret0), SA("_initialize_narrow_environment", ret0), SA("_initialize_wide_environment", ret0), SA("_set_invalid_parameter_handler", ret0), SA("_set_thread_local_invalid_parameter_handler", ret0), SA("_seh_filter_exe", ret0), SA("_seh_filter_dll", ret0), SA("_configthreadlocale", ret0), SA("__lconv_init", ret0), SA("_setmode", ret0), SA("_set_abort_behavior", ret0), SA("_set_error_mode", ret0), SA("_set_printf_count_output", ret0), SA("_get_osfhandle", retm1), SA("_crt_debugger_hook", ret0), SA("_invalid_parameter_noinfo", invalid_parameter_noinfo), SA("_invalid_parameter_noinfo_noreturn", invalid_parameter_noinfo), SA("_beginthreadex", ret0), SA("_beginthread", retm1), SA("_endthreadex", _exit), SA("__threadid", ret0), SA("_lock", ret0), SA("_unlock", ret0), SA("__pctype_func", pctype_func), SA("___mb_cur_max_func", ret1), SA("___lc_codepage_func", ret0), SA("___lc_handle_func", ret0), SA("___lc_collate_cp_func", ret0), SA("_create_locale", ret0), SA("_free_locale", ret0), SA("_get_current_locale", ret0), S(setlocale), S(_wsetlocale), S(localeconv), SA("_setlocale", setlocale),
  SA("__stdio_common_vfprintf", stdio_common_vfprintf), SA("__stdio_common_vfprintf_s", stdio_common_vfprintf), SA("__stdio_common_vfwprintf", stdio_common_vfwprintf), SA("__stdio_common_vsprintf", stdio_common_vsprintf), SA("__stdio_common_vsprintf_s", stdio_common_vsprintf_s), SA("__stdio_common_vsnprintf_s", stdio_common_vsnprintf_s), SA("__stdio_common_vswprintf", stdio_common_vswprintf), SA("__stdio_common_vswprintf_s", stdio_common_vswprintf_s), SA("__stdio_common_vsprintf_p", stdio_common_vsprintf),
  SA("_vscprintf", vscprintf), SA("_vscwprintf", vscwprintf), S(printf), S(wprintf), S(vprintf), S(fprintf), S(vfprintf), S(sprintf), SA("_snprintf", snprintf), S(snprintf), SA("_vsnprintf", vsnprintf), S(vsnprintf), S(vsprintf), SA("__mingw_vfprintf", vfprintf), SA("__mingw_vprintf", vprintf), SA("__mingw_vsprintf", vsprintf), SA("__mingw_vsnprintf", vsnprintf), SA("__mingw_printf", printf), SA("__mingw_fprintf", fprintf), SA("__mingw_sprintf", sprintf), SA("__mingw_snprintf", snprintf), SA("__ms_vsnprintf", vsnprintf),
  S(puts), SA("_putws", putws), S(fputs), S(putchar), S(fputc), SA("putc", fputc), S(fwrite), S(fflush), S(setvbuf), S(fopen), S(fopen_s), S(fclose), S(fgets), S(fread), S(getchar), S(fgetc), S(getc), S(feof), S(ferror), SA("_fileno", fileno), SA("_isatty", isatty), SA("_write", write), SA("_read", read), SA("_open", open), SA("_close", close), SA("_fdopen", ret0), SA("_wfopen", ret0), SA("_lseek", retm1), SA("_lseeki64", retm1), SA("_stat64", retm1), SA("_fstat64", retm1), SA("_wstat64", retm1), SA("_access", retm1), SA("_chsize", retm1), SA("_unlink", retm1), SA("remove", retm1), SA("rename", retm1), SA("_mkdir", retm1), SA("_getcwd", ret0), SA("_dup", retm1), SA("_dup2", retm1), SA("_pipe", retm1), SA("_commit", ret0), SA("_locking", ret0), SA("_filelengthi64", retm1), SA("ungetc", retm1),
  SA("_time64", time64), SA("time", time64), S(clock), SA("_localtime64", localtime64), SA("_gmtime64", gmtime64), SA("_localtime64_s", localtime64_s), SA("_gmtime64_s", localtime64_s), SA("_wcsicmp", wcsicmp), S(wcsncpy_s), S(wcscpy_s), S(wcscat_s), SA("DisableThreadLibraryCalls", ret1), S(GetActiveProcessorCount), S(InitializeProcThreadAttributeList), S(UpdateProcThreadAttribute), S(DeleteProcThreadAttributeList), SA("GetMaximumProcessorCount", GetActiveProcessorCount), SA("GetActiveProcessorGroupCount", ret1), S(wcsncmp), SA("__fpe_flt_rounds", ret1), S(wcschr), S(wcsrchr), S(wcstombs), S(AddDllDirectory), SA("RemoveDllDirectory", ret1), S(SetDllDirectoryW), S(SetDllDirectoryA), SA("CancelIoEx", ret1), SA("SetNamedPipeHandleState", ret1), SA("GetFileVersionInfoSizeW", ret0), SA("GetFileVersionInfoW", ret0), SA("VerQueryValueW", ret0), SA("PssQuerySnapshot", ret1), SA("LookupPrivilegeValueA", ret0), SA("CreateSymbolicLinkW", ret0), SA("_mktime64", mktime64), S(strftime), SA("_tzset", tzset), SA("__timezone", timezone), SA("__daylight", daylight), SA("__tzname", tzname), SA("_get_timezone", ret0), SA("_ftime64", ret0), SA("_difftime64", ret0),
  S(getenv), S(getenv_s), SA("_dupenv_s", ret0), SA("_putenv", ret0), S(system), S(rand), S(srand), SA("rand_s", ret0),
  S(sscanf), S(strlen), S(wcslen), S(strnlen), S(strcmp), SA("_stricmp", stricmp), SA("_strcmpi", stricmp), SA("stricmp", stricmp), S(strncmp), SA("_strnicmp", strnicmp), S(wcscmp), S(strcpy), S(strncpy), S(strcat), S(strchr), S(strrchr), S(strstr), SA("_strdup", strdup), S(strdup), S(strtol), S(strtoul), S(strtoll), SA("_strtoi64", strtoll), SA("_strtoui64", strtoll), SA("strtoull", strtoll), S(atoi), S(atol), S(atoll), SA("_atoi64", atoll), S(strtod), S(atof), S(toupper), S(tolower), S(isdigit), S(isalpha), S(isalnum), S(isspace), S(isupper), S(islower), S(isprint), S(isxdigit), S(ispunct), SA("iscntrl", ret0), SA("isgraph", isprint), SA("iswspace", isspace), SA("iswdigit", isdigit), SA("iswalpha", isalpha), SA("towupper", toupper), SA("towlower", tolower), SA("_isctype", ret0), SA("strerror", ret0), SA("_strerror", ret0), SA("strerror_s", ret0), SA("strcpy_s", ret0), SA("strcat_s", ret0), SA("strncpy_s", ret0), SA("memcpy_s", ret0), SA("memmove_s", ret0), SA("strtok", ret0), SA("strtok_s", ret0), SA("strspn", ret0), SA("strcspn", ret0), SA("strpbrk", ret0), SA("_strlwr", ret0), SA("_strupr", ret0), SA("_strrev", ret0), SA("strcoll", strcmp), SA("_itoa", ret0), SA("_ltoa", ret0), SA("_ultoa", ret0), SA("_i64toa", ret0), SA("_ui64toa", ret0),
  S(memcpy), SA("memmove", memcpy), S(memset), S(memcmp), S(memchr), S(qsort), SA("qsort_s", qsort), S(bsearch), SA("_setjmp", setjmp), SA("_setjmpex", setjmp), SA("_setjmp3", setjmp), S(longjmp), S(div), SA("ldiv", div), S(abs), S(labs), SA("_abs64", llabs), S(llabs),
  S(sin), S(cos), S(tan), S(exp), S(log), S(pow), S(atan2), S(asin), S(acos), S(atan), S(sinh), S(cosh), S(tanh), S(log10), S(log2), S(cbrt), S(sqrt), S(floor), S(ceil), S(fabs), S(fmod), S(round), S(trunc), S(ldexp), S(frexp), S(modf), S(hypot), SA("_hypot", hypot), S(sinf), S(cosf), S(tanf), S(expf), S(logf), S(powf), S(atan2f), S(sqrtf), S(floorf), S(ceilf), S(fabsf), S(fmodf), S(roundf), SA("_dclass", dclass), SA("_fdclass", fdclass), SA("_dsign", dsign), SA("_fdsign", dsign), SA("_dtest", dclass), SA("_fdtest", fdclass), SA("_finite", ret1), SA("_isnan", ret0), SA("_copysign", copysign), S(copysign), S(exp2), SA("nearbyint", round), SA("rint", round), S(lround), SA("llround", lround), S(fmin), S(fmax),
  SA("__CxxFrameHandler4", CxxFrameHandler4), SA("__std_exception_copy", ret0), SA("__std_exception_destroy", ret0), SA("__std_type_info_destroy_list", ret0), SA("__current_exception", ret0), SA("__current_exception_context", ret0), SA("_except_handler4_common", ret1), SA("__intrinsic_setjmp", setjmp), SA("__report_gsfailure", abort), SA("_purecall", abort), SA("terminate", abort), SA("__p__mbctype", pctype_func), SA("_ismbblead", ret0), SA("__mb_cur_max", ret1),
  BCRYPT_SHIMS
  CLR_SHIMS   /* last: these override earlier definitions (CoreCLR needs the real semantics) */
};
/* OLEAUT32 exports most of its API by ordinal only */
static int oleaut_dll(const char *d) { const char *k = "oleaut32"; for (int i = 0; k[i]; i++) { char ch = d[i]; if (ch >= 'A' && ch <= 'Z') ch += 32; if (ch != k[i]) return 0; } return 1; }
static const char *oleaut_ordinal(u32 o) { switch (o) { case 2: return "SysAllocString"; case 4: return "SysAllocStringLen"; case 6: return "SysFreeString"; case 7: return "SysStringLen"; case 5: return "SysReAllocStringLen"; case 8: return "VariantInit"; case 9: return "VariantClear"; case 10: return "VariantCopy"; case 149: return "SysStringByteLen"; case 150: return "SysAllocStringByteLen"; default: return 0; } }
/* ordinal imports ("#n") of ws2_32 / oleaut32 → export name */
static const char *shim_canonical(const char *dll, const char *name) {
  if (name[0] == '#' && dll && (e_stricmp(dll, "ws2_32.dll") == 0 || e_stricmp(dll, "ws2_32") == 0 || e_stricmp(dll, "wsock32.dll") == 0)) {   /* ws2_32 is imported by ordinal: map to the export name */
    static const struct { int ord; const char *n; } ws2[] = { {1,"accept"},{2,"bind"},{3,"closesocket"},{4,"connect"},{5,"getpeername"},{6,"getsockname"},{7,"getsockopt"},{8,"htonl"},{9,"htons"},{10,"ioctlsocket"},{11,"inet_addr"},{12,"inet_ntoa"},{13,"listen"},{14,"ntohl"},{15,"ntohs"},{16,"recv"},{17,"recvfrom"},{18,"select"},{19,"send"},{20,"sendto"},{21,"setsockopt"},{22,"shutdown"},{23,"socket"},{51,"gethostbyaddr"},{52,"gethostbyname"},{53,"getprotobyname"},{54,"getprotobynumber"},{55,"getservbyname"},{56,"getservbyport"},{57,"gethostname"},{111,"WSAGetLastError"},{112,"WSASetLastError"},{115,"WSAStartup"},{116,"WSACleanup"},{151,"__WSAFDIsSet"} };
    int o = 0; for (const char *q = name + 1; *q >= '0' && *q <= '9'; q++) o = o * 10 + (*q - '0');
    for (size_t k = 0; k < sizeof ws2 / sizeof ws2[0]; k++) if (ws2[k].ord == o) { name = ws2[k].n; break; }
  }
  if (name[0] == '#' && dll && (dll[0] == 'O' || dll[0] == 'o') && oleaut_dll(dll)) { u32 o = 0; for (const char *p = name + 1; *p >= '0' && *p <= '9'; p++) o = o * 10 + (u32)(*p - '0'); const char *n = oleaut_ordinal(o); if (n) name = n; }
  return name;
}
static ShimFn find_shim(const char *dll, const char *name) {
  name = shim_canonical(dll, name);
  for (size_t i = 0; i < sizeof shims / sizeof shims[0]; i++) if (!e_strcmp(shims[i].name, name)) return shims[i].fn; extern ShimFn gdi_find_shim(const char *name); return gdi_find_shim(name); }
void w32_api_trace(int on) { g_fs_w->api_trace = on; }
int w32_have_shim(const char *dll, const char *name) { return find_shim(dll, name) != 0; }

/* ---------- init ---------- */
void w32_init(Win32 *w, Cpu *c, Mem *m) { g_fs_w = w; ProcFs *fs = w->fs ? w->fs : emu_alloc(sizeof(ProcFs));
  e_memset(w, 0, sizeof *w); w->fs = fs; w->cpu = c; w->mem = m; c->w = w; c->mem = m;
  w->start_ms = host_now_ms(); w->rand_state = 1; w->next_handle = 0x100;
  u8 *shim = mem_map(m, G_SHIM_BASE, G_SHIM_SIZE, "shim"); e_memset(shim, 0xCC, 0x10000);
  mem_map(m, G_STACK_BASE, G_STACK_SIZE, "stack");
  w->heap_cur = G_HEAP_BASE; w->heap_end = G_HEAP_BASE; heap_grow(w, 0);
  w->fb_cur = G_FB_BASE;
  w->str_cur = G_SHIM_BASE + STR_OFF;
  w->teb = G_SHIM_BASE + TEB_OFF; w->peb = G_SHIM_BASE + PEB_OFF;
  c->gs_base = w->teb; c->fs_base = w->teb;
  w->th[0].used = 1; w->th[0].state = 1; w->th[0].cpu = c; w->th[0].teb = w->teb; w->th[0].kobj = -1; w->th[0].exit_code = 259; w->cur_th = 0; w->nth = 1; w->wait_deadline = -1;
  w->th[0].tls_arr = w32_heap_alloc(w, 8 * MAX_TLS_IMAGES, 1);
  teb_init(w, c, w->teb, G_STACK_BASE, G_STACK_BASE + G_STACK_SIZE, 4242, 4243, w->th[0].tls_arr);
  u64 params = G_SHIM_BASE + PARAMS_OFF; wr64(c, w->peb + 0x20, params); wr32(c, w->peb + 0x118, 10); wr32(c, w->peb + 0x11c, 0); wr16(c, w->peb + 0x120, 19045); wr64(c, w->peb + 0x18, shim_bump(w, 0x100));
  wr8(c, w->peb + 0x02, 0); /* BeingDebugged */
  w->ubuf = shim_bump(w, 0x1000);
  /* FILE structures: 3 × 0x60; mark fileno at +0x30?  UCRT layout is opaque; keep zero */
  wr32(c, w->ubuf + 0x240, 0x4000); wr32(c, w->ubuf + 0x248, 0);
  /* ctype table at ubuf+0x400: 256 shorts (pctype returns pointer to table) */
  wr64(c, w->ubuf + 0x2a8, w->ubuf + 0x400); wr32(c, w->ubuf + 0x288, 43); wr64(c, w->ubuf + 0x298, 0x7ff0000000000000ull); wr32(c, w->ubuf + 0x268, 1); wr64(c, w->ubuf + 0x278, w->ubuf + 0x7c0); wr64(c, w->ubuf + 0x280, w->ubuf + 0x7c0); wr8(c, w->ubuf + 0x7c0, 'U'); wr8(c, w->ubuf + 0x7c1, 'T'); wr8(c, w->ubuf + 0x7c2, 'C'); wr32(c, w->ubuf + 0x2b8, 10); wr32(c, w->ubuf + 0x2c0, 0x0a00); wr32(c, w->ubuf + 0x2b0, 2); wr32(c, w->ubuf + 0x2b4, 0x4a61); wr32(c, w->ubuf + 0x2a0, 1252);
  for (int i = 0; i < 256; i++) { u16 t = 0; if (i >= 'A' && i <= 'Z') t |= 0x101; if (i >= 'a' && i <= 'z') t |= 0x102; if (i >= '0' && i <= '9') t |= 0x204; if (i == ' ' || (i >= 9 && i <= 13)) t |= 0x48; if (i < 32 || i == 127) t |= 0x20; if ((i >= 33 && i <= 47) || (i >= 58 && i <= 64) || (i >= 91 && i <= 96) || (i >= 123 && i <= 126)) t |= 0x10; wr16(c, w->ubuf + 0x400 + i * 2, t); }
  /* fixed slots */
  w->nslots = SLOT_FIRST; w->slots[SLOT_STARTUP].name = "__nebula_startup"; w->slots[SLOT_STARTUP].dll = "nebula"; w->slots[SLOT_CONT].name = "__nebula_continue"; w->slots[SLOT_CONT].dll = "nebula";
  w->screen_w = 800; w->screen_h = 600;
  extern void gdi_init(Win32 *w); gdi_init(w);
}

/* set up command line, argv, environment, stack and the start-up continuation (called by api.c after pe_load) */
void w32_start(Win32 *w, const char *cmdline) {
  Cpu *c = w->cpu;
  w->th[0].tid = w->pid + 1; if (w->mode32) { wr32(c, w->teb + 0x20, (u32)w->pid); wr32(c, w->teb + 0x24, (u32)w->th[0].tid); } else { wr64(c, w->teb + 0x40, (u64)w->pid); wr64(c, w->teb + 0x48, (u64)w->th[0].tid); }
  { int idx = kobj_new(KO_THREAD, 0, 0); if (idx >= 0) { KObj *ko = kobj_get(idx); ko->pid = w->pid; ko->tid = w->th[0].tid; ko->state = 1; ko->exit_code = 259; ko->refs = 1; w->th[0].kobj = idx; } }
  e_snprintf(w->cmdline, sizeof w->cmdline, "%s", cmdline);
  w->cmdline_a = w32_guest_strdup(w, w->cmdline); w->cmdline_w = guest_wstrdup(w, w->cmdline);
  /* argv split (quotes aware) */
  u64 argv = w32_heap_alloc(w, 8 * 64, 1); u64 wargv = w32_heap_alloc(w, 8 * 64, 1); int argc = 0;
  const char *p = w->cmdline; char tok[1024];
  while (*p && argc < 63) {
    while (*p == ' ') p++; if (!*p) break;
    int tl = 0, q = 0;
    while (*p && (q || (*p != ' ' && *p != '\t'))) {   /* MSVC rules: 2n backslashes + quote -> n backslashes + toggle; 2n+1 -> n + literal quote */
      if (*p == '\\') { int bs = 0; while (*p == '\\') { bs++; p++; } if (*p == '"') { for (int k = 0; k < bs / 2 && tl < 1023; k++) tok[tl++] = '\\'; if (bs & 1) { if (tl < 1023) tok[tl++] = '"'; p++; } } else { for (int k = 0; k < bs && tl < 1023; k++) tok[tl++] = '\\'; } continue; }
      if (*p == '"') { if (q && p[1] == '"') { if (tl < 1023) tok[tl++] = '"'; p += 2; continue; } q = !q; p++; continue; }
      if (tl < 1023) tok[tl++] = *p; p++;
    }
    tok[tl] = 0;
    wrp(c, argv + argc * PSZ, w32_guest_strdup(w, tok)); wrp(c, wargv + argc * PSZ, guest_wstrdup(w, tok)); argc++;
  }
  wr32(c, w->ubuf + 0x200, (u32)argc); wr64(c, w->ubuf + 0x208, argv); wr64(c, w->ubuf + 0x210, w->cmdline_a); wr64(c, w->ubuf + 0x218, wargv); wr64(c, w->ubuf + 0x220, w->cmdline_w);
  static const char *base_envs[] = { "PATH=C:\\Windows\\System32;C:\\Windows\\System32\\WindowsPowerShell\\v1.0", "SYSTEMROOT=C:\\Windows", "TEMP=C:\\tmp", "TMP=C:\\tmp", "USERPROFILE=C:\\Users\\user", "HOMEPATH=\\Users\\user", "USERNAME=user", "COMPUTERNAME=NEBULA", "OS=Windows_NT", "NEBULA_X64=1", "BUN_JSC_useConcurrentGC=false", "BUN_JSC_useConcurrentJIT=false", "PROCESSOR_ARCHITECTURE=AMD64", "PROCESSOR_IDENTIFIER=Intel64 Family 6 Model 158 Stepping 10, GenuineIntel", "PROCESSOR_LEVEL=6", "PROCESSOR_REVISION=9e0a", "NUMBER_OF_PROCESSORS=1", "PATHEXT=.COM;.EXE;.BAT;.CMD", "HOMEDRIVE=C:", "WINDIR=C:\\Windows", "COMSPEC=C:\\Windows\\System32\\cmd.exe", 0 };
  /* defaults for the official .NET runtime (CoreCLR) — overridable with --env: no W^X double mapping (we have no MapViewOfFile3 aliasing),
     GC regions in a fixed 768 MB reservation, invariant globalization (no ICU on the virtual disk), no telemetry/first-run noise */
  static const char *dotnet_envs[] = { "DOTNET_EnableWriteXorExecute=0", "DOTNET_GCRegionRange=0x30000000", "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1", "DOTNET_SYSTEM_GLOBALIZATION_PREDEFINED_CULTURES_ONLY=0", "DOTNET_CLI_TELEMETRY_OPTOUT=1", "DOTNET_NOLOGO=1", "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1", "DOTNET_TieredPGO=0", "DOTNET_ROOT=C:\\Program Files\\dotnet", 0 };
  const char *envs[128]; int ne = 0; for (int i = 0; base_envs[i] && ne < 64; i++) envs[ne++] = base_envs[i];
  for (int i = 0; dotnet_envs[i] && ne < 90; i++) { const char *eq = dotnet_envs[i]; size_t nl = 0; while (eq[nl] && eq[nl] != '=') nl++; int dup = 0; for (int k = 0; k < g_nextra_env; k++) if (e_strncmp(g_extra_env[k], dotnet_envs[i], nl) == 0 && g_extra_env[k][nl] == '=') dup = 1; if (!dup) envs[ne++] = dotnet_envs[i]; }
  for (int i = 0; i < g_nextra_env && ne < 127; i++) envs[ne++] = g_extra_env[i]; envs[ne] = 0;
  u64 envp = w32_heap_alloc(w, 8 * 130, 1); size_t total = 1; for (int i = 0; envs[i]; i++) total += e_strlen(envs[i]) + 1;
  u64 blk = w32_heap_alloc(w, total, 1), wblk = w32_heap_alloc(w, total * 2, 1); u64 o = 0;
  for (int i = 0; envs[i]; i++) { size_t l = e_strlen(envs[i]) + 1; u8 *d = mem_ptr(w->mem, blk + o, l, 1); if (d) e_memcpy(d, envs[i], l); for (size_t k = 0; k < l; k++) wr16(c, wblk + (o + k) * 2, (u8)envs[i][k]); wrp(c, envp + i * PSZ, blk + o); o += l; }
  w->env_a = blk; w->env_w = wblk; wr64(c, w->ubuf + 0x228, envp); wr64(c, w->ubuf + 0x230, 0);
  { extern void w32_env_init(const char **envs); w32_env_init(envs); }
  wr64(c, w->ubuf + 0x250, w32_guest_strdup(w, "C:\\nebula\\app.exe"));
  /* ProcessParameters: CommandLine UNICODE_STRING at +0x70, ImagePathName at +0x60 */
  u64 params = G_SHIM_BASE + PARAMS_OFF; size_t cl = e_strlen(w->cmdline);
  if (w->mode32) {   /* RTL_USER_PROCESS_PARAMETERS32: std handles @0x18, ImagePathName @0x38, CommandLine @0x40, Environment @0x48 */
    wr32(c, params + 0x00, 0x400); wr32(c, params + 0x04, 0x1000); wr32(c, params + 0x08, 1); wr32(c, params + 0x18, 0x10); wr32(c, params + 0x1C, 0x11); wr32(c, params + 0x20, 0x12);
    wr16(c, params + 0x40, (u16)(cl * 2)); wr16(c, params + 0x42, (u16)(cl * 2 + 2)); wr32(c, params + 0x44, (u32)w->cmdline_w); wr32(c, params + 0x48, (u32)wblk);
    { char ip[600]; if (w->exe_path[0]) e_snprintf(ip, sizeof ip, "%s", w->exe_path); else e_snprintf(ip, sizeof ip, "C:\\nebula\\%s", w->exe_name); u64 img = guest_wstrdup(w, ip); u16 il = (u16)(e_strlen(ip) * 2); wr16(c, params + 0x38, il); wr16(c, params + 0x3A, il + 2); wr32(c, params + 0x3C, (u32)img); }
    c->r[RSP] = G_STACK_BASE + G_STACK_SIZE - 0x100; c->rip = SLOT_ADDR(SLOT_STARTUP); cpu_push(c, 0); return;
  }
  wr16(c, params + 0x70, (u16)(cl * 2)); wr16(c, params + 0x72, (u16)(cl * 2 + 2)); wr64(c, params + 0x78, w->cmdline_w);
  wr32(c, params + 0x00, 0x400); wr32(c, params + 0x04, 0x1000); wr32(c, params + 0x08, 1);   /* MaximumLength, Length, Flags = NORMALIZED */
  wr64(c, params + 0x20, 0x10); wr64(c, params + 0x28, 0x11); wr64(c, params + 0x30, 0x12);   /* hStdInput/Output/Error: Zig's std.io.getStdOut() reads them straight from the PEB (OpenTUI's renderer wrote its frames to handle 0) */
  { char ip[600]; if (w->exe_path[0]) e_snprintf(ip, sizeof ip, "%s", w->exe_path); else e_snprintf(ip, sizeof ip, "C:\\nebula\\%s", w->exe_name); u64 img = guest_wstrdup(w, ip); u16 il = (u16)(e_strlen(ip) * 2); wr16(c, params + 0x60, il); wr16(c, params + 0x62, il + 2); wr64(c, params + 0x68, img); } wr64(c, params + 0x80, wblk);
  /* stack: top minus red zone; the start-up slot acts as the return address of "the OS" */
  c->r[RSP] = G_STACK_BASE + G_STACK_SIZE - 0x100;
  c->rip = SLOT_ADDR(SLOT_STARTUP); cpu_push(c, 0);   /* fake return address popped by dispatch */
}
