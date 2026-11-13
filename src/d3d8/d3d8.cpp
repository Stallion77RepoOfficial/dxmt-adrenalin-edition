#include <objbase.h>
#include <stdlib.h>
#include <windows.h>
#include <d3d9.h>
#include <initguid.h>

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <memory>
#include <unordered_map>
#include <vector>

namespace d3d8 {
#include "d3d8.h"
}

namespace dxmt {
namespace {

using Direct3DCreate9Proc = IDirect3D9 *(WINAPI *)(UINT);

template <typename To, typename From>
To enum_cast(From value) {
  return static_cast<To>(static_cast<DWORD>(value));
}

template <typename To, typename From>
To struct_cast(const From *value) {
  return value ? *reinterpret_cast<const To *>(value) : To();
}

static UINT format_block_size(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8:
  case D3DFMT_P8:
  case D3DFMT_L8:
    return 1;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A4R4G4B4:
  case D3DFMT_A8L8:
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
    return 2;
  default:
    return 4;
  }
}

static UINT surface_size(UINT width, UINT height, D3DFORMAT format) {
  if (format == D3DFMT_DXT1)
    return ((width + 3) / 4) * ((height + 3) / 4) * 8;
  if (format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
      format == D3DFMT_DXT4 || format == D3DFMT_DXT5)
    return ((width + 3) / 4) * ((height + 3) / 4) * 16;
  return width * height * format_block_size(format);
}

static void convert_present_params(const d3d8::D3DPRESENT_PARAMETERS &in, D3DPRESENT_PARAMETERS &out) {
  std::memset(&out, 0, sizeof(out));
  out.BackBufferWidth            = in.BackBufferWidth;
  out.BackBufferHeight           = in.BackBufferHeight;
  out.BackBufferFormat           = enum_cast<D3DFORMAT>(in.BackBufferFormat);
  out.BackBufferCount            = in.BackBufferCount;
  out.MultiSampleType            = enum_cast<D3DMULTISAMPLE_TYPE>(in.MultiSampleType);
  out.MultiSampleQuality         = 0;
  out.SwapEffect                 = enum_cast<D3DSWAPEFFECT>(in.SwapEffect);
  out.hDeviceWindow              = in.hDeviceWindow;
  out.Windowed                   = in.Windowed;
  out.EnableAutoDepthStencil     = in.EnableAutoDepthStencil;
  out.AutoDepthStencilFormat     = enum_cast<D3DFORMAT>(in.AutoDepthStencilFormat);
  out.Flags                      = in.Flags;
  out.FullScreen_RefreshRateInHz = in.FullScreen_RefreshRateInHz;
  out.PresentationInterval       = in.FullScreen_PresentationInterval;
}

static void convert_present_params(const D3DPRESENT_PARAMETERS &in, d3d8::D3DPRESENT_PARAMETERS &out) {
  std::memset(&out, 0, sizeof(out));
  out.BackBufferWidth                    = in.BackBufferWidth;
  out.BackBufferHeight                   = in.BackBufferHeight;
  out.BackBufferFormat                   = enum_cast<d3d8::D3DFORMAT>(in.BackBufferFormat);
  out.BackBufferCount                    = in.BackBufferCount;
  out.MultiSampleType                    = enum_cast<d3d8::D3DMULTISAMPLE_TYPE>(in.MultiSampleType);
  out.SwapEffect                         = enum_cast<d3d8::D3DSWAPEFFECT>(in.SwapEffect);
  out.hDeviceWindow                      = in.hDeviceWindow;
  out.Windowed                           = in.Windowed;
  out.EnableAutoDepthStencil             = in.EnableAutoDepthStencil;
  out.AutoDepthStencilFormat             = enum_cast<d3d8::D3DFORMAT>(in.AutoDepthStencilFormat);
  out.Flags                              = in.Flags;
  out.FullScreen_RefreshRateInHz         = in.FullScreen_RefreshRateInHz;
  out.FullScreen_PresentationInterval    = in.PresentationInterval;
}

static void convert_surface_desc(const D3DSURFACE_DESC &in, d3d8::D3DSURFACE_DESC &out) {
  out.Format          = enum_cast<d3d8::D3DFORMAT>(in.Format);
  out.Type            = enum_cast<d3d8::D3DRESOURCETYPE>(in.Type);
  out.Usage           = in.Usage;
  out.Pool            = enum_cast<d3d8::D3DPOOL>(in.Pool);
  out.Size            = surface_size(in.Width, in.Height, in.Format);
  out.MultiSampleType = enum_cast<d3d8::D3DMULTISAMPLE_TYPE>(in.MultiSampleType);
  out.Width           = in.Width;
  out.Height          = in.Height;
}

static void convert_volume_desc(const D3DVOLUME_DESC &in, d3d8::D3DVOLUME_DESC &out) {
  out.Format = enum_cast<d3d8::D3DFORMAT>(in.Format);
  out.Type   = enum_cast<d3d8::D3DRESOURCETYPE>(in.Type);
  out.Usage  = in.Usage;
  out.Pool   = enum_cast<d3d8::D3DPOOL>(in.Pool);
  out.Size   = surface_size(in.Width, in.Height, in.Format) * in.Depth;
  out.Width  = in.Width;
  out.Height = in.Height;
  out.Depth  = in.Depth;
}

static void convert_caps(const D3DCAPS9 &in, d3d8::D3DCAPS8 &out) {
  std::memset(&out, 0, sizeof(out));
  out.DeviceType                 = enum_cast<d3d8::D3DDEVTYPE>(in.DeviceType);
  out.AdapterOrdinal             = in.AdapterOrdinal;
  out.Caps                       = in.Caps;
  out.Caps2                      = in.Caps2;
  out.Caps3                      = in.Caps3;
  out.PresentationIntervals      = in.PresentationIntervals;
  out.CursorCaps                 = in.CursorCaps;
  out.DevCaps                    = in.DevCaps;
  out.PrimitiveMiscCaps          = in.PrimitiveMiscCaps;
  out.RasterCaps                 = in.RasterCaps;
  out.ZCmpCaps                   = in.ZCmpCaps;
  out.SrcBlendCaps               = in.SrcBlendCaps;
  out.DestBlendCaps              = in.DestBlendCaps;
  out.AlphaCmpCaps               = in.AlphaCmpCaps;
  out.ShadeCaps                  = in.ShadeCaps;
  out.TextureCaps                = in.TextureCaps;
  out.TextureFilterCaps          = in.TextureFilterCaps;
  out.CubeTextureFilterCaps      = in.CubeTextureFilterCaps;
  out.VolumeTextureFilterCaps    = in.VolumeTextureFilterCaps;
  out.TextureAddressCaps         = in.TextureAddressCaps;
  out.VolumeTextureAddressCaps   = in.VolumeTextureAddressCaps;
  out.LineCaps                   = in.LineCaps;
  out.MaxTextureWidth            = in.MaxTextureWidth;
  out.MaxTextureHeight           = in.MaxTextureHeight;
  out.MaxVolumeExtent            = in.MaxVolumeExtent;
  out.MaxTextureRepeat           = in.MaxTextureRepeat;
  out.MaxTextureAspectRatio      = in.MaxTextureAspectRatio;
  out.MaxAnisotropy              = in.MaxAnisotropy;
  out.MaxVertexW                 = in.MaxVertexW;
  out.GuardBandLeft              = in.GuardBandLeft;
  out.GuardBandTop               = in.GuardBandTop;
  out.GuardBandRight             = in.GuardBandRight;
  out.GuardBandBottom            = in.GuardBandBottom;
  out.ExtentsAdjust              = in.ExtentsAdjust;
  out.StencilCaps                = in.StencilCaps;
  out.FVFCaps                    = in.FVFCaps;
  out.TextureOpCaps              = in.TextureOpCaps;
  out.MaxTextureBlendStages      = in.MaxTextureBlendStages;
  out.MaxSimultaneousTextures    = in.MaxSimultaneousTextures;
  out.VertexProcessingCaps       = in.VertexProcessingCaps;
  out.MaxActiveLights            = in.MaxActiveLights;
  out.MaxUserClipPlanes          = in.MaxUserClipPlanes;
  out.MaxVertexBlendMatrices     = in.MaxVertexBlendMatrices;
  out.MaxVertexBlendMatrixIndex  = in.MaxVertexBlendMatrixIndex;
  out.MaxPointSize               = in.MaxPointSize;
  out.MaxPrimitiveCount          = in.MaxPrimitiveCount;
  out.MaxVertexIndex             = in.MaxVertexIndex;
  out.MaxStreams                 = in.MaxStreams;
  out.MaxStreamStride            = in.MaxStreamStride;
  /* Clamped to what D3D8 has, not passed through. The layer underneath is
   * D3D9 and reports shader model 3.0; D3D8 stops at vs_1_1 and ps_1_4, and
   * its runtime never reported anything higher, so no D3D8 application is
   * written to cope with it. Splinter Cell reads these to choose a rendering
   * path and faulted on a null taking a branch that does not exist in its
   * D3D8 code. The translator still handles everything a D3D8 shader can
   * contain -- this only changes what the application is told it may use. */
  out.VertexShaderVersion        = std::min<DWORD>(in.VertexShaderVersion, D3DVS_VERSION(1, 1));
  out.MaxVertexShaderConst       = in.MaxVertexShaderConst;
  out.PixelShaderVersion         = std::min<DWORD>(in.PixelShaderVersion, D3DPS_VERSION(1, 4));
  /* ps_1_4 registers hold -8..8, and the D3D9 caps carry that same number in
   * PixelShader1xMaxValue. Reporting 1.0 understated the range a shader may
   * use by a factor of eight. */
  out.MaxPixelShaderValue        = in.PixelShader1xMaxValue > 0.0f ? in.PixelShader1xMaxValue : 8.0f;
}

static HRESULT copy_dword_blob(const std::vector<DWORD> &src, void *data, DWORD *size) {
  if (!size)
    return D3DERR_INVALIDCALL;

  DWORD needed = static_cast<DWORD>(src.size() * sizeof(DWORD));
  if (!data) {
    *size = needed;
    return D3D_OK;
  }

  if (*size < needed) {
    *size = needed;
    return D3DERR_MOREDATA;
  }

  std::memcpy(data, src.data(), needed);
  *size = needed;
  return D3D_OK;
}

static std::vector<DWORD> copy_shader_tokens(const DWORD *tokens) {
  std::vector<DWORD> out;
  if (!tokens)
    return out;

  out.push_back(tokens[0]);
  for (UINT i = 1; i < 0x100000; i++) {
    out.push_back(tokens[i]);
    if (tokens[i] == 0x0000ffffu)
      break;
  }
  return out;
}

static std::vector<DWORD> copy_decl_tokens(const DWORD *tokens) {
  std::vector<DWORD> out;
  if (!tokens)
    return out;

  for (UINT i = 0; i < 0x10000; i++) {
    out.push_back(tokens[i]);
    if (tokens[i] == 0xffffffffu)
      break;
  }
  return out;
}

static UINT decl_type_size(BYTE type) {
  static const UINT sizes[] = {4, 8, 12, 16, 4, 4, 4, 8};
  return type < ARRAYSIZE(sizes) ? sizes[type] : 0;
}

static bool map_decl_register(DWORD reg, BYTE *usage, BYTE *usage_index) {
  switch (reg) {
  case d3d8::D3DVSDE_POSITION:     *usage = D3DDECLUSAGE_POSITION;     *usage_index = 0; return true;
  case d3d8::D3DVSDE_BLENDWEIGHT:  *usage = D3DDECLUSAGE_BLENDWEIGHT;  *usage_index = 0; return true;
  case d3d8::D3DVSDE_BLENDINDICES: *usage = D3DDECLUSAGE_BLENDINDICES; *usage_index = 0; return true;
  case d3d8::D3DVSDE_NORMAL:       *usage = D3DDECLUSAGE_NORMAL;       *usage_index = 0; return true;
  case d3d8::D3DVSDE_PSIZE:        *usage = D3DDECLUSAGE_PSIZE;        *usage_index = 0; return true;
  case d3d8::D3DVSDE_DIFFUSE:      *usage = D3DDECLUSAGE_COLOR;        *usage_index = 0; return true;
  case d3d8::D3DVSDE_SPECULAR:     *usage = D3DDECLUSAGE_COLOR;        *usage_index = 1; return true;
  case d3d8::D3DVSDE_POSITION2:    *usage = D3DDECLUSAGE_POSITION;     *usage_index = 1; return true;
  case d3d8::D3DVSDE_NORMAL2:      *usage = D3DDECLUSAGE_NORMAL;       *usage_index = 1; return true;
  default:
    if (reg >= d3d8::D3DVSDE_TEXCOORD0 && reg <= d3d8::D3DVSDE_TEXCOORD7) {
      *usage = D3DDECLUSAGE_TEXCOORD;
      *usage_index = static_cast<BYTE>(reg - d3d8::D3DVSDE_TEXCOORD0);
      return true;
    }
    return false;
  }
}

static bool convert_vertex_declaration8(const DWORD *decl8, std::vector<D3DVERTEXELEMENT9> &out) {
  out.clear();
  if (!decl8)
    return true;

  UINT current_stream = 0;
  WORD offsets[16] = {};

  for (UINT i = 0; i < 0x10000; i++) {
    DWORD token = decl8[i];
    if (token == 0xffffffffu)
      break;

    DWORD token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;
    switch (token_type) {
    case d3d8::D3DVSD_TOKEN_NOP:
      break;

    case d3d8::D3DVSD_TOKEN_STREAM:
      current_stream = token & D3DVSD_STREAMNUMBERMASK;
      if (current_stream >= ARRAYSIZE(offsets))
        return false;
      break;

    case d3d8::D3DVSD_TOKEN_STREAMDATA: {
      if (token & 0x10000000u) {
        DWORD skip = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;
        offsets[current_stream] += static_cast<WORD>(skip * sizeof(float));
        break;
      }

      BYTE type = static_cast<BYTE>((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
      DWORD reg = token & D3DVSD_VERTEXREGMASK;
      BYTE usage = 0, usage_index = 0;
      if (!map_decl_register(reg, &usage, &usage_index))
        return false;

      UINT size = decl_type_size(type);
      if (!size)
        return false;

      D3DVERTEXELEMENT9 element = {
        static_cast<WORD>(current_stream),
        offsets[current_stream],
        type,
        D3DDECLMETHOD_DEFAULT,
        usage,
        usage_index
      };
      out.push_back(element);
      offsets[current_stream] += static_cast<WORD>(size);
      break;
    }

    case d3d8::D3DVSD_TOKEN_CONSTMEM:
    case d3d8::D3DVSD_TOKEN_EXT:
      break;

    default:
      return false;
    }
  }

  out.push_back(D3DDECL_END());
  return true;
}

static bool map_tss_to_sampler(d3d8::D3DTEXTURESTAGESTATETYPE type, D3DSAMPLERSTATETYPE *sampler) {
  switch (type) {
  case d3d8::D3DTSS_ADDRESSU:      *sampler = D3DSAMP_ADDRESSU; return true;
  case d3d8::D3DTSS_ADDRESSV:      *sampler = D3DSAMP_ADDRESSV; return true;
  case d3d8::D3DTSS_ADDRESSW:      *sampler = D3DSAMP_ADDRESSW; return true;
  case d3d8::D3DTSS_BORDERCOLOR:   *sampler = D3DSAMP_BORDERCOLOR; return true;
  case d3d8::D3DTSS_MAGFILTER:     *sampler = D3DSAMP_MAGFILTER; return true;
  case d3d8::D3DTSS_MINFILTER:     *sampler = D3DSAMP_MINFILTER; return true;
  case d3d8::D3DTSS_MIPFILTER:     *sampler = D3DSAMP_MIPFILTER; return true;
  case d3d8::D3DTSS_MIPMAPLODBIAS: *sampler = D3DSAMP_MIPMAPLODBIAS; return true;
  case d3d8::D3DTSS_MAXMIPLEVEL:   *sampler = D3DSAMP_MAXMIPLEVEL; return true;
  case d3d8::D3DTSS_MAXANISOTROPY: *sampler = D3DSAMP_MAXANISOTROPY; return true;
  default: return false;
  }
}

class D3D8Interface;
class D3D8Device;
class D3D8Surface;
class D3D8Volume;
class D3D8SwapChain;
class D3D8VertexBuffer;
class D3D8IndexBuffer;
class D3D8Texture;
class D3D8CubeTexture;
class D3D8VolumeTexture;

template <typename Interface>
static HRESULT qi_this(Interface *iface, void **ppvObject) {
  *ppvObject = iface;
  iface->AddRef();
  return S_OK;
}

/* Recognising our own objects without calling through the pointer.
 *
 * An interface an application passes in is taken to be one of ours only if it
 * carries the vtable an object of that class is built with -- which is how
 * the D3D8 runtime itself tells, and for the same reason. The pointer may not
 * be ours at all: Fishdom 2 hands SetTexture something that is not a texture
 * and carries on as if nothing happened. Or it is ours with the vtable
 * replaced, which Wine's suite does before binding a texture. Asking the
 * object -- QueryInterface, as this used to -- calls into whatever is there.
 * Each class records its vtable when the first object of it is built; before
 * that no pointer can be one. */
template <typename T> struct ClassVtable {
  static inline std::atomic<const void *> value{nullptr};

  static void Record(const void *iface) {
    value.store(*static_cast<const void *const *>(iface), std::memory_order_relaxed);
  }
};

template <typename T, typename Interface>
static T *unsafe_impl(Interface *iface) {
  if (!iface || *reinterpret_cast<const void *const *>(iface) != ClassVtable<T>::value.load(std::memory_order_relaxed))
    return nullptr;
  return static_cast<T *>(iface);
}

class D3D8Interface final : public ComObjectClamp<d3d8::IDirect3D8> {
public:
  D3D8Interface(HMODULE module, Com<IDirect3D9> &&inner)
      : module_(module), inner_(std::move(inner)) {}

  ~D3D8Interface() {
    /* d3d9.dll stays loaded. Objects an application never released can
     * outlive this -- and with a device, the threads DXMT runs for it -- so
     * taking the code out from under them is not ours to do. */
    inner_ = nullptr;
  }

  static D3D8Interface *Create() {
    HMODULE module = LoadLibraryW(L"d3d9_dxmt.dll");
    if (!module)
      module = LoadLibraryW(L"D3D9_DXMT.DLL");
    if (!module) {
      /* No renamed copy: the d3d9 next to this d3d8, by full path, so it is
       * DXMT's and not whichever d3d9 the search order would find. A
       * deployment of the complete set (Wine builtins, a plain prefix) only
       * ships d3d9.dll, and without this every D3D8 application failed
       * Direct3DCreate8. */
      HMODULE self = nullptr;
      wchar_t path[MAX_PATH] = {};
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&D3D8Interface::Create), &self) &&
          GetModuleFileNameW(self, path, MAX_PATH)) {
        wchar_t *sep = wcsrchr(path, L'\\');
        if (sep && (size_t)(sep + 1 - path) + 9 < MAX_PATH) {
          wcscpy(sep + 1, L"d3d9.dll");
          module = LoadLibraryW(path);
        }
      }
    }
    if (!module) {
      return nullptr;
    }

    auto create9 = reinterpret_cast<Direct3DCreate9Proc>(GetProcAddress(module, "Direct3DCreate9"));
    if (!create9) {
      FreeLibrary(module);
      return nullptr;
    }

    IDirect3D9 *inner = create9(32);
    if (!inner) {
      FreeLibrary(module);
      return nullptr;
    }

    return new D3D8Interface(module, Com<IDirect3D9>::transfer(inner));
  }

