/* freestanding helpers: memory allocator, string/format routines, math shims */
#include "emu.h"
#include <stdarg.h>

/* ---------- allocator ---------- */
#ifdef __wasm__
extern unsigned char __heap_base;
static uintptr_t arena_cur = 0, arena_end = 0;
static size_t used_total = 0;
void *emu_alloc(size_t n) {
  if (!arena_cur) { arena_cur = (uintptr_t)&__heap_base; arena_end = (uintptr_t)__builtin_wasm_memory_size(0) * 65536; }
  arena_cur = (arena_cur + 15) & ~(uintptr_t)15;
  if (arena_cur + n > arena_end) {
    size_t need = (arena_cur + n - arena_end + 65535) / 65536 + 16;
    if (__builtin_wasm_memory_grow(0, need) == (size_t)-1) return 0;
    arena_end = (uintptr_t)__builtin_wasm_memory_size(0) * 65536;
  }
  void *p = (void *)arena_cur; arena_cur += n; used_total += n;
  /* no memset: the arena only hands out memory that has never been written (fresh wasm pages are zero), and the browser commits
     pages lazily — clearing a 768 MB GC reservation would make it resident at once (pool_take clears recycled blocks itself) */
  return p;
}
int emu_free(void *p) { (void)p; return 0; }   /* arena: no free (caller pools the block) */
/* make sure the arena can hand out n more bytes with ONE memory.grow (installing a 77 MB runtime pack file by file would grow ~190 times) */
int emu_reserve(size_t n) {
  if (!arena_cur) { arena_cur = (uintptr_t)&__heap_base; arena_end = (uintptr_t)__builtin_wasm_memory_size(0) * 65536; }
  if (arena_cur + n <= arena_end) return 1;
  size_t need = (arena_cur + n - arena_end + 65535) / 65536 + 16;
  if (__builtin_wasm_memory_grow(0, need) == (size_t)-1) return 0;
  arena_end = (uintptr_t)__builtin_wasm_memory_size(0) * 65536; return 1;
}
size_t emu_alloc_used(void) { return used_total; }
#else
#include <stdlib.h>
static size_t used_total = 0;
void *emu_alloc(size_t n) { used_total += n; return calloc(1, n ? n : 1); }
int emu_free(void *p) { free(p); return 1; }
int emu_reserve(size_t n) { (void)n; return 1; }
size_t emu_alloc_used(void) { return used_total; }
#endif

/* ---------- strings ---------- */
size_t e_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int e_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
int e_strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (u8)*a - (u8)*b : 0; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int e_stricmp(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower((u8)*a) - lower((u8)*b); }
void *e_memcpy(void *d, const void *s, size_t n) { u8 *dd = d; const u8 *ss = s; while (n--) *dd++ = *ss++; return d; }
void *e_memmove(void *d, const void *s, size_t n) { u8 *dd = d; const u8 *ss = s; if (dd < ss) { while (n--) *dd++ = *ss++; } else { dd += n; ss += n; while (n--) *--dd = *--ss; } return d; }
void *e_memset(void *d, int c, size_t n) { u8 *dd = d; while (n--) *dd++ = (u8)c; return d; }
int e_memcmp(const void *a, const void *b, size_t n) { const u8 *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
char *e_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
char *e_strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }

#ifdef __wasm__
/* the compiler may emit calls to these even in freestanding mode */
void *memcpy(void *d, const void *s, size_t n) { return e_memcpy(d, s, n); }
void *memset(void *d, int c, size_t n) { return e_memset(d, c, n); }
void *memmove(void *d, const void *s, size_t n) { return e_memmove(d, s, n); }
int memcmp(const void *a, const void *b, size_t n) { return e_memcmp(a, b, n); }
size_t strlen(const char *s) { return e_strlen(s); }
#endif

/* ---------- math ---------- */
double e_sqrt(double x) { return __builtin_sqrt(x); }
double e_floor(double x) { return __builtin_floor(x); }
double e_ceil(double x) { return __builtin_ceil(x); }
double e_fabs(double x) { return __builtin_fabs(x); }
double e_fmod(double a, double b) { if (b == 0) return 0.0 / 0.0; double q = __builtin_trunc(a / b); return a - q * b; }

