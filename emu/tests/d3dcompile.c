/* d3dcompile.exe — HLSL compiled AT RUN TIME by the REAL Microsoft shader compiler (D3DCompiler_43.dll from the DirectX
 * End-User Runtime June 2010, or d3dcompiler_47.dll if present), then drawn through Direct3D 11: the DXBC produced by the
 * real compiler goes through the emulator's DXBC → GLSL translator and WebGL2.  Proves that the DirectX pack DLLs are not
 * just "loadable" but do their job: the compiler is ~2.5 MB of pure x86-64 code (lexer, parser, optimiser, SM4 back end).
 * Without the compiler DLL on the virtual disk the program says so and exits 3. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID src, SIZE_T len, LPCSTR name, const void *defines, void *include, LPCSTR entry, LPCSTR target, UINT f1, UINT f2, ID3DBlob **code, ID3DBlob **errors);
typedef HRESULT (WINAPI *PFN_D3DDisassemble)(LPCVOID data, SIZE_T len, UINT flags, LPCSTR comments, ID3DBlob **out);
static const char *HLSL =
  "cbuffer cb : register(b0) { float4x4 model; float4x4 projectionAndView; };\n"
  "struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
  "struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
  "PSIn vs_main(VSIn i) { PSIn o; o.pos = mul(projectionAndView, mul(model, float4(i.pos, 1.0))); o.uv = i.uv; o.col = i.col; return o; }\n"
  "float4 ps_main(PSIn i) : SV_TARGET { return float4(i.col.rgb * (0.75 + 0.25 * i.uv.y), i.col.a); }\n";
typedef struct { float pos[3]; float uv[2]; float col[4]; } Vtx;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_DESTROY) { PostQuitMessage(0); return 0; } return DefWindowProcA(h, m, w, l); }
#define CHECK(hr, what) do { if (FAILED(hr)) { printf("%s failed: 0x%08lx\n", what, (long)(hr)); return 1; } } while (0)
static ID3DBlob *compile(PFN_D3DCompile fn, const char *entry, const char *target) {
  ID3DBlob *code = 0, *err = 0;
  HRESULT hr = fn(HLSL, strlen(HLSL), "nebula.hlsl", 0, 0, entry, target, 0, 0, &code, &err);
  if (err) { printf("compiler says: %.*s\n", (int)ID3D10Blob_GetBufferSize(err), (char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
  if (FAILED(hr)) { printf("D3DCompile(%s, %s) failed: 0x%08lx\n", entry, target, (long)hr); return 0; }
  const unsigned char *p = ID3D10Blob_GetBufferPointer(code); SIZE_T n = ID3D10Blob_GetBufferSize(code);
  printf("%s %s: %u bytes, magic %c%c%c%c, %s\n", entry, target, (unsigned)n, p[0], p[1], p[2], p[3], memcmp(p, "DXBC", 4) == 0 ? "valid DXBC container" : "NOT DXBC");
  return code;
}
int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 3;
  const char *names[] = { "d3dcompiler_47.dll", "D3DCompiler_43.dll", "D3DCompiler_42.dll", 0 }; HMODULE dll = 0; const char *used = 0;
  for (int i = 0; names[i] && !dll; i++) { dll = LoadLibraryA(names[i]); if (dll) used = names[i]; }
  if (!dll) { printf("no shader compiler DLL (d3dcompiler_47 / D3DCompiler_43): install the DirectX pack (C:\\Windows\\System32) and run again\n"); return 3; }
  char path[MAX_PATH]; GetModuleFileNameA(dll, path, sizeof path);
  PFN_D3DCompile D3DCompile = (PFN_D3DCompile)GetProcAddress(dll, "D3DCompile"); PFN_D3DDisassemble D3DDisassemble = (PFN_D3DDisassemble)GetProcAddress(dll, "D3DDisassemble");
  if (!D3DCompile) { printf("%s has no D3DCompile export\n", used); return 4; }
  printf("shader compiler: %s (%s)\n", used, path);
  ID3DBlob *vs = compile(D3DCompile, "vs_main", "vs_4_0"), *ps = compile(D3DCompile, "ps_main", "ps_4_0");
  if (!vs || !ps) return 5;
  if (D3DDisassemble) { ID3DBlob *dis = 0; if (SUCCEEDED(D3DDisassemble(ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), 0, 0, &dis)) && dis) {
      const char *t = ID3D10Blob_GetBufferPointer(dis); SIZE_T n = ID3D10Blob_GetBufferSize(dis); int lines = 0; for (SIZE_T i = 0; i < n; i++) if (t[i] == '\n') lines++;
      const char *op = strstr(t, "ps_4_0"); printf("disassembly of ps_main: %d lines, target line '%.6s', contains %s\n", lines, op ? op : "?", strstr(t, "mad ") || strstr(t, "mul ") ? "mul/mad arithmetic" : "no arithmetic?");
      ID3D10Blob_Release(dis); } }
  /* ---- draw with the freshly compiled shaders (same scene as d3d11tri) ---- */
  WNDCLASSA wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "d3dcompile"; RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "d3dcompile", "D3D11 + runtime-compiled HLSL", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, 0, 0, wc.hInstance, 0);
  DXGI_SWAP_CHAIN_DESC sd = {0}; sd.BufferCount = 1; sd.BufferDesc.Width = 640; sd.BufferDesc.Height = 480; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
  ID3D11Device *dev = 0; ID3D11DeviceContext *ctx = 0; IDXGISwapChain *sc = 0; D3D_FEATURE_LEVEL fl = 0;
  CHECK(D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx), "D3D11CreateDeviceAndSwapChain");
  ID3D11Texture2D *bb = 0; CHECK(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb), "GetBuffer");
  ID3D11RenderTargetView *rtv = 0; CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, 0, &rtv), "CreateRenderTargetView"); ID3D11Texture2D_Release(bb);
  ID3D11VertexShader *vsh = 0; ID3D11PixelShader *psh = 0; ID3D11InputLayout *layout = 0;
  CHECK(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), 0, &vsh), "CreateVertexShader");
  CHECK(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), 0, &psh), "CreatePixelShader");
  D3D11_INPUT_ELEMENT_DESC el[3] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
  CHECK(ID3D11Device_CreateInputLayout(dev, el, 3, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &layout), "CreateInputLayout");
  Vtx verts[3] = { { { -0.8f, -0.8f, 0 }, { 0, 1 }, { 1, 0.5f, 0, 1 } }, { { 0.0f, 0.8f, 0 }, { 0.5f, 0 }, { 0, 1, 1, 1 } }, { { 0.8f, -0.8f, 0 }, { 1, 1 }, { 1, 0, 1, 1 } } };
  float cb[32] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
  ID3D11Buffer *vb = 0, *cbuf = 0; D3D11_BUFFER_DESC bd = {0}; D3D11_SUBRESOURCE_DATA init = {0};
  bd.ByteWidth = sizeof verts; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; init.pSysMem = verts; CHECK(ID3D11Device_CreateBuffer(dev, &bd, &init, &vb), "CreateBuffer(vb)");
  bd.ByteWidth = sizeof cb; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; init.pSysMem = cb; CHECK(ID3D11Device_CreateBuffer(dev, &bd, &init, &cbuf), "CreateBuffer(cb)");
  ID3D11RasterizerState *rs = 0; D3D11_RASTERIZER_DESC rd = {0}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; CHECK(ID3D11Device_CreateRasterizerState(dev, &rd, &rs), "CreateRasterizerState");
  printf("device ok, feature level 0x%x, runtime-compiled shaders bound\n", (unsigned)fl);
  D3D11_VIEWPORT vp = { 0, 0, 640, 480, 0, 1 }; UINT stride = sizeof(Vtx), offset = 0;
  for (int f = 0; f < frames; f++) {
    const float clear[4] = { 0.05f, 0.15f, 0.1f, 1 }; ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, 0); ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout); ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vsh, 0, 0); ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf); ID3D11DeviceContext_PSSetShader(ctx, psh, 0, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    CHECK(IDXGISwapChain_Present(sc, 1, 0), "Present");
    MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    printf("frame %d: triangle drawn with shaders compiled by %s\n", f, used);
  }
  printf("d3dcompile ok\n"); return 0;
}
