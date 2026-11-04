/* sse4.inc — SSE3 / SSSE3 / SSE4.1 / SSE4.2 / ADX subset (the x86-64-v2 baseline that Bun, JavaScriptCore and simdutf compile against).
 * Included from cpu.c; dispatched from the 0F 38 / 0F 3A / 0F 7C / 0F 7D / 0F D0 / 0F F0 cases. Only the 128-bit XMM forms (66 prefix) are
 * implemented; the MMX forms (no prefix) are reported as unsupported like before. Verified against the host CPU by emu/tests/difftest.c. */

static inline u8 sat_u8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline u16 sat_u16(int v) { return (u16)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
static inline s16 sat_s16(int v) { return (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
static double sse_round(double v, int mode) {   /* mode: 0 nearest-even 1 down 2 up 3 truncate */
  switch (mode & 3) { case 0: return __builtin_nearbyint(v); case 1: return __builtin_floor(v); case 2: return __builtin_ceil(v); default: return __builtin_trunc(v); } }
static int round_mode(Cpu *c, u8 im) { return (im & 4) ? (int)((c->mxcsr >> 13) & 3) : (im & 3); }
static void set_zf_cf_only(Cpu *c, int zf, int cf) { c->rflags &= ~(u64)(F_ZF | F_CF | F_OF | F_SF | F_AF | F_PF); if (zf) c->rflags |= F_ZF; if (cf) c->rflags |= F_CF; }

/* pcmpistri / pcmpestri (SSE4.2 string compare, index result in ECX); bytes or words, implicit (NUL-terminated) or explicit lengths */
static void pcmpstr(Ins *i, int explicit_len, int want_mask) {
  Cpu *c = i->c; decode_modrm(i); fix_rip_rel(i, 1); Xmm b; xmm_load(i, &b, 16); u8 im = fetch8(i); Xmm *a = &c->x[i->reg];
  int words = im & 1, sign = (im >> 1) & 1, n = words ? 8 : 16, agg = (im >> 2) & 3, pol = (im >> 4) & 3, msb = (im >> 6) & 1;
  int la, lb;
  if (explicit_len) { s64 ea = (s64)(i->rexw ? c->r[RAX] : (s32)(u32)c->r[RAX]), ed = (s64)(i->rexw ? c->r[RDX] : (s32)(u32)c->r[RDX]); if (ea < 0) ea = -ea; if (ed < 0) ed = -ed; la = ea > n ? n : (int)ea; lb = ed > n ? n : (int)ed; }
  else { for (la = 0; la < n; la++) if ((words ? a->w[la] : a->b[la]) == 0) break; for (lb = 0; lb < n; lb++) if ((words ? b.w[lb] : b.b[lb]) == 0) break; }
  #define ELT(x, k) (words ? (sign ? (s32)(s16)(x)->w[k] : (s32)(x)->w[k]) : (sign ? (s32)(s8)(x)->b[k] : (s32)(x)->b[k]))
  u32 res = 0;
  for (int j = 0; j < n; j++) {   /* j indexes operand2 (b), the "string" being scanned */
    int bit = 0;
    switch (agg) {
      case 0: for (int k = 0; k < la; k++) if (j < lb && ELT(a, k) == ELT(&b, j)) { bit = 1; break; } break;                             /* equal any */
      case 1: for (int k = 0; k + 1 < la; k += 2) if (j < lb && ELT(&b, j) >= ELT(a, k) && ELT(&b, j) <= ELT(a, k + 1)) { bit = 1; break; } break;   /* ranges */
      case 2: bit = (j < la && j < lb) ? ELT(a, j) == ELT(&b, j) : (j >= la && j >= lb); break;                                           /* equal each */
      default: { bit = 1; for (int k = 0; k < la; k++) { int jb = j + k; int eq = (jb < lb) ? ELT(a, k) == ELT(&b, jb) : 0; if (!eq) { bit = 0; break; } } break; }   /* equal ordered */
    }
    if (bit) res |= 1u << j;
  }
  #undef ELT
  u32 valid = n == 16 ? 0xffff : 0xff;
  if (pol == 1) res = ~res & valid; else if (pol == 3) { u32 m = lb >= n ? valid : ((1u << lb) - 1); res ^= m; }
  res &= valid;
  int zf = lb < n, sf = la < n, cf = res != 0, of = res & 1;
  if (want_mask) { Xmm r = { { 0, 0 } }; if (msb) { for (int j = 0; j < n; j++) if (res & (1u << j)) { if (words) r.w[j] = 0xffff; else r.b[j] = 0xff; } } else r.d[0] = res; c->x[0] = r; }
  else { int idx = n; if (res) { if (msb) { for (int j = n - 1; j >= 0; j--) if (res & (1u << j)) { idx = j; break; } } else idx = __builtin_ctz(res); } c->r[RCX] = (u64)idx; }
  c->rflags &= ~(u64)(F_ZF | F_CF | F_OF | F_SF | F_AF | F_PF); if (zf) c->rflags |= F_ZF; if (sf) c->rflags |= F_SF; if (cf) c->rflags |= F_CF; if (of) c->rflags |= F_OF;
}

static void op_0f38(Ins *i) {
  Cpu *c = i->c; u8 op2 = fetch8(i);
  if (op2 == 0xF6) {   /* adcx (66) / adox (F3): r64 = r64 + r/m64 + CF|OF, only that flag updated */
    decode_modrm(i); int osz = i->rexw ? 8 : 4; u64 a = get_reg(i, i->reg, osz), b = rm_get(i, osz); u64 fl = i->rep == 0xF3 ? F_OF : F_CF; int cin = (c->rflags & fl) ? 1 : 0;
    unsigned __int128 s = (unsigned __int128)a + b + cin; u64 r = (u64)s; int cout = osz == 8 ? (int)(s >> 64) : (r >> 32) != 0; r &= mask(osz);
    set_reg(i, i->reg, osz, r); if (cout) c->rflags |= fl; else c->rflags &= ~fl; return; }
  if (op2 == 0xF0 || op2 == 0xF1) {
    if (i->rep == 0xF2) {   /* crc32 r32, r/m8|16|32|64 */
      decode_modrm(i); int sz = op2 == 0xF0 ? 1 : i->rexw ? 8 : i->p66 ? 2 : 4; u64 v = rm_get(i, sz); u32 crc = (u32)c->r[i->reg];
      for (int k = 0; k < sz; k++) { crc ^= (u8)(v >> (8 * k)); for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1))); }
      c->r[i->reg] = crc; return; }
    /* movbe */ decode_modrm(i); int osz = i->rexw ? 8 : i->p66 ? 2 : 4;
    if (op2 == 0xF0) { u64 v = rdn(c, EA(i), osz); v = osz == 8 ? __builtin_bswap64(v) : osz == 4 ? __builtin_bswap32((u32)v) : __builtin_bswap16((u16)v); set_reg(i, i->reg, osz, v); }
    else { u64 v = get_reg(i, i->reg, osz); v = osz == 8 ? __builtin_bswap64(v) : osz == 4 ? __builtin_bswap32((u32)v) : __builtin_bswap16((u16)v); wrn(c, EA(i), osz, v); }
    return; }
  if (!i->p66) { c->unknown_op[0] = 0x0F; c->unknown_op[1] = 0x38; c->unknown_op[2] = op2; UNSUP("unsupported MMX opcode 0F 38 %02X", op2); }
  decode_modrm(i); Xmm s, *d = &c->x[i->reg], r = { { 0, 0 } };
  int ldsz = (op2 >= 0x20 && op2 <= 0x25) || (op2 >= 0x30 && op2 <= 0x35) ? ((op2 & 7) == 0 || (op2 & 7) == 3 ? 8 : (op2 & 7) == 1 || (op2 & 7) == 4 ? 4 : (op2 & 7) == 2 ? 2 : 8) : 16;
  if ((op2 & 7) == 5 && (op2 == 0x25 || op2 == 0x35)) ldsz = 8;
  xmm_load(i, &s, ldsz);
  switch (op2) {
    case 0x00: for (int k = 0; k < 16; k++) r.b[k] = (s.b[k] & 0x80) ? 0 : d->b[s.b[k] & 15]; *d = r; return;                      /* pshufb */
    case 0x01: for (int k = 0; k < 4; k++) { r.w[k] = (u16)(d->w[2 * k] + d->w[2 * k + 1]); r.w[4 + k] = (u16)(s.w[2 * k] + s.w[2 * k + 1]); } *d = r; return;   /* phaddw */
    case 0x02: for (int k = 0; k < 2; k++) { r.d[k] = d->d[2 * k] + d->d[2 * k + 1]; r.d[2 + k] = s.d[2 * k] + s.d[2 * k + 1]; } *d = r; return;                  /* phaddd */
    case 0x03: for (int k = 0; k < 4; k++) { r.w[k] = (u16)sat_s16((s16)d->w[2 * k] + (s16)d->w[2 * k + 1]); r.w[4 + k] = (u16)sat_s16((s16)s.w[2 * k] + (s16)s.w[2 * k + 1]); } *d = r; return;   /* phaddsw */
    case 0x05: for (int k = 0; k < 4; k++) { r.w[k] = (u16)(d->w[2 * k] - d->w[2 * k + 1]); r.w[4 + k] = (u16)(s.w[2 * k] - s.w[2 * k + 1]); } *d = r; return;   /* phsubw */
    case 0x06: for (int k = 0; k < 2; k++) { r.d[k] = d->d[2 * k] - d->d[2 * k + 1]; r.d[2 + k] = s.d[2 * k] - s.d[2 * k + 1]; } *d = r; return;                  /* phsubd */
    case 0x07: for (int k = 0; k < 4; k++) { r.w[k] = (u16)sat_s16((s16)d->w[2 * k] - (s16)d->w[2 * k + 1]); r.w[4 + k] = (u16)sat_s16((s16)s.w[2 * k] - (s16)s.w[2 * k + 1]); } *d = r; return;   /* phsubsw */
    case 0x04: for (int k = 0; k < 8; k++) r.w[k] = (u16)sat_s16((int)d->b[2 * k] * (s8)s.b[2 * k] + (int)d->b[2 * k + 1] * (s8)s.b[2 * k + 1]); *d = r; return;   /* pmaddubsw */
    case 0x08: for (int k = 0; k < 16; k++) d->b[k] = (s8)s.b[k] < 0 ? (u8)-(s8)d->b[k] : (s8)s.b[k] == 0 ? 0 : d->b[k]; return;                      /* psignb */
    case 0x09: for (int k = 0; k < 8; k++) d->w[k] = (s16)s.w[k] < 0 ? (u16)-(s16)d->w[k] : (s16)s.w[k] == 0 ? 0 : d->w[k]; return;                   /* psignw */
    case 0x0A: for (int k = 0; k < 4; k++) d->d[k] = (s32)s.d[k] < 0 ? (u32)-(s32)d->d[k] : (s32)s.d[k] == 0 ? 0 : d->d[k]; return;                   /* psignd */
    case 0x0B: for (int k = 0; k < 8; k++) d->w[k] = (u16)((((s32)(s16)d->w[k] * (s32)(s16)s.w[k] >> 14) + 1) >> 1); return;                           /* pmulhrsw */
    case 0x10: for (int k = 0; k < 16; k++) if (c->x[0].b[k] & 0x80) d->b[k] = s.b[k]; return;                                                      /* pblendvb */
    case 0x14: for (int k = 0; k < 4; k++) if (c->x[0].d[k] & 0x80000000u) d->d[k] = s.d[k]; return;                                               /* blendvps */
    case 0x15: for (int k = 0; k < 2; k++) if (c->x[0].q[k] >> 63) d->q[k] = s.q[k]; return;                                                        /* blendvpd */
    case 0x17: set_zf_cf_only(c, ((d->q[0] & s.q[0]) | (d->q[1] & s.q[1])) == 0, ((~d->q[0] & s.q[0]) | (~d->q[1] & s.q[1])) == 0); return;         /* ptest */
    case 0x1C: for (int k = 0; k < 16; k++) d->b[k] = (s8)s.b[k] < 0 ? (u8)-(s8)s.b[k] : s.b[k]; return;                                           /* pabsb */
    case 0x1D: for (int k = 0; k < 8; k++) d->w[k] = (s16)s.w[k] < 0 ? (u16)-(s16)s.w[k] : s.w[k]; return;
    case 0x1E: for (int k = 0; k < 4; k++) d->d[k] = (s32)s.d[k] < 0 ? (u32)-(s32)s.d[k] : s.d[k]; return;
    case 0x20: for (int k = 0; k < 8; k++) r.w[k] = (u16)(s16)(s8)s.b[k]; *d = r; return;                                                         /* pmovsxbw */
    case 0x21: for (int k = 0; k < 4; k++) r.d[k] = (u32)(s32)(s8)s.b[k]; *d = r; return;                                                         /* pmovsxbd */
    case 0x22: for (int k = 0; k < 2; k++) r.q[k] = (u64)(s64)(s8)s.b[k]; *d = r; return;                                                         /* pmovsxbq */
    case 0x23: for (int k = 0; k < 4; k++) r.d[k] = (u32)(s32)(s16)s.w[k]; *d = r; return;                                                        /* pmovsxwd */
    case 0x24: for (int k = 0; k < 2; k++) r.q[k] = (u64)(s64)(s16)s.w[k]; *d = r; return;                                                        /* pmovsxwq */
    case 0x25: for (int k = 0; k < 2; k++) r.q[k] = (u64)(s64)(s32)s.d[k]; *d = r; return;                                                        /* pmovsxdq */
    case 0x30: for (int k = 0; k < 8; k++) r.w[k] = s.b[k]; *d = r; return;                                                                       /* pmovzxbw */
    case 0x31: for (int k = 0; k < 4; k++) r.d[k] = s.b[k]; *d = r; return;
    case 0x32: for (int k = 0; k < 2; k++) r.q[k] = s.b[k]; *d = r; return;
    case 0x33: for (int k = 0; k < 4; k++) r.d[k] = s.w[k]; *d = r; return;
    case 0x34: for (int k = 0; k < 2; k++) r.q[k] = s.w[k]; *d = r; return;
    case 0x35: for (int k = 0; k < 2; k++) r.q[k] = s.d[k]; *d = r; return;
    case 0x28: d->q[0] = (u64)((s64)(s32)d->d[0] * (s64)(s32)s.d[0]); d->q[1] = (u64)((s64)(s32)d->d[2] * (s64)(s32)s.d[2]); return;              /* pmuldq */
    case 0x29: for (int k = 0; k < 2; k++) d->q[k] = d->q[k] == s.q[k] ? ~0ull : 0; return;                                                       /* pcmpeqq */
    case 0x2A: *d = s; return;                                                                                                                      /* movntdqa */
    case 0x2B: for (int k = 0; k < 4; k++) { r.w[k] = sat_u16((s32)d->d[k]); r.w[4 + k] = sat_u16((s32)s.d[k]); } *d = r; return;                 /* packusdw */
    case 0x37: for (int k = 0; k < 2; k++) d->q[k] = (s64)d->q[k] > (s64)s.q[k] ? ~0ull : 0; return;                                             /* pcmpgtq */
    case 0x38: for (int k = 0; k < 16; k++) d->b[k] = (s8)d->b[k] < (s8)s.b[k] ? d->b[k] : s.b[k]; return;                                       /* pminsb */
    case 0x39: for (int k = 0; k < 4; k++) d->d[k] = (s32)d->d[k] < (s32)s.d[k] ? d->d[k] : s.d[k]; return;                                      /* pminsd */
    case 0x3A: for (int k = 0; k < 8; k++) d->w[k] = d->w[k] < s.w[k] ? d->w[k] : s.w[k]; return;                                                /* pminuw */
    case 0x3B: for (int k = 0; k < 4; k++) d->d[k] = d->d[k] < s.d[k] ? d->d[k] : s.d[k]; return;                                                /* pminud */
    case 0x3C: for (int k = 0; k < 16; k++) d->b[k] = (s8)d->b[k] > (s8)s.b[k] ? d->b[k] : s.b[k]; return;                                       /* pmaxsb */
    case 0x3D: for (int k = 0; k < 4; k++) d->d[k] = (s32)d->d[k] > (s32)s.d[k] ? d->d[k] : s.d[k]; return;                                      /* pmaxsd */
    case 0x3E: for (int k = 0; k < 8; k++) d->w[k] = d->w[k] > s.w[k] ? d->w[k] : s.w[k]; return;                                                /* pmaxuw */
    case 0x3F: for (int k = 0; k < 4; k++) d->d[k] = d->d[k] > s.d[k] ? d->d[k] : s.d[k]; return;                                                /* pmaxud */
    case 0x40: for (int k = 0; k < 4; k++) d->d[k] = (u32)((s32)d->d[k] * (s32)s.d[k]); return;                                                  /* pmulld */
    case 0x41: { int mi = 0; for (int k = 1; k < 8; k++) if (s.w[k] < s.w[mi]) mi = k; r.w[0] = s.w[mi]; r.w[1] = (u16)mi; *d = r; return; }      /* phminposuw */
    case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: c->unknown_op[0] = 0x0F; c->unknown_op[1] = 0x38; c->unknown_op[2] = op2; UNSUP("AES-NI 0F 38 %02X (cpuid does not advertise AES)", op2);
    default: c->unknown_op[0] = 0x0F; c->unknown_op[1] = 0x38; c->unknown_op[2] = op2; UNSUP("unsupported opcode 66 0F 38 %02X", op2);
  }
}