  IDirect3D9 *Inner() const { return inner_.ptr(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3D8)
      return qi_this(static_cast<d3d8::IDirect3D8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *pInitializeFunction) final {
    return inner_->RegisterSoftwareDevice(pInitializeFunction);
  }

  UINT STDMETHODCALLTYPE GetAdapterCount() final {
    return inner_->GetAdapterCount();
  }

  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, d3d8::D3DADAPTER_IDENTIFIER8 *pIdentifier) final {
    if (!pIdentifier) return D3DERR_INVALIDCALL;
    D3DADAPTER_IDENTIFIER9 id9 = {};
    HRESULT hr = inner_->GetAdapterIdentifier(Adapter, Flags, &id9);
    if (FAILED(hr)) return hr;
    std::memset(pIdentifier, 0, sizeof(*pIdentifier));
    std::memcpy(pIdentifier->Driver, id9.Driver, sizeof(pIdentifier->Driver));
    std::memcpy(pIdentifier->Description, id9.Description, sizeof(pIdentifier->Description));
    pIdentifier->DriverVersion    = id9.DriverVersion;
    pIdentifier->VendorId         = id9.VendorId;
    pIdentifier->DeviceId         = id9.DeviceId;
    pIdentifier->SubSysId         = id9.SubSysId;
    pIdentifier->Revision         = id9.Revision;
    pIdentifier->DeviceIdentifier = id9.DeviceIdentifier;
    pIdentifier->WHQLLevel        = id9.WHQLLevel;
    return D3D_OK;
  }

  /* D3D8 has no format parameter here: one enumeration covers every display
   * format the adapter supports, and D3D9 split it per format afterwards. So
   * the D3D9 lists are concatenated rather than one of them being picked --
   * asking only for X8R8G8B8, as this did, hid every 16-bit mode from a game
   * that offers the user a colour depth. */
  static constexpr D3DFORMAT kD3D8ModeFormats[] = {
      D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5,
  };

  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter) final {
    UINT total = 0;
    for (D3DFORMAT format : kD3D8ModeFormats)
      total += inner_->GetAdapterModeCount(Adapter, format);
    return total;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, UINT Mode, d3d8::D3DDISPLAYMODE *pMode) final {
    for (D3DFORMAT format : kD3D8ModeFormats) {
      UINT count = inner_->GetAdapterModeCount(Adapter, format);
      if (Mode < count)
        return inner_->EnumAdapterModes(Adapter, format, Mode, reinterpret_cast<D3DDISPLAYMODE *>(pMode));
      Mode -= count;
    }
    return D3DERR_INVALIDCALL;
  }

  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, d3d8::D3DDISPLAYMODE *pMode) final {
    HRESULT hr = inner_->GetAdapterDisplayMode(Adapter, reinterpret_cast<D3DDISPLAYMODE *>(pMode));
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, d3d8::D3DDEVTYPE CheckType, d3d8::D3DFORMAT DisplayFormat,
      d3d8::D3DFORMAT BackBufferFormat, WINBOOL Windowed) final {
    return inner_->CheckDeviceType(Adapter, enum_cast<D3DDEVTYPE>(CheckType),
        enum_cast<D3DFORMAT>(DisplayFormat), enum_cast<D3DFORMAT>(BackBufferFormat), Windowed);
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, d3d8::D3DDEVTYPE DeviceType, d3d8::D3DFORMAT AdapterFormat,
      DWORD Usage, d3d8::D3DRESOURCETYPE RType, d3d8::D3DFORMAT CheckFormat) final {
    return inner_->CheckDeviceFormat(Adapter, enum_cast<D3DDEVTYPE>(DeviceType), enum_cast<D3DFORMAT>(AdapterFormat),
        Usage, enum_cast<D3DRESOURCETYPE>(RType), enum_cast<D3DFORMAT>(CheckFormat));
  }

  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, d3d8::D3DDEVTYPE DeviceType,
      d3d8::D3DFORMAT SurfaceFormat, WINBOOL Windowed, d3d8::D3DMULTISAMPLE_TYPE MultiSampleType) final {
    DWORD quality = 0;
    return inner_->CheckDeviceMultiSampleType(Adapter, enum_cast<D3DDEVTYPE>(DeviceType),
        enum_cast<D3DFORMAT>(SurfaceFormat), Windowed, enum_cast<D3DMULTISAMPLE_TYPE>(MultiSampleType), &quality);
  }

  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, d3d8::D3DDEVTYPE DeviceType,
      d3d8::D3DFORMAT AdapterFormat, d3d8::D3DFORMAT RenderTargetFormat, d3d8::D3DFORMAT DepthStencilFormat) final {
    return inner_->CheckDepthStencilMatch(Adapter, enum_cast<D3DDEVTYPE>(DeviceType),
        enum_cast<D3DFORMAT>(AdapterFormat), enum_cast<D3DFORMAT>(RenderTargetFormat),
        enum_cast<D3DFORMAT>(DepthStencilFormat));
  }

  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, d3d8::D3DDEVTYPE DeviceType, d3d8::D3DCAPS8 *pCaps) final {
    if (!pCaps) return D3DERR_INVALIDCALL;
    D3DCAPS9 caps9 = {};
    HRESULT hr = inner_->GetDeviceCaps(Adapter, enum_cast<D3DDEVTYPE>(DeviceType), &caps9);
    if (SUCCEEDED(hr))
      convert_caps(caps9, *pCaps);
    return hr;
  }

  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) final {
    return inner_->GetAdapterMonitor(Adapter);
  }

  HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, d3d8::D3DDEVTYPE DeviceType, HWND hFocusWindow,
      DWORD BehaviorFlags, d3d8::D3DPRESENT_PARAMETERS *pPresentationParameters,
      d3d8::IDirect3DDevice8 **ppReturnedDeviceInterface) final;

