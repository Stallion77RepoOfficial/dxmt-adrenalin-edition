#pragma once
#include "dxmt_buffer.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_staging.hpp"
#include "dxmt_texture.hpp"
#include "d3d11_device_child.hpp"
#include "com/com_pointer.hpp"
#include "com/com_guid.hpp"
#include "d3d11_view.hpp"
#include "dxgi_resource.hpp"
#include "log/log.hpp"
#include "../d3d10/d3d10_buffer.hpp"
#include "../d3d10/d3d10_texture.hpp"
#include "../d3d10/d3d10_view.hpp"
#include <memory>
#include <type_traits>
#include <random>
#include <cstdio>

DEFINE_COM_INTERFACE("9a6f6549-d4b1-45ea-8794-8503d190d3d1",
                     IMTLMinLODClampable)
    : public IUnknown {
  virtual void SetMinLOD(float MinLOD) = 0;
  virtual float GetMinLOD() = 0;
};

namespace dxmt {

template <typename RESOURCE_DESC>
void UpgradeResourceDescription(const RESOURCE_DESC *pSrc, RESOURCE_DESC &dst) {
  dst = *pSrc;
}
template <typename RESOURCE_DESC>
void DowngradeResourceDescription(const RESOURCE_DESC &src,
                                  RESOURCE_DESC *pDst) {
  *pDst = src;
}

template <typename RESOURCE_DESC_SRC, typename RESOURCE_DESC_DST>
void UpgradeResourceDescription(const RESOURCE_DESC_SRC *pSrc,
                                RESOURCE_DESC_DST &dst);
template <typename RESOURCE_DESC_SRC, typename RESOURCE_DESC_DST>
void DowngradeResourceDescription(const RESOURCE_DESC_SRC &src,
                                  RESOURCE_DESC_DST *pDst);

template <typename VIEW_DESC>
void UpgradeViewDescription(const VIEW_DESC *pSrc, VIEW_DESC &dst) {
  dst = *pSrc;
}
template <typename VIEW_DESC>
void DowngradeViewDescription(const VIEW_DESC &src, VIEW_DESC *pDst) {
  *pDst = src;
}

template <typename VIEW_DESC_SRC, typename VIEW_DESC_DST>
void UpgradeViewDescription(const VIEW_DESC_SRC *pSrc, VIEW_DESC_DST &dst);
template <typename VIEW_DESC_SRC, typename VIEW_DESC_DST>
void DowngradeViewDescription(const VIEW_DESC_SRC &src, VIEW_DESC_DST *pDst);

struct tag_buffer {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_BUFFER;
  using COM = ID3D11Buffer;
  using COM_IMPL = ID3D11Buffer;
  using DESC = D3D11_BUFFER_DESC;
  using DESC1 = D3D11_BUFFER_DESC;
  using D3D10 = ID3D10Buffer;
  using D3D10_IMPL = MTLD3D10Buffer;
  static constexpr std::string_view debug_name = "buffer";
};

struct tag_texture_1d {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_TEXTURE1D;
  using COM = ID3D11Texture1D;
  using COM_IMPL = ID3D11Texture1D;
  using DESC = D3D11_TEXTURE1D_DESC;
  using DESC1 = D3D11_TEXTURE1D_DESC;
  using D3D10 = ID3D10Texture1D;
  using D3D10_IMPL = MTLD3D10Texture1D;
  static constexpr std::string_view debug_name = "tex1d";
};

struct tag_texture_2d {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_TEXTURE2D;
  using COM = ID3D11Texture2D;
  using COM_IMPL = ID3D11Texture2D1;
  using DESC = D3D11_TEXTURE2D_DESC;
  using DESC1 = D3D11_TEXTURE2D_DESC1;
  using D3D10 = ID3D10Texture2D;
  using D3D10_IMPL = MTLD3D10Texture2D;
  static constexpr std::string_view debug_name = "tex2d";
};

struct tag_texture_3d {
  static const D3D11_RESOURCE_DIMENSION dimension =
      D3D11_RESOURCE_DIMENSION_TEXTURE3D;
  using COM = ID3D11Texture3D;
  using COM_IMPL = ID3D11Texture3D1;
  using DESC = D3D11_TEXTURE3D_DESC;
  using DESC1 = D3D11_TEXTURE3D_DESC1;
  using D3D10 = ID3D10Texture3D;
  using D3D10_IMPL = MTLD3D10Texture3D;
  static constexpr std::string_view debug_name = "tex3d";
};

struct D3D11ResourceCommon : ID3D11Resource {
    /* ID3D11Buffer::GetDesc */
    /* ID3D11Texture1D::GetDesc */
    /* ID3D11Texture2D::GetDesc */
    /* ID3D11Texture3D::GetDesc */
  virtual void STDMETHODCALLTYPE GetDesc(void *pDesc) = 0;
    /* ID3D11Texture2D1::GetDesc */
    /* ID3D11Texture3D1::GetDesc */
  virtual void STDMETHODCALLTYPE GetDesc1(void *pDesc) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc, ID3D11UnorderedAccessView1 **ppView) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc, ID3D11RenderTargetView1 **ppView) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppView) = 0;
  virtual HRESULT GetSharedHandle(HANDLE *pSharedHandle) = 0;
  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle) = 0;

  virtual Rc<StagingResource> staging(UINT Subresource) = 0;
  virtual Rc<DynamicBuffer> dynamicBuffer(UINT *pBufferLength, UINT *pBindFlags) = 0;
  virtual Rc<DynamicLinearTexture> dynamicLinearTexture(UINT *pBytesPerRow, UINT *pBytesPerImage) = 0;
  virtual Rc<DynamicBuffer> dynamicTexture(UINT Subresource, UINT *pBytesPerRow, UINT *pBytesPerImage) = 0;

  Rc<Buffer> buffer_{};
  Rc<Texture> texture_{};
  uint32_t bind_flags_{};
  uint32_t cpu_access_flags_{};
  uint32_t misc_flags_{};
  D3D11_USAGE usage_{};

  const Rc<Buffer> &buffer() const {
    return buffer_;
  }
  const Rc<Texture> &texture() const {
    return texture_;
  }
  uint32_t bindFlags() const {
    return bind_flags_;
  }
  uint32_t cpuAccessFlags() const {
    return cpu_access_flags_;
  }
  D3D11_USAGE usage() const {
    return usage_;
  }
  uint32_t miscFlags() const {
    return misc_flags_;
  }
  bool
  hazardsFree() const {
    return (bind_flags_ & (D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET |
                           D3D11_BIND_DEPTH_STENCIL)) == 0;
  }
};

