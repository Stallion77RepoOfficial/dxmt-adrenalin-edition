/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_format.hpp"
#include "d3d11_resource.hpp"

namespace dxmt {

struct BufferViewInfo {
  BufferViewKey viewKey;
  uint32_t viewElementOffset;
  uint32_t viewElementWidth;
  uint32_t byteOffset;
  uint32_t byteWidth;
};

class D3D11Buffer : public TResourceBase<tag_buffer> {
private:
#ifdef DXMT_DEBUG
  std::string debug_name;
#endif
  bool structured;
  bool allow_raw_view;

  Rc<DynamicBuffer> dynamic_;

  using SRVBase = TResourceViewBase<tag_shader_resource_view<D3D11Buffer>>;

  class TBufferSRV : public SRVBase {

  public:
    TBufferSRV(
        const tag_shader_resource_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info
    ) :
        SRVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }

    ~TBufferSRV() {}
  };

  using RTVBase = TResourceViewBase<tag_render_target_view<D3D11Buffer>>;

  /* A render target view of a buffer.
   *
   * D3D11 allows it; Metal has no notion of rendering into a buffer. What it
   * does have is a texture created over a buffer's memory, and on Apple
   * silicon such a linear texture can carry render target usage -- verified
   * directly before this was written, by clearing one and reading the colour
   * back out of the buffer.
   *
   * So the view owns a one-row texture aliasing the application's buffer. It
   * owns it because nothing else does: a texture resource hands out views of
   * the texture it already has, while this one is made for the view and dies
   * with it. */
  class TBufferRTV : public RTVBase {
    Rc<Texture> linear_;

  public:
    TBufferRTV(
        const tag_render_target_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        Rc<Texture> &&linear, WMTPixelFormat view_format, const MTL_RENDER_PASS_ATTACHMENT_DESC &attachment
    ) :
        RTVBase(pDesc, pResource, pDevice),
        linear_(std::move(linear)) {
      this->texture_ = linear_.ptr();
      this->view_id_ = this->texture_->createView({
          .format = view_format,
          .type = WMTTextureType2D,
          .firstMiplevel = 0,
          .miplevelCount = 1,
          .firstArraySlice = 0,
          .arraySize = 1,
      });
      this->format_ = view_format;
      this->pass_desc_ = attachment;
    }
  };

  using UAVBase = TResourceViewBase<tag_unordered_access_view<D3D11Buffer>>;

  class UAVWithCounter : public UAVBase {
  public:
    UAVWithCounter(
        const tag_unordered_access_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info, Rc<Counter>&& counter
    ) :
        UAVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      counter_ = std::move(counter);
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }
  };