/* ---------- number parsing ---------- */
long long e_strtoll(const char *s, char **end, int base) {
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
  if (base == 0) base = (s[0] == '0') ? 8 : 10;
  long long v = 0;
  for (;;) {
    int c = *s, d;
    if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'z') d = c - 'a' + 10; else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10; else break;
    if (d >= base) break;
    v = v * base + d; s++;
  }
  if (end) *end = (char *)s;
  return neg ? -v : v;
}
double e_strtod(const char *s, char **end) {
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  double v = 0; int any = 0;
  while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
  if (*s == '.') { s++; double f = 0.1; while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; s++; any = 1; } }
  if (any && (*s == 'e' || *s == 'E')) {
    const char *t = s + 1; int eneg = 0; if (*t == '-') { eneg = 1; t++; } else if (*t == '+') t++;
    if (*t >= '0' && *t <= '9') { int ex = 0; while (*t >= '0' && *t <= '9') { ex = ex * 10 + (*t - '0'); t++; } double p = 1; while (ex--) p *= 10; v = eneg ? v / p : v * p; s = t; }
  }
  if (end) *end = (char *)(any ? s : s);
  return neg ? -v : v;
}

/* ---------- formatting (printf subset: %d %i %u %x %X %o %c %s %p %f %e %g %% with flags/width/precision/length) ---------- */
typedef struct { char *buf; size_t n, pos; } Out;
static void put(Out *o, char c) { if (o->pos + 1 < o->n) o->buf[o->pos] = c; o->pos++; }
static void puts_pad(Out *o, const char *s, size_t len, int width, int left, char padc) {
  int pad = width > (int)len ? width - (int)len : 0;
  if (!left) while (pad-- > 0) put(o, padc);
  for (size_t i = 0; i < len; i++) put(o, s[i]);
  if (left) while (pad-- > 0) put(o, ' ');
}
static size_t utoa_base(unsigned long long v, char *tmp, int base, int upper) {
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char rev[32]; size_t n = 0;
  do { rev[n++] = dig[v % base]; v /= base; } while (v);
  for (size_t i = 0; i < n; i++) tmp[i] = rev[n - 1 - i];
  return n;
}
/* fixed/exp formatting without libc */
static size_t fmt_double(double v, char *tmp, int prec, char conv, int alt) {
  size_t n = 0;
  if (v != v) { e_memcpy(tmp, "nan", 3); return 3; }
  if (v < 0) { tmp[n++] = '-'; v = -v; }
  if (v > 1.7976931348623157e308) { e_memcpy(tmp + n, "inf", 3); return n + 3; }
  if (prec < 0) prec = 6;
  if (conv == 'g' || conv == 'G') {
    if (prec == 0) prec = 1;
    int ex = 0; double t = v;
    if (t != 0) { while (t >= 10) { t /= 10; ex++; } while (t < 1) { t *= 10; ex--; } }
    if (ex < -4 || ex >= prec) { conv = 'e'; prec = prec - 1; }
    else { conv = 'f'; prec = prec - 1 - ex; if (prec < 0) prec = 0; }
    /* strip trailing zeros afterwards */
    size_t m = fmt_double(v, tmp + n, prec, conv, alt) + n;
    if (!alt) {
      /* remove trailing zeros in the mantissa */
      size_t e = m; char *epos = 0;
      for (size_t i = n; i < m; i++) if (tmp[i] == 'e' || tmp[i] == 'E') { epos = tmp + i; break; }
      size_t mant_end = epos ? (size_t)(epos - tmp) : m;
      int has_dot = 0; for (size_t i = n; i < mant_end; i++) if (tmp[i] == '.') has_dot = 1;
      if (has_dot) {
        size_t j = mant_end; while (j > n && tmp[j - 1] == '0') j--; if (j > n && tmp[j - 1] == '.') j--;
        if (epos) { size_t tail = m - mant_end; e_memmove(tmp + j, epos, tail); e = j + tail; } else e = j;
        m = e;
      }
    }
    return m;
  }
  if (conv == 'e' || conv == 'E') {
    int ex = 0;
    if (v != 0) { while (v >= 10) { v /= 10; ex++; } while (v < 1) { v *= 10; ex--; } }
    /* round */
    double p = 1; for (int i = 0; i < prec; i++) p *= 10;
    double r = (double)(unsigned long long)(v * p + 0.5) / p;
    if (r >= 10) { r /= 10; ex++; }
    n += fmt_double(r, tmp + n, prec, 'f', alt);
    tmp[n++] = conv; tmp[n++] = ex < 0 ? '-' : '+'; if (ex < 0) ex = -ex;
    if (ex < 10) tmp[n++] = '0';
    n += utoa_base((unsigned)ex, tmp + n, 10, 0);
    return n;
  }
  /* %f */
  if (prec > 17) prec = 17;
  double p = 1; for (int i = 0; i < prec; i++) p *= 10;
  double scaled = v * p + 0.5;
  if (scaled < 1.8e19) {
    unsigned long long whole = (unsigned long long)scaled;
    unsigned long long ip = whole / (unsigned long long)p, fp = whole % (unsigned long long)p;
    n += utoa_base(ip, tmp + n, 10, 0);
    if (prec > 0 || alt) tmp[n++] = '.';
    if (prec > 0) { char f[32]; size_t fl = utoa_base(fp, f, 10, 0); for (int i = 0; i < prec - (int)fl; i++) tmp[n++] = '0'; e_memcpy(tmp + n, f, fl); n += fl; }
  } else {
    /* huge: print integer part digit by digit */
    double ip = __builtin_floor(v); char rev[400]; size_t k = 0;
    while (ip >= 1 && k < 399) { double q = __builtin_floor(ip / 10); rev[k++] = (char)('0' + (int)(ip - q * 10)); ip = q; }
    while (k) tmp[n++] = rev[--k];
    if (prec > 0) { tmp[n++] = '.'; for (int i = 0; i < prec; i++) tmp[n++] = '0'; }
  }
  return n;
}