inline D3D11ResourceCommon* GetResourceCommon(ID3D11Resource *pResource) {
  return static_cast<D3D11ResourceCommon *>(pResource);
}

inline Rc<DynamicBuffer>
GetDynamicBuffer(ID3D11Resource *pResource, UINT *pBufferLength, UINT *pBindFlags) {
  return static_cast<D3D11ResourceCommon *>(pResource)->dynamicBuffer(pBufferLength, pBindFlags);
}
inline Rc<DynamicLinearTexture>
GetDynamicLinearTexture(ID3D11Resource *pResource, UINT *pBytesPerRow, UINT *pBytesPerImage) {
  return static_cast<D3D11ResourceCommon *>(pResource)->dynamicLinearTexture(pBytesPerRow, pBytesPerImage);
}
inline Rc<DynamicBuffer>
GetDynamicTexture(ID3D11Resource *pResource, UINT Subresource, UINT *pBufferLength, UINT *pBindFlags) {
  return static_cast<D3D11ResourceCommon *>(pResource)->dynamicTexture(Subresource, pBufferLength, pBindFlags);
}
inline Rc<StagingResource>
GetStagingResource(ID3D11Resource *pResource, UINT Subresource) {

  return static_cast<D3D11ResourceCommon *>(pResource)->staging(Subresource);
};
inline Rc<Texture>
GetTexture(ID3D11Resource *pResource) {
  return static_cast<D3D11ResourceCommon *>(pResource)->texture();
}

