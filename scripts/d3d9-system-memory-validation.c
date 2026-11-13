/* GPU readback checks for system-memory uploads, including retained video buffers. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    printf("FAIL %s: %08lx at %d\n", #call, (unsigned long)hr, __LINE__); exit(2); } } while (0)
static IDirect3DDevice9 *device;
static IDirect3DSurface9 *target, *readback;
static int failures, checks;
static const float vertices[6][4] = {{-1,-1,0,1},{-1,1,0,1},{1,1,0,1},
                                   {-1,-1,0,1},{1,1,0,1},{1,-1,0,1}};

static ID3DBlob *compile(const char *text, const char *profile) {
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(text, strlen(text), "system-memory", NULL, NULL, "main", profile, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) printf("%s\n", (char *)ID3D10Blob_GetBufferPointer(errors));
        exit(3);
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

static DWORD read_pixel(IDirect3DSurface9 *surface, UINT x, UINT y) {
    D3DLOCKED_RECT lock;
    CHECK(IDirect3DDevice9_GetRenderTargetData(device, surface, readback));
    CHECK(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY));
    DWORD value = *(DWORD *)((BYTE *)lock.pBits + y * lock.Pitch + x * 4);
    CHECK(IDirect3DSurface9_UnlockRect(readback));
    return value;
}

static void expect(const char *name, DWORD value, DWORD expected) {
    int ok = 1;
    for (int shift = 0; shift < 32; shift += 8)
        if (abs((int)((value >> shift) & 255) - (int)((expected >> shift) & 255)) > 3) ok = 0;
    checks++;
    printf("%s %s: %08lx expected %08lx\n", ok ? "PASS" : "FAIL", name,
           (unsigned long)value, (unsigned long)expected);
    if (!ok) failures++;
}

static DWORD sample(IDirect3DBaseTexture9 *texture, const char *program) {
    ID3DBlob *code = compile(program, "ps_3_0");
    IDirect3DPixelShader9 *shader;
    CHECK(IDirect3DDevice9_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(code), &shader));
    ID3D10Blob_Release(code);
    CHECK(IDirect3DDevice9_SetPixelShader(device, shader));
    CHECK(IDirect3DDevice9_SetTexture(device, 0, texture));
    CHECK(IDirect3DDevice9_BeginScene(device));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0])));
    CHECK(IDirect3DDevice9_EndScene(device));
    DWORD value = read_pixel(target, 16, 16);
    CHECK(IDirect3DDevice9_SetTexture(device, 0, NULL));
    CHECK(IDirect3DDevice9_SetPixelShader(device, NULL));
    IDirect3DPixelShader9_Release(shader);
    return value;
}

static const char *sample2d = "sampler2D s:register(s0);float4 main():COLOR{return tex2D(s,float2(.5,.5));}";
static void fill(D3DLOCKED_RECT lock, UINT w, UINT h, UINT bytes, DWORD value) {
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++)
            memcpy((BYTE *)lock.pBits + y * lock.Pitch + x * bytes, &value, bytes);
}

static void texture_case(const char *name, D3DFORMAT format, UINT bytes, DWORD stored, DWORD expected) {
    IDirect3DTexture9 *src, *dst;
    D3DLOCKED_RECT lock;
    CHECK(IDirect3DDevice9_CreateTexture(device, 16, 16, 1, 0, format, D3DPOOL_SYSTEMMEM, &src, NULL));
    CHECK(IDirect3DDevice9_CreateTexture(device, 16, 16, 1, 0, format, D3DPOOL_DEFAULT, &dst, NULL));
    CHECK(IDirect3DTexture9_LockRect(src, 0, &lock, NULL, 0));
    fill(lock, 16, 16, bytes, 0);
    CHECK(IDirect3DTexture9_UnlockRect(src, 0));
    /* Native video decoders retain the registered CPU buffer. AddDirtyRect
     * announces the new contents without another lock/unlock pair. */
    fill(lock, 16, 16, bytes, stored);
    CHECK(IDirect3DTexture9_AddDirtyRect(src, NULL));
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
    expect(name, sample((IDirect3DBaseTexture9 *)dst, sample2d), expected);
    IDirect3DTexture9_Release(dst); IDirect3DTexture9_Release(src);
}

