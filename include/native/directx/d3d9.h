#pragma once

#include "windows.h"
#include "unknwn.h"

#ifdef __cplusplus
extern "C" {
#endif

// D3D9 Types
typedef DWORD D3DCOLOR;
typedef float D3DVALUE;
typedef unsigned long D3DFIXED;

#define D3DCOLOR_ARGB(a,r,g,b) ((D3DCOLOR)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))
#define D3DCOLOR_RGBA(r,g,b,a) D3DCOLOR_ARGB(a,r,g,b)
#define D3DCOLOR_XRGB(r,g,b)   D3DCOLOR_ARGB(0xff,r,g,b)
#define D3DCOLOR_XYUV(y,u,v)   D3DCOLOR_ARGB(0xff,y,u,v)
#define D3DCOLOR_AYUV(a,y,u,v) D3DCOLOR_ARGB(a,y,u,v)
#define D3DCOLOR_COLORVALUE(r,g,b,a) D3DCOLOR_RGBA((DWORD)((r)*255.f),(DWORD)((g)*255.f),(DWORD)((b)*255.f),(DWORD)((a)*255.f))

// HRESULT codes
#define D3D_OK                          S_OK
#define D3DERR_WRONGTEXTUREFORMAT       MAKE_HRESULT(1,0x876,2072)
#define D3DERR_UNSUPPORTEDCOLOROPERATION MAKE_HRESULT(1,0x876,2073)
#define D3DERR_UNSUPPORTEDCOLORARG      MAKE_HRESULT(1,0x876,2074)
#define D3DERR_UNSUPPORTEDALPHAOPERATION MAKE_HRESULT(1,0x876,2075)
#define D3DERR_UNSUPPORTEDALPHAARG      MAKE_HRESULT(1,0x876,2076)
#define D3DERR_TOOMANYOPERATIONS        MAKE_HRESULT(1,0x876,2077)
#define D3DERR_CONFLICTINGTEXTUREFILTER MAKE_HRESULT(1,0x876,2078)
#define D3DERR_UNSUPPORTEDFACTORVALUE   MAKE_HRESULT(1,0x876,2079)
#define D3DERR_CONFLICTINGRENDERSTATE   MAKE_HRESULT(1,0x876,2081)
#define D3DERR_UNSUPPORTEDTEXTUREFILTER MAKE_HRESULT(1,0x876,2082)
#define D3DERR_CONFLICTINGTEXTUREPALETTE MAKE_HRESULT(1,0x876,2086)
#define D3DERR_DRIVERINTERNALERROR      MAKE_HRESULT(1,0x876,2087)
#define D3DERR_NOTFOUND                 MAKE_HRESULT(1,0x876,2150)
#define D3DERR_MOREDATA                 MAKE_HRESULT(1,0x876,2151)
#define D3DERR_DEVICELOST               MAKE_HRESULT(1,0x876,2152)
#define D3DERR_DEVICENOTRESET           MAKE_HRESULT(1,0x876,2153)
#define D3DERR_NOTAVAILABLE             MAKE_HRESULT(1,0x876,2154)
#define D3DERR_OUTOFVIDEOMEMORY         MAKE_HRESULT(1,0x876,380)
#define D3DERR_INVALIDDEVICE            MAKE_HRESULT(1,0x876,2155)
#define D3DERR_INVALIDCALL              MAKE_HRESULT(1,0x876,2156)
#define D3DERR_DRIVERINVALIDCALL        MAKE_HRESULT(1,0x876,2157)
#define D3DERR_WASSTILLDRAWING          MAKE_HRESULT(1,0x876,540)
#define D3DOK_NOAUTOGEN                 MAKE_HRESULT(0,0x876,2159)
#define D3DERR_DEVICEREMOVED            MAKE_HRESULT(1,0x876,2160)
#define S_NOT_RESIDENT                  MAKE_HRESULT(0,0x876,2165)
#define S_RESIDENT_IN_SHARED_MEMORY     MAKE_HRESULT(0,0x876,2166)
#define S_PRESENT_MODE_CHANGED          MAKE_HRESULT(0,0x876,2167)
#define S_PRESENT_OCCLUDED              MAKE_HRESULT(0,0x876,2168)
#define D3DERR_PRESENT_STATISTICS_DISJOINT MAKE_HRESULT(1,0x876,2180)

// D3D9 Enumerations
typedef enum _D3DDEVTYPE {
    D3DDEVTYPE_HAL         = 1,
    D3DDEVTYPE_REF         = 2,
    D3DDEVTYPE_SW          = 3,
    D3DDEVTYPE_NULLREF     = 4,
    D3DDEVTYPE_FORCE_DWORD = 0x7fffffff
} D3DDEVTYPE;

typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN              =  0,
    D3DFMT_R8G8B8               = 20,
    D3DFMT_A8R8G8B8             = 21,
    D3DFMT_X8R8G8B8             = 22,
    D3DFMT_R5G6B5               = 23,
    D3DFMT_X1R5G5B5             = 24,
    D3DFMT_A1R5G5B5             = 25,
    D3DFMT_A4R4G4B4             = 26,
    D3DFMT_R3G3B2               = 27,
    D3DFMT_A8                   = 28,
    D3DFMT_A8R3G3B2             = 29,
    D3DFMT_X4R4G4B4             = 30,
    D3DFMT_A2B10G10R10          = 31,
    D3DFMT_A8B8G8R8             = 32,
    D3DFMT_X8B8G8R8             = 33,
    D3DFMT_G16R16               = 34,
    D3DFMT_A2R10G10B10          = 35,
    D3DFMT_A16B16G16R16         = 36,
    D3DFMT_A8P8                 = 40,
    D3DFMT_P8                   = 41,
    D3DFMT_L8                   = 50,
    D3DFMT_A8L8                 = 51,
    D3DFMT_A4L4                 = 52,
    D3DFMT_V8U8                 = 60,
    D3DFMT_L6V5U5               = 61,
    D3DFMT_X8L8V8U8             = 62,
    D3DFMT_Q8W8V8U8             = 63,
    D3DFMT_V16U16               = 64,
    D3DFMT_A2W10V10U10          = 67,
    D3DFMT_UYVY                 = MAKEFOURCC('U', 'Y', 'V', 'Y'),
    D3DFMT_R8G8_B8G8            = MAKEFOURCC('R', 'G', 'B', 'G'),
    D3DFMT_YUY2                 = MAKEFOURCC('Y', 'U', 'Y', '2'),
    D3DFMT_G8R8_G8B8            = MAKEFOURCC('G', 'R', 'G', 'B'),
    D3DFMT_DXT1                 = MAKEFOURCC('D', 'X', 'T', '1'),
    D3DFMT_DXT2                 = MAKEFOURCC('D', 'X', 'T', '2'),
    D3DFMT_DXT3                 = MAKEFOURCC('D', 'X', 'T', '3'),
    D3DFMT_DXT4                 = MAKEFOURCC('D', 'X', 'T', '4'),
    D3DFMT_DXT5                 = MAKEFOURCC('D', 'X', 'T', '5'),
    D3DFMT_D16_LOCKABLE         = 70,
    D3DFMT_D32                  = 71,
    D3DFMT_D15S1                = 73,
    D3DFMT_D24S8                = 75,
    D3DFMT_D24X8                = 77,
    D3DFMT_D24X4S4              = 79,
    D3DFMT_D16                  = 80,
    D3DFMT_D32F_LOCKABLE        = 82,
    D3DFMT_D24FS8               = 83,
    D3DFMT_D32_LOCKABLE         = 84,
    D3DFMT_S8_LOCKABLE          = 85,
    D3DFMT_L16                  = 81,
    D3DFMT_VERTEXDATA           = 100,
    D3DFMT_INDEX16              = 101,
    D3DFMT_INDEX32              = 102,
    D3DFMT_Q16W16V16U16         = 110,
    D3DFMT_MULTI2_ARGB8         = MAKEFOURCC('M','E','T','1'),
    D3DFMT_R16F                 = 111,
    D3DFMT_G16R16F              = 112,
    D3DFMT_A16B16G16R16F        = 113,
    D3DFMT_R32F                 = 114,
    D3DFMT_G32R32F              = 115,
    D3DFMT_A32B32G32R32F        = 116,
    D3DFMT_CxV8U8               = 117,
    D3DFMT_A1                   = 118,
    D3DFMT_A2B10G10R10_XR_BIAS  = 119,
    D3DFMT_BINARYBUFFER         = 199,
    D3DFMT_FORCE_DWORD          = 0x7fffffff
} D3DFORMAT;

typedef enum _D3DPOOL {
    D3DPOOL_DEFAULT     = 0,
    D3DPOOL_MANAGED     = 1,
    D3DPOOL_SYSTEMMEM   = 2,
    D3DPOOL_SCRATCH     = 3,
    D3DPOOL_FORCE_DWORD = 0x7fffffff
} D3DPOOL;

typedef enum _D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE            =  0,
    D3DMULTISAMPLE_NONMASKABLE     =  1,
    D3DMULTISAMPLE_2_SAMPLES       =  2,
    D3DMULTISAMPLE_3_SAMPLES       =  3,
    D3DMULTISAMPLE_4_SAMPLES       =  4,
    D3DMULTISAMPLE_5_SAMPLES       =  5,
    D3DMULTISAMPLE_6_SAMPLES       =  6,
    D3DMULTISAMPLE_7_SAMPLES       =  7,
    D3DMULTISAMPLE_8_SAMPLES       =  8,
    D3DMULTISAMPLE_9_SAMPLES       =  9,
    D3DMULTISAMPLE_10_SAMPLES      = 10,
    D3DMULTISAMPLE_11_SAMPLES      = 11,
    D3DMULTISAMPLE_12_SAMPLES      = 12,
    D3DMULTISAMPLE_13_SAMPLES      = 13,
    D3DMULTISAMPLE_14_SAMPLES      = 14,
    D3DMULTISAMPLE_15_SAMPLES      = 15,
    D3DMULTISAMPLE_16_SAMPLES      = 16,
    D3DMULTISAMPLE_FORCE_DWORD     = 0x7fffffff
} D3DMULTISAMPLE_TYPE;