static void op_0f3a(Ins *i) {
  Cpu *c = i->c; u8 op2 = fetch8(i);
  if (!i->p66) { c->unknown_op[0] = 0x0F; c->unknown_op[1] = 0x3A; c->unknown_op[2] = op2; UNSUP("unsupported MMX opcode 0F 3A %02X", op2); }
  if (op2 == 0x60) { pcmpstr(i, 1, 1); return; } if (op2 == 0x61) { pcmpstr(i, 1, 0); return; } if (op2 == 0x62) { pcmpstr(i, 0, 1); return; } if (op2 == 0x63) { pcmpstr(i, 0, 0); return; }
  decode_modrm(i); fix_rip_rel(i, 1);
  Xmm *d = &c->x[i->reg], r = { { 0, 0 } };
  switch (op2) {
    case 0x14: { u8 im = fetch8(i); u64 v = d->b[im & 15]; if (i->mod == 3) set_reg(i, i->rm, 4, v); else wr8(c, EA(i), (u8)v); return; }                          /* pextrb */
    case 0x15: { u8 im = fetch8(i); u64 v = d->w[im & 7]; if (i->mod == 3) set_reg(i, i->rm, 4, v); else wr16(c, EA(i), (u16)v); return; }                         /* pextrw */
    case 0x16: { u8 im = fetch8(i); if (i->rexw) { u64 v = d->q[im & 1]; rm_set(i, 8, v); } else { u64 v = d->d[im & 3]; rm_set(i, 4, v); } return; }              /* pextrd/q */
    case 0x17: { u8 im = fetch8(i); u64 v = d->d[im & 3]; rm_set(i, 4, v); return; }                                                                                /* extractps */
    case 0x20: { u8 im = fetch8(i); u64 v = i->mod == 3 ? get_reg(i, i->rm, 4) : rd8(c, EA(i)); d->b[im & 15] = (u8)v; return; }                                   /* pinsrb */
    case 0x22: { u8 im = fetch8(i); if (i->rexw) d->q[im & 1] = rm_get(i, 8); else d->d[im & 3] = (u32)rm_get(i, 4); return; }                                      /* pinsrd/q */
    case 0x21: { Xmm s; xmm_load(i, &s, i->mod == 3 ? 16 : 4); u8 im = fetch8(i); u32 v = i->mod == 3 ? s.d[(im >> 6) & 3] : s.d[0]; d->d[(im >> 4) & 3] = v;      /* insertps */
      for (int k = 0; k < 4; k++) if (im & (1 << k)) d->d[k] = 0; return; }
    default: break;
  }
  Xmm s; xmm_load(i, &s, (op2 == 0x0A) ? 4 : (op2 == 0x0B) ? 8 : 16); u8 im = fetch8(i);
  switch (op2) {
    case 0x08: for (int k = 0; k < 4; k++) d->f32[k] = (float)sse_round(s.f32[k], round_mode(c, im)); return;                                     /* roundps */
    case 0x09: for (int k = 0; k < 2; k++) d->f64[k] = sse_round(s.f64[k], round_mode(c, im)); return;                                            /* roundpd */
    case 0x0A: d->f32[0] = (float)sse_round(s.f32[0], round_mode(c, im)); return;                                                                 /* roundss */
    case 0x0B: d->f64[0] = sse_round(s.f64[0], round_mode(c, im)); return;                                                                        /* roundsd */
    case 0x0C: for (int k = 0; k < 4; k++) if (im & (1 << k)) d->d[k] = s.d[k]; return;                                                           /* blendps */
    case 0x0D: for (int k = 0; k < 2; k++) if (im & (1 << k)) d->q[k] = s.q[k]; return;                                                           /* blendpd */
    case 0x0E: for (int k = 0; k < 8; k++) if (im & (1 << k)) d->w[k] = s.w[k]; return;                                                           /* pblendw */
    case 0x0F: { u8 t[32]; e_memcpy(t, s.b, 16); e_memcpy(t + 16, d->b, 16); for (int k = 0; k < 16; k++) r.b[k] = (im + k < 32) ? t[im + k] : 0; *d = r; return; }   /* palignr */
    case 0x40: { float sum = 0; for (int k = 0; k < 4; k++) if (im & (0x10 << k)) sum += d->f32[k] * s.f32[k]; for (int k = 0; k < 4; k++) r.f32[k] = (im & (1 << k)) ? sum : 0; *d = r; return; }   /* dpps */
    case 0x41: { double sum = 0; for (int k = 0; k < 2; k++) if (im & (0x10 << k)) sum += d->f64[k] * s.f64[k]; for (int k = 0; k < 2; k++) r.f64[k] = (im & (1 << k)) ? sum : 0; *d = r; return; }  /* dppd */
    case 0x44: { u64 a = d->q[im & 1], b = s.q[(im >> 4) & 1]; u64 lo = 0, hi = 0;                                                                  /* pclmulqdq */
      for (int k = 0; k < 64; k++) if ((b >> k) & 1) { lo ^= a << k; if (k) hi ^= a >> (64 - k); } r.q[0] = lo; r.q[1] = hi; *d = r; return; }
    case 0x42: { int so = (im & 4) ? 4 : 0, bo = (im & 3) * 4; for (int k = 0; k < 8; k++) { u32 sum = 0; for (int j = 0; j < 4; j++) { int x = d->b[so + k + j] - s.b[bo + j]; sum += x < 0 ? -x : x; } r.w[k] = (u16)sum; } *d = r; return; }   /* mpsadbw */
    default: c->unknown_op[0] = 0x0F; c->unknown_op[1] = 0x3A; c->unknown_op[2] = op2; UNSUP("unsupported opcode 66 0F 3A %02X", op2);
  }
}

