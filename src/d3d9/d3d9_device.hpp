#pragma once

#include "d3d9_private.h"
#include "d3d9_surface.hpp"
#include <atomic>

namespace dxmt {

class D3D9Interface;

// Full IDirect3DDevice9Ex implementation wrapping D3D11.
// Methods that are not yet implemented return E_NOTIMPL. Detailed D3D9 tracing
// is opt-in via DXMT_D3D9_TRACE=1.
class D3D9Device final : public IDirect3DDevice9Ex {
public:
  D3D9Device(
    D3D9Interface          *pParent,
    IDXGIAdapter           *pAdapter,
    HWND                    hFocusWindow,
    DWORD                   BehaviorFlags,
    D3DPRESENT_PARAMETERS  *pPP,
    bool                    isEx);

  ~D3D9Device();

  HRESULT Initialize();

  // Internal accessors for d3d9 resource objects
  ID3D11Device        *GetD3D11Device()  { return d3d11_device_.ptr(); }
  ID3D11DeviceContext *GetD3D11Context() { return d3d11_ctx_.ptr(); }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  ULONG   STDMETHODCALLTYPE AddRef() final;
  ULONG   STDMETHODCALLTYPE Release() final;

  // IDirect3DDevice9 — core device management
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() final;
  UINT    STDMETHODCALLTYPE GetAvailableTextureMem() final;
  HRESULT STDMETHODCALLTYPE EvictManagedResources() final;
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **ppD3D9) final;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *pCaps) final;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParams) final;
  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pSurface) final;
  void    STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) final;
  BOOL    STDMETHODCALLTYPE ShowCursor(BOOL bShow) final;
  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPP, IDirect3DSwapChain9 **ppSwapChain) final;
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **ppSwapChain) final;
  UINT    STDMETHODCALLTYPE GetNumberOfSwapChains() final;
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pPP) final;
  HRESULT STDMETHODCALLTYPE Present(const RECT *pSrc, const RECT *pDst, HWND hDest, const RGNDATA *pDirtyRegion) final;
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) final;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) final;
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) final;
  void    STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) final;
  void    STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) final;

  // Texture / surface creation
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) final;
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *pSrc, const RECT *pSrcRect, IDirect3DSurface9 *pDst, const POINT *pDstPoint) final;
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *pSrc, IDirect3DBaseTexture9 *pDst) final;
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *pRT, IDirect3DSurface9 *pDst) final;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDst) final;
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *pSrc, const RECT *pSrcRect, IDirect3DSurface9 *pDst, const RECT *pDstRect, D3DTEXTUREFILTERTYPE Filter) final;
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color) final;
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) final;

  // Render state / output merger
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) final;
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) final;
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *pSurface) final;
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) final;
  HRESULT STDMETHODCALLTYPE BeginScene() final;
  HRESULT STDMETHODCALLTYPE EndScene() final;
  HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) final;
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) final;
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) final;
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) final;
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *pViewport) final;
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) final;
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *pMaterial) final;
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) final;
  HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9 *pLight) final;
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight) final;
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) final;
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL *pEnable) final;
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float *pPlane) final;
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) final;
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) final;
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) final;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) final;
  HRESULT STDMETHODCALLTYPE BeginStateBlock() final;
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) final;
  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) final;
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) final;
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) final;
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) final;
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) final;
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) final;
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) final;
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) final;
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) final;
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) final;
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) final;
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) final;
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *pPaletteNumber) final;
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *pRect) final;
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) final;
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) final;
  BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() final;
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) final;
  float   STDMETHODCALLTYPE GetNPatchMode() final;

  // Draw calls
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) final;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) final;
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) final;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) final;
  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDstBuffer, IDirect3DVertexDeclaration9 *pDecl, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) final;
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) final;
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) final;
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) final;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) final;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader) final;
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) final;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) final;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) final;
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) final;
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) final;
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) final;
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) final;
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) final;
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData) final;
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData) final;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader) final;
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader) final;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) final;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) final;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) final;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) final;
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) final;
  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo) final;
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) final;
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) final;
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) final;

  // IDirect3DDevice9Ex
  HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT Width, UINT Height, float *pRows, float *pColumns) final;
  HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset) final;
  HRESULT STDMETHODCALLTYPE PresentEx(const RECT *pSrc, const RECT *pDst, HWND hDest, const RGNDATA *pDirtyRegion, DWORD dwFlags) final;
  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority) final;
  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) final;
  HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT iSwapChain) final;
  HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9 **pResourceArray, UINT32 NumResources) final;
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) final;
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND hDestinationWindow) final;
  HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage) final;
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage) final;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage) final;
  HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS *pPP, D3DDISPLAYMODEEX *pFullscreenDisplayMode) final;
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) final;

private:
  std::atomic<ULONG>         ref_ = {1};
  D3D9Interface             *parent_;
  Com<IDXGIAdapter>          dxgi_adapter_;
  Com<ID3D11Device>          d3d11_device_;
  Com<ID3D11DeviceContext>   d3d11_ctx_;
  Com<IDXGISwapChain>        swap_chain_;
  HWND                       focus_wnd_;
  DWORD                      behavior_flags_;
  D3DPRESENT_PARAMETERS      present_params_;
  bool                       is_ex_;
  RECT                       scissor_rect_ = {};

  // Output-merger state (Phase 1)
  static constexpr UINT      kMaxRenderTargets = 4;
  Com<D3D9Surface>           backbuffer_;                 // implicit swapchain back buffer
  Com<D3D9Surface>           auto_depth_;                 // implicit auto depth-stencil
  Com<D3D9Surface>           render_targets_[kMaxRenderTargets] = {};
  Com<D3D9Surface>           depth_stencil_;

  // Rebind the currently set render targets / depth-stencil onto the D3D11 OM.
  void ApplyRenderTargets();
  // Build the implicit back buffer (and optional auto depth) surfaces.
  HRESULT CreateImplicitSurfaces();
};

} // namespace dxmt
