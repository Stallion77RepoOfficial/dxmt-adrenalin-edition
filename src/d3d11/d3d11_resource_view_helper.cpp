#include "dxmt_format.hpp"
#include "d3d11_resource.hpp"

namespace dxmt {

void FixDepthStencilFormat(Texture *pTexture, MTL_DXGI_FORMAT_DESC &Desc) {
  /* A swizzle the view asked for (a D3D9 depth format's channel layout)
   * survives the change of base format. */
  uint32_t swizzle = Desc.PixelFormat & WMTPixelFormatCustomSwizzle;
  switch (pTexture->pixelFormat()) {
  case WMTPixelFormatDepth16Unorm:
    // DXGI_FORMAT_R16_TYPELESS
    // DXGI_FORMAT_R16_UNORM
    Desc.PixelFormat = (WMTPixelFormat)(WMTPixelFormatDepth16Unorm | swizzle);
    break;
  case WMTPixelFormatDepth32Float:
    // DXGI_FORMAT_R32_TYPELESS
    // DXGI_FORMAT_R32_FLOAT
    Desc.PixelFormat = (WMTPixelFormat)(WMTPixelFormatDepth32Float | swizzle);
    break;
  default:
    break;
  }
}

template <>
HRESULT
InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture* pTexture,
    D3D11_SHADER_RESOURCE_VIEW_DESC1 &ViewDesc, TextureViewDescriptor &Descriptor
) {
  MTL_DXGI_FORMAT_DESC metal_format;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), ViewDesc.Format, metal_format))) {
    ERR("Failed to create SRV due to unsupported format ", ViewDesc.Format);
    return E_FAIL;
  }

  FixDepthStencilFormat(pTexture, metal_format);

  /* The same range check the render-target and depth-stencil paths carry. A
   * shader resource view differs in covering a run of mip levels rather than
   * one, so both ends of that run are checked. */
  {
    unsigned mip = 0, mips = 1, first_slice = 0, slices = 1;
    switch (ViewDesc.ViewDimension) {
    case D3D_SRV_DIMENSION_TEXTURE1D:
      if (~ViewDesc.Texture1D.MipLevels == 0)
        ViewDesc.Texture1D.MipLevels = MiplevelCount - ViewDesc.Texture1D.MostDetailedMip;
      mip = ViewDesc.Texture1D.MostDetailedMip, mips = ViewDesc.Texture1D.MipLevels;
      break;
    case D3D_SRV_DIMENSION_TEXTURE2D:
      if (~ViewDesc.Texture2D.MipLevels == 0)
        ViewDesc.Texture2D.MipLevels = MiplevelCount - ViewDesc.Texture2D.MostDetailedMip;
      mip = ViewDesc.Texture2D.MostDetailedMip, mips = ViewDesc.Texture2D.MipLevels;
      break;
    case D3D_SRV_DIMENSION_TEXTURE3D:
      if (~ViewDesc.Texture3D.MipLevels == 0)
        ViewDesc.Texture3D.MipLevels = MiplevelCount - ViewDesc.Texture3D.MostDetailedMip;
      mip = ViewDesc.Texture3D.MostDetailedMip, mips = ViewDesc.Texture3D.MipLevels;
      break;
    case D3D_SRV_DIMENSION_TEXTURECUBE:
      if (~ViewDesc.TextureCube.MipLevels == 0)
        ViewDesc.TextureCube.MipLevels = MiplevelCount - ViewDesc.TextureCube.MostDetailedMip;
      mip = ViewDesc.TextureCube.MostDetailedMip, mips = ViewDesc.TextureCube.MipLevels;
      slices = 6;
      break;
    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
      if (~ViewDesc.Texture1DArray.MipLevels == 0)
        ViewDesc.Texture1DArray.MipLevels = MiplevelCount - ViewDesc.Texture1DArray.MostDetailedMip;
      if (~ViewDesc.Texture1DArray.ArraySize == 0)
        ViewDesc.Texture1DArray.ArraySize = ArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
      mip = ViewDesc.Texture1DArray.MostDetailedMip, mips = ViewDesc.Texture1DArray.MipLevels;
      first_slice = ViewDesc.Texture1DArray.FirstArraySlice, slices = ViewDesc.Texture1DArray.ArraySize;
      break;
    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
      if (~ViewDesc.Texture2DArray.MipLevels == 0)
        ViewDesc.Texture2DArray.MipLevels = MiplevelCount - ViewDesc.Texture2DArray.MostDetailedMip;
      if (~ViewDesc.Texture2DArray.ArraySize == 0)
        ViewDesc.Texture2DArray.ArraySize = ArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
      mip = ViewDesc.Texture2DArray.MostDetailedMip, mips = ViewDesc.Texture2DArray.MipLevels;
      first_slice = ViewDesc.Texture2DArray.FirstArraySlice, slices = ViewDesc.Texture2DArray.ArraySize;
      break;
    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
      if (~ViewDesc.TextureCubeArray.MipLevels == 0)
        ViewDesc.TextureCubeArray.MipLevels = MiplevelCount - ViewDesc.TextureCubeArray.MostDetailedMip;
      if (~ViewDesc.TextureCubeArray.NumCubes == 0)
        ViewDesc.TextureCubeArray.NumCubes = (ArraySize - ViewDesc.TextureCubeArray.First2DArrayFace) / 6;
      mip = ViewDesc.TextureCubeArray.MostDetailedMip, mips = ViewDesc.TextureCubeArray.MipLevels;
      first_slice = ViewDesc.TextureCubeArray.First2DArrayFace;
      slices = ViewDesc.TextureCubeArray.NumCubes * 6;
      break;
    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
      if (~ViewDesc.Texture2DMSArray.ArraySize == 0)
        ViewDesc.Texture2DMSArray.ArraySize = ArraySize - ViewDesc.Texture2DMSArray.FirstArraySlice;
      first_slice = ViewDesc.Texture2DMSArray.FirstArraySlice, slices = ViewDesc.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
    if (!mips || mip >= MiplevelCount || mips > MiplevelCount - mip)
      return E_INVALIDARG;
    /* A 3D texture's slices are depth, which the view does not index. */
    if (ViewDesc.ViewDimension != D3D_SRV_DIMENSION_TEXTURE3D &&
        (!slices || first_slice >= ArraySize || slices > ArraySize - first_slice))
      return E_INVALIDARG;
  }

  WMTTextureType TextureType = pTexture->textureType();
  switch (ViewDesc.ViewDimension) {
  default:
    break;
  case D3D_SRV_DIMENSION_TEXTURE1D: {
    if (~ViewDesc.Texture1D.MipLevels == 0)
      ViewDesc.Texture1D.MipLevels = MiplevelCount - ViewDesc.Texture1D.MostDetailedMip;
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1D.MostDetailedMip;
      Descriptor.miplevelCount = ViewDesc.Texture1D.MipLevels;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE1DARRAY: {
    if (~ViewDesc.Texture1DArray.MipLevels == 0)
      ViewDesc.Texture1DArray.MipLevels = MiplevelCount - ViewDesc.Texture1DArray.MostDetailedMip;
    if (~ViewDesc.Texture1DArray.ArraySize == 0)
      ViewDesc.Texture1DArray.ArraySize = ArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1DArray.MostDetailedMip;
      Descriptor.miplevelCount = ViewDesc.Texture1DArray.MipLevels;
      Descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2D: {
    if (~ViewDesc.Texture2D.MipLevels == 0)
      ViewDesc.Texture2D.MipLevels = MiplevelCount - ViewDesc.Texture2D.MostDetailedMip;

    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray ||
        TextureType == WMTTextureTypeCube || TextureType == WMTTextureTypeCubeArray) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2D.MostDetailedMip;
      Descriptor.miplevelCount = ViewDesc.Texture2D.MipLevels;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2DARRAY: {
    if (~ViewDesc.Texture2DArray.MipLevels == 0)
      ViewDesc.Texture2DArray.MipLevels = MiplevelCount - ViewDesc.Texture2DArray.MostDetailedMip;
    if (~ViewDesc.Texture2DArray.ArraySize == 0)
      ViewDesc.Texture2DArray.ArraySize = ArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray ||
        TextureType == WMTTextureTypeCube || TextureType == WMTTextureTypeCubeArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2DArray.MostDetailedMip;
      Descriptor.miplevelCount = ViewDesc.Texture2DArray.MipLevels;
      Descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2DMS: {
    if (TextureType == WMTTextureType2DMultisample) {
      Descriptor.type = WMTTextureType2DMultisample;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray ||
        TextureType == WMTTextureTypeCube) {
      /* cursed use case: MS view on non-MS texture (SampleCount = 1). A
       * single-sample texture with more than one slice is a 2DArray and a
       * cube is a Cube, and a plain 2DMS view of either addresses the first
       * slice -- the same shapes the depth-stencil and render-target paths
       * accept. All three view kinds now agree. */
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: {
    if (~ViewDesc.Texture2DMSArray.ArraySize == 0)
      ViewDesc.Texture2DMSArray.ArraySize = ArraySize - ViewDesc.Texture2DMSArray.FirstArraySlice;
    if (TextureType == WMTTextureType2DMultisample) {
      Descriptor.type = WMTTextureType2DMultisampleArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    if (TextureType == WMTTextureType2DMultisampleArray) {
      Descriptor.type = WMTTextureType2DMultisampleArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize;
      return S_OK;
    }
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray ||
        TextureType == WMTTextureTypeCube) {
      // cursed use case: MS array view on a non-MS texture (SampleCount = 1)
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice =
          TextureType == WMTTextureType2D ? 0 : ViewDesc.Texture2DMSArray.FirstArraySlice;
      Descriptor.arraySize =
          TextureType == WMTTextureType2D ? 1 : ViewDesc.Texture2DMSArray.ArraySize;
      return S_OK;
    }
    if (TextureType == WMTTextureType2DArray) {
      // cursed use case: MS view on non-MS texture (SampleCount = 1)
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE3D: {
    if (~ViewDesc.Texture3D.MipLevels == 0)
      ViewDesc.Texture3D.MipLevels = MiplevelCount - ViewDesc.Texture3D.MostDetailedMip;
    if (TextureType == WMTTextureType3D) {
      Descriptor.type = WMTTextureType3D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture3D.MostDetailedMip;
      Descriptor.miplevelCount =ViewDesc.Texture3D.MipLevels;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURECUBE: {
    if (~ViewDesc.TextureCube.MipLevels == 0)
      ViewDesc.TextureCube.MipLevels = MiplevelCount - ViewDesc.TextureCube.MostDetailedMip;
    if (TextureType == WMTTextureTypeCube || TextureType == WMTTextureTypeCubeArray ||
        (TextureType == WMTTextureType2DArray && pTexture->width() == pTexture->height() && ArraySize >= 6)) {
      Descriptor.type = WMTTextureTypeCube;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.TextureCube.MostDetailedMip;
      Descriptor.miplevelCount = ~ViewDesc.TextureCube.MipLevels ? ViewDesc.TextureCube.MipLevels
                                                               : MiplevelCount - ViewDesc.TextureCube.MostDetailedMip;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 6;
      return S_OK;
    }
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: {
    D3D11_ASSERT((ViewDesc.TextureCubeArray.First2DArrayFace % 6) == 0);
    D3D11_ASSERT((ArraySize % 6) == 0);
    if (~ViewDesc.TextureCubeArray.MipLevels == 0)
      ViewDesc.TextureCubeArray.MipLevels = MiplevelCount - ViewDesc.TextureCubeArray.MostDetailedMip;
    if (~ViewDesc.TextureCubeArray.NumCubes == 0)
      ViewDesc.TextureCubeArray.NumCubes = (ArraySize - ViewDesc.TextureCubeArray.First2DArrayFace) / 6;
    if (TextureType == WMTTextureTypeCube || TextureType == WMTTextureTypeCubeArray ||
        (TextureType == WMTTextureType2DArray && pTexture->width() == pTexture->height() && ArraySize % 6 == 0)) {
      Descriptor.type = WMTTextureTypeCubeArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.TextureCubeArray.MostDetailedMip;
      Descriptor.miplevelCount = ViewDesc.TextureCubeArray.MipLevels;
      Descriptor.firstArraySlice = ViewDesc.TextureCubeArray.First2DArrayFace;
      Descriptor.arraySize = ViewDesc.TextureCubeArray.NumCubes * 6;
      return S_OK;
    }
    break;
  }
  }
  ERR("Unhandled srv creation: \n Source: ", uint32_t(TextureType), "\n Desired: ", ViewDesc.ViewDimension);
  return E_FAIL;
}

template <>
HRESULT
InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture *pTexture,
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 &ViewDesc, TextureViewDescriptor &Descriptor
) {
  MTL_DXGI_FORMAT_DESC metal_format;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), ViewDesc.Format,
                                metal_format))) {
    return E_FAIL;
  }

  FixDepthStencilFormat(pTexture, metal_format);
  WMTTextureType TextureType = pTexture->textureType();
  switch (ViewDesc.ViewDimension) {
  default:
    break;
  case D3D11_UAV_DIMENSION_TEXTURE1D: {
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE1DARRAY: {
    if (~ViewDesc.Texture1DArray.ArraySize == 0)
      ViewDesc.Texture1DArray.ArraySize = ArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize;
      return S_OK;
    }
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE2D: {
    D3D11_ASSERT(ViewDesc.Texture2D.PlaneSlice == 0);
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray ||
        TextureType == WMTTextureTypeCube) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE2DARRAY: {
    if (~ViewDesc.Texture2DArray.ArraySize == 0)
      ViewDesc.Texture2DArray.ArraySize = ArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
    if (TextureType == WMTTextureType2D || TextureType == WMTTextureType2DArray ||
        TextureType == WMTTextureTypeCube || TextureType == WMTTextureTypeCubeArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize;
      return S_OK;
    }
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE3D: {
    ArraySize = std::max(ArraySize >> ViewDesc.Texture3D.MipSlice, 1u); 
    if (~ViewDesc.Texture3D.WSize == 0)
      ViewDesc.Texture3D.WSize = ArraySize - ViewDesc.Texture3D.FirstWSlice;
    if (TextureType == WMTTextureType3D) {
      if (ViewDesc.Texture3D.FirstWSlice >= ArraySize || ViewDesc.Texture3D.WSize > ArraySize - ViewDesc.Texture3D.FirstWSlice)
        return E_INVALIDARG;
      /* A Metal view of a 3D texture always spans all of its slices, so a
       * view of some of them is given all of them -- addressed from slice 0,
       * not FirstWSlice -- rather than refused. */
      if (ViewDesc.Texture3D.FirstWSlice != 0 || ViewDesc.Texture3D.WSize != ArraySize)
        WARN("UAV of 3D texture slices ", ViewDesc.Texture3D.FirstWSlice, "+", ViewDesc.Texture3D.WSize, " of ",
             ArraySize, " covers all of them");
      Descriptor.type = WMTTextureType3D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture3D.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      return S_OK;
    }
    break;
  }
  }
  ERR("Unhandled uav creation: \n Source: ", TextureType,
      "\n Desired: ", ViewDesc.ViewDimension);
  return E_FAIL;
}

inline void
GetRenderTargetSize(Texture *pTexture, uint32_t level, MTL_RENDER_PASS_ATTACHMENT_DESC &AttachmentDesc) {
  AttachmentDesc.Width = std::max(1u, pTexture->width() >> level);
  AttachmentDesc.Height = std::max(1u, pTexture->height() >> level);
}

template <>
HRESULT InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture* pTexture,
    D3D11_RENDER_TARGET_VIEW_DESC1 &ViewDesc, MTL_RENDER_PASS_ATTACHMENT_DESC &AttachmentDesc, TextureViewDescriptor &Descriptor
) {
  /* A render target view names a typed format. UNKNOWN is filled in from the
   * resource, so it arrives still typeless when the texture is, and there is
   * nothing to render as. */
  switch (ViewDesc.Format) {
  case DXGI_FORMAT_UNKNOWN:
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32_TYPELESS:
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R32G32_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R16G16_TYPELESS:
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_R8G8_TYPELESS:
  case DXGI_FORMAT_R16_TYPELESS:
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8X8_TYPELESS:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_R24G8_TYPELESS:
    return E_INVALIDARG;
  default:
    break;
  }

  MTL_DXGI_FORMAT_DESC metal_format;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), ViewDesc.Format,
                                metal_format))) {
    return E_FAIL;
  }

  /* And it has to name a range the texture has. Widening the multisample
   * branches without this made the helper accept descriptors D3D11 rejects:
   * the suite's invalid-descriptor table then got nineteen views back that it
   * never releases, and the device it holds a reference on outlived the test.
   * The depth-stencil path was given the same check earlier. */
  {
    unsigned mip = 0, first_slice = 0, slices = 1;
    switch (ViewDesc.ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE1D:
      mip = ViewDesc.Texture1D.MipSlice;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2D:
      mip = ViewDesc.Texture2D.MipSlice;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
      if (~ViewDesc.Texture1DArray.ArraySize == 0)
        ViewDesc.Texture1DArray.ArraySize = ArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
      mip = ViewDesc.Texture1DArray.MipSlice;
      first_slice = ViewDesc.Texture1DArray.FirstArraySlice;
      slices = ViewDesc.Texture1DArray.ArraySize;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
      if (~ViewDesc.Texture2DArray.ArraySize == 0)
        ViewDesc.Texture2DArray.ArraySize = ArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
      mip = ViewDesc.Texture2DArray.MipSlice;
      first_slice = ViewDesc.Texture2DArray.FirstArraySlice;
      slices = ViewDesc.Texture2DArray.ArraySize;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
      if (~ViewDesc.Texture2DMSArray.ArraySize == 0)
        ViewDesc.Texture2DMSArray.ArraySize = ArraySize - ViewDesc.Texture2DMSArray.FirstArraySlice;
      first_slice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      slices = ViewDesc.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
    /* A 3D view indexes depth slices rather than array slices, and its own
     * branch below checks that range against the mip it applies to. */
    if (ViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE3D &&
        (mip >= MiplevelCount || !slices || first_slice >= ArraySize ||
         slices > ArraySize - first_slice))
      return E_INVALIDARG;
    if (ViewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE3D &&
        ViewDesc.Texture3D.MipSlice >= MiplevelCount)
      return E_INVALIDARG;
  }

  AttachmentDesc.DepthPlane = 0;
  AttachmentDesc.RenderTargetArrayLength = 0;
  AttachmentDesc.SampleCount = pTexture->sampleCount();

  auto texture_type = pTexture->textureType();
  switch (ViewDesc.ViewDimension) {
  default:
    break;
  case D3D11_RTV_DIMENSION_TEXTURE1D: {
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: {
    if (~ViewDesc.Texture1DArray.ArraySize == 0)
      ViewDesc.Texture1DArray.ArraySize = ArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2D: {
    D3D11_ASSERT(ViewDesc.Texture2D.PlaneSlice == 0);
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray ||
        texture_type == WMTTextureTypeCube || texture_type == WMTTextureTypeCubeArray) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: {
    if (~ViewDesc.Texture2DArray.ArraySize == 0)
      ViewDesc.Texture2DArray.ArraySize = ArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray ||
        texture_type == WMTTextureTypeCube || texture_type == WMTTextureTypeCubeArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2DMS: {
    if (texture_type == WMTTextureType2DMultisample) {
      Descriptor.type = WMTTextureType2DMultisample;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray ||
        texture_type == WMTTextureTypeCube) {
      /* cursed use case: MS view on non-MS texture (SampleCount = 1). An
       * array or a cube lands here too -- a single-sample texture created
       * with more than one slice is a 2DArray, and a plain 2DMS view of it
       * addresses the first. The depth-stencil path already accepted all
       * three; leaving them out here failed the same descriptors on a colour
       * target that worked on a depth one. */
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: {
    if (~ViewDesc.Texture2DMSArray.ArraySize == 0)
      ViewDesc.Texture2DMSArray.ArraySize = ArraySize - ViewDesc.Texture2DMSArray.FirstArraySlice;
    if (texture_type == WMTTextureType2DMultisample) {
      Descriptor.type = WMTTextureType2DMultisampleArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0; // FIXME: we only handle 2dms here!
      Descriptor.arraySize = 1;
      AttachmentDesc.RenderTargetArrayLength = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    if (texture_type == WMTTextureType2DMultisampleArray) {
      Descriptor.type = WMTTextureType2DMultisampleArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    if (texture_type == WMTTextureType2DArray || texture_type == WMTTextureType2D ||
        texture_type == WMTTextureTypeCube) {
      // cursed use case: MS array view on a non-MS texture (SampleCount = 1),
      // which is a plain 2D when it has a single slice.
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE3D: {
    ArraySize = std::max(ArraySize >> ViewDesc.Texture3D.MipSlice, 1u); 
    if (~ViewDesc.Texture3D.WSize == 0)
      ViewDesc.Texture3D.WSize = ArraySize - ViewDesc.Texture3D.FirstWSlice;
    if (texture_type == WMTTextureType3D) {
      if (ViewDesc.Texture3D.WSize == 1) {
        Descriptor.type = WMTTextureType3D;
        Descriptor.format = metal_format.PixelFormat;
        Descriptor.firstMiplevel = ViewDesc.Texture3D.MipSlice;
        Descriptor.miplevelCount = 1;
        Descriptor.firstArraySlice = 0;
        Descriptor.arraySize = 1;
        AttachmentDesc.DepthPlane = ViewDesc.Texture3D.FirstWSlice;
        GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
        return S_OK;
      }
      /* A run of depth slices. Metal expresses exactly this on a 3D
       * attachment: depthPlane is where the run starts and
       * renderTargetArrayLength is how many layers it covers. The two were
       * only ever set one at a time here -- a single plane, or the whole
       * texture from slice zero -- so a view starting partway through with
       * more than one slice fell out of the switch and failed. */
      if (ViewDesc.Texture3D.FirstWSlice + ViewDesc.Texture3D.WSize <= ArraySize) {
        Descriptor.type = WMTTextureType3D;
        Descriptor.format = metal_format.PixelFormat;
        Descriptor.firstMiplevel = ViewDesc.Texture3D.MipSlice;
        Descriptor.miplevelCount = 1;
        Descriptor.firstArraySlice = 0;
        Descriptor.arraySize = 1;
        AttachmentDesc.DepthPlane = ViewDesc.Texture3D.FirstWSlice;
        AttachmentDesc.RenderTargetArrayLength = ViewDesc.Texture3D.WSize;
        GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
        return S_OK;
      }
      ERR("tex3d rtv creation not properly handled: ", ViewDesc.Texture3D.FirstWSlice, ":", ViewDesc.Texture3D.WSize,
          ":", ArraySize);
    }
    break;
  }
  }
  ERR("Unhandled rtv creation: \n Source: ", uint32_t(texture_type),
      "\n Desired: ", ViewDesc.ViewDimension);
  return E_FAIL;
}

template <>
HRESULT
InitializeAndNormalizeViewDescriptor(
    MTLD3D11Device *pDevice, unsigned MiplevelCount, unsigned ArraySize, Texture *pTexture,
    D3D11_DEPTH_STENCIL_VIEW_DESC &ViewDesc, MTL_RENDER_PASS_ATTACHMENT_DESC &AttachmentDesc,
    TextureViewDescriptor &Descriptor
) {
  /* A depth-stencil view has to name a fully typed depth format. UNKNOWN gets
   * filled in from the resource, so it arrives here still typeless when the
   * texture itself is typeless -- and there is nothing to infer from. */
  switch (ViewDesc.Format) {
  case DXGI_FORMAT_UNKNOWN:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_R16_TYPELESS:
    return E_INVALIDARG;
  default:
    break;
  }

  MTL_DXGI_FORMAT_DESC metal_format;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), ViewDesc.Format, metal_format))) {
    ERR("Failed to create DSV due to unsupported format ", ViewDesc.Format);
    return E_FAIL;
  }

  /* The view has to name a range the texture actually has. Normalising the
   * ~0 array size first, because that means "the rest of the texture" and is
   * only in range once expanded. Without this the out-of-range descriptors
   * D3D11 rejects reached Metal, which either faulted or silently produced a
   * view onto slices that are not there. */
  {
    unsigned mip = 0, first_slice = 0, slices = 1;
    switch (ViewDesc.ViewDimension) {
    case D3D11_DSV_DIMENSION_TEXTURE1D:
      mip = ViewDesc.Texture1D.MipSlice;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE2D:
      mip = ViewDesc.Texture2D.MipSlice;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
      if (~ViewDesc.Texture1DArray.ArraySize == 0)
        ViewDesc.Texture1DArray.ArraySize = ArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
      mip = ViewDesc.Texture1DArray.MipSlice;
      first_slice = ViewDesc.Texture1DArray.FirstArraySlice;
      slices = ViewDesc.Texture1DArray.ArraySize;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
      if (~ViewDesc.Texture2DArray.ArraySize == 0)
        ViewDesc.Texture2DArray.ArraySize = ArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
      mip = ViewDesc.Texture2DArray.MipSlice;
      first_slice = ViewDesc.Texture2DArray.FirstArraySlice;
      slices = ViewDesc.Texture2DArray.ArraySize;
      break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
      if (~ViewDesc.Texture2DMSArray.ArraySize == 0)
        ViewDesc.Texture2DMSArray.ArraySize = ArraySize - ViewDesc.Texture2DMSArray.FirstArraySlice;
      first_slice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      slices = ViewDesc.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
    if (mip >= MiplevelCount || !slices || first_slice >= ArraySize ||
        slices > ArraySize - first_slice)
      return E_INVALIDARG;
  }

  ViewDesc.Flags = ViewDesc.Flags & 3;
  AttachmentDesc.DepthPlane = 0;
  AttachmentDesc.RenderTargetArrayLength = 0;
  AttachmentDesc.SampleCount = pTexture->sampleCount();

  auto texture_type = pTexture->textureType();
  switch (ViewDesc.ViewDimension) {
  default:
    break;
  case D3D11_DSV_DIMENSION_TEXTURE1D: {
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_DSV_DIMENSION_TEXTURE1DARRAY: {
    if (~ViewDesc.Texture1DArray.ArraySize == 0)
      ViewDesc.Texture1DArray.ArraySize = ArraySize - ViewDesc.Texture1DArray.FirstArraySlice;
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture1DArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_DSV_DIMENSION_TEXTURE2D: {
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray ||
        texture_type == WMTTextureTypeCube || texture_type == WMTTextureTypeCubeArray) {
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_DSV_DIMENSION_TEXTURE2DARRAY: {
    if (~ViewDesc.Texture2DArray.ArraySize == 0)
      ViewDesc.Texture2DArray.ArraySize = ArraySize - ViewDesc.Texture2DArray.FirstArraySlice;
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray ||
        texture_type == WMTTextureTypeCube || texture_type == WMTTextureTypeCubeArray) {
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_DSV_DIMENSION_TEXTURE2DMS: {
    if (texture_type == WMTTextureType2DMultisample || texture_type == WMTTextureType2DMultisampleArray) {
      Descriptor.type = WMTTextureType2DMultisample;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    if (texture_type == WMTTextureType2D || texture_type == WMTTextureType2DArray ||
        texture_type == WMTTextureTypeCube) {
      // cursed use case: MS view on non-MS texture (SampleCount = 1)
      Descriptor.type = WMTTextureType2D;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY: {
    if (~ViewDesc.Texture2DMSArray.ArraySize == 0)
      ViewDesc.Texture2DMSArray.ArraySize = ArraySize - ViewDesc.Texture2DMSArray.FirstArraySlice;
    if (texture_type == WMTTextureType2DMultisample) {
      Descriptor.type = WMTTextureType2DMultisampleArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = 0;
      Descriptor.arraySize = 1;
      AttachmentDesc.RenderTargetArrayLength = 1;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    if (texture_type == WMTTextureType2DMultisampleArray) {
      Descriptor.type = WMTTextureType2DMultisampleArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    if (texture_type == WMTTextureType2DArray || texture_type == WMTTextureType2D ||
        texture_type == WMTTextureTypeCube) {
      // cursed use case: MS view on non-MS texture (SampleCount = 1). A
      // single-slice texture lands here as a plain 2D rather than a 2DArray,
      // and the 2DMS branch above already accepts the same shapes -- leaving
      // them out here failed every MS-array view of an ordinary depth buffer.
      Descriptor.type = WMTTextureType2DArray;
      Descriptor.format = metal_format.PixelFormat;
      Descriptor.firstMiplevel = 0;
      Descriptor.miplevelCount = 1;
      Descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      Descriptor.arraySize = ViewDesc.Texture2DMSArray.ArraySize;
      AttachmentDesc.RenderTargetArrayLength = Descriptor.arraySize;
      GetRenderTargetSize(pTexture, Descriptor.firstMiplevel, AttachmentDesc);
      return S_OK;
    }
    break;
  }
  }
  ERR("Unhandled dsv creation: \n Source: ", uint32_t(texture_type), "\n Desired: ", ViewDesc.ViewDimension);
  return E_FAIL;
}

template <>
void DowngradeViewDescription(const D3D11_SHADER_RESOURCE_VIEW_DESC1 &src,
                              D3D11_SHADER_RESOURCE_VIEW_DESC *pDst) {
  pDst->ViewDimension = src.ViewDimension;
  pDst->Format = src.Format;
  switch (src.ViewDimension) {
  case D3D_SRV_DIMENSION_UNKNOWN:
    break;
  case D3D_SRV_DIMENSION_BUFFER:
    pDst->Buffer = src.Buffer;
    break;
  case D3D_SRV_DIMENSION_TEXTURE1D:
    pDst->Texture1D = src.Texture1D;
    break;
  case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
    pDst->Texture1DArray = src.Texture1DArray;
    break;
  case D3D_SRV_DIMENSION_TEXTURE2D: {
    pDst->Texture2D.MipLevels = src.Texture2D.MipLevels;
    pDst->Texture2D.MostDetailedMip = src.Texture2D.MostDetailedMip;
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2DARRAY: {
    pDst->Texture2DArray.ArraySize = src.Texture2DArray.ArraySize;
    pDst->Texture2DArray.FirstArraySlice = src.Texture2DArray.FirstArraySlice;
    pDst->Texture2DArray.MipLevels = src.Texture2DArray.MipLevels;
    pDst->Texture2DArray.MostDetailedMip = src.Texture2DArray.MostDetailedMip;
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2DMS:
    pDst->Texture2DMS = src.Texture2DMS;
    break;
  case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
    pDst->Texture2DMSArray = src.Texture2DMSArray;
    break;
  case D3D_SRV_DIMENSION_TEXTURE3D:
    pDst->Texture3D = src.Texture3D;
    break;
  case D3D_SRV_DIMENSION_TEXTURECUBE:
    pDst->TextureCube = src.TextureCube;
    break;
  case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
    pDst->TextureCubeArray = src.TextureCubeArray;
    break;
  case D3D_SRV_DIMENSION_BUFFEREX:
    pDst->BufferEx = src.BufferEx;
    break;
  }
}

template <>
void DowngradeViewDescription(const D3D11_RENDER_TARGET_VIEW_DESC1 &src,
                              D3D11_RENDER_TARGET_VIEW_DESC *pDst) {
  pDst->ViewDimension = src.ViewDimension;
  pDst->Format = src.Format;
  switch (src.ViewDimension) {
  case D3D11_RTV_DIMENSION_UNKNOWN:
    break;
  case D3D11_RTV_DIMENSION_BUFFER:
    pDst->Buffer = src.Buffer;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE1D:
    pDst->Texture1D = src.Texture1D;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
    pDst->Texture1DArray = src.Texture1DArray;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE2D: {
    pDst->Texture2D.MipSlice = src.Texture2D.MipSlice;
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: {
    pDst->Texture2DArray.ArraySize = src.Texture2DArray.ArraySize;
    pDst->Texture2DArray.FirstArraySlice = src.Texture2DArray.FirstArraySlice;
    pDst->Texture2DArray.MipSlice = src.Texture2DArray.MipSlice;
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2DMS:
    pDst->Texture2DMS = src.Texture2DMS;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
    pDst->Texture2DMSArray = src.Texture2DMSArray;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE3D:
    pDst->Texture3D = src.Texture3D;
    break;
  }
}

template <>
void DowngradeViewDescription(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 &src,
                              D3D11_UNORDERED_ACCESS_VIEW_DESC *pDst) {
  pDst->ViewDimension = src.ViewDimension;
  pDst->Format = src.Format;
  switch (src.ViewDimension) {
  case D3D11_UAV_DIMENSION_UNKNOWN:
    break;
  case D3D11_UAV_DIMENSION_BUFFER:
    pDst->Buffer = src.Buffer;
    break;
  case D3D11_UAV_DIMENSION_TEXTURE1D:
    pDst->Texture1D = src.Texture1D;
    break;
  case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
    pDst->Texture1DArray = src.Texture1DArray;
    break;
  case D3D11_UAV_DIMENSION_TEXTURE2D: {
    pDst->Texture2D.MipSlice = src.Texture2D.MipSlice;
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE2DARRAY: {
    pDst->Texture2DArray.MipSlice = src.Texture2DArray.MipSlice;
    pDst->Texture2DArray.FirstArraySlice = src.Texture2DArray.FirstArraySlice;
    pDst->Texture2DArray.ArraySize = src.Texture2DArray.ArraySize;
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE3D:
    pDst->Texture3D = src.Texture3D;
    break;
  }
}

template <>
void UpgradeViewDescription(const D3D11_SHADER_RESOURCE_VIEW_DESC *pSrc,
                            D3D11_SHADER_RESOURCE_VIEW_DESC1 &dst) {
  dst.Format = pSrc->Format;
  dst.ViewDimension = pSrc->ViewDimension;
  switch (pSrc->ViewDimension) {
  case D3D_SRV_DIMENSION_UNKNOWN:
    break;
  case D3D_SRV_DIMENSION_BUFFER:
    dst.Buffer = pSrc->Buffer;
    break;
  case D3D_SRV_DIMENSION_TEXTURE1D:
    dst.Texture1D = pSrc->Texture1D;
    break;
  case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
    dst.Texture1DArray = pSrc->Texture1DArray;
    break;
  case D3D_SRV_DIMENSION_TEXTURE2D: {
    dst.Texture2D.MipLevels = pSrc->Texture2D.MipLevels;
    dst.Texture2D.MostDetailedMip = pSrc->Texture2D.MostDetailedMip;
    dst.Texture2D.PlaneSlice = 0;
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2DARRAY: {
    dst.Texture2DArray.MipLevels = pSrc->Texture2DArray.MipLevels;
    dst.Texture2DArray.MostDetailedMip = pSrc->Texture2DArray.MostDetailedMip;
    dst.Texture2DArray.ArraySize = pSrc->Texture2DArray.ArraySize;
    dst.Texture2DArray.FirstArraySlice = pSrc->Texture2DArray.FirstArraySlice;
    dst.Texture2DArray.PlaneSlice = 0;
    break;
  }
  case D3D_SRV_DIMENSION_TEXTURE2DMS:
    dst.Texture2DMS = pSrc->Texture2DMS;
    break;
  case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
    dst.Texture2DMSArray = pSrc->Texture2DMSArray;
    break;
  case D3D_SRV_DIMENSION_TEXTURE3D:
    dst.Texture3D = pSrc->Texture3D;
    break;
  case D3D_SRV_DIMENSION_TEXTURECUBE:
    dst.TextureCube = pSrc->TextureCube;
    break;
  case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
    dst.TextureCubeArray = pSrc->TextureCubeArray;
    break;
  case D3D_SRV_DIMENSION_BUFFEREX:
    dst.BufferEx = pSrc->BufferEx;
    break;
  }
};

template <>
void UpgradeViewDescription(const D3D11_UNORDERED_ACCESS_VIEW_DESC *pSrc,
                            D3D11_UNORDERED_ACCESS_VIEW_DESC1 &dst) {
  dst.Format = pSrc->Format;
  dst.ViewDimension = pSrc->ViewDimension;
  switch (pSrc->ViewDimension) {
  case D3D11_UAV_DIMENSION_UNKNOWN:
    break;
  case D3D11_UAV_DIMENSION_BUFFER:
    dst.Buffer = pSrc->Buffer;
    break;
  case D3D11_UAV_DIMENSION_TEXTURE1D:
    dst.Texture1D = pSrc->Texture1D;
    break;
  case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
    dst.Texture1DArray = pSrc->Texture1DArray;
    break;
  case D3D11_UAV_DIMENSION_TEXTURE2D: {
    dst.Texture2D.MipSlice = pSrc->Texture2D.MipSlice;
    dst.Texture2D.PlaneSlice = 0;
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE2DARRAY: {
    dst.Texture2DArray.MipSlice = pSrc->Texture2DArray.MipSlice;
    dst.Texture2DArray.ArraySize = pSrc->Texture2DArray.ArraySize;
    dst.Texture2DArray.FirstArraySlice = pSrc->Texture2DArray.FirstArraySlice;
    dst.Texture2DArray.PlaneSlice = 0;
    break;
  }
  case D3D11_UAV_DIMENSION_TEXTURE3D:
    dst.Texture3D = pSrc->Texture3D;
    break;
  }
};

template <>
void UpgradeViewDescription(const D3D11_RENDER_TARGET_VIEW_DESC *pSrc,
                            D3D11_RENDER_TARGET_VIEW_DESC1 &dst) {
  dst.Format = pSrc->Format;
  dst.ViewDimension = pSrc->ViewDimension;
  switch (pSrc->ViewDimension) {
  case D3D11_RTV_DIMENSION_UNKNOWN:
    break;
  case D3D11_RTV_DIMENSION_BUFFER:
    dst.Buffer = pSrc->Buffer;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE1D:
    dst.Texture1D = pSrc->Texture1D;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
    dst.Texture1DArray = pSrc->Texture1DArray;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE2D: {
    dst.Texture2D.MipSlice = pSrc->Texture2D.MipSlice;
    dst.Texture2D.PlaneSlice = 0;
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: {
    dst.Texture2DArray.MipSlice = pSrc->Texture2DArray.MipSlice;
    dst.Texture2DArray.ArraySize = pSrc->Texture2DArray.ArraySize;
    dst.Texture2DArray.FirstArraySlice = pSrc->Texture2DArray.FirstArraySlice;
    dst.Texture2DArray.PlaneSlice = 0;
    break;
  }
  case D3D11_RTV_DIMENSION_TEXTURE2DMS:
    dst.Texture2DMS = pSrc->Texture2DMS;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
    dst.Texture2DMSArray = pSrc->Texture2DMSArray;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE3D:
    dst.Texture3D = pSrc->Texture3D;
    break;
  }
};

} // namespace dxmt
