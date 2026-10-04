// A stand-in for the cockpit panel VS: SV_Position from cb0 rows 4..7, one dp4 each.
cbuffer per_draw : register(b0)
  {
  float4 rows[12];
  };
struct vs_out
  {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  };
vs_out main(uint2 inst : INSTANCE, float3 p : POSITION, float2 uv : TEXCOORD0)
  {
  vs_out o;
  float4 q = float4(p, 1);
  o.position = float4(dot(rows[4], q), dot(rows[5], q), dot(rows[6], q), dot(rows[7], q));
  o.uv = uv + inst * 0.0;
  return o;
  }
