#pragma once

#include "d3d9_private.h"
#include "d3d9_shader.hpp"
#include "d3d9_vertex_declaration.hpp"
#include "d3d9_device_child.hpp"
#include <bitset>
#include <map>
#include <set>

namespace dxmt {

class D3D9Device;

// IDirect3DStateBlock9. A block is a set of "captured" slots plus their
// values: Capture() re-reads the current device value of every captured slot,
// Apply() plays the stored values back through the device's own setters.
//
// Blocks come from two places with the same representation:
//  - CreateStateBlock(D3DSBT_*): the predefined slot sets;
//  - BeginStateBlock/EndStateBlock: setters call the Mark* hooks while
//    recording, so exactly the touched slots become captured.
class D3D9StateBlock final : public D3D9DeviceChild<IDirect3DStateBlock9> {
public:
  explicit D3D9StateBlock(D3D9Device *pDevice) : D3D9DeviceChild(pDevice) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DStateBlock9)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE Capture() final;
  HRESULT STDMETHODCALLTYPE Apply() final;

  // --- capture-set construction ------------------------------------------------
  void MarkAll();         // D3DSBT_ALL
  void MarkVertexState(); // D3DSBT_VERTEXSTATE
  void MarkPixelState();  // D3DSBT_PIXELSTATE

  // Recording hooks, called by the device's setters between BeginStateBlock
  // and EndStateBlock. Per D3D9 semantics recorded Set* calls do NOT touch
  // the live device state, so the value is stored here directly.
  void RecordRenderState(UINT s, DWORD v) { rs_mask_.set(s); rs_[s] = v; }
  void RecordSamplerState(UINT slot, UINT type, DWORD v) { ss_mask_.set(slot * 14 + type); ss_[slot][type] = v; }
  void RecordTextureStageState(UINT stage, UINT type, DWORD v) { tss_mask_.set(stage * 33 + type); tss_[stage][type] = v; }
  void RecordTexture(UINT slot, IDirect3DBaseTexture9 *t) { tex_mask_.set(slot); tex_[slot] = t; }
  void RecordVertexShader(D3D9VertexShader *s) { vs_set_ = true; vs_ = s; }
  void RecordPixelShader(D3D9PixelShader *s) { ps_set_ = true; ps_ = s; }
  void RecordVSConstantF(UINT start, const float *v, UINT count) {
    for (UINT i = 0; i < count; i++) { vsf_mask_.set(start + i); memcpy(&vsf_[(start + i) * 4], v + i * 4, 4 * sizeof(float)); }
  }
  void RecordPSConstantF(UINT start, const float *v, UINT count) {
    for (UINT i = 0; i < count; i++) { psf_mask_.set(start + i); memcpy(&psf_[(start + i) * 4], v + i * 4, 4 * sizeof(float)); }
  }
  void RecordVSConstantI(UINT start, const int *v, UINT count) {
    for (UINT i = 0; i < count; i++) { vsi_mask_.set(start + i); memcpy(&vsi_[(start + i) * 4], v + i * 4, 4 * sizeof(int)); }
  }
  void RecordPSConstantI(UINT start, const int *v, UINT count) {
    for (UINT i = 0; i < count; i++) { psi_mask_.set(start + i); memcpy(&psi_[(start + i) * 4], v + i * 4, 4 * sizeof(int)); }
  }
  void RecordVSConstantB(UINT start, const BOOL *v, UINT count) {
    for (UINT i = 0; i < count; i++) { vsb_mask_.set(start + i); vsb_[start + i] = v[i]; }
  }
  void RecordPSConstantB(UINT start, const BOOL *v, UINT count) {
    for (UINT i = 0; i < count; i++) { psb_mask_.set(start + i); psb_[start + i] = v[i]; }
  }
  void RecordVertexDeclaration(D3D9VertexDeclaration *d) { decl_set_ = true; decl_ = d; }
  void RecordFVF(DWORD fvf) { fvf_set_ = true; fvf_ = fvf; decl_set_ = true; decl_ = nullptr; }
  void RecordStreamSource(UINT s, IDirect3DVertexBuffer9 *buf, UINT offset, UINT stride) {
    stream_mask_.set(s); streams_[s].buffer = buf; streams_[s].offset = offset; streams_[s].stride = stride;
  }
  void RecordStreamFreq(UINT s, UINT freq) { freq_mask_.set(s); freq_[s] = freq; }
  void RecordIndices(IDirect3DIndexBuffer9 *ib) { indices_set_ = true; indices_ = ib; }
  void RecordViewport(const D3DVIEWPORT9 &vp) { viewport_set_ = true; viewport_ = vp; }
  void RecordScissorRect(const RECT &r) { scissor_set_ = true; scissor_ = r; }
  void RecordTransform(UINT state, const D3DMATRIX &m) { transforms_captured_.insert(state); transforms_[state] = m; }
  void RecordMaterial(const D3DMATERIAL9 &m) { material_set_ = true; material_ = m; }
  void RecordLight(DWORD index, const D3DLIGHT9 &l) { lights_captured_.insert(index); lights_[index] = l; }
  void RecordLightEnable(DWORD index, BOOL e) { light_enable_captured_.insert(index); light_enables_[index] = e; }
  void RecordClipPlane(UINT index, const float *p) { clip_plane_mask_.set(index); memcpy(clip_planes_[index], p, 4 * sizeof(float)); }

private:
  // Capture masks.
  std::bitset<256> rs_mask_;
  std::bitset<20 * 14> ss_mask_;
  std::bitset<8 * 33> tss_mask_;
  std::bitset<20> tex_mask_;
  std::bitset<256> vsf_mask_, psf_mask_;
  std::bitset<16> vsi_mask_, psi_mask_, vsb_mask_, psb_mask_;
  std::bitset<16> stream_mask_, freq_mask_;
  std::bitset<8> clip_plane_mask_;
  bool vs_set_ = false, ps_set_ = false;
  bool decl_set_ = false, fvf_set_ = false;
  bool indices_set_ = false, viewport_set_ = false, scissor_set_ = false;
  bool material_set_ = false;
  std::set<uint32_t> transforms_captured_;
  std::set<DWORD> lights_captured_, light_enable_captured_;

  // Captured values.
  DWORD rs_[256] = {};
  DWORD ss_[20][14] = {};
  DWORD tss_[8][33] = {};
  Com<IDirect3DBaseTexture9> tex_[20];
  float vsf_[256 * 4] = {};
  float psf_[256 * 4] = {};
  int vsi_[16 * 4] = {};
  int psi_[16 * 4] = {};
  BOOL vsb_[16] = {};
  BOOL psb_[16] = {};
  Com<D3D9VertexShader> vs_;
  Com<D3D9PixelShader> ps_;
  Com<D3D9VertexDeclaration> decl_;
  DWORD fvf_ = 0;
  struct { Com<IDirect3DVertexBuffer9> buffer; UINT offset = 0, stride = 0; } streams_[16];
  UINT freq_[16] = {};
  Com<IDirect3DIndexBuffer9> indices_;
  D3DVIEWPORT9 viewport_ = {};
  RECT scissor_ = {};
  std::map<uint32_t, D3DMATRIX> transforms_;
  D3DMATERIAL9 material_ = {};
  std::map<DWORD, D3DLIGHT9> lights_;
  std::map<DWORD, BOOL> light_enables_;
  float clip_planes_[8][4] = {};
};

} // namespace dxmt
