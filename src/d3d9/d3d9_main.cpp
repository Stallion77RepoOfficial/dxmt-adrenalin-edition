#include "d3d9_interface.hpp"
#include "log/log.hpp"
#include <atomic>

namespace dxmt {
Logger Logger::s_instance("d3d9.log");
}

extern "C" IDirect3D9 * WINAPI
Direct3DCreate9(UINT SDKVersion) {
  return new dxmt::D3D9Interface(false);
}

extern "C" HRESULT WINAPI
Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D) {
  if (!ppD3D) return D3DERR_INVALIDCALL;
  *ppD3D = new dxmt::D3D9Interface(true);
  return D3D_OK;
}

// D3DPERF stubs (PIX / performance marker API). The event calls still keep
// the nesting level they return: the zero-based level an event begins or ends
// at, negative for an end without a begin.
static std::atomic<int> perf_event_level{0};

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR, const WCHAR *) { return perf_event_level++; }
extern "C" int WINAPI D3DPERF_EndEvent() {
  int level = perf_event_level.load();
  while (level > 0 && !perf_event_level.compare_exchange_weak(level, level - 1))
    ;
  return level > 0 ? level - 1 : -1;
}
extern "C" DWORD  WINAPI D3DPERF_GetStatus()                         { return 0; }
extern "C" WINBOOL WINAPI D3DPERF_QueryRepeatFrame()                 { return FALSE; }
extern "C" void   WINAPI D3DPERF_SetMarker(D3DCOLOR, const WCHAR *)  {}
extern "C" void   WINAPI D3DPERF_SetOptions(DWORD)                   {}
extern "C" void   WINAPI D3DPERF_SetRegion(D3DCOLOR, const WCHAR *)  {}

extern "C" void WINAPI DebugSetMute(void) {}

namespace {

using ShaderValidatorCallback = HRESULT(WINAPI *)(const char *file, int line, DWORD_PTR arg3, DWORD_PTR message_id,
                                                  const char *message, void *context);

/* IDirect3DShaderValidator9, which d3dx and some applications fetch to check
 * bytecode before creating anything from it. There is no header for it: it is
 * IUnknown followed by Begin, Instruction and End. Everything passes -- what
 * the translator cannot handle it reports when the shader is created, which
 * is where an application finds out on Windows too. */
class ShaderValidator final {
public:
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) {
    if (!out)
      return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
      AddRef();
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  virtual ULONG STDMETHODCALLTYPE AddRef() { return ++refcount_; }
  virtual ULONG STDMETHODCALLTYPE Release() {
    ULONG refcount = --refcount_;
    if (!refcount)
      delete this;
    return refcount;
  }
  virtual HRESULT STDMETHODCALLTYPE Begin(ShaderValidatorCallback, void *, DWORD_PTR) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Instruction(const char *, int, const DWORD *, unsigned int) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE End() { return S_OK; }

private:
  std::atomic<ULONG> refcount_{1};
};

} // namespace

extern "C" void *WINAPI Direct3DShaderValidatorCreate9(void) { return new ShaderValidator(); }

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(instance);
  return TRUE;
}
#endif
