#pragma once

#include "com/com_guid.hpp"
#include "d3d11.h"

/* DXMT's D3D12 records through a d3d11 device of its own, and
 * D3D11On12CreateDevice hands out that same device: both APIs then encode into
 * one command stream, in call order. d3d11.dll reaches that device, and the
 * d3d11 resource behind a D3D12 resource, through these. */
DEFINE_COM_INTERFACE("a389c030-4583-4c1f-8faa-66e8f2fa8c75", IMTLD3D12DeviceInterop) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetD3D11Device(ID3D11Device **ppDevice) = 0;
};

DEFINE_COM_INTERFACE("f0c40ddc-6579-4502-917a-37cdf723fcb7", IMTLD3D12ResourceInterop) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetD3D11Resource(ID3D11Resource **ppResource) = 0;
};
