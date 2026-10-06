// edworld: the list under the jump panel, drawn right after the game's draw of the panel, as a quad of its own in
// the panel's plane. The game's resources of that draw are still bound and are read as its vertex shader reads them
// (EDVR's transcription, src/d3d11/fss_panel_vs.h): the instance record at t33 (scale, unorm16x4 orientation,
// position), the world-rebase origin cb1[275], the clip rows cb0[4..7]; the instance index comes from the game's
// own instance stream (VB0, its start instance). The quad's corners are local points (list_cb.corner), the list's
// box carried by the panel's map under its edge. The pixel shader samples the list's texture (premultiplied) and
// scales it by the surface's alpha at the gate, so the list goes and comes with the panel.
cbuffer game_cb0 : register(b0)
  {
  float4 cb0[8];
  };

cbuffer game_cb1 : register(b1)
  {
  float4 cb1[276];
  };

struct record_t
  {
  uint bone_base;
  float scale;
  uint quat_xy;
  uint quat_zw;
  float3 pos;
  uint pad[77];
  };

StructuredBuffer<record_t> records : register(t33);

cbuffer list_cb : register(b4)
  {
  float4 corner[4];  // top left, top right, bottom right, bottom left; local x, y, z
  float4 uv_extent;  // the used part of the list's texture: u, v
  int4 gate;         // a pixel of the panel's surface
  float4 gain;       // colour scale (the target may be HDR)
  float4 colour;     // x: the texture's gamma, its colours taken back to linear (0: as they are - the list)
  };

struct vs_out
  {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  };

// the shader's unorm16 decode: 1/32767, which EDVR's listing shows rounded as 0.000031
float4 decode_quat(uint xy, uint zw)
  {
  float4 q;
  q.x = (float)(xy & 0xFFFFu) / 32767.0 - 1.0;
  q.y = (float)(xy >> 16u) / 32767.0 - 1.0;
  q.z = (float)(zw & 0xFFFFu) / 32767.0 - 1.0;
  q.w = (float)(zw >> 16u) / 32767.0 - 1.0;
  return q;
  }

float3 quat_rotate(float4 q, float3 p)
  {
  float3 c = float3(q.y * p.z - p.y * q.z, q.z * p.x - p.z * q.x, q.x * p.y - p.x * q.y);
  float d = dot(q.xyz, p);
  float t = q.w * (q.w + q.w);
  float3 r = t * p - p;
  r = d * (q.xyz + q.xyz) + r;
  r = c * (q.w + q.w) + r;
  return r;
  }

vs_out vs_main(uint2 inst : INSTANCEINDEX, uint vid : SV_VertexID)
  {
  static const uint order[6] = {0, 1, 2, 2, 3, 0};
  static const float2 uvs[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  uint k = order[vid];
  record_t r = records[inst.x];
  float4 q = decode_quat(r.quat_xy, r.quat_zw);
  float3 world = quat_rotate(q, corner[k].xyz) * r.scale + (r.pos - cb1[275].xyz);
  float4 w4 = float4(world, 1.0);
  vs_out o;
  o.pos = float4(dot(cb0[4], w4), dot(cb0[5], w4), dot(cb0[6], w4), dot(cb0[7], w4));
  o.uv = uvs[k] * uv_extent.xy;
  return o;
  }

Texture2D list_texture : register(t0);
Texture2D surface : register(t1);
SamplerState linear_clamp : register(s0);

float4 ps_main(vs_out i) : SV_Target
  {
  float4 c = list_texture.Sample(linear_clamp, i.uv);
  // the cockpit is composed in linear HDR: colours drawn as on a picture (sRGB) come out lighter and paler there
  if(colour.x > 0.0)
    {
    float a = max(c.a, 1e-4);
    c.rgb = pow(saturate(c.rgb / a), colour.x) * a;
    }
  float shown = surface.Load(int3(gate.xy, 0)).a;
  return float4(c.rgb * gain.x, c.a) * shown;
  }