static void mip_case(void) {
    IDirect3DTexture9 *src, *dst;
    D3DLOCKED_RECT locks[2];
    CHECK(IDirect3DDevice9_CreateTexture(device, 16, 16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
    CHECK(IDirect3DDevice9_CreateTexture(device, 16, 16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, NULL));
    for (UINT level = 0; level < 2; level++) {
        CHECK(IDirect3DTexture9_LockRect(src, level, &locks[level], NULL, 0));
        fill(locks[level], 16 >> level, 16 >> level, 4, 0xff000000);
        CHECK(IDirect3DTexture9_UnlockRect(src, level));
        fill(locks[level], 16 >> level, 16 >> level, 4, level ? 0xff00ff00 : 0xffff0000);
    }
    CHECK(IDirect3DTexture9_AddDirtyRect(src, NULL));
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
    expect("mip zero", sample((IDirect3DBaseTexture9 *)dst, sample2d), 0xffff0000);
    /* Read the mip through its surface view, independently of shader LOD translation. */
    IDirect3DSurface9 *mip;
    CHECK(IDirect3DTexture9_GetSurfaceLevel(dst, 1, &mip));
    CHECK(IDirect3DDevice9_StretchRect(device, mip, NULL, target, NULL, D3DTEXF_POINT));
    expect("mip one", read_pixel(target, 16, 16), 0xff00ff00);
    IDirect3DSurface9_Release(mip);
    fill(locks[0], 16, 16, 4, 0xff0000ff);
    CHECK(IDirect3DTexture9_AddDirtyRect(src, NULL));
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
    expect("repeated update", sample((IDirect3DBaseTexture9 *)dst, sample2d), 0xff0000ff);
    CHECK(IDirect3DTexture9_LockRect(src, 0, &locks[0], NULL, 0));
    fill(locks[0], 16, 16, 4, 0xff00ffff);
    CHECK(IDirect3DTexture9_UnlockRect(src, 0));
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
    expect("ordinary lock update", sample((IDirect3DBaseTexture9 *)dst, sample2d), 0xff00ffff);
    /* The CPU mirror is stale after GPU readback into a system-memory level. */
    IDirect3DSurface9 *surface;
    CHECK(IDirect3DTexture9_GetSurfaceLevel(src, 0, &surface));
    IDirect3DSurface9 *small;
    CHECK(IDirect3DDevice9_CreateRenderTarget(device, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &small, NULL));
    CHECK(IDirect3DDevice9_ColorFill(device, small, NULL, 0xffff00ff));
    CHECK(IDirect3DDevice9_GetRenderTargetData(device, small, surface));
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
    expect("preserve GPU-written texture", sample((IDirect3DBaseTexture9 *)dst, sample2d), 0xffff00ff);
    IDirect3DSurface9_Release(small); IDirect3DSurface9_Release(surface);
    IDirect3DTexture9_Release(dst); IDirect3DTexture9_Release(src);
}

static void surface_case(void) {
    IDirect3DSurface9 *src;
    D3DLOCKED_RECT lock;
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
    CHECK(IDirect3DSurface9_LockRect(src, &lock, NULL, 0));
    fill(lock, 32, 32, 4, 0xff000000);
    CHECK(IDirect3DSurface9_UnlockRect(src));
    fill(lock, 32, 32, 4, 0xffffff00);
    CHECK(IDirect3DDevice9_ColorFill(device, target, NULL, 0xff0000ff));
    RECT rect = {4,4,12,12}; POINT point = {8,8};
    CHECK(IDirect3DDevice9_UpdateSurface(device, src, &rect, target, &point));
    expect("surface rectangle", read_pixel(target, 12, 12), 0xffffff00);
    expect("surface outside rectangle", read_pixel(target, 2, 2), 0xff0000ff);
    CHECK(IDirect3DDevice9_ColorFill(device, target, NULL, 0xffff00ff));
    CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, src));
    CHECK(IDirect3DDevice9_ColorFill(device, target, NULL, 0xff000000));
    CHECK(IDirect3DDevice9_UpdateSurface(device, src, NULL, target, NULL));
    expect("preserve GPU-written surface", read_pixel(target, 16, 16), 0xffff00ff);
    IDirect3DSurface9_Release(src);
}

