/* bcrypt.dll (CNG) hashing subset: MD5 / SHA-1 / SHA-256 / SHA-384 / SHA-512 and their HMACs, with real algorithm handles
   (BCryptOpenAlgorithmProvider) and the Windows 10 pseudo handles (BCRYPT_SHA256_ALG_HANDLE = 0x41 …) that .NET uses.
   Used by System.Security.Cryptography (Roslyn hashes sources for deterministic MVIDs, NuGet/MSBuild hash files, …). */
#define BC_STATUS_NOT_SUPPORTED 0xC00000BBu
#define BC_STATUS_INVALID_HANDLE 0xC0000008u
#define BC_STATUS_NOT_FOUND 0xC0000225u
#define BC_STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define BC_STATUS_INVALID_PARAMETER 0xC000000Du
enum { BC_MD5 = 1, BC_SHA1, BC_SHA256, BC_SHA384, BC_SHA512, BC_RNG, BC_ECDSA, BC_RSA, BC_AES };   /* ECDSA/RSA/AES providers open, but every key operation reports STATUS_NOT_SUPPORTED */
static const int bc_digest_len[] = { 0, 16, 20, 32, 48, 64, 0 };
static const int bc_block_len[] = { 0, 64, 64, 64, 128, 128, 0 };
static const char *const bc_alg_name[] = { "", "MD5", "SHA1", "SHA256", "SHA384", "SHA512", "RNG", "ECDSA_P256", "RSA", "AES" };

/* ---- MD5 ---- */
typedef struct { u32 h[4]; u64 len; u8 buf[64]; } Md5;
static const u32 md5_K[64] = { 0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
  0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
  0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
  0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const u8 md5_R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
static u32 rol32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
static u32 ror32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static u64 ror64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }
static void md5_init(Md5 *s) { s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->len = 0; }
static void md5_block(Md5 *s, const u8 *p) {
  u32 w[16]; for (int i = 0; i < 16; i++) w[i] = (u32)p[i*4] | (u32)p[i*4+1] << 8 | (u32)p[i*4+2] << 16 | (u32)p[i*4+3] << 24;
  u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  for (int i = 0; i < 64; i++) { u32 f; int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; } else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; } else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) & 15; } else { f = c ^ (b | ~d); g = (7*i) & 15; }
    u32 t = d; d = c; c = b; b = b + rol32(a + f + md5_K[i] + w[g], md5_R[i]); a = t; }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; }
static void md5_update(Md5 *s, const u8 *p, size_t n) { size_t r = (size_t)(s->len & 63); s->len += n;
  if (r) { size_t k = 64 - r; if (k > n) k = n; e_memcpy(s->buf + r, p, k); p += k; n -= k; if (r + k < 64) return; md5_block(s, s->buf); }
  while (n >= 64) { md5_block(s, p); p += 64; n -= 64; } if (n) e_memcpy(s->buf, p, n); }
static void md5_final(Md5 *s, u8 *out) { u64 bits = s->len * 8; u8 pad = 0x80; md5_update(s, &pad, 1); u8 z = 0; while ((s->len & 63) != 56) md5_update(s, &z, 1);
  u8 l[8]; for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (8*i)); md5_update(s, l, 8); for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i*4+j] = (u8)(s->h[i] >> (8*j)); }
/* ---- SHA-1 ---- */
typedef struct { u32 h[5]; u64 len; u8 buf[64]; } Sha1;
static void sha1_init(Sha1 *s) { s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0; s->len = 0; }
static void sha1_block(Sha1 *s, const u8 *p) { u32 w[80]; for (int i = 0; i < 16; i++) w[i] = (u32)p[i*4] << 24 | (u32)p[i*4+1] << 16 | (u32)p[i*4+2] << 8 | p[i*4+3];
  for (int i = 16; i < 80; i++) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  for (int i = 0; i < 80; i++) { u32 f, k; if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; } else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; } else { f = b ^ c ^ d; k = 0xca62c1d6; }
    u32 t = rol32(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol32(b, 30); b = a; a = t; }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; }
static void sha1_update(Sha1 *s, const u8 *p, size_t n) { size_t r = (size_t)(s->len & 63); s->len += n;
  if (r) { size_t k = 64 - r; if (k > n) k = n; e_memcpy(s->buf + r, p, k); p += k; n -= k; if (r + k < 64) return; sha1_block(s, s->buf); }
  while (n >= 64) { sha1_block(s, p); p += 64; n -= 64; } if (n) e_memcpy(s->buf, p, n); }
