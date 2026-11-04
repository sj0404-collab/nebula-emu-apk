/* dxbc.c — DXBC (Shader Model 4.x/5.0 token stream) → GLSL ES 3.00 translator for the Nebula x64 D3D11 layer.
 *
 *  Scope: the instruction subset that fxc emits for ordinary vs_4_0 / ps_4_0 (and the vs/ps_4_0_level_9_x containers,
 *  whose SHDR chunk is plain SM4).  Anything outside the subset is reported by opcode number — never silently skipped.
 *  Linking: varyings are named after the D3D semantic (TEXCOORD0 → va_TEXCOORD0) so a VS/PS pair links by semantic
 *  exactly like D3D does; SV_Position becomes gl_Position; SV_Target# becomes the fragment output #.
 *  Constant buffers become std140 uniform blocks `cb#` with a vec4 array; textures t# are sampler2D uniforms. */
#include "emu.h"
#include "dxbc.h"

typedef struct { const u8 *p; size_t n; } Span;
typedef struct {
  char *out; size_t cap, len; int err;
  int type;                       /* 0 pixel, 1 vertex */
  DxbcInfo *info;
  int ntemps; int tex_dim[16]; int tex_used[16]; int cb_size[16];
  int indent;
} Tr;

static void emit(Tr *t, const char *fmt, ...) {
  if (t->err) return; char buf[1024]; __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = e_vsnprintf(buf, sizeof buf, fmt, ap); __builtin_va_end(ap);
  if (n < 0) return; if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
  if (t->len + (size_t)n + 1 > t->cap) { t->err = 2; return; }
  e_memcpy(t->out + t->len, buf, (size_t)n); t->len += (size_t)n; t->out[t->len] = 0;
}
static void fail(Tr *t, const char *fmt, ...) {
  if (t->err) return; char buf[256]; __builtin_va_list ap; __builtin_va_start(ap, fmt); e_vsnprintf(buf, sizeof buf, fmt, ap); __builtin_va_end(ap);
  t->len = 0; t->out[0] = 0; emit(t, "%s", buf); t->err = 1;
}
static u32 ld32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* ---- signatures ---- */
static void read_sig(Tr *t, Span s, DxbcSig *sig) {
  if (s.n < 8) return; u32 cnt = ld32(s.p); if (cnt > DXBC_MAX_SIG) cnt = DXBC_MAX_SIG; sig->n = 0;
  for (u32 i = 0; i < cnt; i++) { const u8 *e = s.p + 8 + 24 * i; if ((size_t)(e + 24 - s.p) > s.n) break; DxbcSigElem *d = &sig->e[sig->n++];
    u32 no = ld32(e); const char *nm = (const char *)s.p + no; size_t k = 0; while (no + k < s.n && nm[k] && k < 31) { d->name[k] = nm[k]; k++; } d->name[k] = 0;
    d->index = (int)ld32(e + 4); d->sysval = (int)ld32(e + 8); d->ctype = (int)ld32(e + 12); d->reg = (int)ld32(e + 16); d->mask = e[20]; d->rwmask = e[21]; }
}
static const DxbcSigElem *sig_by_reg(const DxbcSig *s, int reg) { for (int i = 0; i < s->n; i++) if (s->e[i].reg == reg) return &s->e[i]; return 0; }
static int is_position(const DxbcSigElem *e) { return e && (e->sysval == 1 || !e_stricmp(e->name, "SV_Position")); }
static int is_target(const DxbcSigElem *e) { return e && !e_stricmp(e->name, "SV_Target"); }

