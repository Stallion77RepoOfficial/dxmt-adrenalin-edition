#include "d3d9_ffp.hpp"
#include "d3d9_caps.hpp"
#include "d3d9_device.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_vertex_declaration.hpp"
#include "sm4_builder.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cmath>
#include <map>

using namespace dxmt::sm4;

namespace dxmt {

namespace {

// Semantic naming for FFP vertex inputs; only has to be internally consistent
// between the generated shader's ISGN and the input layout built from the
// vertex declaration (same convention as the translated-shader path).
std::string FFPUsageSemantic(BYTE usage) {
  switch (usage) {
  case D3DDECLUSAGE_POSITION: case D3DDECLUSAGE_POSITIONT: return "POSITION";
  case D3DDECLUSAGE_NORMAL: return "NORMAL";
  case D3DDECLUSAGE_COLOR: return "COLOR";
  case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
  case D3DDECLUSAGE_BLENDWEIGHT: return "BLENDWEIGHT";
  case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
  case D3DDECLUSAGE_PSIZE: return "PSIZE";
  case D3DDECLUSAGE_TANGENT: return "TANGENT";
  case D3DDECLUSAGE_BINORMAL: return "BINORMAL";
  case D3DDECLUSAGE_FOG: return "FOG";
  case D3DDECLUSAGE_DEPTH: return "DEPTH";
  default: return "TEXCOORD";
  }
}

Operand CB(uint32_t elem, uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3) {
  return ConstantBuffer(0, elem, x, y, z, w);
}

// out = v * M with M stored transposed (columns as consecutive cb elements).
void EmitMatMul4(Sm4Builder &b, Operand dst_base, uint32_t temp, Operand v, uint32_t cb_base) {
  static const uint8_t kMask[4] = {0b0001, 0b0010, 0b0100, 0b1000};
  (void)dst_base;
  for (int c = 0; c < 4; c++)
    b.Dp4(WithMask(Temp(temp), kMask[c]), v, CB(cb_base + c));
}

void EmitMatMul3(Sm4Builder &b, uint32_t temp, Operand v, uint32_t cb_base) {
  static const uint8_t kMask[4] = {0b0001, 0b0010, 0b0100, 0b1000};
  for (int c = 0; c < 3; c++)
    b.Dp3(WithMask(Temp(temp), kMask[c]), v, CB(cb_base + c));
}

} // namespace

namespace {

// Row-major, row-vector convention (v' = v * M), matching D3DMATRIX.
D3DMATRIX MatMul(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX out = {};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      for (int k = 0; k < 4; k++)
        out.m[r][c] += a.m[r][k] * b.m[k][c];
  return out;
}

// General 4x4 inverse, by cofactors. False for a singular matrix.
bool Invert(const D3DMATRIX &m, D3DMATRIX *out) {
  const float *a = &m.m[0][0];
  float inv[16];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
           a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
           a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
           a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
            a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
           a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
           a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
           a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
            a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
           a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
           a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
            a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
            a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
           a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
           a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
            a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
            a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (det == 0.0f)
    return false;
  float inv_det = 1.0f / det;
  float *o = &out->m[0][0];
  for (int i = 0; i < 16; i++)
    o[i] = inv[i] * inv_det;
  return true;
}

// Writes M transposed (columns as consecutive vec4s) so dp4(v, column) in the
// shader computes v * M.
void WriteTransposed(float *dst, const D3DMATRIX &m) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      dst[c * 4 + r] = m.m[r][c];
}

// Rows of the inverse of M's upper 3x3, padded to vec4s: exactly what the
// shader needs to compute n * (M^-1)^T with dp3-per-column.
void WriteNormalMatrix(float *dst, const D3DMATRIX &m) {
  float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
  float d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
  float g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];
  float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  float det = a * A + b * B + c * C;
  float inv_det = det != 0.0f ? 1.0f / det : 1.0f;
  float inv[3][3] = {
    {A * inv_det, (c * h - b * i) * inv_det, (b * f - c * e) * inv_det},
    {B * inv_det, (a * i - c * g) * inv_det, (c * d - a * f) * inv_det},
    {C * inv_det, (b * g - a * h) * inv_det, (a * e - b * d) * inv_det},
  };
  memset(dst, 0, 4 * 4 * sizeof(float));
  for (int r = 0; r < 3; r++)
    for (int k = 0; k < 3; k++)
      dst[r * 4 + k] = inv[r][k];
}

void WriteColor(float *dst, DWORD argb) {
  dst[0] = ((argb >> 16) & 0xFF) / 255.0f;
  dst[1] = ((argb >> 8) & 0xFF) / 255.0f;
  dst[2] = ((argb >> 0) & 0xFF) / 255.0f;
  dst[3] = ((argb >> 24) & 0xFF) / 255.0f;
}

float AsFloat(DWORD v) {
  float f;
  memcpy(&f, &v, sizeof(f));
  return f;
}

} // namespace

void
FFPShaderCache::FillConstants(const FFPShaderKey &key, float *vs, float *ps) {
  using namespace ffp;
  D3D9Device *dev = device_;
  const DWORD *rs = dev->render_state_;

  memset(vs, 0, kVSSizeVec4 * 16);
  memset(ps, 0, kPSSizeVec4 * 16);

  D3DMATRIX world, view, proj;
  dev->GetTransform(D3DTS_WORLD, &world);
  dev->GetTransform(D3DTS_VIEW, &view);
  dev->GetTransform(D3DTS_PROJECTION, &proj);
  D3DMATRIX wv = MatMul(world, view);
  D3DMATRIX wvp = MatMul(wv, proj);
  WriteTransposed(vs + kVSWorldViewProj * 4, wvp);
  WriteTransposed(vs + kVSWorldView * 4, wv);
  WriteNormalMatrix(vs + kVSNormalMatrix * 4, wv);

  memcpy(vs + kVSMaterialDiffuse * 4, &dev->material_.Diffuse, 4 * sizeof(float));
  memcpy(vs + kVSMaterialAmbient * 4, &dev->material_.Ambient, 4 * sizeof(float));
  memcpy(vs + kVSMaterialSpecular * 4, &dev->material_.Specular, 3 * sizeof(float));
  vs[kVSMaterialSpecular * 4 + 3] = dev->material_.Power;
  memcpy(vs + kVSMaterialEmissive * 4, &dev->material_.Emissive, 4 * sizeof(float));
  WriteColor(vs + kVSGlobalAmbient * 4, rs[D3DRS_AMBIENT]);

  // (start, end, density, 1 / (end - start)). Vertex fog with start == end
  // fogs everything: end and scale both zero make the linear factor 0.
  float fog_start = AsFloat(rs[D3DRS_FOGSTART]), fog_end = AsFloat(rs[D3DRS_FOGEND]);
  vs[kVSFogParams * 4 + 0] = fog_start;
  vs[kVSFogParams * 4 + 1] = fog_start == fog_end ? 0.0f : fog_end;
  vs[kVSFogParams * 4 + 2] = AsFloat(rs[D3DRS_FOGDENSITY]);
  vs[kVSFogParams * 4 + 3] = fog_start == fog_end ? 0.0f : 1.0f / (fog_end - fog_start);

  D3DVIEWPORT9 vp = {};
  dev->GetViewport(&vp);
  float w = vp.Width ? (float)vp.Width : 1.0f;
  float h = vp.Height ? (float)vp.Height : 1.0f;
  vs[kVSViewportScale * 4 + 0] = 2.0f / w;
  vs[kVSViewportScale * 4 + 1] = -2.0f / h;
  vs[kVSViewportOffset * 4 + 0] = -1.0f - 2.0f * vp.X / w;
  vs[kVSViewportOffset * 4 + 1] = 1.0f + 2.0f * vp.Y / h;

  vs[kVSPointSize * 4 + 0] = AsFloat(rs[D3DRS_POINTSIZE]);
  vs[kVSPointSize * 4 + 1] = AsFloat(rs[D3DRS_POINTSIZE_MIN]);
  vs[kVSPointSize * 4 + 2] = std::min(AsFloat(rs[D3DRS_POINTSIZE_MAX]), kMaxPointSize);
  vs[kVSPointSize * 4 + 3] = (float)vp.Height;
  vs[kVSPointScale * 4 + 0] = AsFloat(rs[D3DRS_POINTSCALE_A]);
  vs[kVSPointScale * 4 + 1] = AsFloat(rs[D3DRS_POINTSCALE_B]);
  vs[kVSPointScale * 4 + 2] = AsFloat(rs[D3DRS_POINTSCALE_C]);
  vs[kVSPointScale * 4 + 3] = AsFloat(rs[D3DRS_DEPTHBIAS]);

  for (UINT t = 0; t < 8; t++) {
    D3DMATRIX tm;
    dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + t), &tm);
    WriteTransposed(vs + (kVSTexTransform + t * 4) * 4, tm);
  }

  /* World-space planes into view space: with p_view = p_world * V, a plane
   * P in world space is P * (V^-1)^T in view space. */
  D3DMATRIX view_inverse;
  if (key.clip_planes && Invert(view, &view_inverse)) {
    for (UINT i = 0; i < 8; i++) {
      if (!(rs[D3DRS_CLIPPLANEENABLE] & (1u << i)))
        continue;
      const float *plane = dev->clip_planes_[i];
      for (int j = 0; j < 4; j++)
        vs[(kVSClipPlanes + i) * 4 + j] = view_inverse.m[j][0] * plane[0] + view_inverse.m[j][1] * plane[1] +
                                           view_inverse.m[j][2] * plane[2] + view_inverse.m[j][3] * plane[3];
    }
  }

  UINT li = 0;
  for (auto &l : dev->lights_) {
    if (!l.second.enabled || li >= 8) continue;
    const D3DLIGHT9 &light = l.second.light;
    float *base = vs + (kVSLights + li * 6) * 4;
    memcpy(base + 0 * 4, &light.Diffuse, 4 * sizeof(float));
    memcpy(base + 1 * 4, &light.Specular, 4 * sizeof(float));
    memcpy(base + 2 * 4, &light.Ambient, 4 * sizeof(float));
    // Positions/directions move into view space so they match the shader's
    // view-space vertex position.
    float px = light.Position.x, py = light.Position.y, pz = light.Position.z;
    base[3 * 4 + 0] = px * view.m[0][0] + py * view.m[1][0] + pz * view.m[2][0] + view.m[3][0];
    base[3 * 4 + 1] = px * view.m[0][1] + py * view.m[1][1] + pz * view.m[2][1] + view.m[3][1];
    base[3 * 4 + 2] = px * view.m[0][2] + py * view.m[1][2] + pz * view.m[2][2] + view.m[3][2];
    base[3 * 4 + 3] = light.Range;
    float dx = light.Direction.x, dy = light.Direction.y, dz = light.Direction.z;
    float vdx = dx * view.m[0][0] + dy * view.m[1][0] + dz * view.m[2][0];
    float vdy = dx * view.m[0][1] + dy * view.m[1][1] + dz * view.m[2][1];
    float vdz = dx * view.m[0][2] + dy * view.m[1][2] + dz * view.m[2][2];
    float len = std::sqrt(vdx * vdx + vdy * vdy + vdz * vdz);
    if (len > 0.0f) { vdx /= len; vdy /= len; vdz /= len; }
    base[4 * 4 + 0] = vdx;
    base[4 * 4 + 1] = vdy;
    base[4 * 4 + 2] = vdz;
    base[4 * 4 + 3] = std::cos(light.Theta * 0.5f);
    base[5 * 4 + 0] = light.Attenuation0;
    base[5 * 4 + 1] = light.Attenuation1;
    base[5 * 4 + 2] = light.Attenuation2;
    base[5 * 4 + 3] = std::cos(light.Phi * 0.5f);
    li++;
  }

  WriteColor(ps + kPSTextureFactor * 4, rs[D3DRS_TEXTUREFACTOR]);
  WriteColor(ps + kPSFogColor * 4, rs[D3DRS_FOGCOLOR]);
  ps[kPSFogParams * 4 + 0] = AsFloat(rs[D3DRS_FOGSTART]);
  ps[kPSFogParams * 4 + 1] = AsFloat(rs[D3DRS_FOGEND]);
  ps[kPSFogParams * 4 + 2] = AsFloat(rs[D3DRS_FOGDENSITY]);
  ps[kPSAlphaRef * 4 + 0] = (rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
  for (UINT s = 0; s < 8; s++)
    WriteColor(ps + (kPSStageConstant + s) * 4, dev->tss_[s][D3DTSS_CONSTANT]);

  // Bump environment state. D3D9 passes these floats through
  // SetTextureStageState as DWORDs holding the bit pattern.
  auto stage_float = [&](UINT stage, D3DTEXTURESTAGESTATETYPE type) {
    float value;
    DWORD bits = dev->tss_[stage][type];
    memcpy(&value, &bits, sizeof(value));
    return value;
  };
  for (UINT s = 0; s < 8; s++) {
    float *matrix = ps + (kPSBumpBase + s * kPSBumpStride) * 4;
    matrix[0] = stage_float(s, D3DTSS_BUMPENVMAT00);
    matrix[1] = stage_float(s, D3DTSS_BUMPENVMAT01);
    matrix[2] = stage_float(s, D3DTSS_BUMPENVMAT10);
    matrix[3] = stage_float(s, D3DTSS_BUMPENVMAT11);
    float *luminance = matrix + 4;
    luminance[0] = stage_float(s, D3DTSS_BUMPENVLSCALE);
    luminance[1] = stage_float(s, D3DTSS_BUMPENVLOFFSET);
  }

  (void)key;
}

