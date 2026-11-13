#pragma once

#include "d3d9_private.h"
#include "d3d9_device_child.hpp"

namespace dxmt {

class D3D9Device;

// IDirect3DQuery9 backed by an ID3D11Query.
//
// EVENT (GPU fence sync, used pervasively for frame pacing), OCCLUSION
// (visible sample count) and the three timestamp types are backed by real
// D3D11 queries. Engines use the timestamps to scale quality to the frame
// time they are actually getting, so refusing them costs a game its adaptive
// path.
//
// D3D9 splits GPU timing across three query types where D3D11 uses two:
// TIMESTAMP is D3D11's timestamp, TIMESTAMPDISJOINT is the Disjoint field of
// D3D11's disjoint query, and TIMESTAMPFREQ is that same query's Frequency
// field. The remaining D3D9 types are driver statistics with nothing behind
// them to read.
class D3D9Query final : public D3D9DeviceChild<IDirect3DQuery9> {
public:
  D3D9Query(D3D9Device *pDevice, D3DQUERYTYPE Type);

  bool IsValid() const { return d3d11_query_ != nullptr; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice);
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() final { return type_; }
  DWORD STDMETHODCALLTYPE GetDataSize() final {
    switch (type_) {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
      return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
      return sizeof(UINT64);
    default:
      return sizeof(DWORD);
    }
  }
  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) final;
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) final;

  static bool IsSupportedType(D3DQUERYTYPE type) {
    switch (type) {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_OCCLUSION:
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
      return true;
    default:
      return false;
    }
  }

private:
  D3DQUERYTYPE type_;
  Com<ID3D11Query> d3d11_query_;
  bool started_ = false;
  bool issued_ = false; // an END has asked for a result
  bool polled_ = false; // GetData has run since the last END
};

} // namespace dxmt