static void sha1_final(Sha1 *s, u8 *out) { u64 bits = s->len * 8; u8 pad = 0x80; sha1_update(s, &pad, 1); u8 z = 0; while ((s->len & 63) != 56) sha1_update(s, &z, 1);
  u8 l[8]; for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8*i)); sha1_update(s, l, 8); for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[i*4+j] = (u8)(s->h[i] >> (24 - 8*j)); }
/* ---- SHA-256 ---- */
typedef struct { u32 h[8]; u64 len; u8 buf[64]; } Sha256;
static const u32 sha256_K[64] = { 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static void sha256_init(Sha256 *s) { static const u32 iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 }; e_memcpy(s->h, iv, 32); s->len = 0; }
static void sha256_block(Sha256 *s, const u8 *p) { u32 w[64]; for (int i = 0; i < 16; i++) w[i] = (u32)p[i*4] << 24 | (u32)p[i*4+1] << 16 | (u32)p[i*4+2] << 8 | p[i*4+3];
  for (int i = 16; i < 64; i++) { u32 s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
  u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) { u32 S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + sha256_K[i] + w[i];
    u32 S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj; h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h; }
static void sha256_update(Sha256 *s, const u8 *p, size_t n) { size_t r = (size_t)(s->len & 63); s->len += n;
  if (r) { size_t k = 64 - r; if (k > n) k = n; e_memcpy(s->buf + r, p, k); p += k; n -= k; if (r + k < 64) return; sha256_block(s, s->buf); }
  while (n >= 64) { sha256_block(s, p); p += 64; n -= 64; } if (n) e_memcpy(s->buf, p, n); }
static void sha256_final(Sha256 *s, u8 *out) { u64 bits = s->len * 8; u8 pad = 0x80; sha256_update(s, &pad, 1); u8 z = 0; while ((s->len & 63) != 56) sha256_update(s, &z, 1);
  u8 l[8]; for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8*i)); sha256_update(s, l, 8); for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[i*4+j] = (u8)(s->h[i] >> (24 - 8*j)); }
/* ---- SHA-512 / SHA-384 ---- */
typedef struct { u64 h[8]; u64 len; u8 buf[128]; int is384; } Sha512;
static const u64 sha512_K[80] = { 0x428a2f98d728ae22ull,0x7137449123ef65cdull,0xb5c0fbcfec4d3b2full,0xe9b5dba58189dbbcull,0x3956c25bf348b538ull,0x59f111f1b605d019ull,0x923f82a4af194f9bull,0xab1c5ed5da6d8118ull,
  0xd807aa98a3030242ull,0x12835b0145706fbeull,0x243185be4ee4b28cull,0x550c7dc3d5ffb4e2ull,0x72be5d74f27b896full,0x80deb1fe3b1696b1ull,0x9bdc06a725c71235ull,0xc19bf174cf692694ull,
  0xe49b69c19ef14ad2ull,0xefbe4786384f25e3ull,0x0fc19dc68b8cd5b5ull,0x240ca1cc77ac9c65ull,0x2de92c6f592b0275ull,0x4a7484aa6ea6e483ull,0x5cb0a9dcbd41fbd4ull,0x76f988da831153b5ull,
  0x983e5152ee66dfabull,0xa831c66d2db43210ull,0xb00327c898fb213full,0xbf597fc7beef0ee4ull,0xc6e00bf33da88fc2ull,0xd5a79147930aa725ull,0x06ca6351e003826full,0x142929670a0e6e70ull,
  0x27b70a8546d22ffcull,0x2e1b21385c26c926ull,0x4d2c6dfc5ac42aedull,0x53380d139d95b3dfull,0x650a73548baf63deull,0x766a0abb3c77b2a8ull,0x81c2c92e47edaee6ull,0x92722c851482353bull,
  0xa2bfe8a14cf10364ull,0xa81a664bbc423001ull,0xc24b8b70d0f89791ull,0xc76c51a30654be30ull,0xd192e819d6ef5218ull,0xd69906245565a910ull,0xf40e35855771202aull,0x106aa07032bbd1b8ull,
  0x19a4c116b8d2d0c8ull,0x1e376c085141ab53ull,0x2748774cdf8eeb99ull,0x34b0bcb5e19b48a8ull,0x391c0cb3c5c95a63ull,0x4ed8aa4ae3418acbull,0x5b9cca4f7763e373ull,0x682e6ff3d6b2b8a3ull,
  0x748f82ee5defb2fcull,0x78a5636f43172f60ull,0x84c87814a1f0ab72ull,0x8cc702081a6439ecull,0x90befffa23631e28ull,0xa4506cebde82bde9ull,0xbef9a3f7b2c67915ull,0xc67178f2e372532bull,
  0xca273eceea26619cull,0xd186b8c721c0c207ull,0xeada7dd6cde0eb1eull,0xf57d4f7fee6ed178ull,0x06f067aa72176fbaull,0x0a637dc5a2c898a6ull,0x113f9804bef90daeull,0x1b710b35131c471bull,
  0x28db77f523047d84ull,0x32caab7b40c72493ull,0x3c9ebe0a15c9bebcull,0x431d67c49c100d4cull,0x4cc5d4becb3e42b6ull,0x597f299cfc657e2aull,0x5fcb6fab3ad6faecull,0x6c44198c4a475817ull };
