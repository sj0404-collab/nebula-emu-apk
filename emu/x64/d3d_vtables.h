/* generated from mingw-w64 d3d11.h / dxgi.h: COM vtable method order per interface */
typedef struct { const char *name; int n; const char *const *methods; } ComIface;
static const char *const vt_ID3D11Device[] = { "QueryInterface", "AddRef", "Release", "CreateBuffer", "CreateTexture1D", "CreateTexture2D", "CreateTexture3D", "CreateShaderResourceView", "CreateUnorderedAccessView", "CreateRenderTargetView", "CreateDepthStencilView", "CreateInputLayout", "CreateVertexShader", "CreateGeometryShader", "CreateGeometryShaderWithStreamOutput", "CreatePixelShader", "CreateHullShader", "CreateDomainShader", "CreateComputeShader", "CreateClassLinkage", "CreateBlendState", "CreateDepthStencilState", "CreateRasterizerState", "CreateSamplerState", "CreateQuery", "CreatePredicate", "CreateCounter", "CreateDeferredContext", "OpenSharedResource", "CheckFormatSupport", "CheckMultisampleQualityLevels", "CheckCounterInfo", "CheckCounter", "CheckFeatureSupport", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetFeatureLevel", "GetCreationFlags", "GetDeviceRemovedReason", "GetImmediateContext", "SetExceptionMode", "GetExceptionMode" };
static const char *const vt_ID3D11DeviceContext[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "VSSetConstantBuffers", "PSSetShaderResources", "PSSetShader", "PSSetSamplers", "VSSetShader", "DrawIndexed", "Draw", "Map", "Unmap", "PSSetConstantBuffers", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer", "DrawIndexedInstanced", "DrawInstanced", "GSSetConstantBuffers", "GSSetShader", "IASetPrimitiveTopology", "VSSetShaderResources", "VSSetSamplers", "Begin", "End", "GetData", "SetPredication", "GSSetShaderResources", "GSSetSamplers", "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState", "OMSetDepthStencilState", "SOSetTargets", "DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect", "Dispatch", "DispatchIndirect", "RSSetState", "RSSetViewports", "RSSetScissorRects", "CopySubresourceRegion", "CopyResource", "UpdateSubresource", "CopyStructureCount", "ClearRenderTargetView", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat", "ClearDepthStencilView", "GenerateMips", "SetResourceMinLOD", "GetResourceMinLOD", "ResolveSubresource", "ExecuteCommandList", "HSSetShaderResources", "HSSetShader", "HSSetSamplers", "HSSetConstantBuffers", "DSSetShaderResources", "DSSetShader", "DSSetSamplers", "DSSetConstantBuffers", "CSSetShaderResources", "CSSetUnorderedAccessViews", "CSSetShader", "CSSetSamplers", "CSSetConstantBuffers", "VSGetConstantBuffers", "PSGetShaderResources", "PSGetShader", "PSGetSamplers", "VSGetShader", "PSGetConstantBuffers", "IAGetInputLayout", "IAGetVertexBuffers", "IAGetIndexBuffer", "GSGetConstantBuffers", "GSGetShader", "IAGetPrimitiveTopology", "VSGetShaderResources", "VSGetSamplers", "GetPredication", "GSGetShaderResources", "GSGetSamplers", "OMGetRenderTargets", "OMGetRenderTargetsAndUnorderedAccessViews", "OMGetBlendState", "OMGetDepthStencilState", "SOGetTargets", "RSGetState", "RSGetViewports", "RSGetScissorRects", "HSGetShaderResources", "HSGetShader", "HSGetSamplers", "HSGetConstantBuffers", "DSGetShaderResources", "DSGetShader", "DSGetSamplers", "DSGetConstantBuffers", "CSGetShaderResources", "CSGetUnorderedAccessViews", "CSGetShader", "CSGetSamplers", "CSGetConstantBuffers", "ClearState", "Flush", "GetType", "GetContextFlags", "FinishCommandList" };
static const char *const vt_ID3D11Texture2D[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetType", "SetEvictionPriority", "GetEvictionPriority", "GetDesc" };
static const char *const vt_ID3D11RenderTargetView[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetResource", "GetDesc" };
static const char *const vt_ID3D11DepthStencilView[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetResource", "GetDesc" };
static const char *const vt_ID3D11Buffer[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetType", "SetEvictionPriority", "GetEvictionPriority", "GetDesc" };
static const char *const vt_ID3D11VertexShader[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface" };
static const char *const vt_ID3D11PixelShader[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface" };
static const char *const vt_ID3D11InputLayout[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface" };
static const char *const vt_ID3D11RasterizerState[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetDesc" };
static const char *const vt_ID3D11DepthStencilState[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetDesc" };
static const char *const vt_ID3D11BlendState[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetDesc" };
static const char *const vt_ID3D11SamplerState[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetDesc" };
static const char *const vt_ID3D11ShaderResourceView[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetResource", "GetDesc" };
static const char *const vt_ID3D11Query[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "GetDataSize", "GetDesc" };
static const char *const vt_ID3D11GeometryShader[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface" };
static const char *const vt_IDXGISwapChain[] = { "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent", "GetDevice", "Present", "GetBuffer", "SetFullscreenState", "GetFullscreenState", "GetDesc", "ResizeBuffers", "ResizeTarget", "GetContainingOutput", "GetFrameStatistics", "GetLastPresentCount" };
static const char *const vt_IDXGIFactory1[] = { "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent", "EnumAdapters", "MakeWindowAssociation", "GetWindowAssociation", "CreateSwapChain", "CreateSoftwareAdapter", "EnumAdapters1", "IsCurrent" };
static const char *const vt_IDXGIAdapter1[] = { "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent", "EnumOutputs", "GetDesc", "CheckInterfaceSupport", "GetDesc1" };
static const char *const vt_IDXGIDevice[] = { "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent", "GetAdapter", "CreateSurface", "QueryResourceResidency", "SetGPUThreadPriority", "GetGPUThreadPriority" };
static const char *const vt_IDXGIOutput[] = { "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent", "GetDesc", "GetDisplayModeList", "FindClosestMatchingMode", "WaitForVBlank", "TakeOwnership", "ReleaseOwnership", "GetGammaControlCapabilities", "SetGammaControl", "GetGammaControl", "SetDisplaySurface", "GetDisplaySurfaceData", "GetFrameStatistics" };
static const ComIface g_com_ifaces[] = {
  { "ID3D11Device", 43, vt_ID3D11Device },
  { "ID3D11DeviceContext", 115, vt_ID3D11DeviceContext },
  { "ID3D11Texture2D", 11, vt_ID3D11Texture2D },
  { "ID3D11RenderTargetView", 9, vt_ID3D11RenderTargetView },
  { "ID3D11DepthStencilView", 9, vt_ID3D11DepthStencilView },
  { "ID3D11Buffer", 11, vt_ID3D11Buffer },
  { "ID3D11VertexShader", 7, vt_ID3D11VertexShader },
  { "ID3D11PixelShader", 7, vt_ID3D11PixelShader },
  { "ID3D11InputLayout", 7, vt_ID3D11InputLayout },
  { "ID3D11RasterizerState", 8, vt_ID3D11RasterizerState },
  { "ID3D11DepthStencilState", 8, vt_ID3D11DepthStencilState },
  { "ID3D11BlendState", 8, vt_ID3D11BlendState },
  { "ID3D11SamplerState", 8, vt_ID3D11SamplerState },
  { "ID3D11ShaderResourceView", 9, vt_ID3D11ShaderResourceView },
  { "ID3D11Query", 9, vt_ID3D11Query },
  { "ID3D11GeometryShader", 7, vt_ID3D11GeometryShader },
  { "IDXGISwapChain", 18, vt_IDXGISwapChain },
  { "IDXGIFactory1", 14, vt_IDXGIFactory1 },
  { "IDXGIAdapter1", 11, vt_IDXGIAdapter1 },
  { "IDXGIDevice", 12, vt_IDXGIDevice },
  { "IDXGIOutput", 19, vt_IDXGIOutput },
};
enum { IF_ID3D11DEVICE, IF_ID3D11DEVICECONTEXT, IF_ID3D11TEXTURE2D, IF_ID3D11RENDERTARGETVIEW, IF_ID3D11DEPTHSTENCILVIEW, IF_ID3D11BUFFER, IF_ID3D11VERTEXSHADER, IF_ID3D11PIXELSHADER, IF_ID3D11INPUTLAYOUT, IF_ID3D11RASTERIZERSTATE, IF_ID3D11DEPTHSTENCILSTATE, IF_ID3D11BLENDSTATE, IF_ID3D11SAMPLERSTATE, IF_ID3D11SHADERRESOURCEVIEW, IF_ID3D11QUERY, IF_ID3D11GEOMETRYSHADER, IF_IDXGISWAPCHAIN, IF_IDXGIFACTORY1, IF_IDXGIADAPTER1, IF_IDXGIDEVICE, IF_IDXGIOUTPUT, IF_COUNT };
#define M_ID3D11Device_QueryInterface 0
#define M_ID3D11Device_AddRef 1
#define M_ID3D11Device_Release 2
#define M_ID3D11Device_CreateBuffer 3
#define M_ID3D11Device_CreateTexture2D 5
#define M_ID3D11Device_CreateRenderTargetView 9
#define M_ID3D11Device_CreateDepthStencilView 10
#define M_ID3D11Device_CreateInputLayout 11
#define M_ID3D11Device_CreateVertexShader 12
#define M_ID3D11Device_CreatePixelShader 15
#define M_ID3D11Device_CreateBlendState 20
#define M_ID3D11Device_CreateDepthStencilState 21
#define M_ID3D11Device_CreateRasterizerState 22
#define M_ID3D11Device_CreateSamplerState 23
#define M_ID3D11Device_CreateShaderResourceView 7
#define M_ID3D11Device_CreateQuery 24
#define M_ID3D11Device_GetImmediateContext 40
#define M_ID3D11Device_GetFeatureLevel 37
#define M_ID3D11Device_GetCreationFlags 38
#define M_ID3D11Device_GetDeviceRemovedReason 39
#define M_ID3D11Device_CheckMultisampleQualityLevels 30
#define M_ID3D11Device_CheckFormatSupport 29
#define M_ID3D11DeviceContext_OMSetRenderTargets 33
#define M_ID3D11DeviceContext_ClearRenderTargetView 50
#define M_ID3D11DeviceContext_ClearDepthStencilView 53
#define M_ID3D11DeviceContext_RSSetViewports 44
#define M_ID3D11DeviceContext_RSSetScissorRects 45
#define M_ID3D11DeviceContext_IASetVertexBuffers 18
#define M_ID3D11DeviceContext_IASetIndexBuffer 19
#define M_ID3D11DeviceContext_IASetInputLayout 17
#define M_ID3D11DeviceContext_IASetPrimitiveTopology 24
#define M_ID3D11DeviceContext_VSSetShader 11
#define M_ID3D11DeviceContext_PSSetShader 9
#define M_ID3D11DeviceContext_VSSetConstantBuffers 7
#define M_ID3D11DeviceContext_PSSetConstantBuffers 16
#define M_ID3D11DeviceContext_PSSetSamplers 10
#define M_ID3D11DeviceContext_PSSetShaderResources 8
#define M_ID3D11DeviceContext_Draw 13
#define M_ID3D11DeviceContext_DrawIndexed 12
#define M_ID3D11DeviceContext_Map 14
#define M_ID3D11DeviceContext_Unmap 15
#define M_ID3D11DeviceContext_UpdateSubresource 48
#define M_ID3D11DeviceContext_OMSetBlendState 35
#define M_ID3D11DeviceContext_OMSetDepthStencilState 36
#define M_ID3D11DeviceContext_RSSetState 43
#define M_ID3D11DeviceContext_Flush 111
#define M_ID3D11DeviceContext_GetDevice 3
#define M_ID3D11DeviceContext_ClearState 110
#define M_IDXGISwapChain_Present 8
#define M_IDXGISwapChain_GetBuffer 9
#define M_IDXGISwapChain_GetDesc 12
#define M_IDXGISwapChain_ResizeBuffers 13
#define M_IDXGISwapChain_SetFullscreenState 10
#define M_IDXGISwapChain_GetFullscreenState 11
#define M_IDXGISwapChain_GetParent 6
#define M_IDXGISwapChain_GetDevice 7
#define M_ID3D11Texture2D_GetDesc 10
#define M_ID3D11Texture2D_GetType 7
#define M_ID3D11Texture2D_GetDevice 3
#define M_ID3D11RenderTargetView_GetResource 7
#define M_ID3D11RenderTargetView_GetDesc 8
