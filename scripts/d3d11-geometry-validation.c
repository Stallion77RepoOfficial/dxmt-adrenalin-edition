/* Pixel validation for geometry shaders emitting point lists.
 * Build: x86_64-w64-mingw32-gcc -O2 -Wall -Wextra \
 *   scripts/d3d11-geometry-validation.c -o geometry-validation.exe \
 *   -ld3d11 -ld3dcompiler
 * Run with DXMT_CONFIG=d3d11.asyncPipelineCompilation=False.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { \
  fprintf(stderr, "%s failed: %08lx (line %d)\n", #call, (unsigned long)hr_, __LINE__); exit(1); \
} } while (0)

static ID3DBlob *compile(const char *source, const char *entry, const char *profile) {
  ID3DBlob *code = NULL, *errors = NULL;
  HRESULT hr = D3DCompile(source, strlen(source), "geometry-validation", NULL, NULL,
      entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
  if (errors) {
    fprintf(stderr, "%s\n", (const char *)ID3D10Blob_GetBufferPointer(errors));
    ID3D10Blob_Release(errors);
  }
  CHECK(hr);
  return code;
}

int main(void) {
  const char *vertex = "float4 main(uint id:SV_VertexID):SV_Position { return float4(0,0,0,1); }";
  const char *pixel = "float4 main():SV_Target { return float4(1,0,0,1); }";
  const char *one =
      "struct V { float4 p:SV_Position; };"
      "[maxvertexcount(1)] void main(point V input[1], inout PointStream<V> s) {"
      "V o; o.p=float4(0.015625,-0.015625,0,1); s.Append(o); s.RestartStrip(); }";
  const char *three =
      "struct V { float4 p:SV_Position; };"
      "[maxvertexcount(3)] void main(point V input[1], inout PointStream<V> s) { V o;"
      "o.p=float4(-0.484375,0.484375,0,1); s.Append(o);"
      "o.p=float4(0.015625,-0.015625,0,1); s.Append(o);"
      "o.p=float4(0.515625,-0.515625,0,1); s.Append(o); s.RestartStrip(); }";
  ID3D11Device *device = NULL;
  ID3D11DeviceContext *context = NULL;
  ID3D11Texture2D *target = NULL, *staging = NULL;
  ID3D11RenderTargetView *rtv = NULL;
  ID3D11VertexShader *vs = NULL;
  ID3D11PixelShader *ps = NULL;
  D3D_FEATURE_LEVEL level, requested = D3D_FEATURE_LEVEL_11_0;
  D3D11_TEXTURE2D_DESC desc = {0};
  D3D11_VIEWPORT viewport = {0, 0, 64, 64, 0, 1};
  const float clear[4] = {0, 0, 0, 1};
  ID3DBlob *code;
  unsigned test;
  setvbuf(stdout, NULL, _IONBF, 0);
  CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &requested, 1,
      D3D11_SDK_VERSION, &device, &level, &context));
  desc.Width = desc.Height = 64;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  CHECK(ID3D11Device_CreateTexture2D(device, &desc, NULL, &target));
  CHECK(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)target, NULL, &rtv));
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging));
  code = compile(vertex, "main", "vs_5_0");
  CHECK(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &vs));
  ID3D10Blob_Release(code);
  code = compile(pixel, "main", "ps_5_0");
  CHECK(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &ps));
  ID3D10Blob_Release(code);
  ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
  ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
  ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
  ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
  ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  for (test = 0; test < 2; ++test) {
    ID3D11GeometryShader *gs = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    unsigned x, y, red = 0, expected = test ? 3 : 1;
    code = compile(test ? three : one, "main", "gs_5_0");
    CHECK(ID3D11Device_CreateGeometryShader(device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &gs));
    ID3D10Blob_Release(code);
    ID3D11DeviceContext_GSSetShader(context, gs, NULL, 0);
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear);
    ID3D11DeviceContext_Draw(context, 1, 0);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)target);
    CHECK(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped));
    for (y = 0; y < 64; ++y) for (x = 0; x < 64; ++x) {
      BYTE *p = (BYTE *)mapped.pData + mapped.RowPitch * y + x * 4;
      if (p[0] > 250 && p[1] < 4 && p[2] < 4) ++red;
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    if (red != expected) {
      fprintf(stderr, "FAIL point-list %u: expected %u red pixels, found %u\n", test, expected, red);
      return 2;
    }
    printf("PASS geometry point-list: %u points, %u pixels\n", expected, red);
    ID3D11GeometryShader_Release(gs);
  }
  ID3D11DeviceContext_ClearState(context);
  ID3D11PixelShader_Release(ps);
  ID3D11VertexShader_Release(vs);
  ID3D11RenderTargetView_Release(rtv);
  ID3D11Texture2D_Release(staging);
  ID3D11Texture2D_Release(target);
  ID3D11DeviceContext_Release(context);
  ID3D11Device_Release(device);
  puts("PASS all geometry checks");
  return 0;
}
