// edworld (PoC, volumetric clouds at half size): the clouds marched into a target of half the picture's width and
// height, laid over the picture. Each pixel of the picture takes the four nearest pixels of the half target, weighed
// by distance as in a plain enlargement and by how near the scene's depth behind each of them is to its own, so the
// clouds do not bleed over the edge of the ground or the ship in front of them. t0 is the copy of the scene's depth
// (reversed: the larger, the nearer), t1 the clouds at half size, already weighed by how much of them there is.
Texture2D<float> scene_depth : register(t0);
Texture2D<float4> half_clouds : register(t1);

struct ps_in
  {
  float4 pos : SV_POSITION;
  };

// one triangle over the whole viewport
ps_in vs_main(uint id : SV_VertexID)
  {
  ps_in output;
  float2 uv = float2((id << 1) & 2, id & 2);
  output.pos = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
  return output;
  }

float4 ps_main(ps_in input) : SV_TARGET
  {
  uint full_w, full_h, half_w, half_h;
  scene_depth.GetDimensions(full_w, full_h);
  half_clouds.GetDimensions(half_w, half_h);
  int2 full_max = int2(full_w, full_h) - 1;
  int2 half_max = int2(half_w, half_h) - 1;

  float depth = scene_depth.Load(int3(input.pos.xy, 0));
  float2 at = input.pos.xy * 0.5f - 0.5f;
  float2 base = floor(at);
  float2 f = at - base;
  float4 sum = 0.f;
  float weights = 0.f;
  [unroll]
  for(int k = 0; k != 4; ++k)
    {
    int2 o = int2(k & 1, k >> 1);
    int2 texel = clamp(int2(base) + o, 0, half_max);
    // the depth the march of that pixel stopped at: the same picture pixel it read
    float its_depth = scene_depth.Load(int3(clamp(texel * 2 + 1, 0, full_max), 0));
    float near = (o.x ? f.x : 1.f - f.x) * (o.y ? f.y : 1.f - f.y);
    // reversed depth is a reciprocal of the distance: the relative difference is the same near and far
    float apart = abs(its_depth - depth) / max(max(its_depth, depth), 1e-7f);
    float w = (near + 1e-3f) / (1e-3f + apart);
    sum += half_clouds.Load(int3(texel, 0)) * w;
    weights += w;
    }
  return sum / weights;
  }