template <typename tag, typename... Base>
class TResourceBase : public MTLD3D11DeviceChild<D3D11ResourceCommon, Base...> {
public:
  TResourceBase(const tag::DESC1 &desc, MTLD3D11Device *device) :
      MTLD3D11DeviceChild<D3D11ResourceCommon, Base...>(device),
      desc(desc),
      dxgi_resource(new MTLDXGIResource<TResourceBase<tag, Base...>>(this)),
      d3d10(reinterpret_cast<tag::COM *>(this), device->GetImmediateContextPrivate()) {
    // D3D11ResourceCommonß::bind_flags_
    this->bind_flags_ = desc.BindFlags;
    this->cpu_access_flags_ = desc.CPUAccessFlags;
    this->misc_flags_ = desc.MiscFlags;
    this->usage_ = desc.Usage;
    /* A texture of one subresource is also a DXGI surface -- what Direct2D
     * draws into and what a swap chain's buffer is asked for as. */
    if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ||
                  tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      if (desc.MipLevels == 1 && desc.ArraySize == 1)
        dxgi_surface.reset(new MTLDXGISurface<TResourceBase<tag, Base...>>(this));
    }
  }

  ~TResourceBase() {
    if (gdi_dc_)
      FreeGdiSurface();
  }

  /* D3D11 device children keep their device alive while the application holds
   * one. Without it the device's reference count did not move when a resource
   * or view was created, and an application that releases the device while
   * still using a texture it made from it is entitled to expect that to work.
   *
   * The reference goes to the container, because MTLD3D11Device::AddRef
   * forwards there, and no cycle results: the context holds everything it has
   * bound through Com<T, false>, which is a private reference. */
  ULONG STDMETHODCALLTYPE AddRef() override {
    uint32_t refCount = this->m_refCount++;
    if (unlikely(!refCount)) {
      this->m_parent->AddRef();
      this->AddRefPrivate();
    }
    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    uint32_t refCount = --this->m_refCount;
    if (unlikely(!refCount)) {
      /* The private release can destroy this object, so the device is read
       * before it happens rather than through `this` afterwards. */
      auto *parent = this->m_parent;
      this->ReleasePrivate();
      parent->Release();
    }
    return refCount;
  }

  template <std::size_t n> HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    return E_NOINTERFACE;
  };

  template <std::size_t n, typename V, typename... Args>
  HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    if (riid == __uuidof(V)) {
      *ppvObject = ref_and_cast<V>(this);
      return S_OK;
    }
    return ResolveBase<n + 1, Args...>(riid, ppvObject);
  };

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    auto hr = ResolveBase<0, Base...>(riid, ppvObject);
    if (SUCCEEDED(hr)) {
      return S_OK;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Resource) ||
        riid == __uuidof(typename tag::COM) ||
        riid == __uuidof(typename tag::COM_IMPL)) {
      *ppvObject = ref_and_cast<D3D11ResourceCommon>(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) ||
        riid == __uuidof(ID3D10Resource) ||
        riid == __uuidof(typename tag::D3D10)) {
      *ppvObject = ref_and_cast<typename tag::D3D10>(&d3d10);
      return S_OK;
    }

    if (riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGIResource) || riid == __uuidof(IDXGIResource1)) {
      *ppvObject = ref(dxgi_resource.get());
      return S_OK;
    }

    if (dxgi_surface && (riid == __uuidof(IDXGISurface) || riid == __uuidof(IDXGISurface1) ||
                         riid == __uuidof(IDXGISurface2))) {
      *ppvObject = ref(dxgi_surface.get());
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(typename tag::COM), riid)) {
      WARN("D3D11Resource(", tag::debug_name ,"): Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE GetDesc(void *pDesc) final {
    ::dxmt::DowngradeResourceDescription(desc, (typename tag::DESC *)pDesc);
  }

  void STDMETHODCALLTYPE GetDesc1(void *pDesc) override { *(typename tag::DESC1 *)pDesc = desc; }

  void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) final {
    *pResourceDimension = tag::dimension;
  }

  void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) final {}

  UINT STDMETHODCALLTYPE GetEvictionPriority() final { return DXGI_RESOURCE_PRIORITY_NORMAL; }

  virtual HRESULT GetDeviceInterface(REFIID riid, void **ppDevice) {
    Com<ID3D11Device> device;
    this->GetDevice(&device);
    return device->QueryInterface(riid, ppDevice);
  };

  virtual HRESULT GetDXGIUsage(DXGI_USAGE *pUsage) {
    if (!pUsage) {
      return E_INVALIDARG;
    }
    *pUsage = 0;
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    return E_INVALIDARG;
  };
  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
                            ID3D11UnorderedAccessView1 **ppView) override {
    return E_INVALIDARG;
  };
  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
                         ID3D11RenderTargetView1 **ppView) override {
    return E_INVALIDARG;
  };
  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
                         ID3D11DepthStencilView **ppView) override {
    return E_INVALIDARG;
  };

  virtual HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    return E_INVALIDARG;
  }
  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    return E_INVALIDARG;
  }

  /* IDXGISurface, through MTLDXGISurface. Only single-subresource 1D and 2D
   * textures have one. */
  HRESULT
  GetDXGISurfaceDesc(DXGI_SURFACE_DESC *pDesc) {
    if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      pDesc->Width = desc.Width;
      pDesc->Height = desc.Height;
      pDesc->Format = desc.Format;
      pDesc->SampleDesc = desc.SampleDesc;
      return S_OK;
    } else if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
      pDesc->Width = desc.Width;
      pDesc->Height = 1;
      pDesc->Format = desc.Format;
      pDesc->SampleDesc = {1, 0};
      return S_OK;
    }
    return DXGI_ERROR_INVALID_CALL;
  }

  HRESULT
  MapDXGISurface(DXGI_MAPPED_RECT *pRect, UINT Flags) {
    if (!pRect)
      return DXGI_ERROR_INVALID_CALL;
    D3D11_MAP type;
    if ((Flags & DXGI_MAP_READ) && (Flags & DXGI_MAP_WRITE))
      type = D3D11_MAP_READ_WRITE;
    else if (Flags & DXGI_MAP_READ)
      type = D3D11_MAP_READ;
    else if (Flags & DXGI_MAP_WRITE)
      type = (Flags & DXGI_MAP_DISCARD) ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE;
    else
      return DXGI_ERROR_INVALID_CALL;
    Com<ID3D11DeviceContext> context;
    this->m_parent->GetImmediateContext(&context);
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(static_cast<D3D11ResourceCommon *>(this), 0, type, 0, &mapped);
    if (FAILED(hr))
      return hr;
    pRect->Pitch = mapped.RowPitch;
    pRect->pBits = static_cast<BYTE *>(mapped.pData);
    return S_OK;
  }

  HRESULT
  UnmapDXGISurface() {
    Com<ID3D11DeviceContext> context;
    this->m_parent->GetImmediateContext(&context);
    context->Unmap(static_cast<D3D11ResourceCommon *>(this), 0);
    return S_OK;
  }

  /* GetDC hands out a 32-bit DIB of the texture -- every GDI-compatible format
   * is B8G8R8A8 or B8G8R8X8, the DIB's own layout -- and ReleaseDC uploads it
   * back. */
  HRESULT
  GetDXGISurfaceDC(BOOL Discard, HDC *phdc) {
    if (!phdc)
      return E_INVALIDARG;
    *phdc = nullptr;
    if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE) || gdi_dc_)
        return DXGI_ERROR_INVALID_CALL;
      BITMAPINFO info = {};
      info.bmiHeader.biSize = sizeof(info.bmiHeader);
      info.bmiHeader.biWidth = desc.Width;
      info.bmiHeader.biHeight = -(LONG)desc.Height; // top-down, as the texture
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void *bits = nullptr;
      HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (!bitmap)
        return E_OUTOFMEMORY;
      HDC dc = CreateCompatibleDC(nullptr);
      if (!dc) {
        DeleteObject(bitmap);
        return E_OUTOFMEMORY;
      }
      gdi_old_bitmap_ = SelectObject(dc, bitmap);
      gdi_dc_ = dc;
      gdi_bitmap_ = bitmap;
      gdi_bits_ = bits;
      if (!Discard)
        ReadIntoGdiSurface();
      *phdc = dc;
      return S_OK;
    }
    return DXGI_ERROR_INVALID_CALL;
  }

  HRESULT
  ReleaseDXGISurfaceDC(RECT *pDirtyRect) {
    if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      if (!gdi_dc_)
        return DXGI_ERROR_INVALID_CALL;
      GdiFlush();
      LONG left = 0, top = 0, right = desc.Width, bottom = desc.Height;
      if (pDirtyRect) {
        left = std::max(left, pDirtyRect->left);
        top = std::max(top, pDirtyRect->top);
        right = std::min(right, pDirtyRect->right);
        bottom = std::min(bottom, pDirtyRect->bottom);
      }
      if (left < right && top < bottom) {
        UINT pitch = desc.Width * 4;
        D3D11_BOX box = {(UINT)left, (UINT)top, 0, (UINT)right, (UINT)bottom, 1};
        Com<ID3D11DeviceContext> context;
        this->m_parent->GetImmediateContext(&context);
        context->UpdateSubresource(
            static_cast<D3D11ResourceCommon *>(this), 0, &box,
            static_cast<const char *>(gdi_bits_) + top * pitch + left * 4, pitch, 0
        );
      }
      FreeGdiSurface();
      return S_OK;
    }
    return DXGI_ERROR_INVALID_CALL;
  }