typedef enum _D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD      = 1,
    D3DSWAPEFFECT_FLIP         = 2,
    D3DSWAPEFFECT_COPY         = 3,
    D3DSWAPEFFECT_OVERLAY      = 4,
    D3DSWAPEFFECT_FLIPEX       = 5,
    D3DSWAPEFFECT_FORCE_DWORD  = 0x7fffffff
} D3DSWAPEFFECT;

typedef enum _D3DRESOURCETYPE {
    D3DRTYPE_SURFACE        =  1,
    D3DRTYPE_VOLUME         =  2,
    D3DRTYPE_TEXTURE        =  3,
    D3DRTYPE_VOLUMETEXTURE  =  4,
    D3DRTYPE_CUBETEXTURE    =  5,
    D3DRTYPE_VERTEXBUFFER   =  6,
    D3DRTYPE_INDEXBUFFER    =  7,
    D3DRTYPE_FORCE_DWORD    = 0x7fffffff
} D3DRESOURCETYPE;

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST     = 1,
    D3DPT_LINELIST      = 2,
    D3DPT_LINESTRIP     = 3,
    D3DPT_TRIANGLELIST  = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN   = 6,
    D3DPT_FORCE_DWORD   = 0x7fffffff
} D3DPRIMITIVETYPE;

typedef enum _D3DRENDERSTATETYPE {
    D3DRS_ZENABLE                   =   7,
    D3DRS_FILLMODE                  =   8,
    D3DRS_SHADEMODE                 =   9,
    D3DRS_ZWRITEENABLE              =  14,
    D3DRS_ALPHATESTENABLE           =  15,
    D3DRS_LASTPIXEL                 =  16,
    D3DRS_SRCBLEND                  =  19,
    D3DRS_DESTBLEND                 =  20,
    D3DRS_CULLMODE                  =  22,
    D3DRS_ZFUNC                     =  23,
    D3DRS_ALPHAREF                  =  24,
    D3DRS_ALPHAFUNC                 =  25,
    D3DRS_DITHERENABLE              =  26,
    D3DRS_ALPHABLENDENABLE          =  27,
    D3DRS_FOGENABLE                 =  28,
    D3DRS_SPECULARENABLE            =  29,
    D3DRS_FOGCOLOR                  =  34,
    D3DRS_FOGTABLEMODE              =  35,
    D3DRS_FOGSTART                  =  36,
    D3DRS_FOGEND                    =  37,
    D3DRS_FOGDENSITY                =  38,
    D3DRS_RANGEFOGENABLE            =  48,
    D3DRS_STENCILENABLE             =  52,
    D3DRS_STENCILFAIL               =  53,
    D3DRS_STENCILZFAIL              =  54,
    D3DRS_STENCILPASS               =  55,
    D3DRS_STENCILFUNC               =  56,
    D3DRS_STENCILREF                =  57,
    D3DRS_STENCILMASK               =  58,
    D3DRS_STENCILWRITEMASK          =  59,
    D3DRS_TEXTUREFACTOR             =  60,
    D3DRS_WRAP0                     = 128,
    D3DRS_WRAP1                     = 129,
    D3DRS_WRAP2                     = 130,
    D3DRS_WRAP3                     = 131,
    D3DRS_WRAP4                     = 132,
    D3DRS_WRAP5                     = 133,
    D3DRS_WRAP6                     = 134,
    D3DRS_WRAP7                     = 135,
    D3DRS_CLIPPING                  = 136,
    D3DRS_LIGHTING                  = 137,
    D3DRS_AMBIENT                   = 139,
    D3DRS_FOGVERTEXMODE             = 140,
    D3DRS_COLORVERTEX               = 141,
    D3DRS_LOCALVIEWER               = 142,
    D3DRS_NORMALIZENORMALS          = 143,
    D3DRS_DIFFUSEMATERIALSOURCE     = 145,
    D3DRS_SPECULARMATERIALSOURCE    = 146,
    D3DRS_AMBIENTMATERIALSOURCE     = 147,
    D3DRS_EMISSIVEMATERIALSOURCE    = 148,
    D3DRS_VERTEXBLEND               = 151,
    D3DRS_CLIPPLANEENABLE           = 152,
    D3DRS_POINTSIZE                 = 154,
    D3DRS_POINTSIZE_MIN             = 155,
    D3DRS_POINTSPRITEENABLE         = 156,
    D3DRS_POINTSCALEENABLE          = 157,
    D3DRS_POINTSCALE_A              = 158,
    D3DRS_POINTSCALE_B              = 159,
    D3DRS_POINTSCALE_C              = 160,
    D3DRS_MULTISAMPLEANTIALIAS      = 161,
    D3DRS_MULTISAMPLEMASK           = 162,
    D3DRS_PATCHEDGESTYLE            = 163,
    D3DRS_DEBUGMONITORTOKEN         = 165,
    D3DRS_POINTSIZE_MAX             = 166,
    D3DRS_INDEXEDVERTEXBLENDENABLE  = 167,
    D3DRS_COLORWRITEENABLE          = 168,
    D3DRS_TWEENFACTOR               = 170,
    D3DRS_BLENDOP                   = 171,
    D3DRS_POSITIONDEGREE            = 172,
    D3DRS_NORMALDEGREE              = 173,
    D3DRS_SCISSORTESTENABLE         = 174,
    D3DRS_SLOPESCALEDEPTHBIAS       = 175,
    D3DRS_ANTIALIASEDLINEENABLE     = 176,
    D3DRS_MINTESSELLATIONLEVEL      = 178,
    D3DRS_MAXTESSELLATIONLEVEL      = 179,
    D3DRS_ADAPTIVETESS_X            = 180,
    D3DRS_ADAPTIVETESS_Y            = 181,
    D3DRS_ADAPTIVETESS_Z            = 182,
    D3DRS_ADAPTIVETESS_W            = 183,
    D3DRS_ENABLEADAPTIVETESSELLATION= 184,
    D3DRS_TWOSIDEDSTENCILMODE       = 185,
    D3DRS_CCW_STENCILFAIL           = 186,
    D3DRS_CCW_STENCILZFAIL          = 187,
    D3DRS_CCW_STENCILPASS           = 188,
    D3DRS_CCW_STENCILFUNC           = 189,
    D3DRS_COLORWRITEENABLE1         = 190,
    D3DRS_COLORWRITEENABLE2         = 191,
    D3DRS_COLORWRITEENABLE3         = 192,
    D3DRS_BLENDFACTOR               = 193,
    D3DRS_SRGBWRITEENABLE           = 194,
    D3DRS_DEPTHBIAS                 = 195,
    D3DRS_WRAP8                     = 198,
    D3DRS_WRAP9                     = 199,
    D3DRS_WRAP10                    = 200,
    D3DRS_WRAP11                    = 201,
    D3DRS_WRAP12                    = 202,
    D3DRS_WRAP13                    = 203,
    D3DRS_WRAP14                    = 204,
    D3DRS_WRAP15                    = 205,
    D3DRS_SEPARATEALPHABLENDENABLE  = 206,
    D3DRS_SRCBLENDALPHA             = 207,
    D3DRS_DESTBLENDALPHA            = 208,
    D3DRS_BLENDOPALPHA              = 209,
    D3DRS_FORCE_DWORD               = 0x7fffffff
} D3DRENDERSTATETYPE;

typedef enum _D3DBLEND {
    D3DBLEND_ZERO            =  1,
    D3DBLEND_ONE             =  2,
    D3DBLEND_SRCCOLOR        =  3,
    D3DBLEND_INVSRCCOLOR     =  4,
    D3DBLEND_SRCALPHA        =  5,
    D3DBLEND_INVSRCALPHA     =  6,
    D3DBLEND_DESTALPHA       =  7,
    D3DBLEND_INVDESTALPHA    =  8,
    D3DBLEND_DESTCOLOR       =  9,
    D3DBLEND_INVDESTCOLOR    = 10,
    D3DBLEND_SRCALPHASAT     = 11,
    D3DBLEND_BOTHSRCALPHA    = 12,
    D3DBLEND_BOTHINVSRCALPHA = 13,
    D3DBLEND_BLENDFACTOR     = 14,
    D3DBLEND_INVBLENDFACTOR  = 15,
    D3DBLEND_SRCCOLOR2       = 16,
    D3DBLEND_INVSRCCOLOR2    = 17,
    D3DBLEND_FORCE_DWORD     = 0x7fffffff
} D3DBLEND;

typedef enum _D3DBLENDOP {
    D3DBLENDOP_ADD          = 1,
    D3DBLENDOP_SUBTRACT     = 2,
    D3DBLENDOP_REVSUBTRACT  = 3,
    D3DBLENDOP_MIN          = 4,
    D3DBLENDOP_MAX          = 5,
    D3DBLENDOP_FORCE_DWORD  = 0x7fffffff
} D3DBLENDOP;

typedef enum _D3DCULL {
    D3DCULL_NONE        = 1,
    D3DCULL_CW          = 2,
    D3DCULL_CCW         = 3,
    D3DCULL_FORCE_DWORD = 0x7fffffff
} D3DCULL;

