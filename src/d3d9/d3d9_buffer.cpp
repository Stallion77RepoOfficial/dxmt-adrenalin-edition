#include "d3d9_buffer.hpp"
#include "d3d9_device.hpp"

namespace dxmt {

namespace {

// Bind flag for the backing ID3D11Buffer, selected by the D3D9 interface type.
template <typename Interface> UINT BufferBindFlag();
template <> UINT BufferBindFlag<IDirect3DVertexBuffer9>() { return D3D11_BIND_VERTEX_BUFFER; }
template <> UINT BufferBindFlag<IDirect3DIndexBuffer9>()  { return D3D11_BIND_INDEX_BUFFER;  }

} // namespace

template <typename Interface, typename DescType>
D3D9Buffer<Interface, DescType>::D3D9Buffer(
    D3D9Device *pDevice, UINT Length, DWORD Usage, D3DPOOL Pool, const DescType &Desc)
    : device_(pDevice), length_(Length), desc_(Desc) {
  // DYNAMIC is only honoured for the default pool; managed/system buffers live
  // in their CPU shadow and are uploaded with UpdateSubresource.
  dynamic_ = (Usage & D3DUSAGE_DYNAMIC) && Pool == D3DPOOL_DEFAULT;

  shadow_.resize(Length, 0);

  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth      = Length;
  bd.BindFlags      = BufferBindFlag<Interface>();
  bd.MiscFlags      = 0;
  bd.StructureByteStride = 0;
  if (dynamic_) {
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  } else {
    bd.Usage          = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
  }

  device_->GetD3D11Device()->CreateBuffer(&bd, nullptr, &buffer_);
}

template <typename Interface, typename DescType>
HRESULT STDMETHODCALLTYPE
D3D9Buffer<Interface, DescType>::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(device_);
  return D3D_OK;
}

template <typename Interface, typename DescType>
HRESULT STDMETHODCALLTYPE
D3D9Buffer<Interface, DescType>::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  if (!ppbData) return D3DERR_INVALIDCALL;
  if (locked_)  return D3DERR_INVALIDCALL;

  // A size of 0 means "lock the entire buffer from OffsetToLock".
  if (SizeToLock == 0)
    SizeToLock = length_ - OffsetToLock;
  if (OffsetToLock > length_ || SizeToLock > length_ - OffsetToLock)
    return D3DERR_INVALIDCALL;

  lock_offset_ = OffsetToLock;
  lock_size_   = SizeToLock;
  lock_flags_  = Flags;
  locked_      = true;

  *ppbData = shadow_.data() + OffsetToLock;
  return D3D_OK;
}

template <typename Interface, typename DescType>
HRESULT STDMETHODCALLTYPE
D3D9Buffer<Interface, DescType>::Unlock() {
  if (!locked_) return D3DERR_INVALIDCALL;
  locked_ = false;

  // Read-only locks never modify the buffer, so skip the upload entirely.
  if (lock_flags_ & D3DLOCK_READONLY)
    return D3D_OK;

  auto *ctx = device_->GetD3D11Context();

  if (dynamic_) {
    // DISCARD / NO_OVERWRITE map straight onto the D3D11 dynamic-buffer hints.
    // Without either hint D3D9 still allows the write, so default to DISCARD to
    // avoid stalling on in-flight GPU reads.
    D3D11_MAP map = D3D11_MAP_WRITE_DISCARD;
    if (lock_flags_ & D3DLOCK_NOOVERWRITE)
      map = D3D11_MAP_WRITE_NO_OVERWRITE;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx->Map(buffer_.ptr(), 0, map, 0, &mapped))) {
      // WRITE_DISCARD requires the whole buffer to be rewritten; copy the full
      // shadow so untouched bytes remain valid after the discard.
      if (map == D3D11_MAP_WRITE_DISCARD)
        memcpy(mapped.pData, shadow_.data(), length_);
      else
        memcpy(static_cast<uint8_t *>(mapped.pData) + lock_offset_,
               shadow_.data() + lock_offset_, lock_size_);
      ctx->Unmap(buffer_.ptr(), 0);
    }
  } else {
    // Push just the dirtied range to the default-usage buffer.
    D3D11_BOX box = {};
    box.left   = lock_offset_;
    box.right  = lock_offset_ + lock_size_;
    box.top    = 0; box.bottom = 1;
    box.front  = 0; box.back   = 1;
    ctx->UpdateSubresource(buffer_.ptr(), 0, &box,
                           shadow_.data() + lock_offset_, 0, 0);
  }
  return D3D_OK;
}

// Explicit instantiations for the two concrete buffer kinds.
template class D3D9Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>;
template class D3D9Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>;

} // namespace dxmt