int e_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
  Out o = { buf, n, 0 };
  for (const char *f = fmt; *f; f++) {
    if (*f != '%') { put(&o, *f); continue; }
    f++;
    int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = 0, prec = -1, len = 0;
    for (;; f++) { if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else if (*f == '0') zero = 1; else break; }
    if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; } else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
    while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'I' || *f == 'q') {
      if (*f == 'l') len++; else if (*f == 'h') len--; else if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'q') len = 2;
      else if (*f == 'I') { if (f[1] == '6' && f[2] == '4') { len = 2; f += 2; } else if (f[1] == '3' && f[2] == '2') { len = 0; f += 2; } else len = 2; }
      f++;
    }
    char tmp[512]; size_t tl = 0; char padc = zero && !left ? '0' : ' ';
    switch (*f) {
      case '%': put(&o, '%'); break;
      case 'c': { tmp[0] = (char)va_arg(ap, int); puts_pad(&o, tmp, 1, width, left, ' '); break; }
      case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; size_t sl = e_strlen(s); if (prec >= 0 && (size_t)prec < sl) sl = prec; puts_pad(&o, s, sl, width, left, ' '); break; }
      case 'd': case 'i': {
        long long v = len >= 2 ? va_arg(ap, long long) : len == 1 ? va_arg(ap, long) : va_arg(ap, int);
        if (len == -1) v = (short)v; else if (len == -2) v = (signed char)v;
        char sign = 0; unsigned long long uv = v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
        if (v < 0) sign = '-'; else if (plus) sign = '+'; else if (space) sign = ' ';
        char d[32]; size_t dl = utoa_base(uv, d, 10, 0);
        if (prec >= 0) { padc = ' '; while ((int)dl < prec) { e_memmove(d + 1, d, dl); d[0] = '0'; dl++; } }
        if (sign) tmp[tl++] = sign;
        if (padc == '0') { int pad = width - (int)(dl + tl); while (pad-- > 0) tmp[tl++] = '0'; }
        e_memcpy(tmp + tl, d, dl); tl += dl; puts_pad(&o, tmp, tl, width, left, ' '); break;
      }
      case 'u': case 'x': case 'X': case 'o': case 'p': {
        unsigned long long v; int base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
        if (*f == 'p') { v = (unsigned long long)(uintptr_t)va_arg(ap, void *); len = 2; }
        else v = len >= 2 ? va_arg(ap, unsigned long long) : len == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
        if (len == -1) v = (unsigned short)v; else if (len == -2) v = (unsigned char)v;
        char d[32]; size_t dl = utoa_base(v, d, base, *f == 'X');
        if (prec >= 0) { padc = ' '; while ((int)dl < prec) { e_memmove(d + 1, d, dl); d[0] = '0'; dl++; } }
        if (alt && base == 16 && v) { tmp[tl++] = '0'; tmp[tl++] = *f == 'X' ? 'X' : 'x'; }
        if (padc == '0') { int pad = width - (int)(dl + tl); while (pad-- > 0) tmp[tl++] = '0'; }
        e_memcpy(tmp + tl, d, dl); tl += dl; puts_pad(&o, tmp, tl, width, left, ' '); break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        double v = va_arg(ap, double);
        char d[480]; size_t dl = fmt_double(v, d, prec, *f == 'F' ? 'f' : *f, alt);
        if (d[0] != '-' && (plus || space)) { tmp[tl++] = plus ? '+' : ' '; }
        if (padc == '0') { size_t sgn = d[0] == '-'; if (sgn) tmp[tl++] = '-'; int pad = width - (int)(dl + tl); while (pad-- > 0) tmp[tl++] = '0'; e_memcpy(tmp + tl, d + sgn, dl - sgn); tl += dl - sgn; }
        else { e_memcpy(tmp + tl, d, dl); tl += dl; }
        puts_pad(&o, tmp, tl, width, left, ' '); break;
      }
      case 'n': { int *p = va_arg(ap, int *); if (p) *p = (int)o.pos; break; }
      default: put(&o, '%'); if (*f) put(&o, *f); else f--; break;
    }
  }
  if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
  return (int)o.pos;
}
int e_snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = e_vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
void e_log(const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); int n = e_vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (n > (int)sizeof b - 1) n = sizeof b - 1; host_write(3, b, n); }