typedef enum _D3DCMPFUNC {
    D3DCMP_NEVER        = 1,
    D3DCMP_LESS         = 2,
    D3DCMP_EQUAL        = 3,
    D3DCMP_LESSEQUAL    = 4,
    D3DCMP_GREATER      = 5,
    D3DCMP_NOTEQUAL     = 6,
    D3DCMP_GREATEREQUAL = 7,
    D3DCMP_ALWAYS       = 8,
    D3DCMP_FORCE_DWORD  = 0x7fffffff
} D3DCMPFUNC;

typedef enum _D3DSTENCILOP {
    D3DSTENCILOP_KEEP    = 1,
    D3DSTENCILOP_ZERO    = 2,
    D3DSTENCILOP_REPLACE = 3,
    D3DSTENCILOP_INCRSAT = 4,
    D3DSTENCILOP_DECRSAT = 5,
    D3DSTENCILOP_INVERT  = 6,
    D3DSTENCILOP_INCR    = 7,
    D3DSTENCILOP_DECR    = 8,
    D3DSTENCILOP_FORCE_DWORD = 0x7fffffff
} D3DSTENCILOP;

typedef enum _D3DFILLMODE {
    D3DFILL_POINT       = 1,
    D3DFILL_WIREFRAME   = 2,
    D3DFILL_SOLID       = 3,
    D3DFILL_FORCE_DWORD = 0x7fffffff
} D3DFILLMODE;

typedef enum _D3DSHADEMODE {
    D3DSHADE_FLAT        = 1,
    D3DSHADE_GOURAUD     = 2,
    D3DSHADE_PHONG       = 3,
    D3DSHADE_FORCE_DWORD = 0x7fffffff
} D3DSHADEMODE;

typedef enum _D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE            = 0,
    D3DTEXF_POINT           = 1,
    D3DTEXF_LINEAR          = 2,
    D3DTEXF_ANISOTROPIC     = 3,
    D3DTEXF_PYRAMIDALQUAD   = 6,
    D3DTEXF_GAUSSIANQUAD    = 7,
    D3DTEXF_CONVOLUTIONMONO = 8,
    D3DTEXF_FORCE_DWORD     = 0x7fffffff
} D3DTEXTUREFILTERTYPE;

typedef enum _D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP        = 1,
    D3DTADDRESS_MIRROR      = 2,
    D3DTADDRESS_CLAMP       = 3,
    D3DTADDRESS_BORDER      = 4,
    D3DTADDRESS_MIRRORONCE  = 5,
    D3DTADDRESS_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREADDRESS;

typedef enum _D3DSAMPLERSTATETYPE {
    D3DSAMP_ADDRESSU       =  1,
    D3DSAMP_ADDRESSV       =  2,
    D3DSAMP_ADDRESSW       =  3,
    D3DSAMP_BORDERCOLOR    =  4,
    D3DSAMP_MAGFILTER      =  5,
    D3DSAMP_MINFILTER      =  6,
    D3DSAMP_MIPFILTER      =  7,
    D3DSAMP_MIPMAPLODBIAS  =  8,
    D3DSAMP_MAXMIPLEVEL    =  9,
    D3DSAMP_MAXANISOTROPY  = 10,
    D3DSAMP_SRGBTEXTURE    = 11,
    D3DSAMP_ELEMENTINDEX   = 12,
    D3DSAMP_DMAPOFFSET     = 13,
    D3DSAMP_FORCE_DWORD    = 0x7fffffff
} D3DSAMPLERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP        =  1,
    D3DTSS_COLORARG1      =  2,
    D3DTSS_COLORARG2      =  3,
    D3DTSS_ALPHAOP        =  4,
    D3DTSS_ALPHAARG1      =  5,
    D3DTSS_ALPHAARG2      =  6,
    D3DTSS_BUMPENVMAT00   =  7,
    D3DTSS_BUMPENVMAT01   =  8,
    D3DTSS_BUMPENVMAT10   =  9,
    D3DTSS_BUMPENVMAT11   = 10,
    D3DTSS_TEXCOORDINDEX  = 11,
    D3DTSS_BUMPENVLSCALE  = 22,
    D3DTSS_BUMPENVLOFFSET = 23,
    D3DTSS_TEXTURETRANSFORMFLAGS = 24,
    D3DTSS_COLORARG0      = 26,
    D3DTSS_ALPHAARG0      = 27,
    D3DTSS_RESULTARG      = 28,
    D3DTSS_CONSTANT       = 32,
    D3DTSS_FORCE_DWORD    = 0x7fffffff
} D3DTEXTURESTAGESTATETYPE;

typedef enum _D3DTEXTUREOP {
    D3DTOP_DISABLE                    =  1,
    D3DTOP_SELECTARG1                 =  2,
    D3DTOP_SELECTARG2                 =  3,
    D3DTOP_MODULATE                   =  4,
    D3DTOP_MODULATE2X                 =  5,
    D3DTOP_MODULATE4X                 =  6,
    D3DTOP_ADD                        =  7,
    D3DTOP_ADDSIGNED                  =  8,
    D3DTOP_ADDSIGNED2X                =  9,
    D3DTOP_SUBTRACT                   = 10,
    D3DTOP_ADDSMOOTH                  = 11,
    D3DTOP_BLENDDIFFUSEALPHA          = 12,
    D3DTOP_BLENDTEXTUREALPHA          = 13,
    D3DTOP_BLENDFACTORALPHA           = 14,
    D3DTOP_BLENDTEXTUREALPHAPM        = 15,
    D3DTOP_BLENDCURRENTALPHA          = 16,
    D3DTOP_PREMODULATE                = 17,
    D3DTOP_MODULATEALPHA_ADDCOLOR     = 18,
    D3DTOP_MODULATECOLOR_ADDALPHA     = 19,
    D3DTOP_MODULATEINVALPHA_ADDCOLOR  = 20,
    D3DTOP_MODULATEINVCOLOR_ADDALPHA  = 21,
    D3DTOP_BUMPENVMAP                 = 22,
    D3DTOP_BUMPENVMAPLUMINANCE        = 23,
    D3DTOP_DOTPRODUCT3                = 24,
    D3DTOP_MULTIPLYADD                = 25,
    D3DTOP_LERP                       = 26,
    D3DTOP_FORCE_DWORD                = 0x7fffffff
} D3DTEXTUREOP;

typedef enum _D3DVERTEXBLENDFLAGS {
    D3DVBF_DISABLE  = 0,
    D3DVBF_1WEIGHTS = 1,
    D3DVBF_2WEIGHTS = 2,
    D3DVBF_3WEIGHTS = 3,
    D3DVBF_TWEENING = 255,
    D3DVBF_0WEIGHTS = 256,
    D3DVBF_FORCE_DWORD = 0x7fffffff
} D3DVERTEXBLENDFLAGS;

typedef enum _D3DDECLUSAGE {
    D3DDECLUSAGE_POSITION     = 0,
    D3DDECLUSAGE_BLENDWEIGHT  = 1,
    D3DDECLUSAGE_BLENDINDICES = 2,
    D3DDECLUSAGE_NORMAL       = 3,
    D3DDECLUSAGE_PSIZE        = 4,
    D3DDECLUSAGE_TEXCOORD     = 5,
    D3DDECLUSAGE_TANGENT      = 6,
    D3DDECLUSAGE_BINORMAL     = 7,
    D3DDECLUSAGE_TESSFACTOR   = 8,
    D3DDECLUSAGE_POSITIONT    = 9,
    D3DDECLUSAGE_COLOR        = 10,
    D3DDECLUSAGE_FOG          = 11,
    D3DDECLUSAGE_DEPTH        = 12,
    D3DDECLUSAGE_SAMPLE       = 13
} D3DDECLUSAGE;

typedef enum _D3DDECLMETHOD {
    D3DDECLMETHOD_DEFAULT         = 0,
    D3DDECLMETHOD_PARTIALU        = 1,
    D3DDECLMETHOD_PARTIALV        = 2,
    D3DDECLMETHOD_CROSSUV         = 3,
    D3DDECLMETHOD_UV              = 4,
    D3DDECLMETHOD_LOOKUP          = 5,
    D3DDECLMETHOD_LOOKUPPRESAMPLED = 6
} D3DDECLMETHOD;

typedef enum _D3DDECLTYPE {
    D3DDECLTYPE_FLOAT1    =  0,
    D3DDECLTYPE_FLOAT2    =  1,
    D3DDECLTYPE_FLOAT3    =  2,
    D3DDECLTYPE_FLOAT4    =  3,
    D3DDECLTYPE_D3DCOLOR  =  4,
    D3DDECLTYPE_UBYTE4    =  5,
    D3DDECLTYPE_SHORT2    =  6,
    D3DDECLTYPE_SHORT4    =  7,
    D3DDECLTYPE_UBYTE4N   =  8,
    D3DDECLTYPE_SHORT2N   =  9,
    D3DDECLTYPE_SHORT4N   = 10,
    D3DDECLTYPE_USHORT2N  = 11,
    D3DDECLTYPE_USHORT4N  = 12,
    D3DDECLTYPE_UDEC3     = 13,
    D3DDECLTYPE_DEC3N     = 14,
    D3DDECLTYPE_FLOAT16_2 = 15,
    D3DDECLTYPE_FLOAT16_4 = 16,
    D3DDECLTYPE_UNUSED    = 17
} D3DDECLTYPE;

typedef enum _D3DFOGMODE {
    D3DFOG_NONE        = 0,
    D3DFOG_EXP         = 1,
    D3DFOG_EXP2        = 2,
    D3DFOG_LINEAR      = 3,
    D3DFOG_FORCE_DWORD = 0x7fffffff
} D3DFOGMODE;

