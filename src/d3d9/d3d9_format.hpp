#pragma once

#include "d3d9.h"
#include "dxgi.h"

namespace dxmt::d3d9 {

inline DXGI_FORMAT ConvertFormat(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:       return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_X8R8G8B8:       return DXGI_FORMAT_B8G8R8X8_UNORM;
    case D3DFMT_R8G8B8:         return DXGI_FORMAT_B8G8R8X8_UNORM; // no exact match
    case D3DFMT_R5G6B5:         return DXGI_FORMAT_B5G6R5_UNORM;
    case D3DFMT_A1R5G5B5:       return DXGI_FORMAT_B5G5R5A1_UNORM;
    case D3DFMT_X1R5G5B5:       return DXGI_FORMAT_B5G5R5A1_UNORM;
    case D3DFMT_A4R4G4B4:       return DXGI_FORMAT_B4G4R4A4_UNORM;
    case D3DFMT_A8:              return DXGI_FORMAT_A8_UNORM;
    case D3DFMT_A2R10G10B10:    return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DFMT_A2B10G10R10:    return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DFMT_A8B8G8R8:       return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DFMT_X8B8G8R8:       return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DFMT_G16R16:         return DXGI_FORMAT_R16G16_UNORM;
    case D3DFMT_A16B16G16R16:   return DXGI_FORMAT_R16G16B16A16_UNORM;
    case D3DFMT_L8:              return DXGI_FORMAT_R8_UNORM;
    case D3DFMT_A8L8:            return DXGI_FORMAT_R8G8_UNORM;
    case D3DFMT_V8U8:            return DXGI_FORMAT_R8G8_SNORM;
    case D3DFMT_Q8W8V8U8:       return DXGI_FORMAT_R8G8B8A8_SNORM;
    case D3DFMT_V16U16:         return DXGI_FORMAT_R16G16_SNORM;
    case D3DFMT_Q16W16V16U16:   return DXGI_FORMAT_R16G16B16A16_SNORM;
    case D3DFMT_DXT1:            return DXGI_FORMAT_BC1_UNORM;
    case D3DFMT_DXT2:            return DXGI_FORMAT_BC2_UNORM;
    case D3DFMT_DXT3:            return DXGI_FORMAT_BC2_UNORM;
    case D3DFMT_DXT4:            return DXGI_FORMAT_BC3_UNORM;
    case D3DFMT_DXT5:            return DXGI_FORMAT_BC3_UNORM;
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:   return DXGI_FORMAT_D16_UNORM;
    case D3DFMT_D32:             return DXGI_FORMAT_D32_FLOAT;
    case D3DFMT_D24S8:           return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case D3DFMT_D24X8:           return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case D3DFMT_D32F_LOCKABLE:  return DXGI_FORMAT_D32_FLOAT;
    case D3DFMT_D24FS8:         return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case D3DFMT_R16F:            return DXGI_FORMAT_R16_FLOAT;
    case D3DFMT_G16R16F:        return DXGI_FORMAT_R16G16_FLOAT;
    case D3DFMT_A16B16G16R16F:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case D3DFMT_R32F:            return DXGI_FORMAT_R32_FLOAT;
    case D3DFMT_G32R32F:        return DXGI_FORMAT_R32G32_FLOAT;
    case D3DFMT_A32B32G32R32F:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DFMT_INDEX16:        return DXGI_FORMAT_R16_UINT;
    case D3DFMT_INDEX32:        return DXGI_FORMAT_R32_UINT;
    default:                     return DXGI_FORMAT_UNKNOWN;
    }
}

inline DXGI_FORMAT ConvertDepthFormat(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:   return DXGI_FORMAT_R16_TYPELESS;
    case D3DFMT_D32:             return DXGI_FORMAT_R32_TYPELESS;
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:          return DXGI_FORMAT_R24G8_TYPELESS;
    case D3DFMT_D32F_LOCKABLE:  return DXGI_FORMAT_R32_TYPELESS;
    case D3DFMT_D24FS8:         return DXGI_FORMAT_R24G8_TYPELESS;
    default:                     return DXGI_FORMAT_UNKNOWN;
    }
}