namespace {
/* Whether a stage operation reads D3DTA_TEXTURE through an argument it
 * actually evaluates. */
bool OpReadsTexture(uint8_t op, uint8_t arg1, uint8_t arg2, uint8_t arg0) {
  auto texture = [](uint8_t arg) { return (arg & D3DTA_SELECTMASK) == D3DTA_TEXTURE; };
  if (op == D3DTOP_DISABLE) return false;
  if (texture(arg1) && op != D3DTOP_SELECTARG2) return true;
  if (texture(arg2) && op != D3DTOP_SELECTARG1) return true;
  return texture(arg0) && (op == D3DTOP_MULTIPLYADD || op == D3DTOP_LERP);
}

UINT DeclTypeComponents(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: return 1;
  case D3DDECLTYPE_FLOAT2: case D3DDECLTYPE_SHORT2: case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N: case D3DDECLTYPE_FLOAT16_2: return 2;
  case D3DDECLTYPE_FLOAT3: case D3DDECLTYPE_UDEC3: case D3DDECLTYPE_DEC3N: return 3;
  default: return 4;
  }
}
} // namespace

FFPShaderKey
FFPShaderCache::BuildKey(const std::vector<D3DVERTEXELEMENT9> &decl_elements, bool vs, bool ps,
                         uint32_t &ps_inputs, bool points) {
  D3D9Device *dev = device_;
  const DWORD *rs = dev->render_state_;
  FFPShaderKey key = {};

  if (vs) {
    for (auto &e : decl_elements) {
      if (e.Type == D3DDECLTYPE_UNUSED) continue;
      uint32_t slot = ~0u;
      switch (e.Usage) {
      case D3DDECLUSAGE_POSITION:
        if (e.UsageIndex == 0) slot = kFFPInputPosition;
        break;
      case D3DDECLUSAGE_POSITIONT: key.pretransformed = 1; slot = kFFPInputPosition; break;
      case D3DDECLUSAGE_NORMAL:
        key.has_normal = 1;
        if (e.UsageIndex == 0) slot = kFFPInputNormal;
        break;
      case D3DDECLUSAGE_COLOR:
        if (e.UsageIndex == 0) key.has_diffuse = 1, slot = kFFPInputDiffuse;
        else key.has_specular = 1, slot = kFFPInputSpecular;
        break;
      case D3DDECLUSAGE_TEXCOORD:
        if (e.UsageIndex < 8) {
          key.texcoord_sets |= 1u << e.UsageIndex;
          key.texcoord_size[e.UsageIndex] = (uint8_t)DeclTypeComponents(e.Type);
          slot = kFFPInputTexcoord0 + e.UsageIndex;
        }
        break;
      case D3DDECLUSAGE_PSIZE: key.has_psize = 1; slot = kFFPInputPointSize; break;
      default: break;
      }
      if (slot != ~0u)
        key.input_conversions |= GetD3D9DeclTypeConversion(e.Type) << (slot * 2);
    }
    if (points) {
      key.point_size = 1;
      key.point_scale = rs[D3DRS_POINTSCALEENABLE] && !key.pretransformed;
    } else {
      key.has_psize = 0;
    }

    /* Pretransformed vertices are never clipped against the user planes. */
    key.clip_planes = !key.pretransformed && dev->UserClipPlanesEnabled();
    key.lighting = rs[D3DRS_LIGHTING] && key.has_normal && !key.pretransformed;
    key.color_vertex = rs[D3DRS_COLORVERTEX] != 0;
    key.diffuse_source = (uint8_t)rs[D3DRS_DIFFUSEMATERIALSOURCE];
    key.ambient_source = (uint8_t)rs[D3DRS_AMBIENTMATERIALSOURCE];
    key.specular_source = (uint8_t)rs[D3DRS_SPECULARMATERIALSOURCE];
    key.emissive_source = (uint8_t)rs[D3DRS_EMISSIVEMATERIALSOURCE];
    key.normalize_normals = rs[D3DRS_NORMALIZENORMALS] != 0;
    if (key.lighting) {
      for (auto &l : dev->lights_) {
        if (!l.second.enabled || key.light_count >= 8) continue;
        key.light_type[key.light_count++] = (uint8_t)l.second.light.Type;
      }
    }
    for (UINT s = 0; s < 8; s++) {
      key.stage_texcoord_index[s] = (uint8_t)(dev->tss_[s][D3DTSS_TEXCOORDINDEX] & 7);
      key.stage_texgen[s] = (uint8_t)((dev->tss_[s][D3DTSS_TEXCOORDINDEX] >> 16) & 0xFF);
      DWORD ttf = dev->tss_[s][D3DTSS_TEXTURETRANSFORMFLAGS];
      key.texture_transform[s] = (uint8_t)(ttf & 0xFF);
      key.stage_projected[s] = (ttf & D3DTTFF_PROJECTED) ? 1 : 0;
    }
  }
  key.specular_enable = rs[D3DRS_SPECULARENABLE] != 0;

  key.fog_enable = rs[D3DRS_FOGENABLE] != 0;
  if (key.fog_enable) {
    /* Table fog wins when it is set, and is the pixel stage's work. Otherwise
     * the factor travels on the FOG varying: the fixed-function vertex stage
     * computes it from the depth for an untransformed vertex with a vertex fog
     * mode, and takes the specular alpha in every other case; a programmable
     * vertex stage writes it itself. */
    key.fog_table_mode = (uint8_t)rs[D3DRS_FOGTABLEMODE];
    if (key.fog_table_mode == D3DFOG_NONE) {
      key.fog_mode = vs && !key.pretransformed ? (uint8_t)rs[D3DRS_FOGVERTEXMODE] : (uint8_t)D3DFOG_NONE;
      key.range_fog = key.fog_mode != D3DFOG_NONE && rs[D3DRS_RANGEFOGENABLE] != 0;
    } else {
      /* Pre-transformed vertices follow the projection matrix too: under a
       * perspective one the factor runs on their 1/rhw, otherwise on z. */
      D3DMATRIX proj;
      dev->GetTransform(D3DTS_PROJECTION, &proj);
      bool ortho = proj._14 == 0.0f && proj._24 == 0.0f && proj._34 == 0.0f && proj._44 == 1.0f;
      key.fog_w = !ortho;
    }
  }

  // The stages that sample a texture, and so need their coordinate.
  uint8_t sampled = 0;
  for (UINT s = 0; s < 8; s++) {
    if (dev->tss_[s][D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
    if (dev->textures_[s].texture) sampled |= 1u << s;
  }

  if (ps) {
    for (UINT s = 0; s < 8; s++) {
      key.stage_color_op[s] = (uint8_t)dev->tss_[s][D3DTSS_COLOROP];
      key.stage_color_arg1[s] = (uint8_t)dev->tss_[s][D3DTSS_COLORARG1];
      key.stage_color_arg2[s] = (uint8_t)dev->tss_[s][D3DTSS_COLORARG2];
      key.stage_color_arg0[s] = (uint8_t)dev->tss_[s][D3DTSS_COLORARG0];
      key.stage_alpha_op[s] = (uint8_t)dev->tss_[s][D3DTSS_ALPHAOP];
      key.stage_alpha_arg1[s] = (uint8_t)dev->tss_[s][D3DTSS_ALPHAARG1];
      key.stage_alpha_arg2[s] = (uint8_t)dev->tss_[s][D3DTSS_ALPHAARG2];
      key.stage_alpha_arg0[s] = (uint8_t)dev->tss_[s][D3DTSS_ALPHAARG0];
      key.stage_result_temp[s] = (dev->tss_[s][D3DTSS_RESULTARG] & 0xF) == (D3DTA_TEMP & 0xF);
      key.stage_projected[s] = (dev->tss_[s][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) ? 1 : 0;
      if (auto *tex = dev->textures_[s].texture) {
        switch (D3D9TextureType(tex)) {
        case D3DRTYPE_TEXTURE: key.stage_texture_type[s] = 1; break;
        case D3DRTYPE_CUBETEXTURE: key.stage_texture_type[s] = 2; break;
        case D3DRTYPE_VOLUMETEXTURE: key.stage_texture_type[s] = 3; break;
        default: break;
        }
        int channel = D3D9Fetch4Channel(tex, dev->sampler_state_[s][D3DSAMP_MIPMAPLODBIAS]);
        if (channel >= 0) {
          key.stage_fetch4 |= 1u << s;
          if (channel == 3) key.stage_fetch4_alpha |= 1u << s;
        }
      }
      /* A stage that reads a texture it does not have passes the current
       * colour on unchanged -- what D3D9 hardware does -- rather than reading
       * a sample that was never taken. And a dot product on the colour
       * replaces the alpha operation with itself, so its result lands in all
       * four channels. */
      if (!key.stage_texture_type[s] && OpReadsTexture(key.stage_color_op[s], key.stage_color_arg1[s],
                                                        key.stage_color_arg2[s], key.stage_color_arg0[s])) {
        key.stage_color_op[s] = D3DTOP_SELECTARG1;
        key.stage_color_arg1[s] = D3DTA_CURRENT;
      }
      if (key.stage_color_op[s] == D3DTOP_DOTPRODUCT3) {
        key.stage_alpha_op[s] = D3DTOP_DOTPRODUCT3;
        key.stage_alpha_arg1[s] = key.stage_color_arg1[s];
        key.stage_alpha_arg2[s] = key.stage_color_arg2[s];
        key.stage_alpha_arg0[s] = key.stage_color_arg0[s];
      }
      if (!key.stage_texture_type[s] && OpReadsTexture(key.stage_alpha_op[s], key.stage_alpha_arg1[s],
                                                        key.stage_alpha_arg2[s], key.stage_alpha_arg0[s])) {
        key.stage_alpha_op[s] = D3DTOP_SELECTARG1;
        key.stage_alpha_arg1[s] = D3DTA_CURRENT;
      }
      /* A bump stage writes nothing (see the stage loop), so what its alpha
       * operation and result register were set to does not matter. */
      if (key.stage_color_op[s] == D3DTOP_BUMPENVMAP || key.stage_color_op[s] == D3DTOP_BUMPENVMAPLUMINANCE) {
        key.stage_alpha_op[s] = D3DTOP_DISABLE;
        key.stage_alpha_arg1[s] = key.stage_alpha_arg2[s] = key.stage_alpha_arg0[s] = 0;
        key.stage_result_temp[s] = 0;
      }
    }
    if (rs[D3DRS_ALPHATESTENABLE] && rs[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS)
      key.alpha_test_func = (uint8_t)rs[D3DRS_ALPHAFUNC];
    key.point_sprite = points && rs[D3DRS_POINTSPRITEENABLE];
    ps_inputs = ffp::PixelInputs(key, sampled);
  }

  if (vs) {
    key.vs_texcoord_outputs = (uint8_t)((ps_inputs >> sm3::kLinkTexcoord0) & 0xFF);
    key.vs_default_outputs = ps_inputs & ~ffp::VertexOutputs(key);
  } else {
    /* Nothing of the vertex side reaches the pixel shader then. */
    key.fog_mode = 0;
    key.range_fog = 0;
  }
  return key;
}

namespace ffp {
uint32_t VertexOutputs(const FFPShaderKey &key) {
  uint32_t out = sm3::LinkBit(sm3::kLinkColor0) | sm3::LinkBit(sm3::kLinkColor0 + 1);
  if (key.fog_enable && key.fog_table_mode == D3DFOG_NONE)
    out |= sm3::LinkBit(sm3::kLinkFog);
  return out | (uint32_t(key.vs_texcoord_outputs) << sm3::kLinkTexcoord0);
}

uint32_t PixelInputs(const FFPShaderKey &key, uint8_t sampled_stages) {
  uint32_t in = sm3::LinkBit(sm3::kLinkColor0) | sm3::LinkBit(sm3::kLinkColor0 + 1);
  if (key.fog_enable && key.fog_table_mode == D3DFOG_NONE)
    in |= sm3::LinkBit(sm3::kLinkFog);
  if (key.point_sprite) // the coordinates come from the point, not the vertex stage
    return in;
  return in | (uint32_t(sampled_stages) << sm3::kLinkTexcoord0);
}
} // namespace ffp

FFPShaders *
FFPShaderCache::Get(const FFPShaderKey &key) {
  auto it = cache_.find(key);
  if (it != cache_.end())
    return it->second.vs ? &it->second : nullptr;
  FFPShaders shaders;
  bool ok = Generate(key, shaders);
  it = cache_.emplace(key, std::move(shaders)).first;
  if (!ok) {
    it->second.vs = nullptr;
    it->second.ps = nullptr;
    return nullptr;
  }
  return &it->second;
}

bool
FFPShaderCache::Generate(const FFPShaderKey &key, FFPShaders &out) {
  using namespace ffp;

  // ---------------------------------------------------------------------------
  // Vertex shader
  // ---------------------------------------------------------------------------
  {
    Sm4Builder b(Sm4Builder::Stage::Vertex);
    b.DeclConstantBuffer(0, kVSSizeVec4);

    // Inputs: every element of the current declaration gets an input register
    // in element order; the shader body picks out the ones it understands.
    uint32_t v_pos = ~0u, v_normal = ~0u, v_diffuse = ~0u, v_specular = ~0u, v_psize = ~0u;
    uint32_t v_tex[8];
    for (auto &t : v_tex) t = ~0u;
    {
      // Reconstruct input registers from the key: position(t), normal,
      // colors, texcoords - in a fixed order independent of the declaration
      // (the input layout matches them by semantic, not by position order).
      uint32_t reg = 0;
      auto declare = [&](const char *sem, uint32_t idx) {
        b.DeclInput(reg, 0xf, sem, idx);
        out.vs_inputs.push_back({reg, sem, idx});
        return reg++;
      };
      v_pos = declare("POSITION", 0);
      if (key.has_normal) v_normal = declare("NORMAL", 0);
      if (key.has_diffuse) v_diffuse = declare("COLOR", 0);
      if (key.has_specular) v_specular = declare("COLOR", 1);
      if (key.has_psize) v_psize = declare("PSIZE", 0);
      for (UINT t = 0; t < 8; t++)
        if (key.texcoord_sets & (1u << t))
          v_tex[t] = declare("TEXCOORD", t);
    }

    /* Integer vertex data reads as floats here as it does to a D3D9 vertex
     * shader; the inputs that need converting are read through a temp. */
    std::map<uint32_t, uint32_t> converted;
    {
      const std::pair<uint32_t, uint32_t> slots[] = {
        {v_pos, kFFPInputPosition}, {v_normal, kFFPInputNormal}, {v_diffuse, kFFPInputDiffuse},
        {v_specular, kFFPInputSpecular}, {v_psize, kFFPInputPointSize},
      };
      uint32_t scratch = ~0u;
      auto convert = [&](uint32_t reg, uint32_t slot) {
        uint32_t conversion = (key.input_conversions >> (slot * 2)) & 3;
        if (reg == ~0u || conversion == sm3::kInputAsIs)
          return;
        if (conversion == sm3::kInputDec3N && scratch == ~0u)
          scratch = b.AllocTemp();
        converted[reg] = b.AllocTemp();
        sm3::EmitInputConversion(b, converted[reg], scratch, Input(reg), conversion);
      };
      for (auto &[reg, slot] : slots)
        convert(reg, slot);
      for (UINT t = 0; t < 8; t++)
        convert(v_tex[t], kFFPInputTexcoord0 + t);
    }
    auto Input = [&](uint32_t reg, uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3) -> Operand {
      auto it = converted.find(reg);
      return it != converted.end() ? TempSwizzle(it->second, x, y, z, w) : sm4::Input(reg, x, y, z, w);
    };

    /* Varyings live in the link registers every stage agrees on (see
     * sm3::kLinkColor0), so a programmable pixel shader reads them too. */
    b.DeclOutputPosition(0);
    const uint32_t o_color0 = sm3::kLinkColor0, o_color1 = sm3::kLinkColor0 + 1;
    const uint32_t outputs = VertexOutputs(key) | key.vs_default_outputs;
    for (uint32_t reg = 1; reg < sm3::kLinkRegisterCount; reg++)
      if (outputs & sm3::LinkBit(reg))
        sm3::DeclareLinkIO(b, reg);
    if (key.point_size)
      sm3::DeclareLinkIO(b, sm3::kLinkPointSize);

    uint32_t r_clip = b.AllocTemp();   // clip-space position
    uint32_t r_view = b.AllocTemp();   // view-space position
    Operand pos_in = Input(v_pos);
    if (!key.pretransformed) {
      // Fixed-function transform ignores the w of an XYZW position: it is 1.
      uint32_t r_pos = b.AllocTemp();
      b.Mov(WithMask(Temp(r_pos), 0b0111), pos_in);
      b.Mov(WithMask(Temp(r_pos), 0b1000), Imm4(1, 1, 1, 1));
      pos_in = TempSwizzle(r_pos, 0, 1, 2, 3);
    }

    if (key.pretransformed) {
      // Screen coordinates + rhw: undo the viewport transform and restore a
      // homogeneous w so interpolation stays perspective-correct.
      uint32_t rw = b.AllocTemp();
      b.Rcp(WithMask(Temp(rw), 0b0001), WithSwizzle(pos_in, 3, 3, 3, 3));
      b.Mad(WithMask(Temp(r_clip), 0b0001), WithSwizzle(pos_in, 0, 0, 0, 0), CB(kVSViewportScale, 0, 0, 0, 0), CB(kVSViewportOffset, 0, 0, 0, 0));
      b.Mad(WithMask(Temp(r_clip), 0b0010), WithSwizzle(pos_in, 1, 1, 1, 1), CB(kVSViewportScale, 1, 1, 1, 1), CB(kVSViewportOffset, 1, 1, 1, 1));
      b.Mov(WithMask(Temp(r_clip), 0b0100), WithSwizzle(pos_in, 2, 2, 2, 2));
      b.Mul(WithMask(Temp(r_clip), 0b0111), TempSwizzle(r_clip, 0, 1, 2, 2), TempSwizzle(rw, 0, 0, 0, 0));
      b.Mov(WithMask(Temp(r_clip), 0b1000), TempSwizzle(rw, 0, 0, 0, 0));

      b.Mov(Temp(r_view), pos_in); // fog distance source
    } else {
      EmitMatMul4(b, Output(0), r_clip, pos_in, kVSWorldViewProj);

      EmitMatMul4(b, Operand{}, r_view, pos_in, kVSWorldView);
    }

    // --- view-space normal --------------------------------------------------------
    // Lighting needs it, and so do the generated texture coordinates. D3D9 only
    // renormalises it under D3DRS_NORMALIZENORMALS; a scaled world matrix
    // scales the lighting otherwise.
    bool texgen_normal = false;
    for (UINT s = 0; s < 8; s++)
      if ((key.vs_texcoord_outputs & (1u << s)) && key.stage_texgen[s] != 0 && key.stage_texgen[s] != 2)
        texgen_normal = true;
    uint32_t r_normal = ~0u;
    if (key.lighting || texgen_normal) {
      r_normal = b.AllocTemp();
      if (key.has_normal && !key.pretransformed) {
        EmitMatMul3(b, r_normal, Input(v_normal), kVSNormalMatrix);
        if (key.normalize_normals) {
          uint32_t t = b.AllocTemp();
          b.Dp3(WithMask(Temp(t), 0b0001), TempSwizzle(r_normal, 0, 1, 2, 3), TempSwizzle(r_normal, 0, 1, 2, 3));
          b.Rsq(WithMask(Temp(t), 0b0001), TempSwizzle(t, 0, 0, 0, 0));
          b.Mul(WithMask(Temp(r_normal), 0b0111), TempSwizzle(r_normal, 0, 1, 2, 3), TempSwizzle(t, 0, 0, 0, 0));
        }
      } else {
        b.Mov(Temp(r_normal), Imm4(0, 0, 0, 0));
      }
    }

    /* The user clip planes. The fixed-function pipeline takes them in world
     * space; the constants hold them brought into view space, so the
     * distance to each is a dot product with the view-space position. */
    if (key.clip_planes) {
      b.DeclOutputClipDistance(sm3::kClipDistanceRegister, 0);
      b.DeclOutputClipDistance(sm3::kClipDistanceRegister + 1, 1);
      for (uint32_t i = 0; i < 8; i++)
        b.Dp4(WithMask(Output(sm3::kClipDistanceRegister + i / 4), uint8_t(1u << (i % 4))),
              TempSwizzle(r_view, 0, 1, 2, 3), CB(kVSClipPlanes + i));
    }

    // D3DRS_DEPTHBIAS, added to the depth after the divide (see
    // sm3::kExtraCBDepthBias for why it is not the rasterizer's).
    b.Mad(WithMask(Temp(r_clip), 0b0100), CB(kVSPointScale, 3, 3, 3, 3), TempSwizzle(r_clip, 3, 3, 3, 3),
          TempSwizzle(r_clip, 2, 2, 2, 2));
    b.Mov(Output(0), TempSwizzle(r_clip, 0, 1, 2, 3));

    // --- colors ---------------------------------------------------------------
    Operand mat_diffuse = CB(kVSMaterialDiffuse);
    Operand vtx_diffuse = key.has_diffuse ? Input(v_diffuse) : Imm4(1, 1, 1, 1);
    Operand vtx_specular = key.has_specular ? Input(v_specular) : Imm4(0, 0, 0, 0);

    if (!key.lighting) {
      b.Mov(Output(o_color0), key.has_diffuse ? vtx_diffuse : Imm4(1, 1, 1, 1));
      b.Mov(Output(o_color1), vtx_specular);
    } else {
      auto materialSource = [&](uint8_t source, Operand material_value) -> Operand {
        if (!key.color_vertex) return material_value;
        if (source == D3DMCS_COLOR1 && key.has_diffuse) return Input(v_diffuse);
        if (source == D3DMCS_COLOR2 && key.has_specular) return Input(v_specular);
        return material_value;
      };
      Operand src_diffuse = materialSource(key.diffuse_source, mat_diffuse);
      Operand src_ambient = materialSource(key.ambient_source, CB(kVSMaterialAmbient));
      Operand src_specular = materialSource(key.specular_source, CB(kVSMaterialSpecular));
      Operand src_emissive = materialSource(key.emissive_source, CB(kVSMaterialEmissive));


      uint32_t r_amb = b.AllocTemp();  // ambient+emissive accumulator
      uint32_t r_dif = b.AllocTemp();  // diffuse light accumulator
      uint32_t r_spec = b.AllocTemp(); // specular light accumulator
      b.Mad(Temp(r_amb), CB(kVSGlobalAmbient), src_ambient, src_emissive);
      b.Mov(Temp(r_dif), Imm4(0, 0, 0, 0));
      b.Mov(Temp(r_spec), Imm4(0, 0, 0, 0));

      for (UINT li = 0; li < key.light_count; li++) {
        uint32_t base = kVSLights + li * 6;
        uint32_t r_ldir = b.AllocTemp(); // vertex -> light direction (normalized)
        uint32_t r_att = b.AllocTemp();  // .x attenuation
        uint8_t type = key.light_type[li];

        if (type == D3DLIGHT_DIRECTIONAL) {
          b.Mov(WithMask(Temp(r_ldir), 0b0111), Neg(CB(base + 4)));
          b.Mov(WithMask(Temp(r_att), 0b0001), Imm4(1, 1, 1, 1));
        } else {
          uint32_t r_d = b.AllocTemp();
          b.Add(WithMask(Temp(r_ldir), 0b0111), CB(base + 3), Neg(TempSwizzle(r_view, 0, 1, 2, 3)));
          b.Dp3(WithMask(Temp(r_d), 0b0001), TempSwizzle(r_ldir, 0, 1, 2, 3), TempSwizzle(r_ldir, 0, 1, 2, 3)); // d^2
          b.Rsq(WithMask(Temp(r_d), 0b0010), TempSwizzle(r_d, 0, 0, 0, 0));                                     // 1/d
          b.Mul(WithMask(Temp(r_ldir), 0b0111), TempSwizzle(r_ldir, 0, 1, 2, 3), TempSwizzle(r_d, 1, 1, 1, 1));
          b.Mul(WithMask(Temp(r_d), 0b0100), TempSwizzle(r_d, 0, 0, 0, 0), TempSwizzle(r_d, 1, 1, 1, 1)); // d
          // att = 1 / (a0 + a1*d + a2*d^2), zeroed beyond the light's range.
          uint32_t r_a = b.AllocTemp();
          b.Mad(WithMask(Temp(r_a), 0b0001), CB(base + 5, 1, 1, 1, 1), TempSwizzle(r_d, 2, 2, 2, 2), CB(base + 5, 0, 0, 0, 0));
          b.Mad(WithMask(Temp(r_a), 0b0001), CB(base + 5, 2, 2, 2, 2), TempSwizzle(r_d, 0, 0, 0, 0), TempSwizzle(r_a, 0, 0, 0, 0));
          b.Rcp(WithMask(Temp(r_att), 0b0001), TempSwizzle(r_a, 0, 0, 0, 0));
          uint32_t r_in = b.AllocTemp();
          b.Ge(WithMask(Temp(r_in), 0b0001), CB(base + 3, 3, 3, 3, 3), TempSwizzle(r_d, 2, 2, 2, 2)); // range >= d
          b.Movc(WithMask(Temp(r_att), 0b0001), TempSwizzle(r_in, 0, 0, 0, 0), TempSwizzle(r_att, 0, 0, 0, 0), Imm4(0, 0, 0, 0));
          if (type == D3DLIGHT_SPOT) {
            // spot = saturate((rho - cos(phi/2)) / (cos(theta/2) - cos(phi/2)))
            uint32_t r_rho = b.AllocTemp();
            b.Dp3(WithMask(Temp(r_rho), 0b0001), Neg(TempSwizzle(r_ldir, 0, 1, 2, 3)), CB(base + 4));
            uint32_t r_den = b.AllocTemp();
            b.Add(WithMask(Temp(r_den), 0b0001), CB(base + 4, 3, 3, 3, 3), Neg(CB(base + 5, 3, 3, 3, 3)));
            b.Rcp(WithMask(Temp(r_den), 0b0001), TempSwizzle(r_den, 0, 0, 0, 0));
            b.Add(WithMask(Temp(r_rho), 0b0001), TempSwizzle(r_rho, 0, 0, 0, 0), Neg(CB(base + 5, 3, 3, 3, 3)));
            b.Mul(Saturate(WithMask(Temp(r_rho), 0b0001)), TempSwizzle(r_rho, 0, 0, 0, 0), TempSwizzle(r_den, 0, 0, 0, 0));
            b.Mul(WithMask(Temp(r_att), 0b0001), TempSwizzle(r_att, 0, 0, 0, 0), TempSwizzle(r_rho, 0, 0, 0, 0));
          }
        }

        uint32_t r_ndl = b.AllocTemp();
        b.Dp3(WithMask(Temp(r_ndl), 0b0001), TempSwizzle(r_normal, 0, 1, 2, 3), TempSwizzle(r_ldir, 0, 1, 2, 3));
        b.Max(WithMask(Temp(r_ndl), 0b0001), TempSwizzle(r_ndl, 0, 0, 0, 0), Imm4(0, 0, 0, 0));
        b.Mul(WithMask(Temp(r_ndl), 0b0001), TempSwizzle(r_ndl, 0, 0, 0, 0), TempSwizzle(r_att, 0, 0, 0, 0));
        b.Mad(Temp(r_dif), CB(base + 0), TempSwizzle(r_ndl, 0, 0, 0, 0), TempSwizzle(r_dif, 0, 1, 2, 3));
        // Per-light ambient contribution.
        b.Mad(Temp(r_amb), CB(base + 2), src_ambient, TempSwizzle(r_amb, 0, 1, 2, 3));

        if (key.specular_enable) {
          // Blinn half-vector specular: pow(max(dot(n, h), 0), power)
          uint32_t r_h = b.AllocTemp();
          uint32_t r_vd = b.AllocTemp();
          b.Dp3(WithMask(Temp(r_vd), 0b1000), TempSwizzle(r_view, 0, 1, 2, 3), TempSwizzle(r_view, 0, 1, 2, 3));
          b.Rsq(WithMask(Temp(r_vd), 0b1000), TempSwizzle(r_vd, 3, 3, 3, 3));
          b.Mul(WithMask(Temp(r_vd), 0b0111), Neg(TempSwizzle(r_view, 0, 1, 2, 3)), TempSwizzle(r_vd, 3, 3, 3, 3));
          b.Add(WithMask(Temp(r_h), 0b0111), TempSwizzle(r_ldir, 0, 1, 2, 3), TempSwizzle(r_vd, 0, 1, 2, 3));
          uint32_t r_hn = b.AllocTemp();
          b.Dp3(WithMask(Temp(r_hn), 0b0001), TempSwizzle(r_h, 0, 1, 2, 3), TempSwizzle(r_h, 0, 1, 2, 3));
          b.Rsq(WithMask(Temp(r_hn), 0b0001), TempSwizzle(r_hn, 0, 0, 0, 0));
          b.Mul(WithMask(Temp(r_h), 0b0111), TempSwizzle(r_h, 0, 1, 2, 3), TempSwizzle(r_hn, 0, 0, 0, 0));
          uint32_t r_ndh = b.AllocTemp();
          b.Dp3(WithMask(Temp(r_ndh), 0b0001), TempSwizzle(r_normal, 0, 1, 2, 3), TempSwizzle(r_h, 0, 1, 2, 3));
          b.Max(WithMask(Temp(r_ndh), 0b0001), TempSwizzle(r_ndh, 0, 0, 0, 0), Imm4(0, 0, 0, 0));
          b.Log(WithMask(Temp(r_ndh), 0b0010), Abs(TempSwizzle(r_ndh, 0, 0, 0, 0)));
          b.Mul(WithMask(Temp(r_ndh), 0b0010), TempSwizzle(r_ndh, 1, 1, 1, 1), CB(kVSMaterialSpecular, 3, 3, 3, 3));
          b.ExpI(WithMask(Temp(r_ndh), 0b0010), TempSwizzle(r_ndh, 1, 1, 1, 1));
          // pow(0, e) -> 0, not exp2(e * log2(0)) = NaN pathway
          uint32_t r_gt = b.AllocTemp();
          b.Lt(WithMask(Temp(r_gt), 0b0001), Imm4(0, 0, 0, 0), TempSwizzle(r_ndh, 0, 0, 0, 0));
          b.Movc(WithMask(Temp(r_ndh), 0b0010), TempSwizzle(r_gt, 0, 0, 0, 0), TempSwizzle(r_ndh, 1, 1, 1, 1), Imm4(0, 0, 0, 0));
          b.Mul(WithMask(Temp(r_ndh), 0b0010), TempSwizzle(r_ndh, 1, 1, 1, 1), TempSwizzle(r_att, 0, 0, 0, 0));
          b.Mad(Temp(r_spec), CB(base + 1), TempSwizzle(r_ndh, 1, 1, 1, 1), TempSwizzle(r_spec, 0, 1, 2, 3));
        }
      }

      // color0 = saturate(ambient + diffuse_acc * diffuse_source), alpha from
      // the diffuse source; color1 = saturate(spec_acc * specular_source).
      uint32_t r_c0 = b.AllocTemp();
      b.Mad(Temp(r_c0), TempSwizzle(r_dif, 0, 1, 2, 3), src_diffuse, TempSwizzle(r_amb, 0, 1, 2, 3));
      b.Mov(Saturate(WithMask(Output(o_color0), 0b0111)), TempSwizzle(r_c0, 0, 1, 2, 3));
      b.Mov(Saturate(WithMask(Output(o_color0), 0b1000)), WithSwizzle(src_diffuse, 3, 3, 3, 3));
      uint32_t r_c1 = b.AllocTemp();
      b.Mul(Temp(r_c1), TempSwizzle(r_spec, 0, 1, 2, 3), src_specular);
      b.Mov(Saturate(Output(o_color1)), TempSwizzle(r_c1, 0, 1, 2, 3));
    }

    // --- fog ------------------------------------------------------------------
    // The factor the pixel stage blends with, when it does not compute one
    // from the depth itself. An untransformed vertex with a vertex fog mode
    // gets the mode's curve over its eye depth (or distance, for range fog);
    // in every other case D3D9 takes the specular colour's alpha.
    if (outputs & sm3::LinkBit(sm3::kLinkFog)) {
      Operand fog_out = WithMask(Output(sm3::kLinkFog), 0b0001);
      if (key.fog_mode == D3DFOG_NONE) {
        b.Mov(fog_out, key.has_specular ? WithSwizzle(Input(v_specular), 3, 3, 3, 3) : Imm4(0, 0, 0, 0));
      } else {
        constexpr float kLog2E = 1.442695041f;
        uint32_t r_f = b.AllocTemp();
        if (key.range_fog) {
          b.Dp3(WithMask(Temp(r_f), 0b0001), TempSwizzle(r_view, 0, 1, 2, 3), TempSwizzle(r_view, 0, 1, 2, 3));
          b.Sqrt(WithMask(Temp(r_f), 0b0001), TempSwizzle(r_f, 0, 0, 0, 0));
        } else {
          b.Mov(WithMask(Temp(r_f), 0b0001), Abs(TempSwizzle(r_view, 2, 2, 2, 2)));
        }
        switch (key.fog_mode) {
        case D3DFOG_LINEAR:
          // f = (end - d) * scale, scale = 1 / (end - start); FillConstants
          // makes both zero when start == end, which fogs everything
          b.Add(WithMask(Temp(r_f), 0b0010), CB(kVSFogParams, 1, 1, 1, 1), Neg(TempSwizzle(r_f, 0, 0, 0, 0)));
          b.Mul(Saturate(WithMask(Temp(r_f), 0b0010)), TempSwizzle(r_f, 1, 1, 1, 1), CB(kVSFogParams, 3, 3, 3, 3));
          break;
        case D3DFOG_EXP:
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 0, 0, 0, 0), CB(kVSFogParams, 2, 2, 2, 2));
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 1, 1, 1, 1), Imm4(-kLog2E, -kLog2E, -kLog2E, -kLog2E));
          b.ExpI(Saturate(WithMask(Temp(r_f), 0b0010)), TempSwizzle(r_f, 1, 1, 1, 1));
          break;
        default: // D3DFOG_EXP2
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 0, 0, 0, 0), CB(kVSFogParams, 2, 2, 2, 2));
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 1, 1, 1, 1), TempSwizzle(r_f, 1, 1, 1, 1));
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 1, 1, 1, 1), Imm4(-kLog2E, -kLog2E, -kLog2E, -kLog2E));
          b.ExpI(Saturate(WithMask(Temp(r_f), 0b0010)), TempSwizzle(r_f, 1, 1, 1, 1));
          break;
        }
        b.Mov(fog_out, TempSwizzle(r_f, 1, 1, 1, 1));
      }
    }

    // --- texture coordinates, one per stage --------------------------------------
    // Stage s reads the set D3DTSS_TEXCOORDINDEX names, or a coordinate
    // generated from the eye-space position/normal, through texture matrix s.
    // The transform follows D3D9's fixed-function rules, which differ from a
    // shader's: a set with fewer than four components is padded with 1 in the
    // first missing component and 0 after it; components past the transform's
    // count are zero; and a projected transform moves its divisor into w,
    // where the pixel stage divides by it.
    for (UINT s = 0; s < 8; s++) {
      if (!(key.vs_texcoord_outputs & (1u << s)))
        continue;
      Operand out = Output(sm3::kLinkTexcoord0 + s);
      uint32_t r_c = b.AllocTemp();
      uint32_t components = 3;
      switch (key.stage_texgen[s]) {
      case 0: { // D3DTSS_TCI_PASSTHRU
        uint8_t set = key.stage_texcoord_index[s];
        if (key.texcoord_sets & (1u << set)) {
          b.Mov(Temp(r_c), Input(v_tex[set]));
          components = key.texcoord_size[set];
        } else {
          b.Mov(Temp(r_c), Imm4(0, 0, 0, 0));
          components = 0;
        }
        break;
      }
      case 1: // D3DTSS_TCI_CAMERASPACENORMAL
        b.Mov(WithMask(Temp(r_c), 0b0111), TempSwizzle(r_normal, 0, 1, 2, 3));
        b.Mov(WithMask(Temp(r_c), 0b1000), Imm4(1, 1, 1, 1));
        break;
      case 2: // D3DTSS_TCI_CAMERASPACEPOSITION
        b.Mov(WithMask(Temp(r_c), 0b0111), TempSwizzle(r_view, 0, 1, 2, 3));
        b.Mov(WithMask(Temp(r_c), 0b1000), Imm4(1, 1, 1, 1));
        break;
      case 3:   // D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR
      case 4: { // D3DTSS_TCI_SPHEREMAP
        // r = e - 2 (n.e) n, e the normalised eye-to-vertex direction
        uint32_t r_e = b.AllocTemp();
        b.Dp3(WithMask(Temp(r_e), 0b1000), TempSwizzle(r_view, 0, 1, 2, 3), TempSwizzle(r_view, 0, 1, 2, 3));
        b.Rsq(WithMask(Temp(r_e), 0b1000), TempSwizzle(r_e, 3, 3, 3, 3));
        b.Mul(WithMask(Temp(r_e), 0b0111), TempSwizzle(r_view, 0, 1, 2, 3), TempSwizzle(r_e, 3, 3, 3, 3));
        b.Dp3(WithMask(Temp(r_e), 0b1000), TempSwizzle(r_normal, 0, 1, 2, 3), TempSwizzle(r_e, 0, 1, 2, 3));
        b.Add(WithMask(Temp(r_e), 0b1000), TempSwizzle(r_e, 3, 3, 3, 3), TempSwizzle(r_e, 3, 3, 3, 3));
        b.Mad(WithMask(Temp(r_c), 0b0111), Neg(TempSwizzle(r_normal, 0, 1, 2, 3)), TempSwizzle(r_e, 3, 3, 3, 3),
              TempSwizzle(r_e, 0, 1, 2, 3));
        b.Mov(WithMask(Temp(r_c), 0b1000), Imm4(1, 1, 1, 1));
        if (key.stage_texgen[s] == 4) {
          // (r.xy / m + 0.5, 0, 1), m = 2 |(r.x, r.y, r.z - 1)|
          uint32_t r_m = b.AllocTemp();
          b.Add(WithMask(Temp(r_m), 0b0111), TempSwizzle(r_c, 0, 1, 2, 3), Imm4(0, 0, -1, 0));
          b.Dp3(WithMask(Temp(r_m), 0b1000), TempSwizzle(r_m, 0, 1, 2, 3), TempSwizzle(r_m, 0, 1, 2, 3));
          b.Rsq(WithMask(Temp(r_m), 0b1000), TempSwizzle(r_m, 3, 3, 3, 3));
          b.Mul(WithMask(Temp(r_m), 0b1000), TempSwizzle(r_m, 3, 3, 3, 3), Imm4(0.5f, 0.5f, 0.5f, 0.5f));
          b.Mad(WithMask(Temp(r_c), 0b0011), TempSwizzle(r_c, 0, 1, 0, 0), TempSwizzle(r_m, 3, 3, 3, 3),
                Imm4(0.5f, 0.5f, 0.5f, 0.5f));
          b.Mov(WithMask(Temp(r_c), 0b1100), Imm4(0, 0, 0, 1));
        }
        break;
      }
      default:
        b.Mov(Temp(r_c), Imm4(0, 0, 0, 0));
        components = 0;
        break;
      }

      /* Transformed vertices take no texture matrix. */
      if (key.pretransformed) {
        b.Mov(out, TempSwizzle(r_c, 0, 1, 2, 3));
        continue;
      }
      uint8_t count = key.texture_transform[s];
      bool projected = key.stage_projected[s];
      if (count < 2 || count > 4) {
        // No transform. w is 0 past a short set, or the divisor when projected.
        if (projected && components >= 1 && components <= 3) {
          uint8_t c = (uint8_t)(components - 1);
          b.Mov(WithMask(Temp(r_c), 0b1000), TempSwizzle(r_c, c, c, c, c));
        } else if (components < 4) {
          b.Mov(WithMask(Temp(r_c), 0b1000), Imm4(0, 0, 0, 0));
        }
        b.Mov(out, TempSwizzle(r_c, 0, 1, 2, 3));
        continue;
      }
      // The input layout padded a short set with (.., 0, 0, 1); the
      // fixed-function pipeline pads with (.., 1, 0, 0).
      if (components == 1)
        b.Mov(WithMask(Temp(r_c), 0b1110), Imm4(0, 1, 0, 0));
      else if (components == 2)
        b.Mov(WithMask(Temp(r_c), 0b1100), Imm4(0, 0, 1, 0));
      uint32_t r_t = b.AllocTemp();
      EmitMatMul4(b, Operand{}, r_t, TempSwizzle(r_c, 0, 1, 2, 3), kVSTexTransform + s * 4);
      if (projected && (count == 2 || count == 3)) {
        uint8_t c = (uint8_t)(count - 1);
        b.Mov(WithMask(Temp(r_t), 0b1000), TempSwizzle(r_t, c, c, c, c));
      } else if (count < 4) {
        b.Mov(WithMask(Temp(r_t), 0b1000), Imm4(0, 0, 0, 0));
      }
      if (count < 3)
        b.Mov(WithMask(Temp(r_t), 0b0100), Imm4(0, 0, 0, 0));
      b.Mov(out, TempSwizzle(r_t, 0, 1, 2, 3));
    }

    // --- point size -----------------------------------------------------------
    // size * viewport height / sqrt(A + B d + C d^2) under point scaling, d the
    // eye distance, then clamped to the point size range.
    if (key.point_size) {
      uint32_t r_s = b.AllocTemp();
      b.Mov(WithMask(Temp(r_s), 0b0001),
            key.has_psize ? WithSwizzle(Input(v_psize), 0, 0, 0, 0) : CB(kVSPointSize, 0, 0, 0, 0));
      if (key.point_scale) {
        uint32_t r_d = b.AllocTemp();
        b.Dp3(WithMask(Temp(r_d), 0b0010), TempSwizzle(r_view, 0, 1, 2, 3), TempSwizzle(r_view, 0, 1, 2, 3)); // d^2
        b.Sqrt(WithMask(Temp(r_d), 0b0001), TempSwizzle(r_d, 1, 1, 1, 1));                                    // d
        b.Mad(WithMask(Temp(r_d), 0b0100), CB(kVSPointScale, 1, 1, 1, 1), TempSwizzle(r_d, 0, 0, 0, 0),
              CB(kVSPointScale, 0, 0, 0, 0));
        b.Mad(WithMask(Temp(r_d), 0b0100), CB(kVSPointScale, 2, 2, 2, 2), TempSwizzle(r_d, 1, 1, 1, 1),
              TempSwizzle(r_d, 2, 2, 2, 2));
        b.Rsq(WithMask(Temp(r_d), 0b0100), TempSwizzle(r_d, 2, 2, 2, 2));
        b.Mul(WithMask(Temp(r_s), 0b0001), TempSwizzle(r_s, 0, 0, 0, 0), TempSwizzle(r_d, 2, 2, 2, 2));
        b.Mul(WithMask(Temp(r_s), 0b0001), TempSwizzle(r_s, 0, 0, 0, 0), CB(kVSPointSize, 3, 3, 3, 3));
      }
      b.Max(WithMask(Temp(r_s), 0b0001), TempSwizzle(r_s, 0, 0, 0, 0), CB(kVSPointSize, 1, 1, 1, 1));
      b.Min(WithMask(Output(sm3::kLinkPointSize), 0b0001), TempSwizzle(r_s, 0, 0, 0, 0),
            CB(kVSPointSize, 2, 2, 2, 2));
    }

    /* Whatever else the pixel stage reads: D3D9 hands it zero. */
    for (uint32_t reg = 1; reg < sm3::kLinkRegisterCount; reg++)
      if (key.vs_default_outputs & sm3::LinkBit(reg) & ~VertexOutputs(key))
        b.Mov(WithMask(Output(reg), sm3::LinkMask(reg)), Imm4(0, 0, 0, 0));

    b.Ret();
    if (!b.Finish(out.vs_dxbc))
      return false;
    HRESULT hr = device_->GetD3D11Device()->CreateVertexShader(out.vs_dxbc.data(), out.vs_dxbc.size(), nullptr, &out.vs);
    if (FAILED(hr)) {
      ERR("FFP: vertex shader creation failed");
      return false;
    }
  }

  // ---------------------------------------------------------------------------
  // Pixel shader
  // ---------------------------------------------------------------------------
  {
    std::vector<uint8_t> ps_dxbc;
    if (!ffp::BuildPixelShaderDXBC(key, ps_dxbc))
      return false;
    HRESULT hr = device_->GetD3D11Device()->CreatePixelShader(ps_dxbc.data(), ps_dxbc.size(), nullptr, &out.ps);
    if (FAILED(hr)) {
      ERR("FFP: pixel shader creation failed");
      return false;
    }
  }

  return true;
}

