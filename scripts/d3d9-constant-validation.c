/* GPU checks for shader constant uploads. CPU submission timings are not game FPS. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { \
    printf("FAIL %s: %08lx at %d\n", #call, (unsigned long)hr_, __LINE__); return 1; } } while (0)

static IDirect3DDevice9 *device;
static IDirect3DSurface9 *target, *readback;
static int failures;

static ID3DBlob *compile(const char *text, const char *profile) {
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(text, strlen(text), "constant-test", NULL, NULL, "main", profile, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) printf("%s\n", (char *)ID3D10Blob_GetBufferPointer(errors));
        exit(3);
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

static IDirect3DVertexShader9 *vertex(const char *text) {
    IDirect3DVertexShader9 *shader = NULL;
    ID3DBlob *code = compile(text, "vs_3_0");
    if (FAILED(IDirect3DDevice9_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(code), &shader))) exit(4);
    ID3D10Blob_Release(code);
    return shader;
}

static ID3DBlob *assemble(const char *text) {
    typedef HRESULT (WINAPI *Assemble)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *, UINT, ID3DBlob **, ID3DBlob **);
    Assemble function = (Assemble)GetProcAddress(GetModuleHandleA("d3dcompiler_47.dll"), "D3DAssemble");
    ID3DBlob *code = NULL, *errors = NULL;
    if (!function || FAILED(function(text, strlen(text), "extra-constants", NULL, NULL, 0, &code, &errors))) {
        if (errors) printf("%s\n", (char *)ID3D10Blob_GetBufferPointer(errors));
        exit(3);
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

static IDirect3DPixelShader9 *pixel(const char *text) {
    IDirect3DPixelShader9 *shader = NULL;
    ID3DBlob *code = !strncmp(text, "ps_3_0\n", 7) ? assemble(text) : compile(text, "ps_3_0");
    if (FAILED(IDirect3DDevice9_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(code), &shader))) exit(4);
    ID3D10Blob_Release(code);
    return shader;
}

static DWORD read_pixel(void) {
    D3DLOCKED_RECT lock;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(device, target, readback))) exit(5);
    if (FAILED(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY))) exit(5);
    DWORD value = *(DWORD *)((BYTE *)lock.pBits + 8 * lock.Pitch + 8 * 4);
    IDirect3DSurface9_UnlockRect(readback);
    return value;
}

static int matches(DWORD value, int r, int g, int b) {
    return abs((int)((value >> 16) & 255) - r) <= 3 &&
           abs((int)((value >> 8) & 255) - g) <= 3 && abs((int)(value & 255) - b) <= 3;
}

static void expect(const char *name, int r, int g, int b) {
    if (FAILED(IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 2))) exit(6);
    DWORD value = read_pixel();
    if (!matches(value, r, g, b)) { printf("FAIL %s: %08lx\n", name, (unsigned long)value); failures++; }
    else printf("PASS %s\n", name);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HWND window = CreateWindowExA(0, "STATIC", "D3D9 constant validation", WS_OVERLAPPEDWINDOW,
                                  0, 0, 96, 96, NULL, NULL, GetModuleHandleA(NULL), NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d || !window) return 2;
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    CHECK(IDirect3DDevice9_CreateRenderTarget(device, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, NULL));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL));
    CHECK(IDirect3DDevice9_SetRenderTarget(device, 0, target));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE));
    const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    IDirect3DVertexDeclaration9 *declaration;
    IDirect3DVertexBuffer9 *vb;
    CHECK(IDirect3DDevice9_CreateVertexDeclaration(device, elements, &declaration));
    CHECK(IDirect3DDevice9_SetVertexDeclaration(device, declaration));
    const float vertices[6][4] = {{-1,-1,1,1},{-1,1,1,1},{1,1,1,1},{-1,-1,1,1},{1,1,1,1},{1,-1,1,1}};
    CHECK(IDirect3DDevice9_CreateVertexBuffer(device, sizeof(vertices), 0, 0, D3DPOOL_DEFAULT, &vb, NULL));
    void *mapped;
    CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &mapped, 0)); memcpy(mapped, vertices, sizeof(vertices));
    CHECK(IDirect3DVertexBuffer9_Unlock(vb));
    CHECK(IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(vertices[0])));
    IDirect3DVertexShader9 *low = vertex("float4 c:register(c0);struct O{float4 p:POSITION;float4 c:COLOR0;};O main(float4 p:POSITION){O o;o.p=float4(p.xy,0,1);o.c=c;return o;}");
    IDirect3DVertexShader9 *high = vertex("float4 c:register(c255);struct O{float4 p:POSITION;float4 c:COLOR0;};O main(float4 p:POSITION){O o;o.p=float4(p.xy,0,1);o.c=c;return o;}");
    IDirect3DVertexShader9 *relative = vertex("float4 palette[256]:register(c0);struct O{float4 p:POSITION;float4 c:COLOR0;};O main(float4 p:POSITION){O o;o.p=float4(p.xy,0,1);o.c=palette[(int)(p.z*200)];return o;}");
    IDirect3DPixelShader9 *colour = pixel("float4 main(float4 c:COLOR0):COLOR0{return c;}");
    IDirect3DPixelShader9 *ps_low = pixel("float4 c:register(c0);float4 main():COLOR0{return c;}");
    IDirect3DPixelShader9 *ps_high = pixel("float4 c:register(c223);float4 main():COLOR0{return c;}");
    IDirect3DPixelShader9 *extra = pixel("ps_3_0\ndef c0, 0, 0, 1, 1\ndef c1, 0, 0, 0, 0\nmov r0, c1\nif b0\nrep i0\nadd r0, r0, c0\nendrep\nendif\nmov oC0, r0\n");
    const float red[4] = {1,0,0,1}, green[4] = {0,1,0,1}, blue[4] = {0,0,1,1}, yellow[4] = {1,1,0,1};
    CHECK(IDirect3DDevice9_BeginScene(device));
    CHECK(IDirect3DDevice9_SetVertexShader(device, low)); CHECK(IDirect3DDevice9_SetPixelShader(device, colour));
    CHECK(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, green, 1));
    CHECK(IDirect3DDevice9_SetVertexShaderConstantF(device, 255, red, 1));
    CHECK(IDirect3DDevice9_SetVertexShaderConstantF(device, 200, yellow, 1));
    expect("low vertex register", 0,255,0);
    CHECK(IDirect3DDevice9_SetVertexShader(device, high)); expect("larger vertex span without a setter", 255,0,0);
    CHECK(IDirect3DDevice9_SetVertexShader(device, relative)); expect("relative vertex constant indexing", 255,255,0);
    CHECK(IDirect3DDevice9_SetVertexShader(device, low)); CHECK(IDirect3DDevice9_SetPixelShader(device, ps_low));
    CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, red, 1));
    CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 223, blue, 1));
    expect("low pixel register", 255,0,0);
    CHECK(IDirect3DDevice9_SetPixelShader(device, ps_high)); expect("larger pixel span without a setter", 0,0,255);
    CHECK(IDirect3DDevice9_SetPixelShader(device, ps_low));
    IDirect3DStateBlock9 *state;
    CHECK(IDirect3DDevice9_BeginStateBlock(device)); CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, green, 1));
    CHECK(IDirect3DDevice9_EndStateBlock(device, &state)); CHECK(IDirect3DStateBlock9_Apply(state));
    expect("recorded constants apply", 0,255,0);
    CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, red, 1)); CHECK(IDirect3DStateBlock9_Apply(state));
    expect("state block reapplies after an intervening setter", 0,255,0);
    CHECK(IDirect3DDevice9_SetPixelShader(device, extra));
    const int ints[4] = {1,0,0,0}; const BOOL enabled = TRUE;
    CHECK(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, ints, 1));
    CHECK(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, &enabled, 1)); expect("integer and boolean extra constants", 0,0,255);
    const BOOL disabled = FALSE, noncanonical_true = 7;
    CHECK(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, &disabled, 1)); expect("boolean constant changes", 0,0,0);
    CHECK(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, &noncanonical_true, 1));
    CHECK(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, &enabled, 1)); expect("boolean normalization and unchanged setter", 0,0,255);
    const int zero_ints[4] = {0,0,0,0};
    CHECK(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, zero_ints, 1)); expect("integer constant changes", 0,0,0);
    CHECK(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, ints, 1));
    CHECK(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, ints, 1)); expect("integer unchanged setter", 0,0,255);


    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE));
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    for (unsigned stage = 0; stage < 2; stage++) {
        CHECK(IDirect3DDevice9_SetVertexShader(device, low));
        CHECK(IDirect3DDevice9_SetPixelShader(device, stage ? ps_low : colour));
        LARGE_INTEGER begin, end; QueryPerformanceCounter(&begin);
        for (unsigned i = 0; i < 4096; i++) {
            float value[4] = {((i*37)&255)/255.f, ((i*13)&255)/255.f, ((i*83)&255)/255.f, 1};
            RECT scissor = {(LONG)(i%64), (LONG)(i/64), (LONG)(i%64+1), (LONG)(i/64+1)};
            CHECK(IDirect3DDevice9_SetScissorRect(device, &scissor));
            if (stage) CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, value, 1));
            else CHECK(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, value, 1));
            CHECK(IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 2));
        }
        QueryPerformanceCounter(&end);
        CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, readback));
        D3DLOCKED_RECT lock; CHECK(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY));
        unsigned bad = 0;
        for (unsigned i = 0; i < 4096; i++) {
            DWORD value = *(DWORD *)((BYTE *)lock.pBits + (i/64)*lock.Pitch + (i%64)*4);
            if (!matches(value, (i*37)&255, (i*13)&255, (i*83)&255)) bad++;
        }
        CHECK(IDirect3DSurface9_UnlockRect(readback));
        printf("%s %s ring wraps: %u mismatched pixels, submission %.3f ms\n", bad ? "FAIL" : "PASS", stage ? "pixel" : "vertex", bad,
               (end.QuadPart-begin.QuadPart)*1000.0/frequency.QuadPart);
        if (bad) failures++;
    }
    // Keep the PS snapshot unchanged while only the VS ring wraps repeatedly.
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE));
    CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, blue, 1)); expect("pixel snapshot before vertex wraps", 0,0,255);
    for (unsigned i = 0; i < 600; i++) {
        float value[4] = {i/600.f,0,0,1}; CHECK(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, value, 1));
        CHECK(IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 2));
    }
    expect("vertex wraps preserve unchanged pixel constants", 0,0,255);
    CHECK(IDirect3DDevice9_SetVertexShader(device, NULL));
    expect("fixed vertex stage with programmable pixel constants", 0,0,255);
    CHECK(IDirect3DDevice9_SetVertexShader(device, low));
    expect("vertex constants restored after fixed function", 0,0,255);
    CHECK(IDirect3DDevice9_SetPixelShader(device, NULL));
    CHECK(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, green, 1));
    expect("programmable vertex constants with fixed pixel stage", 0,255,0);
    CHECK(IDirect3DDevice9_SetPixelShader(device, ps_low));
    expect("pixel constants restored after fixed function", 0,0,255);
    IDirect3DSurface9 *blit_target;
    CHECK(IDirect3DDevice9_CreateRenderTarget(device, 32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &blit_target, NULL));
    CHECK(IDirect3DDevice9_EndScene(device));
    CHECK(IDirect3DDevice9_StretchRect(device, target, NULL, blit_target, NULL, D3DTEXF_LINEAR));
    CHECK(IDirect3DDevice9_BeginScene(device));
    expect("unchanged constant ranges restored after StretchRect", 0,0,255);
    CHECK(IDirect3DDevice9_EndScene(device));
    printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