public:
  /* D3D12 permits a buffer to be used as vertex/index/constant storage. Its
   * D3D11 views must share one allocation rather than upload separate copies. */
  D3D11Buffer(const tag_buffer::DESC1 *pDesc, Rc<Buffer> &&backing, MTLD3D11Device *device) :
      TResourceBase<tag_buffer>(*pDesc, device),
      structured(pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED),
      allow_raw_view(pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) {
    buffer_ = std::move(backing);
  }

  D3D11Buffer(const tag_buffer::DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, MTLD3D11Device *device) :
      TResourceBase<tag_buffer>(*pDesc, device) {
    buffer_ = new Buffer(pDesc->ByteWidth, device->GetMTLDevice());
    Flags<BufferAllocationFlag> flags;
    if (!m_parent->IsTraced() && pDesc->Usage == D3D11_USAGE_DYNAMIC)
      flags.set(BufferAllocationFlag::CpuWriteCombined);
    // if (pDesc->Usage != D3D11_USAGE_DEFAULT)
    if (pDesc->Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(BufferAllocationFlag::GpuReadonly);
    if (pDesc->Usage != D3D11_USAGE_DYNAMIC) {
      if (pInitialData)
        flags.set(BufferAllocationFlag::GpuManaged);
      else if (pDesc->BindFlags & kD3D11OutputBindFlags)
        flags.set(BufferAllocationFlag::GpuPrivate);
      else
        flags.set(BufferAllocationFlag::GpuManaged);
    } else {
      flags.set(BufferAllocationFlag::SuballocateFromOnePage);
#ifdef __i386__
      flags.set(BufferAllocationFlag::CpuPlaced);
#endif
    }
    auto allocation = buffer_->allocate(flags);
    auto &initializer = device->GetDXMTDevice().queue().initializer;
    if (pInitialData) {
      allocation->updateContents(0, pInitialData->pSysMem, pDesc->ByteWidth);
    } else if (flags.test(BufferAllocationFlag::GpuPrivate)) {
      initializer.initWithZero(allocation.ptr(), 0, buffer_->length());
    }
    auto _ = buffer_->rename(std::move(allocation));
    D3D11_ASSERT(_.ptr() == nullptr);
    structured = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    allow_raw_view = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (!(desc.BindFlags & kD3D11OutputBindFlags)) {
      dynamic_ = new DynamicBuffer(buffer_.ptr(), flags);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    return TResourceBase<tag_buffer>::QueryInterface(riid, ppvObject);
  }

  Rc<StagingResource>
  staging(UINT Subresource) final {
    return nullptr;
  }
  Rc<DynamicBuffer>
  dynamicBuffer(UINT *pBufferLength, UINT *pBindFlags) override {
    *pBufferLength = desc.ByteWidth;
    *pBindFlags = desc.BindFlags;
    return dynamic_;
  }
  Rc<DynamicLinearTexture>
  dynamicLinearTexture(UINT *, UINT *) final {
    return {};
  };
  Rc<DynamicBuffer>
  dynamicTexture(UINT , UINT *, UINT *) final {
    return {};
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc = {};
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc, &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER &&
        finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) {
      return E_FAIL;
    }
    /* A view of no elements is no view (Buffer and BufferEx share the field). */
    if (!finalDesc.Buffer.NumElements)
      return E_INVALIDARG;
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
    } else if (finalDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX && finalDesc.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel) {
        ERR("D3D11Buffer::CreateShaderResourceView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }

      view_format = format.PixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({.format = view_format});

    auto srv = ref(new TBufferSRV(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        }
    ));
    *ppView = srv;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc, ID3D11RenderTargetView1 **ppView) override {
    /* No "entire resource" form here, unlike a texture: a buffer carries no
     * format for one to be inferred from, so the view has to say what it is. */
    if (!pDesc)
      return E_INVALIDARG;
    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc = *pDesc;
    if (finalDesc.ViewDimension != D3D11_RTV_DIMENSION_BUFFER)
      return E_INVALIDARG;
    if (!(this->desc.BindFlags & D3D11_BIND_RENDER_TARGET))
      return E_INVALIDARG;
    /* A buffer render target has no structure to infer a format from, so the
     * view must name one, and it has to be an ordinary sized format. */
    MTL_DXGI_FORMAT_DESC format;
    if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format)) || !format.BytesPerTexel)
      return E_INVALIDARG;

    uint32_t offset, size;
    CalculateBufferViewOffsetAndSize(
        this->desc, format.BytesPerTexel, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
    );
    uint32_t elements = size / format.BytesPerTexel;
    if (!elements)
      return E_INVALIDARG;

    if (!ppView)
      return S_FALSE;

    /* One row, as many texels wide as the view covers. Rendering needs the
     * render target usage; shader read comes along because a render target is
     * routinely sampled afterwards and Metal wants it declared up front. */
    WMTTextureInfo info{};
    info.type = WMTTextureType2D;
    info.pixel_format = format.PixelFormat;
    info.width = elements;
    info.height = 1;
    info.depth = 1;
    info.mipmap_level_count = 1;
    info.array_length = 1;
    info.sample_count = 1;
    info.usage = WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead;

    uint32_t bytes_per_row = elements * format.BytesPerTexel;
    Rc<Texture> linear = new Texture(bytes_per_row, bytes_per_row, info, m_parent->GetMTLDevice());
    auto *allocation = buffer_->current();
    if (!allocation)
      return E_FAIL;
    auto over = linear->allocateOver(allocation->buffer(), nullptr, offset, {});
    if (!over.ptr())
      return E_FAIL;
    auto _ = linear->rename(std::move(over));

    MTL_RENDER_PASS_ATTACHMENT_DESC attachment{};
    attachment.RenderTargetArrayLength = 0;
    attachment.SampleCount = 1;
    attachment.DepthPlane = 0;
    attachment.Width = elements;
    attachment.Height = 1;

    *ppView = ref(new TBufferRTV(&finalDesc, this, m_parent, std::move(linear), format.PixelFormat, attachment));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc, ID3D11UnorderedAccessView1 **ppView)
      override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc = {};
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc, &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) {
      return E_FAIL;
    }
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    Rc<Counter> counter = {};
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      // when structured buffer is interpreted as typed buffer for any reason
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
      if (finalDesc.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) {
        counter = new Counter();
      }
    } else if (finalDesc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel) {
        ERR("D3D11Buffer::CreateUnorderedAccessView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }

      view_format = format.PixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({.format = view_format});

    auto srv = ref(new UAVWithCounter(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        },
        std::move(counter)
    ));
    *ppView = srv;
    return S_OK;
  };

  void
  OnSetDebugObjectName(LPCSTR Name) override {
    if (!Name) {
      return;
    }
#ifdef DXMT_DEBUG
    debug_name = std::string(Name);
#endif
  }
};