namespace ffp {

/* The fixed-function pixel shader, as DXBC. Split out from Generate so it can
 * be built without a device; everything it needs is in the key. */
bool BuildPixelShaderDXBC(const FFPShaderKey &key, std::vector<uint8_t> &ps_dxbc) {
  {
    Sm4Builder b(Sm4Builder::Stage::Pixel);
    b.DeclConstantBuffer(0, kPSSizeVec4);

    b.DeclInputPositionPS(0);
    const uint32_t i_color0 = sm3::kLinkColor0, i_color1 = sm3::kLinkColor0 + 1, i_fog = sm3::kLinkFog;
    uint8_t sampled = 0;
    for (UINT s = 0; s < 8; s++) {
      if (key.stage_color_op[s] == D3DTOP_DISABLE) break;
      if (key.stage_texture_type[s]) sampled |= 1u << s;
    }
    const uint32_t inputs = PixelInputs(key, sampled);
    for (uint32_t reg = 1; reg < sm3::kLinkRegisterCount; reg++)
      if (inputs & sm3::LinkBit(reg))
        sm3::DeclareLinkIO(b, reg);
    // A system value, in a register no varying of this shader uses.
    const uint32_t i_point_coord = sm3::kLinkRegisterCount - 1;
    if (key.point_sprite)
      b.DeclIOWithSemantic(i_point_coord, 0b0011, "SV_PointCoord", 0);
    b.DeclOutputTarget(0, 0);

    for (UINT s = 0; s < 8; s++) {
      if (key.stage_color_op[s] == D3DTOP_DISABLE) break;
      if (!key.stage_texture_type[s]) continue;
      switch (key.stage_texture_type[s]) {
      case 2: b.DeclResourceTextureCube(s); break;
      case 3: b.DeclResourceTexture3D(s); break;
      default: b.DeclResourceTexture2D(s); break;
      }
      b.DeclSampler(s);
    }

    uint32_t r_cur = b.AllocTemp();  // "current" register
    uint32_t r_tmp = b.AllocTemp();  // D3DTA_TEMP register
    uint32_t r_tex = b.AllocTemp();  // sampled texture of the active stage
    // Bump environment mapping perturbs the *next* stage: D3DTOP_BUMPENVMAP at
    // stage n produces no colour of its own, it offsets the coordinates stage
    // n+1 samples with, and the luminance variant additionally scales what that
    // stage reads. Both are carried forward here and consumed once.
    uint32_t r_bump = b.AllocTemp();     // (du, dv) for the next sample
    uint32_t r_bump_lum = b.AllocTemp(); // luminance scale for the next sample
    b.Mov(Temp(r_cur), Input(i_color0));
    b.Mov(Temp(r_tmp), Imm4(0, 0, 0, 0));
    b.Mov(Temp(r_bump), Imm4(0, 0, 0, 0));
    b.Mov(Temp(r_bump_lum), Imm4(1, 1, 1, 1));

    for (UINT s = 0; s < 8; s++) {
      if (key.stage_color_op[s] == D3DTOP_DISABLE) break;

      if (key.stage_texture_type[s]) {
        // The vertex stage put this stage's coordinate, already through its
        // texture matrix, in the stage's own link register. A projected one
        // carries its divisor in w; a cube map is never projected.
        Operand coord = Input(sm3::kLinkTexcoord0 + s);
        if (key.point_sprite) {
          uint32_t r_pc = b.AllocTemp();
          b.Mov(WithMask(Temp(r_pc), 0b0011), Input(i_point_coord, 0, 1, 0, 0));
          b.Mov(WithMask(Temp(r_pc), 0b1100), Imm4(0, 0, 0, 1));
          coord = TempSwizzle(r_pc, 0, 1, 2, 3);
        } else if (key.stage_projected[s] && key.stage_texture_type[s] != 2) {
          uint32_t r_p = b.AllocTemp();
          b.Rcp(WithMask(Temp(r_p), 0b1000), WithSwizzle(coord, 3, 3, 3, 3));
          b.Mul(WithMask(Temp(r_p), 0b0111), coord, TempSwizzle(r_p, 3, 3, 3, 3));
          coord = TempSwizzle(r_p, 0, 1, 2, 3);
        }
        // Apply a pending perturbation from the previous stage, then clear it:
        // it applies to exactly one sample.
        uint32_t r_perturbed = b.AllocTemp();
        b.Mov(Temp(r_perturbed), coord);
        b.Add(WithMask(Temp(r_perturbed), 0b0011), TempSwizzle(r_perturbed, 0, 1, 0, 0),
              TempSwizzle(r_bump, 0, 1, 0, 0));
        b.Mov(Temp(r_bump), Imm4(0, 0, 0, 0));

        if (key.stage_fetch4 & (1u << s)) {
          // Fetch4: the four texels of AMD's footprint, in D3D9's order (see
          // sm3's EmitFetch4).
          b.ResInfoRcp(WithMask(Temp(r_tex), 0b0011), s);
          b.Mad(WithMask(Temp(r_tex), 0b0011), TempSwizzle(r_tex, 0, 1, 0, 0), Imm4(0.5f, 0.5f, 0, 0),
                TempSwizzle(r_perturbed, 0, 1, 2, 3));
          b.Gather4(Temp(r_tex), TempSwizzle(r_tex, 0, 1, 1, 1), s, s,
                    key.stage_fetch4_alpha & (1u << s) ? 3 : 0);
          b.Mov(Temp(r_tex), TempSwizzle(r_tex, 2, 0, 1, 3));
        } else {
          b.Sample(Temp(r_tex), TempSwizzle(r_perturbed, 0, 1, 2, 3), s, s);
        }

        // And the luminance scale, which likewise applies once.
        b.Mul(WithMask(Temp(r_tex), 0b0111), TempSwizzle(r_tex, 0, 1, 2, 3),
              TempSwizzle(r_bump_lum, 0, 0, 0, 0));
        b.Mov(Temp(r_bump_lum), Imm4(1, 1, 1, 1));
      }

      auto resolveArg = [&](uint8_t arg, bool alpha_channel) -> Operand {
        Operand base;
        switch (arg & 0xF) {
        case D3DTA_DIFFUSE & 0xF: base = Input(i_color0); break;
        case D3DTA_CURRENT & 0xF: base = TempSwizzle(r_cur, 0, 1, 2, 3); break;
        case D3DTA_TEXTURE & 0xF: base = TempSwizzle(r_tex, 0, 1, 2, 3); break;
        case D3DTA_TFACTOR & 0xF: base = CB(kPSTextureFactor); break;
        case D3DTA_SPECULAR & 0xF: base = Input(i_color1); break;
        case D3DTA_TEMP & 0xF: base = TempSwizzle(r_tmp, 0, 1, 2, 3); break;
        case D3DTA_CONSTANT & 0xF: base = CB(kPSStageConstant + s); break;
        default: base = TempSwizzle(r_cur, 0, 1, 2, 3); break;
        }
        if (arg & D3DTA_ALPHAREPLICATE)
          base = WithSwizzle(base, 3, 3, 3, 3);
        if (arg & D3DTA_COMPLEMENT) {
          uint32_t r_c = b.AllocTemp();
          b.Add(Temp(r_c), Imm4(1, 1, 1, 1), Neg(base));
          base = TempSwizzle(r_c, 0, 1, 2, 3);
        }
        (void)alpha_channel;
        return base;
      };

      // Evaluates one color/alpha op into r_out under `mask`.
      auto emitOp = [&](uint8_t op, uint8_t a1, uint8_t a2, uint8_t a0, uint32_t r_out, uint8_t mask) {
        Operand dst = WithMask(Temp(r_out), mask);
        Operand arg1 = resolveArg(a1, mask == 0b1000);
        Operand arg2 = resolveArg(a2, mask == 0b1000);
        switch (op) {
        case D3DTOP_SELECTARG1: b.Mov(dst, arg1); break;
        case D3DTOP_SELECTARG2: b.Mov(dst, arg2); break;
        case D3DTOP_MODULATE: b.Mul(dst, arg1, arg2); break;
        case D3DTOP_MODULATE2X: {
          uint32_t r = b.AllocTemp();
          b.Mul(Temp(r), arg1, arg2);
          b.Add(dst, TempSwizzle(r, 0, 1, 2, 3), TempSwizzle(r, 0, 1, 2, 3));
          break;
        }
        case D3DTOP_MODULATE4X: {
          uint32_t r = b.AllocTemp();
          b.Mul(Temp(r), arg1, arg2);
          b.Mul(dst, TempSwizzle(r, 0, 1, 2, 3), Imm4(4, 4, 4, 4));
          break;
        }
        case D3DTOP_ADD: b.Add(dst, arg1, arg2); break;
        case D3DTOP_ADDSIGNED: {
          uint32_t r = b.AllocTemp();
          b.Add(Temp(r), arg1, arg2);
          b.Add(dst, TempSwizzle(r, 0, 1, 2, 3), Imm4(-0.5f, -0.5f, -0.5f, -0.5f));
          break;
        }
        case D3DTOP_ADDSIGNED2X: {
          uint32_t r = b.AllocTemp();
          b.Add(Temp(r), arg1, arg2);
          b.Add(Temp(r), TempSwizzle(r, 0, 1, 2, 3), Imm4(-0.5f, -0.5f, -0.5f, -0.5f));
          b.Add(dst, TempSwizzle(r, 0, 1, 2, 3), TempSwizzle(r, 0, 1, 2, 3));
          break;
        }
        case D3DTOP_SUBTRACT: b.Add(dst, arg1, Neg(arg2)); break;
        case D3DTOP_ADDSMOOTH: {
          // arg1 + arg2 * (1 - arg1)
          uint32_t r = b.AllocTemp();
          b.Add(Temp(r), Imm4(1, 1, 1, 1), Neg(arg1));
          b.Mad(dst, arg2, TempSwizzle(r, 0, 1, 2, 3), arg1);
          break;
        }
        case D3DTOP_BLENDDIFFUSEALPHA:
        case D3DTOP_BLENDTEXTUREALPHA:
        case D3DTOP_BLENDFACTORALPHA:
        case D3DTOP_BLENDCURRENTALPHA: {
          Operand f;
          switch (op) {
          case D3DTOP_BLENDDIFFUSEALPHA: f = WithSwizzle(Input(i_color0), 3, 3, 3, 3); break;
          case D3DTOP_BLENDTEXTUREALPHA: f = TempSwizzle(r_tex, 3, 3, 3, 3); break;
          case D3DTOP_BLENDFACTORALPHA: f = CB(kPSTextureFactor, 3, 3, 3, 3); break;
          default: f = TempSwizzle(r_cur, 3, 3, 3, 3); break;
          }
          // arg1*f + arg2*(1-f)
          uint32_t r = b.AllocTemp();
          b.Add(Temp(r), arg1, Neg(arg2));
          b.Mad(dst, TempSwizzle(r, 0, 1, 2, 3), f, arg2);
          break;
        }
        case D3DTOP_BLENDTEXTUREALPHAPM: {
          // arg1 + arg2*(1 - tex.a)
          uint32_t r = b.AllocTemp();
          b.Add(WithMask(Temp(r), 0b0001), Imm4(1, 1, 1, 1), Neg(TempSwizzle(r_tex, 3, 3, 3, 3)));
          b.Mad(dst, arg2, TempSwizzle(r, 0, 0, 0, 0), arg1);
          break;
        }
        case D3DTOP_DOTPRODUCT3: {
          uint32_t r = b.AllocTemp();
          b.Add(WithMask(Temp(r), 0b0111), arg1, Imm4(-0.5f, -0.5f, -0.5f, -0.5f));
          uint32_t r2 = b.AllocTemp();
          b.Add(WithMask(Temp(r2), 0b0111), arg2, Imm4(-0.5f, -0.5f, -0.5f, -0.5f));
          uint32_t r3 = b.AllocTemp();
          b.Dp3(Temp(r3), TempSwizzle(r, 0, 1, 2, 3), TempSwizzle(r2, 0, 1, 2, 3));
          b.Mul(Saturate(dst), TempSwizzle(r3, 0, 0, 0, 0), Imm4(4, 4, 4, 4));
          break;
        }
        case D3DTOP_MULTIPLYADD: {
          Operand arg0 = resolveArg(a0, mask == 0b1000);
          b.Mad(dst, arg1, arg2, arg0);
          break;
        }
        case D3DTOP_LERP: {
          Operand arg0 = resolveArg(a0, mask == 0b1000);
          uint32_t r = b.AllocTemp();
          b.Add(Temp(r), arg1, Neg(arg2));
          b.Mad(dst, arg0, TempSwizzle(r, 0, 1, 2, 3), arg2);
          break;
        }
        case D3DTOP_BUMPENVMAP:
        case D3DTOP_BUMPENVMAPLUMINANCE: {
          // No result of its own: this op only sets up the next stage.
          Operand matrix = CB(kPSBumpBase + s * kPSBumpStride);
          uint32_t r_d = b.AllocTemp();
          b.Mul(WithMask(Temp(r_d), 0b0011), WithSwizzle(matrix, 0, 1, 0, 0), TempSwizzle(r_tex, 0, 0, 0, 0));
          b.Mad(WithMask(Temp(r_d), 0b0011), WithSwizzle(matrix, 2, 3, 0, 0), TempSwizzle(r_tex, 1, 1, 1, 1),
                TempSwizzle(r_d, 0, 1, 0, 0));
          b.Mov(WithMask(Temp(r_bump), 0b0011), TempSwizzle(r_d, 0, 1, 0, 0));

          if (op == D3DTOP_BUMPENVMAPLUMINANCE) {
            Operand luminance = CB(kPSBumpBase + s * kPSBumpStride + 1);
            b.Mad(WithMask(Temp(r_bump_lum), 0b0001), TempSwizzle(r_tex, 2, 2, 2, 2),
                  WithSwizzle(luminance, 0, 0, 0, 0), WithSwizzle(luminance, 1, 1, 1, 1));
            b.Mov(WithMask(Temp(r_bump_lum), 0b0001), Saturate(TempSwizzle(r_bump_lum, 0, 0, 0, 0)));
          }
          break;
        }
        default:
          // Anything still unhandled: pass arg1 through so the stage degrades
          // visibly-but-gracefully rather than corrupting.
          b.Mov(dst, arg1);
          break;
        }
      };

      uint32_t r_out = key.stage_result_temp[s] ? r_tmp : r_cur;
      // Compute into a scratch first: ops may read CURRENT, which must keep
      // its pre-stage value until both color and alpha are evaluated.
      uint32_t r_stage = b.AllocTemp();
      emitOp(key.stage_color_op[s], key.stage_color_arg1[s], key.stage_color_arg2[s], key.stage_color_arg0[s],
             r_stage, 0b0111);
      /* A bump stage writes neither CURRENT nor TEMP, and its alpha operation
       * never runs: the stage after it reads what the stage before left. */
      if (key.stage_color_op[s] == D3DTOP_BUMPENVMAP || key.stage_color_op[s] == D3DTOP_BUMPENVMAPLUMINANCE)
        continue;
      if (key.stage_alpha_op[s] != D3DTOP_DISABLE)
        emitOp(key.stage_alpha_op[s], key.stage_alpha_arg1[s], key.stage_alpha_arg2[s], key.stage_alpha_arg0[s],
               r_stage, 0b1000);
      else
        b.Mov(WithMask(Temp(r_stage), 0b1000), TempSwizzle(r_cur, 3, 3, 3, 3));
      b.Mov(Temp(r_out), TempSwizzle(r_stage, 0, 1, 2, 3));
    }

    if (key.specular_enable)
      b.Add(WithMask(Temp(r_cur), 0b0111), TempSwizzle(r_cur, 0, 1, 2, 3), Input(i_color1));

    if (key.fog_enable) {
      constexpr float kLog2E = 1.442695041f;
      Operand factor;
      uint32_t r_f = 0;
      if (key.fog_table_mode == D3DFOG_NONE) {
        r_f = b.AllocTemp();
        b.Mov(Saturate(WithMask(Temp(r_f), 0b0001)), Input(i_fog, 0, 0, 0, 0));
        factor = TempSwizzle(r_f, 0, 0, 0, 0);
      } else {
        /* Table fog runs per pixel on the clip-space w under a perspective
         * projection and on the depth otherwise -- SV_Position.w and .z at
         * input register 0 -- the units FOGSTART and FOGEND are given in. */
        r_f = b.AllocTemp();
        b.Mov(WithMask(Temp(r_f), 0b0001), key.fog_w ? Input(0, 3, 3, 3, 3) : Input(0, 2, 2, 2, 2));
        switch (key.fog_table_mode) {
        case D3DFOG_LINEAR: {
          uint32_t r_den = b.AllocTemp();
          b.Add(WithMask(Temp(r_f), 0b0010), CB(kPSFogParams, 1, 1, 1, 1), Neg(TempSwizzle(r_f, 0, 0, 0, 0)));
          b.Add(WithMask(Temp(r_den), 0b0001), CB(kPSFogParams, 1, 1, 1, 1), Neg(CB(kPSFogParams, 0, 0, 0, 0)));
          b.Rcp(WithMask(Temp(r_den), 0b0001), TempSwizzle(r_den, 0, 0, 0, 0));
          b.Mul(Saturate(WithMask(Temp(r_f), 0b0010)), TempSwizzle(r_f, 1, 1, 1, 1), TempSwizzle(r_den, 0, 0, 0, 0));
          break;
        }
        case D3DFOG_EXP:
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 0, 0, 0, 0), CB(kPSFogParams, 2, 2, 2, 2));
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 1, 1, 1, 1), Imm4(-kLog2E, -kLog2E, -kLog2E, -kLog2E));
          b.ExpI(Saturate(WithMask(Temp(r_f), 0b0010)), TempSwizzle(r_f, 1, 1, 1, 1));
          break;
        default: // D3DFOG_EXP2
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 0, 0, 0, 0), CB(kPSFogParams, 2, 2, 2, 2));
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 1, 1, 1, 1), TempSwizzle(r_f, 1, 1, 1, 1));
          b.Mul(WithMask(Temp(r_f), 0b0010), TempSwizzle(r_f, 1, 1, 1, 1), Imm4(-kLog2E, -kLog2E, -kLog2E, -kLog2E));
          b.ExpI(Saturate(WithMask(Temp(r_f), 0b0010)), TempSwizzle(r_f, 1, 1, 1, 1));
          break;
        }
        factor = TempSwizzle(r_f, 1, 1, 1, 1);
      }
      // lerp(fog color, current, fog factor)
      uint32_t r = b.AllocTemp();
      b.Add(WithMask(Temp(r), 0b0111), TempSwizzle(r_cur, 0, 1, 2, 3), Neg(CB(kPSFogColor)));
      b.Mad(WithMask(Temp(r_cur), 0b0111), TempSwizzle(r, 0, 1, 2, 3), factor, CB(kPSFogColor));
    }

    if (key.alpha_test_func) {
      Operand alpha = TempSwizzle(r_cur, 3, 3, 3, 3);
      Operand ref = CB(kPSAlphaRef, 0, 0, 0, 0);
      if (key.alpha_test_func == D3DCMP_NEVER) {
        b.Discard(true, Imm1(1.0f));
      } else {
        uint32_t t = b.AllocTemp();
        Operand tw = WithMask(Temp(t), 0b0001);
        Operand tr = TempSwizzle(t, 0, 0, 0, 0);
        switch (key.alpha_test_func) {
        case D3DCMP_LESS:         b.Ge(tw, alpha, ref); b.Discard(true, tr); break;
        case D3DCMP_EQUAL:        b.Ne(tw, alpha, ref); b.Discard(true, tr); break;
        case D3DCMP_LESSEQUAL:    b.Lt(tw, ref, alpha); b.Discard(true, tr); break;
        case D3DCMP_GREATER:      b.Ge(tw, ref, alpha); b.Discard(true, tr); break;
        case D3DCMP_NOTEQUAL:     b.Eq(tw, alpha, ref); b.Discard(true, tr); break;
        case D3DCMP_GREATEREQUAL: b.Lt(tw, alpha, ref); b.Discard(true, tr); break;
        default: break;
        }
      }
    }

    b.Mov(Output(0), TempSwizzle(r_cur, 0, 1, 2, 3));
    b.Ret();
    if (!b.Finish(ps_dxbc))
      return false;
  }

  return true;
}

} // namespace ffp

} // namespace dxmt
