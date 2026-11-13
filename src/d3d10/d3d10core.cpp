
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d11.h"
#include "log/log.hpp"

#include <cstring>

namespace dxmt {
Logger Logger::s_instance("d3d10core.log");

using PFNCore11Create = HRESULT(WINAPI *)(
    IDXGIFactory *pFactory, IDXGIAdapter *pAdapter, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
    UINT FeatureLevels, ID3D11Device **ppDevice
);

static void decodeName(char *dst, const unsigned char *src, size_t len) {
  constexpr unsigned char key = 0x5a;
  for (size_t i = 0; i < len; i++)
    dst[i] = char(src[i] ^ key);
  dst[len] = 0;
}

static PFNCore11Create resolveCore11Create() {
  static PFNCore11Create proc = []() -> PFNCore11Create {
    HMODULE self = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&resolveCore11Create), &self)) {
      char path[MAX_PATH] = {};
      if (GetModuleFileNameA(self, path, sizeof(path))) {
        char *slash = std::strrchr(path, '\\');
        char *fwd_slash = std::strrchr(path, '/');
        char *sep = slash > fwd_slash ? slash : fwd_slash;
        if (sep) {
          static const unsigned char dll[] = {
            0x3e, 0x69, 0x3e, 0x6b, 0x6b, 0x05, 0x3e,
            0x22, 0x37, 0x2e, 0x74, 0x3e, 0x36, 0x36,
          };
          decodeName(sep + 1, dll, sizeof(dll));

          HMODULE core = LoadLibraryA(path);
          if (!core) {
            // No renamed copy next to us - fall back to the regular d3d11 in
            // the same directory (complete-set deployments ship only d3d11.dll).
            static const unsigned char dll_plain[] = {
              0x3e, 0x69, 0x3e, 0x6b, 0x6b, 0x74, 0x3e, 0x36, 0x36,
            };
            decodeName(sep + 1, dll_plain, sizeof(dll_plain));
            core = LoadLibraryA(path);
          }
          if (!core)
            return nullptr;

          static const unsigned char fn[] = {
            0x1e, 0x69, 0x1e, 0x6b, 0x6b, 0x19, 0x35,
            0x28, 0x3f, 0x19, 0x28, 0x3f, 0x3b, 0x2e,
            0x3f, 0x1e, 0x3f, 0x2c, 0x33, 0x39, 0x3f,
          };
          char proc_name[sizeof(fn) + 1];
          decodeName(proc_name, fn, sizeof(fn));

          return reinterpret_cast<PFNCore11Create>(GetProcAddress(core, proc_name));
        }
      }
    }

    return nullptr;
  }();
  return proc;
}

extern "C" HRESULT STDMETHODCALLTYPE
D3D10CoreCreateDevice(
    IDXGIFactory *pFactory, IDXGIAdapter *pAdapter, UINT Flags, D3D_FEATURE_LEVEL FeatureLevel, ID3D10Device **ppDevice
) {
  InitReturnPtr(ppDevice);

  Com<ID3D11Device> d3d11_device;

  HRESULT hr = pAdapter->CheckInterfaceSupport(__uuidof(ID3D10Device), nullptr);

  if (FAILED(hr)) {
    ERR("D3D10CoreCreateDevice: adapter does not support D3D10: ", hr);
    return hr;
  }

  auto coreCreate = resolveCore11Create();
  if (!coreCreate) {
    ERR("D3D10CoreCreateDevice: core runtime unavailable");
    return E_FAIL;
  }

  hr = coreCreate(pFactory, pAdapter, Flags | 0x80000000 /* DXMT_D3D10_DEVICE */, &FeatureLevel, 1, &d3d11_device);

  if (FAILED(hr)) {
    ERR("D3D10CoreCreateDevice: device creation failed: ", hr);
    return hr;
  }

  Com<ID3D10Multithread> multithread;
  if (SUCCEEDED(d3d11_device->QueryInterface(IID_PPV_ARGS(&multithread))))
    multithread->SetMultithreadProtected(!(Flags & D3D10_CREATE_DEVICE_SINGLETHREADED));

  hr = d3d11_device->QueryInterface(IID_PPV_ARGS(ppDevice));
  if (FAILED(hr))
    ERR("D3D10CoreCreateDevice: no D3D10 device interface: ", hr);
  return hr;
}

extern "C" HRESULT STDMETHODCALLTYPE
D3D10CoreRegisterLayers() {
  ERR("D3D10CoreRegisterLayers: stub");
  return E_NOTIMPL;
}
} // namespace dxmt