static void sha512_init(Sha512 *s, int is384) {
  static const u64 iv512[8] = { 0x6a09e667f3bcc908ull,0xbb67ae8584caa73bull,0x3c6ef372fe94f82bull,0xa54ff53a5f1d36f1ull,0x510e527fade682d1ull,0x9b05688c2b3e6c1full,0x1f83d9abfb41bd6bull,0x5be0cd19137e2179ull };
  static const u64 iv384[8] = { 0xcbbb9d5dc1059ed8ull,0x629a292a367cd507ull,0x9159015a3070dd17ull,0x152fecd8f70e5939ull,0x67332667ffc00b31ull,0x8eb44a8768581511ull,0xdb0c2e0d64f98fa7ull,0x47b5481dbefa4fa4ull };
  e_memcpy(s->h, is384 ? iv384 : iv512, 64); s->len = 0; s->is384 = is384; }
static void sha512_block(Sha512 *s, const u8 *p) { u64 w[80]; for (int i = 0; i < 16; i++) { w[i] = 0; for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i*8+j]; }
  for (int i = 16; i < 80; i++) { u64 s0 = ror64(w[i-15], 1) ^ ror64(w[i-15], 8) ^ (w[i-15] >> 7), s1 = ror64(w[i-2], 19) ^ ror64(w[i-2], 61) ^ (w[i-2] >> 6); w[i] = w[i-16] + s0 + w[i-7] + s1; }
  u64 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 80; i++) { u64 S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + sha512_K[i] + w[i];
    u64 S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj; h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h; }
static void sha512_update(Sha512 *s, const u8 *p, size_t n) { size_t r = (size_t)(s->len & 127); s->len += n;
  if (r) { size_t k = 128 - r; if (k > n) k = n; e_memcpy(s->buf + r, p, k); p += k; n -= k; if (r + k < 128) return; sha512_block(s, s->buf); }
  while (n >= 128) { sha512_block(s, p); p += 128; n -= 128; } if (n) e_memcpy(s->buf, p, n); }
static void sha512_final(Sha512 *s, u8 *out) { u64 bits = s->len * 8; u8 pad = 0x80; sha512_update(s, &pad, 1); u8 z = 0; while ((s->len & 127) != 112) sha512_update(s, &z, 1);
  u8 l[16] = { 0 }; for (int i = 0; i < 8; i++) l[8 + i] = (u8)(bits >> (56 - 8*i)); sha512_update(s, l, 16);
  int words = s->is384 ? 6 : 8; for (int i = 0; i < words; i++) for (int j = 0; j < 8; j++) out[i*8+j] = (u8)(s->h[i] >> (56 - 8*j)); }