private:
  void
  ReadIntoGdiSurface() {
    if constexpr (tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      D3D11_TEXTURE2D_DESC staging_desc = {};
      staging_desc.Width = desc.Width;
      staging_desc.Height = desc.Height;
      staging_desc.MipLevels = 1;
      staging_desc.ArraySize = 1;
      staging_desc.Format = desc.Format;
      staging_desc.SampleDesc = {1, 0};
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      Com<ID3D11Texture2D> staging;
      if (FAILED(this->m_parent->CreateTexture2D(&staging_desc, nullptr, &staging)))
        return;
      Com<ID3D11DeviceContext> context;
      this->m_parent->GetImmediateContext(&context);
      context->CopyResource(staging.ptr(), static_cast<D3D11ResourceCommon *>(this));
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (FAILED(context->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &mapped)))
        return;
      for (UINT y = 0; y < desc.Height; y++)
        memcpy(static_cast<char *>(gdi_bits_) + y * desc.Width * 4,
               static_cast<const char *>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
      context->Unmap(staging.ptr(), 0);
    }
  }

  void
  FreeGdiSurface() {
    SelectObject(gdi_dc_, gdi_old_bitmap_);
    DeleteDC(gdi_dc_);
    DeleteObject(gdi_bitmap_);
    gdi_dc_ = nullptr;
    gdi_bitmap_ = nullptr;
    gdi_old_bitmap_ = nullptr;
    gdi_bits_ = nullptr;
  }