typedef enum _D3DLIGHTTYPE {
    D3DLIGHT_POINT       = 1,
    D3DLIGHT_SPOT        = 2,
    D3DLIGHT_DIRECTIONAL = 3,
    D3DLIGHT_FORCE_DWORD = 0x7fffffff
} D3DLIGHTTYPE;

typedef enum _D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW        =  2,
    D3DTS_PROJECTION  =  3,
    D3DTS_TEXTURE0    = 16,
    D3DTS_TEXTURE1    = 17,
    D3DTS_TEXTURE2    = 18,
    D3DTS_TEXTURE3    = 19,
    D3DTS_TEXTURE4    = 20,
    D3DTS_TEXTURE5    = 21,
    D3DTS_TEXTURE6    = 22,
    D3DTS_TEXTURE7    = 23,
    D3DTS_FORCE_DWORD = 0x7fffffff
} D3DTRANSFORMSTATETYPE;

#define D3DTS_WORLD  D3DTS_WORLDMATRIX(0)
#define D3DTS_WORLD1 D3DTS_WORLDMATRIX(1)
#define D3DTS_WORLD2 D3DTS_WORLDMATRIX(2)
#define D3DTS_WORLD3 D3DTS_WORLDMATRIX(3)
#define D3DTS_WORLDMATRIX(index) (D3DTRANSFORMSTATETYPE)(index + 256)

typedef enum _D3DQUERYTYPE {
    D3DQUERYTYPE_VCACHE             = 4,
    D3DQUERYTYPE_RESOURCEMANAGER    = 5,
    D3DQUERYTYPE_VERTEXSTATS        = 6,
    D3DQUERYTYPE_EVENT              = 8,
    D3DQUERYTYPE_OCCLUSION          = 9,
    D3DQUERYTYPE_TIMESTAMP          = 10,
    D3DQUERYTYPE_TIMESTAMPDISJOINT  = 11,
    D3DQUERYTYPE_PIPELINETIMINGS    = 12,
    D3DQUERYTYPE_INTERFACETIMINGS   = 13,
    D3DQUERYTYPE_VERTEXTIMINGS      = 14,
    D3DQUERYTYPE_PIXELTIMINGS       = 15,
    D3DQUERYTYPE_BANDWIDTHTIMINGS   = 16,
    D3DQUERYTYPE_CACHEUTILIZATION   = 17,
    D3DQUERYTYPE_MEMORYPRESSURE     = 18
} D3DQUERYTYPE;

// D3D9 Usage Flags
#define D3DUSAGE_RENDERTARGET       0x00000001L
#define D3DUSAGE_DEPTHSTENCIL       0x00000002L
#define D3DUSAGE_DYNAMIC            0x00000200L
#define D3DUSAGE_NONSECURE          0x00800000L
#define D3DUSAGE_AUTOGENMIPMAP      0x00000400L
#define D3DUSAGE_DMAP               0x00004000L
#define D3DUSAGE_QUERY_LEGACYBUMPMAP 0x00008000L
#define D3DUSAGE_QUERY_SRGBREAD     0x00010000L
#define D3DUSAGE_QUERY_FILTER       0x00020000L
#define D3DUSAGE_QUERY_SRGBWRITE    0x00040000L
#define D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING 0x00080000L
#define D3DUSAGE_QUERY_VERTEXTEXTURE 0x00100000L
#define D3DUSAGE_QUERY_WRAPANDMIP   0x00200000L
#define D3DUSAGE_WRITEONLY          0x00000008L
#define D3DUSAGE_SOFTWAREPROCESSING 0x00000010L
#define D3DUSAGE_DONOTCLIP          0x00000020L
#define D3DUSAGE_POINTS             0x00000040L
#define D3DUSAGE_RTPATCHES          0x00000080L
#define D3DUSAGE_NPATCHES           0x00000100L
#define D3DUSAGE_TEXTAPI            0x10000000L
#define D3DUSAGE_RESTRICTED_CONTENT 0x00000800L
#define D3DUSAGE_RESTRICT_SHARED_RESOURCE 0x02000000L
#define D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER 0x00001000L

// Lock Flags
#define D3DLOCK_READONLY            0x00000010L
#define D3DLOCK_DISCARD             0x00002000L
#define D3DLOCK_NOOVERWRITE         0x00001000L
#define D3DLOCK_NOSYSLOCK           0x00000800L
#define D3DLOCK_DONOTWAIT           0x00004000L
#define D3DLOCK_NO_DIRTY_UPDATE     0x00008000L

// Create Device Flags
#define D3DCREATE_FPU_PRESERVE              0x00000002L
#define D3DCREATE_MULTITHREADED             0x00000004L
#define D3DCREATE_PUREDEVICE                0x00000010L
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020L
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040L
#define D3DCREATE_MIXED_VERTEXPROCESSING    0x00000080L
#define D3DCREATE_DISABLE_DRIVER_MANAGEMENT 0x00000100L
#define D3DCREATE_ADAPTERGROUP_DEVICE       0x00000200L
#define D3DCREATE_DISABLE_DRIVER_MANAGEMENT_EX 0x00000400L
#define D3DCREATE_NOWINDOWCHANGES           0x00000800L
#define D3DCREATE_DISABLE_PSGP_THREADING    0x00002000L
#define D3DCREATE_ENABLE_PRESENTSTATS       0x00004000L
#define D3DCREATE_DISABLE_PRESENTSTATS      0x00008000L
#define D3DCREATE_SCREENSAVER               0x10000000L

// Present Flags
#define D3DPRESENT_RATE_DEFAULT             0x00000000
#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER  0x00000001
#define D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL 0x00000002
#define D3DPRESENTFLAG_DEVICECLIP           0x00000004
#define D3DPRESENTFLAG_VIDEO                0x00000010
#define D3DPRESENTFLAG_NOAUTOROTATE         0x00000020
#define D3DPRESENTFLAG_UNPRUNEDMODE         0x00000040

// D3D9 Structures
typedef struct _D3DVIEWPORT9 {
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
} D3DVIEWPORT9;

typedef struct _D3DRECT {
    LONG x1;
    LONG y1;
    LONG x2;
    LONG y2;
} D3DRECT;

typedef struct _D3DCLIPSTATUS9 {
    DWORD ClipUnion;
    DWORD ClipIntersection;
} D3DCLIPSTATUS9;

typedef struct _D3DMATERIAL9 {
    float Diffuse[4];
    float Ambient[4];
    float Specular[4];
    float Emissive[4];
    float Power;
} D3DMATERIAL9;

typedef struct _D3DLIGHT9 {
    D3DLIGHTTYPE  Type;
    float         Diffuse[4];
    float         Specular[4];
    float         Ambient[4];
    float         Position[3];
    float         Direction[3];
    float         Range;
    float         Falloff;
    float         Attenuation0;
    float         Attenuation1;
    float         Attenuation2;
    float         Theta;
    float         Phi;
} D3DLIGHT9;

typedef struct _D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} D3DMATRIX;

typedef struct _D3DVECTOR {
    float x;
    float y;
    float z;
} D3DVECTOR;

typedef struct _D3DVERTEXELEMENT9 {
    WORD    Stream;
    WORD    Offset;
    BYTE    Type;
    BYTE    Method;
    BYTE    Usage;
    BYTE    UsageIndex;
} D3DVERTEXELEMENT9;

#define D3DDECL_END() {0xFF,0,D3DDECLTYPE_UNUSED,0,0,0}

typedef struct _D3DDISPLAYMODE {
    UINT Width;
    UINT Height;
    UINT RefreshRate;
    D3DFORMAT Format;
} D3DDISPLAYMODE;

