/* difftest.c — differential test of the x86-64 interpreter against the host CPU.
 *
 * Reads an objdump listing (intel syntax, --insn-width=16) of a real binary, picks data/ALU/SSE instructions that have no
 * control-flow or stack side effects, and runs each one with random register values both natively (through a generated
 * trampoline on an RWX page) and in the emulator (cpu_run for one instruction). Registers, flags, XMM registers and a
 * 64 KB scratch window (memory operands are pointed into it) must match.
 *
 *   cc -O1 -o /tmp/difftest emu/tests/difftest.c emu/x64/{lib,cpu,pe,win32,gdi,vfs,api,host_native}.c -lm
 *   objdump -d -M intel --insn-width=16 node.exe > /tmp/node.lst
 *   /tmp/difftest /tmp/node.lst [max_instructions] [trials]
 *
 * Linux x86-64 only (needs the host CPU). Instructions the emulator rejects are reported as UNSUP, grouped by mnemonic. */
#define _GNU_SOURCE
#include "../x64/emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <ctype.h>

#define BUF_BASE  0x300000000ull
#define BUF_SIZE  0x100000ull
#define WIN_OFF   0x80000ull            /* memory operands point here */
#define WIN_HALF  0x8000ull             /* compared window: ±32 KB */
#define CODE_BASE 0x400000ull
#define ARENA_BASE 0x400000000ull
#define ARENA_SIZE 0x20000000ull        /* 512 MB RWX: rip-relative targets of the trampoline (placed in the middle) land here */
#define TRAMP_OFF  0x10000000ull

typedef struct { u64 r[16]; u64 flags; Xmm x[16]; } Ctx;      /* shared layout for the trampoline */
static u8 *g_hbuf;            /* host scratch (also the guest address: identity mapping for the test) */
static u8 *g_ebuf;            /* emulator's copy of the scratch */
static Mem g_mem; static Cpu g_cpu;
static u8 *g_code;            /* emulator code page */
static sigjmp_buf g_jmp; static volatile int g_sig;
static void on_sig(int s) { g_sig = s; siglongjmp(g_jmp, 1); }