static void cube_case(void) {
    IDirect3DCubeTexture9 *src, *dst;
    const DWORD colors[6] = {0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,0xff00ffff,0xffff00ff};
    const char *directions[6] = {"1,0,0","-1,0,0","0,1,0","0,-1,0","0,0,1","0,0,-1"};
    CHECK(IDirect3DDevice9_CreateCubeTexture(device, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
    CHECK(IDirect3DDevice9_CreateCubeTexture(device, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, NULL));
    for (UINT face = 0; face < 6; face++) {
        D3DLOCKED_RECT lock;
        CHECK(IDirect3DCubeTexture9_LockRect(src, (D3DCUBEMAP_FACES)face, 0, &lock, NULL, 0));
        fill(lock, 16, 16, 4, 0xff000000);
        CHECK(IDirect3DCubeTexture9_UnlockRect(src, (D3DCUBEMAP_FACES)face, 0));
        fill(lock, 16, 16, 4, colors[face]);
        CHECK(IDirect3DCubeTexture9_AddDirtyRect(src, (D3DCUBEMAP_FACES)face, NULL));
    }
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
    for (UINT face = 0; face < 6; face++) {
        char program[256], name[64];
        snprintf(program, sizeof(program), "samplerCUBE s:register(s0);float4 main():COLOR{return texCUBE(s,float3(%s));}", directions[face]);
        snprintf(name, sizeof(name), "cube face %u", face);
        expect(name, sample((IDirect3DBaseTexture9 *)dst, program), colors[face]);
    }
    IDirect3DCubeTexture9_Release(dst); IDirect3DCubeTexture9_Release(src);
}

static void volume_case(void) {
    IDirect3DVolumeTexture9 *src, *dst; D3DLOCKED_BOX lock;
    CHECK(IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
    CHECK(IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, NULL));
    CHECK(IDirect3DVolumeTexture9_LockBox(src, 0, &lock, NULL, 0));
    memset(lock.pBits, 0, 4 * lock.SlicePitch);
    CHECK(IDirect3DVolumeTexture9_UnlockBox(src, 0));
    for (UINT z=0; z<4; z++) for (UINT y=0; y<4; y++) for (UINT x=0; x<4; x++)
        *(DWORD *)((BYTE *)lock.pBits + z*lock.SlicePitch + y*lock.RowPitch + x*4) = 0xffff8000;
    CHECK(IDirect3DVolumeTexture9_AddDirtyBox(src, NULL));
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
    expect("volume", sample((IDirect3DBaseTexture9 *)dst,
        "sampler3D s:register(s0);float4 main():COLOR{return tex3D(s,float3(.5,.5,.5));}"), 0xffff8000);
    IDirect3DVolumeTexture9_Release(dst); IDirect3DVolumeTexture9_Release(src);
}

static void compressed_case(void) {
    IDirect3DTexture9 *src, *dst; D3DLOCKED_RECT lock;
    CHECK(IDirect3DDevice9_CreateTexture(device,16,16,1,0,D3DFMT_DXT1,D3DPOOL_SYSTEMMEM,&src,NULL));
    CHECK(IDirect3DDevice9_CreateTexture(device,16,16,1,0,D3DFMT_DXT1,D3DPOOL_DEFAULT,&dst,NULL));
    CHECK(IDirect3DTexture9_LockRect(src,0,&lock,NULL,0));
    memset(lock.pBits,0,4*lock.Pitch);
    CHECK(IDirect3DTexture9_UnlockRect(src,0));
    for (UINT y=0; y<4; y++) for (UINT x=0; x<4; x++) {
        BYTE *block=(BYTE *)lock.pBits+y*lock.Pitch+x*8;
        *(WORD *)block=0x07e0; *(WORD *)(block+2)=0; *(DWORD *)(block+4)=0;
    }
    CHECK(IDirect3DTexture9_AddDirtyRect(src,NULL));
    CHECK(IDirect3DDevice9_UpdateTexture(device,(IDirect3DBaseTexture9 *)src,(IDirect3DBaseTexture9 *)dst));
    expect("DXT1 blocks",sample((IDirect3DBaseTexture9 *)dst,sample2d),0xff00ff00);
    IDirect3DTexture9_Release(dst); IDirect3DTexture9_Release(src);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HWND window = CreateWindowExA(0,"STATIC","D3D9 system memory validation",WS_OVERLAPPEDWINDOW,
                                 0,0,96,96,NULL,NULL,GetModuleHandleA(NULL),NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) return 2;
    D3DPRESENT_PARAMETERS pp={0}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window; pp.BackBufferWidth=pp.BackBufferHeight=32;
    pp.BackBufferFormat=D3DFMT_A8R8G8B8; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    CHECK(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device));
    CHECK(IDirect3DDevice9_CreateRenderTarget(device,32,32,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device,32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL));
    CHECK(IDirect3DDevice9_SetRenderTarget(device,0,target));
    CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHABLENDENABLE,FALSE));
    CHECK(IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MIPFILTER,D3DTEXF_POINT));
    ID3DBlob *code=compile("float4 main(float4 p:POSITION):POSITION{return p;}","vs_3_0");
    IDirect3DVertexShader9 *vs; IDirect3DVertexDeclaration9 *decl;
    CHECK(IDirect3DDevice9_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(code),&vs));
    ID3D10Blob_Release(code);
    const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
    CHECK(IDirect3DDevice9_CreateVertexDeclaration(device,elements,&decl));
    CHECK(IDirect3DDevice9_SetVertexDeclaration(device,decl));
    CHECK(IDirect3DDevice9_SetVertexShader(device,vs));
    texture_case("ARGB",D3DFMT_A8R8G8B8,4,0xffff0000,0xffff0000);
    texture_case("L8 video plane",D3DFMT_L8,1,128,0xff808080);
    texture_case("A8",D3DFMT_A8,1,200,0xc8000000);
    texture_case("expanded A4R4G4B4",D3DFMT_A4R4G4B4,2,0xf0f0,0xff00ff00);
    compressed_case(); mip_case(); surface_case(); cube_case(); volume_case();
    CHECK(IDirect3DDevice9_SetVertexShader(device,NULL));
    CHECK(IDirect3DDevice9_SetVertexDeclaration(device,NULL));
    IDirect3DVertexDeclaration9_Release(decl); IDirect3DVertexShader9_Release(vs);
    IDirect3DSurface9_Release(readback); IDirect3DSurface9_Release(target);
    IDirect3DDevice9_Release(device); IDirect3D9_Release(d3d); DestroyWindow(window);
    printf("RESULT checks=%d failures=%d\n",checks,failures);
    return failures ? 1 : 0;
}
