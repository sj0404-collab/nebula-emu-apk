/* win32_math.inc — the rest of the UCRT <math.h>/<complex.h> surface that MSVC-built numeric code (numpy, scipy,
 * OpenBLAS, game engines) imports: float ("f") and long-double ("l" — on x64 MSVC long double IS double) variants,
 * the C99 complex functions and the _dpcomp/_fdpcomp comparisons.
 *
 * MSVC x64 ABI for complex values:  _Fcomplex (8 bytes) travels packed in an integer register (re = low 32 bits,
 * im = high 32) and is returned in RAX;  _Dcomplex/_Lcomplex (16 bytes) are passed by pointer and returned through a
 * hidden first argument (RCX = result buffer, which the callee also returns in RAX).  Real-valued results go in XMM0. */

#define MF0 c->x[0].f32[0]
#define MF1 c->x[1].f32[0]
#define MD0 c->x[0].f64[0]
#define MD1 c->x[1].f64[0]
static double mm_sin(double x) { return host_math(0, x, 0); }
static double mm_cos(double x) { return host_math(1, x, 0); }
static double mm_tan(double x) { return host_math(2, x, 0); }
static double mm_atan2(double y, double x) { return host_math(6, y, x); }
static double mm_asin(double x) { return host_math(7, x, 0); }
static double mm_acos(double x) { return host_math(8, x, 0); }
static double mm_atan(double x) { return host_math(9, x, 0); }
static double mm_sinh(double x) { return host_math(10, x, 0); }
static double mm_cosh(double x) { return host_math(11, x, 0); }
static double mm_tanh(double x) { return host_math(12, x, 0); }
static double mm_log10(double x) { return host_math(13, x, 0); }
static double mm_log2(double x) { return host_math(14, x, 0); }
static double mm_cbrt(double x) { return host_math(15, x, 0); }
static double mm_exp2(double x) { return host_math(5, 2.0, x); }
static double mm_expm1(double x) { return (x < 1e-5 && x > -1e-5) ? x + x * x / 2 + x * x * x / 6 : m_exp(x) - 1; }
static double mm_log1p(double x) { return (x < 1e-4 && x > -1e-4) ? x - x * x / 2 + x * x * x / 3 : m_log(1 + x); }
static double mm_acosh(double x) { return m_log(x + e_sqrt(x * x - 1)); }
static double mm_asinh(double x) { double ax = x < 0 ? -x : x; double r = m_log(ax + e_sqrt(ax * ax + 1)); return x < 0 ? -r : r; }
static double mm_atanh(double x) { return 0.5 * m_log((1 + x) / (1 - x)); }
static double mm_rint(double v) { if (v != v || v > 4503599627370496.0 || v < -4503599627370496.0) return v; double f = e_floor(v); double d = v - f; if (d > 0.5 || (d == 0.5 && e_fmod(f, 2.0) != 0)) f += 1; return f == 0 && v < 0 ? -0.0 : f; }   /* round-half-even (default rounding mode) */
static double mm_hypot(double a, double b) { return e_sqrt(a * a + b * b); }
static double mm_copysign(double a, double b) { u64 x, y; e_memcpy(&x, &a, 8); e_memcpy(&y, &b, 8); x = (x & ~(1ull << 63)) | (y & (1ull << 63)); e_memcpy(&a, &x, 8); return a; }
static double mm_nextafter(double a, double b) { if (a != a || b != b) return a + b; if (a == b) return b; u64 bits; e_memcpy(&bits, &a, 8);
  if (a == 0) { bits = 1; double r; e_memcpy(&r, &bits, 8); return b > 0 ? r : -r; } if ((a < b) == (a > 0)) bits++; else bits--; e_memcpy(&a, &bits, 8); return a; }
static float mm_nextafterf(float a, float b) { if (a != a || b != b) return a + b; if (a == b) return b; u32 bits; e_memcpy(&bits, &a, 4);
  if (a == 0) { bits = 1; float r; e_memcpy(&r, &bits, 4); return b > 0 ? r : -r; } if ((a < b) == (a > 0)) bits++; else bits--; e_memcpy(&a, &bits, 4); return a; }