private:
  HMODULE module_ = nullptr;
  Com<IDirect3D9> inner_;
};

/* What a texture stage binding needs from any of the three texture classes:
 * a private reference, and both halves of the object. */
class D3D8BaseTexture {
public:
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  virtual d3d8::IDirect3DBaseTexture8 *Base8() = 0;
  virtual IDirect3DBaseTexture9 *Base9() = 0;

protected:
  ~D3D8BaseTexture() = default;
};

class D3D8Device final : public ComObjectClamp<d3d8::IDirect3DDevice8> {
public:
  static constexpr UINT kStages = 8;
  static constexpr UINT kStreams = 16;

  D3D8Device(D3D8Interface *parent, IDirect3DDevice9 *inner)
      : parent_(parent), inner_(inner) {
    parent_->AddRef();
  }

  ~D3D8Device();

  /* The D3D8 objects for the implicit surfaces: the implicit swap chain's
   * back buffers and the automatic depth stencil. D3D9 makes new ones on
   * every Reset. */
  void InitImplicitSurfaces();

  IDirect3DDevice9 *Inner() const { return inner_.ptr(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DDevice8)
      return qi_this(static_cast<d3d8::IDirect3DDevice8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() final { return inner_->TestCooperativeLevel(); }
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() final { return inner_->GetAvailableTextureMem(); }
  HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD) final { return inner_->EvictManagedResources(); }

  HRESULT STDMETHODCALLTYPE GetDirect3D(d3d8::IDirect3D8 **ppD3D8) final;

  HRESULT STDMETHODCALLTYPE GetDeviceCaps(d3d8::D3DCAPS8 *pCaps) final {
    if (!pCaps) return D3DERR_INVALIDCALL;
    D3DCAPS9 caps9 = {};
    HRESULT hr = inner_->GetDeviceCaps(&caps9);
    if (SUCCEEDED(hr))
      convert_caps(caps9, *pCaps);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDisplayMode(d3d8::D3DDISPLAYMODE *pMode) final {
    return inner_->GetDisplayMode(0, reinterpret_cast<D3DDISPLAYMODE *>(pMode));
  }

  HRESULT STDMETHODCALLTYPE GetCreationParameters(d3d8::D3DDEVICE_CREATION_PARAMETERS *pParameters) final {
    if (!pParameters) return D3DERR_INVALIDCALL;
    D3DDEVICE_CREATION_PARAMETERS params9 = {};
    HRESULT hr = inner_->GetCreationParameters(&params9);
    if (FAILED(hr)) return hr;
    pParameters->AdapterOrdinal = params9.AdapterOrdinal;
    pParameters->DeviceType     = enum_cast<d3d8::D3DDEVTYPE>(params9.DeviceType);
    pParameters->hFocusWindow   = params9.hFocusWindow;
    pParameters->BehaviorFlags  = params9.BehaviorFlags;
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, d3d8::IDirect3DSurface8 *pCursorBitmap) final;
  void STDMETHODCALLTYPE SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags) final {
    inner_->SetCursorPosition(static_cast<int>(XScreenSpace), static_cast<int>(YScreenSpace), Flags);
  }
  WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL bShow) final { return inner_->ShowCursor(bShow); }
  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(d3d8::D3DPRESENT_PARAMETERS *pPresentationParameters, d3d8::IDirect3DSwapChain8 **pSwapChain) final;
  HRESULT STDMETHODCALLTYPE Reset(d3d8::D3DPRESENT_PARAMETERS *pPresentationParameters) final;
  HRESULT STDMETHODCALLTYPE Present(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region) final {
    return inner_->Present(src_rect, dst_rect, dst_window_override, dirty_region);
  }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer, d3d8::D3DBACKBUFFER_TYPE Type, d3d8::IDirect3DSurface8 **ppBackBuffer) final;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(d3d8::D3DRASTER_STATUS *pRasterStatus) final {
    return inner_->GetRasterStatus(0, reinterpret_cast<D3DRASTER_STATUS *>(pRasterStatus));
  }
  void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const d3d8::D3DGAMMARAMP *ramp) final {
    inner_->SetGammaRamp(0, flags, reinterpret_cast<const D3DGAMMARAMP *>(ramp));
  }
  void STDMETHODCALLTYPE GetGammaRamp(d3d8::D3DGAMMARAMP *pRamp) final {
    inner_->GetGammaRamp(0, reinterpret_cast<D3DGAMMARAMP *>(pRamp));
  }

  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DTexture8 **ppTexture) final;
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DVolumeTexture8 **ppVolumeTexture) final;
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DCubeTexture8 **ppCubeTexture) final;
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, d3d8::D3DPOOL Pool, d3d8::IDirect3DVertexBuffer8 **ppVertexBuffer) final;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DIndexBuffer8 **ppIndexBuffer) final;
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, d3d8::D3DFORMAT Format, d3d8::D3DMULTISAMPLE_TYPE MultiSample, WINBOOL Lockable, d3d8::IDirect3DSurface8 **ppSurface) final;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, d3d8::D3DFORMAT Format, d3d8::D3DMULTISAMPLE_TYPE MultiSample, d3d8::IDirect3DSurface8 **ppSurface) final;
  HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT Width, UINT Height, d3d8::D3DFORMAT Format, d3d8::IDirect3DSurface8 **ppSurface) final;
  HRESULT STDMETHODCALLTYPE CopyRects(d3d8::IDirect3DSurface8 *src_surface, const RECT *src_rects, UINT rect_count, d3d8::IDirect3DSurface8 *dst_surface, const POINT *dst_points) final;
  HRESULT STDMETHODCALLTYPE UpdateTexture(d3d8::IDirect3DBaseTexture8 *pSourceTexture, d3d8::IDirect3DBaseTexture8 *pDestinationTexture) final;
  HRESULT STDMETHODCALLTYPE GetFrontBuffer(d3d8::IDirect3DSurface8 *pDestSurface) final;
  HRESULT STDMETHODCALLTYPE SetRenderTarget(d3d8::IDirect3DSurface8 *pRenderTarget, d3d8::IDirect3DSurface8 *pNewZStencil) final;
  HRESULT STDMETHODCALLTYPE GetRenderTarget(d3d8::IDirect3DSurface8 **ppRenderTarget) final;
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(d3d8::IDirect3DSurface8 **ppZStencilSurface) final;
  HRESULT STDMETHODCALLTYPE BeginScene() final { return inner_->BeginScene(); }
  HRESULT STDMETHODCALLTYPE EndScene() final { return inner_->EndScene(); }
  HRESULT STDMETHODCALLTYPE Clear(DWORD rect_count, const d3d8::D3DRECT *rects, DWORD flags, d3d8::D3DCOLOR color, float z, DWORD stencil) final {
    return inner_->Clear(rect_count, reinterpret_cast<const D3DRECT *>(rects), flags, color, z, stencil);
  }
  HRESULT STDMETHODCALLTYPE SetTransform(d3d8::D3DTRANSFORMSTATETYPE state, const d3d8::D3DMATRIX *matrix) final {
    return inner_->SetTransform(enum_cast<D3DTRANSFORMSTATETYPE>(state), reinterpret_cast<const D3DMATRIX *>(matrix));
  }
  HRESULT STDMETHODCALLTYPE GetTransform(d3d8::D3DTRANSFORMSTATETYPE State, d3d8::D3DMATRIX *pMatrix) final {
    return inner_->GetTransform(enum_cast<D3DTRANSFORMSTATETYPE>(State), reinterpret_cast<D3DMATRIX *>(pMatrix));
  }
  HRESULT STDMETHODCALLTYPE MultiplyTransform(d3d8::D3DTRANSFORMSTATETYPE state, const d3d8::D3DMATRIX *matrix) final {
    return inner_->MultiplyTransform(enum_cast<D3DTRANSFORMSTATETYPE>(state), reinterpret_cast<const D3DMATRIX *>(matrix));
  }
  HRESULT STDMETHODCALLTYPE SetViewport(const d3d8::D3DVIEWPORT8 *viewport) final {
    return inner_->SetViewport(reinterpret_cast<const D3DVIEWPORT9 *>(viewport));
  }
  HRESULT STDMETHODCALLTYPE GetViewport(d3d8::D3DVIEWPORT8 *pViewport) final {
    return inner_->GetViewport(reinterpret_cast<D3DVIEWPORT9 *>(pViewport));
  }
  HRESULT STDMETHODCALLTYPE SetMaterial(const d3d8::D3DMATERIAL8 *material) final {
    return inner_->SetMaterial(reinterpret_cast<const D3DMATERIAL9 *>(material));
  }
  HRESULT STDMETHODCALLTYPE GetMaterial(d3d8::D3DMATERIAL8 *pMaterial) final {
    return inner_->GetMaterial(reinterpret_cast<D3DMATERIAL9 *>(pMaterial));
  }
  HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const d3d8::D3DLIGHT8 *light) final {
    return inner_->SetLight(index, reinterpret_cast<const D3DLIGHT9 *>(light));
  }
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, d3d8::D3DLIGHT8 *pLight) final {
    return inner_->GetLight(Index, reinterpret_cast<D3DLIGHT9 *>(pLight));
  }
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, WINBOOL Enable) final { return inner_->LightEnable(Index, Enable); }
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, WINBOOL *pEnable) final { return inner_->GetLightEnable(Index, pEnable); }
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) final { return inner_->SetClipPlane(index, plane); }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) final { return inner_->GetClipPlane(Index, pPlane); }
  HRESULT STDMETHODCALLTYPE SetRenderState(d3d8::D3DRENDERSTATETYPE State, DWORD Value) final {
    return inner_->SetRenderState(enum_cast<D3DRENDERSTATETYPE>(State), Value);
  }
  HRESULT STDMETHODCALLTYPE GetRenderState(d3d8::D3DRENDERSTATETYPE State, DWORD *pValue) final {
    return inner_->GetRenderState(enum_cast<D3DRENDERSTATETYPE>(State), pValue);
  }
  HRESULT STDMETHODCALLTYPE BeginStateBlock() final;
  HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD *pToken) final;
  HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD Token) final;
  HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD Token) final;
  HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD Token) final;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(d3d8::D3DSTATEBLOCKTYPE Type, DWORD *pToken) final;
  HRESULT STDMETHODCALLTYPE SetClipStatus(const d3d8::D3DCLIPSTATUS8 *clip_status) final {
    D3DCLIPSTATUS9 cs9 = {};
    if (clip_status) {
      cs9.ClipUnion = clip_status->ClipUnion;
      cs9.ClipIntersection = clip_status->ClipIntersection;
    }
    return inner_->SetClipStatus(clip_status ? &cs9 : nullptr);
  }
  HRESULT STDMETHODCALLTYPE GetClipStatus(d3d8::D3DCLIPSTATUS8 *pClipStatus) final {
    if (!pClipStatus) return D3DERR_INVALIDCALL;
    D3DCLIPSTATUS9 cs9 = {};
    HRESULT hr = inner_->GetClipStatus(&cs9);
    if (SUCCEEDED(hr)) {
      pClipStatus->ClipUnion = cs9.ClipUnion;
      pClipStatus->ClipIntersection = cs9.ClipIntersection;
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, d3d8::IDirect3DBaseTexture8 **ppTexture) final;
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, d3d8::IDirect3DBaseTexture8 *pTexture) final;
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, d3d8::D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) final {
    D3DSAMPLERSTATETYPE sampler = D3DSAMP_ADDRESSU;
    if (map_tss_to_sampler(Type, &sampler))
      return inner_->GetSamplerState(Stage, sampler, pValue);
    return inner_->GetTextureStageState(Stage, enum_cast<D3DTEXTURESTAGESTATETYPE>(Type), pValue);
  }
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, d3d8::D3DTEXTURESTAGESTATETYPE Type, DWORD Value) final {
    D3DSAMPLERSTATETYPE sampler = D3DSAMP_ADDRESSU;
    if (map_tss_to_sampler(Type, &sampler))
      return inner_->SetSamplerState(Stage, sampler, Value);
    return inner_->SetTextureStageState(Stage, enum_cast<D3DTEXTURESTAGESTATETYPE>(Type), Value);
  }
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) final { return inner_->ValidateDevice(pNumPasses); }
  /* No device information is offered, which Windows reports as E_FAIL for the
   * ids before D3DDEVINFOID_VCACHE and S_FALSE from there on. Chessmaster
   * 9000 asks for the vertex cache one. */
  HRESULT STDMETHODCALLTYPE GetInfo(DWORD DevInfoID, void *, DWORD) final { return DevInfoID < 4 ? E_FAIL : S_FALSE; }
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) final { return inner_->SetPaletteEntries(palette_idx, entries); }
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) final { return inner_->GetPaletteEntries(PaletteNumber, pEntries); }
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) final { return inner_->SetCurrentTexturePalette(PaletteNumber); }
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) final { return inner_->GetCurrentTexturePalette(PaletteNumber); }
  HRESULT STDMETHODCALLTYPE DrawPrimitive(d3d8::D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) final {
    return inner_->DrawPrimitive(enum_cast<D3DPRIMITIVETYPE>(PrimitiveType), StartVertex, PrimitiveCount);
  }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(d3d8::D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices, UINT startIndex, UINT primCount) final {
    return inner_->DrawIndexedPrimitive(enum_cast<D3DPRIMITIVETYPE>(PrimitiveType), static_cast<INT>(base_vertex_index_), minIndex, NumVertices, startIndex, primCount);
  }
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(d3d8::D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride) final;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(d3d8::D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, d3d8::D3DFORMAT index_format, const void *data, UINT stride) final;
  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, d3d8::IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *declaration, const DWORD *byte_code, DWORD *shader, DWORD usage) final;
  HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD Handle) final;
  HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD *pHandle) final {
    if (!pHandle) return D3DERR_INVALIDCALL;
    *pHandle = current_vertex_shader_;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD Handle) final;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg_idx, const void *data, DWORD count) final {
    return inner_->SetVertexShaderConstantF(reg_idx, static_cast<const float *>(data), count);
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) final {
    return inner_->GetVertexShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData) final;
  HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) final;
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, d3d8::IDirect3DVertexBuffer8 *pStreamData, UINT Stride) final;
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, d3d8::IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) final;
  HRESULT STDMETHODCALLTYPE SetIndices(d3d8::IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex) final;
  HRESULT STDMETHODCALLTYPE GetIndices(d3d8::IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) final;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, DWORD *shader) final;
  HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD Handle) final;
  HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD *pHandle) final {
    if (!pHandle) return D3DERR_INVALIDCALL;
    *pHandle = current_pixel_shader_;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD Handle) final;
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg_idx, const void *data, DWORD count) final {
    return inner_->SetPixelShaderConstantF(reg_idx, static_cast<const float *>(data), count);
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) final {
    return inner_->GetPixelShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) final;
  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float *, const d3d8::D3DRECTPATCH_INFO *) final { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float *, const d3d8::D3DTRIPATCH_INFO *) final { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT) final { return D3D_OK; }

