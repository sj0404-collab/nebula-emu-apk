/* x86-64 interpreter: integer ISA + SSE/SSE2 subset used by compiler-generated user-mode code. */
#include "emu.h"
u64 g_watch[8]; int g_watch_n;
u64 g_dwatch, g_dwatch_rip; u32 g_dwatch_last;   /* X64_DWATCH=addr: log every change of that dword with the instruction that did it */
static u32 watch_rd(Cpu *c, u64 a) { const u8 *p = mem_ptr_fast(c->mem, a, 4, 0); u32 v = 0; if (p) __builtin_memcpy(&v, p, 4); return v; }   /* side-effect-free peek for the watch log */   /* X64_WATCH=rip,rip,…: log registers whenever execution reaches one of these addresses (debug aid) */
#include <stdarg.h>

typedef unsigned __int128 u128; typedef __int128 s128;

/* ---------- faults ---------- */
void cpu_fault(Cpu *c, const char *fmt, ...) {
  if (c->status == RUN_FAULT) return;
  va_list ap; va_start(ap, fmt); e_vsnprintf(c->err, sizeof c->err, fmt, ap); va_end(ap);
  c->status = RUN_FAULT; c->err_rip = c->rip; c->exc_code = 0;
}
static void fault_av(Cpu *c, u64 a, int write) { cpu_fault(c, "%s fault at 0x%llx", write ? "write" : "read", (unsigned long long)a); c->exc_code = 0xC0000005u; c->exc_addr = a; c->exc_info = write ? 1 : 0; }
static void fault_code(Cpu *c, u32 code, const char *what) { cpu_fault(c, "%s", what); c->exc_code = code; c->exc_addr = 0; c->exc_info = 0; }

/* ---------- memory ---------- */
#define RD(T, name) T name(Cpu *c, u64 a) { u8 *p = mem_ptr_fast(c->mem, a, sizeof(T), 0); if (!p) { fault_av(c, a, 0); return 0; } T v; __builtin_memcpy(&v, p, sizeof v); return v; }
#define WR(T, name) void name(Cpu *c, u64 a, T v) { u8 *p = mem_ptr_fast(c->mem, a, sizeof(T), 1); if (!p) { fault_av(c, a, 1); return; } __builtin_memcpy(p, &v, sizeof v); }
RD(u8, rd8) RD(u16, rd16) RD(u32, rd32) RD(u64, rd64)
WR(u8, wr8) WR(u16, wr16) WR(u32, wr32) WR(u64, wr64)
const char *rd_str(Cpu *c, u64 a, size_t max) {
  if (!a) return 0;
  Region *r = mem_region(c->mem, a); if (!r) return 0;
  const char *s = (const char *)(r->host + (a - r->base)); size_t lim = (size_t)(r->size - (a - r->base));
  for (size_t i = 0; i < lim && i < max; i++) if (!s[i]) return s;
  return 0;
}
static u64 rdn(Cpu *c, u64 a, int n) { switch (n) { case 1: return rd8(c, a); case 2: return rd16(c, a); case 4: return rd32(c, a); default: return rd64(c, a); } }
static void wrn(Cpu *c, u64 a, int n, u64 v) { switch (n) { case 1: wr8(c, a, (u8)v); break; case 2: wr16(c, a, (u16)v); break; case 4: wr32(c, a, (u32)v); break; default: wr64(c, a, v); } }
void cpu_push(Cpu *c, u64 v) { if (c->mode32) { c->r[RSP] = (u32)(c->r[RSP] - 4); wr32(c, c->r[RSP], (u32)v); return; } c->r[RSP] -= 8; wr64(c, c->r[RSP], v); }
u64 cpu_pop(Cpu *c) { if (c->mode32) { u32 v = rd32(c, c->r[RSP]); c->r[RSP] = (u32)(c->r[RSP] + 4); return v; } u64 v = rd64(c, c->r[RSP]); c->r[RSP] += 8; return v; }

void cpu_reset(Cpu *c) {
  for (int i = 0; i < 16; i++) { c->r[i] = 0; c->x[i].q[0] = c->x[i].q[1] = 0; }
  c->rflags = 0x202; c->mxcsr = 0x1f80; c->fpu_cw = 0x27f; c->fsw = 0; c->ftop = 0; c->ftag = 0; for (int i = 0; i < 8; i++) c->fp[i] = 0; c->status = RUN_OK; c->icount = 0; c->err[0] = 0;
}

/* ---------- decoding state ---------- */
/* Decode cache: one entry per instruction address with everything the byte-level decode produces (prefixes, opcode,
 * ModRM/SIB fields, displacement, offsets) — nothing register-dependent.  Entries validate themselves against the
 * instruction bytes on every hit (two masked 8-byte compares), so JIT-written / overwritten code is handled without any
 * page tracking, and processes/threads can share the table (same bytes ⇒ same decode).  Direct-mapped. */
typedef struct {
  u64 rip, b0, b1, m0, m1;             /* address, bytes 0-7 / 8-15 (masked), masks for the instruction length */
  s32 disp;
  u8 len, hdr, mr_end, op;             /* length, offset after the first opcode byte, offset after ModRM/SIB/disp */
  u8 rex, rexw, rexr, rexx, rexb, p66, p67, rep, seg, lock;
  u8 modrm, mod, reg, rm, scale, riprel; s8 base, idx;   /* base/idx -1 = none */
  u8 fast, op2, m32;                   /* fast handler class (0 = generic path), second opcode byte for 0F forms, decoded in 32-bit mode */
  const u8 *code; Mem *mem; u32 gen;   /* host pointer to the bytes, valid while mem/gen match (saves the TLB lookup) */
} Dc;
enum { FK_NONE, FK_MOV_RM_R, FK_MOV_R_RM, FK_LEA, FK_ALU_RM_IMM8, FK_ALU_RM_IMM32, FK_TEST_RR, FK_ALU_RM_R, FK_ALU_R_RM, FK_JCC8, FK_JCC32, FK_JMP8, FK_JMP32,
       FK_CALL, FK_RET, FK_PUSH, FK_POP, FK_MOVZX8, FK_MOVZX16, FK_MOVSXD, FK_SHIFT_IMM, FK_FF, FK_MOV_R_IMM, FK_MOV_RM_IMM32, FK_ALU_A_IMM,
       FK_MOV8_RM_R, FK_MOV8_R_RM, FK_ALU8_RM_IMM, FK_ALU8_RM_R, FK_ALU8_R_RM, FK_ALU8_AL_IMM, FK_TEST8, FK_MOV8_RM_IMM, FK_SETCC, FK_CMOV, FK_IMUL_R_RM, FK_IMUL_IMM,
       FK_F7_TEST, FK_F7_NOTNEG, FK_CDQE, FK_CDQ, FK_SHIFT_1, FK_SHIFT_CL, FK_MOVSX8, FK_MOVSX16, FK_NOP, FK_RET_N, FK_TEST_RM_R, FK_SSE_MOV, FK_SSE_ARITH, FK_SSE_LOGIC, FK_SSE_CMP };
#define DC_BITS 15
static Dc g_dc[1 << DC_BITS];
#ifdef X64_OPHIST
static u64 g_ophist[512][4];   /* [op or 256+op2][0: reg form, 1: memory form, 2: rexw, 3: p66] */
void cpu_dump_ophist(void) { for (int pass = 0; pass < 40; pass++) { int best = -1; u64 bv = 0; for (int k = 0; k < 512; k++) { u64 t = g_ophist[k][0] + g_ophist[k][1]; if (t > bv) { bv = t; best = k; } } if (best < 0 || !bv) break;
    e_log("[ophist] %s%02x  total=%llu  reg=%llu mem=%llu rexw=%llu p66=%llu\n", best >= 256 ? "0f " : "", best & 255, (unsigned long long)bv, (unsigned long long)g_ophist[best][0], (unsigned long long)g_ophist[best][1], (unsigned long long)g_ophist[best][2], (unsigned long long)g_ophist[best][3]); g_ophist[best][0] = g_ophist[best][1] = 0; } }
#endif
#define DC_SLOT(rip) (&g_dc[((rip) ^ ((rip) >> DC_BITS)) & ((1 << DC_BITS) - 1)])

typedef struct {
  Cpu *c; u64 start, ip, fend;   /* fend: furthest byte fetched (instruction length for the cache; branches move ip) */
  int rex, rexw, rexr, rexx, rexb, p66, p67, rep, seg, lock;
  int mod, reg, rm, modrm; u64 ea; int ea_valid;
  const u8 *code;   /* direct pointer to 16 bytes at start (NULL → slow path) */
  const Dc *dc;     /* cache hit: decode_modrm takes its fields from here */
  int hdr, mr_end, riprel, scale; s8 base, idx; s32 disp;   /* recorded on a miss for the new entry */
} Ins;

static inline u8 fetch8(Ins *i) { if (i->code) { u64 off = i->ip - i->start; if (off < 16) { i->ip++; if (i->ip > i->fend) i->fend = i->ip; return i->code[off]; } } u8 v = rd8(i->c, i->ip++); if (i->ip > i->fend) i->fend = i->ip; return v; }
static inline u16 fetch16(Ins *i) { u16 v = rd16(i->c, i->ip); i->ip += 2; if (i->ip > i->fend) i->fend = i->ip; return v; }
static inline u32 fetch32(Ins *i) { if (i->code) { u64 off = i->ip - i->start; if (off + 4 <= 16) { u32 v; __builtin_memcpy(&v, i->code + off, 4); i->ip += 4; if (i->ip > i->fend) i->fend = i->ip; return v; } } u32 v = rd32(i->c, i->ip); i->ip += 4; if (i->ip > i->fend) i->fend = i->ip; return v; }
static inline u64 fetch64(Ins *i) { u64 v = rd64(i->c, i->ip); i->ip += 8; if (i->ip > i->fend) i->fend = i->ip; return v; }
static inline s64 fetch_s8(Ins *i) { return (s8)fetch8(i); }
static inline s64 fetch_s32(Ins *i) { return (s32)fetch32(i); }

static inline u64 mask(int sz) { return sz == 8 ? ~0ull : (1ull << (sz * 8)) - 1; }
static inline u64 sext(u64 v, int sz) { switch (sz) { case 1: return (u64)(s64)(s8)v; case 2: return (u64)(s64)(s16)v; case 4: return (u64)(s64)(s32)v; default: return v; } }

/* register access: byte regs 4..7 without REX are AH CH DH BH */
static inline u64 get_reg(Ins *i, int r, int sz) {
  if (sz == 1 && !i->rex && r >= 4 && r < 8) return (i->c->r[r - 4] >> 8) & 0xff;
  return i->c->r[r] & mask(sz);
}
static inline void set_reg(Ins *i, int r, int sz, u64 v) {
  Cpu *c = i->c;
  switch (sz) {
    case 1: if (!i->rex && r >= 4 && r < 8) c->r[r - 4] = (c->r[r - 4] & ~0xff00ull) | ((v & 0xff) << 8); else c->r[r] = (c->r[r] & ~0xffull) | (v & 0xff); break;
    case 2: c->r[r] = (c->r[r] & ~0xffffull) | (v & 0xffff); break;
    case 4: c->r[r] = v & 0xffffffffull; break;
    default: c->r[r] = v;
  }
}

static void decode_modrm(Ins *i) {
  if (i->dc) {   /* cache hit: fields decoded earlier, only the register-dependent address is computed */
    const Dc *e = i->dc; i->modrm = 1; i->mod = e->mod; i->reg = e->reg; i->rm = e->rm; i->ip = i->start + e->mr_end; i->ea_valid = 0;
    if (e->mod == 3) return;
    if (e->riprel) { i->ea = (u64)(s64)e->disp; i->ea_valid = 2; return; }
    u64 ea = (u64)(s64)e->disp; if (e->base >= 0) ea += i->c->r[(int)e->base]; if (e->idx >= 0) ea += i->c->r[(int)e->idx] << e->scale;
    if (i->p67 || i->c->mode32) ea &= 0xffffffffull;
    if (i->seg == 1) ea += i->c->fs_base; else if (i->seg == 2) ea += i->c->gs_base;
    i->ea = ea; i->ea_valid = 1; return;
  }
  u8 m = fetch8(i);
  i->modrm = 1; i->mod = m >> 6; i->reg = ((m >> 3) & 7) | (i->rexr << 3); i->rm = (m & 7) | (i->rexb << 3);
  i->ea_valid = 0; i->base = -1; i->idx = -1; i->scale = 0; i->disp = 0; i->riprel = 0;
  if (i->mod == 3) { i->mr_end = (int)(i->ip - i->start); return; }
  u64 ea = 0; int rmlow = m & 7;
  if (rmlow == 4) {
    u8 sib = fetch8(i); int scale = sib >> 6, idx = ((sib >> 3) & 7) | (i->rexx << 3), base = (sib & 7) | (i->rexb << 3);
    if (idx != 4) { ea += i->c->r[idx] << scale; i->idx = (s8)idx; i->scale = scale; }
    if ((sib & 7) == 5 && i->mod == 0) { s32 d = (s32)fetch32(i); ea += (u64)(s64)d; i->disp = d; }
    else { ea += i->c->r[base]; i->base = (s8)base; }
  } else if (rmlow == 5 && i->mod == 0 && i->c->mode32) { s32 d = (s32)fetch32(i); i->disp = d; ea = (u32)d; }   /* 32-bit mode: absolute disp32 */
  else if (rmlow == 5 && i->mod == 0) {
    s32 d = (s32)fetch32(i); i->disp = d; i->riprel = 1; i->mr_end = (int)(i->ip - i->start);   /* rip-relative: resolved after full decode (needs end ip) → store marker */
    i->ea_valid = 2; i->ea = (u64)(s64)d; return;
  } else { ea += i->c->r[i->rm]; i->base = (s8)i->rm; }
  if (i->mod == 1) { s32 d = (s8)fetch8(i); ea += (u64)(s64)d; i->disp = d; } else if (i->mod == 2) { s32 d = (s32)fetch32(i); ea += (u64)(s64)d; i->disp = d; }
  i->mr_end = (int)(i->ip - i->start);
  if (i->p67 || i->c->mode32) ea &= 0xffffffffull;
  if (i->seg == 1) ea += i->c->fs_base; else if (i->seg == 2) ea += i->c->gs_base;
  i->ea = ea; i->ea_valid = 1;
}
/* rip-relative displacement is relative to the END of the instruction: call after immediates are fetched */
static inline u64 EA(Ins *i) {
  if (i->ea_valid == 2) { i->ea = i->ip + i->ea; if (i->seg == 1) i->ea += i->c->fs_base; else if (i->seg == 2) i->ea += i->c->gs_base; i->ea_valid = 1; }
  return i->ea;
}
/* for rip-relative + immediate instructions we must know the immediate size before computing EA */
static inline void fix_rip_rel(Ins *i, int imm_bytes) { if (i->ea_valid == 2) { i->ea = i->ip + imm_bytes + i->ea; if (i->seg == 1) i->ea += i->c->fs_base; else if (i->seg == 2) i->ea += i->c->gs_base; i->ea_valid = 1; } }

