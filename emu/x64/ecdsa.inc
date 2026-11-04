/* ECDSA P-256 signature verification (FIPS 186-4, curve secp256r1) — a straightforward constant-free implementation:
   256-bit numbers as 8 × u32 limbs (little-endian), Montgomery multiplication for both the field prime p and the group order n,
   Jacobian point doubling/addition, double-and-add scalar multiplication, Fermat inversion.  Used by BCryptVerifySignature. */
typedef struct { u32 v[8]; } Bn;
static const Bn EC_P = {{ 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xffffffff }};
static const Bn EC_N = {{ 0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad, 0xffffffff, 0xffffffff, 0x00000000, 0xffffffff }};
static const Bn EC_B = {{ 0x27d2604b, 0x3bce3c3e, 0xcc53b0f6, 0x651d06b0, 0x769886bc, 0xb3ebbd55, 0xaa3a93e7, 0x5ac635d8 }};
static const Bn EC_GX = {{ 0xd898c296, 0xf4a13945, 0x2deb33a0, 0x77037d81, 0x63a440f2, 0xf8bce6e5, 0xe12c4247, 0x6b17d1f2 }};
static const Bn EC_GY = {{ 0x37bf51f5, 0xcbb64068, 0x6b315ece, 0x2bce3357, 0x7c0f9e16, 0x8ee7eb4a, 0xfe1a7f9b, 0x4fe342e2 }};
static int bn_is_zero(const Bn *a) { u32 o = 0; for (int i = 0; i < 8; i++) o |= a->v[i]; return o == 0; }
static int bn_cmp(const Bn *a, const Bn *b) { for (int i = 7; i >= 0; i--) { if (a->v[i] != b->v[i]) return a->v[i] > b->v[i] ? 1 : -1; } return 0; }
static u32 bn_add(Bn *r, const Bn *a, const Bn *b) { u64 c = 0; for (int i = 0; i < 8; i++) { c += (u64)a->v[i] + b->v[i]; r->v[i] = (u32)c; c >>= 32; } return (u32)c; }
static u32 bn_sub(Bn *r, const Bn *a, const Bn *b) { s64 c = 0; for (int i = 0; i < 8; i++) { c += (s64)a->v[i] - b->v[i]; r->v[i] = (u32)c; c >>= 32; } return (u32)(c & 1); }
static void bn_mod_add(Bn *r, const Bn *a, const Bn *b, const Bn *m) { u32 cy = bn_add(r, a, b); if (cy || bn_cmp(r, m) >= 0) bn_sub(r, r, m); }
static void bn_mod_sub(Bn *r, const Bn *a, const Bn *b, const Bn *m) { if (bn_sub(r, a, b)) bn_add(r, r, m); }
static void bn_from_be(Bn *r, const u8 *p) { for (int i = 0; i < 8; i++) r->v[7 - i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3]; }
/* Montgomery: R = 2^256. mont_mul(a, b) = a·b·R⁻¹ mod m.  m0inv = -m⁻¹ mod 2^32. */
static u32 mont_inv32(u32 m0) { u32 x = 1; for (int i = 0; i < 5; i++) x *= 2 - m0 * x; return (u32)(0u - x); }
static void mont_mul(Bn *r, const Bn *a, const Bn *b, const Bn *m, u32 m0inv) {
  u32 t[10]; for (int i = 0; i < 10; i++) t[i] = 0;
  for (int i = 0; i < 8; i++) { u64 c = 0; for (int j = 0; j < 8; j++) { c += (u64)a->v[j] * b->v[i] + t[j]; t[j] = (u32)c; c >>= 32; } c += t[8]; t[8] = (u32)c; t[9] = (u32)(c >> 32);
    u32 u = t[0] * m0inv; c = (u64)u * m->v[0] + t[0]; c >>= 32; for (int j = 1; j < 8; j++) { c += (u64)u * m->v[j] + t[j]; t[j - 1] = (u32)c; c >>= 32; } c += t[8]; t[7] = (u32)c; t[8] = t[9] + (u32)(c >> 32); }
  for (int i = 0; i < 8; i++) r->v[i] = t[i]; if (t[8] || bn_cmp(r, m) >= 0) bn_sub(r, r, m); }
