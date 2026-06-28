#include "d3d9_interface.hpp"
#include "log/log.hpp"
#include "d3d9_trace.hpp"

namespace dxmt {
Logger Logger::s_instance("d3d9.log");
}

extern "C" IDirect3D9 * WINAPI
Direct3DCreate9(UINT SDKVersion) {
  D3D9_TRACE("Direct3DCreate9 sdk=", SDKVersion);
  return new dxmt::D3D9Interface();
}

extern "C" HRESULT WINAPI
Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D) {
  D3D9_TRACE("Direct3DCreate9Ex sdk=", SDKVersion);
  if (!ppD3D) return D3DERR_INVALIDCALL;
  *ppD3D = new dxmt::D3D9Interface();
  return D3D_OK;
}

// D3DPERF stubs (PIX / performance marker API)
extern "C" int    WINAPI D3DPERF_BeginEvent(D3DCOLOR, const WCHAR *) { return 0; }
extern "C" int    WINAPI D3DPERF_EndEvent()                          { return 0; }
extern "C" DWORD  WINAPI D3DPERF_GetStatus()                         { return 0; }
extern "C" WINBOOL WINAPI D3DPERF_QueryRepeatFrame()                 { return FALSE; }
extern "C" void   WINAPI D3DPERF_SetMarker(D3DCOLOR, const WCHAR *)  {}
extern "C" void   WINAPI D3DPERF_SetOptions(DWORD)                   {}
extern "C" void   WINAPI D3DPERF_SetRegion(D3DCOLOR, const WCHAR *)  {}

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls((HMODULE)reason);
  return TRUE;
}
#endif