/* SSE3 horizontal / add-sub ops: 66 0F 7C haddpd, F2 0F 7C haddps, 66 0F 7D hsubpd, F2 0F 7D hsubps, 66 0F D0 addsubpd, F2 0F D0 addsubps */
static void op_sse3_h(Ins *i, u8 op) {
  Cpu *c = i->c; decode_modrm(i); Xmm s, *d = &c->x[i->reg], r; xmm_load(i, &s, 16);
  if (op == 0xD0) { if (i->p66) { r.f64[0] = d->f64[0] - s.f64[0]; r.f64[1] = d->f64[1] + s.f64[1]; } else { r.f32[0] = d->f32[0] - s.f32[0]; r.f32[1] = d->f32[1] + s.f32[1]; r.f32[2] = d->f32[2] - s.f32[2]; r.f32[3] = d->f32[3] + s.f32[3]; } *d = r; return; }
  int sub = op == 0x7D;
  if (i->p66) { r.f64[0] = sub ? d->f64[0] - d->f64[1] : d->f64[0] + d->f64[1]; r.f64[1] = sub ? s.f64[0] - s.f64[1] : s.f64[0] + s.f64[1]; }
  else { r.f32[0] = sub ? d->f32[0] - d->f32[1] : d->f32[0] + d->f32[1]; r.f32[1] = sub ? d->f32[2] - d->f32[3] : d->f32[2] + d->f32[3]; r.f32[2] = sub ? s.f32[0] - s.f32[1] : s.f32[0] + s.f32[1]; r.f32[3] = sub ? s.f32[2] - s.f32[3] : s.f32[2] + s.f32[3]; }
  *d = r;
}
