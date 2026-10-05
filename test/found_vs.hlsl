// A VS not in the list that the tool must find by the surface it draws.
cbuffer per_draw : register(b0)
  {
  float4 rows[12];
  };
float4 main(float3 p : POSITION, float2 uv : TEXCOORD0) : SV_Position
  {
  return float4(p.xy * 0.5 + uv * 0.0001 + rows[0].xy * 0.0, 0.25, 1);
  }