/* ---- operands ---- */
typedef struct { int type, ncomp, selmode; u32 mask, swz; int ndim; u32 idx[3]; int rel[3]; char relexpr[3][96]; u32 imm[4]; int neg, abs_; } Opnd;
static const char *swzc = "xyzw";
static int parse_operand(Tr *t, const u32 *w, int avail, Opnd *o);
static int operand_index(Tr *t, const u32 *w, int avail, int rep, Opnd *o, int d, int *used) {
  *used = 0; o->rel[d] = 0; o->idx[d] = 0;
  switch (rep) {
    case 0: if (avail < 1) return -1; o->idx[d] = w[0]; *used = 1; return 0;
    case 1: if (avail < 2) return -1; o->idx[d] = w[0]; *used = 2; return 0;
    case 2: case 3: { int base = rep == 3 ? 1 : 0; if (avail < base + 1) return -1; if (rep == 3) o->idx[d] = w[0]; Opnd r; int n = parse_operand(t, w + base, avail - base, &r); if (n < 0) return -1;
      /* relative index: a single selected component of a temp/input */
      const char *reg = r.type == 0 ? "r" : r.type == 1 ? "v" : r.type == 3 ? "x" : "r"; int comp = r.selmode == 2 ? (int)(r.swz & 3) : (int)(r.swz & 3);
      e_snprintf(o->relexpr[d], sizeof o->relexpr[d], "int(floatBitsToInt(%s%u).%c)", reg, r.idx[0], swzc[comp]); o->rel[d] = 1; *used = base + n; return 0; }
    default: return -1;
  }
}
static int parse_operand(Tr *t, const u32 *w, int avail, Opnd *o) {
  if (avail < 1) return -1; u32 tok = w[0]; int n = 1; e_memset(o, 0, sizeof *o);
  o->ncomp = (int)(tok & 3); o->type = (int)((tok >> 12) & 0xff); o->ndim = (int)((tok >> 20) & 3);
  if (o->ncomp == 2) { o->selmode = (int)((tok >> 2) & 3); if (o->selmode == 0) { o->mask = (tok >> 4) & 0xf; o->swz = 0xE4; } else if (o->selmode == 1) { o->swz = (tok >> 4) & 0xff; o->mask = 0xf; } else { u32 c = (tok >> 4) & 3; o->swz = c | c << 2 | c << 4 | c << 6; o->mask = 0xf; } }
  else { o->mask = 0xf; o->swz = 0xE4; }
  u32 ext = tok >> 31; const u32 *q = w + 1;
  while (ext) { if (avail < n + 1) return -1; u32 e = *q++; n++; if ((e & 0x3f) == 1) { u32 m = (e >> 6) & 0xff; o->neg = m == 1 || m == 3; o->abs_ = m == 2 || m == 3; } ext = e >> 31; }
  if (o->type == 4) { int k = o->ncomp == 1 ? 1 : 4; if (avail < n + k) return -1; for (int i = 0; i < k; i++) o->imm[i] = q[i]; q += k; n += k; return n; }
  if (o->type == 5) { int k = o->ncomp == 1 ? 2 : 8; if (avail < n + k) return -1; q += k; n += k; return n; }
  for (int d = 0; d < o->ndim; d++) { int rep = (int)((tok >> (22 + 3 * d)) & 7); int used; if (operand_index(t, q, avail - n, rep, o, d, &used) < 0) return -1; q += used; n += used; }
  return n;
}
static int mask_count(u32 m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }
static void fmt_imm(char *b, size_t cap, u32 v) {
  union { u32 u; float f; } c; c.u = v; float f = c.f; u32 ex = (v >> 23) & 0xff;
  if (v == 0) e_snprintf(b, cap, "0.0");
  else if (ex != 0 && ex != 0xff && (f > 1e-30f || f < -1e-30f) && (f < 1e30f && f > -1e30f)) e_snprintf(b, cap, "%.9g", (double)f);
  else e_snprintf(b, cap, "uintBitsToFloat(%uu)", v);
  /* make sure a float literal has a dot or exponent */
  int dot = 0; for (char *p = b; *p; p++) if (*p == '.' || *p == 'e' || *p == 'u' || *p == 'n') dot = 1; if (!dot) { size_t l = e_strlen(b); e_snprintf(b + l, cap - l, ".0"); }
}
/* source operand as a vec4 expression (swizzle applied) */
static void src_expr(Tr *t, const Opnd *o, char *b, size_t cap) {
  char base[160]; base[0] = 0;
  switch (o->type) {
    case 0: e_snprintf(base, sizeof base, "r%u", o->idx[0]); break;
    case 1: if (t->type == 0) { const DxbcSigElem *e = sig_by_reg(&t->info->in, (int)o->idx[0]); if (is_position(e)) e_snprintf(base, sizeof base, "gl_FragCoord"); else e_snprintf(base, sizeof base, "v%u", o->idx[0]); } else e_snprintf(base, sizeof base, "v%u", o->idx[0]); break;
    case 3: e_snprintf(base, sizeof base, "x%u[%s%s%u]", o->idx[0], o->rel[1] ? o->relexpr[1] : "", o->rel[1] ? "+" : "", o->idx[1]); break;
    case 4: { char c[4][48]; if (o->ncomp == 1) { fmt_imm(c[0], 48, o->imm[0]); e_snprintf(base, sizeof base, "vec4(%s)", c[0]); } else { for (int i = 0; i < 4; i++) fmt_imm(c[i], 48, o->imm[i]); e_snprintf(base, sizeof base, "vec4(%s, %s, %s, %s)", c[0], c[1], c[2], c[3]); } break; }
    case 8: e_snprintf(base, sizeof base, "%s_cb%u_data[%s%s%u]", t->type ? "vs" : "ps", o->idx[0], o->rel[1] ? o->relexpr[1] : "", o->rel[1] ? "+" : "", o->idx[1]); break;
    case 9: e_snprintf(base, sizeof base, "icb[%s%s%u]", o->rel[0] ? o->relexpr[0] : "", o->rel[0] ? "+" : "", o->idx[0]); break;
    case 11: e_snprintf(base, sizeof base, "vec4(intBitsToFloat(gl_PrimitiveID))"); break;
    case 22: e_snprintf(base, sizeof base, "vec4(intBitsToFloat(gl_VertexID))"); break;   /* not an SM4 operand type; kept for siv inputs */
    default: fail(t, "DXBC: unsupported source operand type %d", o->type); e_snprintf(base, sizeof base, "vec4(0.0)"); break;
  }
  char sw[8]; int k = 0; if (o->swz != 0xE4) { sw[k++] = '.'; for (int i = 0; i < 4; i++) sw[k++] = swzc[(o->swz >> (2 * i)) & 3]; } sw[k] = 0;
  if (o->abs_ && o->neg) e_snprintf(b, cap, "(-abs(%s%s))", base, sw); else if (o->abs_) e_snprintf(b, cap, "abs(%s%s)", base, sw); else if (o->neg) e_snprintf(b, cap, "(-%s%s)", base, sw); else e_snprintf(b, cap, "%s%s", base, sw);
}
/* destination: returns the lvalue and the write mask string */
static void dst_expr(Tr *t, const Opnd *o, char *b, size_t cap, char *msk, size_t mcap) {
  int k = 0; msk[k++] = '.'; for (int i = 0; i < 4; i++) if (o->mask & (1u << i)) msk[k++] = swzc[i]; msk[k] = 0; if (o->mask == 0xf) msk[0] = 0;
  switch (o->type) {
    case 0: e_snprintf(b, cap, "r%u", o->idx[0]); break;
    case 2: { const DxbcSigElem *e = sig_by_reg(&t->info->out, (int)o->idx[0]);
      if (t->type == 1 && is_position(e)) e_snprintf(b, cap, "gl_Position");
      else if (t->type == 0 && is_target(e)) e_snprintf(b, cap, "o%u", o->idx[0]);
      else if (t->type == 0 && e && !e_stricmp(e->name, "SV_Depth")) e_snprintf(b, cap, "nebula_depth");
      else if (e) e_snprintf(b, cap, "va_%s%d", e->name, e->index); else e_snprintf(b, cap, "o%u", o->idx[0]); break; }
    case 3: e_snprintf(b, cap, "x%u[%s%s%u]", o->idx[0], o->rel[1] ? o->relexpr[1] : "", o->rel[1] ? "+" : "", o->idx[1]); break;
    case 12: e_snprintf(b, cap, "nebula_depth"); break;
    case 13: e_snprintf(b, cap, "nebula_null"); break;
    default: fail(t, "DXBC: unsupported destination operand type %d", o->type); e_snprintf(b, cap, "r0"); break;
  }
}
/* swizzle a vec4 expression down to the components selected by the destination mask: expr.xy etc. */
static void masked(const char *expr, const char *msk, char *b, size_t cap) { if (msk[0]) e_snprintf(b, cap, "(%s)%s", expr, msk); else e_snprintf(b, cap, "%s", expr); }
static void assign(Tr *t, const Opnd *d, const char *expr, int sat) {
  char lv[128], msk[8], rhs[1200], full[1300]; dst_expr(t, d, lv, sizeof lv, msk, sizeof msk);
  if (sat) e_snprintf(full, sizeof full, "clamp(%s, 0.0, 1.0)", expr); else e_snprintf(full, sizeof full, "%s", expr);
  masked(full, msk, rhs, sizeof rhs);
  if (d->type == 13) return;
  if (!e_strcmp(lv, "nebula_depth")) { emit(t, "%*sgl_FragDepth = (%s).x;\n", t->indent, "", full); return; }
  emit(t, "%*s%s%s = %s;\n", t->indent, "", lv, msk, rhs);
}
static const char *bool4(const char *cmp, const char *a, const char *b, char *out, size_t cap) { e_snprintf(out, cap, "uintBitsToFloat(uvec4(%s(%s, %s)) * 0xFFFFFFFFu)", cmp, a, b); return out; }

