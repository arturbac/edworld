Texture2D surface : register(t2);
SamplerState s : register(s1);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
  {
  return surface.Sample(s, uv);
  }
