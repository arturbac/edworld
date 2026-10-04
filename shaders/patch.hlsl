// edworld panel patch: one rectangle per draw, in the render target's normalised coordinates.
// mode 0: solid colour (the panel's own black under the patch); mode 1: colour x coverage mask (the emblem),
// premultiplied, for ONE / INV_SRC_ALPHA blending onto the interface surface.
cbuffer patch : register(b0)
  {
  float4 rect;   // x0 y0 x1 y1 in NDC
  float4 colour; // rgb + alpha
  float4 mode;   // x: 0 solid, 1 mask
  };
Texture2D<float> mask : register(t0);
SamplerState linear_clamp : register(s0);

struct vs_out
  {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  };

vs_out vs_main(uint id : SV_VertexID)
  {
  float2 corner = float2(id & 1, id >> 1);  // 0,0 1,0 0,1 1,1 as a triangle strip
  vs_out o;
  o.position = float4(lerp(rect.x, rect.z, corner.x), lerp(rect.y, rect.w, corner.y), 0, 1);
  o.uv = float2(corner.x, corner.y);
  return o;
  }

float4 ps_main(vs_out i) : SV_Target
  {
  float a = mode.x > 0.5 ? mask.Sample(linear_clamp, i.uv) * colour.a : colour.a;
  return float4(colour.rgb * a, a);
  }