protected:
  tag::DESC1 desc;
  std::unique_ptr<IDXGIResource1> dxgi_resource;
  std::unique_ptr<MTLDXGISurface<TResourceBase<tag, Base...>>> dxgi_surface;
  HDC gdi_dc_ = nullptr;
  HBITMAP gdi_bitmap_ = nullptr;
  HGDIOBJ gdi_old_bitmap_ = nullptr;
  void *gdi_bits_ = nullptr;
  tag::D3D10_IMPL d3d10;
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11RenderTargetView>
struct tag_render_target_view {
  using COM = ID3D11RenderTargetView;
  using COM1 = ID3D11RenderTargetView1;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_RENDER_TARGET_VIEW_DESC;
  using DESC1 = D3D11_RENDER_TARGET_VIEW_DESC1;
  using D3D10 = ID3D10RenderTargetView;
  using D3D10_1 = ID3D10RenderTargetView;
  using D3D10_IMPL = MTLD3D10RenderTargetView;
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11DepthStencilView>
struct tag_depth_stencil_view {
  using COM = ID3D11DepthStencilView;
  using COM1 = ID3D11DepthStencilView;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_DEPTH_STENCIL_VIEW_DESC;
  using DESC1 = D3D11_DEPTH_STENCIL_VIEW_DESC;
  using D3D10 = ID3D10DepthStencilView;
  using D3D10_1 = ID3D10DepthStencilView;
  using D3D10_IMPL = MTLD3D10DepthStencilView;
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11ShaderResourceView>
struct tag_shader_resource_view {
  using COM = ID3D11ShaderResourceView;
  using COM1 = ID3D11ShaderResourceView1;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_SHADER_RESOURCE_VIEW_DESC;
  using DESC1 = D3D11_SHADER_RESOURCE_VIEW_DESC1;
  using D3D10 = ID3D10ShaderResourceView;
  using D3D10_1 = ID3D10ShaderResourceView1;
  using D3D10_IMPL = MTLD3D10ShaderResourceView;
};

class ImaginaryD3D10UnorderedAccessView {
public:
  ImaginaryD3D10UnorderedAccessView(IUnknown *d3d11) {}
};

template <typename RESOURCE_IMPL_ = ID3D11Resource,
          typename COM_IMPL_ = D3D11UnorderedAccessView>
struct tag_unordered_access_view {
  using COM = ID3D11UnorderedAccessView;
  using COM1 = ID3D11UnorderedAccessView1;
  using COM_IMPL = COM_IMPL_;
  using RESOURCE = ID3D11Resource;
  using RESOURCE_IMPL = RESOURCE_IMPL_;
  using DESC = D3D11_UNORDERED_ACCESS_VIEW_DESC;
  using DESC1 = D3D11_UNORDERED_ACCESS_VIEW_DESC1;
  using D3D10 = IUnknown;
  using D3D10_1 = IUnknown;
  using D3D10_IMPL = ImaginaryD3D10UnorderedAccessView;
};

template <typename tag, typename... Base>
class TResourceViewBase
    : public MTLD3D11DeviceChild<typename tag::COM_IMPL, Base...> {
public:
  TResourceViewBase(const tag::DESC1 *pDesc, tag::RESOURCE_IMPL *pResource,
                    MTLD3D11Device *device)
      : MTLD3D11DeviceChild<typename tag::COM_IMPL, Base...>(device),
        d3d10(static_cast<typename tag::COM_IMPL *>(this)) {
    if (pDesc) {
      desc = *pDesc;
    }
    this->resource_ = pResource;
    this->bind_flags_ = this->resource_->bind_flags_;
  }

  /* D3D11 device children keep their device alive while the application holds
   * one. Without it the device's reference count did not move when a resource
   * or view was created, and an application that releases the device while
   * still using a texture it made from it is entitled to expect that to work.
   *
   * The reference goes to the container, because MTLD3D11Device::AddRef
   * forwards there, and no cycle results: the context holds everything it has
   * bound through Com<T, false>, which is a private reference. */
  ULONG STDMETHODCALLTYPE AddRef() override {
    uint32_t refCount = this->m_refCount++;
    if (unlikely(!refCount)) {
      this->m_parent->AddRef();
      this->AddRefPrivate();
    }
    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    uint32_t refCount = --this->m_refCount;
    if (unlikely(!refCount)) {
      /* The private release can destroy this object, so the device is read
       * before it happens rather than through `this` afterwards. */
      auto *parent = this->m_parent;
      this->ReleasePrivate();
      parent->Release();
    }
    return refCount;
  }

  template <std::size_t n> HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    return E_NOINTERFACE;
  };