typedef struct _D3DPRESENT_PARAMETERS {
    UINT                BackBufferWidth;
    UINT                BackBufferHeight;
    D3DFORMAT           BackBufferFormat;
    UINT                BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD               MultiSampleQuality;
    D3DSWAPEFFECT       SwapEffect;
    HWND                hDeviceWindow;
    BOOL                Windowed;
    BOOL                EnableAutoDepthStencil;
    D3DFORMAT           AutoDepthStencilFormat;
    DWORD               Flags;
    UINT                FullScreen_RefreshRateInHz;
    UINT                PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct _D3DLOCKED_RECT {
    INT  Pitch;
    void *pBits;
} D3DLOCKED_RECT;

typedef struct _D3DLOCKED_BOX {
    INT   RowPitch;
    INT   SlicePitch;
    void *pBits;
} D3DLOCKED_BOX;

typedef struct _D3DBOX {
    UINT Left;
    UINT Top;
    UINT Right;
    UINT Bottom;
    UINT Front;
    UINT Back;
} D3DBOX;

typedef struct _D3DRANGE {
    UINT Offset;
    UINT Size;
} D3DRANGE;

typedef struct _D3DRASTER_STATUS {
    BOOL InVBlank;
    UINT ScanLine;
} D3DRASTER_STATUS;

typedef struct _D3DDEVINFO_D3DVERTEXSTATS {
    DWORD NumRenderedTriangles;
    DWORD NumExtraClippingTriangles;
} D3DDEVINFO_D3DVERTEXSTATS;

typedef struct _D3DDEVINFO_VCACHE {
    DWORD Pattern;
    DWORD OptMethod;
    DWORD CacheSize;
    DWORD MagicNumber;
} D3DDEVINFO_VCACHE;

typedef struct _D3DCAPS9 {
    D3DDEVTYPE  DeviceType;
    UINT        AdapterOrdinal;
    DWORD       Caps;
    DWORD       Caps2;
    DWORD       Caps3;
    DWORD       PresentationIntervals;
    DWORD       CursorCaps;
    DWORD       DevCaps;
    DWORD       PrimitiveMiscCaps;
    DWORD       RasterCaps;
    DWORD       ZCmpCaps;
    DWORD       SrcBlendCaps;
    DWORD       DestBlendCaps;
    DWORD       AlphaCmpCaps;
    DWORD       ShadeCaps;
    DWORD       TextureCaps;
    DWORD       TextureFilterCaps;
    DWORD       CubeTextureFilterCaps;
    DWORD       VolumeTextureFilterCaps;
    DWORD       TextureAddressCaps;
    DWORD       VolumeTextureAddressCaps;
    DWORD       LineCaps;
    DWORD       MaxTextureWidth, MaxTextureHeight;
    DWORD       MaxVolumeExtent;
    DWORD       MaxTextureRepeat;
    DWORD       MaxTextureAspectRatio;
    DWORD       MaxAnisotropy;
    float       MaxVertexW;
    float       GuardBandLeft;
    float       GuardBandTop;
    float       GuardBandRight;
    float       GuardBandBottom;
    float       ExtentsAdjust;
    DWORD       StencilCaps;
    DWORD       FVFCaps;
    DWORD       TextureOpCaps;
    DWORD       MaxTextureBlendStages;
    DWORD       MaxSimultaneousTextures;
    DWORD       VertexProcessingCaps;
    DWORD       MaxActiveLights;
    DWORD       MaxUserClipPlanes;
    DWORD       MaxVertexBlendMatrices;
    DWORD       MaxVertexBlendMatrixIndex;
    float       MaxPointSize;
    DWORD       MaxPrimitiveCount;
    DWORD       MaxVertexIndex;
    DWORD       MaxStreams;
    DWORD       MaxStreamStride;
    DWORD       VertexShaderVersion;
    DWORD       MaxVertexShaderConst;
    DWORD       PixelShaderVersion;
    float       PixelShader1xMaxValue;
    DWORD       DevCaps2;
    float       MaxNpatchTessellationLevel;
    DWORD       Reserved5;
    UINT        MasterAdapterOrdinal;
    UINT        AdapterOrdinalInGroup;
    UINT        NumberOfAdaptersInGroup;
    DWORD       DeclTypes;
    DWORD       NumSimultaneousRTs;
    DWORD       StretchRectFilterCaps;
    DWORD       VS20Caps[4];
    DWORD       PS20Caps[4];
    DWORD       VertexTextureFilterCaps;
    DWORD       MaxVShaderInstructionsExecuted;
    DWORD       MaxPShaderInstructionsExecuted;
    DWORD       MaxVertexShader30InstructionSlots;
    DWORD       MaxPixelShader30InstructionSlots;
} D3DCAPS9;

typedef struct _D3DADAPTER_IDENTIFIER9 {
    char    Driver[512];
    char    Description[512];
    char    DeviceName[32];
    LARGE_INTEGER DriverVersion;
    DWORD   VendorId;
    DWORD   DeviceId;
    DWORD   SubSysId;
    DWORD   Revision;
    GUID    DeviceIdentifier;
    DWORD   WHQLLevel;
} D3DADAPTER_IDENTIFIER9;

typedef struct _D3DSURFACE_DESC {
    D3DFORMAT           Format;
    D3DRESOURCETYPE     Type;
    DWORD               Usage;
    D3DPOOL             Pool;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD               MultiSampleQuality;
    UINT                Width;
    UINT                Height;
} D3DSURFACE_DESC;

typedef struct _D3DVOLUME_DESC {
    D3DFORMAT       Format;
    D3DRESOURCETYPE Type;
    DWORD           Usage;
    D3DPOOL         Pool;
    UINT            Width;
    UINT            Height;
    UINT            Depth;
} D3DVOLUME_DESC;

typedef struct _D3DINDEXBUFFER_DESC {
    D3DFORMAT       Format;
    D3DRESOURCETYPE Type;
    DWORD           Usage;
    D3DPOOL         Pool;
    UINT            Size;
} D3DINDEXBUFFER_DESC;

typedef struct _D3DVERTEXBUFFER_DESC {
    D3DFORMAT       Format;
    D3DRESOURCETYPE Type;
    DWORD           Usage;
    D3DPOOL         Pool;
    UINT            Size;
    DWORD           FVF;
} D3DVERTEXBUFFER_DESC;

// FVF Flags
#define D3DFVF_RESERVED0        0x001
#define D3DFVF_POSITION_MASK    0x400E
#define D3DFVF_XYZ             0x002
#define D3DFVF_XYZRHW          0x004
#define D3DFVF_XYZB1           0x006
#define D3DFVF_XYZB2           0x008
#define D3DFVF_XYZB3           0x00a
#define D3DFVF_XYZB4           0x00c
#define D3DFVF_XYZB5           0x00e
#define D3DFVF_XYZW            0x4002
#define D3DFVF_NORMAL          0x010
#define D3DFVF_PSIZE           0x020
#define D3DFVF_DIFFUSE         0x040
#define D3DFVF_SPECULAR        0x080
#define D3DFVF_TEXCOUNT_MASK   0xf00
#define D3DFVF_TEXCOUNT_SHIFT  8
#define D3DFVF_TEX0           0x000
#define D3DFVF_TEX1           0x100
#define D3DFVF_TEX2           0x200
#define D3DFVF_TEX3           0x300
#define D3DFVF_TEX4           0x400
#define D3DFVF_TEX5           0x500
#define D3DFVF_TEX6           0x600
#define D3DFVF_TEX7           0x700
#define D3DFVF_TEX8           0x800
#define D3DFVF_LASTBETA_UBYTE4    0x1000
#define D3DFVF_LASTBETA_D3DCOLOR  0x8000
#define D3DFVF_RESERVED2          0x6000

// Caps
#define D3DCAPS_READ_SCANLINE               0x20000
#define D3DCAPS2_FULLSCREENGAMMA            0x00020000L
#define D3DCAPS2_CANCALIBRATEGAMMA          0x00100000L
#define D3DCAPS2_RESERVED                   0x02000000L
#define D3DCAPS2_CANMANAGERESOURCE          0x10000000L
#define D3DCAPS2_DYNAMICTEXTURES            0x20000000L
#define D3DCAPS2_CANAUTOGENMIPMAP           0x40000000L
#define D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD  0x00000020L
#define D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION 0x00000080L
#define D3DCAPS3_COPY_TO_VIDMEM             0x00000100L
#define D3DCAPS3_COPY_TO_SYSTEMMEM          0x00000200L
#define D3DCAPS3_DXVAHD                     0x00000400L

#define D3DDEVCAPS_EXECUTESYSTEMMEMORY      0x00000010L
#define D3DDEVCAPS_EXECUTEVIDEOMEMORY       0x00000020L
#define D3DDEVCAPS_TLVERTEX_SYSTEMMEMORY    0x00000040L
#define D3DDEVCAPS_TLVERTEX_VIDEOMEMORY     0x00000080L
#define D3DDEVCAPS_TEXTURESYSTEMMEMORY      0x00000100L
#define D3DDEVCAPS_TEXTUREVIDEOMEMORY       0x00000200L
#define D3DDEVCAPS_DRAWPRIMTLVERTEX         0x00000400L
#define D3DDEVCAPS_CANRENDERAFTERFLIP       0x00000800L
#define D3DDEVCAPS_TEXTURENONLOCALVIDMEM    0x00001000L
#define D3DDEVCAPS_DRAWPRIMITIVES2          0x00002000L
#define D3DDEVCAPS_SEPARATETEXTUREMEMORIES  0x00004000L
#define D3DDEVCAPS_DRAWPRIMITIVES2EX        0x00008000L
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT      0x00010000L
#define D3DDEVCAPS_CANBLTSYSTONONLOCAL      0x00020000L
#define D3DDEVCAPS_HWRASTERIZATION          0x00080000L
#define D3DDEVCAPS_PUREDEVICE               0x00100000L
#define D3DDEVCAPS_QUINTICRTPATCHES         0x00200000L
#define D3DDEVCAPS_RTPATCHES                0x00400000L
#define D3DDEVCAPS_RTPATCHHANDLEZERO        0x00800000L
#define D3DDEVCAPS_NPATCHES                 0x01000000L

#define D3DPRASTERCAPS_DITHER               0x00000001L
#define D3DPRASTERCAPS_ZTEST                0x00000010L
#define D3DPRASTERCAPS_FOGVERTEX            0x00000080L
#define D3DPRASTERCAPS_FOGTABLE             0x00000100L
#define D3DPRASTERCAPS_MIPMAPLODBIAS        0x00002000L
#define D3DPRASTERCAPS_ZBUFFERLESSHSR       0x00008000L
#define D3DPRASTERCAPS_FOGRANGE             0x00010000L
#define D3DPRASTERCAPS_ANISOTROPY           0x00020000L
#define D3DPRASTERCAPS_WBUFFER              0x00040000L
#define D3DPRASTERCAPS_WFOG                 0x00100000L
#define D3DPRASTERCAPS_ZFOG                 0x00200000L
#define D3DPRASTERCAPS_COLORPERSPECTIVE     0x00400000L
#define D3DPRASTERCAPS_SCISSORTEST          0x01000000L
#define D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS  0x02000000L
#define D3DPRASTERCAPS_DEPTHBIAS            0x04000000L
#define D3DPRASTERCAPS_MULTISAMPLE_TOGGLE   0x08000000L

#define D3DPTEXTURECAPS_PERSPECTIVE         0x00000001L
#define D3DPTEXTURECAPS_POW2                0x00000002L
#define D3DPTEXTURECAPS_ALPHA               0x00000004L
#define D3DPTEXTURECAPS_SQUAREONLY          0x00000020L
#define D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE 0x00000040L
#define D3DPTEXTURECAPS_ALPHAPALETTE        0x00000080L
#define D3DPTEXTURECAPS_NONPOW2CONDITIONAL  0x00000100L
#define D3DPTEXTURECAPS_PROJECTED           0x00000400L
#define D3DPTEXTURECAPS_CUBEMAP             0x00000800L
#define D3DPTEXTURECAPS_VOLUMEMAP           0x00002000L
#define D3DPTEXTURECAPS_MIPMAP              0x00004000L
#define D3DPTEXTURECAPS_MIPVOLUMEMAP        0x00008000L
#define D3DPTEXTURECAPS_MIPCUBEMAP          0x00010000L
#define D3DPTEXTURECAPS_CUBEMAP_POW2        0x00020000L
#define D3DPTEXTURECAPS_VOLUMEMAP_POW2      0x00040000L
#define D3DPTEXTURECAPS_NOPROJECTEDBUMPENV  0x00200000L

#define D3DPTFILTERCAPS_MINFPOINT           0x00000100L
#define D3DPTFILTERCAPS_MINFLINEAR          0x00000200L
#define D3DPTFILTERCAPS_MINFANISOTROPIC     0x00000400L
#define D3DPTFILTERCAPS_MINFPYRAMIDALQUAD   0x00000800L
#define D3DPTFILTERCAPS_MINFGAUSSIANQUAD    0x00001000L
#define D3DPTFILTERCAPS_MIPFPOINT           0x00010000L
#define D3DPTFILTERCAPS_MIPFLINEAR          0x00020000L
#define D3DPTFILTERCAPS_CONVOLUTIONMONO     0x00040000L
#define D3DPTFILTERCAPS_MAGFPOINT           0x01000000L
#define D3DPTFILTERCAPS_MAGFLINEAR          0x02000000L
#define D3DPTFILTERCAPS_MAGFANISOTROPIC     0x04000000L
#define D3DPTFILTERCAPS_MAGFPYRAMIDALQUAD   0x08000000L
#define D3DPTFILTERCAPS_MAGFGAUSSIANQUAD    0x10000000L

#define D3DPTADDRESSCAPS_WRAP               0x00000001L
#define D3DPTADDRESSCAPS_MIRROR             0x00000002L
#define D3DPTADDRESSCAPS_CLAMP              0x00000004L
#define D3DPTADDRESSCAPS_BORDER             0x00000008L
#define D3DPTADDRESSCAPS_INDEPENDENTUV      0x00000010L
#define D3DPTADDRESSCAPS_MIRRORONCE         0x00000020L

#define D3DPCMPCAPS_NEVER                   0x00000001L
#define D3DPCMPCAPS_LESS                    0x00000002L
#define D3DPCMPCAPS_EQUAL                   0x00000004L
#define D3DPCMPCAPS_LESSEQUAL               0x00000008L
#define D3DPCMPCAPS_GREATER                 0x00000010L
#define D3DPCMPCAPS_NOTEQUAL                0x00000020L
#define D3DPCMPCAPS_GREATEREQUAL            0x00000040L
#define D3DPCMPCAPS_ALWAYS                  0x00000080L

#define D3DPSHADECAPS_COLORGOURAUDRGB       0x00000008L
#define D3DPSHADECAPS_SPECULARGOURAUDRGB    0x00000200L
#define D3DPSHADECAPS_ALPHAGOURAUDBLEND     0x00004000L
#define D3DPSHADECAPS_FOGGOURAUD            0x00080000L

#define D3DPBLENDCAPS_ZERO                  0x00000001L
#define D3DPBLENDCAPS_ONE                   0x00000002L
#define D3DPBLENDCAPS_SRCCOLOR              0x00000004L
#define D3DPBLENDCAPS_INVSRCCOLOR           0x00000008L
#define D3DPBLENDCAPS_SRCALPHA              0x00000010L
#define D3DPBLENDCAPS_INVSRCALPHA           0x00000020L
#define D3DPBLENDCAPS_DESTALPHA             0x00000040L
#define D3DPBLENDCAPS_INVDESTALPHA          0x00000080L
#define D3DPBLENDCAPS_DESTCOLOR             0x00000100L
#define D3DPBLENDCAPS_INVDESTCOLOR          0x00000200L
#define D3DPBLENDCAPS_SRCALPHASAT           0x00000400L
#define D3DPBLENDCAPS_BOTHSRCALPHA          0x00000800L
#define D3DPBLENDCAPS_BOTHINVSRCALPHA       0x00001000L
#define D3DPBLENDCAPS_BLENDFACTOR           0x00002000L
#define D3DPBLENDCAPS_SRCCOLOR2             0x00004000L
#define D3DPBLENDCAPS_INVSRCCOLOR2          0x00008000L

#define D3DSTENCILCAPS_KEEP                 0x00000001L
#define D3DSTENCILCAPS_ZERO                 0x00000002L
#define D3DSTENCILCAPS_REPLACE              0x00000004L
#define D3DSTENCILCAPS_INCRSAT              0x00000008L
#define D3DSTENCILCAPS_DECRSAT              0x00000010L
#define D3DSTENCILCAPS_INVERT               0x00000020L
#define D3DSTENCILCAPS_INCR                 0x00000040L
#define D3DSTENCILCAPS_DECR                 0x00000080L
#define D3DSTENCILCAPS_TWOSIDED             0x00000100L

#define D3DTEXOPCAPS_DISABLE                    0x00000001L
#define D3DTEXOPCAPS_SELECTARG1                 0x00000002L
#define D3DTEXOPCAPS_SELECTARG2                 0x00000004L
#define D3DTEXOPCAPS_MODULATE                   0x00000008L
#define D3DTEXOPCAPS_MODULATE2X                 0x00000010L
#define D3DTEXOPCAPS_MODULATE4X                 0x00000020L
#define D3DTEXOPCAPS_ADD                        0x00000040L
#define D3DTEXOPCAPS_ADDSIGNED                  0x00000080L
#define D3DTEXOPCAPS_ADDSIGNED2X                0x00000100L
#define D3DTEXOPCAPS_SUBTRACT                   0x00000200L
#define D3DTEXOPCAPS_ADDSMOOTH                  0x00000400L
#define D3DTEXOPCAPS_BLENDDIFFUSEALPHA          0x00000800L
#define D3DTEXOPCAPS_BLENDTEXTUREALPHA          0x00001000L
#define D3DTEXOPCAPS_BLENDFACTORALPHA           0x00002000L
#define D3DTEXOPCAPS_BLENDTEXTUREALPHAPM        0x00004000L
#define D3DTEXOPCAPS_BLENDCURRENTALPHA          0x00008000L
#define D3DTEXOPCAPS_PREMODULATE                0x00010000L
#define D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR     0x00020000L
#define D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA     0x00040000L
#define D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR  0x00080000L
#define D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA  0x00100000L
#define D3DTEXOPCAPS_BUMPENVMAP                 0x00200000L
#define D3DTEXOPCAPS_BUMPENVMAPLUMINANCE        0x00400000L
#define D3DTEXOPCAPS_DOTPRODUCT3                0x00800000L
#define D3DTEXOPCAPS_MULTIPLYADD                0x01000000L
#define D3DTEXOPCAPS_LERP                       0x02000000L

#define D3DVTXPCAPS_TEXGEN                  0x00000001L
#define D3DVTXPCAPS_MATERIALSOURCE7         0x00000002L
#define D3DVTXPCAPS_DIRECTIONALLIGHTS       0x00000008L
#define D3DVTXPCAPS_POSITIONALLIGHTS        0x00000010L
#define D3DVTXPCAPS_LOCALVIEWER             0x00000020L
#define D3DVTXPCAPS_TWEENING                0x00000040L
#define D3DVTXPCAPS_TEXGEN_SPHEREMAP        0x00000100L
#define D3DVTXPCAPS_NO_TEXGEN_NONLOCALVIEWER 0x00000200L

#define D3DPRESENT_INTERVAL_DEFAULT         0x00000000L
#define D3DPRESENT_INTERVAL_ONE             0x00000001L
#define D3DPRESENT_INTERVAL_TWO             0x00000002L
#define D3DPRESENT_INTERVAL_THREE           0x00000004L
#define D3DPRESENT_INTERVAL_FOUR            0x00000008L
#define D3DPRESENT_INTERVAL_IMMEDIATE       0x80000000L

#define D3DCLEAR_TARGET     0x00000001L
#define D3DCLEAR_ZBUFFER    0x00000002L
#define D3DCLEAR_STENCIL    0x00000004L

#define D3DCOLORWRITEENABLE_RED     (1L<<0)
#define D3DCOLORWRITEENABLE_GREEN   (1L<<1)
#define D3DCOLORWRITEENABLE_BLUE    (1L<<2)
#define D3DCOLORWRITEENABLE_ALPHA   (1L<<3)

// D3D9 COM Interfaces forward declarations
typedef struct IDirect3D9            IDirect3D9;
typedef struct IDirect3DDevice9      IDirect3DDevice9;
typedef struct IDirect3DResource9    IDirect3DResource9;
typedef struct IDirect3DBaseTexture9 IDirect3DBaseTexture9;
typedef struct IDirect3DTexture9     IDirect3DTexture9;
typedef struct IDirect3DVolumeTexture9 IDirect3DVolumeTexture9;
typedef struct IDirect3DCubeTexture9 IDirect3DCubeTexture9;
typedef struct IDirect3DVertexBuffer9 IDirect3DVertexBuffer9;
typedef struct IDirect3DIndexBuffer9 IDirect3DIndexBuffer9;
typedef struct IDirect3DSurface9     IDirect3DSurface9;
typedef struct IDirect3DVolume9      IDirect3DVolume9;
typedef struct IDirect3DSwapChain9   IDirect3DSwapChain9;
typedef struct IDirect3DVertexDeclaration9 IDirect3DVertexDeclaration9;
typedef struct IDirect3DVertexShader9 IDirect3DVertexShader9;
typedef struct IDirect3DPixelShader9 IDirect3DPixelShader9;
typedef struct IDirect3DStateBlock9  IDirect3DStateBlock9;
typedef struct IDirect3DQuery9       IDirect3DQuery9;

// GUIDs
DEFINE_GUID(IID_IDirect3D9,           0x81bdcbca, 0x64d4, 0x426d, 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c);
DEFINE_GUID(IID_IDirect3DDevice9,     0xd0223b96, 0xbf7a, 0x43fd, 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb);
DEFINE_GUID(IID_IDirect3DResource9,   0x05eec05d, 0x8443, 0x4cda, 0xb3, 0x57, 0xd5, 0xdc, 0x58, 0x32, 0x24, 0x61);
DEFINE_GUID(IID_IDirect3DBaseTexture9,0x580ca87e, 0x1d3c, 0x4d54, 0x99, 0x1d, 0xb7, 0xd3, 0xe3, 0xc2, 0x98, 0xce);
DEFINE_GUID(IID_IDirect3DTexture9,    0x85c31227, 0x3de5, 0x4f00, 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5);
DEFINE_GUID(IID_IDirect3DVolumeTexture9,0x2518526c,0xe789,0x4111,0xa7,0xb9,0x47,0xef,0x32,0x8d,0x13,0xe6);
DEFINE_GUID(IID_IDirect3DCubeTexture9,0xfff32f81, 0xd953, 0x473a, 0x92, 0x23, 0x93, 0xd6, 0x52, 0xab, 0xa9, 0x3f);
DEFINE_GUID(IID_IDirect3DVertexBuffer9,0xb64bb1b5,0xfd70,0x4df6,0xbf,0x91,0x19,0xd0,0xa1,0x24,0x55,0xe3);
DEFINE_GUID(IID_IDirect3DIndexBuffer9, 0x7c9dd65e,0xd3f7,0x4529,0xac,0xee,0x78,0x58,0x30,0x9e,0x6c,0x7b);
DEFINE_GUID(IID_IDirect3DSurface9,    0x0cfbaf3a, 0x9ff6, 0x429a, 0x99, 0xb3, 0xa2, 0x79, 0x6a, 0xf8, 0xb8, 0x9b);
DEFINE_GUID(IID_IDirect3DVolume9,     0x24f416e6, 0x1f67, 0x4aa7, 0xb8, 0x8e, 0xd3, 0x3f, 0x6f, 0x31, 0x28, 0xa1);
DEFINE_GUID(IID_IDirect3DSwapChain9,  0x794950f2, 0xadfc, 0x458a, 0x90, 0x5e, 0x10, 0xa1, 0x0b, 0x0b, 0x50, 0x3b);
DEFINE_GUID(IID_IDirect3DVertexDeclaration9,0xdd13c59c,0x36fa,0x4098,0xa8,0xfb,0xc7,0xed,0x39,0xdc,0x85,0x46);
DEFINE_GUID(IID_IDirect3DVertexShader9,0xefc5557e,0x6265,0x4613,0x8a,0x94,0x43,0x85,0x78,0x89,0xeb,0x36);
DEFINE_GUID(IID_IDirect3DPixelShader9, 0x6d3bdbdc,0x5b02,0x4415,0xb8,0x52,0xce,0x5e,0x8b,0xcc,0xb2,0x89);
DEFINE_GUID(IID_IDirect3DStateBlock9,  0xb07c4fe5,0x310d,0x4ba8,0xa2,0x3c,0x4f,0x0f,0x20,0x6f,0x21,0x8b);
DEFINE_GUID(IID_IDirect3DQuery9,       0xd9771460,0xa695,0x4f26,0xbb,0xd3,0x27,0xb8,0x40,0xb5,0x41,0xcc);

#ifdef __cplusplus

// COM Interface declarations using vtable approach compatible with C++
struct IDirect3DResource9Vtbl;
struct IDirect3DBaseTexture9Vtbl;

#define DECLARE_INTERFACE_(iface, baseiface) struct iface : public baseiface
#define DECLARE_INTERFACE(iface) struct iface

// Macro helpers for vtable methods
#define STDMETHOD(method)        virtual HRESULT STDMETHODCALLTYPE method
#define STDMETHOD_(type,method)  virtual type STDMETHODCALLTYPE method

struct IDirect3D9 : public IUnknown {
    STDMETHOD(RegisterSoftwareDevice)(void *pInitializeFunction) = 0;
    STDMETHOD_(UINT, GetAdapterCount)() = 0;
    STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) = 0;
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter, D3DFORMAT Format) = 0;
    STDMETHOD(EnumAdapterModes)(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) = 0;
    STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE *pMode) = 0;
    STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) = 0;
    STDMETHOD(CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) = 0;
    STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels) = 0;
    STDMETHOD(CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) = 0;
    STDMETHOD(CheckDeviceFormatConversion)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) = 0;
    STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) = 0;
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) = 0;
    STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface) = 0;
};

