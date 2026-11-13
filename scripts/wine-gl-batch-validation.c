#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
static int failures;
static volatile LONG debug_calls;
static void WINAPI debug_message(GLenum source,GLenum type,GLuint id,GLenum severity,GLsizei length,const char *message,const void *user) {InterlockedIncrement(&debug_calls);}
static void pixel(const char *name, unsigned char red, unsigned char green) {
 unsigned char p[4]={0}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
 if(p[0]!=red || p[1]!=green || p[2]!=0){printf("FAIL %s pixel %u %u %u\n",name,p[0],p[1],p[2]);failures++;}
 else printf("PASS %s\n",name);
}
static void quad(void) {
 GLfloat v[3]={-1,-1,0}; glBegin(GL_QUADS);glTexCoord2f(0,0);glVertex3fv(v);
 v[0]=1;glTexCoord2f(1,0);glVertex3fv(v);v[1]=1;glTexCoord2f(1,1);glVertex3fv(v);
 v[0]=-1;glTexCoord2f(0,1);glVertex3fv(v);v[0]=0;v[1]=0;glEnd();
}
static HDC worker_dc;
static HGLRC worker_context;
static DWORD WINAPI render_worker(void *unused) {
 if (!wglMakeCurrent(worker_dc,worker_context)) return 1;
 glColor4f(1,0,0,1);quad();
 if (!wglMakeCurrent(0,0)) return 2;
 return 0;
}
int main(void) {
 WNDCLASSA wc={0};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="GooseGLBatchValidation";RegisterClassA(&wc);
 HWND w=CreateWindowA(wc.lpszClassName,"OpenGL validation",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,128,128,0,0,wc.hInstance,0);
 ShowWindow(w,SW_SHOW);UpdateWindow(w);MSG msg;while(PeekMessage(&msg,0,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}
 HDC dc=GetDC(w);PIXELFORMATDESCRIPTOR d={sizeof(d),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32,0,0,0,0,0,0,8,0,0,0,0,0,24,8,0,PFD_MAIN_PLANE,0,0,0,0};
 int pf=ChoosePixelFormat(dc,&d);if(!pf||!SetPixelFormat(dc,pf,&d)){printf("FAIL pixel format\n");return 1;}
 HGLRC a=wglCreateContext(dc),b=wglCreateContext(dc);if(!a||!b||!wglMakeCurrent(dc,a)){printf("FAIL contexts\n");return 1;}
 printf("RENDERER %s\nVERSION %s\nMESA_CLIENT_POINTER %d\n",glGetString(GL_RENDERER),glGetString(GL_VERSION),strstr((const char*)glGetString(GL_EXTENSIONS),"GL_MESA_map_buffer_client_pointer")!=NULL);
 glViewport(0,0,64,64);glDisable(GL_DITHER);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
 glColor4ub(0,255,0,255);quad();pixel("pointer values are captured before mutation",0,255);
 glColor4f(1,0,0,1);wglMakeCurrent(dc,b);glViewport(0,0,64,64);glDisable(GL_DITHER);glColor4f(0,1,0,1);quad();pixel("second context",0,255);
 wglMakeCurrent(dc,a);quad();pixel("pending state survives context switch",255,0);
 GLuint list=glGenLists(1);glNewList(list,GL_COMPILE);glColor3f(0,1,0);quad();glEndList();glColor3f(1,0,0);glCallList(list);pixel("display list compile and playback",0,255);glDeleteLists(list,1);
 glColor3f(1,0,0);glBegin(GL_TRIANGLES);for(int i=0;i<500;i++){glVertex3f(-1,-1,0);glVertex3f(1,-1,0);glVertex3f(0,1,0);}glEnd();pixel("bounded batches preserve an open primitive",255,0);
 while(glGetError()!=GL_NO_ERROR){}glBegin(0xdead);GLenum err=glGetError();if(err!=GL_INVALID_ENUM){printf("FAIL invalid enum error %x\n",err);failures++;}else printf("PASS error retrieval flushes pending commands\n");

 GLfloat colour[4]={0};glColor4f(0,1,0,1);glGetFloatv(GL_CURRENT_COLOR,colour);
 if(colour[0]!=0||colour[1]!=1||colour[3]!=1){printf("FAIL current colour query\n");failures++;}else printf("PASS current colour query drains pending state\n");
 GLuint tex=0;const unsigned char red[4]={255,0,0,255};glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,red);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
 glEnable(GL_TEXTURE_2D);quad();pixel("texture state interleaves with captured coordinates",255,0);glDisable(GL_TEXTURE_2D);glDeleteTextures(1,&tex);
 typedef void (WINAPI *ActiveTexture)(GLenum);typedef void (WINAPI *MultiTex2f)(GLenum,GLfloat,GLfloat);
 ActiveTexture active=(ActiveTexture)wglGetProcAddress("glActiveTextureARB");MultiTex2f multi=(MultiTex2f)wglGetProcAddress("glMultiTexCoord2fARB");
 if(active&&multi){GLfloat coord[4]={0};active(0x84c1);multi(0x84c1,0.25f,0.75f);glGetFloatv(GL_CURRENT_TEXTURE_COORDS,coord);
  if(coord[0]!=0.25f||coord[1]!=0.75f){printf("FAIL multitexture state\n");failures++;}else printf("PASS multitexture coordinate query\n");active(0x84c0);
 }else printf("SKIP multitexture extension unavailable\n");
 const GLfloat vertices[6][2]={{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
 glVertexPointer(2,GL_FLOAT,0,vertices);glEnableClientState(GL_VERTEX_ARRAY);glColor4f(0,1,0,1);glDrawArrays(GL_TRIANGLES,0,6);
 pixel("array drawing observes pending immediate state",0,255);glDisableClientState(GL_VERTEX_ARRAY);
 wglMakeCurrent(0,0);worker_dc=dc;worker_context=a;HANDLE thread=CreateThread(0,0,render_worker,0,0,0);DWORD worker_result=99;
 if(thread){WaitForSingleObject(thread,10000);GetExitCodeThread(thread,&worker_result);CloseHandle(thread);}
 if(worker_result||!wglMakeCurrent(dc,a)){printf("FAIL context thread transfer %lu\n",worker_result);failures++;}
 else {GLfloat state[4]={0};glGetFloatv(GL_CURRENT_COLOR,state);
  if(state[0]!=1||state[1]!=0||state[2]!=0||state[3]!=1){printf("FAIL thread transfer current colour %f %f %f %f\n",state[0],state[1],state[2],state[3]);failures++;}
  else printf("PASS context transfer preserves the previous thread state\n");}
 wglMakeCurrent(0,0);glVertex3fv(NULL);wglMakeCurrent(dc,a);printf("PASS null vector without a current context retains fallback semantics\n");
 typedef void (WINAPI *DebugCallback)(void *,const void *);
 DebugCallback debug=(DebugCallback)wglGetProcAddress("glDebugMessageCallbackARB");
 if(debug&&strstr((const char*)glGetString(GL_EXTENSIONS),"GL_ARB_debug_output")){
  glEnable(0x8242);debug((void*)debug_message,0);LONG before=debug_calls;glBegin(0xdead);
  if(debug_calls==before){printf("FAIL synchronous debug callback timing\n");failures++;}else printf("PASS synchronous callback occurs at the API call\n");
  glGetError();debug(0,0);
 }else printf("SKIP driver does not advertise GL_ARB_debug_output\n");
 printf("RESULT failures=%d\n",failures);wglMakeCurrent(0,0);wglDeleteContext(a);wglDeleteContext(b);ReleaseDC(w,dc);DestroyWindow(w);return failures?1:0;
}