inline bool IsDepthFormat(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D32:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8:
    case D3DFMT_D32_LOCKABLE:   return true;
    default:                     return false;
    }
}

inline UINT GetFormatBPP(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:    return 4;
    case D3DFMT_R8G8B8:         return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_R16F:
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_INDEX16:        return 2;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:             return 1;
    case D3DFMT_A16B16G16R16:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_G16R16:
    case D3DFMT_R32F:
    case D3DFMT_G16R16F:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_INDEX32:        return 4;
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_Q16W16V16U16:   return 8;
    default:                     return 4;
    }
}

inline D3D11_BLEND ConvertBlend(D3DBLEND blend) {
    switch (blend) {
    case D3DBLEND_ZERO:             return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE:              return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:         return D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:      return D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:         return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:      return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:        return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA:     return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR:        return D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR:     return D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT:      return D3D11_BLEND_SRC_ALPHA_SAT;
    case D3DBLEND_BLENDFACTOR:      return D3D11_BLEND_BLEND_FACTOR;
    case D3DBLEND_INVBLENDFACTOR:   return D3D11_BLEND_INV_BLEND_FACTOR;
    default:                         return D3D11_BLEND_ONE;
    }
}

inline D3D11_BLEND_OP ConvertBlendOp(D3DBLENDOP op) {
    switch (op) {
    case D3DBLENDOP_ADD:            return D3D11_BLEND_OP_ADD;
    case D3DBLENDOP_SUBTRACT:       return D3D11_BLEND_OP_SUBTRACT;
    case D3DBLENDOP_REVSUBTRACT:    return D3D11_BLEND_OP_REV_SUBTRACT;
    case D3DBLENDOP_MIN:            return D3D11_BLEND_OP_MIN;
    case D3DBLENDOP_MAX:            return D3D11_BLEND_OP_MAX;
    default:                         return D3D11_BLEND_OP_ADD;
    }
}

inline D3D11_COMPARISON_FUNC ConvertCmpFunc(D3DCMPFUNC func) {
    switch (func) {
    case D3DCMP_NEVER:          return D3D11_COMPARISON_NEVER;
    case D3DCMP_LESS:           return D3D11_COMPARISON_LESS;
    case D3DCMP_EQUAL:          return D3D11_COMPARISON_EQUAL;
    case D3DCMP_LESSEQUAL:      return D3D11_COMPARISON_LESS_EQUAL;
    case D3DCMP_GREATER:        return D3D11_COMPARISON_GREATER;
    case D3DCMP_NOTEQUAL:       return D3D11_COMPARISON_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL:   return D3D11_COMPARISON_GREATER_EQUAL;
    case D3DCMP_ALWAYS:         return D3D11_COMPARISON_ALWAYS;
    default:                     return D3D11_COMPARISON_ALWAYS;
    }
}

inline D3D11_STENCIL_OP ConvertStencilOp(D3DSTENCILOP op) {
    switch (op) {
    case D3DSTENCILOP_KEEP:     return D3D11_STENCIL_OP_KEEP;
    case D3DSTENCILOP_ZERO:     return D3D11_STENCIL_OP_ZERO;
    case D3DSTENCILOP_REPLACE:  return D3D11_STENCIL_OP_REPLACE;
    case D3DSTENCILOP_INCRSAT:  return D3D11_STENCIL_OP_INCR_SAT;
    case D3DSTENCILOP_DECRSAT:  return D3D11_STENCIL_OP_DECR_SAT;
    case D3DSTENCILOP_INVERT:   return D3D11_STENCIL_OP_INVERT;
    case D3DSTENCILOP_INCR:     return D3D11_STENCIL_OP_INCR;
    case D3DSTENCILOP_DECR:     return D3D11_STENCIL_OP_DECR;
    default:                     return D3D11_STENCIL_OP_KEEP;
    }
}

inline D3D11_CULL_MODE ConvertCullMode(D3DCULL cull) {
    switch (cull) {
    case D3DCULL_NONE:  return D3D11_CULL_NONE;
    case D3DCULL_CW:    return D3D11_CULL_FRONT; // D3D9 CW = D3D11 front face
    case D3DCULL_CCW:   return D3D11_CULL_BACK;
    default:             return D3D11_CULL_NONE;
    }
}