struct IDirect3DResource9 : public IUnknown {
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD(SetPrivateData)(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) = 0;
    STDMETHOD(GetPrivateData)(REFGUID refguid, void *pData, DWORD *pSizeOfData) = 0;
    STDMETHOD(FreePrivateData)(REFGUID refguid) = 0;
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) = 0;
    STDMETHOD_(DWORD, GetPriority)() = 0;
    STDMETHOD_(void, PreLoad)() = 0;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() = 0;
};

struct IDirect3DBaseTexture9 : public IDirect3DResource9 {
    STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) = 0;
    STDMETHOD_(DWORD, GetLOD)() = 0;
    STDMETHOD_(DWORD, GetLevelCount)() = 0;
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE FilterType) = 0;
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() = 0;
    STDMETHOD_(void, GenerateMipSubLevels)() = 0;
};

struct IDirect3DTexture9 : public IDirect3DBaseTexture9 {
    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC *pDesc) = 0;
    STDMETHOD(GetSurfaceLevel)(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) = 0;
    STDMETHOD(LockRect)(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) = 0;
    STDMETHOD(UnlockRect)(UINT Level) = 0;
    STDMETHOD(AddDirtyRect)(const RECT *pDirtyRect) = 0;
};

struct IDirect3DVolumeTexture9 : public IDirect3DBaseTexture9 {
    STDMETHOD(GetLevelDesc)(UINT Level, D3DVOLUME_DESC *pDesc) = 0;
    STDMETHOD(GetVolumeLevel)(UINT Level, IDirect3DVolume9 **ppVolumeLevel) = 0;
    STDMETHOD(LockBox)(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) = 0;
    STDMETHOD(UnlockBox)(UINT Level) = 0;
    STDMETHOD(AddDirtyBox)(const D3DBOX *pDirtyBox) = 0;
};

