#include "d3d9_query.hpp"
#include "d3d9_device.hpp"
#include <algorithm>
#include <cstring>

namespace dxmt {

D3D9Query::D3D9Query(D3D9Device *pDevice, D3DQUERYTYPE Type) : D3D9DeviceChild(pDevice), type_(Type) {
  if (!IsSupportedType(Type))
    return; // leaves d3d11_query_ null -> IsValid() == false

  D3D11_QUERY_DESC qd = {};
  switch (Type) {
  case D3DQUERYTYPE_EVENT:
    qd.Query = D3D11_QUERY_EVENT;
    break;
  case D3DQUERYTYPE_TIMESTAMP:
    qd.Query = D3D11_QUERY_TIMESTAMP;
    break;
  /* Both of these read a different field of the same D3D11 query. */
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    break;
  default:
    qd.Query = D3D11_QUERY_OCCLUSION;
    break;
  }
  device_->GetD3D11Device()->CreateQuery(&qd, &d3d11_query_);
}

HRESULT STDMETHODCALLTYPE
D3D9Query::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(device_);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Query::Issue(DWORD dwIssueFlags) {
  auto device_lock = this->device_->LockDevice();
  if (!d3d11_query_) return D3DERR_INVALIDCALL;
  // D3D9 events only support End (they always span "up to now"); occlusion
  // queries use Begin/End like D3D11's do.
  auto ctx = device_->GetD3D11Context();
  if (dwIssueFlags & D3DISSUE_BEGIN) {
    if (type_ == D3DQUERYTYPE_OCCLUSION || type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT) {
      ctx->Begin(d3d11_query_.ptr());
      started_ = true;
    }
  }
  if (dwIssueFlags & D3DISSUE_END) {
    polled_ = false;
    if ((type_ == D3DQUERYTYPE_OCCLUSION || type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT) && started_) {
      ctx->End(d3d11_query_.ptr());
      started_ = false;
      issued_ = true;
    } else if (type_ == D3DQUERYTYPE_EVENT || type_ == D3DQUERYTYPE_TIMESTAMP) {
      ctx->End(d3d11_query_.ptr());
      issued_ = true;
    } else if (type_ == D3DQUERYTYPE_TIMESTAMPFREQ) {
      issued_ = true;
      /* The frequency is constant, and D3D9 applications issue this type once
       * with END alone -- there is no span to measure. The disjoint query
       * still has to be begun and ended for its Frequency to be readable. */
      ctx->Begin(d3d11_query_.ptr());
      ctx->End(d3d11_query_.ptr());
    }
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Query::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  auto device_lock = this->device_->LockDevice();
  if (!d3d11_query_) return D3DERR_INVALIDCALL;
  /* A query that has no result to give -- never issued, or still being built
   * -- succeeds with its data filled with 0xdd, as Windows does it. */
  auto unavailable = [&]() {
    if (pData) {
      memset(pData, 0, dwSize);
      memset(pData, 0xdd, std::min<DWORD>(dwSize, GetDataSize()));
    }
    return S_OK;
  };
  if (!issued_) return unavailable();

  auto ctx = device_->GetD3D11Context();
  /* Without D3DGETDATA_FLUSH the work the query waits on may never leave the
   * CPU: a driver hands its command buffer to the GPU on its own sooner or
   * later, DXMT only when told. An application spinning on such a query with
   * nothing more to draw would wait forever, so the first poll after an issue
   * submits the work whatever the flags say. */
  UINT flags = (dwGetDataFlags & D3DGETDATA_FLUSH) || !polled_ ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
  polled_ = true;
  /* The result is read whole and handed out as far as the caller asked: a
   * smaller buffer gets the low bytes and nothing past its end. */
  union {
    BOOL b;
    DWORD dw;
    UINT64 u64;
  } value = {};
  HRESULT hr;
  if (type_ == D3DQUERYTYPE_EVENT) {
    hr = ctx->GetData(d3d11_query_.ptr(), &value.b, sizeof(BOOL), flags);
  } else if (type_ == D3DQUERYTYPE_TIMESTAMP) {
    hr = ctx->GetData(d3d11_query_.ptr(), &value.u64, sizeof(UINT64), flags);
  } else if (type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT || type_ == D3DQUERYTYPE_TIMESTAMPFREQ) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    hr = ctx->GetData(d3d11_query_.ptr(), &dj, sizeof(dj), flags);
    if (type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT)
      value.b = dj.Disjoint;
    else
      value.u64 = dj.Frequency;
  } else {
    UINT64 pixels = 0;
    hr = ctx->GetData(d3d11_query_.ptr(), &pixels, sizeof(pixels), flags);
    value.dw = (DWORD)pixels;
  }
  if (hr == S_FALSE) return S_FALSE; // not ready yet
  if (FAILED(hr)) return unavailable();
  if (pData) memcpy(pData, &value, std::min<DWORD>(dwSize, GetDataSize()));
  return S_OK;
}

} // namespace dxmt
