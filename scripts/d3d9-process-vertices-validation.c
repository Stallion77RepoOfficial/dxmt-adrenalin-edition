/* i686-w64-mingw32-gcc -O2 this-file.c -o process-vertices.exe -ld3d9 -luser32 -lm */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { \
  fprintf(stderr, "FAIL line %d: %s = %08lx\n", __LINE__, #call, (unsigned long)hr_); exit(1); } } while (0)
#define REQUIRE(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(2); } } while (0)
#define SRC_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2)
#define DST_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2)
typedef struct { float x,y,z; DWORD color; float uv[4]; } Source;
typedef struct { float x,y,z,rhw; DWORD color; float uv[4]; } Destination;
static IDirect3DDevice9 *device;

static D3DMATRIX identity(void) {
  D3DMATRIX m = {0}; m.m[0][0]=m.m[1][1]=m.m[2][2]=m.m[3][3]=1; return m;
}
static int close_float(float a, float b) { return fabsf(a-b) < 0.0001f; }
static IDirect3DVertexBuffer9 *buffer(UINT bytes, DWORD fvf, const void *data) {
  IDirect3DVertexBuffer9 *vb; void *p;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(device,bytes,0,fvf,D3DPOOL_SYSTEMMEM,&vb,NULL));
  CHECK(IDirect3DVertexBuffer9_Lock(vb,0,bytes,&p,0));
  if (data) memcpy(p,data,bytes); else memset(p,0xcd,bytes);
  CHECK(IDirect3DVertexBuffer9_Unlock(vb)); return vb;
}
static void read_buffer(IDirect3DVertexBuffer9 *vb, void *data, UINT size) {
  void *p; CHECK(IDirect3DVertexBuffer9_Lock(vb,0,size,&p,D3DLOCK_READONLY));
  memcpy(data,p,size); CHECK(IDirect3DVertexBuffer9_Unlock(vb));
}
static void position(Destination *v, float x,float y,float z,float rhw) {
  REQUIRE(close_float(v->x,x)); REQUIRE(close_float(v->y,y));
  REQUIRE(close_float(v->z,z)); REQUIRE(close_float(v->rhw,rhw));
}
static int untouched(const void *p, UINT size) {
  for (UINT i=0;i<size;i++) if (((const BYTE *)p)[i]!=0xcd) return 0; return 1;
}

int main(void) {
  setvbuf(stdout,NULL,_IONBF,0);
  HWND window=CreateWindowExA(0,"STATIC","ProcessVertices validation",WS_OVERLAPPEDWINDOW,
      0,0,64,64,NULL,NULL,GetModuleHandleA(NULL),NULL);
  REQUIRE(window!=NULL);
  IDirect3D9 *d3d=Direct3DCreate9(D3D_SDK_VERSION); REQUIRE(d3d!=NULL);
  D3DPRESENT_PARAMETERS pp={0}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow=window; pp.BackBufferWidth=pp.BackBufferHeight=16;
  pp.BackBufferFormat=D3DFMT_A8R8G8B8; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  CHECK(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device));
  CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE));
  CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_CLIPPING,FALSE));
  D3DVIEWPORT9 vp={10,20,200,100,0.2f,0.8f}; CHECK(IDirect3DDevice9_SetViewport(device,&vp));
  D3DMATRIX m=identity();
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_WORLD,&m));
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_VIEW,&m));
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_PROJECTION,&m));
  Source vertices[3]={{0},{-.5f,.5f,.25f,0xffabcdef,{.2f,.7f,.4f,.6f}},{.25f,.75f,2,0xff123456,{1,2,3,4}}};
  BYTE source_bytes[16+sizeof(vertices)]; memset(source_bytes,0,sizeof(source_bytes));
  memcpy(source_bytes+16,vertices,sizeof(vertices));
  IDirect3DVertexBuffer9 *source=buffer(sizeof(source_bytes),SRC_FVF,source_bytes);
  IDirect3DVertexBuffer9 *destination=buffer(4*sizeof(Destination),DST_FVF,NULL);
  Destination out[4];
  CHECK(IDirect3DDevice9_SetFVF(device,SRC_FVF));
  CHECK(IDirect3DDevice9_SetStreamSource(device,0,source,16,sizeof(Source)));
  CHECK(IDirect3DDevice9_ProcessVertices(device,1,1,1,destination,NULL,0));
  read_buffer(destination,out,sizeof(out)); position(&out[1],60,45,.35f,1);
  REQUIRE(out[1].color==vertices[1].color); REQUIRE(!memcmp(out[1].uv,vertices[1].uv,sizeof(vertices[1].uv)));
  REQUIRE(untouched(&out[0],sizeof(out[0])) && untouched(&out[2],2*sizeof(out[2])));
  puts("PASS viewport, source offset/index, destination index and attribute copy");

  D3DMATRIX world=identity(),view=identity(),projection={0};
  world.m[3][0]=.25f; view.m[3][1]=-.25f;
  projection.m[0][0]=1.5f; projection.m[1][1]=2; projection.m[2][2]=projection.m[2][3]=1;
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_WORLD,&world));
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_VIEW,&view));
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_PROJECTION,&projection));
  CHECK(IDirect3DDevice9_ProcessVertices(device,2,2,1,destination,NULL,D3DPV_DONOTCOPYDATA));
  read_buffer(destination,out,sizeof(out)); position(&out[2],147.5f,45,.8f,.5f);
  REQUIRE(untouched(&out[2].color,sizeof(out[2])-4*sizeof(float)));
  puts("PASS world/view/projection, perspective divide, depth range and DONOTCOPYDATA");

  REQUIRE(FAILED(IDirect3DDevice9_ProcessVertices(device,0,0,1,NULL,NULL,0)));
  REQUIRE(FAILED(IDirect3DDevice9_ProcessVertices(device,0,0xffffffff,1,destination,NULL,0)));
  REQUIRE(FAILED(IDirect3DDevice9_ProcessVertices(device,0xffffffff,0,1,destination,NULL,0)));
  REQUIRE(FAILED(IDirect3DDevice9_ProcessVertices(device,0,0,0xffffffff,destination,NULL,0)));
  REQUIRE(FAILED(IDirect3DDevice9_ProcessVertices(device,0,0,1,destination,NULL,0x80000000)));
  puts("PASS invalid pointers, ranges, overflow and flags");

  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_WORLD,&m));
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_VIEW,&m));
  CHECK(IDirect3DDevice9_SetTransform(device,D3DTS_PROJECTION,&m));
  BYTE overlap[3*sizeof(Destination)]; memset(overlap,0,sizeof(overlap));
  memcpy(overlap,vertices+1,2*sizeof(Source));
  IDirect3DVertexBuffer9 *alias=buffer(sizeof(overlap),DST_FVF,overlap);
  CHECK(IDirect3DDevice9_SetStreamSource(device,0,alias,0,sizeof(Source)));
  CHECK(IDirect3DDevice9_ProcessVertices(device,0,0,2,alias,NULL,0));
  read_buffer(alias,out,2*sizeof(Destination)); position(&out[0],60,45,.35f,1);
  position(&out[1],135,32.5f,1.4f,1); REQUIRE(out[1].color==vertices[2].color);
  REQUIRE(!memcmp(out[1].uv,vertices[2].uv,sizeof(vertices[2].uv)));
  puts("PASS in-place expansion preserves unread source vertices");

  // Draw transformed vertices and check the GPU result, rather than only
  // checking that the method returned success or copied bytes.
  struct {float x,y,z; DWORD color;} quad[6]={
    {-1,-1,.5f,0xff00ff00},{-1,1,.5f,0xff00ff00},{1,1,.5f,0xff00ff00},
    {-1,-1,.5f,0xff00ff00},{1,1,.5f,0xff00ff00},{1,-1,.5f,0xff00ff00}};
  IDirect3DVertexBuffer9 *quad_src=buffer(sizeof(quad),D3DFVF_XYZ|D3DFVF_DIFFUSE,quad);
  IDirect3DVertexBuffer9 *quad_dst=buffer(6*20,D3DFVF_XYZRHW|D3DFVF_DIFFUSE,NULL);
  IDirect3DSurface9 *target,*readback;
  CHECK(IDirect3DDevice9_CreateRenderTarget(device,16,16,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL));
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device,16,16,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL));
  CHECK(IDirect3DDevice9_SetRenderTarget(device,0,target));
  vp=(D3DVIEWPORT9){0,0,16,16,0,1}; CHECK(IDirect3DDevice9_SetViewport(device,&vp));
  CHECK(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZ|D3DFVF_DIFFUSE));
  CHECK(IDirect3DDevice9_SetStreamSource(device,0,quad_src,0,16));
  CHECK(IDirect3DDevice9_ProcessVertices(device,0,0,6,quad_dst,NULL,0));
  CHECK(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
  CHECK(IDirect3DDevice9_SetStreamSource(device,0,quad_dst,0,20));
  CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));
  CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
  CHECK(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
  CHECK(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff000000,1,0));
  CHECK(IDirect3DDevice9_BeginScene(device));
  CHECK(IDirect3DDevice9_DrawPrimitive(device,D3DPT_TRIANGLELIST,0,2));
  CHECK(IDirect3DDevice9_EndScene(device));
  CHECK(IDirect3DDevice9_GetRenderTargetData(device,target,readback));
  D3DLOCKED_RECT locked; CHECK(IDirect3DSurface9_LockRect(readback,&locked,NULL,D3DLOCK_READONLY));
  DWORD pixel=*(DWORD *)((BYTE *)locked.pBits+8*locked.Pitch+8*4);
  CHECK(IDirect3DSurface9_UnlockRect(readback)); REQUIRE((pixel&0xffffff)==0x00ff00);
  printf("PASS processed vertices render green pixel %08lx\n",(unsigned long)pixel);
  IDirect3DVertexBuffer9_Release(source); IDirect3DVertexBuffer9_Release(destination);
  IDirect3DVertexBuffer9_Release(alias); IDirect3DVertexBuffer9_Release(quad_src); IDirect3DVertexBuffer9_Release(quad_dst);
  IDirect3DSurface9_Release(target); IDirect3DSurface9_Release(readback);
  IDirect3DDevice9_Release(device); IDirect3D9_Release(d3d); DestroyWindow(window); return 0;
}