struct IDirect3DCubeTexture9 : public IDirect3DBaseTexture9 {
    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC *pDesc) = 0;
    STDMETHOD(GetCubeMapSurface)(UINT FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) = 0;
    STDMETHOD(LockRect)(UINT FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) = 0;
    STDMETHOD(UnlockRect)(UINT FaceType, UINT Level) = 0;
    STDMETHOD(AddDirtyRect)(UINT FaceType, const RECT *pDirtyRect) = 0;
};

struct IDirect3DVertexBuffer9 : public IDirect3DResource9 {
    STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) = 0;
    STDMETHOD(Unlock)() = 0;
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC *pDesc) = 0;
};

struct IDirect3DIndexBuffer9 : public IDirect3DResource9 {
    STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) = 0;
    STDMETHOD(Unlock)() = 0;
    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC *pDesc) = 0;
};

struct IDirect3DSurface9 : public IDirect3DResource9 {
    STDMETHOD(GetContainer)(REFIID riid, void **ppContainer) = 0;
    STDMETHOD(GetDesc)(D3DSURFACE_DESC *pDesc) = 0;
    STDMETHOD(LockRect)(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) = 0;
    STDMETHOD(UnlockRect)() = 0;
    STDMETHOD(GetDC)(HDC *phdc) = 0;
    STDMETHOD(ReleaseDC)(HDC hdc) = 0;
};

struct IDirect3DVolume9 : public IUnknown {
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD(SetPrivateData)(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) = 0;
    STDMETHOD(GetPrivateData)(REFGUID refguid, void *pData, DWORD *pSizeOfData) = 0;
    STDMETHOD(FreePrivateData)(REFGUID refguid) = 0;
    STDMETHOD(GetContainer)(REFIID riid, void **ppContainer) = 0;
    STDMETHOD(GetDesc)(D3DVOLUME_DESC *pDesc) = 0;
    STDMETHOD(LockBox)(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) = 0;
    STDMETHOD(UnlockBox)() = 0;
};

struct IDirect3DSwapChain9 : public IUnknown {
    STDMETHOD(Present)(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags) = 0;
    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9 *pDestSurface) = 0;
    STDMETHOD(GetBackBuffer)(UINT iBackBuffer, UINT Type, IDirect3DSurface9 **ppBackBuffer) = 0;
    STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS *pRasterStatus) = 0;
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE *pMode) = 0;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS *pPresentationParameters) = 0;
};

struct IDirect3DVertexDeclaration9 : public IUnknown {
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) = 0;
};

struct IDirect3DVertexShader9 : public IUnknown {
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD(GetFunction)(void *pData, UINT *pSizeOfData) = 0;
};

