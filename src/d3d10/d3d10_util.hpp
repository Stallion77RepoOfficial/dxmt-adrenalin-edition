#pragma once

#include "d3d11.h"

namespace dxmt {

void GetD3D10Device(ID3D11DeviceChild *d3d11, ID3D10Device **ppDevice);

void GetD3D10Resource(ID3D11View *d3d11, ID3D10Resource **ppResource);

UINT ConvertD3D11ResourceFlags(UINT MiscFlags);

UINT ConvertD3D10ResourceFlags(UINT MiscFlags);

/* True when the DXBC container holds a shader this API can run.
 *
 * D3D10 tops out at shader model 4.1 whatever the hardware is, and refuses
 * anything newer. d3d11 underneath does not check -- it takes the bytecode at
 * any feature level -- so without this a D3D10 application gets a shader
 * object back for a 5.0 shader where Windows gives it E_INVALIDARG. */
bool IsShaderModel4OrOlder(const void *pShaderBytecode, SIZE_T BytecodeLength);

} // namespace dxmt