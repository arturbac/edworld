// Any other VS: must not be recorded.
cbuffer per_draw : register(b0)
  {
  float4 rows[12];
  };
float4 main(float3 p : POSITION, float2 uv : TEXCOORD0) : SV_Position
  {
  return float4(p.xy * 0.5 + uv * 0.0001 + rows[0].xy * 0.0, 0.5, 1);
  }