/* ---- generic hash object (+ HMAC) ---- */
typedef struct { int alg, hmac; union { Md5 md5; Sha1 sha1; Sha256 sha256; Sha512 sha512; } u; u8 key[128]; int keylen; } BcHash;
static void bc_hash_begin(BcHash *h) { switch (h->alg) { case BC_MD5: md5_init(&h->u.md5); break; case BC_SHA1: sha1_init(&h->u.sha1); break; case BC_SHA256: sha256_init(&h->u.sha256); break; case BC_SHA384: sha512_init(&h->u.sha512, 1); break; case BC_SHA512: sha512_init(&h->u.sha512, 0); break; } }
static void bc_hash_update(BcHash *h, const u8 *p, size_t n) { switch (h->alg) { case BC_MD5: md5_update(&h->u.md5, p, n); break; case BC_SHA1: sha1_update(&h->u.sha1, p, n); break; case BC_SHA256: sha256_update(&h->u.sha256, p, n); break; default: sha512_update(&h->u.sha512, p, n); } }
static void bc_hash_end(BcHash *h, u8 *out) { switch (h->alg) { case BC_MD5: md5_final(&h->u.md5, out); break; case BC_SHA1: sha1_final(&h->u.sha1, out); break; case BC_SHA256: sha256_final(&h->u.sha256, out); break; default: sha512_final(&h->u.sha512, out); } }
static void bc_init(BcHash *h, int alg, int hmac, const u8 *key, int keylen) {
  h->alg = alg; h->hmac = hmac; h->keylen = 0; int bl = bc_block_len[alg];
  if (hmac) { e_memset(h->key, 0, sizeof h->key);
    if (keylen > bl) { BcHash t; t.alg = alg; t.hmac = 0; bc_hash_begin(&t); bc_hash_update(&t, key, (size_t)keylen); bc_hash_end(&t, h->key); h->keylen = bc_digest_len[alg]; } else { e_memcpy(h->key, key, (size_t)keylen); h->keylen = keylen; } }
  bc_hash_begin(h);
  if (hmac) { u8 ipad[128]; for (int i = 0; i < bl; i++) ipad[i] = h->key[i] ^ 0x36; bc_hash_update(h, ipad, (size_t)bl); } }
static void bc_finish(BcHash *h, u8 *out) {
  if (!h->hmac) { bc_hash_end(h, out); return; }
  u8 inner[64]; bc_hash_end(h, inner); int bl = bc_block_len[h->alg], dl = bc_digest_len[h->alg];
  BcHash o; o.alg = h->alg; o.hmac = 0; bc_hash_begin(&o); u8 opad[128]; for (int i = 0; i < bl; i++) opad[i] = h->key[i] ^ 0x5c; bc_hash_update(&o, opad, (size_t)bl); bc_hash_update(&o, inner, (size_t)dl); bc_hash_end(&o, out); }

/* ---- handle table: algorithm providers and hash objects ---- */
#define BC_MAX 256
typedef struct { int used, kind, alg, hmac; BcHash h; u8 key[64]; } BcObj;   /* kind 1 algorithm, 2 hash, 3 public key (ECC X||Y) */
#include "ecdsa.inc"
static BcObj g_bc[BC_MAX];
#define BC_HANDLE_BASE 0x6bc00000ull
static int bc_alloc(int kind, int alg, int hmac) { for (int i = 1; i < BC_MAX; i++) if (!g_bc[i].used) { e_memset(&g_bc[i], 0, sizeof g_bc[i]); g_bc[i].used = 1; g_bc[i].kind = kind; g_bc[i].alg = alg; g_bc[i].hmac = hmac; return i; } return -1; }
static BcObj *bc_obj(u64 h, int kind) { u64 i = h - BC_HANDLE_BASE; if (i == 0 || i >= BC_MAX || !g_bc[i].used || g_bc[i].kind != kind) return 0; return &g_bc[i]; }
/* algorithm from a handle: real (table) or pseudo (0x21 MD5 … 0x61 SHA512, +0x70 for HMAC) */
static int bc_alg_of(u64 h, int *hmac) {
  BcObj *o = bc_obj(h, 1); if (o) { *hmac = o->hmac; return o->alg; }
  u32 p = (u32)h; *hmac = 0; if (p >= 0x91 && p <= 0xd1 && (p & 0xf) == 1) { *hmac = 1; p -= 0x70; }
  switch (p) { case 0x21: return BC_MD5; case 0x31: return BC_SHA1; case 0x41: return BC_SHA256; case 0x51: return BC_SHA384; case 0x61: return BC_SHA512; case 0x81: return BC_RNG; } return 0; }
static int bc_alg_by_name(const char *n) { for (int i = 1; i <= BC_AES; i++) if (!e_stricmp(n, bc_alg_name[i])) return i; return 0; }