/* float variants */
#define MF_UN(name, expr) static void s_##name(Cpu *c) { double x = (double)MF0; MF0 = (float)(expr); }
MF_UN(log1pf, mm_log1p(x)) MF_UN(expm1f, mm_expm1(x)) MF_UN(exp2f, mm_exp2(x)) MF_UN(cbrtf, mm_cbrt(x)) MF_UN(log2f, mm_log2(x)) MF_UN(log10f, mm_log10(x))
MF_UN(atanhf, mm_atanh(x)) MF_UN(asinhf, mm_asinh(x)) MF_UN(acoshf, mm_acosh(x)) MF_UN(tanhf, mm_tanh(x)) MF_UN(coshf, mm_cosh(x)) MF_UN(sinhf, mm_sinh(x))
MF_UN(atanf, mm_atan(x)) MF_UN(asinf, mm_asin(x)) MF_UN(acosf, mm_acos(x)) MF_UN(truncf, __builtin_trunc(x)) MF_UN(rintf, mm_rint(x)) MF_UN(nearbyintf, mm_rint(x))
MF_UN(erff, m_erf(x)) MF_UN(erfcf, m_erfc(x)) MF_UN(tgammaf, m_tgamma(x)) MF_UN(lgammaf, m_log(e_fabs(m_tgamma(x)))) MF_UN(exp10f_, host_math(5, 10.0, x))
static void s_fminf(Cpu *c) { float a = MF0, b = MF1; MF0 = a != a ? b : b != b ? a : a < b ? a : b; }
static void s_fmaxf(Cpu *c) { float a = MF0, b = MF1; MF0 = a != a ? b : b != b ? a : a > b ? a : b; }
static void s_fdimf(Cpu *c) { MF0 = MF0 > MF1 ? MF0 - MF1 : 0; }
static void s_fmaf(Cpu *c) { MF0 = (float)((double)MF0 * MF1 + c->x[2].f32[0]); }
static void s_copysignf(Cpu *c) { MF0 = (float)mm_copysign(MF0, MF1); }
static void s_nextafterf(Cpu *c) { MF0 = mm_nextafterf(MF0, MF1); }
static void s_hypotf(Cpu *c) { MF0 = (float)mm_hypot(MF0, MF1); }
static void s_modff(Cpu *c) { float v = MF0; float ip = (float)__builtin_trunc((double)v); u32 bits; e_memcpy(&bits, &ip, 4); if (ARG(1)) wr32(c, ARG(1), bits); MF0 = v - ip; }
static void s_frexpf(Cpu *c) { double v = MF0; int e = 0; if (v != 0 && v == v && v - v == 0) { double a = v < 0 ? -v : v; while (a >= 1) { a /= 2; e++; } while (a < 0.5) { a *= 2; e--; } v = v < 0 ? -a : a; } if (ARG(1)) wr32(c, ARG(1), (u32)e); MF0 = (float)v; }
static void s_ldexpf(Cpu *c) { double v = MF0; int e = (int)ARG(1); while (e > 0) { v *= 2; e--; } while (e < 0) { v /= 2; e++; } MF0 = (float)v; }
static void s_remainderf(Cpu *c) { double a = MF0, b = MF1; if (b == 0) { MF0 = (float)(0.0 / 0.0); return; } double q = a / b; double n = q < 0 ? -e_floor(-q + 0.5) : e_floor(q + 0.5); MF0 = (float)(a - n * b); }
static void s_lroundf(Cpu *c) { double v = MF0; RET((u64)(s64)(v < 0 ? -e_floor(-v + 0.5) : e_floor(v + 0.5))); }
static void s_logbf(Cpu *c) { double x = MF0; if (x == 0) { MF0 = (float)(-1.0 / 0.0); return; } if (x < 0) x = -x; int e = 0; while (x >= 2) { x /= 2; e++; } while (x < 1) { x *= 2; e--; } MF0 = (float)e; }
static void s_ilogbf(Cpu *c) { double x = MF0; if (x == 0) { RET((u64)(u32)(int)0x80000000); return; } if (x < 0) x = -x; int e = 0; while (x >= 2) { x /= 2; e++; } while (x < 1) { x *= 2; e--; } RET((u64)(u32)e); }
static void s_scalbnf(Cpu *c) { s_ldexpf(c); }
/* double versions of a few that were missing or only had float/long siblings */
static void s_rint2(Cpu *c) { MD0 = mm_rint(MD0); }
static void s_scalbn(Cpu *c) { double v = MD0; int e = (int)ARG(1); while (e > 0) { v *= 2; e--; } while (e < 0) { v /= 2; e++; } MD0 = v; }
static void s_ilogb(Cpu *c) { double x = MD0; if (x == 0) { RET((u64)(u32)(int)0x80000000); return; } if (x < 0) x = -x; int e = 0; while (x >= 2) { x /= 2; e++; } while (x < 1) { x *= 2; e--; } RET((u64)(u32)e); }
/* _dpcomp / _fdpcomp: _FP_LT 1, _FP_EQ 2, _FP_GT 4, 0 when unordered */
static void s_dpcomp(Cpu *c) { double a = MD0, b = MD1; RET(a != a || b != b ? 0 : a < b ? 1 : a > b ? 4 : 2); }
static void s_fdpcomp(Cpu *c) { float a = MF0, b = MF1; RET(a != a || b != b ? 0 : a < b ? 1 : a > b ? 4 : 2); }

