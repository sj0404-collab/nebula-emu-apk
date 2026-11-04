/* x87.inc — x87 FPU subset, included by cpu.c.
   Registers are kept as IEEE doubles (64-bit); 80-bit loads/stores are converted. That is enough for what 64-bit
   Windows code still uses the FPU for: mingw's own printf/strtod (long double formatting), `long double` arithmetic
   in CRT helpers, and legacy codecs. Precision beyond 53 bits is NOT emulated (documented limit). */

#define FTOP (c->ftop & 7)
#define ST(n) c->fp[(c->ftop + (n)) & 7]
#define F_C0 0x0100
#define F_C1 0x0200
#define F_C2 0x0400
#define F_C3 0x4000
static inline void fpush(Cpu *c, double v) { c->ftop = (c->ftop - 1) & 7; c->fp[c->ftop] = v; c->ftag |= 1u << c->ftop; }
static inline double fpop(Cpu *c) { double v = c->fp[c->ftop]; c->ftag &= ~(1u << c->ftop); c->ftop = (c->ftop + 1) & 7; return v; }
static double pow2d(int e) { double r = 1.0; while (e > 1000) { r *= 1.0715086071862673e301; e -= 1000; } while (e < -1000) { r *= 9.332636185032189e-302; e += 1000; } u64 bits = (u64)(e + 1023) << 52; double s; e_memcpy(&s, &bits, 8); return r * s; }
static double f80_to_double(u64 mant, u16 se) {
  int sign = se >> 15, e = se & 0x7fff;
  if (e == 0 && mant == 0) return sign ? -0.0 : 0.0;
  if (e == 0x7fff) { if (mant << 1) return 0.0 / 0.0; return sign ? -1.0 / 0.0 : 1.0 / 0.0; }
  double r = (double)mant * pow2d(e - 16383 - 63);   /* u64→double rounds to 53 bits */
  return sign ? -r : r;
}
static void double_to_f80(double v, u64 *mant, u16 *se) {
  u64 bits; e_memcpy(&bits, &v, 8); int sign = (int)(bits >> 63); int e = (int)((bits >> 52) & 0x7ff); u64 frac = bits & 0xfffffffffffffull;
  if (e == 0 && frac == 0) { *mant = 0; *se = (u16)(sign << 15); return; }
  if (e == 0x7ff) { *se = (u16)((sign << 15) | 0x7fff); *mant = frac ? (0xc000000000000000ull | (frac << 11)) : 0x8000000000000000ull; return; }
  if (e == 0) { /* subnormal */ int sh = 0; while (!(frac & (1ull << 52))) { frac <<= 1; sh++; } e = 1 - sh; }
  *mant = (frac << 11) | 0x8000000000000000ull; *se = (u16)((sign << 15) | (e - 1023 + 16383));
}
static double x87_ld(Cpu *c, u64 a, int kind) {   /* 0 f32, 1 f64, 2 i16, 3 i32, 4 i64, 5 f80 */
  switch (kind) {
    case 0: { u32 b = rd32(c, a); float f; e_memcpy(&f, &b, 4); return f; }
    case 1: { u64 b = rd64(c, a); double d; e_memcpy(&d, &b, 8); return d; }
    case 2: return (double)(s16)rd16(c, a);
    case 3: return (double)(s32)rd32(c, a);
    case 4: return (double)(s64)rd64(c, a);
    default: return f80_to_double(rd64(c, a), rd16(c, a + 8));
  }
}
static double x87_round(Cpu *c, double v) {
  int rc = (c->fpu_cw >> 10) & 3;   /* 0 nearest-even, 1 down, 2 up, 3 truncate */
  double f = e_floor(v);
  switch (rc) { case 1: return f; case 2: return e_ceil(v); case 3: return v < 0 ? e_ceil(v) : f;
    default: { double d = v - f; if (d > 0.5) return f + 1; if (d < 0.5) return f; return e_fmod(f, 2.0) == 0 ? f : f + 1; } }
}
static void x87_st(Cpu *c, u64 a, int kind, double v) {
  switch (kind) {
    case 0: { float f = (float)v; u32 b; e_memcpy(&b, &f, 4); wr32(c, a, b); break; }
    case 1: { u64 b; e_memcpy(&b, &v, 8); wr64(c, a, b); break; }
    case 2: { double r = x87_round(c, v); wr16(c, a, (r != r || r > 32767 || r < -32768) ? 0x8000 : (u16)(s16)r); break; }
    case 3: { double r = x87_round(c, v); wr32(c, a, (r != r || r > 2147483647.0 || r < -2147483648.0) ? 0x80000000u : (u32)(s32)r); break; }
    case 4: { double r = x87_round(c, v); wr64(c, a, (r != r || r >= 9223372036854775808.0 || r < -9223372036854775808.0) ? 0x8000000000000000ull : (u64)(s64)r); break; }
    case 6: { wr16(c, a, (u16)(s16)__builtin_trunc(v)); break; }   /* fisttp variants: truncate */
    case 7: { wr32(c, a, (u32)(s32)__builtin_trunc(v)); break; }
    case 8: { wr64(c, a, (u64)(s64)__builtin_trunc(v)); break; }
    default: { u64 m; u16 se; double_to_f80(v, &m, &se); wr64(c, a, m); wr16(c, a + 8, se); }
  }
}
static void x87_compare(Cpu *c, double a, double b, int to_eflags) {
  int unordered = (a != a) || (b != b);
  if (to_eflags) { c->rflags &= ~(u64)(F_ZF | F_PF | F_CF); if (unordered) c->rflags |= F_ZF | F_PF | F_CF; else if (a < b) c->rflags |= F_CF; else if (a == b) c->rflags |= F_ZF; }
  else { c->fsw &= ~(F_C0 | F_C2 | F_C3); if (unordered) c->fsw |= F_C0 | F_C2 | F_C3; else if (a < b) c->fsw |= F_C0; else if (a == b) c->fsw |= F_C3; }
}
static double x87_arith(int sub, double a, double b) {   /* sub = modrm.reg: 0 add 1 mul 4 sub 5 subr 6 div 7 divr */
  switch (sub) { case 0: return a + b; case 1: return a * b; case 4: return a - b; case 5: return b - a; case 6: return a / b; default: return b / a; }
}
static int x87_cmov_ok(Cpu *c, int cc) {   /* DA: b, e, be, u ; DB: nb, ne, nbe, nu */
  int cf = !!(c->rflags & F_CF), zf = !!(c->rflags & F_ZF), pf = !!(c->rflags & F_PF);
  switch (cc) { case 0: return cf; case 1: return zf; case 2: return cf || zf; case 3: return pf; case 4: return !cf; case 5: return !zf; case 6: return !cf && !zf; default: return !pf; }
}
static void exec_x87(Ins *i, Cpu *c, u8 op) {
  decode_modrm(i); int sub = i->reg & 7; int sti = i->rm & 7;
  if (i->mod != 3) {
    u64 a = EA(i);
    switch (op) {
      case 0xD8: { double m = x87_ld(c, a, 0); if (sub == 2 || sub == 3) { x87_compare(c, ST(0), m, 0); if (sub == 3) fpop(c); } else ST(0) = x87_arith(sub, ST(0), m); return; }
      case 0xDC: { double m = x87_ld(c, a, 1); if (sub == 2 || sub == 3) { x87_compare(c, ST(0), m, 0); if (sub == 3) fpop(c); } else ST(0) = x87_arith(sub, ST(0), m); return; }
      case 0xDA: { double m = x87_ld(c, a, 3); if (sub == 2 || sub == 3) { x87_compare(c, ST(0), m, 0); if (sub == 3) fpop(c); } else ST(0) = x87_arith(sub, ST(0), m); return; }
      case 0xDE: { double m = x87_ld(c, a, 2); if (sub == 2 || sub == 3) { x87_compare(c, ST(0), m, 0); if (sub == 3) fpop(c); } else ST(0) = x87_arith(sub, ST(0), m); return; }
      case 0xD9: switch (sub) {
        case 0: fpush(c, x87_ld(c, a, 0)); return;                 /* fld m32 */
        case 2: x87_st(c, a, 0, ST(0)); return;                    /* fst m32 */
        case 3: x87_st(c, a, 0, fpop(c)); return;                  /* fstp m32 */
        case 4: { c->fpu_cw = rd16(c, a); c->fsw = rd16(c, a + 4); return; }   /* fldenv (28 bytes; we keep cw/sw) */
        case 5: c->fpu_cw = rd16(c, a); return;                    /* fldcw */
        case 6: { for (int k = 0; k < 28; k += 4) wr32(c, a + k, 0); wr16(c, a, c->fpu_cw); wr16(c, a + 4, (u16)(c->fsw | (FTOP << 11))); wr16(c, a + 8, 0xffff); return; }   /* fnstenv */
        case 7: wr16(c, a, c->fpu_cw); return;                     /* fnstcw */
        default: break; } break;
      case 0xDB: switch (sub) {
        case 0: fpush(c, x87_ld(c, a, 3)); return;                 /* fild m32 */
        case 1: x87_st(c, a, 7, fpop(c)); return;                  /* fisttp m32 */
        case 2: x87_st(c, a, 3, ST(0)); return;                    /* fist m32 */
        case 3: x87_st(c, a, 3, fpop(c)); return;                  /* fistp m32 */
        case 5: fpush(c, x87_ld(c, a, 5)); return;                 /* fld m80 */
        case 7: x87_st(c, a, 5, fpop(c)); return;                  /* fstp m80 */
        default: break; } break;
      case 0xDD: switch (sub) {
        case 0: fpush(c, x87_ld(c, a, 1)); return;                 /* fld m64 */
        case 1: x87_st(c, a, 8, fpop(c)); return;                  /* fisttp m64 */
        case 2: x87_st(c, a, 1, ST(0)); return;                    /* fst m64 */
        case 3: x87_st(c, a, 1, fpop(c)); return;                  /* fstp m64 */
        case 4: { c->fpu_cw = rd16(c, a); c->fsw = rd16(c, a + 4); c->ftop = (c->fsw >> 11) & 7; for (int k = 0; k < 8; k++) c->fp[(c->ftop + k) & 7] = x87_ld(c, a + 28 + k * 10, 5); c->ftag = 0xff; return; }   /* frstor */
        case 6: { for (int k = 0; k < 108; k += 4) wr32(c, a + k, 0); wr16(c, a, c->fpu_cw); wr16(c, a + 4, (u16)(c->fsw | (FTOP << 11))); for (int k = 0; k < 8; k++) x87_st(c, a + 28 + k * 10, 5, ST(k)); c->ftop = 0; c->ftag = 0; c->fsw = 0; c->fpu_cw = 0x37f; return; }   /* fnsave */
        case 7: wr16(c, a, (u16)(c->fsw | (FTOP << 11))); return;  /* fnstsw m16 */
        default: break; } break;
      case 0xDF: switch (sub) {
        case 0: fpush(c, x87_ld(c, a, 2)); return;                 /* fild m16 */
        case 1: x87_st(c, a, 6, fpop(c)); return;                  /* fisttp m16 */
        case 2: x87_st(c, a, 2, ST(0)); return;                    /* fist m16 */
        case 3: x87_st(c, a, 2, fpop(c)); return;                  /* fistp m16 */
        case 4: { /* fbld m80bcd */ u64 v = 0, m = 1; for (int k = 0; k < 9; k++) { u8 b = rd8(c, a + k); v += (u64)(b & 15) * m; m *= 10; v += (u64)(b >> 4) * m; m *= 10; } double d = (double)v; if (rd8(c, a + 9) & 0x80) d = -d; fpush(c, d); return; }
        case 6: { /* fbstp m80bcd: round to integer (current rounding mode), 18 packed digits + sign */ double d = fpop(c); double r = x87_round(c, d);
          int neg = r < 0; if (neg) r = -r; u64 v = r >= 1e18 ? 999999999999999999ull : (u64)r; for (int k = 0; k < 9; k++) { u8 lo = (u8)(v % 10); v /= 10; u8 hi = (u8)(v % 10); v /= 10; wr8(c, a + k, (u8)(lo | hi << 4)); } wr8(c, a + 9, neg ? 0x80 : 0); return; }
        case 5: fpush(c, x87_ld(c, a, 4)); return;                 /* fild m64 */
        case 7: x87_st(c, a, 4, fpop(c)); return;                  /* fistp m64 */
        default: break; } break;
    }
    UNSUP("x87 instruction %02X /%d (memory form)", op, sub);
  }
  /* register forms */
  switch (op) {
    case 0xD8: if (sub == 2 || sub == 3) { x87_compare(c, ST(0), ST(sti), 0); if (sub == 3) fpop(c); } else ST(0) = x87_arith(sub, ST(0), ST(sti)); return;
    case 0xDC: if (sub == 2 || sub == 3) { x87_compare(c, ST(0), ST(sti), 0); if (sub == 3) fpop(c); } else { int s2 = sub == 4 ? 5 : sub == 5 ? 4 : sub == 6 ? 7 : sub == 7 ? 6 : sub; ST(sti) = x87_arith(s2, ST(sti), ST(0)); } return;   /* fsub st(i),st = st(i) - st */
    case 0xDE: if (sub == 3) { if (sti == 1) { x87_compare(c, ST(0), ST(1), 0); fpop(c); fpop(c); return; } break; }   /* fcompp */
      { int s2 = sub == 4 ? 5 : sub == 5 ? 4 : sub == 6 ? 7 : sub == 7 ? 6 : sub; ST(sti) = x87_arith(s2, ST(sti), ST(0)); fpop(c); return; }   /* faddp/fmulp/fsubrp/fsubp/fdivrp/fdivp */
    case 0xD9: switch (sub) {
      case 0: fpush(c, ST(sti)); return;                           /* fld st(i) */
      case 1: { double t = ST(0); ST(0) = ST(sti); ST(sti) = t; return; }   /* fxch */
      case 2: if (sti == 0) return; break;                         /* fnop */
      case 3: ST(sti) = ST(0); fpop(c); return;                    /* fstp st(i) (undocumented alias) */
      case 4: switch (sti) {
        case 0: ST(0) = -ST(0); return;                            /* fchs */
        case 1: ST(0) = e_fabs(ST(0)); return;                     /* fabs */
        case 4: x87_compare(c, ST(0), 0.0, 0); return;             /* ftst */
        case 5: { double v = ST(0); c->fsw &= ~(F_C0 | F_C1 | F_C2 | F_C3); u64 b; e_memcpy(&b, &v, 8); if (b >> 63) c->fsw |= F_C1;   /* fxam */
          if (!(c->ftag & (1u << FTOP))) c->fsw |= F_C0 | F_C3; else if (v != v) c->fsw |= F_C0; else if (v == 0) c->fsw |= F_C3; else if (v * 0 != 0) c->fsw |= F_C0 | F_C2; else if (e_fabs(v) < 2.2250738585072014e-308) c->fsw |= F_C2 | F_C3; else c->fsw |= F_C2; return; }
        default: break; } break;
      case 5: { static const double k[8] = { 1.0, 3.321928094887362, 1.4426950408889634, 3.141592653589793, 0.3010299956639812, 0.6931471805599453, 0.0, 0.0 }; if (sti < 7) { fpush(c, k[sti]); return; } break; }
      case 6: switch (sti) {
        case 0: ST(0) = host_math(5, 2.0, ST(0)) - 1.0; return;    /* f2xm1 */
        case 1: ST(1) = ST(1) * host_math(14, ST(0), 0); fpop(c); return;   /* fyl2x */
        case 2: { double t = host_math(2, ST(0), 0); ST(0) = t; fpush(c, 1.0); c->fsw &= ~F_C2; return; }   /* fptan */
        case 3: ST(1) = host_math(6, ST(1), ST(0)); fpop(c); return;   /* fpatan */
        case 4: { double v = ST(0); if (v == 0) { ST(0) = -1.0 / 0.0; fpush(c, v); return; } int e = 0; double m = e_fabs(v); while (m >= 2) { m /= 2; e++; } while (m < 1) { m *= 2; e--; } ST(0) = e; fpush(c, v < 0 ? -m : m); return; }   /* fxtract */
        case 5: { double a = ST(0), b = ST(1); double q = a / b; double n = (q < 0 ? -e_floor(-q + 0.5) : e_floor(q + 0.5)); ST(0) = a - n * b; c->fsw &= ~(F_C2 | F_C0 | F_C1 | F_C3); s64 qi = (s64)n; if (qi & 1) c->fsw |= F_C1; if (qi & 2) c->fsw |= F_C3; if (qi & 4) c->fsw |= F_C0; return; }   /* fprem1 */
        case 6: c->ftop = (c->ftop - 1) & 7; return;                /* fdecstp */
        case 7: c->ftop = (c->ftop + 1) & 7; return;                /* fincstp */
        default: break; } break;
      case 7: switch (sti) {
        case 0: { double a = ST(0), b = ST(1); double q = __builtin_trunc(a / b); ST(0) = a - q * b; c->fsw &= ~(F_C2 | F_C0 | F_C1 | F_C3); s64 qi = (s64)q; if (qi & 1) c->fsw |= F_C1; if (qi & 2) c->fsw |= F_C3; if (qi & 4) c->fsw |= F_C0; return; }   /* fprem */
        case 1: ST(1) = ST(1) * host_math(14, ST(0) + 1.0, 0); fpop(c); return;   /* fyl2xp1 */
        case 2: ST(0) = e_sqrt(ST(0)); return;                      /* fsqrt */
        case 3: { double v = ST(0); ST(0) = host_math(0, v, 0); fpush(c, host_math(1, v, 0)); c->fsw &= ~F_C2; return; }   /* fsincos */
        case 4: ST(0) = x87_round(c, ST(0)); return;                /* frndint */
        case 5: { double e = __builtin_trunc(ST(1)); ST(0) = ST(0) * host_math(5, 2.0, e); return; }   /* fscale */
        case 6: ST(0) = host_math(0, ST(0), 0); c->fsw &= ~F_C2; return;   /* fsin */
        case 7: ST(0) = host_math(1, ST(0), 0); c->fsw &= ~F_C2; return;   /* fcos */
        default: break; } break;
      default: break; } break;
    case 0xDA: if (sub < 4) { if (x87_cmov_ok(c, sub)) ST(0) = ST(sti); return; } if (sub == 5 && sti == 1) { x87_compare(c, ST(0), ST(1), 0); fpop(c); fpop(c); return; } break;   /* fcmovcc / fucompp */
    case 0xDB: if (sub < 4) { if (x87_cmov_ok(c, 4 + sub)) ST(0) = ST(sti); return; }
      if (sub == 4) { if (sti == 2) { c->fsw &= 0xff00 & ~0x8000; c->fsw &= ~0x80ff; return; } if (sti == 3) { c->fpu_cw = 0x37f; c->fsw = 0; c->ftop = 0; c->ftag = 0; return; } if (sti == 0 || sti == 1 || sti == 4) return; break; }   /* fnclex / fninit / fneni,fndisi,fsetpm */
      if (sub == 5 || sub == 6) { x87_compare(c, ST(0), ST(sti), 1); return; }   /* fucomi / fcomi */
      break;
    case 0xDD: switch (sub) {
      case 0: c->ftag &= ~(1u << ((c->ftop + sti) & 7)); return;   /* ffree */
      case 2: ST(sti) = ST(0); return;                             /* fst st(i) */
      case 3: ST(sti) = ST(0); fpop(c); return;                    /* fstp st(i) */
      case 4: x87_compare(c, ST(0), ST(sti), 0); return;           /* fucom */
      case 5: x87_compare(c, ST(0), ST(sti), 0); fpop(c); return;  /* fucomp */
      default: break; } break;
    case 0xDF: if (sub == 4 && sti == 0) { c->r[RAX] = (c->r[RAX] & ~0xffffull) | (c->fsw | (FTOP << 11)); return; }   /* fnstsw ax */
      if (sub == 5 || sub == 6) { x87_compare(c, ST(0), ST(sti), 1); fpop(c); return; }   /* fucomip / fcomip */
      break;
  }
  UNSUP("x87 instruction %02X /%d st(%d)", op, sub, sti);
}
