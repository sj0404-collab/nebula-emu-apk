/* d3d11clear.exe — minimal Direct3D 11 program: window + device + swap chain, clears the back buffer in three colours and presents. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_DESTROY) { PostQuitMessage(0); return 0; } return DefWindowProcA(h, m, w, l); }
int main(void) {
  WNDCLASSA wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "d3d11clear"; RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "d3d11clear", "D3D11 clear", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, 0, 0, wc.hInstance, 0);
  if (!hwnd) { printf("CreateWindow failed\n"); return 1; }
  DXGI_SWAP_CHAIN_DESC sd = {0}; sd.BufferCount = 1; sd.BufferDesc.Width = 640; sd.BufferDesc.Height = 480; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
  ID3D11Device *dev = 0; ID3D11DeviceContext *ctx = 0; IDXGISwapChain *sc = 0; D3D_FEATURE_LEVEL fl = 0;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
  if (FAILED(hr)) { printf("D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", hr); return 1; }
  printf("device ok, feature level 0x%x\n", (unsigned)fl);
  ID3D11Texture2D *bb = 0; hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
  if (FAILED(hr)) { printf("GetBuffer failed: 0x%08lx\n", hr); return 1; }
  D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(bb, &td); printf("backbuffer %ux%u format %u\n", (unsigned)td.Width, (unsigned)td.Height, (unsigned)td.Format);
  ID3D11RenderTargetView *rtv = 0; hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, 0, &rtv);
  if (FAILED(hr)) { printf("CreateRenderTargetView failed: 0x%08lx\n", hr); return 1; }
  ID3D11Texture2D_Release(bb);
  D3D11_VIEWPORT vp = { 0, 0, 640, 480, 0, 1 }; ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
  ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, 0);
  const float colours[3][4] = { { 0.2f, 0.4f, 0.9f, 1 }, { 0.9f, 0.3f, 0.1f, 1 }, { 0.1f, 0.8f, 0.3f, 1 } };
  for (int frame = 0; frame < 3; frame++) {
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, colours[frame]);
    hr = IDXGISwapChain_Present(sc, 1, 0); if (FAILED(hr)) { printf("Present failed: 0x%08lx\n", hr); return 1; }
    MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    printf("frame %d presented (%.1f %.1f %.1f)\n", frame, colours[frame][0], colours[frame][1], colours[frame][2]);
  }
  ID3D11RenderTargetView_Release(rtv); IDXGISwapChain_Release(sc); ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
  printf("d3d11clear ok\n"); return 0;
}