/* ---- the instruction loop ---- */
static int translate_body(Tr *t, const u32 *w, int nw) {
  int i = 0; char a[512], b[512], c[512], e[1400], e2[600];
  while (i < nw) {
    u32 tok = w[i]; u32 op = tok & 0x7ff; int len = (int)((tok >> 24) & 0x7f); int sat = (tok >> 13) & 1; int nz = (tok >> 18) & 1;
    if (op == 53) { len = (int)w[i + 1]; u32 cls = tok >> 11; if (cls == 3) { int n4 = (len - 2) / 4; emit(t, "const vec4 icb[%d] = vec4[%d](", n4, n4); for (int k = 0; k < n4; k++) { char f[4][48]; for (int j = 0; j < 4; j++) fmt_imm(f[j], 48, w[i + 2 + 4 * k + j]); emit(t, "%svec4(%s, %s, %s, %s)", k ? ", " : "", f[0], f[1], f[2], f[3]); } emit(t, ");\n"); } i += len; continue; }
    if (len <= 0 || i + len > nw) { fail(t, "DXBC: bad instruction length at token %d", i); return -1; }
    const u32 *q = w + i + 1; int avail = len - 1;
    /* extended opcode tokens (sample controls etc.) */
    u32 ext = tok >> 31; while (ext && avail > 0) { u32 x = *q++; avail--; ext = x >> 31; }
    Opnd o[5]; int no = 0;
    #define OPS(k) do { for (no = 0; no < (k); no++) { int n = parse_operand(t, q, avail, &o[no]); if (n < 0) { fail(t, "DXBC: bad operand in opcode %u", op); return -1; } q += n; avail -= n; } } while (0)
    #define SRC(k, buf) src_expr(t, &o[k], buf, sizeof buf)
    switch (op) {
      /* declarations */
      case 89: OPS(1); if (o[0].idx[0] < 16) t->cb_size[o[0].idx[0]] = (int)o[0].idx[1]; break;
      case 88: { OPS(1); u32 dim = (tok >> 11) & 0x1f; if (o[0].idx[0] < 16) { t->tex_dim[o[0].idx[0]] = (int)dim; t->tex_used[o[0].idx[0]] = 1; } break; }
      case 90: OPS(1); break;
      case 95: case 96: case 97: case 98: case 99: case 100: case 101: case 102: case 103: case 91: case 92: case 93: case 94: case 106: break;
      case 104: t->ntemps = (int)q[0]; break;
      case 105: emit(t, "vec4 x%u[%u];\n", q[0], q[1]); break;
      /* arithmetic */
      case 54: OPS(2); SRC(1, a); assign(t, &o[0], a, sat); break;
      case 55: OPS(4); SRC(1, a); SRC(2, b); SRC(3, c); e_snprintf(e, sizeof e, "mix(%s, %s, notEqual(floatBitsToUint(%s), uvec4(0u)))", c, b, a); assign(t, &o[0], e, sat); break;
      case 0: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "(%s + %s)", a, b); assign(t, &o[0], e, sat); break;
      case 56: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "(%s * %s)", a, b); assign(t, &o[0], e, sat); break;
      case 14: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "(%s / %s)", a, b); assign(t, &o[0], e, sat); break;
      case 50: OPS(4); SRC(1, a); SRC(2, b); SRC(3, c); e_snprintf(e, sizeof e, "(%s * %s + %s)", a, b, c); assign(t, &o[0], e, sat); break;
      case 15: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "vec4(dot((%s).xy, (%s).xy))", a, b); assign(t, &o[0], e, sat); break;
      case 16: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "vec4(dot((%s).xyz, (%s).xyz))", a, b); assign(t, &o[0], e, sat); break;
      case 17: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "vec4(dot(%s, %s))", a, b); assign(t, &o[0], e, sat); break;
      case 51: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "min(%s, %s)", a, b); assign(t, &o[0], e, sat); break;
      case 52: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "max(%s, %s)", a, b); assign(t, &o[0], e, sat); break;
      case 75: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "sqrt(%s)", a); assign(t, &o[0], e, sat); break;
      case 68: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "inversesqrt(%s)", a); assign(t, &o[0], e, sat); break;
      case 25: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "exp2(%s)", a); assign(t, &o[0], e, sat); break;
      case 47: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "log2(%s)", a); assign(t, &o[0], e, sat); break;
      case 26: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "fract(%s)", a); assign(t, &o[0], e, sat); break;
      case 64: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "roundEven(%s)", a); assign(t, &o[0], e, sat); break;
      case 65: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "floor(%s)", a); assign(t, &o[0], e, sat); break;
      case 66: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "ceil(%s)", a); assign(t, &o[0], e, sat); break;
      case 67: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "trunc(%s)", a); assign(t, &o[0], e, sat); break;
      case 77: OPS(3); SRC(2, a); if (o[0].type != 13) { e_snprintf(e, sizeof e, "sin(%s)", a); assign(t, &o[0], e, sat); } if (o[1].type != 13) { e_snprintf(e, sizeof e, "cos(%s)", a); assign(t, &o[1], e, sat); } break;
      case 11: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "dFdx(%s)", a); assign(t, &o[0], e, sat); break;
      case 12: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "dFdy(%s)", a); assign(t, &o[0], e, sat); break;
      /* comparisons → 0xFFFFFFFF / 0 masks */
      case 49: OPS(3); SRC(1, a); SRC(2, b); assign(t, &o[0], bool4("lessThan", a, b, e, sizeof e), 0); break;
      case 29: OPS(3); SRC(1, a); SRC(2, b); assign(t, &o[0], bool4("greaterThanEqual", a, b, e, sizeof e), 0); break;
      case 24: OPS(3); SRC(1, a); SRC(2, b); assign(t, &o[0], bool4("equal", a, b, e, sizeof e), 0); break;
      case 57: OPS(3); SRC(1, a); SRC(2, b); assign(t, &o[0], bool4("notEqual", a, b, e, sizeof e), 0); break;
      case 32: case 33: case 34: case 39: { OPS(3); SRC(1, a); SRC(2, b); const char *f = op == 32 ? "equal" : op == 33 ? "greaterThanEqual" : op == 34 ? "lessThan" : "notEqual"; e_snprintf(e2, sizeof e2, "floatBitsToInt(%s)", a); e_snprintf(c, sizeof c, "floatBitsToInt(%s)", b); assign(t, &o[0], bool4(f, e2, c, e, sizeof e), 0); break; }
      case 79: case 80: { OPS(3); SRC(1, a); SRC(2, b); const char *f = op == 79 ? "lessThan" : "greaterThanEqual"; e_snprintf(e2, sizeof e2, "floatBitsToUint(%s)", a); e_snprintf(c, sizeof c, "floatBitsToUint(%s)", b); assign(t, &o[0], bool4(f, e2, c, e, sizeof e), 0); break; }
      /* bitwise / integer */
      case 1: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "uintBitsToFloat(floatBitsToUint(%s) & floatBitsToUint(%s))", a, b); assign(t, &o[0], e, 0); break;
      case 60: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "uintBitsToFloat(floatBitsToUint(%s) | floatBitsToUint(%s))", a, b); assign(t, &o[0], e, 0); break;
      case 87: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "uintBitsToFloat(floatBitsToUint(%s) ^ floatBitsToUint(%s))", a, b); assign(t, &o[0], e, 0); break;
      case 59: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "uintBitsToFloat(~floatBitsToUint(%s))", a); assign(t, &o[0], e, 0); break;
      case 30: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "intBitsToFloat(floatBitsToInt(%s) + floatBitsToInt(%s))", a, b); assign(t, &o[0], e, 0); break;
      case 40: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "intBitsToFloat(-floatBitsToInt(%s))", a); assign(t, &o[0], e, 0); break;
      case 35: case 82: OPS(4); SRC(1, a); SRC(2, b); SRC(3, c); e_snprintf(e, sizeof e, "intBitsToFloat(floatBitsToInt(%s) * floatBitsToInt(%s) + floatBitsToInt(%s))", a, b, c); assign(t, &o[0], e, 0); break;
      case 36: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "intBitsToFloat(max(floatBitsToInt(%s), floatBitsToInt(%s)))", a, b); assign(t, &o[0], e, 0); break;
      case 37: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "intBitsToFloat(min(floatBitsToInt(%s), floatBitsToInt(%s)))", a, b); assign(t, &o[0], e, 0); break;
      case 83: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "uintBitsToFloat(max(floatBitsToUint(%s), floatBitsToUint(%s)))", a, b); assign(t, &o[0], e, 0); break;
      case 84: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "uintBitsToFloat(min(floatBitsToUint(%s), floatBitsToUint(%s)))", a, b); assign(t, &o[0], e, 0); break;
      case 38: case 81: OPS(4); SRC(2, a); SRC(3, b); if (o[1].type != 13) { e_snprintf(e, sizeof e, "intBitsToFloat(floatBitsToInt(%s) * floatBitsToInt(%s))", a, b); assign(t, &o[1], e, 0); } if (o[0].type != 13) { emit(t, "%*s// %s: high 32 bits of the product are not available in GLSL ES; writing 0\n", t->indent, "", op == 38 ? "imul" : "umul"); assign(t, &o[0], "vec4(0.0)", 0); } break;
      case 78: OPS(4); SRC(2, a); SRC(3, b); if (o[0].type != 13) { e_snprintf(e, sizeof e, "uintBitsToFloat(floatBitsToUint(%s) / max(floatBitsToUint(%s), uvec4(1u)))", a, b); assign(t, &o[0], e, 0); } if (o[1].type != 13) { e_snprintf(e, sizeof e, "uintBitsToFloat(floatBitsToUint(%s) %% max(floatBitsToUint(%s), uvec4(1u)))", a, b); assign(t, &o[1], e, 0); } break;
      case 41: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "intBitsToFloat(floatBitsToInt(%s) << (floatBitsToUint(%s) & 31u))", a, b); assign(t, &o[0], e, 0); break;
      case 42: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "intBitsToFloat(floatBitsToInt(%s) >> (floatBitsToUint(%s) & 31u))", a, b); assign(t, &o[0], e, 0); break;
      case 85: OPS(3); SRC(1, a); SRC(2, b); e_snprintf(e, sizeof e, "uintBitsToFloat(floatBitsToUint(%s) >> (floatBitsToUint(%s) & 31u))", a, b); assign(t, &o[0], e, 0); break;
      case 27: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "intBitsToFloat(ivec4(%s))", a); assign(t, &o[0], e, 0); break;
      case 28: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "uintBitsToFloat(uvec4(max(%s, vec4(0.0))))", a); assign(t, &o[0], e, 0); break;
      case 43: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "vec4(floatBitsToInt(%s))", a); assign(t, &o[0], e, sat); break;
      case 86: OPS(2); SRC(1, a); e_snprintf(e, sizeof e, "vec4(floatBitsToUint(%s))", a); assign(t, &o[0], e, sat); break;
      /* textures */
      case 69: case 72: case 74: case 71: case 70: { OPS(op == 69 ? 4 : 5); SRC(1, a); u32 tx = o[2].idx[0], sm = o[3].idx[0]; if (tx < 16 && sm < 16) t->info->tex_sampler[tx] = (int)sm + 1; int dim = tx < 16 ? t->tex_dim[tx] : 3; const char *coord = dim == 2 ? "x" : dim == 5 || dim == 6 ? "xyz" : "xy";
        char swz[8]; { int k = 0; swz[k++] = '.'; for (int j = 0; j < 4; j++) swz[k++] = swzc[(o[2].swz >> (2 * j)) & 3]; swz[k] = 0; if (o[2].swz == 0xE4) swz[0] = 0; }
        if (op == 69) e_snprintf(e, sizeof e, "texture(t%u, (%s).%s)%s", tx, a, coord, swz);
        else if (op == 72) { SRC(4, b); e_snprintf(e, sizeof e, "textureLod(t%u, (%s).%s, (%s).x)%s", tx, a, coord, b, swz); }
        else if (op == 74) { SRC(4, b); e_snprintf(e, sizeof e, "texture(t%u, (%s).%s, (%s).x)%s", tx, a, coord, b, swz); }
        else { SRC(4, b); e_snprintf(e, sizeof e, "vec4(float(texture(t%u, (%s).%s).x >= (%s).x))", tx, a, coord, b); }
        assign(t, &o[0], e, sat); break; }
      case 45: { OPS(3); SRC(1, a); u32 tx = o[2].idx[0]; int dim = tx < 16 ? t->tex_dim[tx] : 3; char swz[8]; { int k = 0; swz[k++] = '.'; for (int j = 0; j < 4; j++) swz[k++] = swzc[(o[2].swz >> (2 * j)) & 3]; swz[k] = 0; if (o[2].swz == 0xE4) swz[0] = 0; }
        if (dim == 1) e_snprintf(e, sizeof e, "texelFetch(t%u, floatBitsToInt(%s).x)%s", tx, a, swz); else e_snprintf(e, sizeof e, "texelFetch(t%u, floatBitsToInt(%s).xy, floatBitsToInt(%s).w)%s", tx, a, a, swz);
        assign(t, &o[0], e, sat); break; }
      case 61: { OPS(3); SRC(1, a); u32 tx = o[2].idx[0]; e_snprintf(e, sizeof e, "vec4(vec2(textureSize(t%u, floatBitsToInt(%s).x)), float(textureQueryLevels(t%u)), 0.0)", tx, a, tx); assign(t, &o[0], e, 0); break; }
      /* control flow */
      case 31: OPS(1); SRC(0, a); emit(t, "%*sif (floatBitsToUint(%s).x %s 0u) {\n", t->indent, "", a, nz ? "!=" : "=="); t->indent += 2; break;
      case 18: t->indent -= 2; emit(t, "%*s} else {\n", t->indent, ""); t->indent += 2; break;
      case 21: t->indent -= 2; emit(t, "%*s}\n", t->indent, ""); break;
      case 48: emit(t, "%*sfor (int nebula_guard = 0; nebula_guard < 65536; nebula_guard++) {\n", t->indent, ""); t->indent += 2; break;
      case 22: t->indent -= 2; emit(t, "%*s}\n", t->indent, ""); break;
      case 2: emit(t, "%*sbreak;\n", t->indent, ""); break;
      case 7: emit(t, "%*scontinue;\n", t->indent, ""); break;
      case 3: OPS(1); SRC(0, a); emit(t, "%*sif (floatBitsToUint(%s).x %s 0u) break;\n", t->indent, "", a, nz ? "!=" : "=="); break;
      case 8: OPS(1); SRC(0, a); emit(t, "%*sif (floatBitsToUint(%s).x %s 0u) continue;\n", t->indent, "", a, nz ? "!=" : "=="); break;
      case 13: OPS(1); SRC(0, a); emit(t, "%*sif (floatBitsToUint(%s).x %s 0u) discard;\n", t->indent, "", a, nz ? "!=" : "=="); break;
      case 62: if (t->indent > 2) emit(t, "%*sreturn;\n", t->indent, ""); break;
      case 63: OPS(1); SRC(0, a); emit(t, "%*sif (floatBitsToUint(%s).x %s 0u) return;\n", t->indent, "", a, nz ? "!=" : "=="); break;
      case 58: break;
      default: fail(t, "DXBC: unsupported opcode %u (SM4 token %d)", op, i); return -1;
    }
    if (t->err) return -1;
    i += len;
  }
  return 0;
}

