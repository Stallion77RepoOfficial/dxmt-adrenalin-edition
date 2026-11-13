#pragma once

#include "d3d9_private.h"
#include "d3d11_1.h"
#include "d3d9_surface.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_vertex_declaration.hpp"
/* The binding slots below hold private references, which need the complete
 * type where the member is declared. */
#include "d3d9_buffer.hpp"
#include "d3d9_swapchain.hpp"
#include "d3d9_stateblock.hpp"
#include "d3d9_ffp.hpp"
#include "d3d9_device_child.hpp"
#include "util_env.hpp"
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace dxmt {

class D3D9Interface;
class D3D9VertexBuffer;
class D3D9IndexBuffer;
class D3D9SwapChain;
class D3D9StateBlock;

// Full IDirect3DDevice9Ex implementation wrapping D3D11.
// Methods that are not yet implemented return E_NOTIMPL.
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
  const D3DPRESENT_PARAMETERS &PresentParams() const { return present_params_; }
  /* Resource locks report their cost here so DXMT_FPS can show it beside the
   * draw work: a lock is a CPU/GPU rendezvous and does not appear in any of
   * the other buckets. */
  void AccountLock(double Seconds) { diag_lock_s_ += Seconds; locks_since_report_++; }
  /* DXMT_FPS=1. Gates every diagnostic clock read: under Wine each one is a
   * syscall, so they cannot run unconditionally on draws and locks. */
  bool DiagnosticsEnabled() const { return fps_report_; }
  /* D3DCREATE_MULTITHREADED: the application calls into the device from several
   * threads, and every entry point holds this lock, for D3D9's own state and for
   * the D3D11 context it records into, which takes one caller at a time. Without
   * the flag D3D9 promises nothing across threads and the lock is never taken. */
  std::unique_lock<std::recursive_mutex> LockDevice() {
    return multithreaded_ ? std::unique_lock<std::recursive_mutex>(mutex_) : std::unique_lock<std::recursive_mutex>();
  }
  /* True when the IDirect3D9 this device came from was created by
   * Direct3DCreate9Ex. That, and not which CreateDevice was called, is what
   * decides whether this device and its swap chain answer to the Ex form. */
  bool ExtendedInterface() const;

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
  Com<ID3D11DeviceContext1>  constant_context_;
  Com<IDXGISwapChain>        swap_chain_;
  HWND                       focus_wnd_;
  DWORD                      behavior_flags_;
  bool                       multithreaded_;
  std::recursive_mutex       mutex_;
  D3DPRESENT_PARAMETERS      present_params_;
  // DXMT_FPS=1: report frame rate and draws per frame from Present.
  bool                       fps_report_ = env::getEnvVar("DXMT_FPS") == "1";
  uint64_t                   draws_since_report_ = 0;
  double                     diag_prepare_s_ = 0, diag_upload_s_ = 0, diag_bind_s_ = 0, diag_present_s_ = 0;
  double                     diag_lock_s_ = 0;
  uint64_t                   locks_since_report_ = 0;
  bool                       is_ex_;
  RECT                       scissor_rect_ = {};
  D3DVIEWPORT9               viewport_ = {}; // as set; D3D11 gets it half a pixel on

  /* The implicit objects, and every binding below them, are held privately.
   *
   * D3D9 gives the implicit swap chain, the implicit back buffer and the auto
   * depth-stencil a public count that starts at zero, returns to zero without
   * the object being freed, and is not forwarded to whatever contains them --
   * so the device cannot be one of the holders of that count. It also requires
   * SetRenderTarget, SetTexture, SetIndices and SetStreamSource to leave a
   * count alone. A private reference does both: the object stays alive for as
   * long as the device needs it while the application's count is its own.
   *
   * That is also what keeps the graph acyclic, since a child holds a public
   * reference on the device (see D3D9DeviceChild): were these public, a bound
   * object and the device would each keep the other alive forever. */
  Com<D3D9SwapChain, false>  implicit_swapchain_;

  // Output-merger state (Phase 1)
  static constexpr UINT      kMaxRenderTargets = 4;
  Com<D3D9Surface, false>    backbuffer_;                 // implicit swapchain back buffer
  Com<D3D9Surface, false>    auto_depth_;                 // implicit auto depth-stencil
  Com<D3D9Surface, false>    render_targets_[kMaxRenderTargets] = {};
  Com<D3D9Surface, false>    depth_stencil_;

  /* Back buffers 1..BackBufferCount-1. Back buffer 0 is the swap chain's
   * own buffer; the others are plain render targets whose contents move one
   * place up the chain on every Present, as D3D9 defines it. */
  std::vector<Com<D3D9Surface, false>> extra_backbuffers_;
  Com<ID3D11Texture2D>       rotate_temp_;
  void RotateBackBuffers();

  // Rebind the currently set render targets / depth-stencil onto the D3D11 OM.
  void ApplyRenderTargets();
  void SetDefaultViewport();
  void ApplyViewport(const D3DVIEWPORT9 &viewport);
  // The DXGI swap chain for present_params_, multisampled as they ask.
  HRESULT CreateSwapChain();
  UINT                       swapchain_samples_ = 1;
  // Build the implicit back buffer (and optional auto depth) surfaces.
  HRESULT CreateImplicitSurfaces();

  /* Scratch storage for the user-pointer draws.
   *
   * DrawPrimitiveUP hands over its vertices by pointer and D3D11 has no such
   * call, so the data has to reach a buffer somehow. Creating one per draw is
   * the obvious translation and the wrong one: it is a GPU allocation on every
   * draw call, and Unreal's menus issue tens of them a frame.
   *
   * One dynamic buffer per kind instead, written at a rolling offset --
   * NO_OVERWRITE while there is room, DISCARD when it wraps, which is the
   * contract that lets the driver keep the frames in flight apart. It grows
   * only when a single draw wants more than it holds. */
  struct UploadRing {
    Com<ID3D11Buffer> buffer;
    UINT              capacity = 0;
    UINT              offset = 0;
  };
  UploadRing                 up_vertices_;
  UploadRing                 up_indices_;
  // Returns the byte offset the data was written at, or -1 on failure.
  UINT WriteToUploadRing(UploadRing &ring, D3D11_BIND_FLAG Bind, const void *pData, UINT Size, UINT Alignment);

  // Draw pipeline state
  static constexpr UINT      kMaxStreams = 16;
  static constexpr UINT      kMaxSamplers = 16;      // pixel stage s0..s15
  static constexpr UINT      kMaxVertexSamplers = 4; // D3DVERTEXTEXTURESAMPLER0..3
  static constexpr UINT      kTotalSamplers = kMaxSamplers + kMaxVertexSamplers;
  static constexpr UINT      kMaxFloatConstants = 256;
  static constexpr UINT      kMaxOtherConstants = 16; // i# / b# registers

  struct StreamBinding {
    Com<D3D9VertexBuffer, false> buffer;
    UINT offset = 0;
    UINT stride = 0;
  };
  StreamBinding              streams_[kMaxStreams] = {};
  UINT                       stream_freq_[kMaxStreams] = {}; // raw SetStreamSourceFreq values, 1 = default
  Com<D3D9IndexBuffer, false> index_buffer_;
  Com<D3D9VertexDeclaration, false> vertex_decl_;
  DWORD                      fvf_ = 0;
  /* One declaration per FVF for the device's lifetime. Rebuilding it on every
   * FVF change freed the previous one, whose memory -- and with it, before
   * layouts were keyed on ids, its cached input layout -- went to the next. */
  std::unordered_map<DWORD, Com<D3D9VertexDeclaration, false>> fvf_decls_;
  Com<D3D9VertexShader, false> vertex_shader_;
  Com<D3D9PixelShader, false>  pixel_shader_;

  /* A bound texture. The slot can hold any of the three texture classes, so
   * the private reference that keeps it alive is taken through their common
   * base while the draw path reads the D3D9 interface. */
  struct TextureBinding {
    Com<D3D9PrivateRef, false> owner;
    IDirect3DBaseTexture9     *texture = nullptr;

    void Set(IDirect3DBaseTexture9 *tex);
    explicit operator bool() const { return texture != nullptr; }
  };
  TextureBinding             textures_[kTotalSamplers] = {}; // 0..15 pixel, 16..19 vertex

  // Per-sampler D3D9 state (indexed by D3DSAMPLERSTATETYPE, 1..13) and the
  // cache of D3D11 sampler objects they resolve to.
  DWORD                      sampler_state_[kTotalSamplers][14] = {};
  static constexpr uint32_t   kAllSamplers = (1u << kTotalSamplers) - 1;
  uint32_t                   samplers_dirty_ = kAllSamplers;
  std::map<std::array<uint32_t, 10>, Com<ID3D11SamplerState>> sampler_cache_;
  // Owned by sampler_cache_; resolve only the slots whose state or shadow
  // comparison mode changed, rather than searching the map 20 times per draw.
  std::array<ID3D11SamplerState *, kTotalSamplers> resolved_samplers_ = {};
  uint16_t                   resolved_shadow_samplers_ = 0;
  void InitDefaultSamplerState();
  ID3D11SamplerState *GetOrCreateSampler(UINT stage, bool comparison = false);
  // Pixel samplers the bound shader reads as shadow maps; they need
  // comparison samplers.
  uint16_t shadow_samplers_ = 0;

  float                      vs_const_f_[kMaxFloatConstants * 4] = {};
  float                      ps_const_f_[kMaxFloatConstants * 4] = {};
  int                        vs_const_i_[kMaxOtherConstants * 4] = {};
  int                        ps_const_i_[kMaxOtherConstants * 4] = {};
  uint32_t                   vs_const_b_[kMaxOtherConstants] = {};
  uint32_t                   ps_const_b_[kMaxOtherConstants] = {};
  Com<ID3D11Buffer>          vs_cbuffer_;
  Com<ID3D11Buffer>          ps_cbuffer_;
  struct ConstantUpload {
    UINT cursor = 0;
    UINT first = 0;
    UINT count = 0;
    UINT uploaded = 0;
  } vs_upload_, ps_upload_;
  Com<ID3D11Buffer>          vs_cbuffer_extra_; // cb1: i#/b#/alpha-ref, layout in sm3_translate.hpp
  Com<ID3D11Buffer>          ps_cbuffer_extra_;
  bool                       vs_const_dirty_ = true;
  bool                       ps_const_dirty_ = true;
  bool                       vs_extra_dirty_ = true;
  bool                       ps_extra_dirty_ = true;

  /* (declaration, shader, stream frequencies) -> input layout, keyed on the
   * objects' unique ids rather than their addresses, which get reused. The
   * shader is the D3D9 vertex shader's id, or the generated fixed-function
   * shader set's address (the FFP cache never frees one). Entries are not
   * dropped when an object dies: a small, bounded leak of D3D11 objects. */
  struct InputLayoutKey {
    uint64_t declaration;
    uint64_t shader;
    bool fixed_function;
    std::array<UINT, 16> stream_freq;
    bool operator==(const InputLayoutKey &) const = default;
  };
  struct InputLayoutKeyHash {
    size_t operator()(const InputLayoutKey &k) const {
      uint64_t h = k.declaration * 0x9E3779B97F4A7C15ull ^ (k.shader + (k.fixed_function ? 0x51ED27ull : 0));
      for (UINT f : k.stream_freq)
        h = (h ^ f) * 0x100000001B3ull;
      return (size_t)h;
    }
  };
  struct CachedInputLayout {
    Com<ID3D11InputLayout> layout;
    bool null_stream = false; // some shader input has no declaration element
  };
  std::unordered_map<InputLayoutKey, CachedInputLayout, InputLayoutKeyHash> input_layout_cache_;
  /* VertexInputConversions() of the last declaration and vertex shader. */
  uint64_t conversions_declaration_ = 0;
  uint64_t conversions_shader_ = 0;
  uint32_t conversions_ = 0;
  /* A shader input the declaration does not provide reads zeroes, as on D3D9
   * hardware: it is fed from this stride-0 buffer in an input slot past
   * D3D9's sixteen streams. */
  static constexpr UINT      kNullStreamSlot = 16;
  Com<ID3D11Buffer>          null_stream_buffer_;
  void BindNullStream();
  void UseInputLayout(const CachedInputLayout &cached);

  const std::vector<D3DVERTEXELEMENT9> *CurrentDeclarationElements();
  D3D9VertexDeclaration *CurrentDeclaration();
  D3D9VertexDeclaration *ImplicitDeclarationForFVF(DWORD fvf);
  HRESULT BindInputLayout();
  uint32_t VertexInputConversions();
  void UploadShaderConstants(bool programmable_vs, bool programmable_ps);
  void BindTexturesAndSamplers();
  HRESULT DrawPrepare(D3DPRIMITIVETYPE type);
  // Instance count for DrawIndexedPrimitive when stream 0 uses
  // D3DSTREAMSOURCE_INDEXEDDATA, or 0 when instancing is off.
  UINT InstancedDrawCount() const;

  // Growable shared index buffer turning a triangle fan with N primitives
  // into a triangle list ((0,i+1,i+2) per triangle); D3D11 dropped fans.
  Com<ID3D11Buffer>          fan_index_buffer_;
  UINT                       fan_index_capacity_ = 0; // in primitives
  bool EnsureFanIndexBuffer(UINT primCount);

  // Full D3D9 render-state block plus caches for the D3D11 state objects it
  // translates into (depth-stencil / rasterizer / blend), keyed on the packed
  // relevant states so re-creation only happens on genuinely new combinations.
  static constexpr UINT kMaxRenderStates = 256;
  DWORD                      render_state_[kMaxRenderStates] = {};
  bool                       render_state_dirty_ = true;
  std::map<uint64_t, Com<ID3D11DepthStencilState>>            dss_cache_;
  std::map<std::array<uint32_t, 4>, Com<ID3D11RasterizerState>> rs_cache_;
  std::map<uint64_t, Com<ID3D11BlendState>>                   bs_cache_;
  void InitDefaultRenderState();
  void ApplyRenderState();

  // Fixed-function state. Stored so Get* round-trips and state blocks work;
  // the FFP draw path consumes it when no programmable shader is bound.
  std::unordered_map<uint32_t, D3DMATRIX> transforms_; // D3DTS_* -> matrix, absent = identity
  D3DMATERIAL9               material_ = {};
  struct LightSlot {
    D3DLIGHT9 light = {};
    bool present = false;
    bool enabled = false;
  };
  std::map<DWORD, LightSlot> lights_;
  static constexpr UINT      kMaxClipPlanes = 8;
  /* D3DRS_CLIPPLANEENABLE, unless D3DRS_CLIPPING turns all clipping off. */
  bool UserClipPlanesEnabled() const {
    return render_state_[D3DRS_CLIPPING] && (render_state_[D3DRS_CLIPPLANEENABLE] & 0xff);
  }
  float                      clip_planes_[kMaxClipPlanes][4] = {};
  static constexpr UINT      kMaxTextureStages = 8;
  static constexpr UINT      kMaxTextureStageStates = 33; // D3DTSS_CONSTANT == 32
  DWORD                      tss_[kMaxTextureStages][kMaxTextureStageStates] = {};
  void InitDefaultTextureStageState();

  // Active BeginStateBlock/EndStateBlock recording, if any. Setters report
  // into it so EndStateBlock captures exactly the touched states.
  /* Private while recording: BeginStateBlock must not move the device's
   * count, and EndStateBlock hands the block over with a public reference
   * that does. */
  Com<D3D9StateBlock, false> recording_;

  // Fixed-function shader generation (used when no VS and/or PS is bound).
  std::unique_ptr<FFPShaderCache> ffp_;
  Com<ID3D11Buffer>          ffp_vs_cbuffer_;
  Com<ID3D11Buffer>          ffp_ps_cbuffer_;
  HRESULT BindInputLayoutFFP(D3D9VertexDeclaration *decl, FFPShaders *shaders, const FFPShaderKey &key);
  void UploadFFPConstants(const FFPShaderKey &key, bool bind_vs, bool bind_ps);

  // Shader-based blit for StretchRect calls CopySubresourceRegion can't
  // express (scaling and/or format conversion).
  Com<ID3D11VertexShader>    blit_vs_;
  Com<ID3D11PixelShader>     blit_ps_;
  Com<ID3D11InputLayout>     blit_layout_;
  Com<ID3D11Buffer>          blit_vb_;
  Com<ID3D11SamplerState>    blit_sampler_point_;
  Com<ID3D11SamplerState>    blit_sampler_linear_;
  Com<ID3D11RasterizerState> blit_rasterizer_;
  bool EnsureBlitObjects();
  HRESULT BlitSurface(ID3D11ShaderResourceView *srv, UINT src_width, UINT src_height, const RECT &sr,
                      D3D9Surface *dst, const RECT &dr, bool linear);
  /* A single-sample copy of a multisampled surface, for what StretchRect
   * cannot resolve in one step: kept, and remade when the size or format
   * changes. */
  ID3D11Texture2D *ResolveToScratch(D3D9Surface *src, ID3D11ShaderResourceView **srv);
  Com<ID3D11Texture2D>       resolve_scratch_;
  Com<ID3D11ShaderResourceView> resolve_scratch_srv_;

  friend class D3D9StateBlock;
  friend class FFPShaderCache;
};

} // namespace dxmt
