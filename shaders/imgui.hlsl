// edworld: the shaders of Dear ImGui's D3D11 backend (third_party/imgui/backends/imgui_impl_dx11.cpp, MIT), compiled
// at build time by fxc instead of at run time by D3DCompile, so the game needs no d3dcompiler_XX.dll for them.
// One addition: t1 holds a copy of the surface as the game drew it this frame, and the output's alpha is scaled
// by the surface's alpha - under the pixel inside the patch's box (b0 box), so the patch shows only where the game
// drew its panel; at one point of the panel (b0 gate) everywhere else, so what is drawn beside the panel, where the
// game draws nothing, goes and comes with the panel.
cbuffer vertexBuffer : register(b0)
  {
  float4x4 ProjectionMatrix;
  };

struct VS_INPUT
  {
  float2 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  };

struct PS_INPUT
  {
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  };

PS_INPUT vs_main(VS_INPUT input)
  {
  PS_INPUT output;
  output.pos = mul(ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));
  output.col = input.col;
  output.uv = input.uv;
  return output;
  }

cbuffer patch_gate : register(b0)
  {
  int4 box;   // x0, y0, x1, y1 (exclusive) on the surface
  int4 gate;  // x, y
  };

sampler sampler0;
Texture2D texture0 : register(t0);
Texture2D game_surface : register(t1);

float4 ps_main(PS_INPUT input) : SV_Target
  {
  float4 out_col = input.col * texture0.Sample(sampler0, input.uv);
  int2 p = int2(input.pos.xy);
  bool inside = p.x >= box.x && p.y >= box.y && p.x < box.z && p.y < box.w;
  out_col.a *= game_surface.Load(int3(inside ? p : gate.xy, 0)).a;
  return out_col;
  }