/* ---------- guest memory ---------- */
/* exited processes return their regions to a pool keyed by size (the arena itself never shrinks) */
#define POOL_MAX 128
static struct { u8 *host; u64 size; } g_pool[POOL_MAX]; static int g_npool;
static u8 *pool_take(u64 size) { for (int i = 0; i < g_npool; i++) if (g_pool[i].size == size) { u8 *h = g_pool[i].host; g_pool[i] = g_pool[--g_npool]; e_memset(h, 0, (size_t)size); return h; } return 0; }
u32 g_mem_gen;
void mem_release(Mem *m) { for (int i = 0; i < m->n; i++) { if (g_npool < POOL_MAX) { g_pool[g_npool].host = m->r[i].host; g_pool[g_npool].size = m->r[i].size; g_npool++; } } m->n = 0; m->last = 0; mem_tlb_flush(m); }
u8 *mem_map(Mem *m, u64 base, u64 size, const char *name) {
  if (m->n >= MAX_REGIONS) return 0;
  size = (size + 0xfff) & ~0xfffull;
  u8 *h = pool_take(size); if (!h) h = emu_alloc((size_t)size);
  if (!h) return 0;
  Region *r = &m->r[m->n++];
  r->base = base; r->size = size; r->host = h; r->name = name; r->prot = 7;
  return h;
}
/* drop a region created by mem_map (VirtualFree MEM_RELEASE of a large reservation); the host memory goes back to the pool (wasm) or the OS (native) */
int mem_unmap(Mem *m, u64 base) {
  for (int i = 0; i < m->n; i++) if (m->r[i].base == base) {
    u8 *h = m->r[i].host; u64 size = m->r[i].size;
    if (!emu_free(h) && g_npool < POOL_MAX) { g_pool[g_npool].host = h; g_pool[g_npool].size = size; g_npool++; }
    for (int j = i + 1; j < m->n; j++) m->r[j - 1] = m->r[j]; m->n--; m->last = 0; mem_tlb_flush(m); return 1; }
  return 0;
}
Region *mem_region(Mem *m, u64 addr) {
  for (int i = 0; i < m->n; i++) if (addr - m->r[i].base < m->r[i].size) return &m->r[i];
  return 0;
}
u8 *mem_ptr(Mem *m, u64 addr, u64 size, int write) {
#ifdef X64_WWATCH
  { extern u64 x64_rip(void); if (write && addr <= (u64)X64_WWATCH && (u64)X64_WWATCH < addr + size) e_log("[wwatch] write %llx+%llu at rip=%llx\n", (unsigned long long)addr, (unsigned long long)size, (unsigned long long)x64_rip()); }
#endif
  Region *r = &m->r[m->last];
  if (m->n && addr - r->base < r->size && addr + size - r->base <= r->size) goto hit;
  for (int i = 0; i < m->n; i++) {
    r = &m->r[i];
    if (addr - r->base < r->size && addr + size - r->base <= r->size) { m->last = i; goto hit; }
  }
  m->fault_addr = addr; m->fault_write = write;
  return 0;
hit:;
  u64 pg = addr >> 12, pb = pg << 12;   /* remember the page if it lies fully inside the region */
  if (pb >= r->base && pb + 4096 <= r->base + r->size) { Tlb *t = &m->tlb[pg & (TLB_N - 1)]; t->tag = pg + 1; t->host = r->host + (pb - r->base); }
  return r->host + (addr - r->base);
}

int e_strnicmp(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) { int x = (u8)a[i], y = (u8)b[i]; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return x - y; if (!x) return 0; } return 0; }