  template <std::size_t n, typename V, typename... Args>
  HRESULT ResolveBase(REFIID riid, void **ppvObject) {
    if (riid == __uuidof(V)) {
      *ppvObject = ref_and_cast<V>(this);
      return S_OK;
    }
    return ResolveBase<n + 1, Args...>(riid, ppvObject);
  };

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    auto hr = ResolveBase<0, Base...>(riid, ppvObject);
    if (SUCCEEDED(hr)) {
      return S_OK;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11View) || riid == __uuidof(typename tag::COM) ||
        riid == __uuidof(typename tag::COM1)) {
      *ppvObject = ref_and_cast<typename tag::COM_IMPL>(this);
      return S_OK;
    }

    if constexpr (!std::is_same<typename tag::D3D10_IMPL, ImaginaryD3D10UnorderedAccessView>::value) {
      if (riid == __uuidof(ID3D10DeviceChild) || riid == __uuidof(ID3D10View) ||
          riid == __uuidof(typename tag::D3D10) ||
          riid == __uuidof(typename tag::D3D10_1)) {
        *ppvObject = ref_and_cast<typename tag::D3D10_IMPL>(&d3d10);
        return S_OK;
      }
    }

    if (logQueryInterfaceError(__uuidof(typename tag::COM), riid)) {
      WARN("D3D11View: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE GetDesc(tag::DESC *pDesc) final {
    DowngradeViewDescription(desc, pDesc);
  }

  void STDMETHODCALLTYPE GetDesc1(tag::DESC1 *pDesc) /* override / final */ { *pDesc = desc; }

  void STDMETHODCALLTYPE GetResource(tag::RESOURCE **ppResource) final {
    this->resource_->QueryInterface(IID_PPV_ARGS(ppResource));
  }

protected:
  tag::DESC1 desc;
  tag::D3D10_IMPL d3d10;
};

#pragma region Resource Factory

inline void MakeUniqueSharedName(char (&out)[54]) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t r1 = dis(gen);
    uint64_t r2 = dis(gen);

    std::snprintf(out, 54, "DXMT_shared_resource_%016llx%016llx",
                  static_cast<unsigned long long>(r1),
                  static_cast<unsigned long long>(r2));
}

HRESULT
CreateStagingBuffer(MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc,
                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                    ID3D11Buffer **ppBuffer);