static const char *R64[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
static const char *R32[16] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d" };
static int reg_index(const char *t, int n) { char b[8]; if (n <= 0 || n > 4) return -1; memcpy(b, t, (size_t)n); b[n] = 0; for (int i = 0; i < 16; i++) if (!strcmp(b, R64[i]) || !strcmp(b, R32[i])) return i; return -1; }

/* ---- host trampoline: load Ctx → run instruction → store Ctx ---- */
static long g_riprel_tested, g_strop_tested; static u8 *g_tramp; static int g_tramp_insn_off; static u8 g_hpage_before[0x2000];
static int build_tramp(const u8 *insn, int len, Ctx *ctx) {
  u8 *p = g_tramp;
  #define EMIT(...) do { static const u8 b_[] = { __VA_ARGS__ }; memcpy(p, b_, sizeof b_); p += sizeof b_; } while (0)
  #define MOVABS_RAX(v) do { *p++ = 0x48; *p++ = 0xB8; u64 v_ = (u64)(v); memcpy(p, &v_, 8); p += 8; } while (0)
  EMIT(0x53, 0x55, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57);                 /* push rbx rbp r12-r15 */
  MOVABS_RAX(ctx);
  EMIT(0xFF, 0xB0, 0x80, 0x00, 0x00, 0x00);                                      /* push [rax+0x80] (flags) */
  EMIT(0x9D);                                                                    /* popfq */
  for (int i = 0; i < 16; i++) {                                                 /* movdqu xmmI, [rax + 0x88 + 16 i] */
    u32 off = 0x88 + 16 * (u32)i; *p++ = 0xF3; if (i >= 8) *p++ = 0x44; *p++ = 0x0F; *p++ = 0x6F; *p++ = (u8)(0x80 | ((i & 7) << 3)); memcpy(p, &off, 4); p += 4; }
  for (int i = 1; i < 16; i++) { if (i == RSP) continue;                        /* mov rI, [rax + 8 i] */
    *p++ = (u8)(0x48 | (i >= 8 ? 4 : 0)); *p++ = 0x8B; *p++ = (u8)(0x80 | ((i & 7) << 3)); u32 off = 8 * (u32)i; memcpy(p, &off, 4); p += 4; }
  EMIT(0x48, 0x8B, 0x00);                                                        /* mov rax, [rax] */
  g_tramp_insn_off = (int)(p - g_tramp);
  memcpy(p, insn, (size_t)len); p += len;                                        /* the instruction under test */
  EMIT(0x50);                                                                    /* push rax */
  MOVABS_RAX(ctx);
  EMIT(0x9C);                                                                    /* pushfq */
  EMIT(0x8F, 0x80, 0x80, 0x00, 0x00, 0x00);                                      /* pop [rax+0x80] */
  EMIT(0xFC);                                                                    /* cld (DF may have been set for string ops) */
  for (int i = 1; i < 16; i++) { if (i == RSP) continue;                        /* mov [rax + 8 i], rI */
    *p++ = (u8)(0x48 | (i >= 8 ? 4 : 0)); *p++ = 0x89; *p++ = (u8)(0x80 | ((i & 7) << 3)); u32 off = 8 * (u32)i; memcpy(p, &off, 4); p += 4; }
  for (int i = 0; i < 16; i++) {                                                 /* movdqu [rax + 0x88 + 16 i], xmmI */
    u32 off = 0x88 + 16 * (u32)i; *p++ = 0xF3; if (i >= 8) *p++ = 0x44; *p++ = 0x0F; *p++ = 0x7F; *p++ = (u8)(0x80 | ((i & 7) << 3)); memcpy(p, &off, 4); p += 4; }
  EMIT(0x59);                                                                    /* pop rcx (original rax) */
  EMIT(0x48, 0x89, 0x08);                                                        /* mov [rax], rcx */
  EMIT(0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C, 0x5D, 0x5B, 0xC3);           /* pop r15 r14 r13 r12 rbp rbx; ret */
  return (int)(p - g_tramp);
}

static u64 rnd64(void) { u64 v = (u64)rand() << 42 ^ (u64)rand() << 21 ^ (u64)rand(); switch (rand() % 6) { case 0: return 0; case 1: return ~0ull; case 2: return v & 0xff; case 3: return (u64)(s64)(s32)v; default: return v; } }
static double rndf(void) { switch (rand() % 8) { case 0: return 0.0; case 1: return -1.5; case 2: return 1e300; case 3: return (double)(rand() - RAND_MAX / 2); case 4: return 0.0 / 0.0; case 5: return 1.0 / 0.0; default: { u64 v = rnd64(); double d; memcpy(&d, &v, 8); return d; } } }

#define FLAGS_CMP (F_CF | F_PF | F_ZF | F_SF | F_OF)

typedef struct { char mnem[24]; int count, fail, unsup, skipped; } Stat;
static Stat g_stats[1024]; static int g_nstats;
static Stat *stat_of(const char *m) { for (int i = 0; i < g_nstats; i++) if (!strcmp(g_stats[i].mnem, m)) return &g_stats[i]; if (g_nstats == 1024) return &g_stats[1023]; Stat *s = &g_stats[g_nstats++]; snprintf(s->mnem, sizeof s->mnem, "%s", m); return s; }

static int flags_relevant(const char *m, u64 *mask) {
  *mask = FLAGS_CMP;
  if (!strncmp(m, "mul", 3) || !strncmp(m, "imul", 4)) { *mask = F_CF | F_OF; return 1; }
  if (!strncmp(m, "div", 3) || !strncmp(m, "idiv", 4)) { *mask = 0; return 1; }
  if (!strcmp(m, "bsf") || !strcmp(m, "bsr")) { *mask = F_ZF; return 1; }
  if (!strncmp(m, "sh", 2) || !strncmp(m, "sa", 2) || !strncmp(m, "ro", 2) || !strncmp(m, "rc", 2)) { *mask = 0; return 1; }   /* shifts/rotates: OF/AF undefined for count>1, nothing for count 0 — flags checked separately by count below */
  if (!strncmp(m, "bt", 2)) { *mask = F_CF; return 1; }
  if (!strncmp(m, "cvt", 3) || !strncmp(m, "mov", 3) || !strncmp(m, "lea", 3) || !strncmp(m, "cmov", 4) || !strncmp(m, "set", 3) || m[0] == 'p' || !strncmp(m, "un", 2) || !strncmp(m, "sqrt", 4) || !strncmp(m, "add", 3) || !strncmp(m, "sub", 3) || !strncmp(m, "mul", 3) || !strncmp(m, "max", 3) || !strncmp(m, "min", 3) || !strncmp(m, "and", 3) || !strncmp(m, "or", 2) || !strncmp(m, "xor", 3)) {
    /* SSE arithmetic doesn't touch flags; integer add/sub/and/or/xor does */
    if (strstr(m, "sd") || strstr(m, "ss") || strstr(m, "pd") || strstr(m, "ps") || m[0] == 'p') { *mask = FLAGS_CMP; return 1; }   /* must leave flags untouched (ucomis/comis/ptest define all of them) */
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: difftest listing.lst [max] [trials]\n"); return 2; }
  int max = argc > 2 ? atoi(argv[2]) : 50000, trials = argc > 3 ? atoi(argv[3]) : 3; srand(12345);
  g_hbuf = mmap((void *)BUF_BASE, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (g_hbuf != (void *)BUF_BASE) { perror("mmap scratch"); return 2; }
  u8 *arena = mmap((void *)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0); if (arena != (void *)ARENA_BASE) { perror("mmap arena"); return 2; }
  g_tramp = arena + TRAMP_OFF;
  g_ebuf = mem_map(&g_mem, BUF_BASE, BUF_SIZE, "scratch"); g_code = mem_map(&g_mem, CODE_BASE, 0x1000, "code");
  g_cpu.mem = &g_mem; g_cpu.w = 0;
  signal(SIGSEGV, on_sig); signal(SIGTRAP, on_sig); signal(SIGILL, on_sig); signal(SIGBUS, on_sig); signal(SIGFPE, on_sig);
  static Ctx base, hc, ec; (void)base;
  FILE *f = fopen(argv[1], "r"); if (!f) { perror(argv[1]); return 2; }
  char line[1024]; int tested = 0, failed = 0, unsup = 0, skipped = 0, seen = 0;
  /* dedupe on the instruction bytes */
  enum { HSZ = 1 << 20 }; static u64 *hset; hset = calloc(HSZ, 8);
  while (fgets(line, sizeof line, f)) {
    char *tab = strchr(line, '\t'); if (!tab) continue; char *tab2 = strchr(tab + 1, '\t'); if (!tab2) continue;
    /* bytes between tab and tab2 */
    u8 insn[16]; int len = 0; for (char *p = tab + 1; p < tab2 && len < 16; ) { while (*p == ' ') p++; if (p >= tab2 || !isxdigit((u8)p[0]) || !isxdigit((u8)p[1])) break; insn[len++] = (u8)strtoul((char[]){ p[0], p[1], 0 }, 0, 16); p += 2; }
    if (!len) continue;
    char *text = tab2 + 1; char *nl = strchr(text, '\n'); if (nl) *nl = 0; char *hash = strchr(text, '#'); if (hash) *hash = 0;
    char mnem[24]; int ml = 0; char *t = text; while (*t == ' ') t++;
    /* skip prefixes objdump prints as separate words */
    for (;;) { if (!strncmp(t, "lock ", 5)) t += 5; else if (!strncmp(t, "rep ", 4)) t += 4; else if (!strncmp(t, "repz ", 5)) t += 5; else if (!strncmp(t, "repnz ", 6)) t += 6; else if (!strncmp(t, "data16 ", 7)) t += 7; else if (!strncmp(t, "rex", 3)) { while (*t && *t != ' ') t++; while (*t == ' ') t++; } else if (!strncmp(t, "cs ", 3) || !strncmp(t, "ds ", 3) || !strncmp(t, "ss ", 3) || !strncmp(t, "es ", 3)) t += 3; else if (!strncmp(t, "addr32 ", 7)) { skipped++; goto next; } else break; }
    while (*t && *t != ' ' && ml < 23) mnem[ml++] = *t++; mnem[ml] = 0; while (*t == ' ') t++;
    seen++;
    /* filter: no control flow, stack, system, string, x87, AVX, segment-relative, rip-relative, rsp-relative */
    if (!ml || mnem[0] == 'j' || !strcmp(mnem, "call") || !strcmp(mnem, "ret") || !strncmp(mnem, "push", 4) || !strncmp(mnem, "pop", 3) || !strcmp(mnem, "leave") || !strcmp(mnem, "int3") || !strcmp(mnem, "int") || !strcmp(mnem, "nop") || !strcmp(mnem, "hlt") || !strcmp(mnem, "ud2") || !strcmp(mnem, "(bad)") || !strcmp(mnem, "syscall") || !strcmp(mnem, "cpuid") || !strcmp(mnem, "rdtsc") || !strcmp(mnem, "rdtscp") || !strcmp(mnem, "xgetbv") || !strcmp(mnem, "pause") || !strcmp(mnem, "mfence") || !strcmp(mnem, "lfence") || !strcmp(mnem, "sfence") || !strncmp(mnem, "prefetch", 8) || !strcmp(mnem, "clflush") || mnem[0] == 'v' || mnem[0] == 'k' || mnem[0] == 'f' || !strcmp(mnem, "in") || !strcmp(mnem, "out") || !strncmp(mnem, "ins", 3) || !strncmp(mnem, "outs", 4) || !strcmp(mnem, "loop") || !strcmp(mnem, "jrcxz") || !strcmp(mnem, "enter") || !strncmp(mnem, "ldmxcsr", 7) || !strncmp(mnem, "stmxcsr", 7) || !strcmp(mnem, "cld") || !strcmp(mnem, "std") || !strcmp(mnem, "pushf") || !strcmp(mnem, "popf") || !strcmp(mnem, "xstore-rng") || !strcmp(mnem, "rdrand") || !strcmp(mnem, "rdseed") || !strncmp(mnem, "sha", 3) || !strncmp(mnem, "aes", 3) || !strncmp(mnem, "pclmul", 6) || !strcmp(mnem, "endbr64") || !strncmp(mnem, "movnt", 5) || !strcmp(mnem, "movbe") || !strncmp(mnem, "pcmp.str", 4) || !strncmp(mnem, "pcmpestr", 8) || !strncmp(mnem, "pcmpistr", 8) || !strcmp(mnem, "cmc") || !strcmp(mnem, "stc") || !strcmp(mnem, "clc")) { skipped++; stat_of(mnem)->skipped++; continue; }
    int riprel = strstr(t, "rip") != 0;
    if (strstr(t, "fs:") || strstr(t, "gs:") || strstr(t, "rsp") || strstr(t, "esp") || strstr(t, "spl")) { skipped++; stat_of(mnem)->skipped++; continue; }
    /* memory operand: base/index registers, displacement range */
    int base_r = -1, idx_r = -1; long disp = 0; char *br = strchr(t, '[');
    if (br && riprel) { char *pm = br + 4; if (*pm == '+' || *pm == '-') { long d = strtol(pm + 1, 0, 16); disp = *pm == '-' ? -d : d; } if (disp > (long)(ARENA_SIZE - TRAMP_OFF - 0x10000) || disp < -(long)(TRAMP_OFF - 0x10000) || (disp < 0x3000 && disp > -0x3000)) { skipped++; stat_of(mnem)->skipped++; continue; } }
    else if (br) { char *q = br + 1; int n = 0; while (isalnum((u8)q[n])) n++; base_r = reg_index(q, n); if (base_r < 0) { skipped++; stat_of(mnem)->skipped++; continue; }
      char *star = strchr(q, '*'); if (star && star < strchr(q, ']')) { char *s = star; while (s > q && isalnum((u8)s[-1])) s--; idx_r = reg_index(s, (int)(star - s)); if (idx_r == base_r) { skipped++; stat_of(mnem)->skipped++; continue; } }
      char *pm = q; while (*pm && *pm != ']' && *pm != '+' && *pm != '-') pm++; if (star) { pm = star + 1; while (*pm && *pm != ']' && *pm != '+' && *pm != '-') pm++; }
      if (*pm == '+' || *pm == '-') { long d = strtol(pm + 1, 0, 16); disp = *pm == '-' ? -d : d; if (strchr(pm + 1, '+') && strchr(pm + 1, '+') < strchr(q, ']')) { skipped++; stat_of(mnem)->skipped++; continue; } }
      if (disp > 0x6000 || disp < -0x6000) { skipped++; stat_of(mnem)->skipped++; continue; } }
    int strop = (!strncmp(mnem, "movs", 4) && (mnem[4] == 0 || mnem[5] == 0) && !strstr(t, "xmm")) || !strncmp(mnem, "stos", 4) || !strncmp(mnem, "cmps", 4) && !strstr(t, "xmm") || !strncmp(mnem, "scas", 4) || !strncmp(mnem, "lods", 4);
    if (!strcmp(mnem, "movsd") && strstr(t, "xmm")) strop = 0; if (!strcmp(mnem, "movsxd") || !strcmp(mnem, "movss") || !strcmp(mnem, "movsx") || !strcmp(mnem, "cmpsd") && strstr(t, "xmm") || !strcmp(mnem, "cmpss")) strop = 0;
    if (strop) { base_r = -2; }   /* rsi/rdi set below */
    /* dedupe */
    { u64 h = 1469598103934665603ull; for (int i = 0; i < len; i++) h = (h ^ insn[i]) * 1099511628211ull; u64 slot = h % HSZ; int dup = 0; while (hset[slot]) { if (hset[slot] == (h | 1)) { dup = 1; break; } slot = (slot + 1) % HSZ; } if (dup) continue; hset[slot] = h | 1; }
    if (tested >= max) break;
    Stat *st = stat_of(mnem);
    int tl = build_tramp(insn, len, &hc); (void)tl; int tl_pre = g_tramp_insn_off;
    for (int trial = 0; trial < trials; trial++) {
      /* identical random state for both */
      for (int i = 0; i < 16; i++) hc.r[i] = rnd64(); hc.r[RSP] = 0; hc.flags = 0x202 | (rand() & FLAGS_CMP);
      for (int i = 0; i < 16; i++) { hc.x[i].f64[0] = rndf(); hc.x[i].f64[1] = rndf(); if (rand() & 1) { hc.x[i].q[0] = rnd64(); hc.x[i].q[1] = rnd64(); } }
      if (base_r >= 0) hc.r[base_r] = BUF_BASE + WIN_OFF + (u64)(rand() % 64) * 8; if (idx_r >= 0) hc.r[idx_r] = (u64)(rand() % 8);
      if (base_r == -2) { hc.r[RSI] = BUF_BASE + WIN_OFF + (u64)(rand() % 64) * 8; hc.r[RDI] = BUF_BASE + WIN_OFF + 0x1000 + (u64)(rand() % 64) * 8; hc.r[RCX] = (u64)(rand() % 40); hc.flags &= ~(u64)F_DF; if (rand() % 4 == 0) { hc.flags |= F_DF; hc.r[RSI] += 0x800; hc.r[RDI] += 0x800; } }
      u64 rcx_in = hc.r[RCX];
      u64 insn_addr = (u64)(g_tramp + tl_pre), rip_target = 0;
      if (riprel) { rip_target = insn_addr + (u64)len + (u64)disp; /* emulator pages at the same addresses as the host */ }
      for (u64 i = 0; i < 2 * WIN_HALF; i += 8) { u64 v = rnd64(); memcpy(g_hbuf + WIN_OFF - WIN_HALF + i, &v, 8); }
      memcpy(g_ebuf + WIN_OFF - WIN_HALF, g_hbuf + WIN_OFF - WIN_HALF, 2 * WIN_HALF);
      ec = hc;
      /* host */
      if (riprel) { u64 pg = (rip_target - 32) & ~0xfffull; u64 cpg = insn_addr & ~0xfffull; if (pg - cpg < 0x2000 || cpg - pg < 0x2000) { /* data overlaps the trampoline's own page: only randomize bytes near the target */ for (int k = 0; k < 64; k++) ((u8 *)rip_target)[k - 32] = (u8)rand(); } else { for (int k = 0; k < 0x2000; k += 8) { u64 v = rnd64(); memcpy((u8 *)pg + k, &v, 8); } } memcpy(g_hpage_before, (u8 *)pg, 0x2000); }
      g_sig = 0; if (sigsetjmp(g_jmp, 1) == 0) { ((void (*)(void))g_tramp)(); } 
      if (g_sig) { st->skipped++; break; }   /* host faulted (bad address/opcode on this CPU): skip the instruction */
      /* emulator */
      cpu_reset(&g_cpu); memcpy(g_cpu.r, ec.r, sizeof ec.r); g_cpu.rflags = ec.flags; memcpy(g_cpu.x, ec.x, sizeof ec.x); g_cpu.rip = CODE_BASE;
      memcpy(g_code, insn, (size_t)len); memset(g_code + len, 0xCC, 16);
      u8 *epage = 0, *hpage = 0; u64 page = 0;
      if (riprel) {
        /* code page + data page(s) at the host addresses; the host page keeps its (random) contents from before the run — restore them for the emulator */
        while (g_mem.n > 2) { g_mem.n--; free(g_mem.r[g_mem.n].host); } g_mem.last = 0;
        u64 cpage = insn_addr & ~0xfffull; page = (rip_target - 32) & ~0xfffull;
        hpage = (u8 *)page;
        if (page - cpage > 0x2000 && cpage - page > 0x2000) {
          u8 *ec_ = mem_map(&g_mem, cpage, 0x2000, "ripcode"); if (!ec_) { st->skipped++; break; }
          memcpy(ec_, (u8 *)cpage, 0x2000);
          epage = mem_map(&g_mem, page, 0x2000, "ripdata"); if (!epage) { st->skipped++; break; } memcpy(epage, g_hpage_before, 0x2000);
        } else {
          u64 lo = page < cpage ? page : cpage;
          u8 *ec_ = mem_map(&g_mem, lo, 0x4000, "ripboth"); if (!ec_) { st->skipped++; break; }
          memcpy(ec_, (u8 *)lo, 0x4000); epage = ec_ + (page - lo); memcpy(epage, g_hpage_before, 0x2000);
        }
        g_cpu.rip = insn_addr;
      }
      int r = cpu_run(&g_cpu, 1);
      if (r == RUN_FAULT) { if (!st->unsup) printf("UNSUP %-12s %s %s  (%s)\n", mnem, t, "", g_cpu.err); st->unsup++; unsup++; break; }
      /* compare */
      u64 fmask; flags_relevant(mnem, &fmask);
      if (!strncmp(mnem, "sh", 2) || !strncmp(mnem, "sa", 2) || !strncmp(mnem, "ro", 2) || !strncmp(mnem, "rc", 2)) {
        int osz8 = strstr(t, "QWORD") || (strchr(t, 'r') && (t[0] == 'r')) ; const char *c2 = strrchr(t, ','); u64 cnt = 0; int known = 0;
        if (c2) { c2++; if (!strcmp(c2, "cl")) { cnt = rcx_in & 0xff; known = 1; } else if (!strncmp(c2, "0x", 2)) { cnt = strtoull(c2, 0, 16); known = 1; } else if (isdigit((u8)*c2)) { cnt = strtoull(c2, 0, 10); known = 1; } }
        else { cnt = 1; known = 1; }   /* "shl rax" = by 1 */
        cnt &= osz8 ? 63 : 31;
        if (!known) fmask = 0; else if (cnt == 0) fmask = 0; else if (!strncmp(mnem, "ro", 2) || !strncmp(mnem, "rc", 2)) fmask = cnt == 1 ? (F_CF | F_OF) : F_CF; else fmask = cnt == 1 ? FLAGS_CMP : (F_CF | F_PF | F_ZF | F_SF);
        if (!strncmp(mnem, "shld", 4) || !strncmp(mnem, "shrd", 4)) { if (cnt == 0) fmask = 0; else fmask = F_CF | F_PF | F_ZF | F_SF; }
      }
      if (strop && !strncmp(mnem, "movs", 4)) fmask = 0; if (strop && (!strncmp(mnem, "stos", 4) || !strncmp(mnem, "lods", 4))) fmask = 0;
      if (!strncmp(mnem, "xchg", 4)) fmask = 0;
      int bad = 0; char why[512] = "";
      for (int i = 0; i < 16; i++) if (i != RSP && g_cpu.r[i] != hc.r[i]) { bad = 1; snprintf(why + strlen(why), sizeof why - strlen(why), "%s: host %016llx emu %016llx; ", R64[i], (unsigned long long)hc.r[i], (unsigned long long)g_cpu.r[i]); }
      for (int i = 0; i < 16; i++) if (memcmp(&g_cpu.x[i], &hc.x[i], 16)) { bad = 1; snprintf(why + strlen(why), sizeof why - strlen(why), "xmm%d: host %016llx%016llx emu %016llx%016llx; ", i, (unsigned long long)hc.x[i].q[1], (unsigned long long)hc.x[i].q[0], (unsigned long long)g_cpu.x[i].q[1], (unsigned long long)g_cpu.x[i].q[0]); }
      if ((g_cpu.rflags & fmask) != (hc.flags & fmask)) { bad = 1; snprintf(why + strlen(why), sizeof why - strlen(why), "flags: host %03llx emu %03llx (mask %03llx); ", (unsigned long long)(hc.flags & 0xfff), (unsigned long long)(g_cpu.rflags & 0xfff), (unsigned long long)fmask); }
      if (riprel && epage && memcmp(epage, hpage, 0x2000)) { bad = 1; snprintf(why + strlen(why), sizeof why - strlen(why), "rip-relative target memory differs; "); }
      if (memcmp(g_ebuf + WIN_OFF - WIN_HALF, g_hbuf + WIN_OFF - WIN_HALF, 2 * WIN_HALF)) { bad = 1; u64 off = 0; for (; off < 2 * WIN_HALF; off++) if (g_ebuf[WIN_OFF - WIN_HALF + off] != g_hbuf[WIN_OFF - WIN_HALF + off]) break; snprintf(why + strlen(why), sizeof why - strlen(why), "memory differs at window+%llx (host %02x emu %02x); ", (unsigned long long)off, g_hbuf[WIN_OFF - WIN_HALF + off], g_ebuf[WIN_OFF - WIN_HALF + off]); }
      if (bad) { if (st->fail < 3) printf("FAIL  %-12s %s   [%s]\n      %s\n", mnem, t, (char[]){0}, why); st->fail++; failed++; break; }
    }
    st->count++; tested++; if (riprel) g_riprel_tested++; if (strop) g_strop_tested++;
  next: ;
  }
  printf("\nrip-relative tested: %ld, string ops tested: %ld", g_riprel_tested, g_strop_tested);
  printf("\n%d instructions in listing, %d unique tested, %d failed, %d unsupported, %d skipped\n", seen, tested, failed, unsup, skipped);
  printf("per mnemonic (fail/unsup/tested):\n");
  for (int i = 0; i < g_nstats; i++) if (g_stats[i].fail || g_stats[i].unsup) printf("  %-14s %d/%d/%d\n", g_stats[i].mnem, g_stats[i].fail, g_stats[i].unsup, g_stats[i].count);
  return failed || unsup ? 1 : 0;
}