inline D3D11_FILL_MODE ConvertFillMode(D3DFILLMODE fill) {
    switch (fill) {
    case D3DFILL_WIREFRAME: return D3D11_FILL_WIREFRAME;
    case D3DFILL_SOLID:     return D3D11_FILL_SOLID;
    default:                 return D3D11_FILL_SOLID;
    }
}

inline D3D11_TEXTURE_ADDRESS_MODE ConvertTextureAddress(D3DTEXTUREADDRESS addr) {
    switch (addr) {
    case D3DTADDRESS_WRAP:       return D3D11_TEXTURE_ADDRESS_WRAP;
    case D3DTADDRESS_MIRROR:     return D3D11_TEXTURE_ADDRESS_MIRROR;
    case D3DTADDRESS_CLAMP:      return D3D11_TEXTURE_ADDRESS_CLAMP;
    case D3DTADDRESS_BORDER:     return D3D11_TEXTURE_ADDRESS_BORDER;
    case D3DTADDRESS_MIRRORONCE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default:                      return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

inline D3D11_FILTER ConvertFilter(D3DTEXTUREFILTERTYPE min, D3DTEXTUREFILTERTYPE mag, D3DTEXTUREFILTERTYPE mip, bool aniso) {
    if (aniso)
        return D3D11_FILTER_ANISOTROPIC;
    bool minLinear  = (min == D3DTEXF_LINEAR);
    bool magLinear  = (mag == D3DTEXF_LINEAR);
    bool mipLinear  = (mip == D3DTEXF_LINEAR);
    if (minLinear && magLinear && mipLinear) return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (minLinear && magLinear)              return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (minLinear && mipLinear)              return D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    if (magLinear && mipLinear)              return D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    if (minLinear)                           return D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    if (magLinear)                           return D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    if (mipLinear)                           return D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    return D3D11_FILTER_MIN_MAG_MIP_POINT;
}

inline D3D11_PRIMITIVE_TOPOLOGY ConvertPrimType(D3DPRIMITIVETYPE type) {
    switch (type) {
    case D3DPT_POINTLIST:     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_LINELIST:      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_TRIANGLELIST:  return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; // converted
    default:                   return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

inline UINT PrimCountToVertexCount(D3DPRIMITIVETYPE type, UINT primCount) {
    switch (type) {
    case D3DPT_POINTLIST:     return primCount;
    case D3DPT_LINELIST:      return primCount * 2;
    case D3DPT_LINESTRIP:     return primCount + 1;
    case D3DPT_TRIANGLELIST:  return primCount * 3;
    case D3DPT_TRIANGLESTRIP: return primCount + 2;
    case D3DPT_TRIANGLEFAN:   return primCount + 2;
    default:                   return primCount * 3;
    }
}

inline DXGI_FORMAT ConvertDeclType(D3DDECLTYPE type) {
    switch (type) {
    case D3DDECLTYPE_FLOAT1:    return DXGI_FORMAT_R32_FLOAT;
    case D3DDECLTYPE_FLOAT2:    return DXGI_FORMAT_R32G32_FLOAT;
    case D3DDECLTYPE_FLOAT3:    return DXGI_FORMAT_R32G32B32_FLOAT;
    case D3DDECLTYPE_FLOAT4:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DDECLTYPE_D3DCOLOR:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4:    return DXGI_FORMAT_R8G8B8A8_UINT;
    case D3DDECLTYPE_SHORT2:    return DXGI_FORMAT_R16G16_SINT;
    case D3DDECLTYPE_SHORT4:    return DXGI_FORMAT_R16G16B16A16_SINT;
    case D3DDECLTYPE_UBYTE4N:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N:   return DXGI_FORMAT_R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N:   return DXGI_FORMAT_R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N:  return DXGI_FORMAT_R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N:  return DXGI_FORMAT_R16G16B16A16_UNORM;
    case D3DDECLTYPE_FLOAT16_2: return DXGI_FORMAT_R16G16_FLOAT;
    case D3DDECLTYPE_FLOAT16_4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                     return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace dxmt::d3d9