HRESULT
CreateStagingTexture1D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE1D_DESC *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture1D **ppTexture);

HRESULT
CreateStagingTexture2D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE2D_DESC1 *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture2D1 **ppTexture);

HRESULT
CreateStagingTexture3D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE3D_DESC1 *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture3D1 **ppTexture);

HRESULT CreateDeviceTexture1D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE1D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture1D **ppTexture);

HRESULT CreateDeviceTexture2D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE2D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture2D1 **ppTexture);

HRESULT CreateDeviceTexture3D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE3D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture3D1 **ppTexture);

HRESULT ImportSharedTexture(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture);

HRESULT ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture);

HRESULT ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
);

HRESULT
CreateBuffer(MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc,
                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                    ID3D11Buffer **ppBuffer);

HRESULT CreateDynamicLinearTexture1D(MTLD3D11Device *pDevice,
                               const D3D11_TEXTURE1D_DESC *pDesc,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture1D **ppTexture);

HRESULT CreateDynamicLinearTexture2D(MTLD3D11Device *pDevice,
                               const D3D11_TEXTURE2D_DESC1 *pDesc,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture2D1 **ppTexture);

HRESULT CreateDynamicTexture1D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE1D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture1D **ppTexture);

HRESULT CreateDynamicTexture2D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE2D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture2D1 **ppTexture);

HRESULT CreateDynamicTexture3D(MTLD3D11Device *pDevice,
                              const D3D11_TEXTURE3D_DESC1 *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture3D1 **ppTexture);
#pragma endregion

#pragma region Helper
template <typename RESOURCE_DESC, typename VIEW_DESC>
HRESULT ExtractEntireResourceViewDescription(const RESOURCE_DESC *pResourceDesc,
                                             VIEW_DESC *pViewDescOut);

template <typename RESOURCE_DESC, typename VIEW_DESC>
HRESULT ExtractEntireResourceViewDescription(const RESOURCE_DESC *pResourceDesc,
                                             const VIEW_DESC *pViewDescIn,
                                             VIEW_DESC *pViewDescOut) {
  if (pViewDescIn) {
    *pViewDescOut = *pViewDescIn;
    if constexpr (!std::is_same_v<RESOURCE_DESC, D3D11_BUFFER_DESC>) {
      if (pViewDescOut->Format == DXGI_FORMAT_UNKNOWN) {
        pViewDescOut->Format = pResourceDesc->Format;
      }
    }
    return S_OK;
  } else {
    return ExtractEntireResourceViewDescription(pResourceDesc, pViewDescOut);
  }
}

template <typename TEXTURE_DESC>
HRESULT CreateMTLTextureDescriptor(MTLD3D11Device *pDevice,
                                   const TEXTURE_DESC *pDesc,
                                   TEXTURE_DESC *pOutDesc,
                                   WMTTextureInfo *pMtlDescOut);

template <typename VIEW_DESC>
HRESULT InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture *pTexture, VIEW_DESC &ViewDesc,
    TextureViewDescriptor &Descriptor
);

template <typename VIEW_DESC>
HRESULT InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture* pTexture,
    VIEW_DESC &ViewDesc, MTL_RENDER_PASS_ATTACHMENT_DESC &AttachmentDesc, TextureViewDescriptor &Descriptor
);

template <typename TEXTURE_DESC>
void GetMipmapSize(const TEXTURE_DESC *pDesc, uint32_t level, uint32_t *pWidth,
                   uint32_t *pHeight, uint32_t *pDepth);

template <typename TEXTURE_DESC>
HRESULT GetLinearTextureLayout(MTLD3D11Device *pDevice,
                               const TEXTURE_DESC &Desc, uint32_t Level,
                               uint32_t &BytesPerRow, uint32_t &BytesPerImage,
                               uint32_t &BytesPerSlice, bool Aligned = true);

constexpr void
CalculateBufferViewOffsetAndSize(const D3D11_BUFFER_DESC &buffer_desc,
                                 uint32_t element_stride,
                                 uint32_t first_element, uint32_t num_elements,
                                 uint32_t &offset, uint32_t &size) {
  offset = first_element * element_stride;
  size = std::min(std::max(0u, buffer_desc.ByteWidth - offset),
                  element_stride * num_elements);
};
#pragma endregion

constexpr UINT kD3D11OutputBindFlags =
    D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_RENDER_TARGET;

} // namespace dxmt