private:
  struct StreamBinding {
    Com<D3D8VertexBuffer, false> buffer;
    UINT stride = 0;
  };

  /* A state block's D3D8 half: the shader handles, which only this layer
   * knows, and the D3D8 objects behind the bindings the block carries --
   * applying it must bring back the very pointers it captured, which the
   * D3D9 half alone cannot name. Each binding is captured and applied only
   * when the block carries it: all of them for D3DSBT_ALL, the shader of its
   * kind for the vertex and pixel blocks, whatever was set while recording. */
  struct StateBlock {
    Com<IDirect3DStateBlock9> inner;
    bool vertex_shader = false;
    bool pixel_shader = false;
    bool indices = false;
    uint32_t streams = 0;  // bit per stream
    uint32_t textures = 0; // bit per stage
    DWORD vs = 0;
    DWORD ps = 0;
    Com<D3D8IndexBuffer, false> index_buffer;
    UINT base_vertex_index = 0;
    StreamBinding stream[kStreams];
    Com<D3D8BaseTexture, false> texture[kStages];
  };

  struct VertexShaderRecord {
    Com<IDirect3DVertexShader9> shader;
    Com<IDirect3DVertexDeclaration9> decl;
    std::vector<DWORD> declaration;
    std::vector<DWORD> function;
    DWORD usage = 0;
  };

  struct PixelShaderRecord {
    Com<IDirect3DPixelShader9> shader;
    std::vector<DWORD> function;
  };

  /* Bindings the D3D9 state no longer has -- after a Reset, which clears
   * them, or before it, which must not find them still holding the old
   * implicit surfaces. */
  void ClearBindings();
  void CaptureBindings(StateBlock &block);
  void ApplyBindings(const StateBlock &block);
  DWORD AllocateStateBlockHandle(std::unique_ptr<StateBlock> &&block);
  DWORD AllocateShaderHandle() { return next_shader_handle_++; }

  D3D8Interface *parent_;
  Com<IDirect3DDevice9> inner_;

  /* The D3D8 objects behind the device's bindings. Private references: D3D8
   * leaves an object's count alone when it is bound, and the device must not
   * keep itself alive through what it binds. */
  Com<D3D8BaseTexture, false> textures_[kStages];
  StreamBinding streams_[kStreams];
  Com<D3D8IndexBuffer, false> indices_;
  UINT base_vertex_index_ = 0;
  Com<D3D8Surface, false> render_target_;
  Com<D3D8Surface, false> depth_stencil_;
  /* The implicit surfaces, also held privately: they live as long as the
   * device does, at a count of zero included, so GetRenderTarget hands back
   * the same pointer every time. */
  std::vector<Com<D3D8Surface, false>> back_buffers_;
  Com<D3D8Surface, false> auto_depth_stencil_;

  DWORD current_vertex_shader_ = 0;
  DWORD current_pixel_shader_ = 0;
  DWORD next_stateblock_handle_ = 1;
  DWORD next_shader_handle_ = 0x80000000u;
  std::unordered_map<DWORD, std::unique_ptr<StateBlock>> stateblocks_;
  std::unique_ptr<StateBlock> recording_;
  std::unordered_map<DWORD, VertexShaderRecord> vertex_shaders_;
  std::unordered_map<DWORD, PixelShaderRecord> pixel_shaders_;
};

/* D3D8's reference rules, which D3D9 kept: an object made by the device holds
 * a reference on it for as long as the application holds one on the object.
 * The first AddRef takes it, the Release that brings the count back to zero
 * gives it back. What the device holds on to -- a binding, an implicit
 * surface -- it holds with a private reference, which keeps the object alive
 * at a count of zero without showing in either count. */
template <typename Interface>
class D3D8DeviceChild : public ComObjectClamp<Interface> {
public:
  explicit D3D8DeviceChild(D3D8Device *device) : device_(device) {}

  ULONG STDMETHODCALLTYPE AddRef() override {
    uint32_t count = this->m_refCount++;
    if (unlikely(!count)) {
      this->AddRefPrivate();
      device_->AddRef();
    }
    return count + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    /* Clamped at zero, as D3D8 and D3D9 are: releasing an implicit surface
     * that nothing holds is legal and returns zero again. */
    ULONG count = this->m_refCount;
    if (unlikely(!count))
      return 0;
    count = --this->m_refCount;
    if (!count) {
      /* ReleasePrivate may destroy this object; the device is read out
       * first and let go of last. */
      D3D8Device *device = device_;
      this->ReleasePrivate();
      device->Release();
    }
    return count;
  }

  D3D8Device *Device() const { return device_; }

protected:
  HRESULT GetDeviceImpl(d3d8::IDirect3DDevice8 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = ref(static_cast<d3d8::IDirect3DDevice8 *>(device_));
    return D3D_OK;
  }

  D3D8Device *device_;
};

template <typename Interface8, typename Interface9>
class D3D8Resource : public D3D8DeviceChild<Interface8> {
public:
  D3D8Resource(D3D8Device *device, Interface9 *inner)
      : D3D8DeviceChild<Interface8>(device), inner_(inner) {}

  Interface9 *Inner() const { return inner_.ptr(); }

  HRESULT STDMETHODCALLTYPE GetDevice(d3d8::IDirect3DDevice8 **ppDevice) final { return this->GetDeviceImpl(ppDevice); }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *data, DWORD data_size, DWORD flags) final {
    return inner_->SetPrivateData(refguid, data, data_size, flags);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) final {
    return inner_->GetPrivateData(refguid, pData, pSizeOfData);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) final { return inner_->FreePrivateData(refguid); }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) final { return inner_->SetPriority(PriorityNew); }
  DWORD STDMETHODCALLTYPE GetPriority() final { return inner_->GetPriority(); }
  void STDMETHODCALLTYPE PreLoad() final { inner_->PreLoad(); }
  d3d8::D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final {
    return enum_cast<d3d8::D3DRESOURCETYPE>(inner_->GetType());
  }

protected:
  Com<Interface9> inner_;
};

class D3D8Surface final : public D3D8DeviceChild<d3d8::IDirect3DSurface8> {
public:
  /* container: the texture a level or face belongs to. Such a surface has no
   * count of its own -- D3D8 gives it the texture's -- and lives exactly as
   * long as the texture, which owns it. Without one it is a child of the
   * device like any other, and GetContainer names the device. */
  D3D8Surface(D3D8Device *device, IDirect3DSurface9 *inner, d3d8::IDirect3DBaseTexture8 *container = nullptr,
              bool texture_level = false)
      : D3D8DeviceChild(device), inner_(inner), container_(container), texture_level_(texture_level) {
    ClassVtable<D3D8Surface>::Record(static_cast<d3d8::IDirect3DSurface8 *>(this));
  }

  IDirect3DSurface9 *Inner() const { return inner_.ptr(); }