static inline u64 rm_get(Ins *i, int sz) { return i->mod == 3 ? get_reg(i, i->rm, sz) : rdn(i->c, EA(i), sz); }
static inline void rm_set(Ins *i, int sz, u64 v) { if (i->mod == 3) set_reg(i, i->rm, sz, v); else wrn(i->c, EA(i), sz, v); }

/* ---------- flags ---------- */
static const u8 parity_tab[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
  P6(1), P6(0), P6(0), P6(1)
};
static inline void set_szp(Cpu *c, u64 res, int sz) {
  u64 m = mask(sz); res &= m;
  c->rflags &= ~(u64)(F_ZF | F_SF | F_PF);
  if (!res) c->rflags |= F_ZF;
  if (res >> (sz * 8 - 1)) c->rflags |= F_SF;
  if (parity_tab[res & 0xff]) c->rflags |= F_PF;
}
static inline void flags_add(Cpu *c, u64 a, u64 b, u64 res, int sz) {
  u64 m = mask(sz); a &= m; b &= m; res &= m; int sh = sz * 8 - 1;
  c->rflags &= ~(u64)(F_CF | F_OF | F_AF);
  if (res < a) c->rflags |= F_CF;
  if (((a ^ res) & (b ^ res)) >> sh & 1) c->rflags |= F_OF;
  if ((a ^ b ^ res) & 0x10) c->rflags |= F_AF;
  set_szp(c, res, sz);
}
static inline void flags_adc(Cpu *c, u64 a, u64 b, u64 cf, u64 res, int sz) {
  u64 m = mask(sz); a &= m; b &= m; res &= m; int sh = sz * 8 - 1;
  c->rflags &= ~(u64)(F_CF | F_OF | F_AF);
  if (res < a || (cf && res == a)) c->rflags |= F_CF;
  if (((a ^ res) & (b ^ res)) >> sh & 1) c->rflags |= F_OF;
  if ((a ^ b ^ res) & 0x10) c->rflags |= F_AF;
  set_szp(c, res, sz);
}
static inline void flags_sub(Cpu *c, u64 a, u64 b, u64 res, int sz) {
  u64 m = mask(sz); a &= m; b &= m; res &= m; int sh = sz * 8 - 1;
  c->rflags &= ~(u64)(F_CF | F_OF | F_AF);
  if (a < b) c->rflags |= F_CF;
  if (((a ^ b) & (a ^ res)) >> sh & 1) c->rflags |= F_OF;
  if ((a ^ b ^ res) & 0x10) c->rflags |= F_AF;
  set_szp(c, res, sz);
}
static inline void flags_sbb(Cpu *c, u64 a, u64 b, u64 cf, u64 res, int sz) {
  u64 m = mask(sz); a &= m; b &= m; res &= m; int sh = sz * 8 - 1;
  c->rflags &= ~(u64)(F_CF | F_OF | F_AF);
  if (a < b || (cf && a == b)) c->rflags |= F_CF;
  if (((a ^ b) & (a ^ res)) >> sh & 1) c->rflags |= F_OF;
  if ((a ^ b ^ res) & 0x10) c->rflags |= F_AF;
  set_szp(c, res, sz);
}
static inline void flags_logic(Cpu *c, u64 res, int sz) { c->rflags &= ~(u64)(F_CF | F_OF | F_AF); set_szp(c, res, sz); }

static int cond(Cpu *c, int cc) {
  u64 f = c->rflags; int r;
  switch (cc >> 1) {
    case 0: r = !!(f & F_OF); break;
    case 1: r = !!(f & F_CF); break;
    case 2: r = !!(f & F_ZF); break;
    case 3: r = !!(f & (F_CF | F_ZF)); break;
    case 4: r = !!(f & F_SF); break;
    case 5: r = !!(f & F_PF); break;
    case 6: r = !!(f & F_SF) != !!(f & F_OF); break;
    default: r = (f & F_ZF) || (!!(f & F_SF) != !!(f & F_OF)); break;
  }
  return (cc & 1) ? !r : r;
}

/* ---------- ALU ---------- */
static u64 alu(Cpu *c, int op, u64 a, u64 b, int sz) {
  u64 m = mask(sz), r;
  switch (op) {
    case 0: r = (a + b) & m; flags_add(c, a, b, r, sz); return r;                                  /* add */
    case 1: r = (a | b) & m; flags_logic(c, r, sz); return r;                                      /* or  */
    case 2: { u64 cf = c->rflags & F_CF; r = (a + b + cf) & m; flags_adc(c, a, b, cf, r, sz); return r; }  /* adc */
    case 3: { u64 cf = c->rflags & F_CF; r = (a - b - cf) & m; flags_sbb(c, a, b, cf, r, sz); return r; }  /* sbb */
    case 4: r = (a & b) & m; flags_logic(c, r, sz); return r;                                      /* and */
    case 5: r = (a - b) & m; flags_sub(c, a, b, r, sz); return r;                                  /* sub */
    case 6: r = (a ^ b) & m; flags_logic(c, r, sz); return r;                                      /* xor */
    default: r = (a - b) & m; flags_sub(c, a, b, r, sz); return a & m;                             /* cmp */
  }
}

