/* Build with either i686- or x86_64-w64-mingw32-gcc:
 *   -O2 scripts/d3d9-sampler-benchmark.c -o sampler-benchmark.exe \
 *       -ld3d9 -ld3dcompiler -luser32
 * Run with synchronous pipeline compilation. Pixel readback checks precede
 * CPU submission measurements; these timings are not game FPS measurements.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { HRESULT result = (call); if (FAILED(result)) { \
  fprintf(stderr, "%s failed: %08lx at line %d\n", #call, (unsigned long)result, __LINE__); \
  exit(1); } } while (0)

static IDirect3DDevice9 *device;
static IDirect3DSurface9 *target, *readback;
static LARGE_INTEGER frequency;

static ID3DBlob *compile(const char *source, const char *profile) {
  ID3DBlob *code = NULL, *errors = NULL;
  HRESULT result = D3DCompile(source, strlen(source), "sampler-test", NULL, NULL,
                             "main", profile, 0, 0, &code, &errors);
  if (FAILED(result)) {
    if (errors) fprintf(stderr, "%s\n", (char *)ID3D10Blob_GetBufferPointer(errors));
    CHECK(result);
  }
  if (errors) ID3D10Blob_Release(errors);
  return code;
}

static void draw(void) {
  CHECK(IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 2));
}

static DWORD read_pixel(void) {
  D3DLOCKED_RECT lock;
  DWORD pixel;
  CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, readback));
  CHECK(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY));
  pixel = *(DWORD *)((BYTE *)lock.pBits + lock.Pitch * 8 + 8 * sizeof(DWORD));
  CHECK(IDirect3DSurface9_UnlockRect(readback));
  return pixel;
}

static void expect_pixel(const char *label, int r, int g, int b) {
  draw();
  DWORD pixel = read_pixel();
  int actual[3] = {(pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255};
  if (abs(actual[0] - r) > 3 || abs(actual[1] - g) > 3 || abs(actual[2] - b) > 3) {
    fprintf(stderr, "FAIL %s: %08lx, expected rgb(%d,%d,%d)\n",
            label, (unsigned long)pixel, r, g, b);
    exit(2);
  }
  printf("PASS %s: %08lx\n", label, (unsigned long)pixel);
}

static void filter(DWORD stage, DWORD value) {
  CHECK(IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MINFILTER, value));
  CHECK(IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MAGFILTER, value));
}

int main(int argc, char **argv) {
  const char *vs_source =
      "struct O { float4 p:POSITION; float2 uv:TEXCOORD0; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.uv=float2(0.5,0.5); return o; }";
  const char *ps_source =
      "sampler2D image:register(s0);"
      "float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(image,uv); }";
  const char *vertex_sample_source =
      "sampler2D image:register(s0);"
      "struct O { float4 p:POSITION; float4 c:COLOR0; };"
      "O main(float4 p:POSITION) { O o; o.p=p;"
      "o.c=tex2Dlod(image,float4(0.5,0.5,0,0)); return o; }";
  const char *colour_source = "float4 main(float4 c:COLOR0):COLOR0 { return c; }";
  const char *shadow_source =
      "sampler2D image:register(s0);"
      "float4 main():COLOR0 { return tex2Dproj(image,float4(0.5,0.5,0.75,1)); }";
  const float vertices[6][4] = {
    {-1,-1,0,1}, {-1,1,0,1}, {1,1,0,1},
    {-1,-1,0,1}, {1,1,0,1}, {1,-1,0,1}
  };
  const D3DVERTEXELEMENT9 declaration[] = {
    {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()
  };
  IDirect3D9 *d3d;
  IDirect3DVertexShader9 *vs, *vs_sample;
  IDirect3DPixelShader9 *ps, *ps_colour, *ps_shadow;
  IDirect3DVertexDeclaration9 *decl;
  IDirect3DVertexBuffer9 *vb;
  IDirect3DTexture9 *texture, *grey, *shadow, *blit_source;
  IDirect3DStateBlock9 *state;
  IDirect3DSurface9 *blit_target, *texture_surface, *shadow_surface;
  ID3DBlob *code;
  D3DLOCKED_RECT lock;
  void *mapped;
  D3DPRESENT_PARAMETERS pp = {0};
  HWND window;
  unsigned draws = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 10000;
  if (!draws || draws > 1000000) return 3;
  setvbuf(stdout, NULL, _IONBF, 0);
  QueryPerformanceFrequency(&frequency);
  window = CreateWindowExA(0, "STATIC", "D3D9 sampler validation", WS_OVERLAPPEDWINDOW,
                            0, 0, 64, 64, NULL, NULL, GetModuleHandleA(NULL), NULL);
  if (!window) return 4;
  d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 5;
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.BackBufferWidth = pp.BackBufferHeight = 16;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window,
                               D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
  CHECK(IDirect3DDevice9_CreateRenderTarget(device, 16, 16, D3DFMT_A8R8G8B8,
                                           D3DMULTISAMPLE_NONE, 0, FALSE, &target, NULL));
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 16, 16, D3DFMT_A8R8G8B8,
                                                    D3DPOOL_SYSTEMMEM, &readback, NULL));
  CHECK(IDirect3DDevice9_SetRenderTarget(device, 0, target));
  CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE));
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(device, declaration, &decl));
  CHECK(IDirect3DDevice9_SetVertexDeclaration(device, decl));
  CHECK(IDirect3DDevice9_CreateVertexBuffer(device, sizeof(vertices), D3DUSAGE_WRITEONLY, 0,
                                           D3DPOOL_DEFAULT, &vb, NULL));
  CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &mapped, 0));
  memcpy(mapped, vertices, sizeof(vertices));
  CHECK(IDirect3DVertexBuffer9_Unlock(vb));
  CHECK(IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(vertices[0])));
  code = compile(vs_source, "vs_3_0");
  CHECK(IDirect3DDevice9_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(code), &vs));
  ID3D10Blob_Release(code);
  code = compile(ps_source, "ps_3_0");
  CHECK(IDirect3DDevice9_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(code), &ps));
  ID3D10Blob_Release(code);
  code = compile(vertex_sample_source, "vs_3_0");
  CHECK(IDirect3DDevice9_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(code), &vs_sample));
  ID3D10Blob_Release(code);
  code = compile(colour_source, "ps_3_0");
  CHECK(IDirect3DDevice9_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(code), &ps_colour));
  ID3D10Blob_Release(code);
  code = compile(shadow_source, "ps_3_0");
  CHECK(IDirect3DDevice9_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(code), &ps_shadow));
  ID3D10Blob_Release(code);
  CHECK(IDirect3DDevice9_SetVertexShader(device, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(device, ps));
  CHECK(IDirect3DDevice9_CreateTexture(device, 2, 1, 1, 0, D3DFMT_A8R8G8B8,
                                      D3DPOOL_MANAGED, &texture, NULL));
  CHECK(IDirect3DTexture9_LockRect(texture, 0, &lock, NULL, 0));
  ((DWORD *)lock.pBits)[0] = 0xffff0000;
  ((DWORD *)lock.pBits)[1] = 0xff00ff00;
  CHECK(IDirect3DTexture9_UnlockRect(texture, 0));
  CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture));
  CHECK(IDirect3DDevice9_BeginScene(device));
  filter(0, D3DTEXF_POINT);
  expect_pixel("pixel point", 0, 255, 0);
  filter(0, D3DTEXF_LINEAR);
  expect_pixel("pixel linear", 128, 128, 0);
  filter(0, D3DTEXF_POINT);
  expect_pixel("pixel point restored", 0, 255, 0);
  CHECK(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_PIXELSTATE, &state));
  filter(0, D3DTEXF_LINEAR);
  expect_pixel("state block changed", 128, 128, 0);
  CHECK(IDirect3DStateBlock9_Apply(state));
  expect_pixel("state block restored", 0, 255, 0);
  IDirect3DStateBlock9_Release(state);
  CHECK(IDirect3DDevice9_CreateTexture(device, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
                                      D3DPOOL_MANAGED, &grey, NULL));
  CHECK(IDirect3DTexture9_LockRect(grey, 0, &lock, NULL, 0));
  *(DWORD *)lock.pBits = 0xff808080;
  CHECK(IDirect3DTexture9_UnlockRect(grey, 0));
  CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)grey));
  expect_pixel("linear texture", 128, 128, 128);
  CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_SRGBTEXTURE, TRUE));
  expect_pixel("sRGB texture", 55, 55, 55);
  CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_SRGBTEXTURE, FALSE));
  CHECK(IDirect3DDevice9_CreateTexture(device, 16, 16, 1, D3DUSAGE_DEPTHSTENCIL,
                                      D3DFMT_D16, D3DPOOL_DEFAULT, &shadow, NULL));
  CHECK(IDirect3DTexture9_GetSurfaceLevel(shadow, 0, &shadow_surface));
  CHECK(IDirect3DDevice9_SetDepthStencilSurface(device, shadow_surface));
  CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 0.25f, 0));
  CHECK(IDirect3DDevice9_SetDepthStencilSurface(device, NULL));
  CHECK(IDirect3DDevice9_SetPixelShader(device, ps_shadow));
  CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)shadow));
  expect_pixel("shadow comparison without sampler state change", 0, 0, 0);
  CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)grey));
  expect_pixel("colour after shadow without sampler state change", 128, 128, 128);
  CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)shadow));
  expect_pixel("shadow comparison restored", 0, 0, 0);
  CHECK(IDirect3DDevice9_SetPixelShader(device, ps));
  CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture));
  // StretchRect requires default-pool surfaces and must run outside a scene.
  CHECK(IDirect3DDevice9_EndScene(device));
  CHECK(IDirect3DDevice9_CreateTexture(device, 2, 1, 1, D3DUSAGE_RENDERTARGET,
                                      D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &blit_source, NULL));
  CHECK(IDirect3DDevice9_CreateRenderTarget(device, 8, 8, D3DFMT_A8R8G8B8,
                                           D3DMULTISAMPLE_NONE, 0, FALSE, &blit_target, NULL));
  CHECK(IDirect3DTexture9_GetSurfaceLevel(blit_source, 0, &texture_surface));
  CHECK(IDirect3DDevice9_ColorFill(device, texture_surface, NULL, 0xff0000ff));
  CHECK(IDirect3DDevice9_StretchRect(device, texture_surface, NULL, blit_target, NULL, D3DTEXF_LINEAR));
  CHECK(IDirect3DDevice9_BeginScene(device));
  expect_pixel("sampler restored after StretchRect", 0, 255, 0);
  IDirect3DSurface9_Release(texture_surface);
  IDirect3DTexture9_Release(blit_source);
  IDirect3DSurface9_Release(blit_target);
  CHECK(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0, (IDirect3DBaseTexture9 *)texture));
  CHECK(IDirect3DDevice9_SetVertexShader(device, vs_sample));
  CHECK(IDirect3DDevice9_SetPixelShader(device, ps_colour));
  filter(D3DVERTEXTEXTURESAMPLER0, D3DTEXF_POINT);
  expect_pixel("vertex point", 0, 255, 0);
  filter(D3DVERTEXTEXTURESAMPLER0, D3DTEXF_LINEAR);
  expect_pixel("vertex linear", 128, 128, 0);
  CHECK(IDirect3DDevice9_SetVertexShader(device, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(device, ps));
  for (unsigned mode = 0; mode < 3; ++mode) {
    for (unsigned i = 0; i < 256; ++i) draw();
    read_pixel();
    LONGLONG ticks = 0;
    DWORD pixel = 0;
    // Drain between bounded batches, outside the measured submission time.
    // A long stream without Present can exhaust command allocator storage.
    for (unsigned batch = 0; batch < draws; batch += 512) {
      unsigned limit = draws - batch < 512 ? draws : batch + 512;
      LARGE_INTEGER begin, end;
      QueryPerformanceCounter(&begin);
      for (unsigned i = batch; i < limit; ++i) {
        if (mode == 1 && i % 64 == 0) filter(0, (i / 64) % 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        if (mode == 2) filter(0, i % 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        draw();
      }
      QueryPerformanceCounter(&end);
      ticks += end.QuadPart - begin.QuadPart;
      pixel = read_pixel();
    }
    double us = ticks * 1000000.0 / frequency.QuadPart / draws;
    printf("BENCH {\"mode\":\"%s\",\"draws\":%u,\"submit_us_per_draw\":%.6f,\"pixel\":%lu}\n",
           mode == 0 ? "stable" : mode == 1 ? "change_every_64" : "change_every_draw",
           draws, us, (unsigned long)pixel);
  }
  CHECK(IDirect3DDevice9_EndScene(device));
  CHECK(IDirect3DDevice9_SetTexture(device, 0, NULL));
  CHECK(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0, NULL));
  CHECK(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0));
  CHECK(IDirect3DDevice9_SetVertexDeclaration(device, NULL));
  CHECK(IDirect3DDevice9_SetVertexShader(device, NULL));
  CHECK(IDirect3DDevice9_SetPixelShader(device, NULL));
  IDirect3DTexture9_Release(grey);
  IDirect3DTexture9_Release(texture);
  IDirect3DSurface9_Release(shadow_surface);
  IDirect3DTexture9_Release(shadow);
  IDirect3DVertexShader9_Release(vs_sample);
  IDirect3DPixelShader9_Release(ps_colour);
  IDirect3DVertexShader9_Release(vs);
  IDirect3DPixelShader9_Release(ps);
  IDirect3DPixelShader9_Release(ps_shadow);
  IDirect3DVertexDeclaration9_Release(decl);
  IDirect3DVertexBuffer9_Release(vb);
  IDirect3DSurface9_Release(target);
  IDirect3DSurface9_Release(readback);
  IDirect3DDevice9_Release(device);
  IDirect3D9_Release(d3d);
  DestroyWindow(window);
  puts("PASS all sampler checks");
  return 0;
}
