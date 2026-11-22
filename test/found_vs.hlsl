// A VS not in the list that the tool must find by the surface it draws.
cbuffer per_draw : register(b0)
  {
  float4 rows[12];
  };
// the outputs panel_ps.hlsl reads (it is drawn with this VS)
struct vs_out
  {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  };
vs_out main(float3 p : POSITION, float2 uv : TEXCOORD0)
  {
  vs_out o;
  o.position = float4(p.xy * 0.5 + uv * 0.0001 + rows[0].xy * 0.0, 0.25, 1);
  o.uv = uv;
  return o;
  }