struct IDirect3DPixelShader9 : public IUnknown {
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD(GetFunction)(void *pData, UINT *pSizeOfData) = 0;
};

typedef enum _D3DSTATEBLOCKTYPE {
    D3DSBT_ALL           = 1,
    D3DSBT_PIXELSTATE    = 2,
    D3DSBT_VERTEXSTATE   = 3,
    D3DSBT_FORCE_DWORD   = 0x7fffffff
} D3DSTATEBLOCKTYPE;

struct IDirect3DStateBlock9 : public IUnknown {
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD(Capture)() = 0;
    STDMETHOD(Apply)() = 0;
};

struct IDirect3DQuery9 : public IUnknown {
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) = 0;
    STDMETHOD_(D3DQUERYTYPE, GetType)() = 0;
    STDMETHOD_(DWORD, GetDataSize)() = 0;
    STDMETHOD(Issue)(DWORD dwIssueFlags) = 0;
    STDMETHOD(GetData)(void *pData, DWORD dwSize, DWORD dwGetDataFlags) = 0;
};

struct IDirect3DDevice9 : public IUnknown {
    STDMETHOD(TestCooperativeLevel)() = 0;
    STDMETHOD_(UINT, GetAvailableTextureMem)() = 0;
    STDMETHOD(EvictManagedResources)() = 0;
    STDMETHOD(GetDirect3D)(IDirect3D9 **ppD3D9) = 0;
    STDMETHOD(GetDeviceCaps)(D3DCAPS9 *pCaps) = 0;
    STDMETHOD(GetDisplayMode)(UINT iSwapChain, D3DDISPLAYMODE *pMode) = 0;
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS *pParameters) = 0;
    STDMETHOD(SetCursorProperties)(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) = 0;
    STDMETHOD_(void, SetCursorPosition)(int X, int Y, DWORD Flags) = 0;
    STDMETHOD_(BOOL, ShowCursor)(BOOL bShow) = 0;
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **pSwapChain) = 0;
    STDMETHOD(GetSwapChain)(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) = 0;
    STDMETHOD_(UINT, GetNumberOfSwapChains)() = 0;
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS *pPresentationParameters) = 0;
    STDMETHOD(Present)(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion) = 0;
    STDMETHOD(GetBackBuffer)(UINT iSwapChain, UINT iBackBuffer, UINT Type, IDirect3DSurface9 **ppBackBuffer) = 0;
    STDMETHOD(GetRasterStatus)(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) = 0;
    STDMETHOD(SetDialogBoxMode)(BOOL bEnableDialogs) = 0;
    STDMETHOD_(void, SetGammaRamp)(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) = 0;
    STDMETHOD_(void, GetGammaRamp)(UINT iSwapChain, D3DGAMMARAMP *pRamp) = 0;
    STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) = 0;
    STDMETHOD(CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) = 0;
    STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) = 0;
    STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) = 0;
    STDMETHOD(CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) = 0;
    STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) = 0;
    STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) = 0;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint) = 0;
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) = 0;
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) = 0;
    STDMETHOD(GetFrontBufferData)(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) = 0;
    STDMETHOD(StretchRect)(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter) = 0;
    STDMETHOD(ColorFill)(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color) = 0;
    STDMETHOD(CreateOffscreenPlainSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) = 0;
    STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) = 0;
    STDMETHOD(GetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) = 0;
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9 *pNewZStencil) = 0;
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9 **ppZStencilSurface) = 0;
    STDMETHOD(BeginScene)() = 0;
    STDMETHOD(EndScene)() = 0;
    STDMETHOD(Clear)(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) = 0;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) = 0;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) = 0;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) = 0;
    STDMETHOD(SetViewport)(const D3DVIEWPORT9 *pViewport) = 0;
    STDMETHOD(GetViewport)(D3DVIEWPORT9 *pViewport) = 0;
    STDMETHOD(SetMaterial)(const D3DMATERIAL9 *pMaterial) = 0;
    STDMETHOD(GetMaterial)(D3DMATERIAL9 *pMaterial) = 0;
    STDMETHOD(SetLight)(DWORD Index, const D3DLIGHT9 *pLight) = 0;
    STDMETHOD(GetLight)(DWORD Index, D3DLIGHT9 *pLight) = 0;
    STDMETHOD(LightEnable)(DWORD Index, BOOL Enable) = 0;
    STDMETHOD(GetLightEnable)(DWORD Index, BOOL *pEnable) = 0;
    STDMETHOD(SetClipPlane)(DWORD Index, const float *pPlane) = 0;
    STDMETHOD(GetClipPlane)(DWORD Index, float *pPlane) = 0;
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) = 0;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State, DWORD *pValue) = 0;
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) = 0;
    STDMETHOD(BeginStateBlock)() = 0;
    STDMETHOD(EndStateBlock)(IDirect3DStateBlock9 **ppSB) = 0;
    STDMETHOD(SetClipStatus)(const D3DCLIPSTATUS9 *pClipStatus) = 0;
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9 *pClipStatus) = 0;
    STDMETHOD(GetTexture)(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) = 0;
    STDMETHOD(SetTexture)(DWORD Stage, IDirect3DBaseTexture9 *pTexture) = 0;
    STDMETHOD(GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) = 0;
    STDMETHOD(SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) = 0;
    STDMETHOD(GetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) = 0;
    STDMETHOD(SetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) = 0;
    STDMETHOD(ValidateDevice)(DWORD *pNumPasses) = 0;
    STDMETHOD(SetPaletteEntries)(UINT PaletteNumber, const PALETTEENTRY *pEntries) = 0;
    STDMETHOD(GetPaletteEntries)(UINT PaletteNumber, PALETTEENTRY *pEntries) = 0;
    STDMETHOD(SetCurrentTexturePalette)(UINT PaletteNumber) = 0;
    STDMETHOD(GetCurrentTexturePalette)(UINT *PaletteNumber) = 0;
    STDMETHOD(SetScissorRect)(const RECT *pRect) = 0;
    STDMETHOD(GetScissorRect)(RECT *pRect) = 0;
    STDMETHOD(SetSoftwareVertexProcessing)(BOOL bSoftware) = 0;
    STDMETHOD_(BOOL, GetSoftwareVertexProcessing)() = 0;
    STDMETHOD(SetNPatchMode)(float nSegments) = 0;
    STDMETHOD_(float, GetNPatchMode)() = 0;
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) = 0;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) = 0;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) = 0;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) = 0;
    STDMETHOD(ProcessVertices)(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer, IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) = 0;
    STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) = 0;
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9 *pDecl) = 0;
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9 **ppDecl) = 0;
    STDMETHOD(SetFVF)(DWORD FVF) = 0;
    STDMETHOD(GetFVF)(DWORD *pFVF) = 0;
    STDMETHOD(CreateVertexShader)(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) = 0;
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9 *pShader) = 0;
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9 **ppShader) = 0;
    STDMETHOD(SetVertexShaderConstantF)(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) = 0;
    STDMETHOD(GetVertexShaderConstantF)(UINT StartRegister, float *pConstantData, UINT Vector4fCount) = 0;
    STDMETHOD(SetVertexShaderConstantI)(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) = 0;
    STDMETHOD(GetVertexShaderConstantI)(UINT StartRegister, int *pConstantData, UINT Vector4iCount) = 0;
    STDMETHOD(SetVertexShaderConstantB)(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) = 0;
    STDMETHOD(GetVertexShaderConstantB)(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) = 0;
    STDMETHOD(SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) = 0;
    STDMETHOD(GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) = 0;
    STDMETHOD(SetStreamSourceFreq)(UINT StreamNumber, UINT Setting) = 0;
    STDMETHOD(GetStreamSourceFreq)(UINT StreamNumber, UINT *pSetting) = 0;
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9 *pIndexData) = 0;
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9 **ppIndexData) = 0;
    STDMETHOD(CreatePixelShader)(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) = 0;
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9 *pShader) = 0;
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9 **ppShader) = 0;
    STDMETHOD(SetPixelShaderConstantF)(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) = 0;
    STDMETHOD(GetPixelShaderConstantF)(UINT StartRegister, float *pConstantData, UINT Vector4fCount) = 0;
    STDMETHOD(SetPixelShaderConstantI)(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) = 0;
    STDMETHOD(GetPixelShaderConstantI)(UINT StartRegister, int *pConstantData, UINT Vector4iCount) = 0;
    STDMETHOD(SetPixelShaderConstantB)(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) = 0;
    STDMETHOD(GetPixelShaderConstantB)(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) = 0;
    STDMETHOD(DrawRectPatch)(UINT Handle, const float *pNumSegs, const void *pRectPatchInfo) = 0;
    STDMETHOD(DrawTriPatch)(UINT Handle, const float *pNumSegs, const void *pTriPatchInfo) = 0;
    STDMETHOD(DeletePatch)(UINT Handle) = 0;
    STDMETHOD(CreateQuery)(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) = 0;
};

typedef struct _D3DDEVICE_CREATION_PARAMETERS {
    UINT        AdapterOrdinal;
    D3DDEVTYPE  DeviceType;
    HWND        hFocusWindow;
    DWORD       BehaviorFlags;
} D3DDEVICE_CREATION_PARAMETERS;

typedef struct _D3DGAMMARAMP {
    WORD red  [256];
    WORD green[256];
    WORD blue [256];
} D3DGAMMARAMP;

#endif // __cplusplus

// Export functions
IDirect3D9 * WINAPI Direct3DCreate9(UINT SDKVersion);

#define D3D_SDK_VERSION 32
#define D3D9b_SDK_VERSION 31

#ifdef __cplusplus
}
#endif