/* BCryptOpenAlgorithmProvider(&hAlg, algId, implementation, flags) ; flags 8 = BCRYPT_ALG_HANDLE_HMAC_FLAG */
static void s_BCryptOpenAlgorithmProvider(Cpu *c) { char name[64]; wstr_to_utf8(c, ARG(1), name, sizeof name, -1); u32 flags = (u32)ARG(3);
  int alg = bc_alg_by_name(name); if (!alg) { e_log("[x64] BCryptOpenAlgorithmProvider(%s): not implemented (hashes + RNG only)\n", name); RET(BC_STATUS_NOT_FOUND); return; }
  int i = bc_alloc(1, alg, (flags & 8) ? 1 : 0); if (i < 0) { RET(0xC0000017u); return; } wrp(c, ARG(0), BC_HANDLE_BASE + (u64)i); RET(0); }
static void s_BCryptCloseAlgorithmProvider(Cpu *c) { BcObj *o = bc_obj(ARG(0), 1); if (o) o->used = 0; RET(0); }
/* BCryptGetProperty(h, propW, out, cbOut, &cbResult, flags) */
static void s_BCryptGetProperty(Cpu *c) { char prop[64]; wstr_to_utf8(c, ARG(1), prop, sizeof prop, -1); u64 out = ARG(2); u32 cb = (u32)ARG(3), *dummy = 0; (void)dummy; u64 resp = ARG(4);
  int hmac = 0, alg = bc_alg_of(ARG(0), &hmac); if (!alg) { BcObj *o = bc_obj(ARG(0), 2); if (!o) { RET(BC_STATUS_INVALID_HANDLE); return; } alg = o->alg; hmac = o->hmac; }
  u32 v = 0; const char *sval = 0;
  if (!e_stricmp(prop, "ObjectLength")) v = (u32)sizeof(BcHash); else if (!e_stricmp(prop, "HashDigestLength")) v = (u32)bc_digest_len[alg]; else if (!e_stricmp(prop, "HashBlockLength")) v = (u32)bc_block_len[alg];
  else if (!e_stricmp(prop, "AlgorithmName")) sval = bc_alg_name[alg];
  else { e_log("[x64] BCryptGetProperty(%s): not supported\n", prop); RET(BC_STATUS_NOT_SUPPORTED); return; }
  if (sval) { u32 need = (u32)(e_strlen(sval) + 1) * 2; if (resp) wr32(c, resp, need); if (!out) { RET(0); return; } if (cb < need) { RET(BC_STATUS_BUFFER_TOO_SMALL); return; } put_w(c, out, sval, (int)need / 2); RET(0); return; }
  if (resp) wr32(c, resp, 4); if (!out) { RET(0); return; } if (cb < 4) { RET(BC_STATUS_BUFFER_TOO_SMALL); return; } wr32(c, out, v); RET(0); }
static void s_BCryptSetProperty(Cpu *c) { RET(0); }
/* BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, pbSecret, cbSecret, flags) */
static void s_BCryptCreateHash(Cpu *c) { int hmac = 0, alg = bc_alg_of(ARG(0), &hmac); if (!alg || alg >= BC_RNG) { RET(BC_STATUS_INVALID_HANDLE); return; }
  u64 secret = ARG(4); u32 cbs = (u32)ARG(5); if (secret && cbs) hmac = 1;
  int i = bc_alloc(2, alg, hmac); if (i < 0) { RET(0xC0000017u); return; }
  u8 key[4096]; u32 kl = cbs > sizeof key ? (u32)sizeof key : cbs; if (hmac && secret) { const u8 *p = mem_ptr(c->mem, secret, kl, 0); if (p) e_memcpy(key, p, kl); else kl = 0; } else kl = 0;
  bc_init(&g_bc[i].h, alg, hmac, key, (int)kl); wrp(c, ARG(1), BC_HANDLE_BASE + (u64)i); RET(0); }
