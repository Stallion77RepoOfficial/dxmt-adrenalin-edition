#include "d3d9_stateblock.hpp"
#include "d3d9_device.hpp"
#include "d3d9_buffer.hpp"

namespace dxmt {

HRESULT STDMETHODCALLTYPE
D3D9StateBlock::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
  return D3D_OK;
}

void
D3D9StateBlock::MarkAll() {
  MarkVertexState();
  MarkPixelState();
  for (UINT i = 0; i < 20; i++) tex_mask_.set(i);
  for (UINT i = 0; i < 16; i++) stream_mask_.set(i);
  indices_set_ = true;
  viewport_set_ = true;
  scissor_set_ = true;
  material_set_ = true;
  for (auto &t : device_->transforms_)
    transforms_captured_.insert(t.first);
  for (auto &l : device_->lights_) {
    lights_captured_.insert(l.first);
    light_enable_captured_.insert(l.first);
  }
  for (UINT i = 0; i < 8; i++) clip_plane_mask_.set(i);
}

void
D3D9StateBlock::MarkVertexState() {
  vs_set_ = true;
  decl_set_ = true;
  fvf_set_ = true;
  vsf_mask_.set();
  vsi_mask_.set();
  vsb_mask_.set();
  for (UINT i = 0; i < 16; i++) freq_mask_.set(i);
  // Vertex-processing related render states (the D3DSBT_VERTEXSTATE list).
  static const D3DRENDERSTATETYPE vertex_rs[] = {
    D3DRS_CULLMODE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND,
    D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE,
    D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_LOCALVIEWER, D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE, D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK, D3DRS_PATCHEDGESTYLE,
    D3DRS_POINTSIZE_MAX, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR, D3DRS_NORMALIZENORMALS,
    D3DRS_SPECULARENABLE, D3DRS_SHADEMODE,
  };
  for (auto s : vertex_rs) rs_mask_.set(s);
  for (auto &l : device_->lights_) {
    lights_captured_.insert(l.first);
    light_enable_captured_.insert(l.first);
  }
  for (auto &t : device_->transforms_)
    transforms_captured_.insert(t.first);
}

void
D3D9StateBlock::MarkPixelState() {
  ps_set_ = true;
  psf_mask_.set();
  psi_mask_.set();
  psb_mask_.set();
  static const D3DRENDERSTATETYPE pixel_rs[] = {
    D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE,
    D3DRS_LASTPIXEL, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
    D3DRS_DITHERENABLE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_ALPHABLENDENABLE,
    D3DRS_DEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS,
    D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR,
    D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6,
    D3DRS_WRAP7, D3DRS_WRAP8, D3DRS_WRAP9, D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12, D3DRS_WRAP13,
    D3DRS_WRAP14, D3DRS_WRAP15, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE,
    D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE,
    D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC,
    D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR,
    D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA,
    D3DRS_BLENDOPALPHA,
  };
  for (auto s : pixel_rs) rs_mask_.set(s);
  for (UINT stage = 0; stage < 20; stage++)
    for (UINT t = 1; t < 14; t++)
      ss_mask_.set(stage * 14 + t);
  for (UINT stage = 0; stage < 8; stage++)
    for (UINT t = 1; t < 33; t++)
      tss_mask_.set(stage * 33 + t);
}