typedef struct { const Bn *m; u32 m0inv; Bn rr; /* R² mod m */ } MontCtx;
static void mont_init(MontCtx *k, const Bn *m) { k->m = m; k->m0inv = mont_inv32(m->v[0]); Bn r; for (int i = 0; i < 8; i++) r.v[i] = 0; /* r = R mod m = 2^256 - m (m > 2^255) */ bn_sub(&r, &r, m); /* 2^256 - m */ k->rr = r; for (int i = 0; i < 256; i++) bn_mod_add(&k->rr, &k->rr, &k->rr, m); /* R·2^256 mod m = R² */ }
static void mont_to(const MontCtx *k, Bn *r, const Bn *a) { mont_mul(r, a, &k->rr, k->m, k->m0inv); }
static void mont_from(const MontCtx *k, Bn *r, const Bn *a) { Bn one = {{ 1, 0, 0, 0, 0, 0, 0, 0 }}; mont_mul(r, a, &one, k->m, k->m0inv); }
static void mont_pow(const MontCtx *k, Bn *r, const Bn *a, const Bn *e) { Bn x = *a, acc; mont_to(k, &acc, &(Bn){{ 1, 0, 0, 0, 0, 0, 0, 0 }}); for (int i = 255; i >= 0; i--) { mont_mul(&acc, &acc, &acc, k->m, k->m0inv); if ((e->v[i >> 5] >> (i & 31)) & 1) mont_mul(&acc, &acc, &x, k->m, k->m0inv); } *r = acc; }
static void mont_inv(const MontCtx *k, Bn *r, const Bn *a) { Bn e; bn_sub(&e, k->m, &(Bn){{ 2, 0, 0, 0, 0, 0, 0, 0 }}); mont_pow(k, r, a, &e); }
/* Jacobian points in Montgomery form over p */
typedef struct { Bn x, y, z; int inf; } Pt;
#define FM(r, a, b) mont_mul(r, a, b, &EC_P, kp->m0inv)
static void pt_double(const MontCtx *kp, Pt *r, const Pt *q) { if (q->inf || bn_is_zero(&q->y)) { r->inf = 1; return; }
  Bn a3; mont_to(kp, &a3, &(Bn){{ 3, 0, 0, 0, 0, 0, 0, 0 }}); Bn zz, m, t, s, yy, x3, y3, z3;
  FM(&zz, &q->z, &q->z); FM(&yy, &q->y, &q->y);
  bn_mod_sub(&t, &q->x, &zz, &EC_P); bn_mod_add(&m, &q->x, &zz, &EC_P); FM(&m, &m, &t); FM(&m, &m, &a3);                 /* M = 3(X-Z²)(X+Z²) (a = -3) */
  FM(&s, &q->x, &yy); bn_mod_add(&s, &s, &s, &EC_P); bn_mod_add(&s, &s, &s, &EC_P);                                        /* S = 4XY² */
  FM(&x3, &m, &m); bn_mod_sub(&x3, &x3, &s, &EC_P); bn_mod_sub(&x3, &x3, &s, &EC_P);                                       /* X' = M² - 2S */
  FM(&t, &yy, &yy); bn_mod_add(&t, &t, &t, &EC_P); bn_mod_add(&t, &t, &t, &EC_P); bn_mod_add(&t, &t, &t, &EC_P);            /* 8Y⁴ */
  bn_mod_sub(&y3, &s, &x3, &EC_P); FM(&y3, &y3, &m); bn_mod_sub(&y3, &y3, &t, &EC_P);                                      /* Y' = M(S - X') - 8Y⁴ */
  FM(&z3, &q->y, &q->z); bn_mod_add(&z3, &z3, &z3, &EC_P);                                                                  /* Z' = 2YZ */
  r->x = x3; r->y = y3; r->z = z3; r->inf = 0; }
static void pt_add(const MontCtx *kp, Pt *r, const Pt *a, const Pt *b) { if (a->inf) { *r = *b; return; } if (b->inf) { *r = *a; return; }
  Bn z1z1, z2z2, u1, u2, s1, s2, h, rr, t, x3, y3, z3;
  FM(&z1z1, &a->z, &a->z); FM(&z2z2, &b->z, &b->z); FM(&u1, &a->x, &z2z2); FM(&u2, &b->x, &z1z1);
  FM(&s1, &a->y, &b->z); FM(&s1, &s1, &z2z2); FM(&s2, &b->y, &a->z); FM(&s2, &s2, &z1z1);
  bn_mod_sub(&h, &u2, &u1, &EC_P); bn_mod_sub(&rr, &s2, &s1, &EC_P);
  if (bn_is_zero(&h)) { if (bn_is_zero(&rr)) { pt_double(kp, r, a); return; } r->inf = 1; return; }
  Bn hh, hhh, v; FM(&hh, &h, &h); FM(&hhh, &hh, &h); FM(&v, &u1, &hh);
  FM(&x3, &rr, &rr); bn_mod_sub(&x3, &x3, &hhh, &EC_P); bn_mod_sub(&x3, &x3, &v, &EC_P); bn_mod_sub(&x3, &x3, &v, &EC_P);
  bn_mod_sub(&t, &v, &x3, &EC_P); FM(&y3, &rr, &t); FM(&t, &s1, &hhh); bn_mod_sub(&y3, &y3, &t, &EC_P);
  FM(&z3, &a->z, &b->z); FM(&z3, &z3, &h);
  r->x = x3; r->y = y3; r->z = z3; r->inf = 0; }
