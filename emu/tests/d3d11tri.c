/* d3d11tri.exe — Direct3D 11 "hello triangle": vertex/index buffers, input layout, constant buffer, VS + PS (precompiled DXBC),
 * rasterizer/blend state, clear + Draw + Present.  In the browser build the triangle is rendered by WebGL2 through the
 * DXBC → GLSL translator; the native runner has no GPU and stops honestly at the first Draw. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include "d3d11_shaders.h"
typedef struct { float pos[3]; float uv[2]; float col[4]; } Vtx;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_DESTROY) { PostQuitMessage(0); return 0; } return DefWindowProcA(h, m, w, l); }
#define CHECK(hr, what) do { if (FAILED(hr)) { printf("%s failed: 0x%08lx\n", what, (long)(hr)); return 1; } } while (0)
int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 3;
  WNDCLASSA wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "d3d11tri"; RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "d3d11tri", "D3D11 triangle", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, 0, 0, wc.hInstance, 0);
  DXGI_SWAP_CHAIN_DESC sd = {0}; sd.BufferCount = 1; sd.BufferDesc.Width = 640; sd.BufferDesc.Height = 480; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
  ID3D11Device *dev = 0; ID3D11DeviceContext *ctx = 0; IDXGISwapChain *sc = 0; D3D_FEATURE_LEVEL fl = 0;
  CHECK(D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx), "D3D11CreateDeviceAndSwapChain");
  ID3D11Texture2D *bb = 0; CHECK(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb), "GetBuffer");
  ID3D11RenderTargetView *rtv = 0; CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, 0, &rtv), "CreateRenderTargetView"); ID3D11Texture2D_Release(bb);
  /* shaders + input layout */
  ID3D11VertexShader *vs = 0; ID3D11PixelShader *ps = 0; ID3D11InputLayout *layout = 0;
  CHECK(ID3D11Device_CreateVertexShader(dev, g_vs_sdl, sizeof g_vs_sdl, 0, &vs), "CreateVertexShader");
  CHECK(ID3D11Device_CreatePixelShader(dev, g_ps_colors, sizeof g_ps_colors, 0, &ps), "CreatePixelShader");
  D3D11_INPUT_ELEMENT_DESC el[3] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
  CHECK(ID3D11Device_CreateInputLayout(dev, el, 3, g_vs_sdl, sizeof g_vs_sdl, &layout), "CreateInputLayout");
  /* geometry: one big triangle (red → green → blue corners) and a small indexed quad in the corner */
  Vtx verts[7] = {
    { { -0.8f, -0.8f, 0 }, { 0, 1 }, { 1, 0, 0, 1 } }, { { 0.0f, 0.8f, 0 }, { 0.5f, 0 }, { 0, 1, 0, 1 } }, { { 0.8f, -0.8f, 0 }, { 1, 1 }, { 0, 0, 1, 1 } },
    { { 0.6f, 0.6f, 0 }, { 0, 0 }, { 1, 1, 0, 1 } }, { { 0.95f, 0.6f, 0 }, { 1, 0 }, { 1, 1, 0, 1 } }, { { 0.95f, 0.95f, 0 }, { 1, 1 }, { 1, 1, 0, 1 } }, { { 0.6f, 0.95f, 0 }, { 0, 1 }, { 1, 1, 0, 1 } } };
  unsigned short idx[6] = { 3, 5, 4, 3, 6, 5 };
  float cb[32] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };   /* model, projectionAndView = identity */
  ID3D11Buffer *vb = 0, *ib = 0, *cbuf = 0; D3D11_BUFFER_DESC bd = {0}; D3D11_SUBRESOURCE_DATA init = {0};
  bd.ByteWidth = sizeof verts; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; init.pSysMem = verts; CHECK(ID3D11Device_CreateBuffer(dev, &bd, &init, &vb), "CreateBuffer(vb)");
  bd.ByteWidth = sizeof idx; bd.BindFlags = D3D11_BIND_INDEX_BUFFER; init.pSysMem = idx; CHECK(ID3D11Device_CreateBuffer(dev, &bd, &init, &ib), "CreateBuffer(ib)");
  bd.ByteWidth = sizeof cb; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; init.pSysMem = cb; CHECK(ID3D11Device_CreateBuffer(dev, &bd, &init, &cbuf), "CreateBuffer(cb)");
  ID3D11RasterizerState *rs = 0; D3D11_RASTERIZER_DESC rd = {0}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; CHECK(ID3D11Device_CreateRasterizerState(dev, &rd, &rs), "CreateRasterizerState");
  ID3D11BlendState *bs = 0; D3D11_BLEND_DESC bld = {0}; bld.RenderTarget[0].BlendEnable = FALSE; bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL; CHECK(ID3D11Device_CreateBlendState(dev, &bld, &bs), "CreateBlendState");
  printf("device ok, feature level 0x%x, shaders + buffers + layout created\n", (unsigned)fl);
  D3D11_VIEWPORT vp = { 0, 0, 640, 480, 0, 1 }; UINT stride = sizeof(Vtx), offset = 0;
  for (int f = 0; f < frames; f++) {
    /* animate the model matrix: slide the triangle a bit each frame through Map/Unmap */
    D3D11_MAPPED_SUBRESOURCE m; CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map(cb)"); cb[12] = 0.05f * f; memcpy(m.pData, cb, sizeof cb); ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbuf, 0);
    const float clear[4] = { 0.1f, 0.1f, 0.3f, 1 }; ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, 0); ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); ID3D11DeviceContext_RSSetState(ctx, rs); ID3D11DeviceContext_OMSetBlendState(ctx, bs, 0, 0xffffffff);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout); ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset); ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, 0, 0); ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf); ID3D11DeviceContext_PSSetShader(ctx, ps, 0, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
    CHECK(IDXGISwapChain_Present(sc, 1, 0), "Present");
    MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    printf("frame %d: triangle + quad drawn and presented\n", f);
  }
  printf("d3d11tri ok\n"); return 0;
}