int dxbc_to_glsl(const u8 *bytes, size_t len, char *out, size_t cap, DxbcInfo *info) {
  Tr t; e_memset(&t, 0, sizeof t); t.out = out; t.cap = cap; t.info = info; out[0] = 0; e_memset(info, 0, sizeof *info);
  if (len < 36 || e_memcmp(bytes, "DXBC", 4)) { fail(&t, "DXBC: not a DXBC container"); return -1; }
  u32 nch = ld32(bytes + 28); Span shdr = {0, 0}, isgn = {0, 0}, osgn = {0, 0};
  for (u32 k = 0; k < nch && k < 16; k++) { u32 off = ld32(bytes + 32 + 4 * k); if ((size_t)off + 8 > len) break; u32 cl = ld32(bytes + off + 4); if ((size_t)off + 8 + cl > len) break; Span s = { bytes + off + 8, cl };
    if (!e_memcmp(bytes + off, "SHDR", 4) || !e_memcmp(bytes + off, "SHEX", 4)) shdr = s; else if (!e_memcmp(bytes + off, "ISGN", 4) || !e_memcmp(bytes + off, "ISG1", 4)) isgn = s; else if (!e_memcmp(bytes + off, "OSGN", 4) || !e_memcmp(bytes + off, "OSG1", 4)) osgn = s; }
  if (!shdr.p || shdr.n < 8) { fail(&t, "DXBC: no SHDR/SHEX chunk (SM1-3 shaders need the D3D9 path)"); return -1; }
  read_sig(&t, isgn, &info->in); read_sig(&t, osgn, &info->out);
  u32 ver = ld32(shdr.p); int ptype = (int)(ver >> 16); int major = (int)((ver >> 4) & 0xf); if (ptype > 1) { fail(&t, "DXBC: program type %d (geometry/hull/domain/compute) is not supported", ptype); return -1; }
  t.type = ptype; info->type = ptype; info->sm_major = major;
  /* body first into a scratch area after the header space we reserve, then prepend declarations */
  size_t head_cap = cap / 3; if (head_cap > 8192) head_cap = 8192; char *body = out + head_cap; Tr bt = t; bt.out = body; bt.cap = cap - head_cap; bt.len = 0; bt.indent = 2;
  bt.out[0] = 0;
  int nw = (int)(shdr.n / 4) - 2; u32 *w = (u32 *)emu_alloc((size_t)nw * 4 + 4); for (int k = 0; k < nw; k++) w[k] = ld32(shdr.p + 8 + 4 * k);
  if (translate_body(&bt, w, nw) < 0 || bt.err) { if (bt.err == 2) { fail(&t, "DXBC: shader too large for the GLSL buffer"); return -1; } char msg[256]; e_snprintf(msg, sizeof msg, "%s", bt.out); fail(&t, "%s", msg); return -1; }
  size_t body_len = bt.len; char *body_copy = (char *)emu_alloc(body_len + 1); e_memcpy(body_copy, body, body_len + 1);
  /* header */
  t.len = 0; emit(&t, "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2D;\n// translated from DXBC %s_%d_x by Nebula x64\n", ptype ? "vs" : "ps", major);
  for (int k = 0; k < 16; k++) if (bt.cb_size[k]) { info->cb_size[k] = bt.cb_size[k]; emit(&t, "layout(std140) uniform %s_cb%d { vec4 %s_cb%d_data[%d]; };\n", ptype ? "vs" : "ps", k, ptype ? "vs" : "ps", k, bt.cb_size[k]); }
  for (int k = 0; k < 16; k++) if (bt.tex_used[k]) { info->tex_dim[k] = bt.tex_dim[k]; emit(&t, "uniform %s t%d;\n", bt.tex_dim[k] == 5 ? "sampler3D" : bt.tex_dim[k] == 6 ? "samplerCube" : "sampler2D", k); }
  if (ptype == 1) {
    for (int k = 0; k < info->in.n; k++) { DxbcSigElem *e = &info->in.e[k]; if (e->sysval == 6 || !e_stricmp(e->name, "SV_VertexID")) continue; if (e->sysval == 8 || !e_stricmp(e->name, "SV_InstanceID")) continue; emit(&t, "layout(location = %d) in vec4 v%d;\n", e->reg, e->reg); }
    for (int k = 0; k < info->out.n; k++) { DxbcSigElem *e = &info->out.e[k]; if (is_position(e)) continue; emit(&t, "out vec4 va_%s%d;\n", e->name, e->index); }
  } else {
    for (int k = 0; k < info->in.n; k++) { DxbcSigElem *e = &info->in.e[k]; if (is_position(e)) continue; emit(&t, "in vec4 va_%s%d;\n", e->name, e->index); }
    for (int k = 0; k < info->out.n; k++) { DxbcSigElem *e = &info->out.e[k]; if (is_target(e)) emit(&t, "layout(location = %d) out vec4 o%d;\n", e->reg, e->reg); }
  }
  emit(&t, "void main() {\n");
  for (int k = 0; k < bt.ntemps; k++) emit(&t, "  vec4 r%d = vec4(0.0);\n", k);
  if (ptype == 0) for (int k = 0; k < info->in.n; k++) { DxbcSigElem *e = &info->in.e[k]; if (is_position(e)) continue; emit(&t, "  vec4 v%d = va_%s%d;\n", e->reg, e->name, e->index); }
  if (ptype == 1) for (int k = 0; k < info->in.n; k++) { DxbcSigElem *e = &info->in.e[k]; if (e->sysval == 6 || !e_stricmp(e->name, "SV_VertexID")) emit(&t, "  vec4 v%d = vec4(intBitsToFloat(gl_VertexID));\n", e->reg); else if (e->sysval == 8 || !e_stricmp(e->name, "SV_InstanceID")) emit(&t, "  vec4 v%d = vec4(intBitsToFloat(gl_InstanceID));\n", e->reg); }
  if (t.len + body_len + 64 > head_cap + 0 && t.len + body_len + 64 > cap) { fail(&t, "DXBC: shader too large for the GLSL buffer"); return -1; }
  e_memcpy(out + t.len, body_copy, body_len); t.len += body_len; out[t.len] = 0;
  if (ptype == 1) emit(&t, "  gl_Position.y = -gl_Position.y;\n  gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n");   /* D3D clip space (y down in the viewport, z in [0,1]) → GL */
  emit(&t, "}\n");
  if (t.err) return -1;
  info->ntemps = bt.ntemps; return (int)t.len;
}