/* ---------- complex ---------- */
typedef struct { double re, im; } Cx;
static Cx cx(double re, double im) { Cx z = { re, im }; return z; }
static Cx cx_rd(Cpu *c, u64 a) { u64 x = rd64(c, a), y = rd64(c, a + 8); Cx z; e_memcpy(&z.re, &x, 8); e_memcpy(&z.im, &y, 8); return z; }
static void cx_wr(Cpu *c, u64 a, Cx z) { u64 x, y; e_memcpy(&x, &z.re, 8); e_memcpy(&y, &z.im, 8); wr64(c, a, x); wr64(c, a + 8, y); }
static Cx cxf_unpack(u64 v) { float re, im; u32 lo = (u32)v, hi = (u32)(v >> 32); e_memcpy(&re, &lo, 4); e_memcpy(&im, &hi, 4); return cx(re, im); }
static u64 cxf_pack(Cx z) { float re = (float)z.re, im = (float)z.im; u32 lo, hi; e_memcpy(&lo, &re, 4); e_memcpy(&hi, &im, 4); return (u64)lo | ((u64)hi << 32); }
static double cx_abs(Cx z) { double a = e_fabs(z.re), b = e_fabs(z.im); if (a < b) { double t = a; a = b; b = t; } if (a == 0) return 0; double r = b / a; return a * e_sqrt(1 + r * r); }
static double cx_arg(Cx z) { return mm_atan2(z.im, z.re); }
static Cx cx_mul(Cx a, Cx b) { return cx(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re); }
static Cx cx_div(Cx a, Cx b) { double d = b.re * b.re + b.im * b.im; return cx((a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d); }
static Cx cx_exp(Cx z) { double e = m_exp(z.re); if (z.im == 0) return cx(e, z.im); return cx(e * mm_cos(z.im), e * mm_sin(z.im)); }
static Cx cx_log(Cx z) { return cx(m_log(cx_abs(z)), cx_arg(z)); }
static Cx cx_sqrt(Cx z) { double r = cx_abs(z); if (r == 0) return cx(0, z.im); double t = e_sqrt((r + e_fabs(z.re)) / 2); if (z.re >= 0) return cx(t, z.im / (2 * t)); return cx(e_fabs(z.im) / (2 * t), z.im < 0 ? -t : t); }
static Cx cx_pow(Cx x, Cx y) { if (x.re == 0 && x.im == 0) return (y.re == 0 && y.im == 0) ? cx(1, 0) : cx(0, 0); if (y.im == 0 && y.re == 2) return cx_mul(x, x); return cx_exp(cx_mul(y, cx_log(x))); }
static Cx cx_sin(Cx z) { return cx(mm_sin(z.re) * mm_cosh(z.im), mm_cos(z.re) * mm_sinh(z.im)); }
static Cx cx_cos(Cx z) { return cx(mm_cos(z.re) * mm_cosh(z.im), -mm_sin(z.re) * mm_sinh(z.im)); }
static Cx cx_tan(Cx z) { return cx_div(cx_sin(z), cx_cos(z)); }
static Cx cx_sinh(Cx z) { return cx(mm_sinh(z.re) * mm_cos(z.im), mm_cosh(z.re) * mm_sin(z.im)); }
static Cx cx_cosh(Cx z) { return cx(mm_cosh(z.re) * mm_cos(z.im), mm_sinh(z.re) * mm_sin(z.im)); }
static Cx cx_tanh(Cx z) { if (z.re > 20) return cx(1, 0); if (z.re < -20) return cx(-1, 0); return cx_div(cx_sinh(z), cx_cosh(z)); }
static Cx cx_asinh(Cx z) { Cx t = cx_sqrt(cx(cx_mul(z, z).re + 1, cx_mul(z, z).im)); return cx_log(cx(z.re + t.re, z.im + t.im)); }
static Cx cx_acosh(Cx z) { Cx a = cx_sqrt(cx(z.re - 1, z.im)), b = cx_sqrt(cx(z.re + 1, z.im)); Cx p = cx_mul(a, b); return cx_log(cx(z.re + p.re, z.im + p.im)); }
static Cx cx_atanh(Cx z) { Cx n = cx_log(cx(1 + z.re, z.im)), d = cx_log(cx(1 - z.re, -z.im)); return cx((n.re - d.re) / 2, (n.im - d.im) / 2); }
static Cx cx_asin(Cx z) { Cx t = cx_asinh(cx(-z.im, z.re)); return cx(t.im, -t.re); }        /* asin z = -i asinh(iz) */
static Cx cx_acos(Cx z) { Cx t = cx_asin(z); return cx(1.5707963267948966 - t.re, -t.im); }
static Cx cx_atan(Cx z) { Cx t = cx_atanh(cx(-z.im, z.re)); return cx(t.im, -t.re); }        /* atan z = -i atanh(iz) */
static Cx cx_proj(Cx z) { if (z.re - z.re != 0 || z.im - z.im != 0) return cx(1.0 / 0.0, z.im < 0 ? -0.0 : 0.0); return z; }
/* double complex: RCX = result, RDX = &x [, R8 = &y];  float complex: RCX = x [, RDX = y] → RAX;  real results → XMM0 */
#define CX_D1(name, fn) static void s_##name(Cpu *c) { cx_wr(c, ARG(0), fn(cx_rd(c, ARG(1)))); RET(ARG(0)); }
#define CX_F1(name, fn) static void s_##name##f(Cpu *c) { RET(cxf_pack(fn(cxf_unpack(ARG(0))))); }
#define CX_BOTH(name, fn) CX_D1(name, fn) CX_F1(name, fn)
CX_BOTH(cexp, cx_exp) CX_BOTH(clog, cx_log) CX_BOTH(csqrt, cx_sqrt) CX_BOTH(csin, cx_sin) CX_BOTH(ccos, cx_cos) CX_BOTH(ctan, cx_tan)
CX_BOTH(csinh, cx_sinh) CX_BOTH(ccosh, cx_cosh) CX_BOTH(ctanh, cx_tanh) CX_BOTH(casin, cx_asin) CX_BOTH(cacos, cx_acos) CX_BOTH(catan, cx_atan)
CX_BOTH(casinh, cx_asinh) CX_BOTH(cacosh, cx_acosh) CX_BOTH(catanh, cx_atanh) CX_BOTH(cproj, cx_proj)
static void s_cpow(Cpu *c) { cx_wr(c, ARG(0), cx_pow(cx_rd(c, ARG(1)), cx_rd(c, ARG(2)))); RET(ARG(0)); }
static void s_cpowf(Cpu *c) { RET(cxf_pack(cx_pow(cxf_unpack(ARG(0)), cxf_unpack(ARG(1))))); }
static void s_conj(Cpu *c) { Cx z = cx_rd(c, ARG(1)); cx_wr(c, ARG(0), cx(z.re, -z.im)); RET(ARG(0)); }
static void s_conjf(Cpu *c) { RET(ARG(0) ^ (1ull << 63)); }
static void s_creal(Cpu *c) { MD0 = cx_rd(c, ARG(0)).re; }
static void s_cimag(Cpu *c) { MD0 = cx_rd(c, ARG(0)).im; }
static void s_cabs(Cpu *c) { MD0 = cx_abs(cx_rd(c, ARG(0))); }
static void s_carg(Cpu *c) { MD0 = cx_arg(cx_rd(c, ARG(0))); }
static void s_norm(Cpu *c) { Cx z = cx_rd(c, ARG(0)); MD0 = z.re * z.re + z.im * z.im; }
static void s_crealf(Cpu *c) { MF0 = (float)cxf_unpack(ARG(0)).re; }
static void s_cimagf(Cpu *c) { MF0 = (float)cxf_unpack(ARG(0)).im; }
static void s_cabsf(Cpu *c) { MF0 = (float)cx_abs(cxf_unpack(ARG(0))); }
static void s_cargf(Cpu *c) { MF0 = (float)cx_arg(cxf_unpack(ARG(0))); }
static void s_normf(Cpu *c) { Cx z = cxf_unpack(ARG(0)); MF0 = (float)(z.re * z.re + z.im * z.im); }
/* _Cbuild / _FCbuild (re, im) → complex; _Cmulcc / _Cmulcr / _FCmulcc / _FCmulcr */
static void s_Cbuild(Cpu *c) { cx_wr(c, ARG(0), cx(MD1, c->x[2].f64[0])); RET(ARG(0)); }
static void s_FCbuild(Cpu *c) { RET(cxf_pack(cx(MF0, MF1))); }
static void s_Cmulcc(Cpu *c) { cx_wr(c, ARG(0), cx_mul(cx_rd(c, ARG(1)), cx_rd(c, ARG(2)))); RET(ARG(0)); }
static void s_Cmulcr(Cpu *c) { Cx z = cx_rd(c, ARG(1)); double r = c->x[2].f64[0]; cx_wr(c, ARG(0), cx(z.re * r, z.im * r)); RET(ARG(0)); }
static void s_FCmulcc(Cpu *c) { RET(cxf_pack(cx_mul(cxf_unpack(ARG(0)), cxf_unpack(ARG(1))))); }
static void s_FCmulcr(Cpu *c) { Cx z = cxf_unpack(ARG(0)); double r = MF1; RET(cxf_pack(cx(z.re * r, z.im * r))); }

/* strncat(dst, src, n) */
static void s_strncat(Cpu *c) { const char *d0 = rd_str(c, ARG(0), 1 << 30), *s = rd_str(c, ARG(1), 1 << 30); if (!d0 || !s) { cpu_fault(c, "strncat bad pointer"); return; }
  size_t dl = e_strlen(d0), sl = e_strlen(s), n = (size_t)ARG(2); if (sl > n) sl = n; u8 *d = mem_ptr(c->mem, ARG(0) + dl, sl + 1, 1); if (d) { e_memmove(d, s, sl); d[sl] = 0; } RET(ARG(0)); }
/* __conio_common_vcprintf(options, format, locale, va_list) → console */
static void s_conio_common_vcprintf(Cpu *c) { ArgSrc src = { c, 1, 0, ARG(3) }; int n = g_format(c, g_fmt_out, sizeof g_fmt_out, ARG(1), 0, &src); if (c->status) return; if (n > 0) con_out(W, 1, g_fmt_out, (size_t)n); RET((u64)(u32)n); }
static void s_cputs(Cpu *c) { const char *s = rd_str(c, ARG(0), 1 << 20); if (s) con_out(W, 1, s, e_strlen(s)); RET(0); }
#undef MF0
#undef MF1
#undef MD0
#undef MD1

#define MATH_SHIMS \
  S(log1pf), S(expm1f), S(exp2f), S(cbrtf), S(log2f), S(log10f), S(atanhf), S(asinhf), S(acoshf), S(tanhf), S(coshf), S(sinhf), S(atanf), S(asinf), S(acosf), S(truncf), S(rintf), S(nearbyintf), S(erff), S(erfcf), S(tgammaf), S(lgammaf), \
  S(fminf), S(fmaxf), S(fdimf), S(fmaf), S(copysignf), SA("_copysignf", copysignf), S(nextafterf), SA("_nextafterf", nextafterf), SA("_hypotf", hypotf), S(hypotf), S(modff), S(frexpf), S(ldexpf), SA("_ldexpf", ldexpf), S(remainderf), S(lroundf), SA("llroundf", lroundf), S(logbf), S(ilogbf), S(scalbnf), SA("_scalbf", scalbnf), SA("scalblnf", scalbnf), \
  SA("rint", rint2), SA("nearbyint", rint2), S(scalbn), SA("_scalb", scalbn), SA("scalbln", scalbn), S(ilogb), SA("_dpcomp", dpcomp), SA("_fdpcomp", fdpcomp), SA("_ldpcomp", dpcomp), \
  /* long double == double on x64 MSVC */ \
  SA("log1pl", log1p), SA("expm1l", expm1), SA("exp2l", exp2), SA("cbrtl", cbrt), SA("log2l", log2), SA("log10l", log10), SA("atanhl", atanh), SA("asinhl", asinh), SA("acoshl", acosh), SA("truncl", trunc), SA("rintl", rint2), SA("nearbyintl", rint2), SA("copysignl", copysign), SA("nextafterl", nextafter), SA("_nextafterl", nextafter), SA("nexttowardl", nextafter), SA("nexttoward", nextafter), SA("nexttowardf", nextafterf), \
  SA("sinl", sin), SA("cosl", cos), SA("tanl", tan), SA("expl", exp), SA("logl", log), SA("powl", pow), SA("sqrtl", sqrt), SA("atan2l", atan2), SA("asinl", asin), SA("acosl", acos), SA("atanl", atan), SA("sinhl", sinh), SA("coshl", cosh), SA("tanhl", tanh), SA("floorl", floor), SA("ceill", ceil), SA("fabsl", fabs), SA("fmodl", fmod), SA("roundl", round), SA("fminl", fmin), SA("fmaxl", fmax), SA("hypotl", hypot), SA("ldexpl", ldexp), SA("frexpl", frexp), SA("modfl", modf), SA("erfl", erf), SA("erfcl", erfc), SA("tgammal", tgamma), SA("lgammal", lgamma), SA("fmal", fma), SA("fdiml", fdim), SA("remainderl", remainder), SA("lroundl", lround), SA("llroundl", lround), SA("logbl", logb), SA("ilogbl", ilogb), SA("scalbnl", scalbn), SA("scalblnl", scalbn), \
  S(cexp), S(cexpf), SA("cexpl", cexp), S(clog), S(clogf), SA("clogl", clog), S(csqrt), S(csqrtf), SA("csqrtl", csqrt), S(csin), S(csinf), SA("csinl", csin), S(ccos), S(ccosf), SA("ccosl", ccos), S(ctan), S(ctanf), SA("ctanl", ctan), \
  S(csinh), S(csinhf), SA("csinhl", csinh), S(ccosh), S(ccoshf), SA("ccoshl", ccosh), S(ctanh), S(ctanhf), SA("ctanhl", ctanh), S(casin), S(casinf), SA("casinl", casin), S(cacos), S(cacosf), SA("cacosl", cacos), S(catan), S(catanf), SA("catanl", catan), \
  S(casinh), S(casinhf), SA("casinhl", casinh), S(cacosh), S(cacoshf), SA("cacoshl", cacosh), S(catanh), S(catanhf), SA("catanhl", catanh), S(cproj), S(cprojf), SA("cprojl", cproj), S(cpow), S(cpowf), SA("cpowl", cpow), S(conj), S(conjf), SA("conjl", conj), \
  S(creal), S(crealf), SA("creall", creal), S(cimag), S(cimagf), SA("cimagl", cimag), S(cabs), S(cabsf), SA("cabsl", cabs), SA("_cabs", cabs), S(carg), S(cargf), SA("cargl", carg), S(norm), S(normf), SA("norml", norm), \
  SA("_Cbuild", Cbuild), SA("_FCbuild", FCbuild), SA("_LCbuild", Cbuild), SA("_Cmulcc", Cmulcc), SA("_Cmulcr", Cmulcr), SA("_FCmulcc", FCmulcc), SA("_FCmulcr", FCmulcr), SA("_LCmulcc", Cmulcc), SA("_LCmulcr", Cmulcr), \
  S(strncat), SA("__conio_common_vcprintf", conio_common_vcprintf), SA("_cputs", cputs), SA("__stdio_common_vsscanf", stdio_common_vsscanf), SA("__stdio_common_vfscanf", stdio_common_vfscanf),