static void pt_mul(const MontCtx *kp, Pt *r, const Pt *p, const Bn *k) { Pt acc; acc.inf = 1; for (int i = 255; i >= 0; i--) { pt_double(kp, &acc, &acc); if ((k->v[i >> 5] >> (i & 31)) & 1) pt_add(kp, &acc, &acc, p); } *r = acc; }
/* 1 = valid, 0 = invalid.  qx/qy: public key (big-endian 32 bytes each); hash: message digest (any length, truncated/left-aligned per FIPS); r||s big-endian. */
static int ecdsa_p256_verify(const u8 *qx, const u8 *qy, const u8 *hash, u32 hlen, const u8 *sig) {
  MontCtx kp_, kn; const MontCtx *kp = &kp_; mont_init(&kp_, &EC_P); mont_init(&kn, &EC_N);
  Bn r, s, e, x, y; bn_from_be(&r, sig); bn_from_be(&s, sig + 32); bn_from_be(&x, qx); bn_from_be(&y, qy);
  if (bn_is_zero(&r) || bn_is_zero(&s) || bn_cmp(&r, &EC_N) >= 0 || bn_cmp(&s, &EC_N) >= 0) return 0;
  if (bn_cmp(&x, &EC_P) >= 0 || bn_cmp(&y, &EC_P) >= 0) return 0;
  u8 hb[32]; for (int i = 0; i < 32; i++) hb[i] = i < (int)hlen ? hash[i] : 0; if (hlen < 32) { for (int i = 31; i >= 0; i--) hb[i] = i >= 32 - (int)hlen ? hash[i - (32 - hlen)] : 0; } bn_from_be(&e, hb); if (bn_cmp(&e, &EC_N) >= 0) bn_sub(&e, &e, &EC_N);
  /* point on curve: y² = x³ - 3x + b */
  Bn xm, ym, lhs, rhs, t, bm; mont_to(kp, &xm, &x); mont_to(kp, &ym, &y); mont_to(kp, &bm, &EC_B); FM(&lhs, &ym, &ym); FM(&rhs, &xm, &xm); FM(&rhs, &rhs, &xm); bn_mod_sub(&rhs, &rhs, &xm, &EC_P); bn_mod_sub(&rhs, &rhs, &xm, &EC_P); bn_mod_sub(&rhs, &rhs, &xm, &EC_P); bn_mod_add(&rhs, &rhs, &bm, &EC_P);
  if (bn_cmp(&lhs, &rhs) != 0) return 0;
  /* w = s⁻¹ mod n ; u1 = e·w ; u2 = r·w */
  Bn sm, w, em, rm, u1, u2; mont_to(&kn, &sm, &s); mont_inv(&kn, &w, &sm); mont_to(&kn, &em, &e); mont_to(&kn, &rm, &r);
  mont_mul(&u1, &em, &w, &EC_N, kn.m0inv); mont_from(&kn, &u1, &u1); mont_mul(&u2, &rm, &w, &EC_N, kn.m0inv); mont_from(&kn, &u2, &u2);
  Pt G, Q, A, B, R; mont_to(kp, &G.x, &EC_GX); mont_to(kp, &G.y, &EC_GY); mont_to(kp, &G.z, &(Bn){{ 1, 0, 0, 0, 0, 0, 0, 0 }}); G.inf = 0;
  Q.x = xm; Q.y = ym; Q.z = G.z; Q.inf = 0;
  pt_mul(kp, &A, &G, &u1); pt_mul(kp, &B, &Q, &u2); pt_add(kp, &R, &A, &B); if (R.inf) return 0;
  Bn zi, zi2, rx; mont_inv(kp, &zi, &R.z); FM(&zi2, &zi, &zi); FM(&rx, &R.x, &zi2); mont_from(kp, &rx, &rx);
  if (bn_cmp(&rx, &EC_N) >= 0) bn_sub(&rx, &rx, &EC_N); (void)t;
  return bn_cmp(&rx, &r) == 0; }
#undef FM