static u64 shift(Cpu *c, int op, u64 v, int cnt, int sz) {
  int bits = sz * 8; u64 m = mask(sz); v &= m;
  cnt &= (sz == 8) ? 63 : 31;
  if (!cnt) return v;
  u64 r = v; int cf = (int)(c->rflags & F_CF);
  if (cnt >= bits && op >= 4) {   /* 8/16-bit shifts by >= width */
    if (op == 7) { int sgn = (int)(v >> (bits - 1)) & 1; r = sgn ? m : 0; c->rflags = (c->rflags & ~(u64)(F_CF | F_OF)) | (sgn ? F_CF : 0); set_szp(c, r, sz); return r; }
    int ncf = cnt == bits ? (op == 5 ? (int)(v >> (bits - 1)) & 1 : (int)(v & 1)) : 0;
    c->rflags = (c->rflags & ~(u64)(F_CF | F_OF)) | (ncf ? F_CF : 0); set_szp(c, 0, sz); return 0;
  }
  switch (op) {
    case 0: { cnt %= bits; if (!cnt) { return v; } r = ((v << cnt) | (v >> (bits - cnt))) & m; cf = r & 1; c->rflags &= ~(u64)(F_CF | F_OF); if (cf) c->rflags |= F_CF; if (((r >> (bits - 1)) ^ cf) & 1) c->rflags |= F_OF; return r; }               /* rol */
    case 1: { cnt %= bits; if (!cnt) { return v; } r = ((v >> cnt) | (v << (bits - cnt))) & m; cf = (int)(r >> (bits - 1)) & 1; c->rflags &= ~(u64)(F_CF | F_OF); if (cf) c->rflags |= F_CF; if (((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1) c->rflags |= F_OF; return r; } /* ror */
    case 2: { cnt %= bits + 1; for (int k = 0; k < cnt; k++) { int nc = (int)(r >> (bits - 1)) & 1; r = ((r << 1) | (u64)cf) & m; cf = nc; } c->rflags &= ~(u64)(F_CF | F_OF); if (cf) c->rflags |= F_CF; if (((r >> (bits - 1)) ^ cf) & 1) c->rflags |= F_OF; return r; }   /* rcl */
    case 3: { cnt %= bits + 1; for (int k = 0; k < cnt; k++) { int nc = (int)(r & 1); r = (r >> 1) | ((u64)cf << (bits - 1)); cf = nc; } c->rflags &= ~(u64)(F_CF | F_OF); if (cf) c->rflags |= F_CF; if (((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1) c->rflags |= F_OF; return r; } /* rcr */
    case 4: case 6: { r = (v << cnt) & m; cf = (int)((v >> (bits - cnt)) & 1); c->rflags &= ~(u64)(F_CF | F_OF | F_AF); if (cf) c->rflags |= F_CF; if (cnt == 1 && (((r >> (bits - 1)) & 1) ^ cf)) c->rflags |= F_OF; set_szp(c, r, sz); return r; }   /* shl */
    case 5: { r = v >> cnt; cf = (int)((v >> (cnt - 1)) & 1); c->rflags &= ~(u64)(F_CF | F_OF | F_AF); if (cf) c->rflags |= F_CF; if (cnt == 1 && (v >> (bits - 1))) c->rflags |= F_OF; set_szp(c, r, sz); return r; }                             /* shr */
    default: { s64 sv = (s64)sext(v, sz); r = (u64)(sv >> cnt) & m; cf = (int)((sv >> (cnt - 1)) & 1); c->rflags &= ~(u64)(F_CF | F_OF | F_AF); if (cf) c->rflags |= F_CF; set_szp(c, r, sz); return r; }                                           /* sar */
  }
}

/* ---------- SSE helpers ---------- */
static void xmm_load(Ins *i, Xmm *out, int bytes) {
  if (i->mod == 3) { *out = i->c->x[i->rm]; return; }
  u64 a = EA(i); out->q[0] = out->q[1] = 0;
  if (bytes == 4) out->d[0] = rd32(i->c, a); else if (bytes == 8) out->q[0] = rd64(i->c, a); else { out->q[0] = rd64(i->c, a); out->q[1] = rd64(i->c, a + 8); }
}
static void xmm_store(Ins *i, const Xmm *v, int bytes) {
  if (i->mod == 3) { if (bytes == 16) i->c->x[i->rm] = *v; else if (bytes == 8) i->c->x[i->rm].q[0] = v->q[0]; else i->c->x[i->rm].d[0] = v->d[0]; return; }
  u64 a = EA(i);
  if (bytes == 4) wr32(i->c, a, v->d[0]); else if (bytes == 8) wr64(i->c, a, v->q[0]); else { wr64(i->c, a, v->q[0]); wr64(i->c, a + 8, v->q[1]); }
}
static void set_cmp_flags(Cpu *c, double a, double b) {
  c->rflags &= ~(u64)(F_ZF | F_PF | F_CF | F_OF | F_SF | F_AF);
  if (a != a || b != b) c->rflags |= F_ZF | F_PF | F_CF;
  else if (a == b) c->rflags |= F_ZF;
  else if (a < b) c->rflags |= F_CF;
}
static inline s32 f2i32(double v, int trunc) {
  if (v != v || v >= 2147483648.0 || v < -2147483648.0) return (s32)0x80000000;
  if (!trunc) v = __builtin_nearbyint(v);
  return (s32)v;
}
static inline s64 f2i64(double v, int trunc) {
  if (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) return (s64)0x8000000000000000ull;
  if (!trunc) v = __builtin_nearbyint(v);
  return (s64)v;
}
static double fminmax(double a, double b, int is_max) { if (a != a || b != b) return b; if (a == b) return b; return is_max ? (a > b ? a : b) : (a < b ? a : b); }
static int cmp_pred(double a, double b, int pred) {
  int un = (a != a) || (b != b);
  switch (pred & 7) {
    case 0: return !un && a == b; case 1: return !un && a < b; case 2: return !un && a <= b; case 3: return un;
    case 4: return un || a != b; case 5: return un || !(a < b); case 6: return un || !(a <= b); default: return !un;
  }
}

/* ---------- string ops ---------- */
static int string_op(Ins *i, int op, int sz) {
  Cpu *c = i->c; s64 step = (c->rflags & F_DF) ? -sz : sz;
  u64 cnt = i->rep ? (i->p67 ? (u32)c->r[RCX] : c->r[RCX]) : 1;
  if (i->rep && cnt == 0) return 1;
  /* fast path: forward `rep movs` / `rep stos` with both ranges inside one region and not overlapping → one host copy/fill
     (memcpy/memset in msvcrt, Bun and the JSC GC are `rep movsb`/`rep stosb` loops; this turns N guest iterations into one) */
  if (i->rep && cnt > 1 && step > 0 && !i->p67 && (op == 0xA4 || op == 0xAA) && cnt <= (1ull << 30) / (u64)sz) {
    u64 bytes = cnt * (u64)sz, si = c->r[RSI], di = c->r[RDI];
    if (i->seg == 1) si += c->fs_base; else if (i->seg == 2) si += c->gs_base;
    u8 *d = mem_ptr(c->mem, di, bytes, 1);
    if (d) {
      if (op == 0xAA) {
        u64 v = c->r[RAX];
        if (sz == 1) e_memset(d, (int)(u8)v, (size_t)bytes);
        else for (u64 k = 0; k < bytes; k += sz) __builtin_memcpy(d + k, &v, sz);
        c->r[RDI] += bytes; c->r[RCX] = 0; return 1;
      }
      int overlap = (di > si && di < si + bytes) || (si > di && si < di + bytes);
      u8 *sp = overlap ? 0 : mem_ptr(c->mem, si, bytes, 0);
      if (sp) { e_memcpy(d, sp, (size_t)bytes); c->r[RSI] += bytes; c->r[RDI] += bytes; c->r[RCX] = 0; return 1; }
    }
  }
  u64 iter = 0;
  while (cnt) {
    u64 si = c->r[RSI], di = c->r[RDI];
    if (i->seg == 1) si += c->fs_base; else if (i->seg == 2) si += c->gs_base;
    switch (op) {
      case 0xA4: wrn(c, di, sz, rdn(c, si, sz)); c->r[RSI] += step; c->r[RDI] += step; break;          /* movs */
      case 0xAA: wrn(c, di, sz, c->r[RAX]); c->r[RDI] += step; break;                                   /* stos */
      case 0xAC: set_reg(i, RAX, sz, rdn(c, si, sz)); c->r[RSI] += step; break;                         /* lods */
      case 0xA6: { u64 a = rdn(c, si, sz), b = rdn(c, di, sz); flags_sub(c, a, b, (a - b) & mask(sz), sz); c->r[RSI] += step; c->r[RDI] += step; break; } /* cmps */
      case 0xAE: { u64 a = c->r[RAX] & mask(sz), b = rdn(c, di, sz); flags_sub(c, a, b, (a - b) & mask(sz), sz); c->r[RDI] += step; break; }            /* scas */
    }
    if (c->status) return 0;
    if (i->rep) { c->r[RCX]--; cnt--; }
    else break;
    if ((op == 0xA6 || op == 0xAE)) { int z = !!(c->rflags & F_ZF); if (i->rep == 0xF3 && !z) break; if (i->rep == 0xF2 && z) break; }
    if (++iter > 1u << 26) { cpu_fault(c, "runaway rep string op"); return 0; }
  }
  if (c->mode32) { c->r[RSI] &= 0xffffffffull; c->r[RDI] &= 0xffffffffull; c->r[RCX] &= 0xffffffffull; }
  return 1;
}

/* ---------- main loop ---------- */
#define UNSUP(...) do { cpu_fault(c, __VA_ARGS__); return; } while (0)
#include "x87.inc"

static void exec_0f(Ins *i, Cpu *c);

/* ---------- fast handlers for the hottest instruction forms (chosen once per cache entry) ----------
 * Only plain forms: no segment/67/lock/rep prefixes, operand size 4 or 8 (p66 excluded).  Flags go through the same
 * helpers as the generic path, so behaviour is identical — the win is skipping the decode and the generic size/operand
 * dispatch.  Memory faults set c->status like everywhere else; cpu_run handles them the same way. */
static u8 dc_classify(const Dc *e) {
  if (e->seg || e->p67 || e->lock) return FK_NONE;
  u8 op = e->op; int osz = e->rexw ? 8 : 4;
  if (e->m32 && (op == 0xFF || op == 0x63 || op == 0xC7 || (op >= 0xB8 && op <= 0xBF))) return FK_NONE;   /* 32-bit mode: psz-dependent / no movsxd / moffs forms take the generic path */
  if (op == 0x0F && e->len > e->hdr) {   /* SSE forms take 66/F2/F3 as mandatory prefixes */
    u8 op2 = e->op2;
    if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29) return (e->p66 && e->rep) ? FK_NONE : FK_SSE_MOV;
    if ((op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E) && !(e->p66 && e->rep)) return FK_SSE_ARITH;
    if (op2 >= 0x54 && op2 <= 0x57 && !e->rep) return FK_SSE_LOGIC;
    if ((op2 == 0x2E || op2 == 0x2F) && !e->rep) return FK_SSE_CMP;
  }
  if (e->rep || e->p66) return (op == 0xC3 && e->rep == 0xF3) ? FK_RET : (op == 0x90 && e->rep == 0xF3) ? FK_NOP : (op == 0x0F && e->op2 == 0x1F && e->p66) ? FK_NOP : FK_NONE;   /* rep ret, pause, 66 0F 1F nop */
  if (op == 0x88) return FK_MOV8_RM_R; if (op == 0x8A) return FK_MOV8_R_RM; if (op == 0x80) return FK_ALU8_RM_IMM; if (op == 0xC6) return FK_MOV8_RM_IMM;
  if (op < 0x40 && (op & 7) == 0) return FK_ALU8_RM_R; if (op < 0x40 && (op & 7) == 2) return FK_ALU8_R_RM; if (op < 0x40 && (op & 7) == 4) return FK_ALU8_AL_IMM;
  if (op == 0x84) return FK_TEST8; if (op == 0x85) return e->mod == 3 ? FK_TEST_RR : FK_TEST_RM_R;
  if (op == 0x69 || op == 0x6B) return FK_IMUL_IMM; if (op == 0xF7) return (e->reg & 7) < 2 ? FK_F7_TEST : (e->reg & 7) < 4 ? FK_F7_NOTNEG : FK_NONE;
  if (op == 0x98) return FK_CDQE; if (op == 0x99) return FK_CDQ; if (op == 0xD1) return FK_SHIFT_1; if (op == 0xD3) return FK_SHIFT_CL; if (op == 0x90) return e->rexb ? FK_NONE : FK_NOP; if (op == 0xC2) return FK_RET_N;
  if (op == 0x89) return FK_MOV_RM_R; if (op == 0x8B) return FK_MOV_R_RM; if (op == 0x8D && e->mod != 3) return FK_LEA;
  if (op == 0x83) return FK_ALU_RM_IMM8; if (op == 0x81) return FK_ALU_RM_IMM32;
  if (op == 0x85 && e->mod == 3) return FK_TEST_RR;
  if (op < 0x40 && (op & 7) == 1) return FK_ALU_RM_R; if (op < 0x40 && (op & 7) == 3) return FK_ALU_R_RM; if (op < 0x40 && (op & 7) == 5) return FK_ALU_A_IMM;
  if (op >= 0x70 && op <= 0x7F) return FK_JCC8; if (op == 0xEB) return FK_JMP8; if (op == 0xE9) return FK_JMP32; if (op == 0xE8) return FK_CALL; if (op == 0xC3) return FK_RET;
  if (op >= 0x50 && op <= 0x57) return FK_PUSH; if (op >= 0x58 && op <= 0x5F) return FK_POP;
  if (op == 0x63 && osz == 8) return FK_MOVSXD; if (op == 0xC1) return FK_SHIFT_IMM; if (op == 0xFF) return FK_FF;
  if (op >= 0xB8 && op <= 0xBF) return FK_MOV_R_IMM; if (op == 0xC7) return FK_MOV_RM_IMM32;
  if (op == 0x0F && e->len > e->hdr) {   /* second opcode byte is inside the validated bytes */
    u8 op2 = (u8)((e->hdr < 8 ? e->b0 >> (e->hdr * 8) : e->b1 >> ((e->hdr - 8) * 8)) & 0xff);
    if (op2 >= 0x80 && op2 <= 0x8F) return FK_JCC32; if (op2 == 0xB6) return FK_MOVZX8; if (op2 == 0xB7) return FK_MOVZX16;
    if (op2 >= 0x90 && op2 <= 0x9F) return FK_SETCC; if (op2 >= 0x40 && op2 <= 0x4F) return FK_CMOV; if (op2 == 0xAF) return FK_IMUL_R_RM;
    if (op2 == 0xBE) return FK_MOVSX8; if (op2 == 0xBF) return FK_MOVSX16; if (op2 == 0x1F) return FK_NOP;
  }
  return FK_NONE;
}
static inline u64 fast_ea(Cpu *c, const Dc *e, u64 start) {
  if (e->riprel) return start + e->len + (u64)(s64)e->disp;
  u64 ea = (u64)(s64)e->disp; if (e->base >= 0) ea += c->r[(int)e->base]; if (e->idx >= 0) ea += c->r[(int)e->idx] << e->scale; return c->mode32 ? (u32)ea : ea;
}
static inline u64 fast_rm_get(Cpu *c, const Dc *e, u64 start, int sz) { if (e->mod == 3) return sz == 8 ? c->r[e->rm] : (u32)c->r[e->rm]; u64 a = fast_ea(c, e, start); return sz == 8 ? rd64(c, a) : rd32(c, a); }
static inline void fast_rm_set(Cpu *c, const Dc *e, u64 start, int sz, u64 v) { if (e->mod == 3) { if (sz == 8) c->r[e->rm] = v; else c->r[e->rm] = (u32)v; return; } u64 a = fast_ea(c, e, start); if (sz == 8) wr64(c, a, v); else wr32(c, a, (u32)v); }
static inline void fast_reg_set(Cpu *c, int r, int sz, u64 v) { if (sz == 8) c->r[r] = v; else c->r[r] = (u32)v; }
static inline u64 fast_reg_get(Cpu *c, int r, int sz) { return sz == 8 ? c->r[r] : (u32)c->r[r]; }
static inline s32 fast_imm32(const u8 *code, int off) { s32 v; __builtin_memcpy(&v, code + off, 4); return v; }
static inline u64 fast_r8_get(Cpu *c, const Dc *e, int r) { return (!e->rex && r >= 4 && r < 8) ? (c->r[r - 4] >> 8) & 0xff : c->r[r] & 0xff; }
static inline void fast_r8_set(Cpu *c, const Dc *e, int r, u64 v) { if (!e->rex && r >= 4 && r < 8) c->r[r - 4] = (c->r[r - 4] & ~0xff00ull) | ((v & 0xff) << 8); else c->r[r] = (c->r[r] & ~0xffull) | (v & 0xff); }
static inline u64 fast_rm8_get(Cpu *c, const Dc *e, u64 start) { return e->mod == 3 ? fast_r8_get(c, e, e->rm) : rd8(c, fast_ea(c, e, start)); }
static inline void fast_rm8_set(Cpu *c, const Dc *e, u64 start, u64 v) { if (e->mod == 3) fast_r8_set(c, e, e->rm, v); else wr8(c, fast_ea(c, e, start), (u8)v); }
static inline void fast_xmm_load(Cpu *c, const Dc *e, u64 start, Xmm *out, int bytes) {
  if (e->mod == 3) { *out = c->x[e->rm]; return; }
  u64 a = fast_ea(c, e, start); out->q[0] = out->q[1] = 0;
  if (bytes == 4) out->d[0] = rd32(c, a); else if (bytes == 8) out->q[0] = rd64(c, a); else { out->q[0] = rd64(c, a); out->q[1] = rd64(c, a + 8); }
}
static inline void fast_xmm_store(Cpu *c, const Dc *e, u64 start, const Xmm *v, int bytes) {
  if (e->mod == 3) { if (bytes == 16) c->x[e->rm] = *v; else if (bytes == 8) c->x[e->rm].q[0] = v->q[0]; else c->x[e->rm].d[0] = v->d[0]; return; }
  u64 a = fast_ea(c, e, start);
  if (bytes == 4) wr32(c, a, v->d[0]); else if (bytes == 8) wr64(c, a, v->q[0]); else { wr64(c, a, v->q[0]); wr64(c, a + 8, v->q[1]); }
}
/* returns the next rip */
static inline u64 fast_exec(Cpu *c, const Dc *e, const u8 *code, u64 start) {
  u64 next = start + e->len; int sz = e->rexw ? 8 : 4;
  switch (e->fast) {
    case FK_MOV_RM_R: fast_rm_set(c, e, start, sz, fast_reg_get(c, e->reg, sz)); return next;
    case FK_MOV_R_RM: { u64 v = fast_rm_get(c, e, start, sz); if (c->status == RUN_OK) fast_reg_set(c, e->reg, sz, v); return next; }
    case FK_LEA: fast_reg_set(c, e->reg, sz, fast_ea(c, e, start)); return next;
    case FK_ALU_RM_IMM8: case FK_ALU_RM_IMM32: { u64 b = e->fast == FK_ALU_RM_IMM8 ? (u64)(s64)(s8)code[e->mr_end] : (u64)(s64)fast_imm32(code, e->mr_end); u64 a = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next;
      int aop = e->reg & 7; u64 r = alu(c, aop, a, b, sz); if (aop != 7) fast_rm_set(c, e, start, sz, r); return next; }
    case FK_TEST_RR: flags_logic(c, fast_reg_get(c, e->rm, sz) & fast_reg_get(c, e->reg, sz), sz); return next;
    case FK_ALU_RM_R: { int aop = e->op >> 3; u64 a = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; u64 r = alu(c, aop, a, fast_reg_get(c, e->reg, sz), sz); if (aop != 7) fast_rm_set(c, e, start, sz, r); return next; }
    case FK_ALU_R_RM: { int aop = e->op >> 3; u64 b = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; u64 r = alu(c, aop, fast_reg_get(c, e->reg, sz), b, sz); if (aop != 7) fast_reg_set(c, e->reg, sz, r); return next; }
    case FK_ALU_A_IMM: { int aop = e->op >> 3; u64 b = (u64)(s64)fast_imm32(code, e->hdr); u64 r = alu(c, aop, fast_reg_get(c, RAX, sz), b, sz); if (aop != 7) fast_reg_set(c, RAX, sz, r); return next; }
    case FK_JCC8: return cond(c, e->op & 15) ? next + (u64)(s64)(s8)code[e->hdr] : next;
    case FK_JCC32: return cond(c, code[e->hdr] & 15) ? next + (u64)(s64)fast_imm32(code, e->hdr + 1) : next;
    case FK_JMP8: return next + (u64)(s64)(s8)code[e->hdr];
    case FK_JMP32: return next + (u64)(s64)fast_imm32(code, e->hdr);
    case FK_CALL: cpu_push(c, next); return next + (u64)(s64)fast_imm32(code, e->hdr);
    case FK_RET: return cpu_pop(c);
    case FK_PUSH: cpu_push(c, c->r[(e->op & 7) | (e->rexb << 3)]); return next;
    case FK_POP: { u64 v = cpu_pop(c); if (c->status == RUN_OK) c->r[(e->op & 7) | (e->rexb << 3)] = v; return next; }
    case FK_MOVZX8: { u64 v; if (e->mod == 3) { int r = e->rm; v = (!e->rex && r >= 4 && r < 8) ? (c->r[r - 4] >> 8) & 0xff : c->r[r] & 0xff; } else v = rd8(c, fast_ea(c, e, start)); if (c->status == RUN_OK) fast_reg_set(c, e->reg, sz, v); return next; }
    case FK_MOVZX16: { u64 v = e->mod == 3 ? c->r[e->rm] & 0xffff : rd16(c, fast_ea(c, e, start)); if (c->status == RUN_OK) fast_reg_set(c, e->reg, sz, v); return next; }
    case FK_MOVSXD: { u64 v = fast_rm_get(c, e, start, 4); if (c->status == RUN_OK) c->r[e->reg] = (u64)(s64)(s32)v; return next; }
    case FK_SHIFT_IMM: { u64 v = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; fast_rm_set(c, e, start, sz, shift(c, e->reg & 7, v, code[e->mr_end], sz)); return next; }
    case FK_FF: { int sub = e->reg & 7;
      switch (sub) {
        case 0: { u64 v = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; u64 r = (v + 1) & mask(sz); u64 cf = c->rflags & F_CF; flags_add(c, v, 1, r, sz); c->rflags = (c->rflags & ~(u64)F_CF) | cf; fast_rm_set(c, e, start, sz, r); return next; }
        case 1: { u64 v = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; u64 r = (v - 1) & mask(sz); u64 cf = c->rflags & F_CF; flags_sub(c, v, 1, r, sz); c->rflags = (c->rflags & ~(u64)F_CF) | cf; fast_rm_set(c, e, start, sz, r); return next; }
        case 2: { u64 t = fast_rm_get(c, e, start, 8); if (c->status != RUN_OK) return next; cpu_push(c, next); return t; }
        case 4: { u64 t = fast_rm_get(c, e, start, 8); return c->status == RUN_OK ? t : next; }
        case 6: { u64 v = fast_rm_get(c, e, start, 8); if (c->status == RUN_OK) cpu_push(c, v); return next; }
        default: return 0;   /* 0 = not handled → generic path */
      } }
    case FK_MOV_R_IMM: { int r = (e->op & 7) | (e->rexb << 3); if (sz == 8) { u64 v; __builtin_memcpy(&v, code + e->hdr, 8); c->r[r] = v; } else c->r[r] = (u32)fast_imm32(code, e->hdr); return next; }
    case FK_MOV_RM_IMM32: fast_rm_set(c, e, start, sz, (u64)(s64)fast_imm32(code, e->mr_end)); return next;
    case FK_MOV8_RM_R: fast_rm8_set(c, e, start, fast_r8_get(c, e, e->reg)); return next;
    case FK_MOV8_R_RM: { u64 v = fast_rm8_get(c, e, start); if (c->status == RUN_OK) fast_r8_set(c, e, e->reg, v); return next; }
    case FK_MOV8_RM_IMM: fast_rm8_set(c, e, start, code[e->mr_end]); return next;
    case FK_ALU8_RM_IMM: { u64 a = fast_rm8_get(c, e, start); if (c->status != RUN_OK) return next; int aop = e->reg & 7; u64 r = alu(c, aop, a, code[e->mr_end], 1); if (aop != 7) fast_rm8_set(c, e, start, r); return next; }
    case FK_ALU8_RM_R: { int aop = e->op >> 3; u64 a = fast_rm8_get(c, e, start); if (c->status != RUN_OK) return next; u64 r = alu(c, aop, a, fast_r8_get(c, e, e->reg), 1); if (aop != 7) fast_rm8_set(c, e, start, r); return next; }
    case FK_ALU8_R_RM: { int aop = e->op >> 3; u64 b = fast_rm8_get(c, e, start); if (c->status != RUN_OK) return next; u64 r = alu(c, aop, fast_r8_get(c, e, e->reg), b, 1); if (aop != 7) fast_r8_set(c, e, e->reg, r); return next; }
    case FK_ALU8_AL_IMM: { int aop = e->op >> 3; u64 r = alu(c, aop, c->r[RAX] & 0xff, code[e->hdr], 1); if (aop != 7) c->r[RAX] = (c->r[RAX] & ~0xffull) | (r & 0xff); return next; }
    case FK_TEST8: { u64 a = fast_rm8_get(c, e, start); if (c->status == RUN_OK) flags_logic(c, a & fast_r8_get(c, e, e->reg), 1); return next; }
    case FK_TEST_RM_R: { u64 a = fast_rm_get(c, e, start, sz); if (c->status == RUN_OK) flags_logic(c, a & fast_reg_get(c, e->reg, sz), sz); return next; }
    case FK_SETCC: fast_rm8_set(c, e, start, cond(c, e->op2 & 15)); return next;
    case FK_CMOV: { u64 v = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; if (cond(c, e->op2 & 15)) fast_reg_set(c, e->reg, sz, v); else if (sz == 4) c->r[e->reg] = (u32)c->r[e->reg]; return next; }
    case FK_IMUL_R_RM: case FK_IMUL_IMM: { s64 a, b;
      if (e->fast == FK_IMUL_R_RM) { a = (s64)sext(fast_reg_get(c, e->reg, sz), sz); b = (s64)sext(fast_rm_get(c, e, start, sz), sz); }
      else { a = (s64)sext(fast_rm_get(c, e, start, sz), sz); b = e->op == 0x6B ? (s64)(s8)code[e->mr_end] : (s64)fast_imm32(code, e->mr_end); }
      if (c->status != RUN_OK) return next;
      s128 p = (s128)a * b; u64 r = (u64)p & mask(sz); int ov = sz == 8 ? (p != (s128)(s64)p) : ((s64)sext(r, sz) != (s64)p);
      c->rflags &= ~(u64)(F_CF | F_OF); if (ov) c->rflags |= F_CF | F_OF; set_szp(c, r, sz); fast_reg_set(c, e->reg, sz, r); return next; }
    case FK_F7_TEST: { u64 a = fast_rm_get(c, e, start, sz); if (c->status == RUN_OK) flags_logic(c, a & (u64)(s64)fast_imm32(code, e->mr_end), sz); return next; }
    case FK_F7_NOTNEG: { u64 v = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; if ((e->reg & 7) == 2) fast_rm_set(c, e, start, sz, ~v); else { u64 r = (0 - v) & mask(sz); flags_sub(c, 0, v, r, sz); fast_rm_set(c, e, start, sz, r); } return next; }
    case FK_CDQE: if (sz == 8) c->r[RAX] = (u64)(s64)(s32)c->r[RAX]; else c->r[RAX] = (u32)(s32)(s16)c->r[RAX]; return next;
    case FK_CDQ: { u64 s_ = (s64)sext(c->r[RAX], sz) < 0 ? ~0ull : 0; fast_reg_set(c, RDX, sz, s_); return next; }
    case FK_SHIFT_1: case FK_SHIFT_CL: { u64 v = fast_rm_get(c, e, start, sz); if (c->status != RUN_OK) return next; fast_rm_set(c, e, start, sz, shift(c, e->reg & 7, v, e->fast == FK_SHIFT_1 ? 1 : (int)(c->r[RCX] & 0xff), sz)); return next; }
    case FK_MOVSX8: { u64 v = fast_rm8_get(c, e, start); if (c->status == RUN_OK) fast_reg_set(c, e->reg, sz, (u64)(s64)(s8)v); return next; }
    case FK_MOVSX16: { u64 v = e->mod == 3 ? c->r[e->rm] & 0xffff : rd16(c, fast_ea(c, e, start)); if (c->status == RUN_OK) fast_reg_set(c, e->reg, sz, (u64)(s64)(s16)v); return next; }
    case FK_NOP: return next;
    case FK_RET_N: { u64 ra = cpu_pop(c); if (c->status != RUN_OK) return next; c->r[RSP] += (u16)(code[e->hdr] | (code[e->hdr + 1] << 8)); if (c->mode32) c->r[RSP] &= 0xffffffffull; return ra; }
    case FK_SSE_MOV: { u8 op2 = e->op2; Xmm v;
      if (op2 == 0x28 || op2 == 0x29) { if (op2 == 0x28) { fast_xmm_load(c, e, start, &v, 16); if (c->status == RUN_OK) c->x[e->reg] = v; } else fast_xmm_store(c, e, start, &c->x[e->reg], 16); return next; }
      int bytes = e->rep == 0xF3 ? 4 : e->rep == 0xF2 ? 8 : 16;
      if (op2 == 0x10) { if (bytes == 16) { fast_xmm_load(c, e, start, &v, 16); if (c->status == RUN_OK) c->x[e->reg] = v; }
        else { fast_xmm_load(c, e, start, &v, bytes); if (c->status != RUN_OK) return next; if (e->mod == 3) { if (bytes == 4) c->x[e->reg].d[0] = v.d[0]; else c->x[e->reg].q[0] = v.q[0]; } else { c->x[e->reg].q[1] = 0; if (bytes == 4) { c->x[e->reg].q[0] = 0; c->x[e->reg].d[0] = v.d[0]; } else c->x[e->reg].q[0] = v.q[0]; } } }
      else fast_xmm_store(c, e, start, &c->x[e->reg], bytes);
      return next; }
    case FK_SSE_ARITH: { u8 op2 = e->op2; Xmm s_, *d = &c->x[e->reg]; int sc = e->rep == 0xF3 ? 1 : e->rep == 0xF2 ? 2 : e->p66 ? 3 : 0;
      fast_xmm_load(c, e, start, &s_, sc == 1 ? 4 : sc == 2 ? 8 : 16); if (c->status != RUN_OK) return next;
#define FOP(a, b) (op2 == 0x58 ? (a) + (b) : op2 == 0x59 ? (a) * (b) : op2 == 0x5C ? (a) - (b) : (a) / (b))
      if (sc == 1) d->f32[0] = (float)FOP((double)d->f32[0], (double)s_.f32[0]);
      else if (sc == 2) d->f64[0] = FOP(d->f64[0], s_.f64[0]);
      else if (sc == 3) { d->f64[0] = FOP(d->f64[0], s_.f64[0]); d->f64[1] = FOP(d->f64[1], s_.f64[1]); }
      else for (int k = 0; k < 4; k++) d->f32[k] = (float)FOP((double)d->f32[k], (double)s_.f32[k]);
#undef FOP
      return next; }
    case FK_SSE_LOGIC: { u8 op2 = e->op2; Xmm s_; fast_xmm_load(c, e, start, &s_, 16); if (c->status != RUN_OK) return next; Xmm *d = &c->x[e->reg];
      for (int k = 0; k < 2; k++) d->q[k] = op2 == 0x54 ? d->q[k] & s_.q[k] : op2 == 0x55 ? ~d->q[k] & s_.q[k] : op2 == 0x56 ? d->q[k] | s_.q[k] : d->q[k] ^ s_.q[k]; return next; }
    case FK_SSE_CMP: { Xmm s_; fast_xmm_load(c, e, start, &s_, e->p66 ? 8 : 4); if (c->status != RUN_OK) return next;
      double a = e->p66 ? c->x[e->reg].f64[0] : (double)c->x[e->reg].f32[0], b = e->p66 ? s_.f64[0] : (double)s_.f32[0]; set_cmp_flags(c, a, b); return next; }
  }
  return 0;
}

static void exec_body(Ins *i, u8 op);
static void exec_one(Ins *i) {
  Cpu *c = i->c;
  u8 op;
  /* prefixes */
  for (;;) {
    op = fetch8(i);
    if (op == 0x66) i->p66 = 1; else if (op == 0x67) i->p67 = 1; else if (op == 0xF2 || op == 0xF3) i->rep = op;
    else if (op == 0x64) i->seg = 1; else if (op == 0x65) i->seg = 2; else if (op == 0xF0) i->lock = 1;
    else if (op == 0x2E || op == 0x3E || op == 0x26 || op == 0x36) { /* cs/ds/es/ss: ignored (branch hints) */ }
    else if ((op & 0xF0) == 0x40 && !c->mode32) { i->rex = 1; i->rexw = (op >> 3) & 1; i->rexr = (op >> 2) & 1; i->rexx = (op >> 1) & 1; i->rexb = op & 1; op = fetch8(i); break; }
    else break;
  }
  if (c->status) return;
  i->hdr = (int)(i->ip - i->start);
  exec_body(i, op);
}
static void exec_body(Ins *i, u8 op) {
  Cpu *c = i->c;
  int osz = i->rexw ? 8 : i->p66 ? 2 : 4;   /* operand size */
  if (op == 0x0F) { exec_0f(i, c); return; }

  /* ALU block 00-3F */
  if (op < 0x40 && (op & 7) < 6) {
    int aop = op >> 3, form = op & 7;
    switch (form) {
      case 0: case 1: { int sz = form ? osz : 1; decode_modrm(i); u64 a = rm_get(i, sz), b = get_reg(i, i->reg, sz); u64 r = alu(c, aop, a, b, sz); if (aop != 7) rm_set(i, sz, r); return; }
      case 2: case 3: { int sz = (form & 1) ? osz : 1; decode_modrm(i); u64 a = get_reg(i, i->reg, sz), b = rm_get(i, sz); u64 r = alu(c, aop, a, b, sz); if (aop != 7) set_reg(i, i->reg, sz, r); return; }
      case 4: { u64 b = fetch8(i); u64 r = alu(c, aop, get_reg(i, RAX, 1), b, 1); if (aop != 7) set_reg(i, RAX, 1, r); return; }
      case 5: { u64 b = osz == 2 ? fetch16(i) : sext(fetch32(i), 4); u64 r = alu(c, aop, get_reg(i, RAX, osz), b, osz); if (aop != 7) set_reg(i, RAX, osz, r); return; }
    }
  }
  switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: { int r = (op & 7) | (i->rexb << 3); u64 v = c->r[r]; if (i->p66) { c->r[RSP] -= 2; wr16(c, c->r[RSP], (u16)v); } else cpu_push(c, v); return; }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: { int r = (op & 7) | (i->rexb << 3); if (i->p66) { u16 v = rd16(c, c->r[RSP]); c->r[RSP] += 2; set_reg(i, r, 2, v); } else { u64 v = cpu_pop(c); c->r[r] = v; } return; }
    case 0x63: { if (c->mode32) UNSUP("arpl"); decode_modrm(i); u64 v = rm_get(i, 4); set_reg(i, i->reg, osz, osz == 8 ? sext(v, 4) : v); return; }   /* movsxd */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: { int r = op & 7; u64 v = get_reg(i, r, osz), res = (v + 1) & mask(osz); u64 cf = c->rflags & F_CF; flags_add(c, v, 1, res, osz); c->rflags = (c->rflags & ~(u64)F_CF) | cf; set_reg(i, r, osz, res); return; }   /* 32-bit mode: inc r */
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: { int r = op & 7; u64 v = get_reg(i, r, osz), res = (v - 1) & mask(osz); u64 cf = c->rflags & F_CF; flags_sub(c, v, 1, res, osz); c->rflags = (c->rflags & ~(u64)F_CF) | cf; set_reg(i, r, osz, res); return; }   /* dec r */
    case 0x60: { if (!c->mode32) UNSUP("pusha outside 32-bit mode"); u64 sp = c->r[RSP]; for (int r = 0; r < 8; r++) cpu_push(c, r == RSP ? sp : c->r[r]); return; }
    case 0x61: { if (!c->mode32) UNSUP("popa outside 32-bit mode"); for (int r = 7; r >= 0; r--) { u64 v = cpu_pop(c); if (r != RSP) c->r[r] = v; } return; }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: { s64 d = fetch_s8(i); int asz = (i->p67 ^ (c->mode32 ? 1 : 0)) ? 4 : 8; if (c->mode32 && i->p67) asz = 2;
      if (op == 0xE3) { if (!(get_reg(i, RCX, asz))) i->ip += d; return; }
      u64 n = (get_reg(i, RCX, asz) - 1) & mask(asz); set_reg(i, RCX, asz, n); int z = !!(c->rflags & F_ZF);
      if (n && (op == 0xE2 || (op == 0xE1 && z) || (op == 0xE0 && !z))) i->ip += d; return; }   /* loopne / loope / loop / jecxz */
    case 0x68: { u64 v = sext(fetch32(i), 4); cpu_push(c, v); return; }
    case 0x6A: { u64 v = (u64)fetch_s8(i); cpu_push(c, v); return; }
    case 0x69: case 0x6B: { decode_modrm(i); fix_rip_rel(i, op == 0x6B ? 1 : (osz == 2 ? 2 : 4)); s64 a = (s64)sext(rm_get(i, osz), osz); s64 b = op == 0x6B ? fetch_s8(i) : (osz == 2 ? (s64)(s16)fetch16(i) : fetch_s32(i));
      s128 full = (s128)a * b; u64 r = (u64)full & mask(osz); int ov = osz == 8 ? (full != (s128)(s64)full) : ((s64)sext(r, osz) != (s64)full);
      c->rflags &= ~(u64)(F_CF | F_OF); if (ov) c->rflags |= F_CF | F_OF; set_szp(c, r, osz); set_reg(i, i->reg, osz, r); return; }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: { s64 d = fetch_s8(i); if (cond(c, op & 15)) i->ip += d; return; }
    case 0x80: case 0x81: case 0x83: { int sz = op == 0x80 ? 1 : osz; decode_modrm(i); fix_rip_rel(i, op == 0x81 ? (sz == 2 ? 2 : 4) : 1);
      u64 b = op == 0x81 ? (sz == 2 ? fetch16(i) : sext(fetch32(i), 4)) : (u64)fetch_s8(i); u64 a = rm_get(i, sz); u64 r = alu(c, i->reg & 7, a, b, sz); if ((i->reg & 7) != 7) rm_set(i, sz, r); return; }
    case 0x84: case 0x85: { int sz = op & 1 ? osz : 1; decode_modrm(i); flags_logic(c, rm_get(i, sz) & get_reg(i, i->reg, sz), sz); return; }
    case 0x86: case 0x87: { int sz = op & 1 ? osz : 1; decode_modrm(i); u64 a = rm_get(i, sz), b = get_reg(i, i->reg, sz); rm_set(i, sz, b); set_reg(i, i->reg, sz, a); return; }
    case 0x88: case 0x89: { int sz = op & 1 ? osz : 1; decode_modrm(i); rm_set(i, sz, get_reg(i, i->reg, sz)); return; }
    case 0x8A: case 0x8B: { int sz = op & 1 ? osz : 1; decode_modrm(i); set_reg(i, i->reg, sz, rm_get(i, sz)); return; }
    case 0x8D: { decode_modrm(i); if (i->mod == 3) UNSUP("lea with register operand"); u64 ea = EA(i); if (i->seg == 1) ea -= c->fs_base; else if (i->seg == 2) ea -= c->gs_base; /* LEA ignores segment overrides (Delphi pads with 64 66 8D 80 … NOPs) */ if (i->p67 || c->mode32) ea &= 0xffffffffull; set_reg(i, i->reg, osz, ea); return; }
    case 0x8F: { u64 v = cpu_pop(c); decode_modrm(i); fix_rip_rel(i, 0); rm_set(i, c->mode32 ? 4 : 8, v); return; }   /* pop r/m64: an rsp-based address is computed AFTER the increment (V8 tail calls rely on `pop [rsp+x]`) */
    case 0x90: if (i->rexb) { u64 t = c->r[RAX]; c->r[RAX] = c->r[R8]; c->r[R8] = t; } return;   /* nop / pause / xchg r8,rax */
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: { int r = (op & 7) | (i->rexb << 3); u64 a = get_reg(i, RAX, osz), b = get_reg(i, r, osz); set_reg(i, RAX, osz, b); set_reg(i, r, osz, a); return; }
    case 0x98: if (osz == 8) c->r[RAX] = sext(c->r[RAX], 4); else if (osz == 4) c->r[RAX] = sext(c->r[RAX], 2) & 0xffffffff; else set_reg(i, RAX, 2, sext(c->r[RAX], 1)); return;
    case 0x99: { u64 s = (s64)sext(c->r[RAX], osz) < 0 ? ~0ull : 0; set_reg(i, RDX, osz, s); return; }
    case 0x9C: cpu_push(c, c->rflags); return;
    case 0x9D: c->rflags = (cpu_pop(c) & 0xcd5) | 0x202; return;
    case 0x9E: c->rflags = (c->rflags & ~0xd5ull) | ((c->r[RAX] >> 8) & 0xd5); return;
    case 0x9F: c->r[RAX] = (c->r[RAX] & ~0xff00ull) | (((c->rflags & 0xd5) | 2) << 8); return;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xA6: case 0xA7: case 0xAE: case 0xAF: string_op(i, op & ~1, op & 1 ? osz : 1); return;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {                                   /* mov al/eax/rax <-> moffs (movabs) */
      u64 a = c->mode32 ? (i->p67 ? (u64)fetch16(i) : (u64)fetch32(i)) : i->p67 ? (u64)fetch32(i) : fetch64(i); if (i->seg == 1) a += c->fs_base; else if (i->seg == 2) a += c->gs_base;
      int sz = (op & 1) ? osz : 1;
      if (op & 2) wrn(c, a, sz, get_reg(i, RAX, sz)); else { u64 v = rdn(c, a, sz); if (c->status != RUN_OK) return; set_reg(i, RAX, sz, v); }
      return; }
    case 0xA8: { flags_logic(c, (c->r[RAX] & 0xff) & fetch8(i), 1); return; }
    case 0xA9: { u64 b = osz == 2 ? fetch16(i) : sext(fetch32(i), 4); flags_logic(c, get_reg(i, RAX, osz) & b, osz); return; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: set_reg(i, (op & 7) | (i->rexb << 3), 1, fetch8(i)); return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: { int r = (op & 7) | (i->rexb << 3); if (osz == 8) c->r[r] = fetch64(i); else if (osz == 2) set_reg(i, r, 2, fetch16(i)); else c->r[r] = fetch32(i); return; }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: { int sz = op & 1 ? osz : 1; decode_modrm(i); fix_rip_rel(i, op <= 0xC1 ? 1 : 0); int cnt = op <= 0xC1 ? fetch8(i) : op <= 0xD1 ? 1 : (int)(c->r[RCX] & 0xff); u64 v = rm_get(i, sz); rm_set(i, sz, shift(c, i->reg & 7, v, cnt, sz)); return; }
    case 0xC2: { u16 n = fetch16(i); u64 ra = cpu_pop(c); c->r[RSP] += n; if (c->mode32) c->r[RSP] &= 0xffffffffull; i->ip = ra; return; }
    case 0xC3: i->ip = cpu_pop(c); return;
    case 0xC6: case 0xC7: { int sz = op & 1 ? osz : 1; decode_modrm(i); fix_rip_rel(i, sz == 1 ? 1 : sz == 2 ? 2 : 4); u64 v = sz == 1 ? fetch8(i) : sz == 2 ? fetch16(i) : sext(fetch32(i), 4); rm_set(i, sz, v); return; }
    case 0xC9: c->r[RSP] = c->mode32 ? (u32)c->r[RBP] : c->r[RBP]; c->r[RBP] = cpu_pop(c); return;
    case 0xCC: c->status = RUN_BREAK; c->rip = i->ip; return;
    case 0xE8: { s64 d = fetch_s32(i); cpu_push(c, i->ip); i->ip += d; return; }
    case 0xE9: { s64 d = fetch_s32(i); i->ip += d; return; }
    case 0xEB: { s64 d = fetch_s8(i); i->ip += d; return; }
    case 0xF4: cpu_fault(c, "hlt"); return;
    case 0xF5: c->rflags ^= F_CF; return;
    case 0xF8: c->rflags &= ~(u64)F_CF; return;
    case 0xF9: c->rflags |= F_CF; return;
    case 0xFC: c->rflags &= ~(u64)F_DF; return;
    case 0xFD: c->rflags |= F_DF; return;
    case 0xF6: case 0xF7: {
      int sz = op & 1 ? osz : 1; decode_modrm(i); int sub = i->reg & 7;
      if (sub < 2) { fix_rip_rel(i, sz == 1 ? 1 : sz == 2 ? 2 : 4); u64 imm = sz == 1 ? fetch8(i) : sz == 2 ? fetch16(i) : sext(fetch32(i), 4); flags_logic(c, rm_get(i, sz) & imm, sz); return; }
      u64 v = rm_get(i, sz);
      switch (sub) {
        case 2: rm_set(i, sz, ~v); return;
        case 3: { u64 r = (0 - v) & mask(sz); flags_sub(c, 0, v, r, sz); rm_set(i, sz, r); return; }
        case 4: { /* mul */ u64 a = get_reg(i, RAX, sz);
          if (sz == 8) { u128 p = (u128)a * v; c->r[RAX] = (u64)p; c->r[RDX] = (u64)(p >> 64); c->rflags &= ~(u64)(F_CF | F_OF); if (c->r[RDX]) c->rflags |= F_CF | F_OF; }
          else { u64 p = a * v; if (sz == 1) set_reg(i, RAX, 2, p); else { set_reg(i, RAX, sz, p); set_reg(i, RDX, sz, p >> (sz * 8)); } c->rflags &= ~(u64)(F_CF | F_OF); if (p >> (sz * 8)) c->rflags |= F_CF | F_OF; }
          return; }
        case 5: { /* imul one operand */ s64 a = (s64)sext(get_reg(i, RAX, sz), sz), b = (s64)sext(v, sz);
          if (sz == 8) { s128 p = (s128)a * b; c->r[RAX] = (u64)p; c->r[RDX] = (u64)(p >> 64); c->rflags &= ~(u64)(F_CF | F_OF); if (p != (s128)(s64)p) c->rflags |= F_CF | F_OF; }
          else { s64 p = a * b; if (sz == 1) set_reg(i, RAX, 2, (u64)p); else { set_reg(i, RAX, sz, (u64)p); set_reg(i, RDX, sz, (u64)p >> (sz * 8)); } c->rflags &= ~(u64)(F_CF | F_OF); if (p != (s64)sext((u64)p, sz)) c->rflags |= F_CF | F_OF; }
          return; }
        case 6: { /* div */ if (!v) { fault_code(c, 0xC0000094u, "integer divide by zero"); return; };
          if (sz == 8) { u128 n = ((u128)c->r[RDX] << 64) | c->r[RAX]; u128 q = n / v; if (q >> 64) { fault_code(c, 0xC0000095u, "integer overflow (div)"); return; }; c->r[RAX] = (u64)q; c->r[RDX] = (u64)(n % v); }
          else if (sz == 1) { u64 n = c->r[RAX] & 0xffff; u64 q = n / v; if (q > 0xff) { fault_code(c, 0xC0000095u, "integer overflow (div)"); return; }; set_reg(i, RAX, 2, (q & 0xff) | ((n % v) << 8)); }
          else { u64 n = (get_reg(i, RDX, sz) << (sz * 8)) | get_reg(i, RAX, sz); u64 q = n / v; if (q > mask(sz)) { fault_code(c, 0xC0000095u, "integer overflow (div)"); return; }; set_reg(i, RAX, sz, q); set_reg(i, RDX, sz, n % v); }
          return; }
        case 7: { /* idiv */ if (!v) { fault_code(c, 0xC0000094u, "integer divide by zero"); return; }; s64 d = (s64)sext(v, sz);
          if (sz == 8) { s128 n = ((s128)(s64)c->r[RDX] << 64) | c->r[RAX]; s128 q = n / d; if (q != (s128)(s64)q) { fault_code(c, 0xC0000095u, "integer overflow (div)"); return; }; c->r[RAX] = (u64)q; c->r[RDX] = (u64)(n % d); }
          else if (sz == 1) { s64 n = (s16)(c->r[RAX] & 0xffff); s64 q = n / d; set_reg(i, RAX, 2, ((u64)q & 0xff) | (((u64)(n % d) & 0xff) << 8)); }
          else { s64 n = (s64)((get_reg(i, RDX, sz) << (sz * 8)) | get_reg(i, RAX, sz)); n = sz == 4 ? n : (s32)n; s64 q = n / d; set_reg(i, RAX, sz, (u64)q); set_reg(i, RDX, sz, (u64)(n % d)); }
          return; }
      }
      return; }
    case 0xFE: case 0xFF: {
      int sz = op & 1 ? osz : 1; decode_modrm(i); int sub = i->reg & 7;
      switch (sub) {
        case 0: { u64 v = rm_get(i, sz), r = (v + 1) & mask(sz); u64 cf = c->rflags & F_CF; flags_add(c, v, 1, r, sz); c->rflags = (c->rflags & ~(u64)F_CF) | cf; rm_set(i, sz, r); return; }
        case 1: { u64 v = rm_get(i, sz), r = (v - 1) & mask(sz); u64 cf = c->rflags & F_CF; flags_sub(c, v, 1, r, sz); c->rflags = (c->rflags & ~(u64)F_CF) | cf; rm_set(i, sz, r); return; }
        case 2: { u64 t = rm_get(i, c->mode32 ? 4 : 8); cpu_push(c, i->ip); i->ip = t; return; }
        case 4: { i->ip = rm_get(i, c->mode32 ? 4 : 8); return; }
        case 6: { u64 v = rm_get(i, c->mode32 ? 4 : 8); cpu_push(c, v); return; }
        default: UNSUP("FF /%d unsupported", sub);
      }
    }
    case 0xC8: { u16 size = fetch16(i); u8 lvl = fetch8(i); (void)lvl; cpu_push(c, c->r[RBP]); c->r[RBP] = c->r[RSP]; c->r[RSP] -= size; if (c->mode32) c->r[RSP] &= 0xffffffffull; return; }
    case 0xCD: { u8 n = fetch8(i); UNSUP("int 0x%02x", n); }
    case 0x9B: return; /* fwait */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: exec_x87(i, c, op); return;
    default: UNSUP("unsupported opcode 0x%02x", op);
  }
}

#include "sse4.inc"
static void exec_0f(Ins *i, Cpu *c) {
  u8 op = fetch8(i);
  int osz = i->rexw ? 8 : i->p66 ? 2 : 4;
  /* jcc rel32 / setcc / cmovcc */
  if (op >= 0x80 && op <= 0x8F) { s64 d = fetch_s32(i); if (cond(c, op & 15)) i->ip += d; return; }
  if (op >= 0x90 && op <= 0x9F) { decode_modrm(i); rm_set(i, 1, cond(c, op & 15)); return; }
  if (op >= 0x40 && op <= 0x4F) { decode_modrm(i); u64 v = rm_get(i, osz); if (cond(c, op & 15)) set_reg(i, i->reg, osz, v); else if (osz == 4) set_reg(i, i->reg, 4, c->r[i->reg]); return; }
  switch (op) {
    case 0x05: UNSUP("syscall");
    case 0x0B: fault_code(c, 0xC000001Du, "ud2 (illegal instruction)"); return;
    case 0x0D: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: decode_modrm(i); return;   /* nop / prefetch / endbr */
    case 0x31: { u64 t = (u64)(host_now_ms() * 1000000.0) + c->icount; c->r[RAX] = t & 0xffffffff; c->r[RDX] = t >> 32; return; }     /* rdtsc */
    case 0xA2: { /* cpuid */ u32 leaf = (u32)c->r[RAX];
      switch (leaf) {
        case 0: c->r[RAX] = 7; c->r[RBX] = 0x756e6547; c->r[RDX] = 0x49656e69; c->r[RCX] = 0x6c65746e; break;
        case 1: c->r[RAX] = 0x000306a9; c->r[RBX] = 0x00100800; c->r[RCX] = (1u << 0) | (1u << 1) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23); c->r[RDX] = 0x078bfbff; break; /* fpu..sse2 + SSE3/PCLMULQDQ/SSSE3/CX16/SSE4.1/SSE4.2/POPCNT; no AES, no AVX/OSXSAVE */
        case 7: c->r[RAX] = c->r[RBX] = c->r[RCX] = c->r[RDX] = 0; break;
        case 2: c->r[RAX] = 0x80302C01; c->r[RBX] = c->r[RCX] = c->r[RDX] = 0; break;   /* cache descriptors: 2C L1D 32K, 30 L1I 32K, 80 L2 512K */
        case 4: { u32 sub = (u32)c->r[RCX]; /* deterministic cache parameters: 32K L1D, 32K L1I, 256K L2, 8M L3 (OpenBLAS and game engines size their blocking from these) */
          static const u32 cp[4][3] = { { 0x121, 63u | (7u << 22), 63 }, { 0x122, 63u | (7u << 22), 63 }, { 0x143, 63u | (7u << 22), 511 }, { 0x163, 63u | (15u << 22), 8191 } };
          if (sub < 4) { c->r[RAX] = cp[sub][0]; c->r[RBX] = cp[sub][1]; c->r[RCX] = cp[sub][2]; c->r[RDX] = 0; } else c->r[RAX] = c->r[RBX] = c->r[RCX] = c->r[RDX] = 0; break; }
        case 0x80000000: c->r[RAX] = 0x80000008; c->r[RBX] = c->r[RCX] = c->r[RDX] = 0; break;
        case 0x80000002: case 0x80000003: case 0x80000004: { static const char brand[48] = "Nebula x86-64 (wasm64 emulated CPU)"; const u32 *b = (const u32 *)(const void *)(brand + (leaf - 0x80000002) * 16); c->r[RAX] = b[0]; c->r[RBX] = b[1]; c->r[RCX] = b[2]; c->r[RDX] = b[3]; break; }
        case 0x80000005: c->r[RAX] = 0; c->r[RBX] = 0; c->r[RCX] = 0x20080140; c->r[RDX] = 0x20080140; break;   /* L1D/L1I 32K, 8-way, 64 B lines */
        case 0x80000006: c->r[RAX] = c->r[RBX] = 0; c->r[RCX] = (256u << 16) | (6u << 12) | 64u; c->r[RDX] = (8192u << 18) | (6u << 12) | 64u; break;   /* L2 256 KB 8-way 64 B (OpenBLAS get_L2_size), L3 8 MB */
        case 0x80000008: c->r[RAX] = 0x3030; c->r[RBX] = c->r[RCX] = c->r[RDX] = 0; break;   /* 48-bit physical/virtual */
        case 0x80000001: c->r[RAX] = c->r[RBX] = 0; c->r[RCX] = 1u << 0; c->r[RDX] = (1u << 29) | (1u << 20) | (1u << 11); break;   /* LAHF_LM (x86-64-v2 needs it), LM, NX, SYSCALL */
        default: c->r[RAX] = c->r[RBX] = c->r[RCX] = c->r[RDX] = 0;
      }
      c->r[RAX] &= 0xffffffff; c->r[RBX] &= 0xffffffff; c->r[RCX] &= 0xffffffff; c->r[RDX] &= 0xffffffff; return; }
    case 0x01: { u8 m = fetch8(i); if (m == 0xD0) { c->r[RAX] = 0; c->r[RDX] = 0; return; } /* xgetbv */ UNSUP("0F 01 %02x", m); }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: { decode_modrm(i); u64 b = get_reg(i, i->reg, osz); int bits = osz * 8; u64 v;
      if (i->mod == 3) { v = get_reg(i, i->rm, osz); b &= bits - 1; }
      else { s64 off = (s64)sext(b, osz); s64 q = off >> (osz == 8 ? 6 : osz == 4 ? 5 : 4); i->ea = EA(i) + (u64)(q * osz); b = (u64)off & (bits - 1); v = rdn(c, i->ea, osz); }
      int bit = (int)((v >> b) & 1); c->rflags = (c->rflags & ~(u64)F_CF) | (bit ? F_CF : 0);
      if (op == 0xA3) return;
      if (op == 0xAB) v |= 1ull << b; else if (op == 0xB3) v &= ~(1ull << b); else v ^= 1ull << b;
      if (i->mod == 3) set_reg(i, i->rm, osz, v); else wrn(c, i->ea, osz, v); return; }
    case 0xBA: { decode_modrm(i); fix_rip_rel(i, 1); u8 b = fetch8(i); int sub = i->reg & 7; u64 v = rm_get(i, osz); b &= osz * 8 - 1; int bit = (int)((v >> b) & 1); c->rflags = (c->rflags & ~(u64)F_CF) | (bit ? F_CF : 0);
      if (sub == 4) return; if (sub == 5) v |= 1ull << b; else if (sub == 6) v &= ~(1ull << b); else if (sub == 7) v ^= 1ull << b; else UNSUP("0F BA /%d", sub); rm_set(i, osz, v); return; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: { decode_modrm(i); fix_rip_rel(i, (op & 1) ? 0 : 1); int cnt = (op & 1) ? (int)(c->r[RCX] & 0xff) : fetch8(i); int bits = osz * 8; cnt &= (osz == 8 ? 63 : 31); if (!cnt) return;
      u64 d = rm_get(i, osz), s = get_reg(i, i->reg, osz), r;
      if (op < 0xA8) { r = (d << cnt) | (s >> (bits - cnt)); c->rflags = (c->rflags & ~(u64)F_CF) | (((d >> (bits - cnt)) & 1) ? F_CF : 0); }
      else { r = (d >> cnt) | (s << (bits - cnt)); c->rflags = (c->rflags & ~(u64)F_CF) | (((d >> (cnt - 1)) & 1) ? F_CF : 0); }
      r &= mask(osz); set_szp(c, r, osz); rm_set(i, osz, r); return; }
    case 0xAF: { decode_modrm(i); s64 a = (s64)sext(get_reg(i, i->reg, osz), osz), b = (s64)sext(rm_get(i, osz), osz); s128 p = (s128)a * b; u64 r = (u64)p & mask(osz);
      int ov = osz == 8 ? (p != (s128)(s64)p) : ((s64)sext(r, osz) != (s64)p); c->rflags &= ~(u64)(F_CF | F_OF); if (ov) c->rflags |= F_CF | F_OF; set_szp(c, r, osz); set_reg(i, i->reg, osz, r); return; }
    case 0xB0: case 0xB1: { int sz = op & 1 ? osz : 1; decode_modrm(i); u64 d = rm_get(i, sz), a = get_reg(i, RAX, sz); flags_sub(c, a, d, (a - d) & mask(sz), sz);
      if (a == d) { c->rflags |= F_ZF; rm_set(i, sz, get_reg(i, i->reg, sz)); } else { c->rflags &= ~(u64)F_ZF; set_reg(i, RAX, sz, d); } return; }
    case 0xC0: case 0xC1: { int sz = op & 1 ? osz : 1; decode_modrm(i); u64 d = rm_get(i, sz), s = get_reg(i, i->reg, sz); u64 r = alu(c, 0, d, s, sz); set_reg(i, i->reg, sz, d); rm_set(i, sz, r); return; }
    case 0xC7: { decode_modrm(i); if (i->reg == 1 && i->mod != 3) { /* CMPXCHG8B m64 / CMPXCHG16B m128 */ u64 a = EA(i);
        if (i->rexw) { u64 lo = rd64(c, a), hi = rd64(c, a + 8); if (lo == c->r[RAX] && hi == c->r[RDX]) { c->rflags |= F_ZF; wr64(c, a, c->r[RBX]); wr64(c, a + 8, c->r[RCX]); } else { c->rflags &= ~(u64)F_ZF; c->r[RAX] = lo; c->r[RDX] = hi; } return; }
        u64 m = rd64(c, a), cmp = (u64)(u32)c->r[RAX] | (u64)(u32)c->r[RDX] << 32; if (m == cmp) { c->rflags |= F_ZF; wr64(c, a, (u64)(u32)c->r[RBX] | (u64)(u32)c->r[RCX] << 32); } else { c->rflags &= ~(u64)F_ZF; c->r[RAX] = (u32)m; c->r[RDX] = (u32)(m >> 32); } return; }
      if (i->reg == 6 && i->mod == 3) { /* RDRAND r */ u64 v = (u64)host_now_ms() * 2654435761u ^ (c->r[RSP] << 7) ^ 0x9E3779B97F4A7C15ull; set_reg(i, i->rm, osz, v); c->rflags = (c->rflags & ~(u64)(F_OF | F_SF | F_ZF | F_AF | F_PF)) | F_CF; return; }
      if (i->reg == 7 && i->mod == 3) { /* RDSEED r */ u64 v = (u64)host_now_ms() * 0x9E3779B97F4A7C15ull ^ (c->r[RSP] << 3); set_reg(i, i->rm, osz, v); c->rflags = (c->rflags & ~(u64)(F_OF | F_SF | F_ZF | F_AF | F_PF)) | F_CF; return; }
      break; }
    case 0xB6: case 0xB7: { decode_modrm(i); set_reg(i, i->reg, osz, rm_get(i, op == 0xB6 ? 1 : 2)); return; }
    case 0xBE: case 0xBF: { decode_modrm(i); int ssz = op == 0xBE ? 1 : 2; set_reg(i, i->reg, osz, sext(rm_get(i, ssz), ssz) & mask(osz)); return; }
    case 0xBC: case 0xBD: { decode_modrm(i); u64 v = rm_get(i, osz); int bits = osz * 8;
      if (i->rep == 0xF3) { /* tzcnt / lzcnt */ int n; if (!v) n = bits; else if (op == 0xBC) n = __builtin_ctzll(v); else n = __builtin_clzll(v) - (64 - bits);
        c->rflags &= ~(u64)(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF); if (!v) c->rflags |= F_CF; if (!n) c->rflags |= F_ZF; set_reg(i, i->reg, osz, (u64)n); return; }   /* OF/SF/PF/AF: undefined, hardware clears them */
      if (!v) { c->rflags |= F_ZF; return; }
      c->rflags &= ~(u64)F_ZF; int n = op == 0xBC ? __builtin_ctzll(v) : 63 - __builtin_clzll(v); set_reg(i, i->reg, osz, (u64)n); return; }
    case 0xB8: { decode_modrm(i); u64 v = rm_get(i, osz); int n = __builtin_popcountll(v); c->rflags &= ~(u64)(F_CF | F_OF | F_SF | F_ZF | F_AF | F_PF); if (!v) c->rflags |= F_ZF; set_reg(i, i->reg, osz, (u64)n); return; }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: { int r = (op & 7) | (i->rexb << 3); if (osz == 8) c->r[r] = __builtin_bswap64(c->r[r]); else c->r[r] = __builtin_bswap32((u32)c->r[r]); return; }
    case 0xAE: { decode_modrm(i); int sub = i->reg & 7;
      if (i->mod == 3) return;                                              /* lfence/mfence/sfence */
      if (sub == 2) { c->mxcsr = rd32(c, EA(i)); return; } if (sub == 3) { wr32(c, EA(i), c->mxcsr); return; }
      if (sub == 0 || sub == 1 || sub == 4 || sub == 5) { /* fxsave/fxrstor/xsave/xrstor: minimal — save/restore xmm block */ u64 a = EA(i);
        if (sub == 0 || sub == 4) { for (int k = 0; k < 16; k++) { wr64(c, a + 160 + k * 16, c->x[k].q[0]); wr64(c, a + 168 + k * 16, c->x[k].q[1]); } wr32(c, a + 24, c->mxcsr); }
        else { for (int k = 0; k < 16; k++) { c->x[k].q[0] = rd64(c, a + 160 + k * 16); c->x[k].q[1] = rd64(c, a + 168 + k * 16); } c->mxcsr = rd32(c, a + 24); } return; }
      UNSUP("0F AE /%d", sub); }

    /* ---------------- SSE ---------------- */
    case 0x10: case 0x11: { decode_modrm(i); Xmm v;
      int bytes = i->rep == 0xF3 ? 4 : i->rep == 0xF2 ? 8 : 16;
      if (op == 0x10) { if (bytes == 16) { xmm_load(i, &v, 16); c->x[i->reg] = v; } else { xmm_load(i, &v, bytes); if (i->mod == 3) { if (bytes == 4) c->x[i->reg].d[0] = v.d[0]; else c->x[i->reg].q[0] = v.q[0]; } else { c->x[i->reg].q[1] = 0; if (bytes == 4) { c->x[i->reg].q[0] = 0; c->x[i->reg].d[0] = v.d[0]; } else c->x[i->reg].q[0] = v.q[0]; } } }
      else xmm_store(i, &c->x[i->reg], bytes);
      return; }
    case 0x12: case 0x13: { decode_modrm(i); /* movlps/movlpd (mem) ; movhlps (reg) ; F2: movddup ; F3: movsldup */
      if (op == 0x12 && i->rep == 0xF2) { u64 v = i->mod == 3 ? c->x[i->rm].q[0] : rd64(c, EA(i)); c->x[i->reg].q[0] = c->x[i->reg].q[1] = v; return; }
      if (op == 0x12 && i->rep == 0xF3) { Xmm v; xmm_load(i, &v, 16); c->x[i->reg].d[0] = c->x[i->reg].d[1] = v.d[0]; c->x[i->reg].d[2] = c->x[i->reg].d[3] = v.d[2]; return; }
      if (op == 0x12) { if (i->mod == 3 && !i->p66) c->x[i->reg].q[0] = c->x[i->rm].q[1]; else c->x[i->reg].q[0] = rd64(c, EA(i)); }
      else wr64(c, EA(i), c->x[i->reg].q[0]); return; }
    case 0x16: case 0x17: { decode_modrm(i); if (op == 0x16 && i->rep == 0xF3) { Xmm v; xmm_load(i, &v, 16); c->x[i->reg].d[0] = c->x[i->reg].d[1] = v.d[1]; c->x[i->reg].d[2] = c->x[i->reg].d[3] = v.d[3]; return; }   /* movshdup */
      if (op == 0x16) { if (i->mod == 3 && !i->p66) c->x[i->reg].q[1] = c->x[i->rm].q[0]; else c->x[i->reg].q[1] = rd64(c, EA(i)); } else wr64(c, EA(i), c->x[i->reg].q[1]); return; }
    case 0x14: case 0x15: { decode_modrm(i); Xmm s; xmm_load(i, &s, 16); Xmm *d = &c->x[i->reg], r;
      if (i->p66) { if (op == 0x14) { r.q[0] = d->q[0]; r.q[1] = s.q[0]; } else { r.q[0] = d->q[1]; r.q[1] = s.q[1]; } }
      else if (op == 0x14) { r.d[0] = d->d[0]; r.d[1] = s.d[0]; r.d[2] = d->d[1]; r.d[3] = s.d[1]; } else { r.d[0] = d->d[2]; r.d[1] = s.d[2]; r.d[2] = d->d[3]; r.d[3] = s.d[3]; }
      *d = r; return; }
    case 0x28: case 0x29: case 0x2B: { decode_modrm(i); if (op == 0x28) { Xmm v; xmm_load(i, &v, 16); c->x[i->reg] = v; } else xmm_store(i, &c->x[i->reg], 16); return; }
    case 0x2A: { decode_modrm(i); /* cvtsi2ss/sd */ s64 v = i->mod == 3 ? (s64)sext(get_reg(i, i->rm, i->rexw ? 8 : 4), i->rexw ? 8 : 4) : (s64)sext(rdn(c, EA(i), i->rexw ? 8 : 4), i->rexw ? 8 : 4);
      if (i->rep == 0xF2) c->x[i->reg].f64[0] = (double)v; else c->x[i->reg].f32[0] = (float)v; return; }
    case 0x2C: case 0x2D: { decode_modrm(i); Xmm s; xmm_load(i, &s, i->rep == 0xF2 ? 8 : 4); double v = i->rep == 0xF2 ? s.f64[0] : (double)s.f32[0];
      if (i->rexw) c->r[i->reg] = (u64)f2i64(v, op == 0x2C); else c->r[i->reg] = (u32)f2i32(v, op == 0x2C); return; }
    case 0x2E: case 0x2F: { decode_modrm(i); Xmm s; xmm_load(i, &s, i->p66 ? 8 : 4); double a = i->p66 ? c->x[i->reg].f64[0] : (double)c->x[i->reg].f32[0], b = i->p66 ? s.f64[0] : (double)s.f32[0]; set_cmp_flags(c, a, b); return; }
    case 0x50: { decode_modrm(i); Xmm *s = &c->x[i->rm]; u64 r = 0; if (i->p66) { r = ((s->q[0] >> 63) & 1) | (((s->q[1] >> 63) & 1) << 1); } else for (int k = 0; k < 4; k++) r |= ((s->d[k] >> 31) & 1) << k; c->r[i->reg] = r; return; }
    case 0x51: { decode_modrm(i); Xmm s, *d = &c->x[i->reg];
      if (i->rep == 0xF3) { xmm_load(i, &s, 4); d->f32[0] = __builtin_sqrtf(s.f32[0]); } else if (i->rep == 0xF2) { xmm_load(i, &s, 8); d->f64[0] = __builtin_sqrt(s.f64[0]); }
      else { xmm_load(i, &s, 16); if (i->p66) { d->f64[0] = __builtin_sqrt(s.f64[0]); d->f64[1] = __builtin_sqrt(s.f64[1]); } else for (int k = 0; k < 4; k++) d->f32[k] = __builtin_sqrtf(s.f32[k]); } return; }
    case 0x52: case 0x53: { decode_modrm(i); Xmm s, *d = &c->x[i->reg];   /* RSQRTPS/SS, RCPPS/SS: approximations (D3DX math uses them with a Newton step, so exact values are fine) */
      if (i->rep == 0xF3) { xmm_load(i, &s, 4); d->f32[0] = op == 0x52 ? 1.0f / __builtin_sqrtf(s.f32[0]) : 1.0f / s.f32[0]; }
      else { xmm_load(i, &s, 16); for (int k = 0; k < 4; k++) d->f32[k] = op == 0x52 ? 1.0f / __builtin_sqrtf(s.f32[k]) : 1.0f / s.f32[k]; } return; }
    case 0x54: case 0x55: case 0x56: case 0x57: { decode_modrm(i); Xmm s; xmm_load(i, &s, 16); Xmm *d = &c->x[i->reg];
      for (int k = 0; k < 2; k++) d->q[k] = op == 0x54 ? d->q[k] & s.q[k] : op == 0x55 ? ~d->q[k] & s.q[k] : op == 0x56 ? d->q[k] | s.q[k] : d->q[k] ^ s.q[k]; return; }
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: { decode_modrm(i); Xmm s, *d = &c->x[i->reg]; int sc = i->rep == 0xF3 ? 1 : i->rep == 0xF2 ? 2 : i->p66 ? 3 : 0; /* ss sd pd ps */
      xmm_load(i, &s, sc == 1 ? 4 : sc == 2 ? 8 : 16);
#define FOP(a, b) (op == 0x58 ? (a) + (b) : op == 0x59 ? (a) * (b) : op == 0x5C ? (a) - (b) : op == 0x5E ? (a) / (b) : fminmax((a), (b), op == 0x5F))
      if (sc == 1) d->f32[0] = (float)FOP((double)d->f32[0], (double)s.f32[0]);
      else if (sc == 2) d->f64[0] = FOP(d->f64[0], s.f64[0]);
      else if (sc == 3) { d->f64[0] = FOP(d->f64[0], s.f64[0]); d->f64[1] = FOP(d->f64[1], s.f64[1]); }
      else for (int k = 0; k < 4; k++) d->f32[k] = (float)FOP((double)d->f32[k], (double)s.f32[k]);
#undef FOP
      return; }
    case 0x5A: { decode_modrm(i); Xmm s, *d = &c->x[i->reg];
      if (i->rep == 0xF3) { xmm_load(i, &s, 4); d->f64[0] = (double)s.f32[0]; }
      else if (i->rep == 0xF2) { xmm_load(i, &s, 8); d->f32[0] = (float)s.f64[0]; }
      else if (i->p66) { xmm_load(i, &s, 16); d->f32[0] = (float)s.f64[0]; d->f32[1] = (float)s.f64[1]; d->q[1] = 0; }
      else { xmm_load(i, &s, 8); d->f64[1] = (double)s.f32[1]; d->f64[0] = (double)s.f32[0]; } return; }
    case 0x5B: { decode_modrm(i); Xmm s, *d = &c->x[i->reg]; xmm_load(i, &s, 16);
      if (i->p66) for (int k = 0; k < 4; k++) d->d[k] = (u32)f2i32((double)s.f32[k], 0);                 /* 66 0F 5B cvtps2dq (round) */
      else if (i->rep == 0xF3) for (int k = 0; k < 4; k++) d->d[k] = (u32)f2i32((double)s.f32[k], 1);     /* F3 0F 5B cvttps2dq (truncate) */
      else for (int k = 0; k < 4; k++) d->f32[k] = (float)(s32)s.d[k];                                       /* cvtdq2ps */
      return; }
    case 0xE6: { decode_modrm(i); Xmm s, *d = &c->x[i->reg];
      if (i->rep == 0xF3) { xmm_load(i, &s, 8); d->f64[0] = (double)(s32)s.d[0]; d->f64[1] = (double)(s32)s.d[1]; }        /* cvtdq2pd */
      else { xmm_load(i, &s, 16); d->d[0] = (u32)f2i32(s.f64[0], i->p66); d->d[1] = (u32)f2i32(s.f64[1], i->p66); d->q[1] = 0; } /* cvt(t)pd2dq */
      return; }
    case 0xC2: { decode_modrm(i); int sc = i->rep == 0xF3 ? 1 : i->rep == 0xF2 ? 2 : i->p66 ? 3 : 0; fix_rip_rel(i, 1); Xmm s, *d = &c->x[i->reg]; xmm_load(i, &s, sc == 1 ? 4 : sc == 2 ? 8 : 16); u8 pred = fetch8(i);
      if (sc == 1) d->d[0] = cmp_pred(d->f32[0], s.f32[0], pred) ? 0xffffffff : 0;
      else if (sc == 2) d->q[0] = cmp_pred(d->f64[0], s.f64[0], pred) ? ~0ull : 0;
      else if (sc == 3) { for (int k = 0; k < 2; k++) d->q[k] = cmp_pred(d->f64[k], s.f64[k], pred) ? ~0ull : 0; }
      else for (int k = 0; k < 4; k++) d->d[k] = cmp_pred(d->f32[k], s.f32[k], pred) ? 0xffffffff : 0; return; }
    case 0xC6: { decode_modrm(i); fix_rip_rel(i, 1); Xmm s, *d = &c->x[i->reg], r; xmm_load(i, &s, 16); u8 im = fetch8(i);
      if (i->p66) { r.q[0] = d->q[im & 1]; r.q[1] = s.q[(im >> 1) & 1]; } else { r.d[0] = d->d[im & 3]; r.d[1] = d->d[(im >> 2) & 3]; r.d[2] = s.d[(im >> 4) & 3]; r.d[3] = s.d[(im >> 6) & 3]; }
      *d = r; return; }
    case 0x60: case 0x61: case 0x62: case 0x68: case 0x69: case 0x6A: case 0x6C: case 0x6D: { decode_modrm(i); Xmm s, *d = &c->x[i->reg], r; xmm_load(i, &s, 16); int hi = op == 0x6C ? 0 : op == 0x6D ? 1 : op >= 0x68;   /* punpckl* 60-62/6C, punpckh* 68-6A/6D */
      switch (op & 7) { case 0: for (int k = 0; k < 8; k++) { r.b[2 * k] = d->b[k + hi * 8]; r.b[2 * k + 1] = s.b[k + hi * 8]; } break;   /* punpck bw */
        case 1: for (int k = 0; k < 4; k++) { r.w[2 * k] = d->w[k + hi * 4]; r.w[2 * k + 1] = s.w[k + hi * 4]; } break;                      /* wd */
        case 2: for (int k = 0; k < 2; k++) { r.d[2 * k] = d->d[k + hi * 2]; r.d[2 * k + 1] = s.d[k + hi * 2]; } break;                      /* dq */
        default: r.q[0] = d->q[hi]; r.q[1] = s.q[hi]; break; }                                                                                   /* qdq */
      *d = r; return; }
    case 0x63: case 0x67: case 0x6B: { decode_modrm(i); Xmm s, *d = &c->x[i->reg], r; xmm_load(i, &s, 16);
      if (op == 0x63) { for (int k = 0; k < 8; k++) { s16 a = (s16)d->w[k], b = (s16)s.w[k]; r.b[k] = (u8)(a < -128 ? -128 : a > 127 ? 127 : a); r.b[k + 8] = (u8)(b < -128 ? -128 : b > 127 ? 127 : b); } }
      else if (op == 0x67) { for (int k = 0; k < 8; k++) { s16 a = (s16)d->w[k], b = (s16)s.w[k]; r.b[k] = (u8)(a < 0 ? 0 : a > 255 ? 255 : a); r.b[k + 8] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b); } }
      else { for (int k = 0; k < 4; k++) { s32 a = (s32)d->d[k], b = (s32)s.d[k]; r.w[k] = (u16)(a < -32768 ? -32768 : a > 32767 ? 32767 : a); r.w[k + 4] = (u16)(b < -32768 ? -32768 : b > 32767 ? 32767 : b); } }
      *d = r; return; }
    case 0x64: case 0x65: case 0x66: case 0x74: case 0x75: case 0x76: { decode_modrm(i); Xmm s, *d = &c->x[i->reg]; xmm_load(i, &s, 16); int gt = op < 0x70;
      if ((op & 3) == 0) for (int k = 0; k < 16; k++) d->b[k] = (gt ? (s8)d->b[k] > (s8)s.b[k] : d->b[k] == s.b[k]) ? 0xff : 0;
      else if ((op & 3) == 1) for (int k = 0; k < 8; k++) d->w[k] = (gt ? (s16)d->w[k] > (s16)s.w[k] : d->w[k] == s.w[k]) ? 0xffff : 0;
      else for (int k = 0; k < 4; k++) d->d[k] = (gt ? (s32)d->d[k] > (s32)s.d[k] : d->d[k] == s.d[k]) ? 0xffffffff : 0; return; }
    case 0x6E: { decode_modrm(i); Xmm *d = &c->x[i->reg]; d->q[1] = 0; if (i->rexw) d->q[0] = rm_get(i, 8); else { d->q[0] = 0; d->d[0] = (u32)rm_get(i, 4); } return; }   /* movd/movq xmm, r/m */
    case 0x7E: { decode_modrm(i); if (i->rep == 0xF3) { Xmm s; xmm_load(i, &s, 8); c->x[i->reg].q[0] = s.q[0]; c->x[i->reg].q[1] = 0; return; } /* movq xmm, xmm/m64 */
      if (i->rexw) rm_set(i, 8, c->x[i->reg].q[0]); else rm_set(i, 4, c->x[i->reg].d[0]); return; }
    case 0x6F: { decode_modrm(i); Xmm v; xmm_load(i, &v, 16); c->x[i->reg] = v; return; }        /* movdqa/movdqu */
    case 0x7F: { decode_modrm(i); xmm_store(i, &c->x[i->reg], 16); return; }
    case 0xD6: { decode_modrm(i); xmm_store(i, &c->x[i->reg], 8); if (i->mod == 3) c->x[i->rm].q[1] = 0; return; }   /* movq xmm/m64, xmm */
    case 0xE7: { decode_modrm(i); xmm_store(i, &c->x[i->reg], 16); return; }                     /* movntdq */
    case 0x70: { decode_modrm(i); fix_rip_rel(i, 1); Xmm s, *d = &c->x[i->reg], r; xmm_load(i, &s, 16); u8 im = fetch8(i);
      if (i->p66) { for (int k = 0; k < 4; k++) r.d[k] = s.d[(im >> (2 * k)) & 3]; }                                      /* pshufd */
      else if (i->rep == 0xF2) { r = s; for (int k = 0; k < 4; k++) r.w[k] = s.w[(im >> (2 * k)) & 3]; }                /* pshuflw */
      else if (i->rep == 0xF3) { r = s; for (int k = 0; k < 4; k++) r.w[4 + k] = s.w[4 + ((im >> (2 * k)) & 3)]; }      /* pshufhw */
      else UNSUP("pshufw (mmx)");
      *d = r; return; }
    case 0x71: case 0x72: case 0x73: { decode_modrm(i); u8 im = fetch8(i); Xmm *d = &c->x[i->rm]; int sub = i->reg & 7;
      if (op == 0x71) { for (int k = 0; k < 8; k++) d->w[k] = sub == 2 ? (im > 15 ? 0 : d->w[k] >> im) : sub == 4 ? (u16)((s16)d->w[k] >> (im > 15 ? 15 : im)) : (im > 15 ? 0 : (u16)(d->w[k] << im)); }
      else if (op == 0x72) { for (int k = 0; k < 4; k++) d->d[k] = sub == 2 ? (im > 31 ? 0 : d->d[k] >> im) : sub == 4 ? (u32)((s32)d->d[k] >> (im > 31 ? 31 : im)) : (im > 31 ? 0 : d->d[k] << im); }
      else { if (sub == 2) for (int k = 0; k < 2; k++) d->q[k] = im > 63 ? 0 : d->q[k] >> im; else if (sub == 6) for (int k = 0; k < 2; k++) d->q[k] = im > 63 ? 0 : d->q[k] << im;
        else if (sub == 3) { /* psrldq */ Xmm r = { { 0, 0 } }; for (int k = 0; k + im < 16; k++) r.b[k] = d->b[k + im]; *d = r; }
        else if (sub == 7) { /* pslldq */ Xmm r = { { 0, 0 } }; for (int k = im; k < 16; k++) r.b[k] = d->b[k - im]; *d = r; }
        else UNSUP("0F 73 /%d", sub); }
      return; }
    case 0xD4: case 0xFB: case 0xFE: case 0xFA: case 0xFD: case 0xF9: case 0xFC: case 0xF8: case 0xD5: case 0xE5: case 0xF4: case 0xDA: case 0xDE: case 0xEA: case 0xEE: case 0xE0: case 0xE3: case 0xD8: case 0xD9: case 0xDC: case 0xDD: case 0xEC: case 0xED: case 0xE8: case 0xE9: case 0xF5: case 0xF6: case 0xE4: {
      decode_modrm(i); Xmm s, *d = &c->x[i->reg]; xmm_load(i, &s, 16);
      switch (op) {
        case 0xD4: d->q[0] += s.q[0]; d->q[1] += s.q[1]; break; case 0xFB: d->q[0] -= s.q[0]; d->q[1] -= s.q[1]; break;
        case 0xFE: for (int k = 0; k < 4; k++) d->d[k] += s.d[k]; break; case 0xFA: for (int k = 0; k < 4; k++) d->d[k] -= s.d[k]; break;
        case 0xFD: for (int k = 0; k < 8; k++) d->w[k] += s.w[k]; break; case 0xF9: for (int k = 0; k < 8; k++) d->w[k] -= s.w[k]; break;
        case 0xFC: for (int k = 0; k < 16; k++) d->b[k] += s.b[k]; break; case 0xF8: for (int k = 0; k < 16; k++) d->b[k] -= s.b[k]; break;
        case 0xD5: for (int k = 0; k < 8; k++) d->w[k] = (u16)(d->w[k] * s.w[k]); break;
        case 0xE5: for (int k = 0; k < 8; k++) d->w[k] = (u16)(((s32)(s16)d->w[k] * (s32)(s16)s.w[k]) >> 16); break;
        case 0xE4: for (int k = 0; k < 8; k++) d->w[k] = (u16)(((u32)d->w[k] * (u32)s.w[k]) >> 16); break;
        case 0xF4: d->q[0] = (u64)d->d[0] * s.d[0]; d->q[1] = (u64)d->d[2] * s.d[2]; break;
        case 0xDA: for (int k = 0; k < 16; k++) d->b[k] = d->b[k] < s.b[k] ? d->b[k] : s.b[k]; break; case 0xDE: for (int k = 0; k < 16; k++) d->b[k] = d->b[k] > s.b[k] ? d->b[k] : s.b[k]; break;
        case 0xEA: for (int k = 0; k < 8; k++) d->w[k] = (s16)d->w[k] < (s16)s.w[k] ? d->w[k] : s.w[k]; break; case 0xEE: for (int k = 0; k < 8; k++) d->w[k] = (s16)d->w[k] > (s16)s.w[k] ? d->w[k] : s.w[k]; break;
        case 0xE0: for (int k = 0; k < 16; k++) d->b[k] = (u8)((d->b[k] + s.b[k] + 1) >> 1); break; case 0xE3: for (int k = 0; k < 8; k++) d->w[k] = (u16)((d->w[k] + s.w[k] + 1) >> 1); break;
        case 0xD8: for (int k = 0; k < 16; k++) d->b[k] = d->b[k] > s.b[k] ? d->b[k] - s.b[k] : 0; break; case 0xDC: for (int k = 0; k < 16; k++) { u32 t = d->b[k] + s.b[k]; d->b[k] = t > 255 ? 255 : (u8)t; } break;
        case 0xD9: for (int k = 0; k < 8; k++) d->w[k] = d->w[k] > s.w[k] ? d->w[k] - s.w[k] : 0; break; case 0xDD: for (int k = 0; k < 8; k++) { u32 t = d->w[k] + s.w[k]; d->w[k] = t > 65535 ? 65535 : (u16)t; } break;
        case 0xEC: for (int k = 0; k < 16; k++) { s32 t = (s8)d->b[k] + (s8)s.b[k]; d->b[k] = (u8)(t < -128 ? -128 : t > 127 ? 127 : t); } break; case 0xED: for (int k = 0; k < 8; k++) { s32 t = (s16)d->w[k] + (s16)s.w[k]; d->w[k] = (u16)(t < -32768 ? -32768 : t > 32767 ? 32767 : t); } break;
        case 0xE8: for (int k = 0; k < 16; k++) { s32 t = (s8)d->b[k] - (s8)s.b[k]; d->b[k] = (u8)(t < -128 ? -128 : t > 127 ? 127 : t); } break; case 0xE9: for (int k = 0; k < 8; k++) { s32 t = (s16)d->w[k] - (s16)s.w[k]; d->w[k] = (u16)(t < -32768 ? -32768 : t > 32767 ? 32767 : t); } break;
        case 0xF5: for (int k = 0; k < 4; k++) d->d[k] = (u32)((s32)(s16)d->w[2 * k] * (s16)s.w[2 * k] + (s32)(s16)d->w[2 * k + 1] * (s16)s.w[2 * k + 1]); break;
        case 0xF6: { u32 a = 0, b = 0; for (int k = 0; k < 8; k++) { a += d->b[k] > s.b[k] ? d->b[k] - s.b[k] : s.b[k] - d->b[k]; b += d->b[k + 8] > s.b[k + 8] ? d->b[k + 8] - s.b[k + 8] : s.b[k + 8] - d->b[k + 8]; } d->q[0] = a; d->q[1] = b; break; }
      }
      return; }
    case 0xDB: case 0xDF: case 0xEB: case 0xEF: { decode_modrm(i); Xmm s, *d = &c->x[i->reg]; xmm_load(i, &s, 16);
      for (int k = 0; k < 2; k++) d->q[k] = op == 0xDB ? d->q[k] & s.q[k] : op == 0xDF ? ~d->q[k] & s.q[k] : op == 0xEB ? d->q[k] | s.q[k] : d->q[k] ^ s.q[k]; return; }
    case 0xD7: { decode_modrm(i); Xmm *s = &c->x[i->rm]; u32 r = 0; for (int k = 0; k < 16; k++) r |= (u32)(s->b[k] >> 7) << k; c->r[i->reg] = r; return; }
    case 0xC4: { decode_modrm(i); fix_rip_rel(i, 1); u64 v = rm_get(i, 2); u8 im = fetch8(i); c->x[i->reg].w[im & 7] = (u16)v; return; }      /* pinsrw */
    case 0xC5: { decode_modrm(i); u8 im = fetch8(i); c->r[i->reg] = c->x[i->rm].w[im & 7]; return; }                                        /* pextrw */
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: { decode_modrm(i); Xmm s, *d = &c->x[i->reg]; xmm_load(i, &s, 16); u64 cnt = s.q[0];
      if (op == 0xD1) for (int k = 0; k < 8; k++) d->w[k] = cnt > 15 ? 0 : d->w[k] >> cnt; else if (op == 0xD2) for (int k = 0; k < 4; k++) d->d[k] = cnt > 31 ? 0 : d->d[k] >> cnt; else if (op == 0xD3) for (int k = 0; k < 2; k++) d->q[k] = cnt > 63 ? 0 : d->q[k] >> cnt;
      else if (op == 0xE1) for (int k = 0; k < 8; k++) d->w[k] = (u16)((s16)d->w[k] >> (cnt > 15 ? 15 : cnt)); else if (op == 0xE2) for (int k = 0; k < 4; k++) d->d[k] = (u32)((s32)d->d[k] >> (cnt > 31 ? 31 : cnt));
      else if (op == 0xF1) for (int k = 0; k < 8; k++) d->w[k] = cnt > 15 ? 0 : (u16)(d->w[k] << cnt); else if (op == 0xF2) for (int k = 0; k < 4; k++) d->d[k] = cnt > 31 ? 0 : d->d[k] << cnt; else for (int k = 0; k < 2; k++) d->q[k] = cnt > 63 ? 0 : d->q[k] << cnt;
      return; }
    case 0x38: op_0f38(i); return;
    case 0x3A: op_0f3a(i); return;
    case 0x7C: case 0x7D: case 0xD0: if (!i->p66 && i->rep != 0xF2) UNSUP("0F %02X without SSE3 prefix", op); op_sse3_h(i, op); return;   /* haddp*, hsubp*, addsubp* */
    case 0xF0: { if (i->rep != 0xF2) UNSUP("0F F0 (not lddqu)"); decode_modrm(i); Xmm v; xmm_load(i, &v, 16); c->x[i->reg] = v; return; }   /* lddqu */
    default: c->unknown_op[0] = 0x0F; c->unknown_op[1] = op; UNSUP("unsupported opcode 0F %02X", op);
  }
}

int cpu_run(Cpu *c, u64 max_insns) {
  c->status = RUN_OK;
  for (u64 n = 0; n < max_insns; n++) {
    /* import trampolines live in the shim region: dispatch to the Win32 personality */
    if (c->rip - G_SHIM_BASE < G_SHIM_SIZE) {
      int r = w32_dispatch(c, c->rip);
      if (r != RUN_OK) { c->status = r; return r; }
      if (c->status != RUN_OK) return c->status;
      continue;
    }
#ifdef X64_WATCH
    if (c->rip == X64_WATCH || c->rip == X64_WATCH2) { e_log("[watch] rip=%llx tid=%d rax=%llx rbx=%llx rsp=%llx rbp=%llx rsi=%llx rdi=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n", (unsigned long long)c->rip, c->w->th[c->w->cur_th].tid, (unsigned long long)c->r[0], (unsigned long long)c->r[3], (unsigned long long)c->r[4], (unsigned long long)c->r[5], (unsigned long long)c->r[6], (unsigned long long)c->r[7], (unsigned long long)c->r[12], (unsigned long long)c->r[13], (unsigned long long)c->r[14], (unsigned long long)c->r[15]);
      u8 *sp = mem_ptr(c->mem, c->r[4], 64, 0); if (sp) { e_log("        stack:"); for (int k = 0; k < 8; k++) { u64 v; e_memcpy(&v, sp + 8 * k, 8); e_log(" %llx", (unsigned long long)v); } e_log("\n"); } }
#endif
    if (__builtin_expect(g_dwatch != 0, 0)) { u32 v = watch_rd(c, g_dwatch); if (v != g_dwatch_last) { e_log("[dwatch] [%llx] %x -> %x after rip=%llx (next %llx)\n", (unsigned long long)g_dwatch, g_dwatch_last, v, (unsigned long long)g_dwatch_rip, (unsigned long long)c->rip); g_dwatch_last = v; } g_dwatch_rip = c->rip; }
    if (__builtin_expect(g_watch_n, 0)) { for (int wi = 0; wi < g_watch_n; wi++) if (g_watch[wi] == c->rip) { e_log("[watch] rip=%llx eax=%llx ecx=%llx edx=%llx ebx=%llx esp=%llx ebp=%llx esi=%llx edi=%llx [esi]=%x [edx]=%x [eax]=%x\n", (unsigned long long)c->rip, (unsigned long long)c->r[RAX], (unsigned long long)c->r[RCX], (unsigned long long)c->r[RDX], (unsigned long long)c->r[RBX], (unsigned long long)c->r[RSP], (unsigned long long)c->r[RBP], (unsigned long long)c->r[RSI], (unsigned long long)c->r[RDI], watch_rd(c, c->r[RSI]), watch_rd(c, c->r[RDX]), watch_rd(c, c->r[RAX])); break; } }
    Dc *e = DC_SLOT(c->rip); u64 w0 = 0, w1 = 0;
    const u8 *code = (e->rip == c->rip && e->mem == c->mem && e->gen == c->mem->gen) ? e->code : mem_ptr_fast(c->mem, c->rip, 16, 0);
    if (code) { __builtin_memcpy(&w0, code, 8); __builtin_memcpy(&w1, code + 8, 8); }
    int hit = e->rip == c->rip && code && (w0 & e->m0) == e->b0 && (w1 & e->m1) == e->b1 && e->m32 == c->mode32;
    if (hit && e->fast) {   /* specialised handler: no Ins at all */
      u64 start = c->rip, nx = fast_exec(c, e, code, start);
      if (nx) { if (c->status != RUN_OK) { c->hist[c->hist_i++ & 4095] = start; if (c->status == RUN_FAULT) { c->err_rip = start; if (c->exc_code) { c->rip = start; if (w32_hw_exception(c)) continue; } } return c->status; }
        if (nx != start + e->len) c->hist[c->hist_i++ & 4095] = nx;   /* history keeps block starts (branch targets); straight-line code is implied */
        c->rip = nx; c->icount++; if (c->trace) e_log("%llx\n", (unsigned long long)c->rip); continue; }
    }
    c->hist[c->hist_i++ & 4095] = c->rip;
    Ins in; in.c = c; in.start = c->rip; in.ip = c->rip; in.fend = c->rip; in.code = code;
    in.modrm = 0; in.ea_valid = 0; in.mod = in.reg = in.rm = 0; in.ea = 0;
    if (hit) {
      in.dc = e; in.rex = e->rex; in.rexw = e->rexw; in.rexr = e->rexr; in.rexx = e->rexx; in.rexb = e->rexb; in.p66 = e->p66; in.p67 = e->p67; in.rep = e->rep; in.seg = e->seg; in.lock = e->lock;
      in.ip = c->rip + e->hdr; in.fend = c->rip + e->len;
#ifdef X64_OPHIST
      { int k = e->op == 0x0F ? 256 + code[e->hdr] : e->op; g_ophist[k][e->modrm && e->mod != 3]++; g_ophist[k][2] += e->rexw; g_ophist[k][3] += e->p66; }
#endif
      exec_body(&in, e->op);
    } else {
      in.dc = 0; in.rex = in.rexw = in.rexr = in.rexx = in.rexb = in.p66 = in.p67 = in.rep = in.seg = in.lock = 0; in.hdr = 0; in.mr_end = 0; in.base = in.idx = -1; in.scale = in.riprel = 0; in.disp = 0;
      exec_one(&in);
      if (c->status == RUN_OK && code) {   /* remember the decode (instruction fully inside the 16 fetched bytes) */
        u64 len = in.fend - in.start;
        if (len >= 1 && len <= 15) {
          e->rip = in.start; e->len = (u8)len; e->hdr = (u8)in.hdr; e->mr_end = (u8)in.mr_end;
          e->m0 = len >= 8 ? ~0ull : (1ull << (len * 8)) - 1; e->m1 = len >= 16 ? ~0ull : len <= 8 ? 0 : (1ull << ((len - 8) * 8)) - 1;
          e->b0 = w0 & e->m0; e->b1 = w1 & e->m1;
          e->rex = in.rex; e->rexw = in.rexw; e->rexr = in.rexr; e->rexx = in.rexx; e->rexb = in.rexb; e->p66 = in.p66; e->p67 = in.p67; e->rep = in.rep; e->seg = in.seg; e->lock = in.lock;
          e->modrm = in.modrm; e->mod = in.mod; e->reg = in.reg; e->rm = in.rm; e->scale = (u8)in.scale; e->riprel = (u8)in.riprel; e->base = in.base; e->idx = in.idx; e->disp = in.disp;
          e->op = code[in.hdr - 1]; e->op2 = code[in.hdr]; e->code = code; e->mem = c->mem; e->gen = mem_gen(c->mem); e->m32 = c->mode32; e->fast = dc_classify(e);
        }
      }
    }
    if (c->status != RUN_OK) {
      if (c->status == RUN_FAULT) { c->err_rip = in.start; if (c->exc_code) { c->rip = in.start; if (w32_hw_exception(c)) continue; } }
      return c->status;
    }
    c->rip = c->mode32 ? (u32)in.ip : in.ip; c->icount++;
    if (c->trace) e_log("%llx\n", (unsigned long long)c->rip);
  }
  return RUN_OK;
}