  ULONG STDMETHODCALLTYPE AddRef() final { return container_ ? container_->AddRef() : D3D8DeviceChild::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() final { return container_ ? container_->Release() : D3D8DeviceChild::Release(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DSurface8)
      return qi_this(static_cast<d3d8::IDirect3DSurface8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(d3d8::IDirect3DDevice8 **ppDevice) final { return GetDeviceImpl(ppDevice); }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *data, DWORD data_size, DWORD flags) final { return inner_->SetPrivateData(refguid, data, data_size, flags); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) final { return inner_->GetPrivateData(refguid, pData, pSizeOfData); }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) final { return inner_->FreePrivateData(refguid); }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final {
    if (!ppContainer) return D3DERR_INVALIDCALL;
    if (container_) return container_->QueryInterface(riid, ppContainer);
    return device_->QueryInterface(riid, ppContainer);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(d3d8::D3DSURFACE_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC desc9 = {};
    HRESULT hr = inner_->GetDesc(&desc9);
    if (SUCCEEDED(hr)) {
      if (system_memory_)
        desc9.Pool = D3DPOOL_SYSTEMMEM;
      convert_surface_desc(desc9, *pDesc);
    }
    return hr;
  }
  /* An image surface whose format only scratch memory takes; see
   * CreateImageSurface. */
  void ReportSystemMemory() { system_memory_ = true; }
  /* D3D8 checks a lock rectangle itself, except on the levels of a 2D
   * texture, which get D3D9's more lenient rules; and where it refuses one,
   * or a second lock, it clears what the caller passed in. */
  HRESULT STDMETHODCALLTYPE LockRect(d3d8::D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) final {
    if (!locked_rect) return D3DERR_INVALIDCALL;
    if (!texture_level_ && rect) {
      D3DSURFACE_DESC desc = {};
      inner_->GetDesc(&desc);
      if (rect->left < 0 || rect->top < 0 || rect->left >= rect->right || rect->top >= rect->bottom ||
          static_cast<UINT>(rect->right) > desc.Width || static_cast<UINT>(rect->bottom) > desc.Height) {
        locked_rect->Pitch = 0;
        locked_rect->pBits = nullptr;
        return D3DERR_INVALIDCALL;
      }
    }
    HRESULT hr = inner_->LockRect(reinterpret_cast<D3DLOCKED_RECT *>(locked_rect), rect, flags);
    if (FAILED(hr) && !texture_level_) {
      locked_rect->Pitch = 0;
      locked_rect->pBits = nullptr;
    }
    return hr;
  }
  /* Unlocking a 2D texture's level that is not locked is not an error. */
  HRESULT STDMETHODCALLTYPE UnlockRect() final {
    HRESULT hr = inner_->UnlockRect();
    return FAILED(hr) && texture_level_ ? D3D_OK : hr;
  }

private:
  Com<IDirect3DSurface9> inner_;
  d3d8::IDirect3DBaseTexture8 *container_;
  bool texture_level_;
  bool system_memory_ = false;
};

/* D3D8 volumes exist only as levels of a volume texture, so they always share
 * its count and lifetime, the way a texture's surfaces do. */
class D3D8Volume final : public D3D8DeviceChild<d3d8::IDirect3DVolume8> {
public:
  D3D8Volume(D3D8Device *device, IDirect3DVolume9 *inner, d3d8::IDirect3DBaseTexture8 *container)
      : D3D8DeviceChild(device), inner_(inner), container_(container) {}

  IDirect3DVolume9 *Inner() const { return inner_.ptr(); }

  ULONG STDMETHODCALLTYPE AddRef() final { return container_->AddRef(); }
  ULONG STDMETHODCALLTYPE Release() final { return container_->Release(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DVolume8)
      return qi_this(static_cast<d3d8::IDirect3DVolume8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(d3d8::IDirect3DDevice8 **ppDevice) final { return GetDeviceImpl(ppDevice); }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *data, DWORD data_size, DWORD flags) final { return inner_->SetPrivateData(refguid, data, data_size, flags); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) final { return inner_->GetPrivateData(refguid, pData, pSizeOfData); }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) final { return inner_->FreePrivateData(refguid); }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final {
    if (!ppContainer) return D3DERR_INVALIDCALL;
    return container_->QueryInterface(riid, ppContainer);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(d3d8::D3DVOLUME_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    D3DVOLUME_DESC desc9 = {};
    HRESULT hr = inner_->GetDesc(&desc9);
    if (SUCCEEDED(hr))
      convert_volume_desc(desc9, *pDesc);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockBox(d3d8::D3DLOCKED_BOX *locked_box, const d3d8::D3DBOX *box, DWORD flags) final {
    return inner_->LockBox(reinterpret_cast<D3DLOCKED_BOX *>(locked_box), reinterpret_cast<const D3DBOX *>(box), flags);
  }
  HRESULT STDMETHODCALLTYPE UnlockBox() final { return inner_->UnlockBox(); }

private:
  Com<IDirect3DVolume9> inner_;
  d3d8::IDirect3DBaseTexture8 *container_;
};

class D3D8VertexBuffer final : public D3D8Resource<d3d8::IDirect3DVertexBuffer8, IDirect3DVertexBuffer9> {
public:
  D3D8VertexBuffer(D3D8Device *device, IDirect3DVertexBuffer9 *inner, DWORD fvf)
      : D3D8Resource(device, inner), fvf_(fvf) {
    ClassVtable<D3D8VertexBuffer>::Record(static_cast<d3d8::IDirect3DVertexBuffer8 *>(this));
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DResource8 || riid == d3d8::IID_IDirect3DVertexBuffer8)
      return qi_this(static_cast<d3d8::IDirect3DVertexBuffer8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) final {
    return inner_->Lock(OffsetToLock, SizeToLock, reinterpret_cast<void **>(ppbData), Flags);
  }
  HRESULT STDMETHODCALLTYPE Unlock() final { return inner_->Unlock(); }
  HRESULT STDMETHODCALLTYPE GetDesc(d3d8::D3DVERTEXBUFFER_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    D3DVERTEXBUFFER_DESC desc9 = {};
    HRESULT hr = inner_->GetDesc(&desc9);
    if (FAILED(hr)) return hr;
    pDesc->Format = enum_cast<d3d8::D3DFORMAT>(desc9.Format);
    pDesc->Type   = enum_cast<d3d8::D3DRESOURCETYPE>(desc9.Type);
    pDesc->Usage  = desc9.Usage;
    pDesc->Pool   = enum_cast<d3d8::D3DPOOL>(desc9.Pool);
    pDesc->Size   = desc9.Size;
    pDesc->FVF    = desc9.FVF ? desc9.FVF : fvf_;
    return D3D_OK;
  }

private:
  DWORD fvf_;
};

class D3D8IndexBuffer final : public D3D8Resource<d3d8::IDirect3DIndexBuffer8, IDirect3DIndexBuffer9> {
public:
  D3D8IndexBuffer(D3D8Device *device, IDirect3DIndexBuffer9 *inner)
      : D3D8Resource(device, inner) {
    ClassVtable<D3D8IndexBuffer>::Record(static_cast<d3d8::IDirect3DIndexBuffer8 *>(this));
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DResource8 || riid == d3d8::IID_IDirect3DIndexBuffer8)
      return qi_this(static_cast<d3d8::IDirect3DIndexBuffer8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) final {
    return inner_->Lock(OffsetToLock, SizeToLock, reinterpret_cast<void **>(ppbData), Flags);
  }
  HRESULT STDMETHODCALLTYPE Unlock() final { return inner_->Unlock(); }
  HRESULT STDMETHODCALLTYPE GetDesc(d3d8::D3DINDEXBUFFER_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    D3DINDEXBUFFER_DESC desc9 = {};
    HRESULT hr = inner_->GetDesc(&desc9);
    if (FAILED(hr)) return hr;
    pDesc->Format = enum_cast<d3d8::D3DFORMAT>(desc9.Format);
    pDesc->Type   = enum_cast<d3d8::D3DRESOURCETYPE>(desc9.Type);
    pDesc->Usage  = desc9.Usage;
    pDesc->Pool   = enum_cast<d3d8::D3DPOOL>(desc9.Pool);
    pDesc->Size   = desc9.Size;
    return D3D_OK;
  }
};

/* What the three texture classes share: the D3D8BaseTexture side, and the
 * IDirect3DBaseTexture8 methods. */
template <typename Interface8, typename Interface9>
class D3D8TextureBase : public D3D8Resource<Interface8, Interface9>, public D3D8BaseTexture {
public:
  using D3D8Resource<Interface8, Interface9>::D3D8Resource;

  void AddRefPrivate() final { ComObject<Interface8>::AddRefPrivate(); }
  void ReleasePrivate() final { ComObject<Interface8>::ReleasePrivate(); }
  d3d8::IDirect3DBaseTexture8 *Base8() final { return this; }
  IDirect3DBaseTexture9 *Base9() final { return this->inner_.ptr(); }

  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) final { return this->inner_->SetLOD(LODNew); }
  DWORD STDMETHODCALLTYPE GetLOD() final { return this->inner_->GetLOD(); }
  DWORD STDMETHODCALLTYPE GetLevelCount() final { return this->inner_->GetLevelCount(); }
};

class D3D8Texture final : public D3D8TextureBase<d3d8::IDirect3DTexture8, IDirect3DTexture9> {
public:
  D3D8Texture(D3D8Device *device, IDirect3DTexture9 *inner)
      : D3D8TextureBase(device, inner), levels_(inner->GetLevelCount()) {
    ClassVtable<D3D8Texture>::Record(static_cast<d3d8::IDirect3DTexture8 *>(this));
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DResource8 ||
        riid == d3d8::IID_IDirect3DBaseTexture8 || riid == d3d8::IID_IDirect3DTexture8)
      return qi_this(static_cast<d3d8::IDirect3DTexture8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, d3d8::D3DSURFACE_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC desc9 = {};
    HRESULT hr = inner_->GetLevelDesc(Level, &desc9);
    if (SUCCEEDED(hr)) convert_surface_desc(desc9, *pDesc);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, d3d8::IDirect3DSurface8 **ppSurfaceLevel) final;
  HRESULT STDMETHODCALLTYPE LockRect(UINT level, d3d8::D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) final {
    return inner_->LockRect(level, reinterpret_cast<D3DLOCKED_RECT *>(locked_rect), rect, flags);
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) final {
    HRESULT hr = inner_->UnlockRect(Level);
    return FAILED(hr) && Level < levels_.size() ? D3D_OK : hr;
  }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *dirty_rect) final { return inner_->AddDirtyRect(dirty_rect); }

private:
  /* One D3D8 surface per level, made the first time it is asked for. */
  std::vector<Com<D3D8Surface, false>> levels_;
};

class D3D8CubeTexture final : public D3D8TextureBase<d3d8::IDirect3DCubeTexture8, IDirect3DCubeTexture9> {
public:
  D3D8CubeTexture(D3D8Device *device, IDirect3DCubeTexture9 *inner)
      : D3D8TextureBase(device, inner), faces_(6u * inner->GetLevelCount()) {
    ClassVtable<D3D8CubeTexture>::Record(static_cast<d3d8::IDirect3DCubeTexture8 *>(this));
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DResource8 ||
        riid == d3d8::IID_IDirect3DBaseTexture8 || riid == d3d8::IID_IDirect3DCubeTexture8)
      return qi_this(static_cast<d3d8::IDirect3DCubeTexture8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, d3d8::D3DSURFACE_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC desc9 = {};
    HRESULT hr = inner_->GetLevelDesc(Level, &desc9);
    if (SUCCEEDED(hr)) convert_surface_desc(desc9, *pDesc);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetCubeMapSurface(d3d8::D3DCUBEMAP_FACES FaceType, UINT Level, d3d8::IDirect3DSurface8 **ppCubeMapSurface) final;
  /* As a face's own LockRect does (D3D8Surface), a refused lock clears what
   * the caller passed in. */
  HRESULT STDMETHODCALLTYPE LockRect(d3d8::D3DCUBEMAP_FACES face, UINT level, d3d8::D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) final {
    if (!locked_rect) return D3DERR_INVALIDCALL;
    HRESULT hr = inner_->LockRect(enum_cast<D3DCUBEMAP_FACES>(face), level, reinterpret_cast<D3DLOCKED_RECT *>(locked_rect), rect, flags);
    if (FAILED(hr)) {
      locked_rect->pBits = nullptr;
      locked_rect->Pitch = 0;
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(d3d8::D3DCUBEMAP_FACES FaceType, UINT Level) final {
    return inner_->UnlockRect(enum_cast<D3DCUBEMAP_FACES>(FaceType), Level);
  }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(d3d8::D3DCUBEMAP_FACES face, const RECT *dirty_rect) final {
    return inner_->AddDirtyRect(enum_cast<D3DCUBEMAP_FACES>(face), dirty_rect);
  }

private:
  /* Face-major, as D3D9 numbers the subresources: face * levels + level. */
  std::vector<Com<D3D8Surface, false>> faces_;
};

class D3D8VolumeTexture final : public D3D8TextureBase<d3d8::IDirect3DVolumeTexture8, IDirect3DVolumeTexture9> {
public:
  D3D8VolumeTexture(D3D8Device *device, IDirect3DVolumeTexture9 *inner)
      : D3D8TextureBase(device, inner), levels_(inner->GetLevelCount()) {
    ClassVtable<D3D8VolumeTexture>::Record(static_cast<d3d8::IDirect3DVolumeTexture8 *>(this));
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DResource8 ||
        riid == d3d8::IID_IDirect3DBaseTexture8 || riid == d3d8::IID_IDirect3DVolumeTexture8)
      return qi_this(static_cast<d3d8::IDirect3DVolumeTexture8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, d3d8::D3DVOLUME_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    D3DVOLUME_DESC desc9 = {};
    HRESULT hr = inner_->GetLevelDesc(Level, &desc9);
    if (SUCCEEDED(hr)) convert_volume_desc(desc9, *pDesc);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, d3d8::IDirect3DVolume8 **ppVolumeLevel) final;
  HRESULT STDMETHODCALLTYPE LockBox(UINT level, d3d8::D3DLOCKED_BOX *locked_box, const d3d8::D3DBOX *box, DWORD flags) final {
    return inner_->LockBox(level, reinterpret_cast<D3DLOCKED_BOX *>(locked_box), reinterpret_cast<const D3DBOX *>(box), flags);
  }
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) final { return inner_->UnlockBox(Level); }
  HRESULT STDMETHODCALLTYPE AddDirtyBox(const d3d8::D3DBOX *dirty_box) final {
    return inner_->AddDirtyBox(reinterpret_cast<const D3DBOX *>(dirty_box));
  }

private:
  std::vector<Com<D3D8Volume, false>> levels_;
};

class D3D8SwapChain final : public D3D8DeviceChild<d3d8::IDirect3DSwapChain8> {
public:
  D3D8SwapChain(D3D8Device *device, IDirect3DSwapChain9 *inner, UINT back_buffer_count)
      : D3D8DeviceChild(device), inner_(inner), back_buffers_(back_buffer_count ? back_buffer_count : 1) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == d3d8::IID_IDirect3DSwapChain8)
      return qi_this(static_cast<d3d8::IDirect3DSwapChain8 *>(this), ppvObject);
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region) final {
    return inner_->Present(src_rect, dst_rect, dst_window_override, dirty_region, 0);
  }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer, d3d8::D3DBACKBUFFER_TYPE Type, d3d8::IDirect3DSurface8 **ppBackBuffer) final {
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    *ppBackBuffer = nullptr;
    if (BackBuffer >= back_buffers_.size()) return D3DERR_INVALIDCALL;
    /* Its back buffers are the swap chain's to keep, the way the device keeps
     * its own: they survive a count of zero until the swap chain goes. */
    if (!back_buffers_[BackBuffer]) {
      Com<IDirect3DSurface9> surface9;
      HRESULT hr = inner_->GetBackBuffer(BackBuffer, enum_cast<D3DBACKBUFFER_TYPE>(Type), &surface9);
      if (FAILED(hr)) return hr;
      back_buffers_[BackBuffer] = new D3D8Surface(device_, surface9.ptr());
    }
    *ppBackBuffer = ref(back_buffers_[BackBuffer].ptr());
    return D3D_OK;
  }

private:
  Com<IDirect3DSwapChain9> inner_;
  std::vector<Com<D3D8Surface, false>> back_buffers_;
};

static D3D8BaseTexture *base_texture8(d3d8::IDirect3DBaseTexture8 *texture) {
  if (auto *t = unsafe_impl<D3D8Texture>(texture)) return t;
  if (auto *t = unsafe_impl<D3D8CubeTexture>(texture)) return t;
  if (auto *t = unsafe_impl<D3D8VolumeTexture>(texture)) return t;
  return nullptr;
}

HRESULT STDMETHODCALLTYPE
D3D8Interface::CreateDevice(UINT Adapter, d3d8::D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, d3d8::D3DPRESENT_PARAMETERS *pPresentationParameters,
    d3d8::IDirect3DDevice8 **ppReturnedDeviceInterface) {
  if (!pPresentationParameters || !ppReturnedDeviceInterface)
    return D3DERR_INVALIDCALL;
  *ppReturnedDeviceInterface = nullptr;

  D3DPRESENT_PARAMETERS pp9 = {};
  convert_present_params(*pPresentationParameters, pp9);

  IDirect3DDevice9 *device9 = nullptr;
  HRESULT hr = inner_->CreateDevice(Adapter, enum_cast<D3DDEVTYPE>(DeviceType), hFocusWindow,
      BehaviorFlags, &pp9, &device9);
  if (FAILED(hr))
    return hr;

  auto *device = new D3D8Device(this, device9);
  device9->Release();
  device->InitImplicitSurfaces();
  *ppReturnedDeviceInterface = ref(device);
  convert_present_params(pp9, *pPresentationParameters);
  return D3D_OK;
}

D3D8Device::~D3D8Device() {
  /* Everything holding D3D9 objects lets go of them before the D3D9 device
   * goes, and the D3D9 device before the parent: the parent holds the
   * IDirect3D9 all of it was made from. */
  recording_.reset();
  stateblocks_.clear();
  ClearBindings();
  vertex_shaders_.clear();
  pixel_shaders_.clear();
  inner_ = nullptr;
  parent_->Release();
}

void
D3D8Device::InitImplicitSurfaces() {
  back_buffers_.clear();
  auto_depth_stencil_ = nullptr;
  render_target_ = nullptr;
  depth_stencil_ = nullptr;

  UINT count = 1;
  Com<IDirect3DSwapChain9> swapchain;
  if (SUCCEEDED(inner_->GetSwapChain(0, &swapchain))) {
    D3DPRESENT_PARAMETERS pp = {};
    if (SUCCEEDED(swapchain->GetPresentParameters(&pp)) && pp.BackBufferCount)
      count = pp.BackBufferCount;
  }
  back_buffers_.resize(count);

  /* The first back buffer is the render target a new device starts with --
   * one object, which GetBackBuffer and GetRenderTarget both hand back. */
  Com<IDirect3DSurface9> rt9;
  if (SUCCEEDED(inner_->GetRenderTarget(0, &rt9))) {
    render_target_ = new D3D8Surface(this, rt9.ptr());
    Com<IDirect3DSurface9> bb9;
    if (SUCCEEDED(inner_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb9)) && bb9.ptr() == rt9.ptr())
      back_buffers_[0] = render_target_;
  }
  Com<IDirect3DSurface9> ds9;
  if (SUCCEEDED(inner_->GetDepthStencilSurface(&ds9))) {
    auto_depth_stencil_ = new D3D8Surface(this, ds9.ptr());
    depth_stencil_ = auto_depth_stencil_;
  }
}

