/* dxbc.h — DXBC (SM4/5) → GLSL ES 3.00 translator used by the D3D11 layer */
#ifndef NEBULA_DXBC_H
#define NEBULA_DXBC_H
#define DXBC_MAX_SIG 32
typedef struct { char name[32]; int index, sysval, ctype, reg; unsigned char mask, rwmask; } DxbcSigElem;
typedef struct { int n; DxbcSigElem e[DXBC_MAX_SIG]; } DxbcSig;
typedef struct {
  int type;            /* 0 pixel, 1 vertex */
  int sm_major, ntemps;
  DxbcSig in, out;     /* ISGN / OSGN */
  int cb_size[16];     /* constant buffer sizes in vec4s (0 = unused) */
  int tex_dim[16];     /* 0 unused, 1 buffer, 2 tex1d, 3 tex2d, 5 tex3d, 6 cube */
  int tex_sampler[16]; /* sampler slot + 1 used with texture t# (0 = none seen) */
} DxbcInfo;
/* returns GLSL length, or -1 with a one-line reason in out (unsupported opcode / operand / program type) */
int dxbc_to_glsl(const unsigned char *bytes, size_t len, char *out, size_t cap, DxbcInfo *info);
#endif