static void s_BCryptHashData(Cpu *c) { BcObj *o = bc_obj(ARG(0), 2); if (!o) { RET(BC_STATUS_INVALID_HANDLE); return; } u32 n = (u32)ARG(2); const u8 *p = n ? mem_ptr(c->mem, ARG(1), n, 0) : (const u8 *)""; if (!p) { RET(BC_STATUS_INVALID_PARAMETER); return; } bc_hash_update(&o->h, p, n); RET(0); }
static void s_BCryptFinishHash(Cpu *c) { BcObj *o = bc_obj(ARG(0), 2); if (!o) { RET(BC_STATUS_INVALID_HANDLE); return; } u32 cb = (u32)ARG(2); int dl = bc_digest_len[o->alg]; if (cb != (u32)dl) { RET(BC_STATUS_INVALID_PARAMETER); return; }
  u8 out[64]; bc_finish(&o->h, out); u8 *d = mem_ptr(c->mem, ARG(1), (u64)dl, 1); if (!d) { RET(BC_STATUS_INVALID_PARAMETER); return; } e_memcpy(d, out, (size_t)dl);
  bc_init(&o->h, o->alg, o->hmac, o->h.key, o->h.keylen);   /* BCRYPT_HASH_REUSABLE_FLAG semantics: ready for the next message */
  RET(0); }
static void s_BCryptDestroyHash(Cpu *c) { BcObj *o = bc_obj(ARG(0), 2); if (!o) { RET(BC_STATUS_INVALID_HANDLE); return; } o->used = 0; RET(0); }
/* BCryptDuplicateHash(hHash, &hNew, pbHashObject, cbHashObject, flags) */
static void s_BCryptDuplicateHash(Cpu *c) { BcObj *o = bc_obj(ARG(0), 2); if (!o) { RET(BC_STATUS_INVALID_HANDLE); return; } int i = bc_alloc(2, o->alg, o->hmac); if (i < 0) { RET(0xC0000017u); return; } g_bc[i].h = o->h; wrp(c, ARG(1), BC_HANDLE_BASE + (u64)i); RET(0); }
/* BCryptHash(hAlg, pbSecret, cbSecret, pbInput, cbInput, pbOutput, cbOutput): one shot */
static void s_BCryptHash(Cpu *c) { int hmac = 0, alg = bc_alg_of(ARG(0), &hmac); if (!alg || alg >= BC_RNG) { RET(BC_STATUS_INVALID_HANDLE); return; }
  u32 cbs = (u32)ARG(2), cbi = (u32)ARG(4), cbo = (u32)ARG(6); if (cbs) hmac = 1; if (cbo != (u32)bc_digest_len[alg]) { RET(BC_STATUS_INVALID_PARAMETER); return; }
  u8 key[4096]; u32 kl = cbs > sizeof key ? (u32)sizeof key : cbs; if (kl) { const u8 *p = mem_ptr(c->mem, ARG(1), kl, 0); if (!p) { RET(BC_STATUS_INVALID_PARAMETER); return; } e_memcpy(key, p, kl); }
  const u8 *in = cbi ? mem_ptr(c->mem, ARG(3), cbi, 0) : (const u8 *)""; u8 *out = mem_ptr(c->mem, ARG(5), cbo, 1); if (!in || !out) { RET(BC_STATUS_INVALID_PARAMETER); return; }
  BcHash h; bc_init(&h, alg, hmac, key, (int)kl); bc_hash_update(&h, in, cbi); u8 d[64]; bc_finish(&h, d); e_memcpy(out, d, cbo); RET(0); }
static void s_BCryptGetFipsAlgorithmMode(Cpu *c) { if (ARG(0)) wr8(c, ARG(0), 0); RET(0); }
/* Asymmetric / symmetric key operations are not implemented: every call reports STATUS_NOT_SUPPORTED so programs that probe for them
   (Inno Setup's ISSig verifier, .NET's RSA) take their "unavailable" path instead of calling a missing import. */
static void s_BCryptDestroyKey(Cpu *c) { BcObj *o = bc_obj(ARG(0), 3); if (o) o->used = 0; RET(0); }
static void s_BCryptGenerateKeyPair(Cpu *c) { if (ARG(1)) wrp(c, ARG(1), 0); RET(0xC00000BB); }
static void s_BCryptFinalizeKeyPair(Cpu *c) { RET(0xC00000BB); }
/* Public keys are accepted (ECCPUBLICBLOB / RSAPUBLICBLOB) so programs can initialise their verifiers; the key handle supports nothing but
   BCryptDestroyKey — a later BCryptVerifySignature reports STATUS_INVALID_SIGNATURE (no ECDSA/RSA arithmetic here). */
