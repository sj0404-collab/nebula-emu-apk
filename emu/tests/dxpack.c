/* dxpack.exe — exercises the DirectX End-User Runtime DLLs ("DirectX pack") as real code inside the emulator:
 *   d3dx9_43.dll   D3DX math (D3DXMatrixPerspectiveFovLH / Multiply / Inverse, D3DXVec3Normalize) — the helpers most D3D9-era
 *                  games call before they ever touch a device,
 *   d3dx11_43.dll  D3DX11CompileFromMemory (loads D3DCompiler_43.dll underneath and compiles HLSL),
 *   X3DAudio1_7.dll X3DAudioInitialize + X3DAudioCalculate (3-D panning math),
 *   xinput1_3.dll  XInputGetState → ERROR_DEVICE_NOT_CONNECTED (the emulator's own honest answer: no controller).
 * Each DLL is optional: a missing one is reported and the exit code counts the failures (0 = everything worked). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct { float m[4][4]; } MAT; typedef struct { float x, y, z; } VEC3;
typedef MAT *(WINAPI *PF_PerspLH)(MAT *, float, float, float, float);
typedef MAT *(WINAPI *PF_Mul)(MAT *, const MAT *, const MAT *);
typedef MAT *(WINAPI *PF_Inv)(MAT *, float *, const MAT *);
typedef VEC3 *(WINAPI *PF_Norm)(VEC3 *, const VEC3 *);
typedef HRESULT (WINAPI *PF_X11Compile)(LPCSTR src, SIZE_T len, LPCSTR name, const void *defs, void *inc, LPCSTR entry, LPCSTR profile, UINT f1, UINT f2, void *pump, void **shader, void **errors, HRESULT *hr);
typedef HRESULT (WINAPI *PF_X3DInit)(UINT mask, float speed, BYTE *inst);
typedef void (WINAPI *PF_X3DCalc)(const BYTE *inst, const void *listener, const void *emitter, UINT flags, void *out);
typedef DWORD (WINAPI *PF_XIGetState)(DWORD, void *);
static int fails = 0;
static HMODULE need(const char *n) { HMODULE h = LoadLibraryA(n); if (!h) { printf("%s: not available (LoadLibrary error %lu) — install the DirectX pack\n", n, GetLastError()); fails++; } return h; }
int main(void) {
  HMODULE x9 = need("d3dx9_43.dll");
  if (x9) {
    PF_PerspLH persp = (PF_PerspLH)GetProcAddress(x9, "D3DXMatrixPerspectiveFovLH"); PF_Mul mul = (PF_Mul)GetProcAddress(x9, "D3DXMatrixMultiply");
    PF_Inv inv = (PF_Inv)GetProcAddress(x9, "D3DXMatrixInverse"); PF_Norm norm = (PF_Norm)GetProcAddress(x9, "D3DXVec3Normalize");
    if (!persp || !mul || !inv || !norm) { printf("d3dx9_43: exports missing\n"); fails++; }
    else {
      MAT p, pi, id; persp(&p, 1.0471976f, 16.0f / 9.0f, 0.1f, 100.0f); inv(&pi, 0, &p); mul(&id, &p, &pi);
      float off = 0; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) off += fabsf(id.m[i][j] - (i == j ? 1.0f : 0.0f));
      VEC3 v = { 3, 4, 12 }, n; norm(&n, &v);
      printf("d3dx9_43: perspective m11=%.4f m22=%.4f m33=%.4f, P*inverse(P) deviates from identity by %.5f, normalize(3,4,12)=(%.4f,%.4f,%.4f)\n", p.m[0][0], p.m[1][1], p.m[2][2], off, n.x, n.y, n.z);
      if (off > 1e-3f || fabsf(n.z - 12.0f / 13.0f) > 1e-4f) { printf("d3dx9_43: WRONG results\n"); fails++; }
    }
  }
  HMODULE x11 = need("d3dx11_43.dll");
  if (x11) {
    PF_X11Compile comp = (PF_X11Compile)GetProcAddress(x11, "D3DX11CompileFromMemory");
    if (!comp) { printf("d3dx11_43: D3DX11CompileFromMemory missing\n"); fails++; }
    else {
      const char *src = "float4 main(float4 p : POSITION) : SV_POSITION { return p * 2.0; }";
      void *blob = 0, *err = 0; HRESULT hr2 = 0; HRESULT hr = comp(src, strlen(src), "t.hlsl", 0, 0, "main", "vs_4_0", 0, 0, 0, &blob, &err, &hr2);
      if (FAILED(hr) || !blob) { printf("d3dx11_43: D3DX11CompileFromMemory failed 0x%08lx\n", (long)hr); fails++; }
      else { IUnknown *u = blob; typedef void *(STDMETHODCALLTYPE *GP)(void *); typedef SIZE_T (STDMETHODCALLTYPE *GS)(void *); void **vt = *(void ***)blob;
        const unsigned char *d = ((GP)vt[3])(blob); SIZE_T n = ((GS)vt[4])(blob); printf("d3dx11_43: HLSL compiled through D3DCompiler_43 → %u bytes %.4s\n", (unsigned)n, d); if (memcmp(d, "DXBC", 4)) fails++; u->lpVtbl->Release(u); }
    }
  }
  HMODULE xa = need("X3DAudio1_7.dll");
  if (xa) {
    PF_X3DInit init = (PF_X3DInit)GetProcAddress(xa, "X3DAudioInitialize"); PF_X3DCalc calc = (PF_X3DCalc)GetProcAddress(xa, "X3DAudioCalculate");
    if (!init || !calc) { printf("X3DAudio1_7: exports missing\n"); fails++; }
    else {
      static BYTE inst[20]; init(3 /* SPEAKER_FRONT_LEFT|RIGHT */, 343.5f, inst);
      /* X3DAUDIO_LISTENER { OrientFront, OrientTop, Position, Velocity, pCone } ; X3DAUDIO_EMITTER with one channel at +x */
      struct { float of[3], ot[3], pos[3], vel[3]; void *cone; } listener; memset(&listener, 0, sizeof listener); listener.of[2] = 1; listener.ot[1] = 1;
      struct { void *cone; float of[3], ot[3], pos[3], vel[3]; float inner, iv; UINT ch; float radius; float *az; void *vol, *lfe, *lpf, *lpfr, *reverb; float curve_scale, doppler; } em;
      memset(&em, 0, sizeof em); em.of[2] = 1; em.ot[1] = 1; em.pos[0] = 1.0f; em.ch = 1; em.curve_scale = 1.0f; em.doppler = 1.0f;
      float matrix[2] = { 0, 0 }; struct { float *m; float *delay; UINT src, dst; float lpfd, lpfr, reverb, dop, angle, dist; } out; memset(&out, 0, sizeof out); out.m = matrix; out.src = 1; out.dst = 2;
      calc(inst, &listener, &em, 0x1 /* X3DAUDIO_CALCULATE_MATRIX */, &out);
      printf("X3DAudio1_7: emitter at +x → left %.3f right %.3f\n", matrix[0], matrix[1]);
      if (!(matrix[1] > matrix[0])) { printf("X3DAudio1_7: panning WRONG\n"); fails++; }
    }
  }
  HMODULE xi = LoadLibraryA("xinput1_3.dll");
  if (!xi) { printf("xinput1_3: LoadLibrary failed\n"); fails++; }
  else { PF_XIGetState gs = (PF_XIGetState)GetProcAddress(xi, "XInputGetState"); BYTE st[16]; DWORD r = gs ? gs(0, st) : 99999; printf("xinput1_3: XInputGetState(0) = %lu (%s)\n", r, r == 1167 ? "ERROR_DEVICE_NOT_CONNECTED — honest: no controller" : "unexpected"); if (r != 1167) fails++; }
  printf("dxpack: %d failure(s)\n", fails); return fails;
}