HRESULT
CreateBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer
) {
  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(pDesc, pInitialData, pDevice)));
  return S_OK;
}

/* Private DXMT interop export. The resource supplied by our D3D12 device is
 * already a DXMT D3D11 buffer; only a COM view is created, with a valid D3D11
 * binding description. Ordinary D3D11 CreateBuffer keeps its allocation path. */
extern "C" HRESULT WINAPI
DXMTCreateBufferAlias(ID3D11Buffer *backing, const D3D11_BUFFER_DESC *desc, ID3D11Buffer **out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!backing || !desc || !desc->ByteWidth || desc->Usage != D3D11_USAGE_DEFAULT)
    return E_INVALIDARG;
  D3D11_BUFFER_DESC original = {};
  backing->GetDesc(&original);
  if (desc->ByteWidth > original.ByteWidth || original.Usage != D3D11_USAGE_DEFAULT)
    return E_INVALIDARG;
  auto allocation = GetResourceCommon(backing)->buffer();
  if (!allocation) return E_INVALIDARG;
  ID3D11Device *device = nullptr;
  backing->GetDevice(&device);
  if (!device) return E_INVALIDARG;
  *out = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(desc, std::move(allocation),
                                                            static_cast<MTLD3D11Device *>(device))));
  device->Release();
  return S_OK;
}

/* D3D12 CPU-visible allocations cannot inherit a private DEFAULT buffer's
 * storage. In WoW64 the backing must also lie in the 32-bit address space. */
extern "C" HRESULT WINAPI
DXMTCreateBufferFromAllocation(ID3D11Device *device, const D3D11_BUFFER_DESC *desc, Buffer *backing,
                               ID3D11Buffer **out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!device || !desc || !backing || !backing->current() || !desc->ByteWidth ||
      desc->ByteWidth > backing->length() || desc->Usage != D3D11_USAGE_DEFAULT) return E_INVALIDARG;
  *out = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(desc, Rc<Buffer>(backing),
                                                            static_cast<MTLD3D11Device *>(device))));
  return S_OK;
}

extern "C" HRESULT WINAPI
DXMTWaitForSubmission(ID3D11Device *device, UINT64 sequence) {
  if (!device) return E_POINTER;
  auto &queue = static_cast<MTLD3D11Device *>(device)->GetDXMTDevice().queue();
  // Keep the atomic wait and notification in this module: static C++ runtime
  // copies in another DLL have separate atomic-wait notification tables.
  queue.WaitCPUFence(sequence);
  return queue.IsDeviceWedged() ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
}

extern "C" HRESULT WINAPI
DXMTCreateSharedBuffer(ID3D11Device *device, const D3D11_BUFFER_DESC *desc, ID3D11Buffer **out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!device || !desc || !desc->ByteWidth || desc->Usage != D3D11_USAGE_DEFAULT) return E_INVALIDARG;
  auto *owner = static_cast<MTLD3D11Device *>(device);
  Rc<Buffer> backing = new Buffer(desc->ByteWidth, owner->GetMTLDevice());
  Flags<BufferAllocationFlag> flags;
#ifdef __i386__
  flags.set(BufferAllocationFlag::CpuPlaced);
#endif
  auto allocation = backing->allocate(flags);
  if (!allocation || !allocation->buffer()) return E_OUTOFMEMORY;
  backing->rename(std::move(allocation));
  *out = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(desc, std::move(backing), owner)));
  return S_OK;
}

} // namespace dxmt