HRESULT STDMETHODCALLTYPE
D3D9StateBlock::Capture() {
  auto device_lock = this->device_->LockDevice();
  D3D9Device *dev = device_;

  for (UINT i = 0; i < 256; i++)
    if (rs_mask_.test(i)) rs_[i] = dev->render_state_[i];
  for (UINT s = 0; s < 20; s++)
    for (UINT t = 1; t < 14; t++)
      if (ss_mask_.test(s * 14 + t)) ss_[s][t] = dev->sampler_state_[s][t];
  for (UINT s = 0; s < 8; s++)
    for (UINT t = 1; t < 33; t++)
      if (tss_mask_.test(s * 33 + t)) tss_[s][t] = dev->tss_[s][t];
  for (UINT i = 0; i < 20; i++)
    if (tex_mask_.test(i)) tex_[i] = dev->textures_[i].texture;

  if (vs_set_) vs_ = dev->vertex_shader_.ptr();
  if (ps_set_) ps_ = dev->pixel_shader_.ptr();
  for (UINT i = 0; i < 256; i++) {
    if (vsf_mask_.test(i)) memcpy(&vsf_[i * 4], &dev->vs_const_f_[i * 4], 4 * sizeof(float));
    if (psf_mask_.test(i)) memcpy(&psf_[i * 4], &dev->ps_const_f_[i * 4], 4 * sizeof(float));
  }
  for (UINT i = 0; i < 16; i++) {
    if (vsi_mask_.test(i)) memcpy(&vsi_[i * 4], &dev->vs_const_i_[i * 4], 4 * sizeof(int));
    if (psi_mask_.test(i)) memcpy(&psi_[i * 4], &dev->ps_const_i_[i * 4], 4 * sizeof(int));
    if (vsb_mask_.test(i)) vsb_[i] = dev->vs_const_b_[i] ? TRUE : FALSE;
    if (psb_mask_.test(i)) psb_[i] = dev->ps_const_b_[i] ? TRUE : FALSE;
  }

  if (decl_set_) decl_ = dev->vertex_decl_.ptr();
  if (fvf_set_) fvf_ = dev->fvf_;
  for (UINT i = 0; i < 16; i++) {
    if (stream_mask_.test(i)) {
      streams_[i].buffer = dev->streams_[i].buffer.ptr();
      streams_[i].offset = dev->streams_[i].offset;
      streams_[i].stride = dev->streams_[i].stride;
    }
    if (freq_mask_.test(i)) freq_[i] = dev->stream_freq_[i];
  }
  if (indices_set_) indices_ = dev->index_buffer_.ptr();
  if (viewport_set_) dev->GetViewport(&viewport_);
  if (scissor_set_) scissor_ = dev->scissor_rect_;

  for (uint32_t t : transforms_captured_) {
    auto it = dev->transforms_.find(t);
    if (it != dev->transforms_.end()) {
      transforms_[t] = it->second;
    } else {
      D3DMATRIX identity = {};
      identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
      transforms_[t] = identity;
    }
  }
  if (material_set_) material_ = dev->material_;
  for (DWORD l : lights_captured_) {
    auto it = dev->lights_.find(l);
    if (it != dev->lights_.end() && it->second.present)
      lights_[l] = it->second.light;
  }
  for (DWORD l : light_enable_captured_) {
    auto it = dev->lights_.find(l);
    light_enables_[l] = it != dev->lights_.end() && it->second.enabled;
  }
  for (UINT i = 0; i < 8; i++)
    if (clip_plane_mask_.test(i)) memcpy(clip_planes_[i], dev->clip_planes_[i], sizeof(float) * 4);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9StateBlock::Apply() {
  auto device_lock = this->device_->LockDevice();
  D3D9Device *dev = device_;

  for (UINT i = 0; i < 256; i++)
    if (rs_mask_.test(i)) dev->SetRenderState((D3DRENDERSTATETYPE)i, rs_[i]);
  for (UINT s = 0; s < 20; s++) {
    DWORD stage = s < 16 ? s : D3DVERTEXTEXTURESAMPLER0 + (s - 16);
    for (UINT t = 1; t < 14; t++)
      if (ss_mask_.test(s * 14 + t)) dev->SetSamplerState(stage, (D3DSAMPLERSTATETYPE)t, ss_[s][t]);
  }
  for (UINT s = 0; s < 8; s++)
    for (UINT t = 1; t < 33; t++)
      if (tss_mask_.test(s * 33 + t)) dev->SetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)t, tss_[s][t]);
  for (UINT i = 0; i < 20; i++) {
    DWORD stage = i < 16 ? i : D3DVERTEXTEXTURESAMPLER0 + (i - 16);
    if (tex_mask_.test(i)) dev->SetTexture(stage, tex_[i].ptr());
  }

  if (vs_set_) dev->SetVertexShader(reinterpret_cast<IDirect3DVertexShader9 *>(vs_.ptr()));
  if (ps_set_) dev->SetPixelShader(reinterpret_cast<IDirect3DPixelShader9 *>(ps_.ptr()));
  for (UINT i = 0; i < 256; i++) {
    if (vsf_mask_.test(i)) dev->SetVertexShaderConstantF(i, &vsf_[i * 4], 1);
    if (psf_mask_.test(i)) dev->SetPixelShaderConstantF(i, &psf_[i * 4], 1);
  }
  for (UINT i = 0; i < 16; i++) {
    if (vsi_mask_.test(i)) dev->SetVertexShaderConstantI(i, &vsi_[i * 4], 1);
    if (psi_mask_.test(i)) dev->SetPixelShaderConstantI(i, &psi_[i * 4], 1);
    if (vsb_mask_.test(i)) dev->SetVertexShaderConstantB(i, &vsb_[i], 1);
    if (psb_mask_.test(i)) dev->SetPixelShaderConstantB(i, &psb_[i], 1);
  }

  // FVF and declaration are mutually exclusive on the device; apply whichever
  // was captured last as non-null, mirroring D3D9 runtime behaviour.
  if (decl_set_ && decl_) dev->SetVertexDeclaration(reinterpret_cast<IDirect3DVertexDeclaration9 *>(decl_.ptr()));
  else if (fvf_set_ && fvf_) dev->SetFVF(fvf_);
  for (UINT i = 0; i < 16; i++) {
    if (stream_mask_.test(i)) dev->SetStreamSource(i, streams_[i].buffer.ptr(), streams_[i].offset, streams_[i].stride);
    if (freq_mask_.test(i)) dev->SetStreamSourceFreq(i, freq_[i] ? freq_[i] : 1);
  }
  if (indices_set_) dev->SetIndices(indices_.ptr());
  if (viewport_set_) dev->SetViewport(&viewport_);
  if (scissor_set_) dev->SetScissorRect(&scissor_);

  for (auto &t : transforms_) dev->SetTransform((D3DTRANSFORMSTATETYPE)t.first, &t.second);
  if (material_set_) dev->SetMaterial(&material_);
  for (auto &l : lights_) dev->SetLight(l.first, &l.second);
  for (auto &e : light_enables_) dev->LightEnable(e.first, e.second);
  for (UINT i = 0; i < 8; i++)
    if (clip_plane_mask_.test(i)) dev->SetClipPlane(i, clip_planes_[i]);

  return D3D_OK;
}

} // namespace dxmt