static void s_BCryptImportKeyPair(Cpu *c) { char type[32]; wstr_to_utf8(c, ARG(2), type, sizeof type, -1); int hm = 0; if (type[0] && (type[0] == 'E' || type[0] == 'R') && type[e_strlen(type) - 1] == 'B') { int alg = bc_alg_of(ARG(0), &hm); if (!alg) { RET(BC_STATUS_INVALID_HANDLE); return; } int i = bc_alloc(3, alg, 0); if (i < 0) { RET(0xC0000017u); return; } u64 blob = ARG(4); u32 blen = (u32)ARG(5); if (alg == BC_ECDSA) { if (blen < 72 || rd32(c, blob) != 0x31534345u /* BCRYPT_ECDSA_PUBLIC_P256_MAGIC */ || rd32(c, blob + 4) != 32) { g_bc[i].used = 0; RET(0xC000000Du); return; } for (int k = 0; k < 64; k++) g_bc[i].key[k] = rd8(c, blob + 8 + k); } if (ARG(3)) wrp(c, ARG(3), BC_HANDLE_BASE + (u64)i); RET(0); return; } if (ARG(3)) wrp(c, ARG(3), 0); RET(0xC00000BB); }
static void s_BCryptExportKey(Cpu *c) { /* (hKey, hExportKey, blobType, out, cbOut, &cbResult, flags): ECCPUBLICBLOB of an imported key */ BcObj *o = bc_obj(ARG(0), 3); if (!o) { RET(BC_STATUS_INVALID_HANDLE); return; } if (o->alg != BC_ECDSA) { RET(0xC00000BB); return; }
  if (ARG(5)) wr32(c, ARG(5), 72); if (!ARG(3)) { RET(0); return; } if ((u32)ARG(4) < 72) { RET(0xC0000023u); return; } wr32(c, ARG(3), 0x31534345u); wr32(c, ARG(3) + 4, 32); for (int k = 0; k < 64; k++) wr8(c, ARG(3) + 8 + k, o->key[k]); RET(0); }
static void s_BCryptImportKey(Cpu *c) { if (ARG(3)) wrp(c, ARG(3), 0); RET(0xC00000BB); }
static void s_BCryptGenerateSymmetricKey(Cpu *c) { if (ARG(1)) wrp(c, ARG(1), 0); RET(0xC00000BB); }
static void s_BCryptEncrypt(Cpu *c) { RET(0xC00000BB); }
static void s_BCryptDecrypt(Cpu *c) { RET(0xC00000BB); }
static void s_BCryptSignHash(Cpu *c) { RET(0xC00000BB); }
static void s_BCryptVerifySignature(Cpu *c) { /* (hKey, padding, hash, cbHash, sig, cbSig, flags) — real ECDSA P-256 check */ BcObj *o = bc_obj(ARG(0), 3); if (!o) { RET(BC_STATUS_INVALID_HANDLE); return; } if (o->alg != BC_ECDSA) { RET(0xC00000BB); return; }
  u32 hl = (u32)ARG(3), sl = (u32)ARG(5); if (sl != 64 || hl == 0 || hl > 64) { RET(0xC000A000); return; } u8 h[64], sig[64]; for (u32 k = 0; k < hl; k++) h[k] = rd8(c, ARG(2) + k); for (int k = 0; k < 64; k++) sig[k] = rd8(c, ARG(4) + k);
  RET(ecdsa_p256_verify(o->key, o->key + 32, h, hl, sig) ? 0 : 0xC000A000); }   /* STATUS_INVALID_SIGNATURE */
static void s_BCryptDeriveKeyPBKDF2(Cpu *c) { RET(0xC00000BB); }
static void s_BCryptDuplicateKey(Cpu *c) { RET(0xC00000BB); }
#define BCRYPT_SHIMS \
  S(BCryptDestroyKey), S(BCryptGenerateKeyPair), S(BCryptFinalizeKeyPair), S(BCryptImportKeyPair), S(BCryptExportKey), S(BCryptImportKey), S(BCryptGenerateSymmetricKey), S(BCryptEncrypt), S(BCryptDecrypt), S(BCryptSignHash), S(BCryptVerifySignature), S(BCryptDeriveKeyPBKDF2), S(BCryptDuplicateKey), \
  S(BCryptOpenAlgorithmProvider), S(BCryptCloseAlgorithmProvider), S(BCryptGetProperty), S(BCryptSetProperty), \
  S(BCryptCreateHash), S(BCryptHashData), S(BCryptFinishHash), S(BCryptDestroyHash), S(BCryptDuplicateHash), \
  S(BCryptHash), S(BCryptGetFipsAlgorithmMode),