void
D3D8Device::ClearBindings() {
  for (auto &texture : textures_)
    texture = nullptr;
  for (auto &stream : streams_) {
    stream.buffer = nullptr;
    stream.stride = 0;
  }
  indices_ = nullptr;
  base_vertex_index_ = 0;
  render_target_ = nullptr;
  depth_stencil_ = nullptr;
  back_buffers_.clear();
  auto_depth_stencil_ = nullptr;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetDirect3D(d3d8::IDirect3D8 **ppD3D8) {
  if (!ppD3D8) return D3DERR_INVALIDCALL;
  *ppD3D8 = ref(static_cast<d3d8::IDirect3D8 *>(parent_));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::Reset(d3d8::D3DPRESENT_PARAMETERS *pPresentationParameters) {
  if (!pPresentationParameters) return D3DERR_INVALIDCALL;
  D3DPRESENT_PARAMETERS pp9 = {};
  convert_present_params(*pPresentationParameters, pp9);

  /* D3D9 drops every binding and makes the implicit surfaces anew; the
   * D3D8 side goes first, so that nothing here still holds the old ones
   * when the swap chain's buffers are resized. */
  recording_.reset();
  ClearBindings();
  current_vertex_shader_ = 0;
  current_pixel_shader_ = 0;
  HRESULT hr = inner_->Reset(&pp9);
  InitImplicitSurfaces();
  if (FAILED(hr))
    return hr;
  convert_present_params(pp9, *pPresentationParameters);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, d3d8::IDirect3DSurface8 *pCursorBitmap) {
  auto *surface = unsafe_impl<D3D8Surface>(pCursorBitmap);
  if (!surface) return D3DERR_INVALIDCALL;
  return inner_->SetCursorProperties(XHotSpot, YHotSpot, surface->Inner());
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateAdditionalSwapChain(d3d8::D3DPRESENT_PARAMETERS *pPresentationParameters, d3d8::IDirect3DSwapChain8 **pSwapChain) {
  if (!pPresentationParameters || !pSwapChain)
    return D3DERR_INVALIDCALL;
  *pSwapChain = nullptr;
  D3DPRESENT_PARAMETERS pp9 = {};
  convert_present_params(*pPresentationParameters, pp9);
  Com<IDirect3DSwapChain9> swapchain9;
  HRESULT hr = inner_->CreateAdditionalSwapChain(&pp9, &swapchain9);
  if (FAILED(hr))
    return hr;
  *pSwapChain = ref(new D3D8SwapChain(this, swapchain9.ptr(), pp9.BackBufferCount));
  convert_present_params(pp9, *pPresentationParameters);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetBackBuffer(UINT BackBuffer, d3d8::D3DBACKBUFFER_TYPE Type, d3d8::IDirect3DSurface8 **ppBackBuffer) {
  if (!ppBackBuffer) return D3DERR_INVALIDCALL;
  *ppBackBuffer = nullptr;
  if (BackBuffer >= back_buffers_.size()) return D3DERR_INVALIDCALL;
  if (!back_buffers_[BackBuffer]) {
    Com<IDirect3DSurface9> surface9;
    HRESULT hr = inner_->GetBackBuffer(0, BackBuffer, enum_cast<D3DBACKBUFFER_TYPE>(Type), &surface9);
    if (FAILED(hr)) return hr;
    back_buffers_[BackBuffer] = new D3D8Surface(this, surface9.ptr());
  }
  *ppBackBuffer = ref(back_buffers_[BackBuffer].ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DTexture8 **ppTexture) {
  if (!ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  Com<IDirect3DTexture9> texture9;
  HRESULT hr = inner_->CreateTexture(Width, Height, Levels, Usage, enum_cast<D3DFORMAT>(Format), enum_cast<D3DPOOL>(Pool), &texture9, nullptr);
  if (FAILED(hr)) return hr;
  *ppTexture = ref(new D3D8Texture(this, texture9.ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DVolumeTexture8 **ppVolumeTexture) {
  if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
  *ppVolumeTexture = nullptr;
  Com<IDirect3DVolumeTexture9> texture9;
  HRESULT hr = inner_->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, enum_cast<D3DFORMAT>(Format), enum_cast<D3DPOOL>(Pool), &texture9, nullptr);
  if (FAILED(hr)) return hr;
  *ppVolumeTexture = ref(new D3D8VolumeTexture(this, texture9.ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DCubeTexture8 **ppCubeTexture) {
  if (!ppCubeTexture) return D3DERR_INVALIDCALL;
  *ppCubeTexture = nullptr;
  Com<IDirect3DCubeTexture9> texture9;
  HRESULT hr = inner_->CreateCubeTexture(EdgeLength, Levels, Usage, enum_cast<D3DFORMAT>(Format), enum_cast<D3DPOOL>(Pool), &texture9, nullptr);
  if (FAILED(hr)) return hr;
  *ppCubeTexture = ref(new D3D8CubeTexture(this, texture9.ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, d3d8::D3DPOOL Pool, d3d8::IDirect3DVertexBuffer8 **ppVertexBuffer) {
  if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
  *ppVertexBuffer = nullptr;
  Com<IDirect3DVertexBuffer9> buffer9;
  HRESULT hr = inner_->CreateVertexBuffer(Length, Usage, FVF, enum_cast<D3DPOOL>(Pool), &buffer9, nullptr);
  if (FAILED(hr)) return hr;
  *ppVertexBuffer = ref(new D3D8VertexBuffer(this, buffer9.ptr(), FVF));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateIndexBuffer(UINT Length, DWORD Usage, d3d8::D3DFORMAT Format, d3d8::D3DPOOL Pool, d3d8::IDirect3DIndexBuffer8 **ppIndexBuffer) {
  if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
  *ppIndexBuffer = nullptr;
  Com<IDirect3DIndexBuffer9> buffer9;
  HRESULT hr = inner_->CreateIndexBuffer(Length, Usage, enum_cast<D3DFORMAT>(Format), enum_cast<D3DPOOL>(Pool), &buffer9, nullptr);
  if (FAILED(hr)) return hr;
  *ppIndexBuffer = ref(new D3D8IndexBuffer(this, buffer9.ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateRenderTarget(UINT Width, UINT Height, d3d8::D3DFORMAT Format, d3d8::D3DMULTISAMPLE_TYPE MultiSample, WINBOOL Lockable, d3d8::IDirect3DSurface8 **ppSurface) {
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  Com<IDirect3DSurface9> surface9;
  HRESULT hr = inner_->CreateRenderTarget(Width, Height, enum_cast<D3DFORMAT>(Format), enum_cast<D3DMULTISAMPLE_TYPE>(MultiSample), 0, Lockable, &surface9, nullptr);
  if (FAILED(hr)) return hr;
  *ppSurface = ref(new D3D8Surface(this, surface9.ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateDepthStencilSurface(UINT Width, UINT Height, d3d8::D3DFORMAT Format, d3d8::D3DMULTISAMPLE_TYPE MultiSample, d3d8::IDirect3DSurface8 **ppSurface) {
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  Com<IDirect3DSurface9> surface9;
  HRESULT hr = inner_->CreateDepthStencilSurface(Width, Height, enum_cast<D3DFORMAT>(Format), enum_cast<D3DMULTISAMPLE_TYPE>(MultiSample), 0, TRUE, &surface9, nullptr);
  if (FAILED(hr)) return hr;
  *ppSurface = ref(new D3D8Surface(this, surface9.ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateImageSurface(UINT Width, UINT Height, d3d8::D3DFORMAT Format, d3d8::IDirect3DSurface8 **ppSurface) {
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  Com<IDirect3DSurface9> surface9;
  HRESULT hr = inner_->CreateOffscreenPlainSurface(Width, Height, enum_cast<D3DFORMAT>(Format), D3DPOOL_SYSTEMMEM, &surface9, nullptr);
  /* An image surface is system memory, but D3D8 creates one the way D3D9
   * creates scratch memory: in any format the runtime knows, whether or not
   * the driver could use it. Those the GPU cannot take become scratch
   * memory underneath, and still report the system-memory pool. */
  bool scratch = false;
  if (FAILED(hr)) {
    hr = inner_->CreateOffscreenPlainSurface(Width, Height, enum_cast<D3DFORMAT>(Format), D3DPOOL_SCRATCH, &surface9, nullptr);
    if (FAILED(hr)) return hr;
    scratch = true;
  }
  auto *surface = new D3D8Surface(this, surface9.ptr());
  if (scratch)
    surface->ReportSystemMemory();
  *ppSurface = ref(surface);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CopyRects(d3d8::IDirect3DSurface8 *src_surface, const RECT *src_rects, UINT rect_count, d3d8::IDirect3DSurface8 *dst_surface, const POINT *dst_points) {
  auto *src = unsafe_impl<D3D8Surface>(src_surface);
  auto *dst = unsafe_impl<D3D8Surface>(dst_surface);
  if (!src || !dst) return D3DERR_INVALIDCALL;
  if (!rect_count)
    return inner_->UpdateSurface(src->Inner(), nullptr, dst->Inner(), dst_points);
  for (UINT i = 0; i < rect_count; i++) {
    HRESULT hr = inner_->UpdateSurface(src->Inner(), &src_rects[i], dst->Inner(), dst_points ? &dst_points[i] : nullptr);
    if (FAILED(hr)) return hr;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::UpdateTexture(d3d8::IDirect3DBaseTexture8 *pSourceTexture, d3d8::IDirect3DBaseTexture8 *pDestinationTexture) {
  D3D8BaseTexture *src = base_texture8(pSourceTexture);
  D3D8BaseTexture *dst = base_texture8(pDestinationTexture);
  if (!src || !dst) return D3DERR_INVALIDCALL;
  return inner_->UpdateTexture(src->Base9(), dst->Base9());
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetFrontBuffer(d3d8::IDirect3DSurface8 *pDestSurface) {
  auto *dst = unsafe_impl<D3D8Surface>(pDestSurface);
  if (!dst) return D3DERR_INVALIDCALL;
  return inner_->GetFrontBufferData(0, dst->Inner());
}

HRESULT STDMETHODCALLTYPE
D3D8Device::SetRenderTarget(d3d8::IDirect3DSurface8 *pRenderTarget, d3d8::IDirect3DSurface8 *pNewZStencil) {
  auto *rt = unsafe_impl<D3D8Surface>(pRenderTarget);
  auto *ds = unsafe_impl<D3D8Surface>(pNewZStencil);
  if ((pRenderTarget && (!rt || rt->Device() != this)) || (pNewZStencil && !ds))
    return D3DERR_INVALIDCALL;

  /* A null render target keeps the current one; a null depth stencil
   * unbinds it. The depth stencil must cover the render target it is used
   * with and be multisampled the same way. */
  if (ds) {
    D3D8Surface *target = rt ? rt : render_target_.ptr();
    if (!target) return D3DERR_NOTFOUND;
    D3DSURFACE_DESC rt_desc = {}, ds_desc = {};
    target->Inner()->GetDesc(&rt_desc);
    ds->Inner()->GetDesc(&ds_desc);
    if (ds_desc.Width < rt_desc.Width || ds_desc.Height < rt_desc.Height ||
        ds_desc.MultiSampleType != rt_desc.MultiSampleType ||
        ds_desc.MultiSampleQuality != rt_desc.MultiSampleQuality)
      return D3DERR_INVALIDCALL;
  }

  HRESULT hr = inner_->SetDepthStencilSurface(ds ? ds->Inner() : nullptr);
  if (FAILED(hr)) return hr;
  if (rt) {
    hr = inner_->SetRenderTarget(0, rt->Inner());
    if (FAILED(hr)) {
      inner_->SetDepthStencilSurface(depth_stencil_ ? depth_stencil_->Inner() : nullptr);
      return hr;
    }
    render_target_ = rt;
  }
  depth_stencil_ = ds;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetRenderTarget(d3d8::IDirect3DSurface8 **ppRenderTarget) {
  if (!ppRenderTarget) return D3DERR_INVALIDCALL;
  *ppRenderTarget = nullptr;
  if (!render_target_) return D3DERR_NOTFOUND;
  *ppRenderTarget = ref(render_target_.ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetDepthStencilSurface(d3d8::IDirect3DSurface8 **ppZStencilSurface) {
  if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
  *ppZStencilSurface = nullptr;
  if (!depth_stencil_) return D3DERR_NOTFOUND;
  *ppZStencilSurface = ref(depth_stencil_.ptr());
  return D3D_OK;
}

void
D3D8Device::CaptureBindings(StateBlock &block) {
  if (block.vertex_shader)
    block.vs = current_vertex_shader_;
  if (block.pixel_shader)
    block.ps = current_pixel_shader_;
  if (block.indices) {
    block.index_buffer = indices_;
    block.base_vertex_index = base_vertex_index_;
  }
  for (UINT i = 0; i < kStreams; i++) {
    if (block.streams & (1u << i)) {
      block.stream[i].buffer = streams_[i].buffer;
      block.stream[i].stride = streams_[i].stride;
    }
  }
  for (UINT i = 0; i < kStages; i++) {
    if (block.textures & (1u << i))
      block.texture[i] = textures_[i];
  }
}

void
D3D8Device::ApplyBindings(const StateBlock &block) {
  if (block.vertex_shader)
    current_vertex_shader_ = block.vs;
  if (block.pixel_shader)
    current_pixel_shader_ = block.ps;
  if (block.indices) {
    indices_ = block.index_buffer;
    base_vertex_index_ = block.base_vertex_index;
  }
  for (UINT i = 0; i < kStreams; i++) {
    if (block.streams & (1u << i)) {
      streams_[i].buffer = block.stream[i].buffer;
      streams_[i].stride = block.stream[i].stride;
    }
  }
  for (UINT i = 0; i < kStages; i++) {
    if (block.textures & (1u << i))
      textures_[i] = block.texture[i];
  }
}

DWORD
D3D8Device::AllocateStateBlockHandle(std::unique_ptr<StateBlock> &&block) {
  DWORD handle = next_stateblock_handle_++;
  stateblocks_.emplace(handle, std::move(block));
  return handle;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::BeginStateBlock() {
  HRESULT hr = inner_->BeginStateBlock();
  if (FAILED(hr)) return hr;
  recording_ = std::make_unique<StateBlock>();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::EndStateBlock(DWORD *pToken) {
  if (!pToken) return D3DERR_INVALIDCALL;
  *pToken = 0;
  Com<IDirect3DStateBlock9> block9;
  HRESULT hr = inner_->EndStateBlock(&block9);
  if (FAILED(hr)) return hr;
  std::unique_ptr<StateBlock> block = std::move(recording_);
  if (!block)
    block = std::make_unique<StateBlock>();
  block->inner = std::move(block9);
  *pToken = AllocateStateBlockHandle(std::move(block));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::ApplyStateBlock(DWORD Token) {
  auto it = stateblocks_.find(Token);
  if (it == stateblocks_.end()) return D3DERR_INVALIDCALL;
  HRESULT hr = it->second->inner->Apply();
  if (FAILED(hr)) return hr;
  ApplyBindings(*it->second);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CaptureStateBlock(DWORD Token) {
  auto it = stateblocks_.find(Token);
  if (it == stateblocks_.end()) return D3DERR_INVALIDCALL;
  HRESULT hr = it->second->inner->Capture();
  if (FAILED(hr)) return hr;
  CaptureBindings(*it->second);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::DeleteStateBlock(DWORD Token) {
  return stateblocks_.erase(Token) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateStateBlock(d3d8::D3DSTATEBLOCKTYPE Type, DWORD *pToken) {
  if (!pToken) return D3DERR_INVALIDCALL;
  *pToken = 0;
  Com<IDirect3DStateBlock9> block9;
  HRESULT hr = inner_->CreateStateBlock(enum_cast<D3DSTATEBLOCKTYPE>(Type), &block9);
  if (FAILED(hr)) return hr;
  auto block = std::make_unique<StateBlock>();
  block->inner = std::move(block9);
  switch (Type) {
  case d3d8::D3DSBT_ALL:
    block->vertex_shader = block->pixel_shader = block->indices = true;
    block->streams = (1u << kStreams) - 1;
    block->textures = (1u << kStages) - 1;
    break;
  case d3d8::D3DSBT_VERTEXSTATE:
    block->vertex_shader = true;
    break;
  case d3d8::D3DSBT_PIXELSTATE:
    block->pixel_shader = true;
    break;
  default:
    break;
  }
  CaptureBindings(*block);
  *pToken = AllocateStateBlockHandle(std::move(block));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetTexture(DWORD Stage, d3d8::IDirect3DBaseTexture8 **ppTexture) {
  if (!ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  /* Stages past D3D8's eight are ignored, as Windows and Wine do. */
  if (Stage >= kStages) return D3D_OK;
  if (textures_[Stage])
    *ppTexture = ref(textures_[Stage]->Base8());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::SetTexture(DWORD Stage, d3d8::IDirect3DBaseTexture8 *pTexture) {
  if (Stage >= kStages) return D3D_OK;
  /* Anything that is not one of our textures binds nothing, and succeeds:
   * see unsafe_impl. */
  D3D8BaseTexture *texture = base_texture8(pTexture);
  HRESULT hr = inner_->SetTexture(Stage, texture ? texture->Base9() : nullptr);
  if (FAILED(hr)) return hr;
  if (recording_) {
    recording_->textures |= 1u << Stage;
    recording_->texture[Stage] = texture;
  } else {
    textures_[Stage] = texture;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::DrawPrimitiveUP(d3d8::D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride) {
  /* D3D8 draws nothing without complaint where D3D9 refuses a zero stride. */
  if (!primitive_count || !stride) return D3D_OK;
  HRESULT hr = inner_->DrawPrimitiveUP(enum_cast<D3DPRIMITIVETYPE>(primitive_type), primitive_count, data, stride);
  /* A user-pointer draw leaves stream 0 unbound, here as in D3D9. */
  if (SUCCEEDED(hr)) {
    streams_[0].buffer = nullptr;
    streams_[0].stride = 0;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::DrawIndexedPrimitiveUP(d3d8::D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, d3d8::D3DFORMAT index_format, const void *data, UINT stride) {
  if (!primitive_count || !stride) return D3D_OK;
  HRESULT hr = inner_->DrawIndexedPrimitiveUP(enum_cast<D3DPRIMITIVETYPE>(primitive_type), min_vertex_idx, vertex_count, primitive_count, index_data, enum_cast<D3DFORMAT>(index_format), data, stride);
  /* And the indices too. */
  if (SUCCEEDED(hr)) {
    streams_[0].buffer = nullptr;
    streams_[0].stride = 0;
    indices_ = nullptr;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, d3d8::IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) {
  auto *dst = unsafe_impl<D3D8VertexBuffer>(pDestBuffer);
  if (!dst) return D3DERR_INVALIDCALL;
  return inner_->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, dst->Inner(), nullptr, Flags);
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreateVertexShader(const DWORD *declaration, const DWORD *byte_code, DWORD *shader, DWORD usage) {
  if (!shader) return D3DERR_INVALIDCALL;
  *shader = 0;

  VertexShaderRecord record;
  record.declaration = copy_decl_tokens(declaration);
  record.function = copy_shader_tokens(byte_code);
  record.usage = usage;

  if (declaration) {
    std::vector<D3DVERTEXELEMENT9> elements;
    if (!convert_vertex_declaration8(declaration, elements))
      return D3DERR_INVALIDCALL;
    HRESULT hr = inner_->CreateVertexDeclaration(elements.data(), &record.decl);
    if (FAILED(hr)) return hr;
  }

  if (byte_code) {
    HRESULT hr = inner_->CreateVertexShader(byte_code, &record.shader);
    if (FAILED(hr)) return hr;
  }

  DWORD handle = AllocateShaderHandle();
  vertex_shaders_.emplace(handle, std::move(record));
  *shader = handle;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::SetVertexShader(DWORD Handle) {
  auto it = vertex_shaders_.find(Handle);
  HRESULT hr;
  if (it == vertex_shaders_.end()) {
    /* Not a handle: an FVF, for the fixed-function pipeline. */
    hr = inner_->SetVertexShader(nullptr);
    if (SUCCEEDED(hr))
      hr = inner_->SetFVF(Handle);
  } else {
    hr = D3D_OK;
    if (it->second.decl)
      hr = inner_->SetVertexDeclaration(it->second.decl.ptr());
    if (SUCCEEDED(hr))
      hr = inner_->SetVertexShader(it->second.shader.ptr());
  }
  if (FAILED(hr)) return hr;
  if (recording_) {
    recording_->vertex_shader = true;
    recording_->vs = Handle;
  } else {
    current_vertex_shader_ = Handle;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::DeleteVertexShader(DWORD Handle) {
  auto it = vertex_shaders_.find(Handle);
  if (it == vertex_shaders_.end()) return D3DERR_INVALIDCALL;
  if (current_vertex_shader_ == Handle)
    SetVertexShader(0);
  vertex_shaders_.erase(it);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData) {
  auto it = vertex_shaders_.find(Handle);
  if (it == vertex_shaders_.end()) return D3DERR_INVALIDCALL;
  return copy_dword_blob(it->second.declaration, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) {
  auto it = vertex_shaders_.find(Handle);
  if (it == vertex_shaders_.end()) return D3DERR_INVALIDCALL;
  return copy_dword_blob(it->second.function, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
D3D8Device::SetStreamSource(UINT StreamNumber, d3d8::IDirect3DVertexBuffer8 *pStreamData, UINT Stride) {
  if (StreamNumber >= kStreams) return D3DERR_INVALIDCALL;
  auto *vb = unsafe_impl<D3D8VertexBuffer>(pStreamData);
  if (pStreamData && !vb) return D3DERR_INVALIDCALL;
  /* Unbinding a stream keeps its stride. */
  if (!vb)
    Stride = streams_[StreamNumber].stride;
  HRESULT hr = inner_->SetStreamSource(StreamNumber, vb ? vb->Inner() : nullptr, 0, Stride);
  if (FAILED(hr)) return hr;
  StreamBinding *binding = &streams_[StreamNumber];
  if (recording_) {
    recording_->streams |= 1u << StreamNumber;
    binding = &recording_->stream[StreamNumber];
  }
  binding->buffer = vb;
  binding->stride = Stride;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetStreamSource(UINT StreamNumber, d3d8::IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) {
  if (!ppStreamData || !pStride) return D3DERR_INVALIDCALL;
  *ppStreamData = nullptr;
  if (StreamNumber >= kStreams) return D3DERR_INVALIDCALL;
  *ppStreamData = ref(static_cast<d3d8::IDirect3DVertexBuffer8 *>(streams_[StreamNumber].buffer.ptr()));
  *pStride = streams_[StreamNumber].stride;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::SetIndices(d3d8::IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex) {
  auto *ib = unsafe_impl<D3D8IndexBuffer>(pIndexData);
  if (pIndexData && !ib) return D3DERR_INVALIDCALL;
  HRESULT hr = inner_->SetIndices(ib ? ib->Inner() : nullptr);
  if (FAILED(hr)) return hr;
  if (recording_) {
    recording_->indices = true;
    recording_->index_buffer = ib;
    recording_->base_vertex_index = BaseVertexIndex;
  } else {
    indices_ = ib;
    base_vertex_index_ = BaseVertexIndex;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetIndices(d3d8::IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) {
  if (!ppIndexData || !pBaseVertexIndex) return D3DERR_INVALIDCALL;
  *ppIndexData = ref(static_cast<d3d8::IDirect3DIndexBuffer8 *>(indices_.ptr()));
  *pBaseVertexIndex = base_vertex_index_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::CreatePixelShader(const DWORD *byte_code, DWORD *shader) {
  if (!byte_code || !shader) return D3DERR_INVALIDCALL;
  *shader = 0;
  PixelShaderRecord record;
  HRESULT hr = inner_->CreatePixelShader(byte_code, &record.shader);
  if (FAILED(hr)) return hr;
  record.function = copy_shader_tokens(byte_code);
  DWORD handle = AllocateShaderHandle();
  pixel_shaders_.emplace(handle, std::move(record));
  *shader = handle;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::SetPixelShader(DWORD Handle) {
  IDirect3DPixelShader9 *shader9 = nullptr;
  if (Handle) {
    auto it = pixel_shaders_.find(Handle);
    if (it == pixel_shaders_.end()) return D3DERR_INVALIDCALL;
    shader9 = it->second.shader.ptr();
  }
  HRESULT hr = inner_->SetPixelShader(shader9);
  if (FAILED(hr)) return hr;
  if (recording_) {
    recording_->pixel_shader = true;
    recording_->ps = Handle;
  } else {
    current_pixel_shader_ = Handle;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::DeletePixelShader(DWORD Handle) {
  auto it = pixel_shaders_.find(Handle);
  if (it == pixel_shaders_.end()) return D3DERR_INVALIDCALL;
  if (current_pixel_shader_ == Handle)
    SetPixelShader(0);
  pixel_shaders_.erase(it);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8Device::GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) {
  auto it = pixel_shaders_.find(Handle);
  if (it == pixel_shaders_.end()) return D3DERR_INVALIDCALL;
  return copy_dword_blob(it->second.function, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
D3D8Texture::GetSurfaceLevel(UINT Level, d3d8::IDirect3DSurface8 **ppSurfaceLevel) {
  if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = nullptr;
  if (Level >= levels_.size()) return D3DERR_INVALIDCALL;
  if (!levels_[Level]) {
    Com<IDirect3DSurface9> surface9;
    HRESULT hr = inner_->GetSurfaceLevel(Level, &surface9);
    if (FAILED(hr)) return hr;
    levels_[Level] = new D3D8Surface(device_, surface9.ptr(), static_cast<d3d8::IDirect3DTexture8 *>(this), true);
  }
  *ppSurfaceLevel = ref(levels_[Level].ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8CubeTexture::GetCubeMapSurface(d3d8::D3DCUBEMAP_FACES FaceType, UINT Level, d3d8::IDirect3DSurface8 **ppCubeMapSurface) {
  if (!ppCubeMapSurface) return D3DERR_INVALIDCALL;
  *ppCubeMapSurface = nullptr;
  UINT levels = static_cast<UINT>(faces_.size() / 6);
  UINT face = static_cast<UINT>(FaceType);
  if (face >= 6 || Level >= levels) return D3DERR_INVALIDCALL;
  auto &slot = faces_[face * levels + Level];
  if (!slot) {
    Com<IDirect3DSurface9> surface9;
    HRESULT hr = inner_->GetCubeMapSurface(enum_cast<D3DCUBEMAP_FACES>(FaceType), Level, &surface9);
    if (FAILED(hr)) return hr;
    slot = new D3D8Surface(device_, surface9.ptr(), static_cast<d3d8::IDirect3DCubeTexture8 *>(this));
  }
  *ppCubeMapSurface = ref(slot.ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D8VolumeTexture::GetVolumeLevel(UINT Level, d3d8::IDirect3DVolume8 **ppVolumeLevel) {
  if (!ppVolumeLevel) return D3DERR_INVALIDCALL;
  *ppVolumeLevel = nullptr;
  if (Level >= levels_.size()) return D3DERR_INVALIDCALL;
  if (!levels_[Level]) {
    Com<IDirect3DVolume9> volume9;
    HRESULT hr = inner_->GetVolumeLevel(Level, &volume9);
    if (FAILED(hr)) return hr;
    levels_[Level] = new D3D8Volume(device_, volume9.ptr(), static_cast<d3d8::IDirect3DVolumeTexture8 *>(this));
  }
  *ppVolumeLevel = ref(levels_[Level].ptr());
  return D3D_OK;
}

} // namespace
} // namespace dxmt

extern "C" d3d8::IDirect3D8 *WINAPI
Direct3DCreate8(UINT SDKVersion) {
  /* Not validated, deliberately. D3D8 ships two SDK versions -- 120 for
   * DirectX 8.0 and 220 for 8.1 -- and neither Windows nor Wine refuses
   * either here; the check belongs to d3d9, which does validate. Requiring
   * 220 meant a game built against the 8.0 SDK got a null interface back and
   * quit without a word, which is what Splinter Cell did. */
  (void)SDKVersion;
  auto *iface = dxmt::D3D8Interface::Create();
  return iface ? dxmt::ref(iface) : nullptr;
}

/* The rest of d3d8.dll's exports. A missing one is not a missing feature: the
 * loader resolves an application's import of it to null, and the application
 * faults the moment it calls it. */

/* The message goes back only when the caller asks for one. Given somewhere to
 * put it but not asked, the caller still gets a string -- an empty one. */
static HRESULT shader_validation_result(const char *message, BOOL return_error, char **errors) {
  if (errors) {
    const char *text = return_error && message ? message : "";
    size_t size = std::strlen(text) + 1;
    if ((*errors = static_cast<char *>(HeapAlloc(GetProcessHeap(), 0, size))))
      std::memcpy(*errors, text, size);
  }
  return message ? E_FAIL : S_OK;
}

extern "C" HRESULT WINAPI
ValidateVertexShader(const DWORD *code, const DWORD *declaration, const d3d8::D3DCAPS8 *caps, BOOL return_error,
                     char **errors) {
  (void)declaration;
  const char *message = nullptr;
  if (!code)
    message = "Invalid shader code pointer.\n";
  else if (*code != D3DVS_VERSION(1, 0) && *code != D3DVS_VERSION(1, 1))
    message = "Unsupported vertex shader version.\n";
  else if (caps && *code > caps->VertexShaderVersion)
    message = "Vertex shader version is above the device's.\n";
  return shader_validation_result(message, return_error, errors);
}

extern "C" HRESULT WINAPI
ValidatePixelShader(const DWORD *code, const d3d8::D3DCAPS8 *caps, BOOL return_error, char **errors) {
  /* Unlike the vertex validator, no code means no answer at all: errors is
   * left as it was. */
  if (!code)
    return E_FAIL;
  const char *message = nullptr;
  if (*code < D3DPS_VERSION(1, 0) || *code > D3DPS_VERSION(1, 4))
    message = "Unsupported pixel shader version.\n";
  else if (caps && *code > caps->PixelShaderVersion)
    message = "Pixel shader version is above the device's.\n";
  return shader_validation_result(message, return_error, errors);
}

extern "C" void WINAPI DebugSetMute(void) {}

extern "C" HRESULT WINAPI D3D8GetSWInfo(void) { return D3D_OK; }

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(instance);
  return TRUE;
}
#endif
