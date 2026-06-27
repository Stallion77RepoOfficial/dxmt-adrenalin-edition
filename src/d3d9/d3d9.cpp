#include "d3d9_main.hpp"

struct IDirect3D9Ex;

extern "C" {

IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion) {
    return new dxmt::d3d9::D3D9Main();
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D) {
    if (!ppD3D) return E_POINTER;
    *ppD3D = nullptr;
    return E_NOTIMPL;
}

int WINAPI D3DPERF_BeginEvent(DWORD col, LPCWSTR wszName) { return 0; }
int WINAPI D3DPERF_EndEvent()                              { return 0; }
void WINAPI D3DPERF_SetMarker(DWORD col, LPCWSTR wszName) {}
void WINAPI D3DPERF_SetRegion(DWORD col, LPCWSTR wszName) {}
BOOL WINAPI D3DPERF_QueryRepeatFrame()                     { return FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD dwOptions)            {}
DWORD WINAPI D3DPERF_GetStatus()                           { return 0; }

} // extern "C"